#pragma once

// The GL layer (adapted from Sly1-Release gl.h / glshaders.h): the window and context, the off-screen scene
// target the frame renders into, embedded shaders, and the screenshot read-back.

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <string>

#include "gl_context.h"
#ifdef __SWITCH__
#include "../platform/switch/deko/gl_deko.h"   // (Sly 2's OpenGL on SafeHouse's deko3d layer: the offline programs)
#endif

class GLSHADER
{
public:
    GLuint ID = 0;
    void Init(const char* vertexFile, const char* fragmentFile);
    // With szPrefix inserted after both sources' #version line (extensions, defines); false (and no program) when either
    // fails to compile or the program to link.
    bool FInit(const char* vertexFile, const char* fragmentFile, const char* szPrefix);
    // The same with a geometry stage (between the two; the prefix goes into all three).
    bool FInit(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* szPrefix);
    // (VR multiview, OpenGL ES with GL_OVR_multiview2) while g_fMultiviewPass: the program's two-view variant (built on
    // first use: the same sources, the clip position of view 1 from view 0's by caneClipLtoR, its uniforms at the same
    // locations), else the program
    void Use() const;
    void Delete() { glDeleteProgram(ID); ID = 0; }
    GLint Loc(const char* name) const { return glGetUniformLocation(ID, name); }

private:
    // (multiview) the sources as compiled, the two-view program, the pass its caneClipLtoR was set for
    std::string m_vs, m_fs;
    bool m_fGeometry = false, m_fFullScreen = false;
    mutable GLuint m_idMv = 0;
    mutable int m_iMvState = 0;   // 0 not tried, 1 built, -1 failed
    mutable unsigned m_serialMv = 0;
    bool FBuildMultiview() const;
    static bool CompileErrors(GLuint shader, const char* type, const char* file);
};

class GL
{
public:
    GLFWwindow* window = nullptr;
    // Scene target (colour RGBA8 + 32-bit float depth, reversed Z: greater is nearer, like the GS).
    GLuint fbo = 0, fbc = 0, rbo = 0;
    GLuint fboMsaa = 0, rboColorMsaa = 0, rboDepthMsaa = 0;
    int msaa = 0;
    int width = 1280, height = 720;          // window
    int renderWidth = 1280, renderHeight = 720;

    // Original 0x1475C0: previous/current ordinary pictures, then an immutable two-sheet atlas.
    GLuint texOverlay = 0;
    void RememberOverlaySourceFrame();
    bool CaptureOverlayFrame();
    void ReleaseOverlayFrame();

    // fVisible false: an invisible window (headless screenshots).
    bool FInit(int width, int height, bool fVisible, const char* title, std::string& error);
    void CreateFramebuffers(int w, int h);
    // The scene target to draw into this frame.
    GLuint FboScene() const { return fboMsaa ? fboMsaa : fbo; }
    // Resolves MSAA and copies the scene to the window.
    void Present();
    // (upscaling, fsr.h; display.h fUpscale) after the scene, before the HUD: the scene (fScene; else nothing, for the
    // overlay's frames that draw only it) upscaled into a target of the picture's size, which then stands in for the
    // scene target until EndNativeUi (fbo / fbc / rbo, renderWidth / renderHeight its, no MSAA), so the HUD, the menus,
    // the foreground kinds and the overlay draw at the window's resolution over it. Nothing when FUpscaleFrame is false.
    void BeginNativeUi(bool fScene);
    // The scene target back (Present calls it; also at the frame's end without a window)
    void EndNativeUi();
    // This frame upscales: the option on, the scene smaller than the picture both ways, FSR 1 available, outside VR
    bool FUpscaleFrame() const;
    bool FNativeUi() const { return fNativeUi; }
    // Reads the resolved scene as RGBA rows (top row first) and writes a PNG.
    bool FWriteScreenshot(const std::string& path);
    void Terminate();

private:
    GLuint fboOverlay = 0, fboOverlaySource[2]{}, texOverlaySource[2]{};
    int iOverlaySource = 0, cOverlaySources = 0;
    // (the overlay's textures' sizes: the scene target's, or the native target's while upscaling)
    int dxOverlaySource = 0, dyOverlaySource = 0, dxOverlay = 0, dyOverlay = 0;
    void ReleaseOverlaySources();
    // (BeginNativeUi) the native target while the scene target waits, and the scene target's MSAA framebuffer
    GLuint fboUi = 0, texUi = 0, rboUi = 0, fboMsaaScene = 0;
    int dxUi = 0, dyUi = 0;
    bool fNativeUi = false;
    void SwapNativeUi();
    void ReleaseNativeUi();
};

extern GL g_gl;
// (GLSHADER::CompileErrors) the programs that failed to compile or link so far (main.cpp --gl-selftest: none may)
extern int g_cShaderErrors;
// (VR multiview, vr.cpp / render.cpp RenderSceneStereo) a pass drawing both eyes at once into a two-view framebuffer:
// GLSHADER::Use binds the programs' two-view variants and sets their caneClipLtoR (view 1's clip from view 0's) for the
// pass g_serialMultiview; a variant that cannot be made sets g_fMultiviewFailed (VR then draws the eyes one by one)
extern bool g_fMultiviewPass, g_fMultiviewFailed;
extern unsigned g_serialMultiview;
extern float g_amatMultiviewLtoR[16];
extern GLSHADER glGlobShader;
// glGlobShader without its fragment discards (NOKILL), for the runs without alpha test and DATE; ID 0: none
extern GLSHADER glGlobShaderNoKill;
extern GLSHADER glGlobShaderVol, glGlobShaderVolNoGeom;   // (render.cpp DrawProjVolume: the pulled no-discard program without CANE_BLEND1, with / without the near-clip stage; ID 0 elsewhere)
// (perf-4) both without the near-clip geometry stage (ID 0: not built); g_fGlobNoGeom: render.cpp may use them
extern GLSHADER glGlobShaderNoGeom, glGlobShaderNoKillNoGeom;
extern bool g_fGlobNoGeom;
// (perf-3) glGlobShader built with ARB_bindless_texture (the pass draws' textures per draw: render.cpp)
extern bool g_fGlobBindless;
// (perf-5) without bindless: the pass draws' textures as array texture layers (glob shaders TEXARRAY; render.cpp)
extern bool g_fGlobArrays;
// OpenGL ES (Android) without EXT_base_instance / multi-draw-indirect: each draw sets glob.vert's caneDrawBase (render.cpp DrawIndexed)
extern bool g_fDrawUniform;
extern bool g_fBlend1;   // (glob.frag CANE_BLEND1, render.cpp FSetBlend: the Vulkan renderer on Android)
// (VR Immersive, vr.h) the HUD drawn over transparency for the headset's panel: its alpha written too (premultiplied
// colours, "over" alpha); false (always outside VR): the alpha kept as before
extern bool g_fHudAlphaLayer;
// The pass runs drawn one by one, not by glMultiDrawElementsIndirect (render.cpp): on Mesa's OpenGL on Direct3D 12
// (Windows on ARM) each indirect draw costs a compute pass (glob.vert reads gl_VertexID)
extern bool g_fDirectRuns;
extern GLSHADER glCelBorderShader;
extern GLSHADER glHudShader;
extern GLSHADER glOverlayShader;

// (draw packets' resource entries, platform/draw_packet.h: textures made, uploaded and deleted natively on the Vulkan
// renderer, no GL binding point touched; null on the others and with SLY2N_VKNATIVE=0: the GL calls)
namespace cane::draw { struct Packets; }
const cane::draw::Packets* PpacketsTex();
// A new render texture (RGBA8, one level, zeros; the filter for both directions, clamped to the edge or GL's repeat):
// native on Vulkan (no binding touched), else glGenTextures + glBindTexture(GL_TEXTURE_2D) + glTexImage2D(nullptr) +
// glTexParameteri, the texture left bound as those leave it. GlDeleteTexture: the same split for glDeleteTextures.
GLuint GlNewTexture(int dx, int dy, GLenum filter, bool fClamp);
void GlDeleteTexture(GLuint tex);
// (the buffer entries: native on Vulkan, no binding point touched; null elsewhere and with SLY2N_VKNATIVE=0) and the
// split helpers: GlNewBuffer = glGenBuffers (+ glBindBuffer(target) + glBufferData(cb, pv, usage) when cb > 0, the buffer
// left bound to target as those leave it); GlBufferData / GlBufferSubData = glBindBuffer(target, buf) + the GL call;
// GlDeleteBuffer = glDeleteBuffers. On Vulkan none of them binds anything.
const cane::draw::Packets* PpacketsBuf();
uint8_t BufferUsageOf(GLenum usage);
GLuint GlNewBuffer(GLenum target, size_t cb, const void* pv, GLenum usage);
void GlBufferData(GLenum target, GLuint buf, size_t cb, const void* pv, GLenum usage);
void GlBufferSubData(GLenum target, GLuint buf, size_t off, size_t cb, const void* pv);
void GlDeleteBuffer(GLuint buf);
// (the vertex array entries, with the buffer ones: native on Vulkan, no binding touched; null elsewhere)
const cane::draw::Packets* PpacketsVao();
// An empty vertex array (glGenVertexArrays: no attributes, for the full-screen programs) and glDeleteVertexArrays.
GLuint GlNewVertexArray();
// (the framebuffer entries: native on Vulkan, no binding touched; null elsewhere) GlNewFramebuffer: a framebuffer of the
// texture tex (colour 0, level 0) and the renderbuffer rboDepth (depth-stencil; 0: none): native on Vulkan, else
// glGenFramebuffers + glBindFramebuffer(GL_FRAMEBUFFER) + the attachments, left bound as those leave it. The deletes:
// the same split.
const cane::draw::Packets* PpacketsFbo();
// (the sampler entries: native on Vulkan; null elsewhere)
const cane::draw::Packets* PpacketsSampler();
GLuint GlNewFramebuffer(GLuint tex, GLuint rboDepth);
void GlDeleteFramebuffers(GLsizei n, const GLuint* afbo);
void GlDeleteRenderbuffer(GLuint rbo);
void GlDeleteVertexArray(GLuint vao);
