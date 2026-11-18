#pragma once

// The map editor's new worlds, Sly 2's side (editor/core/newworld.h; desktop builds with the editor): the command that
// makes one in a project, and the editor's world list taking a project's new worlds.
//   Sly2-Release --data-dir D --new-world PROJECT --nw-name "Test Floor" --nw-template 16 [--nw-keep-level]
//       [--nw-spawn X,Y,Z[,H]] [--nw-entry WORLD:X,Y,Z[,R]] [--nw-return X,Y,Z[,R][:WORLD[:POINT]]]
// (PROJECT: a project / mod package folder, made when missing.) The entry: a sphere in WORLD (an installed world's
// name) that takes the player to the new world; the return: a sphere in the new world back to WORLD (default the
// entry's) at its checkpoint POINT.

#include <string>

namespace cane::editor { struct Installation; }

struct NEWWORLDOPTS
{
    std::string project, name, entry, ret;
    int templateRecord = -1;
    bool keepLevel = false;
    std::string spawn;
};
extern NEWWORLDOPTS g_newworldopts;

int NewWorldCommand();
// (the editor, editor_shell.inc Run) a project's new worlds into the installation's list (and the game's records), so
// --editor N opens them; dirProject: the project's folder
void RegisterProjectWorlds(cane::editor::Installation& inst, const std::string& dirProject);
