#pragma once
// Work for the cores the game thread leaves free (runtime.cpp starts every std::thread on cores 1-2), below the game's
// threads' priority, and whole-file reads: the texture mods' PNGs read and decoded while the game goes on
// (Sly1-Release / Sly2-Release texmod.cpp).
#include "runtime.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace CaneSwitch
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
        LowerThreadPriority();
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

// A whole file in one read: the SD card's file system pays for every read, and stb_image reads a FILE 128 bytes at a
// time.
inline bool ReadFile(const char* path, std::vector<uint8_t>& out)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && st.st_size > 0;
    if (ok)
    {
        out.resize(size_t(st.st_size));
        for (size_t done = 0; ok && done < out.size();)
        {
            const ssize_t n = read(fd, out.data() + done, out.size() - done);
            if (n <= 0) ok = false;
            else done += size_t(n);
        }
    }
    close(fd);
    return ok;
}
}  // namespace CaneSwitch
