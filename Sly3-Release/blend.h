#pragma once

// ASEGBL (cid 0x6E): the blend of several animation segments (module 0x137A30-0x138F08, Sly 1's blend.c). An
// actor builds one from its BL entries {weight, ASEG} (NewBlend 0x137A30), plays it like any ASEG (ApplyAseg on
// the actor) and moves its weights (then RecalcBlend 0x1384C0). The blend's channels are the union of the BLs'
// channels; each channel's track evaluates every weighted BL's track at its own phase and sums them by weight.
//
// Phase: without a segment table a BL's time is u x its length, u the blend's time over the blend's length (the
// weighted sum of the BLs' lengths). With one (the gaits: the times of events of kind 11 in each BL, e.g. the feet's
// contacts) the blend's time is looked up in the blended table (ImrsgLookUp 0x138370) and each BL plays the same
// segment at the same fraction, so the feet land together.
//
// Part of the animation code. In the original the BL array lives in the actor (stride cb, the table times at byte
// offsets ib); natively the blend owns its BLs (abl) and the tables are indexed.

#include "sm.h"

#include <glm/glm.hpp>

#include <vector>

struct ALO;
struct XF;

// A segment of a table: its start time and its length to the next (wrapping)
struct MRSG
{
    float t = 0.0f;
    float dt = 0.0f;
};

// A BL entry (the original's {float weight, ASEG* paseg, ...} of the actor's array)
struct BLENDBL
{
    float weight = 0.0f;               // +0x0 (the actor sets it, then RecalcBlend)
    ASEG* paseg = nullptr;             // +0x4
    std::vector<std::vector<MRSG>> aamrsg;  // the BL's segment tables (orig: at +ib of the entry), by table index
};

// orig: ASEGBL (cid 0x6E, 0xE0 bytes)
struct ASEGBL : ASEG
{
    std::vector<BLENDBL> abl;          // orig +0xC0 stride, +0xC4 count, +0xC8 the array
    std::vector<std::vector<MRSG>> aamrsg;  // orig +0xCC / +0xD0: the blended tables (the BLs' by weight)
    int iblDominant = -1;              // orig +0xD4: the BL of weight > 0.9999 (its tracks are used directly)
    ASEG* pasegSkel = nullptr;         // orig +0xD8: the BL whose events the blend has (times scaled to its length)
    float TMax() const { return asegd.tMax; }  // orig +0x74
};

// kind 5 (0x2CA9E0?): a blended position channel
struct TRKPBL : TRKP
{
    ASEGBL* pbl = nullptr;             // +0x4
    int itable = -1;                   // +0x8 the segment table that times it (-1: none, the plain phase)
    std::vector<const TRKP*> atrk;     // +0xC per BL: its channel's track (null: the BL has no such channel)
    glm::vec4 v{0.0f};                 // +0x10 the value for BLs without the channel (the record's value then)
    void Load(CBinaryInputStream*) override {}
    void Setup(XF* pxf) const override;  // orig 0x138660
    void Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const override;  // orig 0x138970
};

// kind 2: a blended scalar channel
struct TRKGBL : TRKG
{
    ASEGBL* pbl = nullptr;
    int itable = -1;
    std::vector<const TRKG*> atrk;
    float g = 0.0f;                    // +0x10
    void Load(CBinaryInputStream*) override {}
    void Eval(float* pg, float* pdg, float t, float dtScale) const override;  // orig 0x138BA0
};

// orig: 0x00137FB8 FillBlAmrsg: table itable of each BL: the times of its timed events of kind 11 with the values
// aoid (0 when it has none), each segment's length to the next (wrapping at the BL's length)
void FillBlAmrsg(std::vector<BLENDBL>& abl, int itable, const std::vector<int>& aoid);

// orig: 0x00137A30 NewBlend: the blend of the BLs (their tables already filled, actable[i] segments in table i)
// played on palo: a new removed ASEGBL child of palo with the union of the BLs' channels (position channels of
// records without the channel in a BL hold the record's value of now), the events of the first BL that has some, the
// priority / speed of the first BL; the first BL's weight 1, the others 0, then RecalcBlend. Null without ASEGs.
ASEGBL* NewBlend(ALO* palo, std::vector<BLENDBL> abl, const std::vector<int>& actable);

// orig: 0x001380F0 CalculateBlendAmrsg: the blended table itable (-1: none) and the blend's length (*ptMax)
void CalculateBlendAmrsg(ASEGBL* pbl, int itable, float* ptMax);
// orig: 0x00138370 ImrsgLookUp: the segment of amrsg containing t (wrapping at tMax), *pdt the time into it
int ImrsgLookUp(const std::vector<MRSG>& amrsg, float* pdt, float t, float tMax);
// orig: 0x001383F0: the dominant BL; on a change the records its ASEGAs drive fetch their segment again
void SetBlendDominant(ASEGBL* pbl, int ibl);
// orig: 0x001384C0: after the weights changed: the dominant BL, the tables, the length (the ASEGAs keep their
// phase), the events' times
void RecalcBlend(ASEGBL* pbl);

// The channel's blended tracks (for the actors' per-channel table choice, 0x20B868)
void SetBlendChannelTable(ASEGCHN* pchn, int itable);

// cid 0x6E's constructor (sm.cpp's PfnNewAseg)
LO* PloNewAsegbl();
