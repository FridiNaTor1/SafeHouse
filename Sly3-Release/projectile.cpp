// projectile.cpp: the thrown objects (class 0x4F and its variants 0x50 / 0x51 / 0x52: the guards' strikes, Bentley's
// darts): their contacts end the flight, and the end hits the objects near the impact with a contact message (the
// receiver's slot 75: e.g. Sly's damage controller). Part of the AI code. Readable C: sly2-decomp/src/hand
// (func_0013AEB0.c, func_0013B428.c, func_0013B5C8.c, func_0013AB58.c, func_0013B2F0.c, func_0013B5E8.c,
// build/hand_wip/func_0013B860.cpp).

#include "projectile.h"

#include "activate.h"
#include "ainoise.h"
#include "clock.h"
#include "guard.h"
#include "lo.h"
#include "objmethod.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "xf.h"
#include "vecmat.h"

glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
// The class's state (orig +0x154.., +0x174..)
struct PROJ
{
    float tEnd = FLT_MAX;          // +0x154: when the flight ends (0x13B5C8; D_002A4B1C after the end)
    int fHit = 0;                  // +0x174: the impact recorded (0x13B428)
    glm::vec3 posHit{0.0f};        // +0x180
    glm::mat3 matHit{1.0f};        // +0x190
    glm::vec3 vHit{0.0f}, wHit{0.0f};  // +0x1C0 / +0x1D0
    LO* plaunch = nullptr;         // +0x200: the launcher that threw it (0x13BC60)
    uint32_t grfFilterSet = 0;     // (the thrower's contact kind: Bentley's dart 0x80000 (0x1357C0's +0xE4 +0x18); 0: +0x1EC)
};
std::unordered_map<const LO*, PROJ>& MpProj()
{
    static std::unordered_map<const LO*, PROJ> s_mp;
    return s_mp;
}
PROJ& Proj(const LO* plo) { return MpProj()[plo]; }

// a loaded field of the class (the options 486..511 at their offsets: as loaded, or set by a script)
uint32_t UField(const LO* plo, uint32_t ib, uint32_t uDefault) { return plo->U32Field(0, ib, uDefault); }
float GField(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t u;
    std::memcpy(&u, &gDefault, 4);
    u = UField(plo, ib, u);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

glm::vec3 Normal(const glm::vec3& v, const glm::vec3& vDefault)
{
    const float s = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return s < 0.0001f ? vDefault : v * (1.0f / s);
}
const glm::vec3 AXIS_Z(0.0f, 0.0f, 1.0f);  // (D_002A2DC0)

glm::mat3 MatRot(ALO* palo)
{
    // 0x1A6318: the object's rotation (its object record's)
    const glm::mat4 m = palo->apxfRef[2] ? palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
    return glm::mat3(m);
}

// orig: 0x0013B428: the impact recorded once (its place, the velocities, the orientation by +0x170: 0 its z row
// turned to n, 1 facing its velocity, 2 its x row turned to n, 3 kept)
void RecordHit(ALO* palo, const glm::vec3& pos, const glm::vec3& n)
{
    PROJ& p = Proj(palo);
    if (p.fHit) return;
    p.posHit = pos;
    p.fHit = 1;
    p.vHit = VelAloPos(palo);
    p.wHit = AngVelAlo(palo);
    const glm::mat3 m = MatRot(palo);
    switch (static_cast<int>(UField(palo, 0x170, 0)))
    {
    case 1: p.matHit = BuildLookAtMatrix(p.vHit, AXIS_Z); break;  // 0x11B830
    case 3: p.matHit = m; break;
    default:
    {
        // 0x11BA08: the rotation taking its z (0) or x (2) row to n, applied to its rows
        const int k = static_cast<int>(UField(palo, 0x170, 0)) == 2 ? 0 : 2;
        const glm::vec3 a = Normal(m[k], AXIS_Z), b = Normal(n, AXIS_Z);
        const glm::vec3 axis = glm::cross(a, b);
        const float s = glm::length(axis), c = glm::dot(a, b);
        glm::mat3 r(1.0f);
        if (s > 1e-6f) r = glm::mat3(glm::rotate(glm::mat4(1.0f), std::atan2(s, c), axis / s));
        p.matHit = r * m;
        break;
    }
    }
}

// orig: 0x0013B5C8: the flight ends within g
void ExpireIn(ALO* palo, float g)
{
    PROJ& p = Proj(palo);
    p.tEnd = std::min(p.tEnd, g_clock.t + g);
}

// orig: 0x0013B5E8: the objects near the impact the end hits: the world's entries within the larger radius (0x20FC60
// with the filter 0x20FA10: not bit 48, not the thrown object's own), not of tag 1 nor of the thrower's attachment,
// those with a tree touched by the sphere (0x1A93C8: a player's radius +0x160, else +0x15C); each object once
std::vector<LO*> AploVictims(ALO* palo, const glm::vec3& pos)
{
    const float r15C = GField(palo, 0x15C, 200.0f), r160 = GField(palo, 0x160, 200.0f);  // (0x13A9E8: D_002BC9C8)
    const float r = std::max(r15C, r160);
    std::vector<LO*> apo;
    OXA* poxaSelf = Poxa(palo);
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (poxa == poxaSelf) continue;
        const glm::vec3 d = poxa->posCenter - pos;
        const float s = poxa->sRadius + r;
        if (glm::dot(d, d) > s * s) continue;
        for (SOE* psoe : poxa->apsoe)
        {
            if (psoe->f48 || psoe->tag == 1 || !psoe->palo) continue;
            ALO* po = psoe->palo;
            if (!psoe->bsp.abspn.empty())
            {
                const float rTest = FCidDerivedFrom(PloRoot(po)->Cid(), 0x34) ? r160 : r15C;
                std::vector<glm::vec3> apos;
                std::vector<glm::vec4> aplane;
                std::vector<int> akind;
                std::vector<const void*> apv;
                if (CsbiSphereEntry(pos, psoe, nullptr, 1, rTest, &apos, &aplane, &akind, &apv) == 0) continue;
            }
            if (std::find(apo.begin(), apo.end(), po) == apo.end()) apo.push_back(po);
        }
    }
    return apo;
}

// orig: 0x0013B860: the end: the hits (a contact message to each object near the impact: filter +0x1EC, state
// +0x1F0, the impact's place, a push of 500 away from it) unless +0x164; the impact's place, orientation and
// velocities back; its owner told (+0x168's slot 51, message 9: TODO); a noise (709) at the impact by the thrower
// (+0x1E0); out of the world, its end time FLT_MAX, its script event 5
void EndProjectile(ALO* palo)
{
    if (!palo->fInWorld) return;
    if (std::getenv("SLY2N_PROJEND"))  // (debug: every projectile's end, with the time and place)
    {
        const glm::vec3 pos = PosAloCenter(palo);
        const XF* pxf = palo->apxfRef[0];  // (+0x58: the reference's +58@+70)
        const glm::vec3 posR = pxf ? glm::vec3(pxf->matWorld[3]) : pos;
        std::printf("projend t %.3f %d at %.1f %.1f %.1f record %.1f %.1f %.1f\n", g_clock.t, palo->oid, pos.x, pos.y, pos.z,
                    posR.x, posR.y, posR.z);
    }
    RecordHit(palo, PosAloCenter(palo), AXIS_Z);
    PROJ& p = Proj(palo);
    if (!UField(palo, 0x164, 0))
        for (LO* po : AploVictims(palo, p.posHit))
        {
            CTMSG ctm;
            ctm.ploSender = palo;
            ctm.ploReceiver = po;
            ctm.grfFilter = p.grfFilterSet ? p.grfFilterSet : UField(palo, 0x1EC, 0x100);  // (0x13A9E8 defaults: 0x100, -10)
            ctm.nState = static_cast<int>(UField(palo, 0x1F0, static_cast<uint32_t>(-10)));
            ctm.pos = p.posHit;
            ctm.vec = Normal(PosAi(po) - p.posHit, AXIS_Z) * 500.0f;  // (D_002BC9E0)
            if (std::getenv("SLY2N_HITDEBUG"))
                std::printf("projectile %d hits %d (cid 0x%X): filter 0x%X state %d\n", palo->oid, po->oid, po->Cid(), ctm.grfFilter,
                            ctm.nState);
            PaloAcceptContact(&ctm);  // 0x1F7598
        }
    if (STEP* pstep = PstepFromLo(palo)) SetStepPos(pstep, p.posHit - glm::vec3(pstep->vecBase));
    else SetAloPos(palo, p.posHit);
    SetAloRotation(palo, p.matHit);
    SetAloVelocityTo(palo, p.vHit);
    SetAloAngVelocityTo(palo, p.wHit);
    // TODO(ai): +0x168's slot 51 with {9, p, +0x16C, +0x15C}
    if (UField(palo, 0x1E0, 1))
    {
        // 0x156698(SW +0x402C (the fixed noise 709), its place, the thrower: the launcher's owner when it is a class-0x70)
        LO* plo = p.plaunch ? p.plaunch : palo;
        if (p.plaunch && p.plaunch != palo && FCidDerivedFrom(p.plaunch->Cid(), 0x70) && p.plaunch->paloParent)
            plo = p.plaunch->paloParent;
        if (NOISE* pn = PnoiseNew(PtmplNoiseFixed(3), PosAloCenter(palo), plo)) pn->nKind = 1;
    }
    RemoveLo(g_psw, palo);  // (vt slot 9)
    p.fHit = 0;
    p.tEnd = FLT_MAX;
    HandleLoSpliceEvent(palo, 5, false, 0, nullptr);  // (vt slot 3: event 5)
}

// orig: 0x0013AB58 (vt slot 16): the ALO's update, then the end when its time has come or it is slower than +0x158;
// with +0x1F4 the sweep for the objects in its path (0x13AC18: TODO(ai))
void UpdateProjectile(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    ALO* palo = static_cast<ALO*>(plo);
    if (std::getenv("SLY2N_PROJDEBUG"))
    {
        const glm::vec3 pos = PosAloCenter(palo), v = VelAloPos(palo);
        std::printf("projectile %d (cid 0x%X kind %d) update at t %.3f end %.3f pos %.1f %.1f %.1f v %.1f %.1f %.1f detonate below %.1f f1F4 %u\n",
                    palo->oid, palo->Cid(), static_cast<int>(UField(palo, 0x150, 0)), g_clock.t, Proj(palo).tEnd, pos.x, pos.y, pos.z,
                    v.x, v.y, v.z, GField(palo, 0x158, 0.0f), UField(palo, 0x1F4, 0));
    }
    if (Proj(palo).tEnd <= g_clock.t || glm::length(VelAloPos(palo)) < GField(palo, 0x158, 0.0f))
        EndProjectile(palo);
    // orig 0x13AB58's else: with +0x1F4 the sweep 0x13AC18 (and slot 60 0x13ADE8 adds its kind-9 contacts). No object
    // of any world has +0x1F4 after the load (reference RAM scan, gadgets8), so both do nothing so far
    else if (static bool s_fWarned = false; !s_fWarned && UField(palo, 0x1F4, 0))
    {
        s_fWarned = true;
        std::fprintf(stderr, "projectile: 0x13AC18 (the sweep of +0x1F4) not ported (object %d)\n", palo->oid);
    }
}

// orig: 0x0013AEB0 (vt slot 58, after the base 0x1F4F10): kinds 0 / 2 (+0x150) before the impact, 1 (TODO(ai)): the
// nearest contact of its overlaps (to its place) is the impact (kind 2: only a player's (0x32) that 0x1C4098 takes,
// TODO(ai)), the flight ends now
void ProjectileContacts(ALO* palo)
{
    NotifyAloContacts(palo);  // 0x1F4F10
    if (std::getenv("SLY2N_PROJDEBUG")) std::printf("projectile %d contacts at t %.3f\n", palo->oid, g_clock.t);
    const int k = static_cast<int>(UField(palo, 0x150, 0));
    if ((k == 0 || k == 2) && Proj(palo).fHit) return;
    if (k == 1) return;  // TODO(ai): +0x1E4 / +0x1E8, 0x13B1C8, 0x149A50
    OXA* poxa = Poxa(palo);
    if (!poxa) return;
    const glm::vec3 pos = PosAloCenter(palo);
    float gBest = FLT_MAX;  // (D_002A4B18)
    XP* pbest = nullptr;
    for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
        for (XP* x = ox->pxp; x; x = x->pxpNext)
        {
            const float g = glm::length(x->axpd[0].pos - pos);
            if (gBest <= g) continue;
            bool f = k == 0;
            if (k == 2)
            {
                const XPD& other = x->axpd[x->axpd[0].palo == palo ? 1 : 0];
                f = other.palo && FCidDerivedFrom(other.palo->Cid(), 0x32);  // TODO(ai): 0x1C4098
            }
            if (f)
            {
                gBest = g;
                pbest = x;
            }
        }
    if (!pbest) return;
    if (std::getenv("SLY2N_PROJDEBUG"))
        std::printf("projectile %d impact: pos0 %.1f %.1f %.1f (%d) pos1 %.1f %.1f %.1f (%d) dist %.2f kind %d at %.1f %.1f %.1f\n", palo->oid,
                    pbest->axpd[0].pos.x, pbest->axpd[0].pos.y, pbest->axpd[0].pos.z, pbest->axpd[0].palo ? pbest->axpd[0].palo->oid : -1,
                    pbest->axpd[1].pos.x, pbest->axpd[1].pos.y, pbest->axpd[1].pos.z, pbest->axpd[1].palo ? pbest->axpd[1].palo->oid : -1,
                    pbest->gDist, pbest->xpk, pos.x, pos.y, pos.z);
    RecordHit(palo, pbest->axpd[0].pos, Normal(VelAloPos(palo), glm::vec3(1.0f, 0.0f, 0.0f)));
    ExpireIn(palo, 0.0f);
}

// orig: 0x0013B2F0 (vt slot 75): a contact accepted (not of kind 1) with the bit 8: its impact there, the end within
// 0.25 s for another thrown object's, else now
bool FAcceptContactProjectile(ALO* palo, CTMSG* pctm)
{
    if (UField(palo, 0x150, 0) == 1) return false;
    const bool r = pctm->grfAccepted != 0;  // 0x1F7588
    if (r && (pctm->grfAccepted & 8))
    {
        RecordHit(palo, PosAloCenter(palo), Normal(pctm->vec, AXIS_Z));
        ExpireIn(palo, pctm->ploSender && FCidDerivedFrom(pctm->ploSender->Cid(), 0x4F) ? 0.25f : 0.0f);
    }
    return r;
}

// orig: 0x0013BB80 (vt slot 26: a pool instance taken (0x1CB250 / 0x1CAEC8), after the ALO's 0x1CB518 (the proxy's
// mask: AttachProxy)): no impact, no end time (D_002A4B20 FLT_MAX), its own thrower, the linear / angular constraints
// free (0x1F5720 / 0x1F57C0 (0)), its contacts on (0x1F6860 (0)), +0x1E8 0, out of its parent keeping its place,
// rotation and velocities (0x1A58D0 (0, 0xF)). E.g. the RC chopper's bombs (proxy 50744 -> 50746): without it they
// kept the template's fixed constraints, fell without gravity and passed through the ground.
void ResetProjectileInstance(LO* plo, LO*)
{
    ALO* palo = static_cast<ALO*>(plo);
    PROJ& p = Proj(palo);
    p.fHit = 0;
    p.tEnd = FLT_MAX;
    SetProjectileLauncher(palo, palo);
    SetAloCnstrLin(palo, 0);
    SetAloCnstrAng(palo, 0);
    SetAloContactsOff(palo, false);
    plo->SetField(0, 0x1E8, 0);
    using PFN = void (*)(LO*, ALO*, XF*, int);
    if (PFN pfn = PfnVt<PFN>(plo, 10)) pfn(plo, nullptr, nullptr, 0xF);
}
NATIVEFN(0x13BB80, ResetProjectileInstance);

struct INITPROJECTILE
{
    INITPROJECTILE()
    {
        RegisterLoUpdate(0x13AB58, UpdateProjectile);
        // orig: 0x00104C70 (EOPID 492's thunk): 0x13B5C8, the flight ends within the script's time (e.g. the
        // Paris rats' TNT 19874: its fuse; without it the TNT never went off and kept rolling around)
        RegisterObjectMethod(0x104C70, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), 0x4F)) ExpireIn(static_cast<ALO*>(plo), GArg(a, 0));
            return CRef(TAGK_Void);
        });
        // orig: 0x00104CF8 (EOPID 390, option 493's thunk): 0x13B860, the end now (the blast's hits, its event 5), e.g.
        // the Ep3 TNT barrel's charge 18407 (cid 0x4F): the barrel's fuse clip (ASEG 98049's start event: block 95845
        // module 2) sets it off; its event 5 (module 3) then has the dispenser 95844 drop the next barrel (1311)
        RegisterObjectMethod(0x104CF8, [](LO* plo, const std::vector<CRef>&) {
            if (FCidDerivedFrom(plo->Cid(), 0x4F)) EndProjectile(static_cast<ALO*>(plo));
            return CRef(TAGK_Void);
        });
    }
} s_initprojectile;
}  // namespace

// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetProjectiles() { MpProj().clear(); }

// orig: 0x0013BC60: the launcher that threw it (+0x200; its overlaps retested when it changes, 0x134320)
void SetProjectileLauncher(ALO* palo, LO* plaunch)
{
    PROJ& p = Proj(palo);
    if (p.plaunch == plaunch) return;
    p.plaunch = plaunch;
    RecalcSwOxfFilterForObject(palo);
}

glm::vec3 ProjectCnstr(const CNSTRS& c, const XF* pxfOwner, const glm::vec3& v);  // solve.cpp (0x1FA810)
namespace
{
// orig: 0x001B20B8 (class 0x51 ACCMISS, vt slot 70 in place of the ALO's gravity 0x1F9B20): while not stuck to
// anything (+0x1E8; natively never: kind 1's sticking is not ported), its speed smoothed toward svTarget (+0x270,
// 0x1B2070's 2000) at svFast (+0x274, 5000) along its current direction, the change as an acceleration through its
// linear constraint (0x1FA810 / 0x1FAC30). No gravity: Bentley's darts fly straight.
void UpdateAccmissAccel(ALO* palo)
{
    SOST* ps = Psost(palo);
    if (!ps) return;
    const glm::vec3 vel = VelAloPos(palo);  // vt slot 33
    const float sv = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    const glm::vec3 dir = vel * (1.0f / sv);
    const cmm::SMP smp{GField(palo, 0x274, 5000.0f), 0.0f, 1.0f};
    const float svNew = cmm::GSmooth(sv, GField(palo, 0x270, 2000.0f), g_clock.dt, &smp, nullptr);
    const glm::vec3 dv = (dir * svNew - vel) * (1.0f / g_clock.dt);
    AccelAloPos(palo, ProjectCnstr(ps->cnstrLin, PxfPos(palo), dv));
    if (std::getenv("SLY2N_DARTPRINT"))
    {
        const glm::vec3 pos = PosAloCenter(palo);
        std::fprintf(stderr, "DART %d t %.3f pos %.1f %.1f %.1f speed %.1f -> %.1f\n", palo->oid, g_clock.t, pos.x, pos.y, pos.z, sv, svNew);
    }
}
}  // namespace
NATIVEFN(0x1B20B8, UpdateAccmissAccel);

int OverlapKindAlo(ALO* pa, ALO* pb);  // bbmark.cpp: 0x1F4FC8
namespace
{
// orig: 0x0013BBF8 (class 0x4F, vt slot 61): the base overlap kind, at least 1 (no contact response) with anything
// under the same root as the launcher that threw it (+0x200): the TNT leaves the rat's hand instead of bouncing off it
int OverlapKindProjectile(ALO* pa, ALO* pb)
{
    int r = OverlapKindAlo(pa, pb);
    const LO* plaunch = Proj(pa).plaunch;
    if (plaunch && PloRoot(pb) == PloRoot(const_cast<LO*>(plaunch))) r = std::max(r, 1);  // 0x134310
    return r;
}
// orig: 0x001B1F40 (classes 0x50..0x52, vt slot 61): 0x13BBF8, and at least 1 with the objects of the oid list
// (+0x238 count, +0x23C) and of the class list (+0x24C count, +0x250)
int OverlapKindMissile(ALO* pa, ALO* pb)
{
    int r = OverlapKindProjectile(pa, pb);
    const int coid = std::min(4, static_cast<int>(UField(pa, 0x238, 0)));
    for (int i = 0; i < coid; ++i)
        if (static_cast<int>(UField(pa, 0x23C + 4 * i, static_cast<uint32_t>(-1))) == pb->oid) r = std::max(r, 1);
    const int ccid = std::min(4, static_cast<int>(UField(pa, 0x24C, 0)));
    for (int i = 0; i < ccid; ++i)
        if (FCidDerivedFrom(pb->Cid(), static_cast<CID>(UField(pa, 0x250 + 4 * i, 0)))) r = std::max(r, 1);
    return r;
}
}  // namespace
NATIVEFN(0x13BBF8, OverlapKindProjectile);
NATIVEFN(0x1B1F40, OverlapKindMissile);
NATIVEFN(0x13AEB0, ProjectileContacts);
NATIVEFN(0x13B2F0, FAcceptContactProjectile);

// (the actors code, for Bentley's darts: 0x1357C0) the flight's end within g (0x13B5C8) and the contact kind its end
// sends
void ExpireProjectileIn(ALO* palo, float g) { ExpireIn(palo, g); }
void SetProjectileFilter(ALO* palo, uint32_t grf) { Proj(palo).grfFilterSet = grf; }
