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
#if defined(__ANDROID__)
    int refreshRate = 90;               // (90 Hz by default on the Meta Quest)
#else
    int refreshRate = 0;                // 0 the headset's best (up to 120), else Hz (72, 80, 90, 120)
#endif
    float worldScale = 1.0f;            // Immersive: 1 = life size (100 game units a metre)
    bool fCutscenesFlat = false;        // Immersive: the cutscenes on the Flat Screen (Settings.ini "Cutscene View"; default:
                                        // Immersive, the subtitles on a panel following the head; the dance stays Immersive)
    bool fRcTable = false;              // Immersive, experimental: Ep7's RC Combat Club from the table (fixed view)
    int eyeResolution = 0;              // Immersive: 0 dynamic (100 % down to 75 % while frames are missed; from 90 % while
                                        // the compositor upscales, Upscaling), else a fixed %
    int foveation = 2;                  // fixed foveation (no eye tracking): 0 off, 1 low, 2 medium, 3 high (the edges shaded
                                        // more coarsely)
    int msaa = 0;                       // the eyes' MSAA: 2 (2x), 1 off (Vulkan: then one pass an eye, straight into the
                                        // OpenXR images), 0 Automatic (MsaaOption: off on a standalone headset, 2x on a
                                        // PC); at the next VR start
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
//  - BinocTalk: a scripted binocucom talk (a cutscene with the binocucom frame up): the shot seen through the binocucom
//    frame fixed in the room where the head faced when it began (its edges extended, black past them), the shot inside
//    it magnified as the flat picture frames it: turning the head looks at the frame, never away from the shot.
//  - Hack: Bentley's hacking minigame (the player is the hack ship, cid 0x48): from the game camera with its pitch (the
//    arena below, framed as the flat game frames it, filling the view), the head looks around from there.
enum VRVIEW : int { VRVIEW_Normal = 0, VRVIEW_Flat, VRVIEW_Binoc, VRVIEW_RcTable, VRVIEW_BinocTalk, VRVIEW_Hack };
void SetGameView(int view);
// The moment's details for the Immersive view (game.cpp, each frame with SetGameView):
//  - fCutscene: a cutscene (Immersive): the HUD panel (the subtitles) follows the head lazily (smoothed, both ways), the
//    head looks around the scene freely; the subtitle box's top bar left out, its bottom bar kept see-through.
//  - fPause: the pause menu: the world black around the menu's window (its 4:3 canvas on the HUD panel).
//  - fWipe: a transition's wipe: the world black past the HUD panel (the wipe's black reaches the panel's edges), so the
//    wipe and the load behind it cover the whole view, not a box in front.
struct VRMOMENT
{
    bool fCutscene = false;
    bool fPause = false;
    bool fWipe = false;      // a transition's wipe (the Sly mask closing / opening over a load): black past the HUD panel
    bool fDance = false;     // the Ep2 dance: the face buttons as its four (FDanceLayout)
    bool fTitle = false;     // the title screen (shown until New Game / the menu): its bars kept, the HUD panel farther
};                           // away (2 m: the logo and both prompts in view at once)
// The title screen in VR (VRMOMENT::fTitle): hud.cpp keeps the subtitle box's bars there.
bool FTitleMoment();
// The Ep2 dance with the Touch layout's controllers (VRMOMENT::fDance): its four buttons on the four face buttons, laid out
// as on the hands (left X / Y, right A / B): Cross A, Square B (as in play), Circle X, Triangle Y (X / Y are L2 / R2
// otherwise; the grips stay Circle / Triangle too). dance.cpp draws its prompts in that 2 x 2 layout with those icons.
bool FDanceLayout();
void SetMoment(const VRMOMENT& m);
// (hud.cpp, Immersive) a full-screen fill (a fade, a title card's colour, the pause backdrop: 0..1, alpha 0..1) goes over
// the eyes instead of the HUD panel: true when taken (this frame's eyes were drawn). Applied to the next frame's eyes, over
// the world and under the foreground kinds (Sly and his prize over the title card's colour, as the flat game's order).
bool FTakeEyeFill(const glm::vec4& rgba);
// (render.cpp RenderSceneStereo, between the world's pass and the foreground kinds) the fills over the eyes.
void DrawEyeFills(const STEREOVIEW& sv);
// The Immersive cutscene view (the subtitle box's bars: hud.cpp).
bool FCutsceneView();
// Binoc: the binocucom's zoom (game.cpp, each frame): tan(fov / 2) of the camera now over its unzoomed one (1: not
// zoomed in). The eyes' fields of view narrow by it (the picture magnified as in the flat game).
void SetBinocZoom(float k);
// Binoc (Immersive): the game picture is the binocucom's frame over the whole view (its panel fixed to the head, as
// tall as the view): the 4:3 canvas's scale in that picture (display.cpp LayoutDisplay; both < 1: the frame goes on
// past the canvas on all four sides, hud.cpp ExtendEdgeTriangle). False otherwise.
bool FBinocCanvas(glm::vec2* puiScale);
// The game picture over the whole view (Immersive): the binocucom's frame (FBinocCanvas) or a transition's wipe (the
// Sly mask closing / opening over a load, FWipeCanvas): the 4:3 canvas's scale in that picture (display.cpp LayoutDisplay).
bool FWholeViewCanvas(glm::vec2* puiScale);
// (Immersive) a transition's wipe and its load over the whole view: the HUD panel fixed to the head and past the eyes'
// fields, the canvas at the HUD panel's usual angular size, the wipe's black backdrop widened to the panel's edges
// (hud.cpp WidenToPicture), no world past it. The frames without the eyes (the load) on the same panel.
bool FWipeCanvas();
// A queued movie playing (frontend.cpp PlayQueuedMovies, around it): the movie on a screen, the wipe's whole-view layout
// set aside meanwhile (a movie inside a transition's load: a job's end).
void SetMoviePlaying(bool f);
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
// --vr-check: is there an OpenXR runtime with a headset? One "vr-check: ..." line on stdout; 0 = ready (vr.cpp Check).
int Check();
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
// The session runs but has lost the input focus (the Quest's system menu, the guardian: VISIBLE, not FOCUSED): the game
// draws no world then (game.cpp: only the game picture, on the screen), the least GPU work while it is behind the menu.
bool FUnfocused();
// The focus was lost since the game last opened its pause menu for it (hudwidget.cpp opens the pause menu as soon as it
// can, as Quest games do; the player resumes by hand). Cleared when taken, and when the focus comes back first.
bool FPauseForFocus();
void TakePauseForFocus();

// ------------------------------------------------------------ the frame
// Before drawing a game frame: waits for the headset's frame slot (this paces the game) and reads the head.
void BeginFrame();
// This frame draws both eyes (Immersive, the session showing, no full-screen transition).
bool FImmersiveFrame();
// The Immersive view is on now (VR Settings' Mode Immersive, the game's moment not on the Flat Screen: SetGameView).
bool FImmersiveView();
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

// (game.cpp, each frame) the frame's parts on the game thread (ms): VR's perf log, every SLY2N_VR_PERFLOG seconds (20 by
// default, 0 off), with the waits inside them and the GPU's time a frame
void NoteFrameTimes(double msUpdate, double msScene, double msDraw, double msSwap);
// VR Settings' MSAA as it applies: 2 (2x) or 1 (Off); its Automatic: Off on a standalone headset (the Meta Quest, the
// Steam Frame), 2x on a PC
int MsaaOption();
}  // namespace vr
