#pragma once

// The stepping objects' physics (class 0x33, STEP: the base of the actors 0x34 / Sly 0x35 / Murray 0x36 / Bentley 0x37
// and the guards 0x38..; Sly 2's module 0x203948-0x205E70): the custom entry (+0x258) whose appended points probe the
// floor, the floor contacts (kind 14: the own test, vt slot 60 0x204AC8), their height spring (slot 57 0x2044B0), the
// lowest one kept as the floor (+0x208 / +0x20C / +0x230, slot 58 0x204768), the push-apart contacts (slot 55
// 0x204250), the floor's neighbourhood test 0x2048C8, the forgetting of a floor (slot 54 0x204C60), slot 85
// (0x2040F8), and slot 70 (0x204108: gravity, then braking in the heading frame / the turn toward +0x1E0).
// Reference: Sly2-Reference/Sly2-Release/game/m_2040F8.cpp, m_203948_p3.cpp (oracle-verified), the decomp's func_002048C8.
//
// The STEP fields (+0x180..+0x268) are step.h's STEP (the actors' data derives from it). The custom entry +0x258 /
// +0x25C / +0x268 are entry indices and the probes +0x218 / +0x21C point indices of the custom entry's world
// geometry (natively GEOM points are vec3, not the original's qwords).

#include "step.h"

#include <glm/glm.hpp>

struct ALO;
struct LO;
struct SOE;
struct XP;
struct BSPREF;

using STEPPHYS = STEP;  // (step.h)

STEPPHYS* PstepPhysOf(ALO* palo);          // null unless palo is a class 0x33 object
STEPPHYS& StepPhysOf(ALO* palo);           // made on first use (0x203948's defaults)
bool FAloStep(const LO* plo);        // class 0x33 or derived
void ResetStepPhysAll();                // the world's unload
void CloneStepPhys(ALO* palo, const ALO* paloSrc);

void SetStepPhysScale(ALO* palo, float g);     // 0x204D48
void SetStepPhysHeading(ALO* palo, float rad);      // 0x204660
void SetStepPhysHeadingToward(ALO* palo, const glm::vec3& dir);  // 0x2046B0
void SetStepPhysSize(ALO* palo, float s);           // 0x204DA8

// The load / post-load parts (the world's InitLo / load / PostLoad dispatch, or the classes' overrides, call them):
void LoadStepPhysBase(ALO* palo);                   // 0x2039C0's part: +0x240 = the object's origin in its parent's frame
void PostLoadStepPhys(ALO* palo);               // 0x203A38's part: records +0x58 / +0x5C in mode 1, the custom entry
void MakeStepCustomEntry(ALO* palo, SOE* psoeSrc);  // 0x203AF0
// The second mesh (+0x25C): set (0x203D00), followed by the custom entry's point velocities each update (0x203E08),
// the players' choice by state (Sly's slot 90 0x18C060)
void SetStepMesh(ALO* palo, int isoe, bool fSnap);
void UpdateStepMesh(ALO* palo);
void SetPlayerStepMeshForState(ALO* palo, int state);
// The update's part (0x203FC0 after the ALO base 0x1F36E0): the turn's gain +0x180, the floor's time
void UpdateStepPhysTurn(ALO* palo);

// The class's physics slots (registered by original address; the actors' / guards' overrides call these as their base):
void SetStepOverlap(ALO* palo, ALO* paloOther);              // 54: 0x204C60
void OnStepXpNew(ALO* palo, XP* pxp, int i);                 // 55: 0x204250
void OnStepXpUpdate(ALO* palo, XP* pxp, int i);              // 57: 0x2044B0
void FindStepFloor(ALO* palo);                               // 58: 0x204768
void AddStepFloorXps(ALO* palo, SOE* psoeOther, int inodeLimit, const BSPREF& tree, int root, XP** ppxp);  // 60: 0x204AC8
void UpdateStepPhysMotion(ALO* palo);                            // 70: 0x204108
uint32_t GrfStepXp(ALO* palo);                               // 85: 0x2040F8 (1 floor probes, 2 the long probe, 4
                                                             // the floor contacts solved, 8 the normal kept)
// 0x2048C8: whether a face reachable over the edges near pos from isurf (by the height the step can climb) is a
// floor (normal z > 0.7)
bool FStepFloorNear(ALO* palo, const glm::vec3& pos, const SOE* psoe, int isurf);

// Sly's (0x35) overrides that are physics alone (the others read the player state: the actors port)
void OnSlyXpUpdate(ALO* palo, XP* pxp, int i);                // 57: 0x188E78

// orig: 0x00205910: fit the heading frame to the floor below three local probe points.
void FitStepToFloor(ALO* palo, float gMin, float gMax, int cpos, const glm::vec3* apos);
