#pragma once

// The world's lights at run time, natively (module 0x1A3260..0x1A4E08, Sly 1's light.c): each light's 3-qw record
// (LIGHT +0x120) computed from its options and transform as 0x1A3968 does, the default light 0x1A3260, the world
// shader colour SW+0x3E28 (0x20F1C8), handed to light.h (SetWorldLight with the light context's selection data
// for 0x1A3F78). Replaces the reference RAM as the lighting's source.

struct SW;
struct LO;

// The light's fields (orig offsets) from its loaded options (EOPIDs 151..158 of class 4).
struct LIGHTPARAMS
{
    int kind = 0;                   // +0xE0 (EOPID 151, setter 0x1A35B0): 0 directional, 1 point
    float hsv[3] = {};              // +0xF0 (152): hue 0..360, saturation and value 0..255
    float strength = 0;             // +0x150 (153)
    float angle = 240;              // +0x154 (154, degrees)
    int n158 = 1;                   // +0x158 (156): casts the shadow direction (0x1A4478)
    int fNoContext = 0;             // +0x15C (157): not a light-context candidate (0x1A3D00)
    int fNoBake = 0;                // +0x160 (158): not baked (Glob_UploadMatrices 0x167FC0), not the world's light
    float sNear = 200, sFar = 2000; // +0x164 / +0x168 (155): point falloff
};
LIGHTPARAMS LightParamsOfLo(const LO* plo);
// One light's record and selection data as 0x1A3968 computes them (false: not a light kind 0 / 1).
struct LIGHTREC;
struct LIGHTSEL;
bool FLightOfLo(const LO* plo, LIGHTREC& rec, LIGHTSEL& sel);

// orig: 0x001A3968 + 0x001A3260 + 0x0020F1C8: every light of the world into light.h (ResetWorldLights first),
// the world shader colour from the world's options. Returns the number of lights.
int UpdateWorldLightsFromSw(const SW* psw);

// Checks the records against a reference RAM after the load (by oid; prints the differences). Returns the
// number of lights that differ.
int CheckLightsAgainstRam(const SW* psw, const char* ramPath, bool fVerbose);
