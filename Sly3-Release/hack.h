#pragma once

// hack.h: the hack module's records (hack.cpp: Sly 2's module 0x17F090..0x182CC0 on Sly 3's classes 0x49..0x4E;
// hack3.cpp: Sly 3's own functions of the module 0x19A800..0x19F800, registered by their Sly 3 addresses)

#include "alo.h"
#include "crv.h"
#include "lo.h"
#include "sound.h"
#include "splicetypes.h"
#include "vecmat.h"
#include "xf.h"

#include <glm/glm.hpp>

#include <cfloat>
#include <cstdint>
#include <cstring>
#include <vector>

struct ASEG;
struct ASEGA;
struct SM;
struct SMA;
struct CTMSG;
struct PADCTL;
struct XP;

namespace hack
{
inline float FloatField(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t uDefault;
    std::memcpy(&uDefault, &gDefault, 4);
    const uint32_t u = plo->U32Field(0, ib, uDefault);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

// A gun of the ship (orig +0x234, 0x18 bytes, up to 4: method 0x10FD00 -> 0x1800A0)
struct HACKGUN
{
    OID oidPart = -1;          // +0x0: the record (tag oid, kind 6) the right stick turns
    OID oidSm = -1;            // +0x4: the state machine run on the ship (0x68 firing, 0x66 not)
    XF* pxf = nullptr;         // +0x8 (0x1F4C58)
    CTRLPOS* pctrl = nullptr;  // +0xC (0x12DC18: priority 0, mode 7, state 2)
    SM* psm = nullptr;         // +0x10 (0x15E938 kind 4)
    SMA* psma = nullptr;       // +0x14 (0x1F0380)
};

// A state's segment (orig 0x10 bytes: the ship's +0x1C8 x 5 (optid 369 -> 0x1800D8), the enemies' +0x220 x 5
// (optid 369 -> 0x181308), the docks' +0x1D0 x 3 (0x110140)): {oid, its ASEG (0x6D) or SM (0x6F) found from the object (0x17F408), grfapl}
struct HACKSEG
{
    OID oid = -1;
    ASEG* paseg = nullptr;
    SM* psm = nullptr;
    int grf = 0;
};

// An enemy's rule for a state (orig +0x270, 0x28 bytes x 5: methods 0x181330 / 0x181378): the distance / the angle
// ranges to enter it and to stay in it
struct HACKRULE
{
    int fDistance = 0, fAngle = 0;
    LM lmEnterDistance{0, 0}, lmStayDistance{0, 0}, lmEnterAngle{0, 0}, lmStayAngle{0, 0};
};

struct POLY3
{
    float a = 0, b = 0, c = 0;
    float Eval(float t) const { return a + t * (b + t * c); }
};

// The hack object (cid 0x47 fields +0x180..+0x1C0, the ship's 0x48 fields +0x1C0..+0x2B0)
struct HACKSO
{
    int cHpMax = 10;            // +0x180 (EOPID 1363; 0x17F090: 10)
    int cHp = 0;                // +0x184 (EOPID 1364)
    int nPoints = 0;            // +0x188 (EOPID 1365: given at the death, 0x17F5F8)
    int fDead = 0;              // +0x18C (0x17F5B0 / 0x17F110)
    LO* ploBurst = nullptr;     // +0x190 (EOPID 1366: burst at the death)
    float agKnock[4] = {300.0f, 50.0f, 0.0f, 0.0f};  // +0x1A0 (EOPID 1369; D_002BE7D0): g0 + t (g1 + t g2)
    LM lmKnock{0.0f, 800.0f};   // +0x1B0 (EOPID 1370; D_002BE7E0)
    // the ship (0x48)
    int state = -1;             // +0x1C0
    float tState = 0.0f;        // +0x1C4
    float radHeading = 0.0f;    // +0x218
    glm::vec3 vTarget{0.0f};    // +0x220
    int cgun = 0;               // +0x230
    HACKGUN agun[4];            // +0x234
    LO* ploHud = nullptr;       // +0x2A0 (EOPID 1373)
    CSndRef sndEngine;          // +0x2A4: the engine's loop (0x1FBB90 of +0x2A8)
    int idSound = -1;           // +0x2A8 (EOPID 1374)
    float uSound = 0.0f;        // +0x2AC: its volume
    bool fPostLoaded = false;
    static constexpr int CSEG = 7;  // (Sly 3: the ship's 7 states 0..6, 0x19B968(p, 7, +0x1F8); Sly 2's 5)
    HACKSEG aseg[CSEG];            // the ship's +0x1C8 / the enemies' +0x220
    ASEGA* pasegaSeg = nullptr; // +0x194
    SMA* psmaSeg = nullptr;     // +0x198
    // the enemies (0x49): +0x1C8 the cool-down flag, +0x1CC busy (their segments' events 0x25..0x28), +0x1D0 the
    // nav mesh (EOPID 1378), +0x1D4 the target (EOPID 1379), +0x1D8 the path (0x1E94B0(0x2C9, 16)), +0x1DC its place,
    // +0x1E0 / +0x1F0 the last self / target, +0x200 their time, +0x204 the heading, +0x210 the push, the rules +0x270,
    // +0x338 the speed scale (EOPID 1382), +0x340 / +0x350 / +0x360 the polynomials (1383..1385), +0x370 the heading's
    // clamp (1386), +0x378 the turn's {25, 0, 0.25} x r (0x1813C0), +0x384 the push gain 5 x r (0x1813F0)
    int fCooldown = 0, fBusy = 0;
    LO* ploNav = nullptr;
    LO* ploEnemyTarget = nullptr;
    CRV crvPath;
    float sPath = 0.0f;
    glm::vec3 posLastSelf{0.0f}, posLastTarget{0.0f};
    float tPath = 0.0f;
    float radEnemyHeading = 0.0f;
    glm::vec3 vPush{0.0f};
    HACKRULE arule[6];  // (Sly 3: 6 states, +0x2B0 x 6)
    int fKeepTarget = 0;  // (Sly 3 +0x1CC, EOPID 1583: the target is not chosen again)
    float gScale = 250.0f;
    POLY3 polyRemaining{1.0f, 0, 0}, polyAngle{1.0f, 0, 0}, polySimple{0.0f, 1.0f, 0};  // (0x180500: +0x340, +0x350, +0x364 = 1)
    LM lmSimple{-FLT_MAX, FLT_MAX};
    float acTurnEnemy[3] = {25.0f, 0.0f, 0.25f};
    float gPush = 5.0f;
    int cMove = 0;  // (debug: the slot 70 calls)
    LO* ploKiller = nullptr;  // (the posted death's argument)
    LO* ploDock = nullptr;    // the ship's +0x294: its dock point
    CTRL* pctrlDockPos = nullptr;  // +0x298: the docked ship's position controller (0x12DC18)
    CTRL* pctrlDockRot = nullptr;  // +0x29C: its rotation's
    // the docks' state machines to run (+0x200 count, +0x204 x 4 {root oid, SM oid, state on 1, state on 0}: method 526)
    struct RSMG { OID oidRoot = -1, oidSm = -1, oidOn = -1, oidOff = -1; };
    std::vector<RSMG> arsmg;
    // the pieces (0x4A): +0x1C0 the end, +0x1C4 the life (EOPID 1389), +0x1C8 the shooter (method 508), +0x1CC
    // piercing (EOPID 1388), +0x1D0 the target (EOPID 1390), +0x1D4 the turn rate (EOPID 1391)
    float tEnd = FLT_MAX;
    float dtLife = 0.0f;
    LO* ploShooter = nullptr;
    int fPierce = 0;
    LO* ploTarget = nullptr;
    float radTurnRate = 0.0f;
};

HACKSO& Hack(LO* plo);
ALO* Palo(LO* plo);
PADCTL* Ppadctl(LO* plo);
bool FPrint();
bool FShip(const LO* plo);
bool FHackSo(const LO* plo);
void LoadHackFields(LO* plo, HACKSO& h);
void BindHackSegs(LO* plo, HACKSO& h);
void PlayHackSeg(LO* plo, HACKSO& h, const HACKSEG* pe);
void BindShipGuns(LO* plo, HACKSO& h);
bool FShipSegmentDone(LO* plo);
void ResetHackSoBase(LO* plo, HACKSO& h);
void KillHackSo(LO* plo, LO* ploKiller);
void PostKillHackSo(LO* plo, LO* ploKiller);
bool FHackSoHit(LO* plo, CTMSG* pctm);
void SetAloSensors(ALO* palo, bool f);
void DumpHackMapOnce(LO* plo, int state);
extern LO* s_ploShip;
}  // namespace hack

// hack3.cpp (Sly 3's ship, cid 0x4A): the Sly 3 class test, its enable (vt slot 83 0x19BE90: po.cpp's EnablePo), its
// post-load (0x19C048: update.cpp), its state (-2: not a Sly 3 ship)
bool FHackShip3(const LO* plo);
void EnableHackShip3(LO* plo, int n);
void PostLoadHackShip3(LO* plo);
void OnRemoveHackShip3(LO* plo);
void SetHackShip3State(LO* plo, int n);
void OnHackShip3Moved(LO* plo);
void ResetHacks3();
int HackShip3State(const LO* plo);
int NHackHpMax3(const LO* plo, int nDefault);  // (Sly 3 EOPID 937: the hit points' maximum)
LO* PloEnemyNav3(LO* plo);  // (Sly 3 +0x200, EOPID 1572: the enemy's nav mesh)
void SeparateEnemy3(ALO* palo);  // (orig3 0x19D898: the enemies' separation 0x19F6D8)
int HackSide3(LO* plo);  // (Sly 3 +0x1D0, EOPID 1557: the object's side)
void SetHackEnemyHitBy3(LO* plo, LO* ploBy);  // (Sly 3 enemy +0x3F8, EOPID 1584: the last hit's sender, 0x19DBA8)
// (Sly 3 ship +0x30C, EOPID 1567: the other ship of the two-player game; +0x310, EOPID 1568: the camera's height range)
LO* PloHackShipOther3(LO* plo);
void SetHackShipOther3(LO* plo, LO* ploOther);
LM LmHackShipCam3(LO* plo);
// (Sly 3 EOPIDs 937 / 938 of a hack object: its hit points' maximum / its hit points (the SO's +0x170 / +0x16C, kept in
// hack.cpp's record): false when not a hack object
bool FHackHp3(LO* plo, bool fMax, int* pn);
bool FSetHackHp3(LO* plo, bool fMax, int n);
// (tests: route.cpp's fire= at a swarm (cid 0x4E): its live particle nearest to pos)
bool FHackSwarmNearestParticle(LO* plo, const glm::vec3& pos, glm::vec3* pposOut);
