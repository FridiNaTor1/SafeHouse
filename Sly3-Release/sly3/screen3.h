#pragma once

// Sly 3's screen objects (classes 0x9E..0xA7, all loaded by orig3 0x260810; Sly 2's LoadScreenObjFromBrx 0x212298,
// 0.70): the 2D HUD / menu / font assets. Re-derived from the Sly 3 asm (the Sly 3 renderer, 2026-10-28):
// - 0x260810: LoadLoFromBrx 0x1C7D98 (the LO's options) first, then S8 external, F32 +0x80, U32 +0x84, U32 +0x88,
//   U32 +0xAC; external: the file's 64-byte key record (0x119948, AppendSlEntry), else the element set 0x263118 and the
//   group tree 0x2631C8.
// - 0x263118: U16 count; count x {U16 index, element 0x263748} (0x30-byte elements, index 0..count).
// - 0x263748 LoadElemFromBrx: U32 kind (jump table 0x327F40), then by kind: 1 a vector sprite (S32, U16 polygons x
//   {U8 vertices, U8 control, U8 primitive, U8, U8 vertex-bytes; per vertex U32 colour (0x236970), U8, S16 x, S16 y});
//   3 a sound / timing record (U32 colour, F32 x 3, S8 x 3, then S32 when the first S8 is 0 or 1); 4 a reference
//   (S32, U16); 6 / 7 textured quads (S16 x 4, S32 texture); 8 a set: a **font** (U8 mode, U16 count x {U16 character
//   (16-bit: Sly 3's text is UTF-16), F32 advance width, a nested element without index}; a character the world's
//   text does not use (0x14FEB0) is read into a scratch element and dropped); 0, 2, 5: no data.
// - 0x2631C8: U16 groups x {U16 frame sets, U16 frames, U16 parts x {F32 dt, S32 id, U16 keys x {F32 t, U8 items,
//   U8 colours, U8 bytes, U8 (bit 0 -> flag), U8 (3 bits), S32, items x {U16 (10 bits), U8, U8 (6 bits), U8 (1 bit),
//   U8 (<= 0x7F), S32, a 3x3 matrix}, colours x {U32, U32} (0x236970 each), bytes x U8}}}.
// Checked: every screen object of all 39 worlds ends exactly at its record's size (--screen-world3 all).

#include "brx3.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class CBinaryInputStream;

struct SVTX3
{
    uint32_t rgba = 0;
    uint8_t b = 0;
    int16_t x = 0, y = 0;
};
struct SPOLY3
{
    uint8_t control = 0, primitive = 0, unused = 0, fVertexBytes = 0;
    std::vector<SVTX3> avtx;
};

struct SELEM3
{
    uint32_t kind = 0;
    int32_t n = 0;                   // kind 1 / 4: the S32
    std::vector<SPOLY3> apoly;       // kind 1
    uint16_t ref = 0;                // kind 4
    uint32_t rgba3 = 0;              // kind 3
    float ag3[3] = {};
    int8_t ab3[3] = {};
    int32_t n3 = 0;
    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // kind 6 / 7
    int32_t idTexture = 0;
    int mode = 0;                    // kind 8: the font's mode (1: only the characters the text uses; 2: by language)
    std::vector<uint16_t> ach;       // kind 8: the characters (as stored: the font's order)
    std::vector<float> adx;          // kind 8: their advance widths
    std::vector<SELEM3> aglyph;      // kind 8: their glyphs
};

struct SITEM3
{
    uint16_t w = 0;
    uint8_t ab[4] = {};
    int32_t n = 0;
    glm::mat3 mat{1.0f};
};
struct SKEY3
{
    float t = 0;
    uint8_t bFlag = 0, b3 = 0;
    int32_t n = 0;
    std::vector<SITEM3> aitem;
    std::vector<std::array<uint32_t, 2>> acolor;
    std::vector<uint8_t> ab;
};
struct SPART3
{
    float dt = 0;
    int32_t id = 0;
    std::vector<SKEY3> akey;
};
struct SGROUP3
{
    uint16_t cframeSet = 0, cframe = 0;
    std::vector<SPART3> apart;
};

struct SCREENOBJ3
{
    int cid = 0;
    int32_t oid = -1;
    int8_t fExternal = 0;
    float g80 = 0;
    uint32_t ul84 = 0, ul88 = 0, ulAC = 0;   // (m_ext's HUD: 640, 480: the canvas)
    std::vector<uint8_t> abKey;      // external: the 64-byte key record
    std::vector<SELEM3> aelem;       // by index (0..count)
    std::vector<SGROUP3> agroup;
};

// orig3: 0x260810 (after the LO's options): the body of a screen object
void LoadScreenObj3(SCREENOBJ3& so, CBinaryInputStream* pbis);

// A font of a screen object: a set element (kind 8). PglyphFromCh: the glyph of a 16-bit character (null: none).
struct FONT3
{
    const SCREENOBJ3* pso = nullptr;
    const SELEM3* pset = nullptr;
    int ielem = 0;
    const SELEM3* PglyphFromCh(uint16_t ch) const;
    float DxFromCh(uint16_t ch) const;
};
std::vector<FONT3> FindFonts3(const std::vector<SCREENOBJ3>& aso);

// The screen objects of a walked world (their bodies at the walk's offsets); false on a read error (error says where).
// cMismatch: objects whose reads do not end at their record's size.
bool FLoadScreenObjs3(const std::vector<uint8_t>& ab, const WORLDWALK3& w, std::vector<SCREENOBJ3>& aso, int& cMismatch,
                      std::string& error);

// --screen-world3 W|all: loads every world's screen objects, checks their sizes, lists the fonts.
// An external screen object's file (its 64-byte key: an LZSS file like a world's, "ZRB." + U16, then the element set
// 0x263118 and the group tree 0x2631C8 as in the world file) into so.aelem / so.agroup. false: error says why.
bool FLoadScreenFile3(SCREENOBJ3& so, std::string& error, size_t* pcbFile = nullptr);
int ScreenWorld3Command(const std::string& arg, bool fVerbose);

// --text-test3 W TEXT|#id OUT.png: the string (UTF-8) or the world's text id in the world's text font (FindFonts3's
// first: m_ext's oid 1061 element 0, 92 characters up to U+2026), drawn by a software rasterizer, wrapped at 600.
int TextTest3Command(const std::string& world, const std::string& text, const std::string& out);
