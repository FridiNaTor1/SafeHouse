#pragma once

// The world's collision for the native Sly 3 player and camera (the Sly 3 player): the SO / ALO collision entries the
// scene places (scene3.h g_acoll3: the cmesh GEOM in world space and its BSP, as the original's entries after
// 0x166270), with a grid of their bounds for the queries. The queries are the copied physics' segment clip
// (mark.cpp ClipSegmentEntry = orig 0x118668 on the entry's BSP, as Sly 2's PsoHitTestLineObjects 0x14CC58 uses it).

#include <glm/glm.hpp>

struct SOE;

namespace sly3
{
// The grid over g_acoll3 (after FBuildScene3); oidSkip: objects left out (the player's own entries).
void BuildColl3(int oidSkip0, int oidSkip1);
int CsoeColl3();

struct HIT3
{
    const SOE* psoe = nullptr;
    int oid = -1;
    float t = 1.0f;               // along the segment
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};  // the surface's plane at the entry point
};
// The first entry into the collision along a -> b (the segment's start inside an entry is not a hit).
bool FHitSegment3(const glm::vec3& a, const glm::vec3& b, HIT3* phit);
// The floor under (x, y): the first surface down from zTop to zBottom whose normal z is at least nzMin.
bool FFloor3(float x, float y, float zTop, float zBottom, float nzMin, HIT3* phit);
}  // namespace sly3
