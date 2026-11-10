#pragma once

// PC input QoL (Phase 3, ported from ThiefRush-Dev's Sly1-Release Input.cpp / mouseinput.cpp): the keyboard and
// gamepad bindings, the keyboard camera keys, the mouse (camera, aim, bindings, menus) and the last used device
// (for the dynamic button prompts, promptglyph.h). The options live in <user-dir>/Settings.ini
// (platform/sly2_settings.h; the launcher's Settings page edits the same file).
//
// Defaults keep the original's controls: Cross / Circle / ... on the pad's positional buttons, the triggers as
// L2 / R2. Nothing here runs in headless or scripted runs (no window): those keep reading only the press lists.

#include <array>
#include <cstdint>

struct GLFWwindow;
namespace cane::settings { struct SystemSettings; }

// The actions in the settings' order (Sly1-Release JOY_BUTTON): the bindings' index.
enum PCACTION : int
{
    PCA_Up, PCA_Down, PCA_Left, PCA_Right, PCA_Square, PCA_Circle, PCA_Cross, PCA_Triangle,
    PCA_Start, PCA_Select, PCA_L1, PCA_R1, PCA_L2, PCA_R2, PCA_L3, PCA_R3, PCA_Max
};

struct PCINPUTOPT
{
    std::array<int, PCA_Max> keyboard{};       // GLFW keys (or settings::kMouseBindingBase.. mouse bindings)
    std::array<int, PCA_Max> gamepad{};        // GLFW gamepad buttons, 1000 LT, 1001 RT
    std::array<int, 4> cameraKeys{};           // Left Right Up Down (the right stick)
    std::array<int, 2> tankKeys{};             // (settings v19) the tank's left / right track (FTankSticks)
    bool fDisableUnfocusedPad = false;
    // the mouse
    bool fMouseCamera = true;
    int mouseSensitivity = 100;                // % (the follow camera)
    int mouseAimSensitivity = 100;             // % (binocucom, aiming)
    bool fMouseInvertX = false, fMouseInvertY = false;   // the follow camera (and under a table)
    // (settings v17) the mouse in the binocucom and the turrets; the controller's camera (the right stick on the follow
    // camera and under a table: the original's orbit when off). Off: mouse up looks up and mouse right turns right in
    // every view; the controller as the original. The controller's binocucom / turret inversion is the original's own
    // option (the save's flags fReverseLookAround / fReverseAim).
    bool fMouseInvertBinocX = false, fMouseInvertBinocY = false;
    bool fMouseInvertTurretX = false, fMouseInvertTurretY = false;
    bool fPadInvertCameraX = false, fPadInvertCameraY = false;
    bool fAutoCameraRotation = true;           // passive follow-camera steering; scripted cameras still apply
    bool fMouseRaw = true;
    bool fMouseWheelZoom = true;
    bool fMouseMenus = true;
    // prompts
    int promptStyle = 0;                       // settings::PromptStyleValue (0 Auto)
    bool fAlwaysOriginalPs = false;            // "Original PlayStation Icons"
    // cutscenes (skipcut.h)
    bool fInstantSkipCutscenes = false;
    // the hacking minigame (hackmouse.h): with the keyboard / mouse, a cursor; left click fires toward it
    bool fHackMouseAim = true;
    // the debug function keys and the debug menu (game.cpp, debugmenu.cpp): the launcher's Settings > Advanced >
    // Debug mode (Debug Mode.ini, ApplyPcDebugMode); off by default. The settings' v14 debugKeys is ignored.
    bool fDebugKeys = false;
};
extern PCINPUTOPT g_pcinput;
// (PC) a script read a pad's left and right stick Y (spliceglue.cpp: the Ep5 RC tank drives each track by one stick):
// with the keyboard, the tank keys then drive the tracks (ReadPcInput)
void NoteScriptStickRead(int optid);
bool FTankSticks();

PCINPUTOPT PcInputDefaults();
void PcInputFromSettings(const cane::settings::SystemSettings& s);
void PcInputToSettings(cane::settings::SystemSettings& s);

// (tests) SLY2N_PCOPT="alwaysps=1,instantskip=1,promptstyle=2,mousecamera=0,pad6=1,key4=81": options over the
// settings file (padN / keyN: action N's gamepad / keyboard binding; the inversions: invcx / invcy (mouse, camera),
// invbx / invby (mouse, binocucom), invtx / invty (mouse, turrets), cinvx / cinvy (controller, camera))
void ApplyPcInputEnv();
// fDebugKeys from Debug Mode.ini in the user folder (cane::debugMode(): the launcher's Settings > Advanced > Debug mode)
void ApplyPcDebugMode();
// After the GL context exists: the prompt atlases, the forced prompt family of the test environment.
void StartupPcInput();

// The window's input for one game step (UpdateJoy): the buttons in libpad's layout (joy.h BTN_*) and the sticks
// (-1..1, y up). fPad: a gamepad is there. Also updates the mouse and the last used device.
void ReadPcInput(GLFWwindow* window, uint16_t* pgrf, float* pxl, float* pyl, float* pxr, float* pyr, bool* pfPad);

// ---- the mouse (mouseinput.h of ThiefRush)
// The cursor is captured while the game is played (no menu, no title, the window focused).
bool FMouseCaptured();
// This step's camera turn in radians from the mouse (0 while the cursor is free): pan + = mouse right, tilt + =
// mouse up (look up).
void MouseOrbitDelta(float* pdradPan, float* pdradTilt);
// This step's aim turn (binocucom, turrets), radians: + = right / up (each view's own inversion applied).
enum MOUSEAIM { MOUSEAIM_Binocucom, MOUSEAIM_Turret };
void MouseAimDelta(MOUSEAIM aim, float* pdxRight, float* pdyUp);
// Wheel notches this step for the binocucom's zoom (0 when the wheel is bound or the option is off).
float MouseWheelZoom();
const char* PchzMouseBindingName(int binding);
// The free cursor (hackmouse.h's mode: the capture released, the OS cursor hidden): its place in the picture (NDC
// -1..1, y up) and whether the left button is down. False when there is no window or the cursor is captured (outside the
// picture: held at its edge).
bool FCursorPicture(float* pxNdc, float* pyNdc, bool* pfLeft);
// The game reads the window's input (ReadPcInput has run: not a headless or scripted run).
bool FPcInputLive();
const char* PchzKeyName(int key);

// A menu, the title or the episode menu has the pad: the camera keys step the highlight (the d-pad), Enter
// confirms (Cross), the mouse wheel / left / right buttons step / confirm / go back.
bool FPcMenuMode();

// ---- the free cursor in the menus (PC menus): hover and click on the 4:3 canvas
// A menu the mouse can use is up (the pause / options / card pages, the title, the episode menu, the debug menu, the
// gadget grid, Job Help): the OS cursor is shown there, unless a controller was used last and the mouse has not
// moved since. Everywhere else with the cursor free (cutscenes, talks, loads, movies) it is hidden.
bool FMouseMenuUp();
// The cursor in canvas units (640 x 448, y down: the 4:3 frame, wherever it sits in a wider picture) while it is
// free and shown in such a menu with "Mouse in Menus" on; false otherwise (always false headless).
bool FMenuCursor(float* pxCanvas, float* pyCanvas);
// The cursor moved this step (hover selects only then: a still cursor never fights the keys or the pad).
bool FMenuCursorMoved();
// A menu with its own mouse targets calls this from its update while the cursor is over one: the left button's
// next press is then not sent as the confirm button but given to that menu (FMenuClick).
void SetMenuMouseTarget();
// The left button was pressed this step over a menu's own target (see SetMenuMouseTarget).
bool FMenuClick();
// (tests) SLY2N_TESTCURSOR=x:y@frame+count,...: the menu cursor at canvas (x, y) for those steps (window runs; the
// left button through SLY2N_TESTKEYS=2000@frame).

// A debug function key (GLFW_KEY_F1..) held: only with g_pcinput.fDebugKeys on (else always false); in the window, or
// a SLY2N_TESTKEYS key (window may be null then). The SLY2N_* test switches do not go through this.
bool FDebugKeyHeld(GLFWwindow* window, int key);

// ---- capture for the in-game rebinding (menu.cpp's Controls page)
// The first newly pressed key or mouse button this step (or -1); Escape gives settings::kKeyEscape.
int KeyBindingPressed(GLFWwindow* window);
