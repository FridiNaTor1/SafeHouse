// See display.h. Adapted from Sly1-Release gl.cpp (ApplyAspectRatioSettings, ApplyWindowModeSettings,
// FrameBufferSizeCallBack) and cm.cpp (RecalcCm's aspect modes).

#include "texmod.h"
#include "renderer.h"
#include "display.h"
#include "vr.h"

#include "gl.h"
#include "fsr.h"
#include "pcinput.h"
#include "actioninput.h"
#include "sound.h"

#include "../platform/sly2_settings.h"
#include "audio.h"
#include "rumble.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

DISPLAY g_display;
SAVEOPTS g_saveopts;

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
    g_display.fFastShadows = s.fastShadows != 0;   // (v20, low-end: render.cpp FBuildShadow2 / DrawShadow2)
    g_rendererSetting = g_rendererWanted = s.renderer == 1 ? RENDERER::Vulkan : s.renderer == 2 ? RENDERER::OpenGL : RENDERER::Auto;   // (v21, renderer.h)
    PcInputFromSettings(s);   // (the controls and the prompts: pcinput.h)
    g_saveopts.gMusic = s.musicVolume;
    g_saveopts.gEffects = s.soundEffectsVolume;
    g_saveopts.gDialog = s.dialogueVolume;
    ApplySoundOptions();
}

void FromGameOptions(const st::Sly2GameOptions& o)
{
    g_saveopts.fInvertBinoc = o.invertBinocucom != 0;
    g_saveopts.fInvertTurret = o.invertTurret != 0;
    g_saveopts.speakers = std::clamp(o.speakers, 0, 2);
    g_saveopts.language = o.language;
    g_saveopts.voiceLanguage = o.voiceLanguage;
    g_saveopts.outputDevice = o.outputDevice;
    g_saveopts.inputDevice = o.inputDevice;
    g_saveopts.headsetOutputDevice = o.headsetOutputDevice;
    g_saveopts.fHeadset = o.usbHeadset != 0;
    g_saveopts.fTextureCache = o.textureCache != 0;
    SetTextureCacheEnabled(g_saveopts.fTextureCache);   // (texmod.h: read before the first texture)
    g_saveopts.fTextureStreaming = o.textureStreaming != 0;
    SetTextureModStreamingEnabled(g_saveopts.fTextureStreaming);
    ApplySoundOptions();
}

void ToGameOptions(st::Sly2GameOptions& o)
{
    o.invertBinocucom = g_saveopts.fInvertBinoc ? 1 : 0;
    o.invertTurret = g_saveopts.fInvertTurret ? 1 : 0;
    o.speakers = g_saveopts.speakers;
    o.language = g_saveopts.language;
    o.voiceLanguage = g_saveopts.voiceLanguage;
    o.outputDevice = g_saveopts.outputDevice;
    o.inputDevice = g_saveopts.inputDevice;
    o.headsetOutputDevice = g_saveopts.headsetOutputDevice;
    o.usbHeadset = g_saveopts.fHeadset ? 1 : 0;
    o.textureCache = g_saveopts.fTextureCache ? 1 : 0;
    o.textureStreaming = g_saveopts.fTextureStreaming ? 1 : 0;
}

// (vr.h) [VR]
void FromVrOptions(const st::Sly2VrOptions& o)
{
    vr::VROPTS& v = vr::g_vropts;
    v.mode = std::clamp(o.mode, 0, 1);
    v.fAutoCameraRotation = o.autoCameraRotation != 0;
    v.vignette = std::clamp(o.vignette, 0, 3);
    v.screenSize = std::clamp(o.screenSize, 0, 2);
    v.refreshRate = o.refreshRate;
    v.worldScale = o.worldScale;
    v.fCutscenesFlat = o.cutscenesView != 0;   // (Sly 2's own key: Immersive by default)
    v.fRcTable = o.rcTable != 0;
    v.eyeResolution = o.eyeResolution == 0 ? 0 : std::clamp(o.eyeResolution, 50, 100);
    v.msaa = o.msaa <= 1 ? 1 : 2;
    v.foveation = std::clamp(o.foveation, 0, 3);
    v.fMultiview = o.multiview != 0;
    v.upscaling = std::clamp(o.upscaling, 0, 2);
}

void ToVrOptions(st::Sly2VrOptions& o)
{
    const vr::VROPTS& v = vr::g_vropts;
    o.mode = v.mode;
    o.autoCameraRotation = v.fAutoCameraRotation ? 1 : 0;
    o.vignette = v.vignette;
    o.screenSize = v.screenSize;
    o.refreshRate = v.refreshRate;
    o.worldScale = v.worldScale;
    o.cutscenesView = v.fCutscenesFlat ? 1 : 0;
    o.rcTable = v.fRcTable ? 1 : 0;
    o.eyeResolution = v.eyeResolution;
    o.msaa = v.msaa;
    o.foveation = v.foveation;
    o.multiview = v.fMultiview ? 1 : 0;
    o.upscaling = v.upscaling;
}

void FromUpscaleOptions(const st::Sly2UpscaleOptions& o)
{
    g_display.fUpscale = o.enabled != 0;
    g_display.upscaleMethod = st::UpscaleMethod_Fsr1;
    g_display.upscalePreset = std::clamp(o.preset, 0, 3);
    g_display.upscaleSharpness = std::clamp(o.sharpness, 0, 100);
}

void ToUpscaleOptions(st::Sly2UpscaleOptions& o)
{
    o.enabled = g_display.fUpscale ? 1 : 0;
    o.method = g_display.upscaleMethod;
    o.preset = g_display.upscalePreset;
    o.sharpness = g_display.upscaleSharpness;
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
    s.fastShadows = g_display.fFastShadows ? 1 : 0;
    s.renderer = g_rendererSetting == RENDERER::Vulkan ? 1 : g_rendererSetting == RENDERER::OpenGL ? 2 : 0;   // (not --renderer's)
    PcInputToSettings(s);
    s.musicVolume = g_saveopts.gMusic;
    s.soundEffectsVolume = g_saveopts.gEffects;
    s.dialogueVolume = g_saveopts.gDialog;
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

void ApplySoundOptions()
{
    SetSoundUserVolumes(g_saveopts.gMusic, g_saveopts.gEffects, g_saveopts.gDialog);   // (sound.h)
    SetMonoOutput(g_saveopts.speakers == st::Speakers_Mono);                          // (audio.h)
    SetAudioDevices(g_saveopts.outputDevice, g_saveopts.inputDevice, g_saveopts.headsetOutputDevice);   // (audio.h)
}

bool FLoadDisplaySettings(const std::filesystem::path& userDir, std::string* pmessage)
{
    st::Sly2Settings s;
    std::vector<std::string> invalid;
    const st::Sly2Load result = st::loadSly2(userDir, s, &invalid);
    FromSettings(s.system);
    FromGameOptions(s.game);
    FromVrOptions(s.vr);
    FromUpscaleOptions(s.upscale);
    sly2actions::Load(userDir);
#ifdef __SWITCH__
    // (the Switch: one screen at 60 Hz, the swap paces the frames at 30 or 60; the internal resolution from the menu's
    // list, at most the docked screen's 1080 lines)
    g_display.window = DISPWINDOW_Fullscreen;
    g_display.fVsync = true;
    if (g_display.frameRate != 30) g_display.frameRate = 60;
    g_display.msaaSamples = 0;
    if (g_display.internalHeight > 1080) g_display.internalHeight = 0;
#endif
    g_saveopts.fStore = result != st::Sly2Load::Unreadable;
    std::string message;
    switch (result)
    {
    case st::Sly2Load::Ok: message = "loaded"; break;
    case st::Sly2Load::Migrated:
        message = st::saveSly2(userDir, s) ? "made from Saves/system_settings.dat" : "made from Saves/system_settings.dat (not writable)";
        break;
    case st::Sly2Load::Missing: message = st::saveSly2(userDir, s) ? "missing: written with the defaults" : "missing (not writable)"; break;
    case st::Sly2Load::Unreadable: message = "unreadable: defaults (file kept)"; break;
    }
    for (const std::string& key : invalid) message += "; " + key + ": invalid, the default";
    if (pmessage) *pmessage = cane::utf8(st::sly2SettingsPath(userDir)) + ": " + message;
    return result != st::Sly2Load::Unreadable;
}

bool FSaveDisplaySettings(const std::filesystem::path& userDir)
{
    // (the file read again: keys this build does not know, and the pending migration, are kept)
    st::Sly2Settings s;
    if (st::loadSly2(userDir, s) == st::Sly2Load::Unreadable) return false;
    ToSettings(s.system);
    ToGameOptions(s.game);
    ToVrOptions(s.vr);
    ToUpscaleOptions(s.upscale);
    return st::saveSly2(userDir, s);
}

bool FSaveOptionsPending()
{
    if (!g_saveopts.fStore) return false;
    st::Sly2Settings s;
    return st::loadSly2(cane::userDirectory(), s) == st::Sly2Load::Ok && !s.pendingSaveOptions.empty();
}

void MigrateSaveOptions(const st::SaveOptionValues& values)
{
    if (!g_saveopts.fStore) return;
    const auto userDir = cane::userDirectory();
    st::Sly2Settings s;
    if (st::loadSly2(userDir, s) != st::Sly2Load::Ok || s.pendingSaveOptions.empty()) return;
    // (the file's values with the save's over the pending ones; only those change in the running game, so options
    //  given on the command line for this run are neither saved nor undone)
    st::applySaveOptions(s, values);
    g_fControllerVibration = s.system.controllerVibration != 0;
    g_saveopts.gMusic = s.system.musicVolume;
    g_saveopts.gEffects = s.system.soundEffectsVolume;
    FromGameOptions(s.game);
    std::printf("settings: the save's options taken into %s%s\n", cane::utf8(st::sly2SettingsPath(userDir)).c_str(),
                st::saveSly2(userDir, s) ? "" : " (not writable)");
}

void LayoutDisplay(int outputWidth, int outputHeight)
{
    vr::FPanelSize(&outputWidth, &outputHeight);   // (VR: the headset's screen, not the window)
    DISPLAY& d = g_display;
    d.outputWidth = std::max(1, outputWidth);
    d.outputHeight = std::max(1, outputHeight);
    float rAspect = float(d.outputWidth) / float(d.outputHeight);
    // (VR's binocucom over the whole view: its picture as the panel is, the canvas placed by vr.cpp)
    glm::vec2 uiScaleBinoc(1.0f);
    const bool fBinoc = vr::FBinocCanvas(&uiScaleBinoc);
    if (!fBinoc)
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
    if (d.fUpscale && g_gl.window && !vr::FActive() && fsr::FAvailable())
        fsr::RenderSizeForPreset(d.upscalePreset, dx, dy, &dxRender, &dyRender);   // (gl.h BeginNativeUi scales it up)
    else if (d.internalHeight > 0)
    {
        dyRender = d.internalHeight;
        dxRender = std::max(1, int(std::lround(double(dyRender) * double(dx) / double(dy))));
    }
    d.renderWidth = dxRender;
    d.renderHeight = dyRender;
    // The 2D canvas (the PS2 frame at 4:3) centred in the picture.
    const float rCanvas = 4.0f / 3.0f;
    d.uiScale = rAspect > rCanvas ? glm::vec2(rCanvas / rAspect, 1.0f) : glm::vec2(1.0f, rAspect / rCanvas);
    if (fBinoc) d.uiScale = uiScaleBinoc;
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
                // the whole monitor, taskbar included (as Sly1-Release ApplyWindowModeSettings; on Windows the frames
                // go through a DXGI swap chain, platform/gl_context.h, so focus changes do not blank the screen)
                if (const GLFWvidmode* vm = glfwGetVideoMode(monitor))
                {
                    int x = 0, y = 0;
                    glfwGetMonitorPos(monitor, &x, &y);
                    glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
                    glfwSetWindowMonitor(window, nullptr, x, y, vm->width, vm->height, GLFW_DONT_CARE);
                }
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
