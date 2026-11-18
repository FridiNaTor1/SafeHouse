// game/ball3.cpp: Murray's Be The Ball (game/ball3.h; s3-gadgets). Ported from the Sly 3 decomp: func_001E2AC8.cpp,
// func_001E3488.cpp, func_001E4250.cpp, func_001E2A20.c, random_offset_apply_1E33E0.c, scalar_matrix_scale.cpp,
// actor_alternate_select.c, buffer_release_match.c, func_001E1A68.cpp, func_001E23F0.c, actor_state_event_1E25B0.c;
// 0x1E2E88 (no source) from its disassembly. Murray's side is in gang.cpp (his state 22: SetMurrayState enters with
// the mode 0 and leaves with -1, UpdateMurray / UpdateMurrayMotion / OnMurrayXpNew call in).
//
// What the state is: the step's custom geometry entry becomes a sphere (the entry's flag bit 17, the native f49,
// radius 75, its moving mesh taken off), the step's rotation control is switched off (motion kind 2) and the angular
// constraint freed, so the rigid solver rolls him. The contacts' velocity changes are summed after each solve
// (0x1E2AC8); an upward change over 500 presses him into the floor (mode 1) and 0.15 s later he is thrown up again by
// the height the impact would have reached, scaled (cross within 0.4 s: 200 + 1.35 h; circle held: -100 + 0.2 h; else
// -100 + 0.5 h), limited by the highest point reached so far. Square spins him toward the stick.
//
// Not ported: the body's squash matrix (the controller +0x1B10 made by 0x1DCD88 and the ring of the last 8 pushes),
// the blast of an impact over 2500 (0x277C08: radius 225, filter 0x400, state 0x28), the world's ceiling SW +0x4FF0
// (EOPID 1008), the look record's +0x1C (ALO +0x10C) set to -1 while a ball.

#include "game/ball3.h"

#include "spliceglue.h"
#include "activate.h"
#include "actor.h"
#include "alo.h"
#include "aseg.h"
#include "asega.h"
#include "clock.h"
#include "cls.h"
#include "dzg.h"
#include "emitter.h"
#include "gadget.h"
#include "game/items3.h"
#include "game/sly3.h"
#include "game/task3.h"
#include "gang.h"
#include "jump.h"
#include "lo.h"
#include "po.h"
#include "so.h"
#include "sound.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "task.h"
#include "util.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
#include "xfmath.h"
#include "xfphys.h"
#include "ainoise.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern SW* g_psw;
extern int g_iframeHarness;
extern DZG* g_pdzgSolve;   // solve.cpp (orig3 D_0036AEAC: the group being solved)
void SetGangAnim(ACTOR* p, ASEG* paseg, int grfapl);   // gang.cpp (0x23AC58 + 0x137588: the clip from 0, no state machine)
void SetStepOverlap(ALO* palo, ALO* paloOther);        // stepphys.cpp (orig 0x204C60, orig3 0x24F068)
void ApplyAloVelocityChanges(ALO* p);                  // solve.cpp (orig 0x1FA328, orig3 0x243F38)
void SwitchLoFaded(LO* plo, bool fOn, float g);        // asegevent.cpp (0x1F7C68 / 0x1F7BF8)

namespace sly3
{
namespace
{
constexpr uint32_t BTN_CROSS = 0x40, BTN_SQUARE = 0x80, BTN_CIRCLE = 0x20;

template <class T>
T* PPart(ALO* palo, uint32_t ib)
{
    for (const auto& [i, plo] : palo->apart)
        if (i == ib) return dynamic_cast<T*>(plo);
    return nullptr;
}
bool FPrintBall() { static const bool s_f = std::getenv("SLY3_BALLPRINT") != nullptr; return s_f; }

PADCTL* Ppadctl(MURRAY* p)
{
    PODATA* ppd = PpodataFromLo(p->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
bool FPressed(MURRAY* p, uint32_t grf, float dt)   // orig3 0x1A7A80 (pressed within dt, taken)
{
    PADCTL* pc = Ppadctl(p);
    return pc && GrfPadctlTakePressed(pc, grf, dt) != 0;
}
float Eval(const float* a, float t) { return a[0] + t * (a[1] + t * a[2]); }
glm::vec3 VecLimit(const glm::vec3& v, float gMax)   // orig3 0x1205F0
{
    const float l = glm::length(v);
    return gMax < l ? v * (gMax / l) : v;
}
float GGravity(MURRAY* p)   // (the mass state's +0x88: the gravity's z, -1750)
{
    SOST* ps = Psost(p->palo);
    return ps ? ps->dvGravity.z : -1750.0f;
}
SOE* PsoeBall(MURRAY* p)   // (the step's custom entry, Sly 3 +0x288)
{
    const int i = StepPhysOf(p->palo).isoeCustom;
    return i >= 0 && size_t(i) < p->palo->asoe.size() ? &p->palo->asoe[i] : nullptr;
}

// orig3 0x1E2A20: the effect object put at Murray's base (his position + the step's base +0x260), upright, and burst
// (0x1FE728(object, 1))
void BurstAt(MURRAY* p, LO* plo)
{
    if (!plo) return;
    if (ALO* palo = dynamic_cast<ALO*>(plo))
    {
        SetAloPos(palo, PosAloCenter(p->palo) + glm::vec3(StepPhysOf(p->palo).vecBase));
        SetAloRotation(palo, glm::mat3(1.0f));
    }
    AddLo(g_psw, plo);
    ExplodeLo(plo, nullptr);
    RemoveLo(g_psw, plo);
}

// orig3 0x1E33E0: a random spin added (a horizontal axis, 1.5..2.5: D_00324A30)
void AddRandomSpin(MURRAY* p)
{
    const glm::vec3 w = AngVelAlo(p->palo);
    const float rad = GRandInRange(-3.14159265f, 3.14159265f);
    const float s = GRandInRange(1.5f, 2.5f);
    SetAloAngVelocityTo(p->palo, w + cmm::SetVectorCylind(rad, s, 0.0f));
}

// orig3 0x1DCCF0: the moment of inertia = gain * the mass * 2250 on the diagonal
void SetBallInertia(MURRAY* p, float gain)
{
    SOST* ps = Psost(p->palo);
    if (!ps) return;
    const float g = gain * ps->m * 2250.0f;
    ps->momint = glm::mat3(g);
    ps->momintInv = g != 0.0f ? glm::mat3(1.0f / g) : glm::mat3(0.0f);
}

void StartSound(MURRAY* p, int id, float gVol, float gPitch, bool fPitch, bool fAtMurray)
{
    CSnd* ps = PsndStartSound(id, gVol, 0, fAtMurray ? static_cast<void*>(p->palo) : nullptr);
    if (ps && fPitch) ps->SetPitch(gPitch);
}
}  // namespace

void InitBall3(MURRAY* p)
{
    BALL3& b = p->ball;
    ALO* palo = p->palo;
    b.paseg = PPart<ASEG>(palo, FN3_TAG | 0x1AE4u);
    b.ploPartner = PPart<LO>(palo, FN3_TAG | 0x1AE8u);
    b.ploTarget = PPart<LO>(palo, FN3_TAG | 0x1AECu);
    b.ploNear = PPart<LO>(palo, FN3_TAG | 0x1AF0u);
    b.ploFar = PPart<LO>(palo, FN3_TAG | 0x1AF4u);
    b.ploBig = PPart<LO>(palo, FN3_TAG | 0x1AF8u);
    b.ploAnchor = PPart<LO>(palo, FN3_TAG | 0x1AFCu);
    if (FPrintBall())
        std::printf("ball: parts clip %d partner %d target %d near %d far %d big %d anchor %d\n", b.paseg ? b.paseg->oid : -1,
                    b.ploPartner ? b.ploPartner->oid : -1, b.ploTarget ? b.ploTarget->oid : -1, b.ploNear ? b.ploNear->oid : -1,
                    b.ploFar ? b.ploFar->oid : -1, b.ploBig ? b.ploBig->oid : -1, b.ploAnchor ? b.ploAnchor->oid : -1);
}

// orig3 0x1E2E88 (from the disassembly; 1040 bytes): the ball's mode k
void SetBallMode3(MURRAY* p, int k)
{
    BALL3& b = p->ball;
    ALO* palo = p->palo;
    STEPPHYS& s = StepPhysOf(palo);
    if (b.mode == k) return;
    if (b.mode == -1)
    {
        // into the ball: the step entry (+0x288) a sphere (flags |= 0x20000, +8 = 75), its mesh (+0x14) kept aside, the
        // step's motion kind (+0x1B8) 2, no pushed velocity (+0x1A0), the angular constraint off (0x23EC98(p, 0)); the
        // velocity keeps x / y and gets z + 300 in the air, 600 from a floor (+0x218); +0x1B1C = -FLT_MAX (D_00324A28)
        if (SOE* pe = PsoeBall(p))
        {
            pe->f49 = true;
            pe->sRadius = 75.0f;
            b.pmeshSaved = pe->pmesh;
            b.fMeshSaved = true;
            pe->pmesh = nullptr;
        }
        s.nMotion = 2;
        s.vecVel = glm::vec3(0.0f);
        SetAloCnstrAng(palo, 0);
        const glm::vec3 v = VelAloPos(palo);
        SetAloVelocityTo(palo, glm::vec3(v.x, v.y, s.pxpFloor ? 600.0f : v.z + 300.0f));
        b.zHeight = -FLT_MAX;
    }
    else if (b.mode == 2) DetachAloContactsAll(palo);   // 0x23DF78
    b.mode = k;
    b.tMode = g_clock.t;
    ASEG* paseg = nullptr;
    int grfapl = 1;
    switch (k)
    {
    case 0: paseg = b.paseg; grfapl = 9; break;
    case -1:
    {
        // out of the ball: the entry's flag and mesh back, its points pulled inside the radius and the mesh's velocities /
        // accelerations zeroed, the world geometry stale (+0xBC / +0xC0 = -1), the motion kind 0, the angular
        // constraint on, the fast target faded out, the partner removed, the inertia's gain 1, the heading his facing
        if (SOE* pe = PsoeBall(p))
        {
            pe->f49 = false;
            if (b.fMeshSaved) pe->pmesh = b.pmeshSaved;
            b.pmeshSaved = nullptr;
            b.fMeshSaved = false;
            for (glm::vec3& pos : pe->geomLocal.apos) pos = VecLimit(pos, pe->sRadius);
            if (pe->pmesh)
            {
                for (glm::vec3& v : pe->pmesh->av) v = glm::vec3(0.0f);
                for (glm::vec3& a : pe->pmesh->aa) a = glm::vec3(0.0f);
            }
            pe->nStampWorld = -1;
            pe->nStampObj = -1;
        }
        s.nMotion = 0;
        SetAloCnstrAng(palo, 1);
        if (b.ploTarget && b.ploTarget->fInWorld) SwitchLoFaded(b.ploTarget, false, 0.25f);   // 0x2412C8
        if (b.ploPartner && b.ploPartner->fInWorld) RemoveLo(g_psw, b.ploPartner);            // (vt slot 9)
        b.uBlend = 0.0f;
        SetBallInertia(p, 1.0f);
        {
            const glm::mat4 m = PxfObj(palo) ? PxfObj(palo)->matWorld : glm::mat4(1.0f);
            SetStepPhysHeading(palo, std::atan2(m[0][1], m[0][0]));   // 0x2A3EE8 / 0x24E878
        }
        b.sndRoll = CSndRef();
        break;
    }
    case 1:
        paseg = b.paseg;
        grfapl = 9;
        if (2500.0f < b.vecGather.z)
        {
            // (0x277C08(p, his position, 225, 0x400, 0x28, 0): the blast, not ported) and the big burst +0x1AF8
            PnoiseNew(PtmplNoiseFixed(0), PosAloCenter(palo), palo);
            BurstAt(p, b.ploBig);
        }
        break;
    case 2: DetachAloContactsAll(palo); break;
    default: break;
    }
    if (FPrintBall())
        std::printf("ball: frame %d t %.3f mode %d (gather z %.0f)\n", g_iframeHarness, g_clock.t, k, b.vecGather.z);
    if (b.mode == -1) return;
    // 0x23AC58 (the state machine dropped) and the clip unless it already plays (0x137588(clip, p, flags, +0x113C, 0, 1))
    if (!(p->pasega && p->pasega->paseg == paseg)) SetGangAnim(p, paseg, grfapl);
}

// orig3 0x1E1C68's state 22 (after the base 0x24E9E0): the floor contact of the ball: of his attachment's contacts the
// one whose end is his with the normal's z >= 0.7 and the least separation; its other end's point and owner kept
void FindBallFloor3(MURRAY* p)
{
    BALL3& b = p->ball;
    b.pxpHolder = nullptr;
    if (p->state != 22) return;
    ALO* palo = p->palo;
    OXA* poxa = Sort(palo).poxa;
    if (!poxa) return;
    float sMin = FLT_MAX;
    for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
        for (XP* pxp = pox->pxp; pxp; pxp = pxp->pxpNext)
        {
            const int i = pxp->axpd[0].paloRoot != poxa->pso ? 1 : 0;
            if (!pxp->axpd[i].psoe || pxp->axpd[i].psoe->palo != palo || pxp->normal.z < 0.7f) continue;
            if (pxp->gDist < sMin)
            {
                sMin = pxp->gDist;
                b.iHolder = i;
                b.pxpHolder = pxp;
            }
        }
    if (b.pxpHolder)
    {
        b.tHolder = g_clock.t;
        b.ploHolder = b.pxpHolder->axpd[1 - b.iHolder].palo;
    }
}

namespace
{
// orig3 0x1E1C68 (Murray's vt slot 61)
void FindMurrayFloor3(ALO* palo)
{
    // (the base 0x24E9E0 = Sly 2's 0x140D70, sly.cpp's FindActorFloor, through the registry)
    static const auto s_pfnBase = reinterpret_cast<void (*)(ALO*)>(PvNativeFn(0x140D70));
    if (s_pfnBase) s_pfnBase(palo);
    if (MURRAY* p = PmurrayFromLo(palo)) FindBallFloor3(p);
}
// orig3 0x1E1D88 (slot 58): the base, then the ball's floor contact forgotten when its other end is that object's
void SetMurrayOverlap3(ALO* palo, ALO* paloOther)
{
    SetStepOverlap(palo, paloOther);
    MURRAY* p = PmurrayFromLo(palo);
    if (p && p->ball.pxpHolder && p->ball.pxpHolder->axpd[1 - p->ball.iHolder].paloRoot == paloOther) p->ball.pxpHolder = nullptr;
}
// orig3 0x1E2AC8 (slot 70): the base 0x243F38, then in state 22 (not while ending) the solved contacts' velocity
// changes of his ends summed (each row's solution times its response: the group D_0036AEAC); mode 0: an upward change
// over 500 presses him into the floor (mode 1, the near / far burst by 1350), a smaller one is partly given back
// (+0.25 of it) with the thud over 500; mode 1 gathers
void ApplyMurrayVelocityChanges3(ALO* palo)
{
    ApplyAloVelocityChanges(palo);
    MURRAY* p = PmurrayFromLo(palo);
    if (!p || p->state != 22 || p->ball.mode == 2) return;
    BALL3& b = p->ball;
    glm::vec3 sum(0.0f);
    if (DZG* pdzg = g_pdzgSolve)
        for (int i = 0; i < pdzg->cdz && size_t(i) < pdzg->adz.size() && size_t(i) < pdzg->aent.size(); ++i)
        {
            const DZ& dz = pdzg->adz[i];
            if (dz.kind != 0 || dz.g == 0.0f) continue;
            const DZENT& ent = pdzg->aent[i];
            if (!ent.pxp) continue;
            for (int k = 0; k < 2; ++k)
                if (ent.pxp->axpd[k].paloRoot == palo) sum += dz.g * ent.advLin[k];
        }
    // (the ring of pushes for the body's squash: -0.1 + 0.00055 l clamped to 0..1, D_00324A10 / D_00324A20: not kept)
    switch (b.mode)
    {
    case 0:
        if (500.0f < sum.z)
        {
            b.vecGather = sum;
            SetBallMode3(p, 1);
            BurstAt(p, sum.z < 1350.0f ? b.ploNear : b.ploFar);
        }
        else
        {
            SetAloVelocityTo(palo, VelAloPos(palo) + 0.25f * sum);
            if (500.0f < glm::length(sum))
            {
                // (0x2459F8(0x8B) is made and replaced by 0x1B before it starts: one sound, volume 0.8)
                PsndStartSound(0x1B, 0.8f, 0, palo);
            }
        }
        break;
    case 1: b.vecGather += sum; break;
    default: break;
    }
}
}  // namespace
NATIVEFN3(0x1E1C68, FindMurrayFloor3);
NATIVEFN3(0x1E1D88, SetMurrayOverlap3);
NATIVEFN3(0x1E2AC8, ApplyMurrayVelocityChanges3);

// orig3 0x1E1A68 case 22: a new contact of the ball: ending, no friction; else no elasticity (the bounce is the
// state's) and the friction 0.6, 1 right after a spin
void OnBallXpNew3(MURRAY* p, XP* pxp)
{
    const BALL3& b = p->ball;
    if (b.mode == 2) pxp->mu = 0.0f;
    else
    {
        pxp->elas = 0.0f;
        pxp->mu = (1.0f - b.uBlend) * 0.6f + b.uBlend;
    }
}

// orig3 0x1E3488: the state's frame
void UpdateBall3(MURRAY* p)
{
    BALL3& b = p->ball;
    ALO* palo = p->palo;
    STEPPHYS& s = StepPhysOf(palo);
    PADCTL* pc = Ppadctl(p);
    const float g = GGravity(p);
    SetStepPhysHeading(palo, pc ? pc->radJoy : s.radHeading);   // 0x24E878(p, +0x110C)
    const glm::vec3 pos = PosAloCenter(palo);
    glm::vec3 vel = VelAloPos(palo);
    if (b.ploTarget)
    {
        const float sFlat = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        if (b.ploTarget->fInWorld)
        {
            if (b.mode == 0 && b.pxpHolder && sFlat < 500.0f && std::fabs(vel.z) < 1000.0f) SwitchLoFaded(b.ploTarget, false, 1.0f);
        }
        else if (850.0f < sFlat || vel.z < -2000.0f) SwitchLoFaded(b.ploTarget, true, 0.2f);
        else b.zHeight = -FLT_MAX;   // D_00324AA0
    }
    b.zHeight = std::max(b.zHeight, pos.z + vel.z * vel.z / (g * -2.0f));
    if (b.pxpHolder) b.zHeight = std::max(b.zHeight, b.pxpHolder->axpd[1 - b.iHolder].pos.z - s.vecBase.z + 562.0f);
    int mode;
    do
    {
        mode = b.mode;
        if (mode == 0 || mode == 1)
        {
            if (FPressed(p, BTN_SQUARE, 0.0f))
            {
                // the spin: the angular velocity moved by at most 4 toward the stick's (14 at full deflection, about the
                // axis left of the step's heading)
                b.tSpin = g_clock.t;
                const glm::vec3 w = AngVelAlo(palo);
                const glm::vec3 wTo = cmm::SetVectorCylind(s.radHeading + 1.5707964f, (pc ? pc->uJoy : 0.0f) * 14.0f, 0.0f);
                SetAloAngVelocityTo(palo, w + VecLimit(wTo - w, 4.0f));
                StartSound(p, 0x5F, 1.0f, -0.7f, true, true);
            }
        }
        switch (b.mode)
        {
        case 0:
        {
            // the jump: cross within 0.25 s, a floor within 0.15 s, slower upward than the jump's speed sqrt(630 |g|)
            const float vJump = std::sqrt(g * -630.0f);
            if (g_clock.t - b.tHolder < 0.15f && FPressed(p, BTN_CROSS, 0.25f) && vJump > vel.z)
            {
                vel = glm::vec3(vel.x, vel.y, vJump);
                SetAloVelocityTo(palo, vel);
                AddRandomSpin(p);
                BurstAt(p, b.ploAnchor);
                if (FPrintBall()) std::printf("ball: frame %d jump vz %.0f\n", g_iframeHarness, vJump);
            }
            break;
        }
        case 1:
            if (0.15f < g_clock.t - b.tMode)
            {
                bool fBoost = false;
                float h = b.vecGather.z * b.vecGather.z / (g * -2.0f);
                const float hImpact = h;
                if (FPressed(p, BTN_CROSS, 0.4f))
                {
                    static const float s_a[3] = {200.0f, 1.35f, 0.0f};   // D_00324A40
                    h = Eval(s_a, h);
                    BurstAt(p, b.ploAnchor);
                    fBoost = true;
                }
                else
                {
                    static const float s_aLow[3] = {-100.0f, 0.2f, 0.0f};   // D_00324A60 (circle held: +0x18 & 0x20)
                    static const float s_aMid[3] = {-100.0f, 0.5f, 0.0f};   // D_00324A50
                    h = Eval(pc && (pc->grfbtn & BTN_CIRCLE) ? s_aLow : s_aMid, h);
                    StartSound(p, 0x1B, 0.7f, 0.0f, false, false);
                }
                // min(+0x1B1C, SW +0x4FF0: the world's ceiling, option 1007, default 5500)
                h = std::min(h, std::min(b.zHeight, GSwOptionField3(1007, 0, 5500.0f)) - pos.z);
                if (h > 0.0f)
                {
                    vel = glm::vec3(vel.x, vel.y, std::sqrt(g * (h * -2.0f)));
                    SetAloVelocityTo(palo, vel);
                }
                if (850.0f < h) PnoiseNew(PtmplNoiseFixed(0), PosAloCenter(palo), palo);   // 0x173E00
                if (FPrintBall())
                    std::printf("ball: frame %d bounce impact %.0f (h %.0f) -> h %.0f vz %.0f boost %d\n", g_iframeHarness, b.vecGather.z, hImpact, h,
                                vel.z, int(fBoost));
                SetBallMode3(p, 0);
                AddRandomSpin(p);
                if (fBoost && g_psw) Broadcast3(0x5E, b.ploHolder);   // 0x1854C8(GAME, 0x5E, +0x1B0C): Sly 3's code as is (BroadcastTaskEvent translates Sly 2 codes and has no row for 0x5E: OTHER-01)
            }
            break;
        case 2:
        {
            // the end: after 1 s, or slow (|vz| < 600, flat < 600, spin < 8) and upright (the up row's z > 0.9): the
            // jump kind 0 from a floor, 8 in the air (0x1B7268; natively the fall, kind 0)
            const glm::mat4 m = PxfObj(palo) ? PxfObj(palo)->matWorld : glm::mat4(1.0f);
            const bool fSlow = std::fabs(vel.z) < 600.0f && std::sqrt(vel.x * vel.x + vel.y * vel.y) < 600.0f &&
                               glm::length(AngVelAlo(palo)) < 8.0f && 0.9f < m[2][2];
            if (1.0f < g_clock.t - b.tMode || fSlow)
            {
                SetJumpKind(&p->jump, 0, 0, nullptr);
                if (p->state == 22) SetMurrayState(p, 0);   // (natively, should the jump not take him)
                return;
            }
            break;
        }
        default: break;
        }
    } while (b.mode != mode);
    if (b.mode == -1) return;
    {
        static const float s_a[3] = {1.5f, -3.0f, 0.0f};   // D_00324A70, clamped to 0..1 (D_00341FF0)
        b.uBlend = std::clamp(Eval(s_a, g_clock.t - b.tSpin), 0.0f, 1.0f);
        SetBallInertia(p, (1.0f - b.uBlend) * 1.0f + b.uBlend * 2.0f);
    }
    if (b.mode == 0 && b.ploPartner)
    {
        if (1250.0f < std::fabs(vel.z)) { if (!b.ploPartner->fInWorld) AddLo(g_psw, b.ploPartner); }       // (vt slot 8)
        else if (0.5f < g_clock.t - b.tMode && b.pxpHolder && b.ploPartner->fInWorld) RemoveLo(g_psw, b.ploPartner);  // (slot 9)
    }
    // (the body's squash from the ring of pushes and the smoothed velocity +0x1B40: not ported)
    if (b.pxpHolder)
    {
        // the rolling sound: every 70..100 of rolled distance (the spin x the base, flat, over 150)
        const glm::vec3 c = glm::cross(AngVelAlo(palo), glm::vec3(s.vecBase));
        const float l = std::sqrt(c.x * c.x + c.y * c.y);
        if (150.0f < l) b.sRoll -= l * g_clock.dt;
        if (b.sRoll < 0.0f)
        {
            if (b.sndRoll) b.sndRoll->Stop(0.0f);
            CSnd* ps = PsndStartSound(0x6F, 0.8f, 0, nullptr);
            if (ps) ps->SetPitch(-0.5f);
            b.sndRoll = CSndRef(ps);
            b.sRoll += GRandInRange(70.0f, 100.0f);   // D_00324A98
        }
        else if (b.sndRoll && !b.sndRoll->FPlaying()) b.sndRoll = CSndRef();
    }
}

// orig3 0x1E4250: the state's motion (called by 0x1DEF10 after the step's motion)
void UpdateBallMotion3(MURRAY* p)
{
    BALL3& b = p->ball;
    ALO* palo = p->palo;
    STEPPHYS& s = StepPhysOf(palo);
    PADCTL* pc = Ppadctl(p);
    switch (b.mode)
    {
    case 2:
    {
        // ending: turned upright toward the heading's frame ({40, 0, 0.25}: D_00324AA8) and braked: -10 x the velocity,
        // each part scaled by its speed / 600 x 2 - 1 clamped to 0..1 (D_00324AC0 / D_00324AD0)
        static const float s_ac[3] = {40.0f, 0.0f, 0.25f};
        BrakeAloRotation(palo, s.matHeading, s_ac);   // 0x23EDE8
        const glm::vec3 v = VelAloPos(palo);
        const float uFlat = std::clamp(-1.0f + 2.0f * (std::sqrt(v.x * v.x + v.y * v.y) * 0.0016666667f), 0.0f, 1.0f);
        const float uZ = std::clamp(-1.0f + 2.0f * (std::fabs(v.z) * 0.0016666667f), 0.0f, 1.0f);
        AccelAloPos(palo, glm::vec3(uFlat * v.x, uFlat * v.y, uZ * v.z) * -10.0f);   // 0x244828
        break;
    }
    case 0:
    {
        // rolling: the spin pulled toward the stick's (10, 14 after a spin, about the axis left of the heading) with
        // the gain 0.5 + 10 s^2 (5 + 10 s^2 after a spin), and, not while carrying, the flat velocity toward 515 x the
        // stick along the heading with the gain 1.5 + s (2.5 + s)
        const float u = b.uBlend;
        const float radHeading = pc ? pc->radJoy : s.radHeading;
        const float sJoy = (1.0f - u) * (pc ? pc->uJoy : 0.0f) + u;
        const glm::vec3 wTo = cmm::SetVectorCylind(radHeading + 1.5707964f, ((1.0f - u) * 10.0f + u * 14.0f) * sJoy, 0.0f);
        static const float s_aW0[3] = {0.5f, 0.0f, 10.0f}, s_aW1[3] = {5.0f, 0.0f, 10.0f};   // D_00324AE0 / D_00324AF0
        const CLQ clqW{0.0f, (1.0f - u) * Eval(s_aW0, sJoy) + u * Eval(s_aW1, sJoy), 0.0f, 0.0f};
        AccelAloTowardRotation(palo, nullptr, nullptr, wTo, &clqW);   // 0x23EFA0(p, the identity, 0, w, {0, gain, 0})
        if (p->plo13F0) break;   // (+0x1540: carrying)
        const glm::vec3 vel = VelAloPos(palo);
        const glm::vec3 flat(vel.x, vel.y, 0.0f);
        const glm::vec3 vTo = cmm::SetVectorCylind(radHeading, sJoy * 515.0f, 0.0f);
        static const float s_aV0[3] = {1.5f, 1.0f, 0.0f}, s_aV1[3] = {2.5f, 1.0f, 0.0f};   // D_00324B00 / D_00324B10
        glm::vec3 accel = (vTo - flat) * ((1.0f - u) * Eval(s_aV0, sJoy) + u * Eval(s_aV1, sJoy));
        // (above the world's level SW +0x4FF0 the original adds up x flat.z x -4.5: flat has no z, nothing)
        // (0x1B8570(+0x320, &accel), the jump's push off walls: jump.cpp's is file-local, not applied)
        AccelAloPos(palo, accel);
        break;
    }
    default: break;
    }
}

// orig3 0x1E23F0 case 17 (in the ground states): no ball yet, a floor since the last ball ended, nothing carried; in
// state 22 only the ball's own button (its end)
bool FBallCanStart3(MURRAY* p)
{
    if (p->state == 22) return true;
    return p->ball.mode == -1 && p->tFloor > p->ball.tMode && !p->plo13F0;
}

// orig3 0x1E25B0 case 17: in the ball the button ends it (mode 2), else state 22
void StartBall3(MURRAY* p)
{
    if (p->state == 22) SetBallMode3(p, 2);
    else SetMurrayState(p, 22);
}
}  // namespace sly3
