#pragma once

// Run-time render targets: the two 128 x 128 targets of the world (SW+0x45E8 / +0x4610 = 0x3E1A78 / 0x3E1AA0,
// set up by 0x1BE398 from 0x20E180: TEX0 32-bit TBW 2, a clear packet (colour 0, alpha 0, Z 0) and a texture packet
// (TEX1 0x60 bilinear, CLAMP 5)). An object takes one (0x1BEB28): the objects of its list (+0xE4) are drawn into
// it instead of the frame, with the group kinds remapped (3 / 4 -> 15 | 18, 5 / 6 -> 16 | 19, 9 -> 17 | 20; others
// not drawn), seen from the owner's camera (0x1BE6F0). A screen object's kind-6 quad (0x215730) shows the target
// of the object its id names (0x1BEED8: 0x2AE the player's gadget's, 0x2AF the player's, else by id): the
// binocucom view. Natively each target is an FBO (colour texture + depth / stencil).
//
// The game drives it: IrtgtAcquire / ReleaseRtgt for the owner, SetRtgtCamera every frame, SCENEGLOBSET::irtgt
// on the globsets of the owner's list (-1 again on release), g_pfnRtgtOwnerOfId for the screen quads.

#include <glm/glm.hpp>

constexpr int CRTGT = 2;

struct RTGT
{
    int dx = 128, dy = 128;                 // +0 / +4
    const void* powner = nullptr;           // +0x20 the object holding it
    // The owner's camera (0x1BE6F0): position, view axes (from the object's basis b: forward b[1], left b[2],
    // up b[0]; the VU view rows are -b[2], -b[0], b[1]), vertical field of view (+0x11C, default 1.0) and near
    // clip (+0x120, default 4; the far clip +0x124, default 1000, is not needed: reversed-Z infinite far).
    glm::vec3 pos{0.0f};
    glm::vec3 dirForward{1, 0, 0}, dirUp{0, 0, 1};
    float radFovY = 1.0f;
    float sNear = 4.0f;
    unsigned fbo = 0, tex = 0, rboDepth = 0;
};

RTGT* PrtgtGet(int irtgt);
// orig: 0x001BEB28 (the target side: the owner's target, else the first free one; -1 when both are taken)
int IrtgtAcquire(const void* powner);
// orig: 0x001BED18 (the target side: given back)
void ReleaseRtgt(const void* powner);
int IrtgtOfOwner(const void* powner);
// The owner's camera this frame: its position and the object's MATRIX3 rows b0, b1, b2 (as 0x1A6318 returns them).
void SetRtgtCamera(int irtgt, const glm::vec3& pos, const glm::vec3& b0, const glm::vec3& b1, const glm::vec3& b2,
                   float radFovY = 1.0f, float sNear = 4.0f);
// The target's colour texture (GL name; created on first use).
unsigned GlTextureOfRtgt(int irtgt);
// orig: 0x001BEED8 (a kind-6 quad's id -> the object whose target it shows); set by the game, null: no object.
extern const void* (*g_pfnRtgtOwnerOfId)(int id);
// The owner's side of 0x1BEB28 / 0x1BED18 (its list's objects added / hidden, then restored): set by the game,
// called when a target is newly taken / given back.
extern void (*g_pfnRtgtTaken)(const void* powner, int irtgt);
extern void (*g_pfnRtgtGiven)(const void* powner);
