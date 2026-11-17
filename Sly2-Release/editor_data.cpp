// The map editor (editor_impl.h): the names the port knows (classes, options: from the code's comments, the executable
// has none), gameplay areas, the options panel with live updates where safe, oid allocation over every recorded oid
// and the checks before a save. SLY2_EDITOR builds only.
#include "editor_impl.h"

#if SLY2_EDITOR

#include "worldparts.h"
#include "../editor/game/editor_names2.h"

#include <set>

namespace edimpl
{

const char* SzClass(int cid)
{
    // the class names cls.cpp has, then the editor's own for the classes the code knows by number (editor_names2.h)
    if (cid >= 0 && cid < CID_Max && g_acls[cid].name && *g_acls[cid].name && g_acls[cid].name[0] != 'C') return g_acls[cid].name;
    if (const char* sz = SzClassName2(cid)) return sz;
    return SzClassRaw(cid);
}

const char* SzOptionName(int eopid) { return SzOptionName2(eopid); }

bool FOidOption(int eopid)
{
    // the options whose integer value is an object's oid though their kind does not say so (jobs, tasks, patrols)
    switch (eopid)
    {
    case 1243: case 1244: case 1264: case 1274: case 1279: case 273: return true;
    default: return false;
    }
}

std::string StrOption(int eopid)
{
    const char* sz = SzOptionName(eopid);
    return sz ? std::to_string(eopid) + "  " + sz : std::to_string(eopid);
}

GAMEAREA AreaOfCid(int cid) { return AreaOfCid2(cid, FCidDerivedFrom(cid, 0x38)); }

const char* SzArea(GAMEAREA ga)
{
    static const char* s_a[] = {"Other", "Traversal", "Guards", "Triggers / volumes", "Lasers / sensors", "Warps / exits / checkpoints",
                                "Pickups / hiding", "Cameras", "Lights", "Missions", "Hacking", "Two-player", "Buttons / switches",
                                "Gang ability spots", "Rides / vehicles"};
    return ga >= 0 && ga < GA_Max ? s_a[ga] : "?";
}

const std::vector<int>& KeyOptionsOfArea(GAMEAREA ga)
{
    static const std::vector<int> s_a[GA_Max] = {
        {13, 14},
        {13, 1210, 1211, 1212},
        {255, 273, 233, 232, 278, 312, 313, 314},
        {13, 432, 433, 434, 441, 1072, 1073, 1076},
        {455, 312, 313, 314, 997, 999, 1002, 1003},
        {778, 779, 786, 788},
        {897, 13},
        {161, 162, 163, 164, 165, 1055},
        {151, 152, 153, 154, 155, 156},
        {1243, 1244, 1245, 1246, 1264, 1265, 1272, 1274, 1276, 1279, 1285, 642},
        {},
        {},
        {},
        {},
        {}};
    return s_a[ga >= 0 && ga < GA_Max ? ga : 0];
}

}  // namespace edimpl

#endif

// the options panel, list options, oids and the checks before a save: shared with Sly 3
#include "../editor/game/editor_options.inc"
