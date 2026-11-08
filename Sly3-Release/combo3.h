#pragma once

// Sly 3's combo system (the cane's attacks), ported from the Sly 3 asm (combo3.cpp has the addresses and layouts).
//
// Natively Sly's Sly 3 state 23 (the attack) is the copied player's state 0 with the combo holding him
// (FCombo3Attacking): no transitions, the feet's velocity the clip's root motion. The copied UpdateSlyButtons leaves
// square / triangle to the combos (Sly 2's swing modes are not used).
//
// SLY3_COMBODBG=1 prints the definitions and the runner's states and events; SLY3_ABILITIES=41,44,... the abilities
// owned (the upgrades the combo definitions ask for).

struct ASEGMSG9;
struct LO;
struct PADCTL;
struct SLY;
struct SW;

namespace sly3
{
LO* PloFindOwned3(LO* p, int oid);   // a descendant of p by oid (the parts' search)
void DumpCombos3(SW* psw);           // SLY3_COMBODBG=1: the combo definitions (class 0x94) and their options
PADCTL* PpadctlOfSly3(SLY* p);

void UpdateCombo3(SLY* p);                                 // UpdateActor's end (orig3 0x152A48 -> vt +0x314)
void HandleCombo3Event(SLY* p, const ASEGMSG9* pmsg);      // the actor's ASEG events (Sly 2 message 9)
void HandleCombo3Contact(SLY* p);                          // a contact accepted (Sly 2 message 11)
void StartAbilityCombo3(SLY* p, int ability);              // the abilities 31 / 25
bool FCombo3Attacking(SLY* p);                             // Sly 3's state 23
int NCombo3State(SLY* p);                                  // the runner's state (-1 off)
}  // namespace sly3
