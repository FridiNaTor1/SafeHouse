#pragma once

// Sly 3's combo system (the cane's attacks), ported from the Sly 3 asm (combo3.cpp has the addresses and layouts).
//
// Natively Sly's Sly 3 state 23 (the attack) is the copied player's state 0 with the combo holding him
// (FCombo3Attacking): no transitions, the feet's velocity the clip's root motion. The copied UpdateSlyButtons leaves
// square / triangle to the combos (Sly 2's swing modes are not used).
//
// SLY3_COMBODBG=1 prints the definitions and the runner's states and events; SLY3_ABILITIES=41,44,... the abilities
// owned (the upgrades the combo definitions ask for).

struct ACTOR;
struct ASEGMSG9;
struct CTMSG;
struct LO;
struct PADCTL;
struct SLY;
struct SW;

namespace sly3
{
LO* PloFindOwned3(LO* p, int oid);   // a descendant of p by oid (the parts' search)
void DumpCombos3(SW* psw);           // SLY3_COMBODBG=1: the combo definitions (class 0x94) and their options
PADCTL* PpadctlOfSly3(SLY* p);
PADCTL* PpadctlOfActor3(ACTOR* p);

// (Sly and the Guru: the Guru's generic state is his jump kind (vt slots 99 / 100), his can-do slot 113)
void UpdateCombo3(ACTOR* p);                               // UpdateActor's end (orig3 0x152A48 -> vt +0x314)
void HandleCombo3Event(ACTOR* p, const ASEGMSG9* pmsg);    // the actor's ASEG events (Sly 2 message 9)
void HandleCombo3Contact(ACTOR* p, const CTMSG* pctm);     // a contact accepted (Sly 2 message 11)
void SetCombo3Listed(ACTOR* p, int oidDef, bool fIn);      // the ASEG events 25 / 26 (0x1399F0 / 0x1399A0)
bool FCombo3DefMoves(ACTOR* p);                            // orig3 0x1398F0 on the runner's definition
void StartAbilityCombo3(SLY* p, int ability);              // the abilities 31 / 25
bool FAbilityCombo3Ready(SLY* p, int ability);             // 0x1AF308's 31 / 25 (s3-decomp, gadget.cpp)
bool FCombo3Attacking(ACTOR* p);                             // Sly 3's state 23
bool FLeaveCombo3Attack(SLY* p);                             // orig3 0x1AC098 case 23: the attack left for state 0 (moving)
int NCombo3State(ACTOR* p);                                  // the runner's state (-1 off)
bool StartScriptCombo3(ACTOR* p, LO* ploDef, bool fNoTarget); // SCPT 0x28 (orig3 0x13A678 from 0x226D00 case 40)
}  // namespace sly3
