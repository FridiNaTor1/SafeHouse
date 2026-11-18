// rccar3.cpp: Sly 3's RC car (class 0x55, orig3 0x203C18..0x208090), from the Sly 3 decomp.
//
// Bentley's RC car (car 38546: Ep4 recon, m4_rccar, the Ep4 heist's alarm boxes; Ep5's m5_love). A reworked copy of
// Sly 2's vehicle (vehicle.cpp, class 0x4D): the record is VEHICLE with its Sly 3 part VEHICLERC3 (vehicle.h); the
// wheel contacts (slot 63 0x206C10), their removal (slot 58 0x206CD8), the bounds (slot 54 0x206F58) and the drawn
// wheels (0x206FD8) are vehicle.cpp's with Sly 3's differences. Here: the init 0x203C18, the post-load 0x203D88, the
// update 0x203FB0 with its control 0x204048 (the pad: X drives, Square / Circle reverse, the stick steers; a path;
// the jump back to the trail), the gun 0x205238 / 0x205418 (R1: the proxy's projectiles by the launcher, aimed at
// the nearest gun target (an INTEREST with flag 0x100) inside the region), the trail 0x2045B0 / 0x2047D0, the forces
// 0x205A78, the enable 0x207E18 (vt slot 83), the mode change 0x2075E8, the messages 0x207FA0, the hit response
// 0x207AE0 (AUDIT4 MODES-07).
// Not ported: the dust and engine effects (0x204E00, the wheels' 0x78E), the horn (sound 0x2A), the clips on L2 / R2
// (0x205750), the gun's sound gain (0x207880) and rumble, the
// body's lean controller (part 0x22's +0x34C), the line-of-sight test of the aim (0x165000).
// Debug: SLY3_RCPRINT=1 (mode, drive, steering, position, shots).

#include "vehicle.h"

#include "activate.h"
#include "brx.h"
#include "clock.h"
#include "pe.h"
#include "po.h"
#include "projectile.h"
#include "routetele.h"
#include "so.h"
#include "spliceglue.h"
#include "sw.h"
#include "update.h"
#include "crv.h"
#include "objmethod.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
#include "xfphys.h"
#include "game/sly3.h"
#include "game/suck3.h"
#include "game/task3.h"
#include "cm.h"
#include "rumble.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset);   // launcher.cpp
void LaunchFrom(LO* plaunch, LO* po);                                     // launcher.cpp
bool FPointInVolume(LO* plo, const glm::vec3& pos);                       // trigger.cpp
glm::mat3 LevelMatrix3(const glm::mat3&, glm::mat3*);                      // mat.cpp

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_RCPRINT") != nullptr;
    return s_f;
}
float Poly(const glm::vec3& p, float x) { return p.x + x * (p.y + x * p.z); }
float Clamp(float x, float lo, float hi) { return x < lo ? lo : hi < x ? hi : x; }

// an object's world matrix (ALO: its object record; LOX: its record x its local)
glm::mat4 MatWorld(LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (FIsLoDerivedFrom(plo, S3CID(0x30)))
        if (XF* pxf = PxfObj(static_cast<ALO*>(plo))) return pxf->matWorld;
    if (const LOX* plox = dynamic_cast<const LOX*>(plo)) return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    return glm::mat4(1.0f);
}

PADCTL* Ppad(VEHICLE& v) { return PpadctlOfSo3(v.owner); }
uint32_t GrfHeld(VEHICLE& v)
{
    const PADCTL* p = Ppad(v);
    return p && p->nControl == 1 ? p->grfbtn : 0;
}

// orig3 0x205A38: the wheels touching something
int CwheelContact(const VEHICLE& v)
{
    int c = 0;
    for (const VEHICLEWHEEL& w : v.wheels) c += w.contacts != nullptr;
    return c;
}
// orig3 0x2058D8: no wheel touches and the body has no contact (0x23E130: the overlaps' first contact)
bool FAirborne(VEHICLE& v)
{
    if (CwheelContact(v)) return false;
    const OXA* poxa = Poxa(v.owner);
    for (const OX* pox = poxa ? poxa->pox : nullptr; pox; pox = pox->poxNext)
        if (pox->pxp) return false;
    return true;
}
// orig3 0x205888: upright (the object matrix's up row z > 0.5)
bool FUpright(VEHICLE& v) { return glm::mat3(MatWorld(v.owner))[2].z > 0.5f; }

LO* PloRef(LO* plo, const CRef& r)
{
    if (r.m_tagk == TAGK_S32) return r.m_n == -1 || r.m_n == 0 ? nullptr : PloFindRelative(plo, r.m_n);
    return dynamic_cast<LO*>(r.m_pbasic);
}
LO* PloOpt(LO* plo, const OPTVAL& value)
{
    if (value.pbasic) return dynamic_cast<LO*>(value.pbasic);
    return value.n == -1 || value.n == 0 ? nullptr : PloFindRelative(plo, value.n);
}

// orig3 0x205590: the nearest gun target (an in-world INTEREST (0x11) with flag 0x100 at +0xD8) inside the region
bool FFindTarget(VEHICLE& v, glm::vec3* ppos)
{
    VEHICLERC3& rc = v.rc;
    if (!rc.region || !g_psw) return false;
    const glm::vec3 posSelf = PosAloCenter(v.owner);
    float best = 3.4028235e38f;
    bool found = false;
    for (LO* plo : g_psw->mpcidaplo[0x11])
    {
        if (!plo->fInWorld) continue;
        uint32_t grf = plo->U32Field3(0, 0xD8, 0);
        for (const OPT& o : plo->aopt)
            if (o.eopid == 249 && !o.aval.empty()) grf = uint32_t(o.aval[0].n);
        if (!(grf & 0x100)) continue;
        const glm::vec3 pos = PosLoWorld(plo);
        const glm::vec3 d = pos - posSelf;
        const float s = glm::dot(d, d);
        if (best < s || !FPointInVolume(rc.region, pos)) continue;
        best = s;
        found = true;
        *ppos = pos;
    }
    if (FPrint())
    {
        const glm::mat4 m = MatWorld(rc.region);
        std::printf("rccar3: target from %.0f %.0f %.0f (region %d at %.0f %.0f %.0f): %d\n", posSelf.x, posSelf.y, posSelf.z, rc.region->oid, m[3].x, m[3].y, m[3].z, found);
    }
    return found;
}

// orig3 0x205418: one shot from the next muzzle
void Shoot(VEHICLE& v)
{
    VEHICLERC3& rc = v.rc;
    if (rc.resource && rc.launcher && !rc.muzzles.empty())
    {
        LO* pmuzzle = rc.muzzles[rc.cursor % rc.muzzles.size()];
        // (natively the proxy takes its one instance back while it still flies (the acquire detaches the row); the
        // original's pool gives a free one or none: no shot while one of ours is still in the world)
        bool fBusy = false;
        for (const auto& [plo, t] : rc.launched) fBusy |= plo->fInWorld && g_clock.t - t < 3.0f;
        LO* po = pmuzzle && !fBusy ? PloAcquireProxyObject(g_psw, rc.resource, false) : nullptr;
        if (po && FIsLoDerivedFrom(po, S3CID(0x30)))
        {
            rc.launched[po] = g_clock.t;
            if (FPrint()) std::printf("rccar3: acquired %d in world %d active %d\n", po->oid, po->fInWorld, FLoActive(po));
            // 0x1C8FB0: the shot at the muzzle
            const glm::mat4 m = MatWorld(pmuzzle);
            ALO* palo = static_cast<ALO*>(po);
            SetAloRotation(palo, glm::mat3(m));
            SetAloPos(palo, glm::vec3(m[3]));
            glm::vec3 pos;
            if (!FFindTarget(v, &pos)) pos = glm::vec3(m * glm::vec4(rc.offset, 0, 0, 1));
            SetLauncherAim(rc.launcher, nullptr, pos);
            LaunchFrom(rc.launcher, po);
            if (FIsLoDerivedFrom(po, S3CID(0x4F))) SetProjectileLauncher(palo, v.owner);   // 0x14D1F0
            if (rc.cursor < int(rc.flashes.size()) && rc.flashes[rc.cursor]) TriggerPe(rc.flashes[rc.cursor]);   // 0x1FE728
            if (FPrint())
            {
                LO* proot = po;
                while (proot->paloParent) proot = proot->paloParent;
                std::printf("rccar3: %d shot %d (root %d in world %d active %d) from %d to %.0f %.0f %.0f\n", v.owner->oid, po->oid, proot->oid, po->fInWorld,
                            FLoActive(po), pmuzzle->oid, pos.x, pos.y, pos.z);
            }
            rc.cursor = (rc.cursor + 1) % int(rc.muzzles.size());
        }
    }
    rc.nextShot = g_clock.t + 1.0f / rc.rate;
}

// orig3 0x205238: R1 fires while the gun is not overheated (+0x404 rises to 1, then cools below 0.8)
void UpdateGun(VEHICLE& v)
{
    VEHICLERC3& rc = v.rc;
    if (!rc.resource) return;
    static const cmm::SMP s_smpUp{0.25f, 0.1f, 1.0f}, s_smpDown{0.083f, 0.02f, 0.1f};   // D_00325760 / D_00325770
    float target = 0.0f;
    const cmm::SMP* psmp = &s_smpDown;
    if ((GrfHeld(v) & 8) && !rc.saturated)
    {
        if (rc.nextShot < g_clock.t) Shoot(v);
        target = 1.0f;
        psmp = &s_smpUp;
    }
    rc.heat = Clamp(cmm::GSmooth(rc.heat, target, g_clock.dt, psmp, nullptr), 0.0f, 1.0f);
    if (rc.heat == 1.0f) rc.saturated = true;
    else if (rc.heat < 0.8f) rc.saturated = false;
}

// orig3 0x204CB0: the jump to the anchor (rc.jumpBasis / jumpTarget) unless it is too far
void JumpToAnchor(VEHICLE& v, float limit)
{
    VEHICLERC3& rc = v.rc;
    rc.jumpDuration = 1.0f;
    const float s = glm::length(rc.jumpTarget - PosAloCenter(v.owner));
    // (0x1B83F0's flight time from the distance; natively a fixed time per 500 units, at least 0.5 s)
    const float t = std::max(0.5f, s / 500.0f);
    if (limit < t)
    {
        // too far: put there at once (vt slots 29 / 30)
        SetAloPos(v.owner, rc.jumpTarget);
        SetAloRotation(v.owner, rc.jumpBasis);
        return;
    }
    rc.jumpDuration = t;
    SetRcCarMode3(v, 4);
}

// orig3 0x2047D0: back on the trail (fFlag 0: on its back with few wheels down: straight up where it is)
bool FResetToTrail(VEHICLE& v, bool fFlag)
{
    VEHICLERC3& rc = v.rc;
    if (!fFlag && !FUpright(v) && CwheelContact(v) < 3)
    {
        glm::mat3 level;
        LevelMatrix3(glm::mat3(MatWorld(v.owner)), &level);
        rc.jumpBasis = level;
        rc.jumpTarget = PosAloCenter(v.owner) + glm::vec3(0, 0, 60);
        JumpToAnchor(v, 2.0f);
        return true;
    }
    // (the nearest trail point; natively without the wheels' clearance tests)
    if (rc.trail.empty()) return false;
    const glm::vec3 pos = PosAloCenter(v.owner);
    size_t ibest = 0;
    float best = 3.4028235e38f;
    for (size_t i = 0; i < rc.trail.size(); ++i)
        if (const float s = glm::length(rc.trail[i].first - pos); s < best) best = s, ibest = i;
    rc.jumpTarget = rc.trail[ibest].first;
    rc.jumpBasis = rc.trail[ibest].second;
    JumpToAnchor(v, 2.0f);
    return true;
}

// orig3 0x2045B0: the trail (a point every 200 units while driven upright on four wheels); stalled 0.15 s on its
// back or stuck: back on the trail
void UpdateTrail(VEHICLE& v)
{
    VEHICLERC3& rc = v.rc;
    if (rc.mode != 1) return;
    // 0x205920: on the ground, slower than 50, and either upright, driven and not on four wheels, or on its back and
    // not turning (vt slot 39 x / y within 6)
    bool fStalled = false;
    if (!FAirborne(v) && glm::length(VelAloPos(v.owner)) <= 50.0f)
    {
        if (FUpright(v)) fStalled = v.drive != 0.0f && CwheelContact(v) < 4;
        else
        {
            const glm::vec3 w = AngVelAlo(v.owner);
            fStalled = std::fabs(w.x) < 6.0f && std::fabs(w.y) < 6.0f;
        }
    }
    if (fStalled)
    {
        if (0.15f < g_clock.t - rc.trailStamp) FResetToTrail(v, false);
        return;
    }
    rc.trailStamp = g_clock.t;
    if (!rc.trailOn || !FUpright(v) || CwheelContact(v) != 4) return;
    const glm::vec3 pos = PosAloCenter(v.owner);
    if (!rc.trail.empty() && glm::length(rc.trail.front().first - pos) < 200.0f) return;
    rc.trail.insert(rc.trail.begin(), {pos, glm::mat3(MatWorld(v.owner))});
    if (rc.trail.size() > 32) rc.trail.resize(32);
}

// orig3 0x204048: the control by the mode
void UpdateControl(VEHICLE& v)
{
    VEHICLERC3& rc = v.rc;
    // the jump ends after its time
    if (rc.mode == 4 && rc.jumpDuration < g_clock.t - rc.modeTime) SetRcCarMode3(v, 1);
    switch (rc.mode)
    {
    case 0:
        // (idle: no steering and no drive: the jump table's case 0 at 0x2040D8 branches to 0x20454C, the store of 0 to
        // +0x33C that mode 4 ends in too; m4_rccar's car stops at the start line once 69446 sets mode 0)
        rc.steerGoal = 0.0f;
        v.drive = 0.0f;
        break;
    case 1:
    {
        const PADCTL* ppad = Ppad(v);
        if (v.wheels.size() >= 2 && (v.wheels[0].contacts || v.wheels[1].contacts))
        {
            rc.steerGoal = -(ppad && ppad->nControl == 1 ? ppad->x : 0.0f) * rc.steerMax;
            const float speed = glm::length(VelAloPos(v.owner));
            rc.steerGoal *= Clamp(Poly(rc.steerSpeed, speed), rc.steerLimits.gMin, rc.steerLimits.gMax);
        }
        // (D_0047A964 +0x2F bit 0 stops the drive: not ported)
        const uint32_t grf = GrfHeld(v);
        if (grf & 0x40) v.drive = v.maxSpeed;            // X: +0x32C
        else if (grf & 0xA0) v.drive = -v.rc.slipReverse;   // Square / Circle: -+0x330
        else v.drive = 0.0f;
        break;
    }
    case 2:
    case 3:
    {
        const CRV* pcrv = rc.path ? PcrvFromLo(rc.path) : nullptr;
        if (!pcrv) break;
        const glm::vec3 pos = PosAloCenter(v.owner);
        NearestCrvS(pcrv, pos, rc.pathS, nullptr, nullptr, nullptr, &rc.pathS);
        float s = rc.pathS + rc.lookahead;
        const float sMax = SLengthCrv(pcrv);
        s = pcrv->fClosed ? SWrapCrv(pcrv, s) : Clamp(s, 0.0f, sMax);   // 0x22E0B8 (a closed path wraps)
        glm::vec3 posAim;
        EvalCrvS(pcrv, s, &posAim, nullptr);
        const glm::vec3 local = PosConvertXfs(nullptr, PxfObj(v.owner), posAim);
        const float rad = rc.mode == 2 ? std::atan2(local.y, local.x) : std::atan2(local.y, -local.x);
        rc.steerGoal = cmm::GLimitAbs(cmm::RadNormalize(rad), rc.steerMax);
        if (FPrint())
        {
            static int s_c = 0;
            if (++s_c % 30 == 0)
                std::printf("rccar3: path %d s %.0f of %.0f closed %d aim %.0f %.0f %.0f local %.0f %.0f\n", rc.path->oid, rc.pathS, sMax, pcrv->fClosed, posAim.x, posAim.y,
                            posAim.z, local.x, local.y);
        }
        break;
    }
    case 4:
    {
        // the ballistic jump to the anchor: the velocity set at the mode change, the rotation turned toward it
        const float u = Clamp((g_clock.t - rc.modeTime) / std::max(rc.jumpDuration, 0.001f), 0.0f, 1.0f);
        SetAloRotation(v.owner, glm::mat3_cast(glm::slerp(glm::quat_cast(rc.jumpStart), glm::quat_cast(rc.jumpBasis), u)));
        v.drive = 0.0f;
        break;
    }
    default: break;
    }
    if (rc.mode != 4)
    {
        static const cmm::SMP s_smp{10.0f, 1.0f, 0.2f};   // D_003256F8
        v.turn = cmm::GSmooth(v.turn, rc.steerGoal, g_clock.dt, &s_smp, nullptr);
    }
    if (v.wheels.size() >= 2) v.wheels[0].tilt = v.wheels[1].tilt = v.turn;
}

// orig3 0x203FB0
void UpdateRcCar(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);
    VEHICLE* pv = Pvehicle(plo);
    if (!pv || !pv->rc.on) return;
    VEHICLE& v = *pv;
    // (debug, a route's teleport step for the current player (routetele.h): the car put there, heading H, at rest)
    if (g_routetele.fPending && g_psw && PloPoCurrent(g_psw) == plo)
    {
        g_routetele.fPending = false;
        if (g_routetele.fH) SetAloRotation(v.owner, cmm::BuildRotateMatrixAxis(g_routetele.h, glm::vec3(0, 0, 1)));
        SetAloPos(v.owner, glm::vec3(g_routetele.x, g_routetele.y, g_routetele.z));
        SetAloVelocityTo(v.owner, glm::vec3(0.0f));
        SetAloAngVelocityTo(v.owner, glm::vec3(0.0f));
    }
    UpdateControl(v);
    UpdateGun(v);
    if (v.rc.hurt > 0.0f) v.rc.hurt = std::max(0.0f, v.rc.hurt - g_clock.dt);   // 0x207928 (its flashing not drawn)
    UpdateTrail(v);
    if (v.drive != 0.0f || (v.rc.realign && FAirborne(v))) WakeAlo(v.owner);   // 0x23E228
    if (FPrint())
    {
        static int s_c = 0;
        if (++s_c % 30 == 0)
        {
            const glm::vec3 p = PosAloCenter(v.owner), vel = VelAloPos(v.owner);
            std::printf("rccar3: %d mode %d drive %.0f steer %.2f/%.2f pos %.0f %.0f %.0f vel %.0f wheels %d heat %.2f xf %d %d sost %d hp %d/%d\n", plo->oid, v.rc.mode, v.drive,
                        v.turn, v.rc.steerGoal, p.x, p.y, p.z, glm::length(vel), CwheelContact(v), v.rc.heat, PxfPos(v.owner) ? PxfPos(v.owner)->mode : -9,
                        PxfRot(v.owner) ? PxfRot(v.owner)->mode : -9, Psost(v.owner) != nullptr, NSoHealth3(v.owner), NSoHealthMax3(v.owner));
        }
    }
}

// orig3 0x205A78 (vt slot 74): gravity, the realignment in the air, the four wheels' suspension, the turn within the
// impulse budget, then the drive
void RcCarForces(ALO* owner)
{
    VEHICLE* pv = Pvehicle(owner);
    if (!pv || !pv->rc.on)
    {
        ApplyAloGravity(owner);
        return;
    }
    VEHICLE& v = *pv;
    if (v.rc.mode == 4) return;
    ApplyAloGravity(owner);   // 0x243738
    SOST* physics = Psost(owner);
    XF* position = PxfPos(owner);
    XF* rotation = PxfRot(owner);
    XF* space = PxfObj(owner);
    if (!physics || !position || !rotation || !space || v.wheels.size() < 4 || physics->m <= 0.0f) return;
    if (v.rc.realign && FAirborne(v))
    {
        glm::mat3 level;
        LevelMatrix3(glm::mat3(space->matWorld), &level);
        const CLQ clq{v.rc.realignCurve.x, v.rc.realignCurve.y, v.rc.realignCurve.z, 0};
        AccelAloTowardRotation(owner, &level, &clq, glm::vec3(0.0f), nullptr);   // 0x23EDE8
    }
    float total = 0.0f;
    for (const VEHICLEWHEEL& wheel : v.wheels)
        if (XP* hit = BestVehicleContact(wheel))
        {
            const glm::vec3 point = hit->axpd[0].pos, normal = glm::vec3(space->matWorld[2]);
            const glm::vec3 previous = VelXfPoint(space, nullptr, point);
            const glm::vec3 other = VelXfPoint(hit->axpd[1].pxf, nullptr, point);
            const float fraction = Clamp(hit->gDist / (wheel.top - wheel.bottom), 0.0f, 1.0f);
            const float velocityCurve = Poly({0, 0.25f, 0.75f}, fraction), depthCurve = Poly({0, 0, 1}, fraction);   // D_00325790 / D_003257A0
            float response = glm::dot(other - previous, normal) * velocityCurve * 15.0f + hit->gDist * depthCurve * 90.0f;
            if (response <= 0.0f) continue;
            if (3600.0f < response) response = 3600.0f;
            const glm::vec3 force = normal * (response * physics->m);
            VehicleForceAt(owner, point, force);
            total += glm::dot(force, normal);
        }
    float budget = std::min(11250.0f, v.contactScale * total / physics->m);
    const float dt = g_clock.dt;
    const glm::vec3 velocity = VecConvertXfs(position->pxfRef, rotation, glm::vec3(position->vel) + glm::vec3(position->aseg[3]) * dt);
    const glm::vec3 localVelocity = glm::transpose(glm::mat3(rotation->mat)) * glm::vec3(rotation->vel);
    const float width = v.wheels[0].position.x - v.wheels[2].position.x;
    if (width == 0.0f || physics->momint[2][2] == 0.0f) return;
    const float turn = std::min(velocity.x, 1000.0f) * v.turn / width;
    const float acceleration = 2.0f * physics->m * width / physics->momint[2][2];
    const float turnImpulse = cmm::GLimitAbs((turn - localVelocity.z) / dt, acceleration * budget);
    VehicleAccelRotation(owner, glm::mat3(rotation->matWorld) * glm::vec3(0, 0, turnImpulse));
    budget -= std::fabs(0.2f * turnImpulse / acceleration);
    const glm::mat3 phase = cmm::BuildRotateMatrixAxis((localVelocity.z + turnImpulse * (dt * 0.5f)) * dt, glm::vec3(0, 0, 1));
    const glm::vec3 desired = phase * glm::vec3(v.drive, 0, 0);
    const float fraction = velocity.x / v.maxSpeed;
    float minimum, maximum;
    if (0.0f <= v.drive)
    {
        minimum = -v.reverseLimit;
        maximum = Clamp(Poly(v.traction, Clamp(fraction, 0, 1)), v.tractionLimits.gMin, v.tractionLimits.gMax);
    }
    else
    {
        minimum = -Clamp(Poly(v.traction, Clamp(-fraction, 0, 1)), v.tractionLimits.gMin, v.tractionLimits.gMax);
        maximum = v.reverseLimit;
    }
    glm::vec3 correction = (desired - velocity) / dt;
    correction.z = 0.0f;
    correction.x = Clamp(correction.x, minimum, maximum);
    AccelAloPos(owner, VecConvertXfs(rotation, nullptr, cmm::LimitVectorLength(correction, budget)));   // 0x244828
    if (FPrint())
    {
        static int s_c = 0;
        if (++s_c % 30 == 0)
            std::printf("rccar3: forces total %.0f budget %.0f vel %.0f %.0f %.0f corr %.0f %.0f modes %d %d\n", total, budget, velocity.x, velocity.y, velocity.z,
                        correction.x, correction.y, position->mode, rotation->mode);
    }
}
NATIVEFN3(0x205A78, RcCarForces);

// orig3 0x207FA0 (vt slot 5): the base's, then events 6 / 7 track the object that holds it
void OnRcCarMessage(LO* plo, int msgk, void* pv)
{
    using PFN5 = void (*)(LO*, int, void*);
    if (PFN5 pfn = reinterpret_cast<PFN5>(PvNativeFn(0x1C3E20))) pfn(plo, msgk, pv);   // 0x1F4278 = Sly 2's SO handler
    VEHICLE* pveh = Pvehicle(plo);
    if (!pveh || !pv) return;
    struct TRACK { int key; LO* target; };
    const TRACK* pt = static_cast<const TRACK*>(pv);
    if (msgk == 6 && pt->target == plo) pveh->rc.tracked = pt->key;
    if (msgk == 7 && pt->target == plo && pt->key == pveh->rc.tracked) pveh->rc.tracked = 0;
}

void TraceSet(int eopid, LO* plo, const CRef& r)
{
    if (FPrint()) std::printf("rccar3: %d option %d set (%s %.2f)\n", plo->oid, eopid, r.m_tagk == TAGK_S32 ? "n" : "g", r.m_tagk == TAGK_S32 ? float(r.m_n) : r.GAsFloat());
}

struct INITRCCAR3
{
    INITRCCAR3()
    {
        RegisterLoUpdate(FN3_TAG | 0x203FB0, UpdateRcCar);
        // the splice's wheel method (option 1809, thunk 0x1103F0 -> 0x207568: wheel i's radius and travel)
        RegisterObjectMethod3(0x1103F0, [](LO* plo, const std::vector<CRef>& a) {
            VEHICLE* v = Pvehicle(plo);
            const int i = NArg(a, 0);
            if (v && v->rc.on && i >= 0 && i < int(v->wheels.size()) && a.size() > 2)
            {
                const LM lm = a[2].GetLm();
                v->wheels[i].radius = GArg(a, 1);
                v->wheels[i].bottom = lm.gMin;
                v->wheels[i].top = lm.gMax;
            }
            return CRef(TAGK_Void);
        });
        // the scripts' drive (option 388, +0x33C), mode (525, +0x380) and path (1484, +0x388)
        RegisterObjectSetter3(1825, [](LO* plo, const CRef& r) { TraceSet(1825, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->drive = r.GAsFloat(); });
        RegisterObjectGetter3(1825, [](LO* plo) { CRef r; VEHICLE* v = Pvehicle(plo); r.SetF32(v ? v->drive : 0.0f); return r; });
        RegisterObjectSetter3(1821, [](LO* plo, const CRef& r) { TraceSet(1821, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) SetRcCarMode3(*v, r.RefCoerceS32().m_n); });
        RegisterObjectGetter3(1821, [](LO* plo) { CRef r; VEHICLE* v = Pvehicle(plo); r.SetS32(v ? v->rc.mode : 0); return r; });
        // the objects the splice sets (e.g. the heist's: the gun's proxy 1829, its launcher 1830, its region 1834)
        RegisterObjectSetter3(1822, [](LO* plo, const CRef& r) { TraceSet(1822, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.path = PloRef(plo, r); });
        RegisterObjectSetter3(1829, [](LO* plo, const CRef& r) { TraceSet(1829, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.resource = PloRef(plo, r); });
        RegisterObjectSetter3(1830, [](LO* plo, const CRef& r) { TraceSet(1830, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.launcher = PloRef(plo, r); });
        RegisterObjectSetter3(1834, [](LO* plo, const CRef& r) { TraceSet(1834, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.region = PloRef(plo, r); });
        // (field options the splice sets: m4_rccar's drive to the start line (SCPT 3681, splice event 69443) sets the
        // steering limit 1814 (optid 1476) to 0.4 for the path and 69446 restores the loaded one; a field write alone
        // left +0x334 at the load's 0.19 and the car ran wide of its path)
        RegisterObjectSetter3(1814, [](LO* plo, const CRef& r) { TraceSet(1814, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.steerMax = r.GAsFloat(); });
        RegisterObjectSetter3(1815, [](LO* plo, const CRef& r) { TraceSet(1815, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->maxSpeed = r.GAsFloat(); });
        RegisterObjectSetter3(1816, [](LO* plo, const CRef& r) { TraceSet(1816, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.slipReverse = r.GAsFloat(); });
        RegisterObjectSetter3(1823, [](LO* plo, const CRef& r) { TraceSet(1823, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.lookahead = r.GAsFloat(); });
        RegisterObjectSetter3(1824, [](LO* plo, const CRef& r) { TraceSet(1824, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.rearSteer = r.GAsFloat(); });
        RegisterObjectSetter3(1833, [](LO* plo, const CRef& r) { TraceSet(1833, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on && r.GAsFloat() > 0) v->rc.rate = r.GAsFloat(); });
        RegisterObjectSetter3(1835, [](LO* plo, const CRef& r) { TraceSet(1835, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->rc.on) v->rc.offset = r.GAsFloat(); });
    }
} s_initrccar3;
NATIVEFN3(0x207FA0, OnRcCarMessage);

// orig3 0x207DD8 (vt slot 81, the damage status): 1 while the hit flash runs (+0x39C), else 2 with no hit points left
int NRcCarDamageStatus(LO* plo)
{
    VEHICLE* pv = Pvehicle(plo);
    if (pv && pv->rc.hurt > 0.0f) return 1;
    return NSoHealth3(plo) > 0 ? 0 : 2;
}
NATIVEFN3(0x207DD8, NRcCarDamageStatus);

// orig3 0x206D40 (vt slot 59, Sly 2's 55: a new contact of the car): a swept contact (kind 8) on the car's side gets its
// direction (+0xA0) and its depth again (the lowest of the car's entry's points along it, 0x18B1E0); then by the
// contact's normal (the car's side: as is, else reversed): a wall (z under 0.7) or another RC car slides (friction 0,
// elasticity 0.1); a floor along the car's up axis (dot over 0.7) 0.1 / 0.1, a slope 0.5 / 0.1 (AUDIT4 MODES-21)
void RcCarXpNew(ALO* palo, XP* pxp, int i)
{
    if (!palo || !pxp || i < 0 || i > 1) return;
    if (pxp->xpk == 8 && i == 0 && pxp->pv0)
    {
        pxp->normal = *static_cast<const glm::vec3*>(pxp->pv0);
        if (SOE* psoe = pxp->axpd[0].psoe)
        {
            UpdateSoeWorld(psoe);
            float lowest = 0.0f;
            for (const glm::vec3& v : psoe->geomWorld.apos) lowest = std::min(lowest, glm::dot(v - pxp->axpd[1].pos, pxp->normal));
            pxp->gDist = lowest;
        }
    }
    const glm::vec3 n = i == 0 ? pxp->normal : -pxp->normal;
    ALO* paloOther = pxp->axpd[1 - i].palo;
    if (n.z < 0.7f || (paloOther && FIsLoDerivedFrom(paloOther, S3CID(0x55))))
    {
        pxp->mu = 0.0f;
        pxp->elas = 0.1f;
        return;
    }
    const XF* pxf = PxfObj(palo);
    const glm::vec3 up = pxf ? glm::vec3(pxf->matWorld[2]) : glm::vec3(0.0f, 0.0f, 1.0f);
    pxp->mu = 0.7f < glm::dot(up, n) ? 0.1f : 0.5f;
    pxp->elas = 0.1f;
}
NATIVEFN3(0x206D40, RcCarXpNew);
// orig3 0x207D68 (vt slot 80, a hit kind ignored): while hurt / dead (slot 81) or jumping back to the trail (mode 4);
// then the SO's 0x1F42E8 (the player's invulnerability counts at D_0036F8C0 / D_00370A80, or the override +0x180:
// natively not kept, so no)
int FRcCarIgnoresHit(LO* plo, int)
{
    if (NRcCarDamageStatus(plo)) return 1;
    VEHICLE* pv = Pvehicle(plo);
    return pv && pv->rc.mode == 4 ? 1 : 0;
}
NATIVEFN3(0x207D68, FRcCarIgnoresHit);
// orig3 0x207AE0 (vt slot 78, a hit): not while slot 80 ignores it, contact bytes 2 only, not its own shot; heavy kinds
// 0x2000 / 0x4000 destroy it unless driven on the pad (fatal hits always); else it loses the larger of 5 % of its most
// and the amount less the suck's share; a heavy hit while driven leaves it alive only if it can jump back to the trail
// (0x2047D0). The player's car: rumble 4 for 0.75 s and a camera jolt, then the task event 0xA (alive: the flash +0x39C =
// +0x398) or 0xC (destroyed); someone else's: 0xF / 0x10. Before, the car took no damage (MODES-07)
bool FAcceptRcCarContact(ALO* palo, CTMSG* pctm)
{
    VEHICLE* pv = Pvehicle(palo);
    if (!pv || !pctm) return false;
    VEHICLE& v = *pv;
    const int kind = static_cast<int>(pctm->grfFilter);
    if (FRcCarIgnoresHit(palo, kind)) return false;
    if (!(pctm->grfAccepted & 2)) return false;
    LO* pfrom = pctm->ploSender;
    if (pfrom && FIsLoDerivedFrom(pfrom, 0x59) && PloProjectileLauncher(pfrom) == palo) return false;
    palo->SetField3(0, 0x184, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pfrom) & 0xFFFFFFFFu));   // 0x1F4DD8 (the last hit's source)
    const bool fHeavy = kind == 0x2000 || kind == 0x4000;
    if (pctm->nKill3 || pctm->fKill3 || (v.rc.mode != 1 && fHeavy)) SetSoHealth3(palo, 0);
    else
    {
        int amount = static_cast<int>(static_cast<float>(NSoHealthMax3(palo)) * 0.05f);
        const int cap = static_cast<int>(static_cast<float>(pctm->nState) * (1.0f - GSuck3()));
        if (cap >= amount) amount = cap;
        SetSoHealth3(palo, NSoHealth3(palo) - amount);
    }
    if (NSoHealth3(palo) > 0 && fHeavy && v.rc.mode == 1 && !FResetToTrail(v, true)) SetSoHealth3(palo, 0);
    const bool fAlive = NSoHealth3(palo) > 0;
    if (g_psw && PloPoCurrent(g_psw) == palo)   // 0x1F4350(p, -1)
    {
        TriggerJoyRumbleRumk(4, 0.75f);
        JoltCm(&g_cm, 0);
        if (fAlive)
        {
            Broadcast3(0xA, palo);
            v.rc.hurt = v.rc.hurtTime;
        }
        else Broadcast3(0xC, palo);
    }
    else Broadcast3(fAlive ? 0xF : 0x10, palo);
    if (FPrint()) std::printf("rc3: %d hit by %d (kind 0x%X amount %d): hp %d / %d (t %.2f)\n", palo->oid, pfrom ? pfrom->oid : -1, kind, pctm->nState,
                              NSoHealth3(palo), NSoHealthMax3(palo), g_clock.t);
    return true;
}
NATIVEFN3(0x207AE0, FAcceptRcCarContact);

// orig3 0x203EC8 (vt slot 31, a pool row taken again): the SO's 0x1F3C48, then 0x203F38: the hit points full, no flash
// (+0x39C), the gun's heat / saturation / tracking cleared (+0x404..+0x414), the trail on (+0x458), the mode 0 (0x2075E8)
// (MODES-21)
void ResetRcCarSlot31(LO* plo, LO*)
{
    VEHICLE* pv = Pvehicle(plo);
    if (!pv) return;
    VEHICLE& v = *pv;
    SetSoHealth3(plo, NSoHealthMax3(plo));
    v.rc.hurt = 0.0f;
    v.rc.heat = 0.0f;
    v.rc.saturated = false;
    v.rc.tracked = 0;
    v.rc.trailOn = true;
    SetRcCarMode3(v, 0);
}
NATIVEFN3(0x203EC8, ResetRcCarSlot31);
// orig3 0x203EF8 (vt slot 32, the detach): SO's (0x1F3C80), then the clips +0x420 / +0x424 retracted and the engine
// sounds +0x3E8 / +0x3F4 stopped (0x205168): natively the car holds neither (MODES-21)
void DetachRcCar32(LO* plo, LO* pproxy) { DetachSoBase32(plo, pproxy); }
NATIVEFN3(0x203EF8, DetachRcCar32);
}  // namespace

bool FIsRcCar3(const LO* plo) { return plo && plo->pcls && FIsLoDerivedFrom(plo, 0x55); }

// orig3 0x00208030 (vt slot 88, a pickup collected, pickup.cpp GivePickupToCollector3): from a charm (0x69) its +0x1AC
// more hit points (0x1F43D0: up to the most) (MODES-21)
void TakePickupRcCar3(LO* pso, LO* ppickup)
{
    if (!FIsRcCar3(pso) || !ppickup || !FIsLoDerivedFrom(ppickup, 0x69)) return;
    SetSoHealth3(pso, NSoHealth3(pso) + static_cast<int32_t>(ppickup->U32Field3(0, 0x1AC, 0)));
}

// orig3 0x203C18: the SO's init, then the tuning
void InitRcCar3(VEHICLE& v)
{
    VEHICLERC3& rc = v.rc;
    rc.on = true;
    v.maxSpeed = 1100.0f;               // +0x32C
    rc.slipReverse = 700.0f;            // +0x330
    v.contactScale = 1.5f;              // +0x338
    rc.steerMax = 0.8f;                 // +0x334
    v.traction = {2500.0f, -1500.0f, 0.0f};   // +0x310
    v.tractionLimits = {0.0f, 2500.0f};       // +0x320
    v.reverseLimit = 3500.0f;           // +0x328
    rc.hurtTime = 1.0f;                 // +0x398
    rc.rate = 10.0f;                    // +0x3D8
    rc.offset = 3000.0f;                // +0x3DC
    rc.realign = true;                  // +0x428
    rc.realignCurve = {4.0f, 0.0f, 0.5f};
    rc.steerSpeed = {1.0f, 0.0f, 0.0f};
    rc.steerLimits = {0.0f, 1.0f};
    // the hit points: 100 (0x1F43F8(p, 100)) unless loaded (EOPID 937); full (the world start's slot 31 0x203EC8 ->
    // 0x203F38 fills them: natively only the hack objects get that call, so here)
    {
        int nMax = 100;
        for (const OPT& o : v.owner->aopt)
            if (o.eopid == 937 && !o.aval.empty()) nMax = o.aval[0].n;
        v.owner->SetField3(0, 0x170, static_cast<uint32_t>(nMax));
        v.owner->SetField3(0, 0x16C, static_cast<uint32_t>(nMax));
    }
    // the four wheels (0x207568 with D_003256E0 = {-40, 20}): radius 50, parts 0x76D..0x770 (D_003256E8)
    v.wheels.assign(4, VEHICLEWHEEL{});
    for (int i = 0; i < 4; ++i)
    {
        v.wheels[i].id = 0x76D + i;
        v.wheels[i].radius = 50.0f;
        v.wheels[i].bottom = -40.0f;
        v.wheels[i].top = 20.0f;
    }
}

// orig3 0x203D88: the base's post-load, the wheels' places (parts 0x76D.. of kind 0xD); the options
void PostLoadRcCar3(VEHICLE& v)
{
    ALO* p = v.owner;
    VEHICLERC3& rc = v.rc;
    for (const OPT& o : p->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& a = o.aval.front();
        switch (o.eopid)
        {
        case 1809:   // the wheel method's loaded calls (0x207568): wheel i, radius, travel
            if (o.aval.size() >= 3 && o.aval[0].n >= 0 && o.aval[0].n < int(v.wheels.size()))
            {
                VEHICLEWHEEL& w = v.wheels[o.aval[0].n];
                w.radius = o.aval[1].ag[0];
                w.bottom = o.aval[2].ag[0];
                w.top = o.aval[2].ag[1];
            }
            break;
        case 1814: rc.steerMax = a.ag[0]; break;
        case 1815: v.maxSpeed = a.ag[0]; break;
        case 1816: rc.slipReverse = a.ag[0]; break;
        case 1817: v.traction = {a.ag[0], a.ag[1], a.ag[2]}; break;
        case 1818: v.tractionLimits = {a.ag[0], a.ag[1]}; break;
        case 1819: v.reverseLimit = a.ag[0]; break;
        case 1820: v.contactScale = a.ag[0]; break;
        case 1822: rc.path = PloOpt(p, a); break;
        case 1823: rc.lookahead = a.ag[0]; break;
        case 1824: rc.rearSteer = a.ag[0]; break;
        case 1825: v.drive = a.ag[0]; break;
        case 1827: rc.hurtTime = a.ag[0]; break;
        case 1829: rc.resource = PloOpt(p, a); break;
        case 1830: rc.launcher = PloOpt(p, a); break;
        case 1831: for (const OPTVAL& e : o.aval) if (rc.muzzles.size() < 4) rc.muzzles.push_back(PloOpt(p, e)); break;   // 0x207598
        case 1832: for (const OPTVAL& e : o.aval) if (rc.flashes.size() < 4) rc.flashes.push_back(PloOpt(p, e)); break;   // 0x2075C0
        case 1833: rc.rate = a.ag[0]; break;
        case 1834: rc.region = PloOpt(p, a); break;
        case 1835: rc.offset = a.ag[0]; break;
        case 1840: rc.realign = a.n != 0; break;
        case 1841: rc.realignCurve = {a.ag[0], a.ag[1], a.ag[2]}; break;
        case 1843: rc.steerSpeed = {a.ag[0], a.ag[1], a.ag[2]}; break;
        case 1844: rc.steerLimits = {a.ag[0], a.ag[1]}; break;
        case 1846: rc.trailOn = a.n != 0; break;
        default: break;
        }
    }
    if (rc.rate <= 0.0f) rc.rate = 10.0f;
    for (VEHICLEWHEEL& w : v.wheels)
    {
        w.node = nullptr;
        for (const auto& pxf : p->axf)
            if (pxf->tag == w.id && pxf->kind == 13) { w.node = pxf.get(); break; }
        if (!w.node) continue;
        w.position = PosConvertXfs(nullptr, PxfObj(p), glm::vec3(w.node->matWorld[3]));
        w.upper = w.position + glm::vec3(0, 0, w.top);
        w.lower = w.position + glm::vec3(0, 0, w.bottom);
    }
    // (a car without its four wheel parts keeps none: no suspension, as Sly 2's vehicle drops missing wheels)
    bool fAll = true;
    for (const VEHICLEWHEEL& w : v.wheels) fAll &= w.node != nullptr;
    if (!fAll) v.wheels.clear();
    v.width = v.wheels.size() >= 4 ? v.wheels[0].position.x - v.wheels[2].position.x : 0.0f;
    rc.mode = 0;   // the reset 0x203F38
    if (FPrint())
    {
        SOST* ps = Psost(p);
        std::printf("rccar3: %d xf %d %d cnstr %d %d\n", p->oid, PxfPos(p) ? PxfPos(p)->mode : -9, PxfRot(p) ? PxfRot(p)->mode : -9, ps ? ps->cnstrLin.kind : -9,
                    ps ? ps->cnstrAng.kind : -9);
        std::printf("rccar3: %d post-load wheels %zu width %.1f resource %d launcher %d muzzles %zu region %d path %d drive %.0f\n", p->oid,
                    v.wheels.size(), v.width, rc.resource ? rc.resource->oid : -1, rc.launcher ? rc.launcher->oid : -1, rc.muzzles.size(),
                    rc.region ? rc.region->oid : -1, rc.path ? rc.path->oid : -1, v.drive);
        for (const VEHICLEWHEEL& w : v.wheels) std::printf("rccar3:   wheel %d at %.1f %.1f %.1f\n", w.id, w.position.x, w.position.y, w.position.z);
        for (const OPT& o : p->aopt)
            std::printf("rccar3:   option %d: %zu values, n %d g %.2f basic %p\n", o.eopid, o.aval.size(), o.aval.empty() ? 0 : o.aval[0].n,
                        o.aval.empty() ? 0.0f : o.aval[0].ag[0], o.aval.empty() ? nullptr : (void*)o.aval[0].pbasic);
    }
}

// orig3 0x2075E8
void SetRcCarMode3(VEHICLE& v, int mode)
{
    VEHICLERC3& rc = v.rc;
    if (mode == rc.mode) return;
    if (rc.mode == 4)
    {
        // (leaving the jump: the physics on again with no velocity (0x23EBF8 / 0x23EC98 / 0x23FC90 0, vt slots 40 / 41))
        SetAloVelocityTo(v.owner, glm::vec3(0.0f));
        SetAloAngVelocityTo(v.owner, glm::vec3(0.0f));
        rc.trailOn = true;
    }
    if (FPrint()) std::printf("rccar3: %d mode %d -> %d\n", v.owner->oid, rc.mode, mode);
    rc.mode = mode;
    rc.modeTime = g_clock.t;
    switch (mode)
    {
    case 2:
    case 3:
        if (const CRV* pcrv = rc.path ? PcrvFromLo(rc.path) : nullptr)
            NearestCrvAll(pcrv, PosAloCenter(v.owner), nullptr, nullptr, nullptr, &rc.pathS);   // 0x22EA88
        break;
    case 4:
    {
        // the ballistic flight: v = d / t - g t / 2 (D_00325880: gravity -1300), turning from the current basis to
        // the anchor's (0x11CFC0 / 0x11D0B8: the angle and axis of the difference)
        const glm::vec3 d = rc.jumpTarget - PosAloCenter(v.owner);
        SetAloVelocityTo(v.owner, d / rc.jumpDuration + glm::vec3(0, 0, 1300.0f) * (rc.jumpDuration * 0.5f));
        SetAloAngVelocityTo(v.owner, glm::vec3(0.0f));
        rc.jumpStart = glm::mat3(MatWorld(v.owner));
        break;
    }
    default: break;
    }
}

// orig3 0x207E18 (vt slot 83, after the SO's 0x1F4548): with the pad the pad's drive, else idle
void OnRcCarEnable3(LO* plo, int n)
{
    VEHICLE* v = Pvehicle(plo);
    if (!v || !v->rc.on) return;
    SetRcCarMode3(*v, n == 1 ? 1 : 0);
    if (FPrint())
    {
        LO* proot = plo;
        while (proot->paloParent) proot = proot->paloParent;
        SOST* ps = Psost(v->owner);
        std::printf("rccar3: %d enable %d: in world %d root %d active %d iupd %d grfUpd 0x%X st %d xf %d %d cnstr %d %d\n", plo->oid, n, plo->fInWorld, proot->oid,
                    proot->fNodeActive, plo->iupd, plo->grfUpd, plo->st, PxfPos(v->owner) ? PxfPos(v->owner)->mode : -9, PxfRot(v->owner) ? PxfRot(v->owner)->mode : -9,
                    ps ? ps->cnstrLin.kind : -9, ps ? ps->cnstrAng.kind : -9);
    }
}
