// The player actors' AI (0x144B28..0x1461C8), using the same stack and path follower as the guards.
#include "actorai.h"
#include "game/sly3.h"
#include "actor.h"
#include "activate.h"
#include "aipursuit.h"
#include "asega.h"
#include "clock.h"
#include "damage.h"
#include "gang.h"
#include "jump.h"
#include "nav.h"
#include "po.h"
#include "so.h"
#include "step.h"
#include "target.h"
#include "xfmath.h"
#include "jt.h"
#include "stepphys.h"
#include "sw.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
glm::vec3 Pos(LO* p) { return PosAi(p); }
float Dxy(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a - b)); }
bool Debug() { static const bool f = std::getenv("SLY2N_COMPANIONDEBUG") != nullptr; return f; }
// orig: 0x0019F540 (the explicit safe-ground entry, then 0x19F590)
void RecoverTo(ACTOR* pa, const glm::vec3& pos)
{
    SAFEGROUND::ENTRY e{};
    e.pos = pos;
    RecoverActorTo(pa, e);
}
// Native equivalent of SOST +0xDC's region, which native navigation queries from the same actual position.
NAVREGION* Region(LO* p) { return PregionAt(Pos(p), !FCidDerivedFrom(p->Cid(), S3CID(0x30))); }
void Straight(CRV* c, const glm::vec3& a, const glm::vec3& b)
{
    c->crvk = 0;
    c->fClosed = 0;
    c->apos = {a, b};
    c->ag = {0.0f, 1.0f};
    RecalcCrv(c, glm::mat4(1.0f));
}
}  // namespace

// orig: 0x001448F0 (D_002BD2D8 10, D_002BD2D0 1)
void SetActorWatchTarget(ACTOR* pa, const glm::vec3& pos)
{
    auto& w = pa->watch;
    if (w.fActive || glm::length(w.pos - pos) < 10.0f) return;
    w.pos = pos;
    w.s = glm::length(Pos(pa->palo) - pos);
    w.t = g_clock.t + 1.0f;
}

// orig: 0x00144640: the ordinary follower's progress/recovery check. The paused contact-wakeup branch is
// shared with the existing actor-watch TODO (0x1619A0 / 0x1F7BF8); active gameplay uses the progress branch.
void UpdateActorWatch(ACTOR* pa)
{
    auto& w = pa->watch;
    if (w.fActive)
    {
        if (pa->jump.state == -1) w.fActive = 0;
        return;
    }
    if ((!pa->n270 && !pa->prunScript) || g_clock.t < w.t) return;
    w.t = g_clock.t + 1.0f;
    // Script runners keep their existing watch behavior; a companion uses the original A8 states 0 / 1.
    auto* p = dynamic_cast<JOBACTORFOLLOW*>(PjobFind(pa, 0xA8));
    if (pa->prunScript || !p || p->st < 0 || p->st >= 2) return;
    const float d = glm::length(Pos(pa->palo) - w.pos);
    if (w.s - d < 30.0f)  // D_002BD2D4 * D_002BD2D0
    {
        glm::vec3 q = w.pos;
        if (FDropOnGround(&q, 100.0f, -150.0f, false))
        {
            RecoverTo(pa, q);
            w.fActive = 1;
        }
    }
    w.s = d;
}

// orig: 0x00144B28
void JOBACTORFOLLOW::Init() { JOBFOLLOW::Init(); uSpeedActor = 1.0f; }
// orig: 0x00144B58
void JOBACTORFOLLOW::Enter() { JOBFOLLOW::Enter(); SetActorWatchTarget(pactor, glm::vec3(pos)); }
// orig: 0x00144B90
void JOBACTORFOLLOW::Update() { Update1789F8(); SetActorWatchTarget(pactor, glm::vec3(pos)); }
// orig: 0x00178CD8 (A8 uses the common state change, without A7's guard animation jobs)
void JOBACTORFOLLOW::SetState(int n)
{
    if (st == n) return;
    st = n;
    tSt = g_clock.t;
}
// orig: 0x00144BC8 (D_002BD2E0 {0,.01,0}, D_002BD2F0 500)
void JOBACTORFOLLOW::Drive(PADCTL* pc)
{
    AIJOB::Drive(pc);
    if (st < 0 || st >= 2) return;
    glm::vec3 d = glm::vec3(pos) - Pos(pactor->palo);
    d.z = 0.0f;
    const float u = std::clamp(glm::length(glm::vec2(d)) * 0.01f, 0.0f, 1.0f);
    if (u > 0.0f)
    {
        pc->uJoy = u * uSpeedActor;
        pc->radJoy = std::atan2(d.y, d.x);
    }
    if (fWalk) pc->grfbtn |= 8;
    if (!pcCopy || !(dtCopy < g_clock.t - tSt)) return;
    bool fOk = true;
    LO* source = pcCopy->plo;
    if (Dxy(Pos(pactor->palo), Pos(source)) > 500.0f) pc->grfbtn |= 8;
    if (ACTOR* pa = PactorFromLo(source)) fOk = ActorJumpKind(pa) != 0;
    if (!fOk) return;
    pc->grfbtn |= pcCopy->grfbtn & grfCopy;
    StampPadctl(pc);
}

// orig: 0x00144E20
void JOBACTORWALK::Init()
{
    AIJOB::Init();
    sArrive = 40.0f;
    fWalk = 1;
    uSpeed = 1.0f;
}

// orig: 0x00144E68
void JOBACTORWALK::Enter()
{
    AIJOB::Enter();
    auto* follow = static_cast<JOBACTORFOLLOW*>(PjobPush(pactor, 0xA8));
    follow->fNoAvoid = fRun;
    follow->sArrive = sArrive;
    follow->fWalk = fWalk;
    follow->uSpeedActor = uSpeed;
    SetGoal(posGoal);
    follow->Enter();
}

// orig: 0x00144EE8
bool JOBACTORWALK::FAtGoal() const
{
    const auto* follow = static_cast<const JOBACTORFOLLOW*>(pchild);
    return !follow || follow->FArrived() || follow->st == 2;
}

// orig: 0x00144F48
void JOBACTORWALK::SetGoal(const glm::vec3& pos)
{
    posGoal = pos;
    if (!pchild) return;
    CRV* curve = &pactor->crv294;
    const REGENT* entry = Pregent(pactor->palo);
    const NAVREGION* region = entry && entry->nGenNav == NGenNav() ? entry->preg : nullptr;
    const glm::vec3 self = Pos(pactor->palo);
    if (!region || fRun)
        Straight(curve, self, posGoal);
    else
        FNavPathCrv(region, self, posGoal, curve);  // a failed search preserves the existing curve
    static_cast<JOBACTORFOLLOW*>(pchild)->Follow(curve, 0);
}

// orig: 0x00145010 (D_002BD330/338 ranges are 0..600 and 0..1200)
void JOBCOMPANION::Init()
{
    AIJOB::Init();
    st = -1;
    dtPredict = 0.25f;
    sNear = 250.0f; sFar = 350.0f; svNear = 1.0f; svFar = 1.2f;
}
// orig: 0x00145080
void JOBCOMPANION::Enter() { AIJOB::Enter(); SetState(0); }
// orig: 0x001450B0
void JOBCOMPANION::Exit() { AIJOB::Exit(); SetState(-1); }

// orig: 0x001450E0: the companion's and followed actor's target records, preserving their list order.
LO* JOBCOMPANION::FindEnemy()
{
    LO* best = nullptr;
    float scoreBest = FLT_MAX;  // D_002A4E30
    const glm::vec3 pos = Pos(ploFollow);
    const uint64_t mask = GrfStateBit(7) | GrfStateBit(0x10) | GrfStateBit(0x16) | GrfStateBit(0x17) |
                          GrfStateBit(0x2A) | GrfStateBit(0x2B);
    auto candidates = [&](TARGETREC* pt, bool fFollow) {
        if (!pt) return;
        for (GUARD* pg : pt->apguard)
        {
            if (pg->grfState & mask) continue;
            float score = glm::length(pos - Pos(pg->palo));
            if (fFollow) score = 200.0f + score;         // D_002BD310
            if (pg->palo == ploEnemy) score = -200.0f + score * 0.8f;  // D_002BD300
            if (FGuardState(pg, 3)) score = -400.0f + score;           // D_002BD320
            if (score < scoreBest) { scoreBest = score; best = pg->palo; }
        }
    };
    candidates(PtargetrecOf(pactor->palo), false);  // actor +0x1088 (0x1400B8)
    candidates(PtargetrecFind(ploFollow), true);
    return best;
}

// orig: 0x001457A8
void JOBCOMPANION::PlanFollow()
{
    posFollow = Pos(ploFollow);
    glm::mat3 basis(1.0f);
    glm::vec3 pos = posFollow;
    if (STEP* ps = PstepFromLo(ploFollow))
    {
        const float c = std::cos(ps->radHeading), s = std::sin(ps->radHeading);
        basis[0] = glm::vec3(c, s, 0); basis[1] = glm::vec3(-s, c, 0); basis[2] = glm::vec3(0, 0, 1);
        pos -= basis * ps->vecVel * dtPredict;
    }
    else if (FCidDerivedFrom(ploFollow->Cid(), S3CID(0x30)))
    {
        ALO* palo = static_cast<ALO*>(ploFollow);
        basis = palo->apxfRef[2] ? glm::mat3(palo->apxfRef[2]->matWorld) : glm::mat3(1.0f);
        pos += VelAloPos(palo) * dtPredict;
    }
    pos += basis * offset;
    const glm::vec3 self = Pos(pactor->palo);
    NAVREGION* preg = Region(pactor->palo);
    CRV* crv = &pactor->crv294;
    if (!preg) Straight(crv, self, pos);
    else if (Region(ploFollow) != preg) FNavPathCrv(preg, self, self, crv);
    else if (!FNavPathCrv(preg, self, pos, crv)) FNavPathCrv(preg, self, self, crv);
    static_cast<JOBACTORFOLLOW*>(pchild)->Follow(crv, 0);
}

// orig: 0x001459C0 (0x145AA8: target's region, SOST +0xDC -> +0x10)
bool JOBCOMPANION::FNeedRecover()
{
    NAVREGION* preg = Region(ploFollow);
    if (!preg || preg == Region(pactor->palo) || (preg->pmesh && preg->pmesh->oid == 0x2CA)) return false;
    if (ACTOR* pa = PactorFromLo(ploFollow); pa && pa->jump.state != -1) return false;
    glm::vec3 pos = Pos(ploFollow);
    if (ItriNav(preg, pos, 0) < 0) return false;
    NAVPOLY* apoly[0x81];
    const int cpoly = CpolyNear(preg, pos, pos, 0x80, apoly);
    for (int i = 0; i < cpoly; ++i)
        if (FPolyContains(apoly[i], pos)) return false;  // 0x1C09A0
    if (!FDropOnGround(&pos, 100.0f, -150.0f, false)) return false;
    posRecover = pos;
    return true;
}

// orig: 0x00145B18
void JOBCOMPANION::SetState(int n)
{
    if (st == n) return;
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    if (Debug()) std::printf("companion: t %.3f actor %d state %d enemy %d\n", g_clock.t, pactor->palo->oid, n,
                             ploEnemy ? ploEnemy->oidInstance : -1);
    if (n == 0)
    {
        auto* p = static_cast<JOBACTORFOLLOW*>(PjobPush(pactor, 0xA8));
        if (PODATA* pd = PpodataFromLo(ploFollow))
        {
            p->pcCopy = pd->ppadctl;
            p->grfCopy = 8;
            p->dtCopy = GRandInRange(0.1f, 0.3f);
        }
        PlanFollow();
        p->Enter();
    }
    else if (n == 1) RecoverTo(pactor, posRecover);
    else if (n == 2)
    {
        const CID cid = pactor->palo->Cid();
        const CID kind = cid == S3CID(0x36) ? 0xC3 : cid == S3CID(0x37) ? 0xC4 : cid == S3CID(0x35) ? 0xC5 : 0;
        if (kind)
        {
            auto* p = PjobPush(pactor, kind);
            if (auto* pm = dynamic_cast<JOBCOMPANIONATTACK*>(p)) pm->ploEnemy = ploEnemy;
            if (p) p->Enter();
        }
    }
}

// orig: 0x001453F0
void JOBCOMPANION::Update()
{
    AIJOB::Update();
    const glm::vec3 a = Pos(pactor->palo), b = Pos(ploFollow);
    const float dxy = Dxy(a, b);
    LO* enemy = FindEnemy();
    const float d = enemy ? std::min(glm::length(Pos(enemy) - a), glm::length(Pos(enemy) - b)) : FLT_MAX;
    while (!FJobDone(this))
    {
        const int old = st;
        switch (st)
        {
        case 0:
            if (FNeedRecover()) SetState(1);
            else if (0.0f <= d && d <= 600.0f) { ploEnemy = enemy; SetState(2); }
            break;
        case 1: if (pactor->jump.state == -1) SetState(0); break;
        case 2:
            if (FNeedRecover()) SetState(1);
            else if (!(0.0f <= d && d <= 1200.0f)) SetState(0);
            else if (auto* p = dynamic_cast<JOBCOMPANIONATTACK*>(pchild); p && p->st == -1) SetState(0);
            break;
        }
        if (st == old) break;
    }
    if (st == 0)
    {
        auto* p = static_cast<JOBACTORFOLLOW*>(pchild);
        if (!Region(pactor->palo)) p->fNoAvoid = 1;
        if (Dxy(posFollow, b) > 200.0f) PlanFollow();  // D_002BD340
        if (pactor->pasega)
        {
            const float u = std::clamp((dxy - sNear) / (sFar - sNear), 0.0f, 1.0f);
            SetAsegaSpeed(pactor->pasega, (1.0f - u) * svNear + u * svFar);
        }
    }
}

// orig: 0x00145CD0 / 0x001461C8 / 0x001466C0
void JOBCOMPANIONATTACK::Init() { AIJOB::Init(); st = -1; }
// orig: 0x00145D00 / 0x001461F8 / 0x001466F0
void JOBCOMPANIONATTACK::Enter()
{
    AIJOB::Enter();
    SetState(0);
    SetActorTargets(pactor, 4, ploEnemy, 4, ploEnemy);
    pactor->grf10E0 |= 1;
}
// orig: 0x00145D68 / 0x00146260 / 0x00146758
void JOBCOMPANIONATTACK::Exit() { AIJOB::Exit(); pactor->grf10E0 &= ~1u; }
// orig: 0x00146158
void JOBMURRAYATTACK::SetState(int n)
{
    if (st == n) return;
    if (st == 2) UnlockJob(this);
    st = n;
    tSt = g_clock.t;
    if (st == 2) LockJob(this);
    if (Debug()) std::printf("companion attack: t %.3f actor %d state %d enemy %d\n", g_clock.t, pactor->palo->oid, n,
                             ploEnemy ? ploEnemy->oidInstance : -1);
}
// orig: 0x00145DB0
void JOBMURRAYATTACK::Update()
{
    AIJOB::Update();
    if (st == -1) return;
    GUARD* pg = PguardFromLo(ploEnemy);
    if (pactor->nSubDF4 != -1 || !ploEnemy || !FLoActive(ploEnemy) || (pg && pg->f330)) { SetState(-1); return; }
    glm::vec3 d = Pos(ploEnemy) - Pos(pactor->palo);
    d.z = 0.0f;
    const float len = glm::length(d);
    const glm::vec3 dir = len < 1e-4f ? glm::vec3(1, 0, 0) : d * (1.0f / len);  // D_002A2DA0
    int old;
    do
    {
        old = st;
        switch (st)
        {
        case 0:
            if (len < 300.0f)
            {
                const ALO* palo = pactor->palo;
                const glm::vec3 facing = palo->apxfRef[2] ? glm::vec3(palo->apxfRef[2]->matWorld[0]) : glm::vec3(1, 0, 0);
                if (0.8f < glm::dot(facing, dir)) SetState(2);
            }
            else if (350.0f < len) SetState(1);
            break;
        case 1: if (len < 250.0f) SetState(0); break;
        case 2:
            if (auto* pm = dynamic_cast<MURRAY*>(pactor); pm && pm->n1810 == -1 && 0.1f < g_clock.t - tSt) SetState(-1);
            break;
        }
    } while (old != st);
}
// orig: 0x00146068: synthesize ordinary Square input for 0.1 s; contact/damage remains the punch sensor's work.
void JOBMURRAYATTACK::Drive(PADCTL* pc)
{
    AIJOB::Drive(pc);
    if (st < 0 || st >= 3) return;
    const glm::vec3 d = Pos(ploEnemy) - Pos(pactor->palo);
    pc->uJoy = st == 0 ? 0.1f : 1.0f;
    pc->radJoy = std::atan2(d.y, d.x);
    if (st == 2 && g_clock.t - tSt < 0.1f) { pc->grfbtn |= 0x80; StampPadctl(pc); }
    if (Debug()) std::printf("companion pad: t %.3f actor %d state %d u %.3f heading %.3f buttons %X enemy %d\n",
                             g_clock.t, pactor->palo->oid, st, pc->uJoy, pc->radJoy, pc->grfbtn, ploEnemy->oidInstance);
}

namespace
{
// The enemy's direction in xy (D_002A2DA0 when degenerate) and its length.
float DirToEnemy(ACTOR* pa, LO* ploEnemy, glm::vec3* pdir)
{
    glm::vec3 d = Pos(ploEnemy) - Pos(pa->palo);
    d.z = 0.0f;
    const float len = glm::length(d);
    *pdir = len < 1e-4f ? glm::vec3(1, 0, 0) : d * (1.0f / len);
    return len;
}
bool FFacing(ACTOR* pa, const glm::vec3& dir)
{
    const ALO* palo = pa->palo;
    const glm::vec3 facing = palo->apxfRef[2] ? glm::vec3(palo->apxfRef[2]->matWorld[0]) : glm::vec3(1, 0, 0);
    return 0.8f < glm::dot(facing, dir);
}
bool FAttackEnds(ACTOR* pa, LO* ploEnemy)
{
    GUARD* pg = PguardFromLo(ploEnemy);
    return pa->nSubDF4 != -1 || !ploEnemy || !FLoActive(ploEnemy) || (pg && pg->f330);
}
}  // namespace

// orig: 0x00146650
void JOBBENTLEYATTACK::SetState(int n)
{
    if (st == n) return;
    if (st == 2) UnlockJob(this);
    st = n;
    tSt = g_clock.t;
    if (st == 2) LockJob(this);
}
// orig: 0x001462A8 (D_002BD350 350, D_002BD358 {250, 350})
void JOBBENTLEYATTACK::Update()
{
    AIJOB::Update();
    if (st == -1) return;
    if (FAttackEnds(pactor, ploEnemy)) { SetState(-1); return; }
    glm::vec3 dir;
    const float len = DirToEnemy(pactor, ploEnemy, &dir);
    int old;
    do
    {
        old = st;
        switch (st)
        {
        case 0:
            if (len < 350.0f) { if (FFacing(pactor, dir)) SetState(2); }
            else if (350.0f < len) SetState(1);
            break;
        case 1: if (len < 250.0f) SetState(0); break;
        case 2:
            if (BENTLEY* pb = PbentleyFromLo(pactor->palo); pb && pb->sub.state == -1 && 0.1f < g_clock.t - tSt)
                SetState(-1);
            break;
        }
    } while (old != st);
}
// orig: 0x00146560
void JOBBENTLEYATTACK::Drive(PADCTL* pc)
{
    AIJOB::Drive(pc);
    if (st < 0 || st >= 3) return;
    const glm::vec3 d = Pos(ploEnemy) - Pos(pactor->palo);
    pc->uJoy = st == 0 ? 0.1f : 1.0f;
    pc->radJoy = std::atan2(d.y, d.x);
    if (st == 2 && g_clock.t - tSt < 0.1f) { pc->grfbtn |= 0x80; StampPadctl(pc); }
}

// orig: 0x00146E18 (the strikes 2..4 hold the actor)
void JOBSLYATTACK::SetState(int n)
{
    if (st == n) return;
    if (st >= 2 && st <= 4) UnlockJob(this);
    st = n;
    tSt = g_clock.t;
    if (st >= 2 && st <= 4) LockJob(this);
    if (Debug()) std::printf("companion sly attack: t %.3f actor %d state %d enemy %d\n", g_clock.t, pactor->palo->oid, n,
                             ploEnemy ? ploEnemy->oidInstance : -1);
}
// orig: 0x001467A0 (D_002BD360 450, D_002BD368 400, D_002BD36C 550, D_002BD370 0.2)
void JOBSLYATTACK::Update()
{
    AIJOB::Update();
    if (st == -1) return;
    if (FAttackEnds(pactor, ploEnemy)) { SetState(-1); return; }
    glm::vec3 dir;
    const float len = DirToEnemy(pactor, ploEnemy, &dir);
    SLY* ps = PslyFromLo(pactor->palo);
    int old;
    do
    {
        old = st;
        switch (st)
        {
        case 0:
            if (len < 450.0f)
            {
                if (FFacing(pactor, dir)) SetState(GRandInRange(0.0f, 1.0f) < 0.3f ? 3 : 2);
            }
            else if (550.0f < len) SetState(1);
            break;
        case 1: if (len < 400.0f) SetState(0); break;
        case 2: if (ps && ps->mode == -1 && 0.1f < g_clock.t - tSt) SetState(-1); break;
        case 3:
            if (0.1f < g_clock.t - tSt)
            {
                if (pactor->jump.state == -1) SetState(-1);
                else if (pactor->jump.state == 1 && 0.2f < g_clock.t - pactor->jump.t20) SetState(4);
            }
            break;
        case 4: if (0.1f < g_clock.t - tSt && ps && ps->state != 13) SetState(-1); break;
        }
    } while (old != st);
}
// orig: 0x00146B18: 0 leaves the stick; 3 jumps toward him (speed 0.1..1 by the distance: -0.25 + d / 640)
void JOBSLYATTACK::Drive(PADCTL* pc)
{
    AIJOB::Drive(pc);
    if (st < 1 || st > 4) return;
    const glm::vec3 d = Pos(ploEnemy) - Pos(pactor->palo);
    pc->radJoy = std::atan2(d.y, d.x);
    pc->uJoy = 1.0f;
    const bool fNew = g_clock.t - tSt < 0.1f;
    if (st == 3)
    {
        pc->uJoy = std::clamp(-0.25f + glm::length(glm::vec2(d)) * 0.0015625f, 0.1f, 1.0f);
        if (fNew && pactor->jump.state == -1) { pc->grfbtn |= 0x40; StampPadctl(pc); }
    }
    else if ((st == 2 || st == 4) && fNew) { pc->grfbtn |= 0x80; StampPadctl(pc); }
    if (Debug()) std::printf("companion sly pad: t %.3f actor %d state %d u %.3f heading %.3f buttons %X enemy %d\n",
                             g_clock.t, pactor->palo->oid, st, pc->uJoy, pc->radJoy, pc->grfbtn, ploEnemy->oidInstance);
}

// orig: 0x00146E98: Murray's +0x1AC0 on (the charge goes on: 0x1B8EC0), his state 12 (0x1B6078), its wind-up (0x1B8D30)
void JOBMURRAYRAGE::Enter()
{
    AIJOB::Enter();
    MURRAY* pm = PmurrayFromLo(pactor->palo);
    if (!pm) return;
    pm->palo->SetField(0, 0x1AC0, 1);
    SetMurrayState(pm, 12);
    SetMurrayBerserk(pm, 0);
}
// orig: 0x00146EE0
void JOBMURRAYRAGE::Exit()
{
    AIJOB::Exit();
    MURRAY* pm = PmurrayFromLo(pactor->palo);
    if (!pm) return;
    pm->palo->SetField(0, 0x1AC0, 0);
    SetMurrayState(pm, 0);
}
// orig: 0x00146F18: the wind-up faces Sly (0x2046B0) and keeps the heading; the charge turns toward him at 15 rad/s
// at most (0x11CFE0) while he is within 0.5 rad of that heading (D_002BD374 / D_002BD378)
void JOBMURRAYRAGE::Update()
{
    AIJOB::Update();
    MURRAY* pm = PmurrayFromLo(pactor->palo);
    LO* ploSly = nullptr;
    if (g_psw)
        for (LO* p : g_psw->mpcidaplo[S3CID(0x35)])
            if (!p->ploCloneOf) { ploSly = p; break; }  // 0x2E1E40
    if (!pm) return;
    switch (pm->n1840)
    {
    case 0:
        if (ploSly) SetStepPhysHeadingToward(pm->palo, PosLoWorld(ploSly) - PosLoWorld(pm->palo));
        radCharge = pm->radHeading;
        break;
    case 1:
        if (ploSly)
        {
            const glm::vec3 d = PosLoWorld(ploSly) - PosLoWorld(pm->palo);
            const float rad = std::atan2(d.y, d.x);
            if (std::fabs(RadNormalize(rad - radCharge)) < 0.5f)
            {
                const glm::vec3 x(MatJtLo(pm->palo)[0]);
                const float radCur = std::atan2(x.y, x.x);  // 0x204740
                SetStepPhysHeading(pm->palo, radCur + GLimitAbs(RadNormalize(rad - radCur), 15.0f * g_clock.dt));
            }
        }
        break;
    default: break;
    }
}

void SetMurrayRage(ACTOR* pa, bool f)
{
    if (!pa) return;
    ClearJobs(pa);  // 0x1EECD0 (+0x26C)
    if (!f) return;
    AIJOB* pj = PjobPush(pa, 0xC6);  // 0x205438
    pj->Enter();                     // vt slot 8
}

// orig: 0x001EF138 (the actor-job cases)
std::unique_ptr<AIJOB> PjobNewActor(CID cid)
{
    switch (cid)
    {
    case 0xA8: return std::make_unique<JOBACTORFOLLOW>();
    case 0xC1: return std::make_unique<JOBACTORWALK>();
    case 0xC2: return std::make_unique<JOBCOMPANION>();
    case 0xC3: return std::make_unique<JOBMURRAYATTACK>();
    case 0xC4: return std::make_unique<JOBBENTLEYATTACK>();
    case 0xC5: return std::make_unique<JOBSLYATTACK>();
    case 0xC6: return std::make_unique<JOBMURRAYRAGE>();
    default: return nullptr;
    }
}
// orig: 0x00142540
void SetActorFollow(ACTOR* pa, LO* target, const glm::vec3& offset)
{
    ClearJobs(pa);
    if (!target) return;
    auto* p = static_cast<JOBCOMPANION*>(PjobPush(pa, 0xC2));
    p->ploFollow = target;
    p->offset = offset;
    p->Enter();
}


