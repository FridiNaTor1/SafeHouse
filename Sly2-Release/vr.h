#pragma once

// VR (OpenXR): Sly 2 on a headset. notes/VR_2026-11-10.md.
//
// Off unless asked for: on the desktop with --vr (or SLY2N_VR=1), on Android when the device has an OpenXR runtime
// (the Meta Quest). The OpenXR loader is opened at run time (no link-time dependency): without it, or without a
// headset, the game runs exactly as before. Nothing here runs while VR is off.
//
// Two modes (the pause menu's VR Settings, Settings.ini [VR]):
//  - Flat Screen: the normal game picture (HUD and all) on a large virtual screen in front of the player.
//  - Immersive: a stereo view from the game camera's position, level with the horizon; the head looks around and
//    moves (6DoF) from there; the HUD and menus float on a panel in front. The automatic camera rotation is off by
//    default here (VR Settings), and a comfort vignette can darken the edges while the camera turns or moves.
// The animated full-screen transitions and the movies are shown on the screen in both modes.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

struct CAMERA;
struct STEREOVIEW;
struct GLFWwindow;
struct GLFWgamepadstate;

namespace vr
{
enum VRMODE : int { VRMODE_Flat = 0, VRMODE_Immersive = 1 };

// Settings.ini [VR] (platform/sly2_settings.h Sly2VrOptions mirrors these)
// VR Settings Upscaling: the headset compositor's filter on the eyes where the runtime offers it (Quest: Off, quality
// sharpening, or Quest Super Resolution = quality super sampling + sharpening). With it on, dynamic eye resolution
// works between 90 % and 75 % (kEyeResCeilingUpscaled), off between 100 % and 75 %.
enum VRUPSCALE : int { VRUPSCALE_Off = 0, VRUPSCALE_Sharpen = 1, VRUPSCALE_SuperResolution = 2 };
constexpr float kEyeResCeilingUpscaled = 0.9f;
struct VROPTS
{
    int mode = VRMODE_Immersive;
    bool fAutoCameraRotation = false;   // Immersive: the follow camera's passive steering (the flat game's option otherwise)
    int vignette = 0;                   // 0 off, 1 low, 2 medium, 3 high
    int screenSize = 1;                 // Flat Screen: 0 small, 1 medium, 2 large
    int refreshRate = 0;                // 0 the headset's best (up to 120), else Hz (72, 80, 90, 120)
    float worldScale = 1.0f;            // Immersive: 1 = life size (100 game units a metre)
    bool fCutscenesFlat = true;         // Immersive: the cutscenes (and the dance, the RC chopper) on the Flat Screen
    bool fRcTable = false;              // Immersive, experimental: Ep7's RC Combat Club from the table (fixed view)
    int eyeResolution = 0;              // Immersive: 0 dynamic (100 % down to 75 % while frames are missed; from 90 % while
                                        // the compositor upscales, Upscaling), else a fixed %
    int foveation = 2;                  // fixed foveation (no eye tracking): 0 off, 1 low, 2 medium, 3 high (the edges shaded
                                        // more coarsely)
    int msaa = 2;                       // the eyes' MSAA: 2 (2x), 1 off (Vulkan: then one pass an eye, straight into the
                                        // OpenXR images); at the next VR start
    bool fMultiview = true;             // (Vulkan) both eyes in one render pass (VK_KHR_multiview) where the device can
    int upscaling = 2;                  // the compositor's filter for the eyes (XR_FB_composition_layer_settings, Meta's
                                        // runtimes): VRUPSCALE_*
};

// The game's moment, for the Immersive view (game.cpp sets it each frame, before FEyeViews):
//  - Normal: from the game camera (its heading), the head looks around.
//  - Flat: shown on the Flat Screen for its duration (cutscenes, the Ep2 dance, the RC chopper), then back.
//  - Binoc: the binocucom's first-person view; its pan / tilt follow the head (FHeadLook), its eye moves with it.
//  - RcTable: the RC Combat Club from a fixed place at the table (the view captured when the match starts, not
//    following the car); the car steered by the stick relative to itself, its gun aimed where the head looks.
enum VRVIEW : int { VRVIEW_Normal = 0, VRVIEW_Flat, VRVIEW_Binoc, VRVIEW_RcTable };
void SetGameView(int view);
// Binoc: the head's turn (radians: pan + left, tilt + up) since the last call; false (zeros) otherwise.
bool FHeadLook(float* pdradPan, float* pdradTilt);
// RcTable: on (the car's controls and aim change: vehiclecontrol.cpp); the point on the arena the head looks at.
bool FRcTableActive();
bool FRcAimPoint(glm::vec3* ppos);
// RcTable: the arena's height there (the plane the gaze meets: the car's), each step
void SetRcArenaHeight(float z);
// The headset's compositor filters the eyes (XR_FB_composition_layer_settings: the Quest): VR Settings shows Upscaling
// (VROPTS::upscaling) then; otherwise the Display options' upscaler belongs there (menu.cpp AentVr)
bool FCompositorUpscaling();
extern VROPTS g_vropts;

// Asked for (--vr, SLY2N_VR, Android): Start then tries the runtime.
void Request(bool f);
bool FRequested();
// Before any graphics exist (renderer.cpp's choice): an OpenXR loader and a runtime answer here (a headset), so a
// session can be tried; false on a phone or a PC without a runtime. Cached; cheap after the first call.
bool FRuntimeAvailable();
// (renderer.cpp, VR on the Vulkan renderer) the OpenXR instance made first with XR_KHR_vulkan_enable2, and the hooks
// that make the Vulkan instance / device through it (a CaneVk::ExternalDevice*); null: no runtime / headset, or the
// runtime has no Vulkan path (VR then runs on OpenGL)
const void* PvulkanDevice();
// The Vulkan renderer did not start: VR goes on OpenGL (the instance is made again at FStart).
void VulkanFailed();
// After the GL context exists: the runtime, the headset, the session. False (logged): no VR, the game runs flat.
bool FStart();
// A session exists (the headset is in use: frames go to it).
bool FActive();
// The pause menu's VR Settings page is shown (a headset is in use).
bool FMenuShown();
void Shutdown();

// The OpenXR runtime asked the game to stop (the headset's system menu: Quit).
bool FExitRequested();

// ------------------------------------------------------------ the frame
// Before drawing a game frame: waits for the headset's frame slot (this paces the game) and reads the head.
void BeginFrame();
// This frame draws both eyes (Immersive, the session showing, no full-screen transition).
bool FImmersiveFrame();
// The two eyes' views of the game camera, into the eyes' targets of this frame.
bool FEyeViews(const CAMERA& cam, STEREOVIEW* psv);
// After the eyes: the comfort vignette over them.
void FinishEyes();
// GL::Present in VR: the game picture to the screen / HUD panel, the frame to the headset, the window's mirror.
// False: not handled (VR off).
bool FPresent();
// The window's buffer swap is skipped in VR on Android (no window shows); the desktop keeps its mirror window.
bool FSkipSwap();

// ------------------------------------------------------------ the controllers
// The headset's controllers as a pad (PS2 layout: pcinput.cpp ReadPcInput uses it like a controller).
bool FPadState(GLFWgamepadstate* pstate);
// The controllers' interaction profile ("/interaction_profiles/oculus/touch_controller"), empty when none.
const std::string& StrInteractionProfile();
// Rumble on the controllers (0..1 each motor).
void Rumble(float gLow, float gHigh);
// VR Settings' refresh rate taken now.
void ApplyRefreshRate();
// Re-anchor the view on the current head position and heading.
void Recenter();

// The automatic camera rotation in effect (Immersive: VR Settings' own option).
bool FAutoCameraRotation(bool fFlatOption);

// In VR the game picture is a 1920 x 1080 screen (display.cpp LayoutDisplay): its size, false when VR is off.
bool FPanelSize(int* pdx, int* pdy);

// (Android, XR_KHR_android_thread_settings) the calling thread told to the runtime as one of the game's (it then
// runs on the big cores at the runtime's priority): the game thread registers itself at the session's start; a worker
// thread calls this once when it starts (also before a session: it is registered when one begins). No-op elsewhere.
enum VRTHREAD { VRTHREAD_Worker = 0, VRTHREAD_RendererWorker = 1 };
void NoteThread(VRTHREAD kind);
}  // namespace vr
