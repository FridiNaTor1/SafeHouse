#pragma once
// The frame-rate cap's pacer (Sly1-Release main.cpp's FramePacer, shared for Sly2-Release): waits until the frame's
// deadline with a coarse sleep and a short precise finish.
//
// - Windows: a high-resolution waitable timer (Windows 10 1803+; older: timeBeginPeriod(1), link winmm). A plain
//   std::this_thread::sleep_for rounds up to the system timer period (15.6 ms by default), so a frame with time to
//   spare (standing still: less work) overslept its deadline while a busier one (moving) did not sleep at all and
//   kept the rate: the frame rate was lower standing than moving.
// - Linux: the timer slack set to 1 ns (the default 50 us delays the final short sleep past the deadline).
// - A deep idle state's wake-up lateness is measured and the coarse sleep ends that much earlier.
// - With VSync gating at the cap (fSwapPacesFrames), the timer runs 1 % fast so only the swap gates: two gates at
//   nearly equal rates drift against each other and drop a vblank now and then. If the timer still has to wait on
//   every frame the swap is not pacing (a clamped interval, a driver override, a minimized window): exact pacing
//   until the swap configuration changes (swapGeneration).
// - A missed deadline is dropped: the next frame is one interval from now (no catch-up burst).
#include <algorithm>
#include <chrono>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "game_sched.h"   // (timeBeginPeriod / timeEndPeriod)
#elif defined(__linux__)
#include <sys/prctl.h>
#endif

namespace cane {

class FramePacer
{
public:
    using Clock = std::chrono::steady_clock;

    FramePacer() : m_deadline(Clock::now())
    {
#ifdef _WIN32
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2), by value for SDKs that predate the name
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (m_timer == nullptr)
        {
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
            m_fTimerPeriodRaised = timeBeginPeriod(1) == 0u;
            m_minCoarseMargin = std::chrono::milliseconds(2);
        }
#elif defined(__linux__)
        prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
#endif
    }
    ~FramePacer()
    {
#ifdef _WIN32
        if (m_timer != nullptr) CloseHandle(m_timer);
        if (m_fTimerPeriodRaised) timeEndPeriod(1);
#endif
    }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // frameRate: the cap (> 0). fSwapPacesFrames: VSync should block each swap at that rate. swapGeneration: changes
    // whenever the swap configuration changes.
    void Wait(int frameRate, bool fSwapPacesFrames = false, unsigned swapGeneration = 0)
    {
        if (m_frameRate != frameRate)
        {
            m_frameRate = frameRate;
            m_deadline = Clock::now();
        }
        if (m_swapGeneration != swapGeneration)
        {
            m_swapGeneration = swapGeneration;
            m_fSwapNotPacing = false;
            m_waitStreak = 0;
        }
        const bool fLenient = fSwapPacesFrames && !m_fSwapNotPacing;
        double frameSeconds = 1.0 / double(std::max(frameRate, 1));
        if (fLenient) frameSeconds *= 0.99;
        m_deadline += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(frameSeconds));
        const auto now = Clock::now();
        if (now >= m_deadline)
        {
            m_deadline = now;
            m_waitStreak = 0;
            return;
        }
        if (fLenient && ++m_waitStreak >= 30) m_fSwapNotPacing = true;

        const auto coarseDeadline = m_deadline - m_minCoarseMargin - (std::min)(m_wakeLateness, kMaxWakeLateness);
        if (now < coarseDeadline)
        {
#ifdef _WIN32
            if (m_timer != nullptr)
            {
                const auto wait100ns =
                    std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>>(coarseDeadline - now).count();
                LARGE_INTEGER dueTime{};
                dueTime.QuadPart = -(std::max<LONGLONG>)(1, wait100ns);
                if (SetWaitableTimer(m_timer, &dueTime, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(m_timer, INFINITE);
                else std::this_thread::sleep_until(coarseDeadline);
            }
            else std::this_thread::sleep_until(coarseDeadline);
#else
            std::this_thread::sleep_until(coarseDeadline);
#endif
            // (decays by 1/64 a frame: about a second at 60 FPS)
            const auto lateness = Clock::now() - coarseDeadline;
            m_wakeLateness = (std::max)(lateness, m_wakeLateness - m_wakeLateness / 64);
        }
#ifdef _WIN32
        // (SwitchToThread lets the driver and audio threads run; it returns at once when nothing else is ready)
        while (Clock::now() < m_deadline - kSpinMargin) SwitchToThread();
#else
        // (too short for the idle governor to pick a deep state: wakes within microseconds)
        if (Clock::now() < m_deadline - kSpinMargin) std::this_thread::sleep_until(m_deadline - kSpinMargin);
#endif
        while (Clock::now() < m_deadline)
        {
#if defined(_WIN32)
            YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
        }
    }

private:
    static constexpr Clock::duration kSpinMargin = std::chrono::microseconds(50);
    static constexpr Clock::duration kMaxWakeLateness = std::chrono::microseconds(1500);
#ifdef _WIN32
    HANDLE m_timer = nullptr;
    bool m_fTimerPeriodRaised = false;
    Clock::duration m_minCoarseMargin = std::chrono::microseconds(250);
#else
    Clock::duration m_minCoarseMargin = std::chrono::microseconds(200);
#endif
    Clock::duration m_wakeLateness = Clock::duration::zero();
    Clock::time_point m_deadline;
    int m_frameRate = 0;
    int m_waitStreak = 0;
    unsigned m_swapGeneration = 0;
    bool m_fSwapNotPacing = false;
};

} // namespace cane
