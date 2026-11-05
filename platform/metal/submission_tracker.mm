#include "submission_tracker.h"

#include <chrono>

namespace cane::gl_metal {

uint64_t SubmissionTracker::Register(id<MTLCommandBuffer> command_buffer) {
  const uint64_t serial = ++last_submitted_;
  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> finished) {
    const double seconds = finished.GPUEndTime - finished.GPUStartTime;
    const uint64_t gpu_ns = seconds > 0.0 ? static_cast<uint64_t>(seconds * 1e9) : 0;
    OnCompleted(serial, gpu_ns);
  }];
  return serial;
}

void SubmissionTracker::OnCompleted(uint64_t serial, uint64_t gpu_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t completed = last_completed_.load();
  while (completed < serial && !last_completed_.compare_exchange_weak(completed, serial)) {
  }
  durations_[serial] = gpu_ns;
  if (durations_.size() > 256) {
    for (auto it = durations_.begin(); it != durations_.end();) {
      if (it->first + 64 < serial) {
        it = durations_.erase(it);
      } else {
        ++it;
      }
    }
  }
  completed_.notify_all();
}

bool SubmissionTracker::WaitFor(uint64_t serial, uint64_t timeout_ns) {
  if (IsComplete(serial)) return true;
  std::unique_lock<std::mutex> lock(mutex_);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
  return completed_.wait_until(lock, deadline, [&] { return IsComplete(serial); });
}

void SubmissionTracker::WaitAll() {
  const uint64_t target = last_submitted();
  if (IsComplete(target)) return;
  std::unique_lock<std::mutex> lock(mutex_);
  completed_.wait(lock, [&] { return IsComplete(target); });
}

uint64_t SubmissionTracker::GpuDurationNs(uint64_t serial) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = durations_.find(serial);
  return it == durations_.end() ? 0 : it->second;
}

}  // namespace cane::gl_metal
