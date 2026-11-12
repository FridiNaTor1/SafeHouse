#pragma once

// The AI's walkable areas (module 0x1BC050..0x1C2920): the world's BSP of convex regions (cid 0x11 MESHZONE objects:
// vertices with their neighbours, triangles, a 2D BSP whose leaves are the triangles), the blocking polygons (closed
// path objects, cid 0x0D / 0x0E: their world points, bounding box and edge planes, kept by the region under them,
// overlapping ones merged), the queries the jobs make: the region of a point, a point kept on the walkable area,
// a segment through a region's BSP, a path over the region's triangle graph (A*, smoothed), the way around a
// polygon on a patrol path.
//
// Native: built from the loaded LOXMESH (lo.h) and the world's CMESH (sw.h) on first use per world; the regions'
// polygon lists natively are plain lists in slot order (the original's sweep-and-prune gives the same sets).
//
// Part of the animation / AI code.

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct CRV;
struct REGENT;
struct LO;
struct LOXMESH;
struct LOXPATH;
struct NAVREGION;
struct SW;

// A blocking polygon (orig: the 0x1BC050 objects' +0x190..+0x1EC)
struct NAVPOLY
{
    LO* plo = nullptr;                 // the path object (cid 0x0D family)
    const CRV* pcrv = nullptr;         // its curve (the way around follows it)
    std::vector<glm::vec4> aposWorld;  // the world points (closed: the last repeats the first)
    glm::vec4 posMin{0.0f}, posMax{0.0f};  // +0x1A0 / +0x1B0 (z: the fixed range 0x2A76D0)
    std::vector<glm::vec4> aplane;     // +0x1EC the edge planes {normal, distance}
    NAVREGION* pregion = nullptr;      // +0x1DC
    int kind = 0;                      // +0x190 (0x1BC2F8)
    std::shared_ptr<CRV> pcrvOwn;      // (an imported polygon's curve)
};

struct NAVREGION
{
    LOXMESH* pmesh = nullptr;
    struct VTX
    {
        glm::vec4 pos{0.0f};
        struct EDGE { int ivtx; float s; };
        std::vector<EDGE> aedge;       // the neighbours and the xy lengths (0x1C0880)
    };
    std::vector<VTX> avtx;             // +0xE0 / +0xE4
    std::vector<std::array<int, 3>> atri;  // +0xE8 / +0xEC
    struct NODE { glm::vec3 plane; uint16_t child[2]; };
    std::vector<NODE> anode;           // +0xF0 / +0xF4 the 2D BSP (bit 15 a leaf: the triangle in bits 0..13, bit 14
                                       // its side 0, bit 15 side 1)
    std::vector<NAVPOLY*> apoly;       // +0xF8 / +0x10C the polygons in their slots (null: a free one)
    // +0x118: the actors standing on it (their area records: ALO +0xE8 -> +0xDC {owner, dz, radius, mass, region})
    // (live entries carry their REGENT (so.h): the values are read from it; the harnesses' imports set them here)
    struct MEMBER { LO* plo = nullptr; float dz = 0, r = 0, m = 0; const REGENT* pre = nullptr; };
    std::vector<MEMBER> amember;
    int nStamp = 0;                    // +0x114
    int ileaf = -1;                    // +0x124 its leaf in the world's BSP
};

// The regions of the current world (built from the loaded meshes / BSP once)
void ResetNav();
void InitSwNav(SW* psw);   // orig: 0x001C10C8 (including the default mesh 0x001C0648)
NAVREGION* PregionOfMesh(LOXMESH* pmesh);
int NGenNav();  // the regions' generation (a new world's regions: a new one)
int NNavLinkSerial();
void BumpNavLinkSerial();  // orig: 0x1C2078 / 0x1C20B0 (a link enters or leaves the world)
void SetNavLinkSerial(int n);  // for the differential checks (aiimport.cpp)
// orig: 0x001C0D70: the region under a point (none: the default region when fDefault or no flagged tree, else null)
NAVREGION* PregionAt(const glm::vec3& pos, bool fDefault);
// orig: 0x001BF110: the triangle of the region's 2D BSP under pos (side fl & 1), -1 none
int ItriNav(const NAVREGION* preg, const glm::vec3& pos, int fl);
// orig: 0x001BF1F8: the same from a node, points on a plane trying both sides
int ItriNavFrom(const NAVREGION* preg, int inode, const glm::vec3& pos, int fl);
// orig: 0x001BFDB0: the point of the triangle nearest pos in xy (on its plane inside)
glm::vec3 PosNearestTri(const NAVREGION* preg, int itri, const glm::vec3& pos);
// orig: 0x001C0B50: pos kept on the walkable area (out of a blocking polygon by its edges); false off the BSP
bool FKeepOnNav(NAVREGION* preg, const glm::vec3& pos, glm::vec3* pposOut);
// orig: 0x001C0320 / 0x001C0940: the nearest walkable point (false off the BSP)
bool FNavPoint(const NAVREGION* preg, const glm::vec3& pos, glm::vec3* pposOut);
bool FNavPoint(const NAVREGION* preg, const glm::vec3& pos, glm::vec3* pposOut, int fl);

// A piece of a segment in a triangle (orig 0x60 bytes: start, end, the planes at its ends, t0, t1, 3, the triangle)
struct NAVSEG
{
    glm::vec4 posStart{0.0f}, posEnd{0.0f};
    glm::vec4 planeStart{0.0f}, planeEnd{0.0f};
    float t0 = 0, t1 = 0;
    int k = 3;
    int itri = -1;
};
// orig: 0x001BF3D8: the segment's pieces in the triangles of the side (fl & 1); fl & 2: merged across triangles
int CsegNav(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, int cMax, NAVSEG* aseg, int fl);
// orig: 0x001BF900: the segment leaves the walkable area
bool FNavBlocked(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, int fl);
// orig: 0x001BFF28: a path from a to b over the region (up to cMax points), 0 off it
int CposNavPath(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, int cMax, glm::vec4* apos);
// orig: 0x001C02D0: that path into the curve (a linear CRV of its points); false without one
bool FNavPathCrv(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, CRV* pcrv);

// The polygons
// orig: 0x001C1700: the region's polygons whose boxes overlap the box of a and b (in slot order, up to cMax + 1)
int CpolyNear(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b, int cMax, NAVPOLY** apoly);
// orig: 0x001BC9F0: pos inside the polygon (its edge planes)
bool FPolyContains(const NAVPOLY* ppoly, const glm::vec3& pos);
int NNavPathLeavesPoly(NAVPOLY* pp, const CRV* pcrv, bool fForward, float* ps, float s);  // 0x1BD560 (3: never leaves)
NAVPOLY* PpolyAtPoint(const glm::vec3& pos);  // 0x1E9F50
int NNextNavStamp();                          // ++GAME +0x3D70
// A segment's crossing of a polygon (orig 0x1BC688's records: the entry / exit points, their planes, t)
struct POLYHIT
{
    glm::vec4 posA{0.0f}, posB{0.0f}, planeA{0.0f}, planeB{0.0f};
    float t0 = 0, t1 = 0;
    NAVPOLY* ppolyExit = nullptr, * ppolyEnter = nullptr;
};
// orig: 0x001BC688
int ChitPoly(NAVPOLY* ppoly, const glm::vec3& a, const glm::vec3& b, int cMax, POLYHIT* ahit);
// orig: 0x001BD310: of the polygons near the segment, the first it doesn't cross cleanly, else the nearest hit
NAVPOLY* PpolyOnSegment(const NAVREGION* preg, const glm::vec3& a, const glm::vec3& b);
// orig: 0x001BD718: the way around a polygon in front on the path (the path point a step ahead / behind s):
// 0 none, else the way's answer (1 / 2 / 3 / 4); *pppoly the polygon, *ps the distance on it
int NNavWay(const glm::vec3& pos, const NAVREGION* preg, const CRV* pcrv, int fForward, NAVPOLY** pppoly, float* ps,
            float s);

// The polygons' upkeep: the world pass 0x1BE1E8 (the moved / new polygons rebuilt and given to their regions)
void UpdateNavPolys();
void MarkNavPolyDirty(LO* plo);   // orig 0x1BC160 (AddLo) / a move
void RemoveNavPoly(LO* plo);      // orig 0x1BC198
NAVPOLY* PpolyOfLo(LO* plo);
bool FNavPolyMerged(const NAVPOLY* ppoly);  // original polygon +0x1D8 != 0
// For the differential checks (aiimport.cpp): a region's polygon slots replaced (null: a free slot)
void SetRegionPolys(NAVREGION* preg, std::vector<std::shared_ptr<NAVPOLY>> apoly);
