// carry.cpp: the carryable objects (cid 0x64 and its 0x65 / 0x66; carry.h). Ported from the decomp's src/hand
// 0x224EC8 (vt slot 76), 0x224258 / 0x224590 / 0x224610 / 0x224638 / 0x224688 and the reference's m_224258.
#include "jt.h"
#include "worldglobals.h"
#include "carry.h"

#include "ainoise.h"
#include "activate.h"
#include "alo.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "lo.h"
#include "so.h"
#include "sw.h"
#include "vtnative.h"
#include "xf.h"
#include "physworld.h"
#include "vecmat.h"
#include "update.h"
#include "objmethod.h"
#include "smrun.h"
#include "sm.h"
#include "actor.h"
#include "sly.h"
#include "spliceobj.h"
#include "xfphys.h"
#include "worldaccess.h"
#include "crv.h"
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)
#include "guard.h"
#include "ai.h"
#include "aijobs.h"
#include "aihit.h"
#include "task.h"

#include <cstdio>
extern int g_iframeHarness;
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <cmath>
#include <limits>


glm::vec3 DvAloImpulse(ALO* p, const glm::vec3& v);                         // solve.cpp (0x1FAA00)
glm::vec3 DwAloImpulseAt(ALO* p, const glm::vec3& a, const glm::vec3& b);   // solve.cpp (0x1FAA48)
namespace
{
// orig 0x001F7530 (the first ALO up the parents with one of the contact bytes f)
ALO* PaloXpFilter(ALO* p, uint32_t f)
{
    for (; p; p = p->paloParent)
        if (GrfAloXpBytes(p, f)) return p;
    return nullptr;
}

void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "carry: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

struct CARRYRT : CARRY
{
    SMA* psma = nullptr;       // +0x168 (0x224028: the SM +0x164's handle)
    bool fSma = false;
    bool fKindLoaded = false;  // +0x15C taken from the loaded field (Pcarry)
    CTRL* pctrlPos = nullptr;  // +0x180 / +0x17C: the records held while carried (0x12DC18)
    CTRL* pctrlRot = nullptr;
    glm::vec3 dirLand{0.0f};   // +0x190 the landing contact's normal, away from what it touched (0x224380)
    // the Health Extractor (cid 0x66)
    LO* ploSource = nullptr;   // +0x1B0 (a LOX point of the extractor, by its oid)
    bool fSource = false;
    LO* ploActor = nullptr;    // +0x1B4 the guard pulled in (0x225660)
};
std::unordered_map<const LO*, CARRYRT> s_mpcarry;
WORLD_TABLE(s_mpcarry);   // (PC: moved aside while a kept world loads, worldglobals.h)

CARRYRT* Pcarry(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x64)) return nullptr;
    CARRYRT& c = s_mpcarry[plo];
    // +0x15C (EOPID 1104, field 0x15C) is loaded with the object: a world-placed carryable (e.g. Ep7 Old Grizzle
    // Face's grenade 33839, kind 5) carries its kind from the data; the gadgets' own objects set it when taken
    // (gang.cpp). Without it 0x224638 (EOPID 1106) reads false and the grenade's SM 532 skips its flight (N31).
    if (!c.fKindLoaded)
    {
        const uint32_t u = plo->U32Field(0, 0x15C, 0xFFFFFFFFu);
        if (u != 0xFFFFFFFFu)
        {
            if (c.kind == 0) c.kind = static_cast<int>(u);
            c.fKindLoaded = true;
        }
    }
    return &c;
}

// The object's world placement kept while it leaves its carrier's record (the proxy's channel)
void DetachFromChannel(ALO* palo)
{
    if (palo->axf.empty() || !palo->axf[0]->pxfRef) return;
    UpdateAloHierarchyWorlds(palo);
    XF* pxf = palo->axf[0].get();
    pxf->mat = pxf->matWorld;
    pxf->pxfRef = nullptr;
    UpdateAloHierarchyWorlds(palo);
}
}  // namespace

CARRY* PcarryFromLo(LO* plo) { return Pcarry(plo); }

const CARRY* PcarryFind(const LO* plo)
{
    const auto found = s_mpcarry.find(plo);
    return found == s_mpcarry.end() ? nullptr : &found->second;
}

const SMA* PsmaCarryFind(const LO* plo)
{
    const auto found = s_mpcarry.find(plo);
    return found == s_mpcarry.end() ? nullptr : found->second.psma;
}

// orig: 0x00224028 OnCarryAdd (called after the base ALO activation callback).
void OnCarryAdd(LO* plo)
{
    CARRYRT* pc = Pcarry(plo);
    if (!pc || pc->fSma) return;
    pc->fSma = true;
    const OID oidSm = static_cast<OID>(plo->U32Field(0, 0x164, static_cast<uint32_t>(-1)));
    for (LO* child : plo->aploOwned)
        if (child->oid == oidSm)
            if (SM* sm = dynamic_cast<SM*>(child))
                ReplaceSmaHandle(sm, static_cast<ALO*>(plo), -1, 0, &pc->psma);
}

// orig: 0x00224EC8 (vt slot 76)
void SetCarryStateBase(LO* plo, int n)
{
    CARRYRT* pc = Pcarry(plo);
    if (!pc) return;
    ALO* palo = static_cast<ALO*>(plo);
    const int old = pc->state;
    if (n == old) return;
    pc->state = n;
    pc->t = g_clock.t;
    pc->n158 = -1;
    DeleteCtrl(&pc->pctrlPos);  // 0x12E460
    DeleteCtrl(&pc->pctrlRot);
    if (old == 0 || old == 4) RecalcSwOxfFilterForObject(palo);  // 0x134320
    XF* pxfPos = PxfPos(palo);
    XF* pxfRot = PxfRot(palo);
    switch (n)
    {
    case 0:
        SetAloCnstrLin(palo, 0);  // 0x1F5720
        SetAloCnstrAng(palo, 0);  // 0x1F57C0
        if (pxfPos) pxfPos->vel = glm::vec4(0.0f);
        if (pxfRot) pxfRot->vel = glm::vec4(0.0f);
        if (pxfPos) pc->pctrlPos = PctrlNewPos(palo, pxfPos, 0, 7, 2, VecXfValue(pxfPos, pxfPos->mat));  // 0x12DC18
        if (pxfRot) pc->pctrlRot = PctrlNewPos(palo, pxfRot, 0, 7, 2, VecXfValue(pxfRot, pxfRot->mat));
        RecalcSwOxfFilterForObject(palo);
        break;
    case 2:
        SetAloCnstrLin(palo, 1);
        SetAloCnstrAng(palo, 1);
        if (pxfPos) pxfPos->vel = glm::vec4(0.0f);
        if (pxfRot) pxfRot->vel = glm::vec4(0.0f);
        if (pc->kind == 0xB && pxfRot)  // 0x224688
        {
            // the rotation held facing the landing normal: x along it, y from up (0x11B830), in the record's parent
            const glm::vec3 dir = glm::normalize(VecConvertXfs(nullptr, pxfRot->pxfRef, pc->dirLand));  // 0x22FCC8
            const glm::vec3 up = VecConvertXfs(nullptr, pxfRot->pxfRef, glm::vec3(0.0f, 0.0f, 1.0f));     // D_002A2DC0
            const glm::mat3 m = BuildLookAtMatrix(dir, glm::normalize(up));
            pc->pctrlRot = PctrlNewPos(palo, pxfRot, 0, 7, 2, VecXfValue(pxfRot, glm::mat4(m)));  // 0x11BFF0 / 0x12DC18
        }
        break;
    case 1:
        SetLoParentActivation(g_psw, plo, nullptr);  // 0x1A58D0(p, 0, 15)
        DetachFromChannel(palo);
        SetAloCnstrLin(palo, 0);
        if (pc->kind != 0x29) SetAloCnstrAng(palo, 0);
        else
        {
            SetAloCnstrAng(palo, 1);
            // orig: 0x00224EC8: query at most 32 guards in class-list order.
            // The fallback is the global Sly (0x2E1E40), not the selected player.
            BASIC* candidates[32]{};
            const int count = g_splicegame.pfnFindObjectsByClass
                ? g_splicegame.pfnFindObjectsByClass(0x105, 0x38, nullptr, 32, candidates) : 0;
            SLY* sly = PslyGlobal();
            LO* nearest = sly ? sly->palo : nullptr;
            float best = std::numeric_limits<float>::max();  // D_002A97A8
            for (int i = 0; i < count; ++i)
            {
                LO* candidate = static_cast<LO*>(candidates[i]);
                glm::vec3 delta = PosLoWorld(candidate) - PosLoWorld(plo);
                delta.z += delta.z;
                const float distance = std::sqrt((delta.x * delta.x + delta.y * delta.y) + delta.z * delta.z);
                if (distance < best)
                {
                    best = distance;
                    nearest = candidate;
                }
            }
            pc->plo160 = nearest;
        }
        break;
    case 4:
        RecalcSwOxfFilterForObject(palo);
        SwitchLoFaded(palo, false, 0.25f);  // orig 0x1F7BF8
        break;
    default: break;
    }
    // +0x168's goal D_002C4398[state] (0x1F0730; the SM made by the OnAdd callback 0x224028)
    // Offline component loaders can enter a state without the gameplay activation pass.
    if (!pc->fSma) OnCarryAdd(plo);
    if (pc->psma && n >= 0 && n < 6) SeekSma(pc->psma, 109 + n);  // 0x1F0730 (D_002C4398: 0x6D..0x72)
    if (std::getenv("SLY2N_CARRYDEBUG")) std::printf("carry %d: sma %p goal %d\n", plo->oid, static_cast<void*>(pc->psma), 109 + n);
    if (std::getenv("SLY2N_CARRYDEBUG") && n == 0)
    {
        for (const auto& [k, u] : plo->afield) std::printf("  field getter 0x%X +0x%X = 0x%X\n", unsigned(k >> 32), unsigned(k & 0xFFFFFFFF), u);
        for (const OPT& o : plo->aopt) std::printf("  option eopid %d\n", o.eopid);
        for (const auto& [ib, pl] : palo->apart) std::printf("  part +0x%X oid %d cid 0x%X\n", ib, pl->oid, pl->Cid());
    }
    if (std::getenv("SLY2N_CARRYDEBUG"))
        std::printf("carry %d (cid 0x%X): t %.3f state %d -> %d (iupd %d active %d st %d parent %d inworld %d removed %d)\n", plo->oid, plo->Cid(), g_clock.t, old, n,
                    plo->iupd, PloRoot(plo)->fNodeActive, plo->st, plo->paloParent ? plo->paloParent->oid : -1, plo->fInWorld, plo->fRemoved);
}

namespace { void UpdateCarry(LO* plo); }
// ---- the Health Extractor (cid 0x66: vt slots 16 0x225300, 71 0x2255C8, 76 0x225660) ----
void LinkAloHook(ALO* powner, ALO* po);    // water.cpp (0x229710)
void UnlinkAloHook(ALO* powner, ALO* po);  // (0x229738)
extern void (*g_pfnHealthExtractorHook)(ALO* powner, ALO* po);
void SpawnPickups(int cid, const glm::vec3& pos, LO* psrc, int n);  // pickup.cpp (0x1C6FD8)
namespace
{
bool FExtractor(const LO* plo) { return plo->pcls && plo->pcls->PfnOrig(76) == 0x225660; }
// +0x1B0 (the loader's oid: a part of the extractor), else the extractor
LO* PloExtractorSource(LO* plo, CARRYRT* pc)
{
    if (!pc->fSource)
    {
        pc->fSource = true;
        // the loader keeps the field as an oid: the object of that oid among the extractor's descendants
        const OID oid = static_cast<OID>(plo->U32Field(0, 0x1B0, static_cast<uint32_t>(-1)));
        LO* q = nullptr;
        std::vector<LO*> a{plo};
        for (size_t i = 0; i < a.size() && !q; ++i)
        {
            if (i && a[i]->oid == oid) q = a[i];
            for (LO* r : a[i]->aploOwned) a.push_back(r);
            if (FCidDerivedFrom(a[i]->Cid(), 0x30))
                for (LO* r : static_cast<ALO*>(a[i])->aploChild) a.push_back(r);
            if (a.size() > 4096) break;
        }
        pc->ploSource = q;
        if (std::getenv("SLY2N_CARRYDEBUG")) std::printf("extractor %d: source %d\n", plo->oid, q ? q->oid : -1);
    }
    return pc->ploSource ? pc->ploSource : plo;
}
}

// orig: 0x00225660 (vt slot 76): leaving 5, the guard let go (its node 0x229738, its 0xB0 job to 2, its scale back
// to D_002A2DE0 (1, 1, 1)); the base's 0x224EC8; entering 5, the nearest guard within 400 of the source that may
// take mode 11 (0x206EB0) pulled in: its node (0x229710), half its velocity with z D_002C43F4 400 (slots 33 / 35)
// and the reaction 6 (0x209E28)
void SetExtractorState(LO* plo, int n)
{
    CARRYRT* pc = Pcarry(plo);
    if (!pc || n == pc->state) return;
    if (pc->state == 5 && pc->ploActor)
    {
        UnlinkAloHook(static_cast<ALO*>(plo), static_cast<ALO*>(pc->ploActor));  // 0x229738
        if (GUARD* pg = PguardFromLo(pc->ploActor))
        {
            if (AIJOB* pj = PjobFind(pg, 0xB0)) pj->SetState(2);  // 0x17A8C8
            if (pg->pscaleFC4) SetCtrlPos(pg->pscaleFC4, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));  // 0x12D108
        }
        pc->ploActor = nullptr;
    }
    SetCarryStateBase(plo, n);
    if (n != 5) return;
    const glm::vec3 pos = PosLoWorld(PloExtractorSource(plo, pc));
    BASIC* apo[32]{};
    const int c = g_splicegame.pfnFindObjectsByClass ? g_splicegame.pfnFindObjectsByClass(0x105, 0x38, nullptr, 32, apo) : 0;
    float gBest = 400.0f;
    GUARD* pgBest = nullptr;
    for (int i = 0; i < c; ++i)
    {
        LO* po = static_cast<LO*>(apo[i]);
        GUARD* pg = PguardFromLo(po);
        if (!pg || !FGuardModeAllowed(pg, 0xB)) continue;  // 0x206EB0
        const float g = glm::length(PosLoWorld(po) - pos);
        if (g < gBest)
        {
            gBest = g;
            pgBest = pg;
        }
    }
    if (!pgBest) return;
    pc->ploActor = pgBest->palo;
    LinkAloHook(static_cast<ALO*>(plo), pgBest->palo);  // 0x229710 (+0x1B8)
    glm::vec3 v = VelAloPos(pgBest->palo) * 0.5f;      // slot 33 x D_002C43F0
    v.z = 400.0f;                                      // D_002C43F4
    SetAloVelocityTo(pgBest->palo, v);                 // slot 35
    ReactGuardHeld(pgBest->palo, nullptr);  // 0x209E28(guard, 0)
    if (std::getenv("SLY2N_CARRYDEBUG")) std::printf("extractor %d: guard %d pulled in (%.0f away)\n", plo->oid, pgBest->palo->oid, gBest);
}

// orig: 0x002255C8 (vt slot 71, from the guard's gravity 0x229798): its gravity cancelled (0x1FAC30 with -(+0xE8's
// +0x80)), pulled toward the source with D_002C43D0 {0, 15, 0} and its velocity toward 0 with D_002C43E0 {0, 2.5, 0}
void ApplyExtractorHook(ALO* powner, ALO* po)
{
    CARRYRT* pc = Pcarry(powner);
    if (!pc) return;
    if (const SOST* ps = Psost(po)) AccelAloPos(po, -ps->dvGravity);
    static const CLQ s_clqPos{0.0f, 15.0f, 0.0f, 0.0f};
    static const CLQ s_clqVel{0.0f, 2.5f, 0.0f, 0.0f};
    AccelAloTowardPosition(po, PosLoWorld(PloExtractorSource(powner, pc)), &s_clqPos, glm::vec3(0.0f), &s_clqVel);  // 0x1F5E10
}

// orig: 0x00225300 (vt slot 16): the base's update; in 5: no guard -> 3; its 0xB0 job gone or over -> 2; else its
// heading the source's basis (0x209840), the job to 4 within 100 of the source, its scale 1.2 - u^2 (u: the time in
// 5 over D_002C43B0 3 s, limited to 0..1); after 3 s a health charm (0x5D) at the source (0x1C6FD8), the guard removed
// (slot 9), the task event 0x18 with it and 3
void UpdateExtractor(LO* plo)
{
    UpdateCarry(plo);  // 0x2240B0
    CARRYRT* pc = Pcarry(plo);
    if (!pc) return;
    int old;
    do
    {
        old = pc->state;
        if (old != 5) continue;
        GUARD* pg = pc->ploActor ? PguardFromLo(pc->ploActor) : nullptr;
        AIJOB* pj = pg ? PjobFind(pg, 0xB0) : nullptr;
        if (!pc->ploActor) SetCarryState(plo, 3);
        else if (!pj || static_cast<JOBB0*>(pj)->FDone()) SetCarryState(plo, 2);  // 0x17A9C0
        else
        {
            LO* psrc = PloExtractorSource(plo, pc);
            if (!pg->move.fHeadingLocked)  // 0x209840: the basis rows into the guard's heading
            {
                // 0x1A6318 (vt slot 32: the world matrix; a LOX's record x its own)
                const glm::mat4 m = MatJtLo(psrc);
                SetStepPhysHeading(pg->palo, std::atan2(m[0][1], m[0][0]));
            }
            JOBB0* pb = static_cast<JOBB0*>(pj);
            if (std::getenv("SLY2N_CARRYDEBUG"))
            {
                const glm::vec3 a = PosLoWorld(psrc), b = PosLoWorld(pg->palo), c = PxfPos(pg->palo) ? glm::vec3(PxfPos(pg->palo)->matWorld[3]) : glm::vec3(0.0f);
                std::printf("extractor %d: t %.3f source %d (%.1f %.1f %.1f) guard %d (%.1f %.1f %.1f) record (%.1f %.1f %.1f) job %d\n", plo->oid, g_clock.t,
                            psrc->oid, a.x, a.y, a.z, pg->palo->oid, b.x, b.y, b.z, c.x, c.y, c.z, pb->stHeld);
            }
            if (pb->stHeld != 4 && glm::length(PosLoWorld(psrc) - PosLoWorld(pg->palo)) < 100.0f) pb->SetState(4);
            if (pg->pscaleFC4)
            {
                const float u = (g_clock.t - pc->t) / 3.0f;
                float g = 1.2f + u * (0.0f + u * -1.0f);
                g = g < 0.0f ? 0.0f : (1.0f < g ? 1.0f : g);
                SetCtrlPos(pg->pscaleFC4, glm::vec4(g, g, g, g));
            }
            if (3.0f < g_clock.t - pc->t)
            {
                SpawnPickups(0x5D, PosLoWorld(psrc), plo, 1);  // 0x1C6FD8
                LO* ploGuard = pg->palo;
                RemoveLo(g_psw, ploGuard);  // slot 9
                BroadcastTaskEvent(g_psw, 0x18, ploGuard);  // 0x1619A0
                SetCarryState(plo, 3);
            }
        }
    } while (pc->state != old);
}

void SetCarryState(LO* plo, int n)
{
    if (plo && FExtractor(plo)) SetExtractorState(plo, n);
    else SetCarryStateBase(plo, n);
}

// orig: 0x00224068 OnCarryRemove (called after the base ALO removal callback).
void OnCarryRemove(LO* plo)
{
    CARRYRT* pc = Pcarry(plo);
    if (!pc) return;
    SetCarryState(plo, -1);
    ClearSmaHandle(&pc->psma);
    pc->fSma = false;
}

void ResetCarries() { s_mpcarry.clear(); }

// orig: 0x00224258 (a dropped object that touched q: free (2), on q unless the world; kinds 0x1A / 0x29: 3)
void LandCarry(LO* plo, LO* q)
{
    CARRYRT* pc = Pcarry(plo);
    if (!pc) return;
    if (pc->kind == 0x1A || pc->kind == 0x29) { SetCarryState(plo, 3); return; }
    SetAloParent(static_cast<ALO*>(plo),
                 (q && q->oid != 1 && FCidDerivedFrom(q->Cid(), 0x30)) ? static_cast<ALO*>(q) : nullptr,
                 nullptr, 3);  // 0x1A58D0(p, q, 3): preserve world position and rotation.
    SetCarryState(plo, 2);
}

namespace
{
// orig: 0x00224638 (may it land on what it touches: by its kind)
bool FCarryLands(const CARRYRT& c)
{
    switch (c.kind)
    {
    case 4: return c.state == 1;
    case 5: case 7: case 9: case 11: case 13: case 26: case 37: case 41: return true;
    default: return false;
    }
}

// orig: 0x002240B0 (vt slot 16: the ALO's update; the dropped one lands; the carrier's range (slot 75 (0)))
void UpdateCarry(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // 0x1F36E0
    CARRYRT* pc = Pcarry(plo);
    if (!pc) return;
    if (std::getenv("SLY2N_CARRYDEBUG") && pc->state >= 1)
    {
        const glm::vec3 v = PosLoWorld(plo);
        if (pc->psma)
            std::printf("carry %d sma cur %d goal %d asega %d t %.4f svt %.3f\n", plo->oid, OidSmaCur(pc->psma), OidSmaGoal(pc->psma),
                        pc->psma->pasega && pc->psma->pasega->paseg ? pc->psma->pasega->paseg->oid : -1,
                        pc->psma->pasega ? pc->psma->pasega->t : -1.0f, pc->psma->pasega ? pc->psma->pasega->svt : 0.0f);
        std::printf("carry %d update: state %d n158 %d pos %.1f %.1f %.1f inworld %d oxa %p\n", plo->oid, pc->state, pc->n158, v.x, v.y, v.z,
                    plo->fInWorld, static_cast<void*>(Poxa(static_cast<ALO*>(plo))));
    }
    if (pc->state == 1 && pc->n158 != -1) LandCarry(plo, pc->plo1A0);
    if (pc->plo16C && pc->sRange < glm::length(PosLoWorld(plo) - PosLoWorld(pc->plo16C)))
        if (pc->state == 1 || pc->state == 2) SetCarryState(plo, 3);  // (slot 75 0x224190 with 0)
}


// orig: 0x00224380 (vt slot 58: the base's; each contact of its own with something but the steppers, the
// carryables, 0x4F, 0x67: it landed (+0x158 2, +0x1A0 the other); held a while with none: 4)
void UpdateCarryContacts(ALO* palo)
{
    NotifyAloContacts(palo);  // 0x1F4F10
    CARRYRT* pc = Pcarry(palo);
    if (!pc) return;
    if (!pc->state && g_clock.t - pc->t < 0.25f) return;  // (D_002C42E0)
    OXA* poxa = Poxa(palo);
    if (!poxa) return;
    for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
        for (XP* x = ox->pxp; x; x = x->pxpNext)
        {
            ALO* q = ox->psoOther;
            bool fOk = x->xpk != 8 && FCarryLands(*pc);
            for (ALO* r = q; r && fOk; r = r->paloParent)  // 0x224508
                for (CID cid : {0x33, 0x64, 0x4F, 0x67})
                    if (FIsLoDerivedFrom(r, cid)) fOk = false;
            if (fOk)
            {
                if (pc->n158 != 2 && std::getenv("SLY2N_CARRYPRINT"))
                    std::fprintf(stderr, "carry-land: frame %d t %.4f carry %d touched %d (cid 0x%X parent %d) at %.1f %.1f %.1f\n", g_iframeHarness, g_clock.t,
                                 palo->oid, q ? q->oid : -1, q ? q->Cid() : -1, q && q->paloParent ? q->paloParent->oid : -1,
                                 PosAloCenter(palo).x, PosAloCenter(palo).y, PosAloCenter(palo).z);
                pc->plo1A0 = q;
                pc->n158 = 2;
                // +0x190: the contact's normal, its sign by the end the carryable is (D_002C3C60 {1, -1})
                const int i = x->axpd[0].paloRoot != PaloPhysRoot(palo) ? 1 : 0;
                pc->dirLand = x->normal * (i == 0 ? 1.0f : -1.0f);
            }
            else if (!pc->state && pc->n158 == -1)
                pc->n158 = 4;
        }
}

// a field (+ib) of the object: a script's value (its EOPID) or the load's
int NCarryField(LO* plo, int eopid, uint32_t ib, int nDefault)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == eopid && !o.aval.empty()) return o.aval[0].n;
    return static_cast<int>(plo->U32Field(0, ib, static_cast<uint32_t>(nDefault)));
}

// orig: 0x00224820 (one ray of the blast: the nearest entry it hits; its owner with the filter's bytes recorded once
// (a contact message: the impulse along the ray), and its body pushed by the ray (the response 1 - u^2 of the
// point's speed along it, u = that speed / 1000, times 100))
struct BLASTHIT
{
    ALO* palo = nullptr;
    glm::vec3 pos{0.0f};
    glm::vec3 vec{0.0f};
};
void BlastRay(LO* plo, uint32_t grf, const glm::vec3& a, const glm::vec3& b, const std::vector<SOE*>& apsoe,
              std::vector<BLASTHIT>* pahit)
{
    LSG lsg;
    SOE* psoe = PsoeHitTestLineObjects(0, a, b, apsoe, &lsg);  // 0x14CC58
    if (!psoe || !psoe->palo) return;
    const glm::vec3 d = b - a;
    const float l = glm::length(d);
    if (l <= 0.0f) return;
    const glm::vec3 n = d / l;
    ALO* pso = PaloXpFilter(psoe->palo, grf);  // 0x1F7530
    if (!pso) return;
    bool f = false;
    for (const BLASTHIT& h : *pahit) f = f || h.palo == pso;
    if (!f && pahit->size() < 0x20) pahit->push_back(BLASTHIT{pso, lsg.pos0, n * 1000.0f});  // D_002C4334
    ALO* pbody = PaloPhysRoot(pso);
    SOST* ps = pbody ? Psost(pbody) : nullptr;
    if (!ps || ps->m <= 0.0f) return;
    const glm::vec3 r = lsg.pos0 - PosAloCenter(pbody);
    const glm::vec3 vPoint = ps->v + glm::cross(ps->w, r);  // 0x22D348
    const float along = glm::dot(vPoint, n);
    const float u = along / 1000.0f;
    float resp = 1.0f + u * (0.0f + u * -1.0f);  // D_002C4320, limited by D_002BBB60
    resp = resp < 0.0f ? 0.0f : (1.0f < resp ? 1.0f : resp);
    if (!(0.0f < resp)) return;
    const glm::vec3 imp = n * (resp * 100.0f);  // D_002C4330
    // (through the body's linear / angular constraints: a fixed body (kind 3) takes no push (UE8: the floor
    // 10932 of the final job was set drifting by Bentley's second bomb, so his closing walk missed its point))
    const glm::vec3 lin = DvAloImpulse(pbody, imp);                 // 0x1FAA00
    const glm::vec3 ang = DwAloImpulseAt(pbody, lsg.pos0, imp);    // 0x1FAA48
    const XF* pxfRot = PxfRot(pbody);
    const glm::vec3 rRot = lsg.pos0 - (pxfRot ? glm::vec3(pxfRot->matWorld[3]) : glm::vec3(0.0f));  // (+0x5C's position)
    const float change = glm::dot(lin + glm::cross(ang, rRot), n);
    if (!(0.0f < change)) return;
    float frac = (1000.0f - along) / change;
    frac = frac < 0.0f ? 0.0f : (1.0f < frac ? 1.0f : frac);
    SetAloVelocityTo(pbody, VelAloPos(pbody) + lin * frac);    // slots 33 / 35
    SetAloAngVelocityTo(pbody, AngVelAlo(pbody) + ang * frac); // slots 34 / 36
}

// orig: 0x00224BF8 (the blast at pos, radius r: rays in 11 rings (D_002C4368 / D_002C4338) and one toward each
// attachment in reach; a contact message (filter +0x174, state +0x178) to each object hit; then 0x156698, the
// noise the guards hear)
void BlastCarry(LO* plo, float r)
{
    ALO* palo = static_cast<ALO*>(plo);
    const glm::vec3 pos = PosAloCenter(palo);  // (0x224E70: 0x1A62E8, the object record's position, not the root's)
    const uint32_t grf = static_cast<uint32_t>(NCarryField(plo, 1107, 0x174, 0));
    const int nState = NCarryField(plo, 1108, 0x178, -10);
    // 0x20FC60 with the filter 0x20FA10 (context: the blast's own attachment): the attachments whose sphere (+0x20,
    // radius +0x30) is within r, their entries not of bit 48, not of an attachment marked 4 nor of its own
    FlushSwOxaDirty1();  // 0x133E30
    UpdateSwAaox();      // 0x133E80
    std::vector<SOE*> apsoe;
    OXA* poxaOwn = Poxa(palo);
    for (OXA* poxa : g_physw.apoxaActive)
    {
        const glm::vec3 d = pos - poxa->posCenter;
        const float rr = (r + poxa->sRadius) * (r + poxa->sRadius);
        if (rr < glm::dot(d, d)) continue;
        for (SOE* pe : poxa->apsoe)
        {
            if (pe->f48 || !pe->palo) continue;
            OXA* po = Poxa(pe->palo);
            if (po && (po->grf & 4)) continue;
            if (std::getenv("SLY2N_CARRYDEBUG"))
                std::printf("carry %d: blast entry of %d (cid 0x%X) oxa %p own %p grf 0x%X\n", plo->oid, pe->palo->oid, pe->palo->Cid(), (void*)po,
                            (void*)poxaOwn, po ? po->grf : 0);
            if (po == poxaOwn) continue;
            if (apsoe.size() < 0x100) apsoe.push_back(pe);
        }
    }
    std::vector<BLASTHIT> ahit;
    static const int s_ac[11] = {3, 7, 9, 10, 11, 12, 11, 10, 9, 7, 3};
    static const float s_ag[11] = {-1.5f, -1.2f, -0.9f, -0.6f, -0.3f, 0.0f, 0.3f, 0.6f, 0.9f, 1.2f, 1.5f};
    for (int i = 0; i < 11; ++i)
        for (int j = 0; j < s_ac[i]; ++j)
            BlastRay(plo, grf, pos, pos + cmm::SetVectorSphere(static_cast<float>(j) * 2.0f * 3.1415927f / static_cast<float>(s_ac[i]), s_ag[i], r),
                     apsoe, &ahit);
    // one more ray toward each of those entries' records (the entry's +0: its record's world position, +0x70)
    for (size_t k = 0; k < apsoe.size(); ++k)
    {
        const SOE* pe = apsoe[k];
        const glm::vec3 posE = pe->pxf ? glm::vec3(pe->pxf->matWorld[3]) : glm::vec3(0.0f);
        BlastRay(plo, grf, pos, pos + cmm::LimitVectorLength(posE - pos, r), apsoe, &ahit);
    }
    if (std::getenv("SLY2N_CARRYDEBUG"))
        std::printf("carry %d: blast r %.0f filter 0x%X state %d: %zu entries, %zu hit\n", plo->oid, r, grf, nState, apsoe.size(), ahit.size());
    for (const BLASTHIT& h : ahit)
    {
        CTMSG ctm;
        ctm.ploSender = plo;
        ctm.ploReceiver = h.palo;
        ctm.grfFilter = grf;
        ctm.nState = nState;
        ctm.pos = h.pos;
        ctm.vec = h.vec;
        PaloAcceptContact(&ctm);  // 0x1F7598
    }
    // 0x156698(SW +0x402C: the fixed noise 709, the blast's place, the bomb)'s +0x54 = 1: the noise the guards hear
    if (NOISE* pn = PnoiseNew(PtmplNoiseFixed(3), pos, plo)) pn->nKind = 1;
}

// orig: 0x00224190 (vt slot 75: a contact accepted; dropped or free: the blast's state 3)
bool FAcceptCarryContact(ALO* palo, CTMSG*)
{
    CARRYRT* pc = Pcarry(palo);
    if (pc && (pc->state == 1 || pc->state == 2)) SetCarryState(palo, 3);
    return true;
}

// orig: 0x002241D8 (vt slot 5: the base's; message 4 about itself while dropped / free: the blast's state)
void OnCarryMessage(LO* plo, int msgk, void* pv)
{
    // TODO(world): the base 0x133590's messages
    (void)pv;
    if (msgk != 4) return;
    WarnOnce("a carryable's message 4 (its own: state 3)");
}

struct INITCARRY
{
    INITCARRY()
    {
        RegisterLoUpdate(0x2240B0, UpdateCarry);
        RegisterLoUpdate(0x225300, UpdateExtractor);
        g_pfnHealthExtractorHook = ApplyExtractorHook;
        // EOPID 53 (any ALO; thunk 0x100FE0): 0x1F7918, the contact bytes of the filter bits a set to b
        RegisterObjectMethod(0x100FE0, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), 0x30))
                SetAloXpBytes(static_cast<ALO*>(plo), static_cast<uint32_t>(NArg(a, 0)), static_cast<uint8_t>(NArg(a, 1)));
            return CRef(TAGK_Void);
        });
        // EOPID 1105 (0x224610: a dart-like kind 7 / 11) and 1106 (0x224638: it lands on what it touches)
        RegisterObjectGetter(1105, [](LO* plo) {
            CRef r;
            const CARRYRT* pc = Pcarry(plo);
            r.SetBool(pc && (pc->kind == 7 || pc->kind == 11));
            return r;
        });
        RegisterObjectGetter(1106, [](LO* plo) {
            CRef r;
            const CARRYRT* pc = Pcarry(plo);
            r.SetBool(pc && FCarryLands(*pc));
            return r;
        });
        // EOPID 1104 (+0x15C the kind) / 1111 (+0x16C the carrier) as the getters read them
        RegisterObjectGetter(1104, [](LO* plo) {
            CRef r;
            if (const CARRYRT* pc = Pcarry(plo)) r.SetS32(pc->kind);
            return r;
        });
        // EOPID 1110 (thunk 0x10D9E0): 0x224E70 (the blast: its activation parent none (0x1A58D0(p, 0, 1)), at its place)
        RegisterObjectMethod(0x10D9E0, [](LO* plo, const std::vector<CRef>& a) {
            if (Pcarry(plo))
            {
                // 0x1A58D0(p, 0, 1): the ALO's vt slot 10 (0x1F21B8), whose physics move (slot 13) gives a bomb that had
                // landed on an object (LandCarry: parented, sharing its attachment) its own attachment again before the
                // blast, so the blast's 'not its own' test (0x20FA10) keeps that object (U61; the reference's bomb
                // +0xEC leaves box 2's attachment at the blast)
                if (FCidDerivedFrom(plo->Cid(), 0x30)) SetAloParent(static_cast<ALO*>(plo), nullptr, nullptr, 1);
                else SetLoParentActivation(g_psw, plo, nullptr);
                BlastCarry(plo, GArg(a, 0));
            }
            return CRef(TAGK_Void);
        });
    }
} s_initcarry;
}  // namespace
NATIVEFN(0x224380, UpdateCarryContacts);
NATIVEFN(0x224190, FAcceptCarryContact);
NATIVEFN(0x2241D8, OnCarryMessage);

namespace
{
void SetCarryStateVt(LO* plo, int n) { SetCarryState(plo, n); }
}
NATIVEFN(0x224EC8, SetCarryStateVt);
