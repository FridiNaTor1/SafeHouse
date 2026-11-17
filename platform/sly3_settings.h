#pragma once
// Sly 3's settings file: <user-dir>/Settings.ini (the launcher's <profile>/sly3), the same commented INI format as
// Sly 2's (sly2_settings.h: comments, forgiving key spellings, defaults for unreadable values, unknown keys kept,
// atomic writes) with Sly 3's own schema. The game (Sly3-Release display.cpp, menu.cpp) and the launcher's Settings
// page read and write it through this one table. Sly3-Release/notes/QOL3_2026-11-10.md.
//
// The schema is Sly 2's keys that Sly 3 has (the PC options and the original options both games share: vibration,
// speakers, music / effects volume, USB Headset, the audio devices, the language: the PS3 import's French / Spanish)
// without Sly 2's own: the original's Invert Binocucom / Invert Turret (Sly 3's Camera Options instead), the
// keyboard's tank tracks, the hacking minigame's mouse aim and the VR section (no VR for Sly 3 yet).
//
// Migration: with no Settings.ini the old Saves/system_settings.dat is read once (as for Sly 2). Sly 3 never had a
// playable save to take options from, so there are no pending save options.
#include <string_view>
#include "sly2_settings.h"

namespace cane::settings {

inline bool sly3HasKey(std::string_view section, std::string_view key) {
    if (section == "VR") return false;
    if (section == "Display" && key.substr(0, 9) == "Upscaling") return false;   // (Sly 2's renderer only, for now)
    if (section == "Language" && key == "Voices") return false;   // (Sly 3: English voices only, nothing to choose)
    if (section == "Controller" && (key == kSaveOptionInvertBinocucom || key == kSaveOptionInvertTurret)) return false;
    if (section == "Keyboard" && (key == "Tank Left Track" || key == "Tank Right Track")) return false;
    if (section == "Mouse" && key == "Mouse Aim In Hacking") return false;
    if (section == "Graphics" && key == "Texture Streaming") return false;   // (Sly 2's renderer only, for now)
    if (section == "Graphics" && key == "Outline Thickness") return false;   // (Sly 2's renderer only, for now)
    if (section == "Display" && key == "HUD Placement") return false;   // (Sly 2's HUD only, for now)
    return true;
}

// Sly 3's Camera Options pages (the menu's 0x88..0x8E, one per camera kind: orig3 0x345F90), each with Left/Right
// and Up/Down Default / Reversed (0x8F..0x92). The PS2 kept them in each save; here in [Controller].
inline constexpr const char* kSly3CameraKinds[7] = { "Camera", "Binocucom", "Grapple-Cam", "Turret", "Strafe", "Swimming", "Harpoon" };

inline const std::vector<Sly2Key>& sly3Schema() {
    static const std::vector<Sly2Key> schema = [] {
        std::vector<Sly2Key> k;
        for (const Sly2Key& key : sly2Schema())
            if (sly3HasKey(key.section, key.key)) k.push_back(key);
        static std::vector<std::string> names;   // (the keys' storage: Sly2Key holds pointers)
        names.reserve(14);
        for (int kind = 0; kind < 7; ++kind)
            for (int axis = 0; axis < 2; ++axis) {
                names.push_back(std::string(kSly3CameraKinds[kind]) + (axis ? " Up/Down" : " Left/Right"));
                k.push_back({ "Controller", names.back().c_str(),
                              kind == 0 && axis == 0 ? "The original's Camera Options for each camera: Default or Reversed" : nullptr,
                              [kind, axis](const Sly2Settings& s) { return std::string(s.game.camReverse[kind][axis] ? "Reversed" : "Default"); },
                              [kind, axis](Sly2Settings& s, const std::string& v) {
                                  const std::string f = ini::keyForm(v);
                                  if (f == "reversed" || f == "on") return s.game.camReverse[kind][axis] = 1, true;
                                  if (f == "default" || f == "off") return s.game.camReverse[kind][axis] = 0, true;
                                  return false;
                              } });
            }
        return k;
    }();
    return schema;
}

inline const std::vector<std::pair<const char*, const char*>>& sly3Sections() {
    static const std::vector<std::pair<const char*, const char*>> sections = [] {
        std::vector<std::pair<const char*, const char*>> v;
        for (const auto& section : sly2Sections())
            if (sly3HasKey(section.first, "")) v.push_back(section);
        return v;
    }();
    return sections;
}

inline const GameIniLayout& sly3Layout() {
    static const GameIniLayout layout{ sly3Schema, sly3Sections, "SafeHouse - Sly 3: Honor Among Thieves settings.", false };
    return layout;
}

using Sly3Settings = Sly2Settings;
inline fs::path sly3SettingsPath(const fs::path& userDir) { return sly2SettingsPath(userDir); }
inline std::string sly3ToIni(const Sly3Settings& s) { return gameToIni(s, sly3Layout()); }
inline Sly3Settings sly3FromIni(std::string_view text, std::vector<std::string>* invalidOut = nullptr) {
    return gameFromIni(text, sly3Layout(), invalidOut);
}
inline Sly2Load loadSly3(const fs::path& userDir, Sly3Settings& out, std::vector<std::string>* invalidOut = nullptr) {
    return loadGameIni(userDir, sly3Layout(), out, invalidOut);
}
inline bool saveSly3(const fs::path& userDir, const Sly3Settings& s) { return saveGameIni(userDir, sly3Layout(), s); }

} // namespace cane::settings
