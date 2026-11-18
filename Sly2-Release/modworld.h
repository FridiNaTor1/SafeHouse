#pragma once

// PC addition (docs/MODDING.md "New levels", platform/mod_worlds.h): mod packages' new worlds and the ways into them,
// at run time. A package's new world gets a world record after the disc's 44 (44, 45, ... in package load order: so
// it is named, not numbered, outside the process: --world NAME) and a 64-byte key of its own whose sector is the world's
// (stable: from the package's and the world's ids) (disc.h ISECTOR_MODWORLD), so the transition, the warps, --world N
// and the editor load it as any world. It loads with its template's world filter (the template's objects with filter
// lists stay). Entries: an existing exit object's world key redirected, or a sphere that sends the player there.

#include "disc.h"
#include "lo.h"

#include <string>
#include <vector>

struct SW;
struct WREC;

// The disc's world records (the boot world's list) + the packages' new worlds. Called wherever the list is taken from
// a loaded world (the transition, FResolveWorld).
void AppendModWorldRecords(std::vector<WREC>& awrec);
// The world filter a record loads with (a new world: its template's record; else the record itself)
int IwrecFilterOf(int iwrec);
// A new world's record: its package's entry (null: a disc world)
const CDisc::MODWORLD* PmodworldOfRecord(int iwrec);
bool FModWorldRecord(int iwrec);
// The record of the world named name (a disc world's or a new world's name), -1: none
int IwrecOfWorldName(const std::vector<WREC>& awrec, const std::string& name);
// The record of a world named on the command line (--world NAME / --editor NAME): a world's name (any case), a new
// world's id ("@id", "package/id"); -1: none. The records list must be made (EnsureWorldRecords).
int IwrecOfWorldArg(const std::string& arg);
// The world started (transition.cpp, after the checkpoint): a new world's spawn (when no checkpoint placed the
// player), the entries of this world armed, the exits they redirect pointed at their worlds
void OnModWorldStarted(SW* psw, int iwrec, bool fCheckpoint);
// Every frame (game.cpp, after the world update): a sphere entry the player walks into starts its transition
void UpdateModEntries(SW* psw);
