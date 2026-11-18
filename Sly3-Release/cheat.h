#pragma once

// Sly 3's cheat codes (orig3 0x163F58 match / 0x164028 run / effects 0x164170, 0x1641A0, 0x1641B0, 0x164228; the list
// 0x36BB88 built by 0x164518 from 34 entries of 0x38 bytes at 0x3440E0, their six buttons the words before each).
// Sly 3 takes them on the episode menu only (the world loaded is dvd_menu, 0x25E150() == 0; the other test,
// 0x46F74C, is never set): each newly pressed button (pressed now, not the frame before) is matched against every
// code's next button (a miss starts that code over). One second after the last press the longest completed code runs
// (the first of equals): the sound 0x34 first, then its effect; then every code starts over. The table: notes/
// QOL3_2026-11-10.md "Cheat codes".

#include <cstdint>

struct SW;

// orig3 0x00163F58 (from the pad read 0x1A8B88, the first pad only): the buttons now and the frame before.
void MatchCheatCodes(uint32_t grfbtnOld, uint32_t grfbtn);
// orig3 0x00164028 (every frame): a second after the last press, the best code runs.
void UpdateCheatCodes(SW* psw);
// orig3 0x3448E0 (set by the code R1 U U R1 R1 D): every episode open in the episode menu (epmenu3.cpp).
bool FCheatAllEpisodes();
// (tests / the debug menu) the code index last run, -1 none
int IcheatLast();
// (the debug menu) the table, and a code's effect run now (with its sound, as entered)
struct CHEATDESC { const char* pchzName; const uint32_t* an; bool fSave; };
int CCheatCodes();
CHEATDESC CheatCodeDesc(int i);
void RunCheatCode(SW* psw, int i);
