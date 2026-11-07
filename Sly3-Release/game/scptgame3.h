#pragma once

// Sly 3's scripts and tasks in the game: the SCPT executor (scptrun3.h) and the task system
// (task3.h) on the fork's world, and the hooks the other subsystems fill.
//
// The world start (update.cpp StartSw) calls StartupScripts3 before the objects' starts and RestoreTasks3 after them
// (orig3: StartSw 0x25B860's RestoreTask 0x188CB0 per task, vt slot 15); each frame the running scripts update (the
// SCPT's vt slot 16 0x225130, before the objects' updates) and the camera keys of command 0x45 drive the world camera
// (after the camera's pass).
//
// What the scripts ask of the other subsystems goes through SCPTHOOKS3 (g_scpthooks3): a null hook keeps the interim
// behaviour written here (noted at each), so the prologue's scripts run before the screen / player sides exist.
// Owners: the screen code the title, the screen objects and the binocucom's panel; the player code the actors (script animations,
// moves, the binocucom's camera); the world flow code transitions (the scripts' 0x67 / 0x68).

#include <cstdint>
#include <vector>

struct SW;
struct LO;
struct SCPT3;
struct BINOCOPEN3;

struct SCPTHOOKS3
{
    // ---- the title (the screen code; orig3 0x221AC8 ShowTitle / 0x21DD30 the title still up). Interim: shown until START.
    void (*pfnShowTitle)() = nullptr;
    bool (*pfnFTitleShowing)() = nullptr;

    // ---- the binocucom (the screen code: the panel; the player code: the actor and its camera). Interim: opens in 40 frames, a
    // speaker change takes 60, closes in 20 (the reference's numbers), nothing drawn.
    // orig3: 0x1BDA88 OpenBinoc (actor +0xF60), 0x1BDB50 CloseBinoc, actor +0xF80 the state (1 open and idle, -1
    // closed, 0 moving), 0x1BE370 the speaker; the binocucom camera policy actor +0x1008 (0x16ADC0 / 0x16AE00 focus,
    // 0x16AE40 targets, 0x16AE88 zoom, 0x16AE90 snap; done tests 0x16AEF8 / 0x16AFB8 / 0x16B078)
    void (*pfnOpenBinoc)(LO* ploActor, const BINOCOPEN3& b, LO* ploSpeaker, LO* plo60, LO* plo64, LO* plo68) = nullptr;
    void (*pfnCloseBinoc)(LO* ploActor) = nullptr;
    int (*pfnBinocState)(LO* ploActor) = nullptr;
    void (*pfnSetBinocSpeaker)(LO* ploActor, LO* ploSpeaker) = nullptr;
    void (*pfnCmFocus)(LO* ploActor, int kind, LO* plo) = nullptr;
    void (*pfnCmTargets)(LO* ploActor, const float pos[3], const float dir[3], float g) = nullptr;
    void (*pfnCmZoom)(LO* ploActor, float g) = nullptr;
    void (*pfnCmSnap)(LO* ploActor) = nullptr;
    bool (*pfnFCmDone)(LO* ploActor, int id) = nullptr;

    // ---- the screen objects (the screen code; 0x1A / 0x1B: vt slot +0x1A8 show (n: a part, -1 its show part) / +0x1B0 hide,
    // FScreenObjDone 0x260738). Interim: nothing shown, done at once.
    void (*pfnShowScreenObj)(LO* plo, int n) = nullptr;
    void (*pfnHideScreenObj)(LO* plo) = nullptr;
    bool (*pfnFScreenObjDone)(LO* plo) = nullptr;
    // the hint (0x60 / 0x61: 0x21E870 / 0x21E930) and the prompt buttons (0x4E: *(0x4BC228) = mask & 0xF0)
    void (*pfnShowHint)(int id, float g) = nullptr;
    void (*pfnClearHint)(int id) = nullptr;
    void (*pfnSetButtonPrompt)(int grf) = nullptr;

    // ---- the actors (the player code). Interim: none play / move (their waits end at once).
    // PlayActorScriptAnim 0x156578(actor, anim oid, grf, n) / EndActorScriptAnim 0x156690 / FActorInScriptAnim 0x1566D0
    void (*pfnPlayActorScriptAnim)(LO* ploActor, int oidAnim, int grf, int n) = nullptr;
    void (*pfnEndActorScriptAnim)(LO* ploActor) = nullptr;
    bool (*pfnFActorInScriptAnim)(LO* ploActor) = nullptr;
    // 0x22's placement (the object's world rows mat and position pos) and the actor's reset after it (0x1B7268)
    void (*pfnPlaceLo)(LO* plo, const float mat[9], const float pos[3]) = nullptr;
    void (*pfnResetActorAfterPlace)(LO* ploActor) = nullptr;
    // the player's control lock (0x22B238: 0x173448 take / 0x173498 release)
    void (*pfnLockPlayerControl)(bool fLock) = nullptr;
    // 0x36 (move to an object / a point): 0x1C0538; FActorMovingTo; ActorMoveState (actor +0x33C, -1 done)
    void (*pfnActorMoveTo)(LO* ploActor, LO* plo, const float pos[3], int b4, int b8) = nullptr;
    int (*pfnActorMoveState)(LO* ploActor) = nullptr;
    // 0x77 FaceObject 0x24E920
    void (*pfnFaceObject)(LO* ploActor, LO* plo) = nullptr;

    // ---- the AI (the AI code). Interim: nothing runs, the waits end at once.
    // (0x62 TellGuardCommands: the AI code's copied bodies, scptgame3.cpp)
    // 0x33: the actor's (cid 0x33) path-step job 0xF3 (JOBPATHSTEP) on the path pcrv (cid 0x09) with the command's
    // bytes b1 / b2 and float g3 (job +0x34 / +0x5C / +0x60); 0x34: the step-to job 0xF4 toward pos (the target's aim)
    // with g1 / b2 / g3 (job +0x34 / +0x38 / +0x3C). Both after ClearJobs on the actor's job stack (+0x29C); the
    // script animation (if any) is already started. The wait: pfnFActorJobDone(actor, 0xF3 / 0xF4) (no job left, or
    // its FDone 0x1577C0 / 0x157950); returning true, the hook clears the actor's jobs (the original's ClearJobs).
    void (*pfnActorPathStep)(LO* ploActor, LO* pcrv, int b1, int b2, float g3) = nullptr;
    void (*pfnActorStepTo)(LO* ploActor, const float pos[3], float g1, int b2, float g3) = nullptr;
    bool (*pfnFActorJobDone)(LO* ploActor, int kind) = nullptr;
    // 0x63: 0x1985A8(guard, plo) (plo may be null)
    void (*pfnGuardCommand63)(LO* ploGuard, LO* plo) = nullptr;

    // ---- the world (the world flow code; 0x67 BeginActorTransition, 0x68 TriggerExit 0x27FCC8)
    void (*pfnTriggerExit)(LO* ploExit) = nullptr;
};
extern constinit SCPTHOOKS3 g_scpthooks3;

// The world start: the executor's records of the world's SCPT objects (class 0x87), the task records (task3.h).
void StartupScripts3(SW* psw);
// orig3: StartSw's RestoreTask 0x188CB0 per task (after the objects' slot-15 starts)
void RestoreTasks3AtStart(SW* psw);
// The scripts' records of the world left (a new world)
void ResetScripts3();
// True when the world's tasks / scripts are Sly 3's (StartupScripts3 ran): the copied Sly 2 task code stands aside
bool FScripts3Active();

// The record of a SCPT object (null: not one)
SCPT3* PscptOfLo3(LO* plo);
void StartScptOfLo3(LO* pscpt, LO* ploStarter);  // orig3: 0x225EE8
// The title (interim, or the screen code's): showing, and START closing it
bool FTitleShowing3();
// The script camera (0x45) drives the world camera this frame
bool FScriptCameraActive3();

// The world's dialog table (orig3 SW +0x4FB0, 16-byte entries: the world option 508's method 0x25ED20 fills entry i
// with two id lists and a shuffle bag over their count (0x2BFA48: each index once per round, a new round not starting
// with the last one). Read by the guards' spotted lines (0x19AF98: entry 5, the first list or the second by the
// player (+0x2F0)): IdWorldDialog3(i, fSecond) gives the next id (-1: none); the AI code's.
void SetWorldDialogTable3(int i, const std::vector<int>& aid0, const std::vector<int>& aid1);
int IdWorldDialog3(int i, bool fSecond);

// (debug / harness) a task state request as the game makes it (RequestTaskState 0x189300 through the queue 0x189240):
// SLY3_TASKREQ="FRAME:OID=STATE;..." does this at those frames
void RequestTaskState3(int oidTask, int n);
// (debug) the task keys at GAME 0x36C700 +0x1498 / +0x149C / +0x14A0: the current job / active task / current task
void TaskKeys3Of(int akey[3]);
