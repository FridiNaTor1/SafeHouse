// See newworld.h.
#include "newworld.h"

#include <algorithm>
#include <fstream>
#include <set>

namespace cane::editor {

namespace {

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

bool readIndex(const Installation& inst, const WorldEntry& e, nlohmann::json& j, std::string& error) {
    std::ifstream in((e.indexBase.empty() ? inst.dir : e.indexBase) / pathOf(e.index));
    if (!in) { error = "cannot read " + e.index; return false; }
    try {
        in >> j;
    } catch (const std::exception& ex) {
        error = e.index + ": " + ex.what();
        return false;
    }
    return true;
}

std::string dirOf(const std::string& rel) { return rel.substr(0, rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/')); }

}  // namespace

bool addNewWorld(Project& proj, const Installation& inst, const NewWorldSpec& spec, cane::mods::NewWorld* pout, std::string& error) {
    if (inst.sly3()) { error = "new worlds are Sly 2's for now"; return false; }
    const WorldEntry* pt = inst.world(spec.templateRecord);
    if (!pt) { error = "no installed world " + std::to_string(spec.templateRecord) + " to start from"; return false; }
    if (spec.name.empty()) { error = "a new world needs a name"; return false; }
    for (const WorldEntry& w : inst.worlds)
        if (w.name == spec.name) { error = "a world named " + spec.name + " is installed already"; return false; }
    for (const cane::mods::NewWorld& w : cane::mods::newWorlds(proj.manifest))
        if (w.name == spec.name) { error = "the project has a world named " + spec.name + " already"; return false; }
    nlohmann::json tpl;
    if (!readIndex(inst, *pt, tpl, error)) return false;
    const std::string id = cane::mods::idFromName(spec.name);
    const std::string relDir = "Worlds/Mods/" + id;
    const std::string relIndex = relDir + "/" + spec.name + ".world.json";
    const std::string relDirTpl = dirOf(pt->index);
    try {
        // the level groups (the template's own: its code's groups) and the main one (the largest Graphics part)
        std::set<int> setLevel;
        for (const auto& g : tpl.at("groups"))
            if (g.value("level", false)) setLevel.insert(g.value("index", -1));
        int groupMain = -1;
        std::size_t cbMain = 0;
        for (const auto& p : tpl.at("parts"))
            if (p.value("kind", "") == "graphics" && setLevel.count(p.value("group", -1)) && p.contains("file") && p.value("size", std::size_t(0)) >= cbMain) {
                cbMain = p.value("size", std::size_t(0));
                groupMain = p.value("group", -1);
            }
        nlohmann::json index = nlohmann::json::object();
        index["format"] = tpl.value("format", "sly2-world-parts");
        index["version"] = tpl.value("version", 1);
        index["size"] = tpl.value("size", 0);
        index["template"] = pt->name;
        index["world"] = spec.name;
        index["code"] = id;
        index["groups"] = tpl.at("groups");
        nlohmann::json parts = nlohmann::json::array();
        std::vector<std::pair<std::string, std::vector<uint8_t>>> aown;   // (own parts: rel in the project, bytes)
        for (const auto& p : tpl.at("parts")) {
            const std::string kind = p.value("kind", "");
            const int g = p.value("group", -1);
            nlohmann::json n = {{"kind", kind}};
            if (g >= 0) n["group"] = g;
            if (!spec.emptyLevel || g < 0 || !setLevel.count(g)) {
                n["template"] = true;
                parts.push_back(n);
                continue;
            }
            // a level group left out: its blocks empty as the loader reads them (bulk size 0 with its key, no textures,
            // no collision mesh, no objects, no scripts); the main one keeps its textures (an own copy) and gets an own
            // (empty) Objects part
            if (kind == "bulk" && g == groupMain) {
                n["template"] = true;   // (the kept textures' bulk block: its size)
            } else if (kind == "bulk") {
                const std::string hex = p.value("hex", std::string("0000000000000000"));
                n["size"] = 8;
                n["hex"] = "00000000" + hex.substr(std::min<std::size_t>(8, hex.size()));
            } else if (kind == "scripts") {
                n["size"] = 8;
                n["hex"] = "0000000000000000";
            } else if (kind == "collision") {
                n["size"] = 4;
                n["hex"] = "00000000";
            } else if (kind == "graphics" && g == groupMain && p.contains("file")) {
                std::vector<uint8_t> ab;
                const std::string relTpl = relDirTpl + "/" + p["file"].get<std::string>();
                if (!proj.readPart(relTpl, ab)) { error = "cannot read " + relTpl; return false; }
                n = p;
                aown.emplace_back(relDir + "/" + p["file"].get<std::string>(), std::move(ab));
            } else if (kind == "graphics") {
                n["size"] = 8;
                n["hex"] = "0000000000000000";
                n["aligns"] = nlohmann::json::array({nlohmann::json::array({0, 0})});
            } else if (kind == "objects" && g == groupMain && p.contains("file")) {
                n["size"] = 2;
                n["file"] = p["file"];
                n["aligns"] = nlohmann::json::array({nlohmann::json::array({0, 0})});
                n["objects"] = nlohmann::json::array();
                aown.emplace_back(relDir + "/" + p["file"].get<std::string>(), std::vector<uint8_t>{0, 0});
            } else if (kind == "objects") {
                n["size"] = 2;
                n["hex"] = "0000";
                n["aligns"] = nlohmann::json::array({nlohmann::json::array({0, 0})});
            } else {
                n["template"] = true;
            }
            parts.push_back(n);
        }
        index["parts"] = parts;
        proj.begin("New world " + spec.name, true);
        proj.setPart(relIndex, bytesOf(index.dump(1) + "\n"));
        for (auto& [rel, ab] : aown) proj.setPart(rel, std::move(ab));
        proj.end();
    } catch (const std::exception& ex) {
        error = pt->index + ": " + ex.what();
        return false;
    }
    cane::mods::NewWorld w;
    w.id = id;
    w.name = spec.name;
    w.index = relIndex;
    w.templateWorld = pt->name;
    w.episode = spec.episode >= 0 ? spec.episode : pt->episode;
    w.kind = 0;
    w.hasSpawn = spec.hasSpawn;
    std::copy(spec.spawn, spec.spawn + 4, w.spawn);
    std::vector<cane::mods::NewWorld> a = cane::mods::newWorlds(proj.manifest);
    a.push_back(w);
    cane::mods::setNewWorlds(proj.manifest, a);
    if (pout) *pout = w;
    return true;
}

std::vector<WorldEntry> newWorldEntries(const cane::mods::Manifest& m, int firstRecord) {
    std::vector<WorldEntry> a;
    for (const cane::mods::NewWorld& w : cane::mods::newWorlds(m)) {
        WorldEntry e;
        e.record = firstRecord + int(a.size());
        e.code = w.id;
        e.name = w.name;
        e.episode = w.episode;
        e.kind = w.kind;
        e.index = w.index;
        e.indexBase = m.filesDir();
        e.templateName = w.templateWorld;
        a.push_back(e);
    }
    return a;
}

bool ownTemplatePart(Project& proj, const Installation& inst, const std::string& relIndex, const std::string& kind, int group, std::string& relPart,
                     std::string& error) {
    std::vector<uint8_t> abIndex;
    if (!proj.readPart(relIndex, abIndex)) { error = "cannot read " + relIndex; return false; }
    try {
        nlohmann::json index = nlohmann::json::parse(abIndex.begin(), abIndex.end());
        const std::string name = index.value("template", std::string());
        const WorldEntry* pt = nullptr;
        for (const WorldEntry& w : inst.worlds)
            if (w.name == name) pt = &w;
        if (!pt) { error = "the template world " + name + " is not installed"; return false; }
        nlohmann::json tpl;
        if (!readIndex(inst, *pt, tpl, error)) return false;
        for (auto& p : index.at("parts")) {
            if (p.value("kind", "") != kind || p.value("group", -1) != group) continue;
            if (!p.value("template", false)) {
                if (!p.contains("file")) { error = "that part is the world's own (inline)"; return false; }
                relPart = dirOf(relIndex) + "/" + p["file"].get<std::string>();
                return true;   // (already its own)
            }
            for (const auto& q : tpl.at("parts")) {
                if (q.value("kind", "") != kind || q.value("group", -1) != group) continue;
                nlohmann::json n = q;
                if (q.contains("file")) {
                    std::vector<uint8_t> ab;
                    const std::string relTpl = dirOf(pt->index) + "/" + q["file"].get<std::string>();
                    if (!proj.readPart(relTpl, ab)) { error = "cannot read " + relTpl; return false; }
                    relPart = dirOf(relIndex) + "/" + q["file"].get<std::string>();
                    proj.begin("Own " + kind + " part of group " + std::to_string(group), true);
                    proj.setPart(relPart, std::move(ab));
                } else {
                    relPart.clear();
                    proj.begin("Own " + kind + " part of group " + std::to_string(group), true);
                }
                p = n;
                const std::string dump = index.dump(1) + "\n";
                proj.setPart(relIndex, std::vector<uint8_t>(dump.begin(), dump.end()));
                proj.end();
                return true;
            }
            error = "the template has no such part";
            return false;
        }
        error = "the world has no " + kind + " part of group " + std::to_string(group);
        return false;
    } catch (const std::exception& ex) {
        error = relIndex + ": " + ex.what();
        return false;
    }
}

void addEntry(Project& proj, const cane::mods::Entry& e) {
    std::vector<cane::mods::Entry> a = cane::mods::entries(proj.manifest);
    a.push_back(e);
    cane::mods::setEntries(proj.manifest, a);
}

void putMapState(Project& proj, const cane::mods::MapState& s) { cane::mods::putMapState(proj.manifest, s); }

bool removeMapState(Project& proj, const std::string& world, const std::string& id) { return cane::mods::removeMapState(proj.manifest, world, id); }

std::vector<cane::mods::MapState> mapStates(const Project& proj, const std::string& world) {
    std::vector<cane::mods::MapState> a;
    for (const cane::mods::MapState& s : cane::mods::mapStates(proj.manifest))
        if (world.empty() || s.world == world) a.push_back(s);
    return a;
}

}  // namespace cane::editor
