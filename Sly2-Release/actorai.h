#pragma once

// Player-actor AI: the common path follower's input driver (A8), scripted walk (C1), companion controller (C2),
// and Murray's punch controller (C3). Original 0x144B28..0x1461C8; shared stack 0x1EEC60 and follower 0x177E80.
#include "ai.h"
#include "aijobs.h"

struct ACTOR;
struct PADCTL;

struct JOBACTORFOLLOW : JOBFOLLOW
{
    PADCTL* pcCopy = nullptr;      // +0xC0 the followed player's controller
    uint32_t grfCopy = 0;         // +0xC4
    float dtCopy = 0.0f;          // +0xC8
    int fWalk = 0;                // +0xCC
    float uSpeedActor = 1.0f;     // +0xD0 (A7's +0xC4 animation speed is not used by A8)
    void Init() override;
    void Enter() override;
    void Update() override;
    void SetState(int n) override;
    void Drive(PADCTL* pc) override;
};

// 0xC1: a scripted walk goal, followed by an A8 on the actor's own curve.
struct JOBACTORWALK : AIJOB
{
    int fRun = 0;                // +0x30
    float sArrive = 40.0f;       // +0x34
    int fWalk = 1;               // +0x38
    float uSpeed = 1.0f;         // +0x3C
    glm::vec3 posGoal{0.0f};     // +0x40
    void Init() override;
    void Enter() override;
    void SetGoal(const glm::vec3& pos);
    bool FAtGoal() const;
};

struct JOBCOMPANION : AIJOB
{
    LO* ploFollow = nullptr;      // +0x30
    glm::vec3 offset{0.0f};       // +0x40
    glm::vec3 posFollow{0.0f};    // +0x50
    int st = -1;                 // +0x60
    float tSt = 0.0f;            // +0x64
    float dtPredict = 0.25f;     // +0x68
    float sNear = 250.0f, sFar = 350.0f;  // +0x6C / +0x70
    float svNear = 1.0f, svFar = 1.2f;    // +0x74 / +0x78
    glm::vec3 posRecover{0.0f};   // +0x80
    LO* ploEnemy = nullptr;       // +0x90
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;
    LO* FindEnemy();
    bool FNeedRecover();
    void PlanFollow();
};

// The companions' attack jobs (C3 Murray 0x145CD0.., C4 Bentley 0x1461C8.., C5 Sly 0x1466C0..): one enemy, the
// approach / strike states driven as pad input; JOBCOMPANION case 2 ends on st == -1 (0x145DA0 / 0x146298 / 0x146790).
struct JOBCOMPANIONATTACK : AIJOB
{
    int st = -1;                 // +0x30: -1 complete
    float tSt = 0.0f;            // +0x34
    LO* ploEnemy = nullptr;       // +0x38
    void Init() override;
    void Enter() override;
    void Exit() override;
};

// C3: 0 slow approach, 1 approach, 2 punch
struct JOBMURRAYATTACK : JOBCOMPANIONATTACK
{
    void Update() override;
    void SetState(int n) override;
    void Drive(PADCTL* pc) override;
};

// C4 (vtable 0x2CC2C0): Bentley's swing, as C3 with his own ranges (D_002BD350 / D_002BD358) and +0x1714
struct JOBBENTLEYATTACK : JOBCOMPANIONATTACK
{
    void Update() override;
    void SetState(int n) override;
    void Drive(PADCTL* pc) override;
};

// C5 (vtable 0x2CC240): Sly: 0 face, 1 approach, 2 cane strike, 3 jump, 4 the dive (Square in the air)
struct JOBSLYATTACK : JOBCOMPANIONATTACK
{
    void Update() override;
    void SetState(int n) override;
    void Drive(PADCTL* pc) override;
};

// Murray's rage (class 0xC6, vtable 0x2CC1C0; Operation: Trojan Tank's hypno-boxes, method 940 0x1B9CA0): his
// Berserker Charge (state 12) again and again at Sly, steering toward him (R26)
struct JOBMURRAYRAGE : AIJOB
{
    float radCharge = 0.0f;      // +0x30 the heading the charge started on
    void Enter() override;
    void Exit() override;
    void Update() override;
};
// orig: 0x001B9CA0 (method 940 of Murray): his job stack cleared; f: the rage job pushed and entered
void SetMurrayRage(ACTOR* pa, bool f);

std::unique_ptr<AIJOB> PjobNewActor(CID cid);
void SetActorFollow(ACTOR* pa, LO* target, const glm::vec3& offset);  // 0x142540
void SetActorWatchTarget(ACTOR* pa, const glm::vec3& pos);            // 0x1448F0
void UpdateActorWatch(ACTOR* pa);                                   // 0x144640
