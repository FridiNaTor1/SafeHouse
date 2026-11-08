#pragma once

// Sly 2's classes: the class table of the executable (0x2BAAE8: 199 x {base, cid, cidLast, size, pfnNew}),
// generated into clsgen.inc by tools/elfclasses.py, with each class's vtable (the one its pfnNew stores at
// +0xC). The original's virtual functions are found by vtable slot (8-byte GCC 2.95 entries {s16 delta, s16,
// pfn}); a native implementation is bound to each original function address (cls.cpp), so a class gets the
// native function of whatever its original vtable holds, the original's inheritance included.

#include <cstdint>
#include <string>

class CBinaryInputStream;
struct LO;
struct SW;

using CID = int;
using OID = int32_t;  // Sly 2 object ids are S32 (Sly 1: S16)

constexpr int CID_Max = 249;   // Sly 3's class table (orig3 0x3409E0; game/sly3.h sly3::CCLASS)

// The objects of a class, by a Sly 3 cid; an unmapped Sly 2 class (S3CID(...) == -1, game/sly3.h) has none.
#define CIDLIST(mp, cid) ((cid) >= 0 ? (mp)[(cid)] : ::EmptyLoList())
#include <vector>
struct LO;
inline const std::vector<LO*>& EmptyLoList() { static const std::vector<LO*> s_a; return s_a; }

// Original vtable slots (index of the 8-byte entry).
enum VTSLOT
{
    VTSLOT_GetCls = 6,      // returns the class descriptor
    VTSLOT_InitLo = 7,
    VTSLOT_PostLoad = 9,    // called by the world loader for each object of a group after the group's load
    VTSLOT_Load = 14,       // LoadXxxFromBrx(plo, pbis)
    VTSLOT_Clone = 40,
    VTSLOT_PloFindChild = 49,
    VTSLOT_PxfFind = 51,
};

using PFNLOAD = void (*)(LO* plo, CBinaryInputStream* pbis);
using PFNNEW = LO* (*)();

struct CLS
{
    CID cid = 0;
    CID cidBase = -1;     // -1: the root
    CID cidLast = 0;      // classes cid..cidLast-1 derive from this one
    int cbOrig = 0;       // the original object size
    uint32_t pfnNewOrig = 0;
    uint32_t vtOrig = 0;
    const char* name = "";

    // The native implementations bound from the original vtable (null when the original's function has no
    // native port yet; loading such an object is an error).
    PFNLOAD pfnLoad = nullptr;
    uint32_t pfnLoadOrig = 0;
    PFNNEW pfnNew = nullptr;  // the native object (the nearest ancestor with a native struct)

    CLS* pclsBase = nullptr;

    // The copied Sly 2 code's view (vtmap3.cpp): the Sly 2 twin of the function in the Sly 3 slot paired with Sly 2
    // slot `slot` (FN3_TAG | the Sly 3 address when it has no twin; 0 when the slot has no pair).
    uint32_t PfnOrig(int slot) const;
    // The Sly 3 vtable's own function in Sly 3 slot `slot3`.
    uint32_t PfnOrig3(int slot3) const;
};

// (vtmap3.cpp) A Sly 3 function address with no Sly 2 twin, as CLS::PfnOrig gives it and NATIVEFN3 registers it.
constexpr uint32_t FN3_TAG = 0x80000000u;
void InitVtMap3();                        // after the class table (InitClasses)
int Slot3FromSlot2(CID cid, int slot2);   // -1: no pair
uint32_t Fn2FromFn3(uint32_t fn3);        // the Sly 2 twin, else FN3_TAG | fn3

extern CLS g_acls[CID_Max];

// Fills g_acls from clsgen.inc and binds the native functions. Needs the executable (g_disc) open.
void InitClasses();
CLS* PclsFromCid(CID cid);
bool FHasNativeLoader(uint32_t pfnOrig);  // (vtaudit.cpp) a native LoadXxxFromBrx is bound to the original function
// The original's FIsCidDerivedFrom (0x15E988): cid is cidBase or one of its descendants.
bool FCidDerivedFrom(CID cid, CID cidBase);
std::string StrClassName(CID cid);
