#pragma once
// Sly 1's advanced input mapping (the game's Controls page; the launcher can read the same table): Sly 1's actions,
// each bindable to its own key / mouse button and controller button, in the format of Sly 2's
// (sly2_action_bindings.h): <user-dir>/Saves/advanced_controls.json, {"version": 1, "revision": 2, "enabled": bool,
// "actions": {id: {"keyboard": code, "controller": code}}}, actions by stable string id so profiles survive
// actions being added or reordered. "revision" 2: the keyboard defaults of system_settings.h
// kSly1DefaultKeyboardBindings (Sly 2's keys action for action); a revision 1 file's Talk to Bentley key still on 1 moves
// to the right mouse button and Centre the camera's still on the right mouse button to the middle one (as Sly 2's
// revision 2 moved hack_fire), unless another action already uses the new key. Codes are the settings' (system_settings.h: GLFW keys, kMouseBindingBase..
// mouse buttons and the wheel; GLFW gamepad buttons, 1000 / 1001 the triggers; -1 none). "revision" 3: the rhythm
// minigame's actions default to its own keys (system_settings.h kSly1DefaultRhythmKeys); see upgradeKeyboard.
//
// How the game uses it (the fork's Sly1-Release/actionbind.cpp): with the mapping on, each of the PS2 pad's buttons
// is held when one of the actions on that button is held in the current context. An action belongs to every
// context (Context::Any) or to one vehicle / minigame; in a vehicle or minigame its own actions replace the common
// ones on the same button (e.g. in the turret, "Fire" decides Square instead of "Attack"). Off: the standard 16
// bindings (system_settings.h keyboardBindings / gamepadBindings).
#include "system_settings.h"
#include <array>
#include <cstddef>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace cane::sly1input {

// The PS2 pad's buttons in the game's order (Sly1-Release Input.h JOY_BUTTON, the settings' binding order).
enum Pad : int { Up, Down, Left, Right, Square, Circle, Cross, Triangle, Start, Select, L1, R1, L2, R2, L3, R3 };

// Where an action applies: everywhere, or while that vehicle / minigame is the player (spaprops.h CID: TURRET 22,
// SUV 25 Murray's van, TANK 20, PUFFER 24, JETPACK 27, RYTHM 28 the rhythm minigame, VAULT 23 the safe).
enum class Context : int { Any, Turret, Van, Tank, Puffer, Jetpack, Rhythm, Vault, Count };

// name, stable id, label, group, PS2 button, context. The defaults are the standard bindings of that button.
#define CANE_SLY1_ACTIONS(X) \
 X(MoveUp, "move_up", "Move forward", "Movement", Up, Any) \
 X(MoveDown, "move_down", "Move backward", "Movement", Down, Any) \
 X(MoveLeft, "move_left", "Move left", "Movement", Left, Any) \
 X(MoveRight, "move_right", "Move right", "Movement", Right, Any) \
 X(Jump, "jump", "Jump", "Sly", Cross, Any) \
 X(Attack, "attack", "Attack", "Sly", Square, Any) \
 X(Action, "action", "Action (sneak, grab, use)", "Sly", Circle, Any) \
 X(PowerUp, "power_up", "Use power-up", "Sly", Triangle, Any) \
 X(PowerUpPrevious, "power_up_previous", "Previous power-up", "Sly", L2, Any) \
 X(PowerUpNext, "power_up_next", "Next power-up", "Sly", R2, Any) \
 X(Bentley, "bentley", "Talk to Bentley (binocucom)", "Sly", L1, Any) \
 X(SkipDialogue, "skip_dialogue", "Skip dialogue", "Sly", R1, Any) \
 X(CameraCenter, "camera_center", "Centre the camera", "Camera", R3, Any) \
 X(L3Button, "l3", "L3", "Camera", L3, Any) \
 X(Pause, "pause", "Pause", "Menus", Start, Any) \
 X(Map, "map", "Map", "Menus", Select, Any) \
 X(TurretFire, "turret_fire", "Turret: fire", "Vehicles and minigames", Square, Turret) \
 X(TurretFire2, "turret_fire_2", "Turret: fire (second button)", "Vehicles and minigames", Cross, Turret) \
 X(VanAttack, "van_attack", "Murray's van: attack", "Vehicles and minigames", Square, Van) \
 X(TankExit, "tank_exit", "Tank: get out", "Vehicles and minigames", Cross, Tank) \
 X(PufferFire, "puffer_fire", "Puffer: fire", "Vehicles and minigames", Cross, Puffer) \
 X(PufferFire2, "puffer_fire_2", "Puffer: fire (second button)", "Vehicles and minigames", Square, Puffer) \
 X(JetpackThrust, "jetpack_thrust", "Jetpack: thrust", "Vehicles and minigames", Square, Jetpack) \
 X(JetpackCharm, "jetpack_charm", "Jetpack: use a lucky charm", "Vehicles and minigames", Cross, Jetpack) \
 X(RhythmSquare, "rhythm_square", "Rhythm: Square", "Rhythm minigame", Square, Rhythm) \
 X(RhythmTriangle, "rhythm_triangle", "Rhythm: Triangle", "Rhythm minigame", Triangle, Rhythm) \
 X(RhythmCircle, "rhythm_circle", "Rhythm: Circle", "Rhythm minigame", Circle, Rhythm) \
 X(RhythmCross, "rhythm_cross", "Rhythm: Cross", "Rhythm minigame", Cross, Rhythm) \
 X(VaultSquare, "vault_square", "Safe: Square", "Vehicles and minigames", Square, Vault) \
 X(VaultTriangle, "vault_triangle", "Safe: Triangle", "Vehicles and minigames", Triangle, Vault) \
 X(VaultCircle, "vault_circle", "Safe: Circle", "Vehicles and minigames", Circle, Vault) \
 X(VaultCross, "vault_cross", "Safe: Cross", "Vehicles and minigames", Cross, Vault)

enum class Action : int {
#define CANE_SLY1_ACTION_ENUM(name, id, label, group, pad, context) name,
 CANE_SLY1_ACTIONS(CANE_SLY1_ACTION_ENUM)
#undef CANE_SLY1_ACTION_ENUM
 Count
};
inline constexpr size_t kActionCount = size_t(Action::Count);

struct ActionInfo {
    const char* id;
    const char* label;
    const char* group;
    int pad;          // the PS2 button (Pad) the action presses
    Context context;
    int defaultKey;   // the standard keyboard binding of that button
    int defaultPad;   // the standard controller binding of that button
};
// An action's default key: its button's standard key, but the rhythm minigame's own keys (system_settings.h
// kSly1DefaultRhythmKeys: W, A, D, Space) for its four buttons.
inline constexpr int defaultKeyFor(Context context, int pad) {
    if (context == Context::Rhythm && settings::sly1RhythmSlot(pad) >= 0)
        return settings::kSly1DefaultRhythmKeys[settings::sly1RhythmSlot(pad)];
    return settings::kSly1DefaultKeyboardBindings[pad];
}
inline constexpr std::array<ActionInfo, kActionCount> kActions{{
#define CANE_SLY1_ACTION_INFO(name, id, label, group, pad, context) \
 {id, label, group, Pad::pad, Context::context, defaultKeyFor(Context::context, Pad::pad), \
  settings::kDefaultGamepadBindings[Pad::pad]},
 CANE_SLY1_ACTIONS(CANE_SLY1_ACTION_INFO)
#undef CANE_SLY1_ACTION_INFO
}};
#undef CANE_SLY1_ACTIONS

// The common actions cover every button once (so the mapping on with the defaults plays as the standard bindings).
inline constexpr bool everyButtonOnce() {
    for (int pad = 0; pad < settings::kBindingCount; ++pad) {
        int n = 0;
        for (const ActionInfo& a : kActions) n += a.context == Context::Any && a.pad == pad;
        if (n != 1) return false;
    }
    return true;
}
static_assert(everyButtonOnce());

struct Profile {
    bool enabled = false;
    bool readOnly = false;   // a newer file version: shown, never written
    std::array<int, kActionCount> keyboard{}, gamepad{};
};

inline Profile defaults() {
    Profile p;
    for (size_t i = 0; i < kActionCount; ++i) {
        p.keyboard[i] = kActions[i].defaultKey;
        p.gamepad[i] = kActions[i].defaultPad;
    }
    return p;
}
// The context's actions decide its buttons; the common action on such a button is not used there.
inline bool applies(const Profile&, Action action, Context current) {
    const ActionInfo& a = kActions[size_t(action)];
    if (a.context == current) return true;
    if (a.context != Context::Any) return false;
    for (const ActionInfo& other : kActions)
        if (other.context == current && current != Context::Any && other.pad == a.pad) return false;
    return true;
}

inline fs::path profilePath(const fs::path& userDir) { return userDir / "Saves" / "advanced_controls.json"; }

inline constexpr int kProfileRevision = 3;
// A revision 1 profile's keys still on revision 1's defaults (kSly2DefaultKeyboardBindings) move to the new defaults;
// revision 3: the rhythm minigame's actions still on their button's standard key (either layout's) move to the
// minigame's own keys (W, A, D, Space).
inline void upgradeKeyboard(Profile& p, int revision) {
    if (revision >= kProfileRevision) return;
    for (size_t i = 0; i < kActionCount; ++i) {
        const ActionInfo& a = kActions[i];
        if (a.context != Context::Rhythm) continue;
        if (p.keyboard[i] == settings::kSly1DefaultKeyboardBindings[a.pad] || p.keyboard[i] == settings::kSly2DefaultKeyboardBindings[a.pad])
            p.keyboard[i] = a.defaultKey;
    }
    if (revision >= 2) return;
    auto used = [&](int key, size_t except) {
        for (size_t i = 0; i < kActionCount; ++i)
            if (i != except && kActions[i].context == Context::Any && p.keyboard[i] == key) return true;
        return false;
    };
    for (Action a : { Action::CameraCenter, Action::Bentley }) {
        const size_t i = size_t(a);
        const int old = settings::kSly2DefaultKeyboardBindings[kActions[i].pad];
        if (p.keyboard[i] == old && old != kActions[i].defaultKey && !used(kActions[i].defaultKey, i))
            p.keyboard[i] = kActions[i].defaultKey;
    }
}

inline Profile load(const fs::path& userDir) {
    Profile p = defaults();
    std::ifstream in(profilePath(userDir));
    if (!in) return p;
    try {
        const auto json = nlohmann::json::parse(in);
        if (json.value("version", 0) != 1) { p.readOnly = true; return p; }
        p.enabled = json.value("enabled", false);
        const int revision = json.value("revision", 1);
        if (json.contains("actions") && json["actions"].is_object()) {
            const auto& actions = json["actions"];
            for (size_t i = 0; i < kActionCount; ++i) {
                const auto it = actions.find(kActions[i].id);
                if (it == actions.end() || !it->is_object()) continue;
                const int key = it->value("keyboard", p.keyboard[i]);
                const int pad = it->value("controller", p.gamepad[i]);
                if (key == settings::kKeyUnknown || settings::validKeyboardBinding(key)) p.keyboard[i] = key;
                if (pad == settings::kKeyUnknown || settings::validGamepadBinding(pad)) p.gamepad[i] = pad;
            }
        }
        upgradeKeyboard(p, revision);
    } catch (const nlohmann::json::exception&) { p = defaults(); p.readOnly = true; }
    return p;
}

inline bool save(const fs::path& userDir, const Profile& p) {
    if (p.readOnly) return false;
    nlohmann::json json{{"version", 1}, {"revision", kProfileRevision}, {"enabled", p.enabled}, {"actions", nlohmann::json::object()}};
    for (size_t i = 0; i < kActionCount; ++i)
        json["actions"][kActions[i].id] = {{"keyboard", p.keyboard[i]}, {"controller", p.gamepad[i]}};
    const fs::path file = profilePath(userDir);
    fs::path temp = file;
    temp += ".tmp";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << json.dump(2) << '\n';
    out.close();
    return bool(out) && atomicReplace(temp, file);
}
} // namespace cane::sly1input
