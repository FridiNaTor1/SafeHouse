#pragma once

// Sly 3's challenge manager (orig3 0x4BC230, functions 0x283BB0..0x2852B0): a game started from the episode chooser's
// lists (the Two Player games of episode 0, an episode's Jobs && Challenges) runs on a fresh copy of the progress and
// gives the save back when it ends. The game is a class-0xAA object (dvd_menu's; each world that hosts it has a copy):
// its mode (+0x50, EOPID 1429) 0 a Master Thief Challenge, 1 a job replay, 2 a two-player game.
//
// The rules (s3-ui round 2): a value 0..1 (+0x1C): a timed challenge (1448: +0x9C s) runs from 1 down at -1 / +0x9C a
// second, a score challenge (1449: +0xA0) counts points / 1000 toward its goal (+0x84); the node's commands (1451..1453:
// on a broadcast of a type, maybe of an object / a class, a command (0x284A90) with a value and a line) and the scripts'
// (bif 125 option 759) change it, count tries down, fail or complete the job. The HUD 1054 (part 995, the score's 997,
// the timer's 996 + the dial 1000) shows it while a challenge runs. At the job's end the result (EvaluateChallenge3:
// 2 out of tries, 3 short of the goal, 4 done, 6 a new record (kept in the key +0xA8), 5 a two-player end) shows on
// 1066 (jobend3.cpp: the challenge stamp 0x43E + 2 k, strings 1110..1112, then Square replays / Triangle returns).
//
// Start (0x283BB0): the save copied aside (+0x34..: the world, GAMESAVE, GAME +0x10's block), the progress's task
// fields cleared (0x21C860), the game's "done" key (+0xB0) kept, for a two-player game (mode 2) every power-up taken
// (SW +0x4088's 0x43 keys), GAME +0x14A4 = n, then its task requested in its world (0x189E70 with the flags | 0x80) and
// the manager in state 1 (0x283F20).
// End (0x283F70, from the job end's close 0x1874E8 and the job quit): the settings / coins kept, the copied save back,
// dvd_menu loaded at point 0x1A8 (the chooser comes back on the list: GAME +0x1488 = 11).

#include "lo.h"

#include <vector>

struct SW;

// A class-0xAA game object's data (orig3 its fields from the options, in their order: the reset 0x2830D0's defaults,
// the setters 1448 / 1449; see challenge3.cpp NodeFromLo3)
struct NODE3
{
    OID oid = -1;
    int mode = 0;             // +0x50 (1429)
    OID oidJob = -1;          // +0x48 (1428)
    OID oidTask = -1;         // +0x5C (1432)
    int iworld = -1;          // +0x58 (1433)
    OID oidKeyTask = -1;      // +0x60 (1434): the task whose state +0x64 ends the game (-1: the job's current one)
    int value = 2;            // +0x64 (1435)
    int lineFail = -1;        // +0x6C (1437)
    float gStart = 0.0f;      // +0x78 (1439)
    float dgStart = 0.0f;     // +0x7C (1440)
    float gScaleStart = 1.0f; // +0x80 (1441)
    float gGoal = 0.0f;       // +0x84 (1442)
    int cStart = 1;           // +0x88 (1443)
    OID oidGroup = -1;        // +0x8C (1444)
    float gFactor = 1.0f;     // +0x90 (1445)
    bool fTimer = false;      // +0x94 (1446)
    float dyHud = 0.0f;       // +0x98 (1447)
    float dtLimit = 0.0f;     // +0x9C (1448)
    float gScore = 0.0f;      // +0xA0 (1449)
    float gBest = 0.0f;       // +0xA4 (the key +0xA8's value)
    std::vector<int16_t> akeyBest, akeyDone;   // +0xA8 (1456), +0xB0 (1457)
    struct ENTRY { int type = -1; OID oidKey = -1; int cidKey = -1; int kind = -1; float g = 0.0f; int line = -1; };
    std::vector<ENTRY> aentry;   // +0xB8 / +0xBC (1451..1453, 64 at most)
};
NODE3 NodeFromLo3(const LO* plo);
// orig3: 0x00283550: the game's "done" key set
void MarkNodeDone3(const NODE3& n);

// orig3: 0x00283BB0 (grf: the request's flags; n: GAME +0x14A4)
void StartChallenge3(SW* psw, LO* pnode, int grf, int n);
// (PC, copsmap3.h) the same for a two-player game played in another world (iworldNative >= 0: that world loaded at its
// default point, no task: the native rules run there; -1: as StartChallenge3)
void StartChallengeIn3(SW* psw, LO* pnode, int grf, int n, int iworldNative);
// orig3: 0x002852B0: one runs
bool FChallengeRunning3();
// the running game's mode (the object's +0x50: 0 a challenge, 1 a job replay, 2 a two-player game; -1 none)
int NChallengeMode3();
// the running game's job (+0x48; -1 none)
OID OidChallengeJob3();
// orig3 0x4BC230 +0x38 (0x4BC268): the save as the chooser had it while a game runs (the card's "activity" 0x1CFDF8 set:
// the pause menu's status 1693 / 1702 read it), else null
struct GAMESAVE;
const GAMESAVE* PgsChallengeSaved3();
// orig3: 0x00283F70: the game's end (nothing unless one runs)
void EndChallenge3(SW* psw);
// orig3: 0x00284568: the running challenge stopped silently (a task of another job started: 0x189F38)
void AbortChallenge3();
// The two-player score board (class 0xA4: options 1796..1799): the object's score on the board, -1 none
int NScore3(const LO* pboard, const LO* plo);
void ResetScores3();   // (a world loaded)
// (PC, copsmap3.h) the board's entries set as the original's script sets them (0x111900 then 0x111878 per player) and an
// entry's score (0x111978): Cops and Robbers on another hub drives the original board copied from v_ext
void SetScoreEntries3(LO* pboard, LO* ploA, LO* ploB);
void SetScore3(LO* pboard, const LO* plo, int n);
// orig3 0x284A90: the challenge manager's command (the world's option 759 of bif 125: kind, value, line)
void ChallengeCommand3(int k, float g, int line);
// the line the round ended with (the winner's: Cops and Robbers' 5255 / 5254), -1 none
int LineChallengeEnd3();

// orig3: 0x002849A8: a broadcast's last receiver (game/task3.cpp Broadcast3)
void ChallengeOnEvent3(int code, LO* plo);
// orig3: 0x002848D0: each frame (fPaused: the players paused, 0x1A9478)
void UpdateChallenge3(SW* psw, bool fPaused);
// orig3: 0x00283970: a world loaded (the HUD 1054 and the node found again)
void ChallengeWorldLoaded3(SW* psw);
// orig3: 0x00283A50: the world's start (the values reset after a start / a replay; the HUD shown)
void ChallengeWorldStart3(SW* psw);
// the job restarted while one runs (0x186FE8's task request): the next world start resets the values (flag 4)
void ChallengeRestart3();
// orig3: 0x002846A8 / 0x00284778 / 0x00284658 (the job's end, jobend3.cpp)
void EvaluateChallenge3();
void SaveChallengeBest3();
void ChallengeJobFailed3();
// the manager's state (0x7CD4; 0 none) and the running game's node (null: none)
int NChallengeState3();
const NODE3* PnodeChallenge3();
// orig3: 0x00284828: an item count of an object of the node's group scaled by its factor
int CChallengeScaled3(const LO* plo, int c);
// orig3: 0x002845B0: the running game ends with this task reaching value (2 done, 4 active, 5 left): the task's job is
// the node's (+0x48), the node's +0x64 is value and the task is its +0x60 (-1: the current job's last task)
struct TASK3;
bool FChallengeEndTask3(const TASK3* ptask, int value);
// (0x188658 / 0x186810) the checkpoints are taken: no game of the chooser runs, or a job replay (mode 1)
bool FChallengeCheckpoints3();
// orig3 0x186338: the start request's flag 0x80 taken by the job's switch (*pn: the job's tasks' activation count, 1 for a
// challenge / two-player game, 0 for a job replay); false when none is pending
bool FChallengeTakeStartFlag3(int* pn);
