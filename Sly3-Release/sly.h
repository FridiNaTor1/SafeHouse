#pragma once

#include <cstdint>

// Sly (cid 0x35, module 0x188488..0x191090, the reference's m_188488.cpp / m_189530_p3.cpp / m_18E540.cpp): his init,
// post-load, update (vt slot 16 0x18BCE0), the state machine (+0x18C8: 0x18C250 the change, 0x18A8C8 the per-frame
// transitions, 0x18B7E8 the controller's input), the attack mode (+0x18D8: 0x18E540), and the physics slots that read
// his state (17 0x18A278, 49 0x18A2E0, 70 0x188960, 85 0x18DEE0). The data are actor.h's SLY.
//
// Ported so far (round 1): the ground states 0 (standing), 1 / 10 (walking), 12 (running), their transitions and
// animation, the controller's heading and gait; the other states' bodies (2 the jump: jump.cpp, 3..24) are the next
// rounds' (each warns once when reached: "sly: ... not ported").

struct LO;
struct SW;
struct SLY;
struct LAUNCHTARGET;

// orig: 0x00188488 / global 0x002E1E40: the most recently initialized Sly, independent of the selected player.
SLY* PslyGlobal();

// The world's start (after the load, the animation's and the physics' post-loads, transition.cpp): every actor's init
// (0x13FF70 and the class's: Sly 0x188488) and post-load (0x1400B8 and Sly's 0x188608).
void InitWorldActors(SW* psw);
// (PC, split screen's teams) a copy of Sly made while the world runs: its init and post-load; PslyGlobal() unchanged
void InitSlyCopy3(LO* plo);
// orig3 0x152488's mesh groups 0x306 (the 3-D glasses): shown only in the anaglyph mode (never on the PC)
void ShowActorGlasses3(SW* psw);
// Debug (F7): the players' health to capacity and power to 100, every frame while on.
void CheatRefillPlayers(SW* psw);

// Sly's states (+0x18C8, Sly 3's JT +0x19E0 "jts") and substates (+0x18D0, Sly 3's +0x19E8 "jtbs") are Sly 3's numbers
// (AUDIT4 PLAYER-11: orig3 0x1ADDD0 SetJtJts, 0x1AC098 the transitions, 0x1AD1D0 the input). The native code was Sly 2's
// state machine (0x18C250 / 0x18A8C8 / 0x18B7E8) on Sly 2's numbers; its states with no Sly 3 state keep their code
// under numbers past Sly 3's range (100 + Sly 2's number): unreachable from Sly 3's data.
namespace jts
{
enum : int
{
    Stand = 0,          // idle SM (Sly 2's 0)
    Run = 1,            // the run gait +0x1C90 or a worn outfit's (Sly 2's 12)
    Walk = 2,           // R1 held: the walk gait +0x1470 (Sly 2's 1)
    Jump = 3,           // the jump sub-object +0x320 (Sly 2's 2)
    Hang = 4,           // a hook of kind 0 / 1 / 2: the swing (Sly 2's 3)
    Damage = 5,         // +0xE40 (Sly 2's 4)
    Target = 6,         // a guard in reach: jtbs 4 the stance, 5 the walk around it (Sly 2's 5)
    Dodge = 7,          // +0x1E90 (Sly 2's 6)
    Interact = 8,       // +0x500 (Sly 2's 7)
    Climb = 9,          // the pipe +0x1010 (Sly 2's 8)
    Sneak = 10,         // a guard near (rank 3): the root blend +0x1B20 (Sly 2's 10)
    Pickpocket = 11,    // +0x11B8 bit 2 (Sly 2's 11)
    Takedown = 13,      // the held guard / the juggle +0x11B4 (Sly 2's 16)
    ScriptSm = 14,      // a script's state machine +0x1138 (Sly 2's 18)
    ScriptAnim = 15,    // a script's animation (Sly 2's 19)
    ScriptDrive = 16,   // a script's animation driven by the pad (Sly 2's 20)
    HangFollow = 17,    // a hook of kind 3 (Sly 2's 22)
    Throw = 18,         // +0x1290 (Sly 2's 23)
    Boots = 19,         // Rocket Boots (Sly 2's 24, the Stealth Slide): jtbs 6 the charge, 7 the dash
    Carried = 20,       // on Murray (jump target kind 19: D_0038AE90 +0x18EC)
    Ride = 21,          // riding a guard (job 0xE7), its start
    RideOn = 22,        // riding a guard: the buttons to the job
    Attack = 23,        // a combo's attack (combo3.cpp: natively state 0 held by the combo)
    Map = 24,           // the Treasure Map +0x1F20: jtbs 8 held up, 9 its clip
    // Sly 2's states with no Sly 3 state (100 + Sly 2's number)
    N2Rush = 109,       // Sly 2's 9 (Knockout Dive's rush)
    N2AirSwing = 113,   // Sly 2's 13
    N2AirSlam = 114,    // Sly 2's 14
    N2Hypno = 117,      // Sly 2's 17 (the Contessa's hypnosis)
    N2ClimbSurf = 121,  // Sly 2's 21 (the climbing surfaces: jtbs 110 held, 111 moving)
};
// the substates (jtbs)
enum : int
{
    SubSwingBack = 0, SubSwingStill = 1, SubSwingFore = 2, SubHangSm = 3, SubTargetStance = 4, SubTargetWalk = 5,
    SubBootsCharge = 6, SubBootsDash = 7, SubMapUp = 8, SubMapClip = 9,
    // Sly 2's with no Sly 3 substate (100 + Sly 2's number): the charged swing's wind-up 6 / 7 and strike 8 / 9, the
    // climbing surface's 10 / 11
    SubN2WindUpL = 106, SubN2WindUpR = 107, SubN2Strike = 108, SubN2StrikeUp = 109, SubN2SurfHeld = 110, SubN2SurfMove = 111,
};
// Sly 2's state / substate numbers -> these (for the Sly 2 reference harnesses that read Sly 2 RAM)
constexpr int FromSly2(int n)
{
    switch (n)
    {
    case 0: return Stand; case 12: return Run; case 1: return Walk; case 2: return Jump; case 3: return Hang;
    case 4: return Damage; case 5: return Target; case 6: return Dodge; case 7: return Interact; case 8: return Climb;
    case 10: return Sneak; case 11: return Pickpocket; case 16: return Takedown; case 18: return ScriptSm;
    case 19: return ScriptAnim; case 20: return ScriptDrive; case 22: return HangFollow; case 23: return Throw;
    case 24: return Boots; case 9: return N2Rush; case 13: return N2AirSwing; case 14: return N2AirSlam;
    case 17: return N2Hypno; case 21: return N2ClimbSurf; default: return n < 0 ? n : 100 + n;
    }
}
constexpr int SubFromSly2(int m)
{
    switch (m)
    {
    case 12: return SubBootsCharge; case 13: return SubBootsDash;
    default: return m < 6 ? m : 100 + m;
    }
}
}  // namespace jts

// orig3: 0x1ADDD0 SetJtJts (the state n, substate m; Sly 2's 0x18C250)
void SetSlyState(SLY* psly, int n, int m);
// orig: 0x0018E540 (the attack mode; -1 none)
void SetSlyMode(SLY* psly, int n);
// Combat Dodge controller at +0x1FF0 (0x1371B8..0x1375D0).
void InitSlyDodge(SLY* psly);
void UpdateSlyDodgeTransitions(SLY* psly);
void UpdateSlyDodgeInput(SLY* psly);
void UpdateSlyDodgeMotion(SLY* psly);
int SelectSlyDodgeSide(SLY* psly);
void SetSlyDodgeState(SLY* psly, int state);
// Stealth Slide (state 24): the original state/input/motion branches.
void UpdateSlyTransitions(SLY* psly);
void UpdateSlySlideTransitions(SLY* psly);
void UpdateSlySlideInput(SLY* psly);
void UpdateSlySlideMotion(SLY* psly);
void SetSlyHandles(SLY* psly, uint32_t mask);
// orig: 0x0018DE80 / 0x00143220 (slot 96: the guard launch's predicted owner trajectory)
void GetSlyLaunchTarget(SLY* psly, float dt, LAUNCHTARGET* ptarget);
// orig: 0x0018BCE0 (vt slot 16)
void UpdateSly(LO* plo);
// (a harness's hook at the end of Sly's update, before the frame's physics)
extern void (*g_pfnSlyUpdated)(SLY* p);

// The actor base (cid 0x34, 0x13FF70..0x144E20) as far as ported, for the other players (Bentley 0x37, Murray 0x36:
// gang.h):
struct ACTOR;
// orig: 0x00140AF8 (the target +0x10E8 (a guard) within reach: Sly's state 5)
bool FActorTargetInReach(ACTOR* pa, bool fWide);
// orig: 0x0013FF70 (the actor's init)
void InitActorFields(ACTOR* pa);
// orig: 0x001400B8 (the post-load's parts; then the class's own, then PostLoadActorMass: its end)
void BindActorPartTable(ACTOR* pa);
void PostLoadActorParts(ACTOR* pa);
void PostLoadActorMass(ACTOR* pa);
// orig: 0x001403D0 (the actor's update)
void UpdateActor(ACTOR* pa);
// orig: 0x00142868 / 0x00142880 / 0x001430C8 / 0x00143C78
void ActorSetLookFlag(ACTOR* pa, int f);
void ActorNextLookTime(ACTOR* pa);
uint32_t GrfActorLookMask(ACTOR* pa);
// orig: 0x001426E8 (the head's look controller on the record tag x) / 0x001427C8 (the look target)
void AttachActorLook(ACTOR* pa, int x);
void SetActorLookTarget(ACTOR* pa, int k, LO* plo);
uint32_t GrfActorStance(ACTOR* pa);
