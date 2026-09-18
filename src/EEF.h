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

	// Actors that stream in after a load still need a check. Both event
	// sources feed the same batched queue: TESObjectLoadedEvent covers every
	// streaming reference (including scriptless NPCs), TESInitScriptEvent
	// covers scripted ones. Neither queues a task per event -- see
	// ScheduleActorCheck -- so the load-time burst stays cheap.
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
}
