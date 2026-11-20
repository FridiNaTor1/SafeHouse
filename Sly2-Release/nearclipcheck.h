#pragma once
// The near-plane clip stage's startup check (gl.cpp): glob.geom (R51) is a geometry stage, and drivers have compiled
// it wrongly (llvmpipe's OpenGL and, by the reports, AMD's Windows driver and Adreno 750: the binocucom's speaker
// windows came out empty). Before the game draws, a small scene is drawn with the glob programs that carry the stage,
// the way render.cpp draws (the device's texture path, vertex pulling or attributes, render-to-texture with the
// depth-stencil format of the speaker windows), read back and compared with the pixels the stage must give, computed
// here on the CPU. gl.cpp falls back to the GPU's own clipping (SLY2N_NEARCLIP=0) when it fails.

#include <string>

class GLSHADER;

struct NEARCLIPCHECK
{
    bool fOk = false;
    std::string detail;   // what was compared and how many pixels / triangles were off
};

// Draws the check scene once with each program in apsh (those with an ID) and compares. The programs are the glob
// variants gl.cpp built with the stage; fPull: glob.vert reads its vertices itself (CANE_PULL, g_fDrawUniform);
// fBindless / fArrays: the pass draws' textures as handles / array layers (g_fGlobBindless / g_fGlobArrays); fTexh4:
// the handle buffer holds four words a draw (the Vulkan renderer's pulled programs). The GL state the check touches is
// left as GL's defaults (no framebuffer, program, vertex array, textures bound; depth test off).
NEARCLIPCHECK NearClipCheck(const GLSHADER* const* apsh, int cpsh, bool fPull, bool fBindless, bool fArrays, bool fTexh4);

// The check's result per driver and build, remembered in the user folder (nearclip-check.txt): the key is the GL
// renderer and version strings (the Vulkan renderer's name the device and driver version), the build and the programs'
// variant. NearClipCached: 1 passed, 0 failed, -1 not checked yet.
std::string NearClipKey(const std::string& variant);
int NearClipCached(const std::string& key);
void NearClipRemember(const std::string& key, bool fOk);
