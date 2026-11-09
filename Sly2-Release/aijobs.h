#pragma once

// The job classes the guards' patrols and reactions use (ai.h). Offsets: the original's in the job.

#include "ai.h"
#include "crv.h"
#include "guard.h"

#include <glm/glm.hpp>

struct KINDE;
struct NAVPOLY;

// 0xBB (0x30: 0x17D1C0 / 0x17D1F0 / 0x17D280): hanging on a bar (GUARD +0xF80 2), state 0x28; 0xBC (0x17D2B0 /
// 0x17D2E0 / 0x17D330): on a 0x27 (+0xF80 3), state 0x29. Both stand (a 0x9D move, state index 2); 0xBB also turns the
// heading's frame to the bar's x row
struct JOBHOLD : AIJOB
{
    int nState = 0x28;
    void Init() override;
    void Enter() override;
    void Exit() override;
};

// 0xB8: follow the rail attached at GUARD +0xF84 (0x17CAC8..0x17CE30).
struct JOBRAIL : AIJOB
{
    int st = -1;
    float tSt = 0;
    float uSpeed = 1;
    float sGoal = 0;
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetRailState(int n);
    void SetDirection(int forward);
    bool FDone() const;
};

// 0xB9: climb the ladder / pipe attached at GUARD +0xF84 (kind 1, 0x17CE68..0x17D188): its place +0xF88 moved toward
// the goal end at the move's speed (+0x198, eased), the heading's frame the pipe's there (0x1C3500)
struct JOBLADDER : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    float sGoal = 0;              // +0x38
    float gSpeed = 0;             // +0x3C
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetLadderState(int n);   // orig 0x17D058
    void SetDirection(int forward);  // orig 0x17D110
    bool FDone() const;           // orig 0x17D188
};

// A goal (orig 0x2562A8: 0x20 bytes {path, object, , point}); 0x1C1EC0 gives its point seen from a position
struct GOAL
{
    const CRV* pcrv = nullptr;    // +0x0 a path (its nearest point)
    LO* plo = nullptr;            // +0x4 an object (its position)
    glm::vec4 pos{0.0f};          // +0x10 a fixed point
};
// orig: 0x001C1EC0
glm::vec3 PosGoal(const GOAL& goal, const glm::vec3& posFrom);

// 0x9C (0xB0): the reactions (Init 0x1766D8, Enter 0x176708, Update 0x177618, Message 0x176BE8)
struct JOBREACT : AIJOB
{
    AIMSG msg;                    // +0x30 (0x70)
    float tMsg = 0;               // +0xA0
    float tWait = 0;              // +0xA4 (message 17's wait)
    void Init() override;
    void Enter() override;
    void Update() override;
    int Message(int msg, void* pm) override;
    void React(const AIMSG& m);   // orig 0x176750
    void NextReaction();          // orig 0x177BA0 (aihit.cpp)
    void SendHit(int n, LO* ploSender);  // orig 0x176B80
};

// 0x9D (0x50): a move: its animation index in the mask, its kind entry played (Init 0x174920, Enter 0x174950, Exit
// 0x174B28, Update 0x174B68, Steer 0x174F08, State 0x174EA0, Message 0x174BD0)
struct JOBMOVE : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    LO* ploFace = nullptr;        // +0x38 an object to face at the end (0x9E: the record hold flag)
    int nAnim = 0;                // +0x3C the state index
    KINDE* pkinde = nullptr;      // +0x40 the entry (null: the best for the mask)
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void Steer() override;
    int State() override;
    int Message(int msg, void* pm) override;
    void SetMoveState(int n);     // orig 0x1749C0
};
// orig: 0x00209518: the animation currently played by an entry.
ASEGA* PasegaOfKinde(const KINDE* pe);
// 0x9E (0x50): standing (Init 0x174FC8, Enter 0x174FF8, Exit 0x175090, Message 0x1750E0)
struct JOBSTAND : JOBMOVE
{
    void Init() override;
    void Enter() override;
    void Exit() override;
    int Message(int msg, void* pm) override;
};

// 0xAF (0x60): asleep (Bentley's dart; Init 0x17A4B0 nAnim 0x10, Enter 0x17A4E8, Exit 0x17A558, Update 0x17A670,
// SetSleepState 0x17A5B0): 0 asleep until +0x58 (now + GRandInRange(+0xE7C, +0xE80)), 1 waking (the move's end)
struct JOBSLEEP : JOBMOVE
{
    int stSleep = -1;             // +0x50
    float gSt = 0;                // +0x54
    float tWake = 0;              // +0x58
    struct NOISE* pnoise = nullptr;  // (the guard's +0x35C: the sleep's noise, SW +0x4024's template 707)
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetSleepState(int n);
};

// The owner's launch target (actor vt slot96: 0x143220, Sly 0x18DE80).
struct LAUNCHTARGET
{
    int fActive = 0;              // +0x0 whether the owner still supports tracking
    float sRadius = 0.0f, dz = 0.0f;  // +0x4 / +0x8
    glm::vec3 pos{0.0f}, vel{0.0f};  // +0x10 / +0x20
};

// 0xB1 (0x70): a launched guard, then the contextual strike's target (0x17A9D0..0x17B578).
struct JOBB1 : JOBMOVE
{
    int stLaunch = -1;            // +0x50
    float tLaunch = 0.0f;         // +0x54
    int nLaunch = 0;              // +0x58 repeated launch marker
    LO* ploOwner = nullptr;       // +0x5C the attacking actor
    int fTrack = 0;               // +0x60
    struct CTRLTARGET* pctrlTarget = nullptr;  // +0x64 the launch's timed translation
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    int State() override;
    int Message(int msg, void* pm) override;
    void SetState(int n) override;  // orig 0x17B1A8
    void GetTarget(const LAUNCHTARGET& target, glm::vec3* ppos, glm::vec3* pvel);  // 0x17AA78
    void UpdateTarget();            // orig 0x17ABC8
};

// 0xB0 (0x40): held (reaction 6: Murray's pickup 0x209E28 at his carry phase 1, Bentley's Health Extractor 0x225660
// (holder 0), which sets 4 within 100 of its source (0x225300); Init
// 0x17A6F8 the guard's state 0x16, Enter 0x17A730, Exit 0x17A7B8, Update 0x17A7F8, SetState 0x17A8C8): a move child
// 0x9D per state with the animation {18, 19, 20, 21, 19} (its state bit): 0 grabbed, 1 carried (0x209E88), 2 let go
// (0x209EE0), 3 thrown / put down (0x209FB8), 4 (0x225300's hold); 2 / 3 end (-1) on the floor; +0x32C = state < 2
struct JOBB0 : AIJOB
{
    int stHeld = -1;              // +0x30
    float tHeld = 0.0f;           // +0x34
    float tNoise = 0.0f;          // +0x38 the last carried noise
    LO* ploHolder = nullptr;      // +0x3C (the reaction's +0x40: the message's argument)
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;  // orig 0x17A8C8
    bool FDone() const { return stHeld == -1; }  // orig 0x17A9C0
};

// 0xAC (0xA0): the use of an object (a command list's op 13: a chair (13), a ladder (11), a rope (0x27), a bar (0x13 of
// kind 3), else the ground under it): its place and orientation, the move's animation with the state bits for the
// kind (Init 0x179F70 nAnim 15, Enter 0x179FA0, Exit 0x17A3D0, Update 0x17A400)
struct JOBUSE : JOBMOVE
{
    glm::vec3 pos{0.0f};          // +0x50 where
    LO* ploTarget = nullptr;      // +0x60 the object (null after the ground's case)
    int fReverse = 0;             // +0x64
    int nExtra = 0;               // +0x68
    int fBasis = 0;               // +0x6C its orientation +0x70 is set
    glm::mat3 mat{1.0f};          // +0x70
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
};

// 0xAE (0x40): class 0x3C's command list near Sly (Init 0x1BA560, Enter 0x1BA598, Exit 0x1BA5C8, Update 0x1BA608,
// SetState 0x1BA828): 0 running the list (0xA9 from its saved place) while Sly is inside the detector's second volume,
// 1 watching him (0x9E facing him) until he is inside the view volume again (0) or +0x1088 s pass (2: the alarm's
// animation 0x9D with +0x1060, the tasks' event 0x33)
struct JOBPROX : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    LO* plist = nullptr;          // +0x38 the command list (the guard's +0x108C)
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetProxState(int n);
};

// 0xBE (0x50): across a link between navigation regions (navlink.h; Init 0x17E3A0, Enter 0x17E3D8, Exit 0x17E410,
// Update 0x17E458, SetState 0x17E6B0, next record 0x17E580): to each of the link's records after the first in turn;
// state 0 walking there (kind 0: a straight follow 0xA7 with a deadline; kind 1: the use job 0xAC), 1 the way-point
// job 0xBF (the deadline passed, or not within 25 after the use), -1 done
struct NAVLINK;
struct JOBLINK : AIJOB
{
    NAVLINK* plink = nullptr;     // +0x30
    int irec = 0;                 // +0x34 the record walked to
    int st = -1;                  // +0x38
    float tSt = 0;                // +0x3C
    float tEnd = 0;               // +0x40 the walk's deadline
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;
    void NextRecord();            // orig 0x17E580
};

// 0xBF (0x50): to a way-point (Init 0x17E888, Enter 0x17E8B8, Exit 0x17E8E8, Update 0x17E918, Start 0x17E9B0, done
// 0x17EAE8): the point dropped on the floor (else snapped up to a surface), walked to by the use job 0xAC (+0xFE8 bit 1)
// or put there
struct JOBNAVPOINT : AIJOB
{
    GOAL goal;                    // +0x30
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void Start();                 // orig 0x17E9B0
    bool FDone() const;           // orig 0x17EAE8
};

// 0xA7 (0xF0): the path follower (Init 0x178E98 (0x177E80), Enter 0x177EF0, Update 0x178EE0 (0x1789F8, steering
// 0x177F78), SetState 0x1791C8)
struct JOBFOLLOW : AIJOB
{
    const CRV* pcrv = nullptr;    // +0x30 the path
    float s = 0;                  // +0x34 the distance on it
    int fReverse = 0;             // +0x38
    int fOpen = 0;                // +0x3C an open path (its end +0x44 / +0xB0 the goal)
    int fNoAvoid = 0;             // +0x40
    float sEnd = 0;               // +0x44
    float sStart = -1.0f;         // +0x48
    int st = -1;                  // +0x4C
    float tSt = 0;                // +0x50
    const CRV* pcrvOther = nullptr;  // +0x54
    const NAVPOLY* ppolyOther = nullptr;
    int nWay = 0;                 // +0x58 the search's answer (0x1BD718)
    float sOther = 0;             // +0x5C
    float g60 = 0;                // +0x60
    float sArrive = 100.0f, sNear = 25.0f, sFar = 60.0f;  // +0x64 / +0x68 / +0x6C
    float sTarget = 0;            // +0x70
    glm::vec4 pos{0.0f};          // +0x80 the steered point
    glm::vec4 vel{0.0f};          // +0x90
    int fFirst = 1;               // +0xA0
    glm::vec4 posEnd{0.0f};       // +0xB0
    int kMove = 9;                // +0xC0 the move's state index
    float uSpeed = 1.0f;          // +0xC4
    LO* ploArg = nullptr;         // +0xC8 (the go-to's +0xA8: faced at the end)
    glm::vec4 posMoved{0.0f};     // +0xD0
    float tLeft = 1.5f;           // +0xE0
    void Init() override;
    void Enter() override;
    void Update() override;
    void SetState(int n) override;
    void Steer177F78();           // orig 0x177F78
    void Update1789F8();          // orig 0x1789F8
    bool FArrived() const;        // orig 0x178D00
    void Follow(const CRV* pcrv, int fReverse);  // orig 0x178D90
    bool FStuck() const;          // orig 0x1792E8
};

// 0xA9 (0x60): a command list run (Init 0x171228, Exit 0x171398, Update 0x1713E8, SetState 0x1715F0)
struct JOBSCRIPT : AIJOB
{
    CMDLIST* pcmdlist = nullptr;  // +0x30
    LO* ploNear = nullptr;        // +0x34
    CMD* pcmd = nullptr;          // +0x38 the current command
    float tCmd = 0;               // +0x3C
    int fRunning = 0;             // +0x40
    const SAVEDSCRIPT* psaved = nullptr;  // +0x44
    float radFace = 0;            // +0x48
    int fFromReact = 0;           // +0x4C
    KINDE kinde50;               // +0x50 the command-14 entry; its move child keeps a pointer to this member
    void Init() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;  // (the command: natively Execute)
    void Execute(CMD* pcmd);      // orig 0x1715F0
    void SetTarget(CMDLIST* pl, int fFromReact, const SAVEDSCRIPT* psaved);  // orig 0x171258
    bool FDone() const { return !pcmd; }  // orig 0x171458
    bool FRunning();              // orig 0x171468
    LO* PloNear(OID oid);         // orig 0x1715B8
    void Save(SAVEDSCRIPT* p);  // orig 0x171C18
    void SeekMarker(OID oid);  // orig 0x171C98
};

// 0xAA (0x50): waiting between patrols (Init 0x179A58, Enter 0x179AA0, Exit 0x179B60, Update 0x179BA0)
struct JOBWAIT : AIJOB
{
    float tA = 3.4028235e38f;     // +0x30
    float tB = 3.4028235e38f;     // +0x34
    int st = -1;                  // +0x38
    float tSt = 0;                // +0x3C
    CMDLIST* pcmdlist = nullptr;  // +0x40
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetWaitState(int n);     // orig 0x179D30
};

// 0xBA (0xD0): a go-to (Init 0x17D360, Enter 0x17D3B0, Exit 0x17D408, Update 0x17D6C8, SetState 0x17D450)
struct JOBGOTO : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    int nAnim = -1;               // +0x38
    GOAL goal;                    // +0x40
    GOAL goalCur;                 // +0x60
    GOAL goalFallback;            // +0x80
    float tSearch = -3.4028235e38f;  // +0xA0
    int nSerial = 0;              // +0xA4
    LO* plo = nullptr;            // +0xA8 (the move's argument)
    int fAC = 0;                  // +0xAC
    float gB0 = 0;                // +0xB0
    std::vector<void*> anode;     // +0xB4 / +0xB8 the found path's nodes (0x1C21D8)
    int fDirect = 0;              // +0xBC
    int fC0 = 0;                  // +0xC0
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;
    void Target17D8A8();          // orig 0x17D8A8
    void Search();                // orig 0x17DA28
    int NArrived() const;         // orig 0x17DCB8
    bool FStuck() const;          // orig 0x17DCF8 (states 1 / 2 with the follower in its state 3)
    void SetGoal(const GOAL& g);  // orig 0x17DD38
};

// 0xB7 (0x70): the guard's station (reaction 16: a script's 0x103F40 sets the guard's +0xF68 object and +0xF70 point
// in its frame; Init 0x17C638, Enter 0x17C6A0, Exit 0x17C670, Update 0x17C6D0, SetState 0x17C928): 0 going to the
// point (0xBA with the object), its goal moved when the object moves more than 250; 1 hurrying after the object (0xBA
// with the move 0x1C) while it is farther than 700 (from 1000); 2 facing it while the guard is held (0x209098)
struct JOBSTATION : AIJOB
{
    LO* plo = nullptr;            // +0x30 the station object
    glm::vec4 pos{0.0f};          // +0x40 the point in its frame
    int st = -1;                  // +0x50
    float tSt = 0;                // +0x54
    glm::vec3 posLast{0.0f};      // +0x60 the object's position at the last goal
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;
};

// 0xBD (0x50): kept away from an object (reaction 0x12: Guttural Roar's mode 0x15 with the current player, or a
// target of the +0x3C0 classes; Init 0x17DD70 state 0x2B, Enter 0x17DDA8, Exit 0x17DDE8, Update 0x17DE50, SetState
// 0x17E278, the point 0x17E0E0): 0 the alerted move facing it, 1 running to the walkable point farthest from it, 2
// standing while held still
struct JOBBD : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    LO* ploFrom = nullptr;        // +0x38 (the reaction's object)
    float tChoose = 0;            // +0x3C the point's last choice
    float tLeft = 0;              // +0x40 (the reaction's amount: FLT_MAX for the roar)
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;
    void ChooseAway();            // orig 0x17E0E0
};

// 0xC0 (0x60): steps along a path (Init 0x17EB48, Enter 0x17EB88, Exit 0x17EC80, Update 0x17ECB0)
struct JOBPATHSTEP : AIJOB
{
    const CRV* pcrv = nullptr;    // +0x30
    LO* ploPath = nullptr;        //   (its object: the AI record 0x1E9C30)
    int fForward = 0;             // +0x34
    float s = -1.0f;              // +0x38
    glm::vec4 pos{0.0f};          // +0x40
    LO* plo50 = nullptr;          // +0x50
    int n54 = 0;                  // +0x54
    int fFromReact = 0;           // +0x58
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void Step(bool fForce);       // orig 0x17ECF0
    bool FDone();                 // orig 0x17EE78
    bool FAtEnd() const;          // orig 0x17EEB8
};

// the factory's native kinds (aijob.cpp's PjobNew)
std::unique_ptr<AIJOB> PjobNewKind(CID cid);

// orig 0x173610: pos dropped onto the still solid entries under it (the line +gHigh .. +gLow up; unless fAny, not
// steeper than 0.7): false when nothing is hit
bool FDropOnGround(glm::vec3* ppos, float gHigh, float gLow, bool fAny);
