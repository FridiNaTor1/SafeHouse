#include "cls.h"

#include "game/sly3.h"

#include "disc.h"
#include "lo.h"

#include <cstdio>
#include <stdexcept>
#include <unordered_map>

CLS g_acls[CID_Max];

namespace
{
// Sly 3's class table (orig3 0x3409E0; game/gen/classes.inc)
static_assert(sizeof(sly3::gen::aclass) / sizeof(sly3::gen::aclass[0]) == CID_Max);

std::unordered_map<uint32_t, PFNLOAD>& Loaders()
{
    static std::unordered_map<uint32_t, PFNLOAD> s_map;
    return s_map;
}
}

void RegisterLoaders(std::unordered_map<uint32_t, PFNLOAD>& map);  // loaders.cpp
PFNNEW PfnNewNativeForCid(CID cid);                                 // loaders.cpp

// CLS::PfnOrig / PfnOrig3: vtmap3.cpp

void InitClasses()
{
    RegisterLoaders(Loaders());
    static char s_achName[CID_Max][16];
    for (int i = 0; i < CID_Max; ++i)
    {
        const sly3::gen::CLASSROW& g = sly3::gen::aclass[i];
        CLS& c = g_acls[i];
        c.cid = g.cid;
        c.cidBase = g.cidBase;
        c.cidLast = g.cidLast;
        c.cbOrig = g.cb;
        c.pfnNewOrig = g.pfnNew;
        c.vtOrig = g.vt;
        if (const char* n = sly3::SzClassName(i); n && *n) c.name = n;
        else
        {
            std::snprintf(s_achName[i], sizeof(s_achName[i]), "CID3_%02X", i);
            c.name = s_achName[i];
        }
        c.pclsBase = g.cidBase >= 0 ? &g_acls[g.cidBase] : nullptr;
        if (g.pfnNew && g.vt)
        {
            // the native loaders checked against the Sly 3 asm, by the Sly 3 slot-14 function (loaders.cpp
            // RegisterLoaders); other classes are made and their bodies skipped (brx.cpp)
            c.pfnLoadOrig = c.PfnOrig3(VTSLOT_Load);   // (slot 14: the same in both games; keyed by the Sly 3 address)
            auto it = Loaders().find(c.pfnLoadOrig);
            c.pfnLoad = it == Loaders().end() ? nullptr : it->second;
            c.pfnNew = PfnNewNativeForCid(i);
        }
    }
    InitVtMap3();
}

bool FHasNativeLoader(uint32_t pfnOrig) { return Loaders().count(pfnOrig) != 0; }

CLS* PclsFromCid(CID cid) { return cid >= 0 && cid < CID_Max ? &g_acls[cid] : nullptr; }

bool FCidDerivedFrom(CID cid, CID cidBase)
{
    // orig3 0x13BEC8 (Sly 2 0x15E988): the table's ranges [cidBase, cidLast)
    if (cid < 0 || cid >= CID_Max || cidBase < 0 || cidBase >= CID_Max) return false;
    return cid >= cidBase && cid < g_acls[cidBase].cidLast;
}

std::string StrClassName(CID cid)
{
    char ach[48];
    if (cid >= 0 && cid < CID_Max) std::snprintf(ach, sizeof(ach), "%s (0x%02X)", g_acls[cid].name, cid);
    else std::snprintf(ach, sizeof(ach), "cid %d", cid);
    return ach;
}
