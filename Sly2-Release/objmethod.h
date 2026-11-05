#pragma once

// The objects' native methods: the EOPID properties with flags & 1 (geto gives a function of the object,
// 0x1FFD98) and the method options the loader calls (grf 0x400, 0x13E818). Each original thunk (EOPID +0x08 /
// +0x10, the splicewrap functions 0x100A90..) converts its CRefs (RefSetArgsFromSplice 0x201110) and calls a game
// function; natively a body is registered by the thunk's address.

#include "ref.h"

#include <cstdint>
#include <vector>

struct LO;
struct OPTVAL;

using PFNOBJMETHOD = CRef (*)(LO* plo, const std::vector<CRef>& aref);

void RegisterObjectMethod(uint32_t pfnThunk, PFNOBJMETHOD pfn);
PFNOBJMETHOD PfnObjectMethod(uint32_t pfnThunk);
// The properties' getters (geto of a non-method property: a getter function (grf 4, EOPID +0x0C) or a field
// (grf 2) the native object keeps elsewhere): bodies by EOPID index
using PFNOBJGETTER = CRef (*)(LO* plo);
void RegisterObjectGetter(int eopid, PFNOBJGETTER pfn);
PFNOBJGETTER PfnObjectGetter(int eopid);

// The properties' setters (seto! and the loader's setter options, grf 0x100 / the attach's 0x20): by EOPID index
using PFNOBJSETTER = void (*)(LO* plo, const CRef& ref);
void RegisterObjectSetter(int eopid, PFNOBJSETTER pfn);
PFNOBJSETTER PfnObjectSetter(int eopid);

// a splice function (TAGK_Method) calling the body registered for pfnThunk on plo (null when none is registered)
bool FMakeObjectMethod(uint32_t pfnThunk, BASIC* pbasic, int crefReq, CRef* pref);

// The arguments (0x201110): an int (S32, else coerced), a float, a bool, an object (an object CRef, or an oid as
// the loader's options keep them), a list of ints (0x2013D8)
int NArg(const std::vector<CRef>& aref, size_t i, int nDefault = 0);
float GArg(const std::vector<CRef>& aref, size_t i, float gDefault = 0.0f);
LO* PloArg(const std::vector<CRef>& aref, size_t i);
std::vector<int> AnArgList(const std::vector<CRef>& aref, size_t i);

// The loader's option values as CRefs (a list kind as a CRef list)
CRef RefFromOptionValue(const OPTVAL& v);
