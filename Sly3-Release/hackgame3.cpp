// hackgame3.cpp: Sly 3's two-player hacking game on PC (hackgame3.h; notes/HACK3_2026-11-12.md "Round 7").

#include "hackgame3.h"

#include "activate.h"
#include "alo.h"
#include "brx.h"
#include "so.h"
#include "challenge3.h"
#include "clock.h"
#include "cm.h"
#include "cplcy.h"
#include "emitter.h"
#include "eval.h"
#include "objmethod.h"
#include "game/sly3.h"
#include "frame.h"
#include "gc.h"
#include "shd.h"
#include "glob.h"
#include "gauge3.h"
#include "hack.h"
#include "hackmp3.h"
#include "lo.h"
#include "pcloader.h"
#include "po.h"
#include "ref.h"
#include "spliceglue.h"
#include "spliceobj.h"
#include "splitscreen3.h"
#include "sw.h"
#include "sm.h"
#include "smrun.h"
#include "vecmat.h"
#include "game/task3.h"
#include "game/scptgame3.h"
#include "game/scptrun3.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);   // mat.h (orig3 0x11CBA0)
void PrintRef(const CRef& ref, std::string* ps);       // bif.cpp (debug)

namespace hackgame3
{
namespace
{
constexpr int kMaxShips = 4;

bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_HACKGAMEPRINT") != nullptr;
    return s_f;
}

struct GAME
{
    const SW* psw = nullptr;
    uint32_t serial = 0;
    std::array<LO*, kMaxShips> aship{};
    int cship = 0;
    float tLast = -1.0f;
};
GAME s_g;

bool FCurrent(const SW* psw) { return psw && s_g.psw == psw && s_g.serial == psw->serial && s_g.cship > 0; }

// ---- the camera framing the ships (orig3 0x19F8B0, its update 0x19F9C8) ----
// The ship's focus (vt slot 82 0x19BF08) with the other ship (+0x30C) makes a focus object of kind 0xC (0x50 bytes,
// vtable 0x35A248): its point the ships' midpoint smoothed (0x120C80 with {500, 0, 1}), the camera above it at a height
// that frames their distance ((distance + 250) / tan(fov / 2) limited by the ship's +0x310, smoothed up {250, 0.25, 0.5}
// / down {500, 0, 0.5}), 500 back along -x (D_00317530 (1, 0, 0) x -500), looking at the point (0x11CBA0 -> 0x166DE8).
// PC, three / four ships: the point is the middle of the ships' extent, the distance their widest pair (two ships: the
// original's midpoint and distance).
struct CPHACKGAME3 : CPLCY
{
    glm::vec3 pos{0.0f};
    float gHeight = 0.0f;   // (+0x40)
    bool fInit = false;
    uint32_t VtOrig() const override { return 0x35A248; }
    void Update(CPDEFI*, const CMJOY*) override;
};
CPHACKGAME3 s_cp;
const SW* s_pswCam = nullptr;
uint32_t s_serialCam = 0;

void CPHACKGAME3::Update(CPDEFI*, const CMJOY*)
{
    if (!FCurrent(g_psw)) return;
    glm::vec3 lo(0.0f), hi(0.0f);
    int c = 0;
    float len = 0.0f;
    std::array<glm::vec3, kMaxShips> apos{};
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        if (!p || !p->fInWorld) continue;
        const glm::vec3 v = PosLoWorld(p);
        for (int k = 0; k < c; ++k)
            len = std::max(len, glm::length(glm::vec2(v.x - apos[size_t(k)].x, v.y - apos[size_t(k)].y)));
        apos[size_t(c)] = v;
        lo = c ? glm::min(lo, v) : v;
        hi = c ? glm::max(hi, v) : v;
        ++c;
    }
    if (c == 0) return;
    const glm::vec3 mid = 0.5f * (lo + hi);
    static const cmm::SMP s_smpPos{500.0f, 0.0f, 1.0f};
    static const cmm::SMP s_smpUp{250.0f, 0.25f, 0.5f};
    static const cmm::SMP s_smpDown{500.0f, 0.0f, 0.5f};
    const LM lm = LmHackShipCam3(s_g.aship[0]);
    auto limit = [&](float g) { return std::clamp(g, lm.gMin, lm.gMax); };   // 0x11EE68
    if (!fInit)
    {
        // (0x19F8B0: the point the midpoint, the height the camera's own above it, limited)
        pos = mid;
        gHeight = limit(g_cm.pos.z - mid.z);
        fInit = true;
    }
    if (g_cm.fCut) pos = mid;
    pos = cmm::PosSmooth(pos, mid, g_clock.dt, &s_smpPos, nullptr);
    glm::vec3 below = pos + glm::vec3(1.0f, 0.0f, 0.0f) * -500.0f;
    const float t = std::tan(g_cm.radFOV * 0.5f);
    float g = limit((len + 250.0f) / std::max(t, 0.01f));
    g = cmm::GSmooth(gHeight, g, g_clock.dt, g < gHeight ? &s_smpUp : &s_smpDown, nullptr);
    gHeight = g;
    below.z += g;
    SetCmPosMat(&g_cm, below, BuildLookAtMatrixZ(pos - below));
    if (FPrint() && int(g_clock.t * 60.0f) % 60 == 0)
        std::printf("hackgame3: camera %.0f %.0f %.0f -> %.0f %.0f %.0f (ships %d, spread %.0f, height %.0f, fov %.3f) cm %.0f %.0f %.0f\n", below.x, below.y, below.z,
                    pos.x, pos.y, pos.z, c, len, g, g_cm.radFOV, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z);
}

void SyncCamera()
{
    const bool fOn = FCurrent(g_psw) && s_g.aship[0] && s_g.aship[0]->fInWorld && PloPoCurrent(g_psw) == s_g.aship[0] &&
                     !std::getenv("SLY3_HACKGAMENOCAM");
    const bool fSet = s_pswCam && g_psw == s_pswCam && s_serialCam == g_psw->serial;
    if (fOn && !fSet)
    {
        s_cp.fInit = false;
        SetCmPolicy(&g_cm, 7, &s_cp, s_g.aship[0], nullptr);
        s_pswCam = g_psw, s_serialCam = g_psw->serial;
        if (FPrint()) std::printf("hackgame3: camera on (%d ships)\n", s_g.cship);
    }
    else if (!fOn && fSet)
    {
        RevokeCmPolicy(&g_cm, 0xF, 7, &s_cp, s_g.aship[0], nullptr);
        s_pswCam = nullptr;
        if (FPrint()) std::printf("hackgame3: camera off\n");
    }
}

CRef RefOpt(LO* plo, int optid)
{
    if (!plo || !g_splicegame.pfnRefOption) return CRef();
    return g_splicegame.pfnRefOption(111, plo, optid, nullptr);
}
int NOptRef(LO* plo, int optid, int nDefault)
{
    const CRef r = RefOpt(plo, optid);
    if (r.m_tagk == TAGK_S32) return r.m_n;
    if (r.m_tagk == TAGK_F32) return int(r.m_g);
    if (r.m_tagk == TAGK_Bool) return r.m_bool;
    return nDefault;
}

void SetOptRef(LO* plo, int optid, const CRef& r)
{
    if (plo && g_splicegame.pfnRefOption) g_splicegame.pfnRefOption(110, plo, optid, &r);
}
std::shared_ptr<CFrame>& Pframe()
{
    static std::shared_ptr<CFrame> s_pframe;
    if (!s_pframe)
    {
        s_pframe = PframeNew();
        g_gc.AddRootFrame(s_pframe.get());
    }
    return s_pframe;
}
CRef CallRef(const CRef& refProc, std::vector<CRef> aref)
{
    if (refProc.m_tagk != TAGK_Proc && refProc.m_tagk != TAGK_Method) return CRef();
    return RefApply(refProc, int(aref.size()), aref.data(), Pframe().get());
}
CRef CallOpt(LO* plo, int optid, std::vector<CRef> aref = {})
{
    return CallRef(RefOpt(plo, optid), std::move(aref));
}
CRef RefN(int n) { CRef r; r.SetS32(n); return r; }
CRef RefB(bool f) { CRef r; r.SetBool(f); return r; }

bool FAlive(LO* p) { return p && p->fInWorld && HackShip3State(p) >= 0 && HackShip3State(p) != 6; }

// (PC multiplayer, user design: the "between two players" power-ups chain the players in seat order, P1 -> P2 -> ... ->
// PN -> P1, a dead / out player skipped; two left: one link (the original's): s3-split's hackmp3::CChainLinks)
LO* PloChainNext(int i)
{
    bool afIn[kMaxShips] = {};
    for (int j = 0; j < s_g.cship; ++j) afIn[j] = FAlive(s_g.aship[size_t(j)]);
    int alink[kMaxShips][2];
    const int c = hackmp3::CChainLinks(afIn, s_g.cship, alink);
    for (int k = 0; k < c; ++k)
        if (alink[k][0] == i) return s_g.aship[size_t(alink[k][1])];
    // (two living players: the one link names the first; the second's partner is the first, as the original's +0x30C)
    if (c == 1 && alink[0][1] == i) return s_g.aship[size_t(alink[0][0])];
    return nullptr;
}

// the players' colours on the ship model (the ship's options 2463 body / guns / glow and 2464 the arrows and the mirror
// shots, its method 2465 shows the globs: 25 Bentley's green (93016 ...), 31 Penelope's pink (93014 ..., shaders
// 0x16BD8..A), 14259 Sly's blue (93012 ..., 0x16BDB..D); the job's s1621 gives player 1 green / the arrows in player 2's
// colour, player 2 blue (pink once a progress count passes 5: Penelope after Episode 3) / green arrows: each
// ship's 2464 is its partner's colour). PC: each player's chosen character (hackmp3.h, the original's defaults); the
// characters without a set of their own take the blue one (the select's cards recolour it; not in game yet).
int NCodeChar(int ichar)
{
    switch (ichar)
    {
    case hackmp3::CHAR_Bentley: return 25;
    case hackmp3::CHAR_Penelope: return 31;
    default: return 14259;
    }
}
// The players' gauges (PC: each player's chosen character's own gauge, user design): Bentley / Sly / Penelope the hack's
// (h_hack's 92988 authored top left, 92986 / 92984 top right: the original's two players, the job's s1621 names the
// second's), Carmelita / Murray / the Panda King their gauges of the story (class 0xA3 under the character: v_ext's 7465
// under Carmelita 28 and 6094 under Murray 26, c_ext's 7465 under the Panda King 30; authored top left), copied in from
// their worlds kept (pcloader.h). Each in its player's corner: player 1 top left, 2 top right (the original's), 3 bottom
// left, 4 bottom right; moved from where it is authored by its box (640 x 480 canvas units, measured on 4:3 frames: the hack's ovals 18..158 /
// 475..621 x 7..82, the story's 16..160 x 9..91 with the bar under the oval): the other side mirrored, the bottom mirrored.
struct GAUGEDEF
{
    int iwrec = -1;      // -1: the running world (h_hack)
    int oid = -1;
    int oidParent = -1;
    bool fRight = false; // authored on the right
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;   // its box where authored (the portrait and the bar)
};
GAUGEDEF GaugeDefChar(int ichar)
{
    switch (ichar)
    {
    case hackmp3::CHAR_Bentley: return GAUGEDEF{-1, 92988, -1, false, 18.0f, 158.0f, 7.0f, 82.0f};
    case hackmp3::CHAR_Sly: return GAUGEDEF{-1, 92986, -1, true, 475.0f, 621.0f, 7.0f, 82.0f};
    case hackmp3::CHAR_Penelope: return GAUGEDEF{-1, 92984, -1, true, 475.0f, 621.0f, 7.0f, 82.0f};
    case hackmp3::CHAR_Carmelita: return GAUGEDEF{3, 7465, 28, false, 16.0f, 160.0f, 9.0f, 91.0f};
    case hackmp3::CHAR_Murray: return GAUGEDEF{3, 6094, 26, false, 16.0f, 160.0f, 9.0f, 91.0f};
    case hackmp3::CHAR_PandaKing: return GAUGEDEF{23, 7465, 30, false, 16.0f, 160.0f, 9.0f, 91.0f};
    default: return GAUGEDEF{};
    }
}
LO* PloKeptGauge(const GAUGEDEF& d)
{
    SW* pswk = PswKeptWorld(d.iwrec);
    if (!pswk) return nullptr;
    for (const auto& up : pswk->aploAll)
        if (up->oid == d.oid && up->Cid() == 0xA3 && up->paloParent && up->paloParent->oid == d.oidParent) return up.get();
    return nullptr;
}
std::array<LO*, kMaxShips> s_agauge{};   // (the gauge each player has; reset with the game)
bool s_fGaugeKeptWanted = false;          // (a kept world asked for: pumped each step, pfnStep)
void SyncGauges()
{
    if (split3::FBinding()) return;   // (the select is up)
    if (NOptRef(s_g.aship[1], 2463, -1) == -1) return;   // (the job's start (s1621) names the second player's first)
    std::array<LO*, kMaxShips> aused{};
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        if (!p) continue;
        const GAUGEDEF d = GaugeDefChar(hackmp3::IcharPlayer(i));
        if (d.oid < 0) continue;
        LO* pg = s_agauge[size_t(i)];
        if (!pg)
        {
            LO* ptmpl = nullptr;
            if (d.iwrec < 0) ptmpl = PloFindRelative(nullptr, d.oid, 5);
            else if (PckwsKeptWorld(d.iwrec) == PCKWS_Ready) ptmpl = PloKeptGauge(d);
            else if (PckwsKeptWorld(d.iwrec) != PCKWS_Failed)
            {
                RequestPcKeptWorld(d.iwrec);
                s_fGaugeKeptWanted = true;
                continue;
            }
            if (!ptmpl) continue;
            const bool fTaken = std::find(aused.begin(), aused.end(), ptmpl) != aused.end();
            pg = d.iwrec < 0 && !fTaken ? ptmpl : PloPcCloneIntoWorld(g_psw, ptmpl, true);
            if (!pg) continue;
            s_agauge[size_t(i)] = pg;
            const bool fRightSlot = (i & 1) != 0, fBottom = i >= 2;
            const float du = fRightSlot != d.fRight ? 640.0f - d.x0 - d.x1 : 0.0f;
            const float dv = fBottom ? 480.0f - d.y0 - d.y1 : 0.0f;
            static const bool s_fNoMove = std::getenv("SLY3_HACKGAUGENOMOVE") != nullptr;   // (debug: the boxes measured)
            SetGaugeSlot3(pg, (du != 0.0f || dv != 0.0f) && !s_fNoMove, du, dv);
            if (FPrint()) std::printf("hackgame3: player %d's gauge %d%s (moved %.0f %.0f)\n", i + 1, d.oid, pg == ptmpl ? "" : " (a copy)", du, dv);
        }
        aused[size_t(i)] = s_agauge[size_t(i)];
        if (PloSoGauge3(p) != pg) SetSoGauge3(p, pg);
    }
}
// (PC: the characters without a colour set of their own (Carmelita, Murray, the Panda King) drive the blue set
// recoloured, as the select's cards show them (hackmp3 FShipScene's colours): each such ship's parts (its glob sets
// with blue-set globs: the body, the guns, the glow, the arrows) get glob sets of their own, the blue set's vertex
// colours scaled to the character's colour; the arrows (93018) take the partner's character, as their 2464 does)
int IsetOfGlob3(const GLOB& g)   // 0 green 0x16BD5..7, 1 blue 0x16BDB..D, 2 pink 0x16BD8..A
{
    for (const SUBGLOB& sg : g.asubglob)
        if (sg.pshd)
        {
            const int32_t id = sg.pshd->id;
            if (id >= 0x16BD5 && id <= 0x16BD7) return 0;
            if (id >= 0x16BDB && id <= 0x16BDD) return 1;
            if (id >= 0x16BD8 && id <= 0x16BDA) return 2;
        }
    return -1;
}
bool FTintChar(int ichar, int* pr, int* pg, int* pb)
{
    switch (ichar)
    {
    case hackmp3::CHAR_Carmelita: *pr = 0xC8, *pg = 0x9C, *pb = 0x10; return true;
    case hackmp3::CHAR_Murray: *pr = 0x6C, *pg = 0x26, *pb = 0x9A; return true;
    case hackmp3::CHAR_PandaKing: *pr = 0xB8, *pg = 0x1C, *pb = 0x14; return true;
    default: return false;
    }
}
std::array<int, kMaxShips> s_aicharTinted{-1, -1, -1, -1};   // (the body tint each ship has; -1 none)
std::array<int, kMaxShips> s_aicharTintedArrows{-1, -1, -1, -1};
bool s_fSceneRebuild = false;
std::unordered_map<ALO*, std::shared_ptr<GLOBSET>> s_mpglobsetOrig;   // (a tinted part's own glob set as loaded)
void TintAlo(ALO* palo, int ichar)
{
    if (!palo || !palo->pglobset) return;
    auto it = s_mpglobsetOrig.find(palo);
    int r = 0, g = 0, b = 0;
    if (!FTintChar(ichar, &r, &g, &b))
    {
        if (it == s_mpglobsetOrig.end()) return;
        palo->pglobset = it->second;   // (a character with its own set again: the part as loaded)
        s_mpglobsetOrig.erase(it);
        s_fSceneRebuild = true;
        return;
    }
    const std::shared_ptr<GLOBSET> pgsOrig = it != s_mpglobsetOrig.end() ? it->second : palo->pglobset;
    bool fBlue = false;
    for (const GLOB& gl : pgsOrig->aglob) fBlue = fBlue || IsetOfGlob3(gl) == 1;
    if (!fBlue) return;
    s_mpglobsetOrig[palo] = pgsOrig;
    auto pgs = std::make_shared<GLOBSET>(*pgsOrig);
    for (GLOB& gl : pgs->aglob)
    {
        gl.pgl = nullptr;
        for (SUBGLOB& sg : gl.asubglob) sg.pgpu = nullptr;
        if (IsetOfGlob3(gl) != 1) continue;
        // (the blue set's shade is its blue channel: the mid shader's 0x83)
        for (VTXGLOB& v : gl.avtx)
            for (uint32_t* p : {&v.rgbaA, &v.rgbaB})
            {
                const float k = float((*p >> 16) & 0xFF) / float(0x83);
                auto c = [&](int n) { return uint32_t(std::clamp(int(std::lround(float(n) * k)), 0, 255)); };
                *p = (*p & 0xFF000000u) | (c(b) << 16) | (c(g) << 8) | c(r);
            }
    }
    pgs->palo = palo;
    palo->pglobset = pgs;
    s_fSceneRebuild = true;
}
void SyncTints()
{
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        if (!p) continue;
        const int ichar = hackmp3::IcharPlayer(i);
        const int iPartner = s_g.cship == 2 ? 1 - i : (i + 1) % s_g.cship;
        const int icharArrows = hackmp3::IcharPlayer(iPartner);
        if (s_aicharTinted[size_t(i)] == ichar && s_aicharTintedArrows[size_t(i)] == icharArrows) continue;
        s_aicharTinted[size_t(i)] = ichar;
        s_aicharTintedArrows[size_t(i)] = icharArrows;
        LO* parrows = nullptr;
        for (LO* pc : static_cast<ALO*>(p)->aploChild)
            if (pc->oid == 93018) parrows = pc;
        std::vector<ALO*> apalo{static_cast<ALO*>(p)};
        for (size_t k = 0; k < apalo.size(); ++k)
            for (LO* pc : apalo[k]->aploChild)
                if (FCidDerivedFrom(pc->Cid(), sly3::CID_ALO) && pc->Cid() != 0x61 && pc->Cid() != 0x81) apalo.push_back(static_cast<ALO*>(pc));
        for (ALO* pa : apalo)
            TintAlo(pa, pa == parrows || (parrows && FLoInParentChain(pa, parrows)) ? icharArrows : ichar);
        if (FPrint()) std::printf("hackgame3: player %d's ship tinted for character %d (arrows %d)\n", i + 1, ichar, icharArrows);
    }
}

void SyncColours()
{
    if (split3::FBinding()) return;   // (the select is up)
    // (the job's start (s1621) sets them first: until then the script's own would replace ours)
    if (NOptRef(s_g.aship[1], 2463, -1) == -1) return;
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        if (!p) continue;
        const int nBody = NCodeChar(hackmp3::IcharPlayer(i));
        // (the partner: two players the other one, more the next in seat order)
        const int iPartner = s_g.cship == 2 ? 1 - i : (i + 1) % s_g.cship;
        const int nArrow = NCodeChar(hackmp3::IcharPlayer(iPartner));
        if (NOptRef(p, 2463, -1) == nBody && NOptRef(p, 2464, -1) == nArrow) continue;
        SetOptRef(p, 2463, RefN(nBody));
        SetOptRef(p, 2464, RefN(nArrow));
        CallOpt(p, 2465);
        s_aicharTinted[size_t(i)] = -1;   // (tinted again after the script's colours)
        if (FPrint()) std::printf("hackgame3: player %d's colours %d / %d\n", i + 1, nBody, nArrow);
    }
}

// The power-up method 2479 (the ship's s5101 (type, given)) wrapped (three / four players): a shared power-up the ship
// takes itself (not given) is given to every other living ship too (the laser 3: each ship's own beam to its successor
// (+0x30C), a square / triangle; the mirror shots 4 and the arrows 5: the script itself gives its +0x30C partner (its
// successor), the rest here). Players 3 / 4 (copies of player 2's ship) run them in their own script (their s0 run
// again on their own parts: OnWorldStart): the laser by the original's rule (s5101 case 3 / s1: the enemies of class
// 75 in the world when it is taken, each frame every one whose collision entry the beam crosses killed (hit-test-objects
// 131, method 130), the kill credited by the job (1949) to players 1 and 2, the stats knowing two players)
std::unordered_map<const BASIC*, CRef> s_mp2479;
CRef Thunk2479(BASIC* pbasic, int c, CRef* a)
{
    auto it = s_mp2479.find(pbasic);
    if (it == s_mp2479.end()) return CRef(TAGK_Void);
    const CRef orig = it->second;
    const int n = c > 0 ? a[0].RefCoerceS32().m_n : -1;
    const bool fGiven = c > 1 && a[1].FTrue();
    LO* pself = dynamic_cast<LO*>(pbasic);
    int iself = -1;
    for (int i = 0; i < s_g.cship; ++i)
        if (s_g.aship[size_t(i)] == pself) iself = i;
    (void)iself;
    CallRef(orig, {RefN(n), RefB(fGiven)});
    if (fGiven || (n != 3 && n != 4 && n != 5) || s_g.cship < 3) return CRef(TAGK_Void);
    LO* ppartner = PloHackShipOther3(pself);
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        if (p == pself || !FAlive(p) || (n != 3 && p == ppartner)) continue;
        if (FPrint()) std::printf("hackgame3: power-up %d shared to player %d\n", n, i + 1);
        CallOpt(p, 2479, {RefN(n), RefB(true)});
    }
    return CRef(TAGK_Void);
}
void Wrap2479(LO* p)
{
    if (!p || s_mp2479.count(p)) return;
    const CRef orig = RefOpt(p, 2479);
    if (orig.m_tagk != TAGK_Proc)
    {
        if (FPrint()) std::printf("hackgame3: ship %p has no power-up method (tag %d)\n", (void*)p, int(orig.m_tagk));
        return;
    }
    s_mp2479[p] = orig;
    auto pm = std::make_shared<CMethod>();
    pm->m_pbasic = p;
    pm->m_crefReq = 2;
    pm->m_pfnthunk = Thunk2479;
    CRef r;
    r.SetMethod(pm);
    SetOptRef(p, 2479, r);
}

// the chain's links each frame (three / four ships): each living ship's +0x30C its successor
void SyncChain()
{
    if (s_g.cship < 3) return;
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        if (!FAlive(p)) continue;
        Wrap2479(p);
        LO* pnext = PloChainNext(i);
        if (pnext && PloHackShipOther3(p) != pnext)
        {
            SetHackShipOther3(p, pnext);
            if (FPrint())
                for (int j = 0; j < s_g.cship; ++j)
                    if (s_g.aship[size_t(j)] == pnext) std::printf("hackgame3: chain: player %d -> player %d\n", i + 1, j + 1);
        }
    }
}

// ---- PC, three / four players: the four exit docks of a round's end (user design: each player flies into a dock of
// its own). The original's round end (job 3334's SCPT 3338, block 93165 module 138): the round's two exit docks 92941
// (each section has two, at local (0, +-100)) opened (94449: their 2399), each ship flying into one (the dock's trigger,
// module 1's s5, calls the dock's 2400 with the toucher: the ship held (525 = 4), its hit points restored, the alarm its
// option 313 names fired on the script (998): 94447 player 1, 94448 player 2); the script waits for both alarms (its
// threads 94450 / 94451 count the docks' order in the tally, [94] / [95]), then opens the floor, releases the docks
// (2401) and drops both ships (525 = 5). Here (three / four players only; two players: the original's, untouched):
// each section's two docks get a copy each at local (0, +-300) (their own frames: s0 run again), a dock holds one ship
// (a second toucher is ignored), players 3 / 4's ships fire no alarm (313 = -1), and players 1 / 2's alarms wait until
// every living player 3 / 4 is held in a dock, then fire in their docking order; the originals' 2399 / 2401 also open /
// release their copies.
struct EXITDOCK { LO* porig; LO* pcopy; LO* ptrigger; bool fOpen; };   // (ptrigger: the copy's own trigger; fOpen: opened, not released)
std::vector<EXITDOCK> s_aexit;
std::unordered_map<const BASIC*, CRef> s_mpexit2399, s_mpexit2400, s_mpexit2401;
int s_cExitOpen = 0;                              // (original docks opened and not yet released)
constexpr int kOidExitDock = 92941;
constexpr float kDxExitCopy = 260.0f;   // (a copy beside its original, along the section's x)

LO* PloDockHeld(LO* pdock) { CRef r = RefOpt(pdock, 2398); return r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr; }
LO* PloCopyOfDock(LO* p)
{
    for (const EXITDOCK& e : s_aexit)
        if (e.porig == p) return e.pcopy;
    return nullptr;
}
CRef ThunkExit2399(BASIC* pbasic, int, CRef*)
{
    auto it = s_mpexit2399.find(pbasic);
    if (it == s_mpexit2399.end()) return CRef(TAGK_Void);
    CallRef(it->second, {});
    LO* p = dynamic_cast<LO*>(pbasic);
    if (LO* pcopy = PloCopyOfDock(p))
    {
        ++s_cExitOpen;
        CallOpt(pcopy, 2399);
        // (the open clip adds its dock's trigger by the original's oid: the copy's own trigger added here)
        for (EXITDOCK& e : s_aexit)
            if (e.pcopy == pcopy && e.ptrigger)
            {
                e.fOpen = true;
                AddLo(g_psw, e.ptrigger);
                if (FPrint()) std::printf("hackgame3: exit dock copy %p's trigger added (in the world %d, removed %d, the copy in %d)\n", (void*)pcopy, int(e.ptrigger->fInWorld),
                                          int(e.ptrigger->fRemoved), int(pcopy->fInWorld));
            }
        if (FPrint()) std::printf("hackgame3: exit dock %p open, its copy %p too\n", (void*)p, (void*)pcopy);
    }
    return CRef(TAGK_Void);
}
CRef ThunkExit2401(BASIC* pbasic, int, CRef*)
{
    auto it = s_mpexit2401.find(pbasic);
    if (it == s_mpexit2401.end()) return CRef(TAGK_Void);
    CallRef(it->second, {});
    if (LO* pcopy = PloCopyOfDock(dynamic_cast<LO*>(pbasic)))
    {
        s_cExitOpen = std::max(0, s_cExitOpen - 1);
        CallOpt(pcopy, 2401);
        for (EXITDOCK& e : s_aexit)
            if (e.pcopy == pcopy && e.ptrigger)
            {
                e.fOpen = false;
                RemoveLo(g_psw, e.ptrigger);
            }
    }
    return CRef(TAGK_Void);
}
void FoldShots(int iplayer);
CRef ThunkExit2400(BASIC* pbasic, int c, CRef* a)
{
    auto it = s_mpexit2400.find(pbasic);
    if (it == s_mpexit2400.end() || c < 1) return CRef(TAGK_Void);
    LO* pdock = dynamic_cast<LO*>(pbasic);
    LO* pship = a[0].m_tagk == TAGK_Basic ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
    // (one ship a dock)
    if (LO* pheld = PloDockHeld(pdock); pheld && pheld != pship) return CRef(TAGK_Void);
    int iplayer = -1;
    for (int i = 0; i < s_g.cship; ++i)
        if (s_g.aship[size_t(i)] == pship) iplayer = i;
    const int nAlarm = NOptRef(pship, 313, -1);
    if (iplayer >= 2 && nAlarm != -1)
    {
        // (players 3 / 4 fire no alarm: the job's script knows two ships; their 313 is player 2's again once its splice
        // re-arms it at a round's start)
        SetOptRef(pship, 313, RefN(-1));
        CallRef(it->second, {a[0], c > 1 ? a[1] : RefB(false)});
        SetOptRef(pship, 313, RefN(nAlarm));
        if (FPrint()) std::printf("hackgame3: player %d docked (no alarm)\n", iplayer + 1);
        FoldShots(iplayer);
    }
    else
    {
        CallRef(it->second, {a[0], c > 1 ? a[1] : RefB(false)});
        if (FPrint()) std::printf("hackgame3: player %d docked\n", iplayer + 1);
        FoldShots(iplayer);
    }
    return CRef(TAGK_Void);
}
// (a method bound again by its module's start (an object's s0 runs when it enters the world: the exit docks at their
// round) is wrapped again: called each step for the docks)
void WrapProc(LO* p, int optid, std::unordered_map<const BASIC*, CRef>& mp, CRef (*pfn)(BASIC*, int, CRef*), int creq)
{
    if (!p) return;
    const CRef orig = RefOpt(p, optid);
    if (orig.m_tagk != TAGK_Proc) return;
    if (FPrint() && mp.count(p)) std::printf("hackgame3: %d's method %d bound again: wrapped again\n", p->oid, optid);
    mp[p] = orig;
    auto pm = std::make_shared<CMethod>();
    pm->m_pbasic = p;
    pm->m_crefReq = creq;
    pm->m_pfnthunk = pfn;
    CRef r;
    r.SetMethod(pm);
    SetOptRef(p, optid, r);
}
void RunModuleStart(LO* p)
{
    if (!p || !p->pframe) return;
    if (CRef* pref = p->pframe->PrefFindBinding(SYMID(0), 0); pref && pref->m_tagk == TAGK_Proc)
    {
        CRef rself;
        rself.SetBasic(p);
        CallRef(*pref, {rself});
    }
}
void WrapExitDocks()
{
    // (a copy is in the world with its original: the sections' docks enter the world at their round)
    for (const EXITDOCK& e : s_aexit)
        if (e.porig->fInWorld != e.pcopy->fInWorld)
        {
            if (e.porig->fInWorld) AddLo(g_psw, e.pcopy);
            else RemoveLo(g_psw, e.pcopy);
            if (FPrint()) std::printf("hackgame3: exit dock copy %p %s the world with its original\n", (void*)e.pcopy, e.pcopy->fInWorld ? "in" : "out of");
        }
    // (the copy's trigger in the world while the copy is open and holds no ship: the open clip adds the original's by its
    // oid and something takes the copy's own out again after it is added once)
    for (const EXITDOCK& e : s_aexit)
        if (e.ptrigger && e.pcopy->fInWorld)
        {
            const bool fWant = e.fOpen && !PloDockHeld(e.pcopy);
            if (fWant && !e.ptrigger->fInWorld) AddLo(g_psw, e.ptrigger);
            else if (!fWant && e.ptrigger->fInWorld) RemoveLo(g_psw, e.ptrigger);
        }
    for (const EXITDOCK& e : s_aexit)
        for (LO* p : {e.porig, e.pcopy})
        {
            WrapProc(p, 2400, s_mpexit2400, ThunkExit2400, 2);
            if (p == e.porig)
            {
                WrapProc(p, 2399, s_mpexit2399, ThunkExit2399, 0);
                WrapProc(p, 2401, s_mpexit2401, ThunkExit2401, 0);
            }
        }
}
// The round's end alarms (94447 / 94448 on SCPT 3338: the exit docks' triggers' second handlers, block 94622, set them
// when a ship first touches the dock; the script waits for both, then drops the ships): with three / four players held
// back until every living player is held in a dock (state 4), then set in the order they came
std::vector<int> s_anAlarmHeld;
bool s_fAlarmRelease = false;
bool FEveryoneDocked()
{
    for (int i = 0; i < s_g.cship; ++i)
        if (LO* p = s_g.aship[size_t(i)]; FAlive(p) && HackShip3State(p) != 4) return false;
    return true;
}
bool FHoldAlarm(SCPT3* ps, int n)
{
    if (s_fAlarmRelease || !ps || ps->oid != 3338 || (n != 94447 && n != 94448) || s_aexit.empty() || !FCurrent(g_psw)) return false;
    if (FEveryoneDocked()) return false;
    if (std::find(s_anAlarmHeld.begin(), s_anAlarmHeld.end(), n) == s_anAlarmHeld.end()) s_anAlarmHeld.push_back(n);
    if (FPrint()) std::printf("hackgame3: alarm %d held until every player is docked\n", n);
    return true;
}
// (OnWorldStart, three / four players: each original exit dock's copy beside it; players 3 / 4 fire no alarm)
void MakeExitDocks(SW* psw)
{
    s_aexit.clear();
    s_mpexit2399.clear();
    s_mpexit2400.clear();
    s_mpexit2401.clear();
    s_anAlarmHeld.clear();
    s_cExitOpen = 0;
    // (three / four players only, by default since s3mp-hack (2026-11-18: every round's end docks by play with 3 and 4
    // players); SLY3_HACKGAME4DOCKS=0: the old way (players 3 / 4 carried with players 1 / 2), for an A/B)
    if (s_g.cship < 3) return;
    if (const char* sz = std::getenv("SLY3_HACKGAME4DOCKS"); sz && !std::strcmp(sz, "0")) return;
    std::vector<LO*> aorig;
    for (const auto& up : psw->aploAll)
        if (up->oid == kOidExitDock && up->Cid() == S3CID(0x47) && up->paloParent) aorig.push_back(up.get());
    for (LO* porig : aorig)
    {
        ALO* pparent = porig->paloParent;
        LO* pcopy = PloPcCloneIntoWorld(psw, porig, porig->fInWorld, pparent);
        if (!pcopy) continue;
        // (local (0, +-100) -> (260, +-100): beside the original, its mouth facing out the same way (the docks are C shapes
        // open away from the section's middle, so a dock farther out would close the way into the original))
        const glm::vec3 vo = PosLoWorld(porig);
        SetAloPos(static_cast<ALO*>(pcopy), vo + glm::vec3(kDxExitCopy, 0.0f, 0.0f));
        // (the dock's trigger 92891 is out of the world until the open clip adds it (its option 27): the copy skips removed
        // children (brx.cpp PloCloneLo), so the copy had no trigger and its clip added the original's: cloned here, out of
        // the world as the original's)
        LO* ptrigger = nullptr;
        for (LO* pc : std::vector<LO*>(porig->aploOwned))
            if (pc->fRemoved && pc->Cid() == sly3::CID_TRIGGER)
                if (LO* pcNew = PloCloneLo(pc, psw, static_cast<ALO*>(pcopy)))
                {
                    ptrigger = pcNew;
                    // (where the original's is from its dock: 87 beyond it, away from the section's middle)
                    SetAloPos(static_cast<ALO*>(pcNew), PosLoWorld(pcopy) + (PosLoWorld(pc) - PosLoWorld(porig)));
                    RemoveLo(psw, pcNew);
                    if (FPrint()) std::printf("hackgame3: exit dock copy's trigger at %.0f %.0f %.0f (the copy at %.0f %.0f)\n", PosLoWorld(pcNew).x, PosLoWorld(pcNew).y,
                                              PosLoWorld(pcNew).z, PosLoWorld(pcopy).x, PosLoWorld(pcopy).y);
                }
        RunModuleStart(pcopy);
        for (LO* pchild : pcopy->aploOwned) RunModuleStart(pchild);
        // (no state machine for the copy: the dock's SM 92944 clips (open 93156, held, closed) find their trigger 92891 by oid
        // and from a copy reached the original's, taking it out of the world at a copy's release (a later round's original
        // could not be docked); the copy's own trigger is put in / out of the world here instead: WrapExitDocks)
        s_aexit.push_back({porig, pcopy, ptrigger, false});
        if (FPrint()) std::printf("hackgame3: exit dock %d (section %d) copied at %.0f %.0f %.0f\n", porig->oid, pparent->oid, vo.x + kDxExitCopy, vo.y, vo.z);
    }
    WrapExitDocks();
    g_pfnScptAlarmHold3 = FHoldAlarm;
    for (int i = 2; i < s_g.cship; ++i) SetOptRef(s_g.aship[size_t(i)], 313, RefN(-1));
}
// ---- PC, three / four players: the tally counts every player. The job's stats (module 138's s5023 (ship, stat, n):
// group 0 for player 1's ship 93052, else group 1; the stats 0 kills, 1 deaths, 2 power-ups, 3 shots, 4 docks, read by
// the round's bonuses (94455: no deaths, power-ups, accuracy) and the tally 3335's 1807 / 1808) know two players: players
// 3 / 4 are counted with players 1 / 2 (3 with 1, 4 with 2: the original's two columns), their kills credited as the job's
// 1949 credits players 1 / 2's (a piece's ship (552), the laser both), their shots (2475, read at the round's end by
// 94454) added to their partner's when they dock. Two players: nothing here (the original's).
LO* s_pjob = nullptr;
CRef s_ref5023;
std::unordered_map<const BASIC*, CRef> s_mpjob1949;
LO* PloStatShip(LO* p)
{
    for (int i = 2; i < s_g.cship; ++i)
        if (p && p == s_g.aship[size_t(i)]) return s_g.aship[size_t(i - 2)];
    return p;
}
CRef Thunk5023(BASIC*, int c, CRef* a)
{
    if (c < 3) return CRef(TAGK_Void);
    CRef r0 = a[0];
    if (LO* p = r0.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r0.m_pbasic) : nullptr; p && PloStatShip(p) != p)
    {
        if (FPrint()) std::printf("hackgame3: stat %d +%d of a player 3 / 4 counted with its partner\n", a[1].RefCoerceS32().m_n, a[2].RefCoerceS32().m_n);
        r0.SetBasic(PloStatShip(p));
    }
    return CallRef(s_ref5023, {r0, a[1], a[2]});
}
CRef Thunk1949(BASIC* pbasic, int c, CRef* a)
{
    auto it = s_mpjob1949.find(pbasic);
    if (it == s_mpjob1949.end() || c < 1) return CRef(TAGK_Void);
    CallRef(it->second, {a[0]});
    LO* pdead = a[0].m_tagk == TAGK_Basic ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
    CRef rk = RefOpt(pdead, 1447);
    LO* pkiller = rk.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(rk.m_pbasic) : nullptr;
    if (pkiller && FCidDerivedFrom(pkiller->Cid(), 0x4C))   // (a piece: its ship, 552)
    {
        CRef rs = RefOpt(pkiller, 552);
        pkiller = rs.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(rs.m_pbasic) : nullptr;
    }
    if (pkiller && PloStatShip(pkiller) != pkiller)
    {
        CRef rship;
        rship.SetBasic(pkiller);
        CRef aref[3] = {rship, RefN(0), RefN(1)};
        Thunk5023(nullptr, 3, aref);
    }
    return CRef(TAGK_Void);
}
// The respawn (block 93165 module 135): a ship's death (event 12, s20) schedules s4981 2 s later through s4961, which keeps
// the timer's id in s4952 for player 1's ship (s4970), else in s4953; s4981 adds the ship back (41), restores it (948),
// hands it to the job (2466) and cancels / clears that slot (the game over test s4983 reads both). With three / four
// players a copy took player 2's slot: two deaths close together overwrote each other's id and one respawn cancelled the
// other's timer (a dead player never came back and the round's end waited for it forever). Players 3 / 4 keep the
// original's code but not its slots: s4952 / s4953 saved around their calls, -1 while s4981 runs (no cancel of player 1 /
// 2's timer). Two players: nothing wrapped.
LO* s_pjobRespawn = nullptr;
CRef s_ref4961, s_ref4981;
CRef ThunkRespawn(const CRef& ref, int c, CRef* a, bool fClear)
{
    if (c < 2) return CRef(TAGK_Void);
    LO* pship = a[1].m_tagk == TAGK_Basic ? dynamic_cast<LO*>(a[1].m_pbasic) : nullptr;
    const bool fExtra = pship && PloStatShip(pship) != pship;
    CRef* p52 = fExtra && s_pjobRespawn && s_pjobRespawn->pframe ? s_pjobRespawn->pframe->PrefFindBinding(SYMID(4952), 0) : nullptr;
    CRef* p53 = fExtra && s_pjobRespawn && s_pjobRespawn->pframe ? s_pjobRespawn->pframe->PrefFindBinding(SYMID(4953), 0) : nullptr;
    const CRef r52 = p52 ? *p52 : CRef(), r53 = p53 ? *p53 : CRef();
    if (fClear && p52 && p53)
    {
        *p52 = RefN(-1);
        *p53 = RefN(-1);
    }
    CRef r = CallRef(ref, {a[0], a[1]});
    if (p52 && p53)
    {
        *p52 = r52;
        *p53 = r53;
        if (FPrint()) std::printf("hackgame3: %s of a player 3 / 4 (player 1 / 2's respawn slots kept)\n", fClear ? "respawn" : "respawn scheduled");
    }
    return r;
}
CRef Thunk4961(BASIC*, int c, CRef* a) { return ThunkRespawn(s_ref4961, c, a, false); }
CRef Thunk4981(BASIC*, int c, CRef* a) { return ThunkRespawn(s_ref4981, c, a, true); }
// The respawn's start dock (2466 on SCPT 3338, block 93165 module 138): player 1's ship to the round's start dock s5000[0],
// any other to s5000[1]; the dock (block 92886) holds one ship (2398, its player's pad lock s4631) until its clip calls
// 2401 (the release: state 1). With three / four players two ships respawning at that dock within its hold took each
// other's place: the first was never released (held in state 4 for the rest of the game; the round's end waited for it).
// A dock still holding another ship releases it (its own 2401) before it takes the next. Two players: not wrapped.
std::unordered_map<const BASIC*, CRef> s_mpjob2466;
CRef Thunk2466(BASIC* pbasic, int c, CRef* a)
{
    auto it = s_mpjob2466.find(pbasic);
    if (it == s_mpjob2466.end() || c < 1) return CRef(TAGK_Void);
    LO* pship = a[0].m_tagk == TAGK_Basic ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
    LO* pjob = dynamic_cast<LO*>(pbasic);
    if (pship && pjob && pjob->pframe && s_g.cship >= 3)
        if (const CRef* p00 = pjob->pframe->PrefFindBinding(SYMID(5000), 0); p00 && p00->m_tagk == TAGK_Pair)
        {
            const CPair* pp = p00->m_ppair.get();
            if (pp && pship != s_g.aship[0]) pp = pp->m_ppairNext.get();
            LO* pdock = pp ? dynamic_cast<LO*>(pp->m_ref.m_pbasic) : nullptr;
            const CRef rheld = pdock ? RefOpt(pdock, 2398) : CRef();
            LO* pheld = rheld.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(rheld.m_pbasic) : nullptr;
            if (pheld && pheld != pship && HackShip3State(pheld) == 4)
            {
                if (FPrint()) std::printf("hackgame3: start dock %d still holds a ship: released before the next respawn\n", pdock->oid);
                CallOpt(pdock, 2401, {});
            }
        }
    return CallRef(it->second, {a[0]});
}
void WrapJobRespawn(SW* psw)
{
    s_pjobRespawn = nullptr;
    s_ref4961 = s_ref4981 = CRef();
    if (s_g.cship < 3) return;
    for (const auto& up : psw->aploAll)
    {
        LO* p = up.get();
        if (!p->pframe) continue;
        CRef* p61 = p->pframe->PrefFindBinding(SYMID(4961), 0);
        CRef* p81 = p->pframe->PrefFindBinding(SYMID(4981), 0);
        if (!p61 || !p81 || p61->m_tagk != TAGK_Proc || p81->m_tagk != TAGK_Proc || !p->pframe->PrefFindBinding(SYMID(4953), 0)) continue;
        s_pjobRespawn = p;
        s_ref4961 = *p61;
        s_ref4981 = *p81;
        auto wrap = [&](CRef* pref, PFNTHUNK pfn) {
            auto pm = std::make_shared<CMethod>();
            pm->m_pbasic = p;
            pm->m_crefReq = 2;
            pm->m_pfnthunk = pfn;
            pref->SetMethod(pm);
        };
        wrap(p61, Thunk4961);
        wrap(p81, Thunk4981);
        s_mpjob2466.clear();
        for (const auto& uq : psw->aploAll)
            if (RefOpt(uq.get(), 2466).m_tagk == TAGK_Proc) WrapProc(uq.get(), 2466, s_mpjob2466, Thunk2466, 1);
        if (FPrint()) std::printf("hackgame3: the job's respawn (%d) keeps players 3 / 4 out of players 1 / 2's slots\n", p->oid);
        break;
    }
}
void WrapJobStats(SW* psw)
{
    s_pjob = nullptr;
    s_ref5023 = CRef();
    s_mpjob1949.clear();
    if (s_g.cship < 3) return;
    for (const auto& up : psw->aploAll)
        if (LO* p = up.get(); p->pframe && RefOpt(p, 1949).m_tagk == TAGK_Proc)
            if (CRef* pref = p->pframe->PrefFindBinding(SYMID(5023), 0); pref && pref->m_tagk == TAGK_Proc)
            {
                s_pjob = p;
                s_ref5023 = *pref;
                auto pm = std::make_shared<CMethod>();
                pm->m_pbasic = p;
                pm->m_crefReq = 3;
                pm->m_pfnthunk = Thunk5023;
                pref->SetMethod(pm);
                WrapProc(p, 1949, s_mpjob1949, Thunk1949, 1);
                if (FPrint()) std::printf("hackgame3: the job's stats (%d) count players 3 / 4 with players 1 / 2\n", p->oid);
                break;
            }
}
// (a player 3 / 4 docked: its shots this round to its partner's, read by the round's end (94454))
void FoldShots(int iplayer)
{
    if (iplayer < 2 || iplayer >= s_g.cship) return;
    LO* p = s_g.aship[size_t(iplayer)];
    LO* pl = s_g.aship[size_t(iplayer - 2)];
    const int n = NOptRef(p, 2475, 0);
    if (n <= 0) return;
    SetOptRef(pl, 2475, RefN(NOptRef(pl, 2475, 0) + n));
    SetOptRef(p, 2475, RefN(0));
    if (FPrint()) std::printf("hackgame3: player %d's %d shots counted with player %d\n", iplayer + 1, n, iplayer - 1);
}

// (each step: players 1 / 2's held alarms fired once every living player 3 / 4 is held in a dock)
void SyncExitAlarms()
{
    WrapExitDocks();
    if (s_anAlarmHeld.empty() || !FEveryoneDocked()) return;
    if (SCPT3* ps = PscptOfLo3(PloFindRelative(nullptr, 3338, 5)))
    {
        s_fAlarmRelease = true;
        for (int n : s_anAlarmHeld)
        {
            if (FPrint()) std::printf("hackgame3: every player docked: alarm %d set\n", n);
            SetScptAlarm3(ps, n, true);
        }
        s_fAlarmRelease = false;
    }
    s_anAlarmHeld.clear();
}

// (tests: SLY3_HACKGAMEPILOT="P:X Y@F;...": from frame F player P's (1..4, a scripted seat) left stick steers its ship
// toward the world point (X, Y) through the camera's facing, as a route's "move" does for player 1: docking by play)
struct PILOT { int iplayer, frame; float x, y; };
std::vector<PILOT> s_apilot;
// ("P:dock") player iseat's own open exit dock this round: players 1 / 3 the -y side, 2 / 4 the +y side (the ships'
// sides at a round's start), players 1 / 2 the inner dock (the original's), 3 / 4 the outer one (the copy); the goal its
// mouth (-x) first, then the dock
// (a dock's trigger 92891: the original's child, the copy's own)
LO* PloDockTrigger(LO* pdock)
{
    for (const EXITDOCK& e : s_aexit)
        if (e.pcopy == pdock) return e.ptrigger;
    for (LO* pc : pdock->aploOwned)
        if (pc->Cid() == sly3::CID_TRIGGER) return pc;
    return nullptr;
}
bool FDockGoal(int iseat, const glm::vec3& pos, glm::vec2* pgoal)
{
    // the open docks of the player's kind: players 1 / 2 the originals, 3 / 4 the copies beside them (two players: the
    // originals); a dock another ship holds is not free
    const bool fCopy = iseat >= 2;
    std::vector<LO*> adock;
    for (const auto& up : g_psw->aploAll)
        if (up->oid == kOidExitDock && !fCopy) adock.push_back(up.get());
    if (fCopy)
        for (const EXITDOCK& e : s_aexit) adock.push_back(e.pcopy);
    LO* pship = s_g.aship[size_t(iseat)];
    std::vector<LO*> afree;
    for (LO* p : adock)
    {
        if (p->Cid() != S3CID(0x47) || !p->paloParent || !p->fInWorld) continue;
        if (!fCopy && PloCopyOfDock(p) == nullptr && !s_aexit.empty()) continue;   // (a copy listed by its oid)
        LO* pt = PloDockTrigger(p);
        if (!pt || !pt->fInWorld) continue;
        if (LO* ph = PloDockHeld(p); ph && ph != pship) continue;
        afree.push_back(p);
    }
    if (afree.empty()) return false;
    // (the partner of the same kind (1 / 2, 3 / 4) still flying: the pair's docks by the shorter total way, so the two do
    // not head for the same one)
    auto dist = [](LO* pd, LO* ps) { return glm::length(glm::vec2(PosLoWorld(pd)) - glm::vec2(PosLoWorld(ps))); };
    LO* pdock = afree[0];
    for (LO* p : afree)
        if (dist(p, pship) < dist(pdock, pship)) pdock = p;
    const int ipartner = iseat ^ 1;
    LO* ppartner = ipartner < s_g.cship ? s_g.aship[size_t(ipartner)] : nullptr;
    if (afree.size() >= 2 && ppartner && FAlive(ppartner) && HackShip3State(ppartner) != 4)
    {
        float best = 1e30f;
        for (LO* p : afree)
            for (LO* q : afree)
                if (p != q && dist(p, pship) + dist(q, ppartner) < best)
                {
                    best = dist(p, pship) + dist(q, ppartner);
                    pdock = p;
                }
    }
    // (the dock's trigger 92891 lies beyond the dock's body, 87 farther from the section's middle: the dock is entered
    // from outside, toward the middle, along the trigger's line from 300 short of it)
    const glm::vec3 vt = PosLoWorld(PloDockTrigger(pdock));
    const glm::vec2 a(0.0f, vt.y > PosLoWorld(pdock).y ? -1.0f : 1.0f);   // (from outside, toward the section's middle)
    const glm::vec2 t(vt.x, vt.y), p(pos.x, pos.y);
    const float along = glm::dot(p - t, a);            // (< 0: short of the trigger)
    const float off = glm::length((p - t) - along * a); // (from the line)
    // (a ship already sitting in the trigger when the dock opened (it landed there) never enters it: out and in again)
    static bool s_afBack[kMaxShips] = {};
    bool& fBack = s_afBack[size_t(iseat)];
    if (glm::length(p - t) < 40.0f) fBack = true;
    if (fBack && along < -250.0f) fBack = false;
    if (fBack || off > 80.0f || (off > 25.0f && along > -200.0f)) *pgoal = t - 300.0f * a;
    else *pgoal = t + std::min(0.0f, along + 100.0f) * a;   // (a point ahead on the line: held on it)
    return true;
}
void ScriptedSeatPilot(int iseat, int frame, float a[4], uint16_t*)
{
    static const bool s_fParsed = [] {
        if (const char* sz = std::getenv("SLY3_HACKGAMEPILOT"))
            for (const char* q = sz; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            {
                PILOT p{};
                if (std::sscanf(q, "%d:%f %f@%d", &p.iplayer, &p.x, &p.y, &p.frame) == 4) s_apilot.push_back(p);
                else if (std::sscanf(q, "%d:%f %f@open", &p.iplayer, &p.x, &p.y) == 3) { p.frame = -1; s_apilot.push_back(p); }   // (once the exit docks open)
                else if (std::strncmp(std::strchr(q, ':') ? std::strchr(q, ':') : q, ":dock", 5) == 0 && std::sscanf(q, "%d", &p.iplayer) == 1)
                {
                    p.frame = -2;   // (every round: into its own open exit dock, FDockGoal)
                    s_apilot.push_back(p);
                }
            }
        return true;
    }();
    (void)s_fParsed;
    if (!FCurrent(g_psw) || iseat < 0 || iseat >= s_g.cship) return;   // (player 1 too when its seat is scripted)
    const PILOT* pcur = nullptr;
    bool fDock = false;
    for (const PILOT& p : s_apilot)
    {
        if (p.iplayer != iseat + 1) continue;
        if (p.frame == -2) fDock = true;
        else if ((p.frame == -1 ? s_cExitOpen > 0 : p.frame <= frame) && (!pcur || pcur->frame < p.frame)) pcur = &p;
    }
    LO* pship = s_g.aship[size_t(iseat)];
    if (!pship || !pship->fInWorld || HackShip3State(pship) == 4) return;
    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(pship));
    glm::vec2 goal(0.0f);
    if (fDock && FDockGoal(iseat, pos, &goal)) {}
    else if (pcur) goal = glm::vec2(pcur->x, pcur->y);
    else return;
    // (stuck against a dock's body or another ship for 2 s: half a second toward the section's middle, then on)
    struct STUCK { glm::vec2 pos{1e9f}; int frame = 0, frameEscape = -1; };
    static STUCK s_astuck[kMaxShips];
    STUCK& st = s_astuck[size_t(iseat)];
    if (frame - st.frame >= 120)
    {
        if (glm::length(glm::vec2(pos) - st.pos) < 20.0f && glm::length(goal - glm::vec2(pos)) > 60.0f) st.frameEscape = frame;
        st.pos = glm::vec2(pos);
        st.frame = frame;
    }
    if (st.frameEscape >= 0 && frame - st.frameEscape < 30) goal = glm::vec2(pos.x - 200.0f, pos.y);
    // (to the other side of the section: round the docks' row (x 0 .. 260 at y +-100) on the -x side)
    if (fDock && (goal.y < 0.0f) != (pos.y < 0.0f) && std::fabs(goal.y - pos.y) > 150.0f && pos.x > -250.0f) goal = glm::vec2(-350.0f, pos.y);
    glm::vec2 d(goal.x - pos.x, goal.y - pos.y);
    const float l = glm::length(d);
    if (l < 30.0f) return;
    glm::vec2 f(g_cm.mat[0].x, g_cm.mat[0].y);
    if (glm::length(f) < 1e-3f) return;
    f = glm::normalize(f);
    d /= l;
    const float k = std::clamp(l / 150.0f, 0.6f, 1.0f);   // (a firm push near the goal: a dock's mouth slides a weak one off)
    a[0] = k * (d.x * f.y - d.y * f.x);   // (right of the facing)
    a[1] = k * (d.x * f.x + d.y * f.y);   // (along it: stick up)
    if (static const bool s_f = [] { const char* sz = std::getenv("SLY3_HACKGAMEPRINT"); return sz && std::atoi(sz) >= 3; }(); s_f && frame % 60 == 0)
        std::printf("hackgame3: pilot %d frame %d at %.0f %.0f -> %.0f %.0f stick %.2f %.2f state %d\n", iseat + 1, frame, pos.x, pos.y,
                    goal.x, goal.y, a[0], a[1], HackShip3State(pship));
}

// players 3 / 4 go through the rounds with players 1 / 2 (the job's script knows two ships): held / falling with them,
// put beside them at the docks
void SyncExtraShips()
{
    for (int i = 2; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        LO* pl = s_g.aship[size_t(i - 2)];
        if (!p || !pl || !p->fInWorld || !pl->fInWorld) continue;
        const int sl = HackShip3State(pl), s = HackShip3State(p);
        if (s_cExitOpen > 0) continue;   // (a round's end: each player flies into its own exit dock, held there on its own)
        if ((sl == 4 || sl == 5) && s != sl && s != 6)
        {
            const glm::vec3 v = PosLoWorld(pl);
            SetAloPos(static_cast<ALO*>(p), v + glm::vec3(0.0f, (v.y < 0.0f ? 1.0f : -1.0f) * 220.0f, 0.0f));
            SetHackShip3State(p, sl);
        }
        else if (sl == 1 && (s == 4 || s == 5)) SetHackShip3State(p, 1);
    }
}

// (log, SLY3_HACKGAMEPRINT) a round's end: every ship held in a dock (state 4), then dropped to the next level (5)
int s_iround = 0;
bool s_fAllHeld = false, s_fOpenSeen = false;
void LogRounds()
{
    if (!FPrint()) return;
    bool fAll = true, fAny = false, fFall = false;
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        if (!p || !p->fInWorld) continue;
        const int st = HackShip3State(p);
        fAny = true;
        fAll = fAll && st == 4;
        fFall = fFall || st == 5;
    }
    // (a round's end: its exit docks opened (their triggers in the world); at a round's start the ships are held too)
    for (const auto& up : g_psw->aploAll)
        if (up->oid == kOidExitDock && up->fInWorld)
            for (LO* pc : up->aploOwned) s_fOpenSeen = s_fOpenSeen || (pc->Cid() == sly3::CID_TRIGGER && pc->fInWorld);
    if (fAny && fAll && s_fOpenSeen && !s_fAllHeld)
    {
        s_fAllHeld = true;
        std::printf("hackgame3: round %d over: every player docked (%d ships, t %.1f)\n", s_iround + 1, s_g.cship, g_clock.t);
    }
    if (s_fAllHeld && fFall)
    {
        s_fAllHeld = false;
        s_fOpenSeen = false;
        ++s_iround;
        std::printf("hackgame3: round %d starts (t %.1f)\n", s_iround + 1, g_clock.t);
    }
}

// the job's object (its splice frame holds the rounds' state: s1946 the round, s5011 / s4626 the entry, s5015 the boss,
// s4997 the round's board): the object with the kill method 1949
LO* PloJobScpt()
{
    static LO* s_pscpt = nullptr;
    static uint32_t s_serialScpt = 0;
    if (s_serialScpt != g_psw->serial)
    {
        s_pscpt = nullptr;
        s_serialScpt = g_psw->serial;
        // (three / four players: 1949 wrapped by WrapJobStats, a method)
        for (const auto& up : g_psw->aploAll)
            if (const TAGK k = RefOpt(up.get(), 1949).m_tagk; up->pframe && (k == TAGK_Proc || k == TAGK_Method) && up->pframe->PrefFindBinding(SYMID(5015), 0))
                s_pscpt = up.get();
    }
    return s_pscpt && s_pscpt->pframe ? s_pscpt : nullptr;
}

// (debug) the boss of round 6 (92856, one under each of the boss level's boards) is in the world but the job has not
// taken it as the round's boss yet: its entry (5 92856) is read after the first entry (4 -1), about 2 s after the boss
// enters. A boss killed before that is not the job's s5015 when its kill (1949) comes, so the round's end (94444) never
// comes (the original's script the same: in play no bomb can come that early, the power-ups being dropped by the round's
// enemies). The test bombs wait for it.
bool FBossNotTaken()
{
    LO* pjob = PloJobScpt();
    if (!pjob) return false;
    const CRef* pref = pjob->pframe->PrefFindBinding(SYMID(5015), 0);
    const LO* pboss = pref ? dynamic_cast<const LO*>(pref->m_pbasic) : nullptr;
    for (LO* q : g_psw->mpcidaplo[0x4B])   // (Sly 3's HACKENEMY)
        if (q->oid == 92856 && q->fInWorld && q != pboss) return true;
    return false;
}

// (debug / screenshots: SLY3_HACKGAMEPOWER="P:N@F;..." player P (1-based) takes power-up N at frame F of the game)
void DebugPowerUps()
{
    static int s_frame = 0;
    ++s_frame;
    // (debug: SLY3_HACKGAMEKILL="P@F;..." player P's ship dies at frame F of the game: its hit points to 0, its dying state 6
    // (the death's task event 0xC and the job's respawn follow as for a death by play))
    // (only where play can kill it: driven, state 1 (hits are refused while hit / docked / held / falling / dying); a
    // kill that comes in another state waits for it: e.g. one in a round's drop (state 5) respawned the ship at the old
    // level's start dock, out of reach)
    static std::vector<bool> s_afKillDone;
    if (static const char* s_szKill = std::getenv("SLY3_HACKGAMEKILL"); s_szKill)
        for (const char* q = s_szKill; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            int ip = 0, f = 0;
            const size_t ik = size_t(std::count(s_szKill, q, ';'));
            if (s_afKillDone.size() <= ik) s_afKillDone.resize(ik + 1, false);
            if (std::sscanf(q, "%d@%d", &ip, &f) != 2 || f > s_frame || s_afKillDone[ik]) continue;
            if (LO* p = PloShip(ip - 1); p && FAlive(p) && HackShip3State(p) == 1)
            {
                s_afKillDone[ik] = true;
                std::printf("hackgame3: player %d dies (debug)\n", ip);
                FSetHackHp3(p, false, 0);
                SetHackShip3State(p, 6);
            }
        }
    static const char* s_sz = std::getenv("SLY3_HACKGAMEPOWER");
    if (!s_sz) return;
    for (const char* q = s_sz; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
    {
        int ip = 0, n = 0, f = 0;
        if (std::sscanf(q, "%d:%d@%d", &ip, &n, &f) != 3 || f != s_frame) continue;
        if (n == 2 && FBossNotTaken())
        {
            std::printf("hackgame3: player %d's power-up %d held: the boss is not the round's yet (debug)\n", ip, n);
            continue;
        }
        if (LO* p = PloShip(ip - 1))
        {
            std::printf("hackgame3: player %d takes power-up %d (debug)\n", ip, n);
            CallOpt(p, 2479, {RefN(n), RefB(false)});
        }
    }
}

// (debug: SLY3_HACKGAMEPRINT=2) the game's state every 2 s: the core's counter, each ship's hit points, gun level
// (2474), shots (2475), and the power-up parts in the world (the laser 6130, the arrows 93018, the shield's glow 74174)
void PrintState()
{
    static const bool s_f = [] { const char* sz = std::getenv("SLY3_HACKGAMEPRINT"); return sz && std::atoi(sz) >= 2; }();
    if (!s_f || int(g_clock.t * 60.0f) % 120 != 0) return;
    LO* pcount = PloFindRelative(nullptr, 3336, 5);
    int cEnemy = 0;
    for (LO* q : g_psw->mpcidaplo[0x4B]) cEnemy += q->fInWorld ? 1 : 0;
    std::printf("hackgame3 t %.1f: core %d/%d enemies %d", g_clock.t, NOptRef(pcount, 1177, -1), NOptRef(pcount, 1178, -1), cEnemy);
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        int hp = -1;
        FHackHp3(p, false, &hp);
        auto inw = [&](int oid) { LO* q = PloFindRelative(p, oid, 4); return q ? int(q->fInWorld) : -1; };
        std::printf(" | ship %d hp %d st %d gun %d shots %d laser %d arrows %d glow %d", i + 1, hp, HackShip3State(p), NOptRef(p, 2474, -1),
                    NOptRef(p, 2475, -1), inw(6130), inw(93018), inw(74174));
    }
    // (the job's round state: the entry's kind s5011, the entry s4626, the round's boss s5015 and the board s4997)
    if (LO* pjob = PloJobScpt())
    {
        auto str = [&](int sym) {
            std::string r;
            if (CRef* pref = pjob->pframe->PrefFindBinding(SYMID(sym), 0)) PrintRef(*pref, &r);
            return r.substr(0, 120);
        };
        auto obj = [&](int sym) {
            const CRef* pref = pjob->pframe->PrefFindBinding(SYMID(sym), 0);
            const LO* p = pref ? dynamic_cast<const LO*>(pref->m_pbasic) : nullptr;
            if (!p) return std::string("nil");
            int hp = -1;
            FHackHp3(const_cast<LO*>(p), false, &hp);
            char sz[96];
            std::snprintf(sz, sizeof sz, "%d<%d (in world %d hp %d)", p->oid, p->paloParent ? p->paloParent->oid : -1, int(p->fInWorld), hp);
            return std::string(sz);
        };
        std::printf(" | job round %s kind %s entry %s boss %s board %s", str(1946).c_str(), str(5011).c_str(), str(4626).c_str(), obj(5015).c_str(),
                    obj(4997).c_str());
    }
    std::printf("\n");
}

// (a kept world a gauge needs, read while the select is up: instantiated between two steps, as the debug menu's step
// does for its own)
struct INITHACKGAMEHOOKS
{
    INITHACKGAMEHOOKS()
    {
        PCWORLDHOOKS h;
        h.pfnStep = [](SW* psw, bool fDebugKeys) {
            if (s_fGaugeKeptWanted && !fDebugKeys && FCurrent(psw) && FPcKeptWorldsLoading()) PumpPcKeptWorlds(psw);
        };
        RegisterPcWorldHooks(h);
    }
} s_inithackgamehooks;

}  // namespace

// ---- class 0x81 (NEW_ALO_81, vtable 0x3521E0): a ribbon between two objects (the hacking game's laser 6130 under each
// ship: its s5101 case 3 sets +0x134 (optid 1803) to the ship and +0x138 (1804) to the other ship) ----
// Its init (vt slot 7, 0x13CEE0) sets the width +0x130 (optid 1805) to 1; its draw (slot 21, 0x13CF10 -> 0x1C8328) puts
// the object's own model between the two objects' positions per camera: x facing the camera, y the width across
// (n / |n| x width), z the line / 100 (the model is a 100-unit strip), alpha +0x104.
namespace
{
struct RIBBON3 { LO* pfrom = nullptr; LO* pto = nullptr; float width = 1.0f; };
std::unordered_map<const LO*, RIBBON3> s_mpribbon;
const SW* s_pswRibbon = nullptr;
uint32_t s_serialRibbon = 0;
RIBBON3& Ribbon(LO* plo)
{
    if (s_pswRibbon != g_psw || (g_psw && s_serialRibbon != g_psw->serial))
    {
        s_mpribbon.clear();
        s_pswRibbon = g_psw;
        s_serialRibbon = g_psw ? g_psw->serial : 0;
    }
    auto [it, fNew] = s_mpribbon.try_emplace(plo);
    if (fNew)   // (the loaded width, optid 1805 / EOPID 2033; the init's 1 without one)
        for (auto o = plo->aopt.rbegin(); o != plo->aopt.rend(); ++o)
            if (o->eopid == 2033 && !o->aval.empty())
            {
                it->second.width = o->aval[0].k == 1 ? o->aval[0].ag[0] : float(o->aval[0].n);
                if (FPrint()) std::printf("hackgame3: ribbon %d width %.1f (loaded)\n", plo->oid, it->second.width);
                break;
            }
    return it->second;
}
}  // namespace
namespace
{
struct INITRIBBON3
{
    INITRIBBON3()
    {
        auto pob = [](const CRef& r) { return r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr; };
        RegisterObjectSetter3(2031, [](LO* plo, const CRef& r) { Ribbon(plo).pfrom = r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr; });
        RegisterObjectSetter3(2032, [](LO* plo, const CRef& r) { Ribbon(plo).pto = r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr; });
        RegisterObjectSetter3(2033, [](LO* plo, const CRef& r) { Ribbon(plo).width = r.m_tagk == TAGK_F32 ? r.m_g : float(r.m_n); });
        RegisterObjectGetter3(2031, [](LO* plo) { CRef r; if (LO* p = Ribbon(plo).pfrom) r.SetBasic(p); return r; });
        RegisterObjectGetter3(2032, [](LO* plo) { CRef r; if (LO* p = Ribbon(plo).pto) r.SetBasic(p); return r; });
        (void)pob;
    }
} s_initribbon3;
}  // namespace

void CollectRibbonDraws(const SCENE& scene, const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra)
{
    if (!g_psw || s_pswRibbon != g_psw) return;
    static int s_c = 0;
    const bool fLog = FPrint() && (++s_c % 120) == 0;
    for (auto& [plo, r] : s_mpribbon)
    {
        if (fLog)
            std::printf("hackgame3: ribbon %p in world %d removed %d from %p (%d) to %p (%d)\n", (const void*)plo, int(plo->fInWorld), int(plo->fRemoved),
                        (void*)r.pfrom, r.pfrom ? int(r.pfrom->fInWorld) : -1, (void*)r.pto, r.pto ? int(r.pto->fInWorld) : -1);
        if (!plo->fInWorld || plo->fRemoved || !r.pfrom || !r.pto || r.pfrom == r.pto || !r.pfrom->fInWorld || !r.pto->fInWorld) continue;
        const glm::vec3 a = PosLoWorld(r.pfrom), b = PosLoWorld(r.pto);
        const glm::vec3 d = b - a;
        glm::vec3 n = glm::cross(a - posCamera, d);
        const float len = glm::length(n);
        if (!(0.01f < len)) continue;
        const glm::vec3 x = glm::normalize(glm::cross(n, d));
        n *= r.width / len;
        glm::mat4 mat(1.0f);
        mat[0] = glm::vec4(x, 0.0f);
        mat[1] = glm::vec4(n, 0.0f);
        mat[2] = glm::vec4(d * 0.01f, 0.0f);
        mat[3] = glm::vec4(a, 1.0f);
        AppendAloExtraDraws(scene, static_cast<const ALO*>(plo), mat, 1.0f, aextra);
    }
}
bool FRibbonDrawnAsExtra(const LO* plo) { return plo && plo->Cid() == S3CID(0x81); }

bool FActive(const SW* psw) { return FCurrent(psw); }
bool FTakeSceneRebuild()
{
    const bool f = s_fSceneRebuild;
    s_fSceneRebuild = false;
    return f;
}
LO* PloShip(int iplayer) { return iplayer >= 0 && iplayer < s_g.cship ? s_g.aship[size_t(iplayer)] : nullptr; }
int CShips() { return s_g.cship; }

void OnWorldStart(SW* psw)
{
    s_g = GAME{};
    s_iround = 0;
    s_fAllHeld = s_fOpenSeen = false;
    s_pswCam = nullptr;
    s_agauge = {};
    s_aicharTinted.fill(-1);
    s_aicharTintedArrows.fill(-1);
    s_mpglobsetOrig.clear();
    if (!psw || !hackmp3::FGame(psw)) return;
    // the players: the game's first task's option 1402 (task 1815: 93052, 92990), as the world start takes them
    // (0x25B9C0: GAME +0x10 / +0x14)
    const JOB3* pjob = PjobFind3(OidChallengeJob3());
    const TASK3* pt = pjob ? pjob->ptaskFirst : nullptr;
    if (!pt) return;
        // (instances of the ship 92992: found by their instance oids, as the scripts' global find (0x15E938 kind 5))
    LO* aship[2] = {PloFindRelative(nullptr, pt->aoidPlayer[0], 5), PloFindRelative(nullptr, pt->aoidPlayer[1], 5)};
    if (!aship[0] || !aship[1] || !FHackShip3(aship[0]) || !FHackShip3(aship[1])) return;
    s_g.psw = psw;
    s_g.serial = psw->serial;
    s_g.aship[0] = aship[0];
    s_g.aship[1] = aship[1];
    s_g.cship = 2;
    if (std::getenv("SLY3_HACKGAMELAYOUT"))
        for (const auto& up : psw->aploAll)
        {
            LO* p = up.get();
            static const int s_aoid[] = {92946, 92939, 92884, 92895, 92956, 92955, 92954, 92953, 92948, 92853, 92966, 92964, 92962, 92960, 92958,
                                         92881, 92879, 92877, 92868, 92937, 92936, 92935, 92934, 92854, 92941, 92891, 92944};
            if (std::find(std::begin(s_aoid), std::end(s_aoid), p->oid) == std::end(s_aoid)) continue;
            const glm::vec3 v = PosLoWorld(p);
            std::printf("hackgame3 layout: %d inst %d cid 0x%X parent %d (inst %d) at %.0f %.0f %.0f in world %d\n", p->oid, p->oidInstance, p->Cid(),
                        p->paloParent ? p->paloParent->oid : -1, p->paloParent ? p->paloParent->oidInstance : -1, v.x, v.y, v.z, int(p->fInWorld));
        }
    // each ship's gauge: its loaded EOPID 948 (+0x168: 93052 names Bentley's 92988; the job's script names the second
    // player's, 92986 / 92984)
    for (LO* p : aship)
        for (auto o = p->aopt.rbegin(); o != p->aopt.rend(); ++o)
            if (o->eopid == 948 && !o->aval.empty() && o->aval[0].n > 0)
            {
                if (LO* pg = PloFindRelative(p, o->aval[0].n, 5)) SetSoGauge3(p, pg);
                break;
            }
    // player 1: added (vt slot 8), its pad (0x1F4B00(SW, 0, ship): the current player natively)
    if (!aship[0]->fInWorld) AddLo(psw, aship[0]);
    if (PloPoCurrent(psw) != aship[0]) EnablePo(psw, aship[0], 1);
    // player 2: split3's second seat (0x1F4B00(SW, 1, ship): its own pad record)
    if (!aship[1]->fInWorld) AddLo(psw, aship[1]);
    split3::SetPlayerCharacter(psw, 1, aship[1]);
    if (split3::FActive()) EnablePoExtra3(psw, aship[1]);
    if (FPrint())
        for (int cid : {0xA3})
            for (LO* p : psw->mpcidaplo[cid])
            {
                std::printf("hackgame3: gauge %d inst %d clone %d parent %d (inst %d) in world %d opts", p->oid, p->oidInstance, p->iclone,
                            p->paloParent ? p->paloParent->oid : -1, p->paloParent ? p->paloParent->oidInstance : -1, int(p->fInWorld));
                for (const OPT& o : p->aopt) std::printf(" %d=%d", o.eopid, o.aval.empty() ? -9 : o.aval[0].n);
                std::printf("\n");
            }
        for (LO* p : aship)
        {
            std::printf("hackgame3: ship inst %d opts", p->oidInstance);
            for (const OPT& o : p->aopt) std::printf(" %d=%d", o.eopid, o.aval.empty() ? -9 : o.aval[0].n);
            std::printf("\n");
        }
    // PC, three / four players (split3's seats, hackmp3's select): copies of player 2's ship beside it, each on its seat
    const int cseat = std::clamp(split3::CSeats(), 2, kMaxShips);
    for (int i = 2; i < cseat; ++i)
    {
        LO* pcopy = PloPcCloneIntoWorld(psw, aship[1], true);
        if (!pcopy || !FHackShip3(pcopy)) break;
        const glm::vec3 v = PosLoWorld(aship[i - 2]);
        SetAloPos(static_cast<ALO*>(pcopy), v + glm::vec3(0.0f, (v.y < 0.0f ? 1.0f : -1.0f) * 220.0f, 0.0f));
        s_g.aship[size_t(i)] = pcopy;
        // (the parts the original posts at the load (out of the world until a power-up: the laser 6130, the burst 93033,
        // the arrows 93018) are not cloned with their parent (brx.cpp PloCloneLo, as the original's 0x1F66F0): copied under
        // the copy here, in or out of the world as player 2's)
        for (LO* ppart : std::vector<LO*>(aship[1]->aploOwned))
        {
            if (FPrint()) std::printf("hackgame3:   player 2's part %d cid 0x%X parent %d removed %d in world %d\n", ppart->oid, ppart->Cid(),
                                      ppart->paloParent ? ppart->paloParent->oid : -1, int(ppart->fRemoved), int(ppart->fInWorld));
            if (!ppart->fRemoved || ppart->paloParent != aship[1]) continue;
            PloPcCloneIntoWorld(psw, ppart, ppart->fInWorld, static_cast<ALO*>(pcopy));
        }
        s_g.cship = i + 1;
        split3::SetPlayerCharacter(psw, i, pcopy);
        EnablePoExtra3(psw, pcopy);
        if (FPrint()) std::printf("hackgame3: player %d's ship: a copy of %d\n", i + 1, aship[1]->oidInstance);
        // (the copy's frame is a clone of player 2's: its module variables still name player 2's parts (the gun 74183, the
        // laser 6130, the burst 93033, the mirror gun 93036, the arrows 93018, the shield's SMA): its s0 (the module's
        // start, block 92992) run again on the copy makes them its own and its methods (2471 .. 2481) its own closures, so
        // its power-ups run in its own script as players 1 / 2's do)
        if (pcopy->pframe)
            if (CRef* pref = pcopy->pframe->PrefFindBinding(SYMID(0), 0); pref && pref->m_tagk == TAGK_Proc)
            {
                CRef rself;
                rself.SetBasic(pcopy);
                CallRef(*pref, {rself});
            }
    }
    s_mp2479.clear();
    MakeExitDocks(psw);
    WrapJobStats(psw);
    WrapJobRespawn(psw);
    split3::g_pfnScriptedSeatPilot = ScriptedSeatPilot;
    if (FPrint())
        std::printf("hackgame3: players %d / %d (seats %d, shared view %d)\n", aship[0]->oid, aship[1]->oid, split3::CSeats(),
                    int(split3::FSharedView()));
}

void OnShipUpdate(LO* plo)
{
    if (!FCurrent(g_psw) || plo != s_g.aship[0]) return;
    SyncCamera();
    SyncColours();
    if (!split3::FBinding() && NOptRef(s_g.aship[1], 2463, -1) != -1) SyncTints();
    SyncGauges();
    SyncChain();
    SyncExtraShips();
    SyncExitAlarms();
    LogRounds();
    DebugPowerUps();
    PrintState();
}

}  // namespace hackgame3
