#pragma once

// The carryable objects (cid 0x64: Bentley's mines 0x65, the guard trap 0x66; module 0x224258..0x225800): a state
// (+0x150: 0 held, 1 dropped, 2 free (physics), 3 triggered, 4 fading, -1 gone), its time, the contact state +0x158
// and the ability id +0x15C (the carrier's +0x16B4 entry). carry.cpp.

struct ALO;
struct LO;
struct SMA;

struct CARRY
{
    int state = -1;        // +0x150
    float t = 0.0f;        // +0x154
    int n158 = -1;         // +0x158 (2: touched something (+0x1A0), 4: to fade)
    int kind = 0;          // +0x15C the ability's id (5 a mine ...)
    LO* plo160 = nullptr;  // +0x160 the nearest guard (kind 0x29)
    LO* plo16C = nullptr;  // +0x16C the carrier (0x136298)
    LO* plo1A0 = nullptr;  // +0x1A0 what it touched
    float sRange = 3000.0f; // +0x170 the carrier's range (0x223F70)
};

CARRY* PcarryFromLo(LO* plo);  // null outside cid 0x64
// Read existing state without creating a carry record (ordinary-play observation).
const CARRY* PcarryFind(const LO* plo);
const SMA* PsmaCarryFind(const LO* plo);
// orig: 0x00224028 / 0x00224068: activation lifecycle after the base ALO callbacks.
void OnCarryAdd(LO* plo);
void OnCarryRemove(LO* plo);
// orig: 0x00224EC8 (vt slot 76: the state)
void SetCarryState(LO* plo, int n);
// orig: 0x00224258: landing is shared by held contacts and dropped-object updates.
void LandCarry(LO* plo, LO* touched);
void ResetCarries();
