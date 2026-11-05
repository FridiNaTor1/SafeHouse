#pragma once

// The game's saved state (orig 0x3D4A60, 0x5408 bytes): the progress values (0x5400 bytes, laid out by the
// progress trie every world file carries, 0x1D7B88) and the layout's identity (+0x5400 the last record's
// field, +0x5404 the records' byte sum: a save of another layout is cleared, 0x1D7C80). A memory-card save
// slot file (BASCUS-97316YAOTWTD!/save1..3) is exactly this block, so PS2 saves load as they are; native saves
// go to <user-dir>/Saves/saveN in the same format.
//
// Progress values are read and written by path: names (case-insensitive hash, 0x1D8640) or numbers, walked
// down the trie (0x1D7DA0); an indexed node takes the next path element as its index.

#include "ref.h"

#include <cstdint>
#include <string>
#include <vector>

struct PROGRESS;

constexpr int CB_GAMESAVE = 0x5408;

struct GAMESAVE
{
    uint8_t ab[CB_GAMESAVE]{};
};

extern GAMESAVE g_gamesave;

// The progress layout of the loaded world (names and trie nodes, from 0x1D7B88)
void SetProgressLayout(const PROGRESS& progress);
// orig 0x1D7C80: the save is kept when it has the layout's identity, else cleared and stamped
void ValidateGameSave();

// orig 0x1D87C0 + 0x1D7ED8 (0x1D8A38): a progress value (Nil when the path does not resolve)
CRef RefGetProgress(const std::vector<std::string>& apath);
// writes a value at a path (orig 0x1D80C8); false when the path does not resolve or the kind differs
bool FSetProgress(const std::vector<std::string>& apath, const CRef& ref);
// A boolean of the save's settings ("flags" <name>: fReverseLookAround, fReverseAim, ...), false when unset.
bool FSettingFlag(const char* name);
// orig 0x1D8C10: the address of a path's int / float value in the save (null: none); the actors' "hpCur" / "mpCur"
int32_t* PnProgressSlot(const std::vector<std::string>& apath);
// the same by the path's symbols (an option of kind 22: the original's progress references, 0x2581C0 / 0x258200)
CRef RefGetProgressKey(const std::vector<int16_t>& akey);
int NSymProgress(const std::string& name);
std::string StrProgressKey(const std::vector<int16_t>& akey);   // (debug) the path's names  // orig 0x1D8720: a path element's symbol
bool FSetProgressKey(const std::vector<int16_t>& akey, const CRef& ref);

// orig 0x1D82B8: a save's game percent (the episodes' percent_complete weighted 0x2C2DC0, and 26 for each vault:
// the "fVault" bool entries of the layout)
int NGamePercent(const GAMESAVE& gs);

// orig 0x161478: the play times ("dt", the current episode's "dt") advanced by the game step (UpdateClock)
void UpdateGameTimers(float dt);

// the slot files
bool FLoadGameSave(const std::string& path, std::string& error);
bool FWriteGameSave(const std::string& path, std::string& error);
// orig 0x1D84B0: the record as a card slot holds it (every "from_memcard" set)
void CopyGameSaveForSlot(GAMESAVE& gs);
bool FWriteGameSaveBlock(const std::string& path, const GAMESAVE& gs, std::string& error);

// (QoL Replay Jobs, replay.h; not in the original) gs made to keep the progress gsKeep had: per task / job entry
// ("t<n>_*" / "m<n>_*") the higher "state", "complete_count", "focus_count" and "is_complete" set if either has it;
// each episode's higher "percent_complete"; the higher "episodeBest"; gsKeep's "widCurrent", "episodeCurrent" and
// "oidPoCurrent" (where the game resumes). Everything else stays gs's (coins, items, times, settings).
void MergeProgressKeepDone(GAMESAVE& gs, const GAMESAVE& gsKeep);
