// See cond3.h (ported from the old Sly 3 port, 2026-10-28).

#include "cond3.h"

#include "sly3.h"

#include "../lo.h"
#include "../disc.h"

#include <algorithm>
#include <cmath>

#include <cstring>
#include <fstream>
#include <iterator>

CONDSET3 g_condset3;
int g_nFilterCur3 = sly3::WORLD_PROLOGUE;
uint32_t g_grfGame3 = 0;

// orig3: 0x151C98 FFilterMatches
bool FFilterMatches3(int cfilter, const int16_t* afilter)
{
    if (g_nFilterCur3 == -1) return true;
    if (cfilter == 0) return true;
    for (int i = 0; i < cfilter; ++i)
        if (afilter[i] == g_nFilterCur3) return true;
    return false;
}

// orig3: 0x169150
bool FConditionMatches3(int32_t cond)
{
    if (g_condset3.cond == -1 || cond == -1 || g_condset3.cond == cond) return true;
    for (int32_t c : g_condset3.acond)
        if (c == cond) return true;
    return false;
}

// orig3: 0x151D10
bool FConditionsMatch3(int ccond, const int32_t* acond)
{
    if (ccond == 0) return true;
    for (int i = 0; i < ccond; ++i)
        if (FConditionMatches3(acond[i])) return true;
    return false;
}

// orig3: 0x234500
int ItexPass3() { return (g_grfGame3 & 3) == 0 ? 1 : 0; }

namespace
{
// orig3: 0x11F4E0 (h 0..360, s and v 0..1; Sly 2's ConvertHsvToRgb)
void RgbFromHsv3(float h0, float s0, float v0, float* prgb)
{
    float h = !(0.0f <= h0) ? -1.0f : (h0 > 360.0f ? 360.0f : h0);
    const float s = std::clamp(s0, 0.0f, 1.0f), v = std::clamp(v0, 0.0f, 1.0f);
    float r = v, g = v, b = v;
    if (s != 0.0f)
    {
        if (h == 360.0f) h = 0.0f;
        h *= 1.0f / 60.0f;
        const int i = int(std::floor(h));
        const float f = h - float(i);
        const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
        switch (i)
        {
        case 0: case 6: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        default: r = g = b = 0.0f; break;
        }
    }
    prgb[0] = r; prgb[1] = g; prgb[2] = b;
}

constexpr float K255 = 1.0f / 255.0f;   // 0x3B808081
}  // namespace

bool FApplyCondSetOption3(const OPT& opt)
{
    if (opt.aval.empty()) return false;
    const OPTVAL& v = opt.aval[0];
    const uint32_t va = 0x32CA80 + uint32_t(opt.eopid) * sly3::CB_EOPID;   // orig3: the option descriptors
    const uint32_t grf = g_disc.U32Elf(va + 4);
    if (grf & 0x80)
    {
        CONDSET3& c = g_condset3;
        switch (g_disc.U32Elf(va + 0x18))
        {
        case 0x14: c.fogNear = v.ag[0]; return true;
        case 0x18: c.fogFar = v.ag[0]; return true;
        case 0x1C: c.fogMax = v.ag[0]; return true;
        case 0x20: c.rgbaFog = uint32_t(v.n); return true;
        case 0x38: c.gLightness = v.ag[0]; return true;
        case 0x3C: c.gContrast = v.ag[0]; return true;
        case 0x40: c.gSaturation = v.ag[0]; return true;
        case 0x60: for (int i = 0; i < 3; ++i) c.hsvWorld[i] = v.ag[size_t(i)]; return true;   // (grf 0xA2: also 0x169460)
        case 0x70: c.gWorldScale = v.ag[0]; return true;
        case 0x74: c.gWorldAmbient = v.ag[0]; return true;
        case 0x8C: c.gDesaturate = v.ag[0]; return true;
        default: return false;
        }
    }
    switch (g_disc.U32Elf(va + ((grf & 0x100) ? 0x1C : 0x14)))
    {
    case 0x1691B0: g_condset3.rgbaFog = uint32_t(v.n); return true;   // (then AdjustRgba 0x236908: off, GAME +0xC bit 1)
    case 0x1691E0:
    {
        float rgb[3];
        RgbFromHsv3(v.ag[0], v.ag[1] * K255, v.ag[2] * K255, rgb);
        uint32_t u = 0x80000000u;   // 0x11F288: each channel int(x x 255 + 0.5), alpha 0x80
        for (int i = 0; i < 3; ++i) u |= (uint32_t(int(rgb[i] * 255.0f + 0.5f)) & 0xFF) << (8 * i);
        g_condset3.rgbaFog = u;
        return true;
    }
    case 0x1694D8:   // a condition more (the list +0xDC / +0xE0, unless it already matches)
        if (!FConditionMatches3(v.n)) g_condset3.acond.push_back(v.n);
        return true;
    default: return false;
    }
}

void WorldShader3(float* prgbw)
{
    const CONDSET3& c = g_condset3;
    RgbFromHsv3(c.hsvWorld[0], c.hsvWorld[1] * K255, c.hsvWorld[2] * K255, prgbw);
    for (int i = 0; i < 3; ++i) prgbw[i] *= c.gWorldScale * K255;
    prgbw[3] = c.gWorldAmbient * K255;
}

bool FLoadCondSet3FromRam(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ram.size() < 0x2000000)
    {
        error = path + ": not a 32 MB EE RAM dump";
        return false;
    }
    auto u32 = [&](uint32_t va) { uint32_t u; std::memcpy(&u, ram.data() + (va & 0x1FFFFFF), 4); return u; };
    auto f32 = [&](uint32_t va) { float f; std::memcpy(&f, ram.data() + (va & 0x1FFFFFF), 4); return f; };
    const uint32_t s = sly3::VA_CM_STATIC;
    CONDSET3 c;
    c.cond = int32_t(u32(s + 0x10));
    const int n = int(u32(s + 0xDC));
    for (int i = 0; i < n && i < 64; ++i) c.acond.push_back(int32_t(u32(s + 0xE0 + 4 * i)));
    c.fogNear = f32(s + 0x14);
    c.fogFar = f32(s + 0x18);
    c.fogMax = f32(s + 0x1C);
    c.rgbaFog = u32(s + 0x20);
    c.gLightness = f32(s + 0x38);
    c.gContrast = f32(s + 0x3C);
    c.gSaturation = f32(s + 0x40);
    for (int i = 0; i < 3; ++i) c.rgbTint[i] = f32(s + 0x50 + 4 * i);
    for (int i = 0; i < 3; ++i) c.hsvWorld[i] = f32(s + 0x60 + 4 * i);
    c.gWorldScale = f32(s + 0x70);
    c.gWorldAmbient = f32(s + 0x74);
    c.gDesaturate = f32(s + 0x8C);
    g_condset3 = c;
    g_nFilterCur3 = int32_t(u32(sly3::VA_WORLD_INDEX));
    g_grfGame3 = u32(sly3::VA_GAME_FLAGS + 0xC);
    return true;
}
