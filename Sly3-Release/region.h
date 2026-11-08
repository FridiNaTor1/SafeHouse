#pragma once

// The disc's region and the game's language.
//
// One code base for every disc. What differs by region:
// - the executable's addresses (CDisc translates the USA addresses the port uses: disc.h);
// - the world files' block order (LoadSwFromBrx: the European and Japanese builds read the sound block after the
//   world records, so that a memory card's language is known before the world's text and dialog are chosen);
// - the languages on the disc (the executable's region table 0x2BFBD0: the USA disc English only, the European
//   discs 11, the Japanese disc Japanese only), and with two or more of them the title's language menu (the European
//   build's 0x1DDB68 / 0x1DDFB8 / 0x1DDCA8).
// The world files hold every language's text and dialog lines; the movies hold every language's sound track.

#include <cstddef>
#include <cstdint>
#include <string>

// The game's language ids (the order of the executable's names "english", "french", ...).
enum LANGUAGE : int
{
    LANG_English, LANG_French, LANG_Italian, LANG_German, LANG_Spanish, LANG_Dutch, LANG_Portuguese, LANG_Danish,
    LANG_Finnish, LANG_Norwegian, LANG_Swedish, LANG_Japanese, LANG_Korean, LANG_Chinese, LANG_Max
};

// orig 0x2E1E54 (USA): the language of the text, the dialog and the movies' sound.
extern int g_nLang;
// orig (Europe) 0x2CA958: the language chosen on the title's menu this session, or -1. It wins over a save's.
extern int g_nLangChosen;

// orig 0x1A2C48: the disc's languages (its region's list in the executable); orig 0x1A2C98: their count.
int CLanguages();
int LanguageOfIndex(int i);
bool FLanguageOnDisc(int lang);

// The starting language, after the disc is open (orig: the European / Japanese builds' boot 0x1613D0: the region
// set, then 0x1A2DA0 with the console's language). The PC's language stands in for the console's.
void InitLanguage();
// orig 0x1A2BD0: the language set (-1: the region's first).
void SetLanguage(int lang);
const char* SzLanguage(int lang);

// The world files' later block order (the European and Japanese builds' 0x13DA18).
bool FLaterWorldBlockOrder();

// The PS3 collection's world files: read only by the PS3 HD-asset tools, never by the game. Their
// layout is the European builds' with PS3 changes; every reader that differs checks this flag. Off by default.
extern bool g_fPs3World;

// The Japanese build (SCPS-15090): its text is 16-bit characters (orig JP 0x1184C8) and its fonts are kept by
// character (the screen objects' sets: orig JP 0x215DF8). The port keeps a 16-bit character c >= 0x80 in its
// std::string text as the UTF-8 encoding of c (Utf8Encode / ChDecode), so the text code stays byte based.
bool FWideText();
void AppendCh16(std::string& str, uint32_t ch);
// Text ids. The Japanese build's text has a few strings more and other memory card messages, so its ids from 0x12 on
// are the USA ids moved by 2..4 (found from the world files' texts and the executables' calls of the text lookup
// 0x13E368). The European builds' text has the Ratchet & Clank demo's string at 0x6D: their ids from there on are the
// USA ids + 1 (v2.01: "WE'LL BE RIGHT BACK!" moved to 0x27F, the job texts between in place). The port's own constants are USA ids: IdText gives the disc's id for one; ids that come from the disc's
// data (world files, the executable's tables, the scripts) are the disc's already. IdTextUsa is the inverse (-1: the
// disc's string has no USA id).
int IdText(int idUsa);
int IdTextUsa(int id);

// The character at str[ib] (a byte, or a UTF-8 sequence on the Japanese disc); *pcb its length.
uint32_t ChDecode(const std::string& str, std::size_t ib, std::size_t* pcb = nullptr);
// The menus' buttons (the pad's bits). The Japanese build confirms with Circle and goes back with Cross (the menus
// 0x1AEEE0, the episode select 0x15CF30, the shops 0x1DB870 / 0x1DAA90 / 0x1DCD30, the scripts' button wait
// 0x1E1560), and a slot is erased with Square; gameplay keeps Cross. USA / Europe: Cross, Triangle, Circle.
uint32_t BtnMenuConfirm();
uint32_t BtnMenuBack();
uint32_t BtnMenuErase();
// the back button's glyph in the menus' own lines ("&2T&." / the Japanese build's "&2X&.")
const char* SzMenuBackGlyph();
// the confirm button's glyph ("&2X&." / the Japanese build's "&2O&.")
const char* SzMenuConfirmGlyph();

// The Japanese build's line breaks (orig JP 0x220840, tables JP 0x2B25E8 / 0x2B2638): a line does not start with
// the closing marks, the small kana or the long vowel mark, and does not end with an opening mark.
bool FJapaneseNoBreakBefore(uint32_t ch);
bool FJapaneseNoBreakAfter(uint32_t ch);

// The European build's title: the language menu needed (orig 0x1DDB68: two or more languages, none chosen yet this
// session, no game on the card to continue, the title's input not yet taken).
bool FTitleNeedsLanguage(bool fTitleInputTaken, bool fContinueSlot);
// The European title's help line: the "Triangle: Language" label in each language, cycling (orig 0x1DE1F8's end,
// the table at 0x2CAA08); empty without one.
std::string StrLanguageHelpLabel(float tSinceShown);
