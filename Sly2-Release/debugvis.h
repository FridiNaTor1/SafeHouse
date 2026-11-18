#pragma once

// PC debug overlays (the debug menu's Overlays page, debugmenu.h; Debug mode only): what the game keeps hidden, drawn
// over the picture, each its own toggle, all off by default and remembered in the user folder's "Debug Overlays.ini":
//   Hidden Objects   models not drawn (removed, out of the world, faded out) and not updating (inactive), as tinted boxes
//   Guards           their patrols (paths, go-to points, looks), homes, view volumes coloured by what they see, noises
//   Trigger Zones    the shapes / volumes by type (trigger, exit, volume, water, sensor / laser, warp point)
//   Traversal        ledges, pipes, rails, ropes, jump / walk targets and the other traversal objects
//   Collision        the collision entries' edges near the camera (wireframe)
//   Camera Zones     the camera zones, camera targets and the game camera's focus
//   Labels           oid, class and readable name (the map editor's types, editor/game/editor_objtypes.h) within a
//                    distance, or only the object under the cursor / crosshair
//   Stats            what is loaded (objects by class, updating, meshes, textures, sounds / voices, memory), draw calls,
//                    the frame's time (update / draw / swap)
//   Mission          world, job, task states, running scripts (SCPT), the splice events just fired (the script namer's
//                    event names, splicenames.h) and a log of recent events (task states, jobs, guards seeing, worlds)
// Drawing only: nothing here writes the game's state (positions, curves and lists are read, never built or cached in
// the game's own structures), so the simulation is the same with every overlay off or on. The world overlays use the
// drawn camera (2D lines through its projection: OpenGL and Vulkan alike, any aspect); the panels keep inside the HUD
// Placement box. VR: the panels only (the world overlays would not sit on the eyes' pictures).
//
// Test aid: SLY2N_DEBUGVIS=all (or a comma list: hidden,guards,triggers,traversal,collision,cameras,labels,stats,mission)
// turns overlays on for the run, Debug mode or not, without touching the .ini.

#include <cstdint>
#include <string>

struct CAMERA;
struct LO;
struct SW;

enum DEBUGVIS
{
    DV_Hidden, DV_Guards, DV_Triggers, DV_Traversal, DV_Collision, DV_Cameras, DV_Labels, DV_Stats, DV_Mission, DV_Max
};

// the toggle (as the menu shows it) / whether it draws now (the toggle and Debug mode, or the test aid)
bool FDebugVisWant(DEBUGVIS k);
bool FDebugVis(DEBUGVIS k);
bool FDebugVisAny();
void SetDebugVis(DEBUGVIS k, bool f);   // (saved)
void SetAllDebugVis(bool f);
const char* SzDebugVis(DEBUGVIS k);
// the labels' distance (stops; the last: no limit) and their cursor / crosshair mode
std::string StrDebugVisLabelRange();
void StepDebugVisLabelRange(int dir);
bool FDebugVisLabelsCursor();
void SetDebugVisLabelsCursor(bool f);
// the collision wireframe's distance
std::string StrDebugVisCollisionRange();
void StepDebugVisCollisionRange(int dir);

// game.cpp: the camera the frame was drawn with (fWorld false: VR's eyes / no flat picture: the panels only), and the
// last frame's times (ms)
void NoteDebugVisCamera(const CAMERA& cam, bool fWorld);
void NoteDebugVisFrameTimes(double msUpdate, double msDraw, double msSwap);
// debugmenu.cpp UpdateDebugTools, each step after the world's update: the event log's polling
void UpdateDebugVis(SW* psw);
// debugmenu.cpp DrawDebugOverlay, between g_hud.Begin and Flush, under the menu
void DrawDebugVis(SW* psw);

// update.cpp HandleLoSpliceEvent: a bound splice event about to run (null unless the Mission overlay is on)
extern void (*g_pfnDebugVisSpliceEvent)(LO* plo, uint32_t symid, bool fDeferred);
