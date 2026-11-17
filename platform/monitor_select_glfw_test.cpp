// The window mode on the chosen monitor (platform/monitor_select_glfw.h applyWindowMode / refreshMonitor) against a fake
// GLFW with two (then one, then two again) monitors: what the games ask the window system for, without a display.
// GLFW's own behaviour that matters is copied: a fullscreen window on a monitor that is unplugged becomes a window.
#include <GLFW/glfw3.h>

struct GLFWmonitor { const char* name; int x, y, w, h, hz; };
struct GLFWwindow { GLFWmonitor* monitor = nullptr; int x = 50, y = 60, w = 1280, h = 720; bool decorated = true; };

#include "monitor_select_glfw.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
// ---- the fake GLFW
GLFWmonitor g_laptop{ "eDP-1", 0, 0, 1920, 1080, 60 };
GLFWmonitor g_tv{ "LG TV", 1920, 0, 3840, 2160, 120 };
GLFWmonitor g_tvAgain{ "LG TV", 1920, 0, 3840, 2160, 120 };   // (plugged in again: GLFW makes a new monitor)
std::vector<GLFWmonitor*> g_monitors{ &g_laptop, &g_tv };
bool g_wayland = false;
GLFWmonitorfun g_monitorCallback = nullptr;
int g_setMonitorCalls = 0;
GLFWvidmode g_mode;

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}
void unplug(GLFWwindow& w, GLFWmonitor* m) {
    for (size_t i = 0; i < g_monitors.size(); ++i)
        if (g_monitors[i] == m) g_monitors.erase(g_monitors.begin() + std::ptrdiff_t(i));
    if (w.monitor == m) w.monitor = nullptr, w.x = 0, w.y = 0;   // (GLFW: _glfwInputMonitor, the window made windowed)
    if (g_monitorCallback) g_monitorCallback(m, GLFW_DISCONNECTED);
}
void plug(GLFWmonitor* m) {
    g_monitors.push_back(m);
    if (g_monitorCallback) g_monitorCallback(m, GLFW_CONNECTED);
}
}  // namespace

extern "C" {
GLFWmonitor** glfwGetMonitors(int* count) { *count = int(g_monitors.size()); return g_monitors.data(); }
GLFWmonitor* glfwGetPrimaryMonitor(void) { return g_monitors.empty() ? nullptr : g_monitors[0]; }
const char* glfwGetMonitorName(GLFWmonitor* m) { return m->name; }
void glfwGetMonitorPos(GLFWmonitor* m, int* x, int* y) { *x = g_wayland ? 0 : m->x; *y = g_wayland ? 0 : m->y; }
void glfwGetMonitorWorkarea(GLFWmonitor* m, int* x, int* y, int* w, int* h) {
    glfwGetMonitorPos(m, x, y);
    *w = m->w, *h = m->h - 40;   // (a task bar)
}
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* m) { g_mode = { m->w, m->h, 8, 8, 8, m->hz }; return &g_mode; }
const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* m, int* count) { *count = 1; return glfwGetVideoMode(m); }
GLFWmonitor* glfwGetWindowMonitor(GLFWwindow* w) { return w->monitor; }
void glfwGetWindowPos(GLFWwindow* w, int* x, int* y) { *x = g_wayland ? 0 : w->x; *y = g_wayland ? 0 : w->y; }
void glfwGetWindowSize(GLFWwindow* w, int* width, int* height) { *width = w->w, *height = w->h; }
void glfwSetWindowSize(GLFWwindow* w, int width, int height) { w->w = width, w->h = height; }
void glfwSetWindowPos(GLFWwindow* w, int x, int y) { if (!g_wayland) w->x = x, w->y = y; }
void glfwSetWindowAttrib(GLFWwindow* w, int attrib, int value) { if (attrib == GLFW_DECORATED) w->decorated = value != 0; }
void glfwSetWindowMonitor(GLFWwindow* w, GLFWmonitor* m, int x, int y, int width, int height, int) {
    ++g_setMonitorCalls;
    w->monitor = m;
    if (!m && !g_wayland) w->x = x, w->y = y;
    w->w = width, w->h = height;
}
GLFWmonitorfun glfwSetMonitorCallback(GLFWmonitorfun f) { GLFWmonitorfun old = g_monitorCallback; g_monitorCallback = f; return old; }
void glfwGetVersion(int* major, int* minor, int* rev) { *major = 3, *minor = 4, *rev = 0; }
int glfwGetPlatform(void) { return g_wayland ? GLFW_PLATFORM_WAYLAND : GLFW_PLATFORM_X11; }
}

int main() {
    namespace mo = cane::monitor;
    mo::watchConnections();
    const std::string tv = mo::idOf(mo::connected().list, 1);
    check(tv == "LG TV | 1 | 1920,0", "the TV's identifier: " + tv);
    {
        // X11 / Windows. A new window, Primary, windowed: nothing moves
        GLFWwindow w;
        mo::Placement p;
        mo::applyWindowMode(&w, p, 0, "");
        check(w.x == 50 && w.y == 60 && !w.monitor && g_setMonitorCalls == 0, "Primary, windowed: the window stays");
        // Fullscreen with the TV chosen: on the TV at its mode
        mo::applyWindowMode(&w, p, 2, tv);
        check(w.monitor == &g_tv && w.w == 3840 && w.h == 2160, "Fullscreen on the TV");
        check(mo::refreshMonitor(&w, 2, tv) == &g_tv, "the swap follows the TV's refresh rate");
        // nothing changed: nothing done
        const int calls = g_setMonitorCalls;
        check(!mo::applyWindowMode(&w, p, 2, tv) && g_setMonitorCalls == calls, "applied again: no change");
        // Borderless on the TV: a window over it without a frame
        mo::applyWindowMode(&w, p, 1, tv);
        check(!w.monitor && !w.decorated && w.x == 1920 && w.y == 0 && w.w == 3840 && w.h == 2160, "Borderless on the TV");
        check(mo::refreshMonitor(&w, 1, tv) == &g_tv, "Borderless: the TV's refresh rate");
        // Windowed again: back where it was (the place before Fullscreen)
        mo::applyWindowMode(&w, p, 0, tv);
        check(!w.monitor && w.decorated && w.x == 50 && w.y == 60 && w.w == 1280 && w.h == 720, "Windowed: back in its place");
        // the choice changed to the TV while windowed: moved to the middle of the TV's work area
        mo::Placement q;
        GLFWwindow v;
        mo::applyWindowMode(&v, q, 0, "");
        mo::applyWindowMode(&v, q, 0, tv);
        check(v.x == 1920 + (3840 - 1280) / 2 && v.y == (2160 - 40 - 720) / 2, "Windowed: moved onto the TV");
        check(mo::refreshMonitor(&v, 0, tv) == &g_tv, "Windowed on the TV: its refresh rate");
        // back to Primary: onto the laptop
        mo::applyWindowMode(&v, q, 0, "");
        check(v.x == (1920 - 1280) / 2, "Windowed: back onto the primary monitor");
        // a new window with the TV chosen opens there
        GLFWwindow fresh;
        mo::Placement r;
        mo::applyWindowMode(&fresh, r, 0, tv);
        check(fresh.x > 1920, "a new window with the TV chosen opens on it");
        // a window bigger than the monitor is made to fit
        GLFWwindow big;
        big.w = 2500, big.h = 1400, big.x = 2500;   // (on the TV)
        mo::Placement b;
        mo::applyWindowMode(&big, b, 0, "eDP-1 | 1 | 0,0");
        check(big.w == 1920 && big.h == 1040 && big.x == 0, "a window bigger than the monitor fits its work area");

        // Fullscreen on the TV, then the TV unplugged: GLFW makes the window a window; applied again on the primary one
        mo::applyWindowMode(&w, p, 2, tv);
        unplug(w, &g_tv);
        check(mo::connectionsChanged(p), "the unplug is noticed");
        check(mo::applyWindowMode(&w, p, 2, tv) && w.monitor == &g_laptop && w.w == 1920, "unplugged: Fullscreen on the primary monitor");
        check(!mo::connectionsChanged(p), "applied: up to date");
        // plugged in again (a new GLFWmonitor): back on the TV
        plug(&g_tvAgain);
        check(mo::connectionsChanged(p) && mo::applyWindowMode(&w, p, 2, tv) && w.monitor == &g_tvAgain, "plugged in again: back on the TV");
        // a monitor that is not the chosen one unplugged: nothing to do
        unplug(w, &g_laptop);
        check(mo::connectionsChanged(p) && !mo::applyWindowMode(&w, p, 2, tv) && w.monitor == &g_tvAgain, "another monitor unplugged: stays");
        plug(&g_laptop);
        // (the laptop is now listed second: still found by name)
        check(mo::chosen("eDP-1 | 1 | 0,0") == &g_laptop, "a monitor found by name in another order");
    }
    {
        // Wayland: no positions; Borderless is a fullscreen surface on the output; Windowed is not moved
        g_wayland = true;
        g_monitors = { &g_laptop, &g_tv };
        const std::string tvW = mo::idOf(mo::connected().list, 1);
        check(tvW == "LG TV | 1 | 0,0", "Wayland: positions 0,0");
        GLFWwindow w;
        mo::Placement p;
        mo::applyWindowMode(&w, p, 1, tvW);
        check(w.monitor == &g_tv && w.w == 3840, "Wayland: Borderless fills the TV as a fullscreen surface");
        mo::applyWindowMode(&w, p, 2, tvW);
        check(w.monitor == &g_tv, "Wayland: Fullscreen on the TV");
        mo::applyWindowMode(&w, p, 0, tvW);
        check(!w.monitor && w.decorated, "Wayland: Windowed (where the compositor puts it)");
        check(mo::refreshMonitor(&w, 0, tvW) == &g_tv, "Wayland, windowed: the chosen monitor's refresh rate");
        g_wayland = false;
    }
    if (failures == 0) std::printf("monitor-select-glfw: all passed\n");
    return failures == 0 ? 0 : 1;
}
