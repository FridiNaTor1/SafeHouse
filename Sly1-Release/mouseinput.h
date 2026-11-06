#pragma once
// Mouse camera, mouse bindings and mouse menu input (from ThiefRush-Dev's
// Sly1-Release mouseinput.cpp). The options are part of Saves/system_settings.dat
// (v11 fields, platform/system_settings.h), so the launcher edits them too.
//
//   Mouse camera   While playing (no menu or session overlay open) the cursor
//                  is captured; movement orbits the third-person camera and
//                  aims the binocucom (and turrets).
//   Bindings       Mouse buttons 1-8 and the wheel are keyboard bindings
//                  (cane::settings::kMouseBindingBase...).
//   Menus          Hover highlights a prompt response, left click selects
//                  it, right click goes back and the wheel moves the highlight.

struct GLFWwindow;
struct JOY;

struct MOUSEOPTIONS
{
	bool fCamera = true;
	int sensitivity = 100;       // percent, third-person orbit
	int aimSensitivity = 100;    // percent, binocucom / turrets
	bool fInvertX = false;
	bool fInvertY = false;
	bool fRawInput = true;
	bool fWheelZoom = true;      // the wheel zooms the binocucom when unbound
	bool fMenus = true;
};
extern MOUSEOPTIONS g_mouseopt;

// Once per gameplay JOY::Update: button states, the motion since the previous
// call and the cursor capture.
void UpdateMouseInput(GLFWwindow* window);
// Called on focus loss and gain: ignore the activation click until its release.
void MouseFocusChanged();
// Re-applies raw motion after the option changes.
void ApplyMouseOptions();

// Bindings (codes from cane::settings::kMouseBindingBase).
bool FMouseBindingHeld(int binding);
// Binding capture: the first held mouse button or this update's wheel, or -1.
int MouseBindingPressed(GLFWwindow* window);
const char* PchzMouseBindingName(int binding);

// Camera, for this update's motion (0 while the cursor is free).
bool FMouseCaptured();
// The pad a camera reads is this machine's own (g_joy outside another
// player's callback scope) and the mouse is captured.
bool FMouseCameraJoy(const JOY* pjoy);
float MouseCameraOrbit();                         // radians, + = like stick right
void MouseAimDelta(float* pdxRight, float* pdyUp); // radians
float MouseWheelZoom();                            // notches, + = zoom in

// Menus. Coordinates are blot (present-rectangle) pixels.
bool MouseMenuCursor(float* px, float* py);
bool FMouseMenuMoved();
bool FMouseLeftPressed();
bool FMouseRightPressed();
int MouseWheelNotches();

// Session fixture (test_input "mouse"): while active, UpdateMouseInput reads
// these instead of the window (focus assumed). dx/dy and wheel apply once;
// buttons (bit = GLFW mouse button) stay held; x/y < 0 keep the cursor.
void SetTestMouse(bool active, float dx, float dy, int wheel, unsigned buttons, float x, float y);
