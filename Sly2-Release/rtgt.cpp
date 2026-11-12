// See rtgt.h.

#include "rtgt.h"

#include "gl.h"
#include "gl_backend.h"

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
    if (const cane::draw::Packets* pkf = PpacketsFbo())
    {
        // (native: the texture, the depth-stencil renderbuffer and the framebuffer, nothing bound; the clear below binds it)
        tex = GlNewTexture(r.dx, r.dy, GL_LINEAR, true);
        cane::draw::RenderbufferDesc rd;
        rd.width = uint32_t(r.dx), rd.height = uint32_t(r.dy), rd.format = cane::draw::kRenderbufferDepth32fStencil8;
        rbo = pkf->createRenderbuffer(rd);
        fbo = GlNewFramebuffer(tex, rbo);
    }
    else
    {
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    // TEX1 0x60 (bilinear), CLAMP 5 (clamp S and T)
    tex = GlNewTexture(r.dx, r.dy, GL_LINEAR, true);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, r.dx, r.dy);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
    }
    // cleared as 0x1BE398's packet does until something is drawn
    // (draw packets, platform/draw_packet.h: the Vulkan renderer; the masks and the scissor as they are, as for the GL calls)
    const cane::draw::Packets* pk = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    if (pk && pk->clear && pk->bindTarget)
    {
        cane::draw::Clear c;
        c.clear = cane::draw::kClearColor | cane::draw::kClearDepth;
        c.depth = 0.0f;
        c.stencilMaskKept = 1;
        pk->bindTarget(fbo);
        pk->clear(c);
        pk->bindTarget(0);
    }
    else
    {
        glClearColor(0, 0, 0, 0);
        glClearDepth(0.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
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
            GlDeleteFramebuffers(1, &p->fbo);
            GlDeleteTexture(p->tex);
            GlDeleteRenderbuffer(p->rboDepth);
            p->fbo = p->tex = p->rboDepth = 0;
        }
        p->dx = p->dy = dx;
    }
    EnsureFbo(*p);
    return p->tex;
}
