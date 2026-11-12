#ifndef CANE_PLATFORM_DRAW_PACKET_H_
#define CANE_PLATFORM_DRAW_PACKET_H_
// Draw packets: a renderer's draws handed to the backend as decided state instead of OpenGL-style calls
// (Sly2-Release/notes/VKNATIVE_2026-11-11.md).
//
// The game's renderer describes each distinct draw state once (State: the fixed-function part as backend-neutral enums,
// the program and its loose constants); the backend turns it into its own object at that moment (Vulkan: the dynamic
// state values, the pipeline key part and the constant block's bytes, platform/vulkan/gl_vulkan.cpp) and returns a
// small id. Each draw then names that id and its ranges: no per-call translation of GL state.
//
// The backend's OpenGL-style state afterwards is what the equivalent GL calls would have left (the program bound, the
// fields State sets, the vertex array and indirect buffer named), so OpenGL-style calls before and after a packet keep
// working unchanged: a renderer can move its paths to packets one by one.
//
// A backend offers packets through cane::gl::Backend::packets (gl_backend.h); nullptr (OpenGL, GLES, the Switch's
// deko3d and the Metal layer today): the renderer issues its GL calls as before. deko3d / Metal could adopt the same
// table: State maps one to one onto a DkBlendState / DkDepthStencilState / DkRasterizerState + a uniform block, and onto
// an MTLRenderPipelineState (blend, colour mask) + MTLDepthStencilState + setCullMode + setVertexBytes.
#include <cstddef>
#include <cstdint>

namespace cane::draw {

enum Factor : uint8_t {
  kFactorZero, kFactorOne, kFactorSrcAlpha, kFactorOneMinusSrcAlpha, kFactorDstAlpha, kFactorOneMinusDstAlpha,
  kFactorConstantAlpha, kFactorOneMinusConstantAlpha, kFactorSrc1Alpha, kFactorOneMinusSrc1Alpha,
};
enum BlendOp : uint8_t { kBlendAdd, kBlendReverseSubtract };
enum Compare : uint8_t { kNever, kLess, kEqual, kLessEqual, kGreater, kNotEqual, kGreaterEqual, kAlways };
enum StencilOp : uint8_t { kStencilKeep, kStencilZero, kStencilReplace, kStencilIncrWrap, kStencilDecrWrap };

// A loose constant of the program by its uniform location (32 bits: an int, uint or float; for a sampler its unit).
struct Constant {
  int32_t location;
  uint32_t bits;
};

struct State {
  uint32_t program = 0;          // the backend's program object (the GL program name)
  // blend: when blend is 0 the factors are left as they were; fBlendConstant: the blend constant is set too
  uint8_t blend = 0, srcColor = kFactorOne, dstColor = kFactorZero, srcAlpha = kFactorOne, dstAlpha = kFactorZero;
  uint8_t opColor = kBlendAdd, opAlpha = kBlendAdd, fBlendConstant = 0;
  float blendConstant[4] = {};
  uint8_t colorMask = 0xF;       // r 1, g 2, b 4, a 8
  uint8_t cull = 0;              // 0 none (the winding left as it was), 1 back faces culled, 2 front faces culled; frontCCW the
                                 // front winding
  uint8_t frontCCW = 1;
  uint8_t depthTest = 0, depthWrite = 0, depthCompare = kAlways;
  uint8_t stencilTest = 0, stencilCompare = kAlways, stencilRef = 0, stencilCompareMask = 0xFF, stencilWriteMask = 0xFF;
  uint8_t stencilFail = kStencilKeep, stencilDepthFail = kStencilKeep, stencilPass = kStencilKeep;
  uint8_t cconst = 0;
  Constant aconst[10] = {};
  // the back faces' stencil operations when stencilBack (else the front's above for both)
  uint8_t stencilBack = 0, stencilBackFail = kStencilKeep, stencilBackDepthFail = kStencilKeep, stencilBackPass = kStencilKeep;
  // 1: the front winding (frontCCW) set without culling too (two-sided stencil operations tell the faces by it)
  uint8_t winding = 0;
  // (added with the shadow phases) kKeep* bits: fields of the state left as they were (the GL calls do not set them)
  uint8_t keep = 0;
};
enum : uint8_t {
  kKeepDepthCompare = 1,   // the depth compare
  kKeepStencil = 2,        // the stencil compare, reference, compare / write masks and operations (both faces)
  kKeepBlendFactors = 4,   // with blend on: the factors and operations
  kKeepColorMask = 8,      // (added with the screen draws) the colour write mask
  kKeepDepthWrite = 16,    // the depth write mask
};

// A render pass begun by the renderer: GL's glBindFramebuffer(GL_FRAMEBUFFER, framebuffer) + glViewport(viewport) and,
// when clear has bits, glColorMask(all) / glDepthMask(true) / glStencilMask(0xFF), glClearColor / Depth / Stencil and one
// glClear of those bits (the backend: its attachments' load operations when the pass starts there).
struct Pass {
  uint32_t framebuffer = 0;      // the backend's framebuffer object (the GL name; 0 the window)
  int32_t viewport[4] = {};
  uint8_t clear = 0;             // kClear* bits
  uint8_t stencil = 0;
  float color[4] = {};
  float depth = 0.0f;
  uint8_t cullOff = 0;           // (added with the shadow phases) 1: glDisable(GL_CULL_FACE) too
};
enum : uint8_t { kClearColor = 1, kClearDepth = 2, kClearStencil = 4 };

// A clear inside the current pass (added with the shadow phases): GL's glColorMask / glDepthMask / glStencilMask and the
// scissor (glEnable / glDisable(GL_SCISSOR_TEST), glScissor) as given for it, glClearColor / Depth / Stencil of the
// cleared buffers, one glClear of those bits (each buffer as its write mask and the scissor rectangle allow), then the
// masks and the scissor as the GL calls leave them afterwards (the *After fields). kMaskKept in colorMask / depthWrite /
// scissor: left as it is; in a *After field: as during the clear.
constexpr uint8_t kMaskKept = 0xFF;
struct Clear {
  uint8_t clear = 0;             // kClear* bits
  uint8_t colorMask = kMaskKept, depthWrite = kMaskKept, stencilWriteMask = 0xFF;
  uint8_t scissor = kMaskKept;   // 1: the scissor test on with rect, 0 off
  int32_t rect[4] = {};
  float color[4] = {};
  float depth = 0.0f;
  uint8_t stencil = 0;
  uint8_t colorMaskAfter = kMaskKept, depthWriteAfter = kMaskKept, stencilWriteMaskAfter = 0xFF, scissorAfter = kMaskKept;
  uint8_t stencilMaskKept = 0;   // (added with the screen draws) 1: the stencil write mask left as it is (both fields ignored)
};
constexpr uint32_t kSamplerKept = 0xFFFFFFFFu;

// (added with the HUD) a draw's primitive (GL_TRIANGLES / GL_TRIANGLE_STRIP / GL_LINES) and a texture's own sampling
// parameters (glTexParameteri's GL_TEXTURE_WRAP_S / _T: GL_REPEAT / GL_CLAMP_TO_EDGE / GL_MIRRORED_REPEAT; the filters:
// GL_NEAREST, GL_LINEAR, then the GL_*_MIPMAP_* ones in GL's order)
enum Primitive : uint8_t { kTriangles, kTriangleStrip, kLines };
enum Wrap : uint8_t { kWrapRepeat, kWrapClampToEdge, kWrapMirroredRepeat };
enum Filter : uint8_t {
  kFilterNearest, kFilterLinear, kFilterNearestMipNearest, kFilterLinearMipNearest, kFilterNearestMipLinear, kFilterLinearMipLinear,
};

// (added with the resources) a texture made by createTexture: RGBA8 texels, a 2D texture (layers 0) or a 2D array of
// `layers` layers, `levels` mip levels (at most the full chain). storage 1: GL's glTexStorage2D / 3D (immutable levels);
// 0: glTexImage2D's (a 2D texture of one level; generateMipmaps then makes the chain anew, as GL does). The texture's own
// sampling parameters as glTexParameteri sets them (GL's defaults below). pixels: level 0's texels (all layers, layer
// after layer) as tightly packed RGBA8 rows, row 0 first; null: zeros.
struct TextureDesc {
  uint32_t width = 0, height = 0;
  uint32_t layers = 0;
  uint32_t levels = 1;
  uint8_t storage = 1;
  uint8_t wrapS = kWrapRepeat, wrapT = kWrapRepeat, minFilter = kFilterNearestMipLinear, magFilter = kFilterLinear;
  int32_t maxLevel = 1000;
  const void* pixels = nullptr;
};

// (added with the buffers) a buffer's use as glBufferData's usage hint: GL_STATIC_DRAW, GL_DYNAMIC_DRAW, GL_STREAM_DRAW
// (this frame's data only: a backend may place it in per-frame memory); kBufferIndirect or-ed in: see createBuffer
enum BufferUsage : uint8_t { kBufferStatic, kBufferDynamic, kBufferStream };
enum : uint8_t { kBufferIndirect = 0x10 };

// (added with the vertex arrays) one enabled attribute of a vertex array: GL's glVertexAttribPointer(index, components,
// type, normalized, stride, offset) (integer 1: glVertexAttribIPointer) reading buffer, glVertexAttribDivisor(divisor);
// stride 0: tightly packed (components x the type's size).
enum AttribType : uint8_t { kAttribFloat, kAttribUnsignedByte, kAttribUnsignedInt };
struct VertexAttrib {
  uint8_t index = 0, components = 4, type = kAttribFloat, normalized = 0, integer = 0;
  uint32_t buffer = 0, offset = 0, stride = 0, divisor = 0;
};

// (added with the framebuffers) a renderbuffer: its size, its format (GL's GL_RGBA8, GL_DEPTH32F_STENCIL8,
// GL_DEPTH24_STENCIL8), samples (1: glRenderbufferStorage, more: glRenderbufferStorageMultisample's), layers (more than
// 1: a layered image for multiview passes, the Vulkan layer's SetNextRenderbufferLayers), transient (its contents are
// never needed after a pass: SetRenderbufferTransient's hint).
enum RenderbufferFormat : uint8_t { kRenderbufferRgba8, kRenderbufferDepth32fStencil8, kRenderbufferDepth24Stencil8 };
struct RenderbufferDesc {
  uint32_t width = 0, height = 0;
  uint8_t format = kRenderbufferRgba8, samples = 1, layers = 1, transient = 0;
};
// A framebuffer attachment: a texture's level / layer (glFramebufferTexture2D: layer 0; glFramebufferTextureLayer) or a
// renderbuffer (renderbuffer 1); views 2: layers layer and layer + 1 as a pass's two views (multiview: the Vulkan layer's
// FramebufferMultiview); object 0: none.
struct Attachment {
  uint32_t object = 0;
  uint8_t renderbuffer = 0, level = 0;
  uint16_t layer = 0;
  uint8_t views = 0;
};

// (added with the samplers) a sampler object's parameters as glSamplerParameteri sets them (Wrap, Filter; GL's
// defaults below), anisotropy as glSamplerParameterf(GL_TEXTURE_MAX_ANISOTROPY) when above 1, clamped to the device's
// maximum (GL_MAX_TEXTURE_MAX_ANISOTROPY; a device without anisotropic filtering: not set).
struct SamplerDesc {
  uint8_t wrapS = kWrapRepeat, wrapT = kWrapRepeat, minFilter = kFilterNearestMipLinear, magFilter = kFilterLinear;
  float anisotropy = 1.0f;
};

struct Packets {
  // The backend's id for a state (0: it cannot take this one: the renderer draws it with GL calls).
  uint32_t (*createState)(const State& state);
  // GL's glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, offset, count, 0) with the state applied, the vertex
  // array vao and the indirect buffer bound (the storage buffers, textures and viewport as bound).
  void (*drawIndirect)(uint32_t state, uint32_t vao, uint32_t indirectBuffer, size_t offset, uint32_t count);
  // GL's glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, count, GL_UNSIGNED_INT, firstIndex x 4, 1, baseVertex,
  // baseInstance) with the state applied, then the draw's own constants (cconst of them: per-draw values such as a
  // vertex base, which stay set afterwards as glUniform* would leave them) and the vertex array vao bound.
  void (*drawIndexed)(uint32_t state, uint32_t vao, uint32_t count, uint32_t firstIndex, int32_t baseVertex, uint32_t baseInstance,
                      const Constant* aconst, uint32_t cconst);
  // (added with the render pass specials; each may be null in a backend that has only the above)
  // The pass begun (see Pass).
  void (*beginPass)(const Pass& pass);
  // The state applied without a draw (the GL-style state then as the state's GL calls leave it).
  void (*applyState)(uint32_t state);
  // GL's glDrawArrays(GL_TRIANGLES, first, count) with the state applied, the draw's constants and vao bound.
  void (*drawArrays)(uint32_t state, uint32_t vao, uint32_t first, uint32_t count, const Constant* aconst, uint32_t cconst);
  // A wider constant of program by its uniform location (glProgramUniform*: a vec2..4, a mat4 as its four columns, or
  // an array of vec4): bytes of it from data, in the block's layout (a vector's components, 16 bytes per column / element;
  // a sampler uniform: its unit (an int), as glProgramUniform1i.
  void (*setConstant)(uint32_t program, int32_t location, const void* data, uint32_t bytes);
  // (added with the shadow phases; each may be null)
  // A clear in the current pass (see Clear).
  void (*clear)(const Clear& clear);
  // GL's glBindFramebuffer(GL_READ_FRAMEBUFFER, src) + (GL_DRAW_FRAMEBUFFER, dst), glBlitFramebuffer of the colour of
  // rect (x, y, width, height) to the same size at (dstX, dstY), GL_NEAREST, then glBindFramebuffer(GL_FRAMEBUFFER,
  // framebufferAfter).
  void (*copy)(uint32_t src, uint32_t dst, const int32_t rect[4], int32_t dstX, int32_t dstY, uint32_t framebufferAfter);
  // rect null: glDisable(GL_SCISSOR_TEST); else glEnable(GL_SCISSOR_TEST) + glScissor(rect: x, y, width, height).
  void (*scissor)(const int32_t* rect);
  // GL's glActiveTexture(GL_TEXTURE0 + unit), glBindTexture(array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, texture),
  // glBindSampler(unit, sampler) unless sampler is kSamplerKept, then glActiveTexture(GL_TEXTURE0).
  void (*bindTexture)(uint32_t unit, uint32_t texture, uint8_t array, uint32_t sampler);
  // GL's glBindBufferRange(GL_SHADER_STORAGE_BUFFER, binding, buffer, offset, size) (size 0: glBindBufferBase, the
  // whole buffer).
  void (*bindStorage)(uint32_t binding, uint32_t buffer, size_t offset, size_t size);
  // (added with the uploads; may be null) GL's glBufferData(size, nullptr, GL_STREAM_DRAW) on buffer (the GL binding
  // points left as they are): memory of size bytes that is its new contents, written by the caller before the next
  // packet or GL call that reads the buffer (the backend: this frame's ring memory, no copy). Null: the backend cannot
  // here (the GL calls then; nothing changed).
  void* (*mapStream)(uint32_t buffer, size_t size);
  // (added with the in-place builders; may be null) GL's glBufferSubData(offset, size, data) on buffer (the GL binding
  // points left as they are): memory of size bytes that is the range's new contents, written in full by the caller (in
  // order; it may be write-combined: never read back) before the next packet or GL call. The backend keeps its own
  // rules for a range something already recorded reads (Vulkan: in place while nothing does, else a copy on write or a
  // transfer). Null: the backend cannot here (the GL calls then; nothing changed).
  void* (*mapRange)(uint32_t buffer, size_t offset, size_t size);
  // (added with the pass's last GL calls; each may be null) copy's blit with layer `layer` of the 2D array texture
  // `texture` as its source (read through a framebuffer of the backend's own: no framebuffer object of the caller's
  // changes but the bindings: framebufferAfter bound afterwards).
  void (*copyLayer)(uint32_t texture, uint32_t layer, uint32_t dst, const int32_t rect[4], int32_t dstX, int32_t dstY, uint32_t framebufferAfter);
  // GL's glBindFramebuffer(GL_FRAMEBUFFER, framebuffer): the target of the draws after it (the viewport left as it is).
  void (*bindTarget)(uint32_t framebuffer);
  // (added with the HUD; each may be null) drawArrays with the primitive given (Primitive); lineWidth > 0: GL's
  // glLineWidth(lineWidth) first (it stays set afterwards), 0: the line width left as it is.
  void (*drawArraysMode)(uint32_t state, uint32_t vao, uint8_t primitive, uint32_t first, uint32_t count, float lineWidth,
                         const Constant* aconst, uint32_t cconst);
  // GL's glTexParameteri of the 2D texture's wrap S / T (Wrap), minifying / magnifying filters (Filter): the texture's
  // own parameters (what a draw samples it with while no sampler object is bound to its unit); no binding changes.
  void (*textureParams)(uint32_t texture, uint8_t wrapS, uint8_t wrapT, uint8_t minFilter, uint8_t magFilter);
  // (added with the present; each may be null) copy's blit scaled: the colour of srcRect (x, y, width, height) of src
  // into dstRect of dst, linear 1: GL_LINEAR, else GL_NEAREST; then glBindFramebuffer(GL_FRAMEBUFFER, framebufferAfter).
  void (*blit)(uint32_t src, uint32_t dst, const int32_t srcRect[4], const int32_t dstRect[4], uint8_t linear, uint32_t framebufferAfter);
  // GL's glBindFramebuffer(GL_FRAMEBUFFER, framebuffer) + glInvalidateFramebuffer of the attachments in mask (kClear*
  // bits: kClearColor colour attachment 0, kClearDepth | kClearStencil the depth-stencil attachment): their contents not
  // needed any more (the backend may skip storing them).
  void (*invalidate)(uint32_t framebuffer, uint8_t mask);
  // (added with the resources; each may be null: the renderer's GL calls then) Resources made and changed without any
  // GL binding point changing (no texture unit, buffer or framebuffer binding is touched, nor the unpack alignment):
  // GL's glGenTextures + glBindTexture + glTexStorage* / glTexImage2D + glTexParameteri of desc + the level-0 upload, as
  // a new texture name (the same names as glGenTextures', usable by every GL call and packet); 0: it could not.
  uint32_t (*createTexture)(const TextureDesc& desc);
  // GL's glTexSubImage2D / 3D of the rectangle (x, y, width, height) of level `level`, layer `layer` (0 for a 2D
  // texture) from tightly packed RGBA8 rows (row 0 first).
  void (*uploadTexture)(uint32_t texture, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t width, uint32_t height,
                        const void* rgba);
  // GL's glGenerateMipmap of the texture.
  void (*generateMipmaps)(uint32_t texture);
  // GL's glDeleteTextures (a bound texture unbound, an attached one detached, as GL does).
  void (*deleteTextures)(uint32_t count, const uint32_t* textures);
  // (added with the last leftovers; each may be null) GL's glUseProgram(program); 0: no program bound.
  void (*useProgram)(uint32_t program);
  // GL's glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer) + glReadPixels of rect (x, y, width, height) of its colour
  // as tightly packed RGBA8 rows (row 0 = the bottom, as GL's), waiting for the GPU, then glBindFramebuffer(GL_FRAMEBUFFER,
  // framebufferAfter).
  void (*readPixels)(uint32_t framebuffer, const int32_t rect[4], void* rgba, uint32_t framebufferAfter);
  // GPU timers (GL's GL_TIME_ELAPSED queries): createTimer = glGenQueries(1); beginTimer / endTimer = glBeginQuery /
  // glEndQuery(GL_TIME_ELAPSED); timerResult = glGetQueryObjectui64v(GL_QUERY_RESULT) (waits): nanoseconds.
  uint32_t (*createTimer)();
  void (*beginTimer)(uint32_t timer);
  void (*endTimer)();
  uint64_t (*timerResult)(uint32_t timer);
  // (added with the buffers; each may be null: the renderer's GL calls then) Buffers made and filled without any GL
  // binding point changing (no glBindBuffer of any target, the vertex array's element buffer included), unlike the GL
  // calls they replace. usage: BufferUsage, | kBufferIndirect for a buffer the draws read indirect commands from (GL's
  // GL_DRAW_INDIRECT_BUFFER target of the glBufferData: a backend may keep a CPU copy of those).
  // GL's glGenBuffers, then with size > 0 glBufferData(size, data, usage) on it (data null: contents undefined, to be
  // written: mapRange / bufferSubData); a new buffer name usable by every GL call and packet; 0: it could not.
  uint32_t (*createBuffer)(size_t size, const void* data, uint8_t usage);
  // GL's glBufferData(size, data, usage) on the buffer (new storage; data null: undefined contents).
  void (*bufferData)(uint32_t buffer, size_t size, const void* data, uint8_t usage);
  // GL's glBufferSubData(offset, size, data) on the buffer.
  void (*bufferSubData)(uint32_t buffer, size_t offset, size_t size, const void* data);
  // GL's glDeleteBuffers (a bound buffer unbound, a vertex array's use of it dropped, as GL does).
  void (*deleteBuffers)(uint32_t count, const uint32_t* buffers);
  // (added with the vertex arrays; each may be null) GL's glGenVertexArrays + glBindVertexArray +
  // glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elements) + per attribute glBindBuffer(GL_ARRAY_BUFFER, buffer),
  // glEnableVertexAttribArray, glVertexAttribPointer / glVertexAttribIPointer (integer) and glVertexAttribDivisor, as a
  // new vertex array name (every packet's vao and GL's glBindVertexArray take it), with no binding changed (the vertex
  // array, GL_ARRAY_BUFFER left as they are); count 0: an empty one. 0: it could not.
  uint32_t (*createVertexArray)(const VertexAttrib* attribs, uint32_t count, uint32_t elements);
  // GL's glDeleteVertexArrays (a bound one unbound, as GL does).
  void (*deleteVertexArrays)(uint32_t count, const uint32_t* arrays);
  // (added with the framebuffers; each may be null) Render targets made without any GL binding changing (no framebuffer
  // or renderbuffer binding touched), unlike the GL calls they replace. GL's glGenRenderbuffers + glBindRenderbuffer +
  // glRenderbufferStorage(Multisample) of desc (RenderbufferDesc) as a new renderbuffer name; 0: it could not.
  uint32_t (*createRenderbuffer)(const RenderbufferDesc& desc);
  // GL's glDeleteRenderbuffers (an attached one detached, as GL does).
  void (*deleteRenderbuffers)(uint32_t count, const uint32_t* renderbuffers);
  // GL's glGenFramebuffers + glBindFramebuffer + glFramebufferTexture2D / glFramebufferTextureLayer /
  // glFramebufferRenderbuffer of the colour attachment 0 and the depth-stencil attachment (object 0: none), as a new
  // framebuffer name (bindTarget / beginPass / copy / blit ... and GL's glBindFramebuffer take it); 0: it could not.
  uint32_t (*createFramebuffer)(const Attachment& color, const Attachment& depthStencil);
  // The framebuffer's attachment(s) in mask (kClearColor: colour attachment 0; kClearDepth / kClearStencil: the
  // depth-stencil attachment) replaced by a (object 0: detached), as GL's glFramebuffer* calls on it would.
  void (*attach)(uint32_t framebuffer, uint8_t mask, const Attachment& a);
  // GL's glDeleteFramebuffers (a bound one unbound, as GL does).
  void (*deleteFramebuffers)(uint32_t count, const uint32_t* framebuffers);
  // GL's glCheckFramebufferStatus(...) == GL_FRAMEBUFFER_COMPLETE of the framebuffer: 1 complete, 0 not.
  uint8_t (*framebufferComplete)(uint32_t framebuffer);
  // (added with the samplers; each may be null) GL's glGenSamplers + glSamplerParameteri / f of desc (SamplerDesc) as a
  // new sampler object name (bindTexture's sampler, GL's glBindSampler and the bindless handles take it); 0: it could not.
  uint32_t (*createSampler)(const SamplerDesc& desc);
  // GL's glDeleteSamplers (a bound one unbound, as GL does).
  void (*deleteSamplers)(uint32_t count, const uint32_t* samplers);
};

}  // namespace cane::draw

#endif  // CANE_PLATFORM_DRAW_PACKET_H_
