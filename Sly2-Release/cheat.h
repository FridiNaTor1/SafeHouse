#pragma once

// The original game's cheat codes (orig 0x14BF48..0x14C3F0, the list at 0x2DDEC0, the 15 codes at 0x2BD4E0): six
// d-pad / R1 presses entered while a menu is up (the menu's state 0x3DA0B8 = MENU +0x1E8 non-zero: the pause menu,
// the title's menus). Each change of the pad's held buttons to a non-zero set is matched against every code's next
// button (the whole set, so two buttons at once never match; a miss starts the code over). One second after the
// last such change the longest completed code runs: the sound 0x34 first, then its effect; then every code starts
// over. See notes/CHEATS_2026-11-10.md for the table.

#include <cstdint>

struct SW;

// orig 0x0014BF48 (from FReadJoy 0x187998, the first pad only): the held buttons changed from grfbtnOld.
void MatchCheatCodes(uint32_t grfbtnOld, uint32_t grfbtn);
// orig 0x0014BFF8 (from the HUD's update 0x1D9648, every frame): a second after the last press, the best code runs.
void UpdateCheatCodes(SW* psw);
// orig 0x2BE26C (set by the code R R1 D R1 D D, 0x14C1F0): every episode and movie open in the episode menu.
bool FCheatAllEpisodes();
// (tests / the debug menu) the code index 0..14 last run, -1 none
int IcheatLast();
// (the debug menu) the table, and a code's effect run now (with its sound, as entered)
struct CHEATDESC { const char* pchzName; const uint32_t* an; bool fSave; };
int CCheatCodes();
CHEATDESC CheatCodeDesc(int i);
void RunCheatCode(SW* psw, int i);
