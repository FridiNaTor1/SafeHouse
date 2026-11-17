// See challenge3.h. Ported from the Sly 3 decomp (orig3 addresses in the comments).

#include "challenge3.h"
#include "duel3.h"
#include "gauge3.h"

#include "epmenu3.h"
#include "lo.h"
#include "objmethod.h"
#include "jobend3.h"
#include "screen3.h"
#include "ref.h"
#include "savedata.h"
#include "sw.h"
#include "transition.h"
#include "game/sly3.h"
#include "game/task3.h"
#include "clock.h"
#include "brx.h"
#include "screenrt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

float GSmooth(float gCur, float gTarget, float dt, const SMP* psmp, float* pdgNext);   // xfmath.h (0x11D018 / orig3 0x11E598)

namespace
{
// orig3 0x4BC230: +0x14 the game object (kept by value here: the object is dvd_menu's, freed by the world change),
// +0x34.. the save copy (0x283BB0's record), its state (0x283F20: 1 running)
struct CHALLENGE3
{
    bool fRunning = false;
    OID oidNode = -1;
    int mode = -1;            // +0x50
    OID oidJob = -1;          // +0x48
    int iworld = -1;          // +0x58
    OID oidTask = -1;         // +0x5C
    OID oidKeyTask = -1;      // +0x60 (with +0x64: the task / event that ends the game, 0x2845B0; natively the job's
    int value = 2;            // +0x64  own end ends it, which is the same for Cops and Robbers)
    int iworldReturn = 0;     // the record's world (the chooser's dvd_menu)
    int n14A4 = -1;           // GAME +0x14A4
    int lineEnd = -1;         // +0x30 (the line of the end: 0x284A90)
    GAMESAVE gsSaved;         // +0x38 (the save as the chooser had it)
    // the rules (0x283F20 / 0x284DC8 / 0x284C28 / 0x2848D0)
    NODE3 node;               // +0x14's data (the class-0xAA object's options; the world's copy when it has one)
    LO* ploNode = nullptr;    // +0x14 itself in the current world (the broadcasts' object; null: not in this world)
    SW* pswNode = nullptr;    // (the world ploNode is of, and its load count: the node is found again in a new one)
    int cLoadNode = -1;
    LO* ploHud = nullptr;     // +0x18: the challenge HUD 1054 (0x41E: the timer dial / score; 0x283970)
    float g = 0.0f;           // +0x1C the value (0..1: a timer runs 1 -> 0 over +0x9C s; a score is points / 1000)
    float gShown = 0.0f;      // +0x20 the HUD's (eased toward +0x1C)
    float dg = 0.0f;          // +0x24 its rate per second (while the state is 1)
    float gScale = 1.0f;      // +0x28 the factor of command 2
    int cLeft = 1;            // +0x2C the counter (command 8 / 9)
    int state = 0;            // +0x7CD4 (0x284DC8): 1 running, 2 out of tries, 3 short of the goal, 4 done, 5 the
    float tState = 0.0f;      // +0x7CD8                two-player end, 6 a new record
    bool fResetAtStart = false;   // (the next world start resets the values: 0x283A50's request flag 4, see below)
    bool fStartFlag = false;      // (the start request's flag 0x80, SW +0x4348: the job's switch 0x186338 takes it)
};
CHALLENGE3 s_chal;

bool FPrint() { return std::getenv("SLY3_CHALPRINT") != nullptr; }

std::vector<int16_t> AkeyOpt(const LO* plo, int eopid)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == eopid && !o.aval.empty()) return o.aval[0].an;
    return {};
}

int NRefC(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_tagk == TAGK_Bool ? int(r.m_bool) : 0; }

float GProgress(const std::vector<int16_t>& akey)
{
    if (akey.empty()) return 0.0f;
    const CRef r = RefGetProgressKey(akey);
    return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? float(r.m_n) : 0.0f;
}

// the node in the current world (the start's object is dvd_menu's: gone with it)
LO* PloNodeCur()
{
    CHALLENGE3& c = s_chal;
    if (c.pswNode != g_psw || c.cLoadNode != g_cTransitionLoads)
    {
        c.pswNode = g_psw;
        c.cLoadNode = g_cTransitionLoads;
        c.ploNode = g_psw && c.fRunning ? PloFindSwObjectByOid(g_psw, c.oidNode) : nullptr;
    }
    return c.ploNode;
}

void Broadcast(int code) { Broadcast3(code, PloNodeCur()); }   // (0x1854C8(GAME, code, +0x14))

// orig3: 0x00284DC8 (the state: each change broadcast with the node as its object)
void SetChalState(int k)
{
    CHALLENGE3& c = s_chal;
    if (c.state == k) return;
    if (FPrint()) std::printf("challenge3: state %d -> %d (value %.4f)\n", c.state, k, c.g);
    switch (k)
    {
    case 0: Broadcast(0x39); break;   // (GAME +0x14A4 = -1 and +0x14 cleared: natively EndChallenge3's)
    case 1: Broadcast(0x2C); break;
    case 2: Broadcast(0x34); break;
    case 3: Broadcast(0x35); break;
    case 4: MarkNodeDone3(c.node); Broadcast(0x36); break;   // (0x283550: the game's "done" key)
    case 6:
        if (c.node.gGoal <= c.g) MarkNodeDone3(c.node);
        Broadcast(0x38);
        break;
    case 5: Broadcast(0x37); break;
    default: break;
    }
    c.state = k;
    c.tState = g_clock.t;
}

// orig3: 0x00284C28 (the value set, clamped to 0..1: its crossings of 0, 1, the goal (+0x84) and the record (+0xA4)
// broadcast)
void SetChalValue(float g)
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning) return;
    const float gOld = c.g;
    c.g = std::clamp(g, 0.0f, 1.0f);
    if (gOld > 0.0f && c.g == 0.0f) Broadcast(0x2D);
    if (gOld < 1.0f && c.g == 1.0f) Broadcast(0x2E);
    if (c.node.gGoal <= gOld && c.g < c.node.gGoal) Broadcast(0x30);
    if (gOld < c.node.gGoal && c.node.gGoal <= c.g) Broadcast(0x2F);
    if (c.node.gBest <= gOld && c.g < c.node.gBest) Broadcast(0x32);
    if (gOld < c.node.gBest && c.node.gBest <= c.g) Broadcast(0x31);
}

// orig3: 0x00283F20 (the values from the node: +0x78 / +0x7C / +0x80 / +0x88, the line none, then running)
void ResetChalValues()
{
    CHALLENGE3& c = s_chal;
    c.g = c.node.gStart;
    c.state = 0;
    c.dg = c.node.dgStart;
    c.gScale = c.node.gScaleStart;
    c.cLeft = c.node.cStart;
    c.lineEnd = -1;
    SetChalState(1);
}

// orig3: 0x00283620: the value as the node shows it: a score (+0xA0) "%d" of value x 1000, a time (+0x9C) "%d:%02d.%02d"
// of the time taken (1 - value) x +0x9C
std::string StrChalValue(const NODE3& n, float g)
{
    char ach[32];
    if (n.gScore != 0.0f)
    {
        std::snprintf(ach, sizeof(ach), "%d", int(g * 1000.0f + 0.5f));
        return ach;
    }
    if (n.dtLimit == 0.0f) return "";
    const float t = (1.0f - g) * n.dtLimit;
    const int m = int(std::fmod(t, 3600.0f) * (1.0f / 60.0f));
    const int sec = int(std::fmod(t, 60.0f));
    std::snprintf(ach, sizeof(ach), "%d:%02d.%02d", m, sec, int((t - std::floor(t)) * 100.0f));
    return ach;
}
}  // namespace

// orig3: the class-0xAA object's options in their order (the setters 1448 / 1449 write several fields: 0x2834D8 /
// 0x283500), from the reset 0x2830D0's defaults; its commands 1451..1453 (0x10E6B8 / 0x10E780 / 0x10E860 -> 0x283278);
// the record (+0xA4: 0x283150 reads the key +0xA8 at the load)
NODE3 NodeFromLo3(const LO* plo)
{
    NODE3 n;
    if (!plo) return n;
    n.oid = plo->oid;
    auto ival = [](const OPT& o, size_t i) { return i < o.aval.size() ? int(o.aval[i].n) : -1; };
    auto gval = [](const OPT& o, size_t i) { return i < o.aval.size() ? o.aval[i].ag[0] : 0.0f; };
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        switch (o.eopid)
        {
        case 1428: n.oidJob = ival(o, 0); break;
        case 1429: n.mode = ival(o, 0); break;
        case 1438: n.f3d = ival(o, 0); break;
        case 1432: n.oidTask = ival(o, 0); break;
        case 1433: n.iworld = ival(o, 0); break;
        case 1434: n.oidKeyTask = ival(o, 0); break;
        case 1435: n.value = ival(o, 0); break;
        case 1437: n.lineFail = ival(o, 0); break;
        case 1439: n.gStart = gval(o, 0); break;
        case 1440: n.dgStart = gval(o, 0); break;
        case 1441: n.gScaleStart = gval(o, 0); break;
        case 1442: n.gGoal = gval(o, 0); break;
        case 1443: n.cStart = ival(o, 0); break;
        case 1444: n.oidGroup = ival(o, 0); break;
        case 1445: n.gFactor = gval(o, 0); break;
        case 1446: n.fTimer = ival(o, 0) != 0; break;
        case 1447: n.dyHud = gval(o, 0); break;
        case 1448:   // orig3 0x2834D8: a time limit: from 1 down to 0 over it, no goal
            n.gGoal = 0.0f;
            n.gStart = 1.0f;
            n.dtLimit = gval(o, 0);
            n.dgStart = n.dtLimit != 0.0f ? -1.0f / n.dtLimit : 0.0f;
            break;
        case 1449:   // orig3 0x283500: a score: from 0, the goal the score / 1000
            n.gStart = 0.0f;
            n.dgStart = 0.0f;
            n.gScore = gval(o, 0);
            n.gGoal = std::clamp(n.gScore * 0.001f, 0.0f, 1.0f);
            break;
        case 1451: case 1452: case 1453:
        {
            // (type, [object | class], command, value, line)
            NODE3::ENTRY e;
            size_t i = 0;
            e.type = ival(o, i++);
            if (o.eopid == 1452) e.oidKey = ival(o, i++);
            if (o.eopid == 1453) e.cidKey = ival(o, i++);
            e.kind = ival(o, i++);
            e.g = gval(o, i++);
            e.line = ival(o, i++);
            if (n.aentry.size() < 64) n.aentry.push_back(e);
            break;
        }
        case 1456: n.akeyBest = o.aval[0].an; break;
        case 1457: n.akeyDone = o.aval[0].an; break;
        default: break;
        }
    }
    n.gBest = GProgress(n.akeyBest);
    return n;
}

// orig3: 0x00283550: the game's "done" key (+0xB0) set
void MarkNodeDone3(const NODE3& n)
{
    if (n.akeyDone.empty()) return;
    CRef r;
    r.SetBool(true);
    FSetProgressKey(n.akeyDone, r);
}

void StartChallenge3(SW* psw, LO* pnode, int grf, int n) { StartChallengeIn3(psw, pnode, grf, n, -1); }

void StartChallengeIn3(SW* psw, LO* pnode, int grf, int n, int iworldNative)
{
    if (!pnode) return;
    CHALLENGE3& c = s_chal;
    c = CHALLENGE3{};
    c.oidNode = pnode->oid;
    c.mode = int(pnode->U32Field3(0, 0x50, 0));
    c.oidJob = OID(pnode->U32Field3(0, 0x48, 0xFFFFFFFFu));
    c.iworld = int(pnode->U32Field3(0, 0x58, 0xFFFFFFFFu));
    c.oidTask = OID(pnode->U32Field3(0, 0x5C, 0xFFFFFFFFu));
    c.oidKeyTask = OID(pnode->U32Field3(0, 0x60, 0xFFFFFFFFu));
    c.value = int(pnode->U32Field3(0, 0x64, 2));
    c.n14A4 = n;
    g_n3dChoice3 = n;   // (GAME +0x14A4, the 3-D answer: X 0 the normal view, Square's "in 3-D" 1; gauge3.h)
    c.node = NodeFromLo3(pnode);
    c.ploNode = pnode;
    c.pswNode = psw;
    c.cLoadNode = g_cTransitionLoads;
    c.ploHud = nullptr;
    // (+0x34: the current world's id; GAME +0x1498.. the task keys and GAME +0x10's block: the save copy below holds
    // the progress; the keys are the chooser's (none))
    c.iworldReturn = g_transition.iwrecCur >= 0 ? g_transition.iwrecCur : 0;
    c.gsSaved = g_gamesave;
    // the game's "done" key (+0xB0) kept across the reset
    const std::vector<int16_t> akeyDone = AkeyOpt(pnode, 1457);
    CRef refDone;
    if (!akeyDone.empty()) refDone = RefGetProgressKey(akeyDone);
    ClearProgressTaskFields(g_gamesave);   // (0x21C860)
    if (!akeyDone.empty() && refDone.m_tagk != TAGK_Nil) FSetProgressKey(akeyDone, refDone);
    if (c.mode == 2)
    {
        // (the two-player games start with no power-ups: SW +0x4088's 0x43 keys written false)
        for (int i = 0; i < 0x43; ++i)
        {
            CRef r;
            r.SetBool(false);
            FSetProgress({"powerUps", "mppukfAcquired", std::to_string(i)}, r);
        }
    }
    c.fRunning = true;
    // (0x283B98, 0x283F20: the values from the node, state 1; the challenge's world start resets them once more: 0x283A50
    // on the task start's request flag 4 (0x189E70 / 0x189F38), natively fResetAtStart)
    ResetChalValues();
    c.fResetAtStart = true;
    c.fStartFlag = true;
    if (FPrint())
    {
        std::printf("challenge3: start %d (mode %d job %d world %d task %d, end key %d value %d, grf 0x%X)\n", c.oidNode, c.mode,
                    c.oidJob, c.iworld, c.oidTask, c.oidKeyTask, c.value, grf | 0x80);
        const NODE3& d = c.node;
        std::printf("challenge3:   start %.3f rate %.4f scale %.2f goal %.3f tries %d group %d factor %.2f timer %d dy %.1f "
                    "time %.1f score %.0f best %.4f fail line %d, %zu commands\n", d.gStart, d.dgStart, d.gScaleStart, d.gGoal,
                    d.cStart, d.oidGroup, d.gFactor, int(d.fTimer), d.dyHud, d.dtLimit, d.gScore, d.gBest, d.lineFail, d.aentry.size());
        for (const NODE3::ENTRY& e : d.aentry)
            std::printf("challenge3:   on 0x%X (object %d class 0x%X): command %d %.3f line %d\n", e.type, e.oidKey, e.cidKey,
                        e.kind, e.g, e.line);
    }
    if (iworldNative >= 0)
    {
        // (PC, copsmap3.h: Cops and Robbers on another hub: its world at its default point, no task: the native rules)
        c.iworld = iworldNative;
        c.oidTask = -1;
        RequestWorldWarp(psw, iworldNative, sly3::POINT_DEFAULT, -1);
        return;
    }
    // (0x189E70(GAME, the world's record, the task, grf | 0x80))
    RequestTaskStart(psw, c.iworld, c.oidTask);
}

bool FChallengeRunning3() { return s_chal.fRunning; }
bool FChallenge3d3() { return s_chal.fRunning && s_chal.node.f3d != 0; }
const GAMESAVE* PgsChallengeSaved3() { return s_chal.fRunning ? &s_chal.gsSaved : nullptr; }
int NChallengeMode3() { return s_chal.fRunning ? s_chal.mode : -1; }
OID OidChallengeJob3() { return s_chal.fRunning ? s_chal.oidJob : -1; }

void EndChallenge3(SW* psw)
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning) return;
    // (0x283F70: the coins, the captured / sunk counts, the high score, the sound and screen settings, the joysticks'
    // preferences and two of the player's values copied from the current save into the copy, the copy restored
    // (0x284568). Natively the settings live in Settings.ini; the progress's coins / counters are copied by path.)
    GAMESAVE gs = c.gsSaved;
    {
        // (the keys 0x283F70 copies: the coins, the ship battles' sunk / captured counts, the hacking game's high score,
        // the challenges' records and "done" keys (those were written into the copy's progress by 0x284778 / 0x283550:
        // the save the chooser had gets them, as the original's copy of the variables does); the sound / screen /
        // joystick settings are Settings.ini's on the PC)
        static const std::vector<std::vector<std::string>> s_apath = {
            {"loot", "ccoin"}, {"battle", "cCaptured"}, {"battle", "cSunk"}, {"hack_multi", "nHighScore"}};
        std::vector<CRef> aref;
        for (const auto& apath : s_apath) aref.push_back(RefGetProgress(apath));
        const CRef refBest = c.node.akeyBest.empty() ? CRef() : RefGetProgressKey(c.node.akeyBest);
        const CRef refDone = c.node.akeyDone.empty() ? CRef() : RefGetProgressKey(c.node.akeyDone);
        g_gamesave = gs;
        for (size_t i = 0; i < s_apath.size(); ++i)
            if (aref[i].m_tagk != TAGK_Nil) FSetProgress(s_apath[i], aref[i]);
        if (refBest.m_tagk != TAGK_Nil) FSetProgressKey(c.node.akeyBest, refBest);
        if (refDone.m_tagk != TAGK_Nil) FSetProgressKey(c.node.akeyDone, refDone);
    }
    // (0x284568: state 0, the HUD's vt slot 54)
    SetChalState(0);
    if (c.ploHud) HideScreenObj(c.ploHud);
    c.ploHud = nullptr;
    c.fRunning = false;
    if (FPrint()) std::printf("challenge3: end %d, back to world %d\n", c.oidNode, c.iworldReturn);
    // (kind 0x42: 0x289070(GAME, the record's world, 0x1A8, 0x42, -1))
    RequestWorldWarp(psw, c.iworldReturn, sly3::POINT_DEFAULT, -1);
}

// orig3: 0x00284568 (0x189F38: a task of another job than the current one requested while a challenge runs): the
// challenge stops silently: 0x284DC8(router, 0) (state 0: the node slot cleared by 0x283B98, broadcast 0x39) and the
// HUD's vt slot 54 (hidden); no result, no record, the save copy not given back (0x283F70 is not called), no warp
void AbortChallenge3()
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning) return;
    if (FPrint()) std::printf("challenge3: %d stopped by another job's task start\n", c.oidNode);
    SetChalState(0);
    if (c.ploHud) HideScreenObj(c.ploHud);
    c.ploHud = nullptr;
    c.fRunning = false;
}

// The two-player games' score board (class 0xA4, Cops and Robbers' 140111; orig3 its options 1796..1799, Sly 3 EOPIDs
// 2024..2027: 0x111878 -> 0x26A030 an entry {object, 0, "0"} added (two at most, +0x120 the count), 0x111900 ->
// 0x26A088 cleared, 0x111978 -> 0x26A090 the object's value and its text ("%d", D_00327ED0) set, 0x111A20 -> 0x26A0E0
// the object's value (0 when it has none))
namespace
{
struct SCOREENTRY3 { const LO* plo = nullptr; int value = 0; };
std::unordered_map<const LO*, std::vector<SCOREENTRY3>> s_mpscore3;
const LO* s_pboard3 = nullptr;   // (the board last used: its texts are the script strings 0x360 / 0x361, orig3 0x26A150)
const LO* PloArg3(const std::vector<CRef>& a, size_t i)
{
    return i < a.size() && a[i].m_tagk == TAGK_Basic ? dynamic_cast<const LO*>(a[i].m_pbasic) : nullptr;
}
struct INITSCORE3
{
    INITSCORE3()
    {
        // orig3 0x26A150 (the board's vt slot 55: the script strings 0x360 / 0x361 are its entries' texts, "%d")
        for (int i = 0; i < 2; ++i)
            SetScriptString3(0x360 + i, [i] {
                if (std::string s; duel3::FScoreString(&s)) return s;   // (PC, duel3.h: the free-for-all's kills per view)
                const auto it = s_mpscore3.find(s_pboard3);
                if (it == s_mpscore3.end() || size_t(i) >= it->second.size()) return std::string();
                return std::to_string(it->second[size_t(i)].value);
            });
        RegisterObjectMethod3(0x111878, [](LO* plo, const std::vector<CRef>& a) {
            s_pboard3 = plo;
            std::vector<SCOREENTRY3>& v = s_mpscore3[plo];
            if (v.size() < 2) v.push_back({PloArg3(a, 0), 0});
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111900, [](LO* plo, const std::vector<CRef>&) {
            s_mpscore3[plo].clear();
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111978, [](LO* plo, const std::vector<CRef>& a) {
            const LO* p = PloArg3(a, 0);
            for (SCOREENTRY3& e : s_mpscore3[plo])
                if (e.plo == p && a.size() > 1) e.value = a[1].RefCoerceS32().m_n;
            if (FPrint()) std::printf("challenge3: score of %d = %d\n", p ? p->oid : -1, a.size() > 1 ? a[1].RefCoerceS32().m_n : -1);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111A20, [](LO* plo, const std::vector<CRef>& a) {
            const LO* p = PloArg3(a, 0);
            int n = 0;
            for (const SCOREENTRY3& e : s_mpscore3[plo])
                if (e.plo == p) n = e.value;
            CRef r;
            r.SetS32(n);
            return r;
        });
    }
} s_initscore3;
}  // namespace

int NScore3(const LO* pboard, const LO* plo)
{
    const auto it = s_mpscore3.find(pboard);
    if (it != s_mpscore3.end())
        for (const SCOREENTRY3& e : it->second)
            if (e.plo == plo) return e.value;
    return -1;
}

void SetScoreEntries3(LO* pboard, LO* ploA, LO* ploB)
{
    if (!pboard) return;
    s_pboard3 = pboard;
    std::vector<SCOREENTRY3>& v = s_mpscore3[pboard];
    v.clear();
    v.push_back({ploA, 0});
    v.push_back({ploB, 0});
}

void SetScore3(LO* pboard, const LO* plo, int n)
{
    for (SCOREENTRY3& e : s_mpscore3[pboard])
        if (e.plo == plo) e.value = n;
}

void ResetScores3()
{
    s_mpscore3.clear();
    s_pboard3 = nullptr;
}

// orig3: 0x00284A90 (the challenge manager's command: the world's option 759 of bif 125, a node's entries): 0 the value
// set, 1 its rate, 3 the factor, 2 the value + g x factor, 4 / 5 a time (s) set / added (/ +0x9C), 6 / 7 a score set /
// added (/ 1000), 8 the counter set, 9 one down (the broadcast 0x33; at 0 as 10), 10 the job failed with the line
// (0x186EE0(job, line, -1, 0x33)), 11 / 12 the job complete (0x1871C0)
void ChallengeCommand3(int k, float g, int line)
{
    CHALLENGE3& c = s_chal;
    if (FPrint()) std::printf("challenge3: command %d (%.3f, line %d)\n", k, g, line);
    c.lineEnd = line;
    JOB3* pj = g_tasks3.pjobCur;
    switch (k)
    {
    case 0: SetChalValue(g); break;
    case 1: c.dg = g; break;
    case 3: c.gScale = g; break;
    case 2: SetChalValue(c.g + g * c.gScale); break;
    case 4: if (c.node.dtLimit != 0.0f) SetChalValue(g / c.node.dtLimit); break;
    case 5: if (c.node.dtLimit != 0.0f) SetChalValue(c.g + g / c.node.dtLimit); break;
    case 6: SetChalValue(g * 0.001f); break;
    case 7: SetChalValue(std::floor(c.g * 1000.0f + g + 0.5f) * 0.001f); break;
    case 8: c.cLeft = int(g); break;
    case 9:
        --c.cLeft;
        Broadcast(0x33);
        if (c.cLeft > 0) break;
        [[fallthrough]];
    case 10:
        c.cLeft = -1;
        if (pj) JobEndFail3(pj, line);
        break;
    case 11: case 12:
        if (pj) JobCompleteNow3(pj);
        break;
    default: break;
    }
}

// orig3: 0x002849A8 (the last of a broadcast's receivers, 0x1854C8): the node's message 0x14 {code, object}, then each of
// its commands whose type is the code (and object / class match: 0x283340) runs
void ChallengeOnEvent3(int code, LO* plo)
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning) return;
    if (LO* pnode = PloNodeCur(); pnode && g_taskhost3.pfnMessageArgs) g_taskhost3.pfnMessageArgs(pnode, 0x14, code, plo);
    const std::vector<NODE3::ENTRY> a = c.node.aentry;   // (a command may end the game)
    for (const NODE3::ENTRY& e : a)
    {
        if (!c.fRunning) break;
        if (e.type != code) continue;
        if (e.oidKey != -1 && (!plo || (plo->oid != e.oidKey && plo->oidInstance != e.oidKey))) continue;   // (0x1C8768)
        if (e.cidKey != -1 && (!plo || !FIsLoDerivedFrom(plo, CID(e.cidKey)))) continue;                     // (0x13B500)
        ChallengeCommand3(e.kind, e.g, e.line);
    }
}

// orig3: 0x002848D0 (the game's update): while one runs, nothing pauses the players and no world change is pending: the
// value moves at its rate in state 1; the HUD's dial (part 1000) eases toward it (0x11E598, SMP {100, 0, 0.5})
void UpdateChallenge3(SW* psw, bool fPaused)
{
    CHALLENGE3& c = s_chal;
    if (static const bool s_f2 = std::getenv("SLY3_CHALPRINT") && std::getenv("SLY3_CHALPRINT")[0] == '2'; s_f2 && c.fRunning)
    {
        static int s_c = 0;
        if (++s_c % 60 == 0)
            std::printf("challenge3: frame value %.4f shown %.4f rate %.4f state %d paused %d pending %d hud %d\n", c.g, c.gShown,
                        c.dg, c.state, int(fPaused), int(g_transition.fPending), c.ploHud ? int(FScreenObjVisible(c.ploHud)) : -1);
    }
    if (!c.fRunning || !psw || fPaused || g_transition.fPending) return;
    const float dt = g_clock.dt;
    if (c.state == 1) SetChalValue(c.g + c.dg * dt);
    if (c.ploHud && FScreenObjVisible(c.ploHud))
    {
        static const SMP s_smp = {100.0f, 0.0f, 0.5f};
        c.gShown = GSmooth(c.gShown, c.g, dt, &s_smp, nullptr);
        SetScreenObjFraction(c.ploHud, 1000, c.gShown);
    }
}

// orig3: 0x00283970 (a world's reset, 0x25B860): the HUD 1054 and the node found in the new world; the HUD moved down by
// the node's +0x98
void ChallengeWorldLoaded3(SW* psw)
{
    CHALLENGE3& c = s_chal;
    c.ploHud = psw ? PloScreen3(psw, 1054) : nullptr;
    c.pswNode = nullptr;
    if (PloNodeCur()) c.node = NodeFromLo3(c.ploNode);
    if (c.ploHud && c.fRunning && c.node.dyHud != 0.0f) SetScreenObjRootXY(c.ploHud, true, 1.0f, 0.0f, c.node.dyHud);
}

// orig3: 0x00283A50 (the world's start, 0x184B60 first): a challenge started anew (the start's / the replay's task
// request, flag 4) gets its values again (0x283F20) unless it is a job replay (mode 1); a challenge (mode 0) shows its HUD:
// part 995, the score's 997 or the timer's 996 and the dial 1000 at the value
void ChallengeWorldStart3(SW* psw)
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning) return;
    if (c.fResetAtStart && c.mode != 1) ResetChalValues();
    c.fResetAtStart = false;
    (void)psw;
    if (!c.ploHud || c.mode != 0) return;
    ShowScreenObj(c.ploHud, -1);
    SelectScreenObjPart(c.ploHud, 995);
    if (c.node.gScore != 0.0f)
    {
        SelectScreenObjPart(c.ploHud, 997);
        return;
    }
    if (c.node.dtLimit != 0.0f || c.node.fTimer)
    {
        SelectScreenObjPart(c.ploHud, 996);
        SelectScreenObjPart(c.ploHud, 1000);
        c.gShown = c.g;
        SetScreenObjFraction(c.ploHud, 1000, c.g);
    }
}

void ChallengeRestart3() { if (s_chal.fRunning) s_chal.fResetAtStart = true; }

// orig3: 0x002846A8 (the job complete, a challenge or a two-player game): the result's state: 5 a two-player game; out
// of tries 2; short of the goal 3; better than the record 6; else 4
void EvaluateChallenge3()
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning || (c.mode != 0 && c.mode != 2)) return;
    if (c.mode == 2) SetChalState(5);
    else if (c.cLeft <= 0) SetChalState(2);
    else if (c.node.gGoal != 0.0f && c.g < c.node.gGoal) SetChalState(3);
    else if (c.node.gBest < c.g) SetChalState(6);
    else SetChalState(4);
}

// orig3: 0x00284778: a challenge's new record kept (+0xA4, written to the key +0xA8)
void SaveChallengeBest3()
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning || c.mode != 0 || !(c.node.gBest < c.g)) return;
    c.node.gBest = c.g;
    if (c.node.akeyBest.empty()) return;
    CRef r;
    r.SetF32(c.g);
    FSetProgressKey(c.node.akeyBest, r);
}

// orig3: 0x00284658 (a challenge's job failed: the result screen): still running (1) -> short of the goal (3)
void ChallengeJobFailed3()
{
    if (s_chal.fRunning && s_chal.mode == 0 && s_chal.state == 1) SetChalState(3);
}

int NChallengeState3() { return s_chal.fRunning ? s_chal.state : 0; }
const NODE3* PnodeChallenge3() { return s_chal.fRunning ? &s_chal.node : nullptr; }
// (the splice's option 1344 of the manager, bif 125: +0x14 the game object; null without a running game)
LO* PloChallengeNode3() { return PloNodeCur(); }

// orig3: 0x00284828: the item count of an object of the node's group (+0x8C) scaled by its factor (+0x90), at least 1
int CChallengeScaled3(const LO* plo, int c)
{
    const CHALLENGE3& k = s_chal;
    if (!k.fRunning || k.node.oidGroup == -1 || !plo) return c;
    if (plo->oid != k.node.oidGroup && plo->oidInstance != k.node.oidGroup) return c;
    const int n = int(k.node.gFactor * float(c) + 0.5f);
    return c > 0 ? std::min(n, c) : 1;
}

// The challenges' script strings (orig3 0x47A4B8): 998 / 999 (1054: the goal and the score), 1110..1112 (1066's result:
// 0x284F38 / 0x284FC8 / 0x285220), 1487 / 1498 (the chooser's record lines: 0x17CA18 / 0x17CDE0)
namespace
{
std::string StrText(int id) { return id >= 0 ? StrText3(g_psw, id) : std::string(); }
// orig3 0x0017CDE0's / 0x0017CA18's line of a node: the hacking game's high score (mode 2, its task 1815), a done
// challenge's record ("High Score :" / "Best Time :")
std::string StrRecord(const NODE3& n, bool fChallenges)
{
    char ach[32];
    if (n.mode == 2 && n.oidTask == 1815)
    {
        const int v = NRefC(RefGetProgress({"hack_multi", "nHighScore"}));
        if (v <= 0) return "";
        std::snprintf(ach, sizeof(ach), "%d", v);
        return StrText(494) + " " + ach;
    }
    if (!fChallenges || n.mode != 0 || n.akeyDone.empty() || !NRefC(RefGetProgressKey(n.akeyDone))) return "";
    if (n.gScore != 0.0f) return StrText(494) + " " + StrChalValue(n, n.gBest);
    if (n.dtLimit != 0.0f) return StrText(498) + " " + StrChalValue(n, n.gBest);
    return "";
}
struct INITCHALSTRINGS
{
    INITCHALSTRINGS()
    {
        // orig3 0x00285110 / 0x00285198: a challenge with a score (+0xA0): its score / its goal (+0x84)
        SetScriptString3(999, [] {
            const CHALLENGE3& c = s_chal;
            return c.fRunning && c.node.mode == 0 && c.node.gScore != 0.0f ? StrChalValue(c.node, c.g) : std::string();
        });
        SetScriptString3(998, [] {
            const CHALLENGE3& c = s_chal;
            return c.fRunning && c.node.mode == 0 && c.node.gScore != 0.0f ? StrChalValue(c.node, c.node.gGoal) : std::string();
        });
        // orig3 0x00284F38: "Challenge Failed" (2 / 3), "Challenge Complete!" (4 / 6), the two-player end's line (5)
        SetScriptString3(1110, [] {
            const CHALLENGE3& c = s_chal;
            if (!c.fRunning) return std::string();
            switch (c.state)
            {
            case 2: case 3: return StrText(0x1EC);
            case 4: case 6: return StrText(0x1EB);
            case 5: return StrText(c.lineEnd);
            default: return std::string();
            }
        });
        // orig3 0x00284FC8: failed: the line (+0x30, else the job's failure line, else the node's +0x6C); done: "Your
        // Score :" / "Your Time :" and the value, else "You did it! Congratulations!"
        SetScriptString3(1111, [] {
            const CHALLENGE3& c = s_chal;
            if (!c.fRunning) return std::string();
            switch (c.state)
            {
            case 2: case 3:
            {
                int id = c.lineEnd;
                if (id == -1 && g_tasks3.pjobCur) id = g_tasks3.pjobCur->idFail;
                if (id == -1) id = c.node.lineFail;
                return StrText(id);
            }
            case 4: case 6:
                if (c.node.gScore != 0.0f) return StrText(0x1EF) + StrChalValue(c.node, c.g);
                if (c.node.dtLimit != 0.0f) return StrText(0x1F3) + StrChalValue(c.node, c.g);
                return StrText(0x1F4);
            default: return std::string();
            }
        });
        // orig3 0x00285220: a new record (6): "New High Score!" / "New Best Time!"
        SetScriptString3(1112, [] {
            const CHALLENGE3& c = s_chal;
            if (!c.fRunning || c.state != 6) return std::string();
            if (c.node.gScore != 0.0f) return StrText(0x1ED);
            if (c.node.dtLimit != 0.0f) return StrText(0x1F1);
            return std::string();
        });
        // orig3 0x0017CA18 / 0x0017CDE0: the chooser's row (its node), else the running game's
        SetScriptString3(1487, [] {
            const LO* p = PnodeEpMenuCur3();
            if (p) return StrRecord(NodeFromLo3(p), false);
            return s_chal.fRunning ? StrRecord(s_chal.node, false) : std::string();
        });
        SetScriptString3(1498, [] {
            const LO* p = PnodeEpMenuCur3();
            if (p) return StrRecord(NodeFromLo3(p), true);
            return s_chal.fRunning ? StrRecord(s_chal.node, true) : std::string();
        });
    }
} s_initchalstrings;
}  // namespace

int LineChallengeEnd3() { return s_chal.fRunning ? s_chal.lineEnd : -1; }

bool FChallengeEndTask3(const TASK3* ptask, int value)
{
    const CHALLENGE3& c = s_chal;
    if (!c.fRunning || !ptask || !ptask->pjob || ptask->pjob->oid != c.node.oidJob || c.node.value != value) return false;
    if (c.node.oidKeyTask != -1) return c.node.oidKeyTask == ptask->oid;
    return g_tasks3.pjobCur && g_tasks3.pjobCur->ptaskLast == ptask;
}

bool FChallengeCheckpoints3() { return !s_chal.fRunning || s_chal.mode == 1; }

bool FChallengeTakeStartFlag3(int* pn)
{
    CHALLENGE3& c = s_chal;
    if (!c.fRunning || !c.fStartFlag) return false;
    c.fStartFlag = false;
    *pn = c.mode == 1 ? 0 : 1;   // (0x186338: mode 1 -> 0x1862D0(job, 0), modes 0 / 2 -> 1)
    return true;
}
