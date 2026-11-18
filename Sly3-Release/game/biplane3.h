#pragma once

// Sly 3's planes (class 0x52, game/biplane3.cpp; notes/PLANE3_2026-11-12.md).

struct LO;
struct SW;
struct CM;

// a plane (class 0x52: the gang's biplane, the computer's, the 2P duel's copies of 95691)
bool FPlane3(const LO* plo);
// the world's reset (po.cpp)
void ResetPlanes3();
// the harness's autopilot (route.cpp's "dogfight" step)
bool FDogfightStick3(LO* plo, float* px, float* py, bool* pfFire, int* pnSpeed);
// (PC, splitscreen3.cpp) another player's view camera: a plane player's own plane camera (CPPLANE3) in that view
void SyncPlaneCameraView3(LO* plo, CM* pcm);
// orig3 0x10A520 (the SO's option 948) -> 0x1C93A8 -> the plane's vt slot 31 (0x1414D0 -> 0x142848): hit points, ammunition,
// weapon, state 1 (the 2P duel's respawn, damage.cpp)
void RestorePlane3(LO* plo);
// (PC, duel3.h, the free-for-all) the plane that last hit plo (its projectile's launcher, or a rammer) and when (*pt the
// game clock), null none; a plane's team (+0x194: the computer's and the autopilot's targets are the other teams)
LO* PloPlaneLastShooter3(const LO* plo, float* pt);
void SetPlaneTeam3(LO* plo, int team);
int NPlaneHp3(const LO* plo);
// in the world and flying (states 1..4: not dead / not yet reset)
bool FPlaneFlying3(const LO* plo);
