#pragma once

// Two-bone joints (Sly 2 module 0x186610-0x187030, the joints loaded by 0x186670 into ALO::apair): the lower bone
// bends so that the chain reaches the target (law of cosines), the upper bone turns the end onto it. A joint drives
// its upper record in mode 13 (0x22DE88 calls the solve) and its lower one in mode 14, through kind-7 controllers.
// Part of the animation code.

#include "xf.h"

// orig: 0x00186930: the joint's rest pose (after the records' world matrices are set)
void InitAloPairRest(ALOPAIR* ppair);
// orig: 0x00186820: the bone lengths measured again
void RemeasureAloPairs(ALO* palo);
// orig: 0x00186A50: the solve (from mode 13 of the upper record)
void SolveAloPair(ALO* palo, ALOPAIR* ppair);
// The joints' kind-7 controllers (orig 0x12DD28 from 0x186670) and rest poses, for InitAloXfs.
void InitAloPairs(ALO* palo);
