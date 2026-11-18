// See airide3.h. Ported from the Sly 3 decomp (orig3 addresses in the comments).

#include "airide3.h"
#include "activate.h"

#include "aijobs.h"
#include "aihit.h"
#include "aipursuit.h"
#include "animfind.h"
#include "alo.h"
#include "clock.h"
#include "disc.h"
#include "asega.h"
#include "guard.h"
#include "lo.h"
#include "po.h"
#include "step.h"
#include "stepphys.h"
#include "so.h"
#include "sw.h"
#include "game/sly3.h"
#include "game/task3.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

void (*g_pfnRiderOff3)(LO* ploRider) = nullptr;

namespace
{
bool FPrint() { static const bool s_f = std::getenv("SLY3_RIDEPRINT") != nullptr; return s_f; }

// the links' riders (orig3: the link's +0x178)
std::unordered_map<const LO*, LO*> s_mprider;

constexpr uint32_t BTN3_Triangle = 0x10, BTN3_Cross = 0x40, BTN3_Square = 0x80;

// orig3 D_00317530: the feet's forward in the heading's frame (aicarm3.cpp's)
glm::vec3 VecForward()
{
    static const glm::vec3 s_v = [] {
        glm::vec3 v;
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t u = g_disc.U32Elf(0x317530 + 4u * uint32_t(i));
            std::memcpy(&v[i], &u, 4);
        }
        return v;
    }();
    return s_v;
}

// the guard's mode +0x11CC (option 495, Sly 3 EOPID 368)
int NMode(const GUARD* pg) { return GuardField3(pg, 0x11CC, 0); }

// the child (a move) over (orig3 0x238AD0: natively the move's own state)
bool FChildDone(const AIJOB* pc)
{
    if (!pc) return true;
    if (auto* p = dynamic_cast<const JOBMOVE*>(pc)) return p->st == 1;
    return false;
}

JOBRIDE3* PjobRide(GUARD* pg)
{
    if (!pg) return nullptr;
    for (const auto& pj : pg->stack.ajob)
        if (pj->cid == CID_JOBRIDE3) return static_cast<JOBRIDE3*>(pj.get());
    return nullptr;
}

// the position record's world place (kind2.cpp PosRecord: the kind-2 player's frame), else the object's
glm::vec3 PosRecordRide(ALO* palo)
{
    if (XF* pxf = PxfPos(palo)) return glm::vec3(pxf->matWorld[3]);
    return PosAloCenter(palo);
}

// orig3 D_003209E0: the world's gravity (0, 0, -2275), the 0xCB kinds' +0x60 (scaled by the kind's 1 / r^2: 1 here)
constexpr float c_gGravity3 = -2275.0f;

// the guard action +0x11D0 (option 496, an object of Sly 3's class 0x8E; null: none)
LO* PloRideAction(GUARD* pg)
{
    if (!pg || !pg->palo) return nullptr;
    const int oid = GuardField3(pg, 0x11D0, -1);
    return oid == -1 || oid == 0 ? nullptr : PloFindSwNearest(pg->palo, oid);
}

// the stick's world heading in the rider's view (orig3 0x168108 on the rider's player, 0x1F4A80: the camera's x row
// turned by the stick's angle; the pad controller's radJoy already is), else rad
float RadRideStick(const PADCTL* ppc, float rad) { return ppc && ppc->uJoy > 0.0f ? ppc->radJoy : rad; }
}  // namespace

void SetRideRider3(LO* plink, LO* ploRider)
{
    if (!plink) return;
    if (ploRider) s_mprider[plink] = ploRider;
    else s_mprider.erase(plink);
    if (FPrint()) std::printf("ride3: link %d rider %d\n", plink->oid, ploRider ? ploRider->oid : -1);
}

LO* PloRideLink3(GUARD* pg)
{
    if (!pg || !pg->palo) return nullptr;
    const int oid = GuardField3(pg, 0x11C8, -1);
    return oid == -1 ? nullptr : PloFindSwNearest(pg->palo, oid);
}

// (debug: SLY3_RIDETEST=oid: from t 3 that guard is ridden by the current player, as by the Guru, until its Triangle)
static bool s_fRideTestDone = false;
static LO* s_ploRideTest = nullptr;

LO* PloGuardRider3(GUARD* pg)
{
    if (static const int s_oid = std::getenv("SLY3_RIDETEST") ? std::atoi(std::getenv("SLY3_RIDETEST")) : 0;
        s_oid && pg && pg->palo && pg->palo->oid == s_oid && !s_fRideTestDone && g_clock.t > 3.0f && g_psw)
    {
        if (!s_ploRideTest && PloPoCurrent(g_psw) != pg->palo) s_ploRideTest = PloPoCurrent(g_psw);
        return s_ploRideTest;
    }
    if (s_mprider.empty()) return nullptr;
    LO* plink = PloRideLink3(pg);
    if (!plink) return nullptr;
    const auto it = s_mprider.find(plink);
    return it == s_mprider.end() ? nullptr : it->second;
}

void ReleaseGuardRide3(LO* ploGuard)
{
    if (JOBRIDE3* pj = PjobRide(PguardFromLo(ploGuard))) pj->Release();
}

void ResetRide3() { s_mprider.clear(); }

PADCTL* JOBRIDE3::Ppad() const { return pguard ? PpadctlOfSo3(pguard->palo) : nullptr; }

// orig3: 0x1F5AE0: the guard state 0x41 (its ride's kind entries), +0x30 -1, the guard's +0x164 kept
void JOBRIDE3::Init()
{
    AIJOB::Init();
    st = -1;
    ploRider = nullptr;
    if (!pguard) return;
    AddGuardState(pguard, IstateGuard2FromIstate3(0x41));
    u164 = pguard->palo->U32Field3(0, 0x164, 0);
}

// orig3: 0x1F5B28: the broadcast 0x1A (the guard), (the detector target +0x10D0: not ported), the rider's pad, state 0
void JOBRIDE3::Enter()
{
    AIJOB::Enter();
    if (!pguard) return;
    Broadcast3(0x1A, pguard->palo);
    GiveRiderPad();
    SetState(0);
    if (FPrint())
        std::printf("ride3: guard %d ridden by %d (mode %d, +0x1144 0x%X: jump %d, action +0x11D0 %d)\n", pguard->palo->oid,
                    ploRider ? ploRider->oid : -1, NMode(pguard), GuardField3(pguard, 0x1144, 0),
                    (GuardField3(pguard, 0x1144, 0) & 0x10) != 0, GuardField3(pguard, 0x11D0, -1));
}

// orig3: 0x1F5B88
void JOBRIDE3::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    RiderOff();
    if (!pguard) return;
    pguard->palo->SetField3(0, 0x164, u164);
    Broadcast3(0x1B, pguard->palo);
    RemoveGuardState(pguard, IstateGuard2FromIstate3(0x41));
    fSteer = fHeading = false;
}

// orig3: 0x1F5C58: the rider's player (its +0x130) drives the guard (0x1F4B00), the ground watcher off (0x259D10 on
// +0x4B0); (the rider's +0x164 copied, mode 3's +0x1B8: not ported)
void JOBRIDE3::GiveRiderPad()
{
    if (!pguard || !ploRider || !g_psw) return;
    ALO* palo = pguard->palo;
    palo->SetField3(0, 0x130, ploRider->U32Field3(0, 0x130, 0));
    if (PloPoCurrent(g_psw) == ploRider) SetPoCurrent(g_psw, palo);
    if (NPoEnabled(palo) != 1) EnablePo(g_psw, palo, 1);
    ++pguard->watchA.cOff3;
    pguard->watchA.fOn = 0;
}

// orig3: 0x1F5CC8: the rider told to get off (0x22D318), whose state change releases the ride (0x22D2B0 -> 0x1F5CF0)
void JOBRIDE3::RiderOff()
{
    LO* pr = ploRider;
    if (!pr) return;
    if (g_pfnRiderOff3) g_pfnRiderOff3(pr);
    else
    {
        // (no rider code: the ride released here, the link's rider cleared as the rider's state change does)
        Release();
        if (LO* plink = PloRideLink3(pguard)) SetRideRider3(plink, nullptr);
        if (pr == s_ploRideTest) s_fRideTestDone = true;
    }
}

// orig3: 0x1F5CF0: the pad back to the rider (0x1F4B00 with the guard's player index), the watcher back on (0x259D50),
// in mode 2 the job over
void JOBRIDE3::Release()
{
    LO* pr = ploRider;
    if (!pr || !pguard) return;
    ploRider = nullptr;
    ALO* palo = pguard->palo;
    if (g_psw)
    {
        if (PloPoCurrent(g_psw) == palo) SetPoCurrent(g_psw, pr);
        if (NPoEnabled(pr) != 1) EnablePo(g_psw, pr, 1);
        if (NPoEnabled(palo) == 1) EnablePo(g_psw, palo, 0);
    }
    palo->SetField3(0, 0x130, 0xFFFFFFFFu);
    if (pguard->watchA.cOff3 > 0 && --pguard->watchA.cOff3 == 0) pguard->watchA.fOn = 1;
    if (NMode(pguard) == 2) SetState(-1);
    fSteer = fHeading = false;
    if (FPrint()) std::printf("ride3: guard %d released by %d\n", palo->oid, pr->oid);
}

// orig3: 0x1F6148: the old move ended, the new state's (Sly 3 jobs: 0xC9 JOBMOVE, 0xCA stand, 0xCB jumps, 0xCC the
// attack, 0xCE a hit); -1: the job over (0x238AB8: the reaction told its child is done)
void JOBRIDE3::SetState(int n)
{
    if (st == n) return;
    EndChildJob(&pchild);
    EndJumpAttack();
    st = n;
    tSt = g_clock.t;
    if (!pguard) return;
    auto move = [&](CID cid, int nAnim3) {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, cid));
        if (nAnim3 >= 0) o->nAnim = IstateGuard2FromIstate3(nAnim3);
        o->Enter();
    };
    switch (n)
    {
    case 0: move(0x9D, 0x11); break;   // (0xC9 with +0x3C 0x11: getting on)
    case 1: move(0x9E, -1); break;     // (0xCA)
    case 2: move(0x9D, 0xE); break;    // (0xC9 with 0xE: the walk)
    case 6: move(0x9D, 0x11); break;   // (0xCE with the hit: natively the reaction's own hits)
    case 3: Jump(); break;   // (Sly 3's 0xCB with +0xCC 5)
    case 5: Attack(); break;  // (Sly 3's 0xCC with +0x34 the action's kind entry)
    case 4:
    {
        // TODO(sly3 ai): Sly 3's 0xCB kind 4 (a hit's launch toward the message's point, 0x212480); natively the stand
        static bool s_fWarned = false;
        if (!s_fWarned) std::fprintf(stderr, "ride3: state 4 (the hit's launch) not ported: standing\n");
        s_fWarned = true;
        move(0x9E, -1);
        break;
    }
    default: break;
    }
    if (FPrint()) std::printf("ride3: guard %d state %d (t %.2f)\n", pguard->palo->oid, n, g_clock.t);
}

// orig3: 0x1F5BF8: 15 (a hit, in mode 1) -> 6; 8 (a launch) -> 4
int JOBRIDE3::Message(int msg, void* pm)
{
    if (msg == 15 && pguard && NMode(pguard) == 1) { SetState(6); return 1; }
    return AIJOB::Message(msg, pm);
}

// orig3: 0x1F5D70
void JOBRIDE3::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    if (!pg) return;
    PADCTL* ppc = Ppad();
    // (0x200 (L3 / select): 0x1528E0 on the rider: a line and the camera's reset; not ported)
    int old;
    int cguard = 0;
    do
    {
        old = st;
        switch (old)
        {
        case 0:
            if (FChildDone(pchild)) SetState(1);
            break;
        case 1: case 2:
        {
            // (0x1F6300: a wall ahead -> the deferred call D_00325490: not ported)
            if (!ploRider && PloGuardRider3(pg))
            {
                ploRider = PloGuardRider3(pg);
                GiveRiderPad();
                ppc = Ppad();
                break;
            }
            const int mode = NMode(pg);
            if (mode != 1 && ppc && GrfPadctlTakePressed(ppc, BTN3_Triangle, 0.15f)) { RiderOff(); break; }
            if ((GuardField3(pg, 0x1144, 0) & 0x10) && ppc && GrfPadctlTakePressed(ppc, BTN3_Cross, 0.15f) && FGuardFloor(pg))
            {
                SetState(3);
                break;
            }
            if (LO* pact = PloRideAction(pg); pact && !GuardField3(pg, 0x107C, 0) && ppc && GrfPadctlTakePressed(ppc, BTN3_Square, 0.15f))
            {
                // (an action with +0x68 (its concurrent response, option 358 natively ACTIONOBJ::f78) is the guard's
                // response 0x256828 instead of the attack job)
                ACTIONOBJ* pa = PactionOf(pact, pg);
                if (pa && pa->f78) SetGuardResponse(pg, pa, nullptr, glm::vec3(0.0f));
                else SetState(5);
                break;
            }
            if (mode < 2 || mode == 3) SetState(2);
            else if (!ppc || ppc->uJoy == 0.0f) SetState(1);
            else SetState(2);
            break;
        }
        case 3:
            // orig3 0x132A50 state 1 (kind 5: no fall state, gE4 FLT_MAX): on the ground 0.15 s after the take-off ->
            // 0x132BC0 (kinds 3..6: 4, the end: 0x131610(1), the job done) -> 0x1F6148(2); (5 s in the air: the same)
            if ((g_clock.t - tSt > 0.15f && FGuardFloor(pg)) || g_clock.t - tSt > 5.0f) SetState(2);
            else if (NMode(pg) != 1 && ppc && GrfPadctlTakePressed(ppc, BTN3_Triangle, 0.15f)) RiderOff();
            break;
        case 4:
            if (FChildDone(pchild)) SetState(2);
            else if (NMode(pg) != 1 && ppc && GrfPadctlTakePressed(ppc, BTN3_Triangle, 0.15f)) RiderOff();
            break;
        case 5: case 6:
            if (FChildDone(pchild)) SetState(2);
            break;
        default: break;
        }
    } while (st != old && ++cguard < 8);
    // 0x257D78 (states 2, 5, 6, with a rider): the heading to the stick in the player's view (mode 3's swim: not ported);
    // the walk's feet by the stick's deflection
    fSteer = fHeading = false;
    if (st == 3)
    {
        // orig3 0x132438 (0xCB states 1 / 2, kind 5): the guard turned to its velocity once it moves (over 200, else its
        // heading kept); the feet's target velocity (+0x1A0) toward the stick in the rider's view (the heading's when the
        // stick is still) at the jump's ground speed, in modes 0 / 1 / 3 always the full speed, else by the stick's
        // deflection (+0x1A0 is minus the velocity in the heading's frame: natively vecVel's sign too). The feet act through
        // the floor contacts only, so in the air the take-off's velocity carries the guard (the reference's 119107: a
        // straight flight at 1091 a second)
        ALO* palo = pg->palo;
        if (fJumpGravity) SetAloGravity(palo, glm::vec3(0.0f, 0.0f, gJumpGravity));   // (the kind-2 player's end restores its own)
        if (static const bool s_f = std::getenv("SLY3_RIDEJUMPPRINT") != nullptr; s_f)
        {
            const SOST* ps = Psost(palo);
            const glm::vec3 vv = VelAloPos(palo);
            std::printf("ride3 jump: t %.3f z %.1f vz %.1f vxy %.0f gravity %.1f floor %d\n", g_clock.t, PosRecordRide(palo).z, vv.z,
                        std::hypot(vv.x, vv.y), ps ? ps->dvGravity.z : 0.0f, int(FGuardFloor(pg)));
        }
        const glm::vec3 v = VelAloPos(palo);
        const float radCur = St(pg).radHeading;
        const float rad = std::hypot(v.x, v.y) > 200.0f ? std::atan2(v.y, v.x) : radCur;
        const int mode = NMode(pg);
        const float u = (mode == 0 || mode == 1 || mode == 3) ? 1.0f : (ppc ? ppc->uJoy : 0.0f);
        const float radGo = RadRideStick(ppc, rad);
        const glm::vec3 w(std::cos(radGo) * u * sJumpSpeed, std::sin(radGo) * u * sJumpSpeed, 0.0f);
        fHeading = true;
        radHeading = rad;
        fSteer = true;
        uStick = 1.0f;
        vecJumpFeet = -(glm::transpose(St(pg).matHeading) * w);
    }
    else if (ploRider && ppc && (st == 2 || st == 5 || st == 6))
    {
        if (ppc->uJoy > 0.0f)
        {
            fHeading = true;
            radHeading = ppc->radJoy;
        }
        fSteer = st == 2;
        uStick = ppc->uJoy;
    }
    else if (st == 2)
    {
        // (no rider: the walk's feet still; natively the walk's root velocity would carry it on unsteered)
        fSteer = true;
        uStick = 0.0f;
    }
}

// orig3 0x131E10 / 0x132C58(1) with +0xCC 5 (the ride's jump): the move of the jump (state index 0x15, Sly 3's 0xCB
// Init 0x131D78) with its kind entry (for the ride: the entry {0x15, 0x41}, a kind-2 clip), the take-off at once (the
// clip set to its take-off marker 0x3D), the gravity the kind's clip gives (+0x60, 0x258FD0: natively KIND2::g28; the
// reference's guard 119107: -6151) through the flight (0x23ED58, back at the end), the vertical velocity at least what
// reaches the kind object's height +0x7C above the ground (0x250418: sqrt(2 |g| h)), the flight's time sqrt(-8 h / g),
// its ground speed +0x80 / that time (+0xF8); the ground watcher off (0x259D10) until the end. +0x7C / +0x80 are the
// kind-2 object's (Sly 3 class 0x95, its InitLo 0x258F38: 500 / 800; no option writes them: the reference's 119107
// entries hold 500 / 800 at vblank 5300); natively the kind entry holds the clip, so those defaults
void JOBRIDE3::Jump()
{
    GUARD* pg = pguard;
    ALO* palo = pg->palo;
    auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
    o->nAnim = IstateGuard2FromIstate3(0x15);
    o->Enter();
    const float h = 500.0f, d = 800.0f;
    K2PLAY* pk = o->pkinde && o->pkinde->k == 2 ? static_cast<K2PLAY*>(o->pkinde->pvState) : nullptr;
    const KIND2* pt = pk ? pk->ptuning : nullptr;
    // (0x132C58: the kind's +0x60 times r^2, r the playing clip's rate +0x40: the reference's 119107 flies at 1091 a
    // second, 800 / sqrt(8 x 500 / (6151 x 1.1^2)); the kind-2 player's own accel is the same, kind2.cpp)
    const float rate = pk && pk->pasega ? pk->pasega->svt : 1.0f;
    gJumpGravity = pt && pt->g28 < 0.0f ? pt->g28 * rate * rate : c_gGravity3;
    const float t = std::sqrt(-8.0f * h / gJumpGravity);
    sJumpSpeed = t > 0.0f ? d / t : 0.0f;
    PADCTL* ppc = Ppad();
    const int mode = NMode(pg);
    const float u = (mode == 0 || mode == 1 || mode == 3) ? 1.0f : (ppc ? ppc->uJoy : 0.0f);
    const float radGo = RadRideStick(ppc, St(pg).radHeading);
    glm::vec3 v = VelAloPos(palo);
    v.x = std::cos(radGo) * u * sJumpSpeed;
    v.y = std::sin(radGo) * u * sJumpSpeed;
    if (pk && pt && pk->st == 0)
    {
        // (the kind-2 player straight to its take-off: its clip at the marker 0x3D, its launch state; its own launch
        // velocity is replaced below, its target where the flight's ground speed carries the guard meanwhile)
        pk->fBallistic = 0;
        SetK2Target(pk, PosRecordRide(palo) + glm::vec3(v.x, v.y, 0.0f) * t);
        if (pk->pasega) SetAsegaTime(pk->pasega, 0, pt->t22, 1.0f);
        SetK2State(pk, 1);
        gJumpGravityPrev = pk->dvGravityPrev;
    }
    else if (SOST* ps = Psost(palo)) gJumpGravityPrev = ps->dvGravity;
    v.z = std::max(VelAloPos(palo).z, std::sqrt(std::max(0.0f, -2.0f * gJumpGravity * h)));
    SetAloVelocityTo(palo, v);
    SetAloGravity(palo, glm::vec3(0.0f, 0.0f, gJumpGravity));
    fJumpGravity = true;
    ++pg->watchA.cOff3;
    pg->watchA.fOn = 0;
    fJumpWatchOff = true;
    if (FPrint())
        std::printf("ride3: guard %d jumps: kind %d oid %d gravity %.0f height %.0f distance %.0f time %.2f speed %.0f vz %.0f\n", palo->oid,
                    o->pkinde ? o->pkinde->k : -9, o->pkinde ? o->pkinde->oid : -1, gJumpGravity, h, d, t, sJumpSpeed, v.z);
}

// orig3 0x1A3860(0) (Sly 3's 0xCC, Init 0x1A3708: the guard state 3 meanwhile; the guard's +0x107C set): the action's
// kind entry (+0x34 = the action +0x11D0's +0xB0) played by a move (0xC9; a kind-2 entry: 0xCB mode 2 at the target,
// natively the move too); its end: the job done (0x1A39E0)
void JOBRIDE3::Attack()
{
    GUARD* pg = pguard;
    LO* pact = PloRideAction(pg);
    ACTIONOBJ* pa = pact ? PactionOf(pact, pg) : nullptr;
    AddGuardState(pg, IstateGuard2FromIstate3(3));
    pg->palo->SetField3(0, 0x107C, 1);
    fAttack107C = true;
    auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
    o->pkinde = pa && pa->kinde.k >= 0 ? &pa->kinde : nullptr;
    o->Enter();
    // (s3fix-ai3, AUDIT4 AI-18) a kind-2 entry: Sly 3 pushes 0xCB with +0x120 1 / +0xCC 2 and the subject's position
    // (0x132930: the subject +0x30, the action, its vt slot 36's position) as the leap's goal: the kind-2 player's
    // target the action's position (was the move's own leap goal)
    if (o->pkinde && o->pkinde->k == 2 && o->pkinde->pvState && pact)
        SetK2Target(static_cast<K2PLAY*>(o->pkinde->pvState), PosLoWorld(pact));
    if (FPrint())
        std::printf("ride3: guard %d attacks: action %d kind %d oid %d\n", pg->palo->oid, pact ? pact->oid : -1,
                    o->pkinde ? o->pkinde->k : -9, o->pkinde ? o->pkinde->oid : -1);
}

// the jump's / attack's holds released when its state is left (0xCB 0x132C58(4): the watcher back on 0x259D50; 0xCC
// Exit 0x1A3770: the guard state 3 off, 0x1A3860(-1): +0x107C clear)
void JOBRIDE3::EndJumpAttack()
{
    if (!pguard) return;
    if (fJumpGravity)
    {
        fJumpGravity = false;
        SetAloGravity(pguard->palo, gJumpGravityPrev);   // (0x132C58(4): 0x23ED58 with the gravity kept at the start, +0x90)
    }
    if (fJumpWatchOff)
    {
        fJumpWatchOff = false;
        if (pguard->watchA.cOff3 > 0 && --pguard->watchA.cOff3 == 0) pguard->watchA.fOn = 1;
    }
    if (fAttack107C)
    {
        fAttack107C = false;
        pguard->palo->SetField3(0, 0x107C, 0);
        RemoveGuardState(pguard, IstateGuard2FromIstate3(3));
    }
}

int GrfRideXp3(GUARD* pg)
{
    // orig3: the guard's contact flags (vt slot 96 0x256CB0) ask the job stack top first (slot 12); the jump job 0xCB's
    // (0x133530) answers 1 in its flight states 1 / 2 (the floor contacts by the short probe only, no long one): the
    // floor springs (0x24E660) would pull the guard back down at once
    const JOBRIDE3* p = PjobRide(pg);
    return p && p->st == 3 && p->fJumpGravity ? 1 : -1;
}

void SteerRide3(GUARD* pg)
{
    JOBRIDE3* p = PjobRide(pg);
    if (!p) return;
    if (p->fHeading) SetGuardHeading(pg, p->radHeading);
    if (p->fSteer && p->st == 3)
    {
        St(pg).vecVel = p->vecJumpFeet;
        return;
    }
    if (p->fSteer)
    {
        // the walk's root velocity scaled by the stick (none: a walk of 400 a second at full deflection)
        glm::vec3 v = St(pg).vecVel;
        if (glm::length(v) < 1.0f) v = VecForward() * -400.0f;
        St(pg).vecVel = v * p->uStick;
    }
}
