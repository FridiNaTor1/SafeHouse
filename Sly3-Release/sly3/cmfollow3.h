#pragma once

// Sly 3's follow camera policy (the world camera's policy on the m_ext beach), ported from the Sly 3 asm:
// the update 0x274AC0 (Sly 2's CPFOLLOW::Update 0x221DD8 in the same call order: cplcy.cpp is the draft), the focus
// record 0x1F3ED8 with its spring 0x1F3E48, the focus's raw record (Sly's vt slot 85 0x1ABA80 -> the actor's 0x153D40
// -> the step's 0x250690 -> the SO's 0x1F3D48, the jump's 0x1BD758), PosCmeFocus 0x273AB0, the zone path 0x274A50,
// the focus check 0x276510 (its eye-blocked cut), SetCmLookAt 0x166E48.
// Offsets in the comments are the original's (the policy record, the CPDEFI on its stack, the raw record).

#include <glm/glm.hpp>

#include <cstdint>

#include "../vecmat.h"

struct CM3;
struct JOYPS2;

namespace sly3
{
// The focus's raw record (0x80 bytes on 0x1F3ED8's stack, filled by the focus's vt slot 85)
struct CPDEFIRAW3
{
    glm::vec3 pos{0.0f};        // +0x00 (the object's position; z: the floor + 125 on the ground)
    glm::vec3 v{0.0f};          // +0x10 (VelXfPoint at the position)
    glm::vec3 dposFocus{0.0f};  // +0x20 (the target of the focus record's second spring)
    float dTilt = 0, dDist = 0; // +0x30 / +0x34 (added to the samples' tilt / distance)
    cmm::LM lmZ{0, 0};          // +0x38 (the z target's range about the object)
    cmm::LM lmZ2{0, 0};         // +0x40 (the focus z's range about the object)
    cmm::SMP smpZ{0, 0, 1};     // +0x48 (the z smoothing)
    float sAhead = 0;           // +0x54 (the look-ahead's length)
    float radHome = 0;          // +0x58 (behind the facing)
    float tMoveLast = 0;        // +0x5C
    int cfk = 0;                // +0x60
    float kSpring = 0, kDamp = 0;  // +0x64 / +0x68 (the look-ahead's spring)
    uint32_t grf = 0;           // +0x6C (1 moving: no lead, 2 keep the record, 4 keep z, 0x10 z from the record)
    int n70 = 0;                // +0x70
};

// The CPDEFI 0x1F3ED8 hands the policy (0x60 bytes at the update's sp)
struct CPDEFI3
{
    glm::vec3 pos{0.0f};        // +0x00 (the record's position + its offset + the look-ahead)
    glm::vec3 v{0.0f};          // +0x10 (the lead: the focus's velocity)
    glm::vec3 dposAhead{0.0f};  // +0x20
    glm::vec3 dposFocus{0.0f};  // +0x30
    float dTilt = 0, dDist = 0; // +0x40 / +0x44
    uint32_t grf = 0;           // +0x48 (bit 0: moving, bit 3: the player may not turn the camera)
    float radHome = 0;          // +0x4C
    float tMoveLast = 0;        // +0x50
    int cfk = 0;                // +0x54
};

// What the focus's slot 85 reads from the actor (Sly 0x34's fields; player3 fills it each frame)
struct ACTORCM3
{
    glm::vec3 pos{0.0f};        // vt +0x120's record +0x30 (the world position)
    glm::vec3 dirFacing{1, 0, 0};  // the facing record's x row (+0x50's record, its world rows)
    glm::vec3 vel{0.0f};        // VelXfPoint
    float wAng = 0;             // |AngVelAlo| (the rotation record +0x4C)
    int state = 0;              // +0x19E0
    bool fFloor = true;         // +0x218 (the step's floor contact)
    float zFloor = 0;           // +0x248 (the step's floor point z)
    float dzFocus = 125.0f;     // +0x1158
    float dzBody = -75.0f;      // +0x268 (the step's +0x260 z: the root above its foot, negated)
    float tMoveLast = 0;        // +0x1154
    int jstate = -1;            // +0x33C (the jump sub-object +0x320 +0x1C: -1 none)
    int jumpPhase = 0;          // +0x348 (the jump +0x28: 0 rising (moving), 2 / 3 z from the record)
    int jumpHold = 0;           // +0x45C (the jump +0x13C: the jump's z range)
    bool fFirst = false;        // +0x154 (the focus wants a reset)
};

// The zone's entry (ZONECME, the Sly 2 layout; the policy +0x24) and the zone (the policy +0x20; Sly 3 offsets)
struct CME3
{
    int cid = 0x2F;             // +0x00
    float dtPath = -400.0f;     // +0x04
    glm::vec4 acoef[3]{{0, 1, 0, 0}, {0, 1, 0, 0}, {80, 1, 0, 0}};  // +0x10
    cmm::LM alm[3]{{-3.40282347e38f, 3.40282347e38f}, {-3.40282347e38f, 3.40282347e38f}, {-3.40282347e38f, 3.40282347e38f}};  // +0x40
    float dxScreen = 0.0f;      // +0x58
    float dyScreen = -0.25f;    // +0x5C
    float uFocus = 0.5f;        // +0x60
    float uLead = 1.0f;         // +0x64
    float uSampleRate = 1.0f;   // +0x68
    glm::vec3 vecCoefY{1.0f};   // +0x70
    float rSpring = 1.0f;       // +0x80
    float rDamping = 1.0f;      // +0x84
    cmm::LM lmTilt{0.197396f, 0.197396f};  // +0x88
};
struct ZONE3
{
    int oid = -1;
    glm::mat3 mat{1.0f};        // the zone object's rows (its frame)
    glm::vec3 pos{0.0f};
    void* pcrv = nullptr;       // +0x108 (a path: not ported)
    int fPolar = 0;             // +0x11C
    float radPolarLimit = 0;    // +0x128
    int grf = 4;                // +0x12C (1 / 2 the samples, 4 the player may turn the camera)
    int cfk = -1;               // +0x134
    float radFOV = 1.1f;        // +0x13C
    int fNoAvoid = 0;           // +0x140
    void* ploLook = nullptr;    // +0x144 (not ported)
    CME3 cme;                   // +0x110's entry
};

struct CPFOLLOW3
{
    CM3* pcm = nullptr;         // +0x00
    ZONE3* pzone = nullptr;     // +0x20
    // +0x30 the focus record
    glm::vec3 posRec{0.0f};     // +0x30
    glm::vec3 dposSmooth{0.0f}; // +0x40
    glm::vec3 dposAhead{0.0f}, vAhead{0.0f};  // +0x50 / +0x60
    glm::vec3 dposFocus{0.0f}, vFocus{0.0f};  // +0x70 / +0x80
    float tRec = 0;             // +0x9C
    float radPan = 0, radTilt = 0, sDist = 0, dyScreen = 0;  // +0xA0..+0xAC
    int cpmode = 4;             // +0xB0
    int fManual = 0, fLeftSamples = 0;  // +0xB4 / +0xB8
    float dradPan = 0, dradTilt = 0;    // +0xBC / +0xC0
    float tManual = 0;          // +0xC4
    float sPath = 0, radPolar = 0;      // +0xC8 / +0xCC
    float radPan1 = 0, radTilt1 = 0, radPan2 = 0, radTilt2 = 0;  // +0xD0..+0xDC
    float tMoveLast = 0;        // +0xE0
    int fFirst = 0;             // +0xE8
    float uLead = 1.0f;         // +0xF0
    float dsDist = 0, ddyScreen = 0;    // +0xF4 / +0xF8
    glm::vec3 posEye{0.0f};     // +0x100
    float tVisible = 0, tClear = 0;     // +0x110 / +0x114
    float dradPanOffset = 0, dradTiltOffset = 0, ddradPanOffset = 0, ddradTiltOffset = 0;  // +0x118..+0x124
};

// The camera fields the policy reads (the class-0x05 camera: +0x138 / +0x13C the screen ranges, +0x144 the near
// clip, +0x2F4 the cut, +0x2F8 / +0x2FC the cut's angles)
struct CMSTATE3
{
    float xScreenRange = 0.8174f, yScreenRange = 0.6131f;
    float sNearClip = 50.0f;
    int fCut = 0;
    float radCutPan = 0, radCutTilt = 0;
};

struct CMCLOCK3
{
    float t = 0, dt = 0, dtReal = 0;   // 0x36BB98 +0x0 / +0x4 / +0x10
};

// The focus record (0x1F3ED8): fills the CPDEFI from the actor (fFirst: the first frame / a cut)
void GetFocusCpdefi3(CPFOLLOW3* p, const ACTORCM3& actor, bool fFirst, const CMCLOCK3& clk, CPDEFI3* pcpdefi);
// The update 0x274AC0 (the CPDEFI from GetFocusCpdefi3 inside)
void UpdateCpfollow3(CPFOLLOW3* p, const ACTORCM3& actor, CMSTATE3* pcms, const JOYPS2& joy, const CMCLOCK3& clk);
// The beach zone 34523 (m_ext) as the reference's RAM has it (TODO(sly3): the zone objects' options by the loader)
ZONE3* PzoneBeach3();
}  // namespace sly3
