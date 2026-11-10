// GLFW on libnx: the API subset Sly2-Release and the shared platform headers use (controller_family.h,
// gamepad_mappings.h, glfw_platform.h), over the console's own services.
//
// - The window is the screen: its framebuffer is the deko3d swapchain (gl_deko.h), 1280x720 handheld, 1920x1080 docked;
//   a mode change resizes it and reports through the framebuffer-size callback.
// - Every connected controller (the handheld Joy-Cons, a Pro Controller, Joy-Con pairs) reads as one gamepad, jid 0,
//   identified as a Nintendo pad (controller_family.h: Nintendo prompts). Its buttons are positional like GLFW's and
//   SDL's: GLFW_GAMEPAD_BUTTON_A is the south button, which Nintendo labels B (the PS2's Cross).
// - A USB keyboard is the keyboard; the touch screen is the mouse (position and left button).
// - Focus follows the applet (the HOME menu); quitting from the HOME menu closes the window.
#include "runtime.h"
#include "deko/gl_deko.h"

#include <switch.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

struct GLFWmonitor { int dummy; };
struct GLFWcursor { int dummy; };
struct GLFWwindow
{
    bool close = false;
    bool focused = true;
    double x = 0, y = 0;
    int cursorMode = GLFW_CURSOR_NORMAL;
    bool touching = false;
    GLFWframebuffersizefun framebuffer = nullptr;
    GLFWwindowsizefun size = nullptr;
    GLFWcursorposfun cursor = nullptr;
    GLFWmousebuttonfun mouse = nullptr;
    GLFWscrollfun scroll = nullptr;
    GLFWkeyfun key = nullptr;
    GLFWcharfun character = nullptr;
    GLFWcursorenterfun enter = nullptr;
    GLFWwindowfocusfun focus = nullptr;
    GLFWwindowclosefun closeCallback = nullptr;
};

namespace
{
// The graphics backend the window runs on (runtime.h Graphics): the shared one, else a game's own (Sly 1's).
const CaneSwitch::Graphics kSharedGraphics = {CaneDeko::Initialize, CaneDeko::Shutdown, CaneDeko::ResizeOutput, CaneDeko::Present,
                                              CaneDeko::SetSwapInterval, CaneDeko::ProcAddress, CaneDeko::ResetState};
const CaneSwitch::Graphics* g_graphics = &kSharedGraphics;
bool g_graphicsReady = false;
GLFWwindow* g_window = nullptr;
int g_windowUsers = 0;
GLFWmonitor g_monitor{};
GLFWmonitor* g_monitors[] = {&g_monitor};
GLFWvidmode g_mode{1280, 720, 8, 8, 8, 60};
PadState g_pad;
bool g_padReady = false;
HidKeyboardState g_keyboard{};
GLFWerrorfun g_errorCallback = nullptr;
GLFWjoystickfun g_joystickCallback = nullptr;
std::string g_errorDescription;
int g_errorCode = 0;
std::string g_clipboard;
const auto g_epoch = std::chrono::steady_clock::now();
bool g_padConnected = false;
std::string g_padName = "Nintendo Switch Controller";

// GUID in SDL's layout: bus, CRC, vendor (LE), 0000, product (LE), 0000, version: the Nintendo vendor id 057e makes
// controller_family.h pick the Nintendo prompts; the product says which controller.
const char* PadGuid()
{
    const u32 styles = g_padReady ? padGetStyleSet(&g_pad) : 0;
    if (styles & HidNpadStyleTag_NpadFullKey) return "050000007e0500000920000001800000";    // Pro Controller (2009)
    if (styles & HidNpadStyleTag_NpadHandheld) return "060000007e0500000820000000000000";   // the attached Joy-Cons
    return "050000007e0500000e20000001800000";                                                // a Joy-Con pair (200e)
}

void Error(int code, const char* message)
{
    g_errorCode = code;
    g_errorDescription = message;
    if (g_errorCallback) g_errorCallback(code, message);
}

void UpdateMode()
{
    const bool docked = CaneSwitch::Docked();
    const int width = docked ? 1920 : 1280, height = docked ? 1080 : 720;
    if (g_mode.width == width && g_mode.height == height) return;
    g_mode.width = width;
    g_mode.height = height;
    if (g_window && g_graphicsReady)
    {
        g_graphics->resizeOutput(unsigned(width), unsigned(height));
        if (g_window->size) g_window->size(g_window, width, height);
        if (g_window->framebuffer) g_window->framebuffer(g_window, width, height);
    }
}

int KeyUsage(int key)
{
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return 4 + key - GLFW_KEY_A;
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) return 30 + key - GLFW_KEY_1;
    if (key == GLFW_KEY_0) return 39;
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) return 58 + key - GLFW_KEY_F1;
    if (key >= GLFW_KEY_KP_1 && key <= GLFW_KEY_KP_9) return 89 + key - GLFW_KEY_KP_1;
    switch (key)
    {
    case GLFW_KEY_ENTER: return 40; case GLFW_KEY_ESCAPE: return 41; case GLFW_KEY_BACKSPACE: return 42; case GLFW_KEY_TAB: return 43;
    case GLFW_KEY_SPACE: return 44; case GLFW_KEY_MINUS: return 45; case GLFW_KEY_EQUAL: return 46; case GLFW_KEY_LEFT_BRACKET: return 47;
    case GLFW_KEY_RIGHT_BRACKET: return 48; case GLFW_KEY_BACKSLASH: return 49; case GLFW_KEY_SEMICOLON: return 51;
    case GLFW_KEY_APOSTROPHE: return 52; case GLFW_KEY_GRAVE_ACCENT: return 53; case GLFW_KEY_COMMA: return 54; case GLFW_KEY_PERIOD: return 55;
    case GLFW_KEY_SLASH: return 56; case GLFW_KEY_CAPS_LOCK: return 57; case GLFW_KEY_PRINT_SCREEN: return 70; case GLFW_KEY_PAUSE: return 72;
    case GLFW_KEY_INSERT: return 73; case GLFW_KEY_HOME: return 74; case GLFW_KEY_PAGE_UP: return 75; case GLFW_KEY_DELETE: return 76;
    case GLFW_KEY_END: return 77; case GLFW_KEY_PAGE_DOWN: return 78; case GLFW_KEY_RIGHT: return 79; case GLFW_KEY_LEFT: return 80;
    case GLFW_KEY_DOWN: return 81; case GLFW_KEY_UP: return 82; case GLFW_KEY_KP_DIVIDE: return 84; case GLFW_KEY_KP_MULTIPLY: return 85;
    case GLFW_KEY_KP_SUBTRACT: return 86; case GLFW_KEY_KP_ADD: return 87; case GLFW_KEY_KP_ENTER: return 88; case GLFW_KEY_KP_0: return 98;
    case GLFW_KEY_KP_DECIMAL: return 99;
    case GLFW_KEY_LEFT_CONTROL: return 224; case GLFW_KEY_LEFT_SHIFT: return 225; case GLFW_KEY_LEFT_ALT: return 226; case GLFW_KEY_LEFT_SUPER: return 227;
    case GLFW_KEY_RIGHT_CONTROL: return 228; case GLFW_KEY_RIGHT_SHIFT: return 229; case GLFW_KEY_RIGHT_ALT: return 230; case GLFW_KEY_RIGHT_SUPER: return 231;
    default: return -1;
    }
}
bool KeyHeld(const HidKeyboardState& state, int key)
{
    const int usage = KeyUsage(key);
    return usage >= 0 && (state.keys[usage / 64] & (u64(1) << (usage % 64)));
}

void EnsurePad()
{
    if (g_padReady) return;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&g_pad);
    padUpdate(&g_pad);
    hidInitializeTouchScreen();
    hidInitializeKeyboard();
    g_padReady = true;
}

void PollPad()
{
    padUpdate(&g_pad);
    const bool connected = padIsConnected(&g_pad);
    if (connected != g_padConnected)
    {
        g_padConnected = connected;
        if (g_joystickCallback) g_joystickCallback(GLFW_JOYSTICK_1, connected ? GLFW_CONNECTED : GLFW_DISCONNECTED);
    }
}
}  // namespace

// HD rumble on the controllers the game reads (the handheld Joy-Cons and the first player's): low = the big motor.
void CaneSwitchRumble(float low, float high)
{
    if (!g_padReady) return;
    static HidVibrationDeviceHandle s_handles[2][2];
    static bool s_init[2] = {};
    static const HidNpadIdType ids[2] = {HidNpadIdType_Handheld, HidNpadIdType_No1};
    static float s_low = -1, s_high = -1;
    if (low == s_low && high == s_high) return;
    s_low = low;
    s_high = high;
    for (int i = 0; i < 2; ++i)
    {
        const u32 styles = hidGetNpadStyleSet(ids[i]);
        if (!styles) continue;
        const u32 style = (styles & HidNpadStyleTag_NpadHandheld) ? HidNpadStyleTag_NpadHandheld
                        : (styles & HidNpadStyleTag_NpadFullKey) ? HidNpadStyleTag_NpadFullKey : HidNpadStyleTag_NpadJoyDual;
        if (!s_init[i])
        {
            if (R_FAILED(hidInitializeVibrationDevices(s_handles[i], 2, ids[i], HidNpadStyleTag(style)))) continue;
            s_init[i] = true;
        }
        HidVibrationValue values[2];
        for (auto& v : values)
        {
            v.amp_low = std::clamp(low, 0.0f, 1.0f);
            v.freq_low = 160.0f;
            v.amp_high = std::clamp(high, 0.0f, 1.0f);
            v.freq_high = 320.0f;
        }
        hidSendVibrationValues(s_handles[i], values, 2);
    }
}

// The installer's window handed over to the game (the next glfwCreateWindow).
void CaneSwitchReleaseWindow()
{
    if (!g_window) return;
    g_windowUsers = 0;
    if (g_graphics->resetState) g_graphics->resetState();
}

namespace CaneSwitch
{
void ReleaseGraphics()
{
    if (g_window)
    {
        CaneSwitchRumble(0.0f, 0.0f);
        if (g_graphicsReady) g_graphics->shutdown();
        delete g_window;
        g_window = nullptr;
        g_windowUsers = 0;
    }
    else if (g_graphicsReady) g_graphics->shutdown();
    g_graphicsReady = false;
}
void UseGraphics(const Graphics* graphics)
{
    if (!graphics) graphics = &kSharedGraphics;
    if (graphics == g_graphics) return;
    // (one backend owns the screen at a time: the current one closed before the next starts)
    ReleaseGraphics();
    g_graphics = graphics;
}
}  // namespace CaneSwitch

extern "C"
{
int glfwInit(void) { EnsurePad(); return GLFW_TRUE; }
void glfwInitHint(int, int) {}
void glfwGetVersion(int* major, int* minor, int* rev) { if (major) *major = 3; if (minor) *minor = 3; if (rev) *rev = 3; }
const char* glfwGetVersionString(void) { return "3.3.3 libnx"; }
void glfwTerminate(void) {}
void glfwWindowHint(int, int) {}
void glfwWindowHintString(int, const char*) {}
void glfwDefaultWindowHints(void) {}

GLFWwindow* glfwCreateWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*)
{
    EnsurePad();
    // (one window: the installer's and then the game's are the same screen and the same deko3d device)
    if (g_window) { ++g_windowUsers; g_window->close = false; return g_window; }
    UpdateMode();
    if (!g_graphics->initialize(unsigned(g_mode.width), unsigned(g_mode.height)))
    {
        Error(GLFW_PLATFORM_ERROR, "deko3d could not start");
        return nullptr;
    }
    g_graphicsReady = true;
    // glad's function pointers, through which every GL call goes, loaded with the window from its backend: the
    // installer draws (ImGui) before the game's GL init loads them again, and an unloaded pointer is a jump to address 0
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(g_graphics->procAddress)))
    {
        g_graphics->shutdown();
        g_graphicsReady = false;
        Error(GLFW_PLATFORM_ERROR, "the GL entry points could not be loaded");
        return nullptr;
    }
    g_window = new GLFWwindow;
    g_windowUsers = 1;
    return g_window;
}
void glfwDestroyWindow(GLFWwindow* w)
{
    if (!w || w != g_window) return;
    // (the installer's window: the game takes it over with its state reset; the last user shuts the backend down)
    if (--g_windowUsers > 0)
    {
        if (g_graphics->resetState) g_graphics->resetState();
        return;
    }
    CaneSwitch::ReleaseGraphics();
}
void glfwMakeContextCurrent(GLFWwindow*) {}
GLFWwindow* glfwGetCurrentContext(void) { return g_window; }
GLFWglproc glfwGetProcAddress(const char* name) { return reinterpret_cast<GLFWglproc>(g_graphics->procAddress(name)); }
// (an extension is there when the game's graphics have its entry point: Sly 1 asks for GL_ARB_buffer_storage before
// mapping its record streams, Sly1-Release/gl.cpp FBufferStorageAvailable)
int glfwExtensionSupported(const char* name)
{
    if (name && !std::strcmp(name, "GL_ARB_buffer_storage"))
        return g_graphics->procAddress("glBufferStorage") ? GLFW_TRUE : GLFW_FALSE;
    return GLFW_FALSE;
}
void glfwSwapInterval(int interval) { g_graphics->setSwapInterval(interval); }
void glfwSwapBuffers(GLFWwindow* w)
{
    if (w) g_graphics->present();
}
int glfwWindowShouldClose(GLFWwindow* w) { return !w || w->close; }
void glfwSetWindowShouldClose(GLFWwindow* w, int value) { if (w) w->close = value != 0; }

void glfwPollEvents(void)
{
    if (!appletMainLoop())
    {
        if (g_window) g_window->close = true;
        return;
    }
    EnsurePad();
    UpdateMode();
    PollPad();
    if (!g_window) return;
    const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (focused != g_window->focused)
    {
        if (!focused) CaneSwitchRumble(0.0f, 0.0f);
        g_window->focused = focused;
        if (g_window->focus) g_window->focus(g_window, focused);
    }
    const HidKeyboardState previous = g_keyboard;
    if (!hidGetKeyboardStates(&g_keyboard, 1)) g_keyboard = {};
    if (g_window->key || g_window->character)
        for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key)
        {
            const bool was = KeyHeld(previous, key), is = KeyHeld(g_keyboard, key);
            if (was == is) continue;
            if (g_window->key) g_window->key(g_window, key, KeyUsage(key), is ? GLFW_PRESS : GLFW_RELEASE, 0);
            if (is && g_window->character && key >= GLFW_KEY_SPACE && key <= GLFW_KEY_GRAVE_ACCENT)
            {
                unsigned c = unsigned(key);
                if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z && !KeyHeld(g_keyboard, GLFW_KEY_LEFT_SHIFT) && !KeyHeld(g_keyboard, GLFW_KEY_RIGHT_SHIFT)) c += 32;
                g_window->character(g_window, c);
            }
        }
    HidTouchScreenState touch{};
    const bool touching = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
    if (touching)
    {
        // (the touch screen is 1280x720 in either mode: scaled to the framebuffer)
        g_window->x = touch.touches[0].x * (g_mode.width / 1280.0);
        g_window->y = touch.touches[0].y * (g_mode.height / 720.0);
        if (g_window->cursor) g_window->cursor(g_window, g_window->x, g_window->y);
    }
    if (touching != g_window->touching && g_window->mouse)
        g_window->mouse(g_window, GLFW_MOUSE_BUTTON_LEFT, touching ? GLFW_PRESS : GLFW_RELEASE, 0);
    g_window->touching = touching;
}
void glfwWaitEvents(void) { glfwPollEvents(); }
void glfwWaitEventsTimeout(double seconds)
{
    if (std::isfinite(seconds) && seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(std::min(seconds, 0.1)));
    glfwPollEvents();
}
void glfwPostEmptyEvent(void) {}

int glfwGetKey(GLFWwindow* w, int key) { return w && w->focused && KeyHeld(g_keyboard, key) ? GLFW_PRESS : GLFW_RELEASE; }
const char* glfwGetKeyName(int key, int)
{
    static char name[2];
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) { name[0] = char('a' + key - GLFW_KEY_A); name[1] = 0; return name; }
    if (key >= GLFW_KEY_SPACE && key <= GLFW_KEY_GRAVE_ACCENT) { name[0] = char(key); name[1] = 0; return name; }
    return nullptr;
}
int glfwGetKeyScancode(int key) { return KeyUsage(key); }
int glfwGetMouseButton(GLFWwindow* w, int button) { return w && button == GLFW_MOUSE_BUTTON_LEFT && w->touching ? GLFW_PRESS : GLFW_RELEASE; }
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) { if (x) *x = w ? w->x : 0; if (y) *y = w ? w->y : 0; }
void glfwSetCursorPos(GLFWwindow* w, double x, double y) { if (w) { w->x = x; w->y = y; } }
int glfwGetInputMode(GLFWwindow* w, int mode) { return w && mode == GLFW_CURSOR ? w->cursorMode : GLFW_FALSE; }
void glfwSetInputMode(GLFWwindow* w, int mode, int value) { if (w && mode == GLFW_CURSOR) w->cursorMode = value; }
int glfwRawMouseMotionSupported(void) { return GLFW_FALSE; }
GLFWcursor* glfwCreateStandardCursor(int) { return new GLFWcursor{}; }
GLFWcursor* glfwCreateCursor(const GLFWimage*, int, int) { return new GLFWcursor{}; }
void glfwDestroyCursor(GLFWcursor* c) { delete c; }
void glfwSetCursor(GLFWwindow*, GLFWcursor*) {}

int glfwJoystickPresent(int jid) { return jid == GLFW_JOYSTICK_1 && g_padReady && padIsConnected(&g_pad) ? GLFW_TRUE : GLFW_FALSE; }
int glfwJoystickIsGamepad(int jid) { return glfwJoystickPresent(jid); }
const char* glfwGetJoystickName(int jid) { return glfwJoystickPresent(jid) ? g_padName.c_str() : nullptr; }
const char* glfwGetJoystickGUID(int jid) { return glfwJoystickPresent(jid) ? PadGuid() : nullptr; }
const char* glfwGetGamepadName(int jid) { return glfwJoystickPresent(jid) ? "Nintendo Switch Controller" : nullptr; }
void glfwSetJoystickUserPointer(int, void*) {}
void* glfwGetJoystickUserPointer(int) { return nullptr; }
GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun callback) { return std::exchange(g_joystickCallback, callback); }
int glfwUpdateGamepadMappings(const char*) { return GLFW_TRUE; }
int glfwGetGamepadState(int jid, GLFWgamepadstate* state)
{
    if (!state || !glfwJoystickPresent(jid)) return GLFW_FALSE;
    std::memset(state, 0, sizeof(*state));
    const bool live = !g_window || g_window->focused;
    const u64 keys = live ? padGetButtons(&g_pad) : 0;
    // (positional: GLFW's A..Y are south, east, west, north)
    static const u64 kMap[GLFW_GAMEPAD_BUTTON_LAST + 1] = {
        HidNpadButton_B, HidNpadButton_A, HidNpadButton_Y, HidNpadButton_X,
        HidNpadButton_L, HidNpadButton_R, HidNpadButton_Minus, HidNpadButton_Plus, 0,
        HidNpadButton_StickL, HidNpadButton_StickR,
        HidNpadButton_Up, HidNpadButton_Right, HidNpadButton_Down, HidNpadButton_Left};
    for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; ++i) state->buttons[i] = (kMap[i] & keys) ? GLFW_PRESS : GLFW_RELEASE;
    if (live)
    {
        const HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
        state->axes[GLFW_GAMEPAD_AXIS_LEFT_X] = std::clamp(l.x / 32767.0f, -1.0f, 1.0f);
        state->axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = std::clamp(-l.y / 32767.0f, -1.0f, 1.0f);
        state->axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = std::clamp(r.x / 32767.0f, -1.0f, 1.0f);
        state->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] = std::clamp(-r.y / 32767.0f, -1.0f, 1.0f);
    }
    state->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = (keys & HidNpadButton_ZL) ? 1.0f : -1.0f;
    state->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = (keys & HidNpadButton_ZR) ? 1.0f : -1.0f;
    return GLFW_TRUE;
}
const float* glfwGetJoystickAxes(int jid, int* count)
{
    static GLFWgamepadstate state;
    const bool ok = glfwGetGamepadState(jid, &state) == GLFW_TRUE;
    if (count) *count = ok ? GLFW_GAMEPAD_AXIS_LAST + 1 : 0;
    return ok ? state.axes : nullptr;
}
const unsigned char* glfwGetJoystickButtons(int jid, int* count)
{
    static GLFWgamepadstate state;
    const bool ok = glfwGetGamepadState(jid, &state) == GLFW_TRUE;
    if (count) *count = ok ? GLFW_GAMEPAD_BUTTON_LAST + 1 : 0;
    return ok ? state.buttons : nullptr;
}
const unsigned char* glfwGetJoystickHats(int jid, int* count)
{
    static unsigned char hat;
    GLFWgamepadstate state;
    const bool ok = glfwGetGamepadState(jid, &state) == GLFW_TRUE;
    hat = GLFW_HAT_CENTERED;
    if (ok)
    {
        if (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP]) hat |= GLFW_HAT_UP;
        if (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]) hat |= GLFW_HAT_RIGHT;
        if (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN]) hat |= GLFW_HAT_DOWN;
        if (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT]) hat |= GLFW_HAT_LEFT;
    }
    if (count) *count = ok ? 1 : 0;
    return ok ? &hat : nullptr;
}

double glfwGetTime(void) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count(); }
void glfwSetTime(double) {}
uint64_t glfwGetTimerValue(void) { return armGetSystemTick(); }
uint64_t glfwGetTimerFrequency(void) { return armGetSystemTickFreq(); }
int glfwGetError(const char** description) { if (description) *description = g_errorDescription.c_str(); return std::exchange(g_errorCode, 0); }
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun callback) { return std::exchange(g_errorCallback, callback); }

void glfwGetFramebufferSize(GLFWwindow*, int* w, int* h) { if (w) *w = g_mode.width; if (h) *h = g_mode.height; }
void glfwGetWindowSize(GLFWwindow* win, int* w, int* h) { glfwGetFramebufferSize(win, w, h); }
void glfwSetWindowSize(GLFWwindow*, int, int) {}
void glfwGetWindowPos(GLFWwindow*, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; }
void glfwSetWindowPos(GLFWwindow*, int, int) {}
void glfwGetWindowFrameSize(GLFWwindow*, int* l, int* t, int* r, int* b) { if (l) *l = 0; if (t) *t = 0; if (r) *r = 0; if (b) *b = 0; }
void glfwGetWindowContentScale(GLFWwindow*, float* x, float* y) { if (x) *x = 1; if (y) *y = 1; }
void glfwSetWindowTitle(GLFWwindow*, const char*) {}
void glfwSetWindowIcon(GLFWwindow*, int, const GLFWimage*) {}
void glfwSetWindowSizeLimits(GLFWwindow*, int, int, int, int) {}
void glfwSetWindowAspectRatio(GLFWwindow*, int, int) {}
void glfwIconifyWindow(GLFWwindow*) {}
void glfwRestoreWindow(GLFWwindow*) {}
void glfwMaximizeWindow(GLFWwindow*) {}
void glfwShowWindow(GLFWwindow*) {}
void glfwHideWindow(GLFWwindow*) {}
void glfwFocusWindow(GLFWwindow*) {}
void glfwRequestWindowAttention(GLFWwindow*) {}
int glfwGetWindowAttrib(GLFWwindow* w, int attrib)
{
    switch (attrib)
    {
    case GLFW_FOCUSED: return w && w->focused ? GLFW_TRUE : GLFW_FALSE;
    case GLFW_ICONIFIED: case GLFW_DECORATED: case GLFW_RESIZABLE: case GLFW_FLOATING: case GLFW_TRANSPARENT_FRAMEBUFFER: return GLFW_FALSE;
    case GLFW_VISIBLE: case GLFW_HOVERED: return GLFW_TRUE;
    default: return GLFW_FALSE;
    }
}
void glfwSetWindowAttrib(GLFWwindow*, int, int) {}
void glfwSetWindowUserPointer(GLFWwindow*, void*) {}
void* glfwGetWindowUserPointer(GLFWwindow*) { return nullptr; }
GLFWmonitor* glfwGetWindowMonitor(GLFWwindow*) { return &g_monitor; }
void glfwSetWindowMonitor(GLFWwindow* w, GLFWmonitor*, int, int, int, int, int)
{
    if (w && w->framebuffer) w->framebuffer(w, g_mode.width, g_mode.height);
}
GLFWmonitor* glfwGetPrimaryMonitor(void) { return &g_monitor; }
GLFWmonitor** glfwGetMonitors(int* count) { if (count) *count = 1; return g_monitors; }
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*) { return &g_mode; }
const GLFWvidmode* glfwGetVideoModes(GLFWmonitor*, int* count) { if (count) *count = 1; return &g_mode; }
void glfwGetMonitorPos(GLFWmonitor*, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; }
void glfwGetMonitorWorkarea(GLFWmonitor*, int* x, int* y, int* w, int* h) { if (x) *x = 0; if (y) *y = 0; if (w) *w = g_mode.width; if (h) *h = g_mode.height; }
void glfwGetMonitorPhysicalSize(GLFWmonitor*, int* w, int* h) { if (w) *w = 155; if (h) *h = 87; }
void glfwGetMonitorContentScale(GLFWmonitor*, float* x, float* y) { if (x) *x = 1; if (y) *y = 1; }
const char* glfwGetMonitorName(GLFWmonitor*) { return "Nintendo Switch"; }
GLFWmonitorfun glfwSetMonitorCallback(GLFWmonitorfun callback) { static GLFWmonitorfun s_cb = nullptr; return std::exchange(s_cb, callback); }
// (the window never moves nor iconifies: kept, never called)
GLFWwindowposfun glfwSetWindowPosCallback(GLFWwindow*, GLFWwindowposfun callback) { static GLFWwindowposfun s_cb = nullptr; return std::exchange(s_cb, callback); }
GLFWwindowiconifyfun glfwSetWindowIconifyCallback(GLFWwindow*, GLFWwindowiconifyfun callback) { static GLFWwindowiconifyfun s_cb = nullptr; return std::exchange(s_cb, callback); }
const char* glfwGetClipboardString(GLFWwindow*) { return g_clipboard.c_str(); }
void glfwSetClipboardString(GLFWwindow*, const char* text) { g_clipboard = text ? text : ""; }

#define CANE_CALLBACK(fn, type, field) type fn(GLFWwindow* w, type cb) { return w ? std::exchange(w->field, cb) : nullptr; }
CANE_CALLBACK(glfwSetFramebufferSizeCallback, GLFWframebuffersizefun, framebuffer)
CANE_CALLBACK(glfwSetWindowSizeCallback, GLFWwindowsizefun, size)
CANE_CALLBACK(glfwSetCursorPosCallback, GLFWcursorposfun, cursor)
CANE_CALLBACK(glfwSetMouseButtonCallback, GLFWmousebuttonfun, mouse)
CANE_CALLBACK(glfwSetScrollCallback, GLFWscrollfun, scroll)
CANE_CALLBACK(glfwSetKeyCallback, GLFWkeyfun, key)
CANE_CALLBACK(glfwSetCharCallback, GLFWcharfun, character)
CANE_CALLBACK(glfwSetCursorEnterCallback, GLFWcursorenterfun, enter)
CANE_CALLBACK(glfwSetWindowFocusCallback, GLFWwindowfocusfun, focus)
CANE_CALLBACK(glfwSetWindowCloseCallback, GLFWwindowclosefun, closeCallback)
#undef CANE_CALLBACK
}
