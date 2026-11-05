// Sly 3's animation tracks (trk3.h), re-derived from the Sly 3 asm.

#include "trk3.h"

#include "../lo.h"
#include "../sm.h"
#include "../xfmath.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sly3
{
namespace
{
constexpr float kRecip60 = 1.0f / 60.0f;  // 0x3C888889
constexpr float kEpsNear = 1e-4f;         // 0x38D1B717

bool FNear3(const glm::vec4& a, const glm::vec4& b)
{
    return FFloatsNear(a.x, b.x, kEpsNear) && FFloatsNear(a.y, b.y, kEpsNear) && FFloatsNear(a.z, b.z, kEpsNear);
}

// The tail of every keyed Setup (0x12D508..0x12D634): +0x9A 2 when the segment is one point with zero slopes, else 1
void SetSegmentKind3(XF* pxf)
{
    const glm::vec4 zero(0.0f);
    bool f = FNear3(pxf->aseg[0], pxf->aseg[2]);
    if (f) f = FNear3(zero, pxf->aseg[1]);
    if (f) f = FNear3(zero, pxf->aseg[3]);
    pxf->state = f ? 2 : 1;
}

float GFrames(const ASEGTIMES3& times, int i) { return float(times.aframe[std::size_t(i)]) * kRecip60; }

struct TABLES3
{
    std::array<float, 3> agScale{0.0625f, 1.0f / 1024.0f, 1.0f / 1024.0f};
    std::array<std::array<float, 16>, 3> aagTan{};
    TABLES3()
    {
        const std::array<float, 3> agMul{60.0f, 10.0f, 10.0f};  // 0x31F690
        for (int s = 0; s < 3; ++s)
            for (int i = 0; i < 16; ++i)
                aagTan[std::size_t(s)][std::size_t(i)] = std::tan(float(i) * 0.09817477f) * agMul[std::size_t(s)];
    }
};
const TABLES3& Tables()
{
    static const TABLES3 s_tables;
    return s_tables;
}

// A packed tangent: 5 bits per axis from bit 0 (x)
glm::vec3 VecTan3(int s8, uint16_t u)
{
    glm::vec3 v;
    for (int j = 0; j < 3; ++j, u >>= 5)
    {
        const float g = GTanTrk3(s8, u & 0xF);
        v[j] = (u & 0x10) ? -g : g;
    }
    return v;
}
}  // namespace

// The keys i - 1 and i of IkeyFind3, kept inside a list of fewer than two keys (the original reads past its end)
static void KeysAround3(const ASEGTIMES3& times, float g, int* pi0, int* pi1)
{
    const int c = int(times.aframe.size());
    const int i = IkeyFind3(times, g);
    *pi1 = std::max(0, std::min(i, c - 1));
    *pi0 = std::max(0, std::min(i - 1, c - 1));
}

float GScaleTrk3(int s8) { return Tables().agScale.at(std::size_t(s8)); }
float GTanTrk3(int s8, int i) { return Tables().aagTan.at(std::size_t(s8)).at(std::size_t(i)); }

// orig3: 0x12D0F0
int IkeyFind3(const ASEGTIMES3& times, float g)
{
    const int frames = int(g * 60.0f);  // cvt.w.s: toward zero
    const int c = int(times.aframe.size());
    int i = 1;
    while (i < c - 1 && !(frames < int(times.aframe[std::size_t(i)]))) ++i;
    return i;
}

// orig3: 0x12D6F8 / 0x12DCC0 (the segment around g: pg0 / pg1 its times, p0 / v0 the start's value / slope, p1 / v1
// the end's)
void TRKP3V::Fetch(float g, float* pg0, float* pg1, glm::vec4* pp0, glm::vec4* pp1, glm::vec4* pv0,
                   glm::vec4* pv1) const
{
    int i0 = 0, i = 0;
    KeysAround3(*ptimes, g, &i0, &i);
    *pg0 = GFrames(*ptimes, i0);
    *pg1 = GFrames(*ptimes, i);
    int ckeyed = 3;
    std::size_t ig = 0;
    for (int j = 0, grfC = ~grf & 7; grfC; ++j, grfC >>= 1)
        if (grfC & 1)
        {
            --ckeyed;
            (*pp0)[j] = (*pp1)[j] = ag[ig++];
            (*pv0)[j] = (*pv1)[j] = 0.0f;
        }
    const float* k1 = ag.data() + ig + std::size_t(i * ckeyed * cfKey);
    const float* k0 = ag.data() + ig + std::size_t(i0 * ckeyed * cfKey);
    for (int j = 0, grfK = grf; grfK; ++j, grfK >>= 1)
        if (grfK & 1)
        {
            if (cfKey == 3)
            {
                (*pv0)[j] = k0[1];
                (*pp0)[j] = k0[2];
                (*pv1)[j] = k1[0];
                (*pp1)[j] = k1[2];
            }
            else
            {
                (*pp0)[j] = k0[1];
                (*pv0)[j] = k0[0];
                (*pp1)[j] = k1[1];
                (*pv1)[j] = k1[0];
            }
            k0 += cfKey;
            k1 += cfKey;
        }
}

// orig3: 0x12D438 / 0x12DA00
void TRKP3V::Setup(XF* pxf) const
{
    Fetch(pxf->G(), &pxf->g0, &pxf->g1, &pxf->aseg[0], &pxf->aseg[2], &pxf->aseg[1], &pxf->aseg[3]);
    SetSegmentKind3(pxf);
}

// orig3: 0x12D660 / 0x12DC28 (0x118558 = EvalHermiteVector, no acceleration)
void TRKP3V::Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const
{
    float g0 = 0.0f, g1 = 0.0f;
    glm::vec4 p0(0.0f), p1(0.0f), v0(0.0f), v1(0.0f);
    Fetch(g, &g0, &g1, &p0, &p1, &v0, &v1);
    EvalHermiteVector(p0, v0, p1, v1, ppos, pvel, nullptr, g1 - g0, g - g0, dtScale);
}

// orig3: 0x12E490 / 0x12EBB0
void TRKP3P::Fetch(float g, float* pg0, float* pg1, glm::vec4* pp0, glm::vec4* pp1, glm::vec4* pv0,
                   glm::vec4* pv1) const
{
    int i0 = 0, i = 0;
    KeysAround3(*ptimes, g, &i0, &i);
    *pg0 = GFrames(*ptimes, i0);
    *pg1 = GFrames(*ptimes, i);
    std::array<int16_t, 3> an0{}, an1{};
    int stride = 3 + cu16;  // halfwords per key: cu16 + the keyed axes
    std::size_t iu = 0;
    for (int j = 0, grfC = ~grf & 7; grfC; ++j, grfC >>= 1)
        if (grfC & 1)
        {
            --stride;
            an0[std::size_t(j)] = an1[std::size_t(j)] = int16_t(an[iu++]);
        }
    const uint16_t* k1 = an.data() + iu + std::size_t(i * stride);
    const uint16_t* k0 = an.data() + iu + std::size_t(i0 * stride);
    for (int j = 0, grfK = grf, ik = 0; grfK; ++j, grfK >>= 1)
        if (grfK & 1)
        {
            an0[std::size_t(j)] = int16_t(k0[cu16 + ik]);
            an1[std::size_t(j)] = int16_t(k1[cu16 + ik]);
            ++ik;
        }
    const glm::vec3 vOut = VecTan3(s8, cu16 == 2 ? k0[1] : k0[0]);
    const glm::vec3 vIn = VecTan3(s8, k1[0]);
    for (int j = 0; j < 3; ++j)
    {
        (*pv0)[j] = vOut[j];
        (*pv1)[j] = vIn[j];
    }
    const float gScale = GScaleTrk3(s8);
    for (int j = 0; j < 3; ++j)
    {
        (*pp0)[j] = float(an0[std::size_t(j)]) * gScale;
        (*pp1)[j] = float(an1[std::size_t(j)]) * gScale;
    }
}

// orig3: 0x12E1D0 / 0x12E8F0
void TRKP3P::Setup(XF* pxf) const
{
    Fetch(pxf->G(), &pxf->g0, &pxf->g1, &pxf->aseg[0], &pxf->aseg[2], &pxf->aseg[1], &pxf->aseg[3]);
    SetSegmentKind3(pxf);
}

// orig3: 0x12E3F8 / 0x12EB18
void TRKP3P::Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const
{
    float g0 = 0.0f, g1 = 0.0f;
    glm::vec4 p0(0.0f), p1(0.0f), v0(0.0f), v1(0.0f);
    Fetch(g, &g0, &g1, &p0, &p1, &v0, &v1);
    EvalHermiteVector(p0, v0, p1, v1, ppos, pvel, nullptr, g1 - g0, g - g0, dtScale);
}

// orig3: 0x12F120
void TRKG3K::Eval(float* pg, float* pdg, float t, float dtScale) const
{
    int i0 = 0, i = 0;
    KeysAround3(*ptimes, t, &i0, &i);
    const float g0 = GFrames(*ptimes, i0);
    const float g1 = GFrames(*ptimes, i);
    const float* k1 = ag.data() + std::size_t(i) * 3;
    const float* k0 = ag.data() + std::size_t(i0) * 3;
    EvalHermite(pg, pdg, g1 - g0, t - g0, dtScale, k0[0], k0[2], k1[0], k1[1]);
}

std::unique_ptr<TRKP> PtrkpFromAseg3(const ASEG3* paseg, const ASEGTRK3& trk)
{
    switch (trk.kind)
    {
    case 0: case 1: case 2: case 3:
    {
        auto p = std::make_unique<TRKPC>();
        p->kind = trk.kind;
        if (trk.kind == 1) p->pos = glm::vec3(1.0f);
        else if (trk.kind == 2) p->pos = glm::vec3(trk.ag[0], trk.ag[1], trk.ag[2]);
        else if (trk.kind == 3)
            for (int j = 0; j < 3; ++j) p->pos[j] = float(trk.an[std::size_t(j)]) * GScaleTrk3(trk.s8);
        return p;
    }
    case 4: case 5:
    {
        auto p = std::make_unique<TRKP3V>();
        p->kind = trk.kind;
        p->ptimes = &paseg->atimes.at(std::size_t(trk.itimes));
        p->grf = trk.grf;
        p->cfKey = trk.kind == 4 ? 3 : 2;
        p->ag = trk.ag;
        return p;
    }
    case 6: case 7:
    {
        auto p = std::make_unique<TRKP3P>();
        p->kind = trk.kind;
        p->ptimes = &paseg->atimes.at(std::size_t(trk.itimes));
        p->grf = trk.grf;
        p->s8 = trk.s8;
        p->cu16 = trk.kind == 6 ? 2 : 1;
        // as stored: the unkeyed S16, then per key the U16 and the keyed S16 (aseg3 keeps them apart)
        const int ckeyed = __builtin_popcount(unsigned(trk.grf) & 7);
        const int cunkeyed = 3 - ckeyed;
        std::size_t in = 0, iu = 0;
        for (int i = 0; i < cunkeyed; ++i) p->an.push_back(uint16_t(trk.an.at(in++)));
        const std::size_t ckey = p->ptimes->aframe.size();
        for (std::size_t k = 0; k < ckey; ++k)
        {
            for (int i = 0; i < p->cu16; ++i) p->an.push_back(trk.au.at(iu++));
            for (int i = 0; i < ckeyed; ++i) p->an.push_back(uint16_t(trk.an.at(in++)));
        }
        return p;
    }
    default: return nullptr;  // -1: none
    }
}

std::unique_ptr<TRKG> PtrkgFromAseg3(const ASEG3* paseg, const ASEGTRK3& trk)
{
    switch (trk.kind)
    {
    case 0:
    {
        auto p = std::make_unique<TRKGC>();
        p->kind = 0;
        p->g = trk.ag.at(0);
        return p;
    }
    case 1:
    {
        auto p = std::make_unique<TRKG3K>();
        p->kind = 1;
        p->ptimes = &paseg->atimes.at(std::size_t(trk.itimes));
        p->ag = trk.ag;
        return p;
    }
    case 2:
    {
        auto p = std::make_unique<TRKGN>();
        p->kind = 2;
        return p;
    }
    default: return nullptr;
    }
}

void BuildAsegd3(ASEG3* paseg)
{
    ASEGD& d = paseg->asegd;
    d.tMax = paseg->tMax;
    d.achn.clear();
    for (const ASEGCHN3& c : paseg->achn)
    {
        ASEGCHN chn;
        chn.oid = c.oid;
        chn.isub = c.isub;
        chn.fDisabled = c.grf != 0;  // bit 30 (0x136334: the U8 non-zero)
        // bit 31: no position track (-1) or a scalar track read (it replaces the position track, +4)
        chn.fScalar = c.trkp.kind == -1 || c.trkg.kind != -1;
        if (c.trkg.kind != -1) chn.ptrkg = PtrkgFromAseg3(paseg, c.trkg);
        else chn.ptrkp = PtrkpFromAseg3(paseg, c.trkp);
        d.achn.push_back(std::move(chn));
    }
    d.achg.clear();
    for (const ASEGCHG3& c : paseg->achg)
    {
        ASEGCHG chg;
        chg.kind = c.kind;
        chg.n = c.n;
        chg.ptrkg = PtrkgFromAseg3(paseg, c.trkg);
        d.achg.push_back(std::move(chg));
    }
    for (std::size_t l = 0; l < 3; ++l)
    {
        d.aaev[l].clear();
        for (const ASEGEV3& e : paseg->aaev[l])
        {
            ASEGEV ev;
            ev.t = e.t;
            ev.kind = e.kind;
            ev.an = e.an;
            ev.ag = e.ag;
            ev.ab = e.ab;
            d.aaev[l].push_back(ev);
        }
    }
}
}  // namespace sly3
