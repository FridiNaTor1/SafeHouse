#pragma once

// The Sly 3 loaders of the game logic's classes (game/loaders3.cpp): SCPT 0x87, MISSION 0xA8,
// TASK 0xA9, SM 0x84, ASEG 0x82 / 0x83, 0x8F, 0x96, 0x97, 0xAA.

#include "../cls.h"
#include "../lo.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Class 0x8F (0x50 bytes; orig3 0x16F768): U8 c x {S32, S8, S8, S32, S32, a 64-byte key} (+0x40 count)
struct LO8F3 : LO
{
    struct ENTRY { int32_t a = 0; int b = 0, c = 0; int32_t d = 0, e = 0; std::array<uint8_t, 64> abKey{}; };
    std::vector<ENTRY> aentry;
};

// Class 0x97 (0x6A0 bytes; orig3 0x16EE98): S32 n x {a file key, S32, S32} (+0x40 list), S32 m x {S8 kind: 1 S32
// S32, 0 / 2 S32} (+0x48 count)
struct LO97 : LO
{
    struct ENTRY { std::array<uint8_t, 64> abKey{}; int32_t a = 0, b = 0; };
    struct ITEM { int kind = 0; int32_t a = 0, b = 0; };
    std::vector<ENTRY> aentry;
    std::vector<ITEM> aitem;
};

// The loaders by their Sly 3 slot-14 address (loaders.cpp RegisterLoaders calls it)
void RegisterScriptLoaders3(std::unordered_map<uint32_t, PFNLOAD>& m);
// The native struct of these classes (loaders.cpp PfnNewNativeForCid asks first; null: the usual one)
PFNNEW PfnNewScript3(CID cid);
