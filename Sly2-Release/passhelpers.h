#pragma once

// The frame's helper threads, for the per-frame loops over the whole scene (render.cpp's records, scene.cpp's lighting,
// scenesw.cpp's placement): Run(cpart, fn) calls fn(0) on the caller and fn(1) .. fn(cpart - 1) on up to two helpers,
// returning when all have; ForBlocks hands out [0, c) in blocks to whichever thread is free. Written for the Switch, on
// every platform since perf-4 (fastpaths.h; SLY2_FASTPATHS 0: everything on the calling thread). The helpers live as
// long as the process, in an object that is never destroyed (libnx has no thread detach, and a joinable thread's
// destructor terminates).

#include "fastpaths.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#if SLY2_FASTPATHS
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#endif
#endif

class PASSHELPERS
{
public:
    void Run(int cpart, const std::function<void(int)>& fn)
    {
#if SLY2_FASTPATHS
        if (!m_fStarted) Start();
        cpart = std::min(cpart, m_chelper + 1);
        if (cpart > 1)
        {
            RunShared(cpart, fn);
            return;
        }
#else
        (void)cpart;
#endif
        fn(0);
    }
    // fnBlock(begin, end) over [0, c) in blocks of cblock, on every thread there is
    void ForBlocks(size_t c, size_t cblock, const std::function<void(size_t, size_t)>& fnBlock)
    {
        if (c == 0) return;
        cblock = std::max<size_t>(cblock, 1);
        const size_t cblocks = (c + cblock - 1) / cblock;
        std::atomic<size_t> iNext{0};
        const std::function<void(int)> work = [&](int) {
            for (size_t ib; (ib = iNext.fetch_add(1, std::memory_order_relaxed)) < cblocks;)
                fnBlock(ib * cblock, std::min(c, (ib + 1) * cblock));
        };
        Run(cblocks > 1 ? 3 : 1, work);
    }

#if SLY2_FASTPATHS
private:
    // The helpers and the caller spin a while before they sleep: the frame's sections are mostly a fraction of a
    // millisecond apart, and a wake-up from the condition variable costs tens of microseconds.
    void RunShared(int cpart, const std::function<void(int)>& fn)
    {
        m_pfn = &fn;
        m_cdoneSpin.store(0, std::memory_order_relaxed);
        bool fNotify;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sectionSpin.store(((m_sectionSpin.load(std::memory_order_relaxed) >> 8) + 1) << 8 | uint64_t(cpart),
                                std::memory_order_release);
            fNotify = m_csleeping > 0;
        }
        if (fNotify) m_cvWork.notify_all();
        fn(0);
        const int cwait = cpart - 1;
        const auto tSpinEnd = std::chrono::steady_clock::now() + kSpin;
        while (m_cdoneSpin.load(std::memory_order_acquire) != cwait && std::chrono::steady_clock::now() < tSpinEnd) {}
        if (m_cdoneSpin.load(std::memory_order_acquire) != cwait)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_fWaiting = true;
            m_cvDone.wait(lock, [&] { return m_cdoneSpin.load(std::memory_order_acquire) == cwait; });
            m_fWaiting = false;
        }
        m_pfn = nullptr;
    }
    void Start()
    {
        m_fStarted = true;
        // (two helpers: on the Switch the runtime starts threads on cores 1 and 2, the game thread keeps core 0. Elsewhere
        // two fewer than the CPUs the process may run on, at most two: the game thread and the GL driver's thread keep
        // theirs (on two CPUs the helpers measured slower: they and the driver take turns). SLY2N_HELPERS=0..2 sets the
        // count, 0: everything on the calling thread)
        int chelper = 2;
#ifndef __SWITCH__
        int ccpu = int(std::thread::hardware_concurrency());
#if defined(__linux__)
        if (cpu_set_t set; sched_getaffinity(0, sizeof(set), &set) == 0) ccpu = CPU_COUNT(&set);   // (taskset, cgroups)
#endif
        if (ccpu > 0) chelper = std::clamp(ccpu - 2, 0, 2);
        if (const char* psz = std::getenv("SLY2N_HELPERS")) chelper = std::clamp(std::atoi(psz), 0, 2);
#endif
        m_athread.reserve(size_t(std::max(chelper, 0)));
        for (int i = 0; i < chelper; ++i)
        {
            try { m_athread.emplace_back([this, i] { Helper(i + 1); }); }
            catch (const std::exception&) { break; }
            m_chelper = i + 1;
        }
    }
    void Helper(int ipart)
    {
        uint64_t genSeen = 0;
        for (;;)
        {
            // the next section: watched a while (yielding to the core's other threads of its priority), then slept for
            uint64_t section = m_sectionSpin.load(std::memory_order_acquire);
            if (section == genSeen)
            {
                const auto tSpinEnd = std::chrono::steady_clock::now() + kSpin;
                while ((section = m_sectionSpin.load(std::memory_order_acquire)) == genSeen &&
                       std::chrono::steady_clock::now() < tSpinEnd)
                    std::this_thread::yield();
            }
            if (section == genSeen)
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                ++m_csleeping;
                m_cvWork.wait(lock, [&] { return m_sectionSpin.load(std::memory_order_acquire) != genSeen; });
                --m_csleeping;
                section = m_sectionSpin.load(std::memory_order_acquire);
            }
            genSeen = section;
            // (the part count comes with the section's number: a helper left out of a section never reads the caller's
            // other fields, which the next section may be rewriting; one taking part holds the caller until it is done)
            const int cpart = int(section & 0xFF);
            if (ipart >= cpart) continue;
            (*m_pfn)(ipart);
            if (m_cdoneSpin.fetch_add(1, std::memory_order_acq_rel) + 1 == cpart - 1)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_fWaiting) m_cvDone.notify_one();
            }
        }
    }
    std::vector<std::thread> m_athread;
    std::mutex m_mutex;
    std::condition_variable m_cvWork, m_cvDone;
    const std::function<void(int)>* m_pfn = nullptr;
    int m_chelper = 0;
    bool m_fStarted = false;
    // (the hand-off: the section (its number << 8 | its part count) and its finished parts as atomics; the sleepers
    // counted and the caller's wait flagged under m_mutex, so a notify is only sent to a thread that sleeps)
    static constexpr std::chrono::microseconds kSpin{200};
    std::atomic<uint64_t> m_sectionSpin{0};
    std::atomic<int> m_cdoneSpin{0};
    int m_csleeping = 0;
    bool m_fWaiting = false;
#endif
};

inline PASSHELPERS& PassHelpers()
{
    static PASSHELPERS* s_p = new PASSHELPERS;
    return *s_p;
}
