#pragma once

// The guards' detectors (cid 0x10, oid 648: one per guard, its +0xF64; module 0x153B08..0x154C90): the objects it
// watches (the players registered by 0x153EF8 at the guard's reset, others by the AI) and their states: 0 none, 1 seen,
// 2 lost. Each frame (0x153DE0) every entry's state is tested (0x1541D8: the object alive and visible, nothing
// blocking the line from the detector's eye, inside its view volume +0xE8 (and +0xEC), the line-of-sight checks); a
// change tells the detector's owner (its splice event 0x1C) and, at 1, the tasks (0x13).
//
// Part of the animation / AI code.

#include "lo.h"

#include <glm/glm.hpp>

#include <vector>

struct DETENTRY
{
    LO* plo = nullptr;      // +0x0
    int st = 0;             // +0x4
    float t8 = 0;           // +0x8 the state's time
    float tC = 0;           // +0xC when it left 0
};

struct DETECTOR
{
    LO* plo = nullptr;      // the cid 0x10 object
    LO* ploOwner = nullptr; // +0xE0 (the guard: its eye is 80 above it)
    void* pvE4 = nullptr;   // +0xE4 (a list +0x64 of the objects seen through it: TODO(ai))
    LO* ploVolume = nullptr;   // +0xE8 the view volume (cid 0x16 shape; option 315 of the guard)
    LO* ploVolume2 = nullptr;  // +0xEC
    int fAlways = 0;        // +0xF0
    int fNever = 0;         // +0xF4
    uint32_t grf = 0;       // +0xF8 (option 312: 1 no check 0x154770, 2 no check 0x1545F0, 8 ...)
    std::vector<DETENTRY> ae;  // +0xFC / +0x100
    // (a harness's import of the +0xE4 attachment's overlaps {object, in contact}: used instead of the native ones)
    bool fTouchRam = false;
    std::vector<std::pair<LO*, bool>> atouchRam;
    bool fOptionsBound = false; // native: loaded references resolved
};

DETECTOR* PdetectorNew(LO* plo, LO* ploOwner);
DETECTOR* PdetectorFromLo(LO* plo);
void ResetDetectors();
// orig: 0x00154080: an entry's new state (the tasks hear of a guard seeing at 1)
void SetDetentryState(DETECTOR* pdet, DETENTRY* pe, int n);
// orig: 0x00153DE0
void UpdateDetector(DETECTOR* pdet);
// orig: 0x00153E60: the eye
glm::vec3 PosDetectorEye(const DETECTOR* pdet);
// orig: 0x001541D8
int NDetectState(DETECTOR* pdet, LO* pso, int k);
// orig: 0x00154180
DETENTRY* PdetentryFind(DETECTOR* pdet, LO* plo);
// orig: 0x00153EF8 / 0x153F18: an object watched (once)
void AddDetectorTarget(DETECTOR* pdet, LO* plo);

// orig 0x227C90: the point inside the shape (a cid-0x16 object)
bool FPointInShape(LO* plo, const glm::vec3& pos);
bool FGuardSeesPoint(LO* ploGuard, LO* ploVolume, const glm::vec3& pos, const glm::vec3& posT);  // 0x208BE0 kind 1

void PostLoadDetector(LO* object); // loaded object references before world-start events
