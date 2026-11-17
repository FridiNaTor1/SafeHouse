// The map editor for Sly 3: the names of Sly 3's classes and options (the executable has none), its gameplay areas and
// the options panel (editor/game/editor_options.inc, shared with Sly 2). Sly 3's own names first: the classes of
// game/gen/classnames.inc and the options the port's Sly 3 code reads by their Sly 3 numbers (game/task3.h's jobs and
// tasks, sw.cpp's group instance counts); then Sly 2's names (editor/game/editor_names2.h) for a Sly 3 number whose
// Sly 2 twin is known with confidence (game/gen/eopidmap.inc / cidmap.inc, score >= 80). Sly2-Release/notes/
// EDITOR_2026-11-12.md "Round 5".
#include "editor_impl.h"

#if SLY3_EDITOR

#include "../editor/game/editor_names2.h"

#include <cctype>
#include <cstring>

namespace edimpl
{
namespace
{
// a Sly 3 number's Sly 2 twin when the map is confident (-1: none)
template <std::size_t N>
int Twin2(const sly3::gen::MAPROW (&a)[N], int v3)
{
    for (const sly3::gen::MAPROW& r : a)
        if (r.v3 == v3 && r.score >= 80) return int(r.v2);
    return -1;
}
// a generated placeholder name ("LOX_04", "GUARD_3E"): the class's base and its number
bool FPlaceholder(const char* sz)
{
    const char* p = std::strrchr(sz, '_');
    return p && std::strlen(p) == 3 && std::isxdigit(static_cast<unsigned char>(p[1])) && std::isxdigit(static_cast<unsigned char>(p[2]));
}
}  // namespace

const char* SzClass(int cid)
{
    // (the classes the port's Sly 3 modules name by their own number: game/rccar3.cpp, biplane3.cpp, dumptruck3.cpp,
    // racer3.cpp, airide3.h)
    switch (cid)
    {
    case 0x51: return "DUMPTRUCK";
    case 0x52: return "PLANE";
    case 0x55: return "RCCAR";
    case 0x7A: return "RIDELINK";
    case 0x98: return "RACER";
    default: break;
    }
    const char* sz = sly3::SzClassName(cid);
    if (sz && !FPlaceholder(sz)) return sz;
    if (const int c2 = Twin2(sly3::gen::acid, cid); c2 >= 0)
        if (const char* sz2 = SzClassName2(c2)) return sz2;
    return sz ? sz : SzClassRaw(cid);
}

const char* SzOptionName(int eopid)
{
    switch (eopid)
    {
    // jobs (MISSION) and tasks (TASK): game/task3.h
    case 1361: return "job: flag (+0x6C)";
    case 1362: return "job: survives the player's death";
    case 1364: return "job: first task";
    case 1365: return "job: last task";
    case 1381: return "job: exit when complete";
    case 1391: return "task: job";
    case 1399: return "task: activate when available";
    case 1400: return "task: start checkpoint";
    case 1401: return "task: world";
    case 1402: return "task: the players' characters";
    case 1405: return "task: done at once when activated";
    case 1406: return "task: exit";
    case 1408: return "task: script (started when activated)";
    case 1416: return "task: objects shown per state";
    case 1417: return "task: state machine goals";
    case 1423: return "task progress: times activated";
    case 1424: return "task progress: times done";
    case 1425: return "task progress: done";
    case 1426: return "task progress: state";
    case 1427: return "task progress: abandoned";
    // the world's option sets: sw.cpp
    case sly3::EOPID_GROUP_INSTANCES: return "group instance count";
    // uses (USE, cid 0x21: buttons, switches, the gang's ability spots): interact.cpp / interactsplice.cpp / scptgame3.cpp
    case 1162: return "use: the object it acts on";
    case 1164: return "use: script started when used";
    case 1166: return "use: players allowed";
    case 1168: return "use: character (0x34 Sly, 0x35 Murray, 0x36 Bentley, 0x37 Guru, 0x38 Panda King, 0x39 Penelope)";
    case 1169: return "use: runs a state machine (object, machine, state)";
    case 1170: return "use: sets a task's state (task, state)";
    default: break;
    }
    if (const int e2 = Twin2(sly3::gen::aeopid, eopid); e2 >= 0) return SzOptionName2(e2);
    return nullptr;
}

bool FOidOption(int eopid)
{
    switch (eopid)
    {
    case 1364: case 1365: case 1381: case 1391: case 1406: case 1408: return true;
    default: return eopid >= 0 && (eopid == EDEOPID(1279) || eopid == EDEOPID(273));
    }
}

std::string StrOption(int eopid)
{
    const char* sz = SzOptionName(eopid);
    return sz ? std::to_string(eopid) + "  " + sz : std::to_string(eopid);
}

GAMEAREA AreaOfCid(int cid)
{
    switch (cid)
    {
    case 0x51: case 0x52: case 0x54: case 0x55: case 0x98: return GA_Rides;   // the truck, planes, vehicles, the RC car, racers
    case 0x7A: return GA_Gang;                                               // the Guru's ride links (airide3.h)
    case 0x5D: case 0x5E: return GA_Buttons;                                 // pads
    case 0x6A: case 0x6B: case 0x6C: case 0x69: case 0x70: case 0x79: return GA_Pickups;   // loot, clue bottles, pickups, charms, pockets
    default: break;
    }
    if (const char* sz = sly3::SzClassName(cid))
    {
        if (std::strncmp(sz, "HACK", 4) == 0) return GA_Hacking;
        if (std::strcmp(sz, "BOOTYSTAND") == 0) return GA_TwoPlayer;
        if (std::strcmp(sz, "TASKCOUNTER") == 0) return GA_Missions;
    }
    if (FCidDerivedFrom(cid, S3CID(0x23))) return GA_Buttons;               // uses (AreaOfLo: the gang's by who may use them)

    const int c2 = sly3::Cid2FromCid3(cid);
    return c2 >= 0 ? AreaOfCid2(c2, FCidDerivedFrom(cid, S3CID(0x38))) : GA_None;
}

const char* SzArea(GAMEAREA ga)
{
    static const char* s_a[] = {"Other", "Traversal", "Guards", "Triggers / volumes", "Lasers / sensors", "Warps / exits / checkpoints",
                                "Pickups / hiding", "Cameras", "Lights", "Missions", "Hacking", "Two-player", "Buttons / switches",
                                "Gang ability spots", "Rides / vehicles"};
    return ga >= 0 && ga < GA_Max ? s_a[ga] : "?";
}

const std::vector<int>& KeyOptionsOfArea(GAMEAREA ga)
{
    // Sly 2's key options as Sly 3 numbers them (those it has), then Sly 3's own
    static const std::vector<int>* s_a = [] {
        static std::vector<int> a[GA_Max];
        const std::vector<int> a2[GA_Max] = {
            {13, 14},
            {13, 1210, 1211, 1212},
            {255, 273, 233, 232, 278, 312, 313, 314},
            {13, 432, 433, 434, 441, 1072, 1073, 1076},
            {455, 312, 313, 314, 997, 999, 1002, 1003},
            {778, 779, 786, 788},
            {897, 13},
            {161, 162, 163, 164, 165, 1055},
            {151, 152, 153, 154, 155, 156},
            {1243, 1244, 1245, 1246, 1264, 1265, 1272, 1274, 1279, 1285},
            {13},
            {13},
            {},
            {},
            {}};
        for (int e3 : {1168, 1164, 1170, 1169, 1166}) { a[GA_Buttons].push_back(e3); a[GA_Gang].push_back(e3); }
        for (int i = 0; i < GA_Max; ++i)
            for (int e2 : a2[i])
                if (const int e3 = sly3::Eopid3FromEopid2(e2); e3 >= 0) a[i].push_back(e3);
        for (int e3 : {1401, 1402, 1405, 1406, 1408, 1381}) a[GA_Missions].push_back(e3);
        return a;
    }();
    return s_a[ga >= 0 && ga < GA_Max ? ga : 0];
}

}  // namespace edimpl

#endif

// the options panel, list options, oids and the checks before a save: shared with Sly 2
#include "../editor/game/editor_options.inc"
