#pragma once

// Text mods: Mods/Text/<Language>.json replaces the game's strings of that language as each
// world's text block loads (LoadTextFromBrx). --dump-text DIR writes the disc's strings in the same format.
//
//   {
//     "language": "English",
//     "strings": { "109": "The Black Chateau", ... },          every world's string with that id
//     "worlds": { "Paris": { "5021": "...", ... }, ... }      one world's own (wins over "strings")
//     "port": { "HoldToSkip": "...", ... }                    (Mods/Languages/<Language>/Text.json and the languages
//   }                                                          folder's only) the port's own strings: porttext.h
//
// Keys are the string ids, decimal (or "0x6D"). Only ids the world already has are replaced; nothing is added. Strings
// are UTF-8 in the file; the game's fonts have the Latin-1 characters and the discs' typographic marks (Windows-1252's
// 0x80..0x9F: ’ “ ” … – — Œ œ ...; others become '?'). Control sequences such as
// "&2T&." (a button glyph) are the game's own and are kept as written.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The world file being loaded (FLoadWorldFile): its start sector names the world for "worlds".
extern uint32_t g_isectorWorldLoading;

// The mods folder's strings for the current language (g_nLang) applied to a world's text.
void ApplyTextMods(std::vector<std::pair<int, std::string>>& atext);

// --dump-text DIR: every language of the disc to DIR/<Language>.json (with the port's own strings in that language,
// "port", porttext.h), and DIR/usage.json: the worlds each string is in ({"worlds": [names], "strings": {"<USA id>":
// [world indexes]}}).
int DumpTextCommand(const std::string& dir);
// --dump-port-text DIR (no disc): the port's own strings of every built-in language, DIR/<Language>.json {"port": {}}.
int DumpPortTextCommand(const std::string& dir);

// The game's language names as the mods folder spells them ("English", "French", ...).
const char* SzLanguageModName(int lang);

// The game's text encoding (Latin-1 bytes, 0x80..0x9F Windows-1252's typographic marks; the Japanese disc's UTF-8
// form) to UTF-8 and back (the fonts' missing characters become '?'). The text files of older versions have the
// marks as U+0080..U+009F (Latin-1 read plainly): those still give the same bytes.
std::string Utf8OfGameText(const std::string& s);
std::string GameTextOfUtf8(const std::string& s);
// The USA / European fonts' bytes 0x80..0x9F as Windows-1252's characters (’ … “ ” – — Œ œ ...) and back (-1: none)
uint32_t CpOfCp1252(uint8_t b);
int ByteOfCp1252(uint32_t cp);
std::string Utf8OfCp1252(const std::string& s);
