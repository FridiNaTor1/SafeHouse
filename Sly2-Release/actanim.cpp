// The actors' animation side: gaits, root-motion blends, the actor's current animation. See actanim.h.
// Reference: Sly2-Reference/Sly2-Release/game/aseg.cpp (0x12FF10), m_20B840_p3.cpp (0x20B840 .. 0x20BD68), m_1A0300.cpp,
// m_19DE10.cpp, the Sly 2 decomp's func_00135248.c / func_001B6078 (the tails).

#include "actanim.h"

#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "asega.h"
#include "clock.h"
#include "lo.h"
#include "smrun.h"
#include "sw.h"
#include "xf.h"
#include "xfmath.h"

#include <cmath>

std::function<void(int sfx, ALO* palo, float gVolume)> g_pfnGaitFootstep;

// orig: 0x0012FF10
const ASEGCHN* PchnFindAseg(ASEG* paseg, ALO* palo, int k, bool fRoot)
{
    for (const ASEGCHN& chn : PasegdOf(paseg)->achn)
    {
        if (fRoot && !chn.fDisabled) continue;
        if (chn.oid != palo->oid || chn.fScalar) continue;
        const XF* pxf = PxfFindAlo(palo, chn.isub);
        if (pxf && pxf->kind == k) return &chn;
    }
    return nullptr;
}

namespace
{
// the root channel's positions at 0 and at the length (vt slot 3 at (0, 1) and (tMax, 1))
bool FRootEnds(ASEG* paseg, ALO* palo, glm::vec4* ppos0, glm::vec4* ppos1)
{
    const ASEGCHN* pchn = paseg && palo ? PchnFindAseg(paseg, palo, 1, true) : nullptr;
    if (!pchn || !pchn->ptrkp) return false;
    *ppos0 = glm::vec4(0.0f);
    *ppos1 = glm::vec4(0.0f);
    pchn->ptrkp->Eval(ppos0, nullptr, 0.0f, 1.0f);
    pchn->ptrkp->Eval(ppos1, nullptr, PasegdOf(paseg)->tMax, 1.0f);
    return true;
}

ASEGA* PasegaFirst(const ASEGBL* pbl) { return pbl && !pbl->apasega.empty() ? pbl->apasega.front() : nullptr; }
}  // namespace

bool FAsegRootVelocity(ASEG* paseg, ALO* palo, glm::vec4* pvel)
{
    glm::vec4 a, b;
    if (!FRootEnds(paseg, palo, &a, &b)) return false;
    *pvel = (b - a) * (1.0f / PasegdOf(paseg)->tMax);
    return true;
}

// orig: 0x0020B840
void InitGait(GAIT* pgait, ALO* palo, int sfx)
{
    pgait->palo = palo;
    pgait->gVolLo = 0.6f;  // 0x3F19999A
    pgait->sfx = sfx;
    pgait->gVolHi = 1.0f;
}

// orig: 0x0020B868
void BindGait(GAIT* pgait)
{
    if (!pgait->apaseg[0]) return;
    std::vector<BLENDBL> abl(2);
    for (int i = 0; i < 2; ++i) abl[i].paseg = pgait->apaseg[i];
    FillBlAmrsg(abl, 0, {0x17D, 0x17E});  // D_002C3DC0
    FillBlAmrsg(abl, 1, {0x17F, 0x180});  // D_002C3DC8
    for (int i = 0; i < 2; ++i)
        if (pgait->apaseg[i]) FAsegRootVelocity(pgait->apaseg[i], pgait->palo, &pgait->avel[i]);
    pgait->pbl = NewBlend(pgait->palo, std::move(abl), {2, 2});  // D_002C3DD8 {0x20, 0x30} / D_002C3DD0 {2, 2}
    if (!pgait->pbl) return;
    for (ASEGCHN& chn : pgait->pbl->asegd.achn)
    {
        ALO* po = nullptr;
        XF* pxf = nullptr;
        GetAsegChannelTarget(pgait->pbl, pgait->palo, chn, &po, &pxf);
        if (!pxf) continue;  // (the original reads the missing record's tag from address 0x9C)
        const unsigned k = static_cast<unsigned>(pxf->tag - 0x10);
        if (k < 6)
        {
            if (!(k & 1)) SetBlendChannelTable(&chn, 1);
        }
        else
            SetBlendChannelTable(&chn, -1);
    }
    RecalcBlend(pgait->pbl);
}

// orig: 0x0020BAD8 (the w lane gains 1 per entry, as the original's vadda with vf0)
glm::vec4 VecGaitVelocity(const GAIT* pgait)
{
    glm::vec4 v(0.0f);
    const ASEGBL* pbl = pgait->pbl;
    if (!pbl || pbl->aamrsg.empty() || pbl->aamrsg[0].empty()) return v;
    const float r = pbl->aamrsg[0][0].dt;
    for (size_t i = 0; i < 2 && i < pbl->abl.size(); ++i)
    {
        const BLENDBL& bl = pbl->abl[i];
        const float dt = bl.aamrsg.empty() || bl.aamrsg[0].empty() ? 0.0f : bl.aamrsg[0][0].dt;
        const float f = bl.weight * (dt / r);
        v += pgait->avel[i] * f;
        v.w += 1.0f;
    }
    return v;
}

// orig: 0x0020BB58
glm::vec4 SetGaitMix(GAIT* pgait, float g)
{
    if (!pgait->pbl) return glm::vec4(0.0f);
    pgait->gMix = pgait->apaseg[1] ? g : 0.0f;
    pgait->pbl->abl[1].weight = pgait->gMix;
    pgait->pbl->abl[0].weight = 1.0f - g;
    RecalcBlend(pgait->pbl);
    const ASEGA* pasega = PasegaFirst(pgait->pbl);
    const float svt = pasega ? pasega->svt : 0.0f;  // (the original reads address 0x40 without one)
    return VecGaitVelocity(pgait) * -1.0f * svt;
}

// orig: 0x0020BC08
void UpdateGaitFeet(GAIT* pgait)
{
    const ASEGA* pasega = PasegaFirst(pgait->pbl);
    if (!pasega || pgait->pbl->aamrsg.size() < 2) return;
    const float g = pasega->t;
    for (int i = 0; i < 2; ++i)
    {
        const MRSG& ms = pgait->pbl->aamrsg[i][0];
        const float r = GModPositive(g - ms.t, pgait->pbl->TMax());
        const int f = ms.dt <= r ? 1 : 0;
        if (f == pgait->afFoot[i]) continue;
        if (!f && g_pfnGaitFootstep)
        {
            // (0x1FBB90 .. 0x1FB1F8: at the ASEGA's object, range 500 .. 2000, the volume by the mix)
            const float t = pgait->gMix;
            g_pfnGaitFootstep(pgait->sfx, pasega->palo, t * pgait->gVolHi + (1.0f - t) * pgait->gVolLo);
        }
        pgait->afFoot[i] = f;
    }
}

// orig: 0x0020BD68
void ResetGaitFeet(GAIT* pgait)
{
    pgait->afFoot[1] = 0;
    pgait->afFoot[0] = 0;
}

// orig: 0x001A0300
void BindRootBlend(ROOTBLEND* prb, ALO* palo, int c)
{
    std::vector<BLENDBL> abl(c);
    for (int i = 0; i < c; ++i)
    {
        abl[i].paseg = prb->apaseg[i];
        glm::vec4 a, b;
        if (prb->apaseg[i] && FRootEnds(prb->apaseg[i], palo, &a, &b))
            prb->avel[i] = (a - b) * (1.0f / PasegdOf(prb->apaseg[i])->tMax);
    }
    prb->pbl = NewBlend(palo, std::move(abl), {});
}

// orig: 0x0019DE58
void BindSlopeBlend(SLOPEBLEND* psb, ALO* palo)
{
    std::vector<BLENDBL> abl(2);
    for (int i = 0; i < 2; ++i)
    {
        abl[i].paseg = psb->apaseg[i];
        glm::vec4 a, b;
        if (psb->apaseg[i] && FRootEnds(psb->apaseg[i], palo, &a, &b))
            psb->agSlope[i] = (a.z - b.z) / PasegdOf(psb->apaseg[i])->tMax;
    }
    psb->pbl = NewBlend(palo, std::move(abl), {});
}

ASEGBL* PblNewPlain(ALO* palo, const std::vector<ASEG*>& apaseg)
{
    std::vector<BLENDBL> abl(apaseg.size());
    for (size_t i = 0; i < apaseg.size(); ++i) abl[i].paseg = apaseg[i];
    return NewBlend(palo, std::move(abl), {});
}

void SetBlendWeights(ASEGBL* pbl, const std::vector<float>& aw)
{
    if (!pbl) return;
    for (size_t i = 0; i < aw.size() && i < pbl->abl.size(); ++i) pbl->abl[i].weight = aw[i];
    RecalcBlend(pbl);
}

// the tail of 0x135248 / 0x1B6078
void SetActorAnim(ACTANIM* pact, ALO* palo, ASEG* paseg, float t, float svt, int grfapl, SM* psm, OID oidState,
                  uint32_t grfMask, bool fSeekEvent)
{
    SetActorAnimPp(&pact->pasega, &pact->psma, palo, paseg, t, svt, grfapl, psm, oidState, grfMask, fSeekEvent);
}

// (the holder's own fields: the clip's events at its start time (sent by ApplyAseg) see the new ASEGA in the holder,
// as the original's +0x1030 / +0x1034, which the actors' event handlers compare with the event's ASEGA)
void SetActorAnimPp(ASEGA** ppasega, SMA** ppsma, ALO* palo, ASEG* paseg, float t, float svt, int grfapl, SM* psm,
                    OID oidState, uint32_t grfMask, bool fSeekEvent)
{
    RetractAsegaPp(ppasega);   // orig 0x130658
    ClearSmaHandle(ppsma);     // orig 0x1F15B8
    ApplyAsegCur(paseg, palo, t, svt, grfapl, ppasega);  // orig 0x1305B8
    ReplaceSmaHandleMask(psm, palo, oidState, grfapl, grfMask, ppsma);  // orig 0x1F14F8
    ASEGA* const pasega = *ppasega;
    if (!fSeekEvent || !paseg || !pasega) return;
    // 0x1B6078: the seek's arrival by the time of the event 0x1AC (0x12F938)
    float dt = 0.0f;
    for (const ASEGEV& ev : PasegdOf(paseg)->aaev[1])
        if (ev.kind == 11 && ev.an[0] == 0x1AC)
        {
            dt = ev.t;
            break;
        }
    if (dt != 0.0f) SetAsegaSeekTime(pasega, g_clock.t + dt / pasega->svt);
}

void SaveGaitPhase(ACTANIM* pact)
{
    if (pact->pasega) pact->uGait = pact->pasega->t / PasegdOf(pact->pasega->paseg)->tMax;
}

void PlayGait(ACTANIM* pact, GAIT* pgait)
{
    if (!pgait->pbl) return;
    const float t = pact->uGait * pgait->pbl->TMax();
    ResetGaitFeet(pgait);
    SetActorAnim(pact, pgait->palo, pgait->pbl, t, 1.0f, 9);
}

// orig: 0x001B1B18
void SetStepSeg(STEPSEGS* pss, int n, OID oid, int grf)
{
    if (n < 0 || n >= 6 || n == 2 || n == 5) return;
    pss->aoid[n] = oid;
    if (grf != -1) pss->agrf[n] = grf;
}

// orig: 0x001B1BF0
void BindStepSegs(STEPSEGS* pss, ALO* palo)
{
    for (int i = 0; i < 6; ++i)
    {
        if (pss->aoid[i] == -1) continue;
        // orig 0x1B1B98: 0x15E938(G, 1, id, palo), then the ASEG's vt slot 9 (RemoveLo)
        LO* plo = PloFindSwChild(palo, pss->aoid[i]);
        ASEG* paseg = plo && (plo->Cid() == 0x6D || plo->Cid() == 0x6E) ? static_cast<ASEG*>(plo) : nullptr;
        if (paseg) RemoveLo(g_psw, paseg);
        pss->apaseg[i] = paseg;
        pss->aoid[i] = -1;
    }
}

// orig: 0x001B1A38
bool FStepLength(ASEG* paseg, ALO* palo, float* ps)
{
    glm::vec4 a, b;
    if (!paseg || !FRootEnds(paseg, palo, &a, &b)) return false;
    *ps = std::fabs(b.x - a.x) / PasegdOf(paseg)->tMax * paseg->svt;
    return true;
}

void PlayStepState(STEPSEGS* pss, ALO* palo, int st)
{
    if (st == 2 || st < 0 || st >= 6) return;
    ApplyAsegCur(pss->apaseg[st], palo, 0.0f, 1.0f, pss->agrf[st] | 1, &pss->pasega);
}
