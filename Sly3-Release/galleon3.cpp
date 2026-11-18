// galleon3.cpp: the Galleon Duel's PC parts (galleon3.h; notes/PLANE3_2026-11-12.md "Round 7").
#include "galleon3.h"

#include "activate.h"
#include "challenge3.h"
#include "clock.h"
#include "cm.h"
#include "joy.h"
#include "lo.h"
#include "po.h"
#include "splitscreen3.h"
#include "sw.h"
#include "update.h"
#include "guard.h"
#include "activate.h"
#include "game/ship3.h"
#include "game/task3.h"
#include "game/trigger3.h"
#include "hud.h"
#include "hudwidget.h"
#include "porttext.h"
#include "so.h"
#include "scenesw.h"
#include "spliceobj.h"
#include "ref.h"
#include "frame.h"
#include "gc.h"
#include "pair.h"
#include "duel3.h"
#include "eval.h"
#include "epmenu3.h"
#include "render.h"
#include "cmworld.h"
#include "screenrt.h"
#include "screenobj.h"
#include "sm.h"
#include "pcloader.h"
#include "game/sly3.h"
#include "scene.h"

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace galleon3
{
namespace
{
constexpr int kWorld = 32;     // p_at_sea
constexpr int kJob = 4209;     // m5_battle_multi (row 2027, task 1816)
constexpr int kShip = 44092;   // the gang's ship (its group 40 p_boat_hero; the duel's two are its instances 51380 / 51381)
static const int kIwrecWall = std::getenv("SLY3_GALLEONWALLW") ? std::atoi(std::getenv("SLY3_GALLEONWALLW")) : 29;
// (Episode 4's palace hall c_hall_b: the curtained paper screen 69964 behind which the Panda King's daughter Jing King
// stands (Get a Job / Grapple-Cam Break-in's room: General Tsao's hall), her silhouette on it the kind-8 shadow volumes
// of 69942 (seven pieces, shader 75659, darkening the lit paper by the frame's alpha as on the PS2). SLY3_GALLEONWALLOID=
// 73253: the folding screen c_partition as before (no silhouette); SLY3_GALLEONSHADOWOID=0: the screen without her)
static const int kOidPaperWall = std::getenv("SLY3_GALLEONWALLOID") ? std::atoi(std::getenv("SLY3_GALLEONWALLOID")) : 69964;
static const int kOidJingShadow = std::getenv("SLY3_GALLEONSHADOWOID") ? std::atoi(std::getenv("SLY3_GALLEONSHADOWOID")) : 69942;
constexpr int kOidHelm = 44234, kOidDeckSpot = 44236;   // (the ship's helm spot: the battle script's helmsman's place)
constexpr int kOidGuru = 29, kOidPandaKing = 30, kOidDimitri = 27899, kOidMurray = 26, kOidPenelope = 31;

// The original's views in the two-player duel (the reference, warp 32:1816, measured on its 512 x 448 frames, x 1.25 for
// the 640-unit canvas): player 1's scene x 0..388, y 0..220 (its gauge panel at the right, x 388..512), player 2's x
// 126..512, y 225..448 (the panel at the left); the score board's banner between them
bool FHole2p(int band, float a[4])
{
    if (!FDuelRunning() || split3::CView() != 2) return false;
    if (static const bool s_fOff = std::getenv("SLY3_GALLEONNOBOX") != nullptr; s_fOff) return false;   // (debug: the views not narrowed, the "before" shots)
    static const float s_aa[2][4] = {{0.0f, 0.0f, 485.0f, 220.0f}, {157.5f, 225.0f, 640.0f, 448.0f}};
    for (int i = 0; i < 4; ++i) a[i] = s_aa[band & 1][i];
    return true;
}

// (test, tools/galleon3.sh: SLY3_GALLEONPAD=mask) seat i's player (bit i) played on its pad by a bot: the stick walks the
// character over its deck (by its view's camera) to the use point of a ready cannon on the side facing the enemy, Circle
// fires it (the cannon's own use and script, as a player's press), L2 / R2 steer the ship to keep the enemy abeam. Unlike
// SLY3_GALLEONAUTO (which puts the character at the cannon and starts the use itself) everything goes through the pad.
bool FPrint() { static const bool s_f = std::getenv("SLY3_SHIPPRINT") != nullptr; return s_f; }
struct PADBOT { int frameUse = -100000; int frameLog = 0; };
std::array<PADBOT, 4> s_abot;
bool FPadPilot(int iseat, int frame, float a[4], uint16_t* pgrf)
{
    static const unsigned s_mask = std::getenv("SLY3_GALLEONPAD") ? unsigned(std::strtoul(std::getenv("SLY3_GALLEONPAD"), nullptr, 0)) : 0u;
    if (!s_mask || !((s_mask >> iseat) & 1) || iseat > 3 || !FDuelRunning()) return false;
    LO* pchar = split3::FActive() ? split3::PloPlayer(g_psw, iseat) : (iseat == 0 ? PloPoCurrent(g_psw) : nullptr);
    if (!pchar) return false;
    glm::vec3 pos(0.0f), vel(0.0f);
    float heading = 0.0f;
    unsigned grfSteer = 0;
    bool fUse = false;
    if (!FShipBroadside3(pchar, &pos, &heading, &vel, &grfSteer, &fUse)) return false;
    PADBOT& b = s_abot[size_t(iseat)];
    if (grfSteer & 1) *pgrf |= BTN_L2;
    if (grfSteer & 2) *pgrf |= BTN_R2;
    if (!fUse) return true;
    const glm::vec3 posChar = PosLoWorld(pchar);
    glm::vec2 d(pos.x - posChar.x, pos.y - posChar.y);
    const float dist = glm::length(d);
    // the stick by the view's camera (the camera's x row: the view direction)
    const CM& cm = iseat == 0 || !split3::FActive() ? g_cm : split3::CmView(iseat);
    glm::vec2 fwd(cm.mat[0].x, cm.mat[0].y);
    if (glm::length(fwd) < 1e-3f) fwd = glm::vec2(1.0f, 0.0f);
    fwd = glm::normalize(fwd);
    const glm::vec2 right(fwd.y, -fwd.x);
    if (dist > 35.0f)
    {
        const glm::vec2 dn = d / dist;
        const float k = std::clamp(dist / 120.0f, 0.45f, 1.0f);
        a[0] = glm::dot(dn, right) * k;
        a[1] = glm::dot(dn, fwd) * k;
    }
    // at the cannon: Circle (held 6 frames), then 2 s for the shot before the next
    if (dist < 60.0f && frame - b.frameUse > 120) b.frameUse = frame;
    if (frame - b.frameUse < 6) *pgrf |= BTN_Circle;
    if (FPrint() && frame - b.frameLog >= (std::getenv("SLY3_GALLEONPADLOG") ? 20 : 120))
    {
        b.frameLog = frame;
        std::printf("galleon3: pad seat %d (%d) at %.0f %.0f %.0f to %.0f %.0f %.0f dist %.0f stick %.2f %.2f steer %u (frame %d)\n", iseat, pchar->oid, posChar.x, posChar.y, posChar.z, pos.x, pos.y, pos.z, dist, a[0], a[1],
                    grfSteer, frame);
    }
    return true;
}

int s_cChosen = 0;   // (0: not read yet)
int CChosenRaw()
{
    if (s_cChosen == 0)
    {
        s_cChosen = 2;
        if (const char* sz = std::getenv("SLY3_GALLEONPLAYERS")) s_cChosen = std::clamp(std::atoi(sz), 2, 4);
    }
    return s_cChosen;
}
constexpr int kTarget = 3;          // (the original duel's: three wins)
constexpr float kRoundEnd = 3.0f;   // (the last ship afloat, then the next round: the original's restore comes 2.5 s after a sink)
constexpr float kHitWindow = 15.0f; // (a sink within this of a ship's last hit counts for the shooter)
constexpr float kMsg = 3.0f, kWinHold = 6.0f;
// a sunk player's view (UpdateSpectators): its own camera for the sinking, then the battle watched from above the sea
static const float kSpecBack = std::getenv("SLY3_GALLEONSPECBACK") ? float(std::atof(std::getenv("SLY3_GALLEONSPECBACK"))) : 1.2f;
constexpr float kSpecDelay = 2.5f, kSpecDist = 6500.0f, kSpecHeight = 2400.0f, kSeaZ = 0.0f;
struct PLAYER4
{
    LO* pchar = nullptr;
    LO* pship = nullptr;
    int points = 0;     // (rounds won: the last ship afloat)
    int sinks = 0;      // (ships it sank)
    bool fOut = false;  // (sunk this round)
    float tOverboard = -1.0f;
    float tOut = -1.0f;
    int iKiller = -1;   // (the player whose ship sank it)
    // (out of the round: its view watches the battle, UpdateSpectators)
    bool fSpec = false;
    int iWatch = -1;
    float tSpecEnd = -1.0f;
    glm::vec3 posSpec{0.0f}, posLook{0.0f};
    CM cmSpec;
};
struct ROUND4
{
    bool fActive = false;
    SW* psw = nullptr;
    int cplayer = 0;
    std::array<PLAYER4, 4> ap;
    bool fSetUp = false;
    int round = 0;
    float tRoundEnd = -1.0f;
    float tRoundStart = 0.0f;
    int winner = -1;
    float tWin = 0.0f;
    bool fEnded = false;
    std::string msg;
    int iMsg = -1;
    float tMsg = -100.0f;
    int leader = -1;
    LO* pwall = nullptr;   // (player 4's booth's front screen: a copy from c_hall_b; awall[0])
    struct WALL { LO* plo = nullptr; glm::mat4 mat{1.0f}; glm::vec3 pos{0.0f}, n{1.0f, 0.0f, 0.0f}; };
    WALL awall[4];         // (the booth around his wheel: front (toward the main deck, her silhouette), back, the sides)
    int cwall = 0;
    LO* pshadow = nullptr;    // (Jing King's silhouette on the front screen: a copy of c_hall_b's 69942)
    glm::vec3 posWallKept{0.0f};    // (the screen's place in c_hall_b)
    glm::vec3 dposShadow{0.0f};     // (her place there from the screen's)
    bool fWallPlaced = false;
} r;
bool s_fKeptWanted = false;

// ---- the splice's object methods called natively (the duel splice's steps, s1621 / s3398, for every ship)
CRef RefOpt(LO* plo, int optid)
{
    if (!plo || !g_splicegame.pfnRefOption) return CRef();
    return g_splicegame.pfnRefOption(111, plo, optid, nullptr);
}
void SetOptRef(LO* plo, int optid, const CRef& v)
{
    if (plo && g_splicegame.pfnRefOption) g_splicegame.pfnRefOption(110, plo, optid, &v);
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
CRef CallOpt(LO* plo, int optid, std::vector<CRef> aref = {}) { return CallRef(RefOpt(plo, optid), std::move(aref)); }
CRef RefN(int n) { CRef v; v.SetS32(n); return v; }
CRef RefB(bool f) { CRef v; v.SetBool(f); return v; }
CRef RefG(float g) { CRef v; v.SetF32(g); return v; }
CRef RefLo(LO* plo) { CRef v; if (plo) v.SetBasic(plo); return v; }
// the world's option 983 (player i's pad to an object: a ship sails), as the splice's ((world 983) i ship)
void SetSeatObject(int i, LO* plo)
{
    BASIC* pw = g_splicegame.pfnPbasicGlobal ? g_splicegame.pfnPbasicGlobal(117) : nullptr;
    if (!pw || !g_splicegame.pfnRefOption) return;
    CallRef(g_splicegame.pfnRefOption(111, pw, 983, nullptr), {RefN(i), RefLo(plo)});
}

std::string StrPlayer(int i)
{
    char ach[64];
    std::snprintf(ach, sizeof ach, StrPort(PTX_SplitPlayerN).c_str(), i + 1);
    return ach;
}
std::string Fmt1(PTX ptx, const std::string& a)
{
    char ach[200];
    std::snprintf(ach, sizeof ach, StrPort(ptx).c_str(), a.c_str());
    return ach;
}
std::string Fmt2(PTX ptx, const std::string& a, const std::string& b)
{
    char ach[240];
    std::snprintf(ach, sizeof ach, StrPort(ptx).c_str(), a.c_str(), b.c_str());
    return ach;
}
void Message(const std::string& str, int i)
{
    r.msg = str;
    r.iMsg = i;
    r.tMsg = g_clock.t;
    std::printf("galleon3: message \"%s\" (t %.2f)\n", str.c_str(), g_clock.t);
}
int IplayerOfShip(const LO* plo)
{
    for (int i = 0; i < r.cplayer; ++i)
        if (plo && (r.ap[size_t(i)].pship == plo || r.ap[size_t(i)].pchar == plo)) return i;
    return -1;
}

// the players' ships: player 1 Sly's (the instance 51380), player 2 Bentley's (51381), players 3 / 4 the extra copies
LO* PloShipOfPlayer(SW* psw, int i)
{
    std::vector<LO*> aextra;
    for (LO* plo : AploOfOid(psw, kShip))
    {
        if (plo->oid != kShip || !FShip3(plo)) continue;
        if (i == 0 && plo->oidInstance == 51380) return plo;
        if (i == 1 && plo->oidInstance == 51381) return plo;
        if (plo->oidInstance != 51380 && plo->oidInstance != 51381) aextra.push_back(plo);
    }
    std::stable_sort(aextra.begin(), aextra.end(), [](const LO* a, const LO* b) { return a->iclone < b->iclone; });
    return i >= 2 && size_t(i - 2) < aextra.size() ? aextra[size_t(i - 2)] : nullptr;
}

// the ships' places for a round: players 1 / 2's as the original's (their proxies: (20000, 5000) / (-20000, -5000)), players
// 3 / 4 on the other diagonal, each facing the middle
void PlaceShip(int i)
{
    static const glm::vec2 s_apos[4] = {{20000.0f, 5000.0f}, {-20000.0f, -5000.0f}, {-5000.0f, 20000.0f}, {5000.0f, -20000.0f}};
    ALO* pa = dynamic_cast<ALO*>(r.ap[size_t(i)].pship);
    if (!pa) return;
    const glm::vec2 p = s_apos[size_t(i)];
    const float rad = std::atan2(-p.y, -p.x) + 1.5707964f;   // (broadside to the middle: the guns face it)
    SetAloPos(pa, glm::vec3(p, PosLoWorld(pa).z));
    SetAloRotation(pa, glm::mat3(glm::vec3(std::cos(rad), std::sin(rad), 0.0f), glm::vec3(-std::sin(rad), std::cos(rad), 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
}

LO* PloInTreeOf(LO* root, int oid)
{
    ALO* pa = dynamic_cast<ALO*>(root);
    if (!pa) return nullptr;
    for (LO* ch : pa->aploChild)
    {
        if (!ch) continue;
        if (ch->oid == oid || ch->oidInstance == oid) return ch;
        if (LO* q = PloInTreeOf(ch, oid)) return q;
    }
    return nullptr;
}

// a round: every ship restored, its player's pad given to it again (it sails: its machine puts its character at the helm,
// then the battle on the deck), the battle manager's battle of all the ships (the splice's s3398 for two)
void StartRound()
{
    SW* psw = r.psw;
    if (!psw) return;
    for (LO* plo : std::vector<LO*>(psw->mpcidaplo[0x59]))
        if (plo && plo->fInWorld) RemoveLo(psw, plo);   // (the balls in flight: (find-objects-by-class 89) each removed)
    for (int i = 0; i < r.cplayer; ++i)
    {
        PLAYER4& p = r.ap[size_t(i)];
        p.fOut = false;
        p.tOverboard = -1.0f;
        p.tOut = -1.0f;
        // (a next round: every view shows its player's restored ship from above the sea until the character is back on
        // its deck, then its own camera cuts to it: the views' cameras eased across the sea from the last round's places
        // (the ships are placed anew), the sunk players' from under the water)
        if (r.round > 0 && i < split3::CView())
        {
            if (!p.fSpec)
            {
                p.cmSpec = split3::CmView(i);
                p.posSpec = p.cmSpec.pos;
                p.posLook = p.posSpec + p.cmSpec.mat[0] * 2000.0f;
            }
            p.fSpec = true;
            p.iWatch = -2;   // (its own ship: a cut to it)
            p.tSpecEnd = g_clock.t + kSpecBack;
        }
        if (p.pship) SetShipGaugeKept3(p.pship, false);
        if (!p.pship) continue;
        SetSeatObject(i, nullptr);
        CallOpt(p.pship, 948);
        if (i >= 2 || r.round > 0) PlaceShip(i);
        SetSeatObject(i, p.pship);
        if (LO* pm = dynamic_cast<LO*>(RefOpt(p.pship, 946).m_pbasic)) CallOpt(pm, 304);
    }
    EndShipBattle3();
    SetShipBattleTwoPlayer3(1);
    std::vector<LO*> aother;
    for (int i = 1; i < r.cplayer; ++i)
        if (r.ap[size_t(i)].pship) aother.push_back(r.ap[size_t(i)].pship);
    StartShipBattle3(r.ap[0].pship, aother, 1);
    for (int i = 0; i < r.cplayer; ++i)
        if (r.ap[size_t(i)].pship) SetOptRef(r.ap[size_t(i)].pship, 1533, RefG(0.9f));
    r.tRoundEnd = -1.0f;
    r.tRoundStart = g_clock.t;
    ++r.round;
    std::printf("galleon3: round %d starts (t %.2f)\n", r.round, g_clock.t);
}

// players 3 / 4 join the duel once the splice has started it (its s1621: the two ships, the board, the helmsmen): their
// ships (owner: the player's character), the helmsmen (player 3: Dimitri at the helm, as Murray / Penelope are put there)
void SetUp()
{
    SW* psw = r.psw;
    // players 3 / 4's ships: rows of the gang ship's group attached through one of its proxies (as the duel's two, 51380 /
    // 51381, are; a spare row used as it is had no collision: the balls passed through it)
    LO* pproxy = nullptr;
    int iweShip = -1;
    for (size_t k = 0; k < psw->awe.size(); ++k)
        if (psw->awe[k].x10 == kShip) iweShip = int(k);
    for (auto& [pp, ps] : psw->mpproxy)
        if (ps.iwe == iweShip && pp && (!pproxy || pp->oid < pproxy->oid)) pproxy = pp;
    for (int i = 2; i < r.cplayer; ++i)
        if (LO* pnew = pproxy ? PloAttachProxy(psw, pproxy, 2) : nullptr)
        {
            r.ap[size_t(i)].pship = pnew;
            std::printf("galleon3: player %d's ship attached by proxy %d: clone %d\n", i + 1, pproxy->oid, pnew->iclone);
        }
    // (no proxy row free: the spare row taken as a proxy's attach does: its objects out of the spare set, each told its
    // attach (event 17: the ship's module sets up its barriers, its cannons' procedures; the hull's collision))
    if (iweShip >= 0)
    {
        WE& we = psw->awe[size_t(iweShip)];
        for (int i = 2; i < r.cplayer; ++i)
        {
            LO* pship = r.ap[size_t(i)].pship ? r.ap[size_t(i)].pship : PloShipOfPlayer(psw, i);
            if (!pship || !pship->fSpareNs || we.cloLoaded <= 0) continue;
            const size_t j0 = size_t(pship->iclone) * size_t(we.cloLoaded);
            CRef refNone;
            int c = 0;
            std::vector<LO*> adone;
            for (size_t k = 0; k < size_t(we.cloLoaded) && j0 + k < we.aplo.size(); ++k)
            {
                std::function<void(LO*)> attach = [&](LO* plo) {
                    if (!plo || std::find(adone.begin(), adone.end(), plo) != adone.end()) return;
                    adone.push_back(plo);
                    plo->fSpareNs = false;
                    HandleLoSpliceEvent(plo, SPLICEEV3_ATTACH, false, 1, &refNone);
                    ++c;
                    if (ALO* pa = dynamic_cast<ALO*>(plo))
                        for (LO* ch : pa->aploChild) attach(ch);
                };
                attach(we.aplo[j0 + k]);
            }
            std::printf("galleon3: player %d's ship (clone %d): its row's %d objects attached\n", i + 1, pship->iclone, c);
        }
    }
    for (int i = 0; i < r.cplayer; ++i)
    {
        PLAYER4& p = r.ap[size_t(i)];
        if (!p.pship || i < 2) p.pship = PloShipOfPlayer(psw, i);
        p.pchar = split3::PloPlayer(psw, i);
        if (i < 2 || !p.pship) continue;
        if (p.pchar && !p.pchar->fInWorld) AddLo(psw, p.pchar);
        // (the Panda King's update (0x1E9500) is off where the duel's world loads him: on, as an active player's)
        if (p.pchar && p.pchar->Cid() == sly3::CID_PANDA_KING)
        {
            SetLoSt(psw, p.pchar, 1);
            if (p.pchar->iupd >= 0) psw->aupd[size_t(p.pchar->iupd)].grf |= GRFUPD_Update;
            else p.pchar->grfUpd |= GRFUPD_Update;
        }
        SetOptRef(p.pship, 1755, RefLo(p.pchar));   // (the ship's character, EOPID 1978)
        CallOpt(p.pship, 41);
        if (p.pchar) SetOptRef(p.pchar, 861, RefB(false));
        if (i == 2)
            if (LO* pdim = PloFindSwObjectByOid(psw, kOidDimitri))
            {
                CallOpt(pdim, 41);
                CallOpt(pdim, 39, {RefLo(p.pship)});
                CallOpt(pdim, 35, {RefLo(p.pship), RefN(3)});
            }
    }
    // players 3 / 4's gauges: copies of players 1 / 2's (the duel's 57297 / 57295; the extra ships' option names the story's
    // 44149, one object for both), the face part blank (0x7AF): their portraits are drawn in the oval (FHudPiece)
    for (int i = 2; i < r.cplayer; ++i)
    {
        PLAYER4& p = r.ap[size_t(i)];
        LO* ptmpl = p.pship ? PloShipGauge3(r.ap[size_t(i - 2)].pship) : nullptr;
        if (!ptmpl) continue;
        if (LO* pg = PloPcCloneIntoWorld(psw, ptmpl, true))
        {
            SetShipGauge3(p.pship, pg);
            SetShipGaugeFace3(p.pship, 0x7AF);
            std::printf("galleon3: player %d gauge: a copy of %d\n", i + 1, ptmpl->oid);
        }
    }
    for (int i = 2; i < r.cplayer; ++i)
    {
        const char* sz = std::getenv(i == 2 ? "SLY3_GALLEONFACE3" : "SLY3_GALLEONFACE4");
        if (sz && r.ap[size_t(i)].pship) SetShipGaugeFace3(r.ap[size_t(i)].pship, int(std::strtol(sz, nullptr, 0)));
    }
    for (int i = 0; i < r.cplayer; ++i)
        std::printf("galleon3: player %d character %d ship %p (instance %d clone %d)\n", i + 1, r.ap[size_t(i)].pchar ? r.ap[size_t(i)].pchar->oid : -1,
                    (void*)r.ap[size_t(i)].pship, r.ap[size_t(i)].pship ? r.ap[size_t(i)].pship->oidInstance : -1, r.ap[size_t(i)].pship ? r.ap[size_t(i)].pship->iclone : -1);
    r.fSetUp = true;
    char ach[128];
    std::snprintf(ach, sizeof ach, StrPort(PTX_GalleonFirstTo).c_str(), kTarget);
    Message(ach, -1);
    StartRound();
}

// (debug, the shots of player 4's helm: SLY3_GALLEONLOOK="view,player,angle,dist,height": that view watches that player's
// helm from the ship's frame (angle in degrees from the ship's x axis))
bool FDebugLook(int iv, PLAYER4& p)
{
    static const char* s_sz = std::getenv("SLY3_GALLEONLOOK");
    if (!s_sz) return false;
    int v = 0, ip = 3;
    float ang = 0.0f, dist = 1500.0f, h = 600.0f;
    std::sscanf(s_sz, "%d,%d,%f,%f,%f", &v, &ip, &ang, &dist, &h);
    if (v != iv || ip < 0 || ip >= r.cplayer || !r.ap[size_t(ip)].pship) return false;
    LO* pship = r.ap[size_t(ip)].pship;
    LO* phelm = PloInTreeOf(pship, kOidHelm);
    const glm::mat4 m = sly3::MatLoWorld3(pship);
    const glm::vec3 ax = glm::normalize(glm::vec3(m[0])), ay = glm::normalize(glm::vec3(m[1]));
    const glm::vec3 c = phelm ? PosLoWorld(phelm) : PosLoWorld(pship);
    const float a = glm::radians(ang);
    if (!p.fSpec) p.cmSpec = split3::CmView(iv);
    p.fSpec = true;
    p.posSpec = c + (ax * std::cos(a) + ay * std::sin(a)) * dist + glm::vec3(0.0f, 0.0f, h);
    p.posLook = c + glm::vec3(0.0f, 0.0f, 150.0f);
    SetCmLookAt(&p.cmSpec, p.posSpec, p.posLook, 0.0f, 0.0f);
    return true;
}

// ---- a sunk player's view. Its own camera follows its character, who goes down with the ship: under the sea the view was
// black but the sky (Sly floating in the dark, the gauge gone with the ship) until the round's end. After the sinking
// (kSpecDelay, at once when the camera goes under the water) the view watches the battle from above the sea instead: the
// ship that sank it (else the nearest afloat), the camera eased to a point above the water at a distance from it; at the
// next round it stays until the character is back on its ship (kSpecBack), then the own camera cuts back to it.
void UpdateSpectators()
{
    const float t = g_clock.t, dt = std::max(g_clock.dt, 0.0f);
    for (int i = 0; i < r.cplayer && i < split3::CView(); ++i)
    {
        PLAYER4& p = r.ap[size_t(i)];
        if (FDebugLook(i, p)) continue;
        if (!p.fOut)
        {
            if (!p.fSpec) continue;
            const bool fBack = p.pchar && p.pship && PosLoWorld(p.pchar).z > kSeaZ + 300.0f;
            if (t >= p.tSpecEnd && (fBack || t >= p.tSpecEnd + 3.0f))
            {
                p.fSpec = false;
                CutCm(const_cast<CM*>(&split3::CmView(i)));
                std::printf("galleon3: player %d's view back on its character (z %.0f, t %.2f)\n", i + 1, p.pchar ? PosLoWorld(p.pchar).z : 0.0f, t);
                continue;
            }
        }
        if (!p.fSpec)
        {
            const CM& cmOwn = split3::CmView(i);   // (not watching yet: the view's own camera)
            if (t - p.tOut < kSpecDelay && cmOwn.pos.z > kSeaZ + 40.0f) continue;
            p.cmSpec = cmOwn;
            p.posSpec = glm::vec3(cmOwn.pos.x, cmOwn.pos.y, std::max(cmOwn.pos.z, kSeaZ + 600.0f));
            p.posLook = p.posSpec + glm::vec3(cmOwn.mat[0].x, cmOwn.mat[0].y, 0.0f) * 2000.0f;
            p.iWatch = p.iKiller;
            std::printf("galleon3: player %d's view watches the battle (t %.2f)\n", i + 1, t);
        }
        // the watched ship: still afloat, else the nearest afloat (a cut to it: easing across the sea showed the water from
        // close by on the way)
        const int iWatchOld = p.iWatch;
        bool fCut = !p.fSpec;
        if (!p.fOut) p.iWatch = i;   // (the round's restore: its own ship)
        else if (p.iWatch < 0 || r.ap[size_t(p.iWatch)].fOut || !r.ap[size_t(p.iWatch)].pship)
        {
            p.iWatch = -1;
            float dBest = 1e30f;
            for (int j = 0; j < r.cplayer; ++j)
                if (const PLAYER4& o = r.ap[size_t(j)]; j != i && !o.fOut && o.pship)
                    if (const float d = glm::length(PosLoWorld(o.pship) - p.posSpec); d < dBest) dBest = d, p.iWatch = j;
        }
        fCut = fCut || p.iWatch != iWatchOld;
        p.fSpec = true;
        glm::vec3 posWant = p.posSpec, posLookWant = p.posLook;
        if (p.iWatch >= 0)
        {
            const glm::vec3 tgt = PosLoWorld(r.ap[size_t(p.iWatch)].pship);
            glm::vec2 d(p.posSpec.x - tgt.x, p.posSpec.y - tgt.y);
            d = glm::length(d) > 1.0f ? glm::normalize(d) : glm::vec2(1.0f, 0.0f);
            posWant = glm::vec3(tgt.x + d.x * kSpecDist, tgt.y + d.y * kSpecDist, kSeaZ + kSpecHeight);
            posLookWant = glm::vec3(tgt.x, tgt.y, kSeaZ + 700.0f);
        }
        const float k = fCut ? 1.0f : 1.0f - std::exp(-dt * 1.4f);
        p.posSpec += (posWant - p.posSpec) * k;
        p.posSpec.z = std::max(p.posSpec.z, kSeaZ + 300.0f);
        p.posLook += (posLookWant - p.posLook) * k;
        SetCmLookAt(&p.cmSpec, p.posSpec, p.posLook, 0.0f, 0.0f);
    }
}

void Out(int i)
{
    PLAYER4& p = r.ap[size_t(i)];
    if (p.fOut || r.winner >= 0 || r.tRoundEnd >= 0.0f)
    {
        if (!p.fOut) { p.tOut = g_clock.t; if (p.pship) SetShipGaugeKept3(p.pship, true); }
        p.fOut = true;
        return;
    }
    p.fOut = true;
    p.tOut = g_clock.t;
    if (p.pship) SetShipGaugeKept3(p.pship, true);
    float t = -100.0f;
    LO* pby = p.pship ? PloShipHitBy3(p.pship, &t) : nullptr;
    const int ik = pby && g_clock.t - t <= kHitWindow ? IplayerOfShip(pby) : -1;
    p.iKiller = ik != i ? ik : -1;
    if (ik >= 0 && ik != i)
    {
        r.ap[size_t(ik)].sinks++;
        Message(Fmt2(PTX_GalleonSank, StrPlayer(ik), StrPlayer(i)), ik);
    }
    else Message(Fmt1(PTX_GalleonSunk, StrPlayer(i)), i);
    int calive = 0;
    for (int j = 0; j < r.cplayer; ++j) calive += r.ap[size_t(j)].fOut ? 0 : 1;
    std::printf("galleon3: player %d out (by %d), %d afloat (t %.2f)\n", i + 1, ik + 1, calive, g_clock.t);
    if (calive <= 1) r.tRoundEnd = g_clock.t + kRoundEnd;
}

void EndRound()
{
    int iwin = -1;
    for (int j = 0; j < r.cplayer; ++j)
        if (!r.ap[size_t(j)].fOut) iwin = j;
    if (iwin < 0)
    {
        Message(StrPort(PTX_GalleonNoneAfloat), -1);
        StartRound();
        return;
    }
    PLAYER4& w = r.ap[size_t(iwin)];
    w.points++;
    std::printf("galleon3: round %d to player %d; points", r.round, iwin + 1);
    for (int j = 0; j < r.cplayer; ++j) std::printf(" %d", r.ap[size_t(j)].points);
    std::printf("\n");
    if (w.points >= kTarget)
    {
        r.winner = iwin;
        r.tWin = g_clock.t;
        Message(Fmt1(PTX_DuelWins, StrPlayer(iwin)), iwin);
        std::printf("galleon3: winner player %d (%d rounds, %d sinks)\n", iwin + 1, w.points, w.sinks);
        EndShipBattle3();
        return;
    }
    int best = -1, cbest = 0;
    for (int j = 0; j < r.cplayer; ++j)
    {
        const int k = r.ap[size_t(j)].points;
        if (best < 0 || k > r.ap[size_t(best)].points) best = j, cbest = 1;
        else if (k == r.ap[size_t(best)].points) ++cbest;
    }
    if (cbest == 1 && best != r.leader) { r.leader = best; Message(Fmt1(PTX_DuelLeads, StrPlayer(best)), best); }
    else Message(Fmt1(PTX_GalleonRound, StrPlayer(iwin)), iwin);
    StartRound();
}

void DumpScreenObj(LO* plo)
{
    if (!plo || !dynamic_cast<SPRITESET*>(plo)) return;
    const SCREENOBJ* pso = static_cast<SPRITESET*>(plo)->pso.get();
    if (!pso) return;
    std::printf("galleon3: screen %d (cid 0x%X): %zu elements, %zu groups\n", plo->oid, unsigned(plo->Cid()), pso->aelem.size(), pso->agroup.size());
    for (size_t g = 0; g < pso->agroup.size(); ++g)
        for (const SPART& pt : pso->agroup[g].apart)
        {
            std::printf("  group %zu part 0x%X (%d):", g, unsigned(pt.id), pt.id);
            if (!pt.akey.empty())
                for (const SITEM& it : pt.akey[0].aitem) std::printf(" %u", unsigned(it.aw[0]));
            std::printf("\n");
        }
    for (size_t e = 0; e < pso->aelem.size(); ++e)
    {
        const SELEM& el = pso->aelem[e];
        glm::vec2 lo(1e9f), hi(-1e9f);
        for (const SPOLY& p : el.apoly)
            for (const auto& xy : p.axy) lo = glm::min(lo, glm::vec2(xy[0], xy[1]) / 16.0f), hi = glm::max(hi, glm::vec2(xy[0], xy[1]) / 16.0f);
        std::printf("  elem %zu kind %u polys %zu box %.0f %.0f %.0f %.0f\n", e, el.kind, el.apoly.size(), lo.x, lo.y, hi.x, hi.y);
    }
}
void DumpWorld(SW* psw)
{
    for (LO* plo : psw->mpcidaplo[0xA3])
        std::printf("galleon3: gauge 0xA3 %d parent %d in world %d\n", plo->oid, plo->paloParent ? plo->paloParent->oid : -1, int(plo->fInWorld));
    if (const char* sz = std::getenv("SLY3_GALLEONDUMPSO"))
        for (LO* plo : AploOfOid(psw, std::atoi(sz)))
            if (!std::getenv("SLY3_GALLEONDUMPSOPARENT") || (plo->paloParent && plo->paloParent->oid == std::atoi(std::getenv("SLY3_GALLEONDUMPSOPARENT")))) { DumpScreenObj(plo); break; }
    std::printf("galleon3: dump: ships\n");
    for (LO* plo : psw->mpcidaplo[0x58])
        std::printf("  ship %d inst %d clone %d parent %d in world %d removed %d at %.0f %.0f %.0f\n", plo->oid, plo->oidInstance, plo->iclone,
                    plo->paloParent ? plo->paloParent->oid : -1, int(plo->fInWorld), int(plo->fRemoved), PosLoWorld(plo).x, PosLoWorld(plo).y, PosLoWorld(plo).z);
    for (LO* plo : AploOfOid(psw, 44908))
    {
        const LO* root = plo;
        while (root->paloParent) root = root->paloParent;
        std::printf("  cannon %d clone %d parent %d root %d/%d in world %d at %.0f %.0f %.0f\n", plo->oid, plo->iclone, plo->paloParent ? plo->paloParent->oid : -1,
                    root->oid, root->iclone, int(plo->fInWorld), PosLoWorld(plo).x, PosLoWorld(plo).y, PosLoWorld(plo).z);
    }
    for (int cid : {0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3B, 0x3A})
        for (LO* plo : psw->mpcidaplo[size_t(cid)])
            std::printf("  cid 0x%X %d inst %d clone %d parent %d in world %d at %.0f %.0f %.0f\n", cid, plo->oid, plo->oidInstance, plo->iclone,
                        plo->paloParent ? plo->paloParent->oid : -1, int(plo->fInWorld), PosLoWorld(plo).x, PosLoWorld(plo).y, PosLoWorld(plo).z);
}

// ---- drawing (duel3.cpp's style: the HUD font, the players' colours)
constexpr float DEPTH = 16760000.0f;
glm::vec4 Rgb(int rr, int g, int b, int a = 128) { return glm::vec4(float(rr), float(g), float(b), float(a)); }
glm::vec4 RgbOfPlayer(int i, int a = 128)
{
    const uint32_t c = RgbaPlayer(i);
    return glm::vec4(float(c & 0xFF), float((c >> 8) & 0xFF), float((c >> 16) & 0xFF), float(a));
}
glm::vec4 OffOf(int i)
{
    const glm::vec4 c = RgbOfPlayer(i);
    return glm::vec4(std::min(128.0f, c.r * 0.6f + 50.0f), std::min(128.0f, c.g * 0.6f + 50.0f), std::min(128.0f, c.b * 0.6f + 50.0f), 0.0f);
}
void Text(const std::string& text, float x, float y, float size, glm::vec4 off, int halign = 1)
{
    const FONTBRX* pfont = PfontHud();
    if (!pfont) return;
    g_hud.DrawTextBox(*pfont, PfontHudIcons(), text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, halign, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH + 100.0f, false);
}
void Rect(float x0, float y0, float x1, float y1, glm::vec4 rgba, float dz = 0.0f)
{
    g_hud.FillScreenRect(x0, y0, x1, y1, rgba, (DEPTH + dz) / 16777216.0f);
}
constexpr float kXAspect = 640.0f / (448.0f * 4.0f / 3.0f);
void Arrow(float cx, float cy, float ang, float sz, glm::vec4 rgba, float dz)
{
    const glm::vec2 d(std::cos(ang), std::sin(ang)), n(-d.y, d.x);
    auto pt = [&](glm::vec2 v) { return glm::vec2(cx + v.x * kXAspect, cy + v.y); };
    const glm::vec2 a[3] = {pt(d * sz), pt(-d * sz * 0.6f + n * sz * 0.7f), pt(-d * sz * 0.6f - n * sz * 0.7f)};
    g_hud.FillScreenTris(a, 3, rgba, (DEPTH + dz) / 16777216.0f);
}

// ---- players 3 / 4's gauges: each view shows its player's ship gauge only (screenrt.h g_pfnScreenObjSkip3), players 3 / 4's
// with their characters' own portraits in the face's oval: the Guru's / the Panda King's faces of their story gauges (class
// 0xA3 under 29 / 30: the part 966's element)
const SELEM* PelemCharFace(int i)
{
    if (!r.psw || i < 2) return nullptr;
    const int oidChar = i == 2 ? kOidGuru : kOidPandaKing;
    for (LO* plo : r.psw->mpcidaplo[0xA3])
    {
        if (!plo || !plo->paloParent || plo->paloParent->oid != oidChar || !dynamic_cast<SPRITESET*>(plo)) continue;
        const SCREENOBJ* pso = static_cast<SPRITESET*>(plo)->pso.get();
        if (!pso) continue;
        for (const SGROUP& g : pso->agroup)
            for (const SPART& pt : g.apart)
                if (pt.id == 966 && !pt.akey.empty() && !pt.akey[0].aitem.empty())
                {
                    size_t e = pt.akey[0].aitem[0].aw[0];
                    // (a reference to a group: the Panda King's face part places group 4, its picture)
                    if (e < pso->aelem.size() && pso->aelem[e].kind == 4 && pso->aelem[e].ref < pso->agroup.size())
                        for (const SPART& q : pso->agroup[pso->aelem[e].ref].apart)
                            if (!q.akey.empty() && !q.akey[0].aitem.empty()) { e = q.akey[0].aitem[0].aw[0]; break; }
                    if (e < pso->aelem.size() && pso->aelem[e].kind == 1) return &pso->aelem[e];
                    // (else the gauge's always-drawn face: the largest picture of its own groups without parts)
                }
    }
    return nullptr;
}
bool FBox(const SELEM& el, glm::vec2* plo, glm::vec2* phi)
{
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const SPOLY& p : el.apoly)
        for (const auto& xy : p.axy)
        {
            const glm::vec2 v(float(xy[0]) / 16.0f, float(xy[1]) / 16.0f);
            lo = glm::min(lo, v), hi = glm::max(hi, v);
        }
    *plo = lo, *phi = hi;
    return lo.x < hi.x && lo.y < hi.y;
}
void DrawFitted(const SELEM& el, const SELEM& elTarget, const HUDSTATE& st, float fill)
{
    glm::vec2 loT, hiT, loE, hiE;
    if (!FBox(elTarget, &loT, &hiT) || !FBox(el, &loE, &hiE)) return;
    const float k = fill * std::min((hiT.x - loT.x) / (hiE.x - loE.x), (hiT.y - loT.y) / (hiE.y - loE.y));
    const glm::vec2 cT = 0.5f * (loT + hiT), cE = 0.5f * (loE + hiE);
    const glm::vec2 d = cT - k * cE;
    HUDSTATE s = st;
    s.m[0] = st.m[0] * k, s.m[1] = st.m[1] * k, s.m[4] = st.m[4] * k, s.m[5] = st.m[5] * k;
    s.m[8] = st.m[0] * d.x + st.m[4] * d.y + st.m[8];
    s.m[9] = st.m[1] * d.x + st.m[5] * d.y + st.m[9];
    g_hud.DrawSprite(el, s);
}
LO* PloViewGauge()
{
    if (!r.fActive || !r.fSetUp || g_iviewHud3 < 0 || g_iviewHud3 >= r.cplayer) return nullptr;
    return PloShipGauge3(r.ap[size_t(g_iviewHud3)].pship);
}
bool FScreenObjSkip(LO* plo)
{
    if (!r.fActive || !r.fSetUp || g_iviewHud3 < 0 || g_iviewHud3 >= r.cplayer || !plo || plo->Cid() != 0xA6) return false;
    return plo != PloViewGauge();
}
static const float kFaceScale = std::getenv("SLY3_GALLEONFACESCALE") ? float(std::atof(std::getenv("SLY3_GALLEONFACESCALE"))) : 2.0f;
constexpr int kElemGaugeOval = 11;   // (the ship gauge's face oval, the shown part's last piece: the portrait over it)
bool FHudPiece(int oid, int ielem, const SELEM& el, const HUDSTATE& st)
{
    if (static const bool s_f = std::getenv("SLY3_GALLEONHUDDBG") != nullptr; s_f && g_iviewHud3 >= 2)
    {
        static std::vector<long> s_aseen;
        const long k = long(oid) * 64 + ielem * 4 + g_iviewHud3;
        if (std::find(s_aseen.begin(), s_aseen.end(), k) == s_aseen.end())
        {
            s_aseen.push_back(k);
            std::printf("galleon3: view %d piece oid %d elem %d (gauge %d, face %p)\n", g_iviewHud3, oid, ielem, PloViewGauge() ? PloViewGauge()->oid : -1, (const void*)PelemCharFace(g_iviewHud3));
        }
    }
    if (ielem != kElemGaugeOval || !r.fActive || g_iviewHud3 < 2 || g_iviewHud3 >= r.cplayer) return false;
    LO* pg = PloViewGauge();
    if (!pg || pg->oid != oid) return false;
    const SELEM* pface = PelemCharFace(g_iviewHud3);
    g_hud.DrawSprite(el, st);
    if (pface)
    {
        HUDSTATE stFace = st;   // (the portrait's own colours: the oval's piece is drawn dimmed)
        stFace.scale = glm::vec4(kFaceScale, kFaceScale, kFaceScale, st.scale.a);
        stFace.offset = glm::vec4(0.0f, 0.0f, 0.0f, st.offset.a);
        stFace.depth = st.depth + 50.0f;
        DrawFitted(*pface, el, stFace, 0.8f);
    }
    return true;
}

// ---- player 4's helm: the Panda King's daughter Jing King steers hidden in a booth of Episode 4's palace screens around
// his wheel: four copies of c_hall_b's curtained screen 69964 (the room of Tsao in "Get a Job"), scaled to a little more
// than the Panda King's height, lit faces outward, and her silhouette on the front one (toward the main deck): the shadow
// hulls 69942 at their place relative to that screen there. Copies from that world kept (pcloader.h); notes/PLANE3
// "Round 9". SLY3_GALLEONWALLOID=73253: c_partition instead (the old single screen); SLY3_GALLEONSHADOWOID=0: no silhouette.
// (no copy has a rotation record (SetAloRotation does not turn them; her pieces hang off records of their own, which the
// SM's ASEGs animate): each is moved to its place as it stands in c_hall_b and drawn turned and scaled about that place:
// scenesw.h g_pfnPlaceDelta3)
static const float kWallScale = std::getenv("SLY3_GALLEONWALLSCALE") ? float(std::atof(std::getenv("SLY3_GALLEONWALLSCALE"))) : 0.62f;
bool FPlaceDelta(const ALO* palo, glm::mat4* pdmat)
{
    if (!r.fActive || !palo || !r.fWallPlaced) return false;
    if (palo == r.pshadow) { *pdmat = r.awall[0].mat; return true; }
    for (int i = 0; i < r.cwall; ++i)
        if (palo == r.awall[i].plo) { *pdmat = r.awall[i].mat; return true; }
    return false;
}
void PlaceWall()
{
    if (r.pwall || r.cplayer < 4 || !r.ap[3].pship || PckwsKeptWorld(kIwrecWall) != PCKWS_Ready) return;
    LO* ptmpl = PloKeptFind(kIwrecWall, kOidPaperWall, 0);
    if (!ptmpl) { r.pwall = r.ap[3].pship; std::printf("galleon3: no paper wall in the kept world\n"); return; }
    r.posWallKept = glm::vec3(sly3::MatLoWorld3(ptmpl)[3]);
    r.cwall = kOidPaperWall == 69964 ? 4 : 1;
    for (int i = 0; i < r.cwall; ++i) r.awall[i].plo = PloPcCloneIntoWorld(r.psw, ptmpl, true);   // (top level: placed each frame)
    r.pwall = r.awall[0].plo;
    std::printf("galleon3: paper wall %p (a copy of %d, cid 0x%X; %d screens)\n", (void*)r.pwall, ptmpl->oid, r.pwall ? unsigned(r.pwall->Cid()) : 0u, r.cwall);
    if (LO* psh = kOidJingShadow > 0 ? PloKeptFind(kIwrecWall, kOidJingShadow, 0) : nullptr)
    {
        r.pshadow = PloPcCloneIntoWorld(r.psw, psh, true);   // (top level too, beside the front screen as there)
        r.dposShadow = glm::vec3(sly3::MatLoWorld3(psh)[3]) - r.posWallKept;
        std::printf("galleon3: her silhouette %p (a copy of %d)\n", (void*)r.pshadow, psh->oid);
    }
}
void UpdateWall()
{
    ALO* pship = r.cplayer >= 4 ? dynamic_cast<ALO*>(r.ap[3].pship) : nullptr;
    LO* phelm = pship ? PloInTreeOf(pship, kOidHelm) : nullptr;
    if (!r.pwall || r.pwall == pship || !phelm) return;
    const glm::mat4 m = sly3::MatLoWorld3(pship);
    const glm::vec3 ax = glm::normalize(glm::vec3(m[0])), ay = glm::normalize(glm::vec3(m[1]));
    // (the booth's middle: the helmsman's place moved toward the wheel; its half side: the scaled screen's half width (c_hall_b's
    // screen is 511 wide, 572 tall: x 0.62 about 317 x 355, the Panda King ~280 tall); one screen alone (c_partition): 170
    // toward the main deck from the helm as before)
    static const float s_dx = std::getenv("SLY3_GALLEONWALLDX") ? float(std::atof(std::getenv("SLY3_GALLEONWALLDX"))) : r.cwall > 1 ? -60.0f : -170.0f;
    const float h = r.cwall > 1 ? 255.0f * kWallScale : 0.0f;
    const float scale = r.cwall > 1 ? kWallScale : 1.0f;
    // (the turn about z from its place there: c_hall_b's curtained screen shows its lit face (the hall's side, her
    // silhouette's) toward +y there: a quarter turn from the ship's heading puts it toward the ship's -x, the main deck)
    static const float s_rot = std::getenv("SLY3_GALLEONWALLROT") ? float(std::atof(std::getenv("SLY3_GALLEONWALLROT"))) : 1.5707964f;
    glm::vec3 c = PosLoWorld(phelm) + ax * s_dx;
    c.z = PosLoWorld(phelm).z;
    const float rad0 = std::atan2(ax.y, ax.x) + s_rot;
    // front (toward -x), back (+x), the sides (+y / -y): each faces outward
    const glm::vec3 an[4] = {-ax, ax, ay, -ay};
    const float arad[4] = {rad0, rad0 + 3.14159265f, rad0 - 1.5707964f, rad0 + 1.5707964f};
    for (int i = 0; i < r.cwall; ++i)
    {
        ALO* pw = dynamic_cast<ALO*>(r.awall[i].plo);
        if (!pw) continue;
        const glm::vec3 pos = c + an[i] * h;
        SetAloPos(pw, pos);
        if (i == 0)
            if (ALO* psh = dynamic_cast<ALO*>(r.pshadow)) SetAloPos(psh, pos + r.dposShadow);
        glm::mat4 rot(1.0f), tTo(1.0f), tFrom(1.0f);
        rot[0] = glm::vec4(std::cos(arad[i]) * scale, std::sin(arad[i]) * scale, 0.0f, 0.0f);
        rot[1] = glm::vec4(-std::sin(arad[i]) * scale, std::cos(arad[i]) * scale, 0.0f, 0.0f);
        rot[2] = glm::vec4(0.0f, 0.0f, scale, 0.0f);
        tTo[3] = glm::vec4(pos, 1.0f);
        tFrom[3] = glm::vec4(-pos, 1.0f);
        r.awall[i].mat = tTo * rot * tFrom;
        r.awall[i].pos = pos;
        r.awall[i].n = an[i];
    }
    r.fWallPlaced = true;
}

// a booth screen left out of a view it blocks: the view's own player hidden by it from its camera (the segment from the
// camera to the character crossing the screen's plane within its size), or the camera at the paper itself (player 4's
// camera behind him came behind the paper near the helm: the view all orange through it)
struct HIDDENWALL { SCENEGLOBSET* psgs; std::vector<uint8_t> af; };
std::vector<HIDDENWALL> s_ahidden;
bool FWallBlocksView(int iv, int iwall, const glm::vec3& posCam, float sRadius)
{
    ALO* pship = r.cplayer >= 4 ? dynamic_cast<ALO*>(r.ap[3].pship) : nullptr;
    if (!r.fWallPlaced || !pship || iv < 0 || iv >= r.cplayer || r.ap[size_t(iv)].fSpec || !r.ap[size_t(iv)].pchar) return false;
    const glm::vec3 n = r.awall[iwall].n;
    const glm::vec3 wc = r.awall[iwall].pos;
    const glm::vec3 posChar = PosLoWorld(r.ap[size_t(iv)].pchar) + glm::vec3(0.0f, 0.0f, 120.0f);
    const float d0 = glm::dot(posCam - wc, n), d1 = glm::dot(posChar - wc, n);
    auto fInside = [&](const glm::vec3& q) {
        const glm::vec3 v = q - wc;
        const glm::vec3 vPlane = v - n * glm::dot(v, n);
        return glm::length(glm::vec2(vPlane.x, vPlane.y)) < sRadius && v.z > -100.0f && v.z < 2.0f * sRadius;
    };
    if (std::fabs(d0) < 80.0f && fInside(posCam)) return true;
    if ((d0 < 0.0f) == (d1 < 0.0f) || std::fabs(d0 - d1) < 1e-3f) return false;
    return fInside(posCam + (posChar - posCam) * (d0 / (d0 - d1)));
}

struct INITGALLEON3
{
    INITGALLEON3()
    {
        PCWORLDHOOKS h;
        h.pfnStep = [](SW* psw, bool fDebugKeys) {
            if (s_fKeptWanted && !fDebugKeys && psw == r.psw && FPcKeptWorldsLoading()) PumpPcKeptWorlds(psw);
        };
        RegisterPcWorldHooks(h);
        split3::g_pfnHole2p3 = FHole2p;
        split3::g_pfnPadPilot3 = FPadPilot;
    }
} s_initgalleon3;
}  // namespace

int CPlayersChosen() { return CChosenRaw(); }
void StepPlayersChosen(int d) { s_cChosen = std::clamp(CChosenRaw() + d, 2, 4); }
bool FGalleonNode(const LO* pnode) { return pnode && pnode->oid == 2027; }
bool FActive() { return r.fActive; }
int CPlayers() { return r.fActive ? r.cplayer : 0; }
bool FRoundWorld(const SW* psw)
{
    return psw && psw->filter == kWorld && FChallengeRunning3() && OidChallengeJob3() == kJob && CChosenRaw() > 2;
}
void AdjustGroupInstances(SW* psw)
{
    if (!FRoundWorld(psw)) return;
    for (WE& we : psw->awe)
    {
        const int old = we.cinst;
        if (we.x10 == kShip) we.cinst = std::max(we.cinst, CChosenRaw());
        else if (we.x10 == kOidGuru || (we.x10 == kOidPandaKing && CChosenRaw() >= 4)) we.cinst = std::max(we.cinst, 1);
        if (we.cinst != old) std::printf("galleon3: group %d (x10 %d) instances %d -> %d\n", int(&we - psw->awe.data()), we.x10, old, we.cinst);
    }
}
void OnWorldLoaded(SW* psw)
{
    r = ROUND4{};
    if (!psw || psw->filter != kWorld || !FChallengeRunning3() || OidChallengeJob3() != kJob) return;
    if (std::getenv("SLY3_GALLEONDUMP")) DumpWorld(psw);
    if (CChosenRaw() <= 2) return;
    r.fActive = true;
    r.psw = psw;
    r.cplayer = CChosenRaw();
    if (r.cplayer >= 4) { RequestPcKeptWorld(kIwrecWall); s_fKeptWanted = true; }   // (the paper wall, pcloader.h)
    g_pfnHudPiece3 = FHudPiece;   // (only a round answers: these test r.fActive)
    g_pfnPlaceDelta3 = FPlaceDelta;
    g_pfnScreenObjSkip3 = FScreenObjSkip;
    std::printf("galleon3: free-for-all, %d players\n", r.cplayer);
}
int CViewsWanted() { return r.fActive ? r.cplayer : 0; }
void BeginViewScene(SCENE& scene, const std::vector<ALO*>& apalo, int iview, const glm::vec3& posCam)
{
    s_ahidden.clear();
    if (!r.fActive || !r.fSetUp || !r.pwall || !r.fWallPlaced) return;
    // (her silhouette only for the views in front of the front screen: her hulls reach from just before its paper to far
    // behind it, through the booth: a camera behind it (player 4's) would be inside them)
    const float sRadius = (r.cwall > 1 ? kWallScale : 1.0f) * 300.0f;
    bool afBlocks[4] = {};
    int cBlocks = 0;
    for (int i = 0; i < r.cwall; ++i) cBlocks += afBlocks[i] = FWallBlocksView(iview, i, posCam, sRadius);
    const bool fBehind = glm::dot(posCam - r.awall[0].pos, r.awall[0].n) < 80.0f * (r.cwall > 1 ? kWallScale : 1.0f);
    if (!cBlocks && !fBehind) return;
    for (size_t k = 0; k < scene.aglobset.size() && k < apalo.size(); ++k)
    {
        const LO* plo = apalo[k];
        bool fHide = plo && plo == r.pshadow && (fBehind || afBlocks[0]);
        for (int i = 0; i < r.cwall && !fHide; ++i) fHide = afBlocks[i] && plo == r.awall[i].plo;
        if (!fHide) continue;
        SCENEGLOBSET& sgs = scene.aglobset[k];
        s_ahidden.push_back({&sgs, sgs.afVisible});
        std::fill(sgs.afVisible.begin(), sgs.afVisible.end(), uint8_t(0));
    }
    if (static int s_c = 0; FPrint() && cBlocks && s_c++ % 120 == 0) std::printf("galleon3: %d booth screen(s) left out of view %d\n", cBlocks, iview + 1);
}
void EndViewScene()
{
    for (HIDDENWALL& h : s_ahidden) h.psgs->afVisible = std::move(h.af);
    s_ahidden.clear();
}
const CM* PcmSpectator(int iview)
{
    if (!r.fActive || !r.fSetUp || iview < 0 || iview >= r.cplayer || !r.ap[size_t(iview)].fSpec) return nullptr;
    return &r.ap[size_t(iview)].cmSpec;
}
void OnPlayersChosen(SW* psw)
{
    if (std::getenv("SLY3_GALLEONDUMP") && FDuelRunning()) DumpWorld(psw);
    if (!r.fActive) return;
    for (int i = 0; i < r.cplayer; ++i) r.ap[size_t(i)].pchar = split3::PloPlayer(psw, i);
}
LO* PloPlayerWanted(SW* psw, int i)
{
    if (!r.fActive || i <= 0 || i >= r.cplayer) return nullptr;
    static const int s_aoid[4] = {24, 25, kOidGuru, kOidPandaKing};
    return PloFindSwObjectByOid(psw, s_aoid[size_t(i)]);
}
// a seat's character in the duel (the seat's object is its ship while it sails: the ship's character then)
LO* PloOwnerOfSeat(SW* psw, int i)
{
    LO* plo = split3::PloPlayer(psw, i);
    if (plo && FShip3(plo)) return dynamic_cast<LO*>(RefOpt(plo, 1755).m_pbasic);
    return plo;
}
// a seat's character moved at once (a round's restore puts it back on its ship's deck): its view's camera cut to it there
// (eased, the camera went through the hull: player 2's view inside it for about a second after the original's restore)
void CutViewsOnJumps(SW* psw)
{
    static std::array<glm::vec3, 4> s_apos{};
    static std::array<const LO*, 4> s_aplo{};
    for (int i = 0; i < split3::CView() && i < 4; ++i)
    {
        LO* pchar = PloOwnerOfSeat(psw, i);
        if (!pchar) { s_aplo[size_t(i)] = nullptr; continue; }
        const glm::vec3 pos = PosLoWorld(pchar);
        if (s_aplo[size_t(i)] == pchar && glm::length(pos - s_apos[size_t(i)]) > 600.0f && !PcmSpectator(i))
        {
            CutCm(const_cast<CM*>(&split3::CmView(i)));
            if (FPrint()) std::printf("galleon3: player %d's character moved %.0f at once: its view's camera cut (t %.2f)\n", i + 1, glm::length(pos - s_apos[size_t(i)]), g_clock.t);
        }
        s_aplo[size_t(i)] = pchar;
        s_apos[size_t(i)] = pos;
    }
}
void Update(SW* psw)
{
    // (both options) the players other than Sly use their own ships' deck cannons by Circle (ship3.h FUseDeckCannon3)
    if (FDuelRunning() && psw && split3::FActive())
        for (int i = 1; i < split3::CView(); ++i)
            if (LO* pchar = PloOwnerOfSeat(psw, i)) FUseDeckCannon3(pchar);
    if (FDuelRunning() && psw && split3::FActive()) CutViewsOnJumps(psw);
    if (!r.fActive || r.fEnded || !psw || psw != r.psw) return;
    const float t = g_clock.t;
    // (the splice's own start (s1621, then the first round's battle of its two ships): players 3 / 4 join, every ship's round)
    if (!r.fSetUp)
    {
        if (NShipBattleState3() > 0) SetUp();
        return;
    }
    if (r.winner >= 0)
    {
        if (t - r.tWin > kWinHold)
        {
            r.fEnded = true;
            std::printf("galleon3: duel over, back to the chooser\n");
            EndChallenge3(psw);
        }
        return;
    }
    // a ship gone down without the broadcast (its character's fall: the splice counts a death as the ship lost); a
    // character in the sea (below the deck's height 2 s on end: the Guru's / the Panda King's drowning sends no death)
    for (int i = 0; i < r.cplayer; ++i)
    {
        PLAYER4& p = r.ap[size_t(i)];
        if (p.fOut || !p.pship || t - r.tRoundStart < 3.0f) continue;
        if (NShipMode3(p.pship) == 7) { Out(i); continue; }
        if (p.pchar && PosLoWorld(p.pchar).z < PosLoWorld(p.pship).z + 300.0f)
        {
            if (p.tOverboard < 0.0f) p.tOverboard = t;
            else if (t - p.tOverboard > 2.0f)
            {
                std::printf("galleon3: player %d overboard (t %.2f)\n", i + 1, t);
                SinkShip3(p.pship);
                Out(i);
            }
        }
        else p.tOverboard = -1.0f;
    }
    if (r.tRoundEnd >= 0.0f && t >= r.tRoundEnd) EndRound();
    UpdateSpectators();
    PlaceWall();
    UpdateWall();
    if (static bool s_fDone = std::getenv("SLY3_GALLEONINTDBG") == nullptr; !s_fDone && t > 5.0f)
    {
        s_fDone = true;
        for (LO* plo : psw->mpcidaplo[0x11])
        {
            const LO* root = plo;
            while (root->paloParent) root = root->paloParent;
            if (root->oid != kShip) continue;
            int grp = -1;
            for (const OPT& o : plo->aopt) if (o.eopid == 252 && !o.aval.empty()) grp = o.aval[0].n;
            std::printf("galleon3: interest %d (inst %d) root %d player %d group %d in world %d\n", plo->oid, plo->oidInstance, root->oid, IplayerOfShip(root) + 1, grp, int(plo->fInWorld));
        }
    }
    // the players' characters on their own ships' decks (the battle script 44240 parents the character to the deck spot
    // 44236 it finds by its id for 1.4 s: the Panda King was put on another ship's deck and stayed there)
    for (int i = 0; i < r.cplayer; ++i)
    {
        PLAYER4& p = r.ap[size_t(i)];
        if (!p.pchar || !p.pship || !p.pchar->paloParent) continue;
        const LO* root = p.pchar->paloParent;
        while (root->paloParent) root = root->paloParent;
        if (root == p.pship || !FShip3(const_cast<LO*>(root))) continue;
        if (LO* pspot = PloInTreeOf(p.pship, kOidDeckSpot))
        {
            CallOpt(p.pchar, 39, {RefLo(pspot)});
            CallOpt(p.pchar, 35, {RefLo(pspot), RefN(0)});
            std::printf("galleon3: player %d's character to its own deck (parent now %d) (t %.2f)\n", i + 1, p.pchar->paloParent ? p.pchar->paloParent->oid : -1, t);
        }
    }
    // the helmsmen at their own ships' helms (the battle script 44240 parents a helmsman to the helm 44234 it finds by its
    // id: with four copies of the ship Penelope went to player 4's, Dimitri to none): Murray at player 1's, Penelope at
    // player 2's, Dimitri at player 3's; player 4's helm is the Panda King's daughter's, behind the paper wall
    if (t - r.tRoundStart > 1.6f)
    {
        static const int s_aoid[3] = {kOidMurray, kOidPenelope, kOidDimitri};
        for (int i = 0; i < 3 && i < r.cplayer; ++i)
        {
            LO* ph = PloFindSwObjectByOid(psw, s_aoid[i]);
            LO* pship = r.ap[size_t(i)].pship;
            if (!ph || !pship) continue;
            LO* phelm = PloInTreeOf(pship, kOidHelm);
            if (!phelm || ph->paloParent == phelm) continue;
            if (!ph->fInWorld) CallOpt(ph, 41);
            CallOpt(ph, 39, {RefLo(phelm)});
            CallOpt(ph, 35, {RefLo(phelm), RefN(0)});
            std::printf("galleon3: helmsman %d to player %d's helm (parent now %d) (t %.2f)\n", ph->oid, i + 1, ph->paloParent ? ph->paloParent->oid : -1, t);
        }
    }
    if (static const bool s_f = std::getenv("SLY3_GALLEONHELMDBG") != nullptr; s_f && int(t * 60.0f) % 300 == 0)
        for (int oid : {26, 31, kOidDimitri, 24, 25, kOidGuru, kOidPandaKing})
            if (LO* p = PloFindSwObjectByOid(psw, oid))
            {
                const LO* root = p;
                while (root->paloParent) root = root->paloParent;
                std::printf("galleon3: t %.1f %d at %.0f %.0f %.0f in world %d parent %d root %d/%d\n", t, oid, PosLoWorld(p).x, PosLoWorld(p).y, PosLoWorld(p).z, int(p->fInWorld),
                            p->paloParent ? p->paloParent->oid : -1, root->oid, root->iclone);
            }
}
bool FOnBroadcast(int code, LO* plo)
{
    if (!r.fActive || !r.fSetUp || r.fEnded || (code != 0xC && code != 10)) return false;
    // (a helmsman's death (Murray / Penelope / Dimitri drowning with a sunk ship): the splice counted it as its player's ship
    // lost: with both of its ships down it scored the board and restored its two ships in the middle of a free-for-all
    // round (both views faded out, the board's scores in the views, the other ships' players then overboard))
    if (plo && (plo->oid == kOidMurray || plo->oid == kOidPenelope || plo->oid == kOidDimitri))
    {
        if (FPrint()) std::printf("galleon3: broadcast 0x%X of helmsman %d kept from the splice (t %.2f)\n", code, plo->oid, g_clock.t);
        return true;
    }
    const int i = IplayerOfShip(plo);
    if (i < 0) return false;
    // (not the duel splice's: its scoring knows two ships; its sinking camera (10) neither)
    if (code == 0xC)
    {
        if (plo == r.ap[size_t(i)].pchar && r.ap[size_t(i)].pship) SinkShip3(r.ap[size_t(i)].pship);   // (a character's death: its ship lost)
        Out(i);
    }
    return true;
}
uint32_t RgbaPlayer(int i)
{
    // Sly blue, Bentley green, the Guru orange, the Panda King red (0xAABBGGRR, GS units)
    static const uint32_t s_argba[4] = {0x80803818u, 0x80207828u, 0x80105880u, 0x80141880u};
    return s_argba[size_t(std::clamp(i, 0, 3))];
}
bool FTagOffset(int i, glm::vec4* poff)
{
    if (!r.fActive || i < 0 || i >= r.cplayer) return false;
    const uint32_t c = RgbaPlayer(i);
    const glm::vec4 v(float(c & 0xFF), float((c >> 8) & 0xFF), float((c >> 16) & 0xFF), 0.0f);
    const float k = 110.0f / std::max({v.r, v.g, v.b, 1.0f});
    *poff = glm::vec4(v.r * k, v.g * k, v.b * k, 0.0f);
    return true;
}
bool FDescription(const LO* pnode, std::string* pstr)
{
    if (!FGalleonNode(pnode) || CChosenRaw() <= 2) return false;
    *pstr = StrPort(PTX_GalleonDescFFA);
    return true;
}

void DrawOverlay(int width, int height, float xLeft, float xRight, float yTop, float yBottom)
{
    if (!r.fActive || !r.fSetUp || !split3::FActive()) return;
    auto xCanvas = [&](float x) { return xLeft + (xRight - xLeft) * x / float(std::max(width, 1)); };
    auto yCanvas = [&](float y) { return yTop + (yBottom - yTop) * (1.0f - y / float(std::max(height, 1))); };
    const float t = g_clock.t;
    for (int iv = 0; iv < split3::CView() && iv < r.cplayer; ++iv)
    {
        const split3::VIEWRECT rv = split3::RectView(iv, width, height);
        split3::VIEWRECT rs{0, 0, rv.dx, rv.dy};
        split3::FRectScene(iv, rv.dx, rv.dy, &rs);
        const float x0 = xCanvas(float(rv.x + rs.x)), x1 = xCanvas(float(rv.x + rs.x + rs.dx));
        const float y0 = yCanvas(float(rv.y + rs.y + rs.dy)), y1 = yCanvas(float(rv.y + rs.y));
        const float xc = 0.5f * (x0 + x1);
        // the other players' ships: a marker in their colour with their number (an arrow on the view's edge when outside)
        const CAMERA cam = CameraFromCm(split3::CmView(iv));
        const glm::mat4 m = cam.MatWorldToClip(float(rs.dx) / float(std::max(rs.dy, 1)));
        for (int j = 0; j < r.cplayer; ++j)
        {
            const PLAYER4& o = r.ap[size_t(j)];
            if (j == iv || !o.pship || o.fOut || !o.pship->fInWorld) continue;
            const glm::vec4 c = m * glm::vec4(PosLoWorld(o.pship) + glm::vec3(0.0f, 0.0f, 2500.0f), 1.0f);
            glm::vec2 ndc = c.w > 1.0f ? glm::vec2(c.x, c.y) / c.w : glm::vec2(c.x, c.y) * (c.w < 0.0f ? -1.0f : 1.0f);
            const glm::vec4 rgb = RgbOfPlayer(j);
            if (c.w > 1.0f && std::fabs(ndc.x) < 0.95f && std::fabs(ndc.y) < 0.9f)
            {
                const float x = x0 + (ndc.x * 0.5f + 0.5f) * (x1 - x0), y = y1 - (ndc.y * 0.5f + 0.5f) * (y1 - y0);
                Arrow(x, y + 4.0f, 1.5708f, 7.0f, Rgb(0, 0, 0, 110), 41.0f);
                Arrow(x, y + 4.0f, 1.5708f, 5.5f, rgb, 42.0f);
                Text(std::to_string(j + 1), x, y - 16.0f, 12.0f, OffOf(j));
                continue;
            }
            if (std::fabs(ndc.x) < 1e-4f && std::fabs(ndc.y) < 1e-4f) ndc.y = -1.0f;
            const float k = std::min(0.9f / std::max(std::fabs(ndc.x), 1e-4f), 0.8f / std::max(std::fabs(ndc.y), 1e-4f));
            const glm::vec2 e = ndc * k;
            const float x = 0.5f * (x0 + x1) + e.x * 0.5f * (x1 - x0), y = 0.5f * (y0 + y1) - e.y * 0.5f * (y1 - y0);
            const float ang = std::atan2(-e.y * (y1 - y0), e.x * (x1 - x0));
            Arrow(x, y, ang, 7.0f, Rgb(0, 0, 0, 110), 41.0f);
            Arrow(x, y, ang, 5.5f, rgb, 42.0f);
        }
        if (t - r.tMsg < kMsg || r.winner >= 0)
        {
            const float size = r.winner >= 0 ? 26.0f : 13.0f;
            const float yc = r.winner >= 0 ? 0.5f * (y0 + y1) - size : y0 + 6.0f;
            const float wm = std::min(x1 - x0 - 8.0f, r.winner >= 0 ? 300.0f : 260.0f);
            Rect(xc - 0.5f * wm, yc - 4.0f, xc + 0.5f * wm, yc + size * 1.3f, Rgb(0, 0, 0, 90), 30.0f);
            if (r.iMsg >= 0) Rect(xc - 0.5f * wm, yc + size * 1.3f - 2.0f, xc + 0.5f * wm, yc + size * 1.3f, RgbOfPlayer(r.iMsg), 31.0f);
            Text(r.msg, xc, yc, size, r.iMsg >= 0 ? OffOf(r.iMsg) : glm::vec4(110, 110, 110, 0));
        }
    }
    // the rounds won by every player, once, in the middle of the picture (a ship crossed out while it is down this round)
    const float k = float(width) > 2.5f * float(std::max(height, 1)) ? 0.7f : 1.0f;
    const float xc = 0.5f * (xLeft + xRight), yc = 0.5f * (yTop + yBottom);
    const float dx = 30.0f * k, w = dx * float(r.cplayer);
    Rect(xc - 0.5f * w - 4.0f * k, yc - 12.0f * k, xc + 0.5f * w + 4.0f * k, yc + 11.0f * k, Rgb(0, 0, 0, 100), 30.0f);
    for (int j = 0; j < r.cplayer; ++j)
    {
        const float xj = xc - 0.5f * w + dx * (float(j) + 0.5f);
        Rect(xj - 13.0f * k, yc + 9.0f * k, xj + 13.0f * k, yc + 11.0f * k, RgbOfPlayer(j, r.ap[size_t(j)].fOut ? 50 : 128), 31.0f);
        Text(std::to_string(r.ap[size_t(j)].points), xj, yc - 11.0f * k, 17.0f * k, r.ap[size_t(j)].fOut ? glm::vec4(40, 40, 40, 0) : OffOf(j));
    }
}

void DrawMenu(int width, int height)
{
    const LO* pnode = PnodeTwoPlayerShown3();
    if (!FGalleonNode(pnode)) return;
    g_hud.Begin();
    // (the episode menu's right column, as the Biplane Duel's players row: duel3.cpp)
    const int c = CChosenRaw();
    const float xL = 255.0f, xR = 608.0f, y = 262.0f;
    Rect(xL, y, xR, y + 46.0f, Rgb(0, 4, 24, 110), -10.0f);
    Rect(xL, y + 44.0f, xR, y + 46.0f, Rgb(20, 40, 90, 128), -9.0f);
    Text(StrPort(PTX_DuelPlayers), xL + 8.0f, y + 5.0f, 15.0f, glm::vec4(70, 90, 128, 0), 0);
    Text("&2+&.", xL + 8.0f, y + 24.0f, 15.0f, glm::vec4(70, 90, 128, 0), 0);
    const float xc = 0.5f * (xL + xR) + 30.0f;
    if (c > 2) Arrow(xc - 118.0f, y + 15.0f, 3.14159f, 7.0f, Rgb(126, 104, 10), -8.0f);
    if (c < 4) Arrow(xc + 118.0f, y + 15.0f, 0.0f, 7.0f, Rgb(126, 104, 10), -8.0f);
    char ach[64];
    std::snprintf(ach, sizeof ach, StrPort(PTX_DuelNPlayers).c_str(), c);
    Text(ach, xc, y + 3.0f, 22.0f, glm::vec4(128, 106, 10, 0));
    Text(StrPort(c == 2 ? PTX_DuelOriginal : PTX_GalleonFreeForAll), xc, y + 29.0f, 11.0f, glm::vec4(80, 80, 80, 0));
    g_hud.Flush(width, height);
}

bool FDuelRunning() { return g_psw && g_psw->filter == kWorld && FChallengeRunning3() && OidChallengeJob3() == kJob; }
}  // namespace galleon3
