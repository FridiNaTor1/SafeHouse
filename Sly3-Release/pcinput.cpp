// See pcinput.h. After ThiefRush-Dev's Sly1-Release Input.cpp (JOY::Update: bindings, Escape, Enter in menus,
// the camera keys, the device tracker) and mouseinput.cpp (capture, motion, bindings, menu buttons).

#include "pcinput.h"

#include "display.h"
#include "frontend.h"
#include "hackmouse.h"
#include "joy.h"
#include "menu.h"

#include <GLFW/glfw3.h>

#include "../platform/button_prompts.h"
#include "../platform/platform.h"
#include "../platform/controller_family.h"
#include "../platform/system_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace st = cane::settings;

extern int g_frameJoy;   // (joy.cpp: the step's frame)

namespace
{
// (tests) SLY2N_TESTKEYS=binding@frame[+count],...: the binding (a GLFW key code, or a mouse button 2000 + n) held
// for count steps (default 4) from that step, as if pressed in the window (window runs only; prints the pad bits the
// game gets while one is held)
struct TESTKEY { int binding, frame0, frame1; };
const std::vector<TESTKEY>& TestKeys()
{
    static std::vector<TESTKEY> s_a;
    static bool s_fParsed = false;
    if (!s_fParsed)
    {
        s_fParsed = true;
        if (const char* psz = std::getenv("SLY2N_TESTKEYS"))
        {
            std::string s(psz);
            for (size_t i = 0; i < s.size();)
            {
                size_t j = s.find(',', i);
                if (j == std::string::npos) j = s.size();
                int binding = 0, frame = 0, count = 4;
                if (std::sscanf(s.substr(i, j - i).c_str(), "%d@%d+%d", &binding, &frame, &count) >= 2)
                    s_a.push_back({binding, frame, frame + count});
                i = j + 1;
            }
        }
    }
    return s_a;
}
bool FTestKeyHeld(int binding)
{
    for (const TESTKEY& k : TestKeys())
        if (k.binding == binding && g_frameJoy >= k.frame0 && g_frameJoy < k.frame1) return true;
    return false;
}
}  // namespace

PCINPUTOPT g_pcinput = PcInputDefaults();

PCINPUTOPT PcInputDefaults()
{
    PCINPUTOPT o;
    // (Sly 2's own keyboard / mouse layout: st::kSly2DefaultKeyboardBindings)
    std::copy(std::begin(st::kSly2DefaultKeyboardBindings), std::end(st::kSly2DefaultKeyboardBindings), o.keyboard.begin());
    std::copy(std::begin(st::kDefaultGamepadBindings), std::end(st::kDefaultGamepadBindings), o.gamepad.begin());
    std::copy(std::begin(st::kDefaultCameraKeys), std::end(st::kDefaultCameraKeys), o.cameraKeys.begin());
    return o;
}

void PcInputFromSettings(const st::SystemSettings& s)
{
    PCINPUTOPT& o = g_pcinput;
    std::copy(std::begin(s.keyboardBindings), std::end(s.keyboardBindings), o.keyboard.begin());
    std::copy(std::begin(s.gamepadBindings), std::end(s.gamepadBindings), o.gamepad.begin());
    std::copy(std::begin(s.cameraKeyBindings), std::end(s.cameraKeyBindings), o.cameraKeys.begin());
    o.fDisableUnfocusedPad = s.disableControllerInputWhenUnfocused != 0;
    o.fMouseCamera = s.mouseCamera != 0;
    o.mouseSensitivity = s.mouseSensitivity;
    o.mouseAimSensitivity = s.mouseAimSensitivity;
    o.fMouseInvertX = s.mouseInvertX != 0;
    o.fMouseInvertY = s.mouseInvertY != 0;
    o.fMouseRaw = s.mouseRawInput != 0;
    o.fMouseWheelZoom = s.mouseWheelZoom != 0;
    o.fMouseMenus = s.mouseMenus != 0;
    o.promptStyle = s.promptStyle;
    o.fAlwaysOriginalPs = s.alwaysOriginalPsPrompts != 0;
    o.fInstantSkipCutscenes = s.instantSkipCutscenes != 0;
    o.fHackMouseAim = s.hackMouseAim != 0;
    // (fDebugKeys: not from the settings' v14 debugKeys, which is ignored and kept as is since 2026-11-04; the
    //  launcher's Settings > Advanced > Debug mode turns them on, read in main.cpp from Debug Mode.ini)
}

void PcInputToSettings(st::SystemSettings& s)
{
    const PCINPUTOPT& o = g_pcinput;
    std::copy(o.keyboard.begin(), o.keyboard.end(), s.keyboardBindings);
    std::copy(o.gamepad.begin(), o.gamepad.end(), s.gamepadBindings);
    std::copy(o.cameraKeys.begin(), o.cameraKeys.end(), s.cameraKeyBindings);
    s.disableControllerInputWhenUnfocused = o.fDisableUnfocusedPad;
    s.mouseCamera = o.fMouseCamera;
    s.mouseSensitivity = o.mouseSensitivity;
    s.mouseAimSensitivity = o.mouseAimSensitivity;
    s.mouseInvertX = o.fMouseInvertX;
    s.mouseInvertY = o.fMouseInvertY;
    s.mouseRawInput = o.fMouseRaw;
    s.mouseWheelZoom = o.fMouseWheelZoom;
    s.mouseMenus = o.fMouseMenus;
    s.promptStyle = o.promptStyle;
    s.alwaysOriginalPsPrompts = o.fAlwaysOriginalPs;
    s.instantSkipCutscenes = o.fInstantSkipCutscenes;
    s.hackMouseAim = o.fHackMouseAim;
}

void ApplyPcDebugMode()
{
    g_pcinput.fDebugKeys = cane::debugMode();
    std::printf("debug mode: %s\n", g_pcinput.fDebugKeys ? "on" : "off");
}

void ApplyPcInputEnv()
{
    const char* psz = std::getenv("SLY2N_PCOPT");
    if (!psz) return;
    std::string s(psz);
    size_t i = 0;
    while (i < s.size())
    {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        const std::string item = s.substr(i, j - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
        {
            const std::string k = item.substr(0, eq);
            const int n = std::atoi(item.c_str() + eq + 1);
            if (k == "alwaysps") g_pcinput.fAlwaysOriginalPs = n != 0;
            else if (k == "instantskip") g_pcinput.fInstantSkipCutscenes = n != 0;
            else if (k == "promptstyle") g_pcinput.promptStyle = std::clamp(n, 0, cane::input::kPromptStyleLast);
            // (tests: a binding by action index, pcinput.h PCACTION: pad6=1 puts Cross on GLFW button B, key4=81)
            else if (k.rfind("pad", 0) == 0 && std::atoi(k.c_str() + 3) >= 0 && std::atoi(k.c_str() + 3) < PCA_Max)
                g_pcinput.gamepad[size_t(std::atoi(k.c_str() + 3))] = n;
            else if (k.rfind("key", 0) == 0 && std::atoi(k.c_str() + 3) >= 0 && std::atoi(k.c_str() + 3) < PCA_Max)
                g_pcinput.keyboard[size_t(std::atoi(k.c_str() + 3))] = n;
            else if (k == "mousecamera") g_pcinput.fMouseCamera = n != 0;
            else if (k == "hackmouse") g_pcinput.fHackMouseAim = n != 0;
            else if (k == "debugkeys") g_pcinput.fDebugKeys = n != 0;
        }
        i = j + 1;
    }
}

void StartupPcInput()
{
    cane::input::promptAtlas().upload();   // (no atlas: every family falls back to the game's own icons)
    cane::input::tracker().forceFromEnv();
}

bool FPcMenuMode()
{
    // (the menus' own pad: the pause / options / card pages, the title, the episode menu; a world's load or a
    // talk also takes the players' pad (lock bits 4 / 8): no camera there either)
    bool FEpisodeMenuActive();   // frontend.cpp
    return FMenuShown() || FTitleShown() || FEpisodeMenuActive() || (g_grfjoyt & 2) == 0;
}

// ------------------------------------------------------------ the mouse
namespace
{
constexpr int MOUSE_BUTTONS = st::kMouseButtonCount;
// Radians of turn per mouse count at 100% (ThiefRush's tuning).
constexpr float RAD_PER_COUNT_ORBIT = 0.0030f;
constexpr float RAD_PER_COUNT_AIM = 0.0012f;

bool s_fCaptured = false;
bool s_fLive = false;                           // (FPcInputLive)
bool s_fFreeHidden = false;                     // (hackmouse.h) the cursor free in the window, the OS one hidden
bool s_fCursor = false;                         // s_xCursor / s_yCursor: the free cursor in framebuffer pixels
double s_xCursor = 0.0, s_yCursor = 0.0;
bool s_fHaveLast = false;
double s_xLast = 0.0, s_yLast = 0.0;
float s_dxCounts = 0.0f, s_dyCounts = 0.0f;   // + = right / down
double s_dyScroll = 0.0;                        // the scroll callback's sum
int s_wheel = 0;                                // this step's notches, + = up
int s_cwheelMenu = 0;                           // menu steps still to give (one press every other step)
bool s_afButton[MOUSE_BUTTONS] = {};
bool s_afButtonPrev[MOUSE_BUTTONS] = {};
float s_travel = 0.0f;
GLFWscrollfun s_pfnScrollPrev = nullptr;

void ScrollCallback(GLFWwindow* window, double dx, double dy)
{
    s_dyScroll += dy;
    if (s_pfnScrollPrev) s_pfnScrollPrev(window, dx, dy);
}

bool FWheelBound()
{
    return std::any_of(g_pcinput.keyboard.begin(), g_pcinput.keyboard.end(),
                       [](int key) { return key == st::kMouseWheelUp || key == st::kMouseWheelDown; });
}

bool FWantCapture(GLFWwindow* window)
{
    if (!g_pcinput.fMouseCamera) return false;
    if (glfwGetWindowAttrib(window, GLFW_FOCUSED) != GLFW_TRUE || glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE)
        return false;
    return !FPcMenuMode() && !FHackMouseMode();
}

// (hackmouse.h) the cursor moves freely in the window while the game draws its own
bool FWantFreeHidden(GLFWwindow* window)
{
    return glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE && glfwGetWindowAttrib(window, GLFW_ICONIFIED) != GLFW_TRUE &&
           FHackMouseMode();
}

void SetCapture(GLFWwindow* window, bool fCapture, bool fFreeHidden)
{
    if (fCapture)
    {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, g_pcinput.fMouseRaw ? GLFW_TRUE : GLFW_FALSE);
    }
    else
    {
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetInputMode(window, GLFW_CURSOR, fFreeHidden ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);
    }
    s_fCaptured = fCapture;
    s_fFreeHidden = !fCapture && fFreeHidden;
    s_fHaveLast = false;
}

void UpdateMouse(GLFWwindow* window)
{
    static GLFWwindow* s_pwindowHooked = nullptr;
    if (s_pwindowHooked != window)
    {
        s_pfnScrollPrev = glfwSetScrollCallback(window, ScrollCallback);
        s_pwindowHooked = window;
    }
    s_dxCounts = s_dyCounts = 0.0f;
    s_wheel = int(std::lround(s_dyScroll));
    if (s_wheel == 0 && s_dyScroll != 0.0) s_wheel = s_dyScroll > 0.0 ? 1 : -1;
    s_dyScroll = 0.0;
    const bool fFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
    bool fClicked = false;
    for (int b = 0; b < MOUSE_BUTTONS; ++b)
    {
        s_afButtonPrev[b] = s_afButton[b];
        s_afButton[b] = (fFocused && glfwGetMouseButton(window, b) == GLFW_PRESS) || FTestKeyHeld(st::kMouseBindingBase + b);
        fClicked |= s_afButton[b] && !s_afButtonPrev[b];
    }
    if (fClicked || (s_wheel != 0 && fFocused)) cane::input::tracker().noteMouse();
    const bool fWant = FWantCapture(window);
    const bool fWantFree = !fWant && FWantFreeHidden(window);
    if (fWant != s_fCaptured || fWantFree != s_fFreeHidden) SetCapture(window, fWant, fWantFree);
    s_fCursor = false;
    if (!s_fCaptured && !s_fFreeHidden) return;
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (s_fFreeHidden)
    {
        // (window coordinates to framebuffer pixels: they differ on scaled displays)
        int wx = 0, wy = 0, fbw = 0, fbh = 0;
        glfwGetWindowSize(window, &wx, &wy);
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (wx > 0 && wy > 0)
        {
            s_xCursor = x * double(fbw) / double(wx);
            s_yCursor = y * double(fbh) / double(wy);
            s_fCursor = true;
        }
    }
    if (s_fHaveLast)
    {
        const double dx = x - s_xLast, dy = y - s_yLast;
        // (the jump GLFW can report right after the capture changes is ignored)
        if (std::fabs(dx) < 4000.0 && std::fabs(dy) < 4000.0)
        {
            s_dxCounts = float(dx);
            s_dyCounts = float(dy);
        }
    }
    s_xLast = x;
    s_yLast = y;
    s_fHaveLast = true;
    // a deliberate swipe (not a brush of the trackpad) makes the prompts show keys
    s_travel = s_travel * 0.9f + std::fabs(s_dxCounts) + std::fabs(s_dyCounts);
    if (s_travel > 60.0f)
    {
        cane::input::tracker().noteMouse();
        s_travel = 0.0f;
    }
}

bool FMouseBindingHeld(int binding)
{
    if (binding == st::kMouseWheelUp) return s_wheel > 0;
    if (binding == st::kMouseWheelDown) return s_wheel < 0;
    const int b = binding - st::kMouseBindingBase;
    return b >= 0 && b < MOUSE_BUTTONS && s_afButton[b];
}

bool FKeyHeld(GLFWwindow* window, int key)
{
    return key >= GLFW_KEY_SPACE && key <= GLFW_KEY_LAST && (glfwGetKey(window, key) == GLFW_PRESS || FTestKeyHeld(key));
}

bool FBindingHeld(GLFWwindow* window, int binding)
{
    return st::mouseBinding(binding) ? FMouseBindingHeld(binding) : FKeyHeld(window, binding);
}

// The keys that count as keyboard use for the prompts: every bound key, the camera keys and Escape.
uint64_t GrfKeyboardHeld(GLFWwindow* window)
{
    uint64_t grf = 0;
    for (int a = 0; a < PCA_Max; ++a)
        if (FKeyHeld(window, g_pcinput.keyboard[size_t(a)])) grf |= 1ull << a;
    for (int i = 0; i < 4; ++i)
        if (FKeyHeld(window, g_pcinput.cameraKeys[size_t(i)])) grf |= 1ull << (PCA_Max + i);
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) grf |= 1ull << (PCA_Max + 4);
    if (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS) grf |= 1ull << (PCA_Max + 5);
    return grf;
}

constexpr uint16_t s_abtnAction[PCA_Max] = {
    BTN_Up, BTN_Down, BTN_Left, BTN_Right, BTN_Square, BTN_Circle, BTN_Cross, BTN_Triangle,
    BTN_Start, BTN_Select, BTN_L1, BTN_R1, BTN_L2, BTN_R2, BTN_L3, BTN_R3,
};

float Deadzone(float g) { return std::fabs(g) < 0.15f ? 0.0f : g; }
int s_jidPad = -1;
uint64_t s_grfKeyboardPrev = 0;
}  // namespace

void ReadPcInput(GLFWwindow* window, uint16_t* pgrf, float* pxl, float* pyl, float* pxr, float* pyr, bool* pfPad)
{
    uint16_t grf = 0, grfPad = 0;
    float xl = 0, yl = 0, xr = 0, yr = 0;
    *pfPad = false;
    s_fLive = true;
    UpdateMouse(window);
    const bool fFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
    const bool fMenu = FPcMenuMode();

    // the gamepad: the most active mapped pad (Steam can park an idle virtual pad below the one in use)
    if (!g_pcinput.fDisableUnfocusedPad || fFocused)
    {
        GLFWgamepadstate gp{};
        s_jidPad = cane::input::selectActivePad(s_jidPad, &gp);
        cane::input::tracker().pollGamepads();
        if (s_jidPad >= 0)
        {
            *pfPad = true;
            for (int a = 0; a < PCA_Max; ++a)
            {
                const int b = g_pcinput.gamepad[size_t(a)];
                bool fHeld = false;
                if (b >= 0 && b <= GLFW_GAMEPAD_BUTTON_LAST) fHeld = gp.buttons[b] == GLFW_PRESS;
                else if (b == st::kGamepadLeftTrigger) fHeld = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.5f;
                else if (b == st::kGamepadRightTrigger) fHeld = gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.5f;
                if (fHeld) grf |= s_abtnAction[a];
            }
            grfPad = grf;
            xl = Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
            yl = -Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
            xr = Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
            yr = -Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
        }
    }
    else s_jidPad = -1;

    // the keyboard (and mouse bindings): the movement actions move the left stick, the others are buttons
    // (in a menu with "Mouse in Menus" on, the left / right buttons only confirm / go back: an action bound to
    // them, Square / R3 by default, is not sent there)
    const bool fMenuMouse = fMenu && g_pcinput.fMouseMenus;
    bool afKey[PCA_Max] = {};
    for (int a = 0; a < PCA_Max; ++a)
    {
        const int binding = g_pcinput.keyboard[size_t(a)];
        if (fMenuMouse && (binding == st::kMouseBindingBase + GLFW_MOUSE_BUTTON_LEFT ||
                           binding == st::kMouseBindingBase + GLFW_MOUSE_BUTTON_RIGHT))
            continue;
        afKey[a] = FBindingHeld(window, binding);
    }
    // (hackmouse.h) while the hack's cursor is up, left click fires the ship's guns: its own action is not sent
    if (FHackMouseMode())
        for (int a = 0; a < PCA_Max; ++a)
            if (g_pcinput.keyboard[size_t(a)] == st::kMouseBindingBase + GLFW_MOUSE_BUTTON_LEFT) afKey[a] = false;
    if (afKey[PCA_Left] && !afKey[PCA_Right]) xl = -1.0f;
    if (afKey[PCA_Right] && !afKey[PCA_Left]) xl = 1.0f;
    if (afKey[PCA_Up] && !afKey[PCA_Down]) yl = 1.0f;
    if (afKey[PCA_Down] && !afKey[PCA_Up]) yl = -1.0f;
    for (int a = PCA_Square; a < PCA_Max; ++a)
        if (afKey[a]) grf |= s_abtnAction[a];
    // Escape: a fixed second Start (it pauses, and leaves the menus that Start closes)
    const bool fEscape = (fFocused && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) || FTestKeyHeld(GLFW_KEY_ESCAPE);
    if (fEscape) grf |= BTN_Start;
    // a newly pressed key makes the keyboard the last used device
    const uint64_t grfKeys = GrfKeyboardHeld(window);
    if (grfKeys & ~s_grfKeyboardPrev) cane::input::tracker().noteKeyboard();
    s_grfKeyboardPrev = grfKeys;

    // Enter in a menu confirms (Cross), as PC menus do: an Enter still held from opening the menu (Enter is Start
    // by default) waits for its release, and one that confirmed stays a confirm until released (closing a menu
    // with it must not open the pause menu again)
    static bool s_fEnterLatched = false, s_fEnterConfirms = false;
    const bool fEnter = FKeyHeld(window, GLFW_KEY_ENTER) || FKeyHeld(window, GLFW_KEY_KP_ENTER);
    if (!fEnter) s_fEnterLatched = s_fEnterConfirms = false;
    else if (fMenu && !s_fEnterLatched) s_fEnterConfirms = true;
    else if (!fMenu && !s_fEnterConfirms) s_fEnterLatched = true;
    if (s_fEnterConfirms)
    {
        if (fMenu) grf |= BTN_Cross;
        const int kStart = g_pcinput.keyboard[PCA_Start];
        if ((kStart == GLFW_KEY_ENTER || kStart == GLFW_KEY_KP_ENTER) && !fEscape && !(grfPad & BTN_Start))
            grf &= uint16_t(~BTN_Start);
    }

    // the camera keys: the right stick, or in a menu the d-pad
    bool fCamL = FKeyHeld(window, g_pcinput.cameraKeys[0]), fCamR = FKeyHeld(window, g_pcinput.cameraKeys[1]);
    bool fCamU = FKeyHeld(window, g_pcinput.cameraKeys[2]), fCamD = FKeyHeld(window, g_pcinput.cameraKeys[3]);
    if (fMenu)
    {
        if (fCamL) grf |= BTN_Left;
        if (fCamR) grf |= BTN_Right;
        if (fCamU) grf |= BTN_Up;
        if (fCamD) grf |= BTN_Down;
        fCamL = fCamR = fCamU = fCamD = false;
    }
    if (fCamL && !fCamR) xr = -1.0f;
    if (fCamR && !fCamL) xr = 1.0f;
    if (fCamU && !fCamD) yr = 1.0f;
    if (fCamD && !fCamU) yr = -1.0f;

    // the mouse in menus: the wheel steps (one press every other step, so each notch is an edge), left confirms,
    // right goes back
    static bool s_fWheelPulse = false;
    if (fMenu && g_pcinput.fMouseMenus && !FWheelBound())
    {
        if (s_wheel) s_cwheelMenu = std::clamp(s_cwheelMenu - s_wheel, -4, 4);   // (up: negative steps)
        if (s_fWheelPulse) s_fWheelPulse = false;
        else if (s_cwheelMenu)
        {
            grf |= s_cwheelMenu < 0 ? BTN_Up : BTN_Down;
            s_cwheelMenu += s_cwheelMenu < 0 ? 1 : -1;
            s_fWheelPulse = true;
        }
        if (s_afButton[GLFW_MOUSE_BUTTON_LEFT]) grf |= BTN_Cross;
        if (s_afButton[GLFW_MOUSE_BUTTON_RIGHT]) grf |= BTN_Triangle;
    }
    else s_cwheelMenu = 0, s_fWheelPulse = false;

    if (!TestKeys().empty())
    {
        for (const TESTKEY& k : TestKeys())
            if (g_frameJoy >= k.frame0 && g_frameJoy < k.frame1)
            {
                std::printf("testkeys: frame %d binding %d menu %d grf 0x%04x\n", g_frameJoy, k.binding, int(fMenu), unsigned(grf));
                break;
            }
    }
    *pgrf = grf;
    *pxl = xl;
    *pyl = yl;
    *pxr = xr;
    *pyr = yr;
}

bool FMouseCaptured() { return s_fCaptured; }
bool FPcInputLive() { return s_fLive; }

bool FCursorPicture(float* pxNdc, float* pyNdc, bool* pfLeft)
{
    if (!s_fCursor) return false;
    const DISPLAY& d = g_display;
    // (the picture's place in the framebuffer is centred, so its offset is the same from the top as from the bottom)
    const float x = float((s_xCursor - double(d.presentX)) / double(std::max(1, d.presentWidth))) * 2.0f - 1.0f;
    const float y = 1.0f - float((s_yCursor - double(d.presentY)) / double(std::max(1, d.presentHeight))) * 2.0f;
    // (on the bars around the picture: held at its edge)
    *pxNdc = std::clamp(x, -1.0f, 1.0f);
    *pyNdc = std::clamp(y, -1.0f, 1.0f);
    *pfLeft = s_afButton[GLFW_MOUSE_BUTTON_LEFT];
    return true;
}

// (tests) SLY2N_TESTMOUSE=dx:dy:frame0:frame1: these mouse counts every step of frames [frame0, frame1), as if captured
bool FTestMouse(float* pdx, float* pdy)
{
    static const char* s_psz = std::getenv("SLY2N_TESTMOUSE");
    if (!s_psz) return false;
    extern int g_frameJoy;
    float dx = 0, dy = 0;
    int f0 = 0, f1 = 1 << 30;
    std::sscanf(s_psz, "%f:%f:%d:%d", &dx, &dy, &f0, &f1);
    if (g_frameJoy < f0 || g_frameJoy >= f1) return false;
    *pdx = dx;
    *pdy = dy;
    return true;
}

void MouseOrbitDelta(float* pdradPan, float* pdradTilt)
{
    *pdradPan = *pdradTilt = 0.0f;
    float dxT, dyT;
    if (FTestMouse(&dxT, &dyT))
    {
        *pdradPan = dxT * RAD_PER_COUNT_ORBIT;
        *pdradTilt = -dyT * RAD_PER_COUNT_ORBIT;
        return;
    }
    if (!s_fCaptured) return;
    const float k = RAD_PER_COUNT_ORBIT * float(g_pcinput.mouseSensitivity) / 100.0f;
    *pdradPan = s_dxCounts * k * (g_pcinput.fMouseInvertX ? -1.0f : 1.0f);
    *pdradTilt = -s_dyCounts * k * (g_pcinput.fMouseInvertY ? -1.0f : 1.0f);
}

void MouseAimDelta(float* pdxRight, float* pdyUp)
{
    *pdxRight = *pdyUp = 0.0f;
    float dxT, dyT;
    if (FTestMouse(&dxT, &dyT))
    {
        *pdxRight = dxT * RAD_PER_COUNT_AIM;
        *pdyUp = -dyT * RAD_PER_COUNT_AIM;
        return;
    }
    if (!s_fCaptured) return;
    const float k = RAD_PER_COUNT_AIM * float(g_pcinput.mouseAimSensitivity) / 100.0f;
    *pdxRight = s_dxCounts * k * (g_pcinput.fMouseInvertX ? -1.0f : 1.0f);
    *pdyUp = -s_dyCounts * k * (g_pcinput.fMouseInvertY ? -1.0f : 1.0f);
}

float MouseWheelZoom()
{
    if (!s_fCaptured || !g_pcinput.fMouseWheelZoom || FWheelBound()) return 0.0f;
    return float(s_wheel);
}

const char* PchzMouseBindingName(int binding)
{
    switch (binding - st::kMouseBindingBase)
    {
    case 0: return "Left Mouse";
    case 1: return "Right Mouse";
    case 2: return "Middle Mouse";
    case 3: return "Mouse 4";
    case 4: return "Mouse 5";
    case 5: return "Mouse 6";
    case 6: return "Mouse 7";
    case 7: return "Mouse 8";
    case 8: return "Wheel Up";
    case 9: return "Wheel Down";
    default: return "Mouse";
    }
}

const char* PchzKeyName(int key)
{
    if (st::mouseBinding(key)) return PchzMouseBindingName(key);
    switch (key)
    {
    case GLFW_KEY_UNKNOWN: return "Unbound";
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_KP_ENTER: return "Keypad Enter";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_ESCAPE: return "Escape";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    case GLFW_KEY_UP: return "Up Arrow";
    case GLFW_KEY_DOWN: return "Down Arrow";
    case GLFW_KEY_LEFT: return "Left Arrow";
    case GLFW_KEY_RIGHT: return "Right Arrow";
    case GLFW_KEY_INSERT: return "Insert";
    case GLFW_KEY_DELETE: return "Delete";
    case GLFW_KEY_HOME: return "Home";
    case GLFW_KEY_END: return "End";
    case GLFW_KEY_PAGE_UP: return "Page Up";
    case GLFW_KEY_PAGE_DOWN: return "Page Down";
    case GLFW_KEY_CAPS_LOCK: return "Caps Lock";
    default: break;
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25)
    {
        static char s_ach[8];
        std::snprintf(s_ach, sizeof s_ach, "F%d", key - GLFW_KEY_F1 + 1);
        return s_ach;
    }
    if (const char* psz = glfwGetKeyName(key, 0))
    {
        static char s_ach[16];
        std::snprintf(s_ach, sizeof s_ach, "%s", psz);
        if (s_ach[0] >= 'a' && s_ach[0] <= 'z' && s_ach[1] == '\0') s_ach[0] = char(s_ach[0] - 'a' + 'A');
        return s_ach;
    }
    return "Key";
}

int KeyBindingPressed(GLFWwindow* window)
{
    static bool s_afPrev[GLFW_KEY_LAST + 1] = {};
    static bool s_afMousePrev[MOUSE_BUTTONS] = {};
    int found = -1;
    for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
    {
        const bool f = glfwGetKey(window, k) == GLFW_PRESS;
        if (f && !s_afPrev[k] && found < 0) found = k;
        s_afPrev[k] = f;
    }
    for (int b = 0; b < MOUSE_BUTTONS; ++b)
    {
        const bool f = glfwGetMouseButton(window, b) == GLFW_PRESS;
        if (f && !s_afMousePrev[b] && found < 0) found = st::kMouseBindingBase + b;
        s_afMousePrev[b] = f;
    }
    if (found < 0 && s_wheel > 0) found = st::kMouseWheelUp;
    if (found < 0 && s_wheel < 0) found = st::kMouseWheelDown;
    return found;
}

// (pcinput.h) the debug function keys: nothing unless Debug mode is on (fDebugKeys)
bool FDebugKeyHeld(GLFWwindow* window, int key)
{
    if (!g_pcinput.fDebugKeys) return false;
    return FTestKeyHeld(key) || (window && glfwGetKey(window, key) == GLFW_PRESS);
}
