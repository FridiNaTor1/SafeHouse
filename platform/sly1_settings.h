#pragma once
// Sly 1's settings file: <user-dir>/Settings.ini (the launcher's Sly 1 profile folder), the same commented INI format as
// Sly 2's and Sly 3's (sly2_settings.h: comments, forgiving key spellings, defaults for unreadable values, unknown keys
// kept, atomic writes) with Sly 1's own schema. The game (the Sly1/ submodule: Sly1-Release/safehouse/settings.cpp, which
// maps this file onto the game's own VIDEOSAVESETTINGS) and the launcher's Settings page read and write it through this
// one table.
//
// The schema: the keys of Sly 2's that Sly 1 has (display, graphics, volumes, controls, bindings, mouse) and Sly 1's
// own: its PC menu's options the other games lack (Sly1Options: brightness, texture filtering, resolution scale,
// minimize when unfocused, the sticks' dead zone), its Fog, GUI Style / Scale,
// Anisotropic Filtering, Speakers (Stereo / Mono) and the save slot the title screen shows.
//
// Updates of older files (sly1Upgrade, on every load): the default keyboard layout's revision 2 (Sly 2's keys action
// for action, system_settings.h kSly1DefaultKeyboardBindings) moves a binding still on revision 1's default to the new
// one (L1 1 -> right mouse, R3 right mouse -> middle mouse), never a binding the player changed, nor onto a key another
// binding uses; and the removed Dialogue Skip option (L1 / R1 skipped binocucom lines; now every conversation takes
// the cutscenes' hold-to-skip) turns Instant Skip Cutscenes on when it was on; the old Windows-only [Audio] Output
// Device ID (an endpoint's id) is kept for the game (s.sly1.audioDeviceId), which turns it into Output Device (Sly 2's
// key: the device's name, any desktop) on Windows and drops it.
//
// Migration (once): with no Settings.ini the old Saves/system_settings.dat (SafeHouse's, any version: system_settings.h)
// is read and the INI written; the game also reads ProjectCane's own file there (its v15 layout, which SafeHouse's
// reader does not know: Sly1-Release/save.cpp). The old file is left in place. Sly 1's saves never carried options.
#include <string_view>
#include "sly2_settings.h"

namespace cane::settings {

inline bool sly1HasKey(std::string_view section, std::string_view key) {
    if (section == "VR" || section == "Language") return false;   // (no VR; the language: content_settings.json)
    if (key.empty()) return true;   // (a section)
    if (section == "Display")
        return key.rfind("Upscaling", 0) != 0;
    if (section == "Graphics")
        return key != "Long Shadows" && key != "Fast Shadows" && key != "Outline Thickness" &&
               key != "Texture Cache" && key != "Texture Streaming";
    if (section == "Audio")
        return key == kSaveOptionMusic || key == kSaveOptionEffects || key == "Dialogue Volume" || key == "Output Device";
    if (section == "Controller")
        return key != "Invert Camera Horizontal" && key != "Invert Camera Vertical" && key != kSaveOptionInvertBinocucom &&
               key != kSaveOptionInvertTurret;
    if (section == "Mouse")
        return key != "Mouse Aim In Hacking" && key.find("Binocucom") == std::string_view::npos &&
               key.find("Turret") == std::string_view::npos;
    if (section == "Keyboard") return key != "Tank Left Track" && key != "Tank Right Track";
    return true;
}

// Sly 1's internal resolutions: the shared list and the PS2's own 448 lines.
inline constexpr int kSly1Ps2Height = 448;
// The default keyboard layout's revision (system_settings.h kSly1DefaultKeyboardBindings); 1: kSly2DefaultKeyboardBindings.
inline constexpr int kSly1KeyboardRevision = 2;

namespace detail {
inline Sly2Key sly1BoolKey(const char* section, const char* key, const char* comment, int Sly1Options::*field) {
    return { section, key, comment, [field](const Sly2Settings& s) { return std::string(s.sly1.*field ? "On" : "Off"); },
             [field](Sly2Settings& s, const std::string& v) { return names::parseBool(v, s.sly1.*field); } };
}
// A percentage (whole numbers) over a float field: 100 = 1.0.
inline Sly2Key sly1PercentKey(const char* section, const char* key, const char* comment, float Sly1Options::*field, int minimum,
                              int maximum) {
    return { section, key, comment,
             [field](const Sly2Settings& s) { return std::to_string(int(std::lround(s.sly1.*field * 100.0f))); },
             [field, minimum, maximum](Sly2Settings& s, const std::string& v) {
                 std::string t = ini::trim(v);
                 if (!t.empty() && t.back() == '%') t.pop_back();
                 int n = 0;
                 if (!names::parseInt(ini::trim(t), n) || n < minimum || n > maximum) return false;
                 s.sly1.*field = float(n) / 100.0f;
                 return true;
             } };
}
} // namespace detail

inline const std::vector<Sly2Key>& sly1Schema() {
    using namespace detail;
    static const std::vector<Sly2Key> schema = [] {
        std::vector<Sly2Key> k;
        for (const Sly2Key& key : sly2Schema()) {
            const std::string_view section = key.section, name = key.key;
            // (Sly 1's versions of shared keys, in their place)
            if (section == "Graphics" && name == "Internal Resolution") {
                k.push_back({ "Graphics", "Internal Resolution", "Native, 360, 448 (the PS2's), 480, 720, 1080, 1440 or 2160 (the 3D scene's height)",
                              [](const Sly2Settings& s) {
                                  return s.system.internalResolutionHeight ? std::to_string(s.system.internalResolutionHeight) : std::string("Native");
                              },
                              [](Sly2Settings& s, const std::string& v) {
                                  if (ini::keyForm(v) == "native") return s.system.internalResolutionHeight = 0, true;
                                  std::string t = ini::trim(v);
                                  if (!t.empty() && (t.back() == 'p' || t.back() == 'P')) t.pop_back();
                                  int n = 0;
                                  if (!names::parseInt(t, n)) return false;
                                  if (n != kSly1Ps2Height &&
                                      std::find(std::begin(kInternalResolutions), std::end(kInternalResolutions), n) == std::end(kInternalResolutions))
                                      return false;
                                  s.system.internalResolutionHeight = n;
                                  return true;
                              } });
                continue;
            }
            if (section == "Controls" && name == "Instant Skip Cutscenes") {
                k.push_back(boolKey("Controls", "Instant Skip Cutscenes", "On: one press of Circle skips a cutscene. Off: hold it 2 seconds",
                                    &SS::instantSkipCutscenes));
                continue;
            }
            if (!sly1HasKey(section, name)) continue;
            k.push_back(key);
            // (None is a value: a binding left empty)
            if (section == "Keyboard" && std::find(kSly2BindingKeys.begin(), kSly2BindingKeys.end(), name) != kSly2BindingKeys.end()) {
                const auto slot = static_cast<size_t>(std::find(kSly2BindingKeys.begin(), kSly2BindingKeys.end(), name) - kSly2BindingKeys.begin());
                k.back().read = [read = key.read, slot](Sly2Settings& s, const std::string& v) {
                    if (ini::keyForm(v) == "none") return s.system.keyboardBindings[slot] = kKeyUnknown, true;
                    return read(s, v);
                };
            }
            // (Sly 1's own keys after the shared one they belong with)
            if (section == "Display" && name == "Frame Rate Limit")
                k.push_back(sly1BoolKey("Display", "Minimize When Unfocused", "On: a fullscreen game minimizes when another window takes the focus",
                                        &Sly1Options::autoIconify));
            if (section == "Graphics" && name == "Anti-Aliasing") {
                k.push_back({ "Graphics", "Resolution Scale", "50 to 200 (%): the 3D scene's size with Internal Resolution Native",
                              [](const Sly2Settings& s) { return std::to_string(s.sly1.resolutionScale); },
                              [](Sly2Settings& s, const std::string& v) {
                                  int n = 0;
                                  if (!names::parseInt(v, n) || n < 50 || n > 200) return false;
                                  s.sly1.resolutionScale = n;
                                  return true;
                              } });
            }
            if (section == "Graphics" && name == "Draw Distance") {
                k.push_back(choiceKey("Graphics", "Fog", "PS2, PS3 or Off", &SS::fogType,
                                      { { Fog_PS2, "PS2" }, { Fog_PS3, "PS3" }, { Fog_Off, "Off" } }));
                k.push_back(sly1PercentKey("Graphics", "Brightness", "50 to 150 (%): the picture's gamma; 100 is the original's",
                                           &Sly1Options::gamma, 50, 150));
                k.push_back({ "Graphics", "Texture Filtering", "Trilinear or Bilinear",
                              [](const Sly2Settings& s) { return std::string(s.sly1.textureFiltering ? "Trilinear" : "Bilinear"); },
                              [](Sly2Settings& s, const std::string& v) {
                                  const std::string f = ini::keyForm(v);
                                  if (f == "trilinear") return s.sly1.textureFiltering = 1, true;
                                  if (f == "bilinear") return s.sly1.textureFiltering = 0, true;
                                  return false;
                              } });
                k.push_back(choiceKey("Graphics", "Anisotropic Filtering", "Off, 2x, 4x, 8x or 16x", &SS::anisotropy,
                                      { { 1, "Off" }, { 2, "2x" }, { 4, "4x" }, { 8, "8x" }, { 16, "16x" } }));
                k.push_back(choiceKey("Graphics", "GUI Style", "PS2 (the original's menus) or Modern", &SS::guiStyle,
                                      { { Gui_PS2, "PS2" }, { Gui_Modern, "Modern" } }));
                k.push_back({ "Graphics", "GUI Scale", "PS2 style: 75 or 100 (%); Modern style: 100 or 125",
                              [](const Sly2Settings& s) { return std::to_string(int(std::lround(s.system.guiScale * 100.0f))); },
                              [](Sly2Settings& s, const std::string& v) {
                                  std::string t = ini::trim(v);
                                  if (!t.empty() && t.back() == '%') t.pop_back();
                                  int n = 0;
                                  if (!names::parseInt(ini::trim(t), n) || (n != 75 && n != 100 && n != 125)) return false;
                                  s.system.guiScale = float(n) / 100.0f;   // (sanitizeStatic couples it to the style)
                                  return true;
                              } });
            }
            if (section == "Audio" && name == "Dialogue Volume") {
                k.push_back({ "Audio", "Speakers", "Stereo or Mono (both channels mixed together)",
                              [](const Sly2Settings& s) { return std::string(s.system.stereoEnabled ? "Stereo" : "Mono"); },
                              [](Sly2Settings& s, const std::string& v) {
                                  const std::string f = ini::keyForm(v);
                                  if (f == "stereo" || f == "surround") return s.system.stereoEnabled = 1, true;
                                  if (f == "mono") return s.system.stereoEnabled = 0, true;
                                  return false;
                              } });
            }
            if (section == "Keyboard" && name == "Camera Down") {
                k.push_back({ "Keyboard", "Layout Revision", "The default layout these keys were made with (the game updates the keys left\n"
                                                            "on an older layout's defaults once)",
                              [](const Sly2Settings&) { return std::to_string(kSly1KeyboardRevision); },
                              [](Sly2Settings& s, const std::string& v) {
                                  int n = 0;
                                  if (!names::parseInt(v, n) || n < 1) return false;
                                  s.sly1.keyboardRevision = n;
                                  return true;
                              } });
                // (the rhythm minigame's own keys: new keys, so an older file simply gets the defaults; no layout revision)
                static const char* const rhythmKeys[kSly1RhythmKeyCount] = { "Rhythm Triangle", "Rhythm Square", "Rhythm Circle",
                                                                            "Rhythm Cross" };
                for (int i = 0; i < kSly1RhythmKeyCount; ++i)
                    k.push_back({ "Keyboard", rhythmKeys[i],
                                  i == 0 ? "The rhythm minigame's (A Deadly Dance) own keys for its buttons, used only while it\n"
                                           "takes the input (keys only, or None)"
                                         : nullptr,
                                  [i](const Sly2Settings& s) { return names::nameOf(names::keys(), s.sly1.rhythmKeys[size_t(i)]); },
                                  [i](Sly2Settings& s, const std::string& v) {
                                      if (ini::keyForm(v) == "none") return s.sly1.rhythmKeys[size_t(i)] = kKeyUnknown, true;
                                      int n = 0;
                                      if (!names::valueOf(names::keys(), v, n) || !validCameraKey(n)) return false;
                                      s.sly1.rhythmKeys[size_t(i)] = n;
                                      return true;
                                  } });
            }
        }
        // [Controller] the sticks' dead zone (after the bindings)
        auto at = std::find_if(k.begin(), k.end(), [](const Sly2Key& key) { return std::string_view(key.section) == "Mouse"; });
        k.insert(at, sly1PercentKey("Controller", "Dead Zone", "5 to 50 (%): how far a stick moves before it counts",
                                    &Sly1Options::deadZone, 5, 50));
        // [Game] what the game keeps for itself
        k.push_back({ "Game", "Last Save Slot", "The save slot the title screen continues (1 to 6, or None); the game keeps it",
                      [](const Sly2Settings& s) { return s.system.lastSaveSlot < 0 ? std::string("None") : std::to_string(s.system.lastSaveSlot + 1); },
                      [](Sly2Settings& s, const std::string& v) {
                          if (ini::keyForm(v) == "none") return s.system.lastSaveSlot = -1, true;
                          int n = 0;
                          if (!names::parseInt(v, n) || n < 1 || n > kSaveSlotCount) return false;
                          s.system.lastSaveSlot = n - 1;
                          return true;
                      } });
        return k;
    }();
    return schema;
}

inline const std::vector<std::pair<const char*, const char*>>& sly1Sections() {
    static const std::vector<std::pair<const char*, const char*>> sections = [] {
        std::vector<std::pair<const char*, const char*>> v;
        for (const auto& section : sly2Sections())
            if (sly1HasKey(section.first, "")) v.push_back(section);
        v.push_back({ "Game", nullptr });
        return v;
    }();
    return sections;
}

// Sly 1's defaults: the shared defaults() with Sly 1's keyboard / mouse layout (kSly1DefaultKeyboardBindings, Sly 2's
// action for action: the game's own default, Sly1-Release/Input.cpp; a migrated system_settings.dat keeps its bindings,
// updated by sly1Upgrade).
inline Sly2Settings sly1Defaults() {
    Sly2Settings s;
    s.system = defaults();
    std::copy(std::begin(kSly1DefaultKeyboardBindings), std::end(kSly1DefaultKeyboardBindings), s.system.keyboardBindings);
    return s;
}

// A file's bindings made with an older default layout (keyboardRevision below kSly1KeyboardRevision): each binding still
// on that layout's default moves to the new default, unless another binding already uses the new key.
inline void sly1UpgradeKeyboard(int (&keys)[kBindingCount], int revision) {
    if (revision >= kSly1KeyboardRevision) return;
    auto used = [&](int key, int except) {
        for (int i = 0; i < kBindingCount; ++i)
            if (i != except && keys[i] == key) return true;
        return false;
    };
    // (R3 first: the right mouse button it leaves is L1's new key)
    for (int slot : { 15 /*R3*/, 10 /*L1*/ })
        if (keys[slot] == kSly2DefaultKeyboardBindings[slot] && !used(kSly1DefaultKeyboardBindings[slot], slot))
            keys[slot] = kSly1DefaultKeyboardBindings[slot];
}
// Every load: the older layouts' keys and the removed options (see the top of this file).
inline void sly1Upgrade(Sly2Settings& s) {
    sly1UpgradeKeyboard(s.system.keyboardBindings, s.sly1.keyboardRevision);
    s.sly1.keyboardRevision = kSly1KeyboardRevision;
    // (the old Output Device ID stays in the file, among the unknown keys, until the game has turned it into a name)
    s.sly1.audioDeviceId.clear();
    if (ini::Section* audio = s.unknown.section("Audio")) {
        auto& e = audio->entries;
        for (auto it = e.begin(); it != e.end();) {
            if (ini::keyForm(it->key) != ini::keyForm("Output Device ID")) { ++it; continue; }
            const std::string id = audio::nameFromIni(it->value);
            if (id.empty() || !s.game.outputDevice.empty()) { it = e.erase(it); continue; }
            s.sly1.audioDeviceId = id;
            ++it;
        }
    }
    if (ini::Section* controls = s.unknown.section("Controls")) {
        auto& e = controls->entries;
        for (auto it = e.begin(); it != e.end();) {
            if (ini::keyForm(it->key) == ini::keyForm("Dialogue Skip")) {
                int on = 0;
                if (names::parseBool(it->value, on) && on) s.system.instantSkipCutscenes = 1;
                it = e.erase(it);
            } else
                ++it;
        }
    }
}

// (the PS2's 448 lines, which the shared clamps do not know)
inline void sly1Fixup(Sly2Settings& s, const SystemSettings& before) {
    if (before.internalResolutionHeight == kSly1Ps2Height) s.system.internalResolutionHeight = kSly1Ps2Height;
    s.sly1.autoIconify = s.sly1.autoIconify != 0;
    s.sly1.textureFiltering = s.sly1.textureFiltering != 0;
    s.sly1.resolutionScale = std::clamp(s.sly1.resolutionScale, 50, 200);
    if (!std::isfinite(s.sly1.gamma)) s.sly1.gamma = 1.0f;
    s.sly1.gamma = std::clamp(s.sly1.gamma, 0.5f, 1.5f);
    if (!std::isfinite(s.sly1.deadZone)) s.sly1.deadZone = 0.35f;
    s.sly1.deadZone = std::clamp(s.sly1.deadZone, 0.05f, 0.5f);
}

inline const GameIniLayout& sly1Layout() {
    static const GameIniLayout layout{ sly1Schema, sly1Sections, "SafeHouse - Sly Cooper and the Thievius Raccoonus settings.", false,
                                       sly1Defaults, sly1Fixup };
    return layout;
}

using Sly1Settings = Sly2Settings;
inline fs::path sly1SettingsPath(const fs::path& userDir) { return sly2SettingsPath(userDir); }
inline std::string sly1ToIni(const Sly1Settings& s) { return gameToIni(s, sly1Layout()); }
inline Sly1Settings sly1FromIni(std::string_view text, std::vector<std::string>* invalidOut = nullptr) {
    Sly1Settings s = gameFromIni(text, sly1Layout(), invalidOut);
    sly1Upgrade(s);
    return s;
}
inline Sly2Load loadSly1(const fs::path& userDir, Sly1Settings& out, std::vector<std::string>* invalidOut = nullptr) {
    const Sly2Load result = loadGameIni(userDir, sly1Layout(), out, invalidOut);
    sly1Upgrade(out);   // (a migrated system_settings.dat too: its keys are revision 1's; the defaults are already new)
    return result;
}
inline bool saveSly1(const fs::path& userDir, const Sly1Settings& s) { return saveGameIni(userDir, sly1Layout(), s); }

} // namespace cane::settings
