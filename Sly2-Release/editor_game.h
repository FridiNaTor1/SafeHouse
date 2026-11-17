#pragma once
// The map editor's game (editor/game/editor_impl.h includes this from the game's folder): Sly 2. The shared shell
// (editor/game/editor_shell.inc: load, scene, picking, gizmo, transforms, placement, duplicate / delete, undo, save,
// play test, the panels, --editor-do) and the shared feature modules (editor/game/editor_*.inc: options, curves,
// missions, create) ask these; Sly 2's own modules (editor_data.cpp's names, editor_script / model.cpp) are Sly 2's.
#define CANE_EDITOR_3D SLY2_EDITOR
#define EDGAME_SLY2 1
#define EDGAME_SLY3 0
#define EDGAME_NAME "Sly 2"
#define EDGAME_EXE "Sly2-Release"
#define EDGAME_RENDERER_ENV "SLY2N_RENDERER"
#define EDCID(cid2) (cid2)          // a class id as Sly 2 numbers it (the shared code's numbers are Sly 2's)
#define EDEOPID(eopid2) (eopid2)    // an option (EOPID) as Sly 2 numbers it
#define EDGAME_DEFAULT_WORLD 2      // Paris
// the features this game's editor has (each a shared or game module)
#define EDFEAT_OPTIONS 1            // options by name, live edits, list options, the checks before a save
#define EDFEAT_CURVES 1             // curves: points, whole curve, snap, numeric
#define EDFEAT_STRUCT 1             // duplicate / delete, nested records, create palette, copies across groups, make unique
#define EDFEAT_MISSIONS 1           // jobs / tasks / links, a new job from an existing one
#define EDFEAT_CUSTOMTASKS 1        // mod.json custom_tasks (the game keeps their progress beside the save slot)
#define EDFEAT_SCRIPTS 1            // splice scripts: edit, check, compile back
#define EDFEAT_DIALOGUE 1           // dialogue lines and text
#define EDFEAT_MODELS 1             // GLB / FBX import, GLB export, textures, materials
