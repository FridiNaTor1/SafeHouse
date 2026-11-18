#pragma once

// The Guru (Sly 3 class 0x37 GURU, vtable 0x3646C8, module 0x22C188..0x22D538; Episode 2's o_ext / o_quarry): a player
// actor on the player base 0x33 (actor.h ACTOR) with a small state machine of his own. From the Sly 3 decomp
// (src/leaf/actor_defaults_22c188.c, actor_tick_22c308.c, actor_action_update_22c698.cpp, state_change_22ca70.c,
// src/hand/func_0022D3E8.c, func_0022D538.c, func_0022D318.c, ...). The Sly 2 function map paired his functions with
// Sly 2's Murray (Sly 2's Murray file became Sly 3's Guru file); the code here is his own.
//
// His states (+0x1470): 0 standing (the idle SM +0x1134), 1 walking (the gait +0x1480: R1 held, or +0x13E8), 2 running
// (the gait +0x1530, mixed by the stick), 3 the jump (+0x320, jump.cpp), 4 the interaction (+0x500, interact.cpp),
// 5 the damage (+0xE40, damage.cpp), 6 riding (possessing) a guard (+0x15E0's link: the leap of jump state 17 onto it, airide3.cpp's job 0xE8), 7 an effect's
// wait (+0x10D0; not ported yet), 8 a script's animation (the jump kind 5).

#include "actanim.h"
#include "actor.h"

#include <vector>

struct ALO;
struct LO;
struct SW;
struct STEP;
struct SM;

namespace sly3
{
struct GURU : ACTOR
{
    int state = -1;                 // +0x1470
    float tState = 0.0f;            // +0x1474
    GAIT gaitWalk;                  // +0x1480 (state 1; parts +0x1494 / +0x14D4)
    GAIT gaitRun;                   // +0x1530 (state 2; parts +0x1544 / +0x1584)
    float uPhase = 0.0f;            // +0x1B0 the gait's phase when a gait state is left (0x22CA70 states 1 / 2)
    int fWalk13E8 = 0;              // +0x13E8 always walk (state 1 instead of 2)
    SM* psmRide = nullptr;          // +0x15EC (part oid 767; the idle SM +0x1134 when unset): state 6's
    LO* ploRide = nullptr;          // +0x15E0's object (state 6: the object ridden; 0x6D jumps off it)
    LO* ploRideLast = nullptr;      // +0x15F0's (the object left, 0.1 s)
    float tRideLast = 0.0f;         // +0x15F4

    // (the Panda King 0x38, AUDIT4 PLAYER-01: orig3 0x1E9330..0x1EB340 on the same layout; his states: 0..2 as the
    // Guru's, 3 his third gait (+0x15E0) while the fireworks are held (+0x16A8 0 / 1), 4 the jump (+0x320), 5 the
    // interaction (+0x500), 6 the damage (+0xE40), 7 the combo (+0x10D0), 8 a script's animation)
    GAIT gaitThird;                 // +0x15E0 (parts +0x15F4 / +0x1634)
    ASEG* apasegFw[4]{};            // +0x1690.. the fireworks' clips of their states 0..3 (parts 1792 / 1793 / 1791 / 1794)
    ASEGA* pasegaFw = nullptr;      // +0x16A4
    int stateFw = -1;               // +0x16A8 (-1 off, 0 charging, 1 charged, 2 fired, 3 fired by the time; 4 off by a script)
    float tStateFw = 0.0f;          // +0x16AC
    struct FWTARGET { LO* ploOwner = nullptr; LO* ploMarker = nullptr; float t = 0.0f; };
    FWTARGET atarget[4];            // +0x16B0 (the markers follow their owners)
    int ctarget = 0;                // +0x16E0
    int cCharge = 0;                // +0x16E4 (event 0x704 of the fireworks' clip; EOPID 1887)
    int cChargeMax = 4;             // +0x16E8 (EOPID 1886)
    int fFwHeld = 0;                // +0x16EC (a script's hold: EOPIDs 1889 / 1890)
    float dtTargetKeep = 0.5f;      // +0x16F0 (EOPID 1888)
    LO* ploMarkerTmpl = nullptr;    // +0x16F4 (part 1795)
    LO* ploFwCount = nullptr;       // +0x16F8 (EOPID 1881: a screen object, its part 0x3CA the charges)
    LO* ploFwGlow = nullptr;        // +0x16FC (EOPID 1882: its part 0x37C's fraction +0x1700)
    float uFwGlow = 0.0f;           // +0x1700
};

bool FIsGuru3(const LO* plo);
bool FIsPandaKing3(const LO* plo);       // (class 0x38: runs on the GURU struct with his own states)
// orig3 vt slot 102 (Sly 2's 91, "the selection reset"): every actor's combo off (0x155CE0); the Panda King's 0x1EA178
// then his fireworks fired (charged with a charge) or off. Called where Sly 3 calls it: the damage's start (0x1C21F0)
void ActorSlot102Sly3(ACTOR* pa);
int NGuruNoisy3(ACTOR* pa);              // orig3 slot 112 (Guru 0x22D028, Penelope 0x1F0E38, the Panda King 0x1EA070); -2 not one
void GuruCpdefi3(LO* plo, float* pdTilt, float* pdDist, bool fBefore);   // orig3 slot 85 (the Panda King's 0x1EA298): after
bool FPkMovedNow3(LO* plo);             // its part before the base: the last move time set now
bool FGuruCanDo3(LO* plo, int n);        // orig3 0x22D1F8 (vt slot 113: may the action n start)
GURU* PguruFromLo(LO* plo);
STEP* PstepNewGuru3(ALO* palo);          // (gang.cpp's PstepNewGang)
void InitWorldGuru3(SW* psw);            // the init 0x22C188 and the post-load 0x22C2B0 of each Guru (gang.cpp's InitWorldGang)
void SetGuruState3(GURU* p, int n);      // orig3 0x22CA70
void GuruJumpOff3(LO* plo);              // orig3 0x22D318 (SCPT 0x6D)
void UpdateGuruStepMesh3(GURU* p);      // orig3 0x22D080 (vt slot 101; actmesh.cpp)

// The ride links as jump targets (orig3 0x1BBCA8's list 0x479BA4, kind 17; jumptgt.cpp / jump.cpp):
// the actor's target flags (vt slot 106, +0x1110) have 0x1000: the Guru's 0x3080 (Sly 0x2880 / 0x2080 have not)
bool FActorTargetsRideLinks3(const ACTOR* pa);
// the links in the world (class 0x7A: the list's add / remove are its world add / remove 0x1A27D8 / 0x1A2800)
const std::vector<LO*>& AploRideLinks3(SW* psw);
// the guard whose ride link (option 494) is plink (a guard given directly is its own)
LO* PloRideGuard3(LO* plink);
// orig3 0x1A29B0: the link's rider (+0x178; null none)
LO* PloLinkRider3(LO* plink);
// orig3 0x1BD520 case 17 (the leap's end): the Guru (D_0046FF30) +0x15E0 = the link, his state 6 (0x22CA70)
void ArriveRideLink3(LO* plink);
}  // namespace sly3
