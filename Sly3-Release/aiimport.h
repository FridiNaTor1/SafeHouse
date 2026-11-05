#pragma once

// The AI's differential check against the reference (with --anim-ref ... --ai): the guards' state (their fields, job
// stacks, kind tables, detectors, command lists, own paths) imported from a reference snapshot into native GUARDs,
// one native frame of their AI (UpdateGuard: the jobs, the steering), then the result compared with the next
// snapshot: each guard's state mask, steering (+0x190 velocity, +0x1E0 rotation, the heading), its jobs (the stack's
// classes and each job's state), the walk's speed. Re-imported from the snapshot each frame (each frame is checked on
// its own). Part of the animation / AI code.

#include "refram.h"

#include <string>
#include <unordered_map>
#include <vector>

struct ASEGA;
struct GUARD;
struct SMA;

struct AIREF
{
    MAPPING* pmap = nullptr;
    std::unordered_map<uint32_t, ASEGA*>* pmpaasega = nullptr;
    std::unordered_map<uint32_t, SMA*>* pmpasma = nullptr;
    std::vector<std::pair<uint32_t, GUARD*>> aguard;  // the imported guards (reference address, native)
    std::unordered_map<uint32_t, LO*> mpacmdlist;     // reference command lists -> native (made on import)
    bool fVerbose = false;
    bool fCarry = false;     // --ai-carry: the AI imported once; then only its inputs (the guards' records, the regions)
    bool fFree = false;      // --ai-free: imported once; then only the regions' polygons (the guards run natively)
    bool fImported = false;
    bool fNoPlayers = false; // (the players' records not imported: a harness that imports them itself)
};

struct AISTATS
{
    int cguard = 0, cguardOk = 0;
    int cjob = 0, cjobOk = 0;
    int cfield = 0, cfieldOk = 0;
    std::vector<std::string> amsg;
};

// Before the world's ASEGAs are imported: the polygons and regions from the snapshot
void AiImport(const RAM& ram, AIREF* pref);
void AiStep(AIREF* pref);
AISTATS AiCompare(const RAM& ramB, AIREF* pref);
// a job class by its original vtable (0x9C..0xC6; -1 none)
CID CidOfJobVt(uint32_t vt);
// the hits other code delivered in the frame A -> B (message 12 rebuilt from B)
void AiDeliverHits(const RAM& ramA, const RAM& ramB, AIREF* pref);
