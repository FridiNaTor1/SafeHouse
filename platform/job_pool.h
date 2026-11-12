#pragma once
// Background jobs for a game's spare cores (the texture mods read and decoded while the game goes on: Sly2-Release
// texmod.cpp). On the Switch this is platform/switch/job_pool.h's pool (cores 1-2, below the game's threads'
// priority); elsewhere the same pool on plain threads.
#ifdef __SWITCH__
#include "switch/job_pool.h"
namespace cane
{
using JobPool = CaneSwitch::JobPool;
}
#else
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cane
{
class JobPool
{
public:
    explicit JobPool(int threads)
    {
        for (int i = 0; i < threads; ++i) m_threads.emplace_back([this] { Work(); });
    }
    ~JobPool()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            m_jobs.clear();
        }
        m_wake.notify_all();
        for (std::thread& t : m_threads) t.join();
    }
    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    void Push(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_jobs.push_back(std::move(job));
        }
        m_wake.notify_one();
    }
    // The jobs not started yet dropped.
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.clear();
    }

private:
    void Work()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
                if (m_stop) return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            try { job(); }
            catch (...) {}
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_jobs;
    std::vector<std::thread> m_threads;
    bool m_stop = false;
};
}  // namespace cane
#endif
