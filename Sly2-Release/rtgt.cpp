// See rtgt.h.

#include "rtgt.h"

#include "gl.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace
{
RTGT s_artgt[CRTGT];

void EnsureFbo(RTGT& r)
{
    if (r.fbo) return;
    GLuint fbo = 0, tex = 0, rbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r.dx, r.dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // TEX1 0x60 (bilinear), CLAMP 5 (clamp S and T)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, r.dx, r.dy);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
    // cleared as 0x1BE398's packet does until something is drawn
    glClearColor(0, 0, 0, 0);
    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    r.fbo = fbo;
    r.tex = tex;
    r.rboDepth = rbo;
}
}  // namespace

const void* (*g_pfnRtgtOwnerOfId)(int id) = nullptr;
void (*g_pfnRtgtTaken)(const void* powner, int irtgt) = nullptr;
void (*g_pfnRtgtGiven)(const void* powner) = nullptr;

RTGT* PrtgtGet(int irtgt) { return irtgt >= 0 && irtgt < CRTGT ? &s_artgt[irtgt] : nullptr; }

int IrtgtOfOwner(const void* powner)
{
    if (!powner) return -1;
    for (int i = 0; i < CRTGT; ++i)
        if (s_artgt[i].powner == powner) return i;
    return -1;
}

int IrtgtAcquire(const void* powner)
{
    if (!powner) return -1;
    const int i = IrtgtOfOwner(powner);
    if (i >= 0) return i;
    for (int j = 0; j < CRTGT; ++j)
        if (!s_artgt[j].powner)
        {
            s_artgt[j].powner = powner;
            if (g_pfnRtgtTaken) g_pfnRtgtTaken(powner, j);
            return j;
        }
    return -1;
}

void ReleaseRtgt(const void* powner)
{
    const int i = IrtgtOfOwner(powner);
    if (i < 0) return;
    s_artgt[i].powner = nullptr;
    if (g_pfnRtgtGiven) g_pfnRtgtGiven(powner);
}

void SetRtgtCamera(int irtgt, const glm::vec3& pos, const glm::vec3& b0, const glm::vec3& b1, const glm::vec3& b2,
                   float radFovY, float sNear)
{
    RTGT* p = PrtgtGet(irtgt);
    if (!p) return;
    (void)b2;   // left = b2 (the view's x row is -b2): implied by forward and up
    p->pos = pos;
    p->dirForward = b1;
    p->dirUp = b0;
    p->radFovY = radFovY;
    p->sNear = sNear;
}

// QoL (U25): the original's targets are 128 x 128 for a 448-line frame; natively they follow the render height
// (128 per 224 lines: twice the original's texel density, so the speaker windows stay sharp up to 4K).
// SLY2N_RTGT128=1: the original's 128 x 128.
int DxRtgtForDisplay()
{
    static const bool s_f128 = std::getenv("SLY2N_RTGT128") != nullptr;
    if (s_f128 || g_gl.renderHeight <= 0) return 128;
    return std::clamp(int(std::lround(128.0 * g_gl.renderHeight / 224.0)), 128, 2048);
}

unsigned GlTextureOfRtgt(int irtgt)
{
    RTGT* p = PrtgtGet(irtgt);
    if (!p) return 0;
    if (const int dx = DxRtgtForDisplay(); g_gl.window && dx != p->dx)
    {
        if (p->fbo)
        {
            glDeleteFramebuffers(1, &p->fbo);
            glDeleteTextures(1, &p->tex);
            glDeleteRenderbuffers(1, &p->rboDepth);
            p->fbo = p->tex = p->rboDepth = 0;
        }
        p->dx = p->dy = dx;
    }
    EnsureFbo(*p);
    return p->tex;
}
