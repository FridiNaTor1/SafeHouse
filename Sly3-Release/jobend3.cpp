// See jobend3.h. Ported from the Sly 3 asm (orig3 0x1871C0, 0x186EE0, 0x186F90, 0x1878E0, 0x1875B0, 0x186930 /
// 0x186B68 the stamp's parts).

#include "jobend3.h"

#include "challenge3.h"

#include "ai.h"
#include "card3.h"
#include "clock.h"
#include "frontend.h"
#include "lo.h"
#include "menu.h"
#include "region.h"
#include "screen3.h"
#include "screenrt.h"
#include "sw.h"
#include "game/task3.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

constinit JOBENDHOOKS3 g_jobendhooks3{};
extern int g_iframeMain;   // (game.cpp: the flow log's frame)

namespace
{
// (each job keeps its end state: JOB3 nEnd (+0xB8), tEnd (+0xBC), pscptFail (+0xC4), idFail (+0xC8); round 8 had one
// machine for every job, and a second job's end during the first one's stamp was dropped)
SW* s_psw = nullptr;

constexpr int OID_RESULT = 1066, OID_FAILED = 1067;   // SW +0x433C / *0x4797C0 (0x25CD00)

LO* PloResult() { return s_psw ? PloScreen3(s_psw, OID_RESULT) : nullptr; }
LO* PloFailed() { return s_psw ? PloScreen3(s_psw, OID_FAILED) : nullptr; }

// the job's character mask (MISSION +0xB0: 1 Sly, 2 Bentley, 4 Murray, 8 .., 0x40, 0x80; 0 none, e.g. the prologue's
// 1797; Venice's m1_recon 2085: 1)
int GrfCharacter(const JOB3* p)
{
    if (const char* sz = std::getenv("SLY3_JOBCHAR")) return int(std::strtol(sz, nullptr, 0));
    return p && p->plo ? int(p->plo->U32Field(0, 0xB0, 0)) : 0;
}

// orig3: 0x00186930 / 0x00186B68 (the job's character): the stamp's in / out part: a job's "JOB COMPLETE!" 0x42C + 2 k
// (another mask 0x43C), a challenge's result 0x43E + 2 k (0x44E; the manager's mode 0, challenge3.h), a two-player
// game's 0x450 / 0x452 / 0x454 (Sly / Bentley / Carmelita: 1, 2, 0x40; else 0x450)
int IdStampPart(const JOB3* p, bool fOut)
{
    static const int s_amask[8] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80};
    const int grf = GrfCharacter(p);
    if (NChallengeMode3() == 2) return (grf == 2 ? 0x452 : grf == 0x40 ? 0x454 : 0x450) + (fOut ? 1 : 0);
    int k = 8;   // (another mask: 0x43C / 0x43D)
    for (int i = 0; i < 8; ++i)
        if (grf == s_amask[i]) k = i;
    return (NChallengeMode3() == 0 ? 0x43E : 0x42C) + 2 * k + (fOut ? 1 : 0);
}

bool FInputTaken() { return FMenuShown() || FCardMessageShown3() || FTitleShown(); }

// orig3: 0x001878E0
void SetJobEndState(JOB3* p, int s)
{
    if (p->nEnd == s) return;
    if (p->nEnd == 0)
    {
        // (+0xB4: the job's exit task found from its last task's successors; 0x1C9118(job, 1); the flags 0x202)
        PauseGame(2);   // (0x1A93C8(SW, -1, +0x74 ? 0 : 2), 0x1A94A8)
    }
    p->nEnd = s;
    p->tEnd = g_clock.t3C;   // orig3 0x36BBD8 (Sly 3 CLOCK +0x40: the time that runs while the game is paused)
    if (std::getenv("SLY3_JOBENDPRINT")) std::printf("jobend3: job %d state %d (frame %d)\n", p ? p->oid : -1, s, g_iframeMain);
    switch (s)
    {
    case 0:
        p->pscptFail = nullptr;
        if (LO* plo = PloFailed()) HideScreenObj(plo);
        if (LO* plo = PloResult()) HideScreenObj(plo);
        p->idFail = -1;
        ResumeGame(2);   // (0x1A9420, 0x1A94F0)
        break;
    case 1:   // the stamp; a challenge's / two-player game's result evaluated and its record kept (0x2846A8 / 0x284778)
        if (LO* plo = PloResult())
        {
            ShowScreenObj(plo, -1);   // (vt slot 56)
            SelectScreenObjPart(plo, IdStampPart(p, false));
        }
        EvaluateChallenge3();
        SaveChallengeBest3();
        break;
    case 2:
        if (LO* plo = PloResult()) SelectScreenObjPart(plo, IdStampPart(p, true));
        break;
    case 5:
        if (NChallengeMode3() == 0)
        {
            // a challenge failed: its result on 1066 ("Challenge Failed", the line; 0x284658: the manager's state 3)
            if (LO* plo = PloResult())
            {
                ShowScreenObj(plo, -1);
                SelectScreenObjPart(plo, IdStampPart(p, false));
            }
            ChallengeJobFailed3();
        }
        else if (LO* plo = PloFailed()) ShowScreenObj(plo, -1);   // Job Failed!
        break;
    default: break;
    }
}
}  // namespace

void JobEndComplete3(JOB3* p)
{
    if (!p || p->nEnd != 0) return;
    if (GrfCharacter(p)) { SetJobEndState(p, 1); return; }
    // (no character: a game of the chooser's lists ends at once (0x2846A8, 0x284778, 0x283F70), else the rewards and the
    // close now)
    if (FChallengeRunning3())
    {
        EvaluateChallenge3();
        SaveChallengeBest3();
        if (s_psw) EndChallenge3(s_psw);
        return;
    }
    if (g_jobendhooks3.pfnRewards) g_jobendhooks3.pfnRewards(p);
    if (g_jobendhooks3.pfnCompleteClose) g_jobendhooks3.pfnCompleteClose(p);
}

// orig3: 0x186F90: with a failure line (+0xC8) or a challenge running (the manager's mode 0), Job Failed (5), else the
// restart at once
void JobFailPrompt(JOB3* p)
{
    if (p->idFail != -1 || NChallengeMode3() == 0) SetJobEndState(p, 5);
    else
    {
        if (g_jobendhooks3.pfnRestart) g_jobendhooks3.pfnRestart(p);
        SetJobEndState(p, 8);
    }
}

void JobEndFail3(JOB3* p, int idText, LO* pscpt)
{
    if (!p || p->nEnd != 0) return;
    // orig3 0x186EE0: state 9, the line (+0xC8), the failing object (+0xC4), the broadcast 0x20; a failing script:
    // state 4 (0x226010 starts it with the job), else 0x186F90
    SetJobEndState(p, 9);
    p->idFail = idText;
    p->pscptFail = pscpt;
    Broadcast3(0x20, p->plo);
    if (p->pscptFail)
    {
        SetJobEndState(p, 4);
        if (g_jobendhooks3.pfnStartScpt) g_jobendhooks3.pfnStartScpt(p->pscptFail, p);
    }
    else JobFailPrompt(p);
}

void JobEndQuit3(JOB3* p)
{
    if (!p || p->nEnd != 0) return;
    SetJobEndState(p, 8);
}

int NJobEndState3(const JOB3* p) { return p ? p->nEnd : 0; }
// (the screen's line: the job showing Job Failed! (5 / 7))
int IdJobFailText3()
{
    for (auto& pj : g_tasks3.ajob)
        if (pj->nEnd == 5 || pj->nEnd == 7) return pj->idFail;
    return -1;
}

void ResetJobEnd3()
{
    for (auto& pj : g_tasks3.ajob)
    {
        pj->nEnd = 0;
        pj->idFail = -1;
        pj->pscptFail = nullptr;
    }
}

void UpdateJobEndOf(JOB3* p);
void UpdateJobEnd3(SW* psw)
{
    s_psw = psw;
    // (debug) SLY3_JOBEND=frame:c|f (the current job, else the world's first) completes / fails at that frame
    if (static const char* s_sz = std::getenv("SLY3_JOBEND"); s_sz)
    {
        static int s_c = 0;
        if (++s_c == std::atoi(s_sz))
        {
            JOB3* pj = g_tasks3.pjobCur ? g_tasks3.pjobCur : (!g_tasks3.ajob.empty() ? g_tasks3.ajob.front().get() : nullptr);
            if (const char* pch = std::strchr(s_sz, ':'); pch && pch[1] == 'f') JobEndFail3(pj, 4801);
            else JobEndComplete3(pj);
        }
    }
    for (auto& upj : g_tasks3.ajob) UpdateJobEndOf(upj.get());
}

// orig3: 0x1875B0 (one job's end)
void UpdateJobEndOf(JOB3* p)
{
    if (!p || p->nEnd == 0) return;
    switch (p->nEnd)
    {
    case 1:
        if (LO* plo = PloResult(); plo && FScreenObjBusy(plo, IdStampPart(p, false))) return;
        // a challenge / a two-player game: its result waits for Square / Triangle (3)
        if (NChallengeMode3() == 0 || NChallengeMode3() == 2)
        {
            SetJobEndState(p, 3);
            break;
        }
        // orig3: with an exit task (+0xB4), the job's exit (+0xC0), 0x1C6F20 on 0x370AF0 or a job replay (mode 1) the
        // rewards and the close at once (8: the world changes), else the stamp's out part first (2)
        if (PtaskExitOfJob3(p) || p->ploExit || NChallengeMode3() == 1)
        {
            if (g_jobendhooks3.pfnRewards) g_jobendhooks3.pfnRewards(p);
            if (g_jobendhooks3.pfnCompleteClose) g_jobendhooks3.pfnCompleteClose(p);
            SetJobEndState(p, 8);
            break;
        }
        if (g_jobendhooks3.pfnRewards) g_jobendhooks3.pfnRewards(p);
        SetJobEndState(p, 2);
        break;
    case 2:
        if (LO* plo = PloResult(); plo && FScreenObjBusy(plo, IdStampPart(p, true))) return;
        if (g_jobendhooks3.pfnCompleteClose) g_jobendhooks3.pfnCompleteClose(p);
        SetJobEndState(p, 0);
        break;
    case 4:   // orig3 0x1875B0: the failing script ran out
        if (p->pscptFail && g_jobendhooks3.pfnFScptRunning && g_jobendhooks3.pfnFScptRunning(p->pscptFail)) return;
        JobFailPrompt(p);
        break;
    case 3:   // a challenge's result: Square replays it (0x186FE8), Triangle returns to the chooser (0x283F70)
        if (FInputTaken()) return;
        if (GrfbtnTakePressed(0x80))
        {
            if (g_jobendhooks3.pfnRestart) g_jobendhooks3.pfnRestart(p);
            SetJobEndState(p, 8);
        }
        else if (GrfbtnTakePressed(0x10))
        {
            if (s_psw) EndChallenge3(s_psw);
            SetJobEndState(p, 8);
        }
        break;
    case 5:
        if (NChallengeMode3() == 0)
        {
            if (LO* plo = PloResult(); plo && FScreenObjBusy(plo, IdStampPart(p, false))) return;
            SetJobEndState(p, 3);
            break;
        }
        if (LO* plo = PloFailed(); plo && FScreenObjBusy(plo, sly3ui::PART_SHOW)) return;
        SetJobEndState(p, 7);
        break;
    case 7:   // "Press X to try again"
        if (g_clock.t3C - p->tEnd <= 0.5f || FInputTaken()) return;   // (the paused game: the unpaused time)
        if (!GrfbtnTakePressed(BtnMenuConfirm())) return;
        if (g_jobendhooks3.pfnRestart) g_jobendhooks3.pfnRestart(p);
        SetJobEndState(p, 8);
        break;
    default: break;   // (8: the restart's world start resets it: 0x1878A0)
    }
}
