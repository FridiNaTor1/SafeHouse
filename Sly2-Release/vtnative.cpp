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
        c.apv[slot] = PvNativeFn(plo->pcls->PfnOrig(slot));
        if (!c.apv[slot])
            for (const CLS* pb = plo->pcls->pclsBase; pb && !c.apv[slot]; pb = pb->pclsBase)
                if (void* pv = PvNativeFn(pb->PfnOrig(slot)))
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
        std::fprintf(stderr, "[vt] no native function for slot %d of cid 0x%X (orig 0x%06X)\n", slot, plo->pcls->cid, addr);
}
