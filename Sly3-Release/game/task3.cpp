// Sly 3's tasks and jobs (task3.h).

#include "task3.h"

#include "cond3.h"
#include "sly3.h"

#include "../activate.h"
#include "../animfind.h"
#include "../brx.h"
#include "../smrun.h"
#include "../sm.h"
#include "../savedata.h"
#include "../jobend3.h"
#include "../sw.h"
#include "../savedata.h"
#include "../frontend.h"
#include "../transition.h"

#include <cstdio>
#include <cstdlib>
#include <map>

TASKS3 g_tasks3;
TASKHOST3 g_taskhost3;
GAMETASKS3 g_gametasks3;

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

// ---- the progress (the save's values at the task's paths: 0x2BC010 read / 0x2BC050 write on 0x468D30)
bool FReadProgress(const std::vector<int16_t>& akey, int* pn)
{
    if (akey.empty()) return false;
    const CRef r = RefGetProgressKey(akey);
    if (r.m_tagk == TAGK_S32) *pn = r.m_n;
    else if (r.m_tagk == TAGK_Bool) *pn = r.m_bool ? 1 : 0;
    else if (r.m_tagk == TAGK_F32) *pn = int(r.m_g);
    else return false;
    return true;
}
int NProgress3(const std::vector<int16_t>& akey)
{
    int n = 0;
    return FReadProgress(akey, &n) ? n : 0;
}
void WriteProgressS32(const std::vector<int16_t>& akey, int n)
{
    if (akey.empty()) return;
    CRef r;
    r.SetS32(n);
    FSetProgressKey(akey, r);
}
void WriteProgressBool(const std::vector<int16_t>& akey, bool f)
{
    if (akey.empty()) return;
    CRef r;
    r.SetBool(f);
    FSetProgressKey(akey, r);
}

// orig3: 0x188A38 FTaskDoneJh: unloaded (-1), the saved flag; else done (state 2 or 3)
bool FTaskDone3(const TASK3* p)
{
    if (p->nState == -1) return NProgress3(p->akeyDoneBool) != 0;
    return p->nState >= 2;
}

// orig3: 0x188DE0 Task_SaveState (not in the modes 1 / 2: a restore, a reload): the counts, the done flag, the
// state, the abandoned flag cleared (then the job's Task_SaveVars 0x187D30: TODO(sly3))
void SaveTask3(TASK3* p)
{
    if (p->nMode == 1 || p->nMode == 2 || p->nState == -1) return;
    WriteProgressS32(p->akeyActivated, p->cActivated);
    WriteProgressS32(p->akeyDone, p->cDone);
    WriteProgressBool(p->akeyDoneBool, FTaskDone3(p));
    WriteProgressS32(p->akeyState, p->nState);
    WriteProgressBool(p->akeyAbandoned, false);
}

// ---- the broadcasts' messages (0x14 {code, object}: Task_Notify 0x189DF8 / JobOnEvent 0x186730 / the listeners)
void MessageArgs3(LO* plo, int msg, int n, LO* ploArg)
{
    if (plo && g_taskhost3.pfnMessageArgs) g_taskhost3.pfnMessageArgs(plo, msg, n, ploArg);
}

// orig3: 0x188A20 FIsTaskCurrent (SW +0x4048)
bool FIsTaskCurrent(const TASK3* p) { return g_tasks3.ptaskActive == p; }
// orig3: 0x187CB8 FIsMissionCurrent (SW +0x4044)
bool FIsJobCurrent(const JOB3* p) { return g_tasks3.pjobCur == p; }

// orig3: 0x18AA48 (SW +0x4044 and GAME +0x1498: the job's oid; TrackJt 0x1B67C0 when it changes: TODO(sly3))
void SetJobCur3(JOB3* p)
{
    g_tasks3.pjobCur = p;
    g_gametasks3.oidJob = p ? p->oid : -1;
}
// orig3: 0x18AAB8 (SW +0x4048, GAME +0x149C)
void SetTaskActive3(TASK3* p)
{
    g_tasks3.ptaskActive = p;
    g_gametasks3.oidTask = p ? p->oid : -1;
}
// orig3: 0x18AAE0 (SW +0x404C, GAME +0x14A0)
void SetTaskCur3(TASK3* p)
{
    g_tasks3.ptaskCur = p;
    g_gametasks3.oidChk = p ? p->oid : -1;
}

// orig3: 0x189008: every prerequisite done (state >= 2), or available (1) with its +0x5C set
bool FTaskPrereqsDone(const TASK3* p)
{
    for (const TASK3* px : p->aprereq)
        if (px->nState < 2 && !(px->nState == 1 && px->plo && false)) return false;  // TODO(sly3): +0x5C (no EOPID in m_ext)
    return true;
}

// orig3: 0x186338 (from JobSwitch 0x186248): the job becomes the current one, the broadcast 0x25 (TODO(sly3): its
// other effects: the job's objects / HUD, 0x186338's 728 bytes)
void ActivateJob3(JOB3* p)
{
    SetJobCur3(p);
    Broadcast3(0x25, p->plo);
}
// orig3: 0x186610 DeactivateJob: the broadcast 0x26
void DeactivateJob3(JOB3* p)
{
    Broadcast3(0x26, p ? p->plo : nullptr);
    SetJobCur3(nullptr);
}
// orig3: 0x186248 JobSwitch
void JobSwitch3(JOB3* p)
{
    if (FIsJobCurrent(p)) return;
    if (g_tasks3.pjobCur) DeactivateJob3(g_tasks3.pjobCur);
    ActivateJob3(p);
}
// orig3: 0x186DA0 JobBegin: the broadcast 0x1F, the player's booty drop (DropActorBooty 0x155ED8: TODO(sly3)), JobSwitch
void JobBegin3(JOB3* p)
{
    if (FIsJobCurrent(p)) return;
    Broadcast3(0x1F, p->plo);
    JobSwitch3(p);
}

void ChangeTaskState3(TASK3* p, int n);

// orig3: 0x187270 (the job's rewards / close: jobend3's pfnRewards): the last task closed (3) unless the job keeps
// them (+0x6C), the job deactivated (the scene completion 0x21F5D0 and the job's booty / progress records: TODO(sly3))
void JobRewards3(JOB3* p)
{
    if (!p->f6c && p->ptaskLast) ChangeTaskState3(p->ptaskLast, 3);
    if (FIsJobCurrent(p)) DeactivateJob3(p);
    if (!p->f6c && g_taskhost3.pfnAutosave) g_taskhost3.pfnAutosave();   // 0x1874B4: the autosave 0x1CFE08 (the world flow code)
}

// orig3: 0x1874E8 JobCompleteClose (jobend3's pfnCompleteClose): no transition pending: the job's exit (+0xC0, its LO
// word +0x3C | 0x200 first: TODO(sly3) the bit) fired (e.g. the prologue's job 1797 -> exit 4236 -> movie 334 -> the
// episode chooser). (Its next task +0xB4: TODO(sly3))
void JobCompleteClose3(JOB3* p)
{
    if (std::getenv("SLY2N_EXITPRINT"))
        std::printf("task3: job %d complete: exit %d, transition pending %d\n", p->oid, p->ploExit ? p->ploExit->oid : -1,
                    int(g_transition.fPending));
    if (p->ploExit && !g_transition.fPending) TriggerExit(p->ploExit);
}

// orig3: 0x186FE8 (jobend3's pfnRestart, after Job Failed): the job again from its checkpoint task (else its first
// task): RequestTaskStartByOid 0x189F38, the world reloaded
void JobRestart3(JOB3* p)
{
    TASK3* pt = g_tasks3.ptaskCur && g_tasks3.ptaskCur->pjob == p ? g_tasks3.ptaskCur : p->ptaskFirst;
    if (pt) RequestTaskStart(g_psw, g_transition.iwrecCur, pt->oid);
}

// orig3: 0x1871C0 the job complete (+0xB8 clear): the broadcast 0x24, then the screen half (jobend3.h: the stamp, then
// the rewards and the close through the hooks above; at once without a character)
void JobComplete3(JOB3* p)
{
    std::printf("job %d: complete\n", p->oid);  // (tools/progression3.py)
    Broadcast3(0x24, p->plo);
    JobEndComplete3(p);
}

struct INITJOBEND3
{
    INITJOBEND3()
    {
        g_jobendhooks3.pfnRewards = JobRewards3;
        g_jobendhooks3.pfnCompleteClose = JobCompleteClose3;
        g_jobendhooks3.pfnRestart = JobRestart3;
    }
} s_initjobend3;

// orig3: 0x186810 JobTaskReached: a task of the job done: its first begins the job, its last completes it; a
// checkpoint task becomes the checkpoint (SetTaskChk 0x18AAE0)
void JobTaskReached3(JOB3* pjob, TASK3* ptask)
{
    if (ptask == pjob->ptaskFirst) JobBegin3(pjob);
    if (ptask == pjob->ptaskLast) JobComplete3(pjob);
    if (ptask->idChk != -1) SetTaskCur3(ptask);
}

void SetTaskState3(TASK3* p, int n);
void SetTaskStateTree3(TASK3* p, int n);
void ActivateTaskNow3(TASK3* p);

// orig3: 0x188928: the current task left: SW +0x4048 cleared, the broadcast 0x28, the state-5 objects, message 0x17
void DeactivateTaskNow3(TASK3* p)
{
    Log("deactivate", p, 5);
    SetTaskActive3(nullptr);
    if (FTaskLive(p)) Broadcast3(0x28, p->plo);
    if (FTaskLive(p) && g_taskhost3.pfnApplyObjects) g_taskhost3.pfnApplyObjects(p, 5);
    if (FTaskLive(p)) Message3(p, 0x17);
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
    // live: the broadcast 0x2A and message 0x19
    if (FTaskLive(p))
    {
        Broadcast3(0x2A, p->plo);
        Message3(p, 0x19);
    }
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
    // Task_SaveState 0x188DE0 (the progress); live: the broadcast 0x29, message 0x18 (vt slot 3; then slot 4
    // 0x13BE38 with the task: the listeners, TODO(sly3))
    SaveTask3(p);
    if (FTaskLive(p))
    {
        Broadcast3(0x29, p->plo);
        Message3(p, 0x18);
    }
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
    // a task done: of a job, 0x186810 JobTaskReached; with no job and no prerequisites, the autosave 0x1CFE08
    if (FTaskLive(p) && p->nState != nOld && p->nState == 2)
    {
        if (p->pjob) JobTaskReached3(p->pjob, p);
        else if (p->aprereq.empty() && g_taskhost3.pfnAutosave) g_taskhost3.pfnAutosave();
    }
}

// orig3: 0x188868 TaskRefused (the broadcast 0x2B, message 0x15 to the task and its job)
void TaskRefused3(TASK3* p, int n)
{
    Log("refused", p, n);
    Broadcast3(0x2B, p->plo);
    Message3(p, 0x15);
}

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
    Broadcast3(0x27, p->plo);
    if (g_taskhost3.pfnApplyObjects) g_taskhost3.pfnApplyObjects(p, 4);
    // (+0x4C / +0x50: the HUD timer; message 0x16)
    if (p->pscpt && g_taskhost3.pfnStartScpt) g_taskhost3.pfnStartScpt(p->pscpt);  // 0x226010
    if (p->fDoneAtOnce) QueueTaskRequest3(0, p, 2, 0, nullptr);                    // 0x189078(p, 2, 0)
    // +0xC8: the task's exit (EOPID 1406; e.g. v_ext's t1_chalktalk1 1825 -> exit 2135: movie 305, then v_ext again):
    // fired unless a transition is pending (0x2852B0(SW)), its LO word +0x3C | 0x200 first (TODO(sly3): what the bit
    // gates), TriggerExit 0x27FCC8 (frontend.cpp: it fires only an armed exit, state 3) (the world flow code)
    if (std::getenv("SLY2N_EXITPRINT"))
        std::printf("task3: %d activated: exit %d, transition pending %d\n", p->oid, p->ploExit ? p->ploExit->oid : -1,
                    int(g_transition.fPending));
    if (p->ploExit && !g_transition.fPending)
    {
        if (std::getenv("SLY2N_EXITPRINT"))
            std::printf("task3: %d's exit %d triggered (state %d)\n", p->oid, p->ploExit->oid, p->ploExit->nState);
        TriggerExit(p->ploExit);
    }
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
    for (LO* plo : psw->mpcidaplo[sly3::CID_MISSION])
    {
        auto pj = std::make_unique<JOB3>();
        pj->plo = plo;
        pj->oid = plo->oid;
        pj->f6c = NOpt(plo, 1361, 0);
        pj->f70 = NOpt(plo, 1362, 0);
        s_mpoidjob[plo->oid] = pj.get();
        g_tasks3.ajob.push_back(std::move(pj));
    }
    for (LO* plo : psw->mpcidaplo[sly3::CID_TASK])
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
        pt->ploExit = ploFind(NOpt(plo, 1406, -1));   // (EOPID 1406 writes +0xC8: v_ext 1825 -> exit 2135; the world flow code)
        pt->pscpt = ploFind(NOpt(plo, 1408, -1));
        auto akey = [plo](int eopid) {
            const OPT* po = PoptFind(plo, eopid);
            return po ? po->aval[0].an : std::vector<int16_t>{};
        };
        pt->akeyActivated = akey(1423);
        pt->akeyDone = akey(1424);
        pt->akeyDoneBool = akey(1425);
        pt->akeyState = akey(1426);
        pt->akeyAbandoned = akey(1427);
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
        pj->ploExit = NOpt(pj->plo, 1381, -1) < 0 ? nullptr : PloFindSwObjectByOid(psw, NOpt(pj->plo, 1381, -1));
    }
}

// orig3: 0x188CB0 RestoreTask: the saved state (+0xE8; unsaved 0); an abandoned job task (+0xF0) in progress is
// available again (its activations cleared, the job's first-task flag too) when its prerequisites are done; a state
// 0 becomes 1 for a task without prerequisites. Then SetTaskState (in the caller's mode: 1 at a world start, the
// SMs snap and nothing activates).
void RestoreTask3(TASK3* p)
{
    int n = NProgress3(p->akeyState);
    const bool fAbandoned = NProgress3(p->akeyAbandoned) != 0;
    if (fAbandoned && p->pjob && n > 0 && n < 3)
    {
        p->cActivated = 0;
        if (p->pjob->ptaskFirst == p) p->pjob->f7c = 0;
        n = 1;
        for (const TASK3* px : p->aprereq)
            if (px->nState < 2) { n = 0; break; }
    }
    if (n == 0) n = p->aprereq.empty() ? 1 : 0;
    p->cActivated = NProgress3(p->akeyActivated);
    p->cDone = NProgress3(p->akeyDone);
    SetTaskState3(p, n);
}

std::vector<TASK3*> AptaskOrdered3()
{
    std::vector<TASK3*> a;
    std::map<TASK3*, bool> mpf;
    for (auto& pt : g_tasks3.atask) VisitTask3(pt.get(), a, mpf);
    return a;
}

// orig3: 0x18A0D0 (the queued task at the world start): the checkpoint key, the broadcast 0x22, every task restored
// (mode 3), the task made available with what it implies (ChangeTaskState, mode 3) and saved. (The original then
// unloads the states (-1, mode 2) and reloads at the task's checkpoint (0x1856E0); here the start goes on with the
// task as the active one: the reference's --warp W:TASK lands with it available and the job current.)
void StartQueuedTask3(TASK3* px)
{
    SetTaskCur3(px);
    Broadcast3(0x22, px->pjob ? px->pjob->plo : nullptr);
    const std::vector<TASK3*> a = AptaskOrdered3();
    for (TASK3* p : a)
    {
        CMODE3 save(p, 3, nullptr);
        RestoreTask3(p);
    }
    {
        CMODE3 save(px, 3, nullptr);
        ChangeTaskState3(px, 1);
    }
    for (TASK3* p : a)
    {
        CMODE3 save(p, 0, nullptr);
        SaveTask3(p);
    }
    // then unloaded (-1, mode 2: not saved), as the original before its reload: the world start's restore that
    // follows sets every state again from the save, with its objects and SMs
    for (TASK3* p : a)
    {
        CMODE3 save(p, 2, nullptr);
        SetTaskState3(p, -1);
    }
    g_gametasks3.oidJob = px->pjob ? px->pjob->oid : -1;
    g_gametasks3.oidTask = -1;
    // the arrival at the task's checkpoint (the original reloads there, 0x1856E0; Sly 2's StartQueuedTask sets the
    // next point the same way: transition.cpp reads it after the world start)
    if (px->idChk != -1 && g_psw) g_psw->tasks.oidPointNext = px->idChk;
}

// (the copied entry point's name: the world start restores through StartTasks3)
void RestoreTasks3() { StartTasks3(); }

void StartTasks3()
{
    // 0x184AA0 (the transition, flags 4): the queued task, else the job's first task
    if (g_gametasks3.fQueued)
    {
        g_gametasks3.fQueued = false;
        TASK3* px = PtaskFind3(g_gametasks3.oidChk);
        if (!px)
            if (JOB3* pj = PjobFind3(g_gametasks3.oidJob)) px = pj->ptaskFirst;
        if (px) StartQueuedTask3(px);
        else SetTaskCur3(nullptr);
    }
    // 0x184B60: every task restored (mode 1)
    const std::vector<TASK3*> a = AptaskOrdered3();
    for (TASK3* p : a)
    {
        CMODE3 save(p, 1, nullptr);
        RestoreTask3(p);
    }
    // the job and the active task of the keys (cleared first), mode 1
    JOB3* pjob = PjobFind3(g_gametasks3.oidJob);
    TASK3* ptask = PtaskFind3(g_gametasks3.oidTask);
    if (TASK3* pchk = PtaskFind3(g_gametasks3.oidChk)) g_tasks3.ptaskCur = pchk;
    g_gametasks3.oidJob = g_gametasks3.oidTask = -1;
    if (pjob)
    {
        const int nMode = pjob->nMode;
        pjob->nMode = 1;
        JobSwitch3(pjob);
        pjob->nMode = nMode;
    }
    if (ptask)
    {
        CMODE3 save(ptask, 1, nullptr);
        ActivateTask3(ptask);
    }
    // the available tasks that activate themselves (+0x60), not the active one: requested (0x1890B0, mode 0) until
    // the queue settles
    for (int guard = 0; guard < 64; ++guard)
    {
        TASK3* pauto = nullptr;
        for (TASK3* p : g_tasks3.aptaskAvailable)
            if (p->fAutoActivate && !FIsTaskCurrent(p)) pauto = p;
        if (!pauto) break;
        QueueTaskRequest3(0, pauto, 4, 1, nullptr);
        if (FIsTaskCurrent(pauto) || pauto->nState != 1) continue;
        break;  // (refused: the original's loop ends when the request queue is empty)
    }
    // saved (mode 0), then 0x188420 per task (mode 1: +0xC0, the player character inherited from a prerequisite)
    for (TASK3* p : a)
    {
        CMODE3 save(p, 0, nullptr);
        SaveTask3(p);
    }
}

bool FRequestTaskStartByOid3(OID oidTask)
{
    // (0x284568: the sequence reset: TODO(sly3)); the start goes through the world's (re)load in both cases (0x189F38
    // ends in a checkpoint transition too): the keys name the task, the next world start takes it
    if (g_tasks3.pjobCur) DeactivateJob3(g_tasks3.pjobCur);   // (RestartJob 0x187118's part)
    g_gametasks3.oidChk = oidTask;
    g_gametasks3.oidTask = -1;
    g_gametasks3.oidJob = -1;
    g_gametasks3.fQueued = true;
    return PtaskFind3(oidTask) != nullptr;
}

void SaveTasks3()
{
    for (auto& pt : g_tasks3.atask)
    {
        CMODE3 save(pt.get(), 0, nullptr);
        SaveTask3(pt.get());
    }
}

void Broadcast3(int code, LO* plo)
{
    static const bool s_fPrintB = std::getenv("SLY3_BCASTPRINT") != nullptr;
    if (s_fPrintB)
        std::printf("task3: broadcast 0x%X object %d (job %d)\n", code, plo ? plo->oid : -1,
                    g_tasks3.pjobCur ? g_tasks3.pjobCur->oid : -1);
    // the available tasks of the current job: Task_Notify (live ones: message 0x14 {code, object})
    for (TASK3* p : std::vector<TASK3*>(g_tasks3.aptaskAvailable))
        if (p->pjob == g_tasks3.pjobCur && FTaskLive(p)) MessageArgs3(p->plo, 0x14, code, plo);
    // the current job: JobOnEvent 0x186730 (message 0x14 when live; the job's end on 0xC: 0x186EC0, TODO(sly3))
    if (JOB3* pj = g_tasks3.pjobCur)
        if (pj->nMode != 3)
        {
            MessageArgs3(pj->plo, 0x14, code, plo);
            // 0xC (the player's death) in a job that does not survive it (+0x70 clear): 0x186EC0 -> 0x186EE0 the job
            // failed (the broadcast 0x20, its end state: jobend3's Job Failed!, the line -1: the default)
            if (code == 0xC && !pj->f70 && NJobEndState3(pj) == 0)
            {
                Broadcast3(0x20, pj->plo);
                JobEndFail3(pj, -1);
            }
        }
}

int Code3FromCode2(int code2)
{
    // (tools/s3script/bcastmap3.py: the pairs voted by the callers; 0x10 (the player's death) / 0x1D / 0x1E / 0x20 /
    // 0x21 by the receivers: JobOnEvent's 0xC, the job module's order)
    static const int s_amap[][2] = {
        {0x07, 0x02}, {0x08, 0x03}, {0x0C, 0x07}, {0x0D, 0x08}, {0x0F, 0x0A}, {0x10, 0x0C}, {0x11, 0x0D}, {0x12, 0x0E},
        {0x18, 0x10}, {0x1A, 0x1D}, {0x1B, 0x1E}, {0x1C, 0x1F}, {0x1D, 0x20}, {0x1E, 0x22}, {0x1F, 0x23}, {0x20, 0x24},
        {0x21, 0x25}, {0x22, 0x26}, {0x23, 0x27}, {0x24, 0x28}, {0x25, 0x29}, {0x26, 0x2A}, {0x27, 0x2B}, {0x28, 0x3A},
        {0x2C, 0x3E}, {0x2D, 0x3F}, {0x2E, 0x40}, {0x32, 0x45}, {0x36, 0x49}, {0x39, 0x4A}, {0x3A, 0x4B}, {0x3C, 0x4D},
        {0x3D, 0x4E}};
    for (const auto& r : s_amap)
        if (r[0] == code2) return r[1];
    return -1;
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
        if (e.grf & grfState) AddLo(psw, plo);
        else RemoveLo(psw, plo);
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
        if (c != 1 || !FIsLoDerivedFrom(plo, sly3::CID_ALO)) continue;
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
