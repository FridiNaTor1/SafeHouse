#pragma once
// glfwInit() that prefers X11 (XWayland) over native Wayland on Linux.
//
// Steam Input picks a game's controller configuration from the focused window.
// Steam is an X11 client, so it cannot see native Wayland windows: under a
// Wayland desktop session (KDE Plasma) a GLFW window created through Wayland
// leaves the controller in Steam's desktop configuration (mouse/keyboard),
// even for a game launched from Steam. An XWayland window is recognised.
// ThiefRush's builds have no Wayland backend at all, which is why they work.
//
// CANE_GLFW_PLATFORM=wayland|x11|any overrides the choice. If X11 cannot be
// initialised (no DISPLAY, e.g. inside a sandbox), GLFW picks on its own.
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <cstring>
// The games see Libraries/include's GLFW 3.3 header ahead of the 3.4 one they
// link, so the 3.4 platform API is declared here when the header lacks it, and
// used only when the linked library reports 3.4 or later.
#if defined(__linux__) && !defined(__ANDROID__) && !defined(GLFW_PLATFORM)
#define GLFW_PLATFORM 0x00050003
#define GLFW_ANY_PLATFORM 0x00060000
#define GLFW_PLATFORM_WAYLAND 0x00060003
#define GLFW_PLATFORM_X11 0x00060004
extern "C" int glfwPlatformSupported(int platform);
#endif
namespace cane {
inline bool initGlfw() {
#if defined(__linux__) && !defined(__ANDROID__)
    int major = 0, minor = 0, revision = 0;
    glfwGetVersion(&major, &minor, &revision);
    const char* choice = std::getenv("CANE_GLFW_PLATFORM");
    const char* display = std::getenv("DISPLAY");
    int platform = GLFW_PLATFORM_X11;
    if (choice && !std::strcmp(choice, "wayland")) platform = GLFW_PLATFORM_WAYLAND;
    else if ((choice && !std::strcmp(choice, "any")) || !display || !*display) platform = GLFW_ANY_PLATFORM;
    if ((major > 3 || (major == 3 && minor >= 4)) && platform != GLFW_ANY_PLATFORM) {
        if (glfwPlatformSupported(platform)) {
            glfwInitHint(GLFW_PLATFORM, platform);
            if (glfwInit()) return true;
        }
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
    }
#endif
    return glfwInit() == GLFW_TRUE;
}
} // namespace cane
