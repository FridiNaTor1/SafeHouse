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

#include "../platform/sly3_settings.h"
#include "../platform/display_pacing.h"
#include "../platform/monitor_select_glfw.h"
#include "audio.h"
#include "rumble.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

DISPLAY g_display;
SAVEOPTS g_saveopts;

#ifdef __SWITCH__
void CaneSwitchSetFrameRate(int fps);   // platform/switch/glfw_switch.cpp
#endif

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
    g_display.monitor = cane::monitor::load(s.monitor);   // (v22, platform/monitor_select.h)
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
    g_saveopts.outputDevice = o.outputDevice;
    g_saveopts.inputDevice = o.inputDevice;
    g_saveopts.headsetOutputDevice = o.headsetOutputDevice;
    g_saveopts.fHeadset = o.usbHeadset != 0;
    g_saveopts.fTextureCache = o.textureCache != 0;
    SetTextureCacheEnabled(g_saveopts.fTextureCache);   // (texmod.h: read before the first texture)
    g_saveopts.fTextureStreaming = o.textureStreaming != 0;
    SetTextureModStreamingEnabled(g_saveopts.fTextureStreaming);
    for (int k = 0; k < 8; ++k)
        for (int a = 0; a < 2; ++a) g_saveopts.afCamReverse[k][a] = o.camReverse[k][a] != 0;
    g_saveopts.f3dMode = o.mode3d != 0;
    ApplySoundOptions();
}

void ToGameOptions(st::Sly2GameOptions& o)
{
    o.invertBinocucom = g_saveopts.fInvertBinoc ? 1 : 0;
    o.invertTurret = g_saveopts.fInvertTurret ? 1 : 0;
    o.speakers = g_saveopts.speakers;
    o.language = g_saveopts.language;
    o.outputDevice = g_saveopts.outputDevice;
    o.inputDevice = g_saveopts.inputDevice;
    o.headsetOutputDevice = g_saveopts.headsetOutputDevice;
    o.usbHeadset = g_saveopts.fHeadset ? 1 : 0;
    o.textureCache = g_saveopts.fTextureCache ? 1 : 0;
    o.textureStreaming = g_saveopts.fTextureStreaming ? 1 : 0;
    for (int k = 0; k < 8; ++k)
        for (int a = 0; a < 2; ++a) o.camReverse[k][a] = g_saveopts.afCamReverse[k][a] ? 1 : 0;
    o.mode3d = g_saveopts.f3dMode ? 1 : 0;
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
    v.fCutscenesFlat = o.cutscenesFlat != 0;
    v.fRcTable = o.rcTable != 0;
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
    o.cutscenesFlat = v.fCutscenesFlat ? 1 : 0;
    o.rcTable = v.fRcTable ? 1 : 0;
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
    cane::monitor::store(s.monitor, g_display.monitor);
    s.vsyncEnabled = g_display.fVsync ? 1 : 0;
    s.longShadows = g_display.fLongShadows ? 1 : 0;
    s.fastShadows = g_display.fFastShadows ? 1 : 0;
    s.renderer = g_rendererSetting == RENDERER::Vulkan ? 1 : g_rendererSetting == RENDERER::OpenGL ? 2 : 0;   // (not --renderer's)
    PcInputToSettings(s);
    s.musicVolume = g_saveopts.gMusic;
    s.soundEffectsVolume = g_saveopts.gEffects;
    s.dialogueVolume = g_saveopts.gDialog;
}

cane::monitor::Placement s_placement;   // (the window mode applied: platform/monitor_select_glfw.h)

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

static void FromUpscaleOptions(const st::Sly2UpscaleOptions& o)
{
    g_display.fUpscale = o.enabled != 0;
    g_display.upscaleMethod = st::UpscaleMethod_Fsr1;
    g_display.upscalePreset = std::clamp(o.preset, 0, 3);
    g_display.upscaleSharpness = std::clamp(o.sharpness, 0, 100);
}

static void ToUpscaleOptions(st::Sly2UpscaleOptions& o)
{
    o.enabled = g_display.fUpscale ? 1 : 0;
    o.method = g_display.upscaleMethod;
    o.preset = g_display.upscalePreset;
    o.sharpness = g_display.upscaleSharpness;
}

bool FLoadDisplaySettings(const std::filesystem::path& userDir, std::string* pmessage)
{
    st::Sly3Settings s;
    std::vector<std::string> invalid;
    const st::Sly2Load result = st::loadSly3(userDir, s, &invalid);
    FromSettings(s.system);
    FromGameOptions(s.game);
    FromVrOptions(s.vr);
    FromUpscaleOptions(s.upscale);
    g_display.outlineThickness = std::clamp(s.outlineThickness, st::kOutlineThicknessMin, st::kOutlineThicknessMax);
    g_display.hudPlacement = std::clamp(s.hudPlacement, 0, st::kHudPlacementCount - 1);
    sly2actions::Load(userDir);
#ifdef __SWITCH__
    // (the Switch: one screen, the swap paces the frames at 30 or 60, 120 on the Switch 2; the internal resolution
    // from the menu's list, at most the docked screen's 1080 lines, 2160 on the Switch 2)
    g_display.window = DISPWINDOW_Fullscreen;
    g_display.fVsync = true;
#if defined(__SWITCH2__)
    if (g_display.frameRate != 30) g_display.frameRate = g_display.frameRate > 60 ? 120 : 60;
#else
    if (g_display.frameRate != 30) g_display.frameRate = 60;
#endif
    g_display.msaaSamples = 0;
#if defined(__SWITCH2__)
    if (g_display.internalHeight > 2160) g_display.internalHeight = 0;
#else
    if (g_display.internalHeight > 1080) g_display.internalHeight = 0;
#endif
#endif
    g_saveopts.fStore = result != st::Sly2Load::Unreadable;
    std::string message;
    switch (result)
    {
    case st::Sly2Load::Ok: message = "loaded"; break;
    case st::Sly2Load::Migrated:
        message = st::saveSly3(userDir, s) ? "made from Saves/system_settings.dat" : "made from Saves/system_settings.dat (not writable)";
        break;
    case st::Sly2Load::Missing: message = st::saveSly3(userDir, s) ? "missing: written with the defaults" : "missing (not writable)"; break;
    case st::Sly2Load::Unreadable: message = "unreadable: defaults (file kept)"; break;
    }
    for (const std::string& key : invalid) message += "; " + key + ": invalid, the default";
    if (pmessage) *pmessage = cane::utf8(st::sly3SettingsPath(userDir)) + ": " + message;
    return result != st::Sly2Load::Unreadable;
}

bool FSaveDisplaySettings(const std::filesystem::path& userDir)
{
    // (the file read again: keys this build does not know, and the pending migration, are kept)
    st::Sly3Settings s;
    if (st::loadSly3(userDir, s) == st::Sly2Load::Unreadable) return false;
    ToSettings(s.system);
    ToGameOptions(s.game);
    ToVrOptions(s.vr);
    ToUpscaleOptions(s.upscale);
    s.outlineThickness = g_display.outlineThickness;
    s.hudPlacement = g_display.hudPlacement;
    return st::saveSly3(userDir, s);
}

bool FSaveOptionsPending()
{
    if (!g_saveopts.fStore) return false;
    st::Sly3Settings s;
    return st::loadSly3(cane::userDirectory(), s) == st::Sly2Load::Ok && !s.pendingSaveOptions.empty();
}

void MigrateSaveOptions(const st::SaveOptionValues& values)
{
    if (!g_saveopts.fStore) return;
    const auto userDir = cane::userDirectory();
    st::Sly3Settings s;
    if (st::loadSly3(userDir, s) != st::Sly2Load::Ok || s.pendingSaveOptions.empty()) return;
    // (the file's values with the save's over the pending ones; only those change in the running game, so options
    //  given on the command line for this run are neither saved nor undone)
    st::applySaveOptions(s, values);
    g_fControllerVibration = s.system.controllerVibration != 0;
    g_saveopts.gMusic = s.system.musicVolume;
    g_saveopts.gEffects = s.system.soundEffectsVolume;
    FromGameOptions(s.game);
    std::printf("settings: the save's options taken into %s%s\n", cane::utf8(st::sly3SettingsPath(userDir)).c_str(),
                st::saveSly3(userDir, s) ? "" : " (not writable)");
}

float DxHudPlacement()
{
    const int n = g_display.hudPlacement;
    if (n <= st::HudPlacement_4_3 || vr::FActive()) return 0.0f;   // (VR: the HUD panel keeps the 4:3 canvas)
    // the picture's shape on the canvas's scale (the canvas is 4:3, uiScale.x its share of the picture's width)
    const float rPicture = g_display.uiScale.x < 1.0f ? (4.0f / 3.0f) / g_display.uiScale.x : 4.0f / 3.0f;
    const float rBox = std::min(rPicture, n == st::HudPlacement_16_9 ? 16.0f / 9.0f : n == st::HudPlacement_16_10 ? 16.0f / 10.0f : rPicture);
    return std::max(0.0f, 320.0f * (rBox * 0.75f - 1.0f));
}

void LayoutDisplay(int outputWidth, int outputHeight)
{
    vr::FPanelSize(&outputWidth, &outputHeight);   // (VR: the headset's screen, not the window)
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
    if (d.fUpscale && g_gl.window && fsr::FAvailable())
        fsr::RenderSizeForPreset(d.upscalePreset, dx, dy, &dxRender, &dyRender);   // (gl.h BeginNativeUi scales it up)
    else if (d.internalHeight > 0)
    {
        dyRender = d.internalHeight;
#if defined(__SWITCH2__)
        // (at 120 fps, at most the picture's height; the setting applies again at 60)
        if (d.frameRate > 60) dyRender = std::min(dyRender, dy);
#endif
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
    // (the window's monitor when fullscreen, the chosen one for Borderless, else the one holding the window)
    GLFWmonitor* monitor = cane::monitor::refreshMonitor(window, int(g_display.window), g_display.monitor);
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    return mode ? mode->refreshRate : 0;
}

void ApplyDisplaySettings(GLFWwindow* window)
{
    if (!window) return;
    const DISPWINDOW mode = g_display.window;
    // (the window mode on the chosen monitor, platform/monitor_select_glfw.h: Fullscreen / Borderless fill it, Windowed
    // moves onto it when the choice changes; a chosen monitor that is unplugged: the primary one, UpdateDisplayMonitors)
    cane::monitor::watchConnections();
    cane::monitor::applyWindowMode(window, s_placement, int(mode), g_display.monitor);
    // The swap interval (Sly1-Release ApplySwapInterval): with VSync, 1 when the cap reaches the refresh rate (the swap
    // then paces), 2 for a cap of half of it; another cap is paced by the timer between the vblanks.
    int interval = 0, refreshRate = 0;
    bool fSwapPacesFrames = false;
    g_display.displayFps = 0;
#if defined(CANE_IOS)
    // (iOS, as Sly 2's: a CAMetalLayer always presents at a vblank, VSync or not, so the setting does not apply (the
    // menus hide it). The display paces whenever the cap is its rate or a whole fraction of it (display_pacing.h), the
    // game woken by a display link at the cap's rate (game.cpp); another cap is held by the timer)
    {
        refreshRate = DisplayRefreshRate(window);
        const cane::pacing::SwapChoice choice = cane::pacing::ChooseLockedToVblank(g_display.frameRate, refreshRate);
        interval = choice.interval;
        fSwapPacesFrames = choice.fSwapPacesFrames;
        g_display.displayFps = choice.displayFps;
    }
#else
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
#endif
#ifdef __SWITCH__
    CaneSwitchSetFrameRate(g_display.frameRate);
#endif
    cane::gl::SwapInterval(interval);
    g_display.fSwapPacesFrames = fSwapPacesFrames;
    static int s_interval = -1, s_refreshRate = -1, s_frameRate = -1, s_window = -1;
    static std::string s_monitor;   // (another monitor: the pacer checks the swap again)
    if (interval != s_interval || refreshRate != s_refreshRate || g_display.frameRate != s_frameRate || int(mode) != s_window ||
        g_display.monitor != s_monitor)
    {
        s_interval = interval, s_refreshRate = refreshRate, s_frameRate = g_display.frameRate, s_window = int(mode);
        s_monitor = g_display.monitor;
        ++g_display.swapGeneration;
    }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    LayoutDisplay(fbw, fbh);
}

void UpdateDisplayMonitors(GLFWwindow* window)
{
    if (window && cane::monitor::connectionsChanged(s_placement))
    {
        std::printf("display: monitors changed (%s)\n", g_display.monitor.empty() ? "Primary" : g_display.monitor.c_str());
        ApplyDisplaySettings(window);
    }
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
