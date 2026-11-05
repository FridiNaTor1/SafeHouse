#pragma once

// The game host's interim binocucom, dialog and HUD glue (the Sly 3 boot to gameplay), for the SCPT executor's commands in the game
// (game3.cpp GAMESCPTHOST3), until the player side's binocucom (actor +0xF60 / +0xF80 / +0x1008: 0x1BDA88 open,
// 0x1BDB50 close, 0x1BE370 speaker, the camera policy) replaces it:
// - the binocucom's state (actor +0xF80) goes 0 -> 1 40 frames after an open, back to 0 for 60 frames on a speaker
//   change, -1 20 frames after a close (the reference's numbers, sly3/scpttest3.cpp's); the screen objects shown are
//   m_ext's: 1042 the frame, 1039 the lower panel, 1045 / 1038 (under Sly 24 / Bentley 25) the head windows (1038
//   occludes the HUD), with the head cameras from the actors' class-0x06 cameras 772 / 773;
// - the dialog lines play through sound.cpp (PsndStartDialog: the voice, its subtitle, its length);
// - the title's screen object 1699 while the title shows; the hint (0x60 / 0x61) on the prompt 1058 (its text id);
// - the gameplay HUD 7465 (Sly's portrait) once the player is in control.
// TODO(sly3, the Sly 3 player): the binocucom's own update (its open / close animation, the camera's moves), the
// heads' cameras from the actors' animated cameras.

struct LO;
struct SW;

namespace talk3
{
void Reset(SW* psw);                  // a world's start
void Update(int cframe);              // each frame (the binocucom's state, the screens)
void OpenBinoc(LO* ploActor, LO* ploSpeaker);
void CloseBinoc(LO* ploActor);
int BinocState(LO* ploActor);         // actor +0xF80: 1 open and idle, 0 busy, -1 closed
void SetSpeaker(LO* ploActor, LO* ploSpeaker);
void ShowTitleScreen(bool fShow);
void ShowHint(int idText);
void ClearHint();
void ShowGameplayHud();
// the card's messages (orig3 0x1D0D30's prompts): screen object 1466 (the black box with the lines), its script
// strings 847 (the message: a text id) and 848 (the prompt: 1 OK, 2 yes / no)
void ShowCardMessage(int msg, int prompt);
void HideCardMessage();
}  // namespace talk3
