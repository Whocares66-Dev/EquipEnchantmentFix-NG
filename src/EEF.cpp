#include "PCH.h"

#include <SimpleIni.h>

#include <chrono>
#include <mutex>
#include <unordered_map>
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

		// The engine's UpdateArmorAbility does NOT de-duplicate: calling it twice
		// for the same item stacks the effect. Our only guard is HasItemAbility,
		// and that can miss while a just-applied effect is not registered yet
		// (same frame, or an inventory change landing right after an equip). So
		// remember what we applied recently and don't repeat it inside a short
		// window. Long enough to cover the registration lag, short enough that a
		// genuinely lost effect is still restored on the next check.
		constexpr auto kReapplyCooldown = std::chrono::milliseconds(500);

		std::mutex s_reappliedLock;
		std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
			s_reapplied;

		[[nodiscard]] std::uint64_t ApplyKey(RE::Actor* a_actor, RE::TESForm* a_form)
		{
			return (static_cast<std::uint64_t>(a_actor->GetFormID()) << 32) |
			       static_cast<std::uint64_t>(a_form->GetFormID());
		}

		[[nodiscard]] bool WasAppliedRecently(std::uint64_t a_key)
		{
			const auto now = std::chrono::steady_clock::now();

			std::scoped_lock lock(s_reappliedLock);

			// Opportunistic sweep: the table only holds pairs that were touched
			// in the last window, so it stays tiny.
			for (auto it = s_reapplied.begin(); it != s_reapplied.end();) {
				if (now - it->second >= kReapplyCooldown) {
					it = s_reapplied.erase(it);
				} else {
					++it;
				}
			}

			const auto it = s_reapplied.find(a_key);
			return it != s_reapplied.end();
		}

		void NoteApplied(std::uint64_t a_key)
		{
			std::scoped_lock lock(s_reappliedLock);
			s_reapplied[a_key] = std::chrono::steady_clock::now();
		}

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

		// MSVC forbids __try in a function that needs stack unwinding, so each
		// fault-prone engine call gets its own small, object-free helper.
		[[nodiscard]] bool TryHasMagicEffect(RE::Actor* a_actor, RE::EffectSetting* a_effect)
		{
			__try {
				return a_actor->HasMagicEffect(a_effect);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
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

		[[nodiscard]] bool HasItemAbility(RE::Actor* a_actor, [[maybe_unused]] RE::TESForm* a_form, RE::EnchantmentItem* a_enchantment)
		{
			if (!a_actor || !a_enchantment) {
				return false;
			}

			// We cannot walk the actor's active effects here: both
			// MagicTarget::GetActiveEffectList (a RelocateVirtual vtable-slot
			// forwarder) and MagicTarget::VisitEffects fault on this setup.
			// MagicTarget::HasMagicEffect does not -- it is a plain
			// Address-Library call, and it is the same engine entry the Papyrus
			// Actor.HasMagicEffect native function uses.
			//
			// It matches on the magic effect instead of the (source, spell)
			// pair, so it is a coarser test: an effect granted by some other
			// source would read as "already present". For the enchantment
			// abilities we re-apply that is the safe direction -- worst case we
			// skip a re-apply rather than duplicate one.
			for (auto* effect : a_enchantment->effects) {
				if (effect && effect->baseEffect && TryHasMagicEffect(a_actor, effect->baseEffect)) {
					return true;
				}
			}

			return false;
		}

		void ProcessActor(RE::Actor* a_actor)
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
				const auto key = ApplyKey(a_actor, c.form);

				if (HasItemAbility(a_actor, c.form, c.enchantment)) {
					continue;
				}

				if (WasAppliedRecently(key)) {
					// We applied this a moment ago and the engine has not
					// exposed the effect yet. Applying again would stack it.
					continue;
				}

				SKSE::log::debug(
					"re-applying enchantment ability for {:08X} on actor {:08X}",
					c.form->GetFormID(),
					a_actor->GetFormID());

				if (TryUpdateArmorAbility(a_actor, c.form, c.worn)) {
					NoteApplied(key);
				} else {
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
				for (const auto& handle : batch) {
					auto ref = handle.get();
					if (!ref) {
						continue;
					}
					if (auto* loaded = ref->As<RE::Actor>(); IsValidActor(loaded)) {
						ProcessActor(loaded);
					}
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
				ProcessActor(actor);
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
