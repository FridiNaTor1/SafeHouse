// projectile.cpp: the thrown objects (class 0x4F and its variants 0x50 / 0x51 / 0x52: the guards' strikes, Bentley's
// darts): their contacts end the flight, and the end hits the objects near the impact with a contact message (the
// receiver's slot 75: e.g. Sly's damage controller). Part of the AI code. Readable C: sly2-decomp/src/hand
// (func_0013AEB0.c, func_0013B428.c, func_0013B5C8.c, func_0013AB58.c, func_0013B2F0.c, func_0013B5E8.c,
// build/hand_wip/func_0013B860.cpp).

#include "projectile.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "pccharacter.h"   // (PC addition, debug menu only: Carmelita's shot)

#include "activate.h"
#include "ainoise.h"
#include "animfind.h"
#include "asega.h"
#include "clock.h"
#include "emitter.h"
#include "guard.h"
#include "jt.h"
#include "lo.h"
#include "lox.h"
#include "objmethod.h"
#include "po.h"
#include "so.h"
#include "spliceobj.h"
#include "step.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "xf.h"
#include "xfphys.h"
#include "vecmat.h"

glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)
void EvalHermiteVector(const glm::vec4& p0, const glm::vec4& v0, const glm::vec4& p1, const glm::vec4& v1,
                      glm::vec4* ppos, glm::vec4* pvel, glm::vec4* paccel, float dt, float t, float dtScale);  // 0x117120

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
    struct SWEEP
    {
        glm::vec3 pos{0.0f}, normal{0.0f};
        float distance = 0.0f;
        ALO* owner = nullptr;
    };
    float tEnd = FLT_MAX;          // +0x154: when the flight ends (0x13B5C8; D_002A4B1C after the end)
    int fHit = 0;                  // +0x174: the impact recorded (0x13B428)
    glm::vec3 posHit{0.0f};        // +0x180
    glm::mat3 matHit{1.0f};        // +0x190
    glm::vec3 vHit{0.0f}, wHit{0.0f};  // +0x1C0 / +0x1D0
    LO* plaunch = nullptr;         // +0x200: the launcher that threw it (0x13BC60)
    uint32_t grfFilterSet = 0;     // (the thrower's contact kind: Bentley's dart 0x80000 (0x1357C0's +0xE4 +0x18); 0: +0x1EC)
    LO* target = nullptr;          // missile +0x210
    glm::vec3 targetPoint{0.0f};   // +0x220, local to target (world point without a target)
    ASEGA* flightClip = nullptr;   // +0x234, held reference
    CTRL* motionController = nullptr;  // +0x268
    float arrivalTime = 0.0f;      // homing missile +0x278
    ALO* attachment = nullptr;    // +0x1E8, the entry owner to which a sticky projectile attached
    LO* owner = nullptr;          // +0x168, when assigned by a script
    bool ownerAssigned = false;
    std::vector<SWEEP> sweep;     // +0x1F8 / +0x1FC, at most four authored sweep entries
};
std::unordered_map<const LO*, PROJ>& MpProj()
{
    static std::unordered_map<const LO*, PROJ> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
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

LO* ProjectileOwner(LO* plo)
{
    PROJ& p = Proj(plo);
    if (p.ownerAssigned) return p.owner;
    const OID oid = static_cast<OID>(UField(plo, 0x168, 0xFFFFFFFFu));
    if (oid == -1 || oid == 0) return nullptr;
    // Loaded object options resolve in the projectile's instance namespace.
    if (g_splicegame.pfnFindObject)
        return dynamic_cast<LO*>(g_splicegame.pfnFindObject(4, oid, plo, nullptr));
    return PloFindSwNearest(plo, oid);
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
            if (!FPcCarmShotHits(palo, po)) continue;   // (PC addition, debug menu only: Carmelita's shot spares her player)
            if (!psoe->bsp.abspn.empty())
            {
                const float rTest = FCidDerivedFrom(PloRoot(po)->Cid(), S3CID(0x34)) ? r160 : r15C;
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
// velocities back; its owner's burst (+0x168's slot 51, flags 9); a noise (709) at the impact by the thrower
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
    if (LO* owner = ProjectileOwner(palo))
    {
        // orig: 0x0013B860. The first word is BURSTPARAMS flags (source | density), not a listener message.
        BURSTPARAMS burst;
        burst.grf = 9;
        burst.plo = palo;
        burst.g8 = GField(palo, 0x16C, 0.0f);
        burst.gC = GField(palo, 0x15C, 200.0f);
        const int before = g_particlestats.cburst;
        ExplodeLo(owner, &burst);
        if (std::getenv("SLY2N_PROJECTILEAUDIT"))
            std::printf("PROJECTILE_OWNER t=%.6f source=%d clone=%d owner=%d flags=9 density=%.6f radius=%.6f bursts=%d\n",
                        g_clock.t, palo->oid, palo->iclone, owner->oid, burst.g8, burst.gC, g_particlestats.cburst - before);
    }
    if (UField(palo, 0x1E0, 1))
    {
        // 0x156698(SW +0x402C (the fixed noise 709), its place, the thrower: the launcher's owner when it is a class-0x70)
        LO* plo = p.plaunch ? p.plaunch : palo;
        if (p.plaunch && p.plaunch != palo && FCidDerivedFrom(p.plaunch->Cid(), S3CID(0x70)) && p.plaunch->paloParent)
            plo = p.plaunch->paloParent;
        if (NOISE* pn = PnoiseNew(PtmplNoiseFixed(3), PosAloCenter(palo), plo)) pn->nKind = 1;
    }
    RemoveLo(g_psw, palo);  // (vt slot 9)
    p.fHit = 0;
    p.tEnd = FLT_MAX;
    HandleLoSpliceEvent(palo, 5, false, 0, nullptr);  // (vt slot 3: event 5)
}

// orig: 0x0013AC18 (up to four enabled point descriptions whose parent attachments meet the flight segment)
void SweepProjectile(ALO* palo)
{
    PROJ& p = Proj(palo);
    OXA* own = Poxa(palo);
    if (!own || palo->asoe.empty()) return;
    for (const PROJ::SWEEP& hit : p.sweep)
        if (OXA* other = Poxa(hit.owner)) UpdateSwPox(own, other, 2);
    p.sweep.clear();
    for (LO* point : AploJtList(g_psw, JTL_Point))
    {
        if (!FLoActive(point) || !UField(point, 0xF0, 0) || !(UField(point, 0xE8, 0) & 0x100)) continue;
        ALO* owner = nullptr;
        for (ALO* parent = point->paloParent; parent; parent = parent->paloParent)
            if (OXA* other = Poxa(parent)) { owner = other->pso; break; }
        if (!owner || !owner->fInWorld) continue;
        OXA* other = Poxa(owner);
        LSG segment;
        if (!ClipSegmentToSphere(PosLoWorld(point), glm::vec3(palo->asoe.front().mat[3]), PosAloCenter(palo),
                                 1, &segment, other->sRadius)) continue;
        UpdateSwPox(own, other, 1);
        const glm::vec3 normal(segment.plane0);
        p.sweep.push_back({segment.pos0, normal, glm::dot(segment.pos0, normal), owner});
        if (p.sweep.size() == 4) break;
    }
}

// orig: 0x0013ADE8 (slot 60; its base 0x1F5990 is empty, and the other tree arguments are unused here)
void MarkProjectileSweep(ALO* palo, SOE* entry, int, const BSPREF&, int, XP** contacts)
{
    if (!UField(palo, 0x1F4, 0) || !entry || !entry->palo || palo->asoe.empty()) return;
    OXA* other = Poxa(entry->palo);
    for (const PROJ::SWEEP& hit : Proj(palo).sweep)
        if (other && hit.owner == other->pso)
        {
            PxpAddSw(&palo->asoe.front(), entry, hit.pos, hit.normal, 9, nullptr, nullptr, contacts, hit.distance);
            return;
        }
}

// orig: 0x0013AB58 (vt slot 16): ALO update, expiry/slow-motion completion, otherwise the optional sweep.
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
    else if (UField(palo, 0x1F4, 0)) SweepProjectile(palo);
}

// orig: 0x0013B1C8 (excluded attachment classes and their descendants, through the candidate's ancestor chain)
bool FStickyProjectileParent(ALO* parent)
{
    for (LO* p = parent; p; p = p->paloParent)
        for (CID cid : {0x33, 0x64, 0x4F, 0x67})  // D_002BC9D0
            if (FCidDerivedFrom(p->Cid(), cid)) return false;
    return true;
}

// orig: 0x0013B250 (0x149A50's pointer-argument callback, dispatched after contact notifications)
void AttachStickyProjectile(void* object, void* argument)
{
    ALO* palo = static_cast<ALO*>(object);
    ALO* parent = static_cast<ALO*>(argument);
    Proj(palo).attachment = parent;
    using PFN = void (*)(LO*, ALO*, XF*, int);
    if (PFN pfn = PfnVt<PFN>(palo, 10)) pfn(palo, parent->oid != 1 ? parent : nullptr, nullptr, 3);
    SetAloVelocity(palo, glm::vec3(0.0f));
    SetAloAngVelocity(palo, glm::vec3(0.0f));
    SetAloCnstrLin(palo, 1);
    SetAloCnstrAng(palo, 1);
    if (std::getenv("SLY2N_PROJECTILEAUDIT"))
        std::printf("PROJECTILE_ATTACH t=%.6f projectile=%d clone=%d target=%d parent=%d event=27\n",
                    g_clock.t, palo->oid, palo->iclone, parent->oid, palo->paloParent ? palo->paloParent->oid : -1);
    HandleLoSpliceEvent(palo, 27, false, 0, nullptr);
}

// orig: 0x0013AEB0 (nearest eligible contact; kind1 posts attachment, kinds0/2 record impact and expire)
void ProjectileContacts(ALO* palo)
{
    NotifyAloContacts(palo);  // 0x1F4F10
    if (std::getenv("SLY2N_PROJDEBUG")) std::printf("projectile %d contacts at t %.3f\n", palo->oid, g_clock.t);
    const int k = static_cast<int>(UField(palo, 0x150, 0));
    if ((k == 0 || k == 2) && Proj(palo).fHit) return;
    if (k == 1 && (!UField(palo, 0x1E4, 0) || Proj(palo).attachment)) return;
    OXA* poxa = Poxa(palo);
    if (!poxa) return;
    const glm::vec3 pos = palo->asoe.empty() ? PosAloCenter(palo) : glm::vec3(palo->asoe.front().mat[3]);
    float gBest = FLT_MAX;  // (D_002A4B18)
    XP* pbest = nullptr;
    for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
        for (XP* x = ox->pxp; x; x = x->pxpNext)
        {
            const float g = glm::length(x->axpd[0].pos - pos);
            if (gBest <= g) continue;
            bool f = k == 0;
            const XPD& other = x->axpd[x->axpd[0].paloRoot == palo ? 1 : 0];
            if (k == 2)
            {
                // orig: 0x001C4098 -> 0x001C4080 (controller exists and is not in state -1)
                PODATA* player = other.paloRoot ? PpodataFromLo(other.paloRoot) : nullptr;
                f = player && player->ppadctl && player->ppadctl->nControl != -1;
            }
            else if (k == 1) f = other.palo && FStickyProjectileParent(other.palo);
            if (f)
            {
                gBest = g;
                pbest = x;
            }
        }
    if (!pbest) return;
    if (k == 1)
    {
        ALO* parent = pbest->axpd[pbest->axpd[0].paloRoot == palo ? 1 : 0].palo;
        PostDeferredPointerCall(palo, 0x13B250, AttachStickyProjectile, parent);
        return;
    }
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
        ExpireIn(palo, pctm->ploSender && FCidDerivedFrom(pctm->ploSender->Cid(), S3CID(0x4F)) ? 0.25f : 0.0f);
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
    p.attachment = nullptr;
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
        RegisterObjectGetter(394, [](LO* plo) { CRef r; r.SetBasic(ProjectileOwner(plo)); return r; });
        RegisterObjectSetter(394, [](LO* plo, const CRef& r) {
            PROJ& p = Proj(plo);
            p.owner = dynamic_cast<LO*>(r.m_pbasic);
            p.ownerAssigned = true;
        });
        // EOPIDs 400/401 expose the same target/point fields that slot 78 writes (original +0x210/+0x220).
        RegisterObjectGetter(400, [](LO* plo) { CRef r; r.SetBasic(Proj(plo).target); return r; });
        RegisterObjectSetter(400, [](LO* plo, const CRef& r) { Proj(plo).target = dynamic_cast<LO*>(r.m_pbasic); });
        RegisterObjectGetter(401, [](LO* plo) { CRef r; r.SetVector(Proj(plo).targetPoint); return r; });
        RegisterObjectSetter(401, [](LO* plo, const CRef& r) { Proj(plo).targetPoint = r.GetVector(); });
        // orig: 0x00104E80 (EOPID 405): target ALO (or nil), local point, void result; missile slot 78.
        RegisterObjectMethod(0x104E80, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x50)) && a.size() >= 2)
            {
                using PFN = void (*)(ALO*, LO*, const glm::vec3&);
                if (PFN pfn = PfnVt<PFN>(plo, 78)) pfn(static_cast<ALO*>(plo), PloArg(a, 0), a[1].GetVector());
            }
            return CRef(TAGK_Void);
        });
        // orig: 0x00104C70 (EOPID 492's thunk): 0x13B5C8, the flight ends within the script's time (e.g. the
        // Paris rats' TNT 19874: its fuse; without it the TNT never went off and kept rolling around)
        RegisterObjectMethod(0x104C70, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x4F))) ExpireIn(static_cast<ALO*>(plo), GArg(a, 0));
            return CRef(TAGK_Void);
        });
        // orig: 0x00104CF8 (EOPID 390, option 493's thunk): 0x13B860, the end now (the blast's hits, its event 5), e.g.
        // the Ep3 TNT barrel's charge 18407 (cid 0x4F): the barrel's fuse clip (ASEG 98049's start event: block 95845
        // module 2) sets it off; its event 5 (module 3) then has the dispenser 95844 drop the next barrel (1311)
        RegisterObjectMethod(0x104CF8, [](LO* plo, const std::vector<CRef>&) {
            if (std::getenv("SLY2N_PROJEND")) std::printf("projend method t %.3f %d inworld %d\n", g_clock.t, plo->oid, int(plo->fInWorld));
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x4F))) EndProjectile(static_cast<ALO*>(plo));
            return CRef(TAGK_Void);
        });
    }
} s_initprojectile;
}  // namespace

// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetProjectiles() { MpProj().clear(); }

// orig: 0x001B2398; 0x001D6B00 uses the same slot-32 frame/local matrix and 0x2309E8 prediction.
glm::vec3 PredictProjectileAim(LO* target, const glm::vec3& point, float dt)
{
    if (!target) return point;
    XF* frame = nullptr;
    glm::mat4 local(1.0f);
    if (FCidDerivedFrom(target->Cid(), S3CID(0x30))) frame = PxfObj(static_cast<ALO*>(target));  // 0x1F3FB0
    else if (const LOX* located = dynamic_cast<const LOX*>(target))  // 0x231550
    {
        frame = located->pxf;
        local = located->mat;
    }
    glm::mat4 predicted(1.0f);
    PredictXfChain(frame, nullptr, dt, &predicted, nullptr);
    return glm::vec3(predicted * (local * glm::vec4(point, 1.0f)));
}

// orig: 0x001B1E38 (class 0x50's slot 12): base removal has already notified its listeners.
void OnRemoveMissile(LO* plo)
{
    const auto it = MpProj().find(plo);
    if (it == MpProj().end()) return;
    PROJ& p = it->second;
    ApplyAsegCur(nullptr, static_cast<ALO*>(plo), 0.0f, 1.0f, 0, &p.flightClip);
    DeleteCtrl(&p.motionController);
    if (std::getenv("SLY2N_HOMINGTRACE"))
    {
        int motionControllers = 0;
        if (const XF* rotation = PxfRot(static_cast<ALO*>(plo)))
            for (const CTRL* ctrl = rotation->pctrlFirst; ctrl; ctrl = ctrl->pnext)
                if (ctrl->kind == 4) ++motionControllers;
        std::printf("homing remove t %.6f oid %d clone %d clip %d ctrl %d remaining_motion %d\n", g_clock.t,
                    plo->oid, plo->iclone, p.flightClip != nullptr, p.motionController != nullptr, motionControllers);
    }
}

namespace
{
// orig: 0x001B1E88 (class 0x50's slot 78): retain the target/point, apply its looping flight clip,
// and look along the motion with the authored lead parameter (0x12DCA0).
void SetMissileTarget(ALO* palo, LO* target, const glm::vec3& point)
{
    PROJ& p = Proj(palo);
    p.target = target;
    p.targetPoint = point;
    ASEG* clip = nullptr;
    for (const auto& part : palo->apart)
        if (part.first == 0x230) clip = dynamic_cast<ASEG*>(part.second);
    ApplyAsegCur(clip, palo, 0.0f, 1.0f, 8, &p.flightClip);
    if (UField(palo, 0x264, 1))
    {
        auto* ctrl = new CTRLLOOKMOTION();
        ctrl->kind = 4;
        ctrl->palo = palo;
        ctrl->pxf = PxfRot(palo);
        ctrl->prio = 0;
        ctrl->mode = 5;
        ctrl->job.kind = 4;
        ctrl->job.gLo = GField(palo, 0x26C, 1.0f);
        ctrl->job.gHi = 0.0f;
        AddXfCtrl(palo, ctrl->pxf, ctrl);
        p.motionController = ctrl;
    }
}

// orig: 0x001B2228 (class 0x52's slot 78): an authored duration or a bounded arrival deadline
// from the predicted destination, current velocity and +0x274's speed limit (default 1500).
void SetHomingMissileTarget(ALO* palo, LO* target, const glm::vec3& point)
{
    SetMissileTarget(palo, target, point);
    PROJ& p = Proj(palo);
    const float duration = GField(palo, 0x270, 0.0f);
    if (duration > 0.0f) p.arrivalTime = g_clock.t + duration;
    else
    {
        const glm::vec3 destination = PredictProjectileAim(p.target, p.targetPoint, 0.5f);
        const float speedLimit = GField(palo, 0x274, 1500.0f);
        glm::vec3 velocity = VelAloPos(palo) * -0.5f;
        const float length = glm::length(velocity);
        if (length > 0.75f * speedLimit) velocity *= (0.75f * speedLimit) / length;  // 0x11E9F8
        const glm::vec3 endpoint = velocity + (destination - PosAloCenter(palo)) * 1.5f;
        LSG segment;
        ClipSegmentToSphere(glm::vec3(0.0f), velocity, endpoint, 1, &segment, speedLimit);
        p.arrivalTime = g_clock.t + 1.0f / std::max(0.25f, segment.t1);
    }
    if (std::getenv("SLY2N_HOMINGTRACE"))
        std::printf("homing setup t %.6f oid %d clone %d target %d point %.6f %.6f %.6f arrival %.6f clip %d ctrl %d\n",
                    g_clock.t, palo->oid, palo->iclone, target ? target->oid : -1, point.x, point.y, point.z,
                    p.arrivalTime, p.flightClip ? p.flightClip->paseg->oid : -1, p.motionController != nullptr);
}

// orig: 0x001B2468 (class 0x52's slot 70): predict at most half a second ahead, evaluate the
// Hermite curve's velocity one frame ahead, then apply that velocity change as an acceleration.
void UpdateHomingMissileAccel(ALO* palo)
{
    if (UField(palo, 0x1E8, 0)) return;
    const PROJ& p = Proj(palo);
    const float remaining = std::max(p.arrivalTime - g_clock.t, 0.0f) + g_clock.dt;
    const glm::vec3 destination = PredictProjectileAim(p.target, p.targetPoint, std::min(remaining, 0.5f));
    glm::vec3 endVelocity(GField(palo, 0x280, 0.0f), GField(palo, 0x284, 0.0f), GField(palo, 0x288, 0.0f));
    const glm::vec3 position = PosAloCenter(palo), velocity = VelAloPos(palo);
    if (std::fabs(endVelocity.x) < 0.0001f && std::fabs(endVelocity.y) < 0.0001f && std::fabs(endVelocity.z) < 0.0001f)
        endVelocity = (destination - position) * (1.5f / remaining) + velocity * -0.5f;
    glm::vec4 nextVelocity;
    EvalHermiteVector(glm::vec4(position, 1.0f), glm::vec4(velocity, 0.0f), glm::vec4(destination, 1.0f),
                      glm::vec4(endVelocity, 0.0f), nullptr, &nextVelocity, nullptr, remaining, g_clock.dt, 1.0f);
    AccelAloPos(palo, (glm::vec3(nextVelocity) - VelAloPos(palo)) * (1.0f / g_clock.dt));
    if (std::getenv("SLY2N_HOMINGTRACE"))
        std::printf("homing step t %.6f oid %d clone %d remaining %.6f destination %.6f %.6f %.6f pos %.6f %.6f %.6f vel %.6f %.6f %.6f next %.6f %.6f %.6f\n",
                    g_clock.t, palo->oid, palo->iclone, remaining, destination.x, destination.y, destination.z,
                    position.x, position.y, position.z, velocity.x, velocity.y, velocity.z,
                    nextVelocity.x, nextVelocity.y, nextVelocity.z);
}
}  // namespace
NATIVEFN(0x1B1E88, SetMissileTarget);
NATIVEFN(0x1B2228, SetHomingMissileTarget);
NATIVEFN(0x1B2468, UpdateHomingMissileAccel);

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
NATIVEFN(0x13ADE8, MarkProjectileSweep);
NATIVEFN(0x13B2F0, FAcceptContactProjectile);

// (the actors code, for Bentley's darts: 0x1357C0) the flight's end within g (0x13B5C8) and the contact kind its end
// sends
void ExpireProjectileIn(ALO* palo, float g) { ExpireIn(palo, g); }
void SetProjectileFilter(ALO* palo, uint32_t grf) { Proj(palo).grfFilterSet = grf; }

// orig: 0x1A6730 (CloneLo copies the object): a copy keeps what the load's options set in the class's state (e.g.
// setter 394, the owner); natively it lives in MpProj, so a copied projectile had none
void CloneProjectileData(const LO* source, LO* target)
{
    auto& mp = MpProj();
    const auto it = mp.find(source);
    if (it != mp.end()) mp[target] = it->second;
}
