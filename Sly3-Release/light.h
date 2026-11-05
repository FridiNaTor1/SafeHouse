#pragma once

// Lighting of lit shaders (kinds 1 / 7, and meshes with the lit flag): what the VU1 programs compute from the
// world's lights, done natively.
//  - The light bake (kind-0 RPLs, Glob_AddLightPackets 0x1681F8 + Glob_UploadMatrices 0x167FC0, VU1 program P_M
//    0x25E090, read back by the VIF1 interrupt 0x167F78 / 0x167E58 into the chains' colours): static groups
//    without baked table colours, lit by every active light (sphere-rejected point lights skipped).
//  - Forward lighting (P_G 522-636, the light context ALO+0x134 filled by 0x1A4478 with the strongest lights):
//    groups with the lit flag (mesh flag 1), lit by four lights.
// Both produce the shader's two colours: A (the shadow / ambient pass with CLUT 1) and B (the lit pass with
// CLUT 0, its alpha the amount). The math follows Sly2-Reference runtime/native_bake_math.cpp and
// native_forward_lighting.cpp, which were reconstructed from P_M / P_G; here in plain IEEE float.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct GLOB;

// A light's 3 quadwords at LIGHT+0x120 (Sly 2 class 4; prepared by the light's update from its params).
struct LIGHTREC
{
    uint32_t kind = 0;           // 0 directional, 1 point
    float angularOffset = 0;     // +4
    float angularScale = 0;      // +8
    float distanceOffset = 0;    // +0xC
    glm::vec4 rgba{0.0f};        // +0x10
    glm::vec3 dirOrPos{0.0f};    // +0x20
    float distanceScale = 0;     // +0x2C
};

// What the light context's selection (0x1A3F78) reads of a light besides its record: the strength at a distance
// d is gConst + gSlope x d (+0x110 / +0x114) from pos (+0x100; 0 for directional lights).
struct LIGHTSEL
{
    glm::vec3 pos{0.0f};
    float gConst = 0, gSlope = 0;
    int32_t fShadow = 1;    // +0x158: counts for the shadow direction (0x1A4478)
    float gShadowScale = 1.0f;  // Sly 3 (0x1C6418): +0x148 w: 0 no shadow, < 0 the strength, else x w / max(V, strength)
};

// The world's lights and the world shader colour (SW+0x3E28: rgb the shadow colour, w the ambient amount).
struct LIGHTSET
{
    std::vector<LIGHTREC> alight;   // the bake's lights (+0x160 == 0) in the order 0x15E9C8 finds them (at most 256)
    glm::vec4 worldShader{0.0f};
    // The light context's candidates (0x1A3D00: every slotted light with +0x15C == 0), in slot order; empty: the
    // selection falls back to the strongest of alight.
    std::vector<std::pair<LIGHTREC, LIGHTSEL>> actx;
    // 0x1A4AD8 (a render target's own lights, +0x104 / +0x108): actx is the context itself, its first four in
    // order (the rest ALIGHT_DEFAULT), no selection
    bool fFixed = false;
};

// The world's lights for the renderer. The world loader fills it as it loads / updates LIGHT objects (class 4):
// ResetWorldLights at world start, SetWorldLight for each light whenever its +0x120 record changes (fActive:
// the LO's bit 60 of +0x28 and +0x160 == 0, as Glob_UploadMatrices 0x167FC0 filters), SetWorldShader from
// SW+0x3E28. Until it does, the viewer falls back to a reference RAM (scene.cpp FLightsFromRam).
void ResetWorldLights();
void SetWorldLight(const void* plight, const LIGHTREC& rec, bool fActive);   // plight: the light's identity
// The same with the selection data: fBake (+0x160 == 0: the bake's), fContext (+0x15C == 0: a context candidate).
void SetWorldLight(const void* plight, const LIGHTREC& rec, const LIGHTSEL& sel, bool fBake, bool fContext);
void SetWorldShader(const glm::vec4& rgba);
bool FHaveWorldLights();
uint32_t WorldLightsVersion();      // changes whenever a light, the world shader or the set changes
const LIGHTSET& WorldLights();      // the active lights in the order they were first set

// P_M for one static group: colours of every vertex the light bake owns (VTXGLOB::fUnbaked).
// matWorld: the group's world matrix; posSphere/sRadius: its bounding sphere in world space.
// What a bake at this place depends on (the matrix, the lights that reach the sphere, the world shader colour): equal
// keys give equal colours.
uint64_t BakeKey(const glm::mat4& matWorld, const glm::vec3& posSphere, float sRadius, const LIGHTSET& lights);
void BakeGlobLighting(GLOB& glob, const glm::mat4& matWorld, const glm::vec3& posSphere, float sRadius,
                      const LIGHTSET& lights);

// P_G forward lighting for a lit-flag group with the four lights the light context would hold for it.
// pamatBone (a skinned group's bone matrices, the renderer's): the lights in world space and each vertex's normal
// skinned by its bones, as RenderGlob uploads the identity for skinned groups and P_G 808 skins the normals before
// lighting them (522); without, the lights in the group's space and the loaded normals.
#include <vector>
void ForwardGlobLighting(GLOB& glob, const glm::mat4& matWorld, const glm::vec3& posCenter, const LIGHTSET& lights,
                         const std::vector<glm::mat4>* pamatBone = nullptr);

// The four lights 0x1A4478 / 0x1A3F78 would choose at a point (strongest contribution first).
void SelectLights(const LIGHTSET& lights, const glm::vec3& pos, LIGHTREC alight[4]);

// The shadow part of 0x1A4478: from the chosen lights (those with +0x158) the shadow's direction and the
// 5 quadwords VU1 gets at 0xE for an object's cast shadow (mesh type 2 / the light context's +0x24..+0x2C):
// rows across / up (0x11E2D8 of the direction), the axis (direction x length), the centre (w 1), then the
// length ratio and whether the camera is inside the shadow's cone (0x1A4258).
struct SHADOWCTX
{
    bool fValid = false;
    glm::vec3 across{1, 0, 0}, up{0, 1, 0}, axis{0, 0, 1}, center{0.0f};
    float rLength = 0;          // dist / length
    bool fCameraInside = false;
};
// pos: the light context's position; sRadius: its shadow radius (pl +0x1C, the SO's +0x13C; 0: none); sExtra:
// pl +0x20; posCamera / sCameraNear: CM +0x90 / +0x48.
SHADOWCTX ShadowOfLightContext(const LIGHTSET& lights, const glm::vec3& pos, float sRadius, float sExtra,
                               const glm::vec3& posCamera, float sCameraNear);
