#pragma once
// PC addition: the map editor's 3D view (Sly2-Release / Sly3-Release --editor W; desktop builds with SLY2_EDITOR /
// SLY3_EDITOR only, started from the launcher's Editor page; shared by both games: editor/game/). The world is loaded by the game's own loader (no game step runs) and drawn by the game's
// renderer; edits go to a project (editor/core/project.h), a mod package. notes/EDITOR_2026-11-12.md.

#include <string>

struct EDITOROPTS
{
    int record = -1;             // the world record (-1: the game's first hub, Sly 2's Paris 2 / Sly 3's Venice 3)
    std::string world;           // --editor NAME: a world by its name (a project's new world: its record depends on the load)
    std::string project;         // the project folder ("": <user dir>/Editor/<game>/<world>)
    std::string script;          // --editor-do "cmd;cmd;...": automated steps (tools/editorshots.sh)
    bool fHidden = false;        // --editor-hidden: no window shown (automated runs)
    int width = 1600, height = 900;
    std::string language;        // --editor-language CODE (the launcher's; "": SAFEHOUSE_LANGUAGE, else English)
};

// The editor (0: closed normally). Without the game's editor: a message and 2.
int EditorMain(const EDITOROPTS& eo);
