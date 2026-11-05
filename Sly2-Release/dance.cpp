// The dance minigame (Ep2's audition 11:2059 and the heist dance 11:1593): the director (cid 0x79, module
// 0x1526D8..0x153800, D_002DE570) and its dancers (cid 0x44, a STEP: 0x152100..0x152390, 0x1524A0..0x152680), driven
// by the dance script's commands 0x49 (the director's state and the beats' button prompts), 0x4A (a beat: the button
// the dancer must press by a music time; the runner waits until then) and 0x4B (the dancer's default move), all on
// the top music's clock (0x1FE3F8 TMusicTop). A beat pressed in its window plays the dancer's move for that button
// and scores by the timing; a wrong / early / double press or a miss plays the bad move and scores negative; the
// last scores' mean (+0x284, smoothed into +0x288) drives the crowd's state machines (3 levels) and the job's script.
//
// Director (0x79, 0x10B0 bytes): +0x50 state (0 off, 1 the prompts shown and the game paused, 2 playing), +0x54 its
// time, +0x58 / +0x5C the early / late windows (0.4 / 0.4), +0x60 / +0x70 the prompts (6 x 0x30: time, button,
// screen point, done), +0x190 / +0x194 the hint's state / prompt, +0x1A0 / +0x1B0 the hint's point / velocity,
// +0x1C0 / +0x1C4 the beats (3 x 0x24: dancer, button, move, time, t, played, signalled, pressed, over), +0x230 the
// buttons pressed, +0x234 the score range LM (-1.5, 1), +0x23C / +0x240 the scores (16), +0x280 how many make the
// mean (8), +0x284 the mean, +0x288 its smoothed value (SMP +0x28C {range / 3, 0, 5}), +0x298 early (-1.5),
// +0x29C wrong (-0.8), +0x2A0 double (-0.2), +0x2A4 missed (-1.5), +0x2A8 / +0x2AC the crowd (oid, state, SMA,
// level, 3 thresholds).
// Dancer (0x44, 0x410 bytes): +0x2C0 its idle ASEG, +0x2C4 / +0x2C8 its moves (8 x 0x28: button, {ASEG id, ASEG,
// grf} x 3: good, bad, missed), +0x408 its ASEGA, +0x40C busy (messages 0x27 / 0x28).

#include "dance.h"
#include "worldglobals.h"

#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "asega.h"
#include "clock.h"
#include "cls.h"
#include "hud.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "screenobj.h"
#include "smrun.h"
#include "sm.h"
#include "so.h"
#include "sound.h"
#include "splayer.h"
#include "step.h"
#include "sw.h"
#include "task.h"
#include "update.h"
#include "xf.h"
#include "actanim.h"
#include "ai.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

bool FFloatsNear(float g1, float g2, float gEpsilon);                             // xfmath.h (0x11D4D0)
float GRandInRange(float gLow, float gHigh);                                      // xfmath.h (0x11D448)
float GSmooth(float gCur, float gTarget, float dt, const SMP* psmp, float* pdgNext); // xfmath.h (0x11D018)
float GLimitLm(const LM* plm, float g);                                           // util.h (0x11D948)
void EvalHermiteVector(const glm::vec4& p0, const glm::vec4& v0, const glm::vec4& p1, const glm::vec4& v1,
                       glm::vec4* ppos, glm::vec4* pvel, glm::vec4* paccel, float dt, float t, float dtScale);  // 0x117120
float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);  // splayer.cpp

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_DANCEPRINT") != nullptr;  // (debug: the dance's beats and scores)
    return s_f;
}

float FloatField(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t uDefault;
    std::memcpy(&uDefault, &gDefault, 4);
    const uint32_t u = plo->U32Field(0, ib, uDefault);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

struct PROMPT { float t = 0; int k = 0; glm::vec3 pos{0.0f}; int fDone = 0; };
struct BEAT { LO* po = nullptr; int n4 = 0, n8 = 0; float gC = 0, t10 = 0; int n14 = 0, n18 = 0, n1C = 0, n20 = 0; };
struct CROWD { OID oid = -1; int k = -1; SMA* psma = nullptr; int kk = -1; float ag[3] = {}; };
struct DANCE
{
    int state = 0;
    float t54 = 0;
    float g58 = 0.4f, g5C = 0.4f;
    std::vector<PROMPT> aprompt;
    int stateHint = 0, iHint = 0;
    glm::vec3 posHint{0.0f}, velHint{0.0f};
    std::vector<BEAT> abeat;
    int n230 = 0;
    LM lm{-1.5f, 1.0f};
    std::vector<float> ag;   // +0x23C / +0x240 (16)
    int c280 = 8;
    float g284 = 0, g288 = 0;
    SMP smp{};
    float g298 = -1.5f, g29C = -0.8f, g2A0 = -0.2f, g2A4 = -1.5f;
    std::vector<CROWD> acrowd;
    bool fCrowdInit = false;
};
struct MOVE { int k = 0; OID aid[3] = {-1, -1, -1}; LO* apaseg[3] = {}; int agrf[3] = {}; };
struct DANCER
{
    LO* ploIdle = nullptr;   // +0x2C0
    std::vector<MOVE> amove; // +0x2C4 / +0x2C8 (8)
    ASEGA* pasega = nullptr; // +0x408
    int f40C = 0;
    bool fInit = false;
};

std::unordered_map<const LO*, DANCE> s_mpdance;
WORLD_TABLE(s_mpdance);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::unordered_map<const LO*, DANCER> s_mpdancer;
WORLD_TABLE(s_mpdancer);   // (PC: moved aside while a kept world loads, worldglobals.h)
LO* s_ploDance = nullptr;  // D_002DE570
WORLD_VALUE(s_ploDance);   // (PC: moved aside while a kept world loads, worldglobals.h)

DANCE* Pdance(LO* plo)
{
    auto it = s_mpdance.find(plo);
    return it == s_mpdance.end() ? nullptr : &it->second;
}

// orig: 0x001526D8 (vt slot 7: the base 0x1A5270, the global director, the defaults, its update flags 0xA); the loader's
// options may set the fields after it
DANCE& Dance(LO* plo)
{
    auto it = s_mpdance.find(plo);
    if (it != s_mpdance.end()) return it->second;
    DANCE& d = s_mpdance[plo];
    s_ploDance = plo;
    const LO* pfrom = plo->ploCloneOf ? plo->ploCloneOf : plo;
    d.lm.gMin = FloatField(pfrom, 0x234, -1.5f);
    d.lm.gMax = FloatField(pfrom, 0x238, 1.0f);
    d.g58 = FloatField(pfrom, 0x58, 0.4f);
    d.g5C = FloatField(pfrom, 0x5C, 0.4f);
    d.c280 = static_cast<int>(pfrom->U32Field(0, 0x280, 8));
    d.smp.svFast = std::fabs(d.lm.gMax - d.lm.gMin) * 0.33333334f;
    d.smp.svSlow = 0.0f;
    d.smp.dtFast = 5.0f;
    d.g29C = FloatField(pfrom, 0x29C, -0.8f);
    d.g2A0 = FloatField(pfrom, 0x2A0, -0.2f);
    d.g298 = FloatField(pfrom, 0x298, -1.5f);
    d.g2A4 = FloatField(pfrom, 0x2A4, -1.5f);
    if (FPrint())
        std::printf("dance: director %d windows %.2f / %.2f range %.2f..%.2f mean of %d, early %.2f wrong %.2f double %.2f missed %.2f\n", plo->oid,
                    d.g58, d.g5C, d.lm.gMin, d.lm.gMax, d.c280, d.g298, d.g29C, d.g2A0, d.g2A4);
    return d;
}

DANCER& Dancer(LO* plo)
{
    return s_mpdancer[plo];
}


PADCTL* Ppadctl(LO* plo)
{
    PODATA* pd = PpodataFromLo(plo);
    return pd ? pd->ppadctl : nullptr;
}

// orig: 0x00152628 / 0x00152680 (the crowd's states 0x626 / 0x627 / 0x628 <-> the levels 0 / 1 / 2)
int LevelOfState(int k) { return k == 0x626 ? 0 : k == 0x627 ? 1 : k == 0x628 ? 2 : -1; }
int StateOfLevel(int j) { return j == 0 ? 0x626 : j == 1 ? 0x627 : j == 2 ? 0x628 : -1; }

// orig: 0x001524A0 (PlayJtAsegDefault: the ASEG from 0 at speed 1 (grf | 1) into the dancer's +0x408)
void PlayDancerAseg(LO* plo, LO* ploAseg, int grf)
{
    DANCER& r = Dancer(plo);
    ApplyAsegCur(dynamic_cast<ASEG*>(ploAseg), static_cast<ALO*>(plo), 0.0f, 1.0f, grf | 1, &r.pasega);
}

// orig: 0x00152520 (an object of the dancer's by oid (0x15E938 kind 4), out of the world (vt slot 9))
LO* PloDanceAseg(LO* plo, OID oid)
{
    LO* po = PloFindSwNearest(plo, oid);
    if (po && po->fInWorld) RemoveLo(g_psw, po);
    return po;
}

// orig: 0x00152390 (the dancer's move for the button id (-1: any pressed this frame, 0x1C4700), kind j (0 good,
// 1 bad, 2 missed); idNew: the good move replaced first; the records to mode 2)
void PlayDancerMove(LO* plo, int id, int idNew, int j)
{
    DANCER& r = Dancer(plo);
    PADCTL* ppc = Ppadctl(plo);
    for (MOVE& m : r.amove)
    {
        if (!((id == -1 && ppc && GrfPadctlTakePressed(ppc, static_cast<uint32_t>(m.k), 0.0f)) || m.k == id)) continue;
        if (idNew != -1)
            if (LO* po = PloDanceAseg(plo, idNew)) m.apaseg[0] = po;
        ALO* palo = static_cast<ALO*>(plo);
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 2);  // 0x2303A0 (+0x58 / +0x5C)
        if (XF* pxf = PxfRot(palo)) SetXfMode(palo, pxf, 2);
        if (FPrint()) std::printf("dance: %d move button 0x%X kind %d aseg %d t %.3f\n", plo->oid, m.k, j, m.apaseg[j] ? m.apaseg[j]->oid : -1, g_clock.t);
        PlayDancerAseg(plo, m.apaseg[j], m.agrf[j]);
        return;
    }
}

// orig: 0x00153760 (the mean of the last +0x280 scores, in the range)
float GScoreMean(const DANCE& d)
{
    if (d.c280 <= 0) return 0.0f;
    float sum = 0.0f;
    const int c = static_cast<int>(d.ag.size());
    for (int i = c - 1; i >= std::max(c - d.c280, 0); --i) sum += d.ag[static_cast<size_t>(i)];
    return GLimitLm(&d.lm, sum / static_cast<float>(d.c280));
}

// orig: 0x00153640 (a score: a negative one scaled by the dynamic difficulty (D_002BDBE0 {1.2, -0.6, 0} in GSuck,
// clamped to D_002BBB60 {0, 1}); 16 kept; the mean; the task event 0x36)
void AddDanceScore(LO* plo, DANCE& d, float g)
{
    if (g < 0.0f)
    {
        const float t = GSuck();
        g *= std::clamp(1.2f + t * (-0.6f + t * 0.0f), 0.0f, 1.0f);
    }
    if (d.ag.size() == 16) d.ag.erase(d.ag.begin());
    d.ag.push_back(GLimitLm(&d.lm, g));
    d.g284 = GScoreMean(d);
    if (FPrint()) std::printf("dance: score %.3f mean %.3f t %.3f\n", g, d.g284, g_clock.t);
    BroadcastTaskEvent(g_psw, 0x36, plo);  // 0x1619A0
}

// orig: 0x001535A0 (the timing's score: 1 on the beat to 0 at the window's edge, D_002BBB60 {0, 1})
float GBeatScore(const DANCE& d, const BEAT& b)
{
    const float t = TMusicTop();
    const float u = t < b.gC ? (b.gC - t) / d.g58 : (t - b.gC) / d.g5C;
    return 1.0f - u;  // (GLerp(0, 1, 1 - u))
}

// orig: 0x00153128 (the state: leaving 1 resumes the game (0x20F030, 3), entering 1 pauses it (0x20EFC0, 3); the
// beats cleared)
void SetDanceState(DANCE& d, int state)
{
    if (d.state == state) return;
    if (d.state == 1) ResumeGame(3);
    d.state = state;
    d.t54 = g_clock.t;
    d.abeat.clear();
    if (state == 1) PauseGame(3);
}

// orig: 0x00152810 (the world pass: each crowd entry's object (kind 4) runs its SM 0x625 (kind 1) from its state;
// the levels' thresholds random about the range's thirds, the last the minimum)
void InitDanceCrowd(LO* plo, DANCE& d)
{
    const float dd = std::fabs(d.lm.gMax - d.lm.gMin) / 3.0f, h = dd * 0.5f;
    for (CROWD& c : d.acrowd)
    {
        LO* po = PloFindSwNearest(plo, c.oid);
        if (!po) continue;
        LO* psmLo = PloFindSwChild(po, 0x625);
        SM* psm = dynamic_cast<SM*>(psmLo);
        if (!psm) continue;
        ReplaceSmaHandle(psm, static_cast<ALO*>(po), c.k, 0, &c.psma);
        c.kk = LevelOfState(c.k);
        float g = d.lm.gMax;
        for (int j = 0; j < 2; ++j)
        {
            g -= dd;
            c.ag[j] = GRandInRange(g - h, g + h);
        }
        c.ag[2] = d.lm.gMin;
    }
}

// orig: 0x00152B58 (each crowd SMA: retracted: released; else the first level whose threshold the smoothed mean reaches)
void UpdateDanceCrowd(DANCE& d)
{
    for (CROWD& c : d.acrowd)
    {
        if (!c.psma) continue;
        if (FSmaRetracted(c.psma))
        {
            ClearSmaHandle(&c.psma);
            c.k = -1;
            continue;
        }
        for (int j = 0; j < 3; ++j)
            if (c.ag[j] <= d.g288)
            {
                SetSmaGoal(c.psma, StateOfLevel(j));
                c.k = j;
                break;
            }
    }
}

// orig: 0x00152970 (vt slot 16)
void UpdateDance(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1A5E28)) pfn(plo);
    DANCE& d = Dance(plo);
    if (!d.fCrowdInit)
    {
        d.fCrowdInit = true;  // (0x1527C8's world pass 0x152810)
        InitDanceCrowd(plo, d);
    }
    const float t = TMusicTop();
    switch (d.state)
    {
    case 1:
        for (BEAT& b : d.abeat)
        {
            if (b.n14) continue;
            if (b.gC < t)
            {
                PlayDancerMove(b.po, b.n4, b.n8, 0);
                b.n14 = 1;
                break;
            }
        }
        break;
    case 2:
        for (BEAT& b : d.abeat)
        {
            if (b.n20) continue;
            if (!b.n18 && b.gC < t)
            {
                d.n230 = b.n4;
                BroadcastTaskEvent(g_psw, 0x34, plo);
                b.n18 = 1;
            }
            if (b.gC + d.g5C < t)
            {
                b.n20 = 1;
                if (!b.n1C)
                {
                    if (FPrint()) std::printf("dance: beat 0x%X at %.3f missed (t %.3f)\n", b.n4, b.gC, t);
                    PlayDancerMove(b.po, b.n4, b.n8, 2);
                    AddDanceScore(plo, d, d.g2A4);
                }
                break;
            }
        }
        break;
    default: break;
    }
    d.g288 = GSmooth(d.g288, d.g284, g_clock.dt, &d.smp, nullptr);  // 0x11D018
    UpdateDanceCrowd(d);
}

// orig: 0x001533B0 (from the dancer's update while playing: its buttons pressed this frame (0x1C4660, 0xF0); the
// dancer's first beat not over: too early, pressed twice, the right button (scored by the timing; its prompt done)
// or a wrong one)
void CheckDancerBeat(LO* ploDance, DANCE& d, LO* po)
{
    PADCTL* ppc = Ppadctl(po);
    d.n230 = ppc ? static_cast<int>(GrfPadctlPressedWithin(ppc, 0xF0, 0.0f)) : 0;
    if (!d.n230) return;
    for (BEAT& b : d.abeat)
    {
        if (b.po != po || b.n20) continue;
        BroadcastTaskEvent(g_psw, 0x35, ploDance);
        if (TMusicTop() < b.gC - d.g58)
        {
            if (FPrint()) std::printf("dance: beat 0x%X at %.3f too early (t %.3f)\n", b.n4, b.gC, TMusicTop());
            PlayDancerMove(po, -1, -1, 1);
            AddDanceScore(ploDance, d, d.g298);
            return;
        }
        if (b.n1C)
        {
            PlayDancerMove(po, -1, -1, 1);
            AddDanceScore(ploDance, d, d.g2A0);
            return;
        }
        b.n1C = 1;
        if (ppc && GrfPadctlTakePressed(ppc, static_cast<uint32_t>(b.n4), 0.0f))
        {
            if (FPrint()) std::printf("dance: beat 0x%X at %.3f hit (t %.3f)\n", b.n4, b.gC, TMusicTop());
            PlayDancerMove(po, b.n4, b.n8, 0);
            AddDanceScore(ploDance, d, GBeatScore(d, b));  // (0x153638: as is)
            for (PROMPT& pr : d.aprompt)
                if (FFloatsNear(pr.t, b.gC, 0.1f))
                {
                    pr.fDone = 1;
                    break;
                }
        }
        else
        {
            if (FPrint()) std::printf("dance: beat 0x%X at %.3f wrong button 0x%X (t %.3f)\n", b.n4, b.gC, d.n230, TMusicTop());
            PlayDancerMove(po, -1, -1, 1);
            AddDanceScore(ploDance, d, d.g29C);
        }
        return;
    }
}

// orig: 0x00152578 (the moves' ASEGs found: 0x152520) / the idle ASEG (+0x2C0, a field object)
void InitDancer(LO* plo, DANCER& r)
{
    r.fInit = true;
    for (MOVE& m : r.amove)
        for (int j = 0; j < 3; ++j)
            if (m.aid[j] != -1) m.apaseg[j] = PloDanceAseg(plo, m.aid[j]);
    if (ALO* palo = dynamic_cast<ALO*>(plo))
        for (const auto& [ib, p] : palo->apart)
            if (ib == 0x2C0) r.ploIdle = p;
    if (!r.ploIdle)
        if (const OID oid = static_cast<OID>(plo->U32Field(0, 0x2C0, 0xFFFFFFFFu)); oid != -1) r.ploIdle = PloFindSwNearest(plo, oid);
}

// orig: 0x00152148 (vt slot 16: the step's update; no move (or its ASEGA retracted): the records to mode 1, not busy,
// the idle move (grf 8); the director off: a pressed button plays its move; playing: the beat's check; moving: the
// step's velocity the move's root velocity, reversed (0x204E38))
void UpdateDancer(LO* plo)
{
    if (STEP* ps = PstepFromLo(plo)) UpdateStep(ps);
    else if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    DANCER& r = Dancer(plo);
    if (!r.fInit) InitDancer(plo, r);
    if (!r.pasega || FAsegaRetracted(r.pasega))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 1);
        if (XF* pxf = PxfRot(palo)) SetXfMode(palo, pxf, 1);
        r.f40C = 0;
        PlayDancerAseg(plo, r.ploIdle, 8);
    }
    DANCE* pd = s_ploDance ? Pdance(s_ploDance) : nullptr;
    const int k = pd ? pd->state : 0;
    if (k == 0)
    {
        if (!r.f40C) PlayDancerMove(plo, -1, -1, 0);
    }
    else if (k == 2) CheckDancerBeat(s_ploDance, *pd, plo);
    if (r.pasega)
        if (STEP* ps = PstepFromLo(plo))
        {
            glm::vec4 v(0.0f);  // 0x204E38: the move's root velocity (channel kind 1) at its time, reversed into +0x190
            if (!FAsegaRetracted(r.pasega))
                if (const ASEGCHN* pchn = PchnFindAseg(r.pasega->paseg, static_cast<ALO*>(plo), 1, true); pchn && pchn->ptrkp)
                    pchn->ptrkp->Eval(nullptr, &v, r.pasega->t, r.pasega->svt);
            ps->vecVel = -glm::vec3(v);
        }
}

const FONTBRX* s_pfontIcons = nullptr;

// One "&2c&." icon text at (x, y) UI units (the TB at the origin, 640 x 492.8, left / top) at its scale.
void DrawIcon(const FONTBRX& font, const FONTBRX* picons, const std::string& sz, float scale, float x, float y)
{
    g_hud.DrawTextBox(font, picons, sz, x - 320.0f, y - 246.4f, scale * 51.2f, 1.0f, 1.0f, 640.0f, 492.8f, 0, 0,
                      glm::vec4(128.0f, 128.0f, 128.0f, 0.0f), glm::vec4(1.0f), glm::vec4(128.0f, 128.0f, 128.0f, 0.0f), g_zScreenBase);
}
}  // namespace

// orig: 0x001531A8 (command 0x49: the prompts (time, button) laid out along y 468.8 from x 100 over 440 units;
// the hint at 10; then the state (0x153128))
void SetDancePrompts(int state, const std::vector<std::pair<float, int>>& aent)
{
    if (!s_ploDance) return;
    DANCE& d = Dance(s_ploDance);
    d.aprompt.clear();
    if (!aent.empty())
    {
        const float x0 = aent.front().first;
        const float dx = aent.back().first - x0;
        const float r = dx != 0.0f ? 440.0f / dx : 0.0f;
        for (const auto& [t, k] : aent)
        {
            PROMPT pr;
            pr.t = t;
            pr.k = k;
            pr.pos = glm::vec3(100.0f + r * (t - x0), 468.80002f, 0.0f);
            d.aprompt.push_back(pr);
        }
        d.posHint = glm::vec3(10.0f, 468.80002f, 0.0f);
        d.stateHint = 0;
        d.iHint = 0;
    }
    if (FPrint()) std::printf("dance: state %d, %zu prompts t %.3f music %.3f\n", state, aent.size(), g_clock.t, TMusicTop());
    SetDanceState(d, state);
}

// orig: 0x001532D8 (command 0x4A: a beat of the dancer: its button, its move, the music time; 3 kept)
void AddDanceBeat(LO* ploDancer, int nButton, int nMove, float gTime)
{
    if (!s_ploDance) return;
    DANCE& d = Dance(s_ploDance);
    if (d.abeat.size() == 3) d.abeat.erase(d.abeat.begin());
    BEAT b;
    b.po = ploDancer;
    b.n4 = nButton;
    b.n8 = nMove;
    b.gC = gTime;
    b.t10 = TMusicTop();
    d.abeat.push_back(b);
}

bool FDanceDirector() { return s_ploDance != nullptr; }

// the director's init 0x1526D8 (the global director, its update flags 0xA) / post-load 0x1527C8 (its world pass)
void PostLoadDance(LO* plo)
{
    Dance(plo);
    SW* psw = g_psw;
    if (plo->iupd >= 0 && psw) psw->aupd[static_cast<size_t>(plo->iupd)].grf |= 0xA;
    else plo->grfUpd |= 0xA;
}

// orig: 0x00152100 (vt slot 15 of the dancers: the step's 0x203A38, the moves found (0x152578), vt slot 79 (0))
void PostLoadDancer(LO* plo)
{
    InitDancer(plo, Dancer(plo));
    if (g_psw) EnablePo(g_psw, plo, 0);
}
bool FDancer(const LO* plo) { return plo && FCidDerivedFrom(plo->Cid(), 0x44); }

// orig: 0x001E3510.. (command 0x4B: PlayJtAsegDefault 0x1524A0 with grf 0)
void PlayDancerDefault(LO* ploDancer, LO* ploAseg) { PlayDancerAseg(ploDancer, ploAseg, 0); }

// orig: 0x00152C58 (vt slot 19 of the director: while 1 / 2 with prompts, each prompt's four buttons in a diamond
// (D_002BDB70: triangle up, circle right, cross down, square left; the asked one big (0.55), the others "o" (0.35))
// until the dancer may press it (playing, not done, within the late window: "&2?&." at 1.2); the hint "&2m&." (0.6)
// springs (0x117120) along to the next prompt from 0.4 s before it)
void DrawDancePrompts(const FONTBRX* pfont, const FONTBRX* picons)
{
    if (!pfont || !s_ploDance || !s_ploDance->fInWorld) return;
    DANCE& d = Dance(s_ploDance);
    if (d.state < 1 || d.state > 2 || d.aprompt.empty()) return;
    const float t = TMusicTop();
    struct KEY { int id; char ch; float dx, dy; };
    static const KEY s_akey[4] = {{0x10, 'T', -0.5f, -3.0f}, {0x20, 'O', 0.5f, -2.0f}, {0x40, 'X', -0.5f, -1.0f}, {0x80, 'S', -1.5f, -2.0f}};
    const float sBig = 0.55f, sSmall = 0.35f, sA = 1.2f, sB = 0.6f;
    const float dxBig = DxTextUi(*pfont, picons, "&2T&.", sBig), dyBig = sBig * 51.2f;
    const float dxSmall = DxTextUi(*pfont, picons, "&2T&.", sSmall), dySmall = sSmall * 51.2f;
    const float dxA = DxTextUi(*pfont, picons, "&2?&.", sA), dyA = sA * 51.2f;
    const float dxB = DxTextUi(*pfont, picons, "&2m&.", sB), dyB = sB * 51.2f;
    for (const PROMPT& pr : d.aprompt)
    {
        if (d.state == 1 || pr.fDone || pr.t + d.g5C < t)
            for (const KEY& k : s_akey)
            {
                float x = pr.pos.x + k.dx * dxSmall, y = pr.pos.y + k.dy * dySmall;
                char ch = 'o';
                float scale = sSmall;
                if (k.id == pr.k)
                {
                    x -= (dxBig - dxSmall) * 0.5f;
                    y -= (dyBig - dySmall) * 0.5f;
                    ch = k.ch;
                    scale = sBig;
                }
                DrawIcon(*pfont, picons, std::string("&2") + ch + "&.", scale, x, y);
            }
        else DrawIcon(*pfont, picons, "&2?&.", sA, pr.pos.x - dxA * 0.5f, pr.pos.y - dyA);
    }
    if (d.iHint < static_cast<int>(d.aprompt.size()))
    {
        const PROMPT& pr = d.aprompt[static_cast<size_t>(d.iHint)];
        if (d.stateHint == 0 && pr.t - t < 0.4f)
        {
            d.velHint = glm::vec3(100.0f, -1000.0f, 0.0f);  // D_002BDBC0
            d.stateHint = 1;
        }
        else if (d.stateHint == 1 && pr.t < t)
        {
            d.stateHint = 0;
            d.iHint++;
        }
        if (d.stateHint == 1)
        {
            glm::vec4 pos, vel;
            EvalHermiteVector(glm::vec4(d.posHint, 0.0f), glm::vec4(d.velHint, 0.0f), glm::vec4(pr.pos, 0.0f), glm::vec4(100.0f, 1000.0f, 0.0f, 0.0f),
                              &pos, &vel, nullptr, std::max(pr.t - t, g_clock.dt), g_clock.dt, 1.0f);  // 0x117120 (D_002BDBD0)
            d.posHint = glm::vec3(pos);
            d.velHint = glm::vec3(vel);
        }
    }
    DrawIcon(*pfont, picons, "&2m&.", sB, d.posHint.x - dxB * 0.5f, d.posHint.y - dySmall * 3.0f - dyB - 7.0f);
}

// (sw.cpp: a world's objects gone)
void ResetDances()
{
    s_mpdance.clear();
    s_mpdancer.clear();
    s_ploDance = nullptr;
}

namespace
{
struct INITDANCE
{
    INITDANCE()
    {
        RegisterLoUpdate(0x152970, UpdateDance);
        RegisterLoUpdate(0x152148, UpdateDancer);
        // EOPID 1354 (setter 0x153128, +0x50): a script sets the tango's state (pausing / resuming the game on 1);
        // natively the value only reached a raw field (audit 2026-11 P8; items 37 / 38)
        RegisterObjectSetter(1354, [](LO* plo, const CRef& r) { SetDanceState(Dance(plo), r.RefCoerceS32().m_n); });
        RegisterObjectGetter(1354, [](LO* plo) {
            CRef r;
            r.SetS32(Dance(plo).state);
            return r;
        });
        // optid 0x10F918 -> 0x153810 (a crowd entry: its object, its first state; 0x80 at most)
        RegisterObjectMethod(0x10F918, [](LO* plo, const std::vector<CRef>& a) {
            DANCE& d = Dance(plo);
            if (d.acrowd.size() < 0x80)
            {
                CROWD c;
                c.oid = NArg(a, 0, -1);
                c.k = NArg(a, 1, -1);
                d.acrowd.push_back(c);
            }
            return CRef(TAGK_Void);
        });
        // optid 0x10F9C0 -> 0x153800 (the scores cleared)
        RegisterObjectMethod(0x10F9C0, [](LO* plo, const std::vector<CRef>&) {
            DANCE& d = Dance(plo);
            d.ag.clear();
            d.g284 = 0.0f;
            d.g288 = 0.0f;
            return CRef(TAGK_Void);
        });
        // optid 0x10F838 -> 0x1524D8 (a move of the dancer: its button, the good move (ASEG, grf), the bad, the missed)
        RegisterObjectMethod(0x10F838, [](LO* plo, const std::vector<CRef>& a) {
            DANCER& r = Dancer(plo);
            if (r.amove.size() < 8)
            {
                MOVE m;
                m.k = NArg(a, 0, 0);
                m.aid[0] = NArg(a, 1, -1);
                m.agrf[0] = NArg(a, 2, 0);
                m.aid[1] = NArg(a, 3, -1);
                m.aid[2] = NArg(a, 4, -1);
                r.amove.push_back(m);
            }
            return CRef(TAGK_Void);
        });
        // EOPID 1355 (+0x284, a field read only: the scores' mean), as the dance's job reads it on the task event 0x36
        RegisterObjectGetter(1355, [](LO* plo) {
            CRef r;
            if (DANCE* pd = Pdance(plo)) r.SetF32(pd->g284);
            else r.SetF32(FloatField(plo, 0x284, 0.0f));
            return r;
        });
        // orig: 0x00152250 (vt slot 5 of the dancers, after the STEP's 0x1C3E20): message 9 from its own ASEGA (+0x18):
        // 0x27 busy (+0x40C), 0x28 not, 0x15E a push (the clip's root velocity 0x204E38 at the event's time, reversed into
        // +0x190, and turned by the rotation rows +0x1B0 to vt slot 35). (Slot 50 0x152350: actor.cpp.)
        RegisterLoMessageHandler([](LO* plo, int msgk, void* pv) {
            if (msgk != 9 || !pv || !plo || !plo->pcls || plo->pcls->PfnOrig(5) != 0x152250 || std::getenv("SLY2N_NODANCERMSG")) return false;
            const ASEGMSG9* pm = static_cast<const ASEGMSG9*>(pv);
            if (!pm->pasega || pm->pasega->palo != plo) return false;
            DANCER& r = Dancer(plo);
            switch (pm->n)
            {
            case 0x27: r.f40C = 1; break;
            case 0x28: r.f40C = 0; break;
            case 0x15E:
            {
                glm::vec4 v(0.0f);
                ALO* palo = static_cast<ALO*>(plo);
                if (!FAsegaRetracted(pm->pasega))
                    if (const ASEGCHN* pchn = PchnFindAseg(pm->pasega->paseg, palo, 1, true); pchn && pchn->ptrkp)
                        pchn->ptrkp->Eval(nullptr, &v, pm->t, pm->pasega->svt);
                if (STEP* ps = PstepFromLo(plo))
                {
                    ps->vecVel = -glm::vec3(v);
                    SetAloVelocityTo(palo, ps->matHeading * glm::vec3(v));  // vt slot 35
                }
                break;
            }
            default: break;
            }
            return false;  // (the STEP's handlers too)
        });
    }
} s_initdance;
}  // namespace
