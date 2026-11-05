#pragma once

// cid 0x58: a hiding spot (the barrels a player jumps into: the jump's list A 0x3E13E4, 0x197F98 -> the jump's state 6
// -> its arrival 0x19C688 -> the interaction kind 1 on it). From the original's code (the decomp's matching C):
// 0x183900 (InitLo), 0x183970 (vt slot 26), 0x183998 / 0x1839D0 (slots 23 / 22: list A, jt.cpp), 0x183A08 (slot 41),
// 0x183A50 (slot 61), 0x183B00 (slot 70), 0x183B80 (its state), 0x183E08 (slot 75).
//
// Its state (+0x17C): 0 free, 1 a player jumping into it (the jump's state 6), 2 a player inside it (the barrel then
// follows the player's +0x10BC record when +0x18C), 3 knocked over (a guard's hit while occupied: the player thrown
// out). +0x158 how high above it a jump may still take it (300), +0x18C whether it follows its holder (1).

struct ACTOR;
struct LO;

void InitHide(LO* plo);              // orig 0x00183900 (the fields and the contact bytes)
int NHideState(LO* plo);             // +0x17C
float SHideHeight(LO* plo);          // +0x158
bool FHideFollows(LO* plo);          // +0x18C
ACTOR* PactorHideHolder(LO* plo);    // +0x190
void SetHideHolder(LO* plo, ACTOR* pa);
void SetHideState(LO* plo, int n);   // orig 0x00183B80
void ResetHides();                   // a new world
struct SW;
void DebugHitHideHolders(SW* psw);  // (debug: SLY2N_HIDEHITHOLDER)
