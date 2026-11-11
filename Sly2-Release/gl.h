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
    // Reads the resolved scene as RGBA rows (top row first) and writes a PNG.
    bool FWriteScreenshot(const std::string& path);
    void Terminate();

private:
    GLuint fboOverlay = 0, fboOverlaySource[2]{}, texOverlaySource[2]{};
    int iOverlaySource = 0, cOverlaySources = 0;
    void ReleaseOverlaySources();
};

extern GL g_gl;
// (VR multiview, vr.cpp / render.cpp RenderSceneStereo) a pass drawing both eyes at once into a two-view framebuffer:
// GLSHADER::Use binds the programs' two-view variants and sets their caneClipLtoR (view 1's clip from view 0's) for the
// pass g_serialMultiview; a variant that cannot be made sets g_fMultiviewFailed (VR then draws the eyes one by one)
extern bool g_fMultiviewPass, g_fMultiviewFailed;
extern unsigned g_serialMultiview;
extern float g_amatMultiviewLtoR[16];
extern GLSHADER glGlobShader;
// glGlobShader without its fragment discards (NOKILL), for the runs without alpha test and DATE; ID 0: none
extern GLSHADER glGlobShaderNoKill;
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
