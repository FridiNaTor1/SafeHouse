#pragma once

// A script's handle to one transform record of an ALO (the original's class 0x95; xfhandle.cpp).
#include "basic.h"
#include "ref.h"

#include <cstdint>

struct ALO;
struct XF;

struct XFBASIC : BASIC
{
    ALO* palo = nullptr;  // +0x20
    XF* pxf = nullptr;    // +0x24
    uint32_t worldSerial = 0;
};

// the handle's properties 1059..1062 (geto 111 / seto! 110)
CRef RefXfHandleOption(int bifk, XFBASIC* p, int optid, const CRef* pref);
