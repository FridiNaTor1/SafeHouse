#pragma once

// The stepping objects (STEP, cid 0x33, module 0x203948..0x205E70; Sly 1's STEP): the walking characters' base
// (the players 0x34.., the guards 0x38.., 0x44): a heading and its rotation, the velocity the feet push toward
// (+0x190, the object's frame), the floor contact (+0x208, an XP of kind 0xE the floor test makes), the motion
// modes of vt slot 17's physics (0x204108).
//
// Native: the fields live in STEP (by object, at the original's offsets; ACTOR / SLY derive from it), made on first
// use (stepphys.h StepPhysOf: 0x203948's defaults). The physics parts of the module (the custom entry, the floor
// contacts, slots 54 / 55 / 57 / 58 / 60 / 70 / 85, the heading / scale / size setters, the turn gain) are the physics
// code's stepphys.*; this file has the rest (the update 0x203FC0, slots 29 / 30).

#include "po.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

struct ALO;
struct LO;
struct XP;
struct SW;

struct STEP
{
    virtual ~STEP() = default;
    ALO* palo = nullptr;

    float uTurn = 1.0f;             // +0x180 the gain of the turn toward the heading (0x203FC0)
    glm::vec3 vecVel{0.0f};         // +0x190 the velocity the feet push toward (the heading's frame)
    int nMotion = 0;                // +0x1A4 slot 17's motion: 0 braked toward +0x1F4..+0x1FC, 1 toward +0x1E0, -1 none
    float radHeading = 0.0f;        // +0x1A8
    glm::mat3 matHeading{1.0f};     // +0x1B0 (rows +0x1B0 / +0x1C0 / +0x1D0: glm columns)
    glm::vec4 posTarget{0.0f};      // +0x1E0 (mode 1)
    float gScale = 1.0f;            // +0x1F0 (0x204D48)
    glm::vec3 vBrake{0.0f};         // +0x1F4 the braking speeds (0x2C3CB8 scaled)
    float uBrake = 1.0f;            // +0x200
    int fNoContacts = 0;            // +0x204 (slot 85: 0 when set)
    XP* pxpFloor = nullptr;         // +0x208 the floor contact
    float tFloor = 0.0f;            // +0x20C when it was last there
    float sFloorTarget = 0.0f;      // +0x210 the floor contact's target height
    float g214 = -50.0f;            // +0x214
    int iposProbe0 = -1;            // +0x218 the floor probe at the base (a world point of the custom entry: index)
    int iposProbe1 = -1;            // +0x21C the one +0x214 below it (slot 85 flag 2)
    float g220 = 30.0f;             // +0x220
    glm::vec4 posFloor{0.0f};       // +0x230 the floor point (0x204768 / 0x2041C0)
    glm::vec4 vecBase{0.0f};        // +0x240 the object's base in its frame (0x2039C0; Sly 0x1885B8: (0, 0, -75))
    int isoeCustom = -1;            // +0x258 the custom entry (the floor probes; an index in the ALO's entries)
    int isoeOther = -1;             // +0x25C a second mesh (0x203D00)
    float sSize = 1.0f;             // +0x260 (0x204DA8)
    int oidMesh = -1;               // +0x264 the entry (by tag) the custom entry is made from
    int isoeSource = -1;            // +0x268 that entry (turned off)
};

STEP* PstepFromLo(LO* plo);          // null outside cid 0x33 or before its first use
STEP* PstepNew(ALO* palo);           // the object's native data by its class (STEP, ACTOR, SLY), uninitialized
void ResetSteps();
void CopyStep(ALO* palo, const STEP* psrc);  // a clone's STEP part (CloneStepPhys)

// orig: 0x00204740 (the heading of the object's record: atan2 of its matrix's x row)
float RadStepHeading(STEP* pstep);
// orig: 0x00203FC0 (slot 16's step part: the ALO's update 0x1F36E0, the meshes' velocities 0x203E08, the turn gain)
void UpdateStep(STEP* pstep);
void PrintStepDebug(STEP* pstep);  // (debug: SLY2N_STEPDBG=oid)
// orig: 0x002041C0 (slot 29: the position, then +0x230 the base's world point)
void SetStepPos(STEP* pstep, const glm::vec3& pos);
// orig: 0x002041F8 (slot 30: the rotation, the heading from the record, the controller's +0x18 copy of it)
void SetStepRotation(STEP* pstep, const glm::mat3& mat);
