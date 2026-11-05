#ifndef CANE_PLATFORM_METAL_CONTEXT_H_
#define CANE_PLATFORM_METAL_CONTEXT_H_

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "binding_slots.h"
#include "gl_enums.h"
#include "gl_state.h"
#include "object_table.h"
#include "pipeline_cache.h"
#include "resources.h"
#include "shader_library.h"
#include "submission_tracker.h"
#include "transient_allocator.h"

struct GLFWwindow;

namespace cane::gl_metal {

struct ShaderRecord {
  GLenum type = 0;
  std::string source;
  GlslInterface interface;
  bool compiled = false;
  std::string log;
};

struct ProgramRecord {
  uint32_t vertex_shader = 0;
  uint32_t fragment_shader = 0;
  bool linked = false;
  std::string log;
  CompiledProgram compiled;
  std::vector<uint8_t> uniforms;
  std::array<int, kTextureSlotCount> sampler_units{};
  bool uniforms_dirty = true;
};

// What a framebuffer resolves to for one render pass.
struct RenderTarget {
  id<MTLTexture> color = nil;
  id<MTLTexture> depth = nil;
  id<MTLTexture> stencil = nil;
  int width = 0;
  int height = 0;
  int samples = 1;
  bool is_default = false;
  RenderTargetKey Key() const;
};

struct PendingClear {
  bool active = false;
  uint32_t framebuffer = 0;
  bool color = false;
  bool depth = false;
  bool stencil = false;
  std::array<float, 4> color_value{};
  double depth_value = 1.0;
  int stencil_value = 0;
};

class Context {
 public:
  Context();
  ~Context();

  bool Initialize(GLFWwindow* window, std::string* error);
  void Shutdown();

  // Presentation.
  void SetSwapInterval(int interval);
  void SwapBuffers();
  void Flush();
  void Finish();

  // Errors and queries about the implementation.
  void SetError(GLenum error);
  GLenum TakeError();
  const char* GetString(GLenum name);
  const char* GetStringIndexed(GLenum name, uint32_t index);
  bool GetInteger(GLenum name, int* out);
  bool GetFloat(GLenum name, float* out);

  // Buffers.
  void GenBuffers(int count, uint32_t* names);
  void DeleteBuffers(int count, const uint32_t* names);
  void BindBuffer(GLenum target, uint32_t name);
  void BindBufferRange(GLenum target, uint32_t index, uint32_t name, size_t offset, size_t size);
  void BufferData(GLenum target, size_t size, const void* data, GLenum usage);
  void BufferSubData(GLenum target, size_t offset, size_t size, const void* data);
  void* MapBufferRange(GLenum target, size_t offset, size_t length, GLbitfield access);
  bool UnmapBuffer(GLenum target);

  // Textures, samplers, renderbuffers.
  void GenTextures(int count, uint32_t* names);
  void DeleteTextures(int count, const uint32_t* names);
  void ActiveTexture(GLenum unit);
  void BindTexture(GLenum target, uint32_t name);
  void TexImage2D(GLenum target, int level, GLenum internal_format, int width, int height,
                  GLenum format, GLenum type, const void* data);
  void TexStorage2D(GLenum target, int levels, GLenum internal_format, int width, int height);
  void TexStorage3D(GLenum target, int levels, GLenum internal_format, int width, int height,
                    int depth);
  void TexSubImage2D(GLenum target, int level, int x, int y, int width, int height, GLenum format,
                     GLenum type, const void* data);
  void TexSubImage3D(GLenum target, int level, int x, int y, int z, int width, int height,
                     int depth, GLenum format, GLenum type, const void* data);
  void TexParameteri(GLenum target, GLenum pname, int value);
  void GenerateMipmap(GLenum target);
  void PixelStorei(GLenum pname, int value);
  void GenSamplers(int count, uint32_t* names);
  void DeleteSamplers(int count, const uint32_t* names);
  void BindSampler(uint32_t unit, uint32_t name);
  void SamplerParameteri(uint32_t name, GLenum pname, int value);
  void SamplerParameterf(uint32_t name, GLenum pname, float value);
  void GenRenderbuffers(int count, uint32_t* names);
  void DeleteRenderbuffers(int count, const uint32_t* names);
  void BindRenderbuffer(GLenum target, uint32_t name);
  void RenderbufferStorage(GLenum target, int samples, GLenum internal_format, int width,
                           int height);

  // Vertex arrays.
  void GenVertexArrays(int count, uint32_t* names);
  void DeleteVertexArrays(int count, const uint32_t* names);
  void BindVertexArray(uint32_t name);
  void EnableVertexAttribArray(uint32_t index, bool enabled);
  void VertexAttribPointer(uint32_t index, int components, GLenum type, bool normalized,
                           bool integer, int stride, size_t offset);
  void VertexAttribDivisor(uint32_t index, int divisor);

  // Framebuffers.
  void GenFramebuffers(int count, uint32_t* names);
  void DeleteFramebuffers(int count, const uint32_t* names);
  void BindFramebuffer(GLenum target, uint32_t name);
  void FramebufferTexture2D(GLenum target, GLenum attachment, uint32_t texture, int level);
  void FramebufferRenderbuffer(GLenum target, GLenum attachment, uint32_t renderbuffer);
  GLenum CheckFramebufferStatus(GLenum target);
  void BlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                       GLbitfield mask, GLenum filter);
  void ReadPixels(int x, int y, int width, int height, GLenum format, GLenum type, void* data);

  // Shaders and programs.
  uint32_t CreateShader(GLenum type);
  void DeleteShader(uint32_t name);
  void ShaderSource(uint32_t name, int count, const char* const* strings, const int* lengths);
  void CompileShader(uint32_t name);
  bool GetShaderInt(uint32_t name, GLenum pname, int* out);
  void GetShaderInfoLog(uint32_t name, int max_length, int* length, char* out);
  uint32_t CreateProgram();
  void DeleteProgram(uint32_t name);
  void AttachShader(uint32_t program, uint32_t shader);
  void LinkProgram(uint32_t name);
  bool GetProgramInt(uint32_t name, GLenum pname, int* out);
  void GetProgramInfoLog(uint32_t name, int max_length, int* length, char* out);
  void UseProgram(uint32_t name);
  int GetUniformLocation(uint32_t program, const char* name);
  void UniformInts(int location, int count, const int* values);
  void UniformFloats(int location, int count, const float* values);
  void UniformMatrix4(int location, int count, bool transpose, const float* values);

  // Fixed-function state.
  void SetCapability(GLenum cap, bool enabled);
  void Viewport(int x, int y, int width, int height);
  void Scissor(int x, int y, int width, int height);
  void ClearColor(float r, float g, float b, float a);
  void ClearDepth(double depth);
  void ClearStencil(int value);
  void ColorMask(bool r, bool g, bool b, bool a);
  void DepthMask(bool enabled);
  void DepthFunc(GLenum func);
  void StencilMask(uint32_t mask);
  void StencilFunc(GLenum func, int reference, uint32_t mask);
  void StencilOpSeparate(GLenum face, GLenum fail, GLenum depth_fail, GLenum pass);
  void BlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha);
  void BlendEquationSeparate(GLenum rgb, GLenum alpha);
  void BlendColor(float r, float g, float b, float a);
  void CullFace(GLenum face);
  void FrontFace(GLenum face);
  void LineWidth(float width);
  void ClipControl(GLenum origin, GLenum depth);

  // Drawing.
  void Clear(GLbitfield mask);
  void DrawArrays(GLenum mode, int first, int count);
  void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, int count, GLenum type,
                                                   size_t indices, int instances, int base_vertex,
                                                   uint32_t base_instance);
  void MultiDrawElementsIndirect(GLenum mode, GLenum type, size_t offset, int draw_count,
                                 int stride);

  // Queries and fences.
  void GenQueries(int count, uint32_t* names);
  void DeleteQueries(int count, const uint32_t* names);
  void BeginQuery(GLenum target, uint32_t name);
  void EndQuery(GLenum target);
  bool GetQueryResult(uint32_t name, GLenum pname, uint64_t* out);
  void* FenceSync();
  void DeleteSync(void* sync);
  GLenum ClientWaitSync(void* sync, GLbitfield flags, uint64_t timeout_ns);

 private:
  struct DrawBinding {
    id<MTLRenderPipelineState> pipeline = nil;
    std::array<id<MTLBuffer>, kVertexBufferCount> vertex_buffers{};
    std::array<size_t, kVertexBufferCount> vertex_offsets{};
    int vertex_buffer_count = 0;
    bool valid = false;
  };

  // What the open render encoder has been told so far; reset per encoder.
  struct Applied {
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLDepthStencilState> depth_stencil = nil;
    MTLViewport viewport{};
    MTLScissorRect scissor{};
    MTLCullMode cull = MTLCullModeNone;
    MTLWinding winding = MTLWindingClockwise;
    int stencil_front = -1;
    int stencil_back = -1;
    std::array<float, 4> blend_color{};
    std::array<id<MTLTexture>, kTextureSlotCount> textures{};
    std::array<id<MTLSamplerState>, kTextureSlotCount> samplers{};
    std::array<id<MTLBuffer>, kVertexBufferCount> vertex_buffers{};
    std::array<size_t, kVertexBufferCount> vertex_offsets{};
    std::array<id<MTLBuffer>, kStorageBufferCount> storage_buffers{};
    std::array<size_t, kStorageBufferCount> storage_offsets{};
    ProgramRecord* uniform_program = nullptr;
    bool viewport_set = false;
    bool scissor_set = false;
    bool blend_color_set = false;
  };

  struct UniformUpload {
    id<MTLBuffer> buffer = nil;
    size_t offset = 0;
    uint64_t serial = 0;
  };

  // Lifecycle (context.mm).
  id<MTLCommandBuffer> CommandBuffer();
  uint64_t CurrentSerial() const { return tracker_.last_submitted() + 1; }
  void Submit(bool wait);
  void EndEncoders();
  void EndRenderEncoder();
  id<MTLBlitCommandEncoder> BlitEncoder();
  id<CAMetalDrawable> AcquireDrawable();
  void UpdateDrawableSize();
  bool CreatePlaceholders(std::string* error);

  // Resources (context_resources.mm).
  BufferRecord* BoundBuffer(GLenum target);
  uint32_t* BindingSlot(GLenum target);
  id<MTLBuffer> NewBuffer(size_t size);
  void RetireBuffer(id<MTLBuffer> buffer, uint64_t serial);
  bool BufferInUse(const BufferRecord& record) const;
  void OrphanBuffer(BufferRecord* record, size_t size);
  void WriteBuffer(BufferRecord* record, size_t offset, size_t size, const void* data);
  TextureRecord* BoundTexture(GLenum target);
  uint32_t BoundTextureName(GLenum target) const;
  id<MTLTexture> AllocateTexture(GLenum target, MTLPixelFormat format, int width, int height,
                                 int layers, int levels, int samples);
  bool DefineTexture(TextureRecord* record, GLenum target, GLenum internal_format, int width,
                     int height, int layers, int levels);
  void UploadTexture(TextureRecord* record, int level, int x, int y, int layer, int width,
                     int height, GLenum format, GLenum type, const void* data);
  void SetTextureParameter(TextureRecord* record, GLenum pname, int value);
  SamplerKey SamplerKeyFor(const TextureRecord& texture, const SamplerRecord* sampler) const;
  id<MTLTexture> TextureForSampling(TextureRecord* record);
  FramebufferRecord* BoundFramebuffer(GLenum target);
  Attachment* AttachmentFor(FramebufferRecord* framebuffer, GLenum attachment);
  bool ResolveTarget(uint32_t framebuffer, RenderTarget* out);
  id<MTLTexture> AttachmentTexture(const Attachment& attachment, int* samples);
  id<MTLTexture> ReadColorTexture(uint32_t framebuffer, int* width, int* height);
  void TouchBuffer(BufferRecord* record);

  // Programs (context_programs.mm).
  ProgramRecord* CurrentProgram();
  void WriteUniform(ProgramRecord* program, const UniformField& field, const void* data,
                    size_t bytes);

  // Rendering (context_render.mm).
  bool EnsureRenderEncoder(uint32_t framebuffer);
  void FlushPendingClear();
  void ResetApplied();
  float ClipFlip() const { return encoder_target_.is_default ? 1.0f : -1.0f; }
  MTLViewport ViewportFor(const Rect& rect) const;
  MTLScissorRect ScissorFor(const Rect& rect) const;
  MTLScissorRect FullScissor() const;
  void ApplyRasterState();
  bool ClearCoversTarget(GLbitfield mask, const RenderTarget& target) const;
  void ClearWithDraw(GLbitfield mask);
  bool BuildPipeline(ProgramRecord* program, const VertexArrayRecord* vao, DrawBinding* binding);
  bool PrepareDraw(ProgramRecord** program_out);
  bool BindUniforms(ProgramRecord* program);
  void BindTextures(ProgramRecord* program);
  void BindStorageBuffers(ProgramRecord* program);
  MTLVertexFormat VertexFormatFor(const VertexAttribute& attribute) const;
  void ResolveMultisample(id<MTLTexture> source, id<MTLTexture> destination);
  id<MTLTexture> ScratchResolveTexture(id<MTLTexture> like);
  void BlitWithDraw(const RenderTarget& source, bool source_default, int sx0, int sy0, int sx1,
                    int sy1, int dx0, int dy0, int dx1, int dy1, bool linear);

  GLFWwindow* window_ = nullptr;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  CAMetalLayer* layer_ = nil;
  id<CAMetalDrawable> drawable_ = nil;
  id<MTLCommandBuffer> command_buffer_ = nil;
  id<MTLRenderCommandEncoder> render_encoder_ = nil;
  id<MTLBlitCommandEncoder> blit_encoder_ = nil;
  uint32_t encoder_framebuffer_ = UINT32_MAX;
  RenderTarget encoder_target_;
  Applied applied_;
  PendingClear pending_clear_;
  id<MTLTexture> resolve_scratch_ = nil;
  std::unordered_map<ProgramRecord*, UniformUpload> uniform_uploads_;

  SubmissionTracker tracker_;
  std::unique_ptr<TransientAllocator> transient_;
  std::unique_ptr<ShaderLibrary> shaders_;
  std::unique_ptr<PipelineCache> pipelines_;
  UtilityFunctions utilities_;
  id<MTLTexture> placeholder_2d_ = nil;
  id<MTLTexture> placeholder_2d_array_ = nil;
  id<MTLBuffer> placeholder_buffer_ = nil;

  struct RetiredBuffer {
    id<MTLBuffer> buffer;
    uint64_t serial;
  };
  std::vector<RetiredBuffer> retired_buffers_;

  ObjectTable<BufferRecord> buffers_;
  ObjectTable<TextureRecord> textures_;
  ObjectTable<RenderbufferRecord> renderbuffers_;
  ObjectTable<FramebufferRecord> framebuffers_;
  ObjectTable<SamplerRecord> samplers_;
  ObjectTable<VertexArrayRecord> vertex_arrays_;
  ObjectTable<ShaderRecord> shaders_table_;
  ObjectTable<ProgramRecord> programs_;
  ObjectTable<QueryRecord> queries_;
  ObjectTable<SyncRecord> syncs_;
  uint32_t active_query_ = 0;

  GlState state_;
  GLenum error_ = gl::kNoError;
  int swap_interval_ = 1;
  int max_samples_ = 4;
  int drawable_width_ = 0;
  int drawable_height_ = 0;
  std::string renderer_name_;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_CONTEXT_H_
