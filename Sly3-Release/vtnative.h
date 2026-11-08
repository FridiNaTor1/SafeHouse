#pragma once

// Virtual calls of the original's vtables, natively: each module registers its native implementation of an
// original function by the original's address (NATIVEFN), and a call through vtable slot k of an object finds the
// native implementation of whatever function the object's original vtable holds in slot k (inheritance included,
// as the class table's vtables are the executable's own).
//
// (the physics port; general: the update dispatch and other code can use the same registry.)

#include "cls.h"
#include "lo.h"

#include <cstdint>

void RegisterNativeFn(uint32_t addrOrig, void* pfn, const char* name);
void* PvNativeFn(uint32_t addrOrig);
const char* NameNativeFn(uint32_t addrOrig);
// (Sly 3) the native registered by NATIVEFN3 for the Sly 3 function in the slot paired with Sly 2 slot `slot2`
void* PvNativeFn3(const CLS* pcls, int slot2);  // (the registered name; null: none)
// The native function for vtable slot `slot` of the object's class (null: the original's function has no port).
void* PvNativeVt(const LO* plo, int slot);
// Logs (once per original function) a vtable call that has no native implementation.
void NoteMissingVt(const LO* plo, int slot);

template <class FN>
inline FN PfnVt(const LO* plo, int slot)
{
    void* pv = PvNativeVt(plo, slot);
    if (!pv) NoteMissingVt(plo, slot);
    return reinterpret_cast<FN>(pv);
}

struct NATIVEFNREG
{
    NATIVEFNREG(uint32_t addr, void* pfn, const char* name) { RegisterNativeFn(addr, pfn, name); }
};
#define NATIVEFN_CAT2(a, b) a##b
#define NATIVEFN_CAT(a, b) NATIVEFN_CAT2(a, b)
// NATIVEFN(0x1F3980, UpdateAloPhys): fn implements the original function at that address.
#define NATIVEFN(addr, fn) \
    static NATIVEFNREG NATIVEFN_CAT(s_nativefnreg_, __LINE__)(addr, reinterpret_cast<void*>(&fn), #fn)
// NATIVEFN3(0x1AD7F0, UpdateSly3): fn implements the Sly 3 function at that address (orig3; found before the Sly 2
// twin's native, vtmap3.cpp). Its signature is the copied code's for that slot.
#define NATIVEFN3(addr3, fn) \
    static NATIVEFNREG NATIVEFN_CAT(s_nativefnreg_, __LINE__)(FN3_TAG | uint32_t(addr3), reinterpret_cast<void*>(&fn), #fn)
