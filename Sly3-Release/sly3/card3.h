#pragma once

// Sly 3's memory card dialog (orig3 0x1CF0D0..0x1D1470; the card object 0x3751D0, Sly 2's memcard.cpp / menu.cpp
// card part), on host files: the three slots are <user-dir>/Saves/save1..3 (frontend.h SAVESLOT; with the launcher's
// --user-dir <profile>/sly3), each the 0x6808-byte block exactly as the card's BASCUS-97464YAOTWTD!/saveN holds it
// (savedata.h; the 0x5C directory entry of the 0x6864-byte RAM slot is the card's, not stored). PS2 saves load as
// they are; tools/savedump3.py prints them.
//
// The card object: +0x50 the message shown (-1 none; a message is its text id in the world's text: 0x20 = 32 "No Sly 3
// Save File ... create?"), +0x54 when it was shown (real time), +0x58 the card state (SetCardState 0x1D0D30: 1 the
// boot's check), +0x5C its time. The save system's card status (g_save +0x8): 1 no card, 2 unformatted, 3 no Sly 3
// save file, 4 full, 6 ready (the host card: 3 while no slot file exists, else 6).
// Per frame: UpdateCardStatus 0x1CF0D0 (in state 1, 0.3 s after the status settled: status 3 -> message 0x20; status
// 6 -> the slots read, the newest chosen (0x1CF640 / ChooseNewestSlot 0x1CF430), the state back to 0) and the input
// 0x1D0DC8 (0.1 s after a message shows: NCardPrompt 0x1D0D78 kind 1 (OK) SQUARE 0x80 -> CardNo, kind 2 (yes / no)
// SQUARE -> CardYes 0x1D1370, TRIANGLE 0x10 -> CardNo 0x1D1190; then the message's own step: 0x21 "creating" ends
// when the card thread does: closed, SetCardState(1) again (which finds the save), or 0x22 on an error).
// CardYes on 0x20: the save folder made (the thread 0x323BE8: directory, icon, the three slots), message 0x21.
// CardNo on 0x20: closed, the game goes on without saving.

#include <string>

struct CARDHOST3
{
    virtual ~CARDHOST3() = default;
    virtual float TReal() { return 0; }                    // g_clock +0x40 (0x36BBD8)
    virtual bool FBusy() { return false; }                 // FTransitionBusy 0x25AE28, a movie playing 0x1DC520
    virtual bool FButtonPressed(int grf) { (void)grf; return false; }  // 0x1A7A80(the default pad 0x36E740, grf, 0)
    // the card widget (its vt slots +0x50 show / 0x1D0C88 hide; its screen object +0x34 shows the text): the message
    // is a text id of the world's text; prompt: NCardPrompt's kind (0 none, 1 OK, 2 yes / no, 3)
    virtual void ShowMessage(int msg, int prompt) { (void)msg; (void)prompt; }
    virtual void HideMessage() {}
};

struct CARD3
{
    int state = 0;      // +0x58
    float tState = 0;   // +0x5C
    int msg = -1;       // +0x50
    float tShown = 0;   // +0x54
    bool fShown = false;
    int status = 0;     // g_save +0x8 (0: not read yet)
    float tStatus = 0;  // g_save +0x10
    bool fNoSave = false;  // CardNo on 0x20: no card for this game (g_ncardMode 2)
};

extern CARD3 g_card3;

void SetCardHost3(CARDHOST3* phost);
void SetCardState3(CARD3& card, int state);         // orig3: 0x1D0D30
void ShowCardMessage3(CARD3& card, int msg);        // orig3: 0x1D0C08
void CloseCardMessage3(CARD3& card);                // orig3: 0x1D0C88
int NCardPrompt3(int msg);                          // orig3: 0x1D0D78 (the table 0x323B60)
void UpdateCard3(CARD3& card);                      // orig3: 0x1CF0D0 then 0x1D0DC8
bool FCardMessageShown3(const CARD3& card);
// the host card's status: 6 when a slot file exists in <user-dir>/Saves, else 3 (no Sly 3 save file)
int CardStatus3();
// the save folder made (the create thread 0x323BE8): the three slots written with the empty block of the current
// progress layout (zeros, then +0x6800 / +0x6804 its identity, as the original's fresh slots)
bool FCreateCardSaves3(std::string& error);

// The game loop's card (the native boot: BOOTHOSTMAIN3's card check installs it once): UpdateCard3 each frame of
// game3's loop (a frame hook), the pad's SQUARE / TRIANGLE (frontend's GrfbtnTakePressed; SLY3_CARD_PRESS=SQUARE@F,..
// frames of the loop for headless runs), the message printed with its text, the game clock stopped while a message
// shows (as the reference: g_clock T frozen from the prompt (vblank ~1333) to ~19 frames after its close (1521)).
void InstallCardGame3();

// --card-test3 [--press SQUARE@F,TRIANGLE@F] [--frames N]: the boot's card check (state 1) on --user-dir, headless, 60
// frames a second; prints the messages, the prompts, the state changes and what was read or written.
int CardTest3Command(const std::string& press, int cframe);
