#include "PCH.h"

#include <SimpleIni.h>

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
		bool s_redirectDispel{ true };

		constexpr auto kIniPath = "Data\\SKSE\\Plugins\\EquipEnchantmentFix.ini";

		std::mutex s_queueLock;

		// Actors already queued for a re-check, so a burst of events for the same
		// actor (load, equip, effect removal) collapses into a single task.
		std::unordered_set<
			RE::ObjectRefHandle,
			RE::BSCRC32<RE::ObjectRefHandle>>
			s_pending;

		[[nodiscard]] bool IsValidActor(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsDeleted() && !a_actor->IsDead();
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

		[[nodiscard]] bool HasItemAbility(RE::Actor* a_actor, RE::TESForm* a_form, RE::EnchantmentItem* a_enchantment)
		{
			auto* effects = a_actor->GetActiveEffectList();
			if (!effects) {
				return false;
			}

			for (auto& effect : *effects) {
				if (!effect) {
					continue;
				}
				if (effect->source == a_form && effect->spell == a_enchantment) {
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

				a_actor->UpdateArmorAbility(c.form, c.worn);
			}
		}

		// Defer the check to the task queue: during the load / equip / effect event
		// the actor state is not settled yet. Mirrors the original plugin's
		// EnchantmentEnforcerTask, including its per-actor de-duplication.
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

			auto* taskInterface = SKSE::GetTaskInterface();
			if (!taskInterface) {
				return;
			}

			RE::ObjectRefHandle handle(actor);
			if (!handle) {
				return;
			}

			const auto formID = actor->GetFormID();

			{
				std::scoped_lock lock(s_queueLock);
				if (!s_pending.emplace(handle).second) {
					return;
				}
			}

			taskInterface->AddTask([handle, formID]() {
				{
					std::scoped_lock lock(s_queueLock);
					s_pending.erase(handle);
				}

				// Re-validate through the live form table: if the actor was deleted
				// or unloaded after the check was queued, its entry is gone and we
				// must not touch the stale pointer at all -- not even for an
				// IsDeleted()/IsDead() probe, which is itself a virtual call.
				// FormIDs are never reused within a session, so a hit here is the
				// same object, and there is no yield between this lookup and its
				// use below.
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
				if (!ref) {
					return;
				}
				if (auto* loaded = ref->As<RE::Actor>(); IsValidActor(loaded)) {
					ProcessActor(loaded);
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
			s_redirectDispel = ini.GetBoolValue("EEF", "RedirectDispelWornItemEnchantsVisitor", true);

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
		if (s_onEquip && a_event && a_event->equipped && a_event->actor) {
			if (auto* actor = a_event->actor->As<RE::Actor>(); IsValidActor(actor)) {
				if (const auto* form = RE::TESForm::LookupByID(a_event->baseObject); form && form->As<RE::TESObjectARMO>()) {
					ProcessActor(actor);
				}
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl LoadEventHandler::ProcessEvent(
		const RE::TESObjectLoadedEvent*               a_event,
		RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
	{
		if (a_event && a_event->loaded) {
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->formID)) {
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
