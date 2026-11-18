#pragma once
// The map editor's new worlds and map states (desktop; the games read the result on every platform: platform/
// mod_worlds.h, docs/MODDING.md "New levels" / "Map states", notes/EDITOR_PLAN.md "Map states and new worlds").
//
// A new world is a world file of its own in the project (a mod package): its index at
// "Worlds/Mods/<id>/<name>.world.json" refers to an installed template world's parts ("template": true parts, read
// from the installation: nothing copied), except the parts the new world owns (files beside its index). Made empty,
// the template's level groups (its geometry, props, guards, jobs, collision) are left out: what stays are the
// characters, effects and HUD groups, so the player starts in an empty world; the main level group keeps its textures
// and shaders (an own copy of its Graphics part: brushes and imported models take their materials from there and add
// their textures to it) and an own Objects part the editor adds objects to, which starts with the template's lights
// and skies (keepLighting: the new level is lit as the template is). The game gives the world a record
// after the disc's (44, 45, ... in the packages' order; Sly2-Release --world N / --editor N).

#include "project.h"
#include "world.h"

#include "../../platform/mod_worlds.h"

#include <string>
#include <vector>

namespace cane::editor {

struct NewWorldSpec {
    std::string name;            // "Test Floor" (its id from it)
    int templateRecord = -1;     // an installed world (its record)
    bool emptyLevel = true;      // the template's level groups left out (false: the whole template world, a copy to change)
    bool keepLighting = true;    // (empty) the template's lights and skies kept (its level groups' light / sky records)
    int episode = -1;            // -1: the template's
    bool hasSpawn = false;
    float spawn[4] = {0, 0, 0, 0};   // where the player starts (x, y, z, heading)
};

// The new world added to the project (its index and own parts as project parts, mod.json "new_worlds"); pout: its
// entry as written. Save the project to write it.
bool addNewWorld(Project& proj, const Installation& inst, const NewWorldSpec& spec, cane::mods::NewWorld* pout, std::string& error);

// A package's new worlds as world entries for the editor's world list (records firstRecord, firstRecord + 1, ... in the
// manifest's order, as the game numbers them when no other package adds worlds before it)
std::vector<WorldEntry> newWorldEntries(const cane::mods::Manifest& m, int firstRecord);

// One part of a new world made its own (copied from the template into the world's folder, the index changed to name
// it): the editor can then change it. kind: "objects", "graphics", "scripts", "collision", "text"...; group -1: a world part.
bool ownTemplatePart(Project& proj, const Installation& inst, const std::string& relIndex, const std::string& kind, int group, std::string& relPart,
                     std::string& error);

// Ways in (mod.json "entries") and map states (mod.json "map_states"), on the project's manifest (each an undo-less
// manifest change: save the project to write it)
void addEntry(Project& proj, const cane::mods::Entry& e);
void putMapState(Project& proj, const cane::mods::MapState& s);
bool removeMapState(Project& proj, const std::string& world, const std::string& id);
std::vector<cane::mods::MapState> mapStates(const Project& proj, const std::string& world);

}  // namespace cane::editor
