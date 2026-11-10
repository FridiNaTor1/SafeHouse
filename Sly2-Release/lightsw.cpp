// See lightsw.h. Ported from Sly2-Reference game/m_1A3260.cpp (0x1A3260..0x1A3B38), util.cpp (ConvertHsvToRgb
// 0x11DD90, ConvertUserHsvToUserRgb 0x11DFA0) and m_20D9D0_p3.cpp (0x20F1C8), in plain IEEE float.

#include "jt.h"
#include "lightsw.h"

#include "disc.h"
#include "light.h"
#include "lo.h"
#include "sw.h"
#include "worldaccess.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace
{
constexpr uint32_t VA_AEOPID = 0x2AC880;   // the option descriptors (brx.cpp)

// The field an option of this object writes: the EOPID's field offset (grf 0x80: +0x18) or the setter's (grf
// 0x100: the field it sets, +0x08).
uint32_t FieldOfOption(const OPT& opt)
{
    const uint32_t va = VA_AEOPID + uint32_t(opt.eopid) * 0x28;
    return (opt.grf & 0x100) ? g_disc.U32Elf(va + 8) : g_disc.U32Elf(va + 0x18);
}

// orig: 0x0011DD90 ConvertHsvToRgb (h 0..360, s and v 0..1)
glm::vec3 RgbFromHsv(float h0, float s0, float v0)
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

// orig: 0x0011DFA0 ConvertUserHsvToUserRgb (s, v and the result 0..255)
glm::vec3 UserRgbFromUserHsv(const float hsv[3])
{
    const float k = 1.0f / 255.0f;
    return RgbFromHsv(hsv[0], hsv[1] * k, hsv[2] * k) * 255.0f;
}

// orig: 0x001A3730 FitLinearFunction: du + ru x through (x0, y0), (x1, y1)
void FitLinear(float x0, float y0, float x1, float y1, float& du, float& ru)
{
    if (std::fabs(x0 - x1) < 0.0001f) { ru = 0.0f; du = y0; return; }
    ru = (y1 - y0) / (x1 - x0);
    du = y0 - ru * x0;
}

// orig: 0x001A38A8 ConvertAngleStrength (over cos(half angle) + its cube)
void ConvertAngleStrength(float deg0, float g0, float deg1, float g1, float& du, float& ru)
{
    constexpr float kHalfRad = 0.008726647f;   // 0x3C0EFA36
    constexpr float k = 1.0f / 255.0f;
    const float s0 = std::cos(deg0 * kHalfRad), s1 = std::cos(deg1 * kHalfRad);
    FitLinear(s0 + s0 * s0 * s0, g0 * k, s1 + s1 * s1 * s1, g1 * k, du, ru);
}

// orig: 0x001A3968 (the record +0x120 and the selection's +0x100 / +0x110 / +0x114). matWorld: the light's
// world transform (vt slot 31: glm column 0 = its first row, the direction of a directional light).
void ComputeLight(const LIGHTPARAMS& lp, const glm::mat4& matWorld, LIGHTREC& rec, LIGHTSEL& sel)
{
    const float g = std::max(lp.hsv[2], lp.strength);
    const float deg = std::max(lp.angle, 30.0f);
    float du = 0, ru = 0;
    ConvertAngleStrength(deg, 0.0f, 0.0f, g, du, ru);
    const glm::vec3 rgb = UserRgbFromUserHsv(lp.hsv);   // (0x1ED2D8: the colour adjustment, off unless 0x3E12C8)
    glm::vec4 c(rgb, lp.strength);
    if (0.0f < g) c /= g;
    rec = LIGHTREC{};
    rec.kind = uint32_t(lp.kind);
    rec.angularOffset = du;
    rec.angularScale = -ru;
    rec.rgba = c;
    float g110 = 0, g114 = 0;
    sel.pos = glm::vec3(0.0f);
    if (lp.kind == 0)
    {
        g110 = 1.0f;
        g114 = 0.0f;
        rec.dirOrPos = glm::vec3(matWorld[0]);
    }
    else if (lp.kind == 1)
    {
        sel.pos = glm::vec3(matWorld[3]);
        rec.dirOrPos = sel.pos;
        FitLinear(lp.sNear, 1.0f, lp.sFar, 0.0f, rec.distanceOffset, rec.distanceScale);
        g110 = rec.distanceOffset;
        g114 = rec.distanceScale;
    }
    const float m = std::max(lp.strength, lp.hsv[2]);
    sel.gConst = g110 * m;
    sel.gSlope = g114 * m;
}

glm::mat4 MatWorldOfLight(const LO* plo)
{
    if (const LOX* px = dynamic_cast<const LOX*>(plo)) return (px->pxf ? px->pxf->matWorld : glm::mat4(1.0f)) * px->mat;
    if (FCidDerivedFrom(plo->Cid(), 0x30)) return MatJtLo(const_cast<LO*>(plo));  // vt slot 31
    return glm::mat4(1.0f);
}

// The default light of 0x1A3260 (a directional light turned toward (-0.2, 0.3, -1), strength 255, angle 240).
const int s_lightDefault = 0;
}  // namespace

LIGHTPARAMS LightParamsOfLo(const LO* plo)
{
    LIGHTPARAMS lp;
    for (const OPT& opt : plo->aopt)
    {
        if (opt.aval.empty()) continue;
        const OPTVAL& v = opt.aval[0];
        switch (FieldOfOption(opt))
        {
        case 0xE0: lp.kind = v.n; break;
        case 0xF0: for (int i = 0; i < 3; ++i) lp.hsv[i] = v.ag[size_t(i)]; break;
        case 0x150: lp.strength = v.ag[0]; break;
        case 0x154: lp.angle = v.ag[0]; break;
        case 0x158: lp.n158 = v.n; break;
        case 0x15C: lp.fNoContext = v.n; break;
        case 0x160: lp.fNoBake = v.n; break;
        case 0x164: lp.sNear = v.ag[0]; lp.sFar = v.ag[1]; break;
        default: break;
        }
    }
    if (plo->fLightOwned) lp.fNoContext = lp.fNoBake = 1;   // (0x1BEAD0, game.cpp BuildRtgtOwners)
    return lp;
}

bool FLightOfLo(const LO* plo, LIGHTREC& rec, LIGHTSEL& sel)
{
    const LIGHTPARAMS lp = LightParamsOfLo(plo);
    if (lp.kind > 1) return false;
    ComputeLight(lp, MatWorldOfLight(plo), rec, sel);
    sel.fShadow = lp.n158;
    return true;
}

int UpdateWorldLightsFromSw(const SW* psw)
{
    // a new world: a new set (the same world: the records updated in place, unchanged ones keep the version)
    static const SW* s_psw = nullptr;
    static size_t s_clight = 0;
    const size_t clight = psw ? AploLights(psw).size() : 0;
#ifdef __SWITCH__
    // (the Switch: each light's parameters kept while its options are the same (they only grow, by a proxy's binds,
    // activate.cpp: the light, its option count and their storage tell), its record and selection data computed again
    // only when its world matrix changed. A light placed by a record (LOX, nearly all): the record and both matrices
    // compared, not their product computed each frame.)
    struct LIGHTKEPT
    {
        const LO* plo = nullptr;
        const OPT* popt = nullptr;
        size_t copt = 0;
        bool fOwned = false, fRec = false;
        LIGHTPARAMS lp;
        const LOX* plox = nullptr;   // (dynamic_cast's answer for plo)
        const XF* pxf = nullptr;     // its record when the matrix was computed, and the matrices then
        glm::mat4 matXf{0.0f}, matLocal{0.0f};
        glm::mat4 matWorld{0.0f};
        LIGHTREC rec;
        LIGHTSEL sel;
    };
    static std::vector<LIGHTKEPT> s_akept;
    if (psw != s_psw || clight != s_clight) s_akept.clear();
    size_t ilight = 0;
#endif
    if (psw != s_psw || clight != s_clight) ResetWorldLights();
    s_psw = psw;
    s_clight = clight;
    if (!psw) return 0;
    int c = 0;
    bool fWorldLight = false;
    for (const LO* plo : AploLights(psw))
    {
#ifdef __SWITCH__
        // (the lights four ahead, as last frame had them, prefetched: their objects, records and matrices are
        // scattered)
        if (ilight + 4 < s_akept.size())
        {
            const LIGHTKEPT& kn = s_akept[ilight + 4];
            if (kn.plo)
            {
                __builtin_prefetch(&kn.plo->aopt);
                __builtin_prefetch(&kn.plo->fInWorld);
                __builtin_prefetch(&kn.plo->fLightOwned);
            }
            if (kn.plox) __builtin_prefetch(&kn.plox->mat);
            if (kn.pxf) __builtin_prefetch(&kn.pxf->matWorld);
        }
        if (ilight == s_akept.size()) s_akept.emplace_back();
        LIGHTKEPT& k = s_akept[ilight++];
        if (k.plo != plo || k.popt != plo->aopt.data() || k.copt != plo->aopt.size() || k.fOwned != plo->fLightOwned)
        {
            k = LIGHTKEPT{plo, plo->aopt.data(), plo->aopt.size(), plo->fLightOwned, false, LightParamsOfLo(plo)};
            k.plox = dynamic_cast<const LOX*>(plo);
        }
        const LIGHTPARAMS& lp = k.lp;
        if (lp.kind > 1) continue;
        bool fSame = k.fRec;
        if (fSame && k.plox)
        {
            const XF* pxf = k.plox->pxf;
            fSame = pxf == k.pxf && std::memcmp(&k.plox->mat, &k.matLocal, sizeof(glm::mat4)) == 0 &&
                    (!pxf || std::memcmp(&pxf->matWorld, &k.matXf, sizeof(glm::mat4)) == 0);
        }
        glm::mat4 matWorld = k.matWorld;
        if (!k.plox || !fSame)
        {
            matWorld = MatWorldOfLight(plo);
            if (k.plox)
            {
                k.pxf = k.plox->pxf;
                k.matLocal = k.plox->mat;
                if (k.pxf) k.matXf = k.pxf->matWorld;
            }
            else fSame = fSame && std::memcmp(&matWorld, &k.matWorld, sizeof(glm::mat4)) == 0;
        }
        if (!fSame)
        {
            k.rec = LIGHTREC{};
            k.sel = LIGHTSEL{};
            ComputeLight(lp, matWorld, k.rec, k.sel);
            k.matWorld = matWorld;
            k.fRec = true;
        }
        LIGHTREC rec = k.rec;
        LIGHTSEL sel = k.sel;
#else
        const LIGHTPARAMS lp = LightParamsOfLo(plo);
        LIGHTREC rec;
        LIGHTSEL sel;
        ComputeLight(lp, MatWorldOfLight(plo), rec, sel);
        if (lp.kind > 1) continue;
#endif
        sel.fShadow = lp.n158;
        static const bool s_fDbg = std::getenv("SLY2_LIGHTDBG") != nullptr;
        static int s_cdbg = 0;
        if (s_fDbg && s_cdbg < 400)
        {
            ++s_cdbg;
            std::printf("light oid %d inworld %d noctx %d nobake %d kind %d g %.3f %.5f rgba %.2f %.2f %.2f %.2f pos %.1f %.1f %.1f\n", plo->oid,
                        plo->fInWorld, lp.fNoContext, lp.fNoBake, lp.kind, sel.gConst, sel.gSlope, rec.rgba.r, rec.rgba.g, rec.rgba.b,
                        rec.rgba.a, rec.dirOrPos.x, rec.dirOrPos.y, rec.dirOrPos.z);
        }
        // only the lights in the world (LO bit 60): the bake filters on it (0x167FC0) and the context's candidates are
        // the slotted lights (out of the world, e.g. Paris' 3797 / 7031 / 7430, the reference has +0x15C = 1)
        SetWorldLight(plo, rec, sel, lp.fNoBake == 0 && plo->fInWorld, lp.fNoContext == 0 && plo->fInWorld);
        if (lp.fNoBake == 0 && plo->fInWorld) fWorldLight = true;
        ++c;
    }
    // orig: 0x001A3260 (no light with +0x160 == 0: a directional one of strength 255, angle 240, colour 0)
    if (!fWorldLight)
    {
        LIGHTPARAMS lp;
        lp.strength = 255.0f;
        lp.angle = 240.0f;
        glm::mat4 m(1.0f);
        m[0] = glm::vec4(glm::normalize(glm::vec3(-0.2f, 0.3f, -1.0f)), 0.0f);
        LIGHTREC rec;
        LIGHTSEL sel;
        ComputeLight(lp, m, rec, sel);
        SetWorldLight(&s_lightDefault, rec, sel, true, true);
        ++c;
    }
    // orig: 0x0020F1C8 (the world shader colour: +0x3E10 HSV, x +0x3E20 / 255; alpha +0x3E24 / 255)
    float hsv[3] = {180.0f, 255.0f, 255.0f}, gScale = 90.0f, gAlpha = 0.0f;
    for (const OPT& opt : psw->aoptSw)
    {
        if (opt.aval.empty()) continue;
        const OPTVAL& v = opt.aval[0];
        switch (FieldOfOption(opt))
        {
        case 0x3E10: for (int i = 0; i < 3; ++i) hsv[i] = v.ag[size_t(i)]; break;
        case 0x3E20: gScale = v.ag[0]; break;
        case 0x3E24: gAlpha = v.ag[0]; break;
        default: break;
        }
    }
    const float k = 1.0f / 255.0f;
    const glm::vec3 rgb = RgbFromHsv(hsv[0], hsv[1] * k, hsv[2] * k) * (gScale * k);
    SetWorldShader(glm::vec4(rgb, gAlpha * k));   // (+0x3E38: alpha + 0.3 limited to 0..1; not set by the worlds seen)
    return c;
}

int CheckLightsAgainstRam(const SW* psw, const char* ramPath, bool fVerbose)
{
    std::ifstream in(ramPath, std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ram.size() < 0x2000000) { std::fprintf(stderr, "%s: not an EE RAM dump\n", ramPath); return -1; }
    auto u32 = [&](uint32_t va) { uint32_t v; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; };
    auto f32 = [&](uint32_t va) { float v; std::memcpy(&v, ram.data() + (va & 0x1FFFFFF), 4); return v; };
    // the reference's lights by oid (the class lists SW+0x3038 + cid x 0xC, linked by +0x20)
    std::multimap<int32_t, uint32_t> mp;
    for (uint32_t cid = 4; cid < 5; ++cid)
        for (uint32_t p = u32(0x3DD490 + 0x3038 + cid * 0xC); p; p = u32(p + 0x20)) mp.emplace(int32_t(u32(p + 0x18)), p);
    std::map<int32_t, int> mpiseen;
    int cdiff = 0, ccheck = 0;
    for (const LO* plo : AploLights(psw))
    {
        auto range = mp.equal_range(plo->oid);
        int i = mpiseen[plo->oid]++;
        auto it = range.first;
        for (; it != range.second && i > 0; ++it, --i) {}
        if (it == range.second) { if (fVerbose) std::printf("light %d: not in the RAM\n", plo->oid); continue; }
        const uint32_t p = it->second, q = p + 0x120;
        LIGHTREC rec;
        LIGHTSEL sel;
        ComputeLight(LightParamsOfLo(plo), MatWorldOfLight(plo), rec, sel);
        const float aRef[16] = {float(u32(q)), f32(q + 4), f32(q + 8), f32(q + 0xC), f32(q + 0x10), f32(q + 0x14), f32(q + 0x18),
                                f32(q + 0x1C), f32(q + 0x20), f32(q + 0x24), f32(q + 0x28), f32(q + 0x2C),
                                f32(p + 0x110), f32(p + 0x114), f32(p + 0x100), f32(p + 0x104)};
        const float aMine[16] = {float(rec.kind), rec.angularOffset, rec.angularScale, rec.distanceOffset, rec.rgba.r, rec.rgba.g,
                                 rec.rgba.b, rec.rgba.a, rec.dirOrPos.x, rec.dirOrPos.y, rec.dirOrPos.z, rec.distanceScale,
                                 sel.gConst, sel.gSlope, sel.pos.x, sel.pos.y};
        float dMax = 0;
        int iMax = -1;
        for (int k = 0; k < 16; ++k)
        {
            const float d = std::fabs(aRef[k] - aMine[k]) / std::max(1.0f, std::fabs(aRef[k]));
            if (d > dMax) { dMax = d; iMax = k; }
        }
        ++ccheck;
        if (dMax > 1e-4f)
        {
            ++cdiff;
            if (fVerbose)
                std::printf("light %d: field %d differs (ref %g, native %g)\n", plo->oid, iMax, aRef[iMax], aMine[iMax]);
        }
    }
    std::printf("lights: %d checked against the RAM, %d differ\n", ccheck, cdiff);
    return cdiff;
}
