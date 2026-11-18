#pragma once

// Sly 3's binocucom panel: the window record of an actor's binocucom (orig3: the BINOC at actor
// +0xF60, its window at +0x1C; Sly 2's binoc.cpp window). Its screen objects are the actor's: +0x8 the frame (m_ext's
// 1038 under the actor: the compass 0x3CD, the zoom 0x3CC, the indicator 0x3CE; arguments 0 / 1 the current job's name
// and its first open task's goal, 0x1BE918) and +0x10 the speaker's window (1045 under the actor: Bentley's on the left;
// 0x3CF shown, 0x3D0 the speaker going, 0x3D1 the next one coming). The window's state (actor +0xF80) is what the
// scripts wait on: -1 closed, 0 opening, 1 open and idle, 2..4 a speaker change, 5 closing.
// orig3: SetBinocWindowState 0x1BE5A8, SetBinocWindowArgs 0x1BE918, UpdateBinocWindow 0x1BEA30, the speaker 0x1BE370.
//
// the script code's hooks (game/scptgame3.h pfnOpenBinoc / pfnCloseBinoc / pfnBinocState / pfnSetBinocSpeaker) are set to
// these at the world start unless another module set them first: the binocucom's camera, sound and the actor's side
// (orig3 0x1BDBB0) are the player code's, whose hooks then call these for the panel.

struct LO;
struct SW;

// kind: the binocucom's kind (orig3 BINOC +0x18: 1 a talk, with the speaker's window; else the recon view)
void OpenBinocPanel3(LO* ploActor, int kind, LO* ploSpeaker);
void CloseBinocPanel3(LO* ploActor);
int NBinocPanelState3(LO* ploActor);                  // -1 closed .. 5 closing (see above)
// a photo helper driven by a panel here (its state set by the window states, updated every frame by
// UpdateBinocPanels3, orig3 0x1BE5A8 / 0x1BEA30): binoc.cpp's Sly 2 window then leaves it (and the task broadcasts) alone
bool FBinocPanel3Prompt(const void* pprompt);
void SetBinocPanelSpeaker3(LO* ploActor, LO* ploSpeaker);
LO* PloBinocPanelSpeaker3(LO* ploActor);              // the actor in the speaker's window (orig3 BINOC +0x40)
// the speaker in the window held (orig3 0x1BE408, the camera's state 1 of a talk) / released (0x1BE4A8); a speaker
// change then releases the old one and holds the new one (0x1BE3D0)
void HoldBinocPanelSpeaker3(LO* ploActor, bool fHold);

void InitBinocPanels3(SW* psw);   // a world's start (the HUD's init)
void UpdateBinocPanels3(SW* psw); // every frame (the HUD's update)

// (binoccam3.cpp) the binocucom's camera: sets the script hooks' binocucom entries (the panel above plus the camera;
// called by InitBinocPanels3 before its own defaults)
void InstallBinocCam3();
void ResetBinocCam3();
void UpdateBinocCam3();   // every frame (UpdateBinocPanels3)

// (binoc.cpp) the actor side of a binocucom state change (vt slots 94 / 87, the sound channels 0 / 1 ducked and the
// camera cut, the model hidden while it looks: 0x241FC0 / 0x242228)
struct ACTOR;
bool FBinocAnimAllowed3(ACTOR* pa);
void DuckBinocSounds3(float g);
int NActorState3(ACTOR* pa);   // (debug: the gang's state, Sly 3 numbered)
void ShowBinocActorParts3(ACTOR* pa, bool fHidden);
void HideActorForBinoc3(ACTOR* pa, bool fHide);
// the panel's window put to -1 at once (orig3 0x1BDBB0 state -1: 0x1BE5A8(hud, -1))
void ShutBinocPanel3(LO* ploActor);
// the scripts' binocucom's state (orig3 BINOC +0x4 at actor +0xF64: -1 none, 0 raising, 1 up, 2 lowering)
int NBinocMode3(LO* ploActor);
void ShutBinocMode3(LO* ploActor);   // (0x1BDBB0(-1) from elsewhere: put down at once)
