// The OpenGL calls Sly 1's renderer makes beyond Sly 2's and Sly 3's (tools/sly1-metal-gl-coverage.py checks the
// table covers every one): its state mirror's queries (Sly1/Sly1-Release/glstate.cpp Sync), immutable persistently
// mapped buffers, BC textures, layered render targets, the current vertex attribute values, depth range, point size.
#include <algorithm>
#include <cstring>

#include "context.h"
#include "enum_translation.h"
#include "../switch/sly1/deko/texture_decode.h"  // (CaneDeko::DecodeBc: BC1 / BC3 blocks to RGBA8)

namespace cane::gl_metal {

bool Context::IsEnabled(GLenum cap) {
  switch (cap) {
    case gl::kBlend:
      return state_.blend.enabled;
    case gl::kCullFace:
      return state_.raster.cull;
    case gl::kDepthTest:
      return state_.depth_stencil.depth_test;
    case gl::kStencilTest:
      return state_.depth_stencil.stencil_test;
    case gl::kScissorTest:
      return state_.raster.scissor;
    case gl::kProgramPointSize:
      return true;
    default:
      return false;
  }
}

// (the state glGetIntegerv reads back beyond context.mm's GetInteger: the fixed-function state, the limits Sly 1
// checks before it builds its programs)
bool Context::GetStateInteger(GLenum name, int* out) {
  const DepthStencilState& ds = state_.depth_stencil;
  switch (name) {
    case gl::kDepthWritemask:
      *out = ds.depth_write ? 1 : 0;
      return true;
    case gl::kDepthFunc:
      *out = static_cast<int>(ds.depth_func);
      return true;
    case gl::kColorWritemask:
      for (int i = 0; i < 4; ++i) out[i] = state_.blend.write_mask[static_cast<size_t>(i)] ? 1 : 0;
      return true;
    case gl::kBlendSrcRgb:
      *out = static_cast<int>(state_.blend.src_rgb);
      return true;
    case gl::kBlendDstRgb:
      *out = static_cast<int>(state_.blend.dst_rgb);
      return true;
    case gl::kBlendSrcAlpha:
      *out = static_cast<int>(state_.blend.src_alpha);
      return true;
    case gl::kBlendDstAlpha:
      *out = static_cast<int>(state_.blend.dst_alpha);
      return true;
    case gl::kBlendEquationRgb:
      *out = static_cast<int>(state_.blend.equation_rgb);
      return true;
    case gl::kBlendEquationAlpha:
      *out = static_cast<int>(state_.blend.equation_alpha);
      return true;
    case gl::kCullFaceMode:
      *out = static_cast<int>(state_.raster.cull_face);
      return true;
    case gl::kFrontFace:
      *out = static_cast<int>(state_.raster.front_face);
      return true;
    case gl::kStencilFunc:
      *out = static_cast<int>(ds.front.func);
      return true;
    case gl::kStencilRef:
      *out = ds.front.reference;
      return true;
    case gl::kStencilValueMask:
      *out = static_cast<int>(ds.front.compare_mask);
      return true;
    case gl::kStencilFail:
      *out = static_cast<int>(ds.front.fail);
      return true;
    case gl::kStencilPassDepthFail:
      *out = static_cast<int>(ds.front.depth_fail);
      return true;
    case gl::kStencilPassDepthPass:
      *out = static_cast<int>(ds.front.pass);
      return true;
    case gl::kStencilWritemask:
      *out = static_cast<int>(ds.front.write_mask);
      return true;
    case gl::kTextureBinding2DArray:
      *out = static_cast<int>(state_.units[static_cast<size_t>(state_.active_unit)].texture_2d_array);
      return true;
    case gl::kUniformBufferBinding:
      *out = static_cast<int>(state_.uniform_buffer);
      return true;
    case gl::kShaderStorageBufferBinding:
      *out = static_cast<int>(state_.shader_storage_buffer);
      return true;
    case gl::kElementArrayBufferBinding: {
      const VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
      *out = vao ? static_cast<int>(vao->element_buffer) : 0;
      return true;
    }
    case gl::kMaxVertexShaderStorageBlocks:
    case gl::kMaxFragmentShaderStorageBlocks:
    case gl::kMaxCombinedShaderStorageBlocks:
    case gl::kMaxShaderStorageBufferBindings:
      *out = kStorageBufferCount;
      return true;
    case gl::kMaxShaderStorageBlockSize:
      *out = 1 << 30;
      return true;
    case gl::kMaxVertexOutputComponents:
    case gl::kMaxFragmentInputComponents:
      *out = 124;  // (Metal: 31 vec4 varyings on every Apple GPU family Sly 1 can run on)
      return true;
    case gl::kMaxUniformBufferBindings:
      *out = kUniformBlockCount;
      return true;
    case gl::kMaxUniformBlockSize:
      *out = 65536;
      return true;
    case gl::kMaxCombinedTextureImageUnits:
    case gl::kMaxVertexTextureImageUnits:
      *out = kTextureSlotCount;
      return true;
    case gl::kMaxDrawBuffers:
    case gl::kMaxColorAttachments:
      *out = 1;
      return true;
    case gl::kMaxViewportDims:
      out[0] = 16384;
      out[1] = 16384;
      return true;
    case gl::kMaxRenderbufferSize:
      *out = 16384;
      return true;
    case gl::kPackAlignment:
      *out = state_.pack_alignment;
      return true;
    case gl::kUnpackAlignment:
      *out = state_.unpack_alignment;
      return true;
    default:
      return false;
  }
}

bool Context::GetStateFloat(GLenum name, float* out) {
  switch (name) {
    case gl::kDepthRange:
      out[0] = state_.depth_range[0];
      out[1] = state_.depth_range[1];
      return true;
    case gl::kPointSize:
      *out = state_.point_size;
      return true;
    case gl::kAliasedPointSizeRange:
      out[0] = 1.0f;
      out[1] = 511.0f;
      return true;
    case gl::kAliasedLineWidthRange:
      out[0] = 1.0f;
      out[1] = 1.0f;
      return true;
    default:
      return false;
  }
}

void Context::DepthRange(float near_value, float far_value) {
  // (Metal's viewport has a depth range too: context_render.mm ViewportFor)
  state_.depth_range[0] = std::clamp(near_value, 0.0f, 1.0f);
  state_.depth_range[1] = std::clamp(far_value, 0.0f, 1.0f);
}

void Context::PointSize(float size) {
  // (the programs write their point size, GL_PROGRAM_POINT_SIZE; kept for the state queries)
  state_.point_size = size;
}

void Context::VertexAttrib(uint32_t index, const float* values, int count) {
  if (index >= static_cast<uint32_t>(kVertexAttributeCount)) {
    SetError(gl::kInvalidValue);
    return;
  }
  std::array<float, 4> value = {0.0f, 0.0f, 0.0f, 1.0f};
  for (int i = 0; i < std::min(count, 4); ++i) value[static_cast<size_t>(i)] = values[i];
  state_.current_attributes[index] = value;
}

void Context::BufferStorage(GLenum target, size_t size, const void* data, GLbitfield flags) {
  (void)flags;  // (every buffer is shared storage: a persistent coherent map is its contents, MapBufferRange)
  BufferRecord* record = BoundBuffer(target);
  if (!record) return;
  OrphanBuffer(record, size);
  if (data) WriteBuffer(record, 0, size, data);
}

void Context::CompressedTexSubImage2D(GLenum target, int level, int x, int y, int width, int height,
                                      GLenum format, size_t size, const void* data) {
  TextureRecord* record = BoundTexture(target);
  if (!record || !record->texture || !data) {
    SetError(gl::kInvalidOperation);
    return;
  }
  const bool bc3 = format == gl::kCompressedRgbaS3tcDxt5;
  if (format != gl::kCompressedRgbS3tcDxt1 && format != gl::kCompressedRgbaS3tcDxt1 && !bc3) {
    SetError(gl::kInvalidEnum, __func__, format);
    return;
  }
  const size_t need = static_cast<size_t>((width + 3) / 4) * static_cast<size_t>((height + 3) / 4) * (bc3 ? 16 : 8);
  if (size < need || width <= 0 || height <= 0) {
    SetError(gl::kInvalidValue);
    return;
  }
  const std::vector<uint8_t> rgba = CaneDeko::DecodeBc(static_cast<const uint8_t*>(data),
                                                       static_cast<unsigned>(width),
                                                       static_cast<unsigned>(height), bc3);
  const int alignment = state_.unpack_alignment;
  state_.unpack_alignment = 4;
  UploadTexture(record, level, x, y, 0, width, height, gl::kRgba, gl::kUnsignedByte, rgba.data());
  state_.unpack_alignment = alignment;
}

void Context::GetTexImage(GLenum target, int level, GLenum format, GLenum type, void* data) {
  // (Sly 1's debug texture dump only: not supported on Metal)
  (void)target;
  (void)level;
  (void)format;
  (void)type;
  (void)data;
  SetError(gl::kInvalidOperation);
}

void Context::GetTexLevelParameter(GLenum target, int level, GLenum pname, int* out) {
  TextureRecord* record = BoundTexture(target);
  if (!record || !record->texture) {
    *out = 0;
    return;
  }
  switch (pname) {
    case gl::kTextureWidth:
      *out = std::max(1, record->width >> level);
      return;
    case gl::kTextureHeight:
      *out = std::max(1, record->height >> level);
      return;
    case gl::kTextureDepth:
      *out = record->layers;
      return;
    case gl::kTextureInternalFormat:
      *out = static_cast<int>(record->internal_format);
      return;
    case gl::kTextureCompressed:
      *out = 0;
      return;
    default:
      SetError(gl::kInvalidEnum, __func__, pname);
      *out = 0;
      return;
  }
}

void Context::GetFramebufferAttachmentParameter(GLenum target, GLenum attachment, GLenum pname,
                                                int* out) {
  *out = 0;
  const uint32_t name =
      target == gl::kReadFramebuffer ? state_.read_framebuffer : state_.draw_framebuffer;
  if (name == 0) {
    // (the window: a colour buffer only)
    if (attachment == gl::kColor || attachment == gl::kColorAttachment0) {
      if (pname == gl::kFramebufferAttachmentObjectType) *out = static_cast<int>(gl::kFramebufferDefault);
    }
    return;
  }
  FramebufferRecord* framebuffer = framebuffers_.Find(name);
  Attachment* slot = framebuffer ? AttachmentFor(framebuffer, attachment) : nullptr;
  if (!slot) return;
  int samples = 1;
  id<MTLTexture> texture = AttachmentTexture(*slot, &samples);
  switch (pname) {
    case gl::kFramebufferAttachmentObjectType:
      *out = slot->kind == Attachment::Kind::kTexture        ? static_cast<int>(gl::kTexture)
             : slot->kind == Attachment::Kind::kRenderbuffer ? static_cast<int>(gl::kRenderbuffer)
                                                              : 0;
      return;
    case gl::kFramebufferAttachmentObjectName:
      *out = static_cast<int>(slot->name);
      return;
    case gl::kFramebufferAttachmentStencilSize:
      *out = texture && (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ||
                         texture.pixelFormat == MTLPixelFormatStencil8)
                 ? 8
                 : 0;
      return;
    case gl::kFramebufferAttachmentDepthSize:
      *out = texture && (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ||
                         texture.pixelFormat == MTLPixelFormatDepth32Float)
                 ? 32
                 : 0;
      return;
    default:
      return;
  }
}

void Context::FramebufferTextureLayer(GLenum target, GLenum attachment, uint32_t texture, int level,
                                      int layer) {
  FramebufferTexture2D(target, attachment, texture, level);
  FramebufferRecord* framebuffer = BoundFramebuffer(target);
  if (!framebuffer) return;
  if (attachment == gl::kDepthStencilAttachment) {
    framebuffer->depth.layer = layer;
    framebuffer->stencil.layer = layer;
    return;
  }
  if (Attachment* slot = AttachmentFor(framebuffer, attachment)) slot->layer = layer;
}

void Context::ClearBuffer(GLenum buffer, int draw_buffer, const float* value) {
  // (glClearBufferfv: the colour of draw buffer 0, or the depth, through the same path as glClear with the value)
  if (draw_buffer != 0 && buffer == gl::kColor) return;
  if (buffer == gl::kColor) {
    const std::array<float, 4> saved = state_.clear.color;
    state_.clear.color = {value[0], value[1], value[2], value[3]};
    Clear(gl::kColorBufferBit);
    state_.clear.color = saved;
  } else if (buffer == gl::kDepth) {
    const double saved = state_.clear.depth;
    state_.clear.depth = std::clamp(static_cast<double>(value[0]), 0.0, 1.0);
    Clear(gl::kDepthBufferBit);
    state_.clear.depth = saved;
  } else {
    SetError(gl::kInvalidEnum, __func__, buffer);
  }
}

}  // namespace cane::gl_metal
