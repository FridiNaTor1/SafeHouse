// See display.h. Adapted from Sly1-Release gl.cpp (ApplyAspectRatioSettings, ApplyWindowModeSettings,
// FrameBufferSizeCallBack) and cm.cpp (RecalcCm's aspect modes).

#include "display.h"

#include "gl.h"
#include "pcinput.h"
#include "sound.h"

#include "../platform/system_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

DISPLAY g_display;

namespace
{
namespace st = cane::settings;

void FromSettings(const st::SystemSettings& s)
{
    g_display.aspect = DISPASPECT(std::clamp(s.aspectMode, 0, 5));
    g_display.internalHeight = s.internalResolutionHeight;
    g_display.msaaSamples = s.msaaEnabled ? s.msaaSamples : 0;
    g_display.frameRate = std::max(30, s.frameRate);
    g_display.drawDistance = s.drawDistance;
    g_display.window = DISPWINDOW(std::clamp(s.windowMode, 0, 2));
    g_display.fVsync = s.vsyncEnabled != 0;
    g_display.fLongShadows = s.longShadows != 0;   // (v16, QoL: render.cpp FBuildShadow2)
    PcInputFromSettings(s);   // (the controls and the prompts: pcinput.h)
    SetSoundUserVolumes(s.musicVolume, s.soundEffectsVolume, s.dialogueVolume);   // (the launcher's sliders: sound.h)
}

void ToSettings(st::SystemSettings& s)
{
    s.aspectMode = int(g_display.aspect);
    s.internalResolutionHeight = g_display.internalHeight;
    s.msaaEnabled = g_display.msaaSamples > 1 ? 1 : 0;
    if (g_display.msaaSamples > 1) s.msaaSamples = g_display.msaaSamples;
    s.frameRate = g_display.frameRate;
    s.drawDistance = g_display.drawDistance;
    s.windowMode = int(g_display.window);
    s.vsyncEnabled = g_display.fVsync ? 1 : 0;
    s.longShadows = g_display.fLongShadows ? 1 : 0;
    PcInputToSettings(s);
}

int s_xWindowed = 100, s_yWindowed = 100, s_dxWindowed = 1280, s_dyWindowed = 960;
DISPWINDOW s_windowApplied = DISPWINDOW_Windowed;

// The monitor holding most of the window (Sly1-Release MonitorForWindow), else the primary one.
GLFWmonitor* MonitorForWindow(GLFWwindow* window)
{
    if (GLFWmonitor* pm = glfwGetWindowMonitor(window)) return pm;
    int wx, wy, ww, wh;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    int cmonitor = 0;
    GLFWmonitor** apm = glfwGetMonitors(&cmonitor);
    GLFWmonitor* best = glfwGetPrimaryMonitor();
    long areaBest = -1;
    for (int i = 0; i < cmonitor; ++i)
    {
        const GLFWvidmode* mode = glfwGetVideoMode(apm[i]);
        if (!mode) continue;
        int mx, my;
        glfwGetMonitorPos(apm[i], &mx, &my);
        const long dx = std::max(0, std::min(wx + ww, mx + mode->width) - std::max(wx, mx));
        const long dy = std::max(0, std::min(wy + wh, my + mode->height) - std::max(wy, my));
        if (dx * dy > areaBest) { areaBest = dx * dy; best = apm[i]; }
    }
    return best;
}

void FramebufferSizeCallback(GLFWwindow*, int width, int height)
{
    if (width > 0 && height > 0) LayoutDisplay(width, height);
}
}  // namespace

bool FLoadDisplaySettings(const std::filesystem::path& userDir, std::string* pmessage)
{
    const auto file = st::settingsPath(userDir);
    st::SystemSettings s{};
    const st::LoadResult result = st::load(file, s);
    if (!st::loaded(result)) s = st::sly2Defaults();   // (Sly 2's own keyboard / mouse layout)
    st::sanitizeStatic(s, 0, 0, st::kSly2DefaultKeyboardBindings);
    FromSettings(s);
    const char* psz = "";
    switch (result)
    {
    case st::LoadResult::Ok: psz = "loaded"; break;
    case st::LoadResult::Upgraded: psz = "loaded (older version)"; break;
    case st::LoadResult::Missing: psz = st::save(file, s) ? "missing: written with the defaults" : "missing (not writable)"; break;
    case st::LoadResult::Corrupt: psz = "unreadable: defaults"; break;
    case st::LoadResult::Newer: psz = "written by a newer build: defaults (file kept)"; break;
    }
    if (pmessage) *pmessage = cane::utf8(file) + ": " + psz;
    return st::loaded(result);
}

bool FSaveDisplaySettings(const std::filesystem::path& userDir)
{
    const auto file = st::settingsPath(userDir);
    st::SystemSettings s{};
    const st::LoadResult result = st::load(file, s);
    if (result == st::LoadResult::Newer) return false;
    if (!st::loaded(result)) s = st::sly2Defaults();
    ToSettings(s);
    st::sanitizeStatic(s, 0, 0, st::kSly2DefaultKeyboardBindings);
    return st::save(file, s);
}

void LayoutDisplay(int outputWidth, int outputHeight)
{
    DISPLAY& d = g_display;
    d.outputWidth = std::max(1, outputWidth);
    d.outputHeight = std::max(1, outputHeight);
    float rAspect = float(d.outputWidth) / float(d.outputHeight);
    switch (d.aspect)
    {
    case DISPASPECT_Fixed_4_3: case DISPASPECT_PS2_4_3: rAspect = 4.0f / 3.0f; break;
    case DISPASPECT_Fixed_16_9: case DISPASPECT_PS2_16_9: rAspect = 16.0f / 9.0f; break;
    case DISPASPECT_Fixed_16_10: rAspect = 16.0f / 10.0f; break;
    default: break;
    }
    d.rAspectPicture = rAspect;
    int dx = d.outputWidth;
    int dy = std::max(1, int(std::lround(float(dx) / rAspect)));
    if (dy > d.outputHeight)
    {
        dy = d.outputHeight;
        dx = std::max(1, int(std::lround(float(dy) * rAspect)));
    }
    d.presentWidth = dx;
    d.presentHeight = dy;
    d.presentX = (d.outputWidth - dx) / 2;
    d.presentY = (d.outputHeight - dy) / 2;
    int dxRender = dx, dyRender = dy;
    if (d.internalHeight > 0)
    {
        dyRender = d.internalHeight;
        dxRender = std::max(1, int(std::lround(double(dyRender) * double(dx) / double(dy))));
    }
    d.renderWidth = dxRender;
    d.renderHeight = dyRender;
    // The 2D canvas (the PS2 frame at 4:3) centred in the picture.
    const float rCanvas = 4.0f / 3.0f;
    d.uiScale = rAspect > rCanvas ? glm::vec2(rCanvas / rAspect, 1.0f) : glm::vec2(1.0f, rAspect / rCanvas);
    d.uiOffset = glm::vec2(0.0f);
    if (g_gl.window)
    {
        int msaa = d.msaaSamples > 1 ? d.msaaSamples : 0;
        if (msaa)
        {
            GLint cmax = 0;
            glGetIntegerv(GL_MAX_SAMPLES, &cmax);
            msaa = std::min(msaa, int(cmax));
            if (msaa < 2) msaa = 0;
        }
        if (dxRender != g_gl.renderWidth || dyRender != g_gl.renderHeight || msaa != g_gl.msaa || !g_gl.fbo)
        {
            g_gl.msaa = msaa;
            g_gl.CreateFramebuffers(dxRender, dyRender);
        }
    }
}

int DisplayRefreshRate(GLFWwindow* window)
{
    GLFWmonitor* monitor = window ? MonitorForWindow(window) : glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    return mode ? mode->refreshRate : 0;
}

void ApplyDisplaySettings(GLFWwindow* window)
{
    if (!window) return;
    const DISPWINDOW mode = g_display.window;
    if (mode != s_windowApplied)
    {
        if (GLFWmonitor* monitor = MonitorForWindow(window))
        {
            if (s_windowApplied == DISPWINDOW_Windowed)
            {
                glfwGetWindowPos(window, &s_xWindowed, &s_yWindowed);
                glfwGetWindowSize(window, &s_dxWindowed, &s_dyWindowed);
            }
            if (mode == DISPWINDOW_Windowed)
            {
                glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
                glfwSetWindowMonitor(window, nullptr, s_xWindowed, s_yWindowed, std::max(s_dxWindowed, 320),
                                     std::max(s_dyWindowed, 240), GLFW_DONT_CARE);
            }
            else if (mode == DISPWINDOW_Borderless)
            {
                const GLFWvidmode* vm = glfwGetVideoMode(monitor);
                if (!vm) return;
                int x = 0, y = 0;
                glfwGetMonitorPos(monitor, &x, &y);
                int dx = vm->width;
#if defined(_WIN32)
                // Some Windows OpenGL drivers promote monitor-sized windows to fullscreen. An extra
                // column is a workaround for the resulting focus-change blackout, while covering the taskbar.
                // GLFW discussion, replies 2-3 (fullscreen promotion and the reported size workaround):
                // https://discourse.glfw.org/t/cannot-view-window-via-screenshot-or-remote-screen-access/2986
                ++dx;
#endif
                glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
                glfwSetWindowMonitor(window, nullptr, x, y, dx, vm->height, GLFW_DONT_CARE);
            }
            else if (const GLFWvidmode* vm = glfwGetVideoMode(monitor))
            {
                glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
                glfwSetWindowMonitor(window, monitor, 0, 0, vm->width, vm->height, vm->refreshRate);
            }
            s_windowApplied = mode;
        }
    }
    // The swap interval (Sly1-Release ApplySwapInterval): with VSync, 1 when the cap reaches the refresh rate (the swap
    // then paces), 2 for a cap of half of it; another cap is paced by the timer between the vblanks.
    int interval = 0, refreshRate = 0;
    bool fSwapPacesFrames = false;
    if (g_display.fVsync)
    {
        interval = 1;
        refreshRate = DisplayRefreshRate(window);
        // (Windows reports 59.94 Hz modes as 59: the real vblank runs more than 1 % faster than a 59 cap, it never gates)
        const bool fRefreshTruncated = refreshRate < 100 && refreshRate % 10 == 9;
        if (refreshRate > 0 && g_display.frameRate >= refreshRate && !fRefreshTruncated) fSwapPacesFrames = true;
        else if (refreshRate > 0 && g_display.frameRate * 2 == refreshRate)
        {
            interval = 2;   // (Mesa on Wayland clamps it to 1: the pacer notices the swap not pacing and holds the cap)
            fSwapPacesFrames = true;
        }
    }
    cane::gl::SwapInterval(interval);
    g_display.fSwapPacesFrames = fSwapPacesFrames;
    static int s_interval = -1, s_refreshRate = -1, s_frameRate = -1, s_window = -1;
    if (interval != s_interval || refreshRate != s_refreshRate || g_display.frameRate != s_frameRate || int(mode) != s_window)
    {
        s_interval = interval, s_refreshRate = refreshRate, s_frameRate = g_display.frameRate, s_window = int(mode);
        ++g_display.swapGeneration;
    }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    LayoutDisplay(fbw, fbh);
}

void InstallDisplayCallbacks(GLFWwindow* window)
{
    if (window) glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
}

DISPPROJ ProjectionForDisplay(float radFovY, float rAspectCm)
{
    DISPPROJ proj{radFovY, g_display.rAspectPicture};
    if (g_display.aspect == DISPASPECT_PS2_4_3)
        proj.rAspect = rAspectCm;   // the camera's own aspect, shown on a 4:3 picture (as on a TV)
    else if (g_display.aspect == DISPASPECT_PS2_16_9)
    {
        // retail-style widescreen (Vert-): the original's horizontal framing, top and bottom cropped
        proj.rAspect = 16.0f / 9.0f;
        proj.radFovY = 2.0f * std::atan(std::tan(radFovY * 0.5f) * rAspectCm / proj.rAspect);
    }
    return proj;
}

bool FParseAspect(const char* psz, DISPASPECT& aspect)
{
    static const char* s_apsz[] = {"fit", "4:3", "16:9", "16:10", "ps2-4:3", "ps2-16:9"};
    for (int i = 0; i < 6; ++i)
        if (!std::strcmp(psz, s_apsz[i])) { aspect = DISPASPECT(i); return true; }
    if (psz[0] >= '0' && psz[0] <= '5' && !psz[1]) { aspect = DISPASPECT(psz[0] - '0'); return true; }
    return false;
}
