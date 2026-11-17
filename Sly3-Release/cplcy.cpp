// The camera policies (see cplcy.h). The base and the small ones from the reference's cplcy.cpp, m_14F030_p3.cpp and
// m_12E600_p3.cpp (oracle-verified); the follow camera (0x2219C8..0x2236E8) from the original's code with the
// decomp's matching C (0x2219C8, 0x221A18, 0x221818, 0x220D40, 0x220DD8, 0x221CE8, 0x221408 / 0x2215C0 hand C)
// and the reference's ports (0x2236E8 / 0x223A70 m_220D40_p3.cpp; 0x221DD8 round4_camera.cpp, a register-level
// transcription: the structure below is re-derived from the asm).
#include "game/scptgame3.h"
#include "game/task3.h"
#include "cplcy.h"
#include "game/sly3.h"
#include "vr.h"
#include "pcinput.h"
#include "marker.h"

#include "cls.h"
#include "xf.h"
#include "so.h"
#include "activate.h"
#include "task.h"
#include "clock.h"
#include "sw.h"
#include <cstdlib>
#include <cstring>
#include <string>

#include <algorithm>
#include <cmath>

float GRandInRange(float gLow, float gHigh);  // xfmath.h (orig 0x11D448)
bool FCamReverse3(int kind, bool fVertical);  // menu.h (Camera Options...)

CMPOLICIES g_cmpolicies;
CMFOLLOWWORLD g_cmfollowworld;

using namespace cmm;

namespace
{
// orig 0x14FF38 / 0x150938: the zoom's sound 0x26 (paused until the zoom moves) and 0x25
void StartZoomSounds(CSndRef* pzoom, CSndRef* pextra)
{
    CSnd* p = PsndNewPlain(0x26);
    p->SetSource(nullptr);
    *pzoom = CSndRef(p);
    p = PsndNewPlain(0x25);
    p->SetSource(nullptr);
    *pextra = CSndRef(p);
    (*pzoom)->Pause();
}

// orig 0x150160 / 0x150A28: the zoom's sound follows the zoom's speed (pitch |v|, volume |v| / 2 + 1/2)
void UpdateZoomSound(CSndRef& snd, bool fMoving, float v)
{
    if (!snd) return;
    if (fMoving)
    {
        snd->SetPitch(std::clamp(std::fabs(v), -1.0f, 1.0f));   // (0x2BBB68 {-1, 1})
        snd->SetVolume(std::clamp(std::fabs(v) * 0.5f + 0.5f, 0.0f, 1.0f));
        if (snd->m_cpause > 0) snd->Resume();
    }
    else if (snd->m_cpause <= 0) snd->Pause();
}

// orig 0x150610 / 0x151130: stopped (the second after half a second) and dropped
void StopZoomSounds(CSndRef* pzoom, CSndRef* pextra)
{
    if (*pzoom) (*pzoom)->Stop(0.0f);
    *pzoom = CSndRef();
    if (*pextra) (*pextra)->Stop(0.5f);
    *pextra = CSndRef();
}

constexpr float EPS_1E4 = F32(0x38D1B717);
// (PC) the mouse to the right turns the view to the right (a lower pan: the right stick to the
// right orbits the other way, the original's), unless the camera's mouse inversion (mouseInvertX) is on
constexpr float MOUSE_PAN_SIGN = -1.0f;
constexpr float MOUSE_TABLE_TILT_SIGN = -1.0f;  // (PC: mouse up looks up under a table: -y pitch, checked by frames)
constexpr float FLT_MAXF = F32(0x7F7FFFFF);
constexpr float TWO_PI = F32(0x40C90FDB);

// The follow camera's data (0x2C40E0..0x2C42E0).
const LM s_lmTilt{-0.5f, F32(0x3FC70D23)};    // 0x2C41B0
LM s_lmFollow{-FLT_MAXF, FLT_MAXF};           // 0x2C41A8 (reset by the init from 0x2A96F8; not read here)
const LM s_lmUnit{0.0f, 1.0f};                // 0x2BBB60
constexpr float DT_MANUAL_HOLD = 0.75f;       // 0x2C41D8
constexpr float DT_MANUAL = 10.0f;            // 0x2C41DC
constexpr float G_LEAD_LIMIT = 2.5f;          // 0x2C41E0
constexpr float SW_PAN_STICK = 2.5f;          // 0x2C41E4
constexpr float SW_TILT_STICK = 1.0f;         // 0x2C41E8
constexpr float DSW_STICK = 15.0f;            // 0x2C41EC
const float s_aLeadStick[3] = {1.0f, -2.0f, -2.0f};  // 0x2C41F0 (a quadratic of the stick's y)
const LM s_lmLeadStick{1.0f, 2.0f};                   // 0x2C4200
const SMP s_smpSample{0.5f, 0.0f, 0.5f};              // 0x2C4208 (x the rate)
const float s_aLeadTime[3] = {0.0f, 1.0f, 0.0f};     // 0x2C4220 (a quadratic of the time since the last turn)
const SMP s_smpLead{4.0f, 0.0f, 0.25f};               // 0x2C4230
const SMPA s_smpaReturn{5.0f, 0.0f, 0.25f, 35.0f};    // 0x2C4240
const SMPA s_smpaReturnFast{10.0f, 0.0f, 0.1f, 100.0f};  // 0x2C4250
const SMP s_smpTiltPassive{0.75f, 0.0f, 2.0f};        // 0x2C4260 (x the focus's speed)
constexpr float R_DIST_DAMPING = -8.0f;       // 0x2C426C
constexpr float R_DIST_SPRING = 15.0f;        // 0x2C4270
constexpr float R_OFFSET_SPRING = -10.0f;     // 0x2C4274 / 0x2C427C
constexpr float R_OFFSET_DAMPING = -5.0f;     // 0x2C4278 / 0x2C4280
constexpr float DT_VISIBLE_CHECK = 0.5f;      // 0x2C4110
constexpr float DT_VISIBLE_LOST = 2.0f;       // 0x2C4114
constexpr int F_NO_AVOID = 0;                 // 0x2C4118 (a debug switch)
const glm::vec3 s_adposVisible[5] = {{0, 0, 0}, {0, -50, 0}, {0, 50, 0}, {0, 0, 75}, {0, 0, -50}};  // 0x2C4290

const ZONECME& CmeDefault()
{
    static const ZONECME s_cme = ZonecmeDefault();
    return s_cme;
}

CMFOCUS FocusOf(LO* pso)
{
    CMFOCUS f;
    if (pso && g_cmfollowworld.pfnFocus) g_cmfollowworld.pfnFocus(pso, &f);
    return f;
}

bool FIsLoDerivedFromCm(LO* plo, int cid)
{
    return plo && g_cmworld.pfnFIsLoDerivedFrom && g_cmworld.pfnFIsLoDerivedFrom(plo, cid);
}

glm::vec3 PosLoCm(LO* plo) { return plo && g_cmworld.pfnPosLo ? g_cmworld.pfnPosLo(plo) : glm::vec3(0.0f); }
glm::mat3 MatLoCm(LO* plo) { return plo && g_cmworld.pfnMatLo ? g_cmworld.pfnMatLo(plo) : glm::mat3(1.0f); }

// orig: 0x001A6F00 (a point from a's frame (null: the world) to b's: through a's world transform, then b's inverse)
glm::vec3 ConvertPos(LO* a, LO* b, const glm::vec3& v)
{
    if (g_cmfollowworld.pfnConvertPos) return g_cmfollowworld.pfnConvertPos(a, b, v);
    glm::vec3 r = v;
    if (a) r = MatLoCm(a) * r + PosLoCm(a);
    if (b)
    {
        glm::mat4 m(MatLoCm(b));
        m[3] = glm::vec4(PosLoCm(b), 1.0f);
        r = glm::vec3(glm::inverse(m) * glm::vec4(r, 1.0f));
    }
    return r;
}
// orig: 0x001A6FC0 (a direction: through a's rotation, then b's transpose)
glm::vec3 ConvertVec(LO* a, LO* b, const glm::vec3& v)
{
    if (g_cmfollowworld.pfnConvertVec) return g_cmfollowworld.pfnConvertVec(a, b, v);
    glm::vec3 r = v;
    if (a) r = MatLoCm(a) * r;
    if (b) r = glm::transpose(MatLoCm(b)) * r;
    return r;
}
void CrvPos(LO* pcrv, float s, glm::vec3* ppos, glm::vec3* pdir)
{
    if (g_cmfollowworld.pfnCrvPos) g_cmfollowworld.pfnCrvPos(pcrv, s, ppos, pdir);
    else
    {
        *ppos = glm::vec3(0.0f);
        *pdir = glm::vec3(1, 0, 0);
    }
}

glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& vDefault)
{
    const float l = glm::length(v);
    if (l < EPS_1E4) return vDefault;
    return v * (1.0f / l);
}

// orig: 0x0011B830 BuildLookAtMatrix (x along vecX (x when too short), y = vecY x x normalized, z = x x y) (orig3: 0x11CBF0, the same code in Sly 3)
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY)
{
    const float g = glm::length(vecX);
    const glm::vec3 x = g < EPS_1E4 ? glm::vec3(1, 0, 0) : vecX * (1.0f / g);
    glm::vec3 y = glm::cross(vecY, x);
    y = y * (1.0f / glm::length(y));
    glm::mat3 m;
    m[0] = x;
    m[1] = y;
    m[2] = glm::cross(x, y);
    return m;
}

float Quadratic(const float a[3], float x) { return a[0] + x * (a[1] + x * a[2]); }
}  // namespace

const ZONECME* PcmeDefault() { return &CmeDefault(); }

ZONECME ZonecmeDefault()
{
    ZONECME e;
    e.cid = S3CID(0x30);  // (the ALO class: Sly 3 0x2F)
    e.dtPath = -450.0f;
    e.acoef = {glm::vec4(0, 1, 0, 0), glm::vec4(0, 1, 0, 0), glm::vec4(125, 1, 0, 0)};
    e.alm = {LM{-FLT_MAXF, FLT_MAXF}, LM{-FLT_MAXF, FLT_MAXF}, LM{-FLT_MAXF, FLT_MAXF}};
    e.dxScreen = 0.0f;
    e.dyScreen = F32(0xBDCCCCCD);
    e.uFocus = 1.0f;
    e.uLead = 1.0f;
    e.uSampleRate = 1.0f;
    e.vecCoefY = glm::vec3(1.0f);
    e.rSpring = 1.0f;
    e.rDamping = 1.0f;
    e.lmTilt = LM{-FLT_MAXF, FLT_MAXF};
    return e;
}

// orig: 0x0014FCC8 (a policy's activation: the camera's field of view target and near clip saved)
void CPLCY::Activate(LO* psoFocusNew, void* pv)
{
    (void)pv;
    psoFocus = psoFocusNew;
    sNearClipSave = g_cm.sNearClip;
    radFOVTargetSave = g_cm.radFOVTarget;
}

// orig: 0x0014FCE8 (restored)
void CPLCY::Deactivate(LO* psoFocusOld, void* pv)
{
    (void)psoFocusOld; (void)pv;
    psoFocus = nullptr;
    SetCmNearClip(&g_cm, sNearClipSave);
    g_cm.radFOVTarget = radFOVTargetSave;
}

// orig: 0x0014FD50
bool FActiveCplcy(const CPLCY* pcplcy) { return g_cm.acpr[0].pcplcy == pcplcy; }

// orig: 0x0014D3D8 (the constructors: 0x14FD68, 0x221990, 0x14FDE0, 0x12E760, 0x151208, 0x151610, 0x150728,
// 0x1518C8, 0x14FCA8 + vtable 0x2CD378)
void ConstructCmPolicies()
{
    g_cmpolicies = CMPOLICIES{};
    g_cmpolicies.cpfree.sZoom = 7.0f;          // 0x150728: +0xB4 7, +0x18 -1, +0xC0 / +0xC4 0
    g_cmpolicies.cpfree.uZoomTarget = -1.0f;
    g_cmpolicies.cprecon.sZoom = 7.0f;         // 0x14FDE0: +0x28 7, +0x34 / +0x38 0
}

// orig: 0x0014D6D8's inits (0x14FDA0 for +0x3A0, 0x2219C8 for +0x3B0, 0x14FCC0 for the others)
void InitCmPolicies(CM* pcm)
{
    (void)pcm;
    g_cmpolicies.cpdefault.Init();
    g_cmpolicies.cpfollow.Init();
}

CPLCY* PcplcyDefault() { return &g_cmpolicies.cpdefault; }

CPLCY* PcplcyFromCmOffset(uint32_t ib)
{
    switch (ib)
    {
    case 0x3A0: return &g_cmpolicies.cpdefault;
    case 0x3B0: return &g_cmpolicies.cpfollow;
    case 0x450: return &g_cmpolicies.cprecon;
    case 0x4A8: return &g_cmpolicies.cplookat;
    case 0x4C0: return &g_cmpolicies.cp4C0;
    case 0x500: return &g_cmpolicies.cptimed;
    case 0x520: return &g_cmpolicies.cpfree;
    case 0x5F0: return &g_cmpolicies.cpattach;
    case 0x600: return &g_cmpolicies.cppair;
    default: return nullptr;
    }
}

uint32_t CmOffsetFromPcplcy(const CPLCY* p)
{
    for (uint32_t ib : {0x3A0u, 0x3B0u, 0x450u, 0x4A8u, 0x4C0u, 0x500u, 0x520u, 0x5F0u, 0x600u})
        if (PcplcyFromCmOffset(ib) == p) return ib;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The follow camera (module 0x220D40).

namespace
{
// orig: 0x00220D40 (a fixed tilt from the entry's coefficients while its tilt limits are still the defaults) (orig3: 0x273A18, the same C in the Sly 3 decomp)
void InitCmeTilt(ZONECME* pcme)
{
    if (pcme->lmTilt.gMin == -FLT_MAXF && pcme->lmTilt.gMax == FLT_MAXF)  // 0x2A96E0 / 0x2A96E4
    {
        const float a = pcme->dtPath + pcme->acoef[0].x;
        const float f = GLimitLm(&s_lmTilt, std::atan2(pcme->acoef[2].x, std::sqrt(a * a + pcme->acoef[1].x * pcme->acoef[1].x)));
        pcme->lmTilt.gMax = f;
        pcme->lmTilt.gMin = f;
    }
}

// orig: 0x00221408 (pos in the zone's frame at its path parameter: *pposZone; *pnormal the path's direction)
void ConvertZonePos(ZONECM* pzone, const glm::vec3& pos, glm::vec3* pposZone, glm::vec3* pnormal)
{
    const glm::vec3 z(0, 0, 1);
    if (pzone->pcrv)
    {
        glm::vec3 posCrv;
        CrvPos(pzone->pcrv, pzone->sPath, &posCrv, pnormal);
        const glm::mat3 m = BuildLookAtMatrix(*pnormal, z);
        const glm::vec3 d = pos - posCrv;
        *pposZone = glm::vec3(glm::dot(m[0], d), glm::dot(m[1], d), glm::dot(m[2], d));
    }
    else if (pzone->fArc)
    {
        const float r = pzone->sArcRadius;
        const glm::vec3 a = SetVectorCylind(pzone->sPath / r, r, 0.0f);
        const glm::vec3 posArc = pzone->posArc + a;
        const glm::vec3 n = glm::normalize(glm::cross(z, a));
        *pnormal = n;
        const glm::mat3 m = BuildLookAtMatrix(n, z);
        const glm::vec3 d = pos - posArc;
        *pposZone = glm::vec3(glm::dot(m[0], d), glm::dot(m[1], d), glm::dot(m[2], d));
    }
    else
    {
        *pnormal = glm::normalize(MatLoCm(pzone->pzone)[0]);
        *pposZone = ConvertPos(nullptr, pzone->pzone, pos);
    }
}

// orig: 0x002215C0 (the camera sample: three clamped quadratics of the focus's zone coordinates, mirrored for
// mode 2, polar when the zone says, placed on the path at sPath -/+ dsPath (mode 1: +))
glm::vec3 PosZoneSample(ZONECM* pzone, const glm::vec3& posZone, const std::array<glm::vec4, 3>& acoef,
                        const std::array<LM, 3>& alm, int mode, float sPath, float dsPath)
{
    glm::vec3 x;
    for (int i = 0; i < 3; ++i)
    {
        const float t = posZone[i];
        float g = acoef[i].x + t * (acoef[i].y + t * acoef[i].z);
        if (g < alm[i].gMin) g = alm[i].gMin;
        else if (alm[i].gMax < g) g = alm[i].gMax;
        x[i] = g;
    }
    if (mode == 2) x.x = -x.x;
    if (pzone->fPolar) x = SetVectorCylind(x.x, x.y, x.z);
    const glm::vec3 z(0, 0, 1);
    glm::vec3 posBase(0.0f), dpos;
    if (pzone->pcrv || pzone->fArc)
    {
        glm::mat3 m;
        const float s = mode == 1 ? sPath + dsPath : sPath - dsPath;
        if (pzone->pcrv)
        {
            glm::vec3 dir;
            CrvPos(pzone->pcrv, s, &posBase, &dir);
            m = BuildLookAtMatrix(dir, z);
        }
        else
        {
            const float r = pzone->sArcRadius;
            const float ang = std::fmod(s, r * TWO_PI);
            const glm::vec3 a = SetVectorCylind(ang / r, r, 0.0f);
            posBase = a + pzone->posArc;
            m = BuildLookAtMatrix(glm::normalize(glm::cross(z, a)), z);
        }
        dpos = m * x;
    }
    else
    {
        x.x += dsPath;
        dpos = ConvertPos(pzone->pzone, nullptr, x);
    }
    return posBase + dpos;
}

// orig: 0x00220DD8 (the focus point: between the focus's base and adjusted positions; with the zone, also its
// coordinates scaled by the entry's +0x70 in the zone's frame)
glm::vec3 PosCmeFocus(const ZONECME* pcme, const CPDEFI* pcpdefi, ZONECM* pzone)
{
    const float t = pcme->uFocus;
    glm::vec3 pos = glm::vec3(pcpdefi->posBase) * (1.0f - t) + glm::vec3(pcpdefi->posAdjust) * t;
    if (pzone)
    {
        std::array<glm::vec4, 3> acoef{};
        acoef[0].y = pcme->vecCoefY.x;
        acoef[1].y = pcme->vecCoefY.y;
        acoef[2].y = pcme->vecCoefY.z;
        const int fPolar = pzone->fPolar;
        pzone->fPolar = 0;
        glm::vec3 posZone, normal;
        ConvertZonePos(pzone, pos, &posZone, &normal);
        pos = PosZoneSample(pzone, posZone, acoef, pcme->alm, 1, pzone->sPath, 0.0f);
        pzone->fPolar = fPolar;
    }
    return pos;
}

// orig: 0x00221CE8 (the zone's path parameter nearest to the focus)
void UpdateZonePath(CPFOLLOW* p, const CPDEFI* pcpdefi)
{
    ZONECM* pzone = p->pzone;
    if (!pzone) return;
    if (!pzone->pcrv && !pzone->fArc) return;
    const glm::vec3 q = PosCmeFocus(p->pcme, pcpdefi, nullptr);
    if (pzone->pcrv)
    {
        if (g_cmfollowworld.pfnCrvNearest) pzone->sPath = g_cmfollowworld.pfnCrvNearest(pzone->pcrv, q, pzone->sPath);
    }
    else
    {
        const glm::vec3 d = ConvertPos(nullptr, pzone->pzone, q) - pzone->posArc;
        float g = std::atan2(d.y, d.x);
        if (g < 0.0f) g += TWO_PI;
        pzone->sPath = g * pzone->sArcRadius;
    }
}

// orig: 0x002236E8 (the focus's visibility: every 0.5 s five points about the focus are tested against the world
// toward the camera; lost for 2 s, the camera cuts)
void CheckFocusVisible(CPFOLLOW* p)
{
    if (g_cmfollowworld.pfnVisibility && g_cmfollowworld.pfnVisibility(p)) return;
    // (0x2DFBC4 & 2: paused) is the game's; not modelled here
    if (p->pzone && p->pzone->fNoAvoid) return;
    if (F_NO_AVOID) return;
    if (TCm() - p->tVisible < DT_VISIBLE_CHECK) return;
    if (g_cm.fCut) return;
    const glm::vec3 posFocus = PosLoCm(p->psoFocus);
    const glm::vec3 z(0, 0, 1);
    glm::vec3 r1 = glm::cross(z, posFocus - g_cm.pos);
    const float l = glm::length(r1);
    r1 = l < EPS_1E4 ? glm::vec3(1, 0, 0) : r1 * (1.0f / l);
    const glm::vec3 r0 = glm::cross(z, r1);
    // the box of the camera and the five points (0x20FAA0 with 0x14F030)
    glm::vec3 apt[5];
    glm::vec3 mn = g_cm.pos, mx = g_cm.pos;
    for (int i = 0; i < 5; ++i)
    {
        apt[i] = posFocus + r0 * s_adposVisible[i].x + r1 * s_adposVisible[i].y + z * s_adposVisible[i].z;
        mn = glm::min(mn, apt[i]);
        mx = glm::max(mn, apt[i]);  // (the maximum of the new minimum and the point, as the original)
    }
    const CMSOLIDS asolid = AsolidCm(mn, mx);
    bool fSeen = false;
    for (int i = 0; i < 5 && !fSeen; ++i)
    {
        const glm::vec3 pt = apt[i];
        glm::vec3 posScreen = ConvertCmWorldToScreen(&g_cm, pt);
        posScreen.z = g_cm.sNearClip;
        const glm::vec3 posNear = ConvertCmScreenToWorld(&g_cm, posScreen);
        float uTo;
        if (FFindCmClearInterval(0.0f, asolid, pt, posNear, nullptr, &uTo) && 1.0f <= uTo) fSeen = true;
    }
    if (fSeen) p->tVisible = TCm();
    if (DT_VISIBLE_LOST < TCm() - p->tVisible) CutCm(&g_cm);
}

// orig: 0x00223A70 (the eye on a cut, avoiding the world between the focus and the eye: from the focus's free point
// (FindCmClosestClearPos, 500) to the eye; when that is blocked (the policy goes to mode 4) the best of the rays to
// points about the focus on the eye's cylinder, alternating angles (cost (1.25 - free fraction) (|angle| + 0.4712));
// then pushed out by the camera's near radius (SquishCmEye) and back toward the candidate when that is blocked).
// The objects in the box (0x20FAA0), the segment and sphere queries: cm.cpp (the physics runtime's attachments and
// trees, mark.cpp).
glm::vec3 PosEyeCut(CPFOLLOW* p, const glm::vec3& posFocus, const glm::vec3& posEye)
{
    glm::vec3 posOut;
    bool fBlocked = false;
    if (g_cmfollowworld.pfnEyeCut && g_cmfollowworld.pfnEyeCut(p, posFocus, posEye, &posOut, &fBlocked))
    {
        if (fBlocked) p->cpmode = 4;
        return posOut;
    }
    const glm::vec3 posCut = PosCmEyeCut(PosLoCm(p->psoFocus), posFocus, posEye, &fBlocked);
    if (std::getenv("SLY2_CMCUTPRINT"))
    {
        const glm::vec3 o = PosLoCm(p->psoFocus);
        DumpCmSolids(AsolidCm(glm::min(posFocus, glm::min(posEye, o)), glm::max(posFocus, glm::max(posEye, o))));
        std::printf("  eye cut: object %.1f %.1f %.1f focus %.1f %.1f %.1f eye %.1f %.1f %.1f -> %.1f %.1f %.1f blocked %d\n", o.x, o.y, o.z, posFocus.x, posFocus.y,
                    posFocus.z, posEye.x, posEye.y, posEye.z, posCut.x, posCut.y, posCut.z, fBlocked);
    }
    if (fBlocked) p->cpmode = 4;
    return posCut;
}
}  // namespace

glm::vec3 PosCmEyeCut(const glm::vec3& posObj, const glm::vec3& posFocus, const glm::vec3& posEye, bool* pfBlocked)
{
    *pfBlocked = false;
    // 0x20FAA0 with 0x14F030: the box of the eye, the focus and the focus's object
    const CMSOLIDS asolid = AsolidCm(glm::min(posFocus, glm::min(posEye, posObj)), glm::max(posFocus, glm::max(posEye, posObj)));
    const glm::vec3 posOrigin = FindCmClosestClearPos(asolid, posObj, 500.0f);
    float uFrom = 0.0f, uTo = 1.0f;
    FFindCmClearInterval(0.0f, asolid, posOrigin, posEye, &uFrom, &uTo);
    glm::vec3 posCand;
    if (uTo < 1.0f)
    {
        *pfBlocked = true;
        float rad, r, z;
        ConvertVectorToCylind(posEye - posFocus, &rad, &r, &z);
        float costBest = F32(0x7F7FFFFF);  // 0x2A9700
        float ang = 0.0f;
        posCand = posEye;
        for (;;)
        {
            const glm::vec3 posRay = posFocus + SetVectorCylind(rad + ang, r, z);
            FFindCmClearInterval(0.0f, asolid, posOrigin, posRay, &uFrom, &uTo);
            const float cost = (1.25f - uTo) * (std::fabs(ang) + F32(0x3EF1463B));
            if (cost < costBest)
            {
                costBest = cost;
                posCand = posOrigin * (1.0f - uTo) + posRay * uTo;
            }
            float angNext = -ang;
            if (ang <= 0.0f) angNext += F32(0x3E923A14);
            ang = angNext;
            if (costBest < (std::fabs(angNext) + F32(0x3EF1463B)) * 0.25f) break;
            if (!(angNext < PI)) break;
        }
    }
    else posCand = posEye;
    const float r = g_cm.sRadiusNearClip;
    const glm::vec3 posFin = SquishCmEye(&g_cm, asolid, GCmCutWeight, posCand, posCand, r, r);
    FFindCmClearInterval(0.0f, asolid, posCand, posFin, &uFrom, &uTo);
    if (uTo == 1.0f) return posFin;
    const float u = (uFrom + uTo) * 0.5f;
    return posCand * (1.0f - u) + posFin * u;
}

namespace
{
glm::vec3 PosEyeMove(const glm::vec3& posFrom, const glm::vec3& posTo)
{
    glm::vec3 posOut;
    if (g_cmfollowworld.pfnEyeMove && g_cmfollowworld.pfnEyeMove(posFrom, posTo, &posOut)) return posOut;
    return PosCmAvoid(&g_cm, posFrom, posTo);
}
}  // namespace

// orig: 0x002219C8
void CPFOLLOW::Init()
{
    s_lmFollow = LM{-FLT_MAXF, FLT_MAXF};
    pzone = nullptr;
    pcme = nullptr;
}

// orig: 0x00221CC0
void CPFOLLOW::Deactivate(LO* psoFocusOld, void* pv)
{
    pzone = nullptr;
    pcme = nullptr;
    CPLCY::Deactivate(psoFocusOld, pv);
}

// ---------------------------------------------------------------------------------------------------------------
// (Sly 3) the follow camera is Sly 3's (orig3 0x2747D0 activate, 0x274AC0 update, 0x1F3ED8 the focus record,
// 0x276510 the focus check, 0x273AB0 the focus point): the same plan as Sly 2's 0x221DD8, but the policy keeps its
// own focus record (the floor's focus height held through jumps, the look-ahead along the facing, the offset's
// spring), the tilt has its own rate and returns (the zone's return record +0x130), the samples carry the focus's
// tilt / distance offsets, and the stick's turn has no return toward remembered angles.
namespace
{
const LM s_lmUnit3{0.0f, 1.0f};                         // 0x341FF0
const SMP s_smpOffset3{1500.0f, 0.0f, 0.5f};            // 0x325080
const float s_aAhead3[3] = {1.5f, -0.5f, 0.0f};         // 0x325090 (of the focus's angular speed)
const float s_aTiltRate3[3] = {-0.1f, F32(0x3AF04C76), 0.0f};  // 0x3290D0 (of the lead's speed)
const SMP s_smpTilt3{0.75f, 0.0f, 2.0f};                // 0x329140 (x the rate)
const ZONECM::RET3 s_aretDefault3[4] = {{0, 2.0f, 1.0f}, {0, 2.0f, 1.0f}, {1, 0.0f, 1.0f}, {0, 2.0f, 1.0f}};  // 0x34A040
// the stick's turn table (0x345098, kind 0: the camera; {reversed by default: x, y}, the rates)
constexpr float SW_PAN_STICK3 = 2.5f, SW_TILT_STICK3 = -1.0f;

float Quad3(const float a[3], float x) { return a[0] + x * (a[1] + x * a[2]); }
float Clamp3(float g, const LM& lm) { return g < lm.gMin ? lm.gMin : (lm.gMax < g ? lm.gMax : g); }

// orig3: 0x001F3E48 (a damped spring toward the target: dt at most 0.1, the trapezoid on the position)
void SpringFocus3(const glm::vec3& posTarget, glm::vec3* ppos, glm::vec3* pv, float kDamp, float kSpring, float dt)
{
    dt = std::min(dt, 0.1f);
    const glm::vec3 a = (posTarget - *ppos) * kSpring + *pv * kDamp;
    const glm::vec3 v = *pv + a * dt;
    *ppos = *ppos + *pv * (dt * 0.5f) + v * (dt * 0.5f);
    *pv = v;
}

// orig3: 0x001F3ED8 (the focus record of the policy +0x30: the CPDEFI the update reads; *pcpoint the focus's visibility
// points (its raw +0x70))
void GetFocusCpdefi3(CPFOLLOW* p, LO* pso, bool fFirst, CPDEFI3* pc, int* pcpoint)
{
    *pc = CPDEFI3{};
    const float t = TCm();
    const float dt = t - p->tRec3;
    p->tRec3 = t;
    CPDEFIRAW3 r;
    if (g_cmfollowworld.pfnCpdefiRaw3) g_cmfollowworld.pfnCpdefiRaw3(pso, &r);
    else
    {
        r.pos = PosLoCm(pso);
        r.zObj = r.pos.z;
        r.tMoveLast = t;
        const glm::vec3 x = MatLoCm(pso)[0];
        r.radHome = std::atan2(-x.y, -x.x);
    }
    if (fFirst) r.grf &= ~6u;
    pc->cfk = r.cfk;
    pc->radHome = r.radHome;
    pc->tMoveLast = r.tMoveLast;
    pc->dTilt = r.dTilt;
    pc->dDist = r.dDist;
    pc->grf = r.grf;
    *pcpoint = r.n70;
    if (!(r.grf & 2))
    {
        const float zObj = r.zObj;
        if (!(r.grf & 4))
        {
            float z = (r.grf & 0x10) && !fFirst ? p->posRec3.z : r.pos.z;
            if (z < zObj + r.lmZ.gMin) z = zObj + r.lmZ.gMin;
            else if (zObj + r.lmZ.gMax < z) z = zObj + r.lmZ.gMax;
            p->posRec3.z = fFirst ? z : GSmooth(p->posRec3.z, z, dt, &r.smpZ, nullptr);
        }
        p->posRec3.x = r.pos.x;
        p->posRec3.y = r.pos.y;
        if (p->posRec3.z < zObj + r.lmZ2.gMin) p->posRec3.z = zObj + r.lmZ2.gMin;
        else if (zObj + r.lmZ2.gMax < p->posRec3.z) p->posRec3.z = zObj + r.lmZ2.gMax;
    }
    p->dposSmooth3 = fFirst ? glm::vec3(0.0f) : PosSmooth(p->dposSmooth3, glm::vec3(0.0f), dt, &s_smpOffset3, nullptr);
    // the look-ahead: along the facing, shorter while the focus turns fast
    const float sAhead = r.sAhead * Clamp3(Quad3(s_aAhead3, r.wAng), s_lmUnit3);
    const glm::vec3 dposAheadT = SetVectorCylind(pc->radHome + PI, sAhead, 0.0f);
    if (fFirst)
    {
        p->dposAhead3 = dposAheadT;
        p->vAhead3 = glm::vec3(0.0f);
        p->dposFocus3 = r.dposFocus;
        p->vFocus3 = glm::vec3(0.0f);
    }
    else
    {
        SpringFocus3(dposAheadT, &p->dposAhead3, &p->vAhead3, r.kDamp, r.kSpring, dt);
        SpringFocus3(r.dposFocus, &p->dposFocus3, &p->vFocus3, r.kDamp, r.kSpring, dt);
    }
    pc->pos = p->posRec3 + p->dposSmooth3 + p->dposAhead3;
    pc->v = r.v;
    pc->dposAhead = p->dposAhead3;
    pc->dposFocus = p->dposFocus3;
}

// orig3: 0x00273AB0 (the focus point: the record less (1 - uFocus) of the look-ahead; with the zone, its coordinates
// scaled by the entry's +0x70 in the zone's frame)
glm::vec3 PosCmeFocus3(const ZONECME* pcme, const CPDEFI3& c, ZONECM* pzone)
{
    glm::vec3 pos = c.pos + c.dposAhead * (pcme->uFocus - 1.0f);
    if (pzone)
    {
        std::array<glm::vec4, 3> acoef{};
        acoef[0].y = pcme->vecCoefY.x;
        acoef[1].y = pcme->vecCoefY.y;
        acoef[2].y = pcme->vecCoefY.z;
        const int fPolar = pzone->fPolar;
        pzone->fPolar = 0;
        glm::vec3 posZone, normal;
        ConvertZonePos(pzone, pos, &posZone, &normal);
        pos = PosZoneSample(pzone, posZone, acoef, pcme->alm, 1, pzone->sPath, 0.0f);
        pzone->fPolar = fPolar;
    }
    return pos;
}

// orig3: 0x00276510 (the eye blocked for 0.5 s cuts; every 0.5 s the focus's points (cpoint of the five) are tested
// against the world toward the camera; not seen for 1.5 s, the camera cuts)
void CheckFocus3(CPFOLLOW* p, int cpoint)
{
    if (g_cmfollowworld.pfnVisibility && g_cmfollowworld.pfnVisibility(p)) return;
    const float t = TCm();
    if (g_cm.fCut)   // (and 0x1A9478: the game paused)
    {
        p->tVisible = p->tClear3 = t;
        return;
    }
    if (p->pzone && p->pzone->fNoAvoid) return;
    if (F_NO_AVOID) return;
    if (0.5f < t - p->tClear3)
    {
        CutCm(&g_cm);
        return;
    }
    if (t - p->tVisible < DT_VISIBLE_CHECK) return;
    const glm::vec3 posFocus = PosLoCm(p->psoFocus);
    const glm::vec3 z(0, 0, 1);
    glm::vec3 r1 = glm::cross(z, posFocus - g_cm.pos);
    const float l = glm::length(r1);
    if (l < EPS_1E4)
    {
        p->tVisible = t;
        return;
    }
    r1 *= 1.0f / l;
    const glm::vec3 r0 = glm::cross(z, r1);
    cpoint = std::clamp(cpoint, 0, 5);
    glm::vec3 apt[5];
    glm::vec3 mn = g_cm.pos, mx = g_cm.pos;
    for (int i = 0; i < cpoint; ++i)
    {
        apt[i] = posFocus + r0 * s_adposVisible[i].x + r1 * s_adposVisible[i].y + z * s_adposVisible[i].z;
        mn = glm::min(mn, apt[i]);
        mx = glm::max(mn, apt[i]);  // (the maximum of the new minimum and the point, as the original)
    }
    const CMSOLIDS asolid = AsolidCm(mn, mx);
    for (int i = 0; i < cpoint; ++i)
    {
        glm::vec3 posScreen = ConvertCmWorldToScreen(&g_cm, apt[i]);
        posScreen.z = g_cm.sNearClip;
        const glm::vec3 posNear = ConvertCmScreenToWorld(&g_cm, posScreen);
        float uTo;
        if (FFindCmClearInterval(0.0f, asolid, apt[i], posNear, nullptr, &uTo) && 1.0f <= uTo)
        {
            p->tVisible = t;
            return;
        }
    }
    if (1.5f < t - p->tVisible) CutCm(&g_cm);
}
}  // namespace

// orig3: 0x002747D0 (the activation: the zone's entry for the focus, the eye's sphere about the focus from the
// camera's place)
void CPFOLLOW::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    pzone = static_cast<ZONECM*>(pv);
    fFirst = 1;
    tActivate3 = TCm();
    if (pzone && pzone->pacmeShared) pcme = pzone->pacmeShared;
    else if (!pzone || pzone->acme.empty()) pcme = &CmeDefault();
    else
    {
        const ZONECME* pe = &pzone->acme[0];
        for (size_t i = 1; i < pzone->acme.size(); ++i)
        {
            const ZONECME* peT = &pzone->acme[i];
            if (FIsLoDerivedFromCm(psoFocusNew, peT->cid) && FCidDerivedFrom(peT->cid, pe->cid)) pe = peT;
        }
        pcme = pe;
    }
    InitCmeTilt(const_cast<ZONECME*>(pcme));
    CPDEFI3 c;
    int cpoint = 0;
    GetFocusCpdefi3(this, psoFocusNew, true, &c, &cpoint);
    glm::vec3 posFocus = PosCmeFocus3(pcme, c, nullptr);
    if (pzone && pzone->pcrv && g_cmfollowworld.pfnCrvNearestAll)
        pzone->sPath = g_cmfollowworld.pfnCrvNearestAll(pzone->pcrv, posFocus, pzone->sPath);
    posFocus = PosCmeFocus3(pcme, c, pzone);
    const glm::vec3 a = posFocus + c.dposFocus;
    ConvertVectorToSphere(g_cm.pos - a, &radPan, &radTilt, &sDist);
    sDist = sDist - g_cm.sNearClip;
    posEye = glm::vec4(a + SetVectorSphere(radPan, radTilt, sDist), posEye.w);
    dyScreen = pcme->dyScreen;
    cpmode = 0;
    if (pzone && pzone->fCutOnActivate) CutCm(&g_cm);
    if (std::getenv("SLY2_CMCUTPRINT"))
        std::printf("follow3 activate t %.3f: zone %d cut-on-activate %d pan %.3f focus %.1f %.1f %.1f\n", TCm(), pzone && pzone->pzone ? pzone->pzone->oid : -1,
                    pzone ? pzone->fCutOnActivate : -1, radPan, posFocus.x, posFocus.y, posFocus.z);
    tVisible = TCm();
    tClear3 = TCm();
    tMoveLast = TCm();
    fLeftSamples = 0;
    fManual = 0;
    tManual = TCm() - DT_MANUAL;
    if (pzone && pzone->radFOV != 0.0f) g_cm.radFOVTarget = pzone->radFOV;
}

// orig3: 0x00274AC0
LO* g_ploPcCmDist = nullptr;
float g_uPcCmDist = 1.0f;
constinit bool g_fPcCmHoldPan = false;

void CPFOLLOW::Update(CPDEFI* pcpdefiCm, const CMJOY* pjoy)
{
    (void)pcpdefiCm;   // (Sly 3's policy keeps its own record: 0x1F3ED8 below)
    LO* pso = psoFocus;
    ZONECM* pz = pzone;
    const float dtReal = DtRealCm();   // f21
    const float dt = DtCm();
    const float t = TCm();
    CPDEFI3 c;   // sp
    int cpoint = 0;
    GetFocusCpdefi3(this, pso, g_cm.fCut != 0, &c, &cpoint);
    LO* pjt = FIsLoDerivedFromCm(pso, 0x34) ? pso : nullptr;   // fp (the player base 0x33)
    const CMFOCUS focus = pjt ? FocusOf(pjt) : CMFOCUS{};
    CheckFocus3(this, cpoint);
    const int grf = pz ? pz->grf : 4;   // s4
    const ZONECM::RET3* aret = pz ? pz->aret3 : s_aretDefault3;   // s3
    // (0x274A50: the zone path's parameter nearest to the focus)
    if (pz && pz->pcrv && g_cmfollowworld.pfnCrvNearest)
        pz->sPath = g_cmfollowworld.pfnCrvNearest(pz->pcrv, PosCmeFocus3(pcme, c, nullptr), pz->sPath);
    const glm::vec3 posFocus0 = PosCmeFocus3(pcme, c, pz);   // sp+0x60
    const glm::vec3 posFocus = posFocus0 + c.dposFocus;     // sp+0x70
    if (pz && pz->ploLook) c.grf &= ~1u;
    uLead = GSmooth(uLead, (c.grf & 1) ? 0.0f : pcme->uLead, dt, &s_smpLead, nullptr);
    tMoveLast = std::max(c.tMoveLast, tMoveLast);

    // The two samples (pan, tilt, distance) of the zone at the focus, with the focus's tilt / distance offsets.
    float s1[3] = {0, 0, 0}, s2[3] = {0, 0, 0};  // sp+0x140, sp+0x14C
    glm::vec3 normalPath(0.0f);                  // sp+0x80
    if (pz && (grf & 3))
    {
        glm::vec3 posZone;
        ConvertZonePos(pz, posFocus0, &posZone, &normalPath);
        if (pz->fPolar)
        {
            ConvertVectorToCylind(posZone, &posZone.x, &posZone.y, &posZone.z);
            if (!fFirst) posZone.x = posZone.x + GLimitAbs(RadNormalize(radPolar - posZone.x), pz->radPolarLimit);
            radPolar = posZone.x;
        }
        if (grf & 1)
        {
            const glm::vec3 pos = PosZoneSample(pz, posZone, pcme->acoef, pcme->alm, 1, pz->sPath, pcme->dtPath);
            ConvertVectorToSphere(pos - posFocus, &s1[0], &s1[1], &s1[2]);
            s1[1] += c.dTilt;
            s1[2] += c.dDist;
        }
        if (grf & 2)
        {
            const glm::vec3 pos = PosZoneSample(pz, posZone, pcme->acoef, pcme->alm, 2, pz->sPath, pcme->dtPath);
            ConvertVectorToSphere(pos - posFocus, &s2[0], &s2[1], &s2[2]);
            s2[1] += c.dTilt;
            s2[2] += c.dDist;
        }
    }
    if (fFirst) cpmode = 4;
    if (g_cm.fCut)
    {
        if (g_cm.fCut == 2)
        {
            cpmode = 4;
            radPan = g_cm.radCutPan;
            radTilt = GLimitLm(&s_lmTilt, g_cm.radCutTilt - std::atan(pcme->dyScreen * g_cm.yScreenRange));
        }
        else if (!(grf & 3))
        {
            cpmode = 4;
            radPan = c.radHome;
            radTilt = pcme->lmTilt.gMin * 0.5f + pcme->lmTilt.gMax * 0.5f;
        }
        else if (!(grf & 2)) cpmode = 1;
        else if (!(grf & 1)) cpmode = 2;
        else
            cpmode = std::fabs(RadNormalize(s1[0] - c.radHome)) < std::fabs(RadNormalize(s2[0] - c.radHome)) ? 1 : 2;
        tMoveLast = t;
        tManual = t - DT_MANUAL;
    }
    if (!(cpmode & grf) && cpmode > 0 && cpmode < 3) cpmode = 4;

    // The lead (sp+0x90): toward the zone's look object, else the focus's velocity.
    glm::vec3 vLead = c.v;
    if (pz && pz->ploLook)
    {
        glm::vec3 d = PosLoCm(pz->ploLook) - c.pos;
        d.z = 0.0f;
        const float l = glm::length(d);
        const glm::vec3 n = l < EPS_1E4 ? glm::vec3(1, 0, 0) : d * (1.0f / l);
        vLead = n * Clamp3(Quadratic(pz->alead, l), pz->lmLead);
    }

    float pan = radPan, tilt = radTilt;  // f26 / f28
    bool fTurned = false;
    const bool fMouse = pjoy->dradMousePan != 0.0f || pjoy->dradMouseTilt != 0.0f;   // (PC)
    if ((grf & 4) && !(c.grf & 8) && (0.0f < pjoy->uDeflect2 || dradPan != 0.0f || dradTilt != 0.0f || fMouse))
    {
        if (cpmode != 4 && g_cmfollowworld.pfnTurnSound) g_cmfollowworld.pfnTurnSound();
        // 0x1A7B38: the stick's turn rates by the camera kind (zone +0x19C; 0 the camera), reversed by default and by
        // Camera Options' setting (the pad's vt +0x20 / +0x28)
        const float x2 = g_pcinput.fPadInvertCameraX ? -pjoy->x2 : pjoy->x2;
        const float y2 = g_pcinput.fPadInvertCameraY ? -pjoy->y2 : pjoy->y2;
        float dradPanTarget = -std::fabs(x2) * x2 * SW_PAN_STICK3;
        float dradTiltTarget = std::fabs(y2) * y2 * SW_TILT_STICK3;
        if (!FCamReverse3(0, false)) dradPanTarget = -dradPanTarget;
        if (!FCamReverse3(0, true)) dradTiltTarget = -dradTiltTarget;
        const float ddrad = DSW_STICK * dtReal;
        dradPan = std::clamp(dradPanTarget, dradPan - ddrad, dradPan + ddrad);
        {
            const float lo = dradTilt - ddrad, hi = dradTilt + ddrad;
            dradTilt = dradTiltTarget < lo ? lo : (hi < dradTiltTarget ? hi : dradTiltTarget);
        }
        const float ang = std::atan2(std::fabs(pjoy->y2), std::fabs(pjoy->x2));
        if (ang < F32(0x3E860A92))
        {
            dradTilt = 0.0f;
            fManual = 1;
        }
        else if (F32(0x3FA78D36) < ang) dradPan = 0.0f;
        else fManual = 1;
        fLeftSamples = 1;
        pan = RadNormalize(pan + dradPan * dtReal + MOUSE_PAN_SIGN * pjoy->dradMousePan);
        tilt = GLimitLm(&s_lmTilt, RadNormalize(tilt + dradTilt * dtReal - pjoy->dradMouseTilt));
        if (g_cmfollowworld.pfnCameraTurned) g_cmfollowworld.pfnCameraTurned();   // (0x1854C8 every 5 s)
        cpmode = 4;
        tManual = t;
        fTurned = true;
    }
    // Hold the player's chosen angle when automatic rotation is off. Zones that disallow camera control,
    // explicit look targets, camera cuts, and the other scripted camera policies keep their original behavior.
    const bool fAutoRotate = vr::FAutoCameraRotation(g_pcinput.fAutoCameraRotation) || !(grf & 4) || (pz && pz->ploLook);
    if (!fTurned && cpmode == 4 && fAutoRotate)
    {
        // Passive: the pan follows the focus's motion across the view.
        const glm::vec3 dirStick = (c.grf & 8) ? glm::vec3(1, 0, 0) : NormalizeOr(glm::vec3(pjoy->x, pjoy->y, 0.0f), glm::vec3(1, 0, 0));
        glm::vec3 dFocus = posFocus - g_cm.pos;
        dFocus.z *= F32(0x3E99999A);
        const float lFocus = glm::length(dFocus);
        dFocus = lFocus < EPS_1E4 ? glm::vec3(1, 0, 0) : dFocus * (1.0f / lFocus);
        const int cfk = (!pz || pz->cfk == -1) ? c.cfk : pz->cfk;
        glm::vec3 dirSide(0.0f);   // sp+0xC0 (stale for other kinds in the original)
        if (cfk == 0) dirSide = glm::cross(glm::vec3(0, 0, 1), dFocus);
        else if (cfk == 1) dirSide = dFocus;
        dirSide = NormalizeOr(dirSide, glm::vec3(1, 0, 0));
        const float g0 = GLimitAbs(glm::dot(vLead, dirSide) / lFocus, G_LEAD_LIMIT);
        const float g5 = Clamp3(Quadratic(s_aLeadStick, dirStick.y), s_lmLeadStick);
        const float g3 = Clamp3(Quadratic(s_aLeadTime, t - tManual), s_lmUnit);
        const float panPassive = RadNormalize(pan + g0 * uLead * g5 * g3 * dt);   // f29
        float uSample = 0.0f, panTurn = panPassive, tiltTarget;                  // f25 / f21 / f27
        if (!(grf & 3)) tiltTarget = GLimitLm(&pcme->lmTilt, tilt) + c.dTilt;
        else
        {
            // Turning toward a sample: which one, and how much (f24 the pan, f27 the tilt, f22 the rate).
            float panSample, uRate;
            if (!pz->pcrv || cpmode == 1)
            {
                uSample = uRate = 1.0f;
                panSample = s1[0];
                tiltTarget = s1[1];
            }
            else if (pjt && focus.pvZoneCur == pz)
            {
                uRate = 3.0f;
                uSample = 1.0f;
                panSample = s1[0];
                tiltTarget = s1[1];
            }
            else if (cpmode == 2)
            {
                uSample = uRate = 1.0f;
                panSample = s2[0];
                tiltTarget = s2[1];
            }
            else
            {
                glm::vec3 dirFacing = MatLoCm(pso)[0];
                dirFacing.z = 0.0f;
                dirFacing = NormalizeOr(dirFacing, glm::vec3(1, 0, 0));
                const float d = glm::dot(dirFacing, ConvertVec(pz->pzone, nullptr, normalPath));
                uSample = uRate = d * d * (3.0f - (std::fabs(d) + std::fabs(d)));
                if ((grf & 2) && !(0.0f < d)) panSample = s2[0], tiltTarget = s2[1];
                else panSample = s1[0], tiltTarget = s1[1];
            }
            if (pjt)
            {
                const float vxy = std::sqrt(vLead.x * vLead.x + vLead.y * vLead.y);
                uRate *= std::min(1.0f, vxy * ((focus.n4CC != -1 && focus.n4C8 == 0) ? F32(0x3BA3D70A) : F32(0x3ADA740E)));
            }
            else uRate = 0.0f;
            uRate *= pcme->uSampleRate;
            // the pan's returns (the zone's return record: 2 after the last move / turn, then 1 until the samples are left)
            const float dtMove = t - (tMoveLast + DT_MANUAL_HOLD), dtTurn = t - (tManual + DT_MANUAL);
            if (aret[1].f)
            {
                float u = 0.0f;
                if (0.0f < aret[1].dt) u = GLimitLm(&s_lmUnit, dtMove / aret[1].dt) * GLimitLm(&s_lmUnit, dtTurn / aret[1].dt);
                else if (0.0f < dtMove && 0.0f < dtTurn) u = 1.0f;
                u *= aret[1].u;
                if (uRate < u) uSample = 1.0f, uRate = u;
            }
            if (aret[0].f && !fManual)
            {
                float u = aret[0].u;
                if (0.0f < aret[0].dt) u *= GLimitLm(&s_lmUnit, (t - tActivate3) / aret[0].dt);
                if (uRate < u) uSample = 1.0f, uRate = u;
            }
            // (the zone's own turn (vt +0x138's z) adds to the smoothing's speeds)
            const float wz = 0.0f;
            const SMP smp{s_smpSample.svFast * uRate + wz, s_smpSample.svSlow * uRate + wz, s_smpSample.dtFast};
            panTurn = RadSmooth(pan, RadNormalize(pan + RadNormalize(panSample - pan)), dt, &smp, nullptr);
            if (std::fabs(RadNormalize(panSample - panPassive)) < std::fabs(RadNormalize(panSample - panTurn))) panTurn = panPassive;
        }
        pan = RadNormalize(pan + (1.0f - uSample) * RadNormalize(panPassive - pan) + uSample * RadNormalize(panTurn - pan));
        // The tilt: toward its target at the lead's speed's rate, or the returns' (tilt 2 after the last move / turn,
        // tilt 1 until the samples are left: by default on at once)
        float uTilt = Clamp3(Quad3(s_aTiltRate3, std::sqrt(vLead.x * vLead.x + vLead.y * vLead.y)), s_lmUnit3);   // f22 / f24
        const float uTiltSpeed = uTilt;
        if (aret[3].f)
        {
            const float dtMove = t - (tMoveLast + DT_MANUAL_HOLD), dtTurn = t - (tManual + DT_MANUAL);
            float u = 0.0f;
            if (0.0f < aret[3].dt) u = GLimitLm(&s_lmUnit, dtMove / aret[3].dt) * GLimitLm(&s_lmUnit, dtTurn / aret[3].dt);
            else if (0.0f < dtMove && 0.0f < dtTurn) u = 1.0f;
            u *= aret[3].u;
            if (uTiltSpeed < u) uTilt = u;
        }
        if (aret[2].f && !fLeftSamples)
        {
            float u = aret[2].u;
            if (0.0f < aret[2].dt) u *= GLimitLm(&s_lmUnit, (t - tActivate3) / aret[2].dt);
            if (uTilt < u) uTilt = u;
        }
        const SMP smpTilt{s_smpTilt3.svFast * uTilt, s_smpTilt3.svSlow * uTilt, s_smpTilt3.dtFast};
        tilt = RadSmooth(tilt, tiltTarget, dt, &smpTilt, nullptr);
        static const bool s_fDbg = std::getenv("SLY3_CMFOLLOWDBG") != nullptr;
        if (s_fDbg)
            std::printf("follow3 t %.3f pan %.4f passive %.4f turn %.4f uSample %.3f tilt %.4f -> %.4f uTilt %.3f g0 %.3f uLead %.3f cpmode %d grf %d\n", t, pan,
                        panPassive, panTurn, uSample, tilt, tiltTarget, uTilt, g0, uLead, cpmode, grf);
        // Reaching a sample: follow it from now on (not on the first frame; on a linear path only of 3 points or more)
        bool fCheck = (grf & 3) && !fFirst;
        if (fCheck && pz->pcrv && pz->crvk == 0 && !(pz->ccrvPoint < 3)) fCheck = false;
        auto fReached = [&](float angPrev, float angSamplePrev, float ang, float angSample) {
            const float d = RadNormalize(angPrev - angSamplePrev) * RadNormalize(ang - angSample);
            if (!(d <= 0.0f) && !(std::fabs(RadNormalize(ang - angSample)) < EPS_1E4)) return false;
            return std::fabs(RadNormalize(ang - angSample)) < PI * 0.5f;
        };
        if (fCheck)
        {
            if ((grf & 1) && fReached(radPan, radPan1, pan, s1[0]) && fReached(radTilt, radTilt1, tilt, s1[1]))
                fManual = cpmode = fLeftSamples = 1;
            else if ((grf & 2) && fReached(radPan, radPan2, pan, s2[0]) && fReached(radTilt, radTilt2, tilt, s2[1]))
            {
                cpmode = 2;
                fManual = fLeftSamples = 1;
            }
        }
    }
    // (PC addition, debug menu only: a guest's aim mode holds the camera's pan unless the player turns it)
    if (g_fPcCmHoldPan && !fTurned && g_ploPcCmDist && psoFocus == g_ploPcCmDist) pan = radPan;

    // The eye's target (pan, tilt, distance) and screen offsets (f31: x, f30: y) by the mode.
    float aeye[3] = {0, 0, 0};  // sp+0x164
    float dxScreen = 0, dyScreenT = 0;
    if (cpmode == 1)
    {
        aeye[0] = s1[0]; aeye[1] = s1[1]; aeye[2] = s1[2];
        dyScreenT = pcme->dyScreen;
        dxScreen = pcme->dxScreen;
    }
    else if (cpmode == 2)
    {
        aeye[0] = s2[0]; aeye[1] = s2[1]; aeye[2] = s2[2];
        dxScreen = -pcme->dxScreen;
        dyScreenT = pcme->dyScreen;
    }
    else if (cpmode == 4)
    {
        aeye[0] = pan;
        aeye[1] = tilt;
        dyScreenT = pcme->dyScreen;
        if (!(grf & 3))
        {
            dxScreen = pcme->dxScreen;
            aeye[2] = std::fabs(pcme->dtPath) + c.dDist;
        }
        else
        {
            float u;
            if (!(grf & 2)) u = 1.0f;
            else if (!(grf & 1)) u = 0.0f;
            else
            {
                if (radPan < s1[0])
                {
                    if (radPan < s2[0])
                    {
                        if (s2[0] < s1[0]) s1[0] -= TWO_PI;
                        else s2[0] -= TWO_PI;
                    }
                }
                else if (s1[0] < radPan && s2[0] < radPan)
                {
                    if (s1[0] < s2[0]) s1[0] += TWO_PI;
                    else s2[0] += TWO_PI;
                }
                u = (radPan - s2[0]) / (s1[0] - s2[0]);
                u = u * u * (3.0f - (u + u));
            }
            aeye[2] = u * s1[2] + (1.0f - u) * s2[2];
            dxScreen = u * pcme->dxScreen + (1.0f - u) * -pcme->dxScreen;
        }
    }
    if (g_ploPcCmDist && psoFocus == g_ploPcCmDist) aeye[2] *= g_uPcCmDist;   // (PC addition, debug menu only)

    bool fFar = false;   // s0
    if (g_cm.fCut)
    {
        dsDist = 0.0f;
        ddyScreen = 0.0f;
    }
    else
    {
        fFar = 400.0f < sDist - aeye[2];
        const float rDamp = 1.0f + pcme->rDamping * (dt * R_DIST_DAMPING);
        const float rSpring = pcme->rSpring * (dt * R_DIST_SPRING);
        dsDist = rDamp * dsDist + (aeye[2] - sDist) * rSpring;
        ddyScreen = rDamp * ddyScreen + (dyScreenT - dyScreen) * rSpring;
        aeye[2] = sDist + dsDist * dt;
        dyScreenT = dyScreen + ddyScreen * dt;
    }

    glm::vec3 dposEye = SetVectorSphere(aeye[0], aeye[1], aeye[2]);
    glm::vec3 posEyeNew = posFocus + dposEye;
    bool fBlocked = false;   // sp+0x160
    if (!(pz && pz->fNoAvoid))
    {
        if (g_cm.fCut) posEyeNew = PosEyeCut(this, posFocus, posEyeNew);
        else if (!F_NO_AVOID)
        {
            // 0x167660 (the eye's move avoiding the world; *pfBlocked when it was stopped)
            const glm::vec3 posTo = posEyeNew;
            posEyeNew = PosEyeMove(glm::vec3(posEye), posTo);
            fBlocked = glm::length(posEyeNew - posTo) > 0.01f;
        }
    }
    if (!fBlocked || !fFar) tClear3 = t;
    dposEye = posEyeNew - posFocus;
    ConvertVectorToSphere(dposEye, &aeye[0], &aeye[1], &aeye[2]);
    posEye = glm::vec4(posEyeNew, posEye.w);
    posEyeNew = posFocus + dposEye * ((aeye[2] + g_cm.sNearClip) / aeye[2]);
    const float dradPanScreen = std::atan(dxScreen * g_cm.xScreenRange);      // f23
    const float dradTiltScreen = -std::atan(dyScreenT * g_cm.yScreenRange);   // f22
    if (fFirst)
    {
        if (g_cm.fCut) dradPanOffset = dradTiltOffset = ddradPanOffset = ddradTiltOffset = 0.0f;
        else
        {
            float r, z;
            ConvertVectorToCylind(dposEye, nullptr, &r, &z);
            float panCm, tiltCm;
            DecomposeRotateMatrixPanTilt(g_cm.mat, &panCm, &tiltCm);
            const float panLook = RadNormalize(aeye[0] + PI + dradPanScreen);
            const float tiltLook = RadNormalize(std::atan2(-z, r) + dradTiltScreen);
            dradPanOffset = RadNormalize(panCm - panLook);
            dradTiltOffset = RadNormalize(tiltCm - tiltLook);
            ddradPanOffset = ddradTiltOffset = 0.0f;
        }
    }
    else
    {
        ddradPanOffset += (R_OFFSET_SPRING * dradPanOffset + R_OFFSET_DAMPING * ddradPanOffset) * dt;
        ddradTiltOffset += (R_OFFSET_SPRING * dradTiltOffset + R_OFFSET_DAMPING * ddradTiltOffset) * dt;
        dradPanOffset += ddradPanOffset * dt;
        dradTiltOffset += ddradTiltOffset * dt;
    }
    SetCmLookAt(&g_cm, posEyeNew, posFocus, dradPanScreen + dradPanOffset, dradTiltScreen + dradTiltOffset);
    static const bool s_fEyePrint = std::getenv("SLY3_CMEYEPRINT") != nullptr;   // (debug: the eye per frame)
    if (s_fEyePrint)
        std::printf("cmeye t %.3f focus %.1f %.1f %.1f pan %.4f tilt %.4f dist %.1f near %.1f cpmode %d grf %d dyScreen %.3f dDist %.1f dTilt %.3f\n", t, posFocus.x, posFocus.y,
                    posFocus.z, aeye[0], aeye[1], aeye[2], g_cm.sNearClip, cpmode, grf, dyScreenT, c.dDist, c.dTilt);
    dyScreen = dyScreenT;
    radPan = aeye[0];
    radTilt = aeye[1];
    sDist = aeye[2];
    radPan1 = s1[0];
    radTilt1 = s1[1];
    radPan2 = s2[0];
    radTilt2 = s2[1];
    fFirst = 0;
    fReactivate3 = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The camera objects' view (module 0x12E600).

// orig: 0x0012E798
void CPLOOKAT::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    pcmobj = static_cast<CMOBJ*>(pv);
    if (pcmobj && !(pcmobj->radFOV == 0.0f)) SetCmFov(&g_cm, pcmobj->radFOV);
}

// orig: 0x0012E7F0
void CPLOOKAT::Deactivate(LO* psoFocusOld, void* pv)
{
    if (pcmobj == pv)
    {
        if (pcmobj && pcmobj->fCut) CutCm(&g_cm);
        pcmobj = nullptr;
    }
    CPLCY::Deactivate(psoFocusOld, pv);
}

// orig: 0x0012E858
void CPLOOKAT::Update(CPDEFI* pcpdefi, const CMJOY* pjoy)
{
    (void)pcpdefi; (void)pjoy;
    if (!pcmobj) return;
    if (pcmobj->ploTarget)
        SetCmLookAt(&g_cm, PosLoCm(pcmobj->plo), PosLoCm(pcmobj->ploTarget), 0.0f, 0.0f);
    else
    {
        // (orig3 0x130DA8 -> 0x166DE8: Sly 3 gives the camera the object's world matrix as it is; Sly 2's camera
        // object looked along -z with x to its right, the rows (-z, -x, y), which pointed v_ext's flyover at the ground)
        SetCmPosMat(&g_cm, PosLoCm(pcmobj->plo), MatLoCm(pcmobj->plo));
    }
    if (!(pcmobj->cFocal > 0)) return;
    float rad = 2.0f * std::atan(F32(0x418FF79B) / pcmobj->sFocal);
    rad = std::clamp(rad, F32(0x3C23D70A), 3.0f);
    SetCmFov(&g_cm, rad);
}

// ---------------------------------------------------------------------------------------------------------------
// The free / scripted camera (0x150728's class).

namespace
{
const SMP s_smpZoom{1.0f, 0.4f, 1.5f};        // 0x2BD9B8
const SMP s_smpScriptPos{300.0f, 100.0f, 0.5f};  // 0x2BD9C8
const SMP s_smpScriptMat{1.0f, 0.5f, 0.5f};   // 0x2BD9D8
const SMP s_smpScriptFov{1.0f, 0.4f, 1.5f};   // 0x2BD9E8
const SMP s_smpFree{100.0f, 40.0f, 1.5f};     // 0x2BD9F8
const glm::vec3 s_dposFreeEye(-50.0f, 0.0f, 0.0f);  // 0x2A5000
}

// orig: 0x00150780
glm::vec3 CPFREE::PosFocus()
{
    if (ploFocus) return PosLoCm(ploFocus);
    LO* ploPlayer = g_cmworld.pfnPloPlayer ? g_cmworld.pfnPloPlayer() : nullptr;
    if (ploPlayer) return PosLoCm(ploPlayer) + glm::vec3(0, 0, 150.0f);
    return g_cm.pos;
}

// orig: 0x001507F8
glm::vec3 CPFREE::PosEye()
{
    if (ploTarget)
    {
        const glm::vec3 d = glm::normalize(PosLoCm(ploTarget) - PosFocus());
        radPan = std::atan2(d.y, d.x);
        return PosFocus() + d * -glm::length(s_dposFreeEye);
    }
    const glm::mat3 m = BuildPanTiltMatrix(radPan, radTilt);
    return PosFocus() + m * s_dposFreeEye;
}

// orig: 0x00150EF0 (orig3: 0x16AE90, the same C in the Sly 3 decomp)
void CPFREE::Snap()
{
    posFocus = glm::vec4(PosFocus(), posFocus.w);
    posEye = glm::vec4(PosEye(), posEye.w);
    if (0.0f <= uZoomTarget) uZoom = uZoomTarget;
    posScript = posScriptTarget;
    radFOVScript = radFOVScriptTarget;
    eulScript = eulScriptTarget;
}

// orig: 0x00150EA0
void CPFREE::SetTargets(const glm::vec3& pos, const glm::vec3& eul, float radFOV)
{
    posScriptTarget = glm::vec4(pos, 0.0f);
    eulScriptTarget = glm::vec4(eul, 0.0f);
    radFOVScriptTarget = radFOV;
    if (!fScripted) Snap();
    fScripted = 1;
}

// orig: 0x00150E20 / 0x00150E60 (the scripted path ends: snapped to the objects)
void CPFREE::SetFocusObject(LO* plo)
{
    ploFocus = plo;
    if (fScripted) Snap();
    fScripted = 0;
}
void CPFREE::SetTargetObject(LO* plo)
{
    ploTarget = plo;
    if (fScripted) Snap();
    fScripted = 0;
}

// orig: 0x00150938
void CPFREE::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    StartZoomSounds(&sndZoom, &sndExtra);
    DecomposeRotateMatrixPanTilt(g_cm.mat, &radPan, &radTilt);
    radTilt = 0.0f;
    uZoom = 0.0f;
    duZoom = 0.0f;
    radFOVNear = g_cm.radFOVTarget / sZoom;
    radFOVFar = g_cm.radFOVTarget;
    SetCmNearClip(&g_cm, 49.0f);
    Snap();
    g_cm.cFreeStarts++;
    NoteBinocView();  // SW +0x4750 = t (0x150A18)
}

// orig: 0x00151130
void CPFREE::Deactivate(LO* psoFocusOld, void* pv)
{
    StopZoomSounds(&sndZoom, &sndExtra);
    ploFocus = nullptr;
    ploTarget = nullptr;
    fScripted = 0;
    uZoomTarget = -1.0f;
    CutCm(&g_cm);
    CPLCY::Deactivate(psoFocusOld, pv);
}

// orig: 0x00151240
void CPACTUATED::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    pxf = static_cast<XF*>(pv);
    SetCmNearClip(&g_cm, 25.0f);
    SetCmFov(&g_cm, F32(0x3F933333));  // 1.15
    if (pxf) pctrl = PctrlNewPos(pxf->palo, pxf, 1, 8, 3, glm::vec4(0.0f));
    tIdle = -1.0f;
    eulMouse = glm::vec3(0.0f);
}

// orig: 0x001512E0
void CPACTUATED::Deactivate(LO* psoFocusOld, void* pv)
{
    DeleteCtrl(&pctrl);
    CutCm(&g_cm);
    CPLCY::Deactivate(psoFocusOld, pv);
}

// orig: 0x00151340
void CPACTUATED::Update(CPDEFI* pcpdefi, const CMJOY* pjoy)
{
    CPLCY::Update(pcpdefi, pjoy);
    if (!pctrl || !pxf) return;
    auto* pc = static_cast<CTRLPOS*>(pctrl);
    glm::vec3 eul(0.0f), w(0.0f);
    // (PC, N14) the mouse looks around as the right stick does: its turn accumulates within the stick's reach (yaw
    // 0.5, pitch 0.4), mouse right turns right, mouse up looks up (-y); the stick takes over
    if (pjoy && (pjoy->dradMousePan != 0.0f || pjoy->dradMouseTilt != 0.0f))
    {
        eulMouse.z = std::clamp(eulMouse.z - pjoy->dradMousePan, -0.5f, 0.5f);
        eulMouse.y = std::clamp(eulMouse.y + MOUSE_TABLE_TILT_SIGN * pjoy->dradMouseTilt, F32(0xBECCCCCD), F32(0x3ECCCCCD));
    }
    if (pjoy && 0.0f < pjoy->uDeflect2) eulMouse = glm::vec3(0.0f);
    if (pjoy && 0.0f < pjoy->uDeflect2)
    {
        // D_002BDA10/30: clamped quadratics, the original's sign choices for yaw / pitch.
        // (PC, settings v17) the controller's camera inversion: off is the original's
        const float x2 = g_pcinput.fPadInvertCameraX ? -pjoy->x2 : pjoy->x2;
        const float y2 = g_pcinput.fPadInvertCameraY ? -pjoy->y2 : pjoy->y2;
        const float x = std::fabs(x2), y = std::fabs(y2);
        const float z = std::clamp(0.0f + x * (F32(0x3F8CCCCD) + x * F32(0xBE99999A)), 0.0f, 0.5f);
        const float q = std::clamp(0.0f + y * (0.5f + y * F32(0xBDCCCCCD)), 0.0f, F32(0x3ECCCCCD));
        eul.z = (0.0f < x2 ? -1.0f : 1.0f) * z;
        eul.y = (0.0f < y2 ? 1.0f : -1.0f) * q;
    }
    else
    {
        uint32_t state = ~0u;
        if (g_cmworld.pfnFieldLo) g_cmworld.pfnFieldLo(psoFocus, 0x4CC, &state);
        if (state == 9)
        {
            if (tIdle < TCm())
            {
                const float y = GRandInRange(-0.01f, 0.01f);
                const float z = GRandInRange(-0.04f, 0.04f);
                eulIdle = glm::vec3(0.0f, y, z);
                tIdle = TCm() + GRandInRange(1.5f, 2.0f);
            }
            const glm::vec3 d = eulIdle + eulMouse - glm::vec3(pc->pos);
            w = wIdle + (d * 5.0f + wIdle * -1.0f) * DtCm();
            const float halfDt = DtCm() * 0.5f;
            eul = (glm::vec3(pc->pos) + wIdle * halfDt) + w * halfDt;
        }
        else eul = eulMouse;  // (PC) moving under it: the mouse's look held
    }
    if (static const bool s_f = std::getenv("SLY2N_TABLELOOK") != nullptr; s_f)
        std::printf("tablelook: t %.3f eul %.3f %.3f mouse %.4f %.4f fwd z %.3f\n", g_clock.t, eul.y, eul.z, pjoy ? pjoy->dradMousePan : 0.0f,
                    pjoy ? pjoy->dradMouseTilt : 0.0f, glm::mat3(pxf->matWorld)[0].z);
    SetCtrlPos(pc, glm::vec4(eul, 0.0f));
    wIdle = w;
    SetCmPosMat(&g_cm, glm::vec3(pxf->matWorld[3]), glm::mat3(pxf->matWorld));
}

// orig: 0x00150A28
void CPFREE::Update(CPDEFI* pcpdefi, const CMJOY* pjoy)
{
    (void)pcpdefi; (void)pjoy;
    const float dt = DtCm();
    if (dt == 0.0f) return;
    if (0.0f <= uZoomTarget)
    {
        const float u = std::clamp(GSmooth(uZoom, uZoomTarget, dt, &s_smpZoom, nullptr), 0.0f, 1.0f);
        const float uOld = uZoom;
        uZoom = u;
        duZoom = (u - uOld) / dt;
    }
    UpdateZoomSound(sndZoom, duZoom != 0.0f, duZoom);
    if (fScripted)
    {
        posScript = glm::vec4(PosSmooth(glm::vec3(posScript), glm::vec3(posScriptTarget), dt, &s_smpScriptPos, nullptr), posScript.w);
        const glm::mat3 m0 = BuildEulerMatrix(glm::vec3(eulScript));
        const glm::mat3 m1 = BuildEulerMatrix(glm::vec3(eulScriptTarget));
        glm::mat3 m;
        SmoothMatrix(m0, m1, &s_smpScriptMat, dt, &m, nullptr);
        eulScript = glm::vec4(DecomposeEulerMatrix(m), eulScript.w);
        radFOVScript = RadSmooth(radFOVScript, radFOVScriptTarget, dt, &s_smpScriptFov, nullptr);
        SetCmPosMat(&g_cm, glm::vec3(posScript), m);
        SetCmFov(&g_cm, radFOVScript);
        DecomposeRotateMatrixPanTilt(m, &radPan, &radTilt);
        const float u = radFOVNear == radFOVFar ? 0.0f : (radFOVScript - radFOVNear) / (radFOVFar - radFOVNear);
        uZoom = GLimitLm(&s_lmUnit, u);
    }
    else
    {
        const float w = 1.0f - uZoom;
        SetCmFov(&g_cm, (1.0f - w) * radFOVNear + w * radFOVFar);
        posEye = glm::vec4(PosSmooth(glm::vec3(posEye), PosEye(), dt, &s_smpFree, nullptr), posEye.w);
        posFocus = glm::vec4(PosSmooth(glm::vec3(posFocus), PosFocus(), dt, &s_smpFree, nullptr), posFocus.w);
        SetCmLookAt(&g_cm, glm::vec3(posEye), glm::vec3(posFocus), 0.0f, 0.0f);
    }
    NoteBinocView();  // SW +0x4750 = t (0x150DB8, both paths)
}

// ---------------------------------------------------------------------------------------------------------------
// The binocucom (recon) view (0x14FDE0's class).

namespace
{
const LM s_lmReconPanJt{-10.0f, 10.0f};   // 0x2BD950
const LM s_lmReconTiltJt{-1.0f, 1.0f};    // 0x2BD958
const LM s_lmReconTiltObj{-1.5f, 1.5f};   // 0x2BD960
const LM s_lmReconObj{-1.0f, 1.0f};       // 0x2BD968 / 0x2BD970
const float s_aReconZoom[3] = {2.0f, 0.0f, -1.0f};  // 0x2BD980 (the zoom's speed by the zoom)
const float SW_RECON_PAN = F32(0x3F666666);   // 0x2BD990 (0.9)
const float SW_RECON_TILT = F32(0x3F4CCCCD);  // 0x2BD994 (0.8)
const float DSW_RECON = 10.0f;                // 0x2BD998 / 0x2BD99C
const SMP s_smpReconLimit{1.0f, 0.0f, F32(0x3E4CCCCD)};  // 0x2BD9A0
const float s_aReconRate[2] = {0.0f, F32(0x3FA66666)};   // 0x2BD9B0 (the turn's scale by the field of view)
}

// orig: 0x0014FE58 (the focus: its position, raised by 150 for a JT-derived one)
glm::vec3 CPRECON::PosFocus()
{
    glm::vec3 pos = PosLoCm(psoFocus);
    if (FIsLoDerivedFromCm(psoFocus, S3CID(0x34))) pos += glm::vec3(0, 0, 1) * 150.0f;
    return pos;
}

// orig: 0x0014FEC8 (the eye: 50 behind the focus along the view)
glm::vec3 CPRECON::PosEye()
{
    const glm::mat3 m = BuildPanTiltMatrix(radPan, radTilt);
    const glm::vec3 posFocus = PosFocus();
    return posFocus + m * glm::vec3(-50.0f, 0.0f, 0.0f);  // 0x2A5000
}

// orig: 0x0014FE28
void CPRECON::ResetZoom()
{
    uZoom = 0.0f;
    duZoom = 0.0f;
    SetCmFov(&g_cm, radFOVNear);
}

// orig: 0x0014FF38
void CPRECON::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    StartZoomSounds(&sndZoom, &sndExtra);
    DecomposeRotateMatrixPanTilt(g_cm.mat, &radPan, &radTilt);
    if (FIsLoDerivedFromCm(psoFocus, S3CID(0x34)))
    {
        lmPan = s_lmReconPanJt;
        lmTilt = s_lmReconTiltJt;
    }
    else
    {
        lmPan = s_lmReconObj;
        lmTilt = s_lmReconObj;
        radPanStart = radPan;
        radTiltStart = radTilt;
        DecomposeRotateMatrixPanTilt(MatLoCm(psoFocus), &radPan, &radTilt);
        const float t = GLimitLm(&s_lmReconTiltObj, radTilt);
        radTilt = t;
        lmPan.gMin += radPan;
        lmPan.gMax += radPan;
        // (the original adds the tilt to both limits, then stores the clamped tilt in both: the tilt is held)
        lmTilt.gMin = GLimitLm(&s_lmReconTiltObj, t);
        lmTilt.gMax = GLimitLm(&s_lmReconTiltObj, radTilt);
    }
    dradPan = 0.0f;
    dradTilt = 0.0f;
    uZoom = 0.0f;
    duZoom = 0.0f;
    radFOVNear = g_cm.radFOVTarget / sZoom;
    radFOVFar = g_cm.radFOVTarget;
    SetCmNearClip(&g_cm, 49.0f);
    cStart++;
    NoteBinocView();  // SW +0x4750 = t (0x150150)
}

// orig: 0x00150610 (the camera cut to look back the way the view looked)
void CPRECON::Deactivate(LO* psoFocusOld, void* pv)
{
    StopZoomSounds(&sndZoom, &sndExtra);
    if (FIsLoDerivedFromCm(psoFocus, S3CID(0x34))) SetCmCut(&g_cm, radPan + PI, -radTilt);
    else SetCmCut(&g_cm, radPanStart + PI, -radTiltStart);
    CPLCY::Deactivate(psoFocusOld, pv);
}

// orig: 0x00150160
void CPRECON::Update(CPDEFI* pcpdefi, const CMJOY* pjoy)
{
    (void)pcpdefi;
    const float dt = DtCm();
    // the zoom: the right stick's y
    {
        float u = uZoom;
        duZoom = Quadratic(s_aReconZoom, u) * pjoy->y2 * std::fabs(pjoy->y2);
        u = u + duZoom * dt;
        u += pjoy->cwheelZoom * 0.08f;   // (PC: the mouse wheel)
        if (u < 0.0f) u = 0.0f;
        else if (1.0f < u) u = 1.0f;
        uZoom = u;
    }
    UpdateZoomSound(sndZoom, duZoom != 0.0f && 0.0f < uZoom && uZoom < 1.0f, duZoom);
    const float w = 1.0f - uZoom;
    SetCmFov(&g_cm, (1.0f - w) * radFOVNear + w * radFOVFar);
    // the turn: the left stick, accelerating at 10 / s^2, slowed toward the limits
    float dradTiltT = -std::fabs(pjoy->y) * pjoy->y * SW_RECON_TILT;
    const float dradPanT = -std::fabs(pjoy->x) * pjoy->x * SW_RECON_PAN;
    if (g_cmfollowworld.pfnFInvertLook && g_cmfollowworld.pfnFInvertLook()) dradTiltT = -dradTiltT;
    const float dradPanA = dradPan + GLimitAbs(dradPanT - dradPan, DSW_RECON * dt);
    const float dradTiltA = dradTilt + GLimitAbs(dradTiltT - dradTilt, DSW_RECON * dt);
    const float dtHalf = dt * 0.5f;
    float dLo, dHi;
    GSmooth(radPan, lmPan.gMin, dtHalf, &s_smpReconLimit, &dLo);
    GSmooth(radPan, lmPan.gMax, dtHalf, &s_smpReconLimit, &dHi);
    dradPan = dradPanA < dLo ? dLo : (dHi < dradPanA ? dHi : dradPanA);
    GSmooth(radTilt, lmTilt.gMin, dtHalf, &s_smpReconLimit, &dLo);
    GSmooth(radTilt, lmTilt.gMax, dtHalf, &s_smpReconLimit, &dHi);
    dradTilt = dradTiltA < dLo ? dLo : (dHi < dradTiltA ? dHi : dradTiltA);
    const float s = (1.0f - g_cm.radFOV) * s_aReconRate[0] + g_cm.radFOV * s_aReconRate[1];
    radPan = radPan + s * dradPan * dt;
    radTilt = radTilt + s * dradTilt * dt;
    // (VR, vr.h: the binocucom in Immersive) the head's turn onto the angles, one to one, within the limits
    if (float dPan = 0.0f, dTilt = 0.0f; vr::FHeadLook(&dPan, &dTilt))
    {
        radPan += dPan;
        if (TWO_PI <= lmPan.gMax - lmPan.gMin) radPan = RadNormalize(radPan);
        else radPan = std::clamp(radPan, lmPan.gMin, lmPan.gMax);
        radTilt = std::clamp(radTilt + dTilt, lmTilt.gMin, lmTilt.gMax);
    }
    // (PC) the mouse aims straight onto the angles, slower while zoomed in, within the limits
    if (pjoy->dradAimRight != 0.0f || pjoy->dradAimUp != 0.0f)
    {
        const float k = radFOVFar > 1e-4f ? std::clamp(g_cm.radFOV / radFOVFar, 0.05f, 1.0f) : 1.0f;
        radPan = radPan - pjoy->dradAimRight * k;
        if (TWO_PI <= lmPan.gMax - lmPan.gMin) radPan = RadNormalize(radPan);
        else radPan = std::clamp(radPan, lmPan.gMin, lmPan.gMax);
        // (a higher radTilt looks up (the original's left stick up looks down, flight style,
        // unless "Invert Binocucom Controls" is on): mouse up looks up; only the mouse's own binocucom inversion
        // (mouseInvertBinocY, in MouseAimDelta) flips it, the game's option is the stick's)
        radTilt = std::clamp(radTilt + pjoy->dradAimUp * k, lmTilt.gMin, lmTilt.gMax);
    }
    if (TWO_PI <= lmPan.gMax - lmPan.gMin) radPan = RadNormalize(radPan);
    if (static const bool s_f = std::getenv("SLY2N_RECONTILT") != nullptr; s_f)
        std::printf("recon: t %.3f pan %.4f tilt %.4f joy %.2f %.2f mouse %.4f %.4f\n", g_clock.t, radPan, radTilt, pjoy->x, pjoy->y, pjoy->dradAimRight, pjoy->dradAimUp);
    // (debug, a test aid: SLY2N_RECONAIM="oid[:zoom]|oid..,zoom@t;..." turns the view every frame from game time t on (the
    // last entry started wins) at the first of those objects in the world (a recon subject (cid 0x2E) only while its
    // task +0xEC is not done), ignoring the limits, with that zoom)
    if (static const char* s_szAim = std::getenv("SLY2N_RECONAIM"); s_szAim && g_psw)
    {
        std::string strOids;
        float u = uZoom;
        for (const char* q = s_szAim; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            const std::string e(q, std::strchr(q, ';') ? std::strchr(q, ';') : q + std::strlen(q));
            const size_t iat = e.find('@'), ic = e.find(',');
            if (iat != std::string::npos && g_clock.t < std::strtof(e.c_str() + iat + 1, nullptr)) continue;
            strOids = e.substr(0, std::min(ic, iat));
            if (ic != std::string::npos) u = std::strtof(e.c_str() + ic + 1, nullptr);
        }
        const LO* ploAim = nullptr;
        for (size_t i = 0; i < strOids.size() && !ploAim;)
        {
            const size_t j = std::min(strOids.find('|', i), strOids.size());
            const std::string tok = strOids.substr(i, j - i);
            const int oid = std::atoi(tok.c_str());
            // ("oid:zoom": that object's own zoom, e.g. a near large target)
            const float uOid = tok.find(':') != std::string::npos ? std::strtof(tok.c_str() + tok.find(':') + 1, nullptr) : -1.0f;
            i = j + 1;
            // (of several in-world copies, the nearest to the view: e.g. Ep4's four spycam code targets 77142)
            float dBest = 3.4e38f;
            for (const auto& up : g_psw->aploAll)
                if (up->oid == oid && up->fInWorld)
                {
                    if (FCidDerivedFrom(up->Cid(), S3CID(0x2E)))
                    {
                        const int oidTask = static_cast<int>(up->U32Field(0, 0xEC, uint32_t(-1)));
                        if (const TASK3* p3 = FScripts3Active() ? PtaskFind3(oidTask) : nullptr)   // (Sly 3: task3's state)
                        {
                            if (p3->nState >= 2) continue;
                        }
                        else if (const TASK* ptask = dynamic_cast<const TASK*>(PloFindSwObjectByOid(g_psw, oidTask)))
                            if (ptask->nState >= 2) continue;
                    }
                    const float d = glm::length(PosLoWorld(up.get()) - PosFocus());
                    if (d < dBest) { dBest = d; ploAim = up.get(); if (uOid >= 0.0f) u = uOid; }
                }
        }
        if (ploAim)
        {
            const glm::vec3 d = PosLoWorld(ploAim) - PosFocus();
            radPan = std::atan2(d.y, d.x);
            const float t = std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y));
            const glm::vec3 dirA = BuildPanTiltMatrix(radPan, t) * glm::vec3(1, 0, 0);
            radTilt = glm::dot(dirA, d) >= glm::dot(BuildPanTiltMatrix(radPan, -t) * glm::vec3(1, 0, 0), d) ? t : -t;
            uZoom = u;
            SetCmFov(&g_cm, uZoom * radFOVNear + (1.0f - uZoom) * radFOVFar);
        }
    }
    const glm::vec3 posEye = PosEye();
    SetCmLookAt(&g_cm, posEye, PosFocus(), 0.0f, 0.0f);
    NoteBinocView();  // SW +0x4750 = t (the job markers shown: marker.cpp)
}

// ---------------------------------------------------------------------------------------------------------------
// The timed move (0x151610's class) and the attached view (0x1518C8's class).

namespace
{
const glm::vec3 s_acurveTimed[6] = {  // 0x2BDA70: (u^3, u^2, u) coefficients
    {0.0f, 0.0f, 1.0f}, {-1.0f, 2.0f, 0.0f}, {-1.0f, -1.0f, 1.0f}, {-2.0f, 3.0f, 0.0f}, {5.0f, 0.0f, 0.5f}, {2000.0f, 0.0f, 1.0f}};
const SMP s_smpAttachTurn{5.0f, 0.0f, 0.5f};     // 0x2BDAB0
const SMP s_smpAttachMove{2000.0f, 0.0f, 1.0f};  // 0x2BDAC0
}

// orig: 0x00151648
void CPTIMED::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    ptrans = static_cast<CMTRANS*>(pv);
    if (ptrans && ptrans->pfnStarted) ptrans->pfnStarted(ptrans->pvOwner);
}

// orig: 0x00151698
void CPTIMED::Deactivate(LO* psoFocusOld, void* pv)
{
    CPLCY::Deactivate(psoFocusOld, pv);
    CMTRANS* p = static_cast<CMTRANS*>(pv);
    if (p && p->fCut) CutCm(&g_cm);
    if (ptrans && ptrans->pfnEnded) ptrans->pfnEnded(ptrans->pvOwner);
    ptrans = nullptr;
}

// orig: 0x00151700
void CPTIMED::Update(CPDEFI* pcpdefi, const CMJOY* pjoy)
{
    (void)pcpdefi; (void)pjoy;
    if (!ptrans || !ptrans->pstateA || !ptrans->pstateB) return;
    float u = (TCm() - g_cm.tActivateCplcy) / ptrans->dt;
    if (u < 0.0f) u = 0.0f;
    else if (1.0f < u) u = 1.0f;
    const glm::vec3& k = s_acurveTimed[std::clamp(ptrans->icurve, 0, 5)];
    const float s = u * u * u * k.x + u * u * k.y + u * k.z;
    const float r = 1.0f - s;
    const CMSTATE& a = *ptrans->pstateA;
    const CMSTATE& b = *ptrans->pstateB;
    const glm::vec3 pos = a.pos * r + b.pos * s;
    const glm::vec3 eul = a.eul * r + b.eul * s;
    const float radFOV = r * a.radFOV + s * b.radFOV;
    SetCmPosMat(&g_cm, pos, BuildEulerMatrix(eul));
    SetCmFov(&g_cm, radFOV);
}

// orig: 0x00151900
void CPATTACH::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    SetCmNearClip(&g_cm, 49.0f);
    CMATTACH att;
    if (g_cmfollowworld.pfnAttach && g_cmfollowworld.pfnAttach(psoFocus, &att)) g_cm.radFOVTarget = att.radFOV;
}

// orig: 0x00151950
void CPATTACH::Update(CPDEFI* pcpdefi, const CMJOY* pjoy)
{
    (void)pcpdefi; (void)pjoy;
    CMATTACH att;
    if (!g_cmfollowworld.pfnAttach || !g_cmfollowworld.pfnAttach(psoFocus, &att)) return;
    const glm::mat3 matMount(att.matMount);
    const glm::vec3 posMount(att.matMount[3]);
    glm::mat3 mat = matMount;
    glm::vec3 pos = posMount;
    if (!g_cm.fCut && att.fSmooth)
    {
        // the turn from the mount's frame to the camera's, smoothed toward none
        float rad;
        glm::vec3 axis;
        DecomposeRotateMatrixRad(MulMat3TransposeA(matMount, g_cm.mat), &rad, &axis);
        if (!(rad < F32(0x38D1B717)))
        {
            const float g = GSmooth(rad, 0.0f, DtCm(), &s_smpAttachTurn, nullptr);
            mat = MulMat3(matMount, BuildRotateMatrixAxis(g, axis));
        }
        const glm::vec3 d = g_cm.pos - posMount;
        const float l = glm::length(d);
        if (!(l < F32(0x38D1B717)))
        {
            const float g = GSmooth(l, 0.0f, DtCm(), &s_smpAttachMove, nullptr);
            pos = posMount + d * (g / l);
        }
    }
    SetCmPosMat(&g_cm, pos, mat);
    if (g_cm.radFOVTarget == g_cm.radFOV) SetCmFov(&g_cm, att.radFOV);
    else g_cm.radFOVTarget = att.radFOV;
}

// ---------------------------------------------------------------------------------------------------------------
// Two actors framed (0x14FCA8 + vtable 0x2CD378).

namespace
{
// a + d (b + d c) clamped (the 0x2BDAD0.. pairs: a quadratic of the distance, its limits)
float GPair(const float a[3], const LM& lm, float d)
{
    float g = a[0] + d * (a[1] + d * a[2]);
    if (g < lm.gMin) g = lm.gMin;
    else if (lm.gMax < g) g = lm.gMax;
    return g;
}
const float s_aPairHeight[3] = {200.0f, F32(0x3E6147AE), 0.0f};   // 0x2BDAD0
const LM s_lmPairHeight{225.0f, 650.0f};                          // 0x2BDAE0
const float s_aPairSide[3] = {600.0f, F32(0x3EA8F5C3), 0.0f};     // 0x2BDAF0
const LM s_lmPairSide{600.0f, 1500.0f};                           // 0x2BDB00
const float s_aPairRise[3] = {200.0f, F32(0x3E666666), 0.0f};     // 0x2BDB10
const LM s_lmPairRise{0.0f, 500.0f};                              // 0x2BDB20
const float s_aPairFov[3] = {F32(0x3F19999A), F32(0x39EBEDFA), 0.0f};  // 0x2BDB30
const LM s_lmPairFov{F32(0x3F333333), F32(0x3F99999A)};          // 0x2BDB40
const float U_PAIR_MID = F32(0x3EE66666);                         // 0x2BDACC (0.45: the middle's weight of A)
}

// orig: 0x00151BF0
void CPPAIR::Activate(LO* psoFocusNew, void* pv)
{
    CPLCY::Activate(psoFocusNew, pv);
    ploA = static_cast<LO*>(pv);
    ploB = psoFocusNew;
}

// orig: 0x00151C38
void CPPAIR::Update(CPDEFI* pcpdefi, const CMJOY* pjoy)
{
    (void)pcpdefi; (void)pjoy;
    const glm::vec3 posA = PosLoCm(ploA), posB = PosLoCm(ploB);
    glm::vec3 d = posA - posB;
    d.z = 0.0f;
    const float l = glm::length(d);
    glm::vec3 posMid = posA * U_PAIR_MID + posB * (1.0f - U_PAIR_MID);
    posMid.z = 150.0f;
    glm::vec3 posTarget = posMid;
    posTarget.z += GPair(s_aPairHeight, s_lmPairHeight, l);
    const glm::vec3 dirSide = glm::normalize(glm::cross(d, glm::vec3(0, 0, 1)));
    glm::vec3 posEye = posMid + dirSide * GPair(s_aPairSide, s_lmPairSide, l);
    posEye.z += GPair(s_aPairRise, s_lmPairRise, l);
    SetCmPosMat(&g_cm, posEye, BuildLookAtMatrix(posTarget - posEye, glm::vec3(0, 0, 1)));  // 0x11B7E0
    SetCmFov(&g_cm, GPair(s_aPairFov, s_lmPairFov, l));
}
