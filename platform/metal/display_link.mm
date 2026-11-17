// (iOS) The game's frames in step with the display: a CADisplayLink on a thread of its own wakes the game thread at
// the frame-rate cap's rate (gl_metal.h WaitForDisplayFrame; Sly2-Release game.cpp's loop, display.cpp's choice).
//
// Why: iOS always presents a drawable at a vblank. Paced by a timer of its own, the game's 60 frames a second landed
// on a 120 Hz ProMotion screen's vblanks unevenly (a frame up for one vblank, the next for three; frames dropped as the
// timer and the display drifted), which looked slower than the old 60 Hz lock. The link asks for the cap's rate
// (preferredFrameRateRange, so iOS may also run the panel at 60 Hz for a 60 cap) and the game starts a frame on each of
// its ticks; a link that still fires at the full rate is thinned by the tick gate (display_pacing.h).
//
// The link pauses itself when nobody waited for half a second (the game left, the launcher runs) and resumes on the
// next wait. Not on macOS (its CAMetalLayer has displaySyncEnabled: the swap blocks).
#include "gl_metal.h"

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <pthread.h>
#include <pthread/qos.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "../display_pacing.h"

namespace {

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct LinkState {
  cane::pacing::FrameWaiter waiter;
  cane::pacing::TickGate gate;  // (the link's thread only)
  std::atomic<int> want_fps{0};
  std::atomic<int64_t> last_wait_ns{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> paused{false};
  CFRunLoopRef run_loop = nullptr;  // (written before ready)
};

// (never destroyed: the link's thread lives as long as the process, which every program in the app shares)
LinkState& State() {
  static LinkState* state = new LinkState;
  return *state;
}

CADisplayLink* g_link = nil;  // (the link's thread only)

}  // namespace

@interface CaneDisplayLinkTarget : NSObject
@property(nonatomic) int appliedFps;
- (void)applyFps:(CADisplayLink*)link fps:(int)fps;
- (void)tick:(CADisplayLink*)link;
@end

@implementation CaneDisplayLinkTarget
- (void)applyFps:(CADisplayLink*)link fps:(int)fps {
  self.appliedFps = fps;
  if (fps <= 0) return;
  // (iOS 15, the deployment target: cmake/ios-toolchain.cmake)
  const float rate = static_cast<float>(fps);
  link.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
}
- (void)tick:(CADisplayLink*)link {
  LinkState& state = State();
  const int fps = state.want_fps.load(std::memory_order_relaxed);
  if (fps != self.appliedFps) [self applyFps:link fps:fps];
  if (NowNs() - state.last_wait_ns.load(std::memory_order_relaxed) > 500000000) {
    link.paused = YES;
    state.gate.Reset();
    state.paused.store(true, std::memory_order_release);
    return;
  }
  if (state.gate.OnTick(link.targetTimestamp, fps)) state.waiter.Signal();
}
@end

namespace {

void LinkThread() {
  pthread_setname_np("cane display link");
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  LinkState& state = State();
  @autoreleasepool {
    CaneDisplayLinkTarget* target = [CaneDisplayLinkTarget new];
    g_link = [CADisplayLink displayLinkWithTarget:target selector:@selector(tick:)];
    if (!g_link) {
      std::fprintf(stderr, "Metal: no display link (the frame-rate cap's timer paces the frames)\n");
      state.failed.store(true);
      return;
    }
    [target applyFps:g_link fps:state.want_fps.load()];
    [g_link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
    state.run_loop = CFRunLoopGetCurrent();
    CFRetain(state.run_loop);
    state.ready.store(true, std::memory_order_release);
  }
  for (;;) {
    @autoreleasepool {
      if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 60.0, false) == kCFRunLoopRunFinished)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

}  // namespace

namespace cane::gl_metal {

bool WaitForDisplayFrame(int fps) {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(LinkThread).detach(); });
  LinkState& state = State();
  if (fps <= 0 || state.failed.load()) return false;
  // (the game thread only; for testers' logs)
  static int logged_fps = 0;
  if (fps != logged_fps) {
    logged_fps = fps;
    std::fprintf(stderr, "Metal: frames start on the display link's ticks at %d FPS (no timer)\n", fps);
  }
  state.want_fps.store(fps, std::memory_order_relaxed);
  state.last_wait_ns.store(NowNs(), std::memory_order_relaxed);
  if (!state.ready.load(std::memory_order_acquire)) return false;  // (its first frames: the timer)
  if (state.paused.exchange(false, std::memory_order_acq_rel)) {
    CFRunLoopPerformBlock(state.run_loop, kCFRunLoopCommonModes, ^{
      g_link.paused = NO;
    });
    CFRunLoopWakeUp(state.run_loop);
  }
  // (50 ms: a link that stopped (the app in the background) hands the frames to the timer)
  if (state.waiter.Wait(std::chrono::milliseconds(50))) return true;
  static int timeouts = 0;
  if (timeouts++ % 600 == 0)
    std::fprintf(stderr, "Metal: no display link tick in 50 ms (%d times): the timer paces the frames\n", timeouts);
  return false;
}

}  // namespace cane::gl_metal

#else

namespace cane::gl_metal {
bool WaitForDisplayFrame(int) { return false; }
}  // namespace cane::gl_metal

#endif
