// actmesh.cpp: the players' vt slot 90 (the step's second mesh by state: Sly 0x18C060, Bentley 0x135D80, Murray
// 0x1B8008) and the sub-objects' choices it asks (the jump 0x19C870, the damage 0x1A23F8, the interaction 0x1944C8).
// From the decomp's matching C (func_0018C060 / 00135D80 / 001B8008 / 001944C8) and the asm (0x19C870, 0x1A23F8).
//
// The meshes are the classes' parts of kind 0x20 (a geometry entry by its tag, bound at the load by 0x1A64E8 into the
// actor at the part's offset): natively they are looked up by the offset in the class's tables (the base 0x2BC9F0, Sly
// 0x2BE990 / 0x2BF1B0, Bentley 0x2BC390, Murray 0x2C0270) and the entry with that tag.
#include "actmesh.h"
#include "game/sly3.h"

#include "actor.h"
#include "alo.h"
#include "lo.h"
#include "disc.h"
#include "gang.h"
#include "game/guru3.h"
#include "so.h"
#include "stepphys.h"
#include "sly.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
struct PARTTABLE
{
    uint32_t va;
    int c;
};

// (Sly 3) the class's part tables in the Sly 3 executable (game/gen/parts.inc's heads): the actors' 0x342DD0 (59), then
// Sly's load 0x345140 (106) / post-load 0x345638 (11), Bentley's 0x342890 (84), Murray's 0x345FD8 (127). Before
// s3-intro the Sly 2 tables were looked up (S3VA of Sly 2's addresses: Sly's mapped to an unrelated 0x322260, its
// post-load table read at Sly 2's 0x2BF1B0) with Sly 2's offsets, which no Sly 3 row has: no player ever changed his
// collision shape (every state kept the standing 252 with its front 90 ahead)
int CtablesOf(const ALO* palo, PARTTABLE* at)
{
    int c = 0;
    at[c++] = {0x342DD0, 59};
    switch (palo->Cid())
    {
    case S3CID(0x35): at[c++] = {0x345140, 106}; at[c++] = {0x345638, 11}; break;
    case S3CID(0x37): at[c++] = {0x342890, 84}; break;
    case S3CID(0x36): at[c++] = {0x345FD8, 127}; break;
    default: break;
    }
    return c;
}

// the rows of game/gen/parts.inc: entry i of the Sly 3 table va3 is stored at ib3 (the Sly 2 code reads it at ib2)
struct PARTROW2 { uint32_t va3; int i; uint32_t ib3; int ib2; };
constexpr PARTROW2 c_apartrow2[] = {
#define PARTMAP(va, i, ib3, ib2) {va, i, ib3, ib2},
#include "game/gen/parts.inc"
#undef PARTMAP
};

int IsoeTag(ALO* palo, int tag)
{
    if (tag == -1) return -1;
    const int isoeCustom = StepPhysOf(palo).isoeCustom;
    for (size_t i = 0; i < palo->asoe.size(); i++)
        if (palo->asoe[i].tag == tag && static_cast<int>(i) != isoeCustom) return static_cast<int>(i);
    return -1;
}

bool FOldStepMesh()
{
    // (debug: SLY3_STEPMESH2=1: the lookups as before s3-intro (Sly 2's tables / offsets: no shape ever changed))
    static const bool s_f = std::getenv("SLY3_STEPMESH2") != nullptr;
    return s_f;
}

// the entry the part at the Sly 3 offset ib3 names (a kind 0x20 row of the class's tables; -1: none)
int IsoePart3(ALO* palo, uint32_t ib3)
{
    if (FOldStepMesh()) return -1;
    PARTTABLE at[3];
    const int ct = CtablesOf(palo, at);
    for (int t = 0; t < ct; t++)
        for (int i = 0; i < at[t].c; i++)
            if (g_disc.U32Elf(at[t].va + 12u * i + 8) == ib3 && (g_disc.U32Elf(at[t].va + 12u * i) & 0x20))
                return IsoeTag(palo, static_cast<int>(g_disc.U32Elf(at[t].va + 12u * i + 4)));
    return -1;
}

// the same by the Sly 2 offset the copied code reads (parts.inc's pairing)
int IsoePart(ALO* palo, uint32_t ib2)
{
    if (FOldStepMesh()) return -1;
    PARTTABLE at[3];
    const int ct = CtablesOf(palo, at);
    for (const PARTROW2& r : c_apartrow2)
    {
        if (r.ib2 != static_cast<int>(ib2)) continue;
        for (int t = 0; t < ct; t++)
            if (r.va3 == at[t].va && (g_disc.U32Elf(r.va3 + 12u * r.i) & 0x20))
                return IsoeTag(palo, static_cast<int>(g_disc.U32Elf(r.va3 + 12u * r.i + 4)));
    }
    return -1;
}
}  // namespace

// orig: 0x1400B8 / 0x188608, geometry entries bound through the actor part tables (by the Sly 2 offset).
int IsoeActorPart(ALO* palo, uint32_t ib)
{
    return IsoePart(palo, ib);
}

// orig3: 0x001BD718 (Sly 2's 0x19C870; the jump's: its parts +0xC0 (+0x390) below the state 12, the owner's +0x560 in
// 12..14, +0xC4 (+0x394) in 18; never snapped). The native jump keeps Sly 2's states (one lower: 11..13, 17).
int IsoeJumpMesh(ACTOR* pa, bool* pfSnap)
{
    *pfSnap = false;
    const int st = pa->jump.state;
    if (st < 11) return IsoePart3(pa->palo, 0x390);
    if (st < 14) return IsoePart3(pa->palo, 0x560);
    if (st == 17) return IsoePart3(pa->palo, 0x394);
    return IsoePart3(pa->palo, 0x390);
}

// orig: 0x001A23F8 (the damage's: the reaction's part +0xD4 + 4 r (+0xEC4..), or the next in the fallback chain
// 0x2BFB28 until -1; never snapped)
int IsoeDamageMesh(ACTOR* pa, bool* pfSnap)
{
    *pfSnap = false;
    int r = pa->nSubDF4;
    if (r == -1) return -1;
    for (int guard = 0; guard < 32; guard++)
    {
        const int isoe = IsoePart(pa->palo, 0xEC4u + 4u * static_cast<uint32_t>(r));
        if (isoe != -1) return isoe;
        // (Sly 3, interim: Sly 2's fallback chain 0x2BFB28 is not in Sly 3's data; orig3 0x1C4088 walks the +0xE40
        // object's records (+0x38 + 16 i, chain D_00323740): not ported, no reaction mesh)
        (void)r;
        return -1;
    }
    return -1;
}

// orig3: 0x001B5C78 (Sly 2's 0x1944C8; the interaction's: kind 0 its part +0x54 (+0x554), +0x60 (+0x560) in state 5;
// kind 1 +0x58 (+0x558); kind 8 in the states 9 / 10 +0x64 (+0x564) snapped; else none)
int IsoeInteractMesh(ACTOR* pa, bool* pfSnap)
{
    *pfSnap = false;
    switch (pa->n4C8)
    {
    case 0: return IsoePart3(pa->palo, pa->nSub4CC == 5 ? 0x560 : 0x554);
    case 1: return IsoePart3(pa->palo, 0x558);
    case 8:
        if (pa->nSub4CC == 9 || pa->nSub4CC == 10)
        {
            *pfSnap = true;
            return IsoePart3(pa->palo, 0x564);
        }
        return -1;
    default: return -1;
    }
}

// orig: 0x0018C060 / 0x00135D80 / 0x001B8008 (vt slot 90)
void UpdateActorStepMesh(ACTOR* pa)
{
    if (!pa || !pa->palo) return;
    ALO* palo = pa->palo;
    bool fSnap = false;
    int isoe = -1;
    if (SLY* ps = PslyFromLo(palo))
    {
        // (orig3 0x1ADBE0 on Sly 3's states +0x19E0: 8 / 5 / 3 their sub-objects', 17 (the hook of kind 3) +0x560, -1
        // none, the others the word +0x1850 + 4 state (bound: 0 the standing 252, 2 the walk 253, 9 the pipe 255); else
        // the standing +0x1850)
        switch (ps->jts)
        {
        case -1: break;
        case jts::Interact: isoe = IsoeInteractMesh(pa, &fSnap); break;
        case jts::Damage: isoe = IsoeDamageMesh(pa, &fSnap); break;
        case jts::Jump: isoe = IsoeJumpMesh(pa, &fSnap); break;
        case jts::HangFollow: case jts::N2ClimbSurf: isoe = IsoePart3(palo, 0x560); break;
        default:
            if (ps->jts >= 0 && ps->jts <= jts::Map) isoe = IsoePart3(palo, 0x1850u + 4u * static_cast<uint32_t>(ps->jts));
            break;
        }
        if (isoe == -1) isoe = IsoePart3(palo, 0x1850);
        if (FOldStepMesh() && isoe == -1) return;
    }
    else if (BENTLEY* pb = PbentleyFromLo(palo))
    {
        switch (pb->state)
        {
        case 3: isoe = IsoeInteractMesh(pa, &fSnap); break;
        case 4: isoe = IsoeDamageMesh(pa, &fSnap); break;
        case 2: isoe = IsoeJumpMesh(pa, &fSnap); break;
        default: break;
        }
        if (isoe == -1) isoe = StepPhysOf(palo).isoeSource;  // +0x268
    }
    else if (MURRAY* pm = PmurrayFromLo(palo))
    {
        switch (pm->state)
        {
        case 13: isoe = IsoeInteractMesh(pa, &fSnap); break;
        case 14: isoe = IsoeDamageMesh(pa, &fSnap); break;
        case 2:
            // (+0x13F0, the guard held: its part +0x1ABC; TODO(actors): Murray's grab 0x1B7050 keeps it)
            isoe = IsoeJumpMesh(pa, &fSnap);
            break;
        case 5: isoe = IsoePart(palo, 0x1AB0u + 4u * static_cast<uint32_t>(pm->n1458)); break;
        default: isoe = IsoePart(palo, 0x1A58u + 4u * static_cast<uint32_t>(pm->state)); break;
        }
        if (isoe == -1) isoe = StepPhysOf(palo).isoeSource;  // +0x268
    }
    else if (sly3::GURU* pg = sly3::PguruFromLo(palo))
    {
        sly3::UpdateGuruStepMesh3(pg);   // (orig3 0x22D080: his parts by their Sly 3 offsets, game/guru3.cpp)
        return;
    }
    else
        return;
    if (std::getenv("SLY2N_STEPMESH"))
        std::printf("stepmesh %d: entry %d (tag %d)%s\n", palo->oid, isoe,
                    isoe >= 0 && isoe < static_cast<int>(palo->asoe.size()) ? palo->asoe[isoe].tag : -1, fSnap ? " snapped" : "");
    if (isoe >= 0) SetStepMesh(palo, isoe, fSnap);  // 0x203D00
}
