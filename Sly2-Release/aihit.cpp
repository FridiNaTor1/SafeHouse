// The guards hit (module 0x174898 / 0x179328..0x179A20): the reaction job's message handler 0x176BE8 (a contact's hit
// offered to the jobs as message 12: the hit points +0x33C, the block +0x340 / +0x344, the reactions 9 / 10 / 11 /
// 0x13), the hit jobs 0xA0 (base) / 0xA1 / 0xA2 / 0xA3 (the hit's animation from the arrays +0xA70 / +0x8D0 / +0xC10
// by the hit's kind), and the reaction after one (0x177BA0). See aihit.h.

#include "aihit.h"
#include "rumble.h"
#include "step.h"

#include "aipursuit.h"
#include "actor.h"
#include "sly.h"
#include "xfphys.h"
#include "vtnative.h"
#include "aijobs.h"
#include "gang.h"
#include "ainoise.h"
#include "activate.h"
#include "asega.h"
#include "clock.h"
#include "cls.h"
#include "detect.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "po.h"
#include "vecmat.h"
#include "xfmath.h"
#include "pickup.h"
#include "pocket.h"
#include "worldaccess.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr float T_AWARE = 0.2f;
constexpr float T_SLEEP_MORE = 5.0f;  // D_002BE620    // D_002C3DAC (0x20A298: seen this long ago, the hit is not a surprise)

// orig: 0x001F78A0: the index of the lowest bit
int IbitLow(uint32_t u)
{
    for (int i = 0; i < 32; ++i)
        if (u & (1u << i)) return i;
    return 0;
}

AIMSG::HIT HitOf(const CTMSG* pm)
{
    AIMSG::HIT h;
    h.psource = pm->ploSender;
    h.pvOther = pm->pvOther;
    h.preceiver = pm->ploReceiver;
    h.kind = pm->grfFilter;
    h.n = pm->nState;
    h.pos = pm->pos;
    h.vec = pm->vec;
    h.grf = pm->grfAccepted;
    return h;
}

glm::vec3 PosOfLo(LO* plo) { return PosAi(plo); }
}  // namespace

// orig: 0x0020A298: a hit the guard did not see coming (1): the source unseen, seen only just now, or in the mask's state
bool FGuardSurprised(GUARD* pg, LO* psource, uint64_t mask)
{
    if (pg->n3D0) return false;
    if (FCidDerivedFrom(psource->Cid(), 0x36) && !FCidDerivedFrom(pg->palo->Cid(), 0x3A))
    {
        if (!(pg->grfState & GrfStateBit(6))) return true;
    }
    const DETENTRY* pe = pg->pdet ? PdetentryFind(pg->pdet, psource) : nullptr;
    if (!pe || pe->st == 0) return true;
    if (g_clock.t - pe->tC < T_AWARE) return true;
    return (pg->grfState & mask) != 0;
}

// orig: 0x0020A378
bool FGuardLaunchAllowed(GUARD* pg, LO* psource, uint64_t mask)
{
    if (psource && FCidDerivedFrom(psource->Cid(), 0x36) &&
        !FCidDerivedFrom(pg->palo->Cid(), 0x3A) && !FCidDerivedFrom(pg->palo->Cid(), 0x3D) && pg != g_pguard3F)
        return true;
    return FGuardSurprised(pg, psource, mask);
}

// orig: 0x00176B80
void JOBREACT::SendHit(int n, LO* ploSender)
{
    CTMSG m;
    m.ploSender = ploSender;
    m.ploReceiver = pguard->palo;
    m.grfFilter = 0x40000;
    m.nState = n;
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("launch hit: t %.3f guard %d owner %d kind %X amount %d hp %d\n", g_clock.t,
                    pguard->palo->oid, ploSender ? ploSender->oid : -1, m.grfFilter, n, pguard->n33C);
    PaloAcceptContact(&m);
}

// orig: 0x00176BE8 (the reaction job's slot 13: message 12 the contact's hit)
int JOBREACT::Message(int nmsg, void* pv)
{
    if (nmsg != 0xC) return AIJOB::Message(nmsg, pv);
    CTMSG* pm = static_cast<CTMSG*>(pv);
    GUARD* pg = pguard;
    AIMSG m;
    AIMSG::HIT h = HitOf(pm);
    auto send = [&](int k, const AIMSG::HIT* ph) {
        m.k = k;
        if (ph)
        {
            m.hit = *ph;
            m.n1C = static_cast<int>(ph->kind);
            m.plo = ph->psource;
        }
        React(m);
    };
    if (FGuardHitMatters(pg, pm))
    {
        // TODO(actors): actual Bentley's 0x40000 hit with his +143C flag calls 0x1B9DB8.
        if (h.kind == 0x40000)
            if (NOISETMPL* pt = PtmplNoiseFixed(3)) PnoiseNew(pt, PosOfLo(pg->palo), pg->palo);  // 0x156698 (709)
        h.grf = 0x40;  // original immediate knockout overrides the copied accepted flags
        send(0xB, &h);
        return 1;
    }
    if (pg->nMode == 13 || pg->nMode == 8) h.n <<= 2;
    if (pg->nMode == 14) SetGuardMode(pg, -1);
    if (pg->f29C) h.n = 0;
    if (pg->f4A4 && msg.k != 0xB && msg.k != 5 && (h.kind == 0x4000 || h.kind == 0x2000))
    {
        SetAloEntryMode(pg->palo, 0);  // orig: 0x001F7A68, restore the contact-entry mode for recovery
        send(0x13, nullptr);
        return 1;
    }
    switch (msg.k)
    {
    case 10:
        if (!(h.kind & pg->grf354)) return 1;
        break;
    case 9:
        if (!(GrfAloXpBytes(pg->palo, h.kind) & 0x40))
        {
            const auto* previous = dynamic_cast<const JOBHIT*>(pchild);
            if (!(h.kind & pg->grf350) || (previous && (previous->hit.kind & pg->grf350))) return 1;
        }
        break;
    case 11:
    {
        const auto* previous = dynamic_cast<const JOBHIT*>(pchild);
        if (!(h.kind & pg->grf358) || (previous && (previous->hit.kind & pg->grf358))) return 1;
        break;
    }
    default: break;
    }
    if (pg->plo38C && pg->plo38C == h.psource && pg->ptarget3CC) SetTargetrecTimer(pg->ptarget3CC, 5);
    uint32_t s0 = h.grf;
    if (h.kind == 0x80000 && (s0 & 8))
    {
        // Bentley's sleep dart: asleep already (reaction 8): woken later (0x17A5B0(0) again: its wake time at least
        // now + D_002BE620); else +0xE88 lowered by the hit's amount: at 0 or below refilled from +0xE84 and asleep
        // (reaction 8), else the drowsy clip +0xE8C played (0x1305B8 into +0xE90)
        if (msg.k == 8)
        {
            if (auto* o = dynamic_cast<JOBSLEEP*>(pchild))
            {
                o->stSleep = -1;
                const float tWake = o->tWake;
                o->SetSleepState(0);
                o->tWake = std::max(g_clock.t + T_SLEEP_MORE, tWake);
            }
        }
        else
        {
            pg->nE88 += h.n;
            if (pg->nE88 <= 0)
            {
                pg->nE88 = pg->nE84;
                AIMSG ms;
                ms.k = 8;
                React(ms);
            }
            else if (pg->pasegE8C)
                ApplyAseg(pg->pasegE8C, pg->palo, 0.0f, 1.0f, 1, nullptr);  // TODO(ai): kept in +0xE90
        }
        return 1;
    }
    if (h.kind == 0x200000 && (s0 & 8))
    {
        if (FGuardLaunchAllowed(pg, h.psource, GrfStateBit(6)))
        {
            const int hp = pg->n33C + h.n;
            pg->n33C = hp > 4 ? hp : 5;
            // 0x156698(SW +0x4028: the fixed template 708 (radius 750), the guard's place, the guard): the launch is
            // the quiet one; the loud 706 comes only from the finishing hit / the landing
            if (NOISETMPL* pt = PtmplNoiseFixed(2)) PnoiseNew(pt, PosOfLo(pg->palo), pg->palo);
            m.k = 7;
            m.plo = h.psource;
            React(m);
            return 1;
        }
    }
    if ((h.kind == 0x1000000 || h.kind == 0x2000000) && (s0 & 8))
    {
        msg.k = -1;
        send(FGuardSurprised(pg, h.psource, GrfStateBit(0x17)) ? 0xA : 9, &h);
        SetGuardMode(pg, h.kind == 0x1000000 ? 8 : 0x22);
        return 1;
    }
    if (!(s0 & 0xF0)) return 1;
    // the hit's noise at the guard (0x156698: the fixed template 709 for a dart (0x40000), else 706)
    if (NOISETMPL* pt = PtmplNoiseFixed(h.kind == 0x40000 ? 3 : 0)) PnoiseNew(pt, PosOfLo(pg->palo), pg->palo);
    uint32_t s1 = s0;
    if (msg.k >= 8 && msg.k < 11 && FCidDerivedFrom(h.psource->Cid(), 0x65)) s1 = (s0 & 0xB0) ? (s0 | 0x40) : s0;
    if (FGuardSurprised(pg, h.psource, GrfStateBit(0x17))) s1 |= 0x80;
    if (s1 & 0x40) s1 &= ~0xB0u;
    if (s1 & 0xA0) s1 &= ~0x10u;
    if (s1 & 0x80)
    {
        s1 &= ~0x20u;
        const int k = IbitLow(h.kind);
        if (pg->aakinde[0][k].k == -1 && pg->aakinde[1][k].k != -1)
        {
            s1 &= ~0x80u;
            s1 |= 0x20;
        }
        if (pg->aakinde[0][k].k == -1 && !PkindeBest(pg, GrfStateBit(6)))
        {
            s1 &= ~0x80u;
            s1 |= 0x20;
        }
    }
    if (s1 & 0x10)
    {
        if (pg->n340 <= 0) s1 |= 0x20;
        else
        {
            pg->n344 += h.n;
            if (pg->n344 <= 0) s1 |= 0x20;
            else
            {
                // the block's clip (+0x348 into +0x34C: 0x1305B8)
                ApplyAsegCur(pg->paseg348, pg->palo, 0.0f, 1.0f, 1, &pg->pasega34C);
                pg->g360 = g_clock.t;
            }
        }
    }
    AIMSG::HIT hOut = h;
    int kOut = 0;
    if (s1 & 0xA0)
    {
        pg->n344 = pg->n340;
        const int v = pg->n340;
        const int d = v > 0 ? -v : h.n;
        int n = pg->n33C + d;
        if (n < 0) n = 0;
        pg->n33C = n;
        if (std::getenv("SLY2N_GUARDHP"))  // (debug: a guard's hit points lowered by a hit)
            std::printf("guard hp: t %.3f guard %d from %d kind 0x%X amount %d hp %d\n", g_clock.t, pg->palo->oid,
                        h.psource ? h.psource->oid : -1, h.kind, d, pg->n33C);
        if (pg->n33C <= 0) s1 |= 0x40;
        else kOut = (s1 & 0x80) ? 10 : 9;
    }
    if (s1 & 0x40) kOut = 0xB;
    if (!(s1 & 0xE0)) return 1;
    if (FCidDerivedFrom(h.psource->Cid(), 0x34))
    {
        // the push: away from a point 150 behind the source (its facing), 500 long
        const glm::vec3 posG = PosOfLo(pg->palo);
        const ALO* ps = static_cast<ALO*>(h.psource);
        const glm::mat4 msrc = ps->apxfRef[2] ? ps->apxfRef[2]->matWorld : glm::mat4(1.0f);
        const glm::vec3 posS = PosOfLo(h.psource) + glm::vec3(msrc[0]) * -150.0f;
        glm::vec3 v3 = posG - posS;
        // TODO(ai): Murray (0x36) in his state 0xC: the push's x quartered in his frame
        const float l = std::sqrt(glm::dot(v3, v3));
        if (l > 0.0f) hOut.vec = v3 / l * 500.0f;
    }
    else if (pg->nMode == -1 && FCidDerivedFrom(h.psource->Cid(), 0x38))
    {
        if (GUARD* source = PguardFromLo(h.psource); source && source->nMode == 0x22) SetGuardMode(pg, 0x22);
    }
    msg.k = -1;
    send(kOut, &hOut);
    // (Bentley's 0x20000 hits: 0x1B9D28)
    return 1;
}

// orig: 0x00177BA0: the reaction after a finished one
void JOBREACT::NextReaction()
{
    GUARD* pg = pguard;
    AIMSG m;
    LO* pme = g_psw ? PloPoCurrent(g_psw) : nullptr;
    if (FCidDerivedFrom(pg->palo->Cid(), 0x3B) && pg->c1010 > 0)
    {
        m.k = 13;
        m.plo = pme;
    }
    else if (pg->nMode == 21)
    {
        m.k = 18;
        m.plo = pme;
        m.g = FLT_MAX;
    }
    else if (pg->fFA8 != 0 || pg->fF94 != 0)  // 0x177D20
        m.k = 0xC;
    else if (FCidDerivedFrom(pg->palo->Cid(), 0x3C) && pg->n108C > 0)  // 0x177D70
        m.k = 15;
    else if (LO* ptarget = PloGuardTarget(pg))
    {
        m.plo = ptarget;
        if (FTargetClass(pg->acid3C4, ptarget))  // 0x208B38
        {
            m.k = 18;
            m.g = GRandInRange(5.0f, 7.0f);  // 0x208BB0
        }
        else
            m.k = 4;
    }
    else if (msg.k >= 6 && msg.k < 11)
    {
        // 0x156B98: a noise (the fixed template 0) at the guard, investigated
        m.k = 2;
        m.plo = PnoiseNew(PtmplNoiseFixed(0), PosAi(pg->palo), pg->palo);
    }
    else
        m.k = 1;
    React(m);
}

// ---- 0xA0..0xA3: the hits ----

// orig: 0x00179328 (0xA0's Enter, the base of 0xA1..0xA3)
void JOBHIT::Enter()
{
    JOBMOVE::Enter();
    GUARD* pg = pguard;
    float rad;
    const glm::vec3 v = hit.vec;
    if (std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) > 1e-4f && hit.kind != 0x40000)  // (the 3D length, as 0x179328's vmul.xyz)
        rad = std::atan2(-v.y, -v.x);
    else
    {
        const glm::mat4 m = pg->palo->apxfRef[2] ? pg->palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
        rad = std::atan2(m[0][1], m[0][0]);  // (0x1A6318 twice: the facing)
    }
    SetMoveHeading(&pg->move, rad);
    if (std::getenv("SLY2N_HITDIR"))
    {
        const glm::vec3 pp = PosAi(pg->palo), ps = hit.psource ? PosAi(hit.psource) : pp;
        std::printf("hitdir: t %.3f guard %d %p kind 0x%X k %d vec %.1f %.1f rad %.3f locked %d heading %.3f src %d dir %.3f pos %.0f %.0f\n",
                    g_clock.t, pg->palo->oid, (void*)pg, hit.kind, pkinde ? pkinde->k : -9, v.x, v.y, rad, pg->move.fHeadingLocked,
                    St(pg).radHeading, hit.psource ? hit.psource->oid : -1, std::atan2(pp.y - ps.y, pp.x - ps.x), pp.x, pp.y);
    }
    // orig 0x1793F0..0x179460: a kind-2 hit lands its authored root distance behind the guard's
    // heading (+0x1A8); set the player target before its take-off event 0x22 can launch it.
    if (pkinde && pkinde->k == 2 && pkinde->pk2 && pkinde->pvState)
    {
        const float radBack = RadNormalize(St(pg).radHeading + 3.141592741f);
        const glm::vec3 offset = cmm::SetVectorCylind(radBack, pkinde->pk2->s1C, 0.0f);  // 0x11E1A0
        SetK2Target(static_cast<K2PLAY*>(pkinde->pvState), PosOfLo(pg->palo) + offset);  // 0x20C6F8
    }
    SetGuardResponse(pg, nullptr, nullptr, glm::vec3(0.0f)); // orig: 0x20AB68
    if (st == 0) LockJob(this);
    // TODO(ai): 0x156C80 (the hit's marker object +0x35C)
    tB0 = FLT_MAX;
    tB4 = FLT_MAX;
    pg->tF04 = 0.0f;
    if (pg->n334)
    {
        // struck by the current player (the hit's source or one of its parents: 0x1A6AF8) when that is Murray
        // (D_002F7900) or the hit is of kind 0x40000: the pad's rumble (0x187E28 kind 3, 0.15 s) and the camera's
        // shake 0.05 s later (tB0)
        LO* pcur = g_psw ? PloPoCurrent(g_psw) : nullptr;
        if (pcur && FLoInParentChain(hit.psource, pcur))
        {
            MURRAY* pm = PmurrayGlobal();
            if ((pm && pm->palo == pcur) || (hit.kind & 0x40000))
            {
                TriggerJoyRumbleRumk(3, 0.150000006f, "guard struck");
                tB0 = g_clock.t + 0.0500000045f;
            }
        }
        tB4 = g_clock.t + 0.13333334f;
    }
    PrintStepDebug(&St(pg));
}

// orig: 0x00179598
void JOBHIT::Exit()
{
    JOBMOVE::Exit();
    if (std::getenv("SLY2N_HITDIR"))
    {
        const glm::vec3 pp = PosAi(pguard->palo);
        std::printf("hitdir exit: t %.3f guard %d %p pos %.0f %.0f heading %.3f locked %d\n", g_clock.t, pguard->palo->oid, (void*)pguard,
                    pp.x, pp.y, St(pguard).radHeading, pguard->move.fHeadingLocked);
    }
    // (0x156C80(-1): the marker object off)
}

// orig: 0x001795E8
void JOBHIT::Update()
{
    JOBMOVE::Update();
    const float t = g_clock.t + 1e-4f;
    if (tB0 < t) tB0 = FLT_MAX;  // (0x14F750: the camera's shake: TODO(camera))
    if (tB4 < t)
    {
        tB4 = FLT_MAX;
        // the guard's body entries hit (mode 2); the physics' 0x1F7950 then sends its own contact (filter 0x20000,
        // state +0x149) and turns the mode off (build/act/hit: 8 frames after the cane's hit, 2369 -> 2377)
        SetAloEntryMode(pguard->palo, 2);
    }
}

// orig: 0x001796D0 (0xA1: the hit's entry from +0xA70 by its kind; the tasks' event 21; the detector: the source lost)
void JOBHIT::EnterKind(int iarray, int nEvent)
{
    GUARD* pg = pguard;
    KINDE* pe = &pg->aakinde[iarray][IbitLow(hit.kind)];
    pkinde = pe->k == -1 ? nullptr : pe;
    JOBHIT::Enter();
    if (g_psw && nEvent >= 0) BroadcastTaskEvent(g_psw, nEvent, pg->palo);
    if (pg->pdet)
        if (DETENTRY* pd = PdetentryFind(pg->pdet, hit.psource)) SetDetentryState(pg->pdet, pd, 2);
}
void JOBHIT1::Init()
{
    JOBMOVE::Init();
    nAnim = 5;
}
void JOBHIT1::Enter() { EnterKind(1, 21); }
// orig: 0x00179780 / 0x00179820 (0xA2: +0x8D0; the +0xFEC object shown while it lasts)
void JOBHIT2::Init()
{
    JOBMOVE::Init();
    nAnim = 6;
}
void JOBHIT2::Enter()
{
    EnterKind(0, 21);
    if (pguard->ploFEC && g_psw) AddLo(g_psw, pguard->ploFEC);
}
void JOBHIT2::Exit()
{
    JOBHIT::Exit();
    if (pguard->ploFEC && g_psw) RemoveLo(g_psw, pguard->ploFEC);
}
// orig: 0x00179868 / 0x00179898 / 0x00179A20 (0xA3: knocked out: +0xC10; TODO(ai): the drops 0x3D8..)
void JOBHIT3::Init()
{
    JOBMOVE::Init();
    nAnim = 7;
}
void JOBHIT3::Enter()
{
    EnterKind(2, -1);  // original 0x179898 selects the animation before its one-time death work
    if (pguard->f330) return;
    pguard->f330 = 1;
    if (g_psw) BroadcastTaskEvent(g_psw, hit.psource == PloPoCurrent(g_psw) ? 24 : 23, pguard->palo);
    // orig 0x179898 (continued): the knocked-out guard spills coins: +0x3D8 when set (InitLo 0x205EB8: -1), else half
    // (rounded up) of the coins still in its pocket (+0x404 entries of the proxy 0x2B3 from +0x488 to +0x484), from
    // its point +0x3E0 (0x1A6F00: local to world), the charms' count by +0x3F0 (default -200) (0x154E10)
    ALO* palo = pguard->palo;
    int n = static_cast<int32_t>(palo->U32Field(0, 0x3D8, uint32_t(-1)));
    if (n < 0)
    {
        const POCKET& p = PocketOf(palo);
        n = 0;
        for (int i = p.i; i < p.c && i < 8; ++i)
            if (p.ae[i].oid == 0x2B3) n += p.ae[i].c;
        n = (n + 1) / 2;
    }
    auto gfield = [palo](uint32_t ib) {
        const uint32_t u = palo->U32Field(0, ib, 0);
        float g;
        std::memcpy(&g, &u, 4);
        return g;
    };
    const glm::vec3 posLocal(gfield(0x3E0), gfield(0x3E4), gfield(0x3E8));
    const XF* pxfObj = PxfObj(palo);  // vt slot 31 (0x1F3FA0: the object record +0x60's world matrix)
    const glm::vec3 pos = glm::vec3((pxfObj ? pxfObj->matWorld : MatWorldOfAlo(palo)) * glm::vec4(posLocal, 1.0f));
    if (std::getenv("SLY2N_PICKUPPRINT")) std::printf("guard %d knocked out: %d coins\n", palo->oid, n);
    DropPickups(pos, n, static_cast<int32_t>(palo->U32Field(0, 0x3F0, uint32_t(-200))), palo);
    // TODO(physics): 0x134320 (the guard's +0xEC attachment pairs retested)
    // 0x153FF8: the detector forgets (every entry back to state 0 at now)
    if (pguard->pdet)
        for (DETENTRY& e : pguard->pdet->ae)
        {
            e.st = 0;
            e.t8 = g_clock.t;
        }
}

std::unique_ptr<AIJOB> PjobNewHit(CID cid)
{
    switch (cid)
    {
    case 0xA1: return std::make_unique<JOBHIT1>();
    case 0xA2: return std::make_unique<JOBHIT2>();
    case 0xA3: return std::make_unique<JOBHIT3>();
    default: return PjobNewNoise(cid);  // (ainoise.cpp)
    }
}

namespace
{
// orig: 0x0012F938: the first marker event with this value.
const ASEGEV* PeventLaunch(const KINDE* pe, int n)
{
    if (!pe || pe->k != 0 || !pe->plo) return nullptr;
    const ASEGD* pd = PasegdOf(static_cast<ASEG*>(pe->plo));
    if (!pd) return nullptr;
    for (const ASEGEV& ev : pd->aaev[1])
        if (ev.kind == 11 && ev.an[0] == n) return &ev;
    return nullptr;
}

void LaunchOwnerTarget(LO* plo, float dt, LAUNCHTARGET* pt)
{
    if (SLY* ps = dynamic_cast<SLY*>(PactorFromLo(plo)))
        GetSlyLaunchTarget(ps, dt, pt);  // orig 0x18DE80 (owner vt slot96)
    else if (plo && FCidDerivedFrom(plo->Cid(), 0x34))
    {
        // orig 0x143220, the actor's base slot96
        pt->fActive = 0;
        pt->sRadius = 150.0f;
        pt->dz = 75.0f;
        pt->vel = VelAloPos(static_cast<ALO*>(plo));
        pt->vel.z = 0.0f;
        pt->pos = PosAi(plo) + pt->vel * dt;
        // orig 0x1B9B70 (Murray's slot 96): tracked in states 0 / 1 / 9 / 10 / 11; the punch (11) aims at where it ends
        // (+0x1830), standing still; the radius 200
        if (MURRAY* pm = PmurrayFromLo(plo))
        {
            switch (pm->state)
            {
            case 11:
                pt->fActive = 1;
                pt->pos = glm::vec3(pm->pos1830);
                pt->vel = glm::vec3(0.0f);
                break;
            case 0: case 1: case 9: case 10: pt->fActive = 1; break;
            default: break;
            }
            pt->sRadius = 200.0f;
        }
    }
}

void SeekLaunchMarker(JOBB1* pj, int marker)
{
    if (const ASEGEV* ev = PeventLaunch(pj->pkinde, marker))
        if (ASEGA* pa = PasegaOfKinde(pj->pkinde)) SetAsegaTime(pa, 0, ev->t, 1.0f);
}
}  // namespace

// orig: 0x0017A9D0
void JOBB1::Init()
{
    JOBMOVE::Init();
    nAnim = 0x17;
    stLaunch = -1;
}

// orig: 0x0017AA08
void JOBB1::Enter()
{
    JOBMOVE::Enter();
    SetGuardResponse(pguard, nullptr, nullptr, glm::vec3(0.0f)); // orig: 0x20AB68
    // 0x20B210: terminate a navigation-link move. Those movement kinds are not yet implemented natively.
    DetachGuardMove(pguard);  // 0x20B210
    SetState(0);
    LockJob(this);
    // orig 0x154038: mark the launching owner lost in the detector, when it has an entry.
    if (pguard->pdet)
        if (DETENTRY* pe = PdetentryFind(pguard->pdet, ploOwner)) SetDetentryState(pguard->pdet, pe, 2);
}

// orig: 0x0017AA78
void JOBB1::GetTarget(const LAUNCHTARGET& target, glm::vec3* ppos, glm::vec3* pvel)
{
    glm::vec3 dir = PosAi(pguard->palo) - PosAi(ploOwner);
    dir -= glm::vec3(0.0f, 0.0f, 1.0f) * glm::dot(dir, glm::vec3(0.0f, 0.0f, 1.0f));
    const float length = std::sqrt(glm::dot(dir, dir));
    dir = length < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f) : dir * (1.0f / length);
    *ppos = target.pos + dir * target.sRadius + glm::vec3(0.0f, 0.0f, 1.0f) * target.dz;
    *pvel = target.vel + glm::vec3(0.0f, 0.0f, 1.0f) * -1500.0f;  // ELF2BE6C4
}

// orig: 0x0017ABC8
void JOBB1::UpdateTarget()
{
    if (!fTrack || !pctrlTarget) return;
    LAUNCHTARGET target;
    LaunchOwnerTarget(ploOwner, pctrlTarget->tArrive - g_clock.t, &target);
    if (!target.fActive)
    {
        fTrack = 0;
        return;
    }
    glm::vec3 pos, vel;
    GetTarget(target, &pos, &vel);
    XF* pxf = PxfPos(pguard->palo);
    // orig 0x22FC48/0x22FCC8 then 0x12E0F0: update the held arrival target without restarting its time.
    pctrlTarget->pos = glm::vec4(PosConvertXfs(nullptr, pxf ? pxf->pxfRef : nullptr, pos), 1.0f);
    pctrlTarget->vel = glm::vec4(VecConvertXfs(nullptr, pxf ? pxf->pxfRef : nullptr, vel), 0.0f);
    if (pxf && pxf->pctrlFirst == pctrlTarget)
    {
        pxf->aseg[0] = pctrlTarget->pos;
        pxf->aseg[1] = pctrlTarget->vel;
    }
}

// orig: 0x0017AC90
void JOBB1::Update()
{
    JOBMOVE::Update();
    const glm::vec3 velocity = VelAloPos(pguard->palo);
    int saved;
    do
    {
        saved = stLaunch;
        switch (stLaunch)
        {
        case 0: case 1:
        {
            XF* pxf = PxfPos(pguard->palo);
            if ((stLaunch == 1 && FGuardFloor(pguard)) ||
                (pxf && pctrlTarget && (pxf->pctrlFirst != pctrlTarget || pctrlTarget->tArrive < g_clock.t)))
                SetState(2);
            else if (velocity.z < 0.0f && stLaunch == 0) SetState(1);
            else UpdateTarget();
            break;
        }
        case 2:
            if (FGuardFloor(pguard)) SetState(3);
            break;
        case 4: case 5:
        {
            const glm::vec3 pos = PosAi(pguard->palo);
            const glm::vec3 currentVel = VelAloPos(pguard->palo);
            ACTOR* pa = PactorFromLo(ploOwner);
            SLY* ps = dynamic_cast<SLY*>(pa);
            ALO* ppart = ps && ps->plo19A4 && FCidDerivedFrom(ps->plo19A4->Cid(), 0x30) ? static_cast<ALO*>(ps->plo19A4) : nullptr;
            XF* pxf = ppart ? PxfObj(ppart) : nullptr;
            if (!pxf) break;
            glm::mat4 predicted(1.0f);
            PredictXfChain(pxf, nullptr, g_clock.dt, &predicted, nullptr);  // original0x2309E8
            glm::vec3 target(predicted[3]);
            target.z += 35.0f;  // ELF2BE6C8
            glm::vec3 nextVel = (target - PosAi(pguard->palo)) * (1.0f / g_clock.dt);
            if (stLaunch == 4)
            {
                if (!(pos.z < target.z) || 0.25f < g_clock.t - tLaunch)
                {
                    SetState(5);
                    break;
                }
                nextVel.z = currentVel.z;
            }
            SetAloVelocityTo(pguard->palo, nextVel);
            break;
        }
        case 6:
            if (FGuardFloor(pguard)) SetState(-1);
            break;
        default: break;
        }
    } while (saved != stLaunch);
}

// orig: 0x0017AF30
int JOBB1::Message(int msg, void* pm)
{
    if (JOBMOVE::Message(msg, pm)) return 1;
    if (msg == 9)
    {
        const auto* pe = static_cast<const ASEGMSG9*>(pm);
        if (pkinde && PasegaOfKinde(pkinde) == pe->pasega && pe->n == 0x3D) nLaunch = 0;
        return 0;
    }
    if (msg != 12) return 0;
    const auto* hit = static_cast<const CTMSG*>(pm);
    if (hit->grfFilter != 0x200000 || !(hit->grfAccepted & 8) || stLaunch == 0) return 0;
    if (FKindeDone(pkinde))
    {
        StopKinde(pkinde);
        PlayKinde(pkinde, pguard, pkinde->grf | 1, 0.0f);
    }
    ++nLaunch;
    int index = std::min(nLaunch, 4);
    float t = 0.0f;
    while (index > 0)
    {
        if (const ASEGEV* ev = PeventLaunch(pkinde, 0x37 + index))
        {
            t = ev->t;
            break;
        }
        --index;
    }
    if (ASEGA* pa = PasegaOfKinde(pkinde)) SetAsegaTime(pa, 0, t, 1.0f);
    SetState(0);
    // TODO(media): 0x15E938/slot51 sound object 0x3E+index, message5 at the guard's position.
    const int hp = pguard->n33C + hit->nState;
    pguard->n33C = hp > 4 ? hp : 5;
    // 0x156698(SW +0x4028: the fixed template 708, the guard's place, the guard)
    if (NOISETMPL* pt = PtmplNoiseFixed(2)) PnoiseNew(pt, PosAi(pguard->palo), pguard->palo);
    return 1;
}

// orig: 0x0017B178
void JOBB1::Exit()
{
    SetState(-1);
    JOBMOVE::Exit();
}

// orig: 0x0017B1A8
void JOBB1::SetState(int n)
{
    if (n == stLaunch) return;
    ALO* palo = pguard->palo;
    const int old = stLaunch;
    if (old == -1)
    {
        if (SOST* ps = Psost(palo)) SetAloMass(palo, ps->m * 0.1f);  // ELF2BE6FC
    }
    else if (old == 3 && n != -1)
    {
        if (ASEGA* pa = PasegaOfKinde(pkinde)) SetAsegaTime(pa, 0, 0.0f, 1.0f);
    }
    if ((old == 0 || old == 1) && n != 0 && n != 1)
    {
        CTRL* pc = pctrlTarget;
        DeleteCtrl(&pc);
        pctrlTarget = nullptr;
        if (PxfPos(palo)) SetXfMode(palo, PxfPos(palo), 2);
        if (PxfRot(palo)) SetXfMode(palo, PxfRot(palo), 2);
    }
    const bool oldContact = old == 0 || old == 4 || old == 5;
    const bool newContact = n == 0 || n == 4 || n == 5;
    stLaunch = n;
    if (oldContact != newContact)
    {
        RecalcSwOxfFilterForObject(palo);
        DetachAloContactsAll(palo);
    }
    stLaunch = n;
    tLaunch = g_clock.t;
    if (std::getenv("SLY2N_HITDEBUG"))
    {
        const glm::vec3 pos = PosAi(palo), vel = VelAloPos(palo);
        std::printf("launch state: t %.3f guard %d owner %d state %d->%d move %d hp %d pos %.3f %.3f %.3f vel %.3f %.3f %.3f\n",
                    g_clock.t, palo->oid, ploOwner ? ploOwner->oid : -1, old, n, st, pguard->n33C,
                    pos.x, pos.y, pos.z, vel.x, vel.y, vel.z);
    }
    switch (n)
    {
    case -1:
        if (SOST* ps = Psost(palo)) SetAloMass(palo, ps->m / 0.1f);
        SetMoveState(1);
        break;
    case 0:
    {
        LAUNCHTARGET target;
        LaunchOwnerTarget(ploOwner, 0.6f, &target);  // ELF2BE700
        glm::vec3 pos, vel;
        GetTarget(target, &pos, &vel);
        const float scale = 1.0f / 0.6f;  // ELF2BE704 / ELF2BE700
        const glm::vec3 impulse = (pos - PosAi(palo)) * scale + glm::vec3(0.0f, 0.0f, 1750.0f);  // ELF2BE708
        SetAloVelocityTo(palo, impulse);
        XF* pxf = PxfPos(palo);
        if (!pctrlTarget && pxf)
        {
            const float t = g_clock.t + 0.6f;
            const glm::vec3 localPos = PosConvertXfs(nullptr, pxf->pxfRef, pos);
            const glm::vec3 localVel = VecConvertXfs(nullptr, pxf->pxfRef, vel);
            pctrlTarget = PctrlNewTarget(palo, pxf, 0, t, glm::vec4(localPos, 1.0f), glm::vec4(localVel, 0.0f));
        }
        ACTOR* pa = PactorFromLo(ploOwner);
        fTrack = target.fActive && pa && pa->plo10DC == palo;
        // the guard thrown by the current player (its +0x334 set): the pad's rumble (0x187E28 kind 3, 0.15 s)
        if (pguard->n334 && g_psw && ploOwner && ploOwner == PloPoCurrent(g_psw)) TriggerJoyRumbleRumk(3, 0.15f, "guard launched");
        break;
    }
    case 3: SeekLaunchMarker(this, 0x3C); break;
    case 4: SeekLaunchMarker(this, 0x173); break;
    default: break;
    }
}

// orig: 0x0017B578
int JOBB1::State() { return stLaunch == 3 ? 7 : 1; }

// ---- 0xB0: held ----

// orig: 0x0017A6F8
void JOBB0::Init()
{
    AIJOB::Init();
    AddGuardState(pguard, 0x16);  // 0x209890
    stHeld = -1;
}

// orig: 0x0017A730
void JOBB0::Enter()
{
    AIJOB::Enter();
    SetState(0);
    DetachGuardMove(pguard);     // 0x20B210: its kind's mover reset
    SetGuardResponse(pguard, nullptr, nullptr, glm::vec3(0.0f)); // orig: 0x20AB68
    DetachAloContactsAll(pguard->palo);  // 0x1F4E28
    LockJob(this);
    // 0x153FF8: the detector forgets (every entry back to state 0 at now)
    if (DETECTOR* pdet = pguard->pdet)
    {
        for (DETENTRY& e : pdet->ae)
        {
            e.st = 0;
            e.t8 = g_clock.t;
        }
        // 0x154038: the holder's entry state 2
        if (ploHolder)
            if (DETENTRY* pe = PdetentryFind(pdet, ploHolder)) SetDetentryState(pdet, pe, 2);
    }
}

// orig: 0x0017A7B8
void JOBB0::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    RemoveGuardState(pguard, 0x16);  // 0x209970
}

// orig: 0x0017A7F8
void JOBB0::Update()
{
    AIJOB::Update();
    int s;
    do
    {
        s = stHeld;
        if (s < 4 && s >= 2 && FGuardFloor(pguard)) SetState(-1);  // (the guard's STEP +0x208)
    } while (s != stHeld);
    if (s == 1 || s == 4)
        if (g_clock.t - tNoise > 1.0f)  // D_002BE6C0
        {
            tNoise = g_clock.t;
            // 0x156698(SW +0x4020: the fixed template 706, its place, the guard)'s kind +0x54 = 3
            if (NOISE* pn = PnoiseNew(PtmplNoiseFixed(0), PosAi(pguard->palo), pguard->palo)) pn->nKind = 3;
        }
}

// orig: 0x0017A8C8
void JOBB0::SetState(int n)
{
    if (stHeld == n) return;
    EndChildJob(&pchild);  // 0x1EF0D8(+0x18)
    stHeld = n;
    tHeld = g_clock.t;
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("held state: t %.3f guard %d holder %d state %d\n", g_clock.t, pguard->palo->oid, ploHolder ? ploHolder->oid : -1, n);
    switch (n)
    {
    case 0: case 1: case 2: case 3: case 4:
    {
        static const int s_anim[5] = {18, 19, 20, 21, 19};
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9D));  // 0x205438
        o->nAnim = s_anim[n];
        o->Enter();  // vt slot 8
        pguard->f32C = static_cast<unsigned>(stHeld) < 2;
        break;
    }
    case -1:
        UnlockJob(this);  // 0x1EF038
        pguard->f32C = 0;
        break;
    default: break;
    }
}
