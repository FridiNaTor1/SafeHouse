// --scpt-run W OID[,OID...] [--frames N] [-v]: Sly 3's script executor (scptrun3.cpp) on a world's scripts with a
// test host: 60 frames a second; objects are their oids (the world walk's classes answer the class checks); the
// dialog lines last their world's lengths; the binocucom opens in 40 frames and changes speaker in 60 (as the
// reference's m_ext talk does), frames are NTSC fields (1001 / 60000 s); the title shows for SCPT3_TITLE_FRAMES frames (default 0); SCPT3_START_LAG (below). Each executed command prints as
// "frame exec oid N [index] id name", the done tests as they change ("done ... -> 0/1"), like tools/scpttrace3.py
// prints the reference's trace, so the two compare line for line (frames from the first script's start).

#include "scptrun3.h"

#include "brx3.h"
#include "ids3.h"

#include "../disc.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <deque>
#include <fstream>
#include <sstream>

bool FDialogLengthsOfWorld(const std::string& world, std::map<int, float>* pmp);  // sound.cpp

namespace
{
constexpr float c_dt = 1001.0f / 60000.0f;  // a frame: an NTSC field (the reference's vblanks; its 2.5 s waits take 150)

struct TESTHOST3 final : SCPTHOST3
{
    int frame = 0;
    bool fVerbose = false;
    int cframeTitle = 0;
    std::map<int, float> mpidtDialog;
    std::map<int32_t, int> mpoidcid;
    std::map<int32_t, SCPT3*> mpoidscpt;
    std::map<int32_t, int> mpoidstate;  // the tasks' states set by 0x1E (0x760 tests state 1; unknown: true)
    // dialog
    int idDialog = -1;
    float tDialogEnd = -1;
    // binocucom
    int binocState = -1, frameBinoc = 0, binocTarget = -1;
    int frameTitleEnd = -1;

    static void* Plo(int32_t oid) { return reinterpret_cast<void*>(intptr_t(0x10000000) + oid); }
    static int32_t Oid(void* plo) { return plo ? int32_t(reinterpret_cast<intptr_t>(plo) - intptr_t(0x10000000)) : -1; }

    float t = 0;  // accumulated as the game's clock (g_clock +0x0 += dt each frame)
    float T(bool) override { return t; }
    float GRandInRange(float a, float b) override { return 0.5f * (a + b); }
    void* PloFind(int, int oid, void*) override { return oid == -1 ? nullptr : Plo(oid); }
    bool FDerivedFrom(void* plo, int cid) override
    {
        auto it = mpoidcid.find(Oid(plo));
        return it == mpoidcid.end() || FCid3DerivedFrom(it->second, static_cast<CID3>(cid));
    }
    SCPT3* PscptFromLo(void* plo) override
    {
        auto it = mpoidscpt.find(Oid(plo));
        return it == mpoidscpt.end() ? nullptr : it->second;
    }
    bool FObjectCondition(int kind, void* plo) override
    {
        auto it = mpoidstate.find(Oid(plo));
        const bool f = kind == 0x760 ? it == mpoidstate.end() || it->second == 1 : false;
        Log("cond %X oid %d -> %d", kind, Oid(plo), f);
        return f;
    }
    std::map<int, bool> mpcond;  // SCPT3_CONDS=KIND=0|1,...: the script event handler's answers (the splice's; default 0)
    bool FScriptCondition(SCPT3* ps, int kind, int param) override
    {
        auto it = mpcond.find(kind);
        const bool f = it != mpcond.end() && it->second;
        Log("cond %d (the script %d's event handler) param %d -> %d", kind, ps->oid, param, f);
        return f;
    }
    void ScriptEvent(SCPT3* ps, int n) override { Log("event: script %d message 4 (%d)", ps->oid, n); }
    void ScriptStarted(SCPT3* ps) override { Log("script %d started", ps->oid); }
    void ScriptStopped(SCPT3* ps) override { Log("script %d stopped", ps->oid); }
    bool StartDialog(const DIALOGSTART3& d, void* ploSource) override
    {
        idDialog = d.aid[0];
        auto it = mpidtDialog.find(idDialog);
        // (+6 frames: the reference's stream ends that much after the command plus the line's length: m_ext's talk)
        tDialogEnd = T(false) + (it == mpidtDialog.end() ? 3.0f : it->second) + 6.0f * c_dt;
        Log("dialog %d (%.2f s) source %d", idDialog, tDialogEnd - T(false) - 6.0f * c_dt, Oid(ploSource));
        return true;
    }
    bool FDialogPlaying(int id) override { return T(false) < tDialogEnd && (id == -1 || id == idDialog); }
    void StopDialog() override { tDialogEnd = -1; Log("dialog stopped"); }
    void ResetDialogSubtitle() override { Log("subtitle reset"); }
    void OpenBinoc(void* pa, const BINOCOPEN3& b, void* ploSpeaker, void*, void*, void*) override
    {
        binocState = 0;
        binocTarget = 1;
        frameBinoc = frame + 40;
        Log("binoc open: actor %d speaker %d eye (%.1f %.1f %.1f)", Oid(pa), Oid(ploSpeaker), b.posEye[0], b.posEye[1], b.posEye[2]);
    }
    void CloseBinoc(void* pa) override
    {
        binocState = 2;
        binocTarget = -1;
        frameBinoc = frame + 20;
        Log("binoc close: actor %d", Oid(pa));
    }
    int BinocState(void*) override { return binocState; }
    void SetBinocSpeaker(void*, void* plo) override
    {
        binocState = 3;
        binocTarget = 1;
        frameBinoc = frame + 60;
        Log("binoc speaker %d", Oid(plo));
    }
    void CmFocus(void*, int id, void* plo) override { Log("binoc camera %02X focus %d", id, Oid(plo)); }
    void CmTargets(void*, const float pos[3], const float dir[3], float g) override
    {
        Log("binoc camera targets (%.1f %.1f %.1f) (%.2f %.2f %.2f) %.2f", pos[0], pos[1], pos[2], dir[0], dir[1], dir[2], g);
    }
    void CutCm() override { Log("cut camera"); }
    void StartRunCamera(RUN3&, const SCPTCMD3& key, const SCPTCMD3* p1, const SCPTCMD3* p2) override
    {
        Log("camera move: key oid %d next %d %d", key.an.size() > 15 ? key.an[15] : 0,
            p1 && p1->an.size() > 15 ? p1->an[15] : -1, p2 && p2->an.size() > 15 ? p2->an[15] : -1);
    }
    void EndRunCamera(RUN3&) override { Log("camera end"); }
    void ApplyCheckpointTasks(int oidTask, int state, void*) override
    {
        mpoidstate[oidTask] = state;
        Log("task %d state %d", oidTask, state);
    }
    void ShowTitle() override
    {
        frameTitleEnd = frame + cframeTitle;
        Log("title");
    }
    bool FTitleShowing() override { return frame < frameTitleEnd; }
    void PushMusic(int id) override { Log("music push %d", id); }
    void PopMusic(int id) override { Log("music pop %d", id); }
    void Unported(RUN3& run, const SCPTCMD3& c) override
    {
        if (fVerbose) Log("(not ported: %02X %s)", c.id, SzScptCmd3(c.id));
        SCPTHOST3::Unported(run, c);
    }
    // the actor-driven waits: their durations from the reference (SCPT3_DURATIONS, tools/scptdur3.py: "oid index
    // frames", in execution order); a command's done test passes that many frames after it was executed (-1 never; a
    // command not in the file: at once)
    std::map<std::pair<int32_t, int>, std::deque<int>> mpdur;
    std::map<std::pair<RUN3*, int>, int> mpend;  // (run, command) -> the frame its activity ends
    std::pair<RUN3*, int> cur{nullptr, -1};      // the command being tested
    int FrameEnd() const
    {
        auto it = mpend.find(cur);
        return it == mpend.end() ? 0 : it->second;
    }
    bool FEnded() const { const int f = FrameEnd(); return f >= 0 && frame >= f; }
    void Testing(RUN3& run, int icmd) override
    {
        cur = {&run, icmd};
        // an alarm no script sets (the world's splice code sets it): set when its wait's time is up
        const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
        if (c.id == 0x0E && mpend.count(cur) && FEnded() && !FScptAlarm3(run.pscpt, c.an[0]))
        {
            Log("alarm %d set (from outside)", c.an[0]);
            SetScptAlarm3(run.pscpt, c.an[0], true);
        }
    }
    bool GetLoXf(void* plo, float mat[9], float pos[3]) override
    {
        SCPTHOST3::GetLoXf(plo, mat, pos);
        // (0x30's done test: the walking actor is far until its time is up)
        if (cur.first && cur.first->ploPending == plo && !FEnded()) pos[0] = 1e6f;
        return plo != nullptr;
    }
    void PlayActorScriptAnim(void* pa, int oid, int grf, int n) override { Log("actor %d anim %d grf %X n %d", Oid(pa), oid, grf, n); }
    void EndActorScriptAnim(void* pa) override { Log("actor %d anim end", Oid(pa)); }
    bool FActorInScriptAnim(void*) override { const int f = FrameEnd(); return f < 0 || frame < f - 1; }  // (0x16: done a step after)
    void* ApplyAseg(void* paseg, void* plo, int grf) override { Log("aseg %d on %d grf %X", Oid(paseg), Oid(plo), grf); return paseg; }
    bool FAsegaRetracted(void*) override { return FEnded(); }
    void PlaceLo(void* plo, const float*, const float pos[3]) override { Log("place %d at (%.1f %.1f %.1f)", Oid(plo), pos[0], pos[1], pos[2]); }
    void StartInteract(void* pa, void* plo) override { Log("actor %d interact %d", Oid(pa), Oid(plo)); }
    void ActorMoveTo(void* pa, void* plo, void*, const float*, int b4, int b8) override { Log("actor %d move to %d (%d %d)", Oid(pa), Oid(plo), b4, b8); }
    int ActorMoveState(void*) override { return FEnded() ? -1 : 0; }
    bool FActorOnPath(void*) override { return true; }
    float UActorPath(void*) override { return FEnded() ? 0.0f : 1e6f; }
    bool FDone28(void*) override { return FEnded(); }
    bool FScreenObjDone(void*) override { return FEnded(); }
    void ShowScreenObj(void* plo, int n) override { Log("screen %d show %d", Oid(plo), n); }
    void HideScreenObj(void* plo) override { Log("screen %d hide", Oid(plo)); }
    void* AddRemoveByOid(bool fAdd, int oid, float g, void*, SCPT3*) override { Log("%s %d (%.2f)", fAdd ? "add" : "remove", oid, g); return Plo(oid); }
    bool FAddRemoveDone(void*, bool) override { return FEnded(); }
    bool FButtonPressed(void*, int grf) override { (void)grf; return FEnded(); }
    void SetButtonPrompt(int grf) override { Log("prompt %X", grf); }
    void TellGuardCommands(void* pg, void* pc) override { Log("guard %d commands %d", Oid(pg), Oid(pc)); }
    bool FGuardRunsCommands(void*, void*) override { return !FEnded(); }
    void ActivateLo(int cid, void* plo, void*) override { Log("activate %d (cid %X)", Oid(plo), cid); }
    void SetClockFactor(float g) override { Log("clock factor %.3f", g); }
    void PauseGame(int i, int k) override { Log("pause %d kind %d", i, k); }
    void ResumeGame(int i, int k) override { Log("resume %d kind %d", i, k); }
    void JoltCm(int n) override { Log("jolt %d", n); }
    void FaceObject(void* pa, void* plo) override { Log("actor %d faces %d", Oid(pa), Oid(plo)); }
    void* PloProxyCurrent(void* p) override { return p; }
    void* PloProxyAcquire(void* p) override { return p; }
    void* PloProxyInstance(void* p) override { return p; }
    void LockPlayerControl(bool f) override { Log("player control %s", f ? "locked" : "released"); }
    void Executed(RUN3& run, int icmd) override
    {
        const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
        if (auto it = mpdur.find({run.pscpt->oid, icmd}); it != mpdur.end() && !it->second.empty())
        {
            const int d = it->second.front();
            it->second.pop_front();
            mpend[{&run, icmd}] = d < 0 ? -1 : frame + d;
        }
        else
            mpend.erase({&run, icmd});
        std::printf("%5d exec oid %d [%d] %02X %s\n", frame, run.pscpt->oid, icmd, c.id, SzScptCmd3(c.id));
    }
    std::map<std::pair<RUN3*, int>, bool> mpdone;
    void DoneTested(RUN3& run, int icmd, bool fDone) override
    {
        auto [it, fNew] = mpdone.try_emplace({&run, icmd}, fDone);
        if (!fNew && it->second == fDone) return;
        it->second = fDone;
        const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
        std::printf("%5d done oid %d [%d] %02X -> %d\n", frame, run.pscpt->oid, icmd, c.id, fDone ? 1 : 0);
    }
    void Log(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (!fVerbose) return;
        char ach[512];
        va_list va;
        va_start(va, fmt);
        std::vsnprintf(ach, sizeof ach, fmt, va);
        va_end(va);
        std::printf("%5d   %s\n", frame, ach);
    }
    int framePause = -1, frameResume = -1;  // SCPT3_PAUSE=A:B: the game clock stopped for frames [A, B) (the card prompt)
    void Tick()
    {
        ++frame;
        if (frame < framePause || frame >= frameResume) t += c_dt;
        if (binocState != binocTarget && frame >= frameBinoc) binocState = binocTarget;
    }
};
}  // namespace

int ScptRun3Command(const std::string& world, const std::string& oids, int cframe, bool fVerbose)
{
    std::optional<WORLD3> pwd;  // (Worlds() returns a copy)
    for (const WORLD3& wd : g_disc.Worlds())
        if (wd.fk.cb && (wd.name == world || std::to_string(wd.iworld) == world)) pwd = wd;
    if (!pwd)
    {
        std::fprintf(stderr, "no world %s (--list-files)\n", world.c_str());
        return 2;
    }
    TESTHOST3 host;
    host.fVerbose = fVerbose;
    if (const char* e = std::getenv("SCPT3_TITLE_FRAMES")) host.cframeTitle = std::atoi(e);
    // SCPT3_START_LAG=1: the scripts start before the frame's clock update (the reference's m_ext 4268, started by the
    // title's flow, sees the previous frame's time; 4271, started from a script's 0x1E, the current one)
    const int cframeLag = std::getenv("SCPT3_START_LAG") ? std::atoi(std::getenv("SCPT3_START_LAG")) : 0;
    host.t = -float(cframeLag) * c_dt;
    {
        std::vector<uint8_t> ab = g_disc.ReadWorld(pwd->fk);
        WORLDWALK3 w;
        std::string error;
        if (FWalkWorld3(ab, w, error))
            for (const OBJHDR3& o : w.aobj)
                if (o.cid != Icid(CID3::PROXY)) host.mpoidcid.try_emplace(o.oid, o.cid);  // (proxies share oids)
    }
    FDialogLengthsOfWorld(pwd->name, &host.mpidtDialog);
    if (const char* e = std::getenv("SCPT3_CONDS"))
    {
        std::stringstream sc(e);
        for (std::string tok; std::getline(sc, tok, ',');)
            if (const auto eq = tok.find('='); eq != std::string::npos) host.mpcond[std::atoi(tok.c_str())] = std::atoi(tok.c_str() + eq + 1) != 0;
    }
    if (const char* e = std::getenv("SCPT3_PAUSE")) std::sscanf(e, "%d:%d", &host.framePause, &host.frameResume);
    if (const char* e = std::getenv("SCPT3_DURATIONS"))
    {
        std::ifstream f(e);
        for (int32_t oid, icmd, d; f >> oid >> icmd >> d;) host.mpdur[{oid, icmd}].push_back(d);
    }
    std::vector<std::unique_ptr<SCPT3>> ascpt;
    for (SCPT3LOADED& l : AscptLoadWorld3(*pwd))
    {
        auto up = std::make_unique<SCPT3>();
        up->oid = l.oid;
        up->name = l.group;
        up->acmd = std::move(l.acmd);
        up->plo = TESTHOST3::Plo(l.oid);
        host.mpoidscpt[l.oid] = up.get();
        ascpt.push_back(std::move(up));
    }
    SetScptHost3(&host);
    std::stringstream ss(oids);
    for (std::string tok; std::getline(ss, tok, ',');)
    {
        auto it = host.mpoidscpt.find(std::atoi(tok.c_str()));
        if (it == host.mpoidscpt.end())
        {
            std::fprintf(stderr, "no script %s in %s\n", tok.c_str(), pwd->name.c_str());
            SetScptHost3(nullptr);
            return 2;
        }
        StartScpt3(it->second, nullptr);
    }
    if (cframeLag)  // (the clock update of the start's frame, then that frame's script updates)
    {
        host.t += float(cframeLag) * c_dt;
        for (auto& up : ascpt)
            if (FScptRunning3(up.get())) UpdateScpt3(up.get(), c_dt);
    }
    for (int i = 0; i < cframe; ++i)
    {
        host.Tick();
        bool fAny = false;
        for (auto& up : ascpt)
            if (FScptRunning3(up.get()))
            {
                UpdateScpt3(up.get(), c_dt);
                fAny = true;
            }
        if (!fAny) break;
    }
    std::printf("%5d end:", host.frame);
    for (auto& up : ascpt)
        if (FScptRunning3(up.get())) std::printf(" %d", up->oid);
    std::printf(" running\n");
    SetScptHost3(nullptr);
    return 0;
}
