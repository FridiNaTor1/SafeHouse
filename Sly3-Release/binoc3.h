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
void SetBinocPanelSpeaker3(LO* ploActor, LO* ploSpeaker);
LO* PloBinocPanelSpeaker3(LO* ploActor);              // the actor in the speaker's window (orig3 BINOC +0x40)

void InitBinocPanels3(SW* psw);   // a world's start (the HUD's init)
void UpdateBinocPanels3(SW* psw); // every frame (the HUD's update)
