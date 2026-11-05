#pragma once

// The actors' animation side (part of the animation code; the actors code's state logic calls it):
//
//  - GAIT: the gait pair of the players and guards (0x20B840 .. 0x20BD68): a blend of two locomotion ASEGs (walk
//    and run, ...) aligned by their feet's events, its mix from the stick / speed, the actor's velocity from the
//    blended root motion, the footstep sounds when the feet's segments wrap.
//  - root-motion blends of the actor post-load (0x1A0300: 8 entries with their root displacement per second,
//    0x19DE58: 2 entries with their height slope) and plain blends (NewBlend in blend.h: 0x1959C0, 0x1910F8, ...).
//  - ACTANIM: the actor's current animation (orig +0x1030 ASEGA, +0x1034 SMA) as the SetState functions (players
//    0x135248, 0x1B6078, the guards' equivalents) replace it: the ASEG to play (or a state of an SM), the gait
//    phase kept across gait states, the seek time from an event.

#include "blend.h"

#include <glm/glm.hpp>

#include <functional>

struct ALO;
struct ASEGA;
struct SM;
struct SMA;

// orig: 0x0012FF10 PchnFindAseg: paseg's channel driving palo's own record of kind k (fRoot: only the root-motion
// channels, bit 30: those ApplyAseg does not play)
const ASEGCHN* PchnFindAseg(ASEG* paseg, ALO* palo, int k, bool fRoot);
// The displacement per second of paseg's root-motion translation channel on palo over its length: (pos(tMax) -
// pos(0)) / tMax (0x20B868); false when it has none
bool FAsegRootVelocity(ASEG* paseg, ALO* palo, glm::vec4* pvel);

// The gait pair (orig 0xA4 bytes in the actor: +0x1320 / +0x1600 (players), +0x1C30 (Sly), +0x1540 (Bentley), ...)
struct GAIT
{
    ALO* palo = nullptr;          // +0x0 the actor
    ASEGBL* pbl = nullptr;        // +0x4 the blend (BindGait)
    ASEG* apaseg[2]{};            // +0x14 / +0x54: the two ASEGs (the actor's options set them; 1 may be null)
    glm::vec4 avel[2]{};          // +0x20 / +0x60: their root velocities (BindGait)
    float gMix = 0.0f;            // the mix of the second (+0x50, its BL weight)
    int sfx = -1;                 // +0x90 the footstep sound
    float gVolLo = 0.6f;          // +0x94 its volume at mix 0
    float gVolHi = 1.0f;          // +0x98 at mix 1
    int afFoot[2]{};              // +0x9C / +0xA0: each foot's segment state (the sound plays when it wraps)
};

// orig: 0x0020B840 (the actor, the footstep sound)
void InitGait(GAIT* pgait, ALO* palo, int sfx);
// orig: 0x0020B868: the blend of the two ASEGs: their feet's tables (timed events of kind 11: 0x17D / 0x17E, then
// 0x17F / 0x180), their root velocities, each channel timed by its record's tag (16 / 18 / 20: table 1, 17 / 19 / 21:
// table 0, others: the plain phase)
void BindGait(GAIT* pgait);
// orig: 0x0020BAD8: the blended root velocity (each one weighted by its mix and its first segment's length ratio)
glm::vec4 VecGaitVelocity(const GAIT* pgait);
// orig: 0x0020BB58: the mix g (the second ASEG's weight; 0 without it), RecalcBlend; returns the actor's velocity
// (orig: written to the actor's +0x190): -(VecGaitVelocity) x the blend's first ASEGA's speed
glm::vec4 SetGaitMix(GAIT* pgait, float g);
// orig: 0x0020BC08: the footstep sounds (call each frame while the gait plays)
void UpdateGaitFeet(GAIT* pgait);
// orig: 0x0020BD68
void ResetGaitFeet(GAIT* pgait);
// The footstep hook (the media code's sound): sound, the ASEGA's object (the position), volume
extern std::function<void(int sfx, ALO* palo, float gVolume)> g_pfnGaitFootstep;

// orig: 0x001A0300: the blend of up to 8 ASEGs with each one's root displacement (pos(0) - pos(tMax)) / tMax
// (avel[i], as the original computes it; zero without a root channel)
struct ROOTBLEND
{
    ASEG* apaseg[8]{};
    glm::vec4 avel[8]{};
    ASEGBL* pbl = nullptr;
};
void BindRootBlend(ROOTBLEND* prb, ALO* palo, int c = 8);
// orig: 0x0019DE58: the blend of 2 ASEGs with each one's height slope (z(0) - z(tMax)) / tMax
struct SLOPEBLEND
{
    ASEG* apaseg[2]{};
    float agSlope[2]{};
    ASEGBL* pbl = nullptr;
};
void BindSlopeBlend(SLOPEBLEND* psb, ALO* palo);
// A plain blend of the ASEGs (0x137A30 without tables: 0x1959C0 (5), 0x1910F8 (3), 0x21F388 (2 / 4 / 2))
ASEGBL* PblNewPlain(ALO* palo, const std::vector<ASEG*>& apaseg);
// Weights of a blend (then RecalcBlend 0x1384C0)
void SetBlendWeights(ASEGBL* pbl, const std::vector<float>& aw);

// The stepping actors' six animation segments (STEP, the guards' base: orig +0x17C {id, ASEG*, grf} x 6, the playing
// ASEGA +0x1C4; step.cpp 0x1B0B78 .. 0x1B1BF0): one per stepping state (0 rest, 1 walk, 2 none, 3, 4, 5)
struct STEPSEGS
{
    OID aoid[6]{-1, -1, -1, -1, -1, -1};  // set by the actor's options (0x1B1B18), found at the load
    ASEG* apaseg[6]{};
    int agrf[6]{8, 8, -1, 0, 8, -1};      // D_002C0230: the ApplyAseg flags (| 1)
    ASEGA* pasega = nullptr;
};
// orig: 0x001B1B18: segment n's id (and flags unless -1); not 2 or 5
void SetStepSeg(STEPSEGS* pss, int n, OID oid, int grf);
// orig: 0x001B1BF0 / 0x1B1B98: the segments by id found under the actor (kind-1 find), removed from the world
void BindStepSegs(STEPSEGS* pss, ALO* palo);
// orig: 0x001B1A38: a segment's step length per second: |x(tMax) - x(0)| of its root channel / tMax x its speed
// (+0x78); false without a root channel (the length is then kept)
bool FStepLength(ASEG* paseg, ALO* palo, float* ps);
// The tail of 0x1B1328 (a new stepping state): its segment played from 0 (ApplyAsegCur, grf | 1); none in state 2
void PlayStepState(STEPSEGS* pss, ALO* palo, int st);

// The actor's animation (orig +0x1030 ASEGA, +0x1034 SMA, +0x1A0 the gait phase)
struct ACTANIM
{
    ASEGA* pasega = nullptr;
    SMA* psma = nullptr;
    float uGait = 0.0f;
};
// The end of the SetState functions (0x135248 / 0x1B6078): the current ASEGA and SMA dropped, paseg played on palo
// from t at speed svt (ApplyAsegCur 0x1305B8; grfapl 1 seek into it, 9 seek and loop), psm run from state oid
// (0x1F14F8 with the actor's mask c; -1 no state). fSeekEvent (0x1B6078): the seeking records arrive by the time of
// paseg's timed kind-11 event 0x1AC (when it has one) at the ASEGA's speed.
void SetActorAnim(ACTANIM* pact, ALO* palo, ASEG* paseg, float t, float svt, int grfapl, SM* psm = nullptr,
                  OID oidState = -1, uint32_t grfMask = 4, bool fSeekEvent = false);
void SetActorAnimPp(ASEGA** ppasega, SMA** ppsma, ALO* palo, ASEG* paseg, float t, float svt, int grfapl, SM* psm = nullptr,
                  OID oidState = -1, uint32_t grfMask = 4, bool fSeekEvent = false);
// Leaving a gait state (0x135248 states 1 / 7, 0x1B6078 1 / 10): the phase of the playing gait
void SaveGaitPhase(ACTANIM* pact);
// Entering one: the gait played at the kept phase (seek and loop, grfapl 9), its feet reset
void PlayGait(ACTANIM* pact, GAIT* pgait);
