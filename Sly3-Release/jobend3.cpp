// See jobend3.h. Ported from the Sly 3 asm (orig3 0x1871C0, 0x186EE0, 0x186F90, 0x1878E0, 0x1875B0, 0x186930 /
// 0x186B68 the stamp's parts).

#include "jobend3.h"

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

namespace
{
JOB3* s_pjob = nullptr;     // the job ending (its +0xB8 below)
int s_state = 0;            // MISSION +0xB8
float s_tState = 0.0f;      // +0xBC
int s_idFail = -1;          // +0xC8
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

// orig3: 0x00186930 / 0x00186B68 (one player: the job's character): the stamp's in / out part
int IdStampPart(const JOB3* p, bool fOut)
{
    static const int s_amask[8] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80};
    const int grf = GrfCharacter(p);
    int k = 8;   // (another mask: 0x43C / 0x43D)
    for (int i = 0; i < 8; ++i)
        if (grf == s_amask[i]) k = i;
    return 0x42C + 2 * k + (fOut ? 1 : 0);
}

bool FInputTaken() { return FMenuShown() || FCardMessageShown3() || FTitleShown(); }

// orig3: 0x001878E0
void SetJobEndState(JOB3* p, int s)
{
    if (s_state == s) return;
    if (s_state == 0)
    {
        // (+0xB4: the job's exit task found from its last task's successors; 0x1C9118(job, 1); the flags 0x202)
        PauseGame(2);   // (0x1A93C8(SW, -1, +0x74 ? 0 : 2), 0x1A94A8)
    }
    s_state = s;
    s_tState = g_clock.t;
    if (std::getenv("SLY3_JOBENDPRINT")) std::printf("jobend3: job %d state %d\n", p ? p->oid : -1, s);
    switch (s)
    {
    case 0:
        if (LO* plo = PloFailed()) HideScreenObj(plo);
        if (LO* plo = PloResult()) HideScreenObj(plo);
        s_idFail = -1;
        ResumeGame(2);   // (0x1A9420, 0x1A94F0)
        s_pjob = nullptr;
        break;
    case 1:   // the stamp
        if (LO* plo = PloResult())
        {
            ShowScreenObj(plo, -1);   // (vt slot 56)
            SelectScreenObjPart(plo, IdStampPart(p, false));
        }
        break;
    case 2:
        if (LO* plo = PloResult()) SelectScreenObjPart(plo, IdStampPart(p, true));
        break;
    case 5:   // Job Failed! (no challenge running)
        if (LO* plo = PloFailed()) ShowScreenObj(plo, -1);
        break;
    default: break;
    }
}
}  // namespace

void JobEndComplete3(JOB3* p)
{
    if (!p || s_state != 0) return;
    s_pjob = p;
    if (GrfCharacter(p)) { SetJobEndState(p, 1); return; }
    // (no character: the challenge's end when one runs (0x2852B0..: TODO(sly3)), else the rewards and the close now)
    if (g_jobendhooks3.pfnRewards) g_jobendhooks3.pfnRewards(p);
    if (g_jobendhooks3.pfnCompleteClose) g_jobendhooks3.pfnCompleteClose(p);
    s_pjob = nullptr;
}

void JobEndFail3(JOB3* p, int idText)
{
    if (!p || s_state != 0) return;
    s_pjob = p;
    s_idFail = idText;
    // orig3 0x186EE0: (state 9, +0xC4 the failing object, the broadcast 0x20, a failing script: state 4); 0x186F90:
    // with a failure line (+0xC8) Job Failed (5), else the restart at once
    if (idText != -1) SetJobEndState(p, 5);
    else
    {
        if (g_jobendhooks3.pfnRestart) g_jobendhooks3.pfnRestart(p);
        SetJobEndState(p, 8);
    }
}

int NJobEndState3(const JOB3* p) { return p && p == s_pjob ? s_state : 0; }
int IdJobFailText3() { return s_idFail; }

void ResetJobEnd3()
{
    s_pjob = nullptr;
    s_state = 0;
    s_idFail = -1;
}

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
    JOB3* p = s_pjob;
    if (!p || s_state == 0) return;
    switch (s_state)
    {
    case 1:
        if (LO* plo = PloResult(); plo && FScreenObjBusy(plo, IdStampPart(p, false))) return;
        if (g_jobendhooks3.pfnRewards) g_jobendhooks3.pfnRewards(p);
        // (with an exit task (+0xB4), +0xC0, or 0x1C6F20 on 0x370AF0 the close comes at once (8); else the stamp
        // goes first (2): TODO(sly3): +0xB4 / +0xC0)
        SetJobEndState(p, 2);
        break;
    case 2:
        if (LO* plo = PloResult(); plo && FScreenObjBusy(plo, IdStampPart(p, true))) return;
        if (g_jobendhooks3.pfnCompleteClose) g_jobendhooks3.pfnCompleteClose(p);
        SetJobEndState(p, 0);
        break;
    case 5:
        if (LO* plo = PloFailed(); plo && FScreenObjBusy(plo, sly3ui::PART_SHOW)) return;
        SetJobEndState(p, 7);
        break;
    case 7:   // "Press X to try again"
        if (g_clock.t - s_tState <= 0.5f || FInputTaken()) return;
        if (!GrfbtnTakePressed(BtnMenuConfirm())) return;
        if (g_jobendhooks3.pfnRestart) g_jobendhooks3.pfnRestart(p);
        SetJobEndState(p, 8);
        break;
    default: break;   // (8: the restart's world start resets it: 0x1878A0)
    }
}
