#pragma once
#include <cstdint>

// The players' vt slot 90 (orig Sly 0x18C060, Bentley 0x135D80, Murray 0x1B8008): the step's second mesh (stepphys.h
// SetStepMesh, 0x203D00) by the state, and the sub-objects' meshes (the jump 0x19C870, the damage 0x1A23F8, the
// interaction 0x1944C8). Called where the original calls slot 90: the players' state changes, the jump's animation
// state, the damage's reaction, the interaction's state.

struct ACTOR;
struct ALO;

int IsoeActorPart(ALO* palo, uint32_t ib);  // orig: 0x1400B8 / 0x188608 part binding

void UpdateActorStepMesh(ACTOR* pa);
int IsoeJumpMesh(ACTOR* pa, bool* pfSnap);      // orig 0x19C870
int IsoeDamageMesh(ACTOR* pa, bool* pfSnap);    // orig 0x1A23F8
int IsoeInteractMesh(ACTOR* pa, bool* pfSnap);  // orig 0x1944C8
