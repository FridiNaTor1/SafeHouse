#pragma once

// Sly 3's memory card dialog (orig3 0x1CF0D0..0x1D1470, the card widget 0x3751D0) on the PC's save files (frontend.h
// SAVESLOT: <user-dir>/Saves/save1..3, each the 0x6808-byte block). Its messages are text ids of the world's text
// (0x20 = 32 "No Sly 3 Save File ... Do you want to create a Sly 3 Save File?"), shown in the global screen object 1466
// with the script strings 847 (the message) / 848 (its buttons: screen3.h).
//
// PC save handling as Sly 2's port (menu.cpp Q2): a PC has no memory card, so the boot's check does not ask: with no
// save file the folder and save1..3 are made at once (the original's Yes without the "creating" notice); only an error
// shows the original's message (0x22). SLY3_CARDPROMPT=1 (or SLY2N_CARDPROMPT=1) asks the original's question (0x20,
// SQUARE yes / TRIANGLE no) instead.

struct SW;

// the boot's card check (the boot queue's kind 5: SetCardState(card, 1), orig3 0x1D0D30)
void StartCardCheck3();
// every frame (the HUD's update): the check (UpdateCardStatus 0x1CF0D0) and the dialog's input (0x1D0DC8)
void UpdateCard3(SW* psw);
// a message shown (orig3 0x1D0C08) / closed (0x1D0C88); NCardPrompt 0x1D0D78: its buttons (1 OK, 2 / 3 yes / no)
void ShowCardMessage3(int msg);
void CloseCardMessage3();
int NCardPrompt3(int msg);
// the dialog is up (the title's input waits: 0x221C08)
bool FCardMessageShown3();
// the boot's check is over (state 0)
bool FCardSettled3();
// the message shown (-1 none) and how long it has been up (real time)
int IdCardMessage3();
float DtCardMessageShown3();
