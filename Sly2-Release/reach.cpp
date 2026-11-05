// reach.cpp: the reach (reach.h; orig module 0x1CC158..0x1CD2C0). From the original's code: 0x1CC158 / 0x1CC5E8
// (the reference's verified m_1CC158.cpp), 0x1CC168 / 0x1CC288, 0x1CC2E8, 0x1CCAC0, 0x11EE08, 0x204CA8 (the asm).
// The object-frame mode 2 (a grip's frame: 0x1CCA40, 0x1CC1F8 / 0x1CC2B8, the controller +0x80: 0x22FD38 / 0x12D108;
// R03 / R34).
#include "reach.h"

#include "actor.h"
#include "activate.h"
#include "alo.h"
#include "asega.h"
#include "aseg.h"
#include "blend.h"
#include "actanim.h"
#include "brx.h"
#include "clock.h"
#include "crv.h"
#include "jt.h"
#include "lo.h"
#include "so.h"
#include "stepphys.h"
#include "vecmat.h"
#include "xf.h"
#include "xfphys.h"
#include "sw.h"
#include "mat.h"
#include "update.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using cmm::F32;
float TJumpBallistic(const glm::vec3&, const glm::vec3&, float, float, float, float);  // orig 0x196928
float GEventAseg11(ASEG*, int);  // orig 0x12F990
void FadeAloIn(ALO*, float);  // orig 0x1F7C68

namespace
{
constexpr int c_cpoints = 9;  // (0x2C1ED0)

template <class T>
T* PPart(ALO* palo, uint32_t ib)
{
    for (const auto& [ibPart, plo] : palo->apart)
        if (ibPart == ib) return dynamic_cast<T*>(plo);
    return nullptr;
}

// a sampled path at the parameter u (the original's linear PATH of the points u_k = k / (c - 1): 0x1E67B8), in the
// actor's frame
glm::vec3 PosPathU(const std::vector<glm::vec3>& apos, float u)
{
    if (apos.empty()) return glm::vec3(0.0f);
    const int c = static_cast<int>(apos.size());
    const float g = u * static_cast<float>(c - 1);
    int i = static_cast<int>(std::floor(g));
    if (i < 0) return apos.front();
    if (c - 1 <= i) return apos.back();
    const float f = g - static_cast<float>(i);
    return apos[i] + (apos[i + 1] - apos[i]) * f;
}

// orig: 0x0011EE08 (the fraction of the segment a-b nearest p (clamped to [0, 1]) and the distance)
float GNearestSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float* pt)
{
    const glm::vec3 d = b - a, w = p - a;
    float t = ((w.x * d.x + w.y * d.y) + w.z * d.z) / ((d.x * d.x + d.y * d.y) + d.z * d.z);
    if (t < 0.0f) t = 0.0f;
    else if (1.0f < t) t = 1.0f;
    if (pt) *pt = t;
    const glm::vec3 r = p - (a + d * t);
    return std::sqrt((r.x * r.x + r.y * r.y) + r.z * r.z);
}

// orig: 0x001CC168 (the segment between the clips' end paths at u, in the world, against the target); 0x001CC1F8 the
// same over the chain-top record's paths (apos = aposChain, the mode 2)
float GReachDist(REACH* p, const std::vector<glm::vec3>* apos, const glm::vec3& posTarget, float u, float* pw)
{
    const glm::mat4 m = MatJtLo(p->pa->palo);
    const glm::vec3 a = glm::vec3(m * glm::vec4(PosPathU(apos[0], u), 1.0f));
    const glm::vec3 b = glm::vec3(m * glm::vec4(PosPathU(apos[1], u), 1.0f));
    return GNearestSegment(posTarget, a, b, pw);
}

// orig: 0x001CCA40: up to four class-0x14 descendants, in class-list order;
// use the first grip whose mask includes this actor.
LO* PloReachGrip(ACTOR* pa, LO* plo)
{
    int c = 0;
    for (int cid = 0x14; cid < CID_Max && FCidDerivedFrom(cid, 0x14); ++cid)
        for (LO* q : g_psw->mpcidaplo[cid])
        {
            const LO* a = q;
            while (a && a != plo) a = a->paloParent;
            if (!a) continue;
            if (q->U32Field(0, 0xE0) & static_cast<uint32_t>(pa->n1000)) return q;
            if (++c == 4) return nullptr;
        }
    return nullptr;
}

void ReachEndFrame(REACH* p, XF** ppxf, glm::mat4* pmat)
{
    *ppxf = nullptr;
    *pmat = glm::mat4(1.0f);
    if (FIsLoDerivedFrom(p->ploEnd, 0x30)) *ppxf = PxfObj(static_cast<ALO*>(p->ploEnd));
    else if (FIsLoDerivedFrom(p->ploEnd, 3))
    {
        *ppxf = static_cast<LOX*>(p->ploEnd)->pxf;
        *pmat = static_cast<LOX*>(p->ploEnd)->mat;
    }
}

void ClearReachGrabCtrls(REACH* p)
{
    DeleteCtrl(&p->pctrl94);
    DeleteCtrl(&p->pctrl98);
    DeleteCtrl(&p->pctrl9C);
}
}  // namespace

// orig: 0x001CCC88: fly the object to the reach's end at event 0x1C7,
// slowing the actor's animation when the ballistic flight needs more time.
void StartReachGrab(REACH* p, LO* plo)
{
    if (!p->ploEnd || !FIsLoDerivedFrom(plo, 0x30) || !p->pa->pasega) return;
    ClearReachGrabCtrls(p);
    ALO* palo = static_cast<ALO*>(plo);
    ASEGA* a = p->pa->pasega;
    const float tEvent = GEventAseg11(a->paseg, 0x1C7);
    const float dtAnim = (tEvent - a->t) / a->svt;
    XF* pxfEnd = nullptr;
    glm::mat4 local, pose(1.0f);
    ReachEndFrame(p, &pxfEnd, &local);
    CalculateAsegMatrices(a->paseg, p->pa->palo, pxfEnd, PxfObj(p->pa->palo), tEvent, 0.0f, &pose, nullptr);
    glm::mat4 actor(p->pa->matHeading);
    actor[3] = glm::vec4(PosAloCenter(p->pa->palo), 1.0f);
    glm::mat4 target = actor * pose * local;
    if (LO* grip = PloReachGrip(p->pa, plo))
    {
        const glm::mat4 gripLocal = InverseAffineMatrix4(MatJtLo(plo)) * MatJtLo(grip);  // 0x1A71B0
        target = target * InverseAffineMatrix4(gripLocal);
    }
    SetAloParent(palo, nullptr, nullptr, 15);  // 0x1A58D0(p, 0, 15)
    const SOST* ps = Psost(palo);
    const glm::vec3 gravity = ps ? ps->dvGravity : glm::vec3(0.0f, 0.0f, -2250.0f);  // D_002C1EE0
    const glm::vec3 d = glm::vec3(target[3]) - PosAloCenter(palo);
    const float dt = std::max(dtAnim, TJumpBallistic(d, gravity, 400.0f, -400.0f, 1.0f, -1.0f));
    SetAsegaSpeed(a, dtAnim / dt);
    const glm::vec3 vStart = d * (1.0f / dt) - gravity * (dt * 0.5f);
    const glm::vec3 vEnd = d * (1.0f / dt) + gravity * (dt * 0.5f);
    SetAloVelocityTo(palo, vStart);
    p->t94 = g_clock.t + dt;
    XF* pos = PxfPos(palo);
    XF* rot = PxfRot(palo);
    if (pos)
        p->pctrl94 = PctrlNewTarget(palo, pos, 5, p->t94,
            glm::vec4(PosConvertXfs(nullptr, pos->pxfRef, glm::vec3(target[3])), 1.0f),
            glm::vec4(VecConvertXfs(nullptr, pos->pxfRef, vEnd), 0.0f));
    if (rot)
    {
        const glm::vec4 eul(EulerFromMat(MatConvertXfs(nullptr, rot->pxfRef, glm::mat3(target))), 0.0f);
        p->pctrl9C = PctrlNewPos(palo, rot, 5, 9, 2, eul);
        if (p->pctrl9C->FFirst()) rot->tTarget = p->t94;
    }
    SetAloContactsOff(palo, true);  // 0x1F6860(1)
    if (std::getenv("SLY2N_INTERPRINT"))
    {
        const glm::vec3 relative(InverseAffineMatrix4(actor) * target[3]);
        std::printf("reach grab %d: end %d target %.3f %.3f %.3f dt %.6f speed %.6f\n", plo->oid,
                    p->ploEnd->oid, target[3].x, target[3].y, target[3].z, dt, a->svt);
        std::printf("reach grab local %d: %.6f %.6f %.6f\n", plo->oid, relative.x, relative.y, relative.z);
    }
}

// orig: 0x001CD098: parent to the hand's record and hold the object's local pose.
void AttachReachObject(REACH* p, LO* plo)
{
    if (plo == p->ploHeld || !p->ploEnd || !FIsLoDerivedFrom(plo, 0x30)) return;
    ClearReachGrabCtrls(p);
    ALO* palo = static_cast<ALO*>(plo);
    XF* pxfEnd = nullptr;
    glm::mat4 local;
    ReachEndFrame(p, &pxfEnd, &local);
    SetAloParent(palo, p->pa->palo, pxfEnd, 0);
    glm::mat4 mat = MatJtLo(p->ploEnd);
    if (LO* grip = PloReachGrip(p->pa, plo))
        mat = mat * InverseAffineMatrix4(InverseAffineMatrix4(MatJtLo(plo)) * MatJtLo(grip));
    SetAloPos(palo, glm::vec3(mat[3]));  // 0x1A6BE8
    SetAloRotation(palo, glm::mat3(mat));
    if (Psost(palo))
    {
        SetAloCnstrLin(palo, 1);
        SetAloCnstrAng(palo, 1);
    }
    if (XF* r = PxfPos(palo)) p->pctrl98 = PctrlNewPos(palo, r, 5, 0, 2, VecXfValue(r, r->mat));
    if (XF* r = PxfRot(palo)) p->pctrl9C = PctrlNewPos(palo, r, 5, 0, 2, VecXfValue(r, r->mat));
    FadeAloIn(palo, 0.25f);
    p->ploHeld = plo;
    if (std::getenv("SLY2N_INTERPRINT"))
        std::printf("reach hold %d: end %d parent %d record %p pos %.3f %.3f %.3f\n", plo->oid,
                    p->ploEnd->oid, p->pa->palo->oid, static_cast<void*>(pxfEnd), mat[3].x, mat[3].y, mat[3].z);
}

// orig: 0x001CD2C0
void ReleaseReachObject(REACH* p)
{
    if (!p->ploHeld) return;
    ClearReachGrabCtrls(p);
    p->ploHeld = nullptr;
}

// orig: 0x001CD308: the held object put at a target (SCPT command 0x21 through 0x1421F8, e.g. Stealing Voices' wire
// tap and voice modulator into the cable, SCPT 2713: R03): the reach off, the held object's controllers cleared,
// parented like the target (the target's parent with its record (vt slot 32), else the world; grf 15), flown to the
// target's position in 0.25 s (D_002C1EF4) and turned to its orientation; the reach then follows it (+0x84) and holds
// nothing.
void PlaceReachObject(REACH* p, LO* po)
{
    if (!p->ploHeld || !po) return;
    ALO* palo = FIsLoDerivedFrom(p->ploHeld, 0x30) ? static_cast<ALO*>(p->ploHeld) : nullptr;
    SetReachState(p, -1);
    ClearReachGrabCtrls(p);
    if (palo)
    {
        if (po->paloParent)
        {
            // (po's vt slot 32: an ALO's object record, a LOX's record +0x50)
            XF* pxf = nullptr;
            if (FIsLoDerivedFrom(po, 0x30)) pxf = PxfObj(static_cast<ALO*>(po));
            else if (FIsLoDerivedFrom(po, 3)) pxf = static_cast<LOX*>(po)->pxf;
            SetAloParent(palo, po->paloParent, pxf, 15);
        }
        else SetAloParent(palo, nullptr, nullptr, 15);  // 0x1A58D0(p, 0, 15)
        const glm::mat4 matTarget = MatJtLo(po);  // 0x1A62E8 / 0x1A6318: the target's world position and matrix
        // (+0x94's arrival time: what 0x1423A0 waits for in the state 1, so the script's 0x21 (b 0) goes on 0.25 s
        // later, as the reference's; natively t94 was the grab's, already past)
        p->t94 = g_clock.t + 0.25f;  // D_002C1EF4
        if (XF* pos = PxfPos(palo))
            p->pctrl94 = PctrlNewTarget(palo, pos, 5, p->t94,
                                        glm::vec4(PosConvertXfs(nullptr, pos->pxfRef, glm::vec3(matTarget[3])), 1.0f), glm::vec4(0.0f));
        if (XF* rot = PxfRot(palo))
            p->pctrl9C = PctrlNewPos(palo, rot, 5, 7, 2,
                                     glm::vec4(EulerFromMat(MatConvertXfs(nullptr, rot->pxfRef, glm::mat3(matTarget))), 0.0f));
        if (std::getenv("SLY2N_INTERPRINT"))
            std::printf("reach place %d at %d: %.3f %.3f %.3f parent %d\n", palo->oid, po->oid, matTarget[3].x, matTarget[3].y,
                        matTarget[3].z, po->paloParent ? po->paloParent->oid : -1);
    }
    p->ploTarget = p->ploHeld;
    SetReachState(p, 1);
    p->ploHeld = nullptr;
}

// orig: 0x001CC5E8
void BindReach(REACH* p, ACTOR* pa, ASEGA** ppasega, uint32_t ibClip0, uint32_t ibClip1, LO* ploEnd)
{
    ALO* palo = pa->palo;
    p->pa = pa;
    p->ppasega = ppasega;
    p->state = -1;
    p->nMode = -1;
    p->apaseg[0] = PPart<ASEG>(palo, ibClip0);
    p->apaseg[1] = PPart<ASEG>(palo, ibClip1);
    p->ploEnd = ploEnd;
    if (std::getenv("SLY2N_JTPRINT"))
    {
        std::printf("reach %d: clips %p %p end %p; parts:", palo->oid, (void*)p->apaseg[0], (void*)p->apaseg[1], (void*)p->ploEnd);
        for (const auto& [ib, plo] : palo->apart)
            if (ib >= 0x1700 && ib < 0x1900) std::printf(" %X=%d", ib, plo ? plo->oid : -1);
        std::printf("\n");
    }
    if (!p->ploEnd) return;
    // the end's vt slot 32 (+0x38 / +0x40): its record and its matrix in it (an ALO: the object record and the
    // identity; a LOX (e.g. Sly's hand point 643 for the +0x4C0 reach): +0x50 and +0x60)
    XF* pxfEnd = nullptr;
    glm::mat4 matLocal(1.0f);
    if (FIsLoDerivedFrom(p->ploEnd, 0x30))
    {
        ALO* paloEnd = static_cast<ALO*>(p->ploEnd);
        pxfEnd = PxfObj(paloEnd) ? PxfObj(paloEnd) : (paloEnd->axf.empty() ? nullptr : paloEnd->axf[0].get());
    }
    else if (FIsLoDerivedFrom(p->ploEnd, 3))
    {
        pxfEnd = static_cast<LOX*>(p->ploEnd)->pxf;
        matLocal = static_cast<LOX*>(p->ploEnd)->mat;
    }
    else return;
    // +0x38 / +0x40: up the end's chain to the first rotation record (kind 6), the records below it composed into the
    // end's frame in that record's (its own matrix not included: the frame its world matrix gives)
    p->matChain = matLocal;
    p->pxfChain = pxfEnd;
    while (p->pxfChain && p->pxfChain->kind != 6)
    {
        p->matChain = p->pxfChain->mat * p->matChain;
        p->pxfChain = p->pxfChain->pxfRef;
    }
    XF* pxfActor = PxfObj(palo);
    int c = 0;
    for (int i = 0; i < 2; ++i)
    {
        ASEG* paseg = p->apaseg[i];
        p->aposEnd[i].clear();
        p->aposChain[i].clear();
        if (!paseg || !pxfEnd) continue;
        ++c;
        const float tMax = PasegdOf(paseg)->tMax;
        // (A: the end (+0x14 / +0x28); B: the chain-top record's origin (+0x18 / +0x2C), the mode 2's)
        for (int k = 0; k < c_cpoints; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(c_cpoints - 1);
            glm::mat4 m(1.0f);
            CalculateAsegMatrices(paseg, palo, pxfEnd, pxfActor, u * tMax, 0.0f, &m, nullptr);  // 0x1300B8
            p->aposEnd[i].push_back(glm::vec3(m * matLocal[3]));
        }
        if (!p->pxfChain) continue;
        for (int k = 0; k < c_cpoints; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(c_cpoints - 1);
            glm::mat4 m(1.0f);
            CalculateAsegMatrices(paseg, palo, p->pxfChain, pxfActor, u * tMax, 0.0f, &m, nullptr);  // 0x1300B8
            p->aposChain[i].push_back(glm::vec3(m[3]));
        }
    }
    if (c == 2)
    {
        p->aw[0] = 1.0f;
        p->pbl = PblNewPlain(palo, {p->apaseg[0], p->apaseg[1]});  // 0x137A30 (+0x30)
        SetBlendWeights(p->pbl, {p->aw[0], p->aw[1]});
    }
    if (std::getenv("SLY2N_JTPRINT"))
        for (int i = 0; i < 2; ++i)
            for (const glm::vec3& v : p->aposEnd[i]) std::printf("reach %d path %d: %.2f %.2f %.2f\n", palo->oid, i, v.x, v.y, v.z);
}

namespace
{
void PostReachUpdate(REACH* p)
{
    PostDeferredCall(p, 0x1CC2E8, [](void* object, int) { UpdateReach(static_cast<REACH*>(object)); });
}
}  // namespace

// orig: 0x001CCAC0
void SetReachState(REACH* p, int n)
{
    if (n == p->state) return;
    if (std::getenv("SLY2N_REACHTRACE") && p->pa)
    {
        const glm::vec3 e = p->ploEnd ? PosJtLo(p->ploEnd) : glm::vec3(0.0f);
        const glm::vec3 c = p->pxfChain ? glm::vec3(p->pxfChain->matWorld[3]) : glm::vec3(0.0f);
        std::printf("reachstate t %.3f actor %d %d -> %d target %d end %.1f %.1f %.1f chain %.1f %.1f %.1f\n", g_clock.t, p->pa->palo->oid, p->state,
                    n, p->ploTarget ? p->ploTarget->oid : -1, e.x, e.y, e.z, c.x, c.y, c.z);
    }
    // leaving 1 (0x1CD308's placement): its controllers +0x94 / +0x9C freed
    if (p->state == 1)
    {
        DeleteCtrl(&p->pctrl94);
        DeleteCtrl(&p->pctrl9C);
    }
    p->state = n;
    p->tState = g_clock.t;
    if (n == -1)
    {
        DeleteCtrl(&p->pctrl80);  // 0x12E460 (+0x80)
        p->nMode = n;
        p->plo8C = nullptr;
        p->ploTarget = nullptr;
        p->sTarget = 0.0f;
        return;
    }
    if (n < -1 || 2 <= n) return;
    p->nMode = 1;
    p->plo8C = p->ploTarget;
    if (p->ploTarget && FIsLoDerivedFrom(p->ploTarget, 0x0A))
    {
        // a curve: its point nearest the actor (0x1E6F60), followed from there
        if (const CRV* pcrv = PcrvFromLo(p->ploTarget))
            NearestCrvAll(pcrv, PosAloCenter(p->pa->palo), nullptr, nullptr, nullptr, &p->sTarget);
        p->nMode = 0;
    }
    else if (p->ploTarget && FIsLoDerivedFrom(p->ploTarget, 0x14) &&
             (p->ploTarget->U32Field(0, 0xE0) & static_cast<uint32_t>(p->pa->n1000)))
        p->nMode = 2;  // a grip for this actor: its frame
    else if (p->ploTarget && FIsLoDerivedFrom(p->ploTarget, 0x30))
    {
        // an object with a grip for this actor (0x1CCA40): the grip's frame
        if (LO* grip = PloReachGrip(p->pa, p->ploTarget))
        {
            p->plo8C = grip;
            p->nMode = 2;
        }
    }
    if (p->nMode == 2 && p->pxfChain)
    {
        // the chain-top record's rotation driven from its current value (0x12DC18(actor, +0x38, 5, 7, 3, 0x22CA18)).
        // (The original overwrites +0x80 without freeing it; a state change 0 -> 1 does not pass through -1. The
        // reach's own record has one such controller here: the old one is freed.)
        DeleteCtrl(&p->pctrl80);
        p->pctrl80 = PctrlNewPos(p->pa->palo, p->pxfChain, 5, 7, 3, VecXfValue(p->pxfChain, p->pxfChain->mat));
    }
    p->aw[1] = 0.5f;
    p->aw[0] = 0.5f;
    if (p->pbl) SetBlendWeights(p->pbl, {p->aw[0], p->aw[1]});  // 0x1384C0
    // 0x149AB8: a deferred 0x1CC2E8 at the world's deferred calls (0x149C80) of this frame (once per reach): the reach
    // turns the actor once more (with Sly's own call that frame: twice in the frame the reach starts; spireB)
    PostReachUpdate(p);
}

// orig: 0x001CC2E8
void UpdateReach(REACH* p)
{
    if (p->state == -1 || !p->pa) return;
    ALO* palo = p->pa->palo;
    glm::vec3 posTarget(0.0f);
    glm::mat4 matFrame(1.0f);
    switch (p->nMode)
    {
    case 0:
    {
        const CRV* pcrv = p->ploTarget ? PcrvFromLo(p->ploTarget) : nullptr;
        if (!pcrv) return;
        NearestCrvS(pcrv, PosAloCenter(palo), p->sTarget, &posTarget, nullptr, nullptr, &p->sTarget);  // 0x1E7380
        break;
    }
    case 1:
        posTarget = p->plo8C ? PosJtLo(p->plo8C) : glm::vec3(0.0f);
        break;
    case 2:
    {
        // the frame the chain-top record needs for the end to sit at the grip's: the grip's world matrix (vt slot 31)
        // times the inverse of the end's frame in that record (+0x40)
        if (!p->plo8C) return;
        matFrame = MatJtLo(p->plo8C) * InverseAffineMatrix4(p->matChain);  // 0x11C318 / 0x11C168
        posTarget = glm::vec3(matFrame[3]);
        break;
    }
    default:
        return;
    }
    // the target in the actor's frame laid onto its x axis (its horizontal distance), back in the world
    glm::vec3 local = PosJtConvert(nullptr, palo, posTarget);
    local = glm::vec3(std::sqrt(local.x * local.x + local.y * local.y), 0.0f, local.z);
    const glm::vec3 posFlat = PosJtConvert(palo, nullptr, local);
    float u = 0.5f, d = 0.0f, w = 0.0f;
    if (p->nMode == 2)
    {
        // the chain-top record's paths (0x1CC2B8 / 0x1CC1F8), then its rotation: the frame's basis in its reference
        // record's space (0x22FD38) as Euler angles (0x11BFF0) for the controller +0x80 (0x12D108)
        MinimizeRange([&](float uu) { return GReachDist(p, p->aposChain, posFlat, uu, nullptr); }, 0.5f, F32(0x3DCCCCCD), 0.0f, 1.0f, &u, &d);
        GReachDist(p, p->aposChain, posFlat, u, &w);
        if (p->pctrl80 && p->pxfChain)
            SetCtrlPos(static_cast<CTRLPOS*>(p->pctrl80),
                       glm::vec4(EulerFromMat(MatConvertXfs(nullptr, p->pxfChain->pxfRef, glm::mat3(matFrame))), 0.0f));
    }
    else
    {
        MinimizeRange([&](float uu) { return GReachDist(p, p->aposEnd, posFlat, uu, nullptr); }, 0.5f, F32(0x3DCCCCCD), 0.0f, 1.0f, &u, &d);
        GReachDist(p, p->aposEnd, posFlat, u, &w);
    }
    if (p->ppasega && *p->ppasega && p->pbl) SetAsegaTime(*p->ppasega, 0, u * PasegdOf(p->pbl)->tMax, 0.0f);  // 0x130D30
    p->aw[1] = w;
    p->aw[0] = 1.0f - w;
    if (p->pbl) SetBlendWeights(p->pbl, {p->aw[0], p->aw[1]});  // 0x1384C0
    // the actor turns toward the target (0x2046B0), at once (0x204CA8: the rotation's angular velocity z by RadSmooth
    // toward the heading with the step's +0x1F4 SMP over dt 0)
    SetStepPhysHeadingToward(palo, posTarget - PosAloCenter(palo));
    {
        const glm::mat4 m = MatJtLo(palo);
        const float h = std::atan2(m[0][1], m[0][0]);
        XF* pxfRot = PxfRot(palo);
        glm::vec3 wv = pxfRot ? glm::vec3(pxfRot->vel) : AngVelAlo(palo);  // (+0x5C's +0xB0)
        const STEPPHYS& s = StepPhysOf(palo);
        const cmm::SMP smp{s.vBrake.x, s.vBrake.y, s.vBrake.z};
        float dz = wv.z;
        cmm::RadSmooth(h, s.radHeading, 0.0f, &smp, &dz);  // 0x11D2D0
        if (std::getenv("SLY2N_JTPRINT")) std::printf("reach: yaw %.4f heading %.4f w.z %.3f -> %.3f (smp %.1f %.1f %.2f)\n", h, s.radHeading, wv.z, dz, smp.svFast, smp.svSlow, smp.dtFast);
        wv.z = dz;
        SetAloAngVelocityTo(palo, wv);  // vt slot 36
    }
    if (std::getenv("SLY2N_REACHTRACE"))
    {
        // (debug: the end, the target and the chain-top record in the world, per update)
        const glm::vec3 e = p->ploEnd ? PosJtLo(p->ploEnd) : glm::vec3(0.0f);
        const glm::vec3 g = p->plo8C ? PosJtLo(p->plo8C) : glm::vec3(0.0f);
        const glm::vec3 c = p->pxfChain ? glm::vec3(p->pxfChain->matWorld[3]) : glm::vec3(0.0f);
        const glm::mat4 ma = MatJtLo(palo);
        std::printf("reachtrace t %.3f actor %d state %d mode %d target %d u %.3f w %.3f end %.1f %.1f %.1f grip %.1f %.1f %.1f chain %.1f %.1f %.1f"
                    " actor %.1f %.1f %.1f yaw %.4f\n",
                    g_clock.t, palo->oid, p->state, p->nMode, p->plo8C ? p->plo8C->oid : -1, u, w, e.x, e.y, e.z, g.x, g.y, g.z, c.x, c.y,
                    c.z, ma[3].x, ma[3].y, ma[3].z, std::atan2(ma[0][1], ma[0][0]));
    }
    if (std::getenv("SLY2N_JTPRINT") && p->ploEnd && FIsLoDerivedFrom(p->ploEnd, 0x30))
    {
        const glm::vec3 a = PosPathU(p->aposEnd[0], u), b = PosPathU(p->aposEnd[1], u);
        const glm::vec3 e = a + (b - a) * w;
        const glm::vec3 now = PosJtConvert(nullptr, palo, PosAloCenter(static_cast<ALO*>(p->ploEnd)));
        std::printf("reach: end expected %.1f %.1f %.1f, posed %.1f %.1f %.1f; asega t %.3f svt %.2f (blend tMax %.3f)\n", e.x, e.y, e.z, now.x, now.y,
                    now.z, p->ppasega && *p->ppasega ? (*p->ppasega)->t : -1.0f, p->ppasega && *p->ppasega ? (*p->ppasega)->svt : -1.0f,
                    p->pbl ? PasegdOf(p->pbl)->tMax : -1.0f);
    }
    if (std::getenv("SLY2N_JTPRINT"))
        std::printf("reach t %.3f: target %.1f %.1f %.1f (s %.2f) u %.3f w %.3f d %.2f\n", g_clock.t, posTarget.x, posTarget.y, posTarget.z, p->sTarget, u,
                    w, d);
}
