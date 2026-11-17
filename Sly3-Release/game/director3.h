#pragma once

// Sly 3's DIRECTOR (class 0x46): the heist's dance (director3.cpp). The state its draw (vt slot 21 0x20A200, s3-decomp's
// HUD) and its options read.

#include <utility>
#include <vector>

struct LO;
struct SW;

struct DIRECTOR3
{
    LO* plo = nullptr;
    int nMode = 0;               // +0x190 (0 none, 1 the demonstration (the game paused, flags 3), 2 the player's turn)
    float tMode = 0.0f;          // +0x194
    float gEarly = 0.4f;         // +0x198 the window before a cue (EOPID 1519)
    float gLate = 0.4f;          // +0x19C after it (1520)
    struct ITEM { float t; int btn; bool fHit; };
    std::vector<ITEM> aitem;     // +0x1A0 / +0x1B0 the timeline (0x57: up to 10)
    struct CUE { float t = 0; int btn = 0; float tQueued = 0; bool fShown = false, fReached = false, fPressed = false, fDone = false; };
    CUE acue[3];                 // +0x3C4 the live cues (0x58)
    int ccue = 0;                // +0x3C0
    int btnLast = 0;             // +0x418
    float lmMin = -1.5f, lmMax = 1.0f;  // +0x41C (1521)
    int cScore = 0;              // +0x424
    float agScore[16] = {};      // +0x428
    int cAvg = 8;                // +0x468 (1522)
    float gMeterTarget = 0.0f;   // +0x46C (1529)
    float gMeter = 0.0f;         // +0x470 (shown)
    float gPenEarly = -1.5f;     // +0x480 (1524)
    float gPenWrong = -0.8f;     // +0x484 (1525)
    float gPenSecond = -0.2f;    // +0x488 (1526)
    float gPenMiss = -1.5f;      // +0x48C (1527)
    int nResult = 0, btnResult = 0, btnResult2 = 0;  // +0x490 / +0x494 / +0x498 (1531..1533)
};

// a world's start: its directors (the last one's init sets D_00468D04)
void ResetDirectors3(SW* psw);
// the song's clock (0x2483D8(D_00474FF0): the last stream started: music 0x59 or a dialog 0x52)
void NoteStreamStarted3();
float TDirectorStream3();
// 0x57: the timeline from the following 0x58 records {time, button}, then the mode (0x20A758, 0x20A6D0)
void DirectorCmd57_3(int nMode, const std::vector<std::pair<float, int>>& aitem);
// 0x58: a cue queued (0x20A880)
void DirectorCmd58_3(float t, int btn);
// each frame (vt slot 18 0x209FD0 of each director)
void UpdateDirectors3(float dt);
const DIRECTOR3* PdirectorCur3();
