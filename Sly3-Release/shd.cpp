// Textures and shaders: Sly 2's module 0x1EA568-0x1ED7A8 (Sly 1 shd.c) as native loaders. See shd.h.

#include "shd.h"
#include "game/sly3.h"
#include "worldglobals.h"

#include "loadtrace.h"
#include "game/cond3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

SHDS g_shds;
WORLD_TABLE(g_shds);   // (PC: moved aside while a kept world loads, worldglobals.h)
SHDSTATS g_shdstats;
WORLD_VALUE(g_shdstats);   // (PC: kept across a kept world's load, worldglobals.h)
bool g_fColorAdjust = false;

namespace
{
// RGBA <-> bytes, little-endian as on the PS2.
inline uint8_t Byte(uint32_t rgba, int i) { return static_cast<uint8_t>(rgba >> (8 * i)); }
}

// (g_nFilterCur, FFilterMatches and SkipBytes are defined with the object loader, brx.cpp)

// orig: 0x001ED278 (a colour through the colour adjustment, when on: 0x11DCD8 RGBA -> VECTOR, 0x1ED2D8, 0x11DD30)
void AdjustRgba(uint32_t* prgba)
{
    if (!g_fColorAdjust) return;
    // The adjustment (luminance raised to D_002C3698 and limited, back through HSV) is off in the retail
    // configuration (0x3E12C8 = 0 in the reference's RAM after loading Paris). Not ported yet.
    (void)prgba;
}

// orig: 0x001ED1A8 (an RGB5A1 colour through the colour adjustment)
void AdjustRgba16(uint16_t* prgba16)
{
    if (!g_fColorAdjust) return;
    const uint32_t w = *prgba16;
    uint32_t rgba = ((w & 0x1F) << 3) | (((w >> 2) & 0xF8) << 8) | (((w >> 7) & 0xF8) << 16) | ((w & 0x8000) ? 0xFF000000u : 0);
    AdjustRgba(&rgba);
    const uint32_t r = Byte(rgba, 0), g = Byte(rgba, 1), b = Byte(rgba, 2), a = Byte(rgba, 3);
    *prgba16 = static_cast<uint16_t>(((g << 2) & 0x3E0) | (r >> 3) | ((b >> 3) << 10) | ((a >> 7) << 15));
}

void ResetTextureCache();  // render.cpp

void ResetShaders()
{
    // (PC addition: a kept world's load, pcloader.h, keeps the running world's GPU textures; its own records are new)
    if (!g_fKeptWorldLoad) ResetTextureCache();
    g_shds = SHDS{};
    g_shdstats = SHDSTATS{};
}

// orig3: 0x235DE0 (forgets the group's shaders, BMPs, CLUTs and shader animations; the records live on)
void EndShaderGroup()
{
    g_shds.apclutCur.clear();
    g_shds.apbmpCur.clear();
    g_shds.apsaaCur.clear();
    g_shds.apshdCur.clear();
}

namespace
{
// orig3: 0x2338B8 helpers: 0x11F230 unpack_rgb (x / 255), 0x11F288 pack_rgba ((x * 255 + 0.5) truncated), 0x11EE68
// GLimitLm, 0x11F770 RGB -> HLS (H in degrees, L, S: Foley / van Dam), 0x11FA88 HLS -> RGB (0x11F970 the hue ramp)
float GLimit3(float g, float lo, float hi) { return g < lo ? lo : (hi < g ? hi : g); }

void HlsFromRgb3(const float rgb[3], float hls[3])
{
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const float d = mx - mn;
    const float l = (mx + mn) * 0.5f;
    if (d == 0.0f) { hls[0] = 0.0f; hls[1] = l; hls[2] = 0.0f; return; }
    const float s = l <= 0.5f ? d / (mx + mn) : d / (2.0f - mx - mn);
    float h;
    if (r == mx) h = (g - b) / d;
    else if (g == mx) h = 2.0f + (b - r) / d;
    else h = 4.0f + (r - g) / d;
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;
    hls[0] = h; hls[1] = l; hls[2] = s;
}

float HueRamp3(float m1, float m2, float h)
{
    if (h > 360.0f) h -= 360.0f;
    if (h < 0.0f) h += 360.0f;
    if (h < 60.0f) return m1 + (m2 - m1) * h / 60.0f;
    if (h < 180.0f) return m2;
    if (h < 240.0f) return m1 + (m2 - m1) * (240.0f - h) / 60.0f;
    return m1;
}

void RgbFromHls3(const float hls[3], float rgb[3])
{
    const float h = hls[0], l = hls[1], s = hls[2];
    if (s == 0.0f) { rgb[0] = rgb[1] = rgb[2] = l; return; }
    const float m2 = l <= 0.5f ? l + l * s : l + s - l * s;
    const float m1 = 2.0f * l - m2;
    rgb[0] = HueRamp3(m1, m2, h + 120.0f);
    rgb[1] = HueRamp3(m1, m2, h);
    rgb[2] = HueRamp3(m1, m2, h - 120.0f);
}
}  // namespace

// orig3: 0x2338B8
void TransformClutRgba3(uint32_t* prgba)
{
    const CONDSET3& c = g_condset3;
    float rgb[3];
    for (int i = 0; i < 3; ++i) rgb[i] = float(Byte(*prgba, i)) * (1.0f / 255.0f);
    // the per-channel curve: d = channel - mean of the other two (of the tint); d != 0: rgb = (t pow(2c, e) + (1 - t)
    // 2c) / 2 with t = |d| / 100 limited to 1, e = 1/3 for d > 0, else 1.8
    const float d[3] = {c.rgbTint[0] - (c.rgbTint[1] + c.rgbTint[2]) * 0.5f, c.rgbTint[1] - (c.rgbTint[0] + c.rgbTint[2]) * 0.5f,
                        c.rgbTint[2] - (c.rgbTint[0] + c.rgbTint[1]) * 0.5f};
    for (int i = 0; i < 3; ++i)
    {
        if (d[i] == 0.0f) continue;
        float t = GLimit3(d[i] * 0.01f, -1.0f, 1.0f);
        const float c2 = rgb[i] + rgb[i];
        float e;
        if (0.0f < t) e = 1.0f / 3.0f;
        else { e = 1.8f; t = -t; }
        rgb[i] = (t * std::pow(c2, e) + (1.0f - t) * c2) * 0.5f;
    }
    float hls[3];
    HlsFromRgb3(rgb, hls);
    const float q = c.gLightness * 0.25f;
    hls[1] = GLimit3((hls[1] * c.gLightness - q) * c.gContrast + q, 0.0f, 1.0f);
    if (!((g_grfGame3 >> 1) & 1)) hls[2] = GLimit3(hls[2] * c.gSaturation, 0.0f, 1.0f);
    RgbFromHls3(hls, rgb);
    uint32_t out = *prgba & 0xFF000000u;
    for (int i = 0; i < 3; ++i) out |= uint32_t(uint8_t(int32_t(rgb[i] * 255.0f + 0.5f))) << (8 * i);
    *prgba = out;
}

namespace
{
// orig3: 0x233EA8 (a CLUT record: 16 bytes, its S16 filters and S32 conditions; dropped (texk 0) when they fail;
// shared with a loaded one of the same id (0x236CF0), or its colours read, transformed (grf8 bit 0) and adjusted)
void LoadClutFromBrx(CLUT* pclut, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x233EA8, pbis);
    uint8_t ab[0x10];
    pbis->ReadBytes(ab, sizeof(ab));
    std::memcpy(&pclut->id, ab + 0x0, 4);
    pclut->texk = ab[0x4];
    pclut->b5 = ab[0x5];
    pclut->cfilter = static_cast<int8_t>(ab[0x6]);
    pclut->ccond = static_cast<int8_t>(ab[0x7]);
    std::memcpy(&pclut->grf8, ab + 0x8, 8);
    pclut->ccolor = uint16_t((pclut->grf8 >> 7) & 0x1FF);
    pclut->fTransform = (pclut->grf8 & 1) != 0;
    std::memcpy(&pclut->ibBulk, ab + 0xC, 4);
    const uint32_t cb = uint32_t(pclut->ccolor) * 4;
    int16_t afilter[128];
    int32_t acond[128];
    pbis->ReadBytes(afilter, size_t(std::max<int>(pclut->cfilter, 0)) * 2);
    pbis->ReadBytes(acond, size_t(std::max<int>(pclut->ccond, 0)) * 4);
    if (!FFilterMatches3(pclut->cfilter, afilter) || !FConditionsMatch3(pclut->ccond, acond))
    {
        if (pclut->texk != TEXK_Bulk) SkipBytes(pbis, cb);
        pclut->texk = TEXK_Filtered;
        ++g_shdstats.cfiltered;
        return;
    }
    if (pclut->texk == TEXK_Bulk) return;
    auto it = g_shds.mpidclut.find(pclut->id);
    if (it != g_shds.mpidclut.end() && it->second->pargba)
    {
        pclut->pargba = it->second->pargba;
        SkipBytes(pbis, cb);
        ++g_shdstats.cshared;
        return;
    }
    auto argba = std::make_shared<std::vector<uint32_t>>(pclut->ccolor);
    pbis->ReadBytes(argba->data(), cb);
    if (pclut->fTransform)
    {
        for (uint32_t& rgba : *argba) TransformClutRgba3(&rgba);
        ++g_shdstats.ctransformed;
    }
    for (uint32_t& rgba : *argba) AdjustRgba(&rgba);
    pclut->pargba = std::move(argba);
    g_shds.mpidclut[pclut->id] = pclut;
}

// orig3: 0x234518 LoadClutsFromBrx (S32 cb, then U16 records; the copy of the other pass (0x234500) is skipped by cb)
void LoadClutsFromBrx(CBinaryInputStream* pbis, int ipass)
{
    LOAD_SCOPE(0x234518, pbis);
    const int32_t cb = pbis->S32Read();
    if (ItexPass3() != ipass)
    {
        SkipBytes(pbis, size_t(cb));
        ++g_shdstats.cskippedPass;
        return;
    }
    const int c = pbis->U16Read();
    g_shds.apclutCur.clear();
    for (int i = 0; i < c; ++i)
    {
        auto pclut = std::make_unique<CLUT>();
        LoadClutFromBrx(pclut.get(), pbis);
        g_shds.apclutCur.push_back(pclut.get());
        g_shds.aclut.push_back(std::move(pclut));
    }
    g_shdstats.cclut += c;
}

// orig3: 0x234648 (a BMP record: 0x18 bytes, its filters and conditions (dropped when they fail), shared with a
// loaded one of the same id (0x236C18), or its pixels read)
void LoadBmpFromBrx(BMP* pbmp, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x234648, pbis);
    uint8_t ab[0x18];
    pbis->ReadBytes(ab, sizeof(ab));
    std::memcpy(&pbmp->id, ab + 0x0, 4);
    pbmp->texk = ab[0x4];
    pbmp->b5 = ab[0x5];
    pbmp->cfilter = static_cast<int8_t>(ab[0x6]);
    pbmp->ccond = static_cast<int8_t>(ab[0x7]);
    std::memcpy(&pbmp->dx, ab + 0x8, 2);
    std::memcpy(&pbmp->dy, ab + 0xA, 2);
    pbmp->psm = ab[0xC];
    pbmp->tbw = ab[0xD];
    std::memcpy(&pbmp->cgsbit, ab + 0xE, 2);
    std::memcpy(&pbmp->cb, ab + 0x10, 4);
    std::memcpy(&pbmp->ibBulk, ab + 0x14, 4);
    int16_t afilter[128];
    int32_t acond[128];
    pbis->ReadBytes(afilter, size_t(std::max<int>(pbmp->cfilter, 0)) * 2);
    pbis->ReadBytes(acond, size_t(std::max<int>(pbmp->ccond, 0)) * 4);
    if (!FFilterMatches3(pbmp->cfilter, afilter) || !FConditionsMatch3(pbmp->ccond, acond))
    {
        if (pbmp->texk != TEXK_Bulk) SkipBytes(pbis, pbmp->cb);
        pbmp->texk = TEXK_Filtered;
        ++g_shdstats.cfiltered;
        return;
    }
    if (pbmp->texk == TEXK_Bulk) return;
    auto it = g_shds.mpidbmp.find(pbmp->id);
    if (it != g_shds.mpidbmp.end() && it->second->pab)
    {
        pbmp->pab = it->second->pab;
        SkipBytes(pbis, pbmp->cb);
        ++g_shdstats.cshared;
        return;
    }
    auto ab2 = std::make_shared<std::vector<uint8_t>>(pbmp->cb);
    pbis->ReadBytes(ab2->data(), pbmp->cb);
    pbmp->pab = std::move(ab2);
    g_shds.mpidbmp[pbmp->id] = pbmp;
}

// orig3: 0x2347C8 LoadBmpsFromBrx (as the CLUTs)
void LoadBmpsFromBrx(CBinaryInputStream* pbis, int ipass)
{
    LOAD_SCOPE(0x2347C8, pbis);
    const int32_t cb = pbis->S32Read();
    if (ItexPass3() != ipass)
    {
        SkipBytes(pbis, size_t(cb));
        ++g_shdstats.cskippedPass;
        return;
    }
    const int c = pbis->U16Read();
    g_shds.apbmpCur.clear();
    for (int i = 0; i < c; ++i)
    {
        auto pbmp = std::make_unique<BMP>();
        LoadBmpFromBrx(pbmp.get(), pbis);
        g_shds.apbmpCur.push_back(pbmp.get());
        g_shds.abmp.push_back(std::move(pbmp));
    }
    g_shdstats.cbmp += c;
}

// orig3: 0x234900 (a shader's texture state: 8 bytes, its BMPs (+6) and CLUTs (+7) by U16 index in the group's
// tables; a CLUT index 0xFFFF: none). As Sly 2's 0x1EB1A8.
void LoadTexFromBrx(TEX* ptex, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x234900, pbis);
    uint8_t ab[8];
    pbis->ReadBytes(ab, sizeof(ab));
    std::memcpy(&ptex->oid, ab + 0, 4);
    std::memcpy(&ptex->grftex, ab + 4, 2);
    const int cbmp = ab[6], cclut = ab[7];
    for (int i = 0; i < cbmp; ++i)
    {
        const int k = pbis->U16Read();
        if (k >= static_cast<int>(g_shds.apbmpCur.size())) throw std::runtime_error("texture state: BMP index out of range");
        ptex->apbmp.push_back(g_shds.apbmpCur[size_t(k)]);
    }
    for (int i = 0; i < cclut; ++i)
    {
        const int k = pbis->U16Read();
        if (k == 0xFFFF) ptex->apclut.push_back(nullptr);
        else
        {
            if (k >= static_cast<int>(g_shds.apclutCur.size())) throw std::runtime_error("texture state: CLUT index out of range");
            ptex->apclut.push_back(g_shds.apclutCur[size_t(k)]);
        }
    }
}

// orig3: 0x233C90 (texture passes of a shader: the CLUT stride of a state's frames; kind 7's flag is 0x10 in Sly 3)
int CclutPass(const SHD* pshd)
{
    switch (pshd->shdk)
    {
    case 1: return 2;
    case 7: return (pshd->grfshd & 0x10) ? 2 : 3;
    default: return 1;
    }
}

uint64_t AlphaShd(const SHD* pshd) { return (uint64_t((pshd->grfshd & 1) ? 2 : 1) << 2) | 0x40u; }

// orig3: 0x235128 (the GS register lists of each shader; Sly 2 0x1EB960), decoded into SHDPASSes instead of GS
// packets. Sly 3's lists: kinds 0 / 4 / 5 / 6 one pass, TEST = ATE (unless flag 0x20) | (kind 0: 0x117EA ATST GEQUAL
// AREF 0x7E, else 0x117E0 ATST NEVER) | (kind 0: ZTST GREATER 0x60000, else ALWAYS 0x20000); kind 1: CLUT pass 1
// with TEST 0x717EB (0x717EA with flag 0x20), then CLUT pass 0 with 0x51001 / ALPHA 0x48; kind 7: unless flag 0x10 a
// Z pre-pass (CLUT pass 2, nearest, 0x5000D / 0x40), then CLUT pass 1 (0x53001) and CLUT pass 0 (0x53001 / 0x48).
// The FOGCOL of the fogged passes is the condition set's (0x36BC30). Kinds 2 / 3 (volumes) are not drawn yet.
void BuildShdPasses(SHD* pshd)
{
    pshd->apass.clear();
    const uint8_t fl = pshd->grfshd;
    SHDPASS pass;
    switch (pshd->shdk)
    {
    case 0: case 4: case 5: case 6:
        pass.itexPass = 0;
        pass.icolor = 0;
        pass.test = uint64_t((fl & 0x20) ? 0 : 1) | (pshd->shdk == 0 ? 0x117EAu : 0x117E0u) | (pshd->shdk == 0 ? 0x60000u : 0x20000u);
        pass.alpha = AlphaShd(pshd);
        pshd->apass.push_back(pass);
        break;
    case 1:
        pass.itexPass = 1; pass.icolor = 0;
        pass.test = (fl & 0x20) ? 0x717EA : 0x717EB;
        pass.alpha = AlphaShd(pshd);
        pshd->apass.push_back(pass);
        pass.itexPass = 0; pass.icolor = 1;
        pass.test = 0x51001; pass.alpha = 0x48;
        pass.fFogBlack = true;   // (0x3D FOGCOL 0)
        pshd->apass.push_back(pass);
        break;
    case 7:
        if (!(fl & 0x10))
        {
            pass.itexPass = 2; pass.icolor = 1; pass.fNearest = true;
            pass.test = 0x5000D; pass.alpha = 0x40; pass.fFogBlack = true;   // (FOGCOL 0)
            pshd->apass.push_back(pass);
            pass.fNearest = false;
        }
        pass.itexPass = 1; pass.icolor = 0; pass.test = 0x53001; pass.alpha = AlphaShd(pshd); pass.fFogBlack = false;   // (FOGCOL: the world's)
        pshd->apass.push_back(pass);
        pass.itexPass = 0; pass.icolor = 1; pass.test = 0x53001; pass.alpha = 0x48; pass.fFogBlack = true;   // (FOGCOL 0)
        pshd->apass.push_back(pass);
        break;
    case 2:
    case 3:
        // Kind 2 (two untextured passes, ZTST by flag 4: shadow-like screen effects) and kind 3 (four lists:
        // projected volumes) need the volume / mask passes, not done yet: no passes, nothing drawn.
        break;
    default:
        break;
    }
}

// orig3: 0x234A58 (a shader: 0x18 bytes, its colours adjusted, an S8 animation index (Sly 2: S16), its texture
// states, and the register lists its kind needs)
void LoadShdFromBrx(SHD* pshd, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x234A58, pbis);
    uint8_t ab[0x18];
    pbis->ReadBytes(ab, sizeof(ab));
    std::memcpy(&pshd->id, ab + 0, 4);
    pshd->shdk = ab[4];
    pshd->grfshd = ab[5];
    pshd->b6 = ab[6];
    pshd->b7 = ab[7];
    std::memcpy(pshd->argba, ab + 8, 12);
    std::memcpy(&pshd->unSelfIllum, ab + 0x14, 2);
    pshd->b16 = ab[0x16];
    const int ctex = ab[0x17];
    for (uint32_t& rgba : pshd->argba) AdjustRgba(&rgba);
    const int isaa = pbis->S8Read();
    if (isaa >= 0)
    {
        if (isaa >= static_cast<int>(g_shds.apsaaCur.size())) throw std::runtime_error("shader: SAA index out of range");
        pshd->psaa = g_shds.apsaaCur[size_t(isaa)];
    }
    pshd->atex.resize(size_t(ctex));
    for (TEX& tex : pshd->atex)
    {
        LoadTexFromBrx(&tex, pbis);
        tex.pshd = pshd;
    }
    pshd->cframe = pshd->atex.empty() ? 0 : static_cast<int>(pshd->atex[0].apbmp.size());
    switch (pshd->shdk)
    {
    case 0: case 4: case 5: case 6: pshd->cregs = 1; break;
    case 1: case 2: pshd->cregs = 2; break;
    case 7: pshd->cregs = (pshd->grfshd & 0x10) ? 2 : 3; break;
    case 3: pshd->cregs = 4; break;
    default: pshd->cregs = 0; break;
    }
}

// orig3: 0x233AE0 (relocates the group's bulk records into the bulk block; the CLUTs' colours are transformed
// (grf8 bit 0, 0x2338B8) and adjusted there), with Sly 2's sharing of 0x1ECC28 (records of an id already loaded use
// the first one's data).
void RelocateBulkRecords(const std::shared_ptr<const std::vector<uint8_t>>& pbulk)
{
    const std::vector<uint8_t>& ab = *pbulk;
    for (CLUT* pclut : g_shds.apclutCur)
    {
        if (pclut->texk != TEXK_Bulk) continue;
        auto it = g_shds.mpidclut.find(pclut->id);
        if (it != g_shds.mpidclut.end() && it->second->pargba) { pclut->pargba = it->second->pargba; continue; }
        const size_t cb = size_t(pclut->ccolor) * 4;
        if (size_t(pclut->ibBulk) + cb > ab.size()) throw std::runtime_error("CLUT outside the bulk block");
        auto argba = std::make_shared<std::vector<uint32_t>>(pclut->ccolor);
        std::memcpy(argba->data(), ab.data() + pclut->ibBulk, cb);
        if (pclut->fTransform)
        {
            for (uint32_t& rgba : *argba) TransformClutRgba3(&rgba);
            ++g_shdstats.ctransformed;
        }
        for (uint32_t& rgba : *argba) AdjustRgba(&rgba);
        pclut->pargba = std::move(argba);
        g_shds.mpidclut[pclut->id] = pclut;
    }
    for (BMP* pbmp : g_shds.apbmpCur)
    {
        if (pbmp->texk != TEXK_Bulk) continue;
        auto it = g_shds.mpidbmp.find(pbmp->id);
        if (it != g_shds.mpidbmp.end() && it->second->pab) { pbmp->pab = it->second->pab; continue; }
        if (size_t(pbmp->ibBulk) + pbmp->cb > ab.size()) throw std::runtime_error("BMP outside the bulk block");
        pbmp->pab = std::make_shared<std::vector<uint8_t>>(ab.begin() + pbmp->ibBulk, ab.begin() + pbmp->ibBulk + pbmp->cb);
        g_shds.mpidbmp[pbmp->id] = pbmp;
    }
}

// orig3: 0x25C9A0 (Sly 2 0x20F640: the bulk block: read into a temporary slot, relocated and uploaded; or skipped
// when its key is the one already loaded)
void LoadBulkFromBrx(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x25C9A0, pbis);
    pbis->Align(0x10);
    std::shared_ptr<const std::vector<uint8_t>> pbulk;
    if (g_shds.fLoadBulk)
    {
        auto ab = std::make_shared<std::vector<uint8_t>>(size_t(g_shds.cbBulk));
        pbis->ReadBytes(ab->data(), ab->size());
        pbulk = ab;
        g_shds.mpkeybulk[g_shds.keyBulk] = pbulk;
        ++g_shdstats.cbulk;
    }
    else
    {
        SkipBytes(pbis, size_t(g_shds.cbBulk));
        // 0x170828: the textures stay where the last load of this key put them.
        auto it = g_shds.mpkeybulk.find(g_shds.keyBulk);
        if (it != g_shds.mpkeybulk.end()) pbulk = it->second;
    }
    if (pbulk) RelocateBulkRecords(pbulk);
}
}  // namespace

// orig3: 0x25C930 (Sly 2 0x20F5D0, identical: the bulk block's size and key; it is loaded when the key changed)
void LoadBulkHeaderFromBrx(SW* psw, CBinaryInputStream* pbis, bool fLoad)
{
    LOAD_SCOPE(0x25C930, pbis);
    (void)psw;
    const int32_t cb = pbis->S32Read();
    const int32_t key = pbis->S32Read();
    g_shds.cbBulk = cb;
    // The world loader 0x13DA18 always asks for the load (fLoad 1); other callers skip a block whose key is the
    // one last loaded (its VRAM still holds it) unless the native port has not seen its bytes.
    g_shds.fLoadBulk = fLoad || key != g_shds.keyBulk || g_shds.mpkeybulk.find(key) == g_shds.mpkeybulk.end();
    g_shds.keyBulk = key;
}

// orig3: 0x236EB0 (a shader animation from the stream: its class (0xFFFF: none), then its 0x24-byte description
// (Sly 2: 0x20))
SAA* PsaaLoadFromBrx(bool fOwned, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x236EB0, pbis);
    const int cid = pbis->U16Read();
    if (cid == 0xFFFF) return nullptr;
    auto psaa = std::make_unique<SAA>();
    psaa->cid = cid;
    psaa->fOwned = fOwned;
    pbis->ReadBytes(psaa->ab, sizeof(psaa->ab));
    // orig3: 0x236FE0 (SAA base init from the description: +0 oid, +4 grf, +8 n20, +0xA frames, as Sly 2's)
    std::memcpy(&psaa->oid, psaa->ab + 0, 4);
    std::memcpy(&psaa->grf, psaa->ab + 4, 4);
    uint16_t u;
    std::memcpy(&u, psaa->ab + 8, 2); psaa->n20 = u;
    std::memcpy(&u, psaa->ab + 0xA, 2); psaa->cframe = u;
    InitSaa(psaa.get());
    SAA* p = psaa.get();
    g_shds.asaa.push_back(std::move(psaa));
    ++g_shdstats.csaa;
    return p;
}

// orig3: 0x234EA0 (the group's textures: the CLUTs and BMPs of pass 1, then of pass 0 (one copy is kept,
// 0x234500), U8 shader animations (Sly 2: U16), U16 shaders, the bulk block 0x25C9A0, the streamed-texture chains
// 0x2364F0 (not needed natively), the register lists 0x235128, and the texture-frame lists 0x234DA8 of the shaders
// with flag 8 (not needed natively))
void LoadShadersFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x234EA0, pbis);
    (void)psw;
    LoadClutsFromBrx(pbis, 1);
    LoadBmpsFromBrx(pbis, 1);
    LoadClutsFromBrx(pbis, 0);
    LoadBmpsFromBrx(pbis, 0);
    const int csaa = pbis->U8Read();
    g_shds.apsaaCur.clear();
    for (int i = 0; i < csaa; ++i) g_shds.apsaaCur.push_back(PsaaLoadFromBrx(false, pbis));
    const int cshd = pbis->U16Read();
    g_shds.apshdCur.clear();
    for (int i = 0; i < cshd; ++i)
    {
        auto pshd = std::make_unique<SHD>();
        LoadShdFromBrx(pshd.get(), pbis);
        g_shds.apshdCur.push_back(pshd.get());
        g_shds.ashd.push_back(std::move(pshd));
    }
    g_shdstats.cshd += cshd;
    LoadBulkFromBrx(pbis);
    for (SHD* pshd : g_shds.apshdCur) BuildShdPasses(pshd);
}

// orig3: 0x235F68 (shader i of the group)
SHD* PshdFromIndex(int ishd)
{
    if (ishd < 0 || ishd >= static_cast<int>(g_shds.apshdCur.size())) return nullptr;
    return g_shds.apshdCur[size_t(ishd)];
}

// orig3: 0x235E60 (the shader of an id in the group; Sly 2 0x1EC820)
SHD* PshdFindShader(int32_t oid)
{
    if (oid == -1) return nullptr;
    for (SHD* pshd : g_shds.apshdCur)
        if (pshd->id == oid) return pshd;
    return nullptr;
}

// orig3: 0x233CD0 (TEX0 of pass j of state i: the state's BMP and the CLUT at i * passes + j; Sly 2 0x1EA6C8)
TEXSEL TexselFromShd(const SHD* pshd, int iframe, int itexPass)
{
    TEXSEL sel;
    if (!pshd || pshd->atex.empty()) return sel;
    const TEX& tex = pshd->atex[0];
    if (tex.apbmp.empty()) return sel;
    iframe = std::clamp(iframe, 0, static_cast<int>(tex.apbmp.size()) - 1);
    sel.pbmp = tex.apbmp[size_t(iframe)];
    const size_t iclut = size_t(iframe * CclutPass(pshd) + itexPass);
    if (iclut < tex.apclut.size()) sel.pclut = tex.apclut[iclut];
    sel.fClampS = (tex.grftex & 1) != 0;
    sel.fClampT = (tex.grftex & 2) != 0;
    return sel;
}

// The decode follows Sly2-Reference runtime/native_texture_decode.cpp: transfer and sample format agree (the
// sample PSM comes from the CLUT, 0x1EC9D0: more than 16 colours PSMT8, else PSMT4), so the GS swizzle cancels
// and the bytes are linear rows. T4 low nibble first; T8 entries with CSM1 exchange index bits 3 and 4.
bool FDecodeTexture(const BMP* pbmp, const CLUT* pclut, std::vector<uint8_t>& rgba, std::string& error)
{
    if (!pbmp || !pbmp->pab) { error = "no pixels"; return false; }
    const uint32_t dx = pbmp->dx, dy = pbmp->dy;
    const uint64_t c = uint64_t(dx) * dy;
    const std::vector<uint8_t>& ab = *pbmp->pab;
    rgba.resize(size_t(c) * 4);
    if (pbmp->psm == 0x00)
    {
        if (ab.size() < c * 4) { error = "short CT32"; return false; }
        std::memcpy(rgba.data(), ab.data(), size_t(c) * 4);
        return true;
    }
    if (pbmp->psm != 0x13 && pbmp->psm != 0x14) { error = "unsupported PSM " + std::to_string(pbmp->psm); return false; }
    if (!pclut || !pclut->pargba) { error = "indexed without palette"; return false; }
    const uint8_t psmSample = pclut->ccolor > 16 ? 0x13 : 0x14;
    if (psmSample != pbmp->psm) { error = "sample PSM differs from transfer PSM"; return false; }
    const std::vector<uint32_t>& pal = *pclut->pargba;
    const bool fT8 = pbmp->psm == 0x13;
    if (ab.size() < (fT8 ? c : (c + 1) / 2)) { error = "short indexed pixels"; return false; }
    for (uint64_t i = 0; i < c; ++i)
    {
        uint32_t index = fT8 ? ab[size_t(i)] : (ab[size_t(i / 2)] >> ((i & 1) * 4)) & 15;
        if (fT8) index = (index & 0xE7) | ((index & 8) << 1) | ((index & 16) >> 1);
        const uint32_t color = index < pal.size() ? pal[index] : 0;
        std::memcpy(rgba.data() + i * 4, &color, 4);
    }
    return true;
}
