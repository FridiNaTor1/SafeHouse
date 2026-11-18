#pragma once
// Sly 3's password check, the AI job 0xE7 (orig3 0x18B638..0x18C5A4): aipassword3.cpp.

#include "ai.h"

#include <array>
#include <cstdint>

struct LO;
struct FONTBRX;

// Native class id of the Sly 3-only job (the AI code keeps Sly 2's job ids 0x9B..0xC6: ai.h)
constexpr CID CID_JOBPASSWORD3 = 0x1E7;

struct PASSWORDROW { int idLine; std::array<int, 4> abtn; };

struct JOBPASSWORD3 : AIJOB
{
    LO* ploTarget = nullptr;      // +0x30 the disguised player
    int st = -1;                  // +0x34 -1 done, 0 stopped, 1 the start line, 2 the challenge, 3 the buttons shown,
                                  // 4 / 5 the answer, 6 judged, 7 / 8 right (the player's line, the guard's), 9 / 10
                                  // wrong, 11 broken off (the player left its state), 12 too slow
    float tSt = 0;                // +0x38
    int stFace = -1;              // +0x3C 0 turning to the player, 1 standing
    const PASSWORDROW* prow = nullptr;   // +0x44 the challenge's row
    int idLine = -1;              // +0x48 the line playing
    int cShown = 0;               // +0x50 the buttons shown so far
    float tShown = 0;             // +0x54
    int cAnswer = 0;              // +0x58
    std::array<int, 4> aAnswer{}; // +0x5C
    float tAnswer = 0;            // +0x6C the last answer (0.75 s to the next)
    int fPassed = 0;              // +0x70
    int fDone = 0;                // (0x238AB8)

    ~JOBPASSWORD3() override;
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;      // orig3 0x18BB20
    void SetFace(int n);                // orig3 0x18BF48
    void AddAnswer(int btn);            // orig3 0x18C5A8
private:
    void StopLine();
    void PlayLine(int id, LO* ploSpeaker);
    void PlayGuardLine(int i);
    bool FLineDone() const;
    void LockJobDone();
};

// The player's pad while a guard's check holds it (orig3 Sly's states 0x15 / 0x16): true: taken (the player stands);
// the face buttons pressed go to the check (po.cpp)
bool FTakePadForPassword3(LO* ploPlayer, uint32_t grfPressed);
// The guard checking the player (the player's +0x1454), null: none
LO* PloPasswordGuard3(LO* ploPlayer);
void ResetPassword3();
// orig3 0x1ADDD0 case 0x16: Sly's riding camera (0x16C608) on / off
void SetRideCamera3(LO* ploSly, bool fOn);
// orig3 0x1ADDD0 leaving Sly's 0x15 / 0x16 (not into 0x16): the check holding the player broken off (its state 11)
void BreakOffPassword3(LO* ploPlayer);
// (routes) the face button the check holding the player waits for next (0x10 triangle, 0x20 circle, 0x40 cross, 0x80
// square: the row it showed), 0: none (no check, or not answering yet)
int NPasswordButtonWanted3(LO* ploPlayer);
// (routes) the checks passed so far (a count)
int CPasswordsPassed3();
// The check's buttons (slot 15), drawn with the HUD (screenrt.cpp)
void DrawPasswordButtons3(const FONTBRX* pfont, const FONTBRX* picons);
