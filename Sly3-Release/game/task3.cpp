// Sly 3's tasks and jobs (task3.h).

#include "../copsmap3.h"
#include "../duel3.h"
#include "task3.h"
#include "suck3.h"
#include "scptgame3.h"
#include "../challenge3.h"

#include "cond3.h"
#include "trigger3.h"
#include "sly3.h"

#include "../activate.h"
#include "../animfind.h"
#include "../brx.h"
#include "../disc.h"
#include "../modsave.h"
#include "../smrun.h"
#include "../sm.h"
#include "../savedata.h"
#include "../jobend3.h"
#include "../sw.h"
#include "../savedata.h"
#include "../frontend.h"
#include "../transition.h"
#include "../update.h"
#include "../cls.h"
#include "../lo.h"

#include <algorithm>
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

}  // namespace
// orig3: 0x188A38 FTaskDoneJh: unloaded (-1), the saved flag; else done (state 2 or 3)
bool FTaskDone3(const TASK3* p)
{
    if (p->nState == -1)
    {
        // (PC, mod packages: a custom task without a save key of its own: its state from the slot's side file, modsave.h)
        if (p->akeyState.empty() && g_disc.FCustomTasks())
            if (int n = -1; FModTaskState(p->oid, &n)) return n >= 2;
        return NProgress3(p->akeyDoneBool) != 0;
    }
    return p->nState >= 2;
}
namespace
{

// orig3: 0x188DE0 Task_SaveState (not in the modes 1 / 2: a restore, a reload): the counts, the done flag, the
// state, the abandoned flag cleared (then the job's Task_SaveVars 0x187D30: TODO(sly3))
void SaveTask3(TASK3* p)
{
    if (p->nMode == 1 || p->nMode == 2 || p->nState == -1) return;
    // (PC, mod packages: a custom task without a save key of its own keeps its state in the slot's side file, modsave.h)
    if (p->akeyState.empty() && g_disc.FCustomTasks()) FSetModTaskState(p->oid, p->nState);
    WriteProgressS32(p->akeyActivated, p->cActivated);
    WriteProgressS32(p->akeyDone, p->cDone);
    WriteProgressBool(p->akeyDoneBool, FTaskDone3(p));
    WriteProgressS32(p->akeyState, p->nState);
    WriteProgressBool(p->akeyAbandoned, false);
    // orig3: 0x187D30 (the job's save, not in its modes 1 / 2): its count, "is_complete" (0x187CD0: its last task
    // done), its float. Without it no job was ever complete in the save and the chooser opened no episode.
    if (JOB3* pj = p->pjob; pj && pj->nMode != 1 && pj->nMode != 2)
    {
        WriteProgressS32(pj->akeyCount, pj->f7c);
        WriteProgressBool(pj->akeyDoneBool, FJobDone3(pj));
        if (!pj->akeyReal.empty())
        {
            CRef r;
            r.SetF32(pj->gA8);
            FSetProgressKey(pj->akeyReal, r);
        }
    }
}

// ---- the broadcasts' messages (0x14 {code, object}: Task_Notify 0x189DF8 / JobOnEvent 0x186730 / the listeners)
void MessageArgs3(LO* plo, int msg, int n, LO* ploArg)
{
    if (plo && g_taskhost3.pfnMessageArgs) g_taskhost3.pfnMessageArgs(plo, msg, n, ploArg);
}

// orig3: 0x188A20 FIsTaskCurrent (SW +0x4048)
}  // namespace
bool FIsTaskCurrent(const TASK3* p) { return g_tasks3.ptaskActive == p; }
// orig3: 0x187CB8 FIsMissionCurrent (SW +0x4044)
bool FIsJobCurrent(const JOB3* p) { return g_tasks3.pjobCur == p; }
namespace
{

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
    // orig3 0x186338: a chooser's game just started (its request's flag 0x80, SW +0x4348): every task of the job gets
    // the activation count 1 (a challenge / two-player game: the job's intro talks, asked with the script condition
    // 0x760 "first activation", are skipped as the reference's Air Time shows) or 0 (a job replay: played as new), and
    // the job's count the same (0x1862D0)
    if (int n = 0; FChallengeTakeStartFlag3(&n))
    {
        for (auto& upt : g_tasks3.atask)
            if (upt->pjob == p)
            {
                upt->cActivated = n;
                SaveTask3(upt.get());
            }
        p->f7c = n;
        if (p->ptaskFirst) SaveTask3(p->ptaskFirst);
    }
    Broadcast3(0x25, p->plo);
    // orig3: 0x186338's self->Message(0x16): the job's splice handler (e.g. v_ext's follow job 2230: its event-22
    // procedure acquires the guard proxy 139555 that SCPT 2233's 0x62 commands; without it the guard was null and the
    // script's 0x62 wait ended at once). TODO(sly3): the job's objects / SMs (0x185A70 / 0x185DB0 in state 4)
    if (p->plo && g_taskhost3.pfnMessageLo) g_taskhost3.pfnMessageLo(p->plo, 0x16);
}
// orig3: 0x186610 DeactivateJob: the broadcast 0x26
void DeactivateJob3(JOB3* p)
{
    // (0x18AA48(0), the checkpoint task cleared when it is this job's: below as before)
    // in the hubs (worlds 3, 8, 15, 23, 31, 32: 0x25E150) the death's restart point (GAME +0x4428) is the default again
    switch (g_nFilterCur3)
    {
    case 3: case 8: case 15: case 23: case 31: case 32:
        g_transition.oidPointCur = sly3::POINT_DEFAULT;
        g_transition.oidKeyCur = -1;
        break;
    default: break;
    }
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

}  // namespace
// orig3: 0x1878E0's start (the job's end leaves state 0): +0xB4 = the first successor of the last task (+0x58's
// +0x90 list) with an exit (+0xC8), unless one of its other prerequisites (+0x84) is not done (0x188A38)
TASK3* PtaskExitOfJob3(const JOB3* p)
{
    if (!p || !p->ptaskLast) return nullptr;
    for (TASK3* px : p->ptaskLast->anext)
    {
        if (!px->ploExit) continue;
        for (const TASK3* pr : px->aprereq)
            if (pr != p->ptaskLast && !FTaskDone3(pr)) return nullptr;
        return px;
    }
    return nullptr;
}

namespace
{
// orig3: 0x1874E8 JobCompleteClose (jobend3's pfnCompleteClose): no transition pending: the job's exit (+0xC0, its LO
// word +0x3C | 0x200 first: TODO(sly3) the bit) fired (e.g. the prologue's job 1797 -> exit 4236 -> movie 334 -> the
// episode chooser). (Its next task +0xB4: TODO(sly3))
void JobCompleteClose3(JOB3* p)
{
    if (std::getenv("SLY2N_EXITPRINT"))
        std::printf("task3: job %d complete: exit %d, transition pending %d\n", p->oid, p->ploExit ? p->ploExit->oid : -1,
                    int(g_transition.fPending));
    // orig3 0x1874E8: a game of the chooser's lists running (0x2852B0): its end 0x283F70 instead (challenge3.h)
    if (FChallengeRunning3())
    {
        EndChallenge3(g_psw);
        return;
    }
    // orig3 0x1874E8: the job's exit task (+0xB4, chosen by 0x1878E0 when the job's end begins: the first successor of
    // its last task that has an exit (+0xC8) and whose other prerequisites are done; e.g. the gauntlet's last task 2327
    // -> chalk talk 2 1826, exit 2226: movie 306) is requested active (in the world, its exit armed), else the job's
    // own exit (+0xC0) fires
    if (TASK3* ptx = PtaskExitOfJob3(p))
    {
        QueueTaskRequest3(0, ptx, 4, ptx->nMode, p->plo);
        return;
    }
    if (p->ploExit && !g_transition.fPending) TriggerExit(p->ploExit);
}

// orig3: 0x186FE8 (jobend3's pfnRestart, after Job Failed): the job again from its checkpoint task (else its first
// task): RequestTaskStartByOid 0x189F38, the world reloaded. The task's job is the current one (not "changed"), so
// 0x189F38 goes through the wipe 0x36B (the reference's canal chase: 0x289070(record, 0x1A8, flags 4, 0x36B))
void JobRestart3(JOB3* p)
{
    ChallengeRestart3();   // (a challenge's replay: its values reset at the world's start, 0x283A50)
    TASK3* pt = g_tasks3.ptaskCur && g_tasks3.ptaskCur->pjob == p ? g_tasks3.ptaskCur : p->ptaskFirst;
    // (s3-player: 0x189F38 loads the task's own world record (+0xAC, option 1401: 0x25E070), not the current one: a
    // job asked for in its hub whose tasks live elsewhere (Ep5's squid / dive / battle in p_at_sea 32, Ep6's gauntlet
    // in m_vault 37) is taken there, as the reference's --warp 31:4074 (p_ext, then p_at_sea). SLY3_RESTARTCURWORLD=1:
    // the current world as before)
    static const bool s_fCur = std::getenv("SLY3_RESTARTCURWORLD") != nullptr;
    const int iwrec = pt && pt->iworld >= 0 && !s_fCur ? pt->iworld : g_transition.iwrecCur;
    if (pt) RequestTaskStart(g_psw, iwrec, pt->oid, pt->pjob && FIsJobCurrent(pt->pjob) ? 0x36B : -1);
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
    // (a chooser's game running: its end task (challenge3.h: the node's +0x60, else the job's last) done ends it
    // (0x2845B0(.., 2)); the checkpoints only outside a game or in a job replay (the node's mode 1))
    if (FChallengeRunning3())
    {
        if (FChallengeEndTask3(ptask, 2)) JobComplete3(pjob);
    }
    else if (ptask == pjob->ptaskLast) JobComplete3(pjob);
    if (ptask->idChk != -1 && FChallengeCheckpoints3()) SetTaskCur3(ptask);
}

void SetTaskState3(TASK3* p, int n);
void SetTaskStateTree3(TASK3* p, int n);
void ActivateTaskNow3(TASK3* p);

// orig3: 0x188928: the current task left: SW +0x4048 cleared, the broadcast 0x28, the state-5 objects, message 0x17
void DeactivateTaskNow3(TASK3* p)
{
    Log("deactivate", p, 5);
    SetTaskActive3(nullptr);
    // (orig3 0x2845B0(.., 5): a chooser's game that ends when this task is left)
    if (FChallengeEndTask3(p, 5))
    {
        if (p->pjob) JobComplete3(p->pjob);
        return;
    }
    if (FTaskLive(p)) Broadcast3(0x28, p->plo);
    if (FTaskLive(p) && g_taskhost3.pfnApplyObjects) g_taskhost3.pfnApplyObjects(p, 5);
    // (s3-ui) the task's hint (+0x4C, EOPID 1392, while +0x50 / 1393) taken off the hint widget 0x46F770 (0x21E930)
    if (FTaskLive(p))
        if (const int id = NOpt(p->plo, 1392, -1); id != -1 && NOpt(p->plo, 1393, 0) && g_scpthooks3.pfnClearHint) g_scpthooks3.pfnClearHint(id);
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
    // (0x2845B0(0x4BC230, task, 4): a chooser's game that ends when this task becomes active, challenge3.h)
    if (FChallengeEndTask3(p, 4))
    {
        if (p->pjob) JobComplete3(p->pjob);
        return;
    }
    if (p->idChk != -1 && FChallengeCheckpoints3()) SetTaskCur3(p);  // (when 0x4BC230 +0x14 is null or its +0x50 is 1)
    if (p->pjob && !FIsJobCurrent(p->pjob)) JobBegin3(p->pjob);
    if (!FTaskLive(p)) return;
    ++p->cActivated;
    SaveTask3(p);  // orig3: 0x1886F4 (Task_SaveState 0x188DE0: the activation count)
    Broadcast3(0x27, p->plo);
    if (g_taskhost3.pfnApplyObjects) g_taskhost3.pfnApplyObjects(p, 4);
    // (s3-ui) orig3 0x1887xx: the task's hint (+0x4C, EOPID 1392) on the hint widget 0x46F770 until the task is left
    // (0x21E870(widget, +0x4C, -1.0f)) while +0x50 (1393) is set
    if (const int id = NOpt(p->plo, 1392, -1); id != -1 && NOpt(p->plo, 1393, 0) && g_scpthooks3.pfnShowHint) g_scpthooks3.pfnShowHint(id, -1.0f);
    // orig3: 0x1887A8: message 0x16 (vt slot 3) unconditionally: the task's splice handler (v_ext 1825's)
    Message3(p, 0x16);
    if (p->pscpt && g_taskhost3.pfnStartScpt) g_taskhost3.pfnStartScpt(p->pscpt);  // 0x226010
    if (p->fDoneAtOnce) QueueTaskRequest3(0, p, 2, 0, nullptr);                    // 0x189078(p, 2, 0)
    // +0xC8: the task's exit (EOPID 1406; e.g. v_ext's t1_chalktalk1 1825 -> exit 2135: movie 305, then v_ext again):
    // fired unless a transition is pending (0x2852B0(SW)), its LO word +0x3C | 0x200 first (TODO(sly3): what the bit
    // gates), TriggerExit 0x27FCC8 (frontend.cpp: it fires only an armed exit, state 3) (the world flow code)
    if (std::getenv("SLY2N_EXITPRINT"))
        std::printf("task3: %d activated: exit %d, transition pending %d\n", p->oid, p->ploExit ? p->ploExit->oid : -1,
                    int(g_transition.fPending));
    // (orig3 0x1887xx: not while a chooser's game runs, 0x2852B0 on 0x4BC230: a challenge stays in its job's world)
    if (p->ploExit && !g_transition.fPending && !FChallengeRunning3())
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

// orig3: 0x189180: run the list (each request, the ones it adds after it, before the ones waiting)
void RunTaskQueue3()
{
    if (g_tasks3.fRunningQueue) return;
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

// orig3: 0x189240
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
    g_tasks3.areq.push_back(r);
    // dispatch mode 1: the list run when this is its only entry; mode 0 (the world's load / start): it waits
    if (g_tasks3.nDispatch == 1 && g_tasks3.areq.size() == 1) RunTaskQueue3();
}

// ---- the splice methods' bodies (scptgame3.cpp registers them by their Sly 3 thunks)

// orig3: 0x189078 (thunk 0x10E090, EOPID 1409) / 0x1890B0 (0x10E138, 1410): a request queued (0x189240: the record
// takes the task's current mode +0xFC and the context)
void TaskRequestMethod3(TASK3* p, int kind, int n, void* ploContext)
{
    if (p) QueueTaskRequest3(kind, p, n, p->nMode, ploContext);
}

// orig3: 0x189710 / 0x1897C8 / 0x189880 / 0x189938 (thunks 0x10E460.., EOPIDs 1418..1421): the state n forced in
// mode 4 (no context), then the current job from the progress (0x189640)
void TaskForceStateMethod3(TASK3* p, int n)
{
    if (!p) return;
    {
        CMODE3 save(p, 4, nullptr);
        QueueTaskRequest3(1, p, n, p->nMode, nullptr);
    }
    JobFromProgress3();
}

// orig3: 0x188540 (thunk 0x10E640, EOPID 1422)
void TaskActivateMethod3(TASK3* p)
{
    if (p) ActivateTask3(p);
}

// orig3: 0x189640: the job whose first task is done and whose last is not becomes the current one (the last such in
// the world's list), the old one switched out (0x186298) / the new one in (0x186248)
void JobFromProgress3()
{
    JOB3* pfound = nullptr;
    for (auto& pj : g_tasks3.ajob)
        if (pj->ptaskFirst && FTaskDone3(pj->ptaskFirst) && !(pj->ptaskLast && FTaskDone3(pj->ptaskLast)))
            pfound = pj.get();
    if (pfound == g_tasks3.pjobCur) return;
    if (g_tasks3.pjobCur) DeactivateJob3(g_tasks3.pjobCur);
    if (pfound) JobSwitch3(pfound);
}

// orig3: 0x187CD0 (EOPID 1374): the job's last task (+0x58) done
bool FJobDone3(const JOB3* p) { return p && p->ptaskLast && FTaskDone3(p->ptaskLast); }

// a progress path's value (the save's, 0x2BC010 on 0x468D30), for the world load's option-set choice (0x1500F0)
int NProgressPath3(const std::vector<uint16_t>& akey)
{
    std::vector<int16_t> a(akey.begin(), akey.end());
    return NProgress3(a);
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
        {
            auto akeyJ = [plo](int eopid) {
                const OPT* po = PoptFind(plo, eopid);
                return po && !po->aval.empty() ? po->aval[0].an : std::vector<int16_t>{};
            };
            pj->akeyCount = akeyJ(1389);
            pj->akeyDoneBool = akeyJ(1390);
            pj->akeyReal = akeyJ(1387);
            // (its float back from the save (the dynamic difficulty, game/suck3.cpp): the job's save writes it at
            // every task's save, so a job made again at a world load (a restart) kept 0 and erased it)
            if (!pj->akeyReal.empty())
                if (const CRef r = RefGetProgressKey(pj->akeyReal); r.m_tagk == TAGK_F32) pj->gA8 = r.m_g;
        }
        s_mpoidjob[plo->oid] = pj.get();
        g_tasks3.ajob.push_back(std::move(pj));
    }
    for (LO* plo : psw->mpcidaplo[sly3::CID_TASK])
    {
        auto pt = std::make_unique<TASK3>();
        pt->plo = plo;
        pt->oid = plo->oid;
        // +0x40: a task filtered out of this world (made anyway, brx.cpp's fMission) does not react (FTaskLive): its
        // object lists are not applied here. The reference (c_tilt_hall at 30:3721): every task of another world has
        // +0x40 = 1 (m4_rccar's 3680 among them: its list no longer removes the RC car 38546 at the heist's load)
        pt->f40 = plo->fMission ? 1 : 0;
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
        if (const char* sz = std::getenv("SLY3_TASKOPTPRINT"); sz && std::atoi(sz) == plo->oid)   // (debug: a task's options)
            for (const OPT& o : plo->aopt)
                std::printf("task3: %d option %d: %zu values, n %d\n", plo->oid, o.eopid, o.aval.size(), o.aval.empty() ? 0 : o.aval[0].n);
        if (const OPT* po = PoptFind(plo, 1402))
            for (std::size_t i = 0; i < 2 && i < po->aval.size(); ++i) pt->aoidPlayer[i] = po->aval[i].n;
        pt->fDoneAtOnce = NOpt(plo, 1405, 0);
        // (an instance's alias: v_ext 2086 -> 121540, the proxy whose instance is a job marker 35785: the instance, as
        // the reference's 2086 +0x70 = the marker, not the proxy)
        if (const OID oidShown = NOpt(plo, 1407, -1); oidShown >= 0)
        {
            pt->ploShown = ploFind(oidShown);
            if (!pt->ploShown || FCidDerivedFrom(pt->ploShown->Cid(), sly3::CID_PROXY))
                for (LO* px : AploOfOid(psw, oidShown))
                    if (px != pt->ploShown && px->oidInstance == oidShown && !FCidDerivedFrom(px->Cid(), sly3::CID_PROXY)) { pt->ploShown = px; break; }
            if (s_fPrint) std::printf("task3: %d shows %d (oid %d cid 0x%X)\n", plo->oid, oidShown, pt->ploShown ? pt->ploShown->oid : -1, pt->ploShown ? pt->ploShown->Cid() : 0);
        }
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
    // (PC, mod packages: a custom task's state from the slot's side file, modsave.h; none kept: as a task without a key)
    if (p->akeyState.empty() && g_disc.FCustomTasks())
        if (int nMod = -1; FModTaskState(p->oid, &nMod) && nMod > 0 && nMod <= 3) n = nMod;
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
    // (orig3 0x188CB0 reads only the state +0xE8 and the abandoned flag +0xF0: the counts +0x54 / +0x58 start at 0 in
    // every world load; the progress keeps them for the save only. Restored, v_ext's chase 2123 (activated once in
    // v_interpol when its exit fired) counted 2 at its world start activation and 2124 skipped the chase scene.)
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
    for (int i = 0; i < 2; ++i) g_gametasks3.aoidPlayer[i] = px->aoidPlayer[i];   // 0x1856E0 (the original: at its end)
    g_gametasks3.fPlayerNew = true;
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
    // (orig3 0x18A0D0 sets GAME +0x149C = the task's oid: the world start 0x184B60 then activates it (0x188540). Natively
    // done for a game started from the chooser's lists (challenge3.h: Cops and Robbers' intro task 1811 has no other
    // activation); the story's queued starts keep -1 (available only) as they were tuned. TODO(s3-script): the
    // original's activation for every queued start.)
    g_gametasks3.oidTask = FChallengeRunning3() ? px->oid : -1;
    // the arrival at the task's checkpoint (the original reloads there, 0x1856E0; Sly 2's StartQueuedTask sets the
    // next point the same way: transition.cpp reads it after the world start)
    if (px->idChk != -1 && g_psw) g_psw->tasks.oidPointNext = px->idChk;
}

// (the copied entry point's name: the world start restores through StartTasks3)
void RestoreTasks3() { StartTasks3(); }

namespace { bool s_fActivatePending = false; }
void ActivateTasksAtStartCore3(const std::vector<TASK3*>& a);
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
    // (orig3 0x25C2A8: 0x184AA0 (the queued task) comes before the players' placement 0x25B9C0, 0x184B60 (the job's switch,
    // the task's activation and the queue) after it. Natively both ran in the batch before transition.cpp places the
    // players; a challenge's job (Cops and Robbers: its begin places both players at the spawn points farthest apart,
    // splice s6319) then had its placement undone by the checkpoint's. For the chooser's games the rest waits for the
    // placement: ActivateTasksAtStart3, from transition.cpp. TODO(s3-script): the same order for every start.)
    if (FChallengeRunning3())
    {
        s_fActivatePending = true;
        return;
    }
    ActivateTasksAtStartCore3(a);
}

void ActivateTasksAtStart3()
{
    if (!s_fActivatePending) return;
    s_fActivatePending = false;
    ActivateTasksAtStartCore3(AptaskOrdered3());
}

void ActivateTasksAtStartCore3(const std::vector<TASK3*>& a)
{
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
    // orig3 0x184B60's loop: the queue runs (dispatch mode 1; the requests the restores and the activation made wait
    // until here), then the last available task that activates itself (+0x60, the list 0x4794B8) and is not the
    // active one is requested (mode 1, queued with dispatch 0), until the queue stays empty
    for (int guard = 0; guard < 256; ++guard)
    {
        g_tasks3.nDispatch = 1;
        RunTaskQueue3();
        TASK3* pbest = nullptr;
        for (TASK3* p : g_tasks3.aptaskAvailable)
            if (p->fAutoActivate && !FIsTaskCurrent(p)) pbest = p;
        if (pbest)
        {
            CMODE3 save(pbest, 1, nullptr);
            g_tasks3.nDispatch = 0;
            QueueTaskRequest3(1, pbest, 4, pbest->nMode, nullptr);
        }
        if (g_tasks3.areq.empty()) break;
    }
    g_tasks3.nDispatch = 1;
    // saved (mode 0), then 0x188420 per task (mode 1: +0xC0, the player character inherited from a prerequisite)
    for (TASK3* p : a)
    {
        CMODE3 save(p, 0, nullptr);
        SaveTask3(p);
    }
}

bool FRequestTaskStartByOid3(OID oidTask)
{
    // the start goes through the world's (re)load in both cases (0x189F38 ends in a checkpoint transition too): the keys
    // name the task, the next world start takes it. 0x189F38: a task of another job than the current one (SW +0x4044)
    // stops a running challenge silently (0x284568; natively the current job is left by a restart: the challenge's then)
    if (FChallengeRunning3())
        if (const TASK3* ptask = PtaskFind3(oidTask))
        {
            const OID oidJobCur = g_tasks3.pjobCur ? g_tasks3.pjobCur->oid : OidChallengeJob3();
            if ((ptask->pjob ? ptask->pjob->oid : -1) != oidJobCur) AbortChallenge3();
        }
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
    // (PC, copsmap3.h: a native Cops and Robbers round scores the hits / knock-outs and places the players again after
    // a death itself: no world reload)
    if (cops3::FOnBroadcast(code, plo)) return;
    if (duel3::FOnBroadcast(code, plo)) return;   // (PC, duel3.h: the Biplane Duel's free-for-all scores / respawns its deaths)
    static const bool s_fPrintB = std::getenv("SLY3_BCASTPRINT") != nullptr;
    if (s_fPrintB)
        std::printf("task3: broadcast 0x%X object %d %p (job %d)\n", code, plo ? plo->oid : -1, (void*)plo,
                    g_tasks3.pjobCur ? g_tasks3.pjobCur->oid : -1);
    // 0x1855F0: the player's death (0xC) with no job: the current world again at the last checkpoint's point (GAME
    // +0x4428: 0x1A8, or the checkpoint the world was entered at (0x25B9C0); transition.cpp's oidPointCur), flags 0, the
    // wipe 0x36B (0x289070)
    // (not while a world change or its wipe is under way: e.g. the job's restart, which natively leaves no current
    // job (FRequestTaskStartByOid3), and a death broadcast every frame after it)
    if (code == 0xC && !g_tasks3.pjobCur && g_psw && !g_transition.fPending && !g_transition.fWipeReq &&
        g_transition.wipes == WIPES_Idle && !g_gametasks3.fQueued)
        RequestTransition(g_psw, nullptr, g_transition.oidPointCur != -1 ? g_transition.oidPointCur : sly3::POINT_DEFAULT, -1, 0, 0x36B);
    SuckOnEvent3(code, plo);   // 0x171AA0: the dynamic difficulty (game/suck3.cpp)
    // the available tasks of the current job: Task_Notify (live ones: message 0x14 {code, object})
    for (TASK3* p : std::vector<TASK3*>(g_tasks3.aptaskAvailable))
        if (p->pjob == g_tasks3.pjobCur && FTaskLive(p)) MessageArgs3(p->plo, 0x14, code, plo);
    // the listeners 0x479B98: the class 0x93 objects in the world (added to the list by their vt slot 11 0x177220,
    // removed by slot 12): 0x1772D8 their message 0x14 {code, object}
    if (g_psw)
        for (LO* pl : std::vector<LO*>(g_psw->mpcidaplo[0x93]))
            if (sly3::FLoInWorld3(pl)) MessageArgs3(pl, 0x14, code, plo);
    // the current job: JobOnEvent 0x186730 (message 0x14 when live; the job's end on 0xC: 0x186EC0, TODO(sly3))
    if (JOB3* pj = g_tasks3.pjobCur)
        if (pj->nMode != 3)
        {
            MessageArgs3(pj->plo, 0x14, code, plo);
            // 0xC (the player's death) in a job that does not survive it (+0x70 clear): 0x186EC0(job, -1, 0xC) ->
            // 0x186EE0 the job failed (jobend3: the broadcast 0x20, its end state: Job Failed!, the line -1: the default)
            if (code == 0xC && !pj->f70 && NJobEndState3(pj) == 0) JobEndFail3(pj, -1);
        }
    // the challenge manager 0x2849A8 last (challenge3.h: the running game's commands)
    ChallengeOnEvent3(code, plo);
}

int Code3FromCode2(int code2)
{
    // (tools/s3script/bcastmap3.py: the pairs voted by the callers; 0x10 (the player's death) / 0x1D / 0x1E / 0x20 /
    // 0x21 by the receivers: JobOnEvent's 0xC, the job module's order. 0x15 -> 0x0F: a guard's hit reaction, orig3 0x16D038 /
    // 0x16D0C0 (the hit jobs 0xCE / 0xCF Enter: 0x1854C8(GAME, 0xF, guard)); 0x17 -> 0x10: the knock-out, 0x16D1B8 sends 0x10
    // whoever the hit's source (Sly 2: 0x17 another's, 0x18 the player's))
    static const int s_amap[][2] = {
        {0x07, 0x02}, {0x08, 0x03}, {0x0C, 0x07}, {0x0D, 0x08}, {0x0F, 0x0A}, {0x10, 0x0C}, {0x11, 0x0D}, {0x12, 0x0E},
        {0x15, 0x0F}, {0x17, 0x10}, {0x18, 0x10}, {0x19, 0x11}, {0x1A, 0x1D}, {0x1B, 0x1E}, {0x1C, 0x1F}, {0x1D, 0x20}, {0x1E, 0x22}, {0x1F, 0x23}, {0x20, 0x24},
        {0x21, 0x25}, {0x22, 0x26}, {0x23, 0x27}, {0x24, 0x28}, {0x25, 0x29}, {0x26, 0x2A}, {0x27, 0x2B}, {0x28, 0x3A},
        {0x2C, 0x3E}, {0x2D, 0x3F}, {0x2E, 0x40}, {0x32, 0x45}, {0x36, 0x49}, {0x39, 0x4A}, {0x3A, 0x4B}, {0x3C, 0x4D},
        {0x3D, 0x4E}};
    // (SLY3_HITBCAST0=1: the hit jobs' 0x15 / 0x17 not sent, as before round 23)
    static const bool s_fNoHit = std::getenv("SLY3_HITBCAST0") != nullptr;
    if (s_fNoHit && (code2 == 0x15 || code2 == 0x17)) return -1;
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

// (PC) the object of that oid in the instance whose root object is ploRoot: the group's row holding it (WE aplo,
// cloLoaded objects a row) and the objects they own (their namespaces: m_ext's turret root 37350 owns its gun 37351,
// whose child is the shield 16520), each one itself or under it
static LO* PloInInstance3(SW* psw, LO* ploRoot, OID oid)
{
    for (const WE& we : psw->awe)
    {
        if (we.cloLoaded <= 0) continue;
        const auto it = std::find(we.aplo.begin(), we.aplo.end(), ploRoot);
        if (it == we.aplo.end()) continue;
        const std::size_t ib = std::size_t(it - we.aplo.begin()) / std::size_t(we.cloLoaded) * std::size_t(we.cloLoaded);
        std::vector<LO*> aplo;
        for (std::size_t j = ib; j < ib + std::size_t(we.cloLoaded) && j < we.aplo.size(); ++j) aplo.push_back(we.aplo[j]);
        for (std::size_t i = 0; i < aplo.size() && aplo.size() < 4096; ++i)
            for (LO* q : aplo[i]->aploOwned)
                if (std::find(aplo.begin(), aplo.end(), q) == aplo.end()) aplo.push_back(q);
        for (LO* pr : aplo)
            if (pr->oid == oid) return pr;
        for (const auto& pl : psw->aploAll)
            if (pl->oid == oid)
                for (LO* pr : aplo)
                    if (FLoUnder(pl.get(), pr)) return pl.get();
        return nullptr;
    }
    return nullptr;
}

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
        LO* plo = nullptr;
        if (!ploOwner && e.oidOwner != -1)
        {
            // a PROXY's oid names its instance (0x17E1B8 kind 5 also searches the instance names, 0x17DBE8): the object
            // is looked for among that instance's objects (its group's row), e.g. m_ext's turret tasks 4441..4454 (owners
            // the proxies 15994..16007) take their own turret's shield 16520 out (the original's: removed at the start)
            for (const auto& pl : psw->aploAll)
                if (pl->oidInstance == e.oidOwner) { ploOwner = pl.get(); break; }
            if (!ploOwner) continue;
            plo = PloInInstance3(psw, ploOwner, e.oid);
        }
        else plo = ploOwner ? PloFindSwNearest(ploOwner, e.oid) : PloFindSwObjectByOid(psw, e.oid);
        if (!plo)  // a PROXY's oid names its instance (the reference adds the instance's root: 15346 -> 15345's)
            for (const auto& pl : psw->aploAll)
                if (pl->oidInstance == e.oid && (!ploOwner || FLoUnder(pl.get(), ploOwner))) { plo = pl.get(); break; }
        if (s_fPrint)
            std::printf("task3:   objects %d n %d: %d (owner %d, grf 0x%x, filter %d of %d) %s\n", p->oid, n, e.oid, e.oidOwner, e.grf, e.a, g_nFilterCur3,
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
    // (debug) SLY3_TASKGRAPH=1: every task with its job, prerequisites, successors, script, exit, checkpoint, flags
    if (std::getenv("SLY3_TASKGRAPH"))
    {
        for (auto& pj : g_tasks3.ajob)
            std::fprintf(pf, "job %d first %d last %d exit %d name %s\n", pj->oid, pj->ptaskFirst ? pj->ptaskFirst->oid : -1,
                         pj->ptaskLast ? pj->ptaskLast->oid : -1, pj->ploExit ? pj->ploExit->oid : -1,
                         StrProgressKey(pj->akeyDoneBool).c_str());
        for (auto& pt : g_tasks3.atask)
        {
            // (s3-player: the players' characters while the task runs, option 1402: a line of its own)
            if (pt->aoidPlayer[0] != -1 || pt->aoidPlayer[1] != -1) std::fprintf(pf, "taskplayers %d: %d %d\n", pt->oid, pt->aoidPlayer[0], pt->aoidPlayer[1]);
            std::fprintf(pf, "task %d job %d state %d scpt %d exit %d chk %d auto %d atonce %d pre", pt->oid, pt->pjob ? pt->pjob->oid : -1,
                         pt->nState, pt->pscpt ? pt->pscpt->oid : -1, pt->ploExit ? pt->ploExit->oid : -1, pt->idChk,
                         pt->fAutoActivate, pt->fDoneAtOnce);
            for (const TASK3* px : pt->aprereq) std::fprintf(pf, " %d", px->oid);
            std::fprintf(pf, " next");
            for (const TASK3* px : pt->anext) std::fprintf(pf, " %d", px->oid);
            std::fprintf(pf, " name %s", StrProgressKey(pt->akeyState).c_str());
            if (pt->iworld >= 0) std::fprintf(pf, " world %d", pt->iworld);   // (option 1401: the task's own world)
            std::fprintf(pf, "\n");
        }
        // (s3-player) the uses (class 0x21 and below) that set tasks (option 1170: +0x15C count, +0x160 {task, state}
        // pairs, orig3 0x184130 -> 0x161F48), with their places: a route takes them (teleport there, CIRCLE)
        if (g_psw)
            for (const auto& up : g_psw->aploAll)
            {
                LO* plo = up.get();
                if (!FCidDerivedFrom(plo->Cid(), S3CID(0x23))) continue;   // (Sly 3 USE 0x21 = Sly 2 0x23)
                const int c = static_cast<int>(plo->U32Field(0, 0x15C, 0));
                if (c <= 0 || c > 8) continue;
                const glm::vec3 pos = PosLoWorld(plo);
                std::fprintf(pf, "usetasks %d cid 0x%X inworld %d at %.0f %.0f %.0f:", plo->oid, int(plo->Cid()), int(plo->fInWorld), pos.x, pos.y, pos.z);
                for (int k = 0; k < c; ++k)
                    std::fprintf(pf, " {%d %d}", static_cast<int>(plo->U32Field(0, 0x160 + 8 * k)), static_cast<int>(plo->U32Field(0, 0x164 + 8 * k)));
                std::fprintf(pf, "\n");
            }
        // (s3-player, debug: SLY3_OPTDUMP=OID: that object's loaded options)
        if (const char* szo = std::getenv("SLY3_OPTDUMP"); szo && g_psw)
            for (const auto& up : g_psw->aploAll)
                if (up->oid == std::atoi(szo))
                    for (const OPT& o : up->aopt)
                    {
                        std::fprintf(pf, "optdump %d cid 0x%X eopid %d:", up->oid, int(up->Cid()), o.eopid);
                        for (const OPTVAL& v : o.aval) std::fprintf(pf, " %d/%g", v.n, double(v.ag[0]));
                        std::fprintf(pf, "\n");
                    }
        // (s3-player, debug: SLY3_FINDVAL=N: every object with an option value N, e.g. a task's oid)
        if (const char* szv = std::getenv("SLY3_FINDVAL"); szv && g_psw)
            for (const auto& up : g_psw->aploAll)
                for (const OPT& o : up->aopt)
                    for (size_t k = 0; k < o.aval.size(); ++k)
                        if (o.aval[k].n == std::atoi(szv))
                            std::fprintf(pf, "findval %d cid 0x%X eopid %d [%zu] inworld %d\n", up->oid, int(up->Cid()), o.eopid, k, int(up->fInWorld));
        // (s3-player) the prizes / pickups that close a task when collected (EOPID 968: the carryable's task)
        if (g_psw)
            for (const auto& up : g_psw->aploAll)
                for (const OPT& o : up->aopt)
                    if (o.eopid == 489 && !o.aval.empty())
                    {
                        const glm::vec3 pos = PosLoWorld(up.get());
                        std::fprintf(pf, "triggertask %d cid 0x%X inworld %d at %.0f %.0f %.0f: task %d %d\n", up->oid, int(up->Cid()), int(up->fInWorld), pos.x, pos.y, pos.z,
                                     o.aval[0].n, o.aval.size() > 1 ? o.aval[1].n : -9);
                    }
                    else if (o.eopid == 968 && !o.aval.empty())
                    {
                        const glm::vec3 pos = PosLoWorld(up.get());
                        std::fprintf(pf, "prizetask %d cid 0x%X inworld %d parent %d at %.0f %.0f %.0f: task %d\n", up->oid, int(up->Cid()), int(up->fInWorld),
                                     up->paloParent ? up->paloParent->oid : -1, pos.x, pos.y, pos.z, o.aval[0].n);
                    }
    }
}

void JobCompleteNow3(JOB3* p) { if (p) JobComplete3(p); }

// orig3: 0x188460 (TASK's update, every frame): the LO's update, the time limit (+0x64: TODO(sly3)), then the shown
// object +0x70 added while the task is available (1) and no job runs (SW +0x4044), else removed: v_ext's job markers
// (group 45's seven instances: only the available job's is in the world, the reference's RAM at vblank 4900).
// Natively the tasks have no update-table entry: a pass over them each frame (the reference removes the markers every
// frame from 0x18852C; `SLY3_NOTASKSHOWN=1`: off)
namespace
{
struct INITTASK3UPDATE
{
    INITTASK3UPDATE()
    {
        RegisterSwPass(0x1F77C0, [](SW* psw) {
            static const bool s_fOff = std::getenv("SLY3_NOTASKSHOWN") != nullptr;
            if (s_fOff || !psw) return;
            for (const auto& pt : g_tasks3.atask)
                if (pt->ploShown)
                {
                    if (pt->nState == 1 && !g_tasks3.pjobCur) { if (!pt->ploShown->fInWorld) AddLo(psw, pt->ploShown); }  // vt slot 7
                    else if (pt->ploShown->fInWorld) RemoveLo(psw, pt->ploShown);                                    // vt slot 8
                }
        });
    }
} s_inittask3update;
}  // namespace
