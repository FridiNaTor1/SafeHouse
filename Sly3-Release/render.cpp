// See render.h.

#include "render.h"

#include "display.h"
#include "cm.h"
#include "emitter.h"
#include "gl.h"
#include "light.h"
#include "laserrender.h"
#include "pocket.h"
void CollectHackSwarmDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra);  // (hack.cpp: the swarms' particles)
#include "rtgt.h"
#include "wr.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <deque>
#include <map>
#include <unordered_map>
#include <string>

RENDERSTATS g_renderstats;
constinit PFNEXTRADRAWS g_pfnExtraDraws = nullptr;

struct GLGLOB
{
    GLuint vao = 0;             // the geometry arena's (one per BuildSceneGpu: all its globs' vertices and indices)
    GLuint vbo = 0;             // the arena's vertex buffer (colour updates)
    int32_t baseVertex = 0;     // where this glob's vertices / indices start in the arena
    uint32_t firstIndex = 0;
    GLuint ssboEdges = 0;
    GLuint ssboEdgeSkin = 0;   // per edge vertex: bone weights and indices (skinned cel borders)
    glm::vec3 posCenter{0.0f};  // the geometry's bounding sphere (for globs without a draw record)
    float sRadius = -1.0f;
    GLsizei cedge = 0;
    GLsizei cindex = 0;
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
};

namespace
{
// orig3: 0x18E9A4..0x18EA50 (the object) / 0x18EBF8..0x18EC58 (each group with a draw record), RenderGlob 0x18E800:
// the fade is linear over the last 400 units: clamp((MRD - d x s) x 0.0025 (0x321C90), 0, 1), d the camera's distance
// to the object's bounds centre (0x167FE8) for the object and its groups alike, s the camera's +0x114 (0.3 at the
// title's 17-degree shot, 0.81 in the binocucom, 1 in play: the native rMRDAdjust is 1 / s). Sly 2's was quadratic
// (UCmSphereDistanceFade) and used each group's own centre.
inline float UFadeSly3(float d, float sMRD, float rAdjust)
{
    return std::clamp((sMRD - d / rAdjust) * 0.0025f, 0.0f, 1.0f);
}
}  // namespace

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

std::vector<std::unique_ptr<GLGLOB>> s_aglglob;
std::map<std::pair<const BMP*, const CLUT*>, GLuint> s_mptex;
GLuint s_texWhite = 0;
GLuint s_asampler[8] = {};  // bit 0 clamp S, 1 clamp T, 2 nearest
GLuint s_vaoEmpty = 0;
// Geometry arenas: the globs built by one BuildSceneGpu share one VBO / EBO / VAO, so a run of draws with one GL
// state is one glMultiDrawElementsIndirect. Attribute 6 is the draw index (an iota buffer, divisor 1, selected by
// the command's baseInstance) into the per-frame draw data (glob.vert DRAWS / RECS).
std::vector<struct GPUVTX> s_avtxArena;
std::vector<uint32_t> s_aindexArena;
std::vector<GLGLOB*> s_apglArena;
GLuint s_vboDrawIndex = 0;
constexpr uint32_t CDRAW_MAX = 1u << 18;
const float s_celBias = std::getenv("SLY2_CELBIAS") ? float(std::atof(std::getenv("SLY2_CELBIAS"))) : 0.01f;

GLuint TexFromSel(const TEXSEL& sel)
{
    InitCommonOnce();
    if (!sel.pbmp) return s_texWhite;
    const std::pair<const BMP*, const CLUT*> key(sel.pbmp, sel.pclut);
    auto it = s_mptex.find(key);
    if (it != s_mptex.end()) return it->second;
    std::vector<uint8_t> rgba;
    std::string error;
    GLuint tex = s_texWhite;
    if (FDecodeTexture(sel.pbmp, sel.pclut, rgba, error))
    {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sel.pbmp->dx, sel.pbmp->dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        ++g_renderstats.ctexture;
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
    glGenTextures(1, &s_texWhite);
    glBindTexture(GL_TEXTURE_2D, s_texWhite);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glGenSamplers(8, s_asampler);
    for (int i = 0; i < 8; ++i)
    {
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_S, (i & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_T, (i & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        const GLint filter = (i & 4) ? GL_NEAREST : GL_LINEAR;
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MIN_FILTER, filter);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MAG_FILTER, filter);
    }
    glGenVertexArrays(1, &s_vaoEmpty);
    std::vector<uint32_t> aiota(CDRAW_MAX);
    for (uint32_t i = 0; i < CDRAW_MAX; ++i) aiota[i] = i;
    glGenBuffers(1, &s_vboDrawIndex);
    glBindBuffer(GL_ARRAY_BUFFER, s_vboDrawIndex);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(aiota.size() * 4), aiota.data(), GL_STATIC_DRAW);
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
}

void AppendArena(GLGLOB* pgl, const std::vector<GPUVTX>& av, const std::vector<uint32_t>& ai)
{
    SetBounds(pgl, av.size(), [&](size_t i) { return glm::vec3(av[i].pos[0], av[i].pos[1], av[i].pos[2]); });
    pgl->baseVertex = int32_t(s_avtxArena.size());
    pgl->firstIndex = uint32_t(s_aindexArena.size());
    s_avtxArena.insert(s_avtxArena.end(), av.begin(), av.end());
    s_aindexArena.insert(s_aindexArena.end(), ai.begin(), ai.end());
    s_apglArena.push_back(pgl);
}

void FlushArena()
{
    if (s_apglArena.empty()) return;
    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(s_avtxArena.size() * sizeof(GPUVTX)), s_avtxArena.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(s_aindexArena.size() * 4), s_aindexArena.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, rgbaA)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, rgbaB)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, normal)));
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, abone)));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, aw)));
    glBindBuffer(GL_ARRAY_BUFFER, s_vboDrawIndex);
    glEnableVertexAttribArray(6);
    glVertexAttribIPointer(6, 1, GL_UNSIGNED_INT, 4, nullptr);
    glVertexAttribDivisor(6, 1);
    glBindVertexArray(0);
    for (GLGLOB* pgl : s_apglArena) { pgl->vao = vao; pgl->vbo = vbo; }
    s_apglArena.clear();
    std::vector<GPUVTX>().swap(s_avtxArena);
    std::vector<uint32_t>().swap(s_aindexArena);
}

void GpuVerticesOfGlob(const GLOB& glob, std::vector<GPUVTX>& av)
{
    av.resize(glob.avtx.size());
    for (size_t i = 0; i < av.size(); ++i)
    {
        const VTXGLOB& v = glob.avtx[i];
        std::memcpy(av[i].pos, &v.pos, 12);
        std::memcpy(av[i].uv, &v.uv, 8);
        std::memcpy(av[i].rgbaA, &v.rgbaA, 4);
        std::memcpy(av[i].rgbaB, &v.rgbaB, 4);
        std::memcpy(av[i].normal, &v.normal, 12);
        std::memcpy(av[i].abone, v.abone, 4);  // (skinning, animation code)
        std::memcpy(av[i].aw, v.aw, 4);
    }
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
        glGenBuffers(1, &pgl->ssboEdges);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pgl->ssboEdges);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(aedge.size() * sizeof(glm::vec4)), aedge.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &pgl->ssboEdgeSkin);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pgl->ssboEdgeSkin);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(askin.size() * sizeof(EDGESKIN)), askin.data(), GL_STATIC_DRAW);
        pgl->cedge = GLsizei(aedge.size() / 4);
        SetBounds(pgl.get(), aedge.size(), [&](size_t i) { return glm::vec3(aedge[i]); });
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
}

// GS ALPHA -> GL blend: Cv = (A - B) * C + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}. Each colour's coefficient
// is k1 + kC * C; GL factors cover 0, 1, C and 1 - C (and -C via reverse subtract).
bool FSetBlend(uint64_t alpha)
{
    const int a = int(alpha & 3), b = int((alpha >> 2) & 3), c = int((alpha >> 4) & 3), d = int((alpha >> 6) & 3);
    const float fix = float((alpha >> 32) & 0xFF) / 128.0f;
    auto coef = [&](int sel, int& k1, int& kc) {
        k1 = (d == sel) ? 1 : 0;
        kc = (a == sel ? 1 : 0) - (b == sel ? 1 : 0);
    };
    int s1, sc, d1, dc;
    coef(0, s1, sc);
    coef(1, d1, dc);
    const GLenum cf = c == 0 ? GL_SRC_ALPHA : c == 1 ? GL_DST_ALPHA : GL_CONSTANT_ALPHA;
    const GLenum cfInv = c == 0 ? GL_ONE_MINUS_SRC_ALPHA : c == 1 ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_CONSTANT_ALPHA;
    if (c == 2) glBlendColor(0, 0, 0, fix);
    auto factor = [&](int k1, int kc, GLenum& f) {
        if (k1 == 0 && kc == 0) f = GL_ZERO;
        else if (k1 == 1 && kc == 0) f = GL_ONE;
        else if (k1 == 0 && kc == 1) f = cf;
        else if (k1 == 1 && kc == -1) f = cfInv;
        else return false;
        return true;
    };
    GLenum fs, fd;
    if (sc == -1 && s1 == 0 && factor(d1, dc, fd))
    {
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(cf, fd);
        return true;
    }
    glBlendEquation(GL_FUNC_ADD);
    if (!factor(s1, sc, fs) || !factor(d1, dc, fd))
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        return false;
    }
    glBlendFunc(fs, fd);
    return true;
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

struct UNIFORMS
{
    GLint matWorldToClip, icolor, rgbaConst, posCamera, dirCamera;
    GLint tex, fTextured, ate, atst, aref, afailMode, fogColor;
} s_u;
struct CELUNIFORMS { GLint matWorldToClip, matModel, halfWidthNdc, edgeBase, celRgba, depthBias, cbone, iboneFirst; } s_uc;

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
    uint8_t pad[4] = {};
};
static_assert(sizeof(DSTATE) == 32);
bool operator==(const DSTATE& a, const DSTATE& b) { return std::memcmp(&a, &b, sizeof(DSTATE)) == 0; }

enum CMDK : uint8_t { CMDK_Pass, CMDK_Volume, CMDK_Shadow, CMDK_Cel, CMDK_Particles, CMDK_Shadow2 };
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
    const DRAWREC* prec = nullptr;
    const SUBGLOB* psub = nullptr;
};
struct DEICMD { uint32_t count, instanceCount, firstIndex; int32_t baseVertex; uint32_t baseInstance; };

GLuint s_ssboRec = 0, s_ssboDraw = 0, s_bufIndirect = 0, s_ssboBone = 0, s_ssboWarp = 0, s_ssboWarpMat = 0;

// Applies a run's state, setting only what changed since the last run (fForce: everything).
struct STATECACHE
{
    DSTATE cur;
    bool fValid = false;
    void Apply(const DSTATE& st)
    {
        const bool f = !fValid;
        if (f || st.vao != cur.vao) glBindVertexArray(st.vao);
        if (st.fTextured && (f || !cur.fTextured || st.tex != cur.tex)) glBindTexture(GL_TEXTURE_2D, st.tex);
        if (st.fTextured && (f || !cur.fTextured || st.sampler != cur.sampler)) glBindSampler(0, s_asampler[st.sampler]);
        if (f || st.fTextured != cur.fTextured) glUniform1i(s_u.fTextured, st.fTextured);
        if (f || st.icolor != cur.icolor) glUniform1i(s_u.icolor, st.icolor);
        if (f || st.fBlend != cur.fBlend) { if (st.fBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
        if (st.fBlend && (f || !cur.fBlend || st.alpha != cur.alpha)) FSetBlend(st.alpha);
        if (f || st.fCull != cur.fCull)
        {
            if (st.fCull) { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); }
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
        if (f || st.ate != cur.ate) glUniform1i(s_u.ate, st.ate);
        if (f || st.atst != cur.atst) glUniform1i(s_u.atst, st.atst);
        if (f || st.aref != cur.aref) glUniform1f(s_u.aref, float(st.aref));
        if (f || st.afailMode != cur.afailMode) glUniform1i(s_u.afailMode, st.afailMode);
        cur = st;
        fValid = true;
    }
};

// One shader pass of an entry (formerly drawn directly): the GL state its GS registers ask for, as 1..2 commands
// (the alpha test's pass / fail sub-draws).
void EmitPass(std::vector<PASSCMD>& apasscmd, DSTATE st, const SUBGLOB& sub, const SHDPASS& pass, int ipass)
{
    // Texture
    if (pass.fTextured)
    {
        const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, pass.itexPass);
        st.tex = TexFromSel(sel);
        st.sampler = uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0) | (pass.fNearest ? 4 : 0));
    }
    st.fTextured = pass.fTextured ? 1 : 0;
    st.icolor = uint8_t(pass.icolor);
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
    if (!ate || atst == 1)
        draw(0, 0, true, fWritesColor, true);
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
    if (p->pshd != sub.pshd || p->iframe != iframe || p->vao != vao || p->fCull != fCull)
    {
        p->pshd = sub.pshd;
        p->iframe = iframe;
        p->vao = vao;
        p->fCull = fCull;
        p->apasscmd.clear();
        DSTATE st;
        st.vao = vao;
        st.fCull = fCull;
        for (size_t ipass = 0; ipass < sub.pshd->apass.size(); ++ipass)
            EmitPass(p->apasscmd, st, sub, sub.pshd->apass[ipass], int(ipass));
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
GLSHADER s_shShadowVol;
GLuint s_vaoShadowVol = 0, s_vboShadowVol = 0;

bool FBuildShadow2(const SCENEGLOBSET& sgs, size_t iglob, const GLOB& glob, const glm::vec3& posCamera, float sNear, CMD& cmd)
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
    static std::vector<std::vector<glm::vec3>> s_aav;
    s_aav.resize(glob.asubfx.size());
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
                    w += float(fx.aw[v * size_t(fx.cw) + size_t(j)]) / 255.0f * glm::vec3((*pamatBone)[ib] * p);
                }
            }
            else w = glm::vec3(mat * p);
            av[v] = w;
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    }
    const glm::vec3 posCtx = sgs.fPosLight ? sgs.posLight : (lo + hi) * 0.5f;
    float sRadius = 0;   // (pl +0x1C: the shadow-casting hierarchy's radius about the context's position)
    for (const auto& av : s_aav)
        for (const glm::vec3& v : av) sRadius = std::max(sRadius, glm::length(v - posCtx));
    const SHADOWCTX sh = ShadowOfLightContext(WorldLights(), posCtx, sRadius, 400.0f, posCamera, sNear);   // (+0x13C 0x2C3B90)
    if (!sh.fValid) return false;
    if (s_fDbg)
        std::printf("shadow2: lo %.0f %.0f %.0f hi %.0f %.0f %.0f bones %zu\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, pamatBone ? pamatBone->size() : 0),
        std::printf("shadow2: ctx %.0f %.0f %.0f r %.0f centre %.0f %.0f %.0f axis %.0f %.0f %.0f\n", posCtx.x, posCtx.y, posCtx.z, sRadius,
                    sh.center.x, sh.center.y, sh.center.z, sh.axis.x, sh.axis.y, sh.axis.z);
    const glm::vec3 dir = glm::normalize(sh.axis);
    const glm::vec3 posFar = sh.center + sh.axis;
    const float dFar = glm::dot(posFar - sh.center, dir);
    auto project = [&](const glm::vec3& v) {
        const float dv = glm::dot(v - sh.center, dir);
        if (dv <= 1.0f) return v + dir * dFar;
        return sh.center + (v - sh.center) * (dFar / dv);
    };
    const uint32_t ivtxFirst = uint32_t(s_avtxShadow2.size());
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, const glm::vec3& outward) {
        if (glm::dot(glm::cross(b - a, c - a), outward) < 0.0f) std::swap(b, c);
        s_avtxShadow2.push_back(a);
        s_avtxShadow2.push_back(b);
        s_avtxShadow2.push_back(c);
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
        static std::vector<uint64_t> s_abDone((size_t(1) << 21) / 64, 0);
        static std::vector<uint32_t> s_akDone;
        for (uint32_t k : s_akDone) s_abDone[k >> 6] = 0;
        s_akDone.clear();
        auto prism = [&](uint8_t ia, uint8_t ib, uint8_t ic) {
            std::array<uint8_t, 3> key{ia, ib, ic};
            std::sort(key.begin(), key.end());
            if (key[0] == key[1] || key[1] == key[2]) return;
            const uint32_t k = (uint32_t(key[0] >> 1) << 14) | (uint32_t(key[1] >> 1) << 7) | uint32_t(key[2] >> 1);
            if (s_abDone[k >> 6] & (uint64_t(1) << (k & 63))) return;
            s_abDone[k >> 6] |= uint64_t(1) << (k & 63);
            s_akDone.push_back(k);
            const glm::vec3 a = P(ia), b = P(ib), c = P(ic);
            if (!lit(a, b, c) && !lit(a, c, b)) return;   // (edge-on)
            const glm::vec3 af = project(a), bf = project(b), cf = project(c);
            const glm::vec3 m = (a + b + c + af + bf + cf) / 6.0f;
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
    cmd.firstIndex = ivtxFirst;
    cmd.count = uint32_t(s_avtxShadow2.size()) - ivtxFirst;
    cmd.idraw = uint32_t(glob.asubfx[0].pshd->argba[0] >> 24);   // (the darkening alpha)
    return cmd.count > 0;
}

void DrawShadow2(const CMD& cmd, const glm::mat4& matWorldToClip)
{
    if (!s_shShadowVol.ID)
    {
        s_shShadowVol.Init("shadowvol.vert", "shadowvol.frag");
        glGenVertexArrays(1, &s_vaoShadowVol);
        glGenBuffers(1, &s_vboShadowVol);
        glBindVertexArray(s_vaoShadowVol);
        glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    }
    s_shShadowVol.Use();
    glUniformMatrix4fv(s_shShadowVol.Loc("matWorldToClip"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
    glBindVertexArray(s_vaoShadowVol);
    glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(size_t(cmd.count) * sizeof(glm::vec3)), s_avtxShadow2.data() + cmd.firstIndex, GL_STREAM_DRAW);
    const GLint locRgba = s_shShadowVol.Loc("rgba");
    // the z-fail count (as DrawShadow)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x7F);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 0, 0x7F);
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(cmd.count));
    // darken where the count is not zero, and clear it
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDepthFunc(GL_ALWAYS);
    glStencilFunc(GL_NOTEQUAL, 0, 0x7F);
    glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    glUniform4f(locRgba, 0, 0, 0, std::min(float(cmd.idraw) / 128.0f, 1.0f));
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(cmd.count));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

void DrawOne(const CMD& cmd)
{
    glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, GLsizei(cmd.count), GL_UNSIGNED_INT,
                                                  reinterpret_cast<void*>(size_t(cmd.firstIndex) * 4), 1, cmd.baseVertex, cmd.idraw);
    ++g_renderstats.cpass;
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
    glBindVertexArray(cmd.vao);
    glUniform1i(s_u.fTextured, 0);
    glUniform1i(s_u.ate, 0);
    glUniform1i(s_u.afailMode, 0);
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
        glFrontFace(GL_CCW);
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
        static const bool s_fVolDbg = std::getenv("SLY2N_VOLDBG") != nullptr;  // (debug: the filled pixels red)
        if (s_fVolDbg) glUniform4f(s_u.rgbaConst, 255.0f, 0.0f, 0.0f, 128.0f);
        else glUniform4f(s_u.rgbaConst, c.r, c.g, c.b, c.a);
        // (the lists' DATE fill is a flat sprite of (rgba1 + 1) / 2, not faded by the hull's vertex alpha: SLY2N_VOLVA=1
        // fades it for comparison)
        static const bool s_fVa = std::getenv("SLY2N_VOLVA") != nullptr;
        glUniform1i(s_u.icolor, s_fVa ? 3 : 2);
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
    glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_NOTEQUAL, 0x80, 0x80);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glEnable(GL_BLEND);
    FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
    glUniform4f(s_u.rgbaConst, c.r, c.g, c.b, c.a);
    glUniform1i(s_u.icolor, 3);
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
        glFrontFace(GL_CCW);
        glDepthFunc(GL_GEQUAL);
        const TEXSEL sel = TexselFromShd(pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
        glBindTexture(GL_TEXTURE_2D, TexFromSel(sel));
        glBindSampler(0, s_asampler[(sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0)]);
        glUniform1i(s_u.fTextured, 1);
        glUniform1i(s_u.icolor, 0);
        FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
        DrawOne(cmd);
    }
}

// Shader kind 2 on rp 7 / 8: shadows. The original marks the shadowed pixels in the frame buffer's spare channel
// (AddScreenEffect 0x16DA48, two untextured passes per hull, DATE) and darkens them in AddScreenCombine 0x16DD58 by
// the shader's alpha. Natively: a z-fail stencil volume per hull, then the pixels inside darkened by rgba.a / 128.
void DrawShadow(const CMD& cmd)
{
    const GLOB& geom = *cmd.prec->pglobGeom;
    glBindVertexArray(cmd.vao);
    glUniform1i(s_u.fTextured, 0);
    glUniform1i(s_u.ate, 0);
    glUniform1i(s_u.afailMode, 0);
    const uint32_t rgba = geom.asubfx[0].pshd->argba[0];
    glUniform4f(s_u.rgbaConst, 0, 0, 0, float(rgba >> 24));
    glUniform1i(s_u.icolor, 2);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x7F);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    static const bool s_fZFail = std::getenv("SLY2N_SHADOWZFAIL") != nullptr;   // (debug: the earlier z-fail count)
    if (s_fZFail)
    {
        glStencilFunc(GL_ALWAYS, 0, 0x7F);
        // reversed Z: the depth test fails where the scene is nearer than the hull's face
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
        DrawOne(cmd);
    }
    else
    {
        // The original's two passes mark, then unmark (U9): set where the scene is in front of a back face, cleared
        // where it is in front of a front face, so only the scene inside the hull stays marked. A hull hidden
        // behind a wall or floor marks nothing (the z-fail count left open hulls' leftovers there: black shapes
        // through walls in the Ep1 recon rooms).
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glStencilFunc(GL_ALWAYS, 1, 0x7F);
        glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
        DrawOne(cmd);
        glCullFace(GL_BACK);
        glStencilFunc(GL_ALWAYS, 0, 0x7F);
        DrawOne(cmd);
        glDisable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    // darken where the count is not zero, and clear it
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDepthFunc(GL_ALWAYS);
    glStencilFunc(GL_NOTEQUAL, 0, 0x7F);
    glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    DrawOne(cmd);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}
}  // namespace

void UpdateGlobColorsGpu(const GLOB& glob) { UpdateGlobColorsGpu(glob, glob.pgl); }

void SetGlobBoundsGpu(const GLOB& glob, const glm::vec3& posCenter, float sRadius)
{
    if (!glob.pgl) return;
    glob.pgl->posCenter = posCenter;
    glob.pgl->sRadius = sRadius;
}

void UpdateGlobColorsGpu(const GLOB& glob, const GLGLOB* pgl)
{
    if (!pgl || !pgl->vbo || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    static std::vector<GPUVTX> s_av;
    GpuVerticesOfGlob(glob, s_av);
    glBindBuffer(GL_ARRAY_BUFFER, pgl->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(pgl->baseVertex) * sizeof(GPUVTX)), GLsizeiptr(s_av.size() * sizeof(GPUVTX)), s_av.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GLGLOB* PglInstanceCopyGpu(const GLOB& glob)
{
    if (glob.meshk != MESHK_Strips || glob.avtx.empty() || glob.aindex.empty()) return nullptr;
    auto pgl = std::make_unique<GLGLOB>();
    std::vector<GPUVTX> av;
    GpuVerticesOfGlob(glob, av);
    AppendArena(pgl.get(), av, glob.aindex);
    GLGLOB* p = pgl.get();
    s_aglglob.push_back(std::move(pgl));
    return p;
}

void FlushInstanceGlobsGpu() { FlushArena(); }

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

void BuildSceneGpu(SCENE& scene)
{
    InitCommon();
    for (SCENEGLOBSET& sgs : scene.aglobset)
        for (GLOB& glob : sgs.pglobset->aglob)
            BuildGlobGpu(glob);
    FlushArena();
    s_u.matWorldToClip = glGlobShader.Loc("matWorldToClip");
    s_u.icolor = glGlobShader.Loc("icolor");
    s_u.rgbaConst = glGlobShader.Loc("rgbaConst");
    s_u.posCamera = glGlobShader.Loc("posCamera");
    s_u.dirCamera = glGlobShader.Loc("dirCamera");
    s_u.tex = glGlobShader.Loc("tex");
    s_u.fTextured = glGlobShader.Loc("fTextured");
    s_u.ate = glGlobShader.Loc("ate");
    s_u.atst = glGlobShader.Loc("atst");
    s_u.aref = glGlobShader.Loc("aref");
    s_u.afailMode = glGlobShader.Loc("afailMode");
    s_u.fogColor = glGlobShader.Loc("fogColor");
    s_uc.matWorldToClip = glCelBorderShader.Loc("matWorldToClip");
    s_uc.matModel = glCelBorderShader.Loc("matModel");
    s_uc.halfWidthNdc = glCelBorderShader.Loc("halfWidthNdc");
    s_uc.edgeBase = glCelBorderShader.Loc("edgeBase");
    s_uc.celRgba = glCelBorderShader.Loc("celRgba");
    s_uc.depthBias = glCelBorderShader.Loc("depthBias");
    s_uc.cbone = glCelBorderShader.Loc("cbone");
    s_uc.iboneFirst = glCelBorderShader.Loc("iboneFirst");
    if (!s_ssboRec)
    {
        glGenBuffers(1, &s_ssboBone);
        glGenBuffers(1, &s_ssboRec);
        glGenBuffers(1, &s_ssboDraw);
        glGenBuffers(1, &s_bufIndirect);
        glGenBuffers(1, &s_ssboWarp);
        glGenBuffers(1, &s_ssboWarpMat);
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
    float rMRDAdjust = 1.0f;
    int irtgt = -1;
    bool fForeground = false;   // the foreground kinds 22..25 only (after the HUD, over a cleared depth)
    uint32_t rgbaClear = 0;
    uint8_t alphaClear = 0;
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
        uint32_t sum = 0;
        for (uint32_t& c : s_ac) { const uint32_t n = c; c = sum; sum += n; }
        for (uint64_t k : *psrc) (*pdst)[s_ac[(k >> shift) & 0xFFF]++] = k;
        std::swap(psrc, pdst);
    }
    if (psrc != &akey) akey.swap(*psrc);   // (three passes: the result is in the other buffer)
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

void RenderPass(const SCENE& scene, const PASSVIEW& pv, const RENDEROPTS& opts)
{
    const float aspect = pv.aspect;
    const glm::mat4& matWorldToClip = pv.matWorldToClip;
    struct { glm::vec3 pos, dir; glm::vec3 Dir() const { return dir; } } cam{pv.pos, pv.dir};
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
    auto pmatBillboard = [&](const glm::mat4& m, int rtck) -> const glm::mat4* {
        glm::mat4 mb;
        if (!FBillboardMatrix(m, rtck, cam.pos, vecUpCam, mb)) return &m;
        amatBillboard.push_back(mb);
        return &amatBillboard.back();
    };
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
    if (std::getenv("SLY2_RTCKDBG") && pv.irtgt < 0 && !pv.fForeground)   // debugging: the groups with a billboard mode
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
                }
    }
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.fTransform || sgs.irtgt != pv.irtgt) continue;
        static const char* s_range = std::getenv("SLY2_IBRANGE");   // debugging: only globsets in [lo, hi)
        if (s_range)
        {
            size_t lo = 0, hi = 0;
            std::sscanf(s_range, "%zu,%zu", &lo, &hi);
            if (sgs.ib < lo || sgs.ib >= hi) continue;
        }
        const GLOBSET& gs = *sgs.pglobset;
        // orig: 0x1686AC..0x1687A8: the whole sphere precedes every group and supplies f20.
        float wholeFade = 1.0f;
        const float dObj = sgs.fBounds ? glm::distance(sgs.posBounds, cam.pos) : -1.0f;   // (Sly 3: the groups fade by it too)
        if (pv.irtgt < 0 && sgs.fBounds)
        {
            wholeFade = UFadeSly3(dObj, sgs.sMRD, cmFade.rMRDAdjust);
            if (sgs.oid == s_oidFade)
                std::printf("fadeobj oid %d center %.9g %.9g %.9g radius %.9g mrd %.9g adjust %.9g fade %.9g\n",
                            sgs.oid, sgs.posBounds.x, sgs.posBounds.y, sgs.posBounds.z, sgs.sBoundsRadius, sgs.sMRD, cmFade.rMRDAdjust, wholeFade);
            if (wholeFade == 0.0f) { ++g_renderstats.cmrd; continue; }
            if (!s_fNoFrustum && outside(sgs.posBounds, sgs.sBoundsRadius)) { ++g_renderstats.cculled; continue; }
        }
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
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
            const glm::vec3 posWorld = glm::vec3(mat * glm::vec4(glob.posCenter, 1.0f));
            const float d = glm::distance(posWorld, cam.pos);
            static const int s_oidRec = std::getenv("SLY2_RECOID") ? std::atoi(std::getenv("SLY2_RECOID")) : -2;   // debugging
            static const int s_oidOnly = std::getenv("SLY2_ONLYOID") ? std::atoi(std::getenv("SLY2_ONLYOID")) : -2;   // debugging: only this object
            if (s_oidOnly != -2 && sgs.oid != s_oidOnly) continue;
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
            if (pv.irtgt < 0 && glob.fDraw)
            {
                // orig3: 0x18EBF8..0x18EC58: the group's MRD (draw record +0x10) against the object's scaled distance
                const CM* pcmG = pgeom->meshk == MESHK_CelBorder ? &cmFadeCel : &cmFade;
                groupFade = UFadeSly3(dObj >= 0.0f ? dObj : d, glob.sMRD, pcmG->rMRDAdjust);
                fDistance = groupFade != 0.0f;
            }
            const float fade = s_fNoFade ? 1.0f : std::min(wholeFade, groupFade);
            if (sgs.oid == s_oidFade)
                std::printf("fadegroup oid %d group %zu center %.9g %.9g %.9g radius %.9g mrd %.9g skinned %d visible %d whole %.9g groupfade %.9g alpha %.9g alpha2 %.9g\n",
                            sgs.oid, i, posWorld.x, posWorld.y, posWorld.z, radius, glob.sMRD, int(fSkinned), int(fDistance), wholeFade,
                            groupFade, sgs.gAlpha * fade, sgs.gAlpha2 * fade);
            if (!fDistance) { ++g_renderstats.cmrd; continue; }
            const bool fGeomBounds = !glob.fDraw && pgeom->pgl && pgeom->pgl->sRadius >= 0 && !fSkinned;
            if (pv.irtgt < 0 && !s_fNoFrustum && pgeom->meshk != MESHK_Effect && ((glob.fDraw && !fSkinned) || fGeomBounds))
            {
                const glm::vec3 posC = fGeomBounds ? glm::vec3(mat * glm::vec4(pgeom->pgl->posCenter, 1.0f)) : posWorld;
                const float r = fGeomBounds ? pgeom->pgl->sRadius * maxScale(mat) : radius;
                if (outside(posC, r)) { ++g_renderstats.cculled; continue; }
            }
            DRAWREC rec;
            rec.kind = kind;
            // orig: 0x168D64..0x168D94: both input alphas receive min(wholeFade, groupFade).
            // orig: 0x169198 RenderGlobCtx: render-target draws use unit alphas without the main-view tests.
            rec.alpha = pv.irtgt >= 0 ? 1.0f : std::clamp(sgs.gAlpha * fade, 0.0f, 1.0f);
            rec.alpha2 = pv.irtgt >= 0 ? 1.0f : std::clamp(sgs.gAlpha2 * fade, 0.0f, 1.0f);
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
            rec.z = (glob.grfGroup & 0x400) ? glob.gZOrder : d * d;
            rec.pglob = &glob;
            rec.pglobGeom = pgeom;
            rec.pmat = pmatBillboard(sgs.amatGlob[i], glob.rtck);
            rec.psgs = &sgs;
            rec.iglob = i;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty()) rec.pamatBone = &sgs.aamatBone[i];
            arec.push_back(rec);
            if (sgs.oid == s_oidFade)
                std::printf("fadesubmit oid %d group %zu kind %d alpha %.9g alpha2 %.9g\n", sgs.oid, i, rec.kind, rec.alpha, rec.alpha2);
        }
    }
    if (pv.irtgt < 0 && !pv.fForeground && std::getenv("SLY2_RECLIST"))   // debugging: the objects with records this frame
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
            wholeFade = UFadeSly3(glm::distance(x.posBounds, cam.pos), x.sMRD, cmFade.rMRDAdjust);
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
    std::stable_sort(arec.begin(), arec.end(), [](const DRAWREC& a, const DRAWREC& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        switch (a.kind)
        {
        case 1: case 2: case 5: case 6: case 10: case 25: return a.z > b.z;
        case 21: return a.z < b.z;
        default: return false;
        }
    });

    // The frame's draw stream: per record its matrix / fog (RECGPU), per entry its draw data (DRAWGPU), per pass
    // one or two commands with their GL state; volumes, shadows and cel borders are special commands.
    static std::vector<RECGPU> s_arecgpu;
    static std::vector<DRAWGPU> s_adrawgpu;
    static std::vector<CMD> s_acmd;
    static std::vector<glm::mat4> s_amatBone;
    s_amatBone.clear();
    for (DRAWREC& rec : arec)
        if (rec.pamatBone)
        {
            rec.iboneFirst = uint32_t(s_amatBone.size());
            s_amatBone.insert(s_amatBone.end(), rec.pamatBone->begin(), rec.pamatBone->end());
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
    // The type-2 shadows (render kind 7) draw just before the characters (kind 4): on the PS2 they come after them
    // but leave the characters' pixels alone (by the frame's alpha, as the kind-3 volumes' DATE mask); drawing the
    // volumes against the world's depth alone and the characters over them gives the same picture.
    bool fShadow2 = false;
    auto addShadow2 = [&]() {
        if (fShadow2 || !opts.fShadows || pv.fForeground) return;
        fShadow2 = true;
        // all the volumes in one count, darkened once (their union: a character's overlapping pieces, as the PS2's
        // frame-alpha mask, darken the floor once)
        CMD cmdAll;
        cmdAll.firstIndex = uint32_t(s_avtxShadow2.size());
        cmdAll.count = 0;
        cmdAll.idraw = 0;
        for (const DRAWREC& rec : arec)
        {
            const GLOB& geom = *rec.pglobGeom;
            if (geom.meshk != MESHK_Effect || !rec.psgs || geom.asubfx.empty() || !geom.asubfx[0].pshd) continue;
            CMD cmd;
            if (!FBuildShadow2(*rec.psgs, rec.iglob, geom, cam.pos, 52.0f, cmd)) continue;
            cmdAll.count += cmd.count;
            cmdAll.idraw = std::max(cmdAll.idraw, cmd.idraw);
            cmdAll.prec = &rec;
        }
        if (cmdAll.count == 0) return;
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        cmdAll.cmdk = CMDK_Shadow2;
        s_acmd.push_back(cmdAll);
        endSegment(false);
    };
    for (const DRAWREC& rec : arec)
    {
        const GLOB& glob = *rec.pglob;
        const GLOB& geom = *rec.pglobGeom;
        static const bool s_fSh2Late = std::getenv("SLY2_SH2LATE") != nullptr;   // experiment: after the characters
        if (rec.kind >= (s_fSh2Late ? 5 : 4)) addShadow2();
        if (rec.kind > 12) addParticles();
        if (rec.kind != kindSeg) { endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg)); kindSeg = rec.kind; }
        if (s_adrawgpu.size() + geom.asubglob.size() + 1 > CDRAW_MAX) break;
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
            // P_G culls back faces unless the mesh is double-sided (flag 0x10 -> param 0x80).
            const uint8_t fCull = !(geom.grfMesh & 0x10) && !fCullNone;
            base.baseVertex = pglDraw->baseVertex;
            base.prec = &rec;
            for (const SUBGLOB& sub : geom.asubglob)
            {
                if (!sub.pshd || sub.indexCount == 0) continue;
                ++g_renderstats.cdraw;
                base.idraw = uint32_t(s_adrawgpu.size());
                base.count = sub.indexCount;
                base.firstIndex = pglDraw->firstIndex + sub.firstIndex;
                base.psub = &sub;
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
                s_adrawgpu.push_back({sub.psaa ? sub.psaa->du / 4096.0f : 0.0f, sub.psaa ? sub.psaa->dv / 4096.0f : 0.0f, rec.alpha, irec});
                for (const PASSCMD& pc : SubgpuOf(sub, pglDraw->vao, fCull).apasscmd)
                {
                    base.istate = pc.istate;
                    base.order = pc.order;
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
    addShadow2();
    addParticles();
    endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
    // Regroup: within a segment of an unsorted kind, by (pass, sub-draw) then state (stable: record order inside).
    static std::vector<uint64_t> s_akey;
    static std::vector<CMD> s_acmdTmp;
    for (const SEGMENT& seg : aseg)
    {
        if (!seg.fRegroup || seg.icmdLim - seg.icmdFirst < 2) continue;
        s_akey.clear();
        for (size_t i = seg.icmdFirst; i < seg.icmdLim; ++i)
        {
            const CMD& c = s_acmd[i];
            s_akey.push_back((uint64_t(std::min(c.order, 0xFFFu)) << 52) | (uint64_t(c.istate & 0xFFFFFFu) << 28) | uint64_t(i - seg.icmdFirst));
        }
        SortKeysUnique(s_akey);
        s_acmdTmp.assign(s_acmd.begin() + std::ptrdiff_t(seg.icmdFirst), s_acmd.begin() + std::ptrdiff_t(seg.icmdLim));
        for (size_t i = 0; i < s_akey.size(); ++i) s_acmd[seg.icmdFirst + i] = s_acmdTmp[size_t(s_akey[i] & 0xFFFFFFFu)];
    }
    // Runs of equal state -> indirect commands.
    struct RUN { size_t icmd; uint32_t ideiFirst, cdei; };
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
        for (; j < s_acmd.size() && s_acmd[j].cmdk == CMDK_Pass && s_acmd[j].istate == cmd.istate; ++j)
        {
            const CMD& c = s_acmd[j];
            s_adei.push_back({c.count, 1, c.firstIndex, c.baseVertex, c.idraw});
            ++run.cdei;
        }
        s_arun.push_back(run);
        i = j;
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
    auto upload = [](GLenum target, GLuint buf, const void* pv, size_t cb) {
        glBindBuffer(target, buf);
        glBufferData(target, GLsizeiptr(std::max<size_t>(cb, 16)), nullptr, GL_STREAM_DRAW);   // orphan
        if (cb) glBufferSubData(target, 0, GLsizeiptr(cb), pv);
    };
    upload(GL_SHADER_STORAGE_BUFFER, s_ssboRec, s_arecgpu.data(), s_arecgpu.size() * sizeof(RECGPU));
    upload(GL_SHADER_STORAGE_BUFFER, s_ssboBone, s_amatBone.data(), s_amatBone.size() * sizeof(glm::mat4));
    upload(GL_SHADER_STORAGE_BUFFER, s_ssboDraw, s_adrawgpu.data(), s_adrawgpu.size() * sizeof(DRAWGPU));
    upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarp, s_awarpst.data(), s_awarpst.size() * sizeof(glm::vec4));
    upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarpMat, s_amatWarp.data(), s_amatWarp.size() * sizeof(glm::mat4));
    upload(GL_DRAW_INDIRECT_BUFFER, s_bufIndirect, s_adei.data(), s_adei.size() * sizeof(DEICMD));

    glBindFramebuffer(GL_FRAMEBUFFER, pv.fbo);
    glViewport(0, 0, pv.dx, pv.dy);
    const glm::vec4 clear = Vec4FromRgba(pv.rgbaClear, 1.0f / 255.0f);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(clear.r, clear.g, clear.b, float(pv.alphaClear) / 128.0f);
    glClearDepth(0.0);
    glStencilMask(0xFF);
    glClearStencil(0);
    // (the foreground pass: only the depth (0x2DF010's Z 0 over the frame) and the stencil, over the frame and HUD)
    glClear(pv.fForeground ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_CULL_FACE);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_ssboRec);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_ssboDraw);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_ssboBone);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, s_ssboWarp);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_ssboWarpMat);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_bufIndirect);
    glActiveTexture(GL_TEXTURE0);
    STATECACHE sc;
    bool fGlobProgram = false;
    auto useGlob = [&]() {
        if (fGlobProgram) return;
        glGlobShader.Use();
        glUniformMatrix4fv(s_u.matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
        glUniform3fv(s_u.posCamera, 1, glm::value_ptr(cam.pos));
        glUniform3fv(s_u.dirCamera, 1, glm::value_ptr(cam.Dir()));
        glUniform1i(s_u.tex, 0);
        glUniform4fv(s_u.fogColor, 1, glm::value_ptr(Vec4FromRgba(opts.rgbaFog, 1.0f / 255.0f)));
        fGlobProgram = true;
        sc.fValid = false;
    };
    static const bool s_fCelAlways = std::getenv("SLY2_CELALWAYS") != nullptr;
    for (const RUN& run : s_arun)
    {
        const CMD& cmd = s_acmd[run.icmd];
        switch (cmd.cmdk)
        {
        case CMDK_Pass:
            useGlob();
            sc.Apply(s_astate[cmd.istate]);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(run.ideiFirst) * sizeof(DEICMD)),
                                        GLsizei(run.cdei), 0);
            g_renderstats.cpass += int(run.cdei);
            ++g_renderstats.cbatch;
            break;
        case CMDK_Volume:
            useGlob();
            DrawProjVolume(cmd);
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Shadow:
            useGlob();
            DrawShadow(cmd);
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Shadow2:
            DrawShadow2(cmd, matWorldToClip);
            fGlobProgram = false;
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Particles:
            if (opts.fMeshes) DrawParticles(matWorldToClip, cam.pos, aspect);
            fGlobProgram = false;
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Cel:
        {
            const GLOB& geom = *cmd.prec->pglobGeom;
            glDisable(GL_CULL_FACE);
            glCelBorderShader.Use();
            fGlobProgram = false;
            sc.fValid = false;
            glUniformMatrix4fv(s_uc.matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
            glUniformMatrix4fv(s_uc.matModel, 1, GL_FALSE, glm::value_ptr(*cmd.prec->pmat));
            // Half a PS2 pixel (448 lines) in NDC, square pixels; the line is one PS2 pixel wide (AA1 line + plain).
            const float hy = 1.0f / 448.0f;
            glUniform2f(s_uc.halfWidthNdc, hy / aspect, hy);
            glUniform1i(s_uc.edgeBase, 0);
            glUniform1f(s_uc.depthBias, s_celBias);
            const glm::vec4 rgba = Vec4FromRgba(opts.rgbaCel, 1.0f / 255.0f);
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
    glBindVertexArray(0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
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
        RenderPass(scene, pv, opts);
    }
}
}  // namespace

void RenderScene(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts)
{
    g_renderstats.cdraw = g_renderstats.cpass = g_renderstats.ccel = 0;
    g_renderstats.cbatch = g_renderstats.cculled = g_renderstats.cmrd = 0;
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
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.rgbaClear = opts.rgbaClear;
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
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.fForeground = true;
    RenderPass(scene, pv, opts);
}
