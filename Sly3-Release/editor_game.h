#pragma once
// The map editor's game (editor/game/editor_impl.h includes this from the game's folder): Sly 3. The shared shell
// (editor/game/editor_shell.inc) gives Sly 3 the 3D view: the world loaded by Sly 3's loader with the recorder
// (editrec.h, editrec_check.cpp), drawn by its renderer; picking, the scene tree, the inspector, move / rotate / scale
// (own matrices and proxy-placed instances), undo / redo, save as a package ("game": "sly3"), play test. The shared
// feature modules (editor/game/editor_*.inc) run on Sly 3's own numbers: its classes (EDCID) and options (EDEOPID: the
// copied code's Sly 2 numbers mapped to Sly 3's, game/gen/eopidmap.inc; -1 for an option Sly 3 does not have), its
// option names (editor_data3.cpp), its record headers and group table. The features below at 0 are Sly 2's only (their
// formats differ: Sly 3's scripts, dialogue table, glob and shader formats).
#include "game/sly3.h"

#define CANE_EDITOR_3D SLY3_EDITOR
#define EDGAME_SLY3 1
#define EDGAME_SLY2 0
#define EDGAME_NAME "Sly 3"
#define EDGAME_EXE "Sly3-Release"
#define EDGAME_RENDERER_ENV "SLY2N_RENDERER"
#define EDGAME_DEFAULT_WORLD 3      // v_ext, Venice's hub
#define EDCID(cid2) S3CID(cid2)     // the shared code's Sly 2 class numbers as Sly 3's
#define EDEOPID(eopid2) (::sly3::Eopid3FromEopid2(eopid2))   // (-1: none in Sly 3)
#define EDFEAT_OPTIONS 1
#define EDFEAT_CURVES 1
#define EDFEAT_STRUCT 1
#define EDFEAT_MISSIONS 1
#define EDFEAT_CUSTOMTASKS 1
#define EDFEAT_SCRIPTS 1
#define EDFEAT_DIALOGUE 1
#define EDFEAT_MODELS 0
