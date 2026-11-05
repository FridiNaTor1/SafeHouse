#ifndef CANE_PLATFORM_METAL_PIPELINE_CACHE_H_
#define CANE_PLATFORM_METAL_PIPELINE_CACHE_H_

#import <Metal/Metal.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "binding_slots.h"
#include "gl_state.h"
#include "resources.h"

namespace cane::gl_metal {

struct VertexAttributeKey {
  bool enabled = false;
  MTLVertexFormat format = MTLVertexFormatInvalid;
  uint32_t offset = 0;
  uint32_t buffer_index = 0;
};

struct VertexBufferKey {
  uint32_t stride = 0;
  MTLVertexStepFunction step = MTLVertexStepFunctionPerVertex;
  uint32_t step_rate = 1;
};

struct RenderTargetKey {
  MTLPixelFormat color = MTLPixelFormatInvalid;
  MTLPixelFormat depth = MTLPixelFormatInvalid;
  MTLPixelFormat stencil = MTLPixelFormatInvalid;
  uint32_t samples = 1;
};

struct PipelineKey {
  uint32_t program = 0;
  std::array<VertexAttributeKey, kVertexAttributeCount> attributes;
  std::array<VertexBufferKey, kVertexBufferCount> buffers;
  RenderTargetKey target;
  BlendState blend;
  std::string Hash() const;
};

struct DepthStencilKey {
  DepthStencilState state;
  bool has_depth = false;
  bool has_stencil = false;
  std::string Hash() const;
};

struct SamplerKey {
  GLenum min_filter = gl::kLinear;
  GLenum mag_filter = gl::kLinear;
  GLenum wrap_s = gl::kRepeat;
  GLenum wrap_t = gl::kRepeat;
  GLenum wrap_r = gl::kRepeat;
  float max_anisotropy = 1.0f;
  bool mipmapped = true;
  std::string Hash() const;
};

class PipelineCache {
 public:
  explicit PipelineCache(id<MTLDevice> device);

  id<MTLRenderPipelineState> Pipeline(const PipelineKey& key, id<MTLFunction> vertex,
                                      id<MTLFunction> fragment, std::string* error);
  id<MTLDepthStencilState> DepthStencil(const DepthStencilKey& key);
  id<MTLSamplerState> Sampler(const SamplerKey& key);

  void ForgetProgram(uint32_t program);

 private:
  id<MTLDevice> device_;
  std::unordered_map<std::string, id<MTLRenderPipelineState>> pipelines_;
  std::unordered_map<std::string, id<MTLDepthStencilState>> depth_stencils_;
  std::unordered_map<std::string, id<MTLSamplerState>> samplers_;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_PIPELINE_CACHE_H_
