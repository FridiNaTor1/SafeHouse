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
// (PC) The voices' language when it is not the text's (-1: the same as the text, g_nLang): the dialogue lines, the
// binocucom, the movies' sound. Japanese voices with English text, English voices with Japanese text, ... The text,
// its font and the port's own strings follow g_nLang; the lines' keys, lengths (so the subtitles' timing) and the
// movies' sound track follow LangVoice(). The line ids are the same in every language (the subtitles are the lines'
// text ids), so any pair works; a language with no voice lines of its own (a PS3 import, a subtitle mod) is offered
// for the text only.
extern int g_nLangVoice;

// orig 0x1A2C48: the disc's languages (its region's list in the executable); orig 0x1A2C98: their count.
int CLanguagesDisc();
int LanguageOfDiscIndex(int i);
bool FLanguageOnDisc(int lang);
// (PC) The languages the player can choose: the disc's, then the ones the shared languages folder and the mods
// folder add (langstore.h: other regions' languages, imports, user-made dubs and subtitles). With two or more, the
// title offers the language menu on every disc, as the European build does with its own. Custom languages (not one
// of the game's 14) have ids from LANG_Max on.
int CLanguages();
int LanguageOfIndex(int i);
bool FLanguageAvailable(int lang);
// The language whose world-file data is read (the text block's strings, the dialog lines' keys): g_nLang when the
// disc has it, else the disc's language that the chosen one falls back to (English, else the disc's first).
int LangWorld();
// The voices' language (g_nLangVoice when it can be played, else g_nLang) and the language whose dialog keys the
// world files are read with for it (LangWorld() when it is the text's).
int LangVoice();
int LangWorldVoice();
// What a language can be chosen for on this disc: the text (the disc's, or the folder's with text this disc can
// draw) and the voices (the disc's, or the folder's with voice lines of their own).
bool FLanguageTextAvailable(int lang);
bool FLanguageVoiceAvailable(int lang);
// (the pause menu's Language page) the text / voices chosen there, applied when the next world loads (the world's
// text and lines are read in the language as it loads): -2 nothing pending; voice -1 the same as the text.
void SetPendingLanguages(int langText, int langVoice);
bool FLanguagesPending(int* plangText = nullptr, int* plangVoice = nullptr);
// (at the start of a world's load) the pending choice applied; it is the session's (as the title's) and Settings.ini's.
void ApplyPendingLanguages();
// The title's language menu: the text and the voices both become lang (and are saved): nobody stays in a language
// they cannot read.
void ChooseLanguageOnTitle(int lang);
// The language the text is shown in (the port's own text's column, porttext.h): g_nLang, or LangWorld() when the
// chosen language's text cannot be shown with this disc's fonts (Japanese text on the USA / European discs).
int LangText();

// The starting language, after the disc is open (orig: the European / Japanese builds' boot 0x1613D0: the region
// set, then 0x1A2DA0 with the console's language). The PC's language stands in for the console's.
void InitLanguage();
// (PC, Settings.ini "Language") a language set in the settings: chosen for the session (no language menu at the
// title, it wins over a save's); "" or one not installed: nothing changes.
void ApplyLanguageSetting(const std::string& key);
// (Settings.ini "Voices") the voices' language: "" the same as the text; one without voices on this disc: ignored.
void ApplyVoiceLanguageSetting(const std::string& key);
// A language's id by its folder / game name ("" or unknown: -1).
int LangOfSettingKey(const std::string& key);
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
// (PC) The text shown is 16-bit characters (the strings' UTF-8 form above) drawn with the Japanese font: the Japanese
// disc's own text, and on the other discs a language of the languages folder that brings that font (langstore.h:
// Japanese imported from the Japanese disc). It follows the world's text: set when a world's text loads
// (SetStoreWideChars), so a language chosen on the title changes it with the world that reloads.
bool FWideChars();
void SetStoreWideChars(bool fWide);
void AppendCh16(std::string& str, uint32_t ch);
// Text ids. The Japanese build's text has a few strings more and other memory card messages, so its ids from 0x12 on
// are the USA ids moved by 2..4 (found from the world files' texts and the executables' calls of the text lookup
// 0x13E368). The European builds' text has the Ratchet & Clank demo's string at 0x6D: their ids from there on are the
// USA ids + 1 (v2.01: "WE'LL BE RIGHT BACK!" moved to 0x27F, the job texts between in place). The port's own constants are USA ids: IdText gives the disc's id for one; ids that come from the disc's
// data (world files, the executable's tables, the scripts) are the disc's already. IdTextUsa is the inverse (-1: the
// disc's string has no USA id).
int IdText(int idUsa);
int IdTextUsa(int id);
// The Japanese disc's id for a USA id, on any disc (the port's Japanese wording of its messages is kept by them).
int IdTextJapanOfUsa(int idUsa);

// The character at str[ib] (a byte, or a UTF-8 sequence of 16-bit text: FWideChars); *pcb its length.
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
// The text shown with Latin line breaks (at spaces): every text but Japanese text (drawn with the Japanese font).
bool FLatinLineBreaks();
// The European title's help line: the "Triangle: Language" label in each language, cycling (orig 0x1DE1F8's end,
// the table at 0x2CAA08); empty without one.
std::string StrLanguageHelpLabel(float tSinceShown);
