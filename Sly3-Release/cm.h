#pragma once

// CM, the camera (Sly 1's cm.c; the original's g_cm at 0x2DDF40, a global, not a world object), ported from Sly 2's
// code 0x14D3D8..0x151F70 and Sly1-Release cm.cpp. Field comments give the original offset in CM.
//
// The camera runs a prioritised stack of policies (CPR at +0x310, 8 at most): UpdateCm 0x14D478 updates the top
// one, after fetching its focus object's camera definition (CPDEFI, the focus's vtable slot 50) into +0x270.
// The policies are cplcy.h's.
//
// What the camera reads from other modules goes through CMWORLD (below): the native game binds the object model
// there, and the differential checks (cmcheck.cpp) bind a reference RAM snapshot.

#include "vecmat.h"

#include <array>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

struct LO;
struct CM;
struct SOE;
struct CPLCY;

// The pad state the policies read (the original's JOY 0x2DFC00 / 0x2E0D00, its +0x103C.. fields).
struct CMJOY
{
    float x = 0, y = 0, uDeflect = 0;     // JOY +0x103C left stick
    float x2 = 0, y2 = 0, uDeflect2 = 0;  // JOY +0x1070 right stick
    uint32_t grfbtn = 0, grfbtnPressed = 0;  // JOY +0x10B4 / +0x10B8
    // (PC, pcinput.h) the mouse's turn this step while the cursor is captured and the player has the pad
    float dradMousePan = 0, dradMouseTilt = 0;   // + = mouse right / up
    float dradAimRight = 0, dradAimUp = 0;       // the same at the aim sensitivity (the binocucom)
    float cwheelZoom = 0;                        // wheel notches (+ zooms in)
};

// The focus object's camera definition (slot 50 of a SO-derived object; Sly 1's CPDEFI), 5 qwords at CM +0x270.
struct CPDEFI
{
    uint32_t grfcpd = 0;         // +0x00 (bit 0: the focus is moving under its own control; the rest of the qword
    uint32_t aq0[3]{};           //   is not read by the camera)
    glm::vec4 posBase{0.0f};     // +0x10
    glm::vec4 vBase{0.0f};       // +0x20
    float radHome = 0;           // +0x30
    float sAdjust = 0;           // +0x34
    float tMoveLast = 0;         // +0x38
    int cfk = 0;                 // +0x3C
    glm::vec4 posAdjust{0.0f};   // +0x40
};

// A policy record of the stack (Sly 1's CPR): {priority, policy, focus, policy argument}.
struct CPR
{
    int cpp = 0;                 // +0x0
    CPLCY* pcplcy = nullptr;     // +0x4
    LO* psoFocus = nullptr;      // +0x8
    void* pv = nullptr;          // +0xC
};

// The world as the camera sees it. Every function may be left null: the camera then uses the stated default.
struct CMWORLD
{
    float (*pfnT)() = nullptr;          // g_clock.t (0x2DDED0)
    float (*pfnDt)() = nullptr;         // g_clock.dt (+4)
    float (*pfnDtReal)() = nullptr;     // g_clock.dtReal (+0x10)
    // 0x1A62E8 (the object's world position) and 0x1A6318 (its world matrix)
    glm::vec3 (*pfnPosLo)(LO*) = nullptr;
    glm::mat3 (*pfnMatLo)(LO*) = nullptr;
    // 0x132B10 FIsBasicDerivedFrom
    bool (*pfnFIsLoDerivedFrom)(LO*, int cid) = nullptr;
    // the focus's vtable slot 50 (GetSoCpdefi)
    void (*pfnGetSoCpdefi)(LO*, CPDEFI*) = nullptr;
    // the player's focus kind (the JT-derived object's +0x4F4: 0 relax, 1 clamp, 2 focus)
    int (*pfnJtfock)(LO*) = nullptr;
    // the current player (0x1C4A48 of the world)
    LO* (*pfnPloPlayer)() = nullptr;
    // the pad the policies get (0x2DFBC0 & 1 ? 0x2DFC00 : 0x2E0D00)
    const CMJOY* (*pfnPjoy)() = nullptr;
    // G_GAME +0x3E38 (the stereo offset 0x620 is smoothed while set)
    bool (*pfnFStereo)() = nullptr;
    // 0x14E078's world part: the camera volumes (class 5, flag bit 60) measured from the camera's position; returns
    // the nearest one the camera is in (or null)
    LO* (*pfnCameraVolume)(const glm::vec3& pos) = nullptr;
    // An object field the policies read (the original offset; class-specific data the object model may not have
    // yet). Returns false when unknown.
    bool (*pfnFieldLo)(LO*, uint32_t ib, uint32_t* pu) = nullptr;
};
extern CMWORLD g_cmworld;

float TCm();
float DtCm();
float DtRealCm();

// The camera (0x630 bytes in the original).
struct CM
{
    LO* ploCur = nullptr;          // +0x10 (the nearest object of kind 0x105, CM_SortObjects 0x14E078)
    int nCur = 0xFF;               // +0x14 (its +0xE0)
    float rMRD = 1.0f;             // +0x18 (the MRD ratio, the draw distance multiplier)
    float rMRDAdjust = 1.0f;       // +0x1C max(1, rMRD / radFOV)
    float radFOV = 1.0f;           // +0x20 (the vertical field of view)
    float radFOVTarget = 1.0f;     // +0x24
    float dradFOV = 0.0f;          // +0x28 (GSmoothA's velocity)
    cmm::SMPA smpaRadFOV{0.5f, 0.0f, 0.1f, 10.0f};  // +0x2C
    float rAspect = cmm::F32(0x3FA63BDA); // +0x3C (512 / 394.24)
    float xScreenRange = 0;        // +0x40 tan(fov / 2) * rAspect
    float yScreenRange = 0;        // +0x44 tan(fov / 2)
    float sRadiusNearClip = 0;     // +0x48
    float sNearClip = 100.0f;      // +0x4C
    float sFarClip = 100000.0f;    // +0x50
    glm::mat3 mat{1.0f};           // +0x60 (rows: x = the view direction, y = left, z = up)
    glm::vec3 pos{0.0f};           // +0x90
    std::array<glm::vec4, 3> anormalFrustrumTranspose{};      // +0xA0 (planes 0x1C0 transposed)
    std::array<glm::vec4, 3> anormalFrustrumTransposeWide{};  // +0xD0 (planes 0x200 transposed)
    cmm::MATRIX4 matProj;               // +0x100 (BuildSimpleProjectionMatrix)
    cmm::MATRIX4 matWorldToClip;        // +0x140
    cmm::MATRIX4 matClipToWorld;        // +0x180
    std::array<glm::vec4, 4> anormalFrustrum{};      // +0x1C0
    std::array<glm::vec4, 4> anormalFrustrumWide{};  // +0x200 (x 7.93 / 18.1: the wide test)
    float sNearFog = 500.0f;       // +0x240
    float sFarFog = 20000.0f;      // +0x244
    float uFogMax = 0.5f;          // +0x248
    uint32_t rgbaFog = 0x80303030; // +0x24C
    float afFogRecip[4]{};         // +0x250 (CM_BuildFogRecip 0x14E270: the renderer's fog fit)
    LO* psoFocusPrev = nullptr;    // +0x260
    CPDEFI cpdefiPrev;             // +0x270
    glm::vec4 dposAdjust{0.0f};    // +0x2C0
    glm::vec4 vAdjust{0.0f};       // +0x2D0
    glm::vec4 dposFocus{0.0f};     // +0x2E0
    glm::vec4 vFocus{0.0f};        // +0x2F0
    int fCut = 0;                  // +0x300 (1: cut now; 2: cut with the angles below)
    float radCutPan = 0, radCutTilt = 0;  // +0x304 / +0x308 (0x14D8D8)
    int ccpr = 0;                  // +0x30C
    std::array<CPR, 8> acpr{};     // +0x310
    float tActivateCplcy = 0;      // +0x390
    // +0x3A0.. the policies (cplcy.h) live in g_cmpolicies
    int cFreeStarts = 0;           // +0x4A4 (the free camera's starts)
    float tJolt = -10.0f;          // +0x618 (JoltCm 0x14F750: the shake's start)
    int nJolt = -1;                // +0x61C
    float sStereo = 0.0f;          // +0x620
    float sStereoTarget = 10.0f;   // +0x624
};

extern CM g_cm;

// The projection for the renderer (the GL side of RecalcCm): the picture's aspect mode (display.h
// ProjectionForDisplay, Sly1-Release's RecalcCm: Hor+ on the window, the PS2 projection, Vert- 16:9). The gameplay
// values (xScreenRange, yScreenRange, sRadiusNearClip, rMRDAdjust) stay in the PS2's camera space; the draw-distance
// option is applied by the renderer to the MRDs (display.h DISPLAY::drawDistance).
struct CMVIEW
{
    glm::vec3 pos{0.0f};
    glm::mat3 mat{1.0f};      // the camera's rows (x view, y left, z up)
    float radFovY = 1.0f;     // the vertical field of view drawn
    float rAspect = 4.0f / 3.0f;  // the aspect drawn
    float sNear = 100.0f, sFar = 100000.0f;
    glm::mat4 matView{1.0f};  // GL: world to eye (-z forward, y up)
    glm::mat4 MatWorldToClip() const;  // reversed-Z infinite far (the renderer's convention)
};
CMVIEW CmviewFromCm(const CM& cm);

// orig: 0x0014D3D8 / 0x0014F930 (the static constructor: the policies' constructors)
void StartupCm();
// orig: 0x0014D6D8 InitCm (projection defaults, the identity, the default look-at, the default policy pushed)
void InitCm(CM* pcm);
// orig: 0x0014D650 ResetCm (each stacked policy's slot 7)
void ResetCm(CM* pcm);
// orig: 0x0014D478 UpdateCm
void UpdateCm(CM* pcm);
// orig: 0x0014D8C0 CutCm (+0x300 at least 1) / 0x0014D8D8 SetCmCut (at least 2, with the angles)
void CutCm(CM* pcm);
void SetCmCut(CM* pcm, float radPan, float radTilt);
// orig: 0x0014D8F8 CombineEyeLookAtProj (the matrix pm (rows x, y, z, pos) in the renderer's axes (-y, -z, x),
// inverted, times the projection)
cmm::MATRIX4 CombineEyeLookAtProj(const cmm::MATRIX4& matCam, const cmm::MATRIX4& matProj);
// orig: 0x0014DA08 BuildFrustrum
void BuildFrustrum(const glm::mat3& mat, float rx, float ry, glm::vec4 anormal[4]);
// orig: 0x0014DB70 RecalcCm (RecalcCmFrustrum)
void RecalcCm(CM* pcm);
// orig: 0x0014DC58 UpdateCmMat4 (the clip matrices, shaken after JoltCm; the planes; the objects' sort)
void UpdateCmMat4(CM* pcm);
// orig: 0x0014E148 / 0x0014E078
void SetCmCurObject(CM* pcm, LO* plo);
void SortCmObjects(CM* pcm);
// orig: 0x0014E168 SetCmPosMat
void SetCmPosMat(CM* pcm, const glm::vec3& pos, const glm::mat3& mat);
// orig: 0x0014E1A8 SetCmLookAt (from pos to posTarget, turned by dradPan / dradTilt)
void SetCmLookAt(CM* pcm, const glm::vec3& pos, const glm::vec3& posTarget, float dradPan, float dradTilt);
// orig: 0x0014E270 BuildCmFogRecip
void BuildCmFogRecip(CM* pcm, float af[4], float g);
// orig: 0x0014E328 SetCmPolicy / 0x0014E4C8 RevokeCmPolicy (grf 1 cpp, 2 policy, 4 focus, 8 pv)
void SetCmPolicy(CM* pcm, int cpp, CPLCY* pcplcy, LO* psoFocus, void* pv);
void RevokeCmPolicy(CM* pcm, int grf, int cpp, CPLCY* pcplcy, LO* psoFocus, void* pv);
// orig: 0x0014E698 CalcCmAdjust / 0x0014E6D0 SpringCm / 0x0014E768 AdjustCmCpdefi / 0x0014E920 ResetCmAdjust /
// 0x0014E968 GetCmCpdefi
glm::vec4 CalcCmAdjust(CM* pcm, const CPDEFI* pcpdefi);
void SpringCm(CM* pcm, glm::vec4* ppos, glm::vec4* pv, const glm::vec4& posTarget, float rSpring, float rDamping);
void AdjustCmCpdefi(CM* pcm, LO* psoFocus, CPDEFI* pcpdefi);
void ResetCmAdjust(CM* pcm, const CPDEFI* pcpdefi);
void GetCmCpdefi(CM* pcm, LO* psoFocus, CPDEFI* pcpdefi);
// orig: 0x0014EA08 FFindCmClearInterval / 0x0014EB70 FindCmClosestClearPos / 0x0014EDA0 SquishCmEye /
// 0x0014F110 PosCmAvoid (collision queries: need the physics code's API; stubbed, see cm.cpp)
// The world's solid entries the camera's queries test (orig 0x20FAA0 with the filter 0x14F030: the entries with a
// tree in the attachments whose boxes meet the box of a and b; at most 256).
using CMSOLIDS = std::vector<SOE*>;
CMSOLIDS AsolidCm(const glm::vec3& a, const glm::vec3& b);
CMSOLIDS AsolidWithTree(const glm::vec3& a, const glm::vec3& b);  // (not the camera's: no +0x70 test)
void DumpCmSolids(const CMSOLIDS& asolid);
bool FFindCmClearInterval(float duMin, const CMSOLIDS& asolid, const glm::vec3& posFrom, const glm::vec3& posTo,
                          float* puFrom, float* puTo);
glm::vec3 FindCmClosestClearPos(const CMSOLIDS& asolid, const glm::vec3& pos, float sRadius);
glm::vec3 PosCmAvoid(CM* pcm, const glm::vec3& pos, const glm::vec3& posTarget);
// A hit of the sphere query (0x1A93C8's 0x30-byte record): its point, its plane (normal, d), kind, tag.
struct CMHIT
{
    glm::vec3 pos{0.0f};
    glm::vec4 plane{0.0f};
};
using PFNCMHITWEIGHT = float (*)(CM* pcm, const glm::vec3& pos, const glm::vec3& v, const CMHIT& hit);
float GCmAvoidWeight(CM* pcm, const glm::vec3& pos, const glm::vec3& v, const CMHIT& hit);   // 0x14F040
float GCmCutWeight(CM* pcm, const glm::vec3& pos, const glm::vec3& v, const CMHIT& hit);     // 0x223A10
glm::vec3 SquishCmEye(CM* pcm, const CMSOLIDS& asolid, PFNCMHITWEIGHT pfn, const glm::vec3& pos, const glm::vec3& v,
                      float sRadius, float sMax);
// orig: 0x0014F268 ConvertCmScreenToWorld / 0x0014F308 ConvertCmWorldToScreen ((x, y, depth))
glm::vec3 ConvertCmScreenToWorld(const CM* pcm, const glm::vec3& posScreen);
glm::vec3 ConvertCmWorldToScreen(const CM* pcm, const glm::vec3& posWorld);
// orig: 0x0014F5A0 SetCmFov (now) / 0x0014F5F8 SetCmFovTarget / 0x0014F610 SetCmNearClip / 0x0014F630 (+ the top
// policy's saved one) / 0x0014F670 SetCmFarClip / 0x0014F690 SetCmNearFog / 0x0014F6B0 SetCmFarFog /
// 0x0014F6D0 SetCmUFogMax / 0x0014F6F0 SetCmRgbaFog / 0x0014F730 SetCmMrdRatio / 0x0014F750 JoltCm
void SetCmFov(CM* pcm, float radFOV);
void SetCmFovTarget(CM* pcm, float radFOV);
void SetCmNearClip(CM* pcm, float sNearClip);
void SetCmNearClipAll(CM* pcm, float sNearClip);
void SetCmFarClip(CM* pcm, float sFarClip);
void SetCmNearFog(CM* pcm, float sNearFog);
void SetCmFarFog(CM* pcm, float sFarFog);
void SetCmUFogMax(CM* pcm, float uFogMax);
void SetCmRgbaFog(CM* pcm, uint32_t rgba);
void SetCmMrdRatio(CM* pcm, float rMRD);
void JoltCm(CM* pcm, int n);
// orig: 0x0014F768 (the stereo eye's clip matrix: k 1 left, 2 right)
cmm::MATRIX4 MatCmStereoEye(const CM* pcm, int k, const cmm::MATRIX4* pmatProj);
// orig: 0x0014F850 FInsideCmPlanes (the point in front of the three transposed planes by w.x)
bool FInsideCmPlanes(const CM* pcm, const glm::vec3& pos, float sRadius);
// orig: VU0 0x38..0x53 (RenderGlob 0x168740 / 0x168A44): the sphere test's distance fade.
// Radius belongs to the separate plane test, not to the distance from the camera.
float UCmSphereDistanceFade(const CM* pcm, const glm::vec3& dpos, float sMRD);
// orig: 0x0014F898 FInsideCmMrd (a distance vector within sMRD * rMRDAdjust; *puAlpha the fade)
bool FInsideCmMrd(const CM* pcm, const glm::vec3& dpos, float sMRD, float* puAlpha);

// The camera options of a world file (0x13DA18's second option block, applied to g_cm by their EOPIDs).
struct OPT;
void ApplyCmOptions(CM* pcm, const std::vector<OPT>& aopt);
