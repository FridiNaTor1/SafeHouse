#pragma once

// Bentley (cid 0x37, module 0x134648..0x1378D8, the reference's m_134648_p3.cpp) and Murray (cid 0x36, module
// 0x1B4438..0x1BA000, m_1B4438_p3.cpp / m_1B68B8_p3.cpp): the two other players, on the player-actor base 0x34
// (actor.h ACTOR, sly.h's base functions; the actors code). Their fields at the original's offsets.
//
// Ported (round 1): init / load / post-load, the update, the state machines (Bentley 0x134850 / 0x135248 and the
// second one 0x136338, Murray 0x1B4AA0 / 0x1B6078) for the ground states (standing, the two gaits) and the jump
// (state 2 through jump.cpp), the camera definition (slot 50), the motion / contact slots (55 / 70 / 85), the jump's
// owner state (slot 88). The other states' bodies (the bombs, the darts, the carry / throw, the attacks ...) warn
// once when reached ("gang: ... not ported").

#include "actanim.h"
#include "actor.h"

#include <glm/glm.hpp>

struct CPDEFI;
struct SW;

// Bentley's sub-object +0x1710 (0x136D28 init, 0x136DC0 its mode by the state, 0x136F48 the mode change: the
// square button's animation between two clips by the target's height)
struct BENTLEYSUB
{
    int state = -1;                // +0x4
    int n8 = 0;                    // +0x8 the side of +0xE4 (0 / 1: which clip of the pair)
    int nC = 0;                    // +0xC the swing may chain (event 0x242)
    int n10 = 0;                   // +0x10 the hand record released (event 0x16E)
    int n14 = 0;                   // +0x14 square let go since
    float t18 = 0.0f;              // +0x18
    float g1C = 0.0f;              // +0x1C
    glm::vec4 v20{0.0f};           // +0x20 (D_002BC920)
    float g30 = -50.0f, g34 = 50.0f;  // +0x30 / +0x34 the heights' range
    ASEGBL* apbl[6]{};             // +0x50 + 0x1C i: the six blends of two (0x137A30; clips +0x3C / +0x48 + 0x1C i)
    ASEGA* pasega = nullptr;       // +0xE0
    LO* ploE4 = nullptr;           // +0xE4 (part 0x17F4: the side test)
};

// Bentley's abilities (+0x1824 + 0xC i: {id, the proxy of its object, its carried object})
struct ABILITY
{
    int id = -1;
    LO* plo4 = nullptr;
    LO* plo8 = nullptr;
};

struct BENTLEY : ACTOR
{
    float t1080 = 0.0f;            // +0x1080 (slot 50: the last time not standing)
    XF* pxf10B8 = nullptr;         // +0x10B8 the record of tag 0xB kind 1 (0x1F4C58; the swing's hand)
    GAIT gaitWalk;                 // +0x1320 (state 1: R1 held)
    ROOTBLEND rb13E0;              // +0x13E0 (state 5's: pbl +0x14E0)
    ROOTBLEND rb14F0;              // +0x14F0 (state 6's: pbl +0x15F0)
    GAIT gaitRun;                  // +0x1600 (state 7)
    float g1A0 = 0.0f;             // +0x1A0 (the gait's phase kept)
    int state = -1;                // +0x13D0
    float tState = 0.0f;           // +0x13D4
    int n16B0 = -1;                // +0x16B0 state 6's (the mine: 0 the summon, 1 held, 2 falling; 0x136040)
    struct ABILITY* pabil16B4 = nullptr;  // +0x16B4 the ability of state 6 (0x136A28)
    LO* ploCarried = nullptr;      // +0x186C the carried object (cid 0x64)
    ASEGA* pasega16CC = nullptr;   // +0x16CC (0x135AD8's dart event 0x243 source; never set by code)
    int n16D8 = 0;                 // +0x16D8 the dart's kind (0: +0x16DC's proxy, 1: +0x16E0's)
    int n16BC = -1;                // +0x16BC the second state machine (the bombs: 0x136338)
    float t16C0 = 0.0f;            // +0x16C0
    float t1700 = 0.0f;            // +0x1700 the last bomb
    BENTLEYSUB sub;                // +0x1710
    int n1800 = -1;                // +0x1800 the carried controller (0x136530: 0 held by Murray, 1 thrown)
    float t1804 = 0.0f;            // +0x1804 its time
    CTRL* actrl1810[2]{};          // +0x1810 / +0x1814 its position / rotation actuators (0x12DC18)
    int f1818 = 0;                 // +0x1818
    int f181C = 0;                 // +0x181C (the other idle: +0x1820)
    ASEG* paseg1820 = nullptr;     // +0x1820
    ABILITY aabil[6];              // +0x1824.. (0x136A28)
    int n1870 = 5;                 // +0x1870 (triangle's ability)
    ASEG* paseg1878 = nullptr;     // +0x1878 (state 14's; else +0x1374 at 1.5)
};

struct MURRAY : ACTOR
{
    float t1080 = 0.0f;            // +0x1080
    GAIT gaitWalk;                 // +0x1320 (state 1: R1 held)
    GAIT gait1460;                 // +0x1460
    GAIT gaitRun;                  // +0x1540 (state 10)
    GAIT gait1630;                 // +0x1630
    ROOTBLEND rb16F0;              // +0x16F0 (pbl +0x17F0)
    SM* psm16E0 = nullptr;        // +0x16E0 state9's standing state machine
    float g1A0 = 0.0f;             // +0x1A0
    int state = -1;                // +0x13D0
    float tState = 0.0f;           // +0x13D4
    int n13D8 = -1;                // +0x13D8
    float t13DC = 0.0f;            // +0x13DC the Thunder Flop phase's start
    ASEG* paseg13E0 = nullptr;     // +0x13E0 the Thunder Flop
    XP* pxp13E4 = nullptr;         // +0x13E4 copied landing contacts (owned until phase 2 / exit)
    int f13E8 = 0;                // +0x13E8 use the alternate Thunder Flop clip
    ASEG* paseg1AE8 = nullptr;     // +0x1AE8 the alternate clip
    LO* plo13EC = nullptr;         // +0x13EC (circle's state 4 object)
    float g1410 = 1500.0f, g1414 = 3500.0f;  // +0x1410 / +0x1414
    float t1434 = -60.0f;          // +0x1434
    int n144C = -1, n1450 = -1, n1458 = -1;  // +0x144C .. (+0x144C the pickup's kind, +0x1450 the carry's state 0x1B93E8)
    LO* plo13F0 = nullptr;         // +0x13F0 the object taken (0x1B7000)
    CTRL* actrlCarry[4]{};         // +0x13F4..+0x1400: held position/rotation and pickup position/rotation
    int mode1404 = 0, mode1408 = 0; // +0x1404/+0x1408: the carried object's original record modes
    ASEG* paseg140C = nullptr, *paseg1AE0 = nullptr; // +0x140C/+0x1AE0: throw/alternate throw
    glm::vec3 pos1420{0.0f};       // +0x1420: selected throw destination
    LO* plo1430 = nullptr;        // +0x1430: just released object, ignored until contacts separate
    SM* psm1510 = nullptr;        // +0x1510: held idle
    float t145C = 0.0f;           // +0x145C: held movement state's entry time
    ASEG* paseg1440 = nullptr, *paseg1444 = nullptr;  // +0x1440 / +0x1444 the pickup's clips (state 4 by +0x144C)
    float t1454 = 0.0f;            // +0x1454 the carry state's time
    int n17F4 = -1;                // +0x17F4
    float t17F8 = 0.0f;           // +0x17F8 state9's animation state entry time
    int n1448 = 0;                 // +0x1448 (event 0x1C6)
    ASEG* apasegPunch[5]{};        // +0x17FC.. the punches (square 0 / 1, triangle 2 / 3; parts 424 / 425 / 435 / 434 / 196)
    int n1810 = -1;                // +0x1810 the punch (0x1B86B0)
    float t1814 = 0.0f;            // +0x1814
    int f1818 = 0, f181C = 0;      // +0x1818 / +0x181C (events 0x240 / 0x241: the stick steers, the punch may end)
    int f1820 = 0;                 // +0x1820 (event 0x242: the next punch may start)
    glm::vec4 pos1830{0.0f};       // +0x1830 where the punch ends
    ASEGA* pasega190C = nullptr;   // +0x190C a punch left playing (0x1B8938)
    int n1840 = -1;                // +0x1840 Berserker Charge's phase (state 12, 0x1B8D30)
    float t1844 = 0.0f;            // +0x1844 its start
    const struct KIND2* pk1850 = nullptr;  // +0x1850 the bounce off a wall (kind-2 tuning of the part +0x1854)
    struct K2PLAY* pk2play1890 = nullptr;  // +0x1890 / +0x1900 its player while phase 2 runs
    int f1908 = 0;                 // +0x1908 the charge hit a wall (0x1B9130)
    int f143C = 0;                 // +0x143C Diablo Fire Slam: state 8's alternate clip +0x1AE4
    float t1AC8 = -60.0f;          // +0x1AC8
    CTRLPOS* pscale1ACC = nullptr; // +0x1ACC, released before the world's transform records
    // the hold (state 6, 0x1B9C00: an object held up, e.g. a gate)
    SM* psm1520 = nullptr;         // +0x1520 the hold's state machine
    float t1524 = 0.0f;            // +0x1524 the time left in a hold state (0x1B68B8)
    float t1528 = 0.0f;            // +0x1528 its value at the last press
    int n152C = 0;                 // +0x152C the presses still needed
    SMA* psma1530 = nullptr;       // +0x1530 the held object's state machine
    int n1534 = 0;                 // +0x1534 the hold's kind (the rows of 0x2C0908 / 0x2C0948 / 0x2C0990)
    // the second throw controller (state 15: the Bentley toss; peer +0x191C, selected +0x1920 = the global Bentley)
    JTTHROW jtthrow1910;           // +0x1910
};

BENTLEY* PbentleyFromLo(LO* plo);  // null outside 0x37
BENTLEY* PbentleyGlobal();         // D_002DD5BC (the world's Bentley)
MURRAY* PmurrayGlobal();           // D_002F7900 (the world's Murray)
MURRAY* PmurrayFromLo(LO* plo);    // null outside 0x36
// The class's native data (step.cpp PstepNew): null outside 0x36 / 0x37
STEP* PstepNewGang(ALO* palo);

// The world's start (transition.cpp, after InitWorldActors): Bentley's and Murray's inits and post-loads.
void InitWorldGang(SW* psw);

// orig: 0x00135248 (Bentley's state change)
void SetBentleyState(BENTLEY* p, int n);
// orig: 0x001B6078 (Murray's)
void SetMurrayState(MURRAY* p, int n);
void SetMurrayBerserk(MURRAY* p, int st);  // orig 0x1B8D30 (state 12's phase)
// orig: 0x001B9C00 (SCPT 0x4F): Murray holds the object of psmaObj (state 6, kind k); both state machines to oid
// (fSnap: at once, 0x1F0948; else 0x1F0660)
struct SMA;
void StartMurrayHold(MURRAY* p, SMA* psmaObj, int oid, int k, int fSnap);
// orig 0x001B7000: Murray takes the used object (a cid 0x24 use, 0x184330): when he can carry (+0x13EC), it at +0x13F0,
// the pickup kind 0, his state 4, facing the object's owner (+0x180)
void MurrayTakeObject(ACTOR* pa, LO* puse);

// orig: 0x00136530 (Bentley's carried controller +0x1800: -1 none, 0 held by Murray's +0x1910, 1 thrown)
void SetBentleyCarried(BENTLEY* p, int k);

// orig: 0x001358A8 / 0x001B6728 (slot 50)
void GetBentleyCpdefi(BENTLEY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pc);
void GetMurrayCpdefi(MURRAY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pc);

// orig: 0x001A0300 on the parts at ib + 4 + 0x20 i (the actor's root blends) / 0x001A0450 (the weights by +0x190)
struct ROOTBLEND;
struct ALO;
struct ACTOR;
void BindActorRootBlend(ROOTBLEND* prb, ALO* palo, uint32_t ib);
void SteerActorRootBlend(ACTOR* p, ROOTBLEND* prb);

// Native world lifetime: unlink and free Murray's optional scale controller
// while its ALO and XF records still exist.
void ReleaseMurrayScaleControllers(SW* psw);
