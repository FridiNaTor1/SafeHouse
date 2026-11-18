#pragma once

// PC debug menu (debugmenu.h, Debug keys on): the player characters' gravity and the player's character scale. Not in
// the original game. At their defaults (1x) nothing here changes a value the game computes: every hook is skipped.
//
// Gravity: a factor on the gravity of Sly, Bentley and Murray (classes 0x35..0x37: their mass state's +0x80, set
// at rest by 0x1F3590 and by the scripts' 0x1F5880; the game's own changes to it (0x204DA8's size) are relative, so
// they keep the factor). Everything that reads the gravity sees the same value: a jump goes higher and falls slower
// below 1x, the aimed jumps (poles, ropes, ledges) still reach their targets. Guards, vehicles, loot and the other
// objects keep the original's gravity (their AI and the levels' puzzles are tuned for it).
//
// Character scale: the current player drawn scaled about its feet (the step's base point), the camera moved with it
// (its offset from the feet, the near / far planes, the fog and draw distances, a slightly wider view when small),
// and its collision hulls scaled about the feet up to 2x (beyond that the hulls stay at 2x so the levels stay
// passable). Speeds and jump heights stay the original's.

#include <glm/glm.hpp>

struct SW;
struct ALO;
struct CAMERA;
struct RENDEROPTS;

float GDebugGravity();
void SetDebugGravity(SW* psw, float g);
// the factor for this object's gravity: GDebugGravity() for a player character, else 1
float GDebugGravityOf(ALO* palo);

float SDebugCharScale();
void SetDebugCharScale(float s);
// every world step (debugmenu.cpp's UpdateDebugTools): the current player's collision hulls follow the scale
void UpdateDebugCharScale(SW* psw);
// scenesw.cpp: the drawing delta of an object of the current player's tree (false: none)
bool FDebugCharScaleDelta(ALO* palo, glm::mat4* pdmat, float* ps);
// cmworld.cpp: the drawn camera from the game's (CameraFromCm), and the fog (ApplyCmFog)
void ApplyDebugCharScaleCamera(CAMERA* pcam);
void ApplyDebugCharScaleFog(RENDEROPTS* popts);
