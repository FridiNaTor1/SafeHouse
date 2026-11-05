// See frontend.h. Reference: the original's code (disassembly) and the Sly 2 decomp (src/hand, src/auto):
// 0x1A4E08 / 0x1A4E80 (the action queue), memcard.cpp's 0x1AB418 / 0x1AB630 (the slots), m_1DAC58_p3.cpp's
// 0x1DD920 / 0x1DDB08 / 0x1DDBF0 (the title), 0x165E38 (a game from a save), func_0015CDA8.c / 0x15CF30 /
// func_0015D5C8.c / func_0015D648.c / func_0015D798.c / func_0015D880.c / func_0015D8F8.c / func_0015D9C0.c /
// func_0015DA88.c (the episode menu), func_00228358.c / func_002283D0.c / func_00228538.c (the exits),
// func_001880D8.c / func_00187E88.c (the pad's steps).

#include "frontend.h"
#include "replay.h"
#include "game.h"

#include "activate.h"
#include "ai.h"
#include "clock.h"
#include "cmworld.h"
#include "cplcy.h"
#include "objmethod.h"
#include "cls.h"
#include "disc.h"
#include "fmv.h"
#include "gl.h"
#include "hudwidget.h"
#include "menu.h"
#include "joy.h"
#include "moviegl.h"
#include "ref.h"
#include "screenrt.h"
#include "sound.h"
#include "splayer.h"
#include "sw.h"
#include "task.h"
#include "po.h"
#include "alo.h"
#include "so.h"
#include "transition.h"
#include "update.h"
#include "worldaccess.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

void SetExitState(LO* plo, int n);
void TriggerExit(LO* plo);

namespace
{
// ------------------------------------------------------------ the pad (orig JOY +0x1024..)
struct JOYSTEP
{
    int dirX = 0, dirY = 0;
    float tNextX = 0, tRepX = 0, tNextY = 0, tRepY = 0;
};
JOYSTEP s_joystep;
uint32_t s_grfHandled = 0;

uint32_t GrfSly(uint16_t grfNative) { return uint32_t((grfNative & 0xFF) << 8 | (grfNative >> 8)); }

// the frame's pressed buttons not yet taken (UpdateJoy runs once per frame: a new frame clears the handled set)
uint32_t GrfPressedFrame() { return GrfSly(g_joy.grfbtnPressed) & ~s_grfHandled; }

float GFieldLo(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t u = 0;
    std::memcpy(&u, &gDefault, 4);
    u = plo->U32Field(0, ib, u);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

float GMagLeft() { return std::sqrt(g_joy.xLeft * g_joy.xLeft + g_joy.yLeft * g_joy.yLeft); }

void PlayUiSound(int id)
{
    // orig 0x1FBB90 (the sound of a menu id) + 0x1FB2F8
    PsndStartSound(id);
}

// ------------------------------------------------------------ the slots
CRef RefProgressOfSave(const GAMESAVE& gs, const std::vector<std::string>& apath)
{
    GAMESAVE saved = g_gamesave;
    g_gamesave = gs;
    const CRef ref = RefGetProgress(apath);
    g_gamesave = saved;
    return ref;
}
float GRef(const CRef& r) { return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? float(r.m_n) : 0.0f; }
int NRef(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_tagk == TAGK_Bool ? int(r.m_bool) : 0; }

const char* const s_aszEpisode[9] = {"intro", "nightclub", "palace", "temple", "prison", "castle", "train", "sawmill", "blimp"};

// orig 0x2BE080.. (the episode menu's tables in the executable)
int32_t NElf(uint32_t va) { int32_t n = 0; if (const uint8_t* pb = g_disc.PbElf(va, 4)) std::memcpy(&n, pb, 4); return n; }

GAMESAVE s_gsPending;   // orig taskmgr +0x1C (0x2DEB4C): the save the next load starts from
bool s_fPending = false;

// ------------------------------------------------------------ the movie queue (orig 0x2F66B0)
std::vector<std::array<uint8_t, 64>> s_aabMovie;

// ------------------------------------------------------------ the title (orig 0x3DA530: the widget, +0x24 0x5D2)
LO* s_ploTitle = nullptr;
bool s_fTitle = false;

// ------------------------------------------------------------ the episode menu (class 0x83, orig +0x110..)
struct EPMENU
{
    LO* plo = nullptr;
    int cur = 0;        // +0x110 the episode shown (0: the movies)
    int next = -1;      // +0x114
    int max = 8;        // +0x118 the last one open
    int mode = -1;      // +0x11C
    int sub = -1;       // +0x120
    float t124 = 0;     // +0x124
    int imovie = 0;     // +0x128 the movie chosen (cur 0)
    int hint = -1;      // +0x12C the "view the movie" hint (0x5BD / 0x5BE)
    float t130 = 0;     // +0x130
};
EPMENU s_epm;           // orig D_003E1C04 (SW +0x4774): the one menu
int s_imovieReturn = -1;   // orig D_002BE270: the movie the menu returns to
const SW* s_pswText = nullptr;

// orig 0x0015D5C8: the part the mode plays
int IdPartCur(const EPMENU& p)
{
    if (p.mode == -1) return -1;
    if (p.mode == 8 && p.sub != -1)
    {
        const int32_t pa = NElf(0x2BE1B8 + 4 * uint32_t(p.imovie));
        return pa ? NElf(uint32_t(pa) + 4 * uint32_t(p.sub)) : -1;
    }
    const int32_t pa = NElf(0x2BE080 + 4 * uint32_t(p.cur));
    return pa ? NElf(uint32_t(pa) + 4 * uint32_t(p.mode)) : -1;
}

// orig 0x0015D880: the hint
void SetHint(EPMENU& p, int n)
{
    if (p.hint == n) return;
    if (n == 0) SelectScreenObjPart(p.plo, 0x5BE);
    else if (n == 1) SelectScreenObjPart(p.plo, 0x5BD);
    p.hint = n;
    p.t130 = g_clock.tReal;
}

// orig 0x0015D648
void SetEpMode(EPMENU& p, int mode, int sub)
{
    if (p.mode == mode && p.sub == sub) return;
    switch (p.mode)
    {
    case 0: HudAutoSaveFrontEnd(); break;   // (0x1ABD98(g_save, 1))
    case 4:
    case 6:
        p.cur = p.next;
        p.next = -1;
        break;
    case 2: SetHint(p, 0); break;
    default: break;
    }
    if (mode == 2)
    {
        SetHint(p, -1);
        SetHint(p, 0);
    }
    StopScreenObj(p.plo);   // (+0x70..+0x78 -1, the timer 1, the state 1)
    p.mode = mode;
    p.sub = sub;
    p.t124 = g_clock.tReal;
    const int id = IdPartCur(p);
    if (id != -1) ShowScreenObj(p.plo, id);   // vt slot 49
}

// orig 0x0015D798: the movies open for the current episode's list
int CmovieOpen(const EPMENU& p)
{
    if (NElf(0x2BE26C)) return 0x10;
    int n = NElf(0x2BE2D0 + 4 * uint32_t(p.max));
    if (GRef(RefGetProgress({"episode", s_aszEpisode[p.max], "dt"})) == 0.0f) n -= 2;
    else if (NRef(RefGetProgress({"episode", s_aszEpisode[p.max], "percent_complete"})) <= 99) n -= 1;
    return n;
}

bool FPercentDone(int iep) { return NRef(RefGetProgress({"episode", s_aszEpisode[iep], "percent_complete"})) >= 100; }

// orig 0x0015CDA8 (vt slot 15)
void StartEpMenu(EPMENU& p, LO* plo)
{
    p = EPMENU{};
    p.plo = plo;
    p.max = 8;
    while (!NElf(0x2BE26C))
    {
        if (GRef(RefGetProgress({"episode", s_aszEpisode[p.max], "dt"})) != 0.0f) break;
        if (NRef(RefGetProgress({"episode", s_aszEpisode[p.max - 1], "percent_complete"})) >= 100) break;
        if (--p.max < 2) break;
    }
    if (s_imovieReturn == -1)
    {
        p.cur = p.max;
        if (const int iep = IepReplayReturn(); iep >= 1 && iep <= p.max) p.cur = iep;   // (QoL: back from a replay)
        SetEpMode(p, 0, -1);
    }
    else
    {
        p.imovie = s_imovieReturn;
        p.cur = 0;
        s_imovieReturn = -1;
        SetEpMode(p, 8, 0);
    }
    ClearJoySteps();
}

void RequestReload(SW* psw) { RequestTransition(psw, nullptr, 0x189, -1, 0, -1); }

// orig 0x0015CF30 (vt slot 16, after the screen object's own update)
void UpdateEpMenu(EPMENU& p, SW* psw)
{
    if (!p.plo || FMenuShown()) return;   // (the menu 0x3D9ED0, the card status 0x2E6550, the message 0x3DA4F0)
    const int idPart = IdPartCur(p);
    switch (p.mode)
    {
    case -1: return;
    case 2:
    {
        const int n = NStepUpDown();
        if (n < 0 && p.cur <= 0) { PlayUiSound(0x36); return; }
        if (n > 0 && p.cur >= p.max) { PlayUiSound(0x36); return; }
        if (n != 0)
        {
            p.next = p.cur + (n < 0 ? -1 : 1);
            SetEpMode(p, n > 0 ? 4 : 6, -1);   // (0x15D08C: down 4, up 6)
            return;
        }
        if (GrfbtnTakePressed(0x40)) { SetEpMode(p, 7, -1); return; }
        if (p.cur != 0 && GrfbtnTakePressed(0x10))
        {
            p.next = 0;
            SetEpMode(p, 6, -1);
            return;
        }
        if (p.hint == 0)
        {
            const int32_t va = NElf(0x2BE248 + 4 * uint32_t(p.cur));
            const bool fOpen = FPercentDone(p.cur) || NElf(0x2BE26C) != 0;
            if (va && fOpen && g_clock.tReal - p.t130 > 10.0f) SetHint(p, 1);
        }
        else if (p.hint == 1)
        {
            if (GrfbtnTakePressed(0x80))
            {
                // the episode's movie, then the front end again
                QueueMovie(g_disc.PbElf(uint32_t(NElf(0x2BE248 + 4 * uint32_t(p.cur))), 64));
                RequestReload(psw);
                PlayUiSound(0x34);
                return;
            }
            if (g_clock.tReal - p.t130 > 2.0f) SetHint(p, 0);
        }
        return;
    }
    case 8:
        if (p.sub == 0)
        {
            if (GrfbtnTakePressed(0x40))
            {
                s_imovieReturn = p.imovie;
                QueueMovie(g_disc.PbElf(uint32_t(NElf(0x2BE200 + 4 * uint32_t(p.imovie))), 64));
                RequestReload(psw);
                PlayUiSound(0x34);
                return;
            }
            if (GrfbtnTakePressed(0x10)) { SetEpMode(p, 9, -1); return; }
            const int n = NStepLeftRight();
            if (n < 0 && p.imovie <= 0) { PlayUiSound(0x36); return; }
            if (n > 0 && !(p.imovie < CmovieOpen(p))) { PlayUiSound(0x36); return; }
            if (n == 0) return;
            p.imovie += n < 0 ? -1 : 1;
            SetEpMode(p, 8, n > 0 ? 2 : 1);
            return;
        }
        if (p.sub > 0)
        {
            if (p.sub >= 3 || FScreenObjBusy(p.plo, idPart)) return;
            SetEpMode(p, 8, 0);
            return;
        }
        if (p.sub == -1)
        {
            // (QoL Replay Jobs, replay.h: TRIANGLE's list, CIRCLE back while it is offered)
            if (p.cur >= 1)
                switch (NUpdateReplayMenu(p.cur))
                {
                case 1: return;
                case 2: SetEpMode(p, 9, -1); return;
                case 3: SetEpMode(p, 10, -1); return;
                default: break;
                }
            if (GrfbtnTakePressed(0x40)) { SetEpMode(p, 10, -1); return; }
            if (GrfbtnTakePressed(0x10)) SetEpMode(p, 9, -1);
        }
        return;
    case 10:
    {
        if (FScreenObjBusy(p.plo, idPart)) return;
        if (FReplayChosen())   // (QoL Replay Jobs: the chosen job instead of the episode)
        {
            StartReplay(psw);
            SetEpMode(p, -1, -1);
            return;
        }
        // a first visit: the episode's exit (its intro movies, then the world); else the episode's world
        bool fExit = GRef(RefGetProgress({"episode", s_aszEpisode[p.cur], "dt"})) == 0.0f;
        if (fExit)
        {
            LO* ploExit = PloFindSwObjectByOid(psw, NElf(0x2BE2A8 + 4 * uint32_t(p.cur)));
            if (ploExit)
            {
                SetExitState(ploExit, 3);
                TriggerExit(ploExit);
            }
            else fExit = false;
        }
        if (!fExit)
            if (const WREC* pw = PwrecEpisodeMain(p.cur)) RequestTransition(psw, pw->abRecord, 0x189, -1, 0, 0x30A);
        SetEpMode(p, -1, -1);
        return;
    }
    default:
    {
        if (FScreenObjBusy(p.plo, idPart)) return;
        const int next = NElf(0x2BE278 + 4 * uint32_t(p.mode));
        int sub = -1;
        if (next == 8 && p.cur == 0)
        {
            p.imovie = 0;
            sub = 0;
        }
        SetEpMode(p, next, sub);
        return;
    }
    }
}

// ------------------------------------------------------------ the exits (class 0x1A, orig +0x134..)
std::unordered_map<const LO*, float> s_mptLoad;   // +0x158 (the state +0x13C / +0x140: LO::nState / tState)
}  // namespace

// (QoL Replay Jobs, replay.cpp) the episode on the "Start Episode" screen (mode 8 sub -1), else -1
int IepEpMenuShown() { return s_epm.plo && s_epm.mode == 8 && s_epm.sub == -1 && s_epm.cur >= 1 ? s_epm.cur : -1; }

// orig: 0x00228358 SetExitState
void SetExitState(LO* plo, int n)
{
    if (n == plo->nState) return;
    if (std::getenv("SLY2N_EXITPRINT")) std::printf("exit %d: state %d -> %d t %.3f\n", plo->oid, plo->nState, n, g_clock.t);
    if (n == 4)
    {
        SetLoSt(g_psw, plo, 1);  // 0x1A6CA8: keep updating through the exit's delay.
        PauseGame(3);           // 0x20EFC0: lock the pad and pause AI / subtitles until the load.
        if (g_psw) g_psw->ploExitPending = plo;
    }
    plo->nState = n;
    plo->tState = g_clock.t;
}

// orig: 0x002283D0 TriggerExit: an armed exit (state 3) fires: its checkpoint's tasks (0x161F48), the camera's
// cuts (0x14E4C8), state 4 and its time (after the +0x160 delay), vt slot 3 (5).
void TriggerExit(LO* plo)
{
    if (plo->nState != 3) return;
    if (plo->U32Field(0, 0x134, 0))
    {
        SetExitState(plo, 0);
        // orig 0x002280D0: the last exit with +0x130 set, among the first 32 found by class.
        LO* ploNext = nullptr;
        const auto aploExit = AploOfClass(g_psw, 0x1A);
        for (size_t i = 0; i < aploExit.size() && i < 32; ++i)
            if (aploExit[i]->U32Field(0, 0x130, 0)) ploNext = aploExit[i];
        if (ploNext) TriggerExit(ploNext);
        return;
    }
    // 0x161F48 (+0x164 / +0x168: EOPID 788's (task, state) pairs)
    std::vector<std::pair<OID, int>> atask;
    for (const OPT& o : plo->aopt)
        if (o.eopid == 788 && o.aval.size() >= 2) atask.emplace_back(o.aval[0].n, o.aval[1].n);
    if (!atask.empty() && g_psw) ApplyCheckpointTasks(g_psw, plo, atask);
    RevokeCmPolicy(&g_cm, 2, -1, &g_cmpolicies.cprecon, nullptr, nullptr);
    RevokeCmPolicy(&g_cm, 2, -1, &g_cmpolicies.cpfree, nullptr, nullptr);
    SetExitState(plo, 4);
    s_mptLoad[plo] = g_clock.t + GFieldLo(plo, 0x160, 0.0f);
    if (std::getenv("SLY2N_EXITPRINT"))
        std::printf("exit %d: fired t %.3f delay %.3f point %d wipe %d\n", plo->oid, g_clock.t, GFieldLo(plo, 0x160, 0.0f),
                    int(plo->U32Field(0, 0x150, 0x189)), int(plo->U32Field(0, 0x154, uint32_t(-1))));
    HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
}

bool FPointInVolume(LO* plo, const glm::vec3& pos);  // trigger.cpp
namespace
{
struct INITEXITMETHODS
{
    INITEXITMETHODS()
    {
        // orig: 0x0010B7D8 (EOPID 786, property 235) -> 0x002283D0, no arguments.
        RegisterObjectMethod(0x10B7D8, [](LO* plo, const std::vector<CRef>&) {
            TriggerExit(plo);
            return CRef(TAGK_Void);
        });
        // EOPID 779 (property 761): the live checkpoint oid at +0x150, also read by 0x228538.
        RegisterObjectGetter(779, [](LO* plo) {
            CRef r;
            r.SetS32(static_cast<int32_t>(plo->U32Field(0, 0x150, 0x189)));
            return r;
        });
        RegisterObjectSetter(779, [](LO* plo, const CRef& r) { plo->SetField(0, 0x150, uint32_t(NRef(r))); });
    }
} s_initexitmethods;

// orig 0x00228538 (vt slot 16): state 4 at its time: its movies queued, the transition
void UpdateExit(LO* plo, SW* psw)
{
    if (plo->nState == 0 || plo->nState == 1 || plo->nState == 3)
    {
        // the player in the volume (0x118FB0 in its frame): 0 (inside since the start) -> 1 outside; 1 -> 0 inside,
        // or armed (3) after +0x15C s outside; 3: entered, fired (0x2283D0)
        LO* pj = PloPoCurrent(psw);  // 0x1C4840
        if (!pj || !FCidDerivedFrom(pj->Cid(), 0x30)) return;
        const bool f = FPointInVolume(plo, PosAloCenter(static_cast<ALO*>(pj)));  // 0x1A62E8
        switch (plo->nState)
        {
        case 3: if (f) TriggerExit(plo); break;
        case 1:
            if (f) SetExitState(plo, 0);
            else if (GFieldLo(plo, 0x15C, 0.0f) < g_clock.t - plo->tState) SetExitState(plo, 3);
            break;
        case 0: if (!f) SetExitState(plo, 1); break;
        default: break;
        }
        return;
    }
    if (plo->nState != 4) return;
    float& tLoad = s_mptLoad[plo];
    if (!(0.0f < tLoad && tLoad <= g_clock.t)) return;
    const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(plo);
    if (!px || px->aabKey.empty()) return;
    for (size_t i = 1; i < px->aabKey.size(); ++i)
    {
        QueueMovie(px->aabKey[i].data());
        if (std::getenv("SLY2N_EXITPRINT"))
        {
            const FK fk = FkFromRecord(px->aabKey[i].data());
            std::printf("exit %d: movie %u queued (%u bytes)\n", plo->oid, fk.id, fk.cb);
        }
    }
    const int flags = plo->U32Field(0, 0x138, 0) ? 0x20 : 0;
    RequestTransition(psw, px->aabKey[0].data(), OID(plo->U32Field(0, 0x150, 0x189)), -1, flags,
                      OID(plo->U32Field(0, 0x154, uint32_t(-1))));
    tLoad = 0.0f;
}
}  // namespace

// ------------------------------------------------------------ the pad
void JoyBeginFrame() { s_grfHandled = 0; }
uint32_t GrfbtnPressed(uint32_t grfMask) { return GrfPressedFrame() & grfMask; }
uint32_t GrfbtnTakePressed(uint32_t grfMask)
{
    const uint32_t grf = GrfPressedFrame() & grfMask;
    s_grfHandled |= grf;
    return grf;
}
uint32_t GrfbtnHeld() { return GrfSly(g_joy.grfbtn); }

int NStepUpDown()
{
    JOYSTEP& p = s_joystep;
    const float gX = g_joy.xLeft, gY = g_joy.yLeft, gMag = GMagLeft();
    if (!p.dirY)
    {
        const uint32_t grf = GrfbtnTakePressed(0x5000);
        if (grf & 0x1000) p.dirY = -1;
        else if (grf & 0x4000) p.dirY = 1;
        else if (gMag >= 0.8f && std::fabs(gY) > std::fabs(gX)) p.dirY = gY > 0.0f ? -1 : 1;
        if (p.dirY)
        {
            p.tRepY = 0.3333f;
            p.tNextY = g_clock.t3C + p.tRepY;
        }
        return p.dirY;
    }
    const uint32_t grfHeld = GrfbtnHeld();
    bool fHeld = false;
    if (p.dirY < 0) fHeld = (grfHeld & 0x1000) || (gMag >= 0.25f && std::fabs(gY) > std::fabs(gX) && gY > 0.0f);
    else fHeld = (grfHeld & 0x4000) || (gMag >= 0.25f && std::fabs(gY) > std::fabs(gX) && gY < 0.0f);
    if (!fHeld) { p.dirY = 0; return 0; }
    if (p.tNextY > g_clock.t3C) return 0;
    p.tRepY = std::clamp(2.0f * p.tRepY / 3.0f, 0.1f, 0.3333f);
    p.tNextY = g_clock.t3C + p.tRepY;
    return p.dirY;
}

int NStepLeftRight()
{
    JOYSTEP& p = s_joystep;
    const float gX = g_joy.xLeft, gY = g_joy.yLeft, gMag = GMagLeft();
    if (!p.dirX)
    {
        const uint32_t grf = GrfbtnTakePressed(0xA000);
        if (grf & 0x8000) p.dirX = -1;
        else if (grf & 0x2000) p.dirX = 1;
        else if (gMag >= 0.8f && std::fabs(gX) > std::fabs(gY)) p.dirX = gX < 0.0f ? -1 : 1;
        if (p.dirX)
        {
            p.tRepX = 0.3333f;
            p.tNextX = g_clock.t3C + p.tRepX;
        }
        return p.dirX;
    }
    const uint32_t grfHeld = GrfbtnHeld();
    bool fHeld = false;
    if (p.dirX < 0) fHeld = (grfHeld & 0x8000) || (gMag >= 0.25f && std::fabs(gX) > std::fabs(gY) && gX < 0.0f);
    else fHeld = (grfHeld & 0x2000) || (gMag >= 0.25f && std::fabs(gX) > std::fabs(gY) && gX > 0.0f);
    if (!fHeld) { p.dirX = 0; return 0; }
    if (p.tNextX > g_clock.t3C) return 0;
    p.tRepX = std::clamp(2.0f * p.tRepX / 3.0f, 0.1f, 0.3333f);
    p.tNextX = g_clock.t3C + p.tRepX;
    return p.dirX;
}

void ClearJoySteps()
{
    s_joystep.dirX = 0;
    s_joystep.dirY = 0;
}

// ------------------------------------------------------------ the slots
SAVESLOT g_aslot[3];
int g_islotCur = -1;
int g_islotChosen = -1;
static bool s_fSlotsChosen = true;
void ChooseNewestSlot();

void ReadSaveSlots(const std::string& dirDev)
{
    const std::filesystem::path dirUser = cane::userDirectory() / "Saves";
    bool fUser = false;
    for (int i = 0; i < 3; ++i)
        fUser = fUser || std::filesystem::exists(dirUser / ("save" + std::to_string(i + 1)));
    for (int i = 0; i < 3; ++i)
    {
        SAVESLOT& s = g_aslot[i];
        s = SAVESLOT{};
        const std::string name = "save" + std::to_string(i + 1);
        s.path = cane::utf8(dirUser / name);
        std::string pathRead = s.path;
        static const bool s_fNoCard = std::getenv("SLY2N_NOCARD") != nullptr;   // (debug: no card: a new game, no card messages)
        if (!fUser && !dirDev.empty() && !s_fNoCard) pathRead = dirDev + "/" + name;   // (the development card until the user has one)
        std::ifstream in(pathRead, std::ios::binary);
        if (!in) continue;
        in.read(reinterpret_cast<char*>(s.save.ab), CB_GAMESAVE);
        if (in.gcount() != CB_GAMESAVE) continue;
        s.fFile = true;
        std::error_code ec;
        s.tWrite = std::filesystem::last_write_time(pathRead, ec).time_since_epoch().count();
    }
    s_fSlotsChosen = false;
}

// orig 0x001AB418 (6): the newest of the slots with a game (time played +0x3B1C not 0); at a world's start (the
// progress layout is the world's)
void ChooseNewestSlot()
{
    if (s_fSlotsChosen) return;
    s_fSlotsChosen = true;
    g_islotCur = -1;
    // orig 0x001AB630: each slot read validated by the world's layout (0x1AC570 -> 0x1D7C80: another layout's
    // record cleared and stamped); 0x1AB418 (6): a slot without a game cleared (0x1D8D80(slot, 0))
    for (SAVESLOT& sl : g_aslot)
    {
        if (!sl.fFile) continue;
        GAMESAVE saved = g_gamesave;
        g_gamesave = sl.save;
        ValidateGameSave();
        if (GRef(RefGetProgress({"dt"})) == 0.0f) std::memset(g_gamesave.ab, 0, CB_PROGRESS);
        sl.save = g_gamesave;
        g_gamesave = saved;
    }
    // the newest: slot 1 unless a slot with a game is newer (its time, the header +0x5408: 0x1AB398); none without
    // the folder (the card's state is not 6)
    if (!g_aslot[0].fFile && !g_aslot[1].fFile && !g_aslot[2].fFile) return;
    g_islotCur = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (!g_aslot[i].fFile) continue;
        if (GRef(RefProgressOfSave(g_aslot[i].save, {"dt"})) == 0.0f) continue;
        if (g_aslot[g_islotCur].tWrite < g_aslot[i].tWrite || !g_aslot[g_islotCur].fFile) g_islotCur = i;
    }
}

void RechooseSaveSlots()
{
    s_fSlotsChosen = false;
    ChooseNewestSlot();
}

CRef RefProgressOfSlot(int islot, const std::vector<std::string>& apath)
{
    if (islot < 0 || islot > 2 || !g_aslot[islot].fFile) return CRef{};
    return RefProgressOfSave(g_aslot[islot].save, apath);
}

bool FWriteSaveSlot(int islot)
{
    if (islot < 0 || islot > 2) return false;
    SAVESLOT& s = g_aslot[islot];
    if (s.path.empty()) s.path = cane::utf8(cane::userDirectory() / "Saves" / ("save" + std::to_string(islot + 1)));   // (a run without the boot)
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cane::path(s.path)).parent_path(), ec);
    // orig 0x1ABCE8 / 0x1ABD40: 0x1D84B0 (the record into the slot), then the card thread writes it
    GAMESAVE gs;
    CopyGameSaveForSlot(gs);
    MergeReplaySave(gs);   // (QoL Replay Jobs: a replay keeps the save's progress)
    std::string err;
    if (!FWriteGameSaveBlock(s.path, gs, err)) { std::printf("save: %s\n", err.c_str()); return false; }
    s.save = gs;
    s.fFile = true;
    s.tWrite = std::filesystem::last_write_time(cane::path(s.path), ec).time_since_epoch().count();
    g_islotCur = islot;
    void AutoplayNoteSave(const std::string& path);   // (debug: SLY2N_AUTOSAVEDIR, autoplay.cpp)
    AutoplayNoteSave(s.path);
    return true;
}

// ------------------------------------------------------------ the boot sequence and the movies
void StartBootSequence(bool fMovie)
{
    // orig 0x001A4E08: kind 0 (the logo movie 0x2A3868, and the world at 0x2BFCA0: none yet), kind 5 (the card
    // status: the slots read), kind 2 (the boot world 0x2A38A8 at 0x189 with the flags 1)
    if (fMovie) QueueMovie(g_disc.PbElf(0x2A3868, 64));
    // (kind 5: 0x1ACC38(0x2E6550, 1): the card's messages on; debug SLY2N_NOCARD: no card at all, no messages)
    if (g_ncardMode == 0) g_ncardMode = std::getenv("SLY2N_NOCARD") ? 2 : 1;
    ReadSaveSlots(g_fDevCard ? SLY2N_DEV_ROOT "/build/saves/round4-user-card/memcard0/BASCUS-97316YAOTWTD!" : "");
    RequestTransition(nullptr, g_disc.PbElf(VA_FK_BOOT_WORLD, 64), 0x189, -1, 1, -1);
}

// orig: 0x001B3A48 (the queue 0x2F66B0: at most 4; a key whose +0x04 equals its +0x3C is no movie)
void QueueMovie(const uint8_t* pbKey)
{
    if (!pbKey || s_aabMovie.size() >= 4) return;
    uint32_t b = 0, c = 0;
    std::memcpy(&b, pbKey + 4, 4);
    std::memcpy(&c, pbKey + 0x3C, 4);
    if (b == c) return;
    std::array<uint8_t, 64> ab{};
    std::memcpy(ab.data(), pbKey, 64);
    s_aabMovie.push_back(ab);
}

bool FMoviesQueued() { return !s_aabMovie.empty(); }

void PlayQueuedMovies(bool fWindow)
{
    const auto aab = std::move(s_aabMovie);
    s_aabMovie.clear();
    if (!fWindow)   // (headless: skipped; SLY2N_TRANSLOG names them)
    {
        if (std::getenv("SLY2N_TRANSLOG"))
            for (const auto& ab : aab) std::printf("movie %u (headless: skipped)\n", FkFromRecord(ab.data()).id);
        return;
    }
    for (const auto& ab : aab)
    {
        const FK fk = FkFromRecord(ab.data());
        if (!fk.cb) continue;
        MOVIEGL m;
        m.fWindowed = true;
        std::printf("movie %u\n", fk.id);
        FPlayMovie(fk, PresentMovieGl, &m);
    }
}

// ------------------------------------------------------------ the title, the game start
// orig 0x00161A58 (3): the game record at the world's start: reset for the title (request flags 1), the pending
// save (0x40, 0x166008)
void OnGameRecordEvent(int grfReq)
{
    if (grfReq & 1)
    {
        g_gamesave = GAMESAVE{};
        ValidateGameSave();
        g_islotChosen = -1;
    }
    else if ((grfReq & 0x40) && s_fPending)
    {
        // orig 0x00166008: the pending save copied, the vibration setting (0x1D54B8: natively the record's own), its
        // settings applied (0x1D8FF8)
        g_gamesave = s_gsPending;
        ValidateGameSave();
        ApplyGameSettings(g_gamesave);
    }
    s_fPending = false;
    ChooseNewestSlot();   // (orig 0x1AB630: the card's slots read at the world's start)
    // (0x161A58 case 3's end: outside an episode, the card in state 6 without a chosen slot, a record without a game:
    // the newest slot's settings, when it has a game)
    if (IepisodeCur() == 0 && g_islotChosen < 0 && g_islotCur >= 0 && GRef(RefGetProgress({"dt"})) == 0.0f &&
        GRef(RefProgressOfSlot(g_islotCur, {"dt"})) != 0.0f)
        ApplyGameSettings(g_aslot[g_islotCur].save);
}

void StartFrontEnd(SW* psw, int grfReq)
{
    s_pswText = psw;
    s_mptLoad.clear();
    s_fTitle = false;
    s_ploTitle = nullptr;
    s_epm = EPMENU{};
    // the title's screen object (0x3DA530 +0x24) in the boot world started by the boot sequence; shown by its
    // runner's command 0x44 (ShowTitle) once the intro (or its skip) is over
    if (grfReq & 1) s_ploTitle = PloFindSwObjectByOid(psw, 0x5D2);
    // the episode menu (class 0x83) and the exits (class 0x1A, armed: state 3 when their option says so)
    if (std::getenv("SLY2_DUMPOPTS"))
        for (LO* plo : AploOfClass(psw, CID(std::strtol(std::getenv("SLY2_DUMPOPTS"), nullptr, 0))))
        {
            std::printf("opts of %d (cid 0x%X):", plo->oid, plo->Cid());
            for (const OPT& o : plo->aopt)
            {
                std::printf(" [%d grf 0x%X:", o.eopid, o.grf);
                for (const OPTVAL& v : o.aval)
                {
                    std::printf(" k%d n%d", v.k, v.n);
                    for (const OPTVAL& w : v.alist) std::printf(" (k%d n%d)", w.k, w.n);
                }
                std::printf("]");
            }
            std::printf("\n");
        }
    for (LO* plo : AploOfClass(psw, 0x83))
    {
        StartEpMenu(s_epm, plo);
        break;
    }
}

namespace
{
// EOPID 1055 (seto! on a camera object, cid 0x2C): its target; the boot world's title aims camera 150641 at Sly's
// 3788 from ASEG 150645's event 2
struct INITTITLESETTERS
{
    INITTITLESETTERS()
    {
        RegisterObjectSetter(1055, [](LO* plo, const CRef& r) {
            SetCmobjTarget(plo, r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr);
        });
    }
} s_inittitlesetters;
}

bool FTitleShown() { return s_fTitle; }
bool FTitleWorld() { return s_ploTitle != nullptr; }

// orig 0x001DD820 (on 0x3DA530: SCPT command 0x44): the title's screen object shown (its vt slot 49 with -1).
// TODO: the vt slot 10 call before it and RumbleUnknown1 0x1D5498 after it.
void ShowTitle()
{
    if (!s_ploTitle) return;
    s_fTitle = true;
    std::printf("title: shown at t %.2f (newest slot %d: %s)\n", g_clock.t, g_islotCur + 1,
                g_islotCur >= 0 && GRef(RefProgressOfSlot(g_islotCur, {"dt"})) != 0.0f ? "Resume Game" : "New Game");
    ShowScreenObj(s_ploTitle, -1);
}

// orig: 0x00165E38 func_00165E38
void StartGameFromSave(SW* psw, const GAMESAVE* pgs)
{
    const float dt = GRef(RefProgressOfSave(*pgs, {"dt"}));
    std::printf("game: start from a save (dt %g, slot %d)\n", dt, g_islotChosen + 1);
    if (s_fTitle && dt == 0.0f)
    {
        // a new game in the title's world: the title goes (0x1D54B8: the vibration setting; 0x1DD888)
        HideScreenObj(s_ploTitle);
        s_fTitle = false;
        return;
    }
    s_gsPending = *pgs;
    s_fPending = true;
    g_taskmgr.oidJob = -1;   // (taskmgr +0x10 / +0x14 / +0x18)
    g_taskmgr.oidTask = -1;
    if (dt == 0.0f)
    {
        // a new game: the first episode at its first task (0x62F)
        g_taskmgr.oidQueued = 0x62F;
        if (const WREC* pw = PwrecEpisodeMain(0)) RequestTransition(psw, pw->abRecord, 0x189, -1, 0x48, -1);
        return;
    }
    g_taskmgr.oidQueued = -1;
    const int iep = NRef(RefProgressOfSave(*pgs, {"episodeCurrent"}));
    const WREC* pw = iep >= 0 && iep < 9 ? PwrecEpisodeMain(iep) : nullptr;
    const int pct = iep >= 0 && iep < 9 ? NRef(RefProgressOfSave(*pgs, {"episode", s_aszEpisode[iep], "percent_complete"})) : 100;
    const uint8_t* pb = pw && pct < 100 ? pw->abRecord : g_disc.PbElf(0x2A3828, 64);   // (else the front end)
    RequestTransition(psw, pb, 0x189, -1, 0x40, -1);
}

// orig 0x001DDB08: a new game (dt 0) from the current record. TODO: 0x1440D0(po, 1) on Sly / Bentley / Murray
void StartNewGameFromTitle(SW* psw)
{
    CRef r;
    r.SetF32(0.0f);
    FSetProgress({"dt"}, r);
    StartGameFromSave(psw, &g_gamesave);
}

void UpdateFrontEnd(SW* psw)
{
    s_pswText = psw;
    // orig 0x001DD920: the title's input
    if (s_fTitle && !FMenuShown())
    {
        if (GrfbtnTakePressed(0x800))
        {
            g_islotChosen = g_islotCur;   // (0x7D50 = 0x7D54)
            if (g_islotCur >= 0) StartGameFromSave(psw, &g_aslot[g_islotCur].save);   // (0x1ACC38(1) first)
            else StartNewGameFromTitle(psw);
        }
        else if (GrfbtnPressed(0x100) && (GrfbtnHeld() & 0x108) == 0x100)
        {
            GrfbtnTakePressed(0x100);
            OpenDataAccessMenu(psw);   // orig 0x001B04E0(0)
        }
    }
    if (s_epm.plo) UpdateEpMenu(s_epm, psw);
    if (static SW* s_pswListed = nullptr; std::getenv("SLY2N_EXITPRINT") && psw && psw != s_pswListed)
    {
        s_pswListed = psw;
        for (LO* plo : AploOfClass(psw, 0x1A))
        {
            const glm::vec3 pos = PosLoWorld(plo);
            const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(plo);
            std::printf("exit %d: at (%.0f %.0f %.0f) state %d inworld %d delay %.3f point %d wipe %d keys %zu\n", plo->oid, pos.x, pos.y, pos.z,
                        plo->nState, plo->fInWorld, GFieldLo(plo, 0x160, 0.0f), int(plo->U32Field(0, 0x150, 0x189)),
                        int(plo->U32Field(0, 0x154, uint32_t(-1))), px ? px->aabKey.size() : 0);
        }
    }
    for (LO* plo : AploOfClass(psw, 0x1A))
        if (plo->nState == 4 || (plo->fInWorld && (plo->nState == 0 || plo->nState == 1 || plo->nState == 3))) UpdateExit(plo, psw);
}

std::string StrFrontEndScript(int id)
{
    char ach[64];
    switch (id)
    {
    case 0x5D3:
    {
        // orig 0x001DDBF0: the title's help line
        if (!g_joy.fConnected && false) return StrLocalized(s_pswText, 9);
        const float dt = g_islotCur >= 0 ? GRef(RefProgressOfSlot(g_islotCur, {"dt"})) : 0.0f;
        return StrLocalized(s_pswText, dt == 0.0f ? 5 : 6) + "\n" + StrLocalized(s_pswText, 8);
    }
    case 0x52F:
    {
        // orig 0x0015D8F8: the episode's percent (95% of it, +5 with its bonus powerup)
        if (!s_epm.plo || s_epm.cur < 0 || s_epm.cur > 8) return "";
        int n = NRef(RefGetProgress({"episode", s_aszEpisode[s_epm.cur], "percent_complete"}));
        n = n * 95 / 100;
        const int idPowerup = NElf(0x2BE2F8 + 4 * uint32_t(s_epm.cur));
        if (idPowerup != -1 && NRef(RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(idPowerup)})))
            n += 5;
        std::snprintf(ach, sizeof(ach), "%d%%", n);
        return ach;
    }
    case 0x530:
    {
        // orig 0x0015D9C0: the time played in it
        if (!s_epm.plo || s_epm.cur < 0 || s_epm.cur > 8) return "";
        const float g = GRef(RefGetProgress({"episode", s_aszEpisode[s_epm.cur], "dt"}));
        std::snprintf(ach, sizeof(ach), "%01d:%02d", int(g / 3600.0f), int(std::fmod(g, 3600.0f) / 60.0f));
        return ach;
    }
    case 0x5BF:
    case 0x5C0:
    {
        // orig 0x0015DA88: the movie's name (the setup / the getaway)
        if (!s_epm.plo) return "";
        const int k = id == 0x5BF ? s_epm.imovie - 1 : s_epm.imovie;
        if (k >= 1 && k <= 16) return StrLocalized(s_pswText, (k & 1) ? 0x77 : 0x78);
        return "";
    }
    default: return "";
    }
}

// (pcinput.cpp: the episode menu has the pad)
bool FEpisodeMenuActive() { return s_epm.plo != nullptr && s_epm.mode >= 0; }
