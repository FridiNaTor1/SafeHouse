#pragma once
// The debug menus' Free Camera page and game-time keys, shared by Sly 1, Sly 2 and Sly 3 (each game's debugmenu.cpp;
// with Debug mode only, like the rest of the menu): the freeze camera's modes and its follow aim, the speed stops, the
// keys and the short note shown when one is pressed.
//
// The keys (Debug mode, the menu closed; the same in the three games):
//   F1         Free Camera on / off (the fly camera, as before)
//   Shift+F1   Freeze Camera: Off -> Fixed -> Follow -> Off
//   F8         Freeze Time on / off (the world stands still, the picture keeps drawing; the free camera still flies)
//   Shift+F8   Faster (the speed's next stop: 1x -> 1.5x -> 2x -> 4x)
//   Ctrl+F8    Slower (1x -> 0.5x -> 0.25x -> 0.1x)
// (F2..F7, F9..F11 are the games' other debug keys; F12 is Steam's screenshot key.)
//
// Freeze Camera: the picture is drawn from the view the camera has when it is chosen (the game's camera, or the free
// camera's when that is on: the free camera turns off and the player gets the controls back). Fixed keeps that place
// and direction; Follow keeps the place and turns (pans / tilts, smoothly) to keep the current player in view. The
// game's own camera keeps running behind it, so cutscenes, conversations and the binocucom show their own cameras and
// the frozen view comes back after them; a level change turns it off.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

namespace cane::debugmenu {

// ---- the keys' names, for the menu rows
inline constexpr const char* kKeyFreeCam = "F1";
inline constexpr const char* kKeyFreezeCam = "Shift+F1";
inline constexpr const char* kKeyFreezeTime = "F8";
inline constexpr const char* kKeyFaster = "Shift+F8";
inline constexpr const char* kKeySlower = "Ctrl+F8";

// What a press of F1 or F8 does with the modifiers held (Ctrl+F1: nothing).
enum class TimeCamKey { None, FreeCam, FreezeCam, FreezeTime, Faster, Slower };
inline TimeCamKey timeCamKey(bool fF1Pressed, bool fF8Pressed, bool fShift, bool fCtrl) {
    if (fF1Pressed && !fCtrl) return fShift ? TimeCamKey::FreezeCam : TimeCamKey::FreeCam;
    if (fF8Pressed) return fCtrl ? TimeCamKey::Slower : fShift ? TimeCamKey::Faster : TimeCamKey::FreezeTime;
    return TimeCamKey::None;
}

// ---- the freeze camera
enum class Freeze { Off, Fixed, Follow };
inline Freeze freezeNext(Freeze f) { return f == Freeze::Off ? Freeze::Fixed : f == Freeze::Fixed ? Freeze::Follow : Freeze::Off; }
inline const char* freezeName(Freeze f) { return f == Freeze::Fixed ? "Fixed" : f == Freeze::Follow ? "Follow" : "Off"; }

// The follow aim: yaw / pitch in degrees (yaw around +z, 0 along +x, 90 along +y; pitch up positive), turned toward
// the target at kFollowRate (the remaining angle shrinks by e^-rate each second: about 1.2 s to settle, no snap).
inline constexpr float kFollowRate = 4.0f;
struct FollowAim {
    float yaw = 0.0f, pitch = 0.0f;
    void reset(float yawNow, float pitchNow) { yaw = yawNow, pitch = pitchNow; }
    // from: the camera's place; to: the point to keep in view; dt: seconds since the last call (real time)
    void step(float fromX, float fromY, float fromZ, float toX, float toY, float toZ, float dt) {
        const float dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
        const float dxy = std::sqrt(dx * dx + dy * dy);
        if (dxy + std::fabs(dz) < 1e-3f) return;   // (on the target: keep the direction)
        const float kDeg = 57.29577951f;
        const float yawT = std::atan2(dy, dx) * kDeg, pitchT = std::atan2(dz, dxy) * kDeg;
        float dyaw = std::fmod(yawT - yaw, 360.0f);
        if (dyaw > 180.0f) dyaw -= 360.0f;
        if (dyaw < -180.0f) dyaw += 360.0f;
        const float u = 1.0f - std::exp(-kFollowRate * std::clamp(dt, 0.0f, 0.25f));
        yaw += dyaw * u;
        if (yaw > 180.0f) yaw -= 360.0f;
        if (yaw < -180.0f) yaw += 360.0f;
        pitch = std::clamp(pitch + (pitchT - pitch) * u, -89.0f, 89.0f);
    }
};

// ---- the game's speed (the menus' Speed page and the F8 keys)
inline constexpr float kSpeedStops[] = {0.1f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f};
inline float speedFaster(float r) {
    for (float g : kSpeedStops)
        if (g > r * 1.001f) return g;
    return kSpeedStops[std::size(kSpeedStops) - 1];
}
inline float speedSlower(float r) {
    for (size_t i = std::size(kSpeedStops); i-- > 0;)
        if (kSpeedStops[i] < r * 0.999f) return kSpeedStops[i];
    return kSpeedStops[0];
}
inline std::string strSpeed(float r) {
    char ach[16];
    std::snprintf(ach, sizeof ach, "%gx", r);
    return ach;
}
// The small corner note while the time is not normal ("" at 1x)
inline std::string strTimeIndicator(float r, bool fFrozen) {
    if (fFrozen) return "Time frozen";
    if (std::fabs(r - 1.0f) < 1e-4f) return "";
    return "Speed " + strSpeed(r);
}

// ---- a short note on screen after a key (about a second; also printed, "<prefix>: <text>")
struct Note {
    std::string text;
    float t = 0.0f;
    void show(const std::string& s, const char* prefix = "debug keys") {
        text = s;
        t = 1.3f;
        std::printf("%s: %s\n", prefix, s.c_str());
        std::fflush(stdout);
    }
    void tick(float dt) { t = std::max(0.0f, t - dt); }
    bool shown() const { return t > 0.0f && !text.empty(); }
    float alpha() const { return std::clamp(t / 0.3f, 0.0f, 1.0f); }   // (fades out over its last 0.3 s)
};

}  // namespace cane::debugmenu
