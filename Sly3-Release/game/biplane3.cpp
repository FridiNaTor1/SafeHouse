// biplane3.cpp: Sly 3's planes (class 0x52, orig3 0x141058..0x147498), from the Sly 3 decomp.
//
// One class for Bentley's biplane (+0x198 mode 0: the pad) and the computer's planes (mode 1: the dogfighters, mode 2:
// flying to a point, mode 3: no flight control). Ep3 h_dogfight (world 21): the player's plane 95691 (team +0x194 6),
// 31 copies of 95883 (mode 1, teams 0 / 1, +0x190 1: they die when rammed and their death is broadcast 0x53); the world's
// flight bands are its SW options 991..994 (SW +0x4E18 / +0x4E1C the low band, +0x4E20 / +0x4E24 the high band, +0x4E28
// the computer's range, +0x4E2C the player's (XY), +0x4E30 the ceiling; +0x4E34 counts the planes, 0x141350).
//
// The flight: every frame the Euler angles of the object (+0x1E0) are smoothed toward the targets (+0x404 roll, +0x408
// pitch, +0x40C yaw) by the SMPs +0x1BC.. and the rotation record gets the angular velocity that reaches them in a frame;
// the position record is accelerated (at most +0x1A8) toward the speed +0x400 along the new nose (the forces 0x141870, vt
// slot 74). The pad (0x142E18): the stick rolls / yaws (+0x1B0 / +0x1B8) and pitches (+0x1B4, at most +0x1AC), X the
// boost (+0x1A0), Square the brake (+0x1A4), R1 the guns (+0x23C's projectiles from the muzzles +0x244 by the launcher
// +0x240, +0x27C a second, +0x174 the ammunition unless +0x290), Circle (+ the stick) a barrel roll / U-turns (states
// 1..4 of +0x2B8), Triangle a loop (5). Out of the player's range (XY > +0x4E2C): state 2 (the fade +0x42C, then put back
// at the middle facing in, 0x146B80); above the ceiling: state 3 (nose down, the clip +0x2E0, level again); hit by a
// ram: state 4 (the crash avoidance 0x146558). The computer (0x143E50): the target picker 0x1448B0 (the smallest
// distance, penalized out of the nose's cone, preferring its current target and the player), the chase state 0x144640
// (0 cruise, 1 chase, 2 evade the plane chasing it), the predicted aim 4500 ahead of the player / 1500 behind a plane, the
// bands (climb under +0x4E1C, dive over +0x4E20), the avoid shape +0x388 and the fire decision 0x144A90 (a target inside
// +0x3C0 / +0x294 that a camera sees). Hits (vt slot 78, 0x141B78): the hit points (+0x16C) less the amount, a fatal hit
// (the record's +0x28) kills; none left: state 5 (the death effect +0x384, out of the world, broadcast 0xC for the player
// (the checkpoint) or 0x53 for a plane with +0x190 1).
//
// Not ported (noted): the sounds (0x142650 the weapon's, 0x146360 the engine's, 0x146D08 the ceiling warning, the
// shuffled lines 0x147278), the rumble's player rows other than the current player's, the pilot's head target (0x146058's
// second half), the clones of the impact effect (0x141350: one effect object), the power-up weapons' HUD (+0x434), the
// 2P "hijack" (+0x4BC: another plane's right stick steers for 6 s after its hit 0x800000) is kept but untested, the
// shape-vs-shape pass of a tracked shape (+0x358, 0x1451C8's second half: the "fly through" rings).
// Debug: SLY3_PLANEPRINT=1 (options, states, hits), =2 (and every 30 frames each plane).

#include "../activate.h"
#include "../alo.h"
#include "../asega.h"
#include "../brx.h"
#include "../clock.h"
#include "../cm.h"
#include "../cplcy.h"
#include "../detect.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../pe.h"
#include "../po.h"
#include "../proc.h"
#include "../frame.h"
#include "../sidebag.h"
#include "../projectile.h"
#include "../ref.h"
#include "../routetele.h"
#include "../screenrt.h"
#include "../rumble.h"
#include "../smrun.h"
#include "../so.h"
#include "../sound.h"
#include "../splitscreen3.h"
#include "../spliceglue.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../vtnative.h"
#include "../xf.h"
#include "../xfphys.h"
#include "biplane3.h"
#include "sly3.h"
#include "task3.h"
#include "suck3.h"

#include <algorithm>
#include <map>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset);   // launcher.cpp
void LaunchFrom(LO* plaunch, LO* po);                                     // launcher.cpp
void StartFadeLo(LO* plo);                                                // hudwidget.cpp (orig3 0x17D7D0)
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);   // mat.cpp 0x11B830
glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);                         // mat.cpp 0x11B7E0
glm::mat3 LevelMatrix3(const glm::mat3& matSrc, glm::mat3* pmatDst);       // mat.cpp 0x11B938
float GRandInRange(float gLow, float gHigh);                                 // 0x11D448
int NRandInRange(int nLow, int nHigh);                                       // 0x11D3F8
void PrintRef(const CRef& ref, std::string* ps);                             // bif.cpp (debug)

namespace
{
int NPrint()
{
    static const int s_n = std::getenv("SLY3_PLANEPRINT") ? std::atoi(std::getenv("SLY3_PLANEPRINT")) : 0;
    return s_n;
}

constexpr float FLTMAX = 3.40282347e38f;
constexpr float PI = 3.14159274f;
constexpr float PI2 = 6.28318548f;
const cmm::LM s_lmUnit{0.0f, 1.0f};       // D_00341FF0
const cmm::LM s_lmSigned{-1.0f, 1.0f};    // D_00341FF8
const cmm::LM s_lmStick{0.1f, 0.8f};      // D_003201F8

struct PLANEGUN
{
    int oidMuzzle = -1, oidFlash = -1;    // the option's ids (resolved by 0x146F18)
    LO* muzzle = nullptr;                 // +0x0
    LO* flash = nullptr;                  // +0x4 (a PE)
    bool fResolved = false;               // +0x8
};

// orig3: the class's fields from +0x190 (the option offsets of EOPIDs 1684..1774)
struct PLANE3
{
    ALO* palo = nullptr;
    SW* psw = nullptr;
    bool fPostLoad = false, fReset = false;
    int k190 = 0, team = 0, mode = 0;                  // +0x190 / +0x194 / +0x198
    float speed = 1800.0f, boost = 1.6f, brake = 0.7f; // +0x19C / +0x1A0 / +0x1A4
    float accel = 10000.0f;                            // +0x1A8
    float pitchMax = 1.22173059f, rollMax = 1.13446414f;   // +0x1AC / +0x1B0
    float pitchRate = 0.261799425f, yawRate = 0.52359885f; // +0x1B4 / +0x1B8
    cmm::SMP asmp[3]{{2.0f, 0.0f, 1.0f}, {0.8f, 0.0f, 1.0f}, {0.6f, 0.0f, 1.0f}};   // +0x1BC / +0x1C8 / +0x1D4
    glm::vec3 eul{0.0f};                               // +0x1E0 the object's Euler angles (each update)
    float stun = 0.25f, crashPush = 4000.0f;           // +0x1F0 / +0x1F4
    int ramDamage = 0, bumpDamage = 0;                 // +0x1F8 / +0x1FC
    float escape = 500.0f;                             // +0x200 (the crash avoidance's speed)
    LO* fadeHit = nullptr; int fFadeHit = 0;           // +0x204 / +0x208 (the player's hit: a fade instead of a stun)
    LO* prop = nullptr;                                // +0x210 (the propeller: angular velocity by the speed)
    ALO* pilot = nullptr;                              // +0x214 (the pilot's clips' object)
    ASEG* aaseg[4]{};                                  // +0x218 / +0x21C / +0x220 / +0x224 (level, roll, pitch, dead)
    ASEGA* pasegaPilot = nullptr;                      // +0x228
    ASEG* asegGun[2]{};                                // +0x22C / +0x230 (firing / empty)
    ASEGA* pasegaGun = nullptr;                        // +0x234
    LO* gunResource = nullptr, *launcher = nullptr;    // +0x23C / +0x240
    std::vector<PLANEGUN> aguns;                       // +0x244 (4), +0x274 the count
    int igun = 0;                                      // +0x278
    float rate = 10.0f, range = 15000.0f;              // +0x27C / +0x280
    float tShot = 0.0f;                                // +0x284
    cmm::LM lmBurst{0.0f, 0.0f};                       // +0x288 (the computer's pause between bursts)
    int fInfinite = 0;                                 // +0x290
    LO* shapeFire = nullptr;                           // +0x294
    float tFireCheck = 0.0f; int fFire = 0;            // +0x298 / +0x29C
    float tClick = 0.0f;                               // +0x2A0
    float camBoost = -150.0f, camBrake = 200.0f;       // +0x2A8 / +0x2AC
    int state = 0; float tState = 0.0f;                // +0x2B0 / +0x2B4
    int man = 0; float tMan = 0.0f;                    // +0x2B8 / +0x2BC (the maneuver)
    int sub = -1, subPrev = -1; float tSub = 0.0f;     // +0x2C0 / +0x2C4 / +0x2C8
    float tTap = FLTMAX;                               // +0x2CC (Circle's press)
    float rateSub = 0.0f, durSub = 0.0f;               // +0x2D0 / +0x2D4
    int fBlend = 0; float tBlend = 0.0f;               // +0x2D8 / +0x2DC
    ASEG* asegStall = nullptr; ASEGA* pasegaStall = nullptr;   // +0x2E0 / +0x2E4
    int st3 = -1; float tSt3 = 0.0f;                   // +0x2F4 / +0x2F8
    ASEG* asegSurf[3]{}; ASEGA* pasegaSurf[3]{};       // +0x2FC.. (ailerons, elevator, rudder)
    std::vector<LO*> aploBoost;                        // +0x314 (2): on while boosting
    LO* ploHit = nullptr; float tHit = 0.0f;           // +0x320 / +0x324 (the contact's effect)
    LO* pePass = nullptr;                              // +0x328
    std::vector<LO*> aploIn, aploOut;                  // +0x32C (4) / +0x340 (4): the shape pass's effects
    float tOut = 2.0f;                                 // +0x354
    LO* tracked = nullptr; float tTracked = -FLTMAX;   // +0x358 / +0x35C
    LO* aploImpact[4]{}; float atImpact[4]{};          // +0x360 (the impact effects, by age)
    LO* peCrash = nullptr, *peDeath = nullptr;         // +0x380 / +0x384
    LO* shapeAvoid = nullptr; float tAvoid = 0.0f; int fAvoid = 0;   // +0x388 / +0x38C / +0x390
    int ai = 0; float tAiState = 0.0f;                 // +0x394 / +0x398
    int serial = 0;                                    // +0x39C
    glm::vec3 posTarget{0.0f};                         // +0x3A0 (mode 2's point, option 1716; mode 1's aim)
    float tAim = 0.0f, tWander = 0.0f;                 // +0x3B0 / +0x3B4
    PLANE3* ptarget = nullptr; float tPick = 0.0f;     // +0x3B8 / +0x3BC
    LO* region = nullptr;                              // +0x3C0
    PLANE3* pchaser = nullptr; float tChaser = 0.0f;   // +0x3C4 / +0x3C8
    int fBoostOk = 1, fBrakeOk = 1;                    // +0x3CC / +0x3D0
    float spd = 0.0f, roll = 0.0f, pitch = 0.0f, yaw = 0.0f;   // +0x400 / +0x404 / +0x408 / +0x40C
    int fBoosting = 0; float tBoost = 0.0f; int fBraking = 0;  // +0x410 / +0x414 / +0x418
    int fRamProof = 0;                                 // +0x424
    LO* ploRam = nullptr;                              // +0x428 (EOPID 1753): the plane it collides with, during the hit
    LO* ploFade = nullptr;                             // +0x42C (out of range)
    int wpnDefault = -1, wpn = -1;                     // +0x430 / +0x438
    float tWpn = 0.0f; int cShots = 0, cShotsMax = 0;  // +0x43C / +0x440 / +0x444
    LO* partner = nullptr;                             // +0x478 (2P: the other player's plane)
    LO* ploWpnFrom = nullptr, *ploWpnAt = nullptr;     // +0x47C / +0x480
    LO* peWpn = nullptr;                               // +0x484
    LO* awpnRes[7]{};                                  // +0x488..+0x4A0 (power-ups 0x36..0x38, 0x3A, 0x3B, 0x3C, 0x3D)
    SM* psmShield = nullptr; SMA* psmaShield = nullptr;   // +0x4A4 / +0x4A8 (0x1A0 off, 0x1A1 on)
    LO* peShield = nullptr; float tShield = 0.0f;      // +0x4AC / +0x4B0
    float tWpnLast = -FLTMAX, tWpnFire = 0.0f;         // +0x4B4 / +0x4B8
    LO* hijacker = nullptr; float tHijack = 0.0f;      // +0x4BC / +0x4C0
    LO* peHijack = nullptr;                            // +0x4C4
    LO* peHit = nullptr; float tPeHit = FLTMAX;        // +0x4E0 / +0x4E4
    LO* ploWpnHud = nullptr; int idWpnPart = -1;       // +0x448 (EOPID 1757: the power-up HUD, class 0xA0) / +0x44C its part
    int aidHijackLine[4]{}, cHijackLine = 0, iHijackLine = 0;   // +0x4C8..+0x4DC (1771: the hijack's lines, shuffled)
    int aidEmptyLine[4]{}, cEmptyLine = 0, iEmptyLine = 0;      // +0x4E8..+0x4FC (1773: the ammunition emptied's lines)
    int ammo = 0, ammoMax = 300;                       // the SO's +0x174 / +0x178
    int sndCeiling = -1;                               // +0x2E8 (EOPID 1726): the ceiling's warning
    float tCeilWarn = -FLTMAX, tCeilSnd = -FLTMAX;     // +0x2EC / +0x2F0
    int asndEngine[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};   // +0x3D4..+0x3F4 (EOPIDs 1743..1751): the engine's loops
    CSndRef sndEngine; int sndEngineId = -1;           // +0x3F8 (the loop playing, its id)
    // (native) the camera policy of the player's plane
    bool fCam = false;
};
std::unordered_map<const LO*, PLANE3> s_mpplane;
std::unordered_map<const LO*, std::vector<std::pair<int, int>>> s_mpgunPending;   // the load's 1701 calls (muzzle, flash ids)
std::unordered_map<const LO*, std::vector<std::pair<int, int>>> s_mplinePending;  // the load's 1771 / 1773 calls (kind 0 / 1, line)
std::unordered_map<const LO*, std::vector<std::pair<int, CRef>>> s_mpoptPending;  // setters before the record (the start events)
struct SHOOTER3 { LO* plo = nullptr; float t = -1.0f; };
std::unordered_map<const LO*, SHOOTER3> s_mpshooter;   // (PC, duel3.h) a plane's last hitter (a plane) and when

PLANE3* PplaneFind(const LO* plo)
{
    auto it = s_mpplane.find(plo);
    return it != s_mpplane.end() && it->second.psw == g_psw ? &it->second : nullptr;
}

bool FIsPlane(const LO* plo) { return plo && plo->pcls && FIsLoDerivedFrom(plo, 0x52); }

// the world's flight bands (SW options 991..994; the reference's h_dogfight: 1600 4000 / 4500 5500 / 16000 25000 / 7500)
struct BANDS3
{
    SW* psw = nullptr;
    cmm::LM lmLow{0.0f, 0.0f}, lmHigh{0.0f, 0.0f};     // +0x4E18 / +0x4E20
    float rangeAi = 0.0f, rangePlayer = 0.0f;          // +0x4E28 / +0x4E2C
    float ceiling = 0.0f;                              // +0x4E30
    int cPlanes = 0;                                   // +0x4E34
};
BANDS3 s_bands;
BANDS3& Bands()
{
    if (s_bands.psw == g_psw) return s_bands;
    s_bands = BANDS3{};
    s_bands.psw = g_psw;
    if (g_psw)
        for (const OPT& o : g_psw->aoptSw)
        {
            if (o.aval.empty()) continue;
            const OPTVAL& a = o.aval[0];
            switch (o.eopid)
            {
            case 991: s_bands.lmLow = {a.ag[0], a.ag[1]}; break;
            case 992: s_bands.lmHigh = {a.ag[0], a.ag[1]}; break;
            case 993: s_bands.rangeAi = a.ag[0]; s_bands.rangePlayer = a.ag[1]; break;
            case 994: s_bands.ceiling = a.ag[0]; break;
            default: break;
            }
        }
    if (NPrint())
        std::printf("plane3: bands low %.0f %.0f high %.0f %.0f range %.0f / %.0f ceiling %.0f\n", s_bands.lmLow.gMin, s_bands.lmLow.gMax, s_bands.lmHigh.gMin,
                    s_bands.lmHigh.gMax, s_bands.rangeAi, s_bands.rangePlayer, s_bands.ceiling);
    return s_bands;
}

// ---- the object's motion (vt slots 36 / 33 / 35 / 36 of the copied numbering) ----
glm::mat4 MatObj(ALO* palo)
{
    XF* pxf = palo ? PxfObj(palo) : nullptr;
    return pxf ? pxf->matWorld : glm::mat4(1.0f);
}
glm::vec3 PosOf(ALO* palo) { return glm::vec3(MatObj(palo)[3]); }
glm::vec3 FwdOf(ALO* palo) { return glm::vec3(MatObj(palo)[0]); }
glm::mat4 MatLo(LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO)) return MatObj(static_cast<ALO*>(plo));
    if (const LOX* plox = dynamic_cast<const LOX*>(plo)) return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    return glm::mat4(1.0f);
}
bool FInWorld(const LO* plo) { return plo && plo->fInWorld; }
void Remove(LO* plo) { if (plo && plo->fInWorld) RemoveLo(g_psw, plo); }   // vt slot 9
void Add(LO* plo) { if (plo && !plo->fInWorld) AddLo(g_psw, plo); }        // vt slot 8

// the SO's hit points (+0x16C / +0x170, so EOPIDs 937 / 938 answer) and ammunition (+0x174 / +0x178)
int Hp(const PLANE3& p) { return static_cast<int32_t>(p.palo->U32Field(0, 0x16C, 0)); }
int HpMax(const PLANE3& p) { return static_cast<int32_t>(p.palo->U32Field(0, 0x170, 100)); }
void SetHp(PLANE3& p, int n) { p.palo->SetField(0, 0x16C, static_cast<uint32_t>(std::clamp(n, 0, std::max(HpMax(p), 0)))); }   // 0x1F43D0
void SetHpMax(PLANE3& p, int n) { p.palo->SetField(0, 0x170, static_cast<uint32_t>(n)); }                                 // 0x1F43F8 (part)
void SetAmmo(PLANE3& p, int n)   // 0x1F4438
{
    p.ammo = std::clamp(n, 0, std::max(p.ammoMax, 0));
    p.palo->SetField(0, 0x174, static_cast<uint32_t>(p.ammo));
    p.palo->SetField(0, 0x178, static_cast<uint32_t>(p.ammoMax));
}

PADCTL* Ppad(PLANE3& p)
{
    PADCTL* ppad = PpadctlOfSo3(p.palo);
    return ppad && ppad->nControl == 1 ? ppad : nullptr;
}
// the pad's input interface (+0x134): the sticks, the deflection, the buttons down
struct INPUT3
{
    float x = 0, y = 0, u = 0, x2 = 0, y2 = 0;
    uint32_t grf = 0;
};
// the player (0..3) whose plane this is: the current player's (0), else split3's players (the 2P duel's second), -1 none
int IPlayerOfPlane(const LO* plo)
{
    if (!plo || !g_psw) return -1;
    if (PloPoCurrent(g_psw) == plo) return 0;
    for (int i = 1; i < split3::kMaxSeats; i++)
        if (split3::PloPlayer(g_psw, i) == plo) return i;
    return -1;
}
// (test, tools/duel3.sh) SLY3_DUELAUTO=mask: player i's plane (bit i) flown by the harness's autopilot (FDogfightStick3)
// instead of its pad; SLY3_DUELNOFIRE=mask: those players fly level and never fire (the other wins). Nothing without
// the variables.
int GrfDuelAuto(const char* sz)
{
    const char* p = std::getenv(sz);
    return p ? int(std::strtol(p, nullptr, 0)) : 0;
}
bool FDuelAuto(const PLANE3& p, int* piPlayer)
{
    static const int s_grf = GrfDuelAuto("SLY3_DUELAUTO");
    if (!s_grf || p.mode != 0) return false;
    const int i = IPlayerOfPlane(p.palo);
    if (piPlayer) *piPlayer = i;
    return i >= 0 && (s_grf >> i) & 1;
}
struct DUELAUTO3 { float t = -1.0f; INPUT3 in; int cFrame = 0; };
std::unordered_map<const LO*, DUELAUTO3> s_mpduelauto;
INPUT3 InputDuelAuto(PLANE3& p, int iPlayer)
{
    DUELAUTO3& d = s_mpduelauto[p.palo];
    if (d.t == g_clock.t) return d.in;
    d.t = g_clock.t;
    d.cFrame++;
    d.in = INPUT3{};
    float x = 0.0f, y = 0.0f;
    bool fFire = false;
    int nSpeed = 0;
    static const int s_grfNoFire = GrfDuelAuto("SLY3_DUELNOFIRE");
    if ((s_grfNoFire >> iPlayer) & 1) return d.in;   // (a passive target: level flight, no fire)
    if (FDogfightStick3(p.palo, &x, &y, &fFire, &nSpeed))
    {
        d.in.x = x;
        d.in.y = y;
        d.in.u = std::min(1.0f, std::sqrt(x * x + y * y));
        if (fFire) d.in.grf |= 8;           // R1: the guns
        if (fFire && d.cFrame % 8 < 2) d.in.grf |= 4;   // L1: the power-up weapon (a press every 8 frames)
        if (nSpeed > 0) d.in.grf |= 0x40;   // X: the boost
        else if (nSpeed < 0) d.in.grf |= 0x80;   // Square: the brake
    }
    return d.in;
}

INPUT3 Input(PLANE3& p)
{
    INPUT3 in;
    if (int i = -1; FDuelAuto(p, &i)) return InputDuelAuto(p, i);
    if (const PADCTL* ppad = Ppad(p)) in = {ppad->x, ppad->y, ppad->uJoy, ppad->x2, ppad->y2, ppad->grfbtn};
    return in;
}
// 0x1A7A80: the buttons pressed within dt, taken
uint32_t GrfTake(PLANE3& p, uint32_t grf, float dt)
{
    if (int i = -1; FDuelAuto(p, &i)) return InputDuelAuto(p, i).grf & grf & 4;   // (test: only L1, the weapon)
    PADCTL* ppad = Ppad(p);
    return ppad ? GrfPadctlTakePressed(ppad, grf, dt) : 0;
}

LO* PloOpt(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    return v.n == -1 || v.n == 0 ? nullptr : PloFindRelative(plo, v.n);
}
// (an object option of a copy: its own object of that id, not the template's the loaded value may point to)
LO* PloRef(LO* plo, const CRef& r)
{
    if (r.m_tagk == TAGK_S32) return r.m_n == -1 || r.m_n == 0 ? nullptr : PloFindRelative(plo, r.m_n);
    LO* pref = dynamic_cast<LO*>(r.m_pbasic);
    if (pref && plo->ploCloneOf && pref->oid > 0)
        if (LO* pown = PloFindRelative(plo, pref->oid)) return pown;
    return pref;
}
template <class T> T* Pas(LO* plo) { return dynamic_cast<T*>(plo); }

// the planes in the world (orig3: the list D_00479BD4, linked at +0x41C by the add / remove slots 11 / 12)
std::vector<PLANE3*> AplaneWorld();

void SetState(PLANE3& p, int st);
void SetMan(PLANE3& p, int st);
void SetSub(PLANE3& p, int st);
void SetSt3(PLANE3& p, int st);
void SetWeapon(PLANE3& p, int st);
float SpeedBoost(const PLANE3& p) { return p.fBoostOk ? p.speed * p.boost : p.speed; }   // 0x144A50
float SpeedBrake(const PLANE3& p) { return p.fBrakeOk ? p.speed * p.brake : p.speed; }   // 0x144A70

int NOf(const CRef& r) { return r.m_tagk == TAGK_F32 ? static_cast<int>(r.m_g) : r.m_tagk == TAGK_Bool ? r.m_bool : r.m_n; }
float GOf(const CRef& r) { return r.m_tagk == TAGK_S32 ? static_cast<float>(r.m_n) : r.GAsFloat(); }
void SetWeapon(PLANE3& p, int st);

// a field option of the class (EOPIDs 1684..1774: the loader's value, or a script's seto!); false: not one of them
bool ApplyOption(PLANE3& p, int eopid, const CRef& r)
{
    ALO* palo = p.palo;
    auto obj = [&]() { return PloRef(palo, r); };
    switch (eopid)
    {
    case 1684: p.k190 = NOf(r); break;
    case 1685: p.team = NOf(r); break;
    case 1686: p.mode = NOf(r); break;
    case 1687: p.speed = GOf(r); break;
    case 1688: p.boost = GOf(r); break;
    case 1689: p.brake = GOf(r); break;
    case 1690: p.accel = GOf(r); break;
    case 1691: p.pitchMax = GOf(r); break;
    case 1692: p.rollMax = GOf(r); break;
    case 1693: p.pitchRate = GOf(r); break;
    case 1694: p.yawRate = GOf(r); break;
    case 1695: case 1696: case 1697: p.asmp[eopid - 1695] = {r.m_vector.x, r.m_vector.y, r.m_vector.z}; break;
    case 1698: p.gunResource = obj(); break;
    case 1699: p.launcher = obj(); break;
    case 1702: p.rate = GOf(r); p.ammoMax = static_cast<int>(p.rate) * 30; break;
    case 1703: p.range = GOf(r); break;
    case 1704: p.fInfinite = NOf(r); break;
    case 1705: p.region = obj(); break;
    case 1706: p.shapeFire = obj(); break;
    case 1707: p.shapeAvoid = obj(); break;
    case 1708: SetState(p, NOf(r)); break;
    case 1709: p.stun = GOf(r); break;
    case 1710: p.crashPush = GOf(r); break;
    case 1711: p.ramDamage = NOf(r); break;
    case 1712: p.bumpDamage = NOf(r); break;
    case 1713: p.escape = GOf(r); break;
    case 1714: p.fadeHit = obj(); break;
    case 1715: p.fFadeHit = NOf(r); break;
    case 1716: p.posTarget = r.GetVector(); break;
    case 1717: p.spd = GOf(r); break;
    case 1718: p.camBoost = GOf(r); break;
    case 1719: p.camBrake = GOf(r); break;
    case 1720: p.prop = obj(); break;
    case 1721:   // the pilot (0x146FD0: its clips bound by the table D_00342CC0: 1923 level, 1924 roll, 1925 pitch, 1929 dead,
                 // 1926 / 1927 the guns firing / empty)
    {
        LO* plo = obj();
        if (plo == p.pilot) break;
        RetractAsegaPp(&p.pasegaPilot);
        RetractAsegaPp(&p.pasegaGun);
        p.pilot = plo && FIsLoDerivedFrom(plo, sly3::CID_ALO) ? static_cast<ALO*>(plo) : nullptr;
        static const int s_aoid[6] = {1923, 1924, 1925, 1929, 1926, 1927};
        ASEG** apaseg[6] = {&p.aaseg[0], &p.aaseg[1], &p.aaseg[2], &p.aaseg[3], &p.asegGun[0], &p.asegGun[1]};
        for (int i = 0; i < 6; i++) *apaseg[i] = p.pilot ? Pas<ASEG>(PloFindRelative(p.pilot, s_aoid[i])) : nullptr;
        break;
    }
    case 1722: p.asegStall = Pas<ASEG>(obj()); break;
    case 1723: case 1724: case 1725: p.asegSurf[eopid - 1723] = Pas<ASEG>(obj()); break;
    case 1728: if (p.aploBoost.size() < 2) p.aploBoost.push_back(obj()); break;
    case 1729: p.ploHit = obj(); break;
    case 1730: p.pePass = obj(); break;
    case 1732: if (p.aploIn.size() < 4) p.aploIn.push_back(obj()); break;
    case 1734: if (p.aploOut.size() < 4) p.aploOut.push_back(obj()); break;
    case 1735: p.tOut = GOf(r); break;
    case 1736: p.aploImpact[0] = obj(); break;
    case 1737: p.peCrash = obj(); break;
    case 1738: p.peDeath = obj(); break;
    case 1740: p.fBoostOk = NOf(r); break;
    case 1741: p.fBrakeOk = NOf(r); break;
    case 1726: p.sndCeiling = NOf(r); break;
    case 1743: case 1744: case 1745: case 1746: case 1747: case 1748: case 1749: case 1750: case 1751: p.asndEngine[eopid - 1743] = NOf(r); break;
    case 1752: p.fRamProof = NOf(r); break;
    case 1754: p.ploFade = obj(); break;
    case 1755: p.partner = obj(); break;
    case 1756: p.wpnDefault = NOf(r); if (p.fPostLoad) SetWeapon(p, p.wpnDefault); break;
    case 1758: p.ploWpnFrom = obj(); break;
    case 1759: p.ploWpnAt = obj(); break;
    case 1760: p.peWpn = obj(); break;
    case 1762: case 1763: case 1764: case 1765: case 1766: case 1767: case 1768: p.awpnRes[eopid - 1762] = obj(); break;
    case 1769: p.psmShield = Pas<SM>(obj()); if (p.fPostLoad && p.psmShield) p.psmaShield = PsmaApplySm(p.psmShield, palo, -1, 1); break;
    case 1770: p.peShield = obj(); break;
    case 1772: p.peHijack = obj(); break;
    case 1774: p.peHit = obj(); break;
    case 1757: p.ploWpnHud = obj(); break;
    default: return false;
    }
    return true;
}

// ---- init / options (0x141058, the option fields, the post-load 0x141350) ----
void InitPlane(PLANE3& p)
{
    p = PLANE3{p.palo, p.psw};
    SetHpMax(p, 100);                                       // 0x1F43F8(s, 100)
    p.ramDamage = 100 / 3 + 1;                              // +0x1F8
    p.bumpDamage = 100 / 8;                                 // +0x1FC
    p.ammoMax = static_cast<int>(p.rate) * 30;              // +0x178 = (int)+0x27C * 30
    ALO* palo = p.palo;
    for (const OPT& o : palo->aopt)
        for (const OPTVAL& v : o.aval) ApplyOption(p, o.eopid, RefFromOptionValue(v));
    if (auto it = s_mpoptPending.find(palo); it != s_mpoptPending.end())
    {
        const auto aopt = std::move(it->second);
        s_mpoptPending.erase(it);
        for (const auto& [e, r] : aopt) ApplyOption(p, e, r);
    }
    // the SO's hit points as loaded (EOPIDs 937 / 938), else the init's 100
    for (const OPT& o : palo->aopt)
        if (!o.aval.empty() && o.eopid == 937) SetHpMax(p, o.aval[0].n);
    p.ammoMax = static_cast<int>(p.rate) * 30;
    // the post-load 0x141350: the position / rotation records in mode 2, the serial; the shield's SMA
    if (XF* pxf = PxfPos(palo); pxf && !FXfModeMoving(pxf->mode)) SetXfMode(palo, pxf, 2);
    if (XF* pxf = PxfRot(palo); pxf && !FXfModeMoving(pxf->mode)) SetXfMode(palo, pxf, 2);
    if (p.psmShield) p.psmaShield = PsmaApplySm(p.psmShield, palo, -1, 1);
    p.serial = Bands().cPlanes++;
    // the load's lines (1771 / 1773; this object's, else its template's)
    for (const LO* plo = palo; plo && !p.cHijackLine && !p.cEmptyLine; plo = plo->ploCloneOf)
        if (auto it = s_mplinePending.find(plo); it != s_mplinePending.end())
            for (const auto& [k, id] : it->second)
            {
                int* aid = k ? p.aidEmptyLine : p.aidHijackLine;
                int& c = k ? p.cEmptyLine : p.cHijackLine;
                if (c < 4) aid[c++] = id;
                (k ? p.iEmptyLine : p.iHijackLine) = c;
            }
    // the load's guns (this object's, else its template's: a copy of a proxy's group)
    for (const LO* plo = palo; plo && p.aguns.empty(); plo = plo->ploCloneOf)
        if (auto it = s_mpgunPending.find(plo); it != s_mpgunPending.end())
            for (const auto& [oidM, oidF] : it->second)
            {
                PLANEGUN g;
                g.oidMuzzle = oidM;
                g.oidFlash = oidF;
                p.aguns.push_back(g);
            }
    // 0x146F18: the guns' ids to objects
    for (PLANEGUN& g : p.aguns)
    {
        if (!g.muzzle && g.oidMuzzle > 0) g.muzzle = PloFindRelative(palo, g.oidMuzzle);
        if (!g.flash && g.oidFlash > 0) g.flash = PloFindRelative(palo, g.oidFlash);
        g.fResolved = true;
    }
    p.fPostLoad = true;
    if (NPrint()) std::printf("plane3: %d init: in world %d removed %d spare %d clone of %p iclone %d t %.3f\n", palo->oid, palo->fInWorld, palo->fRemoved, palo->fSpareNs, (void*)palo->ploCloneOf, palo->iclone, g_clock.t);
    if (std::getenv("SLY3_PLANESYM") && palo->psidebag)
        for (const SBB& b : palo->psidebag->m_asbb)
        {
            std::string str;
            PrintRef(b.ref, &str);
            std::printf("plane3: %d sidebag %d = %s\n", palo->oid, b.n, str.substr(0, 200).c_str());
        }
    // (debug: SLY3_PLANESYM=SYM[,SYM..]: the bindings of the plane's splice module)
    if (const char* sz = std::getenv("SLY3_PLANESYM"); sz && palo->pframe)
        for (const char* q = sz; *q;)
        {
            const int sym = std::atoi(q);
            if (CRef* pr = palo->pframe->PrefFindBinding(static_cast<SYMID>(sym), 1))
            {
                std::string str;
                CRef body;
                if (pr->m_tagk == TAGK_Proc && pr->m_pproc) body.SetPair(pr->m_pproc->m_ppairCodeExpr);
                PrintRef(pr->m_tagk == TAGK_Proc && pr->m_pproc ? body : *pr, &str);
                std::printf("plane3: %d sym %d: %s\n", palo->oid, sym, str.c_str());
            }
            while (*q && *q != ',') q++;
            if (*q) q++;
        }
    if (NPrint())
    {
        std::printf("plane3: %d post-load mode %d team %d k190 %d speed %.0f boost %.2f brake %.2f hp %d ammo %d guns %zu (res %d launcher %d) region %d "
                    "shape %d avoid %d fade %d target %.0f %.0f %.0f\n",
                    palo->oid, p.mode, p.team, p.k190, p.speed, p.boost, p.brake, HpMax(p), p.ammoMax, p.aguns.size(), p.gunResource ? p.gunResource->oid : -1,
                    p.launcher ? p.launcher->oid : -1, p.region ? p.region->oid : -1, p.shapeFire ? p.shapeFire->oid : -1, p.shapeAvoid ? p.shapeAvoid->oid : -1,
                    p.ploFade ? p.ploFade->oid : -1, p.posTarget.x, p.posTarget.y, p.posTarget.z);
        std::printf("plane3: %d power-up HUD %d lines %d / %d\n", palo->oid, p.ploWpnHud ? p.ploWpnHud->oid : -1, p.cHijackLine, p.cEmptyLine);
        if (NPrint() >= 3)
            for (const OPT& o : palo->aopt)
                std::printf("plane3:   option %d: %zu values, n %d g %.2f basic %p\n", o.eopid, o.aval.size(), o.aval.empty() ? 0 : o.aval[0].n,
                            o.aval.empty() ? 0.0f : o.aval[0].ag[0], o.aval.empty() ? nullptr : (void*)o.aval[0].pbasic);
    }
}

PLANE3* Pplane(LO* plo)
{
    if (!FIsPlane(plo) || !g_psw) return nullptr;
    auto it = s_mpplane.find(plo);
    if (it != s_mpplane.end() && it->second.psw == g_psw) return &it->second;
    PLANE3& p = s_mpplane[plo];
    p.palo = static_cast<ALO*>(plo);
    p.psw = g_psw;
    InitPlane(p);
    return &p;
}

std::vector<PLANE3*> AplaneWorld()
{
    std::vector<PLANE3*> ap;
    if (!g_psw) return ap;
    for (LO* plo : g_psw->mpcidaplo[0x52])
        if (plo->fInWorld)
            if (PLANE3* p = Pplane(plo)) ap.push_back(p);
    return ap;
}

// ---- the weapon (power-up) states 0x36..0x3D (0x141FD8 / 0x1421C0) ----
// orig3 D_00343098 (0x3C-byte entries by state; the power-ups' 0x36..0x3D): the HUD part (+0x28) and per pad row (the
// plane's +0x130: Sly's 0, Carmelita's 1) a run of dialog lines {first, count} with its cursor (+0x2C / +0x34)
struct WPNLINES { int part; int aid[2]; int ac[2]; int ai[2]; };
WPNLINES s_awpnlines[8] = {
    {1974, {344, 372}, {4, 4}, {0, 0}}, {1977, {348, 376}, {4, 4}, {0, 0}}, {1978, {352, 380}, {4, 4}, {0, 0}},
    {1972, {356, 384}, {4, 4}, {0, 0}}, {1979, {360, 388}, {4, 4}, {0, 0}}, {1980, {364, 392}, {4, 4}, {0, 0}},
    {1981, {368, 396}, {4, 4}, {0, 0}}, {0, {0, 0}, {0, 0}, {0, 0}}};
bool FPrintLines() { static const bool s_f = std::getenv("SLY3_PLANELINES") != nullptr || NPrint() > 0; return s_f; }

void SetOptionN(LO* plo, int eopid, int n)
{
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

// orig3 0x142650: the power-up HUD's counts (+0x120 / +0x124: the shots fired / its shots: EOPIDs 1244 / 1245, which its
// text 864..866 shows) and part (the power-up's icon; 0x7AF none), a change of part with the pilot's next line of it
void SelectWeaponHud(PLANE3& p)
{
    if (!p.ploWpnHud) return;
    SetOptionN(p.ploWpnHud, 1244, p.cShots);
    SetOptionN(p.ploWpnHud, 1245, p.cShotsMax);
    if (p.wpn == -1) { p.idWpnPart = 0x7AF; return; }
    WPNLINES* pe = p.wpn >= 0x36 && p.wpn <= 0x3D ? &s_awpnlines[p.wpn - 0x36] : nullptr;
    int part = 0x7AF, id = -1;
    if (pe && unsigned(pe->part + 1) >= 2u) part = pe->part;
    // (the pad row +0x130; PC, the free-for-all: players 3 / 4 fly Sly's / Carmelita's planes and take their pilot's lines)
    const int row = IPlayerOfPlane(p.palo);
    if (pe && row != -1)
    {
        const int k = row & 1;
        if (pe->ac[k])
        {
            id = pe->aid[k] + pe->ai[k];
            pe->ai[k] = (pe->ai[k] + 1) % pe->ac[k];
        }
    }
    if (part != p.idWpnPart)
    {
        SelectScreenObjPartHandle(p.ploWpnHud, part, &p.idWpnPart);
        p.idWpnPart = part;
        if (id != -1)
        {
            PsndStartDialog(id, 1);
            if (FPrintLines()) std::printf("plane3: %d power-up 0x%X: line %d (HUD part %d)\n", p.palo->oid, p.wpn, id, part);
        }
    }
}

// orig3 0x147278: a shuffled run of lines: the first of the list played next unless it plays now; through the list in
// order, reshuffled (the last one played not first again) once every one has played
void PlayShuffledLine(PLANE3& p, int c, int* aid, int* pi)
{
    if (c <= 0) return;
    const int first = aid[0];
    if (FDialogPlaying(first)) return;
    if (*pi == c)
    {
        for (int i = 0; i < c; i++) std::swap(aid[i], aid[NRandInRange(0, c - 1)]);
        if (aid[0] == first) std::swap(aid[0], aid[c - 1]);
        *pi = 0;
    }
    else
    {
        for (int i = 0; i + 1 < c; i++) aid[i] = aid[i + 1];
        aid[c - 1] = first;
        (*pi)++;
    }
    PsndStartDialog(aid[0], 0);
    if (FPrintLines()) std::printf("plane3: %d line %d (shuffled, %d)\n", p.palo->oid, aid[0], c);
}

void SetWeapon(PLANE3& p, int st)
{
    if (st == p.wpn) return;
    // (0x141FD8: the HUD shown while a power-up other than the default is held (vt slot 56), hidden again (slot 54))
    if (p.ploWpnHud)
    {
        auto fCur = [&](int w) { return w == -1 || w == p.wpnDefault; };
        if (fCur(p.wpn) != fCur(st))
        {
            if (fCur(st)) HideScreenObj(p.ploWpnHud);
            else ShowScreenObj(p.ploWpnHud, -1);
        }
    }
    p.wpn = st;
    p.tWpn = g_clock.t;
    p.cShots = 0;
    switch (st)
    {
    case 0x36: p.cShotsMax = 2; break;
    case 0x37: case 0x38: case 0x3B: case 0x3C: p.cShotsMax = 1; if (st == 0x3C) p.tWpnLast = -FLTMAX; break;
    case 0x39: p.cShotsMax = 2; break;
    case 0x3A: p.cShotsMax = 4; break;
    case 0x3D: p.cShotsMax = 3; break;
    default: p.cShotsMax = 0; break;
    }
    SelectWeaponHud(p);
}

// orig3: 0x145080: a power-up's projectile from +0x47C aimed at pos / the object
bool FLaunchWeapon(PLANE3& p, LO* pres, LO* ptarget, const glm::vec3& pos, float g)
{
    if (!p.ploWpnFrom || !pres || !p.launcher) return false;
    LO* po = PloAcquireProxyObject(g_psw, pres, false);
    if (!po || !FIsLoDerivedFrom(po, sly3::CID_ALO)) return false;
    ALO* palo = static_cast<ALO*>(po);
    const glm::mat4 m = MatLo(p.ploWpnFrom);   // 0x1C8FB0: its place
    SetAloRotation(palo, glm::mat3(m));
    SetAloPos(palo, glm::vec3(m[3]));
    SetLauncherAim(p.launcher, ptarget, pos);
    LaunchFrom(p.launcher, po);
    ExpireProjectileIn(palo, g);                 // 0x14CB98
    SetProjectileLauncher(palo, p.palo);         // 0x14D1F0
    return true;
}

void UpdateWeapon(PLANE3& p, const INPUT3& in)
{
    if (p.wpn == -1)
    {
        if (p.wpnDefault == -1) return;
        SetWeapon(p, p.wpnDefault);
    }
    if (p.cShots >= p.cShotsMax && p.wpn != p.wpnDefault) return;
    if (p.state >= 2) return;
    bool fGo = true;
    if (p.wpn == 0x3C && p.tWpnLast > p.tWpn) fGo = false;
    (void)in;
    if (fGo && !GrfTake(p, 4, 0.0f)) return;   // L1 pressed (the input's slot 1 with 4, dt 0; slot 2 takes it)
    bool fOk = false;
    switch (p.wpn)
    {
    case 0x36: fOk = p.awpnRes[0]; break;
    case 0x37: fOk = p.awpnRes[1]; break;
    case 0x38: fOk = p.awpnRes[2]; break;
    case 0x39: fOk = p.psmaShield && OidSmaGoal(p.psmaShield) == 0x1A0; break;
    case 0x3A: fOk = p.awpnRes[3]; break;
    case 0x3B: fOk = p.awpnRes[4]; break;
    case 0x3C: fOk = p.awpnRes[5] && g_clock.t - p.tWpnFire > 0.25f; break;
    case 0x3D: fOk = p.awpnRes[6]; break;
    default: break;
    }
    if (!fOk) return;
    const glm::vec3 posAim = glm::vec3(MatObj(p.palo)[3]) + p.range * FwdOf(p.palo);   // 0x142D20
    LO* pres = nullptr;
    float g = 2.5f;
    switch (p.wpn)
    {
    case 0x36: pres = p.awpnRes[0]; break;
    case 0x37: pres = p.awpnRes[1]; break;
    case 0x38: pres = p.awpnRes[2]; g = 3.0f; break;
    case 0x39: if (p.psmaShield) SetSmaGoal(p.psmaShield, 0x1A1); break;
    case 0x3B: pres = p.awpnRes[4]; g = 3.0f; break;
    case 0x3D: pres = p.awpnRes[6]; g = 3.0f; break;
    default: break;
    }
    if (pres && !FLaunchWeapon(p, pres, p.partner, posAim, g)) return;
    if (p.wpn == 0x3C) p.tWpnFire = g_clock.t;
    if (p.wpn != p.wpnDefault) p.cShots++;
    SelectWeaponHud(p);   // (0x1421C0: the HUD's count)
    if (p.cShots >= p.cShotsMax) SetWeapon(p, p.wpnDefault);   // (0x1421C0: back to the default, -1 none: the HUD hidden)
}

// ---- the maneuvers (+0x2B8: 0x143230; its part +0x2C0: 0x1434A8; 0x143628; 0x143AB8) ----
void SetVelWorld(PLANE3& p, const glm::vec3& v) { SetAloVelocityTo(p.palo, v); }   // vt slot 40 (Sly 2's 35)

void SetSub(PLANE3& p, int st)
{
    if (st == p.sub) return;
    if (p.sub == 2) SetVelWorld(p, p.spd * FwdOf(p.palo));
    p.subPrev = p.sub;
    p.sub = st;
    p.tSub = g_clock.t;
    p.fBlend = 0;
    switch (st)
    {
    case 0:
        p.rateSub = -p.eul.x;
        p.durSub = std::fabs(p.rateSub) / p.rollMax * 0.6f;
        break;
    case 1: p.durSub = std::fabs(p.rateSub) * 0.135281697f; break;
    case 2:
        p.durSub = std::fabs(p.rateSub) * 0.397887349f;
        SetVelWorld(p, glm::vec3(0.0f));
        break;
    default: break;
    }
}

void SetMan(PLANE3& p, int st)
{
    if (st == p.man) return;
    switch (p.man)
    {
    case 1: case 2: SetVelWorld(p, p.spd * FwdOf(p.palo)); SetSub(p, -1); break;
    case 3: case 4: case 5: SetSub(p, -1); break;
    default: break;
    }
    p.man = st;
    p.tMan = g_clock.t;
    p.tTap = FLTMAX;
    switch (st)
    {
    case 1:
    case 2:
    {
        // the barrel roll's side push (2352.94 by the stick's deflection), the roll's turn
        const INPUT3 in = Input(p);
        float g = st == 2 ? -2352.94092f : 2352.94092f;
        const float u = s_lmStick.gMin == s_lmStick.gMax ? 0.0f : (in.u - s_lmStick.gMin) / (s_lmStick.gMax - s_lmStick.gMin);
        g *= cmm::GLimitLm(&s_lmUnit, u);
        const glm::mat3 m = BuildLookAtMatrix(FwdOf(p.palo), glm::vec3(0.0f, 0.0f, 1.0f));
        SetVelWorld(p, VelAloPos(p.palo) + m[1] * g);
        p.rateSub = st == 1 ? -PI2 : PI2;
        SetSub(p, 1);
        break;
    }
    case 3: case 4: case 5: SetSub(p, 0); break;
    default: break;
    }
}

void ExpireSub(PLANE3& p)   // 0x143AB8
{
    int st;
    do
    {
        st = p.sub;
        if (st >= 0 && st < 3 && p.durSub < g_clock.t - p.tSub) SetSub(p, 3);
    } while (st != p.sub);
}

void UpdateMan(PLANE3& p)   // 0x143628
{
    int st, sub;
    do
    {
        st = p.man;
        sub = p.sub;
        switch (st)
        {
        case 0:
        {
            if (Bands().ceiling < PosOf(p.palo).z)
            {
                SetState(p, 3);
                break;
            }
            if (p.hijacker) break;
            if (GrfTake(p, 0x10, 0.25f))   // Triangle: the loop
            {
                SetMan(p, 5);
                break;
            }
            if (GrfTake(p, 0x20, 0.25f)) p.tTap = g_clock.t;   // Circle
            if (p.tTap <= g_clock.t)
            {
                const INPUT3 in = Input(p);
                if (g_clock.t - p.tTap < 0.2f)
                {
                    if (0.1f < in.u && std::fabs(in.y) > std::fabs(in.x)) SetMan(p, 0.0f < in.y ? 4 : 3);
                    else if (s_lmStick.gMin < in.u) SetMan(p, 0.0f < in.x ? 2 : 1);
                }
                else SetMan(p, GRandInRange(0.0f, 1.0f) < 0.5f ? 1 : 2);
            }
            break;
        }
        case 1:
        case 2:
            if (sub == 3) SetMan(p, 0);
            break;
        case 3:
            if (sub != 3) break;
            switch (p.subPrev)
            {
            case 0: p.rateSub = -PI - p.eul.y; SetSub(p, 2); break;
            case 2: p.rateSub = p.eul.x < 0.0f ? -PI : PI; SetSub(p, 1); p.durSub *= 1.5f; break;
            default: SetMan(p, 0); break;
            }
            break;
        case 4:
            if (sub == 3)
            {
                switch (p.subPrev)
                {
                case 0: p.rateSub = p.rateSub < 0.0f ? -PI : PI; SetSub(p, 1); p.durSub *= 1.5f; break;
                case 1: p.rateSub = p.eul.y - PI; SetSub(p, 2); break;
                default: SetMan(p, 0); break;
                }
            }
            else if (sub == 2 && g_clock.t - p.tSub > p.durSub * 0.6f && !p.fBlend)
            {
                p.fBlend = 1;
                p.tBlend = g_clock.t;
            }
            break;
        case 5:
            if (sub == 3)
            {
                if (p.subPrev == 0)
                {
                    p.rateSub = -PI2 - p.eul.y;
                    SetSub(p, 2);
                    p.durSub += p.durSub;
                }
                else SetMan(p, 0);
            }
            else if (sub == 2 && g_clock.t - p.tSub > p.durSub * 0.8f && !p.fBlend)
            {
                p.fBlend = 1;
                p.tBlend = g_clock.t;
            }
            break;
        default: break;
        }
        ExpireSub(p);
    } while (st != p.man || sub != p.sub);
}

// ---- the guns (0x144A90 the decision, 0x144E40 a shot) ----
// 0x167F60: a sphere inside a camera's four side planes, the point taken from the camera's place (its matrix's row 3:
// FInsideCmPlanes wants the offset); the plane-vs-plane rule tries every view's camera (SW +0x3EB8 / +0x3EBC: split3's
// views in split screen, else the world camera)
bool FInsideCm(const CM& cm, const glm::vec3& pos, float sRadius) { return FInsideCmPlanes(&cm, pos - cm.pos, sRadius); }
bool FInsideAnyCm(const glm::vec3& pos, float sRadius)
{
    if (split3::FActive())
    {
        for (int i = 0; i < split3::CView(); i++)
            if (FInsideCm(split3::CmView(i), pos, sRadius)) return true;
        return false;
    }
    return FInsideCm(g_cm, pos, sRadius);
}
// 0x1F4A80: the camera of a player's object (its own view in split screen)
bool FInsidePlayerCm(LO* ploPlayer, const glm::vec3& pos, float sRadius)
{
    if (split3::FActive())
        for (int i = 0; i < split3::CView(); i++)
            if (split3::PloPlayer(g_psw, i) == ploPlayer) return FInsideCm(split3::CmView(i), pos, sRadius);
    return FInsideCm(g_cm, pos, sRadius);
}

bool FInsideShape(LO* pshape, const glm::vec3& pos)   // 0x27F298 (the point in the shape's frame)
{
    return pshape && FPointInShape(pshape, pos);
}

float RadiusOf(ALO* palo)   // ALO +0x90 (the bounding radius)
{
    const OXA* poxa = Poxa(palo);
    return poxa ? poxa->sRadius : 200.0f;
}

bool FCanFire(PLANE3& p)
{
    if (g_clock.t < p.tShot) return false;
    if (!p.mode)
    {
        const bool f = p.ammo > 0 || p.fInfinite;
        if (!f && g_clock.t - p.tClick > 0.5f) p.tClick = g_clock.t;   // (the empty click's sound +0x2A4: not ported)
        return f;
    }
    if (cmm::GLimitLm(&p.lmBurst, g_clock.t) == g_clock.t && p.lmBurst.gMin <= g_clock.t && g_clock.t <= p.lmBurst.gMax) return false;   // FCheckLm
    if (g_clock.t <= p.tFireCheck) return p.fFire;
    while (p.tFireCheck < g_clock.t) p.tFireCheck += 0.25f;
    p.fFire = 0;
    const glm::vec3 pos = PosOf(p.palo);
    for (PLANE3* po : AplaneWorld())
    {
        if (po->team == p.team) continue;
        const glm::vec3 posO = PosOf(po->palo);
        if (!po->mode && p.region && FInsideShape(p.region, posO) && FInsidePlayerCm(po->palo, pos, RadiusOf(p.palo)))
        {
            p.igun = 1;
            p.fFire = 1;
            break;
        }
        if (p.shapeFire && FInsideShape(p.shapeFire, posO) && po->mode)
        {
            if (FInsideAnyCm(pos, RadiusOf(p.palo)))
            {
                p.igun = 0;
                p.fFire = 1;
            }
            break;
        }
    }
    return p.fFire;
}

float g_dfMiss = -1.0f;   // (debug: the harness's last predicted miss / geometry hit, printed with the shots)
int g_fDfGeom = 0;
float g_dfT = 0.0f;
void Shoot(PLANE3& p)   // 0x144E40
{
    if (!FCanFire(p)) return;
    if (p.gunResource && p.launcher && !p.aguns.empty())
    {
        PLANEGUN& g = p.aguns[std::min<size_t>(p.igun, p.aguns.size() - 1)];
        if (g.muzzle)
            if (LO* po = PloAcquireProxyObject(g_psw, p.gunResource, false); po && FIsLoDerivedFrom(po, sly3::CID_ALO))
            {
                ALO* palo = static_cast<ALO*>(po);
                const glm::mat4 m = MatLo(g.muzzle);   // 0x1C8FB0
                SetAloRotation(palo, glm::mat3(m));
                SetAloPos(palo, glm::vec3(m[3]));
                const bool fAhead = glm::dot(glm::vec3(m[0]), FwdOf(p.palo)) > 0.0f;
                const glm::vec3 posAim = PosOf(p.palo) + (fAhead ? p.range : -p.range) * FwdOf(p.palo);   // 0x142D20 / 0x142D98
                SetLauncherAim(p.launcher, nullptr, posAim);
                LaunchFrom(p.launcher, po);   // 0x212770
                if (FIsLoDerivedFrom(po, 0x59)) SetProjectileLauncher(palo, p.palo);   // 0x14D1F0
                if (g.flash) TriggerPe(g.flash);
                if (!p.mode && !p.fInfinite) SetAmmo(p, p.ammo - 1);
                p.igun = (p.igun + 1) % static_cast<int>(p.aguns.size());
                if (NPrint() >= 2)
                {
                    const glm::vec3 vShot = VelAloPos(palo);
                    std::printf("plane3: %d shot %d from %d to %.0f %.0f %.0f (speed %.0f, plane's %.0f) t %.3f miss %.0f geom %d tf %.3f obj %p\n", p.palo->oid, po->oid, g.muzzle->oid, posAim.x, posAim.y,
                                posAim.z, glm::length(vShot), glm::length(VelAloPos(p.palo)), g_clock.t, p.mode ? -1.0f : g_dfMiss, p.mode ? -1 : g_fDfGeom, g_dfT, (void*)po);
                }
            }
    }
    p.tShot = g_clock.t + 1.0f / std::max(p.rate, 0.01f);
    if (p.mode && p.lmBurst.gMin < g_clock.t)
    {
        p.lmBurst.gMin = g_clock.t + GRandInRange(0.5f, 1.5f);
        p.lmBurst.gMax = p.lmBurst.gMin + GRandInRange(1.5f, 2.0f);
    }
}

// ---- the arena (0x1469B0 out of range, 0x146A80 its middle, 0x146AD0 steer back, 0x146B80 put back) ----
bool FOutOfRange(PLANE3& p)
{
    const glm::vec3 pos = PosOf(p.palo);
    switch (p.mode)
    {
    case 0: return std::sqrt(pos.x * pos.x + pos.y * pos.y) > Bands().rangePlayer;
    case 1: return glm::length(pos) > Bands().rangeAi;
    default: return false;
    }
}
glm::vec3 PosMiddle()
{
    const BANDS3& b = Bands();
    return {0.0f, 0.0f, (b.lmHigh.gMin - b.lmLow.gMax) * 0.5f + b.lmLow.gMax};
}
void SteerBack(PLANE3& p)   // 0x146AD0
{
    float pan, tilt;
    cmm::ConvertVectorToSphere(PosMiddle() - PosOf(p.palo), &pan, &tilt, nullptr);
    p.pitch = cmm::GLimitAbs(cmm::RadNormalize(-tilt), p.pitchMax);
    p.yaw = cmm::RadNormalize(pan);
    p.roll = cmm::GLimitAbs(cmm::RadNormalize(p.eul.z - p.yaw), p.rollMax);
    p.spd = SpeedBoost(p);
}
void PutBack(PLANE3& p)   // 0x146B80 (no 0x78B path in h_dogfight: the ring of range - 1000)
{
    const glm::vec3 posMid = PosMiddle();
    glm::vec3 d = posMid - PosOf(p.palo);
    d.z = 0.0f;
    d = cmm::LimitVectorLength(d, Bands().rangePlayer - 1000.0f);
    const glm::vec3 pos = posMid - d;
    SetAloPos(p.palo, pos);
    glm::mat3 m = BuildLookAtMatrixZ(posMid - pos);
    LevelMatrix3(m, &m);
    SetAloRotation(p.palo, m);
    p.eul = cmm::DecomposeEulerMatrix(glm::mat3(MatObj(p.palo)));
}

// ---- the ceiling (state 3: 0x143B30 / 0x143D08) ----
bool FTargetsReached(PLANE3& p)   // 0x1468A8
{
    if (std::fabs(p.spd - glm::length(VelAloPos(p.palo))) > 5.0f) return false;
    if (std::fabs(cmm::RadNormalize(p.roll - p.eul.x)) > 0.1f) return false;
    if (std::fabs(cmm::RadNormalize(p.pitch - p.eul.y)) > 0.1f) return false;
    if (std::fabs(cmm::RadNormalize(p.yaw - p.eul.z)) > 0.1f) return false;
    return true;
}
void SetSt3(PLANE3& p, int st)
{
    if (st == p.st3) return;
    if (p.st3 == 1) RetractAsegaPp(&p.pasegaStall);
    p.st3 = st;
    p.tSt3 = g_clock.t;
    switch (st)
    {
    case 0:
        p.spd = 0.0f;
        p.roll = 0.0f;
        p.pitch = -p.pitchMax * 0.7f;
        break;
    case 1:
        if (p.asegStall) ApplyAsegCur(p.asegStall, p.palo, 0.0f, 1.0f, 1, &p.pasegaStall);
        break;
    case 2:
    {
        p.spd = SpeedBoost(p);
        p.pitch = p.pitchMax * 0.6f;
        glm::mat3 m;
        LevelMatrix3(glm::mat3(MatObj(p.palo)), &m);
        const glm::vec3 pos = PosOf(p.palo) + m[0] * 2000.0f;
        if (Bands().rangePlayer < std::sqrt(pos.x * pos.x + pos.y * pos.y))
        {
            p.yaw = cmm::RadNormalize(p.eul.z - PI);
            p.roll = p.rollMax;
        }
        break;
    }
    default: break;
    }
}
void UpdateSt3(PLANE3& p)
{
    int st;
    do
    {
        st = p.st3;
        switch (st)
        {
        case 0: if (g_clock.t - p.tSt3 > 0.5f || FTargetsReached(p)) SetSt3(p, 1); break;
        case 1:
            if (p.asegStall) { if (!p.pasegaStall || FAsegaRetracted(p.pasegaStall)) SetSt3(p, 2); }
            else if (g_clock.t - p.tSt3 > 1.0f) SetSt3(p, 2);
            break;
        case 2: if (PosOf(p.palo).z < Bands().ceiling) SetState(p, 1); break;
        default: break;
        }
    } while (st != p.st3);
}

// ---- the crash avoidance (state 4, 0x146558): the clear direction nearest the nose among 85 probes of 1500 ----
bool FContact(PLANE3& p)   // 0x23E130
{
    const OXA* poxa = Poxa(p.palo);
    for (const OX* pox = poxa ? poxa->pox : nullptr; pox; pox = pox->poxNext)
        if (pox->pxp) return true;
    return false;
}
void Avoid(PLANE3& p)
{
    glm::mat3 m;
    LevelMatrix3(glm::mat3(MatObj(p.palo)), &m);
    const glm::vec3 dir = m[0];
    glm::vec3 best = -dir;
    float gBest = glm::dot(dir, best);
    static const float s_atilt[9] = {-1.2f, -0.9f, -0.6f, -0.3f, 0.0f, 0.3f, 0.6f, 0.9f, 1.2f};
    static const int s_cpan[9] = {7, 9, 10, 11, 12, 11, 10, 9, 7};
    // (natively the probes are not tested against the world: the nearest to the nose, i.e. straight on, level)
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < s_cpan[i]; j++)
        {
            const glm::vec3 d = cmm::SetVectorSphere(j * PI2 / s_cpan[i], s_atilt[i], 1500.0f);
            const glm::vec3 n = glm::normalize(d);
            const float g = glm::dot(dir, n);
            if (gBest < g)
            {
                gBest = g;
                best = n;
            }
        }
    cmm::ConvertVectorToSphere(best, &p.yaw, &p.pitch, nullptr);
    p.pitch = cmm::GLimitAbs(p.pitch, p.pitchMax);
    p.spd = p.escape;
    const float d = std::clamp(cmm::RadNormalize(p.eul.z - p.yaw), -PI, PI);
    p.roll = d / PI * p.rollMax;
    if (FContact(p)) SetVelWorld(p, -p.crashPush * FwdOf(p.palo));
}

// ---- the main state (+0x2B0, 0x142AE0) ----
void SetState(PLANE3& p, int st)
{
    const int old = p.state;
    if (st == old) return;
    switch (old)
    {
    case 1:
        p.fBraking = 0;
        p.fBoosting = 0;
        SetMan(p, 0);
        break;
    case 3: SetSt3(p, -1); break;
    default: break;
    }
    p.state = st;
    p.tState = g_clock.t;
    if (NPrint()) std::printf("plane3: %d state %d -> %d (t %.3f hp %d of %d, team %d, takes rams %d)\n", p.palo->oid, old, st, g_clock.t, Hp(p), HpMax(p), p.team, p.fRamProof);
    switch (st)
    {
    case 2:   // out of range: faded and put back
        RetractAsegaPp(&p.pasegaPilot);
        Broadcast3(0x54, p.palo);
        if (p.ploFade) StartFadeLo(p.ploFade);
        SetVelWorld(p, glm::vec3(0.0f));
        SetAloAngVelocityTo(p.palo, glm::vec3(0.0f));
        break;
    case 3: SetSt3(p, 0); break;
    case 1: p.spd = p.speed; break;
    case 4:
        if (!p.mode && p.fFadeHit)
        {
            if (p.fadeHit) StartFadeLo(p.fadeHit);
            SetVelWorld(p, glm::vec3(0.0f));
            SetAloAngVelocityTo(p.palo, glm::vec3(0.0f));
        }
        else Avoid(p);
        break;
    case 5:   // dead
        if (p.sndEngine) { p.sndEngine->Stop(0.0f); p.sndEngine = CSndRef(); }   // (out of the world: 0x146360 not run)
        if (p.peDeath) TriggerPe(p.peDeath);
        Remove(p.palo);
        if (!p.mode) Broadcast3(0xC, p.palo);
        else if (p.k190 == 1) Broadcast3(0x53, p.palo);
        break;
    default: break;
    }
}

void UpdateCeilingWarning(PLANE3& p);
// ---- the pad (0x142E18) ----
void UpdatePlayer(PLANE3& p)
{
    UpdateMan(p);
    const int fPrevBoost = p.fBoosting;
    p.fBraking = 0;
    p.fBoosting = 0;
    const INPUT3 in = Input(p);
    if (p.man >= 0 && p.man < 6)
    {
        const bool f = p.man == 0 || p.fBlend;
        if (f && !p.hijacker && (in.grf & 0x40))
        {
            p.spd = SpeedBoost(p);
            p.fBoosting = p.fBoostOk;
            if (!fPrevBoost && p.fBoosting) p.tBoost = g_clock.t;
        }
        else if (f && !p.hijacker && (in.grf & 0x80))
        {
            p.spd = SpeedBrake(p);
            p.fBraking = p.fBrakeOk;
        }
        else p.spd = p.speed;
        float u;
        if (p.fBlend) u = cmm::GLimitLm(&s_lmUnit, (g_clock.t - p.tBlend) / (p.tSub + p.durSub - p.tBlend));
        else u = f ? 1.0f : 0.0f;
        float gx, gy;
        PLANE3* ph = p.hijacker ? Pplane(p.hijacker) : nullptr;
        if (ph && Ppad(*ph))
        {
            const INPUT3 inH = Input(*ph);
            gx = inH.x2 * u;
            gy = inH.y2 * u;
        }
        else
        {
            gx = in.x * u;
            gy = in.y * u;
        }
        if (std::fabs(gx) < 0.2f) gx = 0.0f;
        if (std::fabs(gy) < 0.2f) gy = 0.0f;
        p.roll = gx * p.rollMax;
        p.yaw = cmm::RadNormalize(p.eul.z + -gx * p.yawRate);
        float g = cmm::GLimitAbs(cmm::RadNormalize(p.eul.y + gy * p.pitchRate), p.pitchMax);
        // the maneuver's part: a roll (0 back to level, 1 the barrel roll), the loop's pitch (2)
        const float half = p.durSub * 0.5f;
        float dt = g_clock.t - p.tSub;
        float a = half > 0.0f ? p.rateSub / (half * half) : 0.0f;
        float v0 = 0.0f;
        if (dt >= half)
        {
            v0 = a * half;
            dt -= half;
            a = -a;
        }
        const float x2 = v0 + a * dt;
        switch (p.sub)
        {
        case 0:
        case 1: SetAloAngVelocity(p.palo, glm::vec3(x2, 0.0f, 0.0f)); break;   // 0x23D9D0 (object space)
        case 2:
            if (p.fBlend)
            {
                const float gA = p.eul.y + x2 * g_clock.dt;
                g = (1.0f - u) * gA + u * cmm::RadSmooth(p.eul.y, g, g_clock.dt, &p.asmp[1], nullptr);
            }
            else
            {
                SetAloAngVelocity(p.palo, glm::vec3(0.0f, x2, 0.0f));
                SetAloVelocity(p.palo, glm::vec3(p.spd, 0.0f, 0.0f));   // 0x23D938 (object space)
            }
            break;
        default: break;
        }
        p.pitch = g;
    }
    // (0x146D08: the ceiling's warning sound and rumble: not ported)
    UpdateCeilingWarning(p);   // 0x146D08
    if (in.grf & 8)   // R1: the guns
    {
        const bool fAmmo = p.ammo > 0 || p.fInfinite;
        Shoot(p);
        ASEG* pa = p.asegGun[fAmmo ? 0 : 1];
        if (pa && p.pilot && (!p.pasegaGun || p.pasegaGun->paseg != pa)) ApplyAsegCur(pa, p.pilot, 0.0f, 1.0f, 9, &p.pasegaGun);
    }
    else
    {
        p.tShot = g_clock.t;
        RetractAsegaPp(&p.pasegaGun);
    }
    UpdateWeapon(p, in);
}

// ---- the computer (0x143E50 mode 1, 0x144580 mode 2) ----
void PickTarget(PLANE3& p)   // 0x1448B0
{
    float best = FLTMAX;
    PLANE3* pick = nullptr;
    const glm::vec3 pos = PosOf(p.palo);
    for (PLANE3* po : AplaneWorld())
    {
        if (po->team == p.team) continue;
        float pan, s;
        cmm::ConvertVectorToSphere(PosOf(po->palo) - pos, &pan, nullptr, &s);
        const float e = std::fabs(cmm::RadNormalize(pan - p.eul.z));
        if (p.yawRate < e)
        {
            const float t = (e - p.yawRate) / (PI - p.yawRate);
            s += (1.0f - t) * 500.0f + t * 3000.0f;
        }
        if (po == p.ptarget) s += -1000.0f;
        if (!po->mode) s += -750.0f;
        if (s < best)
        {
            best = s;
            pick = po;
        }
    }
    p.ptarget = pick;
}

void SetAi(PLANE3& p, int st) { if (st != p.ai) p.ai = st; }   // 0x144628

void UpdateChase(PLANE3& p)   // 0x144640
{
    PLANE3* best = nullptr;
    float bestd = FLTMAX;
    const glm::vec3 pos = PosOf(p.palo);
    for (PLANE3* po : AplaneWorld())
    {
        if (p.ptarget == po || po->team == p.team) continue;
        if (po->ptarget != &p && po->mode) continue;
        if (p.region && !FInsideShape(p.region, PosOf(po->palo))) continue;
        float d = glm::length(PosOf(po->palo) - pos);
        if (po == p.pchaser) d += -500.0f;
        if (d < bestd)
        {
            bestd = d;
            best = po;
        }
    }
    if (best)
    {
        if (!p.pchaser) p.tChaser = g_clock.t;
        p.pchaser = best;
    }
    else if (g_clock.t - p.tChaser > 2.5f) p.pchaser = nullptr;
    int st;
    do
    {
        st = p.ai;
        switch (st)
        {
        case 0: if (p.ptarget) SetAi(p, 1); break;
        case 1: if (p.pchaser && g_clock.t - p.tChaser > 1.0f) SetAi(p, 2); break;
        case 2: if (!p.pchaser) SetAi(p, p.ptarget ? 1 : 0); break;
        default: break;
        }
    } while (st != p.ai);
}

void UpdateAi(PLANE3& p)
{
    if (g_clock.t > p.tPick)
    {
        while (p.tPick < g_clock.t) p.tPick += 0.5f;
        PickTarget(p);
    }
    if (g_clock.t > p.tAiState)
    {
        while (p.tAiState < g_clock.t) p.tAiState += 0.25f;
        UpdateChase(p);
    }
    const glm::vec3 pos = PosOf(p.palo);
    switch (p.ai)
    {
    case 0: p.spd = p.speed; break;
    case 1:
        if (p.ptarget)
        {
            if (p.tAim <= g_clock.t)
            {
                const bool fPlayer = !p.ptarget->mode;
                const float dt = fPlayer ? GRandInRange(0.25f, 0.5f) : GRandInRange(0.25f, 1.5f);
                p.posTarget = PredictProjectileAim(p.ptarget->palo, glm::vec3(fPlayer ? 4500.0f : -1500.0f, 0.0f, 0.0f), dt);   // 0x28F680
                p.tAim = g_clock.t + dt;
            }
            float pan, tilt, s;
            cmm::ConvertVectorToSphere(p.posTarget - pos, &pan, &tilt, &s);
            p.pitch = cmm::GLimitAbs(cmm::RadNormalize(-tilt), p.pitchMax);
            p.yaw = cmm::RadNormalize(pan);
            const float u = cmm::GLimitLm(&s_lmUnit, s * 0.0001f);
            p.spd = (1.0f - u) * p.speed + u * SpeedBoost(p);
        }
        else SetAi(p, 0);
        break;
    case 2:
        if (p.tWander < g_clock.t)
        {
            p.pitch = 0.0f;
            const int n = NRandInRange(0, 8);
            const bool f = NRandInRange(0, 3) == 0;
            if (n % 3 == 0) p.yaw = cmm::RadNormalize(p.yaw + p.yawRate * 0.6f);
            else if (n % 3 == 2) p.yaw = cmm::RadNormalize(p.yaw - p.yawRate * 0.6f);
            if (n < 3) p.pitch = -p.pitchMax;
            else if (n >= 6) p.pitch = p.pitchMax;
            p.spd = GRandInRange(SpeedBrake(p), SpeedBoost(p));
            p.tWander = g_clock.t + (f ? 1.0f : 2.0f);
        }
        break;
    default: break;
    }
    if (p.tAvoid < g_clock.t || p.fAvoid)
    {
        while (g_clock.t > p.tAvoid) p.tAvoid += 0.25f;
        p.fAvoid = 0;
        if (p.shapeAvoid)
            for (PLANE3* po : AplaneWorld())
            {
                if (po == &p) continue;
                // (0x27F298 with the avoid shape's inverse matrix: the other plane in the shape carried by this one)
                const glm::mat4 mInv = glm::inverse(MatLo(p.shapeAvoid));
                const glm::vec3 posLocal = glm::vec3(mInv * glm::vec4(PosOf(po->palo), 1.0f));
                if (FPointInShape(p.shapeAvoid, glm::vec3(MatLo(p.shapeAvoid) * glm::vec4(posLocal, 1.0f))))
                {
                    p.pitch = pos.z < PosOf(po->palo).z ? p.pitchMax : -p.pitchMax;
                    p.fAvoid = 1;
                    if (NPrint() >= 3) std::printf("plane3: %d avoids %d (distance %.0f)\n", p.palo->oid, po->palo->oid, glm::length(PosOf(po->palo) - pos));
                    break;
                }
            }
    }
    const BANDS3& b = Bands();
    if (FOutOfRange(p)) SteerBack(p);
    else if (pos.z < b.lmLow.gMax)
    {
        const float u = cmm::GLimitLm(&s_lmUnit, b.lmLow.gMin == b.lmLow.gMax ? 0.0f : (pos.z - b.lmLow.gMin) / (b.lmLow.gMax - b.lmLow.gMin));
        p.pitch = std::min(p.pitch, u * p.pitchMax);
    }
    else if (b.lmHigh.gMin < pos.z && !p.fAvoid)
    {
        const float u = cmm::GLimitLm(&s_lmUnit, b.lmHigh.gMin == b.lmHigh.gMax ? 0.0f : (pos.z - b.lmHigh.gMin) / (b.lmHigh.gMax - b.lmHigh.gMin));
        p.pitch = std::max(p.pitch, (1.0f - u) * -p.pitchMax);
    }
    p.roll = cmm::GLimitAbs(cmm::RadNormalize(p.eul.z - p.yaw), p.rollMax);
    Shoot(p);
}

void UpdateToPoint(PLANE3& p)   // 0x144580
{
    float pan, tilt;
    cmm::ConvertVectorToSphere(p.posTarget - PosOf(p.palo), &pan, &tilt, nullptr);
    p.pitch = cmm::GLimitAbs(cmm::RadNormalize(-tilt), p.pitchMax);
    p.yaw = cmm::RadNormalize(pan);
    p.roll = cmm::GLimitAbs(cmm::RadNormalize(p.eul.z - p.yaw), p.rollMax);
    Shoot(p);
}

// ---- the reset (vt slot 31 0x1414D0 -> 0x142848) ----
void ResetPlane(PLANE3& p)
{
    SetHp(p, HpMax(p));
    SetAmmo(p, p.ammoMax);
    p.wpn = -1;
    SetWeapon(p, p.wpnDefault);
    const float u = Bands().cPlanes ? static_cast<float>(p.serial) / static_cast<float>(Bands().cPlanes) : 0.0f;
    p.tPick = g_clock.t + u * 0.5f;
    p.tAiState = g_clock.t + u * 0.25f;
    p.tAvoid = g_clock.t + u * 0.25f;
    p.fAvoid = 0;
    p.tFireCheck = g_clock.t + u * 0.25f;
    p.fFire = 0;
    for (LO*& plo : p.aploImpact) Remove(plo);
    for (int i = 0; i < 3; i++)
        if (p.asegSurf[i]) ApplyAsegCur(p.asegSurf[i], p.palo, 0.0f, 0.0f, 1, &p.pasegaSurf[i]);
    p.state = 0;
    SetState(p, 1);
    SetAi(p, 0);
    SetMan(p, 0);
    SetSub(p, -1);
    p.tracked = nullptr;
    p.tTracked = -FLTMAX;
    for (LO* plo : p.aploIn) Remove(plo);
    for (LO* plo : p.aploOut) Remove(plo);
    if (p.psmaShield) SnapSmaGoal(p.psmaShield, 0x1A0);
    p.hijacker = nullptr;
    if (p.peHijack) Remove(p.peHijack);
    p.tPeHit = FLTMAX;
    if (p.peHit) Remove(p.peHit);
    p.fReset = true;
}

// ---- the contacts of the frame (0x1451C8, first half): rams between planes, crashes into the world ----
void SendHit(LO* ploFrom, PLANE3& p, int amount, int fKill, const glm::vec3& pos, const glm::vec3& vec)   // 0x240CB0
{
    CTMSG m;
    m.ploSender = ploFrom;
    m.ploReceiver = p.palo;
    m.grfFilter = 0x200;
    m.nState = amount;
    m.nKill3 = fKill;
    m.pos = pos;
    m.vec = vec;
    PaloAcceptContact(&m);
}

void HitQuery(PLANE3& p, PLANE3& o, int* pfKill, int* pamount)   // 0x1459D0
{
    *pfKill = 0;
    *pamount = 0;
    if (!p.mode)
    {
        if (o.psmaShield && OidSmaGoal(o.psmaShield) != 0x1A0)
        {
            *pfKill = 1;
            return;
        }
        // (D_0036DBE0 == 2: the two-player mode's rule: the weaker plane dies, else loses all but one)
        *pamount = p.bumpDamage;
    }
    else if (p.k190 == 1 && !p.fRamProof) *pfKill = 1;
}

void UpdateContacts(PLANE3& p)
{
    OXA* poxa = Poxa(p.palo);
    bool fHit = false, fPush = false;
    glm::vec3 posHit(0.0f);
    for (OX* pox = poxa ? poxa->pox : nullptr; pox && !fHit; pox = pox->poxNext)
        for (XP* pxp = pox->pxp; pxp; pxp = pxp->pxpNext)
        {
            const int iSelf = pxp->axpd[0].paloRoot == p.palo ? 0 : 1;
            const XPD& self = pxp->axpd[iSelf];
            const XPD& other = pxp->axpd[1 - iSelf];
            ALO* po = other.paloRoot ? other.paloRoot : pox->psoOther;
            if (!po || FIsLoDerivedFrom(po, 0x59)) continue;
            const float l = glm::length(self.vel);
            const bool fHead = l > 0.0f && glm::dot(self.vel / l, pxp->normal) < -0.7f;
            fHit = true;
            posHit = self.pos;
            int fKill = 0, amount = 0;
            if (PLANE3* pp = FIsPlane(po) ? Pplane(po) : nullptr)
            {
                fPush = true;
                if (NPrint() && &p < pp)
                    std::printf("plane3: %d / %d contact: distance %.0f gDist %.1f xpk %d head %d (t %.3f)\n", p.palo->oid, pp->palo->oid,
                                glm::length(PosOf(p.palo) - PosOf(pp->palo)), pxp->gDist, pxp->xpk, fHead, g_clock.t);
                if (&p < pp)
                {
                    int fKillO = 0, amountO = 0;
                    HitQuery(p, *pp, &fKill, &amount);
                    HitQuery(*pp, p, &fKillO, &amountO);
                    const bool fFromP = fKill == 0 && fKillO == 0;
                    if (fKillO || amountO > 0)
                    {
                        pp->ploRam = p.palo;
                        SendHit(fFromP ? static_cast<LO*>(pp->palo) : p.palo, *pp, amountO, fKillO, other.pos, self.vel);
                        pp->ploRam = nullptr;
                    }
                    fKill = amount = 0;
                    HitQuery(p, *pp, &fKill, &amount);
                }
                else fKill = amount = 0;
            }
            else if (!p.mode)
            {
                if (fHead)
                {
                    if (p.fFadeHit) amount = p.ramDamage;
                    else fKill = 1;
                }
            }
            else if (p.k190 == 1)
            {
                if (p.fRamProof) { if (fHead && p.state != 4) amount = p.ramDamage; }
                else fKill = 1;
            }
            if (fKill || amount > 0)
            {
                p.ploRam = po;
                SendHit(fHead ? static_cast<LO*>(p.palo) : po, p, amount, fKill, other.pos, self.vel);
                p.ploRam = nullptr;
                break;
            }
        }
    if (!fHit || p.mode) return;
    if (glm::length(VelAloPos(p.palo)) < 750.0f && !fPush)   // stopped against the world: the player's crash
    {
        if (p.fFadeHit) SendHit(p.palo, p, p.ramDamage, 0, posHit, glm::vec3(0.0f));
        else SendHit(p.palo, p, 0, 1, glm::vec3(0.0f), glm::vec3(0.0f));
    }
    else if (p.palo->pcls && 0.2f < g_clock.t - p.tHit)
    {
        TriggerJoyRumbleRumk(1, 0.2f);
        p.tHit = g_clock.t;
    }
}

// ---- the update (vt slot 18, 0x141500) ----
void UpdatePlaneState(PLANE3& p)   // 0x141580
{
    // 0x142750: the hijack and the hit effect time out
    if (p.hijacker)
    {
        PLANE3* ph = Pplane(p.hijacker);
        if (!ph || !FInWorld(p.hijacker) || ph->state != 1 || g_clock.t - p.tHijack > 6.0f)
        {
            p.hijacker = nullptr;
            if (p.peHijack) Remove(p.peHijack);
        }
    }
    if (p.peHit && g_clock.t - p.tPeHit > 1.5f)
    {
        Remove(p.peHit);
        p.tPeHit = FLTMAX;
    }
    int st;
    do
    {
        st = p.state;
        switch (st)
        {
        case 1:
            switch (p.mode)
            {
            case 0:
                if (!FOutOfRange(p)) UpdatePlayer(p);
                else SetState(p, 2);
                break;
            case 1: UpdateAi(p); break;
            case 2: UpdateToPoint(p); break;
            default: break;
            }
            break;
        case 2:
            // (the fade +0x42C: while it plays, wait; natively its time is not read: 1 s)
            if (!p.ploFade || g_clock.t - p.tState > 1.0f)
            {
                PutBack(p);
                SetState(p, 1);
            }
            break;
        case 3:
            if (FOutOfRange(p)) SetState(p, 2);
            else UpdateSt3(p);
            break;
        case 4:
            if (!p.mode && p.fFadeHit)
            {
                if (g_clock.t - p.tState > 1.0f) SetState(p, 1);
            }
            else if (g_clock.t - p.tState > 5.0f)
            {
                SetState(p, -1);
                SetState(p, 4);
            }
            else if (!FContact(p) && FTargetsReached(p)) SetState(p, 1);
            break;
        default: break;
        }
    } while (st != p.state);
}

void UpdateLooks(PLANE3& p)   // 0x145AC0 (the propeller, the control surfaces), 0x145E40, 0x145ED8, 0x146058's clips
{
    if (p.prop && FIsLoDerivedFrom(p.prop, sly3::CID_ALO))
    {
        const float t = glm::length(VelAloPos(p.palo)) / std::max(SpeedBoost(p), 1.0f);
        SetAloAngVelocity(static_cast<ALO*>(p.prop), glm::vec3((1.0f - t) * 0.0f + t * 31.415928f, 0.0f, 0.0f));   // 0x23D9D0
    }
    auto pulse = [&]() {
        float g = cmm::GLimitLm(&s_lmUnit, p.durSub > 0.0f ? (g_clock.t - p.tSub) / p.durSub : 0.0f);
        g = std::fabs(g - 0.5f);
        return 0.0f < p.rateSub ? cmm::GLimitLm(&s_lmUnit, g + 1.0f) : cmm::GLimitLm(&s_lmUnit, g);
    };
    auto apply = [&](int i, float g) {
        if (p.pasegaSurf[i] && p.pasegaSurf[i]->paseg) SetAsegaTime(p.pasegaSurf[i], 0, g * PasegdOf(p.pasegaSurf[i]->paseg)->tMax, 0.0f);
    };
    if (p.pasegaSurf[0])
    {
        float g;
        if (p.sub == 1 || p.sub == 2) g = 0.5f;
        else g = cmm::GLimitLm(&s_lmUnit, (cmm::GLimitLm(&s_lmSigned, cmm::RadNormalize(p.yaw - p.eul.z) / p.yawRate) + 1.0f) * 0.5f);
        apply(0, g);
    }
    if (p.pasegaSurf[1])
    {
        float g;
        if (p.sub == 2) g = pulse();
        else if (p.sub == 1) g = 0.5f;
        else g = cmm::GLimitLm(&s_lmUnit, (cmm::GLimitLm(&s_lmSigned, cmm::RadNormalize(p.pitch - p.eul.y) / p.pitchRate) + 1.0f) * 0.5f);
        apply(1, g);
    }
    if (p.pasegaSurf[2])
    {
        float g;
        if (p.sub == 1) g = pulse();
        else if (p.sub == 2) g = 0.5f;
        else g = cmm::GLimitLm(&s_lmUnit, (cmm::GLimitLm(&s_lmSigned, p.roll / p.rollMax) + 1.0f) * 0.5f);
        apply(2, g);
    }
    for (LO* plo : p.aploBoost)
        if (plo) { if (p.fBoosting) Add(plo); else Remove(plo); }
    for (int i = 0; i < 4; i++)
        if (p.aploImpact[i] && FInWorld(p.aploImpact[i]) && g_clock.t - p.atImpact[i] > 3.0f) Remove(p.aploImpact[i]);
    // the pilot's clips (level / roll / pitch / dead), their time by the stick
    if (p.pilot)
    {
        ASEG* pcur = p.pasegaPilot ? p.pasegaPilot->paseg : nullptr;
        const float gx = std::fabs(p.eul.x), gy = std::fabs(p.eul.y);
        ASEG* pnew;
        int grf = 1;
        if (p.state == 3) { pnew = p.aaseg[3]; grf = 9; }
        else if (gx < 0.2f && gy < 0.2f) { pnew = p.aaseg[0]; grf = 9; }
        else if (gx < gy) pnew = p.aaseg[2];
        else pnew = p.aaseg[1];
        if (pnew && pnew != pcur) ApplyAsegCur(pnew, p.pilot, 0.0f, 1.0f, grf, &p.pasegaPilot);
        if (p.pasegaPilot && p.pasegaPilot->paseg)
        {
            float u = -2.0f;
            if (p.pasegaPilot->paseg == p.aaseg[2]) u = cmm::GLimitLm(&s_lmSigned, p.eul.y / p.pitchMax);
            else if (p.pasegaPilot->paseg == p.aaseg[1]) u = cmm::GLimitLm(&s_lmSigned, p.eul.x / p.rollMax);
            if (u > -2.0f) SetAsegaTime(p.pasegaPilot, 0, cmm::GLimitLm(&s_lmUnit, (u + 1.0f) * 0.5f) * PasegdOf(p.pasegaPilot->paseg)->tMax, 0.0f);
        }
    }
}

void SyncCamera(PLANE3& p);

// orig3 0x146360: the engine's loop for the state: flying (1) by the maneuver (+0x2B8 1..4: +0x3F0), the boost (+0x410:
// +0x3D8 its first 1.5 s, then +0x3DC), the brake (+0x418: +0x3E0), else level (+0x3D4: |roll| < 0.5, |pitch| < 0.2),
// banked (+0x3E4) or climbing / diving (+0x3E8 / +0x3EC: pitch under / over 0); the ceiling's stall (3: +0x3E8 at its
// stage 0, +0x3EC at 2); hit (4: +0x3F4). A change stops the old loop (0x244CA8(0)) and starts the new one from the
// plane (0x2459F8 / 0x244F08, kept 0x2BBCD8)
void UpdateEngineSound(PLANE3& p)
{
    if (static const bool s_fOff = std::getenv("SLY3_PLANENOSND") != nullptr; s_fOff) return;
    int want = -1;
    switch (p.state)
    {
    case 1:
        if (p.man >= 1 && p.man <= 4) want = p.asndEngine[7];
        else if (p.fBoosting) want = g_clock.t - p.tBoost < 1.5f ? p.asndEngine[1] : p.asndEngine[2];
        else if (p.fBraking) want = p.asndEngine[3];
        else
        {
            const float ax = std::fabs(p.eul.x), ay = std::fabs(p.eul.y);
            if (ax < 0.5f && ay < 0.2f) want = p.asndEngine[0];
            else if (ax - 0.5f < ay - 0.2f) want = p.eul.y < 0.0f ? p.asndEngine[5] : p.asndEngine[6];
            else want = p.asndEngine[4];
        }
        break;
    case 3:
        if (p.st3 == 0) want = p.asndEngine[5];
        else if (p.st3 == 2) want = p.asndEngine[6];
        break;
    case 4: want = p.asndEngine[8]; break;
    default: break;
    }
    if (!FInWorld(p.palo)) want = -1;
    const int cur = p.sndEngine ? p.sndEngineId : -1;
    if (want == cur) return;
    if (p.sndEngine)
    {
        p.sndEngine->Stop(0.0f);
        p.sndEngine = CSndRef();
    }
    if (want != -1)
        if (CSndPlain* psnd = PsndNewPlain(want))
        {
            psnd->SetSource(p.palo);
            p.sndEngine = CSndRef(psnd);
            p.sndEngineId = want;
        }
    if (NPrint() >= 2) std::printf("plane3: %d engine sound %d -> %d (state %d man %d)\n", p.palo->oid, cur, want, p.state, p.man);
}

// orig3 0x146D08 (the pad 0x142E18's): the plane 3 s ahead above the ceiling (SW +0x4E30): the HUD's warning (its
// element +0x168, state 0x78A: not drawn natively) and the warning sound (+0x2E8) once a second
void UpdateCeilingWarning(PLANE3& p)
{
    const BANDS3& b = Bands();
    if (b.ceiling <= 0.0f || std::getenv("SLY3_PLANENOSND")) return;
    const glm::vec3 pos = PosOf(p.palo) + 3.0f * VelAloPos(p.palo);
    if (b.ceiling < pos.z)
    {
        if (p.tCeilWarn < 0.0f) p.tCeilWarn = g_clock.t;
        if (g_clock.t - p.tCeilSnd > 1.0f && p.sndCeiling != -1)
        {
            PsndNewPlain(p.sndCeiling);   // (0x244F08 with no source)
            p.tCeilSnd = g_clock.t;
            if (NPrint()) std::printf("plane3: %d ceiling warning sound %d\n", p.palo->oid, p.sndCeiling);
        }
    }
    else if (g_clock.t - p.tCeilWarn > 1.0f) p.tCeilWarn = -FLTMAX;
}

void UpdatePlane(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    PLANE3* pp = Pplane(plo);
    if (!pp) return;
    PLANE3& p = *pp;
    if (!p.fReset) ResetPlane(p);
    // (debug: SLY3_PLANEWPN=state[@seconds]: the player planes given that power-up once, after the seconds (default 5))
    if (static const char* s_sz = std::getenv("SLY3_PLANEWPN"); s_sz && IPlayerOfPlane(plo) >= 0 && p.wpn == p.wpnDefault && p.state == 1)
    {
        static std::map<const LO*, bool> s_mpDone;
        const char* pat = std::strchr(s_sz, '@');
        if (!s_mpDone[plo] && g_clock.t > (pat ? float(std::atof(pat + 1)) : 5.0f))
        {
            s_mpDone[plo] = true;
            SetWeapon(p, int(std::strtol(s_sz, nullptr, 0)));
        }
    }
    if (g_routetele.fPending && g_psw && PloPoCurrent(g_psw) == plo)
    {
        g_routetele.fPending = false;
        if (g_routetele.fH) SetAloRotation(p.palo, cmm::BuildRotateMatrixAxis(g_routetele.h, glm::vec3(0, 0, 1)));
        SetAloPos(p.palo, glm::vec3(g_routetele.x, g_routetele.y, g_routetele.z));
        SetVelWorld(p, p.spd * FwdOf(p.palo));
        SetAloAngVelocityTo(p.palo, glm::vec3(0.0f));
    }
    p.eul = cmm::DecomposeEulerMatrix(glm::mat3(MatObj(p.palo)));
    UpdatePlaneState(p);
    if (p.state != 5) UpdateContacts(p);
    UpdateLooks(p);
    UpdateEngineSound(p);   // 0x146360 (the update 0x141500's last call)
    WakeAlo(p.palo);
    SyncCamera(p);
    if (static const bool s_fStat = std::getenv("SLY3_PLANESTAT") != nullptr; s_fStat && p.mode == 0)
    {
        static int s_c = 0;
        if (++s_c % 600 == 0)   // (debug: the computer's planes every 10 s: alive, avoiding, the nearest pairs)
        {
            const std::vector<PLANE3*> ap = AplaneWorld();
            int cAlive = 0, cAvoid = 0, cNear = 0, cAi[3] = {0, 0, 0};
            float sumNN = 0.0f;
            for (PLANE3* a : ap)
            {
                if (!a->mode || a->state != 1) continue;
                cAlive++;
                cAvoid += a->fAvoid;
                if (a->ai >= 0 && a->ai < 3) cAi[a->ai]++;
                float nn = FLTMAX;
                for (PLANE3* b : ap)
                    if (b != a && b->state == 1) nn = std::min(nn, glm::length(PosOf(b->palo) - PosOf(a->palo)));
                if (nn < FLTMAX) sumNN += nn;
                if (nn < 1500.0f) cNear++;
            }
            std::printf("planestat: t %.1f alive %d avoid %d near1500 %d meanNN %.0f ai %d/%d/%d\n", g_clock.t, cAlive, cAvoid, cNear, cAlive ? sumNN / cAlive : 0.0f,
                        cAi[0], cAi[1], cAi[2]);
        }
    }
    if (NPrint() >= 2)
    {
        static int s_c = 0;
        if (++s_c % (30 * std::max<size_t>(1, s_mpplane.size())) < 1 || (!p.mode && s_c % 30 == 0))
        {
            const glm::vec3 pos = PosOf(p.palo), vel = VelAloPos(p.palo);
            std::printf("plane3: %d mode %d st %d man %d/%d ai %d hp %d ammo %d spd %.0f tgt %.2f %.2f %.2f e %.2f %.2f %.2f pos %.0f %.0f %.0f vel %.0f %.0f %.0f "
                        "target %d\n",
                        plo->oid, p.mode, p.state, p.man, p.sub, p.ai, Hp(p), p.ammo, p.spd, p.roll, p.pitch, p.yaw, p.eul.x, p.eul.y, p.eul.z, pos.x, pos.y, pos.z, vel.x,
                        vel.y, vel.z, p.ptarget ? p.ptarget->palo->oid : -1);
        }
    }
}

// ---- the forces (vt slot 74, 0x141870) ----
void PlaneForces(ALO* palo)
{
    ApplyAloGravity(palo);   // 0x243738
    PLANE3* pp = Pplane(palo);
    if (!pp) return;
    PLANE3& p = *pp;
    switch (p.mode)
    {
    case 3: return;
    case 0:
        if (p.state == 2) return;
        if (p.man && !p.fBlend) return;
        break;
    default: break;
    }
    if (p.state == 4 && g_clock.t - p.tState < p.stun) return;
    if (p.state == 0 || p.state == 5 || p.state == -1) return;
    XF* pos = PxfPos(palo);
    XF* rot = PxfRot(palo);
    const float dt = g_clock.dt;
    if (!pos || !rot || dt <= 0.0f) return;
    cmm::SMP smp = p.asmp[2];
    if (p.mode == 0 && p.state == 1 && p.fBraking) smp.svFast *= 1.5f;
    p.eul = cmm::DecomposeEulerMatrix(glm::mat3(MatObj(palo)));
    glm::vec3 vec;
    vec.x = cmm::RadSmooth(p.eul.x, p.roll, dt, &p.asmp[0], nullptr);
    vec.y = p.fBlend ? p.pitch : cmm::RadSmooth(p.eul.y, p.pitch, dt, &p.asmp[1], nullptr);
    vec.z = cmm::RadSmooth(p.eul.z, p.yaw, dt, &smp, nullptr);
    glm::mat3 matA = cmm::BuildEulerMatrix(vec);
    matA = MatConvertXfs(nullptr, rot->pxfRef, matA);
    const glm::mat3 matC = cmm::MulMat3TransposeA(glm::mat3(rot->mat), matA);
    float g;
    glm::vec3 axis;
    cmm::DecomposeRotateMatrixRad(matC, &g, &axis);
    if (!FXfModeMoving(rot->mode)) SetXfMode(palo, rot, 2);
    rot->vel = glm::vec4(axis * (g / dt), rot->vel.w);
    const glm::vec3 v = glm::vec3(pos->vel) + glm::vec3(pos->aseg[3]) * dt;
    const glm::vec3 w = VecConvertXfs(pos->pxfRef, rot, v);
    const glm::vec3 t = matC * glm::vec3(p.spd, 0.0f, 0.0f);
    const glm::vec3 e = cmm::LimitVectorLength((t - w) / dt, p.accel);
    if (!FXfModeMoving(pos->mode)) SetXfMode(palo, pos, 2);
    AccelAloPos(palo, VecConvertXfs(rot, nullptr, e));   // 0x244828
}
NATIVEFN3(0x141870, PlaneForces);

// ---- a hit (vt slot 78, 0x141B78) ----
bool FAcceptPlaneContact(ALO* palo, CTMSG* pctm)
{
    PLANE3* pp = Pplane(palo);
    if (!pp) return false;
    PLANE3& p = *pp;
    switch (p.state)
    {
    case 2: case 5: case -1: case 0: return false;
    case 4: if (!p.mode && p.fFadeHit) return false; break;
    default: break;
    }
    if (!(pctm->grfAccepted & 2)) return false;
    if (static const bool s_fGod = std::getenv("SLY3_PLANEGOD") != nullptr; s_fGod && !p.mode) return false;
    if (p.psmaShield && OidSmaGoal(p.psmaShield) != 0x1A0)   // the shield is up
    {
        if (NPrint() >= 2) std::printf("plane3: %d hit by %d refused: the shield (t %.3f)\n", palo->oid, pctm->ploSender ? pctm->ploSender->oid : -1, g_clock.t);
        if (p.peShield && g_clock.t - p.tShield > 0.5f)
        {
            p.tShield = g_clock.t;
            TriggerPe(p.peShield);
        }
        return false;
    }
    LO* pfrom = pctm->ploSender;
    if (pfrom && FIsLoDerivedFrom(pfrom, 0x59) && PloProjectileLauncher(pfrom) == palo && !pctm->nKill3) return false;   // its own shot
    palo->SetField(0, 0x184, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pfrom) & 0xFFFFFFFFu));   // 0x1F4DD8 (the last hitter)
    // (PC, duel3.h: the free-for-all scores a kill for its shooter: the plane that fired the projectile, or rammed)
    if (LO* pshooter = pfrom && FIsPlane(pfrom) ? pfrom : pfrom && FIsLoDerivedFrom(pfrom, 0x59) ? PloProjectileLauncher(pfrom) : nullptr;
        pshooter && pshooter != palo && FIsPlane(pshooter))
        s_mpshooter[palo] = {pshooter, g_clock.t};
    switch (pctm->grfFilter)
    {
    case 0x10000:   // the ammunition emptied
        if (p.peHit)
        {
            Add(p.peHit);
            p.tPeHit = g_clock.t;
        }
        PlayShuffledLine(p, p.cEmptyLine, p.aidEmptyLine, &p.iEmptyLine);   // 0x147278
        SetWeapon(p, p.wpnDefault);
        SetAmmo(p, 0);
        return true;
    case 0x800000:   // the partner's hijack
        if (p.partner && FIsPlane(p.partner))
        {
            PlayShuffledLine(p, p.cHijackLine, p.aidHijackLine, &p.iHijackLine);   // 0x147278
            if (p.peHijack) Add(p.peHijack);
            p.hijacker = p.partner;
            p.tHijack = g_clock.t;
            return true;
        }
        break;
    default: break;
    }
    // (Sly 3's amounts are positive damage; the copied projectiles send Sly 2's negative ones, and their default -10 when
    // the damage is Sly 3's option 549 (EOPID 439, the setter of +0x1D4, which the copied code does not read): e.g. the
    // player's gun 95692: 25, the computer's 37428)
    int amount = std::abs(pctm->nState);
    if (pfrom && FIsLoDerivedFrom(pfrom, 0x59) && pctm->nState == -10 && !pfrom->U32Field(0, 0x1D4, 0))
        for (const OPT& o : pfrom->aopt)
            if (o.eopid == 439 && !o.aval.empty()) amount = std::abs(o.aval[0].n);
    // 0x172268 (0x240C78, every hit record): a hit on a player's object is scaled by the dynamic difficulty (D_0036BD20
    // +0x14's +0x20 / +0x24: 1 .. 0.5), truncated, at least 1
    if (!p.mode && amount > 0)
    {
        const float g = GSuck3();
        amount = std::max(1, static_cast<int>(static_cast<float>(amount) * ((1.0f - g) * 1.0f + g * 0.5f)));
    }
    if (pctm->nKill3) SetHp(p, 0);
    else SetHp(p, Hp(p) - amount);
    // the impact's effect: the oldest of the four (or a free one) at the contact
    {
        int ibest = -1;
        float tmin = FLTMAX;
        for (int i = 0; i < 4; i++)
        {
            if (!p.aploImpact[i]) continue;
            if (FInWorld(p.aploImpact[i])) { if (p.atImpact[i] < tmin) { ibest = i; tmin = p.atImpact[i]; } }
            else { ibest = i; break; }
        }
        if (ibest >= 0)
        {
            Add(p.aploImpact[ibest]);
            if (FIsLoDerivedFrom(p.aploImpact[ibest], sly3::CID_ALO)) SetAloPos(static_cast<ALO*>(p.aploImpact[ibest]), pctm->pos);
            p.atImpact[ibest] = g_clock.t;
        }
    }
    if (!p.mode && g_psw && PloPoCurrent(g_psw) == palo)
    {
        TriggerJoyRumbleRumk(4, 0.75f);
        JoltCm(&g_cm, 0);
    }
    if (NPrint())
    {
        int n439 = -999;
        if (pfrom)
            for (const OPT& o : pfrom->aopt)
                if (o.eopid == 439 && !o.aval.empty()) n439 = o.aval[0].n;
        std::printf("plane3: %d hit by %d (cid 0x%X, option 549 %d) kind 0x%X amount %d (%d) kill %d -> hp %d (t %.3f)\n", palo->oid, pfrom ? pfrom->oid : -1,
                    pfrom ? pfrom->Cid() : -1, n439, pctm->grfFilter, pctm->nState, amount, pctm->nKill3, Hp(p), g_clock.t);
    }
    if (Hp(p) > 0)
    {
        if (pfrom == palo && pctm->grfFilter == 0x200)
        {
            if (p.peCrash)
            {
                if (FIsLoDerivedFrom(p.peCrash, sly3::CID_ALO)) SetAloPos(static_cast<ALO*>(p.peCrash), pctm->pos);
                TriggerPe(p.peCrash);
            }
            SetState(p, 4);
        }
        if (!p.mode) Broadcast3(0xA, palo);
        return true;
    }
    SetState(p, 5);
    return true;
}
NATIVEFN3(0x141B78, FAcceptPlaneContact);

// vt slot 31 (0x1414D0: the base's, then the reset 0x142848)
void ResetPlaneSlot31(LO* plo, LO*)
{
    if (PLANE3* p = Pplane(plo)) ResetPlane(*p);
}
NATIVEFN3(0x1414D0, ResetPlaneSlot31);

// vt slot 5 (0x145138): the SO's messages, then 6 / 7 track a shape (+0x358)
void OnPlaneMessage(LO* plo, int msgk, void* pv)
{
    using PFN5 = void (*)(LO*, int, void*);
    if (PFN5 pfn = reinterpret_cast<PFN5>(PvNativeFn(0x1C3E20))) pfn(plo, msgk, pv);   // 0x1F4278 = Sly 2's SO handler
    PLANE3* pp = PplaneFind(plo);
    if (!pp || !pv) return;
    struct TRACK { LO* key; LO* target; };
    const TRACK* pt = static_cast<const TRACK*>(pv);
    if (msgk == 6 && pt->target == plo) pp->tracked = pt->key;
    if (msgk == 7 && pt->target == plo && pt->key == pp->tracked) pp->tracked = nullptr;
}
NATIVEFN3(0x145138, OnPlaneMessage);

// ---- the camera (vt slot 82: 0x141F38 -> a policy of kind 9, priority 7, vtable 0x3530F8; its update 0x147498) ----
struct CPPLANE3 : CPLCY
{
    LO* pplane = nullptr;
    glm::vec3 pos{-650.0f, 0.0f, 220.0f}, vel{0.0f};   // +0x30 / +0x40 (D_00342D60)
    uint32_t VtOrig() const override { return 0x3530F8; }
    void Update(CPDEFI*, const CMJOY*) override;
};
CPPLANE3 s_cpplane;

void CPPLANE3::Update(CPDEFI*, const CMJOY*)
{
    PLANE3* pp = PplaneFind(pplane);
    if (!pp || !FInWorld(pplane)) return;
    PLANE3& q = *pp;
    float gx = 0.0f, gy = 0.0f;
    switch (q.st3)
    {
    case 0: case 1: gx = 0.0f; gy = -0.75f; break;
    case 2: gx = 0.0f; gy = 0.75f; break;
    default:
        if (q.man == 0)
        {
            PLANE3* ph = q.hijacker ? Pplane(q.hijacker) : nullptr;
            const INPUT3 in = ph ? Input(*ph) : Input(q);
            gx = cmm::GLimitAbs(ph ? in.x2 : in.x, 1.0f);
            gy = cmm::GLimitAbs(ph ? in.y2 : in.y, 1.0f);
        }
        break;
    }
    if (q.state != 3 && (q.st3 == 0 || q.st3 == 1 || q.st3 == 2) && q.state != 1) {}
    static const glm::vec3 s_aoff[3][3] = {{{-725, 225, 250}, {-650, 375, 100}, {-725, 225, -100}},
                                           {{-800, 0, 400}, {-650, 0, 220}, {-800, 0, -300}},
                                           {{-725, -225, 250}, {-650, -375, 100}, {-725, -225, -100}}};   // D_00342D20
    const float ax[3] = {std::max(-gx, 0.0f), 1.0f - std::fabs(gx), std::max(gx, 0.0f)};
    const float ay[3] = {std::max(-gy, 0.0f), 1.0f - std::fabs(gy), std::max(gy, 0.0f)};
    glm::vec3 acc(0.0f);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) acc += s_aoff[i][j] * (ax[i] * ay[j]);
    if (q.fBoosting) acc += glm::vec3(1, 0, 0) * q.camBoost;
    else if (q.fBraking) acc += glm::vec3(1, 0, 0) * q.camBrake;
    const glm::vec3 posScreen = ConvertCmWorldToScreen(&g_cm, PosOf(q.palo));
    const float t = cmm::GLimitLm(&s_lmUnit, 2.0f - 4.0f * std::sqrt(posScreen.x * posScreen.x + posScreen.y * posScreen.y));   // s_clq {2, -4, 0}
    const float u = 1.0f - t;
    const float k0 = u * 8.0f + t * 20.0f, k1 = u * 4.0f + t * 6.0f;
    // 0x147410: the spring toward acc
    const float dt = g_clock.dt;
    const glm::vec3 a = k0 * (acc - pos) + k1 * (glm::vec3(0.0f) - vel);
    const glm::vec3 vNew = vel + a * dt;
    pos = pos + (vel + vNew) * (dt * 0.5f);
    vel = vNew;
    const glm::mat4 m = MatObj(q.palo);
    const glm::vec3 posAim = glm::vec3(m[3]) + q.range * glm::vec3(m[0]);   // 0x142D20
    if (q.man >= 3 && q.man < 6 && q.sub != -1 && !q.fBlend)
    {
        const glm::vec3 posEye = glm::vec3(m * glm::vec4(pos, 1.0f));   // 0x1C9428 (the plane's frame)
        const glm::mat3 mat = BuildLookAtMatrix(posAim - posEye, glm::vec3(m[2]));
        SetCmPosMat(&g_cm, posEye, mat);   // 0x166DE8
    }
    else
    {
        const glm::mat3 mat = BuildLookAtMatrix(glm::vec3(m[0]), glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::vec3 posEye = glm::vec3(m[3]) + mat * pos;   // 0x1C9828
        SetCmLookAt(&g_cm, posEye, posAim, 0.0f, 0.0f);
    }
}

void SyncCamera(PLANE3& p)
{
    const bool fCur = !p.mode && g_psw && PloPoCurrent(g_psw) == p.palo && p.palo->fInWorld && !std::getenv("SLY3_PLANENOCAM");
    if (fCur && !p.fCam)
    {
        s_cpplane.pplane = p.palo;
        s_cpplane.pos = glm::vec3(-650.0f, 0.0f, 220.0f);
        s_cpplane.vel = glm::vec3(0.0f);
        SetCmPolicy(&g_cm, 7, &s_cpplane, p.palo, nullptr);
        p.fCam = true;
        if (NPrint()) std::printf("plane3: %d camera on\n", p.palo->oid);
    }
    else if (!fCur && p.fCam)
    {
        RevokeCmPolicy(&g_cm, 0xF, 7, &s_cpplane, p.palo, nullptr);
        p.fCam = false;
        if (NPrint()) std::printf("plane3: %d camera off\n", p.palo->oid);
    }
}

// (PC, splitscreen3.cpp UpdateCameras) another player's view (the 2P duel's player 2): its plane's own camera policy, as
// the current player's (orig3 vt slot 82 -> 0x141F38: each player plane pushes its policy into its player's camera)
std::unordered_map<const LO*, std::unique_ptr<CPPLANE3>> s_mpcpview;

void TraceSet(int eopid, LO* plo, const CRef& r)
{
    if (NPrint()) std::printf("plane3: %d option %d set (%s %.2f)\n", plo->oid, eopid, r.m_tagk == TAGK_S32 ? "n" : "g", r.m_tagk == TAGK_S32 ? float(r.m_n) : r.GAsFloat());
}
template <int E> void SetOpt(LO* plo, const CRef& r)
{
    TraceSet(E, plo, r);
    if (PLANE3* p = PplaneFind(plo)) ApplyOption(*p, E, r);
    else s_mpoptPending[plo].emplace_back(E, r);   // (before the record: applied at its init, after the loaded options)
}
// a field option's value from the record (the scripts' geto: the loaded option list would give the template's value,
// e.g. h_dogfight's 1752 "takes rams" read #t off every copy: the heist's dogfight 3295 then ended at the first kill)
bool FGetOption(const PLANE3& p, int eopid, CRef* pr)
{
    auto n = [&](int v) { pr->SetS32(v); };
    auto g = [&](float v) { pr->SetF32(v); };
    auto b = [&](LO* plo) { pr->SetBasic(plo); };
    switch (eopid)
    {
    case 1684: n(p.k190); break;
    case 1685: n(p.team); break;
    case 1686: n(p.mode); break;
    case 1687: g(p.speed); break;
    case 1688: g(p.boost); break;
    case 1689: g(p.brake); break;
    case 1690: g(p.accel); break;
    case 1691: g(p.pitchMax); break;
    case 1692: g(p.rollMax); break;
    case 1693: g(p.pitchRate); break;
    case 1694: g(p.yawRate); break;
    case 1698: b(p.gunResource); break;
    case 1699: b(p.launcher); break;
    case 1702: g(p.rate); break;
    case 1703: g(p.range); break;
    case 1704: pr->SetBool(p.fInfinite != 0); break;
    case 1705: b(p.region); break;
    case 1757: b(p.ploWpnHud); break;
    case 1706: b(p.shapeFire); break;
    case 1707: b(p.shapeAvoid); break;
    case 1708: n(p.state); break;
    case 1709: g(p.stun); break;
    case 1710: g(p.crashPush); break;
    case 1711: n(p.ramDamage); break;
    case 1712: n(p.bumpDamage); break;
    case 1713: g(p.escape); break;
    case 1714: b(p.fadeHit); break;
    case 1715: pr->SetBool(p.fFadeHit != 0); break;
    case 1716: pr->SetVector(p.posTarget); break;
    case 1717: g(p.spd); break;
    case 1718: g(p.camBoost); break;
    case 1719: g(p.camBrake); break;
    case 1720: b(p.prop); break;
    case 1721: b(p.pilot); break;
    case 1740: pr->SetBool(p.fBoostOk != 0); break;
    case 1741: pr->SetBool(p.fBrakeOk != 0); break;
    case 1752: pr->SetBool(p.fRamProof != 0); break;
    case 1753: b(p.ploRam); break;
    case 1754: b(p.ploFade); break;
    case 1755: b(p.partner); break;
    case 1756: n(p.wpnDefault); break;
    default: return false;
    }
    return true;
}
template <int E> CRef GetOpt(LO* plo)
{
    CRef r;
    if (const PLANE3* p = Pplane(plo))
    {
        FGetOption(*p, E, &r);
        return r;
    }
    // (before the record, e.g. the start event 17 that configures the copies by their 1752: the loaded value, then
    // the setters kept for the record)
    if (E == 1708 || !plo || !FIsLoDerivedFrom(plo, sly3::CID_ALO)) return r;
    PLANE3 t{};
    t.palo = static_cast<ALO*>(plo);
    for (const OPT& o : plo->aopt)
        if (o.eopid == E)
            for (const OPTVAL& v : o.aval) ApplyOption(t, E, RefFromOptionValue(v));
    if (auto it = s_mpoptPending.find(plo); it != s_mpoptPending.end())
        for (const auto& [e, rv] : it->second)
            if (e == E) ApplyOption(t, E, rv);
    FGetOption(t, E, &r);
    return r;
}
template <int... I> void RegisterGetters(std::integer_sequence<int, I...>)
{
    CRef r;
    const PLANE3 pDummy{};
    ((FGetOption(pDummy, 1684 + I, &r) ? RegisterObjectGetter3(1684 + I, &GetOpt<1684 + I>) : void()), ...);
}
template <int... I> void RegisterSetters(std::integer_sequence<int, I...>)
{
    // (1700 / 1701 / 1727 / 1731 / 1733 / 1771 / 1773 are methods: not setters)
    ((1684 + I == 1700 || 1684 + I == 1701 || 1684 + I == 1727 || 1684 + I == 1731 || 1684 + I == 1733 || 1684 + I == 1771 || 1684 + I == 1773
          ? void()
          : RegisterObjectSetter3(1684 + I, &SetOpt<1684 + I>)),
     ...);
}

struct INITPLANE3
{
    INITPLANE3()
    {
        RegisterLoUpdate(FN3_TAG | 0x141500, UpdatePlane);
        // the scripts' setters (every field option of the class) and some getters
        RegisterSetters(std::make_integer_sequence<int, 1775 - 1684>{});
        // the getters of the plain fields (FGetOption); 1753 (+0x428): the plane it collides with while the contact is
        // dispatched (the dogfight's kill count skips the planes two computer planes rammed: h_dogfight's broadcast
        // 0x53 handler counts a death when it is nil or the player)
        RegisterGetters(std::make_integer_sequence<int, 1775 - 1684>{});
        // the SO's ammunition (EOPIDs 939 / 940 = options 941 / 942: +0x178 the most (0x1F4458 / its setter: then the
        // ammunition clamped again) and +0x174 (0x1F4430 / 0x1F4438)); natively only the planes keep them
        RegisterObjectSetter3(939, [](LO* plo, const CRef& r) {
            if (PLANE3* p = Pplane(plo)) { p->ammoMax = NOf(r); SetAmmo(*p, p->ammo); }
        });
        RegisterObjectGetter3(939, [](LO* plo) { CRef r; if (PLANE3* p = Pplane(plo)) r.SetS32(p->ammoMax); return r; });
        RegisterObjectSetter3(940, [](LO* plo, const CRef& r) { if (PLANE3* p = Pplane(plo)) SetAmmo(*p, NOf(r)); });
        RegisterObjectGetter3(940, [](LO* plo) { CRef r; if (PLANE3* p = Pplane(plo)) r.SetS32(p->ammo); return r; });
        // (general, used by h_dogfight's respawn 3146's 2430) an SO's camera, EOPID 945 (option 946, 0x1F4A80: its pad
        // row's camera +0x1194, else the world's first 0x165B28): the world's CAMERA object (cid 0x05; split3's views
        // have no objects of their own), and the camera's method 198 (option 328, thunk 0x102F50: (pos radius) inside
        // its four side planes, 0x167F60): in split screen any view's
        RegisterObjectGetter3(945, [](LO*) {
            CRef r;
            if (g_psw && !g_psw->mpcidaplo[0x05].empty()) r.SetBasic(g_psw->mpcidaplo[0x05].front());
            return r;
        });
        RegisterObjectMethod3(0x102F50, [](LO*, const std::vector<CRef>& a) {
            CRef r;
            r.SetBool(a.size() >= 2 && a[0].m_tagk == TAGK_Vector && FInsideAnyCm(a[0].GetVector(), GOf(a[1])));
            return r;
        });
        // the guns (1700: 0x10FF08 clears; 1701: 0x10FF80 adds by ids, resolved at the post-load / 0x110028 adds objects)
        RegisterObjectMethod3(0x10FF08, [](LO* plo, const std::vector<CRef>&) {
            if (PLANE3* p = Pplane(plo)) p->aguns.clear();
            return CRef(TAGK_Void);
        });
        auto addgun = [](LO* plo, const std::vector<CRef>& a) {
            PLANE3* p = PplaneFind(plo);
            if (!p && a.size() >= 2)   // (at the load: by ids, resolved at the record's init, the copies' from their template's)
            {
                auto idOf = [](const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_pbasic ? dynamic_cast<LO*>(r.m_pbasic) ? dynamic_cast<LO*>(r.m_pbasic)->oid : -1 : -1; };
                auto& v = s_mpgunPending[plo];
                if (v.size() < 4) v.emplace_back(idOf(a[0]), idOf(a[1]));
            }
            else if (p && p->aguns.size() < 4 && a.size() >= 2)
            {
                PLANEGUN g;
                g.muzzle = PloRef(plo, a[0]);
                g.flash = PloRef(plo, a[1]);
                g.fResolved = true;
                p->aguns.push_back(g);
                if (NPrint())
                    std::printf("plane3: %d gun %zu: muzzle %d flash %d (args %d %d / %d %d)\n", plo->oid, p->aguns.size(), g.muzzle ? g.muzzle->oid : -1,
                                g.flash ? g.flash->oid : -1, int(a[0].m_tagk), a[0].m_n, int(a[1].m_tagk), a[1].m_n);
            }
            return CRef(TAGK_Void);
        };
        RegisterObjectMethod3(0x10FF80, addgun);
        RegisterObjectMethod3(0x110028, addgun);
        // 1727 / 1731 / 1733: the effect lists cleared (0x147038 / 0x1470D8 / 0x147178: each removed, vt slot 9)
        RegisterObjectMethod3(0x1100D0, [](LO* plo, const std::vector<CRef>&) {
            if (PLANE3* p = Pplane(plo)) { for (LO* e : p->aploBoost) Remove(e); p->aploBoost.clear(); }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x110148, [](LO* plo, const std::vector<CRef>&) {
            if (PLANE3* p = Pplane(plo)) { for (LO* e : p->aploIn) Remove(e); p->aploIn.clear(); }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x1101C0, [](LO* plo, const std::vector<CRef>&) {
            if (PLANE3* p = Pplane(plo)) { for (LO* e : p->aploOut) Remove(e); p->aploOut.clear(); }
            return CRef(TAGK_Void);
        });
        // 1771 / 1773: a dialog line added to the hijack's / the emptied ammunition's shuffled lines (0x147218 / 0x147248:
        // at most 4; the cursor at the count, so the first play shuffles)
        RegisterObjectMethod3(0x110238, [](LO* plo, const std::vector<CRef>& a) {
            if (!PplaneFind(plo)) { if (!a.empty()) s_mplinePending[plo].emplace_back(0, NArg(a, 0)); return CRef(TAGK_Void); }   // (the load's)
            if (PLANE3* p = Pplane(plo); p && !a.empty() && p->cHijackLine < 4)
            {
                p->aidHijackLine[p->cHijackLine++] = NArg(a, 0);
                p->iHijackLine = p->cHijackLine;
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x1102C0, [](LO* plo, const std::vector<CRef>& a) {
            if (!PplaneFind(plo)) { if (!a.empty()) s_mplinePending[plo].emplace_back(1, NArg(a, 0)); return CRef(TAGK_Void); }   // (the load's)
            if (PLANE3* p = Pplane(plo); p && !a.empty() && p->cEmptyLine < 4)
            {
                p->aidEmptyLine[p->cEmptyLine++] = NArg(a, 0);
                p->iEmptyLine = p->cEmptyLine;
            }
            return CRef(TAGK_Void);
        });
    }
} s_initplane3;
}  // namespace

// (the harness, route.cpp's "dogfight" step) the stick (in the pad's units, after its dead zone: route.cpp turns them into
// the raw bytes), the trigger and the speed buttons that fly the player's plane plo the way a careful player would: a
// target near the nose, the nose on the point where a bullet meets it (the guns' 30000 a second from the next muzzle
// toward the nose's point at the gun range, the target's velocity), the trigger only while that bullet passes within
// the target's radius, the stick feathered under the plane's own dead zone (0x142E18: under 0.2 nothing). It reads the
// game state a player sees (positions, velocities) and changes nothing. False: plo is not a plane under its pad.
namespace {
struct DOGFIGHT3
{
    SW* psw = nullptr;
    LO* ploTarget = nullptr;
    float tPick = 0.0f;
    float accX = 0.0f, accY = 0.0f;   // the feathering's remainders
    int cShots = 0;
    float miss = -1.0f;
    bool fGeomHit = false;
    LO* ploInt = nullptr;
    float eYaw = 0.0f, ePitch = 0.0f, ffYaw = 0.0f, yawPrev = 0.0f, pitchPrev = 0.0f;
    float iYaw = 0.0f, iPitch = 0.0f;
};
std::unordered_map<const LO*, DOGFIGHT3> s_mpdf;   // (per plane: the 2P duel's test flies both)

// the yaw / pitch rate (rad/s) a stick deflection g gives through the target smoothing (0x141870 with the SMP: the target
// g * rate ahead of the current angle, approached at svFast or along the slow end's parabola): its inverse here
float StickForRate(float w, float rate, const cmm::SMP& smp)
{
    const float wAbs = std::fabs(w);
    if (wAbs < 1e-4f || rate <= 0.0f || smp.svFast <= 0.0f) return 0.0f;
    const float dgSlow = (smp.svSlow + smp.svFast) * smp.dtFast * 0.5f;
    const float a = (smp.svFast - smp.svSlow) / (2.0f * std::max(smp.dtFast, 1e-3f));
    float dg;
    if (wAbs >= smp.svFast) dg = std::max(dgSlow, rate);
    else if (a > 0.0f) dg = std::min(dgSlow, (wAbs * wAbs) / (4.0f * a));   // (the parabola dg = a t^2: rate 2 sqrt(a dg))
    else dg = dgSlow;
    return std::copysign(std::min(1.0f, dg / rate), w);
}
float RateForStick(float g, float rate, const cmm::SMP& smp)
{
    const float dg = std::fabs(g) * rate;
    const float dgSlow = (smp.svSlow + smp.svFast) * smp.dtFast * 0.5f;
    const float a = (smp.svFast - smp.svSlow) / (2.0f * std::max(smp.dtFast, 1e-3f));
    return dg >= dgSlow ? smp.svFast : 2.0f * std::sqrt(std::max(a * dg, 0.0f));
}
// a rate under what the plane's dead zone (0.2) allows given as that share of frames at 0.2 (the remainder carried: the
// smoothing has no memory, so the frames' rates add up)
float Feather(float w, float rate, const cmm::SMP& smp, float* pacc)
{
    const float gDead = 0.2f;
    const float g = StickForRate(w, rate, smp);
    if (std::fabs(g) >= gDead) { *pacc = 0.0f; return g; }
    const float wDead = RateForStick(gDead, rate, smp);
    if (wDead <= 0.0f) return 0.0f;
    *pacc += w / wDead;
    if (*pacc >= 1.0f) { *pacc -= 1.0f; return gDead; }
    if (*pacc <= -1.0f) { *pacc += 1.0f; return -gDead; }
    return 0.0f;
}
// the closest a bullet from the muzzle (toward posAim at speed) passes the target (position pos, velocity vel) and when
float BulletMiss(const glm::vec3& posMuzzle, const glm::vec3& posAim, float speed, const glm::vec3& pos, const glm::vec3& vel, float* pt)
{
    const glm::vec3 d = posAim - posMuzzle;
    const float l = glm::length(d);
    const glm::vec3 vb = l > 0.0f ? d * (speed / l) : glm::vec3(0.0f);
    const glm::vec3 r0 = pos - posMuzzle, vr = vel - vb;
    const float vv = glm::dot(vr, vr);
    float t = vv > 0.0f ? -glm::dot(r0, vr) / vv : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    *pt = t;
    return glm::length(r0 + vr * t);
}
}  // namespace

bool FDogfightStick3(LO* plo, float* px, float* py, bool* pfFire, int* pnSpeed)
{
    PLANE3* pp = PplaneFind(plo);
    *pnSpeed = 0;
    if (!pp || pp->mode || pp->state != 1) return false;
    PLANE3& p = *pp;
    DOGFIGHT3& s_df = s_mpdf[plo];
    if (s_df.psw != g_psw) s_df = DOGFIGHT3{}, s_df.psw = g_psw;
    const float speedBullet = 30000.0f;   // (the guns' projectile 95692 leaves the launcher 6701 at 30000 a second)
    const glm::vec3 pos = PosOf(p.palo), fwd = FwdOf(p.palo), vel = VelAloPos(p.palo);
    const float range = p.range > 0.0f ? p.range : 10000.0f;
    // the bullet's meeting point with a plane (two passes of the lead)
    auto lead = [&](PLANE3& o) {
        const glm::vec3 po = PosOf(o.palo), vo = VelAloPos(o.palo);
        glm::vec3 aim = po;
        for (int i = 0; i < 3; i++) aim = po + vo * (glm::length(aim - pos) / speedBullet);
        return aim;
    };
    // the time to bring the nose on a plane: its angle off the nose at the turn rate, plus the distance to close
    auto cost = [&](PLANE3& o) {
        const glm::vec3 d = lead(o) - pos;
        const float s = glm::length(d);
        const float ang = s > 0.0f ? std::acos(std::clamp(glm::dot(d / s, fwd), -1.0f, 1.0f)) : 0.0f;
        return ang / 0.6f + s / 3000.0f;
    };
    PLANE3* best = s_df.ploTarget ? PplaneFind(s_df.ploTarget) : nullptr;
    if (best && (!FInWorld(best->palo) || best->state != 1 || best->team == p.team)) best = nullptr;
    if (!best || g_clock.t > s_df.tPick)
    {
        s_df.tPick = g_clock.t + 0.5f;
        float costBest = best ? cost(*best) * 0.7f : FLTMAX;   // (the current one kept unless another is clearly nearer)
        for (PLANE3* po : AplaneWorld())
        {
            if (po == &p || po->team == p.team || po->state != 1 || !FInWorld(po->palo)) continue;
            const float c = cost(*po);
            if (c < costBest)
            {
                costBest = c;
                best = po;
            }
        }
        s_df.ploTarget = best ? best->palo : nullptr;
    }
    float wYaw = 0.0f, wPitch = 0.0f;
    bool fFire = false;
    if (best)
    {
        const glm::vec3 posT = PosOf(best->palo), velT = VelAloPos(best->palo);
        const glm::vec3 aim = lead(*best);
        const float s = glm::length(posT - pos);
        // the nose that puts the next gun's line through the meeting point: the guns fire from their muzzles toward
        // the nose's point at the gun range (0x142D20), so that point goes on the ray from the muzzle through it
        glm::vec3 dirAim = aim - pos;
        if (!p.aguns.empty())
        {
            const PLANEGUN& g = p.aguns[std::min<size_t>(static_cast<size_t>(std::max(p.igun, 0)), p.aguns.size() - 1)];
            if (g.muzzle)
            {
                const glm::vec3 w = glm::vec3(MatLo(g.muzzle)[3]) - pos;
                const glm::vec3 u = aim - (pos + w);
                const float lu = glm::length(u);
                if (lu > 1.0f)
                {
                    const glm::vec3 uh = u / lu;
                    const float b = glm::dot(w, uh), c = glm::dot(w, w) - range * range;
                    const float lambda = -b + std::sqrt(std::max(b * b - c, 0.0f));
                    dirAim = w + lambda * uh;
                }
            }
        }
        float pan, tilt;
        cmm::ConvertVectorToSphere(dirAim, &pan, &tilt, nullptr);
        const float eYaw = cmm::RadNormalize(pan - p.eul.z);
        const float ePitch = cmm::RadNormalize(-tilt - p.eul.y);
        // the line of sight's own turn (the aim point a frame later), fed forward
        float pan2, tilt2;
        const float dt = std::max(g_clock.dt, 1e-3f);
        cmm::ConvertVectorToSphere(dirAim + (velT - vel) * dt, &pan2, &tilt2, nullptr);
        const float ffYaw = cmm::RadNormalize(pan2 - pan) / dt, ffPitch = cmm::RadNormalize(-tilt2 + tilt) / dt;
        // proportional, the line of sight's turn fed forward, and an integral (the turn the smoothing loses) kept
        // only while the error is small (a new target or a big turn starts it again)
        static const float s_k = std::getenv("SLY3_DFK") ? std::strtof(std::getenv("SLY3_DFK"), nullptr) : 4.0f;
        static const float s_ki = std::getenv("SLY3_DFKI") ? std::strtof(std::getenv("SLY3_DFKI"), nullptr) : 6.0f;
        if (best->palo != s_df.ploInt || std::fabs(eYaw) > 0.3f || std::fabs(ePitch) > 0.3f) s_df.iYaw = s_df.iPitch = 0.0f;
        s_df.ploInt = best->palo;
        s_df.iYaw = std::clamp(s_df.iYaw + eYaw * dt, -0.1f, 0.1f);
        s_df.iPitch = std::clamp(s_df.iPitch + ePitch * dt, -0.1f, 0.1f);
        s_df.eYaw = eYaw; s_df.ePitch = ePitch; s_df.ffYaw = ffYaw;
        wYaw = std::clamp(s_k * eYaw + ffYaw + s_ki * s_df.iYaw, -2.0f, 2.0f);
        wPitch = std::clamp(s_k * ePitch + ffPitch + s_ki * s_df.iPitch, -2.0f, 2.0f);
        // the trigger: the next muzzle's bullet passes the target within its radius, inside the guns' range
        s_df.miss = -1.0f;
        s_df.fGeomHit = false;
        g_fDfGeom = 0;
        if (!p.aguns.empty() && s < 6000.0f)
        {
            const PLANEGUN& g = p.aguns[std::min<size_t>(static_cast<size_t>(std::max(p.igun, 0)), p.aguns.size() - 1)];
            const glm::vec3 posMuzzle = g.muzzle ? glm::vec3(MatLo(g.muzzle)[3]) : pos;
            float t;
            const float miss = BulletMiss(posMuzzle, pos + range * fwd, speedBullet, posT, velT, &t);
            static const float s_rHit = std::getenv("SLY3_DFRHIT") ? std::strtof(std::getenv("SLY3_DFRHIT"), nullptr) : 0.0f;   // (debug: a sphere test of that radius instead of the entries)
            const float rHit = std::min(RadiusOf(best->palo), s_rHit);
            fFire = miss < rHit && t > 0.0f && t < 0.3f;
            s_df.miss = miss;
            g_dfMiss = miss;
            g_dfT = t;
            // the target's own collision entries: the bullet's path seen from the target (its velocity taken off)
            // through them within the gun's range
            if (miss < RadiusOf(best->palo) && t > 0.0f && t < 0.3f)
                if (const OXA* poxa = Poxa(best->palo))
                {
                    const glm::vec3 d = pos + range * fwd - posMuzzle;
                    const float l = glm::length(d);
                    const glm::vec3 vRel = (l > 0.0f ? d * (speedBullet / l) : glm::vec3(0.0f)) - velT;
                    const glm::vec3 b = posMuzzle + vRel * std::min(0.3f, (s + 1000.0f) / speedBullet);
                    LSG alsg[4];
                    bool fHit = false;
                    for (SOE* psoe : poxa->apsoe)
                        if (psoe && ClipSegmentEntry(psoe, posMuzzle, b, 4, alsg) > 0) { fHit = true; break; }
                    if (s_rHit <= 0.0f) fFire = fHit;
                    s_df.fGeomHit = fHit;
                    g_fDfGeom = fHit;
                }
            if (static const bool s_fPrint = std::getenv("SLY3_DFPRINT") != nullptr; s_fPrint && fFire)
                std::printf("dogfight3: shot at %.0f miss %.0f (radius %.0f) t %.3f\n", s, miss, RadiusOf(best->palo), t);
        }
        // the speed: closing on a far target, easing off a near one ahead
        // the speed: the brake while the target is well off the nose (the turn's radius is the speed over the same
        // turn rate: braking out-turns a plane at its own speed), the boost to close on one ahead, easing off near it
        const float ahead = glm::dot(posT - pos, fwd);
        const float offNose = std::max(std::fabs(eYaw), std::fabs(ePitch));
        if (offNose > 0.35f) *pnSpeed = -1;
        else if (s > 2000.0f && ahead > 0.0f) *pnSpeed = 1;
        else if (s < 1000.0f && ahead > 0.0f) *pnSpeed = -1;
        // (s3-integrate) between the two, the boost and the brake in turn every half second, so the range keeps
        // changing: a target flown level at a steady range (the free-for-all's passive planes in tools/duel3.sh) passes
        // each bullet at the same phase of its 470-a-frame step, and the contact marker (0x1AA1D8 / orig3 0x1CDF20) tests
        // only the bullet's place at the frame's end against the smaller object's sphere (the AI-model plane's: radius
        // 224): every bullet then ended 275 past it and none hit (the original's rule: a steady tail chase aliases the
        // same way); a player's range never holds still
        else if (ahead > 0.0f) *pnSpeed = static_cast<int>(g_clock.t * 2.0f) & 1 ? 1 : -1;
        else *pnSpeed = 0;
    }
    // the arena: away from the ceiling (the stall at +0x4E30) and the ground
    const BANDS3& b = Bands();
    if (pos.z < 2200.0f) wPitch = std::min(wPitch, -0.6f);
    if (b.ceiling > 0.0f && pos.z > b.ceiling - 1200.0f) wPitch = std::max(wPitch, 0.6f);
    // the stick: the plane's yaw target is its yaw less x * the yaw rate (+0x1B8), its pitch target its pitch plus
    // y * the pitch rate (+0x1B4); the rates the smoothing gives turned back into deflections, feathered
    *px = -Feather(wYaw, p.yawRate, p.asmp[2], &s_df.accX);
    *py = Feather(wPitch, p.pitchRate, p.asmp[1], &s_df.accY);
    *pfFire = fFire;
    if (fFire) s_df.cShots++;
    struct SETPREV { PLANE3& p; DOGFIGHT3& d; ~SETPREV() { d.yawPrev = p.eul.z; d.pitchPrev = p.eul.y; } } setprev{p, s_df};
    if (static const bool s_fPrint = std::getenv("SLY3_DFPRINT") != nullptr; s_fPrint && (std::getenv("SLY3_DFPRINT")[0] == '2' || static_cast<int>(g_clock.t * 60.0f) % 30 == 0 || fFire))
    {
        const glm::vec3 posT = best ? PosOf(best->palo) : glm::vec3(0.0f);
        const float dtP = std::max(g_clock.dt, 1e-3f);
        std::printf("dogfight3: e %.3f %.3f ff %.2f rate %.2f %.2f\n", s_df.eYaw, s_df.ePitch, s_df.ffYaw, cmm::RadNormalize(p.eul.z - s_df.yawPrev) / dtP,
                    cmm::RadNormalize(p.eul.y - s_df.pitchPrev) / dtP);
        std::printf("dogfight3: t %.2f pos %.0f %.0f %.0f hp %d ammo %d target %d at %.0f (hp %d) miss %.0f w %.2f %.2f stick %.2f %.2f fire %d spd %d\n", g_clock.t,
                    pos.x, pos.y, pos.z, Hp(p), p.ammo, best ? best->palo->oid : -1, best ? glm::length(posT - pos) : 0.0f, best ? Hp(*best) : 0, s_df.miss, wYaw, wPitch,
                    *px, *py, fFire ? 1 : 0, *pnSpeed);
    }
    return true;
}

// (sw.cpp / po.cpp's world reset: the records die with the world)
void ResetPlanes3()
{
    s_mpplane.clear();
    s_mpgunPending.clear();
    s_mplinePending.clear();
    s_mpoptPending.clear();
    s_bands = BANDS3{};
    s_cpplane.pplane = nullptr;
    s_mpcpview.clear();
    s_mpdf.clear();
    s_mpduelauto.clear();
    s_mpshooter.clear();
}

bool FPlane3(const LO* plo) { return FIsPlane(plo); }

void SyncPlaneCameraView3(LO* plo, CM* pcm)
{
    if (!pcm || !FIsPlane(plo) || std::getenv("SLY3_PLANENOCAM")) return;
    PLANE3* pp = PplaneFind(plo);
    if (!pp || pp->mode != 0) return;
    std::unique_ptr<CPPLANE3>& pcp = s_mpcpview[plo];
    if (!pcp) pcp = std::make_unique<CPPLANE3>();
    for (int i = 0; i < pcm->ccpr; i++)
        if (pcm->acpr[size_t(i)].pcplcy == pcp.get()) return;
    pcp->pplane = plo;
    pcp->pos = glm::vec3(-650.0f, 0.0f, 220.0f);
    pcp->vel = glm::vec3(0.0f);
    SetCmPolicy(pcm, 7, pcp.get(), plo, nullptr);
    if (NPrint()) std::printf("plane3: %d view camera on\n", plo->oid);
}

void RestorePlane3(LO* plo)
{
    PLANE3* pp = Pplane(plo);
    if (!pp) return;
    ResetPlane(*pp);
    if (NPrint()) std::printf("plane3: %d restored (option 948): hp %d t %.3f\n", plo->oid, Hp(*pp), g_clock.t);
}

LO* PloPlaneLastShooter3(const LO* plo, float* pt)
{
    const auto it = s_mpshooter.find(plo);
    if (it == s_mpshooter.end()) { if (pt) *pt = -1.0f; return nullptr; }
    if (pt) *pt = it->second.t;
    return it->second.plo;
}

void SetPlaneTeam3(LO* plo, int team)
{
    if (PLANE3* pp = Pplane(plo)) pp->team = team;
}

int NPlaneHp3(const LO* plo)
{
    const PLANE3* pp = PplaneFind(plo);
    return pp ? Hp(*pp) : -1;
}

bool FPlaneFlying3(const LO* plo)
{
    const PLANE3* pp = PplaneFind(plo);
    return pp && pp->state >= 1 && pp->state <= 4 && plo->fInWorld;
}
