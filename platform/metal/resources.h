#ifndef CANE_PLATFORM_METAL_RESOURCES_H_
#define CANE_PLATFORM_METAL_RESOURCES_H_

#import <Metal/Metal.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "binding_slots.h"
#include "gl_enums.h"

namespace cane::gl_metal {

struct BufferRecord {
  id<MTLBuffer> buffer = nil;
  size_t size = 0;
  // Persistent maps hand out the shared-storage contents directly.
  void* mapped = nullptr;
  size_t mapped_offset = 0;
  size_t mapped_length = 0;
  bool mapped_persistent = false;
  std::vector<uint8_t> staging;
  // Set when the GPU may still read the current allocation; the next
  // glBufferData then allocates a fresh MTLBuffer instead of overwriting.
  uint64_t last_use_submission = 0;
};

struct TextureRecord {
  id<MTLTexture> texture = nil;
  GLenum target = 0;
  MTLPixelFormat format = MTLPixelFormatInvalid;
  GLenum internal_format = 0;
  int width = 0;
  int height = 0;
  int layers = 1;
  int levels = 1;
  int samples = 1;
  bool immutable = false;
  bool renderable = false;
  // Per-texture sampling state (glTexParameteri), used when no sampler object
  // is bound to the unit.
  GLenum min_filter = gl::kNearestMipmapLinear;
  GLenum mag_filter = gl::kLinear;
  GLenum wrap_s = gl::kRepeat;
  GLenum wrap_t = gl::kRepeat;
  GLenum wrap_r = gl::kRepeat;
  int max_level = 1000;
  std::array<GLenum, 4> swizzle = {gl::kRed, gl::kGreen, gl::kBlue, gl::kAlpha};
};

struct RenderbufferRecord {
  id<MTLTexture> texture = nil;
  MTLPixelFormat format = MTLPixelFormatInvalid;
  GLenum internal_format = 0;
  int width = 0;
  int height = 0;
  int samples = 1;
};

struct Attachment {
  enum class Kind : uint8_t { kNone, kTexture, kRenderbuffer };
  Kind kind = Kind::kNone;
  uint32_t name = 0;
  int level = 0;
  int layer = 0;
};

struct FramebufferRecord {
  Attachment color;
  Attachment depth;
  Attachment stencil;
};

struct SamplerRecord {
  GLenum min_filter = gl::kNearestMipmapLinear;
  GLenum mag_filter = gl::kLinear;
  GLenum wrap_s = gl::kRepeat;
  GLenum wrap_t = gl::kRepeat;
  GLenum wrap_r = gl::kRepeat;
  float max_anisotropy = 1.0f;
  float min_lod = -1000.0f;
  float max_lod = 1000.0f;
};

struct VertexAttribute {
  bool enabled = false;
  uint32_t buffer = 0;
  int components = 4;
  GLenum type = gl::kFloat;
  bool normalized = false;
  bool integer = false;
  int stride = 0;
  size_t offset = 0;
  int divisor = 0;
};

struct VertexArrayRecord {
  std::array<VertexAttribute, kVertexAttributeCount> attributes;
  uint32_t element_buffer = 0;
};

struct QueryRecord {
  GLenum target = 0;
  uint64_t submission = 0;
  uint64_t result_ns = 0;
  bool pending = false;
};

struct SyncRecord {
  uint64_t submission = 0;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_RESOURCES_H_
