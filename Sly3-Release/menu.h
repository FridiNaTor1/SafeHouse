#pragma once

// Sly 3's paged menu (orig3 0x46F550, module 0x1D2C20..0x1D57B8, Sly 2's 0x3D9ED0 with Sly 3's tables): a stack of
// pages of entries on a screen object (the wheel of 1056 by default, an external screen file), each page's model
// animated by the menu's state (+0x1FC). Its pages: the pause menu (0x1D48D0, table 0x345DA0: Keep Playing / Quit Game
// (Save and Quit) / Exit to Game Menu / Abandon Job / Options... / Control Help / Job Help / the 3-D resets), the
// options (0x1D4C48, 0x345B40: New / Load / Save Game..., Speakers, Music / Effects Volume, USB Headset, Camera
// Options…, Center Screen..., Vibrations; plus the PC's Display Options... / Controls...), the save slots (0x345990 /
// 0x3459F0 / 0x345A50) and the yes / no pages. The card dialog's messages are card3.h's (screen object 1466).

#include "lo.h"
#include "savedata.h"

#include <cstdint>
#include <string>

struct SW;

void InitMenu(SW* psw);        // orig 0x001AE928 (+ the card dialogs' 0x1DA770): after a world load
void UpdateMenu(SW* psw);      // orig 0x001AEEE0 and the card dialog's 0x1ACCC8, every frame
bool FMenuShown();             // orig 0x001D9880 on 0x3D9ED0 / 0x2E6550 / 0x3DA4F0 (the episode menu waits)
bool FMenuActive();            // the menu is in the HUD's list (the game paused under it)
bool FCardMessageActive();     // orig 0x001D9880 on the card widget 0x2E6550
// orig3 0x001D48D0: the pause menu (false when it cannot open now)
bool FOpenPauseMenu(SW* psw);
// orig3 0x001D4BF8: the options page from the title (SELECT)
void OpenDataAccessMenu(SW* psw);
// orig3 0x001D0C08 on 0x3751D0 (card3.h): a card message (Sly 3's text ids: 0x26 saving, 0x28 overwrite?, 0x2F the
// auto-save warning, ...); nAfter: +0x60 (1 boot again, 2 close the menu, 3 an auto-save)
void ShowCardMessage(int idMsg, int nAfter = -1);
// (card3.cpp) the menu's card messages answered (orig3 0x1D1370 yes / 0x1D1190 no): true when the message is the menu's
bool FMenuCardAnswer3(int msg, bool fYes);
// (screen3.cpp) the menu's script strings (815..824 the wheel, 917..929 Control Help, 1692..1698 the slots' pages)
bool FStrMenuScript3(int id, std::string* pstr);
// (the camera code) the options' Camera Options… page: a camera kind's (0 the camera, 1 the binocucom, 2 the grapple-cam,
// 3 a turret, 4 strafing, 5 swimming, 6 the harpoon) Left/Right (fVertical false) or Up/Down reversed
bool FCamReverse3(int kind, bool fVertical);

// (Sly 2's script string ids: none in Sly 3)
std::string StrMenuScript(int id);
// the auto-save the episode menu runs when it starts (orig 0x1ABD98(g_save, 1))
void HudAutoSaveFrontEnd();

// orig 0x2E6550 +0x44 (0x1ACC38): the card's messages: 0 none yet, 1 on (the boot sequence), 2 off (no card for
// this game: the "create a save file?" answered no)
extern int g_ncardMode;

// orig (Europe) 0x001DDFB8: the title's language menu (a disc with two or more languages)
void OpenLanguageMenu(SW* psw);
// the card's status known (the European title's first Start turns the card on: frontend.cpp)
bool FCardSettled();

// orig3 0x0021D360: a record's settings take effect (the mixer mode, the music and effects volumes)
void ApplyGameSettings(const GAMESAVE& gs);
