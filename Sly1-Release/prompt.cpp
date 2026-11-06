#include "textmod.h"
#include "prompt.h"
#include "ui.h"
#include "dialog.h"
#include "wm.h"
#include "sm.h"
#include "credit.h"
#include "coin.h"
#include "pzo.h"
#include "binoc.h"
#include "rog.h"
#include "ctr.h"
#include "screen.h"
#include "gl.h"
#include "gui_layout.h"
#include "prompt_glyphs.h"
#include "mouseinput.h"
#include "save.h"
#include "sound.h"
#include "../platform/system_settings.h"
#include "content.h"
#include "porttext.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

namespace
{
int s_iKeyboardBindingCapture = -1;
bool s_fKeyboardBindingCaptureArmed = false;
bool s_fControllerBindingCapture = false;

// The keyboard / controller mapping's rows (the shoulder buttons keep their names in every language).
const char* PchzKeyboardAction(int button)
{
    static constexpr PTX s_aptx[] =
    {
        PTX_MoveUp, PTX_MoveDown, PTX_MoveLeft, PTX_MoveRight,
        PTX_Square, PTX_Circle, PTX_Cross, PTX_Triangle, PTX_Start, PTX_Select
    };
    static constexpr const char* s_apchzShoulder[] = { "L1", "R1", "L2", "R2", "L3", "R3" };
    if (button >= 0 && button < static_cast<int>(std::size(s_aptx)))
        return PchzPort(s_aptx[button]);
    const int shoulder = button - static_cast<int>(std::size(s_aptx));
    return shoulder >= 0 && shoulder < static_cast<int>(std::size(s_apchzShoulder)) ? s_apchzShoulder[shoulder] : "";
}

int KeyboardKeyPressed(GLFWwindow* window)
{
    for (int key = GLFW_KEY_0; key <= GLFW_KEY_9; ++key)
        if (glfwGetKey(window, key) == GLFW_PRESS) return key;
    for (int key = GLFW_KEY_A; key <= GLFW_KEY_Z; ++key)
        if (glfwGetKey(window, key) == GLFW_PRESS) return key;
    for (int key = GLFW_KEY_F1; key <= GLFW_KEY_F25; ++key)
        if (glfwGetKey(window, key) == GLFW_PRESS) return key;
    for (int key = GLFW_KEY_KP_0; key <= GLFW_KEY_KP_EQUAL; ++key)
        if (glfwGetKey(window, key) == GLFW_PRESS) return key;

    constexpr int specialKeys[] =
    {
        GLFW_KEY_SPACE, GLFW_KEY_APOSTROPHE, GLFW_KEY_COMMA, GLFW_KEY_MINUS,
        GLFW_KEY_PERIOD, GLFW_KEY_SLASH, GLFW_KEY_SEMICOLON, GLFW_KEY_EQUAL,
        GLFW_KEY_LEFT_BRACKET, GLFW_KEY_BACKSLASH, GLFW_KEY_RIGHT_BRACKET,
        GLFW_KEY_GRAVE_ACCENT, GLFW_KEY_ENTER, GLFW_KEY_TAB, GLFW_KEY_BACKSPACE,
        GLFW_KEY_INSERT, GLFW_KEY_DELETE, GLFW_KEY_RIGHT, GLFW_KEY_LEFT,
        GLFW_KEY_DOWN, GLFW_KEY_UP, GLFW_KEY_PAGE_UP, GLFW_KEY_PAGE_DOWN,
        GLFW_KEY_HOME, GLFW_KEY_END, GLFW_KEY_CAPS_LOCK, GLFW_KEY_SCROLL_LOCK,
        GLFW_KEY_NUM_LOCK, GLFW_KEY_PRINT_SCREEN, GLFW_KEY_PAUSE,
        GLFW_KEY_LEFT_SHIFT, GLFW_KEY_LEFT_CONTROL, GLFW_KEY_LEFT_ALT,
        GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SHIFT, GLFW_KEY_RIGHT_CONTROL,
        GLFW_KEY_RIGHT_ALT, GLFW_KEY_RIGHT_SUPER, GLFW_KEY_MENU
    };

    for (int key : specialKeys)
        if (glfwGetKey(window, key) == GLFW_PRESS) return key;
    return GLFW_KEY_UNKNOWN;
}

int GamepadBindingPressed()
{
    if (g_fDisableControllerInputWhenUnfocused &&
        (g_gl.window == nullptr ||
            glfwGetWindowAttrib(g_gl.window, GLFW_FOCUSED) != GLFW_TRUE))
    {
        return -1;
    }

    if (g_joy.joystickId < GLFW_JOYSTICK_1 || g_joy.joystickId > GLFW_JOYSTICK_LAST)
        return -1;

    GLFWgamepadstate state{};
    if (!glfwGetGamepadState(g_joy.joystickId, &state))
        return -1;

    for (int button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; ++button)
        if (state.buttons[button] == GLFW_PRESS) return button;
    if (state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.25f)
        return GAMEPAD_BINDING_LEFT_TRIGGER;
    if (state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.25f)
        return GAMEPAD_BINDING_RIGHT_TRIGGER;
    return -1;
}

bool FGamepadAvailable()
{
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
    {
        if (glfwJoystickPresent(jid) && glfwJoystickIsGamepad(jid))
            return true;
    }
    return false;
}

// The family the Controller Mapping labels were last written for.
cane::input::Family s_familyMapping = cane::input::Family::Keyboard;

void BuildBindingStrings(PRK prk)
{
	s_familyMapping = PromptFamily();
	for (int button = 0; button < BTN_MAX; ++button)
	{
		const char* bindingName = prk == PRK_ControllerMapping
			? PchzGamepadBindingName(g_gamepadBindings[button], s_familyMapping)
			: PchzKeyboardKeyName(g_keyboardBindings[button]);
		std::snprintf(g_aachzKeyboardBindings[button], sizeof(g_aachzKeyboardBindings[button]),
			"%s%s%s", PchzKeyboardAction(button), PchzPort(PTX_Sep), bindingName);
	}
}

bool FMappingPrompt(PRK prk)
{
	return prk == PRK_KeyboardMapping || prk == PRK_ControllerMapping;
}

// The port's own option pages. Their rows outgrew a 720-pixel-high window,
// so SetPromptPrk fits them to the screen: a narrower font when the widest
// row is too wide, then a scrolling window of rows around the selection.
// The disc's own menus never reach this path.
bool FPortOptionPage(PRK prk)
{
	return prk == PRK_VideoMenu || prk == PRK_SoundMenu || prk == PRK_MouseMenu || prk == PRK_ControlsMenu;
}

// What SetPromptPrk last fitted, and the screen it fitted to.
struct PROMPTFIT
{
	PRK prk = PRK_Nil;
	int crespkVisible = 0;
	float rScaleRespk = 1.0f;
	int width = 0, height = 0;
	float xScale = 0.0f, yScale = 0.0f;
};
PROMPTFIT s_promptFit;

// The height of the strips that hold a fitted page's scroll arrows.
constexpr float kPromptScrollMark = 10.0f;

float RScaleRespkPrompt(PRK prk, const PRD& prd)
{
	return s_promptFit.prk == prk ? prd.rScaleRespk * s_promptFit.rScaleRespk : prd.rScaleRespk;
}

int CrespkVisiblePrompt(PRK prk, int crespk)
{
	return s_promptFit.prk == prk ? std::min(crespk, s_promptFit.crespkVisible) : crespk;
}

// The window or GUI scale changed since the page was fitted.
bool FPromptFitStale(PRK prk)
{
	if (s_promptFit.prk != prk)
		return false;
	const GuiScale scale = GetGuiScale();
	return s_promptFit.width != g_gl.width || s_promptFit.height != g_gl.height ||
		s_promptFit.xScale != scale.x || s_promptFit.yScale != scale.y;
}

PRD& PrdPrompt(PRK prk)
{
	// Keep PC-only mapping descriptors out of the release descriptor table.
	// The rest of the prompt system can still treat them exactly like a PRD,
	// without indexing beyond the original table storage in stale/incremental
	// builds.
	static PRD keyboardMapping =
	{
		"Keyboard Mapping", 0.8f, 0.55f, 1,
		static_cast<int>(std::size(s_arespkKeyboardMappingFull)),
		s_arespkKeyboardMappingFull
	};
	static PRD controllerMapping =
	{
		"Controller Mapping", 0.8f, 0.55f, 1,
		static_cast<int>(std::size(s_arespkKeyboardMapping)),
		s_arespkKeyboardMapping
	};

	static RESPK languageResponses[6]{};
	static PRD languageMenu =
	{
		"Language...", 1.0f, 1.0f, 1, 0, languageResponses
	};

	if (prk == PRK_KeyboardMapping)
		return keyboardMapping;
	if (prk == PRK_ControllerMapping)
		return controllerMapping;
	if (prk == PRK_LanguageMenu)
	{
		// The PAL title screen's language select lists only the languages
		// this installation holds (upstream ProjectCane). NTSC-J offers the
		// voice choice instead: Japanese / English.
		if (FJapaneseContent())
		{
			// Reuse the Italian response token as the Japanese voice choice.
			languageResponses[0] = RESPK_LanguageItalian;
			languageResponses[1] = RESPK_LanguageEnglish;
			languageResponses[2] = RESPK_Back;
			languageMenu.crespk = 3;
			return languageMenu;
		}
		static constexpr RESPK responses[] =
		{
			RESPK_LanguageEnglish, RESPK_LanguageFrench, RESPK_LanguageGerman,
			RESPK_LanguageSpanish, RESPK_LanguageItalian
		};
		languageMenu.crespk = 0;
		for (int index = 0; index < 5; ++index)
		{
			if (FContentLanguageAvailable(static_cast<CONTENT_LANGUAGE>(index)))
				languageResponses[languageMenu.crespk++] = responses[index];
		}
		languageResponses[languageMenu.crespk++] = RESPK_Back;
		return languageMenu;
	}
	return s_mpprkprd[prk];
}

RESPK* ArespkPrompt(PRK prk, const PRD& prd)
{
	if (prk == PRK_KeyboardMapping)
		return s_arespkKeyboardMappingFull;
	return prk == PRK_ControllerMapping ? s_arespkKeyboardMapping : prd.arespk;
}

int CrespkPrompt(PRK prk, const PRD& prd)
{
	if (prk == PRK_KeyboardMapping)
		return static_cast<int>(std::size(s_arespkKeyboardMappingFull));
	return FMappingPrompt(prk)
		? static_cast<int>(std::size(s_arespkKeyboardMapping))
		: prd.crespk;
}

const char* PchzPromptTitle(PRK prk, const PRD& prd)
{
	if (!FLocalizedContent())
	{
		// (USA: a text mod's prompt titles, textmod.h; the same string ids as the PAL table)
		const char* modded = prk >= PRK_PauseMenu && prk <= PRK_MtsFailedBestTime
			? ContentLocalizedText(83 + static_cast<int>(prk))
			: prk == PRK_LanguageMenu ? ContentLocalizedText(116) : nullptr;
		return modded != nullptr ? modded : prd.pchz;
	}
	if (prk >= PRK_PauseMenu && prk <= PRK_MtsFailedBestTime)
	{
		if (const char* localized = ContentLocalizedText(83 + static_cast<int>(prk)))
			return localized;
	}
	if (prk == PRK_LanguageMenu)
	{
		if (const char* localized = ContentLocalizedText(116))
			return localized;
	}
	// The game's own "Controls" (its table's 114, the title after PRK_MtsFailedBestTime's), the port's titles
	// from porttext.h.
	if (prk == PRK_ControlsMenu)
	{
		if (const char* localized = ContentLocalizedText(114))
			return localized;
	}
	switch (prk)
	{
	case PRK_VideoMenu: return PchzPort(PTX_Video);
	case PRK_SoundMenu: return PchzPort(PTX_Sound);
	case PRK_KeyboardMapping: return PchzPort(PTX_KeyboardMapping);
	case PRK_ControllerMapping: return PchzPort(PTX_ControllerMapping);
	case PRK_MouseMenu: return PchzPort(PTX_Mouse);
	case PRK_ExitToDesktopConfirm: return PchzPort(PTX_AreYouSure);
	default: return prd.pchz;
	}
}


int PalResponseLocalizationIndex(RESPK respk, int variant)
{
	if (respk >= RESPK_Yes && respk <= RESPK_SaveGame)
		return 117 + static_cast<int>(respk);
	if (respk == RESPK_Quit)
		return 136 + variant;
	if (respk == RESPK_Vibration)
		return 138 + variant;
	if (respk == RESPK_CenterScreen)
		return 126;
	if (respk == RESPK_Music)
		return 140 + variant;
	if (respk == RESPK_Speakers)
		return 142 + variant;
	if (respk >= RESPK_Back && respk <= RESPK_Controls)
		return 127 + (static_cast<int>(respk) - static_cast<int>(RESPK_Back));
	if (respk >= RESPK_BinocInvert && respk <= RESPK_JetpackInvert)
		return 144 + (static_cast<int>(respk) - static_cast<int>(RESPK_BinocInvert)) * 2 + variant;
	// PAL stores these in locale-table order (English, French, Italian,
	// German, Spanish), while the retail language prompt displays/selects
	// English, French, German, Spanish, Italian.
	switch (respk)
	{
	case RESPK_LanguageEnglish: return 150;
	case RESPK_LanguageFrench: return 151;
	case RESPK_LanguageGerman: return 153;
	case RESPK_LanguageSpanish: return 154;
	case RESPK_LanguageItalian: return 152;
	default: break;
	}
	return -1;
}


const char* PalResponseText(RESPK respk, int variant)
{
	if (FJapaneseContent())
	{
		if (respk == RESPK_LanguageItalian)
			return ContentLocalizedText(155); // 日本語
		if (respk == RESPK_LanguageEnglish)
			return ContentLocalizedText(150);
	}
	if (respk == RESPK_CameraInvert)
		return PchzPort(variant != 0 ? PTX_CameraTurnRight : PTX_CameraTurnLeft);
	const int index = PalResponseLocalizationIndex(respk, variant);
	return index >= 0 ? ContentLocalizedText(static_cast<std::size_t>(index)) : nullptr;
}


// -----------------------------------------------------------------------------
// PC input and prompt options (settings v10..v12; from ThiefRush-Dev's
// prompt.cpp). The rows below adjust with left/right as well as with confirm
// (which steps forward and wraps).
// -----------------------------------------------------------------------------
constexpr int kCameraKeyCapture = BTN_MAX; // capture index of RESPK_CamKeyLeft

int StepRange(int value, int minimum, int maximum, int step, int dir, bool wrap)
{
	int next = value + (dir < 0 ? -step : step);
	if (next > maximum) next = wrap ? minimum : maximum;
	if (next < minimum) next = wrap ? maximum : minimum;
	return next;
}

bool FPcOption(RESPK respk)
{
	return respk >= RESPK_PromptStyle && respk < RESPK_CamKeyLeft && respk != RESPK_Mouse;
}

// Changes one option by dir (+1/-1) and saves. False for other rows.
bool AdjustPcOption(RESPK respk, int dir, bool wrap)
{
	const auto toggle = [](bool& value) { value = !value; };
	switch (respk)
	{
	case RESPK_PromptStyle:
		g_promptStyle = StepRange(g_promptStyle, 0, cane::input::kPromptStyleLast, 1, dir, wrap);
		break;
	case RESPK_AlwaysOriginalPsLayout: toggle(g_fAlwaysOriginalPsLayout); break;
	case RESPK_InstantSkipCutscenes: toggle(g_fInstantSkipCutscenes); break;
	case RESPK_MouseCamera: toggle(g_mouseopt.fCamera); break;
	case RESPK_MouseSensitivity: g_mouseopt.sensitivity = StepRange(g_mouseopt.sensitivity, 10, 500, 10, dir, wrap); break;
	case RESPK_MouseAimSensitivity: g_mouseopt.aimSensitivity = StepRange(g_mouseopt.aimSensitivity, 10, 500, 10, dir, wrap); break;
	case RESPK_MouseInvertX: toggle(g_mouseopt.fInvertX); break;
	case RESPK_MouseInvertY: toggle(g_mouseopt.fInvertY); break;
	case RESPK_MouseRawInput: toggle(g_mouseopt.fRawInput); ApplyMouseOptions(); break;
	case RESPK_MouseWheelZoom: toggle(g_mouseopt.fWheelZoom); break;
	case RESPK_MouseMenus: toggle(g_mouseopt.fMenus); break;
	default: return false;
	}
	SaveSystemSettings();
	return true;
}

const char* PchzOnOff(bool value) { return PchzPort(value ? PTX_On : PTX_Off); }

// Label of a PC option row, written into its g_aachzRespkPc buffer.
void BuildPcLabel(RESPK respk)
{
	char* out = g_aachzRespkPc[respk - RESPK_PromptStyle];
	const size_t size = sizeof(g_aachzRespkPc[0]);
	switch (respk)
	{
	case RESPK_PromptStyle:
	{
		// (the controllers keep their names in every language)
		static constexpr const char* s_apchzStyles[] =
			{ nullptr, "PlayStation", "Xbox", "Nintendo", "Steam Deck", "Steam Controller", "PlayStation 4", "PlayStation 5", "Steam Controller (2026)" };
		const int style = std::clamp(g_promptStyle, 0, static_cast<int>(std::size(s_apchzStyles)) - 1);
		std::snprintf(out, size, "%s", StrPortValue(PTX_ButtonIcons,
			style == 0 ? PchzPort(PTX_Automatic) : s_apchzStyles[style]).c_str());
		break;
	}
	case RESPK_AlwaysOriginalPsLayout:
		std::snprintf(out, size, "%s", StrPortOnOff(PTX_OriginalPsIcons, g_fAlwaysOriginalPsLayout).c_str());
		break;
	case RESPK_InstantSkipCutscenes:
		std::snprintf(out, size, "%s", StrPortOnOff(PTX_InstantSkip, g_fInstantSkipCutscenes).c_str());
		break;
	case RESPK_MouseCamera: std::snprintf(out, size, "%s", StrPortOnOff(PTX_MouseCamera, g_mouseopt.fCamera).c_str()); break;
	case RESPK_MouseSensitivity:
		std::snprintf(out, size, "%s", StrPortValue(PTX_CameraSensitivity, std::to_string(g_mouseopt.sensitivity) + "%").c_str());
		break;
	case RESPK_MouseAimSensitivity:
		std::snprintf(out, size, "%s", StrPortValue(PTX_AimSensitivity, std::to_string(g_mouseopt.aimSensitivity) + "%").c_str());
		break;
	case RESPK_MouseInvertX: std::snprintf(out, size, "%s", StrPortOnOff(PTX_InvertHorizontal, g_mouseopt.fInvertX).c_str()); break;
	case RESPK_MouseInvertY: std::snprintf(out, size, "%s", StrPortOnOff(PTX_InvertAimVertical, g_mouseopt.fInvertY).c_str()); break;
	case RESPK_MouseRawInput: std::snprintf(out, size, "%s", StrPortOnOff(PTX_RawInput, g_mouseopt.fRawInput).c_str()); break;
	case RESPK_MouseWheelZoom: std::snprintf(out, size, "%s", StrPortOnOff(PTX_WheelZoomsBinocucom, g_mouseopt.fWheelZoom).c_str()); break;
	case RESPK_MouseMenus: std::snprintf(out, size, "%s", StrPortOnOff(PTX_MouseInMenus, g_mouseopt.fMenus).c_str()); break;
	case RESPK_CamKeyLeft:
	case RESPK_CamKeyRight:
	case RESPK_CamKeyUp:
	case RESPK_CamKeyDown:
	{
		static constexpr PTX s_aptx[] = { PTX_CameraLeft, PTX_CameraRight, PTX_CameraUp, PTX_CameraDown };
		const int index = respk - RESPK_CamKeyLeft;
		const int key = g_cameraKeyBindings[static_cast<size_t>(index)];
		const char* value = s_iKeyboardBindingCapture == kCameraKeyCapture + index ? PchzPort(PTX_PressAKey)
			: key == GLFW_KEY_UNKNOWN ? PchzPort(PTX_KeyUnbound) : PchzKeyboardKeyName(key);
		std::snprintf(out, size, "%s", StrPortValue(s_aptx[index], value).c_str());
		break;
	}
	default: break;
	}
}

// One-line explanation of a PC option row (drawn under the menu).
const char* PchzPcHelp(RESPK respk)
{
	switch (respk)
	{
	case RESPK_PromptStyle: return PchzPort(PTX_HelpPromptStyle);
	case RESPK_AlwaysOriginalPsLayout: return PchzPort(PTX_HelpOriginalPsIcons);
	case RESPK_InstantSkipCutscenes: return PchzPort(PTX_HelpInstantSkip);
	case RESPK_Mouse: return PchzPort(PTX_HelpMouse);
	case RESPK_MouseCamera: return PchzPort(PTX_HelpMouseCamera);
	case RESPK_MouseSensitivity: return PchzPort(PTX_HelpMouseSensitivity);
	case RESPK_MouseAimSensitivity: return PchzPort(PTX_HelpAimSensitivity);
	case RESPK_MouseInvertX: return PchzPort(PTX_HelpInvertHorizontal);
	case RESPK_MouseInvertY: return PchzPort(PTX_HelpInvertAimVertical);
	case RESPK_MouseRawInput: return PchzPort(PTX_HelpRawInput);
	case RESPK_MouseWheelZoom: return PchzPort(PTX_HelpWheelZoom);
	case RESPK_MouseMenus: return PchzPort(PTX_HelpMouseMenus);
	case RESPK_CamKeyLeft: case RESPK_CamKeyRight: case RESPK_CamKeyUp: case RESPK_CamKeyDown:
		return PchzPort(PTX_HelpCameraKeys);
	default: return nullptr;
	}
}

// Mouse menus: the rectangles DrawPrompt last drew each response in.
struct PROMPTHIT { float x0, y0, x1, y1; int irespk; };
PROMPTHIT s_ahit[32]{};
int s_chit = 0;
PRK s_prkHit = PRK_Nil;
float s_tHit = -10.0f;

int IrespkFromMouse(PRK prk)
{
	float mx = 0.0f, my = 0.0f;
	if (prk != s_prkHit || g_clock.tReal - s_tHit > 0.25f || !MouseMenuCursor(&mx, &my))
		return -1;
	for (int i = 0; i < s_chit; ++i)
		if (mx >= s_ahit[i].x0 && mx < s_ahit[i].x1 && my >= s_ahit[i].y0 && my < s_ahit[i].y1)
			return s_ahit[i].irespk;
	return -1;
}

GLFWmonitor* ActiveMonitor()
{
    if (GLFWmonitor* monitor = glfwGetWindowMonitor(g_gl.window))
        return monitor;

    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(g_gl.window, &windowX, &windowY);
    glfwGetWindowSize(g_gl.window, &windowWidth, &windowHeight);

    const int centerX = windowX + windowWidth / 2;
    const int centerY = windowY + windowHeight / 2;
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);

    for (int i = 0; i < monitorCount; ++i)
    {
        int monitorX = 0;
        int monitorY = 0;
        glfwGetMonitorPos(monitors[i], &monitorX, &monitorY);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);

        if (mode != nullptr && centerX >= monitorX && centerX < monitorX + mode->width &&
            centerY >= monitorY && centerY < monitorY + mode->height)
        {
            return monitors[i];
        }
    }

    return glfwGetPrimaryMonitor();
}

std::vector<int> SupportedFrameRates()
{
    std::vector<int> frameRates{30, 60, 90, 120, 240};
    GLFWmonitor* monitor = ActiveMonitor();

    if (monitor != nullptr)
    {
        int modeCount = 0;
        const GLFWvidmode* modes = glfwGetVideoModes(monitor, &modeCount);

        int maximum = 0;
        for (int i = 0; i < modeCount; ++i)
            maximum = std::max(maximum, modes[i].refreshRate);
        if (maximum >= 30) frameRates.push_back(maximum);
    }

    std::sort(frameRates.begin(), frameRates.end());
    frameRates.erase(std::unique(frameRates.begin(), frameRates.end()), frameRates.end());

    return frameRates;
}

void SelectNextSupportedFrameRate()
{
    const std::vector<int> frameRates = SupportedFrameRates();
    auto next = std::upper_bound(frameRates.begin(), frameRates.end(), g_targetFrameRate);

    if (next == frameRates.end())
        next = frameRates.begin();

    g_targetFrameRate = *next;
}

void SelectMsaaSetting(int direction)
{
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maximum);
    std::vector<int> options{0};
    for (int samples : {2, 4, 8, 16})
        if (samples <= maximum) options.push_back(samples);

    const int current = g_fMsaa ? g_msaaSamples : 0;
    const auto found = std::find(options.begin(), options.end(), current);
    const int index = found == options.end() ? 0 : int(found - options.begin());
    const int next = (index + (direction < 0 ? -1 : 1) + int(options.size())) % int(options.size());
    g_fMsaa = options[next] != 0;
    if (g_fMsaa) g_msaaSamples = options[next];
    ApplyMsaaSettings();
}

void SelectNextInternalResolution()
{
    constexpr int resolutions[] = { 0, 360, 480, 720, 1080, 1440, 2160 };
    auto current = std::find(std::begin(resolutions), std::end(resolutions),
        g_internalResolutionHeight);

    if (current == std::end(resolutions) || ++current == std::end(resolutions))
        current = std::begin(resolutions);

    g_internalResolutionHeight = *current;
    ApplyInternalResolutionSettings();
}

void SelectNextGuiScale()
{
    constexpr float ps2Scales[] = { 0.75f, 1.0f };
    constexpr float modernScales[] = { 1.0f, 1.25f };

    if (g_guiStyle == GuiStyle_Modern)
    {
        auto next = std::upper_bound(std::begin(modernScales), std::end(modernScales),
            g_guiScale + 0.001f);
        g_guiScale = next == std::end(modernScales) ? modernScales[0] : *next;
    }
    else
    {
        auto next = std::upper_bound(std::begin(ps2Scales), std::end(ps2Scales),
            g_guiScale + 0.001f);
        g_guiScale = next == std::end(ps2Scales) ? ps2Scales[0] : *next;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(g_gl.window, &width, &height);
    FrameBufferSizeCallBack(g_gl.window, width, height);
}

void SelectNextGuiStyle()
{
    g_guiStyle = g_guiStyle == GuiStyle_PS2 ? GuiStyle_Modern : GuiStyle_PS2;

    // 100% is the common scale between both styles. Normalize a value that
    // belongs only to the previous style when changing presentation modes.
    if ((g_guiStyle == GuiStyle_Modern && g_guiScale < 1.0f) ||
        (g_guiStyle == GuiStyle_PS2 && g_guiScale > 1.0f))
    {
        g_guiScale = 1.0f;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(g_gl.window, &width, &height);
    FrameBufferSizeCallBack(g_gl.window, width, height);
}

void SelectNextDrawDistance()
{
    if (g_pcm == nullptr)
        return;

    constexpr float multipliers[] = { 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };
    auto next = std::upper_bound(std::begin(multipliers), std::end(multipliers),
        g_pcm->rMRD + 0.001f);

    if (next == std::end(multipliers))
        next = std::begin(multipliers);

    SetCmMrdRatio(g_pcm, *next);
    g_drawDistanceMultiplier = *next;
}

void SelectNextWindowMode()
{
    const int next = (static_cast<int>(g_windowMode) + 1) % 3;
    ApplyWindowModeSettings(static_cast<WindowMode>(next));
}
}

bool g_fSwapPacesFrames = false;
unsigned g_swapGeneration = 0;

void ApplySwapInterval()
{
    int interval = 0;
    int refreshRate = 0;
    bool fSwapPacesFrames = false;

    if (g_fVsync)
    {
        interval = 1;
        GLFWmonitor* monitor = g_gl.window != nullptr ? ActiveMonitor() : glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
        refreshRate = mode != nullptr ? mode->refreshRate : 0;

        // Windows reports 59.94 Hz modes as 59. The real vblank then runs more
        // than 1% faster than a 59 FPS target, so the swap could never gate.
        const bool fRefreshTruncated = refreshRate < 100 && refreshRate % 10 == 9;

        if (refreshRate > 0 && g_targetFrameRate >= refreshRate && !fRefreshTruncated)
        {
            fSwapPacesFrames = true;
        }
        // Some drivers clamp the interval to 1 (Mesa on Wayland). FramePacer
        // notices that the swap is not pacing and holds the target itself.
        else if (refreshRate > 0 && g_targetFrameRate * 2 == refreshRate)
        {
            interval = 2;
            fSwapPacesFrames = true;
        }
    }

    cane::gl::SwapInterval(interval);
    g_fSwapPacesFrames = fSwapPacesFrames;

    // FramePacer keeps its verdict on whether the swap really paces until this
    // changes. Window moves re-apply the interval without changing anything.
    static int s_lastInterval = -1;
    static int s_lastRefreshRate = -1;
    static int s_lastTargetFrameRate = -1;
    static int s_lastWindowMode = -1;
    const int windowMode = static_cast<int>(g_windowMode);
    if (interval != s_lastInterval || refreshRate != s_lastRefreshRate ||
        g_targetFrameRate != s_lastTargetFrameRate || windowMode != s_lastWindowMode)
    {
        s_lastInterval = interval;
        s_lastRefreshRate = refreshRate;
        s_lastTargetFrameRate = g_targetFrameRate;
        s_lastWindowMode = windowMode;
        ++g_swapGeneration;
    }
}

void StartupPrompt(PROMPT* pprompt)
{
    g_tePrompt.m_ch = 45;
    // Retail GS RGB is normalized around 0x80, not 0xff.
    g_tePrompt.m_rgba = { 0.0f, 75.0f / 128.0f, 125.0f / 128.0f, 1.0f };

    g_tePrompt.m_dxExtra = 15.0;
    g_tePrompt.m_ryScaling = 0.6;
    g_tePrompt.m_rxScaling = 0.6;

    pprompt->pvtprompt = &g_vtprompt;
}

void PostPromptLoad(PROMPT* pprompt)
{
    PostBlotLoad(pprompt);

    if (FFontLoaded(2))
    {
        pprompt->pte = &g_tePrompt; 
        pprompt->pte->m_pfont = PfontFromFont(2);
    }

    pprompt->prkSaveContext = PRK_Nil;
    pprompt->prk = PRK_Nil;

    for (int i = 0; i < 3; ++i)
        pprompt->mpprpprk[i] = PRK_Nil;

    pprompt->alpha = 1.0f;
}

void HandlePromptPrkTransition(PROMPT* pprompt, PRK prkOld, PRK prkNew)
{
    if (pprompt == nullptr || prkOld == prkNew)
        return;

    if (pprompt->fVagPlaying != 0)
    {
        StopVag();
        pprompt->fVagPlaying = 0;
    }

    switch (prkOld)
    {
        case PRK_MemcardChooseLoadSlot:
        g_pnote.pvtblot->pfnSetBlotBlots(&g_pnote, BLOTS_Hidden);
        g_pnote.pvtblot->pfnSetBlotAchzDraw(&g_pnote, nullptr);
        break;

        default:
        break;
    }

    switch (prkNew)
    {
        case PRK_Nil:
        g_lifectr.pvtlifectr->pfnHideBlot(&g_lifectr);
        g_cluectr.pvtblot->pfnHideBlot(&g_cluectr);
        g_keyctr.pvtkeyctr->pfnHideBlot(&g_keyctr);
        g_coinctr.pvtcoinctr->pfnHideBlot(&g_coinctr);
        g_percentctr.pvtctr->pfnHideBlot(&g_percentctr);
        break;

        case PRK_MemcardChooseLoadSlot:
        {
        // (the disc's words: its table's 157; USA: a text mod's, textmod.h)
        const char* localized = ContentLocalizedText(157);
        g_pnote.pvtblot->pfnSetBlotAchzDraw(&g_pnote, const_cast<char*>(localized != nullptr ? localized : "&2S&.: Erase Game"));
        }
        SetBlotDtVisible(&g_pnote, 0.0f);
        SetBlotFontScale(&g_pnote, 0.6f);
        g_pnote.pvtblot->pfnShowBlot(&g_pnote);
        break;

        default:
        break;
    }
}

void SetPrompt(PROMPT* pprompt, int prp, PRK prk)
{
    pprompt->mpprpprk[prp] = prk;

    // The last non-nil prompt slot has priority.
    PRK prkNew = PRK_Nil;

    for (int i = 0; i < 3; ++i)
    {
        if (pprompt->mpprpprk[i] != PRK_Nil)
            prkNew = static_cast<PRK>(pprompt->mpprpprk[i]);
    }

    const PRK prkCurrent =
        pprompt->fReshow
        ? pprompt->prkReshow
        : pprompt->prk;

    HandlePromptPrkTransition(pprompt, prkCurrent, prkNew);

    if (prkCurrent != prkNew)
    {
        if (pprompt->blots == BLOTS_Hidden)
            ChangePromptPrk(pprompt, prkNew);
        else if (pprompt->blots > BLOTS_Nil && pprompt->blots < BLOTS_Max)
        {
            if (prkNew == PRK_Nil)
            {
                pprompt->fReshow = 0;
                pprompt->prkReshow = PRK_Nil;
            }
            else
            {
                pprompt->fReshow = 1;
                pprompt->prkReshow = prkNew;

                if (pprompt->pvtblot->pfnSetBlotBlots != nullptr)
                    pprompt->pvtblot->pfnSetBlotBlots(pprompt, BLOTS_Disappearing);
            }
        }
    }

    if (prkNew == PRK_Nil)
    {
        if (pprompt->fActive != 0)
            PopUiActiveBlot(&g_ui);
    }
    else if (pprompt->fActive == 0)
        PushUiActiveBlot(&g_ui, pprompt);
}

SMA* PsmaFindSpecialPromptStateMachine()
{
    SM* psm = (SM*)PloFindSwObject(g_psw, 5, (OID)1068, nullptr);

    if (psm == nullptr)
        return nullptr;

    return psm->dlSma.psmaFirst;
}

void SetPromptPrk(PROMPT* pprompt)
{
    if (pprompt == nullptr || pprompt->pfont == nullptr)
        return;

    const PRK prk = pprompt->prk;

    if (prk < PRK_PauseMenu || prk >= PRK_Max)
        return;

    // -------------------------------------------------------------------------
    // Build the dynamic pause menu
    // -------------------------------------------------------------------------

    if (prk == PRK_PauseMenu)
    {
        PRD& prd = s_mpprkprd[PRK_PauseMenu];
        prd.arespk = s_arespkPauseMenu;
        prd.crespk = 0;

        for (RESPK response : s_arespkPauseMenuAll)
        {
            bool include = true;

            switch (response)
            {
                case RESPK_Map:
                include = g_wmc.pwmCurrent != nullptr;
                break;

                case RESPK_RestartRace:
                {
                    const int levelId = (static_cast<int>(g_pgsCur->gameWorldCur) << 8) | static_cast<int>(g_pgsCur->worldLevelCur);
                    include = false;

                    if (levelId == 516 || levelId == 1031)
                    {
                        SMA* psma = PsmaFindSpecialPromptStateMachine();

                        if (psma != nullptr)
                        {
                            int smaState[4]{};
                            GetSmaCur(psma, (OID*)smaState);
                            include = smaState[0] > 1070 && smaState[0] < 1073;
                        }
                    }

                    break;
                }

                case RESPK_RestartSprint:
                include = g_note.mtsState == MTSSTATE_Active;
                break;

                case RESPK_RestartLevel:
                {
                    const int levelId = (static_cast<int>(g_pgsCur->gameWorldCur) << 8) | static_cast<int>(g_pgsCur->worldLevelCur);
                    include = levelId == 772 || levelId == 775 || levelId == 1282;
                    break;
                }

                case RESPK_ExitLevel:
                if (g_pgsCur->gameWorldCur == GAMEWORLD_Intro)
                {
                    include = false;
                }
                else if (g_pgsCur->gameWorldCur == GAMEWORLD_Clockwerk ||
                    g_pgsCur->worldLevelCur == WORLDLEVEL_Hub ||
                    (g_pgsCur->worldLevelCur == WORLDLEVEL_Approach && (g_pwsCur->als[1].grfls & 1U) == 0))
                {
                    response = RESPK_ExitToHideout;
                }
                break;

                default:
                break;
            }

            if (include && prd.crespk < static_cast<int>(std::size(s_arespkPauseMenu)))
                prd.arespk[prd.crespk++] = response;
        }
    }

    // -------------------------------------------------------------------------
    // Build the dynamic options menu
    // -------------------------------------------------------------------------

    else if (prk == PRK_OptionsMenu)
    {
        PRD& prd = s_mpprkprd[PRK_OptionsMenu];
        prd.arespk = s_arespkOptionsMenu;
        prd.crespk = 0;

        for (RESPK response : s_arespkOptionsMenuAll)
        {
            bool include = true;

            if (response == RESPK_NewGame)
                include = g_ui.uisPlaying == UIS_Attract;
            else if (response == RESPK_SaveGame)
                include = g_ui.uisPlaying != UIS_Attract;

            if (include &&prd.crespk < static_cast<int>(std::size(s_arespkOptionsMenu)))
                prd.arespk[prd.crespk++] = response;
        }
    }

	else if (prk == PRK_ControlsMenu)
	{
		PRD& prd = s_mpprkprd[PRK_ControlsMenu];
		prd.arespk = s_arespkControlsMenu;
		prd.crespk = 0;
		const bool gamepadAvailable = FGamepadAvailable();

		for (RESPK response : s_arespkControlsMenuAll)
		{
			if (response == RESPK_ControllerMapping && !gamepadAvailable)
				continue;
			prd.arespk[prd.crespk++] = response;
		}
	}
    // -------------------------------------------------------------------------
    // Rebuild dynamic response strings
    // -------------------------------------------------------------------------

    std::snprintf(g_achzRespk9, sizeof(g_achzRespk9), "%s", s_apchzRespk9[g_saveData.pgsCurrentSave != nullptr ? 1 : 0]);

    char* const slotStrings[SAVE_SLOT_COUNT] =
    {
        g_achzRespk10,
        g_achzRespk11,
        g_achzRespk12,
        g_achzSaveSlot4,
        g_achzSaveSlot5,
        g_achzSaveSlot6
    };

    for (int slot = 0; slot < SAVE_SLOT_COUNT; ++slot)
    {
        GS* pgs = &g_saveData.saveData[slot];

        if (pgs->dt != 0.0f)
            BuildSaveSlotText(pgs, slotStrings[slot]);
        else
        {
            const bool choosingSlot = prk == PRK_MemcardChooseNewSlot || prk == PRK_MemcardChooseLoadSlot;
            // (the disc's words: its table's 134 / 135; USA: a text mod's, textmod.h)
            const char* localized = ContentLocalizedText(choosingSlot ? 134 : 135);
            std::snprintf(slotStrings[slot], 64, "%s", localized != nullptr ? localized : choosingSlot ? "New Game" : "Empty");
        }
    }

    std::snprintf(g_achzRespk13, sizeof(g_achzRespk13), "%s", s_apchzRespk13[FVibrationEnabled(g_pgsCur) ? 0 : 1]);
    std::snprintf(g_achzRespk16, sizeof(g_achzRespk16), "%s",
		s_apchzRespk16[FUserStereoEnabled() ? 0 : 1]);
    std::snprintf(g_achzRespk24, sizeof(g_achzRespk24), "%s", s_apchzRespk24[(g_pgsCur->grfgs & 0x200U) != 0 ? 1 : 0]);
    std::snprintf(g_achzRespk25, sizeof(g_achzRespk25), "%s", s_apchzRespk25[(g_pgsCur->grfgs & 0x400U) != 0 ? 1 : 0]);
    std::snprintf(g_achzRespk26, sizeof(g_achzRespk26), "%s", s_apchzRespk26[(g_pgsCur->grfgs & 0x800U) != 0 ? 1 : 0]);
	std::snprintf(g_achzRespk41, sizeof(g_achzRespk41), "%s",
		PchzPort((g_pgsCur->grfgs & 0x1000U) != 0 ? PTX_CameraTurnRight : PTX_CameraTurnLeft));
	BuildBindingStrings(prk);
	for (int respk = RESPK_PromptStyle; respk < RESPK_Max; ++respk)
		if (respk != RESPK_Mouse)
			BuildPcLabel(static_cast<RESPK>(respk));
    std::snprintf(g_achzRespk27, sizeof(g_achzRespk27), "%s", StrPortValue(PTX_Msaa,
        g_fMsaa ? std::to_string(g_msaaSamples) + "x" : std::string(PchzPort(PTX_Off))).c_str());
    std::snprintf(g_achzRespk28, sizeof(g_achzRespk28), "%s", StrPortValue(PTX_FrameRate,
        StrPortFormat(PTX_Fps, g_targetFrameRate) + (g_targetFrameRate == 30 ? PchzPort(PTX_Performance) : "")).c_str());
    if (g_internalResolutionHeight == 0)
    {
        std::snprintf(g_achzRespk30, sizeof(g_achzRespk30), "%s", StrPortValue(PTX_Resolution,
            StrPortFormat(PTX_NativeResolution, g_gl.renderWidth, g_gl.renderHeight)).c_str());
    }
    else
    {
        std::snprintf(g_achzRespk30, sizeof(g_achzRespk30), "%s", StrPortValue(PTX_Resolution,
            std::to_string(g_gl.renderWidth) + " x " + std::to_string(g_gl.renderHeight)).c_str());
    }

    std::snprintf(g_achzRespk31, sizeof(g_achzRespk31), "%s",
        StrPortValue(PTX_Fog, PchzPort(g_fogType == 2 ? PTX_Ps3Style : PTX_Ps2Style)).c_str());

    const float drawDistance = g_drawDistanceMultiplier;
    PTX ptxDrawDistance = PTX_Max;
    if (drawDistance > 0.749f && drawDistance < 0.751f)
        ptxDrawDistance = PTX_Low;
    else if (drawDistance > 0.999f && drawDistance < 1.001f)
        ptxDrawDistance = PTX_Original;
    else if (drawDistance > 1.499f && drawDistance < 1.501f)
        ptxDrawDistance = PTX_High;
    else if (drawDistance > 1.999f && drawDistance < 2.001f)
        ptxDrawDistance = PTX_VeryHigh;
    else if (drawDistance > 2.999f && drawDistance < 3.001f)
        ptxDrawDistance = PTX_Extreme;

    std::snprintf(g_achzRespk32, sizeof(g_achzRespk32), "%s", StrPortValue(PTX_DrawDistance,
        ptxDrawDistance != PTX_Max ? std::string(PchzPort(ptxDrawDistance))
            : StrPortFormat(PTX_Custom, static_cast<double>(drawDistance))).c_str());

    constexpr PTX windowModeNames[] = { PTX_Windowed, PTX_Borderless, PTX_Fullscreen };
    const int windowModeIndex = (std::clamp)(static_cast<int>(g_windowMode), 0, 2);
    std::snprintf(g_achzRespk33, sizeof(g_achzRespk33), "%s",
        StrPortValue(PTX_WindowMode, PchzPort(windowModeNames[windowModeIndex])).c_str());
    std::snprintf(g_achzRespk34, sizeof(g_achzRespk34), "%s", StrPortOnOff(PTX_VSync, g_fVsync).c_str());
    const char* aspectRatioName = PchzPort(PTX_FitToScreen);
    if (g_gl.aspectMode == Fixed_16_9)
        aspectRatioName = "16:9";
    else if (g_gl.aspectMode == Fixed_16_10)
        aspectRatioName = "16:10";
    else if (g_gl.aspectMode == Fixed_4_3)
        aspectRatioName = "4:3";
    else if (g_gl.aspectMode == PS2_4_3)
        aspectRatioName = "4:3 (PS2)";
    else if (g_gl.aspectMode == PS2_16_9)
        aspectRatioName = "16:9 (PS2)";
    std::snprintf(g_achzRespk35, sizeof(g_achzRespk35), "%s", StrPortValue(PTX_AspectRatio, aspectRatioName).c_str());
    std::snprintf(g_achzRespk40, sizeof(g_achzRespk40), "%s", StrPortValue(PTX_GuiScale,
        std::to_string(static_cast<int>(std::lround(g_guiScale * 100.0f))) + "%").c_str());
    std::snprintf(g_achzRespk60, sizeof(g_achzRespk60), "%s",
        StrPortValue(PTX_GuiStyle, g_guiStyle == GuiStyle_Modern ? PchzPort(PTX_Modern) : "PS2").c_str());
    std::snprintf(g_achzRespk61, sizeof(g_achzRespk61), "%s",
        StrPortOnOff(PTX_DisableControllerUnfocused, g_fDisableControllerInputWhenUnfocused).c_str());

    // -------------------------------------------------------------------------
    // Choose initial response
    // -------------------------------------------------------------------------

    pprompt->irespk = 0;

    switch (prk)
    {
        case PRK_QuitConfirm:
		case PRK_ExitToDesktopConfirm:
        case PRK_MemcardFormatConfirm:
        case PRK_MemcardCreateConfirm:
        case PRK_MemcardOverwriteConfirm:
        case PRK_MemcardEraseConfirm:
        pprompt->irespk = 1;
        break;

        case PRK_MemcardChooseSaveSlot:
        case PRK_MemcardChooseLoadSlot:
        for (int slot = 0; slot < SAVE_SLOT_COUNT; ++slot)
        {
            if (g_saveData.pgsCurrentSave == &g_saveData.saveData[slot])
            {
                pprompt->irespk = slot;
                break;
            }
        }
        break;

        case PRK_MemcardChooseNewSlot:
        while (pprompt->irespk < SAVE_SLOT_COUNT &&
            g_saveData.saveData[pprompt->irespk].dt != 0.0f)
            ++pprompt->irespk;
        break;

        case PRK_MemcardAutoNotify:
        if (!FVagPlaying())
        {
            // Extracted equivalent of the original VAG descriptor at 0x0026A118.
            PreloadVag("PromptPrk18.wav");
            ContinueVag();
            pprompt->fVagPlaying = FVagPlaying();
        }
        break;

		case PRK_LanguageMenu:
		if (FJapaneseContent())
			pprompt->irespk = ContentVoiceLanguage() == CONTENT_LANGUAGE::English ? 1 : 0;
		else
		{
			pprompt->irespk = 0;
			for (int index = 0; index < static_cast<int>(ContentPackage().language); ++index)
			{
				if (FContentLanguageAvailable(static_cast<CONTENT_LANGUAGE>(index)))
					++pprompt->irespk;
			}
		}
		break;

        default:
        break;
    }

    // -------------------------------------------------------------------------
    // Measure prompt
    // -------------------------------------------------------------------------

    PRD& prd = PrdPrompt(prk);
	RESPK* const promptResponses = ArespkPrompt(prk, prd);
	const int promptResponseCount = CrespkPrompt(prk, prd);

    float titleWidth = 0.0f;
    float titleHeight = 0.0f;
    float responseWidth = 0.0f;

    const char* const promptTitle = PchzPromptTitle(prk, prd);
    if (promptTitle != nullptr)
    {
        pprompt->pfont->PushScaling(prd.rScaleTitle, prd.rScaleTitle);
        pprompt->pfont->GetExtents(const_cast<char*>(promptTitle), &titleWidth, &titleHeight, 500.0f);
        pprompt->pfont->PopScaling();

        titleWidth = std::max(titleWidth, 0.0f);
    }

    pprompt->pfont->PushScaling(prd.rScaleRespk, prd.rScaleRespk);

    float lineHeight = static_cast<float>(pprompt->pfont->m_dyUnscaled) * pprompt->pfont->m_ryScale;
    const bool isBindingMenu = FMappingPrompt(prk);
    int visibleResponseCount = isBindingMenu ? std::min(promptResponseCount, 9) : promptResponseCount;

    for (int i = 0; i < promptResponseCount; ++i)
    {
        const RESPK response = promptResponses[i];

        if (response == RESPK_Nil)
            break;

        if (response < RESPK_Yes || response >= RESPK_Max)
            continue;

        const RESPD& responseData = s_arespd[response];
        float itemWidth = 0.0f;

        if (responseData.cpchz <= 0 || responseData.apchz == nullptr)
        {
            CRichText text(const_cast<char*>(AchzFromRespk(response)), pprompt->pfont);
            itemWidth = text.Dx();
        }
        else
        {
            for (int variant = 0; variant < responseData.cpchz; ++variant)
            {
                const char* variantText = PalResponseText(response, variant);
				if (variantText == nullptr)
					variantText = responseData.apchz[variant];
                if (variantText == nullptr)
                    continue;

                CRichText text(const_cast<char*>(variantText), pprompt->pfont);
                itemWidth = std::max(itemWidth, text.Dx());
            }
        }

        if (prd.fVertical != 0)
            responseWidth = std::max(responseWidth, itemWidth);
        else
        {
            if (i != 0)
            {
                const float spaceWidth = static_cast<float>(pprompt->pfont->m_dxSpaceUnscaled) * pprompt->pfont->m_rxScale;
                responseWidth += spaceWidth * 2.0f;
            }

            responseWidth += itemWidth;
        }
    }

    pprompt->pfont->PopScaling();

    // Japanese retail DrawPrompt advances eight authored screen units before
    // its response rows. This is a layout gap, not a global prompt scale.
    const float responseGap = FJapaneseContent() && promptResponseCount > 0 ? 8.0f : 0.0f;

    // Fit the port's option pages to the screen (authored units: the window
    // divided by the GUI scale), with a margin, and room under the box for
    // the help line when a row has one.
    s_promptFit = {};
    if (FPortOptionPage(prk) && prd.fVertical != 0 && promptResponseCount > 0 && g_gl.height > 0)
    {
        const GuiScale scale = GetGuiScale();
        const float screenWidth = static_cast<float>(g_gl.width) / scale.x;
        const float screenHeight = static_cast<float>(g_gl.height) / scale.y;

        bool hasHelp = false;
        for (int i = 0; i < promptResponseCount; ++i)
            hasHelp = hasHelp || PchzPcHelp(promptResponses[i]) != nullptr;
        float helpHeight = 0.0f;
        if (hasHelp)
        {
            pprompt->pfont->PushScaling(0.5f, 0.5f);
            helpHeight = 8.0f + 2.0f * static_cast<float>(pprompt->pfont->m_dyUnscaled) * pprompt->pfont->m_ryScale;
            pprompt->pfont->PopScaling();
        }

        float rFit = 1.0f;
        const float widthBudget = screenWidth * 0.9f - 10.0f;
        if (responseWidth > widthBudget && widthBudget > 0.0f)
            rFit = std::max(0.6f, widthBudget / responseWidth);

        float heightBudget = screenHeight * 0.9f - 2.0f * helpHeight - titleHeight - responseGap;
        const float fittedLine = lineHeight * rFit;
        int rowsFit = fittedLine > 0.0f ? static_cast<int>(heightBudget / fittedLine) : promptResponseCount;
        if (rowsFit < promptResponseCount)
        {
            // Scrolling: strips above and below the rows hold the arrows.
            heightBudget -= 2.0f * kPromptScrollMark;
            rowsFit = fittedLine > 0.0f ? static_cast<int>(heightBudget / fittedLine) : promptResponseCount;
        }

        s_promptFit.prk = prk;
        s_promptFit.rScaleRespk = rFit;
        s_promptFit.crespkVisible = std::clamp(rowsFit, std::min(promptResponseCount, 3), promptResponseCount);
        s_promptFit.width = g_gl.width;
        s_promptFit.height = g_gl.height;
        s_promptFit.xScale = scale.x;
        s_promptFit.yScale = scale.y;

        responseWidth *= rFit;
        lineHeight = fittedLine;
        visibleResponseCount = s_promptFit.crespkVisible;
    }

    float responseHeight = prd.fVertical != 0 ? lineHeight * static_cast<float>(visibleResponseCount) : lineHeight;
    if (promptResponseCount == 0)
        responseHeight = prd.fVertical != 0 ? 0.0f : lineHeight;
    if (s_promptFit.prk == prk && visibleResponseCount < promptResponseCount)
        responseHeight += 2.0f * kPromptScrollMark;
    ResizeBlot(pprompt, std::max(titleWidth, responseWidth) + 10.0f,
        titleHeight + responseGap + responseHeight);

}

void ChangePromptPrk(PROMPT* pprompt, PRK prk)
{
    if ((pprompt->prk != prk) && (pprompt->prk = prk, prk != PRK_Nil))
        SetPromptPrk(pprompt);
}

void OpenMappingPrompt(PROMPT* pprompt, PRK prk)
{
    if (pprompt == nullptr)
        return;

    const PRK oldPrk = pprompt->prk;
    pprompt->mpprpprk[PRP_Basic] = prk;
    pprompt->fReshow = 0;
    pprompt->prkReshow = PRK_Nil;
    HandlePromptPrkTransition(pprompt, oldPrk, prk);
    ChangePromptPrk(pprompt, prk);

    // Mapping pages are PC-only menus and do not need retail's full
    // hide/reshow transition. Opening them directly also prevents a held
    // confirm input from leaving the prompt at its collapsed animation size.
    SetBlotBlots(pprompt, BLOTS_Visible);
}

void ExecutePrompt(PROMPT* pprompt)
{
    if (pprompt == nullptr || pprompt->prk < PRK_PauseMenu || pprompt->prk >= PRK_Max)
        return;

    const PRD& promptData = PrdPrompt(pprompt->prk);
	RESPK* const promptResponses = ArespkPrompt(pprompt->prk, promptData);
	const int promptResponseCount = CrespkPrompt(pprompt->prk, promptData);

    RESPK response = RESPK_Nil;

    if (promptResponses != nullptr && pprompt->irespk >= 0 && pprompt->irespk < promptResponseCount)
        response = promptResponses[pprompt->irespk];

    if (response == RESPK_Back)
    {
        CancelPrompt(pprompt);
        return;
    }

    const auto openOptionsMenu = [&]()
    {
        SetPrompt(pprompt, PRP_Basic, PRK_OptionsMenu);
    };

    const auto playInvalidSelectionSound = [&]()
    {
        StartSound(static_cast<SFXID>(123), nullptr, nullptr, nullptr, 3000.0f, 300.0f, 1.0f, 0.0f, 0.0f, nullptr, nullptr);
    };

    const auto writeSelectedSave = [&]()
    {
        const bool fQuitToTitle = pprompt->prkSaveContext == PRK_QuitConfirm;

        if (!SaveCurrentGameToDisk(&g_saveData))
        {
            SetPrompt(pprompt, PRP_Memcard, PRK_Unknown15);
            return;
        }

        // Retail clears MemcardSlotSaving as soon as the memory-card
        // operation completes successfully.  The PC save is synchronous, so
        // complete the same transition here instead of introducing a second
        // SlotSaved/Continue prompt.
        SetPrompt(pprompt, PRP_Memcard, PRK_Nil);

        if (fQuitToTitle)
        {
            SetPrompt(pprompt, PRP_Basic, PRK_Nil);
            WipeToTitleScreen();
        }
    };

    const auto getSaveSlotIndex = [](RESPK slotResponse) -> int
    {
        for (int slot = 0; slot < SAVE_SLOT_COUNT; ++slot)
        {
            if (s_arespkSlots[slot] == slotResponse)
                return slot;
        }

        return -1;
    };

    const auto closeSaveManagerPrompt = [&]()
    {
        SetPrompt(pprompt, PRP_Memcard, PRK_Nil);
        SetSaveManagerState(&g_saveData, SAVE_MENU_STATE_Closing);

        const PRK basicPrompt = pprompt->mpprpprk[PRP_Basic];

        if (basicPrompt == PRK_MemcardChooseSaveSlot ||
            basicPrompt == PRK_MemcardChooseNewSlot ||
            basicPrompt == PRK_MemcardChooseLoadSlot)
        {
            openOptionsMenu();
        }
    };

    const auto rebuildPromptKeepingSelection = [&]()
    {
        const int selectedResponse = pprompt->irespk;
        SetPromptPrk(pprompt);
        pprompt->irespk = selectedResponse;
    };

    PRP promptGroupToClear = PRP_Basic;

    switch (pprompt->prk)
    {
        case PRK_PauseMenu:
        {
            switch (response)
            {
                case RESPK_ReturnToGame:
                break;

                case RESPK_Map:
                if (g_wmc.pwmCurrent == nullptr)
                {
                    playInvalidSelectionSound();
                    return;
                }

                SetPrompt(pprompt, PRP_Basic, PRK_Nil);
                PushUiActiveBlot(&g_ui, &g_wmc);
                return;

                case RESPK_Options:
                openOptionsMenu();
                return;

                case RESPK_Quit:
                SetPrompt(pprompt, PRP_Basic, PRK_QuitConfirm);
                return;

                case RESPK_ExitToDesktop:
				SetPrompt(pprompt, PRP_Basic, PRK_ExitToDesktopConfirm);
                return;

                case RESPK_RestartRace:
                {
                    SMA* psma = PsmaFindSpecialPromptStateMachine();
                    PO* ppo = PpoCur();

                    if (psma != nullptr && ppo != nullptr && FIsBasicDerivedFrom(ppo, CID_SUV))
                    {
                        int state[4]{};
                        GetSmaCur(psma, (OID*)state);

                        if (state[0] > 1070 && state[0] < 1073)
                        {
                            HandleSuvRaceLoss(reinterpret_cast<SUV*>(ppo));
                            SeekSma(psma, (OID)1070);
                        }
                    }

                    break;
                }

                case RESPK_RestartSprint:
                case RESPK_RestartLevel: 
                ExitOrRestartLevelFromPrompt(pprompt, true, WIPEK_Keyhole);
                return;

                case RESPK_ExitLevel:
                SetPrompt(pprompt, PRP_Basic, PRK_Nil);
                TriggerDefaultExit(true, WIPEK_WorldMap);
                return;

                case RESPK_ExitToHideout:
                {
                    LEVELINFO* plevelHideout = PlevelinfoFromSearchKey(9);

                    if (plevelHideout == nullptr)
                    {
                        std::printf("PROMPT ERROR: Hideout search key 9 not found\n");
                        playInvalidSelectionSound();
                        return;
                    }

                    SetPrompt(pprompt, PRP_Basic, PRK_Nil);
                    WipeToWorldWarp(plevelHideout, OID_Nil, WIPEK_WorldMap);
                    return;
                }

                case RESPK_Controls:
                SetPrompt(pprompt, PRP_Basic, PRK_ControlsMenu);
                return;

                default:
                return;
            }

            break;
        }

        case PRK_VideoMenu:
        switch (response)
        {
            case RESPK_Msaa:
            SelectMsaaSetting(1);
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_FrameRate:
            SelectNextSupportedFrameRate();
            ApplySwapInterval();
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_InternalResolution:
            SelectNextInternalResolution();
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_GuiScale:
            SelectNextGuiScale();
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_GuiStyle:
            SelectNextGuiStyle();
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_DrawDistance:
            SelectNextDrawDistance();
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_WindowMode:
            SelectNextWindowMode();
            ApplySwapInterval();
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_Vsync:
            g_fVsync = !g_fVsync;
            ApplySwapInterval();
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_AspectRatio:
            switch (g_gl.aspectMode)
            {
                case FitToScreen:
                ApplyAspectRatioSettings(Fixed_16_9);
                break;
                case Fixed_16_9:
                ApplyAspectRatioSettings(Fixed_16_10);
                break;
                case Fixed_16_10:
                ApplyAspectRatioSettings(Fixed_4_3);
                break;
                case Fixed_4_3:
                ApplyAspectRatioSettings(PS2_4_3);
                break;
                case PS2_4_3:
                ApplyAspectRatioSettings(PS2_16_9);
                break;
                case PS2_16_9:
                default:
                ApplyAspectRatioSettings(FitToScreen);
                break;
            }
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_Fog:
            g_fogType = g_fogType == 1 ? 2 : 1;
            glGlobShader.Use();
            glUniform1i(glslFogType, g_fogType);
            SaveSystemSettings();
            rebuildPromptKeepingSelection();
            return;

            case RESPK_Back:
            SetPrompt(pprompt, PRP_Basic, PRK_OptionsMenu);
            return;

            default:
            return;
        }

		case PRK_SoundMenu:
		switch (response)
		{
			case RESPK_Speakers:
			SetUserStereoEnabled(!FUserStereoEnabled());
			break;
			case RESPK_MusicVolume:
			SetUserMusicVolume(GetUserMusicVolume() >= 0.999f ? 0.0f : GetUserMusicVolume() + 0.1f);
			break;
			case RESPK_SfxVolume:
			SetUserSfxVolume(GetUserSfxVolume() >= 0.999f ? 0.0f : GetUserSfxVolume() + 0.1f);
			break;
			case RESPK_DialogueVolume:
			SetUserDialogueVolume(GetUserDialogueVolume() >= 0.999f ? 0.0f : GetUserDialogueVolume() + 0.1f);
			break;
			case RESPK_Back:
			SetPrompt(pprompt, PRP_Basic, PRK_OptionsMenu);
			return;
			default:
			return;
		}
		SaveSystemSettings();
		rebuildPromptKeepingSelection();
		return;

        case PRK_GameOver:
        case PRK_TryAgain:
        case PRK_MtsExpired:
        case PRK_MtsBestTime:
        case PRK_MtsFailedBestTime:
        ExitOrRestartLevelFromPrompt(pprompt, response == RESPK_Yes, WIPEK_Keyhole);
        return;

        case PRK_QuitConfirm:
        {
            if (response == RESPK_Yes)
            {
                if (g_saveData.pgsCurrentSave == nullptr)
                {
                    WipeToTitleScreen();
                    return;
                }

                pprompt->prkSaveContext = PRK_QuitConfirm;

                if ((g_pgsCur->grfgs & 0x100) == 0)
                    SetPrompt(pprompt, PRP_Memcard, PRK_MemcardAutoNotify);
                else
                {
                    writeSelectedSave();
                    return;
                }
            }

            break;
        }

		case PRK_ExitToDesktopConfirm:
		if (response == RESPK_Yes)
			fQuitGame = true;
		else
			SetPrompt(pprompt, PRP_Basic, PRK_PauseMenu);
		return;

        case PRK_MemcardMissing:
        case PRK_MemcardFormatError:
        case PRK_MemcardCreateError:
        case PRK_MemcardCardFull:
        case PRK_Unknown21:
        case PRK_Unknown32:
        closeSaveManagerPrompt();
        return;

        case PRK_MemcardFormatConfirm:
        if (response == RESPK_Yes)
        {
            //StartMemoryCardOperation(2529680, 1622440, 2509376);
            SetPrompt(pprompt, PRP_Memcard, PRK_MemcardFormatting);
            return;
        }

        closeSaveManagerPrompt();
        return;

        case PRK_MemcardCreateConfirm:
        if (response == RESPK_Yes)
        {
            //StartMemoryCardOperation(2529680, 1622608, 2509376);
            SetPrompt(pprompt, PRP_Memcard, PRK_MemcardCreateError);
            return;
        }

        closeSaveManagerPrompt();
        return;

        case PRK_MemcardChooseSaveSlot:
        {
            const int slot = getSaveSlotIndex(response);

            if (slot < 0)
                return;

            GS* selectedSave = &g_saveData.saveData[slot];
            g_saveData.pgsSelectedSave = selectedSave;

            if (selectedSave->dt != 0.0f)
            {
                SetPrompt(pprompt, PRP_Memcard, PRK_MemcardOverwriteConfirm);
                return;
            }

            g_saveData.pgsCurrentSave = selectedSave;
            pprompt->prkSaveContext = PRK_MemcardChooseSaveSlot;

            if ((g_pgsCur->grfgs & 0x100) == 0)
                SetPrompt(pprompt, PRP_Memcard, PRK_MemcardAutoNotify);
            else
            {
                writeSelectedSave();
            }

            SetPrompt(pprompt, PRP_Basic, PRK_Nil);
            return;
        }

        case PRK_MemcardSlotSaved:
        promptGroupToClear = PRP_Memcard;
        break;

        case PRK_MemcardOverwriteConfirm:
        if (response != RESPK_Yes)
        {
            promptGroupToClear = PRP_Memcard;
            break;
        }

        g_saveData.pgsCurrentSave = g_saveData.pgsSelectedSave;
        pprompt->prkSaveContext = PRK_MemcardChooseSaveSlot;

        if ((g_pgsCur->grfgs & 0x100) == 0)
            SetPrompt(pprompt, PRP_Memcard, PRK_MemcardAutoNotify);
        else
        {
            writeSelectedSave();
        }

        SetPrompt(pprompt, PRP_Basic, PRK_Nil);
        return;

        case PRK_MemcardChooseNewSlot:
        {
            const int slot = getSaveSlotIndex(response);

            if (slot < 0)
                return;

            if (g_saveData.saveData[slot].dt != 0.0f)
            {
                playInvalidSelectionSound();
                return;
            }

            SetPrompt(pprompt, PRP_Basic, PRK_Nil);
            g_saveData.pgsCurrentSave = &g_saveData.saveData[slot];
            InitGameState(g_pgsCur);
            SaveCurrentGameToDisk(&g_saveData);
            ReloadCurrentLevel();
            return;
        }

        case PRK_MemcardChooseLoadSlot:
        {
            const int slot = getSaveSlotIndex(response);

            if (slot < 0)
                return;

            g_saveData.pgsCurrentSave = &g_saveData.saveData[slot];

            // Retail sends both occupied saves and the "New Game" entries in
            // the Load Game menu through this selection path. An empty slot
            // starts a fresh game instead of being rejected as a failed load.
            if (g_saveData.saveData[slot].dt == 0.0f)
            {
                SetPrompt(pprompt, PRP_Basic, PRK_Nil);
                InitGameState(g_pgsCur);
                SaveCurrentGameToDisk(&g_saveData);
                ReloadCurrentLevel();
                return;
            }

            if (!LoadCurrentSave(&g_saveData))
            {
                playInvalidSelectionSound();
                return;
            }

            SetPrompt(pprompt, PRP_Basic, PRK_Nil);
            ReloadCurrentLevel();
            return;
        }

        case PRK_MemcardEraseConfirm:
        if (response == RESPK_Yes)
        {
            int slot = -1;
            if (g_saveData.pgsSelectedSave != nullptr)
                slot = static_cast<int>(g_saveData.pgsSelectedSave - g_saveData.saveData);

            if (!DeleteSaveSlotFromDisk(&g_saveData, slot))
            {
                playInvalidSelectionSound();
                return;
            }

            SetPrompt(pprompt, PRP_Memcard, PRK_Nil);
            return;
        }

        promptGroupToClear = PRP_Memcard;
        break;

        case PRK_MemcardAutoNotify:
        {
            g_pgsCur->grfgs |= 0x100;

            switch (pprompt->prkSaveContext)
            {
                case PRK_Nil:
                SetPrompt(pprompt, PRP_Memcard, PRK_Nil);
                g_autosave.pvtblot->pfnShowBlot(&g_autosave);
                break;

                case PRK_QuitConfirm:
                case PRK_MemcardChooseSaveSlot:
                writeSelectedSave();
                break;

                default:
                break;
            }
            return;
        }

        case PRK_OptionsMenu:
        {
            switch (response)
            {
                case RESPK_NewGame:
                SetSaveManagerState(&g_saveData, SAVE_MENU_STATE_Selecting);
                SetPrompt(pprompt, PRP_Basic, PRK_MemcardChooseNewSlot);
                return;

                case RESPK_LoadGame:
                SetSaveManagerState(&g_saveData, SAVE_MENU_STATE_Selecting);
                SetPrompt(pprompt, PRP_Basic, PRK_MemcardChooseLoadSlot);
                return;

                case RESPK_SaveGame:
                SetSaveManagerState(&g_saveData, SAVE_MENU_STATE_Selecting);
                SetPrompt(pprompt, PRP_Basic, PRK_MemcardChooseSaveSlot);
                return;

                case RESPK_Vibration:
                ToggleVibration(g_pgsCur);
                rebuildPromptKeepingSelection();
                return;

				case RESPK_Sound:
				SetPrompt(pprompt, PRP_Basic, PRK_SoundMenu);
				return;

                case RESPK_Controls:
                SetPrompt(pprompt, PRP_Basic, PRK_ControlsMenu);
                return;

                case RESPK_Video:
                SetPrompt(pprompt, PRP_Basic, PRK_VideoMenu);
                return;

                default:
                return;
            }
        }

		case PRK_LanguageMenu:
		{
			// The language choice is made here (PAL title screen, Triangle).
			// A future QoL could offer this on NTSC too, and accept
			// user-added dubs / subtitles / new languages beside the
			// extracted ones (content.cpp lists what an install holds).
			CONTENT_LANGUAGE language;
			switch (response)
			{
				case RESPK_LanguageEnglish: language = CONTENT_LANGUAGE::English; break;
				case RESPK_LanguageFrench: language = CONTENT_LANGUAGE::French; break;
				case RESPK_LanguageGerman: language = CONTENT_LANGUAGE::German; break;
				case RESPK_LanguageSpanish: language = CONTENT_LANGUAGE::Spanish; break;
				case RESPK_LanguageItalian: language = FJapaneseContent()
					? CONTENT_LANGUAGE::Japanese : CONTENT_LANGUAGE::Italian; break;
				default: return;
			}

			BeginContentLanguageChange(language);
			SetPrompt(pprompt, PRP_Basic, PRK_Nil);
			if (g_transition.m_plevelCurrent != nullptr)
			{
				// Retail PAL's PRK 0x21 handler reloads language-specific level
				// ID 3 at OID 446 with wipe kind 1. OID 446 is Paris's attract
				// entry point; OID_Nil resumes the ordinary playable spawn instead.
				WipeToWorldWarp(g_transition.m_plevelCurrent,
					static_cast<OID>(446), WIPEK_Keyhole);
			}
			else
			{
				FinishContentLanguageChange(false);
			}
			return;
		}

        case PRK_SpecialRetry:
        {
            SMA* psma = pprompt->psma;
            pprompt->psma = nullptr;

            if (psma != nullptr)
            {
                const OID oidGoal = response == RESPK_Yes ? static_cast<OID>(1076) : static_cast<OID>(1077);
                SetSmaGoal(psma, oidGoal);
            }

            break;
        }

        case PRK_MtsComplete:
        if ((GetGameProgress() & 0x08) == 0)
            ExitOrRestartLevelFromPrompt(pprompt, false, WIPEK_WorldMap);
        else
            PlayEndingFromCompletionFlag(8);

        return;

        case PRK_ControlsMenu:
        switch (response)
        {
			case RESPK_KeyboardMapping:
			s_fControllerBindingCapture = false;
			OpenMappingPrompt(pprompt, PRK_KeyboardMapping);
			return;

			case RESPK_ControllerMapping:
			s_fControllerBindingCapture = true;
			OpenMappingPrompt(pprompt, PRK_ControllerMapping);
			return;

			case RESPK_DisableControllerWhenUnfocused:
			g_fDisableControllerInputWhenUnfocused =
				!g_fDisableControllerInputWhenUnfocused;
			SaveSystemSettings();
			break;

			case RESPK_CameraInvert:
			g_pgsCur->grfgs ^= 0x1000;
			break;

			case RESPK_Mouse:
			SetPrompt(pprompt, PRP_Basic, PRK_MouseMenu);
			return;

			case RESPK_PromptStyle:
			case RESPK_AlwaysOriginalPsLayout:
			case RESPK_InstantSkipCutscenes:
			AdjustPcOption(response, 1, true);
			break;

            case RESPK_BinocInvert:
            g_pgsCur->grfgs ^= 0x200;
            break;

            case RESPK_TurretInvert:
            g_pgsCur->grfgs ^= 0x400;
            break;

            case RESPK_JetpackInvert:
            g_pgsCur->grfgs ^= 0x800;
            break;

            default:
            return;
        }

        rebuildPromptKeepingSelection();
        return;

		case PRK_MouseMenu:
		if (AdjustPcOption(response, 1, true))
			rebuildPromptKeepingSelection();
		return;

		case PRK_KeyboardMapping:
		case PRK_ControllerMapping:
		if (response == RESPK_Back)
		{
			s_iKeyboardBindingCapture = -1;
			SetPrompt(pprompt, PRP_Basic, PRK_ControlsMenu);
			return;
		}

		if (response >= RESPK_KeyUp && response <= RESPK_KeyR3)
		{
			s_iKeyboardBindingCapture = static_cast<int>(response) - static_cast<int>(RESPK_KeyUp);
			s_fKeyboardBindingCaptureArmed = false;
			std::snprintf(g_aachzKeyboardBindings[s_iKeyboardBindingCapture],
				sizeof(g_aachzKeyboardBindings[s_iKeyboardBindingCapture]),
				"%s%s%s", PchzKeyboardAction(s_iKeyboardBindingCapture), PchzPort(PTX_Sep),
				PchzPort(s_fControllerBindingCapture ? PTX_PressAButton : PTX_PressAKey));
		}
		else if (response >= RESPK_CamKeyLeft && response <= RESPK_CamKeyDown && !s_fControllerBindingCapture)
		{
			s_iKeyboardBindingCapture = kCameraKeyCapture + (response - RESPK_CamKeyLeft);
			s_fKeyboardBindingCaptureArmed = false;
			BuildPcLabel(response);
		}
		return;

        default:
        return;
    }

    SetPrompt(pprompt, promptGroupToClear, PRK_Nil);
}

void SetPromptBlots(PROMPT* pprompt, BLOTS blots)
{
    if (pprompt->blots != blots) {
        if ((pprompt->blots == BLOTS_Hidden) && (pprompt->prk == PRK_PauseMenu)) {
            SetPromptPrk(pprompt);
        }
        if (blots == BLOTS_Hidden) {
            if (pprompt->fReshow == 0) {
                if (pprompt->prk == PRK_PauseMenu) {
                    SetPromptPrk(pprompt);
                }
            }
            else {
                blots = BLOTS_Appearing;
                ChangePromptPrk(pprompt, pprompt->prkReshow);
                pprompt->fReshow = 0;
                pprompt->prkReshow = PRK_Nil;
            }
        }
        SetBlotBlots(pprompt, blots);
    }
}

void ShowPrompt(PROMPT* pprompt)
{
    if ((pprompt->blots != BLOTS_Disappearing) || (pprompt->fReshow == 0))
        ShowBlot(pprompt);
}

void HidePrompt(PROMPT* pprompt)
{
    if ((pprompt->blots == BLOTS_Disappearing) && (pprompt->fReshow != 0)) {
        pprompt->fReshow = 0;
        pprompt->prkReshow = (PRK)-1;
    }
    else
        HideBlot(pprompt);
}

const char* AchzFromRespk(RESPK respk)
{
	// (PAL / Japan / Korea: the disc's strings; USA: only a text mod's, textmod.h)
	if (FLocalizedContent() || FTextModsActive())
	{
		int variant = 0;
		switch (respk)
		{
			case RESPK_Quit: variant = g_saveData.pgsCurrentSave != nullptr; break;
			case RESPK_Vibration: variant = FVibrationEnabled(g_pgsCur) ? 0 : 1; break;
			case RESPK_Music: variant = (g_pgsCur->grfgs & 0x80U) != 0; break;
			case RESPK_Speakers: variant = (g_pgsCur->grfgs & 0x40U) != 0; break;
			case RESPK_BinocInvert: variant = (g_pgsCur->grfgs & 0x200U) != 0; break;
			case RESPK_TurretInvert: variant = (g_pgsCur->grfgs & 0x400U) != 0; break;
			case RESPK_JetpackInvert: variant = (g_pgsCur->grfgs & 0x800U) != 0; break;
			default: break;
		}
		if (const char* localized = PalResponseText(respk, variant))
			return localized;
	}

	const auto percent = [](float g) { return std::to_string(static_cast<int>(g * 100.0f + 0.5f)) + "%"; };
	if (respk == RESPK_Speakers)
		std::snprintf(g_achzRespk16, sizeof(g_achzRespk16), "Speakers: %s", FUserStereoEnabled() ? "Stereo" : "Mono");
	else if (respk == RESPK_MusicVolume)
		std::snprintf(g_achzRespk63, sizeof(g_achzRespk63), "%s", StrPortValue(PTX_MusicVolume, percent(GetUserMusicVolume())).c_str());
	else if (respk == RESPK_SfxVolume)
		std::snprintf(g_achzRespk64, sizeof(g_achzRespk64), "%s", StrPortValue(PTX_SfxVolume, percent(GetUserSfxVolume())).c_str());
	else if (respk == RESPK_DialogueVolume)
		std::snprintf(g_achzRespk65, sizeof(g_achzRespk65), "%s", StrPortValue(PTX_DialogueVolume, percent(GetUserDialogueVolume())).c_str());
	// The port's menu entries (porttext.h).
	switch (respk)
	{
	case RESPK_ExitToDesktop: return PchzPort(PTX_ExitToDesktop);
	case RESPK_Video: return PchzPort(PTX_VideoEntry);
	case RESPK_Sound: return PchzPort(PTX_SoundEntry);
	case RESPK_KeyboardMapping: return PchzPort(PTX_KeyboardMappingEntry);
	case RESPK_ControllerMapping: return PchzPort(PTX_ControllerMappingEntry);
	case RESPK_Mouse: return PchzPort(PTX_MouseEntry);
	default: break;
	}
	if (respk >= RESPK_PromptStyle && respk < RESPK_LanguageEnglish && respk != RESPK_Mouse)
		BuildPcLabel(respk);
    return s_mprespkachz[respk];
}

void BuildSaveSlotText(GS* pgs, char* pchzDst)
{
    int percentComplete = CalculatePercentCompletion(pgs);
    const int levelId = (pgs->gameWorldCur << 8) | pgs->worldLevelCur;
    const LEVELINFO* levelInfo = PlevelInfoFromId(levelId);
    const char* pchzLevel = PchzFriendlyFromLevelId(levelId);
    if (levelInfo != nullptr)
    {
        const std::ptrdiff_t levelIndexInTable = levelInfo - g_levelTable;
        const char* localized = levelIndexInTable >= 0 && levelIndexInTable < 46
            ? ContentLocalizedText(25 + static_cast<std::size_t>(levelIndexInTable)) : nullptr;
        if (localized != nullptr)
            pchzLevel = localized;
    }
    int totalSeconds = std::max(static_cast<int>(pgs->dt / 60.0f), 1);

    std::snprintf(pchzDst, 64, "%d%% - %s - %d:%02d", percentComplete, pchzLevel, totalSeconds / 60, totalSeconds % 60);
}

void OnPromptActive(PROMPT* pprompt, int fActive)
{
    if (pprompt->fActive == fActive)
        return;

    const int levelId = (g_pgsCur->gameWorldCur << 8) | g_pgsCur->worldLevelCur;

    if (fActive == 0)
    {
        if (levelId == 0x0308)
        {
            ResumeVag();
            ContinueMusicSequencer();
        }

        if (levelId != 0x0308 &&
            g_binoc.pdialogPlaying == nullptr &&
            pprompt->prk == PRK_PauseMenu)
        {
            SetUiUis(&g_ui, UIS_Unpausing);
        }
        else
            SetUiUis(&g_ui, UIS_Playing);

        HandlePromptPrkTransition(pprompt, pprompt->prk, PRK_Nil);
        pprompt->pvtblot->pfnHideBlot(pprompt);

        RemoveGrfusr(1);

        pprompt->fActive = 0;
        return;
    }

    if (levelId == 0x0308)
    {
        PauseVag();
        PauseMusicSequencer();
        SetUiUis(&g_ui, UIS_Paused);
    }
    else if (g_binoc.pdialogPlaying == nullptr)
    {
        if (g_ui.uis == UIS_Playing)
            SetUiUis(&g_ui, UIS_Pausing);
        else
            SetUiUis(&g_ui, UIS_Paused);
    }
    else
        SetUiUis(&g_ui, UIS_Paused);

    HandlePromptPrkTransition(pprompt, PRK_Nil, pprompt->prk);
    pprompt->pvtblot->pfnShowBlot(pprompt);

    AddGrfusr(1);
    g_joy.StartJoySelection();

    pprompt->fActive = 1;
}

void ExitOrRestartLevelFromPrompt(PROMPT* pprompt, int fRestartLevel, WIPEK wipek)
{
    if (pprompt == nullptr || g_pgsCur == nullptr)
        return;

    SetPrompt(pprompt, PRP_Basic, PRK_Nil);

    const int levelId = (static_cast<int>(g_pgsCur->gameWorldCur) << 8) | static_cast<int>(g_pgsCur->worldLevelCur);

    if (levelId == 0x0106)
    {
        if (fRestartLevel == 0)
        {
            ROB* prob = static_cast<ROB*>(PloFindSwObjectByClass(g_psw, 5, CID_ROB, nullptr));

            if (prob != nullptr)
                SetRobRobs(prob, ROBS_Max);
        }
        else
        {
            LEVELINFO* plevelCurrent = g_transition.m_plevelCurrent;

            if (plevelCurrent == nullptr && !g_transition.m_worldCurrent.empty())
                plevelCurrent = PlevelinfoFromLevelName(g_transition.m_worldCurrent);

            if (plevelCurrent == nullptr)
            {
                std::printf("RESTART LEVEL ERROR: no current level\n");
                return;
            }

            TRANS trans{};
            trans.fSet = 1;
            trans.plevel = plevelCurrent;
            trans.oidWarp = OID_Nil;
            trans.oidWarpContext = OID_Nil;
            trans.grftrans = static_cast<GRFTRANS>(0);

            ActivateWipe(&g_wipe, &trans, wipek);
        }

        return;
    }

    if (fRestartLevel == 0)
    {
        TriggerDefaultExit(0, wipek);
        return;
    }

    LEVELINFO* plevelCurrent = g_transition.m_plevelCurrent;

    if (plevelCurrent == nullptr && !g_transition.m_worldCurrent.empty())
        plevelCurrent = PlevelinfoFromLevelName(g_transition.m_worldCurrent);

    if (plevelCurrent == nullptr)
    {
        std::printf("RESTART LEVEL ERROR: no current level\n");
        return;
    }

    TRANS trans{};
    trans.fSet = 1;
    trans.plevel = plevelCurrent;
    trans.oidWarp = OID_Nil;
    trans.oidWarpContext = OID_Nil;
    trans.grftrans = static_cast<GRFTRANS>(0x10);

    ActivateWipe(&g_wipe, &trans, wipek);
}

void WipeToTitleScreen()
{
    LEVELINFO* plevelSplash = PlevelinfoFromSearchKey(21);

    if (plevelSplash == nullptr)
    {
        std::printf("TITLE SCREEN ERROR: Splash level not found\n");
        return;
    }

    TRANS trans{};
    trans.fSet = 1;
    trans.plevel = plevelSplash;
    trans.oidWarp = OID_Nil;
    trans.oidWarpContext = OID_Nil;
    trans.grftrans = static_cast<GRFTRANS>(0x04);

    ActivateWipe(&g_wipe, &trans, WIPEK_Fade);
}

void UpdatePromptActive(PROMPT* pprompt, JOY* pjoy)
{
    constexpr uint16_t kBtnConfirm = 0x0040;
    constexpr uint16_t kBtnErase = 0x0080;
    constexpr uint16_t kBtnCancel = 0x0810;

    if (pprompt == nullptr || pjoy == nullptr || g_pgsCur == nullptr)
        return;

    const PRK prk = pprompt->prk;

    if (prk < PRK_PauseMenu || prk >= PRK_Max)
        return;

	// Controller names follow the controller in use while the list is open.
	if (prk == PRK_ControllerMapping && s_iKeyboardBindingCapture < 0 && PromptFamily() != s_familyMapping)
		BuildBindingStrings(prk);

	if ((prk == PRK_KeyboardMapping || prk == PRK_ControllerMapping) &&
		s_iKeyboardBindingCapture >= 0 && g_gl.window != nullptr)
	{
		// Escape is reserved for leaving menus and is never assignable.
		if (glfwGetKey(g_gl.window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
		{
			const int selection = pprompt->irespk;
			s_iKeyboardBindingCapture = -1;
			s_fKeyboardBindingCaptureArmed = false;
			SetPromptPrk(pprompt);
			pprompt->irespk = selection;
			return;
		}

		int key = s_fControllerBindingCapture
			? GamepadBindingPressed()
			: KeyboardKeyPressed(g_gl.window);
		// Actions also take mouse buttons and the wheel; camera keys do not.
		if (!s_fControllerBindingCapture && key == GLFW_KEY_UNKNOWN && s_iKeyboardBindingCapture < kCameraKeyCapture)
		{
			const int mouse = MouseBindingPressed(g_gl.window);
			if (mouse >= 0)
				key = mouse;
		}
		const int noBinding = s_fControllerBindingCapture ? -1 : GLFW_KEY_UNKNOWN;
		if (!s_fKeyboardBindingCaptureArmed)
		{
			if (key == noBinding)
				s_fKeyboardBindingCaptureArmed = true;
			return;
		}

		if (key != noBinding)
		{
			const int selection = pprompt->irespk;
			if (s_fControllerBindingCapture)
				g_gamepadBindings[s_iKeyboardBindingCapture] = key;
			else if (s_iKeyboardBindingCapture >= kCameraKeyCapture)
				g_cameraKeyBindings[static_cast<size_t>(s_iKeyboardBindingCapture - kCameraKeyCapture)] = key;
			else
				g_keyboardBindings[s_iKeyboardBindingCapture] = key;
			SaveSystemSettings();
			s_iKeyboardBindingCapture = -1;
			s_fKeyboardBindingCaptureArmed = false;
			SetPromptPrk(pprompt);
			pprompt->irespk = selection;
		}
		return;
	}

    // Refit a port option page when the window or the GUI scale changes
    // under it (a window mode or resolution change applies a frame later).
    if (FPromptFitStale(prk))
    {
        const int selection = pprompt->irespk;
        SetPromptPrk(pprompt);
        pprompt->irespk = std::clamp(selection, 0, std::max(0, CrespkPrompt(prk, PrdPrompt(prk)) - 1));
    }

    const int levelId = (static_cast<int>(g_pgsCur->gameWorldCur) << 8) | static_cast<int>(g_pgsCur->worldLevelCur);
    const PRD& promptData = PrdPrompt(prk);
	RESPK* const promptResponses = ArespkPrompt(prk, promptData);
	const int promptResponseCount = CrespkPrompt(prk, promptData);

    const bool isHudExcludedLevel =
        levelId == 262 ||
        levelId == 516 ||
        levelId == 772 ||
        levelId == 775 ||
        levelId == 1031 ||
        levelId == 1282 ||
        levelId == 1284;

    const bool showGameplayCounters = prk == PRK_PauseMenu && !isHudExcludedLevel;

    if (showGameplayCounters)
    {
        g_lifectr.pvtlifectr->pfnShowBlot(&g_lifectr);
        g_cluectr.pvtblot->pfnShowBlot(&g_cluectr);
        g_keyctr.pvtkeyctr->pfnShowBlot(&g_keyctr);
        g_coinctr.pvtcoinctr->pfnShowBlot(&g_coinctr);
    }
    else
    {
        g_lifectr.pvtlifectr->pfnHideBlot(&g_lifectr);
        g_cluectr.pvtblot->pfnHideBlot(&g_cluectr);
        g_keyctr.pvtkeyctr->pfnHideBlot(&g_keyctr);
        g_coinctr.pvtcoinctr->pfnHideBlot(&g_coinctr);
    }

    const bool showPercentCompletion =
        prk == PRK_PauseMenu ||
        prk == PRK_MemcardChooseSaveSlot ||
        prk == PRK_MemcardChooseLoadSlot;

    if (showPercentCompletion)
    {
        g_percentCompletion = CalculatePercentCompletion(g_pgsCur);
        g_percentctr.pnActual = &g_percentCompletion;
        g_percentctr.pvtctr->pfnShowBlot(&g_percentctr);
    }
    else
        g_percentctr.pvtctr->pfnHideBlot(&g_percentctr);

    if (pprompt->fVagPlaying != 0)
        pprompt->fVagPlaying = FVagPlaying();

    const bool canAcceptInput =
        (pprompt->fVagPlaying == 0 || prk == PRK_MemcardAutoNotify) &&
        pprompt->fReshow == 0 &&
        pprompt->blots == BLOTS_Visible &&
        g_clock.tReal - pprompt->tBlots > 0.1f;

    // PC mouse menus (from ThiefRush-Dev): hover highlights, left click
    // selects, right click goes back and the wheel moves the highlight.
    if (promptResponseCount > 0 && promptResponses != nullptr)
    {
        const int iHover = IrespkFromMouse(prk);
        if (iHover >= 0 && iHover < promptResponseCount && iHover != pprompt->irespk && FMouseMenuMoved())
        {
            pprompt->irespk = iHover;
            StartSound(static_cast<SFXID>(121), nullptr, nullptr, nullptr, 3000.0f, 300.0f, 1.0f, 0.0f, 0.0f, nullptr, nullptr);
        }
        if (canAcceptInput && FMouseLeftPressed() && iHover >= 0 && iHover < promptResponseCount)
        {
            pprompt->irespk = iHover;
            ExecutePrompt(pprompt);
            return;
        }
        const int wheel = MouseWheelNotches();
        if (wheel != 0 && promptData.fVertical != 0)
        {
            pprompt->irespk = (pprompt->irespk + (wheel > 0 ? promptResponseCount - 1 : 1)) % promptResponseCount;
            StartSound(static_cast<SFXID>(121), nullptr, nullptr, nullptr, 3000.0f, 300.0f, 1.0f, 0.0f, 0.0f, nullptr, nullptr);
            return;
        }
    }
    if (canAcceptInput && FMouseRightPressed())
    {
        CancelPrompt(pprompt);
        return;
    }

    if (promptResponseCount > 0)
    {
        const int selectionDelta = promptData.fVertical != 0 ? pjoy->DySelectionJoy(g_clock.tReal) : pjoy->DxSelectionJoy(g_clock.tReal);

        if (selectionDelta > 0)
        {
            pprompt->irespk = (pprompt->irespk + 1) % promptResponseCount;
            StartSound(static_cast<SFXID>(121), nullptr, nullptr, nullptr, 3000.0f, 300.0f, 1.0f, 0.0f, 0.0f, nullptr, nullptr);
            return;
        }

        if (selectionDelta < 0)
        {
            pprompt->irespk = (pprompt->irespk + promptResponseCount - 1) % promptResponseCount;
            StartSound(static_cast<SFXID>(121), nullptr, nullptr, nullptr, 3000.0f, 300.0f, 1.0f, 0.0f, 0.0f, nullptr, nullptr);
            return;
        }
    }

    if (!canAcceptInput)
        return;

    // Video settings use a vertical menu; horizontal input adjusts the
    // selected MSAA value without changing rows. Confirm also cycles forward.
    if (prk == PRK_VideoMenu && promptResponses != nullptr &&
        pprompt->irespk >= 0 && pprompt->irespk < promptResponseCount &&
        promptResponses[pprompt->irespk] == RESPK_Msaa)
    {
        const int direction = pjoy->DxSelectionJoy(g_clock.tReal);
        if (direction != 0) {
            const int selection = pprompt->irespk;
            SelectMsaaSetting(direction);
            SaveSystemSettings();
            SetPromptPrk(pprompt);
            pprompt->irespk = selection;
            return;
        }
    }

    // The PC option rows (Controls, Mouse) also adjust with left / right.
    if (promptData.fVertical != 0 && promptResponses != nullptr &&
        pprompt->irespk >= 0 && pprompt->irespk < promptResponseCount &&
        FPcOption(promptResponses[pprompt->irespk]))
    {
        const int direction = pjoy->DxSelectionJoy(g_clock.tReal);
        if (direction != 0 && AdjustPcOption(promptResponses[pprompt->irespk], direction, false)) {
            const int selection = pprompt->irespk;
            SetPromptPrk(pprompt);
            pprompt->irespk = selection;
            StartSound(static_cast<SFXID>(121), nullptr, nullptr, nullptr, 3000.0f, 300.0f, 1.0f, 0.0f, 0.0f, nullptr, nullptr);
            return;
        }
    }

    // NTSC-J swaps the menu buttons: Circle confirms, Cross backs out. NTSC-K confirms with Circle too.
    const JOY_BUTTON confirmButton = FCircleConfirms() ? BTN_CIRCLE : BTN_CROSS;
    const JOY_BUTTON cancelButton = FJapaneseContent() ? BTN_CROSS : BTN_TRIANGLE;

    if (pjoy->IsPressed(BTN_START) || pjoy->IsPressed(cancelButton))
    {
        pjoy->SetHandled(BTN_START);
        pjoy->SetHandled(cancelButton);
        CancelPrompt(pprompt);
        return;
    }

    if (pjoy->IsPressed(BTN_SQUARE))
    {
        pjoy->SetHandled(BTN_SQUARE);

        if ((prk == PRK_MemcardChooseSaveSlot ||
             prk == PRK_MemcardChooseLoadSlot) &&
            promptResponses != nullptr &&
            pprompt->irespk >= 0 && pprompt->irespk < SAVE_SLOT_COUNT)
        {
            const RESPK response = promptResponses[pprompt->irespk];
            int slot = -1;

            for (int i = 0; i < SAVE_SLOT_COUNT; ++i)
            {
                if (s_arespkSlots[i] == response)
                {
                    slot = i;
                    break;
                }
            }

            if (slot >= 0 && g_saveData.saveData[slot].dt != 0.0f)
            {
                g_saveData.pgsSelectedSave = &g_saveData.saveData[slot];
                SetPrompt(pprompt, PRP_Memcard, PRK_MemcardEraseConfirm);
            }
        }

        return;
    }

    if (pjoy->IsPressed(confirmButton))
    {
        // Consume the confirmation before ExecutePrompt can close the menu or
        // reload a level. Otherwise JT sees this same press as a jump.
        pjoy->SetHandled(confirmButton);
        ExecutePrompt(pprompt);
        return;
    }
}

void CancelPrompt(PROMPT* pprompt)
{
    if (pprompt == nullptr)
        return;

    switch (pprompt->prk)
    {
        case PRK_PauseMenu:
        SetPrompt(pprompt, PRP_Basic, PRK_Nil);
        break;

		case PRK_LanguageMenu:
		SetPrompt(pprompt, PRP_Basic, PRK_Nil);
		break;

        case PRK_QuitConfirm:
		case PRK_ExitToDesktopConfirm:
        SetPrompt(pprompt, PRP_Basic, PRK_PauseMenu);
        break;

        case PRK_MemcardChooseSaveSlot:
        case PRK_MemcardChooseNewSlot:
        case PRK_MemcardChooseLoadSlot:
        case PRK_ControlsMenu:
        case PRK_VideoMenu:
		case PRK_SoundMenu:
        SetPrompt(pprompt, PRP_Basic, PRK_OptionsMenu);
        break;

		case PRK_KeyboardMapping:
		case PRK_ControllerMapping:
		s_iKeyboardBindingCapture = -1;
		s_fKeyboardBindingCaptureArmed = false;
		SetPrompt(pprompt, PRP_Basic, PRK_ControlsMenu);
		break;

		case PRK_MouseMenu:
		SetPrompt(pprompt, PRP_Basic, PRK_ControlsMenu);
		break;

        case PRK_MemcardSlotSaved:
        case PRK_MemcardOverwriteConfirm:
        case PRK_MemcardEraseConfirm:
        SetPrompt(pprompt, PRP_Memcard, PRK_Nil);
        break;

        case PRK_OptionsMenu:
        {
            const unsigned playingState = static_cast<unsigned>(static_cast<int>(g_ui.uisPlaying) + static_cast<int>(UIS_Nil));

            if (playingState > 1U)
                SetPrompt(pprompt, PRP_Basic, PRK_PauseMenu);
            else
                SetPrompt(pprompt, PRP_Basic, PRK_Nil);

            break;
        }

        default:
        break;
    }
}

void DrawPrompt(PROMPT* pprompt)
{
    if (pprompt == nullptr || pprompt->pfont == nullptr || pprompt->blots == BLOTS_Hidden || pprompt->uOn <= 0.0f)
        return;

    if (pprompt->prk < PRK_PauseMenu || pprompt->prk >= PRK_Max)
        return;

    PRD& prd = PrdPrompt(pprompt->prk);
	RESPK* const promptResponses = ArespkPrompt(pprompt->prk, prd);
	const int promptResponseCount = CrespkPrompt(pprompt->prk, prd);

    const float uOn = pprompt->uOn;
    const float dxUnscaled = pprompt->dx * uOn;
    const float dyUnscaled = pprompt->dy * uOn;

    // Retail draws prompts in the PS2's 640 x 492.8 screen space.  The GS
    // presentation then expands that canvas to the output dimensions.  Our
    // blot projection is expressed directly in output pixels, so perform the
    // same conversion here instead of treating retail coordinates as pixels.
    const GuiScale promptScale = GetGuiScale();
    const float promptScaleX = promptScale.x;
    const float promptScaleY = promptScale.y;

    const float centerX = pprompt->xOn + pprompt->dx * 0.5f;
    const float centerY = pprompt->yOn + pprompt->dy * 0.5f;

    const float xUnscaled = pprompt->xOn * uOn + centerX * (1.0f - uOn);
    const float yUnscaled = pprompt->yOn * uOn + centerY * (1.0f - uOn);
    const float dx = dxUnscaled * promptScaleX;
    const float dy = dyUnscaled * promptScaleY;
    // RepositionBlot currently centers xOn/yOn in output-pixel space while
    // retaining the retail (unscaled) dx/dy.  Preserve that already-correct
    // output-space center and scale only the offset from it.  Scaling xOn or
    // yOn again would move the prompt toward the lower-right corner.
    const float x = centerX + (xUnscaled - centerX) * promptScaleX;
    float y = centerY + (yUnscaled - centerY) * promptScaleY;
    const float yBox = y;

    const float pulse = std::sin(g_clock.tReal * 10.0f) * 0.5f + 0.5f;
    const float pulseSquared = pulse * pulse;

    float targetAlpha = 1.0f;

    if (pprompt->prk == PRK_PauseMenu && g_ui.cpblotActive == 2)
        targetAlpha = 1.0f - std::clamp(g_joy.uDeflect2, 0.0f, 1.0f);

    const float alphaBlend = 1.0f - std::exp(-12.0f * g_clock.dtReal);
    pprompt->alpha = glm::mix(pprompt->alpha, targetAlpha, std::clamp(alphaBlend, 0.0f, 1.0f));

    const float alpha = std::clamp(pprompt->alpha, 0.0f, 1.0f);

    // These are PS2 GS vertex-color values. RGB uses 0x80 as full intensity,
    // while alpha in this path is still authored over the full byte range.
    // Dividing the RGB channels by 255 makes the entire prompt about half as
    // bright as retail.
    constexpr float kGsColorScale = 1.0f / 128.0f;
    glm::vec4 edgeBoxColor(128.0f * kGsColorScale, 128.0f * kGsColorScale, 128.0f * kGsColorScale, alpha);
    glm::vec4 edgePulseTarget(111.0f * kGsColorScale, 111.0f * kGsColorScale, 111.0f * kGsColorScale, alpha);
    glm::vec4 titleColor(127.0f * kGsColorScale, 127.0f * kGsColorScale, 127.0f * kGsColorScale, alpha);
    glm::vec4 responseColor(95.0f * kGsColorScale, 95.0f * kGsColorScale, 95.0f * kGsColorScale, alpha);

    const auto GetSelectedColor = [&](float pulseValue)
        {
            const float blue = (47.0f + pulseValue * 64.0f) * kGsColorScale;
            return glm::vec4(111.0f * kGsColorScale, 111.0f * kGsColorScale, blue, alpha);
        };

    CTextBox textBox;

    // -------------------------------------------------------------------------
    // Background/edge rectangle
    // -------------------------------------------------------------------------

    if (pprompt->pte != nullptr)
    {
        textBox.SetPos(x, y);
        textBox.SetSize(dx, dy);
        textBox.SetTextColor(&edgeBoxColor);
        textBox.SetHorizontalJust(JH_Left);
        textBox.SetVerticalJust(JV_Top);

        const glm::vec4 savedEdgeColor = pprompt->pte->m_rgba;

        glm::vec4 edgeColor = savedEdgeColor;
        edgeColor.a = alpha;

        if (promptResponseCount == 0)
            edgeColor = glm::mix(edgeColor, edgePulseTarget, pulseSquared);

        pprompt->pte->m_rgba = edgeColor;

        if (pprompt->pte->m_pfont != nullptr)
        {
            const float savedEdgeScaleX = pprompt->pte->m_rxScaling;
            const float savedEdgeScaleY = pprompt->pte->m_ryScaling;
            pprompt->pte->m_rxScaling *= promptScaleX;
            pprompt->pte->m_ryScaling *= promptScaleY;
            pprompt->pte->m_pfont->EdgeRect(pprompt->pte, &textBox);
            pprompt->pte->m_rxScaling = savedEdgeScaleX;
            pprompt->pte->m_ryScaling = savedEdgeScaleY;
        }

        pprompt->pte->m_rgba = savedEdgeColor;
    }

    // -------------------------------------------------------------------------
    // Prompt title
    // -------------------------------------------------------------------------

    const char* const promptTitle = PchzPromptTitle(pprompt->prk, prd);
    if (promptTitle != nullptr)
    {
        const float titleScaleX = prd.rScaleTitle * uOn * promptScaleX;
        const float titleScaleY = prd.rScaleTitle * uOn * promptScaleY;

        pprompt->pfont->PushScaling(titleScaleX, titleScaleY);

        textBox.SetPos(x, y);
        textBox.SetSize(dx, dy);
        textBox.SetTextColor(&titleColor);
        textBox.SetHorizontalJust(JH_Center);
        textBox.SetVerticalJust(JV_Top);

        CRichText titleText(const_cast<char*>(promptTitle), pprompt->pfont);

        titleText.Draw(&textBox, nullptr);
        y += titleText.DyWrap(500.0f * promptScaleX);

        pprompt->pfont->PopScaling();
    }

    // NTSC/PAL place the divider one unit above the response row. Japanese
    // places it at the title baseline and starts responses eight units later.
    const bool japanesePromptLayout = FJapaneseContent() && promptResponseCount > 0;
    const float dividerY = japanesePromptLayout ? y : y - promptScaleY;
    if (japanesePromptLayout)
        y += 8.0f * promptScaleY;

    // -------------------------------------------------------------------------
    // Responses
    // -------------------------------------------------------------------------

    const float rScaleRespk = RScaleRespkPrompt(pprompt->prk, prd);
    const float responseScaleX = rScaleRespk * uOn * promptScaleX;
    const float responseScaleY = rScaleRespk * uOn * promptScaleY;

    pprompt->pfont->PushScaling(responseScaleX, responseScaleY);

    const float baseLineHeight = static_cast<float>(pprompt->pfont->m_dyUnscaled) * pprompt->pfont->m_ryScale;
    const float spaceWidth = static_cast<float>(pprompt->pfont->m_dxSpaceUnscaled) * pprompt->pfont->m_rxScale;

    float cursorX = x;
    float cursorY = y;

    // The original prompt was designed for short menus. Keep mapping menus
    // within that layout and scroll the visible window around the selection.
    // The port's option pages scroll the same way when they do not fit.
    int firstResponse = 0;
    int lastResponse = promptResponseCount;
    const int visibleBindings = FMappingPrompt(pprompt->prk) ? 9 : CrespkVisiblePrompt(pprompt->prk, promptResponseCount);
    if (visibleBindings < promptResponseCount)
    {
        firstResponse = std::clamp(pprompt->irespk - visibleBindings / 2,
            0, std::max(0, promptResponseCount - visibleBindings));
        lastResponse = std::min(promptResponseCount, firstResponse + visibleBindings);
    }
    const bool scrollMarks = s_promptFit.prk == pprompt->prk && visibleBindings < promptResponseCount;
    if (scrollMarks)
        cursorY += kPromptScrollMark * promptScaleY;

    // Measure the complete horizontal response row.
    if (prd.fVertical == 0)
    {
        float totalWidth = 0.0f;

        for (int i = firstResponse; i < lastResponse; ++i)
        {
            if (i > 0)
                totalWidth += spaceWidth * 2.0f;

            const RESPK response = promptResponses[i];

            if (response == RESPK_Nil)
                continue;

            const char* responseText = AchzFromRespk(response);

            if (responseText == nullptr)
                continue;

            CRichText richText(const_cast<char*>(responseText), pprompt->pfont);

            totalWidth += richText.Dx();
        }

        cursorX = x + (dx - totalWidth) * 0.5f;
    }

    s_chit = 0;
    s_prkHit = pprompt->prk;
    s_tHit = g_clock.tReal;
    for (int i = firstResponse; i < lastResponse; ++i)
    {
        const RESPK response = promptResponses[i];

        if (response == RESPK_Nil)
            continue;

        const char* responseText = AchzFromRespk(response);

        if (responseText == nullptr)
            continue;

        // Measure the item at its normal response scale.
        CRichText unselectedText(const_cast<char*>(responseText), pprompt->pfont);

        const float unselectedWidth = unselectedText.Dx();
        const bool selected = i == pprompt->irespk;

        glm::vec4 color = selected ? GetSelectedColor(pulse) : responseColor;
        const float selectionScale = selected ? 1.0f + (1.0f - pulse) * 0.1f : 1.0f;
        const float itemScaleX = responseScaleX * selectionScale;
        const float itemScaleY = responseScaleY * selectionScale;

        pprompt->pfont->PushScaling(itemScaleX, itemScaleY);

        // Reconstruct after changing the scale so switched fonts inherit it.
        CRichText selectedText(const_cast<char*>(responseText), pprompt->pfont);

        const float selectedWidth = selectedText.Dx();
        const float selectedHeight = static_cast<float>(pprompt->pfont->m_dyUnscaled) * pprompt->pfont->m_ryScale;

        float drawX = cursorX + (unselectedWidth - selectedWidth) * 0.5f;
        const float drawY = cursorY + (baseLineHeight - selectedHeight) * 0.5f;

        if (prd.fVertical != 0)
            drawX = x + (dx - unselectedWidth) * 0.5f + (unselectedWidth - selectedWidth) * 0.5f;

        textBox.SetPos(drawX, drawY);
        textBox.SetSize(dx, selectedHeight);
        textBox.SetTextColor(&color);
        textBox.SetHorizontalJust(JH_Left);
        textBox.SetVerticalJust(JV_Top);

        selectedText.Draw(&textBox, nullptr);

        pprompt->pfont->PopScaling();

        // Mouse target: the whole row of a vertical menu, the word otherwise.
        if (s_chit < static_cast<int>(std::size(s_ahit)))
            s_ahit[s_chit++] = prd.fVertical != 0
                ? PROMPTHIT{ x, cursorY, x + dx, cursorY + baseLineHeight, i }
                : PROMPTHIT{ cursorX - spaceWidth, cursorY, cursorX + unselectedWidth + spaceWidth, cursorY + baseLineHeight, i };

        if (prd.fVertical == 0)
            cursorX += unselectedWidth + spaceWidth * 2.0f;
        else
            cursorY += baseLineHeight;
    }

    pprompt->pfont->PopScaling();

    // -------------------------------------------------------------------------
    // Title/response divider
    // -------------------------------------------------------------------------

    if (promptTitle != nullptr && promptResponseCount > 0)
    {
        const glm::vec4 dividerColor(95.0f * kGsColorScale, 95.0f * kGsColorScale, 95.0f * kGsColorScale, alpha);

        DrawLineScreen(x, dividerY, 0.0f, x + dx, dividerY, 0.0f, dividerColor, false);
    }

    // A fitted page's scroll arrows: a small chevron above the rows when
    // some are hidden above, and below them when some are hidden below.
    if (scrollMarks)
    {
        const glm::vec4 markColor(95.0f * kGsColorScale, 95.0f * kGsColorScale, 95.0f * kGsColorScale, alpha);
        const float cx = x + dx * 0.5f;
        const float halfWidth = 7.0f * promptScaleX;
        const float height = 4.0f * promptScaleY;
        const auto chevron = [&](float yMid, float dir)
            {
                for (int line = 0; line < 2; ++line)
                {
                    const float o = static_cast<float>(line) * promptScaleY;
                    DrawLineScreen(cx - halfWidth, yMid + dir * height * 0.5f + o, 0.0f, cx, yMid - dir * height * 0.5f + o, 0.0f, markColor, false);
                    DrawLineScreen(cx, yMid - dir * height * 0.5f + o, 0.0f, cx + halfWidth, yMid + dir * height * 0.5f + o, 0.0f, markColor, false);
                }
            };
        const float rowsTop = y + kPromptScrollMark * promptScaleY;
        if (firstResponse > 0)
            chevron(y + kPromptScrollMark * 0.5f * promptScaleY, 1.0f);
        if (lastResponse < promptResponseCount)
            chevron(rowsTop + baseLineHeight * static_cast<float>(lastResponse - firstResponse) + kPromptScrollMark * 0.5f * promptScaleY, -1.0f);
    }

    // What the highlighted PC option does, in a smaller line under the box.
    if (promptResponses != nullptr && pprompt->irespk >= 0 && pprompt->irespk < promptResponseCount && uOn >= 1.0f)
    {
        if (const char* help = PchzPcHelp(promptResponses[pprompt->irespk]))
        {
            pprompt->pfont->PushScaling(0.5f * promptScaleX, 0.5f * promptScaleY);
            glm::vec4 helpColor(108.0f * kGsColorScale, 108.0f * kGsColorScale, 108.0f * kGsColorScale, alpha);
            textBox.SetPos(x - dx * 0.25f, yBox + dy + 8.0f * promptScaleY);
            textBox.SetSize(dx * 1.5f, 60.0f * promptScaleY);
            textBox.SetTextColor(&helpColor);
            textBox.SetHorizontalJust(JH_Center);
            textBox.SetVerticalJust(JV_Top);
            CRichText helpText(const_cast<char*>(help), pprompt->pfont);
            helpText.Draw(&textBox, nullptr);
            pprompt->pfont->PopScaling();
        }
    }
}

char GetAnimatedPromptCharacter()
{
    constexpr int frameCount = static_cast<int>(sizeof(s_achzAnimatedPromptFrames) - 1);

    const int frame =
        static_cast<int>(g_clock.tReal * 5.0f) % frameCount;

    return s_achzAnimatedPromptFrames[frame];
}

PROMPT g_prompt;
CTextEdge g_tePrompt;
float g_promptFade;

// The string number of a response in the PAL localization table (textmod.cpp's USA dump uses the same numbers).
int PalResponseStringIndex(RESPK respk, int variant)
{
	return PalResponseLocalizationIndex(respk, variant);
}
