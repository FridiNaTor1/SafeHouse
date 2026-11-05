#pragma once

// The players' gadgets (the abilities on L1 / L2 / R2: module 0x141A50..0x141EF8 and the gadget power 0x1441A0 ..
// 0x1441E8; per character vt slots 99 can-do, 100 start, 101 held, 102 end): the three slots +0x1180 {id, state,
// t} loaded from the progress "<sly|bentley|murray> apukCur <i>", the gadget power the progress "mpCur" (+0x12FC;
// its maximum +0x1300 is always 100: 0x13FF70 is the only store), the item table 0x2BCD00 (+0 the kind of use: 2 a
// press, 1 held, -1 never on a button; +0xC the power it costs).

#include <cstdint>

struct ACTOR;

void UpdateActorAbilities(ACTOR* pa);              // orig 0x00141A50 (from the actor's update, after 0x203FC0)
void SetActorAbilitySlot(ACTOR* pa, int i, int id);  // orig 0x00141FC8 (the gadget menu's assignment)
uint32_t GrfAbilityButton(ACTOR* pa, int id);       // orig 0x00141EA8: the button mask of the slot holding id (0 none)
int NActorPower(ACTOR* pa);                         // orig 0x001441A0
void SetActorPower(ACTOR* pa, int n);               // orig 0x001441C0 (at most +0x1300)
void SpendActorPower(ACTOR* pa, int id);            // orig 0x00141CD0 (the item's cost; +0x1304 the meter's flash)
void DrainActorPower(ACTOR* pa, float gPerSecond);  // orig 0x001441E8 (+0x1308 the fraction kept)
bool FPowerupOwned(int id);                         // orig 0x001DAEA8 (the progress "powerUps mppukfAcquired <id>")
void StartActorAbility(ACTOR* pa, int id);          // the players' vt slot 100 (Sly 0x18DA48, Bentley 0x136A90, Murray 0x1B9A08)
bool ReleaseSmokeBomb(ACTOR* pa);                   // orig 0x00140F90 event 0x27C; caller checks ASEGA owner
bool ReleaseTimeLock(ACTOR* pa);                    // orig 0x00140F90 event 0x27C case 0xE (Temporal Lock)
bool ReleaseRoar(ACTOR* pa);                        // orig 0x00140F90 event 0x27C case 0x15 (Guttural Roar)
void SetGuardsFrozen(bool f);                       // orig 0x0019F170
void UpdateGuardsFrozen();                          // orig 0x0019F120
bool ReleaseCaneAbility(ACTOR* pa);                 // orig 0x00140F90 event 0x27C; caller checks ASEGA owner
struct SW;
void RemoveMegaJump(SW* psw);                       // orig 0x0014C258 (the current player's Mega Jump slot emptied, not owned)
