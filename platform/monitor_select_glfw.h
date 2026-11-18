#pragma once
// The Monitor option with GLFW (platform/monitor_select.h): the connected monitors, the chosen one, window placement and
// connection changes.
#include <GLFW/glfw3.h>
#include "glfw_platform.h"
#include "monitor_select.h"

namespace cane::monitor {

// The connected monitors in GLFW's order, with handles (monitors[i] is list[i]).
struct Connected {
    std::vector<Info> list;
    std::vector<GLFWmonitor*> handles;
};
inline Connected connected() {
    Connected c;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    for (int i = 0; monitors && i < count; ++i) {
        Info m;
        if (const char* name = glfwGetMonitorName(monitors[i])) m.name = trim(name);
        glfwGetMonitorPos(monitors[i], &m.x, &m.y);
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]))
            m.width = mode->width, m.height = mode->height, m.refreshRate = mode->refreshRate;
        int modeCount = 0;
        const GLFWvidmode* modes = glfwGetVideoModes(monitors[i], &modeCount);
        for (int k = 0; modes && k < modeCount; ++k)
            if (modes[k].refreshRate > m.maxRefreshRate) m.maxRefreshRate = modes[k].refreshRate;
        if (m.maxRefreshRate < m.refreshRate) m.maxRefreshRate = m.refreshRate;
        m.primary = monitors[i] == primary;
        c.list.push_back(m);
        c.handles.push_back(monitors[i]);
    }
    return c;
}
// The monitor to play on: the stored one when connected, else the primary one (null without a monitor).
inline GLFWmonitor* chosen(const std::string& id) {
    const Connected c = connected();
    const int i = resolve(c.list, id);
    return i >= 0 ? c.handles[size_t(i)] : glfwGetPrimaryMonitor();
}
inline int indexOfHandle(const Connected& c, GLFWmonitor* m) {
    for (size_t i = 0; i < c.handles.size(); ++i)
        if (c.handles[i] == m) return int(i);
    return -1;
}
// The window system cannot place windows (Wayland).
inline bool cannotPlaceWindows() {
    return onWayland();
}
// The monitor holding the window's centre (its own when fullscreen; null when unknown: Wayland's windowed windows).
inline GLFWmonitor* holding(GLFWwindow* window) {
    if (!window) return nullptr;
    if (GLFWmonitor* m = glfwGetWindowMonitor(window)) return m;
    if (cannotPlaceWindows()) return nullptr;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    const int cx = wx + ww / 2, cy = wy + wh / 2;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; monitors && i < count; ++i) {
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (mode && cx >= mx && cx < mx + mode->width && cy >= my && cy < my + mode->height) return monitors[i];
    }
    return nullptr;
}
// The window (not fullscreen) moved to the middle of monitor's work area, at most its size; false where windows cannot
// be placed (Wayland).
inline bool centreWindowOn(GLFWwindow* window, GLFWmonitor* monitor, int* px = nullptr, int* py = nullptr) {
    if (!window || !monitor || cannotPlaceWindows()) return false;
    int x = 0, y = 0, w = 0, h = 0;
    glfwGetMonitorWorkarea(monitor, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return false;
    int ww = 0, wh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    if (ww > w || wh > h) {
        ww = ww > w ? w : ww, wh = wh > h ? h : wh;
        glfwSetWindowSize(window, ww, wh);
    }
    const int nx = x + (w - ww) / 2, ny = y + (h - wh) / 2;
    glfwSetWindowPos(window, nx, ny);
    if (px) *px = nx;
    if (py) *py = ny;
    return true;
}
// The point (x, y) lies on monitor.
inline bool onMonitor(GLFWmonitor* monitor, int x, int y) {
    if (!monitor) return false;
    int mx = 0, my = 0;
    glfwGetMonitorPos(monitor, &mx, &my);
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    return mode && x >= mx && x < mx + mode->width && y >= my && y < my + mode->height;
}

// Connections and disconnections: a counter that changes with each (the games re-apply the window mode: a chosen monitor
// unplugged falls back to the primary one, and comes back when it is plugged in again). Installs GLFW's monitor callback
// once, keeping one installed before it.
inline unsigned& changeCounter() {
    static unsigned n = 0;
    return n;
}
inline void watchConnections() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    static GLFWmonitorfun previous = nullptr;
    previous = glfwSetMonitorCallback([](GLFWmonitor* m, int event) {
        ++changeCounter();
        if (previous) previous(m, event);
    });
}

// ------------------------------------------------------------ the window mode on the chosen monitor
// (Sly 2 / Sly 3 display.cpp ApplyDisplaySettings; Sly 1 gl.cpp ApplyWindowModeSettings keeps its own error handling
// around the same steps.) Modes: 0 Windowed, 1 Borderless, 2 Fullscreen (system_settings.h WindowModeValue).
struct Placement {
    int mode = 0;                    // the mode applied (a new window is windowed)
    int x = 100, y = 100, width = 1280, height = 960;   // the windowed window's place, kept while it is not windowed
    GLFWmonitor* monitor = nullptr;  // the monitor applied on
    std::string id;                  // the identifier applied
    bool applied = false;            // applied once
    unsigned connections = 0;        // changeCounter() when applied
};

// Borderless and Fullscreen on that monitor; Windowed back in its place, moved onto the chosen monitor when that choice
// changed (or first applied with a monitor chosen and the window elsewhere). Wayland: Borderless is a fullscreen
// surface on the output (a window cannot be placed there, and Wayland never changes an output's mode), Windowed opens
// where the compositor puts it. force: apply even when nothing seems to have changed. Returns true when the window was
// changed (the caller re-reads its framebuffer size and refresh rate).
inline bool applyWindowMode(GLFWwindow* window, Placement& p, int mode, const std::string& id, bool force = false) {
    if (!window) return false;
    GLFWmonitor* target = chosen(id);
    if (!target) return false;
    const bool wayland = cannotPlaceWindows();
    const bool onOutput = mode == 2 || (mode == 1 && wayland);   // (a fullscreen surface)
    const bool idChanged = p.applied && id != p.id;
    // (the window not as applied: GLFW takes a fullscreen window off a monitor that was unplugged)
    const bool stale = onOutput ? glfwGetWindowMonitor(window) != target : glfwGetWindowMonitor(window) != nullptr;
    if (!force && p.applied && mode == p.mode && target == p.monitor && !idChanged && !stale) {
        p.connections = changeCounter();
        return false;
    }
    if (p.mode == 0 && !glfwGetWindowMonitor(window)) {
        glfwGetWindowPos(window, &p.x, &p.y);
        glfwGetWindowSize(window, &p.width, &p.height);
    }
    const GLFWvidmode* vm = glfwGetVideoMode(target);
    if (mode == 0) {
        if (p.mode != 0 || glfwGetWindowMonitor(window)) {
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowMonitor(window, nullptr, p.x, p.y, p.width < 320 ? 320 : p.width, p.height < 240 ? 240 : p.height, GLFW_DONT_CARE);
        }
        // (onto the chosen monitor: a new choice, or the first time with one chosen and the window on another)
        const bool move = idChanged || p.monitor != target || (!p.applied && !id.empty());
        if (move && !wayland) {
            GLFWmonitor* now = holding(window);
            if (now != target && centreWindowOn(window, target, &p.x, &p.y)) glfwGetWindowSize(window, &p.width, &p.height);
        }
    } else if (!vm) {
        return false;
    } else if (onOutput) {
        if (mode == 2) glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(window, target, 0, 0, vm->width, vm->height, mode == 2 ? vm->refreshRate : GLFW_DONT_CARE);
    } else {
        // Borderless: the whole monitor, taskbar included, without a mode change (on Windows the frames go through a DXGI
        // swap chain, platform/gl_context.h, so focus changes do not blank it)
        int x = 0, y = 0;
        glfwGetMonitorPos(target, &x, &y);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowMonitor(window, nullptr, x, y, vm->width, vm->height, GLFW_DONT_CARE);
    }
    p.mode = mode;
    p.monitor = target;
    p.id = id;
    p.applied = true;
    p.connections = changeCounter();
    return true;
}
// A monitor was connected or disconnected since the window mode was applied.
inline bool connectionsChanged(const Placement& p) { return p.connections != changeCounter(); }

// The monitor whose refresh rate the swap follows: the window's own when fullscreen, the chosen one for Borderless,
// else the one holding the window (the chosen one where that is unknown: Wayland).
inline GLFWmonitor* refreshMonitor(GLFWwindow* window, int mode, const std::string& id) {
    if (window)
        if (GLFWmonitor* m = glfwGetWindowMonitor(window)) return m;
    if (mode != 0 || !window) return chosen(id);
    if (GLFWmonitor* m = holding(window)) return m;
    return chosen(id);
}

} // namespace cane::monitor
