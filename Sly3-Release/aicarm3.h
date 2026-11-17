#pragma once
// Sly 3's playable Carmelita, the AI job 0xE9 (orig3 0x15F7A8..0x161DF8: Init 0x15F7A8, Enter 0x15F7F0, Exit 0x15F898,
// Update 0x15FCA8, Message 0x15F918; the machine 0x160960, the pad 0x1600F0, the aim 0x15FCF8 / 0x15FD58, the gun
// 0x161380 / 0x1614E8): the two-player games' second player. Her class 0x3C (CARMELITA, a STEPGUARD) takes a pad by
// its vt slot 83 (0x162810: +0x130 the player index, +0x134 the pad record; her reaction job 0xC8 gets message 0x14 and
// reacts with kind 20: this job). She then moves by the pad in her player's camera: the guard's moves (JOBMOVE 0xC9 by
// state index) and the velocity the feet push toward (+0x1A0: the step's vecVel, set after the move's steering).
//
// Ported here: the machine's walk / run / stand / aim-walk states (0..4), the turn to the stick in the player's view,
// her gun (R1: the shot's ASEG +0x17D4, the charge SM +0x17DC / +0x17D8; the shot's event 0x2AA throws a projectile of
// her proxy +0x17EC at the target in front of her (0x161720: any PO but friendly guards) or straight ahead), the melee /
// kick moves (Square: 11, Circle: 18, Circle let go + Cross: the high jump 19), the aim toggle (Triangle), the jumps
// (Cross: 5, again in the air: the double jump 7; falling: 10; the strike job 0xCB's flight as the move of its state and
// the launch velocity) and the hits (message 12 after the guard's hit handler lowered her health: stunned 20 for 0.5 s,
// at 0 knocked out 14 with the broadcast 0x10; her knock-out clip's event 0x32B: the broadcast 12, the death).
// (The jumps / hits merged from s3-ai's job 0xE9 draft, 2026-11-12.) The power-ups (L1: 0x15FE68: 0x32 the super jump,
// 0x33 the charged shot, 0x34 state 0x11 to the robber, 0x35 the hit; taken by her vt slot 90 0x161BE0 from her shuffled
// four) are in (rounds 7..9). Not yet: the hang (15), message 15 / 8. State 16 (0x1612D0) is inside a sight-blocking
// shape (the Smoke Bomb's cloud), not water: ported. Her flashlight is off while a person plays her (round 9).

#include "ai.h"

#include <glm/glm.hpp>

#include <vector>

struct LO;
struct GUARD;
struct PADCTL;

constexpr CID CID_JOBCARM3 = 0x1E9;   // (Sly 3-only job ids above 0x1C6 natively: ai.h)

struct JOBCARM3 : AIJOB
{
    int st = -1;                  // +0x30 the machine's state (0x160960)
    float tSt = 0;                // +0x34
    int stGun = -1;               // +0x38 the gun's state (0x1614E8: -1 idle, 0 / 3 / 4 the shot, 1 charging, 2 charged)
    float tGun = 0;               // +0x3C
    int stAim = -1;               // +0x40 0 free, 1 aiming (0x15FD58)
    float tAim = 0;               // +0x44
    void* pasegaShot = nullptr;   // +0x48 the shot's ASEGA
    void* psmaCharge = nullptr;   // +0x4C the charge's SMA
    int fCircleUp = 0;            // +0x50 (state 18: Circle let go)
    int idAimPart = -1;           // +0x70 the aim HUD's part handle (0x262B60)
    int nZonePending = -1;        // (PC: the aim zone's forced state waiting for the camera's cut to pass)
    float tFloorLast = 0;         // (0x2503C8: the last time she stood on a floor)
    // (native) what the update wants after the move's steering: the feet's velocity (the heading's frame) and heading
    bool fSteer = false;
    glm::vec3 vecVel{0.0f};
    bool fHeading = false;
    float radHeading = 0.0f;

    ~JOBCARM3() override;
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;   // orig3 0x160960
    void SetAim(int n);              // orig3 0x15FD58
    void SetGun(int n);              // orig3 0x1614E8
    void UsePower();                 // orig3 0x15FE68 (L1: the held power-up)
    int Message(int msg, void* pm) override;   // orig3 0x15F918
    void Fire();                     // orig3 0x161610
    LO* PloShotTarget();             // orig3 0x161720
    PADCTL* Ppad() const;
    bool FInShape17() const;         // orig3 0x1612D0
    glm::vec3 posThrow{0.0f};        // +0x60 the power-up 0x34's place (0x15FB28)
    glm::vec3 PosThrowTarget();      // orig3 0x15FB28
    void ArriveThrow();              // orig3 0x15FBD8 (her clip's event 0x62 in state 0x11)
private:
    void UpdatePad();                // orig3 0x1600F0
    void UpdateGun();                // orig3 0x161380
};

// (guard.cpp UpdateGuard, after the move's steering 0x2097E0) the job's velocity and heading applied
void SteerCarm3(GUARD* pg);
// (split screen) the guard-class character lo takes player i's pad (orig3 slot 83 0x162810 with i; -1: given back)
void SetCarmPlayer3(LO* plo, int iplayer);
// the guard's player index (+0x130; -1 none)
int IPlayerCarm3(const LO* plo);
// a world loaded: no guard has a pad
void ResetCarm3();
// her flashlight off while a person plays her (her splice's option 1839 -> s1414 #f: the beam's groups hidden, its sight
// shapes out of the world), as the original's playable kinds do; Keep: again when a reset of hers showed it (each update)
void SetCarmFlashlightOff3(LO* plo);
void KeepCarmFlashlightOff3(LO* plo);

// orig3 0x161C60 / 0x1F4438 (natively per Carmelita): her held power-up (the guard's +0x18B0: 0x32 the super jump, 0x33
// the charged shot, 0x34 the throw, 0x35 the hit; -1 none) and its meter (SO +0x174, at most +0x178 = 100). The PC's Cops
// and Robbers rounds give them at the power-up spots (copsmap3.cpp); L1 uses it (0x15FE68).
void SetCarmPower3(LO* plo, int kind, int meter);
int KindCarmPower3(const LO* plo);
// orig3 0x161BE0 (her vt slot 90, a power-up pickup's option 947): while a pad drives her the next of her shuffled
// power-ups {0x32, 0x33, 0x34, 0x35} (+0x18AC), the meter full; the kind (-1: not a player)
int GiveCarmPowerup3(LO* plo);
// orig3 0x2BC238 (a shuffled set's next; cursor -1: not shuffled yet)
int NNextShuffled3(std::vector<int>& a, int& cursor);
// the robber the power-ups aim at (the original's global player; PC teams: the nearest robber in the world)
LO* PloRobberNearest(const LO* ploHer);
