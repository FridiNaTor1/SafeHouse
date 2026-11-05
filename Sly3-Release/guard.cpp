// The guards and NPCs (class 0x38 family): their per-frame update, state mask, kind table, targets and detector.
// See guard.h. Reference: Sly2-Reference/Sly2-Release/game/m_205E70.cpp, m_205E70_p3.cpp, m_20B840_p3.cpp (the watchers),
// m_203948_p3.cpp / m_2040F8.cpp (STEP's update and heading), m_14F030_p3.cpp / m_153FF8_p3.cpp (the detector).

#include "guard.h"
#include "pocket.h"
#include "po.h"
#include "aipursuit.h"

#include "activate.h"
#include "actanim.h"
#include "aijobs.h"
#include "animfind.h"
#include "asega.h"
#include "brx.h"
#include "bsp.h"
#include "clock.h"
#include "crv.h"
#include "detect.h"
#include "ref.h"
#include "smrun.h"
#include "spliceobj.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "update.h"
#include "xf.h"
#include "vecmat.h"
#include "stepphys.h"

#include "cls.h"

#include <cfloat>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>
#include <unordered_map>

glm::mat3 BuildRotateVectorsMatrix(const glm::vec3& vecA, const glm::vec3& vecB);  // mat.h (0x11BA08)
namespace
{
std::unordered_map<const LO*, std::unique_ptr<GUARD>>& Map()
{
    static std::unordered_map<const LO*, std::unique_ptr<GUARD>> s_map;
    return s_map;
}
constexpr float T_MODE_8 = 10.0f, T_MODE_34 = 30.0f, T_MODE_21 = 5.0f;  // D_002C3D40..48
constexpr float S_TARGET_NEAR = 1500.0f;               // D_002C3D04
const glm::vec3 AXIS_Z(0.0f, 0.0f, 1.0f);
constexpr int s_acidModeExcluded[] = {0x3B, 0x40, 0x3D, 0x3E, 0x41}; // D_002C3D10

glm::vec3 PosOf(const LO* plo) { return PosAi(plo); }

// the object record's world matrix (vt slot 31: ALO +0x60's +0x40)
glm::mat3 MatOf(const GUARD* pg)
{
    XF* pxf = pg->palo->apxfRef[2] ? pg->palo->apxfRef[2] : (pg->palo->axf.empty() ? nullptr : pg->palo->axf[0].get());
    return pxf ? glm::mat3(pxf->matWorld) : glm::mat3(1.0f);
}
}  // namespace

glm::vec3 PosAi(const LO* plo)
{
    if (plo && FCidDerivedFrom(plo->Cid(), 0x30))
    {
        const ALO* palo = static_cast<const ALO*>(plo);
        if (palo->apxfRef[2]) return glm::vec3(palo->apxfRef[2]->matWorld[3]);
    }
    return plo ? PosLoWorld(plo) : glm::vec3(0.0f);
}

GUARD* PguardFromLo(LO* plo)
{
    auto it = Map().find(plo);
    return it == Map().end() ? nullptr : it->second.get();
}
bool FLoGuard(const LO* plo) { return plo && FCidDerivedFrom(plo->Cid(), 0x38); }
GUARD* PguardNew(ALO* palo)
{
    auto& p = Map()[palo];
    p = std::make_unique<GUARD>();
    p->palo = palo;
    p->stack.pguard = p.get();
    p->move.pguard = p.get();
    p->watchA.pguard = p.get();
    p->watchB.pguard = p.get();
    return p.get();
}
void ResetEndedJobs();  // (aijob.cpp)
void ResetGuards()
{
    ResetEndedJobs();  // (the ended jobs before their guards)
    Map().clear();  // release jobs/listeners before their detectors
    ResetGuardDetectors();
}
std::vector<GUARD*> ApguardAll()
{
    std::vector<GUARD*> a;
    for (auto& [plo, pg] : Map()) a.push_back(pg.get());
    return a;
}

// ---- the heading (STEP's) ----

// orig: 0x00204660
void SetGuardHeading(GUARD* pg, float rad) { SetStepPhysHeading(pg->palo, rad); }

glm::vec3 PosGuardProbe(const GUARD* pg)
{
    if (pg->fProbeRam) return glm::vec3(pg->posProbe);
    const STEPPHYS& s = St(pg);
    ALO* palo = pg->palo;
    if (s.isoeCustom >= 0 && size_t(s.isoeCustom) < palo->asoe.size())
    {
        const auto& apos = palo->asoe[s.isoeCustom].geomWorld.apos;
        if (s.iposProbe0 >= 0 && size_t(s.iposProbe0) < apos.size()) return apos[s.iposProbe0];
    }
    return PosAi(palo);
}
// orig: 0x00204740
float RadGuardHeading(GUARD* pg)
{
    const glm::mat3 m = MatOf(pg);
    return std::atan2(m[0][1], m[0][0]);
}

// orig: 0x00209818
void SetMoveHeading(MOVEREF* pm, float rad)
{
    if (pm->fHeadingLocked == 0) SetGuardHeading(pm->pguard, rad);
}

// ---- the state mask ----

// orig: 0x00209870 (then 0x1F1900: the reward giver's mode, pocket.cpp: no glow / pickpocket while alerted)
void SetGuardStateMask(GUARD* pg, uint64_t grf)
{
    pg->grfState = grf;
    UpdatePocketMode(pg->palo);  // 0x1F1900
}

// orig: 0x00204F78 for a guard (climb.cpp's PairActorWith): the guard and the nearest ancestor of the object with an
// attachment (the object when it is an ALO, else its parent) overlap (f: their contacts ignored) or not (0x133A38)
static void PairGuardWith(GUARD* pg, LO* plo, bool f)
{
    ALO* pso = nullptr;
    if (plo) pso = FIsLoDerivedFrom(plo, 0x30) ? static_cast<ALO*>(plo) : plo->paloParent;
    while (pso && !Poxa(pso)) pso = pso->paloParent;
    OXA* poxa = Poxa(pg->palo);
    if (poxa && pso && Poxa(pso)) UpdateSwPox(poxa, Poxa(pso), f ? 1 : 2);
    MarkAloDirty(pg->palo);  // 0x1F4BF8
}

// orig: 0x0020B2A8 (the kind-2 player's landing on its related object, 0x20C2F8 state 3): the guard moves on it: a
// ladder (cid 0xB) 1 with its place along the ladder, a chair (0xD) 0 likewise, a bar (0x13 in state 3) 2, a 0x27 3;
// the guard and the object's attached ancestor overlap from then on (the arena's lip does not push a guard off its
// ladder). TODO(ai): the ladder's place is the nearest to the guard's +0xF90 sub-object (its hands) in the original;
// natively the guard's own place
void AttachGuardMove(GUARD* pg, LO* plo, int n)
{
    pg->ploMove = plo;
    pg->nMoveKind = n;
    const bool fLadder = FIsLoDerivedFrom(plo, 0xB);
    if (fLadder || FIsLoDerivedFrom(plo, 0xD))
    {
        pg->kMove = fLadder ? 1 : 0;
        if (const CRV* pcrv = PcrvFromLo(plo))
        {
            float s = 0.0f;
            NearestCrvAll(pcrv, PosAi(pg->palo), nullptr, nullptr, nullptr, &s, nullptr);  // 0x1E6F60
            pg->sMove = s;
        }
        PairGuardWith(pg, plo, true);
    }
    else if (FIsLoDerivedFrom(plo, 0x13) && plo->nState == 3)
    {
        pg->kMove = 2;
        PairGuardWith(pg, plo, true);
    }
    else if (FIsLoDerivedFrom(plo, 0x27))
    {
        pg->kMove = 3;
        PairGuardWith(pg, plo, true);
    }
    else pg->ploMove = nullptr;
}

// orig: 0x0020B210 (the move's object let go: the pair undone, walking again)
void DetachGuardMove(GUARD* pg)
{
    if (0 <= pg->kMove && pg->kMove <= 3) PairGuardWith(pg, pg->ploMove, false);
    pg->ploMove = nullptr;
    pg->kMove = -1;
}

// orig: 0x00209890
void AddGuardState(GUARD* pg, int n)
{
    const uint64_t m = GrfStateBit(n);
    SetGuardStateMask(pg, pg->grfState | m);
    CRef aref[2];
    aref[0].SetS32(n);
    aref[1].SetS32(-1);
    HandleLoSpliceEvent(pg->palo, 7, false, 2, aref);  // vt slot 3: (self, added state, removed state)
    // Original 0x2C3D98: test the added bit after the script, even if it was already in the state mask.
    static constexpr int anStateEvent[2][2] = {{3, 20}, {12, 22}};
    for (const auto& entry : anStateEvent)
        if (m & GrfStateBit(entry[0])) BroadcastTaskEvent(g_psw, entry[1], pg->palo);
}

// orig: 0x00209970
void RemoveGuardState(GUARD* pg, int n)
{
    SetGuardStateMask(pg, pg->grfState & ~GrfStateBit(n));
    CRef aref[2];
    aref[0].SetS32(-1);
    aref[1].SetS32(n);
    HandleLoSpliceEvent(pg->palo, 7, false, 2, aref);
}

// orig: 0x00209AD0
KINDE* PkindeBest(GUARD* pg, uint64_t mask)
{
    uint64_t m = 0;
    for (int k = 0; k < 0x30; ++k)
    {
        m = GrfStateBit(k);
        if (mask & m) break;
    }
    int best = 0;
    KINDE* pbest = nullptr;
    for (KINDE& e : pg->akinde)
    {
        // orig 0x209A28: the number of bits of a when they are all in b
        const uint64_t x = e.mask & mask;
        int r = 0;
        if (x == e.mask)
            for (uint64_t y = x; y; y &= y - 1) ++r;
        if (r != 0 && (e.mask & m) != 0 && best < r)
        {
            best = r;
            pbest = &e;
        }
    }
    return pbest;
}

// orig: 0x00209278
void PlayKinde(KINDE* pe, GUARD* pg, int grf, float g)
{
    switch (pe->k)
    {
    case 0:
        if (pe->plo) ApplyAseg(static_cast<ASEG*>(pe->plo), pg->palo, g, 1.0f, grf, &pe->pasega);
        break;
    case 1:
        if (pe->plo) pe->psma = PsmaApplySm(static_cast<SM*>(pe->plo), pg->palo, -1, grf);  // orig 0x1F0198
        break;
    case 2:
        // the kind-2 player on the guard's +0xDB0 (0x20BF80)
        StartK2(pe->pk2, pg->palo, grf, &pg->k2play);
        pe->pvState = &pg->k2play;
        break;
    case 3:
        pe->pvState = &pg->k3play;
        StartK3(pe->pk3, pg->palo, grf, &pg->k3play);  // 0x20C948
        break;
    default: return;
    }
}

// orig: 0x00209360
void StopKinde(KINDE* pe)
{
    switch (pe->k)
    {
    case 0: RetractAsegaPp(&pe->pasega); break;  // 0x130B18 (and the handle released)
    case 2: if (pe->pvState) StopK2(static_cast<K2PLAY*>(pe->pvState)); break;  // 0x20C080
    case 3: if (pe->pvState) StopK3(static_cast<K3PLAY*>(pe->pvState)); break;  // 0x20CA48
    case 1:
        if (pe->psma)
        {
            RetractSma(pe->psma);
            pe->psma = nullptr;
        }
        break;
    default: break;
    }
}

// orig: 0x00209430
bool FKindeDone(const KINDE* pe)
{
    if (!pe) return true;
    switch (pe->k)
    {
    case 0: return !pe->pasega || FAsegaRetracted(pe->pasega);
    case 2: return !pe->pvState || FK2Done(static_cast<const K2PLAY*>(pe->pvState));  // 0x20C0D8
    case 3: return FK3Done(static_cast<const K3PLAY*>(pe->pvState));  // 0x20CA98
    case 1:
        if (!pe->psma) return true;
        if (FSmaRetracted(pe->psma)) return true;
        return !pe->psma->pasega;  // (+0x30 clear)
    default: return true;
    }
}

// orig: 0x00209130 (the 0x15E938 kind 4 find around the guard)
void ResolveKinde(KINDE* pe, GUARD* pg)
{
    if (pe->k == 3) { ResolveK3(pe->pk3, pg->palo); return; }  // 0x20C7E0
    if (pe->k == 2 && pe->pk2) return;
    LO* po = PloFindSwNearest(pg->palo, pe->oid);
    if (!po)
    {
        pe->k = -1;
        return;
    }
    if (FCidDerivedFrom(po->Cid(), 0x6D) && po->Cid() == 0x6D)
    {
        bool f22 = false;
        for (const ASEGEV& ev : PasegdOf(static_cast<ASEG*>(po))->aaev[1])
            if (ev.kind == 11 && ev.an[0] == 0x22) f22 = true;
        if (f22)
        {
            pe->k = 2;  // the kind-2 object (0x20C010 / 0x20BD78 / 0x20BD88)
            pe->pk2 = Pk2Resolve(pg, po);
            if (!pe->pk2) pe->k = -1;
            if (pe->grf == -1) pe->grf = 0;
        }
        else
        {
            pe->plo = po;
            pe->k = 0;
        }
    }
    else if (po->Cid() == 0x6F)
    {
        pe->k = 1;
        pe->plo = po;
        if (pe->grf == -1) pe->grf = 0;
    }
    else pe->k = -1;
}

// orig: 0x00209618 (0x204E38 / 0x204ED8: the root channel's velocity at the ASEGA's time, kinds 1 / 6)
void SteerByKinde(KINDE* pe, GUARD* pg)
{
    glm::vec4 vel(0.0f), rot(0.0f);
    auto root = [&](ASEGA* pasega, int k) {
        glm::vec4 v(0.0f);
        if (pasega && !FAsegaRetracted(pasega))
            if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, pg->palo, k, true))
                if (pchn->ptrkp) pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
        return v;
    };
    if (pe->k == 0) {
        vel = root(pe->pasega, 1);
        rot = root(pe->pasega, 6);
    }
    else if (pe->k == 1)
    {
        ASEGA* pasega = pe->psma ? pe->psma->pasega : nullptr;
        vel = root(pasega, 1);
        rot = root(pasega, 6);
    }
    else if (pe->k == 2 && pe->pvState)
    {
        // the kind-2 player's update, and outside the flight its animation's root velocity
        K2PLAY* pk = static_cast<K2PLAY*>(pe->pvState);
        UpdateK2(pk);
        // (a ballistic hop stays held by its constraint until state 4)
        if (!(pk->st == 1 || pk->st == 2 || pk->st == 4) && !(pk->st == 3 && pk->fBallistic)) vel = root(pk->pasega, 1);
    }
    else if (pe->k == 3 && pe->pvState)
        vel = static_cast<K3PLAY*>(pe->pvState)->vel;  // the original +0xE20 player's +0x10
    if (pg->pscaleFC4) vel *= pg->pscaleFC4->pxf->mat[0][0];
    St(pg).posTarget = rot;
    St(pg).vecVel = glm::vec3(vel) * -1.0f;
}

// ---- targets ----

// orig: 0x002087E8 / 0x002089F0 / 0x00208B38: target class-list membership.
bool FTargetClass(const std::vector<int>& classes, const LO* plo)
{
    for (int cid : classes)
        if (FCidDerivedFrom(plo->Cid(), cid)) return true;
    return false;
}

namespace
{

// orig: 0x00208728 / 0x00208948: preserve the original forward removal order,
// including advancing after a removal if the authored list contains duplicates.
void RemoveTargetClass(std::vector<int>& classes, int cid)
{
    for (size_t i = 0; i < classes.size(); ++i)
        if (classes[i] == cid) classes.erase(classes.begin() + i);
}
}

// orig: 0x00206EB0 (0x206D48 tests the five special guard families).
bool FGuardModeAllowed(GUARD* pg, int n)
{
    if (n == -1) return true;
    if (FGuardState(pg, 7)) return false;
    if (auto* job = dynamic_cast<JOBSCRIPT*>(PjobFind(pg, 0xA9)); job && job->fFromReact) return false;
    auto is = [&](int cid) { return FCidDerivedFrom(pg->palo->Cid(), cid); };
    switch (n)
    {
    case 8: case 13: case 14: return true;
    case 21: return !(is(0x3E) || is(0x42) || is(0x43));
    case 34: case 37:
        for (int cid : s_acidModeExcluded) if (is(cid)) return false;
        return true;
    case 11:
        return !(is(0x3B) || is(0x3D) || is(0x40) || is(0x3E) || is(0x41) || is(0x3F) || is(0x3C) || is(0x42) || is(0x43));
    default: return false;
    }
}

// orig: 0x00207030: assign the new mode/time before ending the old effect and
// starting the new one. Repeated requests do not extend the timer.
void SetGuardMode(GUARD* pg, int n)
{
    if (n == pg->nMode || !FGuardModeAllowed(pg, n)) return;
    const int old = pg->nMode;
    pg->nMode = n;
    pg->tMode = g_clock.t;
    switch (old)
    {
    case 8: case 13:
        if (pg->pscaleFC4) SetCtrlPos(pg->pscaleFC4, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
        break;
    case 34: case 37:
        RemoveTargetClass(pg->acid398, 0x38);  // 0x208728
        for (int cid : s_acidModeExcluded) RemoveTargetClass(pg->acid3AC, cid); // 0x206E50
        break;
    case 14:
        if (auto* move = dynamic_cast<JOBMOVE*>(PjobFind(pg, 0x9D)))
            if (ASEGA* animation = PasegaOfKinde(move->pkinde))
                SetAsegaAsegSpeed(animation, animation->paseg->svt);
        break;
    case 21:
        if (auto* reaction = dynamic_cast<JOBREACT*>(PjobFind(pg, 0x9C)); reaction && reaction->msg.k == 18 &&
            reaction->msg.plo == PloPoCurrent(g_psw) && reaction->msg.g == FLT_MAX)
            reaction->NextReaction();
        break;
    }
    switch (n)
    {
    case 8: case 13:
        if (pg->pscaleFC4) SetCtrlPos(pg->pscaleFC4, glm::vec4(0.5f, 0.5f, 0.5f, 0.0f)); // D_002C3D30
        break;
    case 34: case 37:
        if (pg->acid398.size() < 3) pg->acid398.push_back(0x38); // 0x208700
        // orig: 0x00206DB0: include only special guard classes present in the
        // world (count query 0x605 includes inactive objects as well).
        for (int cid : s_acidModeExcluded)
            if (g_splicegame.pfnFindObjectsByClass &&
                g_splicegame.pfnFindObjectsByClass(0x605, cid, nullptr, 0, nullptr) && pg->acid3AC.size() < 5)
                pg->acid3AC.push_back(cid); // 0x208920
        break;
    case 14:
        if (auto* move = dynamic_cast<JOBMOVE*>(PjobFind(pg, 0x9D)))
            if (ASEGA* animation = PasegaOfKinde(move->pkinde)) SetAsegaAsegSpeed(animation, 0.0f);
        St(pg).vecVel = glm::vec3(0.0f);
        break;
    case 21:
        if (auto* reaction = dynamic_cast<JOBREACT*>(PjobFind(pg, 0x9C)))
            switch (reaction->msg.k)
            {
            case 1: case 2: case 3: case 4: case 12: case 16: case 17: case 18:
                reaction->NextReaction();
                break;
            }
        break;
    }
}


// (0x20A408 PloGuardInterest: ainoise.cpp)

namespace
{
// orig: 0x0014B290 (class 0x3F's vt slot 88: the base's in its state 0x24, else +0x390 when in the world, else the
// detector's live entries by rank (a player 0, a guard of class 0x3A 1, others 2), then by distance (the current
// target's: -400 + 0.8 d, 0x2BD4B0))
LO* PloGuardTarget3F(GUARD* pg)
{
    if (pg->ploTarget && pg->ploTarget->fInWorld) return pg->ploTarget;
    LO* best = nullptr;
    if (!pg->pdet) return nullptr;
    float sBest = FLT_MAX;  // (0x2A4EE8)
    int rankBest = 999;
    const glm::vec3 pos = PosOf(pg->palo);
    for (const DETENTRY& e : pg->pdet->ae)
    {
        LO* cand = e.plo;
        if (!e.st || !cand) continue;
        const int rank = FCidDerivedFrom(cand->Cid(), 0x34) ? 0 : FCidDerivedFrom(cand->Cid(), 0x3A) ? 1 : 2;
        float d = glm::length(pos - PosOf(cand));
        if (cand == pg->plo38C) d = -400.0f + d * (0.8f + d * 0.0f);
        if (rank < rankBest || (rank == rankBest && d < sBest))
        {
            rankBest = rank;
            sBest = d;
            best = cand;
        }
    }
    return best;
}
LO* PloGuardTargetBase(GUARD* pg);
}  // namespace

// orig: 0x002082D0 (vt slot 88; class 0x3F's 0x14B290)
LO* PloGuardTarget(GUARD* pg)
{
    if (pg->palo->pcls && pg->palo->pcls->PfnOrig(88) == 0x14B290 && !FGuardState(pg, 0x24)) return PloGuardTarget3F(pg);
    return PloGuardTargetBase(pg);
}

namespace
{
LO* PloGuardTargetBase(GUARD* pg)
{
    LO* cur = pg->ploTarget;
    if (cur && cur->fInWorld) return cur;
    LO* best = nullptr;
    float bestDistance = FLT_MAX;
    if (!pg->pdet) return nullptr;
    const glm::vec3 pos = PosOf(pg->palo);
    for (const DETENTRY& e : pg->pdet->ae)
    {
        LO* cand = e.plo;
        if (!cand) continue;
        if (!e.st)
        {
            if (pg->nMode != 0x22 && pg->nMode != 0x25) continue;
            if (!FCidDerivedFrom(cand->Cid(), 0x38)) continue;
        }
        if (FTargetClass(pg->acid3AC, cand)) continue;
        if (FTargetClass(pg->acid3C4, cand)) return cand;
        float distance = glm::length(pos - PosOf(cand));
        if (cand == pg->plo38C) distance = -400.0f + distance * 0.8f;  // 0x2C3D70
        if (pg->nMode == 0x22 || pg->nMode == 0x25)
            if (FCidDerivedFrom(cand->Cid(), 0x38)) distance = -500.0f + distance * 0.8f;  // 0x2C3D80
        if (FTargetClass(pg->acid398, cand))
        {
            if (best && !FTargetClass(pg->acid398, best))
            {
                bestDistance = distance;
                best = cand;
                continue;
            }
        }
        else if (best && FTargetClass(pg->acid398, best)) continue;
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = cand;
        }
    }
    return best;
}
}  // namespace

// ---- the watchers (0x20B840 module) ----

namespace
{
// orig 0x20D108
void SetWatchA(WATCHA* p, int n)
{
    if (p->st == n) return;
    p->st = n;
    p->t = g_clock.t;
    // TODO(ai): state 2's message (0x1F7598 {po, 0, po, 0x4000}: a fall)
}
// orig 0x20D020: off the ground with nothing below any of the five probes
bool FWatchA(WATCHA* p)
{
    GUARD* pg = p->pguard;
    if (FGuardFloor(pg)) return false;
    if (pg->kMove != -1) return false;
    if (!pg->f4A0) return false;
    if (FGuardState(pg, 0x13)) return false;
    return false;  // TODO(ai): the five line probes 0x173610 (with the 0x20CFC0 filter) at 0x2C3E00
}
// orig 0x20CE70
void UpdateWatchA(WATCHA* p)
{
    if (!p->fOn) return;
    const bool f = FWatchA(p);
    int k;
    do
    {
        k = p->st;
        if (k == 0) { if (f) SetWatchA(p, 1); }
        else if (k == 1)
        {
            if (!f) SetWatchA(p, 0);
            else if (0.1f < g_clock.t - p->t) SetWatchA(p, 2);  // TODO(ai): unless vt slot 76 (0x4000)
        }
        else if (k == 2) { if (!f) SetWatchA(p, 0); }
    } while (p->st != k);
}
// orig 0x20D3C0: state 2 sends the watched water's contact (its +0x188 filter, +0x184 state) to the guard
void SetWatchB(WATCHB* p, int n)
{
    if (n == p->st) return;
    p->st = n;
    p->t = g_clock.t;
    if (n != 2) return;
    ALO* pw = static_cast<ALO*>(p->plo);
    ALO* po = p->pguard->palo;
    CTMSG ctm;
    ctm.ploSender = pw;
    ctm.ploReceiver = po;
    ctm.grfFilter = pw->U32Field(0, 0x188, 0x2000);
    ctm.nState = static_cast<int>(pw->U32Field(0, 0x184, static_cast<uint32_t>(-10)));
    ctm.pos = PosLoWorld(po);  // 0x1A62E8
    ctm.vec = VelAloPos(po);   // vt slot 33
    PaloAcceptContact(&ctm);   // 0x1F7598
}
// orig 0x20D1F8 (the watched water +0xC, set by the guard's messages 4 / 5 (0x20D460)): 0 until the guard's point is
// inside it (its vt slot 73) while falling; 1 then, 2 (the contact) once the guard is not busy (vt slot 76), at once
// (D_002C3E58 0); back to 0 when the water is gone or the guard busy again
void UpdateWatchB(WATCHB* p)
{
    if (!p->fOn) return;
    GUARD* pg = p->pguard;
    int k;
    do
    {
        k = p->st;
        switch (k)
        {
        case 0:
            if (p->plo && FCidDerivedFrom(p->plo->Cid(), 0x30) && FAloContainsPoint(static_cast<ALO*>(p->plo), PosLoWorld(pg->palo)) &&
                VelAloPos(pg->palo).z < 0.0f)
                SetWatchB(p, 1);
            break;
        case 1:
            if (!p->plo) SetWatchB(p, 0);
            else if (g_clock.t - p->t > 0.0f && !FGuardBusy76(pg)) SetWatchB(p, 2);
            break;
        case 2:
            if (!p->plo || FGuardBusy76(pg)) SetWatchB(p, 0);
            break;
        }
    } while (p->st != k);
}
}  // namespace

// ---- the update ----

// orig 0x207288: the timed modes left (0x207030(p, -1))
void UpdateGuardTimedMode(GUARD* pg)
{
    float lim;
    switch (pg->nMode)
    {
    case 8: case 13: lim = T_MODE_8; break;
    case 34: case 37: lim = T_MODE_34; break;
    case 21: lim = T_MODE_21; break;
    default: return;
    }
    if (lim < g_clock.t - pg->tMode)
        SetGuardMode(pg, -1);
}

// orig 0x206900
void RefreshGuardTargets(GUARD* pg)
{
    static const uint64_t s_mask = [] {
        uint64_t m = 0;
        for (int n : {5, 6, 0x17, 7, 0x10, 1, 0x16, 0x2A}) m |= GrfStateBit(n);
        return m;
    }();
    if (pg->grfState & s_mask) return;
    if (pg->pdet && !pg->acid398.empty() && g_splicegame.pfnFindObjectsByClass)
    {
        struct CANDIDATE { float distance; LO* plo; int st; float t8; };
        std::vector<CANDIDATE> candidates;
        std::vector<float> slotSeenTimes;
        for (const DETENTRY& entry : pg->pdet->ae) slotSeenTimes.push_back(entry.tC);
        const size_t firstGenerated = pg->pdet->ae.size() - std::min(pg->pdet->ae.size(), size_t(pg->c3A4));
        for (int cid : pg->acid398)
        {
            BASIC* objects[256];
            const int count = g_splicegame.pfnFindObjectsByClass(0x505, cid, nullptr, 256, objects);
            for (int i = 0; i < count; ++i)
            {
                LO* other = static_cast<LO*>(objects[i]);
                if (other == pg->palo) continue;
                const glm::vec3 delta = PosOf(pg->palo) - PosOf(other);
                const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                if (!(distance < S_TARGET_NEAR) || candidates.size() >= 64) continue;
                CANDIDATE candidate{distance, other, -1, 0.0f};
                for (size_t j = firstGenerated; j < pg->pdet->ae.size(); ++j)
                    if (pg->pdet->ae[j].plo == other)
                    {
                        candidate.st = pg->pdet->ae[j].st;
                        candidate.t8 = pg->pdet->ae[j].t8;
                        break;
                    }
                candidates.push_back(candidate);
            }
        }
        // orig: 0x002068D8 comparator, passed to 0x2338C0. Equal-distance
        // ordering depends on the sort implementation and is not a parity claim.
        std::qsort(candidates.data(), candidates.size(), sizeof(CANDIDATE), [](const void* a, const void* b) {
            return static_cast<const CANDIDATE*>(a)->distance < static_cast<const CANDIDATE*>(b)->distance ? -1 : 1;
        });
        pg->pdet->ae.resize(firstGenerated);
        pg->c3A4 = 0;
        for (const CANDIDATE& candidate : candidates)
        {
            // orig: 0x002085D0 with the nonzero class pointer: append without
            // deduplication, after the authored targets; the table holds eight.
            if (pg->pdet->ae.size() >= 8) continue;
            DETENTRY entry;
            entry.plo = candidate.plo;
            entry.t8 = g_clock.t;
            // 0x2085D0 initializes only +0/+4/+8 in the destination inline
            // slot. Its +0xC first-seen time stays with that slot, rather than
            // following the target whose state/time are restored below.
            if (pg->pdet->ae.size() < slotSeenTimes.size()) entry.tC = slotSeenTimes[pg->pdet->ae.size()];
            if (candidate.st != -1) { entry.st = candidate.st; entry.t8 = candidate.t8; }
            pg->pdet->ae.push_back(entry);
            ++pg->c3A4;
        }
    }
    if (pg->pdet) UpdateDetector(pg->pdet);
    RefreshGuardTargetrec(pg);  // (the +0x3CC record's timer, aipursuit.cpp)
}

extern ALO* g_paloLaunched;  // launcher.cpp (debug)
extern int g_cLaunchedFrames;

// orig: 0x00207350
void UpdateGuard(GUARD* pg)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(pg->palo);  // the ALO's update (0x203FC0's base)
    // TODO(physics): 0x203E08 (the mesh pair's point velocities)
    UpdateStepPhysTurn(pg->palo);
    if (!pg->palo->fInWorld) return;
    if (pg->nMode == 0xE) return;
    UpdateGuardTimedMode(pg);
    RefreshGuardTargets(pg);
    // TODO(ai): the held object +0xF00 (0x20AB68), the attachment +0xF0C (0x20ACC0)
    // the giver: while its prize is ready (+0x48C), the next prize again (0x1F1CF8: a key whose task became available
    // gets its reward readied, e.g. Ep6's Carmelita once 2905 is available)
    if (pg->plo494 && PocketOf(pg->palo).fReady) NextPocketPrize(pg->palo);
    UpdateWatchA(&pg->watchA);
    UpdateWatchB(&pg->watchB);
    UpdateJobs(pg);
    static const bool s_fHitDebug = std::getenv("SLY2N_HITDEBUG") != nullptr;  // (debug: the stacks that change)
    static float s_tThrown = -1.0f;
    if (s_fHitDebug && g_paloLaunched && g_cLaunchedFrames > 0 && s_tThrown != g_clock.t)
    {
        s_tThrown = g_clock.t;
        --g_cLaunchedFrames;
        const glm::vec3 p = PosAloCenter(g_paloLaunched), v = VelAloPos(g_paloLaunched);
        std::printf("thrown: t %.3f %d at %.1f %.1f %.1f vel %.1f %.1f %.1f inworld %d upd %d\n", g_clock.t, g_paloLaunched->oid, p.x, p.y, p.z,
                    v.x, v.y, v.z, g_paloLaunched->fInWorld, g_paloLaunched->iupd);
    }
    if (s_fHitDebug)
    {
        std::string s;
        char sz[32];
        for (const auto& pj : pg->stack.ajob) { std::snprintf(sz, sizeof sz, " %X", pj->cid); s += sz; }
        static std::unordered_map<GUARD*, std::string> s_mp;
        if (s_mp[pg] != s) std::printf("stack: t %.3f guard %d %p%s\n", g_clock.t, pg->palo->oid, (void*)pg->palo, s.c_str());
        s_mp[pg] = s;
    }
    if (pg->move.pjob) pg->move.pjob->Steer();  // orig 0x2097E0 (vt slot 11)
    if (pg->pasega34C && FAsegaRetracted(pg->pasega34C)) RetractAsegaPp(&pg->pasega34C);
    if (pg->f4A4 && FGuardFloor(pg)) pg->pos4B0 = glm::vec4(PosGuardProbe(pg), 1.0f);
    PrintStepDebug(&St(pg));
}


// orig: 0x002064D8 (the guards' vt slot 12, after the ALO's 0x1F2D48 (activate.cpp OnRemove): out of the world, the
// pursued object cleared (0x208540 with 0), the job stack emptied (0x1EECD0 on +0x26C) and the +0x34C ASEGA retracted
// (0x130658)). The ballroom people of 11:1990 go through it when their walk's fade-out ends (0x1F36E0 -> vt slot 9
// 0x1A5630 -> slot 12), the director adding them again later.
void OnRemoveGuard(LO* plo)
{
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return;
    SetGuardPursued(pg, nullptr);
    ClearJobs(pg);
    RetractAsegaPp(&pg->pasega34C);
}


namespace
{
void UpdateGuardLo(LO* plo)
{
    if (GUARD* pg = PguardFromLo(plo)) UpdateGuard(pg);
    else if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
}
// orig: 0x00205910: the step's heading frame fitted to the floor under cpos probe points (local to the heading
// frame at the object's position): each pass drops every point onto the floor between gMax above and gMin below
// (0x173610; a point without floor is moved 24 down the frame's up row, D_002C3CE0), takes the plane through the
// three, and turns the original frame's up row onto its normal by at most 0.6 (D_002C3CE8) (D_002C3CE4: 2 passes)
static void FitStepToFloor(ALO* palo, float gMin, float gMax, int cpos, const glm::vec3* apos)
{
    STEPPHYS& s = StepPhysOf(palo);
    const glm::mat3 matOrig = cmm::BuildRotateMatrixAxis(s.radHeading, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B500
    const glm::vec3 pos = PosLoWorld(palo);
    glm::mat3 matCur = matOrig;
    // (the radius of 0x20FC60's nearby query: FDropOnGround makes its own query)
    for (int pass = 0; pass < 2; pass++)
    {
        const glm::vec3 up = matCur[2];
        glm::vec3 apt[3];
        for (int i = 0; i < cpos && i < 3; i++)
        {
            glm::vec3 pt = matCur * apos[i] + pos;
            if (FDropOnGround(&pt, gMax, gMin, false)) apt[i] = pt;
            else apt[i] = pt + up * -24.0f;
        }
        const glm::vec3 c = glm::cross(apt[1] - apt[0], apt[2] - apt[0]);
        const float l = glm::length(c);
        const glm::vec3 normal = l < 0.0001f ? glm::vec3(0.0f, 0.0f, 1.0f) : c * (1.0f / l);
        glm::mat3 adjust = ::BuildRotateVectorsMatrix(matOrig[2], normal);  // 0x11BA08
        float rad;
        glm::vec3 axis;
        cmm::DecomposeRotateMatrixRad(adjust, &rad, &axis);  // 0x11BCF8
        if (0.6f < rad) adjust = cmm::BuildRotateMatrixAxis(0.6f, axis);
        matCur = cmm::MulMat3(adjust, matOrig);
    }
    s.matHeading = matCur;
}

// orig: 0x0014FAF8 (class 0x40's update, the Ep4 Contessa): the guards' update, then while walking (+0xF80 -1)
// on a floor (+0x208) the heading frame fitted to it (0x205910 with D_002BD940 {-200, 200} and D_002BD910)
static void UpdateGuard40(LO* plo)
{
    UpdateGuardLo(plo);
    GUARD* pg = PguardFromLo(plo);
    if (!pg || pg->kMove != -1 || !FGuardFloor(pg)) return;
    static const glm::vec3 s_apos[3] = {{150.0f, 0.0f, -140.0f}, {-75.0f, 130.0f, -140.0f}, {-75.0f, -130.0f, -140.0f}};
    FitStepToFloor(pg->palo, -200.0f, 200.0f, 3, s_apos);
}

// orig: 0x002075D0 (vt slot 5 of the guards: messages; the base 0x1C3E20 is TODO(world)), then the jobs (0x1EED78);
// class 0x3F's 0x14B218 after it: a strike (0xB) on itself makes the striker seen (0x154038 state 1)
bool HandleGuardMessage(LO* plo, int msg, void* pv)
{
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return false;
    // 4 / 5: a water entered / left ({water, object}: 0x228D20): the +0x49C watcher's water (0x20D460) when the water
    // has a contact state (+0x184)
    if ((msg == 4 || msg == 5) && pv)
    {
        LO* const* pm = static_cast<LO* const*>(pv);
        if (pm[1] == plo)
        {
            if (msg == 4 && pm[0] && pm[0]->U32Field(0, 0x184, static_cast<uint32_t>(-10)) != 0) pg->watchB.plo = pm[0];
            else if (msg == 5 && pm[0] == pg->watchB.plo) pg->watchB.plo = nullptr;
        }
    }
    // the kind-2 player's take-off (its ASEGA's listener 0x20C290: the event 0x22)
    if (msg == 9 && pv)
    {
        const ASEGMSG9* pev = static_cast<const ASEGMSG9*>(pv);
        if (pev->pasega && pev->pasega == pg->k2play.pasega && pev->n == 0x22) SetK2State(&pg->k2play, 1);
    }
    OfferJobMessage(pg, msg, pv);
    // Original ASEGA notification visits its owning guard before the directional player's listener.
    if (msg == 9 && pv) OnK3Event(&pg->k3play, static_cast<const ASEGMSG9*>(pv));  // 0x20CAA8
    if (plo->pcls && plo->pcls->PfnOrig(5) == 0x14B218 && !FGuardState(pg, 0x24) && msg == 0xB && pv)
    {
        LO* const* pm = static_cast<LO* const*>(pv);
        if (pm[0] == plo && pg->pdet)
            if (DETENTRY* pe = PdetentryFind(pg->pdet, pm[1])) SetDetentryState(pg->pdet, pe, 1);
    }
    return true;
}

struct INITGUARDS
{
    INITGUARDS()
    {
        RegisterLoUpdate(0x207350, UpdateGuardLo);
        RegisterLoUpdate(0x14FAF8, UpdateGuard40);
        RegisterLoMessageHandler(HandleGuardMessage);
    }
} s_initguards;
}  // namespace
