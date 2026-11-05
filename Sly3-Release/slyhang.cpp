// Sly's state 3: the swing on a hook (cid 0x13) of kind 0 / 1 / 2 (+0x124; kind 3 is state 22, climbsurf.cpp).
// Ported from the decomp: the entry / exit parts of 0x18C250 (JTS_Hang), the input 0x190520 (the substates 0..3: 0 the
// push forward, 2 the push back, 1 the free swing, 3 hanging still: its SM +0x1950), the pendulum 0x18FC60 (the
// acceleration +0x1910), the motion 0x1903B8 (+0x1910 applied, turned to hang below the hook), the frame 0x190BE0 and
// the release (0x18A8C8: CROSS after 0.25 s, the jump kind 2).
// TODO(anim): the hand's actuator (+0x1A00 on the record +0x19C0: inserted on entry, 0x190E18 turns it to the hook,
// the draw's 0x190D10 (via 0x18A238) puts the hand on the hook by +0x1960); the hook on a moving object (+0x1900, an XA:
// 0x18F958 / 0x1F5078).
#include "climbsurf.h"
#include "sly3/ids3.h"
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
    if (FIsLoDerivedFrom(plo, Icid(CID3::ALO))) return PxfObj(static_cast<ALO*>(plo));
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
    // TODO(anim): 0x230018 (the hand's actuator +0x1A00 on +0x19C0, its rest cleared), +0x1960 = the hand +0x19A4 less
    // the hook's point (0x190C58), 0x190E18
    if (!phook) return;
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
