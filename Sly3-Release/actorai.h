#pragma once

// Player-actor AI: the common path follower's input driver (A8), scripted walk (C1), companion controller (C2),
// and Murray's punch controller (C3). Original 0x144B28..0x1461C8; shared stack 0x1EEC60 and follower 0x177E80.
#include "ai.h"
#include "aijobs.h"

struct ACTOR;
struct PADCTL;

// Sly 3's 0xEF (vt 0x35D730, base 0xEE NEW_JOB_EE vt 0x35D850): Init 0x1D9010 (0x1D8008), Enter 0x1D9040 (0x1D8070: the
// ranges, state 0; the watch's next check reset 0x1573A0, its target 0x1572A0), Exit 0x1D8110 (the path let go), Update
// 0x1D9080 (0x1D89B0: the step 0x1D8140, then the state by the near / far radius: 0 moving, 1 near the steered point
// (natively st 2); no nav-way states), the drive 0x1D90B8 (state 0 only), arrived 0x1D8AC0 (no open-curve test), Follow
// 0x1D8B40 (the path from its start: s 0). The step 0x1D8140 is Sly 2's 0x177F78 line for line for the actor's own path
// (the same constants: look-ahead 100, {200, -0.5} in [0, 150], 500, springs 7.5 / 30, the push 200 / 10000, the turn
// 350 / 6000 with {0.5, -1} in [0, 1], the keep 1; the turn's side never flipped on an own path): Steer177F78.
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
    void Follow(const CRV* pcrv, int fReverse);  // orig3 0x1D8B40
};

// 0xC1: a scripted walk goal, followed by an A8 on the actor's own curve.
struct JOBACTORWALK : AIJOB
{
    int fRun = 0;                // +0x30
    float sArrive = 40.0f;       // +0x34
    int fWalk = 1;               // +0x38
    float uSpeed = 1.0f;         // +0x3C
    glm::vec3 posGoal{0.0f};     // +0x40
    int fSly3 = 0;               // (Sly 3's 0xF4: 0x157970's region rule in SetGoal)
    void Init() override;
    void Enter() override;
    void SetGoal(const glm::vec3& pos);
    bool FAtGoal() const;
};

// Sly 3's 0xF3 JOBPATHSTEP (orig3 vt 0x354378: Init 0x157508, Enter 0x157538, Update 0x157648; the step 0x157688, done
// 0x1577C0): a script's walk along a path (SCPT 0x33, 0x226D00 case 51) as steps of 200 along it, each walked by its child
// 0xF4 (JOBACTORWALK, natively with its A8 follower); the next step once the child arrived. Natively 0x33 ran one A8 on
// the whole curve (Sly 2's path step for actors is the follow job), which avoidance and the nav keep could hold off the path.
constexpr CID CID_JOBPATHSTEP3 = 0x1F3;
struct JOBPATHSTEP3 : AIJOB
{
    const CRV* pcrv = nullptr;   // +0x30 (the path object's curve)
    int fForward = 0;            // +0x34 (the command's w[1]: toward the curve's end)
    float s = -1.0f;             // +0x38 the step's place on the curve
    glm::vec3 pos{0.0f};         // +0x40 the step's point
    int fNoAvoid = 0;            // +0x58 (the run's +0x104)
    int fWalk = 0;               // +0x5C (w[2])
    float uSpeed = 1.0f;         // +0x60 (w[3])
    void Init() override;
    void Enter() override;
    void Update() override;
    void Step(bool fForce);
    bool FDone() const;
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
