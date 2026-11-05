#pragma once

// WR (cid 0x73, vtable 0x2DCF00): the space warps (Sly 1's wr.c). A WR keeps up to eight warp entries (WRE: a
// circle wave or a bend), each driving one of four 4x4 matrices (amatDpos for positions, amatDuv for texture
// coordinates). The meshes it warps (the WRBG blocks of type-0 globs, glob.h) keep per vertex a state vector per
// matrix; the VU program adds the sum of matrix x state to each vertex (natively glob.vert). Module 0x22B720..0x22C6C0.

#include "lo.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct ALO;
struct GLOB;
struct WRBG;

// Noise terms (0x22BE60 UNoise) of an ONZ: a value in [gMin, gMax] biased by uBias (0x22BF70 GFromOnz)
struct ONZE
{
    float uAmpl;
    float gFreq;
    float gPhase;
    float uRandom;
};
struct ONZ
{
    float gMin;
    float gMax;
    float uBias;
    int32_t conze;
    ONZE aonze[4];
};
static_assert(sizeof(ONZ) == 0x50);

// A warp entry (0xD0 bytes; the option getters 0x22C4D8 / 0x22C4F8 address its fields by offset, so the layout is
// the original's: the circle's and the bend's fields share +0x30..)
struct WRE
{
    int32_t wrek;        // +0x00: 0 circle, 1 bend
    int32_t imat;        // +0x04: its matrix
    int32_t ab08[2];
    float normal[4];     // +0x10: the circle's wave direction / the bend's axis (EOPIDs 947 / 955)
    float vecA[4];       // +0x20: the circle's dpos 0 / the bend's swivel normal (951 / 959)
    union
    {
        struct
        {
            float uvA[4];         // +0x30: duv 0 (953)
            float vecB[4];        // +0x40: dpos 1 (952)
            float uvB[4];         // +0x50: duv 1 (954)
            int32_t is;           // +0x60: its two lanes of the matrix (0 or 2)
            float rgDot;          // +0x64: 1 / the wavelength (948)
            float gFrequency;     // +0x68 (949)
            float uPhase;         // +0x6C (950)
            uint8_t ab70[0x60];
        } circle;
        struct
        {
            ONZ onzBend;          // +0x30: the bend angle (956 LM, 957 bias, 958 noise)
            ONZ onzSwivel;        // +0x80: the swivel angle (960 LM, 961 bias, 962 noise)
        } bend;
    };
};
static_assert(sizeof(WRE) == 0xD0);

struct WR : LO
{
    float tLastUpdate = -1.0f;   // +0x50
    int cwre = 0;                // +0x54
    int cmat = 0;                // +0x58
    int imatHalf = -1;           // +0x5C: a matrix with one circle (its lanes 2 / 3 free)
    WRE awre[8]{};               // +0x60
    int iwreCur = -1;            // +0x6E0: the entry the options address
    glm::mat4 amatDpos[4]{glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f)};  // +0x6F0 (glm columns = rows)
    glm::mat4 amatDuv[4]{glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f)};   // +0x7F0
    std::vector<WRBG*> apwrbg;   // +0x8F0: the warped blocks (most recently bound first)
    bool fValuesChanged = false; // +0x8FC: the state vectors are recomputed at the next update
};

// orig 0x22C3B0 / 0x22C430: a circle / bend entry added (the current one)
void AddWrCircleWarp(WR* pwr);
void AddWrBendWarp(WR* pwr);
// orig 0x22C028 (vt slot 16)
void UpdateWrMatrixes(WR* pwr);
// The WR end of 0x167B68 (an ALO's post-load): each warp block of its globs bound to the WR its oid names (0x22B7A0)
void BindAloWarps(ALO* palo);
// The renderer's view of a glob's warp: null when the glob has none or it is not bound
const WR* PwrOfWrbg(const WRBG* pwrbg);
// A clone's WR data (CloneWr: the entries, matrices, list)
void CloneWrData(const LO* ploSrc, LO* ploDst);
// The loader's options of a WR (the sub-object fields of EOPIDs 947..961 and the field 964): true when handled
bool FLoadWrOption(LO* plo, int eopid, const float* ag, int n);
