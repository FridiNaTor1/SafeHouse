// wr.cpp: the space warps (Sly 3 cid 0x88 WR, module 0x289678..0x28A6E0; Sly 2's 0x73 / Sly 1's wr.c before). The game
// side: the entries and their options (0x28A480 / 0x28A5A8), the matrices (0x289F78, vt slot 53: when a warped glob is
// drawn, once per clock time), the per-vertex state vectors of the warped globs (0x2897B8). The renderer adds
// sum(matrix x state) to each warped vertex (glob.vert). Sly 3 differences from Sly 2: kind 1 is a constant pair of
// rows (no bend / swivel noise), every entry has an amplitude (+0x10, 1), entries carry stamps and each block rebuilds
// only the newer ones, lanes are packed into the matrices by size (2 a circle, 1 a constant), the envelope space is
// the inverse of the block's matrix, and an anchor object (+0x688) keeps the waves of the blocks under it in place.

#include "wr.h"
#include "game/sly3.h"

#include "alo.h"
#include "activate.h"
#include "animfind.h"
#include "brx.h"
#include "clock.h"
#include "disc.h"
#include "glob.h"
#include "objmethod.h"
#include "sw.h"
#include "update.h"
#include "game/ship3.h"

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

glm::vec3 PosJtConvert(LO* a, LO* b, const glm::vec3& v);  // jt.h (0x1A6F00)
// xfmath.h (its SMP clashes with splicetypes.h's through objmethod.h)
void CalculateSinCos(float rad, float* pgSin, float* pgCos);  // 0x11D608
float RadNormalize(float rad);                                // 0x11CF60
float GRandInRange(float gLow, float gHigh);                  // 0x11D448 (orig3 0x11E940)
float GModPositive(float gDividend, float gDivisor);          // 0x11D898 (orig3 0x11EDB8)
glm::mat4 MulMatrix4(const glm::mat4& matA, const glm::mat4& matB);  // mat.h (0x11C168)
glm::mat3 MatRotateAxis(float rad, const glm::vec3& axis);    // 0x11B500

namespace
{
constexpr float k2Pi = 6.2831855f;

WR* PwrOf(LO* plo) { return dynamic_cast<WR*>(plo); }

// a float as the PS2 FPU keeps it: no infinities (+-FLT_MAX), no NaNs
float GSat(float g) { return std::isnan(g) ? 0.0f : std::clamp(g, -FLT_MAX, FLT_MAX); }

glm::vec3 Vec3(const float* ag) { return glm::vec3(ag[0], ag[1], ag[2]); }

// orig3: 0x0028A470 (the WR's stamp: ++ +0x4C)
int NextStamp(WR* pwr) { return ++pwr->stamp; }

// orig3: 0x0028A5A8 PwreGetWrCur (none without entries; 1 = set: the entry restamped)
WRE* PwreGetWrCur(WR* pwr, bool fSet)
{
    if (pwr->cwre == 0) return nullptr;
    WRE* pwre = &pwr->awre[std::clamp(pwr->iwreCur, 0, 7)];
    if (fSet) pwre->stamp = NextStamp(pwr);
    return pwre;
}

// orig3: 0x0028A480 (an entry of kind k taking cLane lanes: the first matrix with room, at most 4 lanes each)
WRE* PwreAddWr(WR* pwr, int k, int cLane)
{
    if (pwr->cwre >= 8) return nullptr;
    const int i = pwr->cwre++;
    WRE* pwre = &pwr->awre[i];
    pwr->iwreCur = i;
    pwre->wrek = k;
    pwre->stamp = NextStamp(pwr);
    pwre->gAmpl = 1.0f;
    for (int j = 0; j < 4 && j <= pwr->cmat; ++j)
        if (cLane + pwr->alane[j] < 5)
        {
            pwre->imat = j;
            pwre->irow = pwr->alane[j];
            pwr->alane[j] += cLane;
            if (j == pwr->cmat) pwr->cmat = j + 1;
            break;
        }
    return pwre;
}

// orig3: 0x0028A518 / 0x0028A698 (the wavelength: +0x70 its reciprocal)
void SetWrWavelength(WR* pwr, float sWavelength)
{
    if (WRE* pwre = PwreGetWrCur(pwr, true)) pwre->rgDot = sWavelength == 0.0f ? 0.0f : 1.0f / sWavelength;
}
float GetWrWavelength(WR* pwr)
{
    const WRE* pwre = PwreGetWrCur(pwr, false);
    return !pwre || pwre->rgDot == 0.0f ? 0.0f : 1.0f / pwre->rgDot;
}

// The sub-object fields (grf 0x2000 through 0x28A600 circle / 0x28A620 constant: both the current entry) of Sly 3's
// EOPIDs 1035..1044 (Sly 2's 947..956): their offsets in Sly 3's entry and float counts (kind 1: 1, 10: 2, 2: 3)
struct WRFIELD { int eopid3; uint32_t ib; int cg; };
constexpr WRFIELD s_awrfield[] = {
    {1035, 0x20, 3}, {1037, 0x74, 1}, {1038, 0x78, 1}, {1039, 0x30, 3}, {1040, 0x50, 3}, {1041, 0x40, 2}, {1042, 0x60, 2},
    {1043, 0x20, 3}, {1044, 0x30, 2},
};
const WRFIELD* PwrfieldOf(int eopid3)
{
    for (const WRFIELD& f : s_awrfield)
        if (f.eopid3 == eopid3) return &f;
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

// the anchor (option 1047): its object found once it exists (natively the option may name one loaded later)
LO* PloAnchor(WR* pwr)
{
    if (!pwr->ploAnchor && pwr->oidAnchor != -1) pwr->ploAnchor = PloFindSwNearest(pwr, pwr->oidAnchor);
    return pwr->ploAnchor;
}

// orig3: 0x002896D0 (a block under the anchor (0x1C8F20: its object or a parent is the anchor) draws with the anchor's
// matrices and takes its positions relative to the anchor)
void BindWrbgAnchor(WR* pwr, WRBG* pwrbg)
{
    const LO* ploAnchor = PloAnchor(pwr);
    bool f = false;
    for (const LO* p = pwrbg->palo; p && ploAnchor; p = p->paloParent)
        if (p == ploAnchor) { f = true; break; }
    pwrbg->fInside = f;
}

// orig3: 0x0028A6E0 (the anchor set: every block bound again)
void SetWrAnchor(WR* pwr, LO* plo, int32_t oid)
{
    pwr->ploAnchor = plo;
    pwr->oidAnchor = plo ? plo->oid : oid;
    for (WRBG* pwrbg : pwr->apwrbg) BindWrbgAnchor(pwr, pwrbg);
}

// orig3: 0x002897B8 (Sly 2's UpdateWrStateVectors 0x22B7D8): per vertex its weight (the WEKI envelope: the length of
// the vertex in the inverse of the block's matrix, the unused axes zeroed per kind), then for every entry newer than
// the block (its stamp) its lanes: 0 where the weight is 0; a circle cos / sin of the wave phase at the world position
// (relative to the anchor for a block under it) times the weight; a constant 1. The block then takes the WR's stamp.
void UpdateWrStateVectors(WR* pwr, WRBG* pwrbg)
{
    ALO* palo = pwrbg->palo;
    if (!palo || !palo->pglobset || pwrbg->iglob < 0 || pwrbg->iglob >= static_cast<int>(palo->pglobset->aglob.size())) return;
    const GLOB& glob = palo->pglobset->aglob[size_t(pwrbg->iglob)];
    glm::mat4 dmat(1.0f);
    float g0 = 0.0f, g1 = 0.0f, gMin = 0.0f, gMax = 0.0f;
    if (pwrbg->wek != -1)
    {
        dmat = glm::inverse(pwrbg->dmat);   // 0x11D768
        gMin = std::min(pwrbg->uInner, pwrbg->uOuter);
        gMax = std::max(pwrbg->uInner, pwrbg->uOuter);
        // FitLinearClq, with the PS2 FPU's saturation: x / 0 is +-FLT_MAX (sign of the xor), sums and products clamp
        const float du = pwrbg->uInner - pwrbg->uOuter, ds = pwrbg->sInner - pwrbg->sOuter;
        g1 = ds == 0.0f ? (std::signbit(du) != std::signbit(ds) ? -FLT_MAX : FLT_MAX) : GSat(du / ds);
        g0 = GSat(pwrbg->uInner - GSat(g1 * pwrbg->sInner));
        auto zero = [&](int c) { for (int r = 0; r < 4; ++r) dmat[r][c] = 0.0f; };
        switch (pwrbg->wek)
        {
        case 1: zero(2); break;
        case 2: zero(1); break;
        case 3: zero(0); break;
        case 4: zero(1); zero(2); break;
        case 5: zero(0); zero(2); break;
        case 6: zero(0); zero(1); break;
        default: break;
        }
    }
    // the group's world matrix (glob +4: its record; the object's first record without one), times glob +8
    const XF* pxf = glob.oidOwner >= 0 ? PxfFindAlo(palo, glob.oidOwner) : nullptr;
    glm::mat4 matWorld = pxf ? pxf->matWorld : palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    if (glob.fMatrix) matWorld = MulMatrix4(matWorld, glob.dmat);
    if (pwrbg->fInside)
        if (LO* ploAnchor = PloAnchor(pwr)) matWorld[3] = glm::vec4(glm::vec3(matWorld[3]) - PosLoWorld(ploAnchor), 1.0f);
    const size_t cvtx = glob.avtx.size();
    const int cmat = std::max({pwrbg->cmat, pwr->cmat, 1});
    if (pwrbg->astate.size() != size_t(cmat) * cvtx) pwrbg->astate.assign(size_t(cmat) * cvtx, glm::vec4(0.0f));
    for (size_t i = 0; i < cvtx; ++i)
    {
        const glm::vec4 pos(glob.avtx[i].pos, 1.0f);
        float u = 1.0f;
        if (pwrbg->wek != -1)
        {
            const float s = glm::length(glm::vec3(dmat * pos));
            u = std::clamp(GSat(g0 + GSat(s * g1)), gMin, gMax);   // (GLimit)
        }
        const glm::vec3 posWorld = glm::vec3(matWorld * pos);
        for (int iwre = 0; iwre < pwr->cwre; ++iwre)
        {
            const WRE& wre = pwr->awre[iwre];
            if (wre.wrek == -1 || wre.stamp <= pwrbg->stamp) continue;
            if (wre.imat < 0 || wre.imat >= cmat) continue;
            glm::vec4& st = pwrbg->astate[size_t(wre.imat) * cvtx + i];
            const int ic = std::clamp(wre.irow, 0, 3);
            if (u == 0.0f)
            {
                st[ic] = 0.0f;
                if (ic < 3) st[ic + 1] = 0.0f;
                continue;
            }
            switch (wre.wrek)
            {
            case 0:
            {
                const glm::vec3 axis = Vec3(wre.axis);
                const float gLen = std::sqrt(glm::dot(axis, axis));
                const float gDot = glm::dot(posWorld, gLen > 0.0f ? axis / gLen : axis);
                float gSin, gCos;
                CalculateSinCos(RadNormalize(gDot * (wre.rgDot * k2Pi)), &gSin, &gCos);
                st[ic] = gCos * u;
                if (ic < 3) st[ic + 1] = gSin * u;
                break;
            }
            case 1: st[ic] = 1.0f; break;
            default: break;
            }
        }
    }
    if (std::getenv("SLY2N_WRSTATE"))
    {
        std::printf("wrstate %d block of %d glob %d wek %d s %g u %g s %g u %g inside %d stamp %d -> %d\n", pwr->oid, palo->oid, pwrbg->iglob, pwrbg->wek,
                    pwrbg->sInner, pwrbg->uInner, pwrbg->sOuter, pwrbg->uOuter, int(pwrbg->fInside), pwrbg->stamp, pwr->stamp);
        for (size_t i = 0; i < cvtx && i < 4; ++i)
            std::printf("   vtx %zu pos %g %g %g state %g %g %g %g\n", i, glob.avtx[i].pos.x, glob.avtx[i].pos.y, glob.avtx[i].pos.z,
                        pwrbg->astate[i].x, pwrbg->astate[i].y, pwrbg->astate[i].z, pwrbg->astate[i].w);
    }
    pwrbg->stamp = pwr->stamp;
    ++pwrbg->nGen;
}

// ---- class 0x89 (orig3 0x28A788..0x28AFC0) ----
WR89* Pwr89Of(LO* plo) { return dynamic_cast<WR89*>(plo); }

float GElf(uint32_t va)
{
    const uint32_t u = g_disc.U32Elf(va);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

// orig3: 0x0028AFC0 (the current item; fDrain (a set): its waves stopped first)
void RemoveWrq(WR89* p, WRITEM& item, int i);
WRITEM* PitemCur(WR89* p, bool fDrain)
{
    WRITEM* pitem = &p->aitem[std::clamp(p->iitemCur, 0, 7)];
    if (fDrain)
        while (pitem->cq > 0) RemoveWrq(p, *pitem, 0);
    return pitem;
}

// orig3: 0x0028A818 (an entry given back: amplitude 0, no owner)
void ReleaseWre(WR89* p, int iwre)
{
    if (iwre < 0 || iwre >= 8) return;
    p->awre[iwre].gAmpl = 0.0f;
    p->afOwned[iwre] = false;
}

// orig3: 0x0028AD48 (wave i of an item ended: its entry released, the later ones moved down; the vacated slot keeps
// its old values, which 0x28ADC8 reads for an empty queue's fade in)
void RemoveWrq(WR89* p, WRITEM& item, int i)
{
    ReleaseWre(p, item.aq[i].iwre);
    for (int j = i; j + 1 < item.cq && j + 1 < 2; ++j) item.aq[j] = item.aq[j + 1];
    --item.cq;
}

// orig3: 0x0028A7D0 (the first entry without an owner)
int IwreClaim(WR89* p)
{
    for (int i = 0; i < p->cwre && i < 8; ++i)
        if (!p->afOwned[i])
        {
            p->afOwned[i] = true;
            return i;
        }
    return -1;
}

// orig3: 0x0028A920 (item i's next wave, fading in over dtIn: random wavelength / amplitude / duration / angle; its end
// pushed past the other items' fades (merged by 0x28A868); the wave a circle along the wind's heading + pi + the
// angle with the deep-water speed sqrt(wavelength x 278.52); a second wave starts in phase with the first at a
// quarter of the fade in)
void StartWrq(WR89* p, int i, float dtIn)
{
    WRITEM& item = p->aitem[i];
    const int iwre = IwreClaim(p);
    if (iwre < 0 || item.cq >= 2) { if (iwre >= 0) p->afOwned[iwre] = false; return; }
    WRQ& q = item.aq[item.cq++];
    q.iwre = iwre;
    const float sWave = GRandInRange(item.lmWavelength[0], item.lmWavelength[1]);
    const float gAmpl = GRandInRange(item.lmAmpl[0], item.lmAmpl[1]);
    float t = GRandInRange(item.lmDuration[0], item.lmDuration[1]);
    const float radAngle = GRandInRange(item.lmAngle[0], item.lmAngle[1]);
    // the other items' fades (tFade, tStop), sorted and merged (0x28A868: an overlap takes the later one's end)
    std::vector<std::pair<float, float>> afade;
    for (int j = 0; j < p->citem && j < 8; ++j)
    {
        if (j == i) continue;
        for (int k = 0; k < p->aitem[j].cq && k < 2; ++k) afade.emplace_back(p->aitem[j].aq[k].tFade, p->aitem[j].aq[k].tStop);
    }
    std::stable_sort(afade.begin(), afade.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t n = 0;
    for (size_t k = 0; k < afade.size(); ++k)
    {
        if (k == 0) { n = 1; continue; }
        if (afade[k].first > afade[n - 1].second) afade[n++] = afade[k];
        else afade[n - 1].second = afade[k].second;
    }
    afade.resize(n);
    const float tNow = g_clock.t;
    float dtFade = std::min(t - dtIn, item.sFade);
    for (const auto& f : afade)
    {
        const float s = tNow + t;
        if (f.first <= s && s - dtFade < f.second)
        {
            t = f.second + item.sFade - tNow;
            dtFade = item.sFade;
        }
    }
    q.tStart = tNow;
    q.tStop = tNow + t;
    q.tFull = tNow + dtIn;
    q.tFade = tNow + t - dtFade;
    WRE& wre = p->awre[iwre];
    const float rad = RadNormalize(RadWind3() + 3.1415927f + radAngle);
    const float sSpeed = std::sqrt(sWave * 278.52115f);
    wre.wrek = 0;
    wre.stamp = NextStamp(p);
    float gSin, gCos;
    CalculateSinCos(rad, &gSin, &gCos);   // (0x11FD70: the cylindrical vector (rad, 1, 0))
    const float axis[4] = {gCos, gSin, 0.0f, 0.0f};
    for (int c = 0; c < 4; ++c)
    {
        wre.axis[c] = axis[c];
        wre.vecA[c] = axis[c] * gAmpl;
        wre.vecB[c] = GElf(0x317550 + 4 * uint32_t(c)) * gAmpl;
    }
    wre.rgDot = 1.0f / sWave;
    wre.gFrequency = item.gSpeed * sSpeed / sWave;
    if (item.cq < 2) wre.uPhase = 0.0f;
    else
    {
        const float tt = tNow + dtIn * 0.25f;
        const WRE& wre0 = p->awre[std::clamp(item.aq[0].iwre, 0, 7)];
        wre.uPhase = GModPositive(wre0.gFrequency * tt + wre0.uPhase - wre.gFrequency * tt, 1.0f);
    }
}

// orig3: 0x0028AC30 (each wave's entry amplitude: smoothstep of in / 1 / out)
void UpdateWrqWeights(WR89* p, int i)
{
    WRITEM& item = p->aitem[i];
    const float tNow = g_clock.t;
    for (int k = 0; k < item.cq && k < 2; ++k)
    {
        const WRQ& q = item.aq[k];
        if (q.iwre < 0) break;
        float u;
        if (tNow < q.tFull) u = (tNow - q.tStart) / (q.tFull - q.tStart);
        else if (q.tFade < tNow) u = (q.tStop - tNow) / (q.tStop - q.tFade);
        else u = 1.0f;
        u = std::isnan(u) ? 0.0f : std::clamp(u, 0.0f, 1.0f);
        p->awre[q.iwre].gAmpl = u * u * (3.0f - (u + u));
    }
}

// orig3: 0x0028ADC8's head (vt slot 53, before 0x289F78): once per clock time the ended waves dropped, an item whose
// queue is empty, or whose wave started fading, starts the next one (fading in over the last one's fade out), then
// the amplitudes
void UpdateWr89Items(WR89* p)
{
    if (g_clock.t == p->tLastUpdate) return;
    const int citem = std::clamp(p->citem, 0, 8);
    for (int i = 0; i < citem; ++i)
    {
        WRITEM& item = p->aitem[i];
        while (item.cq > 0 && g_clock.t > item.aq[0].tStop) RemoveWrq(p, item, 0);
    }
    for (int i = 0; i < citem; ++i)
    {
        WRITEM& item = p->aitem[i];
        const WRQ& q = item.aq[0];
        if (item.cq == 0 || (item.cq == 1 && q.tFade < g_clock.t)) StartWrq(p, i, q.tStop - q.tFade);
        UpdateWrqWeights(p, i);
    }
}

// orig3: 0x0028AF48 (method 1048, thunk 0x10B148: a new item, current, with its circle entry of amplitude 0)
void AddWr89Item(WR89* p)
{
    if (p->citem >= 8) return;
    WRITEM& item = p->aitem[p->citem];
    item.sFade = 1.0f;
    item.gSpeed = 1.0f;
    p->iitemCur = p->citem++;
    AddWrCircleWarp(p);
    if (p->cwre > 0) p->awre[p->iwreCur].gAmpl = 0.0f;
}

// the item options 1051..1056 (grf 0x2092 through 0x28AFC0): offset in the item, float count
struct WRITEMFIELD { int eopid3; uint32_t ib; int cg; };
constexpr WRITEMFIELD s_aitemfield[] = {{1051, 0x08, 2}, {1052, 0x00, 2}, {1053, 0x10, 2}, {1054, 0x18, 2}, {1055, 0x20, 1}, {1056, 0x24, 1}};
const WRITEMFIELD* PitemfieldOf(int eopid3)
{
    for (const WRITEMFIELD& f : s_aitemfield)
        if (f.eopid3 == eopid3) return &f;
    return nullptr;
}
float* PgItemField(WRITEM* pitem, uint32_t ib) { return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pitem) + ib); }
static_assert(offsetof(WRITEM, lmAmpl) == 0x08 && offsetof(WRITEM, sFade) == 0x20 && offsetof(WRITEM, gSpeed) == 0x24 && offsetof(WRITEM, cq) == 0x28);

template <int EOPID3> void SetItemFieldT(LO* plo, const CRef& r)
{
    WR89* p = Pwr89Of(plo);
    const WRITEMFIELD* pf = PitemfieldOf(EOPID3);
    if (!p || !pf) return;
    float* pg = PgItemField(PitemCur(p, true), pf->ib);
    if (pf->cg == 1) pg[0] = r.GAsFloat();
    else { const LM lm = r.GetLm(); pg[0] = lm.gMin; pg[1] = lm.gMax; }
}
template <int EOPID3> CRef GetItemFieldT(LO* plo)
{
    WR89* p = Pwr89Of(plo);
    const WRITEMFIELD* pf = PitemfieldOf(EOPID3);
    if (!p || !pf) return CRef(TAGK_Nil);
    const float* pg = PgItemField(PitemCur(p, false), pf->ib);
    CRef ref;
    if (pf->cg == 1) ref.SetF32(pg[0]);
    else ref.SetLm({pg[0], pg[1]});
    return ref;
}
template <int... AN> void RegisterItemFields(std::integer_sequence<int, AN...>)
{
    ((RegisterObjectSetter3(AN, SetItemFieldT<AN>), RegisterObjectGetter3(AN, GetItemFieldT<AN>), 0), ...);
}
struct INITWR89
{
    INITWR89()
    {
        RegisterObjectMethod3(0x10B148, [](LO* plo, const std::vector<CRef>&) {
            if (WR89* p = Pwr89Of(plo)) AddWr89Item(p);
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter3(1049, [](LO* plo) {
            CRef ref;
            ref.SetS32(Pwr89Of(plo) ? Pwr89Of(plo)->citem : 0);
            return ref;
        });
        RegisterObjectGetter3(1050, [](LO* plo) {
            CRef ref;
            ref.SetS32(Pwr89Of(plo) ? Pwr89Of(plo)->iitemCur : 0);
            return ref;
        });
        RegisterObjectSetter3(1050, [](LO* plo, const CRef& r) {
            if (WR89* p = Pwr89Of(plo)) p->iitemCur = r.m_tagk == TAGK_F32 ? static_cast<int>(r.m_g) : r.m_n;
        });
        RegisterItemFields(std::integer_sequence<int, 1051, 1052, 1053, 1054, 1055, 1056>{});
    }
} s_initwr89;

struct INITWR
{
    INITWR()
    {
        // the method options (Sly 3 EOPIDs 1033 / 1034 = Sly 2's 945 / 946: thunks 0x10B058 / 0x10B0D0, Sly 2's 0x10C370 /
        // 0x10C3E8; Sly 2's noise methods 958 / 962 have no Sly 3 twin)
        RegisterObjectMethod(0x10C370, [](LO* plo, const std::vector<CRef>&) {
            if (WR* pwr = PwrOf(plo)) AddWrCircleWarp(pwr);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10C3E8, [](LO* plo, const std::vector<CRef>&) {
            if (WR* pwr = PwrOf(plo)) AddWrBendWarp(pwr);
            return CRef(TAGK_Void);
        });
        // 948 (Sly 3 1036): the wavelength (0x28A518 / 0x28A698)
        RegisterObjectSetter(948, [](LO* plo, const CRef& r) {
            if (WR* pwr = PwrOf(plo)) SetWrWavelength(pwr, r.GAsFloat());
        });
        RegisterObjectGetter(948, [](LO* plo) {
            CRef ref;
            ref.SetF32(PwrOf(plo) ? GetWrWavelength(PwrOf(plo)) : 0.0f);
            return ref;
        });
        // 963 (1045): the entries' count (+0x44); 964 (1046): the current entry (+0x460)
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
        // 1047 (Sly 3 only): the anchor object (+0x688, its setter 0x28A6E0)
        RegisterObjectSetter3(1047, [](LO* plo, const CRef& r) {
            WR* pwr = PwrOf(plo);
            if (!pwr) return;
            if (r.m_tagk == TAGK_Basic) SetWrAnchor(pwr, dynamic_cast<LO*>(r.m_pbasic), -1);
            else if (r.m_tagk == TAGK_S32) SetWrAnchor(pwr, nullptr, r.m_n);
        });
        RegisterObjectGetter3(1047, [](LO* plo) {
            CRef ref;
            ref.SetBasic(PwrOf(plo) ? PloAnchor(PwrOf(plo)) : nullptr);
            return ref;
        });
    }
} s_initwr;

// the run time setters / getters of the fields, one function per EOPID (the registry keeps plain function pointers)
template <int EOPID3> void SetWreFieldT(LO* plo, const CRef& r)
{
    WR* pwr = PwrOf(plo);
    const WRFIELD* pf = PwrfieldOf(EOPID3);
    if (!pwr || !pf) return;
    float ag[3] = {};
    if (pf->cg == 1) ag[0] = r.GAsFloat();
    else if (pf->cg == 2) { const LM lm = r.GetLm(); ag[0] = lm.gMin; ag[1] = lm.gMax; }
    else { const glm::vec3 v = r.GetVector(); ag[0] = v.x; ag[1] = v.y; ag[2] = v.z; }
    SetWreField(pwr, *pf, ag);
}
template <int EOPID3> CRef GetWreFieldT(LO* plo)
{
    WR* pwr = PwrOf(plo);
    const WRFIELD* pf = PwrfieldOf(EOPID3);
    return pwr && pf ? RefWreField(pwr, *pf) : CRef(TAGK_Nil);
}
template <int... AN> void RegisterWreFields(std::integer_sequence<int, AN...>)
{
    ((RegisterObjectSetter3(AN, SetWreFieldT<AN>), RegisterObjectGetter3(AN, GetWreFieldT<AN>), 0), ...);
}
struct INITWRFIELDS
{
    INITWRFIELDS() { RegisterWreFields(std::integer_sequence<int, 1035, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044>{}); }
} s_initwrfields;
}  // namespace

// orig3: 0x0028A558 (0x28A480(0, 2): a circle; its direction D_00317530)
void AddWrCircleWarp(WR* pwr)
{
    WRE* pwre = PwreAddWr(pwr, 0, 2);
    if (!pwre) return;
    for (int i = 0; i < 4; ++i)
    {
        const uint32_t u = g_disc.U32Elf(0x317530 + 4 * uint32_t(i));
        std::memcpy(&pwre->axis[i], &u, 4);
    }
}

// orig3: 0x0028A788 (0x89's slot 7: the WR's init 0x289678, then a circle entry (D_00317530) of amplitude 0)
WR89::WR89()
{
    AddWrCircleWarp(this);
    if (cwre > 0) awre[0].gAmpl = 0.0f;
}

// orig3: 0x0028A588 (0x28A480(1, 1): a constant entry)
void AddWrBendWarp(WR* pwr) { PwreAddWr(pwr, 1, 1); }

// orig3: 0x00289F78 (vt slot 53)
void UpdateWrMatrixes(WR* pwr, WRBG* pwrbg)
{
    if (WR89* p89 = dynamic_cast<WR89*>(pwr)) UpdateWr89Items(p89);   // (0x89's slot 53, 0x28ADC8)
    if (pwrbg && pwrbg->stamp != pwr->stamp) UpdateWrStateVectors(pwr, pwrbg);
    if (g_clock.t == pwr->tLastUpdate) return;
    for (int i = 0; i < pwr->cwre; ++i)
    {
        const WRE& wre = pwr->awre[i];
        if (wre.imat < 0 || wre.imat > 3) continue;
        glm::mat4& matPos = pwr->amatDpos[wre.imat];
        glm::mat4& matUv = pwr->amatDuv[wre.imat];
        const int ic = std::clamp(wre.irow, 0, 3);
        const float g = wre.gAmpl;
        switch (wre.wrek)
        {
        case 0:
        {
            if (ic > 2) break;
            float gSin, gCos;
            CalculateSinCos(RadNormalize((g_clock.t * wre.gFrequency + wre.uPhase) * k2Pi), &gSin, &gCos);
            const glm::vec3 a = Vec3(wre.vecA), b = Vec3(wre.vecB);
            const glm::vec2 uvA(wre.uvA[0], wre.uvA[1]), uvB(wre.uvB[0], wre.uvB[1]);
            // (rows ic / ic + 1: x, y, z of the positions, x, y of the coordinates; the rest kept)
            const glm::vec3 p0 = g * (gCos * a + gSin * b), p1 = g * (-gSin * a + gCos * b);
            const glm::vec2 t0 = g * (gCos * uvA + gSin * uvB), t1 = g * (-gSin * uvA + gCos * uvB);
            matPos[ic] = glm::vec4(p0, matPos[ic].w);
            matPos[ic + 1] = glm::vec4(p1, matPos[ic + 1].w);
            matUv[ic] = glm::vec4(t0, matUv[ic].z, matUv[ic].w);
            matUv[ic + 1] = glm::vec4(t1, matUv[ic + 1].z, matUv[ic + 1].w);
            break;
        }
        case 1:
            matPos[ic] = glm::vec4(g * Vec3(wre.axis), matPos[ic].w);
            matUv[ic] = glm::vec4(g * wre.vecA[0], g * wre.vecA[1], matUv[ic].z, matUv[ic].w);
            break;
        default: break;
        }
    }
    // the anchor: each circle's lanes rotated by the wave's phase at the anchor (g70 * dot(position, +0x20) * 2pi)
    if (LO* ploAnchor = PloAnchor(pwr))
    {
        glm::mat4 am[4] = {glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};
        const glm::vec3 pos = PosLoWorld(ploAnchor);
        for (int i = 0; i < pwr->cwre; ++i)
        {
            const WRE& wre = pwr->awre[i];
            if (wre.wrek != 0 || wre.imat < 0 || wre.imat > 3 || wre.irow < 0 || wre.irow > 2) continue;
            float gSin, gCos;
            CalculateSinCos(wre.rgDot * (glm::dot(pos, Vec3(wre.axis)) * k2Pi), &gSin, &gCos);
            glm::mat4& m = am[wre.imat];
            const int r = wre.irow;
            m[r][r] = gCos;
            m[r + 1][r] = -gSin;
            m[r][r + 1] = gSin;
            m[r + 1][r + 1] = gCos;
        }
        for (int k = 0; k < std::min(pwr->cmat, 4); ++k)
        {
            pwr->amatDposAnchor[k] = MulMatrix4(pwr->amatDpos[k], am[k]);
            pwr->amatDuvAnchor[k] = MulMatrix4(pwr->amatDuv[k], am[k]);
        }
    }
    pwr->tLastUpdate = g_clock.t;
    // (debug: SLY2N_WRLIST=t: each WR's entries and matrices at the first update at or after clock t)
    static const char* s_szList = std::getenv("SLY2N_WRLIST");
    static float s_tPrinted = -1.0f;
    if (s_szList && g_clock.t >= std::atof(s_szList) && (s_tPrinted < 0.0f || s_tPrinted == g_clock.t))
    {
        s_tPrinted = g_clock.t;
        std::printf("WR %d: t %.4f cwre %d cmat %d stamp %d cur %d blocks %zu anchor %d\n", pwr->oid, g_clock.t, pwr->cwre, pwr->cmat, pwr->stamp,
                    pwr->iwreCur, pwr->apwrbg.size(), pwr->ploAnchor ? pwr->ploAnchor->oid : pwr->oidAnchor);
        for (int i = 0; i < pwr->cwre; ++i)
        {
            const WRE& e = pwr->awre[i];
            std::printf("  wre %d kind %d imat %d row %d stamp %d axis %g %g %g A %g %g %g B %g %g %g rg %g f %g ph %g\n", i, e.wrek, e.imat, e.irow,
                        e.stamp, e.axis[0], e.axis[1], e.axis[2], e.vecA[0], e.vecA[1], e.vecA[2], e.vecB[0], e.vecB[1], e.vecB[2], e.rgDot,
                        e.gFrequency, e.uPhase);
        }
        if (const WR89* p89 = dynamic_cast<const WR89*>(pwr))
            for (int i = 0; i < p89->citem && i < 8; ++i)
            {
                const WRITEM& it = p89->aitem[i];
                std::printf("  item %d wavelength %g %g ampl %g %g duration %g %g angle %g %g fade %g speed %g waves %d", i, it.lmWavelength[0],
                            it.lmWavelength[1], it.lmAmpl[0], it.lmAmpl[1], it.lmDuration[0], it.lmDuration[1], it.lmAngle[0], it.lmAngle[1], it.sFade,
                            it.gSpeed, it.cq);
                for (int k = 0; k < it.cq && k < 2; ++k)
                    std::printf(" [wre %d %.2f %.2f %.2f %.2f]", it.aq[k].iwre, it.aq[k].tStart, it.aq[k].tFull, it.aq[k].tFade, it.aq[k].tStop);
                std::printf("\n");
            }
        for (const WRBG* pw : pwr->apwrbg)
            std::printf("  block of %d glob %d wek %d cmat %d dpos %d duv %d inside %d stamp %d states %zu\n", pw->palo ? pw->palo->oid : -1, pw->iglob,
                        pw->wek, pw->cmat, pw->fDpos, pw->fDuv, int(pw->fInside), pw->stamp, pw->astate.size());
    }
}

const glm::mat4* AmatWrDpos(const WR* pwr, const WRBG* pwrbg)
{
    return pwrbg && pwrbg->fInside && pwr->ploAnchor ? pwr->amatDposAnchor : pwr->amatDpos;
}
const glm::mat4* AmatWrDuv(const WR* pwr, const WRBG* pwrbg)
{
    return pwrbg && pwrbg->fInside && pwr->ploAnchor ? pwr->amatDuvAnchor : pwr->amatDuv;
}

// orig: 0x00167B68's end: each warp block of the object's globs bound to the WR its oid names (0x15E938(G, 4, oid,
// owner), then 0x22B7A0: pushed on the WR's list, +0x0C the WR; Sly 3 also binds it to the anchor, 0x2896D0)
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
        BindWrbgAnchor(pwr, pwrbg);
    }
}

const WR* PwrOfWrbg(const WRBG* pwrbg) { return pwrbg ? pwrbg->pwr : nullptr; }

// a copy (LO's clone: the object's fields copied): the entries, matrices, list and anchor
void CloneWrData(const LO* ploSrc, LO* ploDst)
{
    const WR* ps = dynamic_cast<const WR*>(ploSrc);
    WR* pd = dynamic_cast<WR*>(ploDst);
    if (!ps || !pd) return;
    pd->tLastUpdate = ps->tLastUpdate;
    pd->cwre = ps->cwre;
    pd->cmat = ps->cmat;
    pd->stamp = ps->stamp;
    std::copy(std::begin(ps->alane), std::end(ps->alane), std::begin(pd->alane));
    std::copy(std::begin(ps->awre), std::end(ps->awre), std::begin(pd->awre));
    pd->iwreCur = ps->iwreCur;
    std::copy(std::begin(ps->amatDpos), std::end(ps->amatDpos), std::begin(pd->amatDpos));
    std::copy(std::begin(ps->amatDuv), std::end(ps->amatDuv), std::begin(pd->amatDuv));
    std::copy(std::begin(ps->amatDposAnchor), std::end(ps->amatDposAnchor), std::begin(pd->amatDposAnchor));
    std::copy(std::begin(ps->amatDuvAnchor), std::end(ps->amatDuvAnchor), std::begin(pd->amatDuvAnchor));
    pd->apwrbg = ps->apwrbg;
    pd->ploAnchor = ps->ploAnchor;
    pd->oidAnchor = ps->oidAnchor;
    const WR89* ps89 = dynamic_cast<const WR89*>(ploSrc);
    WR89* pd89 = dynamic_cast<WR89*>(ploDst);
    if (ps89 && pd89)
    {
        pd89->citem = ps89->citem;
        pd89->iitemCur = ps89->iitemCur;
        std::copy(std::begin(ps89->aitem), std::end(ps89->aitem), std::begin(pd89->aitem));
        std::copy(std::begin(ps89->afOwned), std::end(ps89->afOwned), std::begin(pd89->afOwned));
    }
}

// The loader's options of a WR (Sly 3 EOPIDs: the entry fields 1035..1044 and the current entry 1046)
bool FLoadWrOption(LO* plo, int eopid, const float* ag, int n)
{
    WR* pwr = PwrOf(plo);
    if (!pwr) return false;
    if (eopid == S3EOPID(964))
    {
        pwr->iwreCur = n;
        return true;
    }
    if (WR89* p89 = Pwr89Of(plo))
    {
        if (eopid == 1050)
        {
            p89->iitemCur = n;
            return true;
        }
        if (const WRITEMFIELD* pf = PitemfieldOf(eopid))
        {
            std::memcpy(PgItemField(PitemCur(p89, true), pf->ib), ag, sizeof(float) * size_t(pf->cg));
            return true;
        }
    }
    const WRFIELD* pf = PwrfieldOf(eopid);
    if (!pf) return false;
    SetWreField(pwr, *pf, ag);
    return true;
}
