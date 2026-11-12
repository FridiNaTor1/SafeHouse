#pragma once

// The Sly 2 disc converter (convert.cpp).

#include <string>
#include <vector>

// The converted layout's format (extraction_manifest.json "format_version"); the launcher's asset version for Sly 2
// (platform/install.h sly2AssetVersion) moves with it.
// 2 (2026-11-04): the audio as WAV, the movies as M2V + WAV, the banks' samples as WAV. The runtime also reads 1 (the
// PS2 formats: VAG, VPK, PSS): the Switch's installations, and the gates' older data.
constexpr int SLY2_FILES_FORMAT = 2;
constexpr int SLY2_FILES_FORMAT_PS2 = 1;

// --ps2-formats (with --convert): the audio and movies as on the disc (VAG, VPK, PSS: format 1), decoded as the game
// plays them. The Switch installs them so (the European disc: 3.96 GB with its 11 languages, 10.83 GB as WAV).
extern bool g_fConvertPs2;
// --languages English,French (with --convert): the languages installed, of the disc's (empty: every one). The others'
// dialog lines and movie sound tracks are left out, and the game offers only these (region.h CLanguages).
extern std::vector<int> g_alangConvert;
// A language (region.h LANGUAGE) by the converter's name ("English", any case); -1 when there is none.
int LanguageOfConvertName(const std::string& name);
// --flac / --no-flac (with --convert): the dialog lines and the movies' sound tracks as FLAC or WAV; -1 (default):
// FLAC when the disc has more than one language (the European discs).
extern int g_nConvertFlac;
// (with --convert) every world's textures as PNG in Textures/ (the dump Mods/Textures takes): on by default since
// 2026-11-04 (USA 7051 PNGs, ~80 MB, +40 s); --no-export-textures turns it off.
// Without --convert, --export-textures writes them into an existing installation (texmod.h ExportTexturesCommand).
extern bool g_fConvertTextures;

// --convert OUT: reads the open disc once and writes the layout into OUT.
int ConvertCommand(const std::string& out);
// --convert-list: the crawl's files and their paths, nothing written.
int ConvertListCommand();
// A world record's friendly name (the layout's folder and file name: "Paris"; record 1 "Front End"), or nullptr.
const char* SzConvertWorldName(int iwrec);

#ifdef __SWITCH__
#include <cstddef>
#include <functional>
// The Switch installer's conversion (platform/switch/installer.h, main.cpp's install job): the disc image opened and
// checked, its files converted into out with the languages given (the converter's names; empty: every one); "" when
// done, "cancelled", or what went wrong. progress(done, total, stage, file) is called per world and per file; false from
// it cancels.
using CaneConvertProgress = std::function<bool(std::size_t done, std::size_t total, const char* stage, const char* file)>;
std::string CaneConvertDisc(const std::string& iso, const std::string& out, const std::vector<std::string>& languages,
                            const CaneConvertProgress& progress);
// A disc image's languages when it has more than one (the European discs), in the disc's order: the converter's name
// and the language's own ("French", "Français"); `preferred` the one the console's language picks.
struct CaneLanguage
{
    std::string id, name;
};
std::vector<CaneLanguage> CaneDiscLanguages(const std::string& iso, std::size_t* preferred);
#endif
