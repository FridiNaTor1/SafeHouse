#pragma once

// The climbing surfaces (cid 0x09: a PNTS whose points are the vertices of a triangle mesh, its first index triples
// the triangles and its second ones each triangle's neighbours across its edges, -1 open; list 0x3E142C: the Ep7 ice
// walls) and Sly's state 21 on one (module 0x185110..0x1865F8, matched in the decomp).
//
// The surface's own fields: +0x100 the list's link, +0x108 how far above it the player may be (0x185BF8: FLT_MAX;
// a field option), +0x110 its camera zone (an object option: the player's zone while on it, 0x18C250).
// Sly's: +0x1F18 the surface, +0x1F1C its triangle and barycentric place, +0x1F30..+0x1F50 three points in his frame
// (method 771, 0x185E30: the script's (53, -37, 95), (53, 37, 95), (53, 0, -40); +0x1F30's x the hold distance).

#include <glm/glm.hpp>

struct LO;
struct SLY;

struct SURFLOC  // orig 12 bytes {index, u, v}: the triangle and the weights of its first two corners
{
    int itri = 0;
    float u = 0.0f, v = 0.0f;
};

// orig: 0x00185310 (the mesh's point nearest pos in the world: the triangle, the point, the face's unit normal)
void FindClimbSurfClosestPoint(LO* plo, const glm::vec3& pos, SURFLOC* psl, glm::vec3* ppos, glm::vec3* pnormal);
// orig: 0x00185D18 (a jump's landing on the surface t seconds on: the point nearest the ballistic position, out by
// sOffset along the normal; the velocity then, less 300 into the surface)
void ClimbSurfLanding(LO* plo, const glm::vec3& pos, const glm::vec3& vel, const glm::vec3& acc, float sOffset, float t,
                      SURFLOC* psl, glm::vec3* ppos, glm::vec3* pvel);
// +0x108 (0x19B110's reach test)
float SHeightClimbSurf(LO* plo);
// orig: 0x00185E40 (Sly's state 21 each frame, from his update 0x18BCE0)
void UpdateSlyClimbSurf(SLY* p);
// The state 21's entry / exit parts of 0x18C250 (the surface +0x1F18 set by the jump's arrival 0x19C688)
void EnterSlyClimbSurf(SLY* p);
void LeaveSlyClimbSurf(SLY* p);
// State 22 (the hang-follow on a kind-3 hook, +0x18C4): 0x0018C250's entry / exit, its frame 0x18DF60, its cross jump
// (0x18A8C8), the blend (0x188608)
void BindSlyHangBlend(SLY* p);  // (sly.cpp)
void EnterSlyHangFollow(SLY* p);
void LeaveSlyHangFollow(SLY* p);
void UpdateSlyHangFollow(SLY* p);
bool FSlyHangFollowJump(SLY* p);
// State 3 (the swing on a hook of kind 0 / 1 / 2, +0x18C4): slyhang.cpp
void BindSlySwingBlend(SLY* p);      // (sly.cpp: 0x18F908 and the SM +0x1950)
void CalculateSlyHangAccel(SLY* p);  // 0x18FC60
void EnterSlyHang(SLY* p);           // 0x18C250 (entering 3)
void LeaveSlyHang(SLY* p);           // 0x18C250 (leaving 3)
void UpdateSlyHangInput(SLY* p);     // 0x190520
void UpdateSlyHangMotion(SLY* p);    // 0x1903B8
bool FSlyHangRelease(SLY* p);        // 0x18A8C8 (state 3)
glm::vec3 PosSlyHangPoint(SLY* p);   // 0x190C58 (the point the cane holds: past the hook, away from the arm)
void UpdateSlyHangPlace(SLY* p);     // 0x190E18 (state 3's update: the hand's actuator turned to the hook)
void DrawSlyHangPlace(SLY* p);       // 0x190D10 (state 3's draw, 0x18A238: the cane put on the hook, the arm solved)
struct SW;
void DrawSlys(SW* psw);              // 0x18A238 (Sly's draw) for each drawn Sly: its state 3 part, before the scene
