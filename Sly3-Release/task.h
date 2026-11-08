#pragma once

// Jobs (cid 0x84, "MISSION") and tasks (cid 0x85) with the task manager (orig 0x2DEB30, the SW fields +0x3AFC..
// +0x3C8C): the functions 0x161478..0x166270. A job is an episode's heist; its tasks form a graph (each task's
// prerequisites +0x98, its successors +0xA4). A task's state (+0x54): -1 none, 0 locked, 1 available, 2 done,
// 3 done and closed (4 / 5 the "start" / "end" groups of 0x1620B0). The states live in the save's progress
// (the progress references +0xC8..+0xE8); the world's objects follow them (0x1620B0: per state, objects added
// or removed; 0x162250: state-machine states), and the game's events reach them through the broadcast
// 0x1619A0 (numbers 1..60: world start 4, after the load 5, checkpoint 0x1A, ...).

#include "lo.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct SW;
struct JOB;

// 0x163838 / 0x1626C0: an object added in the states of grf, removed in the others (of the same group)
struct TASKOBJE
{
    OID oidA = -1;   // the object
    OID oidB = -1;   // its owner (-1: global)
    uint32_t grf = 0;
    int iwrec = -1;  // only in this world (-1: any)
};
// 0x1638A0 / 0x162728: an object's state-machine state for a task state
struct TASKSME
{
    int nState = -1;  // -1: by the task's state (0x612..0x615)
    OID oid = -1;
    int iSm = 0;
    int idState = -1;
    int fInstant = 0;  // +0x10 (only when the change is not instant)
    int iwrec = -1;
};

struct PROGREF  // an option of kind 22 at a field: a progress path (savedata.h RefGetProgressKey)
{
    std::vector<int16_t> akey;
    bool F() const { return !akey.empty(); }
};

struct TASK : LOLIST
{
    int nState = -1;        // +0x54
    float tState = 0.0f;    // +0x58
    int idDialog5C = -1;    // +0x5C (option 1265)
    int f60 = 0;            // +0x60 (1266)
    int cPlayed = 0;        // +0x64 (1267; the progress +0xC8)
    int cDone = 0;          // +0x68 (the progress +0xD0)
    int f6c = 0, f70 = 0, f74 = 0;  // +0x6C / +0x70 / +0x74 (1271..1273)
    float dtLimit = 0.0f;   // +0x78 (1285)
    JOB* pjob = nullptr;    // +0x7C (1264)
    LO* ploShown = nullptr; // +0x80 (1279): present while the task is available
    std::vector<TASK*> aptaskPrereq;  // +0x98 (the loaded oids an90)
    std::vector<TASK*> aptaskNext;    // +0xA4 (an9C)
    std::vector<TASKOBJE> aobje;      // +0xA8 / +0xAC
    std::vector<TASKSME> asme;        // +0xB0 / +0xB4
    int idChk = -1;         // +0xB8 (1274: the checkpoint id it starts at)
    int iwrec = -1;         // +0xBC (1275: its world)
    OID oidPo = -1;         // +0xC0 (1276): the player character the task starts with (progress "oidPoCurrent")
    LO* ploC4 = nullptr;    // +0xC4 (1278)
    PROGREF prC8, prD0, prD8, prE0, prE8;  // the progress: played, done, closed?, state, restarted
    int nPending = -1;      // +0xF0
    int nMode = 0;          // +0xF4 (1282; the change's mode, saved and restored around calls: CSave)
    LO* ploContext = nullptr;  // +0xF8 (1283)
    int cVisit = 0, nDepth = 0, nJobDepth = 0;  // +0xFC / +0x100 / +0x104 (0x164100)
    bool fInActive = false;  // in SW +0x3B00 (the available tasks)
};

struct JOB : LO
{
    int f50 = 0;            // +0x50 (LO::fMission)
    std::vector<int> anEnd1;  // +0x58 (1236): events that close the job (0x162DD0)
    std::vector<int> anEnd2;  // +0x60 (1237): events that end it (0x162E38)
    TASK* ptaskFirst = nullptr;  // +0x64 (1243)
    TASK* ptaskLast = nullptr;   // +0x68 (1244)
    int n6c = -1, n70 = -1;      // +0x6C / +0x70 (1245 / 1246)
    int f74 = 0, f78 = 0;        // +0x74 / +0x78 (1241 / 1242)
    int cPlayed = 0;             // +0x7C (1249; the progress +0xD0)
    std::vector<TASKOBJE> aobje; // +0x8C / +0x90
    std::vector<TASKSME> asme;   // +0x94 / +0x98
    glm::vec2 vec9C{480.0f, 720.0f};  // +0x9C (1257)
    float gA4 = 0.0f;            // +0xA4 (the progress +0xE0)
    int nAC = -1, nB0 = -1, nB4 = -1;  // +0xAC.. (1258: 0x162660)
    LO* ploB8 = nullptr;         // +0xB8 (0x405 search of +0xAC)
    LO* ploBC = nullptr;         // +0xBC (0x405 search of 0x379)
    TASK* ptaskNext = nullptr;   // +0xC0 (0x163170)
    int nSeq = 0;                // +0xC4 (0x163170: the closing sequence)
    float tSeq = 0.0f;           // +0xC8
    LO* ploCC = nullptr;         // +0xCC (1259)
    PROGREF prD0, prD8, prE0, prE8;
    int nMode = 0;               // +0xF0 (1253)
    LO* ploContext = nullptr;    // +0xF4 (1254)
};

// The world's part (orig SW +0x3B00..)
struct TASKSW
{
    JOB* pjobCur = nullptr;      // +0x3B0C
    TASK* ptaskCur = nullptr;    // +0x3B10
    TASK* ptaskChk = nullptr;    // +0x3B14 (the task whose checkpoint the job restarts at)
    JOB* pjobEnded = nullptr;    // +0x3B18
    std::vector<TASK*> aptaskActive;  // +0x3B00 (state 1)
    int grfReq = 0;              // +0x3BF4: the load's request flags (8: a queued task start, then 4)
    OID oidPointNext = -1;       // +0x3C38: the request's point, changed by the queued task (its checkpoint)
};

// The task manager (orig 0x2DEB30: kept across world loads)
struct TASKMGR
{
    float tIdleEvent = 0.0f;     // +0x00 (the clock's +0x3C at the last broadcast 0xE)
    int n8 = 0;
    int nItemC = -1;             // +0x0C (the item last acquired: 0x1DAF38)
    OID oidJob = -1, oidTask = -1, oidQueued = -1;  // +0x10 / +0x14 / +0x18
    bool fBusy = false;          // 0x2DEB28
};
extern TASKMGR g_taskmgr;

// orig 0x161478's end: the current job's time (+0xA4) while the pad was used in the last 10 s (JOY +0x1020)
void UpdateJobTimer(float dt);
void InitTasks(SW* psw);                     // a world's (from StartSw, before the slot 15 starts)
void StartJob(SW* psw, JOB* pjob);           // orig 0x00162780 (vt slot 15)
void RestartJobAtCheckpoint(SW* psw, JOB* pjob);  // orig 0x00162DF8
void StartTask(SW* psw, TASK* ptask);        // orig 0x001638F8 (vt slot 15)
void BroadcastTaskEvent(SW* psw, int n, LO* plo);  // orig 0x001619A0
bool FRequestTaskStateOid(int oid, int n);  // (debug: SLY2N_TASKREQ)
bool FDebugCompleteJob();  // (debug key F9: the current job completed through the task manager; false: no job)
float GSuck();  // orig 0x00154C90: the dynamic difficulty (0 .. 1), task.cpp
void ApplyCheckpointTasks(SW* psw, LO* pchk, const std::vector<std::pair<OID, int>>& aent);  // orig 0x00161F48
int NTaskStateOid(OID oid, bool* pfDone);  // a task's state +0x54 (-2: none) and done (0x164030)
void RequestTaskStartByOid(SW* psw, const uint8_t* pchk, OID oidTask);  // orig 0x001651D8
int NTasksPercentDone(SW* psw);              // orig 0x00161660
void EndWorldTasks(SW* psw);
void EndJob(SW* psw, JOB* pjob);             // orig 0x00162E38                 // orig 0x0020E068's part: the broadcast 1, an ended job restarted

// (QoL Replay Jobs, replay.h) a job as a loaded world file names it, read without starting the world: its oid, its
// name / description strings (+0x6C / +0x70, the job help's 0x51C / 0x51D), its first / last tasks with their
// progress paths ("state", the tasks' option 1298), the first task's world and the job's tasks, in the job help's
// order (0x1DCED8: the task graph's job depth, then the oid).
struct JOBINFO
{
    OID oidJob = -1;
    int idName = -1, idDesc = -1;
    std::string strName, strDesc;
    OID oidTaskFirst = -1, oidTaskLast = -1;
    OID oidTaskNext = -1;           // the last task's next task of another job (the story's next job, chained)
    int iwrecFirst = -1;            // the first task's world (+0xBC, or its filtered-out copy's first world)
    std::vector<int16_t> akeyFirst, akeyLast;   // the progress paths of the first / last task's state
    std::vector<OID> aoidTask;
    // (the replay's fresh start: the job's and its tasks' played / done counts (0) and closed flags (false), as a
    // first play has them; the tasks' intro scripts read them, e.g. Choo-Choo's train intro 2954 is skipped on a
    // task already played)
    std::vector<PROGREF> aprCount, aprFlag;
};
std::vector<JOBINFO> AjobinfoOfLoadedWorld(SW* psw);
