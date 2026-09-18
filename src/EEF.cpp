#include "PCH.h"

#include <SimpleIni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "REL/Relocation.h"
#include "SKSE/Trampoline.h"

#include "EEF.h"

namespace EEF
{
	namespace
	{
		bool s_onEquip{ true };
		bool s_onActorLoad{ true };
		bool s_recalcWeightOnLoad{ false };
		bool s_redirectDispel{ true };

		constexpr auto kIniPath = "Data\\SKSE\\Plugins\\EquipEnchantmentFix.ini";

		std::mutex s_queueLock;

		// Actors already queued for a re-check, so a burst of events for the same
		// actor (load, equip, effect removal) collapses into a single task.
		std::unordered_set<
			RE::ObjectRefHandle,
			RE::BSCRC32<RE::ObjectRefHandle>>
			s_pending;

		// True while a drain task is already queued, so a burst of events only
		// ever queues one.
		bool s_drainQueued{ false };

		// Form-table lookups are not safe from every context, so every lookup
		// we cannot remove goes through here: a bad table turns into a missing
		// form instead of a crash. The per-event cost is microseconds (x64 SEH
		// is table-driven and free when nothing raises), which is what makes
		// handling thousands of load-time events affordable.
		[[nodiscard]] RE::TESForm* LookupFormSafe(RE::FormID a_id)
		{
			if (!a_id) {
				return nullptr;
			}

			__try {
				return RE::TESForm::LookupByID(a_id);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				SKSE::log::error("TESForm::LookupByID({:08X}) raised; treating as missing", a_id);
				return nullptr;
			}
		}

		template <class T>
		[[nodiscard]] T* LookupFormSafe(RE::FormID a_id)
		{
			auto* form = LookupFormSafe(a_id);
			return form ? form->As<T>() : nullptr;
		}

		[[nodiscard]] bool IsValidActor(RE::Actor* a_actor)
		{
			// IsDeleted() / IsInitialized() read formFlags straight off the
			// object (non-virtual). IsDead() below is virtual, but by the time
			// it runs the pointer has already been validated by the caller
			// (handle or live-form-table lookup), so this is safe.
			return a_actor && !a_actor->IsDeleted() && a_actor->IsInitialized() && !a_actor->IsDead();
		}

		// The instance enchantment actually carried by a worn item's extra data.
		// Only ExtraEnchantment counts (no base-form fallback), matching the
		// original plugin -- otherwise the ability check can never match and we
		// re-apply on every equip.
		[[nodiscard]] RE::EnchantmentItem* GetWornEnchantment(RE::ExtraDataList* a_worn)
		{
			if (!a_worn) {
				return nullptr;
			}
			if (auto* extraEnch = a_worn->GetByType<RE::ExtraEnchantment>()) {
				return extraEnch->enchantment;
			}
			return nullptr;
		}

		// A worn armour can only carry a constant-effect enchantment
		// (CastingType::kConstantEffect). Anything else sitting on it is a
		// foreign configuration -- most commonly a weapon enchantment moved
		// onto armour by a "no enchantment restriction" mod. Feeding that to
		// UpdateArmorAbility drives the engine down a branch it never expects
		// and crashes (null function-pointer call). Skip it rather than
		// trying to "restore" it.
		[[nodiscard]] bool IsValidArmorEnchantment(RE::EnchantmentItem* a_enchantment)
		{
			if (!a_enchantment) {
				return false;
			}
			return a_enchantment->data.castingType == RE::MagicSystem::CastingType::kConstantEffect;
		}

		// Result of "is this item's enchantment already on the actor". The third
		// state matters: the walk can fault, and a fault is NOT "absent".
		// Collapsing it to absent makes every check re-apply the effect, which is
		// exactly how effects ended up stacking -- so a fault is reported
		// separately and the caller skips the item instead of duplicating it.
		enum class AbilityCheck : std::uint8_t
		{
			kMissing,  // no matching (source, spell) -> safe to apply
			kPresent,  // the actor already carries it
			kUnknown   // the walk faulted -> we cannot tell
		};

		std::atomic<std::uint32_t> s_abilityQueryFaults{ 0 };

		// "Is this enchantment already applied" the way the original plugin
		// answered it: walk the actor's active effects and match the (source,
		// spell) pair, where source is the item and spell is its enchantment.
		//
		// Actor::HasMagicEffect looked like the portable equivalent, but it faults
		// on every runtime tested (SE 1.5.97 included), which silently disabled
		// the check. MagicTarget::GetActiveEffectList is a RelocateVirtual
		// vtable-slot forwarder, so it must be reached through a correctly-offset
		// MagicTarget* -- that is what AsMagicTarget() yields; a bare Actor* would
		// read the wrong vtable.
		//
		// The walk lives outside the __try because MSVC refuses __try in a
		// function that needs object unwinding, and the range-for iterator is
		// such an object. A fault raised inside the call still unwinds into the
		// caller's handler.
		struct AbilityQuery
		{
			const void* source;
			const void* spell;
			bool        found;
		};

		[[nodiscard]] bool WalkForAbility(RE::MagicTarget* a_target, AbilityQuery* a_query)
		{
			auto* list = a_target->GetActiveEffectList();
			if (!list) {
				return false;
			}

			for (auto& effect : *list) {
				if (effect && effect->source == a_query->source && effect->spell == a_query->spell) {
					a_query->found = true;
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] AbilityCheck TryHasItemAbility(RE::Actor* a_actor, RE::TESForm* a_form, RE::EnchantmentItem* a_enchantment)
		{
			if (!a_actor || !a_form || !a_enchantment) {
				return AbilityCheck::kMissing;
			}

			auto* target = a_actor->AsMagicTarget();
			if (!target) {
				return AbilityCheck::kUnknown;
			}

			AbilityQuery query{ a_form, a_enchantment, false };

			__try {
				(void)WalkForAbility(target, &query);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				// Log the first fault only: a broken walk faults on every call
				// and would otherwise flood the log.
				if (s_abilityQueryFaults.fetch_add(1, std::memory_order_relaxed) == 0) {
					SKSE::log::error(
						"active-effect walk faulted (actor {:08X}, item {:08X}); ability checks are "
						"treated as unknown from now on, so nothing will be re-applied (a missed "
						"re-apply is harmless, a duplicate is not).",
						a_actor->GetFormID(),
						a_form->GetFormID());
				}
				return AbilityCheck::kUnknown;
			}

			return query.found ? AbilityCheck::kPresent : AbilityCheck::kMissing;
		}

		[[nodiscard]] bool TryUpdateArmorAbility(RE::Actor* a_actor, RE::TESForm* a_form, RE::ExtraDataList* a_extraData)
		{
			__try {
				a_actor->UpdateArmorAbility(a_form, a_extraData);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] bool HasItemAbility(RE::Actor* a_actor, RE::TESForm* a_form, RE::EnchantmentItem* a_enchantment)
		{
			switch (TryHasItemAbility(a_actor, a_form, a_enchantment)) {
			case AbilityCheck::kPresent:
				return true;
			case AbilityCheck::kUnknown:
				// The walk is not trustworthy on this runtime. Report "already
				// present" so ProcessActor does not re-apply: skipping is the
				// safe failure mode, duplicating is not.
				return true;
			case AbilityCheck::kMissing:
			default:
				return false;
			}
		}

		// --- engine hook -------------------------------------------------------
		// The engine applies a worn item's enchantment through
		// Actor::UpdateArmorAbility and does not de-duplicate: calling it a second
		// time for the same item stacks the effect. The original plugin hooked the
		// engine's call to it to block that before it happened; this hooks the same
		// call site, using the exact (source, spell) check ProcessActor uses, so the
		// block and the repair agree on what "already applied" means.
		//
		// Note the hook sits on the ENGINE's call site, not on the function: our own
		// re-apply calls the function directly and therefore still goes through,
		// which is what lets a genuinely missing enchantment be restored.
		using UpdateArmorAbility_t = void (*)(RE::Actor*, RE::TESForm*, RE::ExtraDataList*);
		UpdateArmorAbility_t UpdateArmorAbility_orig{ nullptr };

		// The enchantment the engine applies for a worn instance: the form's own
		// first, the instance's (ExtraEnchantment) only when the form has none.
		// That order is the engine's, read from the routine both UpdateArmorAbility
		// and the dispel visitor use to pick it (1.6.1170). An item carrying both
		// gets the record's; matching the instance's instead would never find the
		// applied effect, and the hook below would let the engine stack a copy.
		[[nodiscard]] RE::EnchantmentItem* GetApplicableEnchantment(RE::TESForm* a_form, RE::ExtraDataList* a_extraData)
		{
			if (a_form) {
				if (auto* enchantable = a_form->As<RE::TESEnchantableForm>(); enchantable && enchantable->formEnchanting) {
					return enchantable->formEnchanting;
				}
			}
			return GetWornEnchantment(a_extraData);
		}

		void UpdateArmorAbility_Hook(RE::Actor* a_actor, RE::TESForm* a_form, RE::ExtraDataList* a_extraData)
		{
			if (a_actor && a_form && a_form->As<RE::TESObjectARMO>()) {
				auto* enchantment = GetApplicableEnchantment(a_form, a_extraData);
				if (enchantment && IsValidArmorEnchantment(enchantment) &&
					HasItemAbility(a_actor, a_form, enchantment)) {
					// Already on the actor. Letting the engine run would stack it.
					return;
				}
			}

			UpdateArmorAbility_orig(a_actor, a_form, a_extraData);
		}

		// The engine reaches UpdateArmorAbility through a five-byte `call rel32`
		// at a known offset inside its caller. That CALL SITE is what gets hooked,
		// not the function itself: SKSE::Trampoline::write_call replaces exactly
		// that instruction and returns the call's original target, so the real
		// function stays untouched and callable. Patching the function entry
		// instead corrupts it -- the trampoline does not preserve the prologue, and
		// the "original" it hands back is computed from the prologue bytes.
		//
		// A call site is a function start (from the Address Library, stable across
		// versions) plus an offset into its body (ours, and stable across nothing:
		// it moves whenever the compiler lays the function out differently). So
		// before patching, the instruction there must be a call, and the call must
		// already lead to the function we expect to replace. Either check failing
		// means the offset describes a different game binary, and refusing is the
		// difference between a lost fix and a corrupted engine.
		template <class Fn>
		[[nodiscard]] bool InstallCallHook(const char* a_what, std::uintptr_t a_callSite, std::uintptr_t a_expectedTarget, Fn a_hook, Fn& a_orig)
		{
			if (!a_callSite) {
				SKSE::log::error("{} call site could not be resolved", a_what);
				return false;
			}

			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_callSite);
			if (bytes[0] != 0xE8) {
				SKSE::log::error("{} call site has opcode {:02X}, not a call; not hooking", a_what, bytes[0]);
				return false;
			}

			std::int32_t rel = 0;
			std::memcpy(&rel, bytes + 1, sizeof(rel));
			const auto target = a_callSite + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
			if (a_expectedTarget && target != a_expectedTarget) {
				SKSE::log::error("{} call site calls {:X}, expected {:X}; not hooking", a_what, target, a_expectedTarget);
				return false;
			}

			try {
				a_orig = reinterpret_cast<Fn>(SKSE::GetTrampoline().write_call<5>(a_callSite, a_hook));
			} catch (const std::exception& e) {
				SKSE::log::error("{} hook failed: {}", a_what, e.what());
				return false;
			} catch (...) {
				SKSE::log::error("{} hook failed (unknown exception)", a_what);
				return false;
			}

			if (!a_orig) {
				SKSE::log::error("{} hook produced a null original pointer", a_what);
				return false;
			}

			SKSE::log::info("{} hook installed at {:X}", a_what, a_callSite);
			return true;
		}

		[[nodiscard]] bool InstallUpdateArmorAbilityHook()
		{
			static REL::Relocation<std::uintptr_t> caller{ REL::RelocationID(36976, 38001) };
			static REL::Relocation<std::uintptr_t> callee{ REL::RelocationID(37802, 38751) };
			const auto site = caller.address() + (REL::Module::IsAE() ? 0x36D : 0x3BB);
			return InstallCallHook("UpdateArmorAbility", site, callee.address(), &UpdateArmorAbility_Hook, UpdateArmorAbility_orig);
		}

		// --- the dispel redirect ----------------------------------------------
		// The container menu's transfer routine ends, for an NPC on either side of
		// the trade, with Actor::DispelWornItemEnchantments followed by a request
		// for a 3D model update. The dispel walks the actor's worn armour and
		// flags every effect those items are the source of; the model update is
		// what re-applies them, and it only rebuilds when an equipment change
		// flagged it. Giving a potion flags nothing, so the dispel is never
		// undone. Those effects also never raise the apply/remove event: the
		// engine sends it only for an effect with a unique id, and assigns the id
		// only when the magic effect form's runtime flag bit 22 is set. No MGEF
		// carries that bit on disk; the engine sets it at load, by all
		// appearances for effects with a script attached, which are the ones
		// the VM has to address by id. So the post-hoc re-check below cannot
		// see a plain enchantment effect go.
		//
		// The original plugin's RedirectDispelWornItemEnchantsVisitor (1.3.5)
		// replaced the call at this site, and 1.3.6 removed it. This is that
		// redirect: leave every effect whose source is still worn, dispel the
		// rest, and queue a re-check so anything already missing is put back.
		// The UpdateArmorAbility hook above is the other half -- the armour
		// trade still re-equips, and without the block that would stack a copy.
		//
		// Site (transfer routine + offset of the call): SE 50212+0x47B from
		// 1.3.5; AE 51141+0x57D, 1.3.5's value, re-verified on 1.6.1170 by
		// listing the callers of Actor::DispelWornItemEnchantments. InstallCallHook
		// checks that the call really leads there before patching.
		using DispelWornItemEnchantments_t = void (*)(RE::Actor*);
		DispelWornItemEnchantments_t DispelWornItemEnchantments_orig{ nullptr };

		void ScheduleActorCheck(RE::TESObjectREFR* a_ref);

		// The effects to dispel: sourced by an armour the actor no longer wears.
		// Weapons are left alone as the engine's own visitor leaves them; the
		// unequip path handles those. Split from its caller so the walk can sit
		// under __try: MSVC refuses __try in a function that owns an object with a
		// destructor, and the vectors live in the caller.
		// One worn instance: the item and the enchantment the engine applied for
		// it. An active effect records its source as the base object only, so
		// this pair is the finest identity there is; matching on the item alone
		// would keep an effect as long as any copy of that item is worn, which is
		// wrong when the worn copy is not the one the effect came from.
		struct WornEnchantment
		{
			RE::TESBoundObject* source;
			RE::MagicItem*      spell;
		};

		struct StaleQuery
		{
			const std::vector<WornEnchantment>* worn;
			std::vector<RE::ActiveEffect*>*     stale;
		};

		void CollectStaleEffects(RE::MagicTarget* a_target, StaleQuery* a_query)
		{
			auto* list = a_target->GetActiveEffectList();
			if (!list) {
				return;
			}
			for (auto* effect : *list) {
				if (!effect || !effect->source || !effect->spell) {
					continue;
				}
				if (effect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
					continue;
				}
				if (!effect->source->As<RE::TESObjectARMO>()) {
					continue;
				}
				const auto stillWorn = std::any_of(a_query->worn->begin(), a_query->worn->end(), [&](const WornEnchantment& w) {
					return w.source == effect->source && w.spell == effect->spell;
				});
				if (!stillWorn) {
					a_query->stale->push_back(effect);
				}
			}
		}

		[[nodiscard]] bool TryCollectStaleEffects(RE::MagicTarget* a_target, StaleQuery* a_query)
		{
			__try {
				CollectStaleEffects(a_target, a_query);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void DispelWornItemEnchantments_Hook(RE::Actor* a_actor)
		{
			if (!IsValidActor(a_actor)) {
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			auto* changes = a_actor->GetInventoryChanges(true);
			if (!changes || !changes->entryList) {
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			// Every worn armour instance with the enchantment the engine applied
			// for it. One base item can be in the bag several times, each instance
			// with its own extra list and its own enchantment (a gold ring of
			// fortify health worn, one of fortify stamina carried); the worn one is
			// found by its ExtraWorn, and only its enchantment is a pair.
			std::vector<WornEnchantment> worn;
			for (auto* entry : *changes->entryList) {
				if (!entry || !entry->object || !entry->object->As<RE::TESObjectARMO>() || !entry->extraLists) {
					continue;
				}
				for (auto* xList : *entry->extraLists) {
					if (!xList || !(xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>())) {
						continue;
					}
					if (auto* enchantment = GetApplicableEnchantment(entry->object, xList)) {
						worn.push_back({ entry->object, enchantment });
					}
				}
			}

			auto* target = a_actor->AsMagicTarget();
			if (!target) {
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			std::vector<RE::ActiveEffect*> stale;
			StaleQuery                     query{ &worn, &stale };
			if (!TryCollectStaleEffects(target, &query)) {
				// The walk is not trustworthy here; the engine's own dispel is the
				// known-safe behaviour, even though it is the one that loses effects.
				SKSE::log::error("active-effect walk faulted in the dispel redirect (actor {:08X}); engine dispel used", a_actor->GetFormID());
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			for (auto* effect : stale) {
				effect->Dispel(false);
			}

			SKSE::log::debug("dispel redirect: actor {:08X}, {} worn armour kept, {} stale effect(s) dispelled",
				a_actor->GetFormID(), worn.size(), stale.size());

			ScheduleActorCheck(a_actor);
		}

		[[nodiscard]] bool InstallDispelRedirect()
		{
			static REL::Relocation<std::uintptr_t> caller{ REL::RelocationID(50212, 51141) };
			static REL::Relocation<std::uintptr_t> callee{ REL::RelocationID(33828, 34620) };
			const auto site = caller.address() + (REL::Module::IsAE() ? 0x57D : 0x47B);
			return InstallCallHook("DispelWornItemEnchantments (container transfer)", site, callee.address(), &DispelWornItemEnchantments_Hook, DispelWornItemEnchantments_orig);
		}

		// a_onlyForm: when the caller knows which item was just equipped
		// (TESEquipEvent::baseObject) only that item is considered. Walking the
		// whole inventory on every equip is what made switching weapons and
		// armour stutter; the original plugin only ever looks at the equipped
		// form. 0 means "no filter" and is what the load-time path passes,
		// since that one has to restore every worn item.
		void ProcessActor(RE::Actor* a_actor, RE::FormID a_onlyForm = 0)
		{
			if (!IsValidActor(a_actor)) {
				return;
			}

			// a_noInit = true: never initialize the inventory from here.
			auto* changes = a_actor->GetInventoryChanges(true);
			if (!changes || !changes->entryList) {
				return;
			}

			// Collect first. UpdateArmorAbility can mutate the inventory, so we must
			// NOT be iterating the entry list while calling it.
			struct Candidate
			{
				RE::TESForm*         form;
				RE::ExtraDataList*   worn;
				RE::EnchantmentItem* enchantment;
			};
			std::vector<Candidate> candidates;

			SKSE::log::debug("re-check actor {:08X} filter {:08X}", a_actor->GetFormID(), a_onlyForm);

			for (auto* entry : *changes->entryList) {
				if (!entry || !entry->object) {
					continue;
				}
				if (!entry->object->As<RE::TESObjectARMO>()) {
					continue;
				}
				if (a_onlyForm && entry->object->GetFormID() != a_onlyForm) {
					continue;
				}
				if (!entry->IsWorn()) {
					continue;
				}

				RE::ExtraDataList* worn = nullptr;
				if (entry->extraLists) {
					for (auto* xList : *entry->extraLists) {
						if (xList && (xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>())) {
							worn = xList;
							break;
						}
					}
				}

				auto* enchantment = GetWornEnchantment(worn);
				SKSE::log::debug("  worn armour {:08X} wornList={} instanceEnch={:08X} casting={}",
					entry->object->GetFormID(), worn != nullptr,
					enchantment ? enchantment->GetFormID() : 0,
					enchantment ? static_cast<int>(enchantment->data.castingType) : -1);
				if (!enchantment) {
					continue;
				}

				if (!IsValidArmorEnchantment(enchantment)) {
					SKSE::log::debug(
						"skipping non-constant enchantment {:08X} on worn armour {:08X} (actor {:08X})",
						enchantment->GetFormID(),
						entry->object->GetFormID(),
						a_actor->GetFormID());
					continue;
				}

				candidates.push_back({ entry->object, worn, enchantment });
			}

			for (auto& c : candidates) {
				const auto check = TryHasItemAbility(a_actor, c.form, c.enchantment);
				SKSE::log::debug("  ability check {:08X}/{:08X} -> {}", c.form->GetFormID(),
					c.enchantment->GetFormID(),
					check == AbilityCheck::kPresent ? "present" : check == AbilityCheck::kMissing ? "missing" : "unknown");
				if (check != AbilityCheck::kMissing) {
					continue;
				}

				SKSE::log::info(
					"  re-applying enchantment ability for {:08X} on actor {:08X}",
					c.form->GetFormID(),
					a_actor->GetFormID());

				if (!TryUpdateArmorAbility(a_actor, c.form, c.worn)) {
					SKSE::log::error("UpdateArmorAbility faulted; skipped");
				}
			}
		}

		// Queue an actor for a re-check. The handle only goes into a set here --
		// no form-table lookup, no per-actor task -- and exactly ONE drain task
		// is queued for the whole batch. On a save load the engine fires these
		// events for thousands of references, and one queued task each is what
		// froze the main thread for seconds.
		void ScheduleActorCheck(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}

			auto* actor = a_ref->As<RE::Actor>();
			// Gate here, while the pointer is guaranteed live (it comes straight
			// from the event): never queue a check for an actor that is already
			// deleted or dead.
			if (!IsValidActor(actor)) {
				return;
			}

			RE::ObjectRefHandle handle(actor);
			if (!handle) {
				return;
			}

			SKSE::log::debug("queued re-check for {:08X}", actor->GetFormID());

			{
				std::scoped_lock lock(s_queueLock);
				s_pending.insert(handle);
				if (s_drainQueued) {
					return;
				}
				s_drainQueued = true;
			}

			auto* taskInterface = SKSE::GetTaskInterface();
			if (!taskInterface) {
				std::scoped_lock lock(s_queueLock);
				s_drainQueued = false;
				return;
			}

			taskInterface->AddTask([]() {
				const auto started = std::chrono::steady_clock::now();

				std::unordered_set<
					RE::ObjectRefHandle,
					RE::BSCRC32<RE::ObjectRefHandle>>
					batch;

				{
					std::scoped_lock lock(s_queueLock);
					batch.swap(s_pending);
					s_drainQueued = false;
				}

				// The handle resolves to a live reference-counted pointer, so a
				// stale actor simply resolves to nothing here.
				std::size_t processed = 0;
				for (const auto& handle : batch) {
					auto ref = handle.get();
					if (!ref) {
						continue;
					}
					if (auto* loaded = ref->As<RE::Actor>(); IsValidActor(loaded)) {
						ProcessActor(loaded);
						++processed;
					}
				}

				// Whether the load-time freeze is ours is a measurement, not a
				// guess: a long span here (or a huge batch) is us, a short one
				// means the time goes somewhere else. One line per drain is
				// cheap, so this stays on.
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
									std::chrono::steady_clock::now() - started)
									.count();
				SKSE::log::info(
					"drain: {} queued, {} processed, {} ms",
					batch.size(),
					processed,
					ms);
			});
		}

		void LoadSettings()
		{
			CSimpleIniA ini;
			ini.SetUnicode();

			const auto rc = ini.LoadFile(kIniPath);
			if (rc < 0) {
				SKSE::log::info("no ini found at {}, using defaults", kIniPath);
			}

			s_onEquip = ini.GetBoolValue("EEF", "OnEquip", true);
			s_onActorLoad = ini.GetBoolValue("EEF", "OnActorLoad", true);
			s_recalcWeightOnLoad = ini.GetBoolValue("EEF", "RecalcPlayerInventoryWeightOnLoad", false);
			s_redirectDispel = ini.GetBoolValue("EEF", "RedirectDispelWornItemEnchantsVisitor", true);

			// The file logger is at info by default; "debug" shows every step of the
			// re-check and the redirect, which is what settled Nordic Souls #183.
			if (const auto* level = ini.GetValue("EEF", "LogLevel", nullptr)) {
				spdlog::set_level(spdlog::level::from_str(level));
				spdlog::flush_on(spdlog::level::from_str(level));
			}

			SKSE::log::info(
				"settings: OnEquip={} OnActorLoad={} RecalcWeight={} RedirectDispel={}",
				s_onEquip,
				s_onActorLoad,
				s_recalcWeightOnLoad,
				s_redirectDispel);
		}
	}

	void ClearPendingChecks()
	{
		std::scoped_lock lock(s_queueLock);
		s_pending.clear();
		s_drainQueued = false;
	}

	void RecalcPlayerWeight()
	{
		if (!s_recalcWeightOnLoad) {
			return;
		}

		auto* taskInterface = SKSE::GetTaskInterface();
		if (!taskInterface) {
			return;
		}

		taskInterface->AddTask([]() {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			if (auto* changes = player->GetInventoryChanges()) {
				changes->totalWeight = -1.0F;
			}
		});
	}

	RE::BSEventNotifyControl EquipEventHandler::ProcessEvent(
		const RE::TESEquipEvent*               a_event,
		RE::BSTEventSource<RE::TESEquipEvent>*)
	{
		// No form-table lookup here on purpose: this event fires while the
		// engine's form-table lock is in play, and looking the base object up
		// from here crashes. ProcessActor already walks the worn list and only
		// touches armour, so the lookup bought us nothing but a crash.
		if (s_onEquip && a_event && a_event->equipped && a_event->actor) {
			if (auto* actor = a_event->actor->As<RE::Actor>(); IsValidActor(actor)) {
				// baseObject is a FormID read straight off the event -- no
				// form-table lookup, which is what used to crash in this
				// handler. It tells ProcessActor to consider only the item that
				// was just equipped instead of rescanning the whole inventory.
				ProcessActor(actor, a_event->baseObject);
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl LoadEventHandler::ProcessEvent(
		const RE::TESObjectLoadedEvent*               a_event,
		RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
	{
		// Deliberately cheap: resolve-and-queue only, no work here. The
		// single shared drain task (see ScheduleActorCheck) is what bounds
		// the load-time burst -- one task no matter how many references load.
		if (a_event && a_event->loaded) {
			if (auto* ref = LookupFormSafe<RE::TESObjectREFR>(a_event->formID)) {
				ScheduleActorCheck(ref);
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl LoadEventHandler::ProcessEvent(
		const RE::TESInitScriptEvent*               a_event,
		RE::BSTEventSource<RE::TESInitScriptEvent>*)
	{
		if (a_event && a_event->objectInitialized) {
			ScheduleActorCheck(a_event->objectInitialized.get());
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl ActiveEffectEventHandler::ProcessEvent(
		const RE::TESActiveEffectApplyRemoveEvent*               a_event,
		RE::BSTEventSource<RE::TESActiveEffectApplyRemoveEvent>*)
	{
		// isApplied == false means an active effect was removed. If it belonged to a
		// worn item's enchantment, the engine may have dispelled it wrongly; queue a
		// re-check and ProcessActor will re-apply the missing ability.
		if (s_redirectDispel && a_event && !a_event->isApplied) {
			SKSE::log::debug("effect removed: uid {} target {:08X} caster {:08X}",
				a_event->activeEffectUniqueID,
				a_event->target ? a_event->target->GetFormID() : 0,
				a_event->caster ? a_event->caster->GetFormID() : 0);
			if (a_event->target) {
				ScheduleActorCheck(a_event->target.get());
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	EquipEventHandler* EquipEventHandler::GetSingleton()
	{
		static EquipEventHandler singleton;
		return &singleton;
	}

	LoadEventHandler* LoadEventHandler::GetSingleton()
	{
		static LoadEventHandler singleton;
		return &singleton;
	}

	ActiveEffectEventHandler* ActiveEffectEventHandler::GetSingleton()
	{
		static ActiveEffectEventHandler singleton;
		return &singleton;
	}

	bool Initialize()
	{
		LoadSettings();

		const auto holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) {
			SKSE::log::error("ScriptEventSourceHolder unavailable");
			return false;
		}

		if (s_onEquip) {
			holder->AddEventSink<RE::TESEquipEvent>(EquipEventHandler::GetSingleton());
		}
		if (s_onActorLoad) {
			holder->AddEventSink<RE::TESObjectLoadedEvent>(LoadEventHandler::GetSingleton());
			holder->AddEventSink<RE::TESInitScriptEvent>(LoadEventHandler::GetSingleton());
		}
		if (s_redirectDispel) {
			// Two halves. The redirect keeps worn enchantments through an inventory
			// change; the block stops the engine's re-equip from stacking a copy of
			// what was kept. The post-hoc recheck stays as the fallback for
			// whichever cannot be installed on this runtime.
			const bool blocked = InstallUpdateArmorAbilityHook();
			if (!blocked) {
				SKSE::log::warn("UpdateArmorAbility hook unavailable; using the post-hoc recheck only");
			}
			if (blocked && !InstallDispelRedirect()) {
				SKSE::log::warn("dispel redirect unavailable; worn enchantments will still drop on an inventory change");
			}
			holder->AddEventSink<RE::TESActiveEffectApplyRemoveEvent>(ActiveEffectEventHandler::GetSingleton());
		}

		SKSE::log::info(
			"registered sinks (OnEquip={} OnActorLoad={} RedirectDispel={})",
			s_onEquip,
			s_onActorLoad,
			s_redirectDispel);

		return true;
	}
}
