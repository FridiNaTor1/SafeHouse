#pragma once
// Controller vibration shared by Sly1-Release and Sly2-Release: the PS2 DualShock 2's two actuators as SDL's two
// rumble motors. Input itself stays on GLFW; SDL (CANE_SDL_RUMBLE) only drives the motors, and the pad it drives is
// the GLFW pad in use, matched by its GUID (GLFW reports SDL-format GUIDs).
//
// The DualShock 2 (libpad scePadSetActDirect, the games' actuator align {0, 1}):
//   actuator 0: the small motor, on / off (a byte, any non-zero value is on): the high-frequency buzz;
//   actuator 1: the big motor, 0-255: the low-frequency rumble.
// SDL_GameControllerRumble(low_frequency, high_frequency): low = the big motor (0-255 scaled to 0-0xFFFF),
// high = the small motor (0 or 0xFFFF). Xbox / PlayStation 4 / 5 pads, the Steam Deck and Steam Input's virtual pads
// all have the same two motors under SDL; a pad without rumble, or the keyboard and mouse, is never sent anything.
//
// The motor state is held by the pad until the next write (as the PS2's was), but every write here is given a finite
// duration (kHoldMs) so that a stalled or crashed game never leaves a pad vibrating; the games' schedulers write at
// least every pattern step (<= 0.25 s), which refreshes it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cane::rumble {

struct Motors {
    uint16_t low = 0;   // SDL low_frequency_rumble (the big motor)
    uint16_t high = 0;  // SDL high_frequency_rumble (the small motor)
    constexpr bool operator==(const Motors& o) const { return low == o.low && high == o.high; }
};

// The DualShock 2's actuator bytes (small: on / off, big: 0-255) as SDL motor strengths.
constexpr Motors MotorsFromDualShock(uint8_t smallMotor, uint8_t bigMotor) {
    return Motors{ static_cast<uint16_t>(bigMotor * 257u), static_cast<uint16_t>(smallMotor != 0 ? 0xFFFFu : 0u) };
}
static_assert(MotorsFromDualShock(0, 0) == Motors{ 0, 0 });
static_assert(MotorsFromDualShock(1, 0) == Motors{ 0, 0xFFFF });
static_assert(MotorsFromDualShock(0xFF, 0) == Motors{ 0, 0xFFFF });
static_assert(MotorsFromDualShock(0, 255) == Motors{ 0xFFFF, 0 });
static_assert(MotorsFromDualShock(0, 128) == Motors{ 128 * 257, 0 });
static_assert(MotorsFromDualShock(1, 210) == Motors{ 210 * 257, 0xFFFF });

// A refresh window long enough to bridge every pattern step and a hitch, short enough that a stopped game goes quiet.
inline constexpr uint32_t kHoldMs = 1000;

} // namespace cane::rumble

#ifdef CANE_SDL_RUMBLE
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <algorithm>
#include <vector>

namespace cane::rumble {

// The SDL side: the attached game controllers (opened at startup and when a pad comes into use (prepare), else
// rescanned every two seconds at a write) and the motor writes. startup() costs SDL's subsystem start (half a second
// here): call it at the game's start, never at the first vibration.
// One instance per game (the games are single threaded here: call it from the input / main thread only).
class SdlRumble {
public:
    // SDL's game controller subsystem (reference counted: the movie window / audio may hold SDL too).
    bool startup() {
        if (m_ready) return true;
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
            std::fprintf(stderr, "[rumble] SDL initialization failed: %s\n", SDL_GetError());
            return false;
        }
        m_ready = true;
        refresh(true, 0.0);
        return true;
    }
    void shutdown() {
        for (Device& device : m_devices) close(device);
        m_devices.clear();
        if (m_ready) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC);
        m_ready = false;
    }
    bool ready() const { return m_ready; }
    // The motors of the pad whose (SDL-format) GUID is `guid` (null / no match: the only attached pad, if there is
    // exactly one). now: seconds, for the rescan's pacing. Returns whether a pad took the write.
    bool set(const char* guid, Motors motors, double now, uint32_t ms = kHoldMs) {
        if (!m_ready) return false;
        refresh(false, now);
        SDL_GameController* controller = find(guid);
        if (controller == nullptr || SDL_GameControllerHasRumble(controller) != SDL_TRUE) return false;
        return SDL_GameControllerRumble(controller, motors.low, motors.high, ms) == 0;
    }
    // The pad whose GUID is `guid` came into use (connected, or switched to): opened now, with its motor path warmed by
    // a motors-off write, so that its first real write costs nothing. (The first write used to pay for the open and
    // SDL's HIDAPI rumble set-up: a stutter at the first vibration, R21.) Call it from the input thread.
    void prepare(const char* guid, double now) {
        if (!m_ready) return;
        if (guid != nullptr)
            for (const Device& device : m_devices)
                if (SDL_strcasecmp(device.guid, guid) == 0) return;   // (already open)
        refresh(true, now);
    }
    // Every attached pad's motors off (a pad switch, the game's exit).
    void stopAll() {
        for (Device& device : m_devices)
            if (device.controller != nullptr) SDL_GameControllerRumble(device.controller, 0, 0, 0);
    }

private:
    struct Device {
        SDL_GameController* controller = nullptr;
        SDL_JoystickID instanceId = -1;
        char guid[33] = {};
    };
    static void close(Device& device) {
        if (device.controller != nullptr) {
            SDL_GameControllerRumble(device.controller, 0, 0, 0);
            SDL_GameControllerClose(device.controller);
        }
        device.controller = nullptr;
        device.instanceId = -1;
    }
    void refresh(bool force, double now) {
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        for (auto it = m_devices.begin(); it != m_devices.end();) {
            if (it->controller == nullptr || !SDL_GameControllerGetAttached(it->controller)) {
                close(*it);
                it = m_devices.erase(it);
            } else {
                ++it;
            }
        }
        if (!force && now < m_nextScan) return;
        m_nextScan = now + 2.0;
        const int count = SDL_NumJoysticks();
        for (int index = 0; index < count; ++index) {
            if (!SDL_IsGameController(index)) continue;
            const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
            if (std::any_of(m_devices.begin(), m_devices.end(), [id](const Device& d) { return d.instanceId == id; }))
                continue;
            SDL_GameController* controller = SDL_GameControllerOpen(index);
            if (controller == nullptr) continue;
            SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
            Device device;
            device.controller = controller;
            device.instanceId = SDL_JoystickInstanceID(joystick);
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), device.guid, sizeof(device.guid));
            // (the warm-up: motors off, the driver's rumble path set up now rather than at the first vibration)
            if (SDL_GameControllerHasRumble(controller) == SDL_TRUE) SDL_GameControllerRumble(controller, 0, 0, 0);
            m_devices.push_back(device);
        }
    }
    SDL_GameController* find(const char* guid) const {
        if (guid != nullptr)
            for (const Device& device : m_devices)
                if (SDL_strcasecmp(device.guid, guid) == 0) return device.controller;
        return m_devices.size() == 1 ? m_devices.front().controller : nullptr;
    }

    std::vector<Device> m_devices;
    double m_nextScan = 0.0;
    bool m_ready = false;
};

} // namespace cane::rumble
#endif
