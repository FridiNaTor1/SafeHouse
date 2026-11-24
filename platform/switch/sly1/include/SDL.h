#pragma once
// Sly 1 on the Switch: the part of SDL2's game controller API Sly1-Release/Input.cpp uses (upstream reads every pad
// through SDL), over the Switch layer's GLFW gamepad (platform/switch/glfw_switch.cpp: every connected controller, the
// handheld Joy-Cons, a Pro Controller or a Joy-Con pair, is one positional pad, jid 0) and its HD rumble. Found first on
// Sly 1's include path in cmake/Switch.cmake; no SDL library is linked on the Switch.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <cstdint>

using Sint16 = int16_t;
using Uint16 = uint16_t;
using Uint32 = uint32_t;
using Uint8 = uint8_t;
using SDL_JoystickID = int32_t;
enum SDL_bool { SDL_FALSE = 0, SDL_TRUE = 1 };

constexpr Uint32 SDL_INIT_JOYSTICK = 0x00000200u;
constexpr Uint32 SDL_INIT_HAPTIC = 0x00001000u;
constexpr Uint32 SDL_INIT_GAMECONTROLLER = 0x00002000u;
constexpr int SDL_ENABLE = 1;
#define SDL_HINT_NO_SIGNAL_HANDLERS "SDL_NO_SIGNAL_HANDLERS"
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"

// SDL's buttons and axes, positional as GLFW's (A: the south button, which Nintendo labels B)
enum SDL_GameControllerButton
{
    SDL_CONTROLLER_BUTTON_A = GLFW_GAMEPAD_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B = GLFW_GAMEPAD_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X = GLFW_GAMEPAD_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y = GLFW_GAMEPAD_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER = GLFW_GAMEPAD_BUTTON_LEFT_BUMPER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER = GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,
    SDL_CONTROLLER_BUTTON_BACK = GLFW_GAMEPAD_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_START = GLFW_GAMEPAD_BUTTON_START,
    SDL_CONTROLLER_BUTTON_GUIDE = GLFW_GAMEPAD_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_LEFTSTICK = GLFW_GAMEPAD_BUTTON_LEFT_THUMB,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK = GLFW_GAMEPAD_BUTTON_RIGHT_THUMB,
    SDL_CONTROLLER_BUTTON_DPAD_UP = GLFW_GAMEPAD_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT = GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN = GLFW_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT = GLFW_GAMEPAD_BUTTON_DPAD_LEFT,
};
enum SDL_GameControllerAxis
{
    SDL_CONTROLLER_AXIS_LEFTX = GLFW_GAMEPAD_AXIS_LEFT_X,
    SDL_CONTROLLER_AXIS_LEFTY = GLFW_GAMEPAD_AXIS_LEFT_Y,
    SDL_CONTROLLER_AXIS_RIGHTX = GLFW_GAMEPAD_AXIS_RIGHT_X,
    SDL_CONTROLLER_AXIS_RIGHTY = GLFW_GAMEPAD_AXIS_RIGHT_Y,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT = GLFW_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT = GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

struct SDL_Joystick { SDL_JoystickID id; };
struct SDL_GameController { SDL_Joystick joystick; };

void CaneSwitchRumble(float low, float high);   // platform/switch/glfw_switch.cpp

namespace CaneSdl
{
inline Uint32 g_initialized = 0;
inline SDL_GameController g_pad{{GLFW_JOYSTICK_1}};
inline bool g_open = false;
inline bool Present() { return glfwJoystickPresent(GLFW_JOYSTICK_1) == GLFW_TRUE; }
inline bool State(GLFWgamepadstate& state) { return glfwGetGamepadState(GLFW_JOYSTICK_1, &state) == GLFW_TRUE; }
}

inline void SDL_SetMainReady() {}
inline SDL_bool SDL_SetHint(const char*, const char*) { return SDL_TRUE; }
inline const char* SDL_GetError() { return ""; }
inline int SDL_InitSubSystem(Uint32 flags) { CaneSdl::g_initialized |= flags; return 0; }
inline void SDL_QuitSubSystem(Uint32 flags) { CaneSdl::g_initialized &= ~flags; }
inline Uint32 SDL_WasInit(Uint32 flags) { return CaneSdl::g_initialized & flags; }
inline void SDL_PumpEvents() {}   // (glfwPollEvents reads the pads)
inline void SDL_GameControllerUpdate() {}
inline int SDL_GameControllerEventState(int state) { return state; }

inline int SDL_NumJoysticks() { return CaneSdl::Present() ? 1 : 0; }
inline SDL_bool SDL_IsGameController(int index) { return index == 0 && CaneSdl::Present() ? SDL_TRUE : SDL_FALSE; }
inline SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int index) { return index == 0 ? GLFW_JOYSTICK_1 : -1; }
inline SDL_GameController* SDL_GameControllerOpen(int index)
{
    if (index != 0 || !CaneSdl::Present()) return nullptr;
    CaneSdl::g_open = true;
    return &CaneSdl::g_pad;
}
inline void SDL_GameControllerClose(SDL_GameController*) { CaneSdl::g_open = false; }
inline SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController* controller)
{
    return controller ? &controller->joystick : nullptr;
}
inline SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick* joystick) { return joystick ? joystick->id : -1; }
inline SDL_bool SDL_GameControllerGetAttached(SDL_GameController* controller)
{
    return controller && CaneSdl::g_open && CaneSdl::Present() ? SDL_TRUE : SDL_FALSE;
}
inline Uint8 SDL_GameControllerGetButton(SDL_GameController* controller, SDL_GameControllerButton button)
{
    GLFWgamepadstate state{};
    return controller && CaneSdl::State(state) && state.buttons[button] == GLFW_PRESS ? 1 : 0;
}
// GLFW's axes are -1..1 (triggers too, at rest -1); SDL's sticks -32768..32767 and triggers 0..32767.
inline Sint16 SDL_GameControllerGetAxis(SDL_GameController* controller, SDL_GameControllerAxis axis)
{
    GLFWgamepadstate state{};
    if (!controller || !CaneSdl::State(state)) return 0;
    float value = state.axes[axis];
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) value = (value + 1.0f) * 0.5f;
    value = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
    return static_cast<Sint16>(value < 0.0f ? value * 32768.0f : value * 32767.0f);
}
inline SDL_bool SDL_GameControllerHasRumble(SDL_GameController* controller) { return controller ? SDL_TRUE : SDL_FALSE; }
// (the Switch layer stops the motors itself when the game stops refreshing them, as SDL's duration does)
inline int SDL_GameControllerRumble(SDL_GameController* controller, Uint16 low, Uint16 high, Uint32)
{
    if (!controller) return -1;
    CaneSwitchRumble(low / 65535.0f, high / 65535.0f);
    return 0;
}
