// wr.cpp: the space warps (cid 0x73 WR, module 0x22B720..0x22C6C0; Sly 1's wr.c, adapted from Sly1-Release/wr.cpp).
// The game side: the entries and their options, the matrices each frame (0x22C028), the per-vertex state vectors of
// the warped globs (0x22B7D8). The renderer adds sum(matrix x state) to each warped vertex (glob.vert).

#include "wr.h"

#include "alo.h"
#include "animfind.h"
#include "brx.h"
#include "clock.h"
#include "glob.h"
#include "objmethod.h"
#include "sw.h"
#include "update.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

glm::vec3 PosJtConvert(LO* a, LO* b, const glm::vec3& v);  // jt.h (0x1A6F00)
// xfmath.h (its SMP clashes with splicetypes.h's through objmethod.h)
void CalculateSinCos(float rad, float* pgSin, float* pgCos);  // 0x11D608
float RadNormalize(float rad);                                // 0x11CF60
glm::mat4 MulMatrix4(const glm::mat4& matA, const glm::mat4& matB);  // mat.h (0x11C168)
glm::mat3 MatRotateAxis(float rad, const glm::vec3& axis);    // 0x11B500

namespace
{
constexpr float k2Pi = 6.2831855f;

WR* PwrOf(LO* plo) { return dynamic_cast<WR*>(plo); }

// a float as the PS2 FPU keeps it: no infinities (+-FLT_MAX), no NaNs
float GSat(float g) { return std::isnan(g) ? 0.0f : std::clamp(g, -FLT_MAX, FLT_MAX); }

glm::vec3 Vec3(const float* ag) { return glm::vec3(ag[0], ag[1], ag[2]); }

// orig: 0x0022C4A0 PwreGetWrCur (ENSK 1 = set: the state vectors are remade at the next update)
WRE* PwreGetWrCur(WR* pwr, bool fSet)
{
    if (pwr->cwre == 0) return nullptr;
    if (fSet) pwr->fValuesChanged = true;
    // (iwreCur is -1 only before the first entry; the original indexes awre[-1] then: never with cwre > 0)
    return &pwr->awre[std::clamp(pwr->iwreCur, 0, 7)];
}

// orig: 0x0022BE08 UNoise(int) (a hash in [0, 1))
float UNoiseN(int n)
{
    const uint32_t x = static_cast<uint32_t>((n << 13) ^ n);
    const uint32_t y = x * x * 0x3D73u + 0xC0AE5u;
    const uint32_t z = x * y + 0x5208DD0Du;
    return static_cast<float>(static_cast<int32_t>(z & 0x7FFFFFFFu)) / 2147483648.0f;
}

// orig: 0x0022BE60 UNoise(gFreq, gPhase, uRandom): smoothed value noise in time
float UNoise(float gFreq, float gPhase, float uRandom)
{
    const float t = gFreq * 2.0f * g_clock.t + gPhase;
    const int n = static_cast<int>(t);
    float u0 = UNoiseN(n);
    float u1 = UNoiseN(n + 1);
    if (n & 1)
    {
        u0 *= uRandom;
        u1 = uRandom * (u1 - 1.0f) + 1.0f;
    }
    else
    {
        u1 *= uRandom;
        u0 = uRandom * (u0 - 1.0f) + 1.0f;
    }
    const float s = t - static_cast<float>(n);
    return u0 + (s * s * (3.0f - (s + s))) * (u1 - u0);
}

// orig: 0x0022BF40 UBias
// (saturating as the PS2 FPU: a bias 0 gives 1 / 0 = FLT_MAX, no NaN at u 1)
float UBias(float u, float uBias)
{
    const float gInv = uBias == 0.0f ? (std::signbit(uBias) ? -FLT_MAX : FLT_MAX) : 1.0f / uBias;
    const float gDen = GSat(GSat(GSat(gInv - 2.0f) * (1.0f - u)) + 1.0f);
    return gDen == 0.0f ? (std::signbit(u) != std::signbit(gDen) ? -FLT_MAX : FLT_MAX) : GSat(u / gDen);
}

// orig: 0x0022BF70 GFromOnz
float GFromOnz(const ONZ* ponz)
{
    float gSum = 0.0f, gAmpl = 0.0f;
    for (int i = 0; i < ponz->conze; ++i)
    {
        const ONZE& e = ponz->aonze[i];
        gSum += e.uAmpl * UNoise(e.gFreq, e.gPhase, e.uRandom);
        gAmpl += e.uAmpl;
    }
    // (no terms: 0 / 0, +FLT_MAX on the PS2; the product with an empty range is 0)
    const float uSum = gAmpl == 0.0f ? (std::signbit(gSum) != std::signbit(gAmpl) ? -FLT_MAX : FLT_MAX) : GSat(gSum / gAmpl);
    const float u = UBias(uSum, ponz->uBias);
    return GSat(ponz->gMin + GSat(u * (ponz->gMax - ponz->gMin)));
}

// orig: 0x0022C5B8 AddOnzOnze
void AddOnzOnze(ONZ* ponz, float uAmpl, float gFreq, float gPhase, float uRandom)
{
    if (static_cast<uint32_t>(ponz->conze) >= 4) return;
    ponz->aonze[ponz->conze++] = ONZE{uAmpl, gFreq, gPhase, uRandom};
}

// orig: 0x0022C5F0 AddWrBendNoise / 0x0022C658 AddWrSwivelNoise
void AddWrBendNoise(WR* pwr, float uAmpl, float gFreq, float gPhase, float uRandom)
{
    if (WRE* pwre = PwreGetWrCur(pwr, true)) AddOnzOnze(&pwre->bend.onzBend, uAmpl, gFreq, gPhase, uRandom);
}
void AddWrSwivelNoise(WR* pwr, float uAmpl, float gFreq, float gPhase, float uRandom)
{
    if (WRE* pwre = PwreGetWrCur(pwr, true)) AddOnzOnze(&pwre->bend.onzSwivel, uAmpl, gFreq, gPhase, uRandom);
}

// orig: 0x0022C518 SetWrWavelength / 0x0022C570 (get)
void SetWrWavelength(WR* pwr, float sWavelength)
{
    if (WRE* pwre = PwreGetWrCur(pwr, true)) pwre->circle.rgDot = sWavelength == 0.0f ? 0.0f : 1.0f / sWavelength;
}
float GetWrWavelength(WR* pwr)
{
    const WRE* pwre = PwreGetWrCur(pwr, true);
    return !pwre || pwre->circle.rgDot == 0.0f ? 0.0f : 1.0f / pwre->circle.rgDot;
}

// The sub-object fields (grf 0x2000 through 0x22C4D8 circle / 0x22C4F8 bend: both the current entry) of EOPIDs
// 947..961: their offsets in the entry and float counts (kind 1: 1, kind 10: 2, kind 2: 3)
struct WRFIELD { int eopid; uint32_t ib; int cg; };
constexpr WRFIELD s_awrfield[] = {
    {947, 0x10, 3}, {949, 0x68, 1}, {950, 0x6C, 1}, {951, 0x20, 3}, {952, 0x40, 3}, {953, 0x30, 2}, {954, 0x50, 2},
    {955, 0x10, 3}, {956, 0x30, 2}, {957, 0x38, 1}, {959, 0x20, 3}, {960, 0x80, 2}, {961, 0x88, 1},
};
const WRFIELD* PwrfieldOf(int eopid)
{
    for (const WRFIELD& f : s_awrfield)
        if (f.eopid == eopid) return &f;
    return nullptr;
}
void SetWreField(WR* pwr, const WRFIELD& f, const float* ag)
{
    WRE* pwre = PwreGetWrCur(pwr, true);
    if (!pwre) return;
    std::memcpy(reinterpret_cast<uint8_t*>(pwre) + f.ib, ag, sizeof(float) * size_t(f.cg));
}
CRef RefWreField(WR* pwr, const WRFIELD& f)
{
    CRef ref;
    const WRE* pwre = PwreGetWrCur(pwr, false);
    float ag[3] = {};
    if (pwre) std::memcpy(ag, reinterpret_cast<const uint8_t*>(pwre) + f.ib, sizeof(float) * size_t(f.cg));
    if (f.cg == 1) ref.SetF32(ag[0]);
    else if (f.cg == 2) ref.SetLm({ag[0], ag[1]});
    else ref.SetVector(glm::vec3(ag[0], ag[1], ag[2]));
    return ref;
}

// orig: 0x0022B7D8 (Sly 1's UpdateWrStateVectors): per bound block, each vertex's weight (the WEKI envelope) and
// per entry its state vector (circle: cos / sin of the wave phase at the vertex in its two lanes; bend: the vertex's
// offset from the bend centre, w 1), all in world space
void UpdateWrStateVectors(WR* pwr)
{
    for (WRBG* pwrbg : pwr->apwrbg)
    {
        ALO* palo = pwrbg->palo;
        if (!palo || !palo->pglobset || pwrbg->iglob < 0 || pwrbg->iglob >= static_cast<int>(palo->pglobset->aglob.size())) continue;
        const GLOB& glob = palo->pglobset->aglob[size_t(pwrbg->iglob)];
        // (glob +8, the group's own matrix: an instance's; the blocks are on templates)
        const glm::mat4* pmat08 = glob.fMatrix ? &glob.dmat : nullptr;
        // the envelope's space and the bend centre in it
        glm::mat4 dmat(1.0f);
        glm::vec3 posBend(0.0f);
        float g0 = 0.0f, g1 = 0.0f, gMin = 0.0f, gMax = 0.0f;
        if (pwrbg->wek != -1)
        {
            dmat = pwrbg->dmat;
            gMin = std::min(pwrbg->uInner, pwrbg->uOuter);
            gMax = std::max(pwrbg->uInner, pwrbg->uOuter);
            // FitLinearClq, with the PS2 FPU's saturation: x / 0 is +-FLT_MAX (sign of the xor), sums and products clamp
            // (e.g. w32's aurora: sInner == sOuter, uInner == uOuter = 1: every weight 1, natively 0 / 0 was NaN)
            const float du = pwrbg->uInner - pwrbg->uOuter, ds = pwrbg->sInner - pwrbg->sOuter;
            g1 = ds == 0.0f ? (std::signbit(du) != std::signbit(ds) ? -FLT_MAX : FLT_MAX) : GSat(du / ds);
            g0 = GSat(pwrbg->uInner - GSat(g1 * pwrbg->sInner));
            const float sMid = (pwrbg->sInner + pwrbg->sOuter) * 0.5f;
            switch (pwrbg->wek)
            {
            case 4: posBend = glm::vec3(sMid, 0.0f, 0.0f); break;
            case 5: posBend = glm::vec3(0.0f, sMid, 0.0f); break;
            case 6: posBend = glm::vec3(0.0f, 0.0f, sMid); break;
            default: break;
            }
            posBend = glm::vec3(glm::inverse(dmat) * glm::vec4(posBend, 1.0f));  // 0x11C318
            if (pmat08) dmat = MulMatrix4(pwrbg->dmat, *pmat08);  // 0x11C168
        }
        const size_t cvtx = glob.avtx.size();
        std::vector<float> au(cvtx);
        for (size_t i = 0; i < cvtx; ++i)
        {
            glm::vec3 pos = glm::vec3(dmat * glm::vec4(glob.avtx[i].pos, 1.0f));
            float s = 0.0f;
            switch (pwrbg->wek)
            {
            case 0: s = glm::length(pos); break;
            case 1: pos.z = 0.0f; s = glm::length(pos); break;
            case 2: pos.y = 0.0f; s = glm::length(pos); break;
            case 3: pos.x = 0.0f; s = glm::length(pos); break;
            case 4: s = pos.x; break;
            case 5: s = pos.y; break;
            case 6: s = pos.z; break;
            default: au[i] = 1.0f; continue;
            }
            au[i] = std::clamp(GSat(g0 + GSat(s * g1)), gMin, gMax);  // (GLimit: below gMin -> gMin, above gMax -> gMax)
        }
        // the group's world matrix (glob +4: its record; the object's first record without one)
        const XF* pxf = glob.oidOwner >= 0 ? PxfFindAlo(palo, glob.oidOwner) : nullptr;
        glm::mat4 matWorld = pxf ? pxf->matWorld : palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
        if (pmat08) matWorld = MulMatrix4(matWorld, *pmat08);
        const glm::vec3 posWorldBend = PosJtConvert(palo, nullptr, posBend);  // 0x1A6F00(pso, 0, posBend)
        const int cmat = std::max({pwrbg->cmat, pwr->cmat, 1});
        if (pwrbg->astate.size() != size_t(cmat) * cvtx) pwrbg->astate.assign(size_t(cmat) * cvtx, glm::vec4(0.0f));
        for (int iwre = 0; iwre < pwr->cwre; ++iwre)
        {
            const WRE& wre = pwr->awre[iwre];
            if (wre.imat < 0 || wre.imat >= cmat) continue;
            glm::vec4* astate = pwrbg->astate.data() + size_t(wre.imat) * cvtx;
            for (size_t i = 0; i < cvtx; ++i)
            {
                const glm::vec3 posWorld = glm::vec3(matWorld * glm::vec4(glob.avtx[i].pos, 1.0f));
                switch (wre.wrek)
                {
                case 0:
                {
                    const int is = std::clamp(wre.circle.is, 0, 2);
                    const glm::vec3 axis = Vec3(wre.normal);
                    const float gLen = std::sqrt(glm::dot(axis, axis));
                    const float gDot = glm::dot(posWorld, gLen > 0.0f ? axis / gLen : axis);
                    float gSin, gCos;
                    CalculateSinCos(RadNormalize(gDot * (wre.circle.rgDot * k2Pi)), &gSin, &gCos);
                    astate[i][is] = gCos * au[i];
                    astate[i][is + 1] = gSin * au[i];
                    break;
                }
                case 1:
                {
                    const glm::vec3 dpos = posWorld - posWorldBend;
                    astate[i] = glm::vec4(au[i] * dpos, 1.0f);
                    break;
                }
                default: break;
                }
            }
        }
        if (std::getenv("SLY2N_WRSTATE"))
        {
            std::printf("wrstate %d block of %d glob %d wek %d s %g u %g s %g u %g dmat x %g %g %g y %g %g %g z %g %g %g t %g %g %g\n", pwr->oid, palo->oid,
                        pwrbg->iglob, pwrbg->wek, pwrbg->sInner, pwrbg->uInner, pwrbg->sOuter, pwrbg->uOuter, pwrbg->dmat[0].x, pwrbg->dmat[0].y,
                        pwrbg->dmat[0].z, pwrbg->dmat[1].x, pwrbg->dmat[1].y, pwrbg->dmat[1].z, pwrbg->dmat[2].x, pwrbg->dmat[2].y, pwrbg->dmat[2].z,
                        pwrbg->dmat[3].x, pwrbg->dmat[3].y, pwrbg->dmat[3].z);
            for (size_t i = 0; i < cvtx && i < 6; ++i)
                std::printf("   vtx %zu pos %g %g %g u %g state %g %g %g %g world %g %g %g\n", i, glob.avtx[i].pos.x, glob.avtx[i].pos.y, glob.avtx[i].pos.z, au[i],
                            pwrbg->astate[i].x, pwrbg->astate[i].y, pwrbg->astate[i].z, pwrbg->astate[i].w, (matWorld * glm::vec4(glob.avtx[i].pos, 1.0f)).x,
                            (matWorld * glm::vec4(glob.avtx[i].pos, 1.0f)).y, (matWorld * glm::vec4(glob.avtx[i].pos, 1.0f)).z);
        }
        ++pwrbg->nGen;
    }
    pwr->fValuesChanged = false;
}

void UpdateWrLo(LO* plo)
{
    if (WR* pwr = PwrOf(plo)) UpdateWrMatrixes(pwr);
}

struct INITWR
{
    INITWR()
    {
        RegisterLoUpdate(0x22C028, UpdateWrLo);
        // the method options (EOPIDs 945 / 946 / 958 / 962: thunks 0x10C370 / 0x10C3E8 / 0x10C460 / 0x10C528)
        RegisterObjectMethod(0x10C370, [](LO* plo, const std::vector<CRef>&) {
            if (WR* pwr = PwrOf(plo)) AddWrCircleWarp(pwr);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10C3E8, [](LO* plo, const std::vector<CRef>&) {
            if (WR* pwr = PwrOf(plo)) AddWrBendWarp(pwr);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10C460, [](LO* plo, const std::vector<CRef>& a) {
            if (WR* pwr = PwrOf(plo)) AddWrBendNoise(pwr, GArg(a, 0), GArg(a, 1), GArg(a, 2), GArg(a, 3));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10C528, [](LO* plo, const std::vector<CRef>& a) {
            if (WR* pwr = PwrOf(plo)) AddWrSwivelNoise(pwr, GArg(a, 0), GArg(a, 1), GArg(a, 2), GArg(a, 3));
            return CRef(TAGK_Void);
        });
        // 948: the wavelength (0x22C518 / 0x22C570)
        RegisterObjectSetter(948, [](LO* plo, const CRef& r) {
            if (WR* pwr = PwrOf(plo)) SetWrWavelength(pwr, r.GAsFloat());
        });
        RegisterObjectGetter(948, [](LO* plo) {
            CRef ref;
            ref.SetF32(PwrOf(plo) ? GetWrWavelength(PwrOf(plo)) : 0.0f);
            return ref;
        });
        // 963: the entries' count (+0x54); 964: the current entry (+0x6E0)
        RegisterObjectGetter(963, [](LO* plo) {
            CRef ref;
            ref.SetS32(PwrOf(plo) ? PwrOf(plo)->cwre : 0);
            return ref;
        });
        RegisterObjectGetter(964, [](LO* plo) {
            CRef ref;
            ref.SetS32(PwrOf(plo) ? PwrOf(plo)->iwreCur : 0);
            return ref;
        });
        RegisterObjectSetter(964, [](LO* plo, const CRef& r) {
            if (WR* pwr = PwrOf(plo)) pwr->iwreCur = r.m_tagk == TAGK_F32 ? static_cast<int>(r.m_g) : r.m_n;
        });
    }
} s_initwr;

// the run time setters / getters of the fields, one function per EOPID (the registry keeps plain function pointers)
template <int EOPID> void SetWreFieldT(LO* plo, const CRef& r)
{
    WR* pwr = PwrOf(plo);
    const WRFIELD* pf = PwrfieldOf(EOPID);
    if (!pwr || !pf) return;
    float ag[3] = {};
    if (pf->cg == 1) ag[0] = r.GAsFloat();
    else if (pf->cg == 2) { const LM lm = r.GetLm(); ag[0] = lm.gMin; ag[1] = lm.gMax; }
    else { const glm::vec3 v = r.GetVector(); ag[0] = v.x; ag[1] = v.y; ag[2] = v.z; }
    SetWreField(pwr, *pf, ag);
}
template <int EOPID> CRef GetWreFieldT(LO* plo)
{
    WR* pwr = PwrOf(plo);
    const WRFIELD* pf = PwrfieldOf(EOPID);
    return pwr && pf ? RefWreField(pwr, *pf) : CRef(TAGK_Nil);
}
template <int... AN> void RegisterWreFields(std::integer_sequence<int, AN...>)
{
    ((PwrfieldOf(AN) ? (RegisterObjectSetter(AN, SetWreFieldT<AN>), RegisterObjectGetter(AN, GetWreFieldT<AN>), 0) : 0), ...);
}
struct INITWRFIELDS
{
    INITWRFIELDS() { RegisterWreFields(std::integer_sequence<int, 947, 949, 950, 951, 952, 953, 954, 955, 956, 957, 959, 960, 961>{}); }
} s_initwrfields;
}  // namespace

// orig: 0x0022C3B0 AddWrCircleWarp (two circles share a matrix: lanes 0 / 1 and 2 / 3)
void AddWrCircleWarp(WR* pwr)
{
    if (pwr->cwre >= 8) return;
    WRE& wre = pwr->awre[pwr->cwre++];
    pwr->iwreCur++;
    wre.wrek = 0;
    wre.normal[0] = 1.0f; wre.normal[1] = 0.0f; wre.normal[2] = 0.0f; wre.normal[3] = 0.0f;
    if (pwr->imatHalf >= 0)
    {
        wre.imat = pwr->imatHalf;
        pwr->imatHalf = -1;
        wre.circle.is = 2;
    }
    else
    {
        wre.imat = pwr->cmat++;
        pwr->imatHalf = wre.imat;
        wre.circle.is = 0;
    }
}

// orig: 0x0022C430 AddWrBendWarp
void AddWrBendWarp(WR* pwr)
{
    if (pwr->cwre >= 8) return;
    WRE& wre = pwr->awre[pwr->cwre++];
    pwr->iwreCur++;
    wre.wrek = 1;
    wre.normal[0] = 0.0f; wre.normal[1] = 1.0f; wre.normal[2] = 0.0f; wre.normal[3] = 0.0f;
    wre.vecA[0] = 0.0f; wre.vecA[1] = 0.0f; wre.vecA[2] = 1.0f; wre.vecA[3] = 0.0f;
    wre.bend.onzBend.uBias = 0.5f;
    wre.bend.onzSwivel.uBias = 0.5f;
    // (the original counts a matrix but leaves the entry's imat as it was: 0 in a new WR)
    pwr->cmat++;
}

// orig: 0x0022C028 UpdateWrMatrixes (vt slot 16)
void UpdateWrMatrixes(WR* pwr)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1A5E28)) pfn(pwr);
    if (g_clock.t == pwr->tLastUpdate) return;
    if (pwr->fValuesChanged) UpdateWrStateVectors(pwr);
    for (int i = 0; i < pwr->cwre; ++i)
    {
        WRE& wre = pwr->awre[i];
        if (wre.imat < 0 || wre.imat > 3) continue;
        glm::mat4& matPos = pwr->amatDpos[wre.imat];
        glm::mat4& matUv = pwr->amatDuv[wre.imat];
        switch (wre.wrek)
        {
        case 0:
        {
            float gSin, gCos;
            CalculateSinCos(RadNormalize((g_clock.t * wre.circle.gFrequency + wre.circle.uPhase) * k2Pi), &gSin, &gCos);
            const int ic = std::clamp(wre.circle.is, 0, 2);
            const glm::vec3 a = Vec3(wre.vecA), b = Vec3(wre.circle.vecB);
            const glm::vec2 uvA(wre.circle.uvA[0], wre.circle.uvA[1]), uvB(wre.circle.uvB[0], wre.circle.uvB[1]);
            // (rows ic / ic + 1: x, y, z of the positions, x, y of the coordinates; the rest kept)
            const glm::vec3 p0 = gCos * a + gSin * b, p1 = -gSin * a + gCos * b;
            const glm::vec2 t0 = gCos * uvA + gSin * uvB, t1 = -gSin * uvA + gCos * uvB;
            matPos[ic] = glm::vec4(p0, matPos[ic].w);
            matPos[ic + 1] = glm::vec4(p1, matPos[ic + 1].w);
            matUv[ic] = glm::vec4(t0, matUv[ic].z, matUv[ic].w);
            matUv[ic + 1] = glm::vec4(t1, matUv[ic + 1].z, matUv[ic + 1].w);
            break;
        }
        case 1:
        {
            const float radBend = GFromOnz(&wre.bend.onzBend);
            const float radSwivel = GFromOnz(&wre.bend.onzSwivel);
            const glm::mat3 matSwivel = MatRotateAxis(radSwivel, glm::normalize(Vec3(wre.vecA)));
            // (0x11B500's rows times the unit axis as a row vector: glm columns = the rows)
            const glm::vec3 axis = matSwivel * glm::normalize(Vec3(wre.normal));
            const glm::mat3 m = MatRotateAxis(radBend, glm::normalize(axis));
            for (int r = 0; r < 3; ++r)
            {
                glm::vec3 row = m[r];
                row[r] -= 1.0f;
                matPos[r] = glm::vec4(row, matPos[r].w);
            }
            break;
        }
        default: break;
        }
    }
    pwr->tLastUpdate = g_clock.t;
    // (debug: SLY2N_WRLIST=t: each WR's entries and matrices at the first update at or after clock t)
    static const char* s_szList = std::getenv("SLY2N_WRLIST");
    static float s_tPrinted = -1.0f;
    if (s_szList && g_clock.t >= std::atof(s_szList) && (s_tPrinted < 0.0f || s_tPrinted == g_clock.t))
    {
        s_tPrinted = g_clock.t;
        std::printf("WR %d: t %.4f cwre %d cmat %d half %d cur %d blocks %zu\n", pwr->oid, g_clock.t, pwr->cwre, pwr->cmat, pwr->imatHalf,
                    pwr->iwreCur, pwr->apwrbg.size());
        for (int i = 0; i < pwr->cwre; ++i)
        {
            const WRE& e = pwr->awre[i];
            std::printf("  wre %d kind %d imat %d n %g %g %g A %g %g %g is %d rg %g f %g ph %g\n", i, e.wrek, e.imat, e.normal[0], e.normal[1],
                        e.normal[2], e.vecA[0], e.vecA[1], e.vecA[2], e.circle.is, e.circle.rgDot, e.circle.gFrequency, e.circle.uPhase);
        }
        for (int m = 0; m < std::min(pwr->cmat, 4); ++m)
        {
            std::printf("  matdpos %d", m);
            for (int r = 0; r < 4; ++r) std::printf(" %.4f %.4f %.4f %.4f", pwr->amatDpos[m][r].x, pwr->amatDpos[m][r].y, pwr->amatDpos[m][r].z, pwr->amatDpos[m][r].w);
            std::printf("\n");
        }
        for (const WRBG* pw : pwr->apwrbg)
            std::printf("  block of %d glob %d wek %d cmat %d dpos %d duv %d states %zu\n", pw->palo ? pw->palo->oid : -1, pw->iglob, pw->wek, pw->cmat,
                        pw->fDpos, pw->fDuv, pw->astate.size());
    }
}

// orig: 0x00167B68's end: each warp block of the object's globs bound to the WR its oid names (0x15E938(G, 4, oid,
// owner), then 0x22B7A0: pushed on the WR's list, +0x0C the WR)
void BindAloWarps(ALO* palo)
{
    if (!palo->pglobset) return;
    for (GLOB& glob : palo->pglobset->aglob)
    {
        WRBG* pwrbg = glob.pwrbg.get();
        if (!pwrbg) continue;
        WR* pwr = PwrOf(PloFindSwNearest(palo, pwrbg->oid));
        if (!pwr) continue;
        if (!pwrbg->palo) pwrbg->palo = palo->pglobset->palo ? palo->pglobset->palo : palo;  // (pso: the globset's owner)
        if (std::find(pwr->apwrbg.begin(), pwr->apwrbg.end(), pwrbg) == pwr->apwrbg.end())
            pwr->apwrbg.insert(pwr->apwrbg.begin(), pwrbg);
        pwrbg->pwr = pwr;
    }
}

const WR* PwrOfWrbg(const WRBG* pwrbg) { return pwrbg ? pwrbg->pwr : nullptr; }

// orig: 0x0022B748 CloneWr (vt slot 40): the entries, matrices and list copied
void CloneWrData(const LO* ploSrc, LO* ploDst)
{
    const WR* ps = dynamic_cast<const WR*>(ploSrc);
    WR* pd = dynamic_cast<WR*>(ploDst);
    if (!ps || !pd) return;
    pd->tLastUpdate = ps->tLastUpdate;
    pd->cwre = ps->cwre;
    pd->cmat = ps->cmat;
    pd->imatHalf = ps->imatHalf;
    std::copy(std::begin(ps->awre), std::end(ps->awre), std::begin(pd->awre));
    pd->iwreCur = ps->iwreCur;
    std::copy(std::begin(ps->amatDpos), std::end(ps->amatDpos), std::begin(pd->amatDpos));
    std::copy(std::begin(ps->amatDuv), std::end(ps->amatDuv), std::begin(pd->amatDuv));
    pd->apwrbg = ps->apwrbg;
    pd->fValuesChanged = ps->fValuesChanged;
}

bool FLoadWrOption(LO* plo, int eopid, const float* ag, int n)
{
    WR* pwr = PwrOf(plo);
    if (!pwr) return false;
    if (eopid == 964)
    {
        pwr->iwreCur = n;
        return true;
    }
    const WRFIELD* pf = PwrfieldOf(eopid);
    if (!pf) return false;
    SetWreField(pwr, *pf, ag);
    return true;
}
