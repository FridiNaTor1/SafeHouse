#pragma once
// Sly 2's settings file: <user-dir>/Settings.ini, a commented, human-readable INI that the game (Sly2-Release display.cpp,
// menu.cpp) and the launcher's Settings page read and write through this one schema, so both always agree. It holds
// every option: the PC options (the shared SystemSettings fields Sly 2 uses: display, graphics, audio, controls,
// bindings, mouse) and the original game's own Options page (vibration, speakers, music / effects volume, the
// controller's binocucom / turret inversion), which the PS2 kept in each save's "flags". notes/OPTIONS_2026-11-10.md.
//
// Robustness: comments (";" / "#") and blank lines are ignored; keys and sections match without case, spaces, "_" or
// "-"; a missing key or a value that cannot be read takes its default; keys this build does not know (a newer
// build's, a typo) are kept and written back in their section; writes are atomic (<file>.tmp, then replace).
//
// Migration (once): when Settings.ini does not exist, the old Saves/system_settings.dat (any version) is read
// (it is left in place, no longer read once the INI exists) and the INI is written; the save-kept options are marked
// pending ([SafeHouse] Pending Save Options) and the game takes them from the newest save the first time it reads the
// card (menu.cpp MigrateSaveOptions). A pending option the launcher changes is no longer pending (the user's choice).
//
// Saves: the progress format stays the original's. The game keeps the save's option "flags" in step with this file
// (menu.cpp SyncOptionsToRecord), so every save it writes carries the current options as compatible values (an
// original or older build that loads it gets the same options); the game itself no longer reads them from a save.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include "audio_devices.h"
#include "settings_ini.h"
#include "system_settings.h"
#include "texture_options.h"

namespace cane::settings {

// The original's Speakers option (the save's flags "nMixerMode"; the menu's 0x4E / 0x4C / 0x4D). On PC Surround
// (the PS2's Pro Logic II mix, its default) and Stereo both play in stereo; Mono mixes both channels together.
enum Sly2Speakers : int { Speakers_Surround = 0, Speakers_Mono = 1, Speakers_Stereo = 2 };

// The original game's own options, besides those merged into SystemSettings (vibration: controllerVibration; music /
// effects volume: musicVolume / soundEffectsVolume).
struct Sly2GameOptions {
    int invertBinocucom = 0; // flags "fReverseLookAround": the controller in the binocucom
    int invertTurret = 0;    // flags "fReverseAim": the controller on a turret
    int speakers = Speakers_Surround;
    // (lang) the game's language: "" (Auto: the PC's language, then the title's language menu each session, as the
    // European discs do) or a language's folder name ("English", "French", "Japanese", "English (UK)", a mod's)
    std::string language;
    // (lang) the voices' language (the dialogue, the binocucom, the movies' sound): "" Same As Text, else a language's
    // folder name with voice lines (Japanese voices with English text, ...); [Language] Voices
    std::string voiceLanguage;
    // (audio, notes/AUDIO_2026-11-10.md; platform/audio_devices.h) the devices by name, "" the system default
    std::string outputDevice;   // the game's sound
    std::string inputDevice;    // the microphone (only recorded while USB Headset is on)
    // The original's USB Headset option (flags "fUseHeadset", the menu's 0x4F / 0x50; the sound manager's +0x10): on, the
    // microphone's level makes noise guards hear (sound.cpp UpdateMicNoise, orig 0x1FDA48) and the dialogue lines go to
    // the headset's speaker (headsetOutputDevice; spiop.cpp, the IRX's lgaud path)
    int usbHeadset = 0;
    std::string headsetOutputDevice;  // "" the game's output (the lines play as without a headset)
    // (texture mods and packs, Sly2-Release texmod.h) their decoded texels kept in the user's cache folder; their
    // load on the other cores while the game goes on (the original texture drawn until a replacement is ready)
    int textureCache = kTextureCacheDefault;
    int textureStreaming = kTextureStreamingDefault;
};

// VR (Sly2-Release vr.h; only used with a headset): [VR] in the file.
enum Sly2VrMode : int { VrMode_Flat = 0, VrMode_Immersive = 1 };
struct Sly2VrOptions {
    int mode = VrMode_Immersive;
    int autoCameraRotation = 0;  // Immersive: the follow camera's passive steering (off: only the right stick turns it)
    int vignette = 0;            // 0 Off, 1 Low, 2 Medium, 3 High
    int screenSize = 1;          // Flat Screen: 0 Small, 1 Medium, 2 Large
    int refreshRate = 0;         // 0 Automatic (the headset's best up to 120 Hz), else 72 / 80 / 90 / 120
    float worldScale = 1.0f;     // Immersive: 1.00 life size (0.50 to 2.00)
    int cutscenesFlat = 1;       // Immersive: the cutscenes on the Flat Screen
    int rcTable = 0;             // Immersive, experimental: Ep7's RC Combat Club from the table
    int eyeResolution = 0;       // Immersive: 0 Dynamic (100 % to 75 %), else 100 / 90 / 75 / 60 / 50 (%)
    int msaa = 2;                // the eyes' MSAA: 2 (2x) or 1 (Off)
    int foveation = 2;           // fixed foveation: 0 Off, 1 Low, 2 Medium, 3 High
    int multiview = 1;           // Vulkan: both eyes drawn in one pass (1 On, 0 Off: the eyes one by one)
    int upscaling = 2;           // the headset compositor's filter on the eyes: 0 Off, 1 Sharpen, 2 Super Resolution
};

struct Sly2Settings {
    SystemSettings system = sly2Defaults();
    Sly2GameOptions game;
    Sly2VrOptions vr;
    // the save-kept options still to be taken from the newest save (migration; the schema's key names)
    std::vector<std::string> pendingSaveOptions;
    // keys and sections this build does not know, written back as they are
    ini::Document unknown;
};

inline constexpr const char* kSly2IniName = "Settings.ini";
inline fs::path sly2SettingsPath(const fs::path& userDir) { return userDir / kSly2IniName; }

// The save-kept options' keys (pendingSaveOptions)
inline constexpr const char* kSaveOptionVibration = "Vibration";
inline constexpr const char* kSaveOptionMusic = "Music Volume";
inline constexpr const char* kSaveOptionEffects = "Effects Volume";
inline constexpr const char* kSaveOptionSpeakers = "Speakers";
inline constexpr const char* kSaveOptionInvertBinocucom = "Invert Binocucom";
inline constexpr const char* kSaveOptionInvertTurret = "Invert Turret";
inline std::vector<std::string> allSaveOptions() {
    return { kSaveOptionVibration, kSaveOptionMusic, kSaveOptionEffects, kSaveOptionSpeakers, kSaveOptionInvertBinocucom,
             kSaveOptionInvertTurret };
}
inline bool pending(const Sly2Settings& s, std::string_view key) {
    return std::any_of(s.pendingSaveOptions.begin(), s.pendingSaveOptions.end(),
                       [&](const std::string& k) { return ini::keyForm(k) == ini::keyForm(key); });
}
inline void clearPending(Sly2Settings& s, std::string_view key) {
    auto& v = s.pendingSaveOptions;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& k) { return ini::keyForm(k) == ini::keyForm(key); }), v.end());
}

// ------------------------------------------------------------ value names
namespace names {
struct Named {
    int value;
    const char* name;
};
// GLFW key codes (the bindings' values) by name; mouse buttons and the wheel (kMouseBindingBase..).
inline const std::vector<Named>& keys() {
    static const std::vector<Named> table = [] {
        std::vector<Named> t = {
            { kKeyUnknown, "None" }, { 32, "Space" }, { 39, "Apostrophe" }, { 44, "Comma" }, { 45, "Minus" }, { 46, "Period" },
            { 47, "Slash" }, { 59, "Semicolon" }, { 61, "Equal" }, { 91, "Left Bracket" }, { 92, "Backslash" },
            { 93, "Right Bracket" }, { 96, "Grave Accent" }, { 161, "World 1" }, { 162, "World 2" }, { 256, "Escape" },
            { 257, "Enter" }, { 258, "Tab" }, { 259, "Backspace" }, { 260, "Insert" }, { 261, "Delete" }, { 262, "Right Arrow" },
            { 263, "Left Arrow" }, { 264, "Down Arrow" }, { 265, "Up Arrow" }, { 266, "Page Up" }, { 267, "Page Down" }, { 268, "Home" },
            { 269, "End" }, { 280, "Caps Lock" }, { 281, "Scroll Lock" }, { 282, "Num Lock" }, { 283, "Print Screen" },
            { 284, "Pause" }, { 330, "Keypad Decimal" }, { 331, "Keypad Divide" }, { 332, "Keypad Multiply" },
            { 333, "Keypad Subtract" }, { 334, "Keypad Add" }, { 335, "Keypad Enter" }, { 336, "Keypad Equal" },
            { 340, "Left Shift" }, { 341, "Left Ctrl" }, { 342, "Left Alt" }, { 343, "Left Super" }, { 344, "Right Shift" },
            { 345, "Right Ctrl" }, { 346, "Right Alt" }, { 347, "Right Super" }, { 348, "Menu" },
            { kMouseBindingBase + 0, "Mouse Left" }, { kMouseBindingBase + 1, "Mouse Right" },
            { kMouseBindingBase + 2, "Mouse Middle" }, { kMouseWheelUp, "Wheel Up" }, { kMouseWheelDown, "Wheel Down" },
            // (other spellings read too; the first name is the one written)
            { 262, "Right" }, { 263, "Left" }, { 264, "Down" }, { 265, "Up" }, { 257, "Return" }, { 256, "Esc" },
            { 341, "Left Control" }, { 345, "Right Control" }, { 32, "Spacebar" },
        };
        static const char* const digits[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
        static const char* const letters[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                               "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
        static const char* const fkeys[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13",
                                             "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "F25" };
        static const char* const keypad[] = { "Keypad 0", "Keypad 1", "Keypad 2", "Keypad 3", "Keypad 4",
                                              "Keypad 5", "Keypad 6", "Keypad 7", "Keypad 8", "Keypad 9" };
        static const char* const mouse[] = { "Mouse 4", "Mouse 5", "Mouse 6", "Mouse 7", "Mouse 8" };
        for (int i = 0; i < 10; ++i) t.push_back({ 48 + i, digits[i] });
        for (int i = 0; i < 26; ++i) t.push_back({ 65 + i, letters[i] });
        for (int i = 0; i < 25; ++i) t.push_back({ 290 + i, fkeys[i] });
        for (int i = 0; i < 10; ++i) t.push_back({ 320 + i, keypad[i] });
        for (int i = 0; i < 5; ++i) t.push_back({ kMouseBindingBase + 3 + i, mouse[i] });
        return t;
    }();
    return table;
}
// GLFW gamepad buttons by the PlayStation names the game uses; L2 / R2 are the triggers.
inline const std::vector<Named>& padButtons() {
    static const std::vector<Named> table = {
        { 0, "Cross" }, { 1, "Circle" }, { 2, "Square" }, { 3, "Triangle" }, { 4, "L1" }, { 5, "R1" }, { 6, "Select" },
        { 7, "Start" }, { 8, "Guide" }, { 9, "L3" }, { 10, "R3" }, { 11, "Dpad Up" }, { 12, "Dpad Right" },
        { 13, "Dpad Down" }, { 14, "Dpad Left" }, { kGamepadLeftTrigger, "L2" }, { kGamepadRightTrigger, "R2" },
    };
    return table;
}
inline std::string nameOf(const std::vector<Named>& table, int value) {
    for (const auto& n : table)
        if (n.value == value) return n.name;
    return std::to_string(value);
}
// A name (without case, spaces, "_" or "-") or a plain number.
inline bool valueOf(const std::vector<Named>& table, const std::string& text, int& out) {
    const std::string form = ini::keyForm(text);
    if (form.empty()) return false;
    for (const auto& n : table)
        if (ini::keyForm(n.name) == form) return out = n.value, true;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end && end != text.c_str() && *end == '\0') return out = int(v), true;
    return false;
}
inline bool parseBool(const std::string& text, int& out) {
    const std::string v = ini::keyForm(text);
    if (v == "on" || v == "true" || v == "yes" || v == "1") return out = 1, true;
    if (v == "off" || v == "false" || v == "no" || v == "0") return out = 0, true;
    return false;
}
inline bool parseInt(const std::string& text, int& out) {
    char* end = nullptr;
    const std::string t = ini::trim(text);
    const long v = std::strtol(t.c_str(), &end, 10);
    if (t.empty() || !end || (*end != '\0' && *end != '%')) return false;
    out = int(v);
    return true;
}
inline bool parseFloat(const std::string& text, float& out) {
    const std::string t = ini::trim(text);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (t.empty() || !end || (*end != '\0' && *end != 'x' && *end != 'X') || !std::isfinite(v)) return false;
    out = float(v);
    return true;
}
inline std::string percent(float v) { return std::to_string(int(std::lround(std::clamp(v, 0.0f, 1.0f) * 100.0f))); }
inline std::string decimal(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", double(v));
    return buffer;
}
} // namespace names

// ------------------------------------------------------------ the schema
// One key: its section, name, comment, how it is written and read (read returns false for an invalid value, which
// then keeps the default).
struct Sly2Key {
    const char* section;
    const char* key;
    const char* comment;
    std::function<std::string(const Sly2Settings&)> write;
    std::function<bool(Sly2Settings&, const std::string&)> read;
};

namespace detail {
using SS = SystemSettings;
inline Sly2Key boolKey(const char* section, const char* key, const char* comment, int SS::*field) {
    return { section, key, comment, [field](const Sly2Settings& s) { return std::string(s.system.*field ? "On" : "Off"); },
             [field](Sly2Settings& s, const std::string& v) { return names::parseBool(v, s.system.*field); } };
}
inline Sly2Key gameBoolKey(const char* section, const char* key, const char* comment, int Sly2GameOptions::*field) {
    return { section, key, comment, [field](const Sly2Settings& s) { return std::string(s.game.*field ? "On" : "Off"); },
             [field](Sly2Settings& s, const std::string& v) { return names::parseBool(v, s.game.*field); } };
}
inline Sly2Key rangeKey(const char* section, const char* key, const char* comment, int SS::*field, int minimum, int maximum) {
    return { section, key, comment, [field](const Sly2Settings& s) { return std::to_string(s.system.*field); },
             [field, minimum, maximum](Sly2Settings& s, const std::string& v) {
                 int n = 0;
                 if (!names::parseInt(v, n) || n < minimum || n > maximum) return false;
                 s.system.*field = n;
                 return true;
             } };
}
inline Sly2Key volumeKey(const char* section, const char* key, const char* comment, float SS::*field) {
    return { section, key, comment, [field](const Sly2Settings& s) { return names::percent(s.system.*field); },
             [field](Sly2Settings& s, const std::string& v) {
                 int n = 0;
                 if (!names::parseInt(v, n) || n < 0 || n > 100) return false;
                 s.system.*field = float(n) / 100.0f;
                 return true;
             } };
}
inline Sly2Key choiceKey(const char* section, const char* key, const char* comment, int SS::*field,
                         std::vector<names::Named> choices) {
    return { section, key, comment, [field, choices](const Sly2Settings& s) { return names::nameOf(choices, s.system.*field); },
             [field, choices](Sly2Settings& s, const std::string& v) {
                 int n = 0;
                 if (!names::valueOf(choices, v, n)) return false;
                 if (std::none_of(choices.begin(), choices.end(), [n](const names::Named& c) { return c.value == n; })) return false;
                 s.system.*field = n;
                 return true;
             } };
}
inline Sly2Key bindingKey(const char* section, const char* key, int* (*slot)(SS&), bool pad, bool (*valid)(int)) {
    return { section, key, nullptr,
             [slot, pad](const Sly2Settings& s) {
                 return names::nameOf(pad ? names::padButtons() : names::keys(), *slot(const_cast<SS&>(s.system)));
             },
             [slot, pad, valid](Sly2Settings& s, const std::string& v) {
                 int n = 0;
                 if (!names::valueOf(pad ? names::padButtons() : names::keys(), v, n) || !valid(n)) return false;
                 *slot(s.system) = n;
                 return true;
             } };
}
} // namespace detail

inline constexpr std::array<const char*, kBindingCount> kSly2BindingKeys{
    "Move Forward", "Move Back", "Move Left", "Move Right", "Square", "Circle", "Cross", "Triangle",
    "Start", "Select", "L1", "R1", "L2", "R2", "L3", "R3" };

inline const std::vector<Sly2Key>& sly2Schema() {
    using namespace detail;
    static const std::vector<Sly2Key> schema = [] {
        std::vector<Sly2Key> k;
        // [Display]
        k.push_back(choiceKey("Display", "Window Mode", "Windowed, Borderless or Fullscreen", &SS::windowMode,
                              { { Window_Windowed, "Windowed" }, { Window_Borderless, "Borderless" }, { Window_Fullscreen, "Fullscreen" } }));
        k.push_back(boolKey("Display", "VSync", "On or Off", &SS::vsyncEnabled));
        k.push_back(choiceKey("Display", "Aspect Ratio", "Fit (the whole window), 4:3, 16:9, 16:10, PS2 4:3 or PS2 16:9",
                              &SS::aspectMode,
                              { { Aspect_Fit, "Fit" }, { Aspect_4_3, "4:3" }, { Aspect_16_9, "16:9" }, { Aspect_16_10, "16:10" },
                                { Aspect_PS2_4_3, "PS2 4:3" }, { Aspect_PS2_16_9, "PS2 16:9" } }));
        k.push_back({ "Display", "Frame Rate Limit", "30 or more (frames per second), or Uncapped",
                      [](const Sly2Settings& s) { return s.system.frameRate >= 1000 ? std::string("Uncapped") : std::to_string(s.system.frameRate); },
                      [](Sly2Settings& s, const std::string& v) {
                          if (ini::keyForm(v) == "uncapped") return s.system.frameRate = 1000, true;
                          int n = 0;
                          if (!names::parseInt(v, n) || n < 30) return false;
                          s.system.frameRate = std::min(n, 1000);
                          return true;
                      } });
        // [Graphics]
        k.push_back({ "Graphics", "Internal Resolution", "Native, 360, 480, 720, 1080, 1440 or 2160 (the 3D scene's height)",
                      [](const Sly2Settings& s) {
                          return s.system.internalResolutionHeight ? std::to_string(s.system.internalResolutionHeight) : std::string("Native");
                      },
                      [](Sly2Settings& s, const std::string& v) {
                          if (ini::keyForm(v) == "native") return s.system.internalResolutionHeight = 0, true;
                          int n = 0;
                          std::string t = ini::trim(v);
                          if (!t.empty() && (t.back() == 'p' || t.back() == 'P')) t.pop_back();
                          if (!names::parseInt(t, n) ||
                              std::find(std::begin(kInternalResolutions), std::end(kInternalResolutions), n) == std::end(kInternalResolutions))
                              return false;
                          s.system.internalResolutionHeight = n;
                          return true;
                      } });
        k.push_back({ "Graphics", "Anti-Aliasing", "Off, 2x, 4x, 8x or 16x (MSAA)",
                      [](const Sly2Settings& s) {
                          return s.system.msaaEnabled && s.system.msaaSamples > 1 ? std::to_string(s.system.msaaSamples) + "x" : std::string("Off");
                      },
                      [](Sly2Settings& s, const std::string& v) {
                          if (ini::keyForm(v) == "off") return s.system.msaaEnabled = 0, true;
                          std::string t = ini::trim(v);
                          if (!t.empty() && (t.back() == 'x' || t.back() == 'X')) t.pop_back();
                          int n = 0;
                          if (!names::parseInt(t, n) || (n != 2 && n != 4 && n != 8 && n != 16)) return false;
                          s.system.msaaEnabled = 1;
                          s.system.msaaSamples = n;
                          return true;
                      } });
        k.push_back({ "Graphics", "Draw Distance", "0.75 to 3.00 (1.00 is the original's)",
                      [](const Sly2Settings& s) { return names::decimal(s.system.drawDistance); },
                      [](Sly2Settings& s, const std::string& v) {
                          float g = 0;
                          if (!names::parseFloat(v, g) || g < 0.75f - 1e-4f || g > 3.0f + 1e-4f) return false;
                          s.system.drawDistance = std::clamp(g, 0.75f, 3.0f);
                          return true;
                      } });
        k.push_back(boolKey("Graphics", "Long Shadows", "On: character shadows from low lights reach the floor. Off: as on the PS2",
                            &SS::longShadows));
        k.push_back(boolKey("Graphics", "Fast Shadows", "On: character shadows with less GPU work (low-end devices; nearly the same picture). Off: as before",
                            &SS::fastShadows));
        k.push_back(gameBoolKey("Graphics", "Texture Cache",
                                "On: texture mods and texture packs load from a cache of their decoded pictures (made as they are\n"
                                "first used; the same picture, faster loading). Off: always from the PNG files",
                                &Sly2GameOptions::textureCache));
        k.push_back(gameBoolKey("Graphics", "Texture Streaming",
                                "On: texture mods and texture packs load in the background; a texture shows its original for a\n"
                                "moment when it first appears. Off: each loads when it is first drawn (a pause on slow disks)",
                                &Sly2GameOptions::textureStreaming));
        // (settings v21, Sly2-Release renderer.h: read when the game starts)
        k.push_back({ "Graphics", "Renderer", "Auto, Vulkan or OpenGL (Auto: Vulkan, with OpenGL as the fallback when Vulkan cannot start on\n"
                                              "this computer; after a failed Vulkan start Auto stays on OpenGL until Vulkan is chosen)",
                      [](const Sly2Settings& s) {
                          return std::string(s.system.renderer == 1 ? "Vulkan" : s.system.renderer == 2 ? "OpenGL" : "Auto");
                      },
                      [](Sly2Settings& s, const std::string& v) {
                          const std::string f = ini::keyForm(v);
                          if (f == "auto" || f == "automatic" || f.rfind("auto(", 0) == 0) return s.system.renderer = 0, true;
                          if (f == "vulkan") return s.system.renderer = 1, true;
                          if (f == "opengl" || f == "gl") return s.system.renderer = 2, true;
                          return false;
                      } });
        // [Audio]
        k.push_back(volumeKey("Audio", kSaveOptionMusic, "0 to 100", &SS::musicVolume));
        k.push_back(volumeKey("Audio", kSaveOptionEffects, "0 to 100", &SS::soundEffectsVolume));
        k.push_back(volumeKey("Audio", "Dialogue Volume", "0 to 100", &SS::dialogueVolume));
        k.push_back({ "Audio", kSaveOptionSpeakers, "Surround, Stereo or Mono (Surround and Stereo both play in stereo)",
                      [](const Sly2Settings& s) {
                          return std::string(s.game.speakers == Speakers_Mono ? "Mono" : s.game.speakers == Speakers_Stereo ? "Stereo" : "Surround");
                      },
                      [](Sly2Settings& s, const std::string& v) {
                          const std::string f = ini::keyForm(v);
                          if (f == "surround") return s.game.speakers = Speakers_Surround, true;
                          if (f == "stereo") return s.game.speakers = Speakers_Stereo, true;
                          if (f == "mono") return s.game.speakers = Speakers_Mono, true;
                          return false;
                      } });
        // (audio) the devices (platform/audio_devices.h: by name; "System Default", a missing one plays on the default)
        auto deviceKey = [](const char* key, const char* comment, std::string Sly2GameOptions::*field) {
            return Sly2Key{ "Audio", key, comment, [field](const Sly2Settings& s) { return audio::nameToIni(s.game.*field); },
                            [field](Sly2Settings& s, const std::string& v) { return s.game.*field = audio::nameFromIni(v), true; } };
        };
        k.push_back(deviceKey("Output Device", "System Default, or a device's name as the launcher lists it (unplugged: the default)",
                              &Sly2GameOptions::outputDevice));
        k.push_back(deviceKey("Input Device", "The microphone for USB Headset: System Default or a device's name", &Sly2GameOptions::inputDevice));
        k.push_back(gameBoolKey("Audio", "USB Headset",
                                "On or Off (the original's option): shouting into the microphone makes noise nearby guards hear, and the\n"
                                "dialogue plays on the Headset Output Device",
                                &Sly2GameOptions::usbHeadset));
        k.push_back({ "Audio", "Headset Output Device",
                      "Same As Output, or a device's name: where USB Headset plays the dialogue (missing: as without a headset)",
                      [](const Sly2Settings& s) {
                          return s.game.headsetOutputDevice.empty() ? std::string("Same As Output") : audio::nameToIni(s.game.headsetOutputDevice);
                      },
                      [](Sly2Settings& s, const std::string& v) {
                          const std::string f = ini::keyForm(v);
                          s.game.headsetOutputDevice = f == "sameasoutput" ? std::string() : audio::nameFromIni(v);
                          return true;
                      } });
        // [Language] (Sly2-Release/langstore.h)
        k.push_back({ "Language", "Language",
                      "Auto (the PC's language; the title then offers its language menu), or English, French, Italian,\n"
                      "German, Spanish, Dutch, Portuguese, Danish, Finnish, Norwegian, Swedish, Japanese, or a folder\n"
                      "of the languages folder (English (UK), a mod's): it must be installed or imported",
                      [](const Sly2Settings& s) { return s.game.language.empty() ? std::string("Auto") : s.game.language; },
                      [](Sly2Settings& s, const std::string& v) {
                          const std::string t = ini::trim(v);
                          s.game.language = ini::keyForm(t) == "auto" ? std::string() : t;
                          return true;
                      } });
        // (the text is Language's; a file without Voices plays the voices in it too)
        k.push_back({ "Language", "Voices",
                      "Same As Text, or a language with voices (English, Japanese, French, ... or a folder of the languages\n"
                      "folder): the dialogue and the cutscenes' sound in another language than the text. Choosing a language\n"
                      "on the title screen sets both back to it",
                      [](const Sly2Settings& s) { return s.game.voiceLanguage.empty() ? std::string("Same As Text") : s.game.voiceLanguage; },
                      [](Sly2Settings& s, const std::string& v) {
                          const std::string t = ini::trim(v);
                          s.game.voiceLanguage = ini::keyForm(t) == "sameastext" ? std::string() : t;
                          return true;
                      } });
        // [Controls]
        k.push_back(boolKey("Controls", kSaveOptionVibration, "On or Off: the controller's rumble", &SS::controllerVibration));
        {
            // (the names of input::promptStyleName(), controller_family.h, written first; the earlier spellings after
            // them are still read)
            std::vector<names::Named> styles = { { Prompt_Auto, "Automatic" }, { Prompt_PlayStation, "PlayStation" },
                                                 { Prompt_Xbox, "Xbox" }, { Prompt_Nintendo, "Nintendo Switch" },
                                                 { Prompt_SteamDeck, "Steam Deck" }, { Prompt_SteamController, "Steam Controller" },
                                                 { Prompt_PS4, "PlayStation 4" }, { Prompt_PS5, "PlayStation 5" },
                                                 { Prompt_SteamController2026, "Steam Controller (2026)" },
                                                 { Prompt_GameCube, "Nintendo GameCube" }, { Prompt_Switch2, "Nintendo Switch 2" },
                                                 { Prompt_Wii, "Wii Classic Controller" }, { Prompt_WiiU, "Wii U Pro Controller" },
                                                 { Prompt_MetaQuest, "Meta Quest Touch" }, { Prompt_ValveIndex, "Valve Index" },
                                                 { Prompt_SteamFrame, "Steam Frame" }, { Prompt_ViveWand, "HTC Vive" },
                                                 { Prompt_Pico, "Pico" },
                                                 { Prompt_Nintendo, "Nintendo" }, { Prompt_SteamController2026, "Steam Controller 2026" },
                                                 { Prompt_GameCube, "GameCube" }, { Prompt_Switch2, "Switch 2" },
                                                 { Prompt_Wii, "Wii" }, { Prompt_WiiU, "Wii U" } };
            k.push_back({ "Controls", "Button Icons",
                          "Automatic, PlayStation, Xbox, Nintendo Switch, Steam Deck, Steam Controller, PlayStation 4, "
                          "PlayStation 5, Steam Controller (2026), Nintendo GameCube, Nintendo Switch 2, Wii Classic Controller, "
                          "Wii U Pro Controller, Meta Quest Touch, Valve Index, Steam Frame, HTC Vive, Pico",
                          [styles](const Sly2Settings& s) { return names::nameOf(styles, s.system.promptStyle); },
                          [styles](Sly2Settings& s, const std::string& v) {
                              int n = 0;
                              // (styles a newer build adds: their number; sanitizeStatic keeps the known range)
                              if (!names::valueOf(styles, v, n) || n < 0) return false;
                              s.system.promptStyle = n;
                              return true;
                          } });
        }
        k.push_back(boolKey("Controls", "Original PlayStation Icons", "On: every device shows the PS2's button icons",
                            &SS::alwaysOriginalPsPrompts));
        k.push_back(boolKey("Controls", "Instant Skip Cutscenes", "On: one press of Circle skips. Off: hold it 3 seconds",
                            &SS::instantSkipCutscenes));
        k.push_back(boolKey("Controls", "Automatic Camera Rotation", "On: the camera turns as you move", &SS::autoCameraRotation));
        k.push_back(boolKey("Controls", "Ignore Controller When Unfocused", "On: no controller input while another window has focus",
                            &SS::disableControllerInputWhenUnfocused));
        // [Controller]
        k.push_back(boolKey("Controller", "Invert Camera Horizontal", "Off: the PS2's orbit camera", &SS::padInvertCameraX));
        k.push_back(boolKey("Controller", "Invert Camera Vertical", "Off: the PS2's orbit camera", &SS::padInvertCameraY));
        k.push_back(gameBoolKey("Controller", kSaveOptionInvertBinocucom, "The original's Invert Binocucom Controls",
                                &Sly2GameOptions::invertBinocucom));
        k.push_back(gameBoolKey("Controller", kSaveOptionInvertTurret, "The original's Invert Turret Controls",
                                &Sly2GameOptions::invertTurret));
        static int* (*const padSlot[kBindingCount])(SS&) = {
#define CANE_SLOT(i) [](SS& s) { return &s.gamepadBindings[i]; }
            CANE_SLOT(0), CANE_SLOT(1), CANE_SLOT(2), CANE_SLOT(3), CANE_SLOT(4), CANE_SLOT(5), CANE_SLOT(6), CANE_SLOT(7),
            CANE_SLOT(8), CANE_SLOT(9), CANE_SLOT(10), CANE_SLOT(11), CANE_SLOT(12), CANE_SLOT(13), CANE_SLOT(14), CANE_SLOT(15)
#undef CANE_SLOT
        };
        for (int i = 0; i < kBindingCount; ++i)
            k.push_back(bindingKey("Controller", kSly2BindingKeys[size_t(i)], padSlot[i], true, validGamepadBinding));
        // [Mouse]
        k.push_back(boolKey("Mouse", "Mouse Camera", "On: while playing, the mouse turns the camera", &SS::mouseCamera));
        k.push_back(rangeKey("Mouse", "Camera Sensitivity", "10 to 500 (percent)", &SS::mouseSensitivity, 10, 500));
        k.push_back(rangeKey("Mouse", "Aim Sensitivity", "10 to 500 (percent): the binocucom and turrets", &SS::mouseAimSensitivity, 10, 500));
        k.push_back(boolKey("Mouse", "Invert Camera Horizontal", nullptr, &SS::mouseInvertX));
        k.push_back(boolKey("Mouse", "Invert Camera Vertical", nullptr, &SS::mouseInvertY));
        k.push_back(boolKey("Mouse", "Invert Binocucom Horizontal", nullptr, &SS::mouseInvertBinocX));
        k.push_back(boolKey("Mouse", "Invert Binocucom Vertical", nullptr, &SS::mouseInvertBinocY));
        k.push_back(boolKey("Mouse", "Invert Turret Horizontal", nullptr, &SS::mouseInvertTurretX));
        k.push_back(boolKey("Mouse", "Invert Turret Vertical", nullptr, &SS::mouseInvertTurretY));
        k.push_back(boolKey("Mouse", "Raw Input", "On: unaccelerated mouse motion where supported", &SS::mouseRawInput));
        k.push_back(boolKey("Mouse", "Wheel Zoom", "On: the wheel zooms the binocucom and turrets when no action uses it", &SS::mouseWheelZoom));
        k.push_back(boolKey("Mouse", "Mouse In Menus", "On: point, left click to choose, right click to go back", &SS::mouseMenus));
        k.push_back(boolKey("Mouse", "Mouse Aim In Hacking", "On: Bentley's hacking ship aims at the cursor", &SS::hackMouseAim));
        // [Keyboard]
        static int* (*const keySlot[kBindingCount])(SS&) = {
#define CANE_SLOT(i) [](SS& s) { return &s.keyboardBindings[i]; }
            CANE_SLOT(0), CANE_SLOT(1), CANE_SLOT(2), CANE_SLOT(3), CANE_SLOT(4), CANE_SLOT(5), CANE_SLOT(6), CANE_SLOT(7),
            CANE_SLOT(8), CANE_SLOT(9), CANE_SLOT(10), CANE_SLOT(11), CANE_SLOT(12), CANE_SLOT(13), CANE_SLOT(14), CANE_SLOT(15)
#undef CANE_SLOT
        };
        for (int i = 0; i < kBindingCount; ++i)
            k.push_back(bindingKey("Keyboard", kSly2BindingKeys[size_t(i)], keySlot[i], false, validKeyboardBinding));
        static const char* const cameraKeys[kCameraKeyCount] = { "Camera Left", "Camera Right", "Camera Up", "Camera Down" };
        static int* (*const cameraSlot[kCameraKeyCount])(SS&) = {
            [](SS& s) { return &s.cameraKeyBindings[0]; }, [](SS& s) { return &s.cameraKeyBindings[1]; },
            [](SS& s) { return &s.cameraKeyBindings[2]; }, [](SS& s) { return &s.cameraKeyBindings[3]; } };
        for (int i = 0; i < kCameraKeyCount; ++i) k.push_back(bindingKey("Keyboard", cameraKeys[i], cameraSlot[i], false, validCameraKey));
        k.push_back(bindingKey("Keyboard", "Tank Left Track", [](SS& s) { return &s.tankKeyBindings[0]; }, false, validCameraKey));
        k.push_back(bindingKey("Keyboard", "Tank Right Track", [](SS& s) { return &s.tankKeyBindings[1]; }, false, validCameraKey));
        // [VR]
        auto vrChoice = [](const char* key, const char* comment, int Sly2VrOptions::*field, std::vector<names::Named> choices) {
            return Sly2Key{ "VR", key, comment,
                            [field, choices](const Sly2Settings& s) { return names::nameOf(choices, s.vr.*field); },
                            [field, choices](Sly2Settings& s, const std::string& v) {
                                int n = 0;
                                if (!names::valueOf(choices, v, n)) return false;
                                if (std::none_of(choices.begin(), choices.end(), [n](const names::Named& c) { return c.value == n; })) return false;
                                s.vr.*field = n;
                                return true;
                            } };
        };
        k.push_back(vrChoice("Mode", "Immersive (a 3D view from the game camera) or Flat Screen (the game on a big screen)",
                             &Sly2VrOptions::mode, { { VrMode_Immersive, "Immersive" }, { VrMode_Flat, "Flat Screen" }, { VrMode_Flat, "Flat" } }));
        k.push_back({ "VR", "Automatic Camera Rotation", "Immersive: On: the camera turns as you move. Off: only the right stick turns it",
                      [](const Sly2Settings& s) { return std::string(s.vr.autoCameraRotation ? "On" : "Off"); },
                      [](Sly2Settings& s, const std::string& v) { return names::parseBool(v, s.vr.autoCameraRotation); } });
        k.push_back(vrChoice("Comfort Vignette", "Off, Low, Medium or High: darker edges while the camera turns or moves",
                             &Sly2VrOptions::vignette, { { 0, "Off" }, { 1, "Low" }, { 2, "Medium" }, { 3, "High" } }));
        k.push_back(vrChoice("Screen Size", "Flat Screen: Small, Medium or Large", &Sly2VrOptions::screenSize,
                             { { 0, "Small" }, { 1, "Medium" }, { 2, "Large" } }));
        k.push_back(vrChoice("Refresh Rate", "Automatic (up to 120 Hz), 72, 80, 90 or 120", &Sly2VrOptions::refreshRate,
                             { { 0, "Automatic" }, { 72, "72" }, { 80, "80" }, { 90, "90" }, { 120, "120" } }));
        k.push_back(vrChoice("Eye Resolution",
                             "Immersive: Dynamic (100 % down to 75 % while the headset's frame rate is missed), 100, 90, 75, 60 or 50 (%)",
                             &Sly2VrOptions::eyeResolution,
                             { { 0, "Dynamic" }, { 100, "100" }, { 90, "90" }, { 75, "75" }, { 60, "60" }, { 50, "50" } }));
        k.push_back(vrChoice("Foveation", "Off, Low, Medium or High: the image's edges shaded more coarsely (fixed, no eye tracking)",
                             &Sly2VrOptions::foveation, { { 0, "Off" }, { 1, "Low" }, { 2, "Medium" }, { 3, "High" } }));
        k.push_back(vrChoice("MSAA", "2x or Off (the eyes' edges; Off is faster: on Vulkan one pass an eye)", &Sly2VrOptions::msaa,
                             { { 2, "2x" }, { 1, "Off" } }));
        k.push_back(vrChoice("Upscaling",
                             "Off, Sharpen or Super Resolution: the headset's compositor sharpens / upscales the eyes (Quest); "
                             "on, Dynamic eye resolution works between 90 and 75 %",
                             &Sly2VrOptions::upscaling, { { 0, "Off" }, { 1, "Sharpen" }, { 2, "Super Resolution" } }));
        k.push_back(vrChoice("Multiview", "On or Off: both eyes drawn in one pass where the device can (Vulkan); Off: the eyes one by one",
                             &Sly2VrOptions::multiview, { { 1, "On" }, { 0, "Off" } }));
        k.push_back({ "VR", "World Scale", "Immersive: 0.50 to 2.00 (1.00: life size)",
                      [](const Sly2Settings& s) { return names::decimal(s.vr.worldScale); },
                      [](Sly2Settings& s, const std::string& v) {
                          float g = 0;
                          if (!names::parseFloat(v, g) || g < 0.5f - 1e-4f || g > 2.0f + 1e-4f) return false;
                          s.vr.worldScale = std::clamp(g, 0.5f, 2.0f);
                          return true;
                      } });
        k.push_back(vrChoice("Cutscenes", "Immersive: Flat Screen (the cutscenes, the Episode 2 dance and the RC chopper on the screen) or Immersive",
                             &Sly2VrOptions::cutscenesFlat, { { 1, "Flat Screen" }, { 0, "Immersive" }, { 1, "Flat" } }));
        k.push_back({ "VR", "RC Car Table View", "Immersive, experimental: On: Episode 7's RC Combat Club seen from the table",
                      [](const Sly2Settings& s) { return std::string(s.vr.rcTable ? "On" : "Off"); },
                      [](Sly2Settings& s, const std::string& v) { return names::parseBool(v, s.vr.rcTable); } });
        return k;
    }();
    return schema;
}

// The sections in the file's order, with a line about each.
inline const std::vector<std::pair<const char*, const char*>>& sly2Sections() {
    static const std::vector<std::pair<const char*, const char*>> sections = {
        { "Display", nullptr },
        { "Graphics", nullptr },
        { "Audio", "Volumes are percentages." },
        { "Language", nullptr },
        { "Controls", nullptr },
        { "Controller", "Bindings: Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Select, Start, Guide,\n"
                        "Dpad Up, Dpad Down, Dpad Left, Dpad Right." },
        { "Mouse", "Inversion: Off is natural (mouse up looks up, mouse right turns right)." },
        { "Keyboard", "Keys by name (W, Space, Left Shift, Enter, F1, Keypad 0...), Mouse Left, Mouse Right, Mouse Middle,\n"
                      "Mouse 4-8, Wheel Up, Wheel Down, or None." },
        { "VR", "Used with a VR headset only (Meta Quest; on a PC: --vr with an OpenXR runtime)." },
    };
    return sections;
}

inline constexpr const char* kSly2MetaSection = "SafeHouse";

// ------------------------------------------------------------ text
inline std::string sly2ToIni(const Sly2Settings& s) {
    ini::Writer w;
    w.comment("SafeHouse - Sly 2: Band of Thieves settings.");
    w.comment("The game and the launcher both use this file. Lines starting with ; are comments.");
    w.comment("A value that cannot be read is replaced by its default.");
    const auto& schema = sly2Schema();
    auto writeUnknown = [&](const char* sectionName) {
        if (const ini::Section* u = s.unknown.section(sectionName))
            for (const auto& e : u->entries) w.value(e.key, e.value);
    };
    for (const auto& [section, about] : sly2Sections()) {
        w.section(section);
        if (about) w.comment(about);
        for (const auto& key : schema) {
            if (std::string_view(key.section) != section) continue;
            if (key.comment) w.comment(key.comment);
            w.value(key.key, key.write(s));
        }
        writeUnknown(section);
    }
    w.section(kSly2MetaSection);
    w.value("Version", "1");
    if (!s.pendingSaveOptions.empty()) {
        std::string list;
        for (const auto& p : s.pendingSaveOptions) list += (list.empty() ? "" : ", ") + p;
        w.value("Pending Save Options", list);
    }
    writeUnknown(kSly2MetaSection);
    for (const auto& u : s.unknown.sections) {
        if (u.entries.empty()) continue;
        bool known = ini::keyForm(u.name) == ini::keyForm(kSly2MetaSection);
        for (const auto& [section, about] : sly2Sections()) known |= ini::keyForm(u.name) == ini::keyForm(section);
        if (known) continue;
        if (u.name.empty()) w.section("Other");
        else w.section(u.name);
        for (const auto& e : u.entries) w.value(e.key, e.value);
    }
    return w.str();
}

// Reads INI text over the defaults (a key missing or invalid keeps its default). invalidOut: the keys whose value
// could not be read.
inline Sly2Settings sly2FromIni(std::string_view text, std::vector<std::string>* invalidOut = nullptr) {
    Sly2Settings s;
    const ini::Document doc = ini::parse(text);
    const auto& schema = sly2Schema();
    for (const auto& section : doc.sections) {
        const std::string sectionForm = ini::keyForm(section.name);
        for (const auto& e : section.entries) {
            const std::string keyForm = ini::keyForm(e.key);
            if (sectionForm == ini::keyForm(kSly2MetaSection)) {
                if (keyForm == "version") continue;
                if (keyForm == "pendingsaveoptions") {
                    s.pendingSaveOptions.clear();
                    std::string item;
                    for (size_t i = 0; i <= e.value.size(); ++i) {
                        if (i == e.value.size() || e.value[i] == ',') {
                            const std::string t = ini::trim(item);
                            for (const auto& known : allSaveOptions())
                                if (ini::keyForm(known) == ini::keyForm(t) && !pending(s, known)) s.pendingSaveOptions.push_back(known);
                            item.clear();
                        } else
                            item += e.value[i];
                    }
                    continue;
                }
            }
            const Sly2Key* match = nullptr;
            for (const auto& key : schema)
                if (ini::keyForm(key.section) == sectionForm && ini::keyForm(key.key) == keyForm) match = &key;
            if (!match) {
                s.unknown.set(section.name, e.key, e.value);
                continue;
            }
            Sly2Settings attempt = s;
            if (match->read(attempt, e.value)) s = std::move(attempt);
            else {
                // (the default: a key repeated later in the file still wins)
                const Sly2Settings defaults;
                std::string d = match->write(defaults);
                match->read(s, d);
                if (invalidOut) invalidOut->push_back(std::string(match->section) + "/" + match->key);
            }
        }
    }
    sanitizeStatic(s.system, 0, 0, kSly2DefaultKeyboardBindings);
    s.game.invertBinocucom = s.game.invertBinocucom != 0;
    s.game.invertTurret = s.game.invertTurret != 0;
    s.game.speakers = std::clamp(s.game.speakers, 0, 2);
    return s;
}

// ------------------------------------------------------------ the file
// Ok: Settings.ini read. Migrated: no INI; Saves/system_settings.dat was read into it (the caller writes the INI).
// Missing: neither exists (defaults). Unreadable: the INI exists but could not be opened (defaults; callers do not
// overwrite it).
enum class Sly2Load { Ok, Migrated, Missing, Unreadable };

inline Sly2Load loadSly2(const fs::path& userDir, Sly2Settings& out, std::vector<std::string>* invalidOut = nullptr) {
    out = Sly2Settings{};
    const fs::path file = sly2SettingsPath(userDir);
    std::error_code ec;
    if (fs::exists(file, ec)) {
        std::string text;
        if (!ini::readFile(file, text)) return Sly2Load::Unreadable;
        out = sly2FromIni(text, invalidOut);
        return Sly2Load::Ok;
    }
    // the old binary file (any version: system_settings.h load)
    SystemSettings old{};
    const LoadResult result = load(settingsPath(userDir), old);
    out.pendingSaveOptions = allSaveOptions();
    if (!loaded(result)) return Sly2Load::Missing;
    sanitizeStatic(old, 0, 0, kSly2DefaultKeyboardBindings);
    out.system = old;
    return Sly2Load::Migrated;
}

inline bool saveSly2(const fs::path& userDir, const Sly2Settings& s) {
    Sly2Settings copy = s;
    sanitizeStatic(copy.system, 0, 0, kSly2DefaultKeyboardBindings);
    return ini::writeFileAtomic(sly2SettingsPath(userDir), sly2ToIni(copy));
}

// The save-kept options of the newest save, taken in once (the migration's pending ones): a save's own value wins over
// the default; vibration needs both the old PC switch and the save's option on; the save's volumes (stored as 1 - the
// channel factor) multiply the old PC sliders (whose factor is the square root of the slider: sound.cpp).
struct SaveOptionValues {
    bool hasVibration = false, noVibration = false;
    bool hasMusic = false, hasEffects = false;
    float musicComplement = 0, effectsComplement = 0;
    bool hasSpeakers = false;
    int speakers = 0;
    bool hasInvertBinocucom = false, invertBinocucom = false;
    bool hasInvertTurret = false, invertTurret = false;
};
inline void applySaveOptions(Sly2Settings& s, const SaveOptionValues& v) {
    if (pending(s, kSaveOptionVibration) && v.hasVibration && v.noVibration) s.system.controllerVibration = 0;
    auto volume = [](float pc, float complement) {
        const float channel = std::clamp(1.0f - complement, 0.0f, 1.0f);
        return std::clamp(pc * channel * channel, 0.0f, 1.0f);
    };
    if (pending(s, kSaveOptionMusic) && v.hasMusic) s.system.musicVolume = volume(s.system.musicVolume, v.musicComplement);
    if (pending(s, kSaveOptionEffects) && v.hasEffects) s.system.soundEffectsVolume = volume(s.system.soundEffectsVolume, v.effectsComplement);
    if (pending(s, kSaveOptionSpeakers) && v.hasSpeakers && v.speakers >= 0 && v.speakers <= 2) s.game.speakers = v.speakers;
    if (pending(s, kSaveOptionInvertBinocucom) && v.hasInvertBinocucom) s.game.invertBinocucom = v.invertBinocucom;
    if (pending(s, kSaveOptionInvertTurret) && v.hasInvertTurret) s.game.invertTurret = v.invertTurret;
    s.pendingSaveOptions.clear();
}
// A save's stored complement for a volume (the original's "uMusicVolume" / "uEffectsVolume": 1 - the channel factor),
// so that the PS2 or an older build plays a save at the same level: the factor is the square root of the volume.
inline float saveComplement(float volume) { return 1.0f - std::sqrt(std::clamp(volume, 0.0f, 1.0f)); }
} // namespace cane::settings
