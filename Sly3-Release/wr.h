#pragma once

// WR (Sly 3 cid 0x88, vtable 0x36AA60; Sly 2's 0x73): the space warps. A WR keeps up to eight warp entries (WRE: a
// circle wave or a constant), each driving lanes of one of four 4x4 matrices (amatDpos for positions, amatDuv for
// texture coordinates). The meshes it warps (the WRBG blocks of type-0 globs, glob.h) keep per vertex a state vector
// per matrix; the VU program adds the sum of matrix x state to each vertex (natively glob.vert). Sly 3's module
// 0x289678..0x28A6E0 (rewritten from Sly 2's 0x22B720..0x22C6C0: no update slot, the matrices made when drawn).

#include "lo.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct ALO;
struct GLOB;
struct WRBG;

// Sly 3's warp entry (orig3 0x80 bytes, WR +0x60, at most 8; the option getters 0x28A600 / 0x28A620 address its fields
// by offset, so the layout is the original's). Sly 3 dropped Sly 2's bend (its ONZ noise): kind 1 is a constant pair
// of rows. Every option set restamps the entry (0x28A5A8 with 1: +0xC = ++WR +0x4C).
struct WRE
{
    int32_t wrek = -1;   // +0x00: 0 circle (two lanes), 1 constant (one lane), -1 none
    int32_t imat = 0;    // +0x04: its matrix
    int32_t irow = 0;    // +0x08: its first lane (row) of the matrix
    int32_t stamp = 0;   // +0x0C: the WR's stamp when it changed (the blocks' state vectors rebuilt for newer entries)
    float gAmpl = 1.0f;  // +0x10: the amplitude (0x28A480: 1; no option sets it)
    int32_t ab14[3] = {};
    float axis[4] = {};  // +0x20: the circle's wave direction (EOPID 1035, Sly 2 947) / kind 1's position row (1043)
    float vecA[4] = {};  // +0x30: the circle's dpos 0 (1039) / kind 1's coordinate row (1044, two floats)
    float uvA[4] = {};   // +0x40: duv 0 (1041)
    float vecB[4] = {};  // +0x50: dpos 1 (1040)
    float uvB[4] = {};   // +0x60: duv 1 (1042)
    float rgDot = 0.0f;  // +0x70: 1 / the wavelength (1036's setter 0x28A698's twin)
    float gFrequency = 0.0f;  // +0x74 (1037)
    float uPhase = 0.0f;      // +0x78 (1038)
    int32_t ab7C = 0;
};
static_assert(sizeof(WRE) == 0x80);

struct WR : LO
{
    float tLastUpdate = -1.0f;   // +0x40
    int cwre = 0;                // +0x44
    int cmat = 0;                // +0x48
    int stamp = -1;              // +0x4C: bumped by every entry added or changed (0x28A470)
    int alane[4] = {};           // +0x50: the lanes used of each matrix (0x28A480: an entry takes the first with room)
    WRE awre[8]{};               // +0x60
    int iwreCur = -1;            // +0x460: the entry the options address
    glm::mat4 amatDpos[4]{glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f)};  // +0x470 (glm columns = rows)
    glm::mat4 amatDuv[4]{glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f)};   // +0x570
    // +0x670 / +0x674: with an anchor (+0x688, EOPID 1047), the matrices of the blocks under it: the ones above times
    // a rotation of each circle's lanes by the anchor's own wave phase (0x289F78), so the wave stays put in the world
    // while those blocks' positions are taken relative to the anchor (0x2897B8)
    glm::mat4 amatDposAnchor[4]{glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f)};
    glm::mat4 amatDuvAnchor[4]{glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f), glm::mat4(0.0f)};
    std::vector<WRBG*> apwrbg;   // +0x678: the warped blocks (most recently bound first)
    LO* ploAnchor = nullptr;     // +0x688
    int32_t oidAnchor = -1;      // (native: the option's oid until its object is found)
};

// Sly 3 class 0x89 (vtable 0x36A8A8, size 0x960; worlds 6 / 20 / 23 / 31 / 32 / 33 / 35 / 36 / 38): a WR whose
// circle entries are dealt out to up to eight scripted wave items. Each item (orig 0x54 bytes at +0x698) keeps the
// random ranges of its waves and a queue of at most two waves, each one entry of the WR whose amplitude (+0x10)
// follows a smoothstep envelope (in / hold / out), the next wave starting while the last fades out (0x28ADC8).
struct WRQ   // orig 0x14 bytes at item +0x2C
{
    int iwre = -1;           // +0x00: the WR entry (orig: its pointer)
    float tStart = 0.0f;     // +0x04
    float tFull = 0.0f;      // +0x08: full from here
    float tFade = 0.0f;      // +0x0C: fading out from here
    float tStop = 0.0f;      // +0x10
};
struct WRITEM
{
    float lmWavelength[2] = {};  // +0x00 (EOPID 1052)
    float lmAmpl[2] = {};        // +0x08 (1051)
    float lmDuration[2] = {};    // +0x10 (1053)
    float lmAngle[2] = {};       // +0x18 (1054): around the wind's heading + pi
    float sFade = 1.0f;          // +0x20 (1055): the longest fade out (and the gap past other items' fades)
    float gSpeed = 1.0f;         // +0x24 (1056): the wave speed's factor
    int cq = 0;                  // +0x28
    WRQ aq[2]{};                 // +0x2C
};
struct WR89 : WR
{
    WR89();                      // 0x28A788 (vt slot 7): one circle entry, amplitude 0
    int citem = 0;               // +0x690 (EOPID 1049)
    int iitemCur = 0;            // +0x694 (1050)
    WRITEM aitem[8]{};           // +0x698
    bool afOwned[8] = {};        // +0x938: the entries handed to an item's wave (orig: the item)
};

// orig3 0x28A558 / 0x28A588 (0x28A480): a circle / constant entry added (the current one)
void AddWrCircleWarp(WR* pwr);
void AddWrBendWarp(WR* pwr);
// orig3 0x289F78 (vt slot 53, called for each warped glob drawn): the block's state vectors rebuilt when the WR changed
// since, then the matrices once per clock time
void UpdateWrMatrixes(WR* pwr, WRBG* pwrbg = nullptr);
// the matrices a block draws with (the anchor's for a block under it, 0x2896D0)
const glm::mat4* AmatWrDpos(const WR* pwr, const WRBG* pwrbg);
const glm::mat4* AmatWrDuv(const WR* pwr, const WRBG* pwrbg);
// The WR end of 0x167B68 (an ALO's post-load): each warp block of its globs bound to the WR its oid names (0x22B7A0)
void BindAloWarps(ALO* palo);
// The renderer's view of a glob's warp: null when the glob has none or it is not bound
const WR* PwrOfWrbg(const WRBG* pwrbg);
// A clone's WR data (CloneWr: the entries, matrices, list)
void CloneWrData(const LO* ploSrc, LO* ploDst);
// The loader's options of a WR (the sub-object fields of EOPIDs 947..961 and the field 964): true when handled
bool FLoadWrOption(LO* plo, int eopid, const float* ag, int n);
