#pragma once

// QoL (USER_BUGS Phase 3 item 7; not in the original game): Replay Jobs. In the episode menu, with an episode
// shown (mode 8, the "Start Episode" screen), TRIANGLE opens a list of that episode's jobs the save has opened
// (each with its name and description: the jobs' own strings +0x6C / +0x70, the pause menu's Job Help texts, read
// from the episode's main world file). Choosing one starts it the way the game starts a task (0x1651D8: its world
// loaded with the task queued, its briefing / intro task activated, the later tasks back to locked as the queued
// start's 0x164DF0 does). While a replay runs, every save keeps the progress the save had before it
// (savedata.h MergeProgressKeepDone); the job's end (complete, abandoned, Exit to Episode Menu) returns to the
// episode menu on the replayed episode with that progress.
//
// With the list available, CIRCLE goes back from the episode screen (the original's TRIANGLE back still works in
// an episode that has no job to replay). SLY2N_NOREPLAYJOBS turns the feature off (the original's menu exactly).

#include "lo.h"

#include <cstdint>

struct SW;
struct JOB;
struct GAMESAVE;

// transition.cpp: before the front end world (record 1) loads, once per run: each episode's main world file read
// into a temporary world for its jobs (no world is current then)
void BuildReplayJobTables(int iwrecLoading);
// transition.cpp: after a world loaded (the front end, or the title (request flags 1), ends a replay: its progress merged)
void OnReplayWorldLoaded(int iwrec, int grfReq);

// the episode menu (frontend.cpp UpdateEpMenu, mode 8 sub -1, episode iep 1..8): 0 not handled (the original's
// input), 1 handled, 2 back (the original's TRIANGLE, mode 9), 3 a job chosen (the start: mode 10, then
// StartReplay)
int NUpdateReplayMenu(int iep);
bool FReplayChosen();
void StartReplay(SW* psw);              // frontend.cpp mode 10: the chosen job's world requested
void CloseReplayMenu();
int IepReplayReturn();                  // the episode a replay just ended in (StartEpMenu shows it), else -1

// the replay
bool FReplayActive();
void MergeReplaySave(GAMESAVE& gs);     // frontend.cpp FWriteSaveSlot: what a save writes during a replay
bool FReplayJobEnd(SW* psw, JOB* pjob); // task.cpp JobCompleteNext / JobEnd: true: the replay returns to the menu
