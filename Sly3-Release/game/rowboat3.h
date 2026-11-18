#pragma once

// Sly 3's rowboat (class 0x53, orig3 0x20ED28..0x20FEF0; game/rowboat3.cpp; notes/PLANE3_2026-11-12.md "Round 4"):
// m3_heist's inflatable boat (h_ext's proxy 93589, Murray rowing it, tasks 3281 / 3285 / 3287).

#include <glm/vec3.hpp>

struct LO;
struct ALO;

// a rowboat (class 0x53)
bool FRowboat3(const LO* plo);
// po.cpp's enable (orig3 vt slot 83 0x20F738): 1 rowed (a pad has it), 0 idle
void OnRowboatEnable3(LO* plo, int n);

// water.cpp's contact (orig3 vt slot 76 0x20EEF0) reads these (the record's +0x190..+0x1B8)
struct BOATWATER3
{
    glm::vec3 bias{0.6f};                        // +0x190: the still water's linear drag
    glm::vec3 damp{0.001f, 0.01f, 0.025f};       // +0x1A0: the drag by the speed per local axis
    float dragBase = 1.0f, dragSpeed = 3.0f;     // +0x1B0 / +0x1B4: the turn's drag
    float turnGain = 20.0f;                      // +0x1B8: the righting toward the surface's normal
};
bool FBoatWater3(const LO* plo, BOATWATER3* pbw);
// (the harness, route.cpp's "row TASK N" step) the stick (pad units) and a stroke for the current player's boat
bool FRowStick3(LO* plo, int oidTask, float* px, float* py, bool* pfStroke);

// Murray's boat state 23 (orig3 0x1DE690 case 23; EOPID 1152's setter 0x1E47F0, the clip 0x1E4900, the update 0x1E49B8)
struct MURRAY;
void SetMurrayBoat3(MURRAY* p, LO* pboat);
void SetMurrayRowClip3(MURRAY* p, int mode);
void EnterMurrayRow3(MURRAY* p);
void LeaveMurrayRow3(MURRAY* p);
void UpdateMurrayRow3(MURRAY* p);
void OnMurrayRowEvent3(MURRAY* p, int ev);   // his clip events 0x247 / 0x248
