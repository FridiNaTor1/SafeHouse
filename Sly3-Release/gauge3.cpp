// See gauge3.h. Ported from the Sly 3 asm (orig3 addresses on each function).

#include "gauge3.h"

#include "card3.h"
#include "game/items3.h"
#include "disc.h"
#include "aicarm3.h"

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
#include "display.h"
#include "sw.h"
#include "vecmat.h"
#include "worldaccess.h"
#include "objmethod.h"
#include "spliceglue.h"
#include "frontend.h"
#include "game/scptgame3.h"
#include "guard.h"
#include "splitscreen3.h"
#include "game/sly3.h"
#include "game/biplane3.h"
#include "game/task3.h"
#include "challenge3.h"
#include "ai.h"
#include "epmenu3.h"

#include <algorithm>
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
// (s3-hack) an SO's gauge, Sly 3 EOPID 948 (optid 949, the SO's +0x168, an object of class 0xA3): e.g. h_hack's two ships
// name their gauges this way (92988 Bentley's, 92986 / 92984 the second player's: the job's script), the gauges having
// no parent there. The gauge's player (+0x13C) is the SO naming it, else its parent.
std::map<const LO*, LO*> s_mpgaugeOwner;   // gauge -> its SO (cleared with the world: ResetGauges3 from hudwidget.cpp)
struct GAUGESLOT3 { float u = 0.0f, v = 0.0f; };
std::map<const LO*, GAUGESLOT3> s_mpgaugeSlot;   // (s3-hack) a gauge moved into its player's corner (SetGaugeSlot3)
LO* s_ploShown = nullptr;     // the gauge shown (the current player's)
SW* s_psw = nullptr;
uint32_t s_serial = 0;

bool FDefaultValues(LO* player, GAUGEVALUES3* pv)
{
    // (a guard player: Carmelita in the two-player games: her health +0x16C / +0x170 = n33C / n338, no gadget power)
    if (const GUARD* pgd = player && !PactorFromLo(player) ? PguardFromLo(player) : nullptr; pgd && pgd->n338 > 0)
    {
        pv->cHit = pgd->n33C;
        pv->cHitMax = pgd->n338;
        pv->nPower = 0;
        pv->nPowerMax = 0;
        return true;
    }
    // (s3-plane: a plane, class 0x52: the SO's own fields as 0x1F43C8 / 0x1F43F0 / 0x1F4430 / 0x1F4458 read them: the hit
    // points +0x16C / +0x170, the ammunition +0x174 / +0x178: the 2P duel's gauges 96161 / 96163)
    if (player && FPlane3(player))
    {
        pv->cHit = int(player->U32Field(0, 0x16C, 0));
        pv->cHitMax = int(player->U32Field(0, 0x170, 0));
        pv->nPower = int(player->U32Field(0, 0x174, 0));
        pv->nPowerMax = int(player->U32Field(0, 0x178, 0));
        return pv->cHitMax > 0;
    }
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
            else
            {
                // orig3 0x21E158: a player (class 0x33) with a guard in reach (+0x11C4 == 4: the native's n10EC, the
                // target's kind) plays 0x3C6 (state 6), else one the guards hear (+0x11A0 != -1: n10C8, the noise
                // meter's state -1 / 0 / 1, s3-ai) 0x3C5 (state 5): the gauge's detection colour
                ACTOR* pa = g.player && FCidDerivedFrom(g.player->Cid(), S3CID(0x34)) ? PactorFromLo(g.player) : nullptr;
                SetGaugeState(g, pa && pa->n10EC == 4 ? 6 : pa && pa->n10C8 != -1 ? 5 : 1);
            }
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

// orig3 0x0021E3B8: the face (part 0x7AF = 1967, group 4) once the gauge is up; in the two-player game (GAME 0x36DBE0
// == 2) the held power-up's: the item table 0x343098 (0x3C bytes a row) by the player's item (an actor's +0x1250: its first
// slot; Carmelita's +0x18B0: her power-up 0x32..0x35), the row's +0x28 the face part (0 / -1: the default), and on a new
// face the row's line for the player's index (+0x130): {+0x2C base, +0x30 count, +0x31 cursor} per index, the lines in turn
constexpr uint32_t VA_ITEMS3 = 0x343098, CB_ITEM3 = 0x3C;
int IitemOfPlayer(SW* psw, LO* player)
{
    if (!player || !psw || psw->nScreenMode3 != 2) return -1;
    if (FCidDerivedFrom(player->Cid(), S3CID(0x34)))
    {
        const int id2 = IdActorAbilitySlot(PactorFromLo(player), 0);
        return id2 < 0 ? -1 : sly3::Item3FromItem2(id2);
    }
    if (PguardFromLo(player)) return KindCarmPower3(player);
    return -1;
}
int PartFaceOfItem(int iitem)
{
    if (iitem < 0 || iitem >= 0x80) return 0x7AF;
    const int part = int(g_disc.U32Elf(VA_ITEMS3 + CB_ITEM3 * uint32_t(iitem) + 0x28));
    return unsigned(part + 1) >= 2u ? part : 0x7AF;
}
void UpdateFace(GAUGE3& g)
{
    if (g.state == -1 || g.state == 0) return;
    const int iitem = IitemOfPlayer(s_psw, g.player);
    const int part = PartFaceOfItem(iitem);
    if (g.idFace == part) return;
    SelectScreenObjPartHandle(g.plo, part, &g.idFace);
    // the row's line for the player's index (the two rows: player 1 / 2; PC teams: by team)
    int iplayer = -1;
    for (int i = 0; i < split3::CView(); ++i)
        if (split3::PloPlayer(s_psw, i) == g.player) iplayer = i & 1;
    if (part == 0x7AF || iplayer < 0) return;
    static std::map<int, uint8_t> s_mpcursor;
    const uint32_t va = VA_ITEMS3 + CB_ITEM3 * uint32_t(iitem) + 0x2C + 8u * uint32_t(iplayer);
    const int base = int(g_disc.U32Elf(va));
    const uint32_t w = g_disc.U32Elf(va + 4);
    const int c = int(w & 0xFF);
    if (c <= 0) return;
    uint8_t& cur = s_mpcursor[int(va)];
    const int id = base + cur % c;
    cur = uint8_t((cur + 1) % c);
    PsndStartDialog(id);
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

// (s3-plane, duel3.h: the free-for-all's players 3 / 4 take a gauge copy of the plane they fly)
void SetGaugeOwner3(LO* plo, LO* player)
{
    const auto it = s_mpgauge.find(plo);
    if (it == s_mpgauge.end() || it->second.player == player) return;
    for (auto& [p, g] : s_mpgauge)   // (one gauge per player)
        if (player && g.player == player && p != plo)
        {
            if (g.state != -1) SetGaugeState(g, -1);
            g.player = nullptr;
        }
    if (it->second.state != -1) SetGaugeState(it->second, -1);
    it->second.player = player;
}

LO* PloGaugeOf3(LO* player)
{
    GAUGE3* pg = PgaugeOfPlayer(player);
    return pg ? pg->plo : nullptr;
}

LO* PloGaugePlayer3(LO* plo)
{
    const auto it = s_mpgauge.find(plo);
    return it == s_mpgauge.end() ? nullptr : it->second.player;
}

namespace
{
// the extra players' shown values (views 3 / 4), smoothed as the gauge's states 3 / 4 do
struct VIEWGAUGE { float h = 1.0f, g = 0.0f; bool fInit = false; };
std::map<const LO*, VIEWGAUGE> s_mpviewgauge;
void StepViewGauges(SW* psw)
{
    if (!split3::FActive() || split3::CView() < 3) { s_mpviewgauge.clear(); return; }
    for (int i = 2; i < split3::CView(); ++i)
    {
        LO* plo = split3::PloPlayer(psw, i);
        if (!plo) continue;
        GAUGE3 tmp;
        tmp.player = plo;
        ReadFractions(tmp);
        VIEWGAUGE& v = s_mpviewgauge[plo];
        if (!v.fInit) v.h = tmp.hTarget, v.g = tmp.gTarget, v.fInit = true;
        const float dt = g_clock.dt;
        v.h = v.h < tmp.hTarget ? tmp.hTarget : cmm::GSmooth(v.h, tmp.hTarget, dt, &s_smpHealth, nullptr);
        v.g = v.g < tmp.gTarget ? tmp.gTarget : cmm::GSmooth(v.g, tmp.gTarget, dt, &s_smpPower, nullptr);
    }
}
}  // namespace

void ApplyViewGauges3(SW* psw, int iview)
{
    if (!psw || !split3::FActive() || split3::CView() < 3 || psw->nScreenMode3 != 2) return;
    for (auto& [plo, g] : s_mpgauge)
    {
        if (g.state == -1) continue;
        int k = -1;
        for (int i = 0; i < 2; ++i)
            if (g.player && g.player == split3::PloPlayer(psw, i)) k = i;
        if (k < 0) continue;
        float h = g.hShown, gp = g.gShown;
        if (iview >= 2 && (iview & 1) == k)
            if (LO* pv = split3::PloPlayer(psw, iview))
                if (auto it = s_mpviewgauge.find(pv); it != s_mpviewgauge.end()) h = it->second.h, gp = it->second.g;
        SetScreenObjFraction(plo, PART_HEALTH, h);
        SetScreenObjFraction(plo, PART_POWER, gp);
    }
}
bool g_fGaugeFollowsPlayer3 = true;

void ResetGauges3()
{
    s_mpgaugeOwner.clear();
    s_mpgaugeSlot.clear();
    s_mpgauge.clear();
    s_ploShown = nullptr;
    s_psw = nullptr;
}

// orig3 0x0021DEE8 (each gauge's init: hidden)
void InitGauges3(SW* psw)
{
    s_mpgauge.clear();
    s_ploShown = nullptr;
    s_psw = psw;
    if (!psw) return;
    s_serial = psw->serial;
    // (s3-plane) the loaded EOPID 948 of the SOs that name a gauge (the planes: h_dogfight's 96161 / 96163; the load
    // keeps its value as an oid): a copy's own gauge, a copy made later than its gauge's (the free-for-all's extra plane)
    // a free copy of that gauge
    std::map<LO*, LO*> mpgaugeSo;
    for (const auto& up : psw->aploAll)
        for (const OPT& opt : up->aopt)
            if (opt.eopid == 948 && !opt.aval.empty() && opt.aval[0].n > 0 && FCidDerivedFrom(up->Cid(), sly3::CID_SO))
            {
                LO* pg = PloFindRelative(up.get(), opt.aval[0].n);
                if (pg && (pg->Cid() != CID_GAUGE3 || mpgaugeSo.count(pg))) pg = nullptr;
                if (!pg && up->ploCloneOf)
                    for (LO* p : AploOfOid(psw, opt.aval[0].n))
                        if (p->Cid() == CID_GAUGE3 && !p->paloParent && !mpgaugeSo.count(p)) { pg = p; break; }
                if (pg) mpgaugeSo[pg] = up.get();
            }
    for (LO* plo : AploOfClass(psw, CID_GAUGE3))
    {
        GAUGE3 g;
        g.plo = plo;
        g.player = plo->paloParent;
        if (auto it = s_mpgaugeOwner.find(plo); it != s_mpgaugeOwner.end()) g.player = it->second;
        else
            // (s3-hack) the gauge's own player, EOPID 1250 (optid 1184, +0x13C, kind 0x2031): e.g. h_hack's 92988 names
            // the first ship 93052 (an instance: the scripts' global find)
            for (auto o = plo->aopt.rbegin(); o != plo->aopt.rend(); ++o)
                if (o->eopid == 1250 && !o->aval.empty() && o->aval[0].n > 0)
                {
                    if (LO* p = PloFindRelative(plo, o->aval[0].n, 5)) g.player = p;
                    break;
                }
        if (!g.player)
            if (auto it = mpgaugeSo.find(plo); it != mpgaugeSo.end()) g.player = it->second;
        s_mpgauge[plo] = g;
        if (static const bool s_f = std::getenv("SLY3_GAUGEPRINT") != nullptr; s_f)   // (debug: the world's gauges)
            std::printf("gauge3: %d (inst %d) player %d (cid 0x%X) in world %d\n", plo->oid, plo->oidInstance, g.player ? g.player->oid : -1,
                        g.player ? g.player->Cid() : 0, int(plo->fInWorld));
    }
}

// (s3-hack) Sly 3 EOPID 948: an SO names its gauge (its +0x168)
void SetSoGauge3(LO* pso, LO* pgauge)
{
    if (!pso || !pgauge || pgauge->Cid() != CID_GAUGE3) return;
    for (auto it = s_mpgaugeOwner.begin(); it != s_mpgaugeOwner.end();)
        it = it->second == pso ? s_mpgaugeOwner.erase(it) : std::next(it);
    s_mpgaugeOwner[pgauge] = pso;
    // (a gauge made after the world's start, e.g. a copy for a third / fourth hacker: taken in)
    if (s_psw && !s_mpgauge.count(pgauge))
    {
        GAUGE3 g;
        g.plo = pgauge;
        s_mpgauge[pgauge] = g;
    }
    for (auto& [plo, g] : s_mpgauge)
        if (plo == pgauge) g.player = pso;
        else if (g.player == pso && plo->paloParent != pso) g.player = nullptr;
}
void SetGaugeSlot3(LO* pgauge, bool fOn, float u, float v)
{
    if (!pgauge) return;
    if (fOn) s_mpgaugeSlot[pgauge] = GAUGESLOT3{u, v};
    else s_mpgaugeSlot.erase(pgauge);
}
LO* PloSoGauge3(const LO* pso)
{
    for (auto& [pg, po] : s_mpgaugeOwner)
        if (po == pso) return const_cast<LO*>(pg);
    return nullptr;
}
namespace
{
struct INITSOGAUGE3
{
    INITSOGAUGE3()
    {
        // Sly 3 EOPID 948 (optid 949, the SO's +0x168, kind 0x20A3: a gauge): a field store natively kept here
        RegisterObjectSetter3(948, [](LO* plo, const CRef& r) {
            if (r.m_tagk == TAGK_Basic) SetSoGauge3(plo, dynamic_cast<LO*>(r.m_pbasic));
        });
        RegisterObjectGetter3(948, [](LO* plo) {
            CRef r;
            if (LO* p = PloSoGauge3(plo)) r.SetBasic(p);
            return r;
        });
    }
} s_initsogauge3;
}  // namespace

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

// orig3 0x21E4F0 / 0x21E558 called by others on the actor's +0x168 (Sly's state 22, the guard's check: 0x1ADDD0 hides it
// entering, shows it leaving): the gauge's slot 54 (state -1, the base hide) / 56 (state 0, the base show)
void ShowGaugeOf3(LO* player, bool fShow)
{
    GAUGE3* pg = PgaugeOfPlayer(player);
    if (!pg || !pg->plo) return;
    if (fShow)
    {
        SetGaugeState(*pg, 0);
        ShowScreenObj(pg->plo, -1);
    }
    else SetGaugeState(*pg, -1);
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
    // the two-player game (GAME 0x36DBE0 == 2): each player's pad shows its gauge (0x1F4548), scaled 0.75 and placed by
    // the player's index (0x21E588: D_00325FE0 {490, 167} player 1 (by Sly's portrait, top right), {15, 403} player 2
    // (by Carmelita's, bottom left))
    const bool f2p = psw->nScreenMode3 == 2 && split3::FActive() && !s_fOff;
    if (f2p)
    {
        // (PC: a player's copy made while the world runs (a hub without Carmelita gets v_ext's, split screen's teams)
        // brings its gauge with it: taken in as the world's own were at the init)
        if (const std::vector<LO*>& a = AploOfClass(psw, CID_GAUGE3); a.size() != s_mpgauge.size())
            for (LO* plo : a)
                if (!s_mpgauge.count(plo))
                {
                    GAUGE3 g;
                    g.plo = plo;
                    g.player = plo->paloParent;
                    s_mpgauge[plo] = g;
                }
        // (s3-hack: 0x21E0C8 gives the gauge +0x130 = SW +0x4FA4 (option 1003, the two players' cameras) and +0x134 its
        // player's index; 0x21E588 scales / places it only with both: the hacking game (one camera) shows each player's
        // gauge full size where it is authored: Bentley's top left, the second player's top right, as the reference)
        const bool fPlaced = split3::FTwoCameras(psw);
        // (0x21E588 also gives the gauge the player index's priority D_00326000 {8, 9} (over the 2P HUD 3325's band), only
        // while its +0x94 is 8 or 9; no index: 8 (the loaded value: screenrt.cpp's priority, the field's Sly 2 twin +0xA4). s3-plane, the Biplane
        // Duel's free-for-all: players 3 / 4 (views 3 / 4: the bands of players 1 / 2) their band's place and priority,
        // each view drawing only its own player's gauge, duel3.h)
        static const float s_auv[2][2] = {{490.0f, 167.0f}, {15.0f, 403.0f}};
        static const int s_aprio[2] = {8, 9};
        // (PC: the hacking game's third / fourth players, split3's seats 3 / 4, each with its gauge in its corner; s3-plane,
        // the Biplane Duel's free-for-all (two cameras): its planes 3 / 4)
        const int cplayer = fPlaced ? std::min(split3::CView(), 4) : std::clamp(split3::CSeats(), 2, 4);
        LO* aplayer[4] = {};
        for (int k = 0; k < cplayer; ++k) aplayer[k] = split3::PloPlayer(psw, k);
        const bool fCard = FCardMessageShown3();
        for (auto& [plo, g] : s_mpgauge)
        {
            int i = -1;
            for (int k = 0; k < cplayer; ++k)
                if (aplayer[k] && g.player == aplayer[k] && (k < 2 || !fPlaced || FPlane3(g.player))) i = k;
            int prio = NScreenObjPrio(plo);
            if (prio == 0) prio = int(plo->U32Field(0, 0xA4, 0));
            const bool fPrio = prio == 8 || prio == 9;
            if (i < 0 || fCard)
            {
                if (g.state != -1) SetGaugeState(g, -1);
                SetScreenObjRootXY(plo, false, 1.0f, 0.0f, 0.0f);
                if (fPrio) SetScreenObjPrio(plo, 8);
                continue;
            }
            if (fPrio) SetScreenObjPrio(plo, s_aprio[i & 1]);
            if (fPlaced) SetScreenObjRootXY(plo, true, 0.75f, s_auv[i & 1][0], s_auv[i & 1][1]);
            else if (auto it = s_mpgaugeSlot.find(plo); it != s_mpgaugeSlot.end()) SetScreenObjRootXY(plo, true, 1.0f, it->second.u, it->second.v);
            else SetScreenObjRootXY(plo, false, 1.0f, 0.0f, 0.0f);
            if (g.state == -1) ReadFractions(g);   // (out of health it hid itself: shown again once restored)
            if (g.state == -1 && g.hTarget > 0.0f)
            {
                SetGaugeState(g, 0);
                ShowScreenObj(plo, -1);
            }
        }
        s_ploShown = nullptr;
    }
    else if (g_fGaugeFollowsPlayer3) SetGaugePlayer3(s_fOff || FCardMessageShown3() ? nullptr : player);
    for (auto& [plo, g] : s_mpgauge)
    {
        if (g.state == -1) continue;
        ReadFractions(g);
        StepGauge(g);
        UpdateFace(g);
    }
    StepViewGauges(psw);
    if (static const bool s_fPrint = std::getenv("SLY3_GAUGEPRINT") != nullptr; s_fPrint)
        for (auto& [plo, g] : s_mpgauge)
            if (g.state != -1)
                std::printf("gauge3 %d (player %d): prio %d state %d health %.2f / %.2f power %.2f / %.2f\n", plo->oid, g.player ? g.player->oid : -1, NScreenObjPrio(plo), g.state,
                            g.hShown, g.hTarget, g.gShown, g.gTarget);
}

// ------------------------------------------------------------ the HUD notice (s3-decomp, 2026-11-10)
// orig3: the widget D_0046FD00 (vtable 0x363B90): the script command 0x1C's 0x222FF0(widget, n, 0, screen) hides a
// shown notice (slot 11), keeps n (+0x30) and the screen object (+0x34) and shows it (slot 10 0x222D38: the world's
// clock channel 9 at 0, PauseGame 0x1A93C8, the screen's slot 53 with 0x377). Its update (slot 7 0x222E48), once the
// screen has settled (state 2) and 0.1 s after: n 1 a choice (square -1 / triangle 0 into D_0036DBA4), n 2 / 3 square
// starts the transition 0x289198(2, 0) or triangle closes, else cross closes; the close (slot 11 0x222DC8) undoes it.
// Busy (FCmdDone 28) while it is up (0x21DD30). Natively: the menus' clock channel 0 (Sly 3's 9 has no native
// channel), no PauseGame, the transition not started (noted).
namespace
{
struct NOTICE3 { int n = -1; LO* plo = nullptr; bool fUp = false; float tShown = 0; int nChoice = -1; };
NOTICE3 s_notice;

// (audit 5 CORE5-08: the clock's factor 9 and PauseGame / ResumeGame (0x1A93C8 / 0x1A9420 (SW, -1, 0)) as the
// original; SLY3_NOTICECLOCK0=1: the menus' factor 0 without the pause, as before)
bool FNoticeOld3() { static const bool s_f = std::getenv("SLY3_NOTICECLOCK0") != nullptr; return s_f; }

void CloseNotice3()
{
    if (!s_notice.fUp) return;
    if (FNoticeOld3()) SetClockFactor(&g_clock, 0, 1.0f);
    else
    {
        SetClockFactor(&g_clock, 9, 1.0f);   // 0x222DC8
        ResumeGame(0);                       // 0x1A9420(SW, -1, 0)
    }
    if (s_notice.plo) HideScreenObj(s_notice.plo);
    s_notice.fUp = false;
    s_notice.plo = nullptr;
}

void ShowNotice3(int n, LO* plo)
{
    if (n == 1 && !FAsk3d3())
    {
        // (the 3-D mode off: the default, Settings.ini [Graphics] 3D Mode; SLY3_ASK3D=0 the test harnesses' routes) the
        // 3-D question is not asked: its No (triangle) taken at once
        g_n3dChoice3 = 0;
        std::printf("3d: the 3-D question (screen %d) answered No without showing it (the 3-D mode off)\n", plo ? plo->oid : -1);
        return;
    }
    CloseNotice3();
    s_notice.n = n;
    s_notice.plo = plo;
    if (!plo) return;
    s_notice.fUp = true;
    s_notice.tShown = g_clock.tReal;
    if (FNoticeOld3()) SetClockFactor(&g_clock, 0, 0.0f);
    else
    {
        SetClockFactor(&g_clock, 9, 0.0f);   // 0x222D38
        PauseGame(0);                        // 0x1A93C8(SW, -1, 0)
    }
    ShowScreenObj(plo, 0x377);
}

bool FNoticeBusy3() { return s_notice.fUp; }
}  // namespace

int g_n3dChoice3 = [] { const char* psz = std::getenv("SLY3_3DANSWER"); return psz ? std::atoi(psz) : 0; }();   // (debug: the answer at the start, 1 the 3-D mode)
bool FAsk3d3()
{
    // (s3-3dtoggle) SLY3_ASK3D (1 asked, 0 not) overrides Settings.ini [Graphics] 3D Mode (off by default)
    static const int s_nEnv = [] { const char* psz = std::getenv("SLY3_ASK3D"); return psz ? (std::atoi(psz) != 0 ? 1 : 0) : -1; }();
    return s_nEnv >= 0 ? s_nEnv != 0 : g_saveopts.f3dMode;
}

bool FNoticeShown3() { return s_notice.fUp; }

bool FJob3d3() { return g_tasks3.pjobCur && g_tasks3.pjobCur->n40 == 1; }
bool F3dChoosable3() { return FChallenge3d3() || FJob3d3(); }

// orig3: 0x00223078 / 0x00223148 (script strings 852 / 853 of the table 0x47A4B8): the notice's text and its buttons
// line by n (+0x30 of the widget: 1 the 3-D question 0x97 with "&2S&. : " Yes 0x99 "\n&2T&. : " No 0x98; 2 / 3 the
// Hazard Room's 0x9A / 0x9B with 0x9C / 0x9D, 0x9E; 4..7 0xA0, 0xA1, 0x9F, 0xA2 with "&2X&. : " OK 0x47)
static std::string StrNoticeText3(bool fButtons)
{
    const SW* psw = g_psw;
    auto t = [&](int id) { return StrText3(psw, id); };
    const int n = s_notice.n;
    if (!fButtons)
    {
        static const int s_aid[8] = {-1, 0x97, 0x9A, 0x9B, 0xA0, 0xA1, 0x9F, 0xA2};
        return n >= 1 && n <= 7 ? t(s_aid[n]) : std::string();
    }
    switch (n)
    {
    case 1: return "&2S&. : " + t(0x99) + "\n&2T&. : " + t(0x98);
    case 2: return "&2S&. : " + t(0x9C) + "\n&2T&. : " + t(0x9D);
    case 3: return "&2S&. : " + t(0x9C) + "\n&2T&. : " + t(0x9E);
    case 4: case 5: case 6: case 7: return "&2X&. : " + t(0x47);
    default: return {};
    }
}

void InstallNotice3()
{
    s_notice = NOTICE3{};
    SetScriptString3(852, [] { return StrNoticeText3(false); });
    SetScriptString3(853, [] { return StrNoticeText3(true); });
    if (!g_scpthooks3.pfnShowNotice) g_scpthooks3.pfnShowNotice = ShowNotice3;
    if (!g_scpthooks3.pfnFNoticeBusy) g_scpthooks3.pfnFNoticeBusy = FNoticeBusy3;
}

void UpdateNotice3()
{
    NOTICE3& p = s_notice;
    if (!p.fUp || !p.plo) return;
    if (NScreenObjState(p.plo) != 2 || g_clock.tReal - p.tShown <= 0.1f) return;
    switch (p.n)
    {
    case 1:   // (the 3-D question; SLY3_ASK3D=0: not asked)
        if (GrfbtnTakePressed(0x80)) p.nChoice = -1;
        else if (GrfbtnTakePressed(0x10)) p.nChoice = 0;
        else return;
        g_n3dChoice3 = p.nChoice;
        std::printf("3d: the 3-D question answered %s (the next world load: %s)\n", p.nChoice == 0 ? "No" : "Yes",
                    p.nChoice == 0 ? "the normal view" : "the world's own mode");
        break;
    case 2: case 3:
        // square: 0x289198(SW, 2, 0, D_00468D30): episode 2 started (epmenu3.cpp StartEpisode3); triangle closes
        if (GrfbtnTakePressed(0x80))
        {
            if (FNoticeOld3()) std::fprintf(stderr, "sly3: the notice's transition 0x289198(2, 0) is not started natively\n");
            else if (g_psw) StartEpisode3(g_psw, 2);
        }
        else if (!GrfbtnTakePressed(0x10)) return;
        break;
    default:
        if (!GrfbtnTakePressed(0x40)) return;
        break;
    }
    CloseNotice3();
}
