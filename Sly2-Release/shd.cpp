// Textures and shaders: Sly 2's module 0x1EA568-0x1ED7A8 (Sly 1 shd.c) as native loaders. See shd.h.

#include "shd.h"
#include "worldglobals.h"

#include "loadtrace.h"

#include <algorithm>
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

// orig: 0x001EC7A0 (forgets the group's shaders, BMPs, CLUTs and shader animations; the records live on)
void EndShaderGroup()
{
    g_shds.apclutCur.clear();
    g_shds.apbmpCur.clear();
    g_shds.apsaaCur.clear();
    g_shds.apshdCur.clear();
}

namespace
{
// orig: 0x001EA8A8 (a CLUT record: shared with a loaded one of the same id, or its colours read and adjusted;
// unwanted ones skipped)
void LoadClutFromBrx(CLUT* pclut, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1EA8A8, pbis);
    uint8_t ab[0x10];
    pbis->ReadBytes(ab, sizeof(ab));
    std::memcpy(&pclut->id, ab + 0x0, 4);
    pclut->texk = ab[0x4];
    pclut->cfilter = static_cast<int8_t>(ab[0x5]);
    std::memcpy(&pclut->ccolor, ab + 0x8, 2);
    std::memcpy(&pclut->cgsbit, ab + 0xA, 2);
    std::memcpy(&pclut->ibBulk, ab + 0xC, 4);
    const uint32_t cb = uint32_t(pclut->ccolor) * 4;
    if (pclut->cfilter > 0)
    {
        int16_t afilter[128];
        pbis->ReadBytes(afilter, size_t(pclut->cfilter) * 2);
        if (!FFilterMatches(pclut->cfilter, afilter))
        {
            if (pclut->texk != TEXK_Bulk) SkipBytes(pbis, cb);
            pclut->texk = TEXK_Filtered;
            ++g_shdstats.cfiltered;
            return;
        }
    }
    if (pclut->texk != TEXK_Bulk)
    {
        auto it = g_shds.mpidclut.find(pclut->id);
        if (it != g_shds.mpidclut.end() && it->second->pargba)
        {
            pclut->pargba = it->second->pargba;
            SkipBytes(pbis, cb);
            ++g_shdstats.cshared;
        }
        else
        {
            auto argba = std::make_shared<std::vector<uint32_t>>(pclut->ccolor);
            pbis->ReadBytes(argba->data(), cb);
            for (uint32_t& rgba : *argba) AdjustRgba(&rgba);
            pclut->pargba = std::move(argba);
        }
    }
}

// orig: 0x001EAE40 (the group's CLUT records)
void LoadClutsFromBrx(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1EAE40, pbis);
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

// orig: 0x001EAF50 (a BMP record: shared with a loaded one of the same id, or its pixels read; unwanted ones
// skipped)
void LoadBmpFromBrx(BMP* pbmp, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1EAF50, pbis);
    uint8_t ab[0x18];
    pbis->ReadBytes(ab, sizeof(ab));
    std::memcpy(&pbmp->id, ab + 0x0, 4);
    pbmp->texk = ab[0x4];
    pbmp->cfilter = static_cast<int8_t>(ab[0x5]);
    std::memcpy(&pbmp->dx, ab + 0x8, 2);
    std::memcpy(&pbmp->dy, ab + 0xA, 2);
    pbmp->psm = ab[0xC];
    pbmp->tbw = ab[0xD];
    std::memcpy(&pbmp->cgsbit, ab + 0xE, 2);
    std::memcpy(&pbmp->cb, ab + 0x10, 4);
    std::memcpy(&pbmp->ibBulk, ab + 0x14, 4);
    if (pbmp->cfilter > 0)
    {
        int16_t afilter[128];
        pbis->ReadBytes(afilter, size_t(pbmp->cfilter) * 2);
        if (!FFilterMatches(pbmp->cfilter, afilter))
        {
            if (pbmp->texk != TEXK_Bulk) SkipBytes(pbis, pbmp->cb);
            pbmp->texk = TEXK_Filtered;
            ++g_shdstats.cfiltered;
            return;
        }
    }
    if (pbmp->texk != TEXK_Bulk)
    {
        auto it = g_shds.mpidbmp.find(pbmp->id);
        if (it != g_shds.mpidbmp.end() && it->second->pab)
        {
            pbmp->pab = it->second->pab;
            SkipBytes(pbis, pbmp->cb);
            ++g_shdstats.cshared;
        }
        else
        {
            auto ab2 = std::make_shared<std::vector<uint8_t>>(pbmp->cb);
            pbis->ReadBytes(ab2->data(), pbmp->cb);
            pbmp->pab = std::move(ab2);
        }
    }
}

// orig: 0x001EB098 (the group's BMP records)
void LoadBmpsFromBrx(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1EB098, pbis);
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

// orig: 0x001EB1A8 (a shader's texture state: its BMPs and CLUTs by index in the group's tables; 0xFFFF: none)
void LoadTexFromBrx(TEX* ptex, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1EB1A8, pbis);
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

// orig: 0x001EA688 (texture passes of a shader: the CLUT stride of a state's frames)
int CclutPass(const SHD* pshd)
{
    switch (pshd->shdk)
    {
    case 1: return 2;
    case 7: return (pshd->grfshd & 0x20) ? 2 : 3;
    default: return 1;
    }
}

// TEST values of 0x1EB960 that depend on the shader's kind and flags.
uint64_t TestShd(const SHD* pshd, uint64_t a1)
{
    const uint64_t a2 = a1 | ((pshd->grfshd & 4) ? 0x13000u : 0x11000u);
    return a2 | (pshd->shdk == 0 ? 0x60000u : 0x20000u);
}
uint64_t AlphaShd(const SHD* pshd) { return (uint64_t((pshd->grfshd & 1) ? 2 : 1) << 2) | 0x40u; }

// orig: 0x001EB960 (the GS register lists of each shader), decoded into SHDPASSes instead of GS packets.
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
        if (fl & 0x20) { pass.test = TestShd(pshd, 1); pass.alpha = 0; }
        else if (fl & 0x10) { pass.test = TestShd(pshd, pshd->shdk == 0 ? 0x7EB : 0x7E1); pass.alpha = (uint64_t(fl & 1) << 3) | 1; }
        else if (fl & 0x80) { pass.test = 0x77000; pass.alpha = 0x54; }
        else { pass.test = TestShd(pshd, pshd->shdk == 0 ? 0x7EB : 0x7E1); pass.alpha = AlphaShd(pshd); }
        pshd->apass.push_back(pass);
        break;
    case 1:
        pass.itexPass = 1; pass.icolor = 0;
        if (fl & 0x80) { pass.test = 0x77000; pass.alpha = 0x54; }
        else { pass.test = (fl & 4) ? 0x737EB : 0x717EB; pass.alpha = AlphaShd(pshd); }
        pshd->apass.push_back(pass);
        pass.itexPass = 0; pass.icolor = 1;
        if (fl & 0x80) { pass.test = 0x57000; pass.alpha = 0x58; }
        else { pass.test = 0x51001; pass.alpha = 0x48; }
        pass.fFogBlack = true;   // (0x3D FOGCOL 0)
        pshd->apass.push_back(pass);
        break;
    case 7:
        if (!(fl & 0x20))
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
        // Kind 2 (two untextured passes subtracting and adding back, DATE-masked: shadow-like screen effects) and
        // kind 3 (four lists: projected volumes, drawn with P_G 0x3628 / 0x37A8 and destination-alpha passes)
        // need the volume / mask passes, not done yet: no passes, nothing drawn.
        break;
    default:
        break;
    }
}

// orig: 0x001EB2F0 (a shader: its colours, animation, states, and the register lists its kind needs)
void LoadShdFromBrx(SHD* pshd, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1EB2F0, pbis);
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
    const int isaa = pbis->S16Read();
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
    case 7: pshd->cregs = (pshd->grfshd & 0x20) ? 2 : 3; break;
    case 3: pshd->cregs = 4; break;
    default: pshd->cregs = 0; break;
    }
}

// orig: 0x001EA568 (relocates the group's bulk records into the bulk block, re-adjusting the CLUTs' colours),
// with the sharing of 0x1ECC28 (records of an id already loaded use the first one's data).
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

// orig: 0x0020F640 (the bulk block: read into a temporary slot, relocated and uploaded; or skipped when its key
// is the one already loaded)
void LoadBulkFromBrx(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x20F640, pbis);
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

// orig: 0x0020F5D0 (the bulk block's size and key; it is loaded when the key changed)
void LoadBulkHeaderFromBrx(SW* psw, CBinaryInputStream* pbis, bool fLoad)
{
    LOAD_SCOPE(0x20F5D0, pbis);
    (void)psw;
    const int32_t cb = pbis->S32Read();
    const int32_t key = pbis->S32Read();
    g_shds.cbBulk = cb;
    // The world loader 0x13DA18 always asks for the load (fLoad 1); other callers skip a block whose key is the
    // one last loaded (its VRAM still holds it) unless the native port has not seen its bytes.
    g_shds.fLoadBulk = fLoad || key != g_shds.keyBulk || g_shds.mpkeybulk.find(key) == g_shds.mpkeybulk.end();
    g_shds.keyBulk = key;
}

// orig: 0x001ED7A8 (a shader animation from the stream: its class (0xFFFF: none), then its 0x20-byte description)
SAA* PsaaLoadFromBrx(bool fOwned, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1ED7A8, pbis);
    const int cid = pbis->U16Read();
    if (cid == 0xFFFF) return nullptr;
    auto psaa = std::make_unique<SAA>();
    psaa->cid = cid;
    psaa->fOwned = fOwned;
    pbis->ReadBytes(psaa->ab, sizeof(psaa->ab));
    // orig: 0x001ED8E0 (SAA base init from the description)
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

// orig: 0x001EB748 (the group's textures: CLUTs, BMPs, shader animations, shaders, then the bulk block 0x20F640,
// the streamed-texture chain 0x1ECE58 and the register lists 0x1EB960)
void LoadShadersFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1EB748, pbis);
    (void)psw;
    LoadClutsFromBrx(pbis);
    LoadBmpsFromBrx(pbis);
    const int csaa = pbis->U16Read();
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

// orig: 0x001EC8F8 (shader i of the group)
SHD* PshdFromIndex(int ishd)
{
    if (ishd < 0 || ishd >= static_cast<int>(g_shds.apshdCur.size())) return nullptr;
    return g_shds.apshdCur[size_t(ishd)];
}

// orig: 0x001EC820 (the shader of an id in the group)
SHD* PshdFindShader(int32_t oid)
{
    if (oid == -1) return nullptr;
    for (SHD* pshd : g_shds.apshdCur)
        if (pshd->id == oid) return pshd;
    return nullptr;
}

// orig: 0x001EA6C8 (TEX0 of pass j of state i: the state's BMP and the CLUT at i * passes + j)
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
