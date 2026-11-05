#pragma once

// The actors' damage controller (orig ACTOR +0xDF0, module 0x1A0B18..0x1A245C; the data: actor.h DAMAGE / DMGREQ):
// a hit's request (vt slot 75 0x140F50 -> 0x1A19A0) takes the counter (Sly: the save's health), flashes the actor,
// tells the game (event 15) and the actor's state machine (vt slot 88 (3): Sly's state 4), and starts a reaction (its
// state +0xDF4: 7 knocked back by the hit's velocity, then 0 the getting up; 8..12 when the counter is out), each with
// its clip (+0xA0 + 4 n). The status (vt slot 77 0x140F70 -> 0x1A23C0) tells the guards whether the actor is busy.

struct ACTOR;
struct CTMSG;
struct XP;

#include "actor.h"
#include <glm/glm.hpp>

// orig: 0x001A0B18 (the init: no reaction, no flash, the flash 2 s, the capacity 40) and Sly's counter
void InitDamage(ACTOR* pa);
// orig: 0x001A19A0 (a hit: false when the actor is immune or the request is refused)
bool FAcceptDamage(ACTOR* pa, CTMSG* pctm);
// orig: 0x001A0B40 (the reaction n: -1 none; its clip, the velocity / heading by kind)
void SetDamageState(ACTOR* pa, int n);
// orig: 0x001A1358 (each frame: the flash's fade; reaction 7 turns against its velocity and ends on the floor)
void UpdateDamage(ACTOR* pa);
// orig: 0x001A15D0 (the reaction's end: the deferred action, else the states 8..12's slot 93, else the jump's kind 11)
void EndDamageReaction(ACTOR* pa);
// orig: 0x001A12F0 (the state machine's transitions in Sly's state 4: the reaction's clip done ends it)
void UpdateDamageTransitions(ACTOR* pa);
// orig: 0x001A23C0 (0 free, 1 reacting, 2 reacting with the counter out; else 1 while flashing)
int NDamageStatus(const ACTOR* pa);
// orig: 0x001A2100 (a new contact in Sly's state 4: the friction / elasticity by reaction)
void AdjustDamageXp(ACTOR* pa, XP* pxp);
// orig: 0x001A1148 (the motion in Sly's state 4: the reactions 1 / 2 / 5 / 11's; the others none)
void UpdateDamageMotion(ACTOR* pa);
struct SOE;
// orig: 0x0019FEB0 (+0x9E0: a floor stood on, its entry and point in the entry's object's frame)
void RecordSafeGround(ACTOR* pa, SOE* psoe, const glm::vec3& posLocal);
// orig: 0x0019FD30 (a safe interaction description and its curve/angular parameter)
void RecordSafeTarget(ACTOR* pa, LO* plo, JTL jtl, float g);
// orig: 0x001942F0 / 0x0019EA60 (the interaction / pipe controller's safe place)
void UpdateSafeInteract(ACTOR* pa);
void UpdateSafePipe(ACTOR* pa);
// orig: 0x001A0048 / 0x001A00D0 (compact each hero's history before an owner or floor is removed)
void RemoveSafeGroundReferences(LO* plo, SOE* psoe);
// orig: 0x0019F3F8 (the place in world space, with its transform predicted dt seconds ahead)
glm::vec3 PosSafeGround(ACTOR* pa, const SAFEGROUND::ENTRY& e, float dt = 0.0f);
// orig: 0x0019FC30 (Sly's: the floor of the states that stand)
void UpdateSafeGround(ACTOR* pa, int state);
// orig: 0x0019F590 (back to the best safe ground: at once, or by the jump's kind 0xA)
void RecoverActor(ACTOR* pa);
// the same with the entry q given (SCPT 0x2A: an object or a point)
void RecoverActorTo(ACTOR* pa, const SAFEGROUND::ENTRY& e);
// orig: 0x00140ED8 (the actor's immunity to a kind: +0xF20 1, the jump's kind 0x15, or 0x1A2230)
bool FActorImmune(ACTOR* pa, int kind);
// orig: 0x001A2460 (the actor's update: the water's contact while its point is in the water +0x108C)
void UpdateActorWater(ACTOR* pa);
// the counter (+0xDF0 +0x10) and its capacity (+0xC), for the players' start (0x1440D0)
int* DmgCounterOf(ACTOR* pa);
int DmgCapacityOf(ACTOR* pa);
void SetDmgCounter(ACTOR* pa, int* pn);
void SetDmgCapacity(ACTOR* pa, int c);
