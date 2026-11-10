// The GL layer, adapted from Sly1-Release gl.cpp (InitGL, CreateFramebuffers) and glshaders.cpp (embedded shader
// sources, compile errors).

#include "rumble.h"
#include "gl.h"

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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

GL g_gl;
GLSHADER glGlobShader;
GLSHADER glGlobShaderNoKill;
GLSHADER glGlobShaderNoGeom, glGlobShaderNoKillNoGeom;
bool g_fGlobNoGeom = false;
bool g_fGlobBindless = false;
bool g_fGlobArrays = false;
bool g_fDrawUniform = false;
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
    if (g_fMultiviewPass && !g_fMultiviewFailed)
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
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &fOk);
        if (!fOk)
        {
            glGetProgramInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "shader link error (%s):\n%s\n", file, log);
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
    // are fetched by name, the desktop-only ones point at ES equivalents (gles_sly2.cpp).
    if (!cane::gles::FillGlad(error)) return false;
    g_fDrawUniform = !cane::gles::HasBaseInstance();
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
    auto initGlob = [&](const char* szPrefix) {
        globPrefix = szPrefix;
        fGlobGeom = s_fNearClip && glGlobShader.FInit("glob.vert", "glob.geom", "glob.frag", (globPrefix + "#define GEOM 1\n").c_str());
        return fGlobGeom || glGlobShader.FInit("glob.vert", "glob.frag", szPrefix);
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
    ReleaseOverlaySources();  // the immutable photograph survives resizing its presentation target
    renderWidth = w;
    renderHeight = h;
    if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &fbc); glDeleteRenderbuffers(1, &rbo); }
    if (fboMsaa) { glDeleteFramebuffers(1, &fboMsaa); glDeleteRenderbuffers(1, &rboColorMsaa); glDeleteRenderbuffers(1, &rboDepthMsaa); fboMsaa = 0; }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &fbc);
    glBindTexture(GL_TEXTURE_2D, fbc);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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

// orig: 0x16FB78/0x16FCA8: retain the displayed picture separately from the one being drawn.
void GL::RememberOverlaySourceFrame()
{
    if (!window) return;
    if (!fboOverlaySource[0]) {
        glGenFramebuffers(2, fboOverlaySource);
        glGenTextures(2, texOverlaySource);
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, texOverlaySource[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderWidth, renderHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlaySource[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texOverlaySource[i], 0);
        }
    }
    // Resolve first: multisample resolves cannot also scale the picture into the atlas.
    glDisable(GL_SCISSOR_TEST);
    if (fboMsaa) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMsaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlaySource[iOverlaySource]);
    glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    iOverlaySource ^= 1;
    cOverlaySources = std::min(cOverlaySources + 1, 2);
    glBindFramebuffer(GL_FRAMEBUFFER, FboScene());
}

// orig: 0x1475C0: previous picture in the left atlas half; current picture in the right.
bool GL::CaptureOverlayFrame()
{
    if (!window || cOverlaySources == 0) return false;
    if (!fboOverlay) {
        glGenFramebuffers(1, &fboOverlay);
        glGenTextures(1, &texOverlay);
        glBindTexture(GL_TEXTURE_2D, texOverlay);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderWidth, renderHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlay);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texOverlay, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    // The original's picture is its whole 4:3 frame; the overlay draws it in the 4:3 UI canvas. At other aspects
    // the native view is wider (or taller) than that frame, so the picture is the centred 4:3 part of the render,
    // not the whole render squeezed into it (U31: squashed at 32:9).
    int dxSrc = renderWidth, dySrc = renderHeight;
    if (dxSrc * 3 > dySrc * 4) dxSrc = std::max(1, int(std::lround(dySrc * 4.0 / 3.0)));
    else if (dxSrc * 3 < dySrc * 4) dySrc = std::max(1, int(std::lround(dxSrc * 3.0 / 4.0)));
    const int xSrc = (renderWidth - dxSrc) / 2, ySrc = (renderHeight - dySrc) / 2;
    for (int i = 0; i < 2; ++i) {
        const int source = i == 0 && cOverlaySources == 2 ? iOverlaySource : iOverlaySource ^ 1;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboOverlaySource[source]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOverlay);
        glBlitFramebuffer(xSrc, ySrc, xSrc + dxSrc, ySrc + dySrc, i * renderWidth / 2, 0,
                          (i + 1) * renderWidth / 2, renderHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, FboScene());
    return true;
}

void GL::ReleaseOverlayFrame()
{
    if (!window) return;
    glDeleteFramebuffers(1, &fboOverlay); glDeleteTextures(1, &texOverlay);
    fboOverlay = texOverlay = 0;
    ReleaseOverlaySources();
}

void GL::ReleaseOverlaySources()
{
    if (!window) return;
    glDeleteFramebuffers(2, fboOverlaySource); glDeleteTextures(2, texOverlaySource);
    for (int i = 0; i < 2; ++i) fboOverlaySource[i] = texOverlaySource[i] = 0;
    iOverlaySource = cOverlaySources = 0;
}

void GL::Present()
{
    if (vr::FPresent()) return;   // (vr.h: the picture to the headset, the window mirrors it)
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
    glBlitFramebuffer(0, 0, renderWidth, renderHeight, d.presentX, d.presentY, d.presentX + d.presentWidth,
                      d.presentY + d.presentHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool GL::FWriteScreenshot(const std::string& path)
{
    if (fboMsaa)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMsaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    std::vector<uint8_t> ab(size_t(renderWidth) * size_t(renderHeight) * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, renderWidth, renderHeight, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
        ReleaseOverlayFrame();
        ResetAnimatedOverlayGraphics();
        glOverlayShader.Delete();
        cane::gl::DestroyContext(window);
    }
    if (window) glfwDestroyWindow(window);
    window = nullptr;
    glfwTerminate();
}
