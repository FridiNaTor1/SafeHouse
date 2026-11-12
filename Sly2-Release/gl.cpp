// The GL layer, adapted from Sly1-Release gl.cpp (InitGL, CreateFramebuffers) and glshaders.cpp (embedded shader
// sources, compile errors).

#include "rumble.h"
#include "gl.h"
#include "gl_backend.h"

#include <cstdlib>
#include "pcinput.h"

#include "display.h"
#include "overlay.h"
#include "embedded_shaders.h"
#include "controller_family.h"
#include "gamepad_mappings.h"
#include "glfw_platform.h"
#if defined(__ANDROID__)
#include "gles_sly2.h"
#endif
#include "vr.h"
#include "renderer.h"
#include "fsr.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

GL g_gl;
GLSHADER glGlobShader;
GLSHADER glGlobShaderNoKill;
GLSHADER glGlobShaderVol;
GLSHADER glGlobShaderNoGeom, glGlobShaderNoKillNoGeom;
bool g_fGlobNoGeom = false;
bool g_fGlobBindless = false;
bool g_fGlobArrays = false;
bool g_fDrawUniform = false;
bool g_fBlend1 = false;   // (glob.frag CANE_BLEND1: the alpha / additive blends as one fixed blend; the Vulkan renderer on Android)
bool g_fHudAlphaLayer = false;
bool g_fDirectRuns = false;
GLSHADER glCelBorderShader;
GLSHADER glHudShader;
GLSHADER glOverlayShader;

namespace
{
std::string ShaderSource(const char* file)
{
    std::string source = CaneEmbeddedShader(file);
    // Strip a UTF-8 BOM (AMD's compiler rejects it before #version).
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF) source.erase(0, 3);
#if defined(__ANDROID__)
    // OpenGL ES 3.2 (platform/android/gles_sly2.cpp): the GLSL 4.30 sources with an ES header in place of #version
    source = cane::gles::TranslateShader(source);
#endif
    return source;
}
}

// ARB_bindless_texture usable: the extension reported, its entry points loaded, and a handle made resident and
// released for a small texture and sampler without a GL error (a broken driver path falls back to bound textures).
static bool FBindlessWorks()
{
    if (!GLAD_GL_ARB_bindless_texture) return false;
    if (!glGetTextureSamplerHandleARB || !glMakeTextureHandleResidentARB || !glMakeTextureHandleNonResidentARB) return false;
    // (Vulkan: the layer reports the extension only where its handles work, global descriptor arrays; no probe texture)
    if (FRendererVulkan()) return true;
    while (glGetError() != GL_NO_ERROR) {}
    GLuint tex = 0, sampler = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint32_t rgba = 0xFFFFFFFFu;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const GLuint64 h = glGetTextureSamplerHandleARB(tex, sampler);
    bool fOk = h != 0 && glGetError() == GL_NO_ERROR;
    if (fOk)
    {
        glMakeTextureHandleResidentARB(h);
        fOk = glGetError() == GL_NO_ERROR;
        if (fOk) glMakeTextureHandleNonResidentARB(h);
        fOk = fOk && glGetError() == GL_NO_ERROR;
    }
    glDeleteSamplers(1, &sampler);
    glDeleteTextures(1, &tex);
    while (glGetError() != GL_NO_ERROR) {}
    if (!fOk) std::printf("OpenGL: ARB_bindless_texture reported but a handle failed: bound textures\n");
    return fOk;
}

bool GLSHADER::FInit(const char* vertexFile, const char* fragmentFile, const char* szPrefix)
{
    return FInit(vertexFile, nullptr, fragmentFile, szPrefix);
}

bool GLSHADER::FInit(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* szPrefix)
{
    if (FRendererVulkan())   // (renderer.h: the programs compiled to SPIR-V at build time)
    {
        ID = IdVulkanProgram(vertexFile, geometryFile, fragmentFile, szPrefix);
        return ID != 0;
    }
#ifdef __SWITCH__
    // (deko3d: the programs and their variants are compiled offline, cmake/switch_shaders.py; a variant not built fails
    // as a failed link would, and the caller falls back)
    const GLuint idDeko = CaneDeko::CreateProgram(vertexFile, geometryFile, fragmentFile, szPrefix);
    if (idDeko) ID = idDeko;
    return idDeko != 0;
#endif
    auto withPrefix = [&](std::string src) {
        const size_t i = src.find("#version");
        const size_t j = i == std::string::npos ? 0 : src.find('\n', i) + 1;
        src.insert(j, szPrefix);
        return src;
    };
    const std::string vs = withPrefix(ShaderSource(vertexFile)), fs = withPrefix(ShaderSource(fragmentFile));
    const char* pvs = vs.c_str();
    const char* pfs = fs.c_str();
    const GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &pvs, nullptr);
    glCompileShader(v);
    const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &pfs, nullptr);
    glCompileShader(f);
    bool fOk = CompileErrors(v, "VERTEX", vertexFile) && CompileErrors(f, "FRAGMENT", fragmentFile);
    GLuint g = 0;
    if (geometryFile)
    {
        const std::string gs = withPrefix(ShaderSource(geometryFile));
        const char* pgs = gs.c_str();
        g = glCreateShader(GL_GEOMETRY_SHADER);
        glShaderSource(g, 1, &pgs, nullptr);
        glCompileShader(g);
        fOk = fOk && CompileErrors(g, "GEOMETRY", geometryFile);
    }
    GLuint id = 0;
    if (fOk)
    {
        id = glCreateProgram();
        glAttachShader(id, v);
        if (g) glAttachShader(id, g);
        glAttachShader(id, f);
        glLinkProgram(id);
        fOk = CompileErrors(id, "PROGRAM", vertexFile);
        if (!fOk) glDeleteProgram(id);
    }
    glDeleteShader(v);
    if (g) glDeleteShader(g);
    glDeleteShader(f);
    if (fOk)
    {
        ID = id;
        m_vs = vs, m_fs = fs, m_fGeometry = geometryFile != nullptr;
        m_fFullScreen = !std::strcmp(vertexFile, "shadowcombine.vert");
    }
    return fOk;
}

bool g_fMultiviewPass = false, g_fMultiviewFailed = false;
unsigned g_serialMultiview = 0;
float g_amatMultiviewLtoR[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

void GLSHADER::Use() const
{
    // (Vulkan: the layer runs the program's two-view stages, made at build time, in a two-view pass)
    if (g_fMultiviewPass && !g_fMultiviewFailed && !FRendererVulkan())
    {
        if (m_iMvState == 0) m_iMvState = FBuildMultiview() ? 1 : -1;
        if (m_iMvState < 0) g_fMultiviewFailed = true;
        else
        {
            glUseProgram(m_idMv);
            // (view 1's matrix in a buffer, not a uniform: a new uniform would move the others' locations)
            static GLuint s_abuf[2] = {};
            static unsigned s_serial = 0;
            if (!s_abuf[0])
            {
                static const float s_aidentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                glGenBuffers(2, s_abuf);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_abuf[1]);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(s_aidentity), s_aidentity, GL_STATIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, s_abuf[1]);
            }
            if (s_serial != g_serialMultiview)
            {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_abuf[0]);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(g_amatMultiviewLtoR), g_amatMultiviewLtoR, GL_STREAM_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, s_abuf[0]);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, s_abuf[1]);
                s_serial = g_serialMultiview;
            }
            return;
        }
    }
    glUseProgram(ID);
}

// The two-view variant: the vertex program's main renamed and called by one that gives view 1 the clip position of
// view 0's times caneClipLtoR (clip = matrix x world for both eyes, so one 4x4 maps the one to the other; a full-screen
// pass keeps its position) and the view to the fragment program, whose frame copies (texDate / texAlpha) become the
// two-view arrays.
bool GLSHADER::FBuildMultiview() const
{
    if (m_vs.empty() || m_fs.empty() || m_fGeometry) return false;
    std::string vs = m_vs, fs = m_fs;
    auto afterVersion = [](std::string& src, const char* sz) {
        const size_t i = src.find("#version");
        const size_t j = i == std::string::npos ? 0 : src.find('\n', i) + 1;
        src.insert(j, sz);
    };
    auto replaceAll = [](std::string& src, const std::string& from, const std::string& to) {
        for (size_t i = src.find(from); i != std::string::npos; i = src.find(from, i + to.size())) src.replace(i, from.size(), to);
    };
    afterVersion(vs, "#extension GL_OVR_multiview2 : require\n");
    const size_t iMain = vs.find("void main()");
    if (iMain == std::string::npos) return false;
    vs.replace(iMain, 11, "void caneMainBody()");
    vs += std::string("\nlayout(num_views = 2) in;\nlayout(std430, binding = ") + (m_fFullScreen ? "12" : "11") +
          ") readonly buffer CANEMULTIVIEW { mat4 caneClipLtoR; };\nflat out uint caneView;\n"
          "void main() { caneMainBody(); caneView = gl_ViewID_OVR; if (gl_ViewID_OVR != 0u) gl_Position = caneClipLtoR * gl_Position; }\n";
    const size_t iMainF = fs.find("void main()");
    if (iMainF == std::string::npos) return false;
    fs.insert(iMainF, "flat in uint caneView;\n");
    for (const char* sz : {"texDate", "texAlpha"})
    {
        replaceAll(fs, std::string("uniform sampler2D ") + sz + ";", std::string("uniform highp sampler2DArray ") + sz + ";");
        replaceAll(fs, std::string("texelFetch(") + sz + ", ivec2(gl_FragCoord.xy), 0)",
                   std::string("texelFetch(") + sz + ", ivec3(ivec2(gl_FragCoord.xy), int(caneView)), 0)");
    }
    const char* pvs = vs.c_str();
    const char* pfs = fs.c_str();
    const GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &pvs, nullptr);
    glCompileShader(v);
    const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &pfs, nullptr);
    glCompileShader(f);
    bool fOk = CompileErrors(v, "VERTEX", "multiview") && CompileErrors(f, "FRAGMENT", "multiview");
    GLuint id = 0;
    if (fOk)
    {
        id = glCreateProgram();
        glAttachShader(id, v);
        glAttachShader(id, f);
        glLinkProgram(id);
        fOk = CompileErrors(id, "PROGRAM", "multiview");
    }
    glDeleteShader(v);
    glDeleteShader(f);
    // (the callers keep the program's uniform locations: the variant's must be the same)
    if (fOk)
    {
        GLint c = 0;
        glGetProgramiv(ID, GL_ACTIVE_UNIFORMS, &c);
        for (GLint i = 0; i < c && fOk; ++i)
        {
            char name[256];
            GLsizei len = 0;
            GLint size = 0;
            GLenum type = 0;
            glGetActiveUniform(ID, GLuint(i), sizeof(name), &len, &size, &type, name);
            if (glGetUniformLocation(ID, name) != glGetUniformLocation(id, name))
            {
                std::printf("OpenGL: multiview variant: uniform %s at another location: eyes one by one\n", name);
                fOk = false;
            }
        }
    }
    if (!fOk) { if (id) glDeleteProgram(id); return false; }
    m_idMv = id;
    return true;
}

int g_cShaderErrors = 0;

bool GLSHADER::CompileErrors(GLuint shader, const char* type, const char* file)
{
    GLint fOk = 0;
    char log[2048];
    if (std::strcmp(type, "PROGRAM") != 0)
    {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &fOk);
        if (!fOk)
        {
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "shader compile error (%s):\n%s\n", file, log);
            ++g_cShaderErrors;
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &fOk);
        if (!fOk)
        {
            glGetProgramInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "shader link error (%s):\n%s\n", file, log);
            ++g_cShaderErrors;
        }
    }
    return fOk != 0;
}

void GLSHADER::Init(const char* vertexFile, const char* fragmentFile)
{
    if (FRendererVulkan())
    {
        ID = IdVulkanProgram(vertexFile, nullptr, fragmentFile, "");
        return;
    }
#ifdef __SWITCH__
    ID = CaneDeko::CreateProgram(vertexFile, nullptr, fragmentFile, "");
    return;
#endif
    const std::string vs = ShaderSource(vertexFile), fs = ShaderSource(fragmentFile);
    const char* pvs = vs.c_str();
    const char* pfs = fs.c_str();
    const GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &pvs, nullptr);
    glCompileShader(v);
    CompileErrors(v, "VERTEX", vertexFile);
    const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &pfs, nullptr);
    glCompileShader(f);
    CompileErrors(f, "FRAGMENT", fragmentFile);
    ID = glCreateProgram();
    glAttachShader(ID, v);
    glAttachShader(ID, f);
    glLinkProgram(ID);
    CompileErrors(ID, "PROGRAM", vertexFile);
    glDeleteShader(v);
    glDeleteShader(f);
    m_vs = vs, m_fs = fs, m_fGeometry = false;
    m_fFullScreen = !std::strcmp(vertexFile, "shadowcombine.vert");
}

bool GL::FInit(int w, int h, bool fVisible, const char* title, std::string& error)
{
    if (!cane::initGlfw()) { error = "glfwInit failed (no display?)"; return false; }
    cane::input::installJoystickCallback();
    cane::input::applyBundledMappings();  // Steam passes its virtual pads' mappings in SDL_GAMECONTROLLERCONFIG
    const auto createWindow = [&](int minor) {
        cane::gl::ApplyWindowHints(4, minor);
        glfwWindowHint(GLFW_DEPTH_BITS, 0);
        glfwWindowHint(GLFW_STENCIL_BITS, 0);
        glfwWindowHint(GLFW_VISIBLE, fVisible ? GLFW_TRUE : GLFW_FALSE);
        return glfwCreateWindow(w, h, title, nullptr, nullptr);
    };
    width = w;
    height = h;
    // (a launcher handing off to the game: the window is shown with its first frame, platform/gl_context.h)
    fVisible = cane::gl::HandoffVisible(fVisible);
    // (renderer.h) Vulkan when it is chosen and starts: the GL calls below then run on it; otherwise OpenGL
    window = PwindowVulkan(w, h, fVisible, title);
    if (!window) window = createWindow(5);
    std::string contextFailure;
#if defined(__linux__) && !defined(__ANDROID__)
    // A GPU whose native Mesa driver stops short of OpenGL 4.5 (ARM devices) may reach it through Mesa's Zink, on Vulkan.
    if (!window)
    {
        contextFailure = cane::gl::DescribeContextFailure();
        std::fprintf(stderr, "OpenGL 4.5 context failed (%s)\n", contextFailure.c_str());
        // Zink on some ARM GPUs stops at 4.3; the shaders are GLSL 430, and what they need from 4.4 and 4.5 is checked below.
        window = createWindow(3);
        if (window) std::printf("OpenGL: 4.5 is not offered, using 4.3 and extensions\n");
        if (!window && !std::getenv("MESA_LOADER_DRIVER_OVERRIDE"))
        {
            glfwTerminate();
            setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
            if (cane::initGlfw())
            {
                cane::input::installJoystickCallback();
                cane::input::applyBundledMappings();
                window = createWindow(5);
                if (!window) window = createWindow(3);
                if (window) std::printf("OpenGL: using Mesa's Zink driver (the native driver has no OpenGL 4.5)\n");
                else contextFailure += "; with Zink: " + cane::gl::DescribeContextFailure();
            }
        }
    }
#endif
    if (!window)
    {
        glfwTerminate();
        error = "OpenGL 4.5 is required: could not create a graphics context";
        if (!contextFailure.empty()) error += " (" + contextFailure + ")";
        return false;
    }
    if (!cane::gl::CreateContext(window, &error)) return false;
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(cane::gl::ProcAddress))) { error = "glad failed"; return false; }
    std::printf("OpenGL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
#if defined(__ANDROID__)
    // The GL 4.3 loader stops at the version an ES context reports (3.2): the ES 3.x entry points with desktop names
    // are fetched by name, the desktop-only ones point at ES equivalents (gles_sly2.cpp). (The Vulkan renderer: its
    // own entry points, the desktop paths.)
    if (!FRendererVulkan())
    {
        if (!cane::gles::FillGlad(error)) return false;
        g_fDrawUniform = !cane::gles::HasBaseInstance();
    }
    else
    {
        // (the Vulkan renderer on Android: render.cpp's merged runs with glob.vert's vertex pulling (CANE_PULL), plain
        // indexed draws in place of multi-draw-indirect: Adreno's tile binning cannot skip the sub-draws of an indirect
        // draw per tile. SLY2N_VKPULL=0: multi-draw-indirect.)
        // (Quest 3S, Paris Immersive: 21 -> 36 fps, the eye passes binned in place of the driver's direct mode)
        g_fDrawUniform = !(std::getenv("SLY2N_VKPULL") && std::atoi(std::getenv("SLY2N_VKPULL")) == 0);
    }
#else
    // (a test of the Android Vulkan path elsewhere: SLY2N_VKPULL=1, the pulled runs and their one blend pair)
    if (FRendererVulkan() && std::getenv("SLY2N_VKPULL") && std::atoi(std::getenv("SLY2N_VKPULL")) == 1) g_fDrawUniform = true;
#endif
    const char* szRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    g_fDirectRuns = szRenderer && !std::strncmp(szRenderer, "D3D12 (", 7);
    StartupPcInput();   // (the prompt icons' atlases: pcinput.h / promptglyph.h)
    cane::gl::SwapInterval(1);
    // Reversed Z in [0, 1]: the GS's depth tests (GREATER / GEQUAL, larger = nearer) map one to one.
    // (glad here is the GL 4.3 loader: fetch glClipControl, GL 4.5 / ARB_clip_control, directly.)
    using PFNCLIPCONTROL = void (*)(GLenum, GLenum);
    auto pfnClipControl = reinterpret_cast<PFNCLIPCONTROL>(cane::gl::ProcAddress("glClipControl"));
    if (!pfnClipControl) { error = "glClipControl (GL 4.5) is required"; return false; }
#if defined(__linux__) && !defined(__ANDROID__)
    if ((GLVersion.major < 4 || (GLVersion.major == 4 && GLVersion.minor < 5)) && !glfwExtensionSupported("GL_ARB_clip_control"))
    {
        error = "OpenGL 4.5 or GL_ARB_clip_control is required; this driver offers " + std::string(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        return false;
    }
#endif
    constexpr GLenum GL_ZERO_TO_ONE_ = 0x935F;
    pfnClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE_);
    CreateFramebuffers(w, h);
    // The picture / scene target for the display options (aspect, internal resolution, MSAA).
    int fbw = w, fbh = h;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    LayoutDisplay(fbw > 0 ? fbw : w, fbh > 0 ? fbh : h);
    // (perf-3) the pass draws' textures as bindless handles where the driver has them (fewer state changes per frame:
    // the runs of draws are no longer split by texture); SLY2N_NOBINDLESS=1: bound textures as before.
    // (release-4) On Windows only with SLY2N_BINDLESS=1: a multi-draw's sub-draws each sample their own handle (a flat
    // varying: not dynamically uniform), which the Windows GL drivers (AMD's above all) are not known to get right, and
    // the gain is small (at most 0.22 ms a frame in Paris). Everywhere the extension's
    // entry points and one handle are tried first (FBindlessWorks); any failure: bound textures.
#if defined(_WIN32)
    const bool fBindlessWanted = std::getenv("SLY2N_BINDLESS") && std::atoi(std::getenv("SLY2N_BINDLESS")) != 0;
#else
    const bool fBindlessWanted = !std::getenv("SLY2N_NOBINDLESS");
#endif
    // (R51) glob.geom clips at the near plane as the original's VU1 program does, so the GS's screen-affine colours
    // start from the clip-space values there; SLY2N_NEARCLIP=0: GL's own clipping (its noperspective rule extrapolates
    // under a low camera). If the stage fails to build, the program without it.
#ifdef __SWITCH__
    constexpr bool s_fNearClip = false;   // (the Switch: GL's own clipping; the stage costs GPU time on every draw)
#elif defined(__APPLE__)
    // (macOS / iOS: the programs run on Metal (platform/metal), which has no geometry stage: glCreateShader refuses
    // GL_GEOMETRY_SHADER with GL_INVALID_ENUM and glob.metal is the program without it, so the stage is not asked for
    // (it used to be: "shader compile error (glob.geom)" and a GL error left behind at every start). GL's own clipping.
    constexpr bool s_fNearClip = false;
#else
    // (VR on OpenGL ES, vr.h: without the stage: geometry programs are slow on mobile GPUs, and the eyes' two-view pass
    // cannot have one; GL's own near clipping)
#if defined(__ANDROID__)
    // (Android: built as everywhere; while an OpenXR session is active the runs use the programs without it, render.cpp
    // useGlob: the stage was half the frame on the Quest's Adreno in VR. VR is requested on every Android device, but a
    // phone never gets a session, so flat devices keep the original near-plane clip.)
    static const bool s_fNearClip = !(std::getenv("SLY2N_NEARCLIP") && std::atoi(std::getenv("SLY2N_NEARCLIP")) == 0);
#else
    static const bool s_fNearClip = !(std::getenv("SLY2N_NEARCLIP") && std::atoi(std::getenv("SLY2N_NEARCLIP")) == 0);
#endif
#endif
    // (the prefix and stages the glob program was built with: its NOKILL variant below takes the same)
    std::string globPrefix;
    bool fGlobGeom = false;
    // (the Vulkan renderer's pulled draws, g_fDrawUniform: OpenGL ES gets CANE_PULL from its translation layer,
    // gles_sly2.cpp; Vulkan's programs are built ahead with it, cmake/vulkan_shaders.py)
    // (and CANE_BLEND1, render.cpp g_fBlend1: the alpha / additive blend pair as one pipeline; SLY2N_VKBLEND1=0: off)
    GLint cDualSource = 0;
    if (g_fDrawUniform && FRendererVulkan()) glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &cDualSource);
    g_fBlend1 = g_fDrawUniform && FRendererVulkan() && cDualSource > 0 &&
                !(std::getenv("SLY2N_VKBLEND1") && std::atoi(std::getenv("SLY2N_VKBLEND1")) == 0);
    const std::string pullDef = g_fDrawUniform && FRendererVulkan() ? "#define CANE_PULL 1\n#define CANE_BLEND1 1\n" : "";
    auto initGlob = [&](const char* szPrefix) {
        globPrefix = szPrefix + pullDef;
        fGlobGeom = s_fNearClip && glGlobShader.FInit("glob.vert", "glob.geom", "glob.frag", (globPrefix + "#define GEOM 1\n").c_str());
        return fGlobGeom || glGlobShader.FInit("glob.vert", "glob.frag", globPrefix.c_str());
    };
    g_fGlobBindless = fBindlessWanted && FBindlessWorks() && initGlob("#extension GL_ARB_bindless_texture : require\n#define BINDLESS 1\n");
    // (perf-5) without bindless textures: the pass draws' textures as layers of array textures (render.cpp LayerRefOfTex),
    // so runs join across textures of one size; needs glCopyImageSubData and glTexStorage3D (GL 4.3 / GLES 3.2).
    // SLY2N_NOTEXARRAYS=1: each run binds its texture as before. Not on the Switch (its programs are built offline).
#ifndef __SWITCH__
    if (!g_fGlobBindless && glCopyImageSubData && glTexStorage3D && !(std::getenv("SLY2N_NOTEXARRAYS") && std::atoi(std::getenv("SLY2N_NOTEXARRAYS")) != 0))
        g_fGlobArrays = initGlob("#define TEXARRAY 1\n");
#endif
    if (!g_fGlobBindless && !g_fGlobArrays && !initGlob(""))
    {
        glGlobShader.Init("glob.vert", "glob.frag");
        globPrefix.clear();
        fGlobGeom = false;
    }
    std::printf("OpenGL: textures %s\n", g_fGlobBindless ? "bindless (ARB_bindless_texture)" : g_fGlobArrays ? "bound, as array layers" : "bound");
    // The glob program without its fragment discards (NOKILL: no alpha test, no DATE), for the runs that need neither
    // (render.cpp): the GPU then tests depth and stencil before the fragments. ID 0 if it does not build: glGlobShader.
    const std::string prefixNoKill = globPrefix + (fGlobGeom ? "#define GEOM 1\n" : "") + "#define NOKILL 1\n";
    if (!(fGlobGeom ? glGlobShaderNoKill.FInit("glob.vert", "glob.geom", "glob.frag", prefixNoKill.c_str())
                    : glGlobShaderNoKill.FInit("glob.vert", "glob.frag", prefixNoKill.c_str())))
        glGlobShaderNoKill.ID = 0;
    // (the Vulkan renderer's pulled programs, CANE_BLEND1) the no-discard program without CANE_BLEND1's second output, for
    // the projected volumes' colour draws (render.cpp FDrawProjVolumePackets): the Quest 3S's Adreno hangs the GPU drawing
    // the volumes' fill with that output (a blend that doesn't read it) while its video capture preempts the GPU (notes/
    // STATUS-VK.md volume-dim). Same pictures. SLY2N_VKVOLPROG=0: the volumes with the glob program as before.
    glGlobShaderVol.ID = 0;
    if (!pullDef.empty() && glGlobShaderNoKill.ID && !(std::getenv("SLY2N_VKVOLPROG") && std::atoi(std::getenv("SLY2N_VKVOLPROG")) == 0))
    {
        std::string prefixVol = prefixNoKill;
        static const char szBlend1[] = "#define CANE_BLEND1 1\n";
        if (const size_t i = prefixVol.find(szBlend1); i != std::string::npos) prefixVol.erase(i, sizeof(szBlend1) - 1);
        if (!(fGlobGeom ? glGlobShaderVol.FInit("glob.vert", "glob.geom", "glob.frag", prefixVol.c_str())
                        : glGlobShaderVol.FInit("glob.vert", "glob.frag", prefixVol.c_str())))
            glGlobShaderVol.ID = 0;
        std::printf("OpenGL: projected volumes %s\n", glGlobShaderVol.ID ? "with the program without CANE_BLEND1" : "with the glob program (no variant)");
    }
    // (perf-4) both without the near-clip stage, for the runs whose draws are all wholly in front of the near plane
    // (render.cpp): glob.geom passes their triangles through unchanged, and a geometry stage costs GPU time on every draw
    // (the Switch leaves it out altogether; tile GPUs and Zink pay most). SLY2N_GLOBNOGEOM=0: the stage everywhere.
    glGlobShaderNoGeom.ID = glGlobShaderNoKillNoGeom.ID = 0;
    g_fGlobNoGeom = false;
    if (fGlobGeom && !(std::getenv("SLY2N_GLOBNOGEOM") && std::atoi(std::getenv("SLY2N_GLOBNOGEOM")) == 0))
    {
        if (!glGlobShaderNoGeom.FInit("glob.vert", "glob.frag", globPrefix.c_str())) glGlobShaderNoGeom.ID = 0;
        if (!glGlobShaderNoKill.ID || !glGlobShaderNoKillNoGeom.FInit("glob.vert", "glob.frag", (globPrefix + "#define NOKILL 1\n").c_str()))
            glGlobShaderNoKillNoGeom.ID = 0;
        g_fGlobNoGeom = glGlobShaderNoGeom.ID != 0;
    }
    glCelBorderShader.Init("celborder.vert", "celborder.frag");
    glHudShader.Init("hud.vert", "hud.frag");
    glOverlayShader.Init("overlay.vert", "overlay.frag");
    return true;
}

void GL::CreateFramebuffers(int w, int h)
{
    EndNativeUi();
    ReleaseOverlaySources();  // the immutable photograph survives resizing its presentation target
    renderWidth = w;
    renderHeight = h;
    if (fbo) { GlDeleteFramebuffers(1, &fbo); GlDeleteTexture(fbc); GlDeleteRenderbuffer(rbo); }
    if (fboMsaa) { GlDeleteFramebuffers(1, &fboMsaa); GlDeleteRenderbuffer(rboColorMsaa); GlDeleteRenderbuffer(rboDepthMsaa); fboMsaa = 0; }
    if (const cane::draw::Packets* pkf = PpacketsFbo())
    {
        // (native render targets: nothing bound, as the GL calls below leave framebuffer 0)
        using namespace cane::draw;
        fbc = GlNewTexture(w, h, GL_LINEAR, false);
        RenderbufferDesc rd;
        rd.width = uint32_t(w), rd.height = uint32_t(h), rd.format = kRenderbufferDepth32fStencil8;
        rbo = pkf->createRenderbuffer(rd);
        fbo = GlNewFramebuffer(fbc, rbo);
        if (msaa > 1)
        {
            rd.samples = uint8_t(msaa);
            rd.format = kRenderbufferRgba8;
            rboColorMsaa = pkf->createRenderbuffer(rd);
            rd.format = kRenderbufferDepth32fStencil8;
            rboDepthMsaa = pkf->createRenderbuffer(rd);
            Attachment c, d;
            c.object = rboColorMsaa, c.renderbuffer = 1;
            d.object = rboDepthMsaa, d.renderbuffer = 1;
            fboMsaa = pkf->createFramebuffer(c, d);
        }
        pkf->bindTarget(0);
        return;
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    fbc = GlNewTexture(w, h, GL_LINEAR, false);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbc, 0);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
    if (msaa > 1)
    {
        glGenFramebuffers(1, &fboMsaa);
        glBindFramebuffer(GL_FRAMEBUFFER, fboMsaa);
        glGenRenderbuffers(1, &rboColorMsaa);
        glBindRenderbuffer(GL_RENDERBUFFER, rboColorMsaa);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboColorMsaa);
        glGenRenderbuffers(1, &rboDepthMsaa);
        glBindRenderbuffer(GL_RENDERBUFFER, rboDepthMsaa);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa, GL_DEPTH32F_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboDepthMsaa);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// (draw packets, platform/draw_packet.h: the Vulkan renderer's table, null on the others: the GL calls)
static const cane::draw::Packets* PacketsActive()
{
    return cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
}

const cane::draw::Packets* PpacketsTex()
{
    const cane::draw::Packets* pk = PacketsActive();
    return pk && pk->createTexture && pk->uploadTexture && pk->generateMipmaps && pk->deleteTextures ? pk : nullptr;
}
const cane::draw::Packets* PpacketsBuf()
{
    const cane::draw::Packets* pk = PacketsActive();
    return pk && pk->createBuffer && pk->bufferData && pk->bufferSubData && pk->deleteBuffers && pk->mapStream && pk->mapRange ? pk : nullptr;
}
GLuint GlNewBuffer(GLenum target, size_t cb, const void* pv, GLenum usage)
{
    if (const cane::draw::Packets* pk = PpacketsBuf())
        return pk->createBuffer(cb, pv, uint8_t(BufferUsageOf(usage) | (target == GL_DRAW_INDIRECT_BUFFER ? cane::draw::kBufferIndirect : 0)));
    GLuint buf = 0;
    glGenBuffers(1, &buf);
    if (!cb) return buf;
    glBindBuffer(target, buf);
    glBufferData(target, GLsizeiptr(cb), pv, usage);
    return buf;
}
uint8_t BufferUsageOf(GLenum usage)
{
    return usage == GL_STREAM_DRAW ? cane::draw::kBufferStream : usage == GL_DYNAMIC_DRAW ? cane::draw::kBufferDynamic : cane::draw::kBufferStatic;
}
void GlBufferData(GLenum target, GLuint buf, size_t cb, const void* pv, GLenum usage)
{
    if (const cane::draw::Packets* pk = PpacketsBuf())
        return pk->bufferData(buf, cb, pv, uint8_t(BufferUsageOf(usage) | (target == GL_DRAW_INDIRECT_BUFFER ? cane::draw::kBufferIndirect : 0)));
    glBindBuffer(target, buf);
    glBufferData(target, GLsizeiptr(cb), pv, usage);
}
void GlBufferSubData(GLenum target, GLuint buf, size_t off, size_t cb, const void* pv)
{
    if (const cane::draw::Packets* pk = PpacketsBuf()) return pk->bufferSubData(buf, off, cb, pv);
    glBindBuffer(target, buf);
    glBufferSubData(target, GLintptr(off), GLsizeiptr(cb), pv);
}
void GlDeleteBuffer(GLuint buf)
{
    if (!buf) return;
    if (const cane::draw::Packets* pk = PpacketsBuf()) pk->deleteBuffers(1, &buf);
    else glDeleteBuffers(1, &buf);
}
const cane::draw::Packets* PpacketsVao()
{
    const cane::draw::Packets* pk = PpacketsBuf();
    return pk && pk->createVertexArray && pk->deleteVertexArrays ? pk : nullptr;
}
const cane::draw::Packets* PpacketsFbo()
{
    const cane::draw::Packets* pk = PpacketsTex();
    return pk && pk->createRenderbuffer && pk->deleteRenderbuffers && pk->createFramebuffer && pk->attach && pk->deleteFramebuffers &&
           pk->framebufferComplete && pk->bindTarget ? pk : nullptr;
}
const cane::draw::Packets* PpacketsSampler()
{
    const cane::draw::Packets* pk = PacketsActive();
    return pk && pk->createSampler && pk->deleteSamplers ? pk : nullptr;
}
GLuint GlNewFramebuffer(GLuint tex, GLuint rboDepth)
{
    if (const cane::draw::Packets* pk = PpacketsFbo())
    {
        cane::draw::Attachment c, d;
        c.object = tex;
        d.object = rboDepth, d.renderbuffer = 1;
        return pk->createFramebuffer(c, d);
    }
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (tex) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (rboDepth) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboDepth);
    return fbo;
}
void GlDeleteFramebuffers(GLsizei n, const GLuint* afbo)
{
    if (const cane::draw::Packets* pk = PpacketsFbo()) pk->deleteFramebuffers(uint32_t(n), afbo);
    else glDeleteFramebuffers(n, afbo);
}
void GlDeleteRenderbuffer(GLuint rbo)
{
    if (const cane::draw::Packets* pk = PpacketsFbo()) pk->deleteRenderbuffers(1, &rbo);
    else glDeleteRenderbuffers(1, &rbo);
}
GLuint GlNewVertexArray()
{
    if (const cane::draw::Packets* pk = PpacketsVao()) return pk->createVertexArray(nullptr, 0, 0);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    return vao;
}
void GlDeleteVertexArray(GLuint vao)
{
    if (!vao) return;
    if (const cane::draw::Packets* pk = PpacketsVao()) pk->deleteVertexArrays(1, &vao);
    else glDeleteVertexArrays(1, &vao);
}
GLuint GlNewTexture(int dx, int dy, GLenum filter, bool fClamp)
{
    GLuint tex = 0;
    if (const cane::draw::Packets* pk = PpacketsTex())
    {
        cane::draw::TextureDesc d;
        d.width = uint32_t(dx), d.height = uint32_t(dy), d.storage = 0;
        d.minFilter = d.magFilter = filter == GL_NEAREST ? cane::draw::kFilterNearest : cane::draw::kFilterLinear;
        if (fClamp) d.wrapS = d.wrapT = cane::draw::kWrapClampToEdge;
        return pk->createTexture(d);
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(filter));
    if (fClamp)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return tex;
}
void GlDeleteTexture(GLuint tex)
{
    if (!tex) return;
    if (const cane::draw::Packets* pk = PpacketsTex()) pk->deleteTextures(1, &tex);
    else glDeleteTextures(1, &tex);
}

// orig: 0x16FB78/0x16FCA8: retain the displayed picture separately from the one being drawn.
void GL::RememberOverlaySourceFrame()
{
    if (!window) return;
    // (upscaling: the frame is the native target's while BeginNativeUi holds; a size change starts over)
    if (fboOverlaySource[0] && (dxOverlaySource != renderWidth || dyOverlaySource != renderHeight)) ReleaseOverlaySources();
    if (!fboOverlaySource[0]) {
        dxOverlaySource = renderWidth, dyOverlaySource = renderHeight;
        if (PpacketsFbo())
            for (int i = 0; i < 2; ++i) {
                texOverlaySource[i] = GlNewTexture(renderWidth, renderHeight, GL_LINEAR, true);
                fboOverlaySource[i] = GlNewFramebuffer(texOverlaySource[i], 0);
            }
        else {
        glGenFramebuffers(2, fboOverlaySource);
        for (int i = 0; i < 2; ++i) {
            texOverlaySource[i] = GlNewTexture(renderWidth, renderHeight, GL_LINEAR, true);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlaySource[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texOverlaySource[i], 0);
        }
        }
    }
    // Resolve first: multisample resolves cannot also scale the picture into the atlas.
    // (draw packets: the scissor off and the scene framebuffer bound afterwards, as the GL calls leave them)
    if (const cane::draw::Packets* pk = PacketsActive(); pk && pk->scissor && pk->copy) {
        const int32_t full[4] = {0, 0, renderWidth, renderHeight};
        pk->scissor(nullptr);
        if (fboMsaa) pk->copy(fboMsaa, fbo, full, 0, 0, fbo);
        pk->copy(fbo, fboOverlaySource[iOverlaySource], full, 0, 0, FboScene());
    } else {
        glDisable(GL_SCISSOR_TEST);
        if (fboMsaa) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMsaa);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlaySource[iOverlaySource]);
        glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, FboScene());
    }
    iOverlaySource ^= 1;
    cOverlaySources = std::min(cOverlaySources + 1, 2);
}

// orig: 0x1475C0: previous picture in the left atlas half; current picture in the right.
bool GL::CaptureOverlayFrame()
{
    if (!window || cOverlaySources == 0) return false;
    if (fboOverlay && (dxOverlay != renderWidth || dyOverlay != renderHeight)) {
        GlDeleteFramebuffers(1, &fboOverlay); GlDeleteTexture(texOverlay);
        fboOverlay = texOverlay = 0;
    }
    if (!fboOverlay && PpacketsFbo()) {
        dxOverlay = renderWidth, dyOverlay = renderHeight;
        texOverlay = GlNewTexture(renderWidth, renderHeight, GL_LINEAR, true);
        fboOverlay = GlNewFramebuffer(texOverlay, 0);
    }
    if (!fboOverlay) {
        dxOverlay = renderWidth, dyOverlay = renderHeight;
        glGenFramebuffers(1, &fboOverlay);
        texOverlay = GlNewTexture(renderWidth, renderHeight, GL_LINEAR, true);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlay);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texOverlay, 0);
    }
    const cane::draw::Packets* pk = PacketsActive();
    if (pk && !(pk->scissor && pk->blit)) pk = nullptr;
    if (pk) pk->scissor(nullptr);
    else glDisable(GL_SCISSOR_TEST);
    // The original's picture is its whole 4:3 frame; the overlay draws it in the 4:3 UI canvas. At other aspects
    // the native view is wider (or taller) than that frame, so the picture is the centred 4:3 part of the render,
    // not the whole render squeezed into it (U31: squashed at 32:9).
    // (the sources' own size: the native target's when they were taken while upscaling)
    const int dxSource = dxOverlaySource, dySource = dyOverlaySource;
    int dxSrc = dxSource, dySrc = dySource;
    if (dxSrc * 3 > dySrc * 4) dxSrc = std::max(1, int(std::lround(dySrc * 4.0 / 3.0)));
    else if (dxSrc * 3 < dySrc * 4) dySrc = std::max(1, int(std::lround(dxSrc * 3.0 / 4.0)));
    const int xSrc = (dxSource - dxSrc) / 2, ySrc = (dySource - dySrc) / 2;
    for (int i = 0; i < 2; ++i) {
        const int source = i == 0 && cOverlaySources == 2 ? iOverlaySource : iOverlaySource ^ 1;
        if (pk) {
            // (draw packets: the scene framebuffer bound afterwards, as the GL calls leave it)
            const int32_t rcSrc[4] = {xSrc, ySrc, dxSrc, dySrc};
            const int32_t rcDst[4] = {i * renderWidth / 2, 0, (i + 1) * renderWidth / 2 - i * renderWidth / 2, renderHeight};
            pk->blit(fboOverlaySource[source], fboOverlay, rcSrc, rcDst, 1, FboScene());
            continue;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboOverlaySource[source]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlay);
        glBlitFramebuffer(xSrc, ySrc, xSrc + dxSrc, ySrc + dySrc, i * renderWidth / 2, 0,
                          (i + 1) * renderWidth / 2, renderHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    if (!pk) glBindFramebuffer(GL_FRAMEBUFFER, FboScene());
    return true;
}

void GL::ReleaseOverlayFrame()
{
    if (!window) return;
    GlDeleteFramebuffers(1, &fboOverlay); GlDeleteTexture(texOverlay);
    fboOverlay = texOverlay = 0;
    ReleaseOverlaySources();
}

void GL::ReleaseOverlaySources()
{
    if (!window) return;
    GlDeleteFramebuffers(2, fboOverlaySource); GlDeleteTexture(texOverlaySource[0]); GlDeleteTexture(texOverlaySource[1]);
    for (int i = 0; i < 2; ++i) fboOverlaySource[i] = texOverlaySource[i] = 0;
    iOverlaySource = cOverlaySources = 0;
    dxOverlaySource = dyOverlaySource = 0;
}

bool GL::FUpscaleFrame() const
{
    const DISPLAY& d = g_display;
    if (fNativeUi) return true;
    if (!window || !d.fUpscale || vr::FActive()) return false;
    return renderWidth < d.presentWidth && renderHeight < d.presentHeight && fsr::FAvailable();
}

void GL::SwapNativeUi()
{
    std::swap(fbo, fboUi);
    std::swap(fbc, texUi);
    std::swap(rbo, rboUi);
    std::swap(fboMsaa, fboMsaaScene);
    std::swap(renderWidth, dxUi);
    std::swap(renderHeight, dyUi);
    fNativeUi = !fNativeUi;
}

void GL::BeginNativeUi(bool fScene)
{
    if (fNativeUi || !FUpscaleFrame()) return;
    const DISPLAY& d = g_display;
    const int dx = d.presentWidth, dy = d.presentHeight;
    if (!fboUi || dxUi != dx || dyUi != dy)
    {
        ReleaseNativeUi();
        dxUi = dx, dyUi = dy;
        if (const cane::draw::Packets* pkf = PpacketsFbo())
        {
            // (native: nothing bound; every user binds the target itself)
            texUi = GlNewTexture(dx, dy, GL_LINEAR, false);
            cane::draw::RenderbufferDesc rd;
            rd.width = uint32_t(dx), rd.height = uint32_t(dy), rd.format = cane::draw::kRenderbufferDepth32fStencil8;
            rboUi = pkf->createRenderbuffer(rd);
            fboUi = GlNewFramebuffer(texUi, rboUi);
        }
        else
        {
        glGenFramebuffers(1, &fboUi);
        glBindFramebuffer(GL_FRAMEBUFFER, fboUi);
        texUi = GlNewTexture(dx, dy, GL_LINEAR, false);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texUi, 0);
        glGenRenderbuffers(1, &rboUi);
        glBindRenderbuffer(GL_RENDERBUFFER, rboUi);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, dx, dy);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboUi);
        }
    }
    const cane::draw::Packets* pk = PacketsActive();
    if (fScene)
    {
        if (fboMsaa && pk && pk->scissor && pk->copy)
        {
            // (draw packets: the scissor off; the framebuffer bound after it is FUpscale's to set)
            const int32_t full[4] = {0, 0, renderWidth, renderHeight};
            pk->scissor(nullptr);
            pk->copy(fboMsaa, fbo, full, 0, 0, fbo);
        }
        else if (fboMsaa)
        {
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMsaa);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        fsr::SOURCE src;
        src.tex = fbc;
        src.dx = renderWidth, src.dy = renderHeight;
        fsr::FUpscale(src, fboUi, 0, 0, dx, dy, fsr::GSharpness(d.upscaleSharpness));
    }
    SwapNativeUi();
    // (the HUD's depth test and the foreground's stencil start from a clear: hud.cpp, render.cpp)
    if (pk && pk->clear && pk->bindTarget)
    {
        // (draw packets: the depth / stencil write masks set as the GL calls set them, the colour mask and scissor as they are)
        cane::draw::Clear c;
        c.clear = cane::draw::kClearDepth | cane::draw::kClearStencil;
        c.depthWrite = 1;
        c.stencilWriteMask = c.stencilWriteMaskAfter = 0xFF;
        pk->bindTarget(fbo);
        pk->clear(c);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearDepth(0.0);
    glClearStencil(0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void GL::EndNativeUi()
{
    if (fNativeUi) SwapNativeUi();
}

void GL::ReleaseNativeUi()
{
    if (fboUi) { GlDeleteFramebuffers(1, &fboUi); GlDeleteTexture(texUi); GlDeleteRenderbuffer(rboUi); }
    fboUi = texUi = rboUi = 0;
    dxUi = dyUi = 0;
}

void GL::Present()
{
    if (vr::FPresent()) return;   // (vr.h: the picture to the headset, the window mirrors it)
    // (draw packets, platform/draw_packet.h: the Vulkan renderer; the GL-style state afterwards as the GL calls below leave
    // it: framebuffer 0 bound, the scissor test off, all colours written, the clear colour black)
    if (const cane::draw::Packets* pk = PacketsActive(); pk && pk->blit && pk->invalidate && pk->clear && pk->copy && pk->bindTarget)
    {
        using namespace cane::draw;
        if (glInvalidateFramebuffer) pk->invalidate(FboScene(), kClearDepth | kClearStencil);
        const int32_t full[4] = {0, 0, renderWidth, renderHeight};
        if (fboMsaa)
        {
            pk->copy(fboMsaa, fbo, full, 0, 0, fboMsaa);
            if (glInvalidateFramebuffer) pk->invalidate(fboMsaa, kClearColor);
        }
        pk->bindTarget(0);
        Clear c;
        c.clear = kClearColor;
        c.color[3] = 1.0f;
        c.colorMask = 0xF, c.scissor = 0;
        c.stencilMaskKept = 1;
        pk->clear(c);
        const DISPLAY& d = g_display;
        const int32_t dst[4] = {d.presentX, d.presentY, d.presentWidth, d.presentHeight};
        pk->blit(fbo, 0, full, dst, fNativeUi ? 0 : 1, 0);
        EndNativeUi();
        return;
    }
    // (perf-4) the scene's depth / stencil are not read after the frame (the next frame's passes clear them first): a tile
    // GPU need not write them back to memory (Zink: the render pass's store op DONT_CARE). GL 4.3 / GLES 3.0.
    if (glInvalidateFramebuffer)
    {
        static const GLenum s_aattDepth[] = {GL_DEPTH_STENCIL_ATTACHMENT};
        glBindFramebuffer(GL_FRAMEBUFFER, FboScene());
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, s_aattDepth);
    }
    if (fboMsaa)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMsaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        // (perf-5) the multisampled colour is not read after its resolve either: every drawn frame starts with a clear
        // (RenderPass, or the animated overlay's DrawAnimatedOverlayFrame); the overlay captures read it before this
        if (glInvalidateFramebuffer)
        {
            static const GLenum s_aattColor[] = {GL_COLOR_ATTACHMENT0};
            glBindFramebuffer(GL_FRAMEBUFFER, fboMsaa);
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, s_aattColor);
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // into the picture's rectangle of the window (black bars around it: display.h)
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const DISPLAY& d = g_display;
    // (upscaling: the native target is the picture's size, so this copies it one to one)
    glBlitFramebuffer(0, 0, renderWidth, renderHeight, d.presentX, d.presentY, d.presentX + d.presentWidth,
                      d.presentY + d.presentHeight, GL_COLOR_BUFFER_BIT, fNativeUi ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    EndNativeUi();
}

bool GL::FWriteScreenshot(const std::string& path)
{
    std::vector<uint8_t> ab(size_t(renderWidth) * size_t(renderHeight) * 4);
    // (draw packets: the resolve as copy (the read / draw bindings after it are the read-back's own), the read-back
    // natively, framebuffer 0 bound afterwards as the GL calls leave it)
    const cane::draw::Packets* pk = PacketsActive();
    if (pk && pk->copy && pk->readPixels)
    {
        const int32_t rect[4] = {0, 0, renderWidth, renderHeight};
        if (fboMsaa) pk->copy(fboMsaa, fbo, rect, 0, 0, fbo);
        pk->readPixels(fbo, rect, ab.data(), 0);
    }
    else
    {
        if (fboMsaa)
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMsaa);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, renderWidth, renderHeight, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    // bottom-up -> top-down, opaque
    std::vector<uint8_t> flipped(ab.size());
    const size_t row = size_t(renderWidth) * 4;
    for (int y = 0; y < renderHeight; ++y)
        std::memcpy(flipped.data() + size_t(y) * row, ab.data() + size_t(renderHeight - 1 - y) * row, row);
    for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
    return stbi_write_png(path.c_str(), renderWidth, renderHeight, 4, flipped.data(), static_cast<int>(row)) != 0;
}

void GL::Terminate()
{
    ShutdownPadRumble();   // (rumble.h: every motor off before the window goes)
    if (window) {
        EndNativeUi();
        ReleaseNativeUi();
        fsr::Release();
        ReleaseOverlayFrame();
        ResetAnimatedOverlayGraphics();
        glOverlayShader.Delete();
        cane::gl::DestroyContext(window);
    }
    if (window) glfwDestroyWindow(window);
    window = nullptr;
    glfwTerminate();
}
