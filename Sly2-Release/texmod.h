#pragma once

// Texture mods (MODDING.md): Mods/Textures/<W>x<H>_<hash>.png replaces the texture whose
// decoded RGBA hashes to <hash> (FNV-1a 64), wherever the renderer's texture cache uploads it; --dump-textures and
// --export-textures write them. A replacement is used at its own resolution: the game's UVs are normalized, so a 4x PNG
// maps exactly like the original and shows 4x the detail. It gets a full mip chain (trilinear, anisotropic where the
// original is bilinear; point-sampled textures stay point-sampled when magnified).

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// The texture's mod name: "<W>x<H>_<16 hex digits>.png".
std::string SzTextureModName(const std::vector<uint8_t>& rgba, int dx, int dy);
// --texture-pack DIR: a texture pack (DIR/Textures.pak, else DIR/Textures; the same names) under the mods folder's own
// textures, which win. The launcher passes the PS3 HD pack this way while it is turned on (ps3hd.cpp).
void SetTexturePackDirectory(const std::string& dir);
// The texture cache (platform/texture_cache.h, notes/TEXCACHE_2026-11-10.md): each replacement's texels, ready to
// upload, kept in a pack file per folder under the user's cache folder (--cache-dir DIR: there), so a PNG is read and
// decoded once; a new or changed PNG (by name, size and time) is added the first time it is used, by a worker thread,
// and a removed one is dropped. On unless Settings.ini's [Graphics] Texture Cache is Off (SLY2N_TEXCACHE=0 / 1 for a
// run). The picture is the same with and without it: the cache holds the PNG's level 0, the mips are made as before.
void SetTextureCacheDirectory(const std::string& dir);
void SetTextureCacheEnabled(bool f);
// Lists the folders on a thread, beside the game's start (the first use waits for it). Without it: at the first use.
void StartTextureMods();
// --clear-texture-cache: the cache's files removed. --build-texture-cache: every replacement not cached yet added
// ("texcache-progress DONE TOTAL" lines on stdout). --texcache-selftest DIR: the cache's rules on made-up PNGs in DIR.
int ClearTextureCacheCommand();
int BuildTextureCacheCommand();
int TextureCacheSelfTest(const std::string& dir);
// The cache's index written (by its thread) when the renderer drops a world's textures (render.cpp ResetTextureCache).
void FlushTextureCache();
// The mods folder's PNG for this decoded texture as the game's texels (rows bottom-up, alpha in the PS2 range), at
// the PNG's own size (*pw x *ph); false: none, or no mods.
bool FLoadTextureMod(const std::vector<uint8_t>& rgba, int dx, int dy, std::vector<uint8_t>& out, int* pw, int* ph);
// A GL texture made from the mods folder's PNG for this decoded texture (with its mip chain), or 0 (none, or no mods).
unsigned GlTexFromMod(const std::vector<uint8_t>& rgba, int dx, int dy);
// True for a texture GlTexFromMod made (the renderer then samples it with the mipmapped samplers).
bool FTextureIsMod(unsigned tex);
// The renderer deleted it (render.cpp ResetTextureCache).
void ForgetModTexture(unsigned tex);
// SLY2N_TEXSTATS=1 (notes/TEXCACHE_2026-11-10.md): where the textures' time goes. Each world's summary is printed when
// the next one loads and at the end: the folder scan, each replacement's read / decode / flip / upload / mips, the
// stock textures' decode / hash / upload, and the frames in which textures were made (the worst ones by the texture
// work in them). With it on the GPU is drained around each upload (glFinish), so its times are the uploads' own.
enum TEXSTATK { TEXSTATK_Scan, TEXSTATK_Read, TEXSTATK_Decode, TEXSTATK_Flip, TEXSTATK_Upload, TEXSTATK_Mips, TEXSTATK_StockDecode,
                TEXSTATK_Hash, TEXSTATK_StockUpload, TEXSTATK_CacheOpen, TEXSTATK_CacheRead, TEXSTATK_CacheStore, TEXSTATK_Wait, TEXSTATK_Max };
bool FTexStats();
double MsTexStatsNow();
void TexStatsAdd(TEXSTATK k, double ms, int c = 1);
// A texture was made in ms (its whole lookup, render.cpp TexFromSel): counted into the frame's texture work.
void TexStatsTexture(double ms, bool fMod);
// Streamed replacements were uploaded in ms (render.cpp TakeStreamedTextureMods): into the frame's texture work.
void TexStatsStreamed(double ms);
// The frame ended (game.cpp), after ms.
void TexStatsFrame(double ms);
// The texture cache was reset (a world load): prints the summary so far and starts the next.
void TexStatsWorld();
// --dump-textures DIR W: every texture of world W (a record index, boot, frontend) as a PNG named for the mods folder.
int DumpTexturesCommand(const std::string& dir, const std::string& world);
// Every world's textures (the boot world and every world record with a file) into DIR, a texture several worlds draw
// written once. fProgress: "textures I N WORLD" lines on stdout (cane-extractor, the launcher). Returns the count
// written, or -1.
int ExportAllTextures(const std::string& dir, bool fProgress);
// The world just loaded's textures (the renderer's tables) into dir, each PNG written by the pool (null: at once);
// setName: the names written so far (a texture several worlds draw is written once). The converter calls it after
// each world of its crawl (convert.cpp), so no world is loaded twice. Returns the PNGs written or queued.
class CWorkPool;
int DumpLoadedWorldTextures(const std::string& dir, std::set<std::string>& setName, CWorkPool* ppool);
// --export-textures without --convert: every world's textures into DIR (main.cpp: --out, else <installation>/Textures), written into
// DIR.partial and renamed when complete (an existing DIR is replaced).
int ExportTexturesCommand(const std::string& dir);

// The texture mods streamed: a replacement's load (from the texture cache, else its PNG's read and decode) on the other
// cores, as one took long enough on a slow disk or CPU to stall the frame that first drew its texture; the original
// texture is drawn until it is ready. RequestTextureMod with the decoded texture and its cache key (nothing without
// texture mods); TakeTextureMods gives the replacements ready since, their GL textures made (at least one, then up to
// about cbBudget bytes of texels); CancelTextureMods at a world's start; WaitTextureMods waits up to ms for every
// request made to be ready (true: all are): the renderer does in the frames right after a world's load, so a world
// starts with its replacements where the workers are fast enough.
// On when the game draws in a window and Settings.ini's [Graphics] Texture Streaming is On (the default but on
// Android: platform/sly2_settings.h); never in a headless run, whose frames stay those of a load at first draw.
// SLY2N_TEXSTREAM=0 / 1 decides for a run.
struct TEXMODREADY
{
    const void* key[2];
    unsigned tex;
};
void SetTextureModStreamingEnabled(bool f);   // (the setting)
void AllowTextureModStreaming(bool f);        // (game.cpp: the run draws in a window)
bool FTextureModStreaming();
void RequestTextureMod(const std::vector<uint8_t>& rgba, int dx, int dy, const void* key0, const void* key1);
void TakeTextureMods(std::vector<TEXMODREADY>& aready, size_t cbBudget);
bool WaitTextureMods(double ms);
void CancelTextureMods();
