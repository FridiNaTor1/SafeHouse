// The trigger state objects (module 0x148668..0x14A1B0; Sly2-Reference game/m_148668_p3.cpp, the original's code for
// 0x1487A0 / 0x148860 / 0x1488D8): a state object embedded in game objects ({+0x8C, +0x90 state, +0x94 its time,
// +0x9C no sound, +0xA0, +0xA4 stays at 1, +0xA8 the object that set it, +0xAC a script, lists}) driven by the
// volume triggers (class 0x18 family, the state at +0x130, update 0x149440): an object entering the volume sets
// state 1 (0x1487A0: the owner's splice event 5, the sound 2, the enter list: ASEGs, state machine goals, the script
// started, task states; then state 2 unless +0xA4), leaving sets 0 (0x148860: event 6 and the leave list).
// The class 0x53 pad update is in pad.cpp; update.cpp owns the deferred-call queue.

#include "activate.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "animfind.h"
#include "aseg.h"
#include "asega.h"
#include "bsp.h"
#include "clock.h"
#include "cls.h"
#include "crv.h"
#include "lo.h"
#include "objmethod.h"
#include "pair.h"
#include "po.h"
#include "scpt.h"
#include "smrun.h"
#include "spliceobj.h"
#include "spliceglue.h"
#include "so.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "trigger.h"
#include "update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

// orig: 0x00148668 / 0x001488D8: resolve each authored ASEG around its owner, then ensure it on the
// corresponding root. Pads embed the state at +0x150 (EOPIDs 415/416); volumes use +0x130 (430/431).
void RunTriggerAsegList(LO* plo, int i)
{
    const bool fPad = FCidDerivedFrom(plo->Cid(), S3CID(0x53));
    const uint32_t ibState = fPad ? 0x150u : 0x130u;
    const int eopid = (fPad ? 415 : 430) + i;
    const uint32_t uSpeed = plo->U32Field(0, ibState + 0x8C, 0x3F800000u);  // 0x148718: speed 1
    float svt;
    std::memcpy(&svt, &uSpeed, sizeof(svt));
    const int seek = plo->U32Field(0, ibState + 0xA0, 0) ? (svt < 0.0f ? 2 : 0) : 1;
    int c = 0;
    for (const OPT& opt : plo->aopt)
    {
        if (opt.eopid != eopid || opt.aval.size() < 2) continue;
        if (c++ >= 8) break;  // 0x148620: at most eight entries in each list
        auto* paseg = dynamic_cast<ASEG*>(PloFindSwNearest(plo, opt.aval[0].n));
        if (!paseg) continue;
        const OID oidRoot = PloRoot(paseg)->oid;  // original ASEG +0x14's root object
        LO* po = plo->oid == oidRoot ? plo : oidRoot != -1 ? PloFindSwNearest(plo, oidRoot) : nullptr;
        PasegaEnsureAseg(paseg, dynamic_cast<ALO*>(po), seek, 0.0f, svt, opt.aval[1].n);
    }
}

namespace
{
struct TRIGSTATE : TRIGGERSTATE
{
    std::vector<std::array<OID, 4>> arsmg;  // +0xB0 / +0xB4: {root, sm, trigger state, no-trigger state}
    std::vector<std::pair<OID, int>> atask; // +0x134 / +0x138
    std::vector<int> aidObj;  // +0x288 (EOPID 432)
    std::vector<LO*> aploObj; // +0x28C entries added at run time (EOPID 432's setter 0x1497B8: an object, after the loaded ones)
    std::vector<int> acid;    // +0x2AC (EOPID 433)
    std::vector<LO*> aploInside; // +0x2C0 / +0x2C4: ordered tracked members (EOPID 444)
    bool fInit = false;
};
std::unordered_map<LO*, TRIGSTATE> s_mptrig;
WORLD_TABLE(s_mptrig);   // (PC: moved aside while a kept world loads, worldglobals.h)
SW* s_psw = nullptr;

const OPT* PoptFind(const LO* plo, int eopid)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == eopid) return &o;
    return nullptr;
}
int NOpt(const LO* plo, int eopid, int nDefault)
{
    const OPT* p = PoptFind(plo, eopid);
    return p && !p->aval.empty() ? p->aval[0].n : nDefault;
}

// the volume's options (cid 0x18: EOPIDs 432 ids, 433 classes, 436..438 +0x1CC..+0x1D4, 441 the script +0x1DC,
// 443 +0x2C8, 439 / 440 the methods 0x105440 / 0x105508 (registered below))
void InitTrigger(LO* plo, TRIGSTATE& t)
{
    t.owner = plo;
    t.fInit = true;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        // 0x1497B8: the loaded ids. A script's set of option 519 before the first update (Ep7 32:3234's module 139
        // pointing each of Bison's three triggers at its own gun's harpoon) is in aploObj already: the option it left
        // here would add the id again, resolved to the nearest copy (the same harpoon for all three triggers, so one
        // shot fired all three: R43); the original's list holds only the object (+0x288 count 1)
        if (o.eopid == S3EOPID(432) && std::none_of(t.aploObj.begin(), t.aploObj.end(), [&](const LO* p) { return p && p->oid == o.aval[0].n; }))
            t.aidObj.push_back(o.aval[0].n);
        if (o.eopid == S3EOPID(433)) t.acid.push_back(o.aval[0].n);     // 0x1497E0
    }
    // orig: 0x00149358: a tracked volume remains held while members are inside.
    if (plo->U32Field(0, 0x2C8, 0)) plo->SetField(0, 0x1D4, 1);
    t.state = 0;  // orig 0x149398 (post-load: 0x149718(p, 0) from -1: nothing run)
    t.tState = g_clock.t;
}

// orig 0x0020FF70 / 0x0020FFF0: each state machine run to the entry's trigger (or no-trigger) state. The root is
// looked up around the trigger (0x15E650 kind 0x204) and must be the only nearest match: the intro's six police cars
// share the oids 150730 / SM 150732, each with its own trigger; a global lookup sent every trigger to the first car.
void ApplyRsmg(LO* plo, const TRIGSTATE& t, bool fTrigger)
{
    for (const auto& e : t.arsmg)
    {
        LO* po = nullptr;
        const int c = CploFindSwNearest(plo, e[0], &po);
        if (std::getenv("SLY2N_RSMGDEBUG"))
        {
            std::printf("rsmg trigger %d (chain", plo->oid);
            for (const LO* p = plo->paloParent; p; p = p->paloParent) std::printf(" %d", p->oid);
            std::printf(") root %d sm %d -> %d / %d: %d found, first %d (chain", e[0], e[1], e[2], e[3], c, po ? po->oid : -1);
            for (const LO* p = po ? po->paloParent : nullptr; p; p = p->paloParent) std::printf(" %d", p->oid);
            std::printf(")\n");
        }
        if (c != 1) continue;
        if (!FCidDerivedFrom(po->Cid(), S3CID(0x30))) continue;
        const OID oid = fTrigger ? e[2] : e[3];
        if (oid == -1) continue;
        for (SMA* psma : g_apsma)
            if (psma->palo == po && (e[1] == -1 || (psma->psm && psma->psm->oid == e[1])))
            {
                SetSmaGoal(psma, oid);
                break;
            }
    }
}

// orig 0x001488D8 (list i: 0 enter, 1 leave)
void RunTriggerList(LO* plo, TRIGSTATE& t, int i)
{
    RunTriggerAsegList(plo, i);  // +4 / +0x48, before state machines, script and tasks
    ApplyRsmg(plo, t, i == 0);
    if (i != 0) return;
    const uint32_t ibScript = FCidDerivedFrom(plo->Cid(), S3CID(0x53)) ? 0x1FCu : 0x1DCu;
    const OID oidScpt = static_cast<OID>(plo->U32Field(0, ibScript, uint32_t(-1)));
    if (oidScpt != -1)
        // Original 0x13E818 object fields resolve through PostLoadOidField
        // with kind 4 and this owner, preserving each instance's script.
        if (LO* pscpt = PloFindRelative(plo, oidScpt); pscpt && FCidDerivedFrom(pscpt->Cid(), S3CID(0x72)))
        {
            if (std::getenv("SLY2N_TASKDEBUG")) std::printf("trigger %d: script %d started\n", plo->oid, oidScpt);
            StartScpt(pscpt, t.ploFound);
        }
    if (!t.atask.empty()) ApplyCheckpointTasks(g_psw, plo, t.atask);
}

// orig 0x00148A78 (+ 0x1487A0 / 0x148860)
void SetTriggerState(LO* plo, TRIGSTATE& t, int n)
{
    const int old = t.state;
    if (old == n) return;
    const bool f1 = old == 0 && n == 1, f0 = old != -1 && n == 0;
    t.state = n;
    t.tState = g_clock.t;
    if (const char* selected = std::getenv("SLY2N_TRIGGERSTATE"); selected && plo->oid == std::atoi(selected))
        std::printf("TRIGGERSTATE,%.9g,%d,%d,%d,%d\n", g_clock.t, plo->oid,
                    plo->paloParent && plo->paloParent->paloParent ? plo->paloParent->paloParent->oid : -1, old, n);
    if (std::getenv("SLY2N_TASKDEBUG")) std::printf("trigger %d: state %d -> %d\n", plo->oid, old, n);
    if (f1)
    {
        HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
        BroadcastTaskEvent(g_psw, 0x1A, plo);  // orig 0x1A61E0: owner event before the enter list
        if (!plo->U32Field(0, 0x1CC, 1))
        {
            CSnd* p = PsndNewPlain(2);
            p->SetSource(plo);
        }
        RunTriggerList(plo, t, 0);
        if (!plo->U32Field(0, 0x1D4, 0)) SetTriggerState(plo, t, 2);
        NotifyLo(plo, 6, static_cast<TRIGGERSTATE*>(&t));
    }
    if (f0)
    {
        HandleLoSpliceEvent(plo, 6, false, 0, nullptr);
        BroadcastTaskEvent(g_psw, 0x1B, plo);  // orig 0x1A61E0: owner event before the leave list
        RunTriggerList(plo, t, 1);
        NotifyLo(plo, 7, static_cast<TRIGGERSTATE*>(&t));
    }
}

}  // namespace
bool FPointInShape(LO* plo, const glm::vec3& pos);  // detect.cpp (0x227C90)
namespace {
bool FInsideVolume(LO* plo, const glm::vec3& pos)
{
    const LOXSHAPE* pshape = dynamic_cast<const LOXSHAPE*>(plo);
    for (const LO* p = plo; pshape && pshape->bsp.abspn.empty() && p->ploCloneOf; p = p->ploCloneOf)
        if (const LOXSHAPE* q = dynamic_cast<const LOXSHAPE*>(p->ploCloneOf)) pshape = q;
    if (!pshape || pshape->bsp.abspn.empty()) return false;
    const glm::vec3 q = glm::vec3(glm::inverse(MatWorldLox(plo)) * glm::vec4(pos, 1.0f));
    const BSPREF ref(pshape->bsp, pshape->geom);
    return IbspFindPoint(ref, 0, ref.cnode, q) != IBSP_NONE;
}

// orig 0x00149440 (the volume trigger's update: kinds 0 / 1 the objects inside, 2 a time-out)
void UpdateVolumeTrigger(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1A5E28)) pfn(plo);
    s_psw = g_psw;
    TRIGSTATE& t = s_mptrig[plo];
    if (std::getenv("SLY2N_TRIGDBG") && plo->oid == std::atoi(std::getenv("SLY2N_TRIGDBG")))
        std::printf("trigdbg %d: update, state %d init %d\n", plo->oid, t.state, t.fInit);
    if (!t.fInit)
    {
        InitTrigger(plo, t);
        if (std::getenv("SLY2N_TASKDEBUG"))
        {
            std::string str;
            for (auto id : t.aidObj) str += " id " + std::to_string(int(id));
            for (auto cid : t.acid) { char sz[16]; std::snprintf(sz, sizeof sz, " cid 0x%X", unsigned(cid)); str += sz; }
            std::printf("trigger %d: first update: %zu ids, %zu classes, script %d:%s\n", plo->oid, t.aidObj.size(), t.acid.size(),
                        int(plo->U32Field(0, 0x1DC, uint32_t(-1))), str.c_str());
        }
        if (std::getenv("SLY2N_TRIGDBG") && plo->oid == std::atoi(std::getenv("SLY2N_TRIGDBG")))
            if (const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(plo))
            {
                glm::vec3 mn(1e30f), mx(-1e30f);
                const glm::mat4 m = MatWorldLox(plo);
                for (const glm::vec3& v : px->geom.apos) { const glm::vec3 w = glm::vec3(m * glm::vec4(v, 1.0f)); mn = glm::min(mn, w); mx = glm::max(mx, w); }
                std::printf("trigdbg %d: geometry %zu points, world box %.0f %.0f %.0f .. %.0f %.0f %.0f; ids", plo->oid, px->geom.apos.size(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
                for (int id : t.aidObj) std::printf(" %d", id);
                std::printf("; classes");
                for (int cid : t.acid) std::printf(" 0x%X (%zu)", cid, s_psw ? s_psw->mpcidaplo[CID(cid)].size() : 0);
                std::printf("\n");
                int cIn = 0;
                glm::vec3 posIn(0.0f);
                for (int i = 0; i < 12; ++i)
                    for (int j = 0; j < 12; ++j)
                        for (int k = 0; k < 12; ++k)
                        {
                            const glm::vec3 q = mn + (mx - mn) * glm::vec3((i + 0.5f) / 12, (j + 0.5f) / 12, (k + 0.5f) / 12);
                            if (FInsideVolume(plo, q)) { if (!cIn++) posIn = q; }
                        }
                std::printf("trigdbg %d: %d of 1728 grid points inside, e.g. %.0f %.0f %.0f\n", plo->oid, cIn, posIn.x, posIn.y, posIn.z);
            }
    }
    // (+0x1C0 is the state object's state (+0x130 + 0x90): a trigger that fired holds state 2 for its hold time
    // +0x1C8 (+0x98, EOPID 434; 1 s by default: 0x148718), then 1, and only then looks for objects again; it was read
    // as a separate field (always 0) so a fired trigger fired again whenever its object left and came back, e.g. the
    // Ep7 boats' third grapple: the harpoon's bounce re-entered trigger 33528, whose second event 5 completed the
    // next task (3175) at once instead of after the closing scene)
    const int k = t.state;
    if (k == 2)
    {
        const uint32_t u = plo->U32Field(0, 0x1C8, 0x3F800000u);  // (the hold time; -1: never)
        float g;
        std::memcpy(&g, &u, 4);
        if (0.0f <= g && g < g_clock.t - t.tState) SetTriggerState(plo, t, 1);
        return;
    }
    if (k < 0 || k >= 3) return;
    std::vector<LO*> apo;
    // orig: 0x0013E590 / 0x0013E5E0: resolve object options by relative namespace, including instance aliases.
    for (int id : t.aidObj)
        if (g_splicegame.pfnFindObject)
            if (LO* p = dynamic_cast<LO*>(g_splicegame.pfnFindObject(4, id, plo, plo))) apo.push_back(p);
    for (LO* p : t.aploObj) apo.push_back(p);
    // orig: 0x0015E9C8(G, 0x505, cid, 0, 0x40 - c): the class and its derived classes, objects in the world, 0x40 in all
    // (an exact-class list missed e.g. world 27's 49540 / 49541 of class 0x34, the players, whose Sly is a 0x34 child)
    for (int cid : t.acid)
        if (g_splicegame.pfnFindObjectsByClass && apo.size() < 0x40)
        {
            BASIC* ap[0x40];
            const int c = g_splicegame.pfnFindObjectsByClass(0x505, cid, nullptr, 0x40 - static_cast<int>(apo.size()), ap);
            for (int i = 0; i < c; ++i)
                if (LO* p = dynamic_cast<LO*>(ap[i])) apo.push_back(p);
        }
    if (t.aidObj.empty() && t.aploObj.empty() && t.acid.empty())
        if (LO* p = PloPoCurrent(s_psw)) apo.push_back(p);  // (the world's player objects +0x3EB8)
    const bool fTrack = plo->U32Field(0, 0x2C8, 0) != 0;
    std::vector<LO*> aploInside;
    bool fFound = false;
    t.ploFound = nullptr;
    for (LO* po : apo)
    {
        if (std::getenv("SLY2N_TRIGDBG") && plo->oid == std::atoi(std::getenv("SLY2N_TRIGDBG")))
        {
            const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(plo);
            const glm::vec3 pos = FCidDerivedFrom(po->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(po)) : glm::vec3(0.0f);
            const glm::vec3 c = glm::vec3(MatWorldLox(plo)[3]);
            std::printf("trigdbg %d: candidate %d removed %d at (%.0f %.0f %.0f), volume at (%.0f %.0f %.0f) bsp %zu inside %d\n",
                        plo->oid, po->oid, po->fRemoved, pos.x, pos.y, pos.z, c.x, c.y, c.z, px ? px->bsp.abspn.size() : 0,
                        FInsideVolume(plo, pos));
        }
        if (!po || !po->fInWorld || po->fRemoved || !FCidDerivedFrom(po->Cid(), S3CID(0x30))) continue;  // (0x1A61A0: in the world)
        if (!FInsideVolume(plo, PosAloCenter(static_cast<ALO*>(po)))) continue;
        fFound = true;
        if (!t.ploFound) t.ploFound = po;
        if (!fTrack) break;
        aploInside.push_back(po);
    }
    // orig: 0x00149440: compare and publish the complete ordered set before event 9,
    // then change the trigger state. The original sorts the candidate array after
    // selection; that does not reorder the selected members exposed by option 533.
    if (fTrack && aploInside != t.aploInside)
    {
        t.aploInside = std::move(aploInside);
        if (const char* selected = std::getenv("SLY2N_TRIGGERSTATE"); selected && plo->oid == std::atoi(selected))
        {
            std::printf("TRIGGERSET,%.9g,%d,%d,%zu", g_clock.t, plo->oid,
                        plo->paloParent && plo->paloParent->paloParent ? plo->paloParent->paloParent->oid : -1, t.aploInside.size());
            for (LO* member : t.aploInside)
                std::printf(",%d,%d", member->oid, member->paloParent ? member->paloParent->oid : -1);
            std::printf("\n");
        }
        HandleLoSpliceEvent(plo, 9, false, 0, nullptr);
    }
    SetTriggerState(plo, t, fFound ? 1 : 0);
}

struct INITTRIGGERS
{
    INITTRIGGERS()
    {
        RegisterLoUpdate(0x149440, UpdateVolumeTrigger);
        // orig: 0x00149848 -> 0x00200F58: option 533 returns object references in
        // stored order, including repeated entries, or Nil for an empty set.
        // EOPID 442 (option 528): the state, the field +0x1C0 (+0x130's +0x90, set by 0x149718 -> 0x148A78)
        RegisterObjectGetter(442, [](LO* plo) {
            CRef ref;
            const auto it = s_mptrig.find(plo);
            ref.SetS32(it != s_mptrig.end() ? it->second.state : -1);
            return ref;
        });
        // EOPID 447 (option 534; table 0x2AC880: grf 2, the field +0x1D8): the first object found inside at the last
        // update (0x149440). The intro's police-car triggers find their own car with (find 150730 (geto trigger 534));
        // unanswered, the lookup was global and all six triggers parked the first car.
        // orig 0x00149718 (EOPID 442's setter, option 528): the state set (0x148A78: enter / leave events by the change)
        RegisterObjectSetter(442, [](LO* plo, const CRef& r) {
            if (r.m_tagk != TAGK_S32 && r.m_tagk != TAGK_F32) return;
            const int n = r.m_tagk == TAGK_S32 ? r.m_n : static_cast<int>(r.m_g);
            TRIGSTATE& t = s_mptrig[plo];
            if (!t.fInit) return;  // (the loader's value: InitTrigger's post-load state 0 follows)
            SetTriggerState(plo, t, n);
        });
        RegisterObjectGetter(447, [](LO* plo) {
            CRef ref;
            const auto it = s_mptrig.find(plo);
            ref.SetBasic(it != s_mptrig.end() ? it->second.ploFound : nullptr);
            return ref;
        });
        // orig 0x001497B8 (EOPID 432's setter, option 519): one more object in the list +0x28C (at most 8). The Ep7 boat
        // job's module 86 (block 33428) points the grapple triggers 33522 / 33523 at the hacked boat's own harpoon this
        // way; the loaded id 33521 alone resolves to another boat's copy.
        RegisterObjectSetter(432, [](LO* plo, const CRef& r) {
            TRIGSTATE& t = s_mptrig[plo];
            LO* po = r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr;
            if (std::getenv("SLY2N_TASKDEBUG"))
                std::printf("trigger %d: object %d added (%zu + %zu)\n", plo->oid, po ? po->oid : -1, t.aidObj.size(), t.aploObj.size());
            if (po && t.aidObj.size() + t.aploObj.size() < 8) t.aploObj.push_back(po);
        });
        RegisterObjectGetter(444, [](LO* plo) {
            CRef ref(TAGK_None);  // (0x200F58 with no members: 0x12A0B0(ret, 0), the empty list, so null? is true)
            const auto it = s_mptrig.find(plo);
            if (it == s_mptrig.end()) return ref;
            std::shared_ptr<CPair> head, tail;
            for (LO* member : it->second.aploInside)
            {
                auto pair = PpairNew();
                pair->m_ref.SetBasic(member);
                if (tail) tail->m_ppairNext = pair;
                else head = pair;
                tail = pair;
            }
            if (head) ref.SetPair(head);
            return ref;
        });
        // orig 0x1492F0 (the volume trigger's init: its update flag, the +0x40 bit 2): set at the world's first frame
        RegisterSwPass(0x1F77C0, [](SW* psw) {
            // debug: SLY2N_STARTSCPT=oid@frame[:starter] starts a script
            static int s_cframe = 0;
            ++s_cframe;
            if (const char* sz = std::getenv("SLY2N_STARTSCPT"); sz && std::strchr(sz, '@') && s_cframe == std::atoi(std::strchr(sz, '@') + 1))
                if (LO* p = PloFindSwObjectByOid(psw, std::atoi(sz)))
                {
                    // (":starter" an object oid or instance alias: the script's starter, as method 613 passes it)
                    LO* ploStarter = nullptr;
                    if (const char* c = std::strchr(sz, ':'))
                        for (const auto& up : psw->aploAll)
                            if (up->oid == std::atoi(c + 1) || up->oidInstance == std::atoi(c + 1)) { ploStarter = up.get(); break; }
                    StartScpt(p, ploStarter);
                }
            if (std::getenv("SLY2N_TRIGDBG"))
                if (LO* p = PloFindSwObjectByOid(psw, std::atoi(std::getenv("SLY2N_TRIGDBG"))))
                    std::printf("trigdbg %d: removed %d inworld %d iupd %d grfUpd 0x%x\n", p->oid, p->fRemoved, p->fInWorld, p->iupd,
                                p->iupd >= 0 ? psw->aupd[size_t(p->iupd)].grf : p->grfUpd);
            static uint32_t s_serial = 0;
            static SW* s_pswDone = nullptr;
            if (!psw || (psw == s_pswDone && psw->serial == s_serial)) return;
            s_pswDone = psw;
            s_serial = psw->serial;
            for (const auto& up : psw->aploAll)
            {
                LO* plo = up.get();
                if (plo && plo->oid == 150310 && std::getenv("SLY2N_TASKDEBUG"))
                    std::printf("trigger 150310: cid 0x%X slot 16 0x%X\n", plo->Cid(), plo->pcls ? plo->pcls->PfnOrig(16) : 0);
                if (!plo || !plo->pcls || plo->pcls->PfnOrig(16) != 0x149440) continue;
                if (plo->iupd >= 0) psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
                else plo->grfUpd |= GRFUPD_Update;
                if (std::getenv("SLY2N_TASKDEBUG")) std::printf("trigger %d: a volume trigger (iupd %d)\n", plo->oid, plo->iupd);
            }
        });
        // orig 0x104718 (EOPID 366, a shape's option 108): the point inside the shape (0x227C90), e.g. Paris' follow
        // job (module 66 of block 123829): Sly inside volume 123886 when Dimitri's list 1826 ends starts 1822, else
        // the job fails (unported, every end failed)
        RegisterObjectMethod(0x104718, [](LO* plo, const std::vector<CRef>& a) {
            CRef r;
            r.SetBool(!a.empty() && FPointInShape(plo, a[0].GetVector()));
            return r;
        });
        // orig 0x105090 -> 0x149270 (pad +0x150), 0x105440 -> 0x149808 (volume +0x130):
        // both call 0x148B38 / 0x20FEC0 on the embedded state's RSMG list.
        auto addRsmg = [](LO* plo, const std::vector<CRef>& a) {
            TRIGSTATE& t = s_mptrig[plo];
            const int fTrigger = NArg(a, 0);
            const OID oidRoot = NArg(a, 1, -1), oidSm = NArg(a, 2, -1), oid = NArg(a, 3, -1);
            for (auto& e : t.arsmg)
                if (e[0] == oidRoot && (e[1] == -1 || e[1] == oidSm))
                {
                    (fTrigger ? e[2] : e[3]) = oid;
                    return CRef(TAGK_Void);
                }
            if (t.arsmg.size() + 1 < 8)
                t.arsmg.push_back(fTrigger ? std::array<OID, 4>{oidRoot, oidSm, oid, -1} : std::array<OID, 4>{oidRoot, oidSm, -1, oid});
            return CRef(TAGK_Void);
        };
        RegisterObjectMethod(0x105090, addRsmg);
        RegisterObjectMethod(0x105440, addRsmg);
        // orig 0x105508 -> 0x149828 (volume) / 0x105158 -> 0x149290 (pad),
        // both 0x148B78 / 0x161F10: append at most four task states.
        auto addTask = [](LO* plo, const std::vector<CRef>& a) {
            TRIGSTATE& t = s_mptrig[plo];
            if (t.atask.size() < 4) t.atask.emplace_back(NArg(a, 0, -1), NArg(a, 1, -1));
            if (std::getenv("SLY2N_TASKREFS"))
                std::fprintf(stderr, "taskref trigger %d (cid 0x%X): task %d state %d\n", plo->oid, plo->Cid(), NArg(a, 0, -1), NArg(a, 1, -1));
            return CRef(TAGK_Void);
        };
        RegisterObjectMethod(0x105508, addTask);
        RegisterObjectMethod(0x105158, addTask);
    }
} s_inittriggers;
}  // namespace

void ResetTriggers() { s_mptrig.clear(); }

// orig 0x00149880 (the volume trigger's removal, after the base 0x1A5848): a trigger with something inside (state 1 / 2)
// leaves it (0x149718 -> 0x148A78 state 0: its event 6, the owner's 0x1B, the leave list); a tracking one (+0x2C8)
// empties its set (+0x2C0) and runs event 9. E.g. Ep7 Thermal Ride: task 3180 done removes the start volume 33905
// with Sly inside, and the volume's leave handler ends the job as the original does.
void OnRemoveTrigger(LO* plo)
{
    auto it = s_mptrig.find(plo);
    if (it == s_mptrig.end()) return;
    TRIGSTATE& t = it->second;
    if (static_cast<unsigned>(t.state - 1) >= 2) return;
    SetTriggerState(plo, t, 0);
    if (plo->U32Field(0, 0x2C8, 0))
    {
        t.aploInside.clear();
        HandleLoSpliceEvent(plo, 9, false, 0, nullptr);
    }
}

void CloneEmbeddedTriggerActions(const LO* source, LO* target)
{
    // orig: 0x001A6730 copies the class data after +0x50. The native action
    // arrays live outside LO, so cloning aopt alone loses these loaded methods.
    const auto it = s_mptrig.find(const_cast<LO*>(source));
    if (it == s_mptrig.end()) return;
    const auto actions = it->second.arsmg;
    const auto tasks = it->second.atask;
    TRIGSTATE& copy = s_mptrig[target];
    copy.owner = target;
    copy.arsmg = actions;
    copy.atask = tasks;
}

void RunTriggerRsmgList(LO* plo, int i)
{
    const auto it = s_mptrig.find(plo);
    if (it != s_mptrig.end()) ApplyRsmg(plo, it->second, i == 0);
}

void RunEmbeddedTriggerList(LO* plo, int i, LO* found)
{
    TRIGSTATE& t = s_mptrig[plo];
    t.owner = plo;
    t.ploFound = found;
    RunTriggerList(plo, t, i);
}

// (exported: a point in a volume's BSP, the exits' 0x118FB0 test)
bool FPointInVolume(LO* plo, const glm::vec3& pos) { return FInsideVolume(plo, pos); }
