#pragma once

// PC addition (the converter, notes/UNRAVEL_2026-11-10.md "Install speed"): a small pool of worker threads for
// independent jobs (a file converted and written, a texture's PNG written). Jobs may carry a weight (about the bytes
// they hold in memory): the pool starts a job only while the weights in flight stay under its budget (one job alone
// always runs), so a few large movies at once cannot take all the memory.

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class CWorkPool
{
public:
    // cthread 0: the machine's (SLY2N_CONVERT_THREADS overrides; 1 runs every job on the caller's thread at Push)
    explicit CWorkPool(int cthread = 0, uint64_t cbBudget = uint64_t(1536) << 20) : m_cbBudget(cbBudget)
    {
        if (const char* sz = std::getenv("SLY2N_CONVERT_THREADS")) cthread = std::atoi(sz);
        if (cthread <= 0) cthread = int(std::clamp<unsigned>(std::thread::hardware_concurrency(), 2u, 16u));
        m_fInline = cthread == 1;
        if (!m_fInline)
            for (int i = 0; i < cthread; ++i) m_athread.emplace_back([this] { Work(); });
    }
    CWorkPool(const CWorkPool&) = delete;
    CWorkPool& operator=(const CWorkPool&) = delete;
    ~CWorkPool()
    {
        Wait();
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_fQuit = true;
        }
        m_cv.notify_all();
        for (std::thread& t : m_athread) t.join();
    }
    [[nodiscard]] int Cthread() const { return m_fInline ? 1 : int(m_athread.size()); }

    void Push(std::function<void()> fn, uint64_t cbWeight = 0)
    {
        if (m_fInline)
        {
            fn();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_ajob.push_back({std::move(fn), cbWeight});
            ++m_cpending;
        }
        m_cv.notify_one();
    }
    // Returns when every job pushed so far has run.
    void Wait()
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cvDone.wait(lock, [this] { return m_cpending == 0; });
    }

private:
    struct JOB
    {
        std::function<void()> fn;
        uint64_t cb;
    };
    void Work()
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        for (;;)
        {
            m_cv.wait(lock, [this] { return m_fQuit || FCanStart(); });
            if (m_ajob.empty() && m_fQuit) return;
            JOB job = std::move(m_ajob.front());
            m_ajob.pop_front();
            m_cbInFlight += job.cb;
            ++m_crunning;
            lock.unlock();
            job.fn();
            lock.lock();
            m_cbInFlight -= job.cb;
            --m_crunning;
            if (--m_cpending == 0) m_cvDone.notify_all();
            m_cv.notify_all();
        }
    }
    bool FCanStart() const
    {
        if (m_ajob.empty()) return false;
        return m_crunning == 0 || m_cbInFlight + m_ajob.front().cb <= m_cbBudget;
    }

    std::vector<std::thread> m_athread;
    std::deque<JOB> m_ajob;
    std::mutex m_mtx;
    std::condition_variable m_cv, m_cvDone;
    uint64_t m_cbBudget, m_cbInFlight = 0;
    int m_crunning = 0;
    std::size_t m_cpending = 0;
    bool m_fQuit = false, m_fInline = false;
};
