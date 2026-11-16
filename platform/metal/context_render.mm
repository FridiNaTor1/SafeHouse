#include <algorithm>
#include <cstring>

#include "context.h"
#include "enum_translation.h"

namespace cane::gl_metal {

namespace {

constexpr uint32_t kClearProgramId = 0xFFFFFFFEu;
constexpr uint32_t kBlitProgramId = 0xFFFFFFFDu;
constexpr GLbitfield kAllClearBits =
    gl::kColorBufferBit | gl::kDepthBufferBit | gl::kStencilBufferBit;
// The largest storage-buffer element the shaders declare (GlobRec).
constexpr size_t kMinStorageBytes = 160;

struct ClearParams {
  float color[4];
  float depth;
  float pad[3];
};

struct BlitParams {
  float dst_ndc[4];
  float src_uv[4];
};

bool SameViewport(const MTLViewport& a, const MTLViewport& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool SameScissor(const MTLScissorRect& a, const MTLScissorRect& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Fixed-function state

void Context::SetCapability(GLenum cap, bool enabled) {
  switch (cap) {
    case gl::kBlend:
      state_.blend.enabled = enabled;
      break;
    case gl::kCullFace:
      state_.raster.cull = enabled;
      break;
    case gl::kDepthTest:
      state_.depth_stencil.depth_test = enabled;
      break;
    case gl::kStencilTest:
      state_.depth_stencil.stencil_test = enabled;
      break;
    case gl::kScissorTest:
      state_.raster.scissor = enabled;
      break;
    case gl::kDepthClamp:
    case gl::kMultisample:
    case gl::kFramebufferSrgb:
    case gl::kPrimitiveRestart:
    case gl::kPolygonOffsetFill:
    case gl::kLineSmooth:
    case gl::kDither:
      break;
    default:
      SetError(gl::kInvalidEnum, __func__, cap);
      break;
  }
}

void Context::Viewport(int x, int y, int width, int height) {
  state_.raster.viewport = {x, y, width, height};
}

void Context::Scissor(int x, int y, int width, int height) {
  state_.raster.scissor_box = {x, y, width, height};
}

void Context::ClearColor(float r, float g, float b, float a) { state_.clear.color = {r, g, b, a}; }

void Context::ClearDepth(double depth) { state_.clear.depth = std::clamp(depth, 0.0, 1.0); }

void Context::ClearStencil(int value) { state_.clear.stencil = value; }

void Context::ColorMask(bool r, bool g, bool b, bool a) { state_.blend.write_mask = {r, g, b, a}; }

void Context::DepthMask(bool enabled) { state_.depth_stencil.depth_write = enabled; }

void Context::DepthFunc(GLenum func) { state_.depth_stencil.depth_func = func; }

void Context::StencilMask(uint32_t mask) {
  state_.depth_stencil.front.write_mask = mask & 0xFF;
  state_.depth_stencil.back.write_mask = mask & 0xFF;
}

void Context::StencilFunc(GLenum func, int reference, uint32_t mask) {
  for (StencilFace* face : {&state_.depth_stencil.front, &state_.depth_stencil.back}) {
    face->func = func;
    face->reference = reference & 0xFF;
    face->compare_mask = mask & 0xFF;
  }
}

void Context::StencilOpSeparate(GLenum face, GLenum fail, GLenum depth_fail, GLenum pass) {
  if (face == gl::kFront || face == gl::kFrontAndBack) {
    state_.depth_stencil.front.fail = fail;
    state_.depth_stencil.front.depth_fail = depth_fail;
    state_.depth_stencil.front.pass = pass;
  }
  if (face == gl::kBack || face == gl::kFrontAndBack) {
    state_.depth_stencil.back.fail = fail;
    state_.depth_stencil.back.depth_fail = depth_fail;
    state_.depth_stencil.back.pass = pass;
  }
}

void Context::BlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha,
                                GLenum dst_alpha) {
  state_.blend.src_rgb = src_rgb;
  state_.blend.dst_rgb = dst_rgb;
  state_.blend.src_alpha = src_alpha;
  state_.blend.dst_alpha = dst_alpha;
}

void Context::BlendEquationSeparate(GLenum rgb, GLenum alpha) {
  state_.blend.equation_rgb = rgb;
  state_.blend.equation_alpha = alpha;
}

void Context::BlendColor(float r, float g, float b, float a) { state_.blend.color = {r, g, b, a}; }

void Context::CullFace(GLenum face) { state_.raster.cull_face = face; }

void Context::FrontFace(GLenum face) { state_.raster.front_face = face; }

void Context::LineWidth(float width) { state_.raster.line_width = width; }

void Context::ClipControl(GLenum origin, GLenum depth) {
  (void)origin;
  state_.clip_zero_to_one = depth == gl::kZeroToOne;
}

// ---------------------------------------------------------------------------
// Render encoder lifecycle

void Context::ResetApplied() { applied_ = Applied{}; }

void Context::FlushPendingClear() {
  if (!pending_clear_.active) return;
  const uint32_t framebuffer = pending_clear_.framebuffer;
  if (render_encoder_ && encoder_framebuffer_ == framebuffer) {
    pending_clear_.active = false;
    return;
  }
  if (EnsureRenderEncoder(framebuffer)) EndRenderEncoder();
  pending_clear_.active = false;
}

bool Context::EnsureRenderEncoder(uint32_t framebuffer) {
  if (render_encoder_ && encoder_framebuffer_ == framebuffer) return true;
  if (pending_clear_.active && pending_clear_.framebuffer != framebuffer) FlushPendingClear();
  EndEncoders();
  RenderTarget target;
  if (!ResolveTarget(framebuffer, &target)) {
    SetError(gl::kInvalidFramebufferOperation);
    return false;
  }
  const bool clear = pending_clear_.active && pending_clear_.framebuffer == framebuffer;
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  if (target.color) {
    MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
    color.texture = target.color;
    color.storeAction = MTLStoreActionStore;
    if (clear && pending_clear_.color) {
      color.loadAction = MTLLoadActionClear;
      const std::array<float, 4>& c = pending_clear_.color_value;
      color.clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
    } else {
      color.loadAction = MTLLoadActionLoad;
    }
  }
  if (target.depth) {
    pass.depthAttachment.texture = target.depth;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    if (clear && pending_clear_.depth) {
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.clearDepth = pending_clear_.depth_value;
    } else {
      pass.depthAttachment.loadAction = MTLLoadActionLoad;
    }
  }
  if (target.stencil) {
    pass.stencilAttachment.texture = target.stencil;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    if (clear && pending_clear_.stencil) {
      pass.stencilAttachment.loadAction = MTLLoadActionClear;
      pass.stencilAttachment.clearStencil = static_cast<uint32_t>(pending_clear_.stencil_value);
    } else {
      pass.stencilAttachment.loadAction = MTLLoadActionLoad;
    }
  }
  pending_clear_.active = false;
  render_encoder_ = [CommandBuffer() renderCommandEncoderWithDescriptor:pass];
  encoder_framebuffer_ = framebuffer;
  encoder_target_ = target;
  ResetApplied();
  return render_encoder_ != nil;
}

MTLViewport Context::ViewportFor(const Rect& rect) const {
  MTLViewport viewport;
  viewport.originX = rect.x;
  viewport.width = rect.width;
  viewport.height = rect.height;
  viewport.originY =
      encoder_target_.is_default ? encoder_target_.height - rect.y - rect.height : rect.y;
  viewport.znear = 0.0;
  viewport.zfar = 1.0;
  return viewport;
}

MTLScissorRect Context::FullScissor() const {
  return {0, 0, static_cast<NSUInteger>(encoder_target_.width),
          static_cast<NSUInteger>(encoder_target_.height)};
}

MTLScissorRect Context::ScissorFor(const Rect& rect) const {
  const int width = encoder_target_.width;
  const int height = encoder_target_.height;
  int x0 = std::clamp(rect.x, 0, width);
  int x1 = std::clamp(rect.x + rect.width, 0, width);
  int gl_y0 = rect.y;
  int gl_y1 = rect.y + rect.height;
  int y0 = encoder_target_.is_default ? height - gl_y1 : gl_y0;
  int y1 = encoder_target_.is_default ? height - gl_y0 : gl_y1;
  y0 = std::clamp(y0, 0, height);
  y1 = std::clamp(y1, 0, height);
  return {static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0),
          static_cast<NSUInteger>(std::max(x1 - x0, 0)),
          static_cast<NSUInteger>(std::max(y1 - y0, 0))};
}

void Context::ApplyRasterState() {
  const MTLViewport viewport = ViewportFor(state_.raster.viewport);
  if (!applied_.viewport_set || !SameViewport(viewport, applied_.viewport)) {
    [render_encoder_ setViewport:viewport];
    applied_.viewport = viewport;
    applied_.viewport_set = true;
  }
  const MTLScissorRect scissor =
      state_.raster.scissor ? ScissorFor(state_.raster.scissor_box) : FullScissor();
  if (!applied_.scissor_set || !SameScissor(scissor, applied_.scissor)) {
    [render_encoder_ setScissorRect:scissor];
    applied_.scissor = scissor;
    applied_.scissor_set = true;
  }
  const MTLCullMode cull =
      state_.raster.cull ? ToCullMode(state_.raster.cull_face) : MTLCullModeNone;
  if (cull != applied_.cull) {
    [render_encoder_ setCullMode:cull];
    applied_.cull = cull;
  }
  // Offscreen targets are drawn with clip y mirrored, which inverts winding.
  MTLWinding winding = ToWinding(state_.raster.front_face);
  if (!encoder_target_.is_default) {
    winding = winding == MTLWindingClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise;
  }
  if (winding != applied_.winding) {
    [render_encoder_ setFrontFacingWinding:winding];
    applied_.winding = winding;
  }
  DepthStencilKey key;
  key.state = state_.depth_stencil;
  key.has_depth = encoder_target_.depth != nil;
  key.has_stencil = encoder_target_.stencil != nil;
  id<MTLDepthStencilState> depth_stencil = pipelines_->DepthStencil(key);
  if (depth_stencil != applied_.depth_stencil) {
    [render_encoder_ setDepthStencilState:depth_stencil];
    applied_.depth_stencil = depth_stencil;
  }
  const int front = state_.depth_stencil.front.reference;
  const int back = state_.depth_stencil.back.reference;
  if (front != applied_.stencil_front || back != applied_.stencil_back) {
    [render_encoder_ setStencilFrontReferenceValue:static_cast<uint32_t>(front)
                                backReferenceValue:static_cast<uint32_t>(back)];
    applied_.stencil_front = front;
    applied_.stencil_back = back;
  }
  if (!applied_.blend_color_set || state_.blend.color != applied_.blend_color) {
    const std::array<float, 4>& c = state_.blend.color;
    [render_encoder_ setBlendColorRed:c[0] green:c[1] blue:c[2] alpha:c[3]];
    applied_.blend_color = c;
    applied_.blend_color_set = true;
  }
}

// ---------------------------------------------------------------------------
// Clears

bool Context::ClearCoversTarget(GLbitfield mask, const RenderTarget& target) const {
  if (state_.raster.scissor) {
    const Rect& box = state_.raster.scissor_box;
    if (box.x > 0 || box.y > 0 || box.x + box.width < target.width ||
        box.y + box.height < target.height) {
      return false;
    }
  }
  if ((mask & gl::kColorBufferBit) && target.color) {
    for (bool write : state_.blend.write_mask) {
      if (!write) return false;
    }
  }
  if ((mask & gl::kDepthBufferBit) && target.depth && !state_.depth_stencil.depth_write) {
    return false;
  }
  if ((mask & gl::kStencilBufferBit) && target.stencil &&
      (state_.depth_stencil.front.write_mask & 0xFF) != 0xFF) {
    return false;
  }
  return true;
}

void Context::Clear(GLbitfield mask) {
  mask &= kAllClearBits;
  if (!mask) return;
  const uint32_t framebuffer = state_.draw_framebuffer;
  RenderTarget target;
  if (!ResolveTarget(framebuffer, &target)) {
    SetError(gl::kInvalidFramebufferOperation);
    return;
  }
  const bool fresh = !(render_encoder_ && encoder_framebuffer_ == framebuffer);
  if (fresh && ClearCoversTarget(mask, target)) {
    if (pending_clear_.active && pending_clear_.framebuffer != framebuffer) FlushPendingClear();
    pending_clear_.active = true;
    pending_clear_.framebuffer = framebuffer;
    if ((mask & gl::kColorBufferBit) && target.color) {
      pending_clear_.color = true;
      pending_clear_.color_value = state_.clear.color;
    }
    if ((mask & gl::kDepthBufferBit) && target.depth) {
      pending_clear_.depth = true;
      pending_clear_.depth_value = state_.clear.depth;
    }
    if ((mask & gl::kStencilBufferBit) && target.stencil) {
      pending_clear_.stencil = true;
      pending_clear_.stencil_value = state_.clear.stencil;
    }
    return;
  }
  if (!EnsureRenderEncoder(framebuffer)) return;
  ClearWithDraw(mask);
}

void Context::ClearWithDraw(GLbitfield mask) {
  const RenderTarget& target = encoder_target_;
  const bool clear_color = (mask & gl::kColorBufferBit) && target.color;
  const bool clear_depth =
      (mask & gl::kDepthBufferBit) && target.depth && state_.depth_stencil.depth_write;
  const bool clear_stencil = (mask & gl::kStencilBufferBit) && target.stencil &&
                             (state_.depth_stencil.front.write_mask & 0xFF) != 0;
  if (!clear_color && !clear_depth && !clear_stencil) return;

  PipelineKey key;
  key.program = kClearProgramId;
  key.target = target.Key();
  key.blend.enabled = false;
  key.blend.write_mask =
      clear_color ? state_.blend.write_mask : std::array<bool, 4>{false, false, false, false};
  std::string error;
  id<MTLRenderPipelineState> pipeline =
      pipelines_->Pipeline(key, utilities_.clear_vertex, utilities_.clear_fragment, &error);
  if (!pipeline) {
    SetError(gl::kInvalidOperation);
    return;
  }
  DepthStencilKey ds;
  ds.has_depth = target.depth != nil;
  ds.has_stencil = target.stencil != nil;
  ds.state.depth_test = clear_depth;
  ds.state.depth_write = clear_depth;
  ds.state.depth_func = gl::kAlways;
  ds.state.stencil_test = clear_stencil;
  for (StencilFace* face : {&ds.state.front, &ds.state.back}) {
    face->func = gl::kAlways;
    face->compare_mask = 0xFF;
    face->write_mask = state_.depth_stencil.front.write_mask;
    face->fail = gl::kReplace;
    face->depth_fail = gl::kReplace;
    face->pass = gl::kReplace;
  }
  ClearParams params{};
  std::copy(state_.clear.color.begin(), state_.clear.color.end(), params.color);
  params.depth = static_cast<float>(state_.clear.depth);
  const TransientAllocation upload = transient_->Allocate(sizeof(params), 256);
  std::memcpy(upload.cpu, &params, sizeof(params));

  [render_encoder_ setRenderPipelineState:pipeline];
  [render_encoder_ setDepthStencilState:pipelines_->DepthStencil(ds)];
  [render_encoder_ setStencilReferenceValue:static_cast<uint32_t>(state_.clear.stencil & 0xFF)];
  [render_encoder_ setCullMode:MTLCullModeNone];
  [render_encoder_ setViewport:ViewportFor({0, 0, target.width, target.height})];
  [render_encoder_
      setScissorRect:state_.raster.scissor ? ScissorFor(state_.raster.scissor_box) : FullScissor()];
  [render_encoder_ setVertexBuffer:upload.buffer offset:upload.offset atIndex:kUniformBufferSlot];
  [render_encoder_ setFragmentBuffer:upload.buffer offset:upload.offset atIndex:kUniformBufferSlot];
  [render_encoder_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  ResetApplied();
}

// ---------------------------------------------------------------------------
// Blits and readback

id<MTLTexture> Context::ScratchResolveTexture(id<MTLTexture> like) {
  if (resolve_scratch_ && resolve_scratch_.width == like.width &&
      resolve_scratch_.height == like.height && resolve_scratch_.pixelFormat == like.pixelFormat) {
    return resolve_scratch_;
  }
  resolve_scratch_ = AllocateTexture(gl::kTexture2D, like.pixelFormat, static_cast<int>(like.width),
                                     static_cast<int>(like.height), 1, 1, 1);
  return resolve_scratch_;
}

void Context::ResolveMultisample(id<MTLTexture> source, id<MTLTexture> destination) {
  FlushPendingClear();
  EndEncoders();
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = source;
  pass.colorAttachments[0].resolveTexture = destination;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
  id<MTLRenderCommandEncoder> encoder = [CommandBuffer() renderCommandEncoderWithDescriptor:pass];
  [encoder endEncoding];
}

void Context::BlitWithDraw(const RenderTarget& source, bool source_default, int sx0, int sy0,
                           int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, bool linear) {
  const RenderTarget& target = encoder_target_;
  PipelineKey key;
  key.program = kBlitProgramId;
  key.target = target.Key();
  key.blend.enabled = false;
  std::string error;
  id<MTLRenderPipelineState> pipeline =
      pipelines_->Pipeline(key, utilities_.blit_vertex, utilities_.blit_fragment, &error);
  if (!pipeline) {
    SetError(gl::kInvalidOperation);
    return;
  }
  const float flip = ClipFlip();
  BlitParams params;
  params.dst_ndc[0] = static_cast<float>(dx0) / static_cast<float>(target.width) * 2.0f - 1.0f;
  params.dst_ndc[1] =
      (static_cast<float>(dy0) / static_cast<float>(target.height) * 2.0f - 1.0f) * flip;
  params.dst_ndc[2] = static_cast<float>(dx1) / static_cast<float>(target.width) * 2.0f - 1.0f;
  params.dst_ndc[3] =
      (static_cast<float>(dy1) / static_cast<float>(target.height) * 2.0f - 1.0f) * flip;
  const float sw = static_cast<float>(source.width);
  const float sh = static_cast<float>(source.height);
  params.src_uv[0] = static_cast<float>(sx0) / sw;
  params.src_uv[2] = static_cast<float>(sx1) / sw;
  if (source_default) {
    params.src_uv[1] = 1.0f - static_cast<float>(sy0) / sh;
    params.src_uv[3] = 1.0f - static_cast<float>(sy1) / sh;
  } else {
    params.src_uv[1] = static_cast<float>(sy0) / sh;
    params.src_uv[3] = static_cast<float>(sy1) / sh;
  }
  const TransientAllocation upload = transient_->Allocate(sizeof(params), 256);
  std::memcpy(upload.cpu, &params, sizeof(params));

  SamplerKey sampler;
  sampler.min_filter = linear ? gl::kLinear : gl::kNearest;
  sampler.mag_filter = sampler.min_filter;
  sampler.wrap_s = gl::kClampToEdge;
  sampler.wrap_t = gl::kClampToEdge;
  sampler.mipmapped = false;

  DepthStencilKey ds;
  ds.has_depth = target.depth != nil;
  ds.has_stencil = target.stencil != nil;
  [render_encoder_ setRenderPipelineState:pipeline];
  [render_encoder_ setDepthStencilState:pipelines_->DepthStencil(ds)];
  [render_encoder_ setCullMode:MTLCullModeNone];
  [render_encoder_ setViewport:ViewportFor({0, 0, target.width, target.height})];
  [render_encoder_
      setScissorRect:state_.raster.scissor ? ScissorFor(state_.raster.scissor_box) : FullScissor()];
  [render_encoder_ setVertexBuffer:upload.buffer offset:upload.offset atIndex:kUniformBufferSlot];
  [render_encoder_ setFragmentTexture:source.color atIndex:0];
  [render_encoder_ setFragmentSamplerState:pipelines_->Sampler(sampler) atIndex:0];
  [render_encoder_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  ResetApplied();
}

void Context::BlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1,
                              int dy1, GLbitfield mask, GLenum filter) {
  if (!(mask & gl::kColorBufferBit)) return;
  const uint32_t read = state_.read_framebuffer;
  const uint32_t draw = state_.draw_framebuffer;
  if (read == draw) {
    SetError(gl::kInvalidOperation);
    return;
  }
  RenderTarget source;
  RenderTarget destination;
  if (!ResolveTarget(read, &source) || !ResolveTarget(draw, &destination) || !source.color ||
      !destination.color) {
    SetError(gl::kInvalidFramebufferOperation);
    return;
  }
  const bool whole_source = sx0 == 0 && sy0 == 0 && sx1 == source.width && sy1 == source.height;
  const bool same_rect = sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0;
  if (source.samples > 1) {
    if (whole_source && same_rect && dx0 == 0 && dy0 == 0 && destination.samples == 1 &&
        destination.width == source.width && destination.height == source.height &&
        destination.color.pixelFormat == source.color.pixelFormat && !destination.is_default) {
      if (pending_clear_.active && pending_clear_.framebuffer == draw) FlushPendingClear();
      ResolveMultisample(source.color, destination.color);
      return;
    }
    id<MTLTexture> scratch = ScratchResolveTexture(source.color);
    ResolveMultisample(source.color, scratch);
    source.color = scratch;
    source.samples = 1;
  }
  const bool copyable = same_rect && !source.is_default && !destination.is_default &&
                        source.color.pixelFormat == destination.color.pixelFormat &&
                        !state_.raster.scissor;
  if (copyable) {
    if (pending_clear_.active && pending_clear_.framebuffer == draw) FlushPendingClear();
    [BlitEncoder() copyFromTexture:source.color
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(sx0),
                                                 static_cast<NSUInteger>(sy0), 0)
                        sourceSize:MTLSizeMake(static_cast<NSUInteger>(sx1 - sx0),
                                               static_cast<NSUInteger>(sy1 - sy0), 1)
                         toTexture:destination.color
                  destinationSlice:0
                  destinationLevel:0
                 destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(dx0),
                                                 static_cast<NSUInteger>(dy0), 0)];
    return;
  }
  if (!EnsureRenderEncoder(draw)) return;
  BlitWithDraw(source, source.is_default, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
               filter == gl::kLinear);
}

void Context::ReadPixels(int x, int y, int width, int height, GLenum format, GLenum type,
                         void* data) {
  if (format != gl::kRgba || type != gl::kUnsignedByte || width <= 0 || height <= 0) {
    SetError(gl::kInvalidEnum, __func__, format != gl::kRgba ? format : type);
    return;
  }
  RenderTarget source;
  if (!ResolveTarget(state_.read_framebuffer, &source) || !source.color || source.is_default ||
      source.samples > 1) {
    SetError(gl::kInvalidFramebufferOperation);
    return;
  }
  if (pending_clear_.active && pending_clear_.framebuffer == state_.read_framebuffer) {
    FlushPendingClear();
  }
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  const size_t total = row_bytes * static_cast<size_t>(height);
  id<MTLBuffer> staging = [device_ newBufferWithLength:total options:MTLResourceStorageModeShared];
  [BlitEncoder() copyFromTexture:source.color
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(x),
                                               static_cast<NSUInteger>(y), 0)
                      sourceSize:MTLSizeMake(static_cast<NSUInteger>(width),
                                             static_cast<NSUInteger>(height), 1)
                        toBuffer:staging
               destinationOffset:0
          destinationBytesPerRow:row_bytes
        destinationBytesPerImage:total];
  Submit(true);
  const size_t alignment = static_cast<size_t>(std::max(state_.pack_alignment, 1));
  const size_t client_row = (row_bytes + alignment - 1) / alignment * alignment;
  const uint8_t* src = static_cast<const uint8_t*>(staging.contents);
  uint8_t* dst = static_cast<uint8_t*>(data);
  const bool bgra = source.color.pixelFormat == MTLPixelFormatBGRA8Unorm;
  for (int row = 0; row < height; ++row) {
    uint8_t* out = dst + static_cast<size_t>(row) * client_row;
    const uint8_t* in = src + static_cast<size_t>(row) * row_bytes;
    std::memcpy(out, in, row_bytes);
    if (bgra) {
      for (size_t i = 0; i < row_bytes; i += 4) std::swap(out[i], out[i + 2]);
    }
  }
}

// ---------------------------------------------------------------------------
// Draws

MTLVertexFormat Context::VertexFormatFor(const VertexAttribute& attribute) const {
  const int n = attribute.components;
  switch (attribute.type) {
    case gl::kFloat:
      return static_cast<MTLVertexFormat>(MTLVertexFormatFloat + (n - 1));
    case gl::kUnsignedByte:
      if (attribute.normalized) {
        return n == 4
                   ? MTLVertexFormatUChar4Normalized
                   : (n == 2 ? MTLVertexFormatUChar2Normalized : MTLVertexFormatUChar3Normalized);
      }
      return n == 4 ? MTLVertexFormatUChar4
                    : (n == 2 ? MTLVertexFormatUChar2 : MTLVertexFormatUChar3);
    case gl::kByte:
      if (attribute.normalized) {
        return n == 4 ? MTLVertexFormatChar4Normalized
                      : (n == 2 ? MTLVertexFormatChar2Normalized : MTLVertexFormatChar3Normalized);
      }
      return n == 4 ? MTLVertexFormatChar4 : (n == 2 ? MTLVertexFormatChar2 : MTLVertexFormatChar3);
    case gl::kUnsignedShort:
      if (attribute.normalized) {
        return n == 4
                   ? MTLVertexFormatUShort4Normalized
                   : (n == 2 ? MTLVertexFormatUShort2Normalized : MTLVertexFormatUShort3Normalized);
      }
      return n == 4 ? MTLVertexFormatUShort4
                    : (n == 2 ? MTLVertexFormatUShort2 : MTLVertexFormatUShort3);
    case gl::kUnsignedInt:
      return static_cast<MTLVertexFormat>(MTLVertexFormatUInt + (n - 1));
    case gl::kInt:
      return static_cast<MTLVertexFormat>(MTLVertexFormatInt + (n - 1));
    default:
      return MTLVertexFormatInvalid;
  }
}

bool Context::BuildPipeline(ProgramRecord* program, const VertexArrayRecord* vao,
                            DrawBinding* binding) {
  PipelineKey key;
  key.program = state_.program;
  key.target = encoder_target_.Key();
  key.blend = state_.blend;
  binding->vertex_buffer_count = 0;
  struct Slot {
    uint32_t buffer;
    int stride;
    int divisor;
    size_t base;
  };
  std::array<Slot, kVertexBufferCount> slots{};
  if (vao) {
    for (int i = 0; i < kVertexAttributeCount; ++i) {
      const VertexAttribute& attribute = vao->attributes[i];
      if (!attribute.enabled || attribute.buffer == 0) continue;
      BufferRecord* record = buffers_.Find(attribute.buffer);
      if (!record || !record->buffer) continue;
      const size_t stride = static_cast<size_t>(std::max(attribute.stride, 1));
      const size_t base = attribute.offset / stride * stride;
      int slot = -1;
      for (int s = 0; s < binding->vertex_buffer_count; ++s) {
        if (slots[s].buffer == attribute.buffer && slots[s].stride == attribute.stride &&
            slots[s].divisor == attribute.divisor && slots[s].base == base) {
          slot = s;
          break;
        }
      }
      if (slot < 0) {
        if (binding->vertex_buffer_count >= kVertexBufferCount) {
          SetError(gl::kInvalidOperation);
          return false;
        }
        slot = binding->vertex_buffer_count++;
        slots[slot] = {attribute.buffer, attribute.stride, attribute.divisor, base};
        binding->vertex_buffers[slot] = record->buffer;
        binding->vertex_offsets[slot] = base;
        key.buffers[slot].stride = static_cast<uint32_t>(attribute.stride);
        key.buffers[slot].step = attribute.divisor > 0 ? MTLVertexStepFunctionPerInstance
                                                       : MTLVertexStepFunctionPerVertex;
        key.buffers[slot].step_rate =
            attribute.divisor > 0 ? static_cast<uint32_t>(attribute.divisor) : 1;
        TouchBuffer(record);
      }
      key.attributes[i].enabled = true;
      key.attributes[i].format = VertexFormatFor(attribute);
      key.attributes[i].offset = static_cast<uint32_t>(attribute.offset - base);
      key.attributes[i].buffer_index = static_cast<uint32_t>(slot);
    }
  }
  std::string error;
  binding->pipeline =
      pipelines_->Pipeline(key, program->compiled.vertex, program->compiled.fragment, &error);
  if (!binding->pipeline) {
    SetError(gl::kInvalidOperation);
    return false;
  }
  binding->valid = true;
  return true;
}

bool Context::BindUniforms(ProgramRecord* program) {
  float clip[4] = {ClipFlip(), state_.clip_zero_to_one ? 1.0f : 0.5f,
                   state_.clip_zero_to_one ? 0.0f : 0.5f, 0.0f};
  if (std::memcmp(program->uniforms.data(), clip, sizeof(clip)) != 0) {
    std::memcpy(program->uniforms.data(), clip, sizeof(clip));
    program->uniforms_dirty = true;
  }
  UniformUpload& upload = uniform_uploads_[program];
  if (program->uniforms_dirty || upload.serial != CurrentSerial() || !upload.buffer) {
    const TransientAllocation allocation = transient_->Allocate(program->uniforms.size(), 256);
    std::memcpy(allocation.cpu, program->uniforms.data(), program->uniforms.size());
    upload.buffer = allocation.buffer;
    upload.offset = allocation.offset;
    upload.serial = CurrentSerial();
    program->uniforms_dirty = false;
    applied_.uniform_program = nullptr;
  }
  if (applied_.uniform_program != program) {
    [render_encoder_ setVertexBuffer:upload.buffer offset:upload.offset atIndex:kUniformBufferSlot];
    [render_encoder_ setFragmentBuffer:upload.buffer
                                offset:upload.offset
                               atIndex:kUniformBufferSlot];
    applied_.uniform_program = program;
  }
  return true;
}

void Context::BindTextures(ProgramRecord* program) {
  for (const UniformField& field : program->compiled.layout.fields) {
    if (field.texture_slot < 0) continue;
    const int unit =
        std::clamp(program->sampler_units[field.texture_slot], 0, kTextureSlotCount - 1);
    const TextureUnit& texture_unit = state_.units[unit];
    const bool array = field.type == GlslType::kSampler2DArray;
    TextureRecord* record =
        textures_.Find(array ? texture_unit.texture_2d_array : texture_unit.texture_2d);
    id<MTLTexture> texture = TextureForSampling(record);
    if (!texture) texture = array ? placeholder_2d_array_ : placeholder_2d_;
    SamplerKey key;
    if (record && record->texture) {
      key = SamplerKeyFor(*record, samplers_.Find(texture_unit.sampler));
    } else {
      key.min_filter = gl::kNearest;
      key.mag_filter = gl::kNearest;
      key.mipmapped = false;
    }
    id<MTLSamplerState> sampler = pipelines_->Sampler(key);
    const size_t slot = static_cast<size_t>(field.texture_slot);
    if (applied_.textures[slot] != texture) {
      [render_encoder_ setFragmentTexture:texture atIndex:slot];
      applied_.textures[slot] = texture;
    }
    if (applied_.samplers[slot] != sampler) {
      [render_encoder_ setFragmentSamplerState:sampler atIndex:slot];
      applied_.samplers[slot] = sampler;
    }
  }
}

void Context::BindStorageBuffers(ProgramRecord* program) {
  for (int binding : program->compiled.storage_bindings) {
    if (binding < 0 || binding >= kStorageBufferCount) continue;
    const BufferRange& range = state_.storage_ranges[static_cast<size_t>(binding)];
    BufferRecord* record = buffers_.Find(range.buffer);
    // Empty arrays arrive as tiny ranges; Metal needs room for one element,
    // so those read the zeroed placeholder instead.
    const bool usable =
        record && record->buffer && record->buffer.length >= range.offset + kMinStorageBytes;
    id<MTLBuffer> buffer = usable ? record->buffer : placeholder_buffer_;
    const size_t offset = usable ? range.offset : 0;
    if (usable) TouchBuffer(record);
    const size_t slot = static_cast<size_t>(binding);
    if (applied_.storage_buffers[slot] != buffer || applied_.storage_offsets[slot] != offset) {
      [render_encoder_ setVertexBuffer:buffer
                                offset:offset
                               atIndex:static_cast<NSUInteger>(kStorageBufferBase + binding)];
      applied_.storage_buffers[slot] = buffer;
      applied_.storage_offsets[slot] = offset;
    }
  }
}

bool Context::PrepareDraw(ProgramRecord** program_out) {
  ProgramRecord* program = CurrentProgram();
  if (!program) return false;
  if (!EnsureRenderEncoder(state_.draw_framebuffer)) return false;
  const VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
  DrawBinding binding;
  if (!BuildPipeline(program, vao, &binding)) return false;
  if (binding.pipeline != applied_.pipeline) {
    [render_encoder_ setRenderPipelineState:binding.pipeline];
    applied_.pipeline = binding.pipeline;
  }
  for (int slot = 0; slot < binding.vertex_buffer_count; ++slot) {
    const size_t s = static_cast<size_t>(slot);
    if (applied_.vertex_buffers[s] != binding.vertex_buffers[s] ||
        applied_.vertex_offsets[s] != binding.vertex_offsets[s]) {
      [render_encoder_ setVertexBuffer:binding.vertex_buffers[s]
                                offset:binding.vertex_offsets[s]
                               atIndex:s];
      applied_.vertex_buffers[s] = binding.vertex_buffers[s];
      applied_.vertex_offsets[s] = binding.vertex_offsets[s];
    }
  }
  ApplyRasterState();
  BindUniforms(program);
  BindTextures(program);
  BindStorageBuffers(program);
  *program_out = program;
  return true;
}

void Context::DrawArrays(GLenum mode, int first, int count) {
  if (count <= 0) return;
  ProgramRecord* program = nullptr;
  if (!PrepareDraw(&program)) return;
  [render_encoder_ drawPrimitives:ToPrimitiveType(mode)
                      vertexStart:static_cast<NSUInteger>(first)
                      vertexCount:static_cast<NSUInteger>(count)];
}

void Context::DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, int count, GLenum type,
                                                          size_t indices, int instances,
                                                          int base_vertex, uint32_t base_instance) {
  if (count <= 0 || instances <= 0) return;
  const VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
  BufferRecord* elements = vao ? buffers_.Find(vao->element_buffer) : nullptr;
  if (!elements || !elements->buffer) {
    SetError(gl::kInvalidOperation);
    return;
  }
  ProgramRecord* program = nullptr;
  if (!PrepareDraw(&program)) return;
  TouchBuffer(elements);
  [render_encoder_ drawIndexedPrimitives:ToPrimitiveType(mode)
                              indexCount:static_cast<NSUInteger>(count)
                               indexType:ToIndexType(type)
                             indexBuffer:elements->buffer
                       indexBufferOffset:indices
                           instanceCount:static_cast<NSUInteger>(instances)
                              baseVertex:base_vertex
                            baseInstance:base_instance];
}

void Context::MultiDrawElementsIndirect(GLenum mode, GLenum type, size_t offset, int draw_count,
                                        int stride) {
  if (draw_count <= 0) return;
  const VertexArrayRecord* vao = vertex_arrays_.Find(state_.vertex_array);
  BufferRecord* elements = vao ? buffers_.Find(vao->element_buffer) : nullptr;
  BufferRecord* indirect = buffers_.Find(state_.draw_indirect_buffer);
  if (!elements || !elements->buffer || !indirect || !indirect->buffer) {
    SetError(gl::kInvalidOperation);
    return;
  }
  ProgramRecord* program = nullptr;
  if (!PrepareDraw(&program)) return;
  TouchBuffer(elements);
  TouchBuffer(indirect);
  const size_t command_stride = stride > 0 ? static_cast<size_t>(stride) : 20;
  const MTLPrimitiveType primitive = ToPrimitiveType(mode);
  const MTLIndexType index_type = ToIndexType(type);
  for (int i = 0; i < draw_count; ++i) {
    [render_encoder_ drawIndexedPrimitives:primitive
                                 indexType:index_type
                               indexBuffer:elements->buffer
                         indexBufferOffset:0
                            indirectBuffer:indirect->buffer
                      indirectBufferOffset:offset + static_cast<size_t>(i) * command_stride];
  }
}

}  // namespace cane::gl_metal
