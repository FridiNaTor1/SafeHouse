// The PS3 collection's HD assets: textures from The Sly Collection (PS3) put over the PS2 game as a
// texture pack (Textures.pak, or the PNG files in Textures: platform/texture_pak.h; <W>x<H>_<hash>.png, named after the
// PS2 texture each one replaces).
//
// The game itself never reads PS3 data: the worlds, scripts and logic stay the PS2 disc's, so the game plays as on the
// PS2 by construction. This tool only reads the PS3 world files to take their textures.
//
// The PS3 worlds (SLY2.WAL, PS3_GAME/USRDIR/SLY2) are the PS2 worlds rebuilt by the PS3 port: the same LZ
// compression and little-endian layout, the European block order, wide text, file keys replaced by names, meshes in
// a new format ("SZMS"), and the textures redone at up to 4x the size with 256-colour palettes. The texture blocks
// (0x1EB748: CLUTs, BMPs, shader animations, shaders, the bulk block) keep the PS2 format, so they are found by a
// strict scan and read with the PS2 loader's own code; a PS3 texture is paired with the PS2 one by its shader's id,
// frame and pass, and checked against it (the PS3 texels scaled down to the PS2 size must match).
#include "region.h"
#include "shd.h"
#include "bis.h"
#include "cls.h"
#include "disc.h"
#include "worldparts.h"
#include "../platform/platform.h"
#include "../platform/ps3disc.h"
#include "../platform/texture_pak.h"

#include <nlohmann/json.hpp>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr size_t SECTOR = 2048;

// The Sly LZ (platform/brx_decompress.h), tolerant of the padding after the stream (the PS3 archive has no sizes:
// a world's extent runs to the next file).
std::vector<uint8_t> DecompressTolerant(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> out;
    out.reserve(in.size() * 2);
    uint8_t work[0x2000] = {};
    size_t op = 0, ip = 0;
    uint32_t flags = 0;
    while (ip + 2 < in.size())
    {
        flags >>= 1;
        if (!(flags & 0x100)) flags = 0xFF00u | in[ip++];
        const uint8_t b = in[ip++];
        if (flags & 1)
        {
            work[op++ & 0x1FFF] = b;
            out.push_back(b);
        }
        else
        {
            const uint16_t t = uint16_t(b | (in[ip++] << 8));
            const size_t s = t & 0x1FFF, l = (t >> 13) + 3u;
            for (size_t i = 0; i < l; ++i)
            {
                const uint8_t x = work[(s + i) & 0x1FFF];
                work[op++ & 0x1FFF] = x;
                out.push_back(x);
            }
        }
    }
    return out;
}

uint16_t U16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// A bounds-checked cursor for the scan.
struct CUR
{
    const uint8_t* pb;
    size_t cb, ib;
    bool FHas(size_t n) const { return ib <= cb && n <= cb - ib; }
};

int BppOfPsm(uint8_t psm)
{
    switch (psm)
    {
    case 0x00: return 32;
    case 0x13: return 8;
    case 0x14: return 4;
    default: return 0;
    }
}

// A texture block (0x1EB748) at ib, checked strictly without loading anything: its CLUTs, BMPs, shader animations
// and shaders must all be well formed and reference each other in range; the bulk block follows (16-aligned) and
// holds every bulk record. cbBulk: the bytes the records need (the block's own size is not in it).
struct BLOCK
{
    size_t ib = 0, ibBulk = 0, cbBulk = 0;
    int cclut = 0, cbmp = 0, cshd = 0;
};

bool FProbeBlock(const uint8_t* pb, size_t cb, size_t ib, BLOCK& blk)
{
    CUR c{pb, cb, ib};
    uint64_t ibMax = 0;
    if (!c.FHas(2)) return false;
    const int cclut = U16(pb + c.ib); c.ib += 2;
    if (cclut > 4096) return false;
    for (int i = 0; i < cclut; ++i)
    {
        if (!c.FHas(16)) return false;
        const uint8_t* r = pb + c.ib;
        const uint8_t texk = r[4];
        const int cfilter = int8_t(r[5]);
        const uint16_t ccolor = U16(r + 8);
        if ((texk != TEXK_Bulk && texk != TEXK_Streamed) || cfilter > 64 || r[6] || r[7]) return false;
        if (ccolor == 0 || ccolor > 256) return false;
        c.ib += 16;
        if (cfilter > 0)
        {
            if (!c.FHas(size_t(cfilter) * 2)) return false;
            c.ib += size_t(cfilter) * 2;
        }
        if (texk == TEXK_Bulk) ibMax = std::max<uint64_t>(ibMax, uint64_t(U32(r + 12)) + ccolor * 4u);
        else
        {
            if (!c.FHas(size_t(ccolor) * 4)) return false;
            c.ib += size_t(ccolor) * 4;
        }
    }
    if (!c.FHas(2)) return false;
    const int cbmp = U16(pb + c.ib); c.ib += 2;
    if (cbmp > 4096) return false;
    for (int i = 0; i < cbmp; ++i)
    {
        if (!c.FHas(0x18)) return false;
        const uint8_t* r = pb + c.ib;
        const uint8_t texk = r[4];
        const int cfilter = int8_t(r[5]);
        const uint16_t dx = U16(r + 8), dy = U16(r + 10);
        const int bpp = BppOfPsm(r[12]);
        const uint32_t cbPix = U32(r + 16);
        if ((texk != TEXK_Bulk && texk != TEXK_Streamed) || cfilter > 64 || r[6] || r[7]) return false;
        if (!bpp || dx == 0 || dy == 0 || dx > 2048 || dy > 2048) return false;
        const uint64_t cbMin = (uint64_t(dx) * dy * bpp + 7) / 8;
        if (cbPix < cbMin || cbPix > std::max<uint64_t>(cbMin * 4, cbMin + 0x4000)) return false;
        c.ib += 0x18;
        if (cfilter > 0)
        {
            if (!c.FHas(size_t(cfilter) * 2)) return false;
            c.ib += size_t(cfilter) * 2;
        }
        if (texk == TEXK_Bulk) ibMax = std::max<uint64_t>(ibMax, uint64_t(U32(r + 20)) + cbPix);
        else
        {
            if (!c.FHas(cbPix)) return false;
            c.ib += cbPix;
        }
    }
    if (!c.FHas(2)) return false;
    const int csaa = U16(pb + c.ib); c.ib += 2;
    if (csaa > 512) return false;
    for (int i = 0; i < csaa; ++i)
    {
        if (!c.FHas(2)) return false;
        const uint16_t cid = U16(pb + c.ib); c.ib += 2;
        if (cid == 0xFFFF) continue;
        if (cid >= 0x400 || !c.FHas(0x20)) return false;
        c.ib += 0x20;
    }
    if (!c.FHas(2)) return false;
    const int cshd = U16(pb + c.ib); c.ib += 2;
    if (cshd > 8192) return false;
    for (int i = 0; i < cshd; ++i)
    {
        if (!c.FHas(0x18 + 2)) return false;
        const uint8_t* r = pb + c.ib;
        if (r[4] > 7 || r[0x17] > 4) return false;
        const int ctex = r[0x17];
        c.ib += 0x18;
        const int isaa = int16_t(U16(pb + c.ib)); c.ib += 2;
        if (isaa < -1 || isaa >= csaa) return false;
        for (int t = 0; t < ctex; ++t)
        {
            if (!c.FHas(8)) return false;
            const int cb2 = pb[c.ib + 6], cc2 = pb[c.ib + 7];
            c.ib += 8;
            if (!c.FHas(size_t(cb2 + cc2) * 2)) return false;
            for (int k = 0; k < cb2; ++k, c.ib += 2)
                if (U16(pb + c.ib) >= cbmp) return false;
            for (int k = 0; k < cc2; ++k, c.ib += 2)
                if (const uint16_t v = U16(pb + c.ib); v != 0xFFFF && v >= cclut) return false;
        }
    }
    if (cbmp == 0 || cshd == 0) return false;   // (no textures to take)
    const size_t ibBulk = (c.ib + 15) & ~size_t(15);
    if (ibBulk > cb || ibMax > cb - ibBulk) return false;
    blk.ib = ib;
    blk.ibBulk = ibBulk;
    blk.cbBulk = size_t(ibMax);
    blk.cclut = cclut;
    blk.cbmp = cbmp;
    blk.cshd = cshd;
    return true;
}

// Every texture block of a world file, loaded with the PS2 loader's code into g_shds (ResetShaders first).
std::vector<BLOCK> LoadTextureBlocks(const std::vector<uint8_t>& ab)
{
    ResetShaders();
    std::vector<BLOCK> ablk;
    CBinaryInputStream bis;
    bis.OpenMemory(static_cast<int>(ab.size()), ab.data());
    size_t ib = 0;
    while (ib + 16 < ab.size())
    {
        BLOCK blk;
        // (cheap pre-checks: a CLUT count, then a CLUT record's texk / padding, or no CLUTs and a BMP count)
        const uint8_t* p = ab.data() + ib;
        const bool fCand = U16(p) <= 4096 && (U16(p) == 0 || ((p[6] == 1 || p[6] == 3) && !p[8] && !p[9] && (p[10] | p[11])));
        if (!fCand || !FProbeBlock(ab.data(), ab.size(), ib, blk)) { ++ib; continue; }
        try
        {
            bis.Skip(ib - bis.GetOffset());
            g_shds.cbBulk = static_cast<int32_t>(blk.cbBulk);
            g_shds.keyBulk = static_cast<int32_t>(ablk.size());
            g_shds.fLoadBulk = true;
            LoadShadersFromBrx(nullptr, &bis);
            EndShaderGroup();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "ps3hd: texture block at 0x%zX did not load: %s\n", ib, e.what());
            EndShaderGroup();
            ResetShaders();  // (the tables may be half filled: start over past this block)
            ablk.clear();
            bis.OpenMemory(static_cast<int>(ab.size()), ab.data());
            ib += 1;
            continue;
        }
        ablk.push_back(blk);
        if (std::getenv("SLY2_PS3HD_DEBUG"))
        {
            std::printf("block 0x%zX clut %d bmp %d shd %d bulk %zu:", ib, blk.cclut, blk.cbmp, blk.cshd, blk.cbBulk);
            for (size_t i = g_shds.ashd.size() - size_t(blk.cshd); i < g_shds.ashd.size(); ++i)
                std::printf(" %d/%d/%zu", g_shds.ashd[i]->id, g_shds.ashd[i]->shdk, g_shds.ashd[i]->atex.size());
            std::printf("\n");
        }
        ib = bis.GetOffset();
    }
    return ablk;
}

// A world's textures, each with its place (block, shader in the block, frame, pass) and its BMP / CLUT ids: the
// decoded RGBA (raw PS2 alpha, rows as the game keeps them).
struct TEXIMG
{
    int iblock = 0, ishd = 0, iframe = 0, ipass = 0;
    int dx = 0, dy = 0;
    int32_t idBmp = 0, idClut = 0;
    std::vector<uint8_t> rgba;
};

std::vector<TEXIMG> DecodeWorldTextures(const std::vector<BLOCK>& ablk)
{
    std::vector<TEXIMG> aimg;
    std::string err;
    size_t ishdAll = 0;
    for (size_t iblock = 0; iblock < ablk.size(); ++iblock)
        for (int ishd = 0; ishd < ablk[iblock].cshd; ++ishd, ++ishdAll)
        {
            if (ishdAll >= g_shds.ashd.size()) return aimg;
            const SHD* pshd = g_shds.ashd[ishdAll].get();
            if (!pshd || pshd->atex.empty()) continue;
            std::set<std::pair<const BMP*, const CLUT*>> setDone;
            const int cframe = std::max<int>(1, int(pshd->atex[0].apbmp.size()));
            for (int iframe = 0; iframe < cframe; ++iframe)
                for (int ipass = 0; ipass < 3; ++ipass)
                {
                    const TEXSEL sel = TexselFromShd(pshd, iframe, ipass);
                    if (!sel.pbmp || !setDone.insert({sel.pbmp, sel.pclut}).second) continue;
                    TEXIMG img;
                    if (!FDecodeTexture(sel.pbmp, sel.pclut, img.rgba, err)) continue;
                    img.iblock = int(iblock);
                    img.ishd = ishd;
                    img.iframe = iframe;
                    img.ipass = ipass;
                    img.dx = sel.pbmp->dx;
                    img.dy = sel.pbmp->dy;
                    img.idBmp = sel.pbmp->id;
                    img.idClut = sel.pclut ? sel.pclut->id : -1;
                    aimg.push_back(std::move(img));
                }
        }
    return aimg;
}

// The PS3 texels scaled to the PS2 size (box filter when the PS3 size is a multiple, else nearest), compared with the
// PS2 texels: the mean absolute difference per channel (alpha included, 0..255), the correlation of the two (all
// channels; 1 when the PS2 texture is flat) and the PS2 texture's spread (its standard deviation).
struct MATCH
{
    double mad = 1e9, r = 0, sd2 = 0;
    double Score() const { return mad + 20.0 * (1.0 - r); }
};

MATCH MatchAgainst(const TEXIMG& a2, const TEXIMG& a3)
{
    const int dx = a2.dx, dy = a2.dy;
    const bool fBox = a3.dx % dx == 0 && a3.dy % dy == 0;
    const int sx = std::max(1, a3.dx / dx), sy = std::max(1, a3.dy / dy);
    // the correlation is taken on cells of c x c PS2 texels (about 16 on the short side): the PS2's dithering and the
    // PS3's smooth redraw of the same art then agree, two different patterns still do not
    const int c = std::max(1, std::min(dx, dy) / 16);
    const int cx = (dx + c - 1) / c, cy = (dy + c - 1) / c;
    std::vector<double> al2(size_t(cx) * cy), al3(size_t(cx) * cy);
    double sum = 0;
    for (int y = 0; y < dy; ++y)
        for (int x = 0; x < dx; ++x)
        {
            double v[4] = {};
            for (int k = 0; k < 4; ++k)
            {
                if (fBox)
                {
                    for (int j = 0; j < sy; ++j)
                        for (int i = 0; i < sx; ++i)
                            v[k] += a3.rgba[(size_t(y * sy + j) * a3.dx + size_t(x * sx + i)) * 4 + k];
                    v[k] /= double(sx * sy);
                }
                else
                {
                    const int X = std::min(a3.dx - 1, x * a3.dx / dx), Y = std::min(a3.dy - 1, y * a3.dy / dy);
                    v[k] = a3.rgba[(size_t(Y) * a3.dx + size_t(X)) * 4 + k];
                }
            }
            // premultiplied by alpha (a transparent texel's colour is not seen), alpha as it is
            const uint8_t* pu = &a2.rgba[(size_t(y) * dx + x) * 4];
            const double a2a = std::min(1.0, pu[3] / 128.0), a3a = std::min(1.0, v[3] / 128.0);
            double l2 = 0, l3 = 0;
            for (int k = 0; k < 4; ++k)
            {
                const double u = k < 3 ? pu[k] * a2a : pu[k], w = k < 3 ? v[k] * a3a : v[k];
                sum += std::fabs(w - u);
                static constexpr double s_aw[4] = {0.299, 0.587, 0.114, 0};
                l2 += s_aw[k] * u;
                l3 += s_aw[k] * w;
            }
            al2[size_t(y / c) * cx + size_t(x / c)] += l2;
            al3[size_t(y / c) * cx + size_t(x / c)] += l3;
        }
    MATCH m;
    m.mad = sum / (double(dx) * dy * 4);
    double s2 = 0, s3 = 0, s22 = 0, s33 = 0, s23 = 0;
    const double n = double(al2.size());
    for (size_t i = 0; i < al2.size(); ++i)
    {
        const double u = al2[i] / (c * c), w = al3[i] / (c * c);
        s2 += u; s3 += w; s22 += u * u; s33 += w * w; s23 += u * w;
    }
    const double var2 = s22 / n - (s2 / n) * (s2 / n), var3 = s33 / n - (s3 / n) * (s3 / n);
    m.sd2 = std::sqrt(std::max(0.0, var2));
    m.r = var2 < 1e-9 ? 1.0 : (var3 < 1e-9 ? 0.0 : (s23 / n - (s2 / n) * (s3 / n)) / std::sqrt(var2 * var3));
    return m;
}

// A pair is taken when the PS3 texture, scaled down, is the PS2 one: close (MAD) and of the same pattern
// (correlation), or both flat. (Checked by eye on Paris's pairs.)
bool FAccept(const MATCH& m) { return m.mad <= 12.0 && (m.r >= 0.85 || (m.sd2 < 4.0 && m.mad < 3.0)); }

uint64_t HashRgba(const std::vector<uint8_t>& ab)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : ab) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

// The mods folder's name of a PS2 texture (texmod.cpp SzTextureModName).
std::string SzName(const TEXIMG& img)
{
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%dx%d_%016llx.png", img.dx, img.dy, static_cast<unsigned long long>(HashRgba(img.rgba)));
    return sz;
}

// As --dump-textures writes a texture: rows top-down, alpha doubled (the PS2's 0x80 = 255).
std::vector<uint8_t> PngRows(const TEXIMG& img)
{
    std::vector<uint8_t> ab(img.rgba.size());
    for (int y = 0; y < img.dy; ++y)
        for (int x = 0; x < img.dx; ++x)
            for (int k = 0; k < 4; ++k)
            {
                const uint8_t v = img.rgba[(size_t(y) * img.dx + x) * 4 + k];
                ab[(size_t(img.dy - 1 - y) * img.dx + x) * 4 + k] = k < 3 ? v : uint8_t(std::min(255, v * 2));
            }
    return ab;
}
bool FWritePng(const std::string& path, const TEXIMG& img)
{
    const std::vector<uint8_t> ab = PngRows(img);
    return stbi_write_png(path.c_str(), img.dx, img.dy, 4, ab.data(), img.dx * 4) != 0;
}
// The same PNG's bytes (for Textures.pak)
bool FEncodePng(const TEXIMG& img, std::vector<uint8_t>& png)
{
    const std::vector<uint8_t> ab = PngRows(img);
    png.clear();
    auto append = [](void* pv, void* pb, int cb) {
        auto& out = *static_cast<std::vector<uint8_t>*>(pv);
        out.insert(out.end(), static_cast<const uint8_t*>(pb), static_cast<const uint8_t*>(pb) + cb);
    };
    return stbi_write_png_to_func(append, &png, img.dx, img.dy, 4, ab.data(), img.dx * 4) != 0 && !png.empty();
}

// The kind of file starting at a sector of SLY2.WAL: worlds (LZ, "FK$X" debug names at +3), dialog (VAGp), music and
// ambience (XVAG) and sound banks (989snd, big-endian), else 0. There is no readable table (the archive's first 256 KB
// are not plain data), so a file runs to the next one.
char KindOfSector(const uint8_t* p)
{
    static const uint8_t s_abBank[12] = {0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0x20};
    if (!std::memcmp(p, "VAGp", 4)) return 'v';
    if (!std::memcmp(p, "XVAG", 4)) return 'x';
    if (!std::memcmp(p, s_abBank, 12)) return 'b';
    if (!std::memcmp(p + 3, "FK$X", 4)) return 'w';
    return 0;
}

void Progress(const char* stage, uint64_t done, uint64_t total, const std::string& item)
{
    std::printf("ps3hd-progress %s %llu %llu %s\n", stage, static_cast<unsigned long long>(done), static_cast<unsigned long long>(total), item.c_str());
    std::fflush(stdout);
}

// Whether an installation's world entry is format 3's index of parts (worldparts.h)
bool FWorldPartsIndex(const std::string& rel)
{
    static const std::string s_ext = ".world.json";
    return rel.size() > s_ext.size() && rel.compare(rel.size() - s_ext.size(), s_ext.size(), s_ext) == 0;
}

// An installed PS2 world's file (decompressed), by its manifest path (relative to the installation). Format 3 keeps a
// world as parts (<World>.world.json and its World folder, worldparts.h): put back together as the game's loader does,
// from the installed parts (never a mod's: the pack is named after the installed textures). fHeader: only the start
// (the group table, SzWorldHeader) is needed: of a whole file its first bytes; the parts are always put together.
bool FReadPs2World(const std::string& rel, bool fHeader, std::vector<uint8_t>& ab, std::string& error)
{
    const fs::path root = cane::path(g_disc.Path());
    const fs::path p = root / cane::path(rel);
    if (FWorldPartsIndex(rel))
    {
        nlohmann::json index;
        try
        {
            std::ifstream in(p);
            if (!in) { error = "cannot be read"; return false; }
            in >> index;
        }
        catch (const std::exception&) { error = "is not a world index"; return false; }
        const size_t islash = rel.rfind('/');
        const std::string relDir = islash == std::string::npos ? std::string() : rel.substr(0, islash);
        try
        {
            return FRavelWorld(index, relDir, [&root](const std::string& relPart) { return cane::utf8(root / cane::path(relPart)); }, ab, error);
        }
        catch (const std::exception& e) { error = e.what(); return false; }
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) { error = "cannot be read"; return false; }
    if (fHeader)
    {
        ab.resize(64 * 1024 * 4);
        in.read(reinterpret_cast<char*>(ab.data()), std::streamsize(ab.size()));
        ab.resize(size_t(in.gcount()));
    }
    else ab.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// A world file's group names (the header: S16 count, then 0x54-byte records whose first 64 bytes are the name):
// the PS3 port kept them, so they identify the PS2 world.
std::string SzWorldHeader(const std::vector<uint8_t>& ab)
{
    if (ab.size() < 2) return "";
    const size_t cwe = U16(ab.data());
    if (ab.size() < 2 + cwe * 0x54) return "";
    std::string s;
    for (size_t i = 0; i < cwe; ++i) s.append(reinterpret_cast<const char*>(ab.data() + 2 + i * 0x54), 64);
    return s;
}
}  // namespace

// (lang, ps3lang.cpp) the PS3 archive's world decompression and file kinds, shared
std::vector<uint8_t> Ps3DecompressWorld(const std::vector<uint8_t>& in) { return DecompressTolerant(in); }
char Ps3KindOfSector(const uint8_t* p) { return KindOfSector(p); }

// --ps3-hd-textures SOURCE OUT (with --disc / --data-dir on the PS2 installation): every PS3 world's textures over its
// PS2 world's, written as OUT/Textures.pak (each under the PS2 texture's mods name; png: OUT/Textures/<that name>, the
// PNG files), and OUT/ps3hd-textures.tsv (the pairs and checks). SOURCE: the extracted disc (or USRDIR, or SLY2.WAL's
// folder), the disc image, or a .zip of it (platform/ps3disc.h): SLY2.WAL is read once, in order, and each world is
// taken as it is read. Errors are printed as "ps3hd: error: <a sentence>" (the launcher shows it), progress as
// "ps3hd-progress <stage> <done> <total> <item>".
int Ps3HdTexturesCommand(const std::string& source, const std::string& out, bool png)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto fail = [](const std::string& message) {
        std::printf("ps3hd: error: %s\n", message.c_str());
        std::fflush(stdout);
        return 2;
    };
    InitClasses();
    g_nFilterCur = -1;  // (every texture, whatever the world's filter)
    cane::ps3::Disc disc;
    std::string err;
    // (a SLY2.WAL file itself is read as a folder holding it)
    fs::path src = cane::path(source);
    std::error_code ec0;
    if (fs::is_regular_file(src, ec0) && cane::utf8(src.filename()) == "SLY2.WAL") src = src.parent_path();
    if (!disc.open(src, err) || !disc.checkSlyCollection(2, err)) return fail(err);
    std::printf("ps3hd: The Sly Collection (%s): %s\n", disc.kindName(), source.c_str());

    // the PS2 worlds of the installation (the manifest), by header
    nlohmann::json m;
    {
        std::ifstream in(cane::path(g_disc.Path()) / "extraction_manifest.json");
        if (!in) return fail("Install Sly 2 with this SafeHouse first: no extraction_manifest.json in its installation.");
        try { in >> m; } catch (...) { return fail("The Sly 2 installation's extraction_manifest.json cannot be read: repair the installation."); }
    }
    // (path: the manifest's, relative to the installation: the world file, or format 3's <World>.world.json)
    struct PS2WORLD { std::string path, name; };
    std::map<std::string, PS2WORLD> mpheader;
    int cworldPs2 = 0, cworldUnread = 0;
    for (const auto& f : m.at("files"))
        if (f.value("kind", "") == "world")
        {
            const std::string rel = f.at("path").get<std::string>();
            ++cworldPs2;
            std::vector<uint8_t> ab;
            std::string errWorld;
            if (!FReadPs2World(rel, true, ab, errWorld))
            {
                ++cworldUnread;
                std::printf("ps3hd: %s: %s\n", rel.c_str(), errWorld.c_str());
                continue;
            }
            std::string name = cane::utf8(cane::path(rel).stem());
            if (FWorldPartsIndex(rel)) name = cane::utf8(cane::path(name).stem());   // ("Paris.world.json")
            mpheader[SzWorldHeader(ab)] = {rel, name};
        }
    if (cworldPs2 && cworldUnread == cworldPs2) return fail("The Sly 2 installation's worlds cannot be read: repair the installation.");

    fs::create_directories(cane::path(out));
    cane::TexturePackWriter pack;
    if (!pack.Open(cane::path(out), png)) return fail("Cannot write the texture pack in " + out + ".");
    FILE* pfTsv = std::fopen(cane::utf8(cane::path(out) / "ps3hd-textures.tsv").c_str(), "w");
    if (pfTsv) std::fprintf(pfTsv, "world\tps2_name\tps2_size\tps3_size\tbmp\tclut\tmad\tr\tresult\tpaired_by\n");

    std::set<std::string> setDoneWorld, setWritten;
    int cworld = 0, cwritten = 0, csame = 0, cmismatch = 0, cmissing = 0;
    uint64_t cbWritten = 0;
    auto processWorld = [&](const std::vector<uint8_t>& comp, uint64_t ib) {
        const std::vector<uint8_t> ab3 = DecompressTolerant(comp);
        const std::string hdr = SzWorldHeader(ab3);
        auto it = mpheader.find(hdr);
        if (it == mpheader.end()) { std::printf("ps3hd: PS3 world at 0x%llX matches no PS2 world\n", (unsigned long long)ib); return; }
        if (!setDoneWorld.insert(it->second.path).second) return;  // (the archive's second copies)
        if (const char* sz = std::getenv("SLY2_PS3HD_ONLY"); sz && it->second.name != sz) return;
        ++cworld;
        Progress("textures", setDoneWorld.size() - 1, mpheader.size(), it->second.name);

        std::vector<uint8_t> ab2;
        if (std::string errWorld; !FReadPs2World(it->second.path, false, ab2, errWorld))
        {
            std::printf("ps3hd: %s: %s\n", it->second.path.c_str(), errWorld.c_str());
            return;
        }
        g_fPs3World = false;
        const std::vector<BLOCK> ablk2 = LoadTextureBlocks(ab2);
        const std::vector<TEXIMG> aimg2 = DecodeWorldTextures(ablk2);
        g_fPs3World = true;
        const std::vector<BLOCK> ablk3 = LoadTextureBlocks(ab3);
        const std::vector<TEXIMG> aimg3 = DecodeWorldTextures(ablk3);
        g_fPs3World = false;
        // The PS3 build renumbered the shaders and redid the palettes, but kept the BMP ids, and its texture blocks
        // follow the PS2 ones (the same groups). A PS2 texture's PS3 counterpart is, among the PS3 textures with its
        // BMP id or in its block (when the two worlds have the same blocks), the one of the same aspect that
        // matches it best once scaled down.
        std::map<int32_t, std::vector<const TEXIMG*>> mpbmp3;
        std::map<int, std::vector<const TEXIMG*>> mpblock3;
        for (const TEXIMG& img : aimg3)
        {
            mpbmp3[img.idBmp].push_back(&img);
            mpblock3[img.iblock].push_back(&img);
        }
        const bool fSameBlocks = ablk2.size() == ablk3.size() &&
                                 std::equal(ablk2.begin(), ablk2.end(), ablk3.begin(), [](const BLOCK& a, const BLOCK& b) { return a.cshd == b.cshd; });
        int cw = 0;
        std::set<std::string> setNameWorld;
        for (const TEXIMG& img2 : aimg2)
        {
            const std::string name = SzName(img2);
            if (!setNameWorld.insert(name).second) continue;
            const TEXIMG* pimg3 = nullptr;
            const char* how = "";
            MATCH best;
            auto consider = [&](const std::vector<const TEXIMG*>& a, const char* szHow) {
                for (const TEXIMG* p : a)
                {
                    if (uint64_t(p->dx) * img2.dy != uint64_t(p->dy) * img2.dx) continue;  // (another aspect)
                    if (p == pimg3) continue;
                    const MATCH m = MatchAgainst(img2, *p);
                    if (m.Score() < best.Score()) { best = m; pimg3 = p; how = szHow; }
                }
            };
            if (auto it3 = mpbmp3.find(img2.idBmp); it3 != mpbmp3.end()) consider(it3->second, "bmp");
            if (fSameBlocks)
                if (auto it3 = mpblock3.find(img2.iblock); it3 != mpblock3.end()) consider(it3->second, "block");
            const char* result;
            double mad = best.mad;
            if (!pimg3) { result = "no-ps3"; ++cmissing; mad = -1; }
            else if (pimg3->dx == img2.dx && pimg3->dy == img2.dy && pimg3->rgba == img2.rgba) { result = "same"; ++csame; }
            else if (!FAccept(best))
            {
                result = "mismatch";
                ++cmismatch;
                if (std::getenv("SLY2_PS3HD_DEBUG"))  // (for a look: the rejected ones beside)
                {
                    fs::create_directories(cane::path(out) / "Rejected");
                    FWritePng(cane::utf8(cane::path(out) / "Rejected" / name), *pimg3);
                }
            }
            else if (uint64_t(pimg3->dx) * pimg3->dy < uint64_t(img2.dx) * img2.dy) { result = "smaller"; ++csame; }
            else if (setWritten.count(name)) result = "dup";
            else
            {
                std::vector<uint8_t> ab;
                if (FEncodePng(*pimg3, ab) && pack.Add(name, ab.data(), ab.size()))
                {
                    setWritten.insert(name);
                    ++cwritten;
                    ++cw;
                    cbWritten += ab.size();
                    result = "written";
                }
                else result = "write-failed";
            }
            if (pfTsv)
                std::fprintf(pfTsv, "%s\t%s\t%dx%d\t%s\t%d\t%d\t%.2f\t%.3f\t%s\t%s\n", it->second.name.c_str(), name.c_str(), img2.dx, img2.dy,
                             pimg3 ? (std::to_string(pimg3->dx) + "x" + std::to_string(pimg3->dy)).c_str() : "-", img2.idBmp,
                             img2.idClut, mad, best.r, result, how);
        }
        const size_t cmap2 = aimg2.size(), cmap3 = aimg3.size();
        std::printf("ps3hd: %-28s blocks %zu / %zu (PS2 / PS3), textures %zu / %zu, %d written\n", it->second.name.c_str(),
                    ablk2.size(), ablk3.size(), cmap2, cmap3, cw);
        std::fflush(stdout);
    };

    // SLY2.WAL in order, a sector at a time: a world runs from its first sector to the next file's
    std::vector<uint8_t> world, carry;
    uint64_t ibWorld = 0;
    char kindCur = 0;
    auto sector = [&](uint64_t at, const uint8_t* p) {
        const char k = KindOfSector(p);
        if (k)
        {
            if (kindCur == 'w' && !world.empty()) processWorld(world, ibWorld);
            world.clear();
            kindCur = k;
            ibWorld = at;
        }
        if (kindCur == 'w') world.insert(world.end(), p, p + SECTOR);
    };
    const bool ok = disc.read(
        {"SLY2/SLY2.WAL"},
        [&](size_t, uint64_t at, const uint8_t* p, size_t n) {
            // (whole sectors: a part sector waits in carry)
            size_t i = 0;
            if (!carry.empty())
            {
                const size_t take = std::min(n, SECTOR - carry.size());
                carry.insert(carry.end(), p, p + take);
                i = take;
                if (carry.size() == SECTOR) sector(at + take - SECTOR, carry.data()), carry.clear();
            }
            for (; i + SECTOR <= n; i += SECTOR) sector(at + i, p + i);
            if (i < n) carry.assign(p + i, p + n);
            return true;
        },
        [&](uint64_t done, uint64_t total) {
            Progress("read", done, total, "");
            return true;
        },
        err);
    if (!ok) return fail(err);
    if (kindCur == 'w' && !world.empty()) processWorld(world, ibWorld);
    if (pfTsv) std::fclose(pfTsv);
    ResetShaders();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("ps3hd: %d worlds, %d HD textures written (%.1f MB), %d unchanged, %d not matching (kept PS2), %d without a PS3 "
                "texture, %.0f s\n", cworld, cwritten, cbWritten / 1e6, csame, cmismatch, cmissing, s);
    if (!cworld) return fail("No world of the disc matched this Sly 2 installation's worlds.");
    if (!pack.Close()) return fail("Cannot write the texture pack in " + out + ".");
    return 0;
}
