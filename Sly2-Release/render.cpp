// See render.h.

#include "fastpaths.h"
#include "texmod.h"
#include "render.h"
#include "simdmat.h"
#include "drawdist.h"

#include "display.h"
#include "cm.h"
#include "emitter.h"
#include "gl.h"
#include "vr.h"
#include "light.h"
#include "laserrender.h"
#include "pocket.h"
void CollectHackSwarmDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra);  // (hack.cpp: the swarms' particles)
#include "rtgt.h"
#include "renderer.h"
#include "wr.h"
#include "passhelpers.h"
#include "gl_backend.h"

#include <stb/stb_image_write.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>

RENDERSTATS g_renderstats;
constinit PFNEXTRADRAWS g_pfnExtraDraws = nullptr;

struct GLGLOB
{
    GLuint vao = 0;             // the geometry arena's (one per BuildSceneGpu: all its globs' vertices and indices)
    GLuint vbo = 0;             // the arena's vertex buffer (moving geometry: UpdateGlobVerticesGpu)
    GLuint vboColor = 0;        // the arena's lit colours (rgbaA, rgbaB per vertex: attributes 2 / 3; UpdateGlobColorsGpu)
    int32_t baseVertex = 0;     // where this glob's vertices / indices start in the arena
    uint32_t firstIndex = 0;
    GLuint ssboEdges = 0;
    GLuint ssboEdgeSkin = 0;   // per edge vertex: bone weights and indices (skinned cel borders)
    glm::vec3 posCenter{0.0f};  // the geometry's bounding sphere (for globs without a draw record)
    float sRadius = -1.0f;
    bool fBoundsExact = false;  // (perf-4) the sphere holds the uploaded vertices (SetBounds; not once they move)
    std::vector<glm::vec4> asphereSub;   // per GLOB::asubglob entry its triangles' sphere (centre, radius; -1: none), while fBoundsExact
    GLsizei cedge = 0;
    GLsizei cindex = 0;
    // its vertices' lowest colour-A alpha as last uploaded: with the texture's lowest, a bound on each fragment's alpha
    // (AllPassOf)
    mutable uint8_t aMinA = 0;
    // ... and the highest (255: not known): with the texture's highest, the alpha tests no fragment passes (AllPassOf)
    mutable uint8_t aMaxA = 255;
};

// A SUBGLOB's commands, cached: per pass sub-draw its state and order.
struct PASSCMD { uint32_t istate; uint32_t order; };
struct SUBGLOBGPU
{
    const SUBGLOB* powner = nullptr;
    const SHD* pshd = nullptr;
    int iframe = -1;
    GLuint vao = 0;
    uint8_t fCull = 0;
    std::vector<PASSCMD> apasscmd;
    SUBGLOBGPU* pnextVao = nullptr;   // the same SUBGLOB drawn from another arena (an instance's own vertices)
    uint32_t genTex = 0;              // the texture cache's generation its states were made in (s_genTexCache)
};

// (vr.cpp, the eyes drawn straight into Vulkan OpenXR images: their clip space's Y flipped, so the triangles' winding is
// too: the counter-clockwise front of every GL_CCW in this file becomes clockwise while the eyes draw)
bool s_fFlipFaces = false;
// (VR) the eyes' runs without the near-clip geometry stage: Android (every renderer) and Vulkan on a tile GPU (the Steam
// Frame): a geometry stage costs a tiler dearly (half the frame on the Quest's Adreno)
bool FVrNoNearClip()
{
#if defined(__ANDROID__)
    return true;
#else
    return FVulkanTileGpu();
#endif
}
GLenum FrontCCW() { return s_fFlipFaces ? GL_CW : GL_CCW; }

namespace
{
void InitCommonOnce();
struct GPUVTX
{
    float pos[3];
    float uv[2];
    uint8_t rgbaA[4];
    uint8_t rgbaB[4];
    float normal[3];
    uint8_t abone[4];   // skinning (animation code): the group's bone indices (VTXGLOB::abone)
    uint8_t aw[4];      // and the raw weights (/255, P_G 726)
};
static_assert(sizeof(GPUVTX) == 48);
// (the colours' own buffer: GPUVTX's rgbaA / rgbaB are not read from the vertex buffer)
struct GPUCOL
{
    uint8_t rgbaA[4];
    uint8_t rgbaB[4];
};
static_assert(sizeof(GPUCOL) == 8);
inline GPUCOL ColOfGpuVtx(const GPUVTX& v)
{
    GPUCOL c;
    std::memcpy(c.rgbaA, v.rgbaA, 4);
    std::memcpy(c.rgbaB, v.rgbaB, 4);
    return c;
}

std::vector<std::unique_ptr<GLGLOB>> s_aglglob;
std::map<std::pair<const BMP*, const CLUT*>, GLuint> s_mptex;
// (bumped when the texture cache is reset: a SUBGLOB's cached states name its GL textures, which are deleted then. Only
// the PC guests' kept-world globs (pcloader.h) outlive a world load with their cache: they are drawn again in the next
// world and must not keep the deleted names, which the new world's textures reuse: R5x, the guests' wrong textures
// after a death's reload)
uint32_t s_genTexCache = 1;
// (the texture mods streamed, texmod.h) Per GL texture the entries whose cached states name it, rebuilt alone when its
// replacement comes in; the replaced original textures, deleted a frame later (that frame's 2D quads may name them);
// the scene frames since the texture cache was reset (a world's load)
std::unordered_map<GLuint, std::unordered_set<SUBGLOBGPU*>> s_mptexapsub;
SUBGLOBGPU* s_psubEmitting = nullptr;
std::vector<GLuint> s_atexRetired;
int s_cframeTexCache = 0;
GLuint s_texWhite = 0;
GLuint s_asampler[16] = {};  // bit 0 clamp S, 1 clamp T, 2 nearest, 3 mipmapped (a texture mod's own mip chain: texmod.h)
// The mipmapped samplers' anisotropy (texture mods only; SLY2N_MODANISO=N, 1: off): clamped to the GPU's maximum
const float s_rModAniso = std::getenv("SLY2N_MODANISO") ? float(std::atof(std::getenv("SLY2N_MODANISO"))) : 8.0f;
// The sampler for a texture: the mipmapped one for a texture mod
inline uint8_t SamplerFor(GLuint tex, uint8_t sampler) { return uint8_t(sampler | (FTextureIsMod(tex) ? 8 : 0)); }
GLuint s_vaoEmpty = 0;
// Geometry arenas: the globs built by one BuildSceneGpu share one VBO / EBO / VAO, so a run of draws with one GL
// state is one glMultiDrawElementsIndirect. Attribute 6 is the draw index (an iota buffer, divisor 1, selected by
// the command's baseInstance) into the per-frame draw data (glob.vert DRAWS / RECS).
std::vector<struct GPUVTX> s_avtxArena;
std::vector<uint32_t> s_aindexArena;
std::vector<GLGLOB*> s_apglArena;
GLuint s_vboDrawIndex = 0;
constexpr uint32_t CDRAW_MAX = 1u << 18;

// (the old world's textures: deleted at the next lookup, where GL is current)
std::vector<GLuint> s_atexFreed;

// Per GL texture of TexFromSel its lowest texel alpha (0: not known, a texture mod's): a filtered sample, a weighted
// mean of texels, is never below it. Per pass state AllPassOf's answer, cleared when a texture name changes meaning.
std::unordered_map<GLuint, uint8_t> s_mptexMinA;
// ... and its highest (none: not known; vkalpha): a filtered sample is never above it
std::unordered_map<GLuint, uint8_t> s_mptexMaxA;
struct ALLPASS
{
    float vNeed = std::numeric_limits<float>::infinity();   // the lowest vertex alpha x object alpha that passes always
    uint32_t istateAlt = UINT32_MAX;                         // the draw's state then, without the test (none: not drawn)
    // (vkalpha) the highest vertex alpha x object alpha at which the test fails on every fragment (-1: none), and the
    // draw's state then: an AFAIL sub-draw (keeps the failing fragments) without the test; the passing one draws nothing
    float vFail = -1.0f;
    uint32_t istateAltFail = UINT32_MAX;
    bool fKnown = false;
};
std::vector<ALLPASS> s_aallpass;
void NoteTexMinAlpha(GLuint tex, uint8_t aMin, int aMax = -1)
{
    s_mptexMinA[tex] = aMin;
    if (aMax >= 0) s_mptexMaxA[tex] = uint8_t(aMax);
    else s_mptexMaxA.erase(tex);
    s_aallpass.clear();
}

// (perf-5, g_fGlobArrays: GL / GLES without bindless textures) the pass draws' textures copied into layers of 2D array
// textures, one array per size ("pages" of several layers): a run of draws then binds one array and each draw picks its
// layer (glob.vert TEXARRAY, SSBO 5), so neighbouring commands that differ only in their textures (of one size) join one
// run, as the bindless runs do. The 2D textures stay (every other user, and the states without a layer: texture mods,
// sizes not known); a layer is a GPU copy of one (glCopyImageSubData), sampled with the same sampler object, so the same
// texels. Freed with the texture cache (ResetTextureCache, a world change).
struct TEXPAGE { GLuint tex = 0; int dx = 0, dy = 0, clayer = 0, cused = 0; };
std::vector<TEXPAGE> s_atexpage;
std::unordered_map<GLuint, uint32_t> s_mptexlayer;              // texture -> page << 16 | layer
std::unordered_map<GLuint, std::pair<int, int>> s_mptexsize;    // TexFromSel's own textures (not mods): their size
uint32_t s_genArray = 1;
constexpr uint32_t LAYERREF_NONE = UINT32_MAX;
uint32_t LayerRefOfTex(GLuint tex)
{
    if (const auto it = s_mptexlayer.find(tex); it != s_mptexlayer.end()) return it->second;
    const auto itSize = s_mptexsize.find(tex);
    if (itSize == s_mptexsize.end()) return s_mptexlayer[tex] = LAYERREF_NONE;
    const auto [dx, dy] = itSize->second;
    size_t ipage = 0;
    while (ipage < s_atexpage.size() && !(s_atexpage[ipage].dx == dx && s_atexpage[ipage].dy == dy && s_atexpage[ipage].cused < s_atexpage[ipage].clayer))
        ++ipage;
    if (ipage == s_atexpage.size())
    {
        if (ipage >= 0xFFFF) return s_mptexlayer[tex] = LAYERREF_NONE;
        static const GLint s_clayerMax = [] { GLint c = 256; glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &c); return std::max<GLint>(c, 1); }();
        // (about 4 MiB a page: small textures share a page by the dozens, a 512 x 512 one has 4 neighbours)
        TEXPAGE page;
        page.dx = dx, page.dy = dy;
        page.clayer = int(std::clamp<size_t>((size_t(4) << 20) / (size_t(dx) * size_t(dy) * 4), 1, size_t(std::min(s_clayerMax, 256))));
        glGenTextures(1, &page.tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, page.tex);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, dx, dy, page.clayer);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        s_atexpage.push_back(page);
    }
    TEXPAGE& page = s_atexpage[ipage];
    const int layer = page.cused++;
    // (a copy needs a complete source: its single level, whatever the texture's own filter says; the draws sample it
    // through sampler objects, which do not read these)
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glCopyImageSubData(tex, GL_TEXTURE_2D, 0, 0, 0, 0, page.tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, dx, dy, 1);
    return s_mptexlayer[tex] = uint32_t(ipage) << 16 | uint32_t(layer);
}
// (perf-3, g_fGlobBindless) the resident handles of (texture, sampler) pairs the pass draws use; a deleted texture's are
// made non-resident first and forgotten (its name may come back), and the per-state cache is invalidated
std::unordered_map<uint64_t, GLuint64> s_mphandle;
uint32_t s_genHandle = 1;
GLuint64 TexHandle(GLuint tex, uint8_t sampler)
{
    const uint64_t key = (uint64_t(tex) << 8) | (sampler & 15u);
    auto [it, fNew] = s_mphandle.try_emplace(key, 0);
    if (fNew)
    {
        it->second = glGetTextureSamplerHandleARB(tex, s_asampler[sampler & 15u]);
        if (it->second) glMakeTextureHandleResidentARB(it->second);
    }
    return it->second;
}
void ForgetTexHandles(const std::vector<GLuint>& atex)
{
    if (s_mphandle.empty()) return;
    for (GLuint tex : atex)
        for (uint64_t i = 0; i < 16; ++i)
        {
            const auto it = s_mphandle.find((uint64_t(tex) << 8) | i);
            if (it == s_mphandle.end()) continue;
            if (it->second) glMakeTextureHandleNonResidentARB(it->second);
            s_mphandle.erase(it);
        }
    ++s_genHandle;
}

GLuint TexFromSel(const TEXSEL& sel)
{
    InitCommonOnce();
    if (!s_atexFreed.empty())
    {
        ForgetTexHandles(s_atexFreed);
        if (const cane::draw::Packets* pk = PpacketsTex()) pk->deleteTextures(uint32_t(s_atexFreed.size()), s_atexFreed.data());
        else glDeleteTextures(GLsizei(s_atexFreed.size()), s_atexFreed.data());
        s_atexFreed.clear();
    }
    if (!sel.pbmp) return s_texWhite;
    const std::pair<const BMP*, const CLUT*> key(sel.pbmp, sel.pclut);
    auto it = s_mptex.find(key);
    if (it != s_mptex.end()) return it->second;
    std::vector<uint8_t> rgba;
    std::string error;
    GLuint tex = s_texWhite;
    // (SLY2N_TEXSTATS: texmod.h)
    const bool fStats = FTexStats();
    const double tStats0 = fStats ? MsTexStatsNow() : 0.0;
    const bool fDecoded = FDecodeTexture(sel.pbmp, sel.pclut, rgba, error);
    if (fStats) TexStatsAdd(TEXSTATK_StockDecode, MsTexStatsNow() - tStats0);
    if (fDecoded)
    {
        // (a texture mod: Mods/Textures/<W>x<H>_<hash>.png; streamed: loaded on the other cores,
        // TakeStreamedTextureMods, the original texture until then)
        const bool fStreamed = FTextureModStreaming();
        if (fStreamed) RequestTextureMod(rgba, sel.pbmp->dx, sel.pbmp->dy, sel.pbmp, sel.pclut);
        if (const GLuint texMod = fStreamed ? 0 : GlTexFromMod(rgba, sel.pbmp->dx, sel.pbmp->dy))
        {
            s_mptex[key] = texMod;
            NoteTexMinAlpha(texMod, 0);   // (the replacement's own alpha: not known here)
            ++g_renderstats.ctexture;
            if (fStats) TexStatsTexture(MsTexStatsNow() - tStats0, true);
            return texMod;
        }
        if (fStats) glFinish();
        const double tUp0 = fStats ? MsTexStatsNow() : 0.0;
        if (const cane::draw::Packets* pk = PpacketsTex())
        {
            // (the GL calls' texture: glTexImage2D's one level, GL's default parameters; no binding left)
            cane::draw::TextureDesc d;
            d.width = uint32_t(sel.pbmp->dx), d.height = uint32_t(sel.pbmp->dy), d.storage = 0, d.pixels = rgba.data();
            tex = pk->createTexture(d);
        }
        else
        {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sel.pbmp->dx, sel.pbmp->dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        }
        if (fStats) { glFinish(); TexStatsAdd(TEXSTATK_StockUpload, MsTexStatsNow() - tUp0); }
        s_mptexsize[tex] = {sel.pbmp->dx, sel.pbmp->dy};   // (g_fGlobArrays: LayerRefOfTex)
        uint8_t aMin = 255, aMax = 0;
        for (size_t i = 3; i < rgba.size(); i += 4) aMin = std::min(aMin, rgba[i]), aMax = std::max(aMax, rgba[i]);
        NoteTexMinAlpha(tex, rgba.size() >= 4 ? aMin : 0, rgba.size() >= 4 ? int(aMax) : -1);
        ++g_renderstats.ctexture;
        if (fStats) TexStatsTexture(MsTexStatsNow() - tStats0, false);
    }
    else
    {
        ++g_renderstats.cfailTexture;
        static int s_cwarn = 0;
        if (s_cwarn++ < 10) std::fprintf(stderr, "texture 0x%X: %s\n", sel.pbmp->id, error.c_str());
    }
    s_mptex[key] = tex;
    return tex;
}

void InitCommon();
void InitCommonOnce() { InitCommon(); }
void InitCommon()
{
    if (s_texWhite) return;
    const uint32_t white = 0x80FFFFFFu;  // RGB 255, alpha 0x80 (1.0)
    if (const cane::draw::Packets* pk = PpacketsTex())
    {
        cane::draw::TextureDesc d;
        d.width = d.height = 1, d.storage = 0, d.pixels = &white;
        s_texWhite = pk->createTexture(d);
    }
    else
    {
        glGenTextures(1, &s_texWhite);
        glBindTexture(GL_TEXTURE_2D, s_texWhite);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    }
    NoteTexMinAlpha(s_texWhite, 0x80, 0x80);
    s_mptexsize[s_texWhite] = {1, 1};
    if (const cane::draw::Packets* pks = PpacketsSampler())
    {
        // (native samplers: the same sixteen as the GL calls below)
        using namespace cane::draw;
        for (int i = 0; i < 16; ++i)
        {
            SamplerDesc d;
            d.wrapS = (i & 1) ? kWrapClampToEdge : kWrapRepeat;
            d.wrapT = (i & 2) ? kWrapClampToEdge : kWrapRepeat;
            d.magFilter = (i & 4) ? kFilterNearest : kFilterLinear;
            d.minFilter = !(i & 8) ? d.magFilter : (i & 4) ? kFilterNearestMipNearest : kFilterLinearMipLinear;
            if ((i & 8) && !(i & 4)) d.anisotropy = s_rModAniso;   // (clamped to the device's maximum by the entry)
            s_asampler[i] = pks->createSampler(d);
        }
    }
    else
    {
    glGenSamplers(16, s_asampler);
    constexpr GLenum kMaxAniso = 0x84FF, kAniso = 0x84FE;  // GL_(MAX_)TEXTURE_MAX_ANISOTROPY (4.6, EXT before)
    GLfloat rAnisoMax = 0.0f;
    glGetFloatv(kMaxAniso, &rAnisoMax);
    while (glGetError() != GL_NO_ERROR) {}
    const float rAniso = std::min(s_rModAniso, rAnisoMax);
    for (int i = 0; i < 16; ++i)
    {
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_S, (i & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_T, (i & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        const GLint filter = (i & 4) ? GL_NEAREST : GL_LINEAR;
        // (mipmapped, for a texture mod: the original's filter between texels, trilinear / nearest-mip between sizes;
        // the anisotropy only where the original is bilinear, so point-sampled pixel art stays crisp)
        const GLint filterMin = !(i & 8) ? filter : (i & 4) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MIN_FILTER, filterMin);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MAG_FILTER, filter);
        if ((i & 8) && !(i & 4) && rAniso > 1.0f) glSamplerParameterf(s_asampler[i], kAniso, rAniso);
    }
    }
    if (const cane::draw::Packets* pkv = PpacketsVao()) s_vaoEmpty = pkv->createVertexArray(nullptr, 0, 0);
    else glGenVertexArrays(1, &s_vaoEmpty);
    std::vector<uint32_t> aiota(CDRAW_MAX);
    for (uint32_t i = 0; i < CDRAW_MAX; ++i) aiota[i] = i;
    s_vboDrawIndex = GlNewBuffer(GL_ARRAY_BUFFER, aiota.size() * 4, aiota.data(), GL_STATIC_DRAW);
}

template <class F> void SetBounds(GLGLOB* pgl, size_t c, F posOf)
{
    if (c == 0) return;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (size_t i = 0; i < c; ++i) { const glm::vec3 p = posOf(i); lo = glm::min(lo, p); hi = glm::max(hi, p); }
    pgl->posCenter = (lo + hi) * 0.5f;
    float r2 = 0;
    for (size_t i = 0; i < c; ++i) { const glm::vec3 d = posOf(i) - pgl->posCenter; r2 = std::max(r2, glm::dot(d, d)); }
    pgl->sRadius = std::sqrt(r2);
    pgl->fBoundsExact = true;
}

// Per entry of a strips glob the sphere of the vertices its triangles use (the uploaded positions, as SetBounds)
void SetSubBounds(GLGLOB* pgl, const GLOB& glob)
{
    pgl->asphereSub.assign(glob.asubglob.size(), glm::vec4(0.0f, 0.0f, 0.0f, -1.0f));
    for (size_t k = 0; k < glob.asubglob.size(); ++k)
    {
        const SUBGLOB& sub = glob.asubglob[k];
        if (sub.indexCount == 0 || size_t(sub.firstIndex) + sub.indexCount > glob.aindex.size()) continue;
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        bool fBad = false;
        for (uint32_t i = sub.firstIndex; i < sub.firstIndex + sub.indexCount && !fBad; ++i)
        {
            const uint32_t iv = glob.aindex[i];
            if (iv >= glob.avtx.size()) { fBad = true; break; }
            lo = glm::min(lo, glob.avtx[iv].pos);
            hi = glm::max(hi, glob.avtx[iv].pos);
        }
        if (fBad) continue;
        const glm::vec3 c = (lo + hi) * 0.5f;
        float r2 = 0.0f;
        for (uint32_t i = sub.firstIndex; i < sub.firstIndex + sub.indexCount; ++i)
        {
            const glm::vec3 d = glob.avtx[glob.aindex[i]].pos - c;
            r2 = std::max(r2, glm::dot(d, d));
        }
        pgl->asphereSub[k] = glm::vec4(c, std::sqrt(r2));
    }
}

void AppendArena(GLGLOB* pgl, const std::vector<GPUVTX>& av, const std::vector<uint32_t>& ai)
{
    SetBounds(pgl, av.size(), [&](size_t i) { return glm::vec3(av[i].pos[0], av[i].pos[1], av[i].pos[2]); });
    pgl->baseVertex = int32_t(s_avtxArena.size());
    pgl->firstIndex = uint32_t(s_aindexArena.size());
    uint8_t aMin = 255, aMax = 0;
    for (const GPUVTX& v : av) aMin = std::min(aMin, v.rgbaA[3]), aMax = std::max(aMax, v.rgbaA[3]);
    pgl->aMinA = av.empty() ? 0 : aMin;
    pgl->aMaxA = av.empty() ? 255 : aMax;
    s_avtxArena.insert(s_avtxArena.end(), av.begin(), av.end());
    s_aindexArena.insert(s_aindexArena.end(), ai.begin(), ai.end());
    s_apglArena.push_back(pgl);
}

// (g_fDrawUniform, OpenGL ES without base instance: glob.vert CANE_PULL) per arena (its VAO) the buffers the vertex
// program reads and a CPU copy of its indices, from which a run's sub-draws become one draw (BuildPullDraws)
// glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer), as a draw packet entry where the backend has it
void BindStorageWhole(GLuint binding, GLuint buffer)
{
    const cane::draw::Packets* pk = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    if (pk && pk->bindStorage) pk->bindStorage(binding, buffer, 0, 0);
    else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer);
}
// glBufferData(target, cbAlloc, nullptr, GL_STREAM_DRAW) + glBufferSubData(target, 0, cb, pv) on buffer (bound to target
// by the caller for the GL calls): through the backend's mapStream where it has one (pv copied into its ring memory)
void UploadStream(GLenum target, GLuint buffer, const void* pv, size_t cb, size_t cbAlloc)
{
    const cane::draw::Packets* pk = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    if (pk && pk->mapStream)
        if (void* p = pk->mapStream(buffer, cbAlloc))
        {
            if (cb) std::memcpy(p, pv, cb);
            return;
        }
    if (const cane::draw::Packets* pkb = PpacketsBuf())
    {
        // (no binding needed: the buffer by name)
        pkb->bufferData(buffer, cbAlloc, nullptr, uint8_t(cane::draw::kBufferStream | (target == GL_DRAW_INDIRECT_BUFFER ? cane::draw::kBufferIndirect : 0)));
        if (cb) pkb->bufferSubData(buffer, 0, cb, pv);
        return;
    }
    glBufferData(target, GLsizeiptr(cbAlloc), nullptr, GL_STREAM_DRAW);
    if (cb) glBufferSubData(target, 0, GLsizeiptr(cb), pv);
}
struct PULLARENA { GLuint vbo = 0, vboColor = 0; std::vector<uint32_t> aindex; };
std::unordered_map<GLuint, PULLARENA> s_mppullarena;
GLuint s_vaoPullBound = 0;   // (the arena whose buffers are on bindings 6 / 7)
GLuint s_vaoPullCur = 0;     // (CANE_PULL: the VAO bound by the pulled draws; 0: not known, bind again)
const PULLARENA* PpullArena(GLuint vao)
{
    const auto it = s_mppullarena.find(vao);
    if (it == s_mppullarena.end()) return nullptr;
    if (vao != s_vaoPullBound)
    {
        BindStorageWhole(6, it->second.vbo);
        BindStorageWhole(7, it->second.vboColor);
        s_vaoPullBound = vao;
    }
    return &it->second;
}

void FlushArena()
{
    if (s_apglArena.empty()) return;
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLuint vboColor = 0;
    if (const cane::draw::Packets* pkb = PpacketsBuf())
    {
        // (native buffers: the colours packed straight into the new buffer's memory, no binding touched)
        vbo = pkb->createBuffer(s_avtxArena.size() * sizeof(GPUVTX), s_avtxArena.data(), cane::draw::kBufferStatic);
        const size_t cbCol = s_avtxArena.size() * sizeof(GPUCOL);
        vboColor = pkb->createBuffer(cbCol, nullptr, cane::draw::kBufferDynamic);
        if (GPUCOL* acol = cbCol ? static_cast<GPUCOL*>(pkb->mapRange(vboColor, 0, cbCol)) : nullptr)
            for (size_t i = 0; i < s_avtxArena.size(); ++i) acol[i] = ColOfGpuVtx(s_avtxArena[i]);
        ebo = pkb->createBuffer(s_aindexArena.size() * 4, s_aindexArena.data(), cane::draw::kBufferStatic);
    }
    else
    {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(s_avtxArena.size() * sizeof(GPUVTX)), s_avtxArena.data(), GL_STATIC_DRAW);
    // (perf-3) the colours in a buffer of their own: the run-time lighting rewrites them (8 bytes a vertex, not the
    // whole vertex), a dynamic buffer (Mesa warns on, and may stall for, updates of a static one)
    {
        std::vector<GPUCOL> acol(s_avtxArena.size());
        for (size_t i = 0; i < acol.size(); ++i) acol[i] = ColOfGpuVtx(s_avtxArena[i]);
        glGenBuffers(1, &vboColor);
        glBindBuffer(GL_ARRAY_BUFFER, vboColor);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(acol.size() * sizeof(GPUCOL)), acol.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
    }
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(s_aindexArena.size() * 4), s_aindexArena.data(), GL_STATIC_DRAW);
    }
    if (const cane::draw::Packets* pkv = PpacketsVao())
    {
        // (native vertex arrays: the same attributes as the GL calls below, no binding touched)
        using cane::draw::VertexAttrib;
        const VertexAttrib aattr[] = {
            {0, 3, cane::draw::kAttribFloat, 0, 0, vbo, uint32_t(offsetof(GPUVTX, pos)), sizeof(GPUVTX), 0},
            {1, 2, cane::draw::kAttribFloat, 0, 0, vbo, uint32_t(offsetof(GPUVTX, uv)), sizeof(GPUVTX), 0},
            {4, 3, cane::draw::kAttribFloat, 0, 0, vbo, uint32_t(offsetof(GPUVTX, normal)), sizeof(GPUVTX), 0},
            {5, 4, cane::draw::kAttribUnsignedByte, 0, 1, vbo, uint32_t(offsetof(GPUVTX, abone)), sizeof(GPUVTX), 0},
            {7, 4, cane::draw::kAttribUnsignedByte, 1, 0, vbo, uint32_t(offsetof(GPUVTX, aw)), sizeof(GPUVTX), 0},
            {2, 4, cane::draw::kAttribUnsignedByte, 0, 0, vboColor, uint32_t(offsetof(GPUCOL, rgbaA)), sizeof(GPUCOL), 0},
            {3, 4, cane::draw::kAttribUnsignedByte, 0, 0, vboColor, uint32_t(offsetof(GPUCOL, rgbaB)), sizeof(GPUCOL), 0},
            {6, 1, cane::draw::kAttribUnsignedInt, 0, 1, s_vboDrawIndex, 0, 4, 1},
        };
        vao = pkv->createVertexArray(aattr, uint32_t(std::size(aattr)), ebo);
    }
    else
    {
    if (PpacketsBuf())
    {
        // (native buffers without native vertex arrays: the GL calls' bindings for the attributes)
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, uv)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, normal)));
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, abone)));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, aw)));
    glBindBuffer(GL_ARRAY_BUFFER, vboColor);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUCOL), reinterpret_cast<void*>(offsetof(GPUCOL, rgbaA)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUCOL), reinterpret_cast<void*>(offsetof(GPUCOL, rgbaB)));
    glBindBuffer(GL_ARRAY_BUFFER, s_vboDrawIndex);
    glEnableVertexAttribArray(6);
    glVertexAttribIPointer(6, 1, GL_UNSIGNED_INT, 4, nullptr);
    glVertexAttribDivisor(6, 1);
    glBindVertexArray(0);
    }
    for (GLGLOB* pgl : s_apglArena) { pgl->vao = vao; pgl->vbo = vbo; pgl->vboColor = vboColor; }
    if (g_fDrawUniform) s_mppullarena[vao] = PULLARENA{vbo, vboColor, s_aindexArena};
    s_apglArena.clear();
    std::vector<GPUVTX>().swap(s_avtxArena);
    std::vector<uint32_t>().swap(s_aindexArena);
}

// (each vertex built whole and stored once, in order: av may be a backend's write-combined memory)
void GpuVerticesInto(const GLOB& glob, GPUVTX* av)
{
    for (size_t i = 0; i < glob.avtx.size(); ++i)
    {
        const VTXGLOB& v = glob.avtx[i];
        GPUVTX g;
        std::memcpy(g.pos, &v.pos, 12);
        std::memcpy(g.uv, &v.uv, 8);
        std::memcpy(g.rgbaA, &v.rgbaA, 4);
        std::memcpy(g.rgbaB, &v.rgbaB, 4);
        std::memcpy(g.normal, &v.normal, 12);
        std::memcpy(g.abone, v.abone, 4);  // (skinning, animation code)
        std::memcpy(g.aw, v.aw, 4);
        av[i] = g;
    }
}
void GpuVerticesOfGlob(const GLOB& glob, std::vector<GPUVTX>& av)
{
    av.resize(glob.avtx.size());
    GpuVerticesInto(glob, av.data());
}
// glBufferSubData(offset, cb) on buffer as memory the caller fills in order (the backend's mapRange, draw_packet.h; the
// GL binding points left as they are); nullptr: the GL call
void* PvMapRange(GLuint buffer, size_t off, size_t cb)
{
    const cane::draw::Packets* pk = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    return pk && pk->mapRange && cb ? pk->mapRange(buffer, off, cb) : nullptr;
}

void BuildGlobGpu(GLOB& glob)
{
    if (glob.pgl) return;  // (a GLOBSET shared by several scene entries)
    if (glob.meshk == MESHK_Strips && !glob.avtx.empty() && !glob.aindex.empty())
    {
        auto pgl = std::make_unique<GLGLOB>();
        std::vector<GPUVTX> av;
        GpuVerticesOfGlob(glob, av);
        AppendArena(pgl.get(), av, glob.aindex);
        SetSubBounds(pgl.get(), glob);
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
    else if (glob.meshk == MESHK_Indexed && !glob.asubfx.empty())
    {
        // Type 3 (shadow hulls of rp 7 / 8): per strip the position dictionary indexed per strip vertex, triangles
        // where the strip mask's (ADC) bit is clear, winding alternating with the vertex index as in type 0.
        std::vector<GPUVTX> av;
        std::vector<uint32_t> ai;
        for (const SUBFX& fx : glob.asubfx)
        {
            const uint32_t base = uint32_t(av.size());
            for (size_t k = 0; k < fx.aipos.size(); ++k)
            {
                GPUVTX v{};
                v.rgbaA[3] = 128;
                const glm::vec3 p = fx.aipos[k] < fx.apos.size() ? fx.apos[fx.aipos[k]] : glm::vec3(0.0f);
                std::memcpy(v.pos, &p, 12);
                av.push_back(v);
                const bool fAdc = (fx.amask[k / 32] >> (k % 32)) & 1;
                if (k < 2 || fAdc) continue;
                const uint32_t i = base + uint32_t(k);
                if (k & 1) { ai.push_back(i - 1); ai.push_back(i - 2); }
                else { ai.push_back(i - 2); ai.push_back(i - 1); }
                ai.push_back(i);
            }
        }
        if (ai.empty()) return;
        auto pgl = std::make_unique<GLGLOB>();
        AppendArena(pgl.get(), av, ai);
        pgl->cindex = GLsizei(ai.size());
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
    else if (glob.meshk == MESHK_CelBorder && !glob.asubcel.empty())
    {
        std::vector<glm::vec4> aedge;
        struct EDGESKIN { glm::vec4 w; glm::uvec4 ab; };   // per edge vertex: weights and the group's bone indices
        std::vector<EDGESKIN> askin;
        for (const SUBCEL& cel : glob.asubcel)
            for (size_t i = 0; i + 3 < cel.aedge.size(); i += 4)
                for (int j = 0; j < 4; ++j)
                {
                    // The stream's records are {OA, E0, E1, OB} (Sly 1's TWEF order 1, 2, 0, 3): checked on Paris, the
                    // triangles (OA, E0, E1) and (E1, E0, OB) of every record share their winding.
                    static const int s_aj[4] = {1, 2, 0, 3};
                    const int jSrc = s_aj[j];
                    const uint8_t ipos = cel.aedge[i + size_t(jSrc)];
                    aedge.emplace_back(ipos < cel.apos.size() ? cel.apos[ipos] : glm::vec3(0.0f), 1.0f);
                    EDGESKIN sk{glm::vec4(0.0f), glm::uvec4(cel.aiboneSlot[0], cel.aiboneSlot[1], cel.aiboneSlot[2], cel.aiboneSlot[3])};
                    for (int k = 0; k < cel.cw && k < 4; ++k)
                        if (size_t(ipos) * size_t(cel.cw) + size_t(k) < cel.aw.size())
                            sk.w[k] = float(cel.aw[size_t(ipos) * size_t(cel.cw) + size_t(k)]) / 255.0f;
                    askin.push_back(sk);
                }
        if (aedge.empty()) return;
        auto pgl = std::make_unique<GLGLOB>();
        pgl->ssboEdges = GlNewBuffer(GL_SHADER_STORAGE_BUFFER, aedge.size() * sizeof(glm::vec4), aedge.data(), GL_STATIC_DRAW);
        pgl->ssboEdgeSkin = GlNewBuffer(GL_SHADER_STORAGE_BUFFER, askin.size() * sizeof(EDGESKIN), askin.data(), GL_STATIC_DRAW);
        pgl->cedge = GLsizei(aedge.size() / 4);
        SetBounds(pgl.get(), aedge.size(), [&](size_t i) { return glm::vec3(aedge[i]); });
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
}

// GS ALPHA -> GL blend: Cv = (A - B) * C + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}. Each colour's coefficient
// is k1 + kC * C; GL factors cover 0, 1, C and 1 - C (and -C via reverse subtract).
// (g_fBlend1, the Vulkan renderer on Android, for a glob program with CANE_BLEND1: the blends Cs x As + Cd x (1 - As)
// (1) and Cs x As + Cd (2) as one fixed blend, ONE / ONE_MINUS_SRC1_ALPHA, the fragment program premultiplying and its
// second output's alpha As or 0 (glob.frag): with no dynamic blend state each blend change would be a pipeline bind.
// 0: any other blend.)
int Blend1Mode(uint64_t alpha)
{
    const int a = int(alpha & 3), b = int((alpha >> 2) & 3), c = int((alpha >> 4) & 3), d = int((alpha >> 6) & 3);
    if (c != 0 || d != 1 || a != 0) return 0;   // (C = As, D = Cd, A = Cs)
    return b == 1 ? 1 : b == 2 ? 2 : 0;          // (B = Cd: alpha blending; B = 0: additive)
}
// The blend a GS ALPHA asks for as draw-packet enums (platform/draw_packet.h): FSetBlend sets it with GL calls, the
// draw packets take it as part of their state. False: not expressible (the alpha blend instead).
struct BLENDGL { uint8_t opColor, opAlpha, src, dst, srcA, dstA; bool fConst; float fix; };
bool FBlendOf(uint64_t alpha, bool fBlend1, BLENDGL& bl)
{
    using namespace cane::draw;
    if (fBlend1 && Blend1Mode(alpha) != 0)
    {
        bl = {kBlendAdd, kBlendAdd, kFactorOne, kFactorOneMinusSrc1Alpha, kFactorOne, kFactorZero, false, 0.0f};
        return true;
    }
    const int a = int(alpha & 3), b = int((alpha >> 2) & 3), c = int((alpha >> 4) & 3), d = int((alpha >> 6) & 3);
    const float fix = float((alpha >> 32) & 0xFF) / 128.0f;
    auto coef = [&](int sel, int& k1, int& kc) {
        k1 = (d == sel) ? 1 : 0;
        kc = (a == sel ? 1 : 0) - (b == sel ? 1 : 0);
    };
    int s1, sc, d1, dc;
    coef(0, s1, sc);
    coef(1, d1, dc);
    const uint8_t cf = c == 0 ? kFactorSrcAlpha : c == 1 ? kFactorDstAlpha : kFactorConstantAlpha;
    const uint8_t cfInv = c == 0 ? kFactorOneMinusSrcAlpha : c == 1 ? kFactorOneMinusDstAlpha : kFactorOneMinusConstantAlpha;
    bl.fConst = c == 2;
    bl.fix = fix;
    auto factor = [&](int k1, int kc, uint8_t& f) {
        if (k1 == 0 && kc == 0) f = kFactorZero;
        else if (k1 == 1 && kc == 0) f = kFactorOne;
        else if (k1 == 0 && kc == 1) f = cf;
        else if (k1 == 1 && kc == -1) f = cfInv;
        else return false;
        return true;
    };
    // The GS blends only the colour: the frame's alpha is the source alpha (the shadow pass, 0x16DA48 / 0x16DD58,
    // reads it: render.md "shadow frame-channel pass").
    bl.srcA = kFactorOne, bl.dstA = kFactorZero, bl.opAlpha = kBlendAdd;
    uint8_t fs, fd;
    if (sc == -1 && s1 == 0 && factor(d1, dc, fd))
    {
        bl.opColor = kBlendReverseSubtract, bl.src = cf, bl.dst = fd;
        return true;
    }
    bl.opColor = kBlendAdd;
    if (!factor(s1, sc, fs) || !factor(d1, dc, fd))
    {
        bl.src = kFactorSrcAlpha, bl.dst = kFactorOneMinusSrcAlpha;
        return false;
    }
    bl.src = fs, bl.dst = fd;
    return true;
}
GLenum GlFactorOf(uint8_t f)
{
    static const GLenum k[] = {GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
                               GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA};
    return k[f];
}
bool FSetBlend(uint64_t alpha, bool fBlend1 = false)
{
    BLENDGL bl;
    const bool fOk = FBlendOf(alpha, fBlend1, bl);
    if (bl.fConst) glBlendColor(0, 0, 0, bl.fix);
    auto eq = [](uint8_t op) { return op == cane::draw::kBlendReverseSubtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD; };
    if (bl.opColor == bl.opAlpha) glBlendEquation(eq(bl.opColor));
    else glBlendEquationSeparate(eq(bl.opColor), eq(bl.opAlpha));
    glBlendFuncSeparate(GlFactorOf(bl.src), GlFactorOf(bl.dst), GlFactorOf(bl.srcA), GlFactorOf(bl.dstA));
    return fOk;
}
// (draw packets, platform/draw_packet.h) FSetBlend's blend as a packet state's (blend on)
void PacketBlendOf(uint64_t alpha, cane::draw::State& ps)
{
    BLENDGL bl;
    FBlendOf(alpha, false, bl);
    ps.blend = 1;
    ps.srcColor = bl.src, ps.dstColor = bl.dst, ps.srcAlpha = bl.srcA, ps.dstAlpha = bl.dstA;
    ps.opColor = bl.opColor, ps.opAlpha = bl.opAlpha;
    ps.fBlendConstant = bl.fConst;
    ps.blendConstant[3] = bl.fix;
}
// The backend's packets for the render pass's special draws (null: the GL calls), and a special's packet state made once
// per key (its program and variant bits; build fills the State)
const cane::draw::Packets* PpacketsSpecial()
{
    const cane::draw::Packets* pk = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    return pk && pk->drawArrays && pk->applyState && pk->setConstant ? pk : nullptr;
}
// ... and with the shadow phases' entries (clears, copies, scissor, texture / storage bindings)
const cane::draw::Packets* PpacketsPhase()
{
    const cane::draw::Packets* pk = PpacketsSpecial();
    return pk && pk->clear && pk->copy && pk->scissor && pk->bindTexture && pk->bindStorage ? pk : nullptr;
}
template <class F> uint32_t PacketStateFor(const cane::draw::Packets& pk, uint64_t key, F&& build)
{
    static std::unordered_map<uint64_t, uint32_t> s_mpid;
    if (auto it = s_mpid.find(key); it != s_mpid.end()) return it->second;
    cane::draw::State ps;
    build(ps);
    return s_mpid[key] = pk.createState(ps);
}

struct DRAWREC
{
    int kind;
    float z;
    const GLOB* pglob;      // the GLOB (for its shaders / visibility)
    const GLOB* pglobGeom;  // where its geometry lives
    const glm::mat4* pmat;
    const std::vector<glm::mat4>* pamatBone = nullptr;  // skinned: the bone matrices (they replace pmat)
    float alpha = 1.0f;                                 // the object alpha (qw8.x; RIPs' fades)
    float alpha2 = 1.0f;                                // the cel borders' (qw8.y)
    const SCENEGLOBSET* psgs = nullptr;                 // the entry and the group (cast shadows)
    size_t iglob = 0;
    uint32_t iboneFirst = 0;                            // their place in the frame's bone SSBO
};

#if SLY2_FASTPATHS
// (the Switch) What RenderPass's command loop reads of the next records, prefetched in stages along their chains of
// pointers: the groups six records ahead, their entries four ahead, what those point to two ahead, the passes one.
void PrefetchRecordsAhead(const std::vector<DRAWREC>& arec, size_t irec)
{
    constexpr size_t CSUB = 4;   // (the entries looked at per group)
    const size_t crec = arec.size();
    if (irec + 6 < crec)
    {
        const DRAWREC& r = arec[irec + 6];
        __builtin_prefetch(r.pglob);
        __builtin_prefetch(r.pglobGeom);
        __builtin_prefetch(&r.pglobGeom->iSpec);
        __builtin_prefetch(&r.pglobGeom->asubglob);
        __builtin_prefetch(&r.pglobGeom->pgl);
        __builtin_prefetch(r.pmat);
        if (r.psgs) __builtin_prefetch(&r.psgs->apglInst);
    }
    if (irec + 4 < crec)
    {
        const DRAWREC& r = arec[irec + 4];
        const std::vector<SUBGLOB>& asub = r.pglobGeom->asubglob;
        for (size_t k = 0; k < asub.size() && k < CSUB; ++k) __builtin_prefetch(&asub[k]);
        if (r.psgs && r.iglob < r.psgs->apglInst.size()) __builtin_prefetch(&r.psgs->apglInst[r.iglob]);
        __builtin_prefetch(r.pglobGeom->pgl);
    }
    if (irec + 2 < crec)
    {
        const DRAWREC& r = arec[irec + 2];
        const std::vector<SUBGLOB>& asub = r.pglobGeom->asubglob;
        for (size_t k = 0; k < asub.size() && k < CSUB; ++k)
        {
            __builtin_prefetch(asub[k].pshd);
            __builtin_prefetch(asub[k].pgpu);
            if (asub[k].psaa) __builtin_prefetch(&asub[k].psaa->iframe);
        }
        if (r.psgs && r.iglob < r.psgs->apglInst.size()) __builtin_prefetch(r.psgs->apglInst[r.iglob]);
    }
    if (irec + 1 < crec)
    {
        const std::vector<SUBGLOB>& asub = arec[irec + 1].pglobGeom->asubglob;
        for (size_t k = 0; k < asub.size() && k < CSUB; ++k)
            if (const SUBGLOBGPU* p = asub[k].pgpu) __builtin_prefetch(p->apasscmd.data());
    }
}
#endif

struct UNIFORMS
{
    GLint matWorldToClip, icolor, rgbaConst, posCamera, dirCamera;
    GLint tex, fTextured, ate, atst, aref, afailMode, fogColor, fFogBlack, fDate, texDate;
    GLint texArr = -1;         // (g_fGlobArrays: glob.frag TEXARRAY's array, unit 2)
    GLint caneDrawBase = -1, caneVertexBase = -1;   // (g_fDrawUniform: glob.vert CANE_PULL's bases)
    GLint caneBlend1 = -1;     // (g_fBlend1: glob.frag CANE_BLEND1's blend)
};
// The glob programs: 0 glGlobShader, 1 glGlobShaderNoKill (no discards: the runs without alpha test and DATE); 2 / 3 the
// same without the near-clip geometry stage (perf-4: the runs whose draws cannot reach the near plane). Each has its own
// uniform locations and values; s_pu is the bound one's (RenderPass's useGlob).
constexpr int CPROGGLOB = 4;
UNIFORMS s_au[CPROGGLOB];
const UNIFORMS* s_pu = &s_au[0];
int s_iprogGlob = 0;
GLSHADER* const s_apshGlob[CPROGGLOB] = {&glGlobShader, &glGlobShaderNoKill, &glGlobShaderNoGeom, &glGlobShaderNoKillNoGeom};
// The glob uniforms outside DSTATE that a draw inherits from whatever set them last (rgbaConst: the volumes and hulls;
// fFogBlack: the passes): kept per program, brought to the last value set when it is bound (SyncGlobInherited).
struct GLOBINHERIT { glm::vec4 rgbaConst{0.0f}; int fFogBlack = 0; };
GLOBINHERIT s_aglobinh[CPROGGLOB], s_globinhLast;
void SetGlobRgbaConst(float r, float g, float b, float a)
{
    glUniform4f(s_pu->rgbaConst, r, g, b, a);
    s_aglobinh[s_iprogGlob].rgbaConst = s_globinhLast.rgbaConst = glm::vec4(r, g, b, a);
}
struct CELUNIFORMS { GLint matWorldToClip, matModel, halfWidthNdc, edgeBase, celRgba, cbone, iboneFirst; } s_uc;

glm::vec4 Vec4FromRgba(uint32_t rgba, float scale)
{
    return glm::vec4(float(rgba & 0xFF), float((rgba >> 8) & 0xFF), float((rgba >> 16) & 0xFF), float(rgba >> 24)) * scale;
}

// Per-frame draw data (glob.vert): per record its matrix and fog, per draw (record x entry) the SAA's ST offset,
// the object alpha and the record.
// bone: first matrix, count (skinned), the vertices' base in the arena; spec0 / spec1 / specPos: GLOB::iSpec's view-angle
// alpha (P_G 638): {g0, g1, g2, kind}, {clamp lo, hi, on, 0}, {posSpec, 0}; warp: the state vectors' first + 1 (0: no
// warp), the glob's vertex count, the first matrix, the matrix count | 0x100 positions | 0x200 coordinates (wr.h)
struct RECGPU { glm::mat4 model; glm::vec4 fog; glm::uvec4 bone{0u}; glm::vec4 spec0{0.0f}, spec1{0.0f}, specPos{0.0f}; glm::uvec4 warp{0u}; };
struct DRAWGPU { float du, dv, uAlpha; uint32_t irec; };

// The GL state a run of draws shares (one glMultiDrawElementsIndirect): everything DrawPass used to set per pass.
struct DSTATE
{
    GLuint vao = 0, tex = 0;
    uint64_t alpha = 0;         // GS ALPHA (fBlend)
    uint8_t sampler = 0, fTextured = 0, icolor = 0, fBlend = 0;
    uint8_t fCull = 0, depthFunc = 0, depthMask = 0, colorMask = 0;   // depthFunc 0 always 1 gequal 2 greater; colorMask 1 rgb 2 a
    uint8_t ate = 0, atst = 0, aref = 0, afailMode = 0;
    uint8_t fFogBlack = 0;      // FOGCOL 0 (0x1EB960: the additive passes of kinds 1 / 7)
    uint8_t fFba = 0;           // FBA 1 (kinds 4 / 6: 0x16E3C8's GS block 0x2DF700): the alpha's bit 7 (stencil STENCIL_FBA)
    uint8_t fNoStencil = 0;     // (vkalpha) before anything in the pass sets the stencil's FBA bit: no stencil test / write
    uint8_t pad[1] = {};
};
static_assert(sizeof(DSTATE) == 32);
bool operator==(const DSTATE& a, const DSTATE& b) { return std::memcmp(&a, &b, sizeof(DSTATE)) == 0; }

enum CMDK : uint8_t { CMDK_Pass, CMDK_Volume, CMDK_Shadow, CMDK_Cel, CMDK_Particles, CMDK_Shadow2, CMDK_ShadowBegin, CMDK_ShadowKind8, CMDK_ShadowCombine, CMDK_FbaClear };
// Interned states: a command refers to its state by index.
struct DSTATEHASH
{
    size_t operator()(const DSTATE& st) const
    {
        uint64_t h = 1469598103934665603ull;
        const uint8_t* pb = reinterpret_cast<const uint8_t*>(&st);
        for (size_t i = 0; i < sizeof(DSTATE); ++i) h = (h ^ pb[i]) * 1099511628211ull;
        return size_t(h);
    }
};
std::vector<DSTATE> s_astate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpstate;
uint32_t IstateOf(const DSTATE& st)
{
    auto it = s_mpstate.find(st);
    if (it != s_mpstate.end()) return it->second;
    const uint32_t i = uint32_t(s_astate.size());
    s_astate.push_back(st);
    s_mpstate.emplace(st, i);
    return i;
}

// (vkalpha) The state without the stencil: for the draws without FBA before the pass's first command that can set the
// stencil's FBA bit (an FBA draw, or a special other than the cel borders): the pass clears the stencil at its start, so
// their REPLACE with 0 changes nothing there. Without a stencil write a desktop GPU keeps its hierarchical depth / stencil
// fast paths (RADV: the main pass ~25 % cheaper with no FBA writes at all) and a tiler its low-resolution Z.
// SLY2N_STENCILHEAD=0: off.
const bool s_fStencilHead = !(std::getenv("SLY2N_STENCILHEAD") && std::atoi(std::getenv("SLY2N_STENCILHEAD")) == 0);
const bool s_fStencilDead = !(std::getenv("SLY2N_STENCILDEAD") && std::atoi(std::getenv("SLY2N_STENCILDEAD")) == 0);
const bool s_fStencilTail = !(std::getenv("SLY2N_STENCILTAIL") && std::atoi(std::getenv("SLY2N_STENCILTAIL")) == 0);
std::vector<uint32_t> s_aistateNoStencil;
uint32_t IstateNoStencil(uint32_t istate)
{
    if (istate >= s_aistateNoStencil.size()) s_aistateNoStencil.resize(istate + 1, UINT32_MAX);
    if (s_aistateNoStencil[istate] == UINT32_MAX)
    {
        DSTATE st = s_astate[istate];
        st.fNoStencil = 1;
        const uint32_t i = IstateOf(st);
        if (istate >= s_aistateNoStencil.size()) s_aistateNoStencil.resize(istate + 1, UINT32_MAX);
        s_aistateNoStencil[istate] = i;
    }
    return s_aistateNoStencil[istate];
}

// The state with FBA set (render kinds 4 / 6).
std::vector<uint32_t> s_aistateFba;
uint32_t IstateFba(uint32_t istate)
{
    if (istate >= s_aistateFba.size()) s_aistateFba.resize(istate + 1, UINT32_MAX);
    if (s_aistateFba[istate] == UINT32_MAX)
    {
        DSTATE st = s_astate[istate];
        st.fFba = 1;
        const uint32_t i = IstateOf(st);
        if (istate >= s_aistateFba.size()) s_aistateFba.resize(istate + 1, UINT32_MAX);
        s_aistateFba[istate] = i;
    }
    return s_aistateFba[istate];
}

// A pass state's alpha test decided by the draw's alpha bounds: the GS alpha floor(t x v / 128) (glob.frag) is at least
// tMin x vMin / 128, so a GEQUAL / GREATER test that bound reaches always passes (with half a unit for the filtering).
// (vkalpha) SLY2N_ALPHAFAILALL=0: the alpha tests no fragment passes drawn as before (notes/VKALPHA_2026-11-11.md)
const bool s_fAlphaFailAll = !(std::getenv("SLY2N_ALPHAFAILALL") && std::atoi(std::getenv("SLY2N_ALPHAFAILALL")) == 0);
ALLPASS AllPassOf(uint32_t istate)
{
    if (istate < s_aallpass.size() && s_aallpass[istate].fKnown) return s_aallpass[istate];
    ALLPASS ap;
    ap.fKnown = true;
    const DSTATE st = s_astate[istate];
    const int need = st.atst == 5 ? int(st.aref) : st.atst == 6 ? int(st.aref) + 1 : 256;
    if (st.ate && (st.afailMode == 1 || st.afailMode == 2) && st.icolor == 0 && need <= 255)
    {
        uint8_t tMin = 0x80;
        if (st.fTextured)
        {
            const auto it = s_mptexMinA.find(st.tex);
            tMin = it != s_mptexMinA.end() ? it->second : 0;
        }
        if (tMin > 0)
        {
            ap.vNeed = 128.0f * (float(need) + 0.5f) / float(tMin);
            if (st.afailMode == 1)
            {
                DSTATE stAlt = st;
                stAlt.ate = stAlt.atst = stAlt.aref = stAlt.afailMode = 0;
                ap.istateAlt = IstateOf(stAlt);
            }
        }
        // (vkalpha) no fragment passes when tMax x vMax / 128 stays half a unit below the reference: the passing
        // sub-draw writes nothing (not drawn), the failing one keeps every fragment (drawn without the test: no discards)
        int tMax = 0x80;
        if (st.fTextured)
        {
            const auto it = s_mptexMaxA.find(st.tex);
            tMax = it != s_mptexMaxA.end() ? it->second : 256;
        }
        if (need >= 1 && tMax <= 255 && s_fAlphaFailAll)
        {
            // (untextured: the alpha is the interpolated vertex alpha itself, not floored: GREATER fails only below aref)
            const int needFail = st.fTextured ? need : int(st.aref);
            ap.vFail = tMax == 0 ? std::numeric_limits<float>::infinity() : 128.0f * (float(needFail) - 0.5f) / float(tMax);
            if (st.afailMode == 2)
            {
                DSTATE stAlt = st;
                stAlt.ate = stAlt.atst = stAlt.aref = stAlt.afailMode = 0;
                ap.istateAltFail = IstateOf(stAlt);
            }
        }
    }
    if (istate >= s_aallpass.size()) s_aallpass.resize(size_t(istate) + 1);
    s_aallpass[istate] = ap;
    return ap;
}

std::deque<SUBGLOBGPU> s_asubgpu;

struct CMD
{
    uint32_t istate = 0;
    GLuint vao = 0;             // (specials)
    uint32_t order = 0;         // pass x 4 + its alpha-test sub-draw (the order within a record)
    uint32_t count = 0, firstIndex = 0;
    int32_t baseVertex = 0;
    uint32_t idraw = 0;
    CMDK cmdk = CMDK_Pass;
    uint8_t fNoClip = 0;        // (perf-4) its record's vertices all in front of the near plane: no glob.geom needed
    uint32_t irect = UINT32_MAX;   // (the sorted kinds' pass draws) its screen rectangles (SortJoinSegment); none: the frame
    uint8_t fAlphaKept = 0;     // (CMDK_ShadowBegin) nothing in the phase writes the frame's alpha (ShadowFetchMode)
    uint8_t fClipFixed = 0;     // (SLY2N_SORTJOIN) fNoClip is its run's in the order as recorded: runs join only draws of one choice
    const DRAWREC* prec = nullptr;
    const SUBGLOB* psub = nullptr;
};
struct DEICMD { uint32_t count, instanceCount, firstIndex; int32_t baseVertex; uint32_t baseInstance; };

GLuint s_ssboRec = 0, s_ssboDraw = 0, s_bufIndirect = 0, s_ssboBone = 0, s_ssboWarp = 0, s_ssboWarpMat = 0, s_ssboTexh = 0;

// (perf-3) The passes' per-frame draw data (records, draw entries, bones, warps, the indirect commands, the texture
// handles) written in place into one persistently mapped buffer (GL 4.4 glBufferStorage, coherent): no copy through the
// driver's staging memory and its thread. Three segments, one a frame (RenderScene), each reused after its fence (three
// frames on). A pass that does not fit the frame's segment uploads as before (orphan + sub-data), and the ring doubles
// at the next frame. Opt-in, SLY2N_PERSIST=1: on Mesa (radeonsi, threaded) it measured the same as the orphaned
// buffers, and some drivers place persistent coherent memory in system memory, which the vertex stage reads per vertex.
struct STREAMRING
{
    static constexpr int CSEG = 3;
    GLuint buf = 0;
    uint8_t* pb = nullptr;
    size_t cbSeg = 0, off = 0, offLim = 0, align = 256;
    int iseg = 0;
    GLsync async[CSEG] = {};
    bool fGrow = false, fDisabled = false;
    typedef void (*PFNBUFFERSTORAGE)(GLenum, GLsizeiptr, const void*, GLbitfield);
    PFNBUFFERSTORAGE pfnBufferStorage = nullptr;

    void Create(size_t cb)
    {
        constexpr GLbitfield grf = 0x0002 /*MAP_WRITE*/ | 0x0040 /*MAP_PERSISTENT*/ | 0x0080 /*MAP_COHERENT*/;
        glGenBuffers(1, &buf);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
        pfnBufferStorage(GL_COPY_WRITE_BUFFER, GLsizeiptr(cb * CSEG), nullptr, grf);
        pb = static_cast<uint8_t*>(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, GLsizeiptr(cb * CSEG), grf));
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        if (!pb)
        {
            glDeleteBuffers(1, &buf);
            buf = 0;
            fDisabled = true;
            return;
        }
        cbSeg = cb;
    }
    void Destroy()
    {
        for (GLsync& sync : async)
            if (sync) { glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); glDeleteSync(sync); sync = nullptr; }
        if (buf)
        {
            glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            glDeleteBuffers(1, &buf);
        }
        buf = 0;
        pb = nullptr;
    }
    // The start of a frame: the last segment fenced, the next one waited for.
    void BeginFrame()
    {
        if (fDisabled) return;
        if (!pfnBufferStorage)
        {
            static const bool s_fOff = std::getenv("SLY2N_PERSIST") == nullptr;
            pfnBufferStorage = reinterpret_cast<PFNBUFFERSTORAGE>(cane::gl::ProcAddress("glBufferStorage"));
#if defined(__linux__)
            // (a 4.3 context, Zink on ARM GPUs: Mesa returns an entry point even when the driver lacks the feature)
            if ((GLVersion.major < 4 || (GLVersion.major == 4 && GLVersion.minor < 4)) && !glfwExtensionSupported("GL_ARB_buffer_storage")) pfnBufferStorage = nullptr;
#endif
            if (s_fOff || !pfnBufferStorage) { fDisabled = true; return; }
            GLint a = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &a);
            align = std::max<size_t>(256, size_t(std::max(a, 1)));
        }
        if (buf)
        {
            if (async[iseg]) glDeleteSync(async[iseg]);
            async[iseg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        if (fGrow || !buf)
        {
            const size_t cb = buf ? cbSeg * 2 : size_t(4) << 20;
            Destroy();
            fGrow = false;
            Create(cb);
            if (!buf) return;
        }
        iseg = (iseg + 1) % CSEG;
        if (async[iseg])
        {
            // (polled first: a waiting call with the flush makes a threaded driver catch up first)
            if (glClientWaitSync(async[iseg], 0, 0) == GL_TIMEOUT_EXPIRED)
                glClientWaitSync(async[iseg], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(async[iseg]);
            async[iseg] = nullptr;
        }
        off = cbSeg * size_t(iseg);
        offLim = off + cbSeg;
    }
    // cb bytes (aligned) of this frame's segment: their offset, or SIZE_MAX when they do not fit
    size_t Alloc(size_t cb)
    {
        if (!pb) return SIZE_MAX;
        const size_t o = (off + align - 1) / align * align;
        if (o + cb > offLim) { fGrow = true; return SIZE_MAX; }
        off = o + cb;
        return o;
    }
};
STREAMRING s_ring;

// (g_fGlobArrays) a pass state's layer (LAYERREF_NONE: untextured, or drawn with its 2D texture bound) and its class: the
// states the same but for the texture (one page) are one class
struct LAYEROFSTATE { uint32_t gen = 0; uint32_t iclass = UINT32_MAX; uint32_t ref = LAYERREF_NONE; };
std::vector<LAYEROFSTATE> s_alayerIstate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpclassArray;
const LAYEROFSTATE& LayerEntryOfIstate(uint32_t istate)
{
    if (istate >= s_alayerIstate.size()) s_alayerIstate.resize(istate + 1);
    LAYEROFSTATE& e = s_alayerIstate[istate];
    if (e.gen != s_genArray)
    {
        DSTATE st = s_astate[istate];
        e.gen = s_genArray;
        e.ref = st.fTextured ? LayerRefOfTex(st.tex) : LAYERREF_NONE;
        st.tex = e.ref == LAYERREF_NONE ? st.tex : 0x80000000u | (e.ref >> 16);
        e.iclass = s_mpclassArray.try_emplace(st, uint32_t(s_mpclassArray.size())).first->second;
    }
    return e;
}
uint32_t LayerRefOfIstate(uint32_t istate) { return LayerEntryOfIstate(istate).ref; }
bool FSameButLayer(uint32_t istateA, uint32_t istateB)
{
    if (istateA == istateB) return true;
    LayerEntryOfIstate(istateA);
    LayerEntryOfIstate(istateB);   // (both made first: a lookup may grow the vector)
    const LAYEROFSTATE& a = s_alayerIstate[istateA];
    const LAYEROFSTATE& b = s_alayerIstate[istateB];
    return a.iclass == b.iclass && a.ref != LAYERREF_NONE && b.ref != LAYERREF_NONE;
}

// (g_fGlobBindless) a state's texture handle (0: untextured, or none made: then drawn with the texture bound) and its
// merge class (the state without its texture and sampler, interned: equal classes differ only in those)
struct HANDLEOFSTATE { uint32_t gen = 0; uint32_t iclass = UINT32_MAX; GLuint64 handle = 0; };
std::vector<HANDLEOFSTATE> s_ahandleIstate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpclass;
const HANDLEOFSTATE& HandleEntryOfIstate(uint32_t istate)
{
    if (istate >= s_ahandleIstate.size()) s_ahandleIstate.resize(istate + 1);
    HANDLEOFSTATE& e = s_ahandleIstate[istate];
    if (e.gen != s_genHandle)
    {
        DSTATE st = s_astate[istate];
        e.gen = s_genHandle;
        e.handle = st.fTextured ? TexHandle(st.tex, st.sampler) : 0;
        st.tex = 0;
        st.sampler = 0;
        e.iclass = s_mpclass.try_emplace(st, uint32_t(s_mpclass.size())).first->second;
    }
    return e;
}
GLuint64 HandleOfIstate(uint32_t istate) { return HandleEntryOfIstate(istate).handle; }
// Two pass states one run can draw with handles: the same but for the texture and sampler (both textured, with handles)
bool FSameButTexture(uint32_t istateA, uint32_t istateB)
{
    if (istateA == istateB) return true;
    HandleEntryOfIstate(istateA);
    HandleEntryOfIstate(istateB);   // (both made first: a lookup may grow the vector)
    const HANDLEOFSTATE& a = s_ahandleIstate[istateA];
    const HANDLEOFSTATE& b = s_ahandleIstate[istateB];
    return a.iclass == b.iclass && a.handle && b.handle;
}

// (the Vulkan renderer's pulled runs with bindless handles: glob.vert / glob.frag CANE_DRAWSTATE, the programs built
// with CANE_PULL and CANE_BLEND1) each pass draw's colour source, fog colour and (g_fBlend1) blend of the alpha /
// additive pair from its draw entry (the handle buffer's third word) in place of the uniforms, so draws in a row that
// differ only in those (and the texture) are one run: the GPU draws a run's triangles in order, as the separate draws,
// and each with its own values (Paris: a sorted object's alpha pass and its additive pass).
// SLY2N_VKDRAWSTATE=0: the uniforms as before (the words 0).
// The handle buffer's layout follows the programs: uvec4 (handle, word, 0) in those, uvec2 elsewhere.
bool FDrawStateLayout() { return g_fGlobBindless && g_fDrawUniform && FRendererVulkan(); }
bool FDrawStateOn()
{
    static const bool s_fOff = std::getenv("SLY2N_VKDRAWSTATE") && std::atoi(std::getenv("SLY2N_VKDRAWSTATE")) == 0;
    return FDrawStateLayout() && !s_fOff;
}
// the draw entry's word: bit 31 set (0: the uniforms, the other draws'), colour source, fog black, CANE_BLEND1 mode
// (0 without g_fBlend1: the blend is the run's own, as the uniform's 0 then)
uint32_t DrawStateWord(const DSTATE& st)
{
    return 0x80000000u | uint32_t(st.icolor & 3u) | (st.fFogBlack ? 4u : 0u) | (uint32_t(g_fBlend1 && st.fBlend ? Blend1Mode(st.alpha) : 0) << 3);
}
// a state's class for FSameDrawState: the state without its texture and sampler (textured: a handle needed), colour
// source, fog black, and with the pair's blends as one (one fixed blend, FSetBlend); UINT32_MAX: joins only itself
struct DRAWSTATECLASS { uint32_t gen = 0; uint32_t iclass = UINT32_MAX; };
std::vector<DRAWSTATECLASS> s_adsclassIstate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpdsclass;
uint32_t DrawStateClassOf(uint32_t istate)
{
    if (istate >= s_adsclassIstate.size()) s_adsclassIstate.resize(istate + 1);
    if (s_adsclassIstate[istate].gen != s_genHandle)
    {
        const GLuint64 handle = HandleOfIstate(istate);   // (may grow s_ahandleIstate, not this vector)
        DSTATE st = s_astate[istate];
        uint32_t iclass = UINT32_MAX;
        if (!st.fTextured || handle)
        {
            st.tex = 0;
            st.sampler = 0;
            st.icolor = 0;
            st.fFogBlack = 0;
            if (g_fBlend1 && st.fBlend && Blend1Mode(st.alpha) != 0) st.alpha = ~0ull;   // (one fixed blend: FSetBlend)
            iclass = s_mpdsclass.try_emplace(st, uint32_t(s_mpdsclass.size())).first->second;
        }
        s_adsclassIstate[istate] = {s_genHandle, iclass};
    }
    return s_adsclassIstate[istate].iclass;
}
bool FSameDrawState(uint32_t istateA, uint32_t istateB)
{
    if (istateA == istateB) return true;
    const uint32_t a = DrawStateClassOf(istateA), b = DrawStateClassOf(istateB);
    return a == b && a != UINT32_MAX;
}

// (SLY2N_GLSTATS=N: the GL state calls per frame, averaged over N frames: the draw path's costs on the driver)
struct GLSTATS { long capply, cfull, ctex, csampler, cuniform, cblend, crun, cpass, cshphase, cshkept; };
GLSTATS s_glstats{};
const int s_cframeGlStats = std::getenv("SLY2N_GLSTATS") ? std::atoi(std::getenv("SLY2N_GLSTATS")) : 0;

// (measurement only, vkalpha: SLY2N_LRZPROBE=1: the glob draws without the stencil's FBA writes (the shadows' DATE then
// wrong): whether a tiler's low-resolution Z was off for draws that write the stencil; notes/VKALPHA_2026-11-11.md)
const bool s_fLrzProbe = std::getenv("SLY2N_LRZPROBE") && std::atoi(std::getenv("SLY2N_LRZPROBE")) != 0;
// Applies a run's state, setting only what changed since the last run (fForce: everything).
struct STATECACHE
{
    DSTATE cur;
    bool fValid = false;
    // the uniforms each glob program was last given (a program keeps its own), and the bound program
    DSTATE aucur[CPROGGLOB];
    bool afuValid[CPROGGLOB] = {};
    int iprog = 0;
    // everything set outside Apply: the GL state and both programs' uniforms set again at the next run
    void Invalidate() { fValid = false; for (bool& f : afuValid) f = false; }
    // fBindTexture false: the run's draws take their textures from handles (g_fGlobBindless): the unit's binding is left
    // as it is (and forgotten: the next run that binds one binds it)
    void Apply(const DSTATE& st, bool fBindTexture = true)
    {
        const bool f = !fValid;
        const bool fu = !afuValid[iprog];   // (the bound program's uniforms)
        const DSTATE& ucur = aucur[iprog];
        if (s_cframeGlStats)
        {
            ++s_glstats.capply; s_glstats.cfull += f;
            
            s_glstats.ctex += fBindTexture && st.fTextured && (f || !cur.fTextured || st.tex != cur.tex || !fTexBound);
            s_glstats.csampler += fBindTexture && st.fTextured && (f || !cur.fTextured || st.sampler != cur.sampler || !fTexBound);
            s_glstats.cuniform += (fu || st.fTextured != ucur.fTextured) + (fu || st.icolor != ucur.icolor) + (fu || st.ate != ucur.ate) + (fu || st.atst != ucur.atst) +
                                  (fu || st.aref != ucur.aref) + (fu || st.afailMode != ucur.afailMode) + (fu || st.fFogBlack != ucur.fFogBlack);
            s_glstats.cblend += st.fBlend && (f || !cur.fBlend || st.alpha != cur.alpha);
        }
        // (CANE_PULL: the vertex program reads the arenas' buffers itself; the VAO only holds the index list, bound at
        // the draw (s_vaoPullCur): no VAO switch for each run)
        if (!g_fDrawUniform && (f || st.vao != cur.vao)) glBindVertexArray(st.vao);
        if (fBindTexture && st.fTextured && (f || !cur.fTextured || st.tex != cur.tex || !fTexBound)) glBindTexture(GL_TEXTURE_2D, st.tex);
        if (fBindTexture && st.fTextured && (f || !cur.fTextured || st.sampler != cur.sampler || !fTexBound)) glBindSampler(0, s_asampler[st.sampler]);
        if (fu || st.fTextured != ucur.fTextured) glUniform1i(s_pu->fTextured, st.fTextured);
        if (fu || st.icolor != ucur.icolor) glUniform1i(s_pu->icolor, st.icolor);
        if (f || st.fBlend != cur.fBlend) { if (st.fBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
        // (g_fBlend1: the program's CANE_BLEND1 mode with the blend; the blend itself changes only between the pair and
        // the other blends)
        const bool fBlend1 = g_fBlend1 && s_pu->caneBlend1 >= 0;
        if (fBlend1)
        {
            const int mode = st.fBlend ? Blend1Mode(st.alpha) : 0, modeCur = ucur.fBlend ? Blend1Mode(ucur.alpha) : 0;
            if (fu || mode != modeCur) glUniform1i(s_pu->caneBlend1, mode);
        }
        if (st.fBlend && (f || !cur.fBlend || st.alpha != cur.alpha))
        {
            // (the pair shares one blend: set again only on a change to or from another blend)
            const int mode = fBlend1 ? Blend1Mode(st.alpha) : 0, modeCur = fBlend1 && cur.fBlend ? Blend1Mode(cur.alpha) : 0;
            if (f || !cur.fBlend || mode == 0 || modeCur == 0) FSetBlend(st.alpha, fBlend1);
        }
        if (f || st.fCull != cur.fCull)
        {
            if (st.fCull) { glEnable(GL_CULL_FACE); glFrontFace(FrontCCW()); }
            else glDisable(GL_CULL_FACE);
        }
        if (f) glEnable(GL_DEPTH_TEST);
        if (f || st.depthFunc != cur.depthFunc) glDepthFunc(st.depthFunc == 0 ? GL_ALWAYS : st.depthFunc == 1 ? GL_GEQUAL : GL_GREATER);
        if (f || st.depthMask != cur.depthMask) glDepthMask(st.depthMask ? GL_TRUE : GL_FALSE);
        if (f || st.colorMask != cur.colorMask)
        {
            const GLboolean c = (st.colorMask & 1) ? GL_TRUE : GL_FALSE;
            glColorMask(c, c, c, (st.colorMask & 2) ? GL_TRUE : GL_FALSE);
        }
        if (fu || st.ate != ucur.ate) glUniform1i(s_pu->ate, st.ate);
        if (fu || st.atst != ucur.atst) glUniform1i(s_pu->atst, st.atst);
        if (fu || st.aref != ucur.aref) glUniform1f(s_pu->aref, float(st.aref));
        if (fu || st.afailMode != ucur.afailMode) glUniform1i(s_pu->afailMode, st.afailMode);
        if (fu || st.fFogBlack != ucur.fFogBlack) glUniform1i(s_pu->fFogBlack, st.fFogBlack);
        s_aglobinh[iprog].fFogBlack = s_globinhLast.fFogBlack = st.fFogBlack;
        // the frame alpha's bit 7 (FBA) in the stencil: set by the FBA kinds' alpha writes, cleared by the others'
        // (the native frame keeps alpha / 128, clamped: the shadow pass's DATE needs the bit, its CLUT the bits below)
        if (f || st.fFba != cur.fFba || st.colorMask != cur.colorMask || st.fNoStencil != cur.fNoStencil)
        {
            if (s_fLrzProbe || st.fNoStencil) glDisable(GL_STENCIL_TEST); else glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, st.fFba ? 0x80 : 0, 0x80);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glStencilMask((st.colorMask & 2) && !s_fLrzProbe && !st.fNoStencil ? 0x80 : 0);
        }
        cur = st;
        fValid = true;
        aucur[iprog] = st;
        afuValid[iprog] = true;
        fTexBound = fBindTexture;
    }
    bool fTexBound = true;
};
// a run's state draws with the program without discards (glGlobShaderNoKill) unless it alpha-tests
inline bool FStateNeedsKill(const DSTATE& st) { return st.ate && st.afailMode; }

// The bound glob program's inherited uniforms brought to the last values set (GLOBINHERIT); a changed fFogBlack makes
// sc set that program's uniforms again at its next run.
// (draw packets with the binding entries: RenderPass's useGlob leaves the glob program to the next packet draw, which
// binds it; the GL calls that need it bound (a run or special drawn with GL calls, and any other command, so the GL-style
// state is the GL calls' at every point a GL call could see it) bind it first: FlushProgramPending)
GLuint s_progPending = 0;
void FlushProgramPending()
{
    if (!s_progPending) return;
    glUseProgram(s_progPending);
    s_progPending = 0;
}
// pk: the uniforms set as constants of the program (glProgramUniform), which then need not be bound
void SyncGlobInherited(STATECACHE& sc, const cane::draw::Packets* pk = nullptr)
{
    GLOBINHERIT& h = s_aglobinh[s_iprogGlob];
    const GLuint prog = s_apshGlob[s_iprogGlob]->ID;
    if (h.rgbaConst != s_globinhLast.rgbaConst)
    {
        if (pk) { if (s_pu->rgbaConst >= 0) pk->setConstant(prog, s_pu->rgbaConst, &s_globinhLast.rgbaConst[0], 16); }
        else glUniform4fv(s_pu->rgbaConst, 1, &s_globinhLast.rgbaConst[0]);
        h.rgbaConst = s_globinhLast.rgbaConst;
    }
    if (h.fFogBlack != s_globinhLast.fFogBlack)
    {
        const int32_t v = s_globinhLast.fFogBlack;
        if (pk) { if (s_pu->fFogBlack >= 0) pk->setConstant(prog, s_pu->fFogBlack, &v, 4); }
        else glUniform1i(s_pu->fFogBlack, s_globinhLast.fFogBlack);
        h.fFogBlack = s_globinhLast.fFogBlack;
        sc.afuValid[s_iprogGlob] = false;
    }
}

// (draw packets, platform/draw_packet.h: the Vulkan renderer) a run's state as the backend's packet state, made once
// per state, glob program and winding: what STATECACHE::Apply sets with GL calls (the fixed-function state, the
// program's loose uniforms), so the backend's GL-style state after a packet draw is the same as after Apply's calls.
uint32_t PacketStateOf(const cane::draw::Packets& pk, uint32_t istate, int iprog)
{
    struct ENTRY { uint32_t id = 0; GLuint prog = 0; };
    static std::vector<ENTRY> s_aentry;
    const size_t k = (size_t(istate) * CPROGGLOB + size_t(iprog)) * 2 + (s_fFlipFaces ? 1 : 0);
    if (k >= s_aentry.size()) s_aentry.resize(k + 1 + s_aentry.size() / 2);
    const GLuint prog = s_apshGlob[iprog]->ID;
    ENTRY& e = s_aentry[k];
    if (e.prog == prog) return e.id;
    using namespace cane::draw;
    const DSTATE& st = s_astate[istate];
    const UNIFORMS& u = s_au[iprog];
    const bool fBlend1 = g_fBlend1 && u.caneBlend1 >= 0;
    State ps;
    ps.program = prog;
    ps.blend = st.fBlend;
    if (st.fBlend)
    {
        BLENDGL bl;
        FBlendOf(st.alpha, fBlend1, bl);
        ps.srcColor = bl.src, ps.dstColor = bl.dst, ps.srcAlpha = bl.srcA, ps.dstAlpha = bl.dstA;
        ps.opColor = bl.opColor, ps.opAlpha = bl.opAlpha;
        ps.fBlendConstant = bl.fConst;
        ps.blendConstant[3] = bl.fix;
    }
    ps.colorMask = uint8_t(((st.colorMask & 1) ? 7 : 0) | ((st.colorMask & 2) ? 8 : 0));
    ps.cull = st.fCull ? 1 : 0;
    ps.frontCCW = FrontCCW() == GL_CCW;
    ps.depthTest = 1;
    ps.depthWrite = st.depthMask ? 1 : 0;
    ps.depthCompare = st.depthFunc == 0 ? kAlways : st.depthFunc == 1 ? kGreaterEqual : kGreater;
    ps.stencilTest = s_fLrzProbe || st.fNoStencil ? 0 : 1;
    ps.stencilCompare = kAlways;
    ps.stencilRef = st.fFba ? 0x80 : 0;
    ps.stencilCompareMask = 0x80;
    ps.stencilWriteMask = (st.colorMask & 2) && !s_fLrzProbe && !st.fNoStencil ? 0x80 : 0;
    ps.stencilFail = ps.stencilDepthFail = kStencilKeep;
    ps.stencilPass = kStencilReplace;
    auto add = [&](GLint loc, uint32_t bits) { if (loc >= 0) ps.aconst[ps.cconst++] = {loc, bits}; };
    const float aref = float(st.aref);
    uint32_t arefBits;
    std::memcpy(&arefBits, &aref, 4);
    add(u.fTextured, st.fTextured);
    add(u.icolor, st.icolor);
    if (fBlend1) add(u.caneBlend1, uint32_t(st.fBlend ? Blend1Mode(st.alpha) : 0));
    add(u.ate, st.ate);
    add(u.atst, st.atst);
    add(u.aref, arefBits);
    add(u.afailMode, st.afailMode);
    add(u.fFogBlack, st.fFogBlack);
    e.id = pk.createState(ps);
    e.prog = prog;
    return e.id;
}

// One shader pass of an entry (formerly drawn directly): the GL state its GS registers ask for, as 1..2 commands
// (the alpha test's pass / fail sub-draws).
void EmitPass(std::vector<PASSCMD>& apasscmd, DSTATE st, const SUBGLOB& sub, const SHDPASS& pass, int ipass)
{
    // Texture
    if (pass.fTextured)
    {
        const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, pass.itexPass);
        st.tex = TexFromSel(sel);
        st.sampler = SamplerFor(st.tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0) | (pass.fNearest ? 4 : 0)));
        if (s_psubEmitting) s_mptexapsub[st.tex].insert(s_psubEmitting);
    }
    st.fTextured = pass.fTextured ? 1 : 0;
    st.icolor = uint8_t(pass.icolor);
    st.fFogBlack = pass.fFogBlack ? 1 : 0;
    // Blend (PRIM ABE)
    st.fBlend = pass.fBlend ? 1 : 0;
    st.alpha = pass.fBlend ? pass.alpha : 0;
    const bool fWritesColor = !((pass.alpha & 0xFF) == 0x40 || ((pass.alpha & 0xFF) == 0x50));  // Cd: (X - X) * C + Cd
    // Depth test (TEST ZTE / ZTST; reversed Z)
    const uint64_t test = pass.test;
    const bool zte = (test >> 16) & 1;
    const int ztst = int((test >> 17) & 3);
    if (zte && ztst == 0) return;
    st.depthFunc = uint8_t(!zte || ztst == 1 ? 0 : ztst == 2 ? 1 : 2);
    // Alpha test (TEST ATE / ATST / AREF / AFAIL)
    const bool ate = test & 1;
    const int atst = int((test >> 1) & 7);
    const int aref = int((test >> 4) & 0xFF);
    const int afail = int((test >> 12) & 3);
    st.ate = ate ? 1 : 0;
    st.atst = uint8_t(atst);
    st.aref = uint8_t(aref);
    auto draw = [&](int iSub, int afailMode, bool fDepth, bool fColor, bool fAlpha) {
        DSTATE stDraw = st;
        stDraw.afailMode = uint8_t(afailMode);
        stDraw.depthMask = fDepth ? 1 : 0;
        stDraw.colorMask = uint8_t((fColor ? 1 : 0) | (fColor && fAlpha ? 2 : 0));
        apasscmd.push_back({IstateOf(stDraw), uint32_t(ipass * 4 + iSub)});
    };
    // The tests the fragment's alpha (0..255 after the GS's clamp, glob.frag) always passes or always fails, by kind
    // and reference, drawn without the test (no discards): the AFAIL sub-draw alone for one that always fails.
    const bool fPassAll = atst == 1 || (atst == 5 && aref == 0) || (atst == 3 && aref >= 255);
    const bool fFailAll = atst == 0 || (atst == 2 && aref == 0) || (atst == 6 && aref >= 255);
    if (ate && (fPassAll || fFailAll)) st.ate = st.atst = st.aref = 0;   // (as without the test: one state, longer runs)
    if (!ate || fPassAll)
        draw(0, 0, true, fWritesColor, true);
    else if (fFailAll)
    {
        switch (afail)
        {
        case 1: draw(1, 0, false, fWritesColor, true); break;    // FB_ONLY
        case 2: draw(1, 0, true, false, false); break;           // ZB_ONLY
        case 3: draw(1, 0, false, fWritesColor, false); break;   // RGB_ONLY
        default: break;                                          // KEEP: nothing drawn
        }
    }
    else
    {
        if (atst != 0) draw(0, 1, true, fWritesColor, true);
        switch (afail)
        {
        case 1: draw(1, 2, false, fWritesColor, true); break;    // FB_ONLY
        case 2: draw(1, 2, true, false, false); break;           // ZB_ONLY
        case 3: draw(1, 2, false, fWritesColor, false); break;   // RGB_ONLY
        default: break;                                          // KEEP
        }
    }
}

// The entry's cached commands (rebuilt when its shader or SAA frame changes).
const SUBGLOBGPU& SubgpuOf(const SUBGLOB& sub, GLuint vao, uint8_t fCull)
{
    SUBGLOBGPU* p = sub.pgpu;
    if (!p || p->powner != &sub)
    {
        p = &s_asubgpu.emplace_back();
        p->powner = &sub;
        sub.pgpu = p;
    }
    // (one cache per arena: the instances' own vertex copies live in other arenas)
    while (p->vao != vao && p->vao != 0)
    {
        if (!p->pnextVao)
        {
            SUBGLOBGPU* pnew = &s_asubgpu.emplace_back();
            pnew->powner = &sub;
            p->pnextVao = pnew;
        }
        p = p->pnextVao;
    }
    const int iframe = sub.psaa ? sub.psaa->iframe : 0;
    if (p->pshd != sub.pshd || p->iframe != iframe || p->vao != vao || p->fCull != fCull || p->genTex != s_genTexCache)
    {
        p->genTex = s_genTexCache;
        p->pshd = sub.pshd;
        p->iframe = iframe;
        p->vao = vao;
        p->fCull = fCull;
        p->apasscmd.clear();
        DSTATE st;
        st.vao = vao;
        st.fCull = fCull;
        s_psubEmitting = FTextureModStreaming() ? p : nullptr;
        for (size_t ipass = 0; ipass < sub.pshd->apass.size(); ++ipass)
            EmitPass(p->apasscmd, st, sub, sub.pshd->apass[ipass], int(ipass));
        s_psubEmitting = nullptr;
    }
    return *p;
}

// ------------------------------------------------------------ cast shadows (mesh type 2)
// What its VU1 program draws, reconstructed from the data and 0x1A4478's shadow quadwords (ShadowOfLightContext):
// the mesh's records are edges {OA, E0, E1, OB} (vertex index x 2: each vertex twice in VU memory, as itself and
// projected), so the program extrudes the silhouette (the edges between a face toward the shadow's centre and one
// away from it) from the vertices to their projections from the centre onto the far plane (centre + axis, normal
// along the axis), with the faces toward the centre as the near cap and their projections as the far cap: a
// shadow volume. The pixels inside are darkened like the type-3 hulls (z-fail stencil, the shader's alpha).
std::vector<glm::vec3> s_avtxShadow2;
#if SLY2_FASTPATHS
std::vector<uint32_t> s_acvtxShadow2Caster;   // (the Switch: the vertex count of each of the pass's casters, in order)
std::vector<uint8_t> s_afClosedShadow2Caster; // (and whether each is a closed, consistently oriented surface: FClosedOriented)
#endif
GLSHADER s_shShadowVol, s_shShadowVolNoKill;
GLuint s_vaoShadowVol = 0, s_vboShadowVol = 0;

// (the volume's vertices appended to avtxOut, cmd's range in it; any thread: the scratch is the thread's own)
bool FBuildShadow2(const SCENEGLOBSET& sgs, size_t iglob, const GLOB& glob, const glm::vec3& posCamera, float sNear,
                   std::vector<glm::vec3>& avtxOut, CMD& cmd)
{
    static const bool s_fDbg = std::getenv("SLY2_SH2DBG") != nullptr;
    static const char* const s_szOnly = std::getenv("SLY2_SH2ONLY");
    if (s_fDbg)
        std::printf("shadow2: oid %d sgs %p glob %zu sMRD %g grfGroup 0x%X n0 %d cbone %d fx %zu oidOwner %d gZ %g\n", sgs.oid, (const void*)&sgs, iglob, glob.sMRD,
                    glob.grfGroup, glob.n0, glob.cbone, glob.asubfx.size(), glob.oidOwner, glob.gZOrder);
    if (s_szOnly && int(iglob) != std::atoi(s_szOnly)) return false;
    const glm::mat4& mat = sgs.amatGlob[iglob];
    const std::vector<glm::mat4>* pamatBone = iglob < sgs.aamatBone.size() && !sgs.aamatBone[iglob].empty() ? &sgs.aamatBone[iglob] : nullptr;
    // the world positions (skinned by the weights' bones)
    static thread_local std::vector<std::vector<glm::vec3>> s_aav;
#if SLY2_FASTPATHS
    // (the Switch: never shrunk, so the pieces' vectors keep their storage from one caster to the next)
    if (s_aav.size() < glob.asubfx.size()) s_aav.resize(glob.asubfx.size());
#else
    s_aav.resize(glob.asubfx.size());
#endif
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
    {
        const SUBFX& fx = glob.asubfx[k];
        auto& av = s_aav[k];
        av.resize(fx.apos.size());
        for (size_t v = 0; v < fx.apos.size(); ++v)
        {
            const glm::vec4 p(fx.apos[v], 1.0f);
            glm::vec3 w(0.0f);
            if (pamatBone && fx.cw > 0)
            {
                for (int j = 0; j < fx.cw && j < 4; ++j)
                {
                    const size_t ib = std::min<size_t>(fx.aiboneSlot[j], pamatBone->size() - 1);
                    w += float(fx.aw[v * size_t(fx.cw) + size_t(j)]) / 255.0f * glm::vec3(MulMat4Vec4((*pamatBone)[ib], p));
                }
            }
            else w = glm::vec3(MulMat4Vec4(mat, p));
            av[v] = w;
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    }
    const glm::vec3 posCtx = sgs.fPosLight ? sgs.posLight : (lo + hi) * 0.5f;
    float sRadius = 0;   // (pl +0x1C: the shadow-casting hierarchy's radius about the context's position)
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
        for (const glm::vec3& v : s_aav[k]) sRadius = std::max(sRadius, glm::length(v - posCtx));
    const SHADOWCTX sh = ShadowOfLightContext(WorldLights(), posCtx, sRadius, 400.0f, posCamera, sNear);   // (+0x13C 0x2C3B90)
    if (!sh.fValid) return false;
    if (s_fDbg)
        std::printf("shadow2: lo %.0f %.0f %.0f hi %.0f %.0f %.0f bones %zu\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, pamatBone ? pamatBone->size() : 0),
        std::printf("shadow2: ctx %.0f %.0f %.0f r %.0f centre %.0f %.0f %.0f axis %.0f %.0f %.0f\n", posCtx.x, posCtx.y, posCtx.z, sRadius,
                    sh.center.x, sh.center.y, sh.center.z, sh.axis.x, sh.axis.y, sh.axis.z);
    const glm::vec3 dir = glm::normalize(sh.axis);
    const glm::vec3 posFar = sh.center + sh.axis;
    const float dFar = glm::dot(posFar - sh.center, dir);
    // PC QoL "Long shadows" (display.h DISPLAY::fLongShadows, default on; NOT the original, render.md "Long shadows"):
    // the original ends every prism at the plane 400 past the caster along the axis (+0x13C), so a low light (the
    // title's window) cuts the shadow off at a straight edge on the floor (R39). Here a ray from the centre through a
    // vertex that descends is lengthened, never shortened, until it reaches the horizontal plane kLongShadowDepth below
    // the caster's lowest vertex (its feet: the floor it stands on), at most kLongShadowMaxExtra past the vertex. So the
    // far end is under the floor the shadow falls on (out of view) instead of on it, and the extension cannot reach
    // more than kLongShadowDepth below the caster (no shadows through floors onto the level below, no streaks across
    // the level). Rays that do not descend (a light below the vertex) keep the original's length.
    constexpr float kLongShadowDepth = 50.0f, kLongShadowMaxExtra = 2000.0f;
    static const bool s_fLongOff = [] { const char* sz = std::getenv("SLY2N_LONGSHADOWS"); return sz && sz[0] == '0'; }();
    const bool fLong = g_display.fLongShadows && !s_fLongOff;
    const float zLongFloor = lo.z - kLongShadowDepth;
    auto project = [&](const glm::vec3& v) {
        const glm::vec3 dvc = v - sh.center;
        const float dv = glm::dot(dvc, dir);
        if (dv <= 1.0f) return v + dir * dFar;
        float s = dFar / dv;
        const float dzDown = -dvc.z;   // (how far the vertex is below the centre)
        if (fLong && dzDown > 1e-3f)
        {
            const float sFloor = (sh.center.z - zLongFloor) / dzDown;
            if (sFloor > s) s = std::min(sFloor, std::max(s, 1.0f + kLongShadowMaxExtra / glm::length(dvc)));
        }
        return sh.center + dvc * s;
    };
    const uint32_t ivtxFirst = uint32_t(avtxOut.size());
    // (perf-3: a prism's 24 vertices written through one resize, not 24 push_backs)
    glm::vec3* pout = nullptr;
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, const glm::vec3& outward) {
        if (glm::dot(glm::cross(b - a, c - a), outward) < 0.0f) std::swap(b, c);
        pout[0] = a;
        pout[1] = b;
        pout[2] = c;
        pout += 3;
    };
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
    {
        const SUBFX& fx = glob.asubfx[k];
        const auto& av = s_aav[k];
        auto P = [&](uint8_t u) { const size_t i = u / 2; return i < av.size() ? av[i] : glm::vec3(0.0f); };
        auto lit = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
            return glm::dot(glm::cross(b - a, c - a), dir) < 0.0f;
        };
        // each face once (it is in three edge records), as its own closed prism from the face to its projection:
        // the union of the prisms is the shadow whatever the mesh's closure and winding (the records' edges
        // between faces toward and away from the centre are the silhouette of a closed mesh; the hulls are not
        // all closed: a silhouette-only volume leaks there)
        // (each face once: a bit per sorted vertex triple; the records index at most 128 vertices x 2)
        static thread_local std::vector<uint64_t> s_abDone((size_t(1) << 21) / 64, 0);
        static thread_local std::vector<uint32_t> s_akDone;
        for (uint32_t k : s_akDone) s_abDone[k >> 6] = 0;
        s_akDone.clear();
        // (display.h fFastShadows, the Switch always)
#ifdef __SWITCH__
        constexpr bool fFastShadows = true;
#else
        static const int s_maskFastSh = std::getenv("SLY2N_FASTSHADOWMASK") ? std::atoi(std::getenv("SLY2N_FASTSHADOWMASK")) : 3;   // (debug: 1 walls, 2 z-pass)
        const bool fFastShadows = g_display.fFastShadows && (s_maskFastSh & 1);
#endif
        if (fFastShadows)
        {
        // (the Switch: the same prisms without the side walls two of them share from opposite sides: the same four
        // corners, one counting +1 where the other counts -1 in z-pass and z-fail alike, so the pair counts nothing.
        // Only a hairline along the wall's diagonal, which each prism split its own way, can count differently.)
        struct PRISM { glm::vec3 av[6]; glm::vec3 m; uint8_t grfWall; };
        static thread_local std::vector<PRISM> s_aprism;
        static thread_local std::vector<uint64_t> s_awall;   // (edge << 32 | prism << 3 | wall << 1 | facing)
        s_aprism.clear();
        s_awall.clear();
        auto prism = [&](uint8_t ia, uint8_t ib, uint8_t ic) {
            std::array<uint8_t, 3> key{ia, ib, ic};
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[2] < key[1]) std::swap(key[1], key[2]);
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[0] == key[1] || key[1] == key[2]) return;
            const uint32_t k = (uint32_t(key[0] >> 1) << 14) | (uint32_t(key[1] >> 1) << 7) | uint32_t(key[2] >> 1);
            if (s_abDone[k >> 6] & (uint64_t(1) << (k & 63))) return;
            s_abDone[k >> 6] |= uint64_t(1) << (k & 63);
            s_akDone.push_back(k);
            const glm::vec3 a = P(ia), b = P(ib), c = P(ic);
            if (!lit(a, b, c) && !lit(a, c, b)) return;   // (edge-on)
            PRISM p{{a, b, c, project(a), project(b), project(c)}, glm::vec3(0.0f), 7};
            p.m = (a + b + c + p.av[3] + p.av[4] + p.av[5]) / 6.0f;
            const uint32_t iprism = uint32_t(s_aprism.size());
            const uint8_t ai[3] = {uint8_t(ia >> 1), uint8_t(ib >> 1), uint8_t(ic >> 1)};
            for (uint32_t w = 0; w < 3; ++w)
            {
                // the edge (w, w + 1)'s wall from its lower vertex, and the prism's side of it. Only a flat wall pairs
                // (a bent one, a corner from project's axis branch, differs by diagonal); an unclear facing is kept.
                const uint32_t i0 = ai[w] < ai[(w + 1) % 3] ? w : (w + 1) % 3, i1 = i0 == w ? (w + 1) % 3 : w;
                const glm::vec3 &wa = p.av[i0], &wb = p.av[i1], &waf = p.av[3 + i0], &wbf = p.av[3 + i1];
                const glm::vec3 n = glm::cross(wb - wa, waf - wa);
                const float ln = glm::length(n);
                if (!(ln > 0.0f) || std::fabs(glm::dot(n, wbf - wa)) > 1e-3f * ln * glm::length(wbf - wa)) continue;
                const glm::vec3 out = (wa + wb + waf + wbf) * 0.25f - p.m;
                const float d = glm::dot(n, out);
                if (!(std::fabs(d) > 1e-4f * ln * glm::length(out))) continue;
                const uint32_t edge = uint32_t(ai[i0]) << 8 | ai[i1];
                s_awall.push_back(uint64_t(edge) << 32 | uint64_t(iprism) << 3 | uint64_t(w) << 1 | (d > 0.0f ? 1u : 0u));
            }
            s_aprism.push_back(p);
        };
        for (size_t r = 0; r + 3 < fx.arec.size(); r += 4)
        {
            const uint8_t oa = fx.arec[r], e0 = fx.arec[r + 1], e1 = fx.arec[r + 2], ob = fx.arec[r + 3];
            prism(oa, e0, e1);
            prism(e1, e0, ob);
        }
        // the walls left out: an edge's exactly two, of opposite facings
        std::sort(s_awall.begin(), s_awall.end());
        for (size_t i = 0; i < s_awall.size();)
        {
            size_t j = i + 1;
            while (j < s_awall.size() && (s_awall[j] >> 32) == (s_awall[i] >> 32)) ++j;
            if (j - i == 2 && ((s_awall[i] ^ s_awall[i + 1]) & 1))
                for (size_t q = i; q < j; ++q) s_aprism[size_t((s_awall[q] >> 3) & 0x1FFFFFFF)].grfWall &= uint8_t(~(1u << ((s_awall[q] >> 1) & 3)));
            i = j;
        }
        size_t cvtxPiece = 0;
        for (const PRISM& p : s_aprism) cvtxPiece += 6 + 6 * size_t(std::popcount(unsigned(p.grfWall)));
        const size_t ivtxPiece = avtxOut.size();
        avtxOut.resize(ivtxPiece + cvtxPiece);
        pout = avtxOut.data() + ivtxPiece;
        for (const PRISM& p : s_aprism)
        {
            const glm::vec3 &a = p.av[0], &b = p.av[1], &c = p.av[2], &af = p.av[3], &bf = p.av[4], &cf = p.av[5];
            auto side = [&](const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2) { tri(v0, v1, v2, (v0 + v1 + v2) / 3.0f - p.m); };
            side(a, b, c);
            side(af, bf, cf);
            if (p.grfWall & 1) { side(a, b, bf); side(a, bf, af); }
            if (p.grfWall & 2) { side(b, c, cf); side(b, cf, bf); }
            if (p.grfWall & 4) { side(c, a, af); side(c, af, cf); }
        }
        }
        else
        {
        auto prism = [&](uint8_t ia, uint8_t ib, uint8_t ic) {
            std::array<uint8_t, 3> key{ia, ib, ic};
            // (sorted: a three-element network, the same order as std::sort)
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[2] < key[1]) std::swap(key[1], key[2]);
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[0] == key[1] || key[1] == key[2]) return;
            const uint32_t k = (uint32_t(key[0] >> 1) << 14) | (uint32_t(key[1] >> 1) << 7) | uint32_t(key[2] >> 1);
            if (s_abDone[k >> 6] & (uint64_t(1) << (k & 63))) return;
            s_abDone[k >> 6] |= uint64_t(1) << (k & 63);
            s_akDone.push_back(k);
            const glm::vec3 a = P(ia), b = P(ib), c = P(ic);
            if (!lit(a, b, c) && !lit(a, c, b)) return;   // (edge-on)
            const glm::vec3 af = project(a), bf = project(b), cf = project(c);
            const glm::vec3 m = (a + b + c + af + bf + cf) / 6.0f;
            const size_t ivtx = avtxOut.size();
            avtxOut.resize(ivtx + 24);
            pout = avtxOut.data() + ivtx;
            auto side = [&](const glm::vec3& p, const glm::vec3& q, const glm::vec3& r) { tri(p, q, r, (p + q + r) / 3.0f - m); };
            side(a, b, c);
            side(af, bf, cf);
            side(a, b, bf); side(a, bf, af);
            side(b, c, cf); side(b, cf, bf);
            side(c, a, af); side(c, af, cf);
        };
        for (size_t r = 0; r + 3 < fx.arec.size(); r += 4)
        {
            const uint8_t oa = fx.arec[r], e0 = fx.arec[r + 1], e1 = fx.arec[r + 2], ob = fx.arec[r + 3];
            prism(oa, e0, e1);
            prism(e1, e0, ob);
        }
        }
    }
    cmd.firstIndex = ivtxFirst;
    cmd.count = uint32_t(avtxOut.size()) - ivtxFirst;
    cmd.idraw = uint32_t(glob.asubfx[0].pshd->argba[0] >> 24);   // (the darkening alpha)
    return cmd.count > 0;
}

// The stencil while a shadow phase is open (0x16DA48 .. 0x16DD58), as the original's frame green: kind 7 counts in
// bits 0..6 from 0x3F (0x16DA48's fill 0x7E, the hulls' +-2 per face: G bit 7), so bit 6 is set where the count is
// 1..64; kind 8 counts in bits 0..5 from 0x1F (fill 0x3E keeping bit 7: G bit 6), bit 5 set at 1..32, bit 6 kept.
// STENCIL_FBA: the frame alpha's bit 7 the FBA kinds wrote (STATECACHE).
constexpr GLuint STENCIL_FBA = 0x80, STENCIL_SH7 = 0x40, STENCIL_SH8 = 0x20, STENCIL_COUNT7 = 0x7F, STENCIL_COUNT8 = 0x3F;

// fDateMarked: the stencil's STENCIL_FBA holds the phase's DATE (FMarkDate), so the program without the discard draws.
// Whether triangles (3 vertices each) form closed, consistently oriented surfaces: each directed edge p -> q (exact
// positions) appears as often as q -> p.
// Fast shadows in effect (DrawShadow2's choice)
bool FFastShadowsNow()
{
#ifdef __SWITCH__
    return true;
#else
    static const int s_maskFastSh = std::getenv("SLY2N_FASTSHADOWMASK") ? std::atoi(std::getenv("SLY2N_FASTSHADOWMASK")) : 3;
    return g_display.fFastShadows && (s_maskFastSh & 2);
#endif
}

bool FClosedOriented(const glm::vec3* av, uint32_t cvtx)
{
    struct EDGE { std::array<uint32_t, 6> a; bool operator<(const EDGE& o) const { return a < o.a; } bool operator==(const EDGE& o) const { return a == o.a; } };
    static thread_local std::vector<EDGE> s_aedge, s_arev;
    s_aedge.clear();
    s_arev.clear();
    auto bits = [](const glm::vec3& p, uint32_t* pu) { std::memcpy(pu, &p, sizeof(glm::vec3)); };
    for (uint32_t t = 0; t + 2 < cvtx; t += 3)
        for (int k = 0; k < 3; ++k)
        {
            const glm::vec3 &p = av[t + uint32_t(k)], &q = av[t + uint32_t((k + 1) % 3)];
            EDGE e, r;
            bits(p, e.a.data()); bits(q, e.a.data() + 3);
            bits(q, r.a.data()); bits(p, r.a.data() + 3);
            s_aedge.push_back(e);
            s_arev.push_back(r);
        }
    std::sort(s_aedge.begin(), s_aedge.end());
    std::sort(s_arev.begin(), s_arev.end());
    return s_aedge == s_arev;
}

// (DrawShadow2) the depth bounds test: GL_EXT_depth_bounds_test, or the Vulkan layer's on a device with depthBounds
constexpr GLenum GL_DEPTH_BOUNDS_TEST_EXT_ = 0x8890;
typedef void (APIENTRYP PFNDEPTHBOUNDS)(GLclampd zmin, GLclampd zmax);
PFNDEPTHBOUNDS s_pfnDepthBounds = nullptr;
bool FShadowDepthBounds()
{
    static int s_state = 0;
    if (s_state) return s_state > 0;
    s_state = -1;
    if (const char* sz = std::getenv("SLY2N_SHBOUNDS"); sz && std::atoi(sz) == 0) return false;
    bool fHave = false;
    if (FRendererVulkan()) fHave = FVulkanDepthBounds();
    else
    {
        GLint cext = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &cext);
        for (GLint i = 0; i < cext && !fHave; ++i)
            if (const char* p = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)))) fHave = !std::strcmp(p, "GL_EXT_depth_bounds_test");
    }
    if (fHave) s_pfnDepthBounds = reinterpret_cast<PFNDEPTHBOUNDS>(cane::gl::ProcAddress("glDepthBoundsEXT"));
    if (s_pfnDepthBounds) s_state = 1;
    std::printf("%s: shadow volumes %s the depth bounds test\n", FRendererVulkan() ? "Vulkan" : "OpenGL", s_state > 0 ? "use" : "without");
    return s_state > 0;
}
struct SH2DRAW { uint32_t first = 0, count = 0; float zlo = 0.0f, zhi = 1.0f; bool fPass = false; };
std::vector<SH2DRAW> s_ash2draw;
std::vector<std::array<float, 2>> s_azrangeShadow2Caster;   // (each caster's window depth range: ZRangeOfCaster)
glm::mat4 s_matShBounds{1.0f};                               // (the matrix they were made with)
// A caster's window depth range (GL_ZERO_TO_ONE: z / w) with a margin far above the GPU's own rounding of its vertices'
// depths; 0..1 if a vertex is behind the camera or past the far plane.
std::array<float, 2> ZRangeOfCaster(const glm::mat4& m, const glm::vec3* av, uint32_t cvtx)
{
    const glm::vec4 rz(m[0][2], m[1][2], m[2][2], m[3][2]), rw(m[0][3], m[1][3], m[2][3], m[3][3]);
    float zlo = 1.0f, zhi = 0.0f;
    for (uint32_t k = 0; k < cvtx; ++k)
    {
        const glm::vec4 v(av[k], 1.0f);
        const float w = glm::dot(rw, v), z = glm::dot(rz, v) / w;
        if (!(w > 1e-6f) || !(z >= 0.0f)) return {0.0f, 1.0f};
        zlo = std::min(zlo, z), zhi = std::max(zhi, z);
    }
    if (!cvtx) return {0.0f, 1.0f};
    return {std::max(0.0f, zlo * (1.0f - 1e-4f) - 1e-6f), std::min(1.0f, zhi * (1.0f + 1e-4f) + 1e-6f)};
}

// pplaneNear (the Switch): the plane past which a vertex lies in front of the drawn near plane (n . p + d > 0).
// pplaneNear1 (two views): the second eye's plane: a caster counts z-pass only in front of both.
void DrawShadow2(const CMD& cmd, const glm::mat4& matWorldToClip, bool fDateMarked, const glm::vec4* pplaneNear = nullptr,
                 const glm::vec4* pplaneNear1 = nullptr)
{
    if (!s_shShadowVol.ID)
    {
        s_shShadowVol.Init("shadowvol.vert", "shadowvol.frag");
        // the variant without the DATE discard (ID 0 if it does not build: the one above draws)
        if (!s_shShadowVolNoKill.FInit("shadowvol.vert", "shadowvol.frag", "#define NOKILL 1\n")) s_shShadowVolNoKill.ID = 0;
        s_vboShadowVol = GlNewBuffer(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);
        if (const cane::draw::Packets* pkv = PpacketsVao())
        {
            const cane::draw::VertexAttrib a{0, 3, cane::draw::kAttribFloat, 0, 0, s_vboShadowVol, 0, sizeof(glm::vec3), 0};
            s_vaoShadowVol = pkv->createVertexArray(&a, 1, 0);
        }
        else
        {
        glGenVertexArrays(1, &s_vaoShadowVol);
        glBindVertexArray(s_vaoShadowVol);
        glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        }
    }
    const bool fDate = !(cmd.order & 8);   // (the shader's flag 8: no DATE)
    const GLSHADER& sh = (!fDate || fDateMarked) && s_shShadowVolNoKill.ID ? s_shShadowVolNoKill : s_shShadowVol;
    // (draw packets: the z-pass and z-fail counts and the GL state left after them as packet states)
    const cane::draw::Packets* ppk = PpacketsSpecial();
    uint32_t aidPk[3] = {};
    struct LOCSV { GLuint prog = 0; GLint matWorldToClip = -1, texDate = -1, fDate = -1, rgba = -1; };
    static LOCSV s_alocsv[2];
    LOCSV& loc = s_alocsv[&sh == &s_shShadowVolNoKill ? 1 : 0];
    if (ppk)
    {
        if (loc.prog != sh.ID)
            loc = {sh.ID, sh.Loc("matWorldToClip"), sh.Loc("texDate"), sh.Loc("fDate"), sh.Loc("rgba")};
        const bool fCCW = FrontCCW() == GL_CCW;
        const uint32_t maskDate = (cmd.order & 8) ? 0 : STENCIL_FBA;
        const int fDateU = fDate && !fDateMarked ? 1 : 0;
        for (int k = 0; k < 3; ++k)
        {
            using namespace cane::draw;
            const uint64_t key = (uint64_t(sh.ID) << 32) | (uint64_t(k) << 8) | (fCCW ? 1u : 0u) | (maskDate ? 2u : 0u) | (fDateU ? 4u : 0u);
            aidPk[k] = PacketStateFor(*ppk, key, [&](State& ps) {
                ps.program = sh.ID;
                ps.winding = 1, ps.frontCCW = fCCW;
                ps.depthTest = 1, ps.depthWrite = 0, ps.depthCompare = kGreater;
                ps.colorMask = k == 2 ? 0xF : 0;
                ps.stencilTest = k != 2, ps.stencilWriteMask = uint8_t(k == 2 ? 0xFFu : STENCIL_COUNT7);
                ps.stencilCompare = kEqual, ps.stencilRef = 0, ps.stencilCompareMask = uint8_t(maskDate);
                ps.stencilBack = 1;
                if (k == 0) ps.stencilPass = kStencilIncrWrap, ps.stencilBackPass = kStencilDecrWrap;
                else ps.stencilDepthFail = kStencilDecrWrap, ps.stencilBackDepthFail = kStencilIncrWrap;
                if (loc.texDate >= 0) ps.aconst[ps.cconst++] = {loc.texDate, 1};
                if (loc.fDate >= 0) ps.aconst[ps.cconst++] = {loc.fDate, uint32_t(fDateU)};
            });
            if (!aidPk[k]) ppk = nullptr;
        }
    }
    if (ppk)
    {
        ppk->setConstant(sh.ID, loc.matWorldToClip, glm::value_ptr(matWorldToClip), 64);
        const float argba0[4] = {};
        ppk->setConstant(sh.ID, loc.rgba, argba0, 16);
    }
    else
    {
        sh.Use();
        glUniformMatrix4fv(sh.Loc("matWorldToClip"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
        glUniform1i(sh.Loc("texDate"), 1);
        glUniform1i(sh.Loc("fDate"), fDate && !fDateMarked ? 1 : 0);
        glUniform4f(sh.Loc("rgba"), 0, 0, 0, 0);
        glBindVertexArray(s_vaoShadowVol);
    }
    if (!ppk || !PpacketsBuf()) glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);   // (the GL calls' upload below)
    const glm::vec3* pvtx = s_avtxShadow2.data() + cmd.firstIndex;
    uint32_t cpass = 0;   // (the Switch: the vertices counted z-pass, uploaded first)
#ifdef __SWITCH__
    constexpr bool fFastShadows = true;
#else
    static const int s_maskFastSh = std::getenv("SLY2N_FASTSHADOWMASK") ? std::atoi(std::getenv("SLY2N_FASTSHADOWMASK")) : 3;
    const bool fFastShadows = g_display.fFastShadows && (s_maskFastSh & 2);   // (display.h: not the same picture, an extra dark quad at the Ep8 roof chase's start)
#endif
    // (the Switch: a caster's volume wholly in front of the drawn near plane neither holds the camera nor is cut by it,
    // so z-pass counts it as z-fail does, and its fragments behind the scene (most of a long shadow) fail early Z)
    static std::vector<glm::vec3> s_avtxSplit;
    uint32_t cvtxCasters = 0;
    for (uint32_t c : s_acvtxShadow2Caster) cvtxCasters += c;
    bool fMapped = false;   // (the split written straight into the backend's stream memory for the vertex buffer)
    // Each caster's count drawn only where the scene's depth can be within its volume's depth range (glDepthBoundsEXT;
    // GL_EXT_depth_bounds_test, the Vulkan layer's depthBounds), the picture unchanged: a face counts (z-fail) only where
    // it is behind the scene, so where the scene lies beyond the whole volume nothing counts; where it lies in front of
    // the whole volume every face counts, which sums to zero exactly when the caster's triangles form a consistently
    // oriented closed surface (FClosedOriented; the z-pass casters always), else that side is kept. A caster reaching
    // behind the camera or past the far plane is drawn everywhere. Not in a two-view pass. SLY2N_SHBOUNDS=0: off.
    const bool fBounds = cvtxCasters == cmd.count && !pplaneNear1 && !g_fMultiviewPass && FShadowDepthBounds() &&
                         s_azrangeShadow2Caster.size() == s_acvtxShadow2Caster.size() && matWorldToClip == s_matShBounds;
    if (fBounds)
    {
        s_ash2draw.resize(s_acvtxShadow2Caster.size());
        uint32_t i = 0;
        for (size_t ic = 0; ic < s_acvtxShadow2Caster.size(); ++ic)
        {
            SH2DRAW& d = s_ash2draw[ic];
            const bool fClosed = ic < s_afClosedShadow2Caster.size() && s_afClosedShadow2Caster[ic];
            d.first = i, d.count = s_acvtxShadow2Caster[ic], d.fPass = false;
            d.zlo = s_azrangeShadow2Caster[ic][0];
            d.zhi = fClosed ? s_azrangeShadow2Caster[ic][1] : 1.0f;
            i += d.count;
        }
    }
    if (fFastShadows && pplaneNear && cvtxCasters == cmd.count)
    {
        glm::vec3* pvtxSplit = nullptr;
        if (ppk && ppk->mapStream && cmd.count)
            pvtxSplit = static_cast<glm::vec3*>(ppk->mapStream(s_vboShadowVol, size_t(cmd.count) * sizeof(glm::vec3)));
        fMapped = pvtxSplit != nullptr;
        if (!fMapped) s_avtxSplit.resize(cmd.count), pvtxSplit = s_avtxSplit.data();
        const glm::vec3 n(*pplaneNear);
        uint32_t cfail = cmd.count, i = 0;
        size_t icaster = 0;
        for (const uint32_t c : s_acvtxShadow2Caster)
        {
            const bool fClosed = icaster < s_afClosedShadow2Caster.size() && s_afClosedShadow2Caster[icaster];
            ++icaster;
            bool fFront = true;
            for (uint32_t k = 0; k < c && fFront; ++k) fFront = glm::dot(n, pvtx[i + k]) + pplaneNear->w > 0.0f;
            if (pplaneNear1)
                for (uint32_t k = 0; k < c && fFront; ++k) fFront = glm::dot(glm::vec3(*pplaneNear1), pvtx[i + k]) + pplaneNear1->w > 0.0f;
            // (perf-5) and only a consistently oriented closed surface: every directed edge met by its reverse as often.
            // Then z-pass counts as z-fail does; a caster whose prism faces the outward test turned the wrong way (the
            // long shadows' twisted prisms) keeps z-fail (the extra dark quad at the Ep8 roof chase's start)
            if (fFront) fFront = fClosed;
            if (fBounds) s_ash2draw[icaster - 1].fPass = fFront, s_ash2draw[icaster - 1].first = fFront ? cpass : cfail - c;
            if (fFront) { std::copy(pvtx + i, pvtx + i + c, pvtxSplit + cpass); cpass += c; }
            else { cfail -= c; std::copy(pvtx + i, pvtx + i + c, pvtxSplit + cfail); }
            i += c;
        }
        pvtx = fMapped ? nullptr : s_avtxSplit.data();
    }
    if (fMapped) {}
    else if (ppk) UploadStream(GL_ARRAY_BUFFER, s_vboShadowVol, pvtx, size_t(cmd.count) * sizeof(glm::vec3), size_t(cmd.count) * sizeof(glm::vec3));
    else glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(size_t(cmd.count) * sizeof(glm::vec3)), pvtx, GL_STREAM_DRAW);
    if (ppk && fBounds)
    {
        glEnable(GL_DEPTH_BOUNDS_TEST_EXT_);
        for (int k = 0; k < 2; ++k)
            for (const SH2DRAW& d : s_ash2draw)
                if (d.fPass == (k == 0))
                {
                    s_pfnDepthBounds(d.zlo, d.zhi);
                    ppk->drawArrays(aidPk[k], s_vaoShadowVol, d.first, d.count, nullptr, 0);
                }
        glDisable(GL_DEPTH_BOUNDS_TEST_EXT_);
        ppk->drawArrays(aidPk[2], s_vaoShadowVol, 0, 0, nullptr, 0);   // (no triangles: the state and vertex array the GL calls leave)
        return;
    }
    if (ppk)
    {
        if (cpass) ppk->drawArrays(aidPk[0], s_vaoShadowVol, 0, cpass, nullptr, 0);
        if (cmd.count > cpass) ppk->drawArrays(aidPk[1], s_vaoShadowVol, cpass, cmd.count - cpass, nullptr, 0);
        ppk->drawArrays(aidPk[2], s_vaoShadowVol, 0, 0, nullptr, 0);   // (no triangles: the state and vertex array the GL calls leave)
        return;
    }
    // the prisms' z-fail count into kind 7's (closed volumes: the original's +-2 per face in front of the scene)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(STENCIL_COUNT7);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, 0, (cmd.order & 8) ? 0 : STENCIL_FBA);   // (DATE: the alpha's bit 7 clear)
    glFrontFace(FrontCCW());
    glDisable(GL_BLEND);
    if (cpass)
    {
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        if (fBounds)
        {
            glEnable(GL_DEPTH_BOUNDS_TEST_EXT_);
            for (const SH2DRAW& d : s_ash2draw)
                if (d.fPass) s_pfnDepthBounds(d.zlo, d.zhi), glDrawArrays(GL_TRIANGLES, GLint(d.first), GLsizei(d.count));
            glDisable(GL_DEPTH_BOUNDS_TEST_EXT_);
        }
        else glDrawArrays(GL_TRIANGLES, 0, GLsizei(cpass));
    }
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    if (fBounds)
    {
        glEnable(GL_DEPTH_BOUNDS_TEST_EXT_);
        for (const SH2DRAW& d : s_ash2draw)
        {
            if (d.fPass) continue;
            s_pfnDepthBounds(d.zlo, d.zhi);
            glDrawArrays(GL_TRIANGLES, GLint(d.first), GLsizei(d.count));
        }
        glDisable(GL_DEPTH_BOUNDS_TEST_EXT_);
    }
    else if (cmd.count > cpass) glDrawArrays(GL_TRIANGLES, GLint(cpass), GLsizei(cmd.count - cpass));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

// One indexed draw with its draw index (glob.vert aDraw): the instance base, or with g_fDrawUniform (OpenGL ES without
// EXT_base_instance) the uniform caneDrawBase of the glob program, which is current for every caller.
// (CANE_PULL: the vertex program reads the bound arena's vertices from caneVertexBase + the index, the draw index
// itself with bit 31 set in caneDrawBase)
static void DrawIndexed(uint32_t count, uint32_t firstIndex, int32_t baseVertex, uint32_t idraw, GLuint vao)
{
    if (g_fDrawUniform)
    {
        PpullArena(vao);
        if (s_vaoPullCur != vao) { glBindVertexArray(vao); s_vaoPullCur = vao; }
        glUniform1ui(s_pu->caneVertexBase, uint32_t(baseVertex));
        glUniform1ui(s_pu->caneDrawBase, idraw | 0x80000000u);
        glDrawElements(GL_TRIANGLES, GLsizei(count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(firstIndex) * 4));
        return;
    }
    glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, GLsizei(count), GL_UNSIGNED_INT,
                                                  reinterpret_cast<void*>(size_t(firstIndex) * 4), 1, baseVertex, idraw);
}

void DrawOne(const CMD& cmd)
{
    DrawIndexed(cmd.count, cmd.firstIndex, cmd.baseVertex, cmd.idraw, cmd.vao);
    ++g_renderstats.cpass;
}

// (draw packets) DrawOne's draw as a packet draw with the state id
// (pu: the program's uniforms, default the bound glob program's)
void DrawOnePacket(const cane::draw::Packets& pk, uint32_t id, const CMD& cmd, const UNIFORMS* pu = nullptr)
{
    if (!pu) pu = s_pu;
    if (g_fDrawUniform)
    {
        PpullArena(cmd.vao);
        const cane::draw::Constant abase[2] = {{pu->caneVertexBase, uint32_t(cmd.baseVertex)}, {pu->caneDrawBase, cmd.idraw | 0x80000000u}};
        pk.drawIndexed(id, cmd.vao, cmd.count, cmd.firstIndex, 0, 0, abase, 2);
        s_vaoPullCur = cmd.vao;
    }
    else
        pk.drawIndexed(id, cmd.vao, cmd.count, cmd.firstIndex, cmd.baseVertex, cmd.idraw, nullptr, 0);
    ++g_renderstats.cpass;
}
// The volumes' colour draws on the Vulkan renderer's pulled path: glGlobShaderVol (gl.cpp: the no-discard program without
// CANE_BLEND1's second output, which hangs the Quest 3S's GPU in these draws under its video capture) with its uniforms
// and the pass's values RenderPass sets at each volume (the glob programs get them at useGlob)
// ([0] glGlobShaderVol, [1] glGlobShaderVolNoGeom: the one matching the glob program's near-clip stage, PshVol)
UNIFORMS s_auVol[2];
// The volumes' colour program for the glob program now (useGlob): without the near-clip stage when its runs draw
// without it (the same triangles as the stencil mask's; no geometry stage in a two-view pass); nullptr: the glob program
const GLSHADER* PshVol(int iprogGlob, const UNIFORMS** ppu)
{
    const int i = (iprogGlob & 2) ? 1 : 0;
    const GLSHADER* psh = i ? &glGlobShaderVolNoGeom : &glGlobShaderVol;
    if (!glGlobShaderVol.ID || !psh->ID) return nullptr;
    *ppu = &s_auVol[i];
    return psh;
}
struct VOLFRAME { glm::mat4 matWorldToClip{1.0f}; glm::vec3 posCamera{0.0f}, dirCamera{0.0f}; glm::vec4 fogColor{0.0f}; } s_volframe;
// (draw packets) DrawProjVolume's draws below as packets: each GL state of its sequence one packet state, the GL-style
// state after it as the GL calls leave it. False: a state the backend cannot take (nothing drawn: the GL calls then).
bool FDrawProjVolumePackets(const cane::draw::Packets& pk, const CMD& cmd)
{
    using namespace cane::draw;
    const SUBGLOB& sub = *cmd.psub;
    const SHD* pshd = sub.pshd;
    const uint8_t fl = pshd->grfshd;
    static const bool s_fVolDbg = std::getenv("SLY2N_VOLDBG") != nullptr;
    static const bool s_fVa = std::getenv("SLY2N_VOLVA") != nullptr;
    if (s_fVolDbg) return false;
    const GLuint progGlob = s_apshGlob[s_iprogGlob]->ID;
    const UNIFORMS* puVol = s_pu;
    const GLSHADER* const pshVol = PshVol(s_iprogGlob, &puVol);
    const bool fVolProg = pshVol != nullptr;
    const GLuint progVol = fVolProg ? pshVol->ID : progGlob;
    const UNIFORMS& uVol = *puVol;
    const bool fCCW = FrontCCW() == GL_CCW;
    const uint64_t alpha = (uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u;
    // (the step k of the textured / untextured sequence; key: program, step, winding, blend, SLY2N_VOLVA; the colour
    // draws 2, 4 and 6 with glGlobShaderVol where built, 7 the GL state the glob program's step 6 leaves)
    auto state = [&](int k) {
        const bool fColor = fVolProg && (k == 2 || k == 4 || k == 6);
        const GLuint prog = fColor ? progVol : progGlob;
        const UNIFORMS& u = fColor ? uVol : *s_pu;
        const int kState = k == 7 ? 6 : k;
        const uint64_t key = (uint64_t(prog) << 32) | (uint64_t(k) << 8) | (fCCW ? 1u : 0u) | ((fl & 1) ? 2u : 0u) | (s_fVa ? 4u : 0u);
        return PacketStateFor(pk, key, [&](State& ps) {
            ps.program = prog;
            ps.frontCCW = fCCW;
            ps.depthTest = 1, ps.depthWrite = 0, ps.depthCompare = kGreater;
            ps.stencilTest = 1, ps.stencilWriteMask = 0x80, ps.stencilCompareMask = 0x80;
            auto add = [&](GLint loc, uint32_t bits) { if (loc >= 0) ps.aconst[ps.cconst++] = {loc, bits}; };
            // (g_fBlend1: glob.frag's CANE_BLEND1 premultiplying off: these blends are the plain GL ones (PacketBlendOf),
            // and the uniform otherwise keeps the last pass run's mode: the colour times As twice, the beams dim)
            add(u.fTextured, 0), add(u.ate, 0), add(u.afailMode, 0), add(u.caneBlend1, 0);
            switch (kState)
            {
            case 0: case 1:   // (untextured: the mask, z-fail through the back faces, then cleared through the front)
                ps.cull = kState == 0 ? 2 : 1;
                ps.colorMask = 0;
                ps.stencilCompare = kAlways, ps.stencilRef = 0x80;
                ps.stencilDepthFail = kState == 0 ? kStencilReplace : kStencilZero;
                break;
            case 2: case 3:   // (the fill through the back faces, no depth test; 3: the GL state left after it)
                ps.cull = kState == 2 ? 2 : 1;
                ps.depthTest = kState == 3;
                ps.stencilTest = kState == 2;
                if (kState == 3) ps.stencilWriteMask = 0xFF;
                ps.stencilCompare = kEqual, ps.stencilRef = 0x80;
                ps.stencilPass = kStencilZero;
                PacketBlendOf(alpha, ps);
                add(u.icolor, s_fVa ? 3 : 2);
                break;
            case 4: case 5:   // (textured: the front faces in front of the scene once per pixel, then the bit cleared)
                ps.cull = 1;
                ps.colorMask = kState == 4 ? 0xF : 0;
                ps.stencilCompare = kState == 4 ? kNotEqual : kAlways, ps.stencilRef = kState == 4 ? 0x80 : 0;
                ps.stencilPass = kState == 4 ? kStencilReplace : kStencilZero;
                PacketBlendOf(alpha, ps);
                add(u.icolor, 3);
                break;
            default:          // (6: the textured surface, blended, no Z write)
                ps.cull = 1;
                ps.stencilTest = 0, ps.stencilWriteMask = 0xFF;
                ps.depthCompare = kGreaterEqual;
                ps.stencilCompare = kAlways, ps.stencilRef = 0;
                ps.stencilPass = kStencilZero;
                PacketBlendOf(alpha, ps);
                ps.cconst = 0;
                add(u.fTextured, 1), add(u.ate, 0), add(u.afailMode, 0), add(u.icolor, 0), add(u.caneBlend1, 0);
                break;
            }
        });
    };
    glm::vec4 c;
    for (int i = 0; i < 4; ++i) c[i] = float(((pshd->argba[1] >> (8 * i)) & 0xFF) + 1) / 2.0f;
    auto rgbaConst = [&]() {
        pk.setConstant(progGlob, s_pu->rgbaConst, glm::value_ptr(c), 16);
        s_aglobinh[s_iprogGlob].rgbaConst = s_globinhLast.rgbaConst = c;
        if (!fVolProg) return;
        // (glGlobShaderVol: the colour and the pass's values the glob programs have from useGlob)
        auto vec = [&](GLint loc, const float* pf, uint32_t cb) { if (loc >= 0) pk.setConstant(progVol, loc, pf, cb); };
        auto one = [&](GLint loc, int32_t v) { if (loc >= 0) pk.setConstant(progVol, loc, &v, 4); };
        vec(uVol.rgbaConst, glm::value_ptr(c), 16);
        vec(uVol.matWorldToClip, glm::value_ptr(s_volframe.matWorldToClip), 64);
        vec(uVol.posCamera, glm::value_ptr(s_volframe.posCamera), 12);
        vec(uVol.dirCamera, glm::value_ptr(s_volframe.dirCamera), 12);
        vec(uVol.fogColor, glm::value_ptr(s_volframe.fogColor), 16);
        one(uVol.fFogBlack, s_globinhLast.fFogBlack);
        one(uVol.tex, 0);
        one(uVol.texDate, 1);
        one(uVol.fDate, 0);
    };
    if (!(fl & 2))
    {
        const uint32_t aid[4] = {state(0), state(1), state(2), state(3)};
        if (!aid[0] || !aid[1] || !aid[2] || !aid[3]) return false;
        DrawOnePacket(pk, aid[0], cmd);
        DrawOnePacket(pk, aid[1], cmd);
        rgbaConst();
        DrawOnePacket(pk, aid[2], cmd, &uVol);
        pk.applyState(aid[3]);
        return true;
    }
    const uint32_t aid[4] = {state(4), state(5), state(6), fVolProg ? state(7) : 1u};
    if (!aid[0] || !aid[1] || !aid[2] || !aid[3]) return false;
    rgbaConst();
    DrawOnePacket(pk, aid[0], cmd, &uVol);
    DrawOnePacket(pk, aid[1], cmd);
    const TEXSEL sel = TexselFromShd(pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
    const GLuint tex = TexFromSel(sel);
    const GLuint sampler = s_asampler[SamplerFor(tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0)))];
    if (pk.bindTexture) pk.bindTexture(0, tex, 0, sampler);   // (the active unit is 0 here)
    else
    {
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(0, sampler);
    }
    DrawOnePacket(pk, aid[2], cmd, &uVol);
    if (fVolProg) pk.applyState(aid[3]);   // (the glob program's state, as the GL calls leave it)
    return true;
}

// Shader kind 3 (rp 11 / 13): a projected volume. 0x1EB960's four lists use the destination alpha's bit 7 as a
// mask (two untextured passes over the volume, a DATE-tested fill with (rgba1 + 1) / 2, a last pass with rgba2);
// as in Sly1-Release's DrawProjVolume the mask is the stencil's bit 7: set where the scene is in front of the
// volume's back faces, cleared where it is in front of its front faces, then filled where it stays set.
void DrawProjVolume(const CMD& cmd)
{
    const SUBGLOB& sub = *cmd.psub;
    const SHD* pshd = sub.pshd;
    const uint8_t fl = pshd->grfshd;
    // (debug: SLY2N_VOLPRINT=1: each volume drawn, its object, shader and place)
    if (static const bool s_fDbg = std::getenv("SLY2N_VOLPRINT") != nullptr; s_fDbg)
        std::printf("vol: oid %d glob %zu kind %d shd %d grfshd 0x%X tris %u rgba %08X %08X %08X at %.0f %.0f %.0f\n",
                    cmd.prec->psgs ? cmd.prec->psgs->oid : -1, cmd.prec->iglob, cmd.prec->kind, pshd->id, unsigned(fl), cmd.count / 3,
                    pshd->argba[0], pshd->argba[1], pshd->argba[2], (*cmd.prec->pmat)[3].x, (*cmd.prec->pmat)[3].y, (*cmd.prec->pmat)[3].z);
    if (const cane::draw::Packets* ppk = PpacketsSpecial(); ppk && FDrawProjVolumePackets(*ppk, cmd)) { s_progPending = 0; return; }
    FlushProgramPending();
    glBindVertexArray(cmd.vao);
    glUniform1i(s_pu->fTextured, 0);
    glUniform1i(s_pu->ate, 0);
    glUniform1i(s_pu->afailMode, 0);
    if (s_pu->caneBlend1 >= 0) glUniform1i(s_pu->caneBlend1, 0);   // (as the packets above: the plain GL blends below)
    glm::vec4 c;
    for (int i = 0; i < 4; ++i) c[i] = float(((pshd->argba[1] >> (8 * i)) & 0xFF) + 1) / 2.0f;
    if (!(fl & 2))
    {
        // An untextured volume (the murk of rp 13, e.g. the intro's rooftop 150055): the mask as the original's lists
        // make it, so only the scene inside the volume is filled (the gang's silhouettes in the dark), not the
        // volume's faces in front of everything: bit 7 set where the scene is in front of a back face (z-fail),
        // cleared where it is in front of a front face, filled (and cleared) where it stays set.
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0x80);
        glEnable(GL_CULL_FACE);
        glFrontFace(FrontCCW());
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GREATER);
        glDisable(GL_BLEND);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_ALWAYS, 0x80, 0x80);
        glCullFace(GL_FRONT);
        glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
        DrawOne(cmd);
        glCullFace(GL_BACK);
        glStencilOp(GL_KEEP, GL_ZERO, GL_KEEP);
        DrawOne(cmd);
        // the fill through the back faces, without the depth test: the camera may be inside the volume (no front face
        // in view), and a back face covers every pixel whose scene point can be inside
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_DEPTH_TEST);
        glCullFace(GL_FRONT);
        glStencilFunc(GL_EQUAL, 0x80, 0x80);
        glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
        glEnable(GL_BLEND);
        FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
        // (debug: SLY2N_VOLDBG=1: the filled pixels in a colour per volume, red, green, blue, ... in draw order)
        static const bool s_fVolDbg = std::getenv("SLY2N_VOLDBG") != nullptr;
        static int s_ivolDbg = 0;
        static const float s_argbDbg[8][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}, {255, 128, 0}};
        if (s_fVolDbg) { const float* prgb = s_argbDbg[s_ivolDbg++ % 8]; SetGlobRgbaConst(prgb[0], prgb[1], prgb[2], 64.0f); }
        else SetGlobRgbaConst(c.r, c.g, c.b, c.a);
        // (the lists' DATE fill is a flat sprite of (rgba1 + 1) / 2, not faded by the hull's vertex alpha: SLY2N_VOLVA=1
        // fades it for comparison)
        static const bool s_fVa = std::getenv("SLY2N_VOLVA") != nullptr;
        glUniform1i(s_pu->icolor, s_fVa ? 3 : 2);
        DrawOne(cmd);
        glCullFace(GL_BACK);
        glEnable(GL_DEPTH_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        return;
    }
    // A textured volume (light beams): the volume's front faces in front of the scene, filled once per pixel
    // (stencil bit 7), with rgba1 / 2 faded by the vertex alpha; then the bit is cleared again. (The exact P_G
    // 0x3628 / 0x37A8 passes are not reconstructed; this matches the beams PCSX2 shows.)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x80);
    glEnable(GL_CULL_FACE);
    glFrontFace(FrontCCW());
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_NOTEQUAL, 0x80, 0x80);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glEnable(GL_BLEND);
    FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
    SetGlobRgbaConst(c.r, c.g, c.b, c.a);
    glUniform1i(s_pu->icolor, 3);
    DrawOne(cmd);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 0, 0x80);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
    DrawOne(cmd);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    if (fl & 2)
    {
        // The textured surface of the volume (list 0 with flag 2): blended, no Z write.
        glFrontFace(FrontCCW());
        glDepthFunc(GL_GEQUAL);
        const TEXSEL sel = TexselFromShd(pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
        const GLuint tex = TexFromSel(sel);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(0, s_asampler[SamplerFor(tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0)))]);
        glUniform1i(s_pu->fTextured, 1);
        glUniform1i(s_pu->icolor, 0);
        FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
        DrawOne(cmd);
    }
}

// Whether a point (the hull's space) is inside a type-3 hull: a ray's crossings of its triangles (as BuildGlobGpu
// makes them), odd inside. (The list-1 caps the original adds where the near plane is inside the hull.)
// A type-3 hull's triangles in its own space (as BuildGlobGpu makes them), kept per glob.
const std::vector<glm::vec3>& HullTriangles(const GLOB& glob)
{
    static std::unordered_map<const GLOB*, std::vector<glm::vec3>> s_mpatri;
    auto [it, fNew] = s_mpatri.try_emplace(&glob);
    std::vector<glm::vec3>& atri = it->second;
    if (fNew)
        for (const SUBFX& fx : glob.asubfx)
            for (size_t k = 2; k < fx.aipos.size(); ++k)
            {
                if ((fx.amask[k / 32] >> (k % 32)) & 1) continue;
                for (size_t j = k - 2; j <= k; ++j) atri.push_back(fx.aipos[j] < fx.apos.size() ? fx.apos[fx.aipos[j]] : glm::vec3(0.0f));
            }
    return atri;
}

bool FPointInHull(const GLOB& glob, const glm::vec3& pos)
{
    const std::vector<glm::vec3>& atri = HullTriangles(glob);
    const glm::vec3 dir = glm::normalize(glm::vec3(0.1234f, 0.4567f, 0.8812f));
    int c = 0;
    for (size_t i = 0; i + 2 < atri.size(); i += 3)
    {
        const glm::vec3 e1 = atri[i + 1] - atri[i], e2 = atri[i + 2] - atri[i];
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-12f) continue;
        const glm::vec3 t = pos - atri[i];
        const float u = glm::dot(t, p) / det;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 q = glm::cross(t, e1);
        const float v = glm::dot(dir, q) / det;
        if (v < 0.0f || u + v > 1.0f) continue;
        if (glm::dot(e2, q) / det > 0.0f) ++c;
    }
    return c & 1;
}

// Whether the segment a..b crosses the triangle (p0, p1, p2).
bool FSegmentHitsTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2)
{
    const glm::vec3 d = b - a, e1 = p1 - p0, e2 = p2 - p0;
    const glm::vec3 p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const glm::vec3 t = a - p0;
    const float u = glm::dot(t, p) / det;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(t, e1);
    const float v = glm::dot(d, q) / det;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float s = glm::dot(e2, q) / det;
    return s >= 0.0f && s <= 1.0f;
}

// Whether the drawn near plane (its four corners, the hull's space) is inside the hull anywhere: a hull triangle cuts
// the near rectangle (a triangle's edge through it or the rectangle's edge through the triangle), or the rectangle lies
// inside (its centre, ray parity).
bool FNearPlaneInHull(const GLOB& glob, const std::array<glm::vec3, 4>& apos)
{
    const std::vector<glm::vec3>& atri = HullTriangles(glob);
    for (size_t i = 0; i + 2 < atri.size(); i += 3)
    {
        const glm::vec3* pt = &atri[i];
        for (int k = 0; k < 3; ++k)
            if (FSegmentHitsTriangle(pt[k], pt[(k + 1) % 3], apos[0], apos[1], apos[2]) ||
                FSegmentHitsTriangle(pt[k], pt[(k + 1) % 3], apos[0], apos[2], apos[3]))
                return true;
        for (int k = 0; k < 4; ++k)
            if (FSegmentHitsTriangle(apos[size_t(k)], apos[size_t((k + 1) % 4)], pt[0], pt[1], pt[2])) return true;
    }
    return FPointInHull(glob, (apos[0] + apos[1] + apos[2] + apos[3]) * 0.25f);
}

// The hull's DATE (TEST DATE without the shader's flag 8).
bool FHullDate(const CMD& cmd) { return !(cmd.prec->pglobGeom->asubfx[0].pshd->grfshd & 8); }

// Shader kind 2 on rp 7 / 8: shadow hulls. The original marks the scene inside them in the frame's green channel
// (0x16DA48 fills it with 0x7E / 0x3E; the hull's faces in front of the scene (list 0, TEST GREATER) subtract their
// colour, +2 or -2 (254) by their facing: the count goes up by one per front face and down by one per back face in
// front of the scene; DATE without the shader's flag 8; the faces the camera's near plane cuts add theirs through list 1,
// ZTST ALWAYS; reference GS log, render.md "shadow frame-channel pass"), and 0x16DD58 darkens the marked pixels by the
// scene's own alpha (ShadowCombine). Natively the same count in the stencil: z-pass (front +1, back -1 where they pass
// the depth test), or z-fail when the drawn near plane is inside the hull anywhere (list 1's near-plane caps: the same
// count for a closed hull). Only the hulls the near plane reaches: a z-fail count for every hull near the camera marked
// open hulls' insides the original leaves alone; a camera-inside test missed the hulls the near plane cuts with the
// camera just outside (R47: a screen-tall unmarked band, the hull's part behind the near plane uncounted).
// fDateMarked: the stencil's STENCIL_FBA holds the phase's DATE (FMarkDate), so the program need not test it.
// pposCamera1 / paposNear1 (two views): the second eye's: z-fail when either eye's near plane reaches the hull.
void DrawShadow(const CMD& cmd, const glm::vec3& posCamera0, float sNearClip, const std::array<glm::vec3, 4>& aposNear0, bool fDateMarked,
                const glm::vec3* pposCamera1 = nullptr, const std::array<glm::vec3, 4>* paposNear1 = nullptr)
{
    const GLOB& geom = *cmd.prec->pglobGeom;
    const GLuint maskCount = cmd.prec->kind == 8 ? STENCIL_COUNT8 : STENCIL_COUNT7;
    bool fNear = false;
    for (int iv = 0; iv < (pposCamera1 && paposNear1 ? 2 : 1) && !fNear; ++iv)
    if (geom.pgl && geom.pgl->sRadius > 0.0f)
    {
        const glm::vec3& posCamera = iv ? *pposCamera1 : posCamera0;
        const std::array<glm::vec3, 4>& aposNear = iv ? *paposNear1 : aposNear0;
        const glm::mat4& mat = *cmd.prec->pmat;
        const float scale = std::max({glm::length(glm::vec3(mat[0])), glm::length(glm::vec3(mat[1])), glm::length(glm::vec3(mat[2]))});
        const glm::vec3 c = glm::vec3(mat * glm::vec4(geom.pgl->posCenter, 1.0f));
        float sNearExtent = sNearClip;
        for (const glm::vec3& pos : aposNear) sNearExtent = std::max(sNearExtent, glm::distance(posCamera, pos));
        if (glm::distance(posCamera, c) < geom.pgl->sRadius * scale + sNearExtent)
        {
            const glm::mat4 matInv = glm::inverse(mat);
            std::array<glm::vec3, 4> apos;
            for (size_t i = 0; i < 4; ++i) apos[i] = glm::vec3(matInv * glm::vec4(aposNear[i], 1.0f));
            static const bool s_fCamOnly = std::getenv("SLY2N_SHNEARCAM") != nullptr;   // (debug: the camera-inside test only)
            fNear = s_fCamOnly ? glm::distance(posCamera, c) < geom.pgl->sRadius * scale + sNearClip &&
                                     FPointInHull(geom, glm::vec3(matInv * glm::vec4(posCamera, 1.0f)))
                               : FNearPlaneInHull(geom, apos);
        }
    }
    static const int s_iShMode = std::getenv("SLY2N_SHCOUNT") ? std::atoi(std::getenv("SLY2N_SHCOUNT")) : 0;   // (debug: 1 z-pass, 2 z-fail)
    if (s_iShMode) fNear = s_iShMode == 2;
    const bool fDate = FHullDate(cmd);
    if (static const bool s_fDbg = std::getenv("SLY2N_SHHULLDBG") != nullptr; s_fDbg)
        std::printf("shhull: oid %d glob %zu kind %d grfshd 0x%X near %d tris %u r %.0f\n", cmd.prec->psgs ? cmd.prec->psgs->oid : -1, cmd.prec->iglob,
                    cmd.prec->kind, unsigned(geom.asubfx[0].pshd->grfshd), int(fNear), cmd.count / 3, geom.pgl ? geom.pgl->sRadius : -1.0f);
    if (const cane::draw::Packets* ppk = PpacketsSpecial())
    {
        // (draw packets: the count's state, then the GL state the calls below leave after it)
        using namespace cane::draw;
        const GLuint prog = s_apshGlob[s_iprogGlob]->ID;
        const UNIFORMS& u = *s_pu;
        const bool fCCW = FrontCCW() == GL_CCW;
        const int fDateU = fDate && !fDateMarked ? 1 : 0;
        uint32_t aid[2];
        for (int k = 0; k < 2; ++k)
        {
            const uint64_t key = (uint64_t(prog) << 32) | (uint64_t(k) << 8) | (fCCW ? 1u : 0u) | (fDate ? 2u : 0u) | (fDateU ? 4u : 0u) |
                                 (fNear ? 8u : 0u) | (maskCount == STENCIL_COUNT8 ? 16u : 0u);
            aid[k] = PacketStateFor(*ppk, key, [&](State& ps) {
                ps.program = prog;
                ps.winding = 1, ps.frontCCW = fCCW;
                ps.depthTest = 1, ps.depthWrite = 0, ps.depthCompare = kGreater;
                ps.colorMask = k ? 0xF : 0;
                ps.stencilTest = !k, ps.stencilWriteMask = uint8_t(k ? 0xFFu : maskCount);
                ps.stencilCompare = kEqual, ps.stencilRef = 0, ps.stencilCompareMask = uint8_t(fDate ? STENCIL_FBA : 0);
                ps.stencilBack = 1;
                if (fNear) ps.stencilDepthFail = kStencilDecrWrap, ps.stencilBackDepthFail = kStencilIncrWrap;
                else ps.stencilPass = kStencilIncrWrap, ps.stencilBackPass = kStencilDecrWrap;
                auto add = [&](GLint loc, uint32_t bits) { if (loc >= 0) ps.aconst[ps.cconst++] = {loc, bits}; };
                add(u.fTextured, 0), add(u.ate, 0), add(u.afailMode, 0), add(u.icolor, 2), add(u.fDate, k ? 0u : uint32_t(fDateU));
            });
        }
        if (aid[0] && aid[1])
        {
            const glm::vec4 rgba0(0.0f);
            ppk->setConstant(prog, u.rgbaConst, glm::value_ptr(rgba0), 16);
            s_aglobinh[s_iprogGlob].rgbaConst = s_globinhLast.rgbaConst = rgba0;
            DrawOnePacket(*ppk, aid[0], cmd);
            ppk->applyState(aid[1]);
            s_progPending = 0;   // (bound by the packets)
            return;
        }
    }
    FlushProgramPending();
    glBindVertexArray(cmd.vao);
    glUniform1i(s_pu->fTextured, 0);
    glUniform1i(s_pu->ate, 0);
    glUniform1i(s_pu->afailMode, 0);
    SetGlobRgbaConst(0, 0, 0, 0);
    glUniform1i(s_pu->icolor, 2);
    // (DATE: the alpha's bit 7 clear: the FBA bit, or alpha >= 0x80; with the mark, the stencil bit alone)
    glUniform1i(s_pu->fDate, fDate && !fDateMarked ? 1 : 0);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(maskCount);
    glStencilFunc(GL_EQUAL, 0, fDate ? STENCIL_FBA : 0);
    if (fNear)
    {
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    }
    else
    {
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glFrontFace(FrontCCW());
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    DrawOne(cmd);
    glUniform1i(s_pu->fDate, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

// 0x16DA48 (AddScreenEffect) at a shadow phase's start: the frame as the scene left it (the original copies its green
// plane away, 0x16D6C8 0x299, and fills it with the marks' base); natively its alpha into a texture the hulls' DATE
// and the combine read (resolved when multisampled).
GLuint s_fboShAlpha = 0, s_texShAlpha = 0;
int s_dxShAlpha = 0, s_dyShAlpha = 0;
// (VR multiview: the two views' copies, a two-layer array, from the eyes' image g_texMultiviewColor)
GLuint s_texShAlphaMv = 0, s_afboShAlphaMv[2] = {}, s_fboShAlphaMvSrc = 0;
int s_dxShAlphaMv = 0, s_dyShAlphaMv = 0;
GLuint g_texMultiviewColor = 0;
// (Vulkan VR multiview: the two views' copies side by side in one texture (view 1 at x + dx: the two-view programs add
// CaneViews' offset to their texel), so the programs keep their 2D samplers)
GLuint s_texShAlphaViews = 0, s_fboShAlphaViews = 0;
int s_dxShAlphaViews = 0, s_dyShAlphaViews = 0;
GLuint ShadowAlphaTexture()
{
    if (!g_fMultiviewPass) return s_texShAlpha;
    return FRendererVulkan() ? s_texShAlphaViews : s_texShAlphaMv;
}
void BindShadowAlpha()
{
    if (const cane::draw::Packets* pk = PpacketsPhase(); pk && FRendererVulkan())
    {
        pk->bindTexture(1, ShadowAlphaTexture(), 0, cane::draw::kSamplerKept);
        return;
    }
    glActiveTexture(GL_TEXTURE1);
    if (g_fMultiviewPass && !FRendererVulkan()) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texShAlphaMv);
    else glBindTexture(GL_TEXTURE_2D, ShadowAlphaTexture());
    glActiveTexture(GL_TEXTURE0);
}
#if SLY2_FASTPATHS
extern std::array<int, 4> s_rectShPhase;
#endif
// (native framebuffers, draw_packet.h: a shadow-alpha target at a new size: GL's glTexImage2D on the attached texture
// becomes a new texture (one level, nearest, zeros) attached in the old one's place, the old one deleted; no binding)
bool FRedefineShAlphaNative(GLuint& tex, GLuint& fbo, int dx, int dy)
{
    const cane::draw::Packets* pkf = PpacketsFbo();
    if (!pkf) return false;
    GlDeleteTexture(tex);
    cane::draw::TextureDesc d;
    d.width = uint32_t(dx), d.height = uint32_t(dy), d.storage = 0;
    d.minFilter = d.magFilter = cane::draw::kFilterNearest;
    tex = pkf->createTexture(d);
    cane::draw::Attachment c;
    c.object = tex;
    if (fbo) pkf->attach(fbo, cane::draw::kClearColor, c);
    else fbo = pkf->createFramebuffer(c, cane::draw::Attachment{});
    return true;
}
void CopyShadowAlphaViews(GLuint fbo, int dx, int dy)
{
    if ((!s_texShAlphaViews || dx != s_dxShAlphaViews || dy != s_dyShAlphaViews) && FRedefineShAlphaNative(s_texShAlphaViews, s_fboShAlphaViews, 2 * dx, dy))
        s_dxShAlphaViews = dx, s_dyShAlphaViews = dy;
    if (!s_texShAlphaViews || dx != s_dxShAlphaViews || dy != s_dyShAlphaViews)
    {
        if (!s_texShAlphaViews) { glGenTextures(1, &s_texShAlphaViews); glGenFramebuffers(1, &s_fboShAlphaViews); }
        if (!s_fboShAlphaMvSrc) glGenFramebuffers(1, &s_fboShAlphaMvSrc);
        glBindTexture(GL_TEXTURE_2D, s_texShAlphaViews);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2 * dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlphaViews);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texShAlphaViews, 0);
        s_dxShAlphaViews = dx, s_dyShAlphaViews = dy;
    }
    const cane::draw::Packets* pk = PpacketsPhase();
    if (pk && pk->copyLayer && pk->bindTarget)
    {
        // (draw packets: each view's layer copied straight from the texture, the frame's framebuffer bound after)
        for (int l = 0; l < 2; ++l)
        {
#if SLY2_FASTPATHS
            const std::array<int, 4>& r = s_rectShPhase;
            if (r[2] > 0 && r[3] > 0) pk->copyLayer(g_texMultiviewColor, uint32_t(l), s_fboShAlphaViews, r.data(), l * dx + r[0], r[1], fbo);
            else pk->bindTarget(fbo);
#else
            const int32_t r[4] = {0, 0, dx, dy};
            pk->copyLayer(g_texMultiviewColor, uint32_t(l), s_fboShAlphaViews, r, l * dx, 0, fbo);
#endif
        }
        return;
    }
    for (int l = 0; l < 2; ++l)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboShAlphaMvSrc);
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, g_texMultiviewColor, 0, l);
        if (pk)
        {
            // (draw packets: the view's copy; the GL calls' framebuffer at the end bound after each)
#if SLY2_FASTPATHS
            const std::array<int, 4>& r = s_rectShPhase;
            if (r[2] > 0 && r[3] > 0) pk->copy(s_fboShAlphaMvSrc, s_fboShAlphaViews, r.data(), l * dx + r[0], r[1], fbo);
            else if (pk->bindTarget) pk->bindTarget(fbo);
            else glBindFramebuffer(GL_FRAMEBUFFER, fbo);
#else
            const int32_t r[4] = {0, 0, dx, dy};
            pk->copy(s_fboShAlphaMvSrc, s_fboShAlphaViews, r, l * dx, 0, fbo);
#endif
            continue;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlphaViews);
#if SLY2_FASTPATHS
        const std::array<int, 4>& r = s_rectShPhase;   // (the phase's volumes' rectangle: both views')
        if (r[2] > 0 && r[3] > 0)
            glBlitFramebuffer(r[0], r[1], r[0] + r[2], r[1] + r[3], l * dx + r[0], r[1], l * dx + r[0] + r[2], r[1] + r[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
#else
        glBlitFramebuffer(0, 0, dx, dy, l * dx, 0, l * dx + dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    }
    if (!pk) glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}
void CopyShadowAlphaMultiview(GLuint fbo, int dx, int dy)
{
    if (!s_texShAlphaMv || dx != s_dxShAlphaMv || dy != s_dyShAlphaMv)
    {
        if (s_texShAlphaMv) { glDeleteTextures(1, &s_texShAlphaMv); glDeleteFramebuffers(2, s_afboShAlphaMv); }
        glGenTextures(1, &s_texShAlphaMv);
        glBindTexture(GL_TEXTURE_2D_ARRAY, s_texShAlphaMv);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, dx, dy, 2);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(2, s_afboShAlphaMv);
        for (int l = 0; l < 2; ++l)
        {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_afboShAlphaMv[l]);
            glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_texShAlphaMv, 0, l);
        }
        if (!s_fboShAlphaMvSrc) glGenFramebuffers(1, &s_fboShAlphaMvSrc);
        s_dxShAlphaMv = dx, s_dyShAlphaMv = dy;
    }
    for (int l = 0; l < 2; ++l)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboShAlphaMvSrc);
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, g_texMultiviewColor, 0, l);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_afboShAlphaMv[l]);
        glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}
GLSHADER s_shShadowCombine;
// (tile GPUs) A shadow phase's frame alpha read in place by framebuffer fetch (shadowdate.frag / shadowcombine.frag
// CANE_FETCH) instead of copied: ShadowBegin's copy ends the tile pass in the middle of the frame (the frame written out,
// copied and read back in). Only for the phases in which nothing writes the frame's alpha (RenderPass marks them: the
// frame's alpha is then at every read what the copy would hold), not in the two-view pass. On OpenGL ES where the driver
// has framebuffer fetch; elsewhere the copy (SLY2N_SHFETCH=0: the copy everywhere; =1 also tries the fetch on desktop
// OpenGL, a test). Multisampled, the fetch reads each sample where the copy held the pixel's resolved value.
// On the Vulkan renderer: the attachment as an input attachment (VK_KHR_dynamic_rendering_local_read, single-sampled
// targets: FVulkanFramebufferFetch), on Android, or anywhere with SLY2N_SHFETCH=1.
// Mode: 0 not decided, 1 EXT_shader_framebuffer_fetch, 2 its non-coherent form, 3 QCOM's non-coherent one, 4 Vulkan's
// local read, -1 the copy.
bool s_fGpuMarks = false;                  // (measurement: renderer.h GpuMark)
const char* s_szGpuMarkLast = nullptr;
int s_modeShFetch = 0;
int s_dxShFrame = 0, s_dyShFrame = 0;          // (the open phase's frame: the combine's alpha clear)
GLSHADER s_shShadowDateFetch, s_shShadowCombineFetch;
typedef void (*PFNFETCHBARRIER)();
PFNFETCHBARRIER s_pfnFetchBarrier = nullptr;   // (the non-coherent forms: before a draw reads what earlier draws wrote)
bool s_fShFetchPhase = false;                  // the open phase reads the frame in place
int ShadowFetchMode()
{
    if (s_modeShFetch) return s_modeShFetch;
    s_modeShFetch = -1;
#ifndef __SWITCH__
    const char* sz = std::getenv("SLY2N_SHFETCH");
    // (Vulkan on a tile GPU (every Android device, the Steam Frame, Windows on ARM): the local read by default; elsewhere,
    // and OpenGL ES's framebuffer fetch, only on request (SLY2N_SHFETCH=1): on an Adreno 840 (driver V@0842.19.13) the
    // GLES fetch faulted the GPU in Prague and on the train and the game process died)
    const bool fWant = FVulkanTileGpu() ? !(sz && *sz && std::atoi(sz) == 0) : (sz && std::atoi(sz) != 0);
    if (!fWant) return s_modeShFetch;
    if (FRendererVulkan())
    {
        if (s_shShadowDateFetch.FInit("shadowcombine.vert", "shadowdate.frag", "#define CANE_FETCH 1\n") &&
            s_shShadowCombineFetch.FInit("shadowcombine.vert", "shadowcombine.frag", "#define CANE_FETCH 1\n"))
            s_modeShFetch = 4;
        std::printf("Vulkan: shadow phases read the frame %s\n", s_modeShFetch > 0 ? "in place (dynamic rendering local read)" : "from a copy");
        return s_modeShFetch;
    }
    bool afExt[4] = {};
    static const char* const s_aszExt[4] = {nullptr, "GL_EXT_shader_framebuffer_fetch", "GL_EXT_shader_framebuffer_fetch_non_coherent",
                                            "GL_QCOM_shader_framebuffer_fetch_noncoherent"};
    GLint cext = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &cext);
    for (GLint i = 0; i < cext; ++i)
        if (const char* p = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i))))
            for (int m = 1; m < 4; ++m) afExt[m] = afExt[m] || !std::strcmp(p, s_aszExt[m]);
    static const char* const s_aszBarrier[4] = {nullptr, nullptr, "glFramebufferFetchBarrierEXT", "glFramebufferFetchBarrierQCOM"};
    for (int m = 1; m < 4 && s_modeShFetch < 0; ++m)
    {
        if (!afExt[m]) continue;
        PFNFETCHBARRIER pfn = nullptr;
        if (s_aszBarrier[m] && !(pfn = reinterpret_cast<PFNFETCHBARRIER>(cane::gl::ProcAddress(s_aszBarrier[m])))) continue;
        const std::string prefix = std::string("#extension ") + s_aszExt[m] + " : require\n#define CANE_FETCH 1\n" + (m > 1 ? "#define CANE_FETCH_NC 1\n" : "");
        if (s_shShadowDateFetch.FInit("shadowcombine.vert", "shadowdate.frag", prefix.c_str()) &&
            s_shShadowCombineFetch.FInit("shadowcombine.vert", "shadowcombine.frag", prefix.c_str()))
        {
            s_modeShFetch = m;
            s_pfnFetchBarrier = pfn;
        }
        else
        {
            if (s_shShadowDateFetch.ID) s_shShadowDateFetch.Delete();
            if (s_shShadowCombineFetch.ID) s_shShadowCombineFetch.Delete();
        }
    }
    std::printf("OpenGL: shadow phases read the frame %s%s\n", s_modeShFetch > 0 ? "in place: " : "from a copy", s_modeShFetch > 0 ? s_aszExt[s_modeShFetch] : "");
#endif
    return s_modeShFetch;
}
#if SLY2_FASTPATHS
// (the Switch) The frame rectangle a shadow phase's volumes can reach (x, y, width, height; set by RenderPass at the
// phase's start): the frame copy, the DATE mark and the combine's ramps only there (the marks are under the volumes).
std::array<int, 4> s_rectShPhase = {0, 0, 0, 0};
#endif
// fFetch: the phase may read the frame's alpha in place (s_modeShFetch): no copy
void ShadowBegin(GLuint fbo, int dx, int dy, int kind, bool fFetch)
{
    const cane::draw::Packets* pk = PpacketsPhase();
    if (pk && pk->bindTarget) pk->bindTarget(fbo);
    else glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (fFetch && s_modeShFetch == 4) fFetch = FVulkanFramebufferFetch();   // (Vulkan: a target the layer can read in place)
    s_fShFetchPhase = fFetch;
    s_dxShFrame = dx, s_dyShFrame = dy;
    if (fFetch) {}
    else if (g_fMultiviewPass && FRendererVulkan()) CopyShadowAlphaViews(fbo, dx, dy);
    else if (g_fMultiviewPass) CopyShadowAlphaMultiview(fbo, dx, dy);
    else if ((!s_fboShAlpha || dx != s_dxShAlpha || dy != s_dyShAlpha) && FRedefineShAlphaNative(s_texShAlpha, s_fboShAlpha, dx, dy))
        s_dxShAlpha = dx, s_dyShAlpha = dy;
    else if (!s_fboShAlpha || dx != s_dxShAlpha || dy != s_dyShAlpha)
    {
        if (!s_fboShAlpha) { glGenFramebuffers(1, &s_fboShAlpha); glGenTextures(1, &s_texShAlpha); }
        glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlpha);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texShAlpha, 0);
        s_dxShAlpha = dx;
        s_dyShAlpha = dy;
    }
    if (pk)
    {
        // (draw packets: the scissor off, the frame's copy, the texture, the count's base as a masked clear)
        pk->scissor(nullptr);
        if (!g_fMultiviewPass && !fFetch)
        {
#if SLY2_FASTPATHS
            const std::array<int, 4>& r = s_rectShPhase;
            if (r[2] > 0 && r[3] > 0) pk->copy(fbo, s_fboShAlpha, r.data(), r[0], r[1], fbo);
            else if (pk->bindTarget) pk->bindTarget(fbo);
            else glBindFramebuffer(GL_FRAMEBUFFER, fbo);
#else
            const int32_t r[4] = {0, 0, dx, dy};
            pk->copy(fbo, s_fboShAlpha, r, 0, 0, fbo);
#endif
        }
        if (!fFetch) BindShadowAlpha();
        if (s_fGpuMarks) GpuMark(s_szGpuMarkLast = "shbegin stencil base");
        cane::draw::Clear c;
        c.clear = cane::draw::kClearStencil;
        c.stencilWriteMask = uint8_t(STENCIL_COUNT7);
        c.stencil = uint8_t(kind == 8 ? 0x1F : 0x3F);
        pk->clear(c);
    }
    else
    {
    glDisable(GL_SCISSOR_TEST);
    if (!g_fMultiviewPass && !fFetch)
    {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlpha);
#if SLY2_FASTPATHS
    {
        const std::array<int, 4>& r = s_rectShPhase;
        if (r[2] > 0 && r[3] > 0)
            glBlitFramebuffer(r[0], r[1], r[0] + r[2], r[1] + r[3], r[0], r[1], r[0] + r[2], r[1] + r[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
#else
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    if (!fFetch) BindShadowAlpha();
    // the count's base (the green fill)
    glStencilMask(STENCIL_COUNT7);
    glClearStencil(kind == 8 ? 0x1F : 0x3F);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    }
    // (debug: SLY2N_SHALPHA=dir writes each phase's frame alpha x 2 as a grey PNG)
    if (static const char* s_szDir = std::getenv("SLY2N_SHALPHA"); s_szDir && !fFetch)
    {
        static int s_c = 0;
        std::vector<uint8_t> ab(size_t(dx) * size_t(dy) * 4), ag(size_t(dx) * size_t(dy));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboShAlpha);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dx, dy, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x) ag[size_t(dy - 1 - y) * size_t(dx) + size_t(x)] = ab[(size_t(y) * size_t(dx) + size_t(x)) * 4 + 3];
        stbi_write_png((std::string(s_szDir) + "/shalpha_" + std::to_string(s_c++) + ".png").c_str(), dx, dy, 1, ag.data(), dx);
    }
}

// A phase's DATE as a stencil mark: STENCIL_FBA also set where the frame's alpha at the phase's start (ShadowBegin's
// copy) is 0x80 or more, the discard's test, so the hulls and type-2 volumes test the bit alone, with programs that
// never discard. False: no mark (the program did not build), the DATE discard instead.
GLSHADER s_shShadowDate;
bool FMarkDate()
{
    static int s_state = 0;   // 0: not tried yet, 1: available, -1: not
    if (s_state == 0)
        s_state = glGlobShaderNoKill.ID && s_shShadowDate.FInit("shadowcombine.vert", "shadowdate.frag", "") ? 1 : -1;
    if (const cane::draw::Packets* pk = PpacketsPhase(); pk && (s_fShFetchPhase || (s_state > 0 && ShadowAlphaTexture())))
    {
        // (draw packets: the mark's state and the one its GL calls leave, made once per program)
        using namespace cane::draw;
        const GLSHADER& sh = s_fShFetchPhase ? s_shShadowDateFetch : s_shShadowDate;
        static GLuint s_prog = 0;
        static GLint s_locTexDate = -1;
        if (s_prog != sh.ID) s_prog = sh.ID, s_locTexDate = sh.Loc("texDate");
        uint32_t aid[2];
        for (int k = 0; k < 2; ++k)
            aid[k] = PacketStateFor(*pk, (uint64_t(sh.ID) << 32) | (uint64_t(0x30 + k) << 8), [&](State& ps) {
                ps.program = sh.ID;
                ps.keep = kKeepDepthCompare;
                ps.depthTest = k ? 1 : 0, ps.depthWrite = 0;
                ps.colorMask = k ? 0xF : 0;
                ps.stencilTest = k ? 0 : 1, ps.stencilWriteMask = uint8_t(k ? 0xFFu : STENCIL_FBA);
                ps.stencilCompare = kAlways, ps.stencilRef = uint8_t(STENCIL_FBA), ps.stencilCompareMask = uint8_t(STENCIL_FBA);
                ps.stencilPass = kStencilReplace;
                if (!s_fShFetchPhase && s_locTexDate >= 0) ps.aconst[ps.cconst++] = {s_locTexDate, 1};
            });
        if (aid[0] && aid[1])
        {
            if (s_fShFetchPhase) { if (s_pfnFetchBarrier) s_pfnFetchBarrier(); }
            else BindShadowAlpha();
#if SLY2_FASTPATHS
            pk->scissor(s_rectShPhase.data());
            pk->drawArrays(aid[0], s_vaoEmpty, 0, 3, nullptr, 0);
            pk->scissor(nullptr);
#else
            pk->drawArrays(aid[0], s_vaoEmpty, 0, 3, nullptr, 0);
#endif
            pk->applyState(aid[1]);
            return true;
        }
    }
    if (s_fShFetchPhase)
    {
        s_shShadowDateFetch.Use();
        if (s_pfnFetchBarrier) s_pfnFetchBarrier();
    }
    else
    {
        if (s_state < 0 || !ShadowAlphaTexture()) return false;
        s_shShadowDate.Use();
        glUniform1i(s_shShadowDate.Loc("texDate"), 1);
        BindShadowAlpha();
    }
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(STENCIL_FBA);
    glStencilFunc(GL_ALWAYS, GLint(STENCIL_FBA), STENCIL_FBA);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
#if SLY2_FASTPATHS
    glEnable(GL_SCISSOR_TEST);   // (the phase's volumes' rectangle: s_rectShPhase)
    glScissor(s_rectShPhase[0], s_rectShPhase[1], s_rectShPhase[2], s_rectShPhase[3]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
#else
    glDrawArrays(GL_TRIANGLES, 0, 3);
#endif
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    return true;
}

// 0x16DA48 for kind 8 after kind 7: the kind-8 count's base, kind 7's mark (bit 6) kept (FBMSK 0xFFFF80FF).
// (draw packets: a stencil clear with the write mask, 0xFF left)
bool FClearStencilPacket(GLuint mask, uint8_t value)
{
    const cane::draw::Packets* pk = PpacketsPhase();
    if (!pk) return false;
    cane::draw::Clear c;
    c.clear = cane::draw::kClearStencil;
    c.stencilWriteMask = uint8_t(mask);
    c.stencil = value;
    pk->clear(c);
    return true;
}
void ShadowKind8()
{
    if (FClearStencilPacket(STENCIL_COUNT8, 0x1F)) return;
    glStencilMask(STENCIL_COUNT8);
    glClearStencil(0x1F);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
}

// Past the FBA kinds without a shadow phase: the stencil's FBA bits cleared (the volumes' mask, kinds 11 / 13, is bit 7).
void ClearFbaBits()
{
    if (FClearStencilPacket(STENCIL_FBA, 0)) return;
    glStencilMask(STENCIL_FBA);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
}

// (draw packets) ShadowCombine below: the unmarked pixels' alpha (a masked clear, or the in-place reset draw), the ramps,
// the marks' clear; each GL state of the sequence a packet state made once per program and variant, the last one the
// state its GL calls leave. False: a state the backend cannot take (the GL calls then; nothing done yet).
bool FShadowCombinePackets(const cane::draw::Packets& pk, const GLSHADER& sh, bool fFetch, bool f7, bool f8, GLuint maskMarks)
{
    using namespace cane::draw;
    struct LOCS { GLuint prog = 0; GLint texAlpha = -1, gain = -1, m8 = -1, reset = -1; };
    static LOCS s_alocs[2];
    LOCS& loc = s_alocs[fFetch ? 1 : 0];
    if (loc.prog != sh.ID)
        loc = {sh.ID, sh.Loc("texAlpha"), sh.Loc("gain"), sh.Loc("m8"), sh.Loc("fReset")};
    // (GAME +0x3E64 / +0x3E70, 0x1EA9F0: 128 / 96 / 160 and black in every world's RAM seen)
    struct RAMP { GLuint stencil; float gain; int m8; bool f; };
    const RAMP aramp[] = {{STENCIL_SH7, 128.0f, 0, f7}, {STENCIL_SH8, 96.0f, 1, f8}, {STENCIL_SH7 | STENCIL_SH8, 160.0f, 1, f7 && f8}};
    int irampLast = -1;
    for (int i = 0; i < 3; ++i) if (aramp[i].f) irampLast = i;
    if (irampLast < 0) return false;
    // steps: 0 the reset draw (in place), 1..3 the ramps, 4 the state left afterwards
    auto state = [&](int k) {
        const uint64_t key = (uint64_t(sh.ID) << 32) | (uint64_t(0x40 + k) << 8) | (f8 ? 1u : 0u) | (f7 ? 2u : 0u) | (uint64_t(irampLast) << 2);
        return PacketStateFor(pk, key, [&](State& ps) {
            ps.program = sh.ID;
            ps.keep = kKeepDepthCompare;
            ps.depthTest = k == 4 ? 1 : 0, ps.depthWrite = 0;
            ps.stencilTest = k == 4 ? 0 : 1, ps.stencilWriteMask = uint8_t(k == 4 ? 0xFFu : 0u);
            auto add = [&](GLint l, uint32_t bits) { if (l >= 0) ps.aconst[ps.cconst++] = {l, bits}; };
            add(loc.texAlpha, 1);
            if (k == 0)
            {
                ps.colorMask = 8;
                if (f7 || f8) ps.stencilCompare = kEqual, ps.stencilRef = 0, ps.stencilCompareMask = uint8_t(maskMarks);
                else ps.stencilCompare = kAlways, ps.stencilRef = 0, ps.stencilCompareMask = 0;
                add(loc.reset, 1);
                return;
            }
            const RAMP& r = aramp[k == 4 ? irampLast : k - 1];
            ps.stencilCompare = kEqual, ps.stencilRef = uint8_t(r.stencil), ps.stencilCompareMask = uint8_t(maskMarks);
            if (k == 4) return;   // (blend off, the ramps' factors and constants left)
            ps.blend = 1;
            ps.srcColor = kFactorZero, ps.dstColor = kFactorOneMinusSrcAlpha, ps.srcAlpha = kFactorOne, ps.dstAlpha = kFactorZero;
            add(loc.reset, 0);
            const float gain = r.gain / 255.0f;
            uint32_t bits;
            std::memcpy(&bits, &gain, 4);
            add(loc.gain, bits);
            add(loc.m8, uint32_t(r.m8));
        });
    };
    uint32_t aid[5] = {};
    for (int k = 0; k < 5; ++k)
    {
        if ((k == 0 && !fFetch) || (k >= 1 && k <= 3 && !aramp[k - 1].f)) continue;
        if (!(aid[k] = state(k))) return false;
    }
    if (fFetch)
    {
        pk.drawArrays(aid[0], s_vaoEmpty, 0, 3, nullptr, 0);
        if (s_pfnFetchBarrier) s_pfnFetchBarrier();
    }
    else
    {
        BindShadowAlpha();
        if (s_fGpuMarks) GpuMark(s_szGpuMarkLast = "shcombine alpha clear");
        Clear c;
        c.clear = kClearColor;
        c.colorMask = 8;
        c.stencilWriteMask = 0;
        c.scissor = 1;
        c.rect[2] = s_dxShFrame, c.rect[3] = s_dyShFrame;   // (the pass's frame, as the draw's viewport covered)
        c.colorMaskAfter = 0xF, c.stencilWriteMaskAfter = 0, c.scissorAfter = 0;
        pk.clear(c);
    }
#if SLY2_FASTPATHS
    pk.scissor(s_rectShPhase.data());   // (the marks are within the phase's volumes' rectangle)
#endif
    if (s_fGpuMarks) GpuMark(s_szGpuMarkLast = "shcombine ramps");
    for (int k = 1; k <= 3; ++k)
        if (aid[k]) pk.drawArrays(aid[k], s_vaoEmpty, 0, 3, nullptr, 0);
#if SLY2_FASTPATHS
    pk.scissor(nullptr);
#endif
    pk.applyState(aid[4]);
    if (s_fGpuMarks) GpuMark(s_szGpuMarkLast = "shcombine stencil clear");
    Clear c;
    c.clear = kClearStencil;
    c.stencilWriteMask = 0xFF;
    c.stencil = 0;
    pk.clear(c);
    return true;
}

// 0x16DD58 (AddScreenCombine) at the phase's end: the unmarked pixels' alpha (entry 0), then the CLUT pass
// (shadowcombine.frag), one draw per ramp the marks make (their pixels are disjoint: one pass over the frame in all);
// the marks are cleared. f7 / f8: the phase drew kind 7 / kind 8.
void ShadowCombine(bool f7, bool f8)
{
    // (debug: SLY2N_SHMARKS=dir writes each combine's marks: red kind 7, green kind 8, blue the FBA bit; no MSAA)
    if (static const char* s_szDir = std::getenv("SLY2N_SHMARKS"); s_szDir && s_dxShAlpha > 0 && !s_fShFetchPhase)
    {
        static int s_c = 0;
        const int dx = s_dxShAlpha, dy = s_dyShAlpha;
        std::vector<uint8_t> as(size_t(dx) * size_t(dy)), argb(size_t(dx) * size_t(dy) * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dx, dy, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, as.data());
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x)
            {
                const uint8_t v = as[size_t(y) * size_t(dx) + size_t(x)];
                uint8_t* p = &argb[(size_t(dy - 1 - y) * size_t(dx) + size_t(x)) * 3];
                p[0] = (v & STENCIL_SH7) ? 255 : 0;
                p[1] = f8 && (v & STENCIL_SH8) ? 255 : 0;
                p[2] = (v & STENCIL_FBA) ? 128 : 0;
            }
        stbi_write_png((std::string(s_szDir) + "/shmarks_" + std::to_string(s_c++) + ".png").c_str(), dx, dy, 3, argb.data(), dx * 3);
    }
    if (!s_shShadowCombine.ID) s_shShadowCombine.Init("shadowcombine.vert", "shadowcombine.frag");
    const bool fFetch = s_fShFetchPhase;
    s_fShFetchPhase = false;
    const GLSHADER& sh = fFetch ? s_shShadowCombineFetch : s_shShadowCombine;
    const GLuint maskMarks0 = f8 ? STENCIL_SH7 | STENCIL_SH8 : STENCIL_SH7;
    if (const cane::draw::Packets* pk = PpacketsPhase(); pk && (f7 || f8) && FShadowCombinePackets(*pk, sh, fFetch, f7, f8, maskMarks0)) return;
    sh.Use();
    glUniform1i(sh.Loc("texAlpha"), 1);
    const GLint locGain = sh.Loc("gain"), locM8 = sh.Loc("m8"), locReset = sh.Loc("fReset");
    if (!fFetch) BindShadowAlpha();
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const GLuint maskMarks = f8 ? STENCIL_SH7 | STENCIL_SH8 : STENCIL_SH7;   // (without kind 8, bit 5 is kind 7's count)
    // The unmarked pixels' alpha (the CLUT's entry 0) as one alpha-only clear before the ramps, not a stencil-tested
    // draw after them: each marked pixel is in a ramp's class, and a ramp sets its alpha whatever the frame's (A = As).
    // (reading in place, the ramps need the marked pixels' alpha: only the unmarked ones, a stencil-tested draw: the marks
    // are exactly the ramps' classes, kind 7's bit never set in a phase without kind 7)
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    if (fFetch)
    {
        glUniform1i(locReset, 1);
        glDisable(GL_BLEND);
        if (f7 || f8) glStencilFunc(GL_EQUAL, 0, maskMarks);
        else glStencilFunc(GL_ALWAYS, 0, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (s_pfnFetchBarrier) s_pfnFetchBarrier();
    }
    else
    {
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, s_dxShFrame, s_dyShFrame);   // (the pass's frame, as the draw's viewport covered)
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);   // (0 - Cd) x As / 128 + Cd; A = As
    glUniform1i(locReset, 0);
    // (GAME +0x3E64 / +0x3E70, 0x1EA9F0: 128 / 96 / 160 and black in every world's RAM seen)
    struct RAMP { GLuint stencil; float gain; int m8; bool f; };
    const RAMP aramp[] = {{STENCIL_SH7, 128.0f, 0, f7}, {STENCIL_SH8, 96.0f, 1, f8}, {STENCIL_SH7 | STENCIL_SH8, 160.0f, 1, f7 && f8}};
#if SLY2_FASTPATHS
    glEnable(GL_SCISSOR_TEST);   // (the marks are within the phase's volumes' rectangle: s_rectShPhase)
    glScissor(s_rectShPhase[0], s_rectShPhase[1], s_rectShPhase[2], s_rectShPhase[3]);
#endif
    for (const RAMP& r : aramp)
    {
        if (!r.f) continue;
        glStencilFunc(GL_EQUAL, GLint(r.stencil), maskMarks);
        glUniform1f(locGain, r.gain / 255.0f);
        glUniform1i(locM8, r.m8);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
#if SLY2_FASTPATHS
    glDisable(GL_SCISSOR_TEST);
#endif
    glDisable(GL_BLEND);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
}
}  // namespace

void UpdateGlobColorsGpu(const GLOB& glob) { UpdateGlobColorsGpu(glob, glob.pgl); }

void UpdateGlobVerticesGpu(const GLOB& glob)
{
    const GLGLOB* pgl = glob.pgl;
    if (!pgl || !pgl->vbo || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    const_cast<GLGLOB*>(pgl)->fBoundsExact = false;   // (perf-4: the sphere no longer holds them)
    const size_t off = size_t(pgl->baseVertex) * sizeof(GPUVTX), cb = glob.avtx.size() * sizeof(GPUVTX);
    if (void* p = PvMapRange(pgl->vbo, off, cb))
    {
        // (built straight into the backend's memory for the range)
        GpuVerticesInto(glob, static_cast<GPUVTX*>(p));
        if (!PpacketsBuf()) glBindBuffer(GL_ARRAY_BUFFER, 0);   // (the binding the GL calls leave)
    }
    else
    {
        static std::vector<GPUVTX> s_av;
        GpuVerticesOfGlob(glob, s_av);
        GlBufferSubData(GL_ARRAY_BUFFER, pgl->vbo, off, cb, s_av.data());
        if (!PpacketsBuf()) glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    UpdateGlobColorsGpu(glob, pgl);
}

void SetGlobBoundsGpu(const GLOB& glob, const glm::vec3& posCenter, float sRadius)
{
    if (!glob.pgl) return;
    glob.pgl->posCenter = posCenter;
    glob.pgl->sRadius = sRadius;
    glob.pgl->fBoundsExact = false;
}

void UpdateGlobColorsGpu(const GLOB& glob, const GLGLOB* pgl)
{
    if (!pgl || !pgl->vboColor || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    static std::vector<GPUCOL> s_acol;
    const size_t off = size_t(pgl->baseVertex) * sizeof(GPUCOL), cb = glob.avtx.size() * sizeof(GPUCOL);
    // (packed straight into the backend's memory for the range where it has mapRange: written in order, never read)
    void* const pv = PvMapRange(pgl->vboColor, off, cb);
    if (!pv) s_acol.resize(glob.avtx.size());
    GPUCOL* const acol = pv ? static_cast<GPUCOL*>(pv) : s_acol.data();
    uint8_t aMin = 255, aMax = 0;
    for (size_t i = 0; i < glob.avtx.size(); ++i)
    {
        GPUCOL c;
        std::memcpy(c.rgbaA, &glob.avtx[i].rgbaA, 4);
        std::memcpy(c.rgbaB, &glob.avtx[i].rgbaB, 4);
        aMin = std::min(aMin, c.rgbaA[3]);
        aMax = std::max(aMax, c.rgbaA[3]);
        acol[i] = c;
    }
    pgl->aMinA = aMin;
    pgl->aMaxA = aMax;
    if (!pv) GlBufferSubData(GL_ARRAY_BUFFER, pgl->vboColor, off, cb, s_acol.data());
    if (!PpacketsBuf()) glBindBuffer(GL_ARRAY_BUFFER, 0);   // (the binding the GL calls leave)
}

#if SLY2_FASTPATHS
void PackGlobColors(const GLOB& glob, GLOBCOLORS& cols)
{
    cols.ab.resize(glob.avtx.size() * sizeof(GPUCOL));
    GPUCOL* acol = reinterpret_cast<GPUCOL*>(cols.ab.data());
    uint8_t aMin = 255, aMax = 0;
    for (size_t i = 0; i < glob.avtx.size(); ++i)
    {
        std::memcpy(acol[i].rgbaA, &glob.avtx[i].rgbaA, 4);
        std::memcpy(acol[i].rgbaB, &glob.avtx[i].rgbaB, 4);
        aMin = std::min(aMin, acol[i].rgbaA[3]);
        aMax = std::max(aMax, acol[i].rgbaA[3]);
    }
    cols.aMin = aMin;
    cols.aMax = aMax;
}

void UploadGlobColorsGpu(const GLOB& glob, const GLGLOB* pgl, const GLOBCOLORS& cols)
{
    if (!pgl || !pgl->vboColor || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    pgl->aMinA = cols.aMin;
    pgl->aMaxA = cols.aMax;
    // (packed on the helpers into cols: one copy into the backend's memory for the range, or the GL call)
    const size_t off = size_t(pgl->baseVertex) * sizeof(GPUCOL);
    if (void* p = PvMapRange(pgl->vboColor, off, cols.ab.size())) std::memcpy(p, cols.ab.data(), cols.ab.size());
    else GlBufferSubData(GL_ARRAY_BUFFER, pgl->vboColor, off, cols.ab.size(), cols.ab.data());
    if (!PpacketsBuf()) glBindBuffer(GL_ARRAY_BUFFER, 0);   // (the binding the GL calls leave)
}
#endif

GLGLOB* PglInstanceCopyGpu(const GLOB& glob)
{
    if (glob.meshk != MESHK_Strips || glob.avtx.empty() || glob.aindex.empty()) return nullptr;
    auto pgl = std::make_unique<GLGLOB>();
    std::vector<GPUVTX> av;
    GpuVerticesOfGlob(glob, av);
    AppendArena(pgl.get(), av, glob.aindex);
    SetSubBounds(pgl.get(), glob);
    GLGLOB* p = pgl.get();
    s_aglglob.push_back(std::move(pgl));
    return p;
}

void FlushInstanceGlobsGpu() { FlushArena(); }

void ResetTextureCache()
{
    // (CANE_PULL) the old world's arenas keep their buffers, not their CPU indices: a kept world's guests draw from
    // them sub-draw by sub-draw (RenderPass)
    for (auto& [vao, pa] : s_mppullarena) std::vector<uint32_t>().swap(pa.aindex);
    // The cache is keyed by the old world's BMP / CLUT records, which ResetShaders frees: a new world's record may get
    // a freed one's address (the wrong texture), and the old textures would stay in GPU memory for the whole session
    // (with texture mods, ~400 MiB per world at 4x: MODDING.md "Limits").
    for (const auto& [key, tex] : s_mptex)
        if (tex && tex != s_texWhite)
        {
            s_atexFreed.push_back(tex);
            ForgetModTexture(tex);
        }
    s_mptex.clear();
    s_aallpass.clear();
    TexStatsWorld();
    FlushTextureCache();
    ++s_genTexCache;
    // (g_fGlobArrays) the pages go with the textures (deleted with them at the next TexFromSel)
    for (const TEXPAGE& page : s_atexpage) s_atexFreed.push_back(page.tex);
    s_atexpage.clear();
    s_mptexlayer.clear();
    for (auto it = s_mptexsize.begin(); it != s_mptexsize.end();) it = it->first == s_texWhite ? std::next(it) : s_mptexsize.erase(it);
    ++s_genArray;
    CancelTextureMods();
    s_mptexapsub.clear();
    s_atexFreed.insert(s_atexFreed.end(), s_atexRetired.begin(), s_atexRetired.end());
    s_atexRetired.clear();
    s_cframeTexCache = 0;
}

// The texture mods streamed in since the last frame (texmod.h), each in its original texture's place: the entries that
// drew the original rebuilt at their next draw (with the mipmapped sampler), the original retired. In the first frames
// after a world's load the frame waits for the workers (not on the Switch, where a load at once is what stalled), so
// the world starts with its replacements; later about 2 MiB of texels a frame are taken (one upload's cost stays
// small) and a replacement shows a frame or a few after its texture is first drawn.
#ifdef __SWITCH__
constexpr double kMsTexModLoadWait = 0.0;
#else
constexpr double kMsTexModLoadWait = 1500.0;
#endif
constexpr int kCframeTexModLoadWait = 4;
void TakeStreamedTextureMods()
{
    if (!s_atexRetired.empty())
    {
        s_atexFreed.insert(s_atexFreed.end(), s_atexRetired.begin(), s_atexRetired.end());
        s_atexRetired.clear();
    }
    if (!FTextureModStreaming()) return;
    std::vector<TEXMODREADY> aready;
    const bool fLoadFrame = s_cframeTexCache < kCframeTexModLoadWait && kMsTexModLoadWait > 0.0;
    ++s_cframeTexCache;
    if (fLoadFrame)
    {
        const bool fStats = FTexStats();
        const double t0 = fStats ? MsTexStatsNow() : 0.0;
        WaitTextureMods(kMsTexModLoadWait);
        if (fStats) TexStatsAdd(TEXSTATK_Wait, MsTexStatsNow() - t0);
    }
    const double tTake0 = FTexStats() ? MsTexStatsNow() : 0.0;
    TakeTextureMods(aready, fLoadFrame ? ~size_t(0) : size_t(2) << 20);
    bool fArrays = false;
    for (const TEXMODREADY& r : aready)
    {
        const auto it = s_mptex.find({static_cast<const BMP*>(r.key[0]), static_cast<const CLUT*>(r.key[1])});
        if (it == s_mptex.end())
        {
            ForgetModTexture(r.tex);
            s_atexRetired.push_back(r.tex);
            continue;
        }
        const GLuint texOld = it->second;
        it->second = r.tex;
        NoteTexMinAlpha(r.tex, 0);   // (the replacement's own alpha: not known here)
        if (const auto itsub = s_mptexapsub.find(texOld); itsub != s_mptexapsub.end())
        {
            for (SUBGLOBGPU* p : itsub->second) p->genTex = 0;
            s_mptexapsub.erase(itsub);
        }
        if (texOld && texOld != s_texWhite)
        {
            // (its name may come back as another texture: what is kept by name forgotten; its array layer stays unused)
            s_atexRetired.push_back(texOld);
            s_mptexMinA.erase(texOld);
            s_mptexMaxA.erase(texOld);
            fArrays |= s_mptexsize.erase(texOld) != 0;
            s_mptexlayer.erase(texOld);
        }
        if (FTexStats()) TexStatsTexture(0.0, true);
    }
    if (fArrays) ++s_genArray;   // (the states' cached layers looked up again)
    if (FTexStats() && !aready.empty()) TexStatsStreamed(MsTexStatsNow() - tTake0);
}

unsigned GlTextureOfShd(const SHD* pshd, int iframe)
{
    InitCommon();
    return TexFromSel(TexselFromShd(pshd, iframe, 0));
}

glm::vec3 CAMERA::Dir() const
{
    if (fMat) return mat[0];
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return glm::vec3(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p));
}

glm::mat4 CAMERA::MatWorldToClip(float aspect) const
{
    // Reversed-Z infinite-far perspective (clip control 0..1): depth = near / w, greater is nearer.
    const float f = 1.0f / std::tan(glm::radians(fovY) * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][3] = -1.0f;
    proj[3][2] = sNear;
    if (fMat)
    {
        // GL eye axes from the camera's rows: right = -y, up = z, back = -x
        glm::mat4 eye(1.0f);
        eye[0] = glm::vec4(-mat[1], 0.0f);
        eye[1] = glm::vec4(mat[2], 0.0f);
        eye[2] = glm::vec4(-mat[0], 0.0f);
        eye[3] = glm::vec4(pos, 1.0f);
        return proj * glm::inverse(eye);
    }
    const glm::mat4 view = glm::lookAt(pos, pos + Dir(), glm::vec3(0, 0, 1));
    return proj * view;
}

void PrewarmRenderer(int scope)
{
    if (const cane::gl::Backend* pb = cane::gl::ActiveBackend(); pb && pb->prewarm) pb->prewarm(scope, 1000);
}

void BuildSceneGpu(SCENE& scene)
{
    InitCommon();
    for (SCENEGLOBSET& sgs : scene.aglobset)
        for (GLOB& glob : sgs.pglobset->aglob)
            BuildGlobGpu(glob);
    FlushArena();
    for (int iprog = 0; iprog <= CPROGGLOB + 1; ++iprog)
    {
        const GLSHADER& sh = iprog < CPROGGLOB ? *s_apshGlob[iprog] : iprog == CPROGGLOB ? glGlobShaderVol : glGlobShaderVolNoGeom;   // (s_auVol)
        if (iprog >= CPROGGLOB && !sh.ID) continue;
        UNIFORMS& u = iprog < CPROGGLOB ? s_au[iprog] : s_auVol[iprog - CPROGGLOB];
        u.matWorldToClip = sh.Loc("matWorldToClip");
        u.icolor = sh.Loc("icolor");
        u.rgbaConst = sh.Loc("rgbaConst");
        u.posCamera = sh.Loc("posCamera");
        u.dirCamera = sh.Loc("dirCamera");
        u.tex = sh.Loc("tex");
        u.fTextured = sh.Loc("fTextured");
        u.ate = sh.Loc("ate");
        u.atst = sh.Loc("atst");
        u.aref = sh.Loc("aref");
        u.afailMode = sh.Loc("afailMode");
        u.fogColor = sh.Loc("fogColor");
        u.fFogBlack = sh.Loc("fFogBlack");
        u.fDate = sh.Loc("fDate");
        u.texDate = sh.Loc("texDate");
        u.texArr = sh.Loc("texArr");
        u.caneDrawBase = g_fDrawUniform ? sh.Loc("caneDrawBase") : -1;
        u.caneVertexBase = g_fDrawUniform ? sh.Loc("caneVertexBase") : -1;
        u.caneBlend1 = g_fBlend1 ? sh.Loc("caneBlend1") : -1;
    }
    s_pu = &s_au[0];
    s_uc.matWorldToClip = glCelBorderShader.Loc("matWorldToClip");
    s_uc.matModel = glCelBorderShader.Loc("matModel");
    s_uc.halfWidthNdc = glCelBorderShader.Loc("halfWidthNdc");
    s_uc.edgeBase = glCelBorderShader.Loc("edgeBase");
    s_uc.celRgba = glCelBorderShader.Loc("celRgba");
    s_uc.cbone = glCelBorderShader.Loc("cbone");
    s_uc.iboneFirst = glCelBorderShader.Loc("iboneFirst");
    if (!s_ssboRec)
    {
        for (GLuint* pbuf : {&s_ssboBone, &s_ssboRec, &s_ssboDraw, &s_bufIndirect, &s_ssboWarp, &s_ssboWarpMat, &s_ssboTexh})
            *pbuf = GlNewBuffer(GL_SHADER_STORAGE_BUFFER, 0, nullptr, GL_STREAM_DRAW);
    }
}

namespace
{
// One view of the scene into one target: the frame (irtgt -1: the globsets not drawn into a render target) or a
// render target (irtgt 0 / 1: only its globsets, their kinds remapped as RenderGlobCtx does through 0x1BEE10).
struct PASSVIEW
{
    glm::mat4 matWorldToClip{1.0f};
    glm::vec3 pos{0.0f}, dir{1, 0, 0};
    GLuint fbo = 0;
    int dx = 1, dy = 1;
    float aspect = 1.0f;        // the target's pixel aspect (cel-border widths)
    float gCelWidth = 1.0f;     // the cel borders' width in PS2 lines of the picture (Outline Thickness; VR: matched, CelWidthEyes)
    float rMRDAdjust = 1.0f;
    uint32_t grfChannels = 0xFF;   // CM +0x14 (the volume tree's channel test)
    int irtgt = -1;
    bool fForeground = false;   // the foreground kinds 22..25 only (after the HUD, over a cleared depth)
    uint32_t rgbaClear = 0;
    uint8_t alphaClear = 0;
    const STEREOVIEW* pstereo = nullptr;   // (VR) both eyes: drawn twice from one choice of records
};

// orig: 0x002C0B28 / 0x002C0B90 (D_002C0BF8's per-target tables: a group kind -> its kind in the target, -1 not
// drawn): 3 / 4 -> 15 (18), 5 / 6 -> 16 (19), 9 -> 17 (20), the fog kinds and the target kinds likewise.
const int8_t s_amapkindRtgt[2][26] = {
    {-1, -1, -1, 15, 15, 16, 16, -1, -1, 17, -1, -1, -1, -1, -1, 15, 16, 17, 15, 16, 17, -1, 15, 16, 17, -1},
    {-1, -1, -1, 18, 18, 19, 19, -1, -1, 20, -1, -1, -1, -1, -1, 18, 19, 20, 18, 19, 20, -1, 18, 19, 20, -1},
};

// The regroup keys (order << 52 | state << 28 | index) ascending. The keys are distinct (the index), so any
// sort gives the same order: a stable LSD radix sort on bits 28..63 (the index part is already ascending)
// for the long segments, std::sort for the short ones.
void SortKeysUnique(std::vector<uint64_t>& akey)
{
    if (akey.size() < 512)
    {
        std::sort(akey.begin(), akey.end());
        return;
    }
    static std::vector<uint64_t> s_atmp;
    static uint32_t s_ac[4096];
    s_atmp.resize(akey.size());
    std::vector<uint64_t>* psrc = &akey;
    std::vector<uint64_t>* pdst = &s_atmp;
    for (int shift = 28; shift < 64; shift += 12)
    {
        std::fill(std::begin(s_ac), std::end(s_ac), 0u);
        for (uint64_t k : *psrc) ++s_ac[(k >> shift) & 0xFFF];
#if SLY2_FASTPATHS
        // (the Switch: a digit every key shares orders nothing: that pass is skipped)
        if (s_ac[(psrc->front() >> shift) & 0xFFF] == psrc->size()) continue;
#endif
        uint32_t sum = 0;
        for (uint32_t& c : s_ac) { const uint32_t n = c; c = sum; sum += n; }
        for (uint64_t k : *psrc) (*pdst)[s_ac[(k >> shift) & 0xFFF]++] = k;
        std::swap(psrc, pdst);
    }
    if (psrc != &akey) akey.swap(*psrc);   // (an odd number of passes: the result is in the other buffer)
}

// orig: VU1 P_G 14-82 (the billboard modes: LoadGlob 0x167320 passes the group's S8 rtck to the type-0 loader, which
// makes rtck 0 / 1 / 2 the param flags 0x100 / 0x200 / 0x400; P_G then builds the model's basis from the camera
// position): Sly 1's RTCK with the values one lower (-1 none). 0: the model's x axis toward the camera, z the
// camera's up made square to it (Sly 1 RTCK_All, AdjustAloRtckMat); 1: x toward the camera about the world's z
// (RTCK_WorldZ). The axes keep the matrix's scales; the model turns about its origin. 2 (only on effect meshes in
// Paris): not changed.
// TODO: check the pivot and mode 2 against P_G (not disassembled here).
static bool FBillboardMatrix(const glm::mat4& m, int rtck, const glm::vec3& posCam, const glm::vec3& vecUpCam, glm::mat4& mOut)
{
    if (rtck != 0 && rtck != 1) return false;
    static const bool s_fOff = std::getenv("SLY2_NOBILLBOARD") != nullptr;   // debugging: the loaded rotation
    if (s_fOff) return false;
    const glm::vec3 pos(m[3]);
    glm::vec3 x = posCam - pos;
    if (rtck == 1) x.z = 0.0f;
    if (glm::dot(x, x) < 1e-6f) return false;
    x = glm::normalize(x);
    glm::vec3 z = rtck == 1 ? glm::vec3(0.0f, 0.0f, 1.0f) : vecUpCam - x * glm::dot(vecUpCam, x);
    if (glm::dot(z, z) < 1e-6f) return false;
    z = glm::normalize(z);
    const glm::vec3 y = glm::cross(z, x);
    mOut = glm::mat4(glm::vec4(x * glm::length(glm::vec3(m[0])), 0.0f), glm::vec4(y * glm::length(glm::vec3(m[1])), 0.0f),
                     glm::vec4(z * glm::length(glm::vec3(m[2])), 0.0f), m[3]);
    return true;
}

// ------------------------------------------------------------ the sorted kinds' runs
// The sorted (blended) kinds draw far to near, each record's passes in order, so neighbouring draws seldom share a state
// (a two-pass shader alternates two) and each draw was its own run. Two draws whose pixels cannot meet give the same
// picture in either order (every effect of a draw, colour, depth and stencil, stays in its own pixels), so a draw may
// move back past the draws it cannot meet to join an earlier run of its state; the draws that can meet keep their
// order. Where a draw can reach: its entry's bounding sphere (GLGLOB::asphereSub: the arena's own unmoved vertices, as
// perf-4's near test uses) under the record's matrix, bounded per clip coordinate, in pixels with a pixel's margin (each
// eye in VR); skinned, warped or moved vertices reach the whole frame. (notes/VKBATCH_2026-11-10.md)
struct PIXRECT { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };   // [x0, x1) x [y0, y1) in the pass's viewport; empty: x0 >= x1
struct SORTRECT { PIXRECT a[2]; };                        // per eye (the second empty without stereo)
inline bool FRectEmpty(const PIXRECT& r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }
inline bool FRectsMeet(const PIXRECT& a, const PIXRECT& b)
{
    return !FRectEmpty(a) && !FRectEmpty(b) && a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
inline bool FSortRectsMeet(const SORTRECT& a, const SORTRECT& b) { return FRectsMeet(a.a[0], b.a[0]) || FRectsMeet(a.a[1], b.a[1]); }
inline PIXRECT RectUnion(const PIXRECT& a, const PIXRECT& b)
{
    if (FRectEmpty(a)) return b;
    if (FRectEmpty(b)) return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// The pixels a sphere (the model's space) can cover drawn with matClip x model into a dx x dy viewport: each clip
// coordinate's range over the ball (a row's value at the centre +- the radius x the row's length), divided into the
// widest NDC range, a pixel more on each side; the whole viewport when w can reach 0 (the near plane's side) or
// anything is not finite.
PIXRECT PixelRectOfSphere(const glm::mat4& matClip, const glm::mat4& model, const glm::vec3& c, float r, int dx, int dy)
{
    const PIXRECT full{0, 0, dx, dy};
    const glm::mat4 m = matClip * model;
    const float rr = r * 1.0001f;
    float lo[3], hi[3];
    const int arow[3] = {0, 1, 3};
    for (int k = 0; k < 3; ++k)
    {
        const int i = arow[k];
        const float v = m[0][i] * c.x + m[1][i] * c.y + m[2][i] * c.z + m[3][i];
        const float a = std::sqrt(m[0][i] * m[0][i] + m[1][i] * m[1][i] + m[2][i] * m[2][i]) * rr;
        const float e = 1e-4f * (std::fabs(v) + a + std::fabs(m[3][i])) + 1e-6f;   // (the GPU's own rounding, generously)
        lo[k] = v - a - e;
        hi[k] = v + a + e;
    }
    if (!(lo[2] > 0.0f) || !std::isfinite(hi[2])) return full;
    auto ndcLo = [&](int k) { return lo[k] >= 0.0f ? lo[k] / hi[2] : lo[k] / lo[2]; };
    auto ndcHi = [&](int k) { return hi[k] >= 0.0f ? hi[k] / lo[2] : hi[k] / hi[2]; };
    const double x0 = (double(ndcLo(0)) * 0.5 + 0.5) * dx, x1 = (double(ndcHi(0)) * 0.5 + 0.5) * dx;
    const double y0 = (double(ndcLo(1)) * 0.5 + 0.5) * dy, y1 = (double(ndcHi(1)) * 0.5 + 0.5) * dy;
    if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1)) return full;
    PIXRECT rc;
    rc.x0 = int(std::clamp(std::floor(x0) - 1.0, 0.0, double(dx)));
    rc.x1 = int(std::clamp(std::ceil(x1) + 1.0, 0.0, double(dx)));
    rc.y0 = int(std::clamp(std::floor(y0) - 1.0, 0.0, double(dy)));
    rc.y1 = int(std::clamp(std::ceil(y1) + 1.0, 0.0, double(dy)));
    return rc;
}

// The sorted segment acmd[first, lim) (pass draws only) reordered: each draw in turn joins the latest run it may join
// (fJoin: the state rule the runs below use) when every run after that one cannot meet it, else it starts a new run at
// the end. Any two draws that can meet keep their order (a draw only moves back past runs it cannot meet). arect: by
// draw (CMD::irect; none: srFull).
template <class FJOIN>
void SortJoinSegment(std::vector<CMD>& acmd, size_t first, size_t lim, const std::vector<SORTRECT>& arect, const SORTRECT& srFull, FJOIN fJoin)
{
    constexpr size_t CRUN_BACK = 64;     // (the runs looked back over: a bound on the work)
    constexpr int CMEMBER_CHECK = 48;    // (a run's draws tested one by one when its union meets; more: taken as meeting)
    struct SJRUN { uint32_t istate, ifirst, ilast; int c; SORTRECT rcUnion; uint8_t fNoClip; };
    static std::vector<SJRUN> s_arun;
    static std::vector<uint32_t> s_anext;
    static std::vector<CMD> s_acmdOut;
    const size_t c = lim - first;
    s_arun.clear();
    s_anext.assign(c, UINT32_MAX);
    auto rectOf = [&](size_t i) -> const SORTRECT& {
        const uint32_t ir = acmd[first + i].irect;
        return ir < arect.size() ? arect[ir] : srFull;
    };
    auto runMeets = [&](const SJRUN& run, const SORTRECT& rc) {
        if (!FSortRectsMeet(run.rcUnion, rc)) return false;
        if (run.c > CMEMBER_CHECK) return true;
        for (uint32_t k = run.ifirst; k != UINT32_MAX; k = s_anext[k])
            if (FSortRectsMeet(rectOf(k), rc)) return true;
        return false;
    };
    for (size_t i = 0; i < c; ++i)
    {
        const CMD& cmd = acmd[first + i];
        const SORTRECT& rc = rectOf(i);
        size_t irunJoin = SIZE_MAX;
        for (size_t k = s_arun.size(); k-- > 0 && s_arun.size() - k <= CRUN_BACK;)
        {
            if (fJoin(s_arun[k].istate, cmd.istate) && s_arun[k].fNoClip == cmd.fNoClip) { irunJoin = k; break; }
            if (runMeets(s_arun[k], rc)) break;
        }
        if (irunJoin == SIZE_MAX)
        {
            s_arun.push_back({cmd.istate, uint32_t(i), uint32_t(i), 1, rc, cmd.fNoClip});
            continue;
        }
        SJRUN& run = s_arun[irunJoin];
        s_anext[run.ilast] = uint32_t(i);
        run.ilast = uint32_t(i);
        ++run.c;
        run.rcUnion.a[0] = RectUnion(run.rcUnion.a[0], rc.a[0]);
        run.rcUnion.a[1] = RectUnion(run.rcUnion.a[1], rc.a[1]);
    }
    if (s_arun.size() == c) return;   // (nothing joined: the order as it was)
    s_acmdOut.clear();
    // (debug: SLY2N_SORTCHECK=1: every two draws whose rectangles meet checked to keep their order; a failure printed)
    static const bool s_fCheck = std::getenv("SLY2N_SORTCHECK") != nullptr;
    static std::vector<uint32_t> s_aiOut;
    s_aiOut.clear();
    for (const SJRUN& run : s_arun)
        for (uint32_t k = run.ifirst; k != UINT32_MAX; k = s_anext[k])
        {
            s_acmdOut.push_back(acmd[first + k]);
            if (s_fCheck) s_aiOut.push_back(k);
        }
    if (s_fCheck)
    {
        static long s_cbad = 0, s_cseg = 0;
        ++s_cseg;
        for (size_t a = 0; a < s_aiOut.size(); ++a)
            for (size_t b = a + 1; b < s_aiOut.size(); ++b)
                if (s_aiOut[a] > s_aiOut[b] && FSortRectsMeet(rectOf(s_aiOut[a]), rectOf(s_aiOut[b])))
                    std::printf("sortcheck: draws %u / %u swapped though they meet (%ld bad)\n", s_aiOut[b], s_aiOut[a], ++s_cbad);
        if (s_cseg % 500 == 0) std::printf("sortcheck: %ld segments, %ld bad\n", s_cseg, s_cbad);
    }
    std::copy(s_acmdOut.begin(), s_acmdOut.end(), acmd.begin() + std::ptrdiff_t(first));
}

// (measurement, OpenGL renderer only: SLY2N_ALPHASTAT=dir:N, notes/VKALPHA_2026-11-11.md) every N-th main pass, the
// sorted transparent kinds' (5 / 6 / 10) cost: before each of their draws two invisible copies (no colour / depth
// writes) in occlusion queries: the fragments that pass its depth (and alpha) test, and of those the ones whose final
// alpha is not 0 (the alpha test forced to "greater than 0"); the first copy also counts them per pixel in the stencil's
// low 7 bits, read back around each sequence of such draws: dir/heat_<n>.pgm (counts) and dir/stat_<n>.txt (per draw:
// kind, object, shader, pass, blend, depth / colour mask, alpha test, indices, fragments, non-zero-alpha fragments,
// screen rectangle). The picture after the first such draw is not the game's (the stencil's low bits).
struct ALPHASTATREC { int kind, oid, shd, ipass; uint64_t alpha; int depthMask, colorMask, ate, depthFunc; uint32_t count, tex; GLuint aq[3]; PIXRECT rect; };
struct ALPHASTAT
{
    std::string dir;
    int every = 0, cpass = 0, n = 0;
    bool fOn = false, fInSeq = false;
    int dx = 0, dy = 0;
    std::vector<uint8_t> abBefore, abAfter;
    std::vector<uint16_t> acount;
    std::vector<ALPHASTATREC> arec;
    std::vector<GLuint> aq;
    size_t iq = 0;
    long cdirtyBefore = 0;
};
ALPHASTAT& AlphaStat()
{
    static ALPHASTAT s = [] {
        ALPHASTAT a;
        if (const char* sz = std::getenv("SLY2N_ALPHASTAT"))
        {
            const char* pc = std::strchr(sz, ':');
            a.dir = pc ? std::string(sz, pc) : std::string(sz);
            a.every = pc ? std::max(1, std::atoi(pc + 1)) : 50;
        }
        return a;
    }();
    return s;
}
GLuint AlphaStatQuery()
{
    ALPHASTAT& a = AlphaStat();
    if (a.iq == a.aq.size()) { a.aq.resize(a.aq.size() + 256); glGenQueries(256, &a.aq[a.iq]); }
    return a.aq[a.iq++];
}
void AlphaStatReadStencil(std::vector<uint8_t>& ab)
{
    ALPHASTAT& a = AlphaStat();
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(fbo));
    ab.resize(size_t(a.dx) * size_t(a.dy));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, a.dx, a.dy, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, ab.data());
}
void AlphaStatSeq(bool fBegin)
{
    ALPHASTAT& a = AlphaStat();
    if (fBegin)
    {
        AlphaStatReadStencil(a.abBefore);
        for (uint8_t b : a.abBefore) a.cdirtyBefore += (b & 0x7F) != 0;
        a.fInSeq = true;
        return;
    }
    AlphaStatReadStencil(a.abAfter);
    for (size_t i = 0; i < a.abAfter.size(); ++i) a.acount[i] = uint16_t(a.acount[i] + ((a.abAfter[i] - a.abBefore[i]) & 0x7F));
    // (the low bits back to 0, as the shadow phases expect them)
    glDisable(GL_SCISSOR_TEST);
    glStencilMask(0x7F);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    a.fInSeq = false;
}
void AlphaStatEnd()
{
    ALPHASTAT& a = AlphaStat();
    if (a.fInSeq) AlphaStatSeq(false);
    const std::string base = a.dir + "/";
    if (FILE* f = std::fopen((base + "heat_" + std::to_string(a.n) + ".pgm").c_str(), "wb"))
    {
        std::fprintf(f, "P5\n%d %d\n65535\n", a.dx, a.dy);
        for (int y = a.dy - 1; y >= 0; --y)
            for (int x = 0; x < a.dx; ++x)
            {
                const uint16_t v = a.acount[size_t(y) * size_t(a.dx) + size_t(x)];
                const uint8_t ab[2] = {uint8_t(v >> 8), uint8_t(v)};
                std::fwrite(ab, 1, 2, f);
            }
        std::fclose(f);
    }
    if (FILE* f = std::fopen((base + "stat_" + std::to_string(a.n) + ".txt").c_str(), "w"))
    {
        std::fprintf(f, "# %dx%d dirty-before %ld fail-all-skipped %d\n# kind oid shd pass alpha depthMask colorMask ate depthFunc indices tex frags nzfrags x0 y0 x1 y1 coverage\n", a.dx, a.dy, a.cdirtyBefore, g_renderstats.cfailAll);
        for (const ALPHASTATREC& r : a.arec)
        {
            GLuint64 c0 = 0, c1 = 0;
            glGetQueryObjectui64v(r.aq[0], GL_QUERY_RESULT, &c0);
            if (r.aq[1]) glGetQueryObjectui64v(r.aq[1], GL_QUERY_RESULT, &c1);
            else c1 = c0;
            GLuint64 c2 = 0;
            glGetQueryObjectui64v(r.aq[2], GL_QUERY_RESULT, &c2);
            std::fprintf(f, "%d %d %d %d %llx %d %d %d %d %u %u %llu %llu %d %d %d %d %llu\n", r.kind, r.oid, r.shd, r.ipass, (unsigned long long)r.alpha, r.depthMask,
                         r.colorMask, r.ate, r.depthFunc, r.count, r.tex, (unsigned long long)c0, (unsigned long long)c1, r.rect.x0, r.rect.y0, r.rect.x1, r.rect.y1, (unsigned long long)c2);
        }
        std::fclose(f);
    }
    a.fOn = false;
    ++a.n;
}

void RenderPass(const SCENE& scene, const PASSVIEW& pv, const RENDEROPTS& opts)
{
    s_fGpuMarks = FGpuMarks();
    if (s_fGpuMarks) GpuMark(s_szGpuMarkLast = "pass setup");
    // (vkalpha) Multisampled targets: a pixel whose centre lies outside a triangle that covers some of its samples gets
    // the varyings extrapolated (vColor is not centroid), so the vertex alpha can exceed the entry's highest there: the
    // alpha-fail-all shortcut then only for entries whose vertex alpha is the same everywhere (vMin == vMax) on such a
    // target (vkparity msaa4 moved 2-8 pixels without this). The stencil shortcuts are exact with MSAA too.
    const bool fSingleSample = pv.irtgt >= 0 || (pv.pstereo ? vr::MsaaOption() <= 1 : g_gl.msaa <= 1);
    const float aspect = pv.aspect;
    const glm::mat4& matWorldToClip = pv.matWorldToClip;
    struct { glm::vec3 pos, dir; glm::vec3 Dir() const { return dir; } } cam{pv.pos, pv.dir};
    const float sNearClipSh = g_cm.sNearClip;   // (CM +0x4C: the hulls' near test, DrawShadow)
    // the drawn near plane's corners (NDC depth 1 in the reversed-Z projection): the hulls it reaches count z-fail
    std::array<glm::vec3, 4> aposNearPass;
    {
        const glm::mat4 m = glm::inverse(matWorldToClip);
        const float ax[4] = {-1.0f, 1.0f, 1.0f, -1.0f}, ay[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        for (size_t i = 0; i < 4; ++i)
        {
            const glm::vec4 v = m * glm::vec4(ax[i], ay[i], 1.0f, 1.0f);
            aposNearPass[i] = glm::vec3(v) / v.w;
        }
    }
    // The view frustum's planes (Gribb / Hartmann on the clip matrix; reversed Z, infinite far: left, right, bottom,
    // top), normalized so a sphere is outside when its centre is at least its radius behind one.
    glm::vec4 aplane[4];
    {
        const glm::mat4 m = glm::transpose(matWorldToClip);   // rows of the clip matrix
        aplane[0] = m[3] + m[0];
        aplane[1] = m[3] - m[0];
        aplane[2] = m[3] + m[1];
        aplane[3] = m[3] - m[1];
        for (glm::vec4& p : aplane) p /= glm::length(glm::vec3(p));
    }
    CM cmFade;
    cmFade.rMRDAdjust = pv.rMRDAdjust * g_display.drawDistance;
    // (QoL draw distance: the cel borders (default MRD 2000) follow it only below 1x; above, the original's limit
    // stays, or the far characters' fixed-width outlines cover them)
    CM cmFadeCel;
    cmFadeCel.rMRDAdjust = pv.rMRDAdjust * std::min(g_display.drawDistance, 1.0f);
    static const int s_oidFade = std::getenv("SLY2_FADEOID") ? std::atoi(std::getenv("SLY2_FADEOID")) : -2;
    auto maxScale = [](const glm::mat4& m) {
        return std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])),
                                   glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                                   glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
    };
    auto outside = [&](const glm::vec3& pos, float radius) {
        for (int i = 0; i < 4; ++i)
            if (glm::dot(glm::vec3(aplane[i]), pos) + aplane[i].w <= -radius) return true;
        return false;
    };
    static const bool s_fNoFrustum = std::getenv("SLY2_NOFRUSTUM") != nullptr;
    static const bool s_fNoFade = std::getenv("SLY2_NOFADE") != nullptr;   // debugging: no distance fade
    static const bool s_fNoTree = std::getenv("SLY2_NOTREE") != nullptr;   // debugging: the volume tree's culling off

    // Draw records (RenderSw 0x1CDA80 / RenderGlob 0x168638): kind = the group's rp, z = distance squared.
    // (one buffer per target kind: the frame's pass and the render targets' passes keep their records apart,
    // reused across frames without reallocating)
    static std::vector<DRAWREC> s_aarec[1 + CRTGT];
    std::vector<DRAWREC>& arec = s_aarec[size_t(pv.irtgt + 1)];
    arec.clear();
    // the billboarded groups' matrices this pass (a deque: the records point into it)
    static std::deque<glm::mat4> s_aamatBillboard[1 + CRTGT];
    std::deque<glm::mat4>& amatBillboard = s_aamatBillboard[size_t(pv.irtgt + 1)];
    amatBillboard.clear();
    const glm::vec3 vecUpCam = [&] {
        const glm::vec3 r1(matWorldToClip[0][1], matWorldToClip[1][1], matWorldToClip[2][1]);   // the clip matrix's y row
        return glm::dot(r1, r1) > 0.0f ? glm::normalize(r1) : glm::vec3(0.0f, 0.0f, 1.0f);
    }();
    // (into the given deque: the record blocks keep their own, built on several threads)
    auto pmatBillboardIn = [&](std::deque<glm::mat4>& amat, const glm::mat4& m, int rtck) -> const glm::mat4* {
        glm::mat4 mb;
        if (!FBillboardMatrix(m, rtck, cam.pos, vecUpCam, mb)) return &m;
        amat.push_back(mb);
        return &amat.back();
    };
    auto pmatBillboard = [&](const glm::mat4& m, int rtck) { return pmatBillboardIn(amatBillboard, m, rtck); };
    static const char* s_szVolDbg = std::getenv("SLY2_VOLDBG");   // debugging: the volumes (shader kind 3) within this distance
    if (s_szVolDbg && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        if (++s_cframe == 1)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const GLOB& gg = !g.pgl && g.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(g.iglobTemplate)] : g;
                    int shdk = -1;
                    for (const SUBGLOB& sub : gg.asubglob) if (sub.pshd) shdk = std::max(shdk, int(sub.pshd->shdk));
                    static const bool s_fFx = std::getenv("SLY2_VOLDBGFX") != nullptr;   // (the type-3 effect meshes instead)
                    if (s_fFx)
                    {
                        if (gg.meshk != MESHK_Indexed || gg.asubfx.empty() || !gg.asubfx[0].pshd) continue;
                        const SHD* ps = gg.asubfx[0].pshd;
                        const glm::vec3 c = i < sgs.amatGlob.size() ? glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f)) : glm::vec3(0);
                        std::printf("fx3: oid %d #%zu rp %d shd %d k%d grfshd 0x%X rgba %08X %08X r %.0f at %.0f %.0f %.0f vis %d\n", sgs.oid, i, g.rp, ps->id, ps->shdk, ps->grfshd,
                                    ps->argba[0], ps->argba[1], g.sRadius, c.x, c.y, c.z, i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1);
                        continue;
                    }
                    if (shdk != 3 && (g.rp & 0x1F) != 11 && (g.rp & 0x1F) != 13) continue;
                    const glm::vec3 c = i < sgs.amatGlob.size() ? glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f)) : glm::vec3(0);
                    const float d = glm::distance(c, cam.pos);
                    if (d > std::atof(s_szVolDbg)) continue;
                    std::printf("vol: oid %d glob@%zu #%zu rp %d shdk %d grfshd 0x%X r %.0f at %.0f %.0f %.0f d %.0f xf %d vis %d hidden %d irtgt %d A %08X\n", sgs.oid, sgs.ib, i,
                                g.rp, shdk, gg.asubglob.empty() || !gg.asubglob[0].pshd ? 0 : gg.asubglob[0].pshd->grfshd, g.sRadius, c.x, c.y, c.z, d,
                                int(sgs.fTransform), i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, int(g.fHidden), sgs.irtgt,
                                gg.avtx.empty() ? 0 : gg.avtx[0].rgbaA);
                    for (const SUBGLOB& sub : gg.asubglob)
                        if (sub.pshd && sub.pshd->shdk == 3)
                            std::printf("    vshd %d grfshd 0x%X argba %08X %08X %08X tex %zu\n", sub.pshd->id, sub.pshd->grfshd, sub.pshd->argba[0], sub.pshd->argba[1], sub.pshd->argba[2], sub.pshd->atex.size());
                }
    }
    if (static const bool s_fRtckDbg = std::getenv("SLY2_RTCKDBG") != nullptr; s_fRtckDbg && pv.irtgt < 0 && !pv.fForeground)   // debugging: the groups with a billboard mode
    {
        static bool s_f = false;
        if (!s_f)
        {
            s_f = true;
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size() && i < sgs.amatGlob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    if (g.rtck == -1) continue;
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f));
                    std::printf("rtck %d: oid %d #%zu rp %d meshk %d r %.0f at %.0f %.0f %.0f vis %d inst %d\n", g.rtck, sgs.oid, i, g.rp, int(g.meshk), g.sRadius, c.x, c.y, c.z,
                                i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, g.iglobTemplate);
                }
        }
    }
    static const char* s_szPtDbg = std::getenv("SLY2_PTDBG");   // debugging: "x y z": the groups whose sphere holds the point
    if (s_szPtDbg && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        glm::vec3 pt(0.0f);
        std::sscanf(s_szPtDbg, "%f %f %f", &pt.x, &pt.y, &pt.z);
        if (++s_cframe == 1)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size() && i < sgs.amatGlob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const GLOB& gg = !g.pgl && g.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(g.iglobTemplate)] : g;
                    const float r = std::max(g.sRadius, gg.pgl ? gg.pgl->sRadius : 0.0f);
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(gg.pgl && !g.fDraw ? gg.pgl->posCenter : g.posCenter, 1.0f));
                    static const float s_rPt = std::getenv("SLY2_PTDBGR") ? float(std::atof(std::getenv("SLY2_PTDBGR"))) : -1.0f;
                    if (glm::distance(c, pt) > (s_rPt > 0.0f ? s_rPt : r)) continue;
                    std::printf("pt: oid %d #%zu n0 %d rp %d meshk %d r %.0f at %.0f %.0f %.0f vis %d xf %d:", sgs.oid, i, g.n0, g.rp, int(gg.meshk), r, c.x, c.y, c.z,
                                i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, int(sgs.fTransform));
                    for (const SUBGLOB& sub : gg.asubglob) if (sub.pshd) std::printf(" <shd %d k%d grf 0x%X %08X>", sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0]);
                    for (const auto& sub : gg.asubfx) if (sub.pshd) std::printf(" <fx shd %d k%d grf 0x%X %08X>", sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0]);
                    std::printf("\n");
                    // (SLY2_PTDBGV=N: also the group's N vertices nearest the point, their colours A / B)
                    if (const char* szV = std::getenv("SLY2_PTDBGV"))
                    {
                        std::vector<std::pair<float, size_t>> ad;
                        for (size_t k = 0; k < gg.avtx.size(); ++k)
                            ad.push_back({glm::distance(glm::vec3(sgs.amatGlob[i] * glm::vec4(gg.avtx[k].pos, 1.0f)), pt), k});
                        std::sort(ad.begin(), ad.end());
                        for (size_t k = 0; k < ad.size() && k < size_t(std::max(1, std::atoi(szV))); ++k)
                        {
                            const VTXGLOB& v = gg.avtx[ad[k].second];
                            const glm::vec3 w = glm::vec3(sgs.amatGlob[i] * glm::vec4(v.pos, 1.0f));
                            std::printf("    v%zu at %.0f %.0f %.0f d %.0f A %08X B %08X unbaked %d\n", ad[k].second, w.x, w.y, w.z, ad[k].first,
                                        v.rgbaA, v.rgbaB, int(v.fUnbaked));
                        }
                    }
                }
    }
    // The records built in items on the pass helpers: runs of whole entries, or a large entry's groups in parts (its
    // own tests first, on this thread). An item writes only its own block; joined in order: the records of one loop.
    struct SGSPRE { bool fDraw = false, fTree = false; float wholeFade = 1.0f; };
    // (a cache line or more each: two threads filling neighbouring items do not share one)
    struct alignas(64) RECBLOCK { std::vector<DRAWREC> arec; std::deque<glm::mat4> amatBillboard; int cmrd = 0, cculled = 0; };
    // whole entries [isgsFirst, isgsLim) (isplit < 0), or the groups [iglobFirst, iglobLim) of entry isgsFirst (the
    // last part to the end: iglobLim SIZE_MAX), whose tests are apre[isplit]
    struct RECITEM { uint32_t isgsFirst = 0, isgsLim = 0; size_t iglobFirst = 0, iglobLim = 0; int isplit = -1; };
    constexpr size_t CGLOB_ITEM = 128, CSGS_ITEM = 32;
    struct RECITEMS { std::vector<RECITEM> aitem; size_t csgs = 0; const SCENEGLOBSET* psgs = nullptr; int csplit = 0; };
    static RECITEMS s_aitems[1 + CRTGT];
    RECITEMS& items = s_aitems[size_t(pv.irtgt + 1)];
    if (items.csgs != scene.aglobset.size() || items.psgs != scene.aglobset.data())
    {
        items = RECITEMS{};
        items.csgs = scene.aglobset.size();
        items.psgs = scene.aglobset.data();
        RECITEM run;
        size_t cglobRun = 0;
        auto endRun = [&](uint32_t isgs) {
            if (run.isgsLim > run.isgsFirst) items.aitem.push_back(run);
            run = RECITEM{};
            run.isgsFirst = run.isgsLim = isgs;
            cglobRun = 0;
        };
        for (uint32_t isgs = 0; isgs < uint32_t(scene.aglobset.size()); ++isgs)
        {
            const size_t cglob = scene.aglobset[isgs].pglobset->aglob.size();
            if (cglob > CGLOB_ITEM)
            {
                endRun(isgs);
                const int isplit = items.csplit++;
                for (size_t i = 0; i < cglob; i += CGLOB_ITEM)
                {
                    RECITEM part;
                    part.isgsFirst = isgs;
                    part.isgsLim = isgs + 1;
                    part.iglobFirst = i;
                    part.iglobLim = i + CGLOB_ITEM >= cglob ? SIZE_MAX : i + CGLOB_ITEM;
                    part.isplit = isplit;
                    items.aitem.push_back(part);
                }
                endRun(isgs + 1);
                continue;
            }
            run.isgsLim = isgs + 1;
            cglobRun += cglob;
            if (cglobRun >= CGLOB_ITEM || run.isgsLim - run.isgsFirst >= CSGS_ITEM) endRun(isgs + 1);
        }
        endRun(uint32_t(scene.aglobset.size()));
    }
    static std::vector<RECBLOCK> s_aablock[1 + CRTGT];
    std::vector<RECBLOCK>& ablock = s_aablock[size_t(pv.irtgt + 1)];
    if (ablock.size() < items.aitem.size()) ablock.resize(items.aitem.size());
    static std::vector<SGSPRE> s_aapre[1 + CRTGT];
    static std::vector<std::vector<float>> s_aaagVis[1 + CRTGT];
    std::vector<SGSPRE>& apre = s_aapre[size_t(pv.irtgt + 1)];
    std::vector<std::vector<float>>& aagVis = s_aaagVis[size_t(pv.irtgt + 1)];
    apre.resize(size_t(items.csplit));
    aagVis.resize(size_t(items.csplit));
    // An entry's own tests (its object sphere's fade and frustum, its tree's fades into agVisTree): whether its groups
    // are looked at
    auto preOfSgs = [&](const SCENEGLOBSET& sgs, std::vector<float>& s_agVisTree, int& cmrd, int& cculled) {
        SGSPRE pre;
        if (!sgs.fTransform || sgs.irtgt != pv.irtgt) return pre;
        static const char* s_range = std::getenv("SLY2_IBRANGE");   // debugging: only globsets in [lo, hi)
        if (s_range)
        {
            size_t lo = 0, hi = 0;
            std::sscanf(s_range, "%zu,%zu", &lo, &hi);
            if (sgs.ib < lo || sgs.ib >= hi) return pre;
        }
        const GLOBSET& gs = *sgs.pglobset;
        // orig: 0x1B4478: a glob set with sphere sets (+0x48) is drawn through its volume tree (0x1D5808), not
        // RenderGlob: from set 0, each record (kind -1 skipped) whose channel mask meets CM +0x14 is tested by VU0
        // micro 0x1C0 (the four side planes by its radius, the MRD fade by its centre, x CM +0x1C); 0 culls it (a
        // kind-1 record: its whole set), a group's record submits the fade as its alpha (qw8 = gVis, 1, scale).
        // Neither the whole-object sphere nor the object's fade apply.
        const bool fTree = !s_fNoTree && pv.irtgt < 0 && !gs.asphereset.empty();
        pre.fTree = fTree;
        if (fTree)
        {
            s_agVisTree.assign(gs.aglob.size(), 0.0f);
#if SLY2_FASTPATHS
            // (the Switch: the groups' cel-border flags from the glob set's list (glob.cpp), while it matches them)
            const uint8_t* afCel = gs.afCelBorder.size() == gs.aglob.size() ? gs.afCelBorder.data() : nullptr;
#endif
            int aiset[256];
            int ciset = 0;
            aiset[ciset++] = 0;
            while (ciset > 0)
            {
                const int iset = aiset[--ciset];
                if (iset < 0 || size_t(iset) >= gs.asphereset.size()) continue;
                for (const GLOBSPHERE& sphere : gs.asphereset[size_t(iset)])
                {
                    if (sphere.kind == -1) continue;
                    if (!(sphere.mask & pv.grfChannels)) continue;
                    if (!s_fNoFrustum && outside(sphere.pos, sphere.sRadius)) continue;
#if SLY2_FASTPATHS
                    const bool fCel = sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size() &&
                                      (afCel ? afCel[size_t(sphere.iglob)] != 0 : gs.aglob[size_t(sphere.iglob)].meshk == MESHK_CelBorder);
#else
                    const bool fCel = sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size() &&
                                      gs.aglob[size_t(sphere.iglob)].meshk == MESHK_CelBorder;
#endif
                    const float gVis = UCmSphereDistanceFade(fCel ? &cmFadeCel : &cmFade, sphere.pos - cam.pos, sphere.sMRD);
                    if (gVis == 0.0f) { ++cmrd; continue; }
                    if (sphere.kind == 1) { if (ciset < 256) aiset[ciset++] = sphere.iset; }
                    else if (sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size())
                        s_agVisTree[size_t(sphere.iglob)] = gVis;
                }
            }
        }
        // orig: 0x1686AC..0x1687A8: the whole sphere precedes every group and supplies f20.
        float wholeFade = 1.0f;
        if (pv.irtgt < 0 && sgs.fBounds && !fTree)
        {
            wholeFade = UCmSphereDistanceFade(sgs.fMrdOriginal ? &cmFadeCel : &cmFade, sgs.posBounds - cam.pos, sgs.sMRD);
            if (sgs.oid == s_oidFade)
                std::printf("fadeobj oid %d center %.9g %.9g %.9g radius %.9g mrd %.9g adjust %.9g fade %.9g\n",
                            sgs.oid, sgs.posBounds.x, sgs.posBounds.y, sgs.posBounds.z, sgs.sBoundsRadius, sgs.sMRD, cmFade.rMRDAdjust, wholeFade);
            if (wholeFade == 0.0f) { ++cmrd; return pre; }
            if (!s_fNoFrustum && outside(sgs.posBounds, sgs.sBoundsRadius)) { ++cculled; return pre; }
        }
        pre.fDraw = true;
        pre.wholeFade = wholeFade;
        return pre;
    };
    // The records of an entry's groups [iFirst, iLim) (its tests passed: pre, s_agVisTree)
    auto recordsOfGlobs = [&](const SCENEGLOBSET& sgs, const SGSPRE& pre, const std::vector<float>& s_agVisTree, size_t iFirst, size_t iLim,
                              RECBLOCK& blk) {
        const GLOBSET& gs = *sgs.pglobset;
        const bool fTree = pre.fTree;
        const float wholeFade = pre.wholeFade;
        iLim = std::min(iLim, gs.aglob.size());
        for (size_t i = iFirst; i < iLim; ++i)
        {
#if SLY2_FASTPATHS
            // (the Switch: the parts of the group four ahead that the tests below read)
            if (i + 4 < iLim)
            {
                const GLOB& g = gs.aglob[i + 4];
                __builtin_prefetch(&g);
                __builtin_prefetch(&g.abone);
                __builtin_prefetch(&g.iglobTemplate);
                __builtin_prefetch(&g.pgl);
            }
#endif
            const GLOB& glob = gs.aglob[i];
            // (afVisible holds the loaded flag (0x800) as changed at run time by ShowAloGlobs: a group loaded hidden
            // and shown by a script draws; the AI code's fix for Paris' guards)
            if (i < sgs.afVisible.size() ? !sgs.afVisible[i] : glob.fHidden) continue;
            int kind = glob.rp & 0x1F;
            if (pv.irtgt >= 0)
            {
                kind = kind < 26 ? s_amapkindRtgt[pv.irtgt][kind] : -1;
                if (kind < 0) continue;
            }
            const GLOB* pgeom = &glob;
            if (!glob.pgl && glob.iglobTemplate >= 0) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
            if (!pgeom->pgl && !(pgeom->meshk == MESHK_Effect && opts.fShadows && pv.irtgt < 0)) continue;
            const glm::mat4& mat = sgs.amatGlob[i];
            const glm::vec3 posWorld = glm::vec3(MulMat4Vec4(mat, glm::vec4(glob.posCenter, 1.0f)));
            const float d = glm::distance(posWorld, cam.pos);
            static const int s_oidRec = std::getenv("SLY2_RECOID") ? std::atoi(std::getenv("SLY2_RECOID")) : -2;   // debugging
            static const int s_oidOnly = std::getenv("SLY2_ONLYOID") ? std::atoi(std::getenv("SLY2_ONLYOID")) : -2;   // debugging: only this object
            if (s_oidOnly != -2 && sgs.oid != s_oidOnly) continue;
            static const int s_oidSkip = std::getenv("SLY2_SKIPOID") ? std::atoi(std::getenv("SLY2_SKIPOID")) : -2;   // debugging: all but this object
            static const char* s_szGlobSkip = std::getenv("SLY2_SKIPGLOB");   // (with SLY2_SKIPOID: only these groups, "a,b,c")
            if (sgs.oid == s_oidSkip &&
                (!s_szGlobSkip || std::strstr((std::string(",") + s_szGlobSkip + ",").c_str(), ("," + std::to_string(i) + ",").c_str())))
                continue;
            static const int s_iglobOnly = std::getenv("SLY2_ONLYGLOB") ? std::atoi(std::getenv("SLY2_ONLYGLOB")) : -1;   // debugging: only this group (with SLY2_ONLYOID)
            if (s_iglobOnly >= 0 && s_oidOnly != -2 && int(i) != s_iglobOnly) continue;
            if (sgs.oid == s_oidRec)
                std::printf("rec oid %d #%zu rp %d d %.0f r %.1f mrd %.0f fDraw %d alpha %.2f at %.0f %.0f %.0f\n", sgs.oid, i, glob.rp, d, glob.sRadius, glob.sMRD,
                            int(glob.fDraw), sgs.gAlpha, posWorld.x, posWorld.y, posWorld.z);
            if (sgs.oid == s_oidRec && std::getenv("SLY2_RECVTX"))
            {
                std::printf("   vtx A alpha:");
                for (const VTXGLOB& v : pgeom->avtx) std::printf(" %u", v.rgbaA >> 24);
                std::printf("\n   vtx src alpha:");
                for (const VTXGLOB& v : pgeom->avtx) std::printf(" %u", v.rgbaSrc >> 24);
                std::printf("\n");
                if (glob.rp == 10)
                    for (const VTXGLOB& v : pgeom->avtx)
                    {
                        const glm::vec3 pw = glm::vec3(mat * glm::vec4(v.pos, 1.0f));
                        const glm::vec3 nw = glm::normalize(glm::mat3(mat) * v.normal);
                        const glm::vec4 c = pv.matWorldToClip * glm::vec4(pw, 1.0f);
                        const glm::vec3 tc = glm::normalize(cam.pos - pw);
                        float aSpec = float(v.rgbaA >> 24);
                        if (pgeom->iSpec == 1)
                        {
                            const glm::mat3 mInv = glm::inverse(glm::mat3(mat));
                            const glm::vec3 vv = glm::normalize(mInv * (cam.pos - glm::vec3(mat[3])) - pgeom->posSpec);
                            const float x = glm::dot(vv, glm::trunc(v.normal * 127.0f));
                            const float g = std::clamp(pgeom->agSpec[0] + pgeom->agSpec[1] * x + pgeom->agSpec[2] * x * x, pgeom->agClamp[0], pgeom->agClamp[1]);
                            aSpec = std::floor(aSpec * g + 0.5f);
                        }
                        std::printf("   gv %.1f %.1f a %.0f ndv %.4f d %.0f w %.1f %.1f %.1f n %.3f %.3f %.3f nlen %.4f\n", (c.x / c.w * 0.5f + 0.5f) * 512.0f,
                                    (0.5f - c.y / c.w * 0.5f) * 448.0f, aSpec, glm::dot(nw, tc), glm::distance(pw, cam.pos), pw.x, pw.y, pw.z, nw.x, nw.y, nw.z, glm::length(v.normal));
                    }
            }
            if (sgs.oid == s_oidRec)
                for (const SUBGLOB& sub : pgeom->asubglob)
                {
                    if (!sub.pshd) continue;
                    std::printf("   sub grf 0x%X idx %u shd %d k%d grfshd 0x%X rgba %08X saa %d:", sub.grf, sub.indexCount, sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd,
                                sub.pshd->argba[0], sub.psaa ? sub.psaa->cid : -1);
                    for (const SHDPASS& ps : sub.pshd->apass)
                        std::printf(" [tex %d itex %d col %d test 0x%llX alpha 0x%llX abe %d]", int(ps.fTextured), ps.itexPass, ps.icolor,
                                    (unsigned long long)ps.test, (unsigned long long)ps.alpha, int(ps.fBlend));
                    const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
                    if (sel.pbmp)
                    {
                        std::vector<uint8_t> rgba; std::string err;
                        if (FDecodeTexture(sel.pbmp, sel.pclut, rgba, err))
                        {
                            int ca[4] = {}; uint64_t sr = 0, sg = 0, sb = 0;
                            for (size_t k = 0; k + 3 < rgba.size(); k += 4) { ++ca[std::min(3, rgba[k + 3] / 0x40)]; sr += rgba[k]; sg += rgba[k + 1]; sb += rgba[k + 2]; }
                            const size_t n = rgba.size() / 4;
                            std::printf(" bmp %dx%d alpha q %d %d %d %d mean rgb %d %d %d", sel.pbmp->dx, sel.pbmp->dy, ca[0], ca[1], ca[2], ca[3], int(sr / n), int(sg / n), int(sb / n));
                        }
                    }
                    std::printf("\n");
                }
            if (sgs.oid == s_oidRec && !pgeom->avtx.empty())
            {
                const VTXGLOB& v = pgeom->avtx[0];
                std::printf("   vtx0 A %08X B %08X pos %.1f %.1f %.1f n %zu mat %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f bones %zu\n", v.rgbaA, v.rgbaB, v.pos.x, v.pos.y, v.pos.z, pgeom->avtx.size(), mat[0][0], mat[0][1], mat[0][2], mat[1][0], mat[1][1], mat[1][2], mat[2][0], mat[2][1], mat[2][2], i < sgs.aamatBone.size() ? sgs.aamatBone[i].size() : 0);
            }
            // orig: 0x1689CC..0x168AC4: scale the group radius; unskinned groups use the VU sphere
            // test, skinned groups use 0x14F898 without a group plane test. Neither subtracts the radius.
            const bool fSkinned = !glob.abone.empty() || !pgeom->abone.empty();
            const float radius = glob.sRadius * maxScale(mat);
            float groupFade = 1.0f;
            bool fDistance = true;
            if (fTree)
            {
                groupFade = s_agVisTree[i];
                fDistance = groupFade != 0.0f;
            }
            else if (pv.irtgt < 0 && glob.fDraw)
            {
                const CM* pcmG = pgeom->meshk == MESHK_CelBorder ? &cmFadeCel : &cmFade;
                if (fSkinned) fDistance = FInsideCmMrd(pcmG, posWorld - cam.pos, glob.sMRD, &groupFade);
                else { groupFade = UCmSphereDistanceFade(pcmG, posWorld - cam.pos, glob.sMRD); fDistance = groupFade != 0.0f; }
            }
            const float fade = s_fNoFade ? 1.0f : std::min(wholeFade, groupFade);
            if (sgs.oid == s_oidFade)
                std::printf("fadegroup oid %d group %zu center %.9g %.9g %.9g radius %.9g mrd %.9g skinned %d visible %d whole %.9g groupfade %.9g alpha %.9g alpha2 %.9g\n",
                            sgs.oid, i, posWorld.x, posWorld.y, posWorld.z, radius, glob.sMRD, int(fSkinned), int(fDistance), wholeFade,
                            groupFade, sgs.gAlpha * fade, sgs.gAlpha2 * fade);
            if (!fDistance) { ++blk.cmrd; continue; }
            const bool fGeomBounds = !fTree && !glob.fDraw && pgeom->pgl && pgeom->pgl->sRadius >= 0 && !fSkinned;
            if (pv.irtgt < 0 && !fTree && !s_fNoFrustum && pgeom->meshk != MESHK_Effect && ((glob.fDraw && !fSkinned) || fGeomBounds))
            {
                const glm::vec3 posC = fGeomBounds ? glm::vec3(MulMat4Vec4(mat, glm::vec4(pgeom->pgl->posCenter, 1.0f))) : posWorld;
                const float r = fGeomBounds ? pgeom->pgl->sRadius * maxScale(mat) : radius;
                if (outside(posC, r)) { ++blk.cculled; continue; }
            }
            DRAWREC rec;
            rec.kind = kind;
            // orig: 0x168D64..0x168D94: both input alphas receive min(wholeFade, groupFade).
            // orig: 0x169198 RenderGlobCtx: render-target draws use unit alphas without the main-view tests.
            rec.alpha = pv.irtgt >= 0 ? 1.0f : std::clamp((fTree ? 1.0f : sgs.gAlpha) * fade, 0.0f, 1.0f);
            rec.alpha2 = pv.irtgt >= 0 || fTree ? 1.0f : std::clamp(sgs.gAlpha2 * fade, 0.0f, 1.0f);
            // the foreground kinds (0x169054..0x1690E8's table by rp - 3)
            if (sgs.fForeground && pv.irtgt < 0)
                switch (kind)
                {
                case 3: case 4: kind = rec.alpha == 1.0f ? 22 : 23; break;
                case 5: case 6: kind = 23; break;
                case 9: kind = 24; break;
                case 10: kind = 25; break;
                default: break;
                }
            if (pv.irtgt < 0 && (kind >= 22 && kind <= 25) != pv.fForeground) continue;
            rec.kind = kind;
            // RenderGlob: a faded group of the opaque kinds 3 / 4 is recorded as the sorted (blended) 5 / 6
            if (rec.alpha != 1.0f && pv.irtgt < 0 && (kind == 3 || kind == 4)) rec.kind = kind + 2;
            static const char* s_kinds = std::getenv("SLY2_KINDS");   // debugging: only these kinds (hex digits list)
            if (s_kinds)
            {
                char sz[8];
                std::snprintf(sz, sizeof(sz), ",%d,", rec.kind);
                if (!std::strstr((std::string(",") + s_kinds + ",").c_str(), sz)) continue;
            }
            // orig: 0x00169100 (RenderGlob): the record's sort key is the group's z order (its header +4) when its
            // flags have 0x400 (e.g. the sky's layers, world 2 / 8), else the squared distance
            // (a group without a draw record: the volume tree's record, keyed by its sphere in the sphere sets, 0x1D5808)
            float dSort = d;
            if (!glob.fDraw && glob.fSphere) dSort = glm::distance(glm::vec3(MulMat4Vec4(mat, glm::vec4(glob.posSphere, 1.0f))), cam.pos);
            rec.z = (glob.grfGroup & 0x400) ? glob.gZOrder : dSort * dSort;
            rec.pglob = &glob;
            rec.pglobGeom = pgeom;
            rec.pmat = pmatBillboardIn(blk.amatBillboard, sgs.amatGlob[i], glob.rtck);
            rec.psgs = &sgs;
            rec.iglob = i;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty()) rec.pamatBone = &sgs.aamatBone[i];
            blk.arec.push_back(rec);
            if (sgs.oid == s_oidFade)
                std::printf("fadesubmit oid %d group %zu kind %d alpha %.9g alpha2 %.9g\n", sgs.oid, i, rec.kind, rec.alpha, rec.alpha2);
        }
    };
    {
        // the split entries' own tests first (their parts share them)
        for (const RECITEM& item : items.aitem)
        {
            if (item.isplit < 0 || item.iglobFirst != 0) continue;
            int cmrd = 0, cculled = 0;
            apre[size_t(item.isplit)] = preOfSgs(scene.aglobset[item.isgsFirst], aagVis[size_t(item.isplit)], cmrd, cculled);
            g_renderstats.cmrd += cmrd;
            g_renderstats.cculled += cculled;
        }
        PassHelpers().ForBlocks(items.aitem.size(), 1, [&](size_t iitem, size_t) {
            const RECITEM& item = items.aitem[iitem];
            RECBLOCK& blk = ablock[iitem];
            blk.arec.clear();
            blk.amatBillboard.clear();
            blk.cmrd = blk.cculled = 0;
            if (item.isplit >= 0)
            {
                const SGSPRE& pre = apre[size_t(item.isplit)];
                if (pre.fDraw)
                    recordsOfGlobs(scene.aglobset[item.isgsFirst], pre, aagVis[size_t(item.isplit)], item.iglobFirst, item.iglobLim, blk);
                return;
            }
            thread_local std::vector<float> t_agVisTree;
            for (uint32_t isgs = item.isgsFirst; isgs < item.isgsLim; ++isgs)
            {
                const SCENEGLOBSET& sgs = scene.aglobset[isgs];
                const SGSPRE pre = preOfSgs(sgs, t_agVisTree, blk.cmrd, blk.cculled);
                if (pre.fDraw) recordsOfGlobs(sgs, pre, t_agVisTree, 0, SIZE_MAX, blk);
            }
        });
        for (size_t iitem = 0; iitem < items.aitem.size(); ++iitem)
        {
            const RECBLOCK& blk = ablock[iitem];
            arec.insert(arec.end(), blk.arec.begin(), blk.arec.end());
            g_renderstats.cmrd += blk.cmrd;
            g_renderstats.cculled += blk.cculled;
        }
    }
    if (static const bool s_fRecList = std::getenv("SLY2_RECLIST") != nullptr; s_fRecList && pv.irtgt < 0 && !pv.fForeground)   // debugging: the objects with records this frame
    {
        std::map<int, int> mp;
        const SCENEGLOBSET* psgsPrev = nullptr;
        for (const DRAWREC& r : arec) if (r.psgs != psgsPrev) { psgsPrev = r.psgs; ++mp[r.psgs->oid]; }
        for (const auto& [o, n] : mp) std::printf("reclist %d %d\n", o, n);
    }
    // extra draws (the frame only): RIPs' models at their own matrices (emitter.cpp)
    static std::vector<EXTRADRAW> s_aextra;
    s_aextra.clear();
    if (pv.irtgt < 0 && !pv.fForeground)
    {
        if (g_pfnExtraDraws) g_pfnExtraDraws(scene, s_aextra);
        CollectDrawDistanceGhosts(scene, s_aextra);   // (QoL draw distance above 1x: drawdist.h)
        CollectLaserDraws(scene, cam.pos, s_aextra);
        CollectPocketDraws(scene, s_aextra);
        CollectHackSwarmDraws(scene, s_aextra);
        if (static const bool s_fList = std::getenv("SLY2N_EXTRALIST") != nullptr; s_fList)
            for (const EXTRADRAW& x : s_aextra)
                std::fprintf(stderr, "extradraw sgs %d glob %d at %.0f %.0f %.0f\n", x.psgs ? x.psgs->oid : -1, x.iglob, x.mat[3].x, x.mat[3].y, x.mat[3].z);
    }
    for (const EXTRADRAW& x : s_aextra)
    {
        const GLOBSET& gs = *x.psgs->pglobset;
        if (x.iglob >= gs.aglob.size()) continue;
        const GLOB& glob = gs.aglob[x.iglob];
        const GLOB* pgeom = &glob;
        if (!glob.pgl && glob.iglobTemplate >= 0) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
        if (!pgeom->pgl) continue;
        const glm::vec3 posWorld = glm::vec3(x.mat * glm::vec4(glob.posCenter, 1.0f));
        float wholeFade = 1.0f, groupFade = 1.0f;
        if (x.fBounds)
        {
            wholeFade = UCmSphereDistanceFade(&cmFade, x.posBounds - cam.pos, x.sMRD);
            if (wholeFade == 0.0f) { ++g_renderstats.cmrd; continue; }
            if (!s_fNoFrustum && outside(x.posBounds, x.sBoundsRadius)) { ++g_renderstats.cculled; continue; }
        }
        if (glob.fDraw)
        {
            const bool fSkinned = !glob.abone.empty() || !pgeom->abone.empty();
            const CM* pcmG = pgeom->meshk == MESHK_CelBorder ? &cmFadeCel : &cmFade;
            if (fSkinned)
            {
                if (!FInsideCmMrd(pcmG, posWorld - cam.pos, glob.sMRD, &groupFade)) { ++g_renderstats.cmrd; continue; }
            }
            else
            {
                groupFade = UCmSphereDistanceFade(pcmG, posWorld - cam.pos, glob.sMRD);
                if (groupFade == 0.0f) { ++g_renderstats.cmrd; continue; }
                if (!s_fNoFrustum && outside(posWorld, glob.sRadius * maxScale(x.mat))) { ++g_renderstats.cculled; continue; }
            }
        }
        const float alpha = x.alpha * (s_fNoFade ? 1.0f : std::min(wholeFade, groupFade));
        DRAWREC rec;
        rec.kind = glob.rp & 0x1F;
        const float d = glm::distance(posWorld, cam.pos);
        rec.z = (glob.grfGroup & 0x400) ? glob.gZOrder : d * d;  // 0x169100 (as above)
        rec.pglob = &glob;
        rec.pglobGeom = pgeom;
        rec.pmat = pmatBillboard(x.mat, glob.rtck);
        rec.alpha = rec.alpha2 = std::clamp(alpha, 0.0f, 1.0f);
        if (rec.alpha != 1.0f && (rec.kind == 3 || rec.kind == 4)) rec.kind += 2;
        arec.push_back(rec);
        if (x.psgs->oid == s_oidFade)
            std::printf("fadesubmitextra oid %d group %zu kind %d alpha %.9g alpha2 %.9g\n", x.psgs->oid, x.iglob, rec.kind, rec.alpha, rec.alpha2);
    }
    if (pv.irtgt < 0 && [] { static const bool f = std::getenv("SLY2_PICK") != nullptr; return f; }())
    {
        glm::vec3 dir = cam.Dir();
        // SLY2_PICK="sx sy" (-1..1, y up): the ray through that screen point instead of the centre
        float sx = 0.0f, sy = 0.0f;
        if (std::sscanf(std::getenv("SLY2_PICK"), "%f %f", &sx, &sy) == 2)
        {
            const glm::mat4 inv = glm::inverse(matWorldToClip);
            const glm::vec4 a = inv * glm::vec4(sx, sy, 0.5f, 1.0f);
            dir = glm::normalize(glm::vec3(a) / a.w - cam.pos);
            std::printf("pick ray %.3f %.3f %.3f from %.0f %.0f %.0f\n", dir.x, dir.y, dir.z, cam.pos.x, cam.pos.y, cam.pos.z);
        }
        for (const SCENEGLOBSET& sgs : scene.aglobset)
        {
            if (!sgs.fTransform) continue;
            if (const char* r = std::getenv("SLY2_IBRANGE"))
            {
                size_t lo = 0, hi = 0;
                std::sscanf(r, "%zu,%zu", &lo, &hi);
                if (sgs.ib < lo || sgs.ib >= hi) continue;
                std::printf("in range: glob@%zu va 0x%X groups %zu\n", sgs.ib, sgs.vaGlob, sgs.pglobset->aglob.size());
                for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f));
                    std::printf("  grfMesh 0x%X cbone %d gZ %g\n", g.grfMesh, g.cbone, g.gZOrder);
                    for (const SUBGLOB& sub : g.asubglob)
                        if (sub.pshd)
                            for (int tp = 0; tp < 2; ++tp)
                            {
                                const TEXSEL sel = TexselFromShd(sub.pshd, 0, tp);
                                if (!sel.pbmp) continue;
                                std::vector<uint8_t> rgba; std::string err;
                                if (!FDecodeTexture(sel.pbmp, sel.pclut, rgba, err)) continue;
                                int ca[4] = {};
                                for (size_t k = 3; k < rgba.size(); k += 4) ++ca[std::min(3, rgba[k] / 0x40)];
                                std::printf("    shd 0x%X pass %d bmp 0x%X %dx%d alpha quartiles %d %d %d %d\n", sub.pshd->id, tp, sel.pbmp->id, sel.pbmp->dx, sel.pbmp->dy, ca[0], ca[1], ca[2], ca[3]);
                            }
                    std::printf("  #%zu rp %d meshk %d r %.0f at %.0f %.0f %.0f hidden %d shdk %d grfshd 0x%X A %08X\n", i, g.rp, g.meshk, g.sRadius, c.x, c.y, c.z,
                                int(g.fHidden || !sgs.afVisible[i]), g.asubglob.empty() || !g.asubglob[0].pshd ? -1 : g.asubglob[0].pshd->shdk,
                                g.asubglob.empty() || !g.asubglob[0].pshd ? 0 : g.asubglob[0].pshd->grfshd, g.avtx.empty() ? 0 : g.avtx[0].rgbaA);
                }
            }
            for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
            {
                const GLOB& glob = sgs.pglobset->aglob[i];
                const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(glob.posCenter, 1.0f));
                const float t = glm::dot(c - cam.pos, dir);
                const float dperp = glm::length(c - cam.pos - dir * t);
                const GLOB& gr = !glob.pgl && glob.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(glob.iglobTemplate)] : glob;
                const float sRPick = std::max(glob.sRadius, gr.pgl ? gr.pgl->sRadius : 0.0f);
                if (t > 0 && dperp < sRPick)
                    std::printf("pick: oid %d #%zu rp %d meshk %d r %.0f t %.0f hidden %d shd0 0x%X c %.0f %.0f %.0f fog %g z %g grf 0x%X A %08X\n", sgs.oid, i,
                                glob.rp, glob.meshk, sRPick, t, int(glob.fHidden || !sgs.afVisible[i]),
                                glob.asubglob.empty() || !glob.asubglob[0].pshd ? 0 : glob.asubglob[0].pshd->id, c.x, c.y, c.z, glob.uFog, glob.gZOrder,
                                glob.grfGroup, gr.avtx.empty() ? 0 : gr.avtx[0].rgbaA);
                if (glob.asubglob.size() && glob.asubglob[0].pshd && t > 0 && dperp < sRPick)
                {
                    const GLOB& g = glob.avtx.empty() && glob.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(glob.iglobTemplate)] : glob;
                    if (!g.avtx.empty())
                        std::printf("      shdk %d grfshd 0x%X grf 0x%X si %d A %08X B %08X unbaked %d inst %d\n", glob.asubglob[0].pshd->shdk,
                                    glob.asubglob[0].pshd->grfshd, glob.asubglob[0].grf, glob.asubglob[0].pshd->unSelfIllum,
                                    g.avtx[0].rgbaA, g.avtx[0].rgbaB, g.avtx[0].fUnbaked, glob.iglobTemplate);
                }
            }
        }
    }
    // DrawRpl 0x157FA0 / SortRpl 0x157AE0: by kind; kinds 1, 2, 5, 6, 10, 25 far to near, 21 near to far.
    auto lessRec = [](const DRAWREC& a, const DRAWREC& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        switch (a.kind)
        {
        case 1: case 2: case 5: case 6: case 10: case 25: return a.z > b.z;
        case 21: return a.z < b.z;
        default: return false;
        }
    };
#if SLY2_FASTPATHS
    // (the Switch: the keys sorted, then the records moved once. A key is the kind, the distance's bits in float order
    // (-0 as +0; reversed for far to near) and the place: ties keep their order as in the stable sort. NaN: the sort.)
    constexpr unsigned CBIT_PLACE = 27;
    static std::vector<uint64_t> s_akeyRec;
    static std::vector<DRAWREC> s_arecSorted;
    bool fKeys = arec.size() < (size_t(1) << CBIT_PLACE);
    s_akeyRec.clear();
    for (size_t i = 0; i < arec.size() && fKeys; ++i)
    {
        const DRAWREC& r = arec[i];
        uint32_t zkey = 0;
        if (r.kind < 0 || r.kind > 31) fKeys = false;
        else if (r.kind == 1 || r.kind == 2 || r.kind == 5 || r.kind == 6 || r.kind == 10 || r.kind == 25 || r.kind == 21)
        {
            if (std::isnan(r.z)) fKeys = false;
            const float z = r.z == 0.0f ? 0.0f : r.z;
            uint32_t u;
            std::memcpy(&u, &z, 4);
            u = (u & 0x80000000u) ? ~u : (u | 0x80000000u);
            zkey = r.kind == 21 ? u : ~u;
        }
        s_akeyRec.push_back(uint64_t(uint32_t(r.kind)) << 59 | uint64_t(zkey) << CBIT_PLACE | uint64_t(i));
    }
    if (fKeys)
    {
        std::sort(s_akeyRec.begin(), s_akeyRec.end());
        s_arecSorted.resize(arec.size());
        for (size_t i = 0; i < s_akeyRec.size(); ++i) s_arecSorted[i] = arec[size_t(s_akeyRec[i] & ((uint64_t(1) << CBIT_PLACE) - 1))];
        arec.swap(s_arecSorted);
    }
    else std::stable_sort(arec.begin(), arec.end(), lessRec);
#else
    std::stable_sort(arec.begin(), arec.end(), lessRec);
#endif

    // The frame's draw stream: per record its matrix / fog (RECGPU), per entry its draw data (DRAWGPU), per pass
    // one or two commands with their GL state; volumes, shadows and cel borders are special commands.
    static std::vector<RECGPU> s_arecgpu;
    static std::vector<DRAWGPU> s_adrawgpu;
    static std::vector<CMD> s_acmd;
    static std::vector<glm::mat4> s_amatBone;
    // (the bones: each record's place counted here; copied once, at the upload, straight into the backend's stream
    // memory where it has mapStream and the frame's ring is off; else gathered here and uploaded as before)
    const cane::draw::Packets* const ppkStream = cane::gl::ActiveBackend() && cane::gl::ActiveBackend()->packets &&
                                                 cane::gl::ActiveBackend()->packets->mapStream && !s_ring.pb ? cane::gl::ActiveBackend()->packets : nullptr;
    s_amatBone.clear();
    size_t cmatBone = 0;
    for (DRAWREC& rec : arec)
        if (rec.pamatBone)
        {
            rec.iboneFirst = uint32_t(cmatBone);
            cmatBone += rec.pamatBone->size();
            if (!ppkStream) s_amatBone.insert(s_amatBone.end(), rec.pamatBone->begin(), rec.pamatBone->end());
        }
    s_arecgpu.clear();
    s_adrawgpu.clear();
    s_acmd.clear();
    // the warped globs' state vectors and their WRs' matrices (wr.h; glob.vert adds sum(matrix x state))
    static std::vector<glm::vec4> s_awarpst;
    static std::vector<glm::mat4> s_amatWarp;
    static std::unordered_map<const WRBG*, uint32_t> s_mpwrbgist;
    static std::unordered_map<const WR*, uint32_t> s_mpwrimat;
    static const bool s_fNoWarp = std::getenv("SLY2_NOWARP") != nullptr;   // debugging: no space warps
    s_awarpst.clear();
    s_amatWarp.clear();
    s_mpwrbgist.clear();
    s_mpwrimat.clear();
    auto warpOf = [&](const GLOB& geom) {
        const WRBG* pw = geom.pwrbg.get();
        const WR* pwr = PwrOfWrbg(pw);
        if (!pwr || s_fNoWarp || !(pw->fDpos || pw->fDuv)) return glm::uvec4(0u);
        const size_t cvtx = geom.avtx.size();
        const uint32_t cmat = uint32_t(std::clamp(pw->cmat, 0, 4));
        if (cmat == 0 || pw->astate.size() < cmat * cvtx) return glm::uvec4(0u);
        auto [itst, fNewSt] = s_mpwrbgist.try_emplace(pw, uint32_t(s_awarpst.size()) + 1u);
        if (fNewSt) s_awarpst.insert(s_awarpst.end(), pw->astate.begin(), pw->astate.begin() + std::ptrdiff_t(cmat * cvtx));
        auto [itmat, fNewMat] = s_mpwrimat.try_emplace(pwr, uint32_t(s_amatWarp.size()));
        if (fNewMat)
        {
            s_amatWarp.insert(s_amatWarp.end(), std::begin(pwr->amatDpos), std::end(pwr->amatDpos));
            s_amatWarp.insert(s_amatWarp.end(), std::begin(pwr->amatDuv), std::end(pwr->amatDuv));
        }
        return glm::uvec4(itst->second, uint32_t(cvtx), itmat->second, cmat | (pw->fDpos ? 0x100u : 0u) | (pw->fDuv ? 0x200u : 0u));
    };
    s_avtxShadow2.clear();
    // Kinds drawn in record order with no sort (DrawRpl): their commands are regrouped by pass and state, so
    // the runs are long; the sorted (blended) kinds keep their order, merging only neighbours.
    static const bool s_fNoRegroup = std::getenv("SLY2_NOREGROUP") != nullptr;
    auto fSortedKind = [](int kind) {
        switch (kind)
        {
        case 1: case 2: case 5: case 6: case 10: case 25: case 21: return true;
        default: return false;
        }
    };
    // (the sorted kinds' draws that cannot overlap on screen joined into runs of one state: SortJoinSegment;
    // SLY2N_SORTJOIN=0: their order as recorded)
    static const bool s_fSortJoin = !(std::getenv("SLY2N_SORTJOIN") && std::atoi(std::getenv("SLY2N_SORTJOIN")) == 0);
    const bool fSortJoin = s_fSortJoin && !s_fNoRegroup;
    struct SEGMENT { size_t icmdFirst, icmdLim; bool fRegroup; };
    std::vector<SEGMENT> aseg;
    size_t icmdSeg = 0;
    int kindSeg = -1;
    auto endSegment = [&](bool fRegroup) {
        if (s_acmd.size() > icmdSeg) aseg.push_back({icmdSeg, s_acmd.size(), fRegroup});
        icmdSeg = s_acmd.size();
    };
    static const char* s_cull = std::getenv("SLY2_CULL");   // "none": no back-face culling
    const bool fCullNone = s_cull && !std::strcmp(s_cull, "none");
    // the particles (render kind 12, emitter.cpp) where the frame draws that kind
    bool fParticles = pv.irtgt >= 0 || pv.fForeground;
    auto addParticles = [&]() {
        if (fParticles) return;
        fParticles = true;
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        CMD cmd;
        cmd.cmdk = CMDK_Particles;
        s_acmd.push_back(cmd);
        endSegment(false);
    };
    // The type-2 shadows (render kind 7: the characters' cast shadows) mark with the kind-7 hulls; the frame's alpha
    // keeps the characters' own pixels out (DATE), as on the PS2.
    bool fShadow2 = false;
    auto addShadow2 = [&]() {
        if (fShadow2 || !opts.fShadows || pv.fForeground) return;
        fShadow2 = true;
        // all the volumes in one count, marked once (their union: a character's overlapping pieces, as the PS2's
        // green count, darken the floor once)
        CMD cmdAll;
        cmdAll.firstIndex = uint32_t(s_avtxShadow2.size());
        cmdAll.count = 0;
        cmdAll.idraw = 0;
        cmdAll.order = 8;
        auto fCaster = [](const DRAWREC& rec) {
            const GLOB& geom = *rec.pglobGeom;
            return geom.meshk == MESHK_Effect && rec.psgs && !geom.asubfx.empty() && geom.asubfx[0].pshd;
        };
        auto addCaster = [&](const DRAWREC& rec, const CMD& cmd) {
            cmdAll.count += cmd.count;
            cmdAll.idraw = std::max(cmdAll.idraw, cmd.idraw);
            if (!(rec.pglobGeom->asubfx[0].pshd->grfshd & 8)) cmdAll.order = 0;   // (DATE unless every shader has flag 8)
            cmdAll.prec = &rec;
        };
#if SLY2_FASTPATHS
        // (the Switch: the volumes built on the frame's helpers, each caster's into its own vertices, joined in order)
        struct CASTER { const DRAWREC* prec = nullptr; std::vector<glm::vec3> avtx; CMD cmd; bool f = false, fClosed = false; std::array<float, 2> zrange{}; };
        static std::vector<CASTER> s_acaster;
        size_t ccaster = 0;
        for (const DRAWREC& rec : arec)
            if (fCaster(rec))
            {
                if (ccaster == s_acaster.size()) s_acaster.emplace_back();
                s_acaster[ccaster++].prec = &rec;
            }
        const bool fShBoundsPass = !g_fMultiviewPass && FShadowDepthBounds();   // (asked here: it may query GL)
        PassHelpers().ForBlocks(ccaster, 1, [&](size_t iFirst, size_t iLim) {
            for (size_t i = iFirst; i < iLim; ++i)
            {
                CASTER& c = s_acaster[i];
                c.avtx.clear();
                c.cmd = CMD{};
                c.f = FBuildShadow2(*c.prec->psgs, c.prec->iglob, *c.prec->pglobGeom, cam.pos, 52.0f, c.avtx, c.cmd);
                // (Fast shadows: each caster's closed-surface test here, on the helpers and once for every eye, not
                // on the main thread at each draw: it was a sixth of the Quest's main thread in VR)
                // (and for the depth bounds test's near side: DrawShadow2)
                c.fClosed = c.f && (FFastShadowsNow() || fShBoundsPass) && FClosedOriented(c.avtx.data(), uint32_t(c.avtx.size()));
                if (c.f && fShBoundsPass) c.zrange = ZRangeOfCaster(pv.matWorldToClip, c.avtx.data(), uint32_t(c.avtx.size()));
            }
        });
        s_acvtxShadow2Caster.clear();
        s_afClosedShadow2Caster.clear();
        s_azrangeShadow2Caster.clear();
        s_matShBounds = pv.matWorldToClip;
        for (size_t i = 0; i < ccaster; ++i)
        {
            const CASTER& c = s_acaster[i];
            if (!c.f) continue;
            s_avtxShadow2.insert(s_avtxShadow2.end(), c.avtx.begin(), c.avtx.end());
            s_acvtxShadow2Caster.push_back(uint32_t(c.avtx.size()));
            s_afClosedShadow2Caster.push_back(c.fClosed);
            if (fShBoundsPass) s_azrangeShadow2Caster.push_back(c.zrange);
            addCaster(*c.prec, c.cmd);
        }
#else
        for (const DRAWREC& rec : arec)
        {
            if (!fCaster(rec)) continue;
            CMD cmd;
            if (!FBuildShadow2(*rec.psgs, rec.iglob, *rec.pglobGeom, cam.pos, 52.0f, s_avtxShadow2, cmd)) continue;
            addCaster(rec, cmd);
        }
#endif
        if (cmdAll.count == 0) return;
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        cmdAll.cmdk = CMDK_Shadow2;
        s_acmd.push_back(cmdAll);
        endSegment(false);
    };
    // A shadow phase: kinds 7 / 8 between 0x16DA48 (begin: the frame's alpha kept) and 0x16DD58 (combine: the marked
    // pixels darkened). count: bit 0 kind 7 drawn, bit 1 kind 8.
    const bool fFbaPass = pv.irtgt < 0 && !pv.fForeground;
    int icmdShPhase = -1;
    bool fFbaOpen = true;   // (the FBA kinds' stencil bits not cleared yet)
    auto shadowCmd = [&](CMDK cmdk) {
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        CMD cmd;
        cmd.cmdk = cmdk;
        s_acmd.push_back(cmd);
        endSegment(false);
    };
    auto shadowPhase = [&](int kind) {
        const bool fShKind = opts.fShadows && !pv.fForeground && (kind == 7 || kind == 8);
        if (fFbaOpen && (kind > 6 || kind < 0) && !fShKind)
        {
            shadowCmd(CMDK_FbaClear);
            fFbaOpen = false;
        }
        if (fShKind) fFbaOpen = false;   // (the combine clears them)
        if (icmdShPhase >= 0 && !fShKind)
        {
            shadowCmd(CMDK_ShadowCombine);
            s_acmd.back().count = s_acmd[size_t(icmdShPhase)].count;
            icmdShPhase = -1;
        }
        else if (fShKind && icmdShPhase < 0)
        {
            shadowCmd(CMDK_ShadowBegin);
            icmdShPhase = int(s_acmd.size()) - 1;
            s_acmd.back().order = uint32_t(kind);
        }
        else if (fShKind && kind == 8) shadowCmd(CMDK_ShadowKind8);
    };
    // (sorted kinds: SortJoinSegment) the pass draws' screen rectangles (CMD::irect); the whole frame without one
    const STEREOVIEW* const psvRect = pv.pstereo;
    static std::vector<SORTRECT> s_asortrect;
    s_asortrect.clear();
    SORTRECT srFull;
    srFull.a[0] = PIXRECT{0, 0, pv.dx, pv.dy};
    if (psvRect) srFull.a[1] = srFull.a[0];
    // (measurement: SLY2N_SKIPKINDS=a,b,..: the records of those kinds not drawn: what each costs the GPU)
    static const uint64_t s_grfSkipKinds = [] {
        uint64_t grf = 0;
        if (const char* sz = std::getenv("SLY2N_SKIPKINDS"))
            for (const char* p = sz; *p;)
            {
                char* end = nullptr;
                const long k = std::strtol(p, &end, 10);
                if (end == p) { ++p; continue; }
                if (k >= 0 && k < 64) grf |= uint64_t(1) << k;
                p = end;
            }
        return grf;
    }();
    for (const DRAWREC& rec : arec)
    {
#if SLY2_FASTPATHS
        PrefetchRecordsAhead(arec, size_t(&rec - arec.data()));
#endif
        if (s_grfSkipKinds && rec.kind >= 0 && rec.kind < 64 && ((s_grfSkipKinds >> rec.kind) & 1)) continue;
        const GLOB& glob = *rec.pglob;
        const GLOB& geom = *rec.pglobGeom;
        if (rec.kind != kindSeg) shadowPhase(rec.kind);
        if (rec.kind == 7 && icmdShPhase >= 0)
        {
            const size_t ccmd = s_acmd.size();
            addShadow2();
            if (s_acmd.size() != ccmd) s_acmd[size_t(icmdShPhase)].count |= 1;
        }
        if (rec.kind > 12) addParticles();
        if (rec.kind != kindSeg) { endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg)); kindSeg = rec.kind; }
        if (s_adrawgpu.size() + geom.asubglob.size() * (g_fGlobBindless || g_fGlobArrays ? 16 : 1) + 1 > CDRAW_MAX) break;
        const uint32_t irec = uint32_t(s_arecgpu.size());
        if (geom.meshk == MESHK_Strips && opts.fMeshes)
        {
            // an instance lit at its own place draws its own copy of the vertices (SCENEGLOBSET::apglInst)
            const GLGLOB* pglDraw = rec.psgs && rec.iglob < rec.psgs->apglInst.size() && rec.psgs->apglInst[rec.iglob] &&
                                            rec.psgs->apglInst[rec.iglob]->vao
                                        ? rec.psgs->apglInst[rec.iglob]
                                        : geom.pgl;
            // CM_BuildFogRecip(g_cm, qw 9, uFog): uFog unset (FLT_MAX) counts as 1.
            const float uFog = glob.uFog == std::numeric_limits<float>::max() ? 1.0f : glob.uFog;
            s_arecgpu.push_back({*rec.pmat, glm::vec4(opts.sFogNear, opts.sFogFar / std::max(opts.sFogNear, 1.0f),
                                                      opts.uFogMax * uFog, opts.fFog ? 1.0f : 0.0f),
                                 glm::uvec4(rec.iboneFirst, rec.pamatBone ? uint32_t(rec.pamatBone->size()) : 0u, uint32_t(pglDraw->baseVertex), 0u)});
            s_arecgpu.back().warp = warpOf(geom);
            // orig: P_G 638 (0x16A000 SPECPARAMS): the view-angle alpha. Unlit groups only: a lit group's gain scales its
            // normals' brightness (w) for the lighting (522) instead (TODO(render): not modelled)
            if (geom.iSpec != -1 && !(geom.grfMesh & 1))
            {
                RECGPU& r = s_arecgpu.back();
                r.spec0 = glm::vec4(geom.agSpec[0], geom.agSpec[1], geom.agSpec[2], float(geom.iSpec));
                r.spec1 = glm::vec4(geom.agClamp[0], geom.agClamp[1], 1.0f, 0.0f);
                r.specPos = glm::vec4(geom.posSpec, 0.0f);
            }
            CMD base;
            base.vao = pglDraw->vao;
            // (perf-4) every vertex in front of the near plane with a wide margin for the GPU's rounding (in each eye's
            // view in VR): glob.geom would pass each triangle through unchanged (its test is w - z >= 0 per vertex, linear
            // in the position), so the run may draw without it. Only the arena's own unmoved, unskinned, unwarped
            // vertices, whose sphere (SetBounds) holds them; the sphere's world radius by the matrix's Frobenius norm
            // (at least its largest stretch).
            if (g_fGlobNoGeom && !rec.pamatBone && s_arecgpu.back().warp.x == 0 && pglDraw->fBoundsExact && pglDraw->sRadius >= 0.0f)
            {
                const glm::mat4& m = *rec.pmat;
                const glm::vec3 c = glm::vec3(m * glm::vec4(pglDraw->posCenter, 1.0f));
                const float frob = std::sqrt(glm::dot(glm::vec3(m[0]), glm::vec3(m[0])) + glm::dot(glm::vec3(m[1]), glm::vec3(m[1])) +
                                             glm::dot(glm::vec3(m[2]), glm::vec3(m[2])));
                const float r = pglDraw->sRadius * frob;
                auto fInFront = [&](const glm::mat4& mc) {
                    const glm::vec4 l = glm::vec4(mc[0][3] - mc[0][2], mc[1][3] - mc[1][2], mc[2][3] - mc[2][2], mc[3][3] - mc[3][2]);
                    const float gl = glm::length(glm::vec3(l));
                    const float dMin = glm::dot(glm::vec3(l), c) + l.w - r * gl;   // (w - z = l . (p, 1))
                    const float margin = 1e-3f * (gl * (glm::length(c) + r) + std::fabs(l.w)) + 1e-3f;
                    return std::isfinite(dMin) && std::isfinite(margin) && dMin > margin;
                };
                base.fNoClip = pv.pstereo ? fInFront(pv.pstereo->amatWorldToClip[0]) && fInFront(pv.pstereo->amatWorldToClip[1])
                                          : fInFront(pv.matWorldToClip);
            }
            // (sorted kinds) the pixels its entries' draws can reach in each eye (SortJoinSegment): the arena's own unmoved,
            // unskinned, unwarped vertices only
            const bool fSubRects = fSortJoin && fSortedKind(rec.kind) && !rec.pamatBone && s_arecgpu.back().warp.x == 0 && pglDraw->fBoundsExact &&
                                   pglDraw->asphereSub.size() == geom.asubglob.size();
            // the bound of the record's vertex alpha x object alpha, for the alpha tests it passes on every fragment
            // (AllPassOf); 0: not decided (the view-angle alpha changes it per vertex)
            const bool fSpec = geom.iSpec != -1 && !(geom.grfMesh & 1);
            const float vMin = fSpec ? 0.0f : float(pglDraw->aMinA) * rec.alpha;
            const float vMax = fSpec ? std::numeric_limits<float>::infinity() : float(pglDraw->aMaxA) * rec.alpha;
            // P_G culls back faces unless the mesh is double-sided (flag 0x10 -> param 0x80).
            const uint8_t fCull = !(geom.grfMesh & 0x10) && !fCullNone;
            base.baseVertex = pglDraw->baseVertex;
            base.prec = &rec;
            for (const SUBGLOB& sub : geom.asubglob)
            {
                if (!sub.pshd || sub.indexCount == 0) continue;
                // (debug: SLY2N_SKIPSHD=id: that shader's entries not drawn)
                if (static const int s_shdSkip = std::getenv("SLY2N_SKIPSHD") ? std::atoi(std::getenv("SLY2N_SKIPSHD")) : -2; sub.pshd->id == s_shdSkip) continue;
                ++g_renderstats.cdraw;
                base.idraw = uint32_t(s_adrawgpu.size());
                base.count = sub.indexCount;
                base.firstIndex = pglDraw->firstIndex + sub.firstIndex;
                base.psub = &sub;
                base.irect = UINT32_MAX;
                if (fSubRects)
                {
                    const glm::vec4& sph = pglDraw->asphereSub[size_t(&sub - geom.asubglob.data())];
                    if (sph.w >= 0.0f)
                    {
                        SORTRECT sr;
                        sr.a[0] = PixelRectOfSphere(psvRect ? psvRect->amatWorldToClip[0] : pv.matWorldToClip, *rec.pmat, glm::vec3(sph), sph.w, pv.dx, pv.dy);
                        if (psvRect) sr.a[1] = PixelRectOfSphere(psvRect->amatWorldToClip[1], *rec.pmat, glm::vec3(sph), sph.w, pv.dx, pv.dy);
                        base.irect = uint32_t(s_asortrect.size());
                        s_asortrect.push_back(sr);
                        // (debug: SLY2N_SORTRECTCHECK=1: each entry's vertices projected here, checked inside its rectangle)
                        if (static const bool s_fRectCheck = std::getenv("SLY2N_SORTRECTCHECK") != nullptr; s_fRectCheck)
                        {
                            const glm::dmat4 mc = glm::dmat4(psvRect ? psvRect->amatWorldToClip[0] : pv.matWorldToClip) * glm::dmat4(*rec.pmat);
                            for (uint32_t k = sub.firstIndex; k < sub.firstIndex + sub.indexCount; ++k)
                            {
                                const glm::dvec4 q = mc * glm::dvec4(glm::dvec3(geom.avtx[geom.aindex[k]].pos), 1.0);
                                if (q.w <= 0.0) continue;
                                const double x = (q.x / q.w * 0.5 + 0.5) * pv.dx, y = (q.y / q.w * 0.5 + 0.5) * pv.dy;
                                if (x < 0 || y < 0 || x > pv.dx || y > pv.dy) continue;
                                if (x < sr.a[0].x0 || x > sr.a[0].x1 || y < sr.a[0].y0 || y > sr.a[0].y1)
                                    std::printf("sortrectcheck: rec %zu sub %zu vertex %u at %.1f %.1f outside %d %d %d %d (sphere %.2f %.2f %.2f r %.3f)\n",
                                                size_t(&rec - arec.data()), size_t(&sub - geom.asubglob.data()), geom.aindex[k], x, y, sr.a[0].x0, sr.a[0].y0,
                                                sr.a[0].x1, sr.a[0].y1, sph.x, sph.y, sph.z, sph.w);
                            }
                        }
                    }
                }
                if (sub.pshd->shdk == 3)
                {
                    // orig: 0x0016A000 (one projected-volume batch can have several entries of the same material).
                    // Keep that hull together through all mask/fill/clear passes. The entries can be open pieces
                    // of one closed hull: clearing the mask after each piece lights surfaces outside the volume.
                    if (!s_acmd.empty())
                    {
                        CMD& prev = s_acmd.back();
                        if (prev.cmdk == CMDK_Volume && prev.prec == &rec && prev.psub->pshd == sub.pshd &&
                            prev.psub->psaa == sub.psaa && prev.psub->ibatch == sub.ibatch &&
                            prev.vao == base.vao && prev.baseVertex == base.baseVertex &&
                            prev.firstIndex + prev.count == base.firstIndex)
                        {
                            prev.count += base.count;
                            continue;
                        }
                    }
                    s_adrawgpu.push_back({0.0f, 0.0f, 1.0f, irec});
                    endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
                    CMD cmd = base;
                    cmd.cmdk = CMDK_Volume;
                    s_acmd.push_back(cmd);
                    endSegment(false);
                    continue;
                }
                const DRAWGPU drawgpu{sub.psaa ? sub.psaa->du / 4096.0f : 0.0f, sub.psaa ? sub.psaa->dv / 4096.0f : 0.0f, rec.alpha, irec};
                if (!g_fGlobBindless && !g_fGlobArrays) s_adrawgpu.push_back(drawgpu);
                for (const PASSCMD& pc : SubgpuOf(sub, pglDraw->vao, fCull).apasscmd)
                {
                    uint32_t istate = pc.istate;
                    if (s_astate[istate].ate)
                    {
                        const ALLPASS ap = AllPassOf(istate);
                        if (vMin > 0.0f && vMin >= ap.vNeed)
                        {
                            if (ap.istateAlt == UINT32_MAX) continue;   // (the AFAIL sub-draw: keeps nothing)
                            istate = ap.istateAlt;
                        }
                        else if ((fSingleSample || vMin == vMax) && vMax <= ap.vFail)
                        {
                            if (ap.istateAltFail == UINT32_MAX) { ++g_renderstats.cfailAll; continue; }   // (the passing sub-draw: keeps nothing)
                            istate = ap.istateAltFail;
                        }
                    }
                    base.istate = fFbaPass && (rec.kind == 4 || rec.kind == 6) ? IstateFba(istate) : istate;
                    base.order = pc.order;
                    // (bindless / arrays: each pass draw its own draw entry, for its texture handle or layer: s_atexh)
                    if (g_fGlobBindless || g_fGlobArrays)
                    {
                        base.idraw = uint32_t(s_adrawgpu.size());
                        s_adrawgpu.push_back(drawgpu);
                    }
                    s_acmd.push_back(base);
                }
            }
        }
        else if (geom.meshk == MESHK_Indexed && opts.fShadows && geom.pgl->cindex > 0 && !geom.asubfx.empty() && geom.asubfx[0].pshd)
        {
            s_arecgpu.push_back({*rec.pmat, glm::vec4(1, 1, 0, 0)});
            endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
            CMD cmd;
            cmd.cmdk = CMDK_Shadow;
            if (icmdShPhase >= 0) s_acmd[size_t(icmdShPhase)].count |= rec.kind == 8 ? 2u : 1u;
            cmd.vao = geom.pgl->vao;
            cmd.count = uint32_t(geom.pgl->cindex);
            cmd.firstIndex = geom.pgl->firstIndex;
            cmd.baseVertex = geom.pgl->baseVertex;
            cmd.idraw = uint32_t(s_adrawgpu.size());
            cmd.prec = &rec;
            s_adrawgpu.push_back({0.0f, 0.0f, 1.0f, irec});
            s_acmd.push_back(cmd);
            endSegment(false);
        }
        else if (geom.meshk == MESHK_CelBorder && opts.fCelBorders && geom.pgl && geom.pgl->cedge > 0)
        {
            endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
            CMD cmd;
            cmd.cmdk = CMDK_Cel;
            cmd.prec = &rec;
            s_acmd.push_back(cmd);
            endSegment(false);
        }
    }
    shadowPhase(-1);
    addParticles();
    endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
    // Regroup: within a segment of an unsorted kind, by (pass, sub-draw) then state (stable: record order inside).
    static std::vector<uint64_t> s_akey;
    static std::vector<CMD> s_acmdTmp;
    // (perf-5, g_fGlobArrays) the states ranked by their array class (the same but for the texture, one page) first, so
    // the states a run can join are neighbours in the regroup as they are with bindless (where any texture joins)
    static std::vector<uint32_t> s_arankIstate;
    static uint32_t s_genRank = 0;
    static size_t s_cstateRank = 0;
    // Opt-in (SLY2N_ARRAYRANK=1): not the same order, so coplanar draws of different states can swap which one shows
    // (single pixels in Paris); with it Paris draws ~1140 runs instead of ~1400 (bound textures: ~1470).
    static const bool s_fRank = std::getenv("SLY2N_ARRAYRANK") && std::atoi(std::getenv("SLY2N_ARRAYRANK")) != 0;
    const bool fRank = g_fGlobArrays && s_fRank;
    if (fRank && (s_genRank != s_genArray || s_cstateRank != s_astate.size()))
    {
        static std::vector<std::pair<uint64_t, uint32_t>> s_aord;
        s_aord.clear();
        for (uint32_t i = 0; i < s_astate.size(); ++i)
        {
            const LAYEROFSTATE& e = LayerEntryOfIstate(i);
            s_aord.push_back({(uint64_t(e.ref == LAYERREF_NONE ? 0xFFFFFFFFu : e.iclass) << 32) | i, i});
        }
        std::sort(s_aord.begin(), s_aord.end());
        s_arankIstate.assign(s_astate.size(), 0);
        for (uint32_t r = 0; r < s_aord.size(); ++r) s_arankIstate[s_aord[r].second] = r;
        s_genRank = s_genArray, s_cstateRank = s_astate.size();
    }
    for (const SEGMENT& seg : aseg)
    {
        if (!seg.fRegroup || seg.icmdLim - seg.icmdFirst < 2) continue;
        s_akey.clear();
        for (size_t i = seg.icmdFirst; i < seg.icmdLim; ++i)
        {
            const CMD& c = s_acmd[i];
            const uint32_t kState = fRank && c.istate < s_arankIstate.size() ? s_arankIstate[c.istate] : c.istate;
            s_akey.push_back((uint64_t(std::min(c.order, 0xFFFu)) << 52) | (uint64_t(kState & 0xFFFFFFu) << 28) | uint64_t(i - seg.icmdFirst));
        }
        SortKeysUnique(s_akey);
        s_acmdTmp.assign(s_acmd.begin() + std::ptrdiff_t(seg.icmdFirst), s_acmd.begin() + std::ptrdiff_t(seg.icmdLim));
        for (size_t i = 0; i < s_akey.size(); ++i) s_acmd[seg.icmdFirst + i] = s_acmdTmp[size_t(s_akey[i] & 0xFFFFFFFu)];
    }
    // The sorted kinds' segments: the draws that cannot meet joined into runs (SortJoinSegment), by the run rule below
    // (the run rule: the same state, or the same but for the texture (bindless handles / array layers), or with
    // per-draw state (FDrawStateOn) the same but for the texture, colour source, fog black and the pair's blends)
    const bool fDrawState = FDrawStateOn();
    auto fJoinState = [](uint32_t istateA, uint32_t istateB) {
        return istateA == istateB || (g_fGlobBindless && FSameButTexture(istateA, istateB)) || (g_fGlobArrays && FSameButLayer(istateA, istateB));
    };
    auto fJoin = [fDrawState, fJoinState](uint32_t istateA, uint32_t istateB) {
        return fJoinState(istateA, istateB) || (fDrawState && FSameDrawState(istateA, istateB));
    };
    if ((fSortJoin || fDrawState) && !arec.empty())
    {
        // (each draw keeps the program it had: a run draws without the near-clip stage only when all its draws may
        // (perf-4), so each draw takes the choice of its run in the order as recorded (by the rule without per-draw
        // state: the programs as before it), and runs join only draws of one choice, below and in SortJoinSegment)
        for (size_t i = 0; i < s_acmd.size();)
        {
            if (s_acmd[i].cmdk != CMDK_Pass) { ++i; continue; }
            size_t j = i;
            uint8_t f = 1;
            for (; j < s_acmd.size() && s_acmd[j].cmdk == CMDK_Pass && fJoinState(s_acmd[i].istate, s_acmd[j].istate); ++j) f &= s_acmd[j].fNoClip;
            for (; i < j; ++i) s_acmd[i].fNoClip = f, s_acmd[i].fClipFixed = 1;
        }
    }
    if (fSortJoin && !arec.empty())
    {
        static int s_cfDbg = 0;
        if (pv.irtgt < 0 && !pv.fForeground) ++s_cfDbg;
        for (const SEGMENT& seg : aseg)
        {
            if (seg.fRegroup) continue;
            bool fSorted = true;
            for (size_t i = seg.icmdFirst; i < seg.icmdLim && fSorted; ++i)
                fSorted = s_acmd[i].cmdk == CMDK_Pass && s_acmd[i].prec && fSortedKind(s_acmd[i].prec->kind);
            // (debug: SLY2N_SORTDBG=N: the N-th frame's sorted segments, each draw's record, entry, state and rectangle)
            static const int s_cfSortDbg = std::getenv("SLY2N_SORTDBG") ? std::atoi(std::getenv("SLY2N_SORTDBG")) : 0;
            if (fSorted && s_cfSortDbg && s_cfDbg == s_cfSortDbg && pv.irtgt < 0 && !pv.fForeground)
                for (size_t i = seg.icmdFirst; i < seg.icmdLim; ++i)
                {
                    const CMD& c = s_acmd[i];
                    const DSTATE& st = s_astate[c.istate];
                    const SORTRECT& sr = c.irect < s_asortrect.size() ? s_asortrect[c.irect] : srFull;
                    std::printf("sortdbg k%d rec %zu sub %zu ord %u ist %u cls %u tex %u blend %d a %llx dm %d cm %d df %d ate %d/%d/%d/%d ic %d fog %d cull %d fba %d vao %u rect %d %d %d %d\n", c.prec->kind,
                                size_t(c.prec - arec.data()), size_t(c.psub - c.prec->pglobGeom->asubglob.data()), c.order, c.istate, g_fGlobBindless ? HandleEntryOfIstate(c.istate).iclass : g_fGlobArrays ? LayerEntryOfIstate(c.istate).iclass : c.istate, st.tex, st.fBlend,
                                (unsigned long long)st.alpha, st.depthMask, st.colorMask, st.depthFunc, st.ate, st.atst, st.aref, st.afailMode, st.icolor, st.fFogBlack, st.fCull, st.fFba, st.vao,
                                sr.a[0].x0, sr.a[0].y0, sr.a[0].x1, sr.a[0].y1);
                }
            if (fSorted) SortJoinSegment(s_acmd, seg.icmdFirst, seg.icmdLim, s_asortrect, srFull, fJoin);
        }
    }
    // The shadow phases in which nothing writes the frame's alpha: the hulls and volumes write only the stencil, so the
    // frame's alpha stays what the phase's copy would hold, and a tile GPU may read it in place (ShadowFetchMode)
    for (size_t i = 0; i < s_acmd.size(); ++i)
    {
        if (s_acmd[i].cmdk != CMDK_ShadowBegin) continue;
        bool fKept = true;
        size_t j = i + 1;
        for (; j < s_acmd.size() && s_acmd[j].cmdk != CMDK_ShadowCombine; ++j)
        {
            const CMD& c = s_acmd[j];
            if (c.cmdk == CMDK_Pass ? (s_astate[c.istate].colorMask & 2) != 0
                                    : !(c.cmdk == CMDK_Shadow || c.cmdk == CMDK_Shadow2 || c.cmdk == CMDK_ShadowKind8 || c.cmdk == CMDK_FbaClear))
                fKept = false;
        }
        s_acmd[i].fAlphaKept = fKept && j < s_acmd.size();
    }
    // (vkalpha) the draws while the stencil's FBA bit is 0 in every pixel: no stencil (IstateNoStencil): their REPLACE
    // with 0 changes nothing. The bit is 0 at the pass's start (its clear) until the first command that can set it (an
    // FBA draw, a shadow phase's begin / hulls / prisms / mark), and again after the commands that clear it (the phase's
    // combine clears the whole stencil, FbaClear the bit). The cel borders and particles leave the stencil alone; a
    // volume sets the bit only in pixels it clears again (the textured one through the same front faces with the same
    // depth test; the untextured one through the back faces without the depth test, a superset of where it set it).
    // Head (SLY2N_STENCILHEAD=0: off): before the first setter, kinds 1-3; tail (SLY2N_STENCILTAIL=0: off): after a
    // clear, mostly kind 10.
    // A stretch in which the bit is set but nothing reads it before it is cleared (no shadow phase, no volume between
    // the first setter and the clear or the pass's end, e.g. a pass whose FBA kinds draw without a shadow phase): its
    // draws, the FBA ones too, need no stencil either (SLY2N_STENCILDEAD=0: off).
    if (s_fStencilHead || s_fStencilTail || s_fStencilDead)
    {
        bool fZero = true, fHead = true;
        size_t icmdDirty = 0;   // (the open stretch's first setter, while !fZero)
        bool fRead = false;     // (something in the open stretch may read the bit)
        auto closeDirty = [&](size_t icmdLim) {
            if (!s_fStencilDead || fRead) return;
            for (size_t k = icmdDirty; k < icmdLim; ++k)
            {
                CMD& c = s_acmd[k];
                if (c.cmdk == CMDK_Pass && (s_astate[c.istate].colorMask & 2)) c.istate = IstateNoStencil(c.istate);
            }
        };
        for (size_t i = 0; i < s_acmd.size(); ++i)
        {
            CMD& c = s_acmd[i];
            switch (c.cmdk)
            {
            case CMDK_Pass:
                if (s_astate[c.istate].fFba)
                {
                    if ((s_astate[c.istate].colorMask & 2) && fZero) fZero = false, fHead = false, icmdDirty = i, fRead = false;
                }
                else if (fZero && (s_astate[c.istate].colorMask & 2) && (fHead ? s_fStencilHead : s_fStencilTail))
                    c.istate = IstateNoStencil(c.istate);
                break;
            case CMDK_Cel: case CMDK_Particles: break;
            case CMDK_Volume: if (!fZero) fRead = true; break;
            case CMDK_FbaClear: case CMDK_ShadowCombine:
                if (!fZero) closeDirty(i);
                fZero = true, fHead = false;
                break;
            default:   // (a shadow phase's begin / hulls / prisms / kind-8 clear: they read the bit, and set it)
                if (fZero) fZero = false, icmdDirty = i;
                fHead = false, fRead = true;
                break;
            }
        }
        if (!fZero) closeDirty(s_acmd.size());
    }
    // Runs of equal state -> indirect commands.
    struct RUN { size_t icmd; uint32_t ideiFirst, cdei; bool fNoClip = false; uint32_t isegFirst = 0, cseg = 0; };
    static std::vector<DEICMD> s_adei;
    static std::vector<RUN> s_arun;
    s_adei.clear();
    s_arun.clear();
    for (size_t i = 0; i < s_acmd.size();)
    {
        const CMD& cmd = s_acmd[i];
        if (cmd.cmdk != CMDK_Pass) { s_arun.push_back({i, 0, 0}); ++i; continue; }
        RUN run{i, uint32_t(s_adei.size()), 0};
        size_t j = i;
        bool fNoClip = true;   // (perf-4: every draw of the run, for the program without the geometry stage)
        // (bindless: the neighbours that differ only in their textures join the run, in the same order; with per-draw
        // state also those differing in what their draw entries hold)
        // (the sorted kinds' runs: one near-clip choice, SortJoinSegment)
        for (; j < s_acmd.size() && s_acmd[j].cmdk == CMDK_Pass &&
               (!(cmd.fClipFixed && s_acmd[j].fClipFixed) || s_acmd[j].fNoClip == cmd.fNoClip) &&
               fJoin(cmd.istate, s_acmd[j].istate); ++j)
        {
            const CMD& c = s_acmd[j];
            s_adei.push_back({c.count, 1, c.firstIndex, c.baseVertex, c.idraw});
            ++run.cdei;
            fNoClip = fNoClip && c.fNoClip;
        }
        run.fNoClip = fNoClip;
        s_arun.push_back(run);
        i = j;
    }
    // (g_fDrawUniform, OpenGL ES without base instance or multi-draw: glob.vert CANE_PULL) each run's sub-draws as
    // few draws of one per-pass index list: an index holds the segment's sub-draw (bits 22..31) and the vertex from the
    // segment's base (bits 0..21); the draw map turns segment base + sub-draw into the draw index. Same triangles in the
    // same order as the sub-draws; built once for all the pass's eyes.
    struct PULLSEG { uint32_t offset, count, vbase, imap; };
    // (per kind of pass its own buffers, kept while its sub-draws stay the same from frame to frame: then nothing is
    // built or uploaded again)
    struct PULLCACHE
    {
        GLuint vao = 0, ebo = 0, ssbo = 0;
        std::vector<DEICMD> adei;
        std::vector<uint32_t> arunsig, arunseg;
        std::vector<PULLSEG> aseg;
    };
    static std::map<int, PULLCACHE> s_mppullcache;
    static std::vector<uint32_t> s_aipull, s_adrawmap;
    static std::vector<uint32_t> s_arunsig;
    const PULLCACHE* ppc = nullptr;
    if (g_fDrawUniform)
    {
        PULLCACHE& pc = s_mppullcache[(pv.fForeground ? 1000 : 0) + pv.irtgt + 1];
        ppc = &pc;
        s_arunsig.clear();
        for (const RUN& run : s_arun)
        {
            s_arunsig.push_back(run.ideiFirst);
            s_arunsig.push_back(run.cdei);
            s_arunsig.push_back(run.cdei ? s_astate[s_acmd[run.icmd].istate].vao : 0);
        }
        const bool fSame = pc.vao && pc.adei.size() == s_adei.size() && pc.arunsig == s_arunsig &&
                           (s_adei.empty() || !std::memcmp(pc.adei.data(), s_adei.data(), s_adei.size() * sizeof(DEICMD)));
        if (!fSame)
        {
            pc.aseg.clear(); pc.arunseg.clear();
            if (!pc.vao)
            {
                pc.ebo = GlNewBuffer(GL_ELEMENT_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);
                pc.ssbo = GlNewBuffer(GL_SHADER_STORAGE_BUFFER, 0, nullptr, GL_STREAM_DRAW);
                if (const cane::draw::Packets* pkv = PpacketsVao()) pc.vao = pkv->createVertexArray(nullptr, 0, pc.ebo);
                else
                {
                glGenVertexArrays(1, &pc.vao);
                glBindVertexArray(pc.vao);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pc.ebo);
                glBindVertexArray(0);
                }
            }
            // (the lists' sizes first, by the same walk as their filling below; then both filled in place: straight in
            // the backend's stream memory where it has mapStream, else in s_aipull / s_adrawmap, uploaded after)
            auto arenaOf = [&](const RUN& run) -> const PULLARENA* {
                if (!run.cdei) return nullptr;
                const auto it = s_mppullarena.find(s_astate[s_acmd[run.icmd].istate].vao);
                return it != s_mppullarena.end() && !it->second.aindex.empty() ? &it->second : nullptr;
            };
            size_t cipull = 0, cdrawmap = 0;
            for (const RUN& run : s_arun)
                if (const PULLARENA* pa = arenaOf(run))
                    for (uint32_t i = run.ideiFirst; i < run.ideiFirst + run.cdei; ++i)
                    {
                        const DEICMD& d = s_adei[i];
                        if (size_t(d.firstIndex) + d.count > pa->aindex.size()) break;
                        ++cdrawmap;
                        cipull += d.count;
                    }
            const cane::draw::Packets* const pkm = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
            uint32_t* pipull = nullptr;
            uint32_t* pdrawmap = nullptr;
            if (pkm && pkm->mapStream)
            {
                pipull = static_cast<uint32_t*>(pkm->mapStream(pc.ebo, std::max<size_t>(cipull, 1) * 4));
                pdrawmap = pipull ? static_cast<uint32_t*>(pkm->mapStream(pc.ssbo, std::max<size_t>(cdrawmap, 1) * 4)) : nullptr;
            }
            const bool fMapped = pipull && pdrawmap;
            if (!fMapped)
            {
                s_aipull.resize(cipull);
                s_adrawmap.resize(cdrawmap);
                pipull = s_aipull.data();
                pdrawmap = s_adrawmap.data();
            }
            uint32_t nipull = 0, ndrawmap = 0;
            for (RUN& run : s_arun)
            {
                run.isegFirst = run.cseg = 0;
                if (const PULLARENA* pa = arenaOf(run))
                {
                    run.isegFirst = uint32_t(pc.aseg.size());
                    PULLSEG seg{0, 0, 0, 0};
                    bool fOpen = false, fBad = false;
                    for (uint32_t i = run.ideiFirst; i < run.ideiFirst + run.cdei && !fBad; ++i)
                    {
                        const DEICMD& d = s_adei[i];
                        if (size_t(d.firstIndex) + d.count > pa->aindex.size()) { fBad = true; break; }
                        uint32_t imax = 0;
                        for (uint32_t k = 0; k < d.count; ++k) imax = std::max(imax, pa->aindex[d.firstIndex + k]);
                        const uint32_t ld = fOpen ? ndrawmap - seg.imap : 0;
                        if (!fOpen || uint32_t(d.baseVertex) < seg.vbase || uint64_t(uint32_t(d.baseVertex) - seg.vbase) + imax >= (1u << 22) || ld >= 1024u)
                        {
                            if (fOpen) { pc.aseg.push_back(seg); ++run.cseg; }
                            seg = PULLSEG{nipull, 0, uint32_t(d.baseVertex), ndrawmap};
                            fOpen = true;
                        }
                        const uint32_t hi = (ndrawmap - seg.imap) << 22, dv = uint32_t(d.baseVertex) - seg.vbase;
                        pdrawmap[ndrawmap++] = d.baseInstance;
                        const uint32_t* pi = pa->aindex.data() + d.firstIndex;
                        for (uint32_t k = 0; k < d.count; ++k) pipull[nipull + k] = hi | (dv + pi[k]);
                        nipull += d.count;
                        seg.count += d.count;
                    }
                    if (fBad) { pc.aseg.resize(run.isegFirst); run.cseg = 0; }
                    else if (fOpen) { pc.aseg.push_back(seg); ++run.cseg; }
                }
                pc.arunseg.push_back(run.isegFirst);
                pc.arunseg.push_back(run.cseg);
            }
            if (!cipull && fMapped) pipull[0] = 0;   // (the one-entry minimum written, as the zeroed vector's would be)
            if (!cdrawmap && fMapped) pdrawmap[0] = 0;
            const bool fBind = !PpacketsBuf();   // (native buffers: uploaded by name, no binding)
            if (fBind) glBindVertexArray(pc.vao);
            if (!fMapped) UploadStream(GL_ELEMENT_ARRAY_BUFFER, pc.ebo, s_aipull.data(), s_aipull.size() * 4, std::max<size_t>(s_aipull.size(), 1) * 4);
            if (fBind) glBindVertexArray(0);
            if (fBind) glBindBuffer(GL_SHADER_STORAGE_BUFFER, pc.ssbo);
            if (!fMapped) UploadStream(GL_SHADER_STORAGE_BUFFER, pc.ssbo, s_adrawmap.data(), s_adrawmap.size() * 4, std::max<size_t>(s_adrawmap.size(), 1) * 4);
            pc.adei = s_adei;
            pc.arunsig = s_arunsig;
        }
        else
            for (size_t r = 0; r < s_arun.size(); ++r) s_arun[r].isegFirst = pc.arunseg[2 * r], s_arun[r].cseg = pc.arunseg[2 * r + 1];
        BindStorageWhole(10, pc.ssbo);
    }
    static const bool s_fPerfDbg = std::getenv("SLY2_PERFDBG") != nullptr;
    if (s_fPerfDbg)
    {
        // records / commands / runs per kind, and records without a draw record (never culled)
        int acrec[32] = {}, acmd[32] = {}, acrun[32] = {}, acnodraw[32] = {};
        for (const DRAWREC& rec : arec) { ++acrec[rec.kind]; if (!rec.pglob->fDraw) ++acnodraw[rec.kind]; }
        for (const CMD& c : s_acmd) if (c.prec) ++acmd[c.prec->kind];
        for (const RUN& r : s_arun) if (s_acmd[r.icmd].prec) ++acrun[s_acmd[r.icmd].prec->kind];
        for (int k = 0; k < 32; ++k)
            if (acrec[k]) std::printf("kind %2d: %5d records (%d without bounds), %5d commands, %5d runs\n", k, acrec[k], acnodraw[k], acmd[k], acrun[k]);
    }
    // (each array's place: in the frame's ring segment when the pass fits (all or none), else its own orphaned buffer)
    struct BUFRANGE { GLuint buf; size_t off, cb; };
    const size_t cbAll = s_arecgpu.size() * sizeof(RECGPU) + cmatBone * sizeof(glm::mat4) + s_adrawgpu.size() * sizeof(DRAWGPU) +
                         s_awarpst.size() * sizeof(glm::vec4) + s_amatWarp.size() * sizeof(glm::mat4) + s_adei.size() * sizeof(DEICMD) +
                         (g_fGlobBindless || g_fGlobArrays ? s_adrawgpu.size() * sizeof(GLuint64) * (FDrawStateLayout() ? 2 : 1) : 0) + 8 * (s_ring.align + 16);
    size_t offRing = s_ring.Alloc(cbAll);
    auto upload = [&](GLenum target, GLuint buf, const void* pv, size_t cb) -> BUFRANGE {
        const size_t cbBind = std::max<size_t>(cb, 16);
        if (offRing != SIZE_MAX)
        {
            const size_t o = (offRing + s_ring.align - 1) / s_ring.align * s_ring.align;
            if (cb) std::memcpy(s_ring.pb + o, pv, cb);
            offRing = o + cbBind;
            return {s_ring.buf, o, cbBind};
        }
        if (!PpacketsBuf()) glBindBuffer(target, buf);
        UploadStream(target, buf, pv, cb, cbBind);   // (orphaned)
        return {buf, 0, cbBind};
    };
    const BUFRANGE brRec = upload(GL_SHADER_STORAGE_BUFFER, s_ssboRec, s_arecgpu.data(), s_arecgpu.size() * sizeof(RECGPU));
    BUFRANGE brBone{0, 0, 0};
    if (uint8_t* pb = ppkStream && offRing == SIZE_MAX ? static_cast<uint8_t*>(ppkStream->mapStream(s_ssboBone, std::max<size_t>(cmatBone * sizeof(glm::mat4), 16))) : nullptr)
    {
        // (the records' bones in their order, straight from the poses)
        for (const DRAWREC& rec : arec)
            if (rec.pamatBone && !rec.pamatBone->empty())
            {
                const size_t cb = rec.pamatBone->size() * sizeof(glm::mat4);
                std::memcpy(pb + size_t(rec.iboneFirst) * sizeof(glm::mat4), rec.pamatBone->data(), cb);
            }
        if (!PpacketsBuf()) glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssboBone);   // (the GL binding the GL calls leave)
        brBone = {s_ssboBone, 0, std::max<size_t>(cmatBone * sizeof(glm::mat4), 16)};
    }
    else
    {
        if (ppkStream)   // (the ring took the pass after all: the bones gathered now)
            for (const DRAWREC& rec : arec)
                if (rec.pamatBone) s_amatBone.insert(s_amatBone.end(), rec.pamatBone->begin(), rec.pamatBone->end());
        brBone = upload(GL_SHADER_STORAGE_BUFFER, s_ssboBone, s_amatBone.data(), s_amatBone.size() * sizeof(glm::mat4));
    }
    const BUFRANGE brDraw = upload(GL_SHADER_STORAGE_BUFFER, s_ssboDraw, s_adrawgpu.data(), s_adrawgpu.size() * sizeof(DRAWGPU));
    const BUFRANGE brWarp = upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarp, s_awarpst.data(), s_awarpst.size() * sizeof(glm::vec4));
    const BUFRANGE brWarpMat = upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarpMat, s_amatWarp.data(), s_amatWarp.size() * sizeof(glm::mat4));
    const BUFRANGE brIndirect = upload(GL_DRAW_INDIRECT_BUFFER, s_bufIndirect, s_adei.data(), s_adei.size() * sizeof(DEICMD));
    BUFRANGE brTexh{0, 0, 0};
    if (g_fGlobBindless)
    {
        // each pass draw's texture handle by its draw entry (0: the other entries, and a state without one)
        if (FDrawStateLayout())
        {
            // (CANE_DRAWSTATE: the handle, then the draw's state word (DrawStateWord; 0: the uniforms))
            struct TEXHDS { GLuint64 handle; uint32_t ds, pad; };
            static std::vector<TEXHDS> s_atexhds;
            s_atexhds.assign(s_adrawgpu.size(), TEXHDS{0, 0, 0});
            for (const CMD& c : s_acmd)
                if (c.cmdk == CMDK_Pass) s_atexhds[c.idraw] = {HandleOfIstate(c.istate), fDrawState ? DrawStateWord(s_astate[c.istate]) : 0u, 0u};
            brTexh = upload(GL_SHADER_STORAGE_BUFFER, s_ssboTexh, s_atexhds.data(), s_atexhds.size() * sizeof(TEXHDS));
        }
        else
        {
            static std::vector<GLuint64> s_atexh;
            s_atexh.assign(s_adrawgpu.size(), 0);
            for (const CMD& c : s_acmd)
                if (c.cmdk == CMDK_Pass) s_atexh[c.idraw] = HandleOfIstate(c.istate);
            brTexh = upload(GL_SHADER_STORAGE_BUFFER, s_ssboTexh, s_atexh.data(), s_atexh.size() * sizeof(GLuint64));
        }
    }
    else if (g_fGlobArrays)
    {
        // each pass draw's layer + 1 by its draw entry (0: the bound texture: the other entries, a state without a layer)
        static std::vector<glm::uvec2> s_alayer;
        s_alayer.assign(s_adrawgpu.size(), glm::uvec2(0u));
        for (const CMD& c : s_acmd)
            if (c.cmdk == CMDK_Pass)
                if (const uint32_t ref = LayerRefOfIstate(c.istate); ref != LAYERREF_NONE) s_alayer[c.idraw].x = (ref & 0xFFFFu) + 1u;
        brTexh = upload(GL_SHADER_STORAGE_BUFFER, s_ssboTexh, s_alayer.data(), s_alayer.size() * sizeof(glm::uvec2));
    }

    // (VR) a stereo pass draws the same runs into each eye with the eye's own matrix and position; a plain pass
    // draws once with its own (the names below are the eye's: the same values as before for a plain pass)
    const int ceyePass = pv.pstereo && !pv.pstereo->fMultiview ? 2 : 1;   // (multiview: both eyes in one)
    bool fIndirectAny = false;   // (an eye's pass bound the indirect buffer: unbound at the end, as the GL calls leave it)
    for (int ieye = 0; ieye < ceyePass; ++ieye)
    {
    const STEREOVIEW* const psv = pv.pstereo;
    const glm::mat4& matWorldToClip = psv ? psv->amatWorldToClip[ieye] : pv.matWorldToClip;
    struct { glm::vec3 pos, dir; glm::vec3 Dir() const { return dir; } } cam{psv ? psv->apos[ieye] : pv.pos, pv.dir};
    // (Vulkan VR multiview: both eyes in this one pass, two views; the programs' two-view stages read each eye's matrix
    // and position from CaneViews, and what the CPU decides per eye below is decided for both: eye 1's values too)
    const bool fTwoViews = psv && psv->fMultiview && FRendererVulkan();
    std::array<glm::vec3, 4> aposNearV[2] = {aposNearPass, aposNearPass};
    for (int iv = 0; psv && iv < (fTwoViews ? 2 : 1); ++iv)
    {
        const glm::mat4 m = glm::inverse(psv->amatWorldToClip[fTwoViews ? iv : ieye]);
        const float ax[4] = {-1.0f, 1.0f, 1.0f, -1.0f}, ay[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        for (size_t i = 0; i < 4; ++i)
        {
            const glm::vec4 v = m * glm::vec4(ax[i], ay[i], 1.0f, 1.0f);
            aposNearV[iv][i] = glm::vec3(v) / v.w;
        }
    }
    const std::array<glm::vec3, 4>& aposNear = aposNearV[0];
    const GLuint fboEye = psv ? GLuint(psv->afbo[ieye]) : pv.fbo;
    // (the backend's draw packets, platform/draw_packet.h: the Vulkan renderer; nullptr: the GL calls)
    const cane::draw::Packets* const ppackets = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    const glm::vec4 clear = Vec4FromRgba(pv.rgbaClear, 1.0f / 255.0f);
    // (the foreground pass: only the depth (0x2DF010's Z 0 over the frame) and the stencil, over the frame and HUD)
    if (ppackets && ppackets->beginPass)
    {
        cane::draw::Pass pass;
        pass.framebuffer = fboEye;
        pass.viewport[2] = pv.dx, pass.viewport[3] = pv.dy;
        pass.clear = uint8_t(cane::draw::kClearDepth | cane::draw::kClearStencil | (pv.fForeground ? 0 : cane::draw::kClearColor));
        pass.color[0] = clear.r, pass.color[1] = clear.g, pass.color[2] = clear.b, pass.color[3] = float(pv.alphaClear) / 128.0f;
        pass.depth = 0.0f;
        pass.stencil = 0;
        pass.cullOff = 1;
        ppackets->beginPass(pass);
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fboEye);
        glViewport(0, 0, pv.dx, pv.dy);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(clear.r, clear.g, clear.b, float(pv.alphaClear) / 128.0f);
        glClearDepth(0.0);
        glStencilMask(0xFF);
        glClearStencil(0);
        glClear(pv.fForeground ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDisable(GL_CULL_FACE);
    }
    // (draw packets with the binding entries: the storage buffers, the frame uniforms, textures through them)
    const cane::draw::Packets* const ppkBind = ppackets && ppackets->beginPass && ppackets->bindStorage && ppackets->setConstant ? ppackets : nullptr;

    auto bindRange = [ppkBind](GLuint ibinding, const BUFRANGE& br) {
        if (ppkBind && br.cb) ppkBind->bindStorage(ibinding, br.buf, br.off, br.cb);
        else glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ibinding, br.buf, GLintptr(br.off), GLsizeiptr(br.cb));
    };
    bindRange(0, brRec);
    bindRange(1, brDraw);
    bindRange(2, brBone);
    bindRange(3, brWarp);
    bindRange(4, brWarpMat);
    if (g_fGlobBindless || g_fGlobArrays) bindRange(5, brTexh);
    if (fTwoViews)
    {
        // (cmake/vulkan_shaders.py MV_BLOCK: each eye's matrix and position, the frame copies' second view's offset)
        struct { glm::mat4 amat[2]; glm::vec4 apos[2], aaux[2]; glm::ivec4 fetch; } views;
        for (int iv = 0; iv < 2; ++iv) views.amat[iv] = psv->amatWorldToClip[iv], views.apos[iv] = glm::vec4(psv->apos[iv], 1.0f), views.aaux[iv] = glm::vec4(0.0f);
        views.fetch = glm::ivec4(pv.dx, 0, 0, 0);
        static GLuint s_ssboViews = 0;
        if (!s_ssboViews) s_ssboViews = GlNewBuffer(GL_SHADER_STORAGE_BUFFER, 0, nullptr, GL_STREAM_DRAW);
        bindRange(11, upload(GL_SHADER_STORAGE_BUFFER, s_ssboViews, &views, sizeof(views)));
    }
    // (native buffers: bound only for a run drawn with the GL calls; the packets take the buffer by name)
    bool fIndirectBound = !PpacketsBuf();
    if (fIndirectBound) glBindBuffer(GL_DRAW_INDIRECT_BUFFER, brIndirect.buf), fIndirectAny = true;
    const size_t offIndirect = brIndirect.off;
    // (unit 0 active for the pass's texture binds; draw packets: every entry leaves unit 0 active already, and every GL-call
    // path restores it, so nothing to set)
    if (!(ppackets && ppackets->bindTexture && PpacketsSampler())) glActiveTexture(GL_TEXTURE0);
    STATECACHE sc;
    // the glob program bound (0 glGlobShader, 1 glGlobShaderNoKill, -1 another program), and which of the two have this
    // pass's frame uniforms
    int iprogGlob = -1;
    GLuint texArrBound = 0;   // (g_fGlobArrays: unit 2's page and sampler)
    int samplerArrBound = -1;
    bool afFrameUniforms[CPROGGLOB] = {};
    auto useGlob = [&](int iprog) {
        // (an OpenXR session on Android or on Vulkan on a tile GPU (the Steam Frame): every run without the near-clip stage,
        // gl.cpp: the stage was half the frame on the Quest's Adreno; flat: only the provable ones)
        if (vr::FActive() && FVrNoNearClip()) iprog |= 2;
        if ((iprog & 2) && !s_apshGlob[iprog]->ID) iprog &= 1;   // (not built: with the geometry stage)
        if (iprog == 1 && !glGlobShaderNoKill.ID) iprog = 0;
        if (iprog == iprogGlob) return;
        const bool fFromOther = iprogGlob < 0;
        iprogGlob = iprog;
        s_iprogGlob = iprog;
        s_pu = &s_au[iprog];
        sc.iprog = iprog;
        if (ppkBind) s_progPending = s_apshGlob[iprog]->ID;   // (bound by the next packet draw, or FlushProgramPending)
        else s_apshGlob[iprog]->Use();
        if (!afFrameUniforms[iprog] && ppkBind)
        {
            const GLuint prog = s_apshGlob[iprog]->ID;
            auto one = [&](GLint loc, int32_t v) { if (loc >= 0) ppkBind->setConstant(prog, loc, &v, 4); };
            auto vec = [&](GLint loc, const float* p, uint32_t cb) { if (loc >= 0) ppkBind->setConstant(prog, loc, p, cb); };
            vec(s_pu->matWorldToClip, glm::value_ptr(matWorldToClip), 64);
            vec(s_pu->posCamera, glm::value_ptr(cam.pos), 12);
            const glm::vec3 dir = cam.Dir();
            vec(s_pu->dirCamera, glm::value_ptr(dir), 12);
            one(s_pu->tex, 0);
            one(s_pu->texDate, 1);
            one(s_pu->texArr, 2);
            one(s_pu->fDate, 0);
            const glm::vec4 fog = Vec4FromRgba(opts.rgbaFog, 1.0f / 255.0f);
            vec(s_pu->fogColor, glm::value_ptr(fog), 16);
            afFrameUniforms[iprog] = true;
        }
        if (!afFrameUniforms[iprog])
        {
            glUniformMatrix4fv(s_pu->matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
            glUniform3fv(s_pu->posCamera, 1, glm::value_ptr(cam.pos));
            glUniform3fv(s_pu->dirCamera, 1, glm::value_ptr(cam.Dir()));
            glUniform1i(s_pu->tex, 0);
            glUniform1i(s_pu->texDate, 1);
            if (s_pu->texArr >= 0) glUniform1i(s_pu->texArr, 2);
            glUniform1i(s_pu->fDate, 0);
            glUniform4fv(s_pu->fogColor, 1, glm::value_ptr(Vec4FromRgba(opts.rgbaFog, 1.0f / 255.0f)));
            afFrameUniforms[iprog] = true;
        }
        SyncGlobInherited(sc, ppkBind);
        if (fFromOther) sc.fValid = false;   // (the GL state another program's draws changed; uniforms are per program)
    };
    auto otherProgram = [&]() { iprogGlob = -1; };
    // the shadow phase's DATE mark (FMarkDate): open between ShadowBegin and ShadowCombine; fDateMarked while the
    // stencil holds it, cleared by a pass run that writes the frame's alpha (its stencil writes rewrite the bit)
    bool fShadowPhase = false, fDateMarked = false;
    auto markDate = [&]() {
        if (fDateMarked || !fShadowPhase) return fDateMarked;
        fDateMarked = FMarkDate();
        if (fDateMarked) { otherProgram(); sc.Invalidate(); }
        return fDateMarked;
    };
    static const bool s_fCelAlways = std::getenv("SLY2_CELALWAYS") != nullptr;
#if SLY2_FASTPATHS
    // (DrawShadow2's plane for its z-pass casters: the drawn near plane (aposNear) moved toward the scene by a margin)
    auto planeNearOf = [&](const std::array<glm::vec3, 4>& apn, const glm::vec3& posCam) {
        glm::vec3 n = glm::cross(apn[1] - apn[0], apn[3] - apn[0]);
        const float len = glm::length(n);
        n = len > 0.0f ? n / len : glm::normalize(cam.Dir());
        if (glm::dot(n, apn[0] - posCam) < 0.0f) n = -n;
        const float dNear = glm::dot(n, apn[0] - posCam);
        return glm::vec4(n, -(glm::dot(n, apn[0]) + 1.0f + 0.01f * std::max(dNear, 0.0f)));
    };
    const glm::vec4 planeNearSh = planeNearOf(aposNear, cam.pos);
    const glm::vec4 planeNearSh1 = fTwoViews ? planeNearOf(aposNearV[1], psv->apos[1]) : planeNearSh;   // (two views: eye 1's)
    // (s_rectShPhase for the shadow phase opened at icmdBegin: its volumes' screen box, a pixel or two around. The
    // prisms' triangles clipped at half the near plane's distance (the GPU keeps less of them), the hulls by their
    // bounding spheres' boxes; the whole frame when that fails or when multisampled (the copy resolves it whole).)
    // (on the pass helpers: the phase's casters' triangles in pieces, each piece's box, then their union: the same
    // points' minimum and maximum as the loop over them in order, so the same rectangle (a NaN coordinate is never
    // taken by std::min / std::max, and -0 / +0 give the same pixel))
    auto shadowPhaseRectOf = [&](size_t icmdBegin, const glm::mat4& matWorldToClip, const std::array<glm::vec3, 4>& aposNear,
                                 const glm::vec3& posCam, const glm::vec4& planeNearSh) -> std::array<int, 4> {
        const std::array<int, 4> full = {0, 0, pv.dx, pv.dy};
        if (pv.fbo != g_gl.fbo) return full;
        const glm::vec3 nClip(planeNearSh);
        const float dClip = 0.5f * glm::dot(nClip, aposNear[0] - posCam);
        if (!(dClip > 0.0f)) return full;
        auto sClip = [&](const glm::vec3& p) { return glm::dot(nClip, p - posCam) - dClip; };
        struct SHBOX { float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f; bool fFull = false; };
        auto addPoint = [&](SHBOX& b, const glm::vec3& p) {
            const glm::vec4 c = matWorldToClip * glm::vec4(p, 1.0f);
            if (!(c.w > 0.0f)) { b.fFull = true; return; }
            const float x = (c.x / c.w * 0.5f + 0.5f) * float(pv.dx), y = (c.y / c.w * 0.5f + 0.5f) * float(pv.dy);
            b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y); b.x1 = std::max(b.x1, x); b.y1 = std::max(b.y1, y);
        };
        // an item: a type-2 caster's triangles [kFirst, kLim) (its vertex indices), or a hull (kLim 0)
        struct SHITEM { uint32_t icmd, kFirst, kLim; };
        auto addItem = [&](SHBOX& b, const SHITEM& it) {
            const CMD& c = s_acmd[it.icmd];
            if (c.cmdk == CMDK_Shadow2)
            {
                for (uint32_t k = it.kFirst; k + 2 < it.kLim && !b.fFull; k += 3)
                {
                    const glm::vec3* pt = &s_avtxShadow2[c.firstIndex + k];
                    const float as[3] = {sClip(pt[0]), sClip(pt[1]), sClip(pt[2])};
                    for (int e = 0; e < 3; ++e)
                    {
                        const int e1 = (e + 1) % 3;
                        if (as[e] > 0.0f) addPoint(b, pt[e]);
                        if ((as[e] > 0.0f) != (as[e1] > 0.0f)) addPoint(b, pt[e] + (pt[e1] - pt[e]) * (as[e] / (as[e] - as[e1])));
                    }
                }
                return;
            }
            const GLOB& geom = *c.prec->pglobGeom;
            if (!geom.pgl || !(geom.pgl->sRadius > 0.0f)) { b.fFull = true; return; }
            const glm::mat4& m = *c.prec->pmat;
            const float scale = std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))});
            const glm::vec3 ctr = glm::vec3(m * glm::vec4(geom.pgl->posCenter, 1.0f));
            const float r = geom.pgl->sRadius * scale * 1.05f;
            for (int k = 0; k < 8 && !b.fFull; ++k) addPoint(b, ctr + glm::vec3((k & 1) ? r : -r, (k & 2) ? r : -r, (k & 4) ? r : -r));
        };
        static std::vector<SHITEM> s_aitemSh;
        static std::vector<SHBOX> s_aboxSh;
        s_aitemSh.clear();
        constexpr uint32_t CVTX_ITEM = 3 * 512;   // (512 triangles a piece)
        for (size_t i = icmdBegin + 1; i < s_acmd.size() && s_acmd[i].cmdk != CMDK_ShadowCombine; ++i)
        {
            const CMD& c = s_acmd[i];
            if (c.cmdk == CMDK_Shadow2)
                for (uint32_t k = 0; k + 2 < c.count; k += CVTX_ITEM) s_aitemSh.push_back({uint32_t(i), k, std::min(c.count, k + CVTX_ITEM)});
            else if (c.cmdk == CMDK_Shadow) s_aitemSh.push_back({uint32_t(i), 0, 0});
        }
        s_aboxSh.assign(s_aitemSh.size(), SHBOX{});
        PassHelpers().ForBlocks(s_aitemSh.size(), 2, [&](size_t iFirst, size_t iLim) {
            for (size_t j = iFirst; j < iLim; ++j) addItem(s_aboxSh[j], s_aitemSh[j]);
        });
        SHBOX box;
        for (const SHBOX& b : s_aboxSh)
        {
            if (b.fFull) return full;
            box.x0 = std::min(box.x0, b.x0); box.y0 = std::min(box.y0, b.y0); box.x1 = std::max(box.x1, b.x1); box.y1 = std::max(box.y1, b.y1);
        }
        const float x0 = box.x0, y0 = box.y0, x1 = box.x1, y1 = box.y1;
        if (x1 < x0 || y1 < y0) return {0, 0, 0, 0};
        const int ix0 = std::clamp(int(std::floor(x0)) - 2, 0, pv.dx), iy0 = std::clamp(int(std::floor(y0)) - 2, 0, pv.dy);
        const int ix1 = std::clamp(int(std::ceil(x1)) + 2, 0, pv.dx), iy1 = std::clamp(int(std::ceil(y1)) + 2, 0, pv.dy);
        return {ix0, iy0, ix1 - ix0, iy1 - iy0};
    };
    // (two views: the union of the eyes' rectangles: each eye's volumes lie inside its own, the marks outside stay clear)
    auto shadowPhaseRect = [&](size_t icmdBegin) -> std::array<int, 4> {
        const std::array<int, 4> r0 = shadowPhaseRectOf(icmdBegin, matWorldToClip, aposNear, cam.pos, planeNearSh);
        if (!fTwoViews) return r0;
        const std::array<int, 4> r1 = shadowPhaseRectOf(icmdBegin, psv->amatWorldToClip[1], aposNearV[1], psv->apos[1], planeNearSh1);
        if (r0[2] <= 0 || r0[3] <= 0) return r1;
        if (r1[2] <= 0 || r1[3] <= 0) return r0;
        const int x0 = std::min(r0[0], r1[0]), y0 = std::min(r0[1], r1[1]);
        const int x1 = std::max(r0[0] + r0[2], r1[0] + r1[2]), y1 = std::max(r0[1] + r0[3], r1[1] + r1[3]);
        return {x0, y0, x1 - x0, y1 - y0};
    };
#endif
    s_vaoPullCur = 0;   // (the pass's first pulled draw binds its VAO)
    // (measurement: SLY2N_ALPHASTAT, OpenGL only, the main pass without VR)
    ALPHASTAT& astat = AlphaStat();
    if (astat.every > 0 && !ppackets && !psv && pv.irtgt < 0 && !pv.fForeground && ++astat.cpass % astat.every == 0)
    {
        astat.fOn = true;
        astat.dx = pv.dx, astat.dy = pv.dy;
        astat.acount.assign(size_t(pv.dx) * size_t(pv.dy), 0);
        astat.arec.clear();
        astat.iq = 0;
        astat.cdirtyBefore = 0;
    }
    // (audit: SLY2N_LRZAUDIT=N, any renderer: every N-th main pass, the draws' states that a tiler's low-resolution Z
    // cares about, in the order drawn: notes/VKALPHA_2026-11-11.md)
    static const int s_cLrzAudit = std::getenv("SLY2N_LRZAUDIT") ? std::atoi(std::getenv("SLY2N_LRZAUDIT")) : 0;
    static int s_cpassLrz = 0;
    const bool fLrzAudit = s_cLrzAudit > 0 && pv.irtgt < 0 && !pv.fForeground && ieye == 0 && ++s_cpassLrz % s_cLrzAudit == 0;
    std::map<std::string, int> mpLrz;
    std::string strLrzSeq;
    int irunLrz = 0;
    auto LrzAuditRun = [&](const CMD& c, uint32_t cdraw) {
        static const char* const s_aszCmdk[] = {"pass", "volume", "shadow", "cel", "particles", "shadow2", "shadowbegin", "shadowkind8", "shadowcombine", "fbaclear"};
        const int kind = c.prec ? c.prec->kind : -1;
        std::string cls;
        if (c.cmdk == CMDK_Pass)
        {
            const DSTATE& st = s_astate[c.istate];
            if (st.depthFunc == 0 && st.depthMask) cls += " ALWAYS+zwrite";
            else if (st.depthFunc == 0) cls += " ALWAYS";
            if (st.colorMask & 2) cls += " stencilwrite";
            if (st.fBlend && st.depthMask) cls += " blend+zwrite";
            if (FStateNeedsKill(st) && st.depthMask) cls += " discard+zwrite";
            else if (FStateNeedsKill(st)) cls += " discard";
            if (cls.empty()) cls = " plain";
        }
        else cls = std::string(" ") + s_aszCmdk[c.cmdk];
        const std::string key = "k" + std::to_string(kind) + cls;
        mpLrz[key] += int(c.cmdk == CMDK_Pass ? cdraw : 1);
        if (c.cmdk != CMDK_Pass || (s_astate[c.istate].depthFunc == 0 && s_astate[c.istate].depthMask))
            strLrzSeq += " " + std::to_string(irunLrz) + ":" + key.substr(0, key.find(' ')) + cls;
        ++irunLrz;
    };
    auto fAlphaStatKind = [](const CMD& c) { return c.cmdk == CMDK_Pass && c.prec && (c.prec->kind == 5 || c.prec->kind == 6 || c.prec->kind == 10); };
    for (const RUN& run : s_arun)
    {
        const CMD& cmd = s_acmd[run.icmd];
        if (cmd.cmdk != CMDK_Pass) s_vaoPullCur = 0;   // (the specials bind VAOs of their own)
        if (cmd.cmdk != CMDK_Pass && cmd.cmdk != CMDK_Volume && cmd.cmdk != CMDK_Shadow) FlushProgramPending();
        if (fLrzAudit) LrzAuditRun(cmd, run.cdei);
        if (astat.fOn && fAlphaStatKind(cmd) != astat.fInSeq) AlphaStatSeq(!astat.fInSeq);
        if (astat.fOn && fAlphaStatKind(cmd))
        {
            for (uint32_t j = 0; j < run.cdei; ++j)
            {
                const CMD& c = s_acmd[run.icmd + j];
                const DSTATE& s = s_astate[c.istate];
                const DEICMD& d = s_adei[run.ideiFirst + j];
                ALPHASTATREC r{c.prec->kind, c.prec->psgs ? c.prec->psgs->oid : -1, c.psub && c.psub->pshd ? c.psub->pshd->id : -1, int(c.order / 4), s.fBlend ? s.alpha : 0,
                               s.depthMask, s.colorMask, s.ate && s.afailMode ? s.atst : -1, s.depthFunc, d.count, s.fTextured ? s.tex : 0, {0, 0, 0}, {}};
                if (c.irect != UINT32_MAX) r.rect = s_asortrect[c.irect].a[0];
                for (int k = 0; k < 3; ++k)
                {
                    const bool fForce = k == 1;
                    if (fForce && FStateNeedsKill(s)) continue;   // (its own alpha test: both counts the same)
                    useGlob((FStateNeedsKill(s) || fForce ? 0 : 1) | (run.fNoClip ? 2 : 0));
                    sc.Apply(s, !g_fGlobBindless || !HandleOfIstate(c.istate));
                    if (fForce)
                    {
                        glUniform1i(s_pu->ate, 1);
                        glUniform1i(s_pu->atst, 6);
                        glUniform1f(s_pu->aref, 0.0f);
                        glUniform1i(s_pu->afailMode, 1);
                    }
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDepthMask(GL_FALSE);
                    glStencilFunc(GL_ALWAYS, 0, 0);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
                    glStencilMask(k == 0 ? 0x7F : 0);
                    if (k == 2) glDepthFunc(GL_ALWAYS);   // (its fragments on the screen, before the depth test)
                    r.aq[k] = AlphaStatQuery();
                    glBeginQuery(GL_SAMPLES_PASSED, r.aq[k]);
                    DrawIndexed(d.count, d.firstIndex, d.baseVertex, d.baseInstance, s.vao);
                    glEndQuery(GL_SAMPLES_PASSED);
                    sc.Invalidate();
                }
                astat.arec.push_back(r);
            }
        }
        if (s_fGpuMarks)
        {
            // (measurement: the GPU time of each command kind / record kind in a row)
            static const char* const s_aszCmdk[] = {"pass", "volume", "shadow", "cel", "particles", "shadow2", "shbegin", "shkind8", "shcombine", "fbaclear"};
            static std::array<std::array<std::string, 64>, 10> s_aastr;
            const int kindRec = cmd.prec ? int(cmd.prec->kind) & 63 : 0;
            std::string& str = s_aastr[cmd.cmdk][kindRec];
            if (str.empty()) str = std::string(s_aszCmdk[cmd.cmdk]) + (cmd.prec ? " k" + std::to_string(kindRec) : "");
            if (str.c_str() != s_szGpuMarkLast) GpuMark(s_szGpuMarkLast = str.c_str());
        }
        switch (cmd.cmdk)
        {
        case CMDK_Pass:
        {
            const DSTATE& st = s_astate[cmd.istate];
            const bool fKill = FStateNeedsKill(st);
            bool fPacketDrawn = false;
#if SLY2_FASTPATHS
            // (the Switch: the blended kinds' runs keep the program with discards, which would otherwise change nearly
            // every run; it draws a run without alpha test the same, its discards needing ate and afailMode, or fDate)
            const bool fKeepKill = cmd.prec && fSortedKind(cmd.prec->kind);
#else
            constexpr bool fKeepKill = false;
#endif
            useGlob((fKill || fKeepKill ? 0 : 1) | (run.fNoClip ? 2 : 0));
            // (bindless: a run with handles leaves the texture unit alone; one without, a single state, binds it)
            if (const uint32_t ref = g_fGlobArrays ? LayerRefOfIstate(cmd.istate) : LAYERREF_NONE; ref != LAYERREF_NONE)
            {
                // (arrays: the run's page and the state's sampler on unit 2; unit 0 left alone)
                const GLuint texPage = s_atexpage[ref >> 16].tex;
                if (texPage != texArrBound || st.sampler != samplerArrBound)
                {
                    glActiveTexture(GL_TEXTURE2);
                    if (texPage != texArrBound) glBindTexture(GL_TEXTURE_2D_ARRAY, texPage);
                    glActiveTexture(GL_TEXTURE0);
                    if (st.sampler != samplerArrBound) glBindSampler(2, s_asampler[st.sampler & 15u]);
                    texArrBound = texPage, samplerArrBound = st.sampler;
                }
                FlushProgramPending(), sc.Apply(st, false);
            }
            else if (const bool fBindTexture = !g_fGlobBindless || !HandleOfIstate(cmd.istate);
                     ppackets && (!fBindTexture || !st.fTextured || ppackets->bindTexture))
            {
                // (draw packets: the state and the draws straight to the backend; the cache then as Apply leaves it)
                if (const uint32_t idPacket = PacketStateOf(*ppackets, cmd.istate, iprogGlob))
                {
                    // (a textured run without a handle (bound textures: no bindless, or a texture without one): its texture
                    // and sampler on unit 0 first, where Apply binds them)
                    if (fBindTexture && st.fTextured &&
                        (!sc.fValid || !sc.cur.fTextured || st.tex != sc.cur.tex || st.sampler != sc.cur.sampler || !sc.fTexBound))
                        ppackets->bindTexture(0, st.tex, 0, s_asampler[st.sampler]);
                    if (g_fDrawUniform && run.cseg)
                    {
                        // (CANE_PULL: the run's segments from the pass's index list, each with its bases)
                        PpullArena(st.vao);
                        for (uint32_t iseg = run.isegFirst; iseg < run.isegFirst + run.cseg; ++iseg)
                        {
                            const PULLSEG& seg = ppc->aseg[iseg];
                            const cane::draw::Constant abase[2] = {{s_pu->caneVertexBase, seg.vbase}, {s_pu->caneDrawBase, seg.imap}};
                            ppackets->drawIndexed(idPacket, ppc->vao, seg.count, seg.offset, 0, 0, abase, 2);
                        }
                        s_vaoPullCur = ppc->vao;
                    }
                    else if (g_fDrawUniform)
                    {
                        for (uint32_t i = run.ideiFirst; i < run.ideiFirst + run.cdei; ++i)
                        {
                            const DEICMD& d = s_adei[i];
                            PpullArena(st.vao);
                            const cane::draw::Constant abase[2] = {{s_pu->caneVertexBase, uint32_t(d.baseVertex)}, {s_pu->caneDrawBase, d.baseInstance | 0x80000000u}};
                            ppackets->drawIndexed(idPacket, st.vao, d.count, d.firstIndex, 0, 0, abase, 2);
                        }
                        s_vaoPullCur = st.vao;
                    }
                    else if (g_fDirectRuns)
                        for (uint32_t i = run.ideiFirst; i < run.ideiFirst + run.cdei; ++i)
                            ppackets->drawIndexed(idPacket, st.vao, s_adei[i].count, s_adei[i].firstIndex, s_adei[i].baseVertex, s_adei[i].baseInstance, nullptr, 0);
                    else
                        ppackets->drawIndirect(idPacket, st.vao, brIndirect.buf, offIndirect + size_t(run.ideiFirst) * sizeof(DEICMD), run.cdei);
                    sc.cur = st;
                    sc.fValid = true;
                    sc.aucur[sc.iprog] = st;
                    sc.afuValid[sc.iprog] = true;
                    sc.fTexBound = fBindTexture;
                    s_aglobinh[sc.iprog].fFogBlack = s_globinhLast.fFogBlack = st.fFogBlack;
                    fPacketDrawn = true;
                    s_progPending = 0;   // (bound by the packet draws)
                }
                else
                    FlushProgramPending(), sc.Apply(st, fBindTexture);
            }
            else
                FlushProgramPending(), sc.Apply(st, !g_fGlobBindless || !HandleOfIstate(cmd.istate));
            // (g_fDrawUniform, OpenGL ES without base instance: DrawIndexed sets the draw index as a uniform)
            if (fPacketDrawn) {}
            else if (g_fDrawUniform && run.cseg)
            {
                // (CANE_PULL: the run's segments from the pass's index list, then the state's own arena back)
                PpullArena(st.vao);
                if (s_vaoPullCur != ppc->vao) { glBindVertexArray(ppc->vao); s_vaoPullCur = ppc->vao; }
                for (uint32_t iseg = run.isegFirst; iseg < run.isegFirst + run.cseg; ++iseg)
                {
                    const PULLSEG& seg = ppc->aseg[iseg];
                    glUniform1ui(s_pu->caneVertexBase, seg.vbase);
                    glUniform1ui(s_pu->caneDrawBase, seg.imap);
                    glDrawElements(GL_TRIANGLES, GLsizei(seg.count), GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(seg.offset) * 4));
                }
            }
            else if (g_fDirectRuns || g_fDrawUniform)
                for (uint32_t i = run.ideiFirst; i < run.ideiFirst + run.cdei; ++i)
                    DrawIndexed(s_adei[i].count, s_adei[i].firstIndex, s_adei[i].baseVertex, s_adei[i].baseInstance, st.vao);
            else
            {
                if (!fIndirectBound) glBindBuffer(GL_DRAW_INDIRECT_BUFFER, brIndirect.buf), fIndirectBound = fIndirectAny = true;
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                            reinterpret_cast<void*>(offIndirect + size_t(run.ideiFirst) * sizeof(DEICMD)), GLsizei(run.cdei), 0);
            }
            // (its alpha writes rewrite the stencil's FBA bit: the DATE mark is made again before the next DATE draw)
            if (st.colorMask & 2) fDateMarked = false;
            // (per-draw state: the uniforms then left as the run's last draw's, as when each draw set its own, for the
            // special draws after it, which read them)
            if (fDrawState && run.cdei > 1)
            {
                const uint32_t istateLast = s_acmd[run.icmd + run.cdei - 1].istate;
                if (istateLast != cmd.istate && (&run == &s_arun.back() || s_acmd[(&run)[1].icmd].cmdk != CMDK_Pass))
                {
                    // (draw packets: the last draw's state as one packet state applied; the cache then as Apply leaves it
                    // (the pulled layout: no vertex array of its own, the texture unit left))
                    if (const uint32_t idLast = fPacketDrawn && ppackets->applyState ? PacketStateOf(*ppackets, istateLast, iprogGlob) : 0)
                    {
                        ppackets->applyState(idLast);
                        const DSTATE& stLast = s_astate[istateLast];
                        sc.cur = stLast;
                        sc.fValid = true;
                        sc.aucur[sc.iprog] = stLast;
                        sc.afuValid[sc.iprog] = true;
                        sc.fTexBound = false;
                        s_aglobinh[sc.iprog].fFogBlack = s_globinhLast.fFogBlack = stLast.fFogBlack;
                    }
                    else FlushProgramPending(), sc.Apply(s_astate[istateLast], false);
                }
            }
            s_glstats.crun += 1; s_glstats.cpass += run.cdei;
            g_renderstats.cpass += int(run.cdei);
            ++g_renderstats.cbatch;
            break;
        }
        case CMDK_Volume:
            // (no alpha test, no DATE: the program without discards)
            useGlob(1);
            if (glGlobShaderVol.ID)
                s_volframe = {matWorldToClip, cam.pos, cam.Dir(), Vec4FromRgba(opts.rgbaFog, 1.0f / 255.0f)};
            DrawProjVolume(cmd);
            fDateMarked = false;   // (its mask is the stencil's bit 7, cleared after)
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        case CMDK_Shadow:
        {
            // a hull's DATE from the stencil mark when the phase has one: then no discard
            const bool fDate = FHullDate(cmd);
            const bool fMarked = fDate && markDate();
            useGlob(!fDate || fMarked ? 1 : 0);
            DrawShadow(cmd, cam.pos, sNearClipSh, aposNear, fMarked, fTwoViews ? &psv->apos[1] : nullptr, fTwoViews ? &aposNearV[1] : nullptr);
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        }
        case CMDK_ShadowBegin:
#if SLY2_FASTPATHS
            s_rectShPhase = shadowPhaseRect(run.icmd);
#endif
            ShadowBegin(fboEye, pv.dx, pv.dy, int(cmd.order), cmd.fAlphaKept && (!g_fMultiviewPass || fTwoViews) && ShadowFetchMode() > 0);
            s_glstats.cshphase += 1; s_glstats.cshkept += cmd.fAlphaKept;
            fShadowPhase = true;
            fDateMarked = false;
            sc.Invalidate();   // (its stencil clear leaves the write mask at 0xFF)
            ++g_renderstats.cbatch;
            break;
        case CMDK_ShadowKind8:
            ShadowKind8();
            sc.Invalidate();   // (likewise)
            ++g_renderstats.cbatch;
            break;
        case CMDK_FbaClear:
            ClearFbaBits();
            sc.Invalidate();
            break;
        case CMDK_ShadowCombine:
            ShadowCombine(cmd.count & 1, cmd.count & 2);
            fShadowPhase = false;
            fDateMarked = false;
            otherProgram();
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        case CMDK_Shadow2:
        {
            const bool fMarked = !(cmd.order & 8) && markDate();
#if SLY2_FASTPATHS
            DrawShadow2(cmd, matWorldToClip, fMarked, &planeNearSh, fTwoViews ? &planeNearSh1 : nullptr);
#else
            DrawShadow2(cmd, matWorldToClip, fMarked);
#endif
            otherProgram();
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        }
        case CMDK_Particles:
            if (opts.fMeshes) DrawParticles(matWorldToClip, cam.pos, aspect, fTwoViews ? &psv->amatWorldToClip[1] : nullptr, fTwoViews ? &psv->apos[1] : nullptr, true);
            else glDisable(GL_STENCIL_TEST);
            otherProgram();
            sc.Invalidate();
            ++g_renderstats.cbatch;
            break;
        case CMDK_Cel:
        {
            const GLOB& geom = *cmd.prec->pglobGeom;
            if (ppackets && ppackets->drawArrays)
            {
                // (draw packets: the border's state made once per program, its values as the draw's constants)
                static uint32_t s_idCel = 0;
                static GLuint s_progCel = 0;
                if (s_progCel != glCelBorderShader.ID)
                {
                    cane::draw::State ps;
                    ps.program = glCelBorderShader.ID;
                    ps.blend = 1;
                    ps.srcColor = ps.srcAlpha = cane::draw::kFactorSrcAlpha;
                    ps.dstColor = ps.dstAlpha = cane::draw::kFactorOneMinusSrcAlpha;
                    ps.depthTest = 1;
                    ps.depthCompare = s_fCelAlways ? cane::draw::kAlways : cane::draw::kGreaterEqual;
                    if (s_uc.edgeBase >= 0) ps.aconst[ps.cconst++] = {s_uc.edgeBase, 0};
                    s_idCel = ppackets->createState(ps);
                    s_progCel = glCelBorderShader.ID;
                }
                if (s_idCel)
                {
                    otherProgram();
                    sc.Invalidate();
                    ppackets->setConstant(glCelBorderShader.ID, s_uc.matWorldToClip, glm::value_ptr(matWorldToClip), 64);
                    ppackets->setConstant(glCelBorderShader.ID, s_uc.matModel, glm::value_ptr(*cmd.prec->pmat), 64);
                    const float hy = pv.gCelWidth / 448.0f;
                    const glm::vec2 halfWidth(hy / aspect, hy);
                    ppackets->setConstant(glCelBorderShader.ID, s_uc.halfWidthNdc, glm::value_ptr(halfWidth), 8);
                    glm::vec4 rgba = Vec4FromRgba(opts.rgbaCel, 1.0f / 255.0f);
                    if (const float gFlash = cmd.prec->psgs ? cmd.prec->psgs->gFlash : 0.0f; 0.0f < gFlash)
                        rgba = glm::vec4(glm::mix(glm::vec3(rgba), glm::vec3(159.0f / 255.0f, 0.0f, 0.0f), std::min(gFlash, 1.0f)), rgba.a);
                    const float a2 = cmd.prec->alpha2;
                    rgba.a = std::min(rgba.a * 255.0f / 128.0f, 1.0f) * (a2 < 1.0f ? a2 * 0.6f : 1.0f);
                    ppackets->setConstant(glCelBorderShader.ID, s_uc.celRgba, glm::value_ptr(rgba), 16);
                    BindStorageWhole(8, geom.pgl->ssboEdges);
                    BindStorageWhole(9, geom.pgl->ssboEdgeSkin);
                    const cane::draw::Constant abone[2] = {{s_uc.cbone, uint32_t(cmd.prec->pamatBone ? GLint(cmd.prec->pamatBone->size()) : 0)},
                                                           {s_uc.iboneFirst, uint32_t(GLint(cmd.prec->iboneFirst))}};
                    ppackets->drawArrays(s_idCel, s_vaoEmpty, 0, uint32_t(geom.pgl->cedge * 6), abone, 2);
                    ++g_renderstats.ccel;
                    ++g_renderstats.cbatch;
                    break;
                }
            }
            glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST);
            glCelBorderShader.Use();
            otherProgram();
            sc.Invalidate();
            glUniformMatrix4fv(s_uc.matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
            glUniformMatrix4fv(s_uc.matModel, 1, GL_FALSE, glm::value_ptr(*cmd.prec->pmat));
            // Half a PS2 pixel (448 lines) in NDC, square pixels; the line is one PS2 pixel wide (AA1 line + plain).
            const float hy = pv.gCelWidth / 448.0f;
            glUniform2f(s_uc.halfWidthNdc, hy / aspect, hy);
            glUniform1i(s_uc.edgeBase, 0);
            glm::vec4 rgba = Vec4FromRgba(opts.rgbaCel, 1.0f / 255.0f);
            // the damage flash (qw8.z): the border toward red (the reference's border at flash 0.5 / 0.75 / 1: red 87 /
            // 123 / 159 of 255, linear in the flash)
            if (const float gFlash = cmd.prec->psgs ? cmd.prec->psgs->gFlash : 0.0f; 0.0f < gFlash)
                rgba = glm::vec4(glm::mix(glm::vec3(rgba), glm::vec3(159.0f / 255.0f, 0.0f, 0.0f), std::min(gFlash, 1.0f)), rgba.a);
            // P_A: the alpha times 0.6 times qw8.y while the object fades (runtime/native_cel_geometry.cpp)
            const float a2 = cmd.prec->alpha2;
            glUniform4f(s_uc.celRgba, rgba.r, rgba.g, rgba.b, std::min(rgba.a * 255.0f / 128.0f, 1.0f) * (a2 < 1.0f ? a2 * 0.6f : 1.0f));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, geom.pgl->ssboEdges);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, geom.pgl->ssboEdgeSkin);
            glUniform1i(s_uc.cbone, cmd.prec->pamatBone ? GLint(cmd.prec->pamatBone->size()) : 0);
            glUniform1i(s_uc.iboneFirst, GLint(cmd.prec->iboneFirst));
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(s_fCelAlways ? GL_ALWAYS : GL_GEQUAL);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindVertexArray(s_vaoEmpty);
            glDrawArrays(GL_TRIANGLES, 0, geom.pgl->cedge * 6);
            ++g_renderstats.ccel;
            ++g_renderstats.cbatch;
            break;
        }
        }
    }
    if (astat.fOn) AlphaStatEnd();
    if (fLrzAudit)
    {
        std::printf("lrzaudit: pass %d, %d runs:", s_cpassLrz, irunLrz);
        for (const auto& [k, n] : mpLrz) std::printf(" [%s %d]", k.c_str(), n);
        std::printf("\nlrzaudit: specials / ALWAYS+zwrite in order:%s\n", strLrzSeq.c_str());
    }
    FlushProgramPending();   // (the pass ends with the GL calls' program bound)
    // (VR) an eye's depth and stencil are not read after its pass (the foreground pass clears them first): a tile GPU
    // need not write them out when the next eye's target is bound
    if (psv && glInvalidateFramebuffer && ppackets && ppackets->invalidate && PpacketsFbo())
        ppackets->invalidate(fboEye, cane::draw::kClearDepth | cane::draw::kClearStencil);   // (the layer: as the two attachments)
    else if (psv && glInvalidateFramebuffer)
    {
        const GLenum aatt[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
        glBindFramebuffer(GL_FRAMEBUFFER, fboEye);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, aatt);
    }
    }   // (the eyes)
    if (s_cframeGlStats && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        if (++s_cframe % s_cframeGlStats == 0)
        {
            const double n = s_cframeGlStats;
            std::printf("glstats: per frame %.0f runs (%.0f draws), %.0f applies (%.0f full), %.0f texture binds, %.0f sampler binds, %.0f uniforms, %.0f blend, "
                        "%.1f shadow phases (%.1f frame alpha kept)%s%s\n",
                        s_glstats.crun / n, s_glstats.cpass / n, s_glstats.capply / n, s_glstats.cfull / n, s_glstats.ctex / n, s_glstats.csampler / n,
                        s_glstats.cuniform / n, s_glstats.cblend / n, s_glstats.cshphase / n, s_glstats.cshkept / n, g_fGlobBindless ? " (bindless)" : "",
                        s_ring.buf ? " (persistent ring)" : "");
            s_glstats = {};
        }
    }
    // (vertex array 0, as the GL calls leave it; native vertex arrays: no GL-style user reads the binding on Vulkan, every
    // GL-call draw path binds its own)
    if (!PpacketsVao()) glBindVertexArray(0);
    if (fIndirectAny) glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    if (g_fGlobArrays) glBindSampler(2, 0);   // (unit 2's sampler object would override another user's texture state)
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
}
}  // namespace

namespace
{
// orig: 0x001BE990 / 0x001BE9E8 (each owned target: its camera's view / projection (0x1BE6F0), the clear packet)
// then the target's kinds 15..17 / 18..20 drawn into it.
void RenderRenderTargets(const SCENE& scene, const RENDEROPTS& opts)
{
    for (int i = 0; i < CRTGT; ++i)
    {
        RTGT* p = PrtgtGet(i);
        if (!p || !p->powner) continue;
        GlTextureOfRtgt(i);   // (the FBO)
        PASSVIEW pv;
        pv.irtgt = i;
        pv.fbo = p->fbo;
        pv.dx = p->dx;
        pv.dy = p->dy;
        pv.aspect = float(p->dx) / float(p->dy);
        pv.pos = p->pos;
        pv.dir = glm::normalize(p->dirForward);
        // reversed-Z infinite far, the target's aspect (BuildSimpleProjectionMatrix with dx / (tan x aspect), dy / tan)
        const float f = 1.0f / std::tan(p->radFovY * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = f / pv.aspect;
        proj[1][1] = f;
        proj[2][3] = -1.0f;
        proj[3][2] = p->sNear;
        pv.matWorldToClip = proj * glm::lookAt(p->pos, p->pos + pv.dir, p->dirUp);
        pv.rgbaClear = 0;   // 0x1BE398's clear: RGBAQ 0, Z 0
        pv.alphaClear = 0;
        pv.gCelWidth = float(g_display.outlineThickness) / 100.0f;
        RenderPass(scene, pv, opts);
    }
}
}  // namespace

void RenderScene(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts)
{
    g_renderstats.cdraw = g_renderstats.cpass = g_renderstats.ccel = g_renderstats.cfailAll = 0;
    g_renderstats.cbatch = g_renderstats.cculled = g_renderstats.cmrd = 0;
    s_ring.BeginFrame();
    TakeStreamedTextureMods();
    if (FGpuMarks()) GpuMark("render targets");
    // The render targets first (their globsets, their cameras), so this frame's screen quads show them.
    RenderRenderTargets(scene, opts);
    PASSVIEW pv;
    pv.aspect = float(g_gl.renderWidth) / float(g_gl.renderHeight);
    // The projection for the picture's aspect mode (display.h: Hor+, the PS2 anamorphic 4:3, Vert- 16:9).
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    pv.matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    pv.rMRDAdjust = cam.rMRDAdjust;
    pv.grfChannels = cam.grfChannels;
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.rgbaClear = opts.rgbaClear;
    pv.gCelWidth = float(g_display.outlineThickness) / 100.0f;
    RenderPass(scene, pv, opts);
}

void RenderSceneForeground(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts)
{
    if (std::none_of(scene.aglobset.begin(), scene.aglobset.end(), [](const SCENEGLOBSET& s) { return s.fForeground && s.irtgt < 0; })) return;
    PASSVIEW pv;
    pv.aspect = float(g_gl.renderWidth) / float(g_gl.renderHeight);
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    pv.matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    pv.rMRDAdjust = cam.rMRDAdjust;
    pv.grfChannels = cam.grfChannels;
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.fForeground = true;
    pv.gCelWidth = float(g_display.outlineThickness) / 100.0f;
    RenderPass(scene, pv, opts);
}

// (VR, vr.h) the frame's pass and the foreground kinds into both eyes: the records chosen once from the frustum
// holding both (STEREOVIEW::matCull), each eye drawn with its own matrix. The picture-aspect options (display.h) do not
// apply: the eyes' projections are the headset's.
// (VR) the cel borders' width in the eyes, in PS2 lines of the eye's height: the flat picture's border is one PS2 line of
// its height (1 / 448), which in an eye's far wider field would cover several times the angle (about 4x on a Quest 3S /
// Steam Frame: the outlines looked thick). Matched: the same angle as the flat picture's border, i.e. scaled by the flat
// vertical field over the eye's (tan(up) - tan(down)); times Outline Thickness; at least one eye pixel wide (thinner
// lines break up). SLY2N_VR_CELWIDTH=g: g PS2 lines of the eye (the old look: 1).
static float CelWidthEyes(const CAMERA& cam, const STEREOVIEW& sv)
{
    static const float s_gForced = std::getenv("SLY2N_VR_CELWIDTH") ? float(std::atof(std::getenv("SLY2N_VR_CELWIDTH"))) : 0.0f;
    if (s_gForced > 0.0f) return s_gForced;
    const float gOption = float(g_display.outlineThickness) / 100.0f;
    if (sv.tanRangeY <= 0.0f || sv.dy <= 0) return gOption;
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    const float tanRangeFlat = 2.0f * std::tan(0.5f * proj.radFovY);
    const float gMatched = gOption * tanRangeFlat / sv.tanRangeY;
    const float gOnePixel = 448.0f / float(sv.dy);   // (the border's NDC width 2 x hy = 2 / dy: one pixel)
    const float g = std::clamp(std::max(gMatched, gOnePixel), 0.05f, 4.0f);
    static float s_gLogged = -1.0f;
    if (std::fabs(g - s_gLogged) > 0.05f * std::max(g, 0.01f) && std::getenv("SLY2N_VR_CELLOG"))
    {
        s_gLogged = g;
        std::printf("vr: cel borders %.3f PS2 lines of the eye (%.2f px; matched %.3f, field flat %.3f / eye %.3f)\n", double(g),
                    double(g * float(sv.dy) / 448.0f), double(gMatched), double(tanRangeFlat), double(sv.tanRangeY));
    }
    return g;
}

void RenderSceneStereo(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts, const STEREOVIEW& sv)
{
    g_renderstats.cdraw = g_renderstats.cpass = g_renderstats.ccel = g_renderstats.cfailAll = 0;
    g_renderstats.cbatch = g_renderstats.cculled = g_renderstats.cmrd = 0;
    s_ring.BeginFrame();
    TakeStreamedTextureMods();
    RenderRenderTargets(scene, opts);
    s_fFlipFaces = sv.fFlipY;
    PASSVIEW pv;
    pv.aspect = float(sv.dx) / float(sv.dy);
    pv.gCelWidth = CelWidthEyes(cam, sv);
    pv.matWorldToClip = sv.matCull;
    pv.rMRDAdjust = cam.rMRDAdjust;
    pv.grfChannels = cam.grfChannels;
    pv.pos = sv.posCenter;
    pv.dir = sv.dirCenter;
    pv.fbo = sv.afbo[0];
    pv.dx = sv.dx;
    pv.dy = sv.dy;
    pv.rgbaClear = opts.rgbaClear;
    pv.pstereo = &sv;
    // (multiview: one pass into the two-view target, view 0's matrix and position, view 1's clip from view 0's)
    if (sv.fMultiview)
    {
        // (OpenGL ES: view 1's clip from view 0's (gl.cpp GLSHADER::Use's two-view variants); Vulkan: the programs' two-view
        // stages read both eyes' values, RenderPass)
        g_fMultiviewPass = true;
        ++g_serialMultiview;
        const glm::mat4 m = sv.amatWorldToClip[1] * glm::inverse(sv.amatWorldToClip[0]);
        std::memcpy(g_amatMultiviewLtoR, glm::value_ptr(m), sizeof(g_amatMultiviewLtoR));
        g_texMultiviewColor = sv.texMultiviewColor;
        // (the glob programs' inherited uniforms are not known for the two-view variants: sent again)
        for (GLOBINHERIT& h : s_aglobinh) h = GLOBINHERIT{glm::vec4(-1.0f), -1};
    }
    RenderPass(scene, pv, opts);
    // (the HUD's full-screen fills over the world (the fades, a title card's colour), under the foreground kinds, as the
    // flat picture's order: vr.h FTakeEyeFill)
    vr::DrawEyeFills(sv);
    if (std::any_of(scene.aglobset.begin(), scene.aglobset.end(), [](const SCENEGLOBSET& s) { return s.fForeground && s.irtgt < 0; }))
    {
        pv.fForeground = true;
        RenderPass(scene, pv, opts);
    }
    if (sv.fMultiview)
    {
        g_fMultiviewPass = false;
        for (GLOBINHERIT& h : s_aglobinh) h = GLOBINHERIT{glm::vec4(-1.0f), -1};
    }
    s_fFlipFaces = false;
    glFrontFace(GL_CCW);
}
