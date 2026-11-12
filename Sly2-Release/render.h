#pragma once

// Rendering (adapted from Sly1-Release render.cpp's pass structure): draw records from the scene's globs, sorted by
// the Sly 2 RPL kind (DrawRpl 0x157FA0: counting sort by the 26 kinds, far-to-near for 1, 2, 5, 6, 10, 25), each
// shader drawn pass by pass with the GS state its register lists ask for (shd.cpp SHDPASS -> GL state).

#include "fastpaths.h"
#include "scene.h"

#include <glm/glm.hpp>

struct CAMERA
{
    glm::vec3 pos{0.0f};
    float yaw = 0.0f;       // degrees, around +Z (0: looking along +X)
    float pitch = 0.0f;     // degrees, up positive
    float fovY = 50.0f;     // degrees
    float sNear = 10.0f;
    float sFar = 1e6f;
    float rMRDAdjust = 1.0f;  // CM +0x1C, before the PC draw-distance multiplier
    uint32_t grfChannels = 0xFF;  // CM +0x14: the camera volume's channels (the volume tree's record masks, 0x1D5808)
    // The game's camera (cm.h): its rows (x view, y left, z up; roll included) instead of yaw / pitch.
    bool fMat = false;
    glm::mat3 mat{1.0f};

    glm::vec3 Dir() const;
    glm::mat4 MatWorldToClip(float aspect) const;
};

struct RENDEROPTS
{
    bool fCelBorders = true;
    bool fMeshes = true;
    bool fEffects = false;          // mesh type 2 (extruded shadows?): not drawn yet
    bool fShadows = true;           // mesh type 3 shadow hulls (rp 7 / 8)
    bool fCull = false;
    bool fFog = false;
    uint32_t rgbaCel = 0x80101010;  // 0x2DEB50 (GS units; its only value, the static initializer 0x16EFE0)
    uint32_t rgbaFog = 0;           // 0x2DE18C FOGCOL
    uint32_t rgbaClear = 0;
    float sFogNear = 3000, sFogFar = 30000, uFogMax = 0.3f;   // CM +0x240 / +0x244 / +0x248
};

void BuildSceneGpu(SCENE& scene);
// A world's load (scope: its record index): the backend makes the pipelines it saw used there before ahead (Vulkan's
// prewarm; nothing on OpenGL), waiting a short while for them.
void PrewarmRenderer(int scope);
// A glob's vertex colours changed (run-time lighting): its colours again into its arena (the colour buffer only).
void UpdateGlobColorsGpu(const GLOB& glob);
// A glob's vertices moved (trails, ropes): its whole vertices (and colours) again into its arena.
void UpdateGlobVerticesGpu(const GLOB& glob);
// The same into an entry's own copy of the glob's vertices (an instance of a shared GLOBSET lit at its own place).
void UpdateGlobColorsGpu(const GLOB& glob, const struct GLGLOB* pgl);
#if SLY2_FASTPATHS
// (the Switch) UpdateGlobColorsGpu in two halves: the colours packed on any thread (the pass helpers, right after they
// light the glob), then uploaded on the game thread
struct GLOBCOLORS
{
    std::vector<uint8_t> ab;   // per vertex its colours A and B, as the colour buffer holds them
    uint8_t aMin = 255;        // the lowest colour-A alpha (GLGLOB::aMinA)
    uint8_t aMax = 255;        // the highest (GLGLOB::aMaxA)
};
void PackGlobColors(const GLOB& glob, GLOBCOLORS& cols);
void UploadGlobColorsGpu(const GLOB& glob, const struct GLGLOB* pgl, const GLOBCOLORS& cols);
#endif
// A copy of a strip glob's vertices and indices for one instance (its own lit colours); not drawable until
// FlushInstanceGlobsGpu.
struct GLGLOB* PglInstanceCopyGpu(const GLOB& glob);
// A glob whose vertices move at run time (the rope meshes): its uploaded bounds set to this sphere.
void SetGlobBoundsGpu(const GLOB& glob, const glm::vec3& posCenter, float sRadius);
void FlushInstanceGlobsGpu();
// The GL texture of a shader's state frame (texture pass 0), for the 2D (kind-7 quads).
unsigned GlTextureOfShd(const SHD* pshd, int iframe);
// World start (shd.cpp ResetShaders): forgets the texture cache, keyed by the old world's records; the GL textures are
// deleted at the next lookup.
void ResetTextureCache();
void RenderScene(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts);
// The foreground kinds 22..25 (SCENEGLOBSET::fForeground) after the HUD, over a cleared depth.
void RenderSceneForeground(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts);

// (VR, vr.h) Both eyes of a stereo frame: the scene's records are chosen once (culled with matCull from posCenter,
// sorted, uploaded), then drawn into each eye's target with its own matrix.
struct STEREOVIEW
{
    glm::mat4 amatWorldToClip[2]{glm::mat4(1.0f), glm::mat4(1.0f)};
    glm::vec3 apos[2]{glm::vec3(0.0f), glm::vec3(0.0f)};
    glm::mat4 matCull{1.0f};        // a frustum holding both eyes' (the records' culling, billboards' up)
    glm::vec3 posCenter{0.0f}, dirCenter{1, 0, 0};
    unsigned afbo[2]{0, 0};
    int dx = 1, dy = 1;
    bool fMultiview = false;           // (OpenGL ES, GL_OVR_multiview2) afbo[0] holds both eyes as two views
    unsigned texMultiviewColor = 0;    // its colour (a two-layer array): the shadow phases' frame copies
    bool fFlipY = false;               // (Vulkan VR, straight into the OpenXR images) the clip space's Y flipped: the winding too
};
// The render targets, then the frame's pass and the foreground kinds, into both eyes.
void RenderSceneStereo(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts, const STEREOVIEW& sv);

// More instances of loaded globs to draw this frame (RIPs' models: the glob iglob of an entry at mat, faded by
// alpha); the particles fill them through the hook.
struct EXTRADRAW
{
    const SCENEGLOBSET* psgs = nullptr;
    size_t iglob = 0;
    glm::mat4 mat{1.0f};
    float alpha = 1.0f;
    bool fBounds = false;
    glm::vec3 posBounds{0.0f};
    float sBoundsRadius = 0.0f, sMRD = 1e10f;
};
using PFNEXTRADRAWS = void (*)(const SCENE& scene, std::vector<EXTRADRAW>& aextra);
extern PFNEXTRADRAWS g_pfnExtraDraws;

struct RENDERSTATS
{
    int cdraw = 0, cpass = 0, ccel = 0, ctexture = 0, cfailTexture = 0;
    int cfailAll = 0;   // (vkalpha) alpha-tested sub-draws no fragment passes: not drawn
    int cbatch = 0;     // GL draw calls for the passes (glMultiDrawElementsIndirect runs and special draws)
    int cculled = 0;    // records outside the view frustum
    int cmrd = 0;       // records beyond their MRD
};
extern RENDERSTATS g_renderstats;
