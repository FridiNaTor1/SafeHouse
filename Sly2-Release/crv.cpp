// The path curves (see crv.h). Each function follows the reference's port named in its comment.
#include "crv.h"

#include "cls.h"
#include "brx.h"
#include "lo.h"
#include "vecmat.h"
#include "xfmath.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>

using cmm::F32;

namespace
{
std::map<const LO*, std::unique_ptr<CRV>> s_mpcrv;


glm::vec3 Point(const glm::mat4& m, const glm::vec3& v) { return glm::vec3(m * glm::vec4(v, 1.0f)); }
glm::vec3 Dir(const glm::mat4& m, const glm::vec3& v) { return glm::mat3(m) * v; }

// orig 0x00117120 EvalHermiteVector (position, velocity x dtScale)
void Hermite(const glm::vec3& p0, const glm::vec3& v0, const glm::vec3& p1, const glm::vec3& v1, glm::vec3* ppos,
             glm::vec3* pvel, float dt, float t, float dtScale)
{
    const float u = t / dt, w = 1.0f - u;
    if (ppos)
    {
        const float uw = u * w * w, u2w = u * u * w;
        const float a = w * w * w + uw * 3.0f, b = uw * dt, c = -u2w * dt, d = u * u * u + u2w * 3.0f;
        *ppos = p0 * a + v0 * b + v1 * c + p1 * d;
    }
    if (pvel)
    {
        const float a = (u * (6.0f - u * 6.0f)) / dt, b = 1.0f - u * (4.0f - u * 3.0f), c = u * (u * 3.0f + -2.0f);
        *pvel = ((p1 - p0) * a + v0 * b + v1 * c) * dtScale;
    }
}

// orig 0x001E7538 (the segment of parameter g, from the last one found; *pdg the offset into it, *pdgSeg its span)
int ISegCrvG(const CRV* p, float g, float* pdg, float* pdgSeg)
{
    g = GWrapCrv(p, g);
    int i = p->iseg;
    const int n = static_cast<int>(p->ag.size()) - 1;
    if (i < 0) i = 0;
    if (i > n) i = n;
    if (p->ag[i] < g)
    {
        while (i < n)
        {
            if (g < p->ag[i]) break;
            ++i;
        }
        --i;
    }
    else
        while (i > 0)
        {
            if (p->ag[i] <= g) break;
            --i;
        }
    if (i < 0) i = 0;
    if (pdg) *pdg = g - p->ag[i];
    if (pdgSeg) *pdgSeg = p->ag[i + 1] - p->ag[i];
    p->iseg = i;
    return i;
}

// orig 0x001E7668 (by distance)
int ISegCrvS(const CRV* p, float s, float* pds, float* pdsSeg)
{
    s = SWrapCrv(p, s);
    int i = p->iseg;
    const int n = static_cast<int>(p->as.size()) - 1;
    if (i < 0) i = 0;
    if (i > n) i = n;
    if (p->as[i] < s)
    {
        for (; i < n; ++i)
            if (s < p->as[i]) break;
        --i;
    }
    else
        for (; i > 0; --i)
            if (p->as[i] <= s) break;
    if (i < 0) i = 0;
    if (pds) *pds = s - p->as[i];
    if (pdsSeg) *pdsSeg = p->as[i + 1] - p->as[i];
    p->iseg = i;
    return i;
}

// orig 0x001E5FC8 (the 11 lengths along cubic segment i at tenths of its span)
void CacheSegCrv(const CRV* p, int i)
{
    if (i == p->isegCached) return;
    const glm::vec3 vIn = Dir(p->mat, p->avIn[i + 1]);
    const glm::vec3 end = Point(p->mat, p->apos[i + 1]);
    const glm::vec3 start = Point(p->mat, p->apos[i]);
    const glm::vec3 vOut = Dir(p->mat, p->avOut[i]);
    const float dt = p->ag[i + 1] - p->ag[i];
    p->asSub[0] = 0.0f;
    glm::vec3 prev(0.0f);
    for (int j = 0; j < 11; ++j)
    {
        glm::vec3 cur;
        Hermite(start, vOut, end, vIn, &cur, nullptr, dt, (static_cast<float>(j) * F32(0x3DCCCCCD)) * dt, 1.0f);
        if (j > 0) p->asSub[j] = p->asSub[j - 1] + glm::length(prev - cur);
        prev = cur;
    }
    p->isegCached = i;
}
}  // namespace

// orig: 0x001E6180
void RecalcCrv(CRV* p, const glm::mat4& mat)
{
    p->iseg = 0;  // 0x1E6330
    p->isegCached = -1;
    p->mat = mat;
    const size_t c = p->apos.size();
    p->as.assign(c, 0.0f);
    if (p->crvk == 0)
    {
        float total = 0.0f;
        glm::vec3 prev = Point(mat, p->apos[0]);
        for (size_t i = 1; i < c; ++i)
        {
            const glm::vec3 cur = Point(mat, p->apos[i]);
            total += glm::length(prev - cur);
            prev = cur;
            p->as[i] = total;
        }
    }
    else
        for (size_t j = 0; j + 1 < c; ++j)
        {
            CacheSegCrv(p, static_cast<int>(j));
            p->as[j + 1] = p->as[j] + p->asSub[10];  // (+0x174: the last sub-length)
        }
}

// 0x231440 (LOX vt slot 31; 0x231368): the matrix in its reference record's frame
glm::mat4 MatWorldLox(const LO* plo)
{
    const LOX* plox = static_cast<const LOX*>(plo);
    return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
}

void InvalidateCrvOfLo(const LO* plo) { s_mpcrv.erase(plo); }

// orig: 0x001BC570's reversal (a clockwise blocking polygon at its post-load): the points swapped end for end, the
// parameters mirrored on the last one (0x1E6630), the curve rebuilt
void ReverseLoxPath(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x0A)) return;
    LOXPATH* ppath = static_cast<LOXPATH*>(plo);
    const int c = static_cast<int>(ppath->avecF4.size());
    const bool fParams = static_cast<int>(ppath->an.size()) == c && c > 0;
    float t = 0.0f;
    if (fParams) std::memcpy(&t, &ppath->an[c - 1], 4);
    for (int i = 0; i < c / 2; ++i)
    {
        const int j = c - i - 1;
        std::swap(ppath->avecF4[i], ppath->avecF4[j]);
        if (!fParams) continue;
        float gi, gj;
        std::memcpy(&gi, &ppath->an[i], 4);
        std::memcpy(&gj, &ppath->an[j], 4);
        const float ni = t - gj, nj = t - gi;
        std::memcpy(&ppath->an[i], &ni, 4);
        std::memcpy(&ppath->an[j], &nj, 4);
    }
    // (an existing curve rebuilt in place: its pointer may be held)
    auto it = s_mpcrv.find(plo);
    if (it == s_mpcrv.end() || !it->second) return;
    CRV& crv = *it->second;
    crv.ag.resize(ppath->an.size());
    for (size_t i = 0; i < ppath->an.size(); ++i) std::memcpy(&crv.ag[i], &ppath->an[i], 4);
    crv.apos = ppath->avecF4;
    crv.iseg = 1;
    crv.isegCached = -1;
    RecalcCrv(&crv, MatWorldLox(plo));
}

// orig: 0x1E5D80 initializes the cache in each path object. The native side
// cache must have that same lifetime, including when a later world reuses storage.
LOXPATH::~LOXPATH() { InvalidateCrvOfLo(this); }

const CRV* PcrvFromLo(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x0A)) return nullptr;
    const LOXPATH* ppath = static_cast<const LOXPATH*>(plo);
    if (ppath->avecF4.size() < 2) return nullptr;
    auto& pcrv = s_mpcrv[plo];
    const glm::mat4 mat = MatWorldLox(plo);
    if (!pcrv || pcrv->ppath != ppath)
    {
        pcrv = std::make_unique<CRV>();
        CRV& c = *pcrv;
        c.ppath = ppath;
        c.crvk = ppath->fE0;
        c.fClosed = ppath->nE4;
        c.ag.resize(ppath->an.size());
        for (size_t i = 0; i < ppath->an.size(); ++i) std::memcpy(&c.ag[i], &ppath->an[i], 4);
        c.apos = ppath->avecF4;
        c.avIn = ppath->avec140;
        c.avOut = ppath->avec144;
        if (c.crvk && (c.avIn.size() != c.apos.size() || c.avOut.size() != c.apos.size())) c.crvk = 0;
        c.iseg = 1;  // (0x1E5D80: last segment 1)
        RecalcCrv(&c, mat);
    }
    else if (pcrv->mat != mat) RecalcCrv(pcrv.get(), mat);
    return pcrv.get();
}

// orig: 0x001E63C0
float SFromCrvG(const CRV* p, float g)
{
    float dg, dgSeg;
    const int i = ISegCrvG(p, g, &dg, &dgSeg);
    if (p->crvk == 0)
    {
        // (the EE's rounding toward zero: on a mode-3 rope's flat segments (rope.cpp, the rest steps [0, 300, 300, ..])
        // the lerp of two equal distances comes out a hair under them (300 -> 299.99997 for g 3.02), and that decides
        // which segment the distance falls in again (0x1E7668 stops before the first 300): the jump onto the Thunder
        // Beak line (R04) kept its target near Sly as in the reference (sTarget 299.99997), instead of the line's far
        // end, which gave the jump up as too fast)
        const float t = DivEe(dg, dgSeg);
        return AddRz(MulRz(AddRz(1.0f, -t), p->as[i]), MulRz(t, p->as[i + 1]));
    }
    CacheSegCrv(p, i);
    const float u = (dg * 10.0f) / dgSeg;
    int j = static_cast<int>(u);
    if (j > 9) j = 9;
    if (j < 0) j = 0;
    const float f = u - static_cast<float>(j);
    return ((1.0f - f) * p->asSub[j] + f * p->asSub[j + 1]) + p->as[i];
}

// orig: 0x001E64C8
float GFromCrvS(const CRV* p, float s)
{
    float ds, dsSeg;
    const int i = ISegCrvS(p, s, &ds, &dsSeg);
    if (p->crvk == 0)
    {
        // (the EE's div.s / mul.s: a zero-length segment (a mode-3 rope's rest steps, rope.cpp) gives 0 / 0 = FLT_MAX
        // and the products saturate instead of IEEE's NaN: R04, the Thunder Beak line's NaN that blacked the screen)
        const float t = DivEe(ds, dsSeg);
        return AddEe(MulEe(1.0f - t, p->ag[i]), MulEe(t, p->ag[i + 1]));
    }
    CacheSegCrv(p, i);
    int j = 1;
    for (; j < 10; ++j)
        if (ds < p->asSub[j]) break;
    --j;
    const float f = (ds - p->asSub[j]) / (p->asSub[j + 1] - p->asSub[j]);
    const float u = (static_cast<float>(j) + f) * F32(0x3DCCCCCD);
    return (1.0f - u) * p->ag[i] + u * p->ag[i + 1];
}

// orig: 0x001E6660
float GWrapCrv(const CRV* p, float g)
{
    if (!p->fClosed) return g;
    const float gMin = p->ag.front(), gMax = p->ag.back();
    if (gMin <= g && g <= gMax) return g;
    return gMin + cmm::GModPositive(g - gMin, gMax - gMin);
}

// orig: 0x001E66C8
float SWrapCrv(const CRV* p, float s)
{
    if (!p->fClosed) return s;
    const float sMax = p->as.back();
    if (0.0f <= s && s <= sMax) return s;
    return cmm::GModPositive(s, sMax);
}

float SLengthCrv(const CRV* p) { return p->as.back(); }

// orig: 0x001E67B8
void EvalCrv(const CRV* p, float g, glm::vec3* ppos, glm::vec3* pdir)
{
    float dg, dgSeg;
    const int i = ISegCrvG(p, g, &dg, &dgSeg);
    if (p->crvk == 0)
    {
        if (ppos)
        {
            const float t = dg / dgSeg;
            *ppos = Point(p->mat, p->apos[i] * (1.0f - t) + p->apos[i + 1] * t);
        }
        if (pdir) *pdir = glm::normalize(Dir(p->mat, p->apos[i + 1] - p->apos[i]));
        return;
    }
    glm::vec3 pos, vel;
    Hermite(p->apos[i], p->avOut[i], p->apos[i + 1], p->avIn[i + 1], &pos, &vel, dgSeg, dg, 1.0f);
    if (ppos) *ppos = Point(p->mat, pos);
    if (pdir) *pdir = glm::normalize(Dir(p->mat, vel));
}

// orig: 0x001E6F10
void EvalCrvS(const CRV* p, float s, glm::vec3* ppos, glm::vec3* pdir) { EvalCrv(p, GFromCrvS(p, s), ppos, pdir); }

// orig: 0x001E6ED8 (0x1E6A30 / 0x1E6DB8 with the filter (0x1FA970 kind 1: along the axis dropped))
void NearestCrv(const CRV* p, const glm::vec3& q, float g, glm::vec3* ppos, glm::vec3* pdir, float* pg, float* ps,
                const glm::vec3* paxisFlat)
{
    auto flat = [&](const glm::vec3& v) { return paxisFlat ? v - *paxisFlat * glm::dot(v, *paxisFlat) : v; };
    float gRes;
    if (p->crvk == 0)
    {
        // orig 0x001E6A30: walk the segments from the one at g toward the projection of q
        float dgUnused, dgSegUnused;
        int i = ISegCrvG(p, g, &dgUnused, &dgSegUnused);
        const int c = static_cast<int>(p->apos.size());
        bool fFwd = false, fBack = false;
        float t = 0.0f;
        for (int n = 0; n < 4 * c + 8; ++n)
        {
            const glm::vec3 p0 = p->apos[i];
            const glm::vec3 dir = flat(Dir(p->mat, p->apos[i + 1] - p0));
            const glm::vec3 pt = Point(p->mat, p0);
            const glm::vec3 d = flat(q - pt);
            t = 0.0f;
            const float len = glm::length(dir);
            const float proj = glm::dot(dir * (1.0f / len), d);
            if (proj < 0.0f)
            {
                if (fFwd)
                {
                    t = 1.0f;
                    break;
                }
                if (i > 0)
                {
                    --i;
                    fBack = true;
                    continue;
                }
                if (!p->fClosed) break;
                i = c - 2;
                fBack = true;
                continue;
            }
            if (!(len < proj))
            {
                t = proj / len;
                break;
            }
            if (fBack) break;
            if (i < c - 2)
            {
                ++i;
                fFwd = true;
                continue;
            }
            if (p->fClosed)
            {
                i = 0;
                fFwd = true;
                continue;
            }
            t = 1.0f;
            break;
        }
        if (ppos) *ppos = Point(p->mat, p->apos[i] * (1.0f - t) + p->apos[i + 1] * t);
        if (pdir) *pdir = glm::normalize(Dir(p->mat, p->apos[i + 1] - p->apos[i]));
        gRes = (1.0f - t) * p->ag[i] + t * p->ag[i + 1];
    }
    else
    {
        // orig 0x001E6DB8: MinimizeRange of the squared distance over the curve's range
        float gMin = p->ag.front();
        const float dg = (p->ag[1] - gMin) * F32(0x3DCCCCCD);
        float gMax = p->ag.back();
        if (p->fClosed)
        {
            gMin = -F32(0x7F7FFFFF);
            gMax = F32(0x7F7FFFFF);
        }
        float gBest;
        MinimizeRange([&](float x) {
            glm::vec3 pos;
            EvalCrv(p, x, &pos, nullptr);
            const glm::vec3 d = flat(q - pos);
            return glm::dot(d, d);
        }, g, dg, gMin, gMax, &gBest, nullptr);
        gRes = GWrapCrv(p, gBest);
        if (ppos || pdir) EvalCrv(p, gRes, ppos, pdir);
    }
    if (pg) *pg = gRes;
    if (ps) *ps = SFromCrvG(p, gRes);
}

// orig: 0x001E7380
void NearestCrvS(const CRV* p, const glm::vec3& q, float s, glm::vec3* ppos, glm::vec3* pdir, float* pg, float* ps,
                 const glm::vec3* paxisFlat)
{
    NearestCrv(p, q, GFromCrvS(p, s), ppos, pdir, pg, ps, paxisFlat);
}

// orig: 0x001E6F60
void NearestCrvAll(const CRV* p, const glm::vec3& q, glm::vec3* ppos, glm::vec3* pdir, float* pg, float* ps,
                   const glm::vec3* paxisFlat)
{
    auto flat = [&](const glm::vec3& v) { return paxisFlat ? v - *paxisFlat * glm::dot(v, *paxisFlat) : v; };
    const int c = static_cast<int>(p->apos.size());
    if (p->crvk == 0)
    {
        float best = F32(0x7F7FFFFF), gBest = -1.0f;
        glm::vec3 prev = Point(p->mat, p->apos[0]);
        for (int i = 1; i < c; ++i)
        {
            const glm::vec3 cur = Point(p->mat, p->apos[i]);
            const glm::vec3 seg = flat(cur - prev);
            float t = glm::dot(flat(q - prev), seg) / glm::dot(seg, seg);
            if (t < 0.0f) t = 0.0f;
            else if (1.0f < t) t = 1.0f;
            const float u = 1.0f - t;
            const glm::vec3 pt = prev * u + cur * t;
            const float l = glm::length(flat(q - pt));
            if (l < best)
            {
                best = l;
                gBest = u * p->ag[i - 1] + t * p->ag[i];
                if (ppos) *ppos = pt;
                if (pdir) *pdir = glm::normalize(cur - prev);
            }
            prev = cur;
        }
        if (pg) *pg = gBest;
        if (ps) *ps = SFromCrvG(p, gBest);
        return;
    }
    float best = F32(0x7F7FFFFF);
    int iBest = 0;
    for (int i = 0; i < c; ++i)
    {
        const float l = glm::length(flat(q - Point(p->mat, p->apos[i])));
        if (l < best)
        {
            best = l;
            iBest = i;
        }
    }
    NearestCrv(p, q, p->ag[iBest], ppos, pdir, pg, ps, paxisFlat);
}
