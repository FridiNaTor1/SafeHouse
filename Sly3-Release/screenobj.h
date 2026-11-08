#pragma once

// Screen objects (Sly 2 module 0x210E90-0x2155C0; Sly 3: classes 0x9E..0xA7, orig3 0x260810, read into these Sly 2
// structs: see screenobj.cpp): the 2D HUD / menu / font assets of the world files, a tree of
// 0x40-byte elements (vector sprites, textured quads, nested element sets such as the fonts, transforms) and the
// animated group tree that places them (groups -> parts -> keys -> items with 2x3 transforms and colours).
// Loaded natively from the stream (0x212298 / 0x214398 / 0x2149D8 / 0x214460); drawn by hud.cpp.

#include "bis.h"
#include "disc.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A vector polygon of a sprite element (kind 1): the 2D VU program P_K21 draws it as a fan, strip or line strip
// of GS 12.4 points through the draw's 2x3 matrix, colour = clamp(rgba x scale + offset).
struct SPOLY
{
    uint8_t primitive = 0;      // +0x00: 1 fan, 2 strip, 3 line strip (AA1 when the final alpha has bit 7)
    uint8_t unused = 0;         // +0x04
    uint8_t control = 0;        // +0x14: layer (z = control x 100 + the draw's depth)
    uint8_t fVertexBytes = 0;   // +0x1C: per-vertex bytes present (bit 7: no kick / ADC)
    uint32_t rgba = 0;          // +0x08 (the last vertex's word)
    std::vector<std::array<int16_t, 2>> axy;   // 12.4 fixed
    std::vector<uint8_t> ab;    // per-vertex bytes
};

enum SELEMK : uint32_t
{
    SELEMK_Sprite = 1,          // vector polygons
    SELEMK_Transform = 2,       // a 2x3 matrix and 0x18-byte records
    SELEMK_Sound = 3,           // a sound / timing record
    SELEMK_Ref = 4,             // a reference (U16)
    SELEMK_Image = 6,           // a textured quad (texture id), blended through the alpha mask
    SELEMK_Quad = 7,            // a textured quad (texture id)
    SELEMK_Set = 8,             // a nested element set with a U32 list and an F32 list (a CFontBrx font: characters, widths)
};

struct SELEM
{
    uint32_t idx = 0;           // +0x00
    uint32_t kind = 0;          // +0x04
    // kind 1
    std::vector<SPOLY> apoly;
    // kind 2
    glm::mat3x2 mat{1.0f};
    std::vector<std::array<uint8_t, 0x18>> arec;
    // kind 3
    uint32_t aul3[2] = {};
    float ag3[3] = {};
    int8_t ab3[3] = {};         // +0x24 halign, +0xC kind (0 localized, 1 script string), (Sly 3) +0x2C the font element
    int32_t n3 = 0;
    // kind 4
    uint16_t ref = 0;
    // (Sly 3) kind 1 / 4: the S32 before the data
    int32_t n = 0;
    // kind 6 / 7
    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int32_t idTexture = 0;
    // kind 8
    std::vector<SELEM> asub;    // by index (0..n)
    std::vector<uint32_t> aul;  // the font's characters
    std::vector<float> ag;      // the font's advance widths
};

// The animated placement tree (0x214460).
struct SITEM
{
    uint16_t aw[3] = {};
    int16_t s6 = 0;
    uint8_t b30 = 0, b31 = 0;
    int32_t n34 = 0;
    glm::mat3x2 mat{1.0f};
};
struct SKEY
{
    float t = 0;
    std::vector<SITEM> aitem;
    std::vector<std::array<uint32_t, 2>> acolor;
    std::vector<uint16_t> aw;
    std::vector<uint8_t> ab;
    std::vector<std::pair<uint8_t, int32_t>> atag;
};
struct SPART
{
    float dt = 0;
    int32_t id = 0;
    std::vector<SKEY> akey;
};
struct SGROUP
{
    uint16_t cframeSet = 0, cframe = 0;
    std::vector<SPART> apart;
};

struct SCREENOBJ
{
    int32_t oid = -1;
    size_t ib = 0;              // where its load starts in the world file
    int8_t fExternal = 0;       // its data is another file (key)
    float g90 = 0;
    uint32_t ul94 = 0, ul98 = 0, ulBC = 0;
    FK fk;                      // fExternal: the file
    std::vector<SELEM> aelem;   // by index (0..n)
    std::vector<SGROUP> agroup;
};

// orig3: 0x00260810 (after the LO's options 0x1C7D98): entry point for the world loader (Sly 2: 0x212298).
void LoadScreenObjFromBrx(SCREENOBJ* pso, CBinaryInputStream* pbis);

// orig: the external screen file (0x118458's key, loaded when the object is shown): "ZRB." + U16, the element set
// and the group tree. Fills an external object's elements / groups.
bool FLoadScreenFile(SCREENOBJ* pso, const std::vector<uint8_t>& abDecompressed, std::string& error);

// A CFontBrx font: a set element whose U32 list is the characters (sorted) and F32 list their widths.
struct FONTBRX
{
    const SCREENOBJ* pso = nullptr;
    const SELEM* pset = nullptr;
    int ifont = 0;
    // (a character: a byte, or the Japanese build's 16-bit characters: region.h ChDecode)
    const SELEM* PglyphFromCh(uint32_t ch) const;  // orig: 0x00216B40 (via PglyffFromCh 0x215A50)
    float DxFromCh(uint32_t ch) const;             // orig: 0x00216B80
};
// Fonts found among loaded screen objects (sets with a sorted character list of the same size as their widths).
std::vector<FONTBRX> FindFonts(const std::vector<std::shared_ptr<SCREENOBJ>>& aso);
