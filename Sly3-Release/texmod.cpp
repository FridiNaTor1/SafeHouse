// See texmod.h.
#include "texmod.h"

#include "cls.h"
#include "disc.h"
#include "shd.h"
#include "sw.h"
#include "workpool.h"
#include "../platform/platform.h"
#include "../platform/texture_cache.h"

#include "gl.h"
#include "../platform/draw_packet.h"
#include <glad/glad.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
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

// The cache's folder name under cane::cacheDirectory
constexpr const char* kCacheGame = "sly3";

// The game's texels: rows bottom-up, RGB as the CLUT holds it, alpha in the PS2's range (0x80 = opaque). The PNG:
// top-down rows, alpha doubled (255 for 0x80); the load halves it back (exact for every alpha up to 0x80).
bool FDecodeModPng(const std::string& path, std::vector<uint8_t>& out, int* pw, int* ph)
{
    const bool fStats = FTexStats();
    double t0 = fStats ? MsTexStatsNow() : 0.0;
    // (the file read whole, then decoded: one read of its size, on any path the folder scan found)
    std::vector<char> abFile;
    {
        std::ifstream in(cane::path(path), std::ios::binary | std::ios::ate);
        const std::streamoff cb = in ? std::streamoff(in.tellg()) : -1;
        if (cb > 0 && cb < (std::streamoff(1) << 30))
        {
            abFile.resize(size_t(cb));
            in.seekg(0);
            if (!in.read(abFile.data(), cb)) abFile.clear();
        }
    }
    if (fStats) { const double t = MsTexStatsNow(); TexStatsAdd(TEXSTATK_Read, t - t0); t0 = t; }
    int w = 0, h = 0;
    stbi_uc* pb = abFile.empty() ? nullptr : stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(abFile.data()), int(abFile.size()), &w, &h, nullptr, 4);
    if (fStats) { const double t = MsTexStatsNow(); TexStatsAdd(TEXSTATK_Decode, t - t0); t0 = t; }
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
    if (fStats) TexStatsAdd(TEXSTATK_Flip, MsTexStatsNow() - t0);
    *pw = w;
    *ph = h;
    return true;
}

// The cache's threads (texmod.h): one lists the folders, one writes the cache's records. SLY2N_TEXTHREADS=0: none
// (both on the game thread, when first needed).
int CTexThreads()
{
    static const int s_c = std::getenv("SLY2N_TEXTHREADS") ? std::clamp(std::atoi(std::getenv("SLY2N_TEXTHREADS")), 0, 1) : 1;
    return s_c;
}
// (the pool lives to the process's end: QuitTexMods waits for its job)
std::atomic<bool> s_fQuit{false};
std::atomic<uint64_t> s_cbStorePending{0};
CWorkPool* PpoolStore()
{
    static CWorkPool* const s_p = CTexThreads() > 0 ? new CWorkPool(1, ~uint64_t(0), true) : nullptr;
    return s_p;
}

// A set of replacement textures: the mods folder's, then the texture pack's where the mods folder has none of that
// name, with their caches. The game has one (Mods()); the self test makes its own.
struct MODSRC
{
    std::string path;
    int isrc = 0;   // 0 the mods folder, 1 the texture pack
};
class CModStore
{
public:
    // adirMods / dirPack: the folders holding Textures (adirMods: the mod folders, lowest priority first: the packages'
    // files/, the loose mods folder; a later folder's PNG wins; "": none). dirCache: the cache's folder ("": no cache).
    // fSync: cache records are written before Load returns (the self test, the cache build command).
    void Init(const std::vector<std::string>& adirMods, const std::string& dirPack, const std::string& dirCache, bool fSync, bool fQuiet = false)
    {
        const double t0 = MsTexStatsNow();
        const std::string aroot[2] = {adirMods.empty() ? std::string() : adirMods.back(), dirPack};
        size_t ac[2] = {};
        for (int isrc = 0; isrc < 2; ++isrc)
        {
            if (aroot[isrc].empty()) continue;
            std::error_code ec;
            // (the mod folders: the highest priority first, the first of a name kept; the cache knows the source by the
            // last folder's Textures path, each PNG by its name, size and time)
            std::vector<std::string> adir;
            if (isrc == 0) adir.assign(adirMods.rbegin(), adirMods.rend());
            else adir.push_back(aroot[isrc]);
            m_adir[isrc] = cane::utf8(cane::path(aroot[isrc]) / "Textures");
            bool fAny = false;
            for (const std::string& root : adir)
            {
                const fs::path dir = cane::path(root) / "Textures";
                if (!fs::is_directory(dir, ec)) continue;
                fAny = true;
                for (const auto& e : fs::directory_iterator(dir, ec))
                    if (e.is_regular_file(ec) && e.path().extension() == ".png" &&
                        m_map.emplace(cane::utf8(e.path().filename()), MODSRC{cane::utf8(e.path()), isrc}).second)
                        ++ac[isrc];
            }
            if (!fAny) { m_adir[isrc].clear(); continue; }
            if (ac[isrc] && !fQuiet) std::printf("mods: %zu textures%s\n", ac[isrc], isrc ? " (texture pack)" : "");
        }
        if (FTexStats()) TexStatsAdd(TEXSTATK_Scan, MsTexStatsNow() - t0, int(m_map.size()));
        m_fSync = fSync;
        // (the cache of a folder that is gone, e.g. a texture pack removed in the launcher: deleted)
        if (!dirCache.empty() && cane::texcache::RemoveOrphans(cane::path(dirCache)) && !fQuiet)
            std::printf("mods: texture cache: the cache of a removed folder deleted\n");
        if (dirCache.empty() || m_map.empty()) return;
        // each folder's pack: the entries of PNGs that are gone dropped (a name the mods folder now has is no longer
        // the pack's)
        const double t1 = MsTexStatsNow();
        for (int isrc = 0; isrc < 2; ++isrc)
        {
            if (!ac[isrc]) continue;
            auto ppack = std::make_unique<cane::texcache::Pack>();
            if (!ppack->Open(cane::path(dirCache), m_adir[isrc]))
            {
                if (!fQuiet) std::fprintf(stderr, "mods: the texture cache in %s cannot be used\n", dirCache.c_str());
                continue;
            }
            const size_t cdropped = ppack->Prune([&](const std::string& name) {
                const auto it = m_map.find(name);
                return it != m_map.end() && it->second.isrc == isrc;
            });
            if (!fQuiet)
                std::printf("mods: texture cache: %zu of %zu%s ready (%.1f MiB)%s\n", ppack->Centry(), ac[isrc], isrc ? " (texture pack)" : "",
                            double(ppack->CbPack()) / 1048576.0, cdropped ? ", removed textures dropped" : "");
            m_appack[isrc] = std::move(ppack);
        }
        if (FTexStats()) TexStatsAdd(TEXSTATK_CacheOpen, MsTexStatsNow() - t1);
    }
    [[nodiscard]] bool FEmpty() const { return m_map.empty(); }
    [[nodiscard]] const std::unordered_map<std::string, MODSRC>& Map() const { return m_map; }
    [[nodiscard]] const MODSRC* Find(const std::string& name) const
    {
        const auto it = m_map.find(name);
        return it == m_map.end() ? nullptr : &it->second;
    }
    [[nodiscard]] cane::texcache::Pack* Ppack(int isrc) const { return m_appack[isrc].get(); }
    // The replacement's texels, from the cache when it has this PNG (by name, size and time), else from the PNG, which
    // is then added to the cache (by the store thread, or here: fSync, no threads). Any thread.
    // *pfCached: it came from the cache.
    bool Load(const std::string& name, const MODSRC& src, std::vector<uint8_t>& out, int* pw, int* ph, bool* pfCached = nullptr)
    {
        cane::texcache::Pack* const ppack = m_appack[src.isrc].get();
        cane::texcache::Stamp stamp;
        const bool fStamp = ppack && cane::texcache::StampOf(cane::path(src.path), stamp);
        if (pfCached) *pfCached = false;
        if (fStamp)
        {
            const double t0 = FTexStats() ? MsTexStatsNow() : 0.0;
            if (ppack->Load(name, stamp, out, pw, ph))
            {
                if (FTexStats()) TexStatsAdd(TEXSTATK_CacheRead, MsTexStatsNow() - t0);
                if (pfCached) *pfCached = true;
                return true;
            }
        }
        if (!FDecodeModPng(src.path, out, pw, ph)) return false;
        if (!fStamp) return true;
        auto store = [ppack, name, stamp, w = *pw, h = *ph](const std::vector<uint8_t>& ab) {
            if (ppack->FHas(name, stamp)) return;   // (loaded twice before its record was written)
            const double t0 = FTexStats() ? MsTexStatsNow() : 0.0;
            ppack->Store(name, stamp, ab.data(), w, h);
            if (FTexStats()) TexStatsAdd(TEXSTATK_CacheStore, MsTexStatsNow() - t0);
        };
        CWorkPool* const ppool = m_fSync ? nullptr : PpoolStore();
        if (!ppool) store(out);
        else if (s_cbStorePending.load() + out.size() <= (uint64_t(64) << 20))
        {
            // (a copy for the store thread; with too many waiting this one is left for the next time it is loaded)
            s_cbStorePending += out.size();
            ppool->Push([store, ab = out] {
                if (!s_fQuit.load()) store(ab);
                s_cbStorePending -= ab.size();
            });
        }
        return true;
    }
    void Flush()
    {
        for (auto& ppack : m_appack)
            if (ppack) ppack->Flush();
    }

private:
    std::unordered_map<std::string, MODSRC> m_map;
    std::string m_adir[2];
    std::unique_ptr<cane::texcache::Pack> m_appack[2];
    bool m_fSync = false;
};

std::string s_dirPack, s_dirCache;
int s_fCacheSetting = 1;
// The cache is on unless Settings.ini's [Graphics] Texture Cache is Off; SLY2N_TEXCACHE=0 / 1 decides for a run.
bool FTextureCacheOn()
{
    if (const char* sz = std::getenv("SLY2N_TEXCACHE")) return std::atoi(sz) != 0;
    return s_fCacheSetting != 0;
}
std::string DirTextureCache()
{
    if (!s_dirCache.empty()) return s_dirCache;
    return cane::utf8(cane::cacheDirectory(kCacheGame) / "Textures");
}

// The game's replacement textures: the folders are listed by a thread started at the game's start (StartTextureMods),
// so the listing (thousands of entries; slow on an SD card) runs beside the boot; the first use waits for it.
struct MODSTATE
{
    CModStore store;
    std::thread thread;
    std::once_flag once;
    std::atomic<bool> fStarted{false}, fUsed{false};
};
MODSTATE& ModState()
{
    static MODSTATE* const s_p = new MODSTATE;   // (never destroyed: the worker threads use it until the process ends)
    return *s_p;
}
void QuitTexMods();
void InitModState()
{
    MODSTATE& st = ModState();
    st.store.Init(g_disc.AdirModRoots(), s_dirPack, FTextureCacheOn() ? DirTextureCache() : std::string(), false);
}
void RegisterQuitTexMods()
{
    static std::once_flag s_once;
    std::call_once(s_once, [] { std::atexit(QuitTexMods); });
}
CModStore& Mods()
{
    MODSTATE& st = ModState();
    std::call_once(st.once, [&st] {
        RegisterQuitTexMods();
        if (st.fStarted.load()) st.thread.join();
        else InitModState();
        st.fUsed.store(true);
    });
    return st.store;
}
}  // namespace

void SetTexturePackDirectory(const std::string& dir) { s_dirPack = dir; }
void SetTextureCacheDirectory(const std::string& dir) { s_dirCache = dir; }
void SetTextureCacheEnabled(bool f) { s_fCacheSetting = f ? 1 : 0; }
void StartTextureMods()
{
    MODSTATE& st = ModState();
    if (CTexThreads() <= 0 || st.fStarted.exchange(true)) return;
    RegisterQuitTexMods();
    try { st.thread = std::thread(InitModState); }
    catch (const std::exception&) { st.fStarted.store(false); }
}

std::string SzTextureModName(const std::vector<uint8_t>& rgba, int dx, int dy)
{
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%dx%d_%016llx.png", dx, dy, static_cast<unsigned long long>(HashRgba(rgba)));
    return sz;
}

bool FLoadTextureMod(const std::vector<uint8_t>& rgba, int dx, int dy, std::vector<uint8_t>& out, int* pw, int* ph)
{
    CModStore& mods = Mods();
    if (mods.FEmpty()) return false;
    const bool fStats = FTexStats();
    const double t0 = fStats ? MsTexStatsNow() : 0.0;
    const std::string name = SzTextureModName(rgba, dx, dy);
    const MODSRC* psrc = mods.Find(name);
    if (fStats) TexStatsAdd(TEXSTATK_Hash, MsTexStatsNow() - t0);
    return psrc && mods.Load(name, *psrc, out, pw, ph);
}

namespace
{
std::unordered_map<unsigned, double> s_settexMod;   // a texture mod's GL texture: its GPU bytes (with mips)
double s_cbMods = 0;

// The replacement's texels as a GL texture with its mip chain, or 0 (over the GPU's limit)
unsigned GlTexFromModTexels(const std::vector<uint8_t>& ab, int w, int h, int dx, int dy)
{
    GLint cMax = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &cMax);
    if (cMax > 0 && (w > cMax || h > cMax))
    {
        std::fprintf(stderr, "mods: a replacement of a %dx%d texture is %dx%d, over this GPU's limit (%d): not used\n", dx, dy, w, h, int(cMax));
        return 0;
    }
    // the full mip chain (sampled trilinear by render.cpp's mipmapped samplers, so a large replacement does not
    // shimmer at a distance); level 0 is the PNG as is
    int clevel = 1;
    for (int d = std::max(w, h); d > 1; d >>= 1) ++clevel;
    GLuint tex = 0;
    const cane::draw::Packets* pk = PpacketsTex();
    if (!pk)
    {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    const bool fStats = FTexStats();
    if (fStats) glFinish();
    double t0 = fStats ? MsTexStatsNow() : 0.0;
    if (pk)
    {
        // (native: glTexStorage2D's levels with level 0 uploaded, GL's default parameters, no binding left)
        cane::draw::TextureDesc d;
        d.width = uint32_t(w), d.height = uint32_t(h), d.levels = uint32_t(clevel), d.pixels = ab.data();
        tex = pk->createTexture(d);
    }
    else
    {
        glTexStorage2D(GL_TEXTURE_2D, clevel, GL_RGBA8, w, h);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
    }
    if (fStats) { glFinish(); const double t = MsTexStatsNow(); TexStatsAdd(TEXSTATK_Upload, t - t0); t0 = t; }
    if (clevel > 1) { if (pk) pk->generateMipmaps(tex); else glGenerateMipmap(GL_TEXTURE_2D); }
    if (fStats) { glFinish(); TexStatsAdd(TEXSTATK_Mips, MsTexStatsNow() - t0); }
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
}  // namespace

unsigned GlTexFromMod(const std::vector<uint8_t>& rgba, int dx, int dy)
{
    std::vector<uint8_t> ab;
    int w = 0, h = 0;
    if (!FLoadTextureMod(rgba, dx, dy, ab, &w, &h)) return 0;
    return GlTexFromModTexels(ab, w, h, dx, dy);
}

namespace
{
// (at the process's end: the records still waiting are left for the next run, the index written)
void QuitTexMods()
{
    s_fQuit.store(true);
    CModStore& mods = Mods();   // (the listing's thread ended)
    if (CWorkPool* p = PpoolStore()) p->Wait();
    mods.Flush();
}
}  // namespace

void FlushTextureCache()
{
    // (a world change: the cache's index written behind the records the old world added)
    if (ModState().fUsed.load())
        if (CWorkPool* p = PpoolStore()) p->Push([] { ModState().store.Flush(); });
}

// ---- SLY2N_TEXSTATS (texmod.h)
namespace
{
struct TEXSTATS
{
    double ams[TEXSTATK_Max] = {}, amsMax[TEXSTATK_Max] = {};
    int ac[TEXSTATK_Max] = {};
    int cframe = 0, cframeTex = 0, ctexStock = 0, ctexMod = 0;
    double msTexFrame = 0;   // the texture work of the frame being drawn
    int ctexFrame = 0;
    double msFrameLoad = 0;  // the first frame (the world's load)
    double msFrameMax = 0;   // the longest frame after it
    struct FRAME { int iframe; double ms, msTex; int ctex; };
    std::vector<FRAME> aworst;   // the frames with the most texture work, most first (the load's frame left out)
    int acOver[3] = {};          // frames after the load with more than 2 / 8 / 16 ms of texture work
    int iworld = 0;
};
TEXSTATS s_ts;
void PrintTexStats()
{
    const TEXSTATS& t = s_ts;
    if (!t.cframe && !t.ctexStock && !t.ctexMod && !t.ac[TEXSTATK_Scan]) return;
    static const char* const s_asz[TEXSTATK_Max] = {"scan", "read", "decode", "flip", "upload", "mips", "stock decode", "hash", "stock upload",
                                                    "cache open", "cache read", "cache store"};
    std::printf("texstats: world %d: %d frames, %d stock textures, %d replaced\n", t.iworld, t.cframe, t.ctexStock, t.ctexMod);
    for (int k = 0; k < TEXSTATK_Max; ++k)
        if (t.ac[k]) std::printf("texstats:   %-12s %9.2f ms total, %7.3f ms worst (%d)\n", s_asz[k], t.ams[k], t.amsMax[k], t.ac[k]);
    std::printf("texstats:   load frame %.1f ms; after it: longest frame %.1f ms, %d frames made textures, over 2 / 8 / 16 ms of texture work: %d / %d / %d\n",
                t.msFrameLoad, t.msFrameMax, t.cframeTex, t.acOver[0], t.acOver[1], t.acOver[2]);
    for (const TEXSTATS::FRAME& f : t.aworst)
        std::printf("texstats:   frame %d: %.2f ms, %.2f ms of it %d textures\n", f.iframe, f.ms, f.msTex, f.ctex);
    std::fflush(stdout);
}
}  // namespace

bool FTexStats()
{
    static const bool s_f = [] {
        const bool f = std::getenv("SLY2N_TEXSTATS") != nullptr;
        if (f) std::atexit(PrintTexStats);
        return f;
    }();
    return s_f;
}
double MsTexStatsNow() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void TexStatsAdd(TEXSTATK k, double ms, int c)
{
    static std::mutex s_mtx;   // (the worker threads' times too)
    std::lock_guard<std::mutex> lock(s_mtx);
    s_ts.ams[k] += ms;
    s_ts.amsMax[k] = std::max(s_ts.amsMax[k], ms);
    s_ts.ac[k] += c;
}
void TexStatsTexture(double ms, bool fMod)
{
    ++(fMod ? s_ts.ctexMod : s_ts.ctexStock);
    s_ts.msTexFrame += ms;
    ++s_ts.ctexFrame;
}
void TexStatsFrame(double ms)
{
    if (!FTexStats()) return;
    TEXSTATS& t = s_ts;
    if (t.cframe == 0) t.msFrameLoad = ms;
    else
    {
        t.msFrameMax = std::max(t.msFrameMax, ms);
        if (t.ctexFrame)
        {
            ++t.cframeTex;
            for (int i = 0; i < 3; ++i)
                if (t.msTexFrame > (i == 0 ? 2.0 : i == 1 ? 8.0 : 16.0)) ++t.acOver[i];
            t.aworst.push_back({t.cframe, ms, t.msTexFrame, t.ctexFrame});
            std::sort(t.aworst.begin(), t.aworst.end(), [](const TEXSTATS::FRAME& a, const TEXSTATS::FRAME& b) { return a.msTex > b.msTex; });
            if (t.aworst.size() > 8) t.aworst.pop_back();
        }
    }
    ++t.cframe;
    t.msTexFrame = 0;
    t.ctexFrame = 0;
}
void TexStatsWorld()
{
    if (!FTexStats()) return;
    PrintTexStats();
    const int iworld = s_ts.iworld + 1;
    s_ts = TEXSTATS{};
    s_ts.iworld = iworld;
}

bool FTextureIsMod(unsigned tex) { return tex != 0 && s_settexMod.count(tex) != 0; }
void ForgetModTexture(unsigned tex)
{
    const auto it = s_settexMod.find(tex);
    if (it == s_settexMod.end()) return;
    s_cbMods -= it->second;
    s_settexMod.erase(it);
}

// The loaded world's textures (the renderer's tables) into dir: decoded here, each PNG written by the pool (or here
// without one). setName: the names already written, by this world or an earlier one. Returns the PNGs queued.
int DumpLoadedWorldTextures(const std::string& dir, std::set<std::string>& setName, CWorkPool* ppool)
{
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
                std::string name = SzTextureModName(rgba, dx, dy);
                if (!setName.insert(name).second) continue;  // (the same texels under another CLUT / bitmap, or world)
                auto write = [dx, dy, rgba = std::move(rgba), path = cane::utf8(cane::path(dir) / name)] {
                    std::vector<uint8_t> ab(rgba.size());
                    for (int y = 0; y < dy; ++y)
                        for (int x = 0; x < dx; ++x)
                            for (int k = 0; k < 4; ++k)
                            {
                                const uint8_t v = rgba[(size_t(y) * dx + x) * 4 + k];
                                ab[(size_t(dy - 1 - y) * dx + x) * 4 + k] = k < 3 ? v : uint8_t(std::min(255, v * 2));
                            }
                    if (!stbi_write_png(path.c_str(), dx, dy, 4, ab.data(), dx * 4)) std::fprintf(stderr, "textures: cannot write %s\n", path.c_str());
                };
                if (ppool) ppool->Push(std::move(write), uint64_t(dx) * dy * 8);
                else write();
                ++c;
            }
    }
    return c;
}

namespace
{
// One world's textures into dir
int DumpWorldTextures(const std::string& dir, const FK& fk, int filter, const std::string& world, std::set<std::string>& setName, CWorkPool* ppool)
{
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) { std::fprintf(stderr, "world %s did not load\n", world.c_str()); return -1; }
    return DumpLoadedWorldTextures(dir, setName, ppool);
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
    const int c = DumpWorldTextures(dir, fk, filter, world, setName, nullptr);
    if (c < 0) return 1;
    std::printf("%d textures of world %s written to %s\n", c, world.c_str(), dir.c_str());
    return 0;
}

int ExportAllTextures(const std::string& dir, bool fProgress)
{
    InitClasses();
    std::error_code ec;
    fs::create_directories(cane::path(dir), ec);
    // the worlds: the boot world (its logo screens), then the records (from the boot world) that have a file (the boot
    // world loaded once for its records; each world once)
    std::vector<std::pair<std::string, std::pair<FK, int>>> aworld;
    std::string err;
    FK fk;
    int filter = 0;
    if (FResolveWorld("boot", fk, filter, err)) aworld.push_back({"boot", {fk, filter}});
    {
        auto pswBoot = std::make_unique<SW>();
        std::size_t cbRead = 0, cbFile = 0;
        if (!aworld.empty() && FLoadWorldFile(pswBoot.get(), aworld[0].second.first, "", &cbRead, &cbFile))
            for (std::size_t iwrec = 0; iwrec < pswBoot->awrec.size(); ++iwrec)
                if (const FK& fkRec = pswBoot->awrec[iwrec].fk; fkRec.cb != 0 && fkRec.id != 0) aworld.push_back({std::to_string(iwrec), {fkRec, int(iwrec)}});
    }
    std::set<std::string> setName;
    int c = 0;
    CWorkPool pool;
    for (size_t i = 0; i < aworld.size(); ++i)
    {
        const int cw = DumpWorldTextures(dir, aworld[i].second.first, aworld[i].second.second, aworld[i].first, setName, &pool);
        if (cw > 0) c += cw;
        if (fProgress)
        {
            std::printf("textures %zu %zu %s\n", i + 1, aworld.size(), aworld[i].first.c_str());
            std::fflush(stdout);
        }
    }
    pool.Wait();
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

// ---- The texture cache's commands (texmod.h)
int ClearTextureCacheCommand()
{
    const fs::path dir = cane::path(DirTextureCache());
    std::error_code ec;
    size_t c = 0;
    uint64_t cb = 0;
    if (fs::is_directory(dir, ec))
    {
        std::vector<fs::path> apath;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            const std::string name = cane::utf8(e.path().filename());
            auto ends = [&name](const char* sz) { const size_t n = std::strlen(sz); return name.size() > n && name.compare(name.size() - n, n, sz) == 0; };
            if (e.is_regular_file(ec) && (ends(".texpack") || ends(".texindex") || ends(".texindex.tmp"))) apath.push_back(e.path());
        }
        for (const fs::path& p : apath)
        {
            const auto cbFile = fs::file_size(p, ec);
            if (fs::remove(p, ec)) { ++c; cb += ec ? 0 : uint64_t(cbFile); }
        }
        fs::remove(dir, ec);   // (when nothing else is in it)
    }
    std::printf("texture cache: %zu files removed (%.1f MiB) from %s\n", c, double(cb) / 1048576.0, cane::utf8(dir).c_str());
    return 0;
}

int BuildTextureCacheCommand()
{
    if (!FTextureCacheOn()) { std::printf("texture cache: turned off (Settings.ini [Graphics] Texture Cache, SLY2N_TEXCACHE)\n"); return 1; }
    const double t0 = MsTexStatsNow();
    CModStore store;
    store.Init(g_disc.AdirModRoots(), s_dirPack, DirTextureCache(), true);
    const size_t ctotal = store.Map().size();
    std::atomic<size_t> cdone{0}, cadded{0}, cfailed{0};
    std::mutex mtx;
    {
        CWorkPool pool;
        for (const auto& kv : store.Map())
            pool.Push([&, pname = &kv.first, psrc = &kv.second] {
                cane::texcache::Pack* const ppack = store.Ppack(psrc->isrc);
                cane::texcache::Stamp stamp;
                if (ppack && cane::texcache::StampOf(cane::path(psrc->path), stamp) && !ppack->FHas(*pname, stamp))
                {
                    std::vector<uint8_t> ab;
                    int w = 0, h = 0;
                    if (store.Load(*pname, *psrc, ab, &w, &h) && ppack->FHas(*pname, stamp)) ++cadded;
                    else ++cfailed;
                }
                const size_t c = ++cdone;
                if (c % 64 == 0 || c == ctotal)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    std::printf("texcache-progress %zu %zu\n", c, ctotal);
                    std::fflush(stdout);
                }
            });
        pool.Wait();
    }
    store.Flush();
    uint64_t cb = 0;
    for (int isrc = 0; isrc < 2; ++isrc)
        if (store.Ppack(isrc)) cb += store.Ppack(isrc)->CbPack();
    std::printf("texture cache: %zu textures, %zu added, %zu not cached, %.1f MiB in %s, %.1f s\n", ctotal, cadded.load(), cfailed.load(),
                double(cb) / 1048576.0, DirTextureCache().c_str(), (MsTexStatsNow() - t0) / 1000.0);
    return cfailed.load() ? 1 : 0;
}

// --texcache-selftest DIR: the cache's rules on made-up PNGs under DIR (its mods, pack and cache folders are made new)
int TextureCacheSelfTest(const std::string& dirIn)
{
    const fs::path root = cane::path(dirIn);
    const fs::path dirMods = root / "mods", dirPack = root / "pack", dirCache = root / "cache";
    std::error_code ec;
    for (const fs::path& p : {dirMods, dirPack, dirCache}) fs::remove_all(p, ec);
    fs::create_directories(dirMods / "Textures", ec);
    fs::create_directories(dirPack / "Textures", ec);
    int ccheck = 0, cfail = 0;
    auto check = [&](bool f, const char* what) {
        ++ccheck;
        if (!f) { ++cfail; std::printf("texcache selftest: FAILED: %s\n", what); }
    };
    // a PNG of made-up texels (its alpha over the whole range); its file's time moved on by `age` seconds, so a
    // rewritten file never keeps its stamp
    int cwritten = 0;
    auto writePng = [&](const fs::path& p, int w, int h, uint32_t seed) {
        std::vector<uint8_t> ab(size_t(w) * h * 4);
        for (uint8_t& b : ab) { seed = seed * 1664525u + 1013904223u; b = uint8_t(seed >> 24); }
        stbi_write_png(cane::utf8(p).c_str(), w, h, 4, ab.data(), w * 4);
        std::error_code ec2;
        fs::last_write_time(p, fs::last_write_time(p, ec2) + std::chrono::seconds(10 * ++cwritten), ec2);
    };
    const std::string A = "8x8_00000000000000a1.png", B = "8x8_00000000000000b2.png", C = "16x4_00000000000000c3.png", D = "4x4_00000000000000d4.png";
    writePng(dirMods / "Textures" / A, 32, 32, 1);
    writePng(dirPack / "Textures" / A, 16, 16, 2);
    writePng(dirPack / "Textures" / B, 64, 32, 3);
    writePng(dirPack / "Textures" / C, 128, 16, 4);
    struct RESULT { bool fFound = false, fLoaded = false, fCached = false, fRight = false; int isrc = -1; };
    // one start of the game: the folders listed, the cache opened, the named textures loaded and compared with their PNG
    // decoded directly
    auto run = [&](const std::vector<std::string>& aname, bool fCache, size_t* pacentry = nullptr, fs::path* papath = nullptr) {
        std::map<std::string, RESULT> mp;
        CModStore store;
        store.Init({cane::utf8(dirMods)}, cane::utf8(dirPack), fCache ? cane::utf8(dirCache) : std::string(), true, true);
        for (const std::string& name : aname)
        {
            RESULT r;
            if (const MODSRC* psrc = store.Find(name))
            {
                r.fFound = true;
                r.isrc = psrc->isrc;
                std::vector<uint8_t> ab, abPng;
                int w = 0, h = 0, wPng = 0, hPng = 0;
                r.fLoaded = store.Load(name, *psrc, ab, &w, &h, &r.fCached);
                r.fRight = r.fLoaded && FDecodeModPng(psrc->path, abPng, &wPng, &hPng) && w == wPng && h == hPng && ab == abPng;
            }
            mp[name] = r;
        }
        store.Flush();
        for (int isrc = 0; isrc < 2; ++isrc)
        {
            if (pacentry) pacentry[isrc] = store.Ppack(isrc) ? store.Ppack(isrc)->Centry() : 0;
            if (papath && store.Ppack(isrc)) papath[isrc] = store.Ppack(isrc)->PathPack(), papath[isrc + 2] = store.Ppack(isrc)->PathIndex();
        }
        return mp;
    };
    size_t ac[2] = {};
    fs::path apath[4];
    // 1. the first start: everything from the PNGs, and into the cache; the mods folder's A, not the pack's
    auto r = run({A, B, C}, true, ac, apath);
    check(r[A].fRight && r[B].fRight && r[C].fRight, "first start: the PNGs' texels");
    check(!r[A].fCached && !r[B].fCached && !r[C].fCached, "first start: nothing cached yet");
    check(r[A].isrc == 0 && r[B].isrc == 1, "the mods folder's texture wins over the pack's");
    check(ac[0] == 1 && ac[1] == 2, "first start: one record each");
    // 2. the second start: everything from the cache, the same texels
    r = run({A, B, C}, true, ac);
    check(r[A].fRight && r[B].fRight && r[C].fRight, "second start: the same texels");
    check(r[A].fCached && r[B].fCached && r[C].fCached, "second start: all from the cache");
    // 3. a mod added: only it is made
    writePng(dirMods / "Textures" / D, 8, 8, 5);
    r = run({A, B, C, D}, true, ac);
    check(r[D].fRight && !r[D].fCached && r[A].fCached && r[B].fCached && r[C].fCached, "a new PNG: only it is made");
    check(ac[0] == 2 && ac[1] == 2, "a new PNG: one more record");
    // 4. a PNG changed (another picture and size under the same name): made again, then cached
    writePng(dirPack / "Textures" / B, 32, 32, 6);
    r = run({A, B, C, D}, true);
    check(r[B].fRight && !r[B].fCached && r[C].fCached, "a changed PNG: made again");
    r = run({B}, true, ac);
    check(r[B].fRight && r[B].fCached && ac[1] == 2, "a changed PNG: cached after");
    // 5. a PNG removed: not used, its record dropped
    fs::remove(dirPack / "Textures" / C, ec);
    r = run({A, B, C, D}, true, ac);
    check(!r[C].fFound && ac[1] == 1 && r[B].fCached, "a removed PNG: dropped");
    // 6. the pack file cut short: found, the lost records made again, never a wrong texel
    writePng(dirPack / "Textures" / C, 128, 16, 7);
    r = run({B, C}, true, ac);
    check(r[C].fRight && !r[C].fCached, "a PNG back: made");
    const auto cbPack = fs::file_size(apath[1], ec);
    fs::resize_file(apath[1], cbPack - 9, ec);
    r = run({A, B, C, D}, true, ac);
    check(r[A].fRight && r[B].fRight && r[C].fRight && r[D].fRight, "a cut pack file: the right texels");
    check(r[B].fCached && !r[C].fCached && ac[1] == 2, "a cut pack file: the cut record made again");
    r = run({B, C}, true);
    check(r[B].fCached && r[C].fCached && r[C].fRight, "a cut pack file: whole again after");
    // 7. bytes of the pack changed (a payload, then everything): found by the sums
    auto poke = [&](const fs::path& p, uint64_t ib, size_t cb) {
        std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
        std::vector<char> ab(cb);
        f.seekg(std::streamoff(ib));
        f.read(ab.data(), std::streamsize(cb));
        for (char& b : ab) b = char(b ^ 0x5A);
        f.seekp(std::streamoff(ib));
        f.write(ab.data(), std::streamsize(cb));
    };
    poke(apath[1], fs::file_size(apath[1], ec) - 40, 16);
    r = run({A, B, C, D}, true);
    check(r[A].fRight && r[B].fRight && r[C].fRight && r[D].fRight, "a damaged record: the right texels");
    r = run({B, C}, true);
    check(r[B].fCached && r[C].fCached && r[B].fRight && r[C].fRight, "a damaged record: made again");
    poke(apath[1], 0, size_t(fs::file_size(apath[1], ec)));
    r = run({A, B, C, D}, true);
    check(r[A].fRight && r[B].fRight && r[C].fRight && r[D].fRight && !r[B].fCached && r[A].fCached, "a pack file of garbage: started again");
    // 8. the index damaged, cut, missing: the pack is walked
    poke(apath[3], 20, 8);
    r = run({B, C}, true, ac);
    check(r[B].fCached && r[C].fCached && r[B].fRight && r[C].fRight && ac[1] == 2, "a damaged index: the records found in the pack");
    fs::resize_file(apath[3], 11, ec);
    r = run({B, C}, true);
    check(r[B].fCached && r[C].fCached && r[B].fRight, "a cut index");
    fs::remove(apath[3], ec);
    r = run({B, C}, true);
    check(r[B].fCached && r[C].fCached && r[C].fRight, "no index");
    // 9. the mods folder's texture removed: the pack's of that name is used, from its own PNG
    fs::remove(dirMods / "Textures" / A, ec);
    r = run({A, D}, true, ac);
    check(r[A].fRight && r[A].isrc == 1 && !r[A].fCached && ac[0] == 1, "a mod removed: the pack's texture");
    // 10. the pack's folder removed: its cache goes at the next start, the mods folder's stays
    {
        const fs::path kept = dirPack;
        fs::path moved = dirPack;
        moved += ".away";
        fs::remove_all(moved, ec);
        fs::rename(kept, moved, ec);
        r = run({A, D}, true, ac);
        check(!r[A].fFound && r[D].fCached && !fs::exists(apath[1], ec) && !fs::exists(apath[3], ec) && fs::exists(apath[0], ec),
              "a removed folder: its cache deleted");
        fs::rename(moved, kept, ec);
    }
    // 11. the cache off: the PNGs, and nothing written
    fs::remove_all(dirCache, ec);
    r = run({A, B, C, D}, false);
    check(r[A].fRight && r[B].fRight && r[C].fRight && r[D].fRight && !r[B].fCached && !fs::exists(dirCache, ec), "the cache off");
    std::printf("texcache selftest: %d checks, %d failed\n", ccheck, cfail);
    return cfail ? 1 : 0;
}
