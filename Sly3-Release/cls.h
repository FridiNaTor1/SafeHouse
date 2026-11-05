#pragma once

// Sly 3's classes (re-keyed 2026-10-28, the Sly 3 class loaders): the class table of the executable (orig3 0x3409E0: 249 x
// {base, cid, cidLast, size, pfnNew}), generated into sly3/cls3gen.inc by tools/gentables3.py, with each class's
// vtable (the one its pfnNew stores at +0xC). The original's virtual functions are found by vtable slot (8-byte GCC
// 2.95 entries {s16 delta, s16, pfn}); a native implementation is bound to each original Sly 3 function address
// (sly3/load3.cpp RegisterLoaders3), so a class gets the native function of whatever its original vtable holds, the
// original's inheritance included. Every cid here is a Sly 3 cid (ids3.h CID3); the Sly 2 code copied into this
// folder still compares Sly 2 cids in places (tools/stale3.py lists them).

#include <cstdint>
#include <string>

class CBinaryInputStream;
struct LO;
struct SW;

using CID = int;
using OID = int32_t;  // Sly 2 object ids are S32 (Sly 1: S16)

constexpr int CID_Max = 249;  // orig3: the class table's size (Sly 2: 199)

// Original vtable slots (index of the 8-byte entry).
enum VTSLOT
{
    VTSLOT_GetCls = 6,      // returns the class descriptor
    VTSLOT_InitLo = 7,
    VTSLOT_PostLoad = 9,    // called by the world loader for each object of a group after the group's load
    VTSLOT_Load = 14,       // LoadXxxFromBrx(plo, pbis)
    VTSLOT_Clone = 40,      // (Sly 2; Sly 3's CloneLo is slot 44, PloCloneLo 0x1507E0)
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

    // The original function in vtable slot i.
    uint32_t PfnOrig(int slot) const;
};

extern CLS g_acls[CID_Max];

// Fills g_acls from sly3/cls3gen.inc and binds the native functions (sly3/load3.cpp). Needs the executable (g_disc) open.
void InitClasses();
CLS* PclsFromCid(CID cid);
bool FHasNativeLoader(uint32_t pfnOrig);  // (vtaudit.cpp) a native LoadXxxFromBrx is bound to the original function
// orig3: 0x13BEC8 FIsCidDerivedFrom: cid is cidBase or one of its descendants.
bool FCidDerivedFrom(CID cid, CID cidBase);
std::string StrClassName(CID cid);
