#pragma once

// The players' interaction sub-object (orig ACTOR +0x4C0, module 0x191090..0x194A40: Sly 1's JT ledge / hide / pipe /
// rail code merged into one state machine): what a player does with an object it holds on to or uses. Its kind is
// ACTOR::n4C8 (+0x8), its state ACTOR::nSub4CC (+0xC); the actor is in its "interaction" state meanwhile (Sly 7,
// Bentley 3, Murray 13: vt slot 88 (2)).
//
// The kinds (0x1928F8's n; who starts them):
//    0 a ledge (cid 0x0C, the jump's arrival 11) / a wall (0x22, 12) / a swing (0x26 by its +0x10C, 13), circle
//      near one (0x1938C0): hanging / flattened, moved sideways by the stick (states 0 still, 1 moving, 2 / 3 the
//      ends, 5 turning toward it)
//    1 a hiding spot (cid 0x58, the jump's 6)    2 Sly's message 0x18DA48     3 a rail (0x0D, the jump's 10)
//    4 the jump's 9 (+0xE0)                       5 / 9 the reach toward an object (the gang's pipes, the vault
//      0x142140 / 0x1421F8): the reach +0x440, a pipe climbed (state 8 / +0xF50) when the hand is near it
//    6 Sly's cane toward a pipe from the ground (circle: mode 0 on the target)
//    7 a use (cid 0x23: the safe houses' doors: its vt slot 51 with the actor)
//    8 a hook (cid 0x1B): its point +0x15C, the walk on it (the root blend +0x330), the camera policy CM +0x4C0
//    10 (a timed pose)   11 another's grab (Murray's / Bentley's events 15 / 21, 0x141D38)   12 the pickpocket
//    (0x1424E0)          13 like 0 (+0x510 first)
// From the original's code: 0x191090 / 0x1910F8 / 0x1928F8 / 0x192A28 / 0x1938C0 / 0x1940B0 / 0x194118 / 0x1944C8
// (the decomp's matching C) and 0x192BD8 / 0x1911A8 / 0x191A10 (the asm; the reference's ports of these three are
// register-level transcriptions: native_old_set_action_state.cpp / native_old_nearby_action.cpp / _update.cpp).

#include "actanim.h"
#include "jt.h"
#include "reach.h"

struct ACTOR;
struct ALO;
struct ASEG;
struct ASEGBL;
struct LO;
struct SM;

// (offsets relative to +0x4C0)
struct INTERACT
{
    float tBegin = 0.0f;           // +0x4 the action's start
    float tState = 0.0f;           // +0x10 the state's start
    LO* ploTarget = nullptr;       // +0x14
    JTL jtl = JTL_Max;             // +0x18 the target's description (orig a pointer to it: JTENT +0x4); natively its list
    float g1C = 0.0f;              // +0x1C a curve target's parameter (kind 3)
    float s20 = 0.0f;              // +0x20 its distance / the description's parameter (kind 0)
    int f24 = 0;                   // +0x24 (kind 3: against the curve's direction)
    int n28 = -1;                  // +0x28 (kind 3: the lean, -1 / 0 / 1)
    int hfdk = -1;                 // +0x2C (0x1940B0: Sly 1's HFDK, -1 none, 0 match, 1 left, 2 right, 3 stand)
    float tHfdk = 0.0f;            // +0x30
    int n34 = 0;                   // +0x34
    glm::vec3 posFocus{0.0f};      // +0x40 the camera's focus point (jtfock 2; 0x18A4E8)
    float g50 = 0.0f;              // +0x50
    LO* ploZone = nullptr;         // +0x68 the camera zone made for it (cid 0xF oid 0x148, 0x13E3C8 in 0x1910F8)
    ROOTBLEND rb70;                // +0x70 (pbl +0x170) kind 0's sidestep (parts +0x74 + 0x20 i)
    ASEG* paseg174 = nullptr;      // +0x174 kind 0's still
    ASEG* paseg178 = nullptr;      // +0x178 / +0x17C the ends (states 2 / 3)
    ASEG* paseg17C = nullptr;
    GAIT gait180;                  // +0x180 kind 1 (state 4)
    SM* psm230 = nullptr;          // +0x230 kind 2's
    ASEG* paseg234 = nullptr;      // +0x234 kind 4's
    ASEG* paseg238 = nullptr;      // +0x238 kind 3's
    GAIT gait240;                  // +0x240 kind 3 (state 4)
    ASEGBL* pbl314 = nullptr;      // +0x314 (the plain blend of +0x2F4 / +0x300 / +0x30C)
    float g318 = 0.0f, g31C = 0.0f;  // +0x318 / +0x31C its lean (left > 0 > right) and the lean's rate (0x189618)
    ASEG* paseg320 = nullptr;      // +0x320 kind 6 / 7's
    ASEG* paseg324 = nullptr;      // +0x324 kind 8's start (state 7)
    ASEG* paseg328 = nullptr;      // +0x328 kind 8's end (state 8)
    SM* psm32C = nullptr;          // +0x32C kind 8's still (state 9)
    ROOTBLEND rb330;               // +0x330 (pbl +0x430) kind 8's walk (state 10; parts +0x334 + 0x20 i)
    REACH reach;                   // +0x440 kinds 5 / 9 (its target +0x4C4)
    ASEG* paseg4F0 = nullptr;      // +0x4F0 / +0x4F4 kind 10 (by the stick)
    ASEG* paseg4F4 = nullptr;
    ASEG* paseg4F8 = nullptr;      // +0x4F8 kind 11
    int f500 = 0;                  // +0x500 (kinds 11 / 12: hold the action)
    ASEG* paseg50C = nullptr;      // +0x50C kind 12's
    ASEG* paseg508 = nullptr;      // +0x508 kind 12's default clip (0x1424E0)
    ASEG* paseg510 = nullptr;      // +0x510 kind 13's first
    ASEG* paseg514 = nullptr;      // +0x514 kind 2's
};

// orig: 0x00191090 (the init) / 0x001910F8 (the post-load: the parts, the blends, the gaits, the reach)
void InitInteract(ACTOR* pa);
// orig: 0x00194558 (the interaction's slot 99)
bool FInteractCanDo(const ACTOR* pa, int k);
void PostLoadInteract(ACTOR* pa);
// orig: 0x00140C88 (the current player's camera zone +0x1024: the old one off, the new one on, 0x221268)
void SetActorZone(ACTOR* pa, LO* plo);
// an object field holding an object (its oid; interact.cpp's PloOidField)
LO* PloInteractOidField(LO* plo, uint32_t ib);
// the camera zone of an interaction object's description part (+0x0 of a ledge's / pipe's +0x190, a wall's /
// swing's +0xE0)
LO* PloJtDescZone(LO* plo, JTL jtl);
// orig: 0x001928F8 (start the action of kind n with the object and its description list)
void StartInteract(ACTOR* pa, int n, LO* ploTarget, JTL jtl);
// orig: 0x001424E0 (show a prize with the supplied pose, or the default pose)
void StartPrizeInteract(ACTOR* pa, ASEG* paseg, LO* ploPrize, int n);
void FaceActorToward(ACTOR* pa, LO* plo);  // orig 0x2046F0 (the heading toward an object)
void TurnActorNow(ACTOR* pa);             // orig 0x204CA8 (refresh angular velocity toward that heading)
// orig: 0x00192BD8 (the state n: the old state left, the new one's animation / state machine; -1 ends the action)
void SetInteractState(ACTOR* pa, int n);
// orig: 0x001911A8 (the transitions, repeated while the state changes)
void UpdateInteractTransitions(ACTOR* pa);
// orig: 0x00191A10 (the input: the state's motion, +0x190)
void UpdateInteract(ACTOR* pa);
// orig: 0x00192A28 (the guards that noticed the actor and lost sight of it forget it)
void ForgetGuardsInteract(ACTOR* pa);
// orig: 0x001938C0 (circle: the first tracked target within 250 starts its action; false: none)
bool FInteractCircle(ACTOR* pa);
// orig: 0x00142140 / 0x00142340 (the splice / SCPT's: the action of kind n (5 / 9) on the object; null ends it when it
// is of that kind (vt slot 88 (0)))
void SetActorInteractObject(ACTOR* pa, int n, LO* plo);
// orig: 0x001840B8 / 0x001840F8 (the use's (cid 0x23 / 0x24) lists: its state machines' states (+0x118 count, +0x11C
// {root, SM, state, -1}) and its tasks' states (+0x15C, +0x160 {task, state}); kept in the object's fields)
void AddUseRsmg(LO* puse, OID oidRoot, OID oidSm, OID oidState);
void AddUseTask(LO* puse, OID oidTask, int state);
// orig: 0x001423A0 (the SCPT's done test of 0x1D / 0x21: a kind-5 pose is done when its reach's end object is within 5
// of where its record chain's controllers take it (0x2309E8 at dt 0), and a grab's (reach state 1) timed move is over)
bool FActorPoseDone(ACTOR* pa);
// orig: 0x00193DE8 (vt slot 59's part: the internal contacts of kinds 3 / 4)
void AddInteractContacts(ACTOR* pa);
// orig: 0x00194170 (the motion: kind 3's slide along the rail)
void UpdateInteractMotion(ACTOR* pa);
