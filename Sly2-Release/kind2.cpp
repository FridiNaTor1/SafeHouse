// kind2.cpp: the guards' kind-2 entries (an ASEG moved by its marker events 0x22 / 0x23 / 0x24: a hop onto a chair,
// a jump down: the animation plays up to the take-off 0x22, the guard flies to the target point in the time between
// the events, the animation goes on at 0x23 / 0x24 and ends). Part of the AI code. Readable C: sly2-decomp
// (build/hand_wip/func_0020BD88.c, func_0020C2F8.cpp, src/hand/func_0020C0E8.c, func_0020C290.c, func_0020C6F8.c,
// func_0020C080.c, func_00196928.c).

#include "guard.h"

#include "actanim.h"
#include "asega.h"
#include "clock.h"
#include "so.h"
#include "stepphys.h"
#include "step.h"
#include "xf.h"
#include "xfphys.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace
{
std::deque<KIND2>& Pool()
{
    static std::deque<KIND2> s_a;  // (orig the pool 0x3E1AD8)
    return s_a;
}

// the root channel's position at t (0x12FF10 kind 1, vt slot 3)
glm::vec3 PosRootAt(ASEG* paseg, ALO* palo, float t)
{
    glm::vec4 v(0.0f);
    if (const ASEGCHN* pchn = PchnFindAseg(paseg, palo, 1, true))
        if (pchn->ptrkp) pchn->ptrkp->Eval(&v, nullptr, t, 1.0f);  // (its value, not its rate)
    return glm::vec3(v);
}

// orig 0x0012F938: a marker event's time (kind 11 with the value n; -1 none)
bool FMarkerTime(ASEG* paseg, int n, float* pt)
{
    const ASEGD* pd = PasegdOf(paseg);
    if (!pd) return false;
    for (const auto& aev : pd->aaev)
        for (const ASEGEV& ev : aev)
            if (ev.kind == 11 && ev.an[0] == n)
            {
                *pt = ev.t;
                return true;
            }
    return false;
}

// orig 0x0011D508: the real roots of a x^2 + b x + c, ascending
int CSolveQuadratic(float a, float b, float c, float* ax)
{
    const float d = b * b - a * (c * 4.0f);
    if (d < 0.0f) return 0;
    const float a2 = a + a;
    const float alpha = std::sqrt(d) / a2, beta = b / a2;
    if (std::fabs(alpha) < 1e-4f)
    {
        ax[0] = -beta;
        return 1;
    }
    const float r0 = -beta - alpha, r1 = -beta + alpha;
    ax[0] = std::min(r0, r1);
    ax[1] = std::max(r0, r1);
    return 2;
}

// orig 0x00196928: the least flight time keeping the take-off and landing slopes within the limits
float TFlight(const glm::vec3& v, const glm::vec3& a, float gLow, float gHigh, float gSlope0, float gSlope1)
{
    const float az = a.z * 0.5f, naz = -az, vz = v.z;
    float best = 0.0f, r[2];
    int n = CSolveQuadratic(naz, -gLow, vz, r);
    if (n > 0) best = std::max(r[n - 1], best);
    n = CSolveQuadratic(az, -gHigh, vz, r);
    if (n > 0) best = std::max(r[n - 1], best);
    const float l = std::sqrt(v.x * v.x + v.y * v.y);
    float t = (gSlope0 * l - vz) / naz;
    if (t > 0.0f) best = std::max(std::sqrt(t), best);
    t = (gSlope1 * l - vz) / az;
    if (t > 0.0f) best = std::max(std::sqrt(t), best);
    return best;
}

glm::vec3 PosRecord(ALO* palo)
{
    XF* pxf = PxfPos(palo);
    return pxf ? glm::vec3(pxf->matWorld[3]) : PosAloCenter(palo);
}
}  // namespace

// orig: 0x0020C010 / 0x0020BD78 / 0x0020BD88: the entry's object: the events' times, the root's flat distance between
// the take-off and the landing, the gravity the root's height at mid-flight implies
KIND2* Pk2Resolve(GUARD* pg, LO* po) { return Pk2ResolveAlo(pg->palo, po); }
KIND2* Pk2ResolveAlo(ALO* palo, LO* po)
{
    if (!po || po->Cid() != 0x6D) return nullptr;
    ASEG* paseg = static_cast<ASEG*>(po);
    KIND2 k;
    k.n0 = po->oid;
    k.paseg = paseg;
    if (!FMarkerTime(paseg, 0x22, &k.t22) || !FMarkerTime(paseg, 0x23, &k.t23) || !FMarkerTime(paseg, 0x24, &k.t24))
        return nullptr;
    k.dt18 = k.t24 - k.t23;
    k.dt14 = k.t24 - k.t22;
    const glm::vec3 v0 = PosRootAt(paseg, palo, k.t22), v1 = PosRootAt(paseg, palo, k.t24);
    const glm::vec3 d = v0 - v1;
    k.s1C = std::sqrt(d.x * d.x + d.y * d.y);
    const float h = (k.t24 - k.t22) * 0.5f;
    const glm::vec3 v2 = PosRootAt(paseg, palo, k.t22 + h);
    k.g28 = h != 0.0f ? (-v2.z + v0.z) / (h * h * 0.5f) : 0.0f;
    k.f30 = 1;
    Pool().push_back(k);
    return &Pool().back();
}

// orig: 0x0020BF80
void StartK2(const KIND2* ptuning, ALO* palo, int grf, K2PLAY* pk)
{
    if (!ptuning || !ptuning->paseg) return;
    *pk = K2PLAY{};
    pk->ptuning = ptuning;
    pk->palo = palo;
    pk->grf = grf;
    SetK2State(pk, 0);
}

// orig: 0x0020C6F8 (state 0: the target point; 1 / 2: a ballistic flight's controller's point, in the record's parent
// space 0x22FC48)
void SetK2Target(K2PLAY* pk, const glm::vec3& pos)
{
    switch (pk->st)
    {
    case 0: pk->posTarget = pos; break;
    case 1: case 2:
        if (pk->fBallistic && pk->pctrl && pk->palo)
            if (XF* pxf = PxfPos(pk->palo))
                static_cast<CTRLTARGET*>(pk->pctrl)->pos = glm::vec4(PosConvertXfs(nullptr, pxf->pxfRef, pos), 1.0f);
        break;
    default: break;
    }
}

// orig: 0x0020C760: the ballistic controller deleted, the record back to mode 2, flag 26 off
void ReleaseK2Ballistic(K2PLAY* pk)
{
    if (!pk->fBallistic) return;
    pk->fBallistic = 0;
    if (!pk->pctrl) return;
    ALO* palo = pk->palo;
    DeleteCtrl(&pk->pctrl);
    if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 2);
    if (palo->psort) palo->psort->grf148 &= ~0x4000000ull;
    RecalcSwOxfFilterForObject(palo);
}

// orig: 0x0020C2F8
void SetK2State(K2PLAY* pk, int st)
{
    if (st == pk->st) return;
    if (std::getenv("SLY2N_K2DEBUG"))
        std::printf("k2: t %.3f %d state %d -> %d (asega t %.3f) target %.1f %.1f %.1f pos %.1f %.1f %.1f\n", g_clock.t, pk->palo ? pk->palo->oid : -1, pk->st, st,
                    pk->pasega ? pk->pasega->t : -1.0f, pk->posTarget.x, pk->posTarget.y, pk->posTarget.z, pk->palo ? PosRecord(pk->palo).x : 0.0f,
                    pk->palo ? PosRecord(pk->palo).y : 0.0f, pk->palo ? PosRecord(pk->palo).z : 0.0f);
    pk->st = st;
    pk->tSt = g_clock.t;
    ALO* palo = pk->palo;
    const KIND2* pt = pk->ptuning;
    switch (st)
    {
    case 0:
        ApplyAseg(pt->paseg, palo, 0.0f, 1.0f, pk->grf, &pk->pasega);  // (its listener: the guard's messages, kind2.cpp)
        if (SOST* ps = Psost(palo)) pk->dvGravityPrev = ps->dvGravity;  // 0x1F5860
        break;
    case 1:
    {
        // orig: 0x20B210 releases the attachment; 0x20D178 disables and
        // resets the falling watcher while this authored flight is in progress.
        if (FCidDerivedFrom(palo->Cid(), 0x38))
            if (GUARD* pg = PguardFromLo(palo))
            {
                DetachGuardMove(pg);
                pg->watchA.fOn = 0;
                if (pg->watchA.st != 0)
                {
                    pg->watchA.st = 0;
                    pg->watchA.t = g_clock.t;
                }
            }
        const float rate = pk->pasega ? pk->pasega->svt : 1.0f;
        const glm::vec3 accel = glm::vec3(0.0f, 0.0f, pt->g28) * (rate * rate);
        const glm::vec3 delta = pk->posTarget - PosRecord(palo);
        float time;
        if (pk->fBallistic)
        {
            float gLow = 1250.0f, gSlope0 = 1.1f, gHigh = -1250.0f, gSlope1 = -1.1f;
            if (pk->ploRelated && (FCidDerivedFrom(pk->ploRelated->Cid(), 0x13) || FCidDerivedFrom(pk->ploRelated->Cid(), 0xB)))
            {
                gHigh = 0.0f;
                gSlope1 = 50.0f;
            }
            else if (pk->fSteep)
            {
                gLow = 1500.0f;
                gSlope0 = 1.33f;
                gHigh = -1500.0f;
                gSlope1 = -1.33f;
            }
            time = TFlight(delta, accel, gLow, gHigh, gSlope0, gSlope1);
        }
        else
            time = pt->dt14 / rate;
        time = std::max(pt->dt18 / rate, time);
        const glm::vec3 vel = delta * (1.0f / time) + accel * (-0.5f * time);
        SetAloVelocityTo(palo, vel);  // (vt slot 35)
        if (std::getenv("SLY2N_K2DEBUG"))
            std::printf("k2: launch time %.3f rate %.3f accel %.1f vel %.1f %.1f %.1f (ballistic %d, dt14 %.3f dt18 %.3f)\n", time, rate, accel.z,
                        vel.x, vel.y, vel.z, pk->fBallistic, pt->dt14, pt->dt18);
        pk->tArrive = g_clock.t + time;
        pk->tEnd = pk->tArrive - pt->dt18 / rate;
        if (pk->fBallistic)
        {
            // the position record driven to the target at the arrival, with the flight's final velocity (a kind-8
            // controller 0x12DE58, priority 5, in the record's parent space 0x22FC48 / 0x22FCC8), flag 26 and the
            // object's filters again (0x134320)
            if (XF* pxf = PxfPos(palo))
            {
                const glm::vec3 velEnd = vel + accel * time;
                const glm::vec3 posL = PosConvertXfs(nullptr, pxf->pxfRef, pk->posTarget);
                const glm::vec3 velL = VecConvertXfs(nullptr, pxf->pxfRef, velEnd);
                pk->pctrl = PctrlNewTarget(palo, pxf, 5, pk->tArrive, glm::vec4(posL, 1.0f), glm::vec4(velL, 0.0f));
            }
            if (palo->psort) palo->psort->grf148 |= 0x4000000ull;
            RecalcSwOxfFilterForObject(palo);
        }
        SetAloGravity(palo, accel);  // 0x1F5880
        break;
    }
    case 2: if (pk->pasega) SetAsegaTime(pk->pasega, 0, pt->t23, 1.0f); break;
    case 3:
        if (pk->pasega) SetAsegaTime(pk->pasega, 0, pt->t24, 1.0f);
        // (the ballistic flight's controller holds the record at the target until state 4)
        // orig: 0x20B2A8, the landing on the related object: a rail (chair), a ladder / pipe (the climb job 0xB9 and
        // slot 59's case 1, R50) or a bar (the hold job 0xBB and slot 59's case 2). 0x27 (slot 59 case 3) remains
        // unported: not attached.
        if (pk->ploRelated && (FCidDerivedFrom(pk->ploRelated->Cid(), 0xD) || FCidDerivedFrom(pk->ploRelated->Cid(), 0xB) ||
                               FCidDerivedFrom(pk->ploRelated->Cid(), 0x13)))
            if (GUARD* pg = PguardFromLo(palo)) AttachGuardMove(pg, pk->ploRelated, pk->kind);
        break;
    case 4:
        ReleaseK2Ballistic(pk);  // 0x20C760
        SetAloGravity(palo, pk->dvGravityPrev);
        // orig: 0x20D1A0, resume ordinary falling detection after the flight.
        if (GUARD* pg = PguardFromLo(palo)) pg->watchA.fOn = 1;
        break;
    default: break;
    }
}

// orig: 0x0020C0E8: 1 lands (on a floor after 0.15 s: 3) or reaches its end (2); 2 lands, or its arrival is 5 s gone
// (3), or at its arrival (ballistic: 3; otherwise resume the falling watcher); 3 at the animation's end (4)
void UpdateK2(K2PLAY* pk)
{
    int st;
    do
    {
        st = pk->st;
        const bool fFloor = pk->palo && StepPhysOf(pk->palo).pxpFloor != nullptr;  // (+0x208)
        switch (st)
        {
        case 1:
            if (fFloor && pk->tSt + 0.15f < g_clock.t) SetK2State(pk, 3);
            else if (pk->tEnd <= g_clock.t) SetK2State(pk, 2);
            break;
        case 2:
            if (fFloor || g_clock.t - pk->tArrive > 5.0f)  // (D_002C3DE0)
                SetK2State(pk, 3);
            else if (pk->tArrive <= g_clock.t)
            {
                if (pk->fBallistic) SetK2State(pk, 3);
                else if (GUARD* pg = PguardFromLo(pk->palo)) pg->watchA.fOn = 1;  // orig: 0x20D1A0
            }
            break;
        case 3:
            if (!pk->pasega || FAsegaRetracted(pk->pasega)) SetK2State(pk, 4);  // 0x131968
            break;
        default: break;
        }
    } while (st != pk->st);
}

// orig: 0x0020C080
void StopK2(K2PLAY* pk)
{
    if (FK2Done(pk)) return;
    SetK2State(pk, 4);
    RetractAsegaPp(&pk->pasega);
}
