#pragma once

// Player-actor AI: the common path follower's input driver (A8), companion controller (C2), and Murray's
// ordinary punch input controller (C3). Original 0x144B28..0x1461C8; shared stack 0x1EEC60 and follower 0x177E80.
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

struct JOBMURRAYATTACK : AIJOB
{
    int st = -1;                 // +0x30: 0 slow approach, 1 approach, 2 punch, -1 complete
    float tSt = 0.0f;            // +0x34
    LO* ploEnemy = nullptr;       // +0x38
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;
    void Drive(PADCTL* pc) override;
};

std::unique_ptr<AIJOB> PjobNewActor(CID cid);
void SetActorFollow(ACTOR* pa, LO* target, const glm::vec3& offset);  // 0x142540
void SetActorWatchTarget(ACTOR* pa, const glm::vec3& pos);            // 0x1448F0
void UpdateActorWatch(ACTOR* pa);                                   // 0x144640
