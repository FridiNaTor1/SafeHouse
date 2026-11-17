// Sly 3's scripts and tasks in the game (scptgame3.h).

#include "ai.h"
#include "scptgame3.h"
#include "../step.h"
#include "../hackgame3.h"
#include "../challenge3.h"
#include "../aijobs.h"
#include "../aicarm3.h"
#include "../skipcut.h"
#include "../frontend.h"
#include "../hudwidget.h"
#include "../jobend3.h"
#include "../epmenu3.h"
#include "../transition.h"

#include "cmkey3.h"
#include "director3.h"
#include "scptrun3.h"
#include "sly3.h"
#include "task3.h"
#include "trigger3.h"

#include "../activate.h"
#include "../marker.h"
#include "../codepanel3.h"
#include "../alo.h"
#include "../animfind.h"
#include "../asega.h"
#include "../aseg.h"
#include "../clock.h"
#include "../frame.h"
#include "../sidebag.h"
#include "../cm.h"
#include "../emitter.h"
#include "../smrun.h"
#include "../sm.h"
#include "../actor.h"
#include "../combo3.h"
#include "../damage.h"
#include "../jt.h"
#include "../jump.h"
#include "../player3.h"
#include "../gang.h"
#include "../crv.h"
#include "../climb.h"
#include "../sly.h"
#include "../binoc.h"
#include "../carry.h"
#include "../interact.h"
#include "../shd.h"
#include "../frontend.h"
#include "../hudwidget.h"
#include "../pe.h"
#include "../po.h"
#include "../rumble.h"
#include "../joy.h"
#include "../objmethod.h"
#include "../sound.h"
#include "../so.h"
#include "../trigger.h"
#include "../sw.h"
#include "../update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <set>
#include <sstream>
#include <unordered_set>
#include <string>
#include <vector>

constinit SCPTHOOKS3 g_scpthooks3;

float GRandInRange(float gLow, float gHigh);  // xfmath.h (its SMP clashes with vecmat.h's)
void TellGuardCommands(LO* pguard, LO* pcmds, bool fRun);  // scptai.cpp (0x171CF8)
bool FGuardRunsCommands(LO* pguard, LO* pcmds);             // scptai.cpp (0x171DA8)
void ApplyCheckpointLo(SW* psw, LO* plo); // transition.cpp (orig 0x228750; Sly 3 0x280010)
void StartFadeLo(LO* plo);                // hudwidget.cpp (0x15E1D8; Sly 3 0x17D7D0)
void UseObjectBy(LO* puse, ACTOR* pa);    // interact.cpp (the use's base 0x184130; Sly 3 0x1A20F8)
void ActivateHackDock(LO* plo);           // hack.cpp (Sly 3 0x19E1F8)
// orig3 0x1F4B38 for player 0 (SCPT 0x13, the world's option 983 with 0): the object takes the pad (see JobCommand)
void SetPlayer0Character3(SW* psw, LO* plo)
{
    if (plo && psw && PloPoCurrent(psw) != plo)
    {
        // (Sly 3: any SO takes the pad here (vt slot 82 with the player's index, 0x1F4548 -> slot 83: its input
        // +0x134), e.g. v_canal's police boat SO 2003, whose script reads the pad by its option 943; natively a
        // plain SO (not a PO, whose own code enables it) gets its controller state by the enable, and the old one
        // that was a plain SO gives it back (slot 82 with -1))
        auto fPlainSo = [](LO* p) { return p && FCidDerivedFrom(p->Cid(), sly3::CID_SO) && !FCidDerivedFrom(p->Cid(), sly3::CID_PO); };
        LO* pold = PloPoCurrent(psw);
        // (s3-ai: a guard class takes the pad by its slot 83 (Carmelita 0x162810: +0x130 the player index, her
        // reaction kind 20 = job 0xE9, aicarm3.cpp), e.g. Ep1's turf war SCPT 2152; the old guard gives it back)
        if (pold && pold != plo && PguardFromLo(pold) && IPlayerCarm3(pold) == 0) SetCarmPlayer3(pold, -1);
        if (PguardFromLo(plo) && FCidDerivedFrom(plo->Cid(), S3CID(0x3F))) SetCarmPlayer3(plo, 0);
        if (fPlainSo(plo)) EnablePo(psw, plo, 1);
        else SetPoCurrent(psw, plo);
        if (fPlainSo(pold) && NPoEnabled(pold) == 1) EnablePo(psw, pold, 0);
    }
    // (the new character takes the pad: its vt slot 83 with the player's +0x1040. A player made current without its
    // pad (e.g. Ep4's get_job after the reload at checkpoint 68635: no player selected at the world start, then SCPT
    // 3459's 0x13 to Sly) had no control: the photo tasks could not be played)
    if (plo && psw && PloPoCurrent(psw) == plo && FCidDerivedFrom(plo->Cid(), sly3::CID_PO) && NPoEnabled(plo) != 1)
        EnablePo(psw, plo, 1);
}

// (the route harness's waitcmd: how often each script command ran, keyed (script oid << 16) | index)
std::unordered_map<int64_t, int> g_mpcScptCmdExec3;

namespace
{
std::unordered_set<const LO*> s_setploAddedScript;   // (the objects SCPT 0x38 added since the world started)
}

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
    void* PloInWorldCopy(void* plo, void* ploNear) override
    {
        LO* p = Plo(plo);
        if (!p || p->fInWorld || !s_psw) return plo;
        LO* pnear = Plo(ploNear);
        LO* pbest = nullptr;
        float best = 3.4e38f;
        for (LO* q : AploOfOid(s_psw, p->oid))
        {
            if (!q || q->oid != p->oid || !q->fInWorld) continue;
            const glm::vec3 d = pnear ? sly3::PosLoWorld3(q) - sly3::PosLoWorld3(pnear) : glm::vec3(0.0f);
            if (glm::dot(d, d) < best) best = glm::dot(d, d), pbest = q;
        }
        if (pbest && FPrint()) std::printf("scpt3: frame %d %d out of the world: its in-world copy %p\n", s_cframe, p->oid, (void*)pbest);
        return pbest ? pbest : plo;
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
        if (static const bool s_f = std::getenv("SLY3_SCPTCONDPRINT") != nullptr; s_f)
            std::printf("scpt3: condition %d of %d: tagk %d n %d\n", kind, plo->oid, int(r.m_tagk), r.m_tagk == TAGK_S32 ? r.m_n : int(r.m_tagk == TAGK_Bool && r.m_bool));
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
        NoteStreamStarted3();   // (the song clock 0x2483D8: the last stream started)
        if (ploSource) p->SetSource(ploSource);
        if (d.sFalloffEnd > 0) p->SetFalloff(d.sFalloffStart, d.sFalloffEnd);
        p->SetVolume(d.gVolume);
        return true;
    }
    bool FDialogPlaying(int id) override { return ::FDialogPlaying(id); }
    void StopDialog() override { ::StopDialog(); }
    void ResetDialogSubtitle() override { ::ResetDialogSubtitle(); }
    void PushMusic(int id) override { PsndPushMusic(id); NoteStreamStarted3(); }
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
    // (the key policy's activation saves the camera's fov target, its deactivation restores it: orig 0x14FCC8 /
    // 0x14FCE8, CPLCY::Activate / Deactivate; the keys set the fov itself (SetCmFov: the target too), so without it a
    // script camera's last fov stayed: e.g. the hacks' intro cameras (ThiefNet's SCPT 3590 ends at 0.71) left the play
    // at 0.71 rad, the reference's at the camera's 1.0 (its picture wider))
    float radFovTargetSave = 0.0f;
    // (s3-player: a camera key's frame / look-at object: the in-world instance before its PROXY of the same oid
    // (v_interpol: Carmelita 28 has a proxy 28; the key in the proxy's frame missed her height and facing))
    static LO* PloFindOid(int oid)
    {
        if (!s_psw) return nullptr;
        LO* ploFirst = nullptr;
        for (LO* plo : AploOfOid(s_psw, OID(oid)))
        {
            if (!ploFirst) ploFirst = plo;
            if (plo->fInWorld && !FCidDerivedFrom(plo->Cid(), CID(sly3::CID_PROXY))) return plo;
        }
        return ploFirst ? ploFirst : PloFindSwObjectByOid(s_psw, oid);
    }
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
        if (static const bool s_f = std::getenv("SLY3_CAMPRINT") != nullptr; s_f)
            std::printf("scpt3: frame %d camera key start of %d: pos %.1f %.1f %.1f eul %.3f %.3f %.3f fov %.3f delay %.2f move %.2f grf 0x%X frame %d (%d) look %d (%d) posLook %.1f %.1f %.1f\n",
                        s_cframe, run.pscpt ? run.pscpt->oid : -1, k.pos.x, k.pos.y, k.pos.z, k.eul.x, k.eul.y, k.eul.z, k.fov, k.dtDelay, k.dtMove, k.grf,
                        key.an.size() > 10 ? key.an[10] : -1, k.ploPos ? k.ploPos->oid : -1, key.an.size() > 11 ? key.an[11] : -1, k.ploLook ? k.ploLook->oid : -1,
                        k.posLook.x, k.posLook.y, k.posLook.z);
        StartCpkey3(mprunkey[&run], k, kn, kn2, PoseOfCm(), T(fReal), fReal);
        if (!prunCamera) radFovTargetSave = g_cm.radFOVTarget;
        prunCamera = &run;
        SetScptCameraActive3(true);   // (PC, skipcut.h: a script holding the camera is a skippable cinematic)
    }
    void EndRunCamera(RUN3& run) override
    {
        mprunkey.erase(&run);
        if (prunCamera == &run)
        {
            prunCamera = nullptr;
            if (radFovTargetSave > 0.0f) g_cm.radFOVTarget = radFovTargetSave;   // (0x14FCE8: then smoothed back, cm.cpp)
            radFovTargetSave = 0.0f;
        }
        if (!prunCamera) SetScptCameraActive3(false);   // (PC, skipcut.h)
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
    // the ASEGs (0x16 on an object: PasegaApplyAseg 0x137428(aseg, plo, 0.0, 1.0, grf); the run waits for its
    // retraction while it holds it). Without these every script ASEG ended at once: m_ext's SCPT 4321 (task 4319
    // after the land beast intro 1803) loops 0x04 { 0x15, 0x06 { 0x16 } } and spun forever inside one frame.
    // 0x28: the actor's combo definition (kind-1 child) started (0x13A678 on +0x10D0, the object as its flag); done at
    // the runner's state -1 (0x229438 case 40)
    void Start28(void* ploActor, void* ploDef, void* plo) override
    {
        ACTOR* pa = PactorFromLo(Plo(ploActor));
        const bool f = pa && sly3::StartScriptCombo3(pa, Plo(ploDef), plo != nullptr);
        if (FPrint()) std::printf("scpt3: frame %d combo %d on %d -> %s\n", s_cframe, Plo(ploDef) ? Plo(ploDef)->oid : -1, Plo(ploActor) ? Plo(ploActor)->oid : -1, f ? "started" : "none");
    }
    bool FDone28(void* ploActor) override
    {
        ACTOR* pa = PactorFromLo(Plo(ploActor));
        return !pa || sly3::NCombo3State(pa) == -1;
    }
    void* ApplyAseg(void* ploAseg, void* plo, int grf) override
    {
        ASEG* paseg = static_cast<ASEG*>(Plo(ploAseg));
        if (!paseg) return nullptr;
        ASEGA* pasega = PasegaApplyAseg(paseg, static_cast<ALO*>(Plo(plo)), 0.0f, 1.0f, grf);
        if (FPrint()) std::printf("scpt3: frame %d aseg %d on %d grf %d -> %s\n", s_cframe, paseg->oid, plo ? Plo(plo)->oid : -1, grf,
                                  pasega ? "playing" : "none");
        return pasega;
    }
    bool FAsegaRetracted(void* pasega) override { return ::FAsegaRetracted(static_cast<ASEGA*>(pasega)); }
    void AddRefAsega(void* pasega) override { ::AddRefAsega(static_cast<ASEGA*>(pasega)); }
    void RetractAsega(void* pasega) override { ::RetractAsega(static_cast<ASEGA*>(pasega)); }
    void ReleaseAsega(void* pasega) override { ::ReleaseAsega(static_cast<ASEGA*>(pasega)); }
    // orig3 0x198D78(pos, 0, 0 (0x25CEC8: the still entries), 100, -350): the point onto what is under it, any slope
    void DropOnGround(float pos[3]) override
    {
        glm::vec3 p(pos[0], pos[1], pos[2]);
        const bool f = FDropOnGround(&p, 100.0f, -350.0f, true);
        if (FPrint()) std::printf("scpt3: frame %d drop %.0f %.0f %.0f -> %d %.0f\n", s_cframe, pos[0], pos[1], pos[2], int(f), p.z);
        if (!f) return;
        pos[0] = p.x, pos[1] = p.y, pos[2] = p.z;
    }
    void PlaceLo(void* plo, const float mat[9], const float pos[3]) override
    {
        if (g_scpthooks3.pfnPlaceLo) return g_scpthooks3.pfnPlaceLo(Plo(plo), mat, pos);
        if (LO* p = Plo(plo)) sly3::PlaceLo3(p, mat, glm::vec3(pos[0], pos[1], pos[2]));
    }
    void ResetActorAfterPlace(void* a) override { if (g_scpthooks3.pfnResetActorAfterPlace) g_scpthooks3.pfnResetActorAfterPlace(Plo(a)); }
    // orig3: 0x1F's PauseGame 0x1A93C8 -> 0x1A9630(the player's record 0x36E740 + i * 0x11C0, kind) / ResumeGame 0x1A9420 ->
    // 0x1A96C8: +0x1180 the pause count, kind & 1 +0x1188 (the subtitle box at 1), kind & 2 +0x118C, kind & 4 +0x1190.
    // +0x118C is what the AI asks (0x1A95D0(SW, -1): the guards' detectors see nothing, a waiting guard stands still,
    // 0x1EF950): natively Sly 2's g_gamepause.c84. Only that count is kept here (the AI's; TODO(sly3): +0x1180 / +0x1188
    // / +0x1190 and 0x1A97B8). E.g. v_ext's chase scene 2124 [4] .. [56]: the reference has +0x118C at 1 for the whole
    // scene, so Carmelita stands where her list ends; natively she walked home meanwhile.
    void PauseGame(int sel, int kind) override
    {
        // (+0x1180 / +0x1190 of the selected players' records: 0x1A9478 / 0x1A9600 for the actors' hits, FActorHitsPaused3)
        for (int i = 0; i < 2; ++i)
            if (sel != 1 - i)
            {
                ++s_ac1180[i];
                if (kind & 4) ++s_ac1190[i];
                // (kind 1: +0x1188; at 1 the letterbox / subtitle box 1060 shown: 0x1A9630 -> 0x1C4F70)
                if ((kind & 1) && ++s_ac1188[i] == 1) ShowSubtitleBox();
            }
        // (s3-ai's interim was opt-in: with the count on, prologue_end failed on a third water hit of the non-current
        // Bentley. s3-player, round 8: since the scripted walks arrive (the no-avoid flag) and a task names its player,
        // Bentley takes one hit there (t 247.8, with Sly, at the scene's end) with the count on or off, and the full
        // gate is the same with it: on by default, SLY3_AIPAUSE=0 turns it off.)
        static const bool s_fOn = [] { const char* sz = std::getenv("SLY3_AIPAUSE"); return !sz || sz[0] != '0'; }();
        if (!(kind & 2) || !s_fOn) return;
        ++g_gamepause.c84;
        ++s_cAiPause3;
        if (std::getenv("SLY2N_PAUSEPRINT")) std::printf("scpt3: frame %d AI pause on (kind %d): %d\n", s_cframe, kind, s_cAiPause3);
    }
    void ResumeGame(int sel, int kind) override
    {
        for (int i = 0; i < 2; ++i)
            if (sel != 1 - i)
            {
                if (s_ac1180[i] > 0) --s_ac1180[i];
                if ((kind & 4) && s_ac1190[i] > 0) --s_ac1190[i];
                // (kind 1: at 0 the box hidden: 0x1A96C8 -> 0x1C4FC0)
                if ((kind & 1) && s_ac1188[i] > 0 && --s_ac1188[i] == 0) HideSubtitleBox();
            }
        if (!(kind & 2) || s_cAiPause3 <= 0) return;
        --g_gamepause.c84;
        --s_cAiPause3;
        if (std::getenv("SLY2N_PAUSEPRINT")) std::printf("scpt3: frame %d AI pause off (kind %d): %d\n", s_cframe, kind, s_cAiPause3);
    }
    static inline int s_cAiPause3 = 0;
    static inline int s_ac1180[2] = {0, 0}, s_ac1188[2] = {0, 0}, s_ac1190[2] = {0, 0};   // (the players' records +0x1180 / +0x1190: the scripts' pauses)
    void LockPlayerControl(bool fLock) override
    {
        if (FPrint()) std::printf("scpt3: frame %d player control %s\n", s_cframe, fLock ? "locked" : "released");
        if (g_scpthooks3.pfnLockPlayerControl) g_scpthooks3.pfnLockPlayerControl(fLock);
    }
    // orig3 0x226D00 case 54 (SCPT 0x36, the actor leaps to an object): the object's interaction component 0x1A2A40 (Sly 3's
    // classes HIDE 0x64, CRAWLHOOK 0x19, WALL 0x20, LEDGE 0x0B, USE 0x21, RESPONSE 0x26, PIPE 0x0A, RAIL 0x0C, SPIRE 0x25,
    // 0x7A: the copied Sly 2 description lists, JtlDescFromLo); its slot 2 the description's parameter for the actor
    // (Sly 2's 0x184670 / GJtDescInit); 0x1C0538 with the entry {plo, component, pos} = Sly 2's 0x19F590 with an entry
    // (+0x4D0 = Sly 2's +0x490, the jump's kind 0xA: RecoverActorTo). E.g. m_ext's attract 4262 [22]: Sly onto the
    // rope 15075 (a RAIL), then its 0x30 walks along it.
    void* PnavOf(void* plo) override
    {
        LO* p = Plo(plo);
        return p && JtlDescFromLo(p) != JTL_Max ? plo : nullptr;
    }
    // (actor +0x1014 != -1 and +0x101C == plo: Sly 2's +0xF54 / +0xF5C, already on that object)
    bool FActorMovingTo(void* a, void* plo) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        return pa && pa->nSubF54 != -1 && pa->ploF5C == Plo(plo);
    }
    void NavGoal(void* pnav, void* a, float pos[3]) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        LO* p = Plo(pnav);
        const JTL jtl = JtlDescFromLo(p);
        pos[0] = pos[1] = pos[2] = 0.0f;
        if (pa && jtl != JTL_Max && jtl != JTL_Hook && jtl != JTL_Jump)
        {
            // (the actor's probe point from its world geometry as it stands now: m_ext's attract 4262 places Sly at 15071
            // ([14] 0x22) and leaps him onto the rope ([22]) in the same frame, before the world update refreshed the
            // geometry; with the stale one (his load place) the rope's nearest point was its far end, 900 off the
            // reference's landing. 0x1F38A8's lazy refresh, by the record's stamp)
            if (pa->palo)
                for (SOE& e : pa->palo->asoe) UpdateSoeWorld(&e);
            pos[0] = GJtDescInit(p, jtl, pa);
        }
        else if (p)
        {
            // (the hook / hiding spot point methods are not ported: their aim, as scpt.cpp's 0x2A)
            float mat[9];
            GetLoXf(pnav, mat, pos);
        }
    }
    void ActorAimOffset(void* a, float d[3]) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        for (int i = 0; i < 3; ++i) d[i] = pa ? pa->vecBase[i] : 0.0f;   // (actor +0x260, Sly 2's +0x220)
    }
    void ActorMoveTo(void* a, void* plo, void* pnav, const float pos[3], int b4, int b8) override
    {
        if (g_scpthooks3.pfnActorMoveTo) return g_scpthooks3.pfnActorMoveTo(Plo(a), Plo(plo), pos, b4, b8);
        ACTOR* pa = PactorFromLo(Plo(a));
        if (!pa) return;
        SAFEGROUND::ENTRY e;
        const JTL jtl = pnav ? JtlDescFromLo(Plo(pnav)) : JTL_Max;
        if (jtl != JTL_Max && jtl != JTL_Hook && jtl != JTL_Jump)
        {
            e.ploOwner = Plo(pnav);
            e.jtl = jtl;
            e.pos.x = pos[0];
        }
        else e.pos = glm::vec3(pos[0], pos[1], pos[2]);
        if (FPrint())
        {
            const glm::vec3 g = PosSafeGround(pa, e, 0.0f);
            std::printf("scpt3: frame %d 0x36 actor %d to %d (list %d) at %.0f %.0f %.0f: the goal %.0f %.0f %.0f\n", s_cframe,
                        pa->palo ? pa->palo->oid : -1, Plo(plo) ? Plo(plo)->oid : -1, int(jtl), e.pos.x, e.pos.y, e.pos.z, g.x, g.y, g.z);
        }
        // (also an actor that is not the current player: natively only the current player's states and jumps update
        // (ep1_heist_end's SCPT 2467 [85] leaps Sly while Bentley is played: the leap never ended and the scene stopped))
        if (pa->jump.pactor != pa || !s_psw || pa->palo != PloPoCurrent(s_psw))
        {
            // (an actor whose jump the native code never set up (InitJump: Murray, the gang's other members in Ep4's
            // 3533 [15], actor 30): placed at the goal instead of the leap; RecoverActorTo crashed in SetJumpKind)
            float mat[9], t[3];
            if (GetLoXf(a, mat, t)) sly3::PlaceLo3(Plo(a), mat, PosSafeGround(pa, e, 0.0f) - glm::vec3(pa->vecBase));
            return;
        }
        RecoverActorTo(pa, e);   // (0x1C0538 with the entry)
        if (b4 == 1) pa->jump.f28 = 1;   // (actor +0x348 = 0: the jump's air count, a double jump allowed)
        if (SLY* psly = dynamic_cast<SLY*>(pa)) psly->n18F0 = b8;   // (cid 0x34: +0x19F0)
    }
    // 0x30's done test (the actor walks toward the point until within the radius): with no actor locomotion (the player code's
    // pfnActorWalkToward false) the actor is put at the point (interim, so the attract's walks end)
    void SetActorWatchTarget(void* a, const float pos[3], float gSpeed, bool fR1) override
    {
        if (!a) return;
        if (g_scpthooks3.pfnActorWalkToward && g_scpthooks3.pfnActorWalkToward(Plo(a), pos, gSpeed, fR1)) return;
        float mat[9], t[3];
        if (!GetLoXf(a, mat, t)) return;
        // (the target point dropped onto what is under it (100 up .. 350 down, as 0x198D78's script drop): a walk up to a
        // higher deck arrives on it (Ep5's ship 44092: SCPT 4006's walk to 44194 on the poop deck, 150 above where Sly
        // stood); else the actor's own height as before)
        glm::vec3 p(pos[0], pos[1], pos[2]);
        if (!FDropOnGround(&p, 100.0f, -350.0f, true)) p.z = t[2];
        sly3::PlaceLo3(Plo(a), mat, p);
    }
    // SCPT 0x30 on an interaction's object (a LEDGE, WALL, PIPE, RESPONSE, HIDE, cid 0x24: orig3 0x226D00 case 48 ->
    // 0x1B5170(actor +0x500), Sly 2's FInteractCircle 0x1938C0: the first tracked target within 250 starts its action).
    // E.g. 4277 [15]: Sly walks to the ledge 15188 and hangs from it, then [16] shimmies along it to 15189.
    void ResetActorInteract(void* a) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        const bool f = pa && FInteractCircle(pa);
        if (FPrint()) std::printf("scpt3: frame %d 0x30 interaction of %d: %s\n", s_cframe, Plo(a) ? Plo(a)->oid : -1, f ? "started" : "none in reach");
    }
    // SCPT 0x26 (orig3 0x226D00 case 38: a climbing actor (+0x1014, Sly 2's +0xF54) along its pipe (+0x101C) to the point
    // nearest an object (0x22EA88 into the run's +0x84); done within the tolerance of that point limited to the climb's
    // range (0x1BF128 / 0x11EE68) at the actor's place +0x1020 (Sly 2's +0xF60); the drive 0x22A7F8 case 38 (Sly 2's 0x1A:
    // the stick along the pipe's tangent toward it, full deflection). E.g. m_ext's attract 4262 [43]: Sly up the pipe 15087
    // to 15088 (70 vblanks in the reference; natively it ended at once and Sly leapt off from the pipe's foot).
    bool FActorOnPath(void* a) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        return pa && pa->nSubF54 != -1 && pa->pcrvF50 && pa->ploF5C;
    }
    void* PcrvActorPath(void* a) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        return pa ? pa->ploF5C : nullptr;   // (the pipe object: NearestOnCurve takes its curve)
    }
    float UActorPath(void* a) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        return pa ? pa->sCrvF50 : 0.0f;
    }
    float GLimitActorPath(void* a, float u) override
    {
        ACTOR* pa = PactorFromLo(Plo(a));
        if (!pa || !pa->pcrvF50) return u;
        const cmm::LM lm = LmClimb(pa);   // 0x1BF128
        const float g = u < lm.gMin ? lm.gMin : (lm.gMax < u ? lm.gMax : u);   // 0x11EE68
        sly3::ActorClimbToward3(pa, u);   // (the done test's every frame: the drive's goal while it is polled)
        return g;
    }
    // NearestCrvAll 0x22EA88 on a curve object (cid 0x09: a path, a pipe, a rail): the nearest point, its tangent and
    // distance along it
    void NearestOnCurve(void* pcrv, const float posFrom[3], float pos[3], float dir[3], float* pu) override
    {
        const CRV* c = Plo(pcrv) ? PcrvFromLo(Plo(pcrv)) : nullptr;
        if (!c) return SCPTHOST3::NearestOnCurve(pcrv, posFrom, pos, dir, pu);
        glm::vec3 p(0.0f), d(1.0f, 0.0f, 0.0f);
        float s = 0.0f;
        NearestCrvAll(c, glm::vec3(posFrom[0], posFrom[1], posFrom[2]), &p, &d, nullptr, &s);
        for (int i = 0; i < 3; ++i) pos[i] = p[i], dir[i] = d[i];
        if (pu) *pu = s;
    }
    // (actor +0x33C: the jump's kind, -1 when its leap is over)
    int ActorMoveState(void* a) override
    {
        if (g_scpthooks3.pfnActorMoveState) return g_scpthooks3.pfnActorMoveState(Plo(a));
        ACTOR* pa = PactorFromLo(Plo(a));
        // (an actor ActorMoveTo placed at the goal (not the current player, or no native jump): its move is over; its
        // jump kind may be a stale one from before another character was played (ep1_heist_end's SCPT 2489 [80]: Sly
        // while Bentley is played, the run waited at [85] until the task was forced))
        if (pa && (pa->jump.pactor != pa || !s_psw || pa->palo != PloPoCurrent(s_psw))) return -1;
        return pa ? pa->jump.kind : -1;
    }
    void FaceObject(void* a, void* plo) override { if (g_scpthooks3.pfnFaceObject) g_scpthooks3.pfnFaceObject(Plo(a), Plo(plo)); }
    float ActorHeading(void* a) override
    {
        const STEP* p = PstepFromLo(Plo(a));
        return p ? p->radHeading : 0.0f;
    }
    // the world transform of an object (rows, position)
    // (orig3 0x22AEC8 takes the target's vt slot 36 matrix: a plain ALO's +0x50 record (0x23CFB8), here apxfRef[2],
    // which carries its rotation record: m_ext's land beast 15151, placed on 15468 by SCPT 4318 [49], took 15468's
    // position record's heading 0 instead of its rotation record's -53 degrees and stood turned 53 degrees in its
    // intro (the closeup on Dr. M then framed its foot). Actors keep their +0x48 record (MatLoWorld3, s3-player's).)
    bool GetLoAimXf(void* plo, float mat[9], float pos[3]) override
    {
        LO* p = Plo(plo);
        if (p && FCidDerivedFrom(p->Cid(), S3CID(0x30)) && !PactorFromLo(p))
            if (const XF* pxf = static_cast<ALO*>(p)->apxfRef[2])
            {
                const glm::mat4& m = pxf->matWorld;
                if (static const bool s_f = std::getenv("SLY3_AIMPRINT") != nullptr; s_f)  // (debug)
                {
                    const glm::mat4 m0 = sly3::MatLoWorld3(p);
                    std::printf("aimxf %d: slot36 %.1f %.1f %.1f x %.2f %.2f / +0x48 %.1f %.1f %.1f x %.2f %.2f\n", p->oid, m[3].x, m[3].y, m[3].z,
                                m[0].x, m[0].y, m0[3].x, m0[3].y, m0[3].z, m0[0].x, m0[0].y);
                }
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) mat[r * 3 + c] = m[r][c];
                for (int i = 0; i < 3; ++i) pos[i] = m[3][i];
                return true;
            }
        return GetLoXf(plo, mat, pos);
    }
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
        if (FPrint()) std::printf("scpt3: release proxy %d instance %d\n", Plo(pproxy)->oid, Plo(plo)->oid);
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
        if (FPrint()) std::printf("scpt3: frame %d %s %d (found %d, cid 0x%X)\n", s_cframe, fAdd ? "add" : "remove", oid, plo ? plo->oid : -1, plo ? plo->Cid() : -1);
        if (!plo || !s_psw) return nullptr;
        SwitchLoFaded(plo, fAdd, g);
        // (transition.cpp's world start and the binocucom's render-target release keep them: FLoAddedByScript3; a script's
        // removal ends that)
        if (fAdd) s_setploAddedScript.insert(plo);
        else s_setploAddedScript.erase(plo);
        return plo;
    }
    // 0x38 / 0x39's done test (0x229438 cases 56 / 57): an add ends when the object is in the world (flag bit 56)
    // with its fade at 1 (+0x104), a removal when it has left the world (the fade out removes it at 0)
    bool FAddRemoveDone(void* pv, bool fAdd) override
    {
        const LO* plo = Plo(pv);
        if (!plo) return true;
        if (!fAdd) return plo->fRemoved;
        if (static const bool s_f = std::getenv("SLY3_CLOCKPRINT") != nullptr; s_f && FCidDerivedFrom(plo->Cid(), S3CID(0x30)))   // (debug)
            std::printf("fade %d: %.6f rate %.4f\n", plo->oid, static_cast<const ALO*>(plo)->gFade, static_cast<const ALO*>(plo)->dgFade);
        return !plo->fRemoved && (!FCidDerivedFrom(plo->Cid(), S3CID(0x30)) || static_cast<const ALO*>(plo)->gFade == 1.0f);
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
    // 0x13 (orig3 0x1F4B00 -> 0x1F4B38): player n's character set to the object (cid 0x31): the old one's / new one's
    // controller slot (vt slot 83: -1 / the player's +0x1040), the camera / HUD refresh (0x168568, 0x1B67C0), the
    // checkpoint key for an actor (0x1856E0). Natively the current player (Sly 2's SetPoCurrent 0x1C4940) for player 0.
    // 0x12 (orig3 0x2271C8): an object activated by its class. 0x50: the code panel entered by the run's actor
    // (0x1716F8, s3-player's codepanel3.cpp); the other classes below (0x47 logged).
    void ActivateLo(int cid, void* pv, void* ploActor) override
    {
        LO* plo = Plo(pv);
        if (FPrint()) std::printf("scpt3: frame %d activate %d (cid 0x%X)\n", s_cframe, plo ? plo->oid : -1, cid);
        switch (cid)
        {
        case 0x50: sly3::ActivateCodePanel3(plo, Plo(ploActor)); return;
        case 0x15:  // the trigger component's vt slot 2 (0x15BF28): its enter without a state change
            if (!sly3::FActivateTrigger3(plo)) HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
            return;
        case 0x27: TriggerWarp(plo); return;                 // 0x27FA30 -> 0x27FA70 (Sly 2's TriggerWarp 0x228170)
        case 0x28: ApplyCheckpointLo(s_psw, plo); return;     // 0x280010 (Sly 2's 0x228750)
        case 0x18: TriggerExit(plo, -1); return;             // 0x27FCC8
        case 0x8A: StartFadeLo(plo); return;                 // 0x17D7D0
        case 0x4D: ActivateHackDock(plo); return;            // 0x19E1F8: the dock's entries on, its message 5
        case 0x71:  // 0x2775B8: the carried gadget's vt slot 80 with 5 for its kind 10 (Sly 3's +0x13C), else 3
            if (const CARRY* pc = PcarryFromLo(plo)) SetCarryState(plo, pc->kind == 10 ? 5 : 3);
            return;
        case 0x21:  // the use's vt slot 55 (0x1A20F8) with the run's actor as the user (Sly 2's UseObjectBase)
            if (ACTOR* pa = PactorFromLo(Plo(ploActor))) UseObjectBy(plo, pa);
            return;
        case 0x5D:  // the pad's trigger component (+0x130, 0x15BDF8) vt slot 2 = 0x15BF28: the pad's message 5, then the
                    // component's enter lists (0x15BFF8(1); natively the pad's embedded lists, trigger.cpp) without a state change
            HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
            RunEmbeddedTriggerList(plo, 0, Plo(ploActor));
            return;
        case 0x47:  // 0x1E8E40: +0x190 = the actor, 0x1E8E60(0): the SO takes player n's pad from the actor (0x1F4B00 with
                    // the actor's +0x130) until X / Triangle (its update 0x1E8D80) gives it back. Only in v_apt (5) /
                    // h_hangar_b (18), and no script of the surveys activates one: not ported natively, logged
            std::fprintf(stderr, "scpt3: activate %d of cid 0x%X not ported\n", plo ? plo->oid : -1, cid);
            return;
        default:   // 0x13BC80: the object's vt slot 3 with message 5 (its splice handler, then the broadcast 0x1D)
            HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
            return;
        }
    }
    void JobCommand(void* pv, int n) override
    {
        LO* plo = Plo(pv);
        if (FPrint()) std::printf("scpt3: frame %d player %d's character -> %d\n", s_cframe, n, plo ? plo->oid : -1);
        if (n == 0) SetPlayer0Character3(s_psw, plo);
    }
    bool FPlayerCharacter(void* pv) override { return s_psw && pv && PloPoCurrent(s_psw) == Plo(pv); }
    // 0x6C's choice menu (s3-ui's hooks; interim: the first line chosen after 1 s)
    int idChoiceAuto = -1, cframeChoice = 0;
    bool FChoiceMenu() override { return true; }
    void ShowChoices(int idTitle, const std::vector<int>& aid, const std::vector<int>& agrf) override
    {
        if (FPrint())
        {
            std::printf("scpt3: frame %d choices (title %d):", s_cframe, idTitle);
            for (int id : aid) std::printf(" %d", id);
            std::printf("\n");
        }
        if (g_scpthooks3.pfnShowChoices) { g_scpthooks3.pfnShowChoices(idTitle, int(aid.size()), aid.data(), agrf.data()); return; }
        idChoiceAuto = aid.empty() ? -1 : aid.front();
        cframeChoice = s_cframe;
    }
    int ChoiceState(int* pid) override
    {
        if (g_scpthooks3.pfnChoiceState) return g_scpthooks3.pfnChoiceState(pid);
        *pid = idChoiceAuto;
        return s_cframe - cframeChoice < 60 ? 1 : 0;
    }
    void HideChoices() override
    {
        if (g_scpthooks3.pfnHideChoices) g_scpthooks3.pfnHideChoices();
        idChoiceAuto = -1;
    }
    void SetLoParent3(void* pv, void* pvParent, int grf) override
    {
        LO* plo = static_cast<LO*>(pv);
        ALO* paloParent = static_cast<ALO*>(static_cast<LO*>(pvParent));
        if (FPrint()) std::printf("scpt3: parent %d (cid 0x%X) at %.0f %.0f %.0f -> %d (cid 0x%X) at %.0f %.0f %.0f grf 0x%X\n", plo->oid, plo->Cid(), PosLoWorld(plo).x, PosLoWorld(plo).y, PosLoWorld(plo).z,
                                paloParent ? paloParent->oid : -1, paloParent ? paloParent->Cid() : 0, paloParent ? PosLoWorld(paloParent).x : 0.0f, paloParent ? PosLoWorld(paloParent).y : 0.0f,
                                paloParent ? PosLoWorld(paloParent).z : 0.0f, grf);
        if (FCidDerivedFrom(plo->Cid(), 0x2F)) SetAloParent(static_cast<ALO*>(plo), paloParent, nullptr, grf);
        else SetLoParentActivation(g_psw, plo, paloParent);
    }
    void ShowAloGlobs3(void* pv, int k, bool fShow) override { ShowAloGlobs(static_cast<ALO*>(static_cast<LO*>(pv)), k, fShow); }
    void SetAloSoes3(void* pv, int k, bool fOn) override { SetAloSoesByTag(static_cast<ALO*>(static_cast<LO*>(pv)), k, fOn); }
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
    void ActorPathStep(void* pa, void* pcrv, int b1, int b2, float g3, int f104) override
    { if (g_scpthooks3.pfnActorPathStep) g_scpthooks3.pfnActorPathStep(Plo(pa), Plo(pcrv), b1, b2, g3, f104); }
    void ActorStepTo(void* pa, const float pos[3], float g1, int b2, float g3, int f104) override
    { if (g_scpthooks3.pfnActorStepTo) g_scpthooks3.pfnActorStepTo(Plo(pa), pos, g1, b2, g3, f104); }
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
        // orig3 0x226D00 case 25: the runner listens to the SMA (0x25D478 registers it; 0x13BD88 removes it) and latches
        // (+0x64) when the SMA enters the goal (its state message), or at once when it already stands there (0x239E70)
        SmaLatch(psma, oidGoal);
        return psma;
    }
    // (on by default since round 21; SLY3_SMALATCH=0 turns it off: the interim test of round 20)
    static bool FSmaLatchOn() { const char* sz = std::getenv("SLY3_SMALATCH"); return !sz || std::atoi(sz) != 0; }
    // orig3 0x229438 case 25: done when the latch is set or the SMA is gone (0x239CC0). (s3-ai round 20: natively the
    // goal's being set counts as reached: the run goes on at once; m_ext's RC chopper scene (SCPT 4277 [30] / [40] /
    // [49]) does not wait for the chopper's hooks, the reference waits 544 / 144 frames. Round 21: the latch is
    // the default (the Ep3 dogfight ep3_job3259 that lost 3295 with it in round 20 keeps it now, after s3-plane's
    // round 5))
    bool FSmGoalReached(void* pv, int oidGoal) override
    {
        const SMA* psma = static_cast<const SMA*>(pv);
        auto it = s_mplatch.find(psma);
        if (std::find(g_apsma.begin(), g_apsma.end(), psma) == g_apsma.end())   // (freed: its listener list went with it)
        {
            if (it != s_mplatch.end()) s_mplatch.erase(it);
            return true;
        }
        if (FSmaRetracted(psma)) return true;
        static const bool s_fLatch = FSmaLatchOn();
        if (!s_fLatch) return OidSmaGoal(psma) == oidGoal;   // (the interim test: the goal set)
        if (it == s_mplatch.end()) return OidSmaCur(psma) == oidGoal;
        if (!it->second->fLatched || it->second->oidGoal != oidGoal) return false;
        SMA* p = const_cast<SMA*>(psma);
        p->aplistener.erase(std::remove(p->aplistener.begin(), p->aplistener.end(), it->second.get()), p->aplistener.end());
        s_mplatch.erase(it);
        return true;
    }
    struct SMALATCH : SMALISTENER
    {
        int oidGoal = -1;
        bool fLatched = false;
        void OnSmaState(const SMAMSG10& msg) override
        {
            if (msg.oidNew == oidGoal) fLatched = true;
            if (static const bool s_f = std::getenv("SLY3_SMALATCHPRINT") != nullptr; s_f)   // (debug)
                std::printf("smalatch: sma %d (sm %d) %d -> %d goal %d%s\n", msg.psma && msg.psma->palo ? msg.psma->palo->oid : -1,
                            msg.psma && msg.psma->psm ? msg.psma->psm->oid : -1, msg.oidOld, msg.oidNew, oidGoal, fLatched ? " latched" : "");
        }
    };
    std::unordered_map<const SMA*, std::unique_ptr<SMALATCH>> s_mplatch;
    void SmaLatch(SMA* psma, int oidGoal)
    {
        if (static const bool s_fLatch = FSmaLatchOn(); !s_fLatch) return;   // (SLY3_SMALATCH=0: off)
        auto& up = s_mplatch[psma];
        if (!up) up = std::make_unique<SMALATCH>();
        // (an entry left by a freed SMA whose address a new one reuses: listen anew)
        if (std::find(psma->aplistener.begin(), psma->aplistener.end(), up.get()) == psma->aplistener.end())
            psma->aplistener.push_back(up.get());
        up->oidGoal = oidGoal;
        up->fLatched = OidSmaCur(psma) == oidGoal;
        if (static const bool s_f = std::getenv("SLY3_SMALATCHPRINT") != nullptr; s_f)
            std::printf("smalatch: frame %d sma %d (sm %d) goal %d from %d%s\n", s_cframe, psma->palo ? psma->palo->oid : -1,
                        psma->psm ? psma->psm->oid : -1, oidGoal, OidSmaCur(psma), up->fLatched ? " latched" : "");
    }
    // (the patterns are Sly 2's table 0x2C2260 natively; Sly 3's is 0x348068: TODO(sly3) compare the two)
    void Rumble(int, int k, float g) override { TriggerJoyRumbleRumk(k, g, "scpt3 0x40"); }
    // (the native contact offer, Sly 2's 0x1F7598 = Sly 3's 0x240A28 restructured: the receiver and its parents).
    // orig3 0x241100 -> 0x240CB0(sender, 0, receiver, kind, amount, kill): the amount (A2) is the message's +0x24 through
    // 0x172268 (0x240C78: scaled by the suck for a player's object: the receivers' handlers that need it scale it, e.g.
    // hack.cpp FHackSoHit), the instant kill (A3) its +0x28 (0x19B6E0: the hit points to 0). Natively the kill flag was
    // passed as the amount (a script's damage of 0 or 1 instead of its amount)
    void SendContact3(void* pto, void* pfrom, int grf, int nAmount, int fKill) override
    {
        CTMSG ctm;
        ctm.ploSender = Plo(pfrom);
        ctm.ploReceiver = Plo(pto);
        ctm.grfFilter = uint32_t(grf);
        ctm.nState = nAmount;
        ctm.fKill3 = fKill;
        ctm.pos = sly3::PosLoWorld3(Plo(pfrom));
        if (FPrint()) std::printf("scpt3: frame %d contact %d -> %d filter 0x%X amount %d kill %d\n", s_cframe, Plo(pfrom)->oid, Plo(pto)->oid, grf, nAmount, fKill);
        PaloAcceptContact(&ctm);
    }
    void SetLoField3(void* pv, int ib, int n) override { Plo(pv)->SetField(0, uint32_t(ib), uint32_t(n)); }
    void SetAloCnstr3(void* pv, int nLin, int nAng, bool fStop) override
    {
        ALO* palo = static_cast<ALO*>(Plo(pv));
        if (FPrint()) std::printf("scpt3: frame %d cnstr %d lin %d ang %d stop %d\n", s_cframe, palo ? palo->oid : -1, nLin, nAng, fStop);
        if (nLin != -1) SetAloCnstrLin(palo, nLin);
        if (nAng != -1) SetAloCnstrAng(palo, nAng);
        if (fStop)
        {
            SetAloVelocityTo(palo, glm::vec3(0.0f));
            SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        }
    }
    void ShowNotice(int n, void* plo) override { if (g_scpthooks3.pfnShowNotice) g_scpthooks3.pfnShowNotice(n, Plo(plo)); }
    bool FNoticeBusy() override { return g_scpthooks3.pfnFNoticeBusy && g_scpthooks3.pfnFNoticeBusy(); }
    void RemoveByClass(int cid) override
    {
        if (!s_psw || cid < 0) return;
        std::vector<LO*> a;
        for (const auto& up : s_psw->aploAll)
            if (up->fInWorld && FCidDerivedFrom(up->Cid(), cid)) a.push_back(up.get());
        if (FPrint()) std::printf("scpt3: frame %d class 0x%X removed: %zu objects\n", s_cframe, cid, a.size());
        for (LO* plo : a) RemoveLo(s_psw, plo);
    }
    void ActorCmd(int id, void* pa, void* ploA, void* ploB, int n, int n2, const float pos[3]) override
    {
        if (g_scpthooks3.pfnActorCmd) { g_scpthooks3.pfnActorCmd(id, Plo(pa), Plo(ploA), Plo(ploB), n, n2, pos); return; }
        static std::set<int> s_setid;   // (noted once per command: the actor's side is s3-player's)
        if (s_setid.insert(id).second) std::fprintf(stderr, "scpt3: actor command 0x%02X: no actor hook yet (pfnActorCmd)\n", id);
    }
    bool FActorCmdDone(int id, void* pa) override { return !g_scpthooks3.pfnFActorCmdDone || g_scpthooks3.pfnFActorCmdDone(id, Plo(pa)); }
    // 0x69 (orig3 0x1528E0 for an actor: outside m_ext's prologue the coin counter (0x1853D8), the sound 0x2A and the
    // camera's marker reveal 0x168670(cm, 0): SW +0x4F48 = t, the markers shown for 12 s (0x282750) as L3 shows them;
    // e.g. v_ext's 2084 [6] as Sly leaves the safe house: the yellow arrow (marker 51011, kind -1) and the coins)
    void Caught(void* plo) override
    {
        if (g_scpthooks3.pfnCaught && plo) { g_scpthooks3.pfnCaught(Plo(plo)); return; }
        if (!plo || std::getenv("SLY3_NOREVEAL69")) return;
        if (FPrint()) std::printf("scpt3: frame %d 0x69: the markers revealed (%d)\n", s_cframe, Plo(plo)->oid);
        ShowWorldStats();
    }
    void SetActorHold5D(void* pa, bool fHold) override { if (ACTOR* p = PactorFromLo(Plo(pa))) sly3::SetActorHold5D3(p, fHold); }
    // (0x5D / 0x5E's done tests: Bentley's +0x1520, his state in Sly 3's numbering: gang.cpp's BS3 states are those)
    int ActorState36(void* pa) override
    {
        if (g_scpthooks3.pfnActorState36 && pa) return g_scpthooks3.pfnActorState36(Plo(pa));
        if (BENTLEY* p = pa ? PbentleyFromLo(Plo(pa)) : nullptr; p && sly3::FHold5D3()) return p->state;
        return -1;
    }
    void GuardCommand63(void* pg, void* plo) override
    { if (g_scpthooks3.pfnGuardCommand63 && pg) g_scpthooks3.pfnGuardCommand63(Plo(pg), Plo(plo)); }
    // 0x42 / 0x55 / 0x56 / 0x29: the copied Sly 2 code's bodies (soundglue.cpp, binoc.cpp, interact.cpp)
    void SetEyesClosed(int oid, int n, float g, void* ploContext) override { ::SetEyesClosedOf(Plo(ploContext), oid, n, g); }
    void HoldActorEffects(void* pa, bool fHold, bool f, int grf) override { ::HoldActorEffects(Plo(pa), fHold, f, uint32_t(grf)); }
    // orig3 0x156468 (the run that drives an actor, actor +0x115C): a run let go while the actor's interaction (+0x508,
    // Sly 2's +0x4C8) is of kind 5 / 6 (0x29's scripted use) resets it (vt slot 99 (0): Sly 2's slot 88 (0)), e.g. m_ext's
    // car intro SCPT 4416 [77] retires the runner 15911 that left Bentley at his console (0x29 15913): his walk [78] then
    // went nowhere (SLY3_RUNNOINTERACTRESET=1: as before)
    std::unordered_map<const ACTOR*, RUN3*> mpactorrun;
    void SetActorRun(void* ploActor, RUN3* prun) override
    {
        ACTOR* p = PactorFromLo(Plo(ploActor));
        if (!p) return;
        RUN3*& pold = mpactorrun[p];
        if (pold == prun) return;
        static const bool s_fOff = std::getenv("SLY3_RUNNOINTERACTRESET") != nullptr;
        if (pold && !s_fOff && (p->n4C8 == 5 || p->n4C8 == 6))
        {
            if (FPrint()) std::printf("scpt3: frame %d run released: %d's interaction %d reset\n", s_cframe, Plo(ploActor)->oid, p->n4C8);
            ::NotifyActorJumpKind(p, 0);
        }
        pold = prun;
    }
    void SetActorInteractObject(void* pa, void* plo) override
    {
        // (an actor whose class has no native init yet (no jump owner: e.g. Episode 4's m4_zombies actor, the Guru) has
        // no reach either: its interact start crashed in reach.cpp PloReachGrip)
        ACTOR* p = PactorFromLo(Plo(pa));
        if (p && p->jump.pactor) ::SetActorInteractObject(p, 5, Plo(plo));
    }
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
        if (run.pscpt) ++g_mpcScptCmdExec3[(int64_t(run.pscpt->oid) << 16) | icmd];   // (route.cpp's waitcmd)
        if (FPrint() && run.pscpt) std::printf("scpt3: frame %d exec %d [%d] %02X %s\n", s_cframe, run.pscpt->oid, icmd,
                                               run.pscpt->acmd[std::size_t(icmd)].id, SzScptCmd3(run.pscpt->acmd[std::size_t(icmd)].id));
        if (static const bool s_fArgs = std::getenv("SLY3_SCPTARGS") != nullptr; s_fArgs && FPrint() && run.pscpt)   // (debug: the arguments)
        {
            const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
            std::printf("scpt3:   oid %d flag %d args", c.oid, int(c.fFlag));
            for (int32_t n : c.an) std::printf(" %d", n);
            std::printf("\n");
        }
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

// ---- the TASK 0xA9 / MISSION 0xA8 splice methods and getters (the Sly 3 EOPID table 0x32CA80: the copied Sly 2
// registrations reach Sly 2's TASK shells, not task3's records). Bodies from the Sly 3 decomp's thunks.
TASK3* PtaskOfLo3(LO* plo) { return plo ? PtaskFind3(plo->oid) : nullptr; }
JOB3* PjobOfLo3(LO* plo) { return plo ? PjobFind3(plo->oid) : nullptr; }
CRef RefS32(int n) { CRef r; r.SetS32(n); return r; }
CRef RefBool(bool f) { CRef r; r.SetBool(f); return r; }
CRef RefLo3(void* plo) { CRef r; if (plo) r.SetBasic(static_cast<LO*>(plo)); return r; }

// orig3: 0x186EE0 (the MISSION methods 1355..1358, thunks 0x10DB10 / 0x10DB98 / 0x10DC40 / 0x10DCE8 -> 0x186E80 /
// 0x186EA0 / 0x186EC0): the job failed with a line, a failing script found from the job (0x17E1B8 kind 4) and a
// second value (+0xCC: unused natively)
void JobFailMethod3(LO* plo, int idText, int oidScpt)
{
    JOB3* pj = PjobOfLo3(plo);
    if (!pj) return;
    if (FPrint()) std::printf("scpt3: frame %d job %d fails (line %d, script %d)\n", s_cframe, pj->oid, idText, oidScpt);
    LO* pscpt = oidScpt != -1 ? static_cast<LO*>(s_host.PloFind(4, oidScpt, plo)) : nullptr;
    JobEndFail3(pj, idText, pscpt);
}

void RegisterTaskMethods3()
{
    // the world's option 984 (EOPID 986, thunk 0x10AA80 -> 0x1F4AD0(SW, n)): player n's character (n 0: the current
    // player; 1: the second player's, GAME +0x14); the scripts of 69 survey runs asked for it (Sly 2's option 847)
    RegisterObjectMethod3(0x10AA80, [](LO*, const std::vector<CRef>& a) {
        CRef r;
        const int n = NArg(a, 0, 0);
        LO* plo = !s_psw ? nullptr : n == 0 ? PloPoCurrent(s_psw)
                : n == 1 && g_gametasks3.aoidPlayer[1] != -1 ? PloFindSwObjectByOid(s_psw, g_gametasks3.aoidPlayer[1]) : nullptr;
        if (plo) r.SetBasic(plo);
        return r;
    });
    // TASK: 1409 RequestTaskState (0x10E090 -> 0x189078), 1410 ForceTaskState (0x10E138 -> 0x1890B0): (n, context)
    RegisterObjectMethod3(0x10E090, [](LO* plo, const std::vector<CRef>& a) {
        TaskRequestMethod3(PtaskOfLo3(plo), 0, NArg(a, 0, -1), PloArg(a, 1));
        return CRef(TAGK_Void);
    });
    RegisterObjectMethod3(0x10E138, [](LO* plo, const std::vector<CRef>& a) {
        TaskRequestMethod3(PtaskOfLo3(plo), 1, NArg(a, 0, -1), PloArg(a, 1));
        return CRef(TAGK_Void);
    });
    // 1413 (0x10E1E0 -> 0x189DA0): a live task's message 0x1A with the value
    RegisterObjectMethod3(0x10E1E0, [](LO* plo, const std::vector<CRef>& a) {
        TASK3* p = PtaskOfLo3(plo);
        if (p && p->f40 == 0 && p->nMode != 3)
        {
            CRef r = RefS32(NArg(a, 0));
            HandleLoSpliceEvent(plo, 0x1A, false, 1, &r);
        }
        return CRef(TAGK_Void);
    });
    // 1418..1421 (0x10E460 / 0x10E4D8 / 0x10E550 / 0x10E5C8 -> 0x189710..): the state 0..3 forced in mode 4
    RegisterObjectMethod3(0x10E460, [](LO* plo, const std::vector<CRef>&) { TaskForceStateMethod3(PtaskOfLo3(plo), 0); return CRef(TAGK_Void); });
    RegisterObjectMethod3(0x10E4D8, [](LO* plo, const std::vector<CRef>&) { TaskForceStateMethod3(PtaskOfLo3(plo), 1); return CRef(TAGK_Void); });
    RegisterObjectMethod3(0x10E550, [](LO* plo, const std::vector<CRef>&) { TaskForceStateMethod3(PtaskOfLo3(plo), 2); return CRef(TAGK_Void); });
    RegisterObjectMethod3(0x10E5C8, [](LO* plo, const std::vector<CRef>&) { TaskForceStateMethod3(PtaskOfLo3(plo), 3); return CRef(TAGK_Void); });
    // 1422 (0x10E640 -> 0x188540 ActivateTask)
    RegisterObjectMethod3(0x10E640, [](LO* plo, const std::vector<CRef>&) { TaskActivateMethod3(PtaskOfLo3(plo)); return CRef(TAGK_Void); });
    // the getters: 1394 (+0x54 times activated), 1395 done (0x188A38), 1396 current (0x188A20), 1397 the goal state's
    // id (0x188AA0: 0x6E3 + state 0..3, else -1), 1411 / 1412 the change's mode / context (+0xFC / +0x100)
    RegisterObjectGetter3(1394, [](LO* plo) { const TASK3* p = PtaskOfLo3(plo); return RefS32(p ? p->cActivated : 0); });
    // a script's EOPID 750 (option 734, 0x226190: SCPT +0x60 bit 0, running): the missions' event handlers pass a
    // broadcast on to a running script's own handler (option 2429), e.g. m3_semifinals 3140 -> SCPT 3147 (the kills,
    // the respawn). Interim (s3-plane, for s3-script / s3-hack): only for the scripts with such a handler.
    // Checked (round 3): the getter is the decomp's (0x226190: +0x60 & 1, set / cleared by StartScpt 0x225EE8 / StopScpt
    // 0x226050 = fRunning) and the script's message 4 comes from the same two places as in the original (the executor's
    // command 0x15, 0x226D00, and an unknown condition kind, 0x226950's default: slot 3 with 4 and the kind). With every
    // script answered, Ep3 hack 4's generators (block 78434's SCPT 78440: [1] while [2] 0x15 -> its s4 spawns from
    // 2337 (78459 / 92975 / 92981) and re-arms alarm 79880 after a random 2339 time [3] waits for it) spawn as long as
    // they run, the hack's route (cases3.py ep3_job3164, hand-made for few enemies) is held at (2073, 529) and the job
    // fails (REGRESS at 3198). Natively the hack's generators start at the hack's activation (frame 1751, 4 copies; the
    // job's s4 condition 94440 / its 2431 path); the reference warped to 15:3172 had all six SCPT 78440 stopped (+0x60
    // 0, out of the world) 20 s into the hack. To do (s3-hack / s3-script): the generators' start, then this getter
    // for every script. SLY3_SCPT750ALL=1: every script.
    // (s3-hack round 7: the getter is right for every script. The reference's Ep3 hack 4 (warp 15:3196, RAM at +5 / +13 /
    // +23 s) keeps its four in-world generators running (SCPT 78440 +0x60 = 3), their chasers capped by the proxies'
    // pools (4 x 78461, 2..4 x 92977), as natively with every script answered (s3-plane's "stopped" was hack 1's warp, 15:3172); the survey's hack 4
    // route was made for one-spawn generators and fails (ep3_job3164 at 3198: redo the route, then drop this rule). Interim: every script in
    // the two-player hacking game (hackgame3.h: its rounds wait for its spawner groups' counts, block 92870), elsewhere
    // the scripts with a 2429 handler. SLY3_SCPT750ALL=1: every script everywhere.)
    RegisterObjectGetter3(750, [](LO* plo) {
        const SCPT3* p = PscptOfLo3(plo);
        static const bool s_fAll = std::getenv("SLY3_SCPT750ALL") != nullptr;
        const bool fHandler = s_fAll || hackgame3::FActive(s_psw) || (plo && plo->psidebag && plo->psidebag->FFindBinding(2429, nullptr));
        return RefBool(fHandler && p && FScptRunning3(p));
    });
    RegisterObjectGetter3(1395, [](LO* plo) { const TASK3* p = PtaskOfLo3(plo); return RefBool(p && FTaskDone3(p)); });
    RegisterObjectGetter3(1396, [](LO* plo) { const TASK3* p = PtaskOfLo3(plo); return RefBool(p && FIsTaskCurrent(p)); });
    RegisterObjectGetter3(1397, [](LO* plo) {
        const TASK3* p = PtaskOfLo3(plo);
        return RefS32(p && p->nState >= 0 && p->nState <= 3 ? 0x6E3 + p->nState : -1);
    });
    // 1391 (optid 1295, kind 0x20A8: a MISSION at +0x6C): the task's job. E.g. Ep3's windmill hacks: each hack task's
    // activation handler (block 93165, event 0x16) calls its job's 2431 (bound by the job's start handler) with the
    // hack's layout: the ship put at the hack's start point 93887 (method 39 = 0x100A00), its hit points refilled, the
    // layout added. Natively the getter was missing (the value read as an untyped option), so the chained hacks 2..4
    // started with the ship where the last one docked: the far dock took it at once
    RegisterObjectGetter3(1391, [](LO* plo) { const TASK3* p = PtaskOfLo3(plo); return RefLo3(p && p->pjob ? p->pjob->plo : nullptr); });
    RegisterObjectGetter3(1411, [](LO* plo) { const TASK3* p = PtaskOfLo3(plo); return RefS32(p ? p->nMode : 0); });
    RegisterObjectGetter3(1412, [](LO* plo) { const TASK3* p = PtaskOfLo3(plo); return RefLo3(p ? p->ploContext : nullptr); });
    // MISSION: 1355..1358 the job fails; 1359 (0x10DDA0 -> 0x187040: the job quit, back to the episode's start, below)
    RegisterObjectMethod3(0x10DB10, [](LO* plo, const std::vector<CRef>& a) { JobFailMethod3(plo, NArg(a, 0, -1), -1); return CRef(TAGK_Void); });
    RegisterObjectMethod3(0x10DB98, [](LO* plo, const std::vector<CRef>& a) { JobFailMethod3(plo, NArg(a, 0, -1), NArg(a, 1, -1)); return CRef(TAGK_Void); });
    RegisterObjectMethod3(0x10DC40, [](LO* plo, const std::vector<CRef>& a) { JobFailMethod3(plo, NArg(a, 0, -1), -1); return CRef(TAGK_Void); });
    RegisterObjectMethod3(0x10DCE8, [](LO* plo, const std::vector<CRef>& a) { JobFailMethod3(plo, NArg(a, 0, -1), NArg(a, 1, -1)); return CRef(TAGK_Void); });
    // 1359 (0x10DDA0 -> 0x187040): the job quit: not while its end runs (+0xB8); a challenge running (0x2852B0: the
    // manager 0x4BC230 +0x14, challenge3.h): its end 0x283F70 instead; no world change under way (0x25AE28): a job of kind 2 (+0x40) back to the world 0 (the
    // chooser) at 0x1A8 (0x289070), else the current episode started again (0x289198); then its end state 8
    RegisterObjectMethod3(0x10DDA0, [](LO* plo, const std::vector<CRef>&) {
        JOB3* pj = PjobOfLo3(plo);
        if (!pj || NJobEndState3(pj) != 0) return CRef(TAGK_Void);
        if (FPrint()) std::printf("scpt3: frame %d job %d quit (current job %d)\n", s_cframe, pj->oid, g_tasks3.pjobCur ? g_tasks3.pjobCur->oid : -1);
        if (!g_transition.fPending && s_psw && FChallengeRunning3()) EndChallenge3(s_psw);   // (0x283F70)
        else if (!g_transition.fPending && s_psw)
        {
            if (plo->U32Field(0, 0x40, 0) == 2) RequestWorldWarp(s_psw, 0, sly3::POINT_DEFAULT, -1);
            else StartEpisode3(s_psw, IepisodeCur());
        }
        JobEndQuit3(pj);
        return CRef(TAGK_Void);
    });
    // the getters: 1373 (+0x7C), 1374 its last task done (0x187CD0), 1375 current (0x187CB8), 1376 / 1377 mode /
    // context (+0xF0 / +0xF4)
    RegisterObjectGetter3(1373, [](LO* plo) { const JOB3* p = PjobOfLo3(plo); return RefS32(p ? p->f7c : 0); });
    RegisterObjectGetter3(1374, [](LO* plo) { return RefBool(FJobDone3(PjobOfLo3(plo))); });
    RegisterObjectGetter3(1375, [](LO* plo) { const JOB3* p = PjobOfLo3(plo); return RefBool(p && FIsJobCurrent(p)); });
    RegisterObjectGetter3(1376, [](LO* plo) { const JOB3* p = PjobOfLo3(plo); return RefS32(p ? p->nMode : 0); });
    RegisterObjectGetter3(1377, [](LO* plo) { const JOB3* p = PjobOfLo3(plo); return RefLo3(p ? p->ploContext : nullptr); });
    // USE (cid 0x21) EOPID 1162 (optid 555, Sly 3 only, kind 0x2002 at +0x11C, grf 0x92: loaded and read): an object;
    // m_ext's USE 15351 asks whether trigger 38243 holds its position (4306). From the loaded option, else the user:
    // the current player (TODO(sly3): the use's own user field once the USE's Sly 3 fields are native)
    RegisterObjectGetter3(1162, [](LO* plo) {
        for (const OPT& o : plo->aopt)
            if (o.eopid == 1162 && !o.aval.empty())
                if (LO* p = s_psw ? PloFindSwObjectByOid(s_psw, o.aval[0].n) : nullptr) return RefLo3(p);
        return RefLo3(s_psw ? PloPoCurrent(s_psw) : nullptr);
    });
    // the failing script's side of the job end (jobend3.h)
    g_jobendhooks3.pfnStartScpt = [](LO* pscpt, JOB3* pj) { StartScptOfLo3(pscpt, pj ? pj->plo : nullptr); };
    g_jobendhooks3.pfnFScptRunning = [](LO* pscpt) { SCPT3* ps = PscptOfLo3(pscpt); return ps && FScptRunning3(ps); };
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
        RegisterTaskMethods3();
        // each frame: the running scripts (the SCPT's vt slot 16 0x225130, before the objects' updates), the debug task
        // requests; after the camera's pass (0x14D478): the script camera
        RegisterSwPass(0x1F77C0, [](SW* psw) { if (s_fActive && psw == s_psw) UpdateScripts3Frame(); }, __FILE__);
        // orig3 0x2267C0 (the paused update: vt slot 16 of the real-clock entries): a script on the real clock (its
        // +0x3C 0x200: the clock factor 0 it set itself, 0x2263B0) runs on while the game is stopped, e.g. the prize /
        // loot hold-up's title card script 7333 (factor 0, a 0.2 s wait, card 7331 until it is done, factor 1). Natively
        // the scripts ran only in the running update, so such a script waited for ever and the game stayed stopped.
        RegisterSwPass(0x2267C0, [](SW* psw) {
            if (!s_fActive || psw != s_psw) return;
            for (const auto& ps : s_ascpt)
                if (ps->fRunning && (ps->grf & 0x200)) UpdateScpt3(ps.get(), g_clock.dtReal);
        }, __FILE__);
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
    UpdateDirectors3(g_clock.dt);   // (each DIRECTOR's vt slot 18 0x209FD0)
    // (debug) SLY3_LOINFO=frame:oid: the object (or the one whose instance name it is) with its parents, flags, position
    if (static const char* s_sz = std::getenv("SLY3_LOINFO"); s_sz && std::atoi(s_sz) == s_cframe && s_psw && std::strchr(s_sz, ':'))
    {
        const int oid = std::atoi(std::strchr(s_sz, ':') + 1);
        for (LO* plo : AploOfOid(s_psw, oid))
            for (const LO* p = plo; p; p = p->paloParent)
            {
                const glm::vec3 pos = sly3::PosLoWorld3(p);
                std::printf("loinfo %d: %d (instance %d) cid 0x%X in world %d removed %d pos %.0f %.0f %.0f\n", oid, p->oid,
                            p->oidInstance, p->Cid(), int(p->fInWorld), int(p->fRemoved), pos.x, pos.y, pos.z);
            }
    }
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
// Sly 3 EOPID 56 (optid 130, class ALO; thunk 0x1011C8 -> 0x241100 -> 0x240CB0(sender, 0, receiver, kind, amount, kill)):
// a contact from the 4th argument's object to this one, the scripts' (o 130) kind amount kill sender. E.g. the
// two-player hack's laser and bomb ((enemy 130) 256 0 #t laser): the enemy killed, its killer the laser. Before, the
// method had no body natively: the call did nothing (the laser's enemies sat on the beam)
struct INITCONTACTMETHOD3
{
    INITCONTACTMETHOD3()
    {
        RegisterObjectMethod3(0x1011C8, [](LO* plo, const std::vector<CRef>& a) {
            if (!plo) return CRef(TAGK_Void);
            LO* pfrom = PloArg(a, 3);
            CTMSG ctm;
            ctm.ploSender = pfrom;
            ctm.ploReceiver = plo;
            ctm.grfFilter = uint32_t(NArg(a, 0));
            ctm.nState = NArg(a, 1);
            ctm.fKill3 = a.size() > 2 && (a[2].FTrue() || (a[2].m_tagk == TAGK_S32 && a[2].m_n != 0)) ? 1 : 0;
            ctm.pos = sly3::PosLoWorld3(pfrom ? pfrom : plo);
            if (FPrint()) std::printf("scpt3: method 130 contact %d -> %d filter 0x%X amount %d kill %d\n", pfrom ? pfrom->oid : -1, plo->oid,
                                      ctm.grfFilter, ctm.nState, ctm.fKill3);
            PaloAcceptContact(&ctm);
            return CRef(TAGK_Void);
        });
    }
} s_initcontactmethod3;
}  // namespace

bool FLoAddedByScript3(const LO* plo) { return s_setploAddedScript.count(plo) != 0; }

void StartupScripts3(SW* psw)
{
    ResetScripts3();
    s_setploAddedScript.clear();
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
        // (debug: SLY3_SCPTDUMP=oid[,oid..] | all: the script's commands at the world start)
        if (static const char* s_sz = std::getenv("SLY3_SCPTDUMP"); s_sz && (!std::strcmp(s_sz, "all") || (std::string(",") + s_sz + ",").find("," + std::to_string(plo->oid) + ",") != std::string::npos))
            for (std::size_t i = 0; i < ps->acmd.size(); ++i)
            {
                const SCPTCMD3& c = ps->acmd[i];
                std::printf("scptdump %d [%zu]: id 0x%X oid %d flag %d child %d sib %d:", plo->oid, i, c.id, c.oid, int(c.fFlag), c.ichild, c.isib);
                for (int32_t n : c.an) std::printf(" %d", n);
                std::printf("\n");
            }
        // +0x3C: the option 0x200 (real time) (TODO(sly3): its EOPID; none of the prologue's scripts sets it)
        s_host.mpploscpt[plo] = ps.get();
        s_ascpt.push_back(std::move(ps));
    }
    SetScptHost3(&s_host);
    if (const char* sz = std::getenv("SLY3_SCPTCMP"); sz && !s_host.pfCmp) s_host.pfCmp = std::fopen(sz, "w");
    // the tasks (task3.h): their records, the host
    InitTasks3(psw);
    ResetDirectors3(psw);   // (the DIRECTORs' init 0x209EB0: D_00468D04)
    g_taskhost3.pfnTime = [] { return g_clock.t; };
    g_taskhost3.pfnAutosave = [] { HudAutoSave(false); };   // orig3 0x1CFE08 (hudwidget.cpp: Sly 3's rules; the world flow code)
    g_taskhost3.pfnMessage = [](TASK3* p, int msg) {
        if (p->plo) HandleLoSpliceEvent(p->plo, uint32_t(msg), false, 0, nullptr);
    };
    g_taskhost3.pfnMessageLo = [](LO* plo, int msg) { HandleLoSpliceEvent(plo, uint32_t(msg), false, 0, nullptr); };
    g_taskhost3.pfnMessageArgs = [](LO* plo, int msg, int n, LO* ploArg) {
        CRef a[2];
        a[0].SetS32(n);
        a[1].SetBasic(ploArg);
        HandleLoSpliceEvent(plo, uint32_t(msg), false, 2, a);
    };
    g_taskhost3.pfnStartScpt = [](LO* plo) {
        if (FPrint()) std::printf("scpt3: frame %d task starts SCPT %d\n", s_cframe, plo ? plo->oid : -1);
        // orig3: 0x226010: StartScpt 0x225EE8(script, player 0's character (0x1F4AD0(SW, 0))): the run's default actor
        // (SCPT 4307's 0x12 hands the code panel the actor that entered it)
        StartScptOfLo3(plo, s_psw ? PloPoCurrent(s_psw) : nullptr);
    };
    // the triggers (class 0x15: their SCPTs and task pairs, trigger3.h)
    sly3::StartTriggers3(psw);
    sly3::g_trighost3.pfnMessage = [](LO* plo, int msg) { HandleLoSpliceEvent(plo, uint32_t(msg), false, 0, nullptr); };
    sly3::g_trighost3.pfnStartScpt = [](LO* pscpt, LO* ploArg) { StartScptOfLo3(pscpt, ploArg); };
    sly3::g_trighost3.pfnSmGoal = [](int oid, int oidSm, int oidGoal, LO* plo) { s_host.RunSmGoal(oid, oidSm, oidGoal, false, plo); };
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
    g_gamepause.c84 -= GAMESCPTHOST3::s_cAiPause3;  // (the old world's scripts' AI pauses go with them)
    GAMESCPTHOST3::s_cAiPause3 = 0;
    for (int i = 0; i < 2; ++i) GAMESCPTHOST3::s_ac1180[i] = GAMESCPTHOST3::s_ac1188[i] = GAMESCPTHOST3::s_ac1190[i] = 0;
    s_ascpt.clear();
    s_host.mpploscpt.clear();
    s_host.asndRun.clear();
    s_mpwdialog.clear();
    s_host.mprunkey.clear();
    s_host.prunCamera = nullptr;
    s_host.radFovTargetSave = 0.0f;
    SetScptCameraActive3(false);   // (PC, skipcut.h: the old world's script camera is gone with it; s3-qol's hold would stay set)
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

bool FWorldDialogTable3() { return !s_mpwdialog.empty(); }

int IdWorldDialog3(int i, bool fSecond, int* pindex)
{
    if (pindex) *pindex = -1;
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
    if (pindex) *pindex = d.ilast;
    return std::size_t(d.ilast) < a.size() ? a[std::size_t(d.ilast)] : -1;
}

// orig3: 0x1A9478 && !0x1A9600 with an actor's player index (+0x130; -1 both players): the selected players' pads paused
// (+0x1180) without the kind-4 pause (+0x1190); natively the scripts' pauses (0x1F) and the native pause count
bool FPlayersPaused3() { return GAMESCPTHOST3::s_ac1180[0] > 0 || GAMESCPTHOST3::s_ac1180[1] > 0; }
bool FPlayerSeatPaused3(int seat) { return seat >= 0 && seat < 2 && GAMESCPTHOST3::s_ac1180[seat] > 0; }

bool FActorHitsPaused3(int sel)
{
    int c1180 = 0, c1190 = 0;
    if (sel != 1) { c1180 += GAMESCPTHOST3::s_ac1180[0]; c1190 += GAMESCPTHOST3::s_ac1190[0]; }
    if (sel != 0) { c1180 += GAMESCPTHOST3::s_ac1180[1]; c1190 += GAMESCPTHOST3::s_ac1190[1]; }
    return c1180 > 0 && c1190 <= 0;
}
