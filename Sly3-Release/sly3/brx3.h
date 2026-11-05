#pragma once

// Sly 3's world file, walked block by block as the original's loader reads it (orig3: 0x14F440 LoadSwFromBrx), for
// --dump-world: every read is the original's, in its order; objects are not created (their bodies are skipped with the
// size their header carries, as the original does for filtered objects), the splice code is parsed but not
// evaluated, textures are parsed but not uploaded. The numbers: sly3/addr.h.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// One entry of a prefix table (orig3: 0x14FA38): {S32 key, S8 value, U8 c, S32 list[c]}.
struct PREFIX3
{
    int32_t key = 0;
    int value = 0;
    std::vector<int32_t> alist;
};

// A top-level object header of a group's object list (orig3: 0x151D80).
struct OBJHDR3
{
    std::size_t ib = 0;   // the header's offset in the file
    int igrp = -1;
    int mode = 0;         // S8: 1 filters + conditions; 2 / 3 shared (0x405 nearest / 0x805 list); 0, <0, >=4 plain
    std::vector<int16_t> afilter;
    std::vector<int32_t> acond;
    int cid = 0;          // a Sly 3 class id (ids3.h CID3)
    int32_t oid = 0;
    int n = 0;            // the splice module index (PloNew's iframe)
    int32_t cb = 0;       // the body's size
    std::size_t ibBody = 0;  // where the body starts (after the header; the renderer's walker, sly3/scene3.cpp)
};

// A group (orig3: the 0x58-byte record of 0x14F440; 0x6C bytes in RAM).
struct GROUP3
{
    char achName[64] = {};
    int32_t x5C = 0, x60 = 0, x10 = 0, x14 = 0;  // as Sly 2's WE fields of the same offsets (x5C: shared group)
    int32_t cinst = 0;    // +0x08: the instance count; 0: the group's data is skipped (Skip(cb))
    uint32_t cb = 0;      // new in Sly 3: the byte size of the group's data (what the loader skips for cinst 0)
    std::size_t ibStart = 0, ibEnd = 0;  // where its data begins and ends (after the final Align(16))
    std::size_t ibShaders = 0, ibCmesh = 0;  // where its shader block (0x234EA0) and its cmesh (0x1ED7B0) begin
    // what the group's data held
    int32_t cbBulk = 0, keyBulk = 0, fScriptDebug = 0;
    uint32_t cmodule = 0;                // splice modules
    int cclutA = 0, cbmpA = 0, cclutB = 0, cbmpB = 0, csaa = 0, cshd = 0;
    uint32_t ccmesh = 0;
    int cobj = 0;
    bool fSkipped = false;
};

// An entry of the option-set tables (orig3: 0x1500F0): A {U32 key, U32 value}; B {U16 array (progress), U32 key, U32 value}
struct OPTSETKEY3
{
    std::vector<uint16_t> aprogress;  // B only (NProgress 0x2BC010 > 0 selects it when no key matched)
    uint32_t key = 0, value = 0;
};

struct SW;
struct ARRIVAL3;
// FWalkWorld3's load mode: the objects are created into psw (sly3/load3.h) instead of skipped.
struct LOAD3OPTS
{
    SW* psw = nullptr;
    int iworld = -1;           // the world filter (orig3 0x47989C)
    int32_t keyA = -1, keyB = -1;  // the arrival keys (orig3 GAME +0x1498 / +0x14A0: -1 at the boot)
};

struct WORLDWALK3
{
    std::size_t cbFile = 0, ibEnd = 0;
    std::vector<PREFIX3> aprefixA, aprefixB;
    std::vector<GROUP3> agroup;
    // the global blocks
    int cprogressName = 0, cprogressRecord = 0;
    int csound = 0, cdialog = 0, cmusic = 0, clang = 0;
    int ctext = 0;
    std::size_t ibMusic = 0, ibMusicEnd = 0, ibText = 0;  // where the music (0x246F58) and text (0x14FD20) blocks are
    int coptSw = 0;
    int coptsetA = 0, coptsetB = 0, coptsetBlock = 0;
    std::vector<OPTSETKEY3> aoptsetA, aoptsetB;
    std::vector<uint32_t> aidOptset;   // the blocks' ids
    std::vector<int> acoptOptset;      // their option counts
    // per block: its group instance counts (EOPID 1032, a CRef pair {group id = record +0x10, count}: the setter
    // 0x10AFB0 -> 0x169500 sets that group's +0x08; scene3.h AcinstGroups3)
    std::vector<std::vector<std::pair<int32_t, int32_t>>> aagrpcount;
    int iblockChosen = -1;             // (load mode) the block 0x1500F0 chose; its id goes to the camera +0x10
    int32_t idChosen = -1;
    int cnodeSet = 0, cnode = 0;
    // the object census
    std::vector<OBJHDR3> aobj;
    std::map<int, int> mpcidc;      // top-level objects per class
    std::map<int, int> mpmodec;     // per header mode
    int cpairSplice = 0;
    std::map<int, int> mptagc;      // splice cells per tag
};

class CBinaryInputStream;
// orig3: 0x1519C8 LoadOptionsFromBrx as the walk reads it (the options until a negative id; the count)
int LoadOptionsFromBrx3Public(CBinaryInputStream* pbis);

// Set by sly3/splice3.cpp (--splice-world): called for every group's splice block (0x2499F0) with its bytes
// [ib, ibEnd) as the walk read them, so that the runtime's own loader can deserialize and evaluate them.
extern void (*g_pfnSpliceBlock3)(const uint8_t* pb, std::size_t ib, std::size_t ibEnd, int igrp, const GROUP3& g);

// Walks a decompressed world file. false (with the error and the offset reached in w.ibEnd) when a read fails.
bool FWalkWorld3(const std::vector<uint8_t>& ab, WORLDWALK3& w, std::string& error, const LOAD3OPTS* pload = nullptr);

// --dump-world's report of one walked world (fObjects: every top-level object header; fGroups: every group).
void PrintWorldWalk3(const WORLDWALK3& w, FILE* pf, bool fGroups, bool fObjects);
