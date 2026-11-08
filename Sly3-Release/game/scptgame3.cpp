// Sly 3's scripts and tasks in the game (scptgame3.h).

#include "scptgame3.h"
#include "../frontend.h"
#include "../hudwidget.h"

#include "cmkey3.h"
#include "scptrun3.h"
#include "sly3.h"
#include "task3.h"
#include "trigger3.h"

#include "../activate.h"
#include "../animfind.h"
#include "../clock.h"
#include "../frame.h"
#include "../cm.h"
#include "../emitter.h"
#include "../smrun.h"
#include "../sm.h"
#include "../actor.h"
#include "../binoc.h"
#include "../interact.h"
#include "../shd.h"
#include "../frontend.h"
#include "../hudwidget.h"
#include "../pe.h"
#include "../joy.h"
#include "../objmethod.h"
#include "../sound.h"
#include "../sw.h"
#include "../update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

constinit SCPTHOOKS3 g_scpthooks3;

float GRandInRange(float gLow, float gHigh);  // xfmath.h (its SMP clashes with vecmat.h's)
void TellGuardCommands(LO* pguard, LO* pcmds, bool fRun);  // scptai.cpp (0x171CF8)
bool FGuardRunsCommands(LO* pguard, LO* pcmds);             // scptai.cpp (0x171DA8)
namespace
{
SW* s_psw = nullptr;
struct WDIALOG3 { std::vector<int> aid0, aid1, abag; std::size_t inext = 0; int ilast = -1; };
std::map<int, WDIALOG3> s_mpwdialog;   // the world option 508's table (cleared by ResetScripts3)
bool s_fActive = false;
int s_cframe = 0;   // the frames since the world start (the interim binocucom's timing, the debug requests)

bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_SCPTPRINT") != nullptr;
    return s_f;
}

LO* Plo(void* pv) { return static_cast<LO*>(pv); }

// ---------------------------------------------------------------- interim title (until the screen code's hooks)
// orig3: 0x221AC8 ShowTitle (+0x54 1, +0x50 0, the widget shown); 0x221C08's START: StartNewGameFromTitle 0x221EC0
// (no save chosen and the card idle: the title leaves the HUD's list). SLY3_START_AT=frame[,frame]: START then
// (harness), as a press of START on the pad does.
struct TITLE3
{
    bool fShown = false;
    std::vector<int> aframeStart;
} s_title;

// ---------------------------------------------------------------- interim binocucom (until the screen code / the player code's hooks)
// The reference's timing (the old port's test host): it opens in 40 frames, a speaker change takes 60, it closes in 20
struct BINOC3
{
    LO* ploActor = nullptr;
    LO* ploSpeaker = nullptr;
    int state = -1;         // actor +0xF80: 1 open and idle, -1 closed, 0 moving
    int stateTarget = -1;
    int cframeReady = 0;
} s_binoc;

void UpdateInterimBinoc()
{
    if (s_binoc.state == 0 && s_cframe >= s_binoc.cframeReady) s_binoc.state = s_binoc.stateTarget;
}

// ---------------------------------------------------------------- the host
struct GAMESCPTHOST3 : SCPTHOST3
{
    std::map<const LO*, SCPT3*> mpploscpt;
    float T(bool fRealTime) override { return fRealTime ? g_clock.tReal : g_clock.t; }
    float GRandInRange(float gMin, float gMax) override
    {
        return ::GRandInRange(gMin, gMax);  // xfmath.cpp (orig 0x11D448; Sly 3 0x11E940)
    }
    // PloFindSwNearest(sw, kind, oid, ploContext) 0x17E1B8: near the context object, else the world's
    void* PloFind(int, int oid, void* ploContext) override
    {
        if (oid == -1 || !s_psw) return nullptr;
        if (LO* ploCtx = Plo(ploContext))
            if (LO* p = PloFindSwNearest(ploCtx, oid)) return p;
        return PloFindSwObjectByOid(s_psw, oid);
    }
    bool FDerivedFrom(void* plo, int cid) override { return plo && FCidDerivedFrom(Plo(plo)->Cid(), cid); }
    SCPT3* PscptFromLo(void* plo) override
    {
        const auto it = mpploscpt.find(static_cast<const LO*>(plo));
        return it == mpploscpt.end() ? nullptr : it->second;
    }

    // 0x760..0x762 (0x226950): the object's +0x54 == 1 (a task: activated once), +0xFC == 0 / 1 (the task's mode)
    bool FObjectCondition(int kind, void* plo) override
    {
        const TASK3* p = plo ? PtaskFind3(Plo(plo)->oid) : nullptr;
        if (!p) return false;
        if (kind == 0x760) return p->cActivated == 1;
        return kind == 0x761 ? p->nMode == 0 : kind == 0x762 ? p->nMode == 1 : false;
    }
    // the script object's messages (vt slot 3 0x1C86B8: the splice handler bound to the message): 4 the conditions /
    // events (with the kind), 5 started, 6 stopped
    bool FScriptCondition(SCPT3* ps, int kind, int) override
    {
        CRef a, r;
        a.SetS32(kind);
        LO* plo = Plo(ps->plo);
        if (!plo || !plo->pframe || !plo->pframe->PrefFindBinding(4, 1)) return false;
        HandleLoSpliceEvent(plo, 4, false, 1, &a, &r);
        return r.m_tagk == TAGK_Bool ? r.m_bool != 0 : r.m_tagk == TAGK_S32 ? r.m_n != 0 : false;
    }
    void ScriptEvent(SCPT3* ps, int n) override
    {
        CRef a;
        a.SetS32(n);
        if (LO* plo = Plo(ps->plo)) HandleLoSpliceEvent(plo, 4, false, 1, &a);
    }
    void ScriptStarted(SCPT3* ps) override { if (LO* plo = Plo(ps->plo)) HandleLoSpliceEvent(plo, 5, false, 0, nullptr); }
    void ScriptStopped(SCPT3* ps) override { if (LO* plo = Plo(ps->plo)) HandleLoSpliceEvent(plo, 6, false, 0, nullptr); }

    // the dialog: PsndStartDialog 0x2485D8 (+ SetSource / SetFalloff / SetVolume), FDialogPlaying 0x248858,
    // StopDialog 0x248740, ResetDialogSubtitle 0x27ED18 (sound.cpp's Sly 2 dialog code plays Sly 3's lines)
    bool StartDialog(const DIALOGSTART3& d, void* ploSource) override
    {
        if (d.aid.empty()) return false;
        CSndVoice* p = PsndStartDialog(d.aid[0], d.fQuiet);
        if (FPrint()) std::printf("scpt3: frame %d dialog %d%s\n", s_cframe, d.aid[0], p ? "" : " (no line)");
        if (!p) return false;
        if (ploSource) p->SetSource(ploSource);
        if (d.sFalloffEnd > 0) p->SetFalloff(d.sFalloffStart, d.sFalloffEnd);
        p->SetVolume(d.gVolume);
        return true;
    }
    bool FDialogPlaying(int id) override { return ::FDialogPlaying(id); }
    void StopDialog() override { ::StopDialog(); }
    void ResetDialogSubtitle() override { ::ResetDialogSubtitle(); }
    void PushMusic(int id) override { PsndPushMusic(id); }
    void PopMusic(int id) override { ::PopMusic(id); }

    // the binocucom (hooks, else the interim state machine)
    void OpenBinoc(void* ploActor, const BINOCOPEN3& b, void* ploSpeaker, void* plo60, void* plo64, void* plo68) override
    {
        if (FPrint()) std::printf("scpt3: frame %d binocucom open (actor %d, speaker %d)\n", s_cframe, Plo(ploActor) ? Plo(ploActor)->oid : -1,
                                  Plo(ploSpeaker) ? Plo(ploSpeaker)->oid : -1);
        if (g_scpthooks3.pfnOpenBinoc) return g_scpthooks3.pfnOpenBinoc(Plo(ploActor), b, Plo(ploSpeaker), Plo(plo60), Plo(plo64), Plo(plo68));
        s_binoc = {Plo(ploActor), Plo(ploSpeaker), 0, 1, s_cframe + 40};
    }
    void CloseBinoc(void* ploActor) override
    {
        if (FPrint()) std::printf("scpt3: frame %d binocucom close\n", s_cframe);
        if (g_scpthooks3.pfnCloseBinoc) return g_scpthooks3.pfnCloseBinoc(Plo(ploActor));
        s_binoc.state = 0;
        s_binoc.stateTarget = -1;
        s_binoc.cframeReady = s_cframe + 20;
    }
    int BinocState(void* ploActor) override
    {
        if (g_scpthooks3.pfnBinocState) return g_scpthooks3.pfnBinocState(Plo(ploActor));
        return s_binoc.state;
    }
    void SetBinocSpeaker(void* ploActor, void* ploSpeaker) override
    {
        if (FPrint()) std::printf("scpt3: frame %d binocucom speaker %d\n", s_cframe, Plo(ploSpeaker) ? Plo(ploSpeaker)->oid : -1);
        if (g_scpthooks3.pfnSetBinocSpeaker) return g_scpthooks3.pfnSetBinocSpeaker(Plo(ploActor), Plo(ploSpeaker));
        s_binoc.ploSpeaker = Plo(ploSpeaker);
        s_binoc.state = 0;
        s_binoc.stateTarget = 1;
        s_binoc.cframeReady = s_cframe + 60;
    }
    void CmFocus(void* a, int kind, void* plo) override { if (g_scpthooks3.pfnCmFocus) g_scpthooks3.pfnCmFocus(Plo(a), kind, Plo(plo)); }
    void CmTargets(void* a, const float pos[3], const float dir[3], float g) override { if (g_scpthooks3.pfnCmTargets) g_scpthooks3.pfnCmTargets(Plo(a), pos, dir, g); }
    void CmZoom(void* a, float g) override { if (g_scpthooks3.pfnCmZoom) g_scpthooks3.pfnCmZoom(Plo(a), g); }
    void CmSnap(void* a) override { if (g_scpthooks3.pfnCmSnap) g_scpthooks3.pfnCmSnap(Plo(a)); }
    bool FCmDone(void* a, int id) override { return g_scpthooks3.pfnFCmDone ? g_scpthooks3.pfnFCmDone(Plo(a), id) : true; }
    void CutCm() override { ::CutCm(&g_cm); }

    // the run's camera keys (0x45): StartRunCamera 0x22BB78 / the key policy 0x16B670 / EndRunCamera 0x22BEE8 on the
    // world camera; the camera follows the newest run's keys
    std::map<const RUN3*, CPKEY3> mprunkey;
    const RUN3* prunCamera = nullptr;
    static LO* PloFindOid(int oid) { return s_psw ? PloFindSwObjectByOid(s_psw, oid) : nullptr; }
    static CMPOSE3 PoseOfCm()
    {
        CMPOSE3 c;
        c.pos = g_cm.pos;
        c.mat = g_cm.mat;
        c.radFov = g_cm.radFOV;
        return c;
    }
    void StartRunCamera(RUN3& run, const SCPTCMD3& key, const SCPTCMD3* pnext, const SCPTCMD3* pnext2) override
    {
        const CMKEY3 k = KeyFromCmd3(key, PloFindOid);
        const CMKEY3 kn = pnext ? KeyFromCmd3(*pnext, PloFindOid) : k;
        const CMKEY3 kn2 = pnext2 ? KeyFromCmd3(*pnext2, PloFindOid) : kn;
        const bool fReal = run.pscpt && (run.pscpt->grf & 0x200);
        StartCpkey3(mprunkey[&run], k, kn, kn2, PoseOfCm(), T(fReal), fReal);
        prunCamera = &run;
    }
    void EndRunCamera(RUN3& run) override
    {
        mprunkey.erase(&run);
        if (prunCamera == &run) prunCamera = nullptr;
    }
    // (after the camera's pass: the active key's pose on the world camera)
    void UpdateCamera()
    {
        if (!prunCamera) return;
        const auto it = mprunkey.find(prunCamera);
        if (it == mprunkey.end()) return;
        CMPOSE3 c = PoseOfCm();
        UpdateCpkey3(it->second, &c, T(it->second.fRealTime));
        g_cm.pos = c.pos;
        g_cm.mat = c.mat;
        SetCmFov(&g_cm, c.radFov);
        if (static const bool s_f = std::getenv("SLY3_CAMPRINT") != nullptr; s_f)
            std::printf("scpt3: frame %d camera key of %d: %.1f %.1f %.1f dir %.3f %.3f %.3f fov %.3f\n", s_cframe,
                        prunCamera->pscpt ? prunCamera->pscpt->oid : -1, c.pos.x, c.pos.y, c.pos.z, c.mat[0].x, c.mat[0].y,
                        c.mat[0].z, c.radFov);
    }

    // the tasks (0x1E: ApplyCheckpointTasks 0x185908, task3.cpp)
    void ApplyCheckpointTasks(int oidTask, int state, void* ploContext) override
    {
        const int apair[2] = {oidTask, state};
        if (FPrint()) std::printf("scpt3: frame %d task %d -> %d\n", s_cframe, oidTask, state);
        ApplyCheckpointTasks3(apair, 1, ploContext);
    }
    // the title (0x50)
    void ShowTitle() override
    {
        if (FPrint()) std::printf("scpt3: frame %d the title\n", s_cframe);
        if (g_scpthooks3.pfnShowTitle) g_scpthooks3.pfnShowTitle();
        s_title.fShown = true;
    }
    bool FTitleShowing() override { return FTitleShowing3(); }

    // the actors (hooks, else nothing plays: the waits end at once)
    void PlayActorScriptAnim(void* a, int oidAnim, int grf, int n) override
    { if (g_scpthooks3.pfnPlayActorScriptAnim) g_scpthooks3.pfnPlayActorScriptAnim(Plo(a), oidAnim, grf, n); }
    void EndActorScriptAnim(void* a) override { if (g_scpthooks3.pfnEndActorScriptAnim) g_scpthooks3.pfnEndActorScriptAnim(Plo(a)); }
    bool FActorInScriptAnim(void* a) override { return g_scpthooks3.pfnFActorInScriptAnim && g_scpthooks3.pfnFActorInScriptAnim(Plo(a)); }
    void PlaceLo(void* plo, const float mat[9], const float pos[3]) override
    {
        if (g_scpthooks3.pfnPlaceLo) return g_scpthooks3.pfnPlaceLo(Plo(plo), mat, pos);
        if (LO* p = Plo(plo)) sly3::PlaceLo3(p, mat, glm::vec3(pos[0], pos[1], pos[2]));
    }
    void ResetActorAfterPlace(void* a) override { if (g_scpthooks3.pfnResetActorAfterPlace) g_scpthooks3.pfnResetActorAfterPlace(Plo(a)); }
    void LockPlayerControl(bool fLock) override
    {
        if (FPrint()) std::printf("scpt3: frame %d player control %s\n", s_cframe, fLock ? "locked" : "released");
        if (g_scpthooks3.pfnLockPlayerControl) g_scpthooks3.pfnLockPlayerControl(fLock);
    }
    void ActorMoveTo(void* a, void* plo, void*, const float pos[3], int b4, int b8) override
    { if (g_scpthooks3.pfnActorMoveTo) g_scpthooks3.pfnActorMoveTo(Plo(a), Plo(plo), pos, b4, b8); }
    // 0x30's done test (the actor walks toward the point until within the radius): with no actor locomotion (the player code's
    // pfnActorMoveTo unset) the actor is put at the point (interim, so the attract's walks end)
    void SetActorWatchTarget(void* a, const float pos[3]) override
    {
        if (g_scpthooks3.pfnActorMoveTo || !a) return;
        float mat[9], t[3];
        if (!GetLoXf(a, mat, t)) return;
        sly3::PlaceLo3(Plo(a), mat, glm::vec3(pos[0], pos[1], t[2]));
    }
    int ActorMoveState(void* a) override { return g_scpthooks3.pfnActorMoveState ? g_scpthooks3.pfnActorMoveState(Plo(a)) : -1; }
    void FaceObject(void* a, void* plo) override { if (g_scpthooks3.pfnFaceObject) g_scpthooks3.pfnFaceObject(Plo(a), Plo(plo)); }
    // the world transform of an object (rows, position)
    bool GetLoXf(void* plo, float mat[9], float pos[3]) override
    {
        LO* p = Plo(plo);
        if (!p) return SCPTHOST3::GetLoXf(plo, mat, pos);
        const glm::mat4 m = sly3::MatLoWorld3(p);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) mat[r * 3 + c] = m[r][c];
        for (int i = 0; i < 3; ++i) pos[i] = m[3][i];
        return true;
    }

    // the screen objects (hooks, else nothing shown)
    void ShowScreenObj(void* plo, int n) override { if (g_scpthooks3.pfnShowScreenObj) g_scpthooks3.pfnShowScreenObj(Plo(plo), n); }
    void HideScreenObj(void* plo) override { if (g_scpthooks3.pfnHideScreenObj) g_scpthooks3.pfnHideScreenObj(Plo(plo)); }
    bool FScreenObjDone(void* plo) override { return g_scpthooks3.pfnFScreenObjDone ? g_scpthooks3.pfnFScreenObjDone(Plo(plo)) : true; }
    void ShowHint(int id, float g) override { if (g_scpthooks3.pfnShowHint) g_scpthooks3.pfnShowHint(id, g); }
    void ClearHint(int id) override { if (g_scpthooks3.pfnClearHint) g_scpthooks3.pfnClearHint(id); }
    // (the AI code) 0x62: TellGuardCommands 0x198540 (the guard's job 0xC8 = Sly 2's reactions 0x9C, message 5 with the list)
    // and FGuardRunsCommands 0x1985F0: the copied Sly 2 bodies (scptai.cpp 0x171CF8 / 0x171DA8)
    void TellGuardCommands(void* ploGuard, void* ploCmds) override { ::TellGuardCommands(Plo(ploGuard), Plo(ploCmds), true); }
    bool FGuardRunsCommands(void* ploGuard, void* ploCmds) override { return ::FGuardRunsCommands(Plo(ploGuard), Plo(ploCmds)); }
    // (the AI code) 0x6F / 0x70: a proxy's instance. 0x1FBF08: the current one (0x1FBEC0) else PloAcquireProxyObject
    // 0x1FBE30(proxy, 1); without the flag 0x1FBE30(proxy, 0). v_ext's task scripts place the guards of the streaming
    // proxies 139045..139049 this way at the start (the reference: AddLo from 0x1FBDA8 at vblanks 1767 / 1768),
    // after the proxies' own remove-at-start.
    void* PloProxyCurrent(void* pproxy) override
    {
        if (LO* p = PloProxyInstanceOf(Plo(pproxy))) return p;
        return PloAcquireProxyObject(s_psw, Plo(pproxy), true);
    }
    void* PloProxyAcquire(void* pproxy) override { return PloAcquireProxyObject(s_psw, Plo(pproxy), false); }
    void LockProxyRow(void* pproxy, void* plo) override { LockProxyRowOf(s_psw, Plo(pproxy), Plo(plo)); }
    void* PloProxyInstance(void* pproxy) override { return PloProxyInstanceOf(Plo(pproxy)); }
    void UnlockProxyRow(void* pproxy, void* plo) override { UnlockProxyRowOf(s_psw, Plo(pproxy), Plo(plo)); }
    void ReleaseProxyInstance(void* pproxy, void* plo) override
    {
        // 0x1FC0E0: the instance's row given back (detached from its proxy)
        ReleaseProxyObject(s_psw, Plo(pproxy), Plo(plo));
    }
    // 0x1FBEC0: the instance attached to the proxy (one object per instance), if any
    static LO* PloProxyInstanceOf(LO* pproxy)
    {
        if (!s_psw || !pproxy) return nullptr;
        const auto it = s_psw->mpproxy.find(pproxy);
        if (it == s_psw->mpproxy.end() || it->second.iwe < 0 || it->second.iwe >= int(s_psw->awe.size())) return nullptr;
        const WE& we = s_psw->awe[it->second.iwe];
        if (we.cloLoaded != 1) return nullptr;
        for (std::size_t i = 0; i < we.aploRowOwner.size() && i < we.aplo.size(); ++i)
            if (we.aploRowOwner[i] == pproxy) return we.aplo[i];
        return nullptr;
    }
    void SetButtonPrompt(int grf) override { if (g_scpthooks3.pfnSetButtonPrompt) g_scpthooks3.pfnSetButtonPrompt(grf); }
    // 0x4E: FButtonPressed 0x1A7A80(pad, mask, 0): the buttons (the game's layout) pressed this frame
    bool FButtonPressed(void*, int grfMask) override { return (g_joyps2.grfbtnPressed & uint32_t(grfMask)) != 0; }
    // 0x1D: SetClockFactor 0x165548(g_clock, 6, g)
    void SetClockFactor(float g) override { ::SetClockFactor(&g_clock, 6, g); }
    // 0x38 / 0x39: AddRemoveByOid 0x25D740 (an ALO faded in / out over g seconds, else added / removed now: the
    // ASEG events 7 / 8's 0x210338 part, asegevent.cpp SwitchLoFaded)
    void* AddRemoveByOid(bool fAdd, int oid, float g, void* ploContext, SCPT3*) override
    {
        LO* plo = Plo(PloFind(4, oid, ploContext));
        if (!plo || !s_psw) return nullptr;
        SwitchLoFaded(plo, fAdd, g);
        return plo;
    }
    // 0x41 (and the ASEG event 17): 0x25D940(sw, effect, source, scale, ctx): a particle effect (PE 0x2D) started
    // (0x1FE728: the effects code's TriggerPe), else an explosion: its burst from the source (or itself), scaled when g != 1
    void Cmd41(int oidEffect, int oidSource, float g, void* ploContext, SCPT3*) override
    {
        if (oidEffect == -1) return;
        LO* plo = Plo(PloFind(4, oidEffect, ploContext));
        if (!plo) return;
        if (FCidDerivedFrom(plo->Cid(), sly3::CID_PE))
        {
            TriggerPe(plo, nullptr);
            return;
        }
        BURSTPARAMS bp;
        bp.grf = 1;
        bp.plo = oidSource != -1 ? Plo(PloFind(4, oidSource, ploContext)) : nullptr;
        if (!bp.plo) bp.plo = plo;
        if (g != 1.0f)
        {
            bp.g8 = g;
            bp.grf |= 0x18;
        }
        ExplodeLo(plo, &bp);
    }
    // 0x43: PlayAsegaSound 0x25DB38 (the sound id, its source (-1: the context), the falloff, then +0x10 the pitch,
    // +0x18 the volume: the ASEG event 0's player, asegevent.cpp); a looping sound is kept until the run retires
    std::vector<CSndRef> asndRun;
    void* PlayAsegaSound(int id, int oidSource, int n2, int n3, float g0, float, float g2, void* ploContext, SCPT3*) override
    {
        if (id == -1) return nullptr;
        LO* pso = oidSource == -1 ? Plo(ploContext) : Plo(PloFind(4, oidSource, ploContext));
        CSnd* psnd = PsndNewPlain(id);
        if (!psnd) return nullptr;
        psnd->SetSource(pso);
        psnd->SetFalloff(float(n2), float(n3));
        psnd->SetVolume(g2);
        psnd->SetPitch(g0);
        if (psnd->Kind() != 2) return nullptr;
        asndRun.emplace_back(psnd);
        return psnd;
    }
    void StopSound(void* pv) override
    {
        for (auto it = asndRun.begin(); it != asndRun.end(); ++it)
            if (it->get() == pv)
            {
                (*it)->Stop(0.0f);
                asndRun.erase(it);
                return;
            }
    }
    // 0x33 / 0x34 / 0x63: the AI's side (the AI code's hooks; interim: nothing runs, done at once). (0x62: the AI code's above)
    void ActorPathStep(void* pa, void* pcrv, int b1, int b2, float g3) override
    { if (g_scpthooks3.pfnActorPathStep) g_scpthooks3.pfnActorPathStep(Plo(pa), Plo(pcrv), b1, b2, g3); }
    void ActorStepTo(void* pa, const float pos[3], float g1, int b2, float g3) override
    { if (g_scpthooks3.pfnActorStepTo) g_scpthooks3.pfnActorStepTo(Plo(pa), pos, g1, b2, g3); }
    void* RunSmGoal(int oid, int oidSm, int oidGoal, bool fSnap, void* ploContext) override
    {
        if (!s_psw || oid == -1) return nullptr;
        // CploFindSwNearest(sw, 0x204, oid, ctx, 1): exactly one such object
        LO* plo = nullptr;
        int c = 0;
        for (const auto& pl : s_psw->aploAll)
            if (pl->oid == oid) { plo = pl.get(); ++c; }
        if (c != 1)
            if (LO* ploCtx = Plo(ploContext)) { plo = PloFindSwNearest(ploCtx, oid); c = plo ? 1 : 0; }
        if (c != 1 || !plo || !FCidDerivedFrom(plo->Cid(), sly3::CID_ALO)) return nullptr;
        SMA* psma = nullptr;  // PsmaFindAloSm 0x23FB68
        for (SMA* pa : g_apsma)
            if (pa->palo == plo && (oidSm == -1 || (pa->psm && pa->psm->oid == oidSm))) { psma = pa; break; }
        if (!psma || oidGoal == -1) return nullptr;
        if (fSnap) SnapSmaGoal(psma, oidGoal);
        else SetSmaGoal(psma, oidGoal);
        return psma;
    }
    bool FSmGoalReached(void* pv, int oidGoal) override
    {
        const SMA* psma = static_cast<const SMA*>(pv);
        if (std::find(g_apsma.begin(), g_apsma.end(), psma) == g_apsma.end()) return true;  // (freed)
        return FSmaRetracted(psma) || OidSmaGoal(psma) == oidGoal;
    }
    void GuardCommand63(void* pg, void* plo) override
    { if (g_scpthooks3.pfnGuardCommand63 && pg) g_scpthooks3.pfnGuardCommand63(Plo(pg), Plo(plo)); }
    // 0x42 / 0x55 / 0x56 / 0x29: the copied Sly 2 code's bodies (soundglue.cpp, binoc.cpp, interact.cpp)
    void SetEyesClosed(int oid, int n, float g, void* ploContext) override { ::SetEyesClosedOf(Plo(ploContext), oid, n, g); }
    void HoldActorEffects(void* pa, bool fHold, bool f, int grf) override { ::HoldActorEffects(Plo(pa), fHold, f, uint32_t(grf)); }
    void SetActorInteractObject(void* pa, void* plo) override
    { if (ACTOR* p = PactorFromLo(Plo(pa))) ::SetActorInteractObject(p, 5, Plo(plo)); }
    bool FActorJobDone(void* pa, int kind) override
    { return !g_scpthooks3.pfnFActorJobDone || g_scpthooks3.pfnFActorJobDone(Plo(pa), kind); }
    // 0x68: TriggerExit 0x27FCC8 (the world flow code): the point (exit +0x178, the option field EOPID 834 writes) unless -1
    void TriggerExit(void* pv, int oidPoint) override
    {
        LO* plo = Plo(pv);
        if (!plo) return;
        if (oidPoint != -1) plo->SetField(0, OFF_EXIT_POINT, uint32_t(oidPoint));   // (+0x178: frontend.h)
        if (std::getenv("SLY2N_EXITPRINT")) std::printf("scpt3: exit %d triggered (point %d)\n", plo->oid, oidPoint);
        if (g_scpthooks3.pfnTriggerExit) g_scpthooks3.pfnTriggerExit(plo);
        else ::TriggerExit(plo);
    }
    // (the executor's trace: SLY3_SCPTCMP=FILE in tools/scpttrace3.py's format, for scptcmp3.py)
    FILE* pfCmp = nullptr;
    std::map<std::pair<RUN3*, int>, bool> mpdone;
    void Executed(RUN3& run, int icmd) override
    {
        if (FPrint() && run.pscpt) std::printf("scpt3: frame %d exec %d [%d] %02X %s\n", s_cframe, run.pscpt->oid, icmd,
                                               run.pscpt->acmd[std::size_t(icmd)].id, SzScptCmd3(run.pscpt->acmd[std::size_t(icmd)].id));
        if (!pfCmp) return;
        const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
        std::fprintf(pfCmp, "%5d exec oid %d [%d] %02X\n", s_cframe, run.pscpt->oid, icmd, c.id);
    }
    void DoneTested(RUN3& run, int icmd, bool fDone) override
    {
        if (!pfCmp) return;
        auto [it, fNew] = mpdone.try_emplace({&run, icmd}, fDone);
        if (!fNew && it->second == fDone) return;
        it->second = fDone;
        const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
        std::fprintf(pfCmp, "%5d done oid %d [%d] %02X -> %d\n", s_cframe, run.pscpt->oid, icmd, c.id, fDone ? 1 : 0);
    }
};
GAMESCPTHOST3 s_host;
std::vector<std::unique_ptr<SCPT3>> s_ascpt;

// ---------------------------------------------------------------- the SCPT / TASK splice methods
// orig3: thunks 0x108918..0x108C20 (EOPIDs 747..754: RefSetArgsFromSplice, then the executor's function)
CRef RefScptStart(LO* plo, const std::vector<CRef>& a)  // 0x108918 (747): StartScpt 0x225EE8(scpt, the argument)
{
    LO* ploStarter = !a.empty() ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
    if (FPrint()) std::printf("scpt3: frame %d splice starts SCPT %d\n", s_cframe, plo ? plo->oid : -1);
    StartScptOfLo3(plo, ploStarter);
    return CRef(TAGK_Void);
}
CRef RefScptStartJob(LO* plo, const std::vector<CRef>&)  // 0x1089A0 (748): 0x226010 StartScpt(scpt, PjobFind(GAME, 0))
{
    StartScptOfLo3(plo, nullptr);  // (TODO(sly3): PjobFind 0x1F4AD0's job 0 as the starter)
    return CRef(TAGK_Void);
}
CRef RefScptStop(LO* plo, const std::vector<CRef>&)  // 0x108A18 (749): StopScpt 0x226050
{
    if (SCPT3* ps = PscptOfLo3(plo)) StopScpt3(ps);
    return CRef(TAGK_Void);
}
CRef RefScptSetAlarm(LO* plo, const std::vector<CRef>& a)  // 0x108A90 (751): 0x2261A0
{
    if (SCPT3* ps = PscptOfLo3(plo); ps && !a.empty()) SetScptAlarm3(ps, a[0].RefCoerceS32().m_n, true);
    return CRef(TAGK_Void);
}
CRef RefScptClearAlarm(LO* plo, const std::vector<CRef>& a)  // 0x108B18 (752): 0x2261F0
{
    if (SCPT3* ps = PscptOfLo3(plo); ps && !a.empty()) SetScptAlarm3(ps, a[0].RefCoerceS32().m_n, false);
    return CRef(TAGK_Void);
}
CRef RefScptAlarm(LO* plo, const std::vector<CRef>& a)  // 0x108BA0 (753): 0x226270, the alarm is set
{
    CRef r;
    SCPT3* ps = PscptOfLo3(plo);
    r.SetBool(ps && !a.empty() && FScptAlarm3(ps, a[0].RefCoerceS32().m_n));
    return r;
}
CRef RefScptClearAlarms(LO* plo, const std::vector<CRef>&)  // 0x108C20 (754): 0x2262B0 (SCPT +0x64 = 0)
{
    if (SCPT3* ps = PscptOfLo3(plo)) ps->aalarm.clear();
    return CRef(TAGK_Void);
}

struct INITSCPT3
{
    INITSCPT3()
    {
        RegisterObjectMethod3(0x108918, RefScptStart);
        RegisterObjectMethod3(0x1089A0, RefScptStartJob);
        RegisterObjectMethod3(0x108A18, RefScptStop);
        RegisterObjectMethod3(0x108A90, RefScptSetAlarm);
        RegisterObjectMethod3(0x108B18, RefScptClearAlarm);
        RegisterObjectMethod3(0x108BA0, RefScptAlarm);
        RegisterObjectMethod3(0x108C20, RefScptClearAlarms);
        // EOPID 1415 (TASK +0x44, grf 2): the task's state
        RegisterObjectGetter3(1415, [](LO* plo) {
            CRef r;
            if (const TASK3* p = plo ? PtaskFind3(plo->oid) : nullptr) r.SetS32(p->nState);
            return r;
        });
        // each frame: the running scripts (the SCPT's vt slot 16 0x225130, before the objects' updates), the debug task
        // requests; after the camera's pass (0x14D478): the script camera
        RegisterSwPass(0x1F77C0, [](SW* psw) { if (s_fActive && psw == s_psw) UpdateScripts3Frame(); }, __FILE__);
        // the active triggers (vt slot 55 0x15D510 each, after the objects' updates: trigger3.h)
        RegisterSwPass(0x19F120, [](SW* psw) { if (s_fActive && psw == s_psw) sly3::UpdateTriggers3(psw); }, __FILE__);
        RegisterSwPass(0x20F7E0, [](SW* psw) { if (s_fActive && psw == s_psw) s_host.UpdateCamera(); }, __FILE__);
    }
    static void UpdateScripts3Frame();
} s_initscpt3;

// SLY3_TASKREQ="FRAME:OID=STATE;..." (debug / harness)
struct TASKREQ { int frame, oid, n; };
std::vector<TASKREQ> AtaskreqEnv()
{
    std::vector<TASKREQ> a;
    if (const char* sz = std::getenv("SLY3_TASKREQ"))
    {
        std::stringstream ss(sz);
        for (std::string tok; std::getline(ss, tok, ';');)
        {
            TASKREQ r{};
            if (std::sscanf(tok.c_str(), "%d:%d=%d", &r.frame, &r.oid, &r.n) == 3) a.push_back(r);
        }
    }
    return a;
}

void INITSCPT3::UpdateScripts3Frame()
{
    ++s_cframe;
    static const std::vector<TASKREQ> s_areq = AtaskreqEnv();
    for (const TASKREQ& r : s_areq)
        if (r.frame == s_cframe) RequestTaskState3(r.oid, r.n);
    // the title: START (orig3 0x221C08's part the boot's title uses), or SLY3_START_AT's frames
    if (s_title.fShown && !g_scpthooks3.pfnFTitleShowing)
    {
        bool fStart = (g_joy.grfbtnPressed & BTN_Start) != 0;
        for (int f : s_title.aframeStart) fStart = fStart || f == s_cframe;
        if (fStart)
        {
            s_title.fShown = false;
            std::printf("title: frame %d START: the title closes\n", s_cframe);
        }
    }
    // (debug) SLY3_INWORLD=FRAME:FILE: every object's oid, class and in-world state at that frame
    // (tools/s3script/inworld3.py compares it with a reference RAM dump's census)
    if (static const char* s_sz = std::getenv("SLY3_INWORLD"); s_sz && std::atoi(s_sz) == s_cframe && s_psw)
        if (const char* pc = std::strchr(s_sz, ':'))
            if (FILE* pf = std::fopen(pc + 1, "w"))
            {
                for (const auto& pl : s_psw->aploAll)
                    std::fprintf(pf, "%d 0x%X %d removed %d parent %d\n", pl->oid, pl->Cid(), pl->fInWorld ? 1 : 0, pl->fRemoved ? 1 : 0, pl->paloParent ? pl->paloParent->oid : -1);
                std::fclose(pf);
            }
    UpdateInterimBinoc();
    for (const auto& ps : s_ascpt)
        if (ps->fRunning) UpdateScpt3(ps.get(), (ps->grf & 0x200) ? g_clock.dtReal : g_clock.dt);
}
}  // namespace

void StartupScripts3(SW* psw)
{
    ResetScripts3();
    s_psw = psw;
    s_fActive = true;
    if (const char* e = std::getenv("SLY3_START_AT"))
    {
        std::stringstream ss(e);
        for (std::string tok; std::getline(ss, tok, ',');) s_title.aframeStart.push_back(std::atoi(tok.c_str()));
    }
    for (LO* plo : psw->mpcidaplo[0x87])
    {
        const LOSCPT3* plos = dynamic_cast<const LOSCPT3*>(plo);
        if (!plos) continue;
        auto ps = std::make_unique<SCPT3>();
        ps->oid = plo->oid;
        ps->acmd = plos->acmd;
        ps->plo = plo;
        // +0x3C: the option 0x200 (real time) (TODO(sly3): its EOPID; none of the prologue's scripts sets it)
        s_host.mpploscpt[plo] = ps.get();
        s_ascpt.push_back(std::move(ps));
    }
    SetScptHost3(&s_host);
    if (const char* sz = std::getenv("SLY3_SCPTCMP"); sz && !s_host.pfCmp) s_host.pfCmp = std::fopen(sz, "w");
    // the tasks (task3.h): their records, the host
    InitTasks3(psw);
    g_taskhost3.pfnTime = [] { return g_clock.t; };
    g_taskhost3.pfnAutosave = [] { HudAutoSave(false); };   // orig3 0x1CFE08 (hudwidget.cpp: Sly 3's rules; the world flow code)
    g_taskhost3.pfnMessage = [](TASK3* p, int msg) {
        if (p->plo) HandleLoSpliceEvent(p->plo, uint32_t(msg), false, 0, nullptr);
    };
    g_taskhost3.pfnMessageArgs = [](LO* plo, int msg, int n, LO* ploArg) {
        CRef a[2];
        a[0].SetS32(n);
        a[1].SetBasic(ploArg);
        HandleLoSpliceEvent(plo, uint32_t(msg), false, 2, a);
    };
    g_taskhost3.pfnStartScpt = [](LO* plo) {
        if (FPrint()) std::printf("scpt3: frame %d task starts SCPT %d\n", s_cframe, plo ? plo->oid : -1);
        StartScptOfLo3(plo, nullptr);
    };
    // the triggers (class 0x15: their SCPTs and task pairs, trigger3.h)
    sly3::StartTriggers3(psw);
    sly3::g_trighost3.pfnMessage = [](LO* plo, int msg) { HandleLoSpliceEvent(plo, uint32_t(msg), false, 0, nullptr); };
    sly3::g_trighost3.pfnStartScpt = [](LO* pscpt, LO* ploArg) { StartScptOfLo3(pscpt, ploArg); };
    // orig3: 0x185A70 ApplyTaskObjects, 0x185DB0 ApplyTaskSms
    g_taskhost3.pfnApplyObjects = [](TASK3* p, int n) {
        ApplyTaskObjects3(s_psw, p, n);
        ApplyTaskSms3(s_psw, p, n);
    };
    if (FPrint()) std::printf("scpt3: %zu scripts, %zu tasks, %zu jobs\n", s_ascpt.size(), g_tasks3.atask.size(), g_tasks3.ajob.size());
}

void RestoreTasks3AtStart(SW* psw)
{
    if (!s_fActive || psw != s_psw) return;
    RestoreTasks3();
    if (std::getenv("SLY3_TASKPRINT")) PrintTasks3(stdout);
}

void ResetScripts3()
{
    // (a new world: the old world's scripts go with its objects, without their stop events: those reach the old
    // world's freed objects (a world reloaded by an exit crashed in HandleLoSpliceEvent); the original's SW is simply
    // cleared by the transition 0x25C2A8's memset. the world flow code)
    s_ascpt.clear();
    s_host.mpploscpt.clear();
    s_host.asndRun.clear();
    s_mpwdialog.clear();
    s_host.mprunkey.clear();
    s_host.prunCamera = nullptr;
    s_psw = nullptr;
    s_fActive = false;
    s_cframe = 0;
    s_title = TITLE3{};
    s_binoc = BINOC3{};
}

bool FScripts3Active() { return s_fActive; }

SCPT3* PscptOfLo3(LO* plo) { return s_host.PscptFromLo(plo); }

void StartScptOfLo3(LO* pscpt, LO* ploStarter)
{
    if (SCPT3* ps = PscptOfLo3(pscpt)) StartScpt3(ps, ploStarter);
}

bool FTitleShowing3() { return g_scpthooks3.pfnFTitleShowing ? g_scpthooks3.pfnFTitleShowing() : s_title.fShown; }

bool FScriptCameraActive3() { return s_host.prunCamera != nullptr; }

void RequestTaskState3(int oidTask, int n)
{
    if (TASK3* p = PtaskFind3(oidTask))
    {
        if (FPrint() || std::getenv("SLY3_TASKPRINT")) std::printf("scpt3: frame %d request task %d -> %d\n", s_cframe, oidTask, n);
        QueueTaskRequest3(0, p, n, 0, nullptr);
    }
}

void TaskKeys3Of(int akey[3]) { TaskKeys3(akey); }

void SetWorldDialogTable3(int i, const std::vector<int>& aid0, const std::vector<int>& aid1)
{
    WDIALOG3& d = s_mpwdialog[i];
    d = WDIALOG3{aid0, aid1, {}, 0, -1};
    if (FPrint()) std::printf("scpt3: world dialog table %d: %zu / %zu ids\n", i, aid0.size(), aid1.size());
}

int IdWorldDialog3(int i, bool fSecond)
{
    auto it = s_mpwdialog.find(i);
    if (it == s_mpwdialog.end()) return -1;
    WDIALOG3& d = it->second;
    const std::vector<int>& a = fSecond ? d.aid1 : d.aid0;
    const int c = int(std::max(d.aid0.size(), d.aid1.size()));
    if (c <= 0) return -1;
    if (d.inext >= d.abag.size())  // 0x2BF928: a new round, shuffled, not starting with the last index
    {
        d.abag.resize(std::size_t(c));
        for (int k = 0; k < c; ++k) d.abag[std::size_t(k)] = k;
        for (int k = c - 1; k > 0; --k) std::swap(d.abag[std::size_t(k)], d.abag[std::size_t(int(GRandInRange(0.0f, float(k) + 0.999f)))]);
        if (c > 2 && d.abag[0] == d.ilast) std::swap(d.abag[0], d.abag[std::size_t(1 + int(GRandInRange(0.0f, float(c - 1) - 0.001f)))]);
        d.inext = 0;
    }
    d.ilast = d.abag[d.inext++];
    return std::size_t(d.ilast) < a.size() ? a[std::size_t(d.ilast)] : -1;
}
