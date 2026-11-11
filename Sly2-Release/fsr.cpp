// Upscaling: see fsr.h.
#include "fsr.h"

#include "gl.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>

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
        if (fOk) glGenVertexArrays(1, &s_vao);
        std::printf("upscaling: FSR 1 %s\n", fOk ? "available" : "not available here (bilinear)");
    }
    return s_state > 0;
}

bool FUpscale(const SOURCE& src, GLuint fboDst, int x, int y, int dx, int dy, float gSharpness, bool fFlipY)
{
    if (!FAvailable() || (src.fArray && !s_shEasuArray.ID) || dx <= 0 || dy <= 0) return false;
    if (!s_fbo || s_dxTex != dx || s_dyTex != dy)
    {
        if (!s_fbo) glGenFramebuffers(1, &s_fbo);
        if (s_tex) glDeleteTextures(1, &s_tex);
        glGenTextures(1, &s_tex);
        glBindTexture(GL_TEXTURE_2D, s_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
        s_dxTex = dx;
        s_dyTex = dy;
    }
    static const int s_every = std::getenv("SLY2N_FSRTIME") ? std::max(1, std::atoi(std::getenv("SLY2N_FSRTIME"))) : 0;
    const auto tCpu = std::chrono::steady_clock::now();
    if (s_every)
    {
        TIMING& t = s_timing;
        if (!t.aq[0]) glGenQueries(4, t.aq);
        else if (t.iq >= 4)
        {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(t.aq[t.iq % 4], GL_QUERY_RESULT, &ns);
            t.msGpu += double(ns) / 1e6;
            if (++t.c == s_every)
            {
                std::printf("upscaling: FSR 1 %dx%d -> %dx%d: GPU %.3f ms, CPU %.3f ms a pass (%d passes)\n", src.dx, src.dy, dx, dy,
                            t.msGpu / t.c, t.msCpu / t.c, t.c);
                t.c = 0, t.msGpu = t.msCpu = 0.0;
            }
        }
        glBeginQuery(GL_TIME_ELAPSED, t.aq[t.iq % 4]);
    }
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
    if (s_every)
    {
        glEndQuery(GL_TIME_ELAPSED);
        ++s_timing.iq;
        s_timing.msCpu += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tCpu).count();
    }
    return true;
}

void Release()
{
    if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
    if (s_tex) glDeleteTextures(1, &s_tex);
    if (s_vao) glDeleteVertexArrays(1, &s_vao);
    s_fbo = s_tex = s_vao = 0;
    s_dxTex = s_dyTex = 0;
    if (s_state > 0)
    {
        s_shEasu.Delete();
        s_shRcas.Delete();
        if (s_shEasuArray.ID) s_shEasuArray.Delete();
    }
    s_state = 0;
}
}
