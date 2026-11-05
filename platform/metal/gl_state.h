#ifndef CANE_PLATFORM_METAL_GL_STATE_H_
#define CANE_PLATFORM_METAL_GL_STATE_H_

#include <array>
#include <cstdint>

#include "binding_slots.h"
#include "gl_enums.h"

namespace cane::gl_metal {

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct BlendState {
  bool enabled = false;
  GLenum src_rgb = gl::kOne;
  GLenum dst_rgb = gl::kZero;
  GLenum src_alpha = gl::kOne;
  GLenum dst_alpha = gl::kZero;
  GLenum equation_rgb = gl::kFuncAdd;
  GLenum equation_alpha = gl::kFuncAdd;
  std::array<float, 4> color = {0.0f, 0.0f, 0.0f, 0.0f};
  std::array<bool, 4> write_mask = {true, true, true, true};

  bool operator==(const BlendState& o) const {
    return enabled == o.enabled && src_rgb == o.src_rgb && dst_rgb == o.dst_rgb &&
           src_alpha == o.src_alpha && dst_alpha == o.dst_alpha && equation_rgb == o.equation_rgb &&
           equation_alpha == o.equation_alpha && write_mask == o.write_mask;
  }
};

struct StencilFace {
  GLenum func = gl::kAlways;
  int reference = 0;
  uint32_t compare_mask = 0xFF;
  uint32_t write_mask = 0xFF;
  GLenum fail = gl::kKeep;
  GLenum depth_fail = gl::kKeep;
  GLenum pass = gl::kKeep;

  bool operator==(const StencilFace& o) const {
    return func == o.func && compare_mask == o.compare_mask && write_mask == o.write_mask &&
           fail == o.fail && depth_fail == o.depth_fail && pass == o.pass;
  }
};

struct DepthStencilState {
  bool depth_test = false;
  bool depth_write = true;
  GLenum depth_func = gl::kLess;
  bool stencil_test = false;
  StencilFace front;
  StencilFace back;

  bool operator==(const DepthStencilState& o) const {
    return depth_test == o.depth_test && depth_write == o.depth_write &&
           depth_func == o.depth_func && stencil_test == o.stencil_test && front == o.front &&
           back == o.back;
  }
};

struct RasterState {
  bool cull = false;
  GLenum cull_face = gl::kBack;
  GLenum front_face = gl::kCcw;
  bool scissor = false;
  Rect scissor_box;
  Rect viewport;
  float line_width = 1.0f;
};

struct ClearState {
  std::array<float, 4> color = {0.0f, 0.0f, 0.0f, 0.0f};
  double depth = 1.0;
  int stencil = 0;
};

struct TextureUnit {
  uint32_t texture_2d = 0;
  uint32_t texture_2d_array = 0;
  uint32_t sampler = 0;
};

struct BufferRange {
  uint32_t buffer = 0;
  size_t offset = 0;
  size_t size = 0;
};

struct GlState {
  BlendState blend;
  DepthStencilState depth_stencil;
  RasterState raster;
  ClearState clear;
  std::array<TextureUnit, kTextureSlotCount> units;
  int active_unit = 0;
  uint32_t program = 0;
  uint32_t vertex_array = 0;
  uint32_t array_buffer = 0;
  uint32_t copy_write_buffer = 0;
  uint32_t copy_read_buffer = 0;
  uint32_t draw_indirect_buffer = 0;
  uint32_t shader_storage_buffer = 0;
  uint32_t uniform_buffer = 0;
  std::array<BufferRange, kStorageBufferCount> storage_ranges;
  std::array<BufferRange, kStorageBufferCount> uniform_ranges;
  uint32_t draw_framebuffer = 0;
  uint32_t read_framebuffer = 0;
  uint32_t renderbuffer = 0;
  int pack_alignment = 4;
  int unpack_alignment = 4;
  bool clip_zero_to_one = false;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_GL_STATE_H_
