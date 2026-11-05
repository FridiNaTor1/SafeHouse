// climb.cpp: the climb (climb.h, actor.h CLIMB; orig ACTOR +0xF50, module 0x19DE10..0x19EF50). From the original's
// code and the Sly 2 decomp's matching C (0x19DE10, 0x19DE58, 0x19E110, 0x19E950, 0x19E9C8, 0x19EAB0); 0x19DF70,
// 0x19E1E0, 0x117308, 0x205008 from the asm (the reference's are register-level: native_old_climb_state.cpp,
// round4_satellite_mid.cpp).
#include "climb.h"

#include "actanim.h"
#include "actor.h"
#include "alo.h"
#include "asega.h"
#include "aseg.h"
#include "binoc.h"
#include "brx.h"
#include "clock.h"
#include "crv.h"
#include "jt.h"
#include "jump.h"
#include "lo.h"
#include "mat.h"
#include "po.h"
#include "sly.h"
#include "smrun.h"
#include "sound.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "update.h"
#include "util.h"
#include "vecmat.h"
#include "xfmath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using cmm::F32;

namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "climb: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

constexpr uint32_t BTN_CROSS = 0x40, BTN_R3 = 0x400;

template <class T>
T* PPart(ALO* palo, uint32_t ib)
{
    for (const auto& [ibPart, plo] : palo->apart)
        if (ibPart == ib) return dynamic_cast<T*>(plo);
    return nullptr;
}

PADCTL* PpadctlOfA(ACTOR* pa)
{
    PODATA* ppd = PpodataFromLo(pa->palo);
    return ppd ? ppd->ppadctl : nullptr;
}

float GDur(ASEG* paseg) { return paseg ? PasegdOf(paseg)->tMax : 0.0f; }  // +0x74

glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& vDefault)
{
    const float l = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
    if (l < F32(0x38D1B717)) return vDefault;
    return v * (1.0f / l);
}

// the pipe's direction at s (its points 50 either side; 0x19EAB0 / 0x19E1E0)
glm::vec3 DirPipeAround(const CRV* pcrv, float s, glm::vec3* pdiff)
{
    glm::vec3 a(0.0f), b(0.0f);
    EvalCrvS(pcrv, SClampCrv(pcrv, s + 50.0f), &a, nullptr);
    EvalCrvS(pcrv, SClampCrv(pcrv, s - 50.0f), &b, nullptr);
    if (pdiff) *pdiff = a - b;
    return NormalizeOr(a - b, glm::vec3(0.0f, 0.0f, 1.0f));
}

// orig: 0x00117308 (the rotation's Hermite step: from m0 (angular velocity w0) toward m1 (w1) arriving in t1, the
// orientation dt on; dtScale unused by the matrix part. NB the original weights the middle term by (3 - t1), not 3.)
glm::mat3 MatHermiteRotation(const glm::mat3& m0, const glm::vec3& w0, const glm::mat3& m1, const glm::vec3& w1, float t1, float dt)
{
    const float u = dt / t1;
    const float v = 1.0f - u;
    const glm::mat3 r1 = MatRotateVector(w0 * (((u * v) * v) * t1));  // 0x11B570
    const glm::mat3 a = r1 * m0;
    const float g20 = (u * u) * v, g21 = (u * u) * u;
    const glm::mat3 r2 = MatRotateVector(w1 * (-g20 * t1));
    const glm::mat3 b = r2 * m1;
    const glm::mat3 d = MulMatrix3TransposeA(a, b);  // 0x11BC00
    float rad = 0.0f;
    glm::vec3 axis(0.0f);
    DecomposeRotateMatrixRad(d, &rad, &axis);  // 0x11BFB8: the rotation vector
    const float g = g20 * (3.0f - t1) + g21;
    return MatRotateVector(axis * rad * g) * a;
}

void PipeEvent(LO* ppipe, int n)
{
    // the pipe's vt slot 3 (0x1A61E0): its splice event n, then the game's event 0x1A (5) / 0x1B (6)
    HandleLoSpliceEvent(ppipe, static_cast<uint32_t>(n), false, 0, nullptr);
    BroadcastTaskEvent(g_psw, n == 5 ? 0x1A : 0x1B, ppipe);
}
}  // namespace

// orig: 0x0019E110 (the distances the climber keeps to: the whole closed curve; else the ends less 35 / 100 by the
// facing)
cmm::LM LmClimb(ACTOR* pa)
{
    const CRV* pcrv = pa->pcrvF50;
    if (pcrv->fClosed) return cmm::LM{-FLT_MAX, FLT_MAX};
    const float s0 = pcrv->as.front(), s1 = SLengthCrv(pcrv);
    if (pa->climb.fDown) return cmm::LM{s0 + 35.0f, s1 - 100.0f};
    return cmm::LM{s0 + 100.0f, s1 - 35.0f};
}

// orig: 0x00204F78 (the pair of the actor and the nearest ancestor with an attachment of the object (0x1A5BF0: the
// object when it is an ALO, else its parent): overlapping (f) or not, 0x133A38; the actor dirty)
void PairActorWith(ACTOR* pa, LO* plo, bool f)
{
    ALO* pso = nullptr;
    if (plo) pso = FIsLoDerivedFrom(plo, 0x30) ? static_cast<ALO*>(plo) : plo->paloParent;
    while (pso && !Poxa(pso)) pso = pso->paloParent;
    OXA* poxa = Poxa(pa->palo);
    if (poxa && pso && Poxa(pso)) UpdateSwPox(poxa, Poxa(pso), f ? 1 : 2);
    MarkAloDirty(pa->palo);
}


// orig: 0x0019DE10
void InitClimb(ACTOR* pa)
{
    pa->nSubF54 = -1;
    pa->climb = CLIMB{};
}

// orig: 0x0019DE58
void PostLoadClimb(ACTOR* pa)
{
    ALO* palo = pa->palo;
    CLIMB& c = pa->climb;
    c.sb.apaseg[0] = PPart<ASEG>(palo, 0xFA4);
    c.sb.apaseg[1] = PPart<ASEG>(palo, 0xFB4);
    c.pasegHold = PPart<ASEG>(palo, 0xFC4);
    c.pasegIdle = PPart<ASEG>(palo, 0xFC8);
    c.pasegGrab = PPart<ASEG>(palo, 0xFCC);
    if (c.sb.apaseg[0] || c.sb.apaseg[1])
    {
        BindSlopeBlend(&c.sb, palo);
        SetBlendWeights(c.sb.pbl, {c.aw[0], c.aw[1]});
    }
    if (std::getenv("SLY2N_JTPRINT"))
        std::printf("climb: %d clips %d %d (slopes %.1f %.1f) hold %d idle %d grab %d\n", palo->oid, c.sb.apaseg[0] ? c.sb.apaseg[0]->oid : -1,
                    c.sb.apaseg[1] ? c.sb.apaseg[1]->oid : -1, c.sb.agSlope[0], c.sb.agSlope[1], c.pasegHold ? c.pasegHold->oid : -1,
                    c.pasegIdle ? c.pasegIdle->oid : -1, c.pasegGrab ? c.pasegGrab->oid : -1);
}

// orig: 0x0019EAB0
void SetClimbState(ACTOR* pa, int n)
{
    CLIMB& c = pa->climb;
    const int old = pa->nSubF54;
    if (n == old) return;
    LO* ppipe = pa->ploF5C;
    switch (old)
    {
    case -1:
    {
        pa->pcrvF50 = PcrvJtPipe(ppipe);
        if (!pa->pcrvF50) break;
        PipeEvent(ppipe, 5);
        // TODO(actors): 0x1333C0 (the climber listens to the pipe)
        PairActorWith(pa, ppipe, true);  // 0x204F78
        c.nDir = old;
        const glm::vec3 d = DirPipeAround(pa->pcrvF50, pa->sCrvF50, nullptr);
        c.fDown = d.z < -0.5f;
        const cmm::LM lm = LmClimb(pa);
        pa->sCrvF50 = pa->sCrvF50 < lm.gMin ? lm.gMin : (lm.gMax < pa->sCrvF50 ? lm.gMax : pa->sCrvF50);  // 0x11D948
        glm::vec3 point(0.0f);
        EvalCrvS(pa->pcrvF50, pa->sCrvF50, &point, nullptr);
        const glm::vec3 hand = PosJtConvert(pa->palo, nullptr, c.posHand);  // 0x1A6F00(actor, 0, +0x30)
        pa->vecDrawOffset = point - hand;
        pa->vecOffset = glm::vec3(pa->posCmTarget) - point;  // (+0x1070 = +0x1060 - the point, flat)
        pa->vecOffset.z = 0.0f;
        SetActorZone(pa, PloJtDescZone(ppipe, JTL_Pipe));  // 0x140C88 (the pipe's camera zone: its description +0x190's +0)
        if (const int sfx = NPipeEffect(ppipe, false))   // (the pipe's attach sound +0x1D0 at the grip point)
            PsndStartSound(sfx, 1.0f, 0, nullptr, &point);    // 0x1FBB90 / 0x1FB2F8(0) / 0x1FB3F0
        break;
    }
    case 0:
        if (SLY* p = dynamic_cast<SLY*>(pa); p && p->palo == PloPoCurrent(g_psw))
            SetSlyMode(p, PPart<ASEG>(p->palo, 0x14D4) ? 10 : 2);
        break;
    default: break;
    }
    pa->nSubF54 = n;
    c.tState = g_clock.t;
    ASEG* paseg = nullptr;
    float t = 0.0f, svt = 1.0f;
    int grf = 1;
    switch (n)
    {
    case -1:
    {
        if (ppipe)
        {
            PipeEvent(ppipe, 6);
            PairActorWith(pa, ppipe, false);  // 0x204F78
            pa->vecOffset = glm::vec3(pa->posCmTarget) - PosAloCenter(pa->palo);
            pa->vecOffset.z = 0.0f;
            SetActorZone(pa, nullptr);  // 0x140C88
            if (const int sfx = NPipeEffect(ppipe, true))  // (the pipe's release sound +0x1D4 at the actor)
            {
                const glm::vec3 pos = PosAloCenter(pa->palo);  // 0x1A62E8
                PsndStartSound(sfx, 1.0f, 0, nullptr, &pos);  // 0x1FBB90 / 0x1FB2F8(0) / 0x1FB3F0
            }
        }
        pa->ploF5C = nullptr;
        pa->pcrvF50 = nullptr;
        return;
    }
    case 0:
        paseg = c.pasegHold;
        if (!paseg)
        {
            paseg = c.pasegGrab;
            t = GDur(paseg) * 0.5f;
            svt = 0.5f;
        }
        c.gSpeed = 0.0f;
        break;
    case 1: paseg = c.pasegIdle; grf = 9; break;
    case 2: paseg = c.sb.pbl; grf = 9; break;
    case 3: paseg = c.sb.pbl; svt = -1.0f; grf = 9; break;
    case 4:
        c.fDown = !c.fDown;
        paseg = c.pasegGrab;
        break;
    default: break;
    }
    ClearSmaHandle(&pa->psma);                                    // 0x1F15B8
    ApplyAsegCur(paseg, pa->palo, t, svt, grf, &pa->pasega);       // 0x1305B8
    pa->vecVel = glm::vec3(0.0f);
}

// orig: 0x0019E1E0
void UpdateClimbTransitions(ACTOR* pa)
{
    CLIMB& c = pa->climb;
    static const cmm::SMP s_smpOffset{1500.0f, 0.0f, 0.5f};  // 0x2BF928
    pa->vecDrawOffset = cmm::PosSmooth(pa->vecDrawOffset, glm::vec3(0.0f), g_clock.dt, &s_smpOffset, nullptr);  // 0x11F088
    PADCTL* ppc = PpadctlOfA(pa);
    LO* ppipe = pa->ploF5C;
    if (!ppipe || !ppipe->fInWorld || !pa->pcrvF50)
    {
        SetJumpKind(&pa->jump, 0, 0, 0);
        return;
    }
    if (pa->nSubF54 != 0 && ppc && GrfPadctlTakePressed(ppc, BTN_CROSS, 0.25f))
    {
        SetJumpKind(&pa->jump, 2, 0, 0);
        return;
    }
    if (ppc && GrfPadctlPressedWithin(ppc, BTN_R3, F32(0x3E19999A)))
    {
        // (vt slot 99 (3): Sly's 0x18D760; TODO(actors): its checks)
        ForgetPadctlPresses(ppc, BTN_R3);
        OpenBinoc(pa, 0, nullptr, nullptr);  // 0x19CC50
    }
    for (int iter = 0; iter < 8; ++iter)
    {
        const int s0 = pa->nSubF54;
        if ((s0 == 0 || s0 == 4) && pa->pasega && FAsegaRetracted(pa->pasega)) SetClimbState(pa, 1);  // 0x131968
        if (pa->nSubF54 == s0) break;
    }
    if (pa->nSubF54 == 0 || pa->nSubF54 == 4) return;
    const CRV* pcrv = pa->pcrvF50;
    glm::vec3 diff(0.0f);
    const glm::vec3 d = DirPipeAround(pcrv, pa->sCrvF50, &diff);
    const float dz = d.z;
    // a steep pipe climbed the wrong way round: turn (4)
    if (0.5f < std::fabs(dz))
    {
        if ((0.0f < dz) == (c.fDown != 0))
        {
            SetClimbState(pa, 4);
            return;
        }
    }
    if (F32(0x3ECCCCCD) < c.gSpeed && c.fDown && F32(0xBE99999A) < dz)
    {
        SetClimbState(pa, 4);
        return;
    }
    if (c.gSpeed < F32(0xBECCCCCD) && !c.fDown && dz < F32(0x3E99999A))
    {
        SetClimbState(pa, 4);
        return;
    }
    // the stick against the pipe's direction on the screen (0x1C4768: the camera frame's -y / z of it)
    float f3 = 0.0f, f4 = 0.0f;
    if (ppc)
    {
        const glm::mat3& m = ppc->matCam;
        const glm::vec3 cc((m[0].x * diff.x + m[0].y * diff.y) + m[0].z * diff.z, (m[1].y * diff.y + m[1].x * diff.x) + m[1].z * diff.z,
                           (m[2].z * diff.z + m[2].x * diff.x) + m[2].y * diff.y);
        const glm::vec3 v = NormalizeOr(glm::vec3(-cc.y, cc.z, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        f4 = ppc->uJoy;
        f3 = (v.x * ppc->x + v.y * ppc->y) + v.z * 0.0f;
    }
    switch (c.nDir)
    {
    case 0:
        if (f4 < 0.25f) c.nDir = -1;
        else if (f3 < F32(0xBF333333)) c.nDir = 1;
        break;
    case 1:
        if (f4 < 0.25f) c.nDir = -1;
        else if (F32(0x3F333333) < f3) c.nDir = 0;
        break;
    case -1:
        if (F32(0x3EB33333) < f4) c.nDir = 0.0f <= f3 ? 0 : 1;
        break;
    default: break;
    }
    int nNew = -1;
    float u = f4;
    switch (c.nDir)
    {
    case 0: nNew = c.fDown ? 3 : 2; break;
    case 1: nNew = c.fDown ? 2 : 3; break;
    case -1: nNew = 1; u = 0.0f; break;
    default: break;
    }
    float f20;
    if (0.01f < u)
    {
        f20 = (1.0f - u) * c.sb.agSlope[0] + u * c.sb.agSlope[1];
        if (nNew == 3) f20 = -f20;
    }
    else
    {
        f20 = 0.0f;
        nNew = 1;
    }
    if (c.fDown) f20 = -f20;
    const float sMax = std::fabs(c.sb.agSlope[1]);
    const float dt = g_clock.dt;
    c.gSpeed = ((-dt) * f20) / sMax + (1.0f - dt) * c.gSpeed;
    if (!pcrv->fClosed)
    {
        // the speed limited so the distance eases into its limits (0x11D018 with the SMP {|slope 1|, 0, 0.2})
        const cmm::LM lm = LmClimb(pa);
        const cmm::SMP smp{sMax, 0.0f, 0.2f};
        float vLo = 0.0f, vHi = 0.0f;
        cmm::GSmooth(pa->sCrvF50, lm.gMin, dt, &smp, &vLo);
        cmm::GSmooth(pa->sCrvF50, lm.gMax, dt, &smp, &vHi);
        if (f20 < -vHi) f20 = -vHi;
        else if (-vLo < f20) f20 = -vLo;
    }
    pa->gCrvF50 = -f20;
    if (f20 != 0.0f)
    {
        pa->sCrvF50 = pa->sCrvF50 + pa->gCrvF50 * dt;
        MarkAloDirty(pa->palo);  // 0x1F4BF8
    }
    if (pcrv->fClosed) pa->sCrvF50 = GModPositive(pa->sCrvF50, SLengthCrv(pcrv));
    if (std::fabs(f20) < 0.01f) nNew = 1;
    else
    {
        const float a0 = std::fabs(c.sb.agSlope[0]), a1 = std::fabs(c.sb.agSlope[1]);
        float w = (std::fabs(f20) - a0) / (a1 - a0);
        w = w < 0.0f ? 0.0f : (1.0f < w ? 1.0f : w);
        c.aw[1] = w;
        c.aw[0] = 1.0f - w;
        SetBlendWeights(c.sb.pbl, {c.aw[0], c.aw[1]});  // 0x1384C0
    }
    SetClimbState(pa, nNew);
}

// orig: 0x0019DF70
void UpdateClimbMotion(ACTOR* pa)
{
    if (!pa->ploF5C) return;
    const glm::mat3 m = MatPipeFrame(pa->ploF5C, !pa->climb.fDown, pa->sCrvF50);  // 0x1C3500
    glm::mat3 mTarget = m;
    const glm::mat3 rot(MatJtLo(pa->palo));  // 0x1A6318
    if (pa->nSubF54 == 0 || pa->nSubF54 == 4)
    {
        const float dt = g_clock.dt;
        float tLeft = dt;
        if (pa->pasega && pa->pasega->paseg) tLeft = std::fmax(dt, GDur(pa->pasega->paseg) - pa->pasega->t);
        mTarget = MatHermiteRotation(rot, AngVelAlo(pa->palo), m, glm::vec3(0.0f), tLeft, dt);  // 0x117308
    }
    const glm::mat3 dm = MulMatrix3TransposeA(rot, mTarget);  // 0x11BC00
    float rad = 0.0f;
    glm::vec3 axis(0.0f);
    DecomposeRotateMatrixRad(dm, &rad, &axis);
    SetAloAngVelocityTo(pa->palo, axis * (rad / g_clock.dt));  // vt slot 36
    if (std::getenv("SLY2N_CLIMBPRINT"))
        std::printf("climb rot t %.3f: row0 %.3f %.3f %.3f row2 %.3f %.3f %.3f -> target row0 %.3f %.3f %.3f row2 %.3f %.3f %.3f; w %.2f %.2f %.2f\n", g_clock.t,
                    rot[0].x, rot[0].y, rot[0].z, rot[2].x, rot[2].y, rot[2].z, mTarget[0].x, mTarget[0].y, mTarget[0].z, mTarget[2].x, mTarget[2].y,
                    mTarget[2].z, axis.x * rad / g_clock.dt, axis.y * rad / g_clock.dt, axis.z * rad / g_clock.dt);
}

// orig: 0x0019E950
void AddClimbContacts(ACTOR* pa)
{
    if (pa->nSubF54 == -1 || !pa->ploF5C || !pa->pcrvF50) return;
    glm::vec3 pos(0.0f), dir(0.0f);
    EvalCrvS(pa->pcrvF50, pa->sCrvF50, &pos, &dir);
    const glm::vec3 hand = PosJtConvert(pa->palo, nullptr, pa->climb.posHand);
    // vt slot 82 (0x140C30): a pipe out of the world drops the climber
    if (!pa->ploF5C->fInWorld)
    {
        SetJumpKind(&pa->jump, 0, 0, 0);
        return;
    }
    AddActorLineXps(pa, 7, pos - pa->vecDrawOffset, dir, hand);
}

// orig: 0x0019E9C8
void AdjustClimbXp(XP* pxp)
{
    pxp->mu = 0.0f;
    pxp->elas = 0.0f;
}

// orig: 0x00205008 (for objects without an SO: the actor's own internal contacts)
void AddActorLineXps(ACTOR* pa, int grf, const glm::vec3& posA, const glm::vec3& dir, const glm::vec3& posHand)
{
    AddStepLineXps(pa, grf, posA, dir, posHand);
}

// The same STEP slot82 used by guards on a rail (0x209C88).
void AddStepLineXps(STEP* pa, int grf, const glm::vec3& posA, const glm::vec3& dir, const glm::vec3& posHand, LO* related)
{
    ALO* palo = pa->palo;
    if (pa->isoeCustom < 0 || pa->isoeCustom >= static_cast<int>(palo->asoe.size())) return;
    SOE* psoe = &palo->asoe[pa->isoeCustom];  // +0x258
    OXA* poxa = Poxa(palo);
    if (!poxa) return;
    // orig: 0x1A5BF0 / 0x205008: attached rails constrain relative to their
    // nearest physical ancestor; static rails use the step's internal list.
    ALO* target = related ? (FIsLoDerivedFrom(related, 0x30) ? static_cast<ALO*>(related) : related->paloParent) : nullptr;
    while (target && !Poxa(target)) target = target->paloParent;
    SOE* otherEntry = nullptr;
    XP** contacts = &poxa->pxpInternal;
    OX* pair = nullptr;
    if (target)
    {
        OXA* other = Poxa(target);
        pair = poxa->pox;
        while (pair && pair->psoOther != other->pso) pair = pair->poxNext;
        if (!pair || other->apsoe.empty()) return;
        contacts = &pair->pxp;
        otherEntry = other->apsoe.front();
    }
    const glm::mat3 m = BuildLookAtMatrix(dir, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B830 (rows: glm columns)
    const glm::vec3 d = posHand - posA;
    float ac[3];
    for (int i = 0; i < 3; ++i) ac[i] = (m[i].x * d.x + m[i].y * d.y) + m[i].z * d.z;
    if (std::getenv("SLY2N_CLIMBPRINT"))
        std::printf("climb xps t %.3f: pipe point %.1f %.1f %.1f hand %.1f %.1f %.1f center %.1f %.1f %.1f g %.2f %.2f %.2f v %.1f %.1f %.1f still %d (internal %p)\n", g_clock.t,
                    posA.x, posA.y, posA.z, posHand.x, posHand.y, posHand.z, PosAloCenter(palo).x, PosAloCenter(palo).y, PosAloCenter(palo).z,
                    ac[0], ac[1], ac[2], VelAloPos(palo).x, VelAloPos(palo).y, VelAloPos(palo).z, FAloStill(palo) ? 1 : 0, static_cast<void*>(poxa->pxpInternal));
    for (int i = 0; i < 3; ++i)
    {
        const bool fOn = grf & (1 << i);
        if (!fOn && !((grf & (8 << i)) && ac[i] < F32(0x38D1B717))) continue;  // (signed: a one-sided contact)
        const int xpk = fOn ? 0xB : (i == 2 ? 0xE : 0xA);
        PxpAddSw(psoe, otherEntry, posHand, m[i], xpk, nullptr, nullptr, contacts, ac[i]);  // 0x1A7C18 (at the actor's point: end 1 on the line)
    }
    if (pair && pair->poxOther) pair->poxOther->pxp = *contacts;
}
