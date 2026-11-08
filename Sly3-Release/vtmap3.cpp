// vtmap3.cpp: the copied Sly 2 code's vtable slots and function addresses on Sly 3's classes (cls.h).
//
// The copied code calls vtables by Sly 2 slot (PfnVt(plo, 16)), compares a class's slot function against Sly 2
// addresses (pcls->PfnOrig(7) == 0x1B9F80) and registers its natives by Sly 2 address (NATIVEFN, RegisterLoUpdate).
// The class table is Sly 3's: its vtables hold Sly 3 functions in Sly 3 slots (Sly 3 inserted virtuals: LO 49 -> 53).
// So CLS::PfnOrig(slot2) = the Sly 2 twin (gen/fnmap.inc) of the Sly 3 function in the paired Sly 3 slot
// (gen/vtslots.inc), and a Sly 3 function without a twin gives FN3_TAG | its address (never a Sly 2 address).
// Natives written from the Sly 3 asm register as NATIVEFN3(addr3, fn) (FN3_TAG | addr3) and are found first.

#include "cls.h"

#include "disc.h"
#include "game/sly3.h"

#include <array>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
struct VTROW
{
    int cid3, slot3, slot2;
};
constexpr VTROW c_avtrow[] = {
#define VTSLOT(c, s3, s2) {c, s3, s2},
#include "game/gen/vtslots.inc"
#undef VTSLOT
};

constexpr int SLOT_Max = 160;

// slot2 -> slot3 per Sly 3 class (-1: none)
std::vector<std::array<int16_t, SLOT_Max>>& SlotMaps()
{
    static std::vector<std::array<int16_t, SLOT_Max>> s_a;
    return s_a;
}

std::unordered_map<uint32_t, uint32_t>& Fn2From3()
{
    static std::unordered_map<uint32_t, uint32_t> s_m;
    return s_m;
}

}  // namespace

void InitVtMap3()
{
    // the reverse function map: the best-scored Sly 2 twin of each Sly 3 function (scores below SLY3_FNMAP_MIN, 50 by
    // default, are not trusted for dispatch)
    static const int s_scoreMin = std::getenv("SLY3_FNMAP_MIN") ? std::atoi(std::getenv("SLY3_FNMAP_MIN")) : 50;
    std::unordered_map<uint32_t, int> mpscore;
    // pairs checked by hand against both executables (tools/s3maps/sbs.py), missing or wrong in gen/fnmap.inc
    struct PAIR { uint32_t va2, va3; };
    static constexpr PAIR c_apairManual[] = {
        {0x188960, 0x1AA630},   // Sly's motion (vt slot 70 / 74): the same dispatch on the state, Sly 3's numbers
        {0x140D00, 0x153498},   // RegisterActorContacts (vt slot 59 / 62; the player code, sbs.py: the same calls, Sly 3's offsets)
        // (the AI code) STEPGUARD's contact flags (Sly 2 slot 85 / Sly 3 96) and motion (70 / 74). Sly 3 asks the guard's job
        // stack (+0x29C, 0x238888 / 0x238828: each job's slot 12 / 13 from the top, 0x1000 = no opinion) where Sly 2
        // asked the move job and the states 0x12..0x16 (0x20B090); the AI keeps Sly 2's jobs, so Sly 2's functions.
        // Without them the guards had no floor contacts (flags 0) and never moved.
        {0x20B090, 0x256CB0},
        {0x204108, 0x257548},
    };
    for (const PAIR& r : c_apairManual) mpscore[r.va3] = 1000, Fn2From3()[r.va3] = r.va2;
    for (const sly3::gen::MAPROW& r : sly3::gen::afn)
    {
        if (r.v3 <= 0 || r.score < s_scoreMin) continue;
        const uint32_t a3 = uint32_t(r.v3);
        auto it = mpscore.find(a3);
        if (it != mpscore.end() && it->second >= r.score) continue;
        mpscore[a3] = r.score;
        Fn2From3()[a3] = uint32_t(r.v2);
    }

    // the slot pairs (gen/vtslots.inc, tools/s3maps/gen_vtslots.py): a class has its base's, then its own rows
    auto& a = SlotMaps();
    a.assign(CID_Max, {});
    for (auto& m : a) m.fill(-1);
    std::vector<std::vector<const VTROW*>> aprow(CID_Max);
    for (const VTROW& r : c_avtrow)
        if (r.cid3 >= 0 && r.cid3 < CID_Max) aprow[r.cid3].push_back(&r);
    std::vector<bool> afDone(CID_Max, false);
    auto build = [&](auto&& self, int cid) -> void {
        if (afDone[cid]) return;
        afDone[cid] = true;
        if (const CLS* pb = g_acls[cid].pclsBase)
        {
            self(self, pb->cid);
            a[cid] = a[pb->cid];
        }
        for (const VTROW* pr : aprow[cid])
            if (pr->slot2 >= 0 && pr->slot2 < SLOT_Max) a[cid][pr->slot2] = int16_t(pr->slot3 < SLOT_Max ? pr->slot3 : -1);
    };
    for (int cid = 0; cid < CID_Max; ++cid) build(build, cid);
    void DumpVtMap3();
    DumpVtMap3();
}

int Slot3FromSlot2(CID cid, int slot2)
{
    const auto& a = SlotMaps();
    if (cid < 0 || cid >= int(a.size()) || slot2 < 0 || slot2 >= SLOT_Max) return -1;
    return a[cid][slot2];
}

uint32_t Fn2FromFn3(uint32_t fn3)
{
    if (!fn3) return 0;
    auto it = Fn2From3().find(fn3);
    return it == Fn2From3().end() ? (FN3_TAG | fn3) : it->second;
}

uint32_t CLS::PfnOrig3(int slot3) const
{
    if (!vtOrig || slot3 < 0) return 0;
    return g_disc.U32Elf(vtOrig + 8u * static_cast<uint32_t>(slot3) + 4u);
}

uint32_t CLS::PfnOrig(int slot2) const
{
    if (!vtOrig) return 0;
    const int slot3 = Slot3FromSlot2(cid, slot2);
    if (slot3 < 0) return 0;
    return Fn2FromFn3(PfnOrig3(slot3));
}

// (debug: SLY3_VTDUMP=cid (hex): the class's Sly 2 slots, their Sly 3 slots and functions and the natives found)
#include "vtnative.h"
#include <cstdio>
void DumpVtMap3()
{
    const char* sz = std::getenv("SLY3_VTDUMP");
    if (!sz) return;
    const int cid = int(std::strtol(sz, nullptr, 16));
    if (cid < 0 || cid >= CID_Max) return;
    const CLS& c = g_acls[cid];
    std::printf("vtmap3: cid 0x%02X %s vt 0x%06X\n", cid, c.name, c.vtOrig);
    for (int s2 = 0; s2 < SLOT_Max; ++s2)
    {
        const int s3 = Slot3FromSlot2(cid, s2);
        if (s3 < 0) continue;
        const uint32_t f3 = c.PfnOrig3(s3), f2 = c.PfnOrig(s2);
        const char* n3 = NameNativeFn(FN3_TAG | f3);
        const char* n2 = NameNativeFn(f2);
        std::printf("  slot2 %3d slot3 %3d fn3 0x%06X twin 0x%08X native %s%s\n", s2, s3, f3, f2, n3 ? n3 : n2 ? n2 : "-", n3 ? " (Sly 3)" : "");
    }
}
