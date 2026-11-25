// A game's window on Vulkan: see vk_game.h. The start / fallback rules follow Sly 3's renderer.cpp (no VR here).
#include "vk_game.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "../gl_backend.h"
#include "../platform.h"
#include "../renderer_choice.h"
#if CANE_VULKAN
#include "gl_vulkan.h"
#include "vk_loader.h"
#include "custom_driver_watch.h"
#endif
#if defined(__ANDROID__)
#include <stdexcept>
#include "../android/android_app.h"
#endif

#ifndef CANE_GAME_VERSION
#define CANE_GAME_VERSION "dev"
#endif

namespace cane::vkgame {
namespace {
bool s_active = false;
std::vector<std::string> s_relaunchArgs;
const char* s_envRenderer = nullptr;

#if CANE_VULKAN
// The marker is removed after kPresentsStarted frames over kSecondsStarted on Vulkan; a Vulkan error within the first
// kPresentsRestart frames restarts the game on OpenGL (Linux desktops); any Vulkan error is remembered.
constexpr unsigned kPresentsStarted = 30, kPresentsRestart = 600;
constexpr double kSecondsStarted = 10.0;
double s_tStart = 0;
bool s_started = false;
unsigned s_presents = 0;
bool s_marker = false;
std::string s_version;

std::filesystem::path MarkerPath() { return cane::userDirectory() / "vulkan-start.txt"; }
void ClearMarker()
{
    if (s_marker) cane::renderer::clearMarker(MarkerPath());
    cane::vk::NoteStartFinished();   // (a custom Android driver started: kept, vk_loader.h)
    s_marker = false;
}
void RememberFailure(const std::string& why)
{
    s_marker = cane::renderer::writeMarker(MarkerPath(), s_version, why);
    cane::vk::NoteStartFailed(why);   // (a custom Android driver that failed: the system's driver next time, vk_loader.h)
}

void OnFatal(const char* why)
{
    // (2026-11-24: not one long after the start, as Sly 2's renderer.cpp: only a start that fails, or Vulkan failing
    // within its first kPresentsRestart frames, sends the next Auto starts to OpenGL)
    if (!s_started || s_presents < kPresentsRestart) RememberFailure(std::string("Vulkan error: ") + why);
    else std::printf("Vulkan: %s (after the start: not remembered)\n", why);
#if !defined(_WIN32) && !defined(__ANDROID__)
    if (s_presents < kPresentsRestart && !s_relaunchArgs.empty() && s_envRenderer)
    {
        std::printf("Vulkan: %s: restarting on OpenGL\n", why);
        std::fflush(nullptr);
        setenv(s_envRenderer, "opengl", 1);
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
    s_active = false;
    if (!s_started) ClearMarker();
}
GLFWglproc ProcAddress(const char* name) { return reinterpret_cast<GLFWglproc>(CaneVk::ProcAddress(name)); }
void SwapInterval(int interval) { CaneVk::SetSwapInterval(interval); }
void SwapBuffers(GLFWwindow*)
{
    cane::vk::WatchCustomDriverPicture(glfwGetTime());   // (a custom driver that shows nothing: custom_driver_watch.h)
    CaneVk::Present();
    if (s_presents < kPresentsRestart) ++s_presents;
    if (!s_started && s_presents >= kPresentsStarted && glfwGetTime() - s_tStart >= kSecondsStarted)
    {
        s_started = true;
        ClearMarker();
    }
}
cane::gl::Backend s_backend{CreateContext, DestroyContext, ProcAddress, SwapInterval, SwapBuffers};

bool ProgramsComplete(const Config& config, std::string* missing)
{
    using PFNDELETEPROGRAM = void (*)(GLuint);
    auto pfnDelete = reinterpret_cast<PFNDELETEPROGRAM>(CaneVk::ProcAddress("glDeleteProgram"));
    for (int i = 0; i < config.programCount; ++i)
    {
        const GLuint id = CaneVk::CreateProgram(config.programs[i][0], nullptr, config.programs[i][1], "");
        if (!id)
        {
            *missing = config.programs[i][0];
            return false;
        }
        if (pfnDelete) pfnDelete(id);
    }
    return true;
}
#endif
}  // namespace

bool ParseChoice(const char* text, Choice* choice)
{
    if (!text) return false;
    std::string s;
    for (const char* p = text; *p; ++p)
        if (*p != ' ' && *p != '-' && *p != '_') s += char(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
    if (s == "auto" || s == "automatic" || s.rfind("auto(", 0) == 0) *choice = Choice::Auto;
    else if (s == "vulkan" || s == "vk") *choice = Choice::Vulkan;
    else if (s == "opengl" || s == "gl" || s == "gles" || s == "opengles") *choice = Choice::OpenGL;
    else return false;
    return true;
}

void SetRelaunch(int argc, char** argv) { s_relaunchArgs.assign(argv, argv + argc); }
bool Active() { return s_active; }
const char* Name() { return s_active ? "Vulkan" : "OpenGL"; }

const char* DeviceName()
{
#if CANE_VULKAN
    if (s_active) return CaneVk::DeviceName();
#endif
    return "";
}

unsigned Program(const char* vertexFile, const char* geometryFile, const char* fragmentFile)
{
#if CANE_VULKAN
    if (s_active) return CaneVk::CreateProgram(vertexFile, geometryFile, fragmentFile, "");
#endif
    (void)vertexFile, (void)geometryFile, (void)fragmentFile;
    return 0;
}

namespace {
GLFWwindow* OpenWindowOnce(int width, int height, bool visible, const char* title, Choice wanted, const Config& config,
                           std::string& error)
{
    s_envRenderer = config.envRenderer;
    if (config.envRenderer)
        if (const char* sz = std::getenv(config.envRenderer); sz && *sz && !ParseChoice(sz, &wanted))
            std::printf("Renderer: %s=%s not understood (auto, vulkan, opengl)\n", config.envRenderer, sz);
#if defined(__ANDROID__)
    // (Android: the fallback is OpenGL ES where the game has that path (Config::glesFallback, Sly 1's gles_sly1). Without
    // it Vulkan whatever the setting says: the game's OpenGL is desktop OpenGL 4.3, which a phone or a headset does not
    // have, so a failed start is an error the game reports, not an OpenGL window.)
    const char* const fallback = config.glesFallback ? "OpenGL ES" : nullptr;
    if (!config.glesFallback) wanted = Choice::Vulkan;
#else
    const char* const fallback = "OpenGL";
#endif
    // (the log line of a failed Vulkan start: what comes instead, or nothing when nothing does)
    auto instead = [fallback]() { return std::string(fallback ? std::string(": ") + fallback + " instead" : std::string()); };
#if CANE_VULKAN
    constexpr bool built = true;
#else
    constexpr bool built = false;
#endif
#if CANE_VULKAN
    s_version = config.version && *config.version ? config.version : CANE_GAME_VERSION;
    // (the start marker's rules and counts, platform/renderer_choice.h decideAt)
    const cane::renderer::Decision decision =
        cane::renderer::decideAt(cane::renderer::Choice(int(wanted)), built, MarkerPath(), s_version);
#else
    const cane::renderer::Decision decision =
        cane::renderer::decide(cane::renderer::Choice(int(wanted)), built, cane::renderer::Marker(),
                               config.version && *config.version ? config.version : CANE_GAME_VERSION);
#endif
    if (decision.tryVulkan && !decision.why.empty()) std::printf("Renderer: %s\n", decision.why.c_str());
    if (!decision.tryVulkan)
    {
        error = decision.why;
        std::printf("Renderer: %s%s%s%s\n", fallback ? fallback : "none", decision.why.empty() ? "" : " (", decision.why.c_str(),
                    decision.why.empty() ? "" : ")");
        return nullptr;
    }
#if CANE_VULKAN
    if (!cane::vk::LoadLoader(&error))
    {
        std::printf("Vulkan: %s%s\n", error.c_str(), instead().c_str());
        return nullptr;
    }
    RememberFailure("");   // (the start in progress)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    auto fallBack = [&](GLFWwindow* made) -> GLFWwindow* {
        std::printf("Vulkan: %s%s\n", error.c_str(), instead().c_str());
        RememberFailure(error);
        if (made) glfwDestroyWindow(made);
        return nullptr;
    };
    if (!window)
    {
        const char* message = nullptr;
        glfwGetError(&message);
        error = std::string("no window (") + (message ? message : "GLFW gave no reason") + ")";
        return fallBack(nullptr);
    }
    CaneVk::Options options;
    options.visible = visible;
    options.pipelineCachePath = cane::utf8(cane::userDirectory() / "vulkan-pipelines.bin");
    options.onFatal = OnFatal;
    // (Sly 1's per-frame streams written in place: gl.cpp CreateStreamBuffer; tests: SLY1_VKNOPERSIST=1, glBufferSubData)
    options.persistentMaps = !(std::getenv("SLY1_VKNOPERSIST") && std::atoi(std::getenv("SLY1_VKNOPERSIST")) != 0);
    if (const char* sz = std::getenv("SLY2N_VKFORCEFAIL"); sz && std::atoi(sz) != 0)
    {
        error = "SLY2N_VKFORCEFAIL is set";
        return fallBack(window);
    }
    if (!CaneVk::Initialize(window, options, &error)) return fallBack(window);
    std::string missing;
    if (!ProgramsComplete(config, &missing))
    {
        error = "the program " + missing + " is missing";
        CaneVk::Shutdown();
        return fallBack(window);
    }
    cane::gl::ActiveBackend() = &s_backend;
    s_active = true;
    s_presents = 0;
    s_started = false;
    s_tStart = glfwGetTime();
    static bool s_atExit = false;
    if (!s_atExit) s_atExit = std::atexit([] { if (!s_started) ClearMarker(); }) == 0;
    std::printf("Renderer: Vulkan (%s, %s)\n", wanted == Choice::Auto ? "Auto" : "chosen", CaneVk::DeviceName());
    return window;
#else
    (void)width, (void)height, (void)visible, (void)title, (void)instead;
    error = "this build has no Vulkan renderer";
    return nullptr;
#endif
}
}  // namespace

GLFWwindow* OpenWindow(int width, int height, bool visible, const char* title, Choice wanted, const Config& config)
{
    std::string error;
    GLFWwindow* window = OpenWindowOnce(width, height, visible, title, wanted, config, error);
#if defined(__ANDROID__)
    if (!window && !config.glesFallback)
        throw std::runtime_error("Vulkan could not start on this device (" + error + "). This game needs Vulkan here.");
#if CANE_VULKAN
    // (the activity's surface goes away while the app is in the background and comes back as a new one on resume)
    if (window) CaneVk::SetSurfaceSerial(cane::android::windowSurfaceSerial);
#endif
#endif
    return window;
}

}  // namespace cane::vkgame
