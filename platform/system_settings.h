#pragma once
// Saves/system_settings.dat: the games' global options (Sly1-Release and
// Sly2-Release each keep their own file), read and written by the games and by
// the launcher's Settings page through this one serializer (Sly1-Release's
// VIDEOSAVESETTINGS is SystemSettings).
//
// File: 20-byte header {char magic[8]="PCANESYS"; u32 version; u32 payloadSize;
// u32 fnv1a32(payload)} followed by the raw little-endian payload. Every
// historic version (1-18) still loads; a write produces kVersion. A
// file with a higher version (for example from a newer or forked build) is
// reported as Newer and never overwritten.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include "platform.h"
namespace cane::settings {
namespace fs = std::filesystem;
inline constexpr char kMagic[8] = { 'P', 'C', 'A', 'N', 'E', 'S', 'Y', 'S' };
inline constexpr uint32_t kVersion = 18;
// Action order: Up, Down, Left, Right, Square, Circle, Cross, Triangle, Start,
// Select, L1, R1, L2, R2, L3, R3 (Input.h JOY_BUTTON).
inline constexpr int kBindingCount = 16;
inline constexpr int kSaveSlotCount = 6;
// GLFW values, repeated so this header needs no GLFW include.
inline constexpr int kKeyUnknown = -1, kKeySpace = 32, kKeyEscape = 256, kKeyLast = 348;
// Keyboard bindings may also name a mouse button (v11): kMouseBindingBase +
// GLFW mouse button 0-7, then the wheel. Older builds never read v11 files.
inline constexpr int kMouseBindingBase = 2000, kMouseButtonCount = 8;
inline constexpr int kMouseWheelUp = kMouseBindingBase + kMouseButtonCount, kMouseWheelDown = kMouseWheelUp + 1;
inline constexpr bool mouseBinding(int binding) { return binding >= kMouseBindingBase && binding <= kMouseWheelDown; }
// Keyboard camera (the right stick): Left, Right, Up, Down. GLFW arrow keys.
inline constexpr int kCameraKeyCount = 4;
inline constexpr int kDefaultCameraKeys[kCameraKeyCount] = { 263 /*Left*/, 262 /*Right*/, 265 /*Up*/, 264 /*Down*/ };
inline constexpr int kGamepadButtonLast = 14;
inline constexpr int kGamepadLeftTrigger = 1000, kGamepadRightTrigger = 1001;
inline constexpr int kDefaultKeyboardBindings[kBindingCount] = {
    87 /*W*/, 83 /*S*/, 65 /*A*/, 68 /*D*/, 74 /*J*/, 76 /*L*/, 75 /*K*/, 73 /*I*/,
    257 /*Enter*/, 259 /*Backspace*/, 81 /*Q*/, 69 /*E*/, 90 /*Z*/, 67 /*C*/,
    kKeyUnknown, kKeyUnknown };
// Sly 2's (Sly2-Release) own keyboard / mouse layout (user, 2026-10-27): Square (attack) = left mouse, Circle = E,
// Cross (jump) = Space, Triangle = Q, R1 = Left Shift, the gadgets L1 / L2 / R2 = 1 / 2 / 3, L3 (the counters) = Tab,
// R3 (the binocucom) = right mouse; movement, Start / Select and the camera keys as Sly 1's. Sly 1 keeps the table
// above (its original PC layout).
inline constexpr int kSly2DefaultKeyboardBindings[kBindingCount] = {
    87 /*W*/, 83 /*S*/, 65 /*A*/, 68 /*D*/, 2000 /*left mouse*/, 69 /*E*/, 32 /*Space*/, 81 /*Q*/,
    257 /*Enter*/, 259 /*Backspace*/, 49 /*1*/, 340 /*Left Shift*/, 50 /*2*/, 51 /*3*/,
    258 /*Tab*/, 2001 /*right mouse*/ };
inline constexpr int kDefaultGamepadBindings[kBindingCount] = {
    11 /*DPAD_UP*/, 13 /*DPAD_DOWN*/, 14 /*DPAD_LEFT*/, 12 /*DPAD_RIGHT*/,
    2 /*X*/, 1 /*B*/, 0 /*A*/, 3 /*Y*/, 7 /*START*/, 6 /*BACK*/,
    4 /*LEFT_BUMPER*/, 5 /*RIGHT_BUMPER*/, kGamepadLeftTrigger, kGamepadRightTrigger,
    9 /*LEFT_THUMB*/, 10 /*RIGHT_THUMB*/ };
static_assert(kSly2DefaultKeyboardBindings[4] == kMouseBindingBase + 0 && kSly2DefaultKeyboardBindings[15] == kMouseBindingBase + 1);
// Field value ranges (gl.h AspectMode/WindowMode/GuiStyle).
enum FogType : int { Fog_Off = 0, Fog_PS2 = 1, Fog_PS3 = 2 };
enum WindowModeValue : int { Window_Windowed = 0, Window_Borderless = 1, Window_Fullscreen = 2 };
enum AspectValue : int { Aspect_Fit = 0, Aspect_4_3 = 1, Aspect_16_9 = 2, Aspect_16_10 = 3,
                         Aspect_PS2_4_3 = 4, Aspect_PS2_16_9 = 5 };
enum GuiStyleValue : int { Gui_PS2 = 0, Gui_Modern = 1 };
// == cane::input::PromptStyle (v10 promptStyle; the launcher also keeps its own
// launcher.json "prompt_style" for its screens).
enum PromptStyleValue : int { Prompt_Auto = 0, Prompt_PlayStation = 1, Prompt_Xbox = 2,
                              Prompt_Nintendo = 3, Prompt_SteamDeck = 4, Prompt_SteamController = 5,
                              Prompt_PS4 = 6, Prompt_PS5 = 7, Prompt_SteamController2026 = 8 };
inline constexpr int kInternalResolutions[] = { 0, 360, 480, 720, 1080, 1440, 2160 };
// v11 video options (ThiefRush's layout; Sly2-Release does not read them yet).
enum UpscalerValue : int { Upscale_Bilinear = 0, Upscale_Fsr1 = 1 };
inline constexpr int kAnisotropyLevels[] = { 1, 2, 4, 8, 16 }; // 1 = off
inline constexpr int kFovPercentMax = 50, kFovPercentStep = 5;

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t payloadSize;
    uint32_t checksum;
};

// Frozen historic payloads. Never edit these.
namespace legacy {
struct V1 {
    int fogType, msaaEnabled, msaaSamples, frameRate, internalResolutionHeight;
    float drawDistance;
    int windowMode, vsyncEnabled, aspectMode;
};
struct V2 {
    int fogType, msaaEnabled, msaaSamples, frameRate, internalResolutionHeight;
    float drawDistance;
    int windowMode, vsyncEnabled, aspectMode;
    float guiScale;
};
struct V3 {
    V2 video;
    int keyboardBindings[kBindingCount];
};
struct V4 {
    int fogType, msaaEnabled, msaaSamples, frameRate, internalResolutionHeight;
    float drawDistance;
    int windowMode, vsyncEnabled, aspectMode;
    float guiScale;
    int keyboardBindings[kBindingCount];
    int gamepadBindings[kBindingCount];
};
struct V5 {
    int fogType, msaaEnabled, msaaSamples, frameRate, internalResolutionHeight;
    float drawDistance;
    int windowMode, vsyncEnabled, aspectMode;
    float guiScale;
    int guiStyle;
    int keyboardBindings[kBindingCount];
    int gamepadBindings[kBindingCount];
};
struct V6 {
    V5 base;
    int disableControllerInputWhenUnfocused;
};
struct V7 {
    V6 base;
    int lastSaveSlot;
};
struct V8 {
    V7 base;
    int stereoEnabled;
    float musicVolume, soundEffectsVolume, dialogueVolume;
};
struct V9 {
    V8 base;
    int streamerMode;
};
struct V10 {
    V9 base;
    int alwaysOriginalPsPrompts, promptStyle;
};
// v11 == ThiefRush's v11 (the fields up to muteWhenUnfocused).
struct V11 {
    V10 base;
    int fovPercent, anisotropy, showFps, upscaler, upscalerSharpness;
    int cameraSensitivity, mouseCamera, mouseSensitivity, mouseAimSensitivity;
    int mouseInvertX, mouseInvertY, mouseRawInput, mouseWheelZoom, mouseMenus;
    int cameraKeyBindings[kCameraKeyCount];
    int muteWhenUnfocused;
};
// v12 (ProjectCane): + instantSkipCutscenes.
struct V12 {
    V11 base;
    int instantSkipCutscenes;
};
// v13 (ProjectCane): + hackMouseAim.
struct V13 {
    V12 base;
    int hackMouseAim;
};
// v14 (ProjectCane): + debugKeys.
struct V14 {
    V13 base;
    int debugKeys;
};
// v15: + controllerVibration.
struct V15 {
    V14 base;
    int controllerVibration;
};
// v16: + longShadows.
struct V16 {
    V15 base;
    int longShadows;
};
// v17: + Sly 2 camera / aim inversion.
struct V17 {
    V16 base;
    int padInvertCameraX, padInvertCameraY;
    int mouseInvertBinocX, mouseInvertBinocY;
    int mouseInvertTurretX, mouseInvertTurretY;
};
} // namespace legacy

// Current (v18) payload (Sly1-Release's VIDEOSAVESETTINGS); its first 196 bytes are the v8 layout, v9..v11 == ThiefRush's.
// Append new fields only at the end, bump kVersion and add a legacy::V<n> snapshot plus a load branch.
struct SystemSettings {
    int fogType;                  // 0 off, 1 PS2 (default), 2 PS3
    int msaaEnabled;              // 0/1
    int msaaSamples;              // 1-16, clamped to GL_MAX_SAMPLES
    int frameRate;                // >= 30 FPS target
    int internalResolutionHeight; // one of kInternalResolutions, 0 = native
    float drawDistance;           // 0.75-3.0
    int windowMode;               // WindowModeValue
    int vsyncEnabled;             // 0/1
    int aspectMode;               // AspectValue
    float guiScale;               // PS2: 0.75/1.0, Modern: 1.0/1.25
    int guiStyle;                 // GuiStyleValue
    int keyboardBindings[kBindingCount]; // GLFW key codes, never Escape
    int gamepadBindings[kBindingCount];  // GLFW gamepad button 0-14, 1000 LT, 1001 RT
    int disableControllerInputWhenUnfocused; // 0/1
    int lastSaveSlot;             // -1..5, internal: preserve it untouched
    int stereoEnabled;            // 0 mono, 1 stereo
    float musicVolume;            // 0-1
    float soundEffectsVolume;     // 0-1
    float dialogueVolume;         // 0-1
    int streamerMode;             // v9, offset 196: (ThiefRush: hide the room code overlay)
    int alwaysOriginalPsPrompts;  // v10, offset 200: force the original PS glyphs
    int promptStyle;              // v10, offset 204: PromptStyleValue, 0 = Auto
    // v11, offset 208: more video options, mouse and keyboard camera.
    int fovPercent;               // 0-50: extra field of view (drawing only)
    int anisotropy;               // kAnisotropyLevels, 1 = off
    int showFps;                  // 0/1: frame-rate counter
    int upscaler;                 // UpscalerValue: scene -> window filter
    int upscalerSharpness;        // 0-100 (FSR 1 RCAS)
    int cameraSensitivity;        // 25-300 %: stick and key camera speed
    int mouseCamera;              // 0/1: captured mouse turns the camera
    int mouseSensitivity;         // 10-500 %: third-person camera
    int mouseAimSensitivity;      // 10-500 %: binocucom and turrets
    int mouseInvertX, mouseInvertY;
    int mouseRawInput;            // 0/1
    int mouseWheelZoom;           // 0/1: the wheel zooms the binocucom
    int mouseMenus;               // 0/1: hover, click and right-click back
    int cameraKeyBindings[kCameraKeyCount]; // GLFW keys, kKeyUnknown = none
    int muteWhenUnfocused;        // 0/1
    // v12, offset 284 (ProjectCane): one press of Circle skips a cutscene (else hold it 3 s)
    int instantSkipCutscenes;     // 0/1, default 0
    // v13, offset 288 (ProjectCane, Sly 2): with the keyboard / mouse, the hacking minigame's ship shoots toward a
    // cursor (left click fires)
    int hackMouseAim;             // 0/1, default 1
    // v14, offset 292 (ProjectCane): was the debug function keys' switch. Unused since 2026-11-04 (kept for the layout,
    // loaded and saved as is, ignored by every game and no longer shown): Debug Mode.ini (cane::debugMode(), the
    // launcher's Settings > Advanced > Debug mode) turns the debug keys on instead. README.md "Debug mode".
    // Off: none of them does anything (the runtimes' SLY2N_* test switches work regardless).
    int debugKeys;                // 0/1, default 0
    // v15, offset 296: controller vibration (platform/pad_rumble.h). On: the games' own rumble reaches the pad (Sly 2
    // also keeps its in-game Vibration option, which still has to be on); off: the motors are never driven.
    int controllerVibration;      // 0/1, default 1
    // v16, offset 300 (Sly 2, a PC QoL, not the original): the characters' cast shadows reach the floor instead of
    // ending at the original's fixed far cap (Sly2-Release render.cpp FBuildShadow2). v15 files load with it on.
    int longShadows;              // 0/1, default 1
    // v17, offset 304 (Sly 2's camera / aim inversion): the controller's camera (the right
    // stick on the follow camera and under a table; the original has no such option: 0 = the original's orbit) and the
    // mouse in the binocucom and the turrets (0 = natural: mouse up looks up, mouse right turns right). The mouse on
    // the follow camera stays mouseInvertX / mouseInvertY (v11, shared with Sly 1 / Sly 3). The controller's binocucom
    // and turret inversion are the original's own options, kept in each save. v16 files load with all of them off.
    int padInvertCameraX;         // 0/1, default 0
    int padInvertCameraY;         // 0/1, default 0
    int mouseInvertBinocX;        // 0/1, default 0
    int mouseInvertBinocY;        // 0/1, default 0
    int mouseInvertTurretX;       // 0/1, default 0
    int mouseInvertTurretY;       // 0/1, default 0
    // v18, offset 328 (Sly 1 / Sly 2): passive follow-camera steering while moving. Forced camera policies remain active.
    int autoCameraRotation;       // 0/1, default 1
};
static_assert(sizeof(Header) == 20);
static_assert(sizeof(legacy::V1) == 36 && sizeof(legacy::V2) == 40 && sizeof(legacy::V3) == 104);
static_assert(sizeof(legacy::V4) == 168 && sizeof(legacy::V5) == 172 && sizeof(legacy::V6) == 176);
static_assert(sizeof(legacy::V7) == 180 && sizeof(legacy::V8) == 196 && sizeof(legacy::V9) == 200);
static_assert(sizeof(legacy::V10) == 208 && sizeof(legacy::V11) == 284 && sizeof(legacy::V12) == 288);
static_assert(sizeof(legacy::V13) == 292 && sizeof(legacy::V14) == 296 && sizeof(legacy::V15) == 300);
static_assert(sizeof(legacy::V16) == 304 && sizeof(legacy::V17) == 328 && sizeof(SystemSettings) == 332);
static_assert(offsetof(SystemSettings, guiStyle) == 40 && offsetof(SystemSettings, keyboardBindings) == 44);
static_assert(offsetof(SystemSettings, gamepadBindings) == 108);
static_assert(offsetof(SystemSettings, disableControllerInputWhenUnfocused) == 172);
static_assert(offsetof(SystemSettings, lastSaveSlot) == 176 && offsetof(SystemSettings, stereoEnabled) == 180);
static_assert(offsetof(SystemSettings, dialogueVolume) == 192);
static_assert(offsetof(SystemSettings, streamerMode) == 196 && offsetof(SystemSettings, alwaysOriginalPsPrompts) == 200);
static_assert(offsetof(SystemSettings, cameraKeyBindings) == 264 && offsetof(SystemSettings, muteWhenUnfocused) == 280);
static_assert(offsetof(SystemSettings, instantSkipCutscenes) == 284 && offsetof(SystemSettings, hackMouseAim) == 288);
static_assert(offsetof(SystemSettings, debugKeys) == 292 && offsetof(SystemSettings, controllerVibration) == 296);
static_assert(offsetof(SystemSettings, longShadows) == 300 && offsetof(SystemSettings, padInvertCameraX) == 304);
static_assert(offsetof(SystemSettings, mouseInvertTurretY) == 324);
static_assert(offsetof(SystemSettings, autoCameraRotation) == 328);

inline uint32_t checksum(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

// v11 defaults keep the original look and controls; the mouse camera is on
// (it only acts while the window holds a captured cursor during play).
inline void defaultVideoExtras(SystemSettings& s) {
    s.fovPercent = 0;
    s.anisotropy = 1;
    s.showFps = 0;
    s.upscaler = Upscale_Bilinear;
    s.upscalerSharpness = 80;
}
inline void defaultMouse(SystemSettings& s) {
    s.cameraSensitivity = 100;
    s.mouseCamera = 1;
    s.mouseSensitivity = 100;
    s.mouseAimSensitivity = 100;
    s.mouseInvertX = 0;
    s.mouseInvertY = 0;
    s.padInvertCameraX = 0;
    s.padInvertCameraY = 0;
    s.mouseInvertBinocX = 0;
    s.mouseInvertBinocY = 0;
    s.mouseInvertTurretX = 0;
    s.mouseInvertTurretY = 0;
    s.mouseRawInput = 1;
    s.mouseWheelZoom = 1;
    s.mouseMenus = 1;
    std::copy(std::begin(kDefaultCameraKeys), std::end(kDefaultCameraKeys), s.cameraKeyBindings);
}
inline void defaultAudio(SystemSettings& s) {
    s.stereoEnabled = 1;
    s.musicVolume = 1.0f;
    s.soundEffectsVolume = 1.0f;
    s.dialogueVolume = 1.0f;
}

// The game's built-in values (glob.cpp g_fogType, gl.cpp, clock.cpp,
// Input.cpp, save.cpp SetDefaultAudioSettings).
inline SystemSettings defaults() {
    SystemSettings s{};
    s.fogType = Fog_PS2;
    s.msaaEnabled = 0;
    s.msaaSamples = 4;
    s.frameRate = 60;
    s.internalResolutionHeight = 0;
    s.drawDistance = 1.0f;
    s.windowMode = Window_Windowed;
    s.vsyncEnabled = 0;
    s.aspectMode = Aspect_Fit;
    s.guiScale = 1.0f;
    s.guiStyle = Gui_PS2;
    std::copy(std::begin(kDefaultKeyboardBindings), std::end(kDefaultKeyboardBindings), s.keyboardBindings);
    std::copy(std::begin(kDefaultGamepadBindings), std::end(kDefaultGamepadBindings), s.gamepadBindings);
    s.disableControllerInputWhenUnfocused = 0;
    s.lastSaveSlot = -1;
    defaultAudio(s);
    s.streamerMode = 0;
    s.alwaysOriginalPsPrompts = 0;
    s.promptStyle = Prompt_Auto;
    defaultVideoExtras(s);
    defaultMouse(s);
    s.muteWhenUnfocused = 0;
    s.instantSkipCutscenes = 0;
    s.hackMouseAim = 1;
    s.debugKeys = 0;
    s.controllerVibration = 1;
    s.longShadows = 1;
    s.autoCameraRotation = 1;
    return s;
}
// Sly 2's defaults: defaults() with Sly 2's keyboard / mouse layout. (A file that lacks the bindings still takes
// defaults()' through upgrade(); every Sly 2 file has them.)
inline SystemSettings sly2Defaults() {
    SystemSettings s = defaults();
    std::copy(std::begin(kSly2DefaultKeyboardBindings), std::end(kSly2DefaultKeyboardBindings), s.keyboardBindings);
    return s;
}

// Upgrades. Fields a version lacks take defaults(); this reproduces the
// game's historic upgrade code exactly.
namespace detail {
template <class T> SystemSettings fromPrefix(const T& old) {
    static_assert(sizeof(T) <= sizeof(SystemSettings));
    SystemSettings s = defaults();
    std::memcpy(&s, &old, sizeof(T));
    return s;
}
} // namespace detail
inline SystemSettings upgrade(const legacy::V1& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V2& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V3& old) {
    SystemSettings s = upgrade(old.video);
    std::copy(std::begin(old.keyboardBindings), std::end(old.keyboardBindings), s.keyboardBindings);
    return s;
}
inline SystemSettings upgrade(const legacy::V4& old) {
    SystemSettings s = defaults();
    std::memcpy(&s, &old, offsetof(legacy::V4, keyboardBindings));
    std::copy(std::begin(old.keyboardBindings), std::end(old.keyboardBindings), s.keyboardBindings);
    std::copy(std::begin(old.gamepadBindings), std::end(old.gamepadBindings), s.gamepadBindings);
    return s;
}
inline SystemSettings upgrade(const legacy::V5& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V6& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V7& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V8& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V9& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V10& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V11& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V12& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V13& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V14& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V15& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V16& old) { return detail::fromPrefix(old); }
inline SystemSettings upgrade(const legacy::V17& old) { return detail::fromPrefix(old); }

// Ok: current version. Upgraded: an older version, converted in memory (the
// file is untouched until save()). Missing: no file. Corrupt: unreadable,
// bad magic, size or checksum. Newer: written by a newer build; callers must
// not overwrite it. On anything but Ok/Upgraded the output is defaults().
enum class LoadResult { Ok, Missing, Upgraded, Corrupt, Newer };
inline bool loaded(LoadResult result) { return result == LoadResult::Ok || result == LoadResult::Upgraded; }

inline fs::path settingsPath(const fs::path& userDir) {
    return userDir / "Saves" / "system_settings.dat";
}

namespace detail {
template <class T> bool readPayload(std::istream& stream, const Header& header, T& payload) {
    return header.payloadSize == sizeof(T) &&
           stream.read(reinterpret_cast<char*>(&payload), sizeof(T)) &&
           checksum(&payload, sizeof(T)) == header.checksum;
}
template <class T> LoadResult loadLegacy(std::istream& stream, const Header& header, SystemSettings& out) {
    T old{};
    if (!readPayload(stream, header, old)) return LoadResult::Corrupt;
    out = upgrade(old);
    return LoadResult::Upgraded;
}
} // namespace detail

inline LoadResult load(const fs::path& file, SystemSettings& out, uint32_t* versionOut = nullptr) {
    out = defaults();
    if (versionOut) *versionOut = 0;
    std::error_code ec;
    if (!fs::exists(file, ec)) return ec ? LoadResult::Corrupt : LoadResult::Missing;
    std::ifstream stream(file, std::ios::binary);
    Header header{};
    if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0)
        return LoadResult::Corrupt;
    if (versionOut) *versionOut = header.version;
    LoadResult result = LoadResult::Corrupt;
    switch (header.version) {
    case 1: result = detail::loadLegacy<legacy::V1>(stream, header, out); break;
    case 2: result = detail::loadLegacy<legacy::V2>(stream, header, out); break;
    case 3: result = detail::loadLegacy<legacy::V3>(stream, header, out); break;
    case 4: result = detail::loadLegacy<legacy::V4>(stream, header, out); break;
    case 5: result = detail::loadLegacy<legacy::V5>(stream, header, out); break;
    case 6: result = detail::loadLegacy<legacy::V6>(stream, header, out); break;
    case 7: result = detail::loadLegacy<legacy::V7>(stream, header, out); break;
    case 8: result = detail::loadLegacy<legacy::V8>(stream, header, out); break;
    case 9: result = detail::loadLegacy<legacy::V9>(stream, header, out); break;
    case 10: result = detail::loadLegacy<legacy::V10>(stream, header, out); break;
    case 11: result = detail::loadLegacy<legacy::V11>(stream, header, out); break;
    case 12: result = detail::loadLegacy<legacy::V12>(stream, header, out); break;
    case 13: result = detail::loadLegacy<legacy::V13>(stream, header, out); break;
    case 14: result = detail::loadLegacy<legacy::V14>(stream, header, out); break;
    case 15: result = detail::loadLegacy<legacy::V15>(stream, header, out); break;
    case 16: result = detail::loadLegacy<legacy::V16>(stream, header, out); break;
    case 17: result = detail::loadLegacy<legacy::V17>(stream, header, out); break;
    case kVersion: {
        SystemSettings current{};
        if (detail::readPayload(stream, header, current)) {
            out = current;
            result = LoadResult::Ok;
        }
        break;
    }
    default:
        return header.version > kVersion ? LoadResult::Newer : LoadResult::Corrupt;
    }
    if (!loaded(result)) out = defaults();
    return result;
}

// The payload size of a version this header writes (kVersion, or an older prefix layout; 0 for any other).
inline uint32_t payloadSize(uint32_t version) {
    switch (version) {
    case 8: return sizeof(legacy::V8);
    case 11: return sizeof(legacy::V11);
    case 12: return sizeof(legacy::V12);
    case 13: return sizeof(legacy::V13);
    case 14: return sizeof(legacy::V14);
    case 15: return sizeof(legacy::V15);
    case 16: return sizeof(legacy::V16);
    case 17: return sizeof(legacy::V17);
    case kVersion: return sizeof(SystemSettings);
    default: return 0;
    }
}

// Writes <file>.tmp, then replaces <file>. Creates the parent
// directory. The payload is written exactly as given: sanitize first.
// version: kVersion, or an older layout that is a prefix of the current one (8, 11: for tests).
inline bool save(const fs::path& file, const SystemSettings& settings, uint32_t version = kVersion) {
    const uint32_t size = payloadSize(version);
    if (size == 0) return false;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return false;
    const Header header{ { 'P', 'C', 'A', 'N', 'E', 'S', 'Y', 'S' }, version, size, checksum(&settings, size) };
    fs::path temporary = file;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(&settings), size);
    stream.flush();
    if (!stream) return false;
    stream.close();
    if (!stream) return false;
    return atomicReplace(temporary, file);
}

inline bool validKeyboardBinding(int key) {
    return (key >= kKeySpace && key <= kKeyLast && key != kKeyEscape) || mouseBinding(key);
}
// Camera keys: any keyboard key but Escape, or none.
inline bool validCameraKey(int key) {
    return key == kKeyUnknown || (key >= kKeySpace && key <= kKeyLast && key != kKeyEscape);
}
inline bool validGamepadBinding(int binding) {
    return (binding >= 0 && binding <= kGamepadButtonLast) || binding == kGamepadLeftTrigger ||
           binding == kGamepadRightTrigger;
}
// Nearest GUI scale that the style offers (both styles share 100%).
inline float coupledGuiScale(int guiStyle, float guiScale) {
    if (guiStyle == Gui_Modern) return guiScale >= 1.125f ? 1.25f : 1.0f;
    return guiScale < 0.875f ? 0.75f : 1.0f;
}

// The device-independent part of the game's ValidateVideoSettings.
// monitorHeight: current monitor height in pixels, 0 skips the resolution cap.
// maxSamples: GL_MAX_SAMPLES, 0 skips the MSAA cap. Invalid bindings reset to
// the default binding for that action (Escape is never a valid key).
// defaultKeyboard: the game's default key table (kDefaultKeyboardBindings, or Sly 2's kSly2DefaultKeyboardBindings),
// for bindings that are out of range.
inline void sanitizeStatic(SystemSettings& s, int monitorHeight, int maxSamples,
                           const int (&defaultKeyboard)[kBindingCount] = kDefaultKeyboardBindings) {
    s.fogType = std::clamp(s.fogType, static_cast<int>(Fog_Off), static_cast<int>(Fog_PS3));
    s.msaaEnabled = s.msaaEnabled != 0;
    s.msaaSamples = std::clamp(s.msaaSamples, 1, 16);
    s.frameRate = (std::max)(s.frameRate, 30);
    s.drawDistance = std::clamp(s.drawDistance, 0.75f, 3.0f);
    s.windowMode = std::clamp(s.windowMode, static_cast<int>(Window_Windowed), static_cast<int>(Window_Fullscreen));
    s.vsyncEnabled = s.vsyncEnabled != 0;
    s.aspectMode = std::clamp(s.aspectMode, static_cast<int>(Aspect_Fit), static_cast<int>(Aspect_PS2_16_9));
    s.guiStyle = std::clamp(s.guiStyle, static_cast<int>(Gui_PS2), static_cast<int>(Gui_Modern));
    s.disableControllerInputWhenUnfocused = s.disableControllerInputWhenUnfocused != 0;
    s.lastSaveSlot = std::clamp(s.lastSaveSlot, -1, kSaveSlotCount - 1);
    s.stereoEnabled = s.stereoEnabled != 0;
    s.musicVolume = std::clamp(s.musicVolume, 0.0f, 1.0f);
    s.soundEffectsVolume = std::clamp(s.soundEffectsVolume, 0.0f, 1.0f);
    s.dialogueVolume = std::clamp(s.dialogueVolume, 0.0f, 1.0f);
    s.streamerMode = s.streamerMode != 0;
    s.alwaysOriginalPsPrompts = s.alwaysOriginalPsPrompts != 0;
    s.promptStyle = std::clamp(s.promptStyle, static_cast<int>(Prompt_Auto),
                               static_cast<int>(Prompt_SteamController2026));
    s.guiScale = coupledGuiScale(s.guiStyle, s.guiScale);
    s.fovPercent = std::clamp(s.fovPercent, 0, kFovPercentMax) / kFovPercentStep * kFovPercentStep;
    if (std::find(std::begin(kAnisotropyLevels), std::end(kAnisotropyLevels), s.anisotropy) == std::end(kAnisotropyLevels))
        s.anisotropy = 1;
    s.showFps = s.showFps != 0;
    s.upscaler = std::clamp(s.upscaler, static_cast<int>(Upscale_Bilinear), static_cast<int>(Upscale_Fsr1));
    s.upscalerSharpness = std::clamp(s.upscalerSharpness, 0, 100);
    s.cameraSensitivity = std::clamp(s.cameraSensitivity, 25, 300);
    s.mouseCamera = s.mouseCamera != 0;
    s.mouseSensitivity = std::clamp(s.mouseSensitivity, 10, 500);
    s.mouseAimSensitivity = std::clamp(s.mouseAimSensitivity, 10, 500);
    s.mouseInvertX = s.mouseInvertX != 0;
    s.mouseInvertY = s.mouseInvertY != 0;
    s.padInvertCameraX = s.padInvertCameraX != 0;
    s.padInvertCameraY = s.padInvertCameraY != 0;
    s.mouseInvertBinocX = s.mouseInvertBinocX != 0;
    s.mouseInvertBinocY = s.mouseInvertBinocY != 0;
    s.mouseInvertTurretX = s.mouseInvertTurretX != 0;
    s.mouseInvertTurretY = s.mouseInvertTurretY != 0;
    s.mouseRawInput = s.mouseRawInput != 0;
    s.mouseWheelZoom = s.mouseWheelZoom != 0;
    s.mouseMenus = s.mouseMenus != 0;
    s.muteWhenUnfocused = s.muteWhenUnfocused != 0;
    s.instantSkipCutscenes = s.instantSkipCutscenes != 0;
    s.hackMouseAim = s.hackMouseAim != 0;
    s.debugKeys = s.debugKeys != 0;
    s.controllerVibration = s.controllerVibration != 0;
    s.longShadows = s.longShadows != 0;
    s.autoCameraRotation = s.autoCameraRotation != 0;
    for (int i = 0; i < kCameraKeyCount; ++i)
        if (!validCameraKey(s.cameraKeyBindings[i])) s.cameraKeyBindings[i] = kDefaultCameraKeys[i];
    for (int button = 0; button < kBindingCount; ++button) {
        if (!validKeyboardBinding(s.keyboardBindings[button]))
            s.keyboardBindings[button] = defaultKeyboard[button];
        if (!validGamepadBinding(s.gamepadBindings[button]))
            s.gamepadBindings[button] = kDefaultGamepadBindings[button];
    }
    if (std::find(std::begin(kInternalResolutions), std::end(kInternalResolutions),
                  s.internalResolutionHeight) == std::end(kInternalResolutions))
        s.internalResolutionHeight = 0;
    if (monitorHeight > 0 && s.internalResolutionHeight > monitorHeight) {
        s.internalResolutionHeight = 0;
        for (int height : kInternalResolutions)
            if (height <= monitorHeight) s.internalResolutionHeight = height;
    }
    if (maxSamples > 0) {
        s.msaaSamples = std::clamp(s.msaaSamples, 1, (std::max)(maxSamples, 1));
        if (maxSamples < 2) s.msaaEnabled = 0;
    }
}
} // namespace cane::settings
