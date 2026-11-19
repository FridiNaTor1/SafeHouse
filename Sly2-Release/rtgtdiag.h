#pragma once
// SLY2N_RTGTDUMP: diagnosis of the render targets (rtgt.h: the binocucom's speaker windows) on a machine we cannot test
// (the AMD Windows report: the windows empty on Vulkan and OpenGL). SLY2N_RTGTDUMP=1: into <user folder>/rtgtdump;
// any other value but 0: that folder. For the frames that draw a target, the first ten and then every 30th (every one
// with SLY2N_RTGTDUMPALL=1), one line a target on stderr (flushed at once): its size, framebuffer, camera, matrix, the
// records it drew and culled, the draws and triangles, and its pixels' numbers (how many differ from the clear, the
// alpha); at a few of those frames (the 1st, 5th, 30th, 120th, 600th, 1800th) the targets' pictures (rtgt<i>_<n>.png,
// alpha kept) and the frame as shown (frame_<n>.png). Also "rtgtdump.txt" in that folder: the same lines.

#include <glm/glm.hpp>

#include <string>

struct RTGTDIAG
{
    int irtgt = -1;
    const void* powner = nullptr;
    int dx = 0, dy = 0;
    unsigned fbo = 0;
    glm::vec3 pos{0.0f}, dirForward{0.0f}, dirUp{0.0f};
    float radFovY = 0.0f, sNear = 0.0f;
    glm::mat4 matWorldToClip{1.0f};
    int crec = 0, cculled = 0, cmrd = 0, cdraw = 0, cpass = 0, cbatch = 0, cfailAll = 0, cnoclip = 0;
    long long ctri = 0;
};

// Whether target irtgt's pass this frame is reported (the first call of a frame counts the frame)
bool FRtgtDiagPass(int irtgt);
// After the pass: its line, and its picture at the picture frames
void RtgtDiagReport(const RTGTDIAG& d);
// After all the targets of a frame (RenderRenderTargets)
void RtgtDiagEndTargets();
// (game.cpp, after the frame is drawn) the path to write the shown frame to this frame, else empty
std::string RtgtDiagCompositePath();
