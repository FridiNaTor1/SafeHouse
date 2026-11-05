#pragma once

// The player characters' actor class (cid 0x34, module 0x13FF70..0x144E20, the reference's m_13FF70.cpp) and Sly
// (cid 0x35, 0x188488..0x19CBF8): the parts other modules use so far.
//   - The camera definition (vt slot 50): 0x1F58F0 (the SO's pose record), 0x141640 (the actor's camera target),
//     0x18A428 (Sly's); the focus point 0x141588 (slot 97 for actors; Sly's 0x18A350).
//   - The checkpoint placement (vt slot 38, 0x2052E0).
// The actor's own fields live in ACTOR (by object), at the original's offsets. What needs the physics runtime (the
// SO's ground contact +0x208 / +0x238, the point velocity 0x22D348, the angular velocity slot 34 0x1F4558) or Sly's
// state machine (+0x18C8 and the sub-objects at +0x2C0 / +0x4C0 / +0xDF0 / +0xF50) is fed by ACTORPOSE and the
// fields below; natively these are stand-ins until the physics port and Sly's controller land.

#include "cm.h"
#include "step.h"
#include "actanim.h"
#include "so.h"
#include "reach.h"
#include "interact.h"
#include "aistack.h"
#include "crv.h"
#include "slyfeet.h"
#include "sound.h"
#include "jtthrow.h"

#include <string>

#include <glm/glm.hpp>

struct LO;
struct CRV;

struct ASEG;
struct ASEGA;
struct ASEGBL;
struct CTRLSOURCE;
struct SM;
struct SMA;

// The jump (the actors' sub-object at +0x2C0: 0x195950 init, 0x1959C0 post-load, 0x195A00 its state, 0x195FB8 its
// animation state, 0x197500 / 0x197F98 / 0x196DF8 the per-frame parts). Offsets relative to +0x2C0.
struct JUMP
{
    struct ACTOR* pactor = nullptr;  // +0x0
    int kind = -1;                 // +0x4 the jump's kind (0..18; 0x195A00's n)
    int n8 = 0;                    // +0x8
    LO* ploC = nullptr;            // +0xC the animation state's object
    float g10 = 0.0f;              // +0x10 the target's curve parameter (0x19B110: the facing point of state 15)
    float s14 = 0.0f;              // +0x14 the target's curve distance (state 15: where the pipe is grabbed)
    float t18 = 0.0f;              // +0x18 the kind's start
    int state = -1;                // +0x1C the animation state (-1 none, 0..22; actor +0x2DC)
    float t20 = 0.0f;              // +0x20 its start
    int f24 = 0, f28 = 0, f2C = 0, f30 = 0;  // +0x24 .. +0x30 (+0x28: the camera's own-control flag, actor +0x2E8)
    float g34 = 1.0f, g38 = 1.0f;  // +0x34 / +0x38 the blend weights
    float z3C = 0.0f;              // +0x3C the height at the kind's start
    int n40 = 0;                   // +0x40
    float t44 = 0.0f, t48 = 0.0f, t4C = 0.0f;  // +0x44..
    LO* ploLeft = nullptr;         // +0xBC the object of the owner's state just left (0x18C250: the pipe of state 8,
                                   // +0x18C4 of 3 / 22, +0x1F18 of 21): its target score is 1000 worse (0x19B088)
    glm::vec3 vecA0{0.0f};         // +0xA0 the air control's target velocity (0x197F98)
    float radB0 = 0.0f, gB4 = 0.0f, sB8 = 0.0f;  // +0xB0 / +0xB4 / +0xB8 (kind 8: the launch's heading, 1, speed)
    glm::vec4 vecF0{0.0f};         // +0xF0 (0x2BF740)
    float g108 = -60.0f, g10C = -50.0f, g110 = 75.0f, g114 = 50.0f, g118 = -150.0f, g11C = 75.0f;  // +0x108.. limits
    ASEG* apasegJump[5]{};         // +0x120.. the jumps' segments by +0x8
    ASEG* apasegFall[5]{};         // +0x134..
    ASEG* pasegLand148 = nullptr, *pasegLand14C = nullptr, *pasegLand158 = nullptr, *pasegLand15C = nullptr;
    ASEG* pasegLand154 = nullptr;  // +0x154 (state 17: the hang)
    const SOE* psoeEdge = nullptr; // +0xC8 the ledge's entry (state 17)
    glm::vec3 posEdgeLocal{0.0f};  // +0xD0 the grabbed point in its record's frame
    glm::vec3 normalEdge{0.0f};    // +0xE0 the edge's normal (out of the wall)
    int xpkEdge = 0;               // +0x100 the contact's kind
    const void* pvEdge = nullptr;  // +0x104 its feature on the ledge's entry
    int f160 = 0;                  // +0x160 (on a moving support)
    float g164 = 1.0f;             // +0x164
    float sSizeSaved = 1.0f;       // +0x168
    LO* ploZone16C = nullptr;      // +0x16C the Mega Jump's camera zone (actor part +0x42C, oid 1516)
    LO* ploZone170 = nullptr;      // +0x170 its second (+0x430, oid 1517: from the top, 0x197F98's end)
    int f174 = 0;                  // +0x174 (the Mega Jump: the size +0x168 restored at its end)
    ASEG* pasegGlide = nullptr;    // +0x178 the paraglider's opening (state 19; actor part +0x438, oid 1518)
    ASEG* apasegGlideBl[5]{};      // +0x180 / +0x18C / +0x198 / +0x1A4 / +0x1B0 the glide blend's BLs (+0x440.. oids
                                   // 1519 1520 1521 1523 1522: level, left, right, forward, back; weights +0x17C..)
    ASEGBL* pbl = nullptr;         // +0x1B8 the glide blend (0x1959C0: 0x137A30 of the five BLs)
    LO* ploGlider = nullptr;       // +0x1BC the paraglider's camera zone (+0x47C, oid 1524; 0x140C88)
    float svGlide1C0 = 0.0f;       // +0x1C0 the glide's forward speed (state 20, 0x197F98)
    float svGlide1C4 = 0.0f;       // +0x1C4 its vertical speed
    float tEnd21 = 0.0f;           // (+0x1F0's +0x1C: the jump back's end, state 0x15)
    struct CTRL* pctrl21 = nullptr;  // state 21's controller (0x12DE58 kind 8: the record driven to the safe ground)
};

// The damage controller (orig ACTOR +0xDF0, 0x1A0B18 init; its state is ACTOR::nSubDF4: -1 none, 0..12 the
// reactions, 7 knocked back): damage.h. A hit is a contact message (so.h CTMSG, 0x1F7598) its slot 75 accepts.
struct ACTOR;
struct DAMAGE
{
    float tStart = 0.0f;           // +0x8 the reaction's start
    int cCapacity = 0x28;          // +0xC
    int* pcount = nullptr;         // +0x10 the counter (Sly: the save's health, 0x3D4AB0)
    void (*pfnNext)(ACTOR*) = nullptr;  // +0x1C the deferred action (a PMF) at the reaction's end
    CTMSG req;                     // +0x30 the last hit (a contact message: the sender, its filter = the kind, its
                                   // state = the counter's change, the velocity +0x30)
    int nFlash = -1;               // +0x90 the flashing (1: hit, invulnerable; 0: healing)
    float tFlash = 0.0f;           // +0x94 its start
    float tFlashFade = 0.0f;       // +0x98 its fade's start (FLT_MAX while reacting)
    float dtFlash = 2.0f;          // +0x9C its length
    float gFlash = 0.0f;           // (the object's +0xD0: the flash's strength; TODO(render))
};

// The safe ground (orig ACTOR +0x9E0, 0x19F3E8 init): the last 32 floors stood on (0x19FEB0: the floor entry and the
// point in its object's frame; 0x19FD30 the states 3 / 22's objects), the best one taken back to after a fall
// (0x19F590): damage.h.
struct SAFEGROUND
{
    struct ENTRY
    {
        int id = 0;                // +0x0 (the floor entry's +0x4)
        LO* plo = nullptr;         // +0x4 (0x19FD30's object)
        SOE* psoe = nullptr;       // +0x8 the floor's entry
        float t = 0.0f;            // +0xC when
        glm::vec3 pos{0.0f};       // +0x10 the point in the entry's object's frame (0x19FD30: +0x10 its parameter)
        // (SCPT 0x2A's entries: +0 the target object, +4 its interaction sub-object (0x184670): natively these two)
        LO* ploTarget = nullptr;
        bool fTargetSub = false;
    };
    int c = 0;                     // +0x4 (+0x9E4)
    ENTRY ae[32];                  // +0x10
};

// The binocucom (orig ACTOR +0xF00, module 0x19CBF8..0x19DE10; its state is ACTOR::nF04, its view ACTOR::nF20):
// binoc.h.
struct BINOCWIN
{
    int state = -1;                // +0x28 (+0x24 +4): -1 off, 0 opening, 1 on, 2 closing
    LO* ploFrame = nullptr;        // +0x2C the frame (Sly 0x35F, Bentley 0x360, Murray 0x361)
    LO* ploWindowDefault = nullptr;  // +0x30 the window (0x368 / 0x369 / 0x36A)
    LO* ploWindow = nullptr;       // +0x34 the window shown (a script's, else the default)
    void* pprompt = nullptr;       // +0x38 the talk prompt (Sly +0x1D40, 0x201D78: TODO)
};
struct BINOC
{
    float tState = 0.0f;           // +0x8
    ASEG* pasegOpen = nullptr;     // +0xC
    ASEG* pasegClose = nullptr;    // +0x10
    ASEGA* pasega = nullptr;       // +0x14 the opening / closing
    ASEGA* pasegaHeld = nullptr;   // +0x18 the actor's animation held while it is on
    BINOCWIN win;                  // +0x24
    float g3C = 0.0f;              // +0x3C the zoom (the camera's +0x470 / +0x5CC)
    LO* ploSpeaker = nullptr;      // +0x40 (view 1: the other speaker)
    LO* ploScreen = nullptr;       // +0x44 (view 1: the window to show)
};

// The climb (orig ACTOR +0xF50, module 0x19DE10..0x19EF50: a pipe climbed along its path; Sly's state 8, Bentley's /
// Murray's 13): climb.h. Its state is ACTOR::nSubF54 (-1 off, 0 grabbing, 1 holding, 2 / 3 climbing up / down (the
// pipe's direction), 4 turning round), the pipe ACTOR::ploF5C / pcrvF50, the distance sCrvF50, the speed gCrvF50.
struct CLIMB
{
    float tState = 0.0f;           // +0x8
    int fDown = 0;                 // +0x18 the climber faces down the curve
    float gSpeed = 0.0f;           // +0x1C the speed's running fraction (of the faster clip's)
    int nDir = 0;                  // +0x20 the stick's class (-1 none, 0 / 1 along / against the pipe on screen)
    glm::vec3 posHand{45.0f, 0.0f, 40.0f};   // +0x30 the hand in the climber's frame (held on the pipe)
    glm::vec3 pos40{45.0f, 0.0f, -20.0f};    // +0x40
    float aw[2]{1.0f, 0.0f};       // +0x50 / +0x60 the climb blend's weights
    SLOPEBLEND sb;                 // +0x54 / +0x64 the climbs, +0x5C / +0x6C their slopes, +0x70 the blend
    ASEG* pasegHold = nullptr;     // +0x74 (state 0; Sly: none)
    ASEG* pasegIdle = nullptr;     // +0x78 (state 1)
    ASEG* pasegGrab = nullptr;     // +0x7C (state 0 without +0x74: from its middle at half speed; state 4)
};

// The actor (cid 0x34: 0x13FF70 init, 0x1400B8 post-load, 0x1403D0 update; 0x1320 bytes): the STEP (step.h), the
// sub-objects at +0x2C0 (the jump, 0x195950), +0x4C0 (0x191090), +0x9E0 (0x19F3E8), +0xDF0 (0x1A0B18), +0xF00
// (0x19CBF8), +0xF50 (0x19DE10), +0xFD0 (0x144638), +0x11B0 (0x21F2E0), and the actor's own state.
struct ACTOR : STEP
{
    bool fPartTableBound = false;  // native: 0x1400B8's table is bound before its posted callbacks
    JUMP jump;                     // +0x2C0
    int f434 = 0;                  // +0x434
    int n270 = 0;                  // +0x270 (0x144068: busy when positive)
    AISTACK stack;                 // +0x26C the actor's own jobs
    CRV crv294;                    // +0x294 the path shared by actor jobs
    struct WATCH
    {
        glm::vec3 pos{0.0f};      // +0xFD0 +0x10 the follower's target point
        float s = 0.0f, t = 0.0f; // +0x20 distance last checked, +0x24 next check
        int fActive = 0;          // +0x28 recovery suspends job input/update
    } watch;
    int n4C8 = -1;                 // +0x4C8 (sub-object 0x4C0's kind; -1 none, as the reference's RAM)
    int nSub4CC = -1;              // +0x4CC (sub-object 0x4C0's)
    int nSubDF4 = -1;              // +0xDF4 (sub-object 0xDF0's)
    DAMAGE dmg;                    // +0xDF0 the damage controller (damage.h)
    SAFEGROUND safe;               // +0x9E0 the safe ground (damage.h)
    SAFEGROUND::ENTRY entryJump;   // +0x490 the safe ground jumped back to (the jump's kind 0xA)
    int nF04 = -1;                 // +0xF04 (sub-object 0xF00's state: -1 by its init 0x19CBF8)
    int nF20 = -1;                 // +0xF20 (its +0x20: -1 by 0x19CBF8)
    BINOC binoc;                   // +0xF00 the binocucom (binoc.h)
    int nSubF54 = -1;              // +0xF54 (sub-object 0xF50's state: climb.h)
    const CRV* pcrvF50 = nullptr;  // +0xF5C's curve (sub-object 0xF50's object: the pipe of state 8)
    LO* ploF5C = nullptr;          // +0xF5C the pipe climbed
    float sCrvF50 = 0.0f;          // +0xF60 (the distance on it)
    float gCrvF50 = 0.0f;          // +0xF64 (its speed: ds / dt)
    CLIMB climb;                   // +0xF50's other fields (climb.h)
    glm::vec3 vecDrawOffset{0.0f}; // +0x1010 the body drawn this far from its physics position (TODO(render))
    int n1000 = 0;                 // +0x1000
    float g1020 = 100.0f;          // +0x1020
    SM* psmIdle = nullptr;         // +0x1028 the idle state machine (Sly: part 0x7C)
    SM* psm102C = nullptr;         // +0x102C
    ASEGA* pasega = nullptr;       // +0x1030 the current animation
    SMA* psma = nullptr;           // +0x1034 the current state machine
    int n1038 = -1;                // +0x1038
    float g103C = 20.0f;           // +0x103C (0x13FF70)
    int f1044 = 0;                 // +0x1044 (the camera target fixed at +0x1060)
    float sHeight = 125.0f;        // +0x1048 (0x13FF70)
    float zFocus = 0.0f;           // +0x104C (the focus height, limited)
    float zFocusSmooth = 0.0f;     // +0x1050 (the smoothed one)
    glm::vec4 posCmTarget{0.0f};   // +0x1060 (the last camera target)
    glm::vec3 vecOffset{0.0f};     // +0x1070 (the target's offset, smoothed to zero)
    void* prunScript = nullptr;    // +0x1084 the script runner driving it (scpt.cpp, 0x143DE8)
    LO* ploWater = nullptr;        // +0x108C the water it is in (messages 4 / 5 of the water 0x59)
    float sWaterHeight = 50.0f;    // +0x10A0 the height of its point tested in the water (Sly 50)
    int n10C8 = -1, f10F0 = 1, n10EC = -1, n10E4 = -1;  // +0x10C8 / +0x10F0 / +0x10EC / +0x10E4 (0x13FF70)
    LO* ploNoise = nullptr;        // +0x10C4 its noise template (the child oid 684, cid 0x75; resolved on first use)
    bool fNoiseResolved = false;
    float g10CC = 0.0f;            // +0x10CC how far the guards would perceive it (0x140750: 0..1)
    float g10D0 = 0.0f;            // +0x10D0 its noise build-up (0x140750: at 1 the noise +0x10C4 is made)
    LO* plo10DC = nullptr;         // +0x10DC the object faced (state 5's substate 4: 0x2046F0)
    LO* plo10E8 = nullptr;         // +0x10E8 the target (0x194A40's pick; +0x10EC its kind: 4 a guard in reach, 0x140AF8)
    float g10D4 = 1.0f, g10D8 = 1.0f;  // +0x10D4 / +0x10D8
    uint32_t grf10E0 = 0;          // +0x10E0
    struct CTRLPOS* pctrlLook = nullptr;  // +0x10F4 the head's look controller (0x1426E8: the record tag 0x17 / 0x16)
    LO* ploLook = nullptr;         // +0x10F8 the object looked at (0x1427C8)
    int nLookKind = 0;             // +0x10FC its kind (the guard's rank, or an interest point's priority)
    int f1100 = 0;                 // +0x1100 the look target held (not chosen each frame; 0x142860 clears it)
    int f1104 = 0;                 // +0x1104 a fixed look (+0x1110 pan, +0x1114 tilt, +0x1118 roll)
    float g1110 = 0.0f, g1114 = 0.0f, g1118 = 0.0f;
    float lmPan[2] = {0.0f, 0.0f};  // +0x1120 the head's pan range (none by default: the head stays)
    float lmTilt[2] = {0.0f, 0.0f}; // +0x1128 its tilt range
    float g1130 = 0.0f, g1134 = 0.0f;  // +0x1130 / +0x1134 the roll at the pan range's ends
    glm::mat3 mat1150{0.0f};       // +0x1150 the head record's rest rotation in the object's frame
    float g1138 = 1.5f, g113C = 2.5f, g1140 = 3.0f, g1144 = 5.0f;  // +0x1138.. the look times' ranges
    int fLook = 0;                 // +0x1148
    float tLookNext = 0.0f;        // +0x114C
    int f12F0 = 1;                 // +0x12F0
    int n1300 = 100;               // +0x1300
    float g1304 = -60.0f;          // +0x1304
    int n1318 = -1;                // +0x1318
    ASEG* pasegAbility = nullptr;  // +0x130C pending cane animation
    ASEGA* pasegaAbility = nullptr; // +0x1310 active cane animation
    int idAbility = 0;             // +0x1314 active ability, -1 after clearing
    int n9BC = -1;                 // +0x9BC
    LO* plo1024 = nullptr;         // +0x1024 the current player's camera zone (0x140C88)
    int n1040 = 0;                 // +0x1040 the side the interaction faces (0x1830D8: 1 flips it)
    INTERACT inter;                // +0x4C0 the interaction sub-object (interact.h; its kind n4C8, state nSub4CC)
    float t131C = 0.0f;            // +0x131C (the jump's last velocity change)
    JTTHROW jtthrow;               // +0x11B0 (the shared aimed throw controller)
};

// Sly (cid 0x35: 0x188488 init, 0x1885B8 load, 0x188608 post-load, 0x18BCE0 update; 0x2060 bytes).
struct SLY : ACTOR
{
    GAIT gaitWalk;                 // +0x1320 (states 1 / 10: the walk gait; sfx 0x6F)
    GAIT gaitRun;                  // +0x1C30 (state 12)
    LO* plo1BCC = nullptr;         // +0x1BCC (the pickpocket's prize held)
    int f1BD4 = 0;                 // +0x1BD4 the pickpocket's hand set (0x18B460)
    float g1BD8 = 0.0f;            // +0x1BD8 the take's time in the clip +0x1BC8 (its event 0x167, 0x18B348)
    float t1BDC = 0.0f;            // +0x1BDC the hand's last set
    int f1514 = 0;                 // +0x1514 (a foot over a drop: 0x17C played)
    float g1A0 = 0.0f;             // +0x1A0 (STEP: the gait phase kept; ACTANIM uGait)
    int cq = 0;                    // +0x1880 the kept positions' count
    glm::vec4 aq[3]{};             // +0x1890 / +0x18A0 / +0x18B0
    LO* ploOther = nullptr;        // +0x18C4 (the object of states 3 / 22)
    REACH reach;                   // +0x17D0 the cane's reach (reach.h; +0x1854 its target, +0x1858 its distance)
    int state = -1;                // +0x18C8 (0..24)
    float tState = 0.0f;           // +0x18CC
    int substate = -1;             // +0x18D0
    float tSubstate = 0.0f;        // +0x18D4
    int mode = -1;                 // +0x18D8 (0x18E540)
    float tMode = 0.0f;            // +0x18DC
    ASEGA* pasegaMode = nullptr;   // +0x18E0 the attack mode's animation (0x18E540; its end: message 3)
    int f18E4 = 0, f18E8 = 0, f18EC = 0, n18F0 = -1;  // +0x18E4.. (the combo window open, its follow-up, square held)
    LO* plo19A0 = nullptr;         // +0x19A0
    LO* plo19A4 = nullptr;         // +0x19A4 the followed object
    LO* plo19A8 = nullptr;         // +0x19A8 the cane of the climbs (mode 11) and mode 2, out of the world otherwise
    float g19AC = 15.0f;           // +0x19AC the kept positions' radius
    float t19B0 = 0.0f;            // +0x19B0
    float tActive = 0.0f;          // +0x1AA4
    ROOTBLEND rb13D0;              // +0x13D0 state 5's walk around the target (pbl +0x14D0)
    ROOTBLEND rb1AC0;              // +0x1AC0 state 10's sneak (pbl +0x1BC0)
    int f1D2C = 0;                 // +0x1D2C contextual juggle strike has reached event 0x173
    int f1D30 = 0;                 // +0x1D30
    SMA* psma1D28 = nullptr;       // +0x1D28 the paraglider's SM (+0x1D24 oid 1526 on +0x1D20 oid 1525: 0x188608's end;
                                   // goal 0x5F7 open, 0x5F8 closed)
    ROOTBLEND rb1E10;              // +0x1E10 state 21's move on a climbing surface (pbl +0x1F10; its SM +0x1F14)
    LO* ploSurface = nullptr;      // +0x1F18 the climbing surface of state 21 (climbsurf.h)
    int itriSurface = 0;           // +0x1F1C its triangle,
    float uSurface = 0.0f, vSurface = 0.0f;  // +0x1F20 / +0x1F24 the place in it
    glm::vec4 aposSurface[3]{};    // +0x1F30 / +0x1F40 / +0x1F50 (method 771, 0x185E30)
    ASEGBL* pblHang = nullptr;     // +0x1F90 state 22's blend (0x137A30: weights +0x1F60 base, +0x1F6C left, +0x1F78
                                   // middle, +0x1F84 right)
    float gHangAmount = 0.0f;      // +0x1F94 state 22's stick across the hook (-1..1)
    float gHangWeight = 0.0f;      // +0x1F98 state 22's stick size (the launch's scale)
    glm::vec3 vecHangOffset{0.0f}; // +0x1FA0 the hand below a hook (method 0x10B6D0 -> 0x18DF58)
    // State 3, the swing on a hook of kind 0 / 1 / 2 (slyhang.cpp)
    float tHang = 0.0f;            // +0x18F4 the stick's last near / far time (entry: the time)
    int nHangPrev = -1;            // +0x18F8 the substate the stick left from (0 / 2; -1 none)
    int fHangBack = 0;             // +0x18FC facing the hook's back
    glm::vec3 vecHangAccel{0.0f};  // +0x1910 the pendulum's acceleration (0x18FC60), applied by the motion 0x1903B8
    SM* psmHang = nullptr;         // +0x1950 substate 3's SM
    ASEGBL* pblSwing = nullptr;    // +0x1994 the swing's blend (0x18F908: clips +0x1974 / +0x1980 / +0x198C)
    float gSwingSpeed = 1.0f;      // +0x1998
    float gSwingActive = 1.0f;     // +0x199C
    struct DODGE
    {
        int state = -1;             // +0x1FF4 (controller +4): -1 off, 0/1 left/right
        float tState = 0.0f;         // +0x1FF8
        int lastSide = 0;           // +0x1FFC
        ASEG* apaseg[2]{};          // +0x2000 / +0x2004
    } dodge;                       // +0x1FF0 (owner is the containing SLY)
    struct SLIDE
    {
        ASEG* apaseg[3]{};          // +0x2018 / +0x2024 / +0x2030 (the BLs' clips)
        ASEGBL* pbl = nullptr;      // +0x2038; native blend owns the three weights
        CTRLSOURCE* aphandle[7]{};  // +0x19E4..+0x19FC (last two use bits 64 / 32)
        std::shared_ptr<CTRLSOURCE> ahandleOwned[7]; // lifetime follows the native actor, including removed handles
        uint32_t grfHandles = 0;    // current 0x18C120 mask
        SLYFOOTPROBE afoot[2];      // +0x1520 / +0x1620
    } slide;
    CSndRef snd2048;               // +0x2048 Shadow Power's loop; appended to preserve native field offsets
    struct POCKET
    {
        int id;
        LO* proxy = nullptr;
        LO* retained = nullptr;
    } pockets[4]{{26}, {37}, {38}, {41}};  // orig: 0x188488, +0x1FB0..+0x1FD4
    LO* plo18C0 = nullptr;        // +0x18C0: selected strike/rush target (0x18F688)
};

// The SO's pose and velocities, as the original reads them from its transform record (+0x60: world matrix +0x40,
// position +0x70) and the physics.
struct ACTORPOSE
{
    glm::mat3 mat{1.0f};           // the record's world matrix (glm columns = the original's rows)
    glm::vec3 pos{0.0f};           // its position
    glm::vec3 vPoint{0.0f};        // 0x22D348(record, 0, pos): the position's velocity
    glm::vec3 w{0.0f};             // vt slot 34 (0x1F4558): the angular velocity
};

// The camera's state the target reads.
struct ACTORCMCTX
{
    bool fCut = false;             // CM +0x300 != 0
    bool fFocus = false;           // CM +0x260 is the actor
    float t = 0.0f, dt = 0.0f;     // g_clock
    float zOther = 0.0f;           // the +0x18C4 object's z (states 3 / 22)
};

ACTOR* PactorFromLo(LO* plo);     // null for objects outside the actor class 0x34
SLY* PslyFromLo(LO* plo);         // null outside 0x35
void InitActorBase(ACTOR* pa);    // 0x2039C0's +0x240 (and Sly's 0x1885B8)
SLY* PslyFromLo(LO* plo);         // null outside 0x35
ACTORPOSE PoseFromLo(LO* plo);    // native (the transform record; velocities by difference: TODO(physics))

// orig 0x1F58F0 (the SO's base: pose record)
void GetSoCpdefiBase(const ACTORPOSE& pose, float t, CPDEFI* pcpdefi);
// orig 0x141588 ActorGetFocus
glm::vec3 PosActorFocus(const ACTOR& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx);
// orig 0x18A350 (Sly's slot 97)
glm::vec3 PosSlyFocus(const SLY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx);
// orig 0x141640 ActorGetCmTarget (fSly: slot 97 is Sly's)
void GetActorCmTarget(ACTOR& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pcpdefi);
// orig 0x18A428 (Sly's slot 50)
void GetSlyCpdefi(SLY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pcpdefi);

// The camera's slot 50 for any focus (by the class's original function; others the base 0x1F58F0).
void GetLoCpdefi(LO* plo, CPDEFI* pcpdefi);

// orig 0x2052E0 (slot 38 of the 0x203948 family: placed at q's matrix, its +0x240 back; +0x1060 above it;
// velocities zeroed (TODO(physics): slots 35 / 36), +0x208 cleared)
void TeleportActor(LO* plo, LO* plodest);
void TeleportLo(LO* plo, LO* ploTo);     // orig 0x1A6B88 (vt slot 38; ALOs: TeleportActor), transition.cpp

// The actors of a new world (forgets the old ones).
void ResetActors();

// --actor-check TRACE --ram RAM: the reference's 0x18A428 calls (SLY2_FNTRACE root *18A428 with helpers 1F4558 and
// 22D348) recomputed from their entry RAM and compared with the CPDEFI and the actor fields at their exits.
int ActorCheck(const std::string& pathTrace, const std::string& ranges, const std::string& pathRam);
