#pragma once

// The transform records' runtime (Sly 2's module 0x22C818-0x231400 and the controllers of 0x12CE50-0x12E4A8): what
// moves an ALO's records (XF, lo.h). Each record has a list of controllers sorted by priority; the head one drives
// it by giving it a segment (a value or a Hermite spline over a parameter, e.g. an ASEGA's time) and a mode (snap
// to it, seek it with velocity limits, spring, ...). Once per frame UpdateAloXfs (0x22DE88) moves every record by
// its mode, then composes the world matrices.
//
// Part of the animation code; the physics code's SO update calls UpdateAloXfs where the
// original's 0x1F3980 calls 0x22DE88.

#include "alo.h"
#include "lo.h"

#include <glm/glm.hpp>

struct ASEGA;
struct TRKP;
struct TRKG;

// Record modes (orig +0x98; names from what 0x22DE88 does with them).
enum XFM : int16_t
{
    XFM_Static = 0,       // the matrix as it is
    XFM_Moving = 1,       // velocity (+ acceleration +0xF0) integrated; at rest -> 3
    XFM_MovingHold = 2,
    XFM_Rest = 3,
    XFM_Mirror = 4,       // minus the translation of the record +0xA8
    XFM_Snap = 5,         // the matrix is the segment's value at the parameter (animation)
    XFM_SnapHold = 6,     // the same; with a fixed target (state 2) -> 10
    XFM_Seek = 7,         // moves toward the target with velocity limits (blends into an animation)
    XFM_SeekHold = 8,
    XFM_SeekTimed = 9,    // arrives by the time +0xA8
    XFM_Arrived = 10,
    XFM_SeekAccel = 11,   // velocity changes limited
    XFM_Arrived11 = 12,
    XFM_Joint = 13,       // a two-bone joint (IK, 0x186A50)
    XFM_Joint14 = 14,
    XFM_Link = 15,        // an attachment (0x1604E8)
    XFM_Chain = 16,       // a chain segment (0x21CB18)
    XFM_Spline = 17,      // to +0xC0 (velocity +0xD0) by the time +0xA8
    XFM_Spring = 18,
    XFM_SpringRest = 19,
    XFM_Spin = 20,        // about +0xC0 at +0xD0 radians per second
    XFM_Hold21 = 21,
};

// A controller of a record (orig: 0x2C..0x80 bytes from the slot heap 0x3E123C, vtables 0x2CAF08.. by kind:
// slot 1 Start (becomes the head), 2 End (no longer the head), 3 Refresh (the head stays), 4 Fetch (the record's
// parameter left its segment: give it the next one), 6 Copy).
struct CTRL
{
    int kind = 0;           // +0x00
    ALO* palo = nullptr;    // +0x04 the record's object
    XF* pxf = nullptr;      // +0x08
    int prio = 0;           // +0x0C (higher first)
    CTRL* pnext = nullptr;  // +0x10
    int mode = 0;           // +0x14 the record's mode while it is the head
    virtual ~CTRL() = default;
    virtual void Start();    // orig 0x12CE98 for most kinds: the record's mode, then its parameters reset (0x22C840)
    virtual void End() {}    // orig 0x12CED8 (empty)
    virtual void Refresh() {}  // orig 0x12CF20 (empty)
    virtual void Fetch() {}  // orig 0x12CF28 (empty)
    // slot 5: the value / velocity it drives the record toward (null: not wanted); orig 0x12CF30: the record's
    // current value, no velocity
    virtual void Query(glm::vec4* ppos, glm::vec4* pvel, float g = 0.0f);
    bool FFirst() const;     // orig 0x12CEE0
};

// kind 1 (0x2CA820): the record rests at a value (the loaded one): orig 0x12D088 puts +0x20 into the segment.
struct CTRLPOS : CTRL
{
    int state = 2;              // +0x1C
    glm::vec4 pos{0.0f};        // +0x20
    void Fetch() override;      // orig 0x12D088
    void Query(glm::vec4* ppos, glm::vec4* pvel, float g = 0.0f) override;  // orig 0x12D0D8
};

// kind 0 (0x2CAF08): an ASEGA's channel driving the record (asega.cpp).
struct CTRLASEG : CTRL
{
    TRKP* ptrkp = nullptr;      // +0x1C
    TRKG* ptrkg = nullptr;      // +0x20
    ASEGA* pasega = nullptr;    // +0x24
    CTRLASEG* pnextAsega = nullptr;  // +0x28
    void Start() override;      // orig 0x131C40
    void End() override;        // orig 0x131C90
    void Fetch() override;      // orig 0x131CD8
    void Query(glm::vec4* ppos, glm::vec4* pvel, float g = 0.0f) override;  // orig 0x131D68 (the track at the ASEGA's time)
};

// The loaded jobs' controllers (kinds 2 / 4 / 9 from 0x22D7A0's flags 8 / 0x10 / 0x20; lookat.cpp, xf.cpp).
struct CTRLJOB : CTRL
{
    XFJOB job;
};

// kind 2 (0x2CA7E0, 0x12D220): the record turns toward a target (an object by oid, else the player) within a range:
// pan / tilt of the direction through two clamped polynomials; out of range it rests at its loaded value and the
// controller takes its other priority.
struct CTRLLOOKAT : CTRLJOB
{
    glm::vec4 posRest{0.0f};    // +0x20
    LO* plo = nullptr;          // +0x70 the target
    bool fPrev = false;         // +0x74 a previous value (the velocity's finite difference)
    void Start() override;      // orig 0x12CE98
    void End() override;        // orig 0x12D210
    void Fetch() override;      // orig 0x12D220
    void Query(glm::vec4* ppos, glm::vec4* pvel, float g = 0.0f) override;  // orig 0x12D4D8
};

// kind 4 (0x2CA7A0, 0x12D508): the record looks along the motion of the translation record up its chain
struct CTRLLOOKMOTION : CTRLJOB
{
    XF* pxfMover = nullptr;     // +0x24
    void Fetch() override;      // orig 0x12D508
};

// kind 8 (0x2CA6E0): a fixed target with velocity and an arrival time (0x12E010: the segment {+0x20, +0x30}, state 2,
// +0xA8 the time)
struct CTRLTARGET : CTRL
{
    float tArrive = 0.0f;       // +0x1C
    glm::vec4 pos{0.0f};        // +0x20
    glm::vec4 vel{0.0f};        // +0x30
    void Fetch() override;      // orig 0x12E010
    void SetGoal(const glm::vec4& newPos, const glm::vec4& newVel);  // orig 0x12E0F0
};

// kind 5 (0x2CA760): forwards to a source controller (+0x10) and adds an offset (+0x20) to its segment
struct CTRLSOURCE : CTRL
{
    CTRL* psrc = nullptr;       // retained native layout field; original +0x10 uses inherited pnext
    CTRL* psrcCur = nullptr;    // +0x1C
    glm::vec4 ofs{0.0f};        // +0x20
    void Start() override;      // orig 0x12D858
    void End() override;        // orig 0x12D890
    void Refresh() override;    // orig 0x12D8D8
    void Fetch() override;      // orig 0x12D958
    void Query(glm::vec4* ppos, glm::vec4* pvel, float g = 0.0f) override;  // orig 0x12D9D0
};

// The controllers other code makes (the camera's / actors' actuators):
// orig 0x0012DC18: a kind-1 controller (mode, state, position) added to the record
CTRLPOS* PctrlNewPos(ALO* palo, XF* pxf, int prio, int mode, int state, const glm::vec4& pos);
// orig 0x0012DE58: a kind-8 controller (mode 0x11) added to the record
CTRLTARGET* PctrlNewTarget(ALO* palo, XF* pxf, int prio, float tArrive, const glm::vec4& pos, const glm::vec4& vel);
// orig 0x0012DBB0: a kind-5 controller (mode -1) added to the record
CTRLSOURCE* PctrlNewSource(ALO* palo, XF* pxf, int prio);
// orig 0x0012D108: the position moves; the record follows when the controller drives it
void SetCtrlPos(CTRLPOS* pctrl, const glm::vec4& pos);
// orig 0x0012DA30: the offset changes; the record's segment moves by it when the controller drives it
void SetCtrlSourceOffset(CTRLSOURCE* pctrl, const glm::vec4& ofs);
// orig 0x0012CFE0 / 0x0012D020: added / taken out unless it already is / is not in its record's list
void EnsureCtrlAdded(CTRL* pctrl);
void EnsureCtrlRemoved(CTRL* pctrl);
// orig 0x0012E460: removed and freed (*ppctrl cleared)
void DeleteCtrl(CTRL** ppctrl);

// The player (orig 0x1C4AF8): kind 2's default target. Null hook: the first Sly / Murray / Bentley object.

// orig: 0x00230018: adds pctrl to its record's list by priority; a new head is started (the old one ended),
// otherwise the head is refreshed
void AddXfCtrl(ALO* palo, XF* pxf, CTRL* pctrl);
// orig: 0x00230100: removes it (the next one becomes the head)
void RemoveXfCtrl(ALO* palo, XF* pxf, CTRL* pctrl);
// orig: 0x002301D0: re-sorts the list by priority
void SortXfCtrls(ALO* palo, XF* pxf);
bool FXfHasCtrl(const XF* pxf, const CTRL* pctrl);  // orig 0x12D058

// orig: 0x0022C818: the parameter defaults (+0x80 = 0, +0x84 = 1, pointers to them)
void ResetXfParams(XF* pxf);
// orig: 0x0022C840: the parameters out of range (unless the mode keeps them), state 0; the object marked (vt 47)
void InvalidateXfParams(ALO* palo, XF* pxf);
// orig: 0x002303A0: the record's mode (entering a moving mode: the velocity of the current motion)
void SetXfMode(ALO* palo, XF* pxf, int mode);
// orig: 0x00230598 / 0x00230630: the hold / release counterparts of the current mode
void HoldXfMode(ALO* palo, XF* pxf);
void ReleaseXfMode(ALO* palo, XF* pxf);
// orig: 0x0022DD00: the parameter left the segment (or a state >= 3): the head controller fetches the next one
void RefreshXf(ALO* palo, XF* pxf);

// orig: 0x0022C8C0: the record's local matrix with the value v put in by its kind (translation, Euler rotation,
// the axis entries of kinds 11 / 12)
glm::mat4 MatXfWithValue(const XF* pxf, const glm::vec4& v);
// orig: 0x0022CA18: the value of the record's kind in the matrix m
glm::vec4 VecXfValue(const XF* pxf, const glm::mat4& m, const glm::vec4& vDefault = glm::vec4(0.0f));
// orig: 0x0022CDC0: the record's derivative matrix (its motion)
glm::mat4 DmatXf(const XF* pxf);
// The segment's value / slope at the record's parameter (the 'value' / 'deriv' by state of 0x22DE88)
glm::vec4 VecXfTarget(const XF* pxf);
glm::vec4 VecXfTargetSlope(const XF* pxf);

// orig: 0x0022DE88: every record of the object by its mode, then its world matrix (t, dt: the clock's, orig
// 0x2DDED0 {t, dt})
void UpdateAloXfs(ALO* palo, float t, float dt);
// orig: 0x0022FAA0: the records' world matrices (parent's world x local)
void UpdateAloXfWorlds(ALO* palo);
// (QoL draw distance above 1x, drawdist-2) the record's local matrix dt seconds on, for drawing only (xf.cpp)
bool FXfVisualPose(const XF* pxf, float dt, glm::mat4* pmat);

// The records' state after the load (the parts of 0x22D7A0 after each record is read: the value into the segment,
// 0x22C818, the jobs' controllers) and the bone binding of the object's meshes (PostLoad 0x1F36A8: 0x22FAA0,
// 0x167B68). Called by the anim post-load pass (anim.cpp) for every ALO.
void InitAloXfs(ALO* palo);

// The change counter (orig D_003DD490 + 0x3D58).
extern int g_cXfStamp;
