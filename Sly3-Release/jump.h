#pragma once

// The actors' jump (actor.h JUMP, the sub-object at +0x2C0; module 0x195950..0x19CBF8): its kind (0x195A00), its
// animation state (0x195FB8), the per-frame transitions (0x197500), the input (0x197F98) and the air motion
// (0x196DF8). The owner's state follows through its vt slot 88 (Sly: 0x18D568).

struct ACTOR;
struct JUMP;
struct LO;

#include <glm/glm.hpp>

// orig: 0x00195950
void InitJump(JUMP* pj, ACTOR* pa);
// orig: 0x001959C0 (the post-load: the paraglider's parts (actor +0x438.. bound by 0x2BC9F0) and its blend
// 0x137A30 of the five BLs at +0x17C)
void PostLoadJump(JUMP* pj);
// orig: 0x00195A00
void SetJumpKind(JUMP* pj, int n, int a2, LO* plo);
// the owner's vt slot 88 (Sly's 0x18D568: the state of a jump kind n; 3 the damage's)
struct ACTOR;
void NotifyActorJumpKind(ACTOR* pa, int n);
// the owner's vt slot 89 (Sly 0x18D620, Bentley 0x135D08, Murray 0x1B7F90: the jump kind of its state, -1 none)
int ActorJumpKind(ACTOR* pa);
// orig: 0x00195FB8
void SetJumpAnimState(JUMP* pj, int n, LO* plo);
// orig: 0x00143D20 (the actor's base below the floor within dt: the floor probe 0x173610 under it)
bool FActorFloorSoon(ACTOR* pa, float dt);
// orig: 0x0019C8D0 (the jump's slot 99)
bool FJumpCanDo(const JUMP* pj, int k);
// orig: 0x00197500
void UpdateJumpTransitions(JUMP* pj);
// orig: 0x00197F98
void UpdateJumpInput(JUMP* pj);
// orig: 0x00196DF8
void UpdateJumpMotion(JUMP* pj);
// orig: 0x0019A750 (the velocity with its vertical part raised for a jump of kind k)
glm::vec3 VelJumpLaunch(JUMP* pj, int k);

// The jump's targets (jumptgt.cpp, module 0x19A400..0x19C870):
// orig: 0x0019B110 (circle in the air: the best object of the target lists in reach (pipes, rails, points, ...);
// the jump's record is driven to it (mode 0x11: +0xA8 the time, +0xC0 the end, +0xD0 its velocity) in the jump's
// state of that kind (pipe 15). ploPrev: only that one is re-evaluated.)
void ChooseJumpTarget(JUMP* pj, LO* ploPrev);
// orig: 0x00197F98's target search (the jump's states 1 / 3 / 4 / 5 / 17 and >= 23): the free hiding spots of list A
// (0x3E13E4, cid 0x58: within 600 horizontally, their top +0x158 not below the actor) and the objects of list B
// (0x3E1420, cid 0x28) the jump fits best (0x19A4A0, under 1500) passing near them (0x11EE08): the jump's record
// aimed at it, its state 6 (A) or 8 (B)
void FindJumpHideTarget(JUMP* pj);
// orig: 0x00197F98's path (states 6 / 8: the record's end where the object will be) and its speed test (states 6,
// 8..16, 18: given up (state 4) above 10000)
void UpdateJumpTargetPath(JUMP* pj);
// orig: 0x0019C688 (the flight to the target is over: the target's action / the owner's state)
void ArriveJumpTarget(JUMP* pj);

// The ledge grab (jumpledge.cpp, orig 0x199D48 and the state 17 parts of 0x195FB8 / 0x197500): falling onto a
// geometry edge with room behind it hangs from it (the jump's state 17)
bool FJumpGrabLedge(JUMP* pj);        // the best ledge among the contacts: state 17 on it
bool FCheckJumpLedgeHold(JUMP* pj);   // still a ledge (0x1999D0 after 0.03 s; its object still there)
void EnterJumpLedge(JUMP* pj);
void LeaveJumpLedge(JUMP* pj);
void AddJumpLedgeContacts(ACTOR* pa);  // orig 0x1991A0 (slot 59's part)
void PruneJumpLedgeXps(ACTOR* pa);     // orig 0x199298 (slot 58's part)
// orig: 0x0019A098 (a falling actor held by its contacts for 1 s; jump.cpp)
bool FActorStuckOnContacts(ACTOR* pa, float tPrev, float* ptStamp);
