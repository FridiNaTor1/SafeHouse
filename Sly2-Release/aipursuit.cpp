// The guards' pursuit (module 0x174898..0x17C640 part): the pursuit controller 0xA5 (0x175758..0x176458), its
// action 0xA4 (0x1753D8..0x1756C0) striking with 0x9F (0x1751C0..0x1752F0), the surround 0xB3 (0x17BED0..0x17C640),
// the action selector 0x1758A0 over the guard's action objects (cid 0x78, its +0xEDC list), and the shared target
// records (0x1F8278 / 0x20A5F8: one per pursued object, the guards on it, its timer). See aipursuit.h.

#include "aipursuit.h"
#include "worldglobals.h"
#include "actor.h"
#include "vecmat.h"
#include "asega.h"
#include "aihit.h"
#include "ainoise.h"

#include "aijobs.h"
#include "animfind.h"
#include "bsp.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "detect.h"
#include "guard.h"
#include "nav.h"
#include "so.h"
#include "sw.h"
#include "xfmath.h"
#include "update.h"
#include "vtnative.h"
#include "activate.h"
#include "step.h"
#include "stepphys.h"
#include "xf.h"
#include "xfphys.h"
#include "mat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace
{
constexpr float T_TARGET_LOST = 5.0f;      // D_002C3DB4 (0x20A7E0)
constexpr float T_SELECT_RETRY = 0.25f;    // D_002BE610
constexpr float S_CHASE_RESEARCH = 500.0f; // D_002BE614
constexpr float AG_ANGLE_WEIGHT[3] = {0.0f, 0.005f, 0.0f};  // D_002BE600 / 604 / 608
constexpr float S_SEEN_NEAR = 1000.0f, S_SEEN_HEIGHT = 250.0f;  // D_002C3D08 / D_002C3D0C
constexpr float S_SURROUND_REPLAN = 200.0f;  // D_002BE748 (0x17C3E8): distance from the follower's path

std::unordered_map<const LO*, std::unique_ptr<TARGETREC>>& MpTarget()
{
    static std::unordered_map<const LO*, std::unique_ptr<TARGETREC>> s_map;
    WORLD_TABLE_LOCAL(s_map);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_map;
}
std::unordered_map<const LO*, std::unique_ptr<ACTIONOBJ>>& MpAction()
{
    static std::unordered_map<const LO*, std::unique_ptr<ACTIONOBJ>> s_map;
    WORLD_TABLE_LOCAL(s_map);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_map;
}

glm::vec3 PosOf(const GUARD* pg) { return PosAi(pg->palo); }
float DistXy(const glm::vec3& a, const glm::vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
bool FInRange(const glm::vec2& r, float g) { return r.x <= g && g <= r.y; }  // 0x11D910
}  // namespace

// ---- the players' status (vt slot 77) ----

namespace
{
std::unordered_map<const LO*, int>& MpStatus77()
{
    static std::unordered_map<const LO*, int> s_map;
    return s_map;
}
}  // namespace
int NPlayerStatus77(LO* plo)
{
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x32)) return 0;
    // (a harness's imported value first: its players are the reference's, not run natively)
    auto it = MpStatus77().find(plo);
    if (it != MpStatus77().end()) return it->second;
    using PFN = int (*)(LO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(plo, 77))) return pfn(plo);
    return 0;
}
void SetPlayerStatus77(LO* plo, int n) { MpStatus77()[plo] = n; }
void ClearPlayerStatus77() { MpStatus77().clear(); }

// ---- the target records ----

// orig: 0x001F8278 (0x1F8240, else a new one 0x20A5F8)
TARGETREC* PtargetrecOf(LO* plo)
{
    auto& p = MpTarget()[plo];
    if (!p)
    {
        p = std::make_unique<TARGETREC>();
        p->plo = plo;
    }
    return p.get();
}
TARGETREC* PtargetrecFind(LO* plo)
{
    auto it = MpTarget().find(plo);
    return it == MpTarget().end() ? nullptr : it->second.get();
}
void ResetTargetRecs()
{
    MpTarget().clear();
    MpAction().clear();
}

// orig: 0x0020A9A8
void SetTargetrecTimer(TARGETREC* pt, int n)
{
    pt->n8 = n;
    pt->t4 = g_clock.t;
}
// orig: 0x0020A7E0
bool FTargetrecExpired(const TARGETREC* pt) { return T_TARGET_LOST < g_clock.t - pt->t4; }

// orig: 0x0020A730
void AddTargetrecGuard(TARGETREC* pt, GUARD* pg)
{
    if (pt->apguard.empty())
    {
        pt->t0 = g_clock.t;
        SetTargetrecTimer(pt, 0);
    }
    pt->apguard.push_back(pg);
}
// orig: 0x0020A790
void RemoveTargetrecGuard(TARGETREC* pt, GUARD* pg)
{
    auto it = std::find(pt->apguard.begin(), pt->apguard.end(), pg);
    if (it != pt->apguard.end()) pt->apguard.erase(it);
}

// orig: 0x0020A690: the record's reference heading (mode 0: the camera's, 1: the target's facing)
float RadTargetrec(const TARGETREC* pt)
{
    if (pt->nMode == 0) return std::atan2(g_cm.mat[0][1], g_cm.mat[0][0]);  // (CM +0x60 / +0x64)
    if (pt->nMode == 1 && pt->plo)
    {
        // (vt slot 31 twice: the target's object matrix's first row)
        const glm::mat4 m = FCidDerivedFrom(pt->plo->Cid(), 0x30) && static_cast<ALO*>(pt->plo)->apxfRef[2]
                                ? static_cast<ALO*>(pt->plo)->apxfRef[2]->matWorld
                                : glm::mat4(1.0f);
        return std::atan2(m[0][1], m[0][0]);
    }
    return 0.0f;
}

// orig: 0x0020A818: whether the guard may act on the record's target now
bool FTargetrecAllows(const TARGETREC* pt, GUARD* pg)
{
    if (pt->f1C) return false;
    if (pg->gF30 == 0.0f)
        for (GUARD* p : pt->apguard)
            if (p->fF3C) return false;
    if (pt->nMode != 0 && pt->nMode != 1) return true;
    const glm::vec3 posG = PosOf(pg);
    if (pt->nMode == 0)
    {
        if (pg->gF34 != 0.0f) return true;
        // the guard on the screen (0x14F850 with the camera's planes, radius 0)
        if (!FInsideCmPlanes(&g_cm, posG - g_cm.pos, 0.0f)) return false;
    }
    // the target -> guard heading (0x11EF08) within +0x18 of the record's heading (0x20A690: the camera's / the
    // object's facing)
    const glm::vec3 d = posG - PosAi(pt->plo);
    const float rad = RadNormalize(RadNormalize(std::atan2(d.y, d.x)) - RadTargetrec(pt));
    return !(pt->g18 < std::fabs(rad));
}

// orig: 0x00208540: the guard's pursued object (+0x38C) and its record (+0x3CC)
void SetGuardPursued(GUARD* pg, LO* plo)
{
    if (plo == pg->plo38C) return;
    if (pg->plo38C && pg->ptarget3CC)
    {
        RemoveTargetrecGuard(pg->ptarget3CC, pg);
        pg->ptarget3CC = nullptr;
    }
    pg->plo38C = plo;
    if (plo)
    {
        pg->ptarget3CC = PtargetrecOf(plo);
        AddTargetrecGuard(pg->ptarget3CC, pg);
    }
}

// orig: the end of 0x206900: the record's timer from the detector's state of its target (1 seen; 2 lost but near)
void RefreshGuardTargetrec(GUARD* pg)
{
    TARGETREC* pt = pg->ptarget3CC;
    if (!pt || !pg->pdet) return;
    const DETENTRY* pe = PdetentryFind(pg->pdet, pt->plo);
    if (!pe) return;
    if (static const bool s_f = std::getenv("SLY2N_TRECPRINT") != nullptr; s_f && pe->st)
        std::printf("trec t %.3f guard %d target %d det st %d\n", g_clock.t, pg->palo->oid, pt->plo ? pt->plo->oid : -1, pe->st);
    if (pe->st == 1) SetTargetrecTimer(pt, 1);
    else if (pe->st == 2)
    {
        const glm::vec3 d = PosAi(pt->plo) - PosOf(pg);
        if (std::sqrt(d.x * d.x + d.y * d.y) < S_SEEN_NEAR && std::fabs(d.z) < S_SEEN_HEIGHT) SetTargetrecTimer(pt, 2);
    }
}

// ---- the action objects (cid 0x78) ----

// orig: 0x0020AE18. Actions can be selected by scripts after loading, so
// resolve every child action before animations alter its reference pose.
void PostLoadGuardAction(LO* plo)
{
    for (LO* owner = plo->paloParent; owner; owner = owner->paloParent)
        if (FCidDerivedFrom(owner->Cid(), 0x38))
        {
            GUARD* guard = PguardFromLo(owner);
            if (!guard) guard = PguardNew(static_cast<ALO*>(owner));
            PactionOf(plo, guard);
            break;
        }
}

ACTIONOBJ* PactionOf(LO* plo, GUARD* pg)
{
    auto& p = MpAction()[plo];
    if (p)
    {
        // +0xA8 (option 363) as a script sets it later (seto! of optid 368: Paris' guards' looks enable theirs)
        for (const OPT& o : plo->aopt)
            if (o.eopid == 363 && !o.aval.empty()) p->fA8 = o.aval[0].n;
        static const char* s_szEnable = std::getenv("SLY2N_ENABLEACTION");  // debug: this action oid always enabled
        if (s_szEnable && plo->oid == std::atoi(s_szEnable)) p->fA8 = 1;
        return p.get();
    }
    p = std::make_unique<ACTIONOBJ>();
    p->plo = plo;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval[0];
        switch (o.eopid)
        {
        case 353: p->r50 = glm::vec2(v.ag[0], v.ag[1]); break;
        case 354: p->r58 = glm::vec2(v.ag[0], v.ag[1]); break;
        case 355: p->r60 = glm::vec2(v.ag[0], v.ag[1]); break;
        case 356: p->g68 = v.ag[0] * 3.14159265f / 180.0f; break;  // (0x20AF08: degrees)
        case 357: p->g68 = v.ag[0]; break;
        case 360: p->g6C = v.ag[0]; break;
        case 359: p->f70 = v.n; break;
        case 361: p->f74 = v.n; break;
        case 358: p->f78 = v.n; break;
        case 363: p->fA8 = v.n; break;
        case 362:
            p->aimRef = v.n;
            if (o.aval.size() > 1) p->aimEndRef = o.aval[1].n;
            if (o.aval.size() > 2) p->aimMarker = o.aval[2].n;
            break;
        case 364:
            p->kinde.oid = v.n;
            p->kinde.grf = o.aval.size() > 1 ? o.aval[1].n : 0;
            break;
        default: break;
        }
    }
    if (pg && p->kinde.oid != -1) ResolveKinde(&p->kinde, pg);
    // orig: 0x0020AE18. Resolve the two kind-6 records and sample the
    // action's authored aiming direction at its marker event.
    if (pg && p->aimRef != -1 && p->kinde.k == 0 && p->kinde.plo)
    {
        XF* end = nullptr;
        for (const auto& xf : pg->palo->axf)
            if (xf->kind == 6)
            {
                if (!p->pxfAim && xf->tag == p->aimRef) p->pxfAim = xf.get();
                if (!end && xf->tag == p->aimEndRef) end = xf.get();
            }
        auto* aseg = static_cast<ASEG*>(p->kinde.plo);
        if (p->pxfAim && end)
            for (const ASEGEV& event : PasegdOf(aseg)->aaev[1])
                if (event.kind == 11 && event.an[0] == p->aimMarker)
                {
                    glm::mat4 mat;
                    CalculateAsegMatrices(aseg, pg->palo, end, p->pxfAim->pxfRef, event.t, 1.0f, &mat, nullptr);
                    p->vecAim = glm::vec3(mat[3]);
                    if (std::getenv("SLY2N_RESPONSEPRINT"))
                        std::fprintf(stderr, "response-aim: guard %d action %d marker %d t %.6f vector %.6f %.6f %.6f\n",
                            pg->palo->oid, plo->oid, p->aimMarker, event.t, p->vecAim.x, p->vecAim.y, p->vecAim.z);
                    break;
                }
    }
    return p.get();
}

// orig: 0x0020AC28. The action's aim controller runs alongside its clip.
void SetGuardActionAim(GUARD* pg, ACTIONOBJ* action, XF* targetFrame, const glm::vec3& targetPos)
{
    if (pg->pactionF08)
    {
        DeleteCtrl(&pg->pctrlF0C);
        pg->pactionF08 = nullptr;
    }
    pg->pactionF08 = action;
    if (action && action->pxfAim)
    {
        pg->pctrlF0C = PctrlNewPos(pg->palo, action->pxfAim, 5, 8, 3, glm::vec4(0.0f));
        pg->pxfF10 = targetFrame;
        pg->posF20 = targetPos;
    }
}

// orig: 0x0020AB68. A response action does not replace the movement job.
void SetGuardResponse(GUARD* pg, ACTIONOBJ* action, XF* targetFrame, const glm::vec3& targetPos)
{
    if (pg->ploF00)
    {
        ACTIONOBJ* previous = PactionOf(pg->ploF00, pg);
        StopKinde(&previous->kinde);
        pg->ploF00 = nullptr;
        pg->tF04 = g_clock.t + GRandInRange(previous->r60.x, previous->r60.y);
    }
    pg->ploF00 = action ? action->plo : nullptr;
    if (action)
    {
        PlayKinde(&action->kinde, pg, action->kinde.grf | 1, 0.0f);
        pg->tF04 = FLT_MAX;
        if (std::getenv("SLY2N_RESPONSEPRINT"))
            std::fprintf(stderr, "response: t %.6f guard %d action %d clip %d aim %d vector %.6f %.6f %.6f\n",
                g_clock.t, pg->palo->oid, action->plo->oid, action->kinde.oid, action->pxfAim ? action->pxfAim->tag : -1,
                action->vecAim.x, action->vecAim.y, action->vecAim.z);
    }
    SetGuardActionAim(pg, action, targetFrame, targetPos);
}

// orig: 0x0020ACC0. Convert the target into the aimed record's parent frame.
void UpdateGuardActionAim(GUARD* pg)
{
    ACTIONOBJ* action = pg->pactionF08;
    if (!action || !action->pxfAim || !pg->pctrlF0C) return;
    const glm::vec3 target = PosConvertXfs(pg->pxfF10, action->pxfAim->pxfRef, pg->posF20);
    const glm::vec3 direction = target - glm::vec3(action->pxfAim->mat[3]);
    const glm::mat3 rotation = BuildRotateVectorsMatrix(action->vecAim, direction);
    static_cast<CTRLPOS*>(pg->pctrlF0C)->pos = glm::vec4(EulerFromMat(rotation), 0.0f);
}

// orig: 0x001758A0: the first of the guard's actions (+0xEDC) it can take on the target now (0: none)
ACTIONOBJ* PactionSelect(GUARD* pg, LO* ptarget, TARGETREC* pgate)
{
    static const bool s_fDebugEarly = std::getenv("SLY2N_SELDEBUG") != nullptr;
    if (s_fDebugEarly && ptarget && g_clock.t >= pg->tF04)
    {
        const glm::vec3 d = PosAi(ptarget) - PosOf(pg);
        std::fprintf(stderr, "select? guard %d t %.2f s %.0f dz %.0f F00 %d status %d gate %d (%zu actions)\n", pg->palo->oid, g_clock.t, std::sqrt(d.x * d.x + d.y * d.y),
                     d.z, pg->ploF00 != nullptr, NPlayerStatus77(ptarget), pgate ? (int)FTargetrecAllows(pgate, pg) : -1, pg->aploEDC.size());
    }
    if (pg->ploF00 || !ptarget || !ptarget->fInWorld) return nullptr;
    if (g_clock.t < pg->tF04) return nullptr;
    if (NPlayerStatus77(ptarget)) return nullptr;  // (a player busy: hit, held)
    if (pgate && !FTargetrecAllows(pgate, pg)) return nullptr;
    const glm::vec3 posG = PosOf(pg), posT = PosAi(ptarget);
    const glm::vec3 d = posT - posG;
    float sXy = std::sqrt(d.x * d.x + d.y * d.y);
    float dz = d.z;
    // (two objects of the same group: 75 apart, by their order)
    if (pg->nMode == 8 || pg->nMode == 0xD)
    {
        // TODO(ai): the +0xFC4 scale (sXy, dz divided by its +0x20)
    }
    for (LO* plo : pg->aploEDC)
    {
        ACTIONOBJ* pa = PactionOf(plo, pg);
        if (!pa->fA8) continue;
        static const bool s_fDebugRange = std::getenv("SLY2N_SELDEBUG") != nullptr;
        if (s_fDebugRange && (!FInRange(pa->r50, sXy) || !FInRange(pa->r58, dz)))
            std::fprintf(stderr, "select guard %d action %d out of range: s %.1f in [%.0f %.0f], dz %.1f in [%.0f %.0f]\n", pg->palo->oid, plo->oid,
                         sXy, pa->r50.x, pa->r50.y, dz, pa->r58.x, pa->r58.y);
        if (!FInRange(pa->r50, sXy)) continue;
        if (!FInRange(pa->r58, dz)) continue;
        // the target's direction in the guard's frame, its angle weighted by the distance
        const glm::mat4 m = pg->palo->apxfRef[2] ? pg->palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
        const glm::vec3 local = glm::vec3(glm::inverse(m) * glm::vec4(posT, 1.0f));
        const float rad = std::fabs(std::atan2(local.y, local.x));
        float w = AG_ANGLE_WEIGHT[0] + sXy * (AG_ANGLE_WEIGHT[1] + sXy * AG_ANGLE_WEIGHT[2]);
        w = std::clamp(w, 0.0f, 1.0f);  // (D_002BBB60 / 64)
        static const bool s_fDebug = std::getenv("SLY2N_SELDEBUG") != nullptr;
        if (s_fDebug)
            std::fprintf(stderr, "select guard %d action %d: s %.1f dz %.1f rad %.3f w %.3f limit %.3f\n", pg->palo->oid, plo->oid, sXy, dz,
                         rad, w, pa->g68);
        if (pa->g68 < w * rad) continue;
        if (pa->f70)
        {
            // the line from the guard to the target through the solid entries (0x20FAA0 with the filter 0x175820:
            // not bit 48, not a stepping object's own; with +0x74 not the class-0x62 objects): a hit nearer than +0x6C
            const glm::vec3 mn = glm::min(posG, posT), mx = glm::max(posG, posT);
            std::vector<SOE*> apsoe;
            for (OXA* poxa : g_physw.apoxaActive)
            {
                if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
                    mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
                    continue;
                for (SOE* psoe : poxa->apsoe)
                {
                    if (psoe->f48 || !psoe->palo) continue;
                    if (pa->f74 && FCidDerivedFrom(psoe->palo->Cid(), 0x62)) continue;
                    if (FCidDerivedFrom(psoe->palo->Cid(), 0x33)) continue;
                    if (apsoe.size() < 0x100) apsoe.push_back(psoe);
                }
            }
            LSG lsg;
            if (s_fDebug)
            {
                LSG l2;
                SOE* ph = apsoe.empty() ? nullptr : PsoeHitTestLineObjects(0, posG, posT, apsoe, &l2);
                std::fprintf(stderr, "   sight (%zu / %zu attachments): %zu entries, hit %d (%s oid %d) at %.1f\n", g_physw.apoxaActive.size(),
                             g_physw.apoxa.size(), apsoe.size(), ph != nullptr,
                             ph && ph->palo ? g_acls[ph->palo->Cid()].name : "", ph && ph->palo ? ph->palo->oid : -1,
                             ph ? glm::length(posG - l2.pos0) : -1.0f);
            }
            if (!apsoe.empty() && PsoeHitTestLineObjects(0, posG, posT, apsoe, &lsg))
                if (glm::length(posG - lsg.pos0) < pa->g6C) continue;
        }
        // the guard's splice event 0x18 (vt slot 3 with a result: 0x1331F8 / 0x133000) with the action's oid (type 6,
        // the entry's +0x18): a bound handler's value replaces the true CRef; anything but true refuses the action
        if (!FLoSpliceEventTrue(pg->palo, 0x18, plo->oid))
        {
            if (s_fDebug) std::fprintf(stderr, "   splice event 0x18 refuses action %d\n", plo->oid);
            continue;
        }
        if (pa->f78)
        {
            XF* targetFrame = nullptr;
            glm::vec3 targetPos(0.0f);
            if (auto* actor = dynamic_cast<ALO*>(ptarget)) targetFrame = PxfObj(actor);
            else if (auto* point = dynamic_cast<LOX*>(ptarget))
            {
                targetFrame = point->pxf;
                targetPos = glm::vec3(point->mat[3]);
            }
            SetGuardResponse(pg, pa, targetFrame, targetPos);
            if (pgate) SetTargetrecTimer(pgate, 3);
            return nullptr;
        }
        return pa;
    }
    return nullptr;
}

// ---- 0xA5: the pursuit ----

// orig: 0x00175758
void JOBPURSUE::Init()
{
    AddGuardState(pguard, 0x1D);
    st = -1;
}
// orig: 0x00175790
void JOBPURSUE::Enter()
{
    SetPursueState(3);
    SetGuardPursued(pguard, ptarget);
}
// orig: 0x001757D0
void JOBPURSUE::Exit()
{
    AIJOB::Exit();
    SetPursueState(-1);
    RemoveGuardState(pguard, 0x1D);
}

namespace
{
// the object record's world matrix (vt slot 31 0x1A6318, as guard.cpp's)
glm::mat3 MatRecord(const GUARD* pg)
{
    XF* pxf = pg->palo->apxfRef[2] ? pg->palo->apxfRef[2] : (pg->palo->axf.empty() ? nullptr : pg->palo->axf[0].get());
    return pxf ? glm::mat3(pxf->matWorld) : glm::mat3(1.0f);
}
}  // namespace

// orig: 0x00175D00: a shot at the target (the +0xF48 range, the +0xF50 / +0xF58 angles, the line clear)
bool JOBPURSUE::FCanShoot()
{
    GUARD* pg = pguard;
    if (static const bool s_f = std::getenv("SLY2N_SHOOTDEBUG") != nullptr; s_f)
        std::fprintf(stderr, "shoot0 guard %d t %.3f st %d FE8 %x F00 %d action %d\n", pg->palo->oid, g_clock.t, st, pg->grfFE8, pg->ploF00 != nullptr,
                     paction != nullptr);
    if (!(pg->grfFE8 & 8) || pg->ploF00 || paction) return false;
    if (!ptarget || !ptarget->fInWorld) return false;
    // a player busy (hit, held: vt slot 77 == 2) is not shot
    if (FCidDerivedFrom(ptarget->Cid(), 0x32) && NPlayerStatus77(ptarget) == 2) return false;
    // k: the target is in a state the guard shoots at at once (climbing, hanging, on a pipe, Sly's 3 / 21 / 22)
    bool k = false;
    if (FCidDerivedFrom(ptarget->Cid(), 0x34))
        if (ACTOR* pt = PactorFromLo(ptarget))
        {
            if (pt->nSub4CC != -1 && pt->n4C8 < 5) k = pt->n4C8 >= 3;
            if (pt->jump.state != -1 && pt->jump.state == 0x11) k = true;
            if (pt->nSubF54 != -1) k = true;
            if (FCidDerivedFrom(ptarget->Cid(), 0x35))
                if (SLY* psly = PslyFromLo(ptarget))
                    if (psly->state == 3 || psly->state == 0x15 || psly->state == 0x16) k = true;
        }
    const glm::vec3 pa = PosAi(pg->palo), pb = PosAi(ptarget);
    const glm::vec3 d = pb - pa;
    const float radHeading = RadGuardHeading(pg);  // 0x204740
    float radPan = 0, radTilt = 0, s = 0;
    cmm::ConvertVectorToSphere(d, &radPan, &radTilt, &s);  // 0x11EFB0
    if (!FInRange(glm::vec2(pg->gF48, pg->gF4C), s)) return false;
    if (!FInRange(glm::vec2(pg->radF50, pg->radF54), RadNormalize(radPan - radHeading))) return false;
    float radTiltOwn = 0;  // 0x11E958 of the record's x row (0x1A6318): its tilt
    {
        const glm::mat3 m = MatRecord(pg);
        const glm::vec3 x = m[0];
        radTiltOwn = RadNormalize(std::atan2(x.z, std::sqrt(x.x * x.x + x.y * x.y)));
    }
    if (!FInRange(glm::vec2(pg->radF58, pg->radF5C), RadNormalize(radTilt - radTiltOwn))) return false;
    if (!k)
    {
        if (st == 0)
        {
            if (g_clock.t - tSt < 3.0f) return false;
        }
        else if (st != 2)
            return false;
    }
    // the line: 0x20FAA0 with 0x20FA10 (not bit 48, not an attachment marked 4, not the guard's own), 0x14CC58: the
    // first entry it meets must be the target's
    FlushSwOxaDirty1();
    UpdateSwAaox();
    OXA* poxaOwn = Poxa(pg->palo);
    const glm::vec3 mn = glm::min(pa, pb), mx = glm::max(pa, pb);
    std::vector<SOE*> apsoe;
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* psoe : poxa->apsoe)
        {
            if (psoe->f48 || !psoe->palo) continue;
            OXA* po = Poxa(psoe->palo);
            if (po && (po->grf & 4)) continue;
            if (po == poxaOwn) continue;
            if (apsoe.size() < 0x100) apsoe.push_back(psoe);
        }
    }
    LSG lsg;
    SOE* ph = apsoe.empty() ? nullptr : PsoeHitTestLineObjects(0, pa, pb, apsoe, &lsg);
    if (static const bool s_f = std::getenv("SLY2N_SHOOTDEBUG") != nullptr; s_f)
        std::fprintf(stderr, "shoot? guard %d t %.3f st %d k %d s %.0f pan %.3f tilt %.3f hit %d\n", pg->palo->oid, g_clock.t, st, k, s,
                     RadNormalize(radPan - radHeading), RadNormalize(radTilt - radTiltOwn), ph && ph->palo ? ph->palo->oid : -1);
    if (ph && PloRoot(ph->palo) != ptarget) return false;
    return true;
}

// orig: 0x00175FF0
bool JOBPURSUE::FTryAction()
{
    GUARD* pg = pguard;
    paction = PactionSelect(pg, ptarget, pg->ptarget3CC);
    if (paction) return true;
    if (pg->tF04 < g_clock.t) pg->tF04 = g_clock.t + T_SELECT_RETRY;
    return false;
}

// orig: 0x00176060: the target lost (no hold, the guard's +0x3D4 clear, the record's timer old)
bool JOBPURSUE::FLost()
{
    if (f40) return false;
    if (pguard->n3D4) return false;
    return pguard->ptarget3CC && FTargetrecExpired(pguard->ptarget3CC);
}

// orig: 0x00176458
void JOBPURSUE::SetPursueState(int n)
{
    if (st == n) return;
    EndChildJob(&pchild);
    GUARD* pg = pguard;
    if (st == 1 && paction)
    {
        pg->tF04 = g_clock.t + GRandInRange(paction->r60.x, paction->r60.y);
        paction = nullptr;
    }
    st = n;
    tSt = g_clock.t;
    switch (n)
    {
    case 0: PjobPush(pg, 0xB3)->Enter(); break;
    case 1:
    {
        auto* o = static_cast<JOBACT*>(PjobPush(pg, 0xA4));
        o->ptarget = ptarget;
        o->paction = paction;
        pg->tF04 = FLT_MAX;  // (D_002A5408)
        o->Enter();
        if (pg->ptarget3CC) SetTargetrecTimer(pg->ptarget3CC, 3);
        break;
    }
    case 2:
    {
        auto* o = static_cast<JOBSHOOT*>(PjobPush(pg, 0xAD));
        o->ptarget = ptarget;
        o->Enter();
        if (pg->ptarget3CC) SetTargetrecTimer(pg->ptarget3CC, 4);
        break;
    }
    case 3:
    {
        auto* o = static_cast<JOBGOTO*>(PjobPush(pg, 0xBA));
        o->fC0 = 1;
        o->nAnim = 0x1C;
        const glm::vec3 pos = ptarget ? PosAi(ptarget) : PosOf(pg);
        GOAL g;
        g.pos = glm::vec4(pos, 0.0f);
        o->SetGoal(g);
        o->fAC = 1;
        o->plo = ptarget;
        o->Enter();
        posGoal = glm::vec4(pos, 0.0f);
        pregionGoal = PregionAt(pos, true);
        break;
    }
    case 4:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
        o->nAnim = 0xD;
        o->Enter();
        // (0x154038: the detector forgets it: its entry's state 0)
        if (pg->pdet)
            if (DETENTRY* pe = PdetentryFind(pg->pdet, ptarget)) SetDetentryState(pg->pdet, pe, 0);
        SetGuardPursued(pg, nullptr);
        break;
    }
    case 5:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->nAnim = 1;
        o->Enter();
        break;
    }
    default: break;
    }
}

// orig: 0x00176098
void JOBPURSUE::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    int prior;
    do
    {
        prior = st;
        switch (prior)
        {
        case 0: case 3:
        {
            if (FJobDone(this)) break;
            if (FGuardStill(pg)) { SetPursueState(5); break; }
            if (FLost()) { SetPursueState(4); break; }
            if (FTryAction()) { SetPursueState(1); break; }
            if (FCanShoot()) { SetPursueState(2); break; }
            const glm::vec3 posT = ptarget ? PosAi(ptarget) : PosOf(pg);
            const float s = DistXy(posT, PosOf(pg));  // (TODO(ai): divided by +0xFC4's scale in modes 8 / 0xD)
            NAVREGION* pregion = PregionAt(posT, true);
            NAVREGION* pregionG = PregionAt(PosOf(pg), false);
            if (static const bool s_f = std::getenv("SLY2N_PURSUEDEBUG") != nullptr; s_f)
                std::printf("pursue %d t %.3f st %d target (%.0f %.0f %.0f) me (%.0f %.0f %.0f) s %.0f close %.0f far %.0f region %p mine %p\n",
                            pg->palo->oid, g_clock.t, st, posT.x, posT.y, posT.z, PosOf(pg).x, PosOf(pg).y, PosOf(pg).z, s, pg->g314, pg->g318,
                            (void*)pregion, (void*)pregionG);
            if (st == 3)
            {
                if (s < pg->g314 && pregionG == pregion) { SetPursueState(0); break; }
                if (pregionGoal == pregion && glm::length(posT - glm::vec3(posGoal)) <= S_CHASE_RESEARCH) break;
                pregionGoal = pregion;
                posGoal = glm::vec4(posT, 0.0f);
                GOAL g;
                g.pos = glm::vec4(posT, 0.0f);
                if (auto* o = static_cast<JOBGOTO*>(pchild)) o->SetGoal(g);
            }
            else if (pg->g318 < s || pregionG != pregion) SetPursueState(3);
            break;
        }
        case 1:
            if (FGuardStill(pg)) SetPursueState(5);
            else if (!FJobDone(this) && pchild && static_cast<JOBACT*>(pchild)->st == -1) SetPursueState(0);
            break;
        case 2:
            if (FGuardStill(pg)) { SetPursueState(5); break; }
            if (!FJobDone(this) && pchild && static_cast<JOBSHOOT*>(pchild)->st == -1) SetPursueState(0);
            break;
        case 4:
            if (!FJobDone(this) && pchild && static_cast<JOBMOVE*>(pchild)->st == 1) SetPursueState(-1);
            break;
        case 5:
            if (!FJobDone(this) && !FGuardStill(pg)) SetPursueState(3);
            break;
        default: break;
        }
    } while (prior != st);
    // orig: 0x176098: per-guard repeat and the global Smoke deadline must both
    // have elapsed. This does not change sight, target records or pursuit state.
    if (pg->nF40 && prior >= 0 && prior < 4 && pg->tF44 <= g_clock.t && TGuardAlarmResume() <= g_clock.t && ptarget)
        RaiseGuardAlarm(pg, ptarget);
}

// ---- 0xA4: an action on the target ----

// orig: 0x001753D8
void JOBACT::Init()
{
    AddGuardState(pguard, 0x18);
    st = -1;
}
// orig: 0x00175410
void JOBACT::Enter()
{
    SetActState(0);
    LockJob(this);
}
// orig: 0x00175448
void JOBACT::Exit()
{
    AIJOB::Exit();
    SetActState(-1);
    RemoveGuardState(pguard, 0x18);
}
// orig: 0x00175488
void JOBACT::SetActState(int n)
{
    if (st == n) return;
    if (st == 0)
    {
        // (0x133448: no longer the target's listener; 0x20AC28(p, 0, 0, 0): the held attachment cleared)
        ptarget = ptarget;
    }
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    GUARD* pg = pguard;
    if (n == 0)
    {
        // (0x1333C0: the guard listens to the target (its message 0xB: the hit landed, Message))
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9F));
        o->ploFace = ptarget;
        // orig 0x20AE18 leaves the generated default action's +0xC0 null;
        // 0x175488 then lets JOBMOVE choose the guard's normal attack kind.
        o->pkinde = paction && paction->plo && paction->plo->oid != 0x2CC ? &paction->kinde : nullptr;
        if (static const bool s_fSel = std::getenv("SLY2N_SELDEBUG") != nullptr; s_fSel && paction)
            std::fprintf(stderr, "act guard %d t %.3f action %d kinde k %d oid %d (%s)\n", pg->palo->oid, g_clock.t, paction->plo ? paction->plo->oid : -1,
                         paction->kinde.k, paction->kinde.plo ? paction->kinde.plo->oid : paction->kinde.oid,
                         paction->kinde.plo ? g_acls[paction->kinde.plo->Cid()].name : "");
        // TODO(ai): +0xA0 (the action's attachment to the target, 0x20AC28)
        o->Enter();
    }
    else if (n == 1)
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
        o->nAnim = 4;
        o->ploFace = ptarget;
        o->Enter();
    }
    else if (n == -1)
        UnlockJob(this);
}
// orig: 0x00175600
void JOBACT::Update()
{
    AIJOB::Update();
    if (FJobDone(this)) return;
    if (std::getenv("SLY2N_ACTDEBUG") && pchild)
    {
        const JOBMOVE* o = static_cast<JOBMOVE*>(pchild);
        const KINDE* pe = o->pkinde;
        std::fprintf(stderr, "act: t %.2f guard %d st %d child st %d kinde k %d asega %p retracted %d\n", g_clock.t, pguard->palo->oid, st, o->st,
                     pe ? pe->k : -9, pe ? (void*)pe->pasega : nullptr, pe && pe->pasega ? (int)FAsegaRetracted(pe->pasega) : -1);
    }
    int prior;
    do
    {
        prior = st;
        if (prior == 0)
        {
            if (fHit) SetActState(1);
            else if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1) SetActState(-1);
        }
        else if (prior == 1 && pchild && static_cast<JOBMOVE*>(pchild)->st == 1)
            SetActState(-1);
    } while (prior != st);
}
// orig: 0x001756C0 (message 0xB from the target: its hit by this guard)
int JOBACT::Message(int msg, void* pm)
{
    // TODO(ai): kind 0xB whose +4 is the target and 0x174898 of it this guard: +0x40 (fHit)
    return AIJOB::Message(msg, pm);
}

// ---- 0x9F: the strike (a move with the action's entry) ----

// orig: 0x001751C0
void JOBSTRIKE::Init()
{
    JOBMOVE::Init();
    nAnim = 3;
}
// orig: 0x001751F0
void JOBSTRIKE::Enter()
{
    pguard->fF3C = 1;
    JOBMOVE::Enter();
    // a kind-2 entry (a leap at the target): its player ballistic (+0x60) and steep (+0x64), the target followed (+0x50)
    if (pkinde && pkinde->k == 2)
    {
        K2PLAY* pk = static_cast<K2PLAY*>(pkinde->pvState);
        if (pk)
        {
            pk->fBallistic = 1;
            pk->fSteep = 1;
        }
        f50 = 1;
    }
}
// orig: 0x00175258: the kind-2 player's target kept on the face object
void JOBSTRIKE::Update()
{
    JOBMOVE::Update();
    if (!pkinde || !pkinde->pvState) f50 = 0;
    if (f50 && ploFace) SetK2Target(static_cast<K2PLAY*>(pkinde->pvState), PosAi(ploFace));  // 0x20C6F8
}
// orig: 0x001752F0: the strike's events 0x2D / 0x2E (+0xF3C on / off), 0x30 (the target let go and the ballistic
// flight's constraint released, 0x20C760)
int JOBSTRIKE::Message(int msg, void* pm)
{
    if (msg == 9 && pkinde)
    {
        const ASEGMSG9* pev = static_cast<const ASEGMSG9*>(pm);
        if (PasegaOfKinde(pkinde) == pev->pasega)
        {
            switch (pev->n)
            {
            case 0x2D: pguard->fF3C = 1; return 1;
            case 0x2E: pguard->fF3C = 0; return 1;
            case 0x30:
                f50 = 0;
                if (pkinde->pvState) ReleaseK2Ballistic(static_cast<K2PLAY*>(pkinde->pvState));
                return 1;
            default: break;
            }
        }
    }
    return JOBMOVE::Message(msg, pm);
}
// orig: 0x001752C0
void JOBSTRIKE::Exit()
{
    JOBMOVE::Exit();
    pguard->fF3C = 0;
}

// ---- 0xB3: around the target ----

// orig: 0x0017BED0
void JOBSURROUND::Init()
{
    AddGuardState(pguard, 0x19);
    st = -1;
}
namespace
{
float RadNorm(float g) { return RadNormalize(g); }  // 0x11CF60
float GClampAbs(float g, float gLim) { return std::clamp(g, -gLim, gLim); }  // 0x11CFE0
float GClampLm(const glm::vec2& lm, float g) { return std::clamp(g, lm.x, lm.y); }  // 0x11D948
bool FInLm(const glm::vec2& lm, float g) { return lm.x <= g && g <= lm.y; }  // 0x11D910

// orig 0x001C0A40: the pieces of a-b on the region's walkable area (0x1C0960: its triangles' pieces, merged, side 2)
// outside its polygons (each polygon's free pieces 0x1BC688 intersected in, 0x14C9F8), as [t0, t1] along a-b
std::vector<glm::vec2> AtFreeNav(NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, int cMax)
{
    std::vector<glm::vec2> at;
    NAVSEG aseg[8];
    const int cseg = CsegNav(preg, a, b, std::min(cMax, 8), aseg, 2);
    for (int i = 0; i < cseg; ++i) at.emplace_back(aseg[i].t0, aseg[i].t1);
    if (at.empty()) return at;
    NAVPOLY* apoly[0x81];
    const int cpoly = std::min(CpolyNear(preg, a, b, 0x80, apoly), 0x80);
    for (int i = 0; i < cpoly && !at.empty(); ++i)
    {
        POLYHIT ahit[2];
        const int chit = ChitPoly(apoly[i], a, b, 2, ahit);
        std::vector<glm::vec2> atNew;
        for (const glm::vec2& r : at)
            for (int k = 0; k < chit; ++k)
            {
                const float t0 = std::max(r.x, ahit[k].t0), t1 = std::min(r.y, ahit[k].t1);
                if (t0 < t1 && static_cast<int>(atNew.size()) < cMax) atNew.emplace_back(t0, t1);
            }
        at = std::move(atNew);
    }
    return at;
}
}  // namespace

// orig: 0x0017BFF8: the guard's place around the target at posOrigin (its region preg, the reference heading
// radHeading): first the guard's own bearing turned toward the heading within the record's +0x14 at its distance
// clamped to the first range (+0x31C), kept on the walkable area (0x1C0B50) and accepted within +0x18 of the heading
// and the second range (+0x324); else rays around the heading (10 within +0x14 over the first range, 10 within +0x18
// over the second, 15 all around up to the second's far end; all around when the record's mode is -1): of their
// walkable pieces the point nearest the guard; else the origin kept on the area
glm::vec3 PosSurroundPlace(GUARD* pg, NAVREGION* preg, const glm::vec3& posOrigin, float radHeading)
{
    glm::vec2 lm1(pg->g31C, pg->g320), lm2(pg->g324, pg->g328);
    // TODO(ai): modes 8 / 0xD: both ranges scaled by +0xFC4's +0x20
    const TARGETREC* pt = pg->ptarget3CC;
    const float gNarrow = pt ? pt->g14 : 1.0f, gWide = pt ? pt->g18 : 2.0f;
    const int nOverride = pt ? pt->nMode : -1;
    const glm::vec3 posG = PosOf(pg);
    const glm::vec3 d = posG - posOrigin;
    const float radG = std::atan2(d.y, d.x), sG = std::sqrt(d.x * d.x + d.y * d.y);
    const float rad = radHeading + GClampAbs(RadNorm(radG - radHeading), gNarrow);
    const float s = GClampLm(lm1, sG);
    glm::vec3 posKept(0.0f);
    if (preg && FKeepOnNav(preg, posOrigin + glm::vec3(std::cos(rad) * s, std::sin(rad) * s, 0.0f), &posKept))
    {
        const glm::vec3 e = posKept - posOrigin;
        const float radK = std::atan2(e.y, e.x), sK = std::sqrt(e.x * e.x + e.y * e.y);
        if (std::fabs(RadNorm(radK - radHeading)) < gWide && FInLm(lm2, sK)) return posKept;
    }
    if (preg)
        for (int mode = 0; mode < 3; ++mode)
        {
            glm::vec2 lm;
            int c;
            float gLim;
            switch (mode)
            {
            case 0: lm = lm1; c = 10; gLim = gNarrow; break;
            case 1: lm = lm2; c = 10; gLim = gWide; break;
            default: lm = glm::vec2(0.0f, lm2.y); c = 15; gLim = 3.14159265f; break;
            }
            if (nOverride == -1) gLim = 3.14159265f;
            float sBest = FLT_MAX;
            glm::vec3 posBest(0.0f);
            const float mid = (c - 1) * 0.5f;
            for (int i = 0; i < c; ++i)
            {
                const float r = radHeading + (static_cast<float>(i) / mid - 1.0f) * gLim;
                const glm::vec3 dir(std::cos(r), std::sin(r), 0.0f);
                const glm::vec3 a = posOrigin + dir * lm.x, b = posOrigin + dir * lm.y;
                for (const glm::vec2& t : AtFreeNav(preg, a, b, 8))
                {
                    const glm::vec3 p0 = a + (b - a) * t.x, p1 = a + (b - a) * t.y;
                    // 0x11EE08: the piece's point nearest the point in $s7 (the original passes `por a0, zero, s7`,
                    // a register this function and its caller 0x17C3E8 never set: an ancestor's 0x2E0000, i.e. the
                    // denormal x 4.2e-39 = 0 on the PS2; RAM dump at 0x17C040 of the --interp-only reference, 2:1761
                    // vb 1654). So the place is the piece point nearest the world origin, not the guard
                    const glm::vec3 posNear(0.0f);
                    const glm::vec3 q = p1 - p0;
                    const float dq = glm::dot(q, q);
                    const float u = dq > 1e-6f ? std::clamp(glm::dot(posNear - p0, q) / dq, 0.0f, 1.0f) : 0.0f;
                    const glm::vec3 pn = p0 + q * u;
                    const float sd = glm::length(posNear - pn);
                    if (sd < sBest)
                    {
                        sBest = sd;
                        posBest = pn;
                    }
                }
            }
            if (sBest != FLT_MAX) return posBest;
        }
    if (preg && FKeepOnNav(preg, posOrigin, &posKept)) return posKept;
    return posOrigin;
}

// orig: 0x0017BF08: begin with a path from the guard's current position to itself. Update chooses its place
// around the target; 0x1C02D0's result does not choose the surround state.
void JOBSURROUND::Enter()
{
    AIJOB::Enter();
    GUARD* pg = pguard;
    if (pg->grfFE8 & 4)
    {
        const glm::vec3 pos = PosOf(pg);
        // The original reads the region through SOST +0xDC's area record (+0x10); native area membership is
        // currently queried from the same actual position (SOST::pvArea is not populated).
        NAVREGION* preg = PregionAt(pos, false);
        if (preg) FNavPathCrv(preg, pos, pos, &pg->crv294);  // 0x17BF78 -> 0x1C02D0
        SetSurroundState(1);
    }
    else SetSurroundState(0);
}
// orig: 0x0017BFB8
void JOBSURROUND::Exit()
{
    AIJOB::Exit();
    SetSurroundState(-1);
    RemoveGuardState(pguard, 0x19);
}
// orig: 0x0017C550
void JOBSURROUND::SetSurroundState(int n)
{
    if (st == n) return;
    EndChildJob(&pchild);
    st = n;
    GUARD* pg = pguard;
    if (n == 0)
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->ploFace = pg->ptarget3CC ? pg->ptarget3CC->plo : nullptr;
        o->Enter();
    }
    else if (n == 1)
    {
        // 0xA7 along the way to the place, its move kind 8
        auto* o = static_cast<JOBFOLLOW*>(PjobPush(pg, 0xA7));
        o->Follow(&pg->crv294, 0);
        o->kMove = 8;
        o->ploArg = pg->ptarget3CC ? pg->ptarget3CC->plo : nullptr;  // original 0x17C610: face it while strafing
        o->Enter();
    }
}
// orig: 0x0017C3E8: replan when the follower is too far from its path, or the endpoint leaves the target's
// allowed angle/range. Retarget the existing follower without restarting its move or animation.
void JOBSURROUND::Update()
{
    AIJOB::Update();
    if (st == 0) return;
    GUARD* pg = pguard;
    TARGETREC* pt = pg->ptarget3CC;
    auto* o = static_cast<JOBFOLLOW*>(pchild);
    if (!pt || !pt->plo || !o) return;
    const glm::vec3 posG = PosOf(pg), posT = PosAi(pt->plo);
    NAVREGION* preg = PregionAt(posG, false);
    const float rad = RadTargetrec(pt);
    CRV* pcrv = &pg->crv294;
    bool fReplan = S_SURROUND_REPLAN < o->sTarget;  // follower +0x70, 0x17C470
    if (!fReplan && !pcrv->apos.empty())
    {
        const glm::vec3 e = pcrv->apos.back() - posT;
        const float radE = std::atan2(e.y, e.x), sE = std::sqrt(e.x * e.x + e.y * e.y);
        fReplan = pt->g18 < std::fabs(RadNormalize(radE - rad)) || !FInRange(glm::vec2(pg->g324, pg->g328), sE);
    }
    if (fReplan)
    {
        const glm::vec3 pos = PosSurroundPlace(pg, preg, posT, rad);
        if (std::getenv("SLY2N_SURRDEBUG"))
            std::printf("surround: t %.3f guard %d at %.0f %.0f target %.0f %.0f heading %.2f mode %d -> place %.0f %.0f (%.0f from the target)\n",
                        g_clock.t, pg->palo->oid, posG.x, posG.y, posT.x, posT.y, rad, pt->nMode, pos.x, pos.y,
                        glm::length(glm::vec2(pos - posT)));
        if (preg) FNavPathCrv(preg, posG, pos, pcrv);  // 0x17C510: a failed query retains the old path
        if (std::getenv("SLY2N_SURRDEBUG"))
        {
            std::printf("surround path (%zu):", pcrv->apos.size());
            for (const glm::vec3& p : pcrv->apos) std::printf(" (%.2f %.2f %.2f)", p.x, p.y, p.z);
            std::printf("\n");
        }
        o->Follow(pcrv, 0);                           // 0x17C520 -> 0x178D90; preserve A7 / 9D / K3
    }
}

// the factory's kinds of this module

// ---- 0xB4 / 0xB5: the boss (class 0x3B) ----
namespace
{
using BOSSREC = GUARD::BOSSREC;
// orig: 0x00155868: the record whose nearer end (A or C) is farthest from the target (D_002BDC50 is 1 in the
// original's data, so used records stay eligible)
BOSSREC* PrecFarthest(GUARD* pg, LO* ptarget)
{
    BOSSREC* best = nullptr;
    float gBest = -FLT_MAX;
    if (!ptarget) return nullptr;
    const glm::vec3 pos = PosLoWorld(ptarget);
    for (BOSSREC& r : pg->abossrec)
    {
        if (!r.aplo[0] || !r.aplo[2]) continue;
        const float g = std::min(glm::length(PosLoWorld(r.aplo[0]) - pos), glm::length(pos - PosLoWorld(r.aplo[2])));
        if (gBest < g) { gBest = g; best = &r; }
    }
    return best;
}
}  // namespace

// orig: 0x00155560
void JOBB4::Init()
{
    AddGuardState(pguard, 0x22);
    st = -1;
}
// orig: 0x001555E0
void JOBB4::Enter()
{
    AIJOB::Enter();
    SetGuardPursued(pguard, ptarget);
    if (pguard->n1018 - pguard->n33C >= 90) prec = PrecFarthest(pguard, ptarget);
    if (std::getenv("SLY2N_BOSSPRINT"))
    {
        const glm::vec3 pt = ptarget ? PosLoWorld(ptarget) : glm::vec3(0.0f);
        std::printf("boss b4: t %.2f enter hp %d (1018 %d) rec %d target (%.0f %.0f %.0f)\n", g_clock.t, pguard->n33C, pguard->n1018,
                    prec ? int(static_cast<BOSSREC*>(prec) - pguard->abossrec.data()) : -1, pt.x, pt.y, pt.z);
        for (size_t i = 0; i < pguard->abossrec.size(); ++i)
        {
            const BOSSREC& r = pguard->abossrec[i];
            const glm::vec3 a = r.aplo[0] ? PosLoWorld(r.aplo[0]) : glm::vec3(0.0f), c = r.aplo[2] ? PosLoWorld(r.aplo[2]) : glm::vec3(0.0f);
            std::printf("boss rec %zu: A %d (%.0f %.0f %.0f) C %d (%.0f %.0f %.0f) used %d\n", i, r.aoid[0], a.x, a.y, a.z, r.aoid[2], c.x, c.y, c.z, r.fUsed);
        }
    }
    SetB4State(prec ? 0 : 1);
}
// orig: 0x00155598
void JOBB4::Exit()
{
    AIJOB::Exit();
    SetB4State(-1);
    RemoveGuardState(pguard, 0x22);
    SetGuardPursued(pguard, nullptr);
}
// orig: 0x00155788
void JOBB4::SetB4State(int k)
{
    if (k == st) return;
    if (st == 0 && prec) static_cast<BOSSREC*>(prec)->fUsed = 1;
    if (std::getenv("SLY2N_BOSSPRINT")) std::printf("boss b4: t %.2f state %d -> %d\n", g_clock.t, st, k);
    EndChildJob(&pchild);
    st = k;
    tSt = g_clock.t;
    switch (k)
    {
    case 0:
    {
        auto* o = static_cast<JOBB5*>(PjobPush(pguard, 0xB5));
        o->prec = prec;
        o->ptarget = ptarget;
        o->Enter();
        pguard->n1018 = pguard->n33C;
        break;
    }
    case 1:
    {
        auto* o = static_cast<JOBPURSUE*>(PjobPush(pguard, 0xA5));
        o->ptarget = ptarget;
        o->f40 = k;
        o->Enter();
        break;
    }
    default: break;
    }
}
// orig: 0x00155660
void JOBB4::Update()
{
    AIJOB::Update();
    while (!FJobDone(this))
    {
        const int s0 = st;
        if (st == 0 && ptarget)
        {
            const glm::vec3 d = PosLoWorld(pguard->palo) - PosLoWorld(ptarget);
            const auto* o = dynamic_cast<const JOBB5*>(pchild);
            if (std::sqrt(d.x * d.x + d.y * d.y) < 850.0f && std::fabs(d.z) < 400.0f && o && o->st != 0) SetB4State(1);
        }
        if (st == s0) return;
    }
}

// orig: 0x0017B808
void JOBB2::Init()
{
    AddGuardState(pguard, 0x20);  // 0x209890
    st = -1;
}
// orig: 0x0017B840
void JOBB2::Enter()
{
    AIJOB::Enter();
    SetB2State(0);
}
// orig: 0x0017B870
void JOBB2::Exit()
{
    AIJOB::Exit();
    SetB2State(-1);
    RemoveGuardState(pguard, 0x20);  // 0x209970
}
// orig: 0x0017B8B0
void JOBB2::SetB2State(int n)
{
    if (n == st) return;
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    switch (n)
    {
    case 0:
        PjobPush(pguard, 0x9E)->Enter();
        SetGuardPursued(pguard, nullptr);  // 0x208540(guard, 0)
        break;
    case 1:
    {
        auto* o = static_cast<JOBPURSUE*>(PjobPush(pguard, 0xA5));
        o->ptarget = ploSelected;
        o->f40 = 1;
        o->Enter();
        break;
    }
    case 2:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9E));
        o->nAnim = 1;
        o->Enter();
        break;
    }
    default: break;
    }
}
namespace
{
// orig: 0x0011ED40 (the segment a -> b's parameter of the point nearest p, unclamped, and that point's distance)
void ParamSegPoint(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float* pt, float* pd)
{
    const glm::vec3 ab = b - a;
    const float l2 = glm::dot(ab, ab);
    const float t = l2 > 0.0f ? glm::dot(p - a, ab) / l2 : 0.0f;
    *pt = t;
    *pd = glm::length(a + ab * t - p);
}
}  // namespace
// orig: 0x0017B9C0
void JOBB2::Choose()
{
    const float now = g_clock.t;
    if (now < tChoose) return;
    tChoose = now + GRandInRange(1.0f, 2.0f);  // D_002BE710
    GUARD* pg = pguard;
    const glm::vec3 origin = PosAloCenter(pg->palo);  // 0x1A62E8
    LO* pbest = nullptr;
    glm::vec3 posBest(0.0f);
    float costBest = FLT_MAX;  // D_002A55FC
    for (int i = 0; i < pg->fF94 && i < 4; ++i)
    {
        LO* pc = pg->aploF98[i];
        if (!pc || !pc->fInWorld) continue;
        const glm::vec3 cand = FCidDerivedFrom(pc->Cid(), 0x30) ? PosAloCenter(static_cast<ALO*>(pc)) : PosLoWorld(pc);
        float cost = glm::length(origin - cand);
        if (pc == ploPreferred) cost = -300.0f + cost * (0.9f + cost * 0.0f);  // D_002BE720
        for (int j = 0; j < pg->fFA8 && j < 4; ++j)
        {
            LO* pn = pg->aploFAC[j];
            if (!pn) continue;
            float t, d;
            ParamSegPoint(FCidDerivedFrom(pn->Cid(), 0x30) ? PosAloCenter(static_cast<ALO*>(pn)) : PosLoWorld(pn), origin, cand, &t, &d);
            if (t < 0.0f || t > 1.0f) continue;
            float pen = 600.0f + d * (-2.0f + d * 0.0f);  // D_002BE730
            pen = std::clamp(pen, 0.0f, 600.0f);           // D_002BE740
            cost += pen;
        }
        if (cost < costBest)
        {
            posBest = cand;
            costBest = cost;
            pbest = pc;
        }
    }
    ploPreferred = pbest;
    LO* pnearest = nullptr;
    float dist = 800.0f;
    for (int k = 0; k < pg->fFA8 && k < 4; ++k)
    {
        LO* pc = pg->aploFAC[k];
        if (!pc) continue;
        const glm::vec3 pos = FCidDerivedFrom(pc->Cid(), 0x30) ? PosAloCenter(static_cast<ALO*>(pc)) : PosLoWorld(pc);
        const float f = glm::length(pos - origin);
        if (f > dist) continue;
        if (pbest)
        {
            float t, d;
            ParamSegPoint(pos, origin, posBest, &t, &d);
            if (t < 0.0f || t > 1.0f || d > 500.0f) continue;
        }
        pnearest = pc;
        dist = f;
    }
    ploNear = pnearest;
    ploSelected = pnearest ? pnearest : ploPreferred;
    if (std::getenv("SLY2N_B2PRINT"))
        std::printf("b2 guard %p t %.3f: preferred %d near %d selected %d state %d\n", (void*)pg, now, ploPreferred ? ploPreferred->oid : -1,
                    ploNear ? ploNear->oid : -1, ploSelected ? ploSelected->oid : -1, st);
}
// orig: 0x0017BD78
void JOBB2::Update()
{
    AIJOB::Update();
    const bool fStill = FGuardStill(pguard);  // 0x209098
    Choose();
    while (!FJobDone(this))
    {
        const int s = st;
        switch (s)
        {
        case 0:
            if (fStill) SetB2State(2);
            else if (ploSelected) SetB2State(1);
            break;
        case 1:
        {
            const auto* o = dynamic_cast<const JOBPURSUE*>(pchild);
            if (fStill) SetB2State(2);
            else if (!ploSelected) SetB2State(0);
            else if (!o || ploSelected != o->ptarget)
            {
                SetB2State(-1);
                SetB2State(1);
            }
            else if (o->st == -1) SetB2State(0);  // 0x175810
            break;
        }
        case 2:
            if (!fStill) SetB2State(0);
            break;
        default: break;
        }
        if (s != st) continue;
        break;
    }
}

// orig: 0x001559B0
void JOBB5::Init()
{
    AddGuardState(pguard, 0x23);
    st = -1;
    g3C = FLT_MAX;
}
// orig: 0x00155A38
void JOBB5::Enter()
{
    AIJOB::Enter();
    SetB5State(0);
}
// orig: 0x001559F8
void JOBB5::Exit()
{
    AIJOB::Exit();
    SetB5State(-1);
    RemoveGuardState(pguard, 0x23);
}
// orig: 0x00155A68: the end whose zone holds the target (A for zone B, C for zone D), else A
LO* JOBB5::PloEnd()
{
    const BOSSREC* r = static_cast<const BOSSREC*>(prec);
    if (!r) return nullptr;
    const glm::vec3 pos = ptarget ? PosLoWorld(ptarget) : glm::vec3(0.0f);
    if (std::getenv("SLY2N_BOSSPRINT"))
        std::printf("boss end: t %.2f target (%.0f %.0f %.0f) zone B %d (%d) D %d (%d)\n", g_clock.t, pos.x, pos.y, pos.z,
                    r->aplo[1] ? int(FPointInShape(r->aplo[1], pos)) : -1, r->aoid[1], r->aplo[3] ? int(FPointInShape(r->aplo[3], pos)) : -1, r->aoid[3]);
    if (r->aplo[1] && FPointInShape(r->aplo[1], pos)) return r->aplo[0];
    if (r->aplo[3] && FPointInShape(r->aplo[3], pos)) return r->aplo[2];
    return r->aplo[0];
}
// orig: 0x00155AE0
void JOBB5::SetB5State(int k)
{
    if (k == st) return;
    GUARD* pg = pguard;
    switch (st)
    {
    case 1: break;  // TODO(ai): 0x20AC28(guard, 0, 0, 0) (the held attachment cleared; not ported, as in 0xA4)
    case 3:
        if (paction) pg->tF04 = g_clock.t + GRandInRange(paction->r60.x, paction->r60.y);
        paction = nullptr;
        break;
    default: break;
    }
    EndChildJob(&pchild);
    if (std::getenv("SLY2N_BOSSPRINT"))
    {
        const glm::vec3 pp = PosLoWorld(pg->palo);
        std::printf("boss b5: t %.2f state %d -> %d at (%.0f %.0f %.0f)\n", g_clock.t, st, k, pp.x, pp.y, pp.z);
    }
    st = k;
    tSt = g_clock.t;
    switch (k)
    {
    case 0:
    {
        ploPoint = PloEnd();
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
        o->nAnim = 0xB;
        if (ploPoint)
        {
            const glm::vec3 d = PosLoWorld(ploPoint) - PosLoWorld(pg->palo);
            SetMoveHeading(&pg->move, std::atan2(d.y, d.x) + 3.1415927f);
        }
        o->Enter();
        g3C = FLT_MAX;
        break;
    }
    case 1:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->ploFace = ptarget;
        o->Enter();
        break;
    }
    case 2:
    {
        auto* o = static_cast<JOBFOLLOW*>(PjobPush(pg, 0xA7));
        CRV* b = &pg->crv294;
        // (+0xE8 = 2: the curve's two points; its kind +0xE0 and closed flag stay the guard's)
        b->apos = {PosLoWorld(pg->palo), ploPoint ? PosLoWorld(ploPoint) : PosLoWorld(pg->palo)};
        b->ag = {0.0f, 1.0f};
        RecalcCrv(b, glm::mat4(1.0f));  // 0x1E6180
        o->Follow(b, 0);                // 0x178D90
        o->kMove = 8;
        o->ploArg = ptarget;
        o->fNoAvoid = 1;
        o->Enter();
        break;
    }
    case 3:
    {
        auto* o = static_cast<JOBACT*>(PjobPush(pg, 0xA4));
        if (std::getenv("SLY2N_BOSSPRINT")) std::printf("boss b5: t %.2f action %p kinde k %d oid %d\n", g_clock.t, (void*)paction, paction ? paction->kinde.k : -9, paction ? paction->kinde.oid : -9);
        o->paction = paction;
        o->ptarget = ptarget;
        o->Enter();
        pg->tF04 = FLT_MAX;
        ++cAction;
        break;
    }
    default: break;
    }
}
// orig: 0x00155D90
void JOBB5::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    while (!FJobDone(this))
    {
        const int s0 = st;
        switch (st)
        {
        case 0:
            if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1)  // 0x174FB8
            {
                // vt slots 29 / 35 / 36: placed at the end (the step's base +0x240 below it), still
                if (STEP* pstep = PstepFromLo(pg->palo); pstep && ploPoint)
                    SetStepPos(pstep, PosLoWorld(ploPoint) - glm::vec3(StepPhysOf(pg->palo).vecBase));
                SetAloVelocityTo(pg->palo, glm::vec3(0.0f));
                SetAloAngVelocityTo(pg->palo, glm::vec3(0.0f));
                t48 = g_clock.t;
                SetB5State(1);
            }
            break;
        case 2:
            if (pchild && static_cast<JOBFOLLOW*>(pchild)->FArrived()) SetB5State(1);  // 0x178D00
            break;
        case 1:
            if (std::getenv("SLY2N_BOSSPRINT"))
            {
                static int s_c = 0;
                if (s_c++ % 60 == 0)
                    std::printf("boss b5 face: t %.2f heading %.3f goal %.3f locked %d child %p\n", g_clock.t, RadGuardHeading(pg),
                                StepPhysOf(pg->palo).radHeading, pg->move.fHeadingLocked, (void*)pchild);
            }
            if (2.5f < g_clock.t - t48 && 0.1f < g_clock.t - tSt)
                if ((paction = PactionSelect(pg, ptarget, nullptr)) != nullptr) SetB5State(3);  // 0x1758A0
            break;
        case 3:
            t44 = g_clock.t;
            if (pchild && static_cast<JOBACT*>(pchild)->st == -1)  // 0x175748
            {
                int k = 1;
                LO* n = PloEnd();
                if (n != ploPoint) { ploPoint = n; k = 2; }
                SetB5State(k);
            }
            break;
        default: break;
        }
        if (st == s0) return;
    }
}

// ---- 0xAD: the shot ----

// orig: 0x0017B590
void JOBSHOOT::Init()
{
    AIJOB::Init();
    st = -1;
}
// orig: 0x0017B5C0
void JOBSHOOT::Enter()
{
    AIJOB::Enter();
    SetShootState(0);
}
// orig: 0x0017B5F0
void JOBSHOOT::Exit()
{
    AIJOB::Exit();
    SetShootState(-1);
}
// orig: 0x0017B620
void JOBSHOOT::Update()
{
    AIJOB::Update();
    while (!FJobDone(this))
    {
        const int s = st;
        if (s == 0 && pchild && static_cast<JOBMOVE*>(pchild)->st == 1) SetShootState(-1);
        if (s == st) break;
    }
}
// orig: 0x0017B690
int JOBSHOOT::Message(int msg, void* pm)
{
    if (msg == 9 && st == 0 && pchild && static_cast<JOBMOVE*>(pchild)->pkinde)
    {
        const ASEGMSG9* pev = static_cast<const ASEGMSG9*>(pm);
        if (PasegaOfKinde(static_cast<JOBMOVE*>(pchild)->pkinde) == pev->pasega)
        {
            if (pev->n == 0x31) { pguard->fF60 = 1; return 1; }
            if (pev->n == 0x32) { pguard->fF60 = 0; return 1; }
        }
    }
    return AIJOB::Message(msg, pm);
}
// orig: 0x0017B750
void JOBSHOOT::SetShootState(int n)
{
    if (n == st) return;
    EndChildJob(&pchild);
    if (st == 0) pguard->fF60 = 0;
    st = n;
    tSt = g_clock.t;
    if (n == 0)
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9D));
        o->nAnim = 0x11;
        o->ploFace = ptarget;
        o->Enter();
        pguard->fF60 = 1;
    }
}

std::unique_ptr<AIJOB> PjobNewPursuit(CID cid)
{
    switch (cid)
    {
    case 0xAD: return std::make_unique<JOBSHOOT>();
    case 0xA5: return std::make_unique<JOBPURSUE>();
    case 0xA4: return std::make_unique<JOBACT>();
    case 0x9F: return std::make_unique<JOBSTRIKE>();
    case 0xB3: return std::make_unique<JOBSURROUND>();
    case 0xB2: return std::make_unique<JOBB2>();
    case 0xB4: return std::make_unique<JOBB4>();
    case 0xB5: return std::make_unique<JOBB5>();
    default: return PjobNewHit(cid);  // (aihit.cpp)
    }
}

