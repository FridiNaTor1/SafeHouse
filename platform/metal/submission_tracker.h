#ifndef CANE_PLATFORM_METAL_SUBMISSION_TRACKER_H_
#define CANE_PLATFORM_METAL_SUBMISSION_TRACKER_H_

#import <Metal/Metal.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace cane::gl_metal {

// Numbers each committed command buffer. Completion is observed through the
// buffer's completion handler, so waits are cheap when the work is done.
class SubmissionTracker {
 public:
  // Registers the command buffer about to be committed and returns its serial.
  uint64_t Register(id<MTLCommandBuffer> command_buffer);

  uint64_t last_submitted() const { return last_submitted_.load(); }
  uint64_t last_completed() const { return last_completed_.load(); }
  bool IsComplete(uint64_t serial) const { return serial <= last_completed(); }

  // Returns false on timeout.
  bool WaitFor(uint64_t serial, uint64_t timeout_ns);
  void WaitAll();

  // GPU elapsed nanoseconds of a completed submission (0 when unknown).
  uint64_t GpuDurationNs(uint64_t serial);

 private:
  void OnCompleted(uint64_t serial, uint64_t gpu_ns);

  std::atomic<uint64_t> last_submitted_{0};
  std::atomic<uint64_t> last_completed_{0};
  std::mutex mutex_;
  std::condition_variable completed_;
  std::unordered_map<uint64_t, uint64_t> durations_;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_SUBMISSION_TRACKER_H_
