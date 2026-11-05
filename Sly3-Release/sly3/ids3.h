#pragma once

// Sly 3's ids: classes (cid), options (EOPID), splice built-ins and tags. Every one of them was renumbered from Sly 2
// (REPORT §3.3, §3.5), so they are strong enums here: copied Sly 2 code that passes a Sly 2 number (an int) does not
// convert to them, and a Sly 3 id must be written as CID3::Xxx or mapped explicitly (Cid3FromCid2...).
//
// The tables behind them are generated (tools/gentables3.py -> cls3gen.inc, eopid3map.inc, bif3map.inc) from the
// research's matching of the two executables (vtables, option kinds, built-in code). A "twin" is the Sly 2 id the
// matching found; its score says how sure (cls: share of vtable slots that agree; -1: found by the option kinds).

#include <cstdint>
#include <string>

// Classes (orig3: the class table 0x3409E0, 249 entries). Names: the Sly 2 twin's where the research found one and
// the roles of REPORT §3.3; the others stay numbers until their code is read.
enum class CID3 : int
{
    Root = 0x00,
    SW = 0x01,
    LO = 0x02,
    LOX = 0x03,
    VOLUME = 0x07,       // Sly 2 0x05
    PROXY = 0x08,        // Sly 2 0x06 (FIsCidDerivedFrom(cid, 8): LoadProxyFromBrx 0x1FA778)
    PATH = 0x09,         // Sly 2 0x0A
    ZONE = 0x0E,         // Sly 2 0x0F
    MESHZONE = 0x10,     // Sly 2 0x11
    SHAPE = 0x13,        // Sly 2 0x16
    TRIGGER = 0x15,      // no Sly 2 twin: a SHAPE with the trigger component (trigger3.h)
    EMITTER = 0x1B,      // Sly 2 0x1D
    GROUPER = 0x1D,      // Sly 2 0x1F
    EXPL = 0x1E,         // Sly 2 0x20
    ALO = 0x2F,          // Sly 2 0x30
    SO = 0x31,           // Sly 2 0x32
    PO = 0x32,           // Sly 2 0x33
    PLAYER = 0x33,       // Sly 2 0x34 (the player base)
    JT = 0x34,           // Sly (Sly 2 0x35)
    MURRAY = 0x35,       // Sly 2 0x36
    BENTLEY = 0x36,      // Sly 2 0x37
    // The new gang members: the only object of m_ext's character groups (whose record +0x10 is the root oid):
    GURU = 0x37,         // FK$Xshaman, oid 29
    PANDA_KING = 0x38,   // FK$Xpanda_king, oid 30
    PENELOPE = 0x39,     // FK$Xpenelope, oid 31
    GUARD = 0x3A,        // the guard base (Sly 2 STEPGUARD 0x38); subclasses 0x3B..0x43
    CARMELITA = 0x3C,    // FK$Xcarmelita, oid 28 (Sly 2 twin 0x3F, 0.43)
    ASEG = 0x82,         // Sly 2 0x6D
    SM = 0x84,           // Sly 2 0x6F
    SCREENOBJ_FIRST = 0x9E,  // the screen objects 0x9E..0xA7
    MISSION = 0xA8,      // Sly 2 0x84
    TASK = 0xA9,         // Sly 2 0x85
    CM = 0xAC,           // Sly 2 0x87
    JOB_FIRST = 0xC7,    // the jobs 0xC7..0xF8
    Max = 249,
};

// The class table's entry (generated from the executable; cls3gen.inc).
struct CLS3GEN
{
    int cid, cidBase, cidLast, cb;
    uint32_t pfnNew, vt, pfnLoad;  // orig3 addresses (pfnLoad: vtable slot 14)
    int cid2;                      // the Sly 2 twin, -1: none found
    int score;                     // x100; -1: found by the option kinds
    const char* name2;             // the twin's Sly 2 name ("" without one)
};
const CLS3GEN& Cls3(int cid);
const char* SzCid3(int cid);            // "JT", "ALO", or "CID3_2D"
std::string SzCid3Twin(int cid);        // "sly2 0x30 ALO 0.72", or "" without a twin
bool FCid3DerivedFrom(int cid, CID3 cidBase);  // the table's base chain (orig3: 0x13BEC8 FIsCidDerivedFrom)
int Cid2FromCid3(int cid);              // the twin, -1: none
int Cid3FromCid2(int cid2);             // the Sly 3 class whose twin is cid2, -1: none
constexpr int Icid(CID3 cid) { return static_cast<int>(cid); }

// Options (orig3: the EOPID table 0x32CA80, 2044 x 0x28). Named ones are added as their uses are ported.
enum class EOPID3 : int
{
    Max = 2044,
};
struct EOPID3MAP
{
    int eopid3;
    uint32_t k, grf, c18;
    int eopid2;  // the aligned Sly 2 EOPID, -1: none
    int score;   // x10
};
const EOPID3MAP& Eopid3(int eopid);
int Eopid2FromEopid3(int eopid3);
int Eopid3FromEopid2(int eopid2);       // -1: none (several Sly 3 ids may align to one Sly 2 id: the best score)

// Splice built-ins (orig3: 0x342030, 178 x 12 {pfn, crefReq, fVarArg}).
enum class BIFK3 : int
{
    Max = 178,
};
struct BIF3MAP
{
    int bif3;
    uint32_t pfn;
    int crefReq, fVarArg;
    int bif2;   // the Sly 2 built-in, -1: new in Sly 3
    int sim;    // x1000
};
const BIF3MAP& Bif3(int bif);
int Bif2FromBif3(int bif3);
int Bif3FromBif2(int bif2);

// Splice tags (orig3: PpairSerializeIn 0x12C108's dispatch at 0x31F610, RefEval 0x1296F0's form table 0x31F4E0,
// RefEvalApply 0x129208). All confirmed from the asm (2026-10-28, the Sly 3 splice port; ref.h's TAGK is the same numbering):
// Sly 2's DefineMacroLambda (4) and Import (17) are gone.
enum class TAGK3 : int
{
    None = 0,
    Set = 1,           // 0x128428 RefSetBinding
    Define = 2,        // 0x1284D8 RefAddBinding
    DefineMacro = 3,   // invalid in the deserializer (as Sly 2's 3); RefEval applies it
    Assert = 4,        // 0x128588: evaluates its argument, void (Sly 2 5)
    Debug = 5,         // 0x128610 (Sly 2 6)
    If = 6, Or = 7, And = 8, Cond = 9, Else = 10, Case = 11, Let = 12, While = 13, Lambda = 14, Begin = 15,  // Sly 2 +1
    Quote = 16,        // (Sly 2 18)
    Pipe = 17,         // invalid in the deserializer (Sly 2 19)
    Dot = 18,          // the rest marker in a lambda's formals (RefEvalApply; Sly 2 20)
    S32 = 19,          // U32 payload (Sly 2 21)
    F32 = 20,          // (Sly 2 22)
    Bool = 21,         // (Sly 2 23)
    Symid = 22,        // (Sly 2 24)
    Bifk = 23,         // (Sly 2 25)
    Vector = 24,       // 3 floats (Sly 2 26)
    Matrix = 25,       // 16 floats (Sly 2 27)
    Clq = 26,          // 3 floats (Sly 2 28)
    Lm = 27,           // 2 floats (Sly 2 29)
    Smp = 28,          // 3 floats (Sly 2 30)
    Pair = 29,         // a nested list (Sly 2 31)
    Proc = 30,         // a closure (RefEvalApply; Sly 2 32): invalid in the deserializer
    Basic = 31,        // an object, U32 (Sly 2 33)
    Max = 32,          // (runtime only: 32 method, 34 void)
};
// The Sly 2 tag of a Sly 3 tag (0..31).
int Tagk2FromTagk3(int tag3);
