#pragma once
// Mod packages' map states and new worlds (every platform: the games honour them; the editor writes them). Kept in the
// package's mod.json beside the other keys (mod_package.h Manifest::extra), documented in docs/MODDING.md "Map
// states" and "New levels":
//
//   "map_states": [{"world": "Paris", "id": "fountain-fixed", "name": "Fountain fixed", "description": "",
//                   "when": [{"job": 1889, "done": true}, {"task": 1761, "states": [2, 3]},
//                            {"progress": "flags.x", "equals": 1}],          (all of them hold; empty: always)
//                   "add": [124011], "remove": [123866],
//                   "sm": [{"object": 33735, "sm": -1, "state": 1556}]}]
//       add: objects that exist only while the state holds (removed otherwise); remove: objects taken out while it holds
//       (left to the game otherwise); sm: an object's state machine sent to a state while it holds.
//
//   "new_worlds": [{"id": "test-floor", "name": "Test Floor", "index": "Worlds/Mods/test-floor/Test Floor.world.json",
//                   "template": "Prison Vault", "episode": 1, "kind": 0, "spawn": [0, 0, 100, 0]}]
//       a world of the package: its index (the parts' format, worldparts.h) in files/; parts marked "template": true
//       are the template world's (an installed world, by its name) parts of the same kind and group. The game gives it
//       a world record after the disc's (44, 45, ... in package load order: --world N); "spawn": where the player
//       starts (x, y, z, heading in radians) when the world has no checkpoint for the entry's point.
//
//   "entries": [{"world": "Paris", "at": [x, y, z], "radius": 150, "to": "Test Floor", "point": -1},
//               {"world": "Paris", "exit": 1739, "to": "Test Floor"}]
//       a way in: entering the sphere (at, radius) in that world, or an existing exit object (its oid: e.g. a door the
//       editor copied), takes the player to the world "to" (a new world's name, or an installed world's) at its
//       checkpoint "point" (-1: the world's default, 393).
// Header-only (nlohmann::json), shared by the games, the launcher and the editor.

#include "mod_package.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace cane::mods {

// A condition of a map state ("when"): a task in one of the states (0 locked, 1 available, 2 done, 3 done and
// closed), a job done / not done (its last task), or a progress value (a dotted save path) equal to a number.
struct StateCond {
    enum Kind { Task, Job, Progress } kind = Task;
    int oid = -1;
    std::vector<int> states;   // (Task) any of
    bool done = true;          // (Job)
    std::string path;          // (Progress) "a.b.c"
    int value = 0;
};

struct StateSm {
    int object = -1, sm = -1, state = -1;   // the object, its state machine (-1: any), the state's oid
};

struct MapState {
    std::string world, id, name, description;
    std::vector<StateCond> when;
    std::vector<int> add, remove;
    std::vector<StateSm> sm;
};

struct NewWorld {
    std::string id, name, index, templateWorld;
    int episode = 1, kind = 0;
    bool hasSpawn = false;
    float spawn[4] = {0, 0, 0, 0};
};

struct Entry {
    std::string world, to;
    int exit = -1;             // an exit object's oid, or -1: the sphere
    float at[3] = {0, 0, 0};
    float radius = 150.0f;
    int point = -1;
};

inline nlohmann::json toJson(const StateCond& c) {
    switch (c.kind) {
    case StateCond::Task: return {{"task", c.oid}, {"states", c.states}};
    case StateCond::Job: return {{"job", c.oid}, {"done", c.done}};
    default: return {{"progress", c.path}, {"equals", c.value}};
    }
}
inline bool fromJson(const nlohmann::json& j, StateCond& c, std::string& error) {
    if (!j.is_object()) { error = "a condition is not an object"; return false; }
    if (j.contains("task") && j["task"].is_number_integer()) {
        c.kind = StateCond::Task;
        c.oid = j["task"].get<int>();
        c.states.clear();
        if (j.contains("states") && j["states"].is_array())
            for (const auto& s : j["states"])
                if (s.is_number_integer() && s.get<int>() >= 0 && s.get<int>() <= 3) c.states.push_back(s.get<int>());
        if (c.states.empty()) c.states = {2, 3};
        return true;
    }
    if (j.contains("job") && j["job"].is_number_integer()) {
        c.kind = StateCond::Job;
        c.oid = j["job"].get<int>();
        c.done = j.value("done", true);
        return true;
    }
    if (j.contains("progress") && j["progress"].is_string()) {
        c.kind = StateCond::Progress;
        c.path = j["progress"].get<std::string>();
        c.value = j.value("equals", 1);
        return !c.path.empty() || (error = "an empty progress path", false);
    }
    error = "a condition needs \"task\", \"job\" or \"progress\"";
    return false;
}

inline nlohmann::json toJson(const MapState& s) {
    nlohmann::json j = {{"world", s.world}, {"id", s.id}, {"name", s.name}};
    if (!s.description.empty()) j["description"] = s.description;
    nlohmann::json w = nlohmann::json::array();
    for (const StateCond& c : s.when) w.push_back(toJson(c));
    j["when"] = w;
    j["add"] = s.add;
    j["remove"] = s.remove;
    if (!s.sm.empty()) {
        nlohmann::json a = nlohmann::json::array();
        for (const StateSm& m : s.sm) a.push_back({{"object", m.object}, {"sm", m.sm}, {"state", m.state}});
        j["sm"] = a;
    }
    return j;
}
inline bool fromJson(const nlohmann::json& j, MapState& s, std::string& error) {
    if (!j.is_object()) { error = "a map state is not an object"; return false; }
    s = MapState();
    s.world = j.value("world", "");
    s.id = j.value("id", "");
    s.name = j.value("name", s.id);
    s.description = j.value("description", "");
    if (s.world.empty() || s.id.empty()) { error = "a map state needs \"world\" and \"id\""; return false; }
    if (j.contains("when")) {
        const nlohmann::json& w = j["when"];
        const nlohmann::json a = w.is_array() ? w : nlohmann::json::array({w});
        for (const auto& e : a) {
            StateCond c;
            if (!fromJson(e, c, error)) { error = s.id + ": " + error; return false; }
            s.when.push_back(c);
        }
    }
    auto oids = [&](const char* key, std::vector<int>& a) {
        if (j.contains(key) && j[key].is_array())
            for (const auto& o : j[key])
                if (o.is_number_integer()) a.push_back(o.get<int>());
    };
    oids("add", s.add);
    oids("remove", s.remove);
    if (j.contains("sm") && j["sm"].is_array())
        for (const auto& e : j["sm"])
            if (e.is_object() && e.contains("object") && e.contains("state"))
                s.sm.push_back({e.value("object", -1), e.value("sm", -1), e.value("state", -1)});
    return true;
}

inline nlohmann::json toJson(const NewWorld& w) {
    nlohmann::json j = {{"id", w.id}, {"name", w.name}, {"index", w.index}, {"template", w.templateWorld},
                        {"episode", w.episode}, {"kind", w.kind}};
    if (w.hasSpawn) j["spawn"] = {w.spawn[0], w.spawn[1], w.spawn[2], w.spawn[3]};
    return j;
}
inline bool fromJson(const nlohmann::json& j, NewWorld& w, std::string& error) {
    if (!j.is_object()) { error = "a new world is not an object"; return false; }
    w = NewWorld();
    w.id = j.value("id", "");
    w.name = j.value("name", w.id);
    w.index = j.value("index", "");
    w.templateWorld = j.value("template", "");
    w.episode = j.value("episode", 1);
    w.kind = j.value("kind", 0);
    if (j.contains("spawn") && j["spawn"].is_array() && j["spawn"].size() >= 3) {
        w.hasSpawn = true;
        for (std::size_t i = 0; i < 4 && i < j["spawn"].size(); ++i)
            if (j["spawn"][i].is_number()) w.spawn[i] = j["spawn"][i].get<float>();
    }
    if (w.id.empty() || w.name.empty() || w.index.empty()) { error = "a new world needs \"id\", \"name\" and \"index\""; return false; }
    if (w.index.find("..") != std::string::npos) { error = w.id + ": an index outside the package"; return false; }
    return true;
}

inline nlohmann::json toJson(const Entry& e) {
    nlohmann::json j = {{"world", e.world}, {"to", e.to}};
    if (e.exit >= 0) j["exit"] = e.exit;
    else {
        j["at"] = {e.at[0], e.at[1], e.at[2]};
        j["radius"] = e.radius;
    }
    j["point"] = e.point;
    return j;
}
inline bool fromJson(const nlohmann::json& j, Entry& e, std::string& error) {
    if (!j.is_object()) { error = "an entry is not an object"; return false; }
    e = Entry();
    e.world = j.value("world", "");
    e.to = j.value("to", "");
    e.exit = j.value("exit", -1);
    e.radius = j.value("radius", 150.0f);
    e.point = j.value("point", -1);
    if (j.contains("at") && j["at"].is_array() && j["at"].size() >= 3)
        for (std::size_t i = 0; i < 3; ++i) e.at[i] = j["at"][i].get<float>();
    else if (e.exit < 0) { error = "an entry needs \"exit\" or \"at\""; return false; }
    if (e.world.empty() || e.to.empty()) { error = "an entry needs \"world\" and \"to\""; return false; }
    return true;
}

// The lists of a manifest (invalid entries skipped; perrors: why)
template <class T>
inline std::vector<T> listOf(const Manifest& m, const char* key, std::vector<std::string>* perrors = nullptr) {
    std::vector<T> a;
    const auto it = m.extra.find(key);
    if (it == m.extra.end() || !it->is_array()) return a;
    for (const auto& e : *it) {
        T t;
        std::string error;
        if (fromJson(e, t, error)) a.push_back(std::move(t));
        else if (perrors) perrors->push_back(m.id + ": " + key + ": " + error);
    }
    return a;
}
inline std::vector<MapState> mapStates(const Manifest& m, std::vector<std::string>* perrors = nullptr) { return listOf<MapState>(m, "map_states", perrors); }
inline std::vector<NewWorld> newWorlds(const Manifest& m, std::vector<std::string>* perrors = nullptr) { return listOf<NewWorld>(m, "new_worlds", perrors); }
inline std::vector<Entry> entries(const Manifest& m, std::vector<std::string>* perrors = nullptr) { return listOf<Entry>(m, "entries", perrors); }

template <class T>
inline void setListOf(Manifest& m, const char* key, const std::vector<T>& a) {
    if (a.empty()) { m.extra.erase(key); return; }
    nlohmann::json j = nlohmann::json::array();
    for (const T& t : a) j.push_back(toJson(t));
    m.extra[key] = j;
}
inline void setMapStates(Manifest& m, const std::vector<MapState>& a) { setListOf(m, "map_states", a); }
inline void setNewWorlds(Manifest& m, const std::vector<NewWorld>& a) { setListOf(m, "new_worlds", a); }
inline void setEntries(Manifest& m, const std::vector<Entry>& a) { setListOf(m, "entries", a); }

// One map state added or replaced (by world + id)
inline void putMapState(Manifest& m, const MapState& s) {
    std::vector<MapState> a = mapStates(m);
    bool found = false;
    for (MapState& x : a)
        if (x.world == s.world && x.id == s.id) { x = s; found = true; }
    if (!found) a.push_back(s);
    setMapStates(m, a);
}
inline bool removeMapState(Manifest& m, const std::string& world, const std::string& id) {
    std::vector<MapState> a = mapStates(m);
    const std::size_t n = a.size();
    a.erase(std::remove_if(a.begin(), a.end(), [&](const MapState& x) { return x.world == world && x.id == id; }), a.end());
    setMapStates(m, a);
    return a.size() != n;
}

// A map state's id from its name ("Fountain fixed" -> "fountain-fixed")
inline std::string stateIdFromName(const std::string& name) { return idFromName(name); }

}  // namespace cane::mods
