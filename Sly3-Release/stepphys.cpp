// The stepping objects' physics (class 0x33; see step.h). Ported from Sly2-Reference/Sly2-Release/game/m_2040F8.cpp and
// m_203948_p3.cpp (oracle-verified RAM-level C) and the decomp's func_002048C8.c; plain IEEE floats (the
// reference's ps2_* operations round toward zero).

#include "stepphys.h"
#include "sly3/ids3.h"
#include "actmesh.h"
#include "actor.h"

#include "anim.h"
#include "clock.h"
#include "cls.h"
#include "disc.h"
#include "xfmath.h"
#include "mat.h"
#include "so.h"
#include "util.h"
#include "vtnative.h"
#include "xfphys.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace
{
constexpr float EPS = 1e-4f;                              // 0x38D1B717
constexpr float G_FLOOR_NORMAL = 0.7f;                    // 0x3F333333: a floor's normal z
constexpr float SV_FLOOR_UP = 9000.0f;                    // 0x460CA000
constexpr float SV_FLOOR_DOWN = -4500.0f;                 // 0xC58CA000
constexpr float MU_FLOOR = 1.5f;                          // 0x2C3C90
constexpr float AG_SIDE[2] = {1.0f, -1.0f};               // 0x2C3C60
const SMP SMP_FLOOR_HEIGHT{3000.0f, 0.0f, 0.35f};         // 0x2C3C98
const SMP SMP_FLOOR_REST{3000.0f, 0.0f, 0.5f};            // 0x2C3CA8
const glm::vec3 SV_BRAKE{25.0f, 0.0f, 0.25f};             // 0x2C3CB8
const glm::vec3 NORMAL_Z(0.0f, 0.0f, 1.0f);               // 0x2A2DC0
const glm::vec3 NORMAL_X(1.0f, 0.0f, 0.0f);               // 0x2A2DA0

inline float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }


SOE* PsoeOf(ALO* palo, int i) { return i >= 0 && size_t(i) < palo->asoe.size() ? &palo->asoe[i] : nullptr; }

glm::vec3 PosProbe(ALO* palo, const STEPPHYS& s, int ipos)
{
    const SOE* pe = PsoeOf(palo, s.isoeCustom);
    if (!pe || ipos < 0 || size_t(ipos) >= pe->geomWorld.apos.size()) return PosAloCenter(palo);
    return pe->geomWorld.apos[ipos];
}

uint32_t VtGrfXp(ALO* palo)  // slot 85
{
    using PFN = uint32_t (*)(ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(palo, 85))) return pfn(palo);
    return 0;
}

void XpNotifyBase(ALO*, XP*, int) {}  // 0x1F4FB0 / 0x1F4FC0: nothing
}  // namespace

bool FAloStep(const LO* plo) { return plo && FCidDerivedFrom(plo->Cid(), Icid(CID3::PO)); }

STEPPHYS* PstepPhysOf(ALO* palo)
{
    if (!FAloStep(palo)) return nullptr;
    return &StepPhysOf(palo);
}

STEPPHYS& StepPhysOf(ALO* palo)
{
    STEP* p = PstepFromLo(palo);
    if (!p)
    {
        // orig: 0x00203948 (init: no +0x264 oid, sizes 1, speed scale 1 (0x204D48), heading 0, +0x220 30, +0x214 -50)
        p = PstepNew(palo);
        p->oidMesh = -1;
        p->sSize = palo && FCidDerivedFrom(palo->Cid(), Icid(CID3::GUARD)) ? 1.5f : 1.0f;  // (the guards' init 0x205EB8: 1.5)
        p->uBrake = 1.0f;
        STEPPHYS& s = *p;
        s.vBrake = SV_BRAKE;
        s.gScale = 1.0f;
        s.radHeading = 0.0f;
        s.matHeading = BuildRotateMatrixAxis(0.0f, NORMAL_Z);
        s.g220 = 30.0f;
        s.g214 = -50.0f;
        // the class inits' +0x264 (Sly's 0x188488: 0xC7), an option over it
        const uint32_t oidDefault = palo && FCidDerivedFrom(palo->Cid(), Icid(CID3::JT)) ? 0xC7u : 0xFFFFFFFFu;
        if (palo) s.oidMesh = static_cast<int>(palo->U32Field(0, 0x264, oidDefault));
    }
    return *p;
}

void ResetStepPhysAll() { ResetSteps(); }

void CloneStepPhys(ALO* palo, const ALO* paloSrc)
{
    STEP* psrc = PstepFromLo(const_cast<ALO*>(paloSrc));
    if (!psrc) return;
    CopyStep(palo, psrc);
}

// orig: 0x00204D48 (the scale +0x1F0; the 0x2C3CB8 speeds scaled by it (x, y) and by 1 / sqrt of it (z))
void SetStepPhysScale(ALO* palo, float g)
{
    STEPPHYS& s = StepPhysOf(palo);
    s.vBrake = SV_BRAKE;
    s.vBrake.x *= g;
    s.vBrake.y *= g;
    s.gScale = g;
    s.vBrake.z *= 1.0f / std::sqrt(g);
}

// orig: 0x00204660 (the heading +0x1A8 and its rotation +0x1B0: 0x11B500 about z)
void SetStepPhysHeading(ALO* palo, float rad)
{
    STEPPHYS& s = StepPhysOf(palo);
    s.radHeading = rad;
    s.matHeading = BuildRotateMatrixAxis(rad, NORMAL_Z);
}

// orig: 0x002046B0
void SetStepPhysHeadingToward(ALO* palo, const glm::vec3& dir) { SetStepPhysHeading(palo, std::atan2(dir.y, dir.x)); }

// orig: 0x00204DA8 (the size +0x260 (at least 0.1): the mass scaled from the old size (0x1F35C8), the mass state's
// +0x80 (the gravity) scaled by s / old)
void SetStepPhysSize(ALO* palo, float g)
{
    STEPPHYS& st = StepPhysOf(palo);
    const float s = std::max(0.1f, g);
    SOST* ps = Psost(palo);
    if (ps)
    {
        SetAloMass(palo, (ps->m * st.sSize) / s);
        ps->dvGravity = ps->dvGravity * s * (1.0f / st.sSize);
    }
    st.sSize = s;
}

// orig: 0x002039C0's part (after the ALO's load 0x1C3DC8: +0x240 = 0x1A6F00(parent, p, 0): the parent's origin in
// the object's frame)
void LoadStepPhysBase(ALO* palo)
{
    STEPPHYS& s = StepPhysOf(palo);
    glm::vec3 v(0.0f);
    if (palo->paloParent && PxfObj(palo->paloParent)) v = XformPos(PxfObj(palo->paloParent)->matWorld, v);
    else if (PxfPos(palo))
        // a root: the original runs this at the load, before the object is placed (its position record still the
        // identity), so the world origin is the position record's origin: natively that, in the object's frame
        // (the position record's parent's origin: the world origin of the original's load; Murray's position record has
        // its own offset (0, 0, 71.76), so the base is not the position record's origin)
        v = PosOf(PxfPos(palo)->pxfRef ? PxfPos(palo)->pxfRef->matWorld : PxfPos(palo)->matWorld);
    if (PxfObj(palo)) v = XformPos(InverseAffineMatrix4(PxfObj(palo)->matWorld), v);
    s.vecBase = glm::vec4(v, 1.0f);
    // Sly's load 0x1885B8 after 0x2039C0: the base (0, 0, -75)
    if (FCidDerivedFrom(palo->Cid(), Icid(CID3::JT))) s.vecBase = glm::vec4(0.0f, 0.0f, -75.0f, s.vecBase.w);
}

// orig: 0x00203A38's part (after the ALO's post-load 0x1F36A8): the position / rotation records in mode 1, the
// custom entry from the +0x264 entry (0x1F6030, 0x203AF0, 0x1F2AD0)
void PostLoadStepPhys(ALO* palo)
{
    STEPPHYS& s = StepPhysOf(palo);
    if (PxfPos(palo)) SetXfMode(palo, PxfPos(palo), 1);
    if (PxfRot(palo)) SetXfMode(palo, PxfRot(palo), 1);
    if (s.oidMesh == -1) return;
    for (size_t i = 0; i < palo->asoe.size(); i++)
        if (palo->asoe[i].tag == s.oidMesh)
        {
            s.isoeSource = int(i);
            MakeStepCustomEntry(palo, &palo->asoe[i]);
            SetSoeOff(palo, &palo->asoe[s.isoeSource]);
            break;
        }
}

// orig: 0x00203AF0 (the next entry as +0x258, rebuilt from m in the object record's space (0x1F70C0 with 2 extra
// points, 3 when all of m's points are pinned and the object is not an actor), its moving mesh (0x1F6C08), +0x210 its
// lowest pinned point; the probe points appended: the base +0x240 and +0x214 below it (and the origin); its world
// geometry; +0x218 / +0x21C the probes' world points; a live object's entries re-added)
void MakeStepCustomEntry(ALO* palo, SOE* psoeSrc)
{
    STEPPHYS& s = StepPhysOf(palo);
    bool fLive = palo->fInWorld;
    for (const SOE& e : palo->asoe) fLive = fLive || e.fInOxa;  // (the entries' addresses change natively)
    if (fLive) RemoveAloSoes(palo);
    const int isrc = int(psoeSrc - palo->asoe.data());
    palo->asoe.emplace_back();
    psoeSrc = &palo->asoe[isrc];
    s.isoeCustom = int(palo->asoe.size()) - 1;
    SOE* pe = &palo->asoe[s.isoeCustom];
    bool fl = false;
    if (!FCidDerivedFrom(palo->Cid(), Icid(CID3::PLAYER))) fl = int(psoeSrc->pinned) == int(psoeSrc->geomLocal.apos.size());
    RebuildSoe(psoeSrc, PxfObj(palo), fl ? 3 : 2, pe);
    pe->palo = palo;
    EnsureSoeMesh(pe);
    float mn = FLT_MAX;  // 0x2A85F0
    for (int i = 0; i < pe->pinned && size_t(i) < pe->geomLocal.apos.size(); i++) mn = std::min(pe->geomLocal.apos[i].z, mn);
    s.sFloorTarget = mn;
    const int c = pe->pinned;
    pe->pinned = int16_t(c + 2);
    auto set = [&](int i, const glm::vec3& v) {
        if (size_t(i) < pe->geomLocal.apos.size()) pe->geomLocal.apos[i] = v;
    };
    const glm::vec3 base(s.vecBase);
    set(c, base);
    set(c + 1, base + NORMAL_Z * s.g214);
    if (fl) set(c + 2, glm::vec3(0.0f));
    {
        const glm::mat4 m = pe->pxf ? pe->pxf->matWorld : g_matIdentity4;
        TransformGeom(m, pe->geomLocal, &pe->geomWorld);  // 0x1664A0
    }
    s.iposProbe0 = c;
    s.iposProbe1 = c + 1;
    if (fLive) AddAloSoes(palo);
}

// orig: 0x00203FC0's part (after the base 0x1F36E0 and the meshes' velocities 0x203E08): +0x180 by the mode (0: the
// gain of the heading error against the object's first row, 1.5 - |d| clamped to [0, 1]; +-1: 1), +0x20C the time
// while there is a floor
void UpdateStepPhysTurn(ALO* palo)
{
    STEPPHYS& s = StepPhysOf(palo);
    if (s.nMotion == 0)
    {
        const glm::mat4 m = PxfObj(palo) ? PxfObj(palo)->matWorld : g_matIdentity4;
        const float h = std::atan2(m[0][1], m[0][0]);
        const float d = std::fabs(RadNormalize(s.radHeading - h));
        float f = 1.5f + d * (-1.0f + d * 0.0f);  // 0x2C3C80
        f = std::clamp(f, 0.0f, 1.0f);            // 0x2BBB60
        s.uTurn = f;
    }
    else if (s.nMotion == 1 || s.nMotion == -1)
        s.uTurn = 1.0f;
    if (s.pxpFloor) s.tFloor = g_clock.t;
}

// orig: 0x00204C60 (vt slot 54: the base 0x1F5988, then the floor forgotten when it is the other object's)
void SetStepOverlap(ALO* palo, ALO* paloOther)
{
    STEPPHYS& s = StepPhysOf(palo);
    if (s.pxpFloor && s.pxpFloor->axpd[1].paloRoot == paloOther) s.pxpFloor = nullptr;
}
NATIVEFN(0x204C60, SetStepOverlap);

// orig: 0x00204250 (vt slot 55, a new contact of the object (its end i): kind 14 gets its friction and the row's
// limits (-4500 / +9000 times the mass; below a floor normal only the upper one); kinds 3 (and 1, 4, 5, 6 at end 1) facing
// the other way along z get a kind-10 contact pushing them apart horizontally)
void OnStepXpNew(ALO* palo, XP* q, int n)
{
    XpNotifyBase(palo, q, n);
    const int k = q->xpk;
    q->mu = 0.0f;
    q->elas = 0.0f;
    if (k == 0xE)
    {
        const float m = Psost(palo) ? Psost(palo)->m : 0.0f;
        if (G_FLOOR_NORMAL < q->normal.z)
        {
            q->mu = MU_FLOOR;
            q->gA0 = m * SV_FLOOR_DOWN;
            q->gA4 = m * SV_FLOOR_UP;
        }
        else
            q->gA4 = m * SV_FLOOR_UP;
        return;
    }
    if (k != 3)
    {
        if (n != 1) return;
        if (k != n && k != 4 && k != 5 && k != 6) return;
    }
    const float s = AG_SIDE[n];
    if (!(0.0f < s * q->normal.z)) return;
    glm::vec3 v = q->normal * s;
    v.z = 0.0f;
    const float len = std::sqrt(std::fabs(Dot3(v, v)));
    const glm::vec3 d = len < EPS ? NORMAL_X : v * (1.0f / len);
    const glm::vec3 pa = q->axpd[n].pos, pb = q->axpd[1 - n].pos;
    const float dot = Dot3(pa - pb, d);
    const glm::vec3 r = pb + d * dot;
    STEPPHYS& st = StepPhysOf(palo);
    PxpAddSw(PsoeOf(palo, st.isoeCustom), q->axpd[1 - n].psoe, r, d, 10, nullptr, nullptr, &q->pxpNext, dot);
}
NATIVEFN(0x204250, OnStepXpNew);

// orig: 0x002044B0 (vt slot 57, a contact's update (its end n): a floor contact's end velocity set so that its
// separation springs toward the object's lowest point (the approach speed at least the smoothed one, the change
// limited to +9000 / -4500 per second), sliding along the lead vector +0x190 by the gain +0x180; the separation follows the
// low point's change)
void OnStepXpUpdate(ALO* palo, XP* q, int n)
{
    XpNotifyBase(palo, q, n);
    if (q->xpk != 0xE) return;
    STEPPHYS& s = StepPhysOf(palo);
    const glm::vec3 c = q->normal;
    float h = Dot3(q->axpd[0].vel - q->axpd[1].vel, c);
    const float dt = g_clock.dt;
    float f0 = 0.0f, f2 = 0.0f;
    GSmooth(q->gDist, s.sFloorTarget, dt, &SMP_FLOOR_HEIGHT, &f0);
    GSmooth(q->gDist, 0.0f, dt, &SMP_FLOOR_REST, &f2);
    h = std::max(f0, h);
    float step;
    if (h < f2)
        step = std::min(dt * SV_FLOOR_UP, f2 - h);
    else
        step = std::max(dt * SV_FLOOR_DOWN, f2 - h);
    h = h + step;
    const glm::vec3 w = s.matHeading * s.vecVel;
    const float t = Dot3(w, c);
    const glm::vec3 wn = w - c * t;
    glm::vec3& o = q->axpd[n].vel;
    o = (o + c * -h) + wn * s.uTurn;
    q->gDist = q->gDist + (s.sFloorTarget - s.vecBase.z);
}
NATIVEFN(0x2044B0, OnStepXpUpdate);

// orig: 0x00204768 (vt slot 58 after the base 0x1F4F10: the lowest kind-14 contact (smallest separation) of the
// attachment's overlaps and internal list is the floor +0x208 (time +0x20C, point +0x230 its end 1); the object's
// other kind-14 contacts are freed)
void FindStepFloor(ALO* palo)
{
    NotifyAloContacts(palo);  // the base 0x1F4F10 (the sensor contacts posted)
    STEPPHYS& s = StepPhysOf(palo);
    OXA* poxa = Poxa(palo);
    if (!poxa) return;
    float best = FLT_MAX;  // 0x2A85F4
    XP* pbest = nullptr;
    auto scan = [&](XP* pxp) {
        for (XP* x = pxp; x; x = x->pxpNext)
            if (x->xpk == 0xE && x->gDist < best)
            {
                best = x->gDist;
                pbest = x;
            }
    };
    for (OX* ox = poxa->pox; ox; ox = ox->poxNext) scan(ox->pxp);
    scan(poxa->pxpInternal);
    s.pxpFloor = pbest;
    if (pbest)
    {
        s.tFloor = g_clock.t;
        s.posFloor = glm::vec4(pbest->axpd[1].pos, 1.0f);
    }
    XP* pxpFreed = nullptr;
    auto prune = [&](XP** ppxp) {
        XP** link = ppxp;
        while (XP* x = *link)
        {
            const XPD& side = x->axpd[x->axpd[0].paloRoot != palo ? 1 : 0];
            if (side.palo == palo && x->xpk == 0xE && x != pbest)
            {
                *link = x->pxpNext;
                x->pxpNext = pxpFreed;
                pxpFreed = x;
            }
            else
                link = &x->pxpNext;
        }
    };
    for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
    {
        prune(&ox->pxp);
        if (ox->poxOther) ox->poxOther->pxp = ox->pxp;
    }
    prune(&poxa->pxpInternal);
    FreeSwXpList(pxpFreed);
}
NATIVEFN(0x204768, FindStepFloor);

// orig: 0x002048C8 (from the face isurf, over the edges within 1.45 times the climb (+0x210 - +0x248) of pos and not
// above it by more than the climb, a breadth-first walk of at most 32 faces: true at the first one with normal
// z > 0.7)
bool FStepFloorNear(ALO* palo, const glm::vec3& pos, const SOE* psoe, int isurf)
{
    STEPPHYS& s = StepPhysOf(palo);
    const GEOM& g = psoe->geomWorld;
    if (isurf < 0 || size_t(isurf) >= g.asurf.size()) return false;
    int aq[32];
    int c = 1, cur = 0;
    const float delta = s.sFloorTarget - s.vecBase.z;
    const float limit = delta * 1.45f;
    aq[0] = isurf;
    do
    {
        const SURF& poly = g.asurf[aq[cur]];
        for (int side = 0; side < 2; side++)
        {
            for (int ie = side == 0 ? poly.iedgeFirst : poly.iedgeOfOthers; ie >= 0 && size_t(ie) < g.aedge.size();
                 ie = side == 0 ? g.aedge[ie].iedgeNextInSurf : g.aedge[ie].iedgeNextOfOther)
            {
                const EDGE& e = g.aedge[ie];
                if (e.ipos0 >= g.apos.size() || e.ipos1 >= g.apos.size()) continue;
                // 0x11EE08: the nearest point of the edge
                const glm::vec3 a = g.apos[e.ipos0], b = g.apos[e.ipos1];
                const glm::vec3 ab = b - a;
                float t = Dot3(pos - a, ab) / Dot3(ab, ab);
                if (t < 0.0f) t = 0.0f;
                else if (1.0f < t) t = 1.0f;
                const glm::vec3 closest = a + ab * t;
                const glm::vec3 dd = pos - closest;
                const float distance = std::sqrt(Dot3(dd, dd));
                if (limit < distance) continue;
                if (pos.z + delta < closest.z) continue;
                const int ineighbor = side == 0 ? e.isurfOther : e.isurf;
                if (size_t(ineighbor) >= g.asurf.size()) continue;
                if (G_FLOOR_NORMAL < g.asurf[ineighbor].normal.z) return true;
                bool fSeen = false;
                for (int j = 0; j < c; j++)
                    if (aq[j] == ineighbor) fSeen = true;
                if (!fSeen && c < 32) aq[c++] = ineighbor;
            }
        }
    } while (++cur < c);
    return false;
}

// orig: 0x00204AC8 (vt slot 60, the own contact test against the other entry k (not of a stepping object), when slot
// 85 allows it (1): the segment from the object's position to the probe (2: the long one) through k; a hit (t > 0)
// adds a kind-14 contact at the base probe along the hit's normal (8: as it is; else up when it is a floor's, or when
// a floor is near over the face's edges (0x2048C8)), separated by the base's height over the hit)
void AddStepFloorXps(ALO* palo, SOE* k, XP** ppxp)
{
    const uint32_t fl = VtGrfXp(palo);
    if (!(fl & 1)) return;
    ALO* pkRoot = k && k->palo && Poxa(k->palo) ? Poxa(k->palo)->pso : nullptr;
    if (FAloStep(pkRoot)) return;
    STEPPHYS& s = StepPhysOf(palo);
    const glm::vec3 pos = PosAloCenter(palo);
    const glm::vec3 v = PosProbe(palo, s, (fl & 2) ? s.iposProbe1 : s.iposProbe0);
    LSG lsg;
    if (ClipSegmentEntry(k, pos, v, 1, &lsg) <= 0) return;
    if (!(0.0f < lsg.t0)) return;
    glm::vec3 nrm(lsg.plane0);
    if (!(fl & 8))
    {
        if (G_FLOOR_NORMAL < lsg.plane0.z)
            nrm = NORMAL_Z;
        else if (lsg.fSphere == 0)
        {
            const int isurf = lsg.psurf0 && !k->geomWorld.asurf.empty() ? int(lsg.psurf0 - k->geomWorld.asurf.data()) : -1;
            if (FStepFloorNear(palo, PosProbe(palo, s, s.iposProbe0), k, isurf)) nrm = NORMAL_Z;
            else nrm = glm::vec3(lsg.plane0);
        }
    }
    const glm::vec3 at = PosProbe(palo, s, s.iposProbe0);
    const float dot = Dot3(at - lsg.pos0, nrm);
    PxpAddSw(PsoeOf(palo, s.isoeCustom), k, at, nrm, 0xE, nullptr, nullptr, ppxp, dot);
}
NATIVEFN(0x204AC8, AddStepFloorXps);

// orig: 0x00204108 (vt slot 70: gravity (0x1F9B20), then by +0x1A4: 0 the rotation braked in the heading frame
// (0x1F59A0 with the speeds +0x1F4 / +0x1F8 times +0x200 and +0x1FC); 1 the rotation record's acceleration pushed so
// that its velocity reaches +0x1E0 in one frame)
void UpdateStepPhysMotion(ALO* palo)
{
    ApplyAloGravity(palo);
    STEPPHYS& s = StepPhysOf(palo);
    if (s.nMotion == 0)
    {
        const float r = s.uBrake;
        const float ac[3] = {s.vBrake.x * r, s.vBrake.y * r, s.vBrake.z};
        BrakeAloRotation(palo, s.matHeading, ac);
    }
    else if (s.nMotion == 1)
    {
        XF* po = PxfRot(palo);
        if (!po) return;
        const float q = g_clock.dt != 0.0f ? 1.0f / g_clock.dt : FLT_MAX;
        po->aseg[3] = po->aseg[3] + (s.posTarget - po->vel) * q;
    }
}
NATIVEFN(0x204108, UpdateStepPhysMotion);

// orig: 0x002040F8 (vt slot 85: 7 unless +0x204)
uint32_t GrfStepXp(ALO* palo) { return StepPhysOf(palo).fNoContacts != 0 ? 0u : 7u; }
NATIVEFN(0x2040F8, GrfStepXp);

namespace
{
// orig: 0x00204248 (vt slot 63: 1)
int StepSlot63(ALO*) { return 1; }
}
NATIVEFN(0x204248, StepSlot63);

// ---- Sly's (class 0x35) physics overrides that are physics alone ----

// orig: 0x00188E78 (Sly's vt slot 57: the base 0x2044B0, then for its own end the velocity without the rotation's
// part at the end's point (slot 34's angular velocity across the point from the object's position))
void OnSlyXpUpdate(ALO* palo, XP* q, int i)
{
    OnStepXpUpdate(palo, q, i);
    XPD& e = q->axpd[i];
    if (e.palo != palo) return;
    using PFN = glm::vec3 (*)(ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(palo, 34));
    const glm::vec3 w = pfn ? pfn(palo) : AngVelAlo(palo);
    const glm::vec3 r = e.pos - PosAloCenter(palo);
    e.vel = e.vel - glm::cross(w, r);
}
NATIVEFN(0x188E78, OnSlyXpUpdate);

// ---- The second mesh (+0x25C): the custom entry's points follow one of the object's entries (the players' per-state
// shapes: e.g. Sly's on a pipe (state 8) has its front point 11 ahead instead of 90)

namespace
{
// M1's points carried into the custom entry's (M0's) space: inverse(M0's record) x M1's record (0x11C318 / 0x11C168)
glm::mat4 MatStepMeshRelative(const SOE* pm0, const SOE* pm1)
{
    const glm::mat4 m0 = pm0->pxf ? pm0->pxf->matWorld : glm::mat4(1.0f);
    const glm::mat4 m1 = pm1->pxf ? pm1->pxf->matWorld : glm::mat4(1.0f);
    return glm::inverse(m0) * m1;
}
}

// orig: 0x00203D00 func_00203D00 (+0x25C = the entry; with fSnap the custom entry's points become its points carried
// into the custom entry's space, their velocities cleared, the attachment dirty (0x1F4BF8))
void SetStepMesh(ALO* palo, int isoe, bool fSnap)
{
    STEPPHYS& s = StepPhysOf(palo);
    s.isoeOther = isoe;
    if (!fSnap) return;
    SOE* pm0 = PsoeOf(palo, s.isoeCustom);
    const SOE* pm1 = PsoeOf(palo, isoe);
    if (!pm0 || !pm1) return;
    const glm::mat4 m = MatStepMeshRelative(pm0, pm1);
    for (size_t i = 0; i < pm1->geomLocal.apos.size() && i < pm0->geomLocal.apos.size(); i++)
        pm0->geomLocal.apos[i] = glm::vec3(m * glm::vec4(pm1->geomLocal.apos[i], 1.0f));
    if (pm0->pmesh) std::fill(pm0->pmesh->av.begin(), pm0->pmesh->av.end(), glm::vec3(0.0f));
    MarkAloDirty(palo);
}

// orig: 0x00203E08 func_00203E08 (from the update 0x203FC0: each of the second mesh's pinned points carried into the
// custom entry's space; the custom entry's point velocity per coordinate (GSmooth from its point toward it with the
// SMP 0x2C3C68 (300, 0, 0.25) over dt - its point) / dt; the physics frame moves the points (0x1F6EF8))
void UpdateStepMesh(ALO* palo)
{
    STEPPHYS* ps = PstepPhysOf(palo);
    if (!ps) return;
    SOE* pm0 = PsoeOf(palo, ps->isoeCustom);
    const SOE* pm1 = PsoeOf(palo, ps->isoeOther);
    if (!pm0 || !pm1 || !pm0->pmesh) return;
    const float dt = g_clock.dt;
    if (!(dt > 0.0f)) return;
    static const SMP s_smp{300.0f, 0.0f, 0.25f};  // 0x2C3C68
    const glm::mat4 m = MatStepMeshRelative(pm0, pm1);
    for (int i = int(pm1->pinned) - 1; i >= 0; i--)
    {
        if (size_t(i) >= pm1->geomLocal.apos.size() || size_t(i) >= pm0->geomLocal.apos.size() ||
            size_t(i) >= pm0->pmesh->av.size())
            continue;
        const glm::vec3 t(m * glm::vec4(pm1->geomLocal.apos[i], 1.0f));
        const glm::vec3 old = pm0->geomLocal.apos[i];
        glm::vec3 v;
        for (int c = 0; c < 3; c++) v[c] = (GSmooth(old[c], t[c], dt, &s_smp, nullptr) - old[c]) / dt;
        pm0->pmesh->av[i] = v;
    }
}

// The players' slot 90: actmesh.cpp (actors) UpdateActorStepMesh (Sly 0x18C060, Bentley 0x135D80, Murray 0x1B8008, with
// the sub-objects' meshes 0x1944C8 / 0x1A23F8 / 0x19C870); kept here for the callers by object and state.
void SetPlayerStepMeshForState(ALO* palo, int)
{
    if (ACTOR* pa = PactorFromLo(palo)) UpdateActorStepMesh(pa);
}
