// jumptgt.cpp: the jump's targets (jump.h; module 0x19A400..0x19C870): circle in the air picks the best object of the
// target lists in reach (jt.h) and drives the jump's record to it, then the target's action at the arrival.
// From the original's code (0x19B110 / 0x19C300 / 0x19AE00 / 0x19ABB8 / 0x19ABF0 / 0x19B088 / 0x19C688 / 0x20F948);
// the reference's ports of these are register-level transcriptions (native_old_movement_target.cpp,
// round4_satellite_mid.cpp, round4_paris_low_blocks5.cpp).
//
// Ported: the pipes (list 0x3E13C0, the jump's state 15 -> Sly's state 8), the rails, the climbing surfaces (0x3E142C,
// state 16 -> Sly's state 21, climbsurf.cpp). The other lists (0x3E13D8 spires, 0x3E13CC
// rails, 0x3E13B4 points, 0x3E142C, Bentley's partner, 0x3E13FC walls, 0x3E1408 swings) warn once; the ledges (0x3E13F0,
// the jump's state 11 -> the interaction kind 0) are ported
// when one of their objects is a candidate and are skipped.
#include "jump.h"
#include "sly3/ids3.h"

#include "actor.h"
#include "alo.h"
#include "brx.h"
#include "bsp.h"
#include "clock.h"
#include "crv.h"
#include "jt.h"
#include "lo.h"
#include "sly.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
#include "xfmath.h"
#include "xfphys.h"
#include "interact.h"
#include "po.h"
#include "util.h"
#include "hide.h"
#include "climbsurf.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include "gang.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using cmm::F32;

glm::vec3 PosActorProbe0(ACTOR* pa);  // jump.cpp (+0x218)

namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "jumptgt: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

bool FPrint() { static const bool s_f = std::getenv("SLY2N_JTPRINT") != nullptr; return s_f; }

glm::vec3 PosActorJ(ACTOR* pa) { return PosAloCenter(pa->palo); }   // 0x1A62E8
glm::vec3 VelActorJ(ACTOR* pa) { return VelAloPos(pa->palo); }      // vt slot 33
glm::vec3 GravActor(ACTOR* pa)
{
    const SOST* ps = Psost(pa->palo);
    return ps ? ps->dvGravity : glm::vec3(0.0f, 0.0f, -1750.0f);  // (+0xE8 +0x80)
}
float Len(const glm::vec3& v) { return std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z); }

// The object's world matrix t seconds on (orig: its vt slot 32's record, 0x2309E8 predicted by t, times the object's
// matrix in the record's frame). TODO(actors): a moving record's prediction; the static ones' is their matrix.
glm::mat4 MatObjectAt(LO* plo, float t)
{
    (void)t;
    return MatJtLo(plo);
}

// orig: 0x0019C300 (a jump of t seconds to the object's local point: the actor's ballistic position then; within
// +0x1020 of the point it is reached (*pg = the distance less +0x1020: <= 0); more than +0x1020 below / above (or t 0):
// 1010000 + the height; else the horizontal shortfall's acceleration over t: min(4 shortfall / t, 1e6); *ppos the
// position at t, *pvel the velocity at t)
float GFitJump(ACTOR* pa, LO* plo, const glm::vec3& posLocal, float t, glm::vec3* ppos, glm::vec3* pvel)
{
    const glm::vec3 pos = PosActorJ(pa), v = VelActorJ(pa), g = GravActor(pa);
    const float h = t * 0.5f;
    glm::vec3 vT = v + g * t;                  // sp+0xE0
    const glm::vec3 posT = (pos + v * h) + vT * h;  // sp+0x120
    const glm::mat4 m = MatObjectAt(plo, t);
    const glm::vec3 posPt = glm::vec3(m * glm::vec4(posLocal, 1.0f));
    glm::vec3 d = posPt - posT;
    const float d2 = (d.x * d.x + d.y * d.y) + d.z * d.z;
    const float r = pa->g1020;
    float score;
    glm::vec3 posOut;
    if (d2 < r * r)
    {
        score = std::sqrt(d2) - r;
        posOut = posT;
    }
    else
    {
        const float dz = std::fabs(d.z);
        if (r < dz || t == 0.0f)
        {
            score = dz + 1010000.0f;
            posOut = pos;
        }
        else
        {
            d.z = 0.0f;
            const float l = Len(d);
            const glm::vec3 dir = l < F32(0x38D1B717) ? glm::vec3(0.0f) : d * (1.0f / l);
            const float sShort = l - std::sqrt(r * r - dz * dz);
            const float a = (sShort + sShort) / (t * t);
            vT = (v + g * t) + (dir * a) * t;
            score = std::min(((2.0f + 2.0f) * sShort) / t, 1000000.0f);  // (0x2BF910: 2)
            posOut = (pos + v * h) + vT * h;
        }
    }
    if (ppos) *ppos = posOut;
    if (pvel) *pvel = vT;
    return score;
}

// orig: 0x0020F948 (the trajectory test's entries: no bit-48 sensors, not the actor's own or an attachment marked 4,
// and a pair kind (vt slot 61 both ways, the larger) neither 1 nor 3)
int NOverlapKind(ALO* p, ALO* pOther)
{
    using PFN = int (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 61))) return pfn(p, pOther);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8))) return pfn(p, pOther);
    return 0;
}
bool FTrajectoryEntry(OXA* poxaOwn, SOE* psoe)
{
    if (psoe->f48) return false;  // 0x20F970 -> 0x20F990
    if (!psoe->palo) return false;
    OXA* poxa = Poxa(psoe->palo);
    if (!poxa) return false;
    if (poxa->grf & 4) return false;
    if (poxa == poxaOwn) return false;
    if (!poxaOwn || !poxaOwn->pso || !poxa->pso) return true;
    const int ownKind = NOverlapKind(poxaOwn->pso, poxa->pso);
    const int otherKind = NOverlapKind(poxa->pso, poxaOwn->pso);
    const int k = std::max(ownKind, otherKind);  // 0x134310
    return k != 1 && k != 3;
}

}  // namespace
bool FJumpPathEntry(OXA* poxaOwn, SOE* psoe) { return FTrajectoryEntry(poxaOwn, psoe); }  // (0x20F948, for interact.cpp)
namespace
{
// orig: 0x0019AE00 (the Hermite path pos0 (vel0) -> pos1 (vel1) over t, in steps of 0.1 s, through the solid entries
// of its box (0x20FAA0 with 0x20F948): a hit farther than 100 from pos1 blocks it; then pos1 -> *pposEnd: a hit
// before its end farther than 50 from it)
bool FJumpPathBlocked(ACTOR* pa, float t, const glm::vec3& pos0, const glm::vec3& vel0, const glm::vec3& pos1,
                      const glm::vec3& vel1, const glm::vec3* pposEnd, const std::vector<OXA*>* papoxaSkip = nullptr)
{
    glm::vec3 mn = glm::min(pos0, pos1), mx = glm::max(pos0, pos1);
    if (pposEnd)
    {
        mn = glm::min(mn, *pposEnd);
        mx = glm::max(mx, *pposEnd);
    }
    OXA* poxaOwn = Poxa(pa->palo);
    std::vector<SOE*> apsoe;
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
        {
            // (0x19B0A8 with a list: no bit-48 sensors, no attachment marked 4, none of the list's)
            const bool f = papoxaSkip ? !psoe->f48 && psoe->palo && Poxa(psoe->palo) && !(Poxa(psoe->palo)->grf & 4) &&
                                            std::find(papoxaSkip->begin(), papoxaSkip->end(), Poxa(psoe->palo)) == papoxaSkip->end()
                                      : FTrajectoryEntry(poxaOwn, psoe);
            if (apsoe.size() < 0x100 && f) apsoe.push_back(psoe);
        }
    }
    LSG lsg;
    auto hit = [&](const glm::vec3& a, const glm::vec3& b) { return !apsoe.empty() && PsoeHitTestLineObjects(0, a, b, apsoe, &lsg); };
    glm::vec3 prev(0.0f);
    for (float u = 0.0f;;)
    {
        glm::vec3 p(0.0f);
        EvalHermiteVector(pos0, vel0, pos1, vel1, &p, nullptr, nullptr, t, u, 1.0f);  // 0x117120
        if (u != 0.0f && hit(prev, p) && 100.0f < Len(lsg.pos0 - pos1)) return true;
        if (u == t) break;
        prev = p;
        u = std::min(t, u + F32(0x3DCCCCCD));
    }
    if (pposEnd && hit(pos1, *pposEnd) && lsg.t1 != 1.0f && 50.0f < Len(lsg.pos0 - *pposEnd)) return true;
    return false;
}

// The object's vt slot 32 record (ALO 0x1F3FB0: the object record; LOX 0x231550: +0x50)
XF* PxfSlot32(LO* plo)
{
    if (!plo) return nullptr;
    if (FIsLoDerivedFrom(plo, Icid(CID3::ALO))) return PxfObj(static_cast<ALO*>(plo));
    if (FIsLoDerivedFrom(plo, 3)) return static_cast<LOX*>(plo)->pxf;
    return nullptr;
}

// orig: 0x0019A400 (the time the launch velocity's rise takes to bring the actor to the target's height: the later
// root of g/2 t^2 + (vz - vTz) t + (z - zT); false when none is positive)
bool FJumpTimeToHeight(ACTOR* pa, const glm::vec3& pos, const glm::vec3& vLaunch, const glm::vec3& posT, const glm::vec3& vT, float* pt)
{
    float ax[2] = {0.0f, 0.0f};
    const int c = CSolveQuadratic(GravActor(pa).z * 0.5f, vLaunch.z - vT.z, pos.z - posT.z, ax);  // 0x11D508
    if (c <= 0) return false;
    const float t = ax[c - 1];
    if (t <= 0.0f) return false;
    *pt = t;
    return true;
}

// orig: 0x0019A4A0 (a jump with the launch velocity to the object's point posT (moving with the object's record,
// plus vOff on arrival): the time; the score (the Hermite's horizontal accelerations at both ends (0x117120) times
// half the time, at most 1e6; out of reach: 1010000 less the apex height above it); the end position / velocity)
void FitJumpToPoint(JUMP* pj, const glm::vec3& vLaunch, LO* plo, const glm::vec3& posT, const glm::vec3& vOff, float* pt,
                    float* pscore, glm::vec3* ppos, glm::vec3* pvel)
{
    ACTOR* pa = pj->pactor;
    const glm::vec3 pos = PosActorJ(pa);
    XF* pxf = PxfSlot32(plo);
    const glm::vec3 vT = pxf ? VelXfPoint(pxf, nullptr, posT) : glm::vec3(0.0f);  // 0x22D348
    float t = 0.0f;
    if (!FJumpTimeToHeight(pa, pos, vLaunch, posT, vT, &t))
    {
        const float g = GravActor(pa).z;
        const float dv = vLaunch.z - vT.z;
        float ta = -dv / g;
        ta = std::max(ta, 0.0f);
        const float h = (pos.z - posT.z) + ((g * (ta * 0.5f)) * ta + dv * ta);
        if (pt) *pt = ta;
        if (pscore) *pscore = 1010000.0f - h;
        return;
    }
    const float dtLead = std::min(F32(0x3E19999A), t);  // 0.15
    const glm::vec3 vEnd = vT + vOff;
    const glm::vec3 posEnd = posT + vT * dtLead;
    if (pt) *pt = t;
    if (pscore)
    {
        glm::vec3 a0(0.0f), a1(0.0f);
        EvalHermiteVector(pos, vLaunch, posEnd, vEnd, nullptr, nullptr, &a0, t, 0.0f, 1.0f);  // 0x117120
        EvalHermiteVector(pos, vLaunch, posEnd, vEnd, nullptr, nullptr, &a1, t, t, 1.0f);
        float g = std::sqrt(a0.x * a0.x + a0.y * a0.y);
        g = g + std::sqrt(a1.x * a1.x + a1.y * a1.y);
        g = g * (t * 0.5f);
        *pscore = std::min(g, 1000000.0f);
    }
    if (ppos) *ppos = posEnd;
    if (pvel) *pvel = vEnd;
}

PADCTL* PpadctlJ(ACTOR* pa)
{
    PODATA* ppd = PpodataFromLo(pa->palo);
    return ppd ? ppd->ppadctl : nullptr;
}

// orig: 0x00193A68 (the +0x4C0's rail point at parameter g for the actor's root (less +0x240) and its expected
// velocity along the rail: the stick (+0x150's direction and size) at the rail gait's speed (+0x2A0), flattened onto
// the rail's horizontal direction, limited by the smoothed speeds toward 50 from either end (SMP {+0x2A0, +0x260, 0.2}))
void RailPointVelocity(ACTOR* pa, LO* prail, float g, glm::vec3* ppos, glm::vec3* pvel)
{
    const CRV* pcrv = PcrvJtPipe(prail);
    glm::vec3 pos(0.0f), dir(0.0f);
    if (pcrv) EvalCrv(pcrv, g, &pos, &dir);  // 0x1E67B8
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    glm::vec3 flat = dir - up * ((dir.x * up.x + dir.y * up.y) + dir.z * up.z);
    const float len = std::sqrt((flat.x * flat.x + flat.y * flat.y) + flat.z * flat.z);
    flat = len < F32(0x38D1B717) ? glm::vec3(0.0f) : flat * (1.0f / len);
    const GAIT& gait = pa->inter.gait240;
    PADCTL* pc = PpadctlJ(pa);
    const glm::vec3 stick = cmm::SetVectorCylind(pc ? pc->radJoy : 0.0f, gait.avel[1].x * (pc ? pc->uJoy : 0.0f), 0.0f);  // 0x11E1A0
    const float v = (stick.x * flat.x + stick.y * flat.y) + stick.z * flat.z;
    const float s = pcrv ? SFromCrvG(pcrv, g) : 0.0f;  // 0x1E63C0
    const SMP smp{gait.avel[1].x, gait.avel[0].x, F32(0x3E4CCCCD)};  // (0x2BF668: 0.2)
    float vLo = 0.0f, vHi = 0.0f;
    GSmooth(s, 50.0f, g_clock.dt, &smp, &vLo);  // 0x11D018
    GSmooth(s, (pcrv ? SLengthCrv(pcrv) : 0.0f) - 50.0f, g_clock.dt, &smp, &vHi);
    *ppos = pos - glm::vec3(pa->vecBase);
    float vClamped;
    if (v < vLo) vClamped = vLo;
    else vClamped = vHi < v ? vHi : v;
    *pvel = flat * vClamped;
}

// orig: 0x0019B088 (a target's score: plus its list's bias, plus 1000 for the object just left)
float GTargetScore(JUMP* pj, LO* plo, float g, float gBias)
{
    g = g + gBias;
    if (plo == pj->ploLeft) g = g + 1000.0f;  // (0x2BF900)
    return g;
}

// orig: 0x0019ABF0 (the best jump's score (0x19ABB8 over t in [0.25, 3] from 0.5) to the curve's point at s)
float GScorePipeS(ACTOR* pa, LO* ppipe, float s)
{
    glm::vec3 pos(0.0f);
    EvalCrvS(PcrvJtPipe(ppipe), s, &pos, nullptr);
    const glm::vec3 local = PosJtConvert(nullptr, ppipe, pos);
    float gResult = 0.0f;
    MinimizeRange([&](float t) { return GFitJump(pa, ppipe, local, t, nullptr, nullptr); }, 0.5f, 0.25f, 0.25f, 3.0f, nullptr,
                  &gResult);
    return gResult;
}
}  // namespace

// orig: 0x0019B110
void ChooseJumpTarget(JUMP* pj, LO* ploPrev)
{
    ACTOR* pa = pj->pactor;
    SW* psw = g_psw;
    float g27 = 0.0f, g26 = 0.0f;  // (+0x10 / +0x14 at the end)
    const glm::vec3 pos = PosActorJ(pa);   // sp+0x10
    const glm::vec3 vel = VelActorJ(pa);   // sp+0x20 (vt slot 33)
    (void)vel;  // (the ledges use it)
    const uint32_t grfMask = static_cast<uint32_t>(pa->n1000);  // s6 (+0x1000)
    float gBest = ploPrev ? -FLT_MAX : FLT_MAX;  // f23
    const glm::vec3 vLaunch = VelJumpLaunch(pj, 4);  // sp+0x80
    (void)vLaunch;
    float tBest = 0.0f;   // f24
    int stBest = -1;      // s5
    LO* ploBest = nullptr;  // s4
    glm::vec3 posEnd(0.0f), velEnd(0.0f);  // sp+0x40 (with +0x240) / sp+0x60
    auto warnList = [&](JTL jtl, const char* what) {
        for (LO* plo : AploJtList(psw, jtl))
            if (JtDescOf(plo, jtl).mask & grfMask)
                if (Len(PosJtLo(plo) - pos) < 2500.0f)
                {
                    WarnOnce(std::string("0x19B110's ") + what);
                    return;
                }
    };
    if (FPrint())
        for (CID cid = 0; cid < CID_Max; ++cid)
            for (LO* plo : psw->mpcidaplo[cid])
                if (cid == 0x0B && plo->oid == 123964)
                    std::printf("jumptgt: pipe %d in world %d removed %d iupd %d st %d\n", plo->oid, plo->fInWorld, plo->fRemoved, plo->iupd, plo->st);
    if (FIsLoDerivedFrom(pa->palo, Icid(CID3::JT)))
    {
        // the spires (0x3E13D8): the description's vt slot 3 (0x1FF4F0: the spire's position less +0x240); not more
        // than +0xE8 below the actor; the jump's time / score / end (0x19A4A0, the end at rest), the end's z velocity
        // the launch's plus g t; under 2500, with the bias -250 (0x2BF8E4); its path clear (0x19AE00)
        for (LO* pspire : AploJtList(psw, JTL_Spire))
        {
            const JTDESC d = JtDescOf(pspire, JTL_Spire);
            if (!(d.mask & grfMask)) continue;
            const glm::vec3 posS = PosJtLo(pspire) - glm::vec3(pa->vecBase);  // 0x1FF4F0 (0x1A62E8)
            if (pspire != ploPrev && d.height < pos.z - posS.z) continue;
            float t = 0.0f, sc = 0.0f;
            glm::vec3 posT(0.0f), velT(0.0f);
            FitJumpToPoint(pj, vLaunch, pspire, posS, glm::vec3(0.0f), &t, &sc, &posT, &velT);  // 0x19A4A0
            velT.z = vLaunch.z + t * GravActor(pa).z;
            if (FPrint())
                std::printf("jumptgt: spire %d candidate score %.2f t %.3f pos z %.1f point %.1f %.1f %.1f blocked %d mask %x/%x\n", pspire->oid, sc, t, pos.z, posS.x, posS.y,
                            posS.z, FJumpPathBlocked(pa, t, pos, vLaunch, posT, velT, nullptr) ? 1 : 0, d.mask, grfMask);
            if (pspire != ploPrev)
            {
                if (!(sc < 2500.0f)) continue;  // (0x2BF8E0)
                sc = GTargetScore(pj, pspire, sc, -250.0f);  // (0x2BF8E4)
                if (gBest <= sc) continue;
                if (FJumpPathBlocked(pa, t, pos, vLaunch, posT, velT, nullptr)) continue;
            }
            stBest = 9;
            ploBest = pspire;
            posEnd = posT + glm::vec3(pa->vecBase);  // (as the rails')
            velEnd = velT;
            gBest = std::min(gBest, sc);
            tBest = t;
            if (FPrint())
                std::printf("jumptgt: spire %d score %.2f t %.3f end %.1f %.1f %.1f vel %.1f %.1f %.1f\n", pspire->oid, sc, t, posT.x, posT.y,
                            posT.z, velT.x, velT.y, velT.z);
        }
        // the rails (0x3E13CC): not more than +0x198 above the nearest point (of the probe point +0x218); the best
        // parameter (0x19AA58: the jump's score to the rail point 0x193A68) over [g(50), g(length - 50)] from the knot
        // of the best score (0x1E7410), in steps of the knots' span / (10 c) (0x1E77E8); under 2500 (+0x2BF8E8 bias);
        // the jump's time and end (0x19A4A0) with the rail's velocity, the end's z velocity the launch's plus g t; its
        // path clear (0x19AE00 with the launch velocity)
        for (LO* prail : AploJtList(psw, JTL_Rail))
        {
            const JTDESC d = JtDescOf(prail, JTL_Rail);
            if (!(d.mask & grfMask)) continue;
            const CRV* pcrv = PcrvJtPipe(prail);
            if (!pcrv) continue;
            if (prail != ploPrev)
            {
                glm::vec3 posNear(0.0f);
                NearestCrvAll(pcrv, PosActorProbe0(pa), &posNear, nullptr, nullptr, nullptr);  // 0x1E6F60
                if (d.height < pos.z - posNear.z) continue;
            }
            auto score = [&](float g) {  // 0x19AA58
                glm::vec3 p(0.0f), v(0.0f);
                RailPointVelocity(pa, prail, g, &p, &v);
                float sc = 0.0f;
                FitJumpToPoint(pj, vLaunch, prail, p, v, nullptr, &sc, nullptr, nullptr);
                return sc;
            };
            float gStart = 0.0f, scMin = F32(0x7F7FFFFF);  // 0x1E7410 over the knots (D_002A813C)
            for (float gk : pcrv->ag)
            {
                const float sc = score(gk);
                if (sc < scMin) { scMin = sc; gStart = gk; }
            }
            const float dg = (pcrv->ag.empty() ? 0.0f : (pcrv->ag.back() - pcrv->ag.front()) / (static_cast<float>(pcrv->ag.size()) * 10.0f));  // 0x1E77E8
            float gBestRail = 0.0f, gScore = 0.0f;
            MinimizeRange(score, gStart, dg, GFromCrvS(pcrv, 50.0f), GFromCrvS(pcrv, SLengthCrv(pcrv) - 50.0f), &gBestRail, &gScore);
            if (prail != ploPrev)
            {
                if (!(gScore < 2500.0f)) continue;
                gScore = GTargetScore(pj, prail, gScore, 0.0f);  // (0x2BF8E8)
                if (gBest <= gScore) continue;
            }
            glm::vec3 posR(0.0f), velR(0.0f);
            RailPointVelocity(pa, prail, gBestRail, &posR, &velR);  // 0x193A68
            float t = 0.0f;
            glm::vec3 posT(0.0f), velT(0.0f);
            FitJumpToPoint(pj, vLaunch, prail, posR, velR, &t, nullptr, &posT, &velT);  // 0x19A4A0
            velT.z = vLaunch.z + t * GravActor(pa).z;
            if (prail != ploPrev && FJumpPathBlocked(pa, t, pos, vLaunch, posT, velT, nullptr)) continue;
            stBest = 10;
            ploBest = prail;
            posEnd = posT + glm::vec3(pa->vecBase);  // (as the pipes': the record takes the end less +0x240)
            velEnd = velT;
            gBest = std::min(gBest, gScore);
            g27 = gBestRail;
            g26 = SFromCrvG(pcrv, gBestRail);  // 0x1E63C0
            tBest = t;
            if (FPrint())
                std::printf("jumptgt: rail %d g %.3f (s %.1f) score %.2f t %.3f end %.1f %.1f %.1f vel %.1f %.1f %.1f\n", prail->oid, gBestRail, g26,
                            gScore, t, posT.x, posT.y, posT.z, velT.x, velT.y, velT.z);
        }
        // the hooks (0x3E13B4: cid 0x13 with +0xE8 bit 1; the forced one only, when forced): not more than +0x108 below,
        // within 1000; the best jump time (0x19ABB8 = 0x19C300 to the hook's own point over [0.25, 3] from 0.5), its end
        // and velocity; under 2500 (+0x2BF8EC: 0); its path clear (0x19AE00 with the actor's velocity, to the hook)
        for (LO* phook : AploJtList(psw, JTL_Point))
        {
            if (FPrint() && (phook->oid == 42970 || Len(glm::vec3(MatJtLo(phook)[3]) - pos) < 1500.0f))
                std::printf("jumptgt: hook %d cid 0x%X e8 %X mask %X/%X height %.1f dz %.1f\n", phook->oid, phook->Cid(), phook->U32Field(0, 0xE8, 0),
                            JtDescOf(phook, JTL_Point).mask, grfMask, JtDescOf(phook, JTL_Point).height, pos.z - MatJtLo(phook)[3].z);
            if (phook != ploPrev)
            {
                if (ploPrev) continue;
                if (!(phook->U32Field(0, 0xE8, 0) & 1)) continue;
                if (!FIsLoDerivedFrom(phook, 0x13)) continue;
            }
            const JTDESC d = JtDescOf(phook, JTL_Point);
            if (!(d.mask & grfMask)) continue;
            const glm::vec3 posHook = glm::vec3(MatJtLo(phook)[3]);  // 0x1A62E8
            if (phook != ploPrev)
            {
                if (d.height < pos.z - posHook.z) continue;
                const glm::vec3 dh = posHook - pos;
                if (1000000.0f < (dh.x * dh.x + dh.y * dh.y) + dh.z * dh.z) continue;
            }
            float t = 0.0f, g = 0.0f;
            MinimizeRange([&](float tt) { return GFitJump(pa, phook, glm::vec3(0.0f), tt, nullptr, nullptr); }, 0.5f, 0.25f, 0.25f, 3.0f, &t, &g);
            glm::vec3 posHit(0.0f), vHit(0.0f);
            GFitJump(pa, phook, glm::vec3(0.0f), t, &posHit, &vHit);  // 0x19C300
            if (FPrint())
                std::printf("jumptgt: hook %d candidate score %.2f t %.3f end %.1f %.1f %.1f\n", phook->oid, g, t, posHit.x, posHit.y, posHit.z);
            if (phook != ploPrev)
            {
                if (!(g < 2500.0f)) continue;
                g = GTargetScore(pj, phook, g, 0.0f);  // (D_002BF8EC)
                if (gBest <= g) continue;
                if (FJumpPathBlocked(pa, t, pos, VelActorJ(pa), posHit, vHit, &posHook)) continue;
            }
            stBest = 14;
            ploBest = phook;
            posEnd = posHit + glm::vec3(pa->vecBase);
            velEnd = vHit;
            gBest = std::min(gBest, g);
            tBest = t;
        }
        // the climbing surfaces (0x3E142C, cid 0x09; no players' mask): the nearest point not more than +0x108 below
        // and within 1000; the jump's time to its landing (0x19AC88: the Hermite's accelerations at both ends times
        // half the time, over t in [0.25, 2] from 1 in steps of 0.25) under 2500; its path clear
        if (SLY* psly = dynamic_cast<SLY*>(pa))
            for (LO* pmesh : AploJtList(psw, JTL_Point2))
            {
                glm::vec3 posTarget(0.0f);
                FindClimbSurfClosestPoint(pmesh, pos, nullptr, &posTarget, nullptr);  // 0x185310
                if (pmesh != ploPrev)
                {
                    if (SHeightClimbSurf(pmesh) < pos.z - posTarget.z) continue;
                    if (1000.0f < Len(posTarget - pos)) continue;
                }
                const float sOffset = psly->aposSurface[0].x;  // (+0x1F30)
                const glm::vec3 grav = GravActor(pa);
                auto score = [&](float t) {  // 0x19AC88
                    const glm::vec3 p0 = PosActorJ(pa);
                    glm::vec3 p1(0.0f), v1(0.0f), a0(0.0f), a1(0.0f);
                    ClimbSurfLanding(pmesh, p0, vLaunch, grav, sOffset, t, nullptr, &p1, &v1);  // 0x185D18
                    EvalHermiteVector(p0, vLaunch, p1, v1, nullptr, nullptr, &a0, t, 0.0f, 1.0f);  // 0x117120
                    EvalHermiteVector(p0, vLaunch, p1, v1, nullptr, nullptr, &a1, t, t, 1.0f);
                    const float g = (Len(a0) + Len(a1)) * (t * 0.5f);
                    return g < 1000000.0f ? g : 1000000.0f;
                };
                float t = 0.0f, gMesh = 0.0f;
                MinimizeRange(score, 1.0f, 0.25f, 0.25f, 2.0f, &t, &gMesh);  // 0x11D9E0
                if (FPrint())
                    std::printf("jumptgt: surface %d candidate near %.1f %.1f %.1f score %.2f t %.3f\n", pmesh->oid, posTarget.x,
                                posTarget.y, posTarget.z, gMesh, t);
                if (pmesh != ploPrev)
                {
                    if (!(gMesh < 2500.0f)) continue;
                    gMesh = GTargetScore(pj, pmesh, gMesh, 0.0f);  // (D_002BF908)
                    if (gBest <= gMesh) continue;
                }
                glm::vec3 posHit(0.0f), vHit(0.0f);
                ClimbSurfLanding(pmesh, pos, vLaunch, grav, sOffset, t, nullptr, &posHit, &vHit);
                if (pmesh != ploPrev && FJumpPathBlocked(pa, t, pos, VelActorJ(pa), posHit, vHit, nullptr)) continue;
                stBest = 16;
                ploBest = pmesh;
                posEnd = posHit + glm::vec3(pa->vecBase);
                velEnd = vHit;
                gBest = std::min(gBest, gMesh);
                tBest = t;
                if (FPrint())
                    std::printf("jumptgt: surface %d score %.2f t %.3f end %.1f %.1f %.1f vel %.1f %.1f %.1f\n", pmesh->oid, gMesh, t,
                                posHit.x, posHit.y, posHit.z, vHit.x, vHit.y, vHit.z);
            }
    }
    // the partner (Bentley only): Murray in his state 15 with his +0x1910 controller holding (state 0): its holding
    // frame +0x191C's point (0x19A4A0, at rest), the end's z velocity the launch's plus g t; under 2500, Murray's score
    // with the bias -500 (0x2BF904), its path clear of all but the two of them (0x19AE00 with 0x19B0A8): the state 18
    if (FIsLoDerivedFrom(pa->palo, Icid(CID3::BENTLEY)))
        if (MURRAY* pm = PmurrayGlobal(); pm && pm->state == 15 && pm->jtthrow1910.state == 0 && pm->jtthrow1910.peer)
        {
            LO* peer = pm->jtthrow1910.peer;
            const glm::vec3 posP = PosJtLo(peer);  // 0x1A62E8
            float t = 0.0f, sc = 0.0f;
            glm::vec3 posT(0.0f), velT(0.0f);
            FitJumpToPoint(pj, vLaunch, peer, posP, glm::vec3(0.0f), &t, &sc, &posT, &velT);
            velT.z = vLaunch.z + t * GravActor(pa).z;
            bool f = true;
            if (peer != ploPrev)
            {
                if (!(sc < 2500.0f)) f = false;
                else
                {
                    sc = GTargetScore(pj, pm->palo, sc, -500.0f);  // (0x2BF904)
                    const std::vector<OXA*> apoxa{Poxa(pa->palo), Poxa(pm->palo)};
                    if (gBest <= sc || FJumpPathBlocked(pa, t, pos, vLaunch, posT, velT, nullptr, &apoxa)) f = false;
                }
            }
            if (FPrint())
                std::printf("jumptgt: partner %d score %.2f t %.3f end %.1f %.1f %.1f taken %d\n", peer->oid, sc, t, posT.x, posT.y, posT.z, f);
            if (f)
            {
                stBest = 18;
                ploBest = peer;
                posEnd = posT + glm::vec3(pa->vecBase);
                velEnd = velT;
                gBest = std::min(sc, gBest);
                tBest = t;
            }
        }
    // the pipes (0x3E13C0): the nearest point; not more than +0x198 below the actor, within 1000; the best distance
    // on the pipe (0x19ABF0 over [50, length - 50] from it in steps of 100); the score under 2500 (+1250); the jump's
    // time to that point (0x19ABB8: [0.25, 3] from 0.5 in steps of 0.25), its path clear (0x19AE00)
    for (LO* ppipe : AploJtList(psw, JTL_Pipe))
    {
        const JTDESC d = JtDescOf(ppipe, JTL_Pipe);
        if (!(d.mask & grfMask)) continue;
        const CRV* pcrv = PcrvJtPipe(ppipe);
        if (!pcrv) continue;
        glm::vec3 posNear(0.0f);
        float sNear = 0.0f;
        NearestCrvAll(pcrv, pos, &posNear, nullptr, nullptr, &sNear);  // 0x1E6F60
        if (ppipe != ploPrev)
        {
            if (d.height < pos.z - posNear.z) continue;
            if (1000.0f < Len(posNear - pos)) continue;
        }
        const float len = SLengthCrv(pcrv);
        float sBest = 0.0f, gScore = 0.0f;
        MinimizeRange([&](float s) { return GScorePipeS(pa, ppipe, s); }, sNear, 100.0f, 50.0f, len - 50.0f, &sBest, &gScore);
        if (ppipe != ploPrev)
        {
            if (!(gScore < 2500.0f)) continue;
            gScore = GTargetScore(pj, ppipe, gScore, 1250.0f);
            if (gBest <= gScore) continue;
        }
        glm::vec3 posPipe(0.0f);
        EvalCrvS(pcrv, sBest, &posPipe, nullptr);
        const glm::vec3 local = PosJtConvert(nullptr, ppipe, posPipe);
        float t = 0.0f;
        MinimizeRange([&](float tt) { return GFitJump(pa, ppipe, local, tt, nullptr, nullptr); }, 0.5f, 0.25f, 0.25f, 3.0f, &t, nullptr);
        glm::vec3 posT(0.0f), velT(0.0f);
        GFitJump(pa, ppipe, local, t, &posT, &velT);
        if (ppipe != ploPrev && FJumpPathBlocked(pa, t, pos, VelActorJ(pa), posT, velT, &posPipe)) continue;
        stBest = 15;
        ploBest = ppipe;
        posEnd = posT + glm::vec3(pa->vecBase);
        velEnd = velT;
        gBest = std::min(gScore, gBest);
        tBest = t;
        g26 = sBest;
        if (FPrint())
            std::printf("jumptgt: pipe %d s %.1f (near %.1f) score %.2f t %.3f end %.1f %.1f %.1f\n", ppipe->oid, sBest, sNear, gScore, t,
                        posT.x, posT.y, posT.z);
    }
    warnList(JTL_Wall, "walls (0x3E13FC)");
    // the ledges (0x3E13F0, cid 0x0C): the nearest point (0x1E6F60 from the actor's position), not more than the
    // description's height below; the best place on the edge (0x19AB38: the jump's score to the description's point,
    // method 3 0x1830D8) over [30, length - 30] from it in steps of 100 (D_002BF918), the score times 1.25 (D_002BF90C)
    // under 2500 (+500, D_002BF8F4); the jump's time and end (0x19A4A0 to the point at rest), the end's z velocity the
    // velocity's plus g t; its path clear (0x19AE00) -> the jump's state 11. (Unlike the spires / rails: from the actor's
    // velocity now (vt slot 33), not the launch velocity 0x19A750)
    for (LO* pledge : AploJtList(psw, JTL_Ledge))
    {
        const JTDESC d = JtDescOf(pledge, JTL_Ledge);
        if (!(d.mask & grfMask)) continue;
        const CRV* pcrv = PcrvJtPipe(pledge);
        if (!pcrv) continue;
        glm::vec3 posNear(0.0f);
        float sNear = 0.0f;
        NearestCrvAll(pcrv, pos, &posNear, nullptr, nullptr, &sNear);  // 0x1E6F60
        if (d.height < pos.z - posNear.z) continue;
        auto score = [&](float s) {  // 0x19AB38
            float sc = 0.0f;
            FitJumpToPoint(pj, vel, pledge, PosJtDesc(pledge, JTL_Ledge, pa, s, nullptr), glm::vec3(0.0f), nullptr, &sc, nullptr, nullptr);
            return sc;
        };
        float u = 0.0f, gScore = 0.0f;
        MinimizeRange(score, sNear, 100.0f, 30.0f, SLengthCrv(pcrv) - 30.0f, &u, &gScore);  // 0x11D9E0
        gScore *= 1.25f;
        if (pledge != ploPrev)
        {
            if (!(gScore < 2500.0f)) continue;
            gScore = GTargetScore(pj, pledge, gScore, 500.0f);
            if (gBest <= gScore) continue;
        }
        float t = 0.0f;
        glm::vec3 posT(0.0f), velT(0.0f);
        FitJumpToPoint(pj, vel, pledge, PosJtDesc(pledge, JTL_Ledge, pa, u, nullptr), glm::vec3(0.0f), &t, nullptr, &posT, &velT);  // 0x19A4A0
        velT.z = vel.z + t * GravActor(pa).z;
        if (pledge != ploPrev && FJumpPathBlocked(pa, t, pos, vel, posT, velT, nullptr)) continue;
        stBest = 11;
        ploBest = pledge;
        posEnd = posT + glm::vec3(pa->vecBase);  // (as the spires': 0x19B110 keeps posHit, the native fit's end is less +0x240)
        velEnd = velT;
        gBest = std::min(gBest, gScore);
        tBest = t;
        g26 = u;
        if (FPrint())
            std::printf("jumptgt: ledge %d s %.1f (near %.1f) score %.2f t %.3f end %.1f %.1f %.1f\n", pledge->oid, u, sNear, gScore, t, posT.x,
                        posT.y, posT.z);
    }
    warnList(JTL_Swing, "swings (0x3E1408, the jump's state 13)");
    if (stBest == -1) return;
    // the record (+0x58) driven to the end by the time: +0xA8, +0xC0 (less +0x240), +0xD0; then the jump's state
    if (XF* pxf = PxfPos(pa->palo))
    {
        pxf->tTarget = g_clock.t + tBest;
        pxf->aseg[0] = glm::vec4(posEnd - glm::vec3(pa->vecBase), 0.0f);
        pxf->aseg[1] = glm::vec4(velEnd, 0.0f);
    }
    pj->g10 = g27;
    pj->s14 = g26;
    if (FPrint()) std::printf("jumptgt: chose %d (state %d) t %.3f score %.2f\n", ploBest ? ploBest->oid : -1, stBest, tBest, gBest);
    SetJumpAnimState(pj, stBest, ploBest);  // 0x195FB8
}

// orig: 0x0019C688
void ArriveJumpTarget(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    switch (pj->state)
    {
    case 15:
        // the pipe: the owner's state 8 on it (+0xF5C / +0xF60, vt slot 88 (8))
        pa->ploF5C = pj->ploC;
        pa->sCrvF50 = pj->s14;
        NotifyActorJumpKind(pa, 8);
        break;
    case 6: StartInteract(pa, 1, pj->ploC, JTL_Jump); break;  // the hiding spot: +0x4C0's kind 1 (its description +0x150)
    case 9: StartInteract(pa, 4, pj->ploC, JTL_Spire); break;  // the spire: +0x4C0's kind 4 (its description +0xE0)
    case 10: StartInteract(pa, 3, pj->ploC, JTL_Rail); break;  // the rail: kind 3 (+0x190)
    case 16:
        // the climbing surface: Sly's state 21 on it (+0x1F18), held (substate 10)
        if (SLY* p = dynamic_cast<SLY*>(pa))
        {
            p->ploSurface = pj->ploC;
            SetSlyState(p, 0x15, 0xA);
        }
        break;
    case 14:
        // the hook: the owner's +0x18C4; its kind +0x124 3: the hang-follow (state 0x16), else the swing (3, 1)
        if (FPrint()) std::printf("jumptgt: arrived at hook %d t %.3f\n", pj->ploC ? pj->ploC->oid : -1, g_clock.t);
        if (SLY* p = dynamic_cast<SLY*>(pa))
        {
            p->ploOther = pj->ploC;
            if (pj->ploC && pj->ploC->U32Field(0, 0x124, 0) == 3) SetSlyState(p, 0x16, -1);
            else SetSlyState(p, 3, 1);
        }
        break;
    case 18:
        SetBentleyState(PbentleyGlobal(), 8);  // 0x135248(D_002DD5BC, 8): held by Murray (0x136530)
        break;
    case 11: StartInteract(pa, 0, pj->ploC, JTL_Ledge); break;  // the ledge: kind 0 (its description +0x190)
    case 12: case 13:
        WarnOnce("0x19C688's arrival in the jump's state " + std::to_string(pj->state));
        SetJumpAnimState(pj, 4, nullptr);
        break;
    default:
        SetJumpAnimState(pj, 4, nullptr);  // 0x195FB8(4, 0)
        break;
    }
}

namespace
{
// orig: 0x0011EE08 (the point of the segment p0 p1 nearest pos: its parameter, clamped to [0, 1], and distance; a
// zero segment divides 0 by 0, which the PS2 gives as +-max (the sign of the numerator, the denominator being +0))
void NearestOnSegment(const glm::vec3& pos, const glm::vec3& p0, const glm::vec3& p1, float* pu, float* pdist)
{
    const glm::vec3 d = p1 - p0, e0 = pos - p0;
    const float vd = (d.x * d.x + d.y * d.y) + d.z * d.z;
    const float vn = (e0.x * d.x + e0.y * d.y) + e0.z * d.z;
    float u = vd != 0.0f ? vn / vd : (std::signbit(vn) ? -FLT_MAX : FLT_MAX);
    if (u < 0.0f) u = 0.0f;
    else if (u > 1.0f) u = 1.0f;
    *pu = u;
    const glm::vec3 c = p0 + d * u;
    const glm::vec3 e = pos - c;
    *pdist = std::sqrt((e.x * e.x + e.y * e.y) + e.z * e.z);
}
}  // namespace

// orig: 0x00197F98's target search (see jump.h)
void FindJumpHideTarget(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    SW* psw = g_psw;
    const glm::vec3 pos = PosActorJ(pa);  // sp+0x10
    const glm::vec3 vel = VelActorJ(pa);  // sp+0x20 (vt slot 33)
    const glm::vec3 base(pa->vecBase);    // +0x240
    LO* ploA = nullptr;  // s4
    LO* ploB = nullptr;  // s6
    float gBest = F32(0x44BB8000), tBest = 0.0f;  // f22 (1500) / f23
    glm::vec3 posEnd(0.0f), velEnd(0.0f);  // sp+0x40 / sp+0x60
    // the side test (0x11EE08): the horizontal relative motion over min(t, 0.25) passes within the limit of the target
    // and t is not 0; else within a quarter of it, not rising
    auto fSide = [&](const glm::vec3& t, const glm::vec3& vT, float tt, float lim) {
        glm::vec3 d = pos - t;
        const float m = std::min(0.25f, tt);
        const glm::vec3 dv = vel - vT;
        glm::vec3 a = d + dv * m;
        d.z = 0.0f;
        a.z = 0.0f;
        float u = 0.0f, dist = 0.0f;
        NearestOnSegment(glm::vec3(0.0f), d, a, &u, &dist);
        if (lim < dist || u == 0.0f)
        {
            if (lim * 0.25f < dist) return false;
            if (0.0f < vel.z) return false;
        }
        return true;
    };
    // list A (0x3E13E4, cid 0x58): free, within 600 horizontally, its top (+0x158 above it) not below the actor
    for (LO* plo : AploJtList(psw, JTL_Jump))
    {
        if (NHideState(plo) != 0) continue;
        const glm::vec3 t = PosJtLo(plo);  // 0x1A62E8
        const glm::vec3 d0 = t - pos;
        if (F32(0x48AFC800) < d0.x * d0.x + d0.y * d0.y) continue;
        if (t.z + SHideHeight(plo) < pos.z) continue;
        float tt = 0.0f, sc = 0.0f;
        glm::vec3 pv(0.0f), pw(0.0f);
        FitJumpToPoint(pj, vel, plo, t - base, glm::vec3(0.0f), &tt, &sc, &pv, &pw);  // 0x19A4A0
        if (FPrint()) std::printf("jumptgt: hide %d fit score %.2f t %.3f (state %d, pos %.1f %.1f %.1f vel %.1f %.1f %.1f)\n", plo->oid, sc, tt, pj->state, pos.x, pos.y, pos.z, vel.x, vel.y, vel.z);
        if (gBest < sc) continue;
        const glm::vec3 vT = FIsLoDerivedFrom(plo, Icid(CID3::ALO)) ? VelAloPos(static_cast<ALO*>(plo)) : glm::vec3(0.0f);  // vt slot 33
        if (!fSide(t, vT, tt, F32(0x42C80000))) continue;  // 100 (0x2BF83C)
        ploA = plo;
        ploB = nullptr;
        posEnd = pv;
        velEnd = pw;
        tBest = tt;
        gBest = sc;
    }
    // list B (0x3E1420, cid 0x28): within 600 (0x2BF840) horizontally, its own side limit +0xE8 (100)
    for (LO* plo : AploJtList(psw, JTL_Jump2))
    {
        const glm::vec3 t = PosJtLo(plo);
        const float r = F32(0x44160000);
        const glm::vec3 d0 = pos - t;
        if (r * r < d0.x * d0.x + d0.y * d0.y) continue;
        float tt = 0.0f, sc = 0.0f;
        glm::vec3 pv(0.0f), pw(0.0f);
        FitJumpToPoint(pj, vel, plo, t - base, glm::vec3(0.0f), &tt, &sc, &pv, &pw);
        if (gBest < sc) continue;
        XF* pxfT = PxfSlot32(plo);
        const glm::vec3 vT = pxfT ? VelXfPoint(pxfT, nullptr, t) : glm::vec3(0.0f);  // 0x22D348
        const uint32_t u = plo->U32Field(0, 0xE8, 0x42C80000u);  // (0x19C9B8: 100)
        float lim;
        std::memcpy(&lim, &u, 4);
        if (!fSide(t, vT, tt, lim)) continue;
        ploB = plo;
        ploA = nullptr;
        posEnd = pv;
        velEnd = pw;
        tBest = tt;
        gBest = sc;
    }
    if (!ploA && !ploB) return;
    // the jump aimed: the record's time, its end and end velocity (the launch's z plus g t) in its parent's space
    if (XF* pxf = PxfPos(pa->palo))
    {
        pxf->tTarget = g_clock.t + tBest;
        pxf->aseg[0] = glm::vec4(PosConvertXfs(nullptr, pxf->pxfRef, posEnd), 0.0f);  // 0x22FC48
        const float z = vel.z + tBest * GravActor(pa).z;
        pxf->aseg[1] = glm::vec4(VecConvertXfs(nullptr, pxf->pxfRef, glm::vec3(velEnd.x, velEnd.y, z)), 0.0f);  // 0x22FCC8
    }
    if (FPrint())
        std::printf("jumptgt: jump to %s %d t %.3f score %.2f end %.1f %.1f %.1f\n", ploA ? "hide" : "list B", (ploA ? ploA : ploB)->oid, tBest, gBest,
                    posEnd.x, posEnd.y, posEnd.z);
    if (ploA) SetJumpAnimState(pj, 6, ploA);  // 0x195FB8
    else SetJumpAnimState(pj, 8, ploB);
}

// orig: 0x00197F98's path to the state's object and its speed test (see jump.h)
void UpdateJumpTargetPath(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    XF* pxf = PxfPos(pa->palo);
    if (!pxf) return;
    const int k = pj->state;
    if ((k == 6 || k == 8) && pj->ploC)
    {
        // the object's point (less +0x240) where the object's record will be when the jump ends (0x2309E8 over the
        // time left), and its velocity there; the end velocity's z the actor's now plus g t
        const glm::vec3 vel = VelActorJ(pa);
        const glm::vec3 rel = PosJtLo(pj->ploC) - glm::vec3(pa->vecBase);  // sp+0x30
        const float f20 = pxf->tTarget - g_clock.t;
        XF* ps = PxfSlot32(pj->ploC);
        glm::mat4 mP(1.0f), mV(0.0f);
        PredictXfChain(ps, nullptr, f20, &mP, &mV);
        const glm::vec3 v150 = PosConvertXfs(nullptr, ps, rel);  // 0x22FC48
        const glm::vec3 r = glm::vec3(mP * glm::vec4(v150, 1.0f));
        pxf->aseg[0] = glm::vec4(PosConvertXfs(nullptr, pxf->pxfRef, r), pxf->aseg[0].w);
        glm::vec3 r2 = glm::vec3(mV * glm::vec4(v150, 1.0f));
        r2.z = vel.z + f20 * GravActor(pa).z;
        pxf->aseg[1] = glm::vec4(VecConvertXfs(nullptr, pxf->pxfRef, r2), pxf->aseg[1].w);
    }
    if (k == 6 || (k >= 8 && k < 17) || k == 18)
    {
        // too fast a path (its accelerations at both ends over half the time left, more than 10000): given up
        const float f = (pxf->tTarget - g_clock.t) + g_clock.dt;
        glm::vec3 a0(0.0f), a1(0.0f);
        const glm::vec3 p0(pxf->mat[3]), v0(pxf->vel), p1(pxf->aseg[0]), v1(pxf->aseg[1]);
        EvalHermiteVector(p0, v0, p1, v1, nullptr, nullptr, &a0, f, 0.0f, 1.0f);  // 0x117120
        EvalHermiteVector(p0, v0, p1, v1, nullptr, nullptr, &a1, f, f, 1.0f);
        const float la = 0.0f + std::sqrt((a0.x * a0.x + a0.y * a0.y) + a0.z * a0.z);
        const float lb = 0.0f + std::sqrt((a1.x * a1.x + a1.y * a1.y) + a1.z * a1.z);
        if (F32(0x461C4000) < (la + lb) * (f * 0.5f))  // 10000 (0x2BF844)
        {
            if (FPrint()) std::printf("jumptgt: the path in state %d too fast (%.1f): given up\n", k, (la + lb) * (f * 0.5f));
            SetJumpAnimState(pj, 4, nullptr);
        }
    }
}
