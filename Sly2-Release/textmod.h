#pragma once

// Text mods: Mods/Text/<Language>.json replaces the game's strings of that language as each
// world's text block loads (LoadTextFromBrx). --dump-text DIR writes the disc's strings in the same format.
//
//   {
//     "language": "English",
//     "strings": { "109": "The Black Chateau", ... },          every world's string with that id
//     "worlds": { "Paris": { "5021": "...", ... }, ... }      one world's own (wins over "strings")
//   }
//
// Keys are the string ids, decimal (or "0x6D"). Only ids the world already has are replaced; nothing is added. Strings
// are UTF-8 in the file; the game's fonts have the Latin-1 characters (others become '?'). Control sequences such as
// "&2T&." (a button glyph) are the game's own and are kept as written.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The world file being loaded (FLoadWorldFile): its start sector names the world for "worlds".
extern uint32_t g_isectorWorldLoading;

// The mods folder's strings for the current language (g_nLang) applied to a world's text.
void ApplyTextMods(std::vector<std::pair<int, std::string>>& atext);

// --dump-text DIR: every language of the disc to DIR/<Language>.json.
int DumpTextCommand(const std::string& dir);

// The game's language names as the mods folder spells them ("English", "French", ...).
const char* SzLanguageModName(int lang);
