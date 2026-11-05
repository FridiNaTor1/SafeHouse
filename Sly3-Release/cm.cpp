// CM, the camera (see cm.h). Ported from the reference's cm.cpp, m_14D6D8_p3.cpp, m_14E1A8_p3.cpp,
// m_14F030_p3.cpp (oracle-verified ports of 0x14D3D8..0x14FA88) and Sly1-Release cm.cpp (names, RecalcCm's aspect
// handling); the order of operations is the original's.
#include "cm.h"
#include "sly3/ids3.h"

#include "cplcy.h"
#include "display.h"
#include "lo.h"
#include "bsp.h"
#include "dzg.h"
#include "so.h"

#include <algorithm>
#include <cmath>
#include <cstring>

CM g_cm;
CMWORLD g_cmworld;

using namespace cmm;

namespace
{
constexpr float EPS_1E4 = F32(0x38D1B717);
const SMP s_smpStereo{20.0f, 10.0f, 0.5f};  // 0x2BD810
constexpr float DT_JOLT = 0.5f;             // 0x2BD850 {tShake, rFreq 50, poly (1, -2, 1), rot (0.05, 0, 0)}
constexpr float SW_JOLT = 50.0f;
const glm::vec3 s_rotJolt(F32(0x3D4CCCCD), 0.0f, 0.0f);  // 0x2BD880 (the position / offset / projection-shift qwords are 0)
const glm::vec3 s_posEyeDefault(0.0f, -2000.0f, 500.0f);  // 0x2BD830
const glm::vec3 s_posCenterDefault(0.0f);                 // 0x2BD840

MATRIX4 MatCam(const glm::mat3& mat, const glm::vec3& pos)
{
    MATRIX4 m;
    m.r[0] = glm::vec4(mat[0], 0.0f);
    m.r[1] = glm::vec4(mat[1], 0.0f);
    m.r[2] = glm::vec4(mat[2], 0.0f);
    m.r[3] = glm::vec4(pos, 1.0f);
    return m;
}

const CMJOY* PjoyCm()
{
    static const CMJOY s_joyNone;
    const CMJOY* pjoy = g_cmworld.pfnPjoy ? g_cmworld.pfnPjoy() : nullptr;
    return pjoy ? pjoy : &s_joyNone;
}
}  // namespace

float TCm() { return g_cmworld.pfnT ? g_cmworld.pfnT() : 0.0f; }
float DtCm() { return g_cmworld.pfnDt ? g_cmworld.pfnDt() : 1.0f / 60.0f; }
float DtRealCm() { return g_cmworld.pfnDtReal ? g_cmworld.pfnDtReal() : DtCm(); }

// orig: 0x0014D3D8 CM::CM / 0x0014F930 (the static construction of g_cm)
void StartupCm()
{
    g_cm = CM{};
    ConstructCmPolicies();
}

// orig: 0x0014D6D8 InitCm
void InitCm(CM* pcm)
{
    static bool s_fRgbaFogDefault = false;  // 0x2DDF34 / 0x2DDF30
    static uint32_t s_rgbaFogDefault = 0;
    if (!s_fRgbaFogDefault)
    {
        s_fRgbaFogDefault = true;
        s_rgbaFogDefault = 0x80303030;
    }
    pcm->sNearClip = 100.0f;
    pcm->sNearFog = 500.0f;    // 0x2BD81C
    pcm->sFarFog = 20000.0f;   // 0x2BD820
    pcm->uFogMax = 0.5f;       // 0x2BD824
    pcm->rAspect = F32(0x3FA63BDA);
    pcm->sFarClip = 100000.0f;
    pcm->rMRDAdjust = 1.0f;
    pcm->rMRD = 1.0f;
    pcm->radFOVTarget = 1.0f;
    pcm->radFOV = 1.0f;
    pcm->rgbaFog = s_rgbaFogDefault;
    pcm->smpaRadFOV = SMPA{0.5f, 0.0f, 0.1f, 10.0f};
    pcm->mat = glm::mat3(1.0f);
    pcm->pos = glm::vec3(0.0f);
    RecalcCm(pcm);
    SetCmLookAt(pcm, s_posEyeDefault, s_posCenterDefault, 0.0f, 0.0f);
    CutCm(pcm);
    pcm->ccpr = 0;
    InitCmPolicies(pcm);
    pcm->ploCur = nullptr;
    pcm->nCur = 0xFF;
    SetCmPolicy(pcm, 0, PcplcyDefault(), nullptr, nullptr);
    pcm->nJolt = -1;
    pcm->tJolt = -10.0f;
}

// orig: 0x0014D650 ResetCm
void ResetCm(CM* pcm)
{
    for (int i = 0; i < pcm->ccpr; ++i) pcm->acpr[i].pcplcy->Reset();
    pcm->sStereo = 0.0f;
    pcm->sStereoTarget = 10.0f;
}

// orig: 0x0014D478 UpdateCm
void UpdateCm(CM* pcm)
{
    if (pcm->ccpr > 0)
    {
        CPLCY* pcplcyPrev = nullptr;
        LO* psoLast = nullptr;
        CPLCY* pcplcy;
        while ((pcplcy = pcm->acpr[0].pcplcy) != pcplcyPrev)
        {
            LO* psoFocus = pcm->acpr[0].psoFocus;
            CPDEFI* pcpdefi;
            if (psoFocus)
            {
                pcpdefi = &pcm->cpdefiPrev;
                if (psoFocus != psoLast)
                {
                    if (g_cmworld.pfnGetSoCpdefi) g_cmworld.pfnGetSoCpdefi(psoFocus, pcpdefi);
                    if (pcplcyPrev || psoFocus != pcm->psoFocusPrev) ResetCmAdjust(pcm, pcpdefi);
                    psoLast = psoFocus;
                    AdjustCmCpdefi(pcm, psoFocus, pcpdefi);
                    pcm->psoFocusPrev = psoFocus;
                }
            }
            else
            {
                pcm->psoFocusPrev = nullptr;
                pcpdefi = nullptr;
                psoLast = nullptr;
            }
            pcplcy->Update(pcpdefi, PjoyCm());
            pcplcyPrev = pcplcy;
            if (pcm->ccpr <= 0) break;
        }
    }
    if (pcm->fCut)
        SetCmFov(pcm, pcm->radFOVTarget);
    else if (!(pcm->radFOVTarget == pcm->radFOV))
    {
        pcm->radFOV = GSmoothA(pcm->radFOV, pcm->dradFOV, pcm->radFOVTarget, DtCm(), &pcm->smpaRadFOV, &pcm->dradFOV);
        RecalcCm(pcm);
    }
    pcm->fCut = 0;
    if (g_cmworld.pfnFStereo && g_cmworld.pfnFStereo())
        pcm->sStereo = GSmooth(pcm->sStereo, pcm->sStereoTarget, DtRealCm(), &s_smpStereo, nullptr);
}

// orig: 0x0014D8C0 CutCm
void CutCm(CM* pcm) { pcm->fCut = std::max(1, pcm->fCut); }

// orig: 0x0014D8D8 SetCmCut
void SetCmCut(CM* pcm, float radPan, float radTilt)
{
    pcm->radCutTilt = radTilt;
    pcm->radCutPan = radPan;
    pcm->fCut = std::max(2, pcm->fCut);
}

// orig: 0x0014D8F8 CombineEyeLookAtProj
MATRIX4 CombineEyeLookAtProj(const MATRIX4& matCam, const MATRIX4& matProj)
{
    MATRIX4 m;
    m.r[0] = glm::vec4(-glm::vec3(matCam.r[1]), 0.0f);
    m.r[1] = glm::vec4(-glm::vec3(matCam.r[2]), 0.0f);
    m.r[2] = glm::vec4(glm::vec3(matCam.r[0]), 0.0f);
    m.r[3] = glm::vec4(glm::vec3(matCam.r[3]), 1.0f);
    return MulMatrix4(matProj, InverseAffineMatrix4(m));
}

// orig: 0x0014DA08 BuildFrustrum
void BuildFrustrum(const glm::mat3& mat, float rx, float ry, glm::vec4 anormal[4])
{
    const glm::vec3 r0 = mat[0], r1 = mat[1], r2 = mat[2];
    auto dir = [](const glm::vec3& a, const glm::vec3& b, float g) { return glm::normalize(a + b * g); };
    anormal[0] = glm::vec4(glm::cross(r2, dir(r0, r1, rx)), 0.0f);
    anormal[1] = glm::vec4(glm::cross(dir(r0, r1, -rx), r2), 0.0f);
    anormal[2] = glm::vec4(glm::cross(dir(r0, r2, ry), r1), 0.0f);
    anormal[3] = glm::vec4(glm::cross(r1, dir(r0, r2, -ry)), 0.0f);
}

// orig: 0x0014DB70 RecalcCm (RecalcCmFrustrum)
void RecalcCm(CM* pcm)
{
    const float t = std::tan(pcm->radFOV * 0.5f);
    const float rFov = 1.0f / pcm->radFOV;
    pcm->yScreenRange = t;
    pcm->xScreenRange = t * pcm->rAspect;
    pcm->rMRDAdjust = std::max(1.0f, pcm->rMRD * rFov);
    pcm->sRadiusNearClip = t * std::sqrt(pcm->rAspect * pcm->rAspect + 1.0f) * pcm->sNearClip + 1.0f;
    const float ry = 224.0f / (t * 4096.0f);
    const float rx = 512.0f / (pcm->xScreenRange * 4096.0f);
    pcm->matProj = BuildSimpleProjectionMatrix(rx, ry, 0.0f, 0.0f, pcm->sNearClip, pcm->sFarClip);
    BuildCmFogRecip(pcm, pcm->afFogRecip, 1.0f);
    UpdateCmMat4(pcm);
}

// orig: 0x0014DC58 UpdateCmMat4
void UpdateCmMat4(CM* pcm)
{
    const float dt = TCm() - pcm->tJolt;
    if (DT_JOLT < dt)
        pcm->matWorldToClip = CombineEyeLookAtProj(MatCam(pcm->mat, pcm->pos), pcm->matProj);
    else
    {
        // The jolt: a rotation about the camera's x by 0.05 sin(50 dt) (1 - u)^2; the original's position, offset
        // and projection-shift terms are zero vectors in the data (0x2BD870 / 0x2BD890 / 0x2BD8A0).
        const float u = dt / DT_JOLT;
        const float g = std::clamp(1.0f + u * (-2.0f + u * 1.0f), 0.0f, 1.0f);  // 0x2BBB60 {0, 1}
        const float s = std::sin(dt * SW_JOLT) * g;
        const glm::mat3 matJolt = BuildRotateMatrixVector(s_rotJolt * s);
        const glm::mat3 mat = MulMat3(matJolt, pcm->mat);
        pcm->matWorldToClip = CombineEyeLookAtProj(MatCam(mat, pcm->pos), pcm->matProj);
    }
    if (!FInvertMatrix4(pcm->matWorldToClip, &pcm->matClipToWorld)) pcm->matClipToWorld = MATRIX4{};
    BuildFrustrum(pcm->mat, pcm->xScreenRange, pcm->yScreenRange, pcm->anormalFrustrum.data());
    for (int i = 0; i < 3; ++i)
        pcm->anormalFrustrumTranspose[i] = glm::vec4(pcm->anormalFrustrum[0][i], pcm->anormalFrustrum[1][i],
                                                     pcm->anormalFrustrum[2][i], pcm->anormalFrustrum[3][i]);
    BuildFrustrum(pcm->mat, pcm->xScreenRange * F32(0x40FDC28F), pcm->yScreenRange * F32(0x4190CCCD), pcm->anormalFrustrumWide.data());
    for (int i = 0; i < 3; ++i)
        pcm->anormalFrustrumTransposeWide[i] = glm::vec4(pcm->anormalFrustrumWide[0][i], pcm->anormalFrustrumWide[1][i],
                                                         pcm->anormalFrustrumWide[2][i], pcm->anormalFrustrumWide[3][i]);
    SortCmObjects(pcm);
}

// orig: 0x0014E148 SetCmCurObject
void SetCmCurObject(CM* pcm, LO* plo)
{
    if (plo == pcm->ploCur) return;
    pcm->ploCur = plo;
    uint32_t n = 0xFF;
    if (g_cmworld.pfnFieldLo) g_cmworld.pfnFieldLo(plo, 0xE0, &n);
    pcm->nCur = static_cast<int>(n);
}

// orig: 0x0014E078 SortCmObjects: the world's objects of kind 0x105 (0x15E9C8, at most 32) each measured from the
// camera (0x2273F0), sorted (0x14E000: +0x110, +0xE4 descending, the distance +0x114, the address); the first becomes
// the current one when its +0x110 is 1.
// (the world part through g_cmworld.pfnCameraVolume: cmworld.cpp)
void SortCmObjects(CM* pcm)
{
    if (!g_cmworld.pfnCameraVolume) return;
    if (LO* plo = g_cmworld.pfnCameraVolume(pcm->pos)) SetCmCurObject(pcm, plo);
}

// orig: 0x0014E168 SetCmPosMat
void SetCmPosMat(CM* pcm, const glm::vec3& pos, const glm::mat3& mat)
{
    pcm->mat = mat;
    pcm->pos = pos;
    UpdateCmMat4(pcm);
}

// orig: 0x0014E1A8 SetCmLookAt
void SetCmLookAt(CM* pcm, const glm::vec3& pos, const glm::vec3& posTarget, float dradPan, float dradTilt)
{
    float radPan, radTilt;
    ConvertVectorToSphere(posTarget - pos, &radPan, &radTilt, nullptr);
    pcm->mat = BuildPanTiltMatrix(radPan + dradPan, radTilt + dradTilt);
    pcm->pos = pos;
    UpdateCmMat4(pcm);
}

// orig: 0x0014E270 BuildCmFogRecip
void BuildCmFogRecip(CM* pcm, float af[4], float g)
{
    if (g == F32(0x7F7FFFFF)) g = 1.0f;  // 0x2A4FF8 FLT_MAX
    const float f0 = pcm->uFogMax * g;
    FitRecipFunction(1.0f, 4080.0f, pcm->sFarFog / pcm->sNearFog, (1.0f - f0) * 4080.0f, &af[0], &af[1]);
    const float a0 = af[0] + F32(0x4B007000);
    af[0] = a0;
    af[3] = a0 + 8192.0f;
    af[2] = pcm->sNearFog;
}

// orig: 0x0014E328 SetCmPolicy
void SetCmPolicy(CM* pcm, int cpp, CPLCY* pcplcy, LO* psoFocus, void* pv)
{
    if (!(pcm->ccpr < 8)) return;
    int i = 0;
    for (; i < pcm->ccpr; ++i)
        if (pcm->acpr[i].cpp < cpp) break;
    for (int j = pcm->ccpr; j > i; --j) pcm->acpr[j] = pcm->acpr[j - 1];
    pcm->acpr[i] = CPR{cpp, pcplcy, psoFocus, pv};
    pcm->ccpr++;
    pcplcy->Set(pv);
    if (i != 0) return;
    if (!(pcm->ccpr < 2)) pcm->acpr[1].pcplcy->Deactivate(pcm->acpr[1].psoFocus, pcm->acpr[1].pv);
    pcm->tActivateCplcy = TCm();
    pcplcy->Activate(psoFocus, pv);
}

// orig: 0x0014E4C8 RevokeCmPolicy
void RevokeCmPolicy(CM* pcm, int grf, int cpp, CPLCY* pcplcy, LO* psoFocus, void* pv)
{
    CPLCY* pcplcyTop = nullptr;
    LO* psoTop = nullptr;
    void* pvTop = nullptr;
    int c = 0;
    const int ccpr = pcm->ccpr;
    for (int i = 0; i < ccpr; ++i)
    {
        const CPR cpr = pcm->acpr[i];
        bool fMatch = true;
        if ((grf & 1) && cpp != cpr.cpp) fMatch = false;
        else if ((grf & 2) && pcplcy != cpr.pcplcy) fMatch = false;
        else if ((grf & 4) && psoFocus != cpr.psoFocus) fMatch = false;
        else if ((grf & 8) && pv != cpr.pv) fMatch = false;
        if (fMatch)
        {
            if (i == 0)
            {
                pcplcyTop = cpr.pcplcy;
                psoTop = cpr.psoFocus;
                pvTop = cpr.pv;
            }
            cpr.pcplcy->Revoke(cpr.pv);
        }
        else pcm->acpr[c++] = cpr;
    }
    pcm->ccpr = c;
    if (!pcplcyTop) return;
    pcplcyTop->Deactivate(psoTop, pvTop);
    if (pcm->ccpr <= 0) return;
    pcm->tActivateCplcy = TCm();
    pcm->acpr[0].pcplcy->Activate(pcm->acpr[0].psoFocus, pcm->acpr[0].pv);
}

// orig: 0x0014E698 CalcCmAdjust
glm::vec4 CalcCmAdjust(CM* pcm, const CPDEFI* pcpdefi)
{
    (void)pcm;
    return glm::vec4(SetVectorCylind(pcpdefi->radHome + PI, pcpdefi->sAdjust, 0.0f), 0.0f);
}

// orig: 0x0014E6D0 SpringCm
void SpringCm(CM* pcm, glm::vec4* ppos, glm::vec4* pv, const glm::vec4& posTarget, float rSpring, float rDamping)
{
    if (pcm->fCut)
    {
        *ppos = posTarget;
        pv->x = pv->y = pv->z = 0.0f;
        return;
    }
    const float dt = DtCm();
    const glm::vec4 dv = (posTarget - *ppos) * rSpring + *pv * rDamping;
    glm::vec4 v = *pv + dv * dt;
    v.w = 1.0f + pv->w + dv.w * dt;  // (the w lane gains 1: ACC = vf0 + v)
    *pv = v;
    glm::vec4 pos = *ppos + v * dt;
    pos.w = 1.0f + ppos->w + v.w * dt;
    *ppos = pos;
}

// orig: 0x0014E768 AdjustCmCpdefi
void AdjustCmCpdefi(CM* pcm, LO* psoFocus, CPDEFI* pcpdefi)
{
    const glm::vec4 dposAdjust = CalcCmAdjust(pcm, pcpdefi);
    const bool fJt = g_cmworld.pfnFIsLoDerivedFrom && g_cmworld.pfnFIsLoDerivedFrom(psoFocus, Icid(CID3::PLAYER));
    if (fJt)
    {
        const int jtfock = g_cmworld.pfnJtfock ? g_cmworld.pfnJtfock(psoFocus) : 0;
        if (jtfock == 0)
        {
            SpringCm(pcm, &pcm->dposAdjust, &pcm->vAdjust, dposAdjust, 5.0f, -3.5f);
            SpringCm(pcm, &pcm->dposFocus, &pcm->vFocus, glm::vec4(0.0f), 5.0f, -3.5f);
        }
        else if (jtfock == 1)
        {
            SpringCm(pcm, &pcm->dposAdjust, &pcm->vAdjust, dposAdjust, 1.0f, -10.0f);
            SpringCm(pcm, &pcm->dposFocus, &pcm->vFocus, pcm->dposFocus, 0.0f, -10.0f);
        }
        else if (jtfock == 2)
            SpringCm(pcm, &pcm->dposAdjust, &pcm->vAdjust, dposAdjust, 5.0f, -3.5f);
    }
    else
    {
        SpringCm(pcm, &pcm->dposAdjust, &pcm->vAdjust, dposAdjust, 5.0f, -3.5f);
        SpringCm(pcm, &pcm->dposFocus, &pcm->vFocus, glm::vec4(0.0f), 5.0f, -3.5f);
    }
    pcpdefi->posAdjust = pcpdefi->posBase + pcm->dposAdjust;
}

// orig: 0x0014E920 ResetCmAdjust
void ResetCmAdjust(CM* pcm, const CPDEFI* pcpdefi)
{
    pcm->dposAdjust = CalcCmAdjust(pcm, pcpdefi);
    pcm->vAdjust = glm::vec4(0.0f, 0.0f, 0.0f, pcm->vAdjust.w);
    pcm->vFocus = glm::vec4(0.0f, 0.0f, 0.0f, pcm->vFocus.w);
}

// orig: 0x0014E968 GetCmCpdefi
void GetCmCpdefi(CM* pcm, LO* psoFocus, CPDEFI* pcpdefi)
{
    if (psoFocus == pcm->psoFocusPrev)
    {
        *pcpdefi = pcm->cpdefiPrev;
        return;
    }
    if (g_cmworld.pfnGetSoCpdefi) g_cmworld.pfnGetSoCpdefi(psoFocus, pcpdefi);
    pcpdefi->posAdjust = pcpdefi->posBase + CalcCmAdjust(pcm, pcpdefi);
}

// orig: 0x0020FAA0 (the world's attachments whose boxes meet the box of a and b: their entries, filtered by
// 0x14F030 (the entries with a tree), at most 256)
CMSOLIDS AsolidCm(const glm::vec3& a, const glm::vec3& b)
{
    CMSOLIDS asolid;
    const glm::vec3 mn = glm::min(a, b), mx = glm::max(a, b);
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
        {
            // 0x14F030: +0x70 > 0, the entry's camera hull (its tree's first +0x70 nodes; the third word of the
            // tree's header, BSP::x8; 0 for an actor's entry such as Sly's)
            if (psoe->bsp.abspn.empty() || !(0 < static_cast<int>(psoe->bsp.x8))) continue;
            if (asolid.size() < 0x100) asolid.push_back(psoe);
        }
    }
    return asolid;
}

// (the earlier stand-in filter, kept for the non-camera users of this box query (the dart's aim 0x1356E8 with
// 0x20FA10, the markers): the entries with a tree and no moving-points mesh)
CMSOLIDS AsolidWithTree(const glm::vec3& a, const glm::vec3& b)
{
    CMSOLIDS asolid;
    const glm::vec3 mn = glm::min(a, b), mx = glm::max(a, b);
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
        {
            if (psoe->bsp.abspn.empty() || psoe->pmesh) continue;
            if (asolid.size() < 0x100) asolid.push_back(psoe);
        }
    }
    return asolid;
}

void DumpCmSolids(const CMSOLIDS& asolid)   // (debugging: the entries' owners)
{
    for (SOE* psoe : asolid)
        std::printf("  solid of %d: nodes %zu mesh %d stamp %d f48 %d f49 %d f50 %d f51 %d state %d pinned %d id %u\n", psoe->palo ? psoe->palo->oid : -1, psoe->bsp.abspn.size(),
                    psoe->pmesh ? 1 : 0, psoe->nStampWorld, psoe->f48, psoe->f49, psoe->f50, psoe->f51, psoe->state, psoe->pinned, psoe->id);
}

// orig: 0x0014EA08 FFindCmClearInterval: the first gap longer than duMin of the segment posFrom..posTo through the
// entries' trees (0x118668 each, merged by 0x14C9F8)
bool FFindCmClearInterval(float duMin, const CMSOLIDS& asolid, const glm::vec3& posFrom, const glm::vec3& posTo,
                          float* puFrom, float* puTo)
{
    constexpr int clsgMax = 0x40;
    LSG alsg[clsgMax];
    int c = 0;
    for (SOE* psoe : asolid)
    {
        const int cNew = ClipSegmentEntryLimit(psoe, posFrom, posTo, clsgMax - c, alsg + c, static_cast<int>(psoe->bsp.x8));
        if (cNew != 0) c = CMergeAlsg(c, alsg, cNew, alsg + c, clsgMax, alsg, 0);
    }
    float g0 = 0.0f, g1 = 1.0f;
    for (int j = 0;; ++j)
    {
        if (c < j) return false;
        g0 = j > 0 ? alsg[j - 1].t1 : 0.0f;
        g1 = j < c ? alsg[j].t0 : 1.0f;
        if (duMin < g1 - g0) break;
    }
    if (puFrom) *puFrom = g0;
    if (puTo) *puTo = g1;
    return true;
}

// orig: 0x0014EB70 FindCmClosestClearPos: rays on 9 rings (1, 5, 10, 12, 15, 12, 10, 5, 1 rays at 0x2BD8B0) of
// radius r around pos; the closest blocked point pulled back by 5 / r.
glm::vec3 FindCmClosestClearPos(const CMSOLIDS& asolid, const glm::vec3& pos, float sRadius)
{
    static const int s_acRay[9] = {1, 5, 10, 12, 15, 12, 10, 5, 1};
    const float du = 5.0f / sRadius;
    glm::vec3 posBest = pos;
    float uBest = F32(0x7F7FFFFF);
    for (int i = 0; i < 9; ++i)
    {
        const float radTilt = static_cast<float>(i - 4) * (PI / 8.0f);
        for (int k = 0; k < s_acRay[i]; ++k)
        {
            const glm::vec3 dir = SetVectorSphere(static_cast<float>(k) * PI / static_cast<float>(s_acRay[i]), radTilt, 1.0f);
            const glm::vec3 posEnd = pos + dir * sRadius;
            float uFrom, uTo;
            if (!FFindCmClearInterval(du + du, asolid, pos, posEnd, &uFrom, &uTo)) continue;
            if (0.0f < uFrom) uFrom += du;
            if (!(uFrom < uBest)) continue;
            uBest = uFrom;
            posBest = pos * (1.0f - uBest) + posEnd * uBest - dir;
        }
    }
    return posBest;
}

// orig: 0x0014F040 (the avoiding push's weight of a hit: the plane's distance smoothed toward the near radius less
// 125 (0x2BD8E0: 2500, 0, 0.25 over dtReal), the eye's velocity along the plane's normal less the smoothing's speed.
// Its second argument (the eye's position) is unused: the dot product is with the velocity, the third)
float GCmAvoidWeight(CM* pcm, const glm::vec3& pos, const glm::vec3& v, const CMHIT& hit)
{
    (void)pos;
    static const SMP s_smp = {F32(0x451C4000), 0.0f, F32(0x3E800000)};
    const glm::vec3 n(hit.plane);
    const float g = ((n.x * hit.pos.x + n.y * hit.pos.y) + n.z * hit.pos.z) - hit.plane.w;
    float dg = 0.0f;
    GSmooth(g, pcm->sRadiusNearClip - F32(0x42FA0000), DtRealCm(), &s_smp, &dg);
    return ((n.x * v.x + n.y * v.y) + n.z * v.z) - dg;
}

// orig: 0x00223A10 (the cut's weight: the point's distance from the hit's plane)
float GCmCutWeight(CM* pcm, const glm::vec3& pos, const glm::vec3& v, const CMHIT& hit)
{
    (void)pcm; (void)v;
    const glm::vec3 n(hit.plane);
    return ((n.x * pos.x + n.y * pos.y) + n.z * pos.z) - hit.plane.w;
}

// orig: 0x0014F110 (the next eye position from pos toward posTarget this frame, avoiding the world's entries in the
// box of 250 about pos: 0x14EDA0 SquishCmEye with 0x14F040, radius 125, at most |v| + 1500)
glm::vec3 PosCmAvoid(CM* pcm, const glm::vec3& pos, const glm::vec3& posTarget)
{
    const float dt = DtRealCm();
    const glm::vec3 v = (posTarget - pos) * (1.0f / dt);
    const CMSOLIDS asolid = AsolidCm(pos - glm::vec3(250.0f), pos + glm::vec3(250.0f));
    const float s = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
    const glm::vec3 vOut = SquishCmEye(pcm, asolid, GCmAvoidWeight, pos, v, F32(0x42FA0000), s + F32(0x44BB8000));
    static const bool s_fDbg = std::getenv("SLY2N_CMAVOIDDBG") != nullptr;
    if (s_fDbg && glm::length(vOut - v) * dt > 0.01f)
    {
        std::printf("cmavoid t %.3f pos %.1f %.1f %.1f target %.1f %.1f %.1f push %.2f %.2f %.2f solids %zu\n", TCm(), pos.x, pos.y,
                    pos.z, posTarget.x, posTarget.y, posTarget.z, (vOut - v).x * dt, (vOut - v).y * dt, (vOut - v).z * dt,
                    asolid.size());
        DumpCmSolids(asolid);
    }
    return pos + vOut * dt;
}

// orig: 0x0014EDA0 SquishCmEye: the hits of the sphere (radius sRadius) at pos with the entries (0x1A93C8, at most 64),
// each weighted by pfn; the normals' Gram system solved (0x11AD30, limit 4 sMax) for the push, limited to sMax
// (0x11E9F8), added to v.
glm::vec3 SquishCmEye(CM* pcm, const CMSOLIDS& asolid, PFNCMHITWEIGHT pfn, const glm::vec3& pos, const glm::vec3& v,
                      float sRadius, float sMax)
{
    std::vector<glm::vec3> apos;
    std::vector<glm::vec4> aplane;
    std::vector<int> akind;
    std::vector<const void*> apv;
    int c = 0;
    for (SOE* psoe : asolid)
        c += CsbiSphereEntry(pos, psoe, nullptr, 0x40 - c, sRadius, &apos, &aplane, &akind, &apv, static_cast<int>(psoe->bsp.x8));
    if (c == 0) return v;
    static const bool s_fDbg = std::getenv("SLY2N_CMAVOIDDBG") != nullptr;
    if (s_fDbg)
        for (int k = 0; k < c; ++k)
            std::printf("  contact %d kind %d pos %.1f %.1f %.1f plane %.3f %.3f %.3f %.1f g %.2f w %.2f\n", k, akind[k], apos[k].x,
                        apos[k].y, apos[k].z, aplane[k].x, aplane[k].y, aplane[k].z, aplane[k].w,
                        glm::dot(glm::vec3(aplane[k]), apos[k]) - aplane[k].w, pfn(pcm, pos, v, CMHIT{apos[k], aplane[k]}));
    std::vector<float> aa(static_cast<size_t>(c) * c), ab(c), ax(c);
    for (int i = 0; i < c; ++i)
        for (int j = 0; j < c; ++j)
        {
            const glm::vec4& ni = aplane[i];
            const glm::vec4& nj = aplane[j];
            aa[static_cast<size_t>(i) * c + j] = (ni.x * nj.x + ni.y * nj.y) + nj.z * ni.z;
        }
    for (int k = 0; k < c; ++k) ab[k] = pfn(pcm, pos, v, CMHIT{apos[k], aplane[k]});
    SolveDzgConstraints(c, aa.data(), ab.data(), ax.data(), F32(0x40800000) * sMax);
    glm::vec3 s(0.0f);
    for (int k = 0; k < c; ++k) s += glm::vec3(aplane[k]) * ax[k];
    return v + LimitVectorLength(s, sMax);
}

// orig: 0x0014F268 ConvertCmScreenToWorld
glm::vec3 ConvertCmScreenToWorld(const CM* pcm, const glm::vec3& posScreen)
{
    glm::vec3 v(posScreen.z, -posScreen.x, posScreen.y);
    if (EPS_1E4 < posScreen.z)
    {
        v.z = posScreen.y * posScreen.z;
        v.y = -posScreen.x * posScreen.z;
    }
    v.y *= pcm->xScreenRange;
    v.z *= pcm->yScreenRange;
    return pcm->mat * v + pcm->pos;
}

// orig: 0x0014F308 ConvertCmWorldToScreen
glm::vec3 ConvertCmWorldToScreen(const CM* pcm, const glm::vec3& posWorld)
{
    const MATRIX4 inv = InverseAffineMatrix4(MatCam(pcm->mat, pcm->pos));
    const glm::vec3 r = glm::vec3(inv.r[0]) * posWorld.x + glm::vec3(inv.r[1]) * posWorld.y +
                        glm::vec3(inv.r[2]) * posWorld.z + glm::vec3(inv.r[3]);
    float a = r.z / pcm->yScreenRange;
    float b = -r.y / pcm->xScreenRange;
    if (EPS_1E4 < r.x)
    {
        a /= r.x;
        b /= r.x;
    }
    return glm::vec3(b, a, r.x);
}

// orig: 0x0014F5A0 SetCmFov
void SetCmFov(CM* pcm, float radFOV)
{
    const bool fChanged = std::fabs(radFOV - pcm->radFOV) > EPS_1E4;
    pcm->radFOVTarget = radFOV;
    pcm->radFOV = radFOV;
    pcm->dradFOV = 0.0f;
    if (fChanged) RecalcCm(pcm);
}

// orig: 0x0014F5F8 SetCmFovTarget (and the top policy's saved one)
void SetCmFovTarget(CM* pcm, float radFOV)
{
    pcm->radFOVTarget = radFOV;
    if (pcm->acpr[0].pcplcy && pcm->ccpr > 0) pcm->acpr[0].pcplcy->radFOVTargetSave = radFOV;
}

// orig: 0x0014F610 SetCmNearClip
void SetCmNearClip(CM* pcm, float sNearClip)
{
    pcm->sNearClip = sNearClip;
    RecalcCm(pcm);
}

// orig: 0x0014F630 (the near clip and the top policy's saved one)
void SetCmNearClipAll(CM* pcm, float sNearClip)
{
    SetCmNearClip(pcm, sNearClip);
    if (pcm->acpr[0].pcplcy && pcm->ccpr > 0) pcm->acpr[0].pcplcy->sNearClipSave = sNearClip;
}

// orig: 0x0014F670 / 0x0014F690 / 0x0014F6B0 / 0x0014F6D0 / 0x0014F730
void SetCmFarClip(CM* pcm, float sFarClip) { pcm->sFarClip = sFarClip; RecalcCm(pcm); }
void SetCmNearFog(CM* pcm, float sNearFog) { pcm->sNearFog = sNearFog; RecalcCm(pcm); }
void SetCmFarFog(CM* pcm, float sFarFog) { pcm->sFarFog = sFarFog; RecalcCm(pcm); }
void SetCmUFogMax(CM* pcm, float uFogMax) { pcm->uFogMax = uFogMax; RecalcCm(pcm); }
void SetCmMrdRatio(CM* pcm, float rMRD) { pcm->rMRD = rMRD; RecalcCm(pcm); }

// orig: 0x0014F6F0 SetCmRgbaFog (0x1ED278: the colour through the colour adjustment when it is on; the renderer's)
void SetCmRgbaFog(CM* pcm, uint32_t rgba)
{
    pcm->rgbaFog = rgba;
    RecalcCm(pcm);
}

// orig: 0x0014F750 JoltCm
void JoltCm(CM* pcm, int n)
{
    pcm->tJolt = TCm();
    pcm->nJolt = n;
}

// orig: 0x0014F768
MATRIX4 MatCmStereoEye(const CM* pcm, int k, const MATRIX4* pmatProj)
{
    float s = 0.0f;
    if (k == 1) s = pcm->sStereo;
    else if (k == 2) s = -pcm->sStereo;
    return CombineEyeLookAtProj(MatCam(pcm->mat, pcm->pos + pcm->mat[1] * s), pmatProj ? *pmatProj : pcm->matProj);
}

// orig: 0x0014F850 FInsideCmPlanes
bool FInsideCmPlanes(const CM* pcm, const glm::vec3& pos, float sRadius)
{
    const glm::vec4 acc = pcm->anormalFrustrumTranspose[0] * pos.x + pcm->anormalFrustrumTranspose[1] * pos.y +
                          pcm->anormalFrustrumTranspose[2] * pos.z - glm::vec4(sRadius);
    return acc.x < 0.0f && acc.y < 0.0f && acc.z < 0.0f && acc.w < 0.0f;
}

// orig: VU0 0x38..0x53 (sphere test called by RenderGlob 0x168740 / 0x168A44).
float UCmSphereDistanceFade(const CM* pcm, const glm::vec3& dpos, float sMRD)
{
    const float r = pcm->rMRDAdjust * sMRD;
    const float r2 = r * r;
    const float inner = r - pcm->rMRDAdjust * 400.0f;
    const float l2 = glm::dot(dpos, dpos);
    const float numerator = r2 - l2;
    if (numerator < 0.0f) return 0.0f;
    const float denominator = r2 - inner * inner;
    // The usual 1e10 MRD rounds inner to r. The original division saturates, then MINIw caps it at one.
    if (denominator == 0.0f) return 1.0f;
    return std::min(0.0f + numerator / denominator, 1.0f);
}

// orig: 0x0014F898 FInsideCmMrd
bool FInsideCmMrd(const CM* pcm, const glm::vec3& dpos, float sMRD, float* puAlpha)
{
    const float l2 = glm::dot(dpos, dpos);
    const float r = sMRD * pcm->rMRDAdjust;
    const float r2 = r * r;
    if (r2 < l2) return false;
    float d = r - pcm->rMRDAdjust * 400.0f;
    d = d * d;
    *puAlpha = d < l2 ? (r2 - l2) / (r2 - d) : 1.0f;
    return true;
}

// The camera block of a world file: EOPIDs 161..173 (0x2AC880: their setters are the CM functions above).
void ApplyCmOptions(CM* pcm, const std::vector<OPT>& aopt)
{
    for (const OPT& opt : aopt)
    {
        if (opt.aval.empty()) continue;
        const OPTVAL& v = opt.aval[0];
        switch (opt.eopid)
        {
        case 161: SetCmFov(pcm, v.ag[0]); break;           // 0x14F5A0
        case 162: SetCmFovTarget(pcm, v.ag[0]); break;     // 0x14F5F8
        case 164: SetCmNearClipAll(pcm, v.ag[0]); break;   // 0x14F630
        case 165: SetCmFarClip(pcm, v.ag[0]); break;       // 0x14F670
        case 166: SetCmNearFog(pcm, v.ag[0]); break;       // 0x14F690
        case 167: SetCmFarFog(pcm, v.ag[0]); break;        // 0x14F6B0
        case 168: pcm->rgbaFog = static_cast<uint32_t>(v.n); break;  // a field option (+0x24C), no setter call
        case 169: SetCmUFogMax(pcm, v.ag[0]); break;       // 0x14F6D0
        case 170: SetCmMrdRatio(pcm, v.ag[0]); break;      // 0x14F730
        case 171:                                          // 0x14F3C8: the position
            SetCmPosMat(pcm, glm::vec3(v.ag[0], v.ag[1], v.ag[2]), pcm->mat);
            break;
        case 173:                                          // 0x14F4E0: Euler angles in degrees
        {
            const float r = F32(0x3C8EFA36);
            SetCmPosMat(pcm, pcm->pos, BuildEulerMatrix(glm::vec3(v.ag[0] * r, v.ag[1] * r, v.ag[2] * r)));
            break;
        }
        default: break;
        }
    }
}

// The renderer's projection: display.h's aspect modes on top of the original's camera.
CMVIEW CmviewFromCm(const CM& cm)
{
    CMVIEW v;
    v.pos = cm.pos;
    v.mat = cm.mat;
    const DISPPROJ proj = ProjectionForDisplay(cm.radFOV, cm.rAspect);
    v.radFovY = proj.radFovY;
    v.rAspect = proj.rAspect;
    v.sNear = cm.sNearClip;
    v.sFar = cm.sFarClip;
    // GL eye axes from the camera's rows: right = -y, up = z, back = -x (Sly1-Release ConvertGameCameraBasisToGl).
    glm::mat4 eye(1.0f);
    eye[0] = glm::vec4(-cm.mat[1], 0.0f);
    eye[1] = glm::vec4(cm.mat[2], 0.0f);
    eye[2] = glm::vec4(-cm.mat[0], 0.0f);
    eye[3] = glm::vec4(cm.pos, 1.0f);
    v.matView = glm::inverse(eye);
    return v;
}

glm::mat4 CMVIEW::MatWorldToClip() const
{
    const float f = 1.0f / std::tan(radFovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = f / rAspect;
    proj[1][1] = f;
    proj[2][3] = -1.0f;
    proj[3][2] = sNear;
    return proj * matView;
}
