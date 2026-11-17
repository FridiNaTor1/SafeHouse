#pragma once

// (not in the original) Mod packages' custom tasks' progress. The map editor's new jobs have tasks without a save key
// of their own (a new key needs the progress layout every world file carries: changing it would clear every save). A
// package lists such tasks in its mod.json ("custom_tasks", platform/mod_package.h); the game keeps their states in a
// side file next to the save slot, "<slot>.mods.json" ({package id: {world: {oid: state}}}), and restores them after
// the save's own (game/task3.cpp RestoreTask3). The save slot is never changed. Without a package listing tasks nothing here
// runs (one bool test in the task code) and no side file is written; a side file's entries of a package that is not
// loaded are kept as they are and written back with the slot.

#include <string>

// a task listed by a loaded package for the current world: *pn its kept state (-1: none kept yet); false: not listed
bool FModTaskState(int oid, int* pn);
// a listed task's state kept (false: not listed)
bool FSetModTaskState(int oid, int n);

// the states of the save at pathSlot (its side file; none: no states), into the running game
void ReadModSave(const std::string& pathSlot);
// the running game's states written next to pathSlot (no states: the side file removed, if there is one)
bool WriteModSave(const std::string& pathSlot);
// the slot's side file removed (the slot erased)
void RemoveModSave(const std::string& pathSlot);
// a new game: no states
void ClearModSave();
// the next game start's states (frontend.cpp's pending save): read from pathSlot ("" : none), applied by ApplyPendingModSave
void PendModSave(const std::string& pathSlot);
void ApplyPendingModSave();
