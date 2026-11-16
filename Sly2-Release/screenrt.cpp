// See screenrt.h. Reference ports: Sly2-Reference game/m_210E90_c.cpp (0x2121E8, 0x212A50, 0x212A70, 0x2128E8,
// 0x212C50, 0x2132D0, 0x213830, 0x213928, 0x213B00), m_210E90_p3.cpp (0x2129E8, 0x212E40, 0x213898, 0x211348),
// round4_paris_high_more.cpp (0x212880), the string table 0x2A8B90 (0x21AA60).

#include "screenrt.h"
#include "worldglobals.h"
#include "langstore.h"
#include "region.h"
#include "dance.h"
#include "actor.h"
#include "po.h"
#include "marker.h"
#include "sound.h"
#include "photo.h"
#include "ai.h"
#include "joy.h"
#include "task.h"
#include "qolview.h"

#include "clock.h"
#include "cm.h"
#include "cplcy.h"
#include "activate.h"
#include "disc.h"
#include "frontend.h"
#include "hud.h"
#include "vr.h"
#include "menu.h"
#include "shop.h"
#include "ref.h"
#include "savedata.h"
#include "sm.h"
#include "splayer.h"
#include "sw.h"
#include "worldaccess.h"
#include "vecmat.h"
#include "brx.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace
{
// orig: the SCREENOBJ fields +0x58..+0xCC
struct SCREENRT
{
    SPRITESET* pss = nullptr;
    uint32_t serialSw = 0;      // the world it was made in (SW::serial: the newest world then)
    CScreenPlayer player;
    bool fPlayer = false;
    int state = 1;              // +0x58
    float tState = 0.0f;        // +0x5C
    int timers = 0;             // +0x64 (1 hide, 2 show)
    float tTimers = 0.0f;       // +0x68
    int id70 = 0x310, id74 = 0x30F, id78 = 0x311;   // +0x70..+0x78 (0x212298)
    int id7C = -1;              // +0x7C the part playing
    float t88 = 0.0f;           // +0x88
    float dtMax = 0.0f;         // +0x8C
    bool fPause = false;        // +0xB0
    int prio = 0;               // +0xA4 (its draw depth, << 18; a shown listed object of a higher one hides it)
    bool fListed = false;       // +0xAC (in the shown list 0x3E1060 while it plays)
    bool fInList = false;       // +0xA8 == 0
    LO* follow = nullptr;       // +0xF4 (the world object whose screen position anchors the root)
    int aarg[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // +0xCC
};

std::unordered_map<const LO*, std::unique_ptr<SCREENRT>> s_mpsrt;
WORLD_TABLE(s_mpsrt);   // (PC: moved aside while a kept world loads, worldglobals.h)
const SCREENRT* s_psrtDrawing = nullptr;   // the object being drawn (its string arguments)
const SW* s_pswText = nullptr;
int s_cpause = 0;                          // 0x2DD318

SCREENRT* Psrt(LO* plo)
{
    SPRITESET* pss = dynamic_cast<SPRITESET*>(plo);
    if (!pss || !pss->pso) return nullptr;
    auto& p = s_mpsrt[plo];
    if (p && p->serialSw != SW::s_serialNext && std::getenv("SLY2N_SCREENLIFE"))
        std::fprintf(stderr, "screen lifetime: oid %d cached world %u current world %u\n",
                     plo->oid, p->serialSw, SW::s_serialNext);
    if (!p)
    {
        p = std::make_unique<SCREENRT>();
        p->pss = pss;
        p->serialSw = SW::s_serialNext;
        p->prio = int(plo->U32Field(0, 0xA4, 0));
        p->fListed = plo->U32Field(0, 0xAC, 0) != 0;
        p->fPause = plo->U32Field(0, 0xB0, 0) != 0;
        // +0x8C: the longest the idle part shows (the counters 0x36D..0x36F: 5 s in the reference's RAM)
        const uint32_t u8C = plo->U32Field(0, 0x8C, 0);
        std::memcpy(&p->dtMax, &u8C, sizeof(float));
        p->state = pss->pso->fExternal ? 0 : 1;   // (0x212298: +0x58 0 for the external ones)
        // the arguments (+0xCC..: options 1133..1142, kind 0x10) as the world sets them
        for (int i = 0; i < 10; ++i) p->aarg[i] = int(plo->U32Field(0, 0xCC + 4 * uint32_t(i), uint32_t(-1)));
    }
    return p.get();
}

// the external file loaded on first use (0x2124F0's role), then the player
bool FReady(SCREENRT* p)
{
    SCREENOBJ* pso = p->pss->pso.get();
    if (pso->fExternal && pso->aelem.empty())
    {
        std::string err;
        if (!FLoadScreenFile(pso, g_disc.ReadWorld(pso->fk), err))
        {
            std::printf("screen 0x%X: file %u: %s\n", pso->oid, pso->fk.id, err.c_str());
            pso->fExternal = 0;   // (not again)
        }
    }
    if (!p->fPlayer && !pso->agroup.empty())
    {
        p->player.Init(pso);
        p->fPlayer = true;
    }
    return p->fPlayer;
}

// orig: 0x002142B0 on the first group
bool FRootHasPart(const SCREENRT* p, int id)
{
    const SCREENOBJ* pso = p->pss->pso.get();
    if (id == -1 || pso->agroup.empty()) return false;
    for (const SPART& part : pso->agroup[0].apart)
        if (part.id == id) return true;
    return false;
}

// orig: 0x00212A50
void SetTimers(SCREENRT* p, int timers)
{
    if (p->timers == timers) return;
    p->timers = timers;
    p->tTimers = g_clock.t3C;
}

// orig: 0x00213830
void StartPart(SCREENRT* p, int k)
{
    if (!FReady(p)) return;
    p->player.StartRootPart(k);
    if (p->fListed) p->fInList = true;   // (0x214300(p, 0))
    p->id7C = k;
}

// orig: 0x00213898
void Stop(SCREENRT* p)
{
    if (p->fPlayer) p->player.StopRoot();
    if (p->fListed) p->fInList = false;   // (0x214300(p, -1))
    p->t88 = 0.0f;
    p->id7C = -1;
}

// orig: 0x00212A70
void SetState(SCREENRT* p, int s)
{
    const int old = p->state;
    if (old == s) return;
    if (static const bool s_f = std::getenv("SLY2N_SCREENPRINT") != nullptr; s_f)
        std::printf("screen %d: state %d -> %d pause cached %d field %u\n", p->pss->oid, old, s, p->fPause ? 1 : 0, p->pss->U32Field(0, 0xB0, 0));
    if (p->fPause)
    {
        if (old < 2 && s >= 2)
        {
            if (std::getenv("SLY2N_TASKDEBUG"))
                for (const auto& [plo, up] : s_mpsrt)
                    if (up.get() == p) std::printf("screen %d: shown, the game paused (t %.3f)\n", plo->oid, g_clock.t);
            if (++s_cpause == 1) SetClockFactor(&g_clock, 7, 0.0f);
        }
        else if (s < 2 && old >= 2)
        {
            if (--s_cpause == 0) SetClockFactor(&g_clock, 7, 1.0f);
        }
    }
    if (old == 0) FReady(p);
    p->state = s;
    p->tState = g_clock.t3C;
    switch (s)
    {
    case 0: Stop(p); break;   // (0x212780: the groups dropped)
    case 1: Stop(p); break;
    case 2: StartPart(p, p->id70); break;
    case 3: StartPart(p, p->id74); break;
    case 4: StartPart(p, p->id78); break;
    default: break;
    }
}

bool FBusy(SCREENRT* p, int k) { return p->fPlayer && p->id7C != -1 && p->player.FGroupBusy(k); }

// orig: 0x00212C50
void Update(SCREENRT* p)
{
    int s = p->state;
    if (s == 1)
    {
        if (p->timers == 2) s = p->id74 == -1 ? 2 : 3;
    }
    else if (p->tTimers != g_clock.t3C)
    {
        if (s == 2)
        {
            if (!FBusy(p, p->id7C)) SetTimers(p, 1);
            if (p->dtMax > 0.0f && p->dtMax < g_clock.t3C - p->tState) SetTimers(p, 1);
            if (p->timers == 1) s = p->id78 == -1 ? 1 : 4;
        }
        else if (s == 3)
        {
            if (!FBusy(p, p->id74))
            {
                if (p->timers == 1) s = p->id78 == -1 ? 1 : 4;
                else if (p->timers == 2) s = 2;
            }
        }
        else if (s == 4)
        {
            if (!FBusy(p, p->id78))
            {
                if (p->timers == 1) s = 1;
                else if (p->timers == 2) s = p->id74 == -1 ? 2 : 3;
            }
        }
    }
    if (static const bool s_f = std::getenv("SLY2N_SCREENPRINT") != nullptr; s_f && p->state >= 2 && p->fPause)
        std::printf("screen update: state %d -> %d timers %d busy %d id7C %d ids %d %d %d dtMax %.2f tState %.2f t3C %.2f\n", p->state, s, p->timers,
                    FBusy(p, p->id7C) ? 1 : 0, p->id7C, p->id70, p->id74, p->id78, p->dtMax, p->tState, g_clock.t3C);
    SetState(p, s);
    if (p->id7C != -1 && p->fPlayer)
    {
        const float dt = g_clock.dt40;
        p->t88 += dt;
        // orig: 0x218220. Project the live world position every update, before the
        // root and nested slots advance through that transform (0x2183E8).
        p->player.mRoot = {1, 0, 0, 1, 0, 0};
        if (p->follow)
        {
            const glm::vec3 screen = ConvertCmWorldToScreen(&g_cm, PosLoWorld(p->follow));
            p->player.mRoot[4] = screen.x * 320.0f + 320.0f;
            p->player.mRoot[5] = screen.y * -246.40000915527344f + 246.40000915527344f;
            if (std::getenv("SLY2N_SCREENFOLLOW"))
                std::printf("screen follow: frame %lld screen %d target %d at %.9g %.9g\n",
                            static_cast<long long>(g_cframeScreen), p->pss->oid, p->follow->oid,
                            p->player.mRoot[4], p->player.mRoot[5]);
        }
        p->player.Update(dt);
    }
}
}  // namespace

// orig: SCREENOBJ +0xF4 (0x202768 assigns the selected photo subject).
void SetScreenObjFollow(LO* plo, LO* target)
{
    if (SCREENRT* p = Psrt(plo)) p->follow = target;
}

static void ShowScreenObjBase(LO* plo, int k)
{
    SCREENRT* p = Psrt(plo);
    if (!p) return;
    if (std::getenv("SLY2_SHOWDBG"))
    {
        std::printf("show screen %d (k %d): args", plo->oid, k);
        for (int a : p->aarg) std::printf(" %d", a);
        std::printf(" opts");
        for (const OPT& o : plo->aopt)
        {
            std::printf(" [%d:", o.eopid);
            for (const OPTVAL& v : o.aval) std::printf(" k%d n%d g%g", v.k, v.n, v.ag[0]);
            std::printf("]");
        }
        std::printf("\n");
    }
    if (const char* sz = std::getenv("SLY2_SOPARTS"); sz && std::strtol(sz, nullptr, 0) == plo->oid && p->pss && p->pss->pso)   // (debug: its parts)
    {
        const SCREENOBJ* pso = p->pss->pso.get();
        for (size_t g = 0; g < pso->agroup.size(); ++g)
        {
            std::printf("screen 0x%X group %zu (%d frames):", plo->oid, g, pso->agroup[g].cframe);
            for (const SPART& part : pso->agroup[g].apart) std::printf(" 0x%X(%zu keys)", part.id, part.akey.size());
            std::printf("\n");
            if (std::getenv("SLY2_SOKEYS"))   // (and every key: its items' slot, piece, interpolation, colours)
                for (const SPART& part : pso->agroup[g].apart)
                    for (const SKEY& key : part.akey)
                    {
                        std::printf("  part 0x%X dt %g key t %.3f:", part.id, part.dt, key.t);
                        for (const SITEM& it : key.aitem)
                        {
                            std::printf(" [slot %u piece %u link %u i%u m%u", it.aw[1], it.aw[0], it.aw[2], it.b30, it.b31);
                            if (it.s6 >= 0 && size_t(it.s6) < key.acolor.size())
                                std::printf(" mul %08X add %08X", key.acolor[size_t(it.s6)][0], key.acolor[size_t(it.s6)][1]);
                            std::printf(" t %.1f %.1f]", it.mat[2][0], it.mat[2][1]);
                        }
                        for (uint8_t b : key.ab) std::printf(" b%u", b);
                        for (uint16_t w : key.aw) std::printf(" rel%u", w);
                        std::printf("\n");
                    }
        }
    }
    if (p->state == 0) SetState(p, 1);
    SetTimers(p, 2);
    p->id70 = k;
    p->id74 = -1;
    p->id78 = -1;
    if (k != -1)
    {
        SetState(p, 1);
        SetState(p, 2);
        return;
    }
    FReady(p);
    p->id70 = 0x314;
    if (FRootHasPart(p, 0x30F)) p->id74 = 0x30F;
    if (FRootHasPart(p, 0x310)) p->id70 = 0x310;
    if (FRootHasPart(p, 0x311)) p->id78 = 0x311;
    if (p->state == 1) Update(p);
}

// orig: 0x00211CA8 (failure screen slot 49), otherwise 0x002128E8.
void ShowScreenObj(LO* plo, int k)
{
    SCREENRT* p = Psrt(plo);
    if (!p) return;
    const bool failure = plo->pcls && plo->pcls->PfnOrig(49) == 0x211CA8;
    if (failure && p->id7C != -1) return;
    ShowScreenObjBase(plo, k);
    if (failure)
    {
        PauseGame(0);
        plo->SetField(0, 0x110, 1);
        SelectScreenObjPart(plo, 0x33A);
    }
}

// orig: 0x00212A30 (shown idle: its idle time starts again)
void RefreshScreenObj(LO* plo)
{
    SCREENRT* p = Psrt(plo);
    if (p && p->state == 2) p->tState = g_clock.t3C;
}

void HideScreenObj(LO* plo)
{
    SCREENRT* p = Psrt(plo);
    if (!p) return;
    SetTimers(p, 1);
    if (p->id78 == -1) SetState(p, 1);
}

void StopScreenObj(LO* plo)
{
    SCREENRT* p = Psrt(plo);
    if (!p) return;
    p->id70 = p->id74 = p->id78 = -1;
    SetTimers(p, 1);
    SetState(p, 1);
}

void SetScreenObjArg(LO* plo, int i, int n)
{
    if (SCREENRT* p = Psrt(plo); p && i >= 0 && i < 10) p->aarg[i] = n;
}

void ShowScreenObjArgs(LO* plo, int carg, const int* aarg)
{
    SCREENRT* p = Psrt(plo);
    if (!p) return;
    for (int i = 0; i < carg && i < 10; ++i) p->aarg[i] = aarg[i];
    ShowScreenObj(plo, -1);
}

void StartScreenObjPart(LO* plo, int k)
{
    if (SCREENRT* p = Psrt(plo)) StartPart(p, k);
}

bool FScreenObjBusy(LO* plo, int k)
{
    SCREENRT* p = Psrt(plo);
    return p && FBusy(p, k);
}

bool FScreenObjIdleLonger(LO* plo, float dt)
{
    SCREENRT* p = Psrt(plo);
    return p && p->state == 2 && p->timers == p->state && g_clock.t3C - p->tState > dt;
}

void SetScreenObjFraction(LO* plo, int k, float g)
{
    SCREENRT* p = Psrt(plo);
    if (p && FReady(p)) p->player.SetPartFraction(k, g);
}

// orig: 0x00212F80 (the part's time; 0x212E40 above is its fraction)
void SetScreenObjTime(LO* plo, int k, float t)
{
    SCREENRT* p = Psrt(plo);
    if (p && FReady(p)) p->player.SetPartTime(k, t);
}

void SelectScreenObjPart(LO* plo, int k)
{
    SCREENRT* p = Psrt(plo);
    if (p && FReady(p)) p->player.SelectPart(k);
}

void ResetScreenObjPart(LO* plo, int k)
{
    SCREENRT* p = Psrt(plo);
    if (p && FReady(p)) p->player.SelectPart(k, true);
}

// orig: 0x002132D0 / 0x002137F0
bool FVisible(const SCREENRT* p)
{
    if (p->id7C == -1) return false;
    for (const auto& [plo, q] : s_mpsrt)
        if (q->fInList && q->serialSw == p->serialSw && p->prio < q->prio) return false;
    return true;
}

void SelectScreenObjPartAt(LO* plo, int k, int match)
{
    SCREENRT* p = Psrt(plo);
    if (p && FReady(p)) p->player.SelectPartAt(k, match);
}
void ResetScreenObjPartAt(LO* plo, int k, int match)
{
    SCREENRT* p = Psrt(plo);
    if (p && FReady(p)) p->player.ResetPartAt(k, match);
}
void SetScreenObjPartMatrix(LO* plo, int k, const std::array<float, 6>& m)
{
    SCREENRT* p = Psrt(plo);
    if (p && FReady(p)) p->player.SetPartMatrix(k, m);
}
// orig: 0x00213DB0
void SelectScreenObjPartHandle(LO* plo, int k, int* pidHandle)
{
    if (k == *pidHandle) return;
    if (*pidHandle != -1) ResetScreenObjPart(plo, *pidHandle);
    *pidHandle = k;
    if (k != -1) SelectScreenObjPart(plo, k);
}

void SetScreenObjParts(LO* plo, int idShow, int idIdle, int idHide)
{
    SCREENRT* p = Psrt(plo);
    if (!p) return;
    p->id74 = idShow;
    p->id70 = idIdle;
    p->id78 = idHide;
}
void SetScreenObjState(LO* plo, int s)
{
    if (SCREENRT* p = Psrt(plo)) SetState(p, s);
}
void SetScreenObjTimers(LO* plo, int timers)
{
    if (SCREENRT* p = Psrt(plo)) SetTimers(p, timers);
}

int CBigScreenObjsShown(const SW*)
{
    int c = 0;
    for (const auto& [plo, p] : s_mpsrt)
        if (p && p->prio >= 16 && p->state >= 2) ++c;
    return c;
}

bool FScreenObjVisible(LO* plo)
{
    SCREENRT* p = Psrt(plo);
    return p && FVisible(p);
}

// orig: 0x002121C8 (gone: no part shown and its timers below 2)
bool FScreenObjDone(LO* plo)
{
    SCREENRT* p = Psrt(plo);
    return !p || (p->state < 2 && p->timers < 2);
}

int NScreenObjState(LO* plo)
{
    SCREENRT* p = Psrt(plo);
    return p ? p->state : 0;
}

void SetScreenObjPauses(LO* plo, bool f)
{
    if (SCREENRT* p = Psrt(plo)) p->fPause = f;
}

// SW +0x476C / +0x4770 (cleared with the SW: ResetScreenObjs of a new world)
float s_tTimerEnd = 0.0f;
WORLD_VALUE(s_tTimerEnd);   // (PC: moved aside while a kept world loads, worldglobals.h)
int s_nTimerTick = 0;
WORLD_VALUE(s_nTimerTick);   // (PC: moved aside while a kept world loads, worldglobals.h)

// orig: 0x0020F7E0 (every frame: the prompt 0x371 and the counters 0x36D / 0x36E / 0x36F (D_002C3E70, found by
// 0x20F710: SW +0x3BA0..) stacked down the screen, 65 apart (D_002C3E80) in that order, the shown ones only: each one's
// transform +0xF8's y smoothed there (0x11D1C0 with D_002C3E88), snapped when it appears; the last (the timer 0x36F)
// ticks when its whole seconds left (SW +0x476C - t, at least 0) change: sound 0x30 on odd, 0x31 on even (SW +0x4770))
void StackCounters(SW* psw)
{
    static const OID s_aoid[4] = {0x371, 0x36D, 0x36E, 0x36F};
    static const cmm::SMPA s_smpa = {500.0f, 0.0f, 0.25f, 2500.0f};
    static float s_adv[4] = {};
    static bool s_afReset[4] = {true, true, true, true};
    float t = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        LO* plo = PloFindSwObjectByOid(psw, s_aoid[i]);
        auto it = plo ? s_mpsrt.find(plo) : s_mpsrt.end();
        SCREENRT* p = it == s_mpsrt.end() ? nullptr : it->second.get();
        if (!p || !FVisible(p))
        {
            s_afReset[i] = true;
            continue;
        }
        if (s_afReset[i])
        {
            p->player.dyOwner = t;
            s_adv[i] = 0.0f;
            s_afReset[i] = false;
        }
        else p->player.dyOwner = cmm::GSmoothA(p->player.dyOwner, s_adv[i], t, g_clock.dt, &s_smpa, &s_adv[i]);
        t += 65.0f;
        if (i == 3)
        {
            const float d = std::max(0.0f, s_tTimerEnd - g_clock.t);
            const int n = static_cast<int>(d);
            if (n != s_nTimerTick)
            {
                PsndStartSound((n & 1) ? 0x30 : 0x31);  // 0x1FB2F8(0x1FBB90(id), 0)
                s_nTimerTick = n;
            }
        }
    }
}

// orig: 0x00211D38: failure-screen update. Its animation and confirmation
// delay use the real clock while the world remains stopped by factor 7.
static void UpdateFailureScreen(SCREENRT* p, SW* psw)
{
    if (p->state < 2 && p->timers < 2) return;
    Update(p);
    // The original also checks the disc-removal message (0x3DA4F0). Native
    // ISO playback has no disc-removal widget; the card widget remains live.
    if (p->state == 2 && 0.5f < g_clock.t3C - p->tState && !FCardMessageActive()
        && (g_joyps2.grfbtnPressed & 0xF0))
    {
        SetJoyPs2BtnHandled(&g_joyps2, 0xF0);  // orig 0x187D80 / 0x187DE0
        ResumeGame(0);
        if (p->pss->U32Field(0, 0x114, 0) == 0 && psw && psw->tasks.pjobCur)
        {
            ++s_cpause;  // keep the world frozen throughout the restart wipe
            RestartJobAtCheckpoint(psw, psw->tasks.pjobCur);  // orig 0x162DF8
        }
        HideScreenObj(p->pss);
    }
    p->pss->SetField(0, 0x110, 1);
}

// The task counters' strings (orig 0x2C3EC0 + 0x30 i: script strings 0x2FF + i, read by 0x210EB0)
char s_aachCounter[10][0x30];


// orig: 0x00211F90 (class 0x81's update, the task counter: e.g. the satellite dishes, the spice plants): its two task
// lists (EOPID 1150 -> +0x110 / +0x118, 1151 -> +0x194 / +0x19C; at most 15) counted by state 1 (open) and done; while
// shown the strings 0x2FF (done of the second list), 0x300 (its count), 0x301 (left); hidden (slot 50) when every
// task is done or none is open, else shown (slot 49, -1)
void UpdateTaskCounter(SCREENRT* p)
{
    Update(p);  // 0x212C50
    auto list = [&](int eopid) {
        std::vector<OID> a;
        for (const OPT& o : p->pss->aopt)
            if (o.eopid == eopid)
                for (const OPTVAL& v : o.aval)
                {
                    if (v.alist.empty()) a.push_back(OID(v.n));
                    for (const OPTVAL& w : v.alist) a.push_back(OID(w.n));
                }
        if (a.size() > 15) a.resize(15);
        return a;
    };
    const std::vector<OID> aA = list(1150), aB = list(1151);
    int ready = 0, readyB = 0, loading = 0;
    for (OID oid : aB)
    {
        bool fDone = false;
        const int n = NTaskStateOid(oid, &fDone);
        if (n == 1) ++loading;
        else if (n != -2 && fDone) { ++ready; ++readyB; }
    }
    for (OID oid : aA)
    {
        bool fDone = false;
        const int n = NTaskStateOid(oid, &fDone);
        if (n == 1) ++loading;
        else if (n != -2 && fDone) ++ready;
    }
    if (FBusy(p, 0x314))  // 0x2121E8(p, 0x314)
    {
        std::snprintf(s_aachCounter[0], sizeof(s_aachCounter[0]), "%d", readyB);
        std::snprintf(s_aachCounter[1], sizeof(s_aachCounter[1]), "%d", int(aB.size()));
        std::snprintf(s_aachCounter[2], sizeof(s_aachCounter[2]), "%d", int(aB.size()) - readyB);
    }
    if (ready == int(aA.size() + aB.size()) || !loading) HideScreenObj(p->pss);  // vt slot 50
    else if (loading > 0) ShowScreenObj(p->pss, -1);                          // vt slot 49
}

void UpdateScreenObjs(SW* psw)
{
    // (debug: SLY2N_SCREENLOOPSCAN: once per world, every screen object's looping key sounds: splayer.cpp)
    if (static const bool s_fScan = std::getenv("SLY2N_SCREENLOOPSCAN") != nullptr; s_fScan && psw)
        if (static uint32_t s_serialScanned = 0; s_serialScanned != psw->serial)
        {
            s_serialScanned = psw->serial;
            void PrintScreenLoopers(const SCREENOBJ* pso);
            for (const auto& aplo : psw->mpcidaplo)
                for (LO* plo : aplo)
                    if (SPRITESET* pss = dynamic_cast<SPRITESET*>(plo); pss && pss->pso)
                    {
                        SCREENOBJ so = *pss->pso;
                        std::string err;
                        if (so.fExternal && so.aelem.empty() && !FLoadScreenFile(&so, g_disc.ReadWorld(so.fk), err)) continue;
                        PrintScreenLoopers(&so);
                    }
        }
    // A failure confirmation can start a previously unused wipe screen.
    // Keep stable player pointers while that screen joins the runtime map.
    std::vector<SCREENRT*> active;
    for (auto& [plo, p] : s_mpsrt)
        if ((p->state >= 2 || p->timers == 2) && !(p->pss->pcls && p->pss->pcls->PfnOrig(16) == 0x211F90)) active.push_back(p.get());
    // the task counters (class 0x81) update shown or not: they show themselves while one of their tasks is open
    std::vector<SCREENRT*> acounter;
    if (psw)
        for (LO* plo : psw->mpcidaplo[0x81])
            if (plo->pcls && plo->pcls->PfnOrig(16) == 0x211F90 && !plo->fRemoved)
                if (SCREENRT* p = Psrt(plo)) acounter.push_back(p);
    for (SCREENRT* p : active)
        if (p->pss->pcls && p->pss->pcls->PfnOrig(16) == 0x211D38) UpdateFailureScreen(p, psw);
        else Update(p);
    for (SCREENRT* p : acounter) UpdateTaskCounter(p);
    if (psw) StackCounters(psw);
}

namespace
{
std::vector<FONTBRX> s_afont;   // (the HUD's fonts: the text and the button icons; found at a world's first draw)
uint32_t s_serialFonts = 0;
}
const FONTBRX* PfontHud()
{
    if (s_afont.empty()) return nullptr;
    // (PC, langstore.h) Japanese text on a USA / European disc: the languages folder's Japanese font in the place of
    // the world's text font (the button icons stay the world's)
    if (const SELEM* pset = PsetLanguageStoreFont())
    {
        static FONTBRX s_fontStore;
        s_fontStore = s_afont[0];
        s_fontStore.pset = pset;
        return &s_fontStore;
    }
    return &s_afont[0];
}
const FONTBRX* PfontHudIcons() { return s_afont.size() > 1 && s_afont[1].pso == s_afont[0].pso ? &s_afont[1] : nullptr; }

// (QoL) SLY2N_WIPEMASKFIX=0: the wipe's mask widened as before
static bool NoWipeMaskFix()
{
    static const bool s_f = [] { const char* sz = std::getenv("SLY2N_WIPEMASKFIX"); return sz && sz[0] == '0'; }();
    return s_f;
}

namespace { bool s_fBinocFrameDrawn = false; }
bool FBinocFrameDrawn() { return s_fBinocFrameDrawn; }
void DrawScreenObjs(SW* psw)
{
    if (!psw) return;
    std::vector<std::shared_ptr<SCREENOBJ>> aso;
    for (LO* plo : AploOfClass(psw, 0x7E))
        if (auto pso = static_cast<SPRITESET*>(plo)->pso) aso.push_back(pso);
    if (s_serialFonts != psw->serial)
    {
        s_afont = FindFonts(aso);
        s_serialFonts = psw->serial;
        if (std::getenv("SLY2N_FONTLIST"))
            for (const FONTBRX& f : s_afont)
            {
                std::printf("font obj 0x%X elem %d:", f.pso->oid, f.ifont);
                for (size_t i = 0; i < f.pset->aul.size(); ++i)
                {
                    std::string ch;
                    AppendCh16(ch, f.pset->aul[i]);   // (UTF-8: the Japanese build's 16-bit characters)
                    std::printf(" %s(%.0f)", ch.c_str(), f.pset->ag[i]);
                }
                std::printf("\n");
            }
    }
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    s_pswText = psw;
    std::string (*pfnOld)(int) = g_pfnScriptString;
    g_pfnScriptString = StrScreenScript;
    // in the world's order (the objects' draw: 0x212C50's 0x218220 queues each at its depth)
    // the visible ones (0x213300's test) at their depths (+0xA4 << 18: the higher on top)
    std::vector<std::pair<LO*, SCREENRT*>> adraw;
    for (LO* plo : AploOfClass(psw, 0x7E))
    {
        auto it = s_mpsrt.find(plo);
        if (it == s_mpsrt.end()) continue;
        SCREENRT* p = it->second.get();
        if (!p->fPlayer || !FVisible(p)) continue;
        if (!plo->fInWorld) continue;   // (removed from the world: not in its draw list, e.g. a PO's 50724 at its init)
        adraw.emplace_back(plo, p);
    }
    // (DrawRpl sorts the kind-21 records with 0x157A98: ascending depth, equal depths in reverse record order. The slots
    // of one object have distinct depths (splayer.cpp); objects of one priority tie slot for slot, so the later queued
    // object's slot is drawn first and the first one's ends on top (the HUD's depth test is GEQUAL: last drawn wins).)
    std::reverse(adraw.begin(), adraw.end());
    s_fBinocFrameDrawn = std::any_of(adraw.begin(), adraw.end(), [](const auto& e) { return e.first->oid >= 0x35F && e.first->oid <= 0x361; });
    std::stable_sort(adraw.begin(), adraw.end(), [](const auto& a, const auto& b) { return a.second->prio < b.second->prio; });
    for (auto [plo, p] : adraw)
    {
        s_psrtDrawing = p;
        static const bool s_fDbg = std::getenv("SLY2_HUDDBG") != nullptr;
        if (s_fDbg) std::printf("screen 0x%X: state %d part 0x%X a4 %d a8 %d ac %d b0 %d\n", plo->oid, p->state, p->id7C, int(plo->U32Field(0, 0xA4, 0x7777)),
                                int(plo->U32Field(0, 0xA8, 0x7777)), int(plo->U32Field(0, 0xAC, 0x7777)), int(plo->U32Field(0, 0xB0, 0x7777)));
        g_zScreenBase = float(std::max(0, p->prio)) * 262144.0f;   // (+0xA4 << 18)
        g_hud.fWideEdgesObj = plo->oid >= 0x35F && plo->oid <= 0x361;   // (the binocucom frames: Sly's, Bentley's, Murray's)
        glm::vec2 uiBinoc;
        g_hud.fVrBandsObj = g_hud.fWideEdgesObj && vr::FBinocCanvas(&uiBinoc);   // (VR: bands above / below, hud.h)
        const bool fFourThree = FFourThreeMenuObj(plo);   // (qolview.h: the gadget grid, Job Help)
        g_hud.fClipCanvasObj = FClipEpisodeMenuToCanvas(plo) || fFourThree;   // (frontend.h: the episode menu on a wide picture)
        g_hud.fWideRectsOnlyObj = plo->oid == 0x30A && !NoWipeMaskFix();   // (the wipe's Sly mask)
        // (VR, vr.h: in the Immersive view the subtitle box's letterbox bars (the scripted pauses: the safe house's door,
        // Ep1's arrival) would sit in front of the eyes as two black slabs; the Flat Screen view and flat play keep them)
        g_hud.fNoBarsObj = plo->oid == 0x373 && vr::FImmersiveView() && !vr::FTitleMoment();   // (the title keeps its bars)
        p->player.Draw(pfont, picons, psw);
        g_hud.EndWideEdgesObj();
        if (fFourThree) g_hud.Pillarbox(glm::vec4(0, 0, 0, 128), (g_zScreenBase + 262000.0f) / 16777216.0f);   // (over the world, under higher priorities)
        g_hud.fWideRectsOnlyObj = false;
        g_hud.fNoBarsObj = false;
        g_hud.fWideEdgesObj = false;
        g_hud.fVrBandsObj = false;
        g_hud.fClipCanvasObj = false;
    }
    s_psrtDrawing = nullptr;
    {
        int prioMax = 0;
        for (auto [plo, p] : adraw) prioMax = std::max(prioMax, p->prio);
        g_zScreenBase = float(prioMax + 1) * 262144.0f;
    }
    DrawGadgetIcons(pfont, picons);   // (the gadget widget's draw, 0x1DAC58)
    DrawMarkerLabels(pfont, picons);  // (the job markers' names, 0x1CD780: marker.cpp)
    DrawDancePrompts(pfont, picons);  // (the dance director's buttons, 0x152C58: dance.cpp)
    if (pfont && std::getenv("SLY2N_ICONSTRIP"))
        g_hud.DrawText(*pfont, picons, std::getenv("SLY2N_ICONSTRIP"), -300.0f, -40.0f, 40.0f, 1.0f, 1.0f, 0, glm::vec4(102.0f), glm::vec4(1.0f), glm::vec4(0.0f));
    g_zScreenBase = 0.0f;
    g_pfnScriptString = pfnOld;
}

// orig: 0x001618F8
void StartHudTimer(float dt, SW* psw)
{
    if (!psw) psw = g_psw;
    if (LO* plo = psw ? PloFindSwObjectByOid(psw, 0x36F) : nullptr)
        if (SCREENRT* p = Psrt(plo))
        {
            p->dtMax = 0.0f;  // +0x8C: no idle limit
            ShowScreenObj(plo, -1);
        }
    s_tTimerEnd = g_clock.t + dt;
}

// orig: 0x00161958
void StopHudTimer()
{
    if (LO* plo = g_psw ? PloFindSwObjectByOid(g_psw, 0x36F) : nullptr) HideScreenObj(plo);
    s_tTimerEnd = 0.0f;
}

float THudTimerEnd() { return s_tTimerEnd; }

float DtHudTimerResume(int grfReq)
{
    if (grfReq & 0xCD) return 0.0f;
    return std::max(0.0f, s_tTimerEnd - g_clock.t);
}

// orig: 0x001D9408: reset the count before the old world's HUD is released.
void ResetScreenObjPauseCount() { s_cpause = 0; }

void ResetScreenObjs(SW* psw)
{
    if (!psw)  // (the old world's end: the SW cleared by 0x20E890's memset of 0x4790 bytes)
    {
        s_tTimerEnd = 0.0f;
        s_nTimerTick = 0;
    }
    // the states of the old worlds' objects dropped (the wipe started by the transition into this one kept)
    const size_t cBefore = s_mpsrt.size();
    for (auto it = s_mpsrt.begin(); it != s_mpsrt.end();)
    {
        if (!psw || it->second->serialSw != psw->serial)
        {
            if (it->second->fPause && it->second->state >= 2 && s_cpause > 0 && --s_cpause == 0) SetClockFactor(&g_clock, 7, 1.0f);
            it = s_mpsrt.erase(it);
        }
        else ++it;
    }
    if (std::getenv("SLY2N_SCREENLIFE"))
        std::fprintf(stderr, "screen lifetime: reset world %u removed %zu remaining %zu pauses %d\n",
                     psw ? psw->serial : 0, cBefore - s_mpsrt.size(), s_mpsrt.size(), s_cpause);
}

std::string StrHudPromptText();   // hudwidget.cpp (0x2113E0's string)

std::string StrScreenScript(int id)
{
    char ach[64];
    if (id >= 0x2DD && id <= 0x2E4) return StrPhotoScript(id);
    if (id >= 0x2FF && id <= 0x308) return s_aachCounter[id - 0x2FF];  // orig 0x210EB0 (0x2C3EC0 + 0x30 i)
    // orig: 0x00210F80 (0x2D1): the HUD timer's time left "%d:%02d" (D_002A8AB0; SW +0x476C - t, at least 0)
    if (id == 0x2D1)
    {
        const float t = std::max(0.0f, s_tTimerEnd - g_clock.t);
        const int n = static_cast<int>(t);
        const int q = n / 60;
        std::snprintf(ach, sizeof(ach), "%d:%02d", q, n - q * 60);
        return ach;
    }
    // orig: 0x0021AA60's last generator: 0x5C1 = 0x210EB0 on D_002C40A0's string 0x2A8AE0 (the episode menu's
    // "view the movie" hint: the SQUARE button icon)
    if (id == 0x5C1)
    {
        const uint32_t va = g_disc.U32Elf(0x2C40A0);
        if (const uint8_t* pb = g_disc.PbElf(va, 1)) return reinterpret_cast<const char*>(pb);
        return "";
    }
    // orig: 0x00210ED8 (0x2CF): the coins (the progress "loot ccoin", SW +0x3B20)
    if (id == 0x2CF)
    {
        const CRef ref = RefGetProgress({"loot", "ccoin"});
        if (ref.m_tagk != TAGK_S32) return "";
        std::snprintf(ach, sizeof(ach), "%d", ref.m_n);
        return ach;
    }
    // orig: 0x00210F50 (0x2D0): the world's clue bottles "%d/%d" (SW +0x4764 collected (0x1C7D08, again at each bottle
    // by 0x1C7C30) / +0x4768 all (0x1C7D00, 30))
    if (id == 0x2D0)
    {
        int CClueCollectedCur();   // vault.cpp
        int CClueNeededCur();
        std::snprintf(ach, sizeof(ach), "%d/%d", CClueCollectedCur(), CClueNeededCur());
        return ach;
    }
    // orig: 0x00210FE0 (0x2DC): the binocucom's readout, by the current player's class: its magnification (the view's
    // policy's +0x28 / +0xB4 (D_002DE3B8 = CM +0x478 recon, D_002DE514 = CM +0x5D4 free) x the binocucom's +0x3C)
    // as "x%d" (Sly), "x%5.3f" (Bentley) or Far / Near / YIKES by its whole part (Murray: below 4, below 8)
    if (id == 0x2DC)
    {
        LO* plo = s_pswText ? PloPoCurrent(const_cast<SW*>(s_pswText)) : nullptr;
        ACTOR* pa = plo && FCidDerivedFrom(plo->Cid(), 0x34) ? PactorFromLo(plo) : nullptr;
        if (!pa) return "";
        float g = 0.0f;
        switch (pa->nF20)
        {
        case 0: g = g_cmpolicies.cprecon.sZoom * pa->binoc.g3C; break;
        case 1: g = g_cmpolicies.cpfree.sZoom * pa->binoc.g3C; break;
        default: break;
        }
        if (FCidDerivedFrom(plo->Cid(), 0x35)) std::snprintf(ach, sizeof(ach), "x%d", static_cast<int>(g));
        else if (FCidDerivedFrom(plo->Cid(), 0x37)) std::snprintf(ach, sizeof(ach), "x%5.3f", static_cast<double>(g));
        else if (FCidDerivedFrom(plo->Cid(), 0x36))
        {
            const int n = static_cast<int>(g);
            std::snprintf(ach, sizeof(ach), "%s", n < 4 ? "Far" : n < 8 ? "Near" : "YIKES");
        }
        else return "";
        return ach;
    }
    // orig: 0x00211348 (0x2E5..0x2EE): the localized string of argument i
    if (id >= 0x2E5 && id <= 0x2EE)
    {
        if (!s_psrtDrawing) return "";
        return StrLocalized(s_pswText, s_psrtDrawing->aarg[id - 0x2E5]);
    }
    // orig: font_brx's 0x2EF (0x2277D0 + 0x227820 on 0x4435F0): the dialog's subtitle (sound.h)
    if (id == 0x2EF)
    {
        std::string str;
        uint32_t rgba = 0;
        return FDialogSubtitle(&str, &rgba) ? str : std::string();
    }
    // orig: 0x002113B8 (0x2F0): the prompt's buttons (0x227940 on the subtitle state's +8 flags: each of the table
    // D_002A97F8's 16 {mask, glyph} set appends "&2<glyph>&." (D_002A9878))
    if (id == 0x2F0)
    {
        static const struct { uint32_t mask; char ch; } s_a[16] = {
            {0x40, 'X'}, {0x20, 'O'}, {0x10, 'T'}, {0x80, 'S'}, {0x800, '!'}, {0x100, '_'}, {0x1000, '^'}, {0x4000, 'v'},
            {0x8000, '<'}, {0x2000, '>'}, {0x4, 'L'}, {0x1, 'l'}, {0x200, '('}, {0x8, 'R'}, {0x2, 'r'}, {0x400, ')'}};
        std::string str;
        for (const auto& e : s_a)
            if (g_dialogsub.grf & e.mask) str += std::string("&2") + e.ch + "&.";
        return str;
    }
    // orig: 0x002113E0 (0x2F1): the HUD prompt's string (0x3DA0C0 +0x18)
    if (id == 0x2F1) return StrHudPromptText();
    // orig: 0x00211420 (0x2F4): the prize title card's name (U28): the current player's interaction of kind 12 (the
    // prize's pose) names its object: a loot (0x5E) by its definition's row of the loot table 0x2C3288 (+4 the
    // string), a prize (0x5C) by its +0x224, else the actor's +0x1318
    if (id == 0x2F4)
    {
        int IlootIndexOfPickup(LO*);  // pickup.cpp
        int ids = -1;
        LO* plo = s_pswText ? PloPoCurrent(const_cast<SW*>(s_pswText)) : nullptr;
        if (plo && FCidDerivedFrom(plo->Cid(), 0x34))
            if (ACTOR* pa = PactorFromLo(plo); pa && pa->inter.ploTarget && pa->n4C8 == 0xC)
            {
                LO* py = pa->inter.ploTarget;
                if (FCidDerivedFrom(py->Cid(), 0x5E))
                {
                    const int iloot = IlootIndexOfPickup(py);
                    if (iloot >= 0) ids = static_cast<int>(g_disc.U32Elf(0x2C3288 + 0xC * uint32_t(iloot) + 4));
                }
                else if (FCidDerivedFrom(py->Cid(), 0x5C)) ids = int(py->U32Field(0, 0x224, uint32_t(-1)));
                else ids = pa->n1318;
            }
        return ids != -1 ? StrLocalized(s_pswText, ids) : std::string();
    }
    // orig: 0x001DDFB0 (0x5D5): the current game's percent "%d%%" (D_002A7AB8; 0x1D82B8 on the save 0x3D4A60)
    if (id == 0x5D5)
    {
        std::snprintf(ach, sizeof(ach), "%d%%", NGamePercent(g_gamesave));
        return ach;
    }
    // orig: 0x001DDF60 (0x5D7): the world's string 0x40 (0x13E368) then " : &2T&." (D_002A7AA8)
    if (id == 0x5D7) return StrLocalized(s_pswText, IdText(0x40)) + " : " + SzMenuBackGlyph();
    if (std::string str = StrMenuScript(id); !str.empty()) return str;
    if (std::string str = StrShopScript(id); !str.empty()) return str;
    if (std::string str = StrGadgetScript(id); !str.empty()) return str;
    if (std::string str = StrJobHelpScript(id); !str.empty()) return str;
    return StrFrontEndScript(id);   // (orig: the other table entries; "Unknown" when absent)
}

// The screen objects' script methods (the splicewrap thunks 0x10DAF0..0x10DEF8: their CRefs by 0x201360, a void
// result)
#include "objmethod.h"
namespace
{
CRef RefVoid()
{
    CRef r;
    r.SetTag(TAGK_Void);
    return r;
}
template <int I>
void SetArgOpt(LO* plo, const CRef& r)
{
    if (SCREENRT* p = Psrt(plo)) p->aarg[I] = r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_n;
}
struct INITSCREENMETHODS
{
    INITSCREENMETHODS()
    {
        RegisterObjectMethod(0x10DAF0, [](LO* plo, const std::vector<CRef>&) { ShowScreenObj(plo, -1); return RefVoid(); });   // vt slot 49 (-1)
        RegisterObjectMethod(0x10DB78, [](LO* plo, const std::vector<CRef>&) { HideScreenObj(plo); return RefVoid(); });        // vt slot 50
        RegisterObjectMethod(0x10DBF8, [](LO* plo, const std::vector<CRef>& a) { SelectScreenObjPart(plo, NArg(a, 0, -1)); return RefVoid(); });  // 0x213B00
        RegisterObjectMethod(0x10DC80, [](LO* plo, const std::vector<CRef>& a) { ResetScreenObjPart(plo, NArg(a, 0, -1)); return RefVoid(); });   // 0x213928
        RegisterObjectMethod(0x10DD08, [](LO* plo, const std::vector<CRef>& a) {   // 0x212E40 (the fraction, the part)
            SetScreenObjFraction(plo, NArg(a, 1, -1), GArg(a, 0, 0.0f));
            return RefVoid();
        });
        RegisterObjectMethod(0x10DDB0, [](LO* plo, const std::vector<CRef>& a) {   // 0x212F80 (the time, the part): method 983
            SetScreenObjTime(plo, NArg(a, 1, -1), GArg(a, 0, 0.0f));
            return RefVoid();
        });
        // options 1133..1142: the arguments +0xCC.. one by one (e.g. the vault's script 1618 sets the power-up screen
        // 4334's first to the reward's name before 4322 shows it)
        static const PFNOBJSETTER s_apfn[10] = {SetArgOpt<0>, SetArgOpt<1>, SetArgOpt<2>, SetArgOpt<3>, SetArgOpt<4>,
                                                SetArgOpt<5>, SetArgOpt<6>, SetArgOpt<7>, SetArgOpt<8>, SetArgOpt<9>};
        for (int i = 0; i < 10; ++i) RegisterObjectSetter(1133 + i, s_apfn[i]);
        RegisterObjectMethod(0x10DE58, [](LO* plo, const std::vector<CRef>& a) {   // 0x212828: the arguments (+0xCC), shown
            const std::vector<int> an = AnArgList(a, 0);
            ShowScreenObjArgs(plo, int(std::min<size_t>(an.size(), 10)), an.data());
            return RefVoid();
        });
    }
} s_initscreenmethods;
}  // namespace

// orig: 0x00214300
void SetScreenObjListed(LO* plo, bool listed)
{
    if (SCREENRT* p = Psrt(plo)) p->fInList = listed;
}

// (debug: a screen object's runtime state, for SLY2N_JOBSEQPRINT)
void PrintScreenObjState(LO* plo)
{
    SCREENRT* p = Psrt(plo);
    if (!p) { std::printf("  screen %d: none\n", plo ? plo->oid : -1); return; }
    std::printf("  screen %d: state %d timers %d id7C 0x%X player %d t3C %.3f tState %.3f\n", plo->oid, p->state, p->timers, p->id7C,
                int(p->fPlayer), g_clock.t3C, p->tState);
    int g = -1, i = -1;
    if (p->player.pso)
        for (size_t gg = 0; gg < p->player.pso->agroup.size() && g < 0; ++gg)
            for (size_t ii = 0; ii < p->player.pso->agroup[gg].apart.size(); ++ii)
                if (p->player.pso->agroup[gg].apart[ii].id == 0x30F) { g = int(gg); i = int(ii); break; }
    if (g >= 0)
    {
        const auto& part = p->player.pso->agroup[size_t(g)].apart[size_t(i)];
        const auto& e = p->player.agroup[size_t(g)].aentry[0];
        std::printf("    group %d part %d (dt %.3f, %zu keys) entry0 ipart %d ikey %d t %.3f trig %d; keys", g, i, part.dt, part.akey.size(), e.ipart, e.ikey, e.t, int(e.fTrig));
        for (const auto& k : part.akey) { std::printf(" [%.3f", k.t); for (uint8_t c : k.ab) std::printf(" %d", c); std::printf("]"); }
        std::printf("; parts");
        for (const auto& pp : p->player.pso->agroup[size_t(g)].apart) std::printf(" 0x%X/%.3f/%zu", pp.id, pp.dt, pp.akey.size());
        std::printf("\n");
    }
}
