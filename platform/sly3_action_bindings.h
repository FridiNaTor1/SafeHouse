#pragma once
// Sly 3's advanced input mapping: the semantic actions Sly 3 offers. The file and its format are Sly 2's
// (sly2_action_bindings.h: <user-dir>/Saves/advanced_controls.json, actions by stable string id), in Sly 3's own user
// folder; the game (Sly3-Release actioninput.cpp, the Remap page) and the launcher's Advanced Input Mapping rows for
// Sly 3 read the same table through this header.
//
// Today Sly 3 offers the actions it shares with Sly 2 (movement, the common actions, climbing, Sly's / Bentley's /
// Murray's moves, carrying and throwing, the binocucom, turrets and vehicles, the hacking ship, noclip). Sly 2's own
// minigames and vehicles (the tank, the RC helicopter, the dance, the vault) are not offered.
//
// HOOK for Sly 3's new moves (s3-player): add a row to CANE_SLY3_OWN_ACTIONS below (same row format as
// CANE_SLY2_ACTIONS: name, stable id, label, group, default key, default pad). The rows become Sly3Own::<name> and are
// listed by ownActions(); loadOwn reads them from the same file (a "sly3" object: id -> [key, pad]); the game reads
// them with ownBinding(). With the first row, also: a saveOwn that keeps the "sly3" object when sly2input::save
// rewrites the file, and the launcher's rows for them (settings_page.h controlsRows, beside the shared actions).
// Ids must never collide with Sly 2's.
#include "sly2_action_bindings.h"

#include <filesystem>
#include <fstream>

namespace cane::sly3input {
using sly2input::Action;
using sly2input::kActions;
using sly2input::kActionCount;

// Whether Sly 3 offers one of the shared table's actions.
inline constexpr bool offered(Action a) {
    switch (a) {
    case Action::TankLeftForward: case Action::TankLeftReverse: case Action::TankRightForward: case Action::TankRightReverse:
    case Action::TankBothForward: case Action::TankBothReverse:
    case Action::RcUp: case Action::RcDown: case Action::RcFire: case Action::RcBomb: case Action::RcCamera:
    case Action::DanceSquare: case Action::DanceCircle: case Action::DanceCross: case Action::DanceTriangle:
    case Action::VaultConfirm: case Action::VaultCancel:
        return false;
    default: return a < Action::Count;
    }
}

// Sly 3's own actions (the hook above). Empty until Sly 3's new characters' moves are ported.
#define CANE_SLY3_OWN_ACTIONS(X) /* X(PandaKingFlames, "panda_king_flames", "Fire flames", "Panda King", 2000, 2) */
enum class Sly3Own {
#define CANE_SLY3_OWN_ENUM(name, id, label, group, key, pad) name,
    CANE_SLY3_OWN_ACTIONS(CANE_SLY3_OWN_ENUM)
#undef CANE_SLY3_OWN_ENUM
    Count
};
inline constexpr size_t kOwnCount = size_t(Sly3Own::Count);
inline const std::vector<sly2input::ActionInfo>& ownActions() {
    static const std::vector<sly2input::ActionInfo> a{
#define CANE_SLY3_OWN_INFO(name, id, label, group, key, pad) {id, label, group, key, pad},
        CANE_SLY3_OWN_ACTIONS(CANE_SLY3_OWN_INFO)
#undef CANE_SLY3_OWN_INFO
    };
    return a;
}
// Sly 3's own bindings, kept in the same file beside the shared ones ("sly3" object, by id)
struct OwnProfile {
    std::vector<int> keyboard, gamepad;
};
inline OwnProfile ownDefaults() {
    OwnProfile p;
    for (const auto& a : ownActions()) p.keyboard.push_back(a.defaultKey), p.gamepad.push_back(a.defaultPad);
    return p;
}
inline OwnProfile loadOwn(const fs::path& userDir) {
    OwnProfile p = ownDefaults();
    try {
        std::ifstream in(sly2input::profilePath(userDir));
        if (!in) return p;
        const nlohmann::json j = nlohmann::json::parse(in);
        if (!j.contains("sly3") || !j["sly3"].is_object()) return p;
        const auto& o = j["sly3"];
        for (size_t i = 0; i < ownActions().size(); ++i)
            if (o.contains(ownActions()[i].id) && o[ownActions()[i].id].is_array() && o[ownActions()[i].id].size() == 2) {
                p.keyboard[i] = o[ownActions()[i].id][0].get<int>();
                p.gamepad[i] = o[ownActions()[i].id][1].get<int>();
            }
    } catch (...) {}
    return p;
}
inline int ownBinding(const OwnProfile& p, Sly3Own a, bool controller) {
    const size_t i = size_t(a);
    const auto& v = controller ? p.gamepad : p.keyboard;
    return i < v.size() ? v[i] : -1;
}
} // namespace cane::sly3input
