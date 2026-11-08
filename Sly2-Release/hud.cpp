// See hud.h.

#include "hud.h"
#include "texmod.h"
#include "region.h"

#include "display.h"
#include "promptglyph.h"
#include "gl.h"
#include "render.h"
#include "shd.h"
#include "qolview.h"

#include <stb/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include "clock.h"
extern int g_iframeHarness;
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>

CHud g_hud;

namespace
{
constexpr float GS_BIAS = 524288.0f;            // 2^19: the float's low 16 bits are GS 12.4
constexpr float GS_ORIGIN = 786432.0f;          // 0x49400000, GS 0
constexpr float FRAME_OFX = 2048.0f - 256.0f;   // the frame is 512 pixels wide (shown as 640)
constexpr float FRAME_OFY = 2048.0f - 112.0f;   // the 2D draws in field lines (224): one line = two frame lines
constexpr float UI_KX = 0.8f;                   // 0x3F4CCCCD
constexpr float UI_KY = 5.0f / 11.0f;           // 0x3EE8BA2F

glm::vec2 ScreenFromGs(float tx, float ty)
{
    // GS pixel = (t - 2^19) mod 4096 (the 12.4 register keeps 16 bits), then the frame offset.
    const float gx = std::fmod(tx - GS_BIAS, 4096.0f), gy = std::fmod(ty - GS_BIAS, 4096.0f);
    return glm::vec2((gx - FRAME_OFX) * 1.25f, (gy - FRAME_OFY) * 2.0f);
}

// (display.h fWideBackgrounds) a vertex on the 4:3 canvas's edge moved to the window's edge
glm::vec2 WidenToPicture(glm::vec2 p)
{
    static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_WIDEBG"); return sz && sz[0] == '0'; }();
    if (!g_display.fWideBackgrounds || s_fOff) return p;
    const glm::vec2 s = g_display.uiScale;
    constexpr float eps = 0.5f;
    // (only where the window reaches past the canvas: a 4:3 picture is unchanged)
    if (s.x < 1.0f)
    {
        if (p.x <= eps) p.x = 320.0f - 320.0f / s.x;
        else if (p.x >= 640.0f - eps) p.x = 320.0f + 320.0f / s.x;
    }
    if (s.y < 1.0f)
    {
        if (p.y <= eps) p.y = 224.0f - 224.0f / s.y;
        else if (p.y >= 448.0f - eps) p.y = 224.0f + 224.0f / s.y;
    }
    return p;
}

// (display.h fWideBackgrounds, the binocucom frames) the canvas range the picture covers past the 4:3 canvas, or
// false when the picture is not wider / taller than the canvas or the option is off
bool FWideExtent(glm::vec2& lo, glm::vec2& hi)
{
    static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_WIDEBG"); return sz && sz[0] == '0'; }();
    const glm::vec2 s = g_display.uiScale;
    if (!g_display.fWideBackgrounds || s_fOff || (s.x >= 1.0f && s.y >= 1.0f)) return false;
    lo = glm::vec2(320.0f - 320.0f / s.x, 224.0f - 224.0f / s.y);
    hi = glm::vec2(320.0f + 320.0f / s.x, 224.0f + 224.0f / s.y);
    return true;
}


glm::vec4 Rgba(uint32_t u) { return glm::vec4(float(u & 0xFF), float((u >> 8) & 0xFF), float((u >> 16) & 0xFF), float(u >> 24)); }

GLuint s_vao = 0, s_vbo = 0;
}  // namespace

void CHud::Begin()
{
    m_aquad.clear();
    m_fBreakBatch = false;
    m_atri.clear();
    m_aline.clear();
    m_abatch.clear();
    m_mode = 0;
    ctri = cline = csprite = 0;
}

void CHud::SetMode(int mode)
{
    CloseBatch();
    m_mode = mode;
}

void CHud::DrawSprite(const SELEM& elem, const HUDSTATE& st)
{
    if (elem.kind != SELEMK_Sprite) return;
    ++csprite;
    if (m_abatch.empty() || m_abatch.back().mode != m_mode || m_fBreakBatch)
        { m_abatch.push_back({m_mode, m_atri.size(), 0, m_aline.size(), 0}); m_fBreakBatch = false; }
    int cvtxLeft = st.cvtxReveal;
    for (const SPOLY& poly : elem.apoly)
    {
        // colour = clamp(rgba x scale + offset) (P_K21; 0x2160D8 for text)
        glm::vec4 c = glm::clamp(Rgba(poly.rgba) * st.scale + st.offset, glm::vec4(0.0f), glm::vec4(255.0f));
        c = glm::floor(c);
        const float z = (float(poly.control) * 100.0f + st.depth) / 16777216.0f;
        std::vector<VTX> av;
        for (size_t i = 0; i < poly.axy.size(); ++i)
        {
            const float x = poly.axy[i][0] / 16.0f, y = poly.axy[i][1] / 16.0f;
            const float tx = st.m[0] * x + st.m[4] * y + st.m[8];
            const float ty = st.m[1] * x + st.m[5] * y + st.m[9];
            av.push_back({ScreenFromGs(tx, ty), c, z});
        }
        // one polygon stays on one side of the GS's 4096-pixel wrap: a vertex wrapped away from the first one (a
        // ticker glyph just at the wrap, e.g. Bentley's long lines scrolling in the HUD prompt 0x2F1) is taken back
        // by the wrap (4096 GS pixels: 5120 canvas units across, 8192 down). Per vertex it made one glyph a polygon
        // 4096 pixels wide that the ticker's mask cut into full-width bands over the text (R23)
        for (size_t i = 1; i < av.size(); ++i)
        {
            const glm::vec2 d = av[i].pos - av[0].pos;
            if (d.x > 2560.0f) av[i].pos.x -= 5120.0f;
            else if (d.x < -2560.0f) av[i].pos.x += 5120.0f;
            if (d.y > 4096.0f) av[i].pos.y -= 8192.0f;
            else if (d.y < -4096.0f) av[i].pos.y += 8192.0f;
        }
        // (display.h fWideBackgrounds) a polygon spanning the whole canvas's width or height is a full-screen
        // background (the pause backdrop, the cut scenes' bars, the binocucom's surround): its edge vertices go to
        // the window's edges; pictures that only touch an edge keep the 4:3 canvas
        if (!av.empty())
        {
            glm::vec2 lo = av[0].pos, hi = av[0].pos;
            for (const VTX& v : av) { lo = glm::min(lo, v.pos); hi = glm::max(hi, v.pos); }
            constexpr float eps = 0.5f;
            const bool fSpan = (lo.x <= eps && hi.x >= 640.0f - eps) || (lo.y <= eps && hi.y >= 448.0f - eps);
            const bool fEdge = lo.x <= eps || hi.x >= 640.0f - eps || lo.y <= eps || hi.y >= 448.0f - eps;
            // (the binocucom frames keep their 4:3 shape and continue flat past the canvas instead
            // (ExtendEdgeTriangle / ExtendEdgeLine): moving their edge vertices stretched the frame's end segments)
            if (static const bool s_fSpanDbg = std::getenv("SLY2N_SPANDBG") != nullptr; s_fSpanDbg && fSpan)
            {
                std::printf("span poly prim %d cv %zu rgba %08X:", poly.primitive, av.size(), poly.rgba);
                for (const VTX& v : av) std::printf(" (%.1f %.1f)", v.pos.x, v.pos.y);
                std::printf("\n");
            }
            // (the wipe's Sly mask: its strips overshoot the canvas when it grows; only its rectangles (the black
            // backdrop) are widened, the mask keeps its shape)
            if (fSpan && !fWideEdgesObj && !(fWideRectsOnlyObj && av.size() > 5))
                for (VTX& v : av) v.pos = WidenToPicture(v.pos);
        }
        // a hair-thin bar (a 4-vertex strip less than one output pixel across, e.g. the recon view's crosshair, 0.3 canvas
        // units wide) still covers a pixel column / row, as the GS draws it at 512 x 448: widened to one output pixel
        if (poly.primitive == 2 && av.size() == 4 && g_gl.renderWidth > 0 && g_gl.renderHeight > 0)
        {
            glm::vec2 lo = av[0].pos, hi = av[0].pos;
            for (const VTX& v : av) { lo = glm::min(lo, v.pos); hi = glm::max(hi, v.pos); }
            const glm::vec2 px(640.0f / (float(g_gl.renderWidth) * g_display.uiScale.x), 448.0f / (float(g_gl.renderHeight) * g_display.uiScale.y));
            for (int k = 0; k < 2; ++k)
                if (hi[k] - lo[k] < px[k] && hi[1 - k] - lo[1 - k] > 2.0f * px[1 - k])
                {
                    const float c = 0.5f * (lo[k] + hi[k]);
                    for (VTX& v : av) v.pos[k] = v.pos[k] < c ? c - 0.5f * px[k] : c + 0.5f * px[k];
                }
        }
        if (static const bool s_fPos = std::getenv("SLY2N_HUDPOS") != nullptr; s_fPos && !av.empty())
        {
            glm::vec2 lo = av[0].pos, hi = av[0].pos;
            for (const VTX& v : av) { lo = glm::min(lo, v.pos); hi = glm::max(hi, v.pos); }
            if (hi.x < 200.0f && hi.y < 100.0f)
                std::printf("hudpos rgba %08X c %.0f %.0f %.0f %.0f x %.2f..%.2f y %.2f..%.2f (gs x %.2f..%.2f y %.2f..%.2f)\n", poly.rgba, c.r, c.g, c.b, c.a, lo.x, hi.x, lo.y, hi.y,
                            lo.x / 1.25f, hi.x / 1.25f, lo.y / 2.0f, hi.y / 2.0f);
        }
        if (static const bool s_fEdgeDbg = std::getenv("SLY2N_HUDEDGEDBG") != nullptr; s_fEdgeDbg && fWideEdgesObj && !av.empty())
        {
            glm::vec2 lo = av[0].pos, hi = av[0].pos;
            for (const VTX& v : av) { lo = glm::min(lo, v.pos); hi = glm::max(hi, v.pos); }
            if (lo.x <= 0.5f || hi.x >= 639.5f)
            {
                std::printf("edge poly prim %d vb %d cv %zu rgba %08X:", poly.primitive, poly.fVertexBytes, av.size(), poly.rgba);
                for (size_t i = 0; i < av.size(); ++i) std::printf(" (%.1f %.1f%s)", av[i].pos.x, av[i].pos.y, poly.fVertexBytes && (poly.ab[i] & 0x80) ? "*" : "");
                std::printf("\n");
            }
        }
        if (static const bool s_fCenterDbg = std::getenv("SLY2_HUDCENTERDBG") != nullptr; s_fCenterDbg && fWideEdgesObj && !av.empty() && glm::distance(av[0].pos, glm::vec2(320.0f, 224.0f)) < 40.0f)
        {
            std::printf("centre poly prim %d ctl %d vb %d cv %zu rgba %08X:", poly.primitive, poly.control, poly.fVertexBytes, av.size(), poly.rgba);
            for (size_t i = 0; i < av.size(); ++i) std::printf(" (%.1f %.1f b%02X)", av[i].pos.x, av[i].pos.y, poly.ab[i]);
            std::printf("\n");
        }
        const size_t n = std::min<size_t>(av.size(), size_t(std::max(cvtxLeft, 0)));
        cvtxLeft -= int(av.size());
        for (size_t i = 0; i < n; ++i)
        {
            // the vertex byte's bit 7: no kick (the vertex still advances the fan / strip)
            if (poly.fVertexBytes && (poly.ab[i] & 0x80)) continue;
            if (poly.primitive == 3)
            {
                if (i)
                {
                    VTX a = av[i - 1], b = av[i];
                    if (fClipCanvasObj && !FClipLineToCanvas(a, b)) continue;
                    // (a binocucom frame's segment wholly past the canvas, e.g. Murray's surround outline at x -41 /
                    // 683: never on screen in the original; on a wide picture it showed as a seam in the surround)
                    static const bool s_fSeamOff = [] { const char* sz = std::getenv("SLY2N_BINOCSEAMFIX"); return sz && sz[0] == '0'; }();
                    if (fWideEdgesObj && !s_fSeamOff && g_display.uiScale.x < 1.0f &&
                        ((a.pos.x < -2.0f && b.pos.x < -2.0f) || (a.pos.x > 642.0f && b.pos.x > 642.0f)))
                        continue;
                    m_aline.push_back(a); m_aline.push_back(b); ++cline;
                }
                if (fWideEdgesObj) ExtendEdgeLine(av[i]);
            }
            else if (i >= 2)
            {
                const VTX& va = poly.primitive == 1 ? av[0] : av[i - 2];
                if (fClipCanvasObj && FClipTriangleToCanvas(va, av[i - 1], av[i])) continue;
                m_atri.push_back(va);
                m_atri.push_back(av[i - 1]);
                m_atri.push_back(av[i]);
                ++ctri;
                if (fWideEdgesObj) ExtendEdgeTriangle(m_atri[m_atri.size() - 3], m_atri[m_atri.size() - 2], m_atri[m_atri.size() - 1]);
            }
        }
    }
}

// (QoL, display.h fWideBackgrounds) the binocucom frames (0x35F..0x361) on a picture wider (or taller) than 4:3: the
// frame keeps its 4:3 shape and place (the talking-head windows stay with the subtitle box, as in the original);
// a triangle with an edge on the canvas's border continues flat to the picture's border (a quad of that edge's two
// vertices moved out across), so the bands, the surround and the scan lines run on straight at any width. A 4:3
// picture is unchanged. (This replaces the earlier edge-vertex move, which stretched the frame's end segments into
// long tapers.)
void CHud::ExtendEdgeTriangle(VTX a, VTX b, VTX c)
{
    glm::vec2 lo, hi;
    if (!FWideExtent(lo, hi)) return;
    // (the frames overshoot the canvas a little (to -9.7 / 652) or stop just short of it (1.6 / 639.5))
    constexpr float tol = 2.0f;
    const VTX av[3] = {a, b, c};
    for (int k = 0; k < 2; ++k)   // x, then y
    {
        if (k == 0 ? lo.x >= 0.0f : lo.y >= 0.0f) continue;
        const float edge1 = k == 0 ? 640.0f : 448.0f;
        for (int side = 0; side < 2; ++side)
        {
            const float to = side ? hi[k] : lo[k];
            int ai[3], n = 0;
            for (int i = 0; i < 3; ++i)
                if (side ? av[i].pos[k] >= edge1 - tol : av[i].pos[k] <= tol) ai[n++] = i;
            if (n < 2) continue;
            // (all three at the edge: the pair that spans the most across)
            int i0 = ai[0], i1 = ai[1];
            if (n == 3)
            {
                float best = -1.0f;
                for (int u = 0; u < 3; ++u)
                    for (int v = u + 1; v < 3; ++v)
                        if (const float d = std::fabs(av[u].pos[1 - k] - av[v].pos[1 - k]); d > best) { best = d; i0 = u; i1 = v; }
            }
            const VTX p = av[i0], q = av[i1];
            if (std::fabs(p.pos[1 - k] - q.pos[1 - k]) <= 1e-3f) continue;
            VTX p2 = p, q2 = q;
            p2.pos[k] = to;
            q2.pos[k] = to;
            m_atri.insert(m_atri.end(), {p, q, p2, p2, q, q2});
            ctri += 2;
        }
    }
}

// (QoL, the episode menu on a picture wider than 4:3) a triangle reaching past the 4:3 canvas's left / right border
// is cut at it (its pictures overshoot the canvas unevenly: the original only ever shows the 4:3 part). Returns true
// when it handled the triangle (pushed its clipped pieces, or nothing); false: inside, the caller pushes it.
bool CHud::FClipTriangleToCanvas(const VTX& a, const VTX& b, const VTX& c)
{
    if (g_display.uiScale.x >= 1.0f) return false;
    auto inside = [](const VTX& v, int side) { return side ? v.pos.x <= 640.0f : v.pos.x >= 0.0f; };
    if (inside(a, 0) && inside(b, 0) && inside(c, 0) && inside(a, 1) && inside(b, 1) && inside(c, 1)) return false;
    std::vector<VTX> apoly = {a, b, c}, anext;
    for (int side = 0; side < 2 && !apoly.empty(); ++side)
    {
        const float xEdge = side ? 640.0f : 0.0f;
        anext.clear();
        for (size_t i = 0; i < apoly.size(); ++i)
        {
            const VTX& p = apoly[i];
            const VTX& q = apoly[(i + 1) % apoly.size()];
            const bool fp = inside(p, side), fq = inside(q, side);
            if (fp) anext.push_back(p);
            if (fp != fq)
            {
                const float u = (xEdge - p.pos.x) / (q.pos.x - p.pos.x);
                VTX r = p;
                r.pos = glm::mix(p.pos, q.pos, u);
                r.pos.x = xEdge;
                r.rgba = glm::mix(p.rgba, q.rgba, u);
                r.z = p.z + (q.z - p.z) * u;
                r.uv = glm::mix(p.uv, q.uv, u);
                anext.push_back(r);
            }
        }
        apoly.swap(anext);
    }
    for (size_t i = 2; i < apoly.size(); ++i)
    {
        m_atri.push_back(apoly[0]);
        m_atri.push_back(apoly[i - 1]);
        m_atri.push_back(apoly[i]);
        ++ctri;
    }
    return true;
}

// (the episode menu's lines, as FClipTriangleToCanvas) false: wholly outside
bool CHud::FClipLineToCanvas(VTX& a, VTX& b)
{
    if (g_display.uiScale.x >= 1.0f) return true;
    for (int side = 0; side < 2; ++side)
    {
        const float xEdge = side ? 640.0f : 0.0f;
        auto inside = [&](const VTX& v) { return side ? v.pos.x <= xEdge : v.pos.x >= xEdge; };
        const bool fa = inside(a), fb = inside(b);
        if (!fa && !fb) return false;
        if (fa != fb)
        {
            VTX& o = fa ? b : a;
            const VTX& in = fa ? a : b;
            const float u = (xEdge - in.pos.x) / (o.pos.x - in.pos.x);
            o.pos = glm::mix(in.pos, o.pos, u);
            o.pos.x = xEdge;
            o.rgba = glm::mix(in.rgba, o.rgba, u);
        }
    }
    return true;
}

// The same for a line's vertex on the canvas's border: a straight segment out to the picture's border.
void CHud::ExtendEdgeLine(VTX v)
{
    glm::vec2 lo, hi;
    if (!FWideExtent(lo, hi)) return;
    constexpr float tol = 2.0f;
    for (int k = 0; k < 2; ++k)
    {
        if (k == 0 ? lo.x >= 0.0f : lo.y >= 0.0f) continue;
        const float edge1 = k == 0 ? 640.0f : 448.0f;
        VTX w = v;
        if (v.pos[k] <= tol) w.pos[k] = lo[k];
        else if (v.pos[k] >= edge1 - tol) w.pos[k] = hi[k];
        else continue;
        m_aline.push_back(v);
        m_aline.push_back(w);
        ++cline;
    }
}

// orig: 0x002177E0 (kind 7: the piece's texture (0x2155C0: TEX0 of the shader draw record with the element's
// id, 0x1EC880; TEX1 0x60, CLAMP 5) and the element's GIF sprite through P_K21 0x650: STQ (0,1) (1,1) (0,0) (1,0)
// at the rect's corners, RGBA 0x80808080, ALPHA 0x44)
void CHud::DrawQuad(const SELEM& elem, const HUDSTATE& st)
{
    if (elem.kind != SELEMK_Quad) return;
    const SHD* pshd = nullptr;
    for (const auto& p : g_shds.ashd)
        if (p->id == elem.idTexture && (p->grfshd & 0x40)) { pshd = p.get(); break; }
    if (!pshd) return;
    if ([] { static const bool f = std::getenv("SLY2_QUADDBG") != nullptr; return f; }())
    {
        const TEXSEL sel = TexselFromShd(pshd, 0, 0);
        for (int tp = 0; tp < 2; ++tp)
        {
            const TEXSEL s2 = TexselFromShd(pshd, 0, tp);
            std::vector<uint8_t> rgba; std::string err;
            if (FDecodeTexture(s2.pbmp, s2.pclut, rgba, err))
            {
                long sum[4] = {};
                for (size_t k = 0; k < rgba.size(); ++k) sum[k % 4] += rgba[k];
                const long n = long(rgba.size() / 4);
                std::printf("  pass %d mean rgba %ld %ld %ld %ld\n", tp, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
                // (SLY2N_QUADDUMP=dir: the decoded pass as dir/quad_<id>_<pass>.png)
                if (const char* szDir = std::getenv("SLY2N_QUADDUMP"); szDir && s2.pbmp)
                    stbi_write_png((std::string(szDir) + "/quad_" + std::to_string(pshd->id) + "_" + std::to_string(tp) + ".png").c_str(),
                                   s2.pbmp->dx, s2.pbmp->dy, 4, rgba.data(), s2.pbmp->dx * 4);
            }
            else std::printf("  pass %d: %s\n", tp, err.c_str());
        }
        std::printf("quad shd 0x%X kind %d tex %zu bmp %p %dx%d psm 0x%X texk %d clut %p n %d\n", pshd->id, pshd->shdk, pshd->atex.size(),
                    (void*)sel.pbmp, sel.pbmp ? sel.pbmp->dx : 0, sel.pbmp ? sel.pbmp->dy : 0, sel.pbmp ? sel.pbmp->psm : 0,
                    sel.pbmp ? sel.pbmp->texk : 0, (void*)sel.pclut, sel.pclut ? sel.pclut->ccolor : 0);
    }
    QUAD q;
    q.tex = GlTextureOfShd(pshd, 0);
    // the sprite's own RGBA (0x80808080: the texture as is); the draw state's alpha fades it
    const glm::vec4 c(128.0f, 128.0f, 128.0f, 128.0f * std::clamp(st.scale.a, 0.0f, 1.0f));
    if ([] { static const bool f = std::getenv("SLY2_QUADDBG") != nullptr; return f; }()) std::printf("  state offset %.1f %.1f %.1f %.1f scale %.2f %.2f %.2f %.2f\n", st.offset.r, st.offset.g, st.offset.b, st.offset.a, st.scale.r, st.scale.g, st.scale.b, st.scale.a);
    const float z = st.depth / 16777216.0f;
    auto pt = [&](float x, float y) {
        x /= 16.0f; y /= 16.0f;
        return ScreenFromGs(st.m[0] * x + st.m[4] * y + st.m[8], st.m[1] * x + st.m[5] * y + st.m[9]);
    };
    glm::vec2 p00 = pt(elem.x0, elem.y0), p10 = pt(elem.x1, elem.y0), p01 = pt(elem.x0, elem.y1), p11 = pt(elem.x1, elem.y1);
    // (QoL, qolview.h) the front end's logo: the launcher's high-quality one over the old one's body
    {
        // (the quad's displayed aspect: a canvas unit is 1 wide by 640 / 448 x 3 / 4 tall)
        const float dxq = glm::length(p10 - p00), dyq = glm::length(p01 - p00) * (640.0f / 448.0f) * (3.0f / 4.0f);
        float arect[4] = {dyq > 0.0f ? dxq / dyq : 1.0f, 0, 0, 0};
        if (const unsigned texHq = GlTexLogoOverride(pshd, arect))
        {
            auto at = [&](float u, float v) { return glm::mix(glm::mix(p00, p10, u), glm::mix(p01, p11, u), v); };
            const glm::vec2 a = at(arect[0], arect[1]), b = at(arect[2], arect[1]), d = at(arect[0], arect[3]), e = at(arect[2], arect[3]);
            p00 = a; p10 = b; p01 = d; p11 = e;
            q.tex = texHq;
            q.fMip = true;
        }
    }
    q.av = {{p00, c, z, {0, 1}}, {p10, c, z, {1, 1}}, {p01, c, z, {0, 0}}, {p01, c, z, {0, 0}}, {p10, c, z, {1, 1}}, {p11, c, z, {1, 0}}};
    if ([] { static const bool f = std::getenv("SLY2_QUADDBG") != nullptr; return f; }())
        std::printf("  quad 0x%X at (%.1f %.1f) (%.1f %.1f) (%.1f %.1f) (%.1f %.1f) elem %d %d %d %d\n", pshd->id, p00.x, p00.y, p10.x, p10.y, p01.x, p01.y,
                    p11.x, p11.y, elem.x0, elem.y0, elem.x1, elem.y1);
    q.mode = m_mode;
    CloseBatch();
    q.ibatch = m_abatch.size();   // drawn before the batches that follow it (the original's draw order)
    m_fBreakBatch = true;
    m_aquad.push_back(q);
}

void CHud::DrawTexQuad(unsigned tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                       glm::vec4 rgba, float z, bool fOnce)
{
    if (!tex) return;
    QUAD q;
    q.tex = tex;
    // (the shader: texel x 255 x colour / 128, alpha / 128: a straight-alpha texel of 1 at colour 0x80 and alpha 0x40)
    const glm::vec4 c(rgba.r, rgba.g, rgba.b, rgba.a * 0.5f);
    const glm::vec2 p00(x0, y0), p10(x1, y0), p01(x0, y1), p11(x1, y1);
    q.av = {{p00, c, z, {u0, v1}}, {p10, c, z, {u1, v1}}, {p01, c, z, {u0, v0}}, {p01, c, z, {u0, v0}}, {p10, c, z, {u1, v1}}, {p11, c, z, {u1, v0}}};
    if (fOnce)
    {
        // the same icon at the same size within a third of its width: the later copy (drawn on top) is the one kept
        const float w = x1 - x0, h = y1 - y0;
        for (QUAD& o : m_aquad)
        {
            if (!o.fOnce || o.tex != tex || o.av.size() != 6 || o.av[0].uv != q.av[0].uv || o.av[5].uv != q.av[5].uv) continue;
            const glm::vec2 d = o.av[5].pos - o.av[0].pos;
            if (std::abs(d.x - w) > 0.01f * std::abs(w) || std::abs(d.y - h) > 0.01f * std::abs(h)) continue;
            if (glm::length(o.av[0].pos - p00) > std::abs(w) / 3.0f) continue;
            o.av = q.av;
            return;
        }
        q.fOnce = true;
    }
    q.mode = m_mode;
    q.fMip = true;
    q.fAlphaTest = true;  // Atlas padding must not write depth or stencil over adjacent artwork.
    CloseBatch();
    q.ibatch = m_abatch.size();
    m_fBreakBatch = true;
    m_aquad.push_back(q);
}

void CHud::DrawIconGlyph(const FONTBRX& icons, char ch, float cx, float cy, float h, float depth)
{
    const SELEM* pglyph = icons.PglyphFromCh(uint8_t(ch));
    if (!pglyph) return;
    HUDSTATE st;
    st.depth = depth;
    st.offset = glm::vec4(0.0f);
    st.m[0] = UI_KX;
    st.m[5] = UI_KY;
    st.m[8] = 2048.0f + GS_ORIGIN;
    st.m[9] = 2048.0f + GS_ORIGIN;
    glm::vec2 lo, hi;
    float z, a;
    if (!FGlyphBox(*pglyph, st, &lo, &hi, &z, &a) || hi.y - lo.y <= 0.0f) return;
    const float k = h / (hi.y - lo.y);
    st.m[0] *= k;
    st.m[5] *= k;
    FGlyphBox(*pglyph, st, &lo, &hi, &z, &a);
    // (screen pixels to GS units: x / 1.25, y / 2)
    st.m[8] += (cx - 0.5f * (lo.x + hi.x)) / 1.25f;
    st.m[9] += (cy - 0.5f * (lo.y + hi.y)) / 2.0f;
    const PROMPTGLYPH pg = PromptGlyphFor(ch);
    if (pg.kind == PROMPTGLYPH::Icon)
    {
        const float hx = 0.5f * h * (480.0f / 448.0f);
        DrawTexQuad(pg.tex, cx - hx, cy - 0.5f * h, cx + hx, cy + 0.5f * h, pg.u0, pg.v0, pg.u1, pg.v1,
                    glm::vec4(128.0f, 128.0f, 128.0f, a), z);
    }
    else if (pg.kind == PROMPTGLYPH::Glyph && icons.PglyphFromCh(uint8_t(pg.ch))) DrawSprite(*icons.PglyphFromCh(uint8_t(pg.ch)), st);
    else DrawSprite(*pglyph, st);
}

void CHud::DrawRing(float cx, float cy, float rOuter, float rInner, float u, glm::vec4 rgba, float z)
{
    u = std::clamp(u, 0.0f, 1.0f);
    if (u <= 0.0f) return;
    if (m_abatch.empty() || m_abatch.back().mode != m_mode || m_fBreakBatch)
        { m_abatch.push_back({m_mode, m_atri.size(), 0, m_aline.size(), 0}); m_fBreakBatch = false; }
    const float kx = 448.0f / 480.0f;   // (a canvas pixel is 480 / 448 as wide as high)
    const int cseg = std::max(2, int(std::ceil(64.0f * u)));
    auto pt = [&](float a, float r) { return glm::vec2(cx + std::sin(a) * r * kx, cy - std::cos(a) * r); };
    for (int i = 0; i < cseg; ++i)
    {
        const float a0 = 6.2831853f * u * float(i) / float(cseg), a1 = 6.2831853f * u * float(i + 1) / float(cseg);
        const VTX o0{pt(a0, rOuter), rgba, z}, o1{pt(a1, rOuter), rgba, z}, i0{pt(a0, rInner), rgba, z}, i1{pt(a1, rInner), rgba, z};
        m_atri.insert(m_atri.end(), {o0, o1, i0, i0, o1, i1});
        ctri += 2;
    }
}

bool CHud::FGlyphBox(const SELEM& glyph, const HUDSTATE& st, glm::vec2* plo, glm::vec2* phi, float* pz, float* pa)
{
    bool f = false;
    int controlMax = 0;
    float aMax = 0.0f;
    for (const SPOLY& poly : glyph.apoly)
    {
        controlMax = std::max(controlMax, int(poly.control));
        const glm::vec4 c = glm::clamp(Rgba(poly.rgba) * st.scale + st.offset, glm::vec4(0.0f), glm::vec4(255.0f));
        aMax = std::max(aMax, c.a);
        for (const auto& v : poly.axy)
        {
            const float x = v[0] / 16.0f, y = v[1] / 16.0f;
            const glm::vec2 p = ScreenFromGs(st.m[0] * x + st.m[4] * y + st.m[8], st.m[1] * x + st.m[5] * y + st.m[9]);
            if (!f) { *plo = *phi = p; f = true; }
            *plo = glm::min(*plo, p);
            *phi = glm::max(*phi, p);
        }
    }
    *pz = (float(controlMax) * 100.0f + st.depth) / 16777216.0f;
    *pa = aMax;
    return f;
}

void CHud::DrawRtgtQuad(const SELEM& elem, const HUDSTATE& st, unsigned tex)
{
    if ([] { static const bool f = std::getenv("SLY2_QUADDBG") != nullptr; return f; }()) std::printf("rtgt quad kind %d tex %u depth %.0f rect %d %d %d %d m %.2f %.2f %.1f %.1f\n", elem.kind, tex, st.depth, elem.x0, elem.y0, elem.x1, elem.y1, st.m[0], st.m[5], st.m[8], st.m[9]);
    if (elem.kind != SELEMK_Image || !tex) return;
    QUAD q;
    q.tex = tex;
    q.fReplace = true;
    const glm::vec4 c(128.0f, 128.0f, 128.0f, 128.0f);
    const float z = st.depth / 16777216.0f;
    auto pt = [&](float x, float y) {
        x /= 16.0f; y /= 16.0f;
        return ScreenFromGs(st.m[0] * x + st.m[4] * y + st.m[8], st.m[1] * x + st.m[5] * y + st.m[9]);
    };
    // STQ (0, 0) at the top left: the target's first row is its top; a GL target's top row is t = 1
    const glm::vec2 p00 = pt(elem.x0, elem.y0), p10 = pt(elem.x1, elem.y0), p01 = pt(elem.x0, elem.y1), p11 = pt(elem.x1, elem.y1);
    q.av = {{p00, c, z, {0, 1}}, {p10, c, z, {1, 1}}, {p01, c, z, {0, 0}}, {p01, c, z, {0, 0}}, {p10, c, z, {1, 1}}, {p11, c, z, {1, 0}}};
    q.mode = m_mode;
    CloseBatch();
    q.ibatch = m_abatch.size();   // drawn before the batches that follow it (the original's draw order)
    m_fBreakBatch = true;
    m_aquad.push_back(q);
}

void CHud::FillScreenRect(float x0, float y0, float x1, float y1, glm::vec4 rgba, float zRect)
{
    if (m_abatch.empty() || m_abatch.back().mode != m_mode || m_fBreakBatch)
        { m_abatch.push_back({m_mode, m_atri.size(), 0, m_aline.size(), 0}); m_fBreakBatch = false; }
    const float z = zRect;   // (1: on top)
    const VTX a{{x0, y0}, rgba, z}, b{{x1, y0}, rgba, z}, c{{x0, y1}, rgba, z}, d{{x1, y1}, rgba, z};
    m_atri.insert(m_atri.end(), {a, b, c, c, b, d});
    ctri += 2;
}

void CHud::SpritePolys(const SELEM& elem, const HUDSTATE& st, std::vector<SPRITEPOLY>& apoly)
{
    apoly.clear();
    if (elem.kind != SELEMK_Sprite) return;
    for (const SPOLY& poly : elem.apoly)
    {
        SPRITEPOLY sp;
        sp.primitive = poly.primitive;
        sp.rgba = glm::floor(glm::clamp(Rgba(poly.rgba) * st.scale + st.offset, glm::vec4(0.0f), glm::vec4(255.0f)));
        sp.z = (float(poly.control) * 100.0f + st.depth) / 16777216.0f;
        for (const auto& v : poly.axy)
        {
            const float x = v[0] / 16.0f, y = v[1] / 16.0f;
            sp.apos.push_back(ScreenFromGs(st.m[0] * x + st.m[4] * y + st.m[8], st.m[1] * x + st.m[5] * y + st.m[9]));
        }
        apoly.push_back(std::move(sp));
    }
}

glm::vec2 CHud::ScreenOfLocal(const HUDSTATE& st, glm::vec2 p)
{
    return ScreenFromGs(st.m[0] * p.x + st.m[4] * p.y + st.m[8], st.m[1] * p.x + st.m[5] * p.y + st.m[9]);
}

void CHud::FillScreenLine(glm::vec2 a, glm::vec2 b, float w, glm::vec4 rgba, float z)
{
    // (in square units: a canvas pixel is 448 / 480 as wide as high, 640 x 448 shown at 4:3)
    const float kx = 480.0f / 448.0f;
    glm::vec2 d((b.x - a.x) / kx, b.y - a.y);
    const float len = glm::length(d);
    if (len <= 1e-4f) return;
    glm::vec2 n(-d.y / len * 0.5f * w, d.x / len * 0.5f * w);
    n.x *= kx;
    const glm::vec2 apos[6] = {a + n, b + n, a - n, a - n, b + n, b - n};
    FillScreenTris(apos, 6, rgba, z);
}

void CHud::FillScreenTris(const glm::vec2* apos, int cpos, glm::vec4 rgba, float z)
{
    if (m_abatch.empty() || m_abatch.back().mode != m_mode || m_fBreakBatch)
        { m_abatch.push_back({m_mode, m_atri.size(), 0, m_aline.size(), 0}); m_fBreakBatch = false; }
    for (int i = 0; i + 2 < cpos; i += 3)
    {
        m_atri.insert(m_atri.end(), {VTX{apos[i], rgba, z}, VTX{apos[i + 1], rgba, z}, VTX{apos[i + 2], rgba, z}});
        ++ctri;
    }
}

// The canvas x / y range the whole picture covers (wider than 0..640 when the canvas is pillarboxed): screen-wide
// primitives span it.
namespace
{
void PictureExtent(float& x0, float& x1, float& y0, float& y1)
{
    const glm::vec2 s = g_display.uiScale;
    x0 = 320.0f - 320.0f / s.x; x1 = 320.0f + 320.0f / s.x;
    y0 = 224.0f - 224.0f / s.y; y1 = 224.0f + 224.0f / s.y;
}
}

void CHud::Pillarbox(glm::vec4 rgba, float z)
{
    float x0, x1, y0, y1;
    PictureExtent(x0, x1, y0, y1);
    if (x0 < 0.0f) FillScreenRect(x0, y0, 0.0f, y1, rgba, z);
    if (x1 > 640.0f) FillScreenRect(640.0f, y0, x1, y1, rgba, z);
}

void CHud::Fade(glm::vec4 rgba)
{
    float x0, x1, y0, y1;
    PictureExtent(x0, x1, y0, y1);
    if (static const bool s_f = std::getenv("SLY2N_FADEPRINT") != nullptr; s_f)
        std::printf("fade %.0f %.0f %.0f %.0f\n", rgba.r, rgba.g, rgba.b, rgba.a);
    FillScreenRect(x0, y0, x1, y1, rgba);
}

void CHud::Letterbox(float u, float hBar, glm::vec4 rgba)
{
    const float h = std::clamp(u, 0.0f, 1.0f) * hBar;
    if (h <= 0) return;
    float x0, x1, y0, y1;
    PictureExtent(x0, x1, y0, y1);
    FillScreenRect(x0, y0, x1, h, rgba);
    FillScreenRect(x0, 448 - h, x1, y1, rgba);
}

void CHud::Wipe(float u, glm::vec4 rgba, float dxEdge)
{
    float xl, xr, yt, yb;
    PictureExtent(xl, xr, yt, yb);
    const float x = xl + std::clamp(u, 0.0f, 1.0f) * (xr - xl + dxEdge);
    if (x <= xl) return;
    if (m_abatch.empty() || m_abatch.back().mode != m_mode || m_fBreakBatch)
        { m_abatch.push_back({m_mode, m_atri.size(), 0, m_aline.size(), 0}); m_fBreakBatch = false; }
    const float xs = std::max(xl, x - dxEdge);
    FillScreenRect(xl, yt, xs, yb, rgba);
    glm::vec4 c0 = rgba;
    c0.a = 0;
    const VTX a{{xs, yt}, rgba, 1.0f}, b{{x, yt}, c0, 1.0f}, c{{xs, yb}, rgba, 1.0f}, d{{x, yb}, c0, 1.0f};
    m_atri.insert(m_atri.end(), {a, b, c, c, b, d});
}

void CHud::CloseBatch()
{
    if (m_abatch.empty()) return;
    m_abatch.back().ctri = m_atri.size() - m_abatch.back().itri;
    m_abatch.back().cline = m_aline.size() - m_abatch.back().iline;
}

// orig: 0x002166B0 + 0x002162D8 + 0x00216750 (one box; no wrapping / clipping box)
void CHud::DrawText(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float x, float y,
                    float size, float sx, float sy, int halign, glm::vec4 offset, glm::vec4 scale, glm::vec4 offsetOther)
{
    DrawTextBox(fontBase, pfontOther, text, x, y, size, sx, sy, 0.0f, 0.0f, halign, 0, offset, scale, offsetOther, 0.0f);
}

// orig: 0x00215F40 (the reader's word wrap): spaces become line breaks where a line passes the box's width
std::string StrWrapped(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88, float wBox)
{
    if (wBox <= 0.0f) return text;
    std::string out = text;
    const FONTBRX* pfont = &fontBase;
    float w = 0.0f;
    size_t iSpace = std::string::npos;
    float wAtSpace = 0.0f;
    // (the Japanese text: the line's characters so far: where each starts (its codes before it included), the
    // character and its width)
    struct LCH { size_t ib; uint32_t ch; float dx; };
    std::vector<LCH> aline;
    size_t ibNext = 0;   // where the next character starts (after the last one)
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i] == '&' && i + 1 < out.size())
        {
            if (out[i + 1] != '&')
            {
                pfont = out[i + 1] == '.' || !pfontOther ? &fontBase : pfontOther;
                ++i;
                continue;
            }
            ++i;
        }
        if (out[i] == '\n') { w = 0.0f; iSpace = std::string::npos; aline.clear(); ibNext = i + 1; continue; }
        if (out[i] == ' ') { iSpace = i; wAtSpace = w; }
        std::size_t cb = 1;
        const uint32_t ch = ChDecode(out, i, &cb);
        const float dx = pfont->DxFromCh(ch) * k88;
        if (FWideText())
        {
            // orig JP 0x217538: the character that passes the box goes to the next line, with the characters before
            // it back to the last place a line may break (JP 0x220840: not before a closing mark, not after an
            // opening one); without one the line breaks before that character. Nothing hangs past the box.
            const LCH lch{ibNext, ch, dx};
            if (w + dx > wBox && w > 0.0f)
            {
                size_t j = aline.size();   // (break before aline[j], j == size: before this character)
                while (j > 0 && (FJapaneseNoBreakBefore(j < aline.size() ? aline[j].ch : ch) || FJapaneseNoBreakAfter(aline[j - 1].ch)))
                    --j;
                if (j == 0) j = aline.size();
                if (static const bool s_fLog = std::getenv("SLY2N_WRAPLOG") != nullptr; s_fLog && j < aline.size())
                    std::printf("wrap: %zu moved down: %s\n", aline.size() - j, out.c_str());
                const size_t ibBreak = j < aline.size() ? aline[j].ib : lch.ib;
                out.insert(ibBreak, 1, '\n');
                std::vector<LCH> anext;
                w = 0.0f;
                for (size_t k = j; k < aline.size(); ++k)
                {
                    anext.push_back({aline[k].ib + 1, aline[k].ch, aline[k].dx});
                    w += aline[k].dx;
                }
                aline = std::move(anext);
                iSpace = std::string::npos;
                ++i;
                aline.push_back({lch.ib + 1, ch, dx});
            }
            else aline.push_back(lch);
            w += dx;
            i += cb - 1;
            ibNext = i + 1;
            continue;
        }
        w += dx;
        i += cb - 1;
        if (w > wBox && iSpace != std::string::npos)
        {
            out[iSpace] = '\n';
            w -= wAtSpace + pfont->DxFromCh(uint8_t(' ')) * k88;
            iSpace = std::string::npos;
        }
    }
    return out;
}

void CHud::DrawTextBox(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& textIn, float x, float y,
                       float size, float sx, float sy, float wBox, float hBox, int halign, int valign, glm::vec4 offset,
                       glm::vec4 scale, glm::vec4 offsetOther, float depth, bool fWrap)
{
    const float k88 = size / 51.2f;     // 0x3CA00000
    if (pfontOther && std::getenv("SLY2N_ICONTEXT") && textIn.find('&') != std::string::npos)
        std::fprintf(stderr, "icontext frame %d t %.3f ", g_iframeHarness, g_clock.t), std::fprintf(stderr, "'%s' off %.0f %.0f %.0f other %.0f %.0f %.0f\n", textIn.c_str(), offset.r, offset.g, offset.b, offsetOther.r, offsetOther.g, offsetOther.b);
    std::string text = pfontOther ? StrPromptFallbackText(textIn) : textIn;
    if (fWrap) text = StrWrapped(fontBase, pfontOther, text, k88, wBox);
    HUDSTATE st;
    st.scale = scale;
    st.depth = depth;
    // 0x2166B0: the box scaled by the glyph scale, lowered by the size; 0x2162D8: the screen matrix
    st.m[0] = sx * k88 * UI_KX;
    st.m[5] = sy * k88 * UI_KY;
    const float x0 = x * UI_KX + 2048.0f + GS_ORIGIN;
    float ty = (y + size * sy) * UI_KY + 2048.0f + GS_ORIGIN;
    const float dyLine = size * sy * UI_KY;
    // line widths for the alignment
    std::vector<float> awLine(1, 0.0f);
    const FONTBRX* pfont = &fontBase;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '&' && i + 1 < text.size() && text[i + 1] != '&')
        {
            pfont = text[i + 1] == '.' || !pfontOther ? &fontBase : pfontOther;
            ++i;
            continue;
        }
        if (text[i] == '&') ++i;
        if (text[i] == '\n') { awLine.push_back(0.0f); continue; }
        std::size_t cb = 1;
        awLine.back() += pfont->DxFromCh(ChDecode(text, i, &cb)) * k88;
        i += cb - 1;
    }
    pfont = &fontBase;
    if (valign != 0)
    {
        const float dy = hBox * sy * UI_KY - float(awLine.size()) * dyLine;
        ty += valign == 2 ? dy : dy * 0.5f;
    }
    size_t iline = 0;
    bool fLineStart = true;
    float tx = x0;
    st.offset = offset;
    for (size_t i = 0; i < text.size(); ++i)
    {
        std::size_t cbCh = 1;
        uint32_t ch = ChDecode(text, i, &cbCh);
        i += cbCh - 1;
        if (ch == '&' && i + 1 < text.size())
        {
            if (text[i + 1] == '&') ++i;
            else
            {
                const bool fBase = text[i + 1] == '.';
                st.offset = fBase ? offset : offsetOther;
                pfont = fBase || !pfontOther ? &fontBase : pfontOther;
                ++i;
                continue;
            }
            ch = uint8_t(text[i]);
        }
        if (ch == '\n') { ++iline; fLineStart = true; ty += dyLine; continue; }
        if (fLineStart)
        {
            tx = x0;
            if (halign != 0)
            {
                const float w = awLine[std::min(iline, awLine.size() - 1)] * sx * UI_KX;
                const float d = wBox * sx * UI_KX - w;
                tx = x0 + (halign == 2 ? d : d * 0.5f);
            }
            fLineStart = false;
        }
        if (const SELEM* pglyph = pfont->PglyphFromCh(ch))
        {
            st.m[8] = tx;
            st.m[9] = ty;
            // the button icons: the bound button for the device in use (promptglyph.h; widths unchanged)
            const PROMPTGLYPH pg = pfont == pfontOther && (ch < 0x80 || !FWideText()) ? PromptGlyphFor(char(ch)) : PROMPTGLYPH{};
            glm::vec2 lo, hi;
            float z = 0.0f, a = 0.0f;
            if (pg.kind == PROMPTGLYPH::Icon && FGlyphBox(*pglyph, st, &lo, &hi, &z, &a))
            {
                // a square of the glyph's height, centred on the game's icon
                float h = hi.y - lo.y;
                const float xc = 0.5f * (lo.x + hi.x), yc = 0.5f * (lo.y + hi.y);
                // (the Start / Select glyphs are flat pills: their icons as tall as the face buttons' (Triangle's), so a
                // keycap or a pad's small button stays readable; N39)
                if (ch == '!' || ch == '_')
                    if (const SELEM* pglyphT = pfont->PglyphFromCh(uint8_t('T')))
                    {
                        glm::vec2 loT, hiT;
                        float zT, aT;
                        if (FGlyphBox(*pglyphT, st, &loT, &hiT, &zT, &aT)) h = std::max(h, hiT.y - loT.y);
                    }
                // (the canvas's pixels are 480 / 448 as wide as high: 640 x 448 shown at 4:3)
                const float hx = 0.5f * h * (480.0f / 448.0f);
                DrawTexQuad(pg.tex, xc - hx, yc - 0.5f * h, xc + hx, yc + 0.5f * h, pg.u0, pg.v0, pg.u1, pg.v1,
                            glm::vec4(128.0f, 128.0f, 128.0f, a), z, true);
            }
            else if (pg.kind == PROMPTGLYPH::Glyph && pfont->PglyphFromCh(uint8_t(pg.ch)))
                DrawSprite(*pfont->PglyphFromCh(uint8_t(pg.ch)), st);
            else DrawSprite(*pglyph, st);
        }
        tx += pfont->DxFromCh(ch) * k88 * sx * UI_KX;
    }
}

void CHud::Flush(int width, int height)
{
    CloseBatch();
    if (!s_vao)
    {
        glGenVertexArrays(1, &s_vao);
        glGenBuffers(1, &s_vbo);
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, pos)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, rgba)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, z)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(VTX), reinterpret_cast<void*>(offsetof(VTX, uv)));
        glBindVertexArray(0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl.FboScene());
    glViewport(0, 0, width, height);
    glClearDepth(0.0);
    if (static const bool s_fKeepZ = std::getenv("SLY2N_HUDKEEPZ") != nullptr; !s_fKeepZ) glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glHudShader.Use();
    glUniform4f(glHudShader.Loc("uCanvas"), g_display.uiScale.x, g_display.uiScale.y, g_display.uiOffset.x, g_display.uiOffset.y);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    // one buffer: the triangles, the lines, the quads
    std::vector<VTX> av = m_atri;
    av.insert(av.end(), m_aline.begin(), m_aline.end());
    std::vector<size_t> aivtxQuad;
    for (const QUAD& q : m_aquad) { aivtxQuad.push_back(av.size()); av.insert(av.end(), q.av.begin(), q.av.end()); }
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(av.size() * sizeof(VTX)), av.data(), GL_STREAM_DRAW);
    glLineWidth(std::max(1.0f, float(height) / 448.0f));
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);
    const GLint locTextured = glHudShader.Loc("fTextured"), locTex = glHudShader.Loc("tex"), locNz = glHudShader.Loc("fAlphaNonZero");
    static const bool s_fNoTest = std::getenv("SLY2_RTGTNOTEST") != nullptr;
    auto setMode = [&](int mode) {
        // the 2D modes of 0x217298 (MSCAL pairs into P_K21's GS-state blocks), as the HUD uses them around a
        // masked piece: 1 writes the mask, 3 draws inside it, 2 clears it, 4 (and 0) draw normally.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        if (mode == 1 || mode == 2)
        {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, mode == 1 ? 1 : 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        }
        else if (mode == 3)
        {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        }
    };
    size_t iquad = 0;
    auto drawQuadsBefore = [&](size_t ibatch) {
        for (; iquad < m_aquad.size() && m_aquad[iquad].ibatch <= ibatch; ++iquad)
        {
            const QUAD& q = m_aquad[iquad];
            setMode(q.mode);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, q.tex);
            glBindSampler(0, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // TEX1 0x60: linear, no mips (the prompt atlases, the HQ logo and texture mods keep their mipmaps)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, q.fMip || FTextureIsMod(q.tex) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glUniform1i(locTextured, 1);
            glUniform1i(locTex, 0);
            glUniform1i(locNz, (q.fReplace || q.fAlphaTest) && !s_fNoTest ? 1 : 0);
            if (q.fReplace) glDisable(GL_BLEND);
            glDrawArrays(GL_TRIANGLES, GLint(aivtxQuad[iquad]), GLsizei(q.av.size()));
            if (q.fReplace) glEnable(GL_BLEND);
            glUniform1i(locNz, 0);
            glUniform1i(locTextured, 0);
        }
    };
    for (size_t ib = 0; ib < m_abatch.size(); ++ib)
    {
        const BATCH& b = m_abatch[ib];
        drawQuadsBefore(ib);
        setMode(b.mode);
        if (b.ctri) glDrawArrays(GL_TRIANGLES, GLint(b.itri), GLsizei(b.ctri));
        if (b.cline) glDrawArrays(GL_LINES, GLint(m_atri.size() + b.iline), GLsizei(b.cline));
    }
    drawQuadsBefore(m_abatch.size());
    glDisable(GL_STENCIL_TEST);
    glUniform1i(glHudShader.Loc("fAlphaNonZero"), 0);
    glUniform1i(glHudShader.Loc("fTextured"), 0);
    glBindVertexArray(0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
}

namespace
{
struct RAMVIEW
{
    std::vector<uint8_t> ram;
    uint32_t U32(uint32_t va) const { uint32_t v = 0; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; }
    float F32(uint32_t va) const { float v = 0; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; }
    bool Valid(uint32_t va) const { va &= 0x1FFFFFF; return va >= 0x100000 && va + 0x40 < ram.size(); }
};

// The loaded element that the RAM element at e (+0 index, +4 kind, +8 its screen object (oid at +0x18)) was
// loaded into: its array starts at e - index * 0x40, which is either the object's own (+0x54) or a set element's
// (+0x14) of the same object.
std::map<uint32_t, uint32_t> s_mpsetbase;   // RAM element array -> the RAM set element holding it
const SELEM* PelemAt(const RAMVIEW& r, uint32_t e, const std::vector<std::shared_ptr<SCREENOBJ>>& aso)
{
    const uint32_t idx = r.U32(e), kind = r.U32(e + 4);
    const uint32_t pso = r.U32(e + 8);
    if (!r.Valid(pso)) return nullptr;
    const int32_t oid = int32_t(r.U32(pso + 0x18));
    const uint32_t base = e - idx * 0x40;
    for (const auto& p : aso)
    {
        if (p->oid != oid) continue;
        const std::vector<SELEM>* paelem = &p->aelem;
        if (base != r.U32(pso + 0x54))
        {
            auto it = s_mpsetbase.find(base);
            if (it == s_mpsetbase.end()) continue;
            const uint32_t iset = r.U32(it->second);
            if (iset >= p->aelem.size() || p->aelem[iset].kind != SELEMK_Set) continue;
            paelem = &p->aelem[iset].asub;
        }
        if (idx < paelem->size() && (*paelem)[idx].kind == kind) return &(*paelem)[idx];
    }
    return nullptr;
}
const SELEM* PelemFromRam(const RAMVIEW& r, uint32_t e, const std::vector<std::shared_ptr<SCREENOBJ>>& aso) { return PelemAt(r, e, aso); }

const SELEM* PquadFromRam(const RAMVIEW& r, uint32_t e, const std::vector<std::shared_ptr<SCREENOBJ>>& aso) { return PelemAt(r, e, aso); }

void ReadState(const RAMVIEW& r, uint32_t pst, HUDSTATE& st)
{
    // the 3 qw at VU 4: +8 depth, +0xC revealed count, +0x10 offset, +0x20 scale
    st.depth = r.F32(pst + 8);
    st.cvtxReveal = int(r.U32(pst + 0xC));
    for (int i = 0; i < 4; ++i)
    {
        st.offset[i] = r.F32(pst + 0x10 + 4 * i);
        st.scale[i] = r.F32(pst + 0x20 + 4 * i);
    }
}
}  // namespace

int CDrawHudFromRam(const std::string& ramPath, const std::vector<std::shared_ptr<SCREENOBJ>>& aso, std::string& log)
{
    RAMVIEW r;
    {
        std::ifstream in(ramPath, std::ios::binary);
        r.ram.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (r.ram.size() < 0x2000000) { log = "no RAM"; return 0; }
    g_hud.SetMode(0);
    // chain -> RAM element (kind 1, +0x1C its chain), from every 0x40-aligned record that looks like one
    std::map<uint32_t, uint32_t> mpchain, mpquad;
    s_mpsetbase.clear();
    for (uint32_t e = 0x100000; e + 0x40 < 0x2000000; e += 0x10)
    {
        const uint32_t kind = r.U32(e + 4);
        if (kind == 8 && r.Valid(r.U32(e + 8)) && r.Valid(r.U32(e + 0x14))) s_mpsetbase.emplace(r.U32(e + 0x14), e);
        if ((kind == 6 || kind == 7) && r.Valid(r.U32(e + 8)))
        {
            const uint32_t chainQ = r.U32(e + (kind == 6 ? 0x1C : 0x18));
            if (r.Valid(chainQ) && !(chainQ & 0xF)) mpquad.emplace(chainQ, e);
            continue;
        }
        if (kind != 1) continue;
        const uint32_t chain = r.U32(e + 0x1C);
        if (!r.Valid(chain) || (chain & 0xF)) continue;
        if (!r.Valid(r.U32(e + 8))) continue;
        mpchain.emplace(chain, e);
    }
    // the kind-21 RPLs, in the order DrawRpl sends them (near to far: z descending)
    struct REC { float z; uint32_t a, b; };
    std::vector<REC> arec;
    for (uint32_t p = 0x3B3980; p < 0x3B3980 + 8192 * 16; p += 16)
        if (r.U32(p) == 21 && r.Valid(r.U32(p + 8))) arec.push_back({r.F32(p + 4), r.U32(p + 8), r.U32(p + 12)});
    std::stable_sort(arec.begin(), arec.end(), [](const REC& x, const REC& y) { return x.z < y.z; });
    int cdrawn = 0, cmiss = 0;
    char sz[256];
    std::string quads;
    for (const REC& rec : arec)
    {
        // walk the packet's DMA list: cnt tags with UNPACKs to VU 0 (matrix, 4 qw), 2 (matrix row 2), 4 (state,
        // 3 qw); call tags to element chains; ret ends it (0x217058 / 0x2174C8)
        HUDSTATE st;
        uint32_t pc = rec.a;
        uint32_t chain = rec.b;
        for (int guard = 0; guard < 4096 && r.Valid(pc); ++guard)
        {
            const uint32_t tag = r.U32(pc);
            const uint32_t id = (tag >> 28) & 7, qwc = tag & 0xFFFF;
            if (id == 1 && r.U32(pc + 16) == 0x13000000u && (r.U32(pc + 20) >> 24) == 0x14)
            {
                // 0x217298's mode packet: FLUSHA, MSCAL a, MSCAL b, FLUSHA
                const uint32_t a = (r.U32(pc + 20) & 0xFFFF) << 3, b = (r.U32(pc + 24) & 0xFFFF) << 3;
                const int mode = a == 0x00 && b == 0xC0 ? 1 : a == 0x30 && b == 0xC0 ? 2 : a == 0x100 && b == 0x60 ? 3 : a == 0x100 && b == 0x90 ? 4 : 0;
                g_hud.SetMode(mode);
                break;
            }
            if (id == 1)  // cnt: VIF codes in the 8 bytes after the tag then data
            {
                uint32_t q = pc + 8;
                const uint32_t end = pc + 16 + qwc * 16;
                while (q < end)
                {
                    const uint32_t vif = r.U32(q);
                    q += 4;
                    if ((vif >> 24) == 0x6C)   // UNPACK V4-32
                    {
                        const uint32_t num = (vif >> 16) & 0xFF, addr = vif & 0x3FF;
                        for (uint32_t k = 0; k < num * 4; ++k)
                        {
                            const uint32_t qw = addr + k / 4;
                            const float v = r.F32(q + 4 * k);
                            if (qw < 4) st.m[qw * 4 + k % 4] = v;
                            else if (qw < 7)
                            {
                                const uint32_t o = (qw - 4) * 16 + (k % 4) * 4;
                                if (o == 8) st.depth = v;
                                else if (o == 0xC) st.cvtxReveal = int(r.U32(q + 4 * k));
                                else if (o >= 0x10 && o < 0x20) st.offset[(o - 0x10) / 4] = v;
                                else if (o >= 0x20) st.scale[(o - 0x20) / 4] = v;
                            }
                        }
                        q += num * 16;
                    }
                }
                pc = end;
                continue;
            }
            if (id == 5 || id == 3)  // call / ref: an element chain
            {
                const uint32_t addr = r.U32(pc + 4);
                auto it = mpchain.find(addr);
                const SELEM* pelem = it != mpchain.end() ? PelemFromRam(r, it->second, aso) : nullptr;
                if (pelem && [] { static const bool f = std::getenv("SLY2_HUDRAMDBG") != nullptr; return f; }())
                    std::printf("ram sprite elem %u z %.0f rpl %.0f m %.2f %.2f pos %.1f %.1f off %.0f %.0f %.0f %.0f scale %.2f %.2f %.2f %.2f\n", pelem->idx, st.depth, rec.z, st.m[0], st.m[5],
                                (st.m[8] - 786432.0f - 1792.0f) / 0.8f, (st.m[9] - 786432.0f - 1936.0f) / (5.0f / 11.0f), st.offset.r, st.offset.g, st.offset.b, st.offset.a,
                                st.scale.r, st.scale.g, st.scale.b, st.scale.a);
                if (pelem) { g_hud.DrawSprite(*pelem, st); ++cdrawn; }
                else if (auto iq = mpquad.find(addr); iq != mpquad.end())
                {
                    const uint32_t e = iq->second;
                    std::snprintf(sz, sizeof(sz), " quad(kind %u id 0x%X)", r.U32(e + 4), r.U32(e + 0x10));
                    quads += sz;
                    if (const SELEM* pq = PquadFromRam(r, e, aso)) { g_hud.DrawQuad(*pq, st); ++cdrawn; }
                }
                else
                {
                    ++cmiss;
                    std::snprintf(sz, sizeof(sz), " miss(call 0x%X%s)", addr, it != mpchain.end() ? " elem" : "");
                    quads += sz;
                }
                pc += 16;
                continue;
            }
            break;   // ret / end
        }
        static const char* const s_szHudSkip = std::getenv("SLY2_HUDSKIP");
        if (chain && s_szHudSkip && std::strtoul(s_szHudSkip, nullptr, 16) == chain) chain = 0;
        if (chain)
        {
            auto it = mpchain.find(chain);
            const SELEM* pelem = it != mpchain.end() ? PelemFromRam(r, it->second, aso) : nullptr;
            if (pelem && [] { static const bool f = std::getenv("SLY2_HUDRAMDBG") != nullptr; return f; }())
                std::printf("ram chain elem %u z %.0f rpl %.0f m %.2f %.2f pos %.1f %.1f off %.0f %.0f %.0f %.0f scale %.2f %.2f %.2f %.2f\n", pelem->idx, st.depth, rec.z, st.m[0], st.m[5],
                            (st.m[8] - 786432.0f - 1792.0f) / 0.8f, (st.m[9] - 786432.0f - 1936.0f) / (5.0f / 11.0f), st.offset.r, st.offset.g, st.offset.b, st.offset.a,
                            st.scale.r, st.scale.g, st.scale.b, st.scale.a);
            if (pelem) { g_hud.DrawSprite(*pelem, st); ++cdrawn; }
            else if (auto iq = mpquad.find(chain); iq != mpquad.end() && PquadFromRam(r, iq->second, aso))
            {
                g_hud.DrawQuad(*PquadFromRam(r, iq->second, aso), st);
                ++cdrawn;
            }
            else
            {
                ++cmiss;
                std::snprintf(sz, sizeof(sz), " miss(B 0x%X%s)", chain, it != mpchain.end() ? " elem" : "");
                quads += sz;
            }
        }
    }
    std::snprintf(sz, sizeof(sz), "%zu kind-21 RPLs, %d sprites drawn, %d unmatched", arec.size(), cdrawn, cmiss);
    log = sz + quads;
    return cdrawn;
}
