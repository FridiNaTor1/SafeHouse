#pragma once

// PC addition (docs/MODDING.md "Map states", notes/EDITOR_PLAN.md "Map states and new worlds"): a world's map states.
//
// What decides how a world looks (Sly 2): the world file holds every object of every state; the save's task states
// decide which are in the world. When a world starts, the tasks get their states from the save (task.cpp RestoreTask,
// broadcast 5), and each task's / job's object lists (TASKOBJE: objects added in some states, removed in the others),
// state machine lists (TASKSME: an object's animation state per task state, e.g. a sign fallen) and shown object apply;
// the world's scripts then add / remove / change objects by the tasks' states (.state, .done?, .current?), the world
// record (<world-index>) and progress values. Objects with world filters load only in some world records, option 26
// takes an object out at the start. So a map state is a set of task states (a save's progress), and the states a
// world has are its episode's progress points: before any job, after each job, the episode complete; a job being
// played is a state too (the game started at its first task).
//
// The API (game side; the editor calls the commands in a hidden process, or links these):
//   --list-map-states W [--out FILE]   the world's states (id, name, the trigger in plain words, the task states it
//                                      sets) and what decides them (each task's / job's object and state machine lists,
//                                      the scripts that act on objects under task / job / progress conditions, world
//                                      filters, objects taken out at the start), the packages' custom states; JSON
//   --map-states-survey [--out FILE]   every world: what decides its states (counts per mechanism)
//   --map-state ID[,ID...]             the game runs in that state: "start", "before:<job oid>", "after:<job oid>", "complete",
//                                      "custom:<id>" (a package's state forced on), "save" (the save as it is); the
//                                      task states written before the tasks are restored; the run's saves are not
//                                      written (the user's slots stay as they are)
//   --dump-presence FILE               at the end of the run (--frames N): every object with its oid, class, whether
//                                      it is in the world, its position (JSON): the editor shows a state with it
// Custom states (mod.json "map_states", platform/mod_worlds.h): honoured at the world's start (after the save's task
// states) and whenever a task changes state.

#include "lo.h"

#include <string>
#include <vector>

struct SW;
struct TASK;

struct MAPSTATEINFO
{
    std::string id, name, trigger, kind;   // kind: "progress" (task states), "job" (played from its first task), "custom"
    std::vector<std::pair<OID, int>> atask;  // (progress) the task states it sets
    OID oidJob = -1, oidTaskStart = -1;      // (job) the job and the task the game starts at (--world W:TASK)
    std::string after;                       // (job) the progress state it starts from
    std::string package;                     // (custom)
};

// The states of a loaded world (not started: a fresh SW) and, in alines, what decides them in plain words
bool FMapStatesOfWorld(SW* psw, int iwrec, std::vector<MAPSTATEINFO>& astate, std::vector<std::string>* palines);

// Commands (main.cpp)
int ListMapStatesCommand(const std::string& world, const std::string& out);
int MapStatesSurveyCommand(const std::string& out);
int MapStatesSelfTest();

// Run time
void SetMapStateRequest(const std::string& ids);   // --map-state (also makes the run's saves read only)
void SetPresenceDump(const std::string& path);     // --dump-presence
void ApplyMapStateRequest(SW* psw);                // task.cpp (broadcast 5, before the tasks' states are restored)
void ApplyCustomMapStates(SW* psw);                // task.cpp (broadcast 5, after)
void OnTaskStateMapStates(TASK* ptask);            // task.cpp SetTaskState
void ResetMapStatesLive();                         // transition.cpp UnloadWorld (the world's custom states let go)
void WritePresenceIfRequested(SW* psw);            // game.cpp (the run's end)
