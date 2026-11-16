#pragma once

// PC addition: the unravelled world files (converter format 3, notes/UNRAVEL_2026-11-10.md, CONVERTER.md "World
// parts"). A world file (Sly 2's own format, decompressed) is stored as its blocks, each a named file (or, when tiny,
// inline in the world's index), in the order the game reads them:
//
//   <World>.world.json            the index: the parts in order (the world file = their bytes one after the other)
//   World/Groups.bin              the group table
//   World/Sound.bin               the sound block (bank, ambience, dialog lines, music)
//   World/Progress.bin, World/World Records.bin, World/Nodes.bin, World/Options.bin
//   World/Text.json               the text (subtitles, menus, every language): JSON, compiled back at load
//   World/<nn> <group>/Scripts.splice   the group's splice scripts: text (S-expressions), compiled back at load
//   World/<nn> <group>/Graphics.bin     its CLUTs, textures, shader animations, shaders and bulk (VRAM) data
//   World/<nn> <group>/Collision.bin    its collision mesh
//   World/<nn> <group>/Objects.bin      its objects (models, animations, actors, ...); the index lists each object
//   World/Tail.bin                the file's end (never read by the game)
//
// The game reads the world by putting the parts back together (FRavelWorld): byte for byte the file the disc has,
// so every reader sees what it always saw. A mod replaces any part by putting a file at the same relative path in
// the mods folder.
//
// The parts are found by the game's own loader: LoadSwFromBrx marks where each block starts (MarkWorldPart) while
// the converter loads the world.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

enum WPK : int
{
    WPK_Groups,
    WPK_Sound,
    WPK_Progress,
    WPK_Records,
    WPK_Text,
    WPK_Nodes,
    WPK_Options,
    WPK_Bulk,
    WPK_Scripts,
    WPK_Graphics,
    WPK_Collision,
    WPK_Objects,
    WPK_Tail,
    WPK_Max
};

struct WPMARK
{
    WPK wpk;
    int iwe;          // the group (-1: the world's header)
    std::size_t ib;   // where the block starts in the decompressed file
};

// Set while the converter loads a world: the loader's marks go here (else nothing is recorded).
extern std::vector<WPMARK>* g_pawpmark;
inline void MarkWorldPart(WPK wpk, int iwe, std::size_t ib)
{
    if (g_pawpmark) g_pawpmark->push_back({wpk, iwe, ib});
}

// The index's "format" / "version".
constexpr const char* SLY2_WORLD_PARTS_FORMAT = "sly2-world-parts";
constexpr int SLY2_WORLD_PARTS_VERSION = 1;

struct WORLDPARTFILE
{
    std::string rel;              // relative to the world's folder ("World/Text.json")
    std::vector<uint8_t> ab;
};

// A world file split at the loader's marks. agroupName: the groups' names (the folders); fWide: the Japanese build's
// 16-bit text. On return: the index (the parts, the groups, every group's objects) and the files to write beside it.
// Every text part is checked to compile back to its bytes; one that does not stays binary (a warning).
// aalign: the loader's Align(16) calls ({position, bytes skipped}: bis.h g_paalignRecord), kept in the index ("aligns":
// from the part's end) so a part of another length (a mod) is padded again where the loader aligns.
bool FUnravelWorld(const std::vector<uint8_t>& ab, const std::vector<WPMARK>& amark, const std::vector<std::pair<std::size_t, std::size_t>>& aalign,
                   const std::vector<std::string>& agroupName,
                   bool fWide, nlohmann::ordered_json& index, std::vector<WORLDPARTFILE>& afile, std::vector<std::string>& awarning,
                   std::string& error);

// An object of a group's Objects part (the loader's record: S8 k [k S16 filters] S16 cid S32 oid S16 n S32 cb, cb bytes).
struct WORLDOBJREC
{
    int iwe = -1;
    int k = 0;
    int cid = 0;
    int oid = 0;
    std::size_t ib = 0, cb = 0;   // the whole record in the part
};

// Which objects a reduced load keeps (PC addition, pcloader.h: a guest character without its whole world). Null: all.
using PFNKEEPOBJ = std::function<bool(const WORLDOBJREC&)>;
// The path on disk of a part (rel: relative to the installation, "/" separated): the mods folder's, else the installed.
using PFNPARTPATH = std::function<std::string(const std::string& rel)>;

// The world file from its parts. relDir: the index's folder relative to the installation. With pfnKeep, the objects
// it rejects are left out (their group's object count and padding made again).
// psetDrop: groups loaded empty (with pfnKeep: but for the objects it keeps) (no textures, collision or objects; their scripts kept): a world without its level
// (CDisc::ReadWorldReduced, the guest characters' kept worlds).
bool FRavelWorld(const nlohmann::json& index, const std::string& relDir, const PFNPARTPATH& pfnPath, std::vector<uint8_t>& ab,
                 std::string& error, const PFNKEEPOBJ& pfnKeep = nullptr, const std::set<int>* psetDrop = nullptr);
// The same with the parts' bytes from pfnBytes (rel: relative to the installation; false: no such part).
using PFNPARTBYTES = std::function<bool(const std::string& rel, std::vector<uint8_t>& ab)>;
bool FRavelWorldFrom(const nlohmann::json& index, const PFNPARTBYTES& pfnBytes, std::vector<uint8_t>& ab, std::string& error,
                     const PFNKEEPOBJ& pfnKeep, const std::set<int>* psetDrop, const std::string& relDir);
// The groups the index marks "level" (the world's own: its code's groups, "<code>", "<code>_rig", ...).
std::set<int> SetLevelGroups(const nlohmann::json& index);
// Marks the level groups in an index (the converter): a group named the world's code or "<code>_...".
void MarkLevelGroups(nlohmann::ordered_json& index, const std::string& code);

// The codecs (also the tools' --world-text / --world-scripts).
bool FTextBlockToJson(const uint8_t* pb, std::size_t cb, bool fWide, nlohmann::ordered_json& j, std::string& error);
bool FTextJsonToBlock(const nlohmann::json& j, std::vector<uint8_t>& ab, std::string& error);
bool FSpliceToText(const uint8_t* pb, std::size_t cb, const std::string& title, std::string& text, std::string& error);
bool FSpliceFromText(const std::string& text, std::vector<uint8_t>& ab, std::string& error);
// A group's Objects part's records (false: not records end to end).
bool FParseObjects(const uint8_t* pb, std::size_t cb, int iwe, std::vector<WORLDOBJREC>& arec, std::string& error);

// Tools (main.cpp): --world-unpack IN.brx DIR (a world file into parts: the converter's split, for a world from
// anywhere), --world-pack INDEX.world.json OUT.brx (the parts back into one file), --world-check DIR (every world of
// an installation put back together and compared with the index's hash).
int WorldUnpackCommand(const std::string& in, const std::string& dir, int filter);
int WorldPackCommand(const std::string& index, const std::string& out);
int WorldCheckCommand(const std::string& dir);

// FNV-1a 64 as the index records it ("fnv64": the world file's hash, hex).
std::string StrFnv64(const uint8_t* pb, std::size_t cb);
