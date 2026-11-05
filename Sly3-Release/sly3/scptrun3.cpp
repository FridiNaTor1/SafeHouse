// Sly 3's script executor (scptrun3.h). Ported from the Sly 3 asm (orig3 addresses on every function); the handlers'
// argument positions are those of the reader 0x225220 (scpt3.cpp SzArgs, the RAM offsets in the comments).

#include "scptrun3.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <set>

namespace
{
SCPTHOST3 s_hostDefault;
SCPTHOST3* s_phost = &s_hostDefault;

constexpr float c_tNever = 3.4028235e38f;  // 0x326430 / 0x326434 / 0x326C3C / 0x326830: FLT_MAX

// The retired runners stay allocated until the outermost call returns (the original keeps them alive with a
// reference taken by vt slot 1 / released by slot 2 around RunRun / the executor).
std::vector<std::unique_ptr<RUN3>> s_arunRetired;
int s_cdepth = 0;
struct DEPTH
{
    DEPTH() { ++s_cdepth; }
    ~DEPTH()
    {
        if (--s_cdepth == 0) s_arunRetired.clear();
    }
};

float F(int32_t n) { return std::bit_cast<float>(n); }
int32_t A(const SCPTCMD3& c, std::size_t i) { return i < c.an.size() ? c.an[i] : 0; }

const SCPTCMD3* Pcmd(const SCPT3* ps, int i)
{
    return i >= 0 && i < int(ps->acmd.size()) ? &ps->acmd[std::size_t(i)] : nullptr;
}
int IChild(const SCPT3* ps, int i)
{
    const SCPTCMD3* p = Pcmd(ps, i);
    return p && p->ichild ? p->ichild : -1;
}
int ISib(const SCPT3* ps, int i)
{
    const SCPTCMD3* p = Pcmd(ps, i);
    return p && p->isib ? p->isib : -1;
}

// orig3: 0x226458 (the script's clock: real time when its flags +0x3C carry 0x200)
float TScpt(const SCPT3* ps) { return s_phost->T((ps->grf & 0x200) != 0); }

// orig3: 0x226770 PloFind (kind 4 from the run's context; a proxy (cid 8) gives its current instance: the host's job)
void* PloFind(RUN3* p, int oid) { return oid == -1 ? nullptr : s_phost->PloFind(4, oid, p->ploContext); }
// orig3: 0x2267E8 (oid 0x20: the nearest of kind 0 (the camera's own?) instead)
void* PloFindCam(RUN3* p, int oid) { return oid == 0x20 ? s_phost->PloFind(0, oid, nullptr) : PloFind(p, oid); }

// orig3: 0x226B78 (the actor whose binocucom this run or a parent opened)
void* PloBinoc(RUN3* p)
{
    for (; p; p = p->prunParent)
        if (p->ploBinoc) return p->ploBinoc;
    return nullptr;
}

// orig3: 0x226348 PrunFind (the runner whose root command's oid is n, other than pSkip)
RUN3* PrunFind(SCPT3* ps, int n, RUN3* pSkip)
{
    for (auto& up : ps->arun)
        if (up.get() != pSkip && !up->fRetired && Pcmd(ps, up->icmdRoot)->oid == n) return up.get();
    return nullptr;
}

bool FHasAlarm(const SCPT3* ps, int n)  // orig3: 0x226270
{
    return std::find(ps->aalarm.begin(), ps->aalarm.end(), n) != ps->aalarm.end();
}
void EnsureAlarm(SCPT3* ps, int n)  // orig3: 0x2261A0
{
    if (!FHasAlarm(ps, n)) ps->aalarm.push_back(n);
}
void RemoveAlarm(SCPT3* ps, int n)  // orig3: 0x2261F0
{
    ps->aalarm.erase(std::remove(ps->aalarm.begin(), ps->aalarm.end(), n), ps->aalarm.end());
}

void Exec(RUN3* p);
void RunRun(RUN3* p);
void RetireRun(RUN3* p);
bool FCmdDone(RUN3* p, int icmd);

// orig3: 0x22B028 SetRunPending (the command whose done test FinishRunPending runs, and the actor it drives: the
// actor is told which run drives it, 0x156468)
void SetRunPending(RUN3* p, int icmd, void* ploActor)
{
    p->icmdPending = icmd;
    if (p->ploPending == ploActor) return;
    if (p->ploPending) s_phost->SetActorRun(p->ploPending, nullptr);
    p->ploPending = ploActor;
    if (ploActor) s_phost->SetActorRun(ploActor, p);
}

// orig3: 0x22B238 (the run holds the player's control lock, run +0x174)
void LockPlayer(RUN3* p, bool f)
{
    if (p->fLockPlayer == f) return;
    p->fLockPlayer = f;
    s_phost->LockPlayerControl(f);
}

// orig3: 0x22B290 (0x1F: the game paused for the run, run +0x3C on / +0x40 kind / +0x44 player)
void PauseRun(RUN3* p, bool fOn, int kind)
{
    if (!fOn)
    {
        if (!p->fPaused) return;
        s_phost->ResumeGame(p->iplayerPause, p->kindPause);
    }
    else
    {
        if (p->fPaused)
        {
            if (kind == p->kindPause) return;
            s_phost->ResumeGame(p->iplayerPause, p->kindPause);
        }
        p->iplayerPause = p->plo28 && s_phost->FDerivedFrom(p->plo28, 0x31) ? s_phost->IPlayerOfJob(p->plo28) : -1;
        s_phost->PauseGame(p->iplayerPause, kind);
    }
    p->kindPause = kind;
    p->fPaused = fOn;
}

// orig3: 0x2263B0 (the clock factor 6; a script runs on real time while it is 0)
void SetScptClockFactor(SCPT3* ps, float g)
{
    s_phost->SetClockFactor(g);
    if (g == 0.0f) ps->grf |= 0x200;
    else ps->grf &= ~0x200u;
}

// orig3: 0x22AEC8 GetRunAim (where an object is, for the run's actor (+0x28, else the pending +0x4C), and its heading:
// a curve (cid 0x09): its point nearest the actor; another object: its translation and its first row's angle; none:
// the actor itself (plus its aim offset for cid 0x32 actors, then its heading +0x1BC))
void GetRunAim(RUN3* p, void* plo, float pos[3], float* pang)
{
    void* pa = p->plo28 ? p->plo28 : p->ploPending;
    float mat[9], t[3], ang = 0;
    if (plo)
    {
        if (s_phost->FDerivedFrom(plo, 0x09))
        {
            float dir[3];
            s_phost->GetLoXf(pa, mat, t);
            s_phost->NearestOnCurve(plo, t, pos, dir, nullptr);
            ang = std::atan2(dir[1], dir[0]);  // 0x1205C8
        }
        else
        {
            s_phost->GetLoXf(plo, mat, pos);
            ang = std::atan2(mat[1], mat[0]);
        }
    }
    else
    {
        s_phost->GetLoXf(pa, mat, pos);
        if (pa && s_phost->FDerivedFrom(pa, 0x32))
        {
            float d[3];
            s_phost->ActorAimOffset(pa, d);
            for (int i = 0; i < 3; ++i) pos[i] += d[i];
            ang = s_phost->ActorHeading(pa);
        }
        else
            ang = std::atan2(mat[1], mat[0]);
    }
    if (pang) *pang = ang;
}

// orig3: 0x22BF40 PrunNew
RUN3* PrunNew(SCPT3* ps, int icmdRoot, RUN3* pParent)
{
    auto up = std::make_unique<RUN3>();
    RUN3* p = up.get();
    p->pscpt = ps;
    p->prunParent = pParent;
    p->ploContext = pParent ? pParent->ploContext : ps->plo;
    const SCPTCMD3* pc = Pcmd(ps, icmdRoot);
    if (pc && pc->oid != -1) p->plo20 = s_phost->PloFind(5, pc->oid, nullptr);
    p->icmdRoot = icmdRoot;
    p->tWake = c_tNever;
    ps->arun.insert(ps->arun.begin(), std::move(up));  // PrependDlEntry
    return p;
}

// orig3: 0x22BB50
bool FRunRetired(const RUN3* p) { return p->fRetired; }

// orig3: 0x22B930 RetireRun: the root's finally block (its U16 argument: an 0x02 block executed at once, without
// waits), then the run's state released (0x22B678.. effects, binocucom 0x22B7F0, the pending command, sound +0x114,
// EndRunCamera 0x22BEE8) and the run freed.
void RetireRun(RUN3* p)
{
    if (!p || p->fRetired) return;
    DEPTH depth;
    SCPT3* ps = p->pscpt;
    auto it = std::find_if(ps->arun.begin(), ps->arun.end(), [p](const auto& up) { return up.get() == p; });
    if (it == ps->arun.end()) return;
    s_arunRetired.push_back(std::move(*it));
    ps->arun.erase(it);
    p->fRetired = true;
    const int icmdFinally = A(*Pcmd(ps, p->icmdRoot), 0) & 0x7FF;
    if (icmdFinally && Pcmd(ps, icmdFinally))
    {
        p->fDescend = true;
        p->aicmdStack.clear();
        p->icmdCur = icmdFinally;
        while (p->icmdCur >= 0) Exec(p);
        p->fDescend = false;
    }
    p->prunParent = nullptr;  // 0x2BF050(run +0x18, 0)
    if (p->pasega)            // 0x22B678
    {
        s_phost->RetractAsega(p->pasega);
        s_phost->ReleaseAsega(p->pasega);
        p->pasega = nullptr;
    }
    if (p->ploAnim)
    {
        s_phost->EndActorScriptAnim(p->ploAnim);
        p->ploAnim = nullptr;
    }
    if (p->pscptWait)  // 0x22B6E0
    {
        StopScpt3(p->pscptWait);
        p->pscptWait = nullptr;
    }
    // (0x22B718: run +0x60, set by no command m_ext's scripts use)
    if (p->ploScreen)  // 0x22B770
    {
        s_phost->HideScreenObj(p->ploScreen);
        p->ploScreen = nullptr;
    }
    // (0x22B7B8: run +0x16C (0x6C / 0x73); 0x22B828: the held run effects (0x55 / 0x56): not used by m_ext's scripts)
    if (p->ploBinoc)  // 0x22B7F0
    {
        s_phost->CloseBinoc(p->ploBinoc);
        p->ploBinoc = nullptr;
    }
    // (0x22B890: ClearJobs(actor +0x29C) when the current command is 0x33 / 0x34: job commands, not ported)
    SetRunPending(p, -1, nullptr);
    // (0x22B090 / 0x22B150 (run +0x74 / +0x7C, set by 0x24 / 0x25): not used by m_ext's scripts)
    LockPlayer(p, false);
    PauseRun(p, false, 0);
    if (p->psnd)
    {
        s_phost->StopSound(p->psnd);
        p->psnd = nullptr;
    }
    s_phost->EndRunCamera(*p);  // 0x22BEE8
}

// orig3: 0x226578 (waiting for the run's wake time +0x88)
bool FRunWaiting(RUN3* p) { return p->tWake != c_tNever && TScpt(p->pscpt) < p->tWake; }

// orig3: 0x226950 (a condition: kind, param)
bool FCondition(RUN3* p, int kind, int param)
{
    switch (kind)
    {
    case 0x75E: return true;
    case 0x75F: return false;
    case 0x760: case 0x761: case 0x762:
        return s_phost->FObjectCondition(kind, s_phost->PloFind(5, param, nullptr));
    case 0x763: return FHasAlarm(p->pscpt, param);
    case 0x764: return !FHasAlarm(p->pscpt, param);
    case 0x765: return param == 0x768;  // 0x25E7A8() (returns 0) indexes {0x768, 0x769, 0x76A, 0x76B, 0x76C}
    case 0x766: return !s_phost->FDialogPlaying(-1);
    case 0x767: return s_phost->FDialogPlaying(-1);
    default: return s_phost->FScriptCondition(p->pscpt, kind, param);
    }
}

// orig3: 0x226BE0 (before a command: its target +0x28)
void PrepareCmd(RUN3* p, const SCPTCMD3& c)
{
    LockPlayer(p, false);
    p->plo28 = c.id == 0 ? nullptr
             : c.oid != -1 ? PloFind(p, c.oid) : p->plo24 ? p->plo24 : p->plo20 ? p->plo20 : p->pscpt->ploStarter;
    // the commands that drive Sly (cid 0x34; the table 0x326480): the script's mode SCPT +0xA8 says how
    if (p->fNoControl || !p->plo28 || !s_phost->FDerivedFrom(p->plo28, 0x34)) return;
    switch (c.id)
    {
    case 0x16: case 0x26: case 0x27: case 0x28: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x30:
    case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x5C: case 0x5D: case 0x5E: case 0x64: case 0x65:
    case 0x66: case 0x67: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x73: case 0x74:
        if (p->pscpt->mode == 0) s_phost->ResetActorControl(p->plo28);
        else if (p->pscpt->mode == 2) LockPlayer(p, true);
        break;
    default: break;
    }
}

// Advance to the next command (the executor's walk, 0x226D00 up to its dispatch): returns false when the run ends.
bool FAdvance(RUN3* p)
{
    SCPT3* ps = p->pscpt;
    const SCPTCMD3* pcur = Pcmd(ps, p->icmdCur);
    if (p->icmdCur == p->icmdRoot || (p->fDescend && pcur && pcur->id == 2))
    {
        const int ichild = IChild(ps, p->icmdCur);
        if (ichild < 0)
        {
            p->icmdCur = -1;
            return false;
        }
        p->aicmdStack.push_back(p->icmdCur);
        p->icmdCur = ichild;
        return true;
    }
    int inext = ISib(ps, p->icmdCur);
    while (inext < 0)
    {
        // pop (the kind table 0x326620)
        int ipop = -1;
        if (!p->aicmdStack.empty())
        {
            ipop = p->aicmdStack.back();
            p->aicmdStack.pop_back();
        }
        p->icmdCur = ipop;
        const SCPTCMD3* pp = Pcmd(ps, ipop);
        if (!pp) return false;
        switch (pp->id)
        {
        case 0x00: case 0x02: p->icmdCur = -1; return false;  // the run's block / the finally block ends
        case 0x04: return true;                               // a while: tested again
        case 0x07: case 0x08: case 0x09:                      // an if's branch: its if too
            if (!p->aicmdStack.empty())
            {
                p->icmdCur = p->aicmdStack.back();
                p->aicmdStack.pop_back();
            }
            else
                p->icmdCur = -1;
            break;
        default: break;
        }
        inext = ISib(ps, p->icmdCur);
    }
    p->icmdCur = inext;
    return true;
}

void Wait(RUN3* p) { p->tStart = TScpt(p->pscpt); }

// orig3: 0x22A430 (0x1D: the clock factor a + u (b + u c), u = t / duration clamped to [0, 1] (1 with no duration),
// clamped to [min, max]; arguments a b c (+0x00..), min max (+0x10 / +0x14), duration (+0x18))
void TimeRamp(RUN3* p, const SCPTCMD3& c, float t)
{
    const float dur = F(A(c, 5));
    float u = 1.0f;
    if (dur != 0.0f) u = std::clamp(t / dur, 0.0f, 1.0f);
    float g = F(A(c, 0)) + u * (F(A(c, 1)) + u * F(A(c, 2)));
    if (g < F(A(c, 3))) g = F(A(c, 3));
    else if (F(A(c, 4)) < g) g = F(A(c, 4));
    SetScptClockFactor(p->pscpt, g);
}  // orig3: 0x2293DC (the waits; the done test does the work)

// orig3: 0x226D00 the executor: the next command, executed
void Exec(RUN3* p)
{
    DEPTH depth;
    SCPT3* ps = p->pscpt;
    if (!FAdvance(p)) return;
    for (int guard = 0; guard < 100000; ++guard)
    {
        const int icmd = p->icmdCur;
        const SCPTCMD3& c = *Pcmd(ps, icmd);
        PrepareCmd(p, c);
        s_phost->Executed(*p, icmd);
        if (static const bool s_f = std::getenv("SLY3_SCPTTRACE") != nullptr; s_f)
            std::printf("scpt3: t %.3f exec oid %d [%d] %02X %s\n", TScpt(ps), ps->oid, icmd, c.id, SzScptCmd3(c.id));
        switch (c.id)
        {
        case 0x00:  // spawn: a runner on this block (its oid names it for 0x0B / 0x0C / 0x0D)
            RunRun(PrunNew(ps, icmd, p));
            return;
        case 0x04:  // while (kind, param): the body while the condition holds
            if (FCondition(p, A(c, 0), A(c, 1)))
                if (const int ichild = IChild(ps, icmd); ichild >= 0)
                {
                    p->aicmdStack.push_back(icmd);
                    p->icmdCur = ichild;
                    continue;
                }
            Wait(p);
            return;
        case 0x06:  // if: the first case 0x07 / 0x08 whose condition holds, or an else 0x09
        {
            int ibranch = IChild(ps, icmd);
            while (ibranch >= 0)
            {
                const SCPTCMD3& b = *Pcmd(ps, ibranch);
                if ((b.id != 7 && b.id != 8) || FCondition(p, A(b, 0), A(b, 1))) break;
                ibranch = ISib(ps, ibranch);
            }
            const int ichild = IChild(ps, ibranch);
            if (ibranch < 0 || ichild < 0)
            {
                Wait(p);
                return;
            }
            p->aicmdStack.push_back(icmd);
            p->aicmdStack.push_back(ibranch);
            p->icmdCur = ichild;
            continue;
        }
        case 0x0B:  // retire: n 0x3B the runs with a parent, -1 this run, else the runner named n
        {
            const int n = A(c, 0);
            if (n == 0x3B)
            {
                std::vector<RUN3*> a;
                for (auto& up : ps->arun)
                    if (up->prunParent) a.push_back(up.get());
                for (RUN3* q : a) RetireRun(q);
            }
            else
                RetireRun(n == -1 ? p : PrunFind(ps, n, nullptr));
            return;
        }
        case 0x0F: case 0x10:  // alarm n of this script (oid -1) or of the script oid: 0x0F set, 0x10 clear
        {
            SCPT3* pt = A(c, 1) == -1 ? ps : nullptr;
            if (!pt)
                if (void* plo = PloFind(p, A(c, 1)); plo && s_phost->FDerivedFrom(plo, 0x87)) pt = s_phost->PscptFromLo(plo);
            if (pt) (c.id == 0x0F ? EnsureAlarm : RemoveAlarm)(pt, A(c, 0));
            return;
        }
        case 0x11: case 0x17:  // stop / start the script oid
            if (SCPT3* pt = s_phost->PscptFromLo(PloFind(p, A(c, 0))))
            {
                if (c.id == 0x11) StopScpt3(pt);
                else
                {
                    StartScpt3(pt, p->plo28);
                    if (c.fFlag) p->pscptWait = pt;  // (the wait flag: RetireRun stops it)
                }
            }
            return;
        case 0x14: p->plo24 = PloFind(p, c.oid); return;  // "with": the default target
        case 0x15: s_phost->ScriptEvent(ps, A(c, 0)); return;
        case 0x1E: s_phost->ApplyCheckpointTasks(A(c, 0), A(c, 1), p->ploContext); return;
        case 0x23: s_phost->CutCm(); return;
        case 0x2F:  // wait a random time in [min, max] (the actor (oid) idles meanwhile: not ported)
            p->gWait = s_phost->GRandInRange(F(A(c, 0)), F(A(c, 1)));
            Wait(p);
            return;
        case 0x46:  // OpenBinoc
        {
            BINOCOPEN3 b;
            b.oidActor = A(c, 0);
            b.oidSpeaker = A(c, 1);
            for (int i = 0; i < 3; ++i) b.posEye[i] = F(A(c, 2 + std::size_t(i)));
            for (int i = 0; i < 3; ++i) b.posLook[i] = F(A(c, 5 + std::size_t(i)));
            b.g30 = F(A(c, 8));
            b.oid60 = A(c, 9);
            b.oid64 = A(c, 10);
            b.oid68 = A(c, 11);
            void* ploSpeaker = PloFind(p, b.oidSpeaker);  // (its class 0x32 is tested, the result unused)
            void* plo68 = s_phost->PloFind(0x405, b.oid68, nullptr);
            void* plo60 = PloFind(p, b.oid60);
            void* plo64 = PloFind(p, b.oid64);
            void* ploActor = PloFind(p, b.oidActor);
            if (ploActor && s_phost->FDerivedFrom(ploActor, 0x33) && s_phost->FBinocCanOpen(ploActor) && !p->ploBinoc)
            {
                p->ploBinoc = ploActor;
                s_phost->OpenBinoc(ploActor, b, ploSpeaker, plo60, plo64, plo68);
            }
            return;
        }
        case 0x47:  // close the binocucom this run opened (0x22B7F0)
            if (p->ploBinoc)
            {
                s_phost->CloseBinoc(p->ploBinoc);
                p->ploBinoc = nullptr;
            }
            return;
        case 0x48: case 0x49:  // the binocucom camera's focus (0x16ADC0 / 0x16AE00)
            if (void* pa = PloBinoc(p)) s_phost->CmFocus(pa, c.id, PloFind(p, A(c, 0)));
            return;
        case 0x4A:  // its targets (pos, dir, g)
            if (void* pa = PloBinoc(p))
            {
                const float pos[3] = {F(A(c, 0)), F(A(c, 1)), F(A(c, 2))};
                const float dir[3] = {F(A(c, 3)), F(A(c, 4)), F(A(c, 5))};
                s_phost->CmTargets(pa, pos, dir, F(A(c, 6)));
            }
            return;
        case 0x4B:
            if (void* pa = PloBinoc(p)) s_phost->CmZoom(pa, F(A(c, 0)));
            return;
        case 0x4C:
            if (void* pa = PloBinoc(p)) s_phost->CmSnap(pa);
            return;
        case 0x4D:  // who is on the binocucom's screen (0x1BE370)
        {
            void* plo = PloFind(p, A(c, 0));
            void* pa = PloBinoc(p);
            if (plo && s_phost->FDerivedFrom(plo, 0x32) && pa) s_phost->SetBinocSpeaker(pa, plo);
            return;
        }
        case 0x50: s_phost->ShowTitle(); Wait(p); return;
        case 0x52:  // a dialog line: PsndStartDialog + the sound's source, falloff, volume (0x22873C)
        {
            DIALOGSTART3 d;
            const int n = std::max(A(c, 0), 0);
            for (int i = 0; i < n; ++i) d.aid.push_back(A(c, 1 + std::size_t(i)));
            d.fQuiet = A(c, 1 + std::size_t(n));
            d.oidSource = A(c, 2 + std::size_t(n));
            d.sFalloffStart = float(A(c, 3 + std::size_t(n)));
            d.sFalloffEnd = float(A(c, 4 + std::size_t(n)));
            d.gVolume = F(A(c, 5 + std::size_t(n)));
            d.ploSpeaker = c.oid != -1 ? p->plo28 : nullptr;
            // (not ported: 0x22B398(run, args), before the line)
            if (!d.aid.empty()) s_phost->StartDialog(d, PloFind(p, d.oidSource));
            p->icmdDialog = icmd;
            return;
        }
        case 0x53:  // stop the dialog if one of the lines plays (or any, with no list)
        {
            const int n = std::max(A(c, 0), 0);
            bool fStop = n == 0;
            for (int i = 0; i < n && !fStop; ++i) fStop = s_phost->FDialogPlaying(A(c, 1 + std::size_t(i)));
            if (fStop) s_phost->StopDialog();
            return;
        }
        case 0x54: s_phost->ResetDialogSubtitle(); return;
        case 0x59: s_phost->PushMusic(A(c, 0)); return;
        case 0x5A: s_phost->PopMusic(A(c, 0)); return;
        case 0x6E:  // close the target actor's binocucom (cid 0x33)
            if (p->plo28 && s_phost->FDerivedFrom(p->plo28, 0x33)) s_phost->CloseBinoc(p->plo28);
            return;
        case 0x12:  // activate an object by its class (0x2271C8)
        {
            void* plo = PloFind(p, A(c, 0));
            if (!plo) return;
            static const int s_acid[] = {0x5D, 0x15, 0x27, 0x28, 0x18, 0x8A, 0x4D, 0x71, 0x21, 0x47, 0x50};
            int cid = 0;
            for (int k : s_acid)
                if (s_phost->FDerivedFrom(plo, k))
                {
                    cid = k;
                    break;
                }
            // (0x21 / 0x47 / 0x50 act on the run's actor: nothing without one of cid 0x33)
            if ((cid == 0x21 || cid == 0x47 || cid == 0x50) && !(p->plo28 && s_phost->FDerivedFrom(p->plo28, 0x33))) return;
            s_phost->ActivateLo(cid, plo, p->plo28);
            return;
        }
        case 0x13:  // a job command (0x1F4B00(GAME, n, job))
            if (void* plo = PloFind(p, A(c, 0)); plo && s_phost->FDerivedFrom(plo, 0x31)) s_phost->JobCommand(plo, A(c, 1));
            return;
        case 0x16:  // an animation: on an actor (PlayActorScriptAnim, n 5) or an ASEG applied to an object
        {
            void* pa = PloFind(p, A(c, 1));
            if (pa && s_phost->FDerivedFrom(pa, 0x33))
            {
                s_phost->PlayActorScriptAnim(pa, A(c, 0), A(c, 2) | 1, 5);
                if (c.fFlag) p->ploAnim = pa;
                return;
            }
            void* plo = pa && s_phost->FDerivedFrom(pa, 0x2F) ? pa : nullptr;
            void* paseg = PloFind(p, A(c, 0));
            if (!paseg || !s_phost->FDerivedFrom(paseg, 0x82)) return;
            void* pasega = s_phost->ApplyAseg(paseg, plo, A(c, 2));
            if (c.fFlag && pasega) p->pasega = pasega;  // (retained: vt slot 1)
            return;
        }
        case 0x1A: case 0x1B:  // a screen object (cid 0x9E): shown with n / hidden
        {
            void* plo = s_phost->PloFind(0x405, A(c, 0), nullptr);
            if (!plo || !s_phost->FDerivedFrom(plo, 0x9E)) return;
            if (c.id == 0x1A)
            {
                s_phost->ShowScreenObj(plo, A(c, 1));
                if (c.fFlag) p->ploScreen = plo;
            }
            else
                s_phost->HideScreenObj(plo);
            return;
        }
        case 0x1D: TimeRamp(p, c, 0.0f); return;  // the clock factor's curve, at t 0 (0x22A430)
        case 0x1F: PauseRun(p, A(c, 0) != 0, A(c, 1)); return;
        case 0x22:  // place the target on an object (its position and heading), or start an interact (cid 0x19)
        {
            void* plo = PloFind(p, A(c, 0));
            void* pa = nullptr;
            if (p->plo28 && s_phost->FDerivedFrom(p->plo28, 0x33))
            {
                pa = p->plo28;
                if (s_phost->FStoredFlagIsClear(pa)) s_phost->CloseBinocNow(pa);
            }
            if (plo && s_phost->FDerivedFrom(plo, 0x27))
            {
                s_phost->Activate27(plo);
                return;
            }
            if (!p->plo28) return;
            if (plo && s_phost->FDerivedFrom(plo, 0x19))
            {
                if (!pa) return;
                s_phost->StartInteract(pa, plo);
                SetRunPending(p, -1, nullptr);
                return;
            }
            float pos[3], ang = 0;
            GetRunAim(p, plo, pos, &ang);
            if (A(c, 2)) s_phost->DropOnGround(pos);
            // MatRotateAxis(Z (0x317550), ang): the rows of a rotation about z
            const float cs = std::cos(ang), sn = std::sin(ang);
            const float mat[9] = {cs, sn, 0, -sn, cs, 0, 0, 0, 1};
            s_phost->PlaceLo(p->plo28, mat, pos);
            if (!pa) return;
            if (A(c, 1) == 0) s_phost->ResetActorAfterPlace(pa);
            SetRunPending(p, -1, nullptr);
            return;
        }
        case 0x26:  // along the actor's path, to the point nearest an object
        {
            void* pa = p->plo28;
            if (!pa || !s_phost->FDerivedFrom(pa, 0x33) || !s_phost->FActorOnPath(pa)) return;
            float pos[3], posN[3], dir[3];
            GetRunAim(p, PloFind(p, A(c, 0)), pos, nullptr);
            s_phost->NearestOnCurve(s_phost->PcrvActorPath(pa), pos, posN, dir, &p->uCrv84);
            SetRunPending(p, icmd, pa);
            return;
        }
        case 0x28:
        {
            void* pa = p->plo28;
            if (!pa || !s_phost->FDerivedFrom(pa, 0x33) || A(c, 1) == -1) return;
            void* plo1 = s_phost->PloFind(1, A(c, 1), pa);
            if (!plo1) return;
            void* plo = PloFind(p, A(c, 0));
            if (!plo && A(c, 0) != -1) return;
            s_phost->Start28(pa, plo1, plo);
            return;
        }
        case 0x30:  // walk toward an object with an animation, until within a radius (the done test)
        {
            void* pa = p->plo28;
            if (!pa || !s_phost->FDerivedFrom(pa, 0x33)) return;
            if (A(c, 3) != -1) s_phost->PlayActorScriptAnim(pa, A(c, 3), 9, 6);
            if (c.fFlag) p->ploAnim = pa;
            if (void* plo = PloFind(p, A(c, 0)))
            {
                for (int cid : {0x20, 0x0B, 0x24, 0x64, 0x0A, 0x26})
                    if (s_phost->FDerivedFrom(plo, cid))
                    {
                        s_phost->ResetActorInteract(pa);
                        break;
                    }
                if (void* pcrv = s_phost->PcrvActorInteract(pa))
                {
                    float pos[3], posN[3], dir[3];
                    GetRunAim(p, plo, pos, nullptr);
                    s_phost->NearestOnCurve(pcrv, pos, posN, dir, &p->uCrv80);
                }
            }
            SetRunPending(p, icmd, pa);  // (0x1573A0 before it: a stub)
            return;
        }
        case 0x36:  // the actor moves to an object (its nav helper's goal, or its aim) or, with none, where it is
        {
            void* pa = p->plo28;
            if (!pa || !s_phost->FDerivedFrom(pa, 0x33)) return;
            void* plo = PloFind(p, A(c, 0));
            float pos[3] = {0, 0, 0};
            void* pnav = nullptr;
            if (!plo)
            {
                float mat[9], d[3];
                s_phost->GetLoXf(pa, mat, pos);
                s_phost->ActorAimOffset(pa, d);
                for (int i = 0; i < 3; ++i) pos[i] += d[i];
            }
            else if ((pnav = s_phost->PnavOf(plo)))
            {
                if (s_phost->FActorMovingTo(pa, plo)) return;
                s_phost->NavGoal(pnav, pa, pos);
            }
            else
            {
                GetRunAim(p, plo, pos, nullptr);
                if (A(c, 3)) s_phost->DropOnGround(pos);
            }
            s_phost->ActorMoveTo(pa, plo, pnav, pos, A(c, 1), A(c, 2));
            SetRunPending(p, icmd, pa);
            return;
        }
        case 0x38: case 0x39:  // an object added / removed (faded in / out: the done test)
        {
            void* plo = s_phost->AddRemoveByOid(c.id == 0x38, A(c, 0), F(A(c, 1)), p->ploContext, ps);
            if (plo && c.fFlag && s_phost->FDerivedFrom(plo, 0x2F)) p->ploAddRemove = plo;
            return;
        }
        case 0x3F: s_phost->JoltCm(A(c, 0)); Wait(p); return;
        case 0x41: s_phost->Cmd41(A(c, 0), A(c, 1), F(A(c, 2)), p->ploContext, ps); return;
        case 0x43:
            p->psnd = s_phost->PlayAsegaSound(A(c, 0), A(c, 1), A(c, 2), A(c, 3), F(A(c, 4)), F(A(c, 5)), F(A(c, 6)),
                                              p->ploContext, ps);
            return;
        case 0x4E: s_phost->SetButtonPrompt(A(c, 0) & 0xF0); return;  // then the done test waits for the buttons
        case 0x60: s_phost->ShowHint(A(c, 0), F(A(c, 1))); return;
        case 0x61: s_phost->ClearHint(A(c, 0)); return;
        case 0x62:  // a guard's commands
        {
            void* pg = PloFind(p, A(c, 0));
            void* pcmds = PloFind(p, A(c, 1));
            s_phost->TellGuardCommands(pg, pcmds);
            if (c.fFlag)
            {
                p->ploGuard = pg;
                p->ploGuardCmds = pcmds;
            }
            return;
        }
        case 0x6F:  // a proxy's instance: the current one (flag 1) or a new one; flag 2 locks its row
        {
            void* pproxy = s_phost->PloFind(4, A(c, 0), p->ploContext);
            if (!pproxy || !s_phost->FDerivedFrom(pproxy, 0x08)) return;
            void* plo = A(c, 1) & 1 ? s_phost->PloProxyCurrent(pproxy) : s_phost->PloProxyAcquire(pproxy);
            if (plo && (A(c, 1) & 2)) s_phost->LockProxyRow(pproxy, plo);
            return;
        }
        case 0x70:  // a proxy's instance released (flag 1: its row unlocked first)
        {
            void* pproxy = s_phost->PloFind(4, A(c, 0), p->ploContext);
            if (!pproxy || !s_phost->FDerivedFrom(pproxy, 0x08)) return;
            void* plo = s_phost->PloProxyInstance(pproxy);
            if (!plo) return;
            if (A(c, 1) & 1) s_phost->UnlockProxyRow(pproxy, plo);
            s_phost->ReleaseProxyInstance(pproxy, plo);
            return;
        }
        case 0x77:  // the actor (cid 0x32) faces an object
            if (p->plo28 && s_phost->FDerivedFrom(p->plo28, 0x32))
                if (void* plo = PloFind(p, A(c, 0))) s_phost->FaceObject(p->plo28, plo);
            return;
        // the waits (0x2293DC): their done tests decide
        case 0x01: case 0x02: case 0x03: case 0x05: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0C: case 0x0D:
        case 0x0E: case 0x20: case 0x27: case 0x45: case 0x4F: case 0x51: case 0x5B: case 0x5F:
            Wait(p);
            return;
        default:
            s_phost->Unported(*p, c);
            return;
        }
    }
}

// orig3: 0x229438 FCmdDone (ids below 0x0C and those without a test: done)
bool FCmdDone(RUN3* p, int icmd)
{
    SCPT3* ps = p->pscpt;
    const SCPTCMD3& c = *Pcmd(ps, icmd);
    switch (c.id)
    {
    case 0x0C:  // until none of the named runners is left
        for (int i = 0; i < A(c, 0); ++i)
            if (PrunFind(ps, A(c, 1 + std::size_t(i)), p)) return false;
        return true;
    case 0x0D:  // a rendezvous: every named runner waits at an 0x0D with the same value (+0x108 marks them)
    {
        if (p->fSynced)
        {
            p->fSynced = 0;
            return true;
        }
        // the others (the list names this run too) must all wait at an 0x0D with the same value, not yet released
        const int n = A(c, 0);
        const int32_t v = A(c, 1 + std::size_t(n));
        std::vector<RUN3*> a;
        for (int i = 0; i < n; ++i)
        {
            const int oid = A(c, 1 + std::size_t(i));
            if (Pcmd(ps, p->icmdRoot)->oid == oid) continue;
            RUN3* q = PrunFind(ps, oid, p);
            const SCPTCMD3* pq = q ? Pcmd(ps, q->icmdCur) : nullptr;
            if (pq && pq->id == 0x0D && A(*pq, 1 + std::size_t(std::max(A(*pq, 0), 0))) == v && !q->fSynced) a.push_back(q);
        }
        if (int(a.size()) != n - 1) return false;
        for (RUN3* q : a) q->fSynced = 1;
        return true;
    }
    case 0x0E:  // until the alarm is set (cleared when the flag says so)
        if (!FHasAlarm(ps, A(c, 0))) return false;
        if (A(c, 1)) RemoveAlarm(ps, A(c, 0));
        return true;
    case 0x2F: return p->gWait < TScpt(ps) - p->tStart;
    case 0x16:  // the actor's script animation ended (a step after: the field cleared first), or the ASEG retracted
        if (p->ploAnim)
        {
            if (!s_phost->FActorInScriptAnim(p->ploAnim)) p->ploAnim = nullptr;
            return false;
        }
        if (p->pasega && s_phost->FAsegaRetracted(p->pasega))
        {
            s_phost->ReleaseAsega(p->pasega);
            p->pasega = nullptr;
        }
        return !p->pasega;
    case 0x1A:
        if (p->ploScreen && s_phost->FScreenObjDone(p->ploScreen))
        {
            s_phost->HideScreenObj(p->ploScreen);  // 0x22B770
            p->ploScreen = nullptr;
        }
        return !p->ploScreen;
    case 0x1D:
    {
        const float t = TScpt(ps) - p->tStart;
        TimeRamp(p, c, t);
        return F(A(c, 5)) <= TScpt(ps) - p->tStart;
    }
    case 0x26:  // the actor's place on its path within the tolerance of the (limited) parameter to reach
    {
        void* pa = p->ploPending;
        if (!pa) return true;
        return std::fabs(s_phost->UActorPath(pa) - s_phost->GLimitActorPath(pa, p->uCrv84)) < F(A(c, 1));
    }
    case 0x28:
        if (!p->plo28 || !s_phost->FDerivedFrom(p->plo28, 0x33)) return true;
        return s_phost->FDone28(p->plo28);
    case 0x30:  // within the radius (in xy) of the object it walks to; then its animation ends
    {
        void* pa = p->ploPending;
        if (!pa) return true;
        bool fDone = true;
        if (void* plo = PloFind(p, A(c, 0)))
        {
            float pos[3], mat[9], t[3];
            GetRunAim(p, plo, pos, nullptr);
            s_phost->GetLoXf(pa, mat, t);
            const float dx = t[0] - pos[0], dy = t[1] - pos[1];
            fDone = std::sqrt(dx * dx + dy * dy) < F(A(c, 2));
            s_phost->SetActorWatchTarget(pa, pos);
        }
        if (fDone)
        {
            s_phost->EndActorScriptAnim(pa);
            p->ploAnim = nullptr;
        }
        return fDone;
    }
    case 0x36: return !p->ploPending || s_phost->ActorMoveState(p->ploPending) == -1;
    case 0x38: case 0x39:
        if (p->ploAddRemove && s_phost->FAddRemoveDone(p->ploAddRemove, c.id == 0x38)) p->ploAddRemove = nullptr;
        return !p->ploAddRemove;
    case 0x3F: return 0.5f < TScpt(ps) - p->tStart;  // (0x344838)
    case 0x4E:  // the buttons pressed (on the job's pad, else the default one)
    {
        void* ppad = nullptr;
        if (A(c, 1) != -1)
            if (void* plo = s_phost->PloFind(5, A(c, 1), nullptr); plo && s_phost->FDerivedFrom(plo, 0x31))
                ppad = s_phost->PpadOfJob(plo);
        const bool f = s_phost->FButtonPressed(ppad, A(c, 0));
        if (f) s_phost->SetButtonPrompt(0);
        return f;
    }
    case 0x62:
        if (p->ploGuardCmds && !s_phost->FGuardRunsCommands(p->ploGuard, p->ploGuardCmds))
            p->ploGuard = p->ploGuardCmds = nullptr;
        return !p->ploGuardCmds;
    case 0x45:  // a camera key: when the previous move ends, the move to the next keys starts (0x22A500, 0x22BB78)
    {
        if (FRunWaiting(p)) return false;
        p->tWake = c_tNever;
        if (A(c, 9) & 0x60)
        {
            s_phost->EndRunCamera(*p);
            return true;
        }
        int inext = -1, inext2 = -1;
        for (int i = ISib(ps, icmd); i >= 0; i = ISib(ps, i))
            if (Pcmd(ps, i)->id == 0x45)
            {
                if (inext < 0) inext = i;
                else
                {
                    inext2 = i;
                    break;
                }
            }
        s_phost->StartRunCamera(*p, c, Pcmd(ps, inext), Pcmd(ps, inext2));
        p->tWake = TScpt(ps) + F(A(c, 7)) + F(A(c, 8));  // 0x22BB78: T + key +0x24 + key +0x50
        return true;
    }
    case 0x46: case 0x4D:  // the binocucom is open (state 1)
        if (void* pa = PloBinoc(p)) return s_phost->BinocState(pa) == 1;
        return true;
    case 0x47:  // closed (state -1)
        if (void* pa = PloBinoc(p)) return s_phost->BinocState(pa) == -1;
        return true;
    case 0x48: case 0x49: case 0x4B:
        if (void* pa = PloBinoc(p)) return s_phost->FCmDone(pa, c.id);
        return true;
    case 0x50: return !s_phost->FTitleShowing();
    case 0x52: return !s_phost->FDialogPlaying(A(c, 1));  // the first line
    case 0x5F: return !s_phost->FDialogPlaying(-1);
    default: return true;
    }
}

// orig3: 0x226700 FinishRunPending (the pending command's done test; passed: released, except 0x29 with an oid)
void FinishRunPending(RUN3* p)
{
    if (p->icmdPending < 0) return;
    const int icmd = p->icmdPending;
    s_phost->Testing(*p, icmd);
    const bool fDone = FCmdDone(p, icmd);
    s_phost->DoneTested(*p, icmd, fDone);
    if (!fDone) return;
    const SCPTCMD3& c = *Pcmd(p->pscpt, icmd);
    if (c.id == 0x29 && A(c, 0) != -1) return;
    SetRunPending(p, -1, nullptr);
}

// orig3: 0x2265E0 RunRun
void RunRun(RUN3* p)
{
    DEPTH depth;
    if (p->icmdCur < 0 && p->tWake == c_tNever)
    {
        p->icmdCur = p->icmdRoot;
        goto exec;
    }
    for (;;)
    {
        if (p->icmdCur < 0) break;
        if (Pcmd(p->pscpt, p->icmdCur)->fFlag)
        {
            s_phost->Testing(*p, p->icmdCur);
            const bool fDone = FCmdDone(p, p->icmdCur);
            s_phost->DoneTested(*p, p->icmdCur, fDone);
            if (!fDone) break;
        }
        FinishRunPending(p);
    exec:
        Exec(p);
        if (FRunRetired(p)) return;
    }
    FinishRunPending(p);
    if (p->icmdCur < 0 && !FRunWaiting(p)) RetireRun(p);
}
}  // namespace

void SCPTHOST3::Unported(RUN3&, const SCPTCMD3& cmd)
{
    static std::set<int> s_setid;
    if (s_setid.insert(cmd.id).second)
        std::fprintf(stderr, "scpt3: command 0x%02X (%s) not ported\n", cmd.id, SzScptCmd3(cmd.id));
}

void SetScptHost3(SCPTHOST3* phost) { s_phost = phost ? phost : &s_hostDefault; }

void SetScptAlarm3(SCPT3* ps, int n, bool fSet) { (fSet ? EnsureAlarm : RemoveAlarm)(ps, n); }
bool FScptAlarm3(const SCPT3* ps, int n) { return FHasAlarm(ps, n); }
SCPTHOST3& ScptHost3() { return *s_phost; }

bool FScptRunning3(const SCPT3* ps) { return ps->fRunning; }

// orig3: 0x225EE8
void StartScpt3(SCPT3* ps, void* ploStarter)
{
    if (ps->fRunning) return;
    DEPTH depth;
    ps->ploStarter = ploStarter;
    ps->fRunning = true;
    ps->fF2 = false;
    s_phost->ScriptStarted(ps);
    if (!ps->fRunning || ps->acmd.empty()) return;
    RunRun(PrunNew(ps, 0, nullptr));
    if (!ps->fRunning) return;
    if (ps->arun.empty())
    {
        StopScpt3(ps);
        return;
    }
    if (!(ps->grf & 2))
    {
        // 0x1C9118(scpt, 1): (the script joins the update list)
        ps->grf |= 2;
        ps->fF2 = true;
    }
}

// orig3: 0x226050
void StopScpt3(SCPT3* ps)
{
    if (!ps->fRunning) return;
    DEPTH depth;
    ps->ploStarter = nullptr;
    ps->tElapsed = 0;
    ps->fRunning = false;
    while (!ps->arun.empty()) RetireRun(ps->arun.front().get());
    // (not ported: 0x2263B0(scpt, 1.0), 0x2262B0(scpt))
    if (ps->fF2)
    {
        // 0x1C9118(scpt, 2)
        ps->grf &= ~2u;
    }
    s_phost->ScriptStopped(ps);
}

// orig3: 0x225130
void UpdateScpt3(SCPT3* ps, float dt)
{
    if (!ps->fRunning) return;
    DEPTH depth;
    ps->tElapsed += dt;
    std::vector<RUN3*> a;
    for (auto& up : ps->arun) a.push_back(up.get());
    for (RUN3* p : a)
        if (!p->fRetired) RunRun(p);
    if (ps->arun.empty()) StopScpt3(ps);
}
