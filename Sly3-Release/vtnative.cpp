// The native implementations of original functions, by address, and the vtable dispatch (vtnative.h).

#include "vtnative.h"

#include "cls.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct REG
{
    void* pfn;
    const char* name;
};
std::unordered_map<uint32_t, REG>& Map()
{
    static std::unordered_map<uint32_t, REG> s_map;
    return s_map;
}
constexpr int SLOT_Max = 128;
struct CACHE
{
    std::array<void*, SLOT_Max> apv{};
    std::array<bool, SLOT_Max> afDone{};
};
std::vector<CACHE>& Caches()
{
    static std::vector<CACHE> s_a(CID_Max);
    return s_a;
}
}

void RegisterNativeFn(uint32_t addrOrig, void* pfn, const char* name) { Map()[addrOrig] = REG{pfn, name}; }

const char* NameNativeFn(uint32_t addrOrig)
{
    auto it = Map().find(addrOrig);
    return it == Map().end() ? nullptr : it->second.name;
}

void* PvNativeFn(uint32_t addrOrig)
{
    auto it = Map().find(addrOrig);
    return it == Map().end() ? nullptr : it->second.pfn;
}

void* PvNativeFn3(const CLS* pcls, int slot2)
{
    const int slot3 = pcls ? Slot3FromSlot2(pcls->cid, slot2) : -1;
    const uint32_t fn3 = slot3 >= 0 ? pcls->PfnOrig3(slot3) : 0;
    return fn3 ? PvNativeFn(FN3_TAG | fn3) : nullptr;
}

void* PvNativeVt(const LO* plo, int slot)
{
    if (!plo || !plo->pcls || slot < 0 || slot >= SLOT_Max) return nullptr;
    const CID cid = plo->pcls->cid;
    if (cid < 0 || cid >= CID_Max) return nullptr;
    CACHE& c = Caches()[cid];
    if (!c.afDone[slot])
    {
        // the class's function; an override not ported natively falls back to the nearest base class's that is
        // (noted once: the override's own work is missing)
        // (Sly 3: a native ported from the Sly 3 asm (NATIVEFN3) first, then the Sly 2 twin's: vtmap3.cpp)
        c.apv[slot] = PvNativeFn3(plo->pcls, slot);
        if (!c.apv[slot]) c.apv[slot] = PvNativeFn(plo->pcls->PfnOrig(slot));
        if (!c.apv[slot])
            for (const CLS* pb = plo->pcls->pclsBase; pb && !c.apv[slot]; pb = pb->pclsBase)
                if (void* pv = PvNativeFn3(pb, slot) ? PvNativeFn3(pb, slot) : PvNativeFn(pb->PfnOrig(slot)))
                {
                    NoteMissingVt(plo, slot);
                    c.apv[slot] = pv;
                }
        c.afDone[slot] = true;
    }
    return c.apv[slot];
}

void NoteMissingVt(const LO* plo, int slot)
{
    static std::unordered_set<uint64_t> s_seen;
    if (!plo || !plo->pcls) return;
    const uint32_t addr = plo->pcls->PfnOrig(slot);
    if (s_seen.insert((uint64_t(addr) << 8) | uint64_t(slot)).second)
        std::fprintf(stderr, "[vt] no native function for Sly 2 slot %d (Sly 3 slot %d, orig3 0x%06X) of cid 0x%X (Sly 2 twin 0x%06X)\n", slot,
                     Slot3FromSlot2(plo->pcls->cid, slot), plo->pcls->PfnOrig3(Slot3FromSlot2(plo->pcls->cid, slot)), plo->pcls->cid, addr);
}
