// ASEGBL: blends of animation segments (module 0x137A30-0x138F08). See blend.h.
// Reference: Sly2-Reference/Sly2-Release/game/m_137A30_p3.cpp (0x137A30), m_137FB8_p3.cpp (0x137FB8 .. 0x138BA0),
// round4_satellite_low.cpp (0x138970); the Sly 2 decomp's FillBlAmrsg / CalculateBlendAmrsg / ImrsgLookUp.

#include "blend.h"

#include "activate.h"
#include "alo.h"
#include "asega.h"
#include "brx.h"
#include "sw.h"
#include "xf.h"
#include "xfmath.h"

#include <cfloat>
#include <cstdio>
#include <cstdlib>

namespace
{
const float s_gZero = 0.0f;  // orig D_002C44E0

// SLY2_BLEND_DEBUG=1: each BL's contribution to the blended channels (Setup / Eval)
bool FBlendDebug()
{
    static const bool s_f = std::getenv("SLY2_BLEND_DEBUG") != nullptr;
    return s_f;
}

// The channel's time u of the blend and the segment (orig: the start of 0x138660 / 0x138970 / 0x138BA0)
float UOfBlend(const ASEGBL* pbl, int itable, float t, int* piseg)
{
    *piseg = 0;
    if (itable < 0) return t / pbl->TMax();
    const std::vector<MRSG>& amrsg = pbl->aamrsg[itable];
    float dt = 0.0f;
    int iseg = ImrsgLookUp(amrsg, &dt, t, pbl->TMax());
    // (no segment contains t only for an empty / degenerate table: the original reads past the table then)
    if (iseg >= static_cast<int>(amrsg.size())) return *piseg = 0, 0.0f;
    *piseg = iseg;
    return dt / amrsg[iseg].dt;
}

// A BL's own time and rate for the blend's u (the per-BL part of the same functions)
void TimeOfBl(const ASEGBL* pbl, int itable, int iseg, const BLENDBL& bl, float u, float* pt, float* pratio)
{
    const float tMaxBl = PasegdOf(bl.paseg)->tMax;
    if (itable < 0)
    {
        *pt = u * tMaxBl;
        *pratio = tMaxBl / pbl->TMax();
        return;
    }
    const MRSG& ms = bl.aamrsg[itable][iseg];
    float t = ms.t + u * ms.dt;
    if (tMaxBl < t) t -= tMaxBl;
    *pt = t;
    *pratio = ms.dt / pbl->aamrsg[itable][iseg].dt;
}
}  // namespace

LO* PloNewAsegbl() { return new ASEGBL(); }

// orig: 0x00137FB8 FillBlAmrsg__FiiPviiP3OID
void FillBlAmrsg(std::vector<BLENDBL>& abl, int itable, const std::vector<int>& aoid)
{
    for (BLENDBL& bl : abl)
    {
        if (!bl.paseg) continue;
        if (static_cast<int>(bl.aamrsg.size()) <= itable) bl.aamrsg.resize(itable + 1);
        std::vector<MRSG>& amrsg = bl.aamrsg[itable];
        amrsg.assign(aoid.size(), MRSG{});
        const ASEGD* pasegd = PasegdOf(bl.paseg);
        for (size_t i = 0; i < aoid.size(); ++i)
        {
            // orig 0x12F990 (0x12F938: the timed event of kind 11 with that value; 0 when none)
            for (const ASEGEV& ev : pasegd->aaev[1])
                if (ev.kind == 11 && ev.an[0] == aoid[i])
                {
                    amrsg[i].t = ev.t;
                    break;
                }
        }
        if (amrsg.empty()) continue;
        MRSG* pprev = &amrsg.back();
        for (MRSG& ms : amrsg)
        {
            float dt = ms.t - pprev->t;
            if (dt < 0.0f) dt += pasegd->tMax;
            pprev->dt = dt;
            pprev = &ms;
        }
    }
}

// orig: 0x001380F0 CalculateBlendAmrsg__FiiPviiP4MRSGPf
void CalculateBlendAmrsg(ASEGBL* pbl, int itable, float* ptMax)
{
    float gSum = 0.0f;
    if (itable < 0)
    {
        if (!ptMax) return;
        for (const BLENDBL& bl : pbl->abl)
            if (bl.weight != 0.0f) gSum += PasegdOf(bl.paseg)->tMax * bl.weight;
        *ptMax = gSum;
        return;
    }
    std::vector<MRSG>& amrsg = pbl->aamrsg[itable];
    for (size_t i = 0; i < amrsg.size(); ++i)
    {
        amrsg[i].dt = 0.0f;
        for (const BLENDBL& bl : pbl->abl)
            if (bl.weight != 0.0f) amrsg[i].dt += bl.aamrsg[itable][i].dt * bl.weight;
        gSum += amrsg[i].dt;
    }
    // The first segment's phase: the first weighted BL's, moved by the others' differences (wrapped into
    // (-0.5, 0.5]) by weight. (Without a weighted BL the original keeps its caller's $f12: 0 here.)
    float gAvg = 0.0f, gFirst = 0.0f;
    bool fFirst = true;
    for (const BLENDBL& bl : pbl->abl)
    {
        if (bl.weight == 0.0f) continue;
        const float g = bl.aamrsg[itable][0].t / PasegdOf(bl.paseg)->tMax;
        if (fFirst)
        {
            fFirst = false;
            gAvg = gFirst = g;
        }
        else
        {
            float d = g - gFirst;
            if (d <= -0.5f) d += 1.0f;
            else if (0.5f < d) d -= 1.0f;
            gAvg += d * bl.weight;
        }
    }
    if (!amrsg.empty())
    {
        amrsg[0].t = GModPositive(gAvg, 1.0f) * gSum;
        for (size_t i = 1; i < amrsg.size(); ++i)
        {
            const float g = amrsg[i - 1].t + amrsg[i - 1].dt;
            amrsg[i].t = gSum <= g ? g - gSum : g;
        }
    }
    if (ptMax) *ptMax = gSum;
}

// orig: 0x00138370 ImrsgLookUp__FffiP4MRSGPf
int ImrsgLookUp(const std::vector<MRSG>& amrsg, float* pdt, float t, float tMax)
{
    const float eps = 0.0001f;  // 0x38D1B717
    int i = 0;
    for (; i < static_cast<int>(amrsg.size()); ++i)
    {
        const float dtSeg = amrsg[i].dt;
        if (dtSeg == 0.0f) continue;
        float dt = t - amrsg[i].t;
        if (dt < 0.0f) dt += tMax;
        if (dt < dtSeg + eps)
        {
            if (pdt) *pdt = dt;
            return i;
        }
    }
    return i;
}

// orig: 0x001383F0
void SetBlendDominant(ASEGBL* pbl, int ibl)
{
    if (ibl == pbl->iblDominant) return;
    pbl->iblDominant = ibl;
    for (ASEGA* pasega : pbl->apasega)
        for (CTRLASEG* pc = pasega->pctrlFirst; pc; pc = pc->pnextAsega)
        {
            if (!pc->ptrkp || !pc->FFirst()) continue;
            // the record's segment invalidated (unlike 0x22C840, whatever its mode)
            XF* pxf = pc->pxf;
            pxf->state = 0;
            pxf->g1 = -FLT_MAX;  // D_002A4AF0
            pxf->g0 = -FLT_MAX;
            pxf->pdgScale = &s_gZero;
            pxf->pg = &s_gZero;
        }
}

// orig: 0x001384C0
void RecalcBlend(ASEGBL* pbl)
{
    const float lim = 0.9999f;  // 0x3F7FF972
    int j = static_cast<int>(pbl->abl.size()) - 1;
    for (; j >= 0; --j)
        if (lim < pbl->abl[j].weight) break;
    SetBlendDominant(pbl, j);
    for (ASEGA* pasega : pbl->apasega) pasega->t /= pbl->asegd.tMax;
    for (size_t i = 0; i < pbl->aamrsg.size(); ++i) CalculateBlendAmrsg(pbl, static_cast<int>(i), &pbl->asegd.tMax);
    if (pbl->aamrsg.empty()) CalculateBlendAmrsg(pbl, -1, &pbl->asegd.tMax);
    for (ASEGA* pasega : pbl->apasega) pasega->t *= pbl->asegd.tMax;
    if (!pbl->pasegSkel) return;
    const ASEGD* pskel = PasegdOf(pbl->pasegSkel);
    const float r = pbl->asegd.tMax / pskel->tMax;
    std::vector<ASEGEV>& aev = pbl->asegd.aaev[1];
    for (size_t i = 0; i < aev.size() && i < pskel->aaev[1].size(); ++i) aev[i].t = r * pskel->aaev[1][i].t;
}

void SetBlendChannelTable(ASEGCHN* pchn, int itable)
{
    if (auto* p = dynamic_cast<TRKPBL*>(pchn->ptrkp.get())) p->itable = itable;
    if (auto* p = dynamic_cast<TRKGBL*>(pchn->ptrkg.get())) p->itable = itable;
}

// orig: 0x00138660 (the record's segment at its parameter: the dominant BL's own segment; else the weighted sum of
// the BLs' values / velocities at their phases as a fixed value with velocity, state 4)
void TRKPBL::Setup(XF* pxf) const
{
    const int idom = pbl->iblDominant;
    if (idom >= 0)
    {
        if (const TRKP* p = atrk[idom])
        {
            p->Setup(pxf);
            return;
        }
        pxf->state = 2;
        pxf->aseg[0] = v;
        pxf->aseg[2] = v;
        pxf->aseg[1] = glm::vec4(0.0f, 0.0f, 0.0f, pxf->aseg[1].w);
        pxf->aseg[3] = glm::vec4(0.0f, 0.0f, 0.0f, pxf->aseg[3].w);
        ResetXfParams(pxf);
        return;
    }
    int iseg;
    const float u = UOfBlend(pbl, itable, pxf->G(), &iseg);
    glm::vec4 a(0.0f), b(0.0f);
    for (size_t i = 0; i < pbl->abl.size(); ++i)
    {
        const BLENDBL& bl = pbl->abl[i];
        if (bl.weight == 0.0f) continue;
        glm::vec4 c, d;
        if (!atrk[i])
        {
            c = v;
            d = glm::vec4(0.0f);
        }
        else
        {
            float t, ratio;
            TimeOfBl(pbl, itable, iseg, bl, u, &t, &ratio);
            // the BL's segment on a scratch record (memset: g0 = g1 = 0), its curve at t
            XF xf;
            xf.mat = glm::mat4(0.0f);
            xf.matWorld = glm::mat4(0.0f);
            xf.g0 = xf.g1 = 0.0f;
            xf.lo = xf.hi = 0;
            xf.pg = &t;
            xf.pdgScale = nullptr;
            atrk[i]->Setup(&xf);
            EvalHermiteVector(xf.aseg[0], xf.aseg[1], xf.aseg[2], xf.aseg[3], &c, &d, nullptr, xf.g1 - xf.g0,
                              t - xf.g0, ratio);
            if (FBlendDebug())
                std::printf("    setup bl %zu w %.4f kind %d t %.4f ratio %.4f g [%.4f %.4f] seg (%.4f %.4f %.4f) (%.4f %.4f %.4f) -> %.4f %.4f %.4f\n",
                            i, bl.weight, atrk[i]->kind, t, ratio, xf.g0, xf.g1, xf.aseg[0].x, xf.aseg[0].y, xf.aseg[0].z,
                            xf.aseg[2].x, xf.aseg[2].y, xf.aseg[2].z, c.x, c.y, c.z);
        }
        // (vadda with vf0: the w lanes gain 1 per BL)
        const float w = bl.weight;
        a += c * w;
        b += d * w;
        a.w += 1.0f;
        b.w += 1.0f;
    }
    pxf->state = 4;
    pxf->aseg[0] = a;
    pxf->aseg[1] = b;
}

// orig: 0x00138970 (the blended value / velocity at time g, rate dtScale)
void TRKPBL::Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const
{
    int iseg;
    const float u = UOfBlend(pbl, itable, g, &iseg);
    glm::vec4 a(0.0f), b(0.0f);
    glm::vec4 c(0.0f), d(0.0f);  // the last BL's value (orig sp+0x20 / sp+0x30)
    for (size_t i = 0; i < pbl->abl.size(); ++i)
    {
        const BLENDBL& bl = pbl->abl[i];
        if (bl.weight == 0.0f) continue;
        if (!atrk[i])
        {
            // As the original: a BL without the channel restarts the sums at v (velocity 0) and adds the previous
            // BL's value again (its buffers are not refilled).
            a = v;
            b = glm::vec4(0.0f, 0.0f, 0.0f, b.w);
        }
        else
        {
            float t, ratio;
            TimeOfBl(pbl, itable, iseg, bl, u, &t, &ratio);
            atrk[i]->Eval(&c, &d, t, dtScale * ratio);
            if (FBlendDebug())
                std::printf("    eval bl %zu w %.4f kind %d t %.4f -> %.4f %.4f %.4f\n", i, bl.weight, atrk[i]->kind, t, c.x, c.y, c.z);
        }
        const float w = bl.weight;
        a += c * w;
        b += d * w;
        a.w += 1.0f;
        b.w += 1.0f;
    }
    if (ppos) *ppos = a;
    if (pvel) *pvel = b;
}

// orig: 0x00138BA0
void TRKGBL::Eval(float* pg, float* pdg, float t, float dtScale) const
{
    int iseg;
    const float u = UOfBlend(pbl, itable, t, &iseg);
    float sa = 0.0f, sb = 0.0f;
    for (size_t i = 0; i < pbl->abl.size(); ++i)
    {
        const BLENDBL& bl = pbl->abl[i];
        if (bl.weight == 0.0f) continue;
        float a = g, b = 0.0f;
        if (atrk[i])
        {
            float tBl, ratio;
            TimeOfBl(pbl, itable, iseg, bl, u, &tBl, &ratio);
            atrk[i]->Eval(&a, &b, tBl, dtScale * ratio);
        }
        sa += bl.weight * a;
        sb += bl.weight * b;
    }
    if (pg) *pg = sa;
    if (pdg) *pdg = sb;
}

// orig: 0x00137A30 NewBlend
ASEGBL* NewBlend(ALO* palo, std::vector<BLENDBL> abl, const std::vector<int>& actable)
{
    ASEG* pasegFirst = nullptr;
    ASEG* pasegSkel = nullptr;
    LO* ploFrame = nullptr;
    for (const BLENDBL& bl : abl)
    {
        if (!bl.paseg) continue;
        if (!pasegFirst) pasegFirst = bl.paseg;
        const ASEGD* pd = PasegdOf(bl.paseg);
        if (!pasegSkel && (!pd->aaev[0].empty() || !pd->aaev[2].empty() || !pd->aaev[1].empty()))
            pasegSkel = bl.paseg;
        if (!ploFrame && bl.paseg->pframe) ploFrame = bl.paseg;
    }
    if (!pasegFirst) return nullptr;

    // orig PloNew(0x6E, palo, -1, -1), then vt slot 9 (RemoveLo): the blend is no world object of its own
    // (Sly 3: the blend class is 0x83 (Sly 2's 0x6E); its table struct is the ASEG loader's, so the ASEGBL is made here)
    auto* pbl = static_cast<ASEGBL*>(PloNewNative(0x83, g_psw, palo, -1, -1, [] { return static_cast<LO*>(new ASEGBL()); }));
    RemoveLo(g_psw, pbl);
    pbl->aamrsg.resize(actable.size());
    for (size_t i = 0; i < actable.size(); ++i) pbl->aamrsg[i].assign(actable[i], MRSG{});
    for (BLENDBL& bl : abl)
    {
        if (bl.aamrsg.size() < actable.size()) bl.aamrsg.resize(actable.size());
        for (size_t i = 0; i < actable.size(); ++i)
            if (bl.aamrsg[i].size() < static_cast<size_t>(actable[i])) bl.aamrsg[i].resize(actable[i]);
    }
    if (pasegSkel)
    {
        pbl->pasegSkel = pasegSkel;
        pbl->asegd.aaev = PasegdOf(pasegSkel)->aaev;
    }

    // the union of the channels (not those of bit 30), one blended track each
    std::vector<ASEGCHN>& achn = pbl->asegd.achn;
    const size_t cbl = abl.size();
    for (size_t i = 0; i < cbl; ++i)
    {
        if (!abl[i].paseg) continue;
        for (const ASEGCHN& src : PasegdOf(abl[i].paseg)->achn)
        {
            if (src.fDisabled) continue;
            ASEGCHN* pchn = nullptr;
            for (ASEGCHN& c : achn)
                if (c.oid == src.oid && c.isub == src.isub) pchn = &c;
            if (!pchn)
            {
                achn.emplace_back();
                pchn = &achn.back();
                pchn->oid = src.oid;
                pchn->isub = src.isub;
            }
            if (src.fScalar)
            {
                if (!pchn->ptrkg && !pchn->ptrkp)
                {
                    auto p = std::make_unique<TRKGBL>();
                    p->kind = 2;
                    p->pbl = pbl;
                    p->itable = actable.empty() ? -1 : 0;
                    p->atrk.assign(cbl, nullptr);
                    pchn->ptrkg = std::move(p);
                    pchn->fScalar = 1;
                }
                if (auto* p = dynamic_cast<TRKGBL*>(pchn->ptrkg.get())) p->atrk[i] = src.ptrkg.get();
            }
            else
            {
                if (!pchn->ptrkp && !pchn->ptrkg)
                {
                    auto p = std::make_unique<TRKPBL>();
                    p->kind = 5;
                    p->pbl = pbl;
                    p->itable = actable.empty() ? -1 : 0;
                    p->atrk.assign(cbl, nullptr);
                    // orig 0x12FE58 + 0x22CA18: the target record's value now
                    ALO* po = nullptr;
                    XF* pxf = nullptr;
                    GetAsegChannelTarget(pbl, palo, *pchn, &po, &pxf);
                    if (pxf) p->v = VecXfValue(pxf, pxf->mat);
                    pchn->ptrkp = std::move(p);
                }
                if (auto* p = dynamic_cast<TRKPBL*>(pchn->ptrkp.get())) p->atrk[i] = src.ptrkp.get();
            }
        }
    }
    pbl->prio = pasegFirst->prio;
    pbl->iblDominant = -1;
    pbl->svt = pasegFirst->svt;
    if (ploFrame) pbl->pframe = ploFrame->pframe;  // orig 0x256EE0
    for (BLENDBL& bl : abl) bl.weight = bl.paseg != pasegFirst ? 0.0f : 1.0f;
    pbl->abl = std::move(abl);
    RecalcBlend(pbl);
    return pbl;
}
