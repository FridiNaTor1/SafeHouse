#pragma once

// The paged menu (orig 0x3D9ED0, module 0x1AE928..0x1B0B78): a stack of pages of entries on a screen object
// (the wheel of 0x370 by default), each page's model animated by the menu's state (+0x1E8). Its pages: the pause
// menu (0x1B0290: Keep Playing / Quit Game / Exit to Episode Menu / Abandon Job / Options... / Controls... /
// Job Help), the options (0x1B04E0: New / Load / Save Game..., the sound, the screen centre, the vibrations; plus
// the PC display options, as Sly1-Release's prompt.cpp options menu), the save slots (the data access pages
// 0x2BFDA8 / 0x2BFE08 / 0x2BFE68: 0x1AD250 / 0x1AD320 / 0x1AD480) and the yes / no pages. The card dialogs
// (0x2E6550 on screen object 0x52C: 0x1ACB58 / 0x1ACCC8) do their messages over host files.

#include "lo.h"
#include "savedata.h"

#include <cstdint>
#include <string>

struct SW;

void InitMenu(SW* psw);        // orig 0x001AE928 (+ the card dialogs' 0x1DA770): after a world load
void UpdateMenu(SW* psw);      // orig 0x001AEEE0 and the card dialog's 0x1ACCC8, every frame
bool FMenuShown();
bool FPauseMenuShown();          // (the pause menu itself: VR's black around its window)             // orig 0x001D9880 on 0x3D9ED0 / 0x2E6550 / 0x3DA4F0 (the episode menu waits)
bool FMenuActive();
int NMenuState();              // orig 0x3DA0B8 (MENU +0x1E8): 0 none; the cheat codes (cheat.h) read it            // the menu is in the HUD's list (the game paused under it)
bool FCardMessageActive();     // orig 0x001D9880 on the card widget 0x2E6550
// orig 0x001B0290: the pause menu (false when it cannot open now)
bool FOpenPauseMenu(SW* psw);
// orig 0x001B04E0(0): the options page from the title (SELECT)
void OpenDataAccessMenu(SW* psw);
// orig 0x001ACB58 on 0x2E6550: a card message (0x1A saving, 0x1C overwrite?, 0x22 the auto-save warning, ...)
void ShowCardMessage(int idMsg, int nAfter = -1);   // nAfter: +0x4C (1 boot again, 2 close the menu, 3 an auto-save)

// the menu's script strings (0x2D2..0x2DB its wheel, 0x5CB..0x5D1 the slots, 0x2F2 / 0x2F3 the card dialog)
std::string StrMenuScript(int id);
bool FMenuActionPrompt(int textId, int& binding, bool& controller);
// the auto-save the episode menu runs when it starts (orig 0x1ABD98(g_save, 1))
void HudAutoSaveFrontEnd();

// orig 0x2E6550 +0x44 (0x1ACC38): the card's messages: 0 none yet, 1 on (the boot sequence), 2 off (no card for
// this game: the "create a save file?" answered no)
extern int g_ncardMode;

// orig (Europe) 0x001DDFB8: the title's language menu (a disc with two or more languages)
void OpenLanguageMenu(SW* psw, bool fCanCancel = true);
// the card's status known (the European title's first Start turns the card on: frontend.cpp)
bool FCardSettled();

// orig 0x001D8FF8: a record's settings take effect (the mixer mode, the music and effects volumes)
void ApplyGameSettings(const GAMESAVE& gs);
// (PC, Settings.ini) the current record's option flags set to the options (display.h GAMEOPTS); nothing in headless
// runs, where the record's flags are the options as in the original
void SyncOptionsToRecord();
