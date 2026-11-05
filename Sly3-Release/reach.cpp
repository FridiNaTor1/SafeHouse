// reach.cpp: the reach (reach.h; orig module 0x1CC158..0x1CD2C0). From the original's code: 0x1CC158 / 0x1CC5E8
// (the reference's verified m_1CC158.cpp), 0x1CC168 / 0x1CC288, 0x1CC2E8, 0x1CCAC0, 0x11EE08, 0x204CA8 (the asm).
// Not yet (each warns once): the object-frame mode 2 (0x1CC1F8 / 0x1CC2B8, the controller +0x80: 0x22FD38 / 0x12D108),
// the classes 0x14 / 0x30 targets (0x1CCA40).
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
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "reach: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

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

// orig: 0x001CC168 (the segment between the clips' end paths at u, in the world, against the target)
float GReachDist(REACH* p, const glm::vec3& posTarget, float u, float* pw)
{
    const glm::mat4 m = MatJtLo(p->pa->palo);
    const glm::vec3 a = glm::vec3(m * glm::vec4(PosPathU(p->aposEnd[0], u), 1.0f));
    const glm::vec3 b = glm::vec3(m * glm::vec4(PosPathU(p->aposEnd[1], u), 1.0f));
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
    XF* pxfActor = PxfObj(palo);
    int c = 0;
    for (int i = 0; i < 2; ++i)
    {
        ASEG* paseg = p->apaseg[i];
        p->aposEnd[i].clear();
        if (!paseg || !pxfEnd) continue;
        ++c;
        const float tMax = PasegdOf(paseg)->tMax;
        for (int k = 0; k < c_cpoints; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(c_cpoints - 1);
            glm::mat4 m(1.0f);
            CalculateAsegMatrices(paseg, palo, pxfEnd, pxfActor, u * tMax, 0.0f, &m, nullptr);  // 0x1300B8
            p->aposEnd[i].push_back(glm::vec3(m * matLocal[3]));
        }
        // (the second path, of the end's record of kind 6 (+0x18 / +0x2C), only for the mode 2: TODO(actors))
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
    // (leaving 1: the controllers +0x94 / +0x9C freed (the mode 2's))
    p->state = n;
    p->tState = g_clock.t;
    if (n == -1)
    {
        // (the controller +0x80 freed: 0x12E460)
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
    else if (p->ploTarget && (FIsLoDerivedFrom(p->ploTarget, 0x14) || FIsLoDerivedFrom(p->ploTarget, 0x30)))
        WarnOnce("0x1CCAC0's classes 0x14 / 0x30 targets (0x1CCA40: the mode 2)");
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
    default:
        WarnOnce("0x1CC2E8's object-frame mode 2");
        return;
    }
    // the target in the actor's frame laid onto its x axis (its horizontal distance), back in the world
    glm::vec3 local = PosJtConvert(nullptr, palo, posTarget);
    local = glm::vec3(std::sqrt(local.x * local.x + local.y * local.y), 0.0f, local.z);
    const glm::vec3 posFlat = PosJtConvert(palo, nullptr, local);
    float u = 0.5f, d = 0.0f, w = 0.0f;
    MinimizeRange([&](float uu) { return GReachDist(p, posFlat, uu, nullptr); }, 0.5f, F32(0x3DCCCCCD), 0.0f, 1.0f, &u, &d);
    GReachDist(p, posFlat, u, &w);
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
