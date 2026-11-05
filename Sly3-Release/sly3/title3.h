#pragma once

// Sly 3's title (the widget 0x46FC30, shown by SCPT 0x50 through ShowTitle 0x221AC8: +0x54 = 1, +0x50 = 0, put on
// the HUD's list 0x46F540; FTitleShowing = 0x21DD30, FFindDlEntry on that list). Its update 0x221C08, while shown and
// not yet answered (+0x50 == 0): START (0x800) on its pad: with a save chosen (g_save +0x13954) SetCardState(1) and
// StartGameFromSave 0x18AB08 (that slot), else (the card idle) StartNewGameFromTitle 0x221EC0 (the title leaves the
// list); TRIANGLE 0x10 the options page (0x6A7, not ported).
// Native: the title's state and its input for the SCPT host (SCPTHOST3::ShowTitle / FTitleShowing) and the game loop.

struct TITLE3
{
    bool fShown = false;  // on the HUD's list (0x21DD30)
    int n50 = 0;          // +0x50 (answered)
    int n54 = 0;          // +0x54
};
extern TITLE3 g_title3;

void ShowTitle3();             // orig3: 0x221AC8
bool FTitleShowing3();         // orig3: 0x21DD30(0x46FC30)
void UpdateTitle3(bool fStart);  // orig3: 0x221C08 (fStart: START pressed this frame)
// the game loop's title input (a frame hook): the pad's START (frontend's GrfbtnTakePressed), or SLY3_START_AT=F,F..
// (frames of game3's loop) for headless runs
// orig3: 0x18AB08 (the slot chosen at START)
void StartGameFromSave3(int islot);
void InstallTitleGame3();
