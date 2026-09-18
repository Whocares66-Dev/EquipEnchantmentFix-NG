#include "PCH.h"

#include "EEF.h"

// Detours needs the Windows API declared first, and asks for it by name.
#include <Windows.h>

#include <detours/detours.h>

// Diagnostic: every dispel of an armour-sourced effect on a player teammate,
// with the addresses that led to it. ActiveEffect::Dispel is the one routine
// every dispel path ends in, so a function-entry detour on it sees them all:
// the engine's worn-item visitor, the unequip path, and anything else. Each
// frame is printed as module+offset so an exe frame can be turned into an
// Address Library id (tools/disasm.py --lookup) and a plugin frame names the
// plugin. Off unless DiagDispelTrace=true in the ini; a detour, not a call
// site, so the trampoline relocates the prologue and the id alone places it.
namespace EEF::DispelTrace
{
	namespace
	{
		using Dispel_t = void (*)(RE::ActiveEffect*, bool);
		Dispel_t Dispel_orig{ nullptr };

		std::string Describe(const void* a_address)
		{
			HMODULE module = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &module) &&
				module) {
				wchar_t path[MAX_PATH]{};
				GetModuleFileNameW(module, path, MAX_PATH);
				std::wstring wide(path);
				const auto slash = wide.find_last_of(L"\\/");
				const std::wstring name = slash == std::wstring::npos ? wide : wide.substr(slash + 1);
				// Module file names here are ASCII; anything else becomes '?'.
				std::string narrow;
				for (const wchar_t c : name) {
					narrow.push_back(c < 0x80 ? static_cast<char>(c) : '?');
				}
				return std::format("{}+{:#x}", narrow, reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module));
			}
			return std::format("{:#x}", reinterpret_cast<std::uintptr_t>(a_address));
		}

		void Dispel_Hook(RE::ActiveEffect* a_effect, bool a_force)
		{
			if (a_effect && a_effect->source && a_effect->source->As<RE::TESObjectARMO>() && a_effect->target) {
				// Never GetTargetActor(): it returns a pointer into the actor and
				// has crashed the game. The stats object, then As<Actor>.
				auto* ref = a_effect->target->GetTargetStatsObject();
				auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (actor && actor->IsPlayerTeammate()) {
					void*      frames[10]{};
					const auto count = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
					std::string trace;
					for (USHORT i = 0; i < count; ++i) {
						trace += (i ? " < " : "") + Describe(frames[i]);
					}
					SKSE::log::info("dispel trace: actor {:08X} source {:08X} spell {:08X} force={} dispelled={} : {}",
						actor->GetFormID(), a_effect->source->GetFormID(),
						a_effect->spell ? a_effect->spell->GetFormID() : 0, a_force,
						a_effect->flags.any(RE::ActiveEffect::Flag::kDispelled), trace);
				}
			}
			Dispel_orig(a_effect, a_force);
		}
	}

	bool Install()
	{
		const REL::Relocation<std::uintptr_t> target{ REL::RelocationID(33286, 34061) };
		Dispel_orig = reinterpret_cast<Dispel_t>(target.address());
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&reinterpret_cast<PVOID&>(Dispel_orig), reinterpret_cast<PVOID>(&Dispel_Hook));
		const LONG result = DetourTransactionCommit();
		if (result != NO_ERROR) {
			SKSE::log::error("dispel trace: could not detour ActiveEffect::Dispel (Detours error {})", result);
			return false;
		}
		SKSE::log::info("dispel trace: ActiveEffect::Dispel at {:X} detoured", target.address());
		return true;
	}
}
