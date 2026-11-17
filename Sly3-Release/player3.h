#pragma once

// Sly 3's player in the native game: the development start in gameplay and the per-frame trace (player3.cpp).
//
// SLY3_START=1 (with --world 35): at the first frame Sly (cid 0x34) is placed where the reference has him when he
// gets the pad (Sly3-research/ref3/boot v.5600.ram), heading 1.2043, and is made the current player (EnablePo).
// SLY3_START=x,y,z[,rad] places him elsewhere. SLY3_PLAYERTRACE=file.csv writes his state per frame.

#include <glm/glm.hpp>

struct ACTOR;
struct LO;
struct PADCTL;
struct SW;

namespace sly3
{
LO* PloSly3(SW* psw);
bool FStartPlayer3(SW* psw);              // (game.cpp, frame 0)
void InstallPlayerHooks3();               // (the scripts' actor hooks: transition.cpp, before the start's activation)
void TracePlayer3(SW* psw, int frame);    // (game.cpp, after each frame's update)
int NPlayerCamKind3(SW* psw);             // orig3 0x1A9260: the current player's camera kind (menu.cpp Camera Options)
// SLY3_SETPOS="F:X Y Z;F2:X Y Z;..": Sly moved there at the start of frame F (his velocities zeroed), as the reference's
// SLY2_SETPOS (vt slots 34 / 40 / 41) at vblank F + the route's base (tools/player3: the moves' routes)
void SetPosPlayer3(SW* psw, int frame);   // (game.cpp, before each frame's update)
// SCPT 0x30's walk: the actor's stick toward the point while the command's done test refreshes it (sly.cpp UpdateActor)
bool FDriveActorWalk30(ACTOR* pa, PADCTL* pc);
// SCPT 0x26's climb: the goal (distance along the pipe) while the command's done test polls it, and the drive (sly.cpp)
void ActorClimbToward3(ACTOR* pa, float sGoal);
bool FDriveActorClimb26(ACTOR* pa, PADCTL* pc);
// orig3 0x22A7F8 cases 93 / 94 (SCPT 0x5D / 0x5E): triangle held on the actor from 0x5D until 0x5E, pressed at its start
// (m_ext's land beast intro SCPT 4318 [31]: Bentley throws his bomb at the lamp post 15452)
bool FHold5D3();   // on by default (SLY3_HOLD5D=0: off; see player3.cpp)
void SetActorHold5D3(ACTOR* pa, bool fHold);
void ApplyActorHold5D3(ACTOR* pa, PADCTL* pc);
}  // namespace sly3
