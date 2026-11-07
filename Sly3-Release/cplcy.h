#pragma once

// CPLCY, the camera policies (Sly 1's cplcy.c and cp*.c): the camera (cm.h) holds one of each kind (the original's
// CM +0x3A0..+0x630) and stacks them by priority with a focus object and an argument. Each kind is known by its
// constructor and vtable (the release build has no RTTI); slots: 1 init, 2 activate, 3 deactivate, 4 set,
// 5 revoke, 6 update, 7 reset.
//
// Ported: the default policy (0x3A0), the follow camera of the zones (0x3B0, 0x2219C8..0x2236E8: the gameplay
// camera), the camera objects' view (0x4A8, 0x12E798..0x12E858: cutscene / briefing cameras), the free camera
// (0x520, 0x150938..0x150EF0), and the actuated under-table view (0x4C0, 0x151240..0x151340).

#include "cm.h"
#include "game/sly3.h"
#include "sound.h"

#include <array>
#include <cstdint>

struct LO;

struct CPLCY
{
    LO* psoFocus = nullptr;        // +0x0 (the base activate's focus)
    float radFOVTargetSave = 1.0f; // +0x4
    float sNearClipSave = 100.0f;  // +0x8
    // +0xC the vtable
    virtual ~CPLCY() = default;
    virtual uint32_t VtOrig() const { return S3VA(0x2CD570); }
    // slot 1 (0x14FCC0: nothing)
    virtual void Init() {}
    // slot 2 0x14FCC8 (saves the camera's field of view target and near clip)
    virtual void Activate(LO* psoFocus, void* pv);
    // slot 3 0x14FCE8 (restores them)
    virtual void Deactivate(LO* psoFocus, void* pv);
    // slots 4 / 5 (0x14FD30 / 0x14FD38: nothing)
    virtual void Set(void* pv) { (void)pv; }
    virtual void Revoke(void* pv) { (void)pv; }
    // slot 6 (0x14FD40: nothing)
    virtual void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) { (void)pcpdefi; (void)pjoy; }
    // slot 7 (0x14FD48: nothing)
    virtual void Reset() {}
};

// orig: 0x0014FD50 (is it the policy on top of the camera's stack)
bool FActiveCplcy(const CPLCY* pcplcy);

// The default policy (CM +0x3A0, vtable 0x2CD528): does nothing (the camera stays where it is).
struct CPDEFAULT : CPLCY
{
    uint32_t VtOrig() const override { return S3VA(0x2CD528); }
};

// A zone's camera entry (0x90 bytes; the zone's +0x114 array, chosen by the focus's class: +0x0).
struct ZONECME
{
    int cid = 0x30;                 // +0x00 (the focus must derive from it)
    float dtPath = -450.0f;         // +0x04 (the path parameter's offset of the two samples: -/+)
    std::array<glm::vec4, 3> acoef{};  // +0x10 (three quadratics c0 + t (c1 + t c2) of the zone coordinates)
    std::array<cmm::LM, 3> alm{};        // +0x40 (their limits)
    float dxScreen = 0.0f;          // +0x58 (the focus's screen offset: x)
    float dyScreen = -0.1f;         // +0x5C (y)
    float uFocus = 1.0f;            // +0x60 (the focus point between the base (0) and the adjusted (1) positions)
    float uLead = 1.0f;             // +0x64 (the passive pan's gain while the focus moves)
    float uSampleRate = 1.0f;       // +0x68 (the speed of the turn toward the samples)
    glm::vec3 vecCoefY{1.0f};       // +0x70 (the coefficients' y lanes of the focus point (0x220DD8))
    float rSpring = 1.0f;           // +0x80 (the distance / screen offset spring)
    float rDamping = 1.0f;          // +0x84
    cmm::LM lmTilt{-3.40282347e38f, 3.40282347e38f};  // +0x88
};
// orig D_002C4120 (the entry when the zone has none)
ZONECME ZonecmeDefault();
const ZONECME* PcmeDefault();

// A ZONE's camera parameters (the original's ZONE object +0x10C..+0x1B8; the zone is the follow policy's argument).
struct ZONECM
{
    LO* pzone = nullptr;            // the ZONE object (its transform; null: the world's frame)
    LO* pcrv = nullptr;             // +0x10C (a PATH the camera follows)
    int crvk = 0;                   //   its +0xE0 (0 linear, 1 cubic)
    int ccrvPoint = 0;              //   its +0xE8
    std::vector<ZONECME> acme;      // +0x110 / +0x114
    ZONECME* pacmeShared = nullptr; //   (instead of acme: one entry in the ELF's data, 0x194118's D_002BF670)
    int fPolar = 0;                 // +0x120 (the coefficients are polar: angle, radius, z)
    int fArc = 0;                   // +0x12C (an arc of radius +0x160 about +0x150 instead of a path)
    float radPolarLimit = 0.0f;     // +0x130 (the polar angle's change per frame)
    float tArc = 0.0f;              // +0x138
    float sPath = 0.0f;             // +0x140 (the focus's parameter along the path / arc)
    glm::vec3 posArc{0.0f};         // +0x150
    float sArcRadius = 0.0f;        // +0x160
    int grf = 4;                    // +0x164 (1: sample 1, 2: sample 2, 4: the player may turn the camera)
    int fReturn1 = 0;               // +0x168
    int fReturn2 = 0;               // +0x16C
    float dtReturn1 = 0.0f;         // +0x170
    float uReturn1 = 0.0f;          // +0x174
    float dtReturn2 = 0.0f;         // +0x178
    float uReturn2 = 0.0f;          // +0x17C
    int cfk = -1;                   // +0x180 (-1: the focus's own)
    int fCutOnActivate = 0;         // +0x184
    float radFOV = 0.0f;            // +0x188 (0: keep)
    int fNoAvoid = 0;               // +0x18C (no collision avoidance / visibility checks)
    LO* ploLook = nullptr;          // +0x190 (an object the camera leads toward)
    float alead[3]{};               // +0x1A0 (its lead's quadratic of the distance)
    cmm::LM lmLead{0.0f, 0.0f};          // +0x1B0
};

// The focus object's fields the follow camera reads (a JT-derived player; the original's offsets).
struct CMFOCUS
{
    bool fJt = false;               // FIsBasicDerivedFrom(focus, 0x34)
    int jtfock = 0;                 // +0x4F4
    glm::vec3 posFocus{0.0f};       // +0x500 (the point jtfock 2 focuses on)
    int n4C8 = 0, n4CC = -1;        // +0x4C8 / +0x4CC
    int n10E4 = 0;                  // +0x10E4 (4: the faster recentring)
    const void* pvZoneCur = nullptr;  // +0x528 (the zone the player is in; compared with the policy's)
    glm::mat3 mat{1.0f};            // 0x1A6318: its world matrix
};

struct CMATTACH;

// What the follow camera needs from other modules besides g_cmworld (curves, transforms, the focus).
struct CMFOLLOWWORLD
{
    bool (*pfnFocus)(LO* pso, CMFOCUS* pfocus) = nullptr;
    // 0x1E6F10: the position / direction at distance s along the curve
    void (*pfnCrvPos)(LO* pcrv, float s, glm::vec3* ppos, glm::vec3* pdir) = nullptr;
    // 0x1E7380: the curve parameter nearest to pos, searching from s
    float (*pfnCrvNearest)(LO* pcrv, const glm::vec3& pos, float s) = nullptr;
    // 0x1E6F60: the curve parameter nearest to pos (the whole curve)
    float (*pfnCrvNearestAll)(LO* pcrv, const glm::vec3& pos, float s) = nullptr;
    // 0x1A6F00 / 0x1A6FC0: a point / a direction from a's frame (null: the world) to b's
    glm::vec3 (*pfnConvertPos)(LO* a, LO* b, const glm::vec3& v) = nullptr;
    glm::vec3 (*pfnConvertVec)(LO* a, LO* b, const glm::vec3& v) = nullptr;
    // 0x2236E8 (the focus's visibility check; may cut the camera) / 0x223A70 (the eye avoiding the world on a cut) /
    // 0x14F110 (the eye's move avoiding the world): overrides of the native versions (the checks replay the
    // reference's results through these)
    bool (*pfnVisibility)(void* pcplcy) = nullptr;
    bool (*pfnEyeCut)(void* pcplcy, const glm::vec3& posFocus, const glm::vec3& posEye, glm::vec3* pposOut,
                      bool* pfBlocked) = nullptr;
    bool (*pfnEyeMove)(const glm::vec3& posFrom, const glm::vec3& posTo, glm::vec3* pposOut) = nullptr;
    // the attached view's actor: its mount's world matrix (+0x1D0's vt slot 31), +0x1D4, +0x1BC
    bool (*pfnAttach)(LO* pso, CMATTACH* pattach) = nullptr;
    // the "invert the look" setting (a progress key: 0x2581C0 of G_GAME +0x3B4C)
    bool (*pfnFInvertLook)() = nullptr;
    // 0x1619A0(0x2DEB30, 6, 0) (the game's "the camera was turned" event) and the turn sound (0x1FBB90(3))
    void (*pfnCameraTurned)() = nullptr;
    void (*pfnTurnSound)() = nullptr;
};
extern CMFOLLOWWORLD g_cmfollowworld;

// The follow camera of the zones (CM +0x3B0, vtable 0x2DB858, the module 0x220D40): the gameplay camera.
// Its argument is the zone's ZONECM (null: the defaults).
struct CPFOLLOW : CPLCY
{
    ZONECM* pzone = nullptr;        // +0x10
    const ZONECME* pcme = nullptr;  // +0x14
    float radPan = 0, radTilt = 0, sDist = 0;  // +0x18 / +0x1C / +0x20 (the eye about the focus)
    float dyScreen = 0;             // +0x24 (the smoothed screen offset)
    int fManual = 0;                // +0x28 (returning toward +0x2C / +0x30)
    float radPanReturn = 0, radTiltReturn = 0;  // +0x2C / +0x30
    int cpmode = 0;                 // +0x34 (1: sample 1, 2: sample 2, 4: free / passive)
    int fFirst = 0;                 // +0x38 (activated this frame)
    int fLeftSamples = 0;           // +0x3C
    float dradPan = 0, dradTilt = 0;  // +0x40 / +0x44 (the turn's speeds)
    float tManual = 0;              // +0x48 (the last turn by the player)
    float radPolar = 0;             // +0x4C
    float radPan1 = 0, radTilt1 = 0, radPan2 = 0, radTilt2 = 0;  // +0x50..+0x5C (the samples last frame)
    float tMoveLast = 0;            // +0x60
    float uLead = 0;                // +0x64
    float dsDist = 0, ddyScreen = 0;  // +0x68 / +0x6C (the springs' speeds)
    glm::vec4 posEye{0.0f};         // +0x70 (the eye last frame)
    float tVisible = 0;             // +0x80 (0x2236E8: the focus last seen)
    float dradPanOffset = 0, dradTiltOffset = 0;  // +0x84 / +0x88 (the activation's offset, decaying)
    float ddradPanOffset = 0, ddradTiltOffset = 0;  // +0x8C / +0x90

    uint32_t VtOrig() const override { return 0x2DB858; }
    void Init() override;                                 // 0x2219C8
    void Activate(LO* psoFocus, void* pv) override;       // 0x221A18
    void Deactivate(LO* psoFocus, void* pv) override;     // 0x221CC0
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x221DD8
};

// PC addition (debug menu only: a guest character's camera, pccharacter.cpp): the follow camera's distance times
// g_uPcCmDist while its focus is g_ploPcCmDist (null: off, normal play)
extern LO* g_ploPcCmDist;
extern float g_uPcCmDist;
// (and its aim mode: the follow camera's pan held, only the player turns it)
extern bool g_fPcCmHoldPan;

// The camera objects' view (CM +0x4A8, vtable 0x2CAB90): the camera at a camera object (cid 0x2C), looking at its
// target or along its matrix, with its field of view.
struct CMOBJ  // what the policy reads of the camera object
{
    LO* plo = nullptr;
    LO* ploTarget = nullptr;   // +0xE0
    int fCut = 0;              // +0x120 (cut the camera when it ends)
    float sFocal = 0.0f;       // +0x128 (the lens: fov = 2 atan(17.996 / sFocal))
    int cFocal = 0;            // +0x12C (the lens is set)
    float radFOV = 0.0f;       // +0x130 (0: keep)
};
struct CPLOOKAT : CPLCY
{
    CMOBJ* pcmobj = nullptr;   // +0x10
    uint32_t VtOrig() const override { return S3VA(0x2CAB90); }
    void Activate(LO* psoFocus, void* pv) override;       // 0x12E798
    void Deactivate(LO* psoFocus, void* pv) override;     // 0x12E7F0
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x12E858
};

// The free / scripted camera (CM +0x520, vtable 0x2CD498).
struct CPFREE : CPLCY
{
    LO* ploFocus = nullptr;    // +0x10
    LO* ploTarget = nullptr;   // +0x14
    float uZoomTarget = -1.0f; // +0x18
    glm::vec4 posFocus{0.0f};  // +0x20
    glm::vec4 posEye{0.0f};    // +0x30
    int fScripted = 0;         // +0x40
    glm::vec4 posScript{0.0f}; // +0x50
    glm::vec4 eulScript{0.0f}; // +0x60
    float radFOVScript = 0;    // +0x70
    glm::vec4 posScriptTarget{0.0f};  // +0x80
    glm::vec4 eulScriptTarget{0.0f};  // +0x90
    float radFOVScriptTarget = 0;     // +0xA0
    float radPan = 0, radTilt = 0;    // +0xA4 / +0xA8
    float uZoom = 0, duZoom = 0;      // +0xAC / +0xB0
    float sZoom = 7.0f;               // +0xB4
    float radFOVNear = 0, radFOVFar = 0;  // +0xB8 / +0xBC
    CSndRef sndZoom, sndExtra;        // +0xC0 / +0xC4 (sounds 0x26 / 0x25)
    uint32_t VtOrig() const override { return S3VA(0x2CD498); }
    void Activate(LO* psoFocus, void* pv) override;       // 0x150938
    void Deactivate(LO* psoFocus, void* pv) override;     // 0x151130
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x150A28
    void Reset() override { sndExtra = CSndRef(); sndZoom = CSndRef(); }  // 0x150DE8 (its sounds)
    void SetTargets(const glm::vec3& pos, const glm::vec3& eul, float radFOV);  // 0x150EA0
    void SetFocusObject(LO* plo);                         // 0x150E20
    void SetTargetObject(LO* plo);                        // 0x150E60
    void SetZoomTarget(float u) { uZoomTarget = u; }      // 0x150EE8
    void Snap();                                          // 0x150EF0
    glm::vec3 PosFocus();                                 // 0x150780
    glm::vec3 PosEye();                                   // 0x1507F8
};

// The binocucom (recon) view (CM +0x450, vtable 0x2CD4E0): a first-person look from the focus (raised by 150 for a
// JT-derived player), the left stick turning within limits, the right stick zooming (7x).
struct CPRECON : CPLCY
{
    float radPan = 0, radTilt = 0;    // +0x10 / +0x14
    float dradPan = 0, dradTilt = 0;  // +0x18 / +0x1C
    float uZoom = 0, duZoom = 0;      // +0x20 / +0x24
    float sZoom = 7.0f;               // +0x28
    float radFOVNear = 0, radFOVFar = 0;  // +0x2C / +0x30
    cmm::LM lmPan{0, 0}, lmTilt{0, 0};    // +0x3C / +0x44
    float radPanStart = 0, radTiltStart = 0;  // +0x4C / +0x50
    int cStart = 0;                   // +0x54
    CSndRef sndZoom, sndExtra;        // +0x34 / +0x38 (sounds 0x26 / 0x25)
    uint32_t VtOrig() const override { return S3VA(0x2CD4E0); }
    void Activate(LO* psoFocus, void* pv) override;       // 0x14FF38
    void Deactivate(LO* psoFocus, void* pv) override;     // 0x150610
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x150160
    void Reset() override { sndExtra = CSndRef(); sndZoom = CSndRef(); }  // 0x1505D8 (its sounds)
    void ResetZoom();                                     // 0x14FE28
    glm::vec3 PosFocus();                                 // 0x14FE58
    glm::vec3 PosEye();                                   // 0x14FEC8
};

// The timed move between two camera states (CM +0x500, vtable 0x2CD408; pushed by 0x1E57A0's objects at priority
// 10): over dt the position, the Euler angles and the field of view blend from A to B along one of the curves at
// 0x2BDA70; the owner is told at the start and the end (its vt slots 1 / 2).
struct CMSTATE  // a camera state of the move ({qw position, qw Euler angles, float field of view})
{
    glm::vec3 pos{0.0f};
    glm::vec3 eul{0.0f};
    float radFOV = 1.0f;
};
struct CMTRANS  // the policy's argument (the original's +0x0 / +0x4 states, +0x8 dt, +0xC cut at the end, +0x10 the
{               // curve, +0x14 the owner)
    const CMSTATE* pstateA = nullptr;
    const CMSTATE* pstateB = nullptr;
    float dt = 1.0f;
    int fCut = 0;
    int icurve = 0;
    void (*pfnStarted)(void* pvOwner) = nullptr;
    void (*pfnEnded)(void* pvOwner) = nullptr;
    void* pvOwner = nullptr;
};
struct CPTIMED : CPLCY
{
    CMTRANS* ptrans = nullptr;  // +0x10
    uint32_t VtOrig() const override { return S3VA(0x2CD408); }
    void Activate(LO* psoFocus, void* pv) override;       // 0x151648
    void Deactivate(LO* psoFocus, void* pv) override;     // 0x151698
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x151700
};

// The attached view (CM +0x5F0, vtable 0x2CD3C0; pushed by 0x225AE8 at priority 9 with the actor as focus): the
// camera at the actor's mount (its +0x1D0 object's world matrix), smoothly (+0x1D4) turning and moving there, with
// the actor's field of view (+0x1BC).
struct CMATTACH
{
    glm::mat4 matMount{1.0f};
    bool fSmooth = false;
    float radFOV = 1.0f;
};
struct CPATTACH : CPLCY
{
    uint32_t VtOrig() const override { return 0x2CD3C0; }
    void Activate(LO* psoFocus, void* pv) override;       // 0x151900
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x151950
};

// Two actors framed (CM +0x600, vtable 0x2CD378; pushed by 0x1CBF90 at priority 6: the argument and the focus): the
// camera to the side of the pair, higher, wider and farther as they part.
struct CPPAIR : CPLCY
{
    LO* ploA = nullptr;  // +0x10 (the argument)
    LO* ploB = nullptr;  // +0x14 (the focus)
    uint32_t VtOrig() const override { return 0x2CD378; }
    void Activate(LO* psoFocus, void* pv) override;       // 0x151BF0
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x151C38
};

// The view from an actor's actuated transform (CM +0x4C0, vtable 0x2CD450). The table/hook interaction passes
// its +0x10C0 transform; the right stick rotates it, and state 9 adds a small idle spring motion.
struct CPACTUATED : CPLCY
{
    struct XF* pxf = nullptr;      // +0x10
    struct CTRL* pctrl = nullptr;  // +0x14 (kind 1, rotation mode 8)
    float tIdle = -1.0f;          // +0x18
    glm::vec3 eulIdle{0.0f};      // +0x20
    glm::vec3 wIdle{0.0f};        // +0x30
    glm::vec3 eulMouse{0.0f};     // (PC, N14) the mouse's look under the table, within the stick's reach
    uint32_t VtOrig() const override { return S3VA(0x2CD450); }
    void Activate(LO* psoFocus, void* pv) override;       // 0x151240
    void Deactivate(LO* psoFocus, void* pv) override;     // 0x1512E0
    void Update(CPDEFI* pcpdefi, const CMJOY* pjoy) override;  // 0x151340
};

struct CMPOLICIES
{
    CPDEFAULT cpdefault;       // +0x3A0
    CPFOLLOW cpfollow;         // +0x3B0
    CPRECON cprecon;           // +0x450
    CPLOOKAT cplookat;         // +0x4A8
    CPACTUATED cp4C0;         // +0x4C0
    CPTIMED cptimed;           // +0x500
    CPFREE cpfree;             // +0x520
    CPATTACH cpattach;         // +0x5F0
    CPPAIR cppair;             // +0x600
};
extern CMPOLICIES g_cmpolicies;

void ConstructCmPolicies();          // 0x14D3D8's constructors
void InitCmPolicies(CM* pcm);        // 0x14D6D8's inits
CPLCY* PcplcyDefault();
// The policy at an original CM offset (0x3A0..0x600), or null.
CPLCY* PcplcyFromCmOffset(uint32_t ib);
uint32_t CmOffsetFromPcplcy(const CPLCY* pcplcy);

// orig: 0x00223A70's work (the eye on a cut avoiding the world between the focus and the eye; *pfBlocked: the way was
// blocked, the follow policy goes to mode 4): posObj the focus object's position.
glm::vec3 PosCmEyeCut(const glm::vec3& posObj, const glm::vec3& posFocus, const glm::vec3& posEye, bool* pfBlocked);
