// Upscaling: see fsr.h.
#include "fsr.h"

#include "gl.h"
#include "gl_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fsr
{
namespace
{
GLSHADER s_shEasu, s_shEasuArray, s_shRcas;
int s_state = 0;   // 0 not tried, 1 built, -1 failed
GLuint s_vao = 0, s_fbo = 0, s_tex = 0;
int s_dxTex = 0, s_dyTex = 0;

// (SLY2N_FSRTIME=N, not with SLY2N_FPSLOG (its query spans the frame): the pass's GPU time (a timer query read back a
// few calls later) and the CPU time of its calls,
// averaged and printed every N calls)
struct TIMING
{
    int every = 0;
    GLuint aq[4] = {};
    int iq = 0, c = 0;
    double msGpu = 0.0, msCpu = 0.0;
} s_timing;

// (draw packets, platform/draw_packet.h: the Vulkan renderer) per program variant [0] EASU, [1] EASU array, [2] RCAS,
// [3] RCAS flipped: its packet state (made once per program; 0: the GL calls) and its uniform locations (each Loc is a
// string search in the layer)
struct PKPROG
{
    GLuint prog = 0;
    uint32_t id = 0;
    GLint locCon0 = -1, locLayer = -1, locSharpness = -1, locSrcMin = -1, locSrcMax = -1, locPosOut = -1;
};
PKPROG s_apkprog[4];

void SetPassState(GLuint fbo, int x, int y, int dx, int dy)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(x, y, dx, dy);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// The two passes as draw packets. The GL-style state afterwards is what UpscaleGl's calls leave: framebuffer fboDst,
// viewport (x, y, dx, dy), blend / depth test / stencil test / scissor / culling off, all colours written, unit 0 active
// with no texture and sampler 0, vertex array 0, no program; the depth write / compare and the stencil function /
// operations, which those calls do not set, kept. False: the backend cannot (nothing done).
bool FUpscalePackets(const SOURCE& src, GLuint fboDst, int x, int y, int dx, int dy, float gSharpness, bool fFlipY)
{
    using namespace cane::draw;
    const Packets* pk = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    if (!pk || !pk->beginPass || !pk->scissor || !pk->drawArrays || !pk->bindTexture || !pk->setConstant) return false;
    const GLSHADER* const ash[4] = {&s_shEasu, &s_shEasuArray, &s_shRcas, &s_shRcas};
    const int iEasu = src.fArray ? 1 : 0, iRcas = fFlipY ? 3 : 2;
    for (int i : {iEasu, iRcas})
    {
        PKPROG& pp = s_apkprog[i];
        const GLSHADER& sh = *ash[i];
        if (pp.prog == sh.ID) continue;
        pp = PKPROG{};
        pp.prog = sh.ID;
        State st;
        st.program = sh.ID;
        st.blend = 0, st.cull = 0, st.depthTest = 0, st.stencilTest = 0, st.colorMask = 0xF;
        st.keep = kKeepDepthWrite | kKeepDepthCompare | kKeepStencil;
        if (const GLint loc = sh.Loc("tex"); loc >= 0) st.aconst[st.cconst++] = {loc, 0};
        if (i >= 2)
            if (const GLint loc = sh.Loc("fFlipY"); loc >= 0) st.aconst[st.cconst++] = {loc, i == 3 ? 1u : 0u};
        pp.id = pk->createState(st);
        pp.locCon0 = sh.Loc("con0"), pp.locLayer = sh.Loc("layer"), pp.locSharpness = sh.Loc("sharpness");
        pp.locSrcMin = sh.Loc("srcMin"), pp.locSrcMax = sh.Loc("srcMax"), pp.locPosOut = sh.Loc("posOut");
    }
    const PKPROG &ppEasu = s_apkprog[iEasu], &ppRcas = s_apkprog[iRcas];
    if (!ppEasu.id || !ppRcas.id) return false;
    auto put = [&](const PKPROG& pp, GLint loc, const void* pv, uint32_t cb) {
        if (loc >= 0) pk->setConstant(pp.prog, loc, pv, cb);
    };
    // EASU: the source rectangle -> the pass's texture (FsrEasuCon's con0; the other constants are the fixed tap offsets)
    Pass ps;
    ps.framebuffer = s_fbo;
    ps.viewport[2] = dx, ps.viewport[3] = dy;
    pk->beginPass(ps);
    pk->scissor(nullptr);
    const float sx = float(src.dx) / float(dx), sy = float(src.dy) / float(dy);
    const float con0[4] = {sx, sy, 0.5f * sx - 0.5f, 0.5f * sy - 0.5f};
    const int32_t srcMin[2] = {src.x, src.y}, srcMax[2] = {src.x + src.dx - 1, src.y + src.dy - 1};
    const float posOut0[2] = {0.0f, 0.0f};
    put(ppEasu, ppEasu.locCon0, con0, sizeof con0);
    put(ppEasu, ppEasu.locSrcMin, srcMin, sizeof srcMin);
    put(ppEasu, ppEasu.locSrcMax, srcMax, sizeof srcMax);
    put(ppEasu, ppEasu.locPosOut, posOut0, sizeof posOut0);
    Constant aconst[1];
    uint32_t cconst = 0;
    if (src.fArray && ppEasu.locLayer >= 0) aconst[cconst++] = {ppEasu.locLayer, uint32_t(src.layer)};
    pk->bindTexture(0, src.tex, src.fArray ? 1 : 0, 0);   // (sampler 0: glBindSampler(0, 0))
    pk->drawArrays(ppEasu.id, s_vao, 0, 3, aconst, cconst);
    pk->bindTexture(0, 0, src.fArray ? 1 : 0, kSamplerKept);
    // RCAS: the pass's texture -> the destination rectangle
    ps.framebuffer = fboDst;
    ps.viewport[0] = x, ps.viewport[1] = y, ps.viewport[2] = dx, ps.viewport[3] = dy;
    pk->beginPass(ps);
    const int32_t rMin[2] = {0, 0}, rMax[2] = {dx - 1, dy - 1};
    const float posOut[2] = {float(x), float(y)};
    put(ppRcas, ppRcas.locSrcMin, rMin, sizeof rMin);
    put(ppRcas, ppRcas.locSrcMax, rMax, sizeof rMax);
    put(ppRcas, ppRcas.locPosOut, posOut, sizeof posOut);
    uint32_t bits;
    std::memcpy(&bits, &gSharpness, 4);
    cconst = 0;
    if (ppRcas.locSharpness >= 0) aconst[cconst++] = {ppRcas.locSharpness, bits};
    pk->bindTexture(0, s_tex, 0, kSamplerKept);
    pk->drawArrays(ppRcas.id, s_vao, 0, 3, aconst, cconst);
    pk->bindTexture(0, 0, 0, kSamplerKept);
    pk->drawArrays(ppRcas.id, 0, 0, 0, nullptr, 0);   // (no triangles: vertex array 0, as the GL calls leave it)
    if (pk->useProgram) pk->useProgram(0);
    else glUseProgram(0);
    return true;
}

void UpscaleGl(const SOURCE& src, GLuint fboDst, int x, int y, int dx, int dy, float gSharpness, bool fFlipY)
{
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindVertexArray(s_vao);
    // EASU: the source rectangle -> the pass's texture (FsrEasuCon's con0; the other constants are the fixed tap offsets)
    SetPassState(s_fbo, 0, 0, dx, dy);
    const GLSHADER& easu = src.fArray ? s_shEasuArray : s_shEasu;
    const GLenum target = src.fArray ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    easu.Use();
    const float sx = float(src.dx) / float(dx), sy = float(src.dy) / float(dy);
    glUniform1i(easu.Loc("tex"), 0);
    if (src.fArray) glUniform1i(easu.Loc("layer"), src.layer);
    glUniform4f(easu.Loc("con0"), sx, sy, 0.5f * sx - 0.5f, 0.5f * sy - 0.5f);
    glUniform2i(easu.Loc("srcMin"), src.x, src.y);
    glUniform2i(easu.Loc("srcMax"), src.x + src.dx - 1, src.y + src.dy - 1);
    glUniform2f(easu.Loc("posOut"), 0.0f, 0.0f);
    glBindTexture(target, src.tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(target, 0);
    // RCAS: the pass's texture -> the destination rectangle
    SetPassState(fboDst, x, y, dx, dy);
    s_shRcas.Use();
    glUniform1i(s_shRcas.Loc("tex"), 0);
    glUniform1f(s_shRcas.Loc("sharpness"), gSharpness);
    glUniform2i(s_shRcas.Loc("srcMin"), 0, 0);
    glUniform2i(s_shRcas.Loc("srcMax"), dx - 1, dy - 1);
    glUniform2f(s_shRcas.Loc("posOut"), float(x), float(y));
    glUniform1i(s_shRcas.Loc("fFlipY"), fFlipY ? 1 : 0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}
}

float RScalePreset(int preset)
{
    switch (preset)
    {
    case PRESET_UltraQuality: return 1.3f;
    case PRESET_Balanced: return 1.7f;
    case PRESET_Performance: return 2.0f;
    default: return 1.5f;
    }
}

void RenderSizeForPreset(int preset, int dxOut, int dyOut, int* pdx, int* pdy)
{
    const double r = RScalePreset(preset);
    *pdx = std::max(1, int(std::lround(dxOut / r)));
    *pdy = std::max(1, int(std::lround(dyOut / r)));
}

float GSharpness(int percent)
{
    const float stops = 2.0f * float(100 - std::clamp(percent, 0, 100)) / 100.0f;
    return std::exp2(-stops);
}

bool FAvailable()
{
    if (s_state == 0)
    {
        const bool fOk = s_shEasu.FInit("vrfull.vert", "fsreasu.frag", "") && s_shRcas.FInit("vrfull.vert", "fsrrcas.frag", "");
        if (!fOk || !s_shEasuArray.FInit("vrfull.vert", "fsreasu.frag", "#define CANE_ARRAY 1\n")) s_shEasuArray.ID = 0;
        s_state = fOk ? 1 : -1;
        if (fOk) s_vao = GlNewVertexArray();
        std::printf("upscaling: FSR 1 %s\n", fOk ? "available" : "not available here (bilinear)");
    }
    return s_state > 0;
}

bool FUpscale(const SOURCE& src, GLuint fboDst, int x, int y, int dx, int dy, float gSharpness, bool fFlipY)
{
    if (!FAvailable() || (src.fArray && !s_shEasuArray.ID) || dx <= 0 || dy <= 0) return false;
    if (!s_fbo || s_dxTex != dx || s_dyTex != dy)
    {
        const cane::draw::Packets* const pkf = PpacketsFbo();
        if (!s_fbo && !pkf) glGenFramebuffers(1, &s_fbo);
        GlDeleteTexture(s_tex);
        s_tex = GlNewTexture(dx, dy, GL_NEAREST, true);
        if (!PpacketsTex()) glBindTexture(GL_TEXTURE_2D, 0);
        if (pkf)
        {
            // (native: the pass's texture attached, nothing bound; the passes bind their targets)
            cane::draw::Attachment c;
            c.object = s_tex;
            if (s_fbo) pkf->attach(s_fbo, cane::draw::kClearColor, c);
            else s_fbo = pkf->createFramebuffer(c, cane::draw::Attachment{});
        }
        else
        {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
        }
        s_dxTex = dx;
        s_dyTex = dy;
    }
    static const int s_every = std::getenv("SLY2N_FSRTIME") ? std::max(1, std::atoi(std::getenv("SLY2N_FSRTIME"))) : 0;
    const auto tCpu = std::chrono::steady_clock::now();
    // (the timer queries natively on the Vulkan renderer: draw_packet.h's timers)
    const cane::draw::Packets* pkTime = cane::gl::ActiveBackend() ? cane::gl::ActiveBackend()->packets : nullptr;
    if (pkTime && !(pkTime->createTimer && pkTime->beginTimer && pkTime->endTimer && pkTime->timerResult)) pkTime = nullptr;
    if (s_every)
    {
        TIMING& t = s_timing;
        if (!t.aq[0])
        {
            if (pkTime) for (GLuint& q : t.aq) q = pkTime->createTimer();
            else glGenQueries(4, t.aq);
        }
        else if (t.iq >= 4)
        {
            GLuint64 ns = 0;
            if (pkTime) ns = pkTime->timerResult(t.aq[t.iq % 4]);
            else glGetQueryObjectui64v(t.aq[t.iq % 4], GL_QUERY_RESULT, &ns);
            t.msGpu += double(ns) / 1e6;
            if (++t.c == s_every)
            {
                std::printf("upscaling: FSR 1 %dx%d -> %dx%d: GPU %.3f ms, CPU %.3f ms a pass (%d passes)\n", src.dx, src.dy, dx, dy,
                            t.msGpu / t.c, t.msCpu / t.c, t.c);
                t.c = 0, t.msGpu = t.msCpu = 0.0;
            }
        }
        if (pkTime) pkTime->beginTimer(t.aq[t.iq % 4]);
        else glBeginQuery(GL_TIME_ELAPSED, t.aq[t.iq % 4]);
    }
    if (!FUpscalePackets(src, fboDst, x, y, dx, dy, gSharpness, fFlipY)) UpscaleGl(src, fboDst, x, y, dx, dy, gSharpness, fFlipY);
    if (s_every)
    {
        if (pkTime) pkTime->endTimer();
        else glEndQuery(GL_TIME_ELAPSED);
        ++s_timing.iq;
        s_timing.msCpu += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tCpu).count();
    }
    return true;
}

void Release()
{
    if (s_fbo) GlDeleteFramebuffers(1, &s_fbo);
    GlDeleteTexture(s_tex);
    GlDeleteVertexArray(s_vao);
    s_fbo = s_tex = s_vao = 0;
    s_dxTex = s_dyTex = 0;
    for (PKPROG& pp : s_apkprog) pp = PKPROG{};
    if (s_state > 0)
    {
        s_shEasu.Delete();
        s_shRcas.Delete();
        if (s_shEasuArray.ID) s_shEasuArray.Delete();
    }
    s_state = 0;
}
}
