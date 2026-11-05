#pragma once

// PC display options, as in Sly1-Release (gl.cpp ApplyAspectRatioSettings / ApplyWindowModeSettings /
// FrameBufferSizeCallBack, cm.cpp RecalcCm), stored in <user-dir>/Saves/system_settings.dat through
// platform/system_settings.h (the launcher's Settings page edits the same file): aspect mode, internal resolution,
// MSAA, frame-rate cap, draw distance, window mode, VSync.
//
// The layout: the window (output) holds a centred picture of the chosen aspect (pillar- / letterboxed); the scene
// renders into an off-screen target of the picture's aspect (at the internal resolution when set); the 2D canvas
// (the PS2 frame, 640 x 448 shown at 4:3) is centred in the picture, pillarboxed on wide pictures.

#include <glm/glm.hpp>

#include <filesystem>
#include <string>

struct GLFWwindow;

// The aspect modes (values = system_settings.h AspectValue = Sly1-Release gl.h AspectMode).
enum DISPASPECT
{
    DISPASPECT_Fit = 0,         // the window's aspect; the original's vertical field of view (Hor+)
    DISPASPECT_Fixed_4_3 = 1,
    DISPASPECT_Fixed_16_9 = 2,
    DISPASPECT_Fixed_16_10 = 3,
    DISPASPECT_PS2_4_3 = 4,     // the original's anamorphic picture: the camera's rAspect squeezed into 4:3
    DISPASPECT_PS2_16_9 = 5,    // the original's horizontal field of view kept, cropped to 16:9 (Vert-)
};
enum DISPWINDOW { DISPWINDOW_Windowed = 0, DISPWINDOW_Borderless = 1, DISPWINDOW_Fullscreen = 2 };

constexpr int DISP_FRAMERATE_UNCAPPED = 1000;   // (the launcher's "Uncapped": a value of the shared field)

struct DISPLAY
{
    // Options (system_settings.dat)
    DISPASPECT aspect = DISPASPECT_Fit;
    int internalHeight = 0;     // 0: the picture's own size
    int msaaSamples = 0;        // 0 / 1: off
    int frameRate = 60;         // the frame-rate cap (>= 30; DISP_FRAMERATE_UNCAPPED and above: none)
    float drawDistance = 1.0f;  // multiplies the distance limits (MRD), 0.75 .. 3
    DISPWINDOW window = DISPWINDOW_Windowed;
    bool fVsync = false;
    // Native QoL, always on (not in system_settings.dat; the launcher dropped its toggle 2026-11-05; --wide-backgrounds 0 or
    // SLY2N_WIDEBG=0 turns it off): the 2D's full-screen backgrounds
    // (vector polygons on the 4:3 canvas's edges: the binocucom's surround, the pause menu's backdrop, the cut
    // scenes' bars, the fades) reach the window's edges; everything else keeps the 4:3 canvas.
    bool fWideBackgrounds = true;
    // Native QoL (not in system_settings.dat; --hq-logo 0 / SLY2N_HQLOGO=0 turn it off): the front end's Sly 2 logo is
    // the launcher's high-quality one (qolview.h GlTexLogoOverride)
    bool fHqLogo = true;

    // Layout (LayoutDisplay)
    int outputWidth = 1, outputHeight = 1;          // the window's framebuffer
    int presentX = 0, presentY = 0, presentWidth = 1, presentHeight = 1;   // the picture in it
    int renderWidth = 1, renderHeight = 1;          // the scene target
    float rAspectPicture = 4.0f / 3.0f;
    // The 2D canvas in the picture: NDC = canvas NDC x scale + offset.
    glm::vec2 uiScale{1.0f}, uiOffset{0.0f};
};
extern DISPLAY g_display;

// Reads the options (a missing file is created with the defaults; a newer one is read-only). Returns false and
// keeps the defaults when the file cannot be used.
bool FLoadDisplaySettings(const std::filesystem::path& userDir, std::string* pmessage = nullptr);
// Writes the options back into the file (the other fields, key bindings and audio, kept).
bool FSaveDisplaySettings(const std::filesystem::path& userDir);

// The window's framebuffer changed (or an option did): the picture, the scene target (g_gl.CreateFramebuffers
// when its size or MSAA changes) and the 2D canvas.
void LayoutDisplay(int outputWidth, int outputHeight);
// Window mode, swap interval and layout for the current options (call after the window exists).
void ApplyDisplaySettings(GLFWwindow* window);
// Installs the framebuffer-size callback (LayoutDisplay).
void InstallDisplayCallbacks(GLFWwindow* window);

// The projection the picture needs, from the original camera's vertical field of view and its aspect (CM +0x3C,
// 512 / 394.24): Sly1-Release RecalcCm. (For the camera's RecalcCm / CMVIEW.)
struct DISPPROJ { float radFovY; float rAspect; };
// (rAspectCm: Sly 3's camera aspect, orig3 CM +0x130 = 640 / 448: the 3D is drawn for square 640 x 448 pixels and shown
// at 4:3 on a TV, so the PS2 picture's 3D is 7% narrower than at 4:3 while its 2D is exact; Sly 2's was 512 / 394.24)
DISPPROJ ProjectionForDisplay(float radFovY, float rAspectCm = 640.0f / 448.0f);

// --aspect / --internal-res style overrides ("fit", "4:3", "16:9", "16:10", "ps2-4:3", "ps2-16:9" or 0..5).
bool FParseAspect(const char* psz, DISPASPECT& aspect);
