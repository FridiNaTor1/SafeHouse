// See condset3.h.

#include "condset3.h"

#include "addr.h"

#include <cstring>
#include <fstream>
#include <iterator>

CONDSET3 g_condset3;
int g_nFilterCur3 = orig3::WORLD_PROLOGUE;
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
    const uint32_t s = orig3::VA_CONDITIONS;
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
    g_nFilterCur3 = int32_t(u32(0x47989C));
    g_grfGame3 = u32(orig3::VA_GAME + 0xC);
    return true;
}
