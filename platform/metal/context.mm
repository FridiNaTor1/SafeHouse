#include "context.h"

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
// iOS (cmake/ios.cmake): the window is SDL's (platform/ios/glfw_sdl_ios.cpp), which makes the Metal view and layer.
#include <GLFW/glfw3.h>
extern "C" void* cane_glfw_metal_layer(GLFWwindow* window);
#else
#define GLFW_EXPOSE_NATIVE_COCOA
#import <Cocoa/Cocoa.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

namespace cane::gl_metal {

RenderTargetKey RenderTarget::Key() const {
  RenderTargetKey key;
  key.color = color ? color.pixelFormat : MTLPixelFormatInvalid;
  key.depth = depth ? depth.pixelFormat : MTLPixelFormatInvalid;
  key.stencil = stencil ? stencil.pixelFormat : MTLPixelFormatInvalid;
  key.samples = static_cast<uint32_t>(samples);
  return key;
}

Context::Context() = default;

Context::~Context() { Shutdown(); }

bool Context::Initialize(GLFWwindow* window, std::string* error) {
  window_ = window;
  device_ = MTLCreateSystemDefaultDevice();
  if (!device_) {
    if (error) *error = "no Metal device";
    return false;
  }
  queue_ = [device_ newCommandQueue];
  renderer_name_ = device_.name.UTF8String;
  for (int samples : {8, 4, 2}) {
    if ([device_ supportsTextureSampleCount:samples]) {
      max_samples_ = samples;
      break;
    }
  }

#if TARGET_OS_IPHONE
  layer_ = (__bridge CAMetalLayer*)cane_glfw_metal_layer(window);
  if (!layer_) {
    if (error) *error = "the window has no Metal layer";
    return false;
  }
  layer_.device = device_;
  layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer_.framebufferOnly = NO;
  layer_.maximumDrawableCount = 3;
#else
  NSWindow* ns_window = glfwGetCocoaWindow(window);
  if (!ns_window) {
    if (error) *error = "GLFW window has no Cocoa window";
    return false;
  }
  layer_ = [CAMetalLayer layer];
  layer_.device = device_;
  layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer_.framebufferOnly = NO;
  layer_.displaySyncEnabled = YES;
  layer_.maximumDrawableCount = 2;
  NSView* view = ns_window.contentView;
  layer_.contentsScale = ns_window.backingScaleFactor;
  view.layer = layer_;
  view.wantsLayer = YES;
#endif
  UpdateDrawableSize();

  transient_ = std::make_unique<TransientAllocator>(device_);
  shaders_ = std::make_unique<ShaderLibrary>(device_);
  pipelines_ = std::make_unique<PipelineCache>(device_);
  std::string log;
  if (!shaders_->CompileUtilities(&utilities_, &log)) {
    if (error) *error = "Metal utility shaders failed: " + log;
    return false;
  }
  return CreatePlaceholders(error);
}

bool Context::CreatePlaceholders(std::string* error) {
  const uint32_t black = 0xFF000000u;
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  placeholder_2d_ = [device_ newTextureWithDescriptor:descriptor];
  [placeholder_2d_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                     mipmapLevel:0
                       withBytes:&black
                     bytesPerRow:4];
  descriptor.textureType = MTLTextureType2DArray;
  descriptor.arrayLength = 1;
  placeholder_2d_array_ = [device_ newTextureWithDescriptor:descriptor];
  [placeholder_2d_array_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                           mipmapLevel:0
                                 slice:0
                             withBytes:&black
                           bytesPerRow:4
                         bytesPerImage:4];
  placeholder_buffer_ = [device_ newBufferWithLength:256 options:MTLResourceStorageModeShared];
  std::memset(placeholder_buffer_.contents, 0, 256);
  if (!placeholder_2d_ || !placeholder_2d_array_ || !placeholder_buffer_) {
    if (error) *error = "Metal placeholder resources failed";
    return false;
  }
  return true;
}

void Context::Shutdown() {
  if (!device_) return;
  EndEncoders();
  if (command_buffer_) Submit(true);
  tracker_.WaitAll();
  drawable_ = nil;
  layer_ = nil;
  pipelines_.reset();
  shaders_.reset();
  transient_.reset();
  retired_buffers_.clear();
  queue_ = nil;
  device_ = nil;
  window_ = nullptr;
}

void Context::UpdateDrawableSize() {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  if (width <= 0 || height <= 0) return;
  if (width != drawable_width_ || height != drawable_height_) {
    drawable_width_ = width;
    drawable_height_ = height;
    layer_.drawableSize = CGSizeMake(width, height);
  }
}

id<CAMetalDrawable> Context::AcquireDrawable() {
  if (drawable_) return drawable_;
  UpdateDrawableSize();
  drawable_ = [layer_ nextDrawable];
  return drawable_;
}

id<MTLCommandBuffer> Context::CommandBuffer() {
  if (!command_buffer_) {
    command_buffer_ = [queue_ commandBuffer];
    transient_->Recycle(tracker_.last_completed());
  }
  return command_buffer_;
}

void Context::EndRenderEncoder() {
  if (render_encoder_) {
    [render_encoder_ endEncoding];
    render_encoder_ = nil;
  }
  encoder_framebuffer_ = UINT32_MAX;
  ResetApplied();
}

void Context::EndEncoders() {
  EndRenderEncoder();
  if (blit_encoder_) {
    [blit_encoder_ endEncoding];
    blit_encoder_ = nil;
  }
}

id<MTLBlitCommandEncoder> Context::BlitEncoder() {
  EndRenderEncoder();
  if (!blit_encoder_) blit_encoder_ = [CommandBuffer() blitCommandEncoder];
  return blit_encoder_;
}

void Context::Submit(bool wait) {
  FlushPendingClear();
  EndEncoders();
  if (!command_buffer_) {
    if (wait) tracker_.WaitAll();
    return;
  }
  const uint64_t serial = tracker_.Register(command_buffer_);
  transient_->EndSubmission(serial);
  [command_buffer_ commit];
  if (wait) [command_buffer_ waitUntilCompleted];
  command_buffer_ = nil;
}

void Context::Flush() { Submit(false); }

void Context::Finish() { Submit(true); }

void Context::SetSwapInterval(int interval) {
  swap_interval_ = interval;
#if TARGET_OS_IPHONE
  // (iOS always presents in step with the display; three drawables keep the GPU fed)
  layer_.maximumDrawableCount = 3;
#else
  layer_.displaySyncEnabled = interval != 0;
  layer_.maximumDrawableCount = interval != 0 ? 2 : 3;
#endif
}

void Context::SwapBuffers() {
  FlushPendingClear();
  EndEncoders();
  if (drawable_) {
    [CommandBuffer() presentDrawable:drawable_];
    drawable_ = nil;
  }
  Submit(false);
}

void Context::SetError(GLenum error, const char* where, GLenum value) {
  if (error_ == gl::kNoError) error_ = error;
  if (!where) return;
  static std::mutex mutex;
  static std::set<std::pair<std::string, GLenum>> reported;
  std::lock_guard<std::mutex> lock(mutex);
  if (reported.size() < 64 && reported.emplace(where, value).second)
    std::fprintf(stderr, "Metal: GL_INVALID_ENUM in %s: 0x%04X (not translated)\n", where, unsigned(value));
}

GLenum Context::TakeError() {
  const GLenum error = error_;
  error_ = gl::kNoError;
  return error;
}

const char* Context::GetString(GLenum name) {
  switch (name) {
    case gl::kVendor:
      return "Apple";
    case gl::kRenderer:
      return renderer_name_.c_str();
    case gl::kVersion:
      return "4.5 Metal";
    case gl::kShadingLanguageVersion:
      return "4.50";
    case gl::kExtensions:
      return "GL_ARB_clip_control";
    default:
      SetError(gl::kInvalidEnum, __func__, name);
      return nullptr;
  }
}

const char* Context::GetStringIndexed(GLenum name, uint32_t index) {
  if (name != gl::kExtensions || index != 0) {
    SetError(gl::kInvalidValue);
    return nullptr;
  }
  return "GL_ARB_clip_control";
}

bool Context::GetInteger(GLenum name, int* out) {
  switch (name) {
    case gl::kMajorVersion:
      *out = 4;
      return true;
    case gl::kMinorVersion:
      *out = 5;
      return true;
    case gl::kNumExtensions:
      *out = 1;
      return true;
    case gl::kMaxTextureSize:
      *out = 16384;
      return true;
    case gl::kMaxSamples:
      *out = max_samples_;
      return true;
    case gl::kMaxArrayTextureLayers:
      *out = 2048;
      return true;
    case gl::kMaxTextureImageUnits:
      *out = kTextureSlotCount;
      return true;
    case gl::kMaxVertexAttribs:
      *out = kVertexAttributeCount;
      return true;
    case gl::kShaderStorageBufferOffsetAlignment:
    case gl::kUniformBufferOffsetAlignment:
      *out = 256;
      return true;
    case gl::kFramebufferBinding:
      *out = static_cast<int>(state_.draw_framebuffer);
      return true;
    case gl::kReadFramebufferBinding:
      *out = static_cast<int>(state_.read_framebuffer);
      return true;
    case gl::kCurrentProgram:
      *out = static_cast<int>(state_.program);
      return true;
    case gl::kVertexArrayBinding:
      *out = static_cast<int>(state_.vertex_array);
      return true;
    case gl::kArrayBufferBinding:
      *out = static_cast<int>(state_.array_buffer);
      return true;
    case gl::kActiveTexture:
      *out = static_cast<int>(gl::kTexture0 + state_.active_unit);
      return true;
    case gl::kTextureBinding2D:
      *out = static_cast<int>(state_.units[state_.active_unit].texture_2d);
      return true;
    case gl::kViewport:
      out[0] = state_.raster.viewport.x;
      out[1] = state_.raster.viewport.y;
      out[2] = state_.raster.viewport.width;
      out[3] = state_.raster.viewport.height;
      return true;
    case gl::kScissorBox:
      out[0] = state_.raster.scissor_box.x;
      out[1] = state_.raster.scissor_box.y;
      out[2] = state_.raster.scissor_box.width;
      out[3] = state_.raster.scissor_box.height;
      return true;
    default:
      return false;
  }
}

bool Context::GetFloat(GLenum name, float* out) {
  if (name == gl::kMaxTextureMaxAnisotropy) {
    *out = 16.0f;
    return true;
  }
  int value = 0;
  if (!GetInteger(name, &value)) return false;
  *out = static_cast<float>(value);
  return true;
}

void Context::GenQueries(int count, uint32_t* names) {
  for (int i = 0; i < count; ++i) names[i] = queries_.Create();
}

void Context::DeleteQueries(int count, const uint32_t* names) {
  for (int i = 0; i < count; ++i) {
    if (names[i] == active_query_) active_query_ = 0;
    queries_.Destroy(names[i]);
  }
}

void Context::BeginQuery(GLenum target, uint32_t name) {
  QueryRecord* query = queries_.Find(name);
  if (!query || target != gl::kTimeElapsed) {
    SetError(gl::kInvalidOperation);
    return;
  }
  query->target = target;
  query->submission = CurrentSerial();
  query->pending = true;
  query->result_ns = 0;
  active_query_ = name;
}

void Context::EndQuery(GLenum target) {
  (void)target;
  QueryRecord* query = queries_.Find(active_query_);
  active_query_ = 0;
  if (!query) return;
  // The span covers every command buffer from the begin serial to the current
  // one; the result is their summed GPU time once they complete.
  query->result_ns = CurrentSerial();
}

bool Context::GetQueryResult(uint32_t name, GLenum pname, uint64_t* out) {
  QueryRecord* query = queries_.Find(name);
  if (!query) return false;
  const uint64_t first = query->submission;
  const uint64_t last = query->pending ? query->result_ns : first;
  if (pname == gl::kQueryResultAvailable) {
    *out = tracker_.IsComplete(last) ? 1 : 0;
    return true;
  }
  if (!tracker_.IsComplete(last)) {
    if (last >= CurrentSerial()) Submit(false);
    tracker_.WaitFor(last, 1000000000ull);
  }
  uint64_t total = 0;
  for (uint64_t serial = first; serial <= last; ++serial) {
    total += tracker_.GpuDurationNs(serial);
  }
  query->pending = false;
  query->result_ns = total;
  *out = total;
  return true;
}

void* Context::FenceSync() {
  const uint32_t name = syncs_.Create();
  syncs_.Find(name)->submission = CurrentSerial();
  return reinterpret_cast<void*>(static_cast<uintptr_t>(name));
}

void Context::DeleteSync(void* sync) {
  syncs_.Destroy(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sync)));
}

GLenum Context::ClientWaitSync(void* sync, GLbitfield flags, uint64_t timeout_ns) {
  SyncRecord* record = syncs_.Find(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sync)));
  if (!record) return gl::kWaitFailed;
  if (tracker_.IsComplete(record->submission)) return gl::kAlreadySignaled;
  if (record->submission >= CurrentSerial()) {
    if (!(flags & gl::kSyncFlushCommandsBit) && timeout_ns == 0) return gl::kTimeoutExpired;
    Submit(false);
  }
  return tracker_.WaitFor(record->submission, timeout_ns) ? gl::kConditionSatisfied
                                                          : gl::kTimeoutExpired;
}

}  // namespace cane::gl_metal
