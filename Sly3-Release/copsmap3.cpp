// copsmap3.cpp: Cops and Robbers on the other hubs (copsmap3.h, notes/SPLIT3_2026-11-12.md "Round 6").
#include "copsmap3.h"

#include "activate.h"
#include "actor.h"
#include "alo.h"
#include "glob.h"
#include "ai.h"
#include "aicarm3.h"
#include "challenge3.h"
#include "clock.h"
#include "cls.h"
#include "cmworld.h"
#include "damage.h"
#include "display.h"
#include "epmenu3.h"
#include "eval.h"
#include "frame.h"
#include "gadget.h"
#include "guard.h"
#include "hud.h"
#include "lo.h"
#include "porttext.h"
#include "render.h"
#include "routetele.h"
#include "screenrt.h"
#include "screen3.h"
#include "pcloader.h"
#include "po.h"
#include "splitscreen3.h"
#include "so.h"
#include "sound.h"
#include "splayer.h"
#include "step.h"
#include "sw.h"
#include "menu.h"
#include "frontend.h"
#include "transition.h"
#include "game/items3.h"
#include "objmethod.h"
#include "game/sly3.h"
#include "game/task3.h"
#include "game/scptgame3.h"
#include "game/scptrun3.h"
#include "game/trigger3.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

void RestoreActorDamage3(ACTOR* pa);   // damage.cpp (option 948: health full, no flash, the reaction left)

extern int g_iframeMain;   // (game.cpp: debug prints)

extern bool g_fSpawnersPaused;   // spawner.cpp

int GiveShipPowerup3(LO* plo);    // game/ship3.cpp (the ships' vt slot 90)
int GivePlanePowerup3(LO* plo);   // game/biplane3.cpp (the planes' vt slot 90)

namespace cops3
{
namespace
{
struct SITE { float x, y, z; };
struct HUB
{
    int iep = 0;
    int iworld = 0;
    // spawns: [0] / [1] the robbers' and the cops' first places (far apart); a respawn takes the one farthest from
    // the other side's players
    std::vector<SITE> aspawn;
    std::vector<SITE> aloot;     // loot stands (three hold a loot at a time)
    std::vector<SITE> asafe;     // safe points (deliveries)
    std::vector<SITE> apower;    // power-up spots
    float zOut = -1e9f;          // a player below it is out of the round's area (default: 2500 below the lowest site)
};

// The hubs (positions from the hubs' own checkpoints (class 0x28: places the game puts Sly back on, so on solid
// ground) and spires (class 0x25: the tops Sly lands on), SLY3_COPSDUMP=1, picked on overhead pictures of each hub).
const std::vector<HUB>& Hubs()
{
    static const std::vector<HUB> s_ahub = {
        // Venice: the original game (v_ext, job 2409's own rules)
        {sly3::EP_Venice, 3, {}, {}, {}, {}},
        // Outback (o_ext): the canyon camp. Robbers start by the tents in the lower camp, the cops on the upper
        // plateau by the mine buildings; loot on the spire tops (the conveyor tower, the rock in the middle, the
        // upper-left lookout, the camp's tent pole, the ramp on the right), the crane pit's rim and the mine's roof;
        // safe points at the arena's bottom, the north mesa, the far west ledge and the east cliff path.
        {sly3::EP_Outback, 8,
         {{-2311, -6908, 1227}, {5681, 3905, 2261}, {2930, -7200, 1624}, {-355, 5793, 2371}, {3202, 101, 658}},
         {{1819, 2868, 2461}, {-61, 864, 1866}, {-3880, 3172, 2427}, {1376, -5838, 1122}, {310, -2043, 2200},
          {6676, 2079, 1564}, {6510, 4468, 3005}},
         {{1906, -1967, 745}, {3398, 7511, 1410}, {-6001, -6281, 1495}, {9733, -1047, 1119}},
         {{546, -1750, 2170}, {4579, 3690, 2380}, {342, -6725, 1242}, {-6514, 3059, 1796}}},
        // Holland (h_ext): the town on the canal, the windmill hills and the airfield. Robbers start in the town, the
        // cops on the airfield's hill; loot on the town square's tower, the north town's roof, the windmills by the
        // airfield, the hangar, the west bridge and the town's canal houses; safe points at the canal dock, the
        // south shore, the north-west island and the west canal.
        {sly3::EP_Holland, 15,
         {{9840, -206, 1200}, {1023, -4355, 1860}, {2536, 4485, 938}, {-2238, -631, -307}, {3733, 995, 574}},
         {{7453, -215, 1575}, {7709, 5185, 1575}, {1303, -4496, 2453}, {-2610, -4457, 2281}, {-2879, -652, 1834},
          {8551, 3048, 1144}, {1490, 5449, 502}},
         {{10326, 1977, 145}, {3109, -8958, 1399}, {-5226, 5318, 1429}, {-5111, 1833, 209}},
         {{3733, 995, 574}, {-17, -10288, 518}, {6704, -2047, 1575}, {46, -1600, -185}}},
        // China (c_ext): the temple ring over the clouds. Robbers start on the west terraces, the cops on the east
        // bridge; loot on the temple's roof and steps, the south gate's spire, the north courtyard, the north-west and
        // north-east roofs and the north pagoda; safe points at the far west cliff, the far east tower, the north
        // terrace and the south stairs.
        {sly3::EP_China, 23,
         {{-7538, 180, 2490}, {5514, 113, 2338}, {-3272, 7945, 2992}, {5460, 5416, 3639}, {1657, -850, 2595}},
         {{977, 383, 3099}, {996, 2048, 4700}, {777, -2135, 3367}, {1741, 5085, 3429}, {-2732, 5510, 3429},
          {7775, 6254, 3429}, {-54, 8226, 6414}},
         {{-8316, -1100, 2066}, {10581, 6642, 3964}, {3004, 7422, 5535}, {1104, -3058, 1854}},
         {{994, 761, 3091}, {-6810, 6048, 2388}, {10078, 7639, 3852}, {-5544, 8158, 2684}}},
        // Pirate (p_ext): the harbour town. No Carmelita of its own: v_ext's (the kept world) is copied in. Sites from the
        // hub's checkpoints and spires in the town's area (SLY3_COPSDUMP=1, spread farthest apart): robbers start on the
        // north pier, the cops by the south docks; loot on the spire tops (the lighthouse side, the tavern, the masts),
        // safe points at the south-west quay, the market, the east terrace and the square.
        {sly3::EP_Pirate, 31,
         {{-6296, 11718, 823}, {1288, -3836, 107}, {-607, 4845, 1612}, {-5439, -732, -39}, {5790, 2378, 2013}},
         {{-11368, 1662, 4963}, {2947, 10008, 2709}, {-9037, -2818, 5007}, {-2436, 8504, 2406}, {-7539, 9155, 3652},
          {2880, -51, 1049}, {52, 6694, 4288}},
         {{-3391, -2817, 228}, {-5070, 4600, 1296}, {2106, 3765, 3096}, {-2852, 3444, 1896}},
         {{-3155, -5333, 228}, {-4221, 5892, 4041}, {3188, 6377, 2394}, {-5119, 2837, 3419}}},
        // Kaine Island (m_ext): no Carmelita of its own (v_ext's copied in, as Pirate's). Its landmarks lie on cliffs and
        // ledges up to 8000 units apart in height, so the sites are hand-picked from walks of the island in a round
        // (notes/SPLIT3 "Round 10"): the west shore below the fortress wall (the beach, the rocks and the ruins between
        // the lighthouse cliffs), the places Sly stood on in four random walks there, all in one region the walks went
        // both ways between on foot (no one-way drop, not the field above it, not in the water). Robbers start at the
        // west rocks, the cops by the east ruins (back from the cliff: a copied Carmelita has no water of her own); loot on the rocks, the ruins' steps and the shore; safe points at the
        // north beach, the east ruins, the south rocks and the middle; power-ups between them.
        {sly3::EP_Isle, 35,
         {{-7730, 2477, -2719}, {-4985, 2585, -2386}, {-7378, 2207, -2740}, {-6293, 3717, -3075}, {-5824, 2569, -2629}},
         {{-6190, 2191, -2726}, {-7370, 1521, -2882}, {-5588, 3007, -2478}, {-7350, 3322, -2652}, {-6640, 2926, -2429},
          {-6188, 1425, -2805}, {-5529, 3766, -3168}},
         {{-6977, 3761, -2925}, {-4645, 2954, -2430}, {-6952, 1413, -2949}, {-5419, 2188, -2375}},
         {{-6985, 2266, -2797}, {-6551, 1816, -2831}, {-6138, 3013, -2719}, {-6973, 1825, -2871}},
         // (the sea at ~-4200: a copied Sly in it has no safe ground to be put back on, its hits go on)
         -3900.0f},
    };
    return s_ahub;
}

constexpr int kWin = 5;
constexpr int kLootsOut = 3;
constexpr float kTakeRadius = 240.0f, kSafeRadius = 320.0f, kPowerRadius = 220.0f, kDz = 300.0f;
constexpr float kFallBelow = 2500.0f;   // (below the hub's lowest site: out of the round's area)
constexpr float kLootBack = 6.0f, kPowerEmpty = -1.0f, kSprint = 12.0f, kWinHold = 5.0f, kMsg = 3.0f;

enum TEAM : int { TEAM_Sly, TEAM_Carm };

struct PLAYER
{
    LO* plo = nullptr;
    int team = TEAM_Sly;
    bool fCarry = false;
    bool fDown = false;        // (a robber caught / a cop knocked out: scored once until placed again)
    float tSprintEnd = 0.0f;
};

struct ROUND
{
    int ihubChosen = 0;
    bool fChosenInit = false;
    int ihubPending = -1;      // (the round's world loading)
    bool fActive = false;
    int ihub = -1;
    SW* psw = nullptr;
    int cplayer = 0;
    std::array<PLAYER, 4> ap{};
    std::array<int, 2> ascore{};
    std::vector<char> afLoot;      // per stand: holds a loot
    std::vector<float> atLoot;     // per stand: a new loot appears then (0: none pending)
    std::vector<float> atPower;    // per spot: 0 a power-up there, kPowerEmpty none
    int winner = -1;
    float tWin = 0.0f;
    float tStart = 0.0f;
    std::string msg;
    float tMsg = -100.0f;
    int teamMsg = -1;
    bool fEnded = false;
    unsigned rng = 12345u;
    // the original's presentation, copied from v_ext (kept world 3 with Cops and Robbers' option block): the 2P HUD
    // 3325, the score board 140111 (null: the kept world failed, the native fallback draws its own)
    LO* pso2p = nullptr;
    LO* pboard = nullptr;
    // v_ext's booty stands (159608), safe points (159616) and loot (BOOTY 9991) copied onto the hub's sites
    std::vector<LO*> aploStand, aploSafe, aploLoot, aploPower;
    bool fResult = false;      // (the job complete stamp shown at the end)
    float tResult = 0.0f;      // (when it was shown: the stamp waits for Triangle / Square, as jobend3's two-player end)
    // script 2414's order after a point: the scorer's line, the board half a second later, then (by the score) the
    // commentator's line once the board is gone; at 5 the winner's line, then the stamp and the chooser
    float tBoardShow = 0.0f;   // (the board pending: shown then; 0 none)
    int teamBoard = -1;
    bool fBoardUp = false;     // (shown: its after-line when it is done)
    float tBoardUp = 0.0f;
    int idEndLine = -1;        // (the winner's line playing: the end after it)
    float tEndLine = 0.0f;
    int teamStreak = -1, cStreak = 0;   // (points in a row: "Three in a row for Sly!", "Four in a row!")
} r;

bool FPrint() { static const bool s_f = std::getenv("SLY3_COPSPRINT") != nullptr; return s_f; }

constexpr int kIwrecVenice = 3;
constexpr OID kOidHud2p = 3325, kOidBoard = 140111, kOidResult = 1066, kOidJobCops = 2409;
constexpr OID kOidStand = 159608, kOidSafe = 159616, kOidLoot = 9991;
constexpr OID kOidPower = 159593;   // (Venice's power-up: a breakable with its glow, at the proxies 140842..140847)
// the board's lines (the arguments script 2414 shows it with): Carmelita scores / wins, Sly scores / wins
constexpr int kTxtSlyWinner = 5255, kTxtCarmWinner = 5254;   // (the end's lines: the stamp's title, script string 1110)
constexpr int kTxtCarmScores = 5354, kTxtCarmWins = 5355, kTxtSlyScores = 5356, kTxtSlyWins = 5357;
// the dialog lines script 2414 plays (the round's intro 2412: 4858 "Go for it!"): a point (Sly 4872, Carmelita 4879),
// after the board three in a row (Sly 4807, Carmelita 4805) and four (4809), the winner (Sly 4833, Carmelita 4837)
constexpr int kDlgIntro = 4858, kDlgSlyScores = 4872, kDlgCarmScores = 4879, kDlgSly3 = 4807, kDlgCarm3 = 4805, kDlg4 = 4809,
              kDlgSlyWins = 4833, kDlgCarmWins = 4837;

// (the subtitles: the running world's text, else v_ext's while a round runs)
std::string (*g_pfnTextOld)(int) = nullptr;
std::string StrLineText(int id)
{
    std::string str = g_pfnTextOld ? g_pfnTextOld(id) : std::string();
    if (str.empty() && r.fActive)
        if (const SW* pk = PswKeptWorld(kIwrecVenice)) str = StrLocalized(pk, id);
    return str;
}

// a copy of v_ext's object in the round's world (in it: shown by the round), null when the kept world is not there
LO* PloFromVenice(SW* psw, OID oid)
{
    LO* ptmpl = PloKeptFind(kIwrecVenice, oid);
    if (!ptmpl) return nullptr;
    LO* plo = PloPcCloneIntoWorld(psw, ptmpl, true);
    if (FPrint()) std::printf("cops3: v_ext's %d copied: %s\n", oid, plo ? "ok" : "failed");
    return plo;
}


unsigned Rand() { r.rng = r.rng * 1103515245u + 12345u; return (r.rng >> 16) & 0x7FFF; }

glm::vec3 V(const SITE& s) { return glm::vec3(s.x, s.y, s.z); }
const HUB* PhubCur() { return r.ihub > 0 && r.ihub < int(Hubs().size()) ? &Hubs()[size_t(r.ihub)] : nullptr; }

bool FNear(const glm::vec3& a, const glm::vec3& b, float rad)
{
    return glm::length(glm::vec2(a) - glm::vec2(b)) < rad && std::fabs(a.z - b.z) < kDz;
}

void Message(PTX ptx, int team)
{
    r.msg = StrPort(ptx);
    r.tMsg = g_clock.t;
    r.teamMsg = team;
    if (FPrint()) std::printf("cops3: message %d (t %.1f, %d : %d)\n", int(ptx), g_clock.t, r.ascore[0], r.ascore[1]);
}

// a new loot on a stand that has none (not the one just emptied, when another is free)
void PlaceLoot(int iAvoid)
{
    std::vector<int> afree;
    for (size_t i = 0; i < r.afLoot.size(); ++i)
        if (!r.afLoot[i] && r.atLoot[i] == 0.0f && int(i) != iAvoid) afree.push_back(int(i));
    if (afree.empty() && iAvoid >= 0 && !r.afLoot[size_t(iAvoid)]) afree.push_back(iAvoid);
    if (afree.empty()) return;
    const int i = afree[Rand() % afree.size()];
    r.afLoot[size_t(i)] = 1;
    if (FPrint()) std::printf("cops3: loot on stand %d\n", i);
}

// the loot models: on the stands that hold one, out of the world on the others
void ShowLoots()
{
    if (!r.psw) return;
    for (size_t i = 0; i < r.aploLoot.size() && i < r.afLoot.size(); ++i)
    {
        LO* plo = r.aploLoot[i];
        if (!plo) continue;
        if (r.afLoot[i] && !plo->fInWorld) AddLo(r.psw, plo);
        else if (!r.afLoot[i] && plo->fInWorld) RemoveLo(r.psw, plo);
    }
}

// the power-ups' 3D models (v_ext's 159593) on the spots that hold one
void ShowPowers()
{
    if (!r.psw) return;
    for (size_t i = 0; i < r.aploPower.size() && i < r.atPower.size(); ++i)
    {
        LO* plo = r.aploPower[i];
        if (!plo) continue;
        const bool f = r.atPower[i] == 0.0f;
        if (f && !plo->fInWorld) AddLo(r.psw, plo);
        else if (!f && plo->fInWorld) RemoveLo(r.psw, plo);
    }
}

int CLootOut()
{
    int c = 0;
    for (char f : r.afLoot) c += f ? 1 : 0;
    for (const PLAYER& p : r.ap) c += p.fCarry ? 1 : 0;
    for (float t : r.atLoot) c += t != 0.0f ? 1 : 0;
    return c;
}

// a loot to come back on a free stand after a while
void ScheduleLoot()
{
    for (size_t i = 0; i < r.atLoot.size(); ++i)
        if (r.atLoot[i] == 0.0f && !r.afLoot[i])
        {
            r.atLoot[i] = g_clock.t + kLootBack;
            return;
        }
}

LO* PloTeamFirst(int team)
{
    for (int i = 0; i < r.cplayer; ++i)
        if (r.ap[size_t(i)].team == team) return r.ap[size_t(i)].plo;
    return nullptr;
}

// the original's board (script 2414): the scorer's entry set, the board shown with its line (the side's first point,
// the win; none otherwise)
void ShowBoard(int team, bool fWin)
{
    if (!r.pboard) return;
    SetScore3(r.pboard, PloTeamFirst(team), r.ascore[size_t(team)]);
    int arg = -1;
    if (fWin) arg = team == TEAM_Sly ? kTxtSlyWins : kTxtCarmWins;
    else if (r.ascore[size_t(team)] == 1) arg = team == TEAM_Sly ? kTxtSlyScores : kTxtCarmScores;
    ShowScreenObjArgs(r.pboard, 1, &arg);
}

void Score(int team)
{
    if (r.winner >= 0) return;
    // (tests: SLY3_COPSNOSCORE=1 no points, e.g. a walk of a hub probing its ground: the falls into the sea are not Carmelita's)
    if (static const bool s_fNo = std::getenv("SLY3_COPSNOSCORE") != nullptr; s_fNo) return;
    ++r.ascore[size_t(team)];
    r.cStreak = r.teamStreak == team ? r.cStreak + 1 : 1;
    r.teamStreak = team;
    if (r.pboard)
    {
        PsndStartDialog(team == TEAM_Sly ? kDlgSlyScores : kDlgCarmScores);
        r.tBoardShow = g_clock.t + 0.5f;
        r.teamBoard = team;
    }
    if (FPrint()) std::printf("cops3: score %d : %d\n", r.ascore[0], r.ascore[1]);
    if (r.ascore[size_t(team)] >= kWin)
    {
        r.winner = team;
        r.tWin = g_clock.t;
        Message(team == TEAM_Sly ? PTX_CopsSlyWins : PTX_CopsCarmWins, team);
        if (FPrint()) std::printf("cops3: winner %s\n", team == TEAM_Sly ? "Sly" : "Carmelita");
    }
}

PLAYER* PplayerOf(const LO* plo)
{
    if (!plo) return nullptr;
    for (int i = 0; i < r.cplayer; ++i)
        if (r.ap[size_t(i)].plo == plo) return &r.ap[size_t(i)];
    return nullptr;
}

// the spawn farthest from the other side's players
glm::vec3 PosSpawnFor(const PLAYER& p)
{
    const HUB* ph = PhubCur();
    if (!ph || ph->aspawn.empty()) return PosLoWorld(p.plo);
    float best = -1.0f;
    glm::vec3 pos = V(ph->aspawn[0]);
    for (const SITE& s : ph->aspawn)
    {
        float dMin = 1e9f;
        for (int i = 0; i < r.cplayer; ++i)
            if (r.ap[size_t(i)].plo && r.ap[size_t(i)].team != p.team)
                dMin = std::min(dMin, glm::length(V(s) - PosLoWorld(r.ap[size_t(i)].plo)));
        if (dMin > best) best = dMin, pos = V(s);
    }
    return pos;
}

void PlacePlayer(PLAYER& p, const glm::vec3& pos)
{
    if (!p.plo) return;
    // facing the middle of the loot stands (the round's centre)
    glm::vec3 c(0.0f);
    if (const HUB* ph = PhubCur(); ph && !ph->aloot.empty())
    {
        for (const SITE& s : ph->aloot) c += V(s);
        c /= float(ph->aloot.size());
    }
    const float h = std::atan2(c.y - pos.y, c.x - pos.x);
    if (p.team == TEAM_Sly && p.plo == split3::PloPlayer(r.psw, 0))
    {
        // (the current player: at his next update, as a route's teleport step: sly.cpp)
        g_routetele.fPending = true;
        g_routetele.x = pos.x, g_routetele.y = pos.y, g_routetele.z = pos.z + 20.0f;
        g_routetele.fH = true;
        g_routetele.h = h;
        g_routetele.fVel = false;
    }
    else
    {
        // (the rows: the local x axis toward h, y to its left, z up)
        const float mat[9] = {std::cos(h), std::sin(h), 0.0f, -std::sin(h), std::cos(h), 0.0f, 0.0f, 0.0f, 1.0f};
        sly3::PlaceLo3(p.plo, mat, pos + glm::vec3(0.0f, 0.0f, 20.0f));
        // (no fall speed kept from before: a fall out of the area at full speed went through the floor again)
        if (STEP* pst = PstepFromLo(p.plo)) SetAloVelocityTo(pst->palo, glm::vec3(0.0f));
        if (GUARD* pg = PguardFromLo(p.plo)) SetGuardHeading(pg, h);
        if (FPrint() && p.team == TEAM_Carm) std::printf("cops3: carm heading %.3f %.3f\n", std::cos(h), std::sin(h));
    }
    if (FPrint()) std::printf("cops3: player %d placed at %.0f %.0f %.0f (cid 0x%X)\n", p.plo->oid, pos.x, pos.y, pos.z, unsigned(p.plo->Cid()));
}

void SetCarmHealth(LO* plo)
{
    if (GUARD* pg = PguardFromLo(plo))
    {
        // (the original's Cops and Robbers: her splice gives her 40 in the two-player game: four cane hits)
        pg->n338 = 40;
        pg->n33C = 40;
    }
}

// ---- drawing (splitscreen3.cpp's helpers)
constexpr float DEPTH = 16760000.0f;
glm::vec4 Rgb(int rr, int g, int b, int a = 128) { return glm::vec4(float(rr), float(g), float(b), float(a)); }
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
constexpr float kXAspect = 640.0f / (448.0f * 4.0f / 3.0f);   // (a canvas x unit is narrower than a y unit)
void Diamond(float cx, float cy, float s, glm::vec4 rgba, float dz)
{
    const float sx = s * kXAspect;
    const glm::vec2 a[6] = {{cx, cy - s}, {cx + sx, cy}, {cx, cy + s}, {cx, cy - s}, {cx, cy + s}, {cx - sx, cy}};
    g_hud.FillScreenTris(a, 6, rgba, (DEPTH + dz) / 16777216.0f);
}
void Arrow(float cx, float cy, float ang, float s, glm::vec4 rgba, float dz)
{
    // a triangle pointing along ang (canvas: x right, y down)
    const glm::vec2 d(std::cos(ang), std::sin(ang)), n(-d.y, d.x);
    auto pt = [&](glm::vec2 v) { return glm::vec2(cx + v.x * kXAspect, cy + v.y); };
    const glm::vec2 a[3] = {pt(d * s), pt(-d * s * 0.6f + n * s * 0.7f), pt(-d * s * 0.6f - n * s * 0.7f)};
    g_hud.FillScreenTris(a, 3, rgba, (DEPTH + dz) / 16777216.0f);
}

// the hub's scripts stopped (its world script plays the episode's arrival: the gang and Sly taken out of the world,
// the docks, the guards' commands; the ambient ones go too: the round's hub is a quiet playground)
void StopHubScripts(SW* psw)
{
    for (LO* plo : psw->mpcidaplo[0x87])
        if (SCPT3* ps = PscptOfLo3(plo); ps && FScptRunning3(ps)) StopScpt3(ps);
}

glm::vec4 RgbTeam(int team, int a = 128) { return team == TEAM_Sly ? Rgb(28, 64, 124, a) : Rgb(124, 92, 16, a); }
glm::vec4 OffTeam(int team) { return team == TEAM_Sly ? glm::vec4(70, 104, 128, 0) : glm::vec4(128, 110, 50, 0); }
}  // namespace

// ------------------------------------------------------------------ the hubs, the choice
// (Pirate is offered since round 8: its bay's overview zone is forced off for the round, OnPlayersChosen)
int CHubs() { return int(Hubs().size()); }
int IepHub(int ihub) { return ihub >= 0 && ihub < int(Hubs().size()) ? Hubs()[size_t(ihub)].iep : sly3::EP_Venice; }
int IworldHub(int ihub) { return ihub >= 0 && ihub < int(Hubs().size()) ? Hubs()[size_t(ihub)].iworld : 3; }
int IHubChosen()
{
    if (!r.fChosenInit)
    {
        r.fChosenInit = true;
        if (const char* sz = std::getenv("SLY3_COPSMAP")) r.ihubChosen = std::clamp(std::atoi(sz), 0, int(Hubs().size()) - 1);
    }
    return r.ihubChosen;
}
void StepHubChosen(int d)
{
    const int c = CHubs();
    r.ihubChosen = ((IHubChosen() + d) % c + c) % c;
}

bool FCopsNode(const LO* pnode)
{
    // (Cops and Robbers' row: the class-0xAA object of job 2409, a two-player game)
    return pnode && pnode->U32Field3(0, 0x48, 0xFFFFFFFFu) == 2409u && pnode->U32Field3(0, 0x50, 0) == 2u;
}

bool FStartChosen(SW* psw, LO* pnode)
{
    if (!FCopsNode(pnode) || IHubChosen() <= 0) return false;
    r.ihubPending = IHubChosen();
    if (FPrint()) std::printf("cops3: start on hub %d (world %d)\n", r.ihubPending, IworldHub(r.ihubPending));
    // (the original's HUD, board and stands: v_ext kept with Cops and Robbers' option block; read now, made in the
    // transition before the hub loads, pcloader.h)
    SetPcKeptWorldKeys(kIwrecVenice, kOidJobCops, 2413);
    RequestPcKeptWorld(kIwrecVenice);
    StartChallengeIn3(psw, pnode, 0, -1, IworldHub(r.ihubPending));
    return true;
}

bool FActive() { return r.fActive; }
bool FRoundLoading(int filter) { return r.ihubPending > 0 && filter == IworldHub(r.ihubPending) && FChallengeRunning3(); }

void OnWorldLoaded(SW* psw)
{
    const bool fRound = r.ihubPending > 0 && psw && psw->filter == IworldHub(r.ihubPending) && FChallengeRunning3();
    r.fActive = false;
    r.psw = psw;
    if (!fRound)
    {
        r.ihubPending = -1;
        r.ihub = -1;
        return;
    }
    r.ihub = r.ihubPending;
    r.ihubPending = -1;
    r.fActive = true;
    // (the original's two-player screen mode: the split screen, both gauges, the 2P Smoke Bomb cost, Exit 2P)
    psw->nScreenMode3 = 2;
    split3::SetTwoCameras(psw, true);   // (the sounds' 2P attenuation, as Cops and Robbers' script sets it)
    const HUB& h = Hubs()[size_t(r.ihub)];
    r.ascore = {0, 0};
    r.afLoot.assign(h.aloot.size(), 0);
    r.atLoot.assign(h.aloot.size(), 0.0f);
    // (Venice: four power-ups streamed among six spots (the proxies 140842..140847); here all spots but one hold one)
    r.atPower.assign(h.apower.size(), 0.0f);
    if (r.atPower.size() > 1) r.atPower[size_t(Rand() % unsigned(r.atPower.size()))] = kPowerEmpty;
    r.winner = -1;
    r.fEnded = false;
    r.tStart = g_clock.t;
    r.rng = 12345u + unsigned(r.ihub) * 7919u;
    r.cplayer = 0;
    r.ap = {};
    r.pso2p = r.pboard = nullptr;
    r.aploStand.clear(), r.aploSafe.clear(), r.aploLoot.clear(), r.aploPower.clear();
    r.fResult = false;
    r.tBoardShow = 0.0f, r.teamBoard = -1, r.fBoardUp = false, r.idEndLine = -1;
    r.teamStreak = -1, r.cStreak = 0;
    for (int i = 0; i < kLootsOut; ++i) PlaceLoot(-1);
    // player 1 is Sly: China's hub comes in with the safe house (0x45, a PO: its gang switch) as the current player, so
    // the round's robber was the safe house (not drawn, no records, taken out with the hub's story below); the split
    // screen chooses its players right after this
    if (LO* pcur = PloPoCurrent(psw); !pcur || !FCidDerivedFrom(pcur->Cid(), sly3::CID_JT))
    {
        LO* psly = nullptr;
        for (LO* plo : psw->mpcidaplo[S3CID(0x35)])
            if (!psly || plo->oid == 24) psly = plo;
        if (psly)
        {
            if (!psly->fInWorld) AddLo(psw, psly);
            EnablePo(psw, psly, 1);
            SetPoCurrent(psw, psly);
            if (FPrint()) std::printf("cops3: Sly %d made the current player (was %d, cid 0x%X)\n", psly->oid, pcur ? pcur->oid : -1, pcur ? unsigned(pcur->Cid()) : 0u);
        }
    }
    // the hub's story quiet for the round: every task of the world taken as done without its handlers (no intro, no
    // prompts: the challenge's fresh progress would make the episode's first task available, and the hub's world
    // script plays its arrival, the gang's prompts and the hazard room's question over the round)
    for (auto& pt : g_tasks3.atask) pt->nState = 3;
    g_tasks3.aptaskAvailable.clear();
    if (FPrint()) std::printf("cops3: round on hub %d (world %d)\n", r.ihub, psw->filter);
}

LO* PloPlayerWanted(SW* psw, int i)
{
    if (!r.fActive || !psw || i != 1) return nullptr;
    // (player 2: Carmelita 28, from her class's list: a find by name can give another object of that name first, e.g.
    // in c_ext; player 1 is Sly, the current player)
    LO* pany = nullptr;
    for (LO* plo : psw->mpcidaplo[sly3::CID_CARMELITA])
    {
        if (plo->oid == 28) return plo;
        if (!pany) pany = plo;
    }
    if (pany) return pany;
    // (a hub without her (Pirate): v_ext's Carmelita copied in from the kept world, once per world)
    static SW* s_psw = nullptr;
    static uint32_t s_serial = 0;
    static LO* s_pcopy = nullptr;
    if (s_psw != psw || s_serial != psw->serial)
    {
        s_psw = psw, s_serial = psw->serial;
        LO* ptmpl = PloKeptFind(kIwrecVenice, 28, sly3::CID_CARMELITA);
        s_pcopy = ptmpl ? PloPcCloneIntoWorld(psw, ptmpl, true) : nullptr;
        // (her scale, EOPID 370: v_ext 28's comes with job 2409's option block, applied in Venice when the job runs: never in
        // the kept world, so its value as Venice's round applies it, 0.75)
        if (GUARD* pg = s_pcopy ? PguardFromLo(s_pcopy) : nullptr)
        {
            ApplyGuardScaleOption3(pg);
            if (pg->gScale3 == 1.0f) ApplyGuardScale3(pg, 0.75f);
        }
        if (FPrint()) std::printf("cops3: Carmelita copied from v_ext: %s\n", s_pcopy ? "ok" : "failed");
    }
    return s_pcopy;
}

void PauseSpawners(bool f);   // (below)

void OnPlayersChosen(SW* psw)
{
    if (!r.fActive || !psw) return;
    PauseSpawners(true);
    r.cplayer = 0;
    // (two players, or four as teams: players 1 / 3 the robbers, 2 / 4 the cops, splitscreen3.cpp's copies)
    for (int i = 0; i < split3::CView() && i < int(r.ap.size()); ++i)
    {
        LO* plo = split3::PloPlayer(psw, i);
        if (!plo) continue;
        PLAYER& p = r.ap[size_t(r.cplayer++)];
        p = PLAYER{};
        p.plo = plo;
        p.team = (i & 1) == 0 ? TEAM_Sly : TEAM_Carm;
    }
    // the hub's story out of the world for the round: its guards (the players' Carmelita stays) and spawners, the job entrances and
    // their prompts (the trigger volumes 0x15: "Only Bentley can complete this job!", the hazard room's question), the
    // markers 0x78, the safe house 0x45 (its door: the gang switch) and the exits 0x18 (to the jobs' worlds)
    int cOut = 0;
    std::vector<LO*> aploOut;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (!plo->fInWorld || PplayerOf(plo)) continue;
        const int cid = plo->Cid();
        // (round 9: and the spawners (Sly 3 0x8D; 0x77 was Sly 2's id, Sly 3's ROPEMESH: MODES-08): they bring guards back around the players from their proxies, spawner.cpp)
        if (FCidDerivedFrom(cid, sly3::CID_GUARD) || cid == sly3::CID_TRIGGER || cid == 0x18 || cid == 0x45 || cid == 0x78 || cid == 0x8D)
            aploOut.push_back(plo);
    }
    // (round 9: the guards' streaming proxies too: a proxy that registers near a player attaches its guard again (activate.cpp
    // RegisterLoTree -> 0x1CA8C0), so six of Outback's guards were back on their patrols within a minute of the round)
    int cProxyOut = 0;
    for (auto& [pproxy, ps] : psw->mpproxy)
    {
        if (!pproxy || ps.iwe < 0 || ps.iwe >= int(psw->awe.size()) || !pproxy->fInWorld) continue;
        const WE& we = psw->awe[size_t(ps.iwe)];
        if (we.aplo.empty() || !we.aplo[0] || !FCidDerivedFrom(we.aplo[0]->Cid(), sly3::CID_GUARD) ||
            FCidDerivedFrom(we.aplo[0]->Cid(), sly3::CID_CARMELITA))
            continue;
        bool fPlayer = false;   // (a proxy holding a player's character stays)
        for (LO* plo : we.aplo) fPlayer = fPlayer || (plo && PplayerOf(plo));
        if (fPlayer) continue;
        if (std::find(aploOut.begin(), aploOut.end(), pproxy) == aploOut.end()) aploOut.push_back(const_cast<LO*>(pproxy)), ++cProxyOut;
    }
    for (LO* plo : aploOut)
    {
        RemoveLo(psw, plo);
        ++cOut;
    }
    if (FPrint()) std::printf("cops3: %d guard proxies out\n", cProxyOut);
    StopHubScripts(psw);
    const HUB& h = Hubs()[size_t(r.ihub)];
    // (Pirate: p_ext's bay zone 56193 (its follow camera 21550 away, 33000 up: the bay's overview, priority 4 over the
    // town's zones) holds the whole town: both views stayed on the far overview. The round is played on foot: that zone
    // forced off (the zones' own forced state, orig3 0x221268), the town's zone cameras (priority 2) follow the players)
    if (h.iworld == 31)
        if (LO* pz = PloFindSwObjectByOid(psw, 56193)) SetZoneForced(pz, 0);
    for (int i = 0; i < r.cplayer; ++i)
    {
        PLAYER& p = r.ap[size_t(i)];
        if (!p.plo->fInWorld) AddLo(psw, p.plo);   // (the arrival script that puts Sly in the world is stopped)
        if (p.team == TEAM_Carm)
        {
            // (a hub's Carmelita that the hub's story had not started yet has no reaction job (0x9C): the guards' reset
            // gives it, then her pad again: her player job 0xE9 comes from that reaction)
            if (GUARD* pg = PguardFromLo(p.plo); pg && !PjobFind(pg, 0x9C))
            {
                ResetGuard(p.plo, nullptr);
                const int iplayer = IPlayerCarm3(p.plo);
                SetCarmPlayer3(p.plo, -1);
                SetCarmPlayer3(p.plo, iplayer >= 0 ? iplayer : i);
            }
            SetCarmHealth(p.plo);
            // (tests: SLY3_COPSCARMPOWER=0x32|0x33 her power-up from the start)
            if (const char* sz = std::getenv("SLY3_COPSCARMPOWER")) SetCarmPower3(p.plo, int(std::strtol(sz, nullptr, 0)), 100);
        }
        PlacePlayer(p, V(h.aspawn[size_t(std::min<int>(i, int(h.aspawn.size()) - 1))]));
    }
    // the original's presentation: the 2P HUD (the bands, the portraits) and the score board, copied from v_ext
    r.pso2p = PloFromVenice(psw, kOidHud2p);
    r.pboard = PloFromVenice(psw, kOidBoard);
    if (r.pso2p) ShowScreenObj(r.pso2p, -1);
    // the original's stands, safe points and loot (3D, v_ext's models) on the hub's sites
    {
        const float mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        auto place = [&](OID oid, const SITE& site, float dz) -> LO* {
            LO* plo = PloFromVenice(psw, oid);
            if (!plo) return nullptr;
            sly3::PlaceLo3(plo, mat, V(site) + glm::vec3(0.0f, 0.0f, dz));
            // (shown only: their collision entries off, nothing stands on them or picks them up; the rules are the round's)
            for (const auto& up : psw->aploAll)
                if ((up.get() == plo || FLoInParentChain(up.get(), plo)) && FCidDerivedFrom(up->Cid(), sly3::CID_ALO))
                    for (SOE& e : static_cast<ALO*>(up.get())->asoe) SetSoeOff(static_cast<ALO*>(up.get()), &e);
            return plo;
        };
        for (const SITE& st : h.aloot) r.aploStand.push_back(place(kOidStand, st, 0.0f));
        for (const SITE& st : h.asafe) r.aploSafe.push_back(place(kOidSafe, st, 0.0f));
        for (const SITE& st : h.aloot) r.aploLoot.push_back(place(kOidLoot, st, 60.0f));
        for (const SITE& st : h.apower) r.aploPower.push_back(place(kOidPower, st, 0.0f));
        ShowLoots();
        ShowPowers();
    }
    if (r.pboard) SetScoreEntries3(r.pboard, PloTeamFirst(TEAM_Sly), PloTeamFirst(TEAM_Carm));
    if (r.pboard)
    {
        // the original's lines (v_ext's dialog streams, their subtitles v_ext's texts) and the intro's line (script 2412)
        // (round 9: and the power-ups' lines (the gauge's new face plays its item row's line: the rows 25 / 35 / 37 / 30 /
        // 50..53, four lines each from 312, gauge3.cpp UpdateFace))
        static const std::vector<int> s_aid = [] {
            std::vector<int> a = {kDlgIntro, kDlgSlyScores, kDlgCarmScores, kDlgSly3, kDlgCarm3, kDlg4, kDlgSlyWins, kDlgCarmWins};
            for (int id = 312; id < 344; ++id) a.push_back(id);
            return a;
        }();
        const int c = CImportPcKeptDialogs(kIwrecVenice, s_aid.data(), int(s_aid.size()));
        static std::string (*s_pfnTextOld)(int) = nullptr;
        if (g_sndhooks.pfnText != StrLineText)
        {
            s_pfnTextOld = g_sndhooks.pfnText;
            g_pfnTextOld = s_pfnTextOld;
            g_sndhooks.pfnText = StrLineText;
        }
        if (FPrint()) std::printf("cops3: %d lines from v_ext\n", c);
        PsndStartDialog(kDlgIntro);
    }
    if (!r.pboard)
    {
        Message(PTX_CopsFirstTo, -1);
        r.tMsg += 3.0f;   // (the rule's line a little longer)
    }
    if (FPrint()) std::printf("cops3: %d players, %d guards out\n", r.cplayer, cOut);
}

// ------------------------------------------------------------------ the power-ups

// orig3: 0x154AB8 (Sly's vt slot 90: a power-up taken, the pickup's option 947): in the two-player games (0x36DBE0 == 2)
// the next of his shuffled four (+0x1450, made by his init 0x1AA2B0: {25 Smoke Bomb, 35 Rocket Boots, 37 the shield, 30 Mega
// Jump}) on his first slot (0x1548E8(0, id)), his power full (0x1F4438(+0x178)). (0x21F5D0(id, 1), the item marked owned in
// the progress with the broadcast 0x4C: not done natively, the two-player game's power-ups stay out of the save.) The id
// (Sly 3's; -1 none).
namespace
{
struct SLYBAG { std::vector<int> a{25, 35, 37, 30}; int cursor = -1; };
std::unordered_map<const LO*, SLYBAG> s_mpslybag;
uint32_t s_serialBag = 0;
}  // namespace

int GivePowerupSly(LO* plo)
{
    if (!g_psw || g_psw->nScreenMode3 != 2 || !plo || !PslyFromLo(plo)) return -1;
    ACTOR* pa = PactorFromLo(plo);
    if (!pa) return -1;
    if (g_psw->serial != s_serialBag) s_serialBag = g_psw->serial, s_mpslybag.clear();
    SLYBAG& b = s_mpslybag[plo];
    int id = NNextShuffled3(b.a, b.cursor);
    if (const char* sz = std::getenv("SLY3_COPSSLYPOWER")) id = std::atoi(sz);   // (tests: this one every time)
    SetActorAbilitySlot(pa, 0, sly3::Item2FromItem3(id));
    SetActorPower(pa, pa->n1300);
    if (FPrint()) std::printf("cops3: %d (player %d) takes the power-up %d\n", plo->oid, split3::IExtraPlayerOf(plo) < 0 ? 1 : split3::IExtraPlayerOf(plo) + 1, id);
    return id;
}

// a power-up given to a player by its class's vt slot 90 (Sly 0x154AB8, Carmelita 0x161BE0); the kind / id, -1 none
int GivePowerup(LO* plo)
{
    if (!plo) return -1;
    if (FCidDerivedFrom(plo->Cid(), sly3::CID_CARMELITA)) return GiveCarmPowerup3(plo);
    // (AUDIT4 MODES-13 / 14: the ships' slot 90 0x2151E8, the planes' 0x141F80)
    if (FCidDerivedFrom(plo->Cid(), S3CID(0x58))) return ::GiveShipPowerup3(plo);
    if (FCidDerivedFrom(plo->Cid(), S3CID(0x52))) return ::GivePlanePowerup3(plo);
    return GivePowerupSly(plo);
}

namespace
{
// orig3: 0x10A4A0 (the SO's method option 947, EOPID 946: vt slot 90 of the object). Venice's power-ups (the breakable
// 159593 at the proxies 140842..140847, four instances streamed among six spots; its splice block 159593: s5, the break,
// calls (947 of its breaker) when the breaker is an SO)
struct INITPOWERUP
{
    INITPOWERUP()
    {
        RegisterObjectMethod3(0x10A4A0, [](LO* plo, const std::vector<CRef>&) {
            GivePowerup(plo);
            return CRef(TAGK_Void);
        });
    }
} s_initpowerup;
}  // namespace

// ------------------------------------------------------------------ the rules

// (round 9: the hub's spawners bring no guards during a native round: they are stepped out of the world too)
void PauseSpawners(bool f)
{
    static bool s_fPausedByUs = false;
    if (f && !g_fSpawnersPaused) g_fSpawnersPaused = s_fPausedByUs = true;
    else if (!f && s_fPausedByUs) g_fSpawnersPaused = s_fPausedByUs = false;
}

void UpdateVeniceCopies(SW* psw);   // (below)

void Update(SW* psw)
{
    if (psw && !r.fActive) UpdateVeniceCopies(psw);
    // (round 9: the hub's spawners bring no guards during a native round: out of the world they are still stepped)
    PauseSpawners(r.fActive && psw);
    if (!r.fActive || !psw || r.fEnded) return;
    StopHubScripts(psw);
    const HUB* ph = PhubCur();
    if (!ph) return;
    const float t = g_clock.t;
    // the board after the scorer's line, the commentator's / winner's line after the board (script 2414's order)
    if (r.tBoardShow != 0.0f && t >= r.tBoardShow)
    {
        r.tBoardShow = 0.0f;
        ShowBoard(r.teamBoard, r.ascore[size_t(r.teamBoard)] >= kWin);
        r.fBoardUp = true;
        r.tBoardUp = t;
    }
    if (r.fBoardUp && r.pboard && t - r.tBoardUp > 0.5f && (FScreenObjDone(r.pboard) || t - r.tBoardUp > 3.0f * kWinHold))
    {
        r.fBoardUp = false;
        const int n = r.ascore[size_t(r.teamBoard)];
        int id = -1;
        if (n >= kWin) id = r.teamBoard == TEAM_Sly ? kDlgSlyWins : kDlgCarmWins;
        else if (r.cStreak == 4) id = kDlg4;
        else if (r.cStreak == 3) id = r.teamBoard == TEAM_Sly ? kDlgSly3 : kDlgCarm3;
        if (id != -1) PsndStartDialog(id);
        if (n >= kWin) r.idEndLine = id, r.tEndLine = t;
    }
    if (r.winner >= 0)
    {
        // (the original: the board with the winner's line until it hides, the winner's line, then the job complete stamp
        // 1066 and the challenge's end: the chooser)
        const bool fDone = !r.pboard ? t - r.tWin > kWinHold
                                     : r.idEndLine != -1 && t - r.tEndLine > 0.3f && (!FDialogPlaying(r.idEndLine) || t - r.tEndLine > 8.0f);
        if (fDone || t - r.tWin > 6.0f * kWinHold)
        {
            if (LO* pres = PloScreen3(psw, kOidResult); pres && !r.fResult)
            {
                r.fResult = true;
                r.tResult = g_clock.tReal;
                ShowScreenObj(pres, -1);
                // (the two-player game's stamp, as jobend3.cpp IdStampPart for the manager's mode 2: by the job's character
                // (+0xB0: 1 Sly 0x450, 0x40 Carmelita 0x454), which Venice's script sets to the winner ("Carmelita is the
                // Winner!" on her win); v_ext's kept job 2409 keeps its loaded value (Sly), so the winner's side here: the
                // robbers' Sly stamp, the cops' Carmelita stamp, also for teams)
                SelectScreenObjPart(pres, r.winner == TEAM_Carm ? 0x454 : 0x450);
                // (its title, "Sly is the Winner!" / "Carmelita is the Winner!": the winner's line as Venice's job ends with it
                // (the manager's command 11 with 5255 / 5254), the text from v_ext's when the hub has none)
                SetChallengeEndLine3(r.winner == TEAM_Carm ? kTxtCarmWinner : kTxtSlyWinner, StrLineText);
                if (FPrint()) std::printf("cops3: round over (%d : %d) at frame %d, the stamp waits (Triangle: the chooser, Square: again)\n", r.ascore[0], r.ascore[1], g_iframeMain);
            }
            if (!r.fResult) { r.fEnded = true; EndChallenge3(psw); return; }
            // (as jobend3's state 3, orig3 0x1875B0: Square replays (here the hub again), Triangle returns to the chooser)
            if (g_clock.tReal - r.tResult < 0.5f || FMenuShown()) return;
            if (GrfbtnTakePressed(0x80))
            {
                r.fEnded = true;
                r.ihubPending = r.ihub;
                if (FPrint()) std::printf("cops3: again on hub %d\n", r.ihub);
                SetPcKeptWorldKeys(kIwrecVenice, kOidJobCops, 2413);
                RequestPcKeptWorld(kIwrecVenice);
                RequestTransition(psw, nullptr, sly3::POINT_DEFAULT, -1, 0, -1);
            }
            else if (GrfbtnTakePressed(0x10))
            {
                r.fEnded = true;
                if (FPrint()) std::printf("cops3: back to the chooser\n");
                EndChallenge3(psw);
            }
        }
        return;
    }
    // new loots
    for (size_t i = 0; i < r.atLoot.size(); ++i)
        if (r.atLoot[i] != 0.0f && t >= r.atLoot[i])
        {
            r.atLoot[i] = 0.0f;
            PlaceLoot(int(i));
        }
    for (int k = 0; k < 8 && CLootOut() < kLootsOut; ++k) ScheduleLoot();

    ShowLoots();
    ShowPowers();

    static float s_tPrint = 0.0f;
    const bool fPrintNow = FPrint() && std::getenv("SLY3_COPSPOS") && t - s_tPrint > 1.0f;
    if (fPrintNow) s_tPrint = t;
    // (the hub's out height (default: far below the lowest site): a player below it fell out of the round's area: Kaine Island's sea has no water volume for a
    // Carmelita (she sank for ever), a fall through the world: placed again at a spawn, no point)
    float zLow = 1e9f;
    for (const auto* pa : {&ph->aspawn, &ph->aloot, &ph->asafe, &ph->apower})
        for (const SITE& st : *pa) zLow = std::min(zLow, st.z);
    const float zOut = ph->zOut > -1e8f ? ph->zOut : zLow - kFallBelow;
    for (int ip = 0; ip < r.cplayer; ++ip)
    {
        PLAYER& p = r.ap[size_t(ip)];
        if (!p.plo || p.fDown) continue;
        const glm::vec3 pos = PosLoWorld(p.plo);
        if (pos.z < zOut)
        {
            if (FPrint()) std::printf("cops3: player %d fell out (z %.0f): placed again\n", ip + 1, pos.z);
            if (p.fCarry) { p.fCarry = false; ScheduleLoot(); }
            if (p.team == TEAM_Carm) SetCarmHealth(p.plo);
            if (STEP* pst = PstepFromLo(p.plo)) SetAloVelocityTo(pst->palo, glm::vec3(0.0f));
            PlacePlayer(p, PosSpawnFor(p));
            continue;
        }
        if (fPrintNow)
        {
            ACTOR* pa = PactorFromLo(p.plo);
            SLY* psly = PslyFromLo(p.plo);
            std::printf("cops3: t %.1f f %d player %d.%d at %.0f %.0f %.0f in %d sub %d carry %d state %d\n", t, g_iframeMain, p.plo->oid, ip, pos.x, pos.y, pos.z,
                        int(p.plo->fInWorld), pa ? pa->nSubDF4 : -9, int(p.fCarry), psly ? psly->jts : -9);
        }
        if (p.team == TEAM_Sly)
        {
            ACTOR* pa = PactorFromLo(p.plo);
            const bool fHurt = pa && pa->nSubDF4 != -1;
            if (!p.fCarry && !fHurt)
                for (size_t i = 0; i < ph->aloot.size(); ++i)
                    if (r.afLoot[i] && FNear(pos, V(ph->aloot[i]), kTakeRadius))
                    {
                        r.afLoot[i] = 0;
                        p.fCarry = true;
                        if (FPrint()) std::printf("cops3: loot taken from stand %zu by player %d\n", i, ip + 1);
                        Message(PTX_CopsSlyLoot, TEAM_Sly);
                        break;
                    }
            if (p.fCarry)
                for (const SITE& s : ph->asafe)
                    if (FNear(pos, V(s), kSafeRadius))
                    {
                        p.fCarry = false;
                        ScheduleLoot();
                        if (FPrint()) std::printf("cops3: loot delivered by player %d\n", ip + 1);
                        Message(PTX_CopsDelivered, TEAM_Sly);
                        Score(TEAM_Sly);
                        break;
                    }
        }
        for (size_t i = 0; i < ph->apower.size(); ++i)
            if (r.atPower[i] == 0.0f && FNear(pos, V(ph->apower[i]), kPowerRadius))
            {
                // (as Venice's streaming: the taken one's instance shows again at once at a free spot, not this one)
                r.atPower[i] = kPowerEmpty;
                std::vector<size_t> afree;
                for (size_t j = 0; j < r.atPower.size(); ++j)
                    if (j != i && r.atPower[j] != 0.0f) afree.push_back(j);
                if (!afree.empty()) r.atPower[afree[Rand() % unsigned(afree.size())]] = 0.0f;
                else r.atPower[i] = 0.0f;
                // the original's power-ups (Venice's pickups: the class's vt slot 90): Sly the next of Smoke Bomb, Rocket
                // Boots, the shield, Mega Jump; Carmelita the next of her super jump, charged shot, the jump to the robber,
                // the hit; the meter full
                GivePowerup(p.plo);
                Message(PTX_CopsPowerCarm, p.team);
                if (FPrint()) std::printf("cops3: power-up %zu to player %d\n", i, p.plo->oid);
                break;
            }
    }
}

// (Venice, a copy: at the spawn point of the round's four (s6284) farthest from the other team, or at the round's start
// the one nearest its own team's player that no player stands on)
void PlaceVeniceCopy(LO* plo, int iplayer, bool fStart)
{
    const int team = iplayer & 1;
    LO* pbest = nullptr;
    float best = fStart ? 1e9f : -1.0f;
    for (OID oid : {121625, 121627, 121629, 121626})
    {
        LO* pspawn = g_psw ? PloFindSwObjectByOid(g_psw, oid) : nullptr;
        if (!pspawn) continue;
        const glm::vec3 pos = PosLoWorld(pspawn);
        float d = fStart ? 1e9f : 1e9f;
        bool fTaken = false;
        for (int k = 0; k < split3::CView(); ++k)
        {
            LO* po = split3::PloPlayer(g_psw, k);
            if (!po || po == plo) continue;
            const float dk = glm::length(pos - PosLoWorld(po));
            if (dk < 300.0f) fTaken = true;
            if (fStart ? (k & 1) == team : (k & 1) != team) d = std::min(d, dk);
        }
        if (fStart && fTaken) continue;
        if (fStart ? d < best : d > best) best = d, pbest = pspawn;
    }
    if (!pbest) return;
    const glm::mat4 m = sly3::MatLoWorld3(pbest);
    float mat[9];
    for (int rr = 0; rr < 3; ++rr)
        for (int c = 0; c < 3; ++c) mat[rr * 3 + c] = m[rr][c];
    sly3::PlaceLo3(plo, mat, glm::vec3(m[3]) + glm::vec3(0.0f, 0.0f, 20.0f));
    if (STEP* pst = PstepFromLo(plo)) SetAloVelocityTo(pst->palo, glm::vec3(0.0f));
    if (GUARD* pg = PguardFromLo(plo)) SetGuardHeading(pg, std::atan2(m[0].y, m[0].x));
    // (a cop's health as the job's splice gives Carmelita 28 in the two-player game: 40, four cane hits)
    if (team == TEAM_Carm && fStart) SetCarmHealth(plo);
    if (FPrint()) std::printf("cops3: Venice: player %d (%d) placed at spawn %d\n", iplayer + 1, plo->oid, pbest->oid);
}

// Venice's round start (the job's s1621 ran: s6290 #t, its two players on their spawn points): the copies onto the free
// spawn points (until then they stood beside player 1 in the hideout: a copy put there fell out of the world)
void UpdateVeniceCopies(SW* psw)
{
    static uint32_t s_serial = 0;
    static bool s_fPlaced = false;
    if (psw->serial != s_serial) s_serial = psw->serial, s_fPlaced = false;
    JOB3* pj = g_tasks3.pjobCur;
    if (s_fPlaced || split3::CView() < 3 || !pj || pj->oid != kOidJobCops || !pj->plo || !pj->plo->pframe) return;
    const CRef* pf = pj->plo->pframe->PrefFindBinding(SYMID(6290), 1);
    if (!pf || !pf->FTrue()) return;
    s_fPlaced = true;
    for (int i = 2; i < split3::CView(); ++i)
        if (LO* plo = split3::PloPlayer(psw, i)) PlaceVeniceCopy(plo, i, true);
}

// Venice's own round with four players (split screen's teams): the job's splice (MISSION 2409, module 103: its event
// handler s20) scores only its two players, Sly 24 (s55) and Carmelita (s1353): `(eq obj s55)` for 0x0A / 0x0C, `(eq obj
// s1353)` for 0x10 / 0x0C. The copies (players 3 / 4) score through the script's own scorer s3406 (the board's add, the
// scorer's line, the win test s3409: first to s3387), and are restored / placed again natively (s6319's part for them:
// the spawn point of s6284 farthest from the other team). Deliveries (0x41 / 0x42 / 0x43 name the loot) need nothing.
bool FOnBroadcastVeniceCopy(int code, LO* plo)
{
    const int iplayer = split3::IExtraPlayerOf(plo);
    JOB3* pj = g_tasks3.pjobCur;
    if (iplayer < 2 || !pj || pj->oid != kOidJobCops || !pj->plo || !pj->plo->pframe) return false;
    if (code != 0x0A && code != 0x10 && code != 0x0C) return false;
    const int team = iplayer & 1;   // (players 1 / 3 the robbers, 2 / 4 the cops)
    CFrame* pf = pj->plo->pframe.get();
    static std::unordered_map<const LO*, bool> s_mpfDown;
    static uint32_t s_serial = 0;
    if (g_psw && g_psw->serial != s_serial) s_serial = g_psw->serial, s_mpfDown.clear();
    bool& fDown = s_mpfDown[plo];
    auto score = [&](int symSide) {
        CRef* pproc = pf->PrefFindBinding(SYMID(3406), 1);
        CRef* pside = pf->PrefFindBinding(SYMID(symSide), 1);
        if (!pproc || !pside) { std::fprintf(stderr, "cops3: Venice's scorer s3406 / s%d not bound\n", symSide); return; }
        CRef a = *pside;
        RefApply(*pproc, 1, &a, pf);
        if (FPrint()) std::printf("cops3: Venice: player %d (%d) scores for %s\n", iplayer + 1, plo->oid, symSide == 55 ? "Sly" : "Carmelita");
    };
    switch (code)
    {
    case 0x0A:   // a robber hurt: caught at 0
        if (team == TEAM_Sly && !fDown)
            if (ACTOR* pa = PactorFromLo(plo))
                if (const int* pn = DmgCounterOf(pa); pn && *pn <= 0)
                {
                    fDown = true;
                    score(1353);
                }
        return true;
    case 0x10:   // a cop knocked out
        if (team == TEAM_Carm && !fDown)
        {
            fDown = true;
            score(55);
        }
        return true;
    default:     // 0x0C: placed again at the spawn point farthest from the other team, restored
    {
        fDown = false;
        if (team == TEAM_Sly) { if (ACTOR* pa = PactorFromLo(plo)) RestoreActorDamage3(pa); }
        else SetCarmHealth(plo);
        PlaceVeniceCopy(plo, iplayer, false);
        return true;
    }
    }
}

bool FOnBroadcast(int code, LO* plo)
{
    if (!r.fActive) return FOnBroadcastVeniceCopy(code, plo);
    if (r.fEnded) return false;
    PLAYER* pp = PplayerOf(plo);
    if (!pp) return false;
    PLAYER& p = *pp;
    switch (code)
    {
    case 0x0A:   // a robber hurt (orig3 0x1C3470's broadcast, after the health changed)
        if (p.team != TEAM_Sly) break;
        if (p.fCarry)
        {
            // the loot drops (back on a stand after a while)
            p.fCarry = false;
            ScheduleLoot();
            Message(PTX_CopsDropped, TEAM_Carm);
        }
        if (ACTOR* pa = PactorFromLo(p.plo))
            if (const int* pn = DmgCounterOf(pa); pn && *pn <= 0 && !p.fDown)
            {
                p.fDown = true;
                if (FPrint()) std::printf("cops3: player %d caught\n", int(pp - r.ap.data()) + 1);
                Message(PTX_CopsCaught, TEAM_Carm);
                Score(TEAM_Carm);
            }
        break;
    case 0x10:   // a cop knocked out (aicarm3.cpp: her state 14)
        if (p.team != TEAM_Carm || p.fDown) break;
        p.fDown = true;
        Message(PTX_CopsKnockedOut, TEAM_Sly);
        Score(TEAM_Sly);
        break;
    case 0xC:    // a death (a robber's after his health ran out or a fall; a cop's knock-out's end): placed again
        if (p.team == TEAM_Sly)
        {
            if (p.fCarry) { p.fCarry = false; ScheduleLoot(); }
            if (ACTOR* pa = PactorFromLo(p.plo)) RestoreActorDamage3(pa);
        }
        else SetCarmHealth(p.plo);
        p.fDown = false;
        PlacePlayer(p, PosSpawnFor(p));
        return true;   // (no world reload: the round goes on)
    default: break;
    }
    return false;
}

float GSpeedScale(const LO* plo)
{
    if (!r.fActive) return 1.0f;
    const PLAYER* p = PplayerOf(plo);
    return p && g_clock.t < p->tSprintEnd ? 1.6f : 1.0f;
}

// ------------------------------------------------------------------ drawing
void DrawOverlay(int width, int height, float xLeft, float xRight, float yTop, float yBottom)
{
    if (!r.fActive || !split3::FActive()) return;
    const HUB* ph = PhubCur();
    if (!ph) return;
    auto xCanvas = [&](float x) { return xLeft + (xRight - xLeft) * x / float(std::max(width, 1)); };
    auto yCanvas = [&](float y) { return yTop + (yBottom - yTop) * (1.0f - y / float(std::max(height, 1))); };
    const float t = g_clock.t;
    const float pulse = 0.5f + 0.5f * std::sin(t * 5.0f);

    for (int iv = 0; iv < split3::CView() && iv < r.cplayer; ++iv)
    {
        const PLAYER& me = r.ap[size_t(iv)];
        if (!me.plo) continue;
        const split3::VIEWRECT rv = split3::RectView(iv, width, height);
        split3::VIEWRECT rs{0, 0, rv.dx, rv.dy};
        split3::FRectScene(iv, rv.dx, rv.dy, &rs);
        const float x0 = xCanvas(float(rv.x + rs.x)), x1 = xCanvas(float(rv.x + rs.x + rs.dx));
        const float y0 = yCanvas(float(rv.y + rs.y + rs.dy)), y1 = yCanvas(float(rv.y + rs.y));   // (y0 the top)
        const CAMERA cam = CameraFromCm(split3::CmView(iv));
        const glm::mat4 m = cam.MatWorldToClip(float(rs.dx) / float(std::max(rs.dy, 1)));
        const glm::vec3 posMe = PosLoWorld(me.plo);
        // a marker over a world point: in the view, else an arrow on its edge toward it
        auto marker = [&](const glm::vec3& pos, glm::vec4 rgba, float s, const char* label, bool fRing) {
            const glm::vec4 c = m * glm::vec4(pos + glm::vec3(0.0f, 0.0f, 120.0f), 1.0f);
            const float cxm = 0.5f * (x0 + x1), cym = 0.5f * (y0 + y1);
            glm::vec2 ndc = c.w > 1.0f ? glm::vec2(c.x, c.y) / c.w : glm::vec2(c.x, c.y) * (c.w < 0.0f ? -1.0f : 1.0f);
            const bool fIn = c.w > 1.0f && std::fabs(ndc.x) < 0.95f && std::fabs(ndc.y) < 0.92f;
            if (fIn)
            {
                const float x = x0 + (ndc.x * 0.5f + 0.5f) * (x1 - x0), y = y1 - (ndc.y * 0.5f + 0.5f) * (y1 - y0);
                if (fRing)
                    g_hud.DrawRing(x, y, s * (1.5f + 0.4f * pulse), s * (1.25f + 0.4f * pulse), 1.0f, glm::vec4(rgba.r, rgba.g, rgba.b, 90.0f),
                                   (DEPTH + 40.0f) / 16777216.0f);
                Diamond(x, y, s, glm::vec4(0, 0, 0, 110), 41.0f);
                Diamond(x, y, s * 0.75f, rgba, 42.0f);
                if (label)
                {
                    char ach[32];
                    std::snprintf(ach, sizeof ach, "%s%dm", label, int(glm::length(pos - posMe) / 100.0f));
                    Text(ach, x, y + s + 1.0f, 11.0f, glm::vec4(rgba.r, rgba.g, rgba.b, 0));
                }
                return;
            }
            // (behind or outside: on the edge, pointing toward it)
            if (std::fabs(ndc.x) < 1e-4f && std::fabs(ndc.y) < 1e-4f) ndc.y = -1.0f;
            // (inside the view's edges, under the score box at the top)
            const float yLim = ndc.y > 0.0f ? 0.6f : 0.84f;
            const float k = std::min(0.9f / std::max(std::fabs(ndc.x), 1e-4f), yLim / std::max(std::fabs(ndc.y), 1e-4f));
            const glm::vec2 e = ndc * k;
            const float x = cxm + e.x * 0.5f * (x1 - x0), y = cym - e.y * 0.5f * (y1 - y0);
            const float ang = std::atan2(-e.y * (y1 - y0), e.x * (x1 - x0));
            Arrow(x, y, ang, s * 1.0f, glm::vec4(0, 0, 0, 110), 41.0f);
            Arrow(x, y, ang, s * 0.75f, rgba, 42.0f);
        };
        // the loot stands (both sides see them), the safe points (a robber who carries), power-ups, the carrier
        const bool fLooking = me.team == TEAM_Sly && !me.fCarry;
        for (size_t i = 0; i < ph->aloot.size(); ++i)
            if (r.afLoot[i]) marker(V(ph->aloot[i]), Rgb(128, 106, 20), fLooking ? 9.0f : 6.0f, fLooking ? "" : nullptr, fLooking);
        if (me.team == TEAM_Sly && me.fCarry)
            for (const SITE& s : ph->asafe) marker(V(s), Rgb(30, 120, 40), 10.0f, "", true);
        for (size_t i = 0; i < ph->apower.size(); ++i)
            if (r.atPower[i] == 0.0f) marker(V(ph->apower[i]), Rgb(30, 110, 128), 7.0f, nullptr, false);
        for (int j = 0; j < r.cplayer; ++j)
        {
            const PLAYER& o = r.ap[size_t(j)];
            if (j == iv || !o.plo) continue;
            if (o.fCarry && me.team == TEAM_Carm) marker(PosLoWorld(o.plo) + glm::vec3(0, 0, 80), Rgb(128, 24, 16), 10.0f, "", true);
        }
        // (the original's board and HUD copied from v_ext show the scores and the lines: nothing more; the native
        // fallback (no kept world) draws its own)
        if (r.pboard) continue;
        // the scores (the view's top, centred): the robbers' and the cops' points in their colours
        const float xc = 0.5f * (x0 + x1);
        Rect(xc - 62.0f, y0 + 6.0f, xc + 62.0f, y0 + 32.0f, Rgb(0, 0, 0, 80), 30.0f);
        Rect(xc - 62.0f, y0 + 30.0f, xc - 2.0f, y0 + 32.0f, RgbTeam(TEAM_Sly), 31.0f);
        Rect(xc + 2.0f, y0 + 30.0f, xc + 62.0f, y0 + 32.0f, RgbTeam(TEAM_Carm), 31.0f);
        char ach[16];
        std::snprintf(ach, sizeof ach, "%d", r.ascore[0]);
        Text(ach, xc - 32.0f, y0 + 7.0f, 20.0f, OffTeam(TEAM_Sly));
        std::snprintf(ach, sizeof ach, "%d", r.ascore[1]);
        Text(ach, xc + 32.0f, y0 + 7.0f, 20.0f, OffTeam(TEAM_Carm));
        Text("-", xc, y0 + 7.0f, 18.0f, glm::vec4(90, 90, 90, 0));
        // what this player carries / holds
        std::string line;
        if (me.fCarry) line = StrPort(PTX_CopsYouCarry);
        else if (me.team == TEAM_Carm && t < me.tSprintEnd) line = StrPort(PTX_CopsPowerCarm);
        if (!line.empty()) Text(line, xc, y0 + 36.0f, 13.0f, OffTeam(me.team));
        // the message (a few seconds), the winner's banner
        if (t - r.tMsg < kMsg || r.winner >= 0)
        {
            const float size = r.winner >= 0 ? 30.0f : 16.0f;
            const float yc = r.winner >= 0 ? 0.5f * (y0 + y1) - size : y0 + 56.0f;
            const float w = std::min(x1 - x0 - 8.0f, r.winner >= 0 ? 330.0f : 250.0f);
            Rect(xc - 0.5f * w, yc - 4.0f, xc + 0.5f * w, yc + size * 1.3f, Rgb(0, 0, 0, 90), 30.0f);
            if (r.teamMsg >= 0) Rect(xc - 0.5f * w, yc + size * 1.3f - 2.0f, xc + 0.5f * w, yc + size * 1.3f, RgbTeam(r.teamMsg), 31.0f);
            Text(r.msg, xc, yc, size, r.teamMsg >= 0 ? OffTeam(r.teamMsg) : glm::vec4(110, 110, 110, 0));
        }
    }
}

void DrawMenu(int width, int height)
{
    const LO* pnode = PnodeTwoPlayerShown3();
    if (!FCopsNode(pnode)) return;
    g_hud.Begin();
    // (the episode menu's 4:3 page: its right column, between the description and "Start Challenge")
    const int ihub = IHubChosen();
    const std::string name = StrEpisodeName3(IepHub(ihub));
    const float xL = 255.0f, xR = 608.0f, y = 262.0f;
    Rect(xL, y, xR, y + 46.0f, Rgb(0, 4, 24, 110), -10.0f);
    Rect(xL, y + 44.0f, xR, y + 46.0f, Rgb(20, 40, 90, 128), -9.0f);
    Text(StrPort(PTX_CopsMap), xL + 8.0f, y + 5.0f, 15.0f, glm::vec4(70, 90, 128, 0), 0);
    Text("&2+&.", xL + 8.0f, y + 24.0f, 15.0f, glm::vec4(70, 90, 128, 0), 0);
    const float xc = 0.5f * (xL + xR) + 30.0f;
    Arrow(xc - 118.0f, y + 15.0f, 3.14159f, 7.0f, Rgb(126, 104, 10), -8.0f);
    Arrow(xc + 118.0f, y + 15.0f, 0.0f, 7.0f, Rgb(126, 104, 10), -8.0f);
    Text(name, xc, y + 3.0f, 22.0f, glm::vec4(128, 106, 10, 0));
    Text(StrPort(ihub == 0 ? PTX_CopsMapOriginal : PTX_CopsMapNative), xc, y + 29.0f, 11.0f, glm::vec4(80, 80, 80, 0));
    // (the hub's picture: the page's own thumbnail frame shows the hub's episode's first replay picture,
    // epmenu3.cpp IdQuadShaderAlias3; Venice keeps the original's)
    g_hud.Flush(width, height);
}

void DumpLandmarks(SW* psw)
{
    if (!psw) return;
    std::printf("copsdump: world %d\n", psw->filter);
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        const int cid = plo->Cid();
        const bool f = cid == 0x28 || cid == 0x27 || cid == 0x25 || cid == 0x18 || cid == 0x23 || (cid >= 0x34 && cid <= 0x43) ||
                       cid == 0x6A || cid == 0x0A || cid == 0x0B || cid == 0x12;
        if (!f) continue;
        const glm::vec3 p = PosLoWorld(plo);
        std::printf("copsdump: cid 0x%02X oid %6d inst %6d in %d at %8.0f %8.0f %7.0f\n", cid, plo->oid, plo->oidInstance, int(plo->fInWorld), p.x, p.y, p.z);
    }
}
}  // namespace cops3
