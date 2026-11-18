// dumptruck3.cpp: Sly 3's dump truck (class 0x51, orig3 0x279DD8..0x27CBE0), from the Sly 3 decomp.
//
// The Ep2 quarry's truck (120533, o_quarry world 9: m2_dumptruck 2707..2721, the towers and the scorpions). The RC
// car's sibling (rccar3.cpp, the same VEHICLE record): the init 0x279DD8 has the car's tuning, the post-load 0x279F00
// finds the wheels (parts 0x76D..0x770, kind 0xD), the ram sensors (parts 0x771 / 0x772) and the body entry (0x773).
// Here: the update 0x27A0B0 (mode 1 the pad: X drives, Square / Circle reverse, the stick steers, Triangle the horn;
// modes 2 / 3 a path), the mode change 0x27C860 (option 525), the sound mix 0x27A658, the forces 0x27ADC8 (vt slot 74),
// the contacts 0x27BF98 (slot 63: the ram sensors while they have a filter, the body's impacts, the wheels), the
// overlap 0x27C1C0 (slot 58), the release 0x27A068 (slot 32); the bounds (slot 54 0x27C278) and the drawn wheels
// (0x27C2F8) are vehicle.cpp's.
// Not ported: the rumble, the body lean (part 0x22's controller), the wheels' dust (0x774), slot 59 (0x27C238: the new
// contacts' mu / elasticity 0.5 / 0.1) and slot 85 (0x27C9E8), the watched objects' list (0x1F4670).
// Debug: SLY3_TRUCKPRINT=1.

#include "vehicle.h"

#include "brx.h"
#include "bsp.h"
#include "clock.h"
#include "cm.h"
#include "crv.h"
#include "objmethod.h"
#include "pe.h"
#include "po.h"
#include "routetele.h"
#include "so.h"
#include "sound.h"
#include "spliceglue.h"
#include "sw.h"
#include "update.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
#include "xfphys.h"
#include "game/sly3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

ALO* PaloXpFilter(ALO* p, uint32_t f);   // mark.cpp (0x1F78D8 / orig3 0x240E10)

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_TRUCKPRINT") != nullptr;
    return s_f;
}
float Poly(const glm::vec3& p, float x) { return p.x + x * (p.y + x * p.z); }
float Clamp(float x, float lo, float hi) { return x < lo ? lo : hi < x ? hi : x; }
float Clamp01(float x) { return Clamp(x, 0.0f, 1.0f); }

glm::mat3 MatRot(ALO* p)
{
    XF* pxf = PxfObj(p);
    return pxf ? glm::mat3(pxf->matWorld) : glm::mat3(1.0f);
}
PADCTL* Ppad(VEHICLE& v) { return PpadctlOfSo3(v.owner); }

LO* PloOpt(LO* plo, const OPTVAL& value)
{
    if (value.pbasic) return dynamic_cast<LO*>(value.pbasic);
    return value.n == -1 || value.n == 0 ? nullptr : PloFindRelative(plo, value.n);
}
LO* PloRef(LO* plo, const CRef& r)
{
    if (r.m_tagk == TAGK_S32) return r.m_n == -1 || r.m_n == 0 ? nullptr : PloFindRelative(plo, r.m_n);
    return dynamic_cast<LO*>(r.m_pbasic);
}
SOE* PsoeByTag(ALO* p, int tag)
{
    for (SOE& e : p->asoe)
        if (e.tag == tag) return &e;
    return nullptr;
}

// the sound refs (0x2459F8 + 0x244F08 + 0x2BBCD8: made, started, kept; 0x244CA8 + 0x2BBCD8(0): stopped, dropped)
void StartLoop(CSndRef& ref, int id)
{
    if (ref || id == -1) return;
    ref = CSndRef(PsndStartSound(id));
}
void StopLoop(CSndRef& ref)
{
    if (!ref) return;
    ref->Stop(0.0f);
    ref = CSndRef();
}
// orig3 0x27A610: the clamped quadratic ease-out
float GEase(float x)
{
    x = Clamp01(x);
    return (x + x) - x * x;
}

// orig3 0x27C860 (option 525's setter): the drive loops with the change to / from off (-1 / 0), the path's nearest
// point for modes 2 / 3
void SetTruckMode(VEHICLE& v, int mode)
{
    VEHICLERC3& rc = v.rc;
    VEHICLETRUCK3& t = v.truck;
    if (mode == rc.mode) return;
    const auto off = [](int k) { return k == -1 || k == 0; };
    if (off(rc.mode) != off(mode))
    {
        if (off(mode))
        {
            StopLoop(t.loops[0]);
            StopLoop(t.loops[1]);
        }
        else
        {
            StartLoop(t.loops[0], t.sounds[2]);
            StartLoop(t.loops[1], t.sounds[3]);
            if (t.loops[0]) t.loops[0]->SetVolume(0.0f);
            if (t.loops[1]) t.loops[1]->SetVolume(0.0f);
        }
    }
    if (FPrint()) std::printf("truck3: %d mode %d -> %d\n", v.owner->oid, rc.mode, mode);
    rc.mode = mode;
    rc.modeTime = g_clock.t;
    if (mode == 2 || mode == 3)
        if (const CRV* pcrv = rc.path ? PcrvFromLo(rc.path) : nullptr)
            NearestCrvAll(pcrv, PosAloCenter(v.owner), nullptr, nullptr, nullptr, &rc.pathS);   // 0x22EA88
}

// orig3 0x27A658: the engine's two layers by the tyres' slips and the load (the forces' +0x40C / +0x410 / +0x408), the
// drive loops by speed and the rear wheels' spin, the reverse loop
void UpdateTruckSound(VEHICLE& v)
{
    VEHICLETRUCK3& t = v.truck;
    static const cmm::SMP s_smpSpeed{2.0f, 0.0f, 0.25f}, s_smpVolume{10.0f, 0.0f, 0.1f};
    t.speedSmooth = cmm::GSmooth(t.speedSmooth, std::fabs(v.drive) / v.maxSpeed, g_clock.dt, &s_smpSpeed, nullptr);
    float slip = 0.0f;
    for (size_t i = 2; i < v.wheels.size() && i < 4; ++i) slip += std::fabs(v.wheels[i].speed) * v.wheels[i].radius * 0.5f / v.maxSpeed;   // (the rear wheels' rims)
    const float mix = t.speedSmooth * 0.39999998f + slip * 0.6f;
    if (t.loops[0]) t.loops[0]->SetVolume(GEase(1.0f - mix)), t.loops[0]->SetPitch(0.3f * mix);
    if (t.loops[1]) t.loops[1]->SetVolume(GEase(mix)), t.loops[1]->SetPitch(-0.3f * (1.0f - mix));
    const auto lim = [](float g) { return Clamp01(g); };   // D_00341FF0 = {0, 1}
    const float gThrottle = lim(Poly({-0.01f, 1.26f, -0.5f}, Clamp01(t.slipSide)));
    const float gBrake = lim(Poly({-0.1f, 3.0f, -1.9f}, Clamp01(t.slipFwd)));
    const float gRpm = lim(Poly({-1.25f, 1.0f, 0.0f}, t.load));
    const float gScale = lim(Poly({0.0f, 2.0f, 0.0f}, t.load));
    const float wThrottle = gThrottle * gScale, wBrake = gBrake * gScale, sum = wThrottle + wBrake + gRpm;
    t.volume = cmm::GSmooth(t.volume, Clamp01(sum), g_clock.dt, &s_smpVolume, nullptr);
    if (t.volume <= 0.0f)
    {
        StopLoop(t.engine[0]);
        StopLoop(t.engine[1]);
    }
    else
    {
        StartLoop(t.engine[0], t.sounds[0]);
        StartLoop(t.engine[1], t.sounds[1]);
        float u = wThrottle * lim(Poly({-0.5f, 1.25f, 0.0f}, Clamp01(t.slipSide))) + wBrake * lim(Poly({0.75f, 0.5f, 0.0f}, Clamp01(t.slipFwd))) +
                  gRpm * lim(Poly({1.0f, 0.0f, 0.0f}, t.load));
        if (sum != 0.0f) u /= sum;
        if (t.engine[0]) t.engine[0]->SetVolume(GEase(t.volume * (1.0f - u)));
        if (t.engine[1]) t.engine[1]->SetVolume(GEase(t.volume * u));
    }
    if (v.drive >= 0.0f) StopLoop(t.reverse);
    else StartLoop(t.reverse, t.sounds[4]);
}

// orig3 0x27A0B0 (vt slot 18)
void UpdateTruck(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    VEHICLE* pv = Pvehicle(plo);
    if (!pv || !pv->truck.on) return;
    VEHICLE& v = *pv;
    VEHICLERC3& rc = v.rc;
    VEHICLETRUCK3& t = v.truck;
    // (debug, a route's teleport step for the current player (routetele.h): the truck put there, heading H, at rest)
    if (g_routetele.fPending && g_psw && PloPoCurrent(g_psw) == plo)
    {
        g_routetele.fPending = false;
        if (g_routetele.fH) SetAloRotation(v.owner, cmm::BuildRotateMatrixAxis(g_routetele.h, glm::vec3(0, 0, 1)));
        SetAloPos(v.owner, glm::vec3(g_routetele.x, g_routetele.y, g_routetele.z));
        SetAloVelocityTo(v.owner, glm::vec3(0.0f));
        SetAloAngVelocityTo(v.owner, glm::vec3(0.0f));
    }
    switch (rc.mode)
    {
    case 0:
        v.turn = 0.0f;
        v.drive = 0.0f;
        break;
    case 1:
    {
        static const cmm::SMP s_smp{10.0f, 1.0f, 0.2f};
        const PADCTL* ppad = Ppad(v);
        const bool fPad = ppad && ppad->nControl == 1;
        v.turn = cmm::GSmooth(v.turn, -(fPad ? ppad->x : 0.0f) * rc.steerMax, g_clock.dt, &s_smp, nullptr);
        const uint32_t grf = fPad ? ppad->grfbtn : 0;
        if (grf & 0x40) v.drive = v.maxSpeed;               // X: +0x32C
        else if (grf & 0xA0) v.drive = -rc.slipReverse;     // Square / Circle: -+0x330
        else v.drive = 0.0f;
        // (the rumble every 0.25 s while driven: not ported)
        if (t.sounds[6] != -1)
        {
            if (grf & 0x10)
            {
                if (!t.horn && 0.25f < g_clock.t - t.hornTime) StartLoop(t.horn, t.sounds[6]);
            }
            else if (t.horn)
            {
                StopLoop(t.horn);
                t.hornTime = g_clock.t;
            }
        }
        break;
    }
    case 2:
    case 3:
    {
        const CRV* pcrv = rc.path ? PcrvFromLo(rc.path) : nullptr;
        if (!pcrv) break;
        NearestCrvS(pcrv, PosAloCenter(v.owner), rc.pathS, nullptr, nullptr, nullptr, &rc.pathS);   // 0x22EEA0
        float s = rc.pathS + rc.lookahead;
        s = pcrv->fClosed ? SWrapCrv(pcrv, s) : Clamp(s, 0.0f, SLengthCrv(pcrv));                   // 0x22E0B8
        glm::vec3 posAim;
        EvalCrvS(pcrv, s, &posAim, nullptr);                                                      // 0x22E330
        const glm::vec3 local = PosConvertXfs(nullptr, PxfObj(v.owner), posAim);
        const float rad = rc.mode == 2 ? std::atan2(local.y, local.x) : std::atan2(local.y, -local.x);
        v.turn = cmm::GLimitAbs(cmm::RadNormalize(rad), rc.steerMax);                              // 0x11E3B8 / 0x11E560
        break;
    }
    default: break;
    }
    if (v.wheels.size() >= 2) v.wheels[0].tilt = v.wheels[1].tilt = v.turn;   // +0x198 / +0x1F8
    WakeAlo(v.owner);                                                          // 0x23E228
    // the ram sensors: a filter (0x200: e.g. the towers' and the scorpions' contact byte) while faster than 600
    const glm::vec3 vel = VecConvertXfs(nullptr, PxfObj(v.owner), VelAloPos(v.owner));   // 0x1C94E8
    if (t.bumpers[0]) t.bumpers[0]->id = vel.x > 600.0f ? 0x200 : 0;
    if (t.bumpers[1]) t.bumpers[1]->id = vel.x < -600.0f ? 0x200 : 0;
    if (MatRot(v.owner)[2].z > 0.5f) t.uprightTime = g_clock.t;
    // the body's impacts: the effect and a camera jolt once per landing when hard enough (FCheckLm 0x11EE30)
    if (t.impacts)
    {
        if (t.impactPe && t.impactTime < t.uprightTime)
        {
            float speed = glm::length(t.impacts->dv);
            if (speed == 0.0f)
                speed = glm::length(VelXfPoint(t.impacts->axpd[0].pxf, nullptr, t.impacts->axpd[0].pos) -
                                    VelXfPoint(t.impacts->axpd[1].pxf, nullptr, t.impacts->axpd[1].pos));
            if (t.impactRange.gMin <= speed && speed <= t.impactRange.gMax)
            {
                TriggerPe(t.impactPe);   // 0x1FE728
                JoltCm(&g_cm, 0);         // 0x167D38
                t.impactTime = g_clock.t;
                if (FPrint()) std::printf("truck3: %d impact %.0f\n", plo->oid, speed);
            }
        }
        FreeSwXpList(t.impacts);
        t.impacts = nullptr;
    }
    UpdateTruckSound(v);
    if (FPrint())
    {
        static int s_c = 0;
        if (++s_c % 30 == 0)
        {
            const glm::vec3 p = PosAloCenter(v.owner);
            int c = 0;
            for (const VEHICLEWHEEL& w : v.wheels) c += w.contacts != nullptr;
            std::printf("truck3: %d %p mode %d drive %.0f turn %.2f pos %.0f %.0f %.0f vel %.0f wheels %d rams %X %X\n", plo->oid, (void*)plo, rc.mode, v.drive, v.turn, p.x, p.y,
                        p.z, vel.x, c, t.bumpers[0] ? t.bumpers[0]->id : 0, t.bumpers[1] ? t.bumpers[1]->id : 0);
        }
    }
}

// orig3 0x27ADC8 (vt slot 74): gravity, the four wheels' suspension (and their slips), the turn within the impulse
// budget, then the drive (the RC car's 0x205A78 without the realignment; a quarter width in the turn's lever)
void TruckForces(ALO* owner)
{
    ApplyAloGravity(owner);   // 0x243738
    VEHICLE* pv = Pvehicle(owner);
    if (!pv || !pv->truck.on) return;
    VEHICLE& v = *pv;
    VEHICLETRUCK3& t = v.truck;
    SOST* physics = Psost(owner);
    XF* position = PxfPos(owner);
    XF* rotation = PxfRot(owner);
    XF* space = PxfObj(owner);
    if (!physics || !position || !rotation || !space || v.wheels.size() < 4 || physics->m <= 0.0f) return;
    const glm::mat3 basis(space->matWorld);
    const glm::vec3 normal = basis[2];
    float total = 0.0f;
    t.slipSide = t.slipFwd = 0.0f;
    for (const VEHICLEWHEEL& wheel : v.wheels)
        if (XP* hit = BestVehicleContact(wheel))   // 0x27AD80
        {
            const glm::vec3 point = hit->axpd[0].pos;
            const glm::vec3 previous = VelXfPoint(space, nullptr, point);
            const glm::vec3 other = VelXfPoint(hit->axpd[1].pxf, nullptr, point);
            const float fraction = Clamp01(hit->gDist / (wheel.top - wheel.bottom));
            const glm::vec3 dv = other - previous;
            float response = glm::dot(dv, normal) * Poly({0, 0.25f, 0.75f}, fraction) * 15.0f + hit->gDist * Poly({0, 0, 1}, fraction) * 90.0f;   // D_003293B0 / C0
            if (response < 0.0f) continue;
            response *= physics->m;
            total += response;
            VehicleForceAt(owner, point, normal * response);   // 0x244770
            const glm::vec3 tangent = dv - glm::dot(dv, hit->normal) * hit->normal;
            const glm::mat3 m = basis * cmm::BuildRotateMatrixAxis(wheel.tilt, glm::vec3(0, 0, 1));
            t.slipFwd += response * std::fabs(glm::dot(tangent, m[1])) / t.slipScale;
            t.slipSide += response * std::fabs(glm::dot(tangent, m[0])) / v.maxSpeed;
        }
    if (0.0f < total)
    {
        t.slipFwd /= total;
        t.slipSide /= total;
    }
    t.load = physics->dvGravity.z != 0.0f ? total / (physics->m * -physics->dvGravity.z) : 0.0f;
    float budget = std::min(11250.0f, v.contactScale * total / physics->m);
    const float dt = g_clock.dt;
    const glm::vec3 velocity = VecConvertXfs(position->pxfRef, rotation, glm::vec3(position->vel) + glm::vec3(position->aseg[3]) * dt);
    const glm::vec3 localVelocity = glm::transpose(glm::mat3(rotation->mat)) * glm::vec3(rotation->vel);
    const float width = v.wheels[0].position.x - v.wheels[2].position.x;
    if (width == 0.0f || physics->momint[2][2] == 0.0f) return;
    const float turn = std::min(velocity.x, 1000.0f) * v.turn / width;
    const float accel = physics->m * (width * 0.25f) / physics->momint[2][2];
    const float turnImpulse = cmm::GLimitAbs((turn - localVelocity.z) / dt, accel * budget);
    budget -= std::fabs(turnImpulse * 0.5f / accel);
    VehicleAccelRotation(owner, glm::mat3(rotation->matWorld) * glm::vec3(0, 0, turnImpulse));   // 0x244890
    const glm::mat3 phase = cmm::BuildRotateMatrixAxis((localVelocity.z + turnImpulse * (dt * 0.5f)) * dt, glm::vec3(0, 0, 1));
    const glm::vec3 desired = phase * glm::vec3(v.drive, 0, 0);
    const float fraction = velocity.x / v.maxSpeed;
    float minimum, maximum;
    if (0.0f <= v.drive)
    {
        minimum = -v.reverseLimit;
        maximum = Clamp(Poly(v.traction, Clamp01(fraction)), v.tractionLimits.gMin, v.tractionLimits.gMax);
    }
    else
    {
        minimum = -Clamp(Poly(v.traction, Clamp01(-fraction)), v.tractionLimits.gMin, v.tractionLimits.gMax);
        maximum = v.reverseLimit;
    }
    glm::vec3 correction = (desired - velocity) / dt;
    correction.z = 0.0f;
    correction.x = Clamp(correction.x, minimum, maximum);
    AccelAloPos(owner, VecConvertXfs(rotation, nullptr, cmm::LimitVectorLength(correction, budget)));   // 0x244828
    // (the body's lean toward the impulse (part 0x22's controller, +0x370): not ported)
}
NATIVEFN3(0x27ADC8, TruckForces);

// orig3 0x27BF98 (vt slot 63, the other's entry k): the ram sensors' contacts while one has a filter the other passes
// (0x240E10), the body's (0x773) into the impacts unless it is off (+0x18 bit 19), then the wheels' supports when the
// other is still (vt slot 66) (0x1CD858 for a sphere entry, else 0x1CC790)
void TruckOwnContacts(ALO* owner, SOE* entry, int inodeLimit, const BSPREF& tree, int root, XP** ppxp)
{
    VEHICLE* pv = Pvehicle(owner);
    if (!pv || !pv->truck.on || !entry) return;
    VEHICLETRUCK3& t = pv->truck;
    const auto mark = [&](SOE* own, XP** pp) {
        if (entry->f49) MarkSphereBspContacts(entry, own, nullptr, pp);
        else MarkBspBspContacts(own, entry, entry->bsp.abspn, entry->bsp.abspn.empty() ? IBSP_NONE : 0, pp);
    };
    SOE* const rams[2] = {t.bumpers[0], t.bumpers[1]};
    const uint32_t key = rams[0] && rams[0]->id ? rams[0]->id : rams[1] && rams[1]->id ? rams[1]->id : 0;
    if (key && entry->palo && PaloXpFilter(entry->palo, key))
        for (SOE* ram : rams)
            if (ram && ram->id)
            {
                XP* before = *ppxp;
                mark(ram, ppxp);
                if (FPrint() && *ppxp != before) std::printf("truck3: %d ram %d hits %d\n", owner->oid, ram->tag, entry->palo->oid);
            }
    if (t.body && !t.body->f51) mark(t.body, &t.impacts);
    VehicleOwnContacts(owner, entry, inodeLimit, tree, root, ppxp);   // (the still test and the four wheels: 0x27B588)
}
NATIVEFN3(0x27BF98, TruckOwnContacts);

// orig3 0x27C1C0 (vt slot 58): the base's (0x23EDC8), then the pair's contacts dropped from the impacts and the wheels
void TruckRemoveContacts(ALO* owner, ALO* other)
{
    VEHICLE* pv = Pvehicle(owner);
    if (!pv || !pv->truck.on) return;
    XP** cursor = &pv->truck.impacts;
    XP* removed = nullptr;
    while (XP* hit = *cursor)
    {
        const int side = hit->axpd[0].paloRoot == owner ? 1 : 0;
        if (hit->axpd[side].paloRoot == other)
        {
            *cursor = hit->pxpNext;
            hit->pxpNext = removed;
            removed = hit;
        }
        else cursor = &hit->pxpNext;
    }
    FreeSwXpList(removed);
    VehicleRemoveContacts(owner, other);
}
NATIVEFN3(0x27C1C0, TruckRemoveContacts);

// orig3 0x27A068 (vt slot 32, the proxy's release): the base's (0x1F3C80), the impacts freed, off
void ReleaseTruck(LO* plo, LO*)
{
    VEHICLE* pv = Pvehicle(plo);
    if (!pv || !pv->truck.on) return;
    FreeSwXpList(pv->truck.impacts);
    pv->truck.impacts = nullptr;
    SetTruckMode(*pv, 0);
}
NATIVEFN3(0x27A068, ReleaseTruck);

void TraceSet(int eopid, LO* plo, const CRef& r)
{
    if (FPrint()) std::printf("truck3: %d option %d set (%s %.2f)\n", plo->oid, eopid, r.m_tagk == TAGK_S32 ? "n" : "g", r.m_tagk == TAGK_S32 ? float(r.m_n) : r.GAsFloat());
}

struct INITTRUCK3
{
    INITTRUCK3()
    {
        RegisterLoUpdate(FN3_TAG | 0x27A0B0, UpdateTruck);
        // the splice's wheel method (option 1617, thunk 0x10FBF8 -> 0x27C830: wheel i's radius and travel)
        RegisterObjectMethod3(0x10FBF8, [](LO* plo, const std::vector<CRef>& a) {
            VEHICLE* v = Pvehicle(plo);
            const int i = NArg(a, 0);
            if (v && v->truck.on && i >= 0 && i < int(v->wheels.size()) && a.size() > 2)
            {
                const LM lm = a[2].GetLm();
                v->wheels[i].radius = GArg(a, 1);
                v->wheels[i].bottom = lm.gMin;
                v->wheels[i].top = lm.gMax;
            }
            return CRef(TAGK_Void);
        });
        // the scripts' mode (525 = 1632, the setter 0x27C860), path (1484 = 1633), lookahead, rear wheels, drive (388 = 1636)
        RegisterObjectSetter3(1632, [](LO* plo, const CRef& r) { TraceSet(1632, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) SetTruckMode(*v, r.RefCoerceS32().m_n); });
        RegisterObjectGetter3(1632, [](LO* plo) { CRef r; VEHICLE* v = Pvehicle(plo); r.SetS32(v ? v->rc.mode : 0); return r; });
        RegisterObjectSetter3(1633, [](LO* plo, const CRef& r) { TraceSet(1633, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) v->rc.path = PloRef(plo, r); });
        RegisterObjectSetter3(1634, [](LO* plo, const CRef& r) { TraceSet(1634, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) v->rc.lookahead = r.GAsFloat(); });
        RegisterObjectSetter3(1635, [](LO* plo, const CRef& r) { TraceSet(1635, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) v->rc.rearSteer = r.GAsFloat(); });
        RegisterObjectSetter3(1636, [](LO* plo, const CRef& r) { TraceSet(1636, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) v->drive = r.GAsFloat(); });
        RegisterObjectGetter3(1636, [](LO* plo) { CRef r; VEHICLE* v = Pvehicle(plo); r.SetF32(v ? v->drive : 0.0f); return r; });
        RegisterObjectSetter3(1625, [](LO* plo, const CRef& r) { TraceSet(1625, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) v->maxSpeed = r.GAsFloat(); });
        RegisterObjectSetter3(1626, [](LO* plo, const CRef& r) { TraceSet(1626, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) v->rc.slipReverse = r.GAsFloat(); });
        RegisterObjectSetter3(1624, [](LO* plo, const CRef& r) { TraceSet(1624, plo, r); if (VEHICLE* v = Pvehicle(plo); v && v->truck.on) v->rc.steerMax = r.GAsFloat(); });
    }
} s_inittruck3;
}  // namespace

bool FIsTruck3(const LO* plo) { return plo && plo->pcls && FIsLoDerivedFrom(plo, 0x51); }

// orig3 0x279DD8: the SO's init, then the tuning (the RC car's numbers) and the four wheels (0x27C830 with
// D_003292D0 = {-40, 20}: radius 50, parts 0x76D..0x770 (D_003292D8))
void InitTruck3(VEHICLE& v)
{
    v.truck.on = true;
    v.rc.on = true;   // (the shared parts: the drawn wheels' rear term, the mode / path fields)
    v.maxSpeed = 1100.0f;                     // +0x32C
    v.rc.slipReverse = 700.0f;                // +0x330
    v.truck.slipScale = 1000.0f;              // +0x414
    v.contactScale = 1.5f;                    // +0x338
    v.rc.steerMax = 0.8f;                     // +0x334
    v.traction = {2500.0f, -1500.0f, 0.0f};   // +0x310
    v.tractionLimits = {0.0f, 2500.0f};       // +0x320
    v.reverseLimit = 3500.0f;                 // +0x328
    v.rc.lookahead = 100.0f;                  // +0x3C0
    v.truck.impactRange = {250.0f, 2000.0f};  // +0x35C
    v.rc.mode = -1;                           // (+0x3B0: set to 0 by the post-load)
    v.wheels.assign(4, VEHICLEWHEEL{});
    for (int i = 0; i < 4; ++i)
    {
        v.wheels[i].id = 0x76D + i;
        v.wheels[i].radius = 50.0f;
        v.wheels[i].bottom = -40.0f;
        v.wheels[i].top = 20.0f;
    }
}

// orig3 0x279F00: the base's post-load, the parts (D_0034A070: entries 0x771 / 0x772 / 0x773 by tag), the wheels'
// places (kind 0xD), off (0x27C860(0)); the loaded options
void PostLoadTruck3(VEHICLE& v)
{
    ALO* p = v.owner;
    VEHICLETRUCK3& t = v.truck;
    for (const OPT& o : p->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& a = o.aval.front();
        switch (o.eopid)
        {
        case 1617:   // the wheel method's loaded calls: wheel i, radius, travel
            if (o.aval.size() >= 3 && a.n >= 0 && a.n < int(v.wheels.size()))
            {
                VEHICLEWHEEL& w = v.wheels[a.n];
                w.radius = o.aval[1].ag[0];
                w.bottom = o.aval[2].ag[0];
                w.top = o.aval[2].ag[1];
            }
            break;
        case 1618: case 1619: case 1620: case 1621: t.dust[o.eopid - 1618] = PloOpt(p, a); break;
        case 1622: t.impactRange = {a.ag[0], a.ag[1]}; break;
        case 1623: t.impactPe = PloOpt(p, a); break;
        case 1624: v.rc.steerMax = a.ag[0]; break;
        case 1625: v.maxSpeed = a.ag[0]; break;
        case 1626: v.rc.slipReverse = a.ag[0]; break;
        case 1627: t.slipScale = a.ag[0]; break;
        case 1628: v.traction = {a.ag[0], a.ag[1], a.ag[2]}; break;
        case 1629: v.tractionLimits = {a.ag[0], a.ag[1]}; break;
        case 1630: v.reverseLimit = a.ag[0]; break;
        case 1631: v.contactScale = a.ag[0]; break;
        case 1633: v.rc.path = PloOpt(p, a); break;
        case 1634: v.rc.lookahead = a.ag[0]; break;
        case 1635: v.rc.rearSteer = a.ag[0]; break;
        case 1636: v.drive = a.ag[0]; break;
        case 1637: case 1638: case 1639: case 1640: case 1641: case 1642: case 1643: t.sounds[o.eopid - 1637] = a.n; break;
        default: break;
        }
    }
    t.bumpers[0] = PsoeByTag(p, 0x771);
    t.bumpers[1] = PsoeByTag(p, 0x772);
    t.body = PsoeByTag(p, 0x773);
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
    bool fAll = true;
    for (const VEHICLEWHEEL& w : v.wheels) fAll &= w.node != nullptr;
    if (!fAll) v.wheels.clear();
    v.width = v.wheels.size() >= 4 ? v.wheels[0].position.x - v.wheels[2].position.x : 0.0f;
    SetTruckMode(v, 0);
    if (FPrint())
    {
        std::printf("truck3: %d post-load wheels %zu width %.1f path %d drive %.0f impact pe %d sounds %d %d %d %d %d %d %d\n", p->oid, v.wheels.size(), v.width,
                    v.rc.path ? v.rc.path->oid : -1, v.drive, t.impactPe ? t.impactPe->oid : -1, t.sounds[0], t.sounds[1], t.sounds[2], t.sounds[3], t.sounds[4],
                    t.sounds[5], t.sounds[6]);
        for (const SOE& e : p->asoe)
            std::printf("truck3:   entry tag %d f48 %d f49 %d f50 %d f51 %d id 0x%X r %.0f\n", e.tag, e.f48, e.f49, e.f50, e.f51, e.id, e.sRadius);
        for (const auto& pxf : p->axf)
            std::printf("truck3:   xf tag %d kind %d world %.0f %.0f %.0f ref %p self %p%s%s%s\n", pxf->tag, pxf->kind, pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z,
                        (void*)pxf->pxfRef, (void*)pxf.get(), pxf.get() == p->apxfRef[0] ? " (+0x48)" : "", pxf.get() == PxfObj(p) ? " (obj)" : "",
                        pxf.get() == PxfPos(p) ? " (pos)" : "");
        for (const VEHICLEWHEEL& w : v.wheels) std::printf("truck3:   wheel %d at %.1f %.1f %.1f r %.0f %.0f..%.0f\n", w.id, w.position.x, w.position.y, w.position.z, w.radius, w.bottom, w.top);
    }
}
