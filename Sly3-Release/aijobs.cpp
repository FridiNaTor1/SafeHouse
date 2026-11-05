// The job classes of the guards' patrols and reactions. See aijobs.h.
// Reference: Sly2-Reference/Sly2-Release/game/m_174898.cpp (0x9C, 0x9D, 0x9E, 0xAA), m_174B28_p3.cpp (0xA7, 0xAA),
// m_171228_p3.cpp (0xA9), m_17D360_p3.cpp (0xBA, 0xC0).

#include "aijobs.h"
#include "task.h"
#include "aichase.h"
#include "actor.h"
#include "navlink.h"
#include "aipursuit.h"
#include "aihit.h"
#include "ainoise.h"

#include "activate.h"
#include "actanim.h"
#include "animfind.h"
#include "brx.h"
#include "asega.h"
#include "clock.h"
#include "detect.h"
#include "guard.h"
#include "lo.h"
#include "nav.h"
#include "so.h"
#include "smrun.h"
#include "stepphys.h"
#include "step.h"
#include "crv.h"
#include "worldaccess.h"
#include "xf.h"
#include "xfmath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

void EnsureScptAlarm(LO* pscpt, int n);  // scpt.cpp: orig 0x1E0020

namespace
{
constexpr float S_JOB_AHEAD = 100.0f;           // D_002BE634: the follower's look-ahead
constexpr float SV_FOLLOW = 500.0f;             // D_002BE630: the steered point's speed
constexpr float AG_FOLLOW_SPEED[3] = {200.0f, -0.5f, 0.0f};  // D_002BE640: by the distance to the path point
constexpr float G_FOLLOW_LO = 0.0f, G_FOLLOW_HI = 150.0f;    // D_002BE650 / D_002BE654
constexpr float K_SPRING_V = 7.5f, K_SPRING_X = 30.0f;      // D_002BE658 / D_002BE65C
constexpr float DZ_AVOID = 250.0f;              // D_002BE660
constexpr float T_PREDICT = 0.5f;               // D_002BE664
constexpr float S_PUSH = 200.0f, K_PUSH = 10000.0f;         // D_002BE668 / D_002BE66C
constexpr float S_TURN = 350.0f, K_TURN = 6000.0f;          // D_002BE670 / D_002BE674
constexpr float AG_TURN[3] = {0.5f, -1.0f, 0.0f};           // D_002BE680
constexpr float S_KEEP = 1.0f;                  // D_002BE690
constexpr float T_STUCK = 1.5f;                 // D_002BE694
constexpr float AG_SPEED[3] = {0.5f, 0.00333333341f, 0.0f};  // D_002BE6A0
constexpr float S_MOVED = 25.0f;                // D_002BE6B0
constexpr float G_CLAMP_LO = 0.0f, G_CLAMP_HI = 1.0f;        // D_002BBB60 / D_002BBB64
constexpr float S_RANGE_A = 25.0f, S_RANGE_B = 60.0f, S_RANGE_C = 100.0f;  // D_002BE624 / 628 / 62C
constexpr float T_WAIT17_LO = 3.0f, T_WAIT17_HI = 5.0f;     // D_002BE618 / 61C
constexpr float T_RESEARCH = 5.0f;              // D_002BE758
constexpr float S_SHORTCUT = 50.0f;             // D_002BE75C
constexpr float S_PATHEND = 10.0f;              // D_002BE7B8
constexpr float S_STEP_3A = 100.0f, S_STEP = 250.0f;  // D_002BE7BC / D_002BE7C0 (class 0x3A, the others)
constexpr float S_STEP_MIN = 5.0f;              // D_002BE7C4
const glm::vec3 AXIS_Z(0.0f, 0.0f, 1.0f);
const glm::vec3 AXIS_X(1.0f, 0.0f, 0.0f);

glm::vec3 PosGuard(const GUARD* pg) { return PosAi(pg->palo); }
float DistXy(const glm::vec3& a, const glm::vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
glm::vec3 Horiz(const glm::vec3& v) { return v - AXIS_Z * glm::dot(v, AXIS_Z); }
glm::vec3 UnitOr(const glm::vec3& v, const glm::vec3& vDefault)
{
    const float l = glm::length(v);
    return l < 1e-4f ? vDefault : v / l;
}
float Poly2(const float* a, float x) { return a[0] + x * (a[1] + x * a[2]); }
float Clamp(float g, float lo, float hi) { return g < lo ? lo : (hi < g ? hi : g); }
float SStart(const CRV* p) { return p->as.empty() ? 0.0f : p->as.front(); }  // 0x1E6620

// the guard's region and its record (orig ALO +0xE8 -> +0xDC (0x1C0D70's record) -> +0x10 the region)
NAVREGION* PregionOfGuard(GUARD* pg) { return PregionAt(PosGuard(pg), false); }

// 0x1E6F60: the path's distance nearest pos
float SNearest(const CRV* pcrv, const glm::vec3& pos)
{
    float s = 0;
    NearestCrvAll(pcrv, pos, nullptr, nullptr, nullptr, &s, &AXIS_Z);
    return s;
}
// 0x1E7380: the same searching from s
float SNearestFrom(const CRV* pcrv, const glm::vec3& pos, float s0, glm::vec3* ppos = nullptr, glm::vec3* pdir = nullptr)
{
    float s = 0;
    NearestCrvS(pcrv, pos, s0, ppos, pdir, nullptr, &s, &AXIS_Z);
    return s;
}
// 0x1E7B38: b - a on the path (a closed one by the shorter way)
float DsCrv(const CRV* pcrv, float a, float b)
{
    float d = b - a;
    if (!pcrv->fClosed) return d;
    const float l = SLengthCrv(pcrv);
    if (l * 0.5f < d) d -= l;
    else if (d < l * -0.5f) d += l;
    return d;
}
}  // namespace

// orig: 0x001C1EC0
glm::vec3 PosGoal(const GOAL& goal, const glm::vec3& posFrom)
{
    if (goal.pcrv)
    {
        glm::vec3 pos(0.0f);
        NearestCrvAll(goal.pcrv, posFrom, &pos, nullptr, nullptr, nullptr, &AXIS_Z);
        return pos;
    }
    if (goal.plo) return PosAi(goal.plo);
    return glm::vec3(goal.pos);
}

// ---- 0x9C: the reactions ----

// orig: 0x001766D8
void JOBREACT::Init() { msg.k = -1; }

// orig: 0x00176708
void JOBREACT::Enter()
{
    AIMSG m;
    m.k = 0;
    React(m);
}

namespace
{
// orig 0x177DC0
bool FSameMsg(const AIMSG& a, const AIMSG& b)
{
    if (a.k != b.k) return false;
    switch (a.k)
    {
    case 9: case 10: case 11: return a.n1C == b.n1C;
    case 2: case 3: case 4: case 5: case 6: case 13: case 14: case 18: return a.plo == b.plo && a.n == b.n;
    default: return true;
    }
}
}  // namespace

// orig: 0x00176750
void JOBREACT::React(const AIMSG& m)
{
    if (FSameMsg(m, msg)) return;
    static const bool s_fDebug = std::getenv("SLY2N_HITDEBUG") != nullptr;
    if (s_fDebug)
        std::printf("react: t %.3f guard %d message %d (was %d)\n", g_clock.t, pguard->palo->oid, m.k, msg.k);
    EndChildJob(&pchild);
    msg = m;
    tMsg = g_clock.t;
    bool fClear38C = false;
    auto job = [&](CID cid) { return PjobPush(pguard, cid); };
    switch (m.k)
    {
    case 5:
    {
        fClear38C = true;
        auto* o = static_cast<JOBSCRIPT*>(job(0xA9));
        o->SetTarget(static_cast<CMDLIST*>(m.plo), 1, nullptr);
        o->Enter();
        break;
    }
    case 1: fClear38C = true; job(0xAA)->Enter(); break;
    case 3:
    {
        auto* o = static_cast<JOBMOVE*>(job(0x9D));
        o->nAnim = 0xB;
        o->ploFace = m.plo;
        o->Enter();
        break;
    }
    case 2:
    {
        fClear38C = true;
        auto* o = static_cast<JOBINVESTIGATE*>(job(0xAB));
        o->SetNoise(dynamic_cast<NOISE*>(m.plo));
        o->Enter();
        break;
    }
    case 4:
    {
        auto* o = static_cast<JOBPURSUE*>(job(0xA5));
        o->ptarget = m.plo;
        o->Enter();
        break;
    }
    case 7:
    {
        auto* o = static_cast<JOBB1*>(job(0xB1));
        o->ploOwner = m.plo;
        o->Enter();
        break;
    }
    case 9: case 10: case 11:
    {
        auto* o = static_cast<JOBHIT*>(job(m.k == 9 ? 0xA1 : m.k == 10 ? 0xA2 : 0xA3));
        o->hit = m.hit;
        if (m.k == 11) fClear38C = true;
        o->Enter();
        break;
    }
    case 14:
    {
        // Carmelita's chase on the message's graph (aichase.cpp)
        auto* o = static_cast<JOBCHASE*>(job(0xB6));
        o->pgraph = dynamic_cast<CHASEGRAPH*>(m.plo);
        o->Enter();
        break;
    }
    case 15:
    {
        auto* o = static_cast<JOBPROX*>(job(0xAE));
        o->plist = pguard->plo108C;
        o->Enter();
        break;
    }
    case 6:
    {
        auto* o = static_cast<JOBB0*>(job(0xB0));
        o->ploHolder = m.plo;  // (+0x3C = the reaction's +0x40, the message's argument)
        o->Enter();
        break;
    }
    case 18:
    {
        auto* o = static_cast<JOBBD*>(job(0xBD));
        o->ploFrom = m.plo;
        o->tLeft = m.g;
        o->Enter();
        break;
    }
    case 13:  // orig 0x176750: the boss's fight (0xB4) at the message's object
    {
        auto* o = static_cast<JOBB4*>(job(0xB4));
        o->ptarget = m.plo;
        o->Enter();
        break;
    }
    case 17:
    {
        auto* o = static_cast<JOBMOVE*>(job(0x9E));
        o->nAnim = 0x1A;
        o->ploFace = m.plo;
        o->Enter();
        tWait = g_clock.t + GRandInRange(T_WAIT17_LO, T_WAIT17_HI);
        break;
    }
    default:
    {
        // TODO(ai): the other reactions' jobs (4 0xA5, 2 0xAB, 9..11 0xA1..0xA3 hits, 8 0xAF, 7 0xB1, 12 0xB2,
        // 13 0xB4, 14 0xB6, 15 0xAE, 16 0xB7, 18 0xBD, 19 0xBF)
        static const CID s_acid[20] = {0, 0, 0xAB, 0, 0xA5, 0, 0xB0, 0xB1, 0xAF, 0xA1, 0xA2, 0xA3, 0xB2, 0xB4, 0xB6, 0xAE, 0xB7, 0, 0xBD, 0xBF};
        if (m.k > 0 && m.k < 20 && s_acid[m.k])
        {
            if (m.k == 2 || m.k == 11 || m.k == 16) fClear38C = true;
            job(s_acid[m.k])->Enter();
        }
        break;
    }
    }
    if (fClear38C) pguard->plo38C = nullptr;  // orig 0x208540(actor, 0)
}

// orig: 0x00177618
void JOBREACT::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    static const bool s_fDebug = std::getenv("SLY2N_NOISEDEBUG") != nullptr;
    if (s_fDebug) std::fprintf(stderr, "react update guard %d msg %d lock %d busy %d\n", pg->palo->oid, msg.k, cLock, pg->fFA8 != 0 || pg->fF94 != 0);
    auto busy = [&]() { return pg->fFA8 != 0 || pg->fF94 != 0; };  // 0x177D20
    auto targetMsg = [&](LO* ptarget, AIMSG* pm) {
        // 0x12 (with its distance) for a target in the +0x3C0 classes, else 3
        if (FTargetClass(pg->acid3C4, ptarget))  // 0x208B38
        {
            pm->k = 0x12;
            pm->plo = ptarget;
            pm->g = GRandInRange(5.0f, 7.0f);  // 0x208BB0
        }
        else
        {
            pm->plo = ptarget;
            pm->k = 3;
        }
    };
    while (!FJobDone(this))
    {
        const AIMSG saved = msg;
        AIMSG m;
        switch (msg.k)
        {
        case 1: case 16: case 17:
        {
            if (busy())
            {
                m.k = 0xC;
                React(m);
                break;
            }
            if (LO* ptarget = PloGuardTarget(pg))
            {
                targetMsg(ptarget, &m);
                React(m);
                break;
            }
            if (LO* pn = PloGuardInterest(pg, nullptr))
            {
                m.plo = pn;
                m.k = 2;
                React(m);
                break;
            }
            if (msg.k == 0x11 && g_clock.t < tWait)
            {
                m.k = 0x11;
                React(m);
                break;
            }
            const bool fF68 = pg->ploF68 && pg->ploF68->fInWorld;  // 0x177D48 (its bit 60)
            m.k = fF68 ? 0x10 : 1;
            React(m);
            break;
        }
        case 0:
            m.k = 1;
            React(m);
            break;
        case 12:
            if (busy()) break;
            m.k = 1;
            React(m);
            break;
        case 8:
            // the sleep over (0x174FB8): a noise at the guard (0x156B98, the fixed template 0) investigated
            if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1)
            {
                m.k = 2;
                m.plo = PnoiseNew(PtmplNoiseFixed(0), PosAi(pg->palo), pg->palo);
                React(m);
            }
            break;
        case 2:
        {
            if (busy()) { m.k = 0xC; React(m); break; }
            if (LO* ptarget = PloGuardTarget(pg))
            {
                targetMsg(ptarget, &m);
                React(m);
                break;
            }
            auto* pj = static_cast<JOBINVESTIGATE*>(pchild);
            LO* pcur = pj ? pj->pnoise : nullptr;
            LO* pnext = PloGuardInterest(pg, pcur);
            if (pnext != pcur)
            {
                if (pj) pj->SetNoise(dynamic_cast<NOISE*>(pnext));
                break;
            }
            if (!pj || pj->st != -1) break;  // (0x172550)
            m.k = 1;
            React(m);
            break;
        }
        case 3:
        {
            if (busy()) { m.k = 0xC; React(m); break; }
            if (!pchild || static_cast<JOBMOVE*>(pchild)->st != 1) break;  // (0x174FB8: the alerted move done)
            m.k = FCidDerivedFrom(pg->palo->Cid(), 0x3B) ? 0xD : 4;
            m.plo = msg.plo;
            React(m);
            break;
        }
        case 4:
        {
            if (busy()) { m.k = 0xC; React(m); break; }
            auto* pj = static_cast<JOBPURSUE*>(pchild);
            if (pj && pj->st == -1)  // (0x175810)
            {
                m.k = 1;
                React(m);
                break;
            }
            LO* ptarget = PloGuardTarget(pg);
            if (ptarget == pg->plo38C) break;
            if (ptarget)
            {
                targetMsg(ptarget, &m);
                React(m);
                break;
            }
            m.k = 1;
            // TODO(ai): a player (0x34) hidden (vt slot 77 == 2) in another guard's hands (0x174898 of its +0xE20): 0x11
            React(m);
            break;
        }
        case 5:
            if (pchild && static_cast<JOBSCRIPT*>(pchild)->FDone()) NextReaction();
            break;
        case 7:
            if (auto* pj = dynamic_cast<JOBB1*>(pchild); pj && pj->st == 1)
            {
                if (pj->stLaunch == -1)
                {
                    SendHit(-50, pj->ploOwner);
                    if (msg.k == 7) NextReaction();
                }
                else if (pj->stLaunch == 3) NextReaction();
            }
            break;
        case 9: case 10:
            if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1) NextReaction();  // (0x174FB8)
            break;
        case 18:
            if (pchild && static_cast<JOBBD*>(pchild)->st == -1) NextReaction();  // (0x17DE40)
            break;
        default:
            break;  // TODO(ai): 2, 3, 4, 6..11, 18, 19 (with their jobs)
        }
        if (FSameMsg(msg, saved)) return;
    }
}

// (0x176BE8, the hits: aihit.cpp)

// ---- 0x9D / 0x9E: moves ----

// orig: 0x00174920
void JOBMOVE::Init()
{
    nAnim = 0;
    st = -1;
}

// orig: 0x00174950
void JOBMOVE::Enter()
{
    AddGuardState(pguard, nAnim);
    if (!pkinde)
    {
        pkinde = PkindeBestOfGuard(pguard);
        if (!pkinde)
        {
            SetMoveState(1);
            return;
        }
    }
    SetMoveState(0);
}

// orig: 0x001749C0
void JOBMOVE::SetMoveState(int n)
{
    if (st == n) return;
    st = n;
    tSt = g_clock.t;
    GUARD* pg = pguard;
    if (n == 0)
    {
        pg->move.pjob = this;  // orig 0x209788
        float g = 0.0f;
        // a walk started right after another (the gait's phase kept: +0xFF0 / +0xFF4)
        if (pkinde->k == 0 && FGuardState(pg, 9) && g_clock.t == pg->tGaitFF4 && pkinde->plo)
            g = PasegdOf(static_cast<ASEG*>(pkinde->plo))->tMax * pg->uGaitFF0;
        PlayKinde(pkinde, pg, pkinde->grf | 1, g);
    }
    else if (n == 1)
    {
        UnlockJob(this);
        if (pkinde)
        {
            if (pkinde->k == 0 && FGuardState(pg, 9) && pkinde->pasega)
            {
                pg->uGaitFF0 = pkinde->pasega->t / PasegdOf(pkinde->pasega->paseg)->tMax;
                pg->tGaitFF4 = g_clock.t;
            }
            StopKinde(pkinde);
            pkinde = nullptr;
            // orig 0x209790: the move cleared, the actor's +0x190.. motion stopped (0x1F5078)
            if (pg->move.pjob == this) pg->move.pjob = nullptr;
            St(pg).vecVel = glm::vec3(0.0f);
        }
        St(pg).nMotion = 0;
    }
}

// orig: 0x00174B28
void JOBMOVE::Exit()
{
    AIJOB::Exit();
    SetMoveState(1);
    RemoveGuardState(pguard, nAnim);
}

// orig: 0x00174B68
void JOBMOVE::Update()
{
    AIJOB::Update();
    int s;
    do
    {
        s = st;
        static const bool s_fDebug = std::getenv("SLY2N_MOVEDEBUG") != nullptr;
        if (s_fDebug)
            std::fprintf(stderr, "move job 0x%X guard %d st %d kinde %p k %d asega %p (%d t %.4f) done %d clock %.4f\n", cid, pguard->palo->oid, st,
                         (void*)pkinde, pkinde ? pkinde->k : -9, pkinde ? (void*)pkinde->pasega : nullptr,
                         pkinde && pkinde->pasega && pkinde->pasega->paseg ? pkinde->pasega->paseg->oid : -1,
                         pkinde && pkinde->pasega ? pkinde->pasega->t : -1.0f, FKindeDone(pkinde), g_clock.t);
        if (s_fDebug)
        {
            const glm::vec3 p = PosLoWorld(pguard->palo), v = VelAloPos(pguard->palo);
            std::fprintf(stderr, "   at %.2f %.2f %.2f vel %.1f %.1f %.1f\n", p.x, p.y, p.z, v.x, v.y, v.z);
        }
        if (s == 0 && FKindeDone(pkinde)) SetMoveState(1);
    } while (s != st);
}

// orig: 0x00174F08 (slot 11)
void JOBMOVE::Steer()
{
    AIJOB::Steer();
    if (st == 0) SteerByKinde(pkinde, pguard);
    else St(pguard).vecVel = glm::vec3(0.0f);
    if (!ploFace) return;
    const glm::vec3 d = PosAi(ploFace) - PosGuard(pguard);
    SetMoveHeading(&pguard->move, std::atan2(d.y, d.x));
}

// orig: 0x00174EA0
int JOBMOVE::State()
{
    if (pkinde && pkinde->k == 2)
    {
        // orig 0x174EA0: full STEP contacts before take-off and after landing; flight keeps only bit 1.
        const auto* pk = static_cast<const K2PLAY*>(pkinde->pvState);
        return pk && (pk->st == 0 || pk->st == 3 || pk->st == 4) ? 7 : 1;
    }
    return AIJOB::State();
}

// orig: 0x00209518: the ASEGA a kind entry plays (an ASEG's own; an SM's transition +0x30; a directional player's +0x24)
ASEGA* PasegaOfKinde(const KINDE* pe)
{
    if (!pe) return nullptr;
    switch (pe->k)
    {
    case 0: return pe->pasega;
    case 1: return pe->psma ? pe->psma->pasega : nullptr;
    case 3: return pe->pvState ? static_cast<K3PLAY*>(pe->pvState)->pasega : nullptr;
    // TODO(ai): kind 2's player +0x38 (K2PLAY::pasega, decomp 0x209518). Returning it makes JOBMOVE::Message take the
    // kind-2 clips' events and breaks job 2:1852's start (jobrecheck: 1852 / 1857 differ at t 2.04): find
    // which event (0x27 lock without its 0x28?) before enabling it
    default: return nullptr;
    }
}

// orig: 0x00174BD0 (message 9: the events of the entry's animation)
int JOBMOVE::Message(int msg, void* pm)
{
    if (msg != 9 || !pkinde) return 0;
    const ASEGMSG9* pev = static_cast<const ASEGMSG9*>(pm);
    if (!pev->pasega || PasegaOfKinde(pkinde) != pev->pasega) return 0;
    GUARD* pg = pguard;
    switch (pev->n)
    {
    case 0x2A: St(pg).nMotion = 0; return 1;
    case 0x27: LockJob(this); return 1;
    case 0x25: pg->move.fHeadingLocked = 1; return 1;  // (0x2097C8)
    case 0x26: pg->move.fHeadingLocked = 0; return 1;  // (0x2097D8)
    case 0x28: UnlockJob(this); return 1;
    case 0x29: St(pg).nMotion = 1; return 1;
    case 0x2F: return 0;  // (the pursuer's rumble 0x187E28(8, 1) of the current player: TODO(media))
    case 0x34: case 0x35: return 0;  // (the hit job's rumble / the camera's shake when the player is hit: TODO)
    case 0x36: SetAloEntryMode(pg->palo, 2); return 0;  // (its body entries hit: the guard's own 0x20000 contact)
    case 0x15E:
    {
        // a push: 0x204E38 (the clip's root velocity, channel kind 1, at the event's time); +0x190 its negation, the
        // velocity it in the heading's frame (+0x1B0 rows, vt slot 35)
        glm::vec4 v(0.0f);
        if (!FAsegaRetracted(pev->pasega))
            if (const ASEGCHN* pchn = PchnFindAseg(pev->pasega->paseg, pg->palo, 1, true))
                if (pchn->ptrkp) pchn->ptrkp->Eval(nullptr, &v, pev->t, pev->pasega->svt);
        St(pg).vecVel = -glm::vec3(v);
        SetAloVelocityTo(pg->palo, St(pg).matHeading * glm::vec3(v));
        if (std::getenv("SLY2N_MOVEDEBUG"))
            std::fprintf(stderr, "move job 0x%X guard %d push at clip t %.4f (event %.4f) clock %.4f v %.1f %.1f %.1f\n", cid, pg->palo->oid,
                         pev->pasega->t, pev->t, g_clock.t, v.x, v.y, v.z);
        return 1;
    }
    default: return 0;
    }
}

// orig: 0x00174FC8
void JOBSTAND::Init()
{
    JOBMOVE::Init();
    nAnim = 2;
}

// orig: 0x00174FF8
void JOBSTAND::Enter()
{
    JOBMOVE::Enter();
    GUARD* pg = pguard;
    if (FGuardState(pg, 1))
    {
        // TODO(ai): 0x20AB68 (the held object dropped), 0x153FF8 (the detector)
        if (pg->ploFCC) ploFace = pg->ploFCC;
    }
    if (pg->palo->apxfRef[0]) SetXfMode(pg->palo, pg->palo->apxfRef[0], 1);
    if (!ploFace && pg->palo->apxfRef[1]) SetXfMode(pg->palo, pg->palo->apxfRef[1], 1);
}

// orig: 0x00175090
void JOBSTAND::Exit()
{
    JOBMOVE::Exit();
    GUARD* pg = pguard;
    if (pg->palo->apxfRef[0]) SetXfMode(pg->palo, pg->palo->apxfRef[0], 2);
    if (ploFace) return;
    if (pg->palo->apxfRef[1]) SetXfMode(pg->palo, pg->palo->apxfRef[1], 2);
}

// orig: 0x001750E0 (0x29 / 0x2A with the rotation record's mode as well)
int JOBSTAND::Message(int msg, void* pm)
{
    if (msg == 9 && pkinde)
    {
        const ASEGMSG9* pev = static_cast<const ASEGMSG9*>(pm);
        if (pev->pasega && PasegaOfKinde(pkinde) == pev->pasega)
        {
            GUARD* pg = pguard;
            if (pev->n == 0x29)
            {
                St(pg).nMotion = 1;
                if (PxfRot(pg->palo)) SetXfMode(pg->palo, PxfRot(pg->palo), 2);
                return 1;
            }
            if (pev->n == 0x2A)
            {
                St(pg).nMotion = 0;
                if (!ploFace && PxfRot(pg->palo)) SetXfMode(pg->palo, PxfRot(pg->palo), 1);
                return 1;
            }
        }
    }
    return JOBMOVE::Message(msg, pm);
}

// ---- 0xA6 common path follower (0xA7 guards, 0xA8 player actors) ----
namespace
{
ALO* PaloFollow(const JOBFOLLOW* p) { return p->pactor ? p->pactor->palo : p->pguard->palo; }
const CRV* PcrvFollowOwn(const JOBFOLLOW* p) { return p->pactor ? &p->pactor->crv294 : &p->pguard->crv294; }
glm::vec3 PosFollow(const JOBFOLLOW* p) { return PosAi(PaloFollow(p)); }
}


// orig: 0x00178E98 (0x177E80)
void JOBFOLLOW::Init()
{
    fFirst = 1;
    s = -1.0f;
    sArrive = S_RANGE_C;
    sNear = S_RANGE_A;
    sFar = S_RANGE_B;
    st = -1;
    sStart = -1.0f;
    nWay = 0;
    kMove = 9;
    tLeft = T_STUCK;
    uSpeed = 1.0f;
}

// orig: 0x00177EF0
void JOBFOLLOW::Enter()
{
    pos = glm::vec4(PosFollow(this), 0.0f);
    const float g = std::max(S_RANGE_C, sArrive);
    sArrive = g;
    sFar = std::min(S_RANGE_B, g);
    sNear = std::min(S_RANGE_A, g * 0.25f);
    SetState(0);
}

// orig: 0x00178CD8 + 0x001791C8
void JOBFOLLOW::SetState(int n)
{
    const int old = st;
    if (n == old) return;
    st = n;
    tSt = g_clock.t;
    if (st < 0) return;
    if (st < 2)
    {
        if (static_cast<unsigned>(old) < 2) return;
        EndChildJob(&pchild);
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9D));
        o->nAnim = kMove;
        if (kMove == 8) o->ploFace = ploArg;
        o->Enter();
        posMoved = glm::vec4(PosFollow(this), 0.0f);
        tLeft = T_STUCK;
    }
    else if (st < 5)
    {
        if (old == 2 || old == 4 || old == 3) return;
        EndChildJob(&pchild);
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9E));
        o->ploFace = ploArg;
        o->Enter();
    }
}

// orig: 0x00178D00
bool JOBFOLLOW::FArrived() const
{
    if (!fOpen) return false;
    return DistXy(PosFollow(this), glm::vec3(posEnd)) <= sArrive;
}

// orig: 0x00178D90
void JOBFOLLOW::Follow(const CRV* p, int fRev)
{
    fReverse = fRev;
    pcrv = p;
    if (0.0f <= sStart)
    {
        s = SNearestFrom(p, PosFollow(this), sStart);
        sStart = -1.0f;
    }
    else s = SNearest(p, PosFollow(this));
    if (p->fClosed)
    {
        fOpen = 0;
        return;
    }
    fOpen = 1;
    sEnd = fReverse ? SStart(p) : SLengthCrv(p);
    glm::vec3 q;
    EvalCrvS(p, sEnd, &q, nullptr);
    posEnd = glm::vec4(q, 0.0f);
}

// orig: 0x001792E8
bool JOBFOLLOW::FStuck() const
{
    if (st >= 2 || st < 0) return false;
    return tLeft < 0.0f;
}

// orig: 0x00177F78
void JOBFOLLOW::Steer177F78()
{
    ALO* palo = PaloFollow(this);
    const glm::vec3 posSelf = PosFollow(this);
    // the actor's region entry's region (ALO +0xE8 -> +0xDC -> +0x10, kept by the ALO's update 0x1F36E0)
    const REGENT* pregentSelf = Pregent(palo);
    NAVREGION* preg = pregentSelf && pregentSelf->nGenNav == NGenNav() ? pregentSelf->preg : nullptr;
    if (!preg) preg = PregionAt(posSelf, false);  // (not placed yet, e.g. a harness's imported world)
    bool fRev = fReverse != 0;
    glm::vec3 posPath, dirPath;
    {
        const float g = fRev ? s - S_JOB_AHEAD : s + S_JOB_AHEAD;
        const float t = SWrapCrv(pcrv, g);
        const float s0 = s;
        s = SNearestFrom(pcrv, posSelf, t, &posPath, &dirPath);  // (0x1E7380 writes +0x34)
        static const int s_oidDebug = std::getenv("SLY2_AI_DEBUG") ? std::atoi(std::getenv("SLY2_AI_DEBUG")) : -1;
        if (palo->oid == s_oidDebug)
            std::fprintf(stderr, "follow %d: s %.3f -> t %.3f -> %.3f (crv %s closed %d n %zu len %.1f) pos %.1f %.1f %.1f path %.1f %.1f\n",
                         palo->oid, s0, t, s, pcrv == PcrvFollowOwn(this) ? "own" : "world", pcrv->fClosed, pcrv->apos.size(),
                         SLengthCrv(pcrv), posSelf.x, posSelf.y, posSelf.z, posPath.x, posPath.y);
    }
    if (st == 1 || st == 4)
    {
        fRev = nWay != 2;
        const float g = fRev ? sOther - S_JOB_AHEAD : sOther + S_JOB_AHEAD;
        const float t = SWrapCrv(pcrvOther, g);
        sOther = SNearestFrom(pcrvOther, posSelf, t, &posPath, &dirPath);
    }
    dirPath = UnitOr(Horiz(dirPath), AXIS_X);
    float gAhead;
    {
        const float d = DistXy(posPath, posSelf);
        sTarget = d;
        const float g = Clamp(Poly2(AG_FOLLOW_SPEED, d), G_FOLLOW_LO, G_FOLLOW_HI);
        gAhead = fRev ? -g : g;
    }
    float sv;
    glm::vec3 posGoal, dirGoal;
    if (FArrived())
    {
        sv = 0.0f;
        dirGoal = AXIS_X;
        posGoal = posSelf;
    }
    else
    {
        sv = SV_FOLLOW;
        if (st == 1 || st == 4)
        {
            posGoal = posPath + dirPath * gAhead;
            dirGoal = dirPath;
        }
        else if (static_cast<unsigned>(st) < 5)
        {
            if (pcrv == PcrvFollowOwn(this))
            {
                const float lo = -s;
                const float hi = SLengthCrv(pcrv) - s;
                const float step = Clamp(gAhead, lo, hi);
                if (gAhead != step) sv = 0.0f;
                posGoal = posPath + dirPath * step;
                dirGoal = dirPath;
            }
            else
            {
                float t;
                if (pcrv->fClosed) t = SWrapCrv(pcrv, s + gAhead);
                else
                {
                    const float lo = -s;
                    const float hi = SLengthCrv(pcrv) - s;
                    const float step = Clamp(gAhead, lo, hi);
                    if (gAhead != step) sv = 0.0f;
                    t = s + step;
                }
                EvalCrvS(pcrv, t, &posGoal, &dirGoal);
            }
        }
    }
    if (fRev) dirGoal = -dirGoal;
    const glm::vec3 velGoal = dirGoal * sv;
    if (fFirst)
    {
        vel = glm::vec4(velGoal, 0.0f);
        pos = glm::vec4(posGoal, 0.0f);
        fFirst = 0;
    }
    // the spring toward the point
    glm::vec3 acc = (velGoal - glm::vec3(vel)) * K_SPRING_V + (posGoal - glm::vec3(pos)) * K_SPRING_X;
    // pushed apart from the other actors on the region (its members +0x118: their predicted positions) and turned
    // around them
    if (!fNoAvoid && preg)
    {
        // (the actor's own entry: its REGENT; the harnesses' imported lists: by object)
        float rSelf = pregentSelf ? pregentSelf->sRadius : 0.0f, mSelf = pregentSelf ? pregentSelf->gMass : 0.0f;
        bool fSelf = pregentSelf != nullptr;
        for (const auto& m : preg->amember)
            if (m.plo == palo && !m.pre) rSelf = m.r, mSelf = m.m, fSelf = true;  // (a harness's imported entry)
        const size_t cmember = preg->amember.size();
        for (size_t imember = 0; imember < cmember && imember < preg->amember.size(); ++imember)
        {
            const NAVREGION::MEMBER& mm = preg->amember[imember];
            const NAVREGION::MEMBER m{mm.plo, mm.pre ? mm.pre->dz : mm.dz, mm.pre ? mm.pre->sRadius : mm.r,
                                     mm.pre ? mm.pre->gMass : mm.m, mm.pre};
            if (m.r == 0.0f || m.m == 0.0f || !m.plo || m.plo == palo || !fSelf) continue;
            glm::vec3 q = PosAi(m.plo);
            q.z += m.dz;
            glm::vec3 v7 = posSelf - q;
            if (DZ_AVOID < std::fabs(v7.z)) continue;
            glm::vec3 ov(0.0f);
            if (FCidDerivedFrom(m.plo->Cid(), 0x30)) ov = VelAloPos(static_cast<ALO*>(m.plo));  // vt slot 33
            const glm::vec3 pred = q + ov * T_PREDICT;
            const glm::vec3 v4 = glm::vec3(pos) - pred;
            v7 = Horiz(v7);
            const glm::vec3 b = Horiz(v4);
            // 0x11EE08: the point of the segment v7 - b nearest the origin
            glm::vec3 c;
            {
                const glm::vec3 d = b - v7;
                float t = glm::dot(-v7, d) / glm::dot(d, d);
                if (!(t >= 0.0f)) t = 0.0f;
                else if (1.0f < t) t = 1.0f;
                c = v7 + d * t;
            }
            const float len = glm::length(c);
            const glm::vec3 cu = UnitOr(c, AXIS_X);
            const float gap = std::max(0.0f, (len - m.r) - rSelf);
            const float ratio = m.m / (m.m + mSelf);
            static const int s_oidDbgM = std::getenv("SLY2_AI_DEBUG") ? std::atoi(std::getenv("SLY2_AI_DEBUG")) : -1;
            if (palo->oid == s_oidDbgM)
                std::fprintf(stderr, "follow %d member %d at %.1f %.1f %.1f r %.0f m %.0f gap %.1f\n", palo->oid, m.plo->oid, q.x, q.y, q.z,
                             m.r, m.m, gap);
            if (gap < S_PUSH)
            {
                const float a = gap / S_PUSH;
                acc += cu * ((ratio * (1.0f - a)) * K_PUSH);
            }
            if (!(gap < S_TURN)) continue;
            float f5 = 1.0f - gap / S_TURN;
            const glm::vec3 dir = UnitOr(v7, AXIS_X);
            const float dd = glm::dot(dir, StepPhysOf(palo).matHeading[0]);
            f5 *= Clamp(Poly2(AG_TURN, dd), G_CLAMP_LO, G_CLAMP_HI);
            if (f5 == 0.0f) continue;
            const glm::vec3 side = glm::cross(AXIS_Z, dirGoal);
            if (pcrv != PcrvFollowOwn(this) && glm::dot(cu, side) < 0.0f) f5 = -f5;
            acc += glm::cross(cu, AXIS_Z) * ((ratio * f5) * K_TURN);
        }
    }
    const float dt = g_clock.dt;
    acc = Horiz(acc);
    const glm::vec3 v0(vel), p0(pos);
    glm::vec3 v1 = v0 + acc * dt;
    glm::vec3 p1 = p0 + v0 * dt + acc * (dt * (dt * 0.5f));
    if (!fNoAvoid && preg)
    {
        glm::vec3 q;
        if (FKeepOnNav(preg, p1, &q))
        {
            const glm::vec3 c = Horiz(q - p1);
            const float l = glm::length(c);
            const glm::vec3 n = UnitOr(c, AXIS_X);
            if (S_KEEP < l)
            {
                const float d = glm::dot(v1, n);
                p1 = q;
                if (d < 0.0f) v1 = v1 - n * d;
            }
        }
    }
    pos = glm::vec4(p1, 0.0f);
    vel = glm::vec4(v1, 0.0f);
}

// orig: 0x001789F8
void JOBFOLLOW::Update1789F8()
{
    AIJOB::Update();
    Steer177F78();
    const glm::vec3 posSelf = PosFollow(this);
    int s0;
    do
    {
        s0 = st;
        switch (s0)
        {
        case 0: case 2: case 3:
        {
            if (!fNoAvoid)
            {
                NAVREGION* preg = PregionAt(PosFollow(this), false);
                NAVPOLY* ppoly = nullptr;
                nWay = NNavWay(posSelf, preg, pcrv, fReverse == 0 ? 1 : 0, &ppoly, &g60, s);
                pcrvOther = ppoly ? ppoly->pcrv : nullptr;
                if (static const char* s_sz = std::getenv("SLY2N_PATHSTEPPRINT"); s_sz && ppoly && PaloFollow(this)->oid == std::atoi(s_sz))
                    std::fprintf(stderr, "follow %d t %.3f st %d way %d poly %d (cid 0x%X kind %d) at %.0f %.0f %.0f\n", PaloFollow(this)->oid, g_clock.t, st, nWay,
                                 ppoly->plo ? ppoly->plo->oid : -1, ppoly->plo ? ppoly->plo->Cid() : 0, ppoly->kind, posSelf.x, posSelf.y, posSelf.z),
                    ppoly->plo && ppoly->plo->paloParent && std::fprintf(stderr, "  owner %d cid 0x%X at %.0f %.0f %.0f box %.0f %.0f .. %.0f %.0f\n", ppoly->plo->paloParent->oid,
                                 ppoly->plo->paloParent->Cid(), PosLoWorld(ppoly->plo->paloParent).x, PosLoWorld(ppoly->plo->paloParent).y, PosLoWorld(ppoly->plo->paloParent).z,
                                 ppoly->posMin.x, ppoly->posMin.y, ppoly->posMax.x, ppoly->posMax.y);
                switch (nWay)
                {
                case 3: case 4: SetState(3); break;
                case 1: case 2:
                {
                    const float sNow = SNearest(pcrvOther, posSelf);
                    const float d = DsCrv(pcrvOther, sNow, g60);
                    if (80.0f < std::fabs(d))
                    {
                        sOther = sNow;
                        SetState(1);
                    }
                    else nWay = 0;
                    break;
                }
                default: break;
                }
            }
            if (nWay == 0)
            {
                const float r = st != 0 ? sFar : sNear;
                const float d = DistXy(posSelf, glm::vec3(pos));
                SetState(r < d ? 0 : 2);
            }
            break;
        }
        case 1: case 4:
        {
            // (the polygon's object alive (bit 60), its +0x1D8 owner none)
            if (!pcrvOther) SetState(0);
            else if (std::fabs(DsCrv(pcrvOther, sOther, g60)) < 80.0f) SetState(0);
            else
            {
                const float r = st != 1 ? sFar : sNear;
                const float d = DistXy(posSelf, glm::vec3(pos));
                SetState(r < d ? 1 : 4);
            }
            break;
        }
        default: break;
        }
    } while (st != s0);
}

// orig: 0x00178EE0
void JOBFOLLOW::Update()
{
    Update1789F8();
    if (static const char* s_sz = std::getenv("SLY2N_FOLLOWPRINT"); s_sz && pguard && pguard->palo && pguard->palo->oid == std::atoi(s_sz))
        std::printf("follow %d t %.3f s %.2f st %d arrive %.1f near %.1f far %.1f target %.2f pos %.2f %.2f vel %.2f %.2f end %.2f %.2f open %d\n",
                    pguard->palo->oid, g_clock.t, s, st, sArrive, sNear, sFar, sTarget, pos.x, pos.y, vel.x, vel.y, posEnd.x, posEnd.y, fOpen);
    if (static_cast<unsigned>(st) >= 2) return;
    const glm::vec3 posSelf = PosGuard(pguard);
    const glm::vec3 d = Horiz(glm::vec3(pos) - posSelf);
    const float dist = std::sqrt(d.x * d.x + d.y * d.y);
    const float sp20 = Clamp(Poly2(AG_SPEED, dist), G_CLAMP_LO, G_CLAMP_HI);
    JOBMOVE* pmove = pchild && FCidDerivedFrom(pchild->cid, 0x9D) ? static_cast<JOBMOVE*>(pchild) : nullptr;
    KINDE* pe = pmove ? pmove->pkinde : nullptr;
    if (pe && pe->k == 0)
    {
        if (pe->pasega) SetAsegaSpeed(pe->pasega, uSpeed * sp20);
        SetMoveHeading(&pguard->move, std::atan2(d.y, d.x));
    }
    else if (pe && pe->k == 3)
    {
        // orig 0x178EE0: the path offset in the actual object's frame; the first authored clip's mean
        // horizontal speed scales it before the directional player selects a clip (0x20CB28).
        auto* pk = static_cast<K3PLAY*>(pe->pvState);
        if (pk && pe->pk3)
        {
            const XF* px = pguard->palo->apxfRef[2];
            const glm::mat3 m = px ? glm::mat3(px->matWorld) : glm::mat3(1.0f);
            const glm::vec3 local = glm::transpose(m) * d;
            const glm::vec4 v = pe->pk3->ae[0].vel;
            const float speed = (uSpeed * sp20) * std::sqrt(v.x * v.x + v.y * v.y);
            const float len = std::sqrt(glm::dot(local, local));
            const glm::vec3 dir = len < 1e-4f ? glm::vec3(0.0f) : local * (1.0f / len);
            SelectK3(pk, dir * speed);
            if (pk->pasega) SetAsegaSpeed(pk->pasega, sp20);
        }
    }
    const glm::vec3 e = posSelf - glm::vec3(posMoved);
    if (S_MOVED < glm::length(e))
    {
        tLeft = T_STUCK;
        posMoved = glm::vec4(posSelf, 0.0f);
    }
    else tLeft = tLeft - g_clock.dt;
}

// ---- 0xA9: a command list ----

// orig: 0x00171228
void JOBSCRIPT::Init() { AddGuardState(pguard, 0x21); }

// orig: 0x00171398
void JOBSCRIPT::Exit()
{
    AIJOB::Exit();
    UnlockJob(this);
    SetTarget(nullptr, 0, nullptr);
    RemoveGuardState(pguard, 0x21);
}

// orig: 0x00171258
void JOBSCRIPT::SetTarget(CMDLIST* pl, int fFrom, const SAVEDSCRIPT* ps)
{
    if (pl == pcmdlist) return;
    if (pcmdlist)
    {
        EndChildJob(&pchild);
        Script(3);
        pframe.reset();  // 0x256EE0 / 0x2580A8, after the old stop handler
        psidebag.reset();
        ploNear = nullptr;
        pcmd = nullptr;
        fRunning = 0;
        fFromReact = 0;
    }
    pcmdlist = pl;
    if (!pl) return;
    pframe = pl->pframe;  // 0x256EE0, before the new start handler
    Script(2);
    fFromReact = fFrom;
    if (ps && ps->pcmd)
    {
        pcmd = ps->pcmd;
        psaved = ps;
        ploNear = ps->plo;
    }
    else
    {
        ploNear = pcmdlist;
        pcmd = pcmdlist->PcmdFirst();  // 0x171338: the list after the start handler
    }
    if (std::getenv("SLY2N_SCPTPRINT") && pcmdlist)
    {
        std::fprintf(stderr, "ai: list %d of guard %d:", pcmdlist->oid, pguard && pguard->palo ? pguard->palo->oid : -1);
        for (const auto& pc : pcmdlist->acmd) std::fprintf(stderr, " [op %d oid %d]", pc->op, pc->oid);
        std::fprintf(stderr, "\n");
    }
    if (pcmd) Execute(pcmd);
    psaved = nullptr;
}

// orig: 0x001713E8
void JOBSCRIPT::Update()
{
    AIJOB::Update();
    while (pcmd)
    {
        if (FRunning()) return;
        CMD* pnext = pcmdlist ? pcmdlist->PcmdNext(pcmd) : nullptr;
        pcmd = pnext;
        if (!pnext) return;
        Execute(pnext);
    }
}

void JOBSCRIPT::SetState(int) {}

// orig: 0x00171C98 (the command cursor; 0x1711F0 finds the first matching kind-9 marker)
void JOBSCRIPT::SeekMarker(OID oid)
{
    if (!pcmdlist) return;
    if (fRunning)
    {
        EndChildJob(&pchild);
        fRunning = 0;
    }
    pcmd = nullptr;
    for (const auto& p : pcmdlist->acmd)
        if (p->op == 9 && p->oid == oid)
        {
            pcmd = p.get();
            break;
        }
}

// orig: 0x001715B8
LO* JOBSCRIPT::PloNear(OID oid)
{
    // 0x15E938(G, near ? 4 : 5, oid, near)
    return ploNear ? PloFindSwNearest(ploNear, oid) : PloFindSwAny(oid);
}

// orig: 0x00171468
bool JOBSCRIPT::FRunning()
{
    if (!fRunning) return false;
    switch (pcmd->op)
    {
    case 6:
        if (pguard->kMove == -1)
        {
            if (pchild && static_cast<JOBPATHSTEP*>(pchild)->FDone()) fRunning = 0;
        }
        // TODO(ai): kinds 0 / 1 (0x17CE30 / 0x17D188)
        break;
    case 7:
        if (pchild && static_cast<JOBGOTO*>(pchild)->NArrived())
        {
            fRunning = 0;
            SetMoveHeading(&pguard->move, radFace);
        }
        break;
    case 5:
        if (pcmd->g8 < g_clock.t - tCmd) fRunning = 0;
        break;
    case 13: case 14:
        if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1) fRunning = 0;  // 0x174FB8
        break;
    default: break;
    }
    if (!fRunning) EndChildJob(&pchild);
    return fRunning != 0;
}

namespace
{
// orig: 0x001710F8 (0x171AF8 with the guard's position): an object's place and heading for a command: none, the
// guard's place; a path (cid 0xA), its point nearest the guard and its direction there; else its position and its
// matrix's x row
void GetCmdAim(GUARD* pg, LO* po, glm::vec3* ppos, float* prad)
{
    const glm::vec3 posG = PosAi(pg->palo);
    glm::vec3 pos = posG;
    float rad = 0.0f;
    if (po && FIsLoDerivedFrom(po, 0xA))
    {
        glm::vec3 dir(1.0f, 0.0f, 0.0f);
        if (const CRV* pcrv = PcrvFromLo(po)) NearestCrvAll(pcrv, posG, &pos, &dir, nullptr, nullptr);
        rad = std::atan2(dir.y, dir.x);
    }
    else if (po)
    {
        pos = PosAi(po);
        if (FIsLoDerivedFrom(po, 0x30))
        {
            const glm::mat4 m = MatWorldOfAlo(static_cast<ALO*>(po));
            rad = std::atan2(m[0][1], m[0][0]);
        }
        else if (const LOX* plox = dynamic_cast<const LOX*>(po))
        {
            const glm::mat4 m = plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
            rad = std::atan2(m[0][1], m[0][0]);
        }
    }
    if (ppos) *ppos = pos;
    if (prad) *prad = rad;
}

// orig: 0x00171B60: the guard placed there facing rad, still (0x20B210: its kind's mover reset (TODO(ai): kinds 1 /
// 3's 0x204F78); slots 29 / 30 position and rotation, 35 / 36 the velocities zero)
void PlaceGuard(GUARD* pg, const glm::vec3& pos, float rad)
{
    ALO* palo = pg->palo;
    if (STEP* pstep = PstepFromLo(palo)) SetStepPos(pstep, pos - glm::vec3(pstep->vecBase));
    else SetAloPos(palo, pos);
    SetStepPhysHeading(palo, rad);  // 0x204660
    if (STEP* pstep = PstepFromLo(palo)) SetStepRotation(pstep, St(pg).matHeading);
    else SetAloRotation(palo, St(pg).matHeading);
    SetAloVelocityTo(palo, glm::vec3(0.0f));
    SetAloAngVelocityTo(palo, glm::vec3(0.0f));
}
}  // namespace

// ---- 0xAE: class 0x3C's command list near Sly ----

namespace
{
// orig 0x2E1E40: the game's Sly (the first loaded class-0x35 object)
LO* PloSly()
{
    if (!g_psw) return nullptr;
    for (LO* p : g_psw->mpcidaplo[0x35])
        if (!p->ploCloneOf) return p;
    return nullptr;
}
}  // namespace

// orig: 0x001BA560
void JOBPROX::Init()
{
    AIJOB::Init();
    st = -1;
    AddGuardState(pguard, 0x25);
}
// orig: 0x001BA598
void JOBPROX::Enter()
{
    AIJOB::Enter();
    SetProxState(0);
}
// orig: 0x001BA5C8
void JOBPROX::Exit()
{
    SetProxState(-1);
    AIJOB::Exit();
    RemoveGuardState(pguard, 0x25);
}
// orig: 0x001BA828
void JOBPROX::SetProxState(int n)
{
    if (n == st) return;
    GUARD* pg = pguard;
    if (st == 0 && pchild) static_cast<JOBSCRIPT*>(pchild)->Save(&pg->saved1090);  // 0x171C18
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    if (n == 0)
    {
        auto* o = static_cast<JOBSCRIPT*>(PjobPush(pg, 0xA9));
        o->SetTarget(dynamic_cast<CMDLIST*>(plist), 0, &pg->saved1090);
        o->Enter();
    }
    else if (n == 1)
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->ploFace = PloSly();
        o->Enter();
    }
    else if (n == 2)
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
        o->pkinde = pg->kinde1060.k == -1 ? nullptr : &pg->kinde1060;
        o->Enter();
        BroadcastTaskEvent(g_psw, 0x33, pg->palo);  // 0x1619A0(the tasks, 0x33, the guard): the tasks' event of the alarm
    }
}
// orig: 0x001BA608: the states by Sly's place in the detector's volumes; while running the list with the state 9,
// the follow speed (a + d (b + d c) clamped, d the distance to Sly) to the path follower (0xA7 +0xC4) or 0xB8
void JOBPROX::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    LO* psly = PloSly();
    while (!FJobDone(this))
    {
        const int prior = st;
        if (st == 1)
        {
            if (pg->pdet && psly && FPointInShape(pg->pdet->ploVolume, PosAi(psly))) SetProxState(0);
            else if (pg->g1088 < g_clock.t - tSt) SetProxState(2);
        }
        else if (st == 0)
        {
            if (!(pg->pdet && psly && FPointInShape(pg->pdet->ploVolume2, PosAi(psly)))) SetProxState(1);
        }
        if (prior == st) break;
    }
    if (st == 0 && FGuardState(pg, 9) && psly)
    {
        const float d = glm::length(PosAi(pg->palo) - PosAi(psly));
        float g = pg->g1070 + d * (pg->g1074 + d * pg->g1078);
        g = g < pg->g1080 ? pg->g1080 : (pg->g1084 < g ? pg->g1084 : g);
        if (auto* pf = static_cast<JOBFOLLOW*>(PjobFind(pg, 0xA7))) pf->uSpeed = g;
        // TODO(ai): 0xB8's +0x38
    }
}

// ---- 0xBE: across a link between regions; 0xBF: to a way-point ----

namespace
{
// orig 0x001C1FA0: the guard's floor probe (+0x218) within r (xy) of the record's point seen from it (always for
// class 0x40)
bool FNearRecord(const GOAL& g, GUARD* pg, float r)
{
    if (FIsLoDerivedFrom(pg->palo, 0x40)) return true;
    const glm::vec3 a = PosGuardProbe(pg);
    const glm::vec3 b = PosGoal(g, a);
    return glm::length(glm::vec2(a - b)) <= r;
}

// orig 0x0017D608: the point moved up to the first surface the line from it to 5000 above (D_002BE750 0 / D_002BE754
// 5000) meets in the world's geometry (0x1B44D8)
bool FSnapPosToSurfaceAbove(glm::vec3* ppos)
{
    std::vector<SOE*> apsoe;
    if (g_psw)
        for (LO* plo : g_psw->mpcidaplo[0x31])
            for (SOE& soe : static_cast<ALO*>(plo)->asoe)
                if (!soe.f48) apsoe.push_back(&soe);
    LSG lsg;
    if (!PsoeHitTestLineObjects(0, *ppos + AXIS_Z * 0.0f, *ppos + AXIS_Z * 5000.0f, apsoe, &lsg)) return false;
    *ppos = lsg.pos1;
    return true;
}
}  // namespace

// orig: 0x0017E3A0
void JOBLINK::Init()
{
    AIJOB::Init();
    st = -1;
    AddGuardState(pguard, 0x2C);  // 0x209890
}

// orig: 0x0017E3D8: locked, then the first record after the begin (the guard is at the begin)
void JOBLINK::Enter()
{
    AIJOB::Enter();
    LockJob(this);
    NextRecord();
}

// orig: 0x0017E410
void JOBLINK::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    UnlockJob(this);
    RemoveGuardState(pguard, 0x2C);  // 0x209970
}

// orig: 0x0017E580: the next record (the link's +0x50 count), else done (-1, unlocked)
void JOBLINK::NextRecord()
{
    ++irec;
    if (plink && irec < static_cast<int>(plink->agoal.size()))
    {
        SetState(-1);
        SetState(0);
        return;
    }
    SetState(-1);
    UnlockJob(this);
}

// orig: 0x0017E6B0
void JOBLINK::SetState(int n)
{
    if (n == st) return;
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    GUARD* pg = pguard;
    if (std::getenv("SLY2N_GOTODEBUG"))
        std::printf("joblink: t %.3f guard %d link %d kind %d rec %d state %d at %.0f %.0f %.0f\n", g_clock.t, pg->palo->oid,
                    plink ? plink->plo->oid : -1, plink ? plink->kind : -1, irec, n, PosAi(pg->palo).x, PosAi(pg->palo).y, PosAi(pg->palo).z);
    if (!plink || irec < 0 || irec >= static_cast<int>(plink->agoal.size())) return;
    const GOAL& rec = plink->agoal[static_cast<size_t>(irec)];
    switch (n)
    {
    case 0:
        if (plink->kind == 0)
        {
            // walked: the guard's own path (+0x294) straight to the record's point, a deadline of its xy length / 75
            // (at least 2 s, D_002BE7B0 / D_002BE7B4); the follower's +0x40 1, its arrival +0x64 10
            auto* o = static_cast<JOBFOLLOW*>(PjobPush(pg, 0xA7));
            CRV* b = &pg->crv294;
            const glm::vec3 pos = PosAi(pg->palo);
            const glm::vec3 q = PosGoal(rec, pos);
            const float len = glm::length(glm::vec2(pos - q));
            const float g = len / 75.0f;
            tEnd = g_clock.t + (g < 2.0f ? 2.0f : g);
            b->crvk = 0;
            b->fClosed = 0;
            b->apos = {pos, q};
            b->ag = {0.0f, 1.0f};
            RecalcCrv(b, glm::mat4(1.0f));  // 0x1E6180
            o->fNoAvoid = 1;
            o->sArrive = 10.0f;
            o->Follow(b, 0);
            o->Enter();
        }
        else if (plink->kind == 1)
        {
            // orig 0x17E628: jumped: the use job to the record's point (+0x68 1)
            auto* o = static_cast<JOBUSE*>(PjobPush(pg, 0xAC));
            o->nExtra = 1;
            o->pos = PosGoal(rec, PosAi(pg->palo));
            o->Enter();
        }
        break;
    case 1:
    {
        auto* o = static_cast<JOBNAVPOINT*>(PjobPush(pg, 0xBF));
        o->goal = rec;
        o->Enter();
        break;
    }
    default: break;
    }
}

// orig: 0x0017E458
void JOBLINK::Update()
{
    AIJOB::Update();
    if (st == -1) return;  // 0x17E570
    GUARD* pg = pguard;
    if (!plink) return;
    switch (st)
    {
    case 1:
        if (pchild && static_cast<JOBNAVPOINT*>(pchild)->FDone()) NextRecord();
        break;
    case 0:
        if (plink->kind == 1)
        {
            if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1)  // 0x174FB8
            {
                if (FNearRecord(plink->agoal[static_cast<size_t>(irec)], pg, 25.0f)) NextRecord();  // 0x17E5F0
                else SetState(1);
            }
        }
        else if (plink->kind == 0)
        {
            if (tEnd < g_clock.t) SetState(1);
            else
            {
                // the guard's area region (ALO +0xE8 -> +0xDC -> +0x10, kept by 0x1F36E0 / 0x1F37C8 from 0x1C0D70 at
                // its position) is the link's end region and the follower arrived (0x178D00)
                NAVREGION* preg = PregionAt(PosAi(pg->palo), true);
                if (preg == plink->pregionEnd && pchild && static_cast<JOBFOLLOW*>(pchild)->FArrived()) NextRecord();
            }
        }
        break;
    default: break;
    }
}

// orig: 0x0017E888
void JOBNAVPOINT::Init()
{
    AIJOB::Init();
    AddGuardState(pguard, 0x2D);  // 0x209890
}

// orig: 0x0017E8B8
void JOBNAVPOINT::Enter()
{
    AIJOB::Enter();
    Start();
}

// orig: 0x0017E8E8
void JOBNAVPOINT::Exit()
{
    AIJOB::Exit();
    RemoveGuardState(pguard, 0x2D);  // 0x209970
}

// orig: 0x0017E918
void JOBNAVPOINT::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    if (pg->grfFE8 & 1)
    {
        if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1 && !FNearRecord(goal, pg, 25.0f)) Start();
    }
    else if (!FNearRecord(goal, pg, 25.0f)) Start();
}

// orig: 0x0017E9B0: the point seen from the guard dropped on what is under it (0x173610 from 150 above to 250 below,
// any slope), else snapped up (0x17D608); walked to with the use job (+0x68 1) when the guard can (+0xFE8 bit 1),
// else the guard put there (its mover reset 0x20B210, vt slot 29 with its base +0x240 off, still: slots 35 / 36)
void JOBNAVPOINT::Start()
{
    EndChildJob(&pchild);
    GUARD* pg = pguard;
    glm::vec3 u = PosGoal(goal, PosAi(pg->palo));
    const glm::vec3 u0 = u;
    if (!FDropOnGround(&u, 150.0f, -250.0f, true)) FSnapPosToSurfaceAbove(&u);
    if (std::getenv("SLY2N_GOTODEBUG"))
        std::printf("navpoint: t %.3f guard %d from %.0f %.0f %.0f to %.0f %.0f %.0f (dropped %.0f %.0f %.0f) fe8 %x\n", g_clock.t, pg->palo->oid,
                    PosAi(pg->palo).x, PosAi(pg->palo).y, PosAi(pg->palo).z, u0.x, u0.y, u0.z, u.x, u.y, u.z, pg->grfFE8);
    if (pg->grfFE8 & 1)
    {
        auto* o = static_cast<JOBUSE*>(PjobPush(pg, 0xAC));
        o->pos = u;
        o->nExtra = 1;
        o->Enter();
    }
    else
    {
        DetachGuardMove(pg);  // 0x20B210
        SetAloPos(pg->palo, u - glm::vec3(St(pg).vecBase));
        SetAloVelocityTo(pg->palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(pg->palo, glm::vec3(0.0f));
    }
}

// orig: 0x0017EAE8: the use job done (when walked) and within 25
bool JOBNAVPOINT::FDone() const
{
    if (pguard->grfFE8 & 1)
        if (!pchild || static_cast<JOBMOVE*>(pchild)->st != 1) return false;
    return FNearRecord(goal, pguard, 25.0f);
}

// ---- 0xAF: asleep ----

// orig: 0x0017A4B0
void JOBSLEEP::Init()
{
    JOBMOVE::Init();
    nAnim = 0x10;
    stSleep = -1;
}
// orig: 0x0017A4E8: the detector forgets (0x153FF8: every entry back to state 0 at now) and the sleep's noise held
// at the guard (0x156C80(SW, 1, ...) -> 0x156BE0: SW +0x4024's template (707) into +0x35C, held (vt slot 1))
void JOBSLEEP::Enter()
{
    JOBMOVE::Enter();
    SetSleepState(0);
    GUARD* pg = pguard;
    if (pg->pdet)
        for (DETENTRY& e : pg->pdet->ae)
        {
            e.st = 0;
            e.t8 = g_clock.t;
        }
    if (pnoise && FNoiseLive(pnoise)) RemoveNoise(pnoise);
    pnoise = PnoiseNew(PtmplNoiseFixed(1), PosAi(pg->palo), pg->palo);
}
// orig: 0x0017A558: the noise let go (0x156C80(SW, -1): 0x156780 and its vt slot 2)
void JOBSLEEP::Exit()
{
    JOBMOVE::Exit();
    SetSleepState(-1);
    if (pnoise && FNoiseLive(pnoise)) RemoveNoise(pnoise);
    pnoise = nullptr;
}
// orig: 0x0017A5B0: 0 asleep (the entry's state machine to 0x44) for GRandInRange(+0xE7C, +0xE80); 1 waking (0x43)
void JOBSLEEP::SetSleepState(int n)
{
    if (n == stSleep) return;
    stSleep = n;
    gSt = static_cast<float>(n);
    GUARD* pg = pguard;
    if (n == 0)
    {
        if (pkinde && pkinde->k == 1 && pkinde->psma) SetSmaGoal(pkinde->psma, 0x44);
        tWake = g_clock.t + GRandInRange(pg->gE7C, pg->gE80);
    }
    else if (n == 1)
    {
        if (pkinde && pkinde->k == 1 && pkinde->psma) SetSmaGoal(pkinde->psma, 0x43);
    }
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("sleep: t %.3f guard %d state %d (wake at %.3f) entry kind %d sma %p\n", g_clock.t, pg->palo->oid, n, tWake,
                    pkinde ? pkinde->k : -9, pkinde ? (void*)pkinde->psma : nullptr);
    if (!pkinde && n == 0 && std::getenv("SLY2N_SLEEPDBG"))  // (debug: the guard's entries with the state 0x10 bit)
        for (const KINDE& e : pg->akinde)
            if (e.mask & GrfStateBit(0x10))
                std::printf("sleepdbg guard %d entry k %d oid %d mask 0x%llX (state 0x%llX)\n", pg->palo->oid, e.k, e.oid,
                            static_cast<unsigned long long>(e.mask), static_cast<unsigned long long>(pg->grfState));
}
// orig: 0x0017A670: the noise kept at the guard (+0x35C's +0x20); held, it does not expire (natively: its time
// renewed while held, the reference count's effect)
void JOBSLEEP::Update()
{
    JOBMOVE::Update();
    int s;
    do
    {
        s = stSleep;
        if (s == 0 && tWake < g_clock.t) SetSleepState(1);
    } while (stSleep != s);
    if (std::getenv("SLY2N_SLEEPDBG") && pkinde && pkinde->psma)
    {
        SMA* p = pkinde->psma;
        std::printf("sleepdbg t %.3f guard %d cur %d goal %d next %d asega %p aseg %d t %.3f svt %.2f\n", g_clock.t, pguard->palo->oid, p->ismsCur, p->ismsGoal,
                    p->ismsNext, (void*)p->pasega, p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -1, p->pasega ? p->pasega->t : -1.0f,
                    p->pasega ? p->pasega->svt : 0.0f);
    }
    if (pnoise && FNoiseLive(pnoise))
    {
        pnoise->pos20 = PosAi(pguard->palo);
        pnoise->t = g_clock.t;
    }
}

// ---- 0xAC: the use of an object ----

// orig: 0x00179F70
void JOBUSE::Init()
{
    JOBMOVE::Init();
    nAnim = 15;
}

// orig: 0x00173610: pos dropped onto what is under it (the line from pos + gHigh up to pos + gLow up through the world's
// still solid entries (0x20FAA0 with 0x20FA50, 0x14CC58)): false when nothing is hit (or, unless fAny, it is steeper
// than 0.7)
bool FDropOnGround(glm::vec3* ppos, float gHigh, float gLow, bool fAny)
{
    const glm::vec3 a = *ppos + AXIS_Z * gHigh, b = *ppos + AXIS_Z * gLow;
    const glm::vec3 mn = glm::min(a, b), mx = glm::max(a, b);
    std::vector<SOE*> apsoe;
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
        {
            if (psoe->f48 || !psoe->palo) continue;
            if (!FAloStill(psoe->palo)) continue;  // (0x20FA50: vt slot 62)
            if (apsoe.size() < 0x100) apsoe.push_back(psoe);
        }
    }
    if (apsoe.empty()) return false;
    LSG lsg;
    if (!PsoeHitTestLineObjects(1, a, b, apsoe, &lsg) || lsg.t0 == 0.0f) return false;  // (grf 1; y.f40 == 0)
    if (!fAny && lsg.plane0.z < 0.7f) return false;
    *ppos = lsg.pos0;
    return true;
}
// (for scpt.cpp: SCPT 0x2A's target point, 0x173610)
bool FDropPointOnGround(glm::vec3* ppos, float gHigh, float gLow) { return FDropOnGround(ppos, gHigh, gLow, false); }

// orig: 0x00179FA0: the object's place and heading (a path: its point nearest the guard and its direction), by its
// class the state bits for the move and its orientation; the move entered with those bits; the guard turned to the
// place; the kind-2 driver told (TODO(ai): 0x20C6F8 with the kind-2 movers); the job locked
void JOBUSE::Enter()
{
    GUARD* pg = pguard;
    const uint64_t mask = GrfStateBit(0x1B) | GrfStateBit(0x27) | GrfStateBit(0x28) | GrfStateBit(0x29);
    const uint64_t grfOld = pg->grfState & mask;
    pg->grfState &= ~mask;
    uint64_t grfActive = 0;
    if (std::getenv("SLY2N_JOBPRINT") && ploTarget)
        std::fprintf(stderr, "jobuse: guard %d t %.3f target %d cid 0x%X state %d\n", pg->palo ? pg->palo->oid : -1, g_clock.t,
                     ploTarget->oid, ploTarget->Cid(), ploTarget->nState);
    if (ploTarget)
    {
        float rad = 0.0f;
        if (FIsLoDerivedFrom(ploTarget, 0xA))
        {
            glm::vec3 dir(1.0f, 0.0f, 0.0f);
            if (const CRV* pcrv = PcrvFromLo(ploTarget)) NearestCrvAll(pcrv, PosAi(pg->palo), &pos, &dir, nullptr, nullptr);
            rad = std::atan2(dir.y, dir.x);
        }
        else
        {
            pos = PosAi(ploTarget);
            if (FIsLoDerivedFrom(ploTarget, 0x30))
            {
                const glm::mat4 m = MatWorldOfAlo(static_cast<ALO*>(ploTarget));
                rad = std::atan2(m[0][1], m[0][0]);
            }
        }
        if (FIsLoDerivedFrom(ploTarget, 13))
        {
            // a chair: facing away from it unless reversed
            grfActive |= GrfStateBit(0x1B);
            if (!fReverse) rad = RadNormalize(rad + 3.1415927f);
            fBasis = 1;
            mat = glm::mat3(glm::rotate(glm::mat4(1.0f), rad, AXIS_Z));
        }
        else if (FIsLoDerivedFrom(ploTarget, 0x27))
            grfActive |= GrfStateBit(0x29);
        else if (FIsLoDerivedFrom(ploTarget, 11) || (FIsLoDerivedFrom(ploTarget, 0x13) && ploTarget->nState == 3))
        {
            // TODO(ai): a ladder's place (0x1C3500) and a bar's orientation (0x11B830), then 0x17A468 (the guard's
            // own offset)
            grfActive |= FIsLoDerivedFrom(ploTarget, 11) ? GrfStateBit(0x27) : GrfStateBit(0x28);
            static bool s_fWarned = false;
            if (!s_fWarned) std::fprintf(stderr, "ai: job 0xAC on a ladder / bar (0x1C3500 / 0x17A468) not ported\n");
            s_fWarned = true;
        }
        else
        {
            FDropOnGround(&pos, 100.0f, -150.0f, true);  // 0x173610 (D_002BE6B8)
            ploTarget = nullptr;
        }
    }
    pg->grfState |= grfActive;
    JOBMOVE::Enter();
    pg->grfState &= ~grfActive;
    pg->grfState |= grfOld;
    if (pkinde)
    {
        if (pg->kMove == -1 || pg->kMove == 0 || pg->kMove == 3)
        {
            const glm::vec3 d = pos - PosAi(pg->palo);
            SetMoveHeading(&pg->move, std::atan2(d.y, d.x));  // 0x209818
        }
        // TODO(ai): kind 2 (0x209840 with its linked object's x row)
        if (pkinde->k == 2 && pkinde->pvState)
        {
            // the driver (the kind-2 player): its related object, kind, ballistic flag and target (0x20C6F8)
            K2PLAY* pk = static_cast<K2PLAY*>(pkinde->pvState);
            pk->fBallistic = nExtra;
            pk->ploRelated = ploTarget;
            pk->kind = fReverse;
            SetK2Target(pk, pos);
        }
    }
    LockJob(this);
}

// orig: 0x0017A3D0
void JOBUSE::Exit()
{
    JOBMOVE::Exit();
    UnlockJob(this);
}

// orig: 0x0017A400: with its orientation and the driver's kind-2 step 2 or more: the heading's frame set to it
// (0x209840: TODO(ai) with the kind-2 movers)
void JOBUSE::Update()
{
    JOBMOVE::Update();
}

// orig: 0x001715F0
void JOBSCRIPT::Execute(CMD* c)
{
    GUARD* pg = pguard;
    if (std::getenv("SLY2N_SCPTPRINT"))
        std::fprintf(stderr, "ai: guard %d command op %d oid %d list %d t %.9g\n",
                     pg->palo ? pg->palo->oid : -1, c->op, c->oid, pcmdlist ? pcmdlist->oid : -1, g_clock.t);
    switch (c->op)
    {
    case 0: ploNear = PloNear(c->oid); break;
    case 1:
        pg->tWaitA0 = c->g8;
        pg->tWaitA1 = c->gC;
        break;
    case 2:
        pg->tWaitB0 = c->g8;
        pg->tWaitB1 = c->gC;
        break;
    case 3:
    {
        // a look: placed at the near object, facing as it does (0x171AF8, 0x171B60)
        glm::vec3 pos;
        float rad;
        GetCmdAim(pg, PloNear(c->oid), &pos, &rad);
        PlaceGuard(pg, pos, rad);
        break;
    }
    case 4:
        Script(4, c->oid);  // event4 argument type6, the command's inline word
        break;
    case 5:
    {
        const int k = pg->kMove;
        if (k == -1) PjobPush(pg, 0x9E)->Enter();
        // TODO(ai): kinds 2 / 3 (0xBB / 0xBC)
        fRunning = 1;
        break;
    }
    case 6:
    {
        LO* po = PloNear(pcmd->oid);
        if (!po) break;
        if (pg->kMove == -1)
        {
            auto* o = static_cast<JOBPATHSTEP*>(PjobPush(pg, 0xC0));
            o->ploPath = po;
            o->pcrv = PcrvFromLo(po);
            o->fForward = c->n8;
            o->fFromReact = fFromReact;
            // TODO(ai): 0x1E9C30 (the path's AI record) when not from the reactions
            if (psaved) o->s = psaved->g;
            if (c->nC != -1) o->plo50 = PloNear(c->nC);
            o->Enter();
        }
        // TODO(ai): kinds 0 / 1 (0xB8 / 0xB9)
        fRunning = 1;
        break;
    }
    case 7:
    {
        GOAL goal;
        if (pcmd->oid == -1)
        {
            goal.pos = pcmd->ppos8 ? *pcmd->ppos8 : glm::vec4(0.0f);
            radFace = pcmd->gC;
        }
        else if (LO* po = PloNear(pcmd->oid))
        {
            GetCmdAim(pg, po, nullptr, &radFace);  // (0x171AF8: the object's facing into +0x48)
            if (FIsLoDerivedFrom(po, 0xA)) goal.pcrv = PcrvFromLo(po);
            else goal.plo = po;
        }
        if (pg->kMove == -1)
        {
            auto* o = static_cast<JOBGOTO*>(PjobPush(pg, 0xBA));
            o->SetGoal(goal);
            if (c->n10 != -1) o->plo = PloNear(c->n10);
            o->gB0 = c->g14;
            o->fDirect = fFromReact;
            o->Enter();
        }
        fRunning = 1;
        break;
    }
    case 9: break;  // a label
    case 10:
        // orig 0x1711F0: go to the list's first label (op 9) of this id; the update continues after it
        if (pcmdlist)
            for (const auto& p : pcmdlist->acmd)
                if (p->op == 9 && p->oid == c->oid)
                {
                    pcmd = p.get();
                    break;
                }
        break;
    case 11: LockJob(this); break;
    case 12: UnlockJob(this); break;
    case 13:
    {
        // the use of an object (0xAC: its object, reversed by the word, +0x68 1)
        auto* o = static_cast<JOBUSE*>(PjobPush(pg, 0xAC));
        o->ploTarget = PloNear(pcmd->oid);
        o->nExtra = 1;
        o->fReverse = c->n8;
        o->Enter();
        fRunning = 1;
        break;
    }
    case 14:
    {
        // orig: 0x2090D0 / 0x209130: reset the kind payload, retaining its playing handles, then resolve it.
        kinde50.k = -1;
        kinde50.oid = c->oid;
        kinde50.plo = nullptr;
        kinde50.pk2 = nullptr;
        kinde50.pk3 = nullptr;
        kinde50.grf = c->n8;
        ResolveKinde(&kinde50, pg);
        if (pg->kMove == -1)
        {
            auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
            o->pkinde = &kinde50;
            o->Enter();
            fRunning = 1;
        }
        if (std::getenv("SLY2N_SCPTPRINT"))
            std::fprintf(stderr, "ai: guard %d command14 kind %d oid %d flags %d running %d child %X t %.9g list %d\n",
                         pg->palo ? pg->palo->oid : -1, kinde50.k, kinde50.plo ? kinde50.plo->oid : kinde50.oid,
                         kinde50.grf, fRunning, pchild ? pchild->cid : 0, g_clock.t, pcmdlist ? pcmdlist->oid : -1);
        break;
    }
    case 15:
        // an alarm of a script object (0x1E0020 on the near object c->oid with the alarm c->n8)
        if (LO* po = PloNear(c->oid)) EnsureScptAlarm(po, c->n8);
        break;
    default:
    {
        static bool s_afWarned[16] = {};
        if (c->op >= 0 && c->op < 16 && !s_afWarned[c->op])
            std::fprintf(stderr, "ai: command %d not ported\n", c->op);  // TODO(ai): 8
        if (c->op >= 0 && c->op < 16) s_afWarned[c->op] = true;
        break;
    }
    }
    tCmd = g_clock.t;
}

// orig: 0x00171C18
void JOBSCRIPT::Save(SAVEDSCRIPT* p)
{
    const int k = pguard->kMove;
    if (k == -1)
    {
        p->pcmd = pcmd;
        p->plo = ploNear;
        if (pcmd && pcmd->op == 6 && pchild) p->g = static_cast<JOBPATHSTEP*>(pchild)->s;
    }
    else if (k >= -1 && k < 2) *p = SAVEDSCRIPT{};
}

// ---- 0xAA: waiting between patrols ----

// orig: 0x00179A58
void JOBWAIT::Init()
{
    st = -1;
    tA = FLT_MAX;
    tB = FLT_MAX;
    AddGuardState(pguard, 0x1E);
}

// orig: 0x00179AA0
void JOBWAIT::Enter()
{
    GUARD* pg = pguard;
    if (pg->pcmdlistPatrol) pcmdlist = pg->pcmdlistPatrol;
    else if (pg->grfFE8 & 2) pcmdlist = pg->pcmdlistHome;
    else
    {
        pcmdlist = nullptr;
        SetWaitState(2);
        return;
    }
    if (pg->saved2DC.pcmd)
    {
        // TODO(ai): 0x170E90 (the saved place re-found nearest the actor)
    }
    SetWaitState(pg->fFC8 || FGamePausedForAi() ? 3 : 0);  // (0x209098)
}

// orig: 0x00179B60
void JOBWAIT::Exit()
{
    SetWaitState(-1);
    AIJOB::Exit();
    RemoveGuardState(pguard, 0x1E);
}

// orig: 0x00179D30
void JOBWAIT::SetWaitState(int n)
{
    if (n == st) return;
    if (st == 0 && pchild) static_cast<JOBSCRIPT*>(pchild)->Save(&pguard->saved2DC);
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    GUARD* pg = pguard;
    if (static const char* s_sz = std::getenv("SLY2N_PATHSTEPPRINT"); s_sz && pg->palo && pg->palo->oid == std::atoi(s_sz))
        std::fprintf(stderr, "wait %d t %.3f state %d at %.0f %.0f %.0f\n", pg->palo->oid, g_clock.t, n, PosAi(pg->palo).x, PosAi(pg->palo).y,
                     PosAi(pg->palo).z);
    if (n == 1 || n == 2)
    {
        PjobPush(pg, 0x9E)->Enter();
        // orig 0x179EF8: the next wait's end from the range +0x2D4
        if (!(tB < FLT_MAX))
        {
            if (pg->tWaitB0 == FLT_MAX) tB = FLT_MAX;
            else tB = g_clock.t + GRandInRange(pg->tWaitB0, pg->tWaitB1);
        }
        tA = FLT_MAX;
    }
    else if (n == 0)
    {
        auto* o = static_cast<JOBSCRIPT*>(PjobPush(pg, 0xA9));
        o->SetTarget(pcmdlist, 0, &pg->saved2DC);
        o->Enter();
        if (!(tA < FLT_MAX))
        {
            if (pg->tWaitA0 == FLT_MAX) tA = FLT_MAX;
            else tA = g_clock.t + GRandInRange(pg->tWaitA0, pg->tWaitA1);
        }
        tB = FLT_MAX;
        if (static const char* s_sz = std::getenv("SLY2N_PATHSTEPPRINT"); s_sz && pg->palo && pg->palo->oid == std::atoi(s_sz))
            std::fprintf(stderr, "wait %d t %.3f run until %.3f (range %.2f..%.2f, stand %.2f..%.2f)\n", pg->palo->oid, g_clock.t, tA, pg->tWaitA0,
                         pg->tWaitA1, pg->tWaitB0, pg->tWaitB1);
    }
    else if (n == 3)
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->nAnim = 1;  // (+0x3C: the alarmed stand)
        o->Enter();
    }
}

// orig: 0x00179BA0
void JOBWAIT::Update()
{
    AIJOB::Update();
    const bool c = pguard->fFC8 != 0 || FGamePausedForAi();  // orig 0x209098
    while (!FJobDone(this))
    {
        const int s0 = st;
        switch (s0)
        {
        case 1:
            if (c) SetWaitState(3);
            else if (tB < g_clock.t) SetWaitState(0);
            break;
        case 2:
            if (c) SetWaitState(3);
            break;
        case 0:
            if (c) SetWaitState(3);
            else if (tA < g_clock.t) SetWaitState(1);
            else if (pchild && static_cast<JOBSCRIPT*>(pchild)->FDone())
            {
                pcmdlist = nullptr;  // (+0x40: no list any more)
                SetWaitState(2);
            }
            break;
        case 3:
            if (!c) SetWaitState(pcmdlist ? 1 : 2);
            break;
        default: break;
        }
        if (st == s0) break;
    }
}

// ---- 0xBA: go to ----

// orig: 0x0017D360
void JOBGOTO::Init()
{
    nAnim = -1;
    tSearch = -FLT_MAX;
    st = -1;
    nSerial = 0;  // TODO(ai): the world BSP's polygon serial (+0x28)
}

// orig: 0x0017D3B0
void JOBGOTO::Enter()
{
    if (nAnim != -1) AddGuardState(pguard, nAnim);
    SetState(4);
    Search();
}

// orig: 0x0017D408
void JOBGOTO::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    if (nAnim != -1) RemoveGuardState(pguard, nAnim);
}

// orig: 0x0017D450
void JOBGOTO::SetState(int n)
{
    const int old = st;
    if (n == old) return;
    if (!(static_cast<unsigned>(n) < 3 && old >= 0 && old < 3)) EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    GUARD* pg = pguard;
    switch (n)
    {
    case 4:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->ploFace = plo;
        o->Enter();
        break;
    }
    case 1: case 2:
    {
        auto* o = static_cast<JOBFOLLOW*>(pchild);
        if (!o)
        {
            o = static_cast<JOBFOLLOW*>(PjobPush(pg, 0xA7));
            o->ploArg = plo;
            o->fNoAvoid = fDirect;
            int k = 9;
            if (plo && !fAC && (pg->grfFE8 & 4)) k = 8;
            o->kMove = k;
            o->Follow(&pg->crv294, 0);
            o->Enter();
        }
        else o->Follow(&pg->crv294, 0);
        break;
    }
    case 3:
    {
        // the link's job (0xBE) on the path's first link
        auto* o = static_cast<JOBLINK*>(PjobPush(pg, 0xBE));
        o->plink = anode.empty() ? nullptr : static_cast<NAVLINK*>(anode.front());
        o->Enter();
        break;
    }
    case 5:
    {
        auto* o = static_cast<JOBNAVPOINT*>(PjobPush(pg, 0xBF));
        o->goal = goalFallback;
        o->Enter();
        break;
    }
    default: break;
    }
}

// orig: 0x0017D8A8
void JOBGOTO::Target17D8A8()
{
    anode.clear();
    goalCur = goal;
    if (fDirect) return;
    const glm::vec3 posSelf = PosAi(pguard->palo);
    const glm::vec3 v = PosGoal(goal, posSelf);
    NAVREGION* q = PregionAt(posSelf, false);
    NAVREGION* r = PregionAt(v, false);
    bool fFail = false;
    if (static const bool s_fDbg = std::getenv("SLY2N_GOTODEBUG") != nullptr; s_fDbg)
    {
        std::printf("goto target: t %.3f guard %d region %d -> %d (%.0f %.0f %.0f -> %.0f %.0f %.0f)\n", g_clock.t, pguard->palo->oid,
                    q && q->pmesh ? q->pmesh->oid : -1, r && r->pmesh ? r->pmesh->oid : -1, posSelf.x, posSelf.y, posSelf.z, v.x, v.y, v.z);
        static bool s_fLinks = false;
        if (!s_fLinks)
        {
            s_fLinks = true;
            for (NAVLINK* pl : AplinkWorld())
                std::printf("  link %d %d->%d kind %d allowed %d recs %zu\n", pl->plo->oid, pl->pregionBegin && pl->pregionBegin->pmesh ? pl->pregionBegin->pmesh->oid : -1,
                            pl->pregionEnd && pl->pregionEnd->pmesh ? pl->pregionEnd->pmesh->oid : -1, pl->kind, FNavLinkAllowed(pl, pguard) ? 1 : 0, pl->agoal.size());
        }
    }
    if (!r) fFail = true;
    else if (r != q)
    {
        // 0x1C21D8: the links over the regions to the target's (else, with +0xC0, to a region holding it: the goal
        // its nearest walkable point there, 0x1C0940)
        std::vector<NAVLINK*> apl;
        int n = CNavLinkPath(pguard, v, true, 1, &apl);
        if (n <= 0)
        {
            if (!fC0) fFail = true;
            else
            {
                n = CNavLinkPath(pguard, v, false, 1, &apl);
                if (!n) fFail = true;
                else
                {
                    NAVREGION* e = q;
                    if (!apl.empty() && apl[0]) e = apl.back()->pregionEnd;
                    else apl.clear();
                    glm::vec3 posNear(0.0f);
                    goalCur.pcrv = nullptr;
                    goalCur.plo = nullptr;
                    if (e && FNavPoint(e, v, &posNear, 1)) goalCur.pos = glm::vec4(posNear, 0.0f);
                }
            }
        }
        for (NAVLINK* pl : apl)
            if (pl) anode.push_back(pl);
    }
    if (fFail && std::getenv("SLY2N_GOTODEBUG"))
    {
        std::printf("goto: t %.3f guard %d no way from region %d to %d (%.0f %.0f %.0f -> %.0f %.0f %.0f)\n", g_clock.t, pguard->palo->oid,
                    q && q->pmesh ? q->pmesh->oid : -1, r && r->pmesh ? r->pmesh->oid : -1, posSelf.x, posSelf.y, posSelf.z, v.x, v.y, v.z);
        for (NAVLINK* pl : AplinkWorld())
            if (pl->pregionBegin == q)
                std::printf("  link %d %d->%d kind %d allowed %d\n", pl->plo->oid, q->pmesh ? q->pmesh->oid : -1,
                            pl->pregionEnd && pl->pregionEnd->pmesh ? pl->pregionEnd->pmesh->oid : -1, pl->kind, FNavLinkAllowed(pl, pguard) ? 1 : 0);
    }
    if (fFail)
    {
        goalCur.pos = glm::vec4(posSelf, 0.0f);
        goalCur.pcrv = nullptr;
        goalCur.plo = nullptr;
    }
}

// orig: 0x0017DA28
void JOBGOTO::Search()
{
    if (st == -1) return;
    if (!(pguard->grfFE8 & 2)) return;
    if (FJobDone(this)) return;
    SetState(0);
    Target17D8A8();
    GOAL g;
    int k;
    if (anode.empty())
    {
        g = goalCur;
        k = 1;
    }
    else
    {
        // the first link's begin; near its walked segment already: the link's job 0xBE (3)
        NAVLINK* pl = static_cast<NAVLINK*>(anode.front());
        g = pl->agoal.empty() ? goalCur : pl->agoal.front();
        k = 2;
        if (pl->kind == 0)
        {
            const glm::vec3 pos = PosAi(pguard->palo);
            const glm::vec3 a = pl->posBegin, b = pl->posEnd;
            const glm::vec3 d = b - a;
            const float dd = glm::dot(d, d);
            const float t = dd > 1e-6f ? glm::dot(pos - a, d) / dd : -1.0f;  // 0x11EE08
            if (0.0f < t && t < 1.0f)
            {
                const glm::vec3 c = a + d * t;
                if (glm::length(glm::vec2(c - pos)) < 50.0f && std::fabs(c.z - pos.z) < 300.0f) k = 3;
            }
        }
    }
    if (k > 0 && k < 3)
    {
        CRV* b = &pguard->crv294;
        const glm::vec3 posSelf = PosAi(pguard->palo);
        const glm::vec3 q = PosGoal(g, posSelf);
        if (fDirect)
        {
            b->crvk = 0;
            b->fClosed = 0;
            b->apos = {posSelf, q};
            b->ag = {0.0f, 1.0f};
            RecalcCrv(b, glm::mat4(1.0f));
        }
        else
        {
            NAVREGION* e = PregionAt(posSelf, false);
            if (!e || !FNavPathCrv(e, posSelf, q, b))
            {
                k = 5;
                goalFallback = g;
            }
        }
    }
    if (static const char* s_sz = std::getenv("SLY2N_PATHSTEPPRINT"); s_sz && pguard->palo && pguard->palo->oid == std::atoi(s_sz))
    {
        const glm::vec3 p = PosAi(pguard->palo), q = PosGoal(g, p);
        std::fprintf(stderr, "goto search %d t %.3f state %d links %zu at %.0f %.0f %.0f to %.0f %.0f %.0f direct %d\n", pguard->palo->oid, g_clock.t, k,
                     anode.size(), p.x, p.y, p.z, q.x, q.y, q.z, fDirect);
    }
    SetState(k);
    tSearch = g_clock.t;
    nSerial = 0;
}

// orig: 0x0017DCB8
int JOBGOTO::NArrived() const
{
    if (st != 1) return 0;
    return pchild && static_cast<JOBFOLLOW*>(pchild)->FArrived() ? st : 0;
}

// orig: 0x0017DCF8
bool JOBGOTO::FStuck() const
{
    if (st >= 3 || st <= 0) return false;
    return pchild && static_cast<JOBFOLLOW*>(pchild)->st == 3;  // 0x178D80
}

// orig: 0x0017DD38
void JOBGOTO::SetGoal(const GOAL& g)
{
    goal = g;
    tSearch = -FLT_MAX;
    Search();
}

// orig: 0x0017D6C8
void JOBGOTO::Update()
{
    AIJOB::Update();
    if (!(pguard->grfFE8 & 2)) return;
    while (!FJobDone(this))
    {
        const int s0 = st;
        switch (s0)
        {
        case 1: case 2:
        {
            auto* t = static_cast<JOBFOLLOW*>(pchild);
            if (s0 == 2 && t && t->FArrived()) SetState(3);
            else if (T_RESEARCH < g_clock.t - tSearch /* || the world's polygons changed (+0xA4) */) Search();
            else if (t && t->FStuck())
            {
                // the shortcut at the path's end (5)
                goalFallback = GOAL{};
                glm::vec3 p;
                EvalCrvS(t->pcrv, std::min(SLengthCrv(t->pcrv), t->s + S_SHORTCUT), &p, nullptr);
                goalFallback.pos = glm::vec4(p, PosGuardProbe(pguard).z);
                SetState(5);
            }
            break;
        }
        case 3:
            // the link crossed (0x17E570: its state -1): searched again from the new region
            if (!pchild || static_cast<JOBLINK*>(pchild)->st == -1) Search();
            break;
        case 5:
            // the way-point reached (0x17EAE8)
            if (!pchild || static_cast<JOBNAVPOINT*>(pchild)->FDone()) Search();
            break;
        default: break;
        }
        if (s0 == st) break;
    }
}

// ---- 0xC0: steps along a path ----
bool FPathSpanAt(LO* ploPath, const CRV* pcrv, float g, float* pgMin, float* pgMax);  // spawner.cpp
void UpdatePathSpans(LO* ploPath, const CRV* pcrv);  // spawner.cpp (0x1EA000)

// orig: 0x0017EB48
void JOBPATHSTEP::Init()
{
    s = -1.0f;
    AddGuardState(pguard, 0x2E);
}

// orig: 0x0017EB88
void JOBPATHSTEP::Enter()
{
    const glm::vec3 posSelf = PosAi(pguard->palo);
    glm::vec3 q;
    if (0.0f <= s) s = SNearestFrom(pcrv, posSelf, s, &q);
    else
    {
        float sN = 0;
        NearestCrvAll(pcrv, posSelf, &q, nullptr, nullptr, &sN, &AXIS_Z);
        s = sN;
    }
    pos = glm::vec4(q, 0.0f);
    auto* o = static_cast<JOBGOTO*>(PjobPush(pguard, 0xBA));
    o->plo = plo50;
    o->fAC = n54;
    o->fDirect = fFromReact;
    Step(true);
    o->Enter();
}

// orig: 0x0017EC80
void JOBPATHSTEP::Exit()
{
    AIJOB::Exit();
    RemoveGuardState(pguard, 0x2E);
}

// orig: 0x0017ECB0
void JOBPATHSTEP::Update()
{
    AIJOB::Update();
    if (pchild && static_cast<JOBGOTO*>(pchild)->NArrived()) Step(false);
}

// orig: 0x0017ECF0
void JOBPATHSTEP::Step(bool fForce)
{
    float d = pguard->palo->Cid() == 0x3A || FCidDerivedFrom(pguard->palo->Cid(), 0x3A) ? S_STEP_3A : S_STEP;
    if (!fForward) d = -d;
    float t = s + d;
    if (!pcrv->fClosed) t = Clamp(t, SStart(pcrv), SLengthCrv(pcrv));
    else t = SWrapCrv(pcrv, t);
    if (!fFromReact)
    {
        // the path's AI record (0x1EA000: kept as loaded; 0x1EA3B0): a step into an off-region span goes on to
        // the span's far end (its end going forward, its start going back), wrapped
        float gMin = 0.0f, gMax = 0.0f;
        UpdatePathSpans(ploPath, pcrv);
        if (FPathSpanAt(ploPath, pcrv, t, &gMin, &gMax)) t = SWrapCrv(pcrv, fForward ? gMax : gMin);
    }
    if (!fForce && std::fabs(s - t) < S_STEP_MIN) return;
    if (static const char* s_sz = std::getenv("SLY2N_PATHSTEPPRINT"); s_sz && pguard->palo && pguard->palo->oid == std::atoi(s_sz))
    {
        if (fForce && std::getenv("SLY2N_PATHSAMPLE"))
        {
            std::fprintf(stderr, "  path crvk %d closed %d points %zu guard at %.1f %.1f %.1f\n", pcrv->crvk, pcrv->fClosed, pcrv->apos.size(),
                         PosAi(pguard->palo).x, PosAi(pguard->palo).y, PosAi(pguard->palo).z);
            for (size_t i = 0; i < pcrv->apos.size(); ++i)
            {
                const glm::vec3 w = glm::vec3(pcrv->mat * glm::vec4(glm::vec3(pcrv->apos[i]), 1.0f));
                std::fprintf(stderr, "  ctl %zu g %.1f s %.1f at %.0f %.0f %.0f\n", i, pcrv->ag[i], SFromCrvG(pcrv, pcrv->ag[i]), w.x, w.y, w.z);
            }
        }
        if (fForce && std::getenv("SLY2N_PATHSAMPLE"))
            for (float g = 0.0f; g < SLengthCrv(pcrv); g += 250.0f)
            {
                glm::vec3 q;
                EvalCrvS(pcrv, g, &q, nullptr);
                std::fprintf(stderr, "  path s %.0f at %.0f %.0f %.0f\n", g, q.x, q.y, q.z);
            }
        glm::vec3 q;
        EvalCrvS(pcrv, t, &q, nullptr);
        std::fprintf(stderr, "pathstep %d t %.3f path %d fwd %d react %d s %.1f -> %.1f (len %.1f closed %d) goal %.0f %.0f %.0f\n", pguard->palo->oid,
                     g_clock.t, ploPath ? ploPath->oid : -1, fForward, fFromReact, s, t, SLengthCrv(pcrv), pcrv->fClosed, q.x, q.y, q.z);
    }
    s = t;
    glm::vec3 q;
    EvalCrvS(pcrv, t, &q, nullptr);
    pos = glm::vec4(q, 0.0f);
    GOAL g;
    g.pos = pos;
    if (pchild) static_cast<JOBGOTO*>(pchild)->SetGoal(g);
}

// orig: 0x0017EEB8
bool JOBPATHSTEP::FAtEnd() const
{
    if (pcrv->fClosed) return false;
    const float g = fForward ? SLengthCrv(pcrv) : SStart(pcrv);
    return !(S_PATHEND < std::fabs(s - g));
}

// orig: 0x0017EE78
bool JOBPATHSTEP::FDone()
{
    if (!FAtEnd()) return false;
    return pchild && static_cast<JOBGOTO*>(pchild)->NArrived();
}

// ---- 0xBD: away from an object ----

namespace
{
constexpr float T_BD_CHOOSE = 0.5f;      // D_002BE760
constexpr float S_BD_GOAL_NEAR = 150.0f; // D_002BE764
constexpr float S_BD_FROM_NEAR = 300.0f; // D_002BE768
constexpr float S_BD_RAY = 500.0f;       // D_002BE76C
constexpr float A_RAD_BD_RAY[16] = {0.0f, 0.15f, -0.15f, 0.3f, -0.3f, 0.6f, -0.6f, 1.0f, -1.0f, 1.5f, -1.5f, 2.0f, -2.0f, 2.5f, -2.5f, 3.14f};  // D_002BE770
float SDistXY(const glm::vec3& a, const glm::vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
}

// orig: 0x0017DD70
void JOBBD::Init()
{
    AIJOB::Init();
    AddGuardState(pguard, 0x2B);
    st = -1;
}

// orig: 0x0017DDA8
void JOBBD::Enter()
{
    AIJOB::Enter();
    SetState(0);
    SetGuardPursued(pguard, ploFrom);  // 0x208540
}

// orig: 0x0017DDE8: the detector forgets (0x153FF8), nothing pursued, state 0x2B removed
void JOBBD::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    GUARD* pg = pguard;
    if (pg->pdet)
        for (DETENTRY& e : pg->pdet->ae)
        {
            e.st = 0;
            e.t8 = g_clock.t;
        }
    SetGuardPursued(pg, nullptr);
    RemoveGuardState(pg, 0x2B);
}

// orig: 0x0017E278
void JOBBD::SetState(int n)
{
    if (n == st) return;
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    switch (n)
    {
    case 0:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9D));
        o->nAnim = 0xB;
        o->ploFace = ploFrom;
        o->Enter();
        break;
    }
    case 1:
    {
        auto* o = static_cast<JOBGOTO*>(PjobPush(pguard, 0xBA));
        o->fC0 = 1;
        o->nAnim = 0x1C;
        ChooseAway();
        o->Enter();
        break;
    }
    case 2:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9E));
        o->nAnim = 1;
        o->Enter();
        break;
    }
    }
}

// orig: 0x0017E0E0: of 16 rays of 500 around the direction away from the object (the walkable area's points,
// 0x1C0B50), the one farthest from it becomes the goto's goal (else the goal's current point)
void JOBBD::ChooseAway()
{
    tChoose = g_clock.t;
    auto* pgoto = static_cast<JOBGOTO*>(pchild);
    if (!pgoto) return;
    const glm::vec3 a = PosAi(ploFrom), b = PosAi(pguard->palo);
    const glm::vec3 d = b - a;
    NAVREGION* preg = PregionAt(b, false);  // (the guard's +0xE8 +0xDC +0x10)
    float best = 0.0f;
    const float rad = std::atan2(d.y, d.x);
    glm::vec3 r = PosGoal(pgoto->goal, b);
    for (float radOff : A_RAD_BD_RAY)
    {
        const glm::vec3 s = b + cmm::SetVectorCylind(rad + radOff, S_BD_RAY, 0.0f);  // 0x11E1A0
        glm::vec3 hit(0.0f);
        const bool fHit = preg && FKeepOnNav(preg, s, &hit);
        if (static bool s_fPolys = std::getenv("SLY2N_AWAYDBG") != nullptr; s_fPolys && preg)
        {
            s_fPolys = false;
            std::printf("  region mesh %d: %zu poly slots\n", preg->pmesh ? reinterpret_cast<LO*>(preg->pmesh)->oid : -1, preg->apoly.size());
            for (const NAVPOLY* pp : preg->apoly)
                if (pp && pp->posMax.x > -2500 && pp->posMin.x < -500 && pp->posMax.y > 5300 && pp->posMin.y < 7000)
                    std::printf("  poly %d (%.0f %.0f)..(%.0f %.0f) pts %zu\n", pp->plo ? pp->plo->oid : -1, pp->posMin.x, pp->posMin.y, pp->posMax.x, pp->posMax.y,
                                pp->aposWorld.size());
        }
        if (std::getenv("SLY2N_AWAYDBG"))
            std::printf("  ray %.2f (%.0f %.0f) -> %d (%.1f %.1f %.1f) d %.1f\n", radOff, s.x, s.y, int(fHit), hit.x, hit.y, hit.z, SDistXY(hit, a));
        if (fHit)
        {
            const float g = SDistXY(hit, a);
            if (g > best)
            {
                best = g;
                r = hit;
            }
        }
    }
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("away: t %.3f guard %d from (%.0f %.0f) at (%.0f %.0f) region %p -> (%.1f %.1f %.1f) best %.1f\n", g_clock.t, pguard->palo->oid, a.x, a.y, b.x, b.y,
                    (void*)preg, r.x, r.y, r.z, best);
    GOAL x;
    x.pos = glm::vec4(r, 0.0f);
    pgoto->SetGoal(x);  // 0x17DD38
}

// orig: 0x0017DE50
void JOBBD::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    while (!FJobDone(this))
    {
        const int s = st;
        switch (st)
        {
        case 0:
            if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1) SetState(1);  // (0x174FB8)
            break;
        case 1:
            if (FGuardStill(pg)) SetState(2);
            else
            {
                tLeft -= g_clock.dt;
                if (tLeft <= 0.0f)
                {
                    LO* o = PloGuardTarget(pg);  // (vt slot 88)
                    if (o == ploFrom && FTargetClass(pg->acid3C4, o)) tLeft = GRandInRange(5.0f, 7.0f);  // 0x208B38 / 0x208BB0
                    else SetState(-1);
                }
            }
            break;
        case 2:
            if (!FGuardStill(pg)) SetState(1);
            break;
        }
        if (st == s) break;
    }
    if (st == 1 && pchild && g_clock.t - tChoose > T_BD_CHOOSE)
    {
        const glm::vec3 posG = PosAi(pg->palo);
        const glm::vec3 q = PosGoal(static_cast<JOBGOTO*>(pchild)->goal, posG);
        if (SDistXY(posG, q) < S_BD_GOAL_NEAR || SDistXY(posG, PosAi(ploFrom)) < S_BD_FROM_NEAR) ChooseAway();
    }
}

std::unique_ptr<AIJOB> PjobNewKind(CID cid)
{
    switch (cid)
    {
    case 0x9C: return std::make_unique<JOBREACT>();
    case 0x9D: return std::make_unique<JOBMOVE>();
    case 0x9E: return std::make_unique<JOBSTAND>();
    case 0xAF: return std::make_unique<JOBSLEEP>();
    case 0xBD: return std::make_unique<JOBBD>();
    case 0xB0: return std::make_unique<JOBB0>();
    case 0xB1: return std::make_unique<JOBB1>();
    case 0xA7: return std::make_unique<JOBFOLLOW>();
    case 0xA9: return std::make_unique<JOBSCRIPT>();
    case 0xAA: return std::make_unique<JOBWAIT>();
    case 0xBA: return std::make_unique<JOBGOTO>();
    case 0xC0: return std::make_unique<JOBPATHSTEP>();
    case 0xAC: return std::make_unique<JOBUSE>();
    case 0xAE: return std::make_unique<JOBPROX>();
    case 0xB6: return std::make_unique<JOBCHASE>();
    case 0xBE: return std::make_unique<JOBLINK>();
    case 0xBF: return std::make_unique<JOBNAVPOINT>();
    default: return PjobNewPursuit(cid);  // (aipursuit.cpp)
    }
}
