// The AI's walkable areas: regions, their 2D BSP and triangle graph, the blocking polygons. See nav.h.
// Reference: Sly2-Reference/Sly2-Release/game/m_1BF110.cpp, m_1BF1F8_p3.cpp, m_1BC050.cpp, m_1BD310_p3.cpp and the original's code
// for 0x1BC688 / 0x1BC9F0 / 0x1BD400 / 0x1BD560 / 0x1BCAC0 / 0x1BCCE0 / 0x1BCF18 (round4_paris_high*.cpp).

#include "nav.h"
#include "worldglobals.h"
#include "objmethod.h"
#include "guard.h"
#include "so.h"
#include "update.h"

#include "activate.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "crv.h"
#include "lo.h"
#include "sw.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace
{
constexpr float EPS_PLANE = 0.00999999978f;   // D_002C0C00
constexpr float S_MERGE = 50.0f;              // D_002C0C04 (the paths' minimum step)
constexpr float EPS_POLY = 1.0f;              // D_002C0AB4 (added to the polygons' plane distances)
constexpr float S_TURN = 0.2f;                // D_002C0AB8
constexpr float DS_SEARCH = 10.0f;            // D_002C0ABC
constexpr float DS_WALK = 50.0f;              // D_002C0AC0
constexpr float DS_AHEAD = 200.0f;            // D_002C0AC4
const glm::vec3 AXIS_Z(0.0f, 0.0f, 1.0f);     // D_002A2DC0
const glm::vec3 AXIS_X(1.0f, 0.0f, 0.0f);     // D_002A2DA0

struct NAVWORLD
{
    SW* psw = nullptr;
    uint32_t serialSw = 0;
    std::unordered_map<const LOXMESH*, std::unique_ptr<NAVREGION>> mpregion;
    std::vector<NAVREGION*> apregionLeaf;   // the world BSP's leaves (+0x18)
    NAVREGION* pregionDefault = nullptr;    // +0x45C8
    std::unordered_map<const LO*, std::unique_ptr<NAVPOLY>> mppoly;
    std::vector<LO*> aploDirty;             // the DL 0x3E1A4C
    std::vector<std::shared_ptr<NAVPOLY>> apolyImported;
    int nStamp = 0;                         // GAME +0x3D70
    // the merges (0x1BDE80): a member's merged polygon (+0x1D8), a merged polygon's members (its DL +0x1C4), the
    // merged polygons (class 0x0E objects oid 0x2CD, +0x1C0 set) and their free pool (the DL 0x3E1A40)
    std::unordered_map<const LO*, LO*> mpowner;
    std::unordered_map<const LO*, std::vector<LO*>> mpachild;
    std::vector<LO*> aploMerged, aploFree;
};
NAVWORLD s_nav;
WORLD_TABLE(s_nav);   // (PC: moved aside while a kept world loads, worldglobals.h)
int s_nGenNav = 1;

void EnsureWorld()
{
    if (s_nav.psw == g_psw && s_nav.serialSw == (g_psw ? g_psw->serial : 0)) return;
    s_nav = NAVWORLD{};
    ++s_nGenNav;
    s_nav.psw = g_psw;
    s_nav.serialSw = g_psw ? g_psw->serial : 0;
}

float DistXy(const glm::vec3& a, const glm::vec3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// orig 0x11EE08: the point of the segment a-b nearest p, its fraction, its distance
glm::vec3 PosNearestSeg(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float* pt = nullptr, float* pd = nullptr)
{
    const glm::vec3 d = b - a;
    float t = glm::dot(p - a, d) / glm::dot(d, d);
    if (t < 0.0f) t = 0.0f;
    else if (1.0f < t) t = 1.0f;
    if (pt) *pt = t;
    const glm::vec3 r = a + d * t;
    if (pd) *pd = glm::length(p - r);
    return r;
}

float CrossZ(const glm::vec3& a, const glm::vec3& b) { return a.x * b.y - b.x * a.y; }

NAVREGION* PregionBuild(LOXMESH* pmesh)
{
    auto preg = std::make_unique<NAVREGION>();
    preg->pmesh = pmesh;
    preg->avtx.resize(pmesh->avtx.size());
    for (size_t i = 0; i < pmesh->avtx.size(); ++i)
    {
        preg->avtx[i].pos = glm::vec4(pmesh->avtx[i].pos, 0.0f);
        for (int j : pmesh->avtx[i].aivtx) preg->avtx[i].aedge.push_back({j, 0.0f});
    }
    // orig 0x1C0880: the xy length of every edge
    for (auto& v : preg->avtx)
        for (auto& e : v.aedge) e.s = DistXy(glm::vec3(v.pos), glm::vec3(preg->avtx[e.ivtx].pos));
    preg->atri = pmesh->atri;
    preg->anode.resize(pmesh->apt.size());
    for (size_t i = 0; i < pmesh->apt.size(); ++i)
    {
        const auto& pt = pmesh->apt[i];
        preg->anode[i].plane = glm::vec3(pt.x, pt.y, pt.z);
        preg->anode[i].child[0] = pt.c;
        preg->anode[i].child[1] = pt.d;
    }
    preg->nStamp = ++s_nav.nStamp;
    NAVREGION* p = preg.get();
    s_nav.mpregion[pmesh] = std::move(preg);
    return p;
}

// the world's BSP leaves (orig 0x1C1138: by the regions' oids)
void BuildLeaves()
{
    if (!s_nav.apregionLeaf.empty() || !g_psw) return;
    if (!g_psw->acmesh.empty())
    {
        const CMESH& cm = *g_psw->acmesh.back();
        for (size_t i = 0; i < cm.an.size(); ++i)
        {
            LOXMESH* pmesh = nullptr;
            for (LO* plo : g_psw->mpcidaplo[0x11])
                if (plo->oid == cm.an[i]) { pmesh = static_cast<LOXMESH*>(plo); break; }
            NAVREGION* preg = pmesh ? PregionOfMesh(pmesh) : nullptr;
            if (preg) preg->ileaf = static_cast<int>(i);
            s_nav.apregionLeaf.push_back(preg);
        }
    }
    if (g_psw->ploMesh2CA) s_nav.pregionDefault = PregionOfMesh(static_cast<LOXMESH*>(g_psw->ploMesh2CA));
}

// the polygon's world points, box and edge planes (orig 0x1BD040 / 0x1BD400), its winding (0x1BC2F8 / 0x1BC570)
void BuildPoly(NAVPOLY* pp)
{
    const LOXPATH* ppath = static_cast<const LOXPATH*>(pp->plo);
    const glm::mat4 m = MatWorldLox(pp->plo);
    const int c = static_cast<int>(ppath->avecF4.size());
    pp->aposWorld.assign(c > 0 ? c : 0, glm::vec4(0.0f));
    if (c < 2) return;
    for (int i = 0; i < c - 1; ++i) pp->aposWorld[i] = glm::vec4(glm::vec3(m * glm::vec4(ppath->avecF4[i], 1.0f)), 0.0f);
    pp->aposWorld[c - 1] = pp->aposWorld[0];
    pp->posMin = pp->posMax = pp->aposWorld[0];
    for (int i = 1; i < c - 1; ++i)
    {
        pp->posMin = glm::min(pp->posMin, pp->aposWorld[i]);
        pp->posMax = glm::max(pp->posMax, pp->aposWorld[i]);
    }
    pp->posMin.z = -FLT_MAX;  // D_002A76D0
    pp->posMax.z = FLT_MAX;
    // the winding (0x1BC2F8: natively the signed area): clockwise polygons are reversed (0x1BC570 at the post-load)
    {
        float area = 0.0f;
        for (int i = 0; i < c - 1; ++i) area += CrossZ(glm::vec3(pp->aposWorld[i]), glm::vec3(pp->aposWorld[i + 1]));
        pp->kind = area < 0.0f ? 4 : 3;
    }
    if (pp->kind == 4)
    {
        // 0x1BC570: the path object's own points and parameters reversed (its curve recalculated, 0x1E6180), so the
        // way around (the follower on pcrv) and the sides tested on the outline (0x1BCAC0) run the same direction
        ReverseLoxPath(pp->plo);
        std::reverse(pp->aposWorld.begin(), pp->aposWorld.end());
        pp->kind = 3;
    }
    // the edge planes (0x1BD400: the edge x z normalised through its first point, 0x119390)
    pp->aplane.assign(c - 1, glm::vec4(0.0f));
    for (int i = 1; i < c; ++i)
    {
        const glm::vec3 a(pp->aposWorld[i - 1]), b(pp->aposWorld[i]);
        glm::vec3 n = glm::cross(b - a, AXIS_Z);
        const float l = glm::length(n);
        if (l > 0.0f) n /= l;
        pp->aplane[i - 1] = glm::vec4(n, glm::dot(n, a));
    }
    pp->pcrv = PcrvFromLo(pp->plo);
}

// orig: 0x002338C0 qsort (the original's library sort, src/lib/qsort.c: the BSD one; its order of equal elements
// differs from the host's)
template <typename T, typename CMP>
void QsortOrig(T* a, size_t n, CMP cmp)
{
    auto med3 = [&](T* x, T* y, T* z) {
        return cmp(x, y) < 0 ? (cmp(y, z) < 0 ? y : (cmp(x, z) < 0 ? z : x))
                             : (cmp(y, z) > 0 ? y : (cmp(x, z) < 0 ? x : z));
    };
    auto vecswap = [](T* x, T* y, ptrdiff_t c) { for (ptrdiff_t i = 0; i < c; ++i) std::swap(x[i], y[i]); };
    for (;;)
    {
        bool fSwap = false;
        if (n < 7)
        {
            for (T* pm = a + 1; pm < a + n; ++pm)
                for (T* pl = pm; pl > a && cmp(pl - 1, pl) > 0; --pl) std::swap(*pl, *(pl - 1));
            return;
        }
        T* pm = a + n / 2;
        if (n > 7)
        {
            T* pl = a;
            T* pn = a + (n - 1);
            if (n > 40)
            {
                const size_t d = n / 8;
                pl = med3(pl, pl + d, pl + 2 * d);
                pm = med3(pm - d, pm, pm + d);
                pn = med3(pn - 2 * d, pn - d, pn);
            }
            pm = med3(pl, pm, pn);
        }
        std::swap(*a, *pm);
        T* pa = a + 1, * pb = a + 1;
        T* pc = a + (n - 1), * pd = a + (n - 1);
        for (;;)
        {
            int r;
            while (pb <= pc && (r = cmp(pb, a)) <= 0)
            {
                if (r == 0) { fSwap = true; std::swap(*pa, *pb); ++pa; }
                ++pb;
            }
            while (pb <= pc && (r = cmp(pc, a)) >= 0)
            {
                if (r == 0) { fSwap = true; std::swap(*pc, *pd); --pd; }
                --pc;
            }
            if (pb > pc) break;
            std::swap(*pb, *pc);
            fSwap = true;
            ++pb;
            --pc;
        }
        if (!fSwap)
        {
            for (T* p = a + 1; p < a + n; ++p)
                for (T* pl = p; pl > a && cmp(pl - 1, pl) > 0; --pl) std::swap(*pl, *(pl - 1));
            return;
        }
        T* pn = a + n;
        ptrdiff_t r = std::min(pa - a, pb - pa);
        vecswap(a, pb - r, r);
        r = std::min(pd - pc, pn - pd - 1);
        vecswap(pb, pn - r, r);
        if ((r = pb - pa) > 1) QsortOrig(a, static_cast<size_t>(r), cmp);
        if ((r = pd - pc) > 1)
        {
            a = pn - r;
            n = static_cast<size_t>(r);
            continue;
        }
        return;
    }
}

void Dequeue(LO* plo)
{
    auto& d = s_nav.aploDirty;
    d.erase(std::remove(d.begin(), d.end(), plo), d.end());
}
void Enqueue(LO* plo)
{
    if (std::find(s_nav.aploDirty.begin(), s_nav.aploDirty.end(), plo) == s_nav.aploDirty.end()) s_nav.aploDirty.push_back(plo);
}

// orig: 0x001BD1E0 (0x1C1968: out of its region's slots)
void RemovePolyFromRegion(NAVPOLY* pp)
{
    if (!pp || !pp->pregion) return;
    auto& a = pp->pregion->apoly;
    auto it = std::find(a.begin(), a.end(), pp);
    if (it != a.end()) *it = nullptr;
    pp->pregion->nStamp = ++s_nav.nStamp;
    pp->pregion = nullptr;
}

// orig: 0x001BDE18 (a member given to the merged polygon: out of its region and the queue)
void AddPolyMember(LO* plo, LO* ploOwner)
{
    s_nav.mpowner[plo] = ploOwner;
    s_nav.mpachild[ploOwner].push_back(plo);
    auto it = s_nav.mppoly.find(plo);
    if (it != s_nav.mppoly.end()) RemovePolyFromRegion(it->second.get());
    Dequeue(plo);
}

// orig: 0x001BDA00 (a merged polygon from the pool D_003E1A40, refilled 16 at a time: class 0x0E, oid 0x2CD, an
// identity matrix, the parameters 0..31, put into the world (its OnAdd queues it))
LOXPATH* PpathNewMerged()
{
    if (s_nav.aploFree.empty() && g_psw)
        for (int i = 0; i < 16; ++i)
        {
            auto* q = static_cast<LOXPATH*>(PloNew(0x0E, g_psw, nullptr, 0x2CD, -1));
            q->fE0 = 0;
            q->nE4 = 1;
            q->mat = glm::mat4(1.0f);
            q->an.resize(32);
            for (int j = 0; j < 32; ++j)
            {
                const float g = static_cast<float>(j);
                std::memcpy(&q->an[j], &g, 4);
            }
            RemoveLo(g_psw, q);  // vt slot 9
            s_nav.aploMerged.push_back(q);
            s_nav.aploFree.push_back(q);
        }
    if (s_nav.aploFree.empty()) return nullptr;
    auto* p = static_cast<LOXPATH*>(s_nav.aploFree.front());
    s_nav.aploFree.erase(s_nav.aploFree.begin());
    AddLo(g_psw, p);  // vt slot 8
    s_nav.mpowner.erase(p);
    return p;
}

// orig: 0x001BD218 (a merged polygon dissolved: its members requeued, itself back to the pool)
void DissolveMerged(LO* ploOwner)
{
    std::vector<LO*> a;
    a.swap(s_nav.mpachild[ploOwner]);
    for (LO* plo : a)
    {
        s_nav.mpowner.erase(plo);
        Enqueue(plo);
    }
    if (g_psw) RemoveLo(g_psw, ploOwner);  // vt slot 9 (0x1BC198)
    s_nav.aploFree.push_back(ploOwner);
}

// orig: 0x001BDE80 (the polygons merged into a new one: the hull of all their points (0x1BDBF0), centred on the
// average point; the old ones its members (merged ones dissolved into it))
void MergePolys(int n, NAVPOLY** ap)
{
    std::vector<glm::vec4> apos;
    for (int i = 0; i < n; ++i)
        for (size_t j = 0; j + 1 < ap[i]->aposWorld.size(); ++j) apos.push_back(ap[i]->aposWorld[j]);
    LOXPATH* pNew = PpathNewMerged();
    if (!pNew) return;
    // 0x1BDBF0: sorted by x (0x1BDBC8: -1 unless b.x < a.x), then the two chains of the hull
    const int c = static_cast<int>(apos.size());
    QsortOrig(apos.data(), apos.size(), [](const glm::vec4* a, const glm::vec4* b) { return b->x < a->x ? 1 : -1; });
    auto crossZ = [](const glm::vec4& a, const glm::vec4& b, const glm::vec4& cc) {
        const glm::vec3 d = glm::vec3(b) - glm::vec3(a), e = glm::vec3(cc) - glm::vec3(b);
        return d.x * e.y - d.y * e.x;
    };
    std::vector<glm::vec4> hull(0x80);
    int k = 2;
    hull[0] = apos[0];
    hull[1] = apos[1];
    for (int i = 2; i < c; ++i)
    {
        hull[k++] = apos[i];
        while (k >= 3 && crossZ(hull[k - 3], hull[k - 2], hull[k - 1]) < 0.0f)
        {
            hull[k - 2] = hull[k - 1];
            --k;
        }
    }
    const int t = k;
    hull[k++] = apos[c - 2];
    for (int i = c - 3; i >= 0; --i)
    {
        hull[k++] = apos[i];
        while (t < k && crossZ(hull[k - 3], hull[k - 2], hull[k - 1]) < 0.0f)
        {
            hull[k - 2] = hull[k - 1];
            --k;
        }
    }
    const int cHull = k > 31 ? 32 : k;
    // the centre: the average of the points but the closing one (the sum from the last down, times 1 / (c - 1))
    glm::vec4 sum(0.0f);
    for (int i = cHull - 2; i >= 0; --i) sum += hull[i];
    const float q = 1.0f / static_cast<float>(cHull - 1);
    sum *= q;
    pNew->mat = glm::mat4(1.0f);
    pNew->mat[3] = glm::vec4(glm::vec3(sum), 1.0f);  // vt slot 29
    pNew->avecF4.resize(cHull);
    for (int i = cHull - 1; i >= 0; --i) pNew->avecF4[i] = glm::vec3(hull[i] - sum);
    pNew->an.resize(cHull);  // (the parameters 0..c-1 of the 32: the curve's points)
    for (int i = 0; i < n; ++i)
    {
        LO* plo = ap[i]->plo;
        if (std::find(s_nav.aploMerged.begin(), s_nav.aploMerged.end(), plo) != s_nav.aploMerged.end())
        {
            std::vector<LO*> a;
            a.swap(s_nav.mpachild[plo]);
            for (LO* pc : a)
            {
                s_nav.mpowner.erase(pc);
                AddPolyMember(pc, pNew);
            }
            if (g_psw) RemoveLo(g_psw, plo);  // vt slot 9
            s_nav.aploFree.push_back(plo);
        }
        else AddPolyMember(plo, pNew);
    }
    pNew->nE4 = 1;
    InvalidateCrvOfLo(pNew);  // 0x1E6180 (the curve rebuilt from the new points)
    if (std::getenv("SLY2N_NAVMERGE"))
        std::printf("navmerge: t %.3f %d + %d -> %d points, centre (%.1f %.1f)\n", g_clock.t, ap[0]->plo ? ap[0]->plo->oid : -1,
                    n > 1 && ap[1]->plo ? ap[1]->plo->oid : -1, cHull, sum.x, sum.y);
    if (std::getenv("SLY2N_NAVMERGE") && std::getenv("SLY2N_NAVMERGEPTS"))
    {
        for (int i = 0; i < cHull; ++i) std::printf(" (%.0f %.0f)", hull[i].x, hull[i].y);
        std::printf("\n  from:");
        for (int i = 0; i < n; ++i)
        {
            for (size_t j = 0; j + 1 < ap[i]->aposWorld.size(); ++j) std::printf(" (%.0f %.0f)", ap[i]->aposWorld[j].x, ap[i]->aposWorld[j].y);
            std::printf(" |");
        }
        std::printf("\n");
    }
}

// orig: 0x001BD8A8 (the polygons overlap: an edge of p crosses q, or q's first point is inside p)
bool FPolysOverlap(NAVPOLY* p, NAVPOLY* q)
{
    const int c = static_cast<int>(p->aposWorld.size());
    POLYHIT hit;
    int i = 1;
    glm::vec3 v(p->aposWorld.empty() ? glm::vec4(0.0f) : p->aposWorld[0]);
    for (; i < c; ++i)
    {
        const glm::vec3 w(p->aposWorld[i]);
        if (ChitPoly(q, v, w, 1, &hit)) break;
        v = w;
    }
    if (i < c) return true;
    return !q->aposWorld.empty() && FPolyContains(p, glm::vec3(q->aposWorld[0]));
}

// orig: 0x001C1BA0 (into the region, or merged with the first polygon near it that it overlaps)
void AddPolyToRegion(NAVREGION* preg, NAVPOLY* pp)
{
    static const bool s_fNoMerge = std::getenv("SLY2N_NOMERGE") != nullptr;  // (debug: the merges off)
    NAVPOLY* ap[0x21];
    const int n = CpolyNear(preg, glm::vec3(pp->posMin), glm::vec3(pp->posMax), 0x20, ap);
    for (int i = 0; i < n; ++i)
        if (!s_fNoMerge && FPolysOverlap(pp, ap[i]))
        {
            NAVPOLY* am[2] = {pp, ap[i]};
            MergePolys(2, am);
            return;
        }
    auto it = std::find(preg->apoly.begin(), preg->apoly.end(), nullptr);
    if (it != preg->apoly.end()) *it = pp;
    else preg->apoly.push_back(pp);
    pp->pregion = preg;
    preg->nStamp = ++s_nav.nStamp;
}
}  // namespace

void ResetNav()
{
    s_nav = NAVWORLD{};
    ++s_nGenNav;
}

int NGenNav() { return s_nGenNav; }

// orig: 0x001F37C8 (the object's region entry moved to preg's list: off the old one's, appended to the new one's;
// 0x1C1440's cached triangle record not kept natively)
void SetAloRegion(ALO* palo, NAVREGION* preg)
{
    REGENT* pre = Pregent(palo);
    if (!pre) return;
    NAVREGION* pold = pre->nGenNav == s_nGenNav ? pre->preg : nullptr;
    if (pold == preg) return;
    // (an entry is the object's: a harness's imported entry of the same object (pre null) is replaced, not doubled)
    if (pold)
    {
        auto& a = pold->amember;
        a.erase(std::remove_if(a.begin(), a.end(), [&](const NAVREGION::MEMBER& m) { return m.pre == pre || m.plo == palo; }), a.end());
    }
    if (preg)
    {
        auto& a = preg->amember;
        auto it = std::find_if(a.begin(), a.end(), [&](const NAVREGION::MEMBER& m) { return m.pre == pre || m.plo == palo; });
        if (it == a.end()) a.push_back({palo, pre->dz, pre->sRadius, pre->gMass, pre});
    }
    pre->preg = preg;
    pre->nGenNav = s_nGenNav;
    if (std::getenv("SLY2N_REGENTPRINT"))
        std::printf("regent %d (cid 0x%X): region %d -> %d (members %zu)\n", palo->oid, palo->Cid(), pold && pold->pmesh ? pold->pmesh->oid : -1,
                    preg && preg->pmesh ? preg->pmesh->oid : -1, preg ? preg->amember.size() : 0);
}

// orig: 0x001F3868
void UpdateAloRegion(ALO* palo)
{
    if (!Pregent(palo)) return;
    SetAloRegion(palo, PregionAt(PosAi(palo), true));
}

namespace
{
// orig: 0x00259B20 (EOPID 83, option 200 of the ALOs, type 0x2011: the getter): the region the object is in, as its
// mesh object (cid 0x11): ALO +0xE8 -> +0xDC (REGENT) -> +0x10; none: nil. Ep5's boss fights (world 17's s2954)
// start the respawn cycle while Sly's region is not the fight's own (option 1642 of the boss's job).
struct INITREGIONGETTER
{
    INITREGIONGETTER()
    {
        RegisterObjectGetter(83, [](LO* plo) {
            CRef r;
            ALO* palo = dynamic_cast<ALO*>(plo);
            const REGENT* pre = palo ? Pregent(palo) : nullptr;
            NAVREGION* preg = pre && pre->nGenNav == s_nGenNav ? pre->preg : nullptr;
            if (preg && preg->pmesh) r.SetBasic(preg->pmesh);
            return r;
        });
    }
} s_initregiongetter;
}

// orig: 0x001C10C8, 0x001C0648: worlds without a file BSP use the authored default walkable quad.
void InitSwNav(SW* psw)
{
    if (!psw->acmesh.empty()) return;
    auto* pmesh = static_cast<LOXMESH*>(PloNew(0x11, psw, nullptr, 0x2CA, -1));
    pmesh->avtx = {
        {{-100000.0f, -100000.0f, 0.0f}, {1, 2, 3}},
        {{100000.0f, -100000.0f, 0.0f}, {0, 2}},
        {{100000.0f, 100000.0f, 0.0f}, {0, 1, 3}},
        {{-100000.0f, 100000.0f, 0.0f}, {0, 2}},
    };
    pmesh->atri = {{0, 1, 2}, {0, 2, 3}};
    const float g = std::sqrt(2.0f) * 0.5f;
    pmesh->apt = {{g, -g, 0.0f, 0xC001, 0xC000}};
    psw->ploMesh2CA = pmesh;
    // PregionBuild computes 0x1C0880's edge lengths and stamps the completed region.
    PregionOfMesh(pmesh);
}

NAVREGION* PregionOfMesh(LOXMESH* pmesh)
{
    EnsureWorld();
    if (!pmesh) return nullptr;
    auto it = s_nav.mpregion.find(pmesh);
    return it != s_nav.mpregion.end() ? it->second.get() : PregionBuild(pmesh);
}

// orig: 0x001C0D70
NAVREGION* PregionAt(const glm::vec3& pos, bool fDefault)
{
    EnsureWorld();
    BuildLeaves();
    if (!g_psw || g_psw->acmesh.empty()) return s_nav.pregionDefault;
    const CMESH& cm = *g_psw->acmesh.back();
    size_t i = 0;
    while (i < cm.ae.size())
    {
        const CMESH::E& e = cm.ae[i];
        const glm::vec4& pl = cm.avec[e.ivec];
        const float d = (pl.x * pos.x + pl.y * pos.y) + 1.0f * (pl.z * pos.z) - 1.0f * pl.w;
        const uint16_t k = static_cast<uint16_t>(0.0f < d ? e.b : e.a);
        if (k & 0x8000)
        {
            const size_t il = k & 0x7FFF;
            return il < s_nav.apregionLeaf.size() ? s_nav.apregionLeaf[il] : nullptr;
        }
        i = k;
    }
    // (the tree's +0x10 flag: natively every loaded tree is unflagged)
    (void)fDefault;
    return s_nav.pregionDefault;
}

// orig: 0x001BF110
int ItriNav(const NAVREGION* preg, const glm::vec3& pos, int fl)
{
    const uint16_t mask = (fl & 1) ? 0x8000 : 0x4000;
    size_t i = 0;
    while (i < preg->anode.size())
    {
        const NAVREGION::NODE& n = preg->anode[i];
        const float d = (n.plane.x * pos.x + n.plane.y * pos.y) + 1.0f * (n.plane.z * -1.0f);
        const uint16_t k = n.child[0.0f < d ? 1 : 0];
        if (k & 0x8000)
        {
            if (!(k & mask)) return -1;
            return k & 0x3FFF;
        }
        i = k;
    }
    return -1;
}

// orig: 0x001BF1F8
int ItriNavFrom(const NAVREGION* preg, int inode, const glm::vec3& pos, int fl)
{
    const uint16_t mask = (fl & 1) ? 0x8000 : 0x4000;
    size_t i = static_cast<size_t>(inode);
    while (i < preg->anode.size())
    {
        const NAVREGION::NODE& n = preg->anode[i];
        const float d = (n.plane.x * pos.x + n.plane.y * pos.y) + 1.0f * (n.plane.z * -1.0f);
        uint16_t k;
        if (d < -EPS_PLANE) k = n.child[0];
        else if (EPS_PLANE < d) k = n.child[1];
        else
        {
            k = n.child[1];
            if (k & 0x8000)
            {
                if (k & mask) return k & 0x3FFF;
            }
            else if (k < preg->anode.size())
            {
                const int it = ItriNavFrom(preg, k, pos, fl);
                if (it >= 0) return it;
            }
            k = n.child[0];
        }
        if (k & 0x8000)
        {
            if (!(k & mask)) return -1;
            return k & 0x3FFF;
        }
        i = k;
    }
    return -1;
}

// orig: 0x001BFDB0
glm::vec3 PosNearestTri(const NAVREGION* preg, int itri, const glm::vec3& p)
{
    const auto& t = preg->atri[itri];
    const glm::vec3 a(preg->avtx[t[0]].pos), b(preg->avtx[t[1]].pos), c(preg->avtx[t[2]].pos);
    if (0.0f < CrossZ(p - a, b - a)) return PosNearestSeg(p, a, b);
    if (0.0f < CrossZ(p - b, c - b)) return PosNearestSeg(p, b, c);
    if (0.0f < CrossZ(p - c, a - c)) return PosNearestSeg(p, c, a);
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float g = glm::dot(a - p, n) / n.z;
    return p + AXIS_Z * g;
}

// orig: 0x001C0320 (from the root, side fl)
bool FNavPoint(const NAVREGION* preg, const glm::vec3& pos, glm::vec3* pposOut, int fl)
{
    // (no region: the original reads the null region's fields (PS2 low memory, 0x1BF1F8 finds no triangle); none here)
    if (!preg) return false;
    const int it = ItriNavFrom(preg, 0, pos, fl);
    if (it < 0) return false;
    *pposOut = PosNearestTri(preg, it, pos);
    return true;
}
bool FNavPoint(const NAVREGION* preg, const glm::vec3& pos, glm::vec3* pposOut) { return FNavPoint(preg, pos, pposOut, 0); }

// orig: 0x001C0B50
bool FKeepOnNav(NAVREGION* preg, const glm::vec3& pos, glm::vec3* pposOut)
{
    const int it = ItriNav(preg, pos, 1);
    if (it < 0) return false;
    const glm::vec3 projected = PosNearestTri(preg, it, pos);
    NAVPOLY* apoly[0x81];
    const int n = CpolyNear(preg, projected, projected, 0x80, apoly);
    NAVPOLY* ppoly = nullptr;
    for (int j = 0; j < n && !ppoly; ++j)
        if (FPolyContains(apoly[j], pos)) ppoly = apoly[j];
    if (!ppoly)
    {
        *pposOut = projected;
        return true;
    }
    float best = FLT_MAX;  // D_002A76DC
    glm::vec3 posBest(0.0f);
    glm::vec3 a(ppoly->aposWorld[0]);
    a.z = projected.z;
    for (size_t i = 1; i < ppoly->aposWorld.size(); ++i)
    {
        glm::vec3 b(ppoly->aposWorld[i]);
        b.z = projected.z;
        NAVSEG aseg[1];
        const int c = CsegNav(preg, a, b, 1, aseg, 2);
        for (int k = 0; k < c; ++k)
        {
            float d;
            const glm::vec3 q = PosNearestSeg(projected, glm::vec3(aseg[k].posStart), glm::vec3(aseg[k].posEnd), nullptr, &d);
            if (d < best)
            {
                best = d;
                posBest = q;
            }
        }
        a = b;
    }
    FNavPoint(preg, posBest, pposOut, 0);  // orig 0x1C0940 (its answer unused)
    return true;
}

// orig: 0x001BF3D8
int CsegNav(const NAVREGION* preg, const glm::vec3& b, const glm::vec3& c, int cMax, NAVSEG* aseg, int fl)
{
    const uint16_t mask = (fl & 1) ? 0x8000 : 0x4000;
    const size_t nodeEnd = preg->anode.size();
    struct ENTRY { size_t node = 0; int itri = -1; float t = 0; glm::vec4 plane{0.0f}; };
    std::vector<ENTRY> ast(1);
    ENTRY cur;
    cur.node = 0;
    ast[0].t = 1.0f;  // the bottom: the segment's end
    auto dist = [](const NAVREGION::NODE& n, float x, float y) {
        return (n.plane.x * x + n.plane.y * y) + 1.0f * (n.plane.z * -1.0f);
    };
    auto side = [](float x) { return EPS_PLANE < x ? 0 : (x < -EPS_PLANE ? 1 : 2); };
    auto leaf = [&](uint16_t k) { return (k & mask) ? (k & 0x3FFF) : -1; };
    NAVSEG* prev = nullptr;
    int cseg = 0;
    for (;;)
    {
        while (cur.node != nodeEnd)
        {
            const NAVREGION::NODE& n = preg->anode[cur.node];
            const float db = dist(n, b.x, b.y), dc = dist(n, c.x, c.y);
            const int sb = side(db), sc = side(dc);
            const bool back = sb == 1 || sc == 1, front = sb == 0 || sc == 0;
            uint16_t k;
            if (back && front)
            {
                const float t0 = cur.t, t1 = ast.back().t;
                const float tc = db / (db - dc);
                const float d1 = t1 * dc + (1.0f - t1) * db;
                const float d0 = t0 * dc + (1.0f - t0) * db;
                if (t0 < tc && tc < t1)
                {
                    ENTRY far;
                    far.t = tc;
                    far.plane = glm::vec4(n.plane.x, n.plane.y, 0.0f, n.plane.z);
                    const uint16_t kf = n.child[d0 <= 0.0f ? 1 : 0];
                    if (kf & 0x8000)
                    {
                        far.node = nodeEnd;
                        far.itri = leaf(kf);
                    }
                    else far.node = kf;
                    ast.push_back(far);
                }
                const bool nearBack = d0 < 0.0f || (d0 == 0.0f && d1 <= 0.0f);
                k = n.child[nearBack ? 0 : 1];
            }
            else k = n.child[(!back && front) ? 1 : 0];
            if (k & 0x8000)
            {
                cur.node = nodeEnd;
                cur.itri = leaf(k);
            }
            else cur.node = k;
        }
        for (;;)
        {
            if (cur.itri >= 0)
            {
                const ENTRY& next = ast.back();
                bool merge = false;
                if (prev && cur.t == prev->t1) merge = (fl & 2) || cur.itri == prev->itri;
                if (merge)
                {
                    const float t1 = next.t;
                    prev->posEnd = glm::vec4(b * (1.0f - t1) + c * t1, 0.0f);
                    prev->t1 = t1;
                    prev->planeEnd = next.plane;
                }
                else
                {
                    if (cseg >= cMax) return cMax;
                    prev = &aseg[cseg++];
                    *prev = NAVSEG{};
                    const float t0 = cur.t, t1 = next.t;
                    prev->posStart = glm::vec4(b * (1.0f - t0) + c * t0, 0.0f);
                    prev->posEnd = glm::vec4(b * (1.0f - t1) + c * t1, 0.0f);
                    prev->k = 3;
                    prev->t0 = t0;
                    prev->t1 = t1;
                    prev->planeStart = cur.plane;
                    prev->planeEnd = next.plane;
                    prev->itri = cur.itri;
                }
            }
            if (ast.size() <= 1) return cseg;
            cur = ast.back();
            ast.pop_back();
            if (cur.node != nodeEnd) break;
        }
    }
}

// orig: 0x001BF900
bool FNavBlocked(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, int fl)
{
    NAVSEG aseg[2];
    const int c = CsegNav(preg, a, b, 2, aseg, fl | 2);
    return !c || aseg[0].t0 != 0.0f || aseg[0].t1 != 1.0f;
}

namespace
{
// orig 0x1BF980: A* over the region's vertices plus the start (index cvtx) and goal (cvtx + 1); the start's edges
// and the goal's incoming ones as 0x1BFF28 adds them
void AstarNav(const NAVREGION* preg, const std::vector<glm::vec3>& apos, const std::vector<std::vector<NAVREGION::VTX::EDGE>>& aaedge,
              int istart, int igoal, std::vector<int>* paipath)
{
    struct NODE { int iv; float f, g; int iparent; int depth; };
    std::vector<NODE> anode;
    anode.reserve(256);
    std::vector<int> aopen;  // indices into anode, sorted by f
    std::vector<char> afClosed(apos.size(), 0);
    anode.push_back({istart, DistXy(apos[istart], apos[igoal]), 0.0f, -1, 1});
    int in = 0;
    if (istart != igoal)
    {
        for (;;)
        {
            const int ivCur = anode[in].iv;
            for (const auto& e : aaedge[ivCur])
            {
                const int iv = e.ivtx;
                if (iv != igoal)
                {
                    if (afClosed[iv]) continue;
                    afClosed[iv] = 1;
                }
                NODE nn;
                nn.iv = iv;
                nn.g = anode[in].g + e.s;
                nn.f = nn.g + DistXy(apos[ivCur], apos[igoal]);  // (the original's estimate: from the expanded node)
                nn.iparent = in;
                nn.depth = anode[in].depth + 1;
                anode.push_back(nn);
                const int inew = static_cast<int>(anode.size()) - 1;
                auto it = aopen.begin();
                while (it != aopen.end() && !(anode[inew].f < anode[*it].f)) ++it;
                aopen.insert(it, inew);
            }
            if (aopen.empty()) { in = -1; break; }
            in = aopen.front();
            aopen.erase(aopen.begin());
            if (anode[in].iv == igoal) break;
        }
    }
    paipath->clear();
    if (in < 0) return;
    paipath->assign(anode[in].depth, -1);
    for (int i = in; i >= 0; i = anode[i].iparent) (*paipath)[anode[i].depth - 1] = anode[i].iv;
}

// orig 0x1BFC18: the points the path can skip dropped (recursively)
void SmoothNav(const NAVREGION* preg, const std::vector<glm::vec3>& apos, std::vector<int>* pai)
{
    std::vector<int>& ai = *pai;
    const int n = static_cast<int>(ai.size());
    std::vector<int> atmp;
    atmp.push_back(ai[0]);
    int iprev = 0, icur = 1;
    for (int inext = 2; inext < n; ++inext)
    {
        if (FNavBlocked(preg, apos[ai[iprev]], apos[ai[inext]], 0))
        {
            iprev = icur;
            atmp.push_back(ai[icur]);
        }
        icur = inext;
    }
    atmp.push_back(ai[n - 1]);
    const int k = static_cast<int>(atmp.size());
    if (k < n && k >= 3) SmoothNav(preg, apos, &atmp);
    ai = atmp;
}
}  // namespace

// orig: 0x001BFF28
int CposNavPath(const NAVREGION* preg, const glm::vec3& qa, const glm::vec3& qb, int cMax, glm::vec4* aout)
{
    const int la = ItriNavFrom(preg, 0, qa, 1);
    if (la < 0) return 0;
    const glm::vec3 pa = PosNearestTri(preg, la, qa);
    const int lb = ItriNavFrom(preg, 0, qb, 1);
    if (lb < 0) return 0;
    const glm::vec3 pb = PosNearestTri(preg, lb, qb);
    int n;
    int iwrite = 1;  // (sp+0x68: out + 0x10)
    if (la == lb)
    {
        aout[0] = glm::vec4(qa, 0.0f);
        aout[1] = glm::vec4(pa, 0.0f);
        aout[2] = glm::vec4(pb, 0.0f);
        n = 3;
    }
    else
    {
        const int cvtx = static_cast<int>(preg->avtx.size());
        std::vector<glm::vec3> apos(cvtx + 2);
        std::vector<std::vector<NAVREGION::VTX::EDGE>> aaedge(cvtx + 2);
        for (int i = 0; i < cvtx; ++i)
        {
            apos[i] = glm::vec3(preg->avtx[i].pos);
            aaedge[i] = preg->avtx[i].aedge;
        }
        const int istart = cvtx, igoal = cvtx + 1;
        apos[istart] = pa;
        apos[igoal] = pb;
        for (int k = 0; k < 3; ++k)
        {
            const int iv = preg->atri[la][k];
            aaedge[istart].push_back({iv, DistXy(pa, apos[iv])});
        }
        for (int k = 0; k < 3; ++k)
        {
            const int iv = preg->atri[lb][k];
            aaedge[iv].push_back({igoal, DistXy(apos[iv], pb)});
        }
        std::vector<int> ai;
        AstarNav(preg, apos, aaedge, istart, igoal, &ai);
        if (ai.size() >= 3) SmoothNav(preg, apos, &ai);
        const int c = static_cast<int>(ai.size());
        n = cMax < c ? cMax : c;
        for (int k = 0; k < n; ++k) aout[k] = glm::vec4(apos[ai[k]], 0.0f);
    }
    // the points too close to the previous kept one dropped
    glm::vec3 last(aout[0]);
    int w = iwrite;
    for (int r = iwrite; r < n; ++r)
    {
        const glm::vec3 v(aout[r]);
        if (S_MERGE < DistXy(v, last))
        {
            last = v;
            aout[w++] = aout[r];
        }
    }
    n = w;
    if (n == 1)
    {
        aout[1] = aout[0];
        aout[0] = glm::vec4(glm::vec3(aout[0]) + AXIS_X * S_MERGE, aout[0].w + 1.0f);
        n = 2;
    }
    return n;
}

// orig: 0x001C02D0
bool FNavPathCrv(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, CRV* pcrv)
{
    glm::vec4 apos[0x10];
    const int n = CposNavPath(preg, a, b, 0x10, apos);
    if (n < 2) return false;
    pcrv->apos.resize(n);
    for (int i = 0; i < n; ++i) pcrv->apos[i] = glm::vec3(apos[i]);
    if (pcrv->ag.size() < static_cast<size_t>(n))
        for (size_t i = pcrv->ag.size(); i < static_cast<size_t>(n); ++i) pcrv->ag.push_back(static_cast<float>(i));
    pcrv->ag.resize(n);
    RecalcCrv(pcrv, glm::mat4(1.0f));  // orig 0x1E6180
    return true;
}

// ---- the polygons ----

// orig: 0x001C1700
int CpolyNear(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, int cMax, NAVPOLY** apoly)
{
    const float lox = std::min(a.x, b.x), hix = std::max(a.x, b.x);
    const float loy = std::min(a.y, b.y), hiy = std::max(a.y, b.y);
    int n = 0;
    for (NAVPOLY* pp : preg->apoly)
    {
        if (!pp) continue;
        if (!(pp->posMin.x <= hix && lox <= pp->posMax.x && pp->posMin.y <= hiy && loy <= pp->posMax.y)) continue;
        if (cMax < n) break;
        apoly[n++] = pp;
    }
    return n;
}

// orig: 0x001BC9F0
bool FPolyContains(const NAVPOLY* pp, const glm::vec3& pos)
{
    for (const glm::vec4& pl : pp->aplane)
        if (0.0f < glm::dot(glm::vec3(pl), pos) - pl.w) return false;
    return true;
}

// orig: 0x001BC688
int ChitPoly(NAVPOLY* pp, const glm::vec3& a, const glm::vec3& b, int cMax, POLYHIT* ahit)
{
    float tEnter = 0.0f, tExit = 1.0f;
    glm::vec4 planeEnter(0.0f), planeExit(0.0f);
    for (const glm::vec4& pl : pp->aplane)
    {
        const float da = glm::dot(glm::vec3(pl), a) - pl.w + EPS_POLY;
        const float db = glm::dot(glm::vec3(pl), b) - pl.w + EPS_POLY;
        if (da < 0.0f && db < 0.0f) continue;
        if (0.0f < da && 0.0f < db)
        {
            tEnter = tExit = 0.0f;
            break;
        }
        const float t = da / (da - db);
        if (da < 0.0f)
        {
            if (t < tExit) tExit = t;
            planeExit = pl;
        }
        else
        {
            if (tEnter < t) tEnter = t;
            planeEnter = pl;
        }
        if (tExit <= tEnter) break;
    }
    int n = 0;
    if (tExit <= tEnter)
    {
        if (cMax > 0)
        {
            ahit[0] = POLYHIT{};
            ahit[0].posA = glm::vec4(a, 0.0f);
            ahit[0].posB = glm::vec4(b, 0.0f);
            ahit[0].t0 = 0.0f;
            ahit[0].t1 = 1.0f;
            return 1;
        }
    }
    if (0.0f < tEnter && cMax > 0)
    {
        POLYHIT& h = ahit[n++];
        h = POLYHIT{};
        h.posA = glm::vec4(a, 0.0f);
        h.posB = glm::vec4(a * (1.0f - tEnter) + b * tEnter, 0.0f);
        h.planeB = -planeEnter;
        h.t0 = 0.0f;
        h.t1 = tEnter;
        h.ppolyExit = pp;
    }
    if (tExit < 1.0f && n < cMax)
    {
        POLYHIT& h = ahit[n++];
        h = POLYHIT{};
        h.posA = glm::vec4(a * (1.0f - tExit) + b * tExit, 0.0f);
        h.planeA = -planeExit;
        h.posB = glm::vec4(b, 0.0f);
        h.t0 = tExit;
        h.t1 = 1.0f;
        h.ppolyEnter = pp;
    }
    return n;
}

// orig: 0x001BD310
NAVPOLY* PpolyOnSegment(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b)
{
    NAVPOLY* apoly[0x81];
    const int n = CpolyNear(preg, a, b, 0x80, apoly);
    float gMin = 1.0f;
    NAVPOLY* pbest = nullptr;
    for (int i = 0; i < n; ++i)
    {
        POLYHIT ahit[2];
        const int r = ChitPoly(apoly[i], a, b, 2, ahit);
        if (!r || ahit[0].t0 != 0.0f) return apoly[i];
        const float f = ahit[0].t1;
        if (gMin <= f) continue;
        gMin = f;
        pbest = apoly[i];
    }
    return pbest;
}

namespace
{
// orig 0x1BD560: along the path from s in steps of 50 to where it leaves the polygon (-1: *ppos its exit point,
// *ps the distance), 3 at the path's end
int NPathLeavesPoly(NAVPOLY* pp, const CRV* pcrv, bool fForward, glm::vec3* ppos, float* ps, float s)
{
    const float ds = fForward ? DS_WALK : -DS_WALK;
    glm::vec3 pos, posPrev;
    EvalCrvS(pcrv, s, &pos, nullptr);
    bool fWrapped = false;
    const float sStart = pcrv->as.empty() ? 0.0f : pcrv->as.front();  // 0x1E6620
    const float sEnd = SLengthCrv(pcrv);                               // 0x1E6648
    for (;;)
    {
        posPrev = pos;
        s += ds;
        if (s <= sStart)
        {
            if (fWrapped) return 3;
            fWrapped = true;
            s = pcrv->fClosed ? SWrapCrv(pcrv, s) : sStart;
        }
        else if (sEnd <= s)
        {
            if (fWrapped) return 3;
            fWrapped = true;
            s = pcrv->fClosed ? SWrapCrv(pcrv, s) : sEnd;
        }
        EvalCrvS(pcrv, s, &pos, nullptr);
        POLYHIT ahit[2];
        const int c = ChitPoly(pp, posPrev, pos, 2, ahit);
        if (c <= 0) continue;
        if (ps) *ps = s;
        if (ppos) *ppos = glm::vec3(ahit[c - 1].posA);
        return -1;
    }
}

// orig 0x1BCF18: a point of the polygon's curve near s (searched outward by 10 within range) over the region's side
bool FPolyPointOnRegion(NAVPOLY* pp, const NAVREGION* preg, float s, float range, float* ps, int fl)
{
    for (float d = 0.0f; d < range; d += DS_SEARCH)
    {
        float t = SWrapCrv(pp->pcrv, s - d);
        glm::vec3 pos;
        EvalCrvS(pp->pcrv, t, &pos, nullptr);
        if (ItriNav(preg, pos, fl) >= 0)
        {
            if (ps) *ps = t;
            return true;
        }
        t = SWrapCrv(pp->pcrv, s + d);
        EvalCrvS(pp->pcrv, t, &pos, nullptr);
        if (ItriNav(preg, pos, fl) >= 0)
        {
            if (ps) *ps = t;
            return true;
        }
    }
    return false;
}

// orig 0x1E7668: the segment of the curve at distance s (wrapped)
int IsegCrvAt(const CRV* pcrv, float s)
{
    s = SWrapCrv(pcrv, s);
    int i = pcrv->iseg;
    const auto& as = pcrv->as;
    if (i < 0) i = 0;
    if (i >= static_cast<int>(as.size())) i = static_cast<int>(as.size()) - 1;
    if (as[i] < s)
    {
        const int n = static_cast<int>(as.size()) - 1;
        for (; i < n; ++i)
            if (s < as[i]) break;
        --i;
    }
    else
        for (; i > 0; --i)
            if (as[i] <= s) break;
    pcrv->iseg = i;
    return i;
}

// orig 0x1BC530: an index wrapped into 0 .. c - 2 (the last point repeats the first)
int IpolyWrap(const NAVPOLY* pp, int n)
{
    const int c = static_cast<int>(pp->aposWorld.size());
    int r = c ? n % c : 0;
    if (r < 0) r = r - 1 + c;
    else if (r == c - 1) r = 0;
    return r;
}

// orig 0x1BCAC0: around the polygon from sLow to sHigh (fReverse: the other way): the first side (0 / 1) with a
// clear walk along its points, 2 when both are blocked
int NPolyClearSide(NAVPOLY* pp, const NAVREGION* preg, bool fReverse, float sLow, float sHigh)
{
    const int i0 = IsegCrvAt(pp->pcrv, sLow), i1 = IsegCrvAt(pp->pcrv, sHigh);
    int cBlocked = 0;
    for (;;)
    {
        const int fl = cBlocked == 1 ? 1 : 0;
        const int iStart = fReverse ? IpolyWrap(pp, i0 + 1) : i0;
        const int iEnd = fReverse ? IpolyWrap(pp, i1 + 1) : i1;
        glm::vec3 prev;
        EvalCrvS(pp->pcrv, sLow, &prev, nullptr);
        bool fBlocked = false;
        for (int i = iStart; i != iEnd; i = IpolyWrap(pp, fReverse ? i + 1 : i - 1))
        {
            const glm::vec3 p(pp->aposWorld[i]);
            if (FNavBlocked(preg, prev, p, fl))
            {
                fBlocked = true;
                break;
            }
            prev = p;
        }
        if (!fBlocked)
        {
            glm::vec3 q;
            EvalCrvS(pp->pcrv, sHigh, &q, nullptr);
            if (!FNavBlocked(preg, prev, q, fl)) return fl;
        }
        ++cBlocked;
        if (cBlocked >= 2) return 2;
    }
}

// orig 0x1BCCE0: which way around the polygon from s1 to *ps2 (1 / 2, 4 none)
int NPolyWay(NAVPOLY* pp, const NAVREGION* preg, float* ps2, int mode, float s1)
{
    const float len = SLengthCrv(pp->pcrv);
    float sA, sB;
    if (!FPolyPointOnRegion(pp, preg, s1, len * 0.5f, &sA, 0) && !FPolyPointOnRegion(pp, preg, s1, len * 0.5f, &sA, 1))
        return 4;
    if (!FPolyPointOnRegion(pp, preg, *ps2, len * 0.5f, &sB, 0) && !FPolyPointOnRegion(pp, preg, *ps2, len * 0.5f, &sB, 1))
        return 4;
    *ps2 = sB;
    float d = sB - sA;
    if (d < 0.0f) d += len;
    float g = 1.0f;
    if (mode == 2) g = 1.0f + S_TURN;       // (0x1BCCE0: mode 2 the longer way allowed, 1 the shorter)
    else if (mode == 1) g = 1.0f - S_TURN;
    const bool fShort = d < len * g * 0.5f;
    const int n1 = NPolyClearSide(pp, preg, true, sA, sB);
    const int n0 = NPolyClearSide(pp, preg, false, sA, sB);
    if (static const bool s_f = std::getenv("SLY2N_NAVWAYPRINT") != nullptr; s_f)
    {
        std::printf("navway: poly %d region %d sA %.1f sB %.1f len %.1f n1 %d n0 %d short %d mode %d\n", pp->plo ? pp->plo->oid : -1,
                    preg && preg->pmesh ? preg->pmesh->oid : -1, sA, sB, len, n1, n0, fShort ? 1 : 0, mode);
        for (const auto& v : pp->aposWorld) std::printf("  v %.0f %.0f %.0f\n", v.x, v.y, v.z);
    }
    if (n0 < n1) return 1;
    if (n1 < n0) return 2;
    if (n1 == 2) return 4;
    return fShort ? 2 : 1;
}
}  // namespace

// orig 0x1BD560 for the path records (0x1EA000)
int NNavPathLeavesPoly(NAVPOLY* pp, const CRV* pcrv, bool fForward, float* ps, float s)
{
    return NPathLeavesPoly(pp, pcrv, fForward, nullptr, ps, s);
}

// orig: 0x001E9F50 (the polygon of the point's region containing it, null when none)
NAVPOLY* PpolyAtPoint(const glm::vec3& pos)
{
    NAVREGION* preg = PregionAt(pos, false);
    if (!preg) return nullptr;
    NAVPOLY* ap[0x40];
    const int c = CpolyNear(preg, pos, pos, 0x40, ap);
    for (int i = 0; i < c; ++i)
        if (FPolyContains(ap[i], pos)) return ap[i];
    return nullptr;
}

// the GAME's stamp counter (+0x3D70), for the path records' last update
int NNextNavStamp() { return ++s_nav.nStamp; }

// orig: 0x001BD718
int NNavWay(const glm::vec3& pos, const NAVREGION* preg, const CRV* pcrv, int fForward, NAVPOLY** pppoly, float* ps, float s)
{
    const float t = SWrapCrv(pcrv, fForward ? s + DS_AHEAD : s - DS_AHEAD);
    glm::vec3 posAhead;
    EvalCrvS(pcrv, t, &posAhead, nullptr);
    NAVPOLY* pp = preg ? PpolyOnSegment(preg, pos, posAhead) : nullptr;
    if (pppoly) *pppoly = pp;
    if (!pp) return 0;
    glm::vec3 posExit;
    const int r = NPathLeavesPoly(pp, pcrv, fForward != 0, &posExit, nullptr, t);
    if (r != -1) return r;
    float s1 = 0, s2 = 0;
    NearestCrvAll(pp->pcrv, pos, nullptr, nullptr, nullptr, &s1, &AXIS_Z);
    NearestCrvAll(pp->pcrv, posExit, nullptr, nullptr, nullptr, &s2, &AXIS_Z);
    if (ps) *ps = s2;
    float* p2 = ps ? ps : &s2;
    return NPolyWay(pp, preg, p2, -1, s1);
}

// ---- the polygons' upkeep ----

NAVPOLY* PpolyOfLo(LO* plo)
{
    EnsureWorld();
    auto it = s_nav.mppoly.find(plo);
    return it == s_nav.mppoly.end() ? nullptr : it->second.get();
}

void MarkNavPolyDirty(LO* plo)
{
    EnsureWorld();
    if (std::find(s_nav.aploDirty.begin(), s_nav.aploDirty.end(), plo) == s_nav.aploDirty.end()) s_nav.aploDirty.push_back(plo);
}

// orig: 0x001BC198 (out of the world: its merged polygon dissolved, out of its region and the queue)
void RemoveNavPoly(LO* plo)
{
    EnsureWorld();
    auto ito = s_nav.mpowner.find(plo);
    if (ito != s_nav.mpowner.end() && ito->second) DissolveMerged(ito->second);
    RemovePolyFromRegion(PpolyOfLo(plo));
    Dequeue(plo);
}

// orig: 0x001BE1E8 (until the queue is empty: merged polygons queued on the way are built in the same pass)
void UpdateNavPolys()
{
    EnsureWorld();
    BuildLeaves();
    while (!s_nav.aploDirty.empty())
    {
        LO* plo = s_nav.aploDirty.front();
        s_nav.aploDirty.erase(s_nav.aploDirty.begin());
        auto& pp = s_nav.mppoly[plo];
        if (!pp)
        {
            pp = std::make_unique<NAVPOLY>();
            pp->plo = plo;
        }
        if (pp->pregion) RemovePolyFromRegion(pp.get());
        BuildPoly(pp.get());
        NAVREGION* preg = PregionAt(PosLoWorld(plo), false);
        if (preg) AddPolyToRegion(preg, pp.get());
    }
}

void SetRegionPolys(NAVREGION* preg, std::vector<std::shared_ptr<NAVPOLY>> apoly)
{
    EnsureWorld();
    preg->apoly.clear();
    for (auto& p : apoly)
    {
        preg->apoly.push_back(p.get());
        if (p)
        {
            p->pregion = preg;
            s_nav.apolyImported.push_back(p);
        }
    }
    preg->nStamp = ++s_nav.nStamp;
}

namespace
{
// the world update's pass 0x1BE1E8 (after 0x1F8DD8): the queued polygons rebuilt into their regions
struct INITNAVPASS
{
    INITNAVPASS()
    {
        RegisterSwPass(0x1BE1E8, [](SW*) { UpdateNavPolys(); });
    }
} s_initnavpass;
}

