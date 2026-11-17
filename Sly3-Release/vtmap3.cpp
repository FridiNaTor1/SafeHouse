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
        // (s3-player) BREAKABLE 0x6F's contact message (Sly 2 slot 75 0x1BADF0): Sly 3's is 0x1E4EE8 (the decomp: the test
        // 0x1E4FC8 split out (valid event, the hit bit, the trigger classes), then the deferred hit 0x1E5070); the
        // function map paired 0x1BADF0 with the test, so no breakable took a hit
        {0x1BADF0, 0x1E4EE8},
        // (s3-player) Murray (Sly 2 cid 0x36 / Sly 3 0x35): his Sly 3 overrides call the bases and add his carry / throw
        // state 22 (the decomp: 0x1E1D88 -> 0x24F068, 0x1E1C68 -> 0x24E9E0, 0x1E2AC8 -> 0x243F38, 0x1E0D68 -> 0x156948);
        // paired with Sly 2's Murray slots 54 / 58 / 66 / 61 / 70, so his motion (and the pad) runs at all
        {0x1B50B0, 0x1DD590},   // UpdateMurray (slot 16 / 18; the function map paired it with 0x22C308, score 50)
        {0x1B67F8, 0x1DEF10},   // UpdateMurrayMotion (slot 70 / 74)
        {0x1B8450, 0x1E0D68},   // his contact flags (slot 61 / 65)
        {0x204C60, 0x1E1D88},   // SetStepOverlap (slot 54 / 58)
        {0x140D70, 0x1E1C68},   // FindStepFloor (slot 58 / 61)
        {0x1FA328, 0x1E2AC8},   // (slot 66 / 70)
        // (s3-gadgets) Bentley's messages (vt slot 5): Sly 3's 0x13EE68 is Sly 2's 0x135AD8 (the base's handler first, then
        // his animation events: the summon 0x264 (Sly 2 0x216), the dart flags) plus the Fishing Pole's events in state
        // 16; it had no pair, so none of his animation events reached the native handler
        {0x135AD8, 0x13EE68},
        // (s3-player) the pickups' motion (Sly 2 slot 17 / Sly 3 slot 19 of 0x67 / 0x68 ..): Sly 3's 0x1F68C8 lets the
        // position record integrate (damping -5 in its states 0 / 4, gravity D_00346FD0 in 2 / 3 through 0x244828, then
        // the ALO's 0x28CA78) where Sly 2's 0x1C5148 integrates +0x170 / +0x180 itself; the native pickup keeps Sly 2's
        // states and motion. Unpaired, no launched pickup ever moved (coins and prizes thrown from a pocket hung in the
        // air in state 3: the Ep4 webcam's pickpocket prize 88325 was never collected)
        {0x1C5148, 0x1F68C8},
        // (s3-gadgets) Murray's messages (vt slot 5): Sly 3's 0x1DEFE8 is Sly 2's 0x1B68B8 with Sly 3's event ids (gang.cpp
        // OnMurrayMessage); unpaired like Bentley's, so his flop, throw, carry and punch events never arrived
        {0x1B68B8, 0x1DEFE8},
        // (s3-player round 18) Murray's jump kind (Sly 2 slot 89 0x1B7F90 / Sly 3 0x1E08F8, his vtable 0x35DC50 +0x320):
        // the function map paired 0x1B7F90 with 0x22CFA8, so his slot had no native and every query of his kind gave -1:
        // a script clip's wait (SCPT 0x16, 0x1566D0) ended at once (m4_rccar's 69353 let him go from the fireworks cart
        // the frame after it harnessed him). Sly 3's table is on its own states (+0x1520); the native Murray keeps Sly 2's
        // states, so Sly 2's table (the inverse of the native slot 88, already paired 0x1B7E40 / 0x1E0770)
        {0x1B7F90, 0x1E08F8},
        {0x140D00, 0x153498},   // RegisterActorContacts (vt slot 59 / 62; the player code, sbs.py: the same calls, Sly 3's offsets)
        // (the AI code) STEPGUARD's contact flags (Sly 2 slot 85 / Sly 3 96) and motion (70 / 74). Sly 3 asks the guard's job
        // stack (+0x29C, 0x238888 / 0x238828: each job's slot 12 / 13 from the top, 0x1000 = no opinion) where Sly 2
        // asked the move job and the states 0x12..0x16 (0x20B090); the AI keeps Sly 2's jobs, so Sly 2's functions.
        // Without them the guards had no floor contacts (flags 0) and never moved.
        {0x20B090, 0x256CB0},
        {0x204108, 0x257548},
        // (s3-ai round 12) the guards' hits (Sly 2 slot 75 / Sly 3 78): Sly 3's 0x251C08 (health, the outcome offered to
        // the job stack as messages 0xD..0x12) had no twin, so the guards fell back to SO's 0x240DA0 (= Sly 2's
        // FAcceptContactAlo: accepted, nothing done) and took no hit at all: no fight could be won. Natively 0x207468's
        // function (guardphys.cpp FAcceptContactGuard) is now Sly 3's 0x251C08 (round 13: the health, the flags, the
        // outcomes handed to the copied reaction job as Sly 2's).
        {0x207468, 0x251C08},
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
    // (s3-player) the Guru 0x37: gen_vtslots.py gave him no slots (every base slot -1: no update, motion or messages
    // reached him). His vtable 0x3646C8 has Bentley's (0x352788) layout, slots 0..120 (checked word by word: the base's
    // functions in the same slots, his own where Bentley has his), so he takes Bentley's map; his own functions are
    // natives by their Sly 3 address (game/guru3.cpp)
    if (sly3::CID_GURU < CID_Max && sly3::CID_BENTLEY < CID_Max) a[sly3::CID_GURU] = a[sly3::CID_BENTLEY];
    // (s3-player round 21) Penelope 0x39 (vtable 0x35F0F8, 121 slots, the same layout as the Guru's: the base's functions in
    // the same slots, her own where he has his) likewise; her own are natives by their Sly 3 address (game/guru3.cpp)
    if (sly3::CID_PENELOPE < CID_Max && sly3::CID_BENTLEY < CID_Max) a[sly3::CID_PENELOPE] = a[sly3::CID_BENTLEY];
    // (s3-player round 18) Murray 0x35's slot 89 (the jump kind) is his Sly 3 slot 100 (vtable 0x35DC50 +0x320 =
    // 0x1E08F8, next to slot 88 = 99 = 0x1E0770, as Bentley's 0x13F040 / 0x13F128): gen_vtslots.py left it out
    if (sly3::CID_MURRAY < CID_Max && a[sly3::CID_MURRAY][89] < 0) a[sly3::CID_MURRAY][89] = 100;
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
