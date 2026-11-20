#pragma once

// (test aid: SLY2N_STALLPRINT=ms) a watchdog thread that says on stderr when the game thread went that long without
// looking at the window's events, with the phase it was in and the frame, and how long the stall lasted once it is
// over (tools/prologuesmoke.sh fails on one). Windows marks such a window "Not Responding" after 5 s (exit 0xCFFFFFFF
// when the player closes it; platform/crash_report.h reports it in the field after 10 s). Beat() where the game thread
// polls the window's events; Phase() names what it does next (a string literal).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace stall {

inline std::atomic<int64_t> g_msBeat{0};
inline std::atomic<const char*> g_szPhase{"start"};
inline std::atomic<int> g_frame{0};
inline int64_t MsNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void Beat() { g_msBeat.store(MsNow(), std::memory_order_relaxed); }
inline void Phase(const char* sz) { g_szPhase.store(sz, std::memory_order_relaxed); }
inline void Frame(int frame) { g_frame.store(frame, std::memory_order_relaxed); }

// Starts the watchdog (once)
inline void Start()
{
    static bool s_fStarted = false;
    const char* sz = std::getenv("SLY2N_STALLPRINT");
    const int64_t msSet = sz ? std::atoll(sz) : 0;
    if (s_fStarted || msSet <= 0) return;
    s_fStarted = true;
    const int64_t msLimit = std::max<int64_t>(50, msSet);
    Beat();
    std::thread([msLimit] {
        int64_t msWorst = 0;
        bool fIn = false;
        int64_t msBeatIn = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(msLimit >= 2000 ? 100 : 20));
            const int64_t msBeat = g_msBeat.load(std::memory_order_relaxed);
            const int64_t ms = MsNow() - msBeat;
            if (!fIn && ms >= msLimit)
            {
                fIn = true;
                msBeatIn = msBeat;
                std::fprintf(stderr, "stall: no window events for %lld ms (frame %d, %s)\n", static_cast<long long>(ms),
                             g_frame.load(), g_szPhase.load());
            }
            else if (fIn && msBeat != msBeatIn)
            {
                fIn = false;
                const int64_t msLen = msBeat - msBeatIn;
                if (msLen > msWorst) msWorst = msLen;
                std::fprintf(stderr, "stall: over after %lld ms (now frame %d, %s; worst %lld ms)\n", static_cast<long long>(msLen),
                             g_frame.load(), g_szPhase.load(), static_cast<long long>(msWorst));
            }
        }
    }).detach();
}

}  // namespace stall
