#pragma once

// BSP trees of planes over a GEOM's surfaces (orig module 0x118668-0x1195E0): point searches, segment clipping,
// the tree's pruning by a sphere, planes of faces.
//
// Native representation: a tree is an array of BSPN (geom.h: a surface index and two child indices); the planes
// are the surfaces of a GEOM (the entry's world geometry). The original's two sentinel planes and nodes:
//   plane 0x2BBAF0 (0, 0, 0, 1e8): behind everything; the plane of the loaded leaves (isurf -1) and of 0x2BBB20;
//   plane 0x2BBAD0 (0, 0, 0, -1e8): in front of everything, the plane of node 0x2BBB10;
//   node 0x2BBB10 (IBSP_EMPTY): a leaf outside; node 0x2BBB20 (IBSP_SOLID): a leaf inside.

#include "geom.h"

#include <glm/glm.hpp>

#include <vector>

constexpr int IBSP_NONE = -1;
constexpr int IBSP_EMPTY = -2;  // orig node 0x2BBB10
constexpr int IBSP_SOLID = -3;  // orig node 0x2BBB20 (also the empty pruned list, BSP_EMPTY)
constexpr int ISURF_BEHIND = -1;  // plane 0x2BBAF0
constexpr int ISURF_FRONT = -2;   // plane 0x2BBAD0

// A tree: its nodes and the geometry whose surfaces are the node planes.
struct BSPREF
{
    const BSPN* anode = nullptr;
    int cnode = 0;
    const GEOM* pgeom = nullptr;
    BSPREF() = default;
    BSPREF(const BSP& bsp, const GEOM& geom) : anode(bsp.abspn.data()), cnode(int(bsp.abspn.size())), pgeom(&geom) {}
    BSPREF(const std::vector<BSPN>& a, const GEOM& geom) : anode(a.data()), cnode(int(a.size())), pgeom(&geom) {}
    const BSPN& Node(int i) const;
    glm::vec4 Plane(int i) const;            // the node's plane (normal, d)
    const SURF* Psurf(int i) const;          // the node's surface (null: a sentinel plane)
    int Child(int i, bool fRight, int inodeMax) const;  // children at or past inodeMax count as none
};

// The signed distance of pos from the plane (normal . pos - d).
inline float PlaneDist(const glm::vec4& plane, const glm::vec3& pos)
{
    const float x = plane.x * pos.x, y = plane.y * pos.y, z = plane.z * pos.z;
    return ((x + y) + z) - plane.w;
}
inline glm::vec4 PlaneOfSurf(const SURF& s) { return glm::vec4(s.normal, s.gDot); }

// A piece of a segment behind a tree's plane (the original's LSG, 0x60 bytes).
struct LSG
{
    glm::vec3 pos0{0.0f}, pos1{0.0f};    // +0x00, +0x10: the ends
    glm::vec4 plane0{0.0f}, plane1{0.0f};  // +0x20, +0x30: the planes the ends are on
    float t0 = 0.0f, t1 = 0.0f;          // +0x40, +0x44: their parameters on the segment
    int fSphere = 0;                     // +0x48: made by the sphere clip (0x14C748)
    const SURF* psurf0 = nullptr;        // +0x4C: the surface at end 0 (null: the segment's own end, not embedded)
    const SURF* psurf1 = nullptr;        // +0x50
    const EDGE* pedge = nullptr;         // +0x54: the edge clipped (set by the callers)
    int iend = 0;                        // +0x58: which end of the edge is end 0 (CategorizeAlsg swaps)
    LSG* plsgNext = nullptr;             // +0x5C
};

int IbspFindPoint(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos);     // 0x118FB0
int IbspFindPointEps(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos);  // 0x119090
int ClipSegmentBsp(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos0, const glm::vec3& pos1, int crecMax,
                   LSG* alsg);                                                            // 0x118668
// 0x118A88: the tree classified against the sphere (pos, gEps): 0 behind (inside), 1 in front (outside), 2 a pruned
// tree appended to *pabspn (at most *pcnodeLeft more nodes): its root is the last node.
int ClipBspPlane(const BSPREF& ref, int inode, int inodeMax, const glm::vec3& pos, float gEps, int* pcnodeLeft,
                 std::vector<BSPN>* pabspn);
glm::vec4 PlaneFromPoints(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);  // 0x119308
glm::vec4 PlaneFromNormalPos(const glm::vec3& normal, const glm::vec3& pos);           // 0x119390
