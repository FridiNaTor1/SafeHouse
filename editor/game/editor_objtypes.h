#pragma once
// The map editor's object types (notes/EDITOR_PLAN.md), without the editor: every object gets a type from its class (and,
// for the guards' family, its shared group's name), a friendly name, an icon colour and a vis group. Shared by the editor
// (editor_types.inc: CEditor::TypeOfLo) and the games' debug overlays (Sly2-Release/debugvis.cpp), which build on every
// platform (no ImGui here). Include after the game's lo.h / brx.h (LO, FIsLoDerivedFrom); EDCID maps a Sly 2 class
// number to the game's (identity unless the game's editor_game.h defined it).

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#ifndef EDCID
#define EDCID(cid2) (cid2)
#endif

namespace edimpl
{

// the gameplay areas the options panel groups classes by (editor_names2.h AreaOfCid2)
enum GAMEAREA : int
{
    GA_None, GA_Traversal, GA_Guards, GA_Triggers, GA_Lasers, GA_Warps, GA_Pickups, GA_Cameras, GA_Lights, GA_Missions, GA_Hacking,
    GA_TwoPlayer, GA_Buttons, GA_Gang, GA_Rides, GA_Max
};

enum VISGROUP : int
{
    VG_Characters, VG_Gameplay, VG_Triggers, VG_Paths, VG_Cameras, VG_Lighting, VG_Sound, VG_Effects, VG_Missions, VG_Logic,
    VG_World, VG_Other, VG_Max
};
enum OBJTYPE : int
{
    OT_Player, OT_Guard, OT_Swarmer, OT_Boss, OT_Npc, OT_Pickup, OT_Loot, OT_Vault, OT_Carryable, OT_HideSpot, OT_Trigger,
    OT_Volume, OT_Button, OT_Sensor, OT_Turret, OT_Door, OT_Warp, OT_Checkpoint, OT_Spawner, OT_Path, OT_Traversal,
    OT_NavMesh, OT_Camera, OT_CameraZone, OT_Light, OT_Sound, OT_Effect, OT_Water, OT_Sky, OT_Job, OT_Task, OT_JobMarker,
    OT_Scene, OT_Logic, OT_Animation, OT_Interest, OT_Ui, OT_Mesh, OT_Prop, OT_Breakable, OT_Proxy, OT_Grouper, OT_Other,
    OT_Max
};
struct OBJTYPEINFO
{
    const char* name;       // (English; T() at the drawing)
    const char* plural;
    VISGROUP vg;
    uint32_t rgb;           // its marker / icon colour
    char icon;              // its icon letter
};

inline const OBJTYPEINFO& InfoOfType(OBJTYPE ot)
{
    static const OBJTYPEINFO s_a[OT_Max] = {
        {"Player", "Players", VG_Characters, 0xF4C531, 'P'},
        {"Guard", "Guards", VG_Characters, 0xF0506E, 'G'},
        {"Swarmer", "Swarmers", VG_Characters, 0xE07A5F, 'g'},
        {"Boss", "Bosses", VG_Characters, 0xFF3355, 'B'},
        {"NPC", "NPCs", VG_Characters, 0xE6A1FF, 'N'},
        {"Pickup", "Pickups", VG_Gameplay, 0xFFD75E, 'c'},
        {"Loot", "Loot", VG_Gameplay, 0xFFB000, '$'},
        {"Vault", "Vaults", VG_Gameplay, 0xFFB000, 'V'},
        {"Carryable", "Carryables", VG_Gameplay, 0xD9AA14, 'k'},
        {"Hiding spot", "Hiding spots", VG_Gameplay, 0x8FA8C8, 'h'},
        {"Trigger", "Triggers", VG_Triggers, 0xFF8C1A, 'T'},
        {"Volume", "Volumes", VG_Triggers, 0xC07020, 'v'},
        {"Button / use", "Buttons / uses", VG_Gameplay, 0x3CC66B, 'U'},
        {"Sensor / laser", "Sensors / lasers", VG_Triggers, 0xFF4040, 'L'},
        {"Turret", "Turrets", VG_Gameplay, 0xC04060, 't'},
        {"Door / exit", "Doors / exits", VG_Gameplay, 0x60D0A0, 'D'},
        {"Warp", "Warps", VG_Gameplay, 0x40E0E0, 'W'},
        {"Checkpoint", "Checkpoints", VG_Gameplay, 0x40C0FF, 'C'},
        {"Spawner", "Spawners", VG_Gameplay, 0xD060D0, 'S'},
        {"Path", "Paths", VG_Paths, 0x5EC0FF, '~'},
        {"Traversal", "Traversal", VG_Paths, 0x3AA3F0, 'r'},
        {"Navigation", "Navigation", VG_Paths, 0x4070A0, 'n'},
        {"Camera", "Cameras", VG_Cameras, 0xFFFFFF, 'K'},
        {"Camera zone", "Camera zones", VG_Cameras, 0xB0C8F0, 'Z'},
        {"Light", "Lights", VG_Lighting, 0xFFF0A0, '*'},
        {"Sound", "Sounds", VG_Sound, 0x70E0B0, 's'},
        {"Effect", "Effects", VG_Effects, 0xC090FF, 'e'},
        {"Water", "Water", VG_Effects, 0x3080FF, 'w'},
        {"Sky", "Sky", VG_Lighting, 0x90C0FF, 'y'},
        {"Job", "Jobs", VG_Missions, 0xF4C531, 'J'},
        {"Task", "Tasks", VG_Missions, 0xD9AA14, 'j'},
        {"Job marker", "Job markers", VG_Missions, 0xFFD75E, 'M'},
        {"Cutscene", "Cutscenes", VG_Missions, 0xE0C080, 'Q'},
        {"Logic", "Logic", VG_Logic, 0x8090B0, 'x'},
        {"Animation", "Animations", VG_Logic, 0x7080A0, 'a'},
        {"Interest point", "Interest points", VG_Logic, 0xA0B0D0, 'i'},
        {"Screen / UI", "Screen / UI", VG_Logic, 0x6C80A3, 'u'},
        {"World mesh", "World meshes", VG_World, 0xA8B9D6, 'm'},
        {"Prop", "Props", VG_World, 0xC8D4E8, 'p'},
        {"Breakable", "Breakables", VG_World, 0xE0A070, 'b'},
        {"Instance (proxy)", "Instances (proxies)", VG_Logic, 0x8FA8C8, 'I'},
        {"Group", "Groups", VG_Logic, 0x8FA8C8, 'o'},
        {"Other", "Other", VG_Other, 0x6C80A3, '?'},
    };
    return s_a[ot >= 0 && ot < OT_Max ? ot : OT_Other];
}

inline const char* SzVisGroup(VISGROUP vg)
{
    static const char* const s_a[VG_Max] = {"Characters", "Gameplay", "Triggers", "Paths / traversal", "Cameras", "Lighting",
                                            "Sound", "Effects", "Missions", "Logic", "World", "Other"};
    return s_a[vg >= 0 && vg < VG_Max ? vg : VG_Other];
}

inline std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// a shared group's name as words: "npc_moose_guard" -> "Moose guard"
inline std::string PrettyGroupName(const std::string& grp)
{
    // the characters the groups name (the game's own short names)
    static const std::pair<const char*, const char*> s_aknown[] = {
        {"jt", "Sly"}, {"bentley", "Bentley"}, {"murray", "Murray"}, {"npc_dimitri", "Dimitri"}, {"npc_boss_neela", "Neyla"},
        {"npc_boss_canada", "Jean Bison"}, {"npc_boss_india", "Rajan"}, {"npc_boss_prague", "The Contessa"},
        {"npc_boss_paris", "Dimitri"}, {"npc_boss_clockla", "Clock-La"}, {"npc_boss_arpeggio", "Arpeggio"},
        {"npc_carmelita", "Carmelita"}, {"safe_house", "Safe house"}, {"rc_chopper", "RC chopper"}};
    const std::string l = Lower(grp);
    for (const auto& [k, v] : s_aknown)
        if (l == k) return v;
    std::string s = l;
    // (an episode's letter prefix: "f_awning_yellow")
    if (s.size() > 2 && s[1] == '_' && std::isalpha(static_cast<unsigned char>(s[0]))) s = s.substr(2);
    for (const char* pre : {"npc_boss_", "npc_", "obj_", "prop_"})
        if (s.rfind(pre, 0) == 0) { s = s.substr(std::strlen(pre)); break; }
    for (char& c : s)
        if (c == '_') c = ' ';
    if (!s.empty()) s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// An object's type by its class. grpLower: its shared group's name in lower case (the guards' family: boss / swarmer /
// guard / NPC); fLevelGroup: its group is a level group (not a shared one: a model there is the world's mesh, else a prop)
inline OBJTYPE OtOfLo(const LO* plo, const std::string& grpLower, bool fLevelGroup)
{
    if (!plo) return OT_Other;
    LO* p = const_cast<LO*>(plo);
    const int cid = plo->Cid();
    // (macros: Sly 3's EDCID needs constants)
#define is(c2) (EDCID(c2) >= 0 && cid == EDCID(c2))
#define from(c2) (EDCID(c2) >= 0 && FIsLoDerivedFrom(p, EDCID(c2)))
    OBJTYPE ot = OT_Other;
    if (from(0x34)) ot = OT_Player;
    else if (from(0x38))
    {
        // the guards' family by what their shared group is called (npc_boss_*, npc_swarmer_*, npc_*_guard, npc_dimitri)
        const std::string& g = grpLower;
        if (g.find("boss") != std::string::npos) ot = OT_Boss;
        else if (g.find("swarm") != std::string::npos) ot = OT_Swarmer;
        else if (g.find("guard") != std::string::npos || g.empty()) ot = OT_Guard;
        else ot = OT_Npc;
    }
    else if (from(0x33)) ot = OT_Npc;
    else if (is(0x25) || is(0x67) || is(0x5C)) ot = OT_Loot;
    else if (is(0x4E)) ot = OT_Vault;
    else if (from(0x5A)) ot = OT_Pickup;
    else if (from(0x64)) ot = OT_Carryable;
    else if (is(0x58)) ot = OT_HideSpot;
    else if (from(0x18)) ot = OT_Trigger;
    else if (is(0x1A)) ot = OT_Door;
    else if (from(0x23) || is(0x53)) ot = OT_Button;
    else if (is(0x05) || from(0x16) || is(0x11)) ot = OT_Volume;
    else if (is(0x10) || from(0x54) || is(0x55)) ot = OT_Sensor;
    else if (is(0x45)) ot = OT_Turret;
    else if (is(0x56)) ot = OT_Door;
    else if (is(0x29) || is(0x73)) ot = OT_Warp;
    else if (is(0x2A)) ot = OT_Checkpoint;
    else if (is(0x77)) ot = OT_Spawner;
    else if (is(0x0E) || is(0x7C)) ot = OT_NavMesh;
    else if (is(0x0A)) ot = OT_Path;
    else if (from(0x0A) || from(0x08) || from(0x12) || is(0x1B) || is(0x28) || is(0x6A) || is(0x27)) ot = OT_Traversal;
    else if (is(0x2C)) ot = OT_Camera;
    else if (is(0x0F)) ot = OT_CameraZone;
    else if (is(0x04)) ot = OT_Light;
    else if (is(0x2B)) ot = OT_Sound;
    else if (is(0x59)) ot = OT_Water;
    else if (is(0x61)) ot = OT_Sky;
    else if (is(0x1C) || is(0x1D) || is(0x20) || is(0x21) || is(0x69) || is(0x63) || from(0x4F)) ot = OT_Effect;
    else if (is(0x84)) ot = OT_Job;
    else if (is(0x85)) ot = OT_Task;
    else if (is(0x6B)) ot = OT_JobMarker;
    else if (is(0x72)) ot = OT_Scene;
    else if (is(0x6D) || is(0x6E)) ot = OT_Animation;
    else if (is(0x15) || is(0x2E)) ot = OT_Interest;
    else if (from(0x7E)) ot = OT_Ui;
    else if (is(0x6F) || is(0x70) || is(0x71) || is(0x74) || is(0x75) || is(0x78) || is(0x81) || is(0x2D) || is(0x46)) ot = OT_Logic;
    else if (from(0x06)) ot = OT_Proxy;
    else if (is(0x1F)) ot = OT_Grouper;
    else if (is(0x62) || from(0x1E)) ot = OT_Breakable;
    else if (is(0x31)) ot = OT_Mesh;
    else if (from(0x30))
    {
        // a level group's own model: the world's mesh; a shared group's (an instance's) or a part of one: a prop
        const LO* root = plo;
        while (root->paloParent) root = root->paloParent;
        ot = root->oidInstance >= 0 || !fLevelGroup ? OT_Prop : OT_Mesh;
    }
#undef is
#undef from
    return ot;
}

}  // namespace edimpl
