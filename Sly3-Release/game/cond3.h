#pragma once

// Sly 3's world state that the texture / mesh loaders test and use (owner: the Sly 3 renderer; the world loader
// (the Sly 3 class loaders) fills it):
// - the world filter (orig3 0x25E150: the S32 at 0x47989C, the index of the world being loaded; -1 everything
//   matches), tested by FFilterMatches 0x151C98 against an object's / CLUT's / BMP's / glob's S16 filter list;
// - the condition set (orig3 0x36BC10, loaded from the chosen block of the option sets 0x1500F0), tested by 0x151D10
//   (any of a list) / 0x169150 (one condition) against its current condition (+0x10) and its list (+0xDC count, +0xE0);
// - the colour and fog parameters the same record carries (CLUT colour transform 0x2338B8, the shaders' FOGCOL
//   0x235128, the fog the mesh loaders' BuildCmFogRecip 0x1692E8 reads).
//
// Defaults: the values of PCSX2's RAM at m_ext's title, binocucom and gameplay (three savestates: all
// three are the same), until the option-set loader fills the record.

#include <cstdint>
#include <string>
#include <vector>

struct CONDSET3
{
    int32_t cond = 13;               // +0x10 (-1: every condition matches)
    std::vector<int32_t> acond;      // +0xE0 [+0xDC]
    float fogNear = 5500.0f;         // +0x14 (guess: the fog's start / end / limit, as Sly 2's CM +0x240..+0x248)
    float fogFar = 60000.0f;         // +0x18
    float fogMax = 0.4f;             // +0x1C
    uint32_t rgbaFog = 0x80965515u;  // +0x20 (the shaders' FOGCOL, 0x235128)
    float gLightness = 1.5f;         // +0x38 (0x2338B8: L x this)
    float gContrast = 1.2f;          // +0x3C (0x2338B8: (L - this / 4) x contrast + this / 4)
    float gSaturation = 1.4f;        // +0x40 (0x2338B8: S x this unless GAME +0xC bit 1)
    float rgbTint[3] = {0, 0, 0};    // +0x50 / +0x54 / +0x58 (0x2338B8's per-channel curve; 0 = none)
    // the world shader colour (0x169398 -> +0x78 rgb / +0x84 w, light3.h): HSV +0x60 (setter 0x169460; s and v 0..255),
    // the colour's scale +0x70 (0x1694A0) and the ambient +0x74 (0x169480), both / 255
    float hsvWorld[3] = {210.0f, 150.0f, 100.0f};
    float gWorldScale = 60.0f;       // +0x70
    float gWorldAmbient = 35.0f;     // +0x74
    float gDesaturate = 0.0f;        // +0x8C (0x2369D8's amount, used only when GAME +0xC bit 1)
};

extern CONDSET3 g_condset3;
extern int g_nFilterCur3;          // orig3: 0x47989C (via 0x25E150)
extern uint32_t g_grfGame3;        // orig3: GAME 0x36DBE0 +0xC (0 in the PCSX2 runs: texture pass 1, saturation on)

// orig3: 0x151C98 FFilterMatches (no filter, the current one -1, or the current one in the list)
bool FFilterMatches3(int cfilter, const int16_t* afilter);
// orig3: 0x169150 (one condition: the set's is -1, it is -1, it is the set's, or it is in the set's list)
bool FConditionMatches3(int32_t cond);
// orig3: 0x151D10 (no conditions, or any of them matches)
bool FConditionsMatch3(int ccond, const int32_t* acond);
// orig3: 0x234500 (the CLUT / BMP copy the loader keeps: 1 when GAME +0xC & 3 == 0, else 0)
int ItexPass3();

// orig3: the static camera's options (the world's chosen option block, 0x1500F0) as the original's descriptors apply
// them (0x32CA80: grf 0x80 a field at +0x18, else the setter +0x14 / +0x1C): +0x14 / +0x18 / +0x1C the fog (EOPIDs
// 1010 / 1011 / 1014), +0x20 the fog colour (1012: 0x1691B0 RGBA; 1013: 0x1691E0 from HSV, s / v 0..255), the world
// shader's HSV 0x169460 (1015), ambient 0x169480 (1016), scale 0x1694A0 (1017), a condition added 0x1694D8 (1029).
// true: the option is one of them.
struct OPT;
bool FApplyCondSetOption3(const OPT& opt);
// orig3: 0x169398 the world shader colour (camera +0x78 rgb, +0x84 w): RgbFromHsv(h, s / 255, v / 255) x scale / 255,
// w = ambient / 255 (0x2369D8's desaturation only with GAME +0xC bit 1: off in the released game)
void WorldShader3(float* prgbw);

// Fills g_condset3 / g_nFilterCur3 / g_grfGame3 from an EE RAM dump (32 MB eeMemory.bin of PCSX2). false: unreadable.
bool FLoadCondSet3FromRam(const std::string& path, std::string& error);
