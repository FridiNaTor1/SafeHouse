#pragma once

// Textures and shaders (Sly 3 module 0x233AE0-0x237EC8; Sly 2's 0x1EA568-0x1ED7A8, Sly 1's shd.c): the colour tables
// (CLUT), bitmaps (BMP), texture states (TEX), shaders (SHD) and shader animations (SAA) of a world, loaded per object
// group by LoadShadersFromBrx (orig3 0x234EA0), and the optional "bulk" block of permanent texture data (0x25C930 /
// 0x25C9A0). Ported from the Sly 3 asm on 2026-10-28 (the Sly 3 renderer): the record layouts below are Sly 3's.
//
// Native layout: records are owned C++ structs; pixel / colour bytes are kept as loaded (already colour-adjusted,
// PS2 alpha 0x80 = 1.0) and decoded to RGBA8 on demand by the renderer (TexImage*).

#include "bis.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct SW;

// Where a CLUT's / BMP's data lives (+4 of the record; Sly 1's grfzon-like "kind").
enum TEXK : uint8_t
{
    TEXK_Filtered = 0,   // excluded by the record's filter (or never loaded)
    TEXK_Bulk = 1,       // in the bulk block (uploaded once to permanent VRAM by 0x170730)
    TEXK_Streamed = 3,   // in the record (uploaded every frame through 0x1ECE58 chains)
};

// orig3 record: 0x20 bytes (the group's table *0x474FA4, count 0x474FA0); 16 bytes from the stream (0x233EA8)
struct CLUT
{
    int32_t id = 0;              // +0x00
    uint8_t texk = 0;            // +0x04
    uint8_t b5 = 0;              // +0x05
    int8_t cfilter = 0;          // +0x06 S16 filters after the record
    int8_t ccond = 0;            // +0x07 S32 conditions after them
    uint64_t grf8 = 0;           // +0x08: bit 0 the colour transform (0x2338B8), bits 7..15 the colour count
    uint16_t ccolor = 0;         // (grf8 >> 7) & 0x1FF (<= 16: PSMT4 sampling, else PSMT8)
    bool fTransform = false;     // grf8 & 1
    uint32_t ibBulk = 0;         // +0x0C offset in the bulk block (TEXK_Bulk)
    // RGBA32 colours in upload order (CSM1: T8 indices have bits 3/4 swapped), adjusted (0x1ED278).
    std::shared_ptr<const std::vector<uint32_t>> pargba;
};

// orig3 record: 0x28 bytes (*0x474F9C, count 0x474F98); 0x18 bytes from the stream (0x234648)
struct BMP
{
    int32_t id = 0;              // +0x00
    uint8_t texk = 0;            // +0x04
    uint8_t b5 = 0;              // +0x05
    int8_t cfilter = 0;          // +0x06 S16 filters after the record
    int8_t ccond = 0;            // +0x07 S32 conditions after them
    uint16_t dx = 0, dy = 0;     // +0x08 / +0x0A
    uint8_t psm = 0;             // +0x0C transfer PSM (0x13 PSMT8, 0x14 PSMT4, 0 PSMCT32)
    uint8_t tbw = 0;             // +0x0D GS buffer width / 64
    uint16_t cgsbit = 0;         // +0x0E
    uint32_t cb = 0;             // +0x10 encoded byte count
    uint32_t ibBulk = 0;         // +0x14 offset in the bulk block (TEXK_Bulk)
    std::shared_ptr<const std::vector<uint8_t>> pab;   // the encoded pixels (exactly cb bytes)
};

struct SHD;

// A shader's texture state (0x14 bytes at SHD+0x18; Sly 1 TEX): per animation frame one BMP, and per frame and
// texture pass a CLUT (index frame * cclutPass + pass; null = none).
struct TEX
{
    int32_t oid = 0;             // +0x00 (raw first word)
    uint16_t grftex = 0;         // +0x04: 1 clamp S, 2 clamp T
    SHD* pshd = nullptr;         // +0x08
    std::vector<BMP*> apbmp;     // +0x0C (count +6)
    std::vector<CLUT*> apclut;   // +0x10 (count +7)
};

// The shader kinds (+4 of the SHD record; Sly 3 keeps Sly 2's numbers: the pass builder 0x235128 has the same
// cases); numbers differ from Sly 1's SHDK.
enum SHDK2 : uint8_t
{
    SHDK2_Plain = 0,        // one pass, vertex colour x material colour
    SHDK2_Lit = 1,          // two passes (two CLUTs), colours from the light bake
    SHDK2_Shadow = 2,       // two untextured passes (subtract)
    SHDK2_Volume = 3,       // four passes (projected volume)
    SHDK2_Plain4 = 4,
    SHDK2_Plain5 = 5,
    SHDK2_Plain6 = 6,
    SHDK2_ThreeWay = 7,     // three passes (two with flag 0x10; Sly 2: 0x20)
};

// A GS drawing pass of a shader, decoded from the register lists 0x1EB960 builds (TEST / ALPHA / prim / which
// vertex colour / which texture pass), so the renderer can set GL state without GS emulation.
struct SHDPASS
{
    bool fTextured = true;
    int itexPass = 0;            // the CLUT pass this pass samples (0x1EA6C8 j)
    int icolor = 0;              // 0: the vertex's colour A (qw 1), 1: colour B (qw 2), 2: constant rgba
    bool fNearest = false;       // TEX1 MMAG/MMIN (pass 2: nearest)
    uint64_t test = 0;           // GS TEST_1
    uint64_t alpha = 0;          // GS ALPHA_1
    bool fBlend = true;          // PRIM ABE
    uint32_t rgbaConst = 0;      // RGBAQ for untextured passes
    bool fFogBlack = false;      // FOGCOL 0 (the additive / pre-passes of kinds 1 and 7): fogged toward black, not the world's colour
};

// orig3 record: 0x2C bytes (*0x46FF3C, count 0x46FF38)
struct SHD
{
    int32_t id = 0;              // +0x00
    uint8_t shdk = 0;            // +0x04 SHDK2
    // +0x05, Sly 3's bits (0x235128 / 0x234A58 / 0x18FF98): 1 additive (ALPHA), 2 kind 3's textured first pass,
    // 4 kind 2's ZTST (Sly 2: 8), 8 a texture-frame list (0x234DA8), 0x10 kind 7 with two passes (Sly 2: 0x20),
    // 0x20 no alpha test (ATE off; new), 0x40 draw record. Sly 2's 4 (RGB only), 0x10 / 0x20 (kind 0's test
    // variants) and 0x80 are gone from the pass builder.
    uint8_t grfshd = 0;
    uint8_t b6 = 0;              // +0x06
    uint8_t b7 = 0;              // +0x07
    uint32_t argba[3] = {};      // +0x08 material colour (adjusted), +0x0C, +0x10
    uint16_t unSelfIllum = 0;    // +0x14
    uint8_t b16 = 0;             // +0x16
    std::vector<TEX> atex;       // +0x18 (count +0x17)
    int cregs = 0;               // +0x1C register-list count (passes)
    int cframe = 0;              // +0x24 = atex[0].apbmp.size()
    struct SAA* psaa = nullptr;  // +0x28
    std::vector<SHDPASS> apass;  // decoded register lists (0x1EB960)
};

// Shader animation (Sly 1 SAA; created by orig3 0x236EB0 from a class id of the class table 0x3409E0 and a
// 0x24-byte description (Sly 2: 0x20) read by the class's vtable slot 7; the base init 0x236FE0 and the classes'
// inits read the first 0x20 bytes as Sly 2's did).
struct SAA
{
    int cid = -1;                // the class (index of the class table)
    uint8_t ab[0x24] = {};       // the stream description
    int32_t oid = 0;             // ab+0 (-> +0x14)
    int32_t grf = 0;             // ab+4 (-> +0x18)
    int cframe = 0;              // ab+0xA (-> +0x1C)
    int n20 = 0;                 // ab+8 (-> +0x20): 1 -> entry flag 0x10, 2 -> 8
    bool fOwned = false;         // created with an owner (glob SAAs: 0x1ED7A8(1, pbis))

    // The state the VU receives (SAI: +0x24 du, +0x26 dv in 1/4096, +0x28 frame), see saa.cpp.
    int iframe = 0;
    int16_t du = 0, dv = 0;
    // Class state (LOOP/PINGPONG: rate, frame, pause; EYES: state machine; ...).
    float aflt[12] = {};
    int eyess = -1;
    float tEyess = 0;
    float tUpdated = -1;
};

// Shader-animation classes (Sly 3 cids of the class table 0x3409E0, base 0xAE; their vtables' init slot 7 / update
// slot 8; Sly 2's 139..144, same order). The names keep SAAK2_ for the copied saa.cpp.
enum SAAK2
{
    SAAK2_Loop = 0xAF,      // orig3: 0x237360 / 0x237408 (Sly 2 0x1EDB98 / 0x1EDC40)
    SAAK2_PingPong = 0xB0,  // orig3: 0x237528 / 0x2375D8
    SAAK2_Shuffle = 0xB1,   // orig3: 0x237760 / 0x2377A0
    SAAK2_Eyes = 0xB2,      // orig3: 0x237850 / 0x2379E8 (slot 10 0x237BE0)
    SAAK2_Scroller = 0xB3,  // orig3: 0x237C40 / 0x237CA0
    SAAK2_Circler = 0xB4,   // orig3: 0x237DE8 / 0x237E38
};

// orig: vtable slot 10, 0x1EDB80 (EYES 0x1EE420): the animation drives this shader (its id is the SAA's oid).
bool FSaaForShader(const SAA* psaa, const SHD* pshd);
// orig: vtable slot 7 (InitSaa 0x1ED8E0 + the class's own), called when it is read.
void InitSaa(SAA* psaa);
// orig: 0x001EE3B0 (an EYES animation's closedness: 1 shut, 0 open; t the game clock)
void SetSaaEyesClosed(SAA* psaa, float g, float t);
void SetScrollerMasterSpeeds(SAA* psaa, float su, float sv);  // orig 0x1EE678
void SetSaaOffsets(SAA* psaa, float du, float dv);             // orig 0x1EDAD8
struct ALO;
struct LO;
// orig: 0x001F80D0 (the object's and its ALO children's EYES animations, soundglue.cpp)
void SetAloEyesClosed(ALO* palo, float g);
// orig: 0x00210678 (SCPT 0x35, ASEG events: an object's eyes (oid around ploCtx) and / or an EYES animation by id)
void SetEyesClosedOf(LO* ploCtx, int oid, int idSaa, float g);
// orig: 0x1EC968 / vtable slot 8 (once per clock time, FUpdatableSaa 0x1EDA20): every animation of the world.
void UpdateShaderAnimations(float t, float dt);

// The world's texture data, as the loader leaves it after each group (the hash tables 0x3DD010 / 0x3DD018
// share records of the same id across groups).
struct SHDS
{
    std::vector<std::unique_ptr<CLUT>> aclut;
    std::vector<std::unique_ptr<BMP>> abmp;
    std::vector<std::unique_ptr<SAA>> asaa;
    std::vector<std::unique_ptr<SHD>> ashd;

    // The current group's tables (G_ACLUT / G_ABMP / G_ASAA / G_ASHD; reset per group by 0x1EC7A0).
    std::vector<CLUT*> apclutCur;
    std::vector<BMP*> apbmpCur;
    std::vector<SAA*> apsaaCur;
    std::vector<SHD*> apshdCur;

    // Id -> first loaded record (0x1ED4E0 / 0x1ED5D0).
    std::map<int32_t, CLUT*> mpidclut;
    std::map<int32_t, BMP*> mpidbmp;

    // The bulk block (0x20F5D0 header: cb, key; 0x20F640 data), by key: a group with an already loaded key
    // skips it and keeps using the earlier bytes.
    int32_t cbBulk = 0;
    int32_t keyBulk = -1;
    bool fLoadBulk = false;
    std::map<int32_t, std::shared_ptr<const std::vector<uint8_t>>> mpkeybulk;
};

extern SHDS g_shds;

// The world filter and the conditions: Sly 3's are game/cond3.h (FFilterMatches3 / FConditionsMatch3), which these
// loaders use. Sly 2's g_nFilterCur / FFilterMatches (brx.cpp, 0x13FAD0) stay for the copied Sly 2 code.
extern int g_nFilterCur;
bool FFilterMatches(int cfilter, const int16_t* afilter);
void SkipBytes(CBinaryInputStream* pbis, size_t cb);  // CBinaryInputStream::Skip

// Colour adjustment (0x3E12C8 g_fColorAdjust: off in the retail game's settings we compare against).
extern bool g_fColorAdjust;
void AdjustRgba(uint32_t* prgba);       // orig3: 0x236908 (Sly 2 0x1ED278)
void AdjustRgba16(uint16_t* prgba16);   // orig3: 0x236830 (Sly 2 0x1ED1A8)
// orig3: 0x2338B8 (a CLUT colour of a record with grf8 bit 0, through the condition set's tint, lightness /
// contrast and saturation in HLS; alpha kept)
void TransformClutRgba3(uint32_t* prgba);

// Entry points for the world loader.
void ResetShaders();                                            // world start: forget everything
void LoadBulkHeaderFromBrx(SW* psw, CBinaryInputStream* pbis, bool fLoad = true);  // orig3: 0x25C930 (S32 cb, S32 key)
void LoadShadersFromBrx(SW* psw, CBinaryInputStream* pbis);     // orig3: 0x234EA0 (includes the bulk block 0x25C9A0)
void EndShaderGroup();                                          // orig3: 0x235DE0 (after a group's objects)

// Lookups for the glob loader (current group).
SHD* PshdFromIndex(int ishd);           // orig3: 0x235F68
SHD* PshdFindShader(int32_t oid);       // orig3: 0x235E60
SAA* PsaaLoadFromBrx(bool fOwned, CBinaryInputStream* pbis);  // orig3: 0x236EB0

// The texture a pass samples: frame iframe of the state, texture pass itexPass (0x1EA6C8).
struct TEXSEL
{
    BMP* pbmp = nullptr;
    CLUT* pclut = nullptr;
    bool fClampS = false, fClampT = false;
};
TEXSEL TexselFromShd(const SHD* pshd, int iframe, int itexPass);

// Decodes a BMP with its CLUT to RGBA8 (raw PS2 alpha). Returns false (and a reason) for unsupported layouts.
bool FDecodeTexture(const BMP* pbmp, const CLUT* pclut, std::vector<uint8_t>& rgba, std::string& error);

// Statistics for --test-shd.
struct SHDSTATS
{
    int cclut = 0, cbmp = 0, csaa = 0, cshd = 0, cbulk = 0;
    int cfiltered = 0, cshared = 0, ctransformed = 0, cskippedPass = 0;
};
extern SHDSTATS g_shdstats;
