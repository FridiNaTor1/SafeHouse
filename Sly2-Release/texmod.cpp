// See texmod.h.
#include "texmod.h"

#include "cls.h"
#include "disc.h"
#include "shd.h"
#include "sw.h"
#include "../platform/platform.h"

#include <glad/glad.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
uint64_t HashRgba(const std::vector<uint8_t>& ab)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : ab) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

// The replacement textures by name: the mods folder's, then the texture pack's (--texture-pack: the PS3 HD pack)
// where the mods folder has none of that name. Scanned once.
std::string s_dirPack;
const std::unordered_map<std::string, std::string>& ModTextures()
{
    static const std::unordered_map<std::string, std::string> s_map = [] {
        std::unordered_map<std::string, std::string> map;
        auto scan = [&](const std::string& root, const char* what) {
            if (root.empty()) return;
            std::error_code ec;
            const fs::path dir = cane::path(root) / "Textures";
            if (!fs::is_directory(dir, ec)) return;
            size_t c = 0;
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (e.is_regular_file() && e.path().extension() == ".png" && map.emplace(cane::utf8(e.path().filename()), cane::utf8(e.path())).second) ++c;
            if (c) std::printf("mods: %zu textures%s\n", c, what);
        };
        scan(g_disc.DirMods(), "");
        scan(s_dirPack, " (texture pack)");
        return map;
    }();
    return s_map;
}
}  // namespace

void SetTexturePackDirectory(const std::string& dir) { s_dirPack = dir; }

std::string SzTextureModName(const std::vector<uint8_t>& rgba, int dx, int dy)
{
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%dx%d_%016llx.png", dx, dy, static_cast<unsigned long long>(HashRgba(rgba)));
    return sz;
}

// The game's texels: rows bottom-up, RGB as the CLUT holds it, alpha in the PS2's range (0x80 = opaque). The PNG:
// top-down rows, alpha doubled (255 for 0x80); the load halves it back (exact for every alpha up to 0x80).
bool FLoadTextureMod(const std::vector<uint8_t>& rgba, int dx, int dy, std::vector<uint8_t>& out, int* pw, int* ph)
{
    const auto& map = ModTextures();
    if (map.empty()) return false;
    const auto it = map.find(SzTextureModName(rgba, dx, dy));
    if (it == map.end()) return false;
    const std::string& path = it->second;
    int w = 0, h = 0;
    stbi_uc* pb = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    if (!pb || w <= 0 || h <= 0 || w > 16384 || h > 16384)
    {
        std::fprintf(stderr, "mods: cannot read %s\n", path.c_str());
        if (pb) stbi_image_free(pb);
        return false;
    }
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const stbi_uc* ps = pb + (size_t(y) * w + x) * 4;
            uint8_t* pd = out.data() + (size_t(h - 1 - y) * w + x) * 4;
            for (int k = 0; k < 3; ++k) pd[k] = ps[k];
            pd[3] = uint8_t((ps[3] + 1) / 2);
        }
    stbi_image_free(pb);
    *pw = w;
    *ph = h;
    return true;
}

namespace
{
std::unordered_map<unsigned, double> s_settexMod;   // a texture mod's GL texture: its GPU bytes (with mips)
double s_cbMods = 0;
}

unsigned GlTexFromMod(const std::vector<uint8_t>& rgba, int dx, int dy)
{
    std::vector<uint8_t> ab;
    int w = 0, h = 0;
    if (!FLoadTextureMod(rgba, dx, dy, ab, &w, &h)) return 0;
    GLint cMax = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &cMax);
    if (cMax > 0 && (w > cMax || h > cMax))
    {
        std::fprintf(stderr, "mods: %s is %dx%d, over this GPU's limit (%d): not used\n", SzTextureModName(rgba, dx, dy).c_str(), w, h,
                     int(cMax));
        return 0;
    }
    // the full mip chain (sampled trilinear by render.cpp's mipmapped samplers, so a large replacement does not
    // shimmer at a distance); level 0 is the PNG as is
    int clevel = 1;
    for (int d = std::max(w, h); d > 1; d >>= 1) ++clevel;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexStorage2D(GL_TEXTURE_2D, clevel, GL_RGBA8, w, h);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
    if (clevel > 1) glGenerateMipmap(GL_TEXTURE_2D);
    const double cb = double(w) * h * 4 * (clevel > 1 ? 4.0 / 3.0 : 1.0);
    s_settexMod[tex] = cb;
    s_cbMods += cb;
    // (SLY2N_MODSTATS=1: each replacement's upload, with the live count and GPU memory; MODDING.md "Limits")
    static const bool s_fStats = std::getenv("SLY2N_MODSTATS") != nullptr;
    if (s_fStats)
        std::printf("mods: uploaded %dx%d for %dx%d (%zu textures, %.1f MiB with mips)\n", w, h, dx, dy, s_settexMod.size(),
                    s_cbMods / 1048576.0);
    return tex;
}

bool FTextureIsMod(unsigned tex) { return tex != 0 && s_settexMod.count(tex) != 0; }
void ForgetModTexture(unsigned tex)
{
    const auto it = s_settexMod.find(tex);
    if (it == s_settexMod.end()) return;
    s_cbMods -= it->second;
    s_settexMod.erase(it);
}

namespace
{
// One world's textures into dir (setName: the names already written, by this world or an earlier one).
int DumpWorldTextures(const std::string& dir, const FK& fk, int filter, const std::string& world, std::set<std::string>& setName)
{
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) { std::fprintf(stderr, "world %s did not load\n", world.c_str()); return -1; }
    std::set<std::pair<const BMP*, const CLUT*>> setDone;
    std::string err;
    int c = 0;
    for (const auto& pshd : g_shds.ashd)
    {
        if (!pshd || pshd->atex.empty()) continue;
        const int cframe = std::max<int>(1, int(pshd->atex[0].apbmp.size()));
        for (int iframe = 0; iframe < cframe; ++iframe)
            for (int ipass = 0; ipass < 3; ++ipass)
            {
                const TEXSEL sel = TexselFromShd(pshd.get(), iframe, ipass);
                if (!sel.pbmp || !setDone.insert({sel.pbmp, sel.pclut}).second) continue;
                std::vector<uint8_t> rgba;
                if (!FDecodeTexture(sel.pbmp, sel.pclut, rgba, err)) continue;
                const int dx = sel.pbmp->dx, dy = sel.pbmp->dy;
                const std::string name = SzTextureModName(rgba, dx, dy);
                if (!setName.insert(name).second) continue;  // (the same texels under another CLUT / bitmap, or world)
                std::vector<uint8_t> ab(rgba.size());
                for (int y = 0; y < dy; ++y)
                    for (int x = 0; x < dx; ++x)
                        for (int k = 0; k < 4; ++k)
                        {
                            const uint8_t v = rgba[(size_t(y) * dx + x) * 4 + k];
                            ab[(size_t(dy - 1 - y) * dx + x) * 4 + k] = k < 3 ? v : uint8_t(std::min(255, v * 2));
                        }
                const std::string path = cane::utf8(cane::path(dir) / name);
                if (stbi_write_png(path.c_str(), dx, dy, 4, ab.data(), dx * 4)) ++c;
            }
    }
    return c;
}
}  // namespace

int DumpTexturesCommand(const std::string& dir, const std::string& world)
{
    InitClasses();
    FK fk;
    int filter = 0;
    std::string err;
    if (!FResolveWorld(world, fk, filter, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    fs::create_directories(cane::path(dir));
    std::set<std::string> setName;
    const int c = DumpWorldTextures(dir, fk, filter, world, setName);
    if (c < 0) return 1;
    std::printf("%d textures of world %s written to %s\n", c, world.c_str(), dir.c_str());
    return 0;
}

int ExportAllTextures(const std::string& dir, bool fProgress)
{
    InitClasses();
    std::error_code ec;
    fs::create_directories(cane::path(dir), ec);
    // the worlds: the boot world (its logo screens), then the records (from the boot world) that have a file
    std::vector<std::pair<std::string, std::pair<FK, int>>> aworld;
    std::string err;
    FK fk;
    int filter = 0;
    if (FResolveWorld("boot", fk, filter, err)) aworld.push_back({"boot", {fk, filter}});
    for (int iwrec = 0;; ++iwrec)
    {
        err.clear();
        if (FResolveWorld(std::to_string(iwrec), fk, filter, err)) aworld.push_back({std::to_string(iwrec), {fk, filter}});
        else if (err.find("no world record") != std::string::npos || err.find("boot world") != std::string::npos) break;
    }
    std::set<std::string> setName;
    int c = 0;
    for (size_t i = 0; i < aworld.size(); ++i)
    {
        const int cw = DumpWorldTextures(dir, aworld[i].second.first, aworld[i].second.second, aworld[i].first, setName);
        if (cw > 0) c += cw;
        if (fProgress)
        {
            std::printf("textures %zu %zu %s\n", i + 1, aworld.size(), aworld[i].first.c_str());
            std::fflush(stdout);
        }
    }
    std::printf("%d textures of %zu worlds written to %s\n", c, aworld.size(), dir.c_str());
    return aworld.empty() ? -1 : c;
}

int ExportTexturesCommand(const std::string& dirIn)
{
    const fs::path dir = cane::path(dirIn);
    fs::path partial = dir;
    partial += ".partial";
    std::error_code ec;
    fs::remove_all(partial, ec);
    if (ExportAllTextures(cane::utf8(partial), true) < 0)
    {
        std::fprintf(stderr, "export-textures: no world loaded\n");
        return 1;
    }
    fs::remove_all(dir, ec);
    fs::rename(partial, dir, ec);
    if (ec)
    {
        std::fprintf(stderr, "export-textures: cannot rename %s: %s\n", cane::utf8(partial).c_str(), ec.message().c_str());
        return 1;
    }
    return 0;
}
