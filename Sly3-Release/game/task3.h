#pragma once

// Sly 3's tasks and jobs (classes TASK 0xA9 / MISSION 0xA8; module orig3 0x184918..0x18AFB0, Sly 2's 0x161478..
// 0x166270 at 0.45..0.97). Re-derived from the Sly 3 asm and checked against the reference's FNTRACE of the m_ext
// prologue (the Sly 3 class loaders):
//   world start (vblank 1323): RestoreTask 0x188CB0 per task: the state from the save (NProgress +0xE8) or, unsaved,
//     1 when the task has no prerequisites (4253) else 0;
//   the attract skip (2630): SCPT 4262's 0x1E -> 0x185908 -> RequestTaskState(4253, 3) -> successors: 4266 -> 1 ->
//     (+0x60) Force(4266, 4) -> ActivateTaskNow: the current task (SW +0x4048, GAME 0x36C700 +0x149C), SCPT 4268;
//   START on the title (3031): 4268's 0x1E -> Request(4266, 2) -> 1798 -> 1 (its objects: trigger 16374) ->
//     the trigger's 0x185908 {1798, -1} -> activate 1798: 4266 deactivated, SetTaskCur (+0x14A0), JobBegin(1797) ->
//     SetJobCur (+0x1498), SCPT 4271 (the binocucom talk).
//
// The task's fields (Sly 3 offsets, from the EOPIDs that m_ext's 115 tasks / 9 jobs use):
//   +0x44 state (-1 none, 0 locked, 1 available, 2 done, 3 done and closed; requests 4 activate, 5 deactivate),
//   +0x48 the time of the last change, +0x54 times activated, +0x58 times done, +0x60 activate when available (1399),
//   +0x6C its job (1391), +0x84/+0x88 the prerequisites (the loaded U8 x S32 list), +0x90/+0x94 the successors (the
//   second list), +0x98..+0xA4 the per-state objects / state machines (1416 / 1417 methods), +0xA8 a checkpoint (1400,
//   -1 none), +0xAC its world (1401), +0xC4 done at once when activated (1405), +0xC8 an exit (1406), +0xCC its SCPT
//   (1408, started when activated), +0xD0..+0xF0 the progress paths (1423..1427), +0xF8 the collected target state,
//   +0xFC / +0x100 the change's mode / context (saved and restored around calls).
//   Job: +0x54 / +0x58 the first / last task (1364 / 1365), +0x6C (1361), +0x7C, +0xF0 / +0xF4 mode / context.

#include "../lo.h"

#include <functional>
#include <string>
#include <vector>

struct SW;
struct JOB3;

struct TASK3
{
    LO* plo = nullptr;
    OID oid = -1;
    int nState = -1;             // +0x44
    float tState = 0.0f;         // +0x48
    int cActivated = 0;          // +0x54
    int cDone = 0;               // +0x58
    int fAutoActivate = 0;       // +0x60 (EOPID 1399)
    JOB3* pjob = nullptr;        // +0x6C (1391)
    std::vector<TASK3*> aprereq; // +0x84 / +0x88
    std::vector<TASK3*> anext;   // +0x90 / +0x94
    // 1416 (0x10E260), 0x14 bytes: +0 the object, +8 the states it shows in, +4 its owner (found near it), +0xC the
    // world (-1 any), +0x10
    struct OBJE { OID oid = -1; int grf = 0; OID oidOwner = -1; int a = -1, b = 0; };
    std::vector<OBJE> aobje;     // +0x98 / +0x9C
    // 1417 (0x10E358), 0x1C bytes: +0 the state (-1 any: the goal by the state, 0x6E3 + n), +4 the object, +8 its SM
    // (-1 any), +0xC the goal state, +0x10 only when restoring, +0x14 the world, +0x18
    struct SME { int nState = -1; OID oid = -1; OID oidSm = -1; OID oidGoal = -1; int fInstant = 0; int iworld = -1; int x18 = 0; };
    std::vector<SME> asme;       // +0xA0 / +0xA4
    int idChk = -1;              // +0xA8 (1400)
    int iworld = -1;             // +0xAC (1401)
    // +0xB0 / +0xB4 (1402): the players' characters while the task runs (oids; -1 none: e.g. the heist's end 2466 and
    // the canal run 2477: Bentley 25; v_ext's turf wars: Sly 24). StartTask 0x18A0D0 copies them to GAME +0x10 (0x1856E0)
    OID aoidPlayer[2] = {-1, -1};
    int fDoneAtOnce = 0;         // +0xC4 (1405)
    LO* ploShown = nullptr;      // +0x70 (1407): in the world while the task is available and no job runs (0x188460)
    LO* ploExit = nullptr;       // +0xC8 (1406)
    LO* pscpt = nullptr;         // +0xCC (1408)
    int nTarget = -1;            // +0xF8
    int nMode = 0;               // +0xFC
    void* ploContext = nullptr;  // +0x100
    int f40 = 0;                 // +0x40 (the LO's: non-zero, the task does not react)
    // the progress paths (options of kind 22, EOPIDs 1423..1427): +0xD0 times activated, +0xD8 times done, +0xE0 done
    // (a bool, FTaskDoneJh 0x188A38), +0xE8 the state, +0xF0 abandoned (a bool: restored as available)
    std::vector<int16_t> akeyActivated, akeyDone, akeyDoneBool, akeyState, akeyAbandoned;
};

struct JOB3
{
    LO* plo = nullptr;
    OID oid = -1;
    TASK3* ptaskFirst = nullptr; // +0x54 (1364)
    TASK3* ptaskLast = nullptr;  // +0x58 (1365)
    int f6c = 0;                 // +0x6C (1361)
    int f70 = 0;                 // +0x70 (1362: the job survives the player's death, JobOnEvent 0x186730)
    int f7c = 0;                 // +0x7C
    LO* ploExit = nullptr;       // +0xC0 (1381): the exit fired when the job completes (JobCompleteClose 0x1874E8)
    int nMode = 0;               // +0xF0
    void* ploContext = nullptr;  // +0xF4
    // the job's end (jobend3.cpp: the state machine 0x1878E0 / 0x1875B0 keeps these per job)
    // the progress paths (kind 22 options): +0xD0 (EOPID 1389) the count +0x7C, +0xD8 (1390) "is_complete" (its last
    // task done: what the episode chooser reads), +0xE0 (1387) the float +0xA8
    std::vector<int16_t> akeyCount, akeyDoneBool, akeyReal;
    float gA8 = 0.0f;            // +0xA8
    int nEnd = 0;                // +0xB8 its end state (0 none)
    float tEnd = 0.0f;           // +0xBC the time of its last change
    LO* pscptFail = nullptr;     // +0xC4 the failing script
    int idFail = -1;             // +0xC8 the failure line
};

// The task system of the loaded world (SW +0x4044 / +0x4048 / +0x404C and their oids at GAME 0x36C700 +0x1498 /
// +0x149C / +0x14A0, the request queue 0x36C6E8, the busy flag 0x36C6E0).
struct TASKS3
{
    std::vector<std::unique_ptr<TASK3>> atask;
    std::vector<std::unique_ptr<JOB3>> ajob;
    JOB3* pjobCur = nullptr;     // SW +0x4044 (oid at 0x36DB98: GAME +0x1498)
    TASK3* ptaskActive = nullptr;// SW +0x4048 (+0x149C): FIsTaskCurrent 0x188A20
    TASK3* ptaskCur = nullptr;   // SW +0x404C (+0x14A0)
    std::vector<TASK3*> aptaskAvailable;  // 0x4794B8: the tasks in state 1
    bool fBusy = false;          // 0x36C6E0 (0x189B08)
    struct REQ { int kind = 0; TASK3* ptask = nullptr; int n = 0; int nMode = 0; void* ploContext = nullptr; };
    std::vector<REQ> areq;       // 0x36C6E8 (+0x08 the list, +0x14 the cursor)
    bool fRunningQueue = false;
    std::size_t ireqNext = 0;
    // the queue's dispatch mode (0x36C6E8 +0: 0x189160): 1 a request runs at once (when none runs), 0 they wait; the
    // world load sets 0 (0x189118 from the transition 0x25C2A8), the world start's loop (0x184B60) runs them
    int nDispatch = 0;
};
extern TASKS3 g_tasks3;

// What the game plugs in (game3.cpp): the task's SCPT started (0x226010: StartScpt(scpt, PjobFind 0x1F4AD0)), the
// per-state objects shown / hidden (ApplyTaskObjects 0x185A70), the clock.
struct TASKHOST3
{
    std::function<void(LO* pscpt)> pfnStartScpt;
    std::function<void(TASK3*, int n)> pfnApplyObjects;
    std::function<float()> pfnTime;
    // the task's messages to its object (vt slot 3 0x1C86B8: the splice handler bound to the message's symbol; e.g.
    // 0x18 after a state change: m_ext 4253's handler starts SCPT 4262 in state 1) (the Sly 3 boot to gameplay)
    std::function<void(TASK3*, int msg)> pfnMessage;
    // the same with arguments (the broadcasts' message 0x14 {code, object})
    std::function<void(LO* plo, int msg, int n, LO* ploArg)> pfnMessageArgs;
    // a message with no arguments to any object (the job's 0x16 at its activation, 0x186338)
    std::function<void(LO* plo, int msg)> pfnMessageLo;
    // a task without job and prerequisites done: the autosave (0x1CFE08; the world flow code)
    std::function<void()> pfnAutosave;
};
extern TASKHOST3 g_taskhost3;

// orig3: 0x185A70 ApplyTaskObjects: the entries of the states in the group of n (4 / 5: {4, 5}, else {0..3}) are
// added (AddLo, vt slot 8) when they show in n, else removed (RemoveLo, slot 9); trigger3.h's AddLo3 / RemoveLo3
void ApplyTaskObjects3(SW* psw, TASK3* p, int n);
// orig3: 0x185DB0 / 0x185C30 ApplyTaskSms: the SMs' goals for the state n (at once in the modes 1 / 4: SnapSmaGoal
// 0x239FE0, else SetSmaGoal 0x239CE8)
void ApplyTaskSms3(SW* psw, TASK3* p, int n);

// The game's task keys (GAME 0x36C700 +0x1498 / +0x149C / +0x14A0: the current job, the active task, the checkpoint
// task), kept across worlds (the world start's 0x184B60 restores the job and the active task from them); fQueued:
// a task start queued by RequestTaskStartByOid 0x189E70 (the transition's flags | 6) for the next world start
struct GAMETASKS3
{
    OID oidJob = -1, oidTask = -1, oidChk = -1;
    // GAME +0x10 / +0x14 (0x1856E0 / 0x185700): the players' characters, which the world start 0x25B9C0 adds and makes
    // the players (the others of the list 0x344050 removed); set by a task's start, -1 none
    OID aoidPlayer[2] = {-1, -1};
    bool fPlayerNew = false;   // (native: set by a task's start, taken by the next world start's player selection)
    bool fQueued = false;
};
extern GAMETASKS3 g_gametasks3;

// orig3: 0x1854C8 the task broadcast (code, object): 0x1855F0 (code 0xC with no job: the world again at the last
// checkpoint), 0x171AA0 (the dynamic difficulty: TODO(sly3)), the available tasks of the current job (Task_Notify
// 0x189DF8: message 0x14 {code, object}), the listeners of 0x479B98 (the class 0x93 objects: message 0x14), the current
// job (JobOnEvent 0x186730), 0x2849A8 (a challenge's: none natively). Codes are Sly 3's (Code3FromCode2 for the copied Sly 2 callers).
void Broadcast3(int code, LO* plo);
// The Sly 3 broadcast code of a Sly 2 one (tools/s3script/bcastmap3.py: the callers paired by the function map;
// -1: none)
int Code3FromCode2(int code2);
// orig3: 0x184AA0 + 0x184B60 (the transition's world start): the queued task started, every task restored from the
// save, the job / active task from the keys, the available auto-activating tasks activated, then saved
void StartTasks3();
// (a chooser's game: the job's switch and the task's activation of the world start, after the players' placement:
// orig3 0x25C2A8's 0x184B60 after 0x25B9C0; transition.cpp)
void ActivateTasksAtStart3();
// orig3: 0x189E70 RequestTaskStartByOid: a task of this world started now (0x189F38), else queued for the world the
// caller loads (false: not in this world)
bool FRequestTaskStartByOid3(OID oidTask);
// orig3: 0x188DE0 Task_SaveState of every task (the save writer's: the progress up to date)
void SaveTasks3();

// The task records of a loaded world (after the load, before StartSw: the options resolved to objects)
void InitTasks3(SW* psw);
// orig3: 0x188CB0 RestoreTask for every task (StartSw's), prerequisites first (VisitTask 0x18A900)
void RestoreTasks3();
TASK3* PtaskFind3(OID oid);
JOB3* PjobFind3(OID oid);
// orig3: 0x185908 ApplyCheckpointTasks: per pair {oid, state}: the task found (0x105 search) and 0x189078's request
void ApplyCheckpointTasks3(const int* apair, int cpair, void* ploContext);
// orig3: 0x189240 (kind 0: RequestTaskState 0x189300, kind 1: ForceTaskState 0x189500), run now unless a request is
// running (then after it, in order)
void QueueTaskRequest3(int kind, TASK3* ptask, int n, int nMode, void* ploContext);
// (tests: the keys the reference shows at GAME 0x36C700 +0x1498 / +0x149C / +0x14A0)
void TaskKeys3(int akey[3]);
// SLY3_TASKPRINT=1: every state change printed
void PrintTasks3(FILE* pf);

// The splice methods' bodies (orig3 thunks 0x10E090 / 0x10E138: kind 0 RequestTaskState / 1 ForceTaskState, queued
// with the task's mode; 0x10E460..0x10E5C8: the state forced in mode 4; 0x10E640: ActivateTask 0x188540)
void TaskRequestMethod3(TASK3* p, int kind, int n, void* ploContext);
void TaskForceStateMethod3(TASK3* p, int n);
void TaskActivateMethod3(TASK3* p);
// orig3: 0x189640: the current job from the progress (first task done, last not)
void JobFromProgress3();
// orig3: 0x188A38 FTaskDoneJh (EOPID 1395) / 0x187CD0 the job's last task done (1374)
bool FTaskDone3(const TASK3* p);
bool FJobDone3(const JOB3* p);
bool FIsTaskCurrent(const TASK3* p);
bool FIsJobCurrent(const JOB3* p);
// orig3: 0x1878E0's start: the job's exit task (+0xB4): its last task's first successor with an exit whose other
// prerequisites are done (null: none)
TASK3* PtaskExitOfJob3(const JOB3* p);
// a progress path's value (0x2BC010 on the save buffer 0x468D30; 0: none)
int NProgressPath3(const std::vector<uint16_t>& akey);
void JobCompleteNow3(JOB3* p);   // orig3 0x1871C0 (task3.cpp JobComplete3)
