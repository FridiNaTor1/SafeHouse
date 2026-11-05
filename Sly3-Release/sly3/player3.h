#pragma once

// Sly in the native Sly 3 game loop (the Sly 3 player): the beach start of m_ext (gameplay after the binocucom talk).
//
// What is the original's here and what is interim:
// - The numbers of Sly's state machine are Sly 3's (JTS3, orig3 0x1ADDD0 SetSlyState's tables 0x322750 / 0x322810,
//   the per-frame transitions 0x1AC098's table 0x322550): 0 stand, 1 run, 2 the R1 sneak, 3 the jump (the jump
//   sub-object +0x320: its kind +0x4, its animation state +0x1C), 10 around a target. The transitions of the ground
//   states (0x1AC26C..0x1AC504: X within 0.1 s -> the jump's kind 1, the stick's deflection > 0.1 (from 0) / > 0
//   (moving) -> 1, R1 -> 2, none -> 0) are the asm's; the others are not reached on the beach.
// - The motion is INTERIM: it reproduces the reference's measured kinematics (tools/refroute3.sh routes3/beach.route:
//   the run ramps 56.93 / frame to 510 x deflection, the stop decelerates at about 4000 / s^2, the jump leaves at
//   986.3 up, gravity 2277.6, the double jump 675.6) until the STEP physics (orig3 0x24E1A0 UpdateStep, 0x24E2B8)
//   and the jump (0x1B9020 / 0x1B9C48 / 0x1B88E0) are ported; the floor is a probe of the static render geometry
//   until the collision (the SO's geometry, not loaded yet) is.
// - The camera is INTERIM: a follow at the reference's beach offset (392 behind, 139 above the root record) until the
//   camera policies (the world camera, class 0x05, and its policies) are ported.
//
// Enable: SLY3_PLAY=1 (the world start puts Sly where the reference has him at gameplay, vblank 5900 of
// refroute3.sh, and gives him the pad; the camera follows him). SLY3_ROUTE=file plays a refroute3 route (frames
// relative to the world start), SLY3_PLAYERTRACE=file.csv writes refroute3.py's columns per frame
// (tools/playercmp3.py compares the two).

struct SW;

namespace sly3
{
enum JTS3
{
    JTS3_None = -1,
    JTS3_Stand = 0,    // Sly 2's 0
    JTS3_Run = 1,      // Sly 2's 12
    JTS3_Sneak = 2,    // Sly 2's 1 (R1)
    JTS3_Jump = 3,     // Sly 2's 2
    JTS3_Swing = 4,    // Sly 2's 3 (CalculateSlyHangAccel 0x1B0FF0 at its entry)
    JTS3_Damage = 5,   // Sly 2's 4
    JTS3_Target = 6,   // Sly 2's 5
    JTS3_Dodge = 7,    // Sly 2's 6
    JTS3_Interact = 8, // Sly 2's 7
    JTS3_Climb = 9,    // Sly 2's 8
    JTS3_Around = 10,  // Sly 2's 10 (a target of rank 3, +0x11BC)
    JTS3_Pickpocket = 11,  // Sly 2's 11
};
// The Sly 2 state of the copied sly.cpp for a Sly 3 one and back (-2: no counterpart known)
int Jts2FromJts3(int jts3);
int Jts3FromJts2(int jts2);

bool FPlayEnabled3();
// (the Sly 3 boot to gameplay) the game hands Sly the pad at gameplay (game3: the talk over, no script camera): StartPlayer3 without
// SLY3_PLAY; the loop's input step (game3 UpdateInput3) reads the pad
extern bool g_fPlayGame3;
// The world start (game3's FLoadWorld3): finds Sly (cid 0x34) and his scene entry, registers the update.
void StartPlayer3(SW* psw);
// The world camera's follow policy (cmfollow3), from the cameras' update after the objects (game3 UpdateCameras3);
// fKeyActive: a script's camera key holds the camera. True when the follow moved the camera.
bool FUpdatePlayerCamera3(bool fKeyActive);
}  // namespace sly3
