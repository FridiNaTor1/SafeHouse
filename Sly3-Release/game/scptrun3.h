#pragma once

// Sly 3's script executor (orig3 module 0x225100..0x22C400): a script (SCPT, cid 0x87)
// holds its command tree (scpt3.h, read by 0x225220); StartScpt 0x225EE8 makes a runner (PrunNew 0x22BF40) on the
// root command and runs it (RunRun 0x2265E0); the script's update 0x225130 runs every runner each frame and stops the
// script (0x226050) once none is left. A runner walks the tree with a stack of open commands (the executor 0x226D00):
// it executes a command, and while the command carries the wait flag (header bit 8) and its done test (0x229438) says
// no, it stays there; the flow commands (spawn 0x00, while 0x04, if 0x06 with its cases 0x07 / 0x08 / else 0x09)
// open their children; the waits (0x01.. the shared handler 0x2293DC) only record their start time.
//
// The game effects go through SCPTHOST3 (the object finder, the clock, the dialog, the binocucom, the camera, the
// tasks, ...): the game's systems implement it (the player / binocucom / camera side, the dialog side); the defaults
// log the call. `--scpt-run W OID` runs a script with a logging host that simulates time (scptrun3.cpp).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "scpt3.h"

struct RUN3;
struct SCPT3;

// The arguments of OpenBinoc (0x46, reader 0x225994: a 0x70-byte block at the command's +8).
struct BINOCOPEN3
{
    int32_t oidActor = -1;    // +0x00 the actor who looks (Sly, cid 0x33-derived)
    int32_t oidSpeaker = -1;  // +0x04 who is on the screen (Bentley, cid 0x32-derived)
    float posEye[3]{};        // +0x10
    float posLook[3]{};       // +0x20
    float g30 = 0;            // +0x30
    int32_t oid60 = -1;       // +0x60 (PloFind)
    int32_t oid64 = -1;       // +0x64 (PloFind)
    int32_t oid68 = -1;       // +0x68 (PloFindSwNearest kind 0x405)
};

// The arguments of a dialog line (0x52, reader 0x225ABC: {+0x00 S8 fQuiet, +0x04 the sound source's oid, +0x08 / +0x0C
// the falloff start / end (S32, converted to floats), +0x10 the volume, +0x14 / +0x18 the count, +0x1C the ids}).
struct DIALOGSTART3
{
    std::vector<int32_t> aid;  // the line ids (the first one plays: PsndStartDialog(g_dialog, aid[0], ...))
    int fQuiet = 0;
    int32_t oidSource = -1;
    float sFalloffStart = 0, sFalloffEnd = 0, gVolume = 1;
    void* ploSpeaker = nullptr;  // the run's target object when the command has an oid (+4 != -1), else null
};

// What the executor asks of the game. Objects are opaque (the game's LO*); oid -1 means none.
struct SCPTHOST3
{
    virtual ~SCPTHOST3() = default;

    // the clock (0x226458: the script's real-time flag 0x200 chooses g_clock +0x44 over +0x0 (0x36BB98))
    virtual float T(bool fRealTime) { (void)fRealTime; return 0.0f; }
    virtual float GRandInRange(float gMin, float gMax) { return 0.5f * (gMin + gMax); }  // 0x11E940

    // objects: PloFindSwNearest(sw, kind, oid, ploContext) 0x17E1B8 (PloFind 0x226770 = kind 4 from the run's context)
    virtual void* PloFind(int kind, int oid, void* ploContext) { (void)kind; (void)oid; (void)ploContext; return nullptr; }
    // FIsLoDerivedFrom(plo, cid) 0x13B500 (the handlers' class checks: 0x31 job, 0x32 / 0x33 / 0x34 actors, 0x87 SCPT)
    virtual bool FDerivedFrom(void* plo, int cid) { (void)plo; (void)cid; return plo != nullptr; }
    // another script object (0x0F / 0x10 alarms, 0x11 stop, 0x17 start)
    virtual SCPT3* PscptFromLo(void* plo) { (void)plo; return nullptr; }

    // the condition kinds 0x760..0x762 (0x226950: an object's +0x54 == 1 (0x760, a task's state?), +0xFC == 0 (0x761),
    // +0xFC == 1 (0x762)); the kinds outside 0x75E..0x767 ask the script's event handler (vt slot 3, message 4)
    virtual bool FObjectCondition(int kind, void* plo) { (void)kind; (void)plo; return false; }
    virtual bool FScriptCondition(SCPT3* ps, int kind, int param) { (void)ps; (void)kind; (void)param; return false; }
    // 0x15: the script's event handler (vt slot 3) with message 4 and the command's argument (a splice callback)
    virtual void ScriptEvent(SCPT3* ps, int n) { (void)ps; (void)n; }
    // the script started (vt slot 3 message 5) / stopped (message 6)
    virtual void ScriptStarted(SCPT3* ps) { (void)ps; }
    virtual void ScriptStopped(SCPT3* ps) { (void)ps; }

    // dialog (the dialog system g_dialog 0x474FF0): PsndStartDialog 0x2485D8 (+ SetSource / SetFalloff / SetVolume),
    // FDialogPlaying 0x248858 (id -1: any), StopDialog 0x248740, ResetDialogSubtitle 0x27ED18 (on 0x4BC220)
    virtual bool StartDialog(const DIALOGSTART3& d, void* ploSource) { (void)d; (void)ploSource; return false; }
    virtual bool FDialogPlaying(int id) { (void)id; return false; }
    virtual void StopDialog() {}
    virtual void ResetDialogSubtitle() {}

    // the binocucom of an actor (actor +0xF60; its state +0xF80: 1 open and idle, -1 closed; its camera policy +0x1008)
    virtual void OpenBinoc(void* ploActor, const BINOCOPEN3& b, void* ploSpeaker, void* plo60, void* plo64, void* plo68)
    { (void)ploActor; (void)b; (void)ploSpeaker; (void)plo60; (void)plo64; (void)plo68; }
    virtual void CloseBinoc(void* ploActor) { (void)ploActor; }               // 0x1BDB50
    virtual int BinocState(void* ploActor) { (void)ploActor; return -1; }     // actor +0xF80
    virtual bool FBinocCanOpen(void* ploActor) { (void)ploActor; return true; }  // actor +0x130 != -1
    virtual void SetBinocSpeaker(void* ploActor, void* ploSpeaker) { (void)ploActor; (void)ploSpeaker; }  // 0x1BE370 (0x4D)
    // the binocucom camera (actor +0x1008, a CPFREE-like policy): 0x48 / 0x49 focus 0x16ADC0 / 0x16AE00, 0x4A targets
    // 0x16AE40, 0x4B zoom 0x16AE88, 0x4C snap 0x16AE90; done tests 0x48 0x16AEF8, 0x49 0x16AFB8, 0x4B 0x16B078
    virtual void CmFocus(void* ploActor, int kind, void* plo) { (void)ploActor; (void)kind; (void)plo; }
    virtual void CmTargets(void* ploActor, const float pos[3], const float dir[3], float g) { (void)ploActor; (void)pos; (void)dir; (void)g; }
    virtual void CmZoom(void* ploActor, float g) { (void)ploActor; (void)g; }
    virtual void CmSnap(void* ploActor) { (void)ploActor; }
    virtual bool FCmDone(void* ploActor, int id) { (void)ploActor; (void)id; return true; }
    virtual void CutCm() {}  // 0x23
    // the run's camera keys (0x45): the move from a key toward the next ones (StartRunCamera 0x22BB78, the next keys
    // found by 0x22A500); EndRunCamera 0x22BEE8 (a key with the flags 0x60)
    virtual void StartRunCamera(RUN3& run, const SCPTCMD3& key, const SCPTCMD3* pkeyNext, const SCPTCMD3* pkeyNext2)
    { (void)run; (void)key; (void)pkeyNext; (void)pkeyNext2; }
    virtual void EndRunCamera(RUN3& run) { (void)run; }

    // tasks (0x1E: ApplyCheckpointTasks(0x36C700, 1, {oid, state}, ctx) 0x185908)
    virtual void ApplyCheckpointTasks(int oidTask, int state, void* ploContext) { (void)oidTask; (void)state; (void)ploContext; }
    // the title (0x50: ShowTitle; done when 0x21DD30(0x46FC30) says it is gone)
    virtual void ShowTitle() {}
    virtual bool FTitleShowing() { return false; }
    // music (0x59 PsndPushMusic, 0x5A PopMusic)
    virtual void PushMusic(int id) { (void)id; }
    virtual void PopMusic(int id) { (void)id; }

    // ---- the actor commands (the Sly 3 script port; each names the game function the handler calls, the actor fields it reads)
    // The world transform of an object (its vt slot at +0x120/+0x124: rows mat[0..2], translation +0x30).
    virtual bool GetLoXf(void* plo, float mat[9], float pos[3]) { (void)plo; for (int i = 0; i < 9; ++i) mat[i] = i % 4 == 0; pos[0] = pos[1] = pos[2] = 0; return plo != nullptr; }
    // an actor's aim offset (+0x260, added for cid 0x32 actors) and heading (+0x1BC)
    virtual void ActorAimOffset(void* ploActor, float d[3]) { (void)ploActor; d[0] = d[1] = d[2] = 0; }
    virtual float ActorHeading(void* ploActor) { (void)ploActor; return 0; }
    // NearestCrvAll 0x22EA88 on a curve object (cid 0x09) or an actor's path: the nearest point / tangent / parameter
    virtual void NearestOnCurve(void* pcrv, const float posFrom[3], float pos[3], float dir[3], float* pu)
    { (void)pcrv; for (int i = 0; i < 3; ++i) { pos[i] = posFrom[i]; dir[i] = i == 0; } if (pu) *pu = 0; }
    // FDropOnGround 0x198D78(pos, 0, 0, 100, -350): the point dropped onto the ground below it
    virtual void DropOnGround(float pos[3]) { (void)pos; }
    // SetRunPending 0x22B028's actor side: 0x156468(actor, run) (null: released)
    virtual void SetActorRun(void* ploActor, RUN3* prun) { (void)ploActor; (void)prun; }
    // the player's control lock (0x22B238: *(0x370AC0) +0x1108, a counter at +0x64: 0x173448 take / 0x173498 release)
    virtual void LockPlayerControl(bool fLock) { (void)fLock; }
    // 0x172D10(actor +0x1108, -1): an actor's script-control reset (PrepareCmd 0x226BE0, scripts with SCPT +0xA8 0)
    virtual void ResetActorControl(void* ploActor) { (void)ploActor; }
    // the script animations: PlayActorScriptAnim 0x156578(actor, anim oid, grf, n), EndActorScriptAnim 0x156690,
    // FActorInScriptAnim 0x1566D0
    virtual void PlayActorScriptAnim(void* ploActor, int oidAnim, int grf, int n) { (void)ploActor; (void)oidAnim; (void)grf; (void)n; }
    virtual void EndActorScriptAnim(void* ploActor) { (void)ploActor; }
    virtual bool FActorInScriptAnim(void* ploActor) { (void)ploActor; return false; }
    // an ASEG (cid 0x82) applied: PasegaApplyAseg 0x137428(aseg, plo, 0.0, 1.0, grf) (the handle is retained by the run
    // while it waits); FAsegaRetracted 0x138AC8; RetractAsega 0x137B00; the handle's release (vt slot 2)
    virtual void* ApplyAseg(void* ploAseg, void* plo, int grf) { (void)ploAseg; (void)plo; (void)grf; return nullptr; }
    virtual bool FAsegaRetracted(void* pasega) { (void)pasega; return true; }
    virtual void RetractAsega(void* pasega) { (void)pasega; }
    virtual void ReleaseAsega(void* pasega) { (void)pasega; }
    // 0x22: the binocucom shut at once (0x1BDBB0(actor +0xF60, -1), when StoredFlagIsClear 0x1569A8(actor)); an object
    // of cid 0x27 activated (0x27FA30); an interact start (cid 0x19: StartInteract 0x1B40F0(actor +0x500, 8, plo, plo
    // +0x120), SetInteractState 0x1B4430(9)); an object placed (its vt slot +0x158: the matrix); the actor's state
    // reset after a placement (0x1B7268(actor +0x320, 0xB, 0, 0, 1.0) unless actor +0x13EC, DetachAloContacts 0x23DF78)
    virtual bool FStoredFlagIsClear(void* ploActor) { (void)ploActor; return true; }
    virtual void CloseBinocNow(void* ploActor) { (void)ploActor; }
    virtual void Activate27(void* plo) { (void)plo; }
    virtual void StartInteract(void* ploActor, void* plo) { (void)ploActor; (void)plo; }
    virtual void PlaceLo(void* plo, const float mat[9], const float pos[3]) { (void)plo; (void)mat; (void)pos; }
    virtual void ResetActorAfterPlace(void* ploActor) { (void)ploActor; }
    // 0x30 (walk toward an object, playing an animation): 0x1B5170(actor +0x500) for targets of cids 0x0A / 0x0B / 0x20
    // / 0x24 / 0x26 / 0x64; the actor's path interact (+0x50C != -1, +0x508 == 3: its curve +0x514); the done test's
    // SetActorWatchTarget 0x1572A0(actor +0x10A0, pos)
    virtual void ResetActorInteract(void* ploActor) { (void)ploActor; }
    virtual void* PcrvActorInteract(void* ploActor) { (void)ploActor; return nullptr; }
    virtual void SetActorWatchTarget(void* ploActor, const float pos[3]) { (void)ploActor; (void)pos; }
    // 0x36 (the actor moves to an object or a point): the object's nav helper 0x1A2A40(plo) and its goal (helper vt
    // slot +0x10/+0x14 (helper, actor, pos)); FActorMovingTo (actor +0x1014 != -1 and +0x101C == plo); the move
    // 0x1C0538(actor, goal {+0 plo, +4 helper, +0x10 pos}); then b4 == 1: actor +0x348 = 0; cid 0x34: +0x19F0 = b8.
    // Done: ActorMoveState (actor +0x33C) == -1.
    virtual void* PnavOf(void* plo) { (void)plo; return nullptr; }
    virtual void NavGoal(void* pnav, void* ploActor, float pos[3]) { (void)pnav; (void)ploActor; (void)pos; }
    virtual bool FActorMovingTo(void* ploActor, void* plo) { (void)ploActor; (void)plo; return false; }
    virtual void ActorMoveTo(void* ploActor, void* plo, void* pnav, const float pos[3], int b4, int b8)
    { (void)ploActor; (void)plo; (void)pnav; (void)pos; (void)b4; (void)b8; }
    virtual int ActorMoveState(void* ploActor) { (void)ploActor; return -1; }
    // 0x26 (along the actor's path to the point nearest an object): FActorOnPath (actor +0x1014 != -1), its path +0x101C,
    // the climb limits LmClimb 0x1BF128(actor +0x1010) / GLimitLm 0x11EE68, the actor's position on it +0x1020
    virtual bool FActorOnPath(void* ploActor) { (void)ploActor; return false; }
    virtual void* PcrvActorPath(void* ploActor) { (void)ploActor; return nullptr; }
    virtual float GLimitActorPath(void* ploActor, float u) { (void)ploActor; return u; }
    virtual float UActorPath(void* ploActor) { (void)ploActor; return 0; }
    // 0x28: 0x13A678(actor +0x10D0, the kind-1 object, plo); done when actor +0x10D4 == -1
    virtual void Start28(void* ploActor, void* ploKind1, void* plo) { (void)ploActor; (void)ploKind1; (void)plo; }
    virtual bool FDone28(void* ploActor) { (void)ploActor; return true; }
    // 0x77: FaceObject 0x24E920(actor, plo)
    virtual void FaceObject(void* ploActor, void* plo) { (void)ploActor; (void)plo; }
    // 0x12: an object activated by its class (cid: the first of 0x5D (+0x21C's vt), 0x15 (+0x20C's vt), 0x27 / 0x28
    // (ApplyCheckpoint 0x280010), 0x18, 0x8A (StartFadeLo 0x17D7D0), 0x4D (0x19E1F8), 0x71 (0x2775B8), 0x21 (vt slot
    // +0x1B8, with the actor), 0x47 (0x1E8E40), 0x50 (0x1716F8); 0 = none: 0x13BC80(plo))
    virtual void ActivateLo(int cid, void* plo, void* ploActor) { (void)cid; (void)plo; (void)ploActor; }
    // 0x13: 0x1F4B00(GAME, n, job) on a job (cid 0x31)
    virtual void JobCommand(void* ploJob, int n) { (void)ploJob; (void)n; }
    // 0x1A / 0x1B: a screen object (cid 0x9E) shown (vt slot +0x1A8, n) / hidden (+0x1B0); FScreenObjDone 0x260738
    virtual void ShowScreenObj(void* plo, int n) { (void)plo; (void)n; }
    virtual void HideScreenObj(void* plo) { (void)plo; }
    virtual bool FScreenObjDone(void* plo) { (void)plo; return true; }
    // 0x1D / StopScpt: 0x2263B0: SetClockFactor 0x165548(g_clock, 6, g) (a script turns real time while g == 0)
    virtual void SetClockFactor(float g) { (void)g; }
    // 0x1F (0x22B290): PauseGame 0x1A93C8 + 0x1A94A8 (GAME, player, kind) / ResumeGame 0x1A9420 + 0x1A94F0; the player
    // index: a job's +0x130 (-1: all)
    virtual void PauseGame(int iplayer, int kind) { (void)iplayer; (void)kind; }
    virtual void ResumeGame(int iplayer, int kind) { (void)iplayer; (void)kind; }
    virtual int IPlayerOfJob(void* ploJob) { (void)ploJob; return -1; }
    // 0x38 / 0x39: AddRemoveByOid 0x25D740(sw, fAdd, oid, g, ctx, scpt) (returns the object); its done test: added
    // (+0x2F bit 0 and +0x104 == 1.0) / removed (+0x2F bit 0 clear)
    virtual void* AddRemoveByOid(bool fAdd, int oid, float g, void* ploContext, SCPT3* ps) { (void)fAdd; (void)oid; (void)g; (void)ploContext; (void)ps; return nullptr; }
    virtual bool FAddRemoveDone(void* plo, bool fAdd) { (void)plo; (void)fAdd; return true; }
    // 0x3F: JoltCm 0x167D38(cm, n) on each camera of SW +0x3EBC (done 0.5 s later, the constant 0x344838)
    virtual void JoltCm(int n) { (void)n; }
    // 0x68: TriggerExit 0x27FCC8 on the exit, its point (+0x178) set first unless -1 (the world flow code)
    virtual void TriggerExit(void* ploExit, int oidPoint) { (void)ploExit; (void)oidPoint; }
    // 0x41: 0x25D940(sw, oid0, oid1, g, ctx, scpt)
    virtual void Cmd41(int oid0, int oid1, float g, void* ploContext, SCPT3* ps) { (void)oid0; (void)oid1; (void)g; (void)ploContext; (void)ps; }
    // 0x43: PlayAsegaSound 0x25DB38(sw, h, w, w, w, g, g, g, 0, ctx, scpt, &run +0x114): the sound (stopped by
    // RetireRun: CSnd::Stop 0x244CA8(snd, 0))
    virtual void* PlayAsegaSound(int h, int n1, int n2, int n3, float g0, float g1, float g2, void* ploContext, SCPT3* ps)
    { (void)h; (void)n1; (void)n2; (void)n3; (void)g0; (void)g1; (void)g2; (void)ploContext; (void)ps; return nullptr; }
    virtual void StopSound(void* psnd) { (void)psnd; }
    // 0x4E: the prompt buttons (*(0x4BC228) = mask & 0xF0), FButtonPressed 0x1A7A80(pad, mask, 0) on the job's pad (cid
    // 0x31 +0x134) or the default pad 0x36E740 (null)
    virtual void SetButtonPrompt(int grf) { (void)grf; }
    virtual void* PpadOfJob(void* ploJob) { (void)ploJob; return nullptr; }
    virtual bool FButtonPressed(void* ppad, int grfMask) { (void)ppad; (void)grfMask; return false; }
    // 0x60 / 0x61: the hint (0x21E870 / 0x21E930 on the global 0x46F770)
    virtual void ShowHint(int id, float g) { (void)id; (void)g; }
    virtual void ClearHint(int id) { (void)id; }
    // 0x62: TellGuardCommands 0x198540(guard, commands); done when FGuardRunsCommands 0x1985F0 says no more
    virtual void TellGuardCommands(void* ploGuard, void* ploCmds) { (void)ploGuard; (void)ploCmds; }
    // 0x33 / 0x34: an actor's (cid 0x33) AI job pushed on its job stack (actor +0x29C, after ClearJobs 0x238568 and its
    // vt slot 0x318/8 (0)): 0x33 the path-step job 0xF3 (JOBPATHSTEP: +0x30 the path (cid 0x09), +0x34 / +0x5C the
    // two bytes, +0x60 the float, +0x58 the run's +0x104), 0x34 the step-to job 0xF4 (+0x30 the run's +0x104, +0x34 the
    // float, +0x38 the byte, +0x3C the float, 0x157970 the aim point). Done when the job is gone or its FDone
    // (0x1577C0 / 0x157950) says so: then ClearJobs.
    virtual void ActorPathStep(void* ploActor, void* pcrv, int b1, int b2, float g3) { (void)ploActor; (void)pcrv; (void)b1; (void)b2; (void)g3; }
    virtual void ActorStepTo(void* ploActor, const float pos[3], float g1, int b2, float g3) { (void)ploActor; (void)pos; (void)g1; (void)b2; (void)g3; }
    virtual bool FActorJobDone(void* ploActor, int kind) { (void)ploActor; (void)kind; return true; }
    // 0x19: RunDockRsmgs 0x25D478: the one object oid (a proxy: its instance; an ALO) and its SM (oidSm, -1 any):
    // its goal set (fSnap: snapped); returns the SMA (the run keeps it for the done test), null when none
    virtual void* RunSmGoal(int oid, int oidSm, int oidGoal, bool fSnap, void* ploContext)
    { (void)oid; (void)oidSm; (void)oidGoal; (void)fSnap; (void)ploContext; return nullptr; }
    // the done test's: FSmaRetracted 0x239CC0 / OidSmaGoal 0x239E70
    virtual bool FSmGoalReached(void* psma, int oidGoal) { (void)psma; (void)oidGoal; return true; }
    // 0x63: 0x1985A8(guard, plo) (the guard's second command kind: the AI code)
    virtual void GuardCommand63(void* ploGuard, void* plo) { (void)ploGuard; (void)plo; }
    // 0x42: SetEyesClosedOf 0x25DA70(sw, oid, n, g, ctx) (the eyes of the context's object)
    virtual void SetEyesClosed(int oid, int n, float g, void* ploContext) { (void)oid; (void)n; (void)g; (void)ploContext; }
    // 0x55 / 0x56 (HoldRunEffects 0x22B4A8 / ReleaseRunEffects 0x22B580; Sly 2 0x1E51A0): an actor's effects held
    // (its vt slot: binoc.h HoldActorEffects) while the run holds them, at most 4 actors per run
    virtual void HoldActorEffects(void* ploActor, bool fHold, bool f, int grf) { (void)ploActor; (void)fHold; (void)f; (void)grf; }
    // 0x29: SetActorInteractObject 0x154B48(actor, plo)
    virtual void SetActorInteractObject(void* ploActor, void* plo) { (void)ploActor; (void)plo; }
    virtual bool FGuardRunsCommands(void* ploGuard, void* ploCmds) { (void)ploGuard; (void)ploCmds; return false; }
    // 0x6F / 0x70: a proxy's (cid 0x08) instance: the current one 0x1FBF08 or a new one PloAcquireProxyObject 0x1FBE30
    // (proxy, 0), the row lock 0x1FC060; 0x70: its instance 0x1FBEC0, unlock 0x1FC090, release 0x1FC0E0
    virtual void* PloProxyCurrent(void* pproxy) { (void)pproxy; return nullptr; }
    virtual void* PloProxyAcquire(void* pproxy) { (void)pproxy; return nullptr; }
    virtual void LockProxyRow(void* pproxy, void* plo) { (void)pproxy; (void)plo; }
    virtual void* PloProxyInstance(void* pproxy) { (void)pproxy; return nullptr; }
    virtual void UnlockProxyRow(void* pproxy, void* plo) { (void)pproxy; (void)plo; }
    virtual void ReleaseProxyInstance(void* pproxy, void* plo) { (void)pproxy; (void)plo; }
    // the done test about to run (the test hosts use it to know which command an actor query is about)
    virtual void Testing(RUN3& run, int icmd) { (void)run; (void)icmd; }

    // a command the executor does not port yet (it finishes at once): the default logs it once per id
    virtual void Unported(RUN3& run, const SCPTCMD3& cmd);
    // a trace of every executed command (the tests)
    virtual void Executed(RUN3& run, int icmd) { (void)run; (void)icmd; }
    virtual void DoneTested(RUN3& run, int icmd, bool fDone) { (void)run; (void)icmd; (void)fDone; }
};

// A runner (orig3: 0x180 bytes, the pool 0x479D44, vtable at +0xC).
struct RUN3
{
    SCPT3* pscpt = nullptr;      // +0x14
    RUN3* prunParent = nullptr;  // +0x18 (a reference)
    void* ploContext = nullptr;  // +0x1C (the parent's, or the script itself)
    void* plo20 = nullptr;       // +0x20 the root command's object (kind 5)
    void* plo24 = nullptr;       // +0x24 set by 0x14 ("with")
    void* plo28 = nullptr;       // +0x28 the current command's target (0x226BE0)
    int icmdRoot = 0;            // +0x2C
    bool fDescend = false;       // +0x30 (the executor descends into an 0x02 when set; no setter found yet)
    int icmdCur = -1;            // +0x34 (-1: none)
    float tStart = 0;            // +0x38 the current command's start time
    int icmdDialog = -1;         // +0x70 the last dialog command
    void* ploBinoc = nullptr;    // +0x90 the actor whose binocucom this run opened (0x46; closed by 0x47)
    int fSynced = 0;             // +0x108 (0x0D)
    float tWake = 3.4028235e38f; // +0x88 (FLT_MAX: not waiting to start)
    float gWait = 0;             // +0x168 (0x2F: the drawn duration)
    std::vector<int> aicmdStack; // +0x118 depth / +0x11C the open commands (16 in the original)
    int icmdPending = -1;        // +0x48 the pending command (SetRunPending 0x22B028: an actor command's done test,
    void* ploPending = nullptr;  // +0x4C   run by FinishRunPending 0x226700 each step until it passes) and its actor
    void* pasega = nullptr;      // +0x54 the ASEG handle 0x16 waits on
    SCPT3* pscptWait = nullptr;  // +0x58 the script 0x17 started with the wait flag (stopped by RetireRun)
    void* ploAnim = nullptr;     // +0x5C the actor whose script animation the run waits on (0x16 / 0x30)
    void* ploAddRemove = nullptr;// +0x68 (0x38 / 0x39)
    void* ploScreen = nullptr;   // +0x6C the screen object 0x1A shows (hidden by RetireRun)
    float uCrv80 = 0;            // +0x80 (0x30: the actor's interact curve parameter)
    float uCrv84 = 0;            // +0x84 (0x26: the path parameter to reach)
    void* ploJobActor = nullptr; // (0x33 / 0x34: the actor whose job the done test watches)
    void* psma = nullptr;        // +0x60 (0x19: the SMA whose goal the run waits for)
    int f104 = 0;                // +0x104 (0x31 sets it: the jobs of 0x33 / 0x34 take it, job +0x58 / +0x30)
    struct HOLD { void* plo; int grf; int f; };
    std::vector<HOLD> ahold;     // (0x55 / 0x56: the held actors' effects, released by RetireRun 0x22B828)
    void* ploGuard = nullptr;    // +0xFC / +0x100 (0x62)
    void* ploGuardCmds = nullptr;
    void* psnd = nullptr;        // +0x114 (0x43)
    bool fNoControl = false;     // +0x170 (set by 0x6A: the actor's control left alone)
    bool fLockPlayer = false;    // +0x174 (0x22B238)
    bool fPaused = false;        // +0x3C / +0x40 / +0x44 (0x1F: 0x22B290)
    int kindPause = 0;
    int iplayerPause = -1;
    bool fRetired = false;
};

// A script (orig3: SCPT 0xB0 bytes; Sly 2's 0x72).
struct SCPT3
{
    int32_t oid = 0;                 // LO +0x18
    std::string name;
    std::vector<SCPTCMD3> acmd;      // +0x40 / +0x44
    uint32_t grf = 0;                // +0x3C (0x200: real time; 0x2: set while it runs)
    bool fRunning = false;           // +0x60 bit 0
    bool fF2 = false;                // +0x60 bit 1
    void* ploStarter = nullptr;      // +0x58
    float tElapsed = 0;              // +0x5C
    std::vector<std::unique_ptr<RUN3>> arun;  // +0x48 the runners (newest first, as PrependDlEntry)
    std::vector<int> aalarm;         // +0x64 count / +0x68 the alarms (0x0F sets, 0x10 clears, 0x0E waits)
    void* plo = nullptr;             // the game's object (opaque)
    int mode = 0;                    // +0xA8 (PrepareCmd 0x226BE0: 0 the actor's control reset, 2 the player locked)
};

void SetScptHost3(SCPTHOST3* phost);
SCPTHOST3& ScptHost3();

// a script's alarm set / cleared from outside (the splice callbacks a script's 0x15 registers: m_ext 4262's alarm
// 15086 is set by its world code, 0x0E waits for it); orig3 0x2261A0 / 0x2261F0
void SetScptAlarm3(SCPT3* ps, int n, bool fSet);
bool FScptAlarm3(const SCPT3* ps, int n);

void StartScpt3(SCPT3* ps, void* ploStarter);  // orig3: 0x225EE8
void StopScpt3(SCPT3* ps);                     // orig3: 0x226050
void UpdateScpt3(SCPT3* ps, float dt);         // orig3: 0x225130 (dt: the frame's time, real or game)
bool FScptRunning3(const SCPT3* ps);

