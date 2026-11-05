#pragma once

// Collision geometry as read from world files (CBinaryInputStream::ReadGeom 0x117FD0, ReadBsp 0x118220).
// Indices instead of the original's pointers, so geometry can be copied (the world copy of a local GEOM).

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

struct EDGE
{
    uint16_t ipos0 = 0;    // orig +0
    uint16_t ipos1 = 0;    // orig +2
    uint16_t isurf = 0;    // orig +4: the surface the edge was read with
    uint16_t isurfOther = 0;  // orig +6: the neighbouring surface
    int iedgeNextInSurf = -1;  // orig -8 link of the next edge of the surface
    int iedgeNextOfOther = -1;   // orig +0xC: next edge in asurf[isurfOther].iedgeOfOthers
};

struct SURF
{
    glm::vec3 normal{0.0f};  // the plane of the first three points (orig 0x119308)
    float gDot = 0.0f;
    int ipos0 = 0;           // orig +0x1C
    int iedgeFirst = -1;     // orig +0x10
    int iedgeOfOthers = -1;     // orig +0x14: edges of other surfaces that border this one
    float ag[4]{};           // the four floats after the indices (the original reads and drops them)
    mutable uint32_t grfMark = 0;  // orig +0x18: scratch marks of the contact code (the physics port, mark.cpp)
};

struct GEOM
{
    std::vector<glm::vec3> apos;
    std::vector<SURF> asurf;
    std::vector<EDGE> aedge;
};

// A BSP node: the surface (isurf < 0: a leaf, the original's sentinel 0x2BBAF0) and the two subtrees (-1: none).
struct BSPN
{
    int isurf = -1;
    int ibspNeg = -1;  // orig +0
    int ibspPos = -1;  // orig +4
};

struct BSP
{
    uint32_t c = 0, x4 = 0, x8 = 0;  // orig +0, +4, +8 (count and two words)
    std::vector<BSPN> abspn;
};
