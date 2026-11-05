#pragma once

// The reach (orig module 0x1CC158..0x1CD2C0, 0xB0 bytes at Sly +0x17D0, the +0x4C0 sub-object's +0x440 and the gang's
// +0x900): two clips of a limb (Sly: the cane held out) blended so that an end object (Sly: the cane +0x19A4) comes
// nearest a target. The clips' paths of the end are sampled at the post-load (0x1CC5E8: 9 points over each clip, in
// the actor's frame); each frame (0x1CC2E8) the clip time u and the blend weight w are chosen by the target's distance
// to the segment between the two paths' points at u (0x11D9E0 over u, 0x11EE08 for w), the clip's animation is set
// to u and the actor turns toward the target.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct ACTOR;
struct ASEG;
struct ASEGA;
struct ASEGBL;
struct LO;
struct XF;
struct CTRL;

struct REACH
{
    ACTOR* pa = nullptr;            // +0x0
    ASEGA** ppasega = nullptr;      // +0x4 the animation the blend plays on (Sly: +0x18E0)
    float aw[2]{1.0f, 0.0f};        // +0x8 / +0x1C the blend's weights
    ASEG* apaseg[2]{};              // +0xC / +0x20 the clips
    std::vector<glm::vec3> aposEnd[2];   // +0x14 / +0x28 the end's paths (the actor's frame; uniform in u)
    ASEGBL* pbl = nullptr;          // +0x30
    LO* ploEnd = nullptr;           // +0x34 the end object
    LO* ploTarget = nullptr;        // +0x84 the target
    float sTarget = 0.0f;           // +0x88 its distance (a curve target)
    LO* plo8C = nullptr;            // +0x8C
    int nMode = -1;                 // +0x90 0 a curve's nearest point, 1 an object, 2 an object's frame
    float t94 = 0.0f;               // +0x94 the grab's timed move (0x12DE58 by 0x1CD308): its end time (+0x1C)
    CTRL* pctrl94 = nullptr;         // +0x94 / +0x98 / +0x9C: grab flight and held local pose
    CTRL* pctrl98 = nullptr;
    CTRL* pctrl9C = nullptr;
    LO* ploHeld = nullptr;          // +0xA0
    int state = -1;                 // +0xA4
    float tState = 0.0f;            // +0xA8
};

// orig: 0x001CC158 + 0x001CC5E8 (the parts: the clips at ibClip0 / ibClip1; the end object (Sly: +0x1804 = +0x19A4, the
// cane); its paths sampled)
void BindReach(REACH* preach, ACTOR* pa, ASEGA** ppasega, uint32_t ibClip0, uint32_t ibClip1, LO* ploEnd);
// orig: 0x001CCAC0 (-1 off, 0 / 1 on the target ploTarget)
void SetReachState(REACH* preach, int n);
// orig: 0x001CC2E8
void UpdateReach(REACH* preach);
void StartReachGrab(REACH* preach, LO* plo);  // orig 0x1CCC88
void AttachReachObject(REACH* preach, LO* plo);  // orig 0x1CD098
void ReleaseReachObject(REACH* preach);  // orig 0x1CD2C0
