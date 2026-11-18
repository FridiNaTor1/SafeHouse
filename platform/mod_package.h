#pragma once
// Mod packages (every platform: the games load them; the launcher's editor writes them). The format is documented in
// MODDING.md "Mod packages" and Sly2-Release/notes/EDITOR_2026-11-12.md:
//
//   <Mods>/Packages/<id>/            a package (a folder; "<id>.disabled" is skipped)
//     mod.json                        the manifest (below)
//     files/                          files at the installation's relative paths, as in the loose Mods folder
//                                     (e.g. "Worlds/1-Paris/World/227 jt/Objects.bin": one world part; the names from before
//                                     2026-11-17, "Worlds/1 The Black Chateau/Paris/...", match too: Sly2 RelCanonical)
//     editor/                         (optional) the editor's own project state; the games never read it
//
//   mod.json: {"format": "safehouse-mod", "format_version": 1, "id": "paris-benches", "name": "Paris benches",
//              "version": "1.0.0", "author": "...", "description": "...", "game": "sly2", "regions": ["USA"],
//              "files_format": 3, "priority": 0, "dependencies": [{"id": "other-mod", "version": ">=1.2"}],
//              "worlds": [{"path": "Worlds/.../Paris.world.json", "fnv64": "..."}],
//              "custom_tasks": [{"world": "Paris", "tasks": [1001, 1002]}]}       (customTasks below)
//
// "regions": the installations' regions (extraction_manifest.json "region": "USA", "Europe", "Japan", "Korea"; empty or
// absent: any). "files_format": the converter format the parts were made against (Sly 2: 3, the worlds as parts).
// Load order: dependencies first, then "priority" (lower first), then the id; a later package's file wins. The loose
// Mods folder wins over every package; a package given on the command line (the editor's play test) over all.
// A package whose game, region or format does not match, or whose dependency is missing or too old, is skipped with
// a message. Header-only (nlohmann::json, <filesystem>) so the games on every platform, the launcher and the editor
// share it.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace cane::mods {

inline constexpr const char* kFormat = "safehouse-mod";
inline constexpr int kFormatVersion = 1;
inline constexpr const char* kManifestName = "mod.json";
inline constexpr const char* kFilesFolder = "files";
inline constexpr const char* kPackagesFolder = "Packages";

struct Dependency {
    std::string id, version;   // version: "", "1.2", ">=1.2", "=1.2.3"
};

struct Manifest {
    std::string id, name, version = "1.0.0", author, description, game;
    std::vector<std::string> regions;
    int filesFormat = 0;
    int priority = 0;
    std::vector<Dependency> dependencies;
    nlohmann::json worlds = nlohmann::json::array();
    nlohmann::json extra = nlohmann::json::object();   // keys this version does not know, kept on a rewrite
    std::filesystem::path dir;                         // the package's folder (not stored)
    std::filesystem::path filesDir() const { return dir / kFilesFolder; }
};

inline std::filesystem::path pathOf(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
inline std::string utf8Of(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

// "1.10.2" > "1.9": numeric parts compared in order (missing parts are 0; text after a part's digits ignored).
inline int compareVersions(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) x = x * 10 + (a[i++] - '0');
        while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) y = y * 10 + (b[j++] - '0');
        if (x != y) return x < y ? -1 : 1;
        while (i < a.size() && a[i] != '.') ++i;
        while (j < b.size() && b[j] != '.') ++j;
        if (i < a.size()) ++i;
        if (j < b.size()) ++j;
    }
    return 0;
}

// have satisfies spec ("": any; ">=x", ">x", "<=x", "<x", "=x" / "x": at least x).
inline bool versionSatisfies(const std::string& have, const std::string& spec) {
    if (spec.empty() || spec == "*") return true;
    auto rest = [&](std::size_t n) { return spec.substr(n); };
    if (spec.rfind(">=", 0) == 0) return compareVersions(have, rest(2)) >= 0;
    if (spec.rfind("<=", 0) == 0) return compareVersions(have, rest(2)) <= 0;
    if (spec.rfind(">", 0) == 0) return compareVersions(have, rest(1)) > 0;
    if (spec.rfind("<", 0) == 0) return compareVersions(have, rest(1)) < 0;
    if (spec.rfind("==", 0) == 0) return compareVersions(have, rest(2)) == 0;
    if (spec.rfind("=", 0) == 0) return compareVersions(have, rest(1)) == 0;
    return compareVersions(have, spec) >= 0;
}

// A package id: letters, digits, '-', '_', '.', 1..64 characters (also the folder name).
inline bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64 || id[0] == '.') return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
    });
}

inline std::string idFromName(const std::string& name) {
    std::string id;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) id += char(std::tolower(static_cast<unsigned char>(c)));
        else if (!id.empty() && id.back() != '-') id += '-';
    }
    while (!id.empty() && id.back() == '-') id.pop_back();
    if (id.size() > 64) id.resize(64);
    return id.empty() ? std::string("mod") : id;
}

inline bool manifestFromJson(const nlohmann::json& j, Manifest& m, std::string& error) {
    try {
        if (j.value("format", "") != kFormat) { error = "not a SafeHouse mod manifest"; return false; }
        if (j.value("format_version", 0) < 1 || j.value("format_version", 0) > kFormatVersion) {
            error = "made by a newer SafeHouse (manifest format " + std::to_string(j.value("format_version", 0)) + ")";
            return false;
        }
        m.id = j.value("id", "");
        if (!validId(m.id)) { error = "the manifest's id \"" + m.id + "\" is not valid"; return false; }
        m.name = j.value("name", m.id);
        m.version = j.value("version", "1.0.0");
        m.author = j.value("author", "");
        m.description = j.value("description", "");
        m.game = j.value("game", "");
        m.regions.clear();
        if (j.contains("regions"))
            for (const auto& r : j.at("regions")) m.regions.push_back(r.get<std::string>());
        m.filesFormat = j.value("files_format", 0);
        m.priority = j.value("priority", 0);
        m.dependencies.clear();
        if (j.contains("dependencies"))
            for (const auto& d : j.at("dependencies"))
                m.dependencies.push_back({d.value("id", ""), d.value("version", "")});
        m.worlds = j.contains("worlds") ? j.at("worlds") : nlohmann::json::array();
        m.extra = nlohmann::json::object();
        static const std::set<std::string> known{"format", "format_version", "id", "name", "version", "author", "description",
                                                 "game", "regions", "files_format", "priority", "dependencies", "worlds"};
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!known.count(it.key())) m.extra[it.key()] = it.value();
        return true;
    } catch (const std::exception& e) {
        error = std::string("bad manifest: ") + e.what();
        return false;
    }
}

inline nlohmann::ordered_json manifestToJson(const Manifest& m) {
    nlohmann::ordered_json j;
    j["format"] = kFormat;
    j["format_version"] = kFormatVersion;
    j["id"] = m.id;
    j["name"] = m.name;
    j["version"] = m.version;
    j["author"] = m.author;
    j["description"] = m.description;
    j["game"] = m.game;
    j["regions"] = m.regions;
    j["files_format"] = m.filesFormat;
    j["priority"] = m.priority;
    nlohmann::ordered_json deps = nlohmann::ordered_json::array();
    for (const Dependency& d : m.dependencies) deps.push_back({{"id", d.id}, {"version", d.version}});
    j["dependencies"] = deps;
    j["worlds"] = m.worlds;
    for (auto it = m.extra.begin(); it != m.extra.end(); ++it) j[it.key()] = it.value();
    return j;
}

inline bool readManifest(const std::filesystem::path& dir, Manifest& m, std::string& error) {
    std::ifstream in(dir / kManifestName);
    if (!in) { error = utf8Of(dir) + ": no " + kManifestName; return false; }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        error = utf8Of(dir / kManifestName) + ": " + e.what();
        return false;
    }
    if (!manifestFromJson(j, m, error)) { error = utf8Of(dir / kManifestName) + ": " + error; return false; }
    m.dir = dir;
    return true;
}

inline bool writeManifest(const Manifest& m, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(m.dir, ec);
    std::ofstream out(m.dir / kManifestName, std::ios::binary | std::ios::trunc);
    if (!out) { error = "cannot write " + utf8Of(m.dir / kManifestName); return false; }
    out << manifestToJson(m).dump(1) << '\n';
    return bool(out);
}

// A package's custom tasks (the editor's new jobs: tasks without a save key of the game's own), per world:
//   "custom_tasks": [{"world": "Paris", "tasks": [1001, 1002]}]
// The games keep these tasks' states in a side file next to the save slot ("<slot>.mods.json": {package id: {world:
// {oid: state}}}); the save slot itself is never changed. A side file's entries of a package that is not loaded are
// kept as they are.
struct CustomTasks {
    std::string world;
    std::vector<int> tasks;
};
inline std::vector<CustomTasks> customTasks(const Manifest& m) {
    std::vector<CustomTasks> a;
    const auto it = m.extra.find("custom_tasks");
    if (it == m.extra.end() || !it->is_array()) return a;
    for (const auto& e : *it) {
        if (!e.is_object() || !e.contains("world") || !e.at("world").is_string() || !e.contains("tasks") || !e.at("tasks").is_array()) continue;
        CustomTasks ct;
        ct.world = e.at("world").get<std::string>();
        for (const auto& t : e.at("tasks"))
            if (t.is_number_integer()) ct.tasks.push_back(t.get<int>());
        if (!ct.world.empty() && !ct.tasks.empty()) a.push_back(std::move(ct));
    }
    return a;
}
inline void setCustomTasks(Manifest& m, const std::string& world, const std::vector<int>& tasks) {
    nlohmann::json a = nlohmann::json::array();
    if (const auto it = m.extra.find("custom_tasks"); it != m.extra.end() && it->is_array())
        for (const auto& e : *it)
            if (!(e.is_object() && e.value("world", "") == world)) a.push_back(e);
    if (!tasks.empty()) a.push_back({{"world", world}, {"tasks", tasks}});
    if (a.empty()) m.extra.erase("custom_tasks");
    else m.extra["custom_tasks"] = a;
}

// A package's cutscene rules (the map editor's cutscene editor), per world and script:
//   "cutscenes": [{"world": "Paris", "script": 144037, "once": true, "task": 1001, "states": [1, 4],
//                  "skip": "auto" | "always" | "never", "skip_to_label": -1}]
// once: the script starts only the first time (kept per save slot, "<slot>.mods.json"); task / states: it starts only
// while the task is in one of the states; skip: the cutscene skip's choice (auto: the game's own, by what the scene
// has); skip_to_label: a skip stops where the script reaches this label (the rest plays), -1 the scene's end.
struct CutsceneRule {
    std::string world;
    int script = 0;
    bool once = false;
    int task = -1;
    std::vector<int> states;
    std::string skip = "auto";
    int skipToLabel = -1;
    bool empty() const { return !once && task < 0 && skip == "auto" && skipToLabel < 0; }
};
inline std::vector<CutsceneRule> cutsceneRules(const Manifest& m) {
    std::vector<CutsceneRule> a;
    const auto it = m.extra.find("cutscenes");
    if (it == m.extra.end() || !it->is_array()) return a;
    for (const auto& e : *it) {
        if (!e.is_object() || !e.contains("world") || !e.at("world").is_string() || !e.contains("script") || !e.at("script").is_number_integer()) continue;
        CutsceneRule r;
        r.world = e.at("world").get<std::string>();
        r.script = e.at("script").get<int>();
        if (e.contains("once") && e.at("once").is_boolean()) r.once = e.at("once").get<bool>();
        if (e.contains("task") && e.at("task").is_number_integer()) r.task = e.at("task").get<int>();
        if (e.contains("states") && e.at("states").is_array())
            for (const auto& s : e.at("states"))
                if (s.is_number_integer()) r.states.push_back(s.get<int>());
        if (e.contains("skip") && e.at("skip").is_string()) r.skip = e.at("skip").get<std::string>();
        if (e.contains("skip_to_label") && e.at("skip_to_label").is_number_integer()) r.skipToLabel = e.at("skip_to_label").get<int>();
        if (!r.world.empty()) a.push_back(std::move(r));
    }
    return a;
}
inline void setCutsceneRule(Manifest& m, const CutsceneRule& r) {
    nlohmann::json a = nlohmann::json::array();
    if (const auto it = m.extra.find("cutscenes"); it != m.extra.end() && it->is_array())
        for (const auto& e : *it)
            if (!(e.is_object() && e.value("world", "") == r.world && e.value("script", 0) == r.script)) a.push_back(e);
    if (!r.empty()) {
        nlohmann::json e = {{"world", r.world}, {"script", r.script}};
        if (r.once) e["once"] = true;
        if (r.task >= 0) { e["task"] = r.task; e["states"] = r.states; }
        if (r.skip != "auto") e["skip"] = r.skip;
        if (r.skipToLabel >= 0) e["skip_to_label"] = r.skipToLabel;
        a.push_back(e);
    }
    if (a.empty()) m.extra.erase("cutscenes");
    else m.extra["cutscenes"] = a;
}

// Why the package does not fit this installation ("" when it does).
inline std::string mismatch(const Manifest& m, const std::string& game, const std::string& region, int filesFormat) {
    if (!m.game.empty() && m.game != game) return "made for " + m.game + ", not " + game;
    if (!m.regions.empty() && !region.empty() && std::find(m.regions.begin(), m.regions.end(), region) == m.regions.end())
        return "made for another region than " + region;
    if (m.filesFormat && filesFormat && m.filesFormat != filesFormat)
        return "made for installation format " + std::to_string(m.filesFormat) + ", this one is " + std::to_string(filesFormat);
    return {};
}

struct Resolved {
    std::vector<Manifest> packages;      // in load order
    std::vector<std::string> messages;   // the skipped packages and why
};

// The packages in <packagesDir> (each a folder with mod.json) that fit, in load order.
inline Resolved discover(const std::filesystem::path& packagesDir, const std::string& game, const std::string& region, int filesFormat) {
    Resolved r;
    std::error_code ec;
    if (packagesDir.empty() || !std::filesystem::is_directory(packagesDir, ec)) return r;
    std::map<std::string, Manifest> byId;
    for (const auto& e : std::filesystem::directory_iterator(packagesDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string folder = utf8Of(e.path().filename());
        if (folder.size() > 9 && folder.compare(folder.size() - 9, 9, ".disabled") == 0) continue;
        if (folder.empty() || folder[0] == '.') continue;
        Manifest m;
        std::string error;
        if (!readManifest(e.path(), m, error)) { r.messages.push_back(error); continue; }
        if (const std::string why = mismatch(m, game, region, filesFormat); !why.empty()) {
            r.messages.push_back(m.id + ": skipped, " + why);
            continue;
        }
        if (byId.count(m.id)) { r.messages.push_back(m.id + ": skipped, a second package with this id (" + folder + ")"); continue; }
        byId.emplace(m.id, std::move(m));
    }
    // missing / too old dependencies drop a package (and, in turn, the ones needing it)
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = byId.begin(); it != byId.end();) {
            std::string why;
            for (const Dependency& d : it->second.dependencies) {
                const auto dep = byId.find(d.id);
                if (dep == byId.end()) why = "needs " + d.id + (d.version.empty() ? "" : " " + d.version);
                else if (!versionSatisfies(dep->second.version, d.version)) why = "needs " + d.id + " " + d.version + ", have " + dep->second.version;
                if (!why.empty()) break;
            }
            if (!why.empty()) {
                r.messages.push_back(it->first + ": skipped, " + why);
                it = byId.erase(it);
                changed = true;
            } else ++it;
        }
    }
    // dependencies first, then priority, then id (a dependency cycle: the rest in that order)
    std::set<std::string> done;
    while (done.size() < byId.size()) {
        const Manifest* next = nullptr;
        for (const auto& [id, m] : byId) {
            if (done.count(id)) continue;
            const bool ready = std::all_of(m.dependencies.begin(), m.dependencies.end(), [&](const Dependency& d) { return done.count(d.id) > 0; });
            if (!ready) continue;
            if (!next || m.priority < next->priority) next = &m;
        }
        if (!next)
            for (const auto& [id, m] : byId)
                if (!done.count(id) && (!next || m.priority < next->priority)) next = &m;
        done.insert(next->id);
        r.packages.push_back(*next);
    }
    return r;
}

}  // namespace cane::mods
