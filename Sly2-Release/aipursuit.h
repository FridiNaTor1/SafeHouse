#pragma once
#include <cfloat>

// The guards' pursuit of a seen object (aipursuit.cpp): the controller 0xA5 (states 0 around the target 0xB3, 1 an
// action 0xA4, 2 a shot 0xAD, 3 the chase 0xBA, 4 lost 0x9D, 5 the game paused 0x9E), the action 0xA4 striking with
// 0x9F, the action objects (cid 0x78: ranges, the kind entry they play) and the per-target records the pursuing
// guards share (+0x3CC). Part of the animation / AI code.

#include "ai.h"
#include "aijobs.h"
#include "crv.h"
#include "guard.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

struct NAVREGION;

// A pursued object's record (orig 0xAC bytes, 0x20A5F8, listed at 0x3E145C)
struct TARGETREC
{
    float t0 = 0.0f;              // +0x0 when the first guard came
    float t4 = 0.0f;              // +0x4 the timer's time (0x20A9A8)
    int n8 = -1;                  // +0x8 the timer's value (1 seen, 2 lost but near, 3 an action, 4 a shot, 5 hit)
    LO* plo = nullptr;            // +0xC the object
    int nMode = -1;               // +0x10 (0 the camera's heading, 1 the object's facing)
    float g14 = 1.0f, g18 = 2.0f; // +0x14 / +0x18 (+0x18: the angle the guards may act within)
    int f1C = 0;                  // +0x1C (no action)
    std::vector<GUARD*> apguard;  // +0x28 / +0x2C the guards pursuing it
};
TARGETREC* PtargetrecOf(LO* plo);   // orig 0x1F8278
TARGETREC* PtargetrecFind(LO* plo);  // orig 0x1F8240 (none: null)
void ResetTargetRecs();
void SetTargetrecTimer(TARGETREC* pt, int n);         // orig 0x20A9A8
bool FTargetrecExpired(const TARGETREC* pt);          // orig 0x20A7E0
void AddTargetrecGuard(TARGETREC* pt, GUARD* pg);     // orig 0x20A730
void RemoveTargetrecGuard(TARGETREC* pt, GUARD* pg);  // orig 0x20A790
float RadTargetrec(const TARGETREC* pt);              // orig 0x20A690
bool FTargetrecAllows(const TARGETREC* pt, GUARD* pg);  // orig 0x20A818
void SetGuardPursued(GUARD* pg, LO* plo);             // orig 0x208540
void RefreshGuardTargetrec(GUARD* pg);                // orig 0x206900's end

// An action object (cid 0x78, the guard's +0xEDC list; options 353..364)
struct ACTIONOBJ
{
    LO* plo = nullptr;
    // (the defaults of its InitLo 0x20AD38)
    glm::vec2 r50{0.0f, 300.0f};       // +0x50 the horizontal distance range (353)
    glm::vec2 r58{-5000.0f, 5000.0f};  // +0x58 the height range (354)
    glm::vec2 r60{0.2f, 0.5f};         // +0x60 the wait after it (355)
    float g68 = 0.17453295f;           // +0x68 the angle it reaches (356, degrees; 10)
    float g6C = 3.4028235e38f;    // +0x6C (360)
    int f70 = 0, f74 = 0, f78 = 0;  // +0x70 clear line (359), +0x74 (361), +0x78 concurrent response (358)
    int fA8 = 1;                  // +0xA8 enabled (363)
    int aimRef = -1, aimEndRef = -1, aimMarker = -1; // +0x7C/+0x80/+0x84 (362)
    XF* pxfAim = nullptr;         // +0xA0: rotation record aimed while the action plays
    glm::vec3 vecAim{0.0f};       // +0x90: authored direction at the marker
    KINDE kinde;                  // +0xC0 what it plays (364 {oid, grf})
};
ACTIONOBJ* PactionOf(LO* plo, GUARD* pg);
void PostLoadGuardAction(LO* plo); // orig: 0x0020AE18
// orig: 0x001758A0
ACTIONOBJ* PactionSelect(GUARD* pg, LO* ptarget, TARGETREC* pgate);
// orig: 0x0020AB68 / 0x0020AC28 / 0x0020ACC0
void SetGuardResponse(GUARD* pg, ACTIONOBJ* action, XF* targetFrame, const glm::vec3& targetPos);
void SetGuardActionAim(GUARD* pg, ACTIONOBJ* action, XF* targetFrame, const glm::vec3& targetPos);
void UpdateGuardActionAim(GUARD* pg);

// 0xA5 (0x60): the pursuit
struct JOBPURSUE : AIJOB
{
    LO* ptarget = nullptr;        // +0x30
    int st = -1;                  // +0x34
    float tSt = 0;                // +0x38
    ACTIONOBJ* paction = nullptr; // +0x3C
    int f40 = 0;                  // +0x40
    NAVREGION* pregionGoal = nullptr;  // +0x44
    glm::vec4 posGoal{0.0f};      // +0x50
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetPursueState(int n);   // orig 0x176458
    bool FCanShoot();             // orig 0x175D00
    bool FTryAction();            // orig 0x175FF0
    bool FLost();                 // orig 0x176060
};
// 0xA4 (0x50): an action
struct JOBACT : AIJOB
{
    LO* ptarget = nullptr;        // +0x30
    int st = -1;                  // +0x34
    float tSt = 0;                // +0x38
    ACTIONOBJ* paction = nullptr; // +0x3C
    int fHit = 0;                 // +0x40
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    int Message(int msg, void* pm) override;
    void SetActState(int n);      // orig 0x175488
};
// 0x9F (0x60): the strike
struct JOBSTRIKE : JOBMOVE
{
    int f50 = 0;                  // +0x50 a kind-2 entry's target kept on the face object (0x175258)
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    int Message(int msg, void* pm) override;
};
// 0xB3 (0x40): around the target
struct JOBSURROUND : AIJOB
{
    int st = -1;                  // +0x30
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetSurroundState(int n); // orig 0x17C550
};

// 0xAD (0x40): the shot (Init 0x17B590, Enter 0x17B5C0, Exit 0x17B5F0, Update 0x17B620, Message 0x17B690): state 0
// plays the guard's state 0x11 (0x9D facing the target: the gun's clip, whose events fire), -1 once it ends; the clip's
// kind-9 events 0x31 / 0x32 set / clear the guard's +0xF60
struct JOBSHOOT : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    LO* ptarget = nullptr;        // +0x38
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    int Message(int msg, void* pm) override;
    void SetShootState(int n);    // orig 0x17B750
};

std::unique_ptr<AIJOB> PjobNewPursuit(CID cid);

// A player's vt slot 77 (0x140F70: its +0xDF0 controller's status: 0 free, 1 / 2 busy (hit, held); 0x32's 0x1C3ED0: 0).
// The value a harness imported (SetPlayerStatus77: its players are the reference's), else the native actor's
// (0x140F70 registered); ClearPlayerStatus77 when the players run natively.
int NPlayerStatus77(LO* plo);
void SetPlayerStatus77(LO* plo, int n);
void ClearPlayerStatus77();

// 0xB4 (0x40): the boss's (class 0x3B) fight reaction (reaction 13: Init 0x155560, Enter 0x1555E0, Exit 0x155598,
// Update 0x155660, SetState 0x155788): after a loss of 90 hit points since its last escape (+0x1018), it escapes to
// the record whose nearer end is farthest from the target (0x155868) with 0xB5 (state 0), then pursues (0xA5, state 1)
// once the target is within 850 (|dz| < 400) and the escape job has left its jump; else it pursues at once.
struct JOBB4 : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    LO* ptarget = nullptr;        // +0x38
    void* prec = nullptr;         // +0x3C the record (GUARD::BOSSREC)
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetB4State(int k);
};
// 0xB2 (0x50): the guard busy with objects (option 426's lists, reaction 12; module 0x17B178..0x17BED0, Init 0x17B808,
// Enter 0x17B840, Exit 0x17B870, Update 0x17BD78, SetState 0x17B8B0, the choice 0x17B9C0): every 1..2 s the target is
// the nearest list-2 object (within 800 and near the way to the best list-1 object) else the best list-1 object (by
// distance, the previous one favoured, the list-2 objects near the way penalised); 0 standing (0x9E), 1 the pursuit of
// the target (0xA5), 2 standing (0x9E kind 1) while the guard is held still. E.g. the Ep1 heist siege's rats (1911:
// option 426 (the truck's breakable 123957) (Sly)) attack the truck
struct JOBB2 : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    LO* ploPreferred = nullptr;   // +0x38
    LO* ploNear = nullptr;        // +0x3C
    LO* ploSelected = nullptr;    // +0x40
    float tChoose = 0;            // +0x44
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetB2State(int n);
    void Choose();
};
// 0xB5 (0x60): the boss's escape (Init 0x1559B0, Enter 0x155A38, Exit 0x1559F8, Update 0x155D90, SetState 0x155AE0):
// 0 the jump away (0x9D kind 11) then the move to the record's end, 1 facing the target (0x9E) until an action is
// selected (0x1758A0, after 2.5 s), 3 the action (0xA4), then 2 the walk to the record's other end (0xA7) when the
// target moved into that end's zone (0x155A68)
struct JOBB5 : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    void* prec = nullptr;         // +0x38
    float g3C = FLT_MAX;          // +0x3C
    LO* ploPoint = nullptr;       // +0x40 the current end
    float t44 = 0, t48 = 0;       // +0x44 / +0x48
    LO* ptarget = nullptr;        // +0x4C
    int cAction = 0;              // +0x50
    struct ACTIONOBJ* paction = nullptr;  // +0x54
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetB5State(int k);
    LO* PloEnd();                 // orig 0x155A68
};

