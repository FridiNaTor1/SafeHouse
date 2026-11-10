#include "cls.h"

#include "disc.h"
#include "lo.h"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

CLS g_acls[CID_Max];

namespace
{
struct CLSGEN
{
    int cid, cidBase, cidLast, cb;
    uint32_t pfnNew, vt;
};
constexpr CLSGEN s_aclsgen[] = {
#include "clsgen.inc"
};
static_assert(sizeof(s_aclsgen) / sizeof(s_aclsgen[0]) == CID_Max);

// Names: Sly 1's where the class is clearly the same thing, the reference's otherwise; CID_xx when unknown.
const char* NameOfCid(int cid)
{
    switch (cid)
    {
    case 0x01: return "SW";
    case 0x02: return "LO";
    case 0x03: return "LOX";        // LO + a transform reference and a matrix (0x231768)
    case 0x05: return "VOLUME";     // LOX + GEOM + BSP (0x227380)
    case 0x06: return "PROXY";      // loaded by 0x1C9B08 (instances of shared groups)
    case 0x08: return "PNTS";       // LOX + points and index triples (0x185150)
    case 0x0A: return "PATH";       // LOX + nodes (0x1E5DF8)
    case 0x0F: return "ZONE";       // LOX + GEOM + BSP + children (0x220FD0)
    case 0x11: return "MESHZONE";   // 0x1C03C0
    case 0x16: return "SHAPE";      // LOX + GEOM + BSP (0x227BB8)
    case 0x1D: return "EMITTER";    // 0x258598
    case 0x1F: return "GROUPER";    // children (0x15C1E8)
    case 0x20: return "EXPL";       // 0x259030
    case 0x30: return "ALO";
    case 0x32: return "SO";         // ALO + a skipped string (0x1C3DC8)
    case 0x33: return "PO";
    case 0x34: return "PLAYER";
    case 0x35: return "JT";         // Sly
    case 0x36: return "MURRAY";
    case 0x37: return "BENTLEY";
    case 0x38: return "STEPGUARD";
    case 0x6D: return "ASEG";       // 0x12F2A8
    case 0x6E: return "ASEGBL";
    case 0x6F: return "SM";         // 0x1EFAC0
    case 0x84: return "MISSION";
    case 0x85: return "TASK";
    case 0x87: return "CM";
    default: return nullptr;
    }
}

std::unordered_map<uint32_t, PFNLOAD>& Loaders()
{
    static std::unordered_map<uint32_t, PFNLOAD> s_map;
    return s_map;
}
}

void RegisterLoaders(std::unordered_map<uint32_t, PFNLOAD>& map);  // loaders.cpp
PFNNEW PfnNewNativeForCid(CID cid);                                 // loaders.cpp

namespace
{
// PfnOrig's answers per class (of g_acls) and slot, + 1 (0: not read yet): each is a read through the executable's
// image, which the per-object tests (FAloDrawn's draw slot, the vehicles', the HUD's) make every frame for every
// object. The image does not change once the disc is open; InitClasses clears them (atomic: the pass helpers ask too).
constexpr int CSLOT_PFN_CACHE = 128;
std::atomic<uint32_t> s_apfnCache[CID_Max][CSLOT_PFN_CACHE];
}

uint32_t CLS::PfnOrig(int slot) const
{
    if (!vtOrig) return 0;
    const uintptr_t p = reinterpret_cast<uintptr_t>(this), p0 = reinterpret_cast<uintptr_t>(&g_acls[0]);
    if (p >= p0 && p < p0 + sizeof(g_acls) && slot >= 0 && slot < CSLOT_PFN_CACHE)
    {
        std::atomic<uint32_t>& cached = s_apfnCache[(p - p0) / sizeof(CLS)][slot];
        if (const uint32_t v = cached.load(std::memory_order_relaxed)) return v - 1;
        const uint32_t pfn = g_disc.U32Elf(vtOrig + 8u * static_cast<uint32_t>(slot) + 4u);
        cached.store(pfn + 1, std::memory_order_relaxed);
        return pfn;
    }
    return g_disc.U32Elf(vtOrig + 8u * static_cast<uint32_t>(slot) + 4u);
}

void InitClasses()
{
    for (auto& acache : s_apfnCache)
        for (auto& cached : acache) cached.store(0, std::memory_order_relaxed);
    RegisterLoaders(Loaders());
    static char s_achName[CID_Max][16];
    for (int i = 0; i < CID_Max; ++i)
    {
        const CLSGEN& g = s_aclsgen[i];
        CLS& c = g_acls[i];
        c.cid = g.cid;
        c.cidBase = g.cidBase;
        c.cidLast = g.cidLast;
        c.cbOrig = g.cb;
        c.pfnNewOrig = g.pfnNew;
        c.vtOrig = g.vt;
        if (const char* n = NameOfCid(i)) c.name = n;
        else
        {
            std::snprintf(s_achName[i], sizeof(s_achName[i]), "CID_%02X", i);
            c.name = s_achName[i];
        }
        c.pclsBase = g.cidBase >= 0 ? &g_acls[g.cidBase] : nullptr;
        if (g.pfnNew && g.vt)
        {
            c.pfnLoadOrig = c.PfnOrig(VTSLOT_Load);
            auto it = Loaders().find(c.pfnLoadOrig);
            c.pfnLoad = it == Loaders().end() ? nullptr : it->second;
            c.pfnNew = PfnNewNativeForCid(i);
        }
    }
}

bool FHasNativeLoader(uint32_t pfnOrig) { return Loaders().count(pfnOrig) != 0; }

CLS* PclsFromCid(CID cid) { return cid >= 0 && cid < CID_Max ? &g_acls[cid] : nullptr; }

bool FCidDerivedFrom(CID cid, CID cidBase)
{
    // orig 0x15E988: the table's ranges [cidBase, cidLast)
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
