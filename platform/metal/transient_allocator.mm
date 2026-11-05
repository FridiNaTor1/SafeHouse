#include "transient_allocator.h"

#include <algorithm>

namespace cane::gl_metal {

TransientAllocator::TransientAllocator(id<MTLDevice> device, size_t page_bytes)
    : device_(device), page_bytes_(page_bytes) {}

TransientAllocator::Page TransientAllocator::NewPage(size_t bytes) {
  Page page;
  page.buffer = [device_
      newBufferWithLength:bytes
                  options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
  return page;
}

TransientAllocation TransientAllocator::Allocate(size_t bytes, size_t alignment) {
  const size_t aligned = (current_.used + alignment - 1) / alignment * alignment;
  if (current_.buffer == nil || aligned + bytes > current_.buffer.length) {
    if (current_.buffer != nil) in_flight_.push_back(current_);
    current_ = Page{};
    const size_t needed = std::max(page_bytes_, bytes);
    while (!free_.empty()) {
      Page candidate = free_.front();
      free_.pop_front();
      if (candidate.buffer.length >= needed) {
        current_ = candidate;
        current_.used = 0;
        break;
      }
    }
    if (current_.buffer == nil) current_ = NewPage(needed);
    return Allocate(bytes, alignment);
  }
  TransientAllocation result;
  result.buffer = current_.buffer;
  result.offset = aligned;
  result.cpu = static_cast<uint8_t*>(current_.buffer.contents) + aligned;
  current_.used = aligned + bytes;
  return result;
}

void TransientAllocator::EndSubmission(uint64_t serial) {
  if (current_.buffer != nil && current_.used > 0) {
    in_flight_.push_back(current_);
    current_ = Page{};
  }
  for (Page& page : in_flight_) {
    if (page.submission == 0) page.submission = serial;
  }
}

void TransientAllocator::Recycle(uint64_t last_completed) {
  auto it = std::remove_if(in_flight_.begin(), in_flight_.end(), [&](Page& page) {
    if (page.submission != 0 && page.submission <= last_completed) {
      page.used = 0;
      page.submission = 0;
      free_.push_back(page);
      return true;
    }
    return false;
  });
  in_flight_.erase(it, in_flight_.end());
}

}  // namespace cane::gl_metal
