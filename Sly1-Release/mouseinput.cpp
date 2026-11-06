// Mouse camera, mouse bindings and mouse menu input. See mouseinput.h.
// From ThiefRush-Dev's Sly1-Release mouseinput.cpp (its multiplayer and
// Vita / Android parts left out).
#include "mouseinput.h"
#include "porttext.h"
#include <cstdio>
#include "../platform/system_settings.h"
#include "../platform/controller_family.h"
#include "debug.h"
#include "Input.h"
#include "sw.h"
#include "gl.h"
#include "cm.h"
#include "ui.h"
#include "prompt.h"
#include "wm.h"
#include "mouse.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cmath>
#include <utility>

MOUSEOPTIONS g_mouseopt;

bool FMouseCameraJoy(const JOY* pjoy)
{
	if (pjoy != &g_joy || !FMouseCaptured())
		return false;
	return true;
}

namespace
{
using cane::settings::kMouseBindingBase;
using cane::settings::kMouseButtonCount;
// Radians of turn per mouse count at 100%, as tuned in the QOL fork.
constexpr float kRadPerCountOrbit = 0.0030f;
constexpr float kRadPerCountAim = 0.0012f;

bool s_fCaptured = false;
bool s_fHaveLast = false;
double s_xLast = 0.0, s_yLast = 0.0;
float s_dxCounts = 0.0f, s_dyCounts = 0.0f; // + = right / down
int s_wheel = 0;                            // + = up
bool s_afButton[kMouseButtonCount] = {};
bool s_afButtonPrev[kMouseButtonCount] = {};
bool s_afButtonBlocked[kMouseButtonCount] = {};
bool s_fImGuiMouse = false;
bool s_fMenuCursorValid = false;
float s_xMenu = 0.0f, s_yMenu = 0.0f;
float s_xMenuLast = -1.0f, s_yMenuLast = -1.0f;
bool s_fMenuMoved = false;
float s_travel = 0.0f;

struct TESTMOUSE
{
	bool fActive = false;
	float dx = 0.0f, dy = 0.0f, x = -1.0f, y = -1.0f;
	int wheel = 0;
	unsigned buttons = 0;
} s_test;

bool FWheelBound()
{
	return std::any_of(g_keyboardBindings.begin(), g_keyboardBindings.end(), [](int key) {
		return key == cane::settings::kMouseWheelUp || key == cane::settings::kMouseWheelDown; });
}

bool FWantCapture(GLFWwindow* window)
{
	if (!g_mouseopt.fCamera || g_fDebugGuiOpen)
		return false;
	if (!s_test.fActive && (glfwGetWindowAttrib(window, GLFW_FOCUSED) != GLFW_TRUE ||
		glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE))
		return false;
	if (g_psw == nullptr || g_ui.uis != UIS_Playing || g_prompt.blots != BLOTS_Hidden || g_wmc.fActive)
		return false;
	// The debug free camera drives the cursor itself.
	if (g_pcm != nullptr && g_pcm->ccpr > 0 && g_pcm->acpr[0].cpp == CPP_ManualOverride)
		return false;
	return true;
}

void SetCapture(GLFWwindow* window, bool fCapture)
{
	if (fCapture)
	{
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, g_mouseopt.fRawInput ? GLFW_TRUE : GLFW_FALSE);
	}
	else
	{
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	}
	s_fCaptured = fCapture;
	s_fHaveLast = false;
}

void NoteMouse()
{
	cane::input::tracker().noteMouse();
}
} // namespace

void MouseFocusChanged()
{
	// A click used to activate the window must not reach gameplay, menus, or binding capture.
	std::fill_n(s_afButtonBlocked, kMouseButtonCount, true);
	s_fHaveLast = false;
	s_dxCounts = s_dyCounts = 0.0f;
	s_wheel = 0;
	(void)MOUSE::GetScrollDX();
	(void)MOUSE::GetScrollDY();
}

void SetTestMouse(bool active, float dx, float dy, int wheel, unsigned buttons, float x, float y)
{
	s_test.fActive = active;
	s_test.dx += dx;
	s_test.dy += dy;
	s_test.wheel += wheel;
	s_test.buttons = buttons;
	if (x >= 0.0f && y >= 0.0f)
	{
		s_test.x = x;
		s_test.y = y;
	}
}

void UpdateMouseInput(GLFWwindow* window)
{
	s_dxCounts = s_dyCounts = 0.0f;
	s_wheel = 0;
	s_fMenuMoved = false;
	s_fMenuCursorValid = false;
	if (window == nullptr)
		return;
	if (s_test.fActive)
	{
		s_wheel = std::exchange(s_test.wheel, 0);
		s_fImGuiMouse = false;
		for (int button = 0; button < kMouseButtonCount; ++button)
		{
			s_afButtonPrev[button] = s_afButton[button];
			s_afButton[button] = (s_test.buttons >> button & 1u) != 0;
		}
		const bool fWant = FWantCapture(window);
		if (fWant != s_fCaptured)
			SetCapture(window, fWant);
		if (s_fCaptured)
		{
			s_dxCounts = std::exchange(s_test.dx, 0.0f);
			s_dyCounts = std::exchange(s_test.dy, 0.0f);
			return;
		}
		s_fMenuCursorValid = s_test.x >= 0.0f;
		s_fMenuMoved = s_fMenuCursorValid && (s_test.x != s_xMenu || s_test.y != s_yMenu);
		s_xMenu = s_test.x;
		s_yMenu = s_test.y;
		return;
	}

	// Wheel notches since the previous update (mouse.cpp accumulates them).
	const double wheel = MOUSE::GetScrollDY();
	s_wheel = static_cast<int>(std::lround(wheel));
	if (s_wheel == 0 && wheel != 0.0)
		s_wheel = wheel > 0.0 ? 1 : -1;

	// ImGui (debug menus, chat entry) owns the mouse while it hovers a window.
	s_fImGuiMouse = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse && !s_fCaptured;
	const bool fFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
	bool fClicked = false;
	for (int button = 0; button < kMouseButtonCount; ++button)
	{
		s_afButtonPrev[button] = s_afButton[button];
		const bool fHeld = glfwGetMouseButton(window, button) == GLFW_PRESS;
		if (!fHeld) s_afButtonBlocked[button] = false;
		s_afButton[button] = fFocused && !s_fImGuiMouse && fHeld && !s_afButtonBlocked[button];
		fClicked |= s_afButton[button] && !s_afButtonPrev[button];
	}
	if (fClicked || (s_wheel != 0 && fFocused))
		NoteMouse();

	const bool fWant = FWantCapture(window);
	if (fWant != s_fCaptured)
		SetCapture(window, fWant);

	double x = 0.0, y = 0.0;
	glfwGetCursorPos(window, &x, &y);
	if (s_fCaptured)
	{
		if (s_fHaveLast)
		{
			const double dx = x - s_xLast, dy = y - s_yLast;
			// Ignore the jump GLFW can report right after the capture changes.
			if (std::abs(dx) < 4000.0 && std::abs(dy) < 4000.0)
			{
				s_dxCounts = static_cast<float>(dx);
				s_dyCounts = static_cast<float>(dy);
			}
		}
		s_xLast = x;
		s_yLast = y;
		s_fHaveLast = true;
		// A deliberate swipe (not a trackpad brush) makes prompts show keys.
		s_travel = s_travel * 0.9f + std::abs(s_dxCounts) + std::abs(s_dyCounts);
		if (s_travel > 60.0f)
		{
			NoteMouse();
			s_travel = 0.0f;
		}
		return;
	}

	// Free cursor: window coordinates to blot (present-rectangle) pixels.
	int windowW = 0, windowH = 0, fbW = 0, fbH = 0;
	glfwGetWindowSize(window, &windowW, &windowH);
	glfwGetFramebufferSize(window, &fbW, &fbH);
	if (!fFocused || windowW <= 0 || windowH <= 0 || fbW <= 0 || fbH <= 0)
		return;
	const float fbX = static_cast<float>(x) * static_cast<float>(fbW) / static_cast<float>(windowW);
	const float fbY = static_cast<float>(y) * static_cast<float>(fbH) / static_cast<float>(windowH);
	s_xMenu = fbX - static_cast<float>(g_gl.presentX);
	s_yMenu = fbY - static_cast<float>(g_gl.presentY);
	s_fMenuCursorValid = !s_fImGuiMouse;
	if (std::abs(s_xMenu - s_xMenuLast) >= 1.0f || std::abs(s_yMenu - s_yMenuLast) >= 1.0f)
	{
		s_fMenuMoved = s_xMenuLast >= 0.0f; // the first sample is not a move
		s_xMenuLast = s_xMenu;
		s_yMenuLast = s_yMenu;
		if (s_fMenuMoved && g_mouseopt.fMenus && g_prompt.blots != BLOTS_Hidden)
			NoteMouse();
	}
}

void ApplyMouseOptions()
{
	if (s_fCaptured && g_gl.window != nullptr && glfwRawMouseMotionSupported())
		glfwSetInputMode(g_gl.window, GLFW_RAW_MOUSE_MOTION, g_mouseopt.fRawInput ? GLFW_TRUE : GLFW_FALSE);
}

bool FMouseBindingHeld(int binding)
{
	if (binding == cane::settings::kMouseWheelUp)
		return s_wheel > 0;
	if (binding == cane::settings::kMouseWheelDown)
		return s_wheel < 0;
	const int button = binding - kMouseBindingBase;
	return button >= 0 && button < kMouseButtonCount && s_afButton[button];
}

int MouseBindingPressed(GLFWwindow* window)
{
	if (window == nullptr || glfwGetWindowAttrib(window, GLFW_FOCUSED) != GLFW_TRUE)
		return -1;
	for (int button = 0; button < kMouseButtonCount; ++button)
		if (FMouseBindingHeld(kMouseBindingBase + button))
			return kMouseBindingBase + button;
	if (s_wheel > 0)
		return cane::settings::kMouseWheelUp;
	if (s_wheel < 0)
		return cane::settings::kMouseWheelDown;
	return -1;
}

bool FMouseCaptured() { return s_fCaptured; }

float MouseCameraOrbit()
{
	if (!s_fCaptured)
		return 0.0f;
	return s_dxCounts * kRadPerCountOrbit * (static_cast<float>(g_mouseopt.sensitivity) / 100.0f) *
		(g_mouseopt.fInvertX ? -1.0f : 1.0f);
}

void MouseAimDelta(float* pdxRight, float* pdyUp)
{
	*pdxRight = *pdyUp = 0.0f;
	if (!s_fCaptured)
		return;
	const float scale = kRadPerCountAim * (static_cast<float>(g_mouseopt.aimSensitivity) / 100.0f);
	*pdxRight = s_dxCounts * scale * (g_mouseopt.fInvertX ? -1.0f : 1.0f);
	*pdyUp = -s_dyCounts * scale * (g_mouseopt.fInvertY ? -1.0f : 1.0f);
}

float MouseWheelZoom()
{
	if (!s_fCaptured || !g_mouseopt.fWheelZoom || FWheelBound())
		return 0.0f;
	return static_cast<float>(s_wheel);
}

static bool FMouseMenuEnabled() { return g_mouseopt.fMenus && !s_fCaptured && !s_fImGuiMouse; }

bool MouseMenuCursor(float* px, float* py)
{
	if (!FMouseMenuEnabled() || !s_fMenuCursorValid)
		return false;
	*px = s_xMenu;
	*py = s_yMenu;
	return true;
}
bool FMouseMenuMoved() { return FMouseMenuEnabled() && s_fMenuMoved; }
bool FMouseLeftPressed()
{
	return FMouseMenuEnabled() && s_afButton[GLFW_MOUSE_BUTTON_LEFT] && !s_afButtonPrev[GLFW_MOUSE_BUTTON_LEFT];
}
bool FMouseRightPressed()
{
	return FMouseMenuEnabled() && s_afButton[GLFW_MOUSE_BUTTON_RIGHT] && !s_afButtonPrev[GLFW_MOUSE_BUTTON_RIGHT];
}
int MouseWheelNotches() { return FMouseMenuEnabled() ? s_wheel : 0; }

const char* PchzMouseBindingName(int binding)
{
	switch (binding - cane::settings::kMouseBindingBase)
	{
	case 0: return PchzPort(PTX_MouseLeft);
	case 1: return PchzPort(PTX_MouseRight);
	case 2: return PchzPort(PTX_MouseMiddle);
	case 3: case 4: case 5: case 6: case 7:
	{
		static char s_aach[5][32];
		const int index = binding - cane::settings::kMouseBindingBase;
		std::snprintf(s_aach[index - 3], sizeof(s_aach[0]), PchzPort(PTX_MouseN), index + 1);
		return s_aach[index - 3];
	}
	case 8: return PchzPort(PTX_WheelUp);
	case 9: return PchzPort(PTX_WheelDown);
	default: return PchzPort(PTX_MouseButton);
	}
}
