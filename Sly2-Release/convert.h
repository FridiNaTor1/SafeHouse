#pragma once

// The Sly 2 disc converter (convert.cpp).

#include <string>

// The converted layout's format (extraction_manifest.json "format_version"); the launcher's asset version for Sly 2
// (platform/install.h sly2AssetVersion) moves with it.
// 2 (2026-11-04): the audio as WAV, the movies as M2V + WAV, the banks' samples as WAV. The runtime also reads 1 (the
// PS2 formats: VAG, VPK, PSS), which the gates' older data may still be.
constexpr int SLY2_FILES_FORMAT = 2;
constexpr int SLY2_FILES_FORMAT_PS2 = 1;

// --ps2-formats (with --convert): the audio and movies as on the disc (VAG, VPK, PSS: format 1), for comparisons.
extern bool g_fConvertPs2;
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
// checked, its files converted into out; "" when done, "cancelled", or what went wrong. progress(done, total, stage,
// file) is called per world and per file; false from it cancels.
using CaneConvertProgress = std::function<bool(std::size_t done, std::size_t total, const char* stage, const char* file)>;
std::string CaneConvertDisc(const std::string& iso, const std::string& out, const CaneConvertProgress& progress);
#endif
