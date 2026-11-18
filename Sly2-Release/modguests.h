#pragma once

// PC addition (docs/MODDING.md "Characters from other levels", platform/mod_worlds.h "guests"): mod packages' guests,
// characters of another level (guards, NPCs, bosses) brought into a world with their behaviour. The guest's own level
// is loaded aside as a kept world (pcloader.h: its classes, animations, AI data, sounds, effects and textures stay
// there) and a live copy of the character is made in the world being loaded, started with it, at the guest's place;
// a guard given a path of this world walks along it (a command list of one "walk along" made as the spawners make
// theirs). Every platform: nothing here is the editor's.

#include <cstdint>
#include <string>

struct SW;
struct LO;

// transition.cpp, before the kept worlds are pumped (no world running): the guests' own levels asked for, so they
// load with the transition
void RequestModGuestWorlds(const uint8_t* abChk);
// --guest FROM:OBJECT@X,Y,Z[,HEADING][:PATROL] (a test aid: a guest in every world loaded); false: not understood
bool FAddGuestArg(const std::string& arg);
// The guests' copies in the running world (their roots; the editor and the tests)
int CModGuestsLive();
LO* PloModGuestLive(int i);
