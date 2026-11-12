#pragma once

// The languages beyond the disc's own (PC QoL, lang 2026-11-10; notes/LANG_2026-11-10.md, MODDING.md "Languages").
//
// Every Sly 2 installation (USA, Europe, Japan) shares one languages folder, `sly2-languages` beside the
// installations (the launcher's --languages-dir; default: the installation's parent folder). Each language is a
// folder there, and the mods folder's `Languages/<folder>/` adds to it or overrides single files:
//
//   <Folder>/                   "French", "Japanese", "English (UK)", "Klingon": the folder name is the key; the
//                               game's 14 language names ("English" .. "Chinese") are those languages
//     language.json             optional: {"name": "Français", "code": "fr", "script": "latin" | "japanese",
//                               "text": "<folder>", "voice": "<folder>" (what is missing comes from there; default
//                               English), "source": "Europe v2.01" (the disc it came from)}
//     Text.json                 the strings by USA text id (the Mods/Text format: textmod.h)
//     Dialog/**/<line>.flac|wav|vag   dialog lines by USA line id (= the subtitle's text id); any subfolders
//     Dialog/lengths.json       optional: {"<line>": seconds} (the world files' line lengths; default: the audio's)
//     Movies/<Name>.flac|wav    a movie's sound by its name (CONVERTER.md: "Prologue", "Ep1 Intro", ...)
//     Font.bin                  optional ("script": "japanese"): the Japanese disc's text font (kana, kanji, its
//                               ASCII), which the import takes from the user's own Japanese disc; without it the
//                               USA / European discs cannot show the language's text (format: langstore.cpp)
//
// The disc's own languages play from the world files as before (byte for byte: the gates). A language the disc
// lacks is offered on the title's language menu (region.h CLanguages) and, chosen, plays the world files' text and
// dialog of LangWorld() with the folder's strings, lines and movie sound over them. Japanese text on the USA /
// European discs is drawn with the language's own Font.bin (region.h FWideChars), with the Japanese build's line
// breaks and its few wider text boxes (splayer.cpp); a Japanese language without that file falls back to the disc's
// English text (the title's language menu and the launcher then name it "voices only"). Latin text on the Japanese
// disc is shown without accents (its font has ASCII only).

#include "disc.h"

struct SELEM;

#include <string>
#include <utility>
#include <vector>

// Scans the languages folder (g_disc.DirLanguages()) and the mods folder's Languages/ (after g_disc.FOpen, before
// InitLanguage; registers every dialog line as an external file: disc.h IsectorExternal).
void InitLanguageStore();
// The store languages offered on this disc (not the disc's own, not ones taken from this very disc), in menu order.
const std::vector<int>& AlangStoreOffered();
// A store language's folder name (custom languages; nullptr: none).
const char* SzLangStoreKey(int lang);
// The language of a folder name (-1: none).
int LangStoreOfKey(const char* key);
// A custom language's display name in the game's text encoding ("" for the game's 14: the disc's text names them).
std::string StrLangStoreDisplayName(int lang);
// The disc language a store language's text comes from when it has none of its own (-1: none / its own).
int LangStoreTextFallback(int lang);

// g_nLang's text comes from the store (it is not the disc's, and its text can be shown), and that text's language
// / script.
bool FStoreTextShown();
int LangStoreTextLanguage();
bool FStoreTextWide();
// A store language whose text this disc cannot show (Japanese script without its Font.bin on a USA / European disc):
// chosen, it plays its voices and movie sound over the disc's text. The language menu says so.
bool FLangStoreVoicesOnly(int lang);
// A store language with voice lines of its own (it can be chosen as the voices' language: region.h g_nLangVoice).
bool FLangStoreHasVoices(int lang);
// (LoadTextFromBrx, before the strings) g_nLang's text as this world shows it: sets region.h's FWideChars and the
// font below.
void LatchLanguageStoreText();
// The text font of the languages folder while FWideChars() on a USA / European disc (a set element: the characters,
// their widths, their glyphs, as a Japanese world's font: screenobj.h), else nullptr.
const SELEM* PsetLanguageStoreFont();

// (LoadTextFromBrx) g_nLang's store strings over the world's (USA ids mapped to the disc's: region.h IdText): a
// language the disc lacks (fDiscMods false), or the mods folder's Mods/Languages/<key>/Text.json over a disc language
// (fDiscMods true: counted as a text mod).
void ApplyLanguageStoreText(std::vector<std::pair<int, std::string>>& atext, bool fDiscMods);
// (LoadMusicFromBrx) g_nLang's store dialog line for the disc's line id: its key (an external sector) and length.
bool FLanguageStoreDialog(int idLineDisc, FK& fk, float& tLength);
// (the movie player) g_nLang's store sound for the movie at isector ("": none: the disc's own track).
std::string PathLanguageStoreMovieAudio(uint32_t isector);

// UTF-8 Latin text for the Japanese disc's font (ASCII, kana, kanji): accents dropped, typographic marks plain.
std::string StrFoldLatinForWideFont(const std::string& utf8);
// The game's language names as folders ("English", "French", ...: textmod.h SzLanguageModName).
int LangOfGameName(const std::string& name);

// (--import-languages) the world loaders report every language's strings and dialog lines while this is set.
struct LANGCAPTURE
{
    virtual ~LANGCAPTURE() = default;
    virtual void Text(int lang, int id, const std::string& str) = 0;          // the disc's id and encoding
    virtual void Dialog(int lang, int id, const FK& fk, float tLength) = 0;   // the disc's line id
    virtual void Font(const SELEM&) {}                                        // a font set of a Japanese world
};
extern LANGCAPTURE* g_plangcapture;

// --import-languages: the source opened as g_disc (a disc image, a converted installation) into the languages folder
// `dirOut`. Writes Text.json, the dialog lines, the movies' sound and language.json of every language of the source
// the folder lacks (or all with fReplace). fMove (an installation): the installation's own language files are moved
// into the folder (or deleted, when the folder has the same already) and its manifests point there ("@languages/":
// the converter's install, and the launcher's "share this installation's languages"). Prints "progress N M item".
struct LANGIMPORT
{
    std::string dirOut;
    std::string dirInstall;          // the installation to move from (fMove), else ""
    bool fMove = false;
    bool fReplace = false;
    std::vector<std::string> alangOnly;   // only these (folder names), else every language of the source
    const char* szProgress = "progress";  // the progress lines' word (the converter's install: "languages")
};
int ImportLanguagesCommand(const LANGIMPORT& opts);
// --list-languages: the available languages as JSON lines (the launcher's languages list).
int ListLanguagesCommand();
// --language-selftest (no disc), --check-language KEY (a disc / an installation and the languages folder): langstore.cpp
int LanguageSelfTest();
int CheckLanguageCommand(const std::string& key);
