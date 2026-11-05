// BSP trees of planes (orig module 0x118668-0x1195E0; bsp.h). Ported from the reference's m_118668.cpp,
// m_118668_p3.cpp and round4_high.cpp (0x119090), checked against the original's code.

#include "bsp.h"

#include <cmath>
#include <cstdio>

namespace
{
const BSPN s_bspnEmpty{ISURF_FRONT, IBSP_NONE, IBSP_NONE};   // orig 0x2BBB10
const BSPN s_bspnSolid{ISURF_BEHIND, IBSP_NONE, IBSP_NONE};  // orig 0x2BBB20
constexpr float EPS = 1e-4f;  // 0x38D1B717
}

const BSPN& BSPREF::Node(int i) const
{
    if (i == IBSP_EMPTY) return s_bspnEmpty;
    if (i == IBSP_SOLID || i < 0 || i >= cnode) return s_bspnSolid;
    return anode[i];
}

glm::vec4 BSPREF::Plane(int i) const
{
    const int isurf = Node(i).isurf;
    if (isurf >= 0 && pgeom && isurf < int(pgeom->asurf.size())) return PlaneOfSurf(pgeom->asurf[isurf]);
    if (isurf == ISURF_FRONT) return glm::vec4(0.0f, 0.0f, 0.0f, -1e8f);
    return glm::vec4(0.0f, 0.0f, 0.0f, 1e8f);
}

const SURF* BSPREF::Psurf(int i) const
{
    const int isurf = Node(i).isurf;
    if (isurf >= 0 && pgeom && isurf < int(pgeom->asurf.size())) return &pgeom->asurf[isurf];
    return nullptr;
}

int BSPREF::Child(int i, bool fRight, int inodeMax) const
{
    if (i < 0) return IBSP_NONE;  // (the sentinel nodes have no children)
    const BSPN& n = Node(i);
    const int c = fRight ? n.ibspPos : n.ibspNeg;
    if (c >= 0 && c >= inodeMax) return IBSP_NONE;
    return c;
}

// orig: 0x00118FB0 func_00118FB0 (the leaf-side search of a point: left while behind (d < 1e-4), else right;
// behind with no left child: that node; in front with no right child: none)
int IbspFindPoint(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos)
{
    if (inode == IBSP_NONE) return IBSP_NONE;
    for (;;)
    {
        const float d = PlaneDist(ref.Plane(inode), pos);
        if (d < EPS)
        {
            const int p = ref.Child(inode, false, inodeMax);
            if (p == IBSP_NONE) return inode;
            inode = p;
        }
        else
        {
            inode = ref.Child(inode, true, inodeMax);
            if (inode == IBSP_NONE) return IBSP_NONE;
        }
    }
}

// orig: 0x00119090 func_00119090 (a containing node with a tolerance: outside the band (-1e-4, 1e-4) one child;
// inside it the right subtree first, then the left; a missing left child returns the node)
int IbspFindPointEps(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos)
{
    while (inode != IBSP_NONE)
    {
        const float d = PlaneDist(ref.Plane(inode), pos);
        if (d < -EPS)
        {
            const int left = ref.Child(inode, false, inodeMax);
            if (left == IBSP_NONE) return inode;
            inode = left;
        }
        else if (EPS < d)
            inode = ref.Child(inode, true, inodeMax);
        else
        {
            const int right = ref.Child(inode, true, inodeMax);
            if (right != IBSP_NONE)
            {
                const int found = IbspFindPointEps(ref, right, inodeMax, pos);
                if (found != IBSP_NONE) return found;
            }
            const int left = ref.Child(inode, false, inodeMax);
            if (left == IBSP_NONE) return inode;
            inode = left;
        }
    }
    return IBSP_NONE;
}

namespace
{
// The point at t on the segment a..b (vmulax / vmaddx: a (1 - t) + b t)
inline glm::vec3 Lerp(const glm::vec3& a, const glm::vec3& b, float u, float t) { return a * u + b * t; }

// An entry of the segment walk: the node to visit, the node whose back the piece is on (none: in front), the
// piece's start parameter, its plane.
struct SEGENT
{
    int inode = IBSP_NONE;
    int inodeBack = IBSP_NONE;
    float t = 0.0f;
    const SURF* psurf = nullptr;
    glm::vec4 plane{0.0f};
};
}

// orig: 0x00118668 func_00118668 (clips the segment pos0..pos1 against the tree: an explicit stack of the pieces
// still to visit, split at the planes they cross; each piece ending behind a plane makes an LSG (a piece that
// continues the last one extends it); crecMax when they ran out)
int ClipSegmentBsp(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos0, const glm::vec3& pos1, int crecMax,
                   LSG* arec)
{
    SEGENT stk[33];
    int top = 0;
    SEGENT cur;
    cur.inode = (inode >= 0 && inode >= inodeMax) ? IBSP_NONE : inode;
    stk[0].t = 1.0f;
    LSG* precLast = nullptr;
    int crec = 0;
    for (;;)
    {
        while (cur.inode != IBSP_NONE)
        {
            const int pn = cur.inode;
            const glm::vec4 plane = ref.Plane(pn);
            const float d0 = PlaneDist(plane, pos0);
            const float d1 = PlaneDist(plane, pos1);
            bool left;
            if (d0 <= 0.0f && d1 <= 0.0f)
                left = true;
            else if (0.0f < d0 && 0.0f < d1)
                left = false;
            else
            {
                const float dd = d0 - d1;
                const float t0 = cur.t, t1 = stk[top].t;
                const float tSplit = d0 / dd;
                const float u0 = (1.0f - t0) * d0;
                const float g1 = d1 * t1 + (1.0f - t1) * d0;
                const float g0 = d1 * t0 + u0;
                if (t0 < tSplit && tSplit < t1)
                {
                    if (top >= 32)
                    {
                        std::fprintf(stderr, "[bsp] ClipSegmentBsp: segment stack overflow\n");
                        return crec;
                    }
                    top++;
                    SEGENT& e = stk[top];
                    e.t = tSplit;
                    e.plane = plane;
                    e.psurf = ref.Psurf(pn);
                    if (g0 <= 0.0f)
                    {
                        e.inodeBack = IBSP_NONE;
                        e.inode = ref.Child(pn, true, inodeMax);
                    }
                    else
                    {
                        e.inodeBack = pn;
                        e.inode = ref.Child(pn, false, inodeMax);
                    }
                }
                if (g0 < 0.0f) left = true;
                else if (g0 == 0.0f) left = g1 <= 0.0f;
                else left = false;
            }
            if (left)
            {
                cur.inodeBack = pn;
                cur.inode = ref.Child(pn, false, inodeMax);
            }
            else
            {
                cur.inodeBack = IBSP_NONE;
                cur.inode = ref.Child(pn, true, inodeMax);
            }
        }
        for (;;)
        {
            if (cur.inodeBack != IBSP_NONE)
            {
                const SEGENT& e = stk[top];
                if (precLast && cur.t == precLast->t1)
                {
                    precLast->pos1 = Lerp(pos0, pos1, 1.0f - e.t, e.t);
                    precLast->plane1 = e.plane;
                    precLast->t1 = e.t;
                    precLast->psurf1 = e.psurf;
                }
                else
                {
                    if (!(crec < crecMax)) return crecMax;
                    precLast = &arec[crec++];
                    *precLast = LSG{};
                    const float t0 = cur.t, t1 = e.t;
                    precLast->pos0 = Lerp(pos0, pos1, 1.0f - t0, t0);
                    precLast->pos1 = Lerp(pos0, pos1, 1.0f - t1, t1);
                    precLast->plane0 = cur.plane;
                    precLast->plane1 = e.plane;
                    precLast->t0 = t0;
                    precLast->t1 = t1;
                    precLast->psurf0 = cur.psurf;
                    precLast->psurf1 = e.psurf;
                }
            }
            if (!(top > 0)) return crec;
            cur = stk[top];
            top--;
            if (cur.inode != IBSP_NONE) break;
        }
    }
}

// orig: 0x00118A88 func_00118A88 (the tree against the sphere (pos, gEps): the side the point is on; where it lies
// on a node's plane both children are classified and, when they differ (or are both 2), a new node with the
// node's plane is appended: 0 behind, 1 in front, 2 the new node (the last one appended))
int ClipBspPlane(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos, float gEps, int* pcnodeLeft,
                 std::vector<BSPN>* pabspn)
{
    float d = 0.0f;  // (the original's $f1 when no node is visited: callers always pass a node)
    while (inode != IBSP_NONE)
    {
        d = PlaneDist(ref.Plane(inode), pos);
        if (gEps < d)
            inode = ref.Child(inode, true, inodeMax);
        else if (d < -gEps)
            inode = ref.Child(inode, false, inodeMax);
        else
        {
            int iL = ref.Child(inode, false, inodeMax);
            int kL;
            if (iL != IBSP_NONE)
            {
                kL = ClipBspPlane(ref, iL, inodeMax, pos, gEps, pcnodeLeft, pabspn);
                iL = int(pabspn->size()) - 1;
            }
            else
                kL = 0;
            int iR = ref.Child(inode, true, inodeMax);
            int kR;
            if (iR != IBSP_NONE)
            {
                kR = ClipBspPlane(ref, iR, inodeMax, pos, gEps, pcnodeLeft, pabspn);
                iR = int(pabspn->size()) - 1;
            }
            else
                kR = 1;
            if (kL == kR && kR != 2) return kR;
            if (!(*pcnodeLeft > 0)) return 1;
            --*pcnodeLeft;
            BSPN n;
            n.isurf = ref.Node(inode).isurf;
            n.ibspNeg = kL == 1 ? IBSP_EMPTY : (kL == 0 ? IBSP_NONE : iL);
            n.ibspPos = kR == 1 ? IBSP_NONE : (kR == 0 ? IBSP_SOLID : iR);
            pabspn->push_back(n);
            return 2;
        }
    }
    return (0.0f < d) ? 1 : 0;
}

// orig: 0x00119308 func_00119308 (the plane of the triangle: (b - a) x (c - a) normalized, d = normal . a)
glm::vec4 PlaneFromPoints(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 x = glm::cross(b - a, c - a);
    const float len = std::sqrt(std::fabs(glm::dot(x, x)));
    const glm::vec3 n = len != 0.0f ? x * (1.0f / len) : glm::vec3(3.4028235e38f);
    const float d = (n.x * a.x + n.y * a.y) + n.z * a.z;
    return glm::vec4(n, d);
}

// orig: 0x00119390 func_00119390 (the plane through pos with the normal)
glm::vec4 PlaneFromNormalPos(const glm::vec3& n, const glm::vec3& p)
{
    return glm::vec4(n, (n.x * p.x + n.y * p.y) + n.z * p.z);
}
