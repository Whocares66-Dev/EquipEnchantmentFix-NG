#include "PCH.h"

#include "EEF.h"

namespace
{
	void SetupLog()
	{
		auto logsFolder = SKSE::log::log_directory();
		if (!logsFolder) {
			SKSE::stl::report_and_fail("no log dir");
		}
		auto path = *logsFolder / "EquipEnchantmentFix.log";
		auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto logger = std::make_shared<spdlog::logger>("log", std::move(fileSink));
		spdlog::set_default_logger(std::move(logger));
		spdlog::set_level(spdlog::level::trace);
		spdlog::flush_on(spdlog::level::trace);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			EEF::Initialize();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// Stale re-checks must not survive into a different save.
			EEF::ClearPendingChecks();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			// Only the weight recompute here. The actor re-check runs from the
			// event path alone (TESObjectLoadedEvent / TESInitScriptEvent): an
			// extra sweep from this message would process the same actors a
			// second time in the same frame, and UpdateArmorAbility does not
			// de-duplicate, so effects would be applied twice.
			EEF::RecalcPlayerWeight();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SetupLog();
	SKSE::log::info("EquipEnchantmentFix loading");
	// The UpdateArmorAbility hook needs a trampoline; ask for a small one up front.
	// DIAG: .log = false keeps the trace-level logger SetupLog installed. With
	// .log = true CommonLib replaces it with its own logger at info, and every
	// debug line in EEF.cpp is dropped.
	SKSE::Init(a_skse, SKSE::InitInfo{ .log = false, .trampoline = true, .trampolineSize = 1 << 7 });

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		SKSE::log::error("Failed to register message listener");
		return false;
	}

	SKSE::log::info("EquipEnchantmentFix loaded");
	return true;
}
