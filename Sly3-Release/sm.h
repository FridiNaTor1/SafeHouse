#pragma once

// LO classes with their own loaders that are not transforms: animation segments (ASEG, cid 0x6D/0x6E,
// 0x12F2A8, loaded by aseg.cpp), state machines (SM, cid 0x6F, 0x1EFAC0) and the sprite sets of the 2D UI (cid 0x7E..0x83,
// 0x212298: sprites, groups, animations).

#include "cls.h"
#include "lo.h"
#include "aseg.h"
#include "screenobj.h"

#include <memory>

#include <unordered_map>

struct ASEGA;
struct SMA;

// ASEG (cid 0x6D/0x6E): the splice code's ASEGD loader (aseg.h) on the object; the runtime fields (asega.cpp)
// as InitAseg 0x12F248 sets them.
struct ASEG : LO
{
    ASEGD asegd;
    std::vector<ASEGA*> apasega;   // orig +0x50: its playing ASEGAs
    int prio = 0;                  // orig +0x6C: its controllers' priority
    int fSound = 0;                // orig +0x70: the ASEGAs' initial event-sound flag
    float svt = 1.0f;              // orig +0x78
    float gSuck0 = 1.0f, gSuck1 = 0.0f, gSuck2 = 0.0f;  // orig +0x80/+0x84/+0x88: the speed's polynomial of the
    float gSuckMin = 0.0f, gSuckMax = 1.0f;             // suck history, clamped into [+0x90, +0x94]
    uint32_t grfAseg = 0x40;       // orig +0x40 (0x100: the ASEGAs run on the real dt)
    bool fApplyParams = false;     // orig +0xB0 (EOPID 107, 0x12FDE8): applied by PostAsegLoad
    float tApply = 0.0f, svtApply = 1.0f;
    int grfApply = 0;
    std::shared_ptr<const struct ASEGD3> pasegd3;  // Sly 3: the loaded data its tracks point into (game/aseg3.h)
};

// SM (cid 0x6F, 0x1EFAC0): states (the loaded objects' oids with the next state), transitions (an ASEG between two
// states, flags, a weight) and the distance tables per flag group; SMAs run it on objects (smrun.cpp).
struct SM : LO
{
    std::vector<LO*> aploChild;
    std::vector<std::pair<int32_t, int32_t>> anode;   // orig +0x54 {oid, next oid (-1 none, 0x20 random)}
    struct EDGE { int32_t from, to, grf, value; float weight; LO* plo; };
    std::vector<EDGE> aedge;                          // as loaded (oids)
    // The runtime form (PostLoad / 0x1EFAC0's end): indices.
    struct SMS { int32_t oid; int inext; };           // inext: -1 none, -2 random, else the state index
    struct SMT { ASEG* paseg; int isFrom, isTo; uint32_t grf; float gProbability; uint32_t value = 0; };  // (+0x10 value: the look mask 0x1F1350 tests)
    struct SMG { uint32_t grf; std::vector<int8_t> adist; };  // a flag group's distance matrix (csms x csms)
    std::vector<SMS> asms;
    std::vector<SMT> asmt;
    std::vector<SMG> asmg;
    bool fInit = false;
    bool fInitial = false;            // orig +0x74 (EOPID 370, 0x1F0128): the SMA applied by PostSmLoad
    int32_t oidInitial = -1, oidStateInitial = -1, grfaplInitial = 0;
    std::vector<SMA*> apsma;          // orig +0x68
};

struct SPRITESET : LO
{
    int f = 0;
    float g90 = 0.0f;
    uint32_t u94 = 0, u98 = 0, uBC = 0;
    uint8_t abKey[64]{};
    int csprite = 0;
    int cgroup = 0;
    std::vector<std::pair<int32_t, float>> aanim;  // the groups' animations: (id, duration) (0x214460)
    // the animation playing (vt slot 49 starts one by id; 0x2121E8 asks whether one still plays)
    int32_t idAnimPlaying = -1;
    float tAnimEnd = 0.0f;
    std::shared_ptr<SCREENOBJ> pso;   // the renderer's native data (screenobj.h)
};

void RegisterAsegLoaders(std::unordered_map<uint32_t, PFNLOAD>& m);
PFNNEW PfnNewAseg(CID cid);
// orig 0x001EFAC0 LoadSmFromBrx (Sly 3 0x238FF8 reads the same)
void LoadSmFromBrx(LO* plo, CBinaryInputStream* pbis);
