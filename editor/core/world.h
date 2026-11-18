#pragma once
// The map editor's core (desktop only: the launcher's Editor page and Sly2-Release --editor). Reads an installed
// Sly 2 or Sly 3 world from its parts (Sly2-Release/worldparts.h, converter format 3) without running the game: the world
// list, a world's index (groups, parts, the groups' top-level object records), its text and scripts. A project
// (project.h) overlays changed parts on the installation and is itself a mod package (platform/mod_package.h).
// Design: Sly2-Release/notes/EDITOR_2026-11-12.md.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cane::editor {

namespace fs = std::filesystem;

std::string utf8(const fs::path& p);
fs::path pathOf(const std::string& s);
bool readFile(const fs::path& p, std::vector<uint8_t>& ab);
bool writeFile(const fs::path& p, const std::vector<uint8_t>& ab);

struct WorldEntry {
    int record = -1;          // the world record (Sly2-Release --world N)
    std::string code, name;   // "f_nightclub_exterior", "Paris"
    int episode = 0, kind = 0;
    std::string index;        // the index's path relative to the installation (".../Paris.world.json")
    fs::path indexBase;       // (a mod package's new world: newworld.h) the folder index is relative to; empty: the installation
    std::string templateName; // (a new world) its template world's name
};

// An installation: Sly 2's converted files (extraction_manifest.json, format 3: the worlds as parts), or Sly 3's disc
// image with its worlds as parts beside it (Worlds/worlds.json, "sly3-worlds"; dir: the image's folder or the image).
// The parts' paths are relative to dir either way ("Worlds/..."), as the games' mod files are.
struct Installation {
    fs::path dir;
    std::string game = "sly2", region, disc;
    int filesFormat = 0;
    std::vector<WorldEntry> worlds;
    bool open(const fs::path& dir, std::string& error);
    bool sly3() const { return game == "sly3"; }
    const WorldEntry* world(int record) const;
};

// A top-level object record of a group's Objects part (S8 k [k S16 filters] S16 cid S32 oid S16 n S32 cb, cb bytes).
// Sly 3's header (fSly3): S8 mode, mode 1: U8 n1, U8 n2, n1 S16 filters, n2 S32 conditions; k is the mode.
struct ObjRecord {
    int k = 0, cid = 0, oid = 0, n = 0;
    std::size_t ib = 0, cb = 0;   // the whole record in the part
    std::size_t ibBody = 0;       // its data
};
bool parseObjects(const uint8_t* pb, std::size_t cb, std::vector<ObjRecord>& arec, std::string& error, bool fSly3 = false);

struct GroupInfo {
    int index = -1;
    std::string name, folder;   // name without the "FK$X" prefix
    bool level = false;
    std::string objectsPart, scriptsPart;   // relative to the installation ("" when inline / none)
    std::vector<ObjRecord> objects;         // from the index ("offset" / "size"; the class name below)
    std::vector<std::string> classNames;
};

struct WorldIndex {
    WorldEntry entry;
    std::string relDir;                 // the index's folder relative to the installation
    nlohmann::json json;
    std::vector<GroupInfo> groups;
    std::string textPart;               // World/Text.json (relative to the installation)
    std::size_t size = 0;
    std::string fnv64;
    // pfnRead: the part's bytes (the project's, else the installation's)
    bool load(const Installation& inst, const WorldEntry& e, std::string& error);
    int maxOid() const;
};

// The text strings of a Text.json part: id + the first language's text.
struct TextString {
    int id = 0;
    std::string text;
};
bool readTextStrings(const std::vector<uint8_t>& ab, std::vector<TextString>& a, std::string& language, std::string& error);

}  // namespace cane::editor
