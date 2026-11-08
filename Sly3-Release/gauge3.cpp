// See gauge3.h. Ported from the Sly 3 asm (orig3 addresses on each function).

#include "gauge3.h"

#include "card3.h"

#include "actor.h"
#include "clock.h"
#include "cls.h"
#include "damage.h"
#include "gadget.h"
#include "savedata.h"
#include "sound.h"
#include "lo.h"
#include "screen3.h"
#include "screenrt.h"
#include "sw.h"
#include "vecmat.h"
#include "worldaccess.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>


namespace
{
constexpr int CID_GAUGE3 = 0xA3;
constexpr int PART_HIT = 0x3C4, PART_STATE5 = 0x3C5, PART_STATE6 = 0x3C6, PART_HEALTH = 0x3CA, PART_POWER = 0x3CB;
constexpr cmm::SMP s_smpHealth{100.0f, 0.0f, 0.5f};   // orig3 0x349020
constexpr cmm::SMP s_smpPower{100.0f, 0.0f, 0.5f};    // orig3 0x349030

struct GAUGE3
{
    LO* plo = nullptr;
    LO* player = nullptr;     // +0x13C (the parent player)
    int state = -1;           // +0x138
    float hShown = 0.0f;      // +0x120
    float hTarget = 0.0f;     // +0x124
    float gShown = 0.0f;      // +0x128
    float gTarget = 0.0f;     // +0x12C
    int idHandle = -1;        // +0x140 (the part selected for the state)
    int idFace = -1;          // +0x144 (the face's part: 0x7AF, the two-player game's per character)
};
std::map<LO*, GAUGE3> s_mpgauge;
LO* s_ploShown = nullptr;     // the gauge shown (the current player's)
SW* s_psw = nullptr;
uint32_t s_serial = 0;

bool FDefaultValues(LO* player, GAUGEVALUES3* pv)
{
    ACTOR* actor = player ? PactorFromLo(player) : nullptr;
    int* health = actor ? DmgCounterOf(actor) : nullptr;
    if (!health || DmgCapacityOf(actor) <= 0) return false;
    pv->cHit = *health;
    pv->cHitMax = DmgCapacityOf(actor);
    pv->nPower = NActorPower(actor);
    pv->nPowerMax = actor->n1300;
    return true;
}

// orig3 0x0021E028: the player's fractions (+0x124 the health, +0x12C the power)
void ReadFractions(GAUGE3& g)
{
    GAUGEVALUES3 v;
    if (!g_pfnGaugeValues3(g.player, &v))
    {
        // (no values: a full gauge, as a player that cannot be hurt; the gadget bar empty)
        g.hTarget = 1.0f;
        g.gTarget = 0.0f;
        return;
    }
    g.hTarget = v.cHitMax > 0 ? float(v.cHit) / float(v.cHitMax) : 0.0f;
    g.gTarget = v.nPowerMax > 0 ? float(v.nPower) / float(v.nPowerMax) : 0.0f;
}

void SelectHandle(GAUGE3& g, int id)
{
    SelectScreenObjPartHandle(g.plo, id, &g.idHandle);   // (orig3 0x262B60 on +0x140)
}

// orig3 0x0021DF20
void SetGaugeState(GAUGE3& g, int n)
{
    if (n == g.state) return;
    if (g.state == -1) g.idFace = -1;   // (shown again: the face selected again)
    g.state = n;
    switch (n)
    {
    case -1: HideScreenObj(g.plo); break;   // (vt slot 54: the base hide)
    case 1:
        ReadFractions(g);
        g.hShown = g.hTarget;
        g.gShown = g.gTarget;
        SelectScreenObjPart(g.plo, PART_HEALTH);
        SetScreenObjFraction(g.plo, PART_HEALTH, g.hShown);
        SelectScreenObjPart(g.plo, PART_POWER);
        SetScreenObjFraction(g.plo, PART_POWER, g.gShown);
        SelectHandle(g, -1);
        break;
    case 2: SelectHandle(g, PART_HIT); break;
    case 5: SelectHandle(g, PART_STATE5); break;
    case 6: SelectHandle(g, PART_STATE6); break;
    default: SelectHandle(g, -1); break;
    }
}

// orig3 0x0021E158 (the state's step, again while it changes)
void StepGauge(GAUGE3& g)
{
    const float dt = g_clock.dt;
    for (int guard = 0; guard < 8; ++guard)
    {
        const int s = g.state;
        switch (s)
        {
        case 0:
        {
            const int st = NScreenObjState(g.plo);
            if (st == 2 || st == 3) SetGaugeState(g, 1);
            break;
        }
        case 1:
        case 5:
        case 6:
            if (g.hTarget <= 0.0f) SetGaugeState(g, -1);
            else if (g.hTarget < g.hShown) SetGaugeState(g, 2);
            else if (g.hShown < g.hTarget) SetGaugeState(g, 3);
            else if (g.gTarget != g.gShown) SetGaugeState(g, 4);
            // (the player's states 0x3C6 (+0x11C4 == 4) / 0x3C5 (+0x11A0 != -1): TODO(sly3) the Sly 3 player's fields)
            break;
        case 2:
            if (!FScreenObjBusy(g.plo, PART_HIT)) SetGaugeState(g, 3);
            break;
        case 3:
            g.hShown = g.hShown < g.hTarget ? g.hTarget : cmm::GSmooth(g.hShown, g.hTarget, dt, &s_smpHealth, nullptr);
            SetScreenObjFraction(g.plo, PART_HEALTH, g.hShown);
            if (g.hShown == g.hTarget) SetGaugeState(g, 1);
            break;
        case 4:
            g.gShown = g.gShown < g.gTarget ? g.gTarget : cmm::GSmooth(g.gShown, g.gTarget, dt, &s_smpPower, nullptr);
            SetScreenObjFraction(g.plo, PART_POWER, g.gShown);
            if (g.gShown == g.gTarget) SetGaugeState(g, 1);
            break;
        default: break;
        }
        if (g.state == s) break;
    }
}

// orig3 0x0021E3B8: the face (part 0x7AF = 1967, group 4) once the gauge is up; the two-player game (GAME 0x36DBE0 == 2)
// picks the character's (the table 0x343098) and plays its line: not natively
void UpdateFace(GAUGE3& g)
{
    if (g.state == -1 || g.state == 0) return;
    constexpr int PART_FACE = 0x7AF;
    if (g.idFace != PART_FACE) SelectScreenObjPartHandle(g.plo, PART_FACE, &g.idFace);
}

GAUGE3* PgaugeOfPlayer(LO* player)
{
    if (!player) return nullptr;
    for (auto& [plo, g] : s_mpgauge)
        if (g.player == player) return &g;
    return nullptr;
}
}  // namespace

// ------------------------------------------------------------ the coin counter (class 0xA7: m_ext's 1048)
namespace
{
constexpr int CID_COINS3 = 0xA7;
struct COINS3
{
    LO* plo = nullptr;
    int cShown = 0;     // +0x120
    float tNext = 0;    // +0x124 (the next step's time)
};
std::vector<COINS3> s_acoins;
uint32_t s_serialCoins = 0;
bool s_fCoinsKnown = false;

// orig3 0x001852E0 CCoins: the save's coins (NProgress on the key 0x479504: "loot" "ccoin")
int CCoins3()
{
    const CRef r = RefGetProgress({"loot", "ccoin"});
    return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : 0;
}
}  // namespace

// orig3: 0x0026A1F8 (the counter's update, after the base update): counting up to the coins, a step every 0.13 s (1, 10
// or 100 coins by the difference, the sound 150 each); behind a higher priority (0x262128) or with nothing to count, it
// shows the coins at once (0x26A1C8). (Natively it shows itself when the coins grow: the pickups' show, orig3 vt slot
// 53 from the coin code, is not wired to Sly 3's pickups yet.)
void UpdateCoins3(SW* psw)
{
    if (!psw) return;
    if (psw->serial != s_serialCoins)
    {
        s_serialCoins = psw->serial;
        s_acoins.clear();
        for (LO* plo : AploOfClass(psw, CID_COINS3)) s_acoins.push_back(COINS3{plo, CCoins3(), 0.0f});
        s_fCoinsKnown = false;
    }
    const int c = CCoins3();
    for (COINS3& k : s_acoins)
    {
        const int d = c - k.cShown;
        if (d > 0 && !FScreenObjVisible(k.plo) && s_fCoinsKnown) ShowScreenObj(k.plo, -1);
        if (d <= 0 || !FScreenObjVisible(k.plo)) { if (!FScreenObjVisible(k.plo) || d < 0) k.cShown = c; continue; }
        if (k.tNext >= g_clock.t) continue;
        static const struct { int n; int sfx; float dt; } s_astep[3] = {{1, 150, 0.13f}, {10, 150, 0.14f}, {100, 150, 0.15f}};
        const auto& st = s_astep[d >= 201 ? 2 : d >= 21 ? 1 : 0];
        k.cShown += st.n;
        k.tNext = g_clock.t + st.dt;
        PsndStartSound(st.sfx);
        RefreshScreenObj(k.plo);
    }
    s_fCoinsKnown = true;
}
int CCoinsShown3(const LO* plo)
{
    for (const COINS3& k : s_acoins)
        if (k.plo == plo) return k.cShown;
    return CCoins3();
}

bool (*g_pfnGaugeValues3)(LO* player, GAUGEVALUES3* pv) = FDefaultValues;
bool g_fGaugeFollowsPlayer3 = true;

void ResetGauges3()
{
    s_mpgauge.clear();
    s_ploShown = nullptr;
    s_psw = nullptr;
}

// orig3 0x0021DEE8 (each gauge's init: hidden)
void InitGauges3(SW* psw)
{
    ResetGauges3();
    s_psw = psw;
    if (!psw) return;
    s_serial = psw->serial;
    for (LO* plo : AploOfClass(psw, CID_GAUGE3))
    {
        GAUGE3 g;
        g.plo = plo;
        g.player = plo->paloParent;
        s_mpgauge[plo] = g;
    }
}

// orig3 0x001F4548 (a pad takes / leaves the player): the gauge shown (slot 56: 0x21E558, state 0 then the base show
// 0x260E30) / hidden (slot 54: 0x21E4F0)
void SetGaugePlayer3(LO* player)
{
    GAUGE3* pg = PgaugeOfPlayer(player);
    LO* plo = pg ? pg->plo : nullptr;
    if (plo == s_ploShown) return;
    if (s_ploShown)
        if (auto it = s_mpgauge.find(s_ploShown); it != s_mpgauge.end()) SetGaugeState(it->second, -1);
    s_ploShown = plo;
    if (!pg) return;
    SetGaugeState(*pg, 0);
    ShowScreenObj(plo, -1);
}

// orig3 0x0021E0C8 (the update of each shown gauge, after the base update)
void UpdateGauges3(SW* psw, LO* player)
{
    if (psw != s_psw || (psw && psw->serial != s_serial)) InitGauges3(psw);
    if (!psw) return;
    static const bool s_fOff = std::getenv("SLY3_NOGAUGE") != nullptr;
    // (debug: SLY3_GAUGEPLAYER=oid the gauge of that player, as if a pad had taken it)
    if (static const char* s_sz = std::getenv("SLY3_GAUGEPLAYER"); s_sz)
        for (auto& [plo, g] : s_mpgauge)
            if (g.player && g.player->oid == std::atoi(s_sz)) player = g.player;
    if (g_fGaugeFollowsPlayer3) SetGaugePlayer3(s_fOff || FCardMessageShown3() ? nullptr : player);
    for (auto& [plo, g] : s_mpgauge)
    {
        if (g.state == -1) continue;
        ReadFractions(g);
        StepGauge(g);
        UpdateFace(g);
    }
    if (static const bool s_fPrint = std::getenv("SLY3_GAUGEPRINT") != nullptr; s_fPrint)
        for (auto& [plo, g] : s_mpgauge)
            if (g.state != -1)
                std::printf("gauge3 %d (player %d): state %d health %.2f / %.2f power %.2f / %.2f\n", plo->oid, g.player ? g.player->oid : -1, g.state,
                            g.hShown, g.hTarget, g.gShown, g.gTarget);
}
