#pragma once

// Sly 3's animation segments (ASEG, class 0x82; 0x83 derives from it): the load (orig3 0x135EE8 -> LoadAsegFromBrx
// 0x135FB0), re-derived from the Sly 3 asm (Sly 2's 0x12F2A8 is only 0.48 similar) and checked against the
// reference's SLY2_LOADTRACE of m_ext read for read. Sly 3 shares the key times: the segment has a table of time lists
// ({S8 fWide, U8 c, c x U8 / U16 frames}) and each keyed track names one by index (0x12D1C0).
//
// Body: the LO's options, S8 fDeferred (+0xA4: non-zero, the segment is loaded later by 0x135F48 from another stream,
// and the rest of the body is not read now), then 0x135FB0:
//   U16 frames (+0x64 = frames / 60), U16 ctimes, U16 cbTimes (the pool), ctimes x time list (0x12D078),
//   S16 cchn (+0x4C), per channel (8 bytes at +0x50): S32 (low 20 bits: the oid), S16 (low 10 bits: the sub index),
//     S8 position-track kind (-1 none, 0 / 1 static, 2 a vector (0x12D218), 3 S8 + 3 x S16 (0x12DED0), 4..7 U32 size then
//     the keyed tracks 0x12D330 / 0x12D908 / 0x12E0B0 / 0x12E7E0), S8 scalar-track kind (-1 none, else 0x12EF30:
//     0 F32 (0x12EFE0), 1 keyed (0x12F030), 2 nothing (0x149050)), U8 flags (bits 0..7 of the channel's word);
//   U8 cchg (+0x54, 12 bytes at +0x58): U8 kind, S32, S8 scalar kind + its track;
//   three event lists (0x1364A0: +0x88/+0x8C, +0x90/+0x94 with times, +0x98/+0x9C).

#include "../lo.h"
#include "../sm.h"

#include <array>
#include <cstdint>
#include <vector>

class CBinaryInputStream;

struct ASEGTIMES3   // 0x12D078
{
    bool fWide = false;            // +0: S8 (0: U8 frames, else U16)
    std::vector<uint16_t> aframe;  // +1: U8 count, +2: the frames
};

struct ASEGTRK3
{
    int kind = -1;                 // the S8 that chose it
    int itimes = -1;               // keyed tracks: the time list (0x12D1C0's first U16)
    int n = 0;                     // keyed tracks: 0x12D1C0's second U16
    uint32_t cb = 0;               // kinds 4..7: the U32 before the track (its size)
    int grf = 0;                   // keyed vector tracks: the keyed axes (bits 0..2; the others are constants)
    int s8 = 0;                    // kinds 3, 6, 7: an S8
    std::vector<float> ag;         // floats as read (constants, then per key per keyed axis)
    std::vector<int16_t> an;       // shorts as read (kinds 3, 6, 7)
    std::vector<uint16_t> au;      // kinds 6 / 7: the U16 per key
};

struct ASEGCHN3
{
    int32_t oid = 0;               // S32 << 12 >> 12
    int isub = 0;                  // S16 & 0x3FF (sign-extended)
    ASEGTRK3 trkp, trkg;           // the position / scalar tracks
    uint8_t grf = 0;               // the U8
};

struct ASEGCHG3
{
    int kind = 0;                  // U8
    int32_t n = 0;                 // S32
    ASEGTRK3 trkg;
};

struct ASEGEV3
{
    int kind = 0;                  // S8 (0..26)
    float t = 0.0f;                // the timed list: U16 frames / 60
    std::array<int32_t, 4> an{};
    std::array<float, 3> ag{};
    std::array<uint8_t, 24> ab{};  // kind 24
};

// The Sly 3 data of an ASEG (held by the ASEG / ASEGBL of the copied runtime, sm.h ASEG::pasegd3: its runtime tracks
// point into atimes); BuildAsegd3 (trk3.h) gives the ASEG's ASEGD Sly 3's tracks after the load
struct ASEGD3
{
    int fDeferred = 0;             // +0xA4
    float tMax = 0.0f;             // +0x64
    std::vector<ASEGTIMES3> atimes;
    std::vector<ASEGCHN3> achn;
    std::vector<ASEGCHG3> achg;
    std::array<std::vector<ASEGEV3>, 3> aaev;
};

// orig3: 0x135FB0 LoadAsegFromBrx (the segment's data, after the options and the S8); fills paseg's ASEGD too
void LoadAsegFromBrx3(ASEG* paseg, CBinaryInputStream* pbis);
// orig3: 0x135EE8 (the class's load, vt slot 14 of 0x82 / 0x83)
void LoadAsegObjFromBrx3(LO* plo, CBinaryInputStream* pbis);
