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
#include "so.h"
#include "stepphys.h"

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

// the tables bound for a class (0x13FF70's base, then the class's load / post-load ones)
int CtablesOf(const ALO* palo, PARTTABLE* at)
{
    int c = 0;
    at[c++] = {S3VA(0x2BC9F0), 0x35};
    switch (palo->Cid())
    {
    case S3CID(0x35): at[c++] = {S3VA(0x2BE990), 0xAD}; at[c++] = {0x2BF1B0, 0x12}; break;
    case S3CID(0x37): at[c++] = {S3VA(0x2BC390), 0x76}; break;
    case S3CID(0x36): at[c++] = {S3VA(0x2C0270), 0x89}; break;
    default: break;
    }
    return c;
}

// orig 0x00203D00's argument: the entry the part at ib names (-1: none)
int IsoePart(ALO* palo, uint32_t ib)
{
    PARTTABLE at[3];
    const int ct = CtablesOf(palo, at);
    int tag = -1;
    for (int t = 0; t < ct && tag == -1; t++)
        for (int i = 0; i < at[t].c; i++)
            if (g_disc.U32Elf(at[t].va + 12u * i + 8) == ib && (g_disc.U32Elf(at[t].va + 12u * i) & 0x20))
            {
                tag = static_cast<int>(g_disc.U32Elf(at[t].va + 12u * i + 4));
                break;
            }
    if (tag == -1) return -1;
    const int isoeCustom = StepPhysOf(palo).isoeCustom;
    for (size_t i = 0; i < palo->asoe.size(); i++)
        if (palo->asoe[i].tag == tag && static_cast<int>(i) != isoeCustom) return static_cast<int>(i);
    return -1;
}
}  // namespace

// orig: 0x1400B8 / 0x188608, geometry entries bound through the actor part tables.
int IsoeActorPart(ALO* palo, uint32_t ib)
{
    return IsoePart(palo, ib);
}

// orig: 0x0019C870 (the jump's: its parts +0xC0 (+0x380) below state 11, the +0x4C0 sub-object's +0x60 (+0x520) in
// 11..13, +0xC4 (+0x384) in 17; never snapped)
int IsoeJumpMesh(ACTOR* pa, bool* pfSnap)
{
    *pfSnap = false;
    const int st = pa->jump.state;
    if (st < 11) return IsoePart(pa->palo, 0x380);
    if (st < 14) return IsoePart(pa->palo, 0x520);
    if (st == 17) return IsoePart(pa->palo, 0x384);
    return IsoePart(pa->palo, 0x380);
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
        // (Sly 3, interim: Sly 2's fallback chain 0x2BFB28 is not in Sly 3's data; orig3 0x1C4088 walks another
        // record (+0x3740 / +0x38): not ported, no fallback mesh)
        (void)r;
        return -1;
    }
    return -1;
}

// orig: 0x001944C8 (the interaction's: kind 0 its part +0x54 (+0x514), +0x60 (+0x520) in state 5; kind 1 +0x58 (+0x518);
// kind 8 in the states 9 / 10 +0x64 (+0x524) snapped; else none)
int IsoeInteractMesh(ACTOR* pa, bool* pfSnap)
{
    *pfSnap = false;
    switch (pa->n4C8)
    {
    case 0: return IsoePart(pa->palo, pa->nSub4CC == 5 ? 0x520 : 0x514);
    case 1: return IsoePart(pa->palo, 0x518);
    case 8:
        if (pa->nSub4CC == 9 || pa->nSub4CC == 10)
        {
            *pfSnap = true;
            return IsoePart(pa->palo, 0x524);
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
        switch (ps->state)
        {
        case 7: isoe = IsoeInteractMesh(pa, &fSnap); break;
        case 4: isoe = IsoeDamageMesh(pa, &fSnap); break;
        case 2: isoe = IsoeJumpMesh(pa, &fSnap); break;
        case 21: case 22: isoe = IsoePart(palo, 0x520); break;
        default: isoe = IsoePart(palo, 0x1720u + 4u * static_cast<uint32_t>(ps->state)); break;
        }
        if (isoe == -1) isoe = IsoePart(palo, 0x1720);
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
    else
        return;
    if (std::getenv("SLY2N_STEPMESH"))
        std::printf("stepmesh %d: entry %d (tag %d)%s\n", palo->oid, isoe,
                    isoe >= 0 && isoe < static_cast<int>(palo->asoe.size()) ? palo->asoe[isoe].tag : -1, fSnap ? " snapped" : "");
    if (isoe >= 0) SetStepMesh(palo, isoe, fSnap);  // 0x203D00
}
