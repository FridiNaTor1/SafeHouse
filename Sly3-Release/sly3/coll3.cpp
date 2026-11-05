// See coll3.h.

#include "coll3.h"

#include "scene3.h"

#include "../bsp.h"
#include "../lo.h"
#include "../so.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace sly3
{
namespace
{
constexpr float c_sCell = 512.0f;
struct ENT
{
    const SOE* psoe = nullptr;
    int oid = -1;
    glm::vec3 posMin{0.0f}, posMax{0.0f};
};
std::vector<ENT> s_aent;
std::unordered_map<int64_t, std::vector<int>> s_mpcell;
std::vector<int> s_aientHuge;   // entries over too many cells (sea planes, ...): tested always
std::vector<uint32_t> s_amark;
uint32_t s_nMark = 0;

int64_t Key(int ix, int iy) { return (int64_t(ix) << 32) ^ uint32_t(iy); }
int ICell(float g) { return int(std::floor(g / c_sCell)); }

// the entries whose bounds the box (a, b) may touch, each once
template <class F> void ForEntries(const glm::vec3& a, const glm::vec3& b, F f)
{
    const glm::vec3 lo = glm::min(a, b), hi = glm::max(a, b);
    if (++s_nMark == 0) { std::fill(s_amark.begin(), s_amark.end(), 0); s_nMark = 1; }
    auto visit = [&](int i) {
        if (s_amark[size_t(i)] == s_nMark) return;
        s_amark[size_t(i)] = s_nMark;
        const ENT& e = s_aent[size_t(i)];
        if (hi.x < e.posMin.x || lo.x > e.posMax.x || hi.y < e.posMin.y || lo.y > e.posMax.y || hi.z < e.posMin.z ||
            lo.z > e.posMax.z)
            return;
        f(e);
    };
    for (int i : s_aientHuge) visit(i);
    for (int ix = ICell(lo.x); ix <= ICell(hi.x); ++ix)
        for (int iy = ICell(lo.y); iy <= ICell(hi.y); ++iy)
            if (auto it = s_mpcell.find(Key(ix, iy)); it != s_mpcell.end())
                for (int i : it->second) visit(i);
}

int ClipPieces(const SOE* psoe, const glm::vec3& a, const glm::vec3& b, LSG* alsg, int c)
{
    return ClipSegmentEntry(psoe, a, b, c, alsg);
}
}  // namespace

void BuildColl3(int oidSkip0, int oidSkip1)
{
    s_aent.clear();
    s_mpcell.clear();
    s_aientHuge.clear();
    for (const SCENECOLL3& c : g_acoll3)
    {
        if (c.oid == oidSkip0 || c.oid == oidSkip1 || !c.psoe || c.psoe->geomWorld.apos.empty()) continue;
        if (c.psoe->bsp.abspn.empty()) continue;   // (no tree: not a solid for the segment clip)
        ENT e;
        e.psoe = c.psoe.get();
        e.oid = c.oid;
        e.posMin = e.posMax = c.psoe->geomWorld.apos[0];
        for (const glm::vec3& p : c.psoe->geomWorld.apos)
        {
            e.posMin = glm::min(e.posMin, p);
            e.posMax = glm::max(e.posMax, p);
        }
        const int i = int(s_aent.size());
        s_aent.push_back(e);
        const int ix0 = ICell(e.posMin.x), ix1 = ICell(e.posMax.x), iy0 = ICell(e.posMin.y), iy1 = ICell(e.posMax.y);
        if (int64_t(ix1 - ix0 + 1) * (iy1 - iy0 + 1) > 1024) { s_aientHuge.push_back(i); continue; }
        for (int ix = ix0; ix <= ix1; ++ix)
            for (int iy = iy0; iy <= iy1; ++iy) s_mpcell[Key(ix, iy)].push_back(i);
    }
    s_amark.assign(s_aent.size(), 0);
    s_nMark = 0;
    std::printf("coll3: %zu collision entries (%zu over the whole world) in %zu cells\n", s_aent.size(), s_aientHuge.size(),
                s_mpcell.size());
}

int CsoeColl3() { return int(s_aent.size()); }

bool FHitSegment3(const glm::vec3& a, const glm::vec3& b, HIT3* phit)
{
    bool fHit = false;
    HIT3 best;
    ForEntries(a, b, [&](const ENT& e) {
        LSG alsg[4];
        const int c = ClipPieces(e.psoe, a, b, alsg, 4);
        for (int j = 0; j < c; ++j)
        {
            if (!alsg[j].psurf0) continue;   // (the segment starts inside: not an entry)
            if (alsg[j].t0 < best.t)
            {
                best.t = alsg[j].t0;
                best.pos = alsg[j].pos0;
                best.normal = alsg[j].psurf0->normal;
                best.psoe = e.psoe;
                best.oid = e.oid;
                fHit = true;
            }
            break;
        }
    });
    if (fHit && phit) *phit = best;
    return fHit;
}

bool FFloor3(float x, float y, float zTop, float zBottom, float nzMin, HIT3* phit)
{
    const glm::vec3 a(x, y, zTop), b(x, y, zBottom);
    bool fHit = false;
    HIT3 best;
    ForEntries(a, b, [&](const ENT& e) {
        LSG alsg[8];
        const int c = ClipPieces(e.psoe, a, b, alsg, 8);
        for (int j = 0; j < c; ++j)
        {
            const SURF* ps = alsg[j].psurf0;
            if (!ps || ps->normal.z < nzMin) continue;
            if (alsg[j].t0 < best.t)
            {
                best.t = alsg[j].t0;
                best.pos = alsg[j].pos0;
                best.normal = ps->normal;
                best.psoe = e.psoe;
                best.oid = e.oid;
                fHit = true;
            }
            break;
        }
    });
    if (fHit && phit) *phit = best;
    return fHit;
}
}  // namespace sly3
