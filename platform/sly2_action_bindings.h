#pragma once

// Sly 2's semantic controls are separate from the legacy PS2-button mapping.
// Stable string IDs keep profiles compatible when actions are added or reordered.
#include "system_settings.h"
#include <array>
#include <nlohmann/json.hpp>
#include <string>

namespace cane::sly2input {

// Each row ends with the default keyboard/mouse and controller binding codes.
#define CANE_SLY2_ACTIONS(X) \
 X(MoveUp, "move_up", "Move forward", "Movement", 87, 11) \
 X(MoveDown, "move_down", "Move backward", "Movement", 83, 13) \
 X(MoveLeft, "move_left", "Move left", "Movement", 65, 14) \
 X(MoveRight, "move_right", "Move right", "Movement", 68, 12) \
 X(Attack, "attack", "Attack", "Common actions", 2000, 2) \
 X(Interact, "interact", "Interact", "Common actions", 69, 1) \
 X(Jump, "jump", "Jump", "Common actions", 32, 0) \
 X(Pause, "pause", "Pause", "Common actions", 257, 7) \
 X(GadgetMenu, "gadget_menu", "Open gadget menu", "Gadgets", 73, 6) \
 X(Gadget1, "gadget_1", "Use gadget slot 1", "Gadgets", 82, 4) \
 X(Sprint, "sprint", "Sprint", "Common actions", 340, 5) \
 X(Gadget2, "gadget_2", "Use gadget slot 2", "Gadgets", 86, 1000) \
 X(Gadget3, "gadget_3", "Use gadget slot 3", "Gadgets", 341, 1001) \
 X(WorldStats, "world_stats", "World statistics", "Common actions", 258, 9) \
 X(Binocucom, "binocucom", "Open binocucom", "Common actions", 67, 10) \
 X(DoubleJump, "double_jump", "Double jump", "Common actions", 32, 0) \
 X(ClimbJump, "climb_jump", "Jump off a climb", "Climbing and ledges", 32, 0) \
 X(HangJump, "hang_jump", "Jump from a ledge", "Climbing and ledges", 32, 0) \
 X(SlyPickpocket, "sly_pickpocket", "Pickpocket", "Sly", 69, 1) \
 X(SlyUppercut, "sly_uppercut", "Uppercut / launch enemy", "Sly", 2001, 3) \
 X(SlyAirAttack, "sly_air_attack", "Aerial cane attack", "Sly", 2000, 2) \
 X(Paraglider, "paraglider", "Deploy / hold paraglider", "Sly", 340, 5) \
 X(SlyDive, "sly_dive", "Aerial dive", "Sly", 2001, 3) \
 X(BentleySwing, "bentley_swing", "Swing dart gun", "Bentley", 2000, 2) \
 X(BentleyDart, "bentley_dart", "Fire sleep / noise dart", "Bentley", 2000, 5) \
 X(BentleySpecial, "bentley_special", "Place bomb / use special item", "Bentley", 2001, 3) \
 X(MurrayUppercut, "murray_uppercut", "Uppercut / lift", "Murray", 2001, 3) \
 X(MurrayFlop, "murray_flop", "Thunder Flop", "Murray", 2000, 2) \
 X(CarryThrow, "carry_throw", "Throw a carried object", "Carrying and throwing", 2000, 2) \
 X(ThrowRelease, "throw_release", "Throw aimed item", "Carrying and throwing", 2000, 2) \
 X(ThrowCancel, "throw_cancel", "Cancel throw / put down", "Carrying and throwing", 69, 1) \
 X(ReconClose, "recon_close", "Close binocucom", "Binocucom", 67, 10) \
 X(Photo, "take_photo", "Take a photograph", "Binocucom", 2000, 5) \
 X(ZoomIn, "zoom_in", "Zoom in", "Binocucom", 2008, 1001) \
 X(ZoomOut, "zoom_out", "Zoom out", "Binocucom", 2009, 1000) \
 X(TurretFire, "turret_fire", "Fire turret", "Vehicles and turrets", 2000, 2) \
 X(VehicleDrive, "vehicle_drive", "Accelerate vehicle", "Vehicles and turrets", 340, 0) \
 X(VehiclePrimary, "vehicle_primary", "Primary weapon", "Vehicles and turrets", 2000, 2) \
 X(VehicleSecondary, "vehicle_secondary", "Secondary weapon", "Vehicles and turrets", 2001, 1) \
 X(TankLeftForward, "tank_left_forward", "Left track forward", "Vehicles and turrets", 81, -1) \
 X(TankLeftReverse, "tank_left_reverse", "Left track reverse", "Vehicles and turrets", 65, -1) \
 X(TankRightForward, "tank_right_forward", "Right track forward", "Vehicles and turrets", 69, -1) \
 X(TankRightReverse, "tank_right_reverse", "Right track reverse", "Vehicles and turrets", 68, -1) \
 X(TankBothForward, "tank_both_forward", "Both tracks forward", "Vehicles and turrets", 87, -1) \
 X(TankBothReverse, "tank_both_reverse", "Both tracks reverse", "Vehicles and turrets", 83, -1) \
 X(RcUp, "rc_up", "RC helicopter ascend", "RC helicopter", 32, 1001) \
 X(RcDown, "rc_down", "RC helicopter descend", "RC helicopter", 341, 1000) \
 X(RcFire, "rc_fire", "RC helicopter fire", "RC helicopter", 2000, 2) \
 X(RcBomb, "rc_bomb", "RC helicopter drop bomb", "RC helicopter", 2001, 0) \
 X(RcCamera, "rc_camera", "RC helicopter camera", "RC helicopter", 258, 10) \
 X(HackFire, "hack_fire", "Fire hacking ship", "Minigames", 2000, 2) \
 X(DanceSquare, "dance_left", "Dance left / Square cue", "Minigames", 65, 2) \
 X(DanceCircle, "dance_right", "Dance right / Circle cue", "Minigames", 68, 1) \
 X(DanceCross, "dance_down", "Dance down / Cross cue", "Minigames", 83, 0) \
 X(DanceTriangle, "dance_up", "Dance up / Triangle cue", "Minigames", 87, 3) \
 X(VaultConfirm, "vault_confirm", "Confirm vault digit", "Minigames", 32, 0) \
 X(VaultCancel, "vault_cancel", "Cancel vault entry", "Minigames", 69, 1) \
 X(NoclipAscend, "noclip_ascend", "Ascend", "Noclip", 32, 0) \
 X(NoclipDescend, "noclip_descend", "Descend", "Noclip", 341, 1) \
 X(NoclipFast, "noclip_fast", "Speed up", "Noclip", 340, 5) \
 X(NoclipSlow, "noclip_slow", "Slow down", "Noclip", 342, 4)

enum class Action {
#define CANE_ACTION_ENUM(name, id, label, group, key, pad) name,
 CANE_SLY2_ACTIONS(CANE_ACTION_ENUM)
#undef CANE_ACTION_ENUM
 Count
};
// A legacy button can have no standalone advanced action. Use its basic binding in that case.
inline constexpr Action legacyButtonAction(int button) {
    constexpr std::array actions{
        Action::MoveUp, Action::MoveDown, Action::MoveLeft, Action::MoveRight,
        Action::Attack, Action::Interact, Action::Jump, Action::Count,
        Action::Pause, Action::GadgetMenu, Action::Gadget1, Action::Sprint,
        Action::Gadget2, Action::Gadget3, Action::WorldStats, Action::Binocucom,
    };
    static_assert(actions.size() == settings::kBindingCount);
    return button >= 0 && size_t(button) < actions.size() ? actions[size_t(button)] : Action::Count;
}
inline constexpr std::array kGadgetSlots{Action::Gadget1, Action::Gadget2, Action::Gadget3};
inline constexpr bool isGadgetSlot(Action action) {
    for (Action slot : kGadgetSlots)
        if (action == slot) return true;
    return false;
}
inline constexpr bool isNoclipAction(Action action) {
    return action >= Action::NoclipAscend && action <= Action::NoclipSlow;
}

inline constexpr size_t kActionCount = size_t(Action::Count);
struct ActionInfo { const char* id; const char* label; const char* group; int defaultKey; int defaultPad; };
inline constexpr std::array<ActionInfo, kActionCount> kActions{{
#define CANE_ACTION_INFO(name, id, label, group, key, pad) {id, label, group, key, pad},
 CANE_SLY2_ACTIONS(CANE_ACTION_INFO)
#undef CANE_ACTION_INFO
}};
#undef CANE_SLY2_ACTIONS

struct Profile {
    bool enabled = false;
    bool readOnly = false;
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
inline fs::path profilePath(const fs::path& userDir) { return userDir / "Saves" / "advanced_controls.json"; }
inline Profile load(const fs::path& userDir) {
    Profile p = defaults();
    std::ifstream in(profilePath(userDir));
    if (!in) return p;
    try {
        const auto json = nlohmann::json::parse(in);
        if (json.value("version", 0) != 1) { p.readOnly = true; return p; }
        p.enabled = json.value("enabled", false);
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
    } catch (const nlohmann::json::exception&) { p = defaults(); p.readOnly = true; }
    return p;
}
inline bool save(const fs::path& userDir, const Profile& p) {
    if (p.readOnly) return false;
    nlohmann::json json{{"version", 1}, {"enabled", p.enabled}, {"actions", nlohmann::json::object()}};
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
} // namespace cane::sly2input
