// platform/display_pacing.h: the swap interval chosen for a display locked to its vblanks (iOS), the display link's
// tick gate and the game thread's wait on it.
#include "display_pacing.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

using namespace cane::pacing;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}
bool same(SwapChoice c, int interval, bool paces, int fps) {
    return c.interval == interval && c.fSwapPacesFrames == paces && c.displayFps == fps;
}
// the ticks of a display link firing at hz for `seconds`, the game wanting fps: how many woke it
int woken(double hz, int fps, double seconds) {
    TickGate gate;
    int c = 0;
    for (int i = 0; i < int(hz * seconds + 0.5); ++i) c += gate.OnTick(double(i) / hz, fps);
    return c;
}
}  // namespace

int main() {
    // 120 Hz ProMotion
    check(same(ChooseLockedToVblank(60, 120), 2, true, 60), "60 on 120 Hz: every 2nd vblank, the display paces");
    check(same(ChooseLockedToVblank(120, 120), 1, true, 120), "120 on 120 Hz: every vblank");
    check(same(ChooseLockedToVblank(1000, 120), 1, true, 120), "uncapped on 120 Hz: the display's rate");
    check(same(ChooseLockedToVblank(144, 120), 1, true, 120), "144 on 120 Hz: the display's rate");
    check(same(ChooseLockedToVblank(30, 120), 4, true, 30), "30 on 120 Hz: every 4th vblank");
    check(same(ChooseLockedToVblank(40, 120), 3, true, 40), "40 on 120 Hz: every 3rd vblank");
    check(same(ChooseLockedToVblank(90, 120), 1, false, 0), "90 on 120 Hz: the timer");
    // 60 Hz screens (older iPhones; the old 60 Hz lock)
    check(same(ChooseLockedToVblank(60, 60), 1, true, 60), "60 on 60 Hz: every vblank");
    check(same(ChooseLockedToVblank(120, 60), 1, true, 60), "120 on 60 Hz: the display's 60");
    check(same(ChooseLockedToVblank(30, 60), 2, true, 30), "30 on 60 Hz: every 2nd vblank");
    check(same(ChooseLockedToVblank(60, 0), 1, false, 0), "refresh unknown: the timer");

    // The gate: a link that fires at the display's full rate whatever it was asked
    check(woken(120, 60, 10) == 600, "120 Hz ticks, 60 wanted: 60 a second");
    check(woken(120, 120, 10) == 1200, "120 Hz ticks, 120 wanted: all");
    check(woken(120, 40, 10) == 400, "120 Hz ticks, 40 wanted: 40 a second");
    check(woken(120, 30, 10) == 300, "120 Hz ticks, 30 wanted: 30 a second");
    check(woken(60, 120, 10) == 600, "60 Hz ticks (Low Power Mode), 120 wanted: every tick");
    check(woken(60, 60, 10) == 600, "60 Hz ticks, 60 wanted: every tick");
    {
        // (a tick lost: the next one wakes, a vblank later, no burst)
        TickGate gate;
        int c = 0;
        for (int i = 0; i < 1200; ++i)
            if (i != 2) c += gate.OnTick(double(i) / 120.0, 60);
        check(c == 600, "a lost tick: one wake a vblank late, still 60 a second");
    }

    // The waiter: a frame that began while the game was busy wakes it at once (no second wait); none pending waits
    {
        FrameWaiter w;
        check(!w.Wait(std::chrono::milliseconds(5)), "no tick: timeout (the caller's timer paces)");
        w.Signal();
        const auto t0 = std::chrono::steady_clock::now();
        check(w.Wait(std::chrono::milliseconds(200)), "a tick during the frame: at once");
        check(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50), "...without waiting");
        w.Signal();
        w.Signal();
        check(w.Wait(std::chrono::milliseconds(200)), "two ticks during a long frame: one wake");
        check(!w.Wait(std::chrono::milliseconds(5)), "...not two (no catch-up burst)");
    }
    {
        // the game thread at a 60 Hz link's pace: frames of 5 ms wait out the rest of each period
        FrameWaiter w;
        std::atomic<bool> stop{false};
        std::thread link([&] {
            auto t = std::chrono::steady_clock::now();
            while (!stop) {
                t += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(t);
                w.Signal();
            }
        });
        w.Wait(std::chrono::milliseconds(100));
        const auto t0 = std::chrono::steady_clock::now();
        int frames = 0;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (w.Wait(std::chrono::milliseconds(100))) ++frames;
        }
        stop = true;
        link.join();
        // (real time: a busy CI machine oversleeps and gets fewer frames, so only the upper bound is strict: the
        // waiter never runs ahead of the link or bursts to catch up)
        check(frames >= 1 && frames <= 32, "60 Hz link, 5 ms frames: at most 30 frames in 0.5 s (got " + std::to_string(frames) + ")");
    }

    if (failures) {
        std::printf("display pacing: %d failed\n", failures);
        return 1;
    }
    std::printf("display pacing: all passed\n");
    return 0;
}
