// Sly's state 3: the swing on a hook (cid 0x13) of kind 0 / 1 / 2 (+0x124; kind 3 is state 22, climbsurf.cpp).
// Ported from the decomp: the entry / exit parts of 0x18C250 (JTS_Hang), the input 0x190520 (the substates 0..3: 0 the
// push forward, 2 the push back, 1 the free swing, 3 hanging still: its SM +0x1950), the pendulum 0x18FC60 (the
// acceleration +0x1910), the motion 0x1903B8 (+0x1910 applied, turned to hang below the hook), the frame 0x190BE0 and
// the release (0x18A8C8: CROSS after 0.25 s, the jump kind 2).
// The hand: its actuator (+0x1A00 on the record +0x19C0) is added on entry and 0x190E18 turns it to the hook each
// update; the draw (0x18A238 -> 0x190D10) moves the record +0x19C4 so the cane +0x19A4 sits on the hook point
// (0x190C58, plus +0x1960 easing to 0), then solves the arm's joint and the world matrices.
// TODO: the hook on a moving object (+0x1900, an XA: 0x18F958 / 0x1F5078).
#include "climbsurf.h"
#include "activate.h"
#include "actor.h"
#include "alo.h"
#include "asega.h"
#include "binoc.h"
#include "blend.h"
#include "clock.h"
#include "interact.h"
#include "jt.h"
#include "jump.h"
#include "lo.h"
#include "mat.h"
#include "po.h"
#include "sly.h"
#include "sm.h"
#include "so.h"
#include "sound.h"
#include "stepphys.h"
#include "update.h"
#include "vecmat.h"
#include "xf.h"
#include "xfmath.h"
#include "xfphys.h"
#include "lox.h"
#include "brx.h"
#include "actanim.h"
#include "ik.h"
#include "sw.h"
#include "refram.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// (local copies of sly.cpp's / jumptgt.cpp's helpers)
PADCTL* PpadctlOf(SLY* p) { PODATA* ppd = PpodataFromLo(p->palo); return ppd ? ppd->ppadctl : nullptr; }
float RadJoy(SLY* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->radJoy : 0.0f; }
float UJoy(SLY* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->uJoy : 0.0f; }
bool FBtnTake(SLY* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlTakePressed(pc, grf, dt); }
bool FBtnWithin(SLY* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlPressedWithin(pc, grf, dt); }
float GDuration(const ASEG* paseg) { return paseg ? PasegdOf(const_cast<ASEG*>(paseg))->tMax : 0.0f; }  // +0x74
XF* PxfSlot32(LO* plo)
{
    if (!plo) return nullptr;
    if (FIsLoDerivedFrom(plo, 0x30)) return PxfObj(static_cast<ALO*>(plo));
    if (FIsLoDerivedFrom(plo, 3)) return static_cast<LOX*>(plo)->pxf;
    return nullptr;
}
}  // namespace

namespace
{
constexpr uint32_t BTN_CROSS = 0x40, BTN_R3 = 0x400;
constexpr float kPi = 3.14159274101257324f;

bool FHangPrint() { static const bool f = std::getenv("SLY2N_HANGPRINT") != nullptr; return f; }

float GHookField(LO* phook, uint32_t ib)
{
    const uint32_t u = phook->U32Field(0, ib, 0);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

glm::vec3 Gravity(ALO* palo)
{
    const SOST* ps = Psost(palo);
    return ps ? ps->dvGravity : glm::vec3(0.0f, 0.0f, -2275.0f);
}

glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& fallback)
{
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l < 0.0001f ? fallback : v * (1.0f / l);
}

// orig: 0x00190BE0 (the hook's frame: the heading of its x axis)
float RadHangFrame(SLY* p)
{
    const glm::mat4 m = MatJtLo(p->ploOther);  // vt slot 31
    return std::atan2(m[0].y, m[0].x);          // 0x2458F0
}
}  // namespace

// orig: 0x0018FC60 CalculateJtHangAccel (the pendulum about the hook: the swing's angular velocity (its own, the
// gravity's, damped along / across the hook's y axis; pushed for 0.25 s on entering substate 0 / 2), the rope's length
// smoothed to 230 less the hook's +0x134; +0x1910 = the acceleration that reaches the next frame's velocity, less gravity)
void CalculateSlyHangAccel(SLY* p)
{
    LO* phook = p->ploOther;
    if (!phook) return;
    ALO* palo = p->palo;
    const float dt = g_clock.dt;
    const glm::vec3 anchor = PosLoWorld(phook);       // 0x1A62E8
    const glm::mat4 basis = MatJtLo(phook);           // 0x1A6318 (rows)
    XF* pxf = PxfSlot32(phook);
    const glm::vec3 vAnchor = pxf ? VelXfPoint(pxf, nullptr, anchor) : glm::vec3(0.0f);  // 0x22D348
    const glm::vec3 displacement = PosLoWorld(palo) - anchor;
    const float distance = std::sqrt(displacement.x * displacement.x + displacement.y * displacement.y + displacement.z * displacement.z);
    const glm::vec3 axis = distance < 0.0001f ? glm::vec3(0.0f, 0.0f, 1.0f) : displacement * (1.0f / distance);
    const glm::vec3 relative = VelAloPos(palo) - vAnchor;  // vt slot 33
    const float radial = glm::dot(relative, axis);          // 0x11E290
    const glm::vec3 perpendicular = relative - axis * radial;
    const glm::vec3 firstAxis = NormalizeOr(glm::cross(axis, perpendicular), glm::vec3(1.0f, 0.0f, 0.0f));
    const float ratio = glm::length(perpendicular) / distance;
    const glm::vec3 angularFirst = firstAxis * ratio;
    const glm::vec3 grav = Gravity(palo);  // (+0xE8 -> +0x80)
    const glm::vec3 projected = grav - axis * glm::dot(grav, axis);
    const glm::vec3 secondAxis = NormalizeOr(glm::cross(axis, projected), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 angularSecond = secondAxis * (glm::length(projected) / distance);
    float drag = 1.5f + ratio * (-0.35f + ratio * 0.0f);  // D_002BF4F0
    drag = drag < -5.0f ? -5.0f : (-0.25f < drag ? -0.25f : drag);  // D_002BF500
    const bool fPush = p->substate == 0 || p->substate == 2;
    const float bias = fPush ? -2.5f : -1.0f;  // D_002BF508 / D_002BF50C
    glm::vec3 angular = angularFirst + angularSecond * dt;
    const glm::vec3 hookY = glm::vec3(basis[1]);
    const glm::vec3 parallel = hookY * glm::dot(angular, hookY);
    angular = parallel * (drag * dt + 1.0f) + (angular - parallel) * (bias * dt + 1.0f);
    if (fPush && g_clock.t - p->tSubstate < 0.25f)  // D_002BF510
    {
        float amount = dt * 5.0f;  // D_002BF514
        if (p->substate == 0) amount = -amount;
        if (p->fHangBack) amount = -amount;
        const float down = -grav.z;
        const float speed = glm::length(perpendicular);
        const float t = (down * displacement.z + speed * 0.5f * speed) / down;
        float g = 0.5f + t * (-0.01f + t * 0.0f);  // D_002BF520
        g = g < 0.0f ? 0.0f : (1.0f < g ? 1.0f : g);  // D_002BBB60
        amount *= g;
        angular += hookY * amount;
    }
    static const cmm::SMPA s_smpa{1000.0f, 0.0f, 0.25f, 5000.0f};  // D_002BF530
    float speedOut = 0.0f;
    const float next = cmm::GSmoothA(distance, radial, 230.0f - GHookField(phook, 0x134), dt, &s_smpa, &speedOut);  // 0x11D1C0
    angular *= std::min(230.0f, distance) / std::max(75.0f, next);  // D_002BF544 / D_002BF540
    const glm::vec3 velocity = vAnchor;  // (the hook on a moving object +0x1900: TODO)
    const glm::mat3 rotation = cmm::BuildRotateMatrixVector((angularFirst + angular) * (dt * 0.5f));  // 0x11B570
    const glm::vec3 direction = rotation * axis;
    const glm::vec3 radius = direction * next;
    const glm::vec3 vFinal = velocity + glm::cross(angular, radius) + direction * speedOut;
    p->vecHangAccel = (vFinal - VelAloPos(palo)) * (1.0f / dt) - grav;
    if (FHangPrint())
        std::printf("hang: t %.3f sub %d d %.2f next %.2f pos %.1f %.1f %.1f v %.1f %.1f %.1f\n", g_clock.t, p->substate, distance, next,
                    PosLoWorld(palo).x, PosLoWorld(palo).y, PosLoWorld(palo).z, VelAloPos(palo).x, VelAloPos(palo).y, VelAloPos(palo).z);
}

// orig: 0x0018C250 (entering state 3): returns the swing's blend (played still at its middle)
void EnterSlyHang(SLY* p)
{
    LO* phook = p->ploOther;
    p->nHangPrev = -1;
    p->gSwingSpeed = 1.0f;
    p->gSwingActive = 1.0f;
    p->tHang = g_clock.t;
    if (!phook) return;
    if (p->pctrlHand)
    {
        p->pctrlHand->pos = glm::vec4(0.0f, 0.0f, 0.0f, p->pctrlHand->pos.w);  // (its rest +0x20 cleared)
        if (!FXfHasCtrl(p->pxfHandRot, p->pctrlHand.get())) AddXfCtrl(p->palo, p->pxfHandRot, p->pctrlHand.get());  // 0x230018
    }
    if (p->plo19A4) p->dposHang = PosLoWorld(p->plo19A4) - PosSlyHangPoint(p);  // 0x1A62E8 / 0x190C58
    UpdateSlyHangPlace(p);  // 0x190E18
    HandleLoSpliceEvent(phook, 5, false, 0, nullptr);
    const float radFrame = RadHangFrame(p);
    const float radBack = RadNormalize(radFrame + kPi);
    const int nKind = static_cast<int>(phook->U32Field(0, 0x124, 0));
    switch (nKind)
    {
    case 1: SetStepPhysHeading(p->palo, radFrame); p->fHangBack = 0; break;
    case 2: SetStepPhysHeading(p->palo, radBack); p->fHangBack = 1; break;
    case 0:
        if (std::fabs(RadNormalize(radFrame - RadJoy(p))) < 1.57079637f)
        {
            SetStepPhysHeading(p->palo, radFrame);
            p->fHangBack = 0;
        }
        else
        {
            SetStepPhysHeading(p->palo, radBack);
            p->fHangBack = 1;
        }
        break;
    default: break;
    }
    const int sfx = static_cast<int>(phook->U32Field(0, 0x12C, 8));
    const glm::vec3 pos = PosLoWorld(p->palo);
    PsndStartSound(sfx, 1.0f, 0, nullptr, &pos);  // 0x1FBB90 / 0x1FB2F8(0) / 0x1FB3F0
    SetSlyMode(p, 1);  // 0x18E540
    p->vecVel = glm::vec3(0.0f);
    CalculateSlyHangAccel(p);
    SetActorZone(p, PloJtDescZone(phook, JTL_Point));  // (SetPlayerTn: the hook's +0x100)
    if (FHangPrint()) std::printf("hang: enter hook %d kind %d back %d t %.3f\n", phook->oid, nKind, p->fHangBack, g_clock.t);
}

// orig: 0x0018C250 (leaving state 3)
void LeaveSlyHang(SLY* p)
{
    if (p->ploOther) HandleLoSpliceEvent(p->ploOther, 6, false, 0, nullptr);
    SetSlyMode(p, -1);
    if (p->pctrlHand && FXfHasCtrl(p->pxfHandRot, p->pctrlHand.get()))
        RemoveXfCtrl(p->palo, p->pxfHandRot, p->pctrlHand.get());  // 0x230100
    p->ploOther = nullptr;
    SetActorZone(p, nullptr);
    if (FHangPrint()) std::printf("hang: leave t %.3f\n", g_clock.t);
}

// orig: 0x00190520 (state 3's input: the facing, the stick near / far of the heading -> the substates, the pendulum,
// R3 the binocucom, the blend's weights and its time by the swing's angle)
void UpdateSlyHangInput(SLY* p)
{
    LO* phook = p->ploOther;
    if (!phook) return;
    ALO* palo = p->palo;
    float heading = RadHangFrame(p);
    if (p->fHangBack) heading = RadNormalize(heading + kPi);
    SetStepPhysHeading(palo, heading);  // 0x204660
    const float diff = RadNormalize(RadJoy(p) - p->radHeading);
    bool fNear = false, fFar = false;
    if (UJoy(p) > 0.3f)
    {
        const float d = std::fabs(diff);
        if (d < 1.04719758f) { fNear = true; p->tHang = g_clock.t; }
        else if (d > 2.09439516f) { fFar = true; p->tHang = g_clock.t; }
    }
    else p->nHangPrev = -1;
    // the swing's angle in the heading's frame: (hook - Sly) turned into it, its pitch from straight up
    const glm::vec3 dir = NormalizeOr(PosLoWorld(phook) - PosLoWorld(palo), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat3 m = MatRotateAxis(p->radHeading, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B500
    const glm::vec3 local = glm::transpose(m) * dir;
    const float angle = RadNormalize(std::atan2(local.z, local.x) - 1.57079637f);  // 0x2458F0
    int next = p->substate;
    const float t = g_clock.t;
    const auto speed = [&]() { return glm::length(VelAloPos(palo)); };
    switch (next)
    {
    case 0:
        if (fFar) next = 2;
        else if (fNear) { if (t - p->tSubstate > 1.4f) { p->nHangPrev = 0; next = 1; } }  // D_002BF554
        else if (t - p->tHang > 0.25f) next = 1;  // D_002BF55C
        break;
    case 2:
        if (fNear) next = 0;
        else if (fFar) { if (t - p->tSubstate > 1.4f) { p->nHangPrev = 2; next = 1; } }  // D_002BF558
        else if (t - p->tHang > 0.25f) next = 1;
        break;
    case 1:
        if (fNear && p->nHangPrev != 0) { next = 0; p->nHangPrev = -1; }
        else if (fFar && p->nHangPrev != 2) { next = 2; p->nHangPrev = -1; }
        else if (std::fabs(angle) < 0.1f && speed() < 80.0f) next = 3;  // D_002BF560 / D_002BF568
        break;
    case 3:
        if (fNear && p->nHangPrev != 0) { next = 0; p->nHangPrev = -1; }
        else if (fFar && p->nHangPrev != 2) { next = 2; p->nHangPrev = -1; }
        else if (std::fabs(angle) > 0.4f && speed() > 120.0f) next = 1;  // D_002BF564 / D_002BF56C
        break;
    default: break;
    }
    if (FHangPrint()) std::printf("hang: input t %.3f heading %.3f joy %.3f u %.3f near %d far %d angle %.3f sub %d -> %d\n", g_clock.t, p->radHeading, RadJoy(p), UJoy(p), fNear, fFar, angle, p->substate, next);
    SetSlyState(p, 3, next);  // 0x18C250
    if (p->state != 3) return;
    CalculateSlyHangAccel(p);  // 0x18FC60
    // (+0x1900: 0x1F5078 on the moving object: TODO)
    if (FBtnWithin(p, BTN_R3, 0.15f))
    {
        // TODO(actors): slot 99 (3)'s other checks (0x18D760)
        ForgetPadctlPresses(PpadctlOf(p), BTN_R3);  // 0x1C4738
        OpenBinoc(p, 0, nullptr, nullptr);         // 0x19CC50
    }
    if (p->substate == 3) return;
    float activeTarget = 1.0f, speedTarget = 1.0f;
    switch (p->substate)
    {
    case 0: activeTarget = 1.0f; speedTarget = 1.0f; break;
    case 2: activeTarget = 1.0f; speedTarget = 0.0f; break;
    case 1: activeTarget = 0.0f; speedTarget = p->gSwingSpeed > 0.5f ? 1.0f : 0.0f; break;
    default: break;
    }
    static const cmm::SMP s_smpActive{1.0f, 0.0f, 0.1f}, s_smpSpeed{4.0f, 0.0f, 0.1f};  // D_002BF570 / D_002BF580
    p->gSwingActive = cmm::GSmooth(p->gSwingActive, activeTarget, g_clock.dt, &s_smpActive, nullptr);  // 0x11D018
    p->gSwingSpeed = cmm::GSmooth(p->gSwingSpeed, speedTarget, g_clock.dt, &s_smpSpeed, nullptr);
    const float active = p->gSwingActive, sp = p->gSwingSpeed;
    if (p->pblSwing) SetBlendWeights(p->pblSwing, {active * sp, active * (1.0f - sp), 1.0f - active});  // 0x1384C0
    // the clip's time follows the swing's angle: its slope toward tMax (angle / 1.7 + 1) / 2, at most 3 a second
    if (p->pblSwing && p->pasega && p->pasega->paseg == p->pblSwing)
    {
        const float steering = std::clamp(angle / 1.7f, -1.0f, 1.0f);  // D_002BF58C
        float target = GDuration(p->pblSwing) * (steering + 1.0f);
        target *= 0.5f;
        float svt = (target - p->pasega->t) / g_clock.dt;
        svt = std::clamp(svt, -3.0f, 3.0f);  // 0x11CFE0 (D_002BF590)
        p->pasega->svt = svt;
    }
}

// orig: 0x001903B8 (state 3's motion, after gravity: +0x1910 applied, the rotation braked toward hanging below the
// hook at the heading)
void UpdateSlyHangMotion(SLY* p)
{
    if (!p->ploOther) return;
    ALO* palo = p->palo;
    AccelAloPos(palo, p->vecHangAccel);  // 0x1FAC30
    const glm::vec3 d = PosLoWorld(p->ploOther) - PosLoWorld(palo);
    const glm::mat3 m1 = BuildRotateVectorsMatrix(glm::vec3(0.0f, 0.0f, 1.0f), d);  // 0x11BA08
    const glm::mat3 m2 = MatRotateAxis(p->radHeading, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B500
    static const float s_ac[3] = {15.0f, 0.0f, 0.1f};  // D_002BF548
    BrakeAloRotation(palo, m1 * m2, s_ac);  // 0x1F59A0 (the original's m2 m1, rows)
}

// orig: 0x0018A8C8 (state 3's transitions: CROSS after 0.25 s lets go: the jump kind 2)
bool FSlyHangRelease(SLY* p)
{
    if (!(g_clock.t - p->tState > 0.25f)) return false;
    if (!FBtnTake(p, BTN_CROSS, 0.25f)) return false;
    if (FHangPrint()) std::printf("hang: release t %.3f\n", g_clock.t);
    SetJumpKind(&p->jump, 2, 0, nullptr);
    return true;
}

// orig: 0x00190C58 (the hook's point less... past it: the hook's position plus its +0x134 along the direction from the
// arm's record +0x19CC to the hook; up when they meet)
glm::vec3 PosSlyHangPoint(SLY* p)
{
    LO* phook = p->ploOther;
    if (!phook) return glm::vec3(0.0f);
    const glm::vec3 q = PosLoWorld(phook);  // 0x1A62E8
    const glm::vec3 d = p->pxfArm ? q - glm::vec3(p->pxfArm->matWorld[3]) : glm::vec3(0.0f);
    const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const glm::vec3 dir = l < 0.0001f ? glm::vec3(0.0f, 0.0f, 1.0f) : d * (1.0f / l);  // D_002A2DC0
    return q + dir * GHookField(phook, 0x134);
}

namespace
{
// 0x190E18's rotation: the x axis turned onto q (in the record's parent's frame), then 1.5 radians about it
glm::vec3 EulHandToward(const glm::vec3& q)
{
    const glm::vec3 x(1.0f, 0.0f, 0.0f);  // D_002A2DA0
    // (the original's rows: rotate(x, 1.5) x vectors(x, q))
    const glm::mat3 m = BuildRotateVectorsMatrix(x, q) * MatRotateAxis(1.5f, x);  // 0x11BA08 / 0x11B500 (D_002BF594)
    return EulerFromMat(m);  // 0x11BFF0
}
}  // namespace

// (debug: SLY2N_HANGRAM=ram,x,y,z: the reference's RAM inside 0x190D10 and the hook's position; prints the actuator's
// angles the reference stored (+0x1A00 +0x20) and the ones EulHandToward gives from that RAM's records)
void DebugCheckHangRam()
{
    const char* sz = std::getenv("SLY2N_HANGRAM");
    if (!sz) return;
    char path[512] = {};
    float hx = 0, hy = 0, hz = 0;
    if (std::sscanf(sz, "%511[^,],%f,%f,%f", path, &hx, &hy, &hz) != 4) return;
    RAM ram;
    if (!ram.FLoad(path)) { std::printf("hangram: no RAM %s\n", path); return; }
    const uint32_t sly = ram.U32(0x3E138C);
    const uint32_t r0 = ram.U32(sly + 0x19C0), rArm = ram.U32(sly + 0x19CC), ctrl = ram.U32(sly + 0x1A00), ref = ram.U32(r0 + 0xA0);
    auto row = [&](uint32_t a) { return glm::vec3(ram.F32(a), ram.F32(a + 4), ram.F32(a + 8)); };
    const glm::mat3 R(row(ref + 0x40), row(ref + 0x50), row(ref + 0x60));  // (glm columns = the original's rows)
    const glm::vec3 d = glm::vec3(hx, hy, hz) - row(rArm + 0x70);
    const glm::vec3 q = glm::transpose(R) * d;  // 0x22FCC8(0, ref, d)
    const glm::vec3 e = EulHandToward(q), r = row(ctrl + 0x20);
    if (SLY* ps = PslyGlobal())
        for (ALOPAIR& q : ps->palo->apair)
            std::printf("hangram: pair target %d/%d (%p) upper %d/%d lower %d/%d end %d/%d; hand pos %p rot %p arm %p; target ref %p\n", q.apxf[0] ? q.apxf[0]->tag : -1,
                        q.apxf[0] ? q.apxf[0]->kind : -1, (void*)q.apxf[0], q.apxf[1] ? q.apxf[1]->tag : -1, q.apxf[1] ? q.apxf[1]->kind : -1,
                        q.apxf[2] ? q.apxf[2]->tag : -1, q.apxf[2] ? q.apxf[2]->kind : -1, q.apxf[3] ? q.apxf[3]->tag : -1, q.apxf[3] ? q.apxf[3]->kind : -1,
                        (void*)ps->pxfHandPos, (void*)ps->pxfHandRot, (void*)ps->pxfArm, q.apxf[0] ? (void*)q.apxf[0]->pxfRef : nullptr);
    std::printf("hangram: sly %08X state %d ref %.4f %.4f %.4f native %.4f %.4f %.4f\n", sly, ram.S32(sly + 0x18C8), r.x, r.y, r.z, e.x, e.y, e.z);
}

// orig: 0x00190E18 (the hand's actuator: the record +0x19C0 turned so its x axis points from the arm to the hook, then
// 1.5 radians about it; +0x1960 eased to 0)
void UpdateSlyHangPlace(SLY* p)
{
    if (!p->ploOther || !p->pxfHandRot || !p->pxfArm || !p->pctrlHand) return;
    const glm::vec3 d = PosLoWorld(p->ploOther) - glm::vec3(p->pxfArm->matWorld[3]);
    const glm::vec3 q = VecConvertXfs(nullptr, p->pxfHandRot->pxfRef, d);  // 0x22FCC8
    SetCtrlPos(p->pctrlHand.get(), glm::vec4(EulHandToward(q), 0.0f));  // 0x12D108
    static const cmm::SMP s_smp{3000.0f, 1000.0f, 0.15f};  // D_002BF598
    p->dposHang = cmm::PosSmooth(p->dposHang, glm::vec3(0.0f), g_clock.dt, &s_smp, nullptr);  // 0x11F088
}

// orig: 0x00190D10 (from Sly's draw 0x18A238 in state 3: the record +0x19C4 moved so the cane +0x19A4 is on the hook
// point (+0x1960 still added), its world matrix, the arm's joint solved (0x186A50 with +0x19CC's +0xA8), vt slot 18)
void DrawSlyHangPlace(SLY* p)
{
    XF* pxf = p->pxfHandPos;
    if (!p->ploOther || !pxf || !p->plo19A4 || !p->pxfArm) return;
    const glm::vec3 d = PosSlyHangPoint(p) + p->dposHang - PosLoWorld(p->plo19A4);
    const glm::vec3 v = VecConvertXfs(nullptr, pxf->pxfRef, d);  // 0x22FCC8
    pxf->mat[3] = glm::vec4(glm::vec3(pxf->mat[3]) + v, 1.0f);
    pxf->nStamp = ++g_cXfStamp;
    pxf->matWorld = pxf->pxfRef ? pxf->pxfRef->matWorld * pxf->mat : pxf->mat;  // 0x11C168
    ALOPAIR* ppair = nullptr;  // (+0x19CC's +0xA8: the joint whose upper record it is)
    for (ALOPAIR& q : p->palo->apair)
        if (q.apxf[1] == p->pxfArm) { ppair = &q; break; }
    if (ppair) SolveAloPair(p->palo, ppair);  // 0x186A50
    else if (static bool s_f = false; !s_f) { s_f = true; std::fprintf(stderr, "sly: 0x190D10 without the arm's joint\n"); }
    UpdateAloXfWorldsHierarchy(p->palo);  // vt slot 18 (0x22FBE0)
    if (FHangPrint())
    {
        const glm::vec3 c = PosLoWorld(p->plo19A4), h = PosSlyHangPoint(p);
        float gap = -1.0f;
        if (ppair && ppair->apxf[0] && ppair->apxf[3])
            gap = glm::length(glm::vec3(ppair->apxf[3]->matWorld * glm::vec4(ppair->vecB0, 1.0f)) -
                              glm::vec3(ppair->apxf[0]->matWorld * glm::vec4(ppair->vecA0, 1.0f)));
        std::printf("hang: draw t %.3f cane %.1f %.1f %.1f point %.1f %.1f %.1f ofs %.1f %.1f %.1f d %.2f arm gap %.2f mode %d\n", g_clock.t, c.x, c.y,
                    c.z, h.x, h.y, h.z, p->dposHang.x, p->dposHang.y, p->dposHang.z, glm::length(d), gap, pxf->mode);
    }
}

// orig: 0x0018A238 (Sly's draw: 0x190D10 in state 3, then the actors' draw 0x1425B0, which the scene does) for each
// Sly the world draws
void DrawSlys(SW* psw)
{
    static bool s_fRam = (DebugCheckHangRam(), true);
    (void)s_fRam;
    for (CID cid = 0x35; cid < g_acls[0x35].cidLast; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            SLY* p = PslyFromLo(plo);
            if (!p || p->state != 3 || !FAloDrawn(psw, p->palo)) continue;
            DrawSlyHangPlace(p);
        }
}
