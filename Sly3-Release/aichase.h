#pragma once

// Carmelita's chase (the class 0x3F guard, module 0x149FE8..0x14B468): the chase graph objects (cid 0x7B, loader
// 0x14A008, post-load 0x14A0A8) and the chase job 0xB6 (0x14A1B0..0x14AE38) the reaction 0xE starts (method 1173,
// 0x14B468). Part of the Murray camera code.

#include "ai.h"
#include "crv.h"
#include "lo.h"

#include <vector>

struct ACTIONOBJ;

// cid 0x7B (0x60 bytes): the places Carmelita runs / hops between, in order
struct CHASEGRAPH : LO
{
    struct ENTRY
    {
        int k = 0;                // +0x0 how she gets to it from the previous one: 0 runs, 1 hops
        OID oid = -1;             // +0x4
        LO* plo = nullptr;        // +0x8 (0x14A0A8)
    };
    int nDir = 0;                 // +0x50 (1: the curve's distances counted the other way)
    std::vector<ENTRY> aentry;    // +0x54 / +0x58 (at most 0x100: 0x14A178)
    CRV crv;                      // +0x5C the curve through the entries (0x1E94B0(0x62C, c): a path object, linear)
};
LO* PloNewChasegraph();
// orig 0x14A0A8 (vt slot 15)
void PostLoadChasegraph(LO* plo);

// 0xB6 (0x50): the chase (Init 0x14A1B0, Enter 0x14A200, Exit 0x14A248, Update 0x14A2A0, SetState 0x14AE38): 0 decide,
// 1 run to the next entry (0xA7 on the guard's own path), 2 hop to it (0xAC), 3 an action on the target (0xA4), 4 the
// guard held still (0x9E)
struct JOBCHASE : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    CHASEGRAPH* pgraph = nullptr; // +0x38
    int i = -1;                   // +0x3C the entry she is at / going to
    int iTarget = -1;             // +0x40 the best entry (0x14AA30)
    ACTIONOBJ* paction = nullptr; // +0x44
    float tMoved = 0;             // +0x48 the last time the player moved
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetState(int n) override;
    void Aim();                   // orig 0x14A4D0
    bool FAirborne();             // orig 0x14A478
    int IBest();                  // orig 0x14AA30
    void Step();                  // orig 0x14AD70
    LO* PloEntry() const;         // orig 0x14AE18
};
