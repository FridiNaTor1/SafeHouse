#pragma once

// PC display options, as in Sly1-Release (gl.cpp ApplyAspectRatioSettings / ApplyWindowModeSettings /
// FrameBufferSizeCallBack, cm.cpp RecalcCm), stored in <user-dir>/Settings.ini through
// platform/sly2_settings.h (the launcher's Settings page edits the same file): aspect mode, internal resolution,
// MSAA, frame-rate cap, draw distance, window mode, VSync.
//
// The layout: the window (output) holds a centred picture of the chosen aspect (pillar- / letterboxed); the scene
// renders into an off-screen target of the picture's aspect (at the internal resolution when set); the 2D canvas
// (the PS2 frame, 640 x 448 shown at 4:3) is centred in the picture, pillarboxed on wide pictures.

#include "../platform/texture_options.h"
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
    // Options (Settings.ini)
    DISPASPECT aspect = DISPASPECT_Fit;
    int internalHeight = 0;     // 0: the picture's own size
    int msaaSamples = 0;        // 0 / 1: off
    int frameRate = 60;         // the frame-rate cap (>= 30; DISP_FRAMERATE_UNCAPPED and above: none)
    float drawDistance = 1.0f;  // multiplies the distance limits (MRD), 0.75 .. 3; above 1x also drawdist.h (visual only)
    DISPWINDOW window = DISPWINDOW_Windowed;
    bool fVsync = false;
    // Native QoL, always on (not in Settings.ini; the launcher dropped its toggle 2026-11-05; --wide-backgrounds 0 or
    // SLY2N_WIDEBG=0 turns it off): the 2D's full-screen backgrounds
    // (vector polygons on the 4:3 canvas's edges: the binocucom's surround, the pause menu's backdrop, the cut
    // scenes' bars, the fades) reach the window's edges; everything else keeps the 4:3 canvas.
    bool fWideBackgrounds = true;
    // Native QoL (not in Settings.ini; --hq-logo 0 / SLY2N_HQLOGO=0 turn it off): the front end's Sly 2 logo is
    // the launcher's high-quality one (qolview.h GlTexLogoOverride)
    bool fHqLogo = true;
    // Native QoL (Settings.ini Long Shadows, default on; --long-shadows 0 / SLY2N_LONGSHADOWS=0 turn it off):
    // the type-2 cast shadows (Sly's, the guards') reach the floor instead of ending at the original's fixed far cap
    // (render.cpp FBuildShadow2, render.md "Long shadows", R39). Off: the original's prisms exactly.
    bool fLongShadows = true;
    // Low-end option (perf-4; default off = the current look): the type-2 cast shadows with less stencil fill, as the
    // Switch draws them (render.cpp FBuildShadow2 / DrawShadow2): the side walls two prisms share from opposite sides
    // left out, and casters wholly in front of the near plane with closed, consistently oriented volumes counted z-pass
    // (their far parts fail early Z). The same pictures in the frames tested (perf-5; a hairline along a left-out wall's
    // diagonal can differ). The shadow volumes
    // are the largest GPU fill of a frame (~36 screens of stencil samples in Paris): for tile GPUs (Quest, Steam Frame).
    // --fast-shadows 1 / SLY2N_FASTSHADOWS=1; the Switch always.
    bool fFastShadows = false;

    // The swap (ApplyDisplaySettings): VSync gates the frames at the cap (the pacer then runs 1 % fast, so only the
    // swap gates: platform/frame_pacer.h), and a generation that changes with the swap's configuration
    bool fSwapPacesFrames = false;
    unsigned swapGeneration = 0;

    // Layout (LayoutDisplay)
    int outputWidth = 1, outputHeight = 1;          // the window's framebuffer
    int presentX = 0, presentY = 0, presentWidth = 1, presentHeight = 1;   // the picture in it
    int renderWidth = 1, renderHeight = 1;          // the scene target
    float rAspectPicture = 4.0f / 3.0f;
    // The 2D canvas in the picture: NDC = canvas NDC x scale + offset.
    glm::vec2 uiScale{1.0f}, uiOffset{0.0f};
};
extern DISPLAY g_display;
// The refresh rate of the monitor holding most of the window (0: unknown)
int DisplayRefreshRate(GLFWwindow* window);

// (PC, Settings.ini: platform/sly2_settings.h) the original game's own options, which the PS2 kept in each save's
// "flags": on PC they live in Settings.ini with every other option (the launcher edits the same file) and the save's
// flags only mirror them (menu.cpp SyncOptionsToRecord). Vibration is rumble.h g_fControllerVibration.
// fStore: Settings.ini was read (a game run with a window: FLoadDisplaySettings); otherwise (headless tests) the
// original's behaviour: the record's flags are the options.
struct SAVEOPTS
{
    bool fStore = false;
    bool fInvertBinoc = false;    // flags "fReverseLookAround"
    bool fInvertTurret = false;   // flags "fReverseAim"
    int speakers = 0;             // flags "nMixerMode": 0 Surround, 1 Mono, 2 Stereo (settings::Sly2Speakers)
    float gMusic = 1.0f, gEffects = 1.0f, gDialog = 1.0f;   // 0..1 (sound.h SetSoundUserVolumes)
    std::string language;         // (lang) "" Auto, else a language's folder name (region.h InitLanguage)
    // (audio, notes/AUDIO_2026-11-10.md) the devices by name ("" the system default) and the original's USB Headset
    // (flags "fUseHeadset": the sound manager's +0x10, sound.cpp UpdateMicNoise / spiop's headset tracks)
    std::string outputDevice, inputDevice, headsetOutputDevice;
    bool fHeadset = false;
    // (texmod.h: Settings.ini [Graphics] Texture Cache / Texture Streaming)
    bool fTextureCache = cane::settings::kTextureCacheDefault != 0;
    bool fTextureStreaming = cane::settings::kTextureStreamingDefault != 0;
};
extern SAVEOPTS g_saveopts;

// Reads every option from <user-dir>/Settings.ini (made once from the old Saves/system_settings.dat when missing).
// Returns false and keeps the defaults when the file cannot be used.
bool FLoadDisplaySettings(const std::filesystem::path& userDir, std::string* pmessage = nullptr);
// Writes every option back into Settings.ini (keys this build does not know kept).
bool FSaveDisplaySettings(const std::filesystem::path& userDir);
// The volumes and the speakers (g_saveopts) to the sound (sound.h SetSoundUserVolumes, audio.h SetMonoOutput)
void ApplySoundOptions();
// (migration, once) the save-kept options of the newest save into Settings.ini while it marks them pending
namespace cane::settings { struct SaveOptionValues; }
bool FSaveOptionsPending();
void MigrateSaveOptions(const cane::settings::SaveOptionValues& values);

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
DISPPROJ ProjectionForDisplay(float radFovY, float rAspectCm = 512.0f / 394.24f);

// --aspect / --internal-res style overrides ("fit", "4:3", "16:9", "16:10", "ps2-4:3", "ps2-16:9" or 0..5).
bool FParseAspect(const char* psz, DISPASPECT& aspect);
