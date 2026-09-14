#pragma once

namespace EEF
{
	bool Initialize();

	// Drop every queued actor re-check (new game / load game).
	void ClearPendingChecks();

	// Force the player's inventory weight to be recomputed after a save loads.
	void RecalcPlayerWeight();

	class EquipEventHandler final : public RE::BSTEventSink<RE::TESEquipEvent>
	{
	public:
		static EquipEventHandler* GetSingleton();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESEquipEvent*               a_event,
			RE::BSTEventSource<RE::TESEquipEvent>* a_eventSource) override;
	};

	// On actor load / script init, the engine can restore a save without replaying
	// the enchantment abilities onto worn items. Verify them a frame later.
	class LoadEventHandler final :
		public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
		public RE::BSTEventSink<RE::TESInitScriptEvent>
	{
	public:
		static LoadEventHandler* GetSingleton();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESObjectLoadedEvent*               a_event,
			RE::BSTEventSource<RE::TESObjectLoadedEvent>* a_eventSource) override;

		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESInitScriptEvent*               a_event,
			RE::BSTEventSource<RE::TESInitScriptEvent>* a_eventSource) override;
	};

	// The engine can wrongly dispel a worn item's enchantment while the inventory
	// changes. Without a hook we cannot redirect the engine's dispel visitor, so
	// instead we watch for a removed effect and re-check the affected actor a
	// frame later -- ProcessActor re-applies anything that was wrongly taken.
	class ActiveEffectEventHandler final :
		public RE::BSTEventSink<RE::TESActiveEffectApplyRemoveEvent>
	{
	public:
		static ActiveEffectEventHandler* GetSingleton();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESActiveEffectApplyRemoveEvent*               a_event,
			RE::BSTEventSource<RE::TESActiveEffectApplyRemoveEvent>* a_eventSource) override;
	};
}
