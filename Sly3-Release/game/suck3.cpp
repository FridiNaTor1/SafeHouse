// suck3.cpp: Sly 3's dynamic difficulty (orig3 0x171AA0..0x171E48; game/suck3.h)

#include "suck3.h"

#include "../bif.h"
#include "../challenge3.h"
#include "../savedata.h"
#include "cond3.h"
#include "task3.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
const float s_gDeath = 0.1f, s_gTaskDone = -0.2f;   // D_0036BD20 +0x14: {0, 1, 0.1, -0.2} (the reference's RAM)

bool FPrint() { static const bool s_f = std::getenv("SLY3_SUCKPRINT") != nullptr; return s_f; }

// (the mission: SW +0x4044, its +0xAC the float's place in the save: the progress path of EOPID 1388, <job>.suck_value;
// before s3fix-core2 the native kept it in 1387's, <job>.time_played, which 0x187D30 also writes with the job's time)
JOB3* PjobMission() { return g_tasks3.pjobCur && !g_tasks3.pjobCur->akeySuck.empty() ? g_tasks3.pjobCur : nullptr; }

void AddSuck3(float dg)
{
    SetSuck3(GSuck3() + dg);
}
}  // namespace

float GSuck3()
{
    if (g_nFilterCur3 == 0x16) return 1.0f;
    if (FChallengeRunning3())
    {
        const int n = NChallengeMode3();
        if (n == 0 || n == 2) return 0.0f;
    }
    if (JOB3* pj = PjobMission())
    {
        const CRef r = RefGetProgressKey(pj->akeySuck);
        return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? static_cast<float>(r.m_n) : 0.0f;
    }
    return 0.0f;
}

void SetSuck3(float g)
{
    if (FChallengeRunning3()) return;
    JOB3* pj = PjobMission();
    if (!pj) return;
    CRef r;
    r.SetF32(std::clamp(g, 0.0f, 1.0f));
    FSetProgressKey(pj->akeySuck, r);
    if (FPrint()) std::printf("suck3: mission %d -> %.2f\n", pj->oid, r.m_g);
}

void SuckOnEvent3(int code, LO* plo)
{
    if (FPrint() && (code == 0xC || code == 0x20))
        std::printf("suck3: event 0x%X job %d (survives a death %d) suck %.2f\n", code, g_tasks3.pjobCur ? g_tasks3.pjobCur->oid : -1,
                    g_tasks3.pjobCur ? g_tasks3.pjobCur->f70 : -1, GSuck3());
    switch (code)
    {
    case 0xC:    // (the history's bit 1: not kept natively) only in a mission that survives the death (+0x70): else
                 // the job fails, and its 0x20 adds it (the reference: one death of 1807's player, 0.1)
        if (g_tasks3.pjobCur && g_tasks3.pjobCur->f70) AddSuck3(s_gDeath);
        break;
    case 0x20: AddSuck3(s_gDeath); break;
    case 0x29:
        for (const auto& pt : g_tasks3.atask)
            if (pt->plo == plo && pt->pjob && pt->pjob == g_tasks3.pjobCur && pt->nState == 2 && pt->cDone == 1) AddSuck3(s_gTaskDone);
        break;
    default: break;
    }
}

// (the built-in 107 itself: gamebif.cpp)
