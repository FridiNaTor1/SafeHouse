// Sly 3's DIRECTOR (class 0x46, an SO: the heist's dance, Episode 1's SCPT 2464): the script commands 0x57 / 0x58, the
// update (the cues on the song's clock, the pad window, the scoring and the meter), its options. Ported from the Sly 3
// decomp: init 0x209EB0, start 0x209FA0, update 0x209FD0, input 0x20A930, mode 0x20A6D0, timeline 0x20A758, cue queue
// 0x20A880, result 0x20AE20, score 0x20AC40 / 0x20AD58 / 0x20ABA0, meter reset 0x20ADE0 (method 1530). The timeline's
// draw (vt slot 21 0x20A200) is s3-decomp's (HUD): director3.h gives it the state.

#include "director3.h"
#include "scptgame3.h"
#include "task3.h"

#include "../ai.h"
#include "../clock.h"
#include "../frontend.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

void PauseGame(int grf);   // ai.cpp (orig3 0x1A93C8 with flags)
void ResumeGame(int grf);  // ai.cpp (orig3 0x1A9420)

namespace
{
std::map<const LO*, DIRECTOR3> s_mpdir;
DIRECTOR3* s_pdirCur = nullptr;   // D_00468D04 (the director whose init ran last)
float s_tStream = 0.0f;           // the clock's start: the last stream (music / dialog) started

bool FPrint() { static const bool s_f = std::getenv("SLY3_DIRECTORPRINT") != nullptr; return s_f; }

// orig3: 0x2483D8(D_00474FF0): the position of the last stream of the sound manager (1/294912000 s)
float TStream() { return g_clock.t - s_tStream; }

float GClampLm(float lo, float hi, float g) { return g < lo ? lo : hi < g ? hi : g; }

// orig3: 0x20AE20: the result {kind, button, button pressed} and the director's own message 32 (its splice handler)
void Result(DIRECTOR3& d, int kind, int b, int b2)
{
    d.nResult = kind;
    d.btnResult = b;
    d.btnResult2 = b2;
    if (FPrint()) std::printf("director3: %d result %d button 0x%X / 0x%X (t %.3f)\n", d.plo->oid, kind, b, b2, TStream());
    HandleLoSpliceEvent(d.plo, 32, false, 0, nullptr);
}

// orig3: 0x20AD58: the mean of the last +0x468 scores, clamped to +0x41C
float GMeter(const DIRECTOR3& d)
{
    if (d.cAvg <= 0) return 0.0f;
    float sum = 0.0f;
    const int iMin = std::max(d.cScore - d.cAvg, 0);
    for (int i = d.cScore - 1; i >= iMin; --i) sum += d.agScore[i];
    return GClampLm(d.lmMin, d.lmMax, sum / float(d.cAvg));
}

// orig3: 0x20AC40: a score (a penalty scaled by the difficulty polynomial 0x325920 of 0x171C98: the difficulty's t
// natively 0), into the 16-entry history, the meter's target, the broadcast 0x49
void Score(DIRECTOR3& d, float g)
{
    if (g < 0.0f)
    {
        const float t = 0.0f;   // (0x171C98(D_0036BD20): TODO(sly3) the difficulty)
        g *= GClampLm(0.0f, 1.0f, 1.2f + t * (-0.6f + t * 0.0f));
    }
    if (d.cScore == 16)
    {
        std::rotate(d.agScore, d.agScore + 1, d.agScore + 16);
        d.agScore[15] = GClampLm(d.lmMin, d.lmMax, g);
    }
    else d.agScore[d.cScore++] = GClampLm(d.lmMin, d.lmMax, g);
    d.gMeterTarget = GMeter(d);
    Broadcast3(0x49, d.plo);
}

// orig3: 0x20ABA0: the timing's score (1 on time, falling to 0 at the window's edges: 0x341FF0 = {0, 1})
float GTiming(const DIRECTOR3& d, float tCue)
{
    const float now = TStream();
    const float dd = now < tCue ? (tCue - now) / d.gEarly : (now - tCue) / d.gLate;
    return 1.0f - dd;
}

// orig3: 0x20ADF0: the cue after i (null: none)
DIRECTOR3::CUE* PcueNext(DIRECTOR3& d, int i) { return i + 1 < d.ccue ? &d.acue[i + 1] : nullptr; }

// the pad (the owner +0x134's vt slot 1 with 0xF0, then 0x1A7A80's button test): natively the joystick's face buttons;
// SLY3_DIRECTORAUTO=1 (tests): the cue's button at its time
uint32_t GrfbtnInput(DIRECTOR3& d)
{
    if (static const bool s_fAuto = std::getenv("SLY3_DIRECTORAUTO") != nullptr; s_fAuto)
    {
        const float t = TStream();
        for (int i = 0; i < d.ccue; ++i)
            if (!d.acue[i].fDone && !d.acue[i].fPressed && std::fabs(t - d.acue[i].t) < 0.05f) return uint32_t(d.acue[i].btn);
        return 0;
    }
    return GrfbtnTakePressed(0xF0);
}

// orig3: 0x20A930 (mode 2: a press)
void Input(DIRECTOR3& d)
{
    d.btnLast = int(GrfbtnInput(d));
    if (!d.btnLast) return;
    const float t = TStream();
    for (int i = 0; i < d.ccue; ++i)
    {
        DIRECTOR3::CUE* e = &d.acue[i];
        if (e->fDone) continue;
        Broadcast3(0x48, d.plo);
        if (t < e->t - d.gEarly)  // too early
        {
            Result(d, 2, e->btn, d.btnLast);
            Score(d, d.gPenEarly);
            break;
        }
        if (e->fPressed)  // pressed twice: the next cue when it is near, else a penalty
        {
            DIRECTOR3::CUE* next = PcueNext(d, i);
            if (next && next->t - d.gEarly < t)
            {
                e->fDone = true;
                e = next;
            }
            else
            {
                Result(d, 3, e->btn, d.btnLast);
                Score(d, d.gPenSecond);
                break;
            }
        }
        e->fPressed = true;
        if (uint32_t(d.btnLast) & uint32_t(e->btn))
        {
            Result(d, 4, e->btn, d.btnLast);
            Score(d, GTiming(d, e->t));
            for (int j = 0; j < int(d.aitem.size()); ++j)
                if (std::fabs(d.aitem[std::size_t(j)].t - e->t) < 0.1f)
                {
                    d.aitem[std::size_t(j)].fHit = true;
                    break;
                }
        }
        else
        {
            Result(d, 5, e->btn, d.btnLast);
            Score(d, d.gPenWrong);
        }
        break;
    }
}

// orig3: 0x209FD0 (vt slot 18)
void Update(DIRECTOR3& d, float dt)
{
    const float t = TStream();
    switch (d.nMode)
    {
    case 1:  // the demonstration: each cue's prompt when its time passes
        for (int j = 0; j < d.ccue; ++j)
        {
            DIRECTOR3::CUE& e = d.acue[j];
            if (e.fShown) continue;
            if (e.t < t)
            {
                Result(d, 0, e.btn, e.btn);
                e.fShown = true;
                break;
            }
        }
        break;
    case 2:  // the player's turn
        Input(d);
        for (int i = 0; i < d.ccue; ++i)
        {
            DIRECTOR3::CUE& e = d.acue[i];
            if (e.fDone) continue;
            if (!e.fReached && e.t < t)
            {
                d.btnLast = e.btn;
                Broadcast3(0x47, d.plo);
                e.fReached = true;
            }
            DIRECTOR3::CUE* next = PcueNext(d, i);
            if (e.fPressed && next && next->t - d.gEarly < t)
            {
                e.fDone = true;
                continue;
            }
            if (e.t + d.gLate < t || (next && next->t < t))
            {
                e.fDone = true;
                if (!e.fPressed)
                {
                    Result(d, 1, e.btn, -1);
                    Score(d, d.gPenMiss);
                }
                break;
            }
        }
        break;
    default: break;
    }
    // the meter shown follows its target (0x11E598 with the SMP +0x474: natively a plain approach)
    const float k = std::min(1.0f, dt * 5.0f);
    d.gMeter += (d.gMeterTarget - d.gMeter) * k;
}

// orig3: 0x209EB0's defaults, then the loaded options (EOPIDs 1519..1527)
DIRECTOR3& Dir(LO* plo)
{
    auto it = s_mpdir.find(plo);
    if (it != s_mpdir.end()) return it->second;
    DIRECTOR3& d = s_mpdir[plo];
    d.plo = plo;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        switch (o.eopid)
        {
        case 1519: d.gEarly = o.aval[0].ag[0]; break;
        case 1520: d.gLate = o.aval[0].ag[0]; break;
        case 1521: d.lmMin = o.aval[0].ag[0]; d.lmMax = o.aval[0].ag[1]; break;
        case 1522: d.cAvg = o.aval[0].n; break;
        case 1524: d.gPenEarly = o.aval[0].ag[0]; break;
        case 1525: d.gPenWrong = o.aval[0].ag[0]; break;
        case 1526: d.gPenSecond = o.aval[0].ag[0]; break;
        case 1527: d.gPenMiss = o.aval[0].ag[0]; break;
        default: break;
        }
    }
    s_pdirCur = &d;
    return d;
}

// orig3: 0x20A6D0 (option 1528's setter too)
void SetMode(DIRECTOR3& d, int n)
{
    if (d.nMode == n) return;
    if (d.nMode == 1) ResumeGame(3);
    d.nMode = n;
    d.tMode = g_clock.t;
    d.ccue = 0;
    if (n == 1) PauseGame(3);
    if (FPrint()) std::printf("director3: %d mode %d (t %.3f)\n", d.plo->oid, n, TStream());
}

CRef RefS32(int n) { CRef r; r.SetS32(n); return r; }
CRef RefF32(float g) { CRef r; r.SetF32(g); return r; }

struct INITDIRECTOR3
{
    INITDIRECTOR3()
    {
        // 1528 the mode (+0x190: getter, setter 0x20A6D0), 1529 the meter (+0x46C), 1530 the meter reset (0x10EE68 ->
        // 0x20ADE0), 1531..1533 the last result {kind +0x490, button +0x494, button pressed +0x498}
        RegisterObjectGetter3(1528, [](LO* plo) { return RefS32(Dir(plo).nMode); });
        RegisterObjectSetter3(1528, [](LO* plo, const CRef& r) { SetMode(Dir(plo), r.RefCoerceS32().m_n); });
        RegisterObjectGetter3(1529, [](LO* plo) { return RefF32(Dir(plo).gMeterTarget); });
        RegisterObjectMethod3(0x10EE68, [](LO* plo, const std::vector<CRef>&) {
            DIRECTOR3& d = Dir(plo);
            d.cScore = 0;
            d.gMeterTarget = 0.0f;
            d.gMeter = 0.0f;
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter3(1531, [](LO* plo) { return RefS32(Dir(plo).nResult); });
        RegisterObjectGetter3(1532, [](LO* plo) { return RefS32(Dir(plo).btnResult); });
        RegisterObjectGetter3(1533, [](LO* plo) { return RefS32(Dir(plo).btnResult2); });
    }
} s_initdirector3;
}  // namespace

void ResetDirectors3(SW* psw)
{
    s_mpdir.clear();
    s_pdirCur = nullptr;
    if (!psw) return;
    for (LO* plo : psw->mpcidaplo[0x46]) Dir(plo);   // (each one's init 0x209EB0 sets D_00468D04: the last one)
}

void NoteStreamStarted3() { s_tStream = g_clock.t; }
float TDirectorStream3() { return TStream(); }

void DirectorCmd57_3(int nMode, const std::vector<std::pair<float, int>>& aitem)
{
    if (!s_pdirCur) return;
    DIRECTOR3& d = *s_pdirCur;
    // orig3 0x20A758: the timeline (up to 10 items laid out on 440 px from x 100 at y 456: the draw's)
    d.aitem.clear();
    for (const auto& it : aitem)
        if (d.aitem.size() < 10) d.aitem.push_back({it.first, it.second, false});
    SetMode(d, nMode);
}

void DirectorCmd58_3(float t, int btn)
{
    if (!s_pdirCur) return;
    DIRECTOR3& d = *s_pdirCur;
    // orig3 0x20A880: the cue queued (3 at most: the oldest dropped)
    if (d.ccue == 3)
    {
        std::rotate(d.acue, d.acue + 1, d.acue + 3);
        d.ccue = 2;
    }
    DIRECTOR3::CUE& e = d.acue[d.ccue++];
    e = DIRECTOR3::CUE{};
    e.t = t;
    e.btn = btn;
    e.tQueued = TStream();
}

void UpdateDirectors3(float dt)
{
    for (auto& [plo, d] : s_mpdir) Update(d, dt);
}

const DIRECTOR3* PdirectorCur3() { return s_pdirCur; }
