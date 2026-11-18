#pragma once
// Sly 3's ridden guard, the AI job 0xE8 (orig3 vt 0x35FC60: Init 0x1F5AE0, Enter 0x1F5B28, Exit 0x1F5B88, Update 0x1F5D70,
// Message 0x1F5BF8; the state change 0x1F6148, the rider's pad given 0x1F5C58 / taken back 0x1F5CF0, the rider told to
// get off 0x1F5CC8, the heading 0x257D78). The classes table calls Sly 3's class 0xE8 JOBNAVPOINT (a weak match with
// Sly 2's 0xBF): it is the Guru's ride (Ep2 m2_shaman): a guard driven by the pad of the one riding it.
//
// How it starts (the decomp): a guard's option 494 (+0x11C8, Sly 3 EOPID 367) is its ride link, an ALO of class 0x7A;
// the Guru's jump of target kind 17 (0x1BD520) puts the link in the Guru's +0x15E0 and the Guru's state 6 (0x22CA70)
// sets the link's rider (+0x178, 0x1A28C8). The guard's reaction job then sees a rider (FContinue 0x19AF68: the link's
// +0x178) and reacts with kind 19 (0x19A2D0 case 19): this job, its +0x38 the rider. The rider's player index (+0x130)
// then drives the guard (0x1F4B00): states 1 / 2 stand / walk by the stick in the player's view, 3 the jump (Cross, when
// the guard's +0x1144 has 0x10), 5 the attack (Square, with a guard action +0x11D0), 6 a hit (message 15, mode 1),
// Triangle (unless the guard's mode +0x11CC is 1) has the rider get off (0x22D318 on the rider, whose state change
// releases the ride: 0x22D2B0 -> 0x1F5CF0).
//
// Ported here: the start / end (broadcasts 0x1A / 0x1B, the guard state 0x41), the rider's pad to the guard and back,
// the states 0 / 1 / 2 / 6 and the Triangle; the walk's feet (natively the move's root velocity by the stick's deflection
// in the stick's heading, applied after the move's steering, as aicarm3's); (s3-ai round 18) the jump (state 3: Sly 3's
// job 0xCB kind 5, 0x131E10 / 0x132C58 / 0x132438 / 0x132A50: the take-off velocity up to the kind object's height
// +0x7C (0x250418, gravity D_003209E0 -2275), the flight's ground speed +0x80 over its time sqrt(8 h / g) toward the
// stick in the rider's view (full speed in modes 0 / 1 / 3), landed: done; natively the jump's move 0x15 with the launch,
// as aicarm3's) and the attack (state 5: Sly 3's job 0xCC 0x1A3860: the guard action +0x11D0's kind entry (+0xB0) played
// by a move, the guard state 3 and +0x107C held meanwhile; an action with +0x68 is the guard's response 0x256828
// instead). Not yet: the hit's launch (state 4, 0xCB kind 4), the wall test 0x1F6300 and its deferred call D_00325490,
// mode 3's swim (0x257D78's +0x1F0), the copied +0x164 / +0x1B8, the detector target 0x170360.

#include "ai.h"

#include <glm/glm.hpp>

struct LO;
struct GUARD;
struct PADCTL;

constexpr CID CID_JOBRIDE3 = 0x1E8;   // (Sly 3-only job ids above 0x1C6 natively: ai.h)

struct JOBRIDE3 : AIJOB
{
    int st = -1;                  // +0x30 the state (0x1F6148)
    float tSt = 0;                // +0x34
    LO* ploRider = nullptr;       // +0x38 the rider (a PO: its +0x130 the player index)
    uint32_t u164 = 0;            // +0x3C the guard's +0x164 at the start
    // (native) the feet / heading the update wants after the move's steering (SteerRide3)
    bool fSteer = false;
    float uStick = 0.0f;
    bool fHeading = false;
    float radHeading = 0.0f;
    // (round 18) the jump (state 3, Sly 3's 0xCB kind 5) and the attack (state 5, 0xCC)
    float sJumpSpeed = 0.0f;      // 0xCB +0xF8: the flight's ground speed (+0x80 / its time)
    glm::vec3 vecJumpFeet{0.0f};  // 0xCB's guard +0x1A0 in the flight (natively vecVel)
    bool fJumpWatchOff = false;   // 0xCB +0x100: the ground watcher held off during the flight (0x259D10 / 0x259D50)
    bool fJumpGravity = false;    // the kind's gravity set for the flight (0x23ED58)
    float gJumpGravity = -2275.0f;
    glm::vec3 gJumpGravityPrev{0.0f, 0.0f, -2275.0f};   // 0xCB +0x90: the gravity before
    bool fAttack107C = false;     // 0xCC: the guard's +0x107C set while the attack plays

    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;   // orig3 0x1F6148
    int Message(int msg, void* pm) override;   // orig3 0x1F5BF8
    void GiveRiderPad();             // orig3 0x1F5C58
    void RiderOff();                 // orig3 0x1F5CC8
    void Release();                  // orig3 0x1F5CF0
    void Jump();                     // (state 3: Sly 3's 0xCB kind 5)
    void Attack();                   // (state 5: Sly 3's 0xCC)
    void EndJumpAttack();
    PADCTL* Ppad() const;
};

// orig3 0x1A28C8 on a ride link (the guard's +0x11C8 object): its rider (+0x178; null: none). For guru3.cpp's state 6:
// SetRideRider3(link, guru) on entry, SetRideRider3(link, nullptr) on exit (after ReleaseGuardRide3)
void SetRideRider3(LO* plink, LO* ploRider);
// the guard's ride link (option 494, +0x11C8; null: none)
LO* PloRideLink3(GUARD* pg);
// orig3 0x19AF68: the rider on the guard's link (null: none)
LO* PloGuardRider3(GUARD* pg);
// orig3 0x22D2B0's part: the guard's ride job (0xE8) releases its rider (0x1F5CF0): the pad back to the rider
void ReleaseGuardRide3(LO* ploGuard);
// the rider's own get-off (orig3 0x22D318 on the rider, guru3.cpp), called on Triangle; null: ReleaseGuardRide3 directly
extern void (*g_pfnRiderOff3)(LO* ploRider);
// (guardphys.cpp GrfGuardXp, the guard's contact flags) the ride's jump in flight: 1 (orig3 0xCB's 0x133530), else -1
int GrfRideXp3(GUARD* pg);
// (guard.cpp UpdateGuard, after the move's steering 0x2097E0) the job's feet and heading applied
void SteerRide3(GUARD* pg);
// a world loaded: no link has a rider
void ResetRide3();
