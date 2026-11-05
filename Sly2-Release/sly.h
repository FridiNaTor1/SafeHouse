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
// Debug (F7): the players' health to capacity and power to 100, every frame while on.
void CheatRefillPlayers(SW* psw);

// orig: 0x0018C250 (the state n, substate m)
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
