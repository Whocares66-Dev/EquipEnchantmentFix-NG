#include "PCH.h"

#include <SimpleIni.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "EEF.h"

namespace EEF
{
	namespace
	{
		bool s_onEquip{ true };
		bool s_onActorLoad{ true };
		bool s_recalcWeightOnLoad{ false };
		bool s_redirectDispel{ false };

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
				if (HasItemAbility(a_actor, c.form, c.enchantment)) {
					continue;
				}

				SKSE::log::debug(
					"re-applying enchantment ability for {:08X} on actor {:08X}",
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
				if (ms >= 25 || batch.size() >= 32) {
					SKSE::log::info(
						"drain: {} queued, {} processed, {} ms",
						batch.size(),
						processed,
						ms);
				}
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
			s_redirectDispel = ini.GetBoolValue("EEF", "RedirectDispelWornItemEnchantsVisitor", false);

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
