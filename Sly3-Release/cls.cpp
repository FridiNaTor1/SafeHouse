#include "cls.h"

#include "disc.h"
#include "lo.h"
#include "sly3/ids3.h"

#include <cstdio>
#include <stdexcept>
#include <unordered_map>

CLS g_acls[CID_Max];

namespace
{
std::unordered_map<uint32_t, PFNLOAD>& Loaders()
{
    static std::unordered_map<uint32_t, PFNLOAD> s_map;
    return s_map;
}
}

void RegisterLoaders3(std::unordered_map<uint32_t, PFNLOAD>& map);  // sly3/load3.cpp (keyed by orig3 addresses)
PFNNEW PfnNewNativeForCid3(CID cid);                                  // sly3/load3.cpp

uint32_t CLS::PfnOrig(int slot) const
{
    if (!vtOrig) return 0;
    return g_disc.U32Elf(vtOrig + 8u * static_cast<uint32_t>(slot) + 4u);
}

void InitClasses()
{
    static bool s_fDone = false;
    if (s_fDone) return;
    s_fDone = true;
    RegisterLoaders3(Loaders());
    for (int i = 0; i < CID_Max; ++i)
    {
        const CLS3GEN& g = Cls3(i);
        CLS& c = g_acls[i];
        c.cid = g.cid;
        c.cidBase = g.cidBase;
        c.cidLast = g.cidLast;
        c.cbOrig = g.cb;
        c.pfnNewOrig = g.pfnNew;
        c.vtOrig = g.vt;
        c.name = SzCid3(i);
        c.pclsBase = g.cidBase >= 0 ? &g_acls[g.cidBase] : nullptr;
        if (g.pfnNew && g.vt)
        {
            c.pfnLoadOrig = g.pfnLoad;  // vt slot 14 (cls3gen.inc; the same as PfnOrig(VTSLOT_Load))
            auto it = Loaders().find(c.pfnLoadOrig);
            c.pfnLoad = it == Loaders().end() ? nullptr : it->second;
            c.pfnNew = PfnNewNativeForCid3(i);
        }
    }
}

bool FHasNativeLoader(uint32_t pfnOrig) { return Loaders().count(pfnOrig) != 0; }

CLS* PclsFromCid(CID cid) { return cid >= 0 && cid < CID_Max ? &g_acls[cid] : nullptr; }

bool FCidDerivedFrom(CID cid, CID cidBase)
{
    // orig3: 0x13BEC8 (the table's base chain; the ranges [cidBase, cidLast) agree with it)
    if (cid < 0 || cid >= CID_Max || cidBase < 0 || cidBase >= CID_Max) return false;
    return FCid3DerivedFrom(cid, static_cast<CID3>(cidBase));
}

std::string StrClassName(CID cid)
{
    char ach[48];
    if (cid >= 0 && cid < CID_Max) std::snprintf(ach, sizeof(ach), "%s (0x%02X)", SzCid3(cid), cid);
    else std::snprintf(ach, sizeof(ach), "cid %d", cid);
    return ach;
}
