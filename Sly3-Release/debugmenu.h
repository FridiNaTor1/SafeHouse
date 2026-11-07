#pragma once

// PC debug menu (not in the retail game; in the spirit of the March 2004 prototype's debug menu, which the retail
// build compiled out). Only with the launcher's Settings > Advanced > Debug mode on (pcinput.h fDebugKeys): with it
// off nothing here reads or swallows any input and the game runs exactly as without this file.
//
// F10, or R1 + Select on a pad, opens it: the game freezes (no world update at all) and a translucent panel lists
// Warps / Character / Noclip / Teleport / Jobs / Player / Speed / Display / Reset World / Save. D-pad or stick (or the
// arrow keys / W S / mouse wheel) move, Cross (Enter / Space / left click) selects, Circle / Triangle / Start (Esc /
// E / Q / right click) go back. F11 toggles the HUD.
//
// Everything that changes the save is marked "[save]" in the menu; the rest only changes the running game.

#include <cstdint>

struct SW;
struct LO;
struct GLFWwindow;

// joy.cpp UpdateJoy, after the step's buttons / sticks are known (the window's and scripted presses): the menu's
// open / close keys and its own input. Returns the buttons the game gets (all of them taken while the menu is open,
// and the ones still held from closing it until they are released); the sticks are zeroed while it is open.
uint16_t GrfDebugMenuFilter(uint16_t grf, float* pxl, float* pyl, float* pxr, float* pyr, uint8_t abAxis[4]);

// The menu is open: game.cpp's step runs UpdateDebugMenu instead of the world.
bool FDebugMenuOpen();
void UpdateDebugMenu(SW* psw);

// game.cpp's step, after the world's update (where the F keys are handled): the queued menu actions, noclip, the
// time scale, infinite health, F11.
void UpdateDebugTools(SW* psw);

// hudwidget.cpp DrawHud: the HUD hidden (F11; the game's own menus stay).
bool FDebugHudHidden();
// game.cpp, after the HUD and the foreground objects drawn over it: the menu and the perf line, a 2D pass of their own.
void DrawDebugOverlay(int width, int height);

// update.cpp UpdateSw: the object's update (vt slot 16) is skipped (the noclip's player, placed by the menu instead).
bool FDebugSkipUpdate(const SW* psw, const LO* plo);

// cm.cpp AsolidCm: the noclip is on (the current world's player flies): the camera ignores the world's solids too, so
// it follows the flight through walls instead of being pushed out or cut in front of them (debug menu only).
bool FDebugNoclipCamera();

// game.cpp, each frame while the F1 free camera is on: its pad controls (the active gamepad, or the scripted presses
// of a test run: SLY2N_FREECAMAT=frame turns the free camera on at that frame). Left stick moves (forward / back along
// the view, strafes), right stick looks, R2 / L2 up / down, R1 held fast, L1 held slow, Select leaves it (R1 + Select:
// the debug menu, as always). Returns false when it left the free camera.
struct CAMERA;
bool FUpdateDebugFreeCamPad(CAMERA& cam, int frame);

// game.cpp: the F1 free camera's flag (the menu's Display page toggles it too) and F7's infinite health.
extern bool g_fDebugFreeCam;
extern bool g_fDebugInfinite;

// The build's version for the title line: the top-level CMake project version and the short git hash.
const char* PchzPortVersion();
