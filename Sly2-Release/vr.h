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
extern VROPTS g_vropts;

// Asked for (--vr, SLY2N_VR, Android): Start then tries the runtime.
void Request(bool f);
bool FRequested();
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
}  // namespace vr
