#pragma once

// The front end in the game loop: the boot sequence (0x1A4E08's queued actions: the logo movie, the card check,
// the boot world as the title), the save slots (the memory card's three, 0x1AA978.. g_save 0x2E65E0: here host
// files, as Sly1-Release does), the title (the HUD widget 0x3DA530 on screen object 0x5D2: 0x1DD920), starting
// or resuming a game (0x165E38), the episode menu of the front end world (class 0x83: 0x15CDA8 / 0x15CF30), the
// exits that start an episode (class 0x1A: 0x228358 / 0x2283D0 / 0x228538) and the movies they queue
// (0x1B3A48, played by the game window's presenter, moviegl.h).

#include "lo.h"
#include "savedata.h"

#include <cstdint>
#include <string>
#include <vector>

struct SW;

// ---- the pad as the menus read it (orig JOY 0x2DFC00: GrfbtnTakePressedJoy 0x187D80, the steps 0x1880D8 /
// 0x187E88): Sly's button layout (0x800 Start, 0x100 Select, 0x10 Triangle, 0x20 Circle, 0x40 Cross,
// 0x80 Square, 0x1000 Up, 0x2000 Right, 0x4000 Down, 0x8000 Left)
void JoyBeginFrame();                           // (each frame, before the menus read it)
uint32_t GrfbtnTakePressed(uint32_t grfMask);   // orig 0x00187D80 (taken: later calls this frame miss them)
uint32_t GrfbtnPressed(uint32_t grfMask);       // orig 0x00187DD0
uint32_t GrfbtnHeld();
int NStepUpDown();      // orig 0x001880D8: -1 up, 1 down, with repeat
int NStepLeftRight();   // orig 0x00187E88: -1 left, 1 right
void ClearJoySteps();   // orig 0x00187E78

// ---- the save slots (orig g_save +0x24: three slots of 0x5464 bytes, the save block and the card's header)
struct SAVESLOT
{
    std::string path;       // <user-dir>/Saves/save<i+1>
    bool fFile = false;     // the file exists and loaded
    GAMESAVE save;
    int64_t tWrite = 0;     // its time (orig the header +0x5408: 0x1D8DB8 compares)
};
// QoL (Q4, not in the original): more slots than the card's three, paged three at a time on the data access pages
// (menu.cpp, L1 / R1). Slots 0..2 are the original save1..3 (same files, same format); slot i >= 3 is the extra file
// <user-dir>/Saves/save<i+1>, made only when a game is saved there. 10 pages of 3 = 30 slots.
constexpr int kSlotsPerPage = 3;
constexpr int kSlotPages = 10;
constexpr int kSlotMax = kSlotsPerPage * kSlotPages;
constexpr int kSlotsCard = 3;   // (the original's: the save folder's files, the card's checks)
extern SAVESLOT g_aslot[kSlotMax];
extern int g_islotCur;      // orig g_save +0xFD54: the newest slot (-1: none)
extern int g_islotChosen;   // orig g_save +0xFD50: the slot the game is played from (its saves go there)
// orig 0x001AB630 / 0x001AB418 state 6: the slots read, the newest chosen. dirDev: a fallback directory
// with save1..3 (the development card) when the user has none. (QoL: save4..save30 too, from <user-dir>/Saves.)
void ReadSaveSlots(const std::string& dirDev);
// orig 0x001AB630 after the card changed (the folder made): the slots validated, the newest chosen again
void RechooseSaveSlots();
// a progress value of a slot (orig 0x2581C0 on the slot's block)
CRef RefProgressOfSlot(int islot, const std::vector<std::string>& apath);
// orig 0x001AD320 + 0x001ABCE8: the current progress into slot i (its file written)
bool FWriteSaveSlot(int islot);

// ---- the boot sequence (orig 0x1A4E08 / 0x1A4E80)
// Starts it: the logo movie (2056), the card check, then the boot world as the title (request flags 1).
void StartBootSequence(bool fMovie);
// Movies queued for the next world change (orig 0x1B3A48 on 0x2F66B0): played before it by the presenter.
void QueueMovie(const uint8_t* pbKey);
void PlayQueuedMovies(bool fWindow);
extern bool g_fMoviePlaying;   // (a queued movie is on: PlayQueuedMovies)
bool FMoviesQueued();

// ---- the title and the game start
// After each world load (with the request's flags): the title world's title object found (shown later by its
// script's command 0x44, ShowTitle), the episode menu and the exits started (orig: their vt slot 15).
void StartFrontEnd(SW* psw, int grfReq);
// Every frame (with the HUD's update): the title's input, the episode menu, the exits.
void UpdateFrontEnd(SW* psw);
// orig 0x00165E38: start the game of save block pgs (a slot's, or the current progress)
void StartGameFromSave(SW* psw, const GAMESAVE* pgs);
bool FTitleShown();
// (PC menus) on the episode menu's "Start Episode" screen (mode 8, list closed): its back button in Sly 2's bit
// order (Triangle, or Circle / Japan's Cross while Replay Jobs is offered: replay.h); 0 elsewhere
uint32_t BtnEpisodeScreenBack();
// orig 0x001DDB08: a new game from the title (no slot)
void StartNewGameFromTitle(SW* psw);
// the world was started for the title (the boot world with request flags 1)
bool FTitleWorld();
// orig 0x001DD820: the title shown (SCPT command 0x44)
void ShowTitle();
// orig 0x00161A58 (3): the game record at a world's start (request flags 1: reset, 0x40: the pending save)
void OnGameRecordEvent(int grfReq);
// the exits (class 0x1A): orig 0x00228358 / 0x002283D0
void SetExitState(LO* plo, int n);
void TriggerExit(LO* plo);

// the front end's script strings (0x5D3 the title's help, 0x52F / 0x530 / 0x5BF / 0x5C0 the episode menu's)
std::string StrFrontEndScript(int id);
