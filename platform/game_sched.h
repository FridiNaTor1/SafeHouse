#pragma once
// Windows scheduling for a game process (shared by Sly1-Release and Sly2-Release; nothing elsewhere).
//
// A tester's mid-range Windows PC (RX 580) ran the Prologue slowly with the CPU at 32 % and the GPU at 12 %: the
// time went to waiting, not work. Windows' defaults are made for background work:
// - the system timer ticks every 15.6 ms: a sleep of 1 ms takes up to 15.6 (the sound thread's 240 Hz tick, which
//   holds the sound lock the game thread waits on, woke 15 ms late and ran its missed ticks in a burst);
//   timeBeginPeriod(1) for the process's life (timeEndPeriod at exit; winmm);
// - the power manager's throttling (EcoQoS) of a process it considers background: opted out;
// - MMCSS: the game thread registered as "Games", the sound thread as "Audio" (avrt.dll, loaded at run time: absent
//   on some trimmed systems; then nothing is done).
// Every call is best effort: a failure changes nothing. Wine accepts all of them.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// (timeBeginPeriod / timeEndPeriod (winmm) declared here: mmsystem.h breaks where a compat header undefines
// near / far, and WIN32_LEAN_AND_MEAN leaves it out of windows.h)
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int uPeriod);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int uPeriod);
#include <cstdlib>

namespace cane::sched {

namespace detail {
inline HANDLE MmcssRegister(const wchar_t* task)
{
    using PFN = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    static PFN s_pfn = [] {
        HMODULE h = LoadLibraryW(L"avrt.dll");
        return h ? reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(h, "AvSetMmThreadCharacteristicsW"))) : nullptr;
    }();
    DWORD index = 0;
    return s_pfn ? s_pfn(task, &index) : nullptr;
}
}  // namespace detail

// At the process's start, from the game (main) thread.
inline void StartupGameScheduling()
{
    static bool s_fDone = false;
    if (s_fDone) return;
    s_fDone = true;
    if (timeBeginPeriod(1) == 0u) std::atexit([] { timeEndPeriod(1); });
    // ProcessPowerThrottling (4): execution speed throttling off (Windows 10 1709+; SetProcessInformation: Windows 8+)
    struct POWER_THROTTLING { ULONG Version, ControlMask, StateMask; } pt{1, 0x1, 0};
    using PFNSPI = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    if (auto pfn = reinterpret_cast<PFNSPI>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"))))
        pfn(GetCurrentProcess(), 4, &pt, sizeof(pt));
    detail::MmcssRegister(L"Games");
}

// From the sound thread (its tick paces the 989snd voices).
inline void RegisterAudioThread() { detail::MmcssRegister(L"Audio"); }

}  // namespace cane::sched
#elif defined(__SWITCH__)
// The Switch: the game thread keeps core 0 to itself; the 989snd tick runs beside the mixer (platform/switch/runtime.h).
#include "switch/runtime.h"
namespace cane::sched {
inline void StartupGameScheduling() { CaneSwitch::PinGameThread(); }
inline void RegisterAudioThread() { CaneSwitch::PinSoundThread(); }
}  // namespace cane::sched
#else
namespace cane::sched {
inline void StartupGameScheduling() {}
inline void RegisterAudioThread() {}
}  // namespace cane::sched
#endif
