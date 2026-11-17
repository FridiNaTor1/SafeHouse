#pragma once
// Frame pacing on a display that always presents in step with its vblanks (iOS: a CAMetalLayer has no tearing mode, so
// "VSync off" there only meant a timer between the vblanks, drifting against them). Shared by Sly2-Release's display.cpp
// (the swap interval), platform/metal/display_link.mm (the display link that wakes the game) and its unit test
// (display_pacing_test.cpp, Linux).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace cane::pacing {

struct SwapChoice {
    int interval = 1;               // vblanks each frame stays up at least (Metal: presentAfterMinimumDuration from 2)
    bool fSwapPacesFrames = false;  // the display's frames pace the game (no timer)
    int displayFps = 0;             // the display link's rate when they do
};

// cap: the frame-rate cap (DISP_FRAMERATE_UNCAPPED and above: none, the display's rate); refresh: the display's maximum
// (0: not known). The display paces whenever the cap is its rate (or more) or a whole fraction of it: 120 Hz ProMotion
// with a 60 cap is every 2nd vblank, 40 every 3rd, 30 every 4th; a 60 Hz screen with 60 every vblank. Another cap
// (90 on a 120 Hz screen) is held by the frame-rate cap's timer, each frame at the next vblank.
inline SwapChoice ChooseLockedToVblank(int cap, int refresh) {
    SwapChoice c;
    if (refresh <= 0 || cap <= 0) return c;
    if (cap >= refresh) {
        c.fSwapPacesFrames = true;
        c.displayFps = refresh;
    } else if (refresh % cap == 0) {
        c.interval = refresh / cap;
        c.fSwapPacesFrames = true;
        c.displayFps = cap;
    }
    return c;
}

// A display link asked for a rate may still fire at the display's full rate (the system's choice; ProMotion): only the
// ticks at least 7/8 of a period of the wanted rate apart wake the game (an 8.3 ms tick short of 60's 16.7 is skipped,
// the 2nd one taken; 30: the 4th, never the 3rd). Times: the ticks' target timestamps, seconds.
class TickGate {
public:
    bool OnTick(double t, int fps) {
        if (fps <= 0 || m_last < 0.0 || t < m_last || t - m_last >= 0.875 / double(fps)) {
            m_last = t;
            return true;
        }
        return false;
    }
    void Reset() { m_last = -1.0; }

private:
    double m_last = -1.0;
};

// The game thread waits here for the next display frame (Signal: the display link's thread). A frame that began while
// the game was still busy wakes it at once: one wait per display frame at most, never a second sleep on top.
class FrameWaiter {
public:
    void Signal() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_seq;
        }
        m_cv.notify_one();
    }
    // True: a display frame began since the last call; false: none within the timeout (the link is paused or gone:
    // the caller paces with its timer instead).
    bool Wait(std::chrono::microseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_seq == m_seen && !m_cv.wait_for(lock, timeout, [&] { return m_seq != m_seen; })) return false;
        m_seen = m_seq;
        return true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_seq = 0, m_seen = 0;
};

}  // namespace cane::pacing
