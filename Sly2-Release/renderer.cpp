// The renderer choice and Vulkan's start: see renderer.h.
#include "renderer.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gl_context.h"
#include "platform.h"
#include "vr.h"
#if CANE_VULKAN
#include "vulkan/gl_vulkan.h"
#endif

RENDERER g_rendererWanted = RENDERER::Auto;
RENDERER g_rendererSetting = RENDERER::Auto;

namespace
{
bool s_fVulkan = false;
// Auto: Vulkan where it starts once the comparison harness shows the same pictures and it is faster on the measured
// scenes (notes/VULKAN_2026-11-10.md); until then Auto is OpenGL and Vulkan is opt-in.
constexpr bool kAutoPrefersVulkan = false;

#if CANE_VULKAN
bool CreateContext(GLFWwindow*, std::string*) { return true; }
void DestroyContext(GLFWwindow*)
{
    CaneVk::Shutdown();
    s_fVulkan = false;
}
GLFWglproc ProcAddress(const char* name) { return reinterpret_cast<GLFWglproc>(CaneVk::ProcAddress(name)); }
void SwapInterval(int interval) { CaneVk::SetSwapInterval(interval); }
void SwapBuffers(GLFWwindow*) { CaneVk::Present(); }
const cane::gl::Backend s_backend{CreateContext, DestroyContext, ProcAddress, SwapInterval, SwapBuffers};
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

GLFWwindow* PwindowVulkan(int width, int height, bool fVisible, const char* title)
{
#if CANE_VULKAN
    RENDERER r = g_rendererWanted;
    if (const char* sz = std::getenv("SLY2N_RENDERER"); sz && *sz && !FParseRenderer(sz, &r))
        std::printf("Renderer: SLY2N_RENDERER=%s not understood (auto, vulkan, opengl)\n", sz);
    if (r == RENDERER::OpenGL || (r == RENDERER::Auto && !kAutoPrefersVulkan)) return nullptr;
    if (vr::FRequested())
    {
        std::printf("Renderer: VR runs on OpenGL\n");
        return nullptr;
    }
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, fVisible ? GLFW_TRUE : GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    glfwDefaultWindowHints();
    if (!window)
    {
        const char* message = nullptr;
        glfwGetError(&message);
        std::printf("Vulkan: no window (%s): OpenGL instead\n", message ? message : "GLFW gave no reason");
        return nullptr;
    }
    CaneVk::Options options;
    options.visible = fVisible;
    options.pipelineCachePath = cane::utf8(cane::userDirectory() / "vulkan-pipelines.bin");
    std::string error;
    if (!CaneVk::Initialize(window, options, &error))
    {
        std::printf("Vulkan: %s: OpenGL instead\n", error.c_str());
        glfwDestroyWindow(window);
        return nullptr;
    }
    cane::gl::ActiveBackend() = &s_backend;
    s_fVulkan = true;
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
