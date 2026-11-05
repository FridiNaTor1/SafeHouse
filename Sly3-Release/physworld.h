#pragma once

// The physics in the world's loop (physworld.cpp).

struct ALO;
struct LO;
struct SW;

// orig 0x1F8DD8: the frame's physics on the world's update table (contacts, slot 17 of the moving entries, the two
// solve passes); the 0x1F8DD8 pass of UpdateSw calls it.
void UpdateSwPhysFrame(SW* psw);
// The first frame's refresh of the entries' world geometry and the stepping objects' headings (from their records)
// done now, so that a harness importing a reference state afterwards keeps it (animref.cpp --ai-free).
void RefreshSwPhysWorldNow(SW* psw);
// The world's hooks of the physics (the active-list test of 0x1336F0); called once at the world load.
void InitPhysWorldHooks();
// The physics lists emptied for a new world (the transition, before its load).
void ResetPhysWorld();
// The stepping objects' physics load / post-load parts (0x2039C0 / 0x203A38: step.h), after the world's records'
// post-load state (anim's InitWorldAnimation resets the records' modes).
void PostLoadSwPhys(SW* psw);
// Set by the actors' code once a player actor's (0x34..) state code runs natively: without it PostLoadSwPhys keeps
// the actor's constraints fixed (lin / ang 3) so that it does not slide away at the world's start.
extern bool (*g_pfnFActorDrivesPhys)(ALO* palo);
// Called after the world's first physics frame refreshed the entries and rebuilt the pairs (the harnesses import the
// reference's contacts there).
extern void (*g_pfnPhysWorldRefreshed)(SW* psw);

// The ALO's physics parts of the activation slots (no-ops for other classes):
void OnLoAddPhys(LO* plo);         // vt slot 11 (ALO 0x1F2C30): records' worlds, entries into the attachment, flags 0x14
void OnLoRemovePhys(LO* plo);      // vt slot 12 (ALO 0x1F2D48): entries out of the attachment, fade
void OnLoRegisterPhys(LO* plo);    // vt slot 23 (ALO 0x1F3EA8): the attachment onto the active list, bounds
void OnLoUnregisterPhys(LO* plo);
void OnLoMovePhys(LO* plo, ALO* paloNew, ALO* paloOld);  // vt slot 13 (ALO 0x1F2E78): the entries rebuilt
  // vt slot 22 (ALO 0x1F3E00): bounds, off the active list
