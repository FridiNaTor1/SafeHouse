#pragma once

// Phase 3 QoL (keyboard / mouse only): mouse aim in Bentley's hacking minigame (the ship, cid 0x48, hack.cpp).
// While the ship is driven and the keyboard / mouse is the last used device, the game draws a small crosshair at
// the mouse (the capture is released, the OS cursor hidden: pcinput.cpp) and the ship's guns point at the place in the
// hack level under it (the cursor's ray through the hack camera's projection, met with the ship's plane); left click
// (held) fires, as the right stick's deflection does on a pad. The left click's own binding is not sent meanwhile.
// A gamepad keeps the original controls exactly (no cursor). Option: Pause > Options > Controls and Prompts > "Mouse
// Aim in Hacking" (g_pcinput.fHackMouseAim, settings v13 hackMouseAim; default on).
//
// Tests (headless): SLY2N_TESTCURSOR="X Y@F0-F1[c];..." puts the cursor at canvas pixel (X, Y) (640 x 448, the 2D
// canvas) for the input frames [F0, F1), the left button held with "c", as if the mouse were the last used device.

struct LO;

// The cursor is up this frame (the ship driven, keyboard / mouse, the option on, no menu or talk).
bool FHackMouseMode();
// (hack.cpp UpdateShip, the driven state) with the mouse: the guns' world heading and whether they fire. False: the
// pad's right stick as the original.
bool FHackMouseAim(LO* plo, float* pradAim, bool* pfFire);
// (DrawHud) the crosshair over the 2D.
void DrawHackCursor();

// hack.cpp: the ship's state (-1 removed, 0 idle, 1 driven, 2 segment, 3 docked, 4 dead), -2 for other objects
int HackShipState(LO* plo);
