#ifndef CANE_PLATFORM_METAL_TRANSIENT_ALLOCATOR_H_
#define CANE_PLATFORM_METAL_TRANSIENT_ALLOCATOR_H_

#import <Metal/Metal.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace cane::gl_metal {

struct TransientAllocation {
  id<MTLBuffer> buffer = nil;
  size_t offset = 0;
  void* cpu = nullptr;
};

// Bump allocation out of large shared buffers. A page becomes reusable when
// the submission that last used it completes.
class TransientAllocator {
 public:
  explicit TransientAllocator(id<MTLDevice> device, size_t page_bytes = 4u << 20);

  TransientAllocation Allocate(size_t bytes, size_t alignment = 256);

  // Marks every page touched since the previous call as owned by `serial`.
  void EndSubmission(uint64_t serial);
  // Returns pages whose submission completed to the free list.
  void Recycle(uint64_t last_completed);

 private:
  struct Page {
    id<MTLBuffer> buffer = nil;
    size_t used = 0;
    uint64_t submission = 0;
  };

  Page NewPage(size_t bytes);

  id<MTLDevice> device_;
  size_t page_bytes_;
  Page current_;
  std::vector<Page> in_flight_;
  std::deque<Page> free_;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_TRANSIENT_ALLOCATOR_H_
