#pragma once

// Carrying: the booty (cid 0x67, module 0x13BDF0..0x13CEB4; e.g. job 1810's forged painting, object 6435) and its
// stands (cid 0x25, a use: 0x13CEB8..0x13D3F8, the save's "aoidAssembly"), the carrier's side (ACTOR +0x11A4 the
// holder part, +0x11A8 the booty carried; 0x1432E0..0x143BD4, the save's "<player>.oidBooty"). booty.cpp.
//
// The booty object is one mesh with every piece of booty as a group; a record (+0x154 / +0x158, 0x2C bytes, added by
// the load's options) picks the groups shown and the collision tag. States (+0x184): 0 out of the world, 1 on its
// stand (+0x15C), 2 flying from the stand to the carrier (+0x160), 3 flying from the carrier onto a stand, 4 carried
// (at the carrier's holder part), 5 thrown off (dropped), 6 given (to the carrier without the flight).

#include "basic.h"

#include <cstdint>
#include <glm/mat4x4.hpp>

struct ACTOR;
struct ALO;
struct LO;
struct OPTVAL;
struct SW;

// orig: 0x0013C560 (the state; leave the old, enter the new)
void SetBootyState(LO* plo, int n);
// orig: 0x00143380 (the carried booty dropped: the save (0x143678 kind 2), the booty thrown off (state 5))
void DropActorBooty(ACTOR* pa);
// orig: 0x001433C8 (the carried booty put back on its stand: the save (0x143678 kind 1), state 0)
void ReturnActorBooty(ACTOR* pa);
// orig: 0x001432E0 (the booty of record id n given to the actor, unless it carries it already; -1: none)
void GiveActorBooty(ACTOR* pa, int n);
// ACTOR +0x11A8 (null: none)
LO* PlootCarriedBy(const ACTOR* pa);
// the carried booty's record +0x8: any damage drops it (0x1A19A0)
bool FActorBootyFragile(const ACTOR* pa);
// orig: 0x0013C000 / 0x001A5EE8: carried scale as a world-space draw transform; the simulation records stay intact.
bool FBootyDrawDelta(const ALO* palo, glm::mat4* pdmat);
// orig: 0x0013D020 (cid 0x25's vt slot 51: the stand used by the actor: take its booty / put the carried one on it,
// then the use's base 0x184130); false: not a stand
bool FUseBootyStand(LO* pstand, ACTOR* pa);
// the load's options on the current record (EOPIDs 1155 / 1161..1163: the getter 0x13C4D8)
void SetBootyRecordField(LO* plo, uint32_t ib, const OPTVAL& v);
// orig: 0x0013BF00 (cid 0x67's vt slot 15) / 0x0013CEF8 (cid 0x25's)
void PostLoadBooty(LO* plo);
void PostLoadBootyStand(SW* psw, LO* plo);
// orig: 0x0013CFD8 (cid 0x25's vt slot 23: its booty attached at the deferred calls, 0x13CF38)
void OnBootyStandRegistered(LO* plo);
// orig: 0x00143BD8 (posted by the actors' post-load 0x1400B8: the save's booty carried again)
void PostLoadActorBooty(ACTOR* pa);
void QueuePostLoadActorBooty(LO* plo);  // (the actor resolved at the deferred calls)
void ResetBooties();  // a new world
