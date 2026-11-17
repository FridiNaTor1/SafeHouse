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

// Class 0x9D (0x60 bytes; orig3 load 0x17AA08, post-load 0x17AB08): the episode chooser's lists of dvd_menu (an
// episode's Two Player games / Jobs && Challenges (mode 0: rows = class-0xAA game objects by oid), its Gallery (mode
// 1: rows = picture ids), its Bonus Movies (mode 2: rows = movie keys)); options: +0x40 the mode, +0x44 the episode,
// +0x54 / +0x58 the text / picture owners (class-0x97 groups). Runtime: +0x50 the row chosen, +0x5C some row open.
struct LOLIST9D3 : LO
{
    struct ROW
    {
        int32_t kind = 0;               // +0: its S32 (unused by the chooser)
        int32_t n = 0;                  // +4: mode 0 the game object's oid, mode 1 a picture id
        std::array<uint8_t, 64> abKey{};// mode 2: the movie's key (0x119948 ReadCb64)
        LO* pnode = nullptr;            // mode 0: the object (0x17E1B8 kind 0x105)
        bool fAvail = false;            // +8
    };
    int mode = 0, iep = 0;
    std::vector<ROW> arow;
    int isel = -1;
    bool fEnabled = false;
};

// The loaders by their Sly 3 slot-14 address (loaders.cpp RegisterLoaders calls it)
void RegisterScriptLoaders3(std::unordered_map<uint32_t, PFNLOAD>& m);
// The native struct of these classes (loaders.cpp PfnNewNativeForCid asks first; null: the usual one)
PFNNEW PfnNewScript3(CID cid);
