// See light.h.

#include "light.h"

#include "glob.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace
{
struct WORLDLIGHTENTRY
{
    const void* plight = nullptr;
    LIGHTREC rec;
    LIGHTSEL sel;
    bool fBake = false, fContext = false, fSel = false;
};
struct WORLDLIGHTS
{
    std::vector<WORLDLIGHTENTRY> a;
    glm::vec4 worldShader{0.0f};
    bool fSet = false;
    LIGHTSET set;
    bool fDirty = true;
    uint32_t version = 1;
} s_wl;
}  // namespace

void ResetWorldLights()
{
    const uint32_t version = s_wl.version + 1;
    s_wl = WORLDLIGHTS{};
    s_wl.version = version;
}
uint32_t WorldLightsVersion() { return s_wl.version; }

void SetWorldLight(const void* plight, const LIGHTREC& rec, bool fActive)
{
    s_wl.fSet = true;
    s_wl.fDirty = true;
    for (auto& e : s_wl.a)
        if (e.plight == plight) { e.rec = rec; e.fBake = fActive; e.fSel = false; return; }
    WORLDLIGHTENTRY e;
    e.plight = plight; e.rec = rec; e.fBake = fActive;
    s_wl.a.push_back(e);
}

void SetWorldLight(const void* plight, const LIGHTREC& rec, const LIGHTSEL& sel, bool fBake, bool fContext)
{
    s_wl.fSet = true;
    for (auto& e : s_wl.a)
        if (e.plight == plight)
        {
            if (e.fSel && e.fBake == fBake && e.fContext == fContext && !std::memcmp(&e.rec, &rec, sizeof(LIGHTREC)) &&
                !std::memcmp(&e.sel, &sel, sizeof(LIGHTSEL)))
                return;   // unchanged
            e.rec = rec; e.sel = sel; e.fBake = fBake; e.fContext = fContext; e.fSel = true;
            ++s_wl.version;
            s_wl.fDirty = true;
            return;
        }
    ++s_wl.version;
    s_wl.fDirty = true;
    WORLDLIGHTENTRY e;
    e.plight = plight; e.rec = rec; e.sel = sel; e.fBake = fBake; e.fContext = fContext; e.fSel = true;
    s_wl.a.push_back(e);
}

void SetWorldShader(const glm::vec4& rgba)
{
    if (rgba == s_wl.worldShader) return;
    s_wl.worldShader = rgba;
    s_wl.fDirty = true;
    ++s_wl.version;
}
bool FHaveWorldLights() { return s_wl.fSet; }

const LIGHTSET& WorldLights()
{
    if (s_wl.fDirty)
    {
        s_wl.set.alight.clear();
        s_wl.set.actx.clear();
        for (const auto& e : s_wl.a)
        {
            if (e.fBake && s_wl.set.alight.size() < 256) s_wl.set.alight.push_back(e.rec);
            if (e.fSel && e.fContext && s_wl.set.actx.size() < 256) s_wl.set.actx.emplace_back(e.rec, e.sel);
        }
        s_wl.set.worldShader = s_wl.worldShader;
        s_wl.fDirty = false;
    }
    return s_wl.set;
}

namespace
{
uint8_t Ch(uint32_t rgba, int i) { return static_cast<uint8_t>(rgba >> (8 * i)); }

float Bits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// The normal matrix: cofactors of the model's 3x3 over the determinant (P_G 83 -> 1909, P_M 170-183).
glm::mat3 NormalMatrix(const glm::mat4& m)
{
    const glm::mat3 m3(m);
    return glm::transpose(glm::inverse(m3));
}

// The two-lane decomposition both programs end with: RGB illumination capped at `cap`, its alpha the midtone
// amount; the unlit remainder coloured by the world shader colour (A), the prepared colour carried by B with
// B's alpha counterbalancing the shadow.
void Decompose(glm::vec4 acc, float cap, float alphaOne, float alphaScale, const glm::vec4& world, float baseIntensity,
               const uint8_t base[4], int32_t a[4], int32_t b[4])
{
    const float maximum = std::max({acc.x, acc.y, acc.z});
    const float remaining = std::max(cap - maximum, 0.0f);
    for (int c = 0; c < 3; ++c) acc[c] = std::min(acc[c], cap);
    const float midtone = std::min(acc.w, remaining);
    const float shadow = remaining - midtone;
    const float alphaB = (alphaOne - shadow) * alphaScale;
    const float correction = alphaB > 0 ? float(base[3]) / alphaB : 0.0f;
    for (int c = 0; c < 3; ++c)
    {
        a[c] = static_cast<int32_t>((acc[c] + world[c] * shadow) * baseIntensity);
        b[c] = static_cast<int32_t>(float(base[c]) * midtone * correction);
    }
    a[3] = base[3];
    b[3] = static_cast<int32_t>(alphaB);
}

uint32_t Pack(const int32_t c[4], bool fQuantize)
{
    uint32_t rgba = 0;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t v = static_cast<uint8_t>(std::clamp(c[i], 0, 255));
        if (fQuantize && i < 3) v &= 0xF8;   // read back as RGB5A1 (0x167E58), expanded again by the VU (V4-5)
        rgba |= v << (8 * i);
    }
    return rgba;
}

float Angular(const LIGHTREC& l, const glm::vec3& normal, const glm::vec3& dir)
{
    const float d = glm::dot(normal, dir);
    return std::clamp(l.angularOffset + (d * d * d + d) * l.angularScale, 0.0f, 1.0f);
}

// A light's contribution at a point on a surface facing it (for choosing the strongest lights).
float Strength(const LIGHTREC& l, const glm::vec3& pos)
{
    float att = 1.0f;
    if (l.kind == 1) att = std::clamp(l.distanceOffset + glm::distance(pos, l.dirOrPos) * l.distanceScale, 0.0f, 1.0f);
    const float facing = std::clamp(l.angularOffset - 2.0f * l.angularScale, 0.0f, 1.0f);  // d = -1
    return att * facing * std::max({std::fabs(l.rgba.r), std::fabs(l.rgba.g), std::fabs(l.rgba.b), std::fabs(l.rgba.a)});
}
}  // namespace

uint64_t BakeKey(const glm::mat4& matWorld, const glm::vec3& posSphere, float sRadius, const LIGHTSET& lights)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* pv, size_t cb) {
        const uint8_t* pb = static_cast<const uint8_t*>(pv);
        for (size_t i = 0; i < cb; ++i) h = (h ^ pb[i]) * 1099511628211ull;
    };
    mix(&matWorld, sizeof(matWorld));
    mix(&lights.worldShader, sizeof(lights.worldShader));
    for (size_t i = 0; i < lights.alight.size() && i < 256; ++i)
    {
        const LIGHTREC& l = lights.alight[i];
        if (l.kind == 1 && l.distanceOffset + (glm::distance(posSphere, l.dirOrPos) - sRadius) * l.distanceScale <= 0) continue;
        mix(&l, sizeof(LIGHTREC));
    }
    return h;
}

// orig: VU1 P_M 0x25E090 (per vertex: world position and normal, every light's angular x distance weight
// accumulated, then the A/B decomposition), via the read-back 0x167E58 into RGB5A1 + alpha bytes.
void BakeGlobLighting(GLOB& glob, const glm::mat4& matWorld, const glm::vec3& posSphere, float sRadius,
                      const LIGHTSET& lights)
{
    const glm::mat3 matNormal = NormalMatrix(matWorld);
    // Point lights whose range does not reach the group's sphere are skipped for the whole group.
    std::vector<const LIGHTREC*> apl;
    for (size_t i = 0; i < lights.alight.size() && i < 256; ++i)
    {
        const LIGHTREC& l = lights.alight[i];
        if (l.kind == 1)
        {
            const float nearest = glm::distance(posSphere, l.dirOrPos) - sRadius;
            if (l.distanceOffset + nearest * l.distanceScale <= 0) continue;
        }
        apl.push_back(&l);
    }
    const float alphaScale = Bits(0x42fdff7d), alphaOne = 1.0f + Bits(0x3c010247);
    for (const SUBGLOB& sub : glob.asubglob)
    {
        const SHD* pshd = sub.pshd;
        const uint32_t m = pshd ? pshd->argba[0] : 0x80808080u;
        const float selfIllum = pshd ? pshd->unSelfIllum / 4096.0f : 0.0f;
        for (uint32_t k = 0; k < sub.cvtx; ++k)
        {
            VTXGLOB& v = glob.avtx[sub.ivtxFirst + k];
            if (!v.fUnbaked) continue;
            // 0x16A000's preparation of the bake input: vertex x material (RGB / 256, alpha / 510).
            uint8_t base[4];
            for (int c = 0; c < 3; ++c) base[c] = static_cast<uint8_t>((Ch(v.rgbaSrc, c) * Ch(m, c) + 255) / 256);
            base[3] = static_cast<uint8_t>((Ch(v.rgbaSrc, 3) * Ch(m, 3) + 255) / 510);
            const glm::vec3 pos = glm::vec3(matWorld * glm::vec4(v.pos, 1.0f));
            glm::vec3 n = matNormal * v.normal;
            const float len = glm::length(n);
            n = len > 0 ? n / len : glm::vec3(0, 0, 1);
            glm::vec4 acc(0, 0, 0, selfIllum + lights.worldShader.w);
            for (const LIGHTREC* pl : apl)
            {
                const LIGHTREC& l = *pl;
                glm::vec3 dir = l.dirOrPos;
                float att = 1.0f;
                if (l.kind == 1)
                {
                    dir = pos - l.dirOrPos;
                    const float d = glm::length(dir);
                    if (d <= 0) continue;
                    att = std::clamp(l.distanceOffset + d * l.distanceScale, 0.0f, 1.0f);
                    dir /= d;
                }
                acc += l.rgba * (Angular(l, n, dir) * att);
            }
            int32_t a[4], b[4];
            Decompose(acc, 1.0f, alphaOne, alphaScale, lights.worldShader, float(std::max({base[0], base[1], base[2]})), base, a, b);
            const uint32_t alphaA = v.rgbaA & 0xFF000000u;
            v.rgbaA = (Pack(a, true) & 0x00FFFFFF) | alphaA;
            v.rgbaB = Pack(b, true);
        }
    }
}

// orig: P_G 123-188 (the four lights in the object's local space) and 522-636 (per vertex: cubic response per
// light, capped; illumination = constant + responses; the A/B decomposition with a 2.0 cap).
void ForwardGlobLighting(GLOB& glob, const glm::mat4& matWorld, const glm::vec3& posCenter, const LIGHTSET& lights,
                         const std::vector<glm::mat4>* pamatBone)
{
    LIGHTREC al[4];
    SelectLights(lights, posCenter, al);
    const bool fSkin = pamatBone && !pamatBone->empty();
    // world -> local for directions (a skinned group's space is the world: the identity)
    const glm::mat3 cof = fSkin ? glm::mat3(1.0f) : glm::transpose(glm::inverse(glm::mat3(matWorld)));
    glm::vec3 dirLocal[4];
    glm::vec4 respColor[4], constant(0.0f);
    float upper[4];
    for (int k = 0; k < 4; ++k)
    {
        const LIGHTREC& l = al[k];
        glm::vec3 dir = l.dirOrPos;
        float offset = l.angularOffset, scale = l.angularScale;
        if (l.kind == 1)
        {
            dir = posCenter - l.dirOrPos;
            const float att = std::clamp(l.distanceOffset + l.distanceScale * glm::length(dir), 0.0f, 1.0f);
            offset *= att;
            scale *= att;
        }
        glm::vec3 local = glm::transpose(cof) * dir;
        const float len = glm::length(local);
        local = len > 0 ? local / len : glm::vec3(0.0f);
        dirLocal[k] = local * Bits(0x3c010204u);   // the normals are bytes x 127
        constant += l.rgba * offset;
        respColor[k] = l.rgba * scale * 2.0f;
        upper[k] = scale != 0 ? -offset / scale : 1e30f;
    }
    constant *= 2.0f;
    const float alphaScale = Bits(0x427dff7du), alphaOne = 2.0f + Bits(0x3c810247u);
    for (const SUBGLOB& sub : glob.asubglob)
    {
        const SHD* pshd = sub.pshd;
        const uint32_t m = pshd ? pshd->argba[0] : 0x80808080u;
        const float shadow1212 = pshd ? float(int16_t(pshd->unSelfIllum)) / 4096.0f : 0.0f;
        for (uint32_t k = 0; k < sub.cvtx; ++k)
        {
            VTXGLOB& v = glob.avtx[sub.ivtxFirst + k];
            if (!v.fUnbaked) continue;
            // 0x16A000's render items: normal x 127, intensity = the colour's largest channel / 255 x 127,
            // colour = vertex x material / 512 (alpha (a * m + 255) / 510).
            glm::vec3 n(float(int(v.normal.x * 127.0f)), float(int(v.normal.y * 127.0f)), float(int(v.normal.z * 127.0f)));
            if (fSkin)
            {
                // P_G 808: the weighted sum of the bones' rotations of the normal (the bone list as glob.vert reads it)
                glm::vec3 ns(0.0f);
                const size_t cbone = pamatBone->size();
                for (int w = 0; w < 4; ++w)
                    if (v.aw[w]) ns += (float(v.aw[w]) / 255.0f) * (glm::mat3((*pamatBone)[std::min<size_t>(v.abone[w], cbone - 1)]) * n);
                n = ns;
            }
            const float intensity = float(int(float(std::max({Ch(v.rgbaSrc, 0), Ch(v.rgbaSrc, 1), Ch(v.rgbaSrc, 2)})) / 255.0f * 127.0f));
            uint8_t base[4];
            for (int c = 0; c < 3; ++c) base[c] = static_cast<uint8_t>((Ch(v.rgbaSrc, c) * Ch(m, c) + 255) >> 9);
            base[3] = static_cast<uint8_t>((Ch(v.rgbaSrc, 3) * Ch(m, 3) + 255) / 510);
            // orig: 0x21C340 updates the live material-color alpha before P_G reads it.
            if (glob.fHistoryWorld) base[3] = uint8_t(v.rgbaA >> 24);
            glm::vec4 illum = constant;
            for (int j = 0; j < 4; ++j)
            {
                const float d = glm::dot(dirLocal[j], n);
                illum += respColor[j] * std::min(d + d * d * d, upper[j]);
            }
            illum.w += shadow1212 + lights.worldShader.w;
            int32_t a[4], b[4];
            Decompose(illum, 2.0f, alphaOne, alphaScale, lights.worldShader, intensity, base, a, b);
            // B's alpha: (2 + e) * s - shadow * s
            v.rgbaA = (Pack(a, false) & 0x00FFFFFF) | (v.rgbaA & 0xFF000000u);
            v.rgbaB = Pack(b, false);
        }
    }
}

// orig: 0x001A3F78: the four strongest candidates at pos (strength gConst + gSlope x distance), strongest first
// (an equal strength before), never a negative one.
static void SelectStrongest(const LIGHTSET& lights, const glm::vec3& pos, float ag[4], const LIGHTREC* apl[4], const LIGHTSEL* apsel[4])
{
    constexpr int n = 4;
    for (const auto& [rec, sel] : lights.actx)
    {
        const float g = sel.gConst + sel.gSlope * glm::length(pos - sel.pos);
        if (g < ag[n - 1]) continue;
        int k = n;
        while (k > 0)
        {
            const bool fMove = k < n;
            k -= 1;
            if (fMove) { ag[k + 1] = ag[k]; apl[k + 1] = apl[k]; apsel[k + 1] = apsel[k]; }
            if (k <= 0 || g < ag[k - 1]) break;
        }
        if (k < n) { ag[k] = g; apl[k] = &rec; apsel[k] = &sel; }
    }
}

// orig: 0x001A3F78 (with 0x1A4478's packet: the record of each, ALIGHT_DEFAULT 0x2BFC60 for an empty slot)
// The candidates in slot order, each with the strength gConst + gSlope x distance; the n strongest kept,
// strongest first (an equal strength goes before), a negative strength never. (The world's BSP of lit volumes
// 0x455C only limits which point lights are tried: those it leaves out are out of their range, so negative.)
void SelectLights(const LIGHTSET& lights, const glm::vec3& pos, LIGHTREC alight[4])
{
    static const bool s_fOld = std::getenv("SLY2_OLDSELECT") != nullptr;   // debugging: the old stand-in
    if (lights.fFixed)
    {
        for (int k = 0; k < 4; ++k)
        {
            if (k < static_cast<int>(lights.actx.size())) alight[k] = lights.actx[size_t(k)].first;
            else
            {
                alight[k] = LIGHTREC{};
                alight[k].angularScale = 0.5f;   // ALIGHT_DEFAULT 0x2BFC60
            }
        }
        return;
    }
    if (!lights.actx.empty() && !s_fOld)
    {
        constexpr int n = 4;
        float ag[n] = {};
        const LIGHTREC* apl[n] = {};
        const LIGHTSEL* apsel[n] = {};
        SelectStrongest(lights, pos, ag, apl, apsel);
        for (int k = 0; k < n; ++k)
        {
            if (apl[k]) alight[k] = *apl[k];
            else
            {
                // ALIGHT_DEFAULT 0x2BFC60: a black light
                alight[k] = LIGHTREC{};
                alight[k].angularScale = 0.5f;
            }
        }
        return;
    }
    std::vector<std::pair<float, const LIGHTREC*>> a;
    for (const LIGHTREC& l : lights.alight) a.emplace_back(Strength(l, pos), &l);
    std::stable_sort(a.begin(), a.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (int k = 0; k < 4; ++k)
    {
        if (k < static_cast<int>(a.size()) && a[size_t(k)].first > 0) alight[k] = *a[size_t(k)].second;
        else
        {
            alight[k] = LIGHTREC{};
            alight[k].angularScale = 0.5f;   // a black light (ALIGHT_DEFAULT 0x2BFC60)
        }
    }
}

// orig: 0x001A4478 (the shadow part, after the selection) and 0x001A4258 (the camera against the cone)
SHADOWCTX ShadowOfLightContext(const LIGHTSET& lights, const glm::vec3& pos, float sRadius, float sExtra,
                               const glm::vec3& posCamera, float sCameraNear)
{
    SHADOWCTX sh;
    if (sRadius == 0.0f || lights.actx.empty()) return sh;
    float ag[4] = {};
    const LIGHTREC* apl[4] = {};
    const LIGHTSEL* apsel[4] = {};
    SelectStrongest(lights, pos, ag, apl, apsel);
    constexpr float gDirDist = 1000.0f;   // 0x2BFC90: how far a directional light is placed
    constexpr float gRangeK = 2.5f;       // 0x2BFC94
    float gSum = 0;
    glm::vec3 acc(0.0f);
    for (int i = 0; i < 4 && apl[i]; ++i)
    {
        if (!apsel[i]->fShadow) continue;
        const float g = ag[i] * apsel[i]->gShadowScale;   // (Sly 3's weight; 1 for Sly 2's lights)
        if (apl[i]->kind == 0)
        {
            gSum += g;
            acc += (pos - apl[i]->dirOrPos * gDirDist) * g;
        }
        else if (apl[i]->kind == 1)
        {
            gSum += g;
            acc += apl[i]->dirOrPos * g;
        }
    }
    if (gSum == 0.0f) return sh;
    glm::vec3 center = acc / gSum;
    glm::vec3 dir = pos - center;
    float dist = glm::length(dir);
    const float gRange = gRangeK * sRadius;
    if (dist < gRange)
    {
        dir = dist < 1.0f ? glm::vec3(0, 0, 1) : dir / dist;
        dist = gRange;
        center = pos - dir * gRange;
    }
    else dir /= dist;
    const float gLen = dist + sExtra;
    // 0x11E2D8(dir, (0, 1, 0), (1, 0, 0))
    glm::vec3 across = glm::cross(glm::vec3(0, 1, 0), dir);
    if (glm::dot(across, across) < 1e-12f) across = glm::cross(glm::vec3(1, 0, 0), dir);
    across = glm::normalize(across);
    sh.across = across;
    sh.up = glm::normalize(glm::cross(dir, across));
    sh.axis = dir * gLen;
    sh.center = center;
    sh.rLength = dist / gLen;
    sh.fValid = true;
    // the cone (pos r0, center + axis r1, center + 1.5 axis 0) against the camera (0x1A4258 with its near margin)
    const float r0 = sRadius, r1 = sRadius * gLen / dist;
    const glm::vec3 ac[3] = {pos, center + sh.axis, center + sh.axis * 1.5f};
    const float ar[3] = {r0, r1, 0.0f};
    for (int i = 0; i < 3 && !sh.fCameraInside; ++i)
        if (glm::length(posCamera - ac[i]) < sCameraNear + ar[i]) sh.fCameraInside = true;
    for (int j = 1; j < 3 && !sh.fCameraInside; ++j)
    {
        const glm::vec3 A = ac[j - 1], B = ac[j];
        const float rA = ar[j - 1], rB = ar[j];
        const glm::vec3 BA = B - A, PA = posCamera - A;
        const float L = glm::length(BA);
        if (L <= 0) continue;
        const glm::vec3 U = BA / L;
        const float s = (rA - rB) / L;
        glm::vec3 N = glm::cross(U, glm::cross(PA, BA));
        const float lN = glm::length(N);
        if (lN <= 0) continue;
        N /= lN;
        const float c = std::sqrt(std::max(0.0f, 1.0f - s * s));
        const glm::vec3 D = U * s + N * c;
        const glm::vec3 A1 = A + D * rA, B1 = B + D * rB;
        const float d1 = glm::dot(posCamera - A1, D);
        const glm::vec3 BA1 = B1 - A1;
        const float t = glm::dot(posCamera - A1, BA1) / glm::dot(BA1, BA1);
        if (d1 < sCameraNear && 0.0f <= t && t <= 1.0f) sh.fCameraInside = true;
    }
    return sh;
}
