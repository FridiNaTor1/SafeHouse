// The GL layer, adapted from Sly1-Release gl.cpp (InitGL, CreateFramebuffers) and glshaders.cpp (embedded shader
// sources, compile errors).

#include "gl.h"
#include "pcinput.h"

#include "display.h"
#include "overlay.h"
#include "embedded_shaders.h"
#include "controller_family.h"
#include "gamepad_mappings.h"
#include "glfw_platform.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

GL g_gl;
GLSHADER glGlobShader;
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
    return source;
}
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
}

bool GL::FInit(int w, int h, bool fVisible, const char* title, std::string& error)
{
    if (!cane::initGlfw()) { error = "glfwInit failed (no display?)"; return false; }
    cane::input::installJoystickCallback();
    cane::input::applyBundledMappings();  // Steam passes its virtual pads' mappings in SDL_GAMECONTROLLERCONFIG
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
    glfwWindowHint(GLFW_VISIBLE, fVisible ? GLFW_TRUE : GLFW_FALSE);
    width = w;
    height = h;
    window = glfwCreateWindow(w, h, title, nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        error = "OpenGL 4.5 is required: could not create a graphics context";
        return false;
    }
    if (!cane::gl::CreateContext(window, &error)) return false;
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(cane::gl::ProcAddress))) { error = "glad failed"; return false; }
    std::printf("OpenGL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    StartupPcInput();   // (the prompt icons' atlases: pcinput.h / promptglyph.h)
    cane::gl::SwapInterval(1);
    // Reversed Z in [0, 1]: the GS's depth tests (GREATER / GEQUAL, larger = nearer) map one to one.
    // (glad here is the GL 4.3 loader: fetch glClipControl, GL 4.5 / ARB_clip_control, directly.)
    using PFNCLIPCONTROL = void (*)(GLenum, GLenum);
    auto pfnClipControl = reinterpret_cast<PFNCLIPCONTROL>(glfwGetProcAddress("glClipControl"));
    if (!pfnClipControl) { error = "glClipControl (GL 4.5) is required"; return false; }
    constexpr GLenum GL_ZERO_TO_ONE_ = 0x935F;
    pfnClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE_);
    CreateFramebuffers(w, h);
    // The picture / scene target for the display options (aspect, internal resolution, MSAA).
    int fbw = w, fbh = h;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    LayoutDisplay(fbw > 0 ? fbw : w, fbh > 0 ? fbh : h);
    glGlobShader.Init("glob.vert", "glob.frag");
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
    if (fboMsaa)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMsaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
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
    if (window) {
        ReleaseOverlayFrame();
        ResetAnimatedOverlayGraphics();
        glOverlayShader.Delete();
    }
    if (window)
    {
        cane::gl::DestroyContext(window);
        glfwDestroyWindow(window);
    }
    window = nullptr;
    glfwTerminate();
}
