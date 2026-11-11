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
};
enum : uint8_t { kClearColor = 1, kClearDepth = 2, kClearStencil = 4 };

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
  // an array of vec4): bytes of it from data, in the block's layout (a vector's components, 16 bytes per column / element).
  void (*setConstant)(uint32_t program, int32_t location, const void* data, uint32_t bytes);
};

}  // namespace cane::draw

#endif  // CANE_PLATFORM_DRAW_PACKET_H_
