#pragma once
// game/ball3.h: Murray's Be The Ball (Sly 3's item 17, his state 22; s3-gadgets). His step entry becomes a rigid
// sphere of radius 75 that the solver rolls and bounces; the state's code is ported from the Sly 3 decomp
// (func_001E2AC8.cpp the contacts' sum, func_001E3488.cpp the frame, func_001E4250.cpp the motion, func_001E2A20.c,
// random_offset_apply_1E33E0.c, scalar_matrix_scale.cpp, actor_alternate_select.c the floor contact, func_001E1A68.cpp
// the contacts' friction) and from the disassembly of 0x1E2E88 (the mode's setter: no decomp source).
#include <glm/glm.hpp>
#include <memory>

#include "sound.h"

struct MURRAY;
struct ALO;
struct LO;
struct ASEG;
struct XP;
struct SOMESH;

namespace sly3
{
struct BALL3
{
    int mode = -1;                 // +0x1AD4: -1 off, 0 rolling, 1 pressed into a floor (the bounce is gathered), 2 ending
    float tMode = 0.0f;            // +0x1AD8
    std::shared_ptr<SOMESH> pmeshSaved;  // +0x1ADC: the step entry's moving mesh while the entry is a sphere
    bool fMeshSaved = false;
    ASEG* paseg = nullptr;         // +0x1AE4 (oid 560): the ball's clip, looping
    LO* ploPartner = nullptr;      // +0x1AE8 (oid 565): added while |vz| > 1250, removed 0.5 s after on a floor
    LO* ploTarget = nullptr;       // +0x1AEC (oid 566): faded in when fast (850 flat / falling 2000), out when slow
    LO* ploNear = nullptr;         // +0x1AF0 (oid 568): the burst of a bounce under 1350
    LO* ploFar = nullptr;          // +0x1AF4 (oid 569): of a harder one
    LO* ploBig = nullptr;          // +0x1AF8 (oid 570): of an impact over 2500
    LO* ploAnchor = nullptr;       // +0x1AFC (oid 571): of a jump (cross)
    XP* pxpHolder = nullptr;       // +0x1B00: the floor contact (normal z >= 0.7, the nearest; 0x1E1C68)
    int iHolder = 0;               // +0x1B04: his end of it
    float tHolder = -1000.0f;      // +0x1B08: when there last was one
    LO* ploHolder = nullptr;       // +0x1B0C: the other end's owner (the task event 0x5E's value: its oid natively)
    float tSpin = -1000.0f;        // +0x1B14: the last spin (square)
    float uBlend = 0.0f;           // +0x1B18: 1 at a spin, 0 half a second later
    float zHeight = -3.4028235e38f;  // +0x1B1C: the highest point reached (the bounces' ceiling)
    float sRoll = 0.0f;            // +0x1B20: the rolling sound's distance left
    CSndRef sndRoll;               // +0x1B24
    glm::vec3 vecGather{0.0f};     // +0x1B30: the contacts' velocity change gathered while pressed (mode 1)
};

void InitBall3(MURRAY* p);                 // the parts (table 0x345FD8's entries 107..113)
void SetBallMode3(MURRAY* p, int k);       // orig3 0x1E2E88
void UpdateBall3(MURRAY* p);               // orig3 0x1E3488 (the state's frame)
void UpdateBallMotion3(MURRAY* p);         // orig3 0x1E4250 (the state's motion, after the step's)
void OnBallXpNew3(MURRAY* p, XP* pxp);     // orig3 0x1E1A68 case 22 (friction / elasticity of a new contact)
bool FBallCanStart3(MURRAY* p);            // orig3 0x1E23F0 case 17
void StartBall3(MURRAY* p);                // orig3 0x1E25B0 case 17
}  // namespace sly3
