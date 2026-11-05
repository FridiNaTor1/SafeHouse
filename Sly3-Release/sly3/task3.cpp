// Sly 3's tasks and jobs (task3.h).

#include "task3.h"

#include "condset3.h"
#include "ids3.h"
#include "load3.h"
#include "trigger3.h"

#include "../animfind.h"
#include "../brx.h"
#include "../smrun.h"
#include "../sm.h"
#include "../sw.h"

#include <cstdio>
#include <cstdlib>
#include <map>

TASKS3 g_tasks3;
TASKHOST3 g_taskhost3;

namespace
{
std::map<OID, TASK3*> s_mpoidtask;
std::map<OID, JOB3*> s_mpoidjob;
bool s_fPrint = false;

const OPT* PoptFind(const LO* plo, int eopid)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == eopid && !o.aval.empty()) return &o;
    return nullptr;
}
int NOpt(const LO* plo, int eopid, int nDefault)
{
    const OPT* p = PoptFind(plo, eopid);
    return p ? p->aval[0].n : nDefault;
}

float TNow() { return g_taskhost3.pfnTime ? g_taskhost3.pfnTime() : 0.0f; }

void Log(const char* sz, const TASK3* p, int n)
{
    if (s_fPrint) std::printf("task3: %s %d -> %d (state %d)\n", sz, p ? p->oid : -1, n, p ? p->nState : -1);
}

// The change's mode and context written into the task and its job for a call, restored after (the original's
// stack save of +0xFC / +0x100 and the job's +0xF0 / +0xF4)
struct CMODE3
{
    TASK3* ptask;
    JOB3* pjob;
    int nModeTask = 0, nModeJob = 0;
    void* ploTask = nullptr;
    void* ploJob = nullptr;
    CMODE3(TASK3* p, int nMode, void* plo) : ptask(p), pjob(p ? p->pjob : nullptr)
    {
        if (ptask) { nModeTask = ptask->nMode; ploTask = ptask->ploContext; ptask->nMode = nMode; ptask->ploContext = plo; }
        if (pjob) { nModeJob = pjob->nMode; ploJob = pjob->ploContext; pjob->nMode = nMode; pjob->ploContext = plo; }
    }
    ~CMODE3()
    {
        if (pjob) { pjob->nMode = nModeJob; pjob->ploContext = ploJob; }
        if (ptask) { ptask->nMode = nModeTask; ptask->ploContext = ploTask; }
    }
};

// orig3: 0x1880F0: the task reacts (its +0x40 clear and the change not a restore, mode 3)
bool FTaskLive(const TASK3* p) { return p->f40 == 0 && p->nMode != 3; }

// vt slot 3 (0x1C86B8) on the task's object: the message msg with no arguments
void Message3(TASK3* p, int msg)
{
    if (g_taskhost3.pfnMessage) g_taskhost3.pfnMessage(p, msg);
}

// orig3: 0x188A20 FIsTaskCurrent (SW +0x4048)
bool FIsTaskCurrent(const TASK3* p) { return g_tasks3.ptaskActive == p; }
// orig3: 0x187CB8 FIsMissionCurrent (SW +0x4044)
bool FIsJobCurrent(const JOB3* p) { return g_tasks3.pjobCur == p; }

// orig3: 0x18AA48 (SW +0x4044 and GAME +0x1498: the job's oid; TrackJt 0x1B67C0 when it changes: TODO(sly3))
void SetJobCur3(JOB3* p) { g_tasks3.pjobCur = p; }
// orig3: 0x18AAB8 (SW +0x4048, GAME +0x149C)
void SetTaskActive3(TASK3* p) { g_tasks3.ptaskActive = p; }
// orig3: 0x18AAE0 (SW +0x404C, GAME +0x14A0)
void SetTaskCur3(TASK3* p) { g_tasks3.ptaskCur = p; }

// orig3: 0x189008: every prerequisite done (state >= 2), or available (1) with its +0x5C set
bool FTaskPrereqsDone(const TASK3* p)
{
    for (const TASK3* px : p->aprereq)
        if (px->nState < 2 && !(px->nState == 1 && px->plo && false)) return false;  // TODO(sly3): +0x5C (no EOPID in m_ext)
    return true;
}

// orig3: 0x186338 (from JobSwitch 0x186248): the job becomes the current one (TODO(sly3): its other effects:
// the job's objects / HUD, 0x186338's 728 bytes)
void ActivateJob3(JOB3* p) { SetJobCur3(p); }
// orig3: 0x186298 DeactivateJob
void DeactivateJob3(JOB3*) { SetJobCur3(nullptr); }
// orig3: 0x186248 JobSwitch
void JobSwitch3(JOB3* p)
{
    if (FIsJobCurrent(p)) return;
    if (g_tasks3.pjobCur) DeactivateJob3(g_tasks3.pjobCur);
    ActivateJob3(p);
}
// orig3: 0x186DA0 JobBegin: the broadcast 0x1F, the player's booty drop (TODO(sly3)), JobSwitch
void JobBegin3(JOB3* p)
{
    if (FIsJobCurrent(p)) return;
    JobSwitch3(p);
}

void SetTaskState3(TASK3* p, int n);
void SetTaskStateTree3(TASK3* p, int n);
void ActivateTaskNow3(TASK3* p);

// orig3: 0x188928: the current task left: SW +0x4048 cleared, the broadcast 0x28, the state-5 objects, message 0x17
void DeactivateTaskNow3(TASK3* p)
{
    Log("deactivate", p, 5);
    SetTaskActive3(nullptr);
    if (FTaskLive(p) && g_taskhost3.pfnApplyObjects) g_taskhost3.pfnApplyObjects(p, 5);
}
// orig3: 0x188620
void DeactivateTask3(TASK3* p)
{
    if (FIsTaskCurrent(p)) DeactivateTaskNow3(p);
}

// orig3: 0x18A3E8 SetTaskState
void SetTaskState3(TASK3* p, int n)
{
    if (p->nState == n) return;
    Log("state", p, n);
    // live: the broadcast 0x2A (0x1854C8: TODO(sly3)) and message 0x19
    if (FTaskLive(p)) Message3(p, 0x19);
    if (p->nState == 1)
        std::erase(g_tasks3.aptaskAvailable, p);
    if (FTaskLive(p) && g_taskhost3.pfnApplyObjects) g_taskhost3.pfnApplyObjects(p, n);  // 0x185A70 / 0x185DB0
    if (n == 1) g_tasks3.aptaskAvailable.push_back(p);
    else if (n == 0 && p->nMode == 4)
    {
        p->cActivated = 0;
        p->cDone = 0;
        if (p->pjob && p->pjob->ptaskFirst == p) p->pjob->f7c = 0;
    }
    else if (n == 2 && (p->nMode == 0 || p->nMode == 3)) ++p->cDone;
    p->nState = n;
    p->tState = TNow();
    // (Task_SaveState 0x188DE0: the progress); live: the broadcast 0x29 (TODO(sly3)), message 0x18 (vt slot 3; then
    // slot 4 0x13BE38 with the task: the listeners, TODO(sly3))
    if (FTaskLive(p)) Message3(p, 0x18);
    if (p->nMode != 0 && p->nMode != 4) return;
    if (p->nState == 1)
    {
        if (p->fAutoActivate) QueueTaskRequest3(1, p, 4, 0, nullptr);  // 0x1890B0
    }
    else if (p->nState == 0 || p->nState == 3) DeactivateTask3(p);
}

// orig3: 0x18A778: a successor's state from its predecessor's (n)
void StateFromParent3(TASK3* p, int n)
{
    if (p->nTarget != -1) return;
    int nNew = p->nState;
    if (n >= 0 && n < 2) nNew = 0;
    else if (n >= 2 && n < 4)
    {
        bool fAll2 = true, fAll3 = true;
        for (const TASK3* px : p->aprereq)
        {
            if (px->nState < 2) { fAll2 = fAll3 = false; break; }
            if (px->nState < 3 || px->pjob != p->pjob || !px->pjob) fAll3 = false;
        }
        if (!fAll2) nNew = 0;
        else if (n == 3 && fAll3) nNew = 3;
        else if (p->nState == 0) nNew = 1;
        else if (p->nState == 3) nNew = n;
    }
    SetTaskStateTree3(p, nNew);
}

// orig3: 0x18A670: the state, then each successor's from it
void SetTaskStateTree3(TASK3* p, int n)
{
    if (p->nState == n) return;
    SetTaskState3(p, n);
    for (TASK3* px : p->anext)
    {
        CMODE3 save(px, p->nMode, p->ploContext);
        StateFromParent3(px, p->nState);
    }
}

// orig3: 0x1899F0 CollectTasks: the prerequisites that must reach a state for p to reach n, then p (+0xF8 = n)
void CollectTasks3(TASK3* p, int n, std::vector<TASK3*>& a)
{
    for (TASK3* px : p->aprereq)
    {
        int nChild = -1;
        if (n == 3) nChild = 3;
        else if (n > 0 && n < 3)
        {
            if (!px->pjob) nChild = 3;
            else if (px->pjob == p->pjob) nChild = 2;
            else nChild = px->pjob->f6c ? 2 : 3;
        }
        if (px->nTarget == -1 && px->nState < nChild) CollectTasks3(px, nChild, a);
    }
    a.push_back(p);
    p->nTarget = n;
}

// orig3: 0x189B08: the state change n with the prerequisites it implies (queued while another runs)
void ChangeTaskState3(TASK3* p, int n)
{
    const int nOld = p->nState;
    if (n == nOld) return;
    if (g_tasks3.fBusy)
    {
        QueueTaskRequest3(1, p, n, p->nMode, p->ploContext);
        return;
    }
    g_tasks3.fBusy = true;
    std::vector<TASK3*> a;
    CollectTasks3(p, n, a);
    for (TASK3* px : a)
    {
        CMODE3 save(px, p->nMode, p->ploContext);
        SetTaskStateTree3(px, px->nTarget);
    }
    for (TASK3* px : a) px->nTarget = -1;
    g_tasks3.fBusy = false;
    // (a job task done: 0x186810 JobTaskReached; no job, no prerequisites: the autosave 0x1CFE08: TODO(sly3))
}

// orig3: 0x188868 TaskRefused (the broadcast 0x2B, message 0x15 to the task and its job)
void TaskRefused3(TASK3* p, int n) { Log("refused", p, n); }

// orig3: 0x188658: the task becomes the active one
void ActivateTaskNow3(TASK3* p)
{
    Log("activate", p, 4);
    SetTaskActive3(p);
    // (0x2845B0(0x4C4230, task, 4): a running sequence takes it: none in the prologue)
    if (p->idChk != -1) SetTaskCur3(p);  // (when 0x4C4230 +0x14 is null or its +0x50 is 1)
    if (p->pjob && !FIsJobCurrent(p->pjob)) JobBegin3(p->pjob);
    if (!FTaskLive(p)) return;
    ++p->cActivated;
    if (g_taskhost3.pfnApplyObjects) g_taskhost3.pfnApplyObjects(p, 4);
    // (+0x4C / +0x50: the HUD timer; message 0x16)
    if (p->pscpt && g_taskhost3.pfnStartScpt) g_taskhost3.pfnStartScpt(p->pscpt);  // 0x226010
    if (p->fDoneAtOnce) QueueTaskRequest3(0, p, 2, 0, nullptr);                    // 0x189078(p, 2, 0)
    // (+0xC8: TriggerExit 0x27FCC8: TODO(sly3))
}

// orig3: 0x188540: the active task left for this one
void ActivateTask3(TASK3* p)
{
    if (FIsTaskCurrent(p)) return;
    if (TASK3* pcur = g_tasks3.ptaskActive)
    {
        CMODE3 save(pcur, p->nMode, p->ploContext);
        DeactivateTask3(pcur);
    }
    ActivateTaskNow3(p);
}

// orig3: 0x189300 RequestTaskState (-1 / 4 activate, 5 deactivate, 0..3 a state by the rules)
void RequestTaskState3(TASK3* p, int n, int nMode, void* ploContext)
{
    CMODE3 save(p, nMode, ploContext);
    if (n == -1) n = 4;
    const int nCur = p->nState;
    if (nCur == -1) return;
    if (n == 3 && p->pjob) n = 2;
    if (n == 4)
    {
        if (nCur == 0)
        {
            if (!FTaskPrereqsDone(p)) { TaskRefused3(p, 4); return; }
            ChangeTaskState3(p, 1);
        }
        else if (nCur == 2 || nCur == 3) return;
        ActivateTask3(p);
        return;
    }
    if (n == 5) { DeactivateTask3(p); return; }
    if (n < 0 || n > 5) return;
    bool fOk;
    if (nCur == 1) fOk = n == 2 || n == 3;
    else if (nCur == 0) fOk = n == 1 && FTaskPrereqsDone(p);
    else if (nCur == 3) return;
    else fOk = n - nCur == 1;
    if (fOk) ChangeTaskState3(p, n);
    else TaskRefused3(p, n);
}

// orig3: 0x189500 ForceTaskState (no checks)
void ForceTaskState3(TASK3* p, int n, int nMode, void* ploContext)
{
    CMODE3 save(p, nMode, ploContext);
    if (n == -1) n = 4;
    if (p->nState == -1) return;
    if (n == 4)
    {
        if (!p->pjob && FTaskPrereqsDone(p)) ChangeTaskState3(p, 1);
        ActivateTask3(p);
    }
    else if (n == 5) DeactivateTask3(p);
    else if (n >= 0 && n < 4) ChangeTaskState3(p, n);
}

// orig3: 0x18A900 VisitTask: prerequisites first
void VisitTask3(TASK3* p, std::vector<TASK3*>& a, std::map<TASK3*, bool>& mpf)
{
    if (mpf[p]) return;
    mpf[p] = true;
    for (TASK3* px : p->aprereq) VisitTask3(px, a, mpf);
    a.push_back(p);
}
}  // namespace

void QueueTaskRequest3(int kind, TASK3* ptask, int n, int nMode, void* ploContext)
{
    if (!ptask) return;
    TASKS3::REQ r{kind, ptask, n, nMode, ploContext};
    if (g_tasks3.fRunningQueue)
    {
        // InsertDlEntryBefore(list, entry, cursor): after the request running, before the ones waiting
        g_tasks3.areq.insert(g_tasks3.areq.begin() + std::ptrdiff_t(g_tasks3.ireqNext), r);
        ++g_tasks3.ireqNext;
        return;
    }
    // orig3: 0x189180: run the list
    g_tasks3.areq.push_back(r);
    g_tasks3.fRunningQueue = true;
    while (!g_tasks3.areq.empty())
    {
        const TASKS3::REQ q = g_tasks3.areq.front();
        g_tasks3.areq.erase(g_tasks3.areq.begin());
        g_tasks3.ireqNext = 0;
        if (q.kind == 0) RequestTaskState3(q.ptask, q.n, q.nMode, q.ploContext);
        else ForceTaskState3(q.ptask, q.n, q.nMode, q.ploContext);
    }
    g_tasks3.fRunningQueue = false;
}

TASK3* PtaskFind3(OID oid)
{
    const auto it = s_mpoidtask.find(oid);
    return it == s_mpoidtask.end() ? nullptr : it->second;
}
JOB3* PjobFind3(OID oid)
{
    const auto it = s_mpoidjob.find(oid);
    return it == s_mpoidjob.end() ? nullptr : it->second;
}

void InitTasks3(SW* psw)
{
    g_tasks3 = TASKS3{};
    s_mpoidtask.clear();
    s_mpoidjob.clear();
    s_fPrint = std::getenv("SLY3_TASKPRINT") != nullptr;
    auto ploFind = [psw](OID oid) -> LO* { return oid < 0 ? nullptr : PloFindSwObjectByOid(psw, oid); };
    for (LO* plo : psw->mpcidaplo[Icid(CID3::MISSION)])
    {
        auto pj = std::make_unique<JOB3>();
        pj->plo = plo;
        pj->oid = plo->oid;
        pj->f6c = NOpt(plo, 1361, 0);
        s_mpoidjob[plo->oid] = pj.get();
        g_tasks3.ajob.push_back(std::move(pj));
    }
    for (LO* plo : psw->mpcidaplo[Icid(CID3::TASK)])
    {
        auto pt = std::make_unique<TASK3>();
        pt->plo = plo;
        pt->oid = plo->oid;
        s_mpoidtask[plo->oid] = pt.get();
        g_tasks3.atask.push_back(std::move(pt));
    }
    for (auto& pt : g_tasks3.atask)
    {
        const LO* plo = pt->plo;
        const LOLIST* plist = static_cast<const LOLIST*>(plo);
        for (int32_t oid : plist->an90)
            if (TASK3* px = PtaskFind3(oid)) pt->aprereq.push_back(px);
        for (int32_t oid : plist->an9C)
            if (TASK3* px = PtaskFind3(oid)) pt->anext.push_back(px);
        pt->fAutoActivate = NOpt(plo, 1399, 0);
        pt->pjob = PjobFind3(NOpt(plo, 1391, -1));
        pt->idChk = NOpt(plo, 1400, -1);
        pt->iworld = NOpt(plo, 1401, -1);
        pt->fDoneAtOnce = NOpt(plo, 1405, 0);
        pt->ploExit = ploFind(NOpt(plo, 1404, -1));
        pt->pscpt = ploFind(NOpt(plo, 1408, -1));
        for (const OPT& o : plo->aopt)  // 1416 (0x10E260): {object, [states], owner, a, b}
            if (o.eopid == 1416 && o.aval.size() >= 5)
            {
                TASK3::OBJE e;
                e.oid = o.aval[0].n;
                for (const OPTVAL& v : o.aval[1].alist) e.grf |= 1 << v.n;
                if (o.aval[1].alist.empty()) e.grf |= 1 << o.aval[1].n;
                e.oidOwner = o.aval[2].n;
                e.a = o.aval[3].n;
                e.b = o.aval[4].n;
                pt->aobje.push_back(e);
            }
        for (const OPT& o : plo->aopt)  // 1417 (0x10E358): {state, object, SM, goal, instant, world, +0x18}
            if (o.eopid == 1417 && o.aval.size() >= 6)
            {
                TASK3::SME e;
                e.nState = o.aval[0].n;
                e.oid = o.aval[1].n;
                e.oidSm = o.aval[2].n;
                e.oidGoal = o.aval[3].n;
                e.fInstant = o.aval[4].n;
                e.iworld = o.aval[5].n;
                if (o.aval.size() > 6) e.x18 = o.aval[6].n;
                pt->asme.push_back(e);
            }
    }
    for (auto& pj : g_tasks3.ajob)
    {
        pj->ptaskFirst = PtaskFind3(NOpt(pj->plo, 1364, -1));
        pj->ptaskLast = PtaskFind3(NOpt(pj->plo, 1365, -1));
    }
}

// orig3: 0x188CB0 RestoreTask (the save's state, NProgress +0xE8 / +0xF0: TODO(sly3) the progress; a new game's are
// 0): unsaved, a task without prerequisites is available. The mode is 1 (live: the reference's restore of 4253, state
// 1, adds its object 16368 and its message starts SCPT 4262; the SMs snap to their goals; no activation follows).
void RestoreTasks3()
{
    std::vector<TASK3*> a;
    std::map<TASK3*, bool> mpf;
    for (auto& pt : g_tasks3.atask) VisitTask3(pt.get(), a, mpf);
    for (TASK3* p : a)
    {
        const int nSaved = 0;
        const int n = nSaved != 0 ? nSaved : (p->aprereq.empty() ? 1 : 0);
        // (the reference's restore runs in the task mode 1: its SMs snap, no activation follows; FNTRACE of
        // ApplyTaskSms 0x185DB0 at vblanks 1323..1329: t0 = 1)
        CMODE3 save(p, 1, nullptr);
        SetTaskState3(p, n);
    }
}

bool FLoUnder(const LO* plo, const LO* ploAnc)
{
    for (const LO* p = plo; p; p = p->paloParent)
        if (p == ploAnc) return true;
    return false;
}

// orig3: 0x185980 GrfState
int GrfState3(int n) { return n == -1 ? 0 : 1 << n; }

void ApplyTaskObjects3(SW* psw, TASK3* p, int n)
{
    const int grfState = GrfState3(n);
    const int grfGroup = (n == 4 || n == 5) ? GrfState3(4) | GrfState3(5) : 0xF;  // 0x1859A8 over 0x3218C0 / 0x3218C8
    for (const TASK3::OBJE& e : p->aobje)
    {
        if (!(e.grf & grfGroup)) continue;
        if (e.a != -1 && e.a != g_nFilterCur3) continue;  // 0x25E150
        // PloFindSwNearest(sw, 5, owner, 0), then (sw, owner ? 4 : 5, oid, owner)
        LO* ploOwner = e.oidOwner == -1 ? nullptr : PloFindSwObjectByOid(psw, e.oidOwner);
        if (!ploOwner && e.oidOwner != -1) continue;
        LO* plo = ploOwner ? PloFindSwNearest(ploOwner, e.oid) : PloFindSwObjectByOid(psw, e.oid);
        if (!plo)  // a PROXY's oid names its instance (the reference adds the instance's root: 15346 -> 15345's)
            for (const auto& pl : psw->aploAll)
                if (pl->oidInstance == e.oid && (!ploOwner || FLoUnder(pl.get(), ploOwner))) { plo = pl.get(); break; }
        if (s_fPrint)
            std::printf("task3:   objects %d n %d: %d (owner %d, grf 0x%x) %s\n", p->oid, n, e.oid, e.oidOwner, e.grf,
                        !plo ? "none" : (e.grf & grfState) ? "add" : "remove");
        if (!plo) continue;
        if (e.grf & grfState) sly3::AddLo3(psw, plo);
        else sly3::RemoveLo3(psw, plo);
    }
}

void ApplyTaskSms3(SW* psw, TASK3* p, int n)
{
    for (const TASK3::SME& e : p->asme)  // 0x185C30 per entry
    {
        if (e.nState != -1 && e.nState != n) continue;
        if (e.fInstant && p->nMode == 0) continue;
        if (e.iworld != -1 && e.iworld != g_nFilterCur3) continue;
        // CploFindSwNearest(sw, 0x205, oid, 0, 1, ...): exactly one object of that oid
        LO* plo = nullptr;
        int c = 0;
        for (const auto& pl : psw->aploAll)
            if (pl->oid == e.oid) { plo = pl.get(); ++c; }
        if (c != 1 || !FIsLoDerivedFrom(plo, Icid(CID3::ALO))) continue;
        SMA* psma = nullptr;  // PsmaFindAloSm 0x23FB68 (the SM's oid, -1 any)
        for (SMA* pa : g_apsma)
            if (pa->palo == plo && (e.oidSm == -1 || (pa->psm && pa->psm->oid == e.oidSm))) { psma = pa; break; }
        int id = e.oidGoal;
        if (e.nState == -1) id = (n >= 0 && n <= 3) ? 0x6E3 + n : e.oidGoal;
        if (s_fPrint)
            std::printf("task3:   sm %d n %d: object %d sm %d goal %d %s\n", p->oid, n, e.oid, e.oidSm, id,
                        psma ? (p->nMode == 1 || p->nMode == 4 ? "snap" : "set") : "no SMA");
        if (!psma || id == -1) continue;
        if (p->nMode == 1 || p->nMode == 4) SnapSmaGoal(psma, id);
        else SetSmaGoal(psma, id);
    }
}

void ApplyCheckpointTasks3(const int* apair, int cpair, void* ploContext)
{
    for (int i = 0; i < cpair; ++i)
        if (TASK3* p = PtaskFind3(apair[2 * i])) QueueTaskRequest3(0, p, apair[2 * i + 1], 0, ploContext);  // 0x189078
}

void TaskKeys3(int akey[3])
{
    akey[0] = g_tasks3.pjobCur ? g_tasks3.pjobCur->oid : -1;
    akey[1] = g_tasks3.ptaskActive ? g_tasks3.ptaskActive->oid : -1;
    akey[2] = g_tasks3.ptaskCur ? g_tasks3.ptaskCur->oid : -1;
}

void PrintTasks3(FILE* pf)
{
    int ak[3];
    TaskKeys3(ak);
    std::fprintf(pf, "tasks: keys %d %d %d;", ak[0], ak[1], ak[2]);
    for (auto& pt : g_tasks3.atask)
        if (pt->nState != 0) std::fprintf(pf, " %d=%d", pt->oid, pt->nState);
    std::fprintf(pf, "\n");
}
