// The renderer choice and Vulkan's start: see renderer.h.
#include "renderer.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "gl_context.h"
#include "platform.h"
#include "renderer_choice.h"
#include "sly2_version.h"
#include "vr.h"
#if CANE_VULKAN
#include "vulkan/gl_vulkan.h"
#include "vulkan/vk_loader.h"
#endif

RENDERER g_rendererWanted = RENDERER::Auto;
RENDERER g_rendererSetting = RENDERER::Auto;

namespace
{
bool s_fVulkan = false;
std::vector<std::string> s_relaunchArgs;   // (SetRendererRelaunch: the command line, for a restart on OpenGL)

#if CANE_VULKAN
// The start (platform/renderer_choice.h): the marker is removed after kPresentsStarted frames over kSecondsStarted on
// Vulkan (a slow device draws few frames a second); a Vulkan error
// within the first kPresentsRestart frames restarts the game on OpenGL (Linux); any Vulkan error is remembered, so the
// next Auto start uses OpenGL.
constexpr unsigned kPresentsStarted = 30, kPresentsRestart = 600;
constexpr double kSecondsStarted = 10.0;
double s_tStart = 0;
bool s_fStarted = false;
unsigned s_cPresents = 0;
bool s_fMarker = false;
std::filesystem::path MarkerPath() { return cane::userDirectory() / "vulkan-start.txt"; }
void ClearMarker()
{
    if (s_fMarker) cane::renderer::clearMarker(MarkerPath());
    s_fMarker = false;
}
void RememberFailure(const std::string& why) { s_fMarker = cane::renderer::writeMarker(MarkerPath(), SLY2_PORT_VERSION, why); }

// (CaneVk::Options::onFatal: Vulkan stopped with an error it cannot recover from; the process ends after this)
void OnVulkanFatal(const char* why)
{
    RememberFailure(std::string("Vulkan error: ") + why);
#if !defined(_WIN32)
    if (s_cPresents < kPresentsRestart && !s_relaunchArgs.empty())
    {
        std::printf("Vulkan: %s: restarting on OpenGL\n", why);
        std::fflush(nullptr);
        setenv("SLY2N_RENDERER", "opengl", 1);
        std::vector<char*> argv;
        for (std::string& s : s_relaunchArgs) argv.push_back(s.data());
        argv.push_back(nullptr);
        execv("/proc/self/exe", argv.data());
        execvp(argv[0], argv.data());
        std::printf("Vulkan: the restart failed; OpenGL at the next start\n");
    }
#endif
    std::fflush(nullptr);
}

bool CreateContext(GLFWwindow*, std::string*) { return true; }
void DestroyContext(GLFWwindow*)
{
    CaneVk::Shutdown();
    s_fVulkan = false;
    if (!s_fStarted) ClearMarker();   // (a normal exit before the first frames: the start did not fail)
}
GLFWglproc ProcAddress(const char* name) { return reinterpret_cast<GLFWglproc>(CaneVk::ProcAddress(name)); }
void SwapInterval(int interval) { CaneVk::SetSwapInterval(interval); }
void SwapBuffers(GLFWwindow*)
{
    CaneVk::Present();
    if (s_cPresents < kPresentsRestart) ++s_cPresents;
    if (!s_fStarted && s_cPresents >= kPresentsStarted && glfwGetTime() - s_tStart >= kSecondsStarted)
    {
        s_fStarted = true;
        ClearMarker();
    }
}
const cane::gl::Backend s_backend{CreateContext, DestroyContext, ProcAddress, SwapInterval, SwapBuffers};

// The programs every frame draws with: Vulkan without them (SPIR-V that does not match the build) cannot run the game.
bool FProgramsComplete(std::string* missing)
{
    static const char* const programs[][2] = {{"glob.vert", "glob.frag"}, {"celborder.vert", "celborder.frag"},
                                              {"hud.vert", "hud.frag"}, {"overlay.vert", "overlay.frag"}};
    using PFNDELETEPROGRAM = void (*)(GLuint);
    auto pfnDelete = reinterpret_cast<PFNDELETEPROGRAM>(CaneVk::ProcAddress("glDeleteProgram"));
    for (const auto& p : programs)
    {
        const GLuint id = CaneVk::CreateProgram(p[0], nullptr, p[1], "");
        if (!id)
        {
            *missing = p[0];
            return false;
        }
        if (pfnDelete) pfnDelete(id);
    }
    return true;
}
#endif
}  // namespace

bool FParseRenderer(const char* sz, RENDERER* prenderer)
{
    if (!sz) return false;
    std::string s;
    for (const char* p = sz; *p; ++p)
        if (*p != ' ' && *p != '-' && *p != '_') s += char(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
    if (s == "auto" || s == "automatic" || s.rfind("auto(", 0) == 0) *prenderer = RENDERER::Auto;
    else if (s == "vulkan" || s == "vk") *prenderer = RENDERER::Vulkan;
    else if (s == "opengl" || s == "gl") *prenderer = RENDERER::OpenGL;
    else return false;
    return true;
}

bool FRendererVulkan() { return s_fVulkan; }
const char* SzRendererName() { return s_fVulkan ? "Vulkan" : "OpenGL"; }

void SetRendererRelaunch(int argc, char** argv) { s_relaunchArgs.assign(argv, argv + argc); }

GLFWwindow* PwindowVulkan(int width, int height, bool fVisible, const char* title)
{
    RENDERER r = g_rendererWanted;
    if (const char* sz = std::getenv("SLY2N_RENDERER"); sz && *sz && !FParseRenderer(sz, &r))
        std::printf("Renderer: SLY2N_RENDERER=%s not understood (auto, vulkan, opengl)\n", sz);
#if CANE_VULKAN
    constexpr bool fBuilt = true;
#else
    constexpr bool fBuilt = false;
#endif
    cane::renderer::Marker marker;
#if CANE_VULKAN
    if (r == RENDERER::Auto) marker = cane::renderer::readMarker(MarkerPath());
#endif
    const cane::renderer::Decision decision = cane::renderer::decide(
        cane::renderer::Choice(int(r)), fBuilt, r != RENDERER::OpenGL && vr::FRequested(), marker, SLY2_PORT_VERSION);
    if (!decision.tryVulkan)
    {
        if (!decision.why.empty()) std::printf("Renderer: OpenGL (%s)\n", decision.why.c_str());
        return nullptr;
    }
#if CANE_VULKAN
    // (no Vulkan loader: OpenGL before any window exists; nothing to remember, so a driver installed later is used)
    std::string error;
    if (!cane::vk::LoadLoader(&error))
    {
        std::printf("Vulkan: %s: OpenGL instead\n", error.c_str());
        return nullptr;
    }
    RememberFailure("");   // (the start in progress)
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, fVisible ? GLFW_TRUE : GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    glfwDefaultWindowHints();
    if (!window)
    {
        const char* message = nullptr;
        glfwGetError(&message);
        error = std::string("no window (") + (message ? message : "GLFW gave no reason") + ")";
        std::printf("Vulkan: %s: OpenGL instead\n", error.c_str());
        RememberFailure(error);
        return nullptr;
    }
    CaneVk::Options options;
    options.visible = fVisible;
    options.pipelineCachePath = cane::utf8(cane::userDirectory() / "vulkan-pipelines.bin");
    options.onFatal = OnVulkanFatal;
    std::string missing;
    bool fOk = CaneVk::Initialize(window, options, &error);
    if (fOk && !FProgramsComplete(&missing))
    {
        error = "the program " + missing + " is missing";
        CaneVk::Shutdown();
        fOk = false;
    }
    if (!fOk)
    {
        std::printf("Vulkan: %s: OpenGL instead\n", error.c_str());
        RememberFailure(error);
        glfwDestroyWindow(window);
        return nullptr;
    }
    cane::gl::ActiveBackend() = &s_backend;
    s_fVulkan = true;
    s_cPresents = 0;
    s_fStarted = false;
    s_tStart = glfwGetTime();
    // (a normal exit (exit(), no DestroyContext) in the first frames is not a failed start either)
    static bool s_fAtExit = false;
    if (!s_fAtExit) s_fAtExit = std::atexit([] { if (!s_fStarted) ClearMarker(); }) == 0;
    std::printf("Renderer: Vulkan (%s)\n", r == RENDERER::Auto ? "Auto" : "chosen");
    return window;
#else
    (void)width, (void)height, (void)fVisible, (void)title;
    return nullptr;
#endif
}

unsigned IdVulkanProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix)
{
#if CANE_VULKAN
    return CaneVk::CreateProgram(vertexFile, geometryFile, fragmentFile, prefix);
#else
    (void)vertexFile, (void)geometryFile, (void)fragmentFile, (void)prefix;
    return 0;
#endif
}
