// ship3.cpp: Sly 3's sailing ships (class 0x58, orig3 0x2139D0..0x21A570), from the Sly 3 decomp.
//
// Ep5's ships: the gang's ship (the player's: sailed on the pad in mode 2, or in a battle (mode 4) by its helmsman
// while the player runs the deck and fires the cannons) and the computer's (phase 2: patrol curve, chase, broadside).
// The record keeps the class's fields (+0x190..+0x564). Ported:
//  - the init 0x2139D0 (defaults), the options (EOPIDs 1954..2016: the loaded ones replayed in the load's order at the
//    record's init, as the original's setters / methods ran then; the scripts' setters / getters / methods later);
//  - the modes 0x2159D8 (0 / 1 idle, 2 sailing, 3 the map, 4 battle, 5 nearly sunk, 6 out, 7 sunk; the records'
//    modes and constraints, the cannons' states), the phases 0x215F10 (0: the ship takes its character's pad, 1: the
//    character takes the ship's, 2 none: the computer's);
//  - the update 0x215FD8: the motion 0x216020 (the speed from the wind's angle 0x219000 / 0x219058, the turn rate
//    from the rudder), the pad's steering 0x216350 (L2 / R2, a battle started near another ship 0x21A068), the
//    computer's sailing 0x216680 (patrol 0x217470, chase / broadside, avoidance, the route 0x217328), the cannons'
//    timers 0x217D90 / the shot 0x2185B0, the computer's broadsides 0x217650 / 0x2183F8 / 0x218C00;
//  - the forces 0x214028 (vt slot 74: the yaw rate, the speed along the turned bow, the height spring);
//  - the hits 0x214370 (vt slot 78: by side, the cannons lost 0x217F68, mode 5 / 7), the reset 0x213F08 / 0x215640;
//  - the battle manager (class 0x9B, the object 1943: 0x219768 / 0x219A18 / 0x21A068 / 0x21A1C8 / 0x21A300 / 0x21A450):
//    the participants, the end, the tally screen 1945 (strings 1952..1966).
// Not ported (noted): the sails' / cannons' animation labels (0x239DB8 / 0x239CE8: D_00325BC0 / D_00325CA0), the
//  hull's heel 0x2179A0's sway, the engine sound 0x217040, the map screen's markers 0x219210 (mode 3), the BSP
//  ship-ship contacts 0x214F88 / 0x214DA8, the path planner 0x1ED1B0 (a straight route, the original's fallback).
// Debug: SLY3_SHIPPRINT=1 (options, modes, hits, shots), =2 (each ship every second).

#include "ship3.h"
#include "../aicarm3.h"

#include "../activate.h"
#include "../alo.h"
#include "../brx.h"
#include "../bsp.h"
#include "../clock.h"
#include "../cm.h"
#include "../cmworld.h"
#include "../crv.h"
#include "../lo.h"
#include "../nav.h"
#include "../bif.h"
#include "../objmethod.h"
#include "../pair.h"
#include "../savedata.h"
#include "../screen3.h"
#include "../screenrt.h"
#include "../po.h"
#include "../projectile.h"
#include "../ref.h"
#include "../sm.h"
#include "../smrun.h"
#include "../so.h"
#include "../step.h"
#include "../stepphys.h"
#include "../splitscreen3.h"
#include "../spliceglue.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../vtnative.h"
#include "../xf.h"
#include "../xfphys.h"
#include "sly3.h"
#include "scptgame3.h"
#include "scptrun3.h"
#include "task3.h"

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

void VehicleAccelRotation(ALO* palo, const glm::vec3& v);   // vehicle.cpp (orig3 0x244890)
int NRandInRange(int nLow, int nHigh);   // 0x11D3F8
void UseObjectBy(LO* puse, struct ACTOR* pa);   // interact.cpp (the use's vt slot 55: its Sly 3 script)
struct ACTOR* PactorFromLo(LO* plo);            // actor.h
float GRandInRange(float gLow, float gHigh);   // 0x11D448
void FadeAloOut(ALO* palo, float dt);   // asegevent.cpp (orig3 0x2412C8)
void UpdateShipBattle3();
void UpdateGalleonAuto3();
bool FShipBroadside3(LO* plo, glm::vec3* ppos, float* pheading, glm::vec3* pvel, unsigned* pgrf, bool* pfUse);
bool FShipInBattle3(const LO* plo);   // (the battle manager, below)
void OnShipOut3(LO* plo);
void ParkBeside3(LO* plo);

namespace
{
using cmm::SMP;
using cmm::GSmooth;
using cmm::RadNormalize;
using cmm::GLimitAbs;
inline bool FFloatsNear(float a, float b, float e) { return std::fabs(a - b) < e; }
constexpr int CID_SHIP = 0x58;
constexpr float kPi = 3.1415927f;
constexpr float kHalfPi = 1.5707964f;
int NPrint()
{
    static const int s_n = std::getenv("SLY3_SHIPPRINT") ? std::atoi(std::getenv("SLY3_SHIPPRINT")) : 0;
    return s_n;
}

// a goal record (0x2C bytes at +0x1D4, the current one +0x1DC: the mode it is for, the speed, the rudder)
struct GOAL3
{
    int mode = -1;               // +0x00 (EOPID 1954: the record added for a mode)
    float speed = 0.0f;          // +0x04 (1955)
    float accel = 0.0f;          // +0x08 (1956): the drive's limit (0x1205F0)
    SMP smp{0.0f, 0.0f, 0.0f};   // +0x0C (1960): the rudder's smoothing
    float rudderMax = 0.0f;      // +0x18 (1959)
    SMP smpSway{0.0f, 0.0f, 0.0f};   // +0x1C (1958): the heel's smoothing (not drawn)
    float sway = 0.0f;           // +0x28 (1957)
};

// a cannon (0x1C bytes, +0x270 + (side * 6 + i) * 0x1C)
struct CANNON3
{
    LO* cannon = nullptr;        // +0x00 (EOPID 1989's oids, resolved at the post-load 0x2181D0: an ALO)
    LO* muzzle = nullptr;        // +0x04 its child (class 3)
    LO* anim = nullptr;          // its state machine (class 0x84)
    SMA* psma = nullptr;         // +0x08 the machine applied to the cannon (0x23AAE8): its goal by the state (D_00325CA0)
    int idCannon = -1, idMuzzle = -1, idAnim = -1;
    int active = 0;              // +0x0C
    int state = 0;               // +0x10: 0 off, 1 ready, 2 armed, 3 fired, 4 broken
    float timer = 0.0f;          // +0x14
    float delay = 0.0f;          // +0x18
};

struct SHIP3
{
    ALO* palo = nullptr;
    SW* psw = nullptr;
    uint32_t serial = 0;                      // (native: a reloaded world can reuse the SW's and the objects' addresses)
    std::array<int, 2> hp{100, 100};          // +0x190 / +0x194: the port / starboard hit points
    std::array<int, 2> hpMax{100, 100};       // +0x198 / +0x19C
    std::array<int, 2> hpSave{100, 100};      // +0x1A0 / +0x1A4 (the cannons' share)
    LO* child = nullptr;                      // +0x1A8 (EOPID 1962)
    int side = -1;                            // +0x1AC the side last hit (0 port, 1 starboard, 2 both)
    int mode = -1;                            // +0x1B0
    float tMode = 0.0f;                       // +0x1B4
    int phase = 0;                            // +0x1B8
    int submode = -1;                         // +0x1C0
    float tSubmode = 0.0f;                    // +0x1C4
    int group = 0;                            // +0x1C8 (EOPID 1975)
    int groupOwner = 0;                       // +0x1CC (1976)
    LO* owner = nullptr;                      // +0x1D0 (1978): the ship's character (its pad's, phases 0 / 1)
    std::vector<GOAL3> agoal;                 // +0x1D4 / +0x1D8 (at most 4)
    int igoal = -1;                           // +0x1DC
    std::array<float, 13> aspeed{0.3f, 0.5f, 0.743f, 0.9f, 1.1f, 1.2f, 1.55f, 1.5f, 1.45f, 1.4f, 1.35f, 1.3f, 1.3f};   // +0x1E0
    int cspeed = 13;                          // +0x214
    float speedScale = 0.0f;                  // +0x218
    float windAngle = 0.0f;                   // +0x21C
    float length = 4000.0f;                   // +0x220 (1979)
    float speed = 0.0f;                       // +0x224
    float yawRate = 0.0f;                     // +0x228
    float rudder = 0.0f;                      // +0x22C
    float height = 0.0f;                      // +0x248 (the reset's z)
    float sink = 0.0f;                        // +0x24C (2012)
    LO* nav = nullptr;                        // +0x250 (1977)
    SHIP3* ptarget = nullptr;                 // +0x254
    std::vector<glm::vec3> route;             // +0x258
    float routeS = 0.0f;                      // +0x25C
    int sideFire = -1;                        // +0x260
    float tSideFire = 0.0f;                   // +0x264
    LO* patrol = nullptr;                     // +0x268 (2007)
    int patrolDir = 1;                        // +0x26C
    std::array<std::array<CANNON3, 6>, 2> acannon{};   // +0x270
    std::array<int, 2> ccannon{0, 0};         // +0x3C0
    LO* balls = nullptr;                      // +0x3C8 (1980): the cannonballs' proxy
    float step = 0.1f;                        // +0x3CC (1981): between a broadside's shots
    float range = 30000.0f;                   // +0x3D0 (1982)
    glm::vec3 flight{0.35f, 0.00005f, 0.0f};  // +0x3E0 (1986): the flight time by the distance
    float yawA = 0.03490659f, yawB = 0.13962635f;   // +0x3F0 (1987)
    LM tilt{-0.26179942f, 0.26179942f};       // +0x3F8 (1988): a hit within it hits both sides
    LM delayFire{10.0f, 20.0f};               // +0x400 (1998): a broadside's stagger
    int limit = 4;                            // +0x408 (1983)
    float fraction = 0.5f;                    // +0x40C (1984)
    LO* anim = nullptr;                       // +0x410 (1985): the ship's state machine (its sails)
    SMA* psmaShip = nullptr;                  // +0x414 (the post-load's 0x23AAE8): sought per mode (D_00325BC0)
    int targetGroup = 0;                      // +0x418 (1990)
    std::array<LO*, 2> aimTarget{nullptr, nullptr};   // +0x41C
    LO* current = nullptr;                    // +0x424 (1991)
    float outer = 20000.0f;                   // +0x428 (1992): the engage radius
    float inner = 7500.0f;                    // +0x42C (1993)
    LO* fx = nullptr;                         // +0x430 (1994)
    LO* focus = nullptr;                      // +0x444 (2001)
    LO* mapScreen = nullptr;                  // +0x448 (2002)
    LO* partner = nullptr;                    // +0x44C (2003)
    float threshold = 0.0f;                   // +0x454 (2004)
    int type = -1;                            // +0x4F4 the power-up (0x3E..0x42)
    float tType = 0.0f, durType = -1.0f;      // +0x4F8 / +0x4FC
    LM delayType{6.0f, 12.0f};                // +0x500 (1999)
    LM reward2{100.0f, 200.0f};               // +0x508 (2008)
    LM reward3{200.0f, 400.0f};               // +0x510 (2009)
    int busy = 0;                             // +0x518 (2010)
    int windOwn = 0;                          // +0x550: an entry of a class 0x62 object touched the hull (+0x544) this frame
                                              // (set by the own-contacts slot 63 0x215008, cleared by slot 19 0x214928)
    int n54C = -1;                            // +0x54C (2011)
    int resourceId = -1;                      // +0x55C (2015)
    LO* shape = nullptr;                      // +0x564 (2016): the outline for the ship-ship contacts (0x214DA8)
    SOE* psoeResource = nullptr;              // +0x560: the collision entry with that tag (0x23F3B8)
    std::vector<int> acidCollide;             // +0x51C (2013, at most 4): the classes it collides with
    std::vector<int> aoidCollide;             // +0x530 (2014): the objects (or aliases)
    float tLastHit = -1.0f;                   // +0x458
    bool fFaded = false;                      // (mode 7: the fade-out started)
    glm::mat4 matPrev{0.0f};                  // (the last frame's matrix: the deck's carry of a copy's character)
    unsigned grfAuto = 0;                     // (the harness: SLY3_GALLEONAUTO's steering for this ship's seat)
    bool fAuto = false;
    float tAutoUse = -100.0f;
    LO* ploLastHit = nullptr;                 // (natively a ball's contacts with several of the hull's entries in one
    float tLastHitSender = -1.0f;             //  frame are one hit: the original's ball dies at its first)
    bool fInit = false;
};
std::unordered_map<const LO*, SHIP3> s_mpship;
std::vector<std::pair<ALO*, float>> g_aballTrace3;   // (debug: SLY3_SHIPPRINT=3, the cannonballs' flights)

// ---- small helpers ----
// a loaded option's value set (screen3.cpp reads class 0xA0's counts 1244 / 1245 from the options: +0x120 / +0x124)
void SetOptionN(LO* plo, int eopid, int n)
{
    if (!plo) return;
    for (OPT& o : plo->aopt)
        if (o.eopid == eopid)
        {
            if (o.aval.empty()) o.aval.emplace_back();
            o.aval[0].n = n;
            return;
        }
    OPT o;
    o.eopid = eopid;
    o.aval.emplace_back();
    o.aval[0].n = n;
    plo->aopt.push_back(o);
}
glm::mat4 MatWorld(ALO* palo)
{
    XF* pxf = palo ? PxfObj(palo) : nullptr;
    return pxf ? pxf->matWorld : glm::mat4(1.0f);
}
glm::vec3 PosOf(LO* plo)
{
    if (!plo) return glm::vec3(0.0f);
    if (FIsLoDerivedFrom(plo, S3CID(0x30)))
        if (XF* pxf = PxfObj(static_cast<ALO*>(plo))) return glm::vec3(pxf->matWorld[3]);
    return PosLoWorld(plo);
}
glm::mat4 MatOf(LO* plo)
{
    if (plo && FIsLoDerivedFrom(plo, S3CID(0x30))) return MatWorld(static_cast<ALO*>(plo));
    if (plo && FIsLoDerivedFrom(plo, 3)) return MatWorldLox(plo);   // (a LOX: the muzzles, class 3)
    if (plo) { glm::mat4 m(1.0f); m[3] = glm::vec4(PosLoWorld(plo), 1.0f); return m; }
    return glm::mat4(1.0f);
}
// world -> the ship's space (0x1C9428(0, ship, pos)) and back
glm::vec3 ToLocal(ALO* palo, const glm::vec3& pos)
{
    const glm::mat4 m = MatWorld(palo);
    return glm::transpose(glm::mat3(m)) * (pos - glm::vec3(m[3]));
}
glm::vec3 VecToLocal(ALO* palo, const glm::vec3& v) { return glm::transpose(glm::mat3(MatWorld(palo))) * v; }
float LenXY(const glm::vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y); }
float Atan2Of(const glm::vec3& v) { return std::atan2(v.y, v.x); }
float Heading(ALO* palo)
{
    const glm::mat4 m = MatWorld(palo);
    return std::atan2(m[0][1], m[0][0]);
}

LO* PloOpt(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    return v.n == -1 || v.n == 0 ? nullptr : PloFindRelative(plo, v.n);
}
LO* PloRef(LO* plo, const CRef& r)
{
    if (r.m_tagk == TAGK_S32) return r.m_n == -1 || r.m_n == 0 ? nullptr : PloFindRelative(plo, r.m_n);
    LO* pref = dynamic_cast<LO*>(r.m_pbasic);
    if (pref && plo->ploCloneOf && pref->oid > 0)
        if (LO* pown = PloFindRelative(plo, pref->oid)) return pown;
    return pref;
}
int NOf(const CRef& r) { return r.m_tagk == TAGK_F32 ? static_cast<int>(r.m_g) : r.m_tagk == TAGK_Bool ? r.m_bool : r.m_n; }
float GOf(const CRef& r) { return r.m_tagk == TAGK_S32 ? static_cast<float>(r.m_n) : r.GAsFloat(); }
int IdOf(const CRef& r)
{
    if (r.m_tagk == TAGK_S32) return r.m_n;
    if (LO* plo = dynamic_cast<LO*>(r.m_pbasic)) return plo->oid;
    return -1;
}

SHIP3* PshipFind(const LO* plo)
{
    if (!plo || !g_psw) return nullptr;
    auto it = s_mpship.find(plo);
    return it != s_mpship.end() && it->second.psw == g_psw && it->second.serial == g_psw->serial && it->second.fInit ? &it->second : nullptr;
}
SHIP3* Pship(LO* plo);
PADCTL* PpadOf(LO* plo);
void SetMode(SHIP3& s, int mode);
SHIP3* PshipOf(LO* plo) { return plo && FShip3(plo) ? Pship(plo) : nullptr; }

GOAL3* Pgoal(SHIP3& s) { return s.igoal >= 0 && s.igoal < int(s.agoal.size()) ? &s.agoal[size_t(s.igoal)] : nullptr; }
// orig3 0x2151A0: the last record of the mode
int IgoalForMode(const SHIP3& s, int mode)
{
    int i = -1;
    for (int j = 0; j < int(s.agoal.size()); ++j)
        if (s.agoal[size_t(j)].mode == mode) i = j;
    return i;
}

// the ships in the world (orig3: the list D_00479BEC, linked at +0x554 by the add / remove slots 25 / 24)
std::vector<SHIP3*> AshipWorld()
{
    std::vector<SHIP3*> a;
    if (!g_psw) return a;
    for (LO* plo : g_psw->mpcidaplo[CID_SHIP])
        if (plo && plo->fInWorld)
            if (SHIP3* ps = Pship(plo)) a.push_back(ps);
    return a;
}

void SetMode(SHIP3& s, int mode);
void SetPhase(SHIP3& s, int phase);
void SetSubmode(SHIP3& s, int sub);
void SetCannonState(CANNON3& c, int state, float g)   // orig3 0x217D50
{
    c.timer = g;
    c.state = state;
    // its machine's goal (0x239CE8 SetSmaGoal): 146 off, 132 ready / armed (the use point in the world), 134 fired, 149 broken
    static const int s_agoal[5] = {146, 132, 132, 134, 149};
    if (c.psma && state >= 0 && state < 5) SetSmaGoal(c.psma, s_agoal[state]);
}

// ---- the hit points (0x2157D0 / 0x215808 / 0x215870) and the cannons the hits break (0x217F68) ----
void UpdateBrokenCannons(SHIP3& s, int side)   // orig3 0x217F68
{
    CANNON3* aother[8];
    CANNON3* abroken[8];
    int cOther = 0, cBroken = 0;
    const int c = s.ccannon[size_t(side)];
    for (int i = 0; i < c; ++i)
    {
        CANNON3& k = s.acannon[size_t(side)][size_t(i)];
        if (!k.active) continue;
        if (k.state == 4) abroken[cBroken++] = &k;
        else aother[cOther++] = &k;
    }
    if (cBroken + cOther != c || s.hpMax[size_t(side)] <= 0) return;
    const float ratio = float(s.hpMax[size_t(side)] - s.hpSave[size_t(side)]) / float(s.hpMax[size_t(side)]) / ((1.0f - s.fraction) / float(s.limit));
    int want = std::min(int(std::floor(ratio)), s.limit);
    int a = want - cBroken;
    int b = (c - want) - cOther;
    while (a > 0 && cOther > 0)
    {
        const int pick = NRandInRange(0, cOther - 1);
        --a;
        SetCannonState(*aother[pick], 4, 3.4028235e38f);
        for (int j = pick; j < cOther - 1; ++j) aother[j] = aother[j + 1];
        --cOther;
    }
    const int st = s.mode == 1 || s.mode == 4 ? 1 : 0;
    while (b > 0 && cBroken > 0)
    {
        const int pick = NRandInRange(0, cBroken - 1);
        --b;
        SetCannonState(*abroken[pick], st, 3.4028235e38f);
        for (int j = pick; j < cBroken - 1; ++j) abroken[j] = abroken[j + 1];
        --cBroken;
    }
}
void SetHp(SHIP3& s, int a, int b)   // 0x2157D0
{
    s.hp[0] = std::min(a, s.hpMax[0]);
    s.hp[1] = std::min(b, s.hpMax[1]);
}
void SetHpSave(SHIP3& s, int a, int b)   // 0x215808
{
    s.hpSave[0] = std::min(a, s.hpMax[0]);
    s.hpSave[1] = std::min(b, s.hpMax[1]);
    UpdateBrokenCannons(s, 0);
    UpdateBrokenCannons(s, 1);
}
void SetHpMax(SHIP3& s, int a, int b)   // 0x215870
{
    s.hpMax = {a, b};
    for (int i = 0; i < 2; ++i)
    {
        s.hp[size_t(i)] = std::min(s.hp[size_t(i)], s.hpMax[size_t(i)]);
        s.hpSave[size_t(i)] = std::min(s.hpSave[size_t(i)], s.hpMax[size_t(i)]);
    }
    UpdateBrokenCannons(s, 0);
    UpdateBrokenCannons(s, 1);
}

// ---- the power-ups (0x215388 / 0x2155F0): 0x3E / 0x3F / 0x42 timed, 0x40 repairs the weaker side, 0x41 restaggers ----
void SetType(SHIP3& s, int type)
{
    if (type == s.type) return;
    s.type = type;
    s.tType = g_clock.t;
    switch (type)
    {
    case 0x3E: case 0x3F: case 0x42: s.durType = 60.0f; break;
    case 0x40:
    {
        s.durType = -1.0f;
        int best = 0, lowest = s.hpMax[0];
        for (int k = 0; k < 2; ++k)
            if (s.hp[size_t(k)] < lowest) lowest = s.hp[size_t(k)], best = k;
        s.hp[size_t(best)] = std::min(s.hpMax[size_t(best)], s.hp[size_t(best)] + int(float(s.hpMax[size_t(best)]) * 0.2f));
        break;
    }
    case 0x41: s.durType = 60.0f; break;   // (the armed cannons' times re-interpolated: not needed, they fire by +0x500 next)
    default: s.durType = -1.0f; break;
    }
}

// ---- the options ----
void ApplyOption(SHIP3& s, int eopid, const std::vector<CRef>& a);

void AddGoal(SHIP3& s, int mode)   // 0x215160
{
    if (s.agoal.size() >= 4) return;
    GOAL3 g;
    g.mode = mode;
    s.agoal.push_back(g);
    s.igoal = int(s.agoal.size()) - 1;
}
void AddCannon(SHIP3& s, int side, int idCannon, int idMuzzle, int idAnim)   // 0x217F10 (resolved at 0x2181D0)
{
    if (side < 0 || side > 1 || s.ccannon[size_t(side)] >= 6) return;
    CANNON3& c = s.acannon[size_t(side)][size_t(s.ccannon[size_t(side)]++)];
    c = CANNON3{};
    c.idCannon = idCannon;
    c.idMuzzle = idMuzzle;
    c.idAnim = idAnim;
}
// orig3 0x2181D0: a cannon's ids to objects (the cannon an ALO, the muzzle its child of class 3, the animation its
// class 0x84 child): none: the slot cleared
void ResolveCannons(SHIP3& s)
{
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < s.ccannon[size_t(side)]; ++i)
        {
            CANNON3& c = s.acannon[size_t(side)][size_t(i)];
            if (c.cannon) continue;
            LO* pcannon = c.idCannon != -1 ? PloFindRelative(s.palo, c.idCannon) : nullptr;
            LO* pmuzzle = pcannon ? PloFindRelative(pcannon, c.idMuzzle) : nullptr;
            LO* panim = pcannon && c.idAnim != -1 ? PloFindRelative(pcannon, c.idAnim) : nullptr;
            if (pcannon && pmuzzle)
            {
                c.cannon = pcannon;
                c.muzzle = pmuzzle;
                c.anim = panim;
                c.active = 1;
                if (SM* psm = dynamic_cast<SM*>(panim); psm && FIsLoDerivedFrom(pcannon, S3CID(0x30)))
                    c.psma = PsmaApplySm(psm, static_cast<ALO*>(pcannon), -1, 0);   // 0x23AAE8
            }
            else if (NPrint()) std::printf("ship3: %d cannon %d/%d (%d %d %d) not resolved\n", s.palo->oid, side, i, c.idCannon, c.idMuzzle, c.idAnim);
        }
}

std::vector<CRef> ArefFromOpt(const OPT& o)
{
    std::vector<CRef> a;
    auto one = [&](const OPTVAL& v) {
        CRef r;
        if (v.pbasic) r.SetBasic(v.pbasic);
        else if (v.tagk == TAGK_F32 || v.k == 1) r.SetF32(v.ag[0]);
        else r.SetS32(v.n);
        a.push_back(r);
    };
    for (const OPTVAL& v : o.aval)
    {
        if (!v.alist.empty())
            for (const OPTVAL& w : v.alist) one(w);
        else one(v);
    }
    return a;
}

// a loaded option replayed at the record's init (the original applied them at the load, in this order)
void ApplyLoaded(SHIP3& s, const OPT& o)
{
    if (o.aval.empty() && o.eopid != 1961 && o.eopid != 1995 && o.eopid != 1996) return;
    const OPTVAL v = o.aval.empty() ? OPTVAL{} : o.aval[0];
    auto pv = [&]() { return PloOpt(s.palo, v); };
    GOAL3* pg = Pgoal(s);
    switch (o.eopid)
    {
    case 1954: AddGoal(s, v.n); break;
    case 1955: if (pg) pg->speed = v.ag[0]; break;
    case 1956: if (pg) pg->accel = v.ag[0]; break;
    case 1957: if (pg) pg->sway = v.ag[0]; break;
    case 1958: if (pg) pg->smpSway = {v.ag[0], v.ag[1], v.ag[2]}; break;
    case 1959: if (pg) pg->rudderMax = v.ag[0]; break;
    case 1960: if (pg) pg->smp = {v.ag[0], v.ag[1], v.ag[2]}; break;
    case 1961: s.igoal = -1; break;
    case 1962: s.child = pv(); break;
    case 1972: s.side = v.n; break;
    case 1973: s.mode = v.n; break;   // (the load's value: the mode is entered at the reset 0x215640 through the phase)
    case 1974: s.phase = v.n; break;
    case 1975: s.group = v.n; break;
    case 1976: s.groupOwner = v.n; break;
    case 1977: s.nav = pv(); break;
    case 1978: s.owner = pv(); break;
    case 1979: s.length = v.ag[0]; break;
    case 1980: s.balls = pv(); break;
    case 1981: s.step = v.ag[0]; break;
    case 1982: s.range = v.ag[0]; break;
    case 1983: s.limit = v.n; break;
    case 1984: s.fraction = v.ag[0]; break;
    case 1985: s.anim = pv(); break;
    case 1986: s.flight = {v.ag[0], v.ag[1], v.ag[2]}; break;
    case 1987: s.yawA = v.ag[0]; s.yawB = v.ag[1]; break;
    case 1988: s.tilt = {v.ag[0], v.ag[1]}; break;
    case 1990: s.targetGroup = v.n; break;
    case 1991: s.current = pv(); break;
    case 1992: s.outer = v.ag[0]; break;
    case 1993: s.inner = v.ag[0]; break;
    case 1994: s.fx = pv(); break;
    case 1998: s.delayFire = {v.ag[0], v.ag[1]}; break;
    case 1999: s.delayType = {v.ag[0], v.ag[1]}; break;
    case 2001: s.focus = pv(); break;
    case 2002: s.mapScreen = pv(); break;
    case 2003: s.partner = pv(); break;
    case 2004: s.threshold = v.ag[0]; break;
    case 2007: s.patrol = pv(); break;
    case 2008: s.reward2 = {v.ag[0], v.ag[1]}; break;
    case 2009: s.reward3 = {v.ag[0], v.ag[1]}; break;
    case 2010: s.busy = v.n; break;
    case 2011: s.n54C = v.n; break;
    case 2012: s.sink = v.ag[0]; break;
    case 2015: s.resourceId = v.n; break;
    case 2016: s.shape = pv(); break;
    case 2013: if (s.acidCollide.size() < 4) s.acidCollide.push_back(v.n); break;
    case 2014: if (s.aoidCollide.size() < 4) s.aoidCollide.push_back(v.n); break;
    default:
        // the methods with several arguments (1963..1971 the hit points, 1989 a cannon, 2000 the speed table)
        ApplyOption(s, o.eopid, ArefFromOpt(o));
        break;
    }
}

// a script's setter / method (and the loaded methods)
void ApplyOption(SHIP3& s, int eopid, const std::vector<CRef>& a)
{
    auto n = [&](size_t i) { return i < a.size() ? NOf(a[i]) : 0; };
    auto g = [&](size_t i) { return i < a.size() ? GOf(a[i]) : 0.0f; };
    auto obj = [&](size_t i) { return i < a.size() ? PloRef(s.palo, a[i]) : nullptr; };
    GOAL3* pg = Pgoal(s);
    switch (eopid)
    {
    case 1954: AddGoal(s, n(0)); break;
    case 1955: if (pg) pg->speed = g(0); break;
    case 1956: if (pg) pg->accel = g(0); break;
    case 1957: if (pg) pg->sway = g(0); break;
    case 1959: if (pg) pg->rudderMax = g(0); break;
    case 1961: s.igoal = -1; break;
    case 1962: s.child = obj(0); break;
    case 1964: case 1965: SetHp(s, n(0), n(1)); break;
    case 1967: case 1968: SetHpMax(s, n(0), n(1)); break;
    case 1970: case 1971: SetHpSave(s, n(0), n(1)); break;
    case 1973: SetMode(s, n(0)); break;
    case 1974: SetPhase(s, n(0)); break;
    case 1975: s.group = n(0); break;
    case 1976: s.groupOwner = n(0); break;
    case 1977: s.nav = obj(0); break;
    case 1978: s.owner = obj(0); break;
    case 1979: s.length = g(0); break;
    case 1980: s.balls = obj(0); break;
    case 1981: s.step = g(0); break;
    case 1982: s.range = g(0); break;
    case 1983: s.limit = n(0); break;
    case 1984: s.fraction = g(0); break;
    case 1989:
        if (a.size() >= 4) AddCannon(s, n(0), IdOf(a[1]), IdOf(a[2]), IdOf(a[3]));
        if (s.fInit) ResolveCannons(s);
        break;
    case 1990: s.targetGroup = n(0); break;
    case 1991: s.current = obj(0); break;
    case 1992: s.outer = g(0); break;
    case 1993: s.inner = g(0); break;
    case 1994: s.fx = obj(0); break;
    case 2000:
        // 0x218FC8: the speed by the wind's angle (up to 13)
    {
        // (the argument is a list of floats, or the floats themselves: 0x24B330 gathers up to 13)
        std::vector<float> ag;
        for (const CRef& r : a)
        {
            if (r.m_ppair)
                for (const CPair* pp = r.m_ppair.get(); pp; pp = pp->m_ppairNext.get()) ag.push_back(GOf(pp->m_ref));
            else ag.push_back(GOf(r));
        }
        if (ag.empty()) break;
        s.cspeed = 0;
        for (size_t i = 0; i < ag.size() && i < 13; ++i) s.aspeed[i] = ag[i], ++s.cspeed;
        if (NPrint()) std::printf("ship3: %d speed table set (%d entries, first %.2f)\n", s.palo->oid, s.cspeed, s.aspeed[0]);
    }
        break;
    case 2001: s.focus = obj(0); break;
    case 2002: s.mapScreen = obj(0); break;
    case 2003: s.partner = obj(0); break;
    case 2004: s.threshold = g(0); break;
    case 2007: s.patrol = obj(0); break;
    case 2010: s.busy = n(0); break;
    case 2011: s.n54C = n(0); break;
    case 2012: s.sink = g(0); break;
    default: break;
    }
}

// orig3 0x2139D0 (the defaults) and the post-load 0x213C90 (the cannons resolved) with the loaded options
void InitShip(SHIP3& s)
{
    for (const OPT& o : s.palo->aopt)
        if (o.eopid >= 1954 && o.eopid <= 2016) ApplyLoaded(s, o);
    // the SO's hit points as loaded (EOPIDs 1967 / 1968 set the maximum; the current from it)
    ResolveCannons(s);
    // (0x213C90's end: the resource entry, the first collision entry with the tag of EOPID 2015 (0x23F3B8), +0x560, is
    // flagged 0x10000 (the entry's bit 48): it is left out of the pairwise contacts and collides only through the own
    // contacts, slot 63 0x215008 below; SLY3_SHIPRESOURCE0=1: as before)
    s.psoeResource = nullptr;
    if (s.resourceId != -1 && !std::getenv("SLY3_SHIPRESOURCE0"))
        for (SOE& e : s.palo->asoe)
            if (e.tag == s.resourceId)
            {
                s.psoeResource = &e;
                e.f48 = true;
                break;
            }
    if (SM* psm = dynamic_cast<SM*>(s.anim)) s.psmaShip = PsmaApplySm(psm, s.palo, -1, 0);
    s.hp = s.hpMax;
    s.hpSave = s.hpMax;
    s.fInit = true;
    if (NPrint())
    {
        std::printf("ship3: %d (%p) parent %d (%p) root %p\n", s.palo->oid, (void*)s.palo, s.palo->paloParent ? s.palo->paloParent->oid : -1, (void*)s.palo->paloParent, (void*)PloRoot(s.palo));
        std::printf("ship3: %d init: mode %d phase %d group %d/%d owner %d goals %zu length %.0f range %.0f outer %.0f inner %.0f hp %d/%d balls %d nav %d patrol %d "
                    "cannons %d + %d target group %d\n", s.palo->oid, s.mode, s.phase, s.group, s.groupOwner, s.owner ? s.owner->oid : -1, s.agoal.size(), s.length, s.range,
                    s.outer, s.inner, s.hpMax[0], s.hpMax[1], s.balls ? s.balls->oid : -1, s.nav ? s.nav->oid : -1, s.patrol ? s.patrol->oid : -1, s.ccannon[0], s.ccannon[1],
                    s.targetGroup);
        if (s.shape) std::printf("ship3: %d   outline %d (cid 0x%X)\n", s.palo->oid, s.shape->oid, unsigned(s.shape->Cid()));
        std::printf("ship3: %d   collides with cids", s.palo->oid);
        for (int c : s.acidCollide) std::printf(" 0x%X", c);
        std::printf(", oids");
        for (int o : s.aoidCollide) std::printf(" %d", o);
        std::printf("\n");
        std::printf("ship3: %d   speed table (%d):", s.palo->oid, s.cspeed);
        for (int i = 0; i < s.cspeed; ++i) std::printf(" %.2f", s.aspeed[size_t(i)]);
        std::printf("\n");
        for (const GOAL3& g : s.agoal)
            std::printf("ship3: %d   goal mode %d speed %.0f accel %.0f rudder %.3f smp %.2f %.2f %.2f sway %.2f\n", s.palo->oid, g.mode, g.speed, g.accel, g.rudderMax, g.smp.svFast,
                        g.smp.svSlow, g.smp.dtFast, g.sway);
        if (NPrint() >= 4)
        {
            std::function<void(LO*, int)> dump = [&](LO* p, int depth) {
                if (depth > 3) return;
                if (ALO* pa = dynamic_cast<ALO*>(p))
                    for (LO* c : pa->aploChild)
                    {
                        std::printf("ship3: %d %*schild %d cid 0x%X opts %zu:", s.palo->oid, depth * 2, "", c->oid, unsigned(c->Cid()), c->aopt.size());
                        if (c->Cid() == 0x61) for (const OPT& o : c->aopt) for (const OPTVAL& v : o.aval) std::printf(" %d=%d", o.eopid, v.n);
                        std::printf("\n");
                        dump(c, depth + 1);
                    }
            };
            dump(s.palo, 0);
        }
        if (NPrint() >= 3)
            for (const OPT& o : s.palo->aopt)
                if (o.eopid >= 1954 && o.eopid <= 2016)
                {
                    std::printf("ship3: %d   opt %d:", s.palo->oid, o.eopid);
                    for (const OPTVAL& v : o.aval) std::printf(" [k %d n %d g %.3f %.3f %.3f list %zu]", v.k, v.n, v.ag[0], v.ag[1], v.ag[2], v.alist.size());
                    std::printf("\n");
                }
    }
}

SHIP3* Pship(LO* plo)
{
    if (!plo || !g_psw || !FShip3(plo)) return nullptr;
    auto it = s_mpship.find(plo);
    if (it != s_mpship.end() && it->second.psw == g_psw && it->second.serial == g_psw->serial && it->second.fInit) return &it->second;
    if (!g_psw->fNavReady) return nullptr;   // (not during the load: the loaded options are replayed at the init)
    // orig3 0x213988 (the init's): the battle manager, the global object 1943 of class 0x9B (0x150690 = PloNew)
    if (!PloFindSwObjectByOid(g_psw, 1943))
    {
        try { PloNew(0x9B, g_psw, nullptr, 1943, -1); }
        catch (const std::exception& e) { if (NPrint()) std::printf("ship3: no battle manager: %s\n", e.what()); }
    }
    SHIP3& s = s_mpship[plo];
    s = SHIP3{};
    s.palo = static_cast<ALO*>(plo);
    s.psw = g_psw;
    s.serial = g_psw->serial;
    InitShip(s);
    // (natively made after the load: the reset (vt slot 31) and the pad's enable (slot 83) the original ran then)
    void ResetShipRecord(SHIP3&);
    ResetShipRecord(s);
    if (PpadOf(s.palo)) SetMode(s, 2);
    return &s;
}

// ---- the input (orig3: the input interface +0x134 of the ship, or of its character in phase 1) ----
PADCTL* PpadOf(LO* plo)
{
    PADCTL* ppad = plo ? PpadctlOfSo3(plo) : nullptr;
    return ppad && ppad->nControl == 1 ? ppad : nullptr;
}
PADCTL* PpadSteer(SHIP3& s)
{
    switch (s.mode)
    {
    case 2: return PpadOf(s.palo);
    case 4: return s.phase == 1 ? PpadOf(s.owner) : PpadOf(s.palo);
    default: return nullptr;
    }
}

// ---- the modes (orig3 0x2159D8) and phases (0x215F10) ----
void SetPhase(SHIP3& s, int phase)
{
    if (s.phase == phase) return;
    s.phase = phase;
    if (NPrint()) std::printf("ship3: %d phase %d (t %.2f)\n", s.palo->oid, phase, g_clock.t);
    // 0: the ship takes its character's pad; 1: the character the ship's (0x1F4B00: SW's player rows)
    // (PC, split screen: the ship of seat i >= 1 (split3's player i) gives that seat to its character, view i follows it)
    int iseat = -1;
    if (g_psw && split3::FActive())
        for (int i = 1; i < 4; ++i)
            if (split3::PloPlayer(g_psw, i) == (phase == 0 ? s.owner : s.palo)) iseat = i;
    switch (phase)
    {
    case 0:
        if (iseat >= 1) split3::SetPlayerCharacter(g_psw, iseat, s.palo), split3::GivePlayerObject(g_psw, iseat, s.palo);
        else if (s.owner && g_psw && PloPoCurrent(g_psw) == s.owner) EnablePo(g_psw, s.palo, 1);
        break;
    case 1:
        if (iseat >= 1 && s.owner) split3::SetPlayerCharacter(g_psw, iseat, s.owner), split3::GivePlayerObject(g_psw, iseat, s.owner);
        else if (s.owner && g_psw && PloPoCurrent(g_psw) == s.palo) EnablePo(g_psw, s.owner, 1);
        break;
    default: break;
    }
}

void SetMode(SHIP3& s, int mode)
{
    if (s.mode == mode) return;
    ALO* palo = s.palo;
    if (NPrint()) std::printf("ship3: %d mode %d -> %d (phase %d, t %.2f)\n", palo->oid, s.mode, mode, s.phase, g_clock.t);
    // (0x1F47C8 / 0x273E48: the sailing camera's zone +0x444 forced on while sailing or on the map (modes 2 / 3), off
    // otherwise: the post-load's entry 0x213C90 for the player's ships)
    if (s.focus && s.phase != 2)
    {
        const bool fWas = s.mode == 2 || s.mode == 3, fIs = mode == 2 || mode == 3;
        if (fWas != fIs || s.mode == -1) SetZoneForced(s.focus, fIs ? 1 : 0);
    }
    s.mode = mode;
    s.tMode = g_clock.t;
    s.igoal = IgoalForMode(s, mode);
    if (s.phase != 2)
    {
        if (mode == 2) SetPhase(s, 0);
        else if (mode == 4) SetPhase(s, 1);
        else SetPhase(s, -1);
    }
    s.rudder = 0.0f;
    switch (mode)
    {
    case 0: case 1: case 6:
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        break;
    case 5:
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < s.ccannon[size_t(i)]; ++j) SetCannonState(s.acannon[size_t(i)][size_t(j)], 4, 3.4028235e38f);
        break;
    case 7:
        s.fFaded = false;
        Broadcast3(s.phase == 2 ? 0x10 : 0xC, palo);   // (0x1854C8: a computer's ship sunk 0x10, the player's 0xC)
        break;
    default: break;
    }
    switch (mode)
    {
    case 2: case 3: case 4: case 5:
        if (std::getenv("SLY3_SHIPNOXF")) break;
        if (XF* pxf = PxfPos(palo); pxf && !FXfModeVelocity(pxf->mode)) SetXfMode(palo, pxf, 2);
        if (XF* pxf = PxfRot(palo); pxf && !FXfModeVelocity(pxf->mode)) SetXfMode(palo, pxf, 2);
        SetAloCnstrAng(palo, 4);
        SetAloCnstrLin(palo, 5);
        WakeAlo(palo);
        break;
    default:
        SetAloCnstrAng(palo, 1);
        SetAloCnstrLin(palo, 1);
        break;
    }
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < s.ccannon[size_t(i)]; ++j)
        {
            CANNON3& c = s.acannon[size_t(i)][size_t(j)];
            if (mode == 1 || mode == 4) { if (c.state == 0) SetCannonState(c, 1, 3.4028235e38f); }
            else if (c.state != 4) SetCannonState(c, 0, 3.4028235e38f);
        }
    // the ship's machine (its sails / the sinking): 0x239DB8 SeekSma(+0x414, D_00325BC0[mode])
    static const int s_agoalShip[8] = {146, 1946, 1947, 1948, 1949, 1950, 1951, 149};
    // (natively the machine's transitions take frames: a player's ship told to sail and to fight in the same frame (the
    // Galleon Duel's round start: option 983 then method 2019) would seek the battle state 1949 without arriving at the
    // sailing one 1947, whose arrival puts the character at the helm (SCPT 44238) and makes 1949's put him on the deck
    // (SCPT 44240's 2162 = "came from 1947"): the sailing state is reached first)
    if (s.psmaShip && mode == 4 && s.phase != 2 && OidSmaCur(s.psmaShip) != 1947 && OidSmaGoal(s.psmaShip) == 1947) SnapSmaGoal(s.psmaShip, 1947);
    if (s.psmaShip && mode >= 0 && mode < 8) SeekSma(s.psmaShip, s_agoalShip[mode]);
}

// orig3 0x2171E8: the computer's sailing submode (0 the patrol curve, 1 back to it, 2 chase, 3 broadside, 4 back on the mesh)
void PlanRoute(SHIP3& s, const glm::vec3& from, const glm::vec3& to);
void SetSubmode(SHIP3& s, int sub)
{
    if (s.submode == sub) return;
    s.submode = sub;
    s.tSubmode = g_clock.t;
    if (sub == 0)
    {
        if (const CRV* pcrv = s.patrol ? PcrvFromLo(s.patrol) : nullptr)
        {
            glm::vec3 pos, dir;
            float gN, sN;
            NearestCrvAll(pcrv, PosOf(s.palo), &pos, &dir, &gN, &sN, nullptr);
            s.routeS = sN;
        }
    }
    else if (sub == 1)
    {
        glm::vec3 to = PosOf(s.palo);
        if (const CRV* pcrv = s.patrol ? PcrvFromLo(s.patrol) : nullptr)
        {
            glm::vec3 dir;
            float gN, sN;
            NearestCrvAll(pcrv, PosOf(s.palo), &to, &dir, &gN, &sN, nullptr);
        }
        PlanRoute(s, PosOf(s.palo), to);
    }
}

// orig3 0x217328: the route between two points (the path planner 0x1ED1B0 not ported: the straight route, its
// fallback), dropped to the ship's height
void PlanRoute(SHIP3& s, const glm::vec3& from, const glm::vec3& to)
{
    s.route = {from, to};
    const float z = PosOf(s.palo).z;
    for (glm::vec3& p : s.route) p.z = z;
    s.routeS = 0.0f;
}

// ---- the motion (orig3 0x216020) ----
float WindHeading()
{
    // SW +0x4E48 (EOPID 997): the wind's heading
    static SW* s_psw = nullptr;
    static uint32_t s_serial = 0;
    static float s_rad = 0.0f;
    if (s_psw != g_psw || (g_psw && s_serial != g_psw->serial))
    {
        s_psw = g_psw;
        s_serial = g_psw ? g_psw->serial : 0;
        s_rad = 0.0f;
        if (g_psw)
            for (const OPT& o : g_psw->aoptSw)
                if (o.eopid == 997 && !o.aval.empty()) s_rad = o.aval[0].ag[0];
    }
    return s_rad;
}
// 0x219000: the heading against the wind (0: running before it)
float WindAngle(SHIP3& s) { return RadNormalize(Heading(s.palo) - (WindHeading() + kPi)); }
// 0x219058: the speed factor at |angle| / pi
float SpeedFactor(const SHIP3& s, float angle)
{
    const int last = std::max(1, s.cspeed - 1);
    const float u = std::clamp(std::fabs(angle) * 0.31830987f, 0.0f, 1.0f);
    const float step = 1.0f / float(last);
    const int i = std::min(int(u * float(last)), last);
    const int j = std::min(i + 1, last);
    const float f = (u - float(i) * step) / step;
    return (1.0f - f) * s.aspeed[size_t(i)] + f * s.aspeed[size_t(j)];
}

void StartBattleNear(SHIP3& s);   // 0x21A068 (the manager)
bool FBattleOn();

// orig3 0x216350: the pad's steering (mode 2: the ship's input; mode 4: its character's)
void SteerByPad(SHIP3& s)
{
    PADCTL* ppad = PpadSteer(s);
    if (s.mode == 2)
    {
        if (ppad && GrfPadctlTakePressed(ppad, 0x6D0, 0.1f) && s.mapScreen)
        {
            SetMode(s, 3);
            return;
        }
        bool fFound = false;
        const float outerSq = s.outer * s.outer, innerSq = s.inner * s.inner;
        for (SHIP3* po : AshipWorld())
        {
            if (po == &s || po->mode == 7) continue;
            const glm::vec3 d = PosOf(s.palo) - PosOf(po->palo);
            const float dd = d.x * d.x + d.y * d.y;
            if (outerSq < dd) continue;
            if (dd < innerSq || (ppad && GrfPadctlTakePressed(ppad, 0x20, 0.1f)))
            {
                fFound = true;
                break;
            }
        }
        if (fFound && !FBattleOn()) StartBattleNear(s);
    }
    float dir = 0.0f;
    const unsigned grf = GrfShipSteer3(s.palo);
    if (grf & 1) dir = 1.0f;
    if (grf & 2) dir -= 1.0f;
    GOAL3* pg = Pgoal(s);
    if (!pg) return;
    SMP smp = pg->smp;
    const float gain = pg->rudderMax;
    if (dir != 0.0f)
    {
        const glm::vec3 w = VecToLocal(s.palo, AngVelAlo(s.palo));
        if (w.z * dir < 0.0f) smp.svFast *= 2.5f;
    }
    else smp.svFast *= 2.5f;
    s.rudder = GSmooth(s.rudder, dir * gain, g_clock.dt, &smp, nullptr);
    if (s.type != -1 && g_clock.t - s.tType > s.durType && s.durType >= 0.0f) SetType(s, -1);   // 0x2155F0
}

// orig3 0x217470: the heading error toward a look-ahead point on a curve (dir 0: backwards); the route's
// parameter updated
float SteerAlongPoints(SHIP3& s, const std::vector<glm::vec3>& apos, int dir)
{
    const glm::vec3 pos = PosOf(s.palo);
    if (apos.size() < 2) return 0.0f;
    // the nearest point on the polyline and its tangent
    float best = 3.4e38f, sBest = 0.0f, sAcc = 0.0f;
    glm::vec3 ptBest = apos[0], tanBest(1.0f, 0.0f, 0.0f);
    for (size_t i = 0; i + 1 < apos.size(); ++i)
    {
        const glm::vec3 a = apos[i], b = apos[i + 1];
        const glm::vec3 ab = b - a;
        const float l = glm::length(ab);
        if (l < 1e-4f) continue;
        const float t = std::clamp(glm::dot(pos - a, ab) / (l * l), 0.0f, 1.0f);
        const glm::vec3 p = a + ab * t;
        const float d = glm::length(pos - p);
        if (d < best) best = d, ptBest = p, tanBest = ab / l, sBest = sAcc + t * l;
        sAcc += l;
    }
    s.routeS = sBest;
    glm::vec3 tan = dir ? tanBest : -tanBest;
    const float look = s.length * 1.5f;
    const float ahead = std::clamp(look - LenXY(pos - ptBest), 0.0f, look);
    glm::vec3 to = ptBest + tan * ahead - pos;
    const float l = glm::length(to);
    const glm::vec3 d = l < 1e-4f ? tan : to / l;
    return RadNormalize(Atan2Of(d) - Heading(s.palo));
}
float SteerAlongCurve(SHIP3& s, LO* pcurve, int dir)
{
    const CRV* pcrv = pcurve ? PcrvFromLo(pcurve) : nullptr;
    if (!pcrv) return 0.0f;
    const glm::vec3 pos = PosOf(s.palo);
    glm::vec3 pt, tan;
    float gN, sN;
    NearestCrvS(pcrv, pos, s.routeS, &pt, &tan, &gN, &sN, nullptr);
    s.routeS = sN;
    if (!dir) tan = -tan;
    if (glm::length(tan) > 1e-4f) tan = glm::normalize(tan);
    const float look = s.length * 1.5f;
    const float ahead = std::clamp(look - LenXY(pos - pt), 0.0f, look);
    const glm::vec3 to = pt + tan * ahead - pos;
    const float l = glm::length(to);
    const glm::vec3 d = l < 1e-4f ? tan : to / l;
    return RadNormalize(Atan2Of(d) - Heading(s.palo));
}

const NAVREGION* PregNav(SHIP3& s)
{
    LOXMESH* pmesh = s.nav ? dynamic_cast<LOXMESH*>(s.nav) : nullptr;
    return pmesh ? PregionOfMesh(pmesh) : nullptr;
}
bool FOnNav(SHIP3& s, const glm::vec3& pos)
{
    const NAVREGION* preg = PregNav(s);
    return !preg || ItriNav(preg, pos, 0) >= 0;
}

// orig3 0x218E98: the nearest other ship that is not of this group (and, sailing, not owned by another group)
SHIP3* PshipNearest(SHIP3& s)
{
    SHIP3* pbest = nullptr;
    float best = 3.4e38f;
    for (SHIP3* po : AshipWorld())
    {
        if (po == &s) continue;
        if (s.group == po->group && s.group != 0) continue;
        if (po->groupOwner != 0 && po->groupOwner != s.group && s.mode == 2) continue;
        if (po->phase == 2 && s.phase == 2) continue;   // (+0x1B8 == 2: the computer's ships do not chase each other)
        const glm::vec3 d = PosOf(s.palo) - PosOf(po->palo);
        const float dd = glm::dot(d, d);
        if (dd < best) best = dd, pbest = po;
    }
    return pbest;
}

void ArmBroadsides(SHIP3& s);   // 0x217650

// orig3 0x216680: the computer's sailing (phase 2)
void SailByComputer(SHIP3& s)
{
    const glm::vec3 pos = PosOf(s.palo);
    s.ptarget = PshipNearest(s);
    glm::vec3 tpos = pos;
    float dist = 0.0f;
    if (s.ptarget)
    {
        tpos = PosOf(s.ptarget->palo);
        dist = LenXY(tpos - pos);
    }
    for (int guard = 0; guard < 8; ++guard)
    {
        const int sub = s.submode;
        switch (sub)
        {
        case 0:
            if (s.ptarget && s.ptarget->phase != 2 && dist < s.ptarget->outer) SetSubmode(s, 2);
            break;
        case 1:
        {
            float total = 0.0f;
            for (size_t i = 0; i + 1 < s.route.size(); ++i) total += glm::length(s.route[i + 1] - s.route[i]);
            if (s.ptarget && s.ptarget->phase != 2 && dist < s.ptarget->outer) SetSubmode(s, 2);
            else if (total - s.routeS < s.length) SetSubmode(s, 0);
            break;
        }
        case 2:
            if (!s.ptarget && s.patrol) { SetSubmode(s, 1); break; }
            if (!FOnNav(s, pos)) { SetSubmode(s, 4); break; }
            if (s.mode == 4 && dist < s.range * 0.75f) SetSubmode(s, 3);
            break;
        case 4:
            if (FOnNav(s, pos)) SetSubmode(s, 2);
            break;
        case 3:
            if (!s.ptarget && s.patrol) { SetSubmode(s, 1); break; }
            if (!FOnNav(s, pos)) { SetSubmode(s, 4); break; }
            if (s.range < dist) { SetSubmode(s, 2); break; }
            if (s.sideFire >= 0 && s.sideFire < 2)
            {
                if (g_clock.t - s.tSideFire > 45.0f) s.sideFire = s.sideFire ? 0 : 1, s.tSideFire = g_clock.t;   // 0x217938
            }
            else s.sideFire = NRandInRange(0, 1), s.tSideFire = g_clock.t;
            if (s.sideFire >= 2 || s.sideFire < 0) SetSubmode(s, 0);
            break;
        default:
            SetSubmode(s, s.patrol ? 0 : 2);
            break;
        }
        if (s.submode == sub) break;
    }
    float angle = 0.0f;
    switch (s.submode)
    {
    case 0: angle = SteerAlongCurve(s, s.patrol, s.patrolDir); break;
    case 1: angle = SteerAlongPoints(s, s.route, 1); break;
    case 2: case 4:
    {
        const bool fReplan = s.route.size() < 2 || s.length < LenXY(s.route.back() - tpos);
        if (fReplan) PlanRoute(s, pos, tpos);
        angle = SteerAlongPoints(s, s.route, 1);
        break;
    }
    case 3:
        if (s.ptarget)
        {
            const float side = s.sideFire == 0 ? -kHalfPi : s.sideFire == 1 ? kHalfPi : 0.0f;
            angle = RadNormalize(side + Atan2Of(ToLocal(s.palo, tpos)));
        }
        break;
    default: break;
    }
    // the avoidance of the other computer's ships 0.5 s ahead
    glm::vec3 avoid(0.0f);
    const glm::vec3 ahead = pos + VelAloPos(s.palo) * 0.5f;
    for (SHIP3* po : AshipWorld())
    {
        if (po == &s || po->phase != 2) continue;
        const glm::vec3 opos = PosOf(po->palo);
        glm::vec3 rel = pos - opos;
        glm::vec3 d2 = ahead - (opos + VelAloPos(po->palo) * 0.5f);
        rel.z = 0.0f;
        d2.z = 0.0f;
        // the closest point to the origin on [rel, d2]
        const glm::vec3 seg = d2 - rel;
        const float ll = glm::dot(seg, seg);
        const float t = ll < 1e-8f ? 0.0f : std::clamp(-glm::dot(rel, seg) / ll, 0.0f, 1.0f);
        glm::vec3 closest = rel + seg * t;
        const float L = glm::length(closest);
        closest = L < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f) : closest / L;
        const float gap = std::max(0.0f, L - po->length - s.length);
        if (gap < s.length) avoid += closest * ((1.0f - gap / s.length) * 0.5f * 6000.0f);
    }
    avoid.z = 0.0f;
    const bool fAvoid = !(FFloatsNear(0.0f, avoid.x, 1e-4f) && FFloatsNear(0.0f, avoid.y, 1e-4f));
    if (fAvoid)
    {
        glm::vec3 want = pos + avoid, clamped;
        if (const NAVREGION* preg = PregNav(s); preg && FNavPoint(preg, want, &clamped, 1)) avoid = clamped - pos;
        angle = Atan2Of(VecToLocal(s.palo, avoid));
    }
    GOAL3* pg = Pgoal(s);
    if (!pg) return;
    SMP smp = pg->smp;
    float gain = pg->rudderMax;
    if (fAvoid)
    {
        const float L = glm::length(avoid);
        const float k = 1.5f + L * 0.00025f;
        smp.svFast *= k;
        gain *= k;
    }
    else if (s.submode == 0) gain *= 10.0f, smp.svFast *= 10.0f;
    else if (s.submode == 4)
    {
        gain *= 5.0f;
        smp.svFast *= 5.0f;
        if (kHalfPi <= std::fabs(angle)) s.speedScale = 0.5f;
    }
    else
    {
        const glm::vec3 w = VecToLocal(s.palo, AngVelAlo(s.palo));
        if (w.z * angle < 0.0f) smp.svFast *= 2.5f;
    }
    s.rudder = GSmooth(s.rudder, GLimitAbs(angle, gain), g_clock.dt, &smp, nullptr);
    if (s.mode == 4 && g_clock.t - s.tMode > 5.0f) ArmBroadsides(s);
}

void UpdateMotion(SHIP3& s)   // orig3 0x216020
{
    s.speedScale = 0.0f;
    for (int guard = 0; guard < 4; ++guard)
    {
        const int mode = s.mode;
        switch (mode)
        {
        case 2: case 4:
            s.windAngle = WindAngle(s);
            if (s.phase == 2) SailByComputer(s);
            else SteerByPad(s);
            break;
        case 3:
            // the map (Triangle and the others close it again: 0x1A7A80 with 0x6F0)
            if (PADCTL* ppad = PpadOf(s.palo); !ppad || GrfPadctlTakePressed(ppad, 0x6F0, 0.1f)) SetMode(s, 2);
            break;
        default: break;
        }
        if (s.mode == mode || s.mode == -1) break;
    }
    float scale = s.speedScale != 0.0f ? s.speedScale : SpeedFactor(s, s.windAngle);
    GOAL3* pg = Pgoal(s);
    const float speedGoal = pg ? pg->speed : 0.0f;
    auto turnRate = [&](float v) { return s.length > 0.0f ? v * std::sin(s.rudder) / s.length : 0.0f; };
    switch (s.mode)
    {
    case 2:
        if (s.windOwn) s.speed = GSmooth(s.speed, 2000.0f, g_clock.dt, nullptr, nullptr);
        else s.speed = speedGoal * scale;
        s.yawRate = turnRate((s.phase == 2 && s.submode == 4) || s.windOwn ? speedGoal : speedGoal * std::min(1.0f, scale));
        break;
    case 4:
        s.speed = speedGoal * scale;
        if (NPrint() >= 4) std::printf("ship3: %d motion goal %.0f scale %.3f (override %.3f wind %.3f) igoal %d\n", s.palo->oid, speedGoal, scale, s.speedScale, s.windAngle, s.igoal);
        s.yawRate = turnRate(speedGoal);
        if (s.type == 0x3E) s.yawRate *= 1.75f;
        break;
    case 5:
    {
        static const SMP s_smp{2000.0f, 500.0f, 1.5f};   // D_00325C10 (an approach to 0)
        s.speed = GSmooth(s.speed, 0.0f, g_clock.dt, &s_smp, nullptr);
        s.yawRate = 0.0f;
        break;
    }
    case 7:
        // sunk: the hull settles (natively by the height spring: the sinking clip 149 of the listener +0x414 is not
        // played), then fades out (0x2412C8, 1 s) and leaves the battle (slot 12 0x213FD0 -> 0x21A450)
        s.speed = 0.0f;
        s.yawRate = 0.0f;
        if (!s.fFaded && g_clock.t - s.tMode > 2.5f && s.palo->fInWorld)
        {
            s.fFaded = true;
            FadeAloOut(s.palo, 1.0f);
            if (s.busy == 2) OnShipOut3(s.palo);
        }
        break;
    default:
        s.speed = 0.0f;
        s.yawRate = 0.0f;
        break;
    }
}

// ---- the cannons: the broadsides (0x2183F8 / 0x217650 / 0x218C00), the timers (0x217D90), the shot (0x2185B0) ----
// an INTEREST's group (EOPID 252, its +0xE4: the loaded option; natively stored at the Sly 2 twin's +0xF4)
int NInterestGroup(LO* plo)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == 252 && !o.aval.empty()) return o.aval[0].n;
    return int(plo->U32Field(0, 0xF4, plo->U32Field(0, 0xE4, 0)));
}
// 0x218C00: the aim point of a side: the target objects (class 0x11, the list D_00479AD8) of the ship's target group
// on that side within 55 degrees of the beam, the ships' first, the nearer first
LO* PloAimTarget(SHIP3& s, int side)
{
    if (s.current) return s.current;
    if (!g_psw) return nullptr;
    LO* pbest = nullptr;
    float best = 3.4e38f;
    for (LO* plo : g_psw->mpcidaplo[0x11])
    {
        if (!plo || !plo->fInWorld) continue;
        if (NInterestGroup(plo) != s.targetGroup) continue;
        if (plo->paloParent == s.palo) continue;
        const glm::vec3 local = ToLocal(s.palo, PosOf(plo));
        const bool fOk = side == 0 ? local.y > 0.0f : local.y < 0.0f;
        if (!fOk) continue;
        float score = std::fabs(RadNormalize((side == 0 ? -kHalfPi : kHalfPi) + Atan2Of(local)));
        if (score >= 0.9599312f) continue;
        if (!(plo->paloParent && FShip3(plo->paloParent))) score += 0.3490659f;
        score += LenXY(local) * 1.7453294e-05f;
        if (score < best) best = score, pbest = plo;
    }
    return pbest;
}
void RestartSide(SHIP3& s, CANNON3* pfirst, int side)   // 0x2183F8
{
    std::vector<CANNON3*> a{pfirst};
    for (int j = 0; j < s.ccannon[size_t(side)]; ++j)
    {
        CANNON3& c = s.acannon[size_t(side)][size_t(j)];
        if (&c != pfirst && c.state == 1) a.push_back(&c);
    }
    for (size_t k = 1; k < a.size(); ++k) std::swap(a[k], a[size_t(NRandInRange(1, int(a.size()) - 1))]);
    const LM& range = s.type == 0x41 ? s.delayType : s.delayFire;
    float target = range.gMax;
    const float d = a.size() > 1 ? (target - range.gMin) / float(a.size() - 1) : 0.0f;
    float t = g_clock.t;
    for (CANNON3* pc : a)
    {
        SetCannonState(*pc, 2, t);
        pc->delay = target;
        target -= d;
        t += s.step;
    }
    s.aimTarget[size_t(side)] = PloAimTarget(s, side);
}
// orig3 0x217650: a broadside at the nearest ship on each side within the range and the guns' cone
void ArmBroadsides(SHIP3& s)
{
    struct CAND { SHIP3* ps = nullptr; CANNON3* pc = nullptr; float d = 3.4e38f; } ac[2];
    bool fAny = false;
    for (int h = 0; h < 2; ++h)
    {
        for (int j = 0; j < s.ccannon[size_t(h)]; ++j)
            if (s.acannon[size_t(h)][size_t(j)].state == 1) { ac[h].pc = &s.acannon[size_t(h)][size_t(j)]; break; }
        if (ac[h].pc) fAny = true;
    }
    if (!fAny) return;
    for (SHIP3* po : AshipWorld())
    {
        if (po == &s) continue;
        if (s.group == po->group && s.group != 0) continue;
        if (po->busy || po->mode == 7) continue;
        const glm::vec3 local = ToLocal(s.palo, PosOf(po->palo));
        const int side = local.y <= 0.0f ? 1 : 0;
        CAND& c = ac[side];
        if (!c.pc) continue;
        const float d = LenXY(local);
        if (s.range < d || c.d < d) continue;
        const float root = std::tan(s.yawA);
        const float bias = s.length / (root + root);
        const float a = std::atan(std::fabs(local.x) / (std::fabs(local.y) + bias));
        const float b = std::atan((s.range * root + po->length) / (s.range + bias));
        if (a < b) c.ps = po, c.d = d;
    }
    for (int g = 0; g < 2; ++g)
        if (ac[g].ps && ac[g].pc) RestartSide(s, ac[g].pc, g);
}

LO* PloTakeBall(SHIP3& s)
{
    if (!s.balls || !g_psw) return nullptr;
    LO* po = PloAcquireProxyObject(g_psw, s.balls, false);
    return po && FIsLoDerivedFrom(po, sly3::CID_ALO) ? po : nullptr;
}
// a launch velocity reaching `to` from `from` in dt under gravity (0x212628)
glm::vec3 VelLaunch(const glm::vec3& from, const glm::vec3& to, const glm::vec3& grav, float dt)
{
    if (dt <= 1e-4f) return to - from;
    return (to - from) / dt - grav * (0.5f * dt);
}
// orig3 0x2185B0: a shot from an armed cannon at its side's aim point (led by the target's motion), within the guns' arc
bool FFireCannon(SHIP3& s, CANNON3& c, int side)
{
    if (side == s.sideFire) s.sideFire = -1;
    if (!s.balls || !c.cannon || !c.muzzle || c.state != 2) return false;
    LO* pball = PloTakeBall(s);
    if (!pball) return false;
    ALO* pb = static_cast<ALO*>(pball);
    const glm::mat4 mMuzzle = MatOf(c.muzzle);
    SetAloRotation(pb, glm::mat3(mMuzzle));
    SetAloPos(pb, glm::vec3(mMuzzle[3]));
    if (FIsLoDerivedFrom(pball, 0x59)) SetProjectileLauncher(pb, s.palo);
    float range = s.range * (s.type == 0x42 ? 1.5f : 1.0f);
    LO* ptgt = s.aimTarget[size_t(side)];
    float d = range;
    if (ptgt) d = std::min(range, glm::length(PosOf(pball) - PosOf(ptgt)));
    const float dt = s.flight.x + d * (s.flight.y + d * s.flight.z);
    const glm::vec3 muzzleLocal = ToLocal(s.palo, glm::vec3(mMuzzle[3]));
    glm::vec3 world;
    if (ptgt)
    {
        glm::vec3 offset(0.0f);
        if (ptgt->paloParent && FShip3(ptgt->paloParent))
            if (SHIP3* pt = PshipOf(ptgt->paloParent))
            {
                const float half = pt->length * 0.5f;
                float u = std::clamp(muzzleLocal.x / s.length + 0.5f, 0.0f, 1.0f);
                if (glm::dot(glm::vec3(MatWorld(s.palo)[0]), glm::vec3(MatWorld(pt->palo)[0])) < 0.0f) u = 1.0f - u;
                offset.x = (1.0f - u) * -half + u * half;
            }
        // (0x28F680: the target's motion over the flight: its root's, the ship it is part of; natively the point's own
        // prediction has no velocity of its own, the ship sailed ~1500 away during a flight and every ball missed)
        LO* proot = ptgt->paloParent && FShip3(ptgt->paloParent) ? ptgt->paloParent : ptgt;
        if (FIsLoDerivedFrom(proot, S3CID(0x30)))
        {
            ALO* pr = static_cast<ALO*>(proot);
            const glm::mat3 r = glm::mat3(MatWorld(pr));
            const glm::vec3 w = AngVelAlo(pr);
            const glm::vec3 posT = PosOf(ptgt) + r * offset;
            const glm::vec3 posR = PosOf(pr);
            // the point turned with the ship's yaw over the flight, moved by its velocity
            const glm::mat3 turn = cmm::BuildRotateMatrixAxis(w.z * dt, glm::vec3(0.0f, 0.0f, 1.0f));
            world = posR + VelAloPos(pr) * dt + turn * (posT - posR);
        }
        else world = PredictProjectileAim(ptgt, offset, dt);
    }
    else world = glm::vec3(mMuzzle * glm::vec4(d, 0.0f, 0.0f, 1.0f));
    // the arc of the guns about the muzzle's up axis
    glm::vec3 local = glm::transpose(glm::mat3(mMuzzle)) * (world - glm::vec3(mMuzzle[3]));
    LM yaw{0.0f, 0.0f};
    if (ptgt)
    {
        const bool fFlip = side == 0 ? muzzleLocal.x < 0.0f : 0.0f < muzzleLocal.x;
        yaw = fFlip ? LM{-s.yawB, s.yawA} : LM{-s.yawA, s.yawB};
    }
    const float a = std::clamp(RadNormalize(Atan2Of(local)), yaw.gMin, yaw.gMax);
    local.y = local.x * std::tan(a);
    world = glm::vec3(mMuzzle * glm::vec4(local, 1.0f));
    const glm::vec3 mpos = glm::vec3(mMuzzle[3]);
    const glm::vec3 v = world - mpos;
    const float L = glm::length(v);
    const glm::vec3 dir = L < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f) : v / L;
    world = mpos + dir * std::min(range, L);
    glm::vec3 grav(0.0f, 0.0f, -2275.0f);
    if (SOST* pst = Psost(pb)) grav = pst->dvGravity;
    SetAloVelocityTo(pb, VelLaunch(PosOf(pball), world, grav, dt));
    if (NPrint() >= 3) g_aballTrace3.emplace_back(pb, g_clock.t);
    if (NPrint())
        std::printf("ship3: %d (%p) fires side %d from %d at %d (parent %p) (%.0f %.0f %.0f) d %.0f dt %.2f t %.2f\n", s.palo->oid, (void*)s.palo, side, c.cannon->oid,
                    ptgt ? ptgt->oid : -1, ptgt ? (void*)ptgt->paloParent : nullptr, world.x, world.y, world.z, d, dt, g_clock.t);
    return true;
}
void UpdateCannons(SHIP3& s)   // orig3 0x217D90
{
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < s.ccannon[size_t(i)]; ++j)
        {
            CANNON3& c = s.acannon[size_t(i)][size_t(j)];
            for (int guard = 0; guard < 4; ++guard)
            {
                const int st = c.state;
                if (st == 2 && c.timer < g_clock.t)
                {
                    if (FFireCannon(s, c, i)) SetCannonState(c, 3, g_clock.t + c.delay);
                    else SetCannonState(c, 1, 3.4028235e38f);
                }
                else if (st == 3 && c.timer < g_clock.t) SetCannonState(c, 1, 3.4028235e38f);
                if (c.state == st) break;
            }
        }
}

// ---- the ship's gauge (class 0xA6, its +0x1A8 child: EOPID 1962; orig3 0x21B4B8..0x21BC58) ----
// Shown while the ship sails / fights (modes 2..5: 0x215958 slot 56, else slot 54); its two bars (parts 0x792 / 0x793)
// are the sides' hit points (0x21B6C0: +0x190 / +0x198), a loss plays the bar's fall part (0x795 / 0x796) and the bar
// then eases down (0x21B7E8 state 3: D_00325EA8-style {100, 0, 0.5}); none left: hidden. In the two-player battle
// (D_0047A424) the gauge moves to its player's corner (0x21BC58: (470, 136) / (-6, 373)). The power-up's timer part
// 0x794 (0x21BA40) is not shown natively.
struct SHIPGAUGE3
{
    int mode = -1;               // +0x138
    int idFace = -1;             // +0x144 (the face part, 0x7AF: the default +0x148)
    int iseat = -2;              // (the corner it was put in: 0x21BC58)
    float cur[2]{0.0f, 0.0f};    // +0x120
    float tgt[2]{0.0f, 0.0f};    // +0x128
};
std::unordered_map<const LO*, SHIPGAUGE3> s_mpgaugeShip;
bool Battle2P();
void UpdateShipGauge(SHIP3& s)
{
    LO* pg = s.child;
    if (!pg) return;
    SHIPGAUGE3& g = s_mpgaugeShip[pg];
    const bool fShown = s.mode >= 2 && s.mode <= 5;
    for (int i = 0; i < 2; ++i) g.tgt[i] = s.hpMax[size_t(i)] > 0 ? float(s.hp[size_t(i)]) / float(s.hpMax[size_t(i)]) : 0.0f;
    if (!fShown)
    {
        if (g.mode != -1) { HideScreenObj(pg); g.mode = -1; }
        return;
    }
    // the player's index (0x21B720: the 2P flag D_0047A424 and the ship's pad row, or its character's): the corner
    int iseat = -1;
    if (g_psw && split3::FActive() && Battle2P())
        for (int i = 0; i < split3::CView(); ++i)
            if (split3::PloPlayer(g_psw, i) == s.palo || (s.owner && split3::PloPlayer(g_psw, i) == s.owner)) iseat = i;
    if (iseat != g.iseat)
    {
        g.iseat = iseat;
        static const float s_aofs[4][2] = {{470.0f, 136.0f}, {-6.0f, 373.0f}, {470.0f, 373.0f}, {-6.0f, 136.0f}};   // D_00325E.. (PC: 3 / 4)
        if (iseat >= 0) SetScreenObjRootXY(pg, true, 1.0f, s_aofs[iseat][0], s_aofs[iseat][1]);
        else SetScreenObjRootXY(pg, false, 1.0f, 0.0f, 0.0f);
    }
    if (g.mode == -1)
    {
        ShowScreenObj(pg, -1);
        g.mode = 1;
        g.idFace = -1;
        if (NPrint())
        {
            std::printf("ship3: %d gauge %d shown (seat %d), options:", s.palo->oid, pg->oid, iseat);
            for (const OPT& o : pg->aopt) std::printf(" %d=%d", o.eopid, o.aval.empty() ? -1 : o.aval[0].n);
            std::printf("\n");
        }
        // (0x21BA40: the default face +0x148, EOPID 2023 (optid 1795; 0x21B4B8: 0x7AF): the Galleon Duel's gauges 57297 /
        // 57295 load Sly's 57888 / Bentley's 57889, the story's none)
        int idFace = 0x7AF;
        for (const OPT& o : pg->aopt)
            if (o.eopid == 2023 && !o.aval.empty()) idFace = o.aval[0].n;
        SelectScreenObjPartHandle(pg, idFace, &g.idFace);
        for (int i = 0; i < 2; ++i)
        {
            g.cur[i] = g.tgt[i];
            SelectScreenObjPart(pg, 0x792 + i);   // (0x262450)
            SetScreenObjFraction(pg, 0x792 + i, g.cur[i]);
        }
        return;
    }
    static const SMP s_smp{100.0f, 0.0f, 0.5f};
    for (int i = 0; i < 2; ++i)
    {
        if (g.tgt[i] < g.cur[i] - 1e-4f && g.mode == 1)
        {
            SelectScreenObjPart(pg, 0x795 + i);   // (state 2: the fall)
            g.mode = 2;
        }
        if (g.cur[i] < g.tgt[i]) g.cur[i] = g.tgt[i];
        else g.cur[i] = GSmooth(g.cur[i], g.tgt[i], g_clock.dt, &s_smp, nullptr);
        SetScreenObjFraction(pg, 0x792 + i, g.cur[i]);
    }
    if (g.mode == 2 && std::fabs(g.cur[0] - g.tgt[0]) < 1e-3f && std::fabs(g.cur[1] - g.tgt[1]) < 1e-3f) g.mode = 1;
}

// The characters a ship's scripts parent to its spots (method 35 = 0x1A58D0 with the flags 0): the battle script 44240 puts
// the character on the deck spot 44236 (for 1.4 s, then 51658 lets him go with the flags 15) and the helmsman at the helm
// 44234 (for the whole battle). Natively a character parented to an object that moves does not follow it (its STEP
// physics keeps its world place): while a character of the ship's (its owner, the helmsmen) has a parent in the ship's
// tree, it is kept at that parent's place (method 39 put it there), turned with the ship and given the ship's velocity.
// Once let go, the deck carries him by the original's support: the hull barrier 26725 is his floor and its velocity his
// (STEPPHYS's floor), as in the story's battles. (Before round 6 a box-heuristic carry moved anyone on a copy's deck.)
bool Battle2P();
void CarryDeck(SHIP3& s)
{
    if (NPrint() >= 2 && s.owner && FIsLoDerivedFrom(s.owner, S3CID(0x30)))
    {
        static std::unordered_map<const LO*, float> s_mpt;
        float& t = s_mpt[s.palo];
        static const bool s_fEvery = std::getenv("SLY3_SHIPFLOORDBG") != nullptr;
        if (g_clock.t - t >= 1.0f || s_fEvery)
        {
            t = g_clock.t;
            STEPPHYS& sp = StepPhysOf(static_cast<ALO*>(s.owner));
            if (XP* x = sp.pxpFloor)
                std::printf("ship3: %d's owner %d floor %d (root %d) vel0 %.0f %.0f vel1 %.0f %.0f\n", s.palo->oid, s.owner->oid, x->axpd[1].palo ? x->axpd[1].palo->oid : -1,
                            x->axpd[1].paloRoot ? x->axpd[1].paloRoot->oid : -1, x->axpd[0].vel.x, x->axpd[0].vel.y, x->axpd[1].vel.x, x->axpd[1].vel.y);
            else
            {
                const glm::vec3 l = glm::vec3(glm::inverse(MatWorld(s.palo)) * glm::vec4(PosOf(s.owner), 1.0f));
                std::printf("ship3: %d's owner %d no floor (local %.0f %.0f %.0f, clone %d)\n", s.palo->oid, s.owner->oid, l.x, l.y, l.z, s.palo->ploCloneOf ? 1 : 0);
            }
            for (LO* ch : s.palo->aploChild)
                if (ch && (ch->oid == 26725 || ch->oid == 44249))
                    if (OXA* px = Poxa(static_cast<ALO*>(ch)))
                    {
                        const glm::vec3 l = glm::vec3(glm::inverse(MatWorld(s.palo)) * glm::vec4(px->posCenter, 1.0f));
                        std::printf("ship3: %d's %d box centre local %.0f %.0f %.0f radius %.0f inworld %d\n", s.palo->oid, ch->oid, l.x, l.y, l.z, px->sRadius, int(ch->fInWorld));
                    }
        }
    }
    if (std::getenv("SLY3_SHIPNOCARRY")) return;
    const glm::mat4 m = MatWorld(s.palo);
    if ((s.mode == 2 || s.mode == 4 || s.mode == 5) && s.matPrev[3][3] != 0.0f && g_psw)
    {
        const glm::mat4 d = m * glm::inverse(s.matPrev);
        const glm::vec3 vel = VelAloPos(s.palo);
        auto carry = [&](LO* plo) {
            if (!plo || !plo->fInWorld || !FIsLoDerivedFrom(plo, S3CID(0x30))) return;
            LO* pparent = plo->paloParent;
            if (!pparent || pparent == s.palo || !FLoInParentChain(pparent, s.palo)) return;
            ALO* pa = static_cast<ALO*>(plo);
            SetAloPos(pa, PosLoWorld(pparent));
            SetAloRotation(pa, glm::mat3(d) * glm::mat3(MatWorld(pa)));   // (the heading turned with the ship)
            SetAloVelocityTo(pa, vel);
        };
        carry(s.owner);
        for (int oid : {26, 31, 29, 27})   // (the helmsmen: Murray, Penelope; PC: Dimitri, the Panda King's daughter)
            for (LO* p : AploOfOid(g_psw, oid))
                if (p->oid == oid) carry(p);
    }
    s.matPrev = m;
}

// ---- the update (orig3 0x215FD8, vt slot 18) ----
void TraceBalls()
{
    for (auto it = g_aballTrace3.begin(); it != g_aballTrace3.end();)
    {
        const float age = g_clock.t - it->second;
        const int k = int(age * 10.0f);
        if (age > 3.0f) { it = g_aballTrace3.erase(it); continue; }
        if (k != int((age - g_clock.dt) * 10.0f) && k % 3 == 0)
        {
            const glm::vec3 p = PosOf(it->first), v = VelAloPos(it->first);
            std::printf("ship3: ball %p age %.1f at %.0f %.0f %.0f vel %.0f %.0f %.0f in world %d\n", (void*)it->first, age, p.x, p.y, p.z, v.x, v.y, v.z, int(it->first->fInWorld));
        }
        ++it;
    }
}
void UpdateShip(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    SHIP3* ps = Pship(plo);
    if (!ps) return;
    SHIP3& s = *ps;
    static float s_tBattle = -1.0f;
    if (g_clock.t != s_tBattle) { s_tBattle = g_clock.t; UpdateShipBattle3(); UpdateGalleonAuto3(); if (NPrint() >= 3) TraceBalls(); }
    UpdateMotion(s);
    UpdateCannons(s);
    UpdateShipGauge(s);
    CarryDeck(s);
    if (NPrint() >= 2)
    {
        static std::unordered_map<const LO*, float> s_mpt;
        float& t = s_mpt[plo];
        if (g_clock.t - t >= (NPrint() >= 3 ? 0.0f : 1.0f) || g_clock.t < t)
        {
            t = g_clock.t;
            const glm::vec3 p = PosOf(plo);
            const glm::vec3 v = VelAloPos(s.palo);
            if (s.psmaShip) std::printf("ship3: %d machine state %d goal %d\n", plo->oid, int(OidSmaCur(s.psmaShip)), int(OidSmaGoal(s.psmaShip)));
            if (s.owner) { const glm::vec3 q = PosOf(s.owner), l = ToLocal(s.palo, q); std::printf("ship3: %d owner %d at %.0f %.0f %.0f (local %.0f %.0f %.0f) in world %d\n", plo->oid, s.owner->oid, q.x, q.y, q.z, l.x, l.y, l.z, int(s.owner->fInWorld)); }
            std::printf("ship3: %d t %.2f mode %d phase %d sub %d pos %.0f %.0f %.0f vel %.0f %.0f %.0f yaw %.2f wind %.2f speed %.0f rate %.3f rudder %.3f hp %d/%d target %d\n",
                        plo->oid, g_clock.t, s.mode, s.phase, s.submode, p.x, p.y, p.z, v.x, v.y, v.z, Heading(s.palo), s.windAngle, s.speed, s.yawRate, s.rudder, s.hp[0],
                        s.hp[1], s.ptarget ? s.ptarget->palo->oid : -1);
        }
    }
}

// orig3 0x214028 (vt slot 74): the yaw rate, the speed along the turned bow, the height (the frames as the vehicle's
// forces 0x1D2E18 natively: the position record's velocity with this frame's acceleration, in the rotation's frame)
void ShipForces(ALO* palo)
{
    ApplyAloGravity(palo);   // 0x243738
    SHIP3* ps = PshipFind(palo);
    if (!ps) return;
    SHIP3& s = *ps;
    if (s.mode != 2 && s.mode != 4 && s.mode != 5 && s.mode != 7) return;
    if (static const bool s_fOff = std::getenv("SLY3_SHIPNOFORCE") != nullptr; s_fOff) return;
    XF* position = PxfPos(palo);
    XF* rotation = PxfRot(palo);
    if (!position || !rotation) return;
    const float dt = std::max(g_clock.dt, 1e-4f);
    // (the records moving: a record at rest (3) is not integrated natively; the original's sailing keeps them moving)
    if (position->mode == XFM_Rest) SetXfMode(palo, position, XFM_MovingHold);
    if (rotation->mode == XFM_Rest) SetXfMode(palo, rotation, XFM_MovingHold);
    const glm::vec3 wLocal = glm::transpose(glm::mat3(rotation->mat)) * glm::vec3(rotation->vel);
    const float lift = (s.yawRate - wLocal.z) / dt;
    VehicleAccelRotation(palo, glm::mat3(rotation->matWorld) * glm::vec3(0.0f, 0.0f, lift));   // 0x244890
    const glm::mat3 phase = cmm::BuildRotateMatrixAxis((wLocal.z + lift * (dt * 0.5f)) * dt, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 want = phase * glm::vec3(s.speed, 0.0f, 0.0f);
    const glm::vec3 velocity = VecConvertXfs(position->pxfRef, rotation, glm::vec3(position->vel) + glm::vec3(position->aseg[3]) * dt);
    glm::vec3 drive = (want - velocity) / dt;
    if (GOAL3* pg = Pgoal(s); pg && pg->accel > 0.0f) drive = cmm::LimitVectorLength(drive, pg->accel);   // 0x1205F0
    drive.z = 0.0f;
    AccelAloPos(palo, VecConvertXfs(rotation, nullptr, drive));   // 0x244828
    // the height spring (D_00325BA8 {50, 0, 0.5}) toward +0x248 (+ +0x24C while sinking)
    const glm::vec3 pos = glm::vec3(MatWorld(palo)[3]);
    float zWant = s.height + (s.mode >= 5 ? s.sink : 0.0f);
    if (s.mode == 7) zWant = s.height - 600.0f * std::min(3.0f, g_clock.t - s.tMode);
    static const SMP s_smp{50.0f, 0.0f, 0.5f};
    float dz = 0.0f;
    GSmooth(pos.z, zWant, dt, &s_smp, &dz);
    const glm::vec3 vWorld = VecConvertXfs(position->pxfRef, nullptr, glm::vec3(position->vel) + glm::vec3(position->aseg[3]) * dt);
    AccelAloPos(palo, glm::vec3(0.0f, 0.0f, (dz - vWorld.z) / dt));
}

// orig3 0x214370 (vt slot 78): a hit while sailing / in battle / nearly sunk, from a cannonball of another ship (or
// a ship's ram): the side hit by the impact's angle (both within the tilt range), the hit points less the damage
// (halved by the 0x3F power-up, quartered between the computer's ships), the cannons broken; none left: nearly sunk
// (5, the computer's ship in a battle the manager still holds) or sunk (7)
bool FAcceptShipContact(ALO* palo, CTMSG* pctm)
{
    SHIP3* ps = PshipFind(palo);
    if (!ps) return false;
    if (NPrint() >= 2)
        std::printf("ship3: %d contact from %d (cid 0x%X) accepted 0x%X state %d mode %d\n", palo->oid, pctm->ploSender ? pctm->ploSender->oid : -1,
                    pctm->ploSender ? unsigned(pctm->ploSender->Cid()) : 0u, unsigned(pctm->grfAccepted), pctm->nState, ps->mode);
    SHIP3& s = *ps;
    if (s.mode != 2 && s.mode != 4 && s.mode != 5) return false;
    if (!(pctm->grfAccepted & 2)) return false;
    if (s.mode == 5 && g_clock.t - s.tMode <= 3.0f) return false;   // 0x214320 (vt slot 80)
    LO* pfrom = pctm->ploSender;
    const bool fBall = pfrom && FIsLoDerivedFrom(pfrom, 0x59);
    LO* powner = fBall ? PloProjectileLauncher(pfrom) : pfrom;
    if (fBall && powner == palo) return false;
    if (!powner || !FShip3(powner)) return false;
    if (pfrom == s.ploLastHit && g_clock.t - s.tLastHitSender < 0.05f) return true;
    s.ploLastHit = pfrom;
    s.tLastHitSender = g_clock.t;
    SHIP3* pown = PshipOf(powner);
    palo->SetField(0, 0x184, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pfrom) & 0xFFFFFFFFu));   // 0x1F4DD8
    int damage = std::abs(pctm->nState);
    if (s.type == 0x3F) damage /= 2;
    if (s.phase == 2 && pown && pown->phase == s.phase) damage /= 4;
    if (pctm->nKill3 || pctm->fKill3) s.hp = {0, 0};
    else
    {
        s.side = -1;
        if (fBall)
        {
            const glm::vec3 r = VecToLocal(palo, VelAloPos(static_cast<ALO*>(pfrom)));
            const float angle = RadNormalize(std::atan2(r.y, std::fabs(r.x)));
            if (angle >= s.tilt.gMin && angle <= s.tilt.gMax) s.side = 2;
            else s.side = r.y < 0.0f ? 0 : 1;
        }
        else s.side = ToLocal(palo, PosOf(pfrom)).y > 0.0f ? 0 : 1;
        if (s.side == 0 || s.side == 1)
        {
            s.hp[size_t(s.side)] = std::max(0, s.hp[size_t(s.side)] - damage);
            if (fBall)
            {
                s.hpSave[size_t(s.side)] = std::max(0, s.hpSave[size_t(s.side)] - damage);
                UpdateBrokenCannons(s, s.side);
            }
        }
        else if (s.side == 2)
        {
            const int d = int(float(damage) * (s.phase == 2 ? 3.0f : 2.0f));
            for (int i = 0; i < 2; ++i)
            {
                s.hp[size_t(i)] = std::max(0, s.hp[size_t(i)] - d);
                if (fBall) s.hpSave[size_t(i)] = std::max(0, s.hpSave[size_t(i)] - d);
            }
            if (fBall) UpdateBrokenCannons(s, 0), UpdateBrokenCannons(s, 1);
        }
    }
    if (NPrint())
        std::printf("ship3: %d hit by %d (%d) side %d damage %d: %d / %d (t %.2f)\n", palo->oid, pfrom ? pfrom->oid : -1, powner->oid, s.side, damage, s.hp[0], s.hp[1], g_clock.t);
    if (s.hp[0] <= 0 || s.hp[1] <= 0)
    {
        if (s.phase == 2 && s.mode == 4 && FShipInBattle3(palo))
        {
            SetMode(s, 5);
            s.hp = {1, 1};
            Broadcast3(0xF, palo);
        }
        else
        {
            s.busy = 2;   // 0x215FD0(2): sunk
            SetMode(s, 7);
        }
    }
    else Broadcast3(s.phase == 2 ? 0xF : 0xA, palo);
    return true;
}

// orig3 0x213F08 -> 0x215640 (vt slot 31): the hit points, the cannons, the mode / submode, the height
void ResetShipRecord(SHIP3& s);
void ResetShip(LO* plo, LO*)
{
    if (SHIP3* ps = Pship(plo)) ResetShipRecord(*ps);
}
void ResetShipRecord(SHIP3& s)
{
    LO* plo = s.palo;
    s.hp = s.hpMax;
    s.hpSave = s.hpMax;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < s.ccannon[size_t(i)]; ++j) SetCannonState(s.acannon[size_t(i)][size_t(j)], 0, 3.4028235e38f);
    s.mode = -1;
    SetMode(s, 0);
    if (s.phase == 2) SetSubmode(s, s.patrol ? 1 : 2);
    else SetSubmode(s, -1);
    s.busy = 0;
    s.sideFire = -1;
    SetType(s, -1);
    s.height = PosOf(plo).z;
}

struct INITSHIP3
{
    INITSHIP3()
    {
        RegisterLoUpdate(FN3_TAG | 0x215FD8, UpdateShip);
        // the scripts' setters (the field options) and methods
        static constexpr int s_aeset[] = {1955, 1956, 1957, 1959, 1962, 1973, 1974, 1975, 1976, 1977, 1978, 1979, 1980, 1981, 1982, 1983, 1984, 1990,
                                          1991, 1992, 1993, 1994, 2001, 2002, 2003, 2004, 2007, 2010, 2011, 2012};
        static std::unordered_map<int, int> s_mp;
        (void)s_mp;
#define SHIPSET(E) RegisterObjectSetter3(E, [](LO* plo, const CRef& r) { if (SHIP3* s = PshipOf(plo)) ApplyOption(*s, E, {r}); })
        SHIPSET(1955); SHIPSET(1956); SHIPSET(1957); SHIPSET(1959); SHIPSET(1962); SHIPSET(1973); SHIPSET(1974); SHIPSET(1975); SHIPSET(1976);
        SHIPSET(1977); SHIPSET(1978); SHIPSET(1979); SHIPSET(1980); SHIPSET(1981); SHIPSET(1982); SHIPSET(1983); SHIPSET(1984); SHIPSET(1990);
        SHIPSET(1991); SHIPSET(1992); SHIPSET(1993); SHIPSET(1994); SHIPSET(2001); SHIPSET(2002); SHIPSET(2003); SHIPSET(2004); SHIPSET(2007);
        SHIPSET(2010); SHIPSET(2011); SHIPSET(2012);
#undef SHIPSET
        RegisterObjectGetter3(1973, [](LO* plo) { CRef r; SHIP3* s = PshipOf(plo); r.SetS32(s ? s->mode : -1); return r; });
        RegisterObjectGetter3(1974, [](LO* plo) { CRef r; SHIP3* s = PshipOf(plo); r.SetS32(s ? s->phase : -1); return r; });
        RegisterObjectGetter3(1972, [](LO* plo) { CRef r; SHIP3* s = PshipOf(plo); r.SetS32(s ? s->side : -1); return r; });
        RegisterObjectGetter3(1975, [](LO* plo) { CRef r; SHIP3* s = PshipOf(plo); r.SetS32(s ? s->group : 0); return r; });
        RegisterObjectGetter3(1979, [](LO* plo) { CRef r; SHIP3* s = PshipOf(plo); r.SetF32(s ? s->length : 0.0f); return r; });
        RegisterObjectGetter3(1982, [](LO* plo) { CRef r; SHIP3* s = PshipOf(plo); r.SetF32(s ? s->range : 0.0f); return r; });
        RegisterObjectGetter3(2010, [](LO* plo) { CRef r; SHIP3* s = PshipOf(plo); r.SetS32(s ? s->busy : 0); return r; });
        RegisterObjectGetter3(1978, [](LO* plo) {
            CRef r;
            SHIP3* s = PshipOf(plo);
            if (s && s->owner) r.SetBasic(s->owner);
            if (NPrint() >= 2) std::printf("ship3: %d's owner (1978): %d\n", plo ? plo->oid : -1, s && s->owner ? s->owner->oid : -1);
            return r;
        });
        // the methods (orig3 thunks 0x110AA8..0x1116A0)
        auto meth = [](uint32_t thunk, int eopid) {
            static std::unordered_map<uint32_t, int> s_mpe;
            s_mpe[thunk] = eopid;
            (void)s_mpe;
        };
        (void)meth;
        RegisterObjectMethod3(0x110AA8, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1954, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x110B30, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1961, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x110BA8, [](LO* plo, const std::vector<CRef>& a) {
            CRef r; SHIP3* s = PshipOf(plo); const int i = a.empty() ? 0 : NOf(a[0]) & 1; r.SetS32(s ? s->hp[size_t(i)] : 0); return r; });
        RegisterObjectMethod3(0x110D78, [](LO* plo, const std::vector<CRef>& a) {
            CRef r; SHIP3* s = PshipOf(plo); const int i = a.empty() ? 0 : NOf(a[0]) & 1; r.SetS32(s ? s->hpMax[size_t(i)] : 0); return r; });
        RegisterObjectMethod3(0x110F48, [](LO* plo, const std::vector<CRef>& a) {
            CRef r; SHIP3* s = PshipOf(plo); const int i = a.empty() ? 0 : NOf(a[0]) & 1; r.SetS32(s ? s->hpSave[size_t(i)] : 0); return r; });
        RegisterObjectMethod3(0x110C28, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1964, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x110CD0, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1965, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x110DF8, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1967, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x110EA0, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1968, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x110FC8, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1970, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x111070, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1971, a); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x111118, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 1989, a); return CRef(TAGK_Void); });
        // 1995 0x218B68: out (mode 6, the manager told); 1996 0x218BA0: sunk by the script (phase 3, mode 7)
        // (0x218B68 = SetMode(6) then 0x21A570: the player's ship parked beside it, not the battle's item gone 0x21A450:
        // the boarding (Ep5's 58082) takes the crippled ship out of the fight; its capture (1996) ends the battle)
        RegisterObjectMethod3(0x1111E0, [](LO* plo, const std::vector<CRef>&) {
            if (SHIP3* s = PshipOf(plo)) { SetMode(*s, 6); ParkBeside3(plo); }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111258, [](LO* plo, const std::vector<CRef>&) {
            if (SHIP3* s = PshipOf(plo)) { s->busy = 3; s->fFaded = true; SetMode(*s, 7); OnShipOut3(plo); }   // (captured: 0x215FD0(3))
            return CRef(TAGK_Void);
        });
        // 1997 0x2112D0 -> 0x218358: the cannon given fires its side's broadside (the deck's cannons, used by the player)
        RegisterObjectMethod3(0x1112D0, [](LO* plo, const std::vector<CRef>& a) {
            LO* pkey = !a.empty() ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
            if (!pkey && plo && !a.empty()) pkey = PloRef(plo, a[0]);
            if (!pkey || !g_psw) return CRef(TAGK_Void);
            // (the cannon's ship: the one whose slot holds this cannon; a copy's cannon names its template's ship in its
            // option 2149 natively, the template being out of the world: the Galleon Duel's two copies of 44092)
            for (LO* p : g_psw->mpcidaplo[CID_SHIP])
                if (SHIP3* s = PshipOf(p))
                    for (int g = 0; g < 2; ++g)
                        for (int i = 0; i < s->ccannon[size_t(g)]; ++i)
                            if (s->acannon[size_t(g)][size_t(i)].cannon == pkey)
                            {
                                if (NPrint()) std::printf("ship3: %d broadside %d by cannon %d (t %.2f)\n", p->oid, g, pkey->oid, g_clock.t);
                                RestartSide(*s, &s->acannon[size_t(g)][size_t(i)], g);
                                return CRef(TAGK_Void);
                            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111618, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo); s && !a.empty() && s->acidCollide.size() < 4) s->acidCollide.push_back(NOf(a[0])); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x1116A0, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo); s && !a.empty() && s->aoidCollide.size() < 4) s->aoidCollide.push_back(IdOf(a[0])); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x111358, [](LO* plo, const std::vector<CRef>& a) { if (SHIP3* s = PshipFind(plo)) ApplyOption(*s, 2000, a); return CRef(TAGK_Void); });
    }
} s_initship3;
// orig3 0x2149A0 (vt slot 65, the overlap kind): with lists, 2 (collides) with a listed class or object, else 1
// (passes); without, the ALO's
int OverlapKindShip(ALO* palo, ALO* pother)
{
    SHIP3* ps = PshipFind(palo);
    if (NPrint() >= 3 && pother && FIsLoDerivedFrom(pother, 0x59))
    {
        static int s_c = 0;
        if (++s_c % 20 == 1) std::printf("ship3: %d (%p) overlap with ball %d (t %.2f)\n", palo->oid, (void*)palo, pother->oid, g_clock.t);
    }
    if (ps && (!ps->acidCollide.empty() || !ps->aoidCollide.empty()))
    {
        for (int cid : ps->acidCollide)
            if (cid >= 0 && FIsLoDerivedFrom(pother, CID(cid))) return 2;
        for (int oid : ps->aoidCollide)
            if (pother->oid == oid || pother->oidInstance == oid) return 2;
        return 1;
    }
    using PFN = int (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8))) return pfn(palo, pother);
    return 0;
}
// orig3 0x214928 (vt slot 19, Sly 2's 17: the physics update): the ALO's (0x23C970), then +0x550 cleared
void UpdateShipPhys(ALO* palo)
{
    UpdateAloPhys(palo);
    if (SHIP3* ps = PshipFind(palo)) ps->windOwn = 0;
}
// orig3 0x213F68 (vt slot 32, Sly 2's 27: the pooled object's detach): SO's (0x1F3C80), the screen object +0x430
// (EOPID 1994) hidden (its vt slot 54: SCREENOBJ 0x260FE8 = HideScreenObj), and the engine sound +0x548 stopped
// (0x244CA8(snd, 0) / the reference cleared: natively the engine sound 0x217040 is not ported, so none is held)
void DetachShip32(LO* plo, LO* pproxy)
{
    DetachSoBase32(plo, pproxy);
    if (SHIP3* ps = PshipFind(plo))
        if (ps->fx) HideScreenObj(ps->fx);
}
// orig3 0x215008 (vt slot 63, Sly 2's 60: the own contacts of the ship with another object's entry k): the ALO's
// (0x23EDD0: none); an entry of a class 0x62 object (the wind barriers) is tested against +0x544, which nothing in the
// executable writes (its one reference is this read), so it never touches the hull; any other entry against the
// resource entry +0x560 (InitShip): k a sphere (its bit 49, the 0x20000 flag) by MarkSphereBsp (0x1CD858(SW, k, res)),
// else MarkBspBsp (0x1CC790(SW, res, k, the tree arguments)), into the caller's list
void ShipOwnContacts3(ALO* palo, SOE* k, int, const BSPREF&, int iroot, XP** ppxp)
{
    if (!k || !k->palo || !ppxp) return;
    if (FCidDerivedFrom(k->palo->Cid(), S3CID(0x62))) return;
    SHIP3* ps = PshipFind(palo);
    SOE* pres = ps ? ps->psoeResource : nullptr;
    if (!pres || pres == k) return;
    if (k->f49) MarkSphereBspContacts(k, pres, nullptr, ppxp);
    else MarkBspBspContacts(pres, k, k->bsp.abspn, k->bsp.abspn.empty() ? IBSP_NONE : iroot, ppxp);
}
// orig3 0x214A70: the outline's points in the world (the path's points by its matrix) and one edge plane per
// consecutive pair (the normal: the edge x z, through the edge's first point)
void OutlineWorld(LO* shape, std::vector<glm::vec3>* papos, std::vector<glm::vec4>* paplane)
{
    papos->clear();
    paplane->clear();
    const CRV* pcrv = PcrvFromLo(shape);
    if (!pcrv) return;
    const glm::mat4 m = MatWorldLox(shape);
    for (const glm::vec3& p : pcrv->apos) papos->push_back(glm::vec3(m * glm::vec4(p, 1.0f)));
    for (size_t j = 0; j + 1 < papos->size(); ++j)
    {
        const glm::vec3 e = (*papos)[j + 1] - (*papos)[j];
        glm::vec3 n = glm::cross(e, glm::vec3(0.0f, 0.0f, 1.0f));
        const float l = glm::length(n);
        n = l > 0.0f ? n / l : glm::vec3(0.0f);
        paplane->push_back(glm::vec4(n, glm::dot(n, (*papos)[j])));   // 0x119EA8
    }
}
// orig3 0x214C40: the point deepest inside the planes (each point's largest plane distance, stopping at the first one
// in front of it; the smallest of those below 0)
void DeepestPoint(int cpos, const std::vector<glm::vec3>& apos, const std::vector<glm::vec4>& aplane, float* pdepth, glm::vec3* ppos,
                  glm::vec4* pplane)
{
    float best = 0.0f;
    for (int i = 0; i < cpos && i < int(apos.size()); ++i)
    {
        float dist = -FLT_MAX;   // (D_00325BB8)
        glm::vec4 hit(0.0f);
        for (const glm::vec4& pl : aplane)
        {
            const float d = glm::dot(glm::vec3(pl), apos[size_t(i)]) - pl.w;
            if (dist < d)
            {
                dist = d;
                hit = pl;
                if (dist >= 0.0f) break;
            }
        }
        if (dist < best)
        {
            best = dist;
            *pplane = hit;
            *ppos = apos[size_t(i)];
        }
    }
    *pdepth = best;
}
// orig3 0x214F88 (the ships' vt slot 64, the pair test after the entries' contacts): once per pair of ships that both
// have an outline (+0x564, EOPID 2016) and a resource entry (+0x560): 0x214DA8 - both outlines upright (the matrix's
// z.z at least 0.5), each one's points against the other's planes; overlapping, the deeper point a contact of kind 10
// between the resource entries (0x1CB8B0)
void ShipPairContacts3(ALO* palo, ALO* paloOther, XP** ppxp)
{
    if (!palo || !paloOther || palo >= paloOther || !ppxp) return;
    if (!FShip3(palo) || !FShip3(paloOther)) return;
    SHIP3* pa = PshipFind(palo);
    SHIP3* pb = PshipFind(paloOther);
    if (!pa || !pb || !pa->shape || !pb->shape || !pa->psoeResource || !pb->psoeResource) return;
    if (MatWorldLox(pa->shape)[2][2] < 0.5f || MatWorldLox(pb->shape)[2][2] < 0.5f) return;
    std::vector<glm::vec3> aposA, aposB;
    std::vector<glm::vec4> aplaneA, aplaneB;
    OutlineWorld(pa->shape, &aposA, &aplaneA);
    OutlineWorld(pb->shape, &aposB, &aplaneB);
    if (aposA.size() < 2 || aposB.size() < 2) return;
    float depthA = 0.0f, depthB = 0.0f;
    glm::vec3 posA(0.0f), posB(0.0f);
    glm::vec4 planeA(0.0f), planeB(0.0f);
    DeepestPoint(int(aposA.size()) - 1, aposA, aplaneB, &depthA, &posA, &planeA);
    DeepestPoint(int(aposB.size()) - 1, aposB, aplaneA, &depthB, &posB, &planeB);
    if (depthA >= 0.0f && depthB >= 0.0f) return;
    if (depthA < depthB) PxpAddSw(pa->psoeResource, pb->psoeResource, posA, glm::vec3(planeA), 10, nullptr, nullptr, ppxp, depthA);
    else PxpAddSw(pb->psoeResource, pa->psoeResource, posB, glm::vec3(planeB), 10, nullptr, nullptr, ppxp, depthB);
    if (NPrint() >= 2) std::printf("ship3: %d / %d outlines touch (depth %.1f / %.1f, t %.2f)\n", palo->oid, paloOther->oid, depthA, depthB, g_clock.t);
}
NATIVEFN3(0x215008, ShipOwnContacts3);
struct INITSHIPPAIR3 { INITSHIPPAIR3() { g_pfnPairContacts3 = ShipPairContacts3; } } s_initshippair3;
NATIVEFN3(0x214928, UpdateShipPhys);
NATIVEFN3(0x213F68, DetachShip32);
NATIVEFN3(0x2149A0, OverlapKindShip);
NATIVEFN3(0x214028, ShipForces);
NATIVEFN3(0x214370, FAcceptShipContact);
NATIVEFN3(0x213F08, ResetShip);
}  // namespace

bool FShip3(const LO* plo) { return plo && plo->pcls && FIsLoDerivedFrom(plo, CID_SHIP); }

// orig3 0x002151E8 (vt slot 90, a power-up given (the SO option 947, 0x10A4A0)): a ship whose character has its pad
// (phase +0x1B8 1) takes the next of its shuffled power-ups (+0x4F0: 0x213C90's {0x3E..0x42} unless the computer's,
// phase 2) through 0x215388. Returns the power-up (-1: none)
int GiveShipPowerup3(LO* plo)
{
    SHIP3* ps = FShip3(plo) ? PshipFind(plo) : nullptr;
    if (!ps || ps->phase != 1) return -1;
    static std::unordered_map<const LO*, std::pair<std::vector<int>, int>> s_mpbag;
    auto& bag = s_mpbag[plo];
    if (bag.first.empty()) bag = {{0x3E, 0x3F, 0x40, 0x41, 0x42}, -1};
    const int type = NNextShuffled3(bag.first, bag.second);
    SetType(*ps, type);
    if (NPrint()) std::printf("ship3: %d power-up 0x%X\n", plo->oid, type);
    return type;
}

// (cmfocus3.cpp, orig3 0x214820: the ships' vt slot 85) the ship's length (+0x220, EOPID 1979)
float GShipLength3(LO* plo)
{
    SHIP3* ps = FShip3(plo) ? Pship(plo) : nullptr;
    return ps ? ps->length : 0.0f;
}

// orig3 0x00215248 (vt slot 88, a pickup collected, pickup.cpp GivePickupToCollector3): from a class 0x6C pickup with
// its split flag (+0x1B0) a random amount in +0x1A0..+0x1A4 shared between the sides' hit points (the lower side topped
// up first, the rest halved, an odd point to the port side: 0x2157D0, then the cannons' share 0x215808 from the new
// values); without, a random amount in +0x1A8..+0x1AC more coins (0x185328) (MODES-13)
void AddCoins3(int n);   // pickup.cpp
void TakePickupShip3(LO* pso, LO* ppickup)
{
    if (!pso || !ppickup || !FShip3(pso) || !FIsLoDerivedFrom(ppickup, 0x6C)) return;
    SHIP3* ps = Pship(pso);
    if (!ps) return;
    SHIP3& s = *ps;
    auto g = [&](uint32_t ib) { return std::bit_cast<float>(ppickup->U32Field(0, ib, 0)); };
    if (ppickup->U32Field(0, 0x1B0, 0))
    {
        int amount = static_cast<int>(GRandInRange(g(0x1A0), g(0x1A4)));
        const int first = s.hp[0], second = s.hp[1], diff = first - second;
        int addFirst = 0, addSecond = 0;
        if (diff < 0) addFirst = std::min(amount, -diff);
        else if (diff > 0) addSecond = std::min(amount, diff);
        amount -= addSecond + addFirst;
        const bool odd = amount & 1;
        amount /= 2;
        if (odd) { addSecond += amount; addFirst = addFirst + 1 + amount; }
        else { addFirst += amount; addSecond += amount; }
        SetHp(s, first + addFirst, second + addSecond);
        SetHpSave(s, s.hp[0] + addFirst, s.hp[1] + addSecond);
        if (NPrint() >= 1) std::printf("ship3: %d took pickup %d: hp %d / %d\n", pso->oid, ppickup->oid, s.hp[0], s.hp[1]);
    }
    else
        AddCoins3(static_cast<int>(GRandInRange(g(0x1A8), g(0x1AC))));
}

int NShipMode3(const LO* plo)
{
    SHIP3* ps = PshipFind(plo);
    return ps ? ps->mode : -1;
}
bool FShipHp3(const LO* plo, int* pport, int* pstarboard, int* pmaxPort, int* pmaxStarboard)
{
    SHIP3* ps = PshipFind(plo);
    if (!ps) return false;
    if (pport) *pport = ps->hp[0];
    if (pstarboard) *pstarboard = ps->hp[1];
    if (pmaxPort) *pmaxPort = ps->hpMax[0];
    if (pmaxStarboard) *pmaxStarboard = ps->hpMax[1];
    return true;
}

// orig3 0x2147B0 (vt slot 83): a pad taken sails the ship (mode 2); released: idle (0) unless in battle / the map
void OnShipEnable3(LO* plo, int n)
{
    SHIP3* ps = PshipOf(plo);
    if (!ps) return;
    if (n == -1)
    {
        if (ps->phase != 1 && ps->mode != 1) SetMode(*ps, 0);
        return;
    }
    SetMode(*ps, 2);
}

unsigned GrfShipSteer3(LO* plo)
{
    SHIP3* ps = PshipFind(plo);
    if (!ps) return 0;
    PADCTL* ppad = PpadSteer(*ps);
    if (NPrint() >= 3)
    {
        static float s_t = -1.0f;
        if (g_clock.t - s_t > 1.0f)
        {
            s_t = g_clock.t;
            PADCTL* pown = ps->owner ? PpadctlOfSo3(ps->owner) : nullptr;
            std::printf("ship3: %d steer pad %p grf 0x%X (owner pad %p control %d grf 0x%X)\n", plo->oid, (void*)ppad, ppad ? ppad->grfbtn : 0u, (void*)pown,
                        pown ? pown->nControl : -9, pown ? pown->grfbtn : 0u);
        }
    }
    if (ps->fAuto) return ps->grfAuto;
    if (!ppad) return 0;
    unsigned grf = ppad->grfbtn & 3u;
    return grf;
}

void ResetShips3() { s_mpship.clear(); }

// (the harness, tools/galleon3.sh: SLY3_GALLEONAUTO=mask of seats) a seat's character on its ship in a battle plays by
// itself: the steering that keeps the nearest enemy abeam (or closes to the guns' range), and every 6 s the character put
// at a ready cannon's use point of the side facing the enemy, the use started (UseObjectBy: the cannon's Sly 3 script fires
// the side), as the route step "broadside" does for the first player
void UpdateGalleonAuto3()
{
    static const unsigned s_mask = std::getenv("SLY3_GALLEONAUTO") ? unsigned(std::strtoul(std::getenv("SLY3_GALLEONAUTO"), nullptr, 0)) : 0u;
    if (!s_mask || !g_psw) return;
    for (int i = 0; i < 4; ++i)
    {
        LO* pchar = split3::FActive() ? split3::PloPlayer(g_psw, i) : (i == 0 ? PloPoCurrent(g_psw) : nullptr);
        if (!pchar) continue;
        SHIP3* ps = nullptr;
        for (LO* p : g_psw->mpcidaplo[CID_SHIP])
            if (SHIP3* q = PshipFind(p); q && q->owner == pchar && q->mode == 4) ps = q;
        if (!ps) continue;
        ps->fAuto = (s_mask >> i) & 1;
        if (!ps->fAuto) continue;
        glm::vec3 pos(0.0f), vel(0.0f);
        float heading = 0.0f;
        unsigned grf = 0;
        bool fUse = false;
        if (!FShipBroadside3(pchar, &pos, &heading, &vel, &grf, &fUse)) continue;
        ps->grfAuto = grf;
        if (fUse && g_clock.t - ps->tAutoUse > 6.0f && FIsLoDerivedFrom(pchar, S3CID(0x30)))
        {
            ps->tAutoUse = g_clock.t;
            ALO* pa = static_cast<ALO*>(pchar);
            SetAloPos(pa, pos);
            SetAloVelocityTo(pa, vel);
            // the use point's object: the cannon's use the step found (its position)
            LO* puse = nullptr;
            float best = 1e9f;
            for (int side = 0; side < 2; ++side)
                for (int k = 0; k < ps->ccannon[size_t(side)]; ++k)
                    if (ALO* pc = dynamic_cast<ALO*>(ps->acannon[size_t(side)][size_t(k)].cannon))
                        for (LO* ch : pc->aploChild)
                            if (ch && ch->Cid() == 0x21 && glm::length(PosLoWorld(ch) - pos) < best) best = glm::length(PosLoWorld(ch) - pos), puse = ch;
            if (puse && PactorFromLo(pchar)) UseObjectBy(puse, PactorFromLo(pchar));
            if (NPrint()) std::printf("ship3: auto seat %d (%d) uses %d on %d (t %.2f)\n", i, pchar->oid, puse ? puse->oid : -1, ps->palo->oid, g_clock.t);
        }
    }
}

void FadeAloIn(ALO* palo, float dt);   // asegevent.cpp (orig3 0x2412F8)
void RestoreShip3(LO* plo)
{
    SHIP3* ps = PshipOf(plo);
    if (!ps || !g_psw) return;
    if (!plo->fInWorld || plo->fRemoved) AddLo(g_psw, plo);
    FadeAloIn(ps->palo, 0.0f);
    SetAloVelocityTo(ps->palo, glm::vec3(0.0f));
    SetAloAngVelocityTo(ps->palo, glm::vec3(0.0f));
    ResetShipRecord(*ps);
    if (NPrint()) std::printf("ship3: %d restored (option 948, t %.2f)\n", plo->oid, g_clock.t);
}


// (the harness, route.cpp's "sail" step) the steering buttons for the ship the player sails (or steers from its deck):
// L2 (0x1) turns it to port (the heading up), R2 (0x2) to starboard, toward pos; *pdist its horizontal distance
bool FSailToward3(LO* plo, const glm::vec3& pos, unsigned* pgrf, float* pdist)
{
    SHIP3* ps = PshipFind(plo);
    if (!ps && g_psw)
        for (LO* p : g_psw->mpcidaplo[CID_SHIP])
            if (SHIP3* q = PshipFind(p); q && q->phase == 1 && q->owner == plo) ps = q;
    if (!ps) return false;
    const glm::vec3 d = pos - PosOf(ps->palo);
    *pdist = LenXY(d);
    const float err = RadNormalize(Atan2Of(d) - Heading(ps->palo));
    *pgrf = err > 0.08f ? 1u : err < -0.08f ? 2u : 0u;
    return true;
}

// (the harness, route.cpp's "broadside" step) the player on the deck of a ship in battle (phase 1): the use point of a
// ready cannon (state 1) on the side that faces the nearest enemy ship (side 0: port, the enemy at local y > 0), the
// heading toward the cannon, the ship's velocity; *pgrf the steering that keeps the enemy abeam (L2 0x1 / R2 0x2)
bool FShipBroadside3(LO* plo, glm::vec3* ppos, float* pheading, glm::vec3* pvel, unsigned* pgrf, bool* pfUse)
{
    *pfUse = false;
    if (!g_psw || !plo) return false;
    SHIP3* ps = nullptr;
    for (LO* p : g_psw->mpcidaplo[CID_SHIP])
        if (SHIP3* q = PshipFind(p); q && q->phase == 1 && q->owner == plo && q->mode == 4) ps = q;
    if (!ps) return false;
    SHIP3* pt = PshipNearest(*ps);
    if (!pt) return false;
    // (the enemy nearly sunk (mode 5): its boarding point, a use (class 0x21) of the ship in the world)
    if (pt->mode == 5)
    {
        std::function<LO*(LO*)> findUse = [&](LO* p) -> LO* {
            ALO* pa = dynamic_cast<ALO*>(p);
            if (!pa) return nullptr;
            for (LO* ch : pa->aploChild)
            {
                if (ch && ch->Cid() == 0x21 && ch->fInWorld && !ch->fRemoved) return ch;
                if (LO* q = findUse(ch)) return q;
            }
            return nullptr;
        };
        *pgrf = 0;
        if (LO* puse = findUse(pt->palo))
        {
            *ppos = PosLoWorld(puse);
            *pheading = Heading(pt->palo);
            *pvel = VelAloPos(pt->palo);
            *pfUse = true;
            if (NPrint() >= 2) std::printf("ship3: boarding use %d of %d at %.0f %.0f %.0f\n", puse->oid, pt->palo->oid, ppos->x, ppos->y, ppos->z);
        }
        return true;
    }
    const glm::vec3 local = ToLocal(ps->palo, PosOf(pt->palo));
    const int side = local.y > 0.0f ? 0 : 1;
    // keep the enemy abeam: its bearing toward +-90 degrees
    const float bearing = Atan2Of(local);
    // (beyond 3/4 of the guns' range: bow on toward it; nearer: abeam)
    const float want = LenXY(local) > ps->range * 0.75f ? 0.0f : (side == 0 ? kHalfPi : -kHalfPi);
    const float err = RadNormalize(bearing - want);
    *pgrf = err > 0.15f ? 1u : err < -0.15f ? 2u : 0u;   // (a left turn (L2) brings the bearing down)
    // (the guns reach only a few degrees around the beam (+0x3F0 / +0x3F4): the use only with the enemy abeam, in range)
    if (want == 0.0f || std::fabs(err) > ps->yawB + 0.05f) return true;
    for (int i = 0; i < ps->ccannon[size_t(side)]; ++i)
    {
        CANNON3& c = ps->acannon[size_t(side)][size_t(i)];
        if (c.state != 1 || !c.cannon) continue;
        ALO* pcannon = dynamic_cast<ALO*>(c.cannon);
        LO* puse = nullptr;
        if (pcannon)
            for (LO* ch : pcannon->aploChild)
                if (ch && ch->Cid() == 0x21) puse = ch;
        const glm::vec3 posCannon = PosOf(c.cannon);
        const glm::vec3 posUse = puse ? PosLoWorld(puse) : posCannon;
        if (NPrint() >= 3 && puse)
            std::printf("ship3: broadside use %d (cid 0x%X) in world %d removed %d at %.0f %.0f %.0f, cannon %d at %.0f %.0f %.0f\n", puse->oid, unsigned(puse->Cid()),
                        int(puse->fInWorld), int(puse->fRemoved), posUse.x, posUse.y, posUse.z, c.cannon->oid, posCannon.x, posCannon.y, posCannon.z);
        glm::vec3 d = posCannon - posUse;
        d.z = 0.0f;
        *ppos = posUse;
        *pheading = glm::length(d) > 1.0f ? Atan2Of(d) : Heading(ps->palo) + (side == 0 ? kHalfPi : -kHalfPi);
        *pvel = VelAloPos(ps->palo);
        *pfUse = true;
        return true;
    }
    return true;   // (no ready cannon: the steering only, no use point)
}

// ---- the battle manager (class 0x9B, the global object 1943; orig3 0x219768..0x21A720) ----
// The scripts start a battle with method 2019 (0x111728 -> 0x219F70: the player's ship and the list of the
// computer's ships; the others leave the world), end it with 2020 (0x21A418); 2021 (+0xAC) marks the two-player battle
// (both ships count). While it runs the HUD 1944 counts the computer's ships (its +0x124 left, +0x120 sunk); when none is
// left the tally 1945 lists each ship sunk (2) / captured (3) with its reward in coins (states 2..8), then the player's
// ship sails again (mode 2).
namespace
{
struct BATTLE3
{
    SW* psw = nullptr;
    uint32_t serial = 0;                  // (native: a reloaded world can reuse the SW's address)
    LO* plo = nullptr;                    // the object 1943
    int state = 0;                        // +0x5C
    float tState = 0.0f, tStateReal = 0.0f;
    LO* target = nullptr;                 // +0x40: the player's ship (the one that started it)
    std::vector<LO*> aitem;               // +0x44 (at most 5)
    struct SLOT { LO* item = nullptr; int value = 0; };
    std::vector<SLOT> aslot;              // +0x6C (the tally's rows)
    int index = 0, counter = 0;           // +0x98 / +0x9C
    LO* hud = nullptr;                    // +0xA0 (1944)
    LO* panel = nullptr;                  // +0xA4 (1945)
    int fNoSave = 0;                      // +0xA8 (the start's flag)
    int fTwoPlayer = 0;                   // +0xAC (EOPID 2021)
    bool fPaused = false;                 // +0x68
    int cSunkHud = 0;                     // (the HUD's +0x120)
};
BATTLE3 s_battle;
BATTLE3& Battle()
{
    if (s_battle.psw != g_psw || (g_psw && s_battle.serial != g_psw->serial))
    {
        s_battle = BATTLE3{};
        s_battle.psw = g_psw;
        s_battle.serial = g_psw ? g_psw->serial : 0;
        if (g_psw)
        {
            s_battle.plo = PloFindSwObjectByOid(g_psw, 1943);
            s_battle.hud = PloFindSwObjectByOid(g_psw, 1944);     // 0x2196D0 (kind 0x405)
            s_battle.panel = PloFindSwObjectByOid(g_psw, 1945);
            if (NPrint())
                std::printf("ship3: battle manager %p hud %d (cid 0x%X) panel %d (cid 0x%X)\n", (void*)s_battle.plo, s_battle.hud ? s_battle.hud->oid : -1,
                            s_battle.hud ? unsigned(s_battle.hud->Cid()) : 0u, s_battle.panel ? s_battle.panel->oid : -1, s_battle.panel ? unsigned(s_battle.panel->Cid()) : 0u);
        }
    }
    return s_battle;
}
int CItemsLeft(BATTLE3& b)   // 0x21A720
{
    int c = 0;
    for (LO* p : b.aitem)
    {
        if (p == b.target && !b.fTwoPlayer) continue;
        SHIP3* ps = PshipFind(p);
        if (ps && !ps->busy) ++c;
    }
    return c;
}
void SetBattleState(BATTLE3& b, int st);
void TallyBattle(BATTLE3& b)   // 0x21A300
{
    b.aslot.clear();
    b.index = 0;
    b.counter = 0;
    for (LO* p : b.aitem)
    {
        SHIP3* ps = PshipFind(p);
        if (!ps || !ps->busy) continue;
        BATTLE3::SLOT sl;
        sl.item = p;
        if (ps->busy == 3) sl.value = int(GRandInRange(ps->reward3.gMin, ps->reward3.gMax));
        else if (ps->busy == 2) sl.value = int(GRandInRange(ps->reward2.gMin, ps->reward2.gMax));
        b.aslot.push_back(sl);
    }
    SetBattleState(b, b.panel && !b.aslot.empty() && !b.fNoSave ? 2 : 8);
}
void OnBattleItemGone(BATTLE3& b, LO* plo)   // 0x21A450
{
    if (b.state <= 0) return;
    if (b.hud && !b.fTwoPlayer && plo != b.target) SetOptionN(b.hud, 1244, ++b.cSunkHud);
    const int c = CItemsLeft(b);
    if (NPrint()) std::printf("ship3: battle: %d gone, %d left (t %.2f)\n", plo ? plo->oid : -1, c, g_clock.t);
    if (c == 0 || (b.fTwoPlayer && c == 1)) TallyBattle(b);
}
std::string StrBattleValue(int i)   // 0x21A7B8
{
    BATTLE3& b = Battle();
    if (i < 0 || i >= int(b.aslot.size())) return std::string();
    return std::to_string(b.aslot[size_t(i)].value);
}
int CCoinsNow()
{
    CRef r = RefGetProgress({"loot", "ccoin"});
    return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : 0;
}
void AddProgressCount(const char* sz)
{
    CRef r = RefGetProgress({"battle", sz});
    CRef w;
    w.SetS32((r.m_tagk == TAGK_S32 ? r.m_n : 0) + 1);
    FSetProgress({"battle", sz}, w);
}
// orig3 0x219A18
void SetBattleState(BATTLE3& b, int st)
{
    const int old = b.state;
    if (st == old) return;
    if (old == 1)
    {
        if (b.hud && !b.fTwoPlayer) HideScreenObj(b.hud);
        Broadcast3(0x5C, nullptr);   // (orig3 0x219A18 passes no object: MODES-16)
    }
    b.state = st;
    b.tState = g_clock.t;
    b.tStateReal = g_clock.tReal;
    if (NPrint()) std::printf("ship3: battle state %d -> %d (t %.2f)\n", old, st, g_clock.t);
    static const int s_alabel[4] = {-1, 0x7A0, 0x7A1, 0x7A2};
    static const int s_atitle[4] = {0x7A8, 0x7A9, 0x7AA, 0x7AB};
    static const int s_areward[4] = {0x7A4, 0x7A5, 0x7A6, 0x7A7};
    switch (st)
    {
    case 0:
        b.aitem.clear();
        break;
    case 1:
        if (b.hud && !b.fTwoPlayer)
        {
            b.cSunkHud = 0;
            SetOptionN(b.hud, 1245, CItemsLeft(b));   // (+0x124: 0x21A720)
            SetOptionN(b.hud, 1244, 0);               // (+0x120)
            ShowScreenObj(b.hud, -1);
        }
        Broadcast3(0x5B, nullptr);   // (orig3 0x219A18: no object)
        break;
    case 3:
        if (b.index < int(b.aslot.size()))
        {
            SHIP3* ps = PshipFind(b.aslot[size_t(b.index)].item);
            const int kind = ps ? std::clamp(ps->busy, 0, 3) : 0;
            if (b.panel && s_alabel[kind] != -1) StartScreenObjPart(b.panel, s_alabel[kind]);
            if (kind == 2) { AddProgressCount("cSunk"); if (b.panel) StartScreenObjPart(b.panel, 0x7AD); }
            if (kind == 3) { AddProgressCount("cCaptured"); if (b.panel) StartScreenObjPart(b.panel, 0x7AE); }
        }
        break;
    case 4:
        if (b.index < int(b.aslot.size()))
        {
            if (b.panel) StartScreenObjPart(b.panel, s_atitle[std::min(b.index, 3)]);
            const int gain = b.aslot[size_t(b.index)].value;
            if (gain > 0)
            {
                CRef r;
                r.SetS32(std::min(CCoinsNow() + gain, 9999));
                FSetProgress({"loot", "ccoin"}, r);
                if (b.panel) StartScreenObjPart(b.panel, s_areward[std::min(b.counter, 3)]);
                ++b.counter;
            }
        }
        break;
    case 7:
        if (!b.fTwoPlayer)
            if (SHIP3* ps = PshipFind(b.target)) SetMode(*ps, 2);
        break;
    case 8:
        if (!b.fTwoPlayer)
            if (SHIP3* ps = PshipFind(b.target)) SetMode(*ps, 2);
        Broadcast3(0x5D, b.plo);
        b.aitem.clear();
        b.target = nullptr;
        break;
    default: break;
    }
    if (b.panel)
    {
        switch (b.state)
        {
        case -1: case 0: case 1: case 7: case 8: HideScreenObj(b.panel); break;
        default:
            if (old < 2 || old > 6) { ShowScreenObj(b.panel, -1); StartScreenObjPart(b.panel, 0x7A3); }
            break;
        }
    }
    b.fPaused = b.state >= 2 && b.state < 8;
}
// orig3 0x21A1C8: the listed ships fight (mode 4; the computer's chase), the others leave the world
void BeginBattle(BATTLE3& b, LO* item)
{
    if (b.aitem.size() < 2) { b.aitem.clear(); return; }
    for (SHIP3* ps : AshipWorld())
    {
        const bool fIn = std::find(b.aitem.begin(), b.aitem.end(), ps->palo) != b.aitem.end();
        if (fIn)
        {
            ps->busy = 0;
            if (ps->phase == 2) SetSubmode(*ps, 2);
            SetMode(*ps, 4);
        }
        else if (g_psw) RemoveLo(g_psw, ps->palo);
    }
    b.target = item;
    SetBattleState(b, 1);
}
// orig3 0x21A068: a battle started by the player's ship near others (the ships its camera sees)
void StartBattleNearImpl(SHIP3& s)
{
    BATTLE3& b = Battle();
    if (b.state > 0) return;
    b.aitem = {s.palo};
    for (SHIP3* po : AshipWorld())
    {
        if (po == &s || po->mode == 7) continue;
        // (0x167F60 subtracts the camera's position itself; the native FInsideCmPlanes takes a camera-relative one)
        if (!FInsideCmPlanes(&g_cm, PosOf(po->palo) - g_cm.pos, 0.0f)) continue;
        if (b.aitem.size() >= 5) break;
        b.aitem.push_back(po->palo);
    }
    if (b.aitem.size() >= 2)
    {
        b.fNoSave = 0;
        BeginBattle(b, s.palo);
    }
    else b.aitem.clear();
}
// orig3 0x219768 (vt slot 18): the tally's steps; while it runs, the ships that sailed 60000 away leave the battle
void UpdateBattle()
{
    BATTLE3& b = Battle();
    for (int guard = 0; guard < 8; ++guard)
    {
        const int st = b.state;
        const float dt = g_clock.tReal - b.tStateReal;
        switch (st)
        {
        case 2: if (!b.panel || !FScreenObjBusy(b.panel, -1) || dt > 1.0f) SetBattleState(b, 3); break;
        case 3: if (dt > 1.0f) SetBattleState(b, 4); break;
        case 4: if (dt > 1.0f) SetBattleState(b, 5); break;
        case 5:
            ++b.index;
            SetBattleState(b, b.index >= int(b.aslot.size()) ? 6 : 3);
            break;
        case 6:
        {
            // a face button (0xF0: Triangle, Circle, Cross, Square) on the first pad
            PADCTL* ppad = nullptr;
            if (SHIP3* ps = PshipFind(b.target)) ppad = PpadOf(ps->palo) ? PpadOf(ps->palo) : PpadOf(ps->owner);
            if ((ppad && GrfPadctlTakePressed(ppad, 0xF0, 0.1f)) || std::getenv("SLY3_SHIPTALLYSKIP")) SetBattleState(b, 7);
            break;
        }
        case 7: SetBattleState(b, 8); break;
        case 8: SetBattleState(b, 0); break;
        default: break;
        }
        if (b.state == st) break;
    }
    if (b.state == 1 && b.target)
        for (LO* p : b.aitem)
        {
            SHIP3* ps = PshipFind(p);
            if (!ps || ps->mode != 2 || ps->group == 5 || ps->busy) continue;
            const glm::vec3 d = PosOf(b.target) - PosOf(p);
            if (d.x * d.x + d.y * d.y > 3.6e9f)
            {
                ps->busy = 1;
                OnBattleItemGone(b, p);
            }
        }
}
bool FBattleOn() { return Battle().state > 0; }
bool Battle2P() { return Battle().fTwoPlayer != 0; }
void StartBattleNear(SHIP3& s) { StartBattleNearImpl(s); }

struct INITBATTLE3
{
    INITBATTLE3()
    {
        // 2019: (ship, ships, flag) starts a battle (0x219F70)
        RegisterObjectMethod3(0x111728, [](LO* plo, const std::vector<CRef>& a) {
            BATTLE3& b = Battle();
            if (NPrint())
            {
                std::printf("ship3: battle method 2019 on %d: %zu args (state %d):", plo ? plo->oid : -1, a.size(), b.state);
                for (const CRef& r : a)
                {
                    LO* p = dynamic_cast<LO*>(r.m_pbasic);
                    std::printf(" [tag %d obj %d cid 0x%X pair %d]", int(r.m_tagk), p ? p->oid : -1, p ? unsigned(p->Cid()) : 0u, r.m_ppair ? 1 : 0);
                    if (r.m_ppair)
                        for (const CPair* pp = r.m_ppair.get(); pp; pp = pp->m_ppairNext.get())
                        {
                            LO* q = dynamic_cast<LO*>(pp->m_ref.m_pbasic);
                            std::printf(" (%d 0x%X)", q ? q->oid : -1, q ? unsigned(q->Cid()) : 0u);
                        }
                }
                std::printf("\n");
            }
            if (b.state > 0) return CRef(TAGK_Void);
            LO* item = a.size() >= 1 ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
            if (!item) return CRef(TAGK_Void);
            b.aitem.clear();
            if (a.size() >= 2)
            {
                std::function<void(const CRef&)> add = [&](const CRef& r) {
                    if (LO* p = dynamic_cast<LO*>(r.m_pbasic); p && FShip3(p)) { if (b.aitem.size() < 5) b.aitem.push_back(p); }
                    else if (r.m_ppair)
                        for (const CPair* pp = r.m_ppair.get(); pp; pp = pp->m_ppairNext.get()) add(pp->m_ref);
                };
                add(a[1]);
            }
            if (b.aitem.size() >= 5) { b.aitem.clear(); return CRef(TAGK_Void); }
            b.aitem.push_back(item);
            b.fNoSave = a.size() >= 3 ? NOf(a[2]) : 0;
            if (NPrint()) std::printf("ship3: battle starts: %d ships, the player's %d (t %.2f)\n", int(b.aitem.size()), item->oid, g_clock.t);
            BeginBattle(b, item);
            return CRef(TAGK_Void);
        });
        // 2020: the battle ended by the script (0x21A418)
        RegisterObjectMethod3(0x111800, [](LO*, const std::vector<CRef>&) {
            BATTLE3& b = Battle();
            if (b.state > 0) SetBattleState(b, 0);
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter3(2017, [](LO*) { CRef r; r.SetBool(Battle().state > 0); return r; });
        RegisterObjectSetter3(2021, [](LO*, const CRef& r) { Battle().fTwoPlayer = NOf(r); });
        RegisterObjectGetter3(2021, [](LO*) { CRef r; r.SetS32(Battle().fTwoPlayer); return r; });
        for (int i = 0; i < 4; ++i) SetScriptString3(1960 + i, [i] { return StrBattleValue(i); });
        // (for s3-script) SCPT EOPID 756 (optid 740, kind 0x2002, the field +0x58): the object that started the script
        // (StartScpt 0x225EE8's: the battle intro 4122's is the gang's ship 44092, the reference's RAM, which its handler
        // passes to 2019); natively it had no getter (nil)
        RegisterObjectGetter3(756, [](LO* plo) {
            CRef r;
            if (static const bool s_fOff = std::getenv("SLY3_NO756") != nullptr; s_fOff) return r;
            SCPT3* ps = plo ? PscptOfLo3(plo) : nullptr;
            if (ps && ps->ploStarter) r.SetBasic(static_cast<LO*>(ps->ploStarter));
            if (NPrint() >= 2) std::printf("ship3: SCPT %d's starter (756): %d\n", plo ? plo->oid : -1, ps && ps->ploStarter ? static_cast<LO*>(ps->ploStarter)->oid : -1);
            return r;
        });
        // built-in 126 (orig3 0x1266B8): the battle manager D_00468D24
        SetBifGameHook(BIFK(126), [](int, CRef*, CFrame*) {
            CRef r;
            BATTLE3& b = Battle();
            if (!b.plo && g_psw) b.plo = PloFindSwObjectByOid(g_psw, 1943);
            if (b.plo) r.SetBasic(b.plo);
            return r;
        });
    }
} s_initbattle3;
}  // namespace

bool FShipInBattle3(const LO* plo)   // 0x21A4E8
{
    BATTLE3& b = Battle();
    if (b.state <= 0 || CItemsLeft(b) != 1) return false;
    return std::find(b.aitem.begin(), b.aitem.end(), plo) != b.aitem.end();
}
void OnShipOut3(LO* plo) { OnBattleItemGone(Battle(), plo); }
// orig3 0x21A570 (the boarded ship's out, 0x218B68): the battle's player ship (+0x40) put beside the given ship: the first
// of 8 directions (D_00325D70) x 2, 4, 8 of the player ship's length (+0x220) from it that is on the player ship's nav
// mesh (+0x250, 0x1ED170), nearest the player ship (in XY); then that ship's mode 6 (still)
void ParkBeside3(LO* plo)
{
    BATTLE3& b = Battle();
    SHIP3* pt = b.target ? PshipFind(b.target) : nullptr;
    if (!pt || !plo) return;
    static const glm::vec2 s_adir[8] = {{-1.0f, 0.0f}, {-0.707f, -0.707f}, {0.0f, -1.0f}, {0.707f, -0.707f},
                                        {1.0f, 0.0f}, {-0.707f, 0.707f}, {0.0f, 1.0f}, {0.707f, 0.707f}};
    const glm::vec3 posT = PosOf(pt->palo), posS = PosOf(plo);
    glm::vec3 best = posT;
    float dBest = 3.4028235e38f;
    bool fFound = false;
    float k = 2.0f;
    for (int ring = 0; ring < 3 && !fFound; ++ring, k += k)
        for (const glm::vec2& d : s_adir)
        {
            const glm::vec3 c = posS + glm::vec3(d * (k * pt->length), 0.0f);
            if (pt->nav && !FOnNav(*pt, c)) continue;
            const float dd = (c.x - posT.x) * (c.x - posT.x) + (c.y - posT.y) * (c.y - posT.y);
            if (!(dd < dBest)) continue;
            dBest = dd, best = c, fFound = true;
        }
    if (fFound) SetAloPos(pt->palo, best);   // (vt slot 34)
    if (NPrint()) std::printf("ship3: %d parked beside %d at %.0f %.0f %.0f (found %d)\n", pt->palo->oid, plo->oid, best.x, best.y, best.z, int(fFound));
    SetMode(*pt, 6);
}
void UpdateShipBattle3() { UpdateBattle(); }
int CShipsSunkHud3() { return Battle().cSunkHud; }
int CShipsLeftHud3() { BATTLE3& b = Battle(); return b.state > 0 ? CItemsLeft(b) + b.cSunkHud : 0; }
