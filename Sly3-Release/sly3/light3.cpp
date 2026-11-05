// See light3.h.

#include "light3.h"

#include "addr.h"
#include "condset3.h"

#include "../light.h"
#include "../lo.h"
#include "../sw.h"
#include "../worldaccess.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
// orig3: 0x11F4E0 RgbFromHsv (Sly 2 0x11DD90; h 0..360, s and v 0..1)
glm::vec3 RgbFromHsv3(float h0, float s0, float v0)
{
    float h = !(0.0f <= h0) ? -1.0f : (h0 > 360.0f ? 360.0f : h0);
    const float s = std::clamp(s0, 0.0f, 1.0f), v = std::clamp(v0, 0.0f, 1.0f);
    if (s == 0.0f) return glm::vec3(v);
    if (h == 360.0f) h = 0.0f;
    h = h * (1.0f / 60.0f);
    const int i = int(std::floor(h));
    const float f = h - float(i);
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i)
    {
    case 0: case 6: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    case 5: return {v, p, q};
    default: return glm::vec3(0.0f);
    }
}

// orig3: 0x2369D8: with GAME +0xC bit 1 a colour moves toward its luminance (weights 0x326E70) by the camera's +0x8C
glm::vec4 AdjustColor3(glm::vec4 c)
{
    if (!(g_grfGame3 & 2)) return c;
    const float k = g_condset3.gDesaturate;
    const float lum = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;   // TODO(sly3): the weights at 0x326E70
    for (int i = 0; i < 4; ++i) c[i] = c[i] * (1.0f - k) + (i < 3 ? lum : c[i]) * k;
    return c;
}

// orig3: 0x1C5630 FitLinearFunction
void FitLinear3(float x0, float y0, float x1, float y1, float& du, float& ru)
{
    if (std::fabs(x0 - x1) < 0.0001f) { ru = 0.0f; du = y0; return; }
    ru = (y1 - y0) / (x1 - x0);
    du = y0 - ru * x0;
}

// orig3: 0x1C57A8 ConvertAngleStrength (over cos(half angle) + its cube)
void ConvertAngleStrength3(float deg0, float g0, float deg1, float g1, float& du, float& ru)
{
    constexpr float kHalfRad = 0.008726647f;
    constexpr float k = 1.0f / 255.0f;
    const float s0 = std::cos(deg0 * kHalfRad), s1 = std::cos(deg1 * kHalfRad);
    FitLinear3(s0 + s0 * s0 * s0, g0 * k, s1 + s1 * s1 * s1, g1 * k, du, ru);
}

struct LIGHTPARAMS3
{
    int kind = 0;                       // +0xD0 (EOPID 159, setter)
    float hsv[3] = {0, 0, 0};           // +0xE0 (160)
    float strength = 0;                 // +0x140 (161)
    float angle = 240;                  // +0x144 (162)
    int fNoContext = 0;                 // +0x14C (164)
    int fNoBake = 0;                    // +0x150 (165)
    float sNear = 200, sFar = 2000;     // +0x154 / +0x158 (163)
    float gShadow = -1.0f;              // +0x148 (the shadow direction's weight, 0x1C6418; -1 in m_ext)
};

LIGHTPARAMS3 LightParamsOfLo3(const LO* plo)
{
    LIGHTPARAMS3 lp;
    for (const OPT& opt : plo->aopt)
    {
        if (opt.aval.empty()) continue;
        const OPTVAL& v = opt.aval[0];
        switch (opt.eopid)
        {
        case 159: lp.kind = v.n; break;
        case 160: for (int i = 0; i < 3; ++i) lp.hsv[i] = v.ag[size_t(i)]; break;
        case 161: lp.strength = v.ag[0]; break;
        case 162: lp.angle = v.ag[0]; break;
        case 164: lp.fNoContext = v.n; break;
        case 165: lp.fNoBake = v.n; break;
        case 163: lp.sNear = v.ag[0]; lp.sFar = v.ag[1]; break;
        default: break;
        }
    }
    return lp;
}

// orig3: 0x1C6418's weight of a chosen light in the shadow's direction: +0x148 w: 0 none, < 0 its strength at the point,
// else the strength x w / max(V, strength)
void SetShadowWeight3(LIGHTSEL& sel, float w, float gMax)
{
    sel.fShadow = w != 0.0f;
    sel.gShadowScale = w < 0.0f || gMax == 0.0f ? 1.0f : w / gMax;
}

// orig3: 0x1C5868 ComputeLight (the record +0x110, the selection's +0xF0 / +0x100 / +0x104)
void ComputeLight3(const LIGHTPARAMS3& lp, const glm::mat4& matWorld, LIGHTREC& rec, LIGHTSEL& sel)
{
    const float g = std::max(lp.hsv[2], lp.strength);
    float du = 0, ru = 0;
    ConvertAngleStrength3(std::max(lp.angle, 30.0f), 0.0f, 0.0f, g, du, ru);
    const glm::vec3 rgb = RgbFromHsv3(lp.hsv[0], lp.hsv[1] / 255.0f, lp.hsv[2] / 255.0f) * 255.0f;   // 0x11F6E8
    glm::vec4 c = AdjustColor3(glm::vec4(rgb, lp.strength));
    if (0.0f < g) c /= g;
    rec = LIGHTREC{};
    rec.kind = uint32_t(lp.kind);
    rec.angularOffset = du;
    rec.angularScale = -ru;
    rec.rgba = c;
    float g100 = 0, g104 = 0;
    sel.pos = glm::vec3(0.0f);
    if (lp.kind == 0)
    {
        g100 = 1.0f;
        rec.dirOrPos = glm::vec3(matWorld[0]);   // the world matrix's first row (vt slot 0x120's result)
    }
    else if (lp.kind == 1)
    {
        sel.pos = glm::vec3(matWorld[3]);
        rec.dirOrPos = sel.pos;
        FitLinear3(lp.sNear, 1.0f, lp.sFar, 0.0f, rec.distanceOffset, rec.distanceScale);
        g100 = rec.distanceOffset;
        g104 = rec.distanceScale;
    }
    const float m = std::max(lp.strength, lp.hsv[2]);
    sel.gConst = g100 * m;
    sel.gSlope = g104 * m;
    SetShadowWeight3(sel, lp.gShadow, m);
}

glm::mat4 MatWorldOfLight3(const LO* plo)
{
    if (const LOX* px = dynamic_cast<const LOX*>(plo)) return (px->pxf ? px->pxf->matWorld : glm::mat4(1.0f)) * px->mat;
    return glm::mat4(1.0f);
}

const int s_lightDefault3 = 0;
}  // namespace

// orig3: 0x169398
glm::vec4 WorldShader3()
{
    const CONDSET3& c = g_condset3;
    constexpr float k = 1.0f / 255.0f;   // 0x3B808081
    glm::vec4 rgb(RgbFromHsv3(c.hsvWorld[0], c.hsvWorld[1] * k, c.hsvWorld[2] * k) * (c.gWorldScale * k), 0.0f);
    rgb = AdjustColor3(rgb);
    return glm::vec4(glm::vec3(rgb), c.gWorldAmbient * k);
}

int UpdateWorldLights3(const SW* psw)
{
    static const SW* s_psw = nullptr;
    static size_t s_clight = 0;
    const std::vector<LO*>* papl = psw ? &psw->mpcidaplo[4] : nullptr;
    const size_t clight = papl ? papl->size() : 0;
    if (psw != s_psw || clight != s_clight) ResetWorldLights();
    s_psw = psw;
    s_clight = clight;
    if (!psw) return 0;
    int c = 0;
    bool fBaked = false;
    static const bool s_fDbg = std::getenv("SLY3_LIGHTDBG") != nullptr;
    for (const LO* plo : *papl)
    {
        if (plo->Cid() != 4) continue;
        const LIGHTPARAMS3 lp = LightParamsOfLo3(plo);
        if (lp.kind > 1) continue;
        LIGHTREC rec;
        LIGHTSEL sel;
        ComputeLight3(lp, MatWorldOfLight3(plo), rec, sel);
        if (s_fDbg)
            std::printf("light3 oid %d in %d ctx %d bake %d kind %d g %.3f %.5f rgba %.3f %.3f %.3f %.3f at %.1f %.1f %.1f\n", plo->oid,
                        int(plo->fInWorld), !lp.fNoContext, !lp.fNoBake, lp.kind, sel.gConst, sel.gSlope, rec.rgba.r, rec.rgba.g,
                        rec.rgba.b, rec.rgba.a, rec.dirOrPos.x, rec.dirOrPos.y, rec.dirOrPos.z);
        const bool fIn = plo->fInWorld;
        SetWorldLight(plo, rec, sel, lp.fNoBake == 0 && fIn, lp.fNoContext == 0 && fIn);
        if (lp.fNoBake == 0 && fIn) fBaked = true;
        ++c;
    }
    // orig3: 0x1C5150 (no light with +0x150 == 0: a directional one, strength 120, angle 240, black)
    if (!fBaked)
    {
        LIGHTPARAMS3 lp;
        lp.strength = 120.0f;
        lp.angle = 240.0f;
        glm::mat4 m(1.0f);
        m[0] = glm::vec4(glm::normalize(glm::vec3(-0.2f, 0.3f, -1.0f)), 0.0f);
        LIGHTREC rec;
        LIGHTSEL sel;
        ComputeLight3(lp, m, rec, sel);
        SetWorldLight(&s_lightDefault3, rec, sel, true, true);
        ++c;
    }
    SetWorldShader(WorldShader3());
    return c;
}

namespace
{
struct RAMLIGHT3
{
    LIGHTREC rec;
    LIGHTSEL sel;
    bool fBake = false, fContext = false;
};

std::vector<RAMLIGHT3> AlightRam3(const std::vector<uint8_t>& ram)
{
    std::vector<RAMLIGHT3> a;
    if (ram.size() < 0x2000000) return a;
    auto u32 = [&](uint32_t va) { uint32_t v; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; };
    auto f32 = [&](uint32_t va) { float v; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; };
    int c = 0;
    for (uint32_t p = u32(orig3::VA_SW + 0x3020 + 4 * 0xC); p && c < 4096; p = u32(p + 0x20), ++c)
    {
        uint64_t grf;
        std::memcpy(&grf, ram.data() + ((p + 0x28) & 0x1FFFFFF), 8);
        if (!((grf >> 59) & 1)) continue;
        const uint32_t q = p + 0x110;
        RAMLIGHT3 l;
        l.rec.kind = u32(q);
        if (l.rec.kind > 1) continue;
        l.rec.angularOffset = f32(q + 4);
        l.rec.angularScale = f32(q + 8);
        l.rec.distanceOffset = f32(q + 0xC);
        l.rec.rgba = glm::vec4(f32(q + 0x10), f32(q + 0x14), f32(q + 0x18), f32(q + 0x1C));
        l.rec.dirOrPos = glm::vec3(f32(q + 0x20), f32(q + 0x24), f32(q + 0x28));
        l.rec.distanceScale = f32(q + 0x2C);
        l.sel.pos = glm::vec3(f32(p + 0xF0), f32(p + 0xF4), f32(p + 0xF8));
        l.sel.gConst = f32(p + 0x100);
        l.sel.gSlope = f32(p + 0x104);
        SetShadowWeight3(l.sel, f32(p + 0x148), std::max(f32(p + 0xE8), f32(p + 0x140)));
        l.fBake = u32(p + 0x150) == 0;
        l.fContext = u32(p + 0x14C) == 0;
        a.push_back(l);
    }
    return a;
}
}  // namespace

int SetWorldLightsFromRam3(const std::vector<uint8_t>& ram)
{
    ResetWorldLights();
    const std::vector<RAMLIGHT3> a = AlightRam3(ram);
    static std::vector<char> s_aid;   // (the lights' identities)
    if (s_aid.size() < a.size()) s_aid.resize(a.size());
    for (size_t i = 0; i < a.size(); ++i) SetWorldLight(&s_aid[i], a[i].rec, a[i].sel, a[i].fBake, a[i].fContext);
    float aw[4];
    std::memcpy(aw, ram.data() + ((orig3::VA_CONDITIONS + 0x78) & 0x1FFFFFF), 16);
    SetWorldShader(glm::vec4(aw[0], aw[1], aw[2], aw[3]));
    return int(a.size());
}

bool FLightsFromRam3(const std::vector<uint8_t>& ram, LIGHTSET& lights)
{
    if (ram.size() < 0x2000000) return false;
    lights = LIGHTSET{};
    for (const RAMLIGHT3& l : AlightRam3(ram))
    {
        if (l.fBake && lights.alight.size() < 256) lights.alight.push_back(l.rec);
        if (l.fContext) lights.actx.emplace_back(l.rec, l.sel);
    }
    float aw[4];
    std::memcpy(aw, ram.data() + ((orig3::VA_CONDITIONS + 0x78) & 0x1FFFFFF), 16);
    lights.worldShader = glm::vec4(aw[0], aw[1], aw[2], aw[3]);
    return true;
}

int CheckLights3AgainstRam(const SW* psw, const std::vector<uint8_t>& ram, bool fVerbose)
{
    (void)psw;
    const std::vector<RAMLIGHT3> aram = AlightRam3(ram);
    const LIGHTSET& wl = WorldLights();
    int cmiss = 0;
    for (const RAMLIGHT3& r : aram)
    {
        float dBest = 1e30f;
        const LIGHTREC* pbest = nullptr;
        for (const LIGHTREC& n : wl.alight)
        {
            if (n.kind != r.rec.kind) continue;
            const float d = glm::distance(n.dirOrPos, r.rec.dirOrPos) + 1000.0f * glm::distance(n.rgba, r.rec.rgba) +
                            1000.0f * std::fabs(n.distanceScale - r.rec.distanceScale);
            if (d < dBest) dBest = d, pbest = &n;
        }
        const float tol = r.rec.kind == 0 ? 1e-3f : 1.0f;
        if (!r.fBake) continue;
        if (!pbest || dBest > tol)
        {
            ++cmiss;
            if (fVerbose)
                std::printf("RAM light kind %u at %.1f %.1f %.1f rgba %.3f %.3f %.3f %.3f: %s (best %.3f)\n", r.rec.kind, r.rec.dirOrPos.x,
                            r.rec.dirOrPos.y, r.rec.dirOrPos.z, r.rec.rgba.r, r.rec.rgba.g, r.rec.rgba.b, r.rec.rgba.a,
                            pbest ? "no exact native match" : "no native light", dBest);
        }
    }
    int cbakeRam = 0;
    for (const RAMLIGHT3& r : aram) cbakeRam += r.fBake;
    std::printf("lights: RAM %zu in the world (%d baked), native %zu baked; %d RAM lights without a native match\n", aram.size(),
                cbakeRam, wl.alight.size(), cmiss);
    const glm::vec4 w = WorldShader3();
    float aw[4];
    std::memcpy(aw, ram.data() + ((orig3::VA_CONDITIONS + 0x78) & 0x1FFFFFF), 16);
    std::printf("world shader: native %.4f %.4f %.4f %.4f, RAM %.4f %.4f %.4f %.4f\n", w.r, w.g, w.b, w.a, aw[0], aw[1], aw[2], aw[3]);
    return cmiss;
}
