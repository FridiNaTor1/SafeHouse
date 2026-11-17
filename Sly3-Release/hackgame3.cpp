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

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);   // mat.h (orig3 0x11CBA0)

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
        if (pnext && PloHackShipOther3(p) != pnext) SetHackShipOther3(p, pnext);
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
struct EXITDOCK { LO* porig; LO* pcopy; };
std::vector<EXITDOCK> s_aexit;
std::unordered_map<const BASIC*, CRef> s_mpexit2399, s_mpexit2400, s_mpexit2401;
std::vector<std::pair<LO*, int>> s_aalarmHeld;   // (player 1 / 2 docked while players 3 / 4 were out: ship, alarm)
int s_cExitOpen = 0;                              // (original docks opened and not yet released)
constexpr int kOidExitDock = 92941;

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
    }
    return CRef(TAGK_Void);
}
CRef ThunkExit2400(BASIC* pbasic, int c, CRef* a)
{
    auto it = s_mpexit2400.find(pbasic);
    if (it == s_mpexit2400.end() || c < 1) return CRef(TAGK_Void);
    LO* pdock = dynamic_cast<LO*>(pbasic);
    LO* pship = a[0].m_tagk == TAGK_Basic ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
    // (one ship a dock)
    if (LO* pheld = PloDockHeld(pdock); pheld && pheld != pship) return CRef(TAGK_Void);
    // (players 1 / 2 while a living player 3 / 4 is not held yet: the alarm held back)
    int iplayer = -1;
    for (int i = 0; i < s_g.cship; ++i)
        if (s_g.aship[size_t(i)] == pship) iplayer = i;
    bool fHold = false;
    if (iplayer == 0 || iplayer == 1)
        for (int i = 2; i < s_g.cship; ++i)
            if (LO* p = s_g.aship[size_t(i)]; FAlive(p) && HackShip3State(p) != 4) fHold = true;
    const int nAlarm = NOptRef(pship, 313, -1);
    if (fHold && nAlarm != -1)
    {
        SetOptRef(pship, 313, RefN(-1));
        CallRef(it->second, {a[0]});
        SetOptRef(pship, 313, RefN(nAlarm));
        s_aalarmHeld.push_back({pship, nAlarm});
        if (FPrint()) std::printf("hackgame3: player %d docked, its alarm %d held for players 3 / 4\n", iplayer + 1, nAlarm);
    }
    else if (iplayer >= 2 && nAlarm != -1)
    {
        // (players 3 / 4 fire no alarm: the job's script knows two ships; their 313 is player 2's again once its splice
        // re-arms it at a round's start)
        SetOptRef(pship, 313, RefN(-1));
        CallRef(it->second, {a[0]});
        SetOptRef(pship, 313, RefN(nAlarm));
        if (FPrint()) std::printf("hackgame3: player %d docked (no alarm)\n", iplayer + 1);
    }
    else
    {
        CallRef(it->second, {a[0]});
        if (FPrint()) std::printf("hackgame3: player %d docked\n", iplayer + 1);
    }
    return CRef(TAGK_Void);
}
void WrapProc(LO* p, int optid, std::unordered_map<const BASIC*, CRef>& mp, CRef (*pfn)(BASIC*, int, CRef*), int creq)
{
    if (!p || mp.count(p)) return;
    const CRef orig = RefOpt(p, optid);
    if (orig.m_tagk != TAGK_Proc) return;
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
// (OnWorldStart, three / four players: each original exit dock's copy beside it; players 3 / 4 fire no alarm)
void MakeExitDocks(SW* psw)
{
    s_aexit.clear();
    s_mpexit2399.clear();
    s_mpexit2400.clear();
    s_mpexit2401.clear();
    s_aalarmHeld.clear();
    s_cExitOpen = 0;
    // (work in progress, s3-hack round 9: SLY3_HACKGAME4DOCKS=1; not on by default until a whole 4P round end docks by
    // play: notes/HACK3_2026-11-12.md "Round 9")
    if (s_g.cship < 3 || !std::getenv("SLY3_HACKGAME4DOCKS")) return;
    std::vector<LO*> aorig;
    for (const auto& up : psw->aploAll)
        if (up->oid == kOidExitDock && up->Cid() == S3CID(0x47) && up->paloParent) aorig.push_back(up.get());
    for (LO* porig : aorig)
    {
        ALO* pparent = porig->paloParent;
        LO* pcopy = PloPcCloneIntoWorld(psw, porig, porig->fInWorld, pparent);
        if (!pcopy) continue;
        // (local (0, +-100) -> (0, +-300): 200 farther from the section's middle)
        const glm::vec3 vp = PosLoWorld(pparent), vo = PosLoWorld(porig);
        SetAloPos(static_cast<ALO*>(pcopy), vo + glm::vec3(0.0f, vo.y < vp.y ? -200.0f : 200.0f, 0.0f));
        // (the dock's trigger 92891 is out of the world until the open clip adds it (its option 27): the copy skips removed
        // children (brx.cpp PloCloneLo), so the copy had no trigger and its clip added the original's: cloned here, out of
        // the world as the original's)
        for (LO* pc : std::vector<LO*>(porig->aploOwned))
            if (pc->fRemoved && pc->Cid() == sly3::CID_TRIGGER)
                if (LO* pcNew = PloCloneLo(pc, psw, static_cast<ALO*>(pcopy))) RemoveLo(psw, pcNew);
        RunModuleStart(pcopy);
        for (LO* pchild : pcopy->aploOwned) RunModuleStart(pchild);
        // its state machines: the original's SMAs' SMs applied to the copy too (the SMs' post-load 0x1F00C8 ran at the
        // world's start, before the copy was made; without the SMA the copy's 2399 found none ((dock 190) 92944) and its
        // open clip never put its trigger 92891 in the world: s3-hack round 11)
        {
            std::vector<SMA*> aorigSma;
            for (SMA* psma : g_apsma)
                if (psma->palo == porig && psma->psm) aorigSma.push_back(psma);
            for (SMA* psma : aorigSma)
            {
                bool fHas = false;
                for (SMA* q : g_apsma) fHas = fHas || (q->palo == pcopy && q->psm == psma->psm);
                if (!fHas) PsmaApplySm(psma->psm, static_cast<ALO*>(pcopy), psma->psm->oidStateInitial, psma->psm->grfaplInitial);
                if (FPrint()) std::printf("hackgame3: exit dock copy %p: SM %d %s\n", (void*)pcopy, psma->psm->oid, fHas ? "had its SMA" : "applied");
            }
        }
        s_aexit.push_back({porig, pcopy});
        if (FPrint()) std::printf("hackgame3: exit dock %d (section %d) copied at %.0f %.0f %.0f\n", porig->oid, pparent->oid, vo.x, vo.y + (vo.y < vp.y ? -200.0f : 200.0f), vo.z);
    }
    for (const EXITDOCK& e : s_aexit)
        for (LO* p : {e.porig, e.pcopy})
        {
            WrapProc(p, 2400, s_mpexit2400, ThunkExit2400, 1);
            if (p == e.porig)
            {
                WrapProc(p, 2399, s_mpexit2399, ThunkExit2399, 0);
                WrapProc(p, 2401, s_mpexit2401, ThunkExit2401, 0);
            }
        }
    for (int i = 2; i < s_g.cship; ++i) SetOptRef(s_g.aship[size_t(i)], 313, RefN(-1));
}
// (each step: players 1 / 2's held alarms fired once every living player 3 / 4 is held in a dock)
void SyncExitAlarms()
{
    if (s_aalarmHeld.empty()) return;
    for (int i = 2; i < s_g.cship; ++i)
        if (LO* p = s_g.aship[size_t(i)]; FAlive(p) && HackShip3State(p) != 4) return;
    LO* pscpt = PloFindRelative(nullptr, 3338, 5);
    for (const auto& [pship, nAlarm] : s_aalarmHeld)
    {
        if (FPrint()) std::printf("hackgame3: players 3 / 4 docked: alarm %d fired\n", nAlarm);
        if (pscpt) CallOpt(pscpt, 998, {RefN(nAlarm), RefN(0)});
    }
    s_aalarmHeld.clear();
}

// (tests: SLY3_HACKGAMEPILOT="P:X Y@F;...": from frame F player P's (1..4, a scripted seat) left stick steers its ship
// toward the world point (X, Y) through the camera's facing, as a route's "move" does for player 1: docking by play)
struct PILOT { int iplayer, frame; float x, y; };
std::vector<PILOT> s_apilot;
void ScriptedSeatPilot(int iseat, int frame, float a[4], uint16_t*)
{
    static const bool s_fParsed = [] {
        if (const char* sz = std::getenv("SLY3_HACKGAMEPILOT"))
            for (const char* q = sz; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            {
                PILOT p{};
                if (std::sscanf(q, "%d:%f %f@%d", &p.iplayer, &p.x, &p.y, &p.frame) == 4) s_apilot.push_back(p);
                else if (std::sscanf(q, "%d:%f %f@open", &p.iplayer, &p.x, &p.y) == 3) { p.frame = -1; s_apilot.push_back(p); }   // (once the exit docks open)
            }
        return true;
    }();
    (void)s_fParsed;
    if (!FCurrent(g_psw) || iseat < 0 || iseat >= s_g.cship) return;   // (player 1 too when its seat is scripted)
    const PILOT* pcur = nullptr;
    for (const PILOT& p : s_apilot)
        if (p.iplayer == iseat + 1 && (p.frame < 0 ? s_cExitOpen > 0 : p.frame <= frame) && (!pcur || pcur->frame < p.frame)) pcur = &p;
    LO* pship = s_g.aship[size_t(iseat)];
    if (!pcur || !pship || !pship->fInWorld) return;
    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(pship));
    glm::vec2 d(pcur->x - pos.x, pcur->y - pos.y);
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
                    pcur->x, pcur->y, a[0], a[1], HackShip3State(pship));
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
        if (sl == 4 && s_cExitOpen > 0) continue;   // (a round's end: players 3 / 4 fly into their own exit docks)
        if ((sl == 4 || sl == 5) && s != sl && s != 6)
        {
            const glm::vec3 v = PosLoWorld(pl);
            SetAloPos(static_cast<ALO*>(p), v + glm::vec3(0.0f, (v.y < 0.0f ? 1.0f : -1.0f) * 220.0f, 0.0f));
            SetHackShip3State(p, sl);
        }
        else if (sl == 1 && (s == 4 || s == 5)) SetHackShip3State(p, 1);
    }
}

// (debug / screenshots: SLY3_HACKGAMEPOWER="P:N@F;..." player P (1-based) takes power-up N at frame F of the game)
void DebugPowerUps()
{
    static const char* s_sz = std::getenv("SLY3_HACKGAMEPOWER");
    if (!s_sz) return;
    static int s_frame = 0;
    ++s_frame;
    for (const char* q = s_sz; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
    {
        int ip = 0, n = 0, f = 0;
        if (std::sscanf(q, "%d:%d@%d", &ip, &n, &f) != 3 || f != s_frame) continue;
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
    std::printf("hackgame3 t %.1f: core %d/%d", g_clock.t, NOptRef(pcount, 1177, -1), NOptRef(pcount, 1178, -1));
    for (int i = 0; i < s_g.cship; ++i)
    {
        LO* p = s_g.aship[size_t(i)];
        int hp = -1;
        FHackHp3(p, false, &hp);
        auto inw = [&](int oid) { LO* q = PloFindRelative(p, oid, 4); return q ? int(q->fInWorld) : -1; };
        std::printf(" | ship %d hp %d st %d gun %d shots %d laser %d arrows %d glow %d", i + 1, hp, HackShip3State(p), NOptRef(p, 2474, -1),
                    NOptRef(p, 2475, -1), inw(6130), inw(93018), inw(74174));
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
                                         92881, 92879, 92877, 92868, 92937, 92936, 92935, 92934, 92854};
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
    DebugPowerUps();
    PrintState();
}

}  // namespace hackgame3
