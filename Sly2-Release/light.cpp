// See light.h.

#include "light.h"

#include "glob.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace
{
struct WORLDLIGHTENTRY
{
    const void* plight = nullptr;
    LIGHTREC rec;
    LIGHTSEL sel;
    bool fBake = false, fContext = false, fSel = false;
    // (FBakeLightsUnchangedSince: the bake's view of this light before its last change, and the versions of its last
    // two changes; a new entry was "not a bake light" before)
    LIGHTREC recBefore;
    bool fBakeBefore = false;
    uint32_t vChanged = 0, vChangedPrev = 0;
};
struct WORLDLIGHTS
{
    std::vector<WORLDLIGHTENTRY> a;
    glm::vec4 worldShader{0.0f};
    bool fSet = false;
    LIGHTSET set;
    bool fDirty = true;
    uint32_t version = 1;
    // (FBakeLightsUnchangedSince) the entries changed, in version order (from vLogBase on); the version of the reset,
    // of the last world shader change, of the last change not logged; the most bake entries there were
    std::vector<std::pair<uint32_t, uint32_t>> alog;
    uint32_t vLogBase = 0, vReset = 0, vShader = 0, vUntracked = 0;
    size_t cBakeMax = 0;
#ifdef __SWITCH__
    size_t ieNext = 0;   // (SetWorldLight: the entry after the last one set)
#endif
} s_wl;

void NoteBakeCount()
{
    size_t cBake = 0;
    for (const auto& x : s_wl.a) cBake += x.fBake;
    s_wl.cBakeMax = std::max(s_wl.cBakeMax, cBake);
}
// (an entry changed at the current version: what the bake saw before kept, logged)
void NoteEntryChange(WORLDLIGHTENTRY& e)
{
    e.recBefore = e.rec;
    e.fBakeBefore = e.fBake;
    e.vChangedPrev = e.vChanged;
}
void LogEntryChange(WORLDLIGHTENTRY& e, size_t ie)
{
    e.vChanged = s_wl.version;
    if (s_wl.alog.size() >= 8192)
    {
        // (old changes dropped: a group last checked before them takes the full key)
        const size_t cDrop = s_wl.alog.size() / 2;
        s_wl.vLogBase = s_wl.alog[cDrop - 1].first;
        s_wl.alog.erase(s_wl.alog.begin(), s_wl.alog.begin() + std::ptrdiff_t(cDrop));
    }
    s_wl.alog.emplace_back(s_wl.version, uint32_t(ie));
    if (e.fBake && !e.fBakeBefore) NoteBakeCount();
}
}  // namespace

void ResetWorldLights()
{
    const uint32_t version = s_wl.version + 1;
    s_wl = WORLDLIGHTS{};
    s_wl.version = version;
    s_wl.vReset = version;
}
uint32_t WorldLightsVersion() { return s_wl.version; }

void SetWorldLight(const void* plight, const LIGHTREC& rec, bool fActive)
{
    s_wl.fSet = true;
    s_wl.fDirty = true;
    s_wl.vUntracked = s_wl.version;   // (no version change: groups checked at this version or before take the full key)
    for (auto& e : s_wl.a)
        if (e.plight == plight) { e.rec = rec; e.fBake = fActive; e.fSel = false; NoteBakeCount(); return; }
    WORLDLIGHTENTRY e;
    e.plight = plight; e.rec = rec; e.fBake = fActive;
    s_wl.a.push_back(e);
    NoteBakeCount();
}

void SetWorldLight(const void* plight, const LIGHTREC& rec, const LIGHTSEL& sel, bool fBake, bool fContext)
{
    s_wl.fSet = true;
#ifdef __SWITCH__
    // (the Switch: the lights are set in the same order every frame (UpdateWorldLightsFromSw), so the entry after the
    // last one set is usually this light's, found without a search; a light has one entry at most)
    const size_t ieFirst = s_wl.ieNext < s_wl.a.size() && s_wl.a[s_wl.ieNext].plight == plight ? s_wl.ieNext : 0;
#else
    constexpr size_t ieFirst = 0;
#endif
    for (size_t ie = ieFirst; ie < s_wl.a.size(); ++ie)
        if (WORLDLIGHTENTRY& e = s_wl.a[ie]; e.plight == plight)
        {
#ifdef __SWITCH__
            s_wl.ieNext = ie + 1;
#endif
            if (e.fSel && e.fBake == fBake && e.fContext == fContext && !std::memcmp(&e.rec, &rec, sizeof(LIGHTREC)) &&
                !std::memcmp(&e.sel, &sel, sizeof(LIGHTSEL)))
                return;   // unchanged
            NoteEntryChange(e);
            e.rec = rec; e.sel = sel; e.fBake = fBake; e.fContext = fContext; e.fSel = true;
            ++s_wl.version;
            s_wl.fDirty = true;
            LogEntryChange(e, ie);
            return;
        }
    ++s_wl.version;
    s_wl.fDirty = true;
    WORLDLIGHTENTRY e;
    e.plight = plight; e.rec = rec; e.sel = sel; e.fBake = fBake; e.fContext = fContext; e.fSel = true;
    s_wl.a.push_back(e);
#ifdef __SWITCH__
    s_wl.ieNext = s_wl.a.size();
#endif
    LogEntryChange(s_wl.a.back(), s_wl.a.size() - 1);
}

void SetWorldShader(const glm::vec4& rgba)
{
    if (rgba == s_wl.worldShader) return;
    s_wl.worldShader = rgba;
    s_wl.fDirty = true;
    ++s_wl.version;
    s_wl.vShader = s_wl.version;
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
// (std::max({a, b, c}) exactly (the first of the largest), without the list's round trip through memory: it was the
// per-vertex lighting's top cost)
template <class T> inline T Max3(T a, T b, T c) { return std::max(std::max(a, b), c); }

[[gnu::always_inline]] inline void Decompose(glm::vec4 acc, float cap, float alphaOne, float alphaScale, const glm::vec4& world, float baseIntensity,
               const uint8_t base[4], int32_t a[4], int32_t b[4])
{
    const float maximum = Max3(acc.x, acc.y, acc.z);
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

[[gnu::always_inline]] inline uint32_t Pack(const int32_t c[4], bool fQuantize)
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

namespace
{
// whether the bake key (and the bake) takes a light: point lights whose range does not reach the sphere are skipped
inline bool FBakeKeyTakes(const LIGHTREC& l, const glm::vec3& posSphere, float sRadius)
{
    return !(l.kind == 1 && l.distanceOffset + (glm::distance(posSphere, l.dirOrPos) - sRadius) * l.distanceScale <= 0);
}
}  // namespace

bool FBakeLightsUnchangedSince(uint32_t v, const glm::vec3& posSphere, float sRadius, const LIGHTSET& lights)
{
    // (the world's set only, with every bake entry in it (never more than its 256), no reset, world shader change,
    // unlogged or dropped change since v)
    if (&lights != &s_wl.set || s_wl.fDirty || s_wl.cBakeMax > 256) return false;
    if (v < s_wl.vReset || v < s_wl.vShader || v <= s_wl.vUntracked || v < s_wl.vLogBase || v > s_wl.version) return false;
    auto it = std::upper_bound(s_wl.alog.begin(), s_wl.alog.end(), v, [](uint32_t x, const std::pair<uint32_t, uint32_t>& p) { return x < p.first; });
    if (s_wl.alog.end() - it > 64) return false;
    for (; it != s_wl.alog.end(); ++it)
    {
        const WORLDLIGHTENTRY& e = s_wl.a[it->second];
        if (e.vChangedPrev > v) return false;   // (changed twice since: its state at v is not kept)
        // its state at v: recBefore / fBakeBefore (the last change came after v, the one before at or before v)
        const bool fSameForBake = e.fBake == e.fBakeBefore && (!e.fBake || !std::memcmp(&e.rec, &e.recBefore, sizeof(LIGHTREC)));
        if (fSameForBake) continue;
        if (e.fBakeBefore && FBakeKeyTakes(e.recBefore, posSphere, sRadius)) return false;
        if (e.fBake && FBakeKeyTakes(e.rec, posSphere, sRadius)) return false;
    }
    return true;
}

bool FBakeLightsUnchangedForAll(uint32_t v, const LIGHTSET& lights)
{
    // (FBakeLightsUnchangedSince's tests without a sphere: true when every change since v is one no bake takes, which
    // that function then answers true for at any sphere)
    if (&lights != &s_wl.set || s_wl.fDirty || s_wl.cBakeMax > 256) return false;
    if (v < s_wl.vReset || v < s_wl.vShader || v <= s_wl.vUntracked || v < s_wl.vLogBase || v > s_wl.version) return false;
    auto it = std::upper_bound(s_wl.alog.begin(), s_wl.alog.end(), v, [](uint32_t x, const std::pair<uint32_t, uint32_t>& p) { return x < p.first; });
    if (s_wl.alog.end() - it > 64) return false;
    for (; it != s_wl.alog.end(); ++it)
    {
        const WORLDLIGHTENTRY& e = s_wl.a[it->second];
        if (e.vChangedPrev > v) return false;
        const bool fSameForBake = e.fBake == e.fBakeBefore && (!e.fBake || !std::memcmp(&e.rec, &e.recBefore, sizeof(LIGHTREC)));
        if (!fSameForBake) return false;
    }
    return true;
}

uint64_t BakeKey(const glm::mat4& matWorld, const glm::vec3& posSphere, float sRadius, const LIGHTSET& lights)
{
    // (a change key: compared only with the last one. Mixed 8 bytes at a time (every hashed record is a multiple of
    // 8 bytes): every static group computes it every frame the lights change)
    static_assert(sizeof(glm::mat4) % 8 == 0 && sizeof(glm::vec4) % 8 == 0 && sizeof(LIGHTREC) % 8 == 0);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* pv, size_t cb) {
        const uint8_t* pb = static_cast<const uint8_t*>(pv);
        for (size_t i = 0; i < cb; i += 8)
        {
            uint64_t w;
            std::memcpy(&w, pb + i, 8);
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
    };
    mix(&matWorld, sizeof(matWorld));
    mix(&lights.worldShader, sizeof(lights.worldShader));
    for (size_t i = 0; i < lights.alight.size() && i < 256; ++i)
    {
        const LIGHTREC& l = lights.alight[i];
        if (!FBakeKeyTakes(l, posSphere, sRadius)) continue;
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
#ifdef __SWITCH__
    // (the Switch: each thread's list kept from one bake to the next, the game thread's and the pass helpers')
    static thread_local std::vector<const LIGHTREC*> apl;
    apl.clear();
#else
    std::vector<const LIGHTREC*> apl;
#endif
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
#if defined(__aarch64__) || defined(_M_ARM64)
        // (AArch64: four vertices at a time through the lights, one per NEON lane, each lane the loop below's
        // operations in its order (-ffp-contract=off: no fused multiply-adds): the same colours. The position, the
        // normal and the decomposition stay the scalar code's.)
        {
            const float32x4_t zero = vdupq_n_f32(0.0f), one = vdupq_n_f32(1.0f);
            // std::clamp(x, 0, 1): x < 0 ? 0 : 1 < x ? 1 : x
            auto clamp01 = [&](float32x4_t x) { return vbslq_f32(vcltq_f32(x, zero), zero, vbslq_f32(vcltq_f32(one, x), one, x)); };
            for (uint32_t k0 = 0; k0 < sub.cvtx; k0 += 4)
            {
                const uint32_t cl = std::min(4u, sub.cvtx - k0);
                alignas(16) float apx[4] = {}, apy[4] = {}, apz[4] = {}, anx[4] = {}, any[4] = {}, anz[4] = {};
                uint8_t abase[4][4] = {};
                bool fAny = false;
                for (uint32_t i = 0; i < cl; ++i)
                {
                    const VTXGLOB& v = glob.avtx[sub.ivtxFirst + k0 + i];
                    if (!v.fUnbaked) continue;
                    fAny = true;
                    for (int c = 0; c < 3; ++c) abase[i][c] = static_cast<uint8_t>((Ch(v.rgbaSrc, c) * Ch(m, c) + 255) / 256);
                    abase[i][3] = static_cast<uint8_t>((Ch(v.rgbaSrc, 3) * Ch(m, 3) + 255) / 510);
                    const glm::vec3 pos = glm::vec3(matWorld * glm::vec4(v.pos, 1.0f));
                    glm::vec3 n = matNormal * v.normal;
                    const float len = glm::length(n);
                    n = len > 0 ? n / len : glm::vec3(0, 0, 1);
                    apx[i] = pos.x; apy[i] = pos.y; apz[i] = pos.z;
                    anx[i] = n.x; any[i] = n.y; anz[i] = n.z;
                }
                if (!fAny) continue;
                const float32x4_t px = vld1q_f32(apx), py = vld1q_f32(apy), pz = vld1q_f32(apz);
                const float32x4_t nx = vld1q_f32(anx), ny = vld1q_f32(any), nz = vld1q_f32(anz);
                float32x4_t accR = zero, accG = zero, accB = zero, accA = vdupq_n_f32(selfIllum + lights.worldShader.w);
                for (const LIGHTREC* pl : apl)
                {
                    const LIGHTREC& l = *pl;
                    float32x4_t dx, dy, dz, att = one;
                    uint32x4_t lit = vdupq_n_u32(~0u);
                    if (l.kind == 1)
                    {
                        dx = vsubq_f32(px, vdupq_n_f32(l.dirOrPos.x));
                        dy = vsubq_f32(py, vdupq_n_f32(l.dirOrPos.y));
                        dz = vsubq_f32(pz, vdupq_n_f32(l.dirOrPos.z));
                        const float32x4_t d = vsqrtq_f32(vaddq_f32(vaddq_f32(vmulq_f32(dx, dx), vmulq_f32(dy, dy)), vmulq_f32(dz, dz)));
                        lit = vmvnq_u32(vcleq_f32(d, zero));   // (the scalar loop's "if (d <= 0) continue")
                        att = clamp01(vaddq_f32(vdupq_n_f32(l.distanceOffset), vmulq_f32(d, vdupq_n_f32(l.distanceScale))));
                        dx = vdivq_f32(dx, d);
                        dy = vdivq_f32(dy, d);
                        dz = vdivq_f32(dz, d);
                    }
                    else
                    {
                        dx = vdupq_n_f32(l.dirOrPos.x);
                        dy = vdupq_n_f32(l.dirOrPos.y);
                        dz = vdupq_n_f32(l.dirOrPos.z);
                    }
                    // Angular: offset + (d d d + d) x scale, clamped; times the attenuation
                    const float32x4_t dd = vaddq_f32(vaddq_f32(vmulq_f32(nx, dx), vmulq_f32(ny, dy)), vmulq_f32(nz, dz));
                    const float32x4_t ang = clamp01(vaddq_f32(vdupq_n_f32(l.angularOffset),
                                                              vmulq_f32(vaddq_f32(vmulq_f32(vmulq_f32(dd, dd), dd), dd), vdupq_n_f32(l.angularScale))));
                    const float32x4_t f = vmulq_f32(ang, att);
                    accR = vbslq_f32(lit, vaddq_f32(accR, vmulq_f32(vdupq_n_f32(l.rgba.r), f)), accR);
                    accG = vbslq_f32(lit, vaddq_f32(accG, vmulq_f32(vdupq_n_f32(l.rgba.g), f)), accG);
                    accB = vbslq_f32(lit, vaddq_f32(accB, vmulq_f32(vdupq_n_f32(l.rgba.b), f)), accB);
                    accA = vbslq_f32(lit, vaddq_f32(accA, vmulq_f32(vdupq_n_f32(l.rgba.a), f)), accA);
                }
                alignas(16) float aR[4], aG[4], aB[4], aA[4];
                vst1q_f32(aR, accR); vst1q_f32(aG, accG); vst1q_f32(aB, accB); vst1q_f32(aA, accA);
                for (uint32_t i = 0; i < cl; ++i)
                {
                    VTXGLOB& v = glob.avtx[sub.ivtxFirst + k0 + i];
                    if (!v.fUnbaked) continue;
                    int32_t a[4], b[4];
                    Decompose(glm::vec4(aR[i], aG[i], aB[i], aA[i]), 1.0f, alphaOne, alphaScale, lights.worldShader,
                              float(Max3(abase[i][0], abase[i][1], abase[i][2])), abase[i], a, b);
                    const uint32_t alphaA = v.rgbaA & 0xFF000000u;
                    v.rgbaA = (Pack(a, true) & 0x00FFFFFF) | alphaA;
                    v.rgbaB = Pack(b, true);
                }
            }
            continue;
        }
#endif
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
            Decompose(acc, 1.0f, alphaOne, alphaScale, lights.worldShader, float(Max3(base[0], base[1], base[2])), base, a, b);
            const uint32_t alphaA = v.rgbaA & 0xFF000000u;
            v.rgbaA = (Pack(a, true) & 0x00FFFFFF) | alphaA;
            v.rgbaB = Pack(b, true);
        }
    }
}

#if defined(__SSE2__) || defined(_M_X64)
#define FWD_SSE 1
namespace
{
// float(int(float(c) / 255 * 127)) for a byte (the per-vertex intensity: a table of the same values)
struct INTENSITY127
{
    float a[256];
    INTENSITY127() { for (int i = 0; i < 256; ++i) a[i] = float(int(float(uint8_t(i)) / 255.0f * 127.0f)); }
};
const INTENSITY127 s_intensity127;

// (perf-3) ForwardGlobLighting's per-vertex step for four vertices at a time, one per SSE lane, each lane the scalar
// code's operations in its order: glm's dot (x + y) + z, mat3 x vec3 (c0 x + c1 y) + c2 z, std::min(a, b) = b < a ?
// b : a = _mm_min_ps(b, a), std::max(a, b) = a < b ? b : a = _mm_max_ps(b, a), float -> int truncating (cvtt), the
// clamp to 0..255 by saturating packs. Returns the first vertex of the sub-group not done (the scalar tail).
uint32_t ForwardLitSse(GLOB& glob, const SUBGLOB& sub, uint32_t m, float wAdd, const glm::vec3 dirLocal[4], const glm::vec4 respColor[4],
                       const glm::vec4& constant, const float upper[4], float alphaOne, float alphaScale, const glm::vec4& world,
                       const std::vector<glm::mat4>* pamatBone)
{
    const uint32_t c4 = sub.cvtx & ~3u;
    const size_t cbone = pamatBone ? pamatBone->size() : 0;
    const __m128 k127 = _mm_set1_ps(127.0f), zero = _mm_setzero_ps(), cap = _mm_set1_ps(2.0f);
    const __m128 vAlphaOne = _mm_set1_ps(alphaOne), vAlphaScale = _mm_set1_ps(alphaScale), vWAdd = _mm_set1_ps(wAdd);
    const __m128 cR = _mm_set1_ps(constant.r), cG = _mm_set1_ps(constant.g), cB = _mm_set1_ps(constant.b), cA = _mm_set1_ps(constant.a);
    const __m128 wR = _mm_set1_ps(world.r), wG = _mm_set1_ps(world.g), wB = _mm_set1_ps(world.b);
    const uint32_t m0 = Ch(m, 0), m1 = Ch(m, 1), m2 = Ch(m, 2), m3 = Ch(m, 3);
    for (uint32_t k = 0; k < c4; k += 4)
    {
        VTXGLOB* apv[4] = {&glob.avtx[sub.ivtxFirst + k], &glob.avtx[sub.ivtxFirst + k + 1], &glob.avtx[sub.ivtxFirst + k + 2],
                           &glob.avtx[sub.ivtxFirst + k + 3]};
        if (!(apv[0]->fUnbaked | apv[1]->fUnbaked | apv[2]->fUnbaked | apv[3]->fUnbaked)) continue;
        __m128 an[4];
        alignas(16) float aIntensity[4], aBase[4][4];
        for (int i = 0; i < 4; ++i)
        {
            const VTXGLOB& v = *apv[i];
            // normal x 127, truncated (the 4th lane is uv.x: unused)
            __m128 n = _mm_cvtepi32_ps(_mm_cvttps_epi32(_mm_mul_ps(_mm_loadu_ps(&v.normal.x), k127)));
            if (pamatBone)
            {
                const __m128 nx = _mm_shuffle_ps(n, n, 0x00), ny = _mm_shuffle_ps(n, n, 0x55), nz = _mm_shuffle_ps(n, n, 0xAA);
                __m128 ns = zero;
                for (int w = 0; w < 4; ++w)
                    if (v.aw[w])
                    {
                        const glm::mat4& b = (*pamatBone)[std::min<size_t>(v.abone[w], cbone - 1)];
                        __m128 t = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(&b[0][0]), nx), _mm_mul_ps(_mm_loadu_ps(&b[1][0]), ny));
                        t = _mm_add_ps(t, _mm_mul_ps(_mm_loadu_ps(&b[2][0]), nz));
                        ns = _mm_add_ps(ns, _mm_mul_ps(_mm_set1_ps(float(v.aw[w]) / 255.0f), t));
                    }
                n = ns;
            }
            an[i] = n;
            const uint32_t s = v.rgbaSrc;
            aIntensity[i] = s_intensity127.a[Max3(Ch(s, 0), Ch(s, 1), Ch(s, 2))];
            aBase[0][i] = float(uint8_t((Ch(s, 0) * m0 + 255) >> 9));
            aBase[1][i] = float(uint8_t((Ch(s, 1) * m1 + 255) >> 9));
            aBase[2][i] = float(uint8_t((Ch(s, 2) * m2 + 255) >> 9));
            aBase[3][i] = float(glob.fHistoryWorld ? uint8_t(v.rgbaA >> 24) : uint8_t((Ch(s, 3) * m3 + 255) / 510));
        }
        _MM_TRANSPOSE4_PS(an[0], an[1], an[2], an[3]);   // an[0] = the x's, an[1] the y's, an[2] the z's
        __m128 r = cR, g = cG, bl = cB, a = cA;
        for (int j = 0; j < 4; ++j)
        {
            const __m128 d = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(dirLocal[j].x), an[0]), _mm_mul_ps(_mm_set1_ps(dirLocal[j].y), an[1])),
                                        _mm_mul_ps(_mm_set1_ps(dirLocal[j].z), an[2]));
            const __m128 resp = _mm_min_ps(_mm_set1_ps(upper[j]), _mm_add_ps(d, _mm_mul_ps(_mm_mul_ps(d, d), d)));
            r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(respColor[j].r), resp));
            g = _mm_add_ps(g, _mm_mul_ps(_mm_set1_ps(respColor[j].g), resp));
            bl = _mm_add_ps(bl, _mm_mul_ps(_mm_set1_ps(respColor[j].b), resp));
            a = _mm_add_ps(a, _mm_mul_ps(_mm_set1_ps(respColor[j].a), resp));
        }
        a = _mm_add_ps(a, vWAdd);
        // Decompose (cap 2)
        const __m128 maximum = _mm_max_ps(bl, _mm_max_ps(g, r));
        const __m128 remaining = _mm_max_ps(zero, _mm_sub_ps(cap, maximum));
        r = _mm_min_ps(cap, r);
        g = _mm_min_ps(cap, g);
        bl = _mm_min_ps(cap, bl);
        const __m128 midtone = _mm_min_ps(remaining, a);
        const __m128 shadow = _mm_sub_ps(remaining, midtone);
        const __m128 alphaB = _mm_mul_ps(_mm_sub_ps(vAlphaOne, shadow), vAlphaScale);
        const __m128 base3 = _mm_load_ps(aBase[3]);
        const __m128 correction = _mm_and_ps(_mm_cmpgt_ps(alphaB, zero), _mm_div_ps(base3, alphaB));
        const __m128 intensity = _mm_load_ps(aIntensity);
        const __m128i aR = _mm_cvttps_epi32(_mm_mul_ps(_mm_add_ps(r, _mm_mul_ps(wR, shadow)), intensity));
        const __m128i aG = _mm_cvttps_epi32(_mm_mul_ps(_mm_add_ps(g, _mm_mul_ps(wG, shadow)), intensity));
        const __m128i aB = _mm_cvttps_epi32(_mm_mul_ps(_mm_add_ps(bl, _mm_mul_ps(wB, shadow)), intensity));
        const __m128i bR = _mm_cvttps_epi32(_mm_mul_ps(_mm_mul_ps(_mm_load_ps(aBase[0]), midtone), correction));
        const __m128i bG = _mm_cvttps_epi32(_mm_mul_ps(_mm_mul_ps(_mm_load_ps(aBase[1]), midtone), correction));
        const __m128i bB = _mm_cvttps_epi32(_mm_mul_ps(_mm_mul_ps(_mm_load_ps(aBase[2]), midtone), correction));
        const __m128i bA = _mm_cvttps_epi32(alphaB);
        // Pack: clamp(c, 0, 255) (int32 -> int16 -> uint8 saturating), then the planes interleaved to RGBA per lane
        auto pack = [](__m128i x, __m128i y, __m128i z, __m128i w) {
            __m128i p = _mm_packus_epi16(_mm_packs_epi32(x, y), _mm_packs_epi32(z, w));   // r0..3 g0..3 b0..3 a0..3
            p = _mm_unpacklo_epi8(p, _mm_srli_si128(p, 8));                                 // r0 b0 r1 b1 .. g0 a0 g1 a1 ..
            return _mm_unpacklo_epi8(p, _mm_srli_si128(p, 8));                              // r0 g0 b0 a0 r1 ..
        };
        alignas(16) uint32_t aA[4], aB4[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(aA), pack(aR, aG, aB, _mm_setzero_si128()));
        _mm_store_si128(reinterpret_cast<__m128i*>(aB4), pack(bR, bG, bB, bA));
        for (int i = 0; i < 4; ++i)
        {
            VTXGLOB& v = *apv[i];
            if (!v.fUnbaked) continue;
            v.rgbaA = (aA[i] & 0x00FFFFFF) | (v.rgbaA & 0xFF000000u);
            v.rgbaB = aB4[i];
        }
    }
    return c4;
}
}  // namespace
#define FORWARD_LIT_4 ForwardLitSse
#elif defined(__aarch64__) || defined(_M_ARM64)
#define FWD_SSE 1
namespace
{
// (as the SSE step's table)
struct INTENSITY127
{
    float a[256];
    INTENSITY127() { for (int i = 0; i < 256; ++i) a[i] = float(int(float(uint8_t(i)) / 255.0f * 127.0f)); }
};
const INTENSITY127 s_intensity127;

// SSE's min / max as the step above uses them (x < y ? x : y, x > y ? x : y), so the lanes keep the scalar code's
// results bit for bit (NEON's own fmin / fmax differ on signed zeros)
inline float32x4_t MinSse(float32x4_t x, float32x4_t y) { return vbslq_f32(vcltq_f32(x, y), x, y); }
inline float32x4_t MaxSse(float32x4_t x, float32x4_t y) { return vbslq_f32(vcgtq_f32(x, y), x, y); }

// ForwardLitSse on NEON (AArch64): the same operations in the same order per lane (-ffp-contract=off: no fused
// multiply-adds), float -> int truncating (fcvtzs, as the scalar casts compile on AArch64), the clamp to 0..255 by
// saturating narrows. Returns the first vertex of the sub-group not done (the scalar tail).
uint32_t ForwardLitNeon(GLOB& glob, const SUBGLOB& sub, uint32_t m, float wAdd, const glm::vec3 dirLocal[4], const glm::vec4 respColor[4],
                        const glm::vec4& constant, const float upper[4], float alphaOne, float alphaScale, const glm::vec4& world,
                        const std::vector<glm::mat4>* pamatBone)
{
    const uint32_t c4 = sub.cvtx & ~3u;
    const size_t cbone = pamatBone ? pamatBone->size() : 0;
    const float32x4_t k127 = vdupq_n_f32(127.0f), zero = vdupq_n_f32(0.0f), cap = vdupq_n_f32(2.0f);
    const float32x4_t vAlphaOne = vdupq_n_f32(alphaOne), vAlphaScale = vdupq_n_f32(alphaScale), vWAdd = vdupq_n_f32(wAdd);
    const float32x4_t cR = vdupq_n_f32(constant.r), cG = vdupq_n_f32(constant.g), cB = vdupq_n_f32(constant.b), cA = vdupq_n_f32(constant.a);
    const float32x4_t wR = vdupq_n_f32(world.r), wG = vdupq_n_f32(world.g), wB = vdupq_n_f32(world.b);
    const uint32_t m0 = Ch(m, 0), m1 = Ch(m, 1), m2 = Ch(m, 2), m3 = Ch(m, 3);
    for (uint32_t k = 0; k < c4; k += 4)
    {
        VTXGLOB* apv[4] = {&glob.avtx[sub.ivtxFirst + k], &glob.avtx[sub.ivtxFirst + k + 1], &glob.avtx[sub.ivtxFirst + k + 2],
                           &glob.avtx[sub.ivtxFirst + k + 3]};
        if (!(apv[0]->fUnbaked | apv[1]->fUnbaked | apv[2]->fUnbaked | apv[3]->fUnbaked)) continue;
        float32x4_t an[4];
        alignas(16) float aIntensity[4], aBase[4][4];
        for (int i = 0; i < 4; ++i)
        {
            const VTXGLOB& v = *apv[i];
            // normal x 127, truncated (the 4th lane is uv.x: unused)
            float32x4_t n = vcvtq_f32_s32(vcvtq_s32_f32(vmulq_f32(vld1q_f32(&v.normal.x), k127)));
            if (pamatBone)
            {
                const float32x4_t nx = vdupq_laneq_f32(n, 0), ny = vdupq_laneq_f32(n, 1), nz = vdupq_laneq_f32(n, 2);
                float32x4_t ns = zero;
                for (int w = 0; w < 4; ++w)
                    if (v.aw[w])
                    {
                        const glm::mat4& b = (*pamatBone)[std::min<size_t>(v.abone[w], cbone - 1)];
                        float32x4_t t = vaddq_f32(vmulq_f32(vld1q_f32(&b[0][0]), nx), vmulq_f32(vld1q_f32(&b[1][0]), ny));
                        t = vaddq_f32(t, vmulq_f32(vld1q_f32(&b[2][0]), nz));
                        ns = vaddq_f32(ns, vmulq_f32(vdupq_n_f32(float(v.aw[w]) / 255.0f), t));
                    }
                n = ns;
            }
            an[i] = n;
            const uint32_t s = v.rgbaSrc;
            aIntensity[i] = s_intensity127.a[Max3(Ch(s, 0), Ch(s, 1), Ch(s, 2))];
            aBase[0][i] = float(uint8_t((Ch(s, 0) * m0 + 255) >> 9));
            aBase[1][i] = float(uint8_t((Ch(s, 1) * m1 + 255) >> 9));
            aBase[2][i] = float(uint8_t((Ch(s, 2) * m2 + 255) >> 9));
            aBase[3][i] = float(glob.fHistoryWorld ? uint8_t(v.rgbaA >> 24) : uint8_t((Ch(s, 3) * m3 + 255) / 510));
        }
        // the x's, the y's and the z's of the four normals
        const float32x4x2_t t01 = vtrnq_f32(an[0], an[1]), t23 = vtrnq_f32(an[2], an[3]);
        const float32x4_t nX = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
        const float32x4_t nY = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
        const float32x4_t nZ = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
        float32x4_t r = cR, g = cG, bl = cB, a = cA;
        for (int j = 0; j < 4; ++j)
        {
            const float32x4_t d = vaddq_f32(vaddq_f32(vmulq_f32(vdupq_n_f32(dirLocal[j].x), nX), vmulq_f32(vdupq_n_f32(dirLocal[j].y), nY)),
                                            vmulq_f32(vdupq_n_f32(dirLocal[j].z), nZ));
            const float32x4_t resp = MinSse(vdupq_n_f32(upper[j]), vaddq_f32(d, vmulq_f32(vmulq_f32(d, d), d)));
            r = vaddq_f32(r, vmulq_f32(vdupq_n_f32(respColor[j].r), resp));
            g = vaddq_f32(g, vmulq_f32(vdupq_n_f32(respColor[j].g), resp));
            bl = vaddq_f32(bl, vmulq_f32(vdupq_n_f32(respColor[j].b), resp));
            a = vaddq_f32(a, vmulq_f32(vdupq_n_f32(respColor[j].a), resp));
        }
        a = vaddq_f32(a, vWAdd);
        // Decompose (cap 2)
        const float32x4_t maximum = MaxSse(bl, MaxSse(g, r));
        const float32x4_t remaining = MaxSse(zero, vsubq_f32(cap, maximum));
        r = MinSse(cap, r);
        g = MinSse(cap, g);
        bl = MinSse(cap, bl);
        const float32x4_t midtone = MinSse(remaining, a);
        const float32x4_t shadow = vsubq_f32(remaining, midtone);
        const float32x4_t alphaB = vmulq_f32(vsubq_f32(vAlphaOne, shadow), vAlphaScale);
        const float32x4_t base3 = vld1q_f32(aBase[3]);
        const float32x4_t correction =
            vreinterpretq_f32_u32(vandq_u32(vcgtq_f32(alphaB, zero), vreinterpretq_u32_f32(vdivq_f32(base3, alphaB))));
        const float32x4_t intensity = vld1q_f32(aIntensity);
        const int32x4_t aR = vcvtq_s32_f32(vmulq_f32(vaddq_f32(r, vmulq_f32(wR, shadow)), intensity));
        const int32x4_t aG = vcvtq_s32_f32(vmulq_f32(vaddq_f32(g, vmulq_f32(wG, shadow)), intensity));
        const int32x4_t aB = vcvtq_s32_f32(vmulq_f32(vaddq_f32(bl, vmulq_f32(wB, shadow)), intensity));
        const int32x4_t bR = vcvtq_s32_f32(vmulq_f32(vmulq_f32(vld1q_f32(aBase[0]), midtone), correction));
        const int32x4_t bG = vcvtq_s32_f32(vmulq_f32(vmulq_f32(vld1q_f32(aBase[1]), midtone), correction));
        const int32x4_t bB = vcvtq_s32_f32(vmulq_f32(vmulq_f32(vld1q_f32(aBase[2]), midtone), correction));
        const int32x4_t bA = vcvtq_s32_f32(alphaB);
        // Pack: clamp(c, 0, 255) (int32 -> int16 -> uint8 saturating), the planes interleaved to RGBA per lane
        auto pack = [](int32x4_t x, int32x4_t y, int32x4_t z, int32x4_t w, uint32_t out[4]) {
            const uint8x8_t xy = vqmovun_s16(vcombine_s16(vqmovn_s32(x), vqmovn_s32(y)));   // r0..3 g0..3
            const uint8x8_t zw = vqmovun_s16(vcombine_s16(vqmovn_s32(z), vqmovn_s32(w)));   // b0..3 a0..3
            const uint8x8x2_t rb = vzip_u8(xy, zw);                                          // r0 b0 r1 b1 .., g0 a0 g1 a1 ..
            const uint8x8x2_t rgba = vzip_u8(rb.val[0], rb.val[1]);                          // r0 g0 b0 a0 r1 ..
            vst1q_u8(reinterpret_cast<uint8_t*>(out), vcombine_u8(rgba.val[0], rgba.val[1]));
        };
        alignas(16) uint32_t aA[4], aB4[4];
        pack(aR, aG, aB, vdupq_n_s32(0), aA);
        pack(bR, bG, bB, bA, aB4);
        for (int i = 0; i < 4; ++i)
        {
            VTXGLOB& v = *apv[i];
            if (!v.fUnbaked) continue;
            v.rgbaA = (aA[i] & 0x00FFFFFF) | (v.rgbaA & 0xFF000000u);
            v.rgbaB = aB4[i];
        }
    }
    return c4;
}
}  // namespace
#define FORWARD_LIT_4 ForwardLitNeon
#else
#define FWD_SSE 0
#endif

// orig: P_G 123-188 (the four lights in the object's local space) and 522-636 (per vertex: cubic response per
// light, capped; illumination = constant + responses; the A/B decomposition with a 2.0 cap).
void ForwardGlobLighting(GLOB& glob, const glm::mat4& matWorld, const glm::vec3& posCenter, const LIGHTSET& lights,
                         const std::vector<glm::mat4>* pamatBone, const LIGHTREC* palSel)
{
    LIGHTREC al[4];
    if (palSel) std::copy(palSel, palSel + 4, al);
    else SelectLights(lights, posCenter, al);
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
    // (perf-3) one vertex as P_G 522-636 computes it; the 4-wide version below does the same IEEE operations in the
    // same order per lane (no FMA: -ffp-contract=off), so the same colours bit for bit. SLY2N_FWDSCALAR=1: this one
    // for every vertex; SLY2N_FWDCHECK=1: both, and prints any difference.
    const size_t cbone = fSkin ? pamatBone->size() : 0;
    auto litScalar = [&](VTXGLOB& v, uint32_t m, float shadow1212) {
        // 0x16A000's render items: normal x 127, intensity = the colour's largest channel / 255 x 127,
        // colour = vertex x material / 512 (alpha (a * m + 255) / 510).
        glm::vec3 n(float(int(v.normal.x * 127.0f)), float(int(v.normal.y * 127.0f)), float(int(v.normal.z * 127.0f)));
        if (fSkin)
        {
            // P_G 808: the weighted sum of the bones' rotations of the normal (the bone list as glob.vert reads it)
            glm::vec3 ns(0.0f);
            for (int w = 0; w < 4; ++w)
                if (v.aw[w]) ns += (float(v.aw[w]) / 255.0f) * (glm::mat3((*pamatBone)[std::min<size_t>(v.abone[w], cbone - 1)]) * n);
            n = ns;
        }
        const float intensity = float(int(float(Max3(Ch(v.rgbaSrc, 0), Ch(v.rgbaSrc, 1), Ch(v.rgbaSrc, 2))) / 255.0f * 127.0f));
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
    };
    static const bool s_fScalar = std::getenv("SLY2N_FWDSCALAR") != nullptr;
    static const bool s_fCheck = std::getenv("SLY2N_FWDCHECK") != nullptr;
    static thread_local std::vector<uint32_t> s_aA;   // (per thread: scene.cpp lights groups on the pass helpers)
    if (s_fCheck)
    {
        s_aA.resize(glob.avtx.size());
        for (size_t i = 0; i < glob.avtx.size(); ++i) s_aA[i] = glob.avtx[i].rgbaA;
    }
    for (const SUBGLOB& sub : glob.asubglob)
    {
        const SHD* pshd = sub.pshd;
        const uint32_t m = pshd ? pshd->argba[0] : 0x80808080u;
        const float shadow1212 = pshd ? float(int16_t(pshd->unSelfIllum)) / 4096.0f : 0.0f;
        uint32_t k = 0;
#if FWD_SSE
        if (!s_fScalar)
            k = FORWARD_LIT_4(glob, sub, m, shadow1212 + lights.worldShader.w, dirLocal, respColor, constant, upper, alphaOne, alphaScale,
                              lights.worldShader, fSkin ? pamatBone : nullptr);
#endif
        for (; k < sub.cvtx; ++k)
        {
            VTXGLOB& v = glob.avtx[sub.ivtxFirst + k];
            if (!v.fUnbaked) continue;
            litScalar(v, m, shadow1212);
        }
    }
    if (s_fCheck)
    {
        int cdiff = 0;
        for (const SUBGLOB& sub : glob.asubglob)
        {
            const SHD* pshd = sub.pshd;
            const uint32_t m = pshd ? pshd->argba[0] : 0x80808080u;
            const float shadow1212 = pshd ? float(int16_t(pshd->unSelfIllum)) / 4096.0f : 0.0f;
            for (uint32_t k = 0; k < sub.cvtx; ++k)
            {
                VTXGLOB v = glob.avtx[sub.ivtxFirst + k];
                if (!v.fUnbaked) continue;
                v.rgbaA = s_aA[sub.ivtxFirst + k];
                litScalar(v, m, shadow1212);
                const VTXGLOB& w = glob.avtx[sub.ivtxFirst + k];
                if ((v.rgbaA != w.rgbaA || v.rgbaB != w.rgbaB) && cdiff++ < 4)
                    std::printf("fwdcheck: vertex %u: scalar %08X %08X, sse %08X %08X\n", sub.ivtxFirst + k, v.rgbaA, v.rgbaB, w.rgbaA, w.rgbaB);
            }
        }
        static thread_local long s_cglob = 0, s_cdiff = 0;
        ++s_cglob;
        if (cdiff) ++s_cdiff;
        if ((s_cglob & 1023) == 0 || cdiff) std::printf("fwdcheck: %ld groups, %ld differ\n", s_cglob, s_cdiff);
    }
}

// orig: 0x001A3F78: the four strongest candidates at pos (strength gConst + gSlope x distance), strongest first
// (an equal strength before), never a negative one.
static void SelectStrongest(const LIGHTSET& lights, const glm::vec3& pos, float ag[4], const LIGHTREC* apl[4], const LIGHTSEL* apsel[4])
{
    constexpr int n = 4;
    for (const auto& [rec, sel] : lights.actx)
    {
        // (a strength that can only fall with the distance (gSlope <= 0) is at most gConst: below the fourth kept, the
        // test below fails without the distance's square root)
        if (sel.gSlope <= 0.0f && sel.gConst < ag[n - 1]) continue;
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
        if (apl[i]->kind == 0)
        {
            gSum += ag[i];
            acc += (pos - apl[i]->dirOrPos * gDirDist) * ag[i];
        }
        else if (apl[i]->kind == 1)
        {
            gSum += ag[i];
            acc += apl[i]->dirOrPos * ag[i];
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
