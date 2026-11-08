#pragma once

// Sly 3's episode chooser: the EPMENU screen object (class 0xA2, vtable 0x357B20: init 0x17AFA8, start
// 0x17AFF0 (slot 17), update 0x17B298 (slot 18), module 0x17AE78..0x17CFC0) of dvd_menu (world 0, oid 165135), where
// the prologue's end, the hazard room's door and the pause menu's Exit to Game Menu go. A wheel of the episodes
// (0 Two Player, 1 the Intro "Beginning of the End", 2 the Hazard Room, 3..8 Episodes 1..6), each with a page of
// entries (Start / Continue Episode, its movies, Jobs && Challenges, Gallery, Bonus Movies); X starts the episode
// (0x289198). Fields: +0x124 the episode shown, +0x128 the last one open, +0x12C the page's entry, +0x130 the list,
// +0x140 the state (0..31), +0x144 the state after the current one, +0x1E8 / +0x1EC the backdrop's idle parts.

#include <string>

struct SW;
struct LO;

// (StartFrontEnd) the world's EPMENU started (orig3 0x17AFF0); none: nothing
void StartEpMenu3(SW* psw);
// (UpdateFrontEnd) its input and states (orig3 0x17B298, after the screen object's own update)
void UpdateEpMenu3(SW* psw);
bool FEpMenu3Active();
// the chooser's script strings (1468..1500: the wheel's names, Start / Continue, percent, time, back); false: not one
bool FStrEpMenu3(int id, std::string* pstr);

// orig3: 0x001D5598's first half (the pause menu's Exit to Game Menu): unless a return is set already, the chooser
// comes back on episode iep's page (the Hazard Room's: the wheel) (GAME +0x1488 / +0x148C / +0x1490)
void SetEpMenuReturn3(int iep);

// orig3: 0x0025E4A0: the episode's jobs all complete (0x349B20: {count, names}; each "<job>" "is_complete")
bool FEpisodeComplete3(int iep);
// orig3: 0x0025E1F0: the episode's play time ("episode" <name> "dt")
float DtEpisode3(int iep);
// orig3: 0x0025E5E8: its percent (jobs 0x349B20 and challenges 0x349B68 done of all: 0, 1..99, 100)
int NEpisodePercent3(int iep);
// orig3: 0x00289198: start episode iep (the Intro's binocucom task 1798 while it is not complete, else the episode's
// first world at the default point)
void StartEpisode3(SW* psw, int iep);
