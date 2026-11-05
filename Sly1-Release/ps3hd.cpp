// The PS3 collection's HD textures for Sly 1: The Sly Collection's textures put over this
// installation's as a texture pack (OUT/Textures/<the PCBRX texture's mods name>.png, texmod.h).
//
// The game never reads PS3 data: its levels stay the installation's (PCBRX, rebuilt from the PS2 disc), so it plays
// as before by construction. This tool only reads the PS3 levels to take their textures.
//
// The PS3 levels (SLY1/SLY.WAL, listed by SLY.WAC: name[23], type, sector, size; 'W' a level, "_f" / "_s" its French
// and Spanish copies) are the PS2 levels rebuilt: the same LZ, text as big-endian UTF-16, and the texture tables
// (CLUTs, BMPs, shaders with their texture references, fonts: the PS2 layout, little-endian) with the bitmaps redrawn
// at up to 4x the size, nearly all 8-bit with 256-colour palettes. The texels (offsets from the texture data, the
// level's last part: its size is the level's first word) are GS-style linear data. A PS3 level is matched with its
// PCBRX level by its textures (votes, below), and each PCBRX image (one BMP + CLUT of the PS2 level, BC1 / BC3 /
// RGBA8) with the PS3 image that matches it best once scaled down; it is taken only when it is the same art.
#include "texmod.h"
#include "shd.h"
#include "../platform/platform.h"
#include "../platform/ps3disc.h"

#include <stb/stb_image_write.h>

#include <algorithm>
#include <array>
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
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The Sly LZ (platform/brx_decompress.h), tolerant of the padding after the stream (a WAC size is sector padded).
std::vector<uint8_t> DecompressTolerant(const uint8_t* in, size_t cb)
{
    std::vector<uint8_t> out;
    out.reserve(cb * 2);
    uint8_t work[0x2000] = {};
    size_t op = 0, ip = 0;
    uint32_t flags = 0;
    while (ip + 2 < cb)
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

// An image to pair: RGBA in the PS2's row order (row 0 first, as the GS keeps it) and the PS2's alpha (0x80 opaque).
struct IMG
{
    int dx = 0, dy = 0;
    std::vector<uint8_t> rgba;
    std::array<float, 64> thumb{};  // 4x4 cells x premultiplied RGB + A (the level votes)
};

void Thumb4(IMG& img)
{
    std::array<double, 64> acc{};
    std::array<int, 16> n{};
    for (int y = 0; y < img.dy; ++y)
        for (int x = 0; x < img.dx; ++x)
        {
            const int c = (y * 4 / img.dy) * 4 + x * 4 / img.dx;
            const uint8_t* p = &img.rgba[(size_t(y) * img.dx + x) * 4];
            const double a = std::min(1.0, p[3] / 128.0);
            for (int k = 0; k < 3; ++k) acc[size_t(c) * 4 + k] += p[k] * a;
            acc[size_t(c) * 4 + 3] += p[3];
            ++n[size_t(c)];
        }
    for (int c = 0; c < 16; ++c)
        for (int k = 0; k < 4; ++k) img.thumb[size_t(c) * 4 + k] = n[size_t(c)] ? float(acc[size_t(c) * 4 + k] / n[size_t(c)]) : 0.f;
}

// ---------------------------------------------------------------------------------------------- the PS3 levels

struct CLUT3 { uint16_t count = 0, size = 0; uint32_t offset = 0; };
struct BMP3 { uint16_t dx = 0, dy = 0; uint8_t psm = 0; uint32_t cb = 0, offset = 0; };
struct TEXREF { uint8_t shdk = 0; std::vector<uint16_t> bmps, cluts; };
struct LEVEL3
{
    std::vector<CLUT3> cluts;
    std::vector<BMP3> bmps;
    std::vector<TEXREF> texs;
    std::vector<std::pair<uint16_t, uint16_t>> fonts;  // (bmp, clut)
    size_t ibTables = 0, ibEnd = 0, ibData = 0;
};

// The texture tables at ib, parsed strictly (every count in range, every reference valid): the PS2's
// LoadShadersFromBrx layout (File Extractor GUI/NativeTools/BrxRebuilder/AssetWalker.cpp).
bool FParseTables(const std::vector<uint8_t>& ab, size_t ib, LEVEL3& lv)
{
    const uint8_t* pb = ab.data();
    const size_t cb = ab.size();
    size_t o = ib;
    auto has = [&](size_t n) { return o <= cb && n <= cb - o; };
    lv = {};
    if (!has(2)) return false;
    const int nc = U16(pb + o); o += 2;
    if (nc < 1 || nc > 8192 || !has(size_t(nc) * 12)) return false;
    for (int i = 0; i < nc; ++i, o += 12)
    {
        CLUT3 c{U16(pb + o + 4), U16(pb + o + 6), U32(pb + o + 8)};
        if (c.count < 1 || c.count > 256 || (c.size != 1 && c.size != 2 && c.size != 4)) return false;
        lv.cluts.push_back(c);
    }
    if (!has(2)) return false;
    const int nb = U16(pb + o); o += 2;
    if (nb < 1 || nb > 8192 || !has(size_t(nb) * 20)) return false;
    for (int i = 0; i < nb; ++i, o += 20)
    {
        BMP3 b{U16(pb + o), U16(pb + o + 2), pb[o + 8], U32(pb + o + 12), U32(pb + o + 16)};
        const int bpp = b.psm == 0x13 ? 8 : b.psm == 0x14 ? 4 : b.psm == 0 ? 32 : 0;
        if (b.dx < 1 || b.dy < 1 || b.dx > 4096 || b.dy > 4096 || !bpp) return false;
        if (b.psm != 0 && uint64_t(b.cb) * 8 < uint64_t(b.dx) * b.dy * bpp) return false;  // (32-bit: render targets, no texels)
        lv.bmps.push_back(b);
    }
    if (!has(4)) return false;
    const int ns = U16(pb + o); o += 4;
    if (ns > 16384) return false;
    int cref = 0, cbad = 0;
    for (int s = 0; s < ns; ++s)
    {
        if (!has(4 + 8 + 4 + 2 + 1 + 1 + 2)) return false;
        const uint8_t shdk = pb[o];
        if (shdk > 15) return false;
        o += 4 + 8 + 4 + 2 + 1;
        const int nt = pb[o++];
        if (nt > 8) return false;
        const uint16_t saa = U16(pb + o); o += 2;
        if (saa != 0 && saa != 0xFFFF)
        {
            if (saa > 8 || !has(28)) return false;
            o += 28;
        }
        for (int t = 0; t < nt; ++t)
        {
            if (!has(6)) return false;
            const int nbi = pb[o + 4], nci = pb[o + 5];
            o += 6;
            if (!has(size_t(nbi + nci) * 2)) return false;
            TEXREF tex;
            tex.shdk = shdk;
            for (int k = 0; k < nbi; ++k, o += 2)
            {
                const uint16_t v = U16(pb + o);
                // (retail data keeps a few unresolved references, as the PS2's: they are not drawn, nor used here)
                if (v != 0xFFFF && v >= nb) ++cbad;
                tex.bmps.push_back(v != 0xFFFF && v >= nb ? uint16_t(0xFFFF) : v);
                ++cref;
            }
            for (int k = 0; k < nci; ++k, o += 2)
            {
                const uint16_t v = U16(pb + o);
                if (v != 0xFFFF && v >= nc) ++cbad;
                tex.cluts.push_back(v != 0xFFFF && v >= nc ? uint16_t(0xFFFF) : v);
                ++cref;
            }
            lv.texs.push_back(std::move(tex));
        }
    }
    if (!has(2)) return false;
    const int nf = U16(pb + o); o += 2;
    if (nf > 64) return false;
    for (int f = 0; f < nf; ++f)
    {
        if (!has(16)) return false;
        const uint16_t bmp = U16(pb + o);
        const int16_t clut = int16_t(U16(pb + o + 2));
        const uint32_t ng = U16(pb + o + 12);  // (the PS3's upper half is not cleared)
        if (bmp >= nb || clut < 0 || clut >= nc || ng > 0x10000) return false;
        o += 16;
        if (!has(size_t(ng) * 8)) return false;
        o += size_t(ng) * 8;
        lv.fonts.push_back({bmp, uint16_t(clut)});
    }
    if (lv.texs.empty() || cbad * 4 > cref) return false;
    lv.ibTables = ib;
    lv.ibEnd = o;
    return true;
}

// The candidates for the start of the texture data, highest first: the data is the level's last part, 16-aligned,
// after the objects, and every palette read from there must have the PS2's alpha range (0..0x80). A few starts 16
// bytes apart pass that (palettes follow palettes); the caller picks the one whose images match the installation's.
std::vector<size_t> DataCandidates(const std::vector<uint8_t>& ab, const LEVEL3& lv)
{
    std::vector<size_t> out;
    uint64_t cbMax = 0;
    for (const BMP3& b : lv.bmps) cbMax = std::max<uint64_t>(cbMax, uint64_t(b.offset) + b.cb);
    if (cbMax > ab.size()) return out;
    std::vector<uint32_t> offs;
    for (const CLUT3& c : lv.cluts) offs.push_back(c.offset);
    for (const BMP3& b : lv.bmps) offs.push_back(b.offset);
    std::sort(offs.begin(), offs.end());
    auto size = [&](const CLUT3& c) {
        size_t n = std::min<size_t>(size_t(c.count) * c.size * 4, 1024);
        const auto it = std::upper_bound(offs.begin(), offs.end(), c.offset);
        if (it != offs.end()) n = std::min<size_t>(n, *it - c.offset);
        return n;
    };
    const size_t top = (ab.size() - size_t(cbMax)) & ~size_t(15);
    const size_t bottom = std::max(lv.ibEnd, top > (8u << 20) ? top - (8u << 20) : size_t(0));
    size_t lastPass = 0;
    for (size_t c = top; c >= bottom && c >= 16 && out.size() < 64; c -= 16)
    {
        bool ok = true;
        for (const CLUT3& cl : lv.cluts)
        {
            const size_t n = size(cl);
            if (c + cl.offset + n > ab.size()) { ok = false; break; }
            for (size_t i = 3; i < n && ok; i += 4) ok = ab[c + cl.offset + i] <= 0x80;
            if (!ok) break;
        }
        if (!ok)
        {
            if (lastPass && lastPass - c > 0x4000) break;  // (past the passing run)
            continue;
        }
        lastPass = c;
        out.push_back(c);
    }
    return out;
}

// The PS2's CLUT order for 256 colours (CSM1): 8 entries swapped in each 32.
uint8_t Csm(uint8_t i)
{
    const uint8_t j = i & 0x18;
    return j == 8 ? uint8_t(i + 8) : j == 0x10 ? uint8_t(i - 8) : i;
}

bool FDecode3(const std::vector<uint8_t>& ab, const LEVEL3& lv, uint16_t ibmp, uint16_t iclut, IMG& img)
{
    if (ibmp >= lv.bmps.size()) return false;
    const BMP3& b = lv.bmps[ibmp];
    const size_t ib = lv.ibData + b.offset;
    if (ib + b.cb > ab.size()) return false;
    img.dx = b.dx;
    img.dy = b.dy;
    img.rgba.assign(size_t(b.dx) * b.dy * 4, 0);
    const size_t np = size_t(b.dx) * b.dy;
    if (b.psm == 0)
    {
        if (b.cb < np * 4) return false;
        std::memcpy(img.rgba.data(), ab.data() + ib, np * 4);
        return true;
    }
    if (iclut >= lv.cluts.size()) return false;
    const CLUT3& c = lv.cluts[iclut];
    const size_t ic = lv.ibData + c.offset;
    const size_t ncolor = b.psm == 0x13 ? 256 : 16;
    if (ic + std::min<size_t>(ncolor, size_t(c.count) * c.size * 4 / 4) * 4 > ab.size()) return false;
    const size_t cbPal = std::min<size_t>(ncolor * 4, ab.size() - ic);
    for (size_t p = 0; p < np; ++p)
    {
        size_t k;
        if (b.psm == 0x13) k = Csm(ab[ib + p]);
        else k = (ab[ib + p / 2] >> ((p & 1) * 4)) & 15;
        if (k * 4 + 3 >= cbPal) continue;
        std::memcpy(&img.rgba[p * 4], ab.data() + ic + k * 4, 4);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- the PCBRX levels

std::vector<uint8_t> ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

// The English (else the default) payload of a PCBRX file (v17, or v19's unified container: PCBRX-v19.md).
std::vector<uint8_t> Payload(const fs::path& path)
{
    const std::vector<uint8_t> data = ReadFile(path);
    if (data.size() < 8 || U32(data.data()) != 0x52424350u) return {};
    const uint32_t version = U32(data.data() + 4);
    if (version == 17) return data;
    if (version != 19 || data.size() < 28) return {};
    size_t o = 8;
    const uint32_t ccountry = U32(data.data() + o + 8), deflang = U32(data.data() + o + 12), cchunk = U32(data.data() + o + 16);
    o += 20;
    std::map<uint32_t, std::vector<uint32_t>> mplang;
    for (uint32_t i = 0; i < ccountry && o + 16 <= data.size(); ++i)
    {
        const uint32_t lang = U32(data.data() + o), cref = U32(data.data() + o + 12);
        o += 16;
        if (o + size_t(cref) * 4 > data.size()) return {};
        for (uint32_t r = 0; r < cref; ++r, o += 4) mplang[lang].push_back(U32(data.data() + o));
    }
    std::vector<std::pair<size_t, uint32_t>> chunks;
    for (uint32_t i = 0; i < cchunk && o + 8 <= data.size(); ++i)
    {
        const uint32_t cb = U32(data.data() + o);
        o += 8;
        if (o + cb > data.size()) return {};
        chunks.emplace_back(o, cb);
        o += cb;
    }
    const auto it = mplang.count(0) ? mplang.find(0) : mplang.find(deflang);
    if (it == mplang.end()) return {};
    std::vector<uint8_t> out;
    for (uint32_t chunk : it->second)
    {
        if (chunk >= chunks.size()) return {};
        out.insert(out.end(), data.begin() + ptrdiff_t(chunks[chunk].first), data.begin() + ptrdiff_t(chunks[chunk].first + chunks[chunk].second));
    }
    return out;
}

// One image of a PCBRX level: its mods name and its texels in the PS2's order.
struct IMG2
{
    std::string name;
    IMG img;
};

// The image table (the rebuilder's: u32 count, then 32-byte records w, h, zone, mips, bytes, offset, flags, format;
// the texels packed in order at the payload's end, before an optional PLOC block), found by a strict scan.
std::vector<IMG2> LoadPcbrxImages(const fs::path& path, std::string& error)
{
    std::vector<IMG2> out;
    const std::vector<uint8_t> p = Payload(path);
    if (p.empty()) { error = "not a PCBRX level"; return out; }
    size_t end = p.size();
    if (end >= 8 && std::memcmp(p.data() + end - 8, "PLOC", 4) == 0 && U32(p.data() + end - 4) <= end - 8) end -= 8 + U32(p.data() + end - 4);
    for (size_t ib = 8; ib + 4 + 32 <= end; ++ib)
    {
        const uint32_t n = U32(p.data() + ib);
        if (n < 1 || n > 65535 || ib + 4 + size_t(n) * 32 > end) continue;
        // the first record must be well formed before the whole table is checked
        auto rec = [&](uint32_t i) { return p.data() + ib + 4 + size_t(i) * 32; };
        auto good = [&](uint32_t i, uint64_t expectOffset) {
            const uint8_t* r = rec(i);
            const uint32_t w = U32(r), h = U32(r + 4), mips = U32(r + 12), cb = U32(r + 16), off = U32(r + 20), flags = U32(r + 24), fmt = U32(r + 28);
            if (w < 1 || h < 1 || w > 8192 || h > 8192 || mips != 1 || fmt > 2 || flags > 7 || off != expectOffset) return uint64_t(0);
            const uint64_t want = fmt == 0 ? uint64_t(w) * h * 4 : uint64_t((w + 3) / 4) * ((h + 3) / 4) * (fmt == 1 ? 8 : 16);
            return cb == want ? want : uint64_t(0);
        };
        uint64_t at = 0;
        uint32_t i = 0;
        for (; i < n; ++i)
        {
            const uint64_t cb = good(i, at);
            if (!cb) break;
            at += cb;
        }
        if (i != n || at > end - (ib + 4 + size_t(n) * 32)) continue;
        const size_t ibData = end - size_t(at);
        out.reserve(n);
        for (i = 0; i < n; ++i)
        {
            const uint8_t* r = rec(i);
            BMP b{};
            b.bmpWidth = U32(r);
            b.bmpHeight = U32(r + 4);
            b.mipCount = 1;
            b.cbPixels = U32(r + 16);
            b.textureFlags = U32(r + 24);
            b.textureFormat = U32(r + 28);
            b.diffuseTexture.assign(p.begin() + ptrdiff_t(ibData + U32(r + 20)), p.begin() + ptrdiff_t(ibData + U32(r + 20) + b.cbPixels));
            IMG2 im;
            std::vector<uint8_t> stored;
            if (!DecodeTextureLevel0(b, stored)) continue;
            im.name = TextureModName(stored, b.bmpWidth, b.bmpHeight);
            im.img.dx = int(b.bmpWidth);
            im.img.dy = int(b.bmpHeight);
            // to the PS2's order and alpha: fonts were flipped by the rebuilder (texmod.cpp FStoredFlipped)
            im.img.rgba.resize(stored.size());
            const bool flipped = (b.textureFlags & 4) == 0;
            const size_t row = size_t(b.bmpWidth) * 4;
            for (uint32_t y = 0; y < b.bmpHeight; ++y)
            {
                const uint8_t* s = stored.data() + (flipped ? b.bmpHeight - 1 - y : y) * row;
                uint8_t* d = im.img.rgba.data() + y * row;
                for (size_t x = 0; x < row; x += 4)
                {
                    d[x] = s[x];
                    d[x + 1] = s[x + 1];
                    d[x + 2] = s[x + 2];
                    d[x + 3] = uint8_t((s[x + 3] * 128 + 127) / 255);
                }
            }
            out.push_back(std::move(im));
        }
        return out;
    }
    error = "no texture table";
    return out;
}

// ---------------------------------------------------------------------------------------------- matching

// As Sly2-Release/ps3hd.cpp: the PS3 texels scaled to the PS2 size against the PS2 texels, premultiplied by alpha:
// the mean absolute difference (0..255 a channel, the PS2's alpha) and the correlation of the luminance on a coarse
// grid (the PS2's dithering and the PS3's smooth redraw agree there).
struct MATCH
{
    double mad = 1e9, r = 0, sd2 = 0;
    double Score() const { return mad + 20.0 * (1.0 - r); }
};

MATCH MatchAgainst(const IMG& a2, const IMG& a3)
{
    const int dx = a2.dx, dy = a2.dy;
    const bool fBox = a3.dx % dx == 0 && a3.dy % dy == 0;
    const int sx = std::max(1, a3.dx / dx), sy = std::max(1, a3.dy / dy);
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
                        for (int i = 0; i < sx; ++i) v[k] += a3.rgba[(size_t(y * sy + j) * a3.dx + size_t(x * sx + i)) * 4 + k];
                    v[k] /= double(sx * sy);
                }
                else
                {
                    const int X = std::min(a3.dx - 1, x * a3.dx / dx), Y = std::min(a3.dy - 1, y * a3.dy / dy);
                    v[k] = a3.rgba[(size_t(Y) * a3.dx + size_t(X)) * 4 + k];
                }
            }
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

// The thresholds of Sly 2's pairs (checked by eye there, and on Sly 1's). The PCBRX side is BC1 / BC3
// compressed (a few levels of error), well inside them.
bool FAccept(const MATCH& m) { return m.mad <= 12.0 && (m.r >= 0.85 || (m.sd2 < 4.0 && m.mad < 3.0)); }

float ThumbDistance(const IMG& a, const IMG& b)
{
    float d = 0;
    for (size_t i = 0; i < 64; ++i) d += std::fabs(a.thumb[i] - b.thumb[i]);
    return d / 64;
}

// The PNG of a PS3 image under a PCBRX name: rows as --dump-textures writes them (the PS2's order reversed), alpha
// 0..255.
bool FWritePng(const fs::path& path, const IMG& img)
{
    std::vector<uint8_t> ab(img.rgba.size());
    const size_t row = size_t(img.dx) * 4;
    for (int y = 0; y < img.dy; ++y)
    {
        const uint8_t* s = img.rgba.data() + size_t(y) * row;
        uint8_t* d = ab.data() + size_t(img.dy - 1 - y) * row;
        for (size_t x = 0; x < row; x += 4)
        {
            d[x] = s[x];
            d[x + 1] = s[x + 1];
            d[x + 2] = s[x + 2];
            d[x + 3] = uint8_t(std::min(255u, unsigned(s[x + 3]) * 255u / 128u));
        }
    }
    return stbi_write_png(cane::utf8(path).c_str(), img.dx, img.dy, 4, ab.data(), int(row)) != 0;
}

struct WACENTRY { std::string name; char type = 0; uint64_t ib = 0, cb = 0; };

std::vector<WACENTRY> ParseWac(const std::vector<uint8_t>& ab)
{
    std::vector<WACENTRY> out;
    if (ab.size() < 4) return out;
    const uint32_t n = U32(ab.data());
    if (n == 0 || n > 100000 || 4 + size_t(n) * 32 > ab.size()) return out;
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint8_t* r = ab.data() + 4 + size_t(i) * 32;
        WACENTRY e;
        e.name.assign(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 23));
        e.type = char(r[23]);
        e.ib = uint64_t(U32(r + 24)) * 2048;
        e.cb = U32(r + 28);
        out.push_back(e);
    }
    return out;
}

void Progress(const char* stage, uint64_t done, uint64_t total, const std::string& item)
{
    std::printf("ps3hd-progress %s %llu %llu %s\n", stage, static_cast<unsigned long long>(done), static_cast<unsigned long long>(total), item.c_str());
    std::fflush(stdout);
}
}  // namespace

int Ps3HdTexturesCommand(const fs::path& installation, const std::string& source, const std::string& out)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto fail = [](const std::string& message) {
        std::printf("ps3hd: error: %s\n", message.c_str());
        std::fflush(stdout);
        return 2;
    };
    // The installation's levels, each with its images' thumbnails (the votes) and names.
    struct LEVEL2 { fs::path path; std::string name; std::vector<IMG2> images; };
    std::vector<LEVEL2> alevel2;
    {
        std::error_code ec;
        const fs::path worlds = installation / "Worlds";
        if (!fs::is_directory(worlds, ec)) return fail("Install Sly 1 with this SafeHouse first: no Worlds folder in " + cane::utf8(installation) + ".");
        for (auto it = fs::recursive_directory_iterator(worlds, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".brx" && it->path().stem() == it->path().parent_path().filename())
                alevel2.push_back({it->path(), cane::utf8(it->path().stem()), {}});
        std::sort(alevel2.begin(), alevel2.end(), [](const LEVEL2& a, const LEVEL2& b) { return a.name < b.name; });
        if (alevel2.empty()) return fail("Install Sly 1 with this SafeHouse first: its installation has no levels.");
    }
    cane::ps3::Disc disc;
    std::string error;
    if (!disc.open(cane::path(source), error) || !disc.checkSlyCollection(1, error)) return fail(error);
    std::printf("ps3hd: The Sly Collection (%s): %s\n", disc.kindName(), source.c_str());

    // the installation's images (for the votes: thumbnails only)
    // (one per texture name, with every level holding it: a texture many levels share says little about the level)
    struct THUMB { std::vector<int> levels; int dx, dy; std::array<float, 64> t; };
    std::vector<THUMB> athumb;
    std::map<std::string, size_t> mpthumb;
    for (size_t i = 0; i < alevel2.size(); ++i)
    {
        Progress("prepare", i, alevel2.size(), alevel2[i].name);
        std::string err;
        std::vector<IMG2> images = LoadPcbrxImages(alevel2[i].path, err);
        if (images.empty()) std::printf("ps3hd: %s: %s\n", alevel2[i].name.c_str(), err.c_str());
        for (IMG2& im : images)
        {
            if (im.img.dx < 16 || im.img.dy < 16) continue;
            const auto [it, fNew] = mpthumb.emplace(im.name, athumb.size());
            if (fNew)
            {
                Thumb4(im.img);
                athumb.push_back({{}, im.img.dx, im.img.dy, im.img.thumb});
            }
            THUMB& t = athumb[it->second];
            if (t.levels.empty() || t.levels.back() != int(i)) t.levels.push_back(int(i));
        }
    }

    const fs::path dirOut = cane::path(out), dirTex = dirOut / "Textures";
    std::error_code ec;
    fs::create_directories(dirTex, ec);
    FILE* pfTsv = std::fopen(cane::utf8(dirOut / "ps3hd-textures.tsv").c_str(), "w");
    if (pfTsv) std::fprintf(pfTsv, "ps3_level\tlevel\tps2_name\tps2_size\tps3_size\tmad\tr\tresult\n");
    std::set<std::string> setWritten, setLevelDone;
    int clevel = 0, cwritten = 0, csame = 0, cmismatch = 0, cmissing = 0, csmaller = 0;
    uint64_t cbWritten = 0;

    // A PS3 level: its texture tables, its level in the installation (the most votes), then the pairs.
    auto processLevel = [&](const WACENTRY& e, const std::vector<uint8_t>& comp) {
        const std::vector<uint8_t> ab = DecompressTolerant(comp.data(), comp.size());
        LEVEL3 lv;
        bool found = false;
        for (size_t ib = 4; ib + 64 < ab.size() && !found; ++ib)
        {
            // (a CLUT count, then a CLUT record's colour count / size)
            const uint16_t nc = U16(ab.data() + ib);
            if (nc < 1 || nc > 8192) continue;
            const uint16_t cc = U16(ab.data() + ib + 6), cs = U16(ab.data() + ib + 8);
            if (cc < 1 || cc > 256 || (cs != 1 && cs != 2 && cs != 4)) continue;
            found = FParseTables(ab, ib, lv);
        }
        const std::vector<size_t> astart = found ? DataCandidates(ab, lv) : std::vector<size_t>{};
        if (!found || astart.empty())
        {
            std::printf("ps3hd: %s: %s\n", e.name.c_str(), found ? "no texture data found" : "no texture tables found");
            return;
        }
        // the PS3 images: each BMP with each CLUT a texture or font samples it with
        std::set<std::pair<uint16_t, uint16_t>> setPair;
        for (const TEXREF& t : lv.texs)
            for (size_t f = 0; f < t.bmps.size(); ++f)
            {
                if (t.bmps[f] == 0xFFFF) continue;
                if (t.shdk == 0)  // (three-way: three CLUTs a frame)
                    for (size_t k = 0; k < 3 && f * 3 + k < t.cluts.size(); ++k) setPair.insert({t.bmps[f], t.cluts[f * 3 + k]});
                else if (f < t.cluts.size()) setPair.insert({t.bmps[f], t.cluts[f]});
            }
        for (const auto& f : lv.fonts) setPair.insert(f);
        // the nearest installation image (by thumbnail) of a PS3 image of 32x32 or more, and its distance
        auto nearest = [&](const IMG& im, float& dist) -> const THUMB* {
            dist = 1e30f;
            const THUMB* pbest = nullptr;
            for (const THUMB& t : athumb)
            {
                if (uint64_t(t.dx) * im.dy != uint64_t(t.dy) * im.dx) continue;
                float d = 0;
                for (size_t i = 0; i < 64; ++i) d += std::fabs(t.t[i] - im.thumb[i]);
                d /= 64;
                if (d < dist) dist = d, pbest = &t;
            }
            return pbest;
        };
        // The data start: of the candidates, the one whose larger images (one per BMP) are nearest to the
        // installation's. A quick pass with 8 images keeps the 6 best, a closer one with 32 picks: the most images
        // close to one of the installation's, then the smallest mean distance.
        std::vector<std::pair<uint16_t, uint16_t>> asample;
        {
            std::set<uint16_t> setBmp;
            std::vector<std::pair<uint16_t, uint16_t>> all(setPair.begin(), setPair.end());
            std::stable_sort(all.begin(), all.end(), [&](const auto& a, const auto& b) {
                return uint64_t(lv.bmps[a.first].dx) * lv.bmps[a.first].dy > uint64_t(lv.bmps[b.first].dx) * lv.bmps[b.first].dy;
            });
            for (const auto& pr : all)
                if (pr.second != 0xFFFF && lv.bmps[pr.first].dx >= 32 && lv.bmps[pr.first].dy >= 32 && setBmp.insert(pr.first).second)
                    asample.push_back(pr);
        }
        struct SCORE { int good = 0; double mean = 1e20; bool operator<(const SCORE& o) const { return good != o.good ? good > o.good : mean < o.mean; } };
        auto score = [&](size_t start, size_t nimg) {
            lv.ibData = start;
            SCORE sc;
            double sum = 0;
            int n = 0;
            for (size_t i = 0; i < asample.size() && i < nimg; ++i)
            {
                IMG img;
                if (!FDecode3(ab, lv, asample[i].first, asample[i].second, img)) continue;
                Thumb4(img);
                float d;
                if (!nearest(img, d)) continue;
                sum += std::min(d, 40.f), ++n;
                if (d < 6) ++sc.good;
            }
            if (n) sc.mean = sum / n;
            return sc;
        };
        std::vector<std::pair<SCORE, size_t>> aquick;
        for (size_t start : astart) aquick.push_back({score(start, 8), start});
        std::stable_sort(aquick.begin(), aquick.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (aquick.size() > 6) aquick.resize(6);
        SCORE bestStart;
        size_t startBest = aquick.front().second;
        for (const auto& q : aquick)
        {
            const SCORE sc = score(q.second, 32);
            if (std::getenv("SLY1_PS3HD_DEBUG")) std::printf("ps3hd: %s: data at 0x%zX: %d good, %.2f\n", e.name.c_str(), q.second, sc.good, sc.mean);
            if (sc < bestStart) bestStart = sc, startBest = q.second;
        }
        lv.ibData = startBest;
        std::vector<IMG> aimg3;
        for (const auto& [b, c] : setPair)
        {
            if (c == 0xFFFF && lv.bmps[b].psm != 0) continue;
            IMG img;
            if (!FDecode3(ab, lv, b, c, img)) continue;
            Thumb4(img);
            aimg3.push_back(std::move(img));
        }
        // the votes: each larger PS3 image for the installation's level holding its nearest image
        std::vector<double> avote(alevel2.size(), 0);
        for (const IMG& im : aimg3)
        {
            if (im.dx < 32 || im.dy < 32) continue;
            float best;
            const THUMB* pbest = nearest(im, best);
            if (pbest && best < 12)
                for (int l : pbest->levels) avote[size_t(l)] += 1.0 / double(pbest->levels.size());
        }
        const int ilevel = int(std::max_element(avote.begin(), avote.end()) - avote.begin());
        const double votes = avote[size_t(ilevel)];
        double second = 0;
        for (size_t i = 0; i < avote.size(); ++i)
            if (int(i) != ilevel) second = std::max(second, avote[i]);
        if (votes < 3 || votes < second * 1.2)
        {
            std::printf("ps3hd: %s: no matching level in the installation (%.1f / %.1f votes)\n", e.name.c_str(), votes, second);
            return;
        }
        LEVEL2& l2 = alevel2[size_t(ilevel)];
        if (!setLevelDone.insert(l2.name).second)
        {
            std::printf("ps3hd: %s: %s again (skipped)\n", e.name.c_str(), l2.name.c_str());
            return;
        }
        ++clevel;
        std::string err;
        std::vector<IMG2> aimg2 = LoadPcbrxImages(l2.path, err);
        int cw = 0;
        std::set<std::string> setName;
        for (IMG2& im2 : aimg2)
        {
            if (!setName.insert(im2.name).second) continue;
            Thumb4(im2.img);
            // the candidates: the PS3 images of its aspect, not smaller, nearest by thumbnail; then the full check
            std::vector<std::pair<float, const IMG*>> acand;
            for (const IMG& im3 : aimg3)
                if (uint64_t(im3.dx) * im2.img.dy == uint64_t(im3.dy) * im2.img.dx) acand.push_back({ThumbDistance(im2.img, im3), &im3});
            // (stable: equal distances keep the PS3 order, so every platform picks the same)
            std::stable_sort(acand.begin(), acand.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            const IMG* pbest = nullptr;
            MATCH best;
            for (size_t i = 0; i < acand.size() && i < 8; ++i)
            {
                const MATCH m = MatchAgainst(im2.img, *acand[i].second);
                if (m.Score() < best.Score()) best = m, pbest = acand[i].second;
            }
            const char* result;
            if (!pbest) { result = "no-ps3"; ++cmissing; }
            else if (!FAccept(best)) { result = "mismatch"; ++cmismatch; }
            else if (uint64_t(pbest->dx) * pbest->dy < uint64_t(im2.img.dx) * im2.img.dy) { result = "smaller"; ++csmaller; }
            else if (pbest->dx == im2.img.dx && pbest->dy == im2.img.dy && best.mad < 1.0) { result = "same"; ++csame; }
            else if (setWritten.count(im2.name)) result = "dup";
            else if (FWritePng(dirTex / im2.name, *pbest))
            {
                setWritten.insert(im2.name);
                ++cwritten;
                ++cw;
                cbWritten += fs::file_size(dirTex / im2.name, ec);
                result = "written";
            }
            else result = "write-failed";
            if (const char* sz = std::getenv("SLY1_PS3HD_PAIRS"); sz && pbest)  // (for a look: both sides of each pair)
            {
                const fs::path dir = cane::path(sz) / (FAccept(best) ? "accepted" : "refused");
                fs::create_directories(dir, ec);
                FWritePng(dir / (im2.name.substr(0, im2.name.size() - 4) + "_ps2.png"), im2.img);
                FWritePng(dir / (im2.name.substr(0, im2.name.size() - 4) + "_ps3.png"), *pbest);
            }
            if (pfTsv)
                std::fprintf(pfTsv, "%s\t%s\t%s\t%dx%d\t%s\t%.2f\t%.3f\t%s\n", e.name.c_str(), l2.name.c_str(), im2.name.c_str(), im2.img.dx, im2.img.dy,
                             pbest ? (std::to_string(pbest->dx) + "x" + std::to_string(pbest->dy)).c_str() : "-", pbest ? best.mad : -1.0, best.r, result);
        }
        std::printf("ps3hd: %-22s -> %-28s images %zu / %zu (installation / PS3), %d written (votes %.1f, next %.1f)\n", e.name.c_str(),
                    l2.name.c_str(), aimg2.size(), aimg3.size(), cw, votes, second);
        std::fflush(stdout);
    };

    // One pass over SLY.WAC and SLY.WAL in disc order (the table comes first on the disc; else two passes).
    std::vector<WACENTRY> awant;
    std::vector<uint8_t> abWac;
    const cane::ps3::Disc::File* pwac = disc.find("SLY1/SLY.WAC");
    const cane::ps3::Disc::File* pwal = disc.find("SLY1/SLY.WAL");
    auto takeTable = [&]() -> bool {
        const std::vector<WACENTRY> all = ParseWac(abWac);
        for (const WACENTRY& e : all)
        {
            const bool lang = e.name.size() > 2 && e.name[e.name.size() - 2] == '_' && (e.name.back() == 'f' || e.name.back() == 's');
            if (e.type == 'W' && !lang && e.cb > 0 && e.ib + e.cb <= pwal->size) awant.push_back(e);
        }
        std::sort(awant.begin(), awant.end(), [](const WACENTRY& a, const WACENTRY& b) { return a.ib < b.ib; });
        return !awant.empty();
    };
    size_t iwant = 0;
    std::vector<uint8_t> comp;
    bool fBadTable = false;
    auto sinkWal = [&](uint64_t at, const uint8_t* p, size_t n) {
        const uint64_t at0 = at, end = at + n;
        while (iwant < awant.size() && at < end)
        {
            const WACENTRY& e = awant[iwant];
            if (e.ib + e.cb <= at) { ++iwant; comp.clear(); continue; }
            if (e.ib >= end) break;
            const uint64_t a = std::max(at, e.ib), b = std::min(end, e.ib + e.cb);
            if (comp.empty()) comp.reserve(size_t(e.cb));
            comp.insert(comp.end(), p + (a - at0), p + (b - at0));
            if (b == e.ib + e.cb)
            {
                Progress("textures", iwant, awant.size(), e.name);
                processLevel(e, comp);
                comp.clear();
                ++iwant;
            }
            at = b;
        }
        return iwant < awant.size();
    };
    auto progress = [&](uint64_t done, uint64_t total) {
        Progress("read", done, total, "");
        return true;
    };
    const bool fOnePass = disc.kind() == cane::ps3::Disc::Kind::Folder || (!pwac->extents.empty() && !pwal->extents.empty() && pwac->extents.front().lba < pwal->extents.front().lba);
    if (!fOnePass && (!disc.readAll("SLY1/SLY.WAC", abWac, error) || !takeTable())) return fail(error.empty() ? "SLY1/SLY.WAC is not a table: is the disc image decrypted?" : error);
    abWac.reserve(size_t(pwac->size));
    const bool ok = disc.read(
        fOnePass ? std::vector<std::string>{"SLY1/SLY.WAC", "SLY1/SLY.WAL"} : std::vector<std::string>{"SLY1/SLY.WAL"},
        [&](size_t ifile, uint64_t at, const uint8_t* p, size_t n) {
            if (fOnePass && ifile == 0)
            {
                abWac.insert(abWac.end(), p, p + n);
                if (abWac.size() >= pwac->size && !takeTable()) { fBadTable = true; return false; }
                return true;
            }
            return sinkWal(at, p, n);
        },
        progress, error);
    if (fBadTable) return fail("SLY1/SLY.WAC is not a table of levels: this is not The Sly Collection, or the disc image is still encrypted.");
    if (!ok) return fail(error);
    if (pfTsv) std::fclose(pfTsv);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("ps3hd: %d levels, %d HD textures written (%.1f MB), %d unchanged, %d smaller on the PS3, %d not matching (kept), "
                "%d without a PS3 texture, %.0f s\n", clevel, cwritten, cbWritten / 1e6, csame, csmaller, cmismatch, cmissing, s);
    if (clevel == 0) return fail("No level of the disc matched this installation's levels.");
    return 0;
}
