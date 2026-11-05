#pragma once

// The guards hit (aihit.cpp): the reaction job's message 12 (0x176BE8), the hit jobs 0xA1 / 0xA2 / 0xA3 (their entries
// from the arrays +0xA70 / +0x8D0 / +0xC10 by the hit's kind), the reaction after one (JOBREACT::NextReaction 0x177BA0).
// Part of the animation / AI code.

#include "aijobs.h"

#include <memory>

struct CTMSG;

// orig: 0x0020A298: a hit the guard did not see coming
bool FGuardSurprised(GUARD* pg, LO* psource, uint64_t mask);
// orig: 0x0020A378: the generic launch / grab acceptance gate
bool FGuardLaunchAllowed(GUARD* pg, LO* psource, uint64_t mask);
// orig: 0x00209EE0: release a held/launched guard and limit its velocity
void ReleaseGuardHeld(LO* plo);
// orig: 0x00209E28: reaction 6 (held, job 0xB0) with the holder as its argument
void ReactGuardHeld(LO* plo, LO* ploHolder);
// orig: 0x00209E88: a held guard (state 0x16 or 0x12) now carried (0xB0 state 1)
void CarryGuardHeld(LO* plo);
// orig: 0x00209FB8: a held / carried guard (state 0x16 or 0x13) thrown or put down (0xB0 state 3)
void ThrowGuardHeld(LO* plo);
// orig: 0x00207530 (guardphys.cpp: vt slot 86): a hit that knocks it out at once (deeper than +0x338, of the kinds that hurt)
bool FGuardHitMatters(GUARD* pg, const CTMSG* pctm);

// 0xA0 (0xC0): a hit (a move playing the hit's entry, facing where it came from)
struct JOBHIT : JOBMOVE
{
    AIMSG::HIT hit;               // +0x50 the hit (the message's +0x10..)
    float tB0 = 0, tB4 = 0;       // +0xB0 / +0xB4
    void Enter() override;
    void Exit() override;
    void Update() override;
    void EnterKind(int iarray, int nEvent);
};
struct JOBHIT1 : JOBHIT { void Init() override; void Enter() override; };  // 0xA1 (nAnim 5)
struct JOBHIT2 : JOBHIT { void Init() override; void Enter() override; void Exit() override; };  // 0xA2 (6)
struct JOBHIT3 : JOBHIT { void Init() override; void Enter() override; };  // 0xA3 knocked out (7)

std::unique_ptr<AIJOB> PjobNewHit(CID cid);
