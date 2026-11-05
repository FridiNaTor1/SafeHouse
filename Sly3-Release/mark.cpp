// The contacts between the attachments' geometry entries (Sly 1's mark.c; orig module 0x1A7C18-0x1AA978): edges
// clipped against the other entry's BSP into segments (LSG), the embedded vertices, edge-edge and point contacts
// (XP), swept vertices, sphere contacts. Ported from the reference's m_1A7C18_p3.cpp (oracle-verified), the
// decomp's 0x1A9C40 and the original's code for 0x1A9108 / 0x1A9B28 (only register-level transcriptions in the
// reference), m_14B1A0_p3.cpp (0x14C6A0 / 0x14C6D8 / 0x14C748).

#include "so.h"

#include "mat.h"
#include "vtnative.h"
#include "xfphys.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
constexpr float EPS = 1e-4f;
constexpr int INODE_MAX = INT_MAX;  // the original's 0x7FFFFFFF: no limit
inline float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
inline float Len3(const glm::vec3& a) { return std::sqrt(std::fabs(Dot3(a, a))); }

void VtXpNotify(ALO* palo, int slot, XP* pxp, int i)  // slots 55 (new), 56 (moved), 57 (updated): ALO nothing
{
    if (!palo) return;
    using PFN = void (*)(ALO*, XP*, int);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(palo, slot))) pfn(palo, pxp, i);
}
void XpNotifyNothing(ALO*, XP*, int) {}
NATIVEFN(0x1F4FB0, XpNotifyNothing);
NATIVEFN(0x1F4FB8, XpNotifyNothing);
NATIVEFN(0x1F4FC0, XpNotifyNothing);
inline glm::vec3 PosObj(const SOE* pe) { return pe && pe->pxf ? PosOf(pe->pxf->matWorld) : glm::vec3(0.0f); }

// A pruned or whole BSP of an entry: the nodes, the root and the planes (the entry's world geometry).
struct BSPSEL
{
    const std::vector<BSPN>* pa = nullptr;  // null: the entry's own tree (root 0)
    int iroot = IBSP_NONE;
    const SOE* psoe = nullptr;
    BSPREF Ref() const { return pa ? BSPREF(*pa, psoe->geomWorld) : BSPREF(psoe->bsp, psoe->geomWorld); }
};
inline bool FHasBsp(const SOE* pe) { return !pe->bsp.abspn.empty(); }
}

// orig: 0x001A7E10 FreeSwXpList__FP2SWP2XP (a list of contacts back to the world's pool)
void FreeSwXpList(XP* pxp)
{
    while (pxp)
    {
        XP* next = pxp->pxpNext;
        g_physw.FreeXp(pxp);
        pxp = next;
    }
}

// orig: 0x001A7FC0 func_001A7FC0 (the velocities of n contacts' ends (0x22D348), their objects told (slots 56, 57),
// the relative velocity)
void UpdateXpWorldPositions(int n, XP** apxp)
{
    for (int i = 0; i < n; i++)
    {
        XP* p = apxp[i];
        for (int j = 0; j < 2; j++)
        {
            XPD& e = p->axpd[j];
            if (!e.psoe) continue;
            e.vel = e.pxf ? VelXfPoint(e.pxf, nullptr, e.pos) : glm::vec3(0.0f);
            VtXpNotify(e.palo, 56, p, j);
        }
        for (int j = 0; j < 2; j++) VtXpNotify(p->axpd[j].palo, 57, p, j);
        p->dv = p->axpd[0].vel - p->axpd[1].vel;
    }
}

// orig: 0x001A7C18 func_001A7C18 (a new contact between the entries a and b at pos along dir (end 1 at pos - g dir),
// its kind and tags; its ends told (slot 55), its velocities; put in front of *ppxp)
XP* PxpAddSw(SOE* a, SOE* b, const glm::vec3& pos, const glm::vec3& dir, int xpk, const void* pv0, const void* pv1,
             XP** ppxp, float g)
{
    XP* pxp = g_physw.PxpNew();
    pxp->axpd[0].pos = pos;
    pxp->axpd[1].pos = pos + dir * -g;
    pxp->normal = dir;
    for (int i = 0; i < 2; i++)
    {
        XPD& e = pxp->axpd[i];
        e.psoe = i == 0 ? a : b;
        if (e.psoe)
        {
            e.pxf = e.psoe->pxf;
            e.palo = e.psoe->palo;
            OXA* poxa = e.palo ? Poxa(e.palo) : nullptr;
            e.paloRoot = poxa ? poxa->pso : nullptr;
        }
        e.posLocal = PosConvertXfs(nullptr, e.pxf, e.pos);
    }
    pxp->pv1 = pv1;
    pxp->gDist = g;
    pxp->xpk = xpk;
    pxp->mu = 0.5f;    // 0x2BFD30
    pxp->elas = 0.5f;  // 0x2BFD34
    pxp->pv0 = pv0;
    for (int i = 0; i < 2; i++) VtXpNotify(pxp->axpd[i].palo, 55, pxp, i);
    UpdateXpWorldPositions(1, &pxp);
    XP* t = pxp;
    while (t->pxpNext) t = t->pxpNext;
    t->pxpNext = *ppxp;
    *ppxp = pxp;
    return pxp;
}

// orig: 0x001A7E60 func_001A7E60 (the ends' positions again (kinds 4-6: end 0 on the sphere's surface along the
// normal; kinds 7, 9: end 1; else from the object's frame) and the separation along the normal)
void UpdateXpPositions(int n, XP** apxp)
{
    for (int i = 0; i < n; i++)
    {
        XP* p = apxp[i];
        for (int j = 0; j < 2; j++)
        {
            XPD& e = p->axpd[j];
            if (!e.psoe) continue;
            const int k = p->xpk;
            bool fLin = false;
            float g = 0.0f;
            if (j == 0)
            {
                if (k >= 4 && k < 7)
                {
                    g = -e.psoe->sRadius;
                    fLin = true;
                }
            }
            else if (k == 7 || k == 9)
            {
                g = e.psoe->sRadius;
                fLin = true;
            }
            if (fLin)
                e.pos = PosObj(e.psoe) + p->normal * g;
            else
                e.pos = PosConvertXfs(e.pxf, nullptr, e.posLocal);
        }
        p->gDist = Dot3(p->normal, p->axpd[0].pos - p->axpd[1].pos);
    }
}

namespace
{
// orig: 0x001A80D8 ClsgClipSoAllEdges__FP2SOT0P3BSPiP3LSG (every edge of the entry's world geometry clipped against
// the tree; the LSGs point at their edge)
int ClsgClipSoAllEdges(const SOE* pgeom, const BSPSEL& bsp, int clsgMax, LSG* alsg)
{
    int ilsg = 0, clsg = 0;
    const GEOM& g = pgeom->geomWorld;
    const BSPREF ref = bsp.Ref();
    for (int iedge = int(g.aedge.size()) - 1; iedge >= 0; iedge--)
    {
        const EDGE& e = g.aedge[iedge];
        clsg += ClipSegmentBsp(ref, bsp.iroot, INODE_MAX, g.apos[e.ipos0], g.apos[e.ipos1], clsgMax - clsg, alsg + clsg);
        for (; ilsg < clsg; ilsg++) alsg[ilsg].pedge = &e;
    }
    return clsg;
}

// orig: 0x001A81F8 func_001A81F8 (the edges between two of the pruned surfaces of pgeom (marks 1 / 2), clipped
// against pso's tree)
int ClsgClipNearEdges(const SOE* pso, const SOE* pgeom, const std::vector<BSPN>& aent, int clsgMax, LSG* alsg)
{
    int ilsg = 0, clsg = 0;
    const GEOM& g = pgeom->geomWorld;
    const int cent = int(aent.size());
    auto psurf = [&](int i) -> const SURF* {
        const int is = aent[i].isurf;
        return (is >= 0 && is < int(g.asurf.size())) ? &g.asurf[is] : nullptr;
    };
    for (int i = cent - 1; i >= 0; i--)
        if (const SURF* s = psurf(i)) s->grfMark |= 1u;
    const BSPREF ref(pso->bsp, pso->geomWorld);
    for (int i = cent - 1; i >= 0; i--)
    {
        const SURF* s = psurf(i);
        if (!s) continue;
        const uint32_t grf = s->grfMark;
        if (!(grf & 1u)) continue;
        if (grf & 2u) continue;
        s->grfMark = grf | 2u;
        for (int ie = s->iedgeFirst; ie >= 0; ie = g.aedge[ie].iedgeNextInSurf)
        {
            const EDGE& e = g.aedge[ie];
            if (e.isurfOther >= g.asurf.size()) continue;
            if (!(g.asurf[e.isurfOther].grfMark & 1u)) continue;
            clsg += ClipSegmentBsp(ref, 0, INODE_MAX, g.apos[e.ipos0], g.apos[e.ipos1], clsgMax - clsg, alsg + clsg);
            for (; ilsg < clsg; ilsg++) alsg[ilsg].pedge = &e;
        }
    }
    for (int i = cent - 1; i >= 0; i--)
        if (const SURF* s = psurf(i)) s->grfMark &= ~3u;
    return clsg;
}

// orig: 0x001A8410 CategorizeAlsg__FiP3LSGPiT2PP3LSGT4 (both ends embedded -> the edge list; neither -> the free list;
// one (made end 1, the ends swapped) -> grouped by the edge's vertex at end 0; then the one-LSG groups no free LSG
// touches go to the edge list)
void CategorizeAlsg(int clsg, LSG* alsg, int* pciposEmbed, int* aiposEmbed, LSG** aplsgEmbed, LSG** pplsgEdge)
{
    LSG* plsgFree = nullptr;
    int cipos = 0;
    *pplsgEdge = nullptr;
    for (int i = 0; i < clsg; i++)
    {
        LSG* plsg = &alsg[i];
        if (plsg->psurf0)
        {
            if (plsg->psurf1)
            {
                plsg->plsgNext = *pplsgEdge;
                *pplsgEdge = plsg;
                continue;
            }
            std::swap(plsg->pos0, plsg->pos1);
            std::swap(plsg->plane0, plsg->plane1);
            std::swap(plsg->t0, plsg->t1);
            std::swap(plsg->psurf0, plsg->psurf1);
            plsg->iend = 1;
        }
        else if (!plsg->psurf1)
        {
            plsg->plsgNext = plsgFree;
            plsgFree = plsg;
            continue;
        }
        const EDGE* pedge = plsg->pedge;
        const int ipos = plsg->iend ? pedge->ipos1 : pedge->ipos0;
        int j = 0;
        while (j < cipos && aiposEmbed[j] != ipos) j++;
        if (j < cipos)
            plsg->plsgNext = aplsgEmbed[j];
        else
        {
            aiposEmbed[j] = ipos;
            cipos++;
        }
        aplsgEmbed[j] = plsg;
    }
    for (int j = 0; j < cipos;)
    {
        LSG* plsg = aplsgEmbed[j];
        if (plsg->plsgNext)
        {
            j++;
            continue;
        }
        const int ipos = aiposEmbed[j];
        bool fTouched = false;
        for (LSG* p = plsgFree; p; p = p->plsgNext)
            if (p->pedge->ipos0 == ipos || p->pedge->ipos1 == ipos)
            {
                fTouched = true;
                break;
            }
        if (fTouched)
        {
            j++;
            continue;
        }
        plsg->plsgNext = *pplsgEdge;
        cipos--;
        *pplsgEdge = aplsgEmbed[j];
        for (int k = j; k < cipos; k++)
        {
            aplsgEmbed[k] = aplsgEmbed[k + 1];
            aiposEmbed[k] = aiposEmbed[k + 1];
        }
    }
    *pciposEmbed = cipos;
}

// orig: 0x001A86B8 func_001A86B8 (a breadth-first search, at most 64 nodes, over the LSG list's edges from face
// isurfStart to face isurfEnd (faces visited: mark 4); the path's LSGs (the last first); 0 without a path)
int CPathAlsg(const GEOM& g, int isurfStart, int isurfEnd, LSG* plsgFirst, LSG** aplsg)
{
    struct NODE
    {
        int iparent;
        LSG* plsg;
        int isurf;
    };
    NODE an[64];
    an[0] = NODE{-1, nullptr, isurfStart};
    int cnode = 1, c = 0;
    bool fFound = false;
    for (int inode = 0; inode < cnode && !fFound; inode++)
    {
        for (LSG* plsg = plsgFirst; plsg; plsg = plsg->plsgNext)
        {
            const EDGE* pedge = plsg->pedge;
            const int isurfNode = an[inode].isurf;
            int isurf;
            if (pedge->isurf == isurfNode) isurf = pedge->isurfOther;
            else if (pedge->isurfOther == isurfNode) isurf = pedge->isurf;
            else continue;
            if (isurf == isurfEnd)
            {
                aplsg[0] = plsg;
                c = 1;
                for (int in = inode; an[in].plsg; in = an[in].iparent) aplsg[c++] = an[in].plsg;
                fFound = true;
                break;
            }
            if (isurf >= int(g.asurf.size())) continue;
            const uint32_t grf = g.asurf[isurf].grfMark;
            if (grf & 4u) continue;
            g.asurf[isurf].grfMark = grf | 4u;
            if (cnode >= 64) continue;  // (native guard: the original's 64 nodes on the stack)
            an[cnode++] = NODE{inode, plsg, isurf};
        }
    }
    if (!fFound) c = 0;
    for (int inode = 0; inode < cnode; inode++)
        if (an[inode].isurf >= 0 && an[inode].isurf < int(g.asurf.size())) g.asurf[an[inode].isurf].grfMark &= ~4u;
    return c;
}

// orig: 0x001A87F0 func_001A87F0 (for each embedded vertex of pso (its LSGs grouped): the plane (an LSG's surface at
// end 1, each once: mark 8) leaving the group's other ends deepest; a kind-1 contact at the vertex along it, then
// kind-2 contacts at the LSGs' end 1 points below it)
void MarkEmbeddedVertices(SOE* pso, SOE* psoOther, int cipos, const int* aipos, LSG* const* aplsg, XP** ppxp)
{
    glm::vec4 planeBest(0.0f);  // (kept across the groups when none is picked, as the original's stack)
    float f24 = 0.0f;
    const GEOM& g = pso->geomWorld;
    for (int i = 0; i < cipos; i++)
    {
        const SURF* best = nullptr;
        const int ipos = aipos[i];
        LSG* plsgFirst = aplsg[i];
        float f23 = -FLT_MAX;  // 0x2A6AD0
        const glm::vec3 pos = g.apos[ipos];
        for (LSG* s0 = plsgFirst; s0; s0 = s0->plsgNext)
        {
            const SURF* s2 = s0->psurf1;
            if (!s2) continue;
            if (s2->grfMark & 8u) continue;
            s2->grfMark |= 8u;
            const glm::vec4 plane = PlaneOfSurf(*s2);
            const float f22 = PlaneDist(plane, pos);
            float f21 = f22;
            for (LSG* s1 = plsgFirst; s1; s1 = s1->plsgNext)
            {
                const EDGE* pedge = s1->pedge;
                const int iend = 1 - s1->iend;
                const glm::vec3 p = g.apos[iend ? pedge->ipos1 : pedge->ipos0];
                const float f20 = PlaneDist(plane, p);
                if (f20 < f21) f21 = f20;
            }
            if (f23 < f21)
            {
                planeBest = plane;
                best = s2;
                f23 = f21;
                f24 = f22;
            }
        }
        for (LSG* s0 = plsgFirst; s0; s0 = s0->plsgNext)
            if (s0->psurf1) s0->psurf1->grfMark &= ~8u;
        PxpAddSw(pso, psoOther, pos, glm::vec3(planeBest), 1, &g.apos[ipos], best, ppxp, f24);
        for (LSG* s1 = plsgFirst; s1; s1 = s1->plsgNext)
        {
            const float f20 = PlaneDist(planeBest, s1->pos1);
            if (!(f20 < f24)) continue;
            PxpAddSw(pso, psoOther, s1->pos1, glm::vec3(planeBest), 2, s1->pedge, best, ppxp, f20);
        }
    }
}

// orig: 0x001A8B50 func_001A8B50 (Sly 1's MarkSoContactsBspBsp: pso0's edges against pso1's (pruned) tree and pso1's
// pruned surfaces' edges against pso0's (0x80 LSGs each); the embedded vertices' contacts both ways; for each of
// set 0's edge LSGs: edge-edge contacts (kind 3) along the path of set 1's edges between its faces, or when there is
// none and no set-1 LSG ends on its edge's faces, a kind-1 contact at its deeper end unless one is there; when
// nothing was added and only one side clipped, a kind-13 contact at pso0's position)
void MarkBspBsp(SOE* pso0, SOE* pso1, const std::vector<BSPN>& absp, const BSPSEL& bsp1, XP** ppxp)
{
    XP* pxpFirst = *ppxp;
    static thread_local std::vector<LSG> s_alsg0, s_alsg1;
    s_alsg0.assign(0x80, LSG{});
    s_alsg1.assign(0x80, LSG{});
    const int clsg0 = ClsgClipSoAllEdges(pso0, bsp1, 0x80, s_alsg0.data());
    const int clsg1 = ClsgClipNearEdges(pso0, pso1, absp, 0x80, s_alsg1.data());
    if (clsg0 == 0 && clsg1 == 0) return;
    int acipos[2] = {0, 0};
    int aaipos[2][0x80];
    LSG* aaplsg[2][0x80];
    LSG* aplsgEdge[2] = {nullptr, nullptr};
    CategorizeAlsg(clsg0, s_alsg0.data(), &acipos[0], aaipos[0], aaplsg[0], &aplsgEdge[0]);
    CategorizeAlsg(clsg1, s_alsg1.data(), &acipos[1], aaipos[1], aaplsg[1], &aplsgEdge[1]);
    MarkEmbeddedVertices(pso0, pso1, acipos[0], aaipos[0], aaplsg[0], ppxp);
    MarkEmbeddedVertices(pso1, pso0, acipos[1], aaipos[1], aaplsg[1], ppxp);
    const GEOM& g0 = pso0->geomWorld;
    const GEOM& g1 = pso1->geomWorld;
    auto isurfOf = [](const GEOM& g, const SURF* s) { return int(s - g.asurf.data()); };
    for (LSG* s2 = aplsgEdge[0]; s2; s2 = s2->plsgNext)
    {
        if (!s2->psurf0 || !s2->psurf1) continue;
        LSG* apath[0x40];
        const int c = CPathAlsg(g1, isurfOf(g1, s2->psurf0), isurfOf(g1, s2->psurf1), aplsgEdge[1], apath);
        if (c > 0)
        {
            for (int j = 0; j < c; j++)
            {
                const EDGE* edgeA = s2->pedge;
                const EDGE* edgeB = apath[j]->pedge;
                const glm::vec3 a1 = g0.apos[edgeA->ipos1], a0 = g0.apos[edgeA->ipos0];
                const glm::vec3 b1 = g1.apos[edgeB->ipos1], b0 = g1.apos[edgeB->ipos0];
                float uv[2] = {0.0f, 0.0f};
                glm::vec3 normal(0.0f);
                FClosestPointsLines(a0, a1, b0, b1, uv, &normal);
                const float u = uv[0], v = uv[1];
                const glm::vec3 pa = a0 * (1.0f - u) + a1 * u;
                const glm::vec3 pb = b0 * (1.0f - v) + b1 * v;
                const float len = Len3(pa - pb);
                PxpAddSw(pso0, pso1, pa, normal, 3, edgeA, edgeB, ppxp, -len);
            }
            continue;
        }
        bool fShared = false;
        const EDGE* edgeA = s2->pedge;
        const SURF* v0 = edgeA->isurf < g0.asurf.size() ? &g0.asurf[edgeA->isurf] : nullptr;
        const SURF* v1 = edgeA->isurfOther < g0.asurf.size() ? &g0.asurf[edgeA->isurfOther] : nullptr;
        for (int j = 0; j < clsg1; j++)
        {
            const LSG& l = s_alsg1[j];
            if (l.psurf0 == v0 || l.psurf0 == v1 || l.psurf1 == v0 || l.psurf1 == v1)
            {
                fShared = true;
                break;
            }
        }
        if (fShared) continue;
        const glm::vec3 posObj = PosObj(pso0);
        const float d20 = PlaneDist(s2->plane0, posObj);
        const float d30 = PlaneDist(s2->plane1, posObj);
        int iend;
        const SURF* s1;
        if (d30 < d20)
        {
            iend = 1 - s2->iend;
            s1 = s2->psurf0;
        }
        else
        {
            iend = s2->iend;
            s1 = s2->psurf1;
        }
        const glm::vec3* s0 = &g0.apos[iend ? s2->pedge->ipos1 : s2->pedge->ipos0];
        XP* pxp = *ppxp;
        while (pxp && !(pxp->xpk == 1 && pxp->pv0 == s0 && pxp->pv1 == s1)) pxp = pxp->pxpNext;
        if (pxp) continue;
        const glm::vec4 plane = PlaneOfSurf(*s1);
        const float g = PlaneDist(plane, *s0);
        PxpAddSw(pso0, pso1, *s0, glm::vec3(plane), 1, s0, s1, ppxp, g);
    }
    if (*ppxp != pxpFirst) return;
    if (clsg0 != 0 && clsg1 != 0) return;
    PxpAddSw(pso0, pso1, PosObj(pso0), g_normalZ, 0xD, nullptr, nullptr, ppxp, 0.0f);
}

// orig: 0x0014C748 func_0014C748 (the segment a-b through the sphere (c, r): the entry / exit points and their
// planes into an LSG; 0 when it misses)
int ClipSegmentSphere(const glm::vec3& c, const glm::vec3& a, const glm::vec3& b, int cMax, LSG* plsg, float r)
{
    if (cMax <= 0) return 0;
    const float r2 = r * r;
    const glm::vec3 ac = a - c;
    const float lac = Dot3(ac, ac);
    const glm::vec3 d = b - a;
    const float C = lac - r2;
    const float dot = Dot3(d, ac);
    const float A = Dot3(d, d);
    const float C4 = C * 4.0f;
    const float B = dot + dot;
    const float disc = B * B - A * C4;
    if (disc < EPS) return 0;
    const float s = std::sqrt(disc);
    const float A2 = A + A;
    const float nb = -B;
    float t1 = (nb - s) / A2, t2 = (nb + s) / A2;
    if (t1 < 0.0f && t2 < 0.0f) return 0;
    if (1.0f < t1 && 1.0f < t2) return 0;
    t2 = std::min(1.0f, t2);
    t1 = std::max(0.0f, t1);
    *plsg = LSG{};
    const float at[2] = {t1, t2};
    for (int k = 0; k < 2; k++)
    {
        const float t = at[k];
        const float u = 1.0f - t;
        (k == 0 ? plsg->t0 : plsg->t1) = t;
        const glm::vec3 pt = b * t + a * u;
        const glm::vec3 v = pt - c;
        (k == 0 ? plsg->pos0 : plsg->pos1) = pt;
        const float len = Len3(v);
        const glm::vec3 n = len < EPS ? g_normalX : v * (1.0f / len);
        (k == 0 ? plsg->plane0 : plsg->plane1) = PlaneFromNormalPos(n, pt);
    }
    plsg->fSphere = 1;
    return 1;
}

// orig: 0x0014C6A0 / 0x0014C6D8 (a segment's pieces inside an entry: its tree, else (flag 49) its sphere)
int ClipSegmentSoe(const SOE* p, const glm::vec3& a, const glm::vec3& b, int c, LSG* alsg)
{
    if (FHasBsp(p)) return ClipSegmentBsp(BSPREF(p->bsp, p->geomWorld), 0, INODE_MAX, a, b, c, alsg);
    if (!p->f49) return 0;
    return ClipSegmentSphere(PosObj(p), a, b, c, alsg, p->sRadius);
}

// orig: 0x001A9D38 func_001A9D38 (the swept vertices of a (pinned .. local count: from their position at the last
// matrix copy to their world position) against b (in b's frame of then when it moved): a kind-8 contact (kind 9 with
// no tag when b is a sphere) at the first hit, along the sweep, separated by the deepest of a's points)
void MarkSweptVertices(SOE* a, SOE* b, XP** ppxp)
{
    LSG alsg[2];
    for (int i = a->pinned; i < int(a->geomLocal.apos.size()) && i < int(a->geomWorld.apos.size());)
    {
        glm::vec3 vf6 = XformPos(a->mat, a->geomLocal.apos[i]);
        if (b->nStampWorld != b->nStampObj)
        {
            vf6 = XformPos(InverseAffineMatrix4(b->mat), vf6);
            vf6 = XformPos(b->pxf ? b->pxf->matWorld : g_matIdentity4, vf6);
        }
        const glm::vec3 dest = a->geomWorld.apos[i];
        i++;
        const int c = ClipSegmentSoe(b, vf6, dest, 2, alsg);
        if (c == 0) continue;
        const int skip = alsg[0].t0 == 0.0f ? 1 : 0;
        if (!(skip < c)) continue;
        const glm::vec3 d = vf6 - dest;
        const float len = Len3(d);
        vf6 = alsg[0].pos0;
        const glm::vec3 vf5 = d * (1.0f / len);
        const void* tag = alsg[0].psurf0;
        float g = 0.0f;
        for (const glm::vec3& pp : a->geomWorld.apos)
        {
            const float t = Dot3(pp - vf6, vf5);
            if (t < g) g = t;
        }
        if (a->f49)
        {
            const float t = Dot3(PosObj(a) - vf6, vf5) - a->sRadius;
            if (t < g) g = t;
        }
        const bool fSphere = b->f49;
        PxpAddSw(a, b, vf6, vf5, fSphere ? 9 : 8, fSphere ? nullptr : tag, nullptr, ppxp, g);
    }
}

// A contact of a sphere with a geometry (orig SBI, 0x30 bytes).
struct SBI
{
    glm::vec3 pos{0.0f};
    glm::vec4 plane{0.0f};
    int kind = 0;
    const void* pv = nullptr;
};

// orig: 0x001A9108 func_001A9108 (a sphere inside a geometry: the tree pruned by a growing radius (from max(r, 100)
// doubling below 50000); for each pruned surface a segment from outside along its normal to the centre, clipped by
// the pruned tree: the hit nearest the centre gives one kind-12 contact)
void SphereInsideContacts(const SOE* pso, const glm::vec3& pos, SBI* asbi, float r, int inodeLimit = INODE_MAX)
{
    float f20 = std::max(r, 100.0f);
    std::vector<BSPN> abuf;
    const BSPREF ref(pso->bsp, pso->geomWorld);
    while (f20 < 50000.0f)
    {
        abuf.clear();
        int cnodeLeft = 0x1555;
        const int v = ClipBspPlane(ref, 0, inodeLimit, pos, f20, &cnodeLeft, &abuf);
        if (v == 2)
        {
            float f21 = 0.0f;
            const BSPREF refPruned(abuf, pso->geomWorld);
            const int iroot = int(abuf.size()) - 1;
            for (const BSPN& n : abuf)
            {
                const SURF* ps = refPruned.Psurf(int(&n - abuf.data()));
                if (!ps) continue;
                if (ps->grfMark & 0x1000u) continue;
                ps->grfMark |= 0x1000u;
                const glm::vec3 p0 = pos + ps->normal * (f20 + 1.0f);
                LSG alsg[16];
                const int c = ClipSegmentBsp(refPruned, iroot, INODE_MAX, p0, pos, 16, alsg);
                if (c > 0 && f21 < alsg[0].t0)
                {
                    f21 = alsg[0].t0;
                    const glm::vec3 d = p0 - pos;
                    const glm::vec3 n = d * (1.0f / Len3(d));
                    asbi[0].pos = alsg[0].pos0;
                    asbi[0].plane = PlaneFromNormalPos(n, alsg[0].pos0);
                    asbi[0].kind = 12;
                    asbi[0].pv = nullptr;
                }
            }
            for (const BSPN& n : abuf)
                if (const SURF* ps = refPruned.Psurf(int(&n - abuf.data()))) ps->grfMark &= ~0x1000u;
            if (0.0f < f21) return;
        }
        f20 = f20 + f20;
    }
}

// orig: 0x001A93C8 func_001A93C8 (Sly 1's CsbiIntersectSphereBsp: the sphere's contacts with the entry's geometry
// (the pruned tree given, or pruned here by the sphere): faces its normal segment hits (kind 6), edges (5) and
// vertices (4, unless an edge contact uses them) within r; when none, a sphere inside gets 0x1A9108's)
int SphereBspContacts(const glm::vec3& P, const SOE* pso, const std::vector<BSPN>* pabsp, int csbiMax, SBI* asbi, float r,
                      int inodeLimit = INODE_MAX)
{
    int csbi = 0;
    if (csbiMax <= 0) return 0;
    std::vector<BSPN> abuf;
    const std::vector<BSPN>* pa = pabsp;
    bool fTail = false;
    const GEOM& g = pso->geomWorld;
    if (!pa)
    {
        int c = int(pso->bsp.x4);
        if (0x1554 < c) c = 0x1555;
        const int v = ClipBspPlane(BSPREF(pso->bsp, g), 0, inodeLimit, P, r, &c, &abuf);
        if (v == 0) fTail = true;
        else if (v == 1) return 0;
        pa = &abuf;
    }
    if (!fTail)
    {
        const std::vector<BSPN>& absp = *pa;
        const int cbsp = int(absp.size());
        const BSPREF ref(absp, g);
        const int iroot = cbsp - 1;
        auto surf_at = [&](int i) { return ref.Psurf(i); };
        for (int i = cbsp - 1; i >= 0; i--)
            if (const SURF* s = surf_at(i)) s->grfMark |= 0x10u;
        bool fFull = false;
        const float nr = -r;
        for (int i = cbsp - 1; i >= 0 && !fFull; i--)
        {
            const SURF* s0 = surf_at(i);
            if (!s0) continue;
            const uint32_t grf = s0->grfMark;
            if (!(grf & 0x10u) || (grf & 0x40u)) continue;
            s0->grfMark = grf | 0x40u;
            const glm::vec3 p0 = P + s0->normal * r;
            const glm::vec3 p1 = P + s0->normal * nr;
            LSG alsg[0x10];
            const int clsg = ClipSegmentBsp(ref, iroot, INODE_MAX, p0, p1, 0x10, alsg);
            bool fHit = false;
            for (int j = 0; j < clsg; j++)
            {
                const float u = alsg[j].t0 + alsg[j].t1;
                const SURF* ps = (1.0f < u) ? alsg[j].psurf0 : alsg[j].psurf1;
                if (ps == s0)
                {
                    fHit = true;
                    break;
                }
            }
            if (!fHit) continue;
            if (csbi < csbiMax)
            {
                SBI& sbi = asbi[csbi++];
                sbi.kind = 6;
                sbi.plane = PlaneOfSurf(*s0);
                sbi.pv = s0;
                sbi.pos = P + s0->normal * nr;
                if (!(csbi < csbiMax))
                {
                    fFull = true;
                    break;
                }
            }
            s0->grfMark |= 0x80u;
        }
        if (!fFull)
        {
            const int csbiFaces = csbi;
            struct VTXC
            {
                int ipos;
                glm::vec3 c;
                glm::vec4 plane;
            };
            std::vector<VTXC> av;
            for (int i = cbsp - 1; i >= 0 && !fFull; i--)
            {
                const SURF* s0 = surf_at(i);
                if (!s0) continue;
                const uint32_t grf = s0->grfMark;
                if (!(grf & 0x10u)) continue;
                if (grf & 0xA0u) continue;
                s0->grfMark = grf | 0x20u;
                for (int ie = s0->iedgeFirst; ie >= 0; ie = g.aedge[ie].iedgeNextInSurf)
                {
                    const EDGE& s1 = g.aedge[ie];
                    if (s1.isurfOther >= g.asurf.size()) continue;
                    const uint32_t g2 = g.asurf[s1.isurfOther].grfMark;
                    if (!(g2 & 0x10u)) continue;
                    if (g2 & 0x80u) continue;
                    const int i0 = s1.ipos0, i1 = s1.ipos1;
                    const glm::vec3 pp0 = g.apos[i0], pp1 = g.apos[i1];
                    const glm::vec3 e = pp1 - pp0;
                    const float len2 = Dot3(e, e);
                    glm::vec3 d = P - pp0;
                    const float u = Dot3(d, e) / len2;
                    int ipos;
                    glm::vec3 ppt;
                    if (u < EPS)
                    {
                        ipos = i0;
                        ppt = pp0;
                    }
                    else if (0.999899983f < u)  // 0x3F7FF972
                    {
                        ppt = pp1;
                        ipos = i1;
                        d = P - pp1;
                    }
                    else
                    {
                        ipos = -1;
                        ppt = pp0 + e * u;
                        d = P - ppt;
                    }
                    if (r * r < Dot3(d, d)) continue;
                    if (ipos >= 0)
                    {
                        bool fDup = false;
                        for (int k = int(av.size()) - 1; k >= 0; k--)
                            if (av[k].ipos == ipos)
                            {
                                fDup = true;
                                break;
                            }
                        if (fDup) continue;
                    }
                    const glm::vec3 n = d * (1.0f / Len3(d));
                    const glm::vec4 plane = PlaneFromNormalPos(n, ppt);
                    const glm::vec3 c = P + n * -r;
                    if (ipos >= 0)
                    {
                        if (!(av.size() < 0x100)) continue;
                        av.push_back(VTXC{ipos, c, plane});
                    }
                    else
                    {
                        if (!(csbi < csbiMax)) continue;
                        SBI& sbi = asbi[csbi++];
                        sbi.pos = c;
                        sbi.plane = plane;
                        sbi.kind = 5;
                        sbi.pv = &s1;
                        if (!(csbi < csbiMax))
                        {
                            fFull = true;
                            break;
                        }
                    }
                }
            }
            if (!fFull)
            {
                const int t3 = csbi;
                for (int k = int(av.size()) - 1; k >= 0; k--)
                {
                    const int ipos = av[k].ipos;
                    bool fCovered = false;
                    for (int m = csbiFaces; m < t3; m++)
                    {
                        const EDGE* pe = static_cast<const EDGE*>(asbi[m].pv);
                        if (pe->ipos0 == ipos || pe->ipos1 == ipos)
                        {
                            fCovered = true;
                            break;
                        }
                    }
                    if (fCovered) continue;
                    if (!(csbi < csbiMax)) continue;
                    SBI& sbi = asbi[csbi++];
                    sbi.pos = av[k].c;
                    sbi.kind = 4;
                    sbi.plane = av[k].plane;
                    sbi.pv = &g.apos[ipos];
                    if (!(csbi < csbiMax)) break;
                }
            }
        }
        for (int i = cbsp - 1; i >= 0; i--)
            if (const SURF* s = surf_at(i)) s->grfMark &= 0xFFFFFF0Fu;
    }
    if (csbi == 0)
    {
        if (FHasBsp(pso) && IbspFindPointEps(BSPREF(pso->bsp, g), 0, inodeLimit, P) != IBSP_NONE)
        {
            csbi = 1;
            asbi[0] = SBI{};
            SphereInsideContacts(pso, P, asbi, r, inodeLimit);
        }
    }
    return csbi;
}

// orig: 0x001A9B28 func_001A9B28 (the sphere entry a against b's geometry (0x1A93C8, at most 64): a contact per SBI,
// the last first, separated by the SBI point's distance from its plane)
void MarkSphereBsp(SOE* a, SOE* b, const std::vector<BSPN>* pabsp, XP** ppxp)
{
    SBI asbi[64];
    const int c = SphereBspContacts(PosObj(a), b, pabsp, 64, asbi, a->sRadius);
    for (int i = c - 1; i >= 0; i--)
    {
        const SBI& s = asbi[i];
        const float g = PlaneDist(s.plane, s.pos);
        PxpAddSw(a, b, s.pos, glm::vec3(s.plane), s.kind, nullptr, s.pv, ppxp, g);
    }
}

// orig: 0x001A9C40 func_001A9C40 (two spheres: a kind-7 contact on a's surface along the centres' direction when
// they overlap)
void MarkSphereSphere(SOE* a, SOE* b, XP** ppxp)
{
    const glm::vec3 pa = PosObj(a), pb = PosObj(b);
    const float rsum = a->sRadius + b->sRadius;
    const float r2 = rsum * rsum;
    const glm::vec3 d = pa - pb;
    const float len2 = Dot3(d, d);
    if (r2 < len2) return;
    float g;
    glm::vec3 dir;
    if (len2 < EPS)
    {
        g = 0.0f;
        dir = g_normalZ;
    }
    else
    {
        g = std::sqrt(len2);
        dir = d * (1.0f / g);
    }
    const glm::vec3 pos = pa + dir * -a->sRadius;
    PxpAddSw(a, b, pos, dir, 7, nullptr, nullptr, ppxp, g - rsum);
}

}  // namespace

// orig: 0x0014C748 (public entry to the same clip used by the physics queries)
int ClipSegmentToSphere(const glm::vec3& center, const glm::vec3& a, const glm::vec3& b,
                        int countMax, LSG* result, float radius)
{
    return ClipSegmentSphere(center, a, b, countMax, result, radius);
}

// orig: 0x001A9FE0 func_001A9FE0 (a list put in front of *ppxp)
void PrependXpList(XP* first, XP** phead)
{
    if (!first) return;
    XP* last = first;
    while (last->pxpNext) last = last->pxpNext;
    last->pxpNext = *phead;
    *phead = first;
}

// orig: 0x001AA020 func_001AA020 (a copy of a list from the pool)
XP* PxpCloneList(XP* head)
{
    XP* first = nullptr;
    XP** pp = &first;
    for (XP* p = head; p; p = p->pxpNext)
    {
        XP* n = g_physw.PxpNew();
        *n = *p;
        n->pxpNext = nullptr;
        *pp = n;
        pp = &n->pxpNext;
    }
    return first;
}

// orig: 0x001F78D8 / 0x001F7530 (the first ALO up the parents with one of the contact bytes f set)
ALO* PaloXpFilter(ALO* p, uint32_t f)
{
    for (; p; p = p->paloParent)
    {
        uint32_t r = 0, m = 1;
        if (p->psort)
            for (int i = 0; i < 26; i++, m <<= 1)
                if (m & f) r |= p->psort->abXp[i];
        if (r) return p;
    }
    return nullptr;
}

namespace
{
// orig: 0x001AA0B0 func_001AA0B0 (the entry's owner's own contact test (slot 60; ALO: 0x1F5990, none) with k;
// a sensor's contacts whose other side passes its filter copied to l's sensor list; the result into *ppxp)
void MarkOwnContacts(OXA* o, OXA* l, SOE* k, XP** ppxp)
{
    ALO* vo = o->pso;
    XP* res = nullptr;
    using PFN = void (*)(ALO*, SOE*, XP**);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(vo, 60))) pfn(vo, k, k->id ? &res : ppxp);
    if (!res) return;
    for (XP* e = res; e; e = e->pxpNext)
    {
        const int idx = (e->axpd[0].psoe != k) ? 0 : 1;
        if (PaloXpFilter(e->axpd[idx].palo, k->id))
        {
            XP* n = g_physw.PxpNew();
            *n = *e;
            n->pxpNext = l->pxpSensor;
            l->pxpSensor = n;
        }
    }
    PrependXpList(res, ppxp);
}
void OwnContactsNothing(ALO*, SOE*, XP**) {}
NATIVEFN(0x1F5990, OwnContactsNothing);
}

// orig: 0x001AA1D8 func_001AA1D8 (Sly 1's AddSoXps: the contacts between two attachments' entries; s5 the one with the
// smaller radius; per pair of entries (sensors (bit 48) only with a filter (+0x18) that accepts the other; spheres
// bit 49) the swept vertices, then sphere-sphere, sphere-BSP or BSP-BSP contacts (the big one's tree pruned by the
// small one's sphere); sensors' contacts to their own lists; then the entries' own tests. Returns the new list.)
XP* PxpMarkOxaContacts(OXA* psoA, OXA* psoB)
{
    OXA* s5 = psoA;
    OXA* fp = psoB;
    XP* result = nullptr;
    if (fp->sRadius < s5->sRadius)
    {
        s5 = psoB;
        fp = psoA;
    }
    int cmax = 0;
    for (SOE* s1 : fp->apsoe) cmax = std::max(cmax, int(s1->bsp.x4));
    if (0x1554 < cmax) cmax = 0x1555;
    std::vector<BSPN> abuf;
    for (SOE* s1 : fp->apsoe)
    {
        const bool f18 = s1->id != 0;
        if (s1->f48 && !f18) continue;
        int kind = 2;
        BSPSEL bsp1;
        bsp1.psoe = s1;
        const std::vector<BSPN>* pabsp = nullptr;  // the pruned nodes (null: none)
        static const std::vector<BSPN> s_aEmpty;
        if (s1->f49 || (s5->grf & 0x20))
        {
            bsp1.pa = &s_aEmpty;
            bsp1.iroot = IBSP_NONE;
            pabsp = &s_aEmpty;
        }
        else
        {
            abuf.clear();
            int cnodeLeft = cmax;
            kind = FHasBsp(s1) ? ClipBspPlane(BSPREF(s1->bsp, s1->geomWorld), 0, INODE_MAX, s5->posCenter, s5->sRadius,
                                              &cnodeLeft, &abuf)
                               : 1;
            if (kind == 1) continue;
            if (kind != 0)
            {
                bsp1.pa = &abuf;
                bsp1.iroot = int(abuf.size()) - 1;
                pabsp = &abuf;
            }
            else
            {
                bsp1.pa = &s_aEmpty;
                bsp1.iroot = IBSP_SOLID;
                pabsp = &s_aEmpty;
            }
        }
        for (SOE* s0 : s5->apsoe)
        {
            static const int s_oidDbg = std::getenv("SLY2N_MARKDEBUG") ? std::atoi(std::getenv("SLY2N_MARKDEBUG")) : 0;
            if (s_oidDbg && (s5->pso->oid == s_oidDbg || fp->pso->oid == s_oidDbg))
                std::printf("mark %d/%d: s1 tag %d id 0x%X f48 %d f49 %d (filter on s0's %d: %d) / s0 tag %d id 0x%X f48 %d f49 %d (filter on s1's %d: %d)\n",
                            fp->pso->oid, s5->pso->oid, s1->tag, s1->id, s1->f48, s1->f49, s0->palo ? s0->palo->oid : -1,
                            s1->id ? PaloXpFilter(s0->palo, s1->id) != nullptr : -1, s0->tag, s0->id, s0->f48, s0->f49,
                            s1->palo ? s1->palo->oid : -1, s0->id ? PaloXpFilter(s1->palo, s0->id) != nullptr : -1);
            const bool s6 = s0->id != 0;
            if (s0->f48 && !s6) continue;
            if (s1->f48 && s0->f48) continue;
            XP* sensorList = nullptr;
            XP** s2 = &result;
            if (f18)
            {
                const bool ok = PaloXpFilter(s0->palo, s1->id) != nullptr;
                if (s1->f48)
                {
                    if (!ok) continue;
                    s2 = &sensorList;
                }
                else if (ok)
                    s2 = s0->f48 ? &result : &sensorList;
            }
            if (s6)
            {
                const bool ok = PaloXpFilter(s1->palo, s0->id) != nullptr;
                if (s0->f48)
                {
                    if (!ok) continue;
                    s2 = &sensorList;
                }
                else if (ok)
                {
                    if (!s1->f48) s2 = &sensorList;
                }
            }
            XP* const s4 = *s2;
            if (s0->nStampObj != s0->nStampWorld || s1->nStampObj != s1->nStampWorld)
            {
                if (s0->pinned < int(s0->geomLocal.apos.size())) MarkSweptVertices(s0, s1, s2);
                if (s1->pinned < int(s1->geomLocal.apos.size())) MarkSweptVertices(s1, s0, s2);
            }
            if (*s2 == s4)
            {
                if (s1->f49)
                {
                    if (s0->f49) MarkSphereSphere(s0, s1, s2);
                    else MarkSphereBsp(s1, s0, nullptr, s2);
                }
                else if (s0->f49)
                {
                    bool fFar = true;
                    if (kind != 0)
                    {
                        const glm::vec3 d = PosObj(s1) - s5->posCenter;
                        fFar = 1.0f < Dot3(d, d);
                        if (!fFar) fFar = 1.0f < std::fabs(s1->sRadius - s5->sRadius);
                    }
                    MarkSphereBsp(s0, s1, fFar ? nullptr : pabsp, s2);
                }
                else
                    MarkBspBsp(s0, s1, *pabsp, bsp1, s2);
            }
            XP* a1 = sensorList;
            if (!a1) continue;
            bool fOwn = false;
            if (f18)
            {
                if (s1->f48)
                    PrependXpList(a1, &fp->pxpSensor);
                else if (!s0->f48)
                {
                    fOwn = true;
                    PrependXpList(PxpCloneList(a1), &fp->pxpSensor);
                }
            }
            if (s6)
            {
                XP* a1b = sensorList;
                if (s0->f48)
                    PrependXpList(a1b, &s5->pxpSensor);
                else if (!s1->f48)
                {
                    fOwn = true;
                    PrependXpList(PxpCloneList(a1b), &s5->pxpSensor);
                }
            }
            if (fOwn) PrependXpList(sensorList, &result);
        }
        if (s1->f48) continue;
        MarkOwnContacts(s5, fp, s1, &result);
    }
    for (SOE* s0 : s5->apsoe)
    {
        if (s0->f48) continue;
        MarkOwnContacts(fp, s5, s0, &result);
    }
    return result;
}

// orig: 0x001A8B50 (public entry point, also used by physcheck.cpp)
void MarkBspBspContacts(SOE* pso0, SOE* pso1, const std::vector<BSPN>& absp, int iroot1, XP** ppxp)
{
    BSPSEL bsp1;
    bsp1.pa = &absp;
    bsp1.iroot = iroot1;
    bsp1.psoe = pso1;
    MarkBspBsp(pso0, pso1, absp, bsp1, ppxp);
}
void MarkSweptVerticesCheck(SOE* a, SOE* b, XP** ppxp) { MarkSweptVertices(a, b, ppxp); }
// orig: 0x001A9B28 (public entry point, also used by physcheck.cpp)
void MarkSphereBspContacts(SOE* a, SOE* b, const std::vector<BSPN>* pabsp, XP** ppxp) { MarkSphereBsp(a, b, pabsp, ppxp); }

// ---- Queries (orig module 0x14B1A0-0x14CDB0's line tests)

// orig: 0x0014C6A0 / 0x0014C6D8 (the pieces of the segment a-b inside the entry: by its tree, else (flag 49) its
// sphere; at most c into alsg)
int ClipSegmentEntry(const SOE* psoe, const glm::vec3& a, const glm::vec3& b, int c, LSG* alsg)
{
    return ClipSegmentSoe(psoe, a, b, c, alsg);
}

// orig 0x00118668 on the entry's tree with only its first inodeLimit nodes (the camera's hull: 0x14EA08 passes the
// entry's +0x74 nodes .. +0x74 + +0x70)
int ClipSegmentEntryLimit(const SOE* psoe, const glm::vec3& a, const glm::vec3& b, int c, LSG* alsg, int inodeLimit)
{
    if (!FHasBsp(psoe)) return 0;
    return ClipSegmentBsp(BSPREF(psoe->bsp, psoe->geomWorld), 0, inodeLimit, a, b, c, alsg);
}

// orig: 0x0014CC58 PsoHitTestLineObjects__FiP6VECTORT1iPP2SOP3LSG (the nearest of the entries the line a-b hits (grf
// 1: a hit at t 0 ignored); its segment into *plsg)
SOE* PsoeHitTestLineObjects(int grf, const glm::vec3& a, const glm::vec3& b, const std::vector<SOE*>& apsoe, LSG* plsg)
{
    SOE* psoBest = nullptr;
    float tBest = FLT_MAX;  // 0x2A4FF4
    LSG alsg[2];
    for (SOE* pso : apsoe)
    {
        const int c = ClipSegmentSoe(pso, a, b, 2, alsg);
        int j = 0;
        if (c > 0 && alsg[0].t0 == 0.0f) j = (grf & 1) ? 1 : 0;
        if (!(j < c)) continue;
        const float t = alsg[j].t0;
        if (!(t < tBest)) continue;
        tBest = t;
        psoBest = pso;
        if (plsg) *plsg = alsg[j];
    }
    return psoBest;
}

// orig: 0x0014C9F8 func_0014C9F8 (two sorted segment lists merged into their union (k 0) or intersection (k 1), at most
// cMax into alsgOut; the count)
int CMergeAlsg(int c1, const LSG* alsg1, int c2, const LSG* alsg2, int cMax, LSG* alsgOut, int k)
{
    std::vector<LSG> alsgT(std::max(cMax, 0) + 1);
    int i1 = 0, i2 = 0, fPrev = 0;
    const LSG* p0 = alsg1;
    const LSG* p1 = alsg2;
    const LSG* end1 = alsg1 + c1;
    const LSG* end2 = alsg2 + c2;
    size_t iout = 0;
    const int n = k == 1 ? 2 : 1;
    for (;;)
    {
        if (!(p0 < end1) && !(p1 < end2)) break;
        const float g1 = p0 < end1 ? (i1 ? p0->t1 : p0->t0) : FLT_MAX;  // 0x2A4FF0
        const float g2 = p1 < end2 ? (i2 ? p1->t1 : p1->t0) : FLT_MAX;
        int iOld;
        const LSG** ppl;
        if (g1 <= g2)
        {
            iOld = i1;
            i1 = 1 - iOld;
            ppl = &p0;
        }
        else
        {
            iOld = i2;
            i2 = 1 - iOld;
            ppl = &p1;
        }
        const int fIn = !(i1 + i2 < n) ? 1 : 0;
        const LSG* pl = *ppl;
        if (fIn != fPrev)
        {
            LSG& o = alsgT[iout];
            if (fIn)
            {
                o.t0 = pl->t0;
                o.pos0 = pl->pos0;
                o.plane0 = pl->plane0;
                o.psurf0 = pl->psurf0;
            }
            else
            {
                o.t1 = pl->t1;
                o.pos1 = pl->pos1;
                o.plane1 = pl->plane1;
                o.psurf1 = pl->psurf1;
                iout++;
                if (cMax < int(iout))
                {
                    iout--;
                    break;
                }
            }
            fPrev = fIn;
        }
        if (iOld == 1) *ppl = pl + 1;
    }
    for (size_t i = 0; i < iout; i++) alsgOut[i] = alsgT[i];
    return int(iout);
}

// 0x1A93C8 for the checks / other modules: the sphere's contacts with the entry (absp: pruned nodes of its tree, null:
// pruned here); up to csbiMax into the arrays (point, plane, kind, tag)
int CsbiSphereEntry(const glm::vec3& pos, const SOE* pso, const std::vector<BSPN>* pabsp, int csbiMax, float r,
                    std::vector<glm::vec3>* papos, std::vector<glm::vec4>* paplane, std::vector<int>* pakind,
                    std::vector<const void*>* papv, int inodeLimit)
{
    std::vector<SBI> asbi(std::max(1, csbiMax));
    const int c = SphereBspContacts(pos, pso, pabsp, csbiMax, asbi.data(), r, inodeLimit < 0 ? INODE_MAX : inodeLimit);
    for (int i = 0; i < c; i++)
    {
        papos->push_back(asbi[i].pos);
        paplane->push_back(asbi[i].plane);
        pakind->push_back(asbi[i].kind);
        papv->push_back(asbi[i].pv);
    }
    return c;
}
