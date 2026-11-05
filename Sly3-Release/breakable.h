#pragma once

// The touch / hit triggered objects (cid 0x62: breakable.cpp).
struct ALO;
struct LO;

void InitBreakableBytes(ALO* palo);  // orig 0x1BAB50's contact bytes (before the load)
void PostLoadBreakable(LO* plo);     // orig 0x1BABA0's end: the first event current
void ResetBreakable(LO* plo);        // orig 0x1BBEA8 (vt slot 26, a proxy's attach): back to the first event
void ResetBreakables();              // a new world
bool FBreakableIntact(LO* plo);    // orig 0x1BBE18 (the current event valid: not broken yet)
