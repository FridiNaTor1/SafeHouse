// The near-plane clip stage's startup check (nearclipcheck.h).
//
// The scene: a 512 x 512 render texture (RGBA8 + DEPTH32F_STENCIL8, as rtgt.cpp's speaker windows) cut into 16 x 16
// cells, one triangle a cell, in four draws of 64 triangles (untextured / textured, with / without fog). In each draw
// the triangles wholly in front of the near plane alternate with ones that cross it (one or two vertices behind the
// plane, some behind the camera): the miscompiled stage lost the triangles wholly in front wherever a triangle that
// crossed the plane shared their SIMD group. Each crossing triangle's clipped part stays in its cell (its vertices are
// the cell's centre ray times the depth plus an offset, scaled until the clipped part fits). The expected picture follows glob.geom: the polygon clipped against w - z >= 0 with the
// vertices on the plane linear in clip space, drawn as the strip a b [d] c, colours and fog affine in screen space within
// each strip triangle, UVs perspective-correct; then glob.frag. Only pixels well inside a polygon (1 pixel from its
// edges) and well outside all of them are compared, with a tolerance for the GPUs' interpolation and filtering.

#include "nearclipcheck.h"

#include "gl.h"
#include "renderer.h"
#include "platform.h"
#include "sly2_version.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{
constexpr int kDim = 512;          // the target's size
constexpr int kCells = 16;         // cells a side
constexpr int kDraws = 4;          // 0 untextured, 1 untextured + fog, 2 textured, 3 textured + fog
constexpr int kTrisPerDraw = kCells * kCells / kDraws;
constexpr float kNear = 1.0f;      // clip z (render.cpp: reversed Z, z = near); in front: w - z >= 0
constexpr int kTex = 16;           // the texture's size
constexpr int kTolerance = 10;     // per channel, of 255
constexpr float kFogColor[3] = {64.0f / 255.0f, 160.0f / 255.0f, 224.0f / 255.0f};

// glob.vert's inputs (render.cpp GPUVTX / GPUCOL, REC, DRAWGPU)
struct VTX { float pos[3]; float uv[2]; uint8_t rgbaA[4]; uint8_t rgbaB[4]; float normal[3]; uint8_t abone[4]; uint8_t aw[4]; };
static_assert(sizeof(VTX) == 48);
struct COL { uint8_t rgbaA[4]; uint8_t rgbaB[4]; };
struct REC { glm::mat4 model; glm::vec4 fog; glm::uvec4 bone; glm::vec4 spec0, spec1, specPos; glm::uvec4 warp; };
static_assert(sizeof(REC) == 160);

// One vertex as glob.geom sees it: clip position, colour (GS units), UV, fog
struct GV { glm::vec4 p; glm::vec4 c; glm::vec2 uv; float fog; };

glm::mat4 MatWorldToClip()
{
    // (x, y, d) -> (x, y, near, d)
    glm::mat4 m(0.0f);
    m[0][0] = 1.0f;
    m[1][1] = 1.0f;
    m[2][3] = 1.0f;
    m[3][2] = kNear;
    return m;
}

float FogOf(float w, bool fFog)
{
    // glob.vert with the check's fog record: near 1, far / near 4, max 0.6
    if (!fFog) return 0.0f;
    const float x = std::max(w / 1.0f, 1.0f);
    const float k = 1.0f - 1.0f / 4.0f;
    return std::clamp(0.6f * (1.0f - 1.0f / x) / k, 0.0f, 1.0f);
}

// glob.geom: the polygon (up to four vertices, the triangle's order)
int ClipPolygon(const GV (&v)[3], GV (&out)[4])
{
    float d[3];
    bool f[3];
    for (int i = 0; i < 3; ++i) d[i] = v[i].p.w - v[i].p.z, f[i] = d[i] >= 0.0f;
    int c = 0;
    auto mixv = [&](int i, int j, float t) {
        GV g;
        g.p = glm::mix(v[i].p, v[j].p, t);
        g.c = glm::mix(v[i].c, v[j].c, t);
        g.uv = glm::mix(v[i].uv, v[j].uv, t);
        g.fog = v[i].fog + (v[j].fog - v[i].fog) * t;
        return g;
    };
    for (int i = 0; i < 3; ++i)
    {
        const int j = (i + 1) % 3;
        if (f[i]) out[c++] = v[i];
        if (f[i] != f[j]) out[c++] = mixv(i, j, d[i] / (d[i] - d[j]));
    }
    return c;
}

// The texture: linear in u and v between its texel centres (bilinear filtering keeps that), so the expected sample is
// exact: R = 256 u, G = 256 v, B = 255 - 256 u, A = 60 + 128 v.
uint8_t TexelOf(int i, int j, int ch)
{
    switch (ch)
    {
    case 0: return uint8_t(16 * i + 8);
    case 1: return uint8_t(16 * j + 8);
    case 2: return uint8_t(247 - 16 * i);
    default: return uint8_t(64 + 8 * j);
    }
}
glm::vec4 TexOf(glm::vec2 uv)
{
    return glm::vec4(256.0f * uv.x, 256.0f * uv.y, 255.0f - 256.0f * uv.x, 60.0f + 128.0f * uv.y);
}

// glob.frag (no alpha test, no DATE, the uniforms' fog colour)
std::array<int, 4> FragmentOf(glm::vec4 color, glm::vec2 uv, float fog, bool fTextured)
{
    glm::vec4 c = color;
    if (fTextured) c = glm::floor(TexOf(uv) * color / 128.0f);
    c = glm::clamp(c, glm::vec4(0.0f), glm::vec4(255.0f));
    std::array<int, 4> out;
    for (int ch = 0; ch < 3; ++ch)
    {
        const float x = c[ch] / 255.0f;
        out[ch] = int(std::lround((x + (kFogColor[ch] - x) * fog) * 255.0f));
    }
    out[3] = int(std::lround(std::min(c.a / 128.0f, 1.0f) * 255.0f));
    return out;
}

struct LCG
{
    uint32_t s = 0x2545F491u;
    float Next() { s = s * 1664525u + 1013904223u; return float(s >> 8) / float(1u << 24); }   // [0, 1)
    float In(float lo, float hi) { return lo + (hi - lo) * Next(); }
};

struct SCENE
{
    std::vector<VTX> avtx;
    std::vector<uint32_t> aindex;   // three a triangle, in draw order
    std::vector<int> adrawOfTri;
    std::vector<bool> afCross;
};

SCENE BuildScene()
{
    SCENE sc;
    LCG r;
    const float cell = 2.0f / kCells;
    // the base shape (offsets from the cell's centre ray, NDC at depth 1)
    const glm::vec2 aoff[3] = {{-0.050f, -0.046f}, {0.052f, -0.030f}, {-0.004f, 0.052f}};
    for (int t = 0; t < kCells * kCells; ++t)
    {
        const int idraw = t / kTrisPerDraw;
        const int cx = t % kCells, cy = t / kCells;
        const glm::vec2 centre(-1.0f + cell * (float(cx) + 0.5f), -1.0f + cell * (float(cy) + 0.5f));
        const bool fCross = (t & 1) != 0;
        const bool fTextured = idraw >= 2;
        // which vertices are behind the plane (crossing ones): one (a quad) or two (a triangle); some behind the camera
        const int kind = (t >> 1) % 6;
        float ad[3];
        for (int i = 0; i < 3; ++i) ad[i] = fCross ? r.In(2.0f, 3.5f) : r.In(1.2f, 3.0f);
        if (fCross)
        {
            const int ib = kind % 3;
            ad[ib] = kind < 3 ? r.In(0.55f, 0.95f) : r.In(-0.8f, -0.2f);
            if (kind == 1 || kind == 5) ad[(ib + 1) % 3] = r.In(0.4f, 0.9f);
        }
        // (every other cell the shape turned half a turn: both windings)
        const float flip = (cx + cy) & 1 ? -1.0f : 1.0f;
        glm::vec2 ao[3];
        for (int i = 0; i < 3; ++i) ao[i] = flip * aoff[i] + glm::vec2(r.In(-0.006f, 0.006f), r.In(-0.006f, 0.006f));
        // In front: the offset in the picture. Crossing: the offset times the depth (3 times it behind the camera), so a
        // point of the clipped part at depth d projects to the centre plus sum(l_i o_i) / d, a convex mix of the shape's
        // offsets while every depth is positive; scaled down until the clipped part is inside the cell.
        glm::vec3 apos[3];
        for (float scale = 1.0f; scale > 0.05f; scale *= 0.85f)
        {
            for (int i = 0; i < 3; ++i)
                apos[i] = fCross ? glm::vec3(centre * ad[i] + scale * ao[i] * (ad[i] >= 0.0f ? ad[i] : 3.0f), ad[i])
                                 : glm::vec3((centre + ao[i]) * ad[i], ad[i]);
            if (!fCross) break;
            GV v[3], poly[4];
            for (int i = 0; i < 3; ++i) v[i] = GV{MatWorldToClip() * glm::vec4(apos[i], 1.0f), glm::vec4(0.0f), glm::vec2(0.0f), 0.0f};
            const int c = ClipPolygon(v, poly);
            bool fIn = true;
            const float lim = cell * 0.5f - 4.0f * 2.0f / kDim;   // (4 pixels from the cell's edge)
            for (int i = 0; i < c; ++i)
            {
                const glm::vec2 q = glm::vec2(poly[i].p) / poly[i].p.w - centre;
                fIn = fIn && std::fabs(q.x) <= lim && std::fabs(q.y) <= lim;
            }
            if (fIn) break;
        }
        for (int i = 0; i < 3; ++i)
        {
            VTX v{};
            v.pos[0] = apos[i].x, v.pos[1] = apos[i].y, v.pos[2] = apos[i].z;
            v.uv[0] = r.In(0.1f, 0.9f), v.uv[1] = r.In(0.1f, 0.9f);
            for (int ch = 0; ch < 3; ++ch) v.rgbaA[ch] = uint8_t(fTextured ? r.In(96.0f, 160.0f) : r.In(40.0f, 220.0f));
            v.rgbaA[3] = uint8_t(fTextured ? r.In(110.0f, 150.0f) : r.In(48.0f, 128.0f));
            std::memcpy(v.rgbaB, v.rgbaA, 4);
            v.normal[2] = 1.0f;
            v.aw[0] = 255;
            sc.aindex.push_back(uint32_t(sc.avtx.size()));
            sc.avtx.push_back(v);
        }
        sc.adrawOfTri.push_back(idraw);
        sc.afCross.push_back(fCross);
    }
    return sc;
}

// The expected picture: per pixel 0 the clear colour, 1 not compared, 2 aexp; and per pixel its triangle (-1 none)
struct EXPECTED
{
    std::vector<uint8_t> aclass;
    std::vector<std::array<int, 4>> aexp;
    std::vector<int> atri;
};

EXPECTED Expect(const SCENE& sc)
{
    EXPECTED e;
    e.aclass.assign(size_t(kDim) * kDim, 0);
    e.aexp.assign(size_t(kDim) * kDim, std::array<int, 4>{0, 0, 0, 0});
    e.atri.assign(size_t(kDim) * kDim, -1);
    const glm::mat4 m = MatWorldToClip();
    for (size_t t = 0; t < sc.adrawOfTri.size(); ++t)
    {
        const int idraw = sc.adrawOfTri[t];
        const bool fFog = idraw & 1, fTextured = idraw >= 2;
        GV v[3];
        for (int i = 0; i < 3; ++i)
        {
            const VTX& x = sc.avtx[sc.aindex[t * 3 + i]];
            v[i].p = m * glm::vec4(x.pos[0], x.pos[1], x.pos[2], 1.0f);
            v[i].c = glm::vec4(x.rgbaA[0], x.rgbaA[1], x.rgbaA[2], x.rgbaA[3]);
            v[i].uv = glm::vec2(x.uv[0], x.uv[1]);
            v[i].fog = FogOf(v[i].p.w, fFog);
        }
        GV poly[4];
        const int c = ClipPolygon(v, poly);
        if (c < 3) continue;
        glm::vec2 s[4];
        for (int i = 0; i < c; ++i)
            s[i] = glm::vec2((poly[i].p.x / poly[i].p.w * 0.5f + 0.5f) * kDim, (poly[i].p.y / poly[i].p.w * 0.5f + 0.5f) * kDim);
        // the polygon's signed area (its winding) for the inside test
        float area = 0.0f;
        for (int i = 0; i < c; ++i) { const glm::vec2 a = s[i], b = s[(i + 1) % c]; area += a.x * b.y - b.x * a.y; }
        if (std::fabs(area) < 1e-6f) continue;
        const float sgn = area > 0.0f ? 1.0f : -1.0f;
        // the strip's triangles: a b c, or a b d and b c d for a b c d (glob.geom emits a b d c)
        const int atri[2][3] = {{0, 1, c == 4 ? 3 : 2}, {1, 2, 3}};
        const int ctri = c == 4 ? 2 : 1;
        glm::vec2 lo = s[0], hi = s[0];
        for (int i = 1; i < c; ++i) lo = glm::min(lo, s[i]), hi = glm::max(hi, s[i]);
        const int x0 = std::max(0, int(std::floor(lo.x)) - 2), x1 = std::min(kDim - 1, int(std::ceil(hi.x)) + 2);
        const int y0 = std::max(0, int(std::floor(lo.y)) - 2), y1 = std::min(kDim - 1, int(std::ceil(hi.y)) + 2);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
                // distance to each edge, inside positive
                float dMin = 1e30f;
                for (int i = 0; i < c; ++i)
                {
                    const glm::vec2 a = s[i], b = s[(i + 1) % c];
                    const glm::vec2 ab = b - a;
                    const float len = glm::length(ab);
                    if (len < 1e-6f) continue;
                    dMin = std::min(dMin, sgn * (ab.x * (p.y - a.y) - ab.y * (p.x - a.x)) / len);
                }
                const size_t ip = size_t(y) * kDim + size_t(x);
                if (dMin <= -1.0f) continue;   // (well outside this one: as it was)
                if (dMin < 1.0f) { e.aclass[ip] = 1; continue; }
                // in the strip triangle whose barycentrics are all >= 0 (the best of the two near the diagonal)
                int best = 0;
                float bestMin = -1e30f;
                float alambda[2][3];
                for (int k = 0; k < ctri; ++k)
                {
                    const glm::vec2 a = s[atri[k][0]], b = s[atri[k][1]], q = s[atri[k][2]];
                    const float den = (b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y);
                    if (std::fabs(den) < 1e-9f) continue;
                    const float l1 = ((p.x - a.x) * (q.y - a.y) - (q.x - a.x) * (p.y - a.y)) / den;
                    const float l2 = ((b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y)) / den;
                    alambda[k][0] = 1.0f - l1 - l2, alambda[k][1] = l1, alambda[k][2] = l2;
                    const float mn = std::min({alambda[k][0], l1, l2});
                    if (mn > bestMin) bestMin = mn, best = k;
                }
                const float* l = alambda[best];
                const GV& A = poly[atri[best][0]];
                const GV& B = poly[atri[best][1]];
                const GV& C = poly[atri[best][2]];
                const glm::vec4 col = l[0] * A.c + l[1] * B.c + l[2] * C.c;
                const float fog = l[0] * A.fog + l[1] * B.fog + l[2] * C.fog;
                const float qa = l[0] / A.p.w, qb = l[1] / B.p.w, qc = l[2] / C.p.w;
                const glm::vec2 uv = (qa * A.uv + qb * B.uv + qc * C.uv) / (qa + qb + qc);
                e.aclass[ip] = 2;
                e.aexp[ip] = FragmentOf(col, uv, fog, fTextured);
                e.atri[ip] = int(t);
            }
    }
    return e;
}

struct GLOBJS
{
    GLuint fbo = 0, texColor = 0, rbo = 0, tex = 0, texArr = 0, sampler = 0, vao = 0;
    GLuint vbo = 0, vboColor = 0, vboDraw = 0, ebo = 0;
    GLuint ssboRec = 0, ssboDraw = 0, ssboBone = 0, ssboWarp = 0, ssboWarpMat = 0, ssboTex = 0, ssboMap = 0;
    GLuint64 handle = 0;
};

// (SLY2N_NEARCLIPDUMP=dir) a picture as a PNG, top row first
void DumpPng(const std::string& path, const std::vector<uint8_t>& apx)
{
    std::vector<uint8_t> aflip(apx.size());
    for (int y = 0; y < kDim; ++y) std::memcpy(&aflip[size_t(kDim - 1 - y) * kDim * 4], &apx[size_t(y) * kDim * 4], size_t(kDim) * 4);
    // (alpha forced opaque: the colours are what the comparison looks at first)
    for (size_t i = 3; i < aflip.size(); i += 4) aflip[i] = 255;
    stbi_write_png(path.c_str(), kDim, kDim, 4, aflip.data(), kDim * 4);
}

GLuint NewBuffer(GLenum target, const void* pv, size_t cb)
{
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBuffer(target, b);
    glBufferData(target, GLsizeiptr(cb), pv, GL_STATIC_DRAW);
    return b;
}
} // namespace

NEARCLIPCHECK NearClipCheck(const GLSHADER* const* apsh, int cpsh, bool fPull, bool fBindless, bool fArrays, bool fTexh4)
{
    NEARCLIPCHECK res;
    const SCENE sc = BuildScene();
    const EXPECTED exp = Expect(sc);
    static int s_ccall = 0;
    const int icall = s_ccall++;
    const char* szDump = std::getenv("SLY2N_NEARCLIPDUMP");
    if (szDump && *szDump && icall == 0)
    {
        std::vector<uint8_t> aexp(size_t(kDim) * kDim * 4, 0);
        for (size_t ip = 0; ip < size_t(kDim) * kDim; ++ip)
            for (int ch = 0; ch < 4; ++ch)
                aexp[ip * 4 + size_t(ch)] = exp.aclass[ip] == 1 ? (ch == 0 ? 255 : 0) : uint8_t(exp.aexp[ip][size_t(ch)]);
        DumpPng(std::string(szDump) + "/nearclip_expected.png", aexp);
    }
    while (glGetError() != GL_NO_ERROR) {}

    GLint aviewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, aviewport);
    GLfloat aclear[4] = {0, 0, 0, 0};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, aclear);
    GLfloat clearDepth = 1.0f;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);

    GLOBJS o;
    // the target: as rtgt.cpp's (RGBA8 texture, DEPTH32F_STENCIL8 renderbuffer)
    glGenTextures(1, &o.texColor);
    glBindTexture(GL_TEXTURE_2D, o.texColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kDim, kDim, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenRenderbuffers(1, &o.rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, o.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, kDim, kDim);
    glGenFramebuffers(1, &o.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o.texColor, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, o.rbo);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    // the texture, bound (unit 0), as a handle and as array layer 1 (the same texels each way)
    std::vector<uint8_t> atexel(size_t(kTex) * kTex * 4);
    for (int j = 0; j < kTex; ++j)
        for (int i = 0; i < kTex; ++i)
            for (int ch = 0; ch < 4; ++ch) atexel[(size_t(j) * kTex + size_t(i)) * 4 + size_t(ch)] = TexelOf(i, j, ch);
    glGenTextures(1, &o.tex);
    glBindTexture(GL_TEXTURE_2D, o.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTex, kTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, atexel.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenSamplers(1, &o.sampler);
    glSamplerParameteri(o.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(o.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(o.sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(o.sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (fBindless)
    {
        o.handle = glGetTextureSamplerHandleARB(o.tex, o.sampler);
        if (o.handle) glMakeTextureHandleResidentARB(o.handle);
    }
    if (fArrays)
    {
        glGenTextures(1, &o.texArr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, o.texArr);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kTex, kTex, 2);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, kTex, kTex, 1, GL_RGBA, GL_UNSIGNED_BYTE, atexel.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }

    // glob.vert's buffers: the records (1: fog), the draws, no bones or warps; the draws' texture entries
    REC arec[2] = {};
    for (REC& r : arec) r.model = glm::mat4(1.0f);
    arec[0].fog = glm::vec4(1.0f, 4.0f, 0.6f, 0.0f);
    arec[1].fog = glm::vec4(1.0f, 4.0f, 0.6f, 1.0f);
    glm::vec4 adraw[kDraws];
    for (int d = 0; d < kDraws; ++d)
    {
        const uint32_t irec = uint32_t(d & 1);
        float f;
        std::memcpy(&f, &irec, 4);
        adraw[d] = glm::vec4(0.0f, 0.0f, 1.0f, f);
    }
    std::vector<uint32_t> atexent(size_t(kDraws) * 4, 0u);
    for (int d = 2; d < kDraws; ++d)
    {
        if (fBindless && fTexh4) atexent[size_t(d) * 4] = uint32_t(o.handle), atexent[size_t(d) * 4 + 1] = uint32_t(o.handle >> 32);
        else if (fBindless) atexent[size_t(d) * 2] = uint32_t(o.handle), atexent[size_t(d) * 2 + 1] = uint32_t(o.handle >> 32);
        else if (fArrays) atexent[size_t(d) * 2] = 2u;   // (layer 1 + 1)
    }
    const glm::mat4 aident[8] = {glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f),
                                 glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};
    const glm::vec4 awarp[4] = {};
    const uint32_t amap[4] = {0u, 1u, 2u, 3u};
    o.ssboRec = NewBuffer(GL_SHADER_STORAGE_BUFFER, arec, sizeof(arec));
    o.ssboDraw = NewBuffer(GL_SHADER_STORAGE_BUFFER, adraw, sizeof(adraw));
    o.ssboBone = NewBuffer(GL_SHADER_STORAGE_BUFFER, aident, sizeof(aident));
    o.ssboWarp = NewBuffer(GL_SHADER_STORAGE_BUFFER, awarp, sizeof(awarp));
    o.ssboWarpMat = NewBuffer(GL_SHADER_STORAGE_BUFFER, aident, sizeof(aident));
    o.ssboTex = NewBuffer(GL_SHADER_STORAGE_BUFFER, atexent.data(), atexent.size() * 4);
    o.ssboMap = NewBuffer(GL_SHADER_STORAGE_BUFFER, amap, sizeof(amap));

    // the vertices: as attributes (render.cpp FlushArena's layout) or pulled (bindings 6 / 7, the index = the vertex)
    std::vector<COL> acol(sc.avtx.size());
    for (size_t i = 0; i < sc.avtx.size(); ++i) std::memcpy(acol[i].rgbaA, sc.avtx[i].rgbaA, 4), std::memcpy(acol[i].rgbaB, sc.avtx[i].rgbaB, 4);
    glGenVertexArrays(1, &o.vao);
    glBindVertexArray(o.vao);
    o.ebo = NewBuffer(GL_ELEMENT_ARRAY_BUFFER, sc.aindex.data(), sc.aindex.size() * 4);
    o.vbo = NewBuffer(GL_ARRAY_BUFFER, sc.avtx.data(), sc.avtx.size() * sizeof(VTX));
    o.vboColor = NewBuffer(GL_ARRAY_BUFFER, acol.data(), acol.size() * sizeof(COL));
    o.vboDraw = NewBuffer(GL_ARRAY_BUFFER, amap, sizeof(amap));
    if (!fPull)
    {
        glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, pos)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, uv)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, normal)));
        glEnableVertexAttribArray(5);
        glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, abone)));
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, aw)));
        glBindBuffer(GL_ARRAY_BUFFER, o.vboColor);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(COL), reinterpret_cast<void*>(offsetof(COL, rgbaA)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(COL), reinterpret_cast<void*>(offsetof(COL, rgbaB)));
        glBindBuffer(GL_ARRAY_BUFFER, o.vboDraw);
        glEnableVertexAttribArray(6);
        glVertexAttribIPointer(6, 1, GL_UNSIGNED_INT, 4, nullptr);
        glVertexAttribDivisor(6, 1);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, o.ssboRec);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, o.ssboDraw);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, o.ssboBone);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, o.ssboWarp);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, o.ssboWarpMat);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, o.ssboTex);
    if (fPull)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, o.vbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, o.vboColor);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, o.ssboMap);
    }
    for (GLuint unit = 0; unit < 3; ++unit)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, o.tex);
        if (unit == 2 && o.texArr) glBindTexture(GL_TEXTURE_2D_ARRAY, o.texArr);
        glBindSampler(unit, o.sampler);
    }
    glActiveTexture(GL_TEXTURE0);

    glViewport(0, 0, kDim, kDim);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(0.0f);

    const glm::mat4 matWorldToClip = MatWorldToClip();
    std::vector<uint8_t> apx(size_t(kDim) * kDim * 4);
    int cprog = 0;
    res.fOk = status == GL_FRAMEBUFFER_COMPLETE;
    std::string detail = status == GL_FRAMEBUFFER_COMPLETE ? "" : "target incomplete; ";
    for (int iprog = 0; iprog < cpsh && res.fOk; ++iprog)
    {
        const GLSHADER& sh = *apsh[iprog];
        if (!sh.ID) continue;
        ++cprog;
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        sh.Use();
        glUniformMatrix4fv(sh.Loc("matWorldToClip"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
        glUniform1i(sh.Loc("icolor"), 0);
        glUniform4f(sh.Loc("rgbaConst"), 128.0f, 128.0f, 128.0f, 128.0f);
        glUniform3f(sh.Loc("posCamera"), 0.0f, 0.0f, 0.0f);
        glUniform3f(sh.Loc("dirCamera"), 0.0f, 0.0f, 1.0f);
        glUniform1i(sh.Loc("tex"), 0);
        glUniform1i(sh.Loc("texDate"), 1);
        glUniform1i(sh.Loc("texArr"), 2);
        glUniform1i(sh.Loc("ate"), 0);
        glUniform1i(sh.Loc("atst"), 1);
        glUniform1f(sh.Loc("aref"), 0.0f);
        glUniform1i(sh.Loc("afailMode"), 0);
        glUniform4f(sh.Loc("fogColor"), kFogColor[0], kFogColor[1], kFogColor[2], 1.0f);
        glUniform1i(sh.Loc("fFogBlack"), 0);
        glUniform1i(sh.Loc("fDate"), 0);
        glUniform1i(sh.Loc("caneBlend1"), 0);
        const GLint locDrawBase = sh.Loc("caneDrawBase"), locVertexBase = sh.Loc("caneVertexBase");
        for (int d = 0; d < kDraws; ++d)
        {
            glUniform1i(sh.Loc("fTextured"), d >= 2 ? 1 : 0);
            const void* off = reinterpret_cast<const void*>(size_t(d) * kTrisPerDraw * 3 * 4);
            if (fPull)
            {
                glUniform1ui(locVertexBase, 0u);
                glUniform1ui(locDrawBase, uint32_t(d) | 0x80000000u);
                glDrawElements(GL_TRIANGLES, kTrisPerDraw * 3, GL_UNSIGNED_INT, off);
            }
            else
                glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, kTrisPerDraw * 3, GL_UNSIGNED_INT, off, 1, 0, GLuint(d));
        }
        glReadPixels(0, 0, kDim, kDim, GL_RGBA, GL_UNSIGNED_BYTE, apx.data());
        if (szDump && *szDump) DumpPng(std::string(szDump) + "/nearclip_" + std::to_string(icall) + "_" + std::to_string(iprog) + ".png", apx);
        // compare
        size_t cIn = 0, cInBad = 0, cOut = 0, cOutBad = 0;
        std::vector<int> ainTri(sc.adrawOfTri.size(), 0), abadTri(sc.adrawOfTri.size(), 0);
        for (size_t ip = 0; ip < size_t(kDim) * kDim; ++ip)
        {
            if (exp.aclass[ip] == 1) continue;
            const uint8_t* p = &apx[ip * 4];
            bool fBad = false;
            for (int ch = 0; ch < 4; ++ch) fBad = fBad || std::abs(int(p[ch]) - exp.aexp[ip][size_t(ch)]) > kTolerance;
            if (exp.aclass[ip] == 0) { ++cOut; cOutBad += fBad; continue; }
            ++cIn;
            cInBad += fBad;
            ++ainTri[size_t(exp.atri[ip])];
            abadTri[size_t(exp.atri[ip])] += fBad;
        }
        int cTriBad = 0, cTriBadFront = 0, cTriBadCross = 0, cInMin = 1 << 30;
        for (size_t t = 0; t < ainTri.size(); ++t)
        {
            cInMin = std::min(cInMin, ainTri[t]);
            if (ainTri[t] >= 8 && abadTri[t] * 5 > ainTri[t])
            {
                ++cTriBad;
                ++(sc.afCross[t] ? cTriBadCross : cTriBadFront);
            }
        }
        const bool fOk = cTriBad == 0 && cInBad * 50 <= cIn && cOutBad * 100 <= cOut && cIn > 0;
        char sz[256];
        std::snprintf(sz, sizeof sz, "%sprogram %d: %zu/%zu inside pixels off, %zu/%zu outside, triangles wrong %d in front + %d crossing (of %zu, min %d px)",
                      detail.empty() ? "" : "; ", iprog, cInBad, cIn, cOutBad, cOut, cTriBadFront, cTriBadCross, ainTri.size(), cInMin);
        detail += sz;
        res.fOk = res.fOk && fOk;
    }
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        char sz[64];
        std::snprintf(sz, sizeof sz, "; GL error 0x%04X", unsigned(err));
        detail += sz;
        res.fOk = false;
    }
    if (cprog == 0) res.fOk = false, detail += "no program";
    res.detail = detail;

    // GL's defaults back
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    for (GLuint unit = 0; unit < 3; ++unit)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glBindSampler(unit, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    for (GLuint b : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 10u}) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glViewport(aviewport[0], aviewport[1], aviewport[2], aviewport[3]);
    glClearColor(aclear[0], aclear[1], aclear[2], aclear[3]);
    glClearDepthf(clearDepth);
    if (o.handle) glMakeTextureHandleNonResidentARB(o.handle);
    const GLuint abuf[] = {o.vbo, o.vboColor, o.vboDraw, o.ebo, o.ssboRec, o.ssboDraw, o.ssboBone, o.ssboWarp, o.ssboWarpMat, o.ssboTex, o.ssboMap};
    glDeleteBuffers(GLsizei(std::size(abuf)), abuf);
    glDeleteVertexArrays(1, &o.vao);
    glDeleteSamplers(1, &o.sampler);
    const GLuint atex[] = {o.texColor, o.tex, o.texArr};
    glDeleteTextures(o.texArr ? 3 : 2, atex);
    glDeleteFramebuffers(1, &o.fbo);
    glDeleteRenderbuffers(1, &o.rbo);
    while (glGetError() != GL_NO_ERROR) {}
    return res;
}

namespace
{
std::filesystem::path CachePath() { return cane::userDirectory() / "nearclip-check.txt"; }
}

std::string NearClipKey(const std::string& variant)
{
    const char* szRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* szVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::string key = std::string(szRenderer ? szRenderer : "?") + "|" + (szVersion ? szVersion : "?") + "|" + SLY2_PORT_VERSION + "|" + variant;
    for (char& ch : key)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ';';
    return key;
}

int NearClipCached(const std::string& key)
{
    std::ifstream in(CachePath());
    std::string line;
    int result = -1;
    while (std::getline(in, line))
    {
        const size_t i = line.rfind('\t');
        if (i != std::string::npos && line.compare(0, i, key) == 0 && i == key.size()) result = line.compare(i + 1, std::string::npos, "pass") == 0 ? 1 : 0;
    }
    return result;
}

void NearClipRemember(const std::string& key, bool fOk)
{
    // (the other keys kept: a second renderer or GPU on this machine; the newest line for a key wins)
    std::vector<std::string> alines;
    {
        std::ifstream in(CachePath());
        std::string line;
        while (std::getline(in, line))
            if (!(line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '\t') && alines.size() < 32)
                alines.push_back(line);
    }
    alines.push_back(key + "\t" + (fOk ? "pass" : "fail"));
    std::ofstream out(CachePath(), std::ios::trunc);
    for (const std::string& l : alines) out << l << "\n";
}
