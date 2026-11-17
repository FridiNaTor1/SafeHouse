// Shader animations (Sly 2 module 0x1ED7A8-0x1EE978, Sly 1 shdanim.c / loop.c / pingpong.c / shuffle.c /
// eyes.c / scroller.c / circler.c): frame and UV-offset animation of textures. The original pushes the dirty
// states into registered VIF unpack targets (0x1EE7B0); natively the renderer reads SAA::iframe / du / dv.
// Reference ports: Sly2-Reference game/shdanim.cpp (LOOP, EYES, SCROLLER, CIRCLER); PINGPONG and SHUFFLE follow
// the decomp's inits (0x1EDD60) and Sly1-Release's updates.

#include "shd.h"
#include "so.h"
#include "worldaccess.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace
{
float F(const SAA* p, int ib) { float f; std::memcpy(&f, p->ab + ib, 4); return f; }
// orig: 0x0011D448 GRandInRange (orig3: 0x11E940, the same code in Sly 3)
float GRandInRange(float lo, float hi) { return lo + (hi - lo) * (float(std::rand()) / float(RAND_MAX)); }

// orig: 0x001EDA68 SetSaiIframe (clamped to the frame count)
void SetIframe(SAA* p, int n) { p->iframe = std::clamp(n, 0, std::max(p->cframe - 1, 0)); }
// orig: 0x001EDAD8 SetSaiDuDv (1/4096, mod 1)
void SetDuDv(SAA* p, float du, float dv)
{
    p->du = static_cast<int16_t>(int(std::floor(du * 4096.0f + 0.5f)) & 0xFFF);
    p->dv = static_cast<int16_t>(int(std::floor(dv * 4096.0f + 0.5f)) & 0xFFF);
}

// LOOP / PINGPONG: aflt 0..3 the time ranges, 4 rate (+0x60), 5 frame (+0x64), 6/7 pause (+0x68 / +0x6C)
void InitLoop(SAA* p, float cRate)
{
    for (int i = 0; i < 4; ++i) p->aflt[i] = F(p, 0xC + 4 * i);
    uint16_t c;
    std::memcpy(&c, p->ab + 0x1C, 2);
    p->aflt[5] = float(c);
    p->aflt[4] = cRate * float(p->cframe) / std::max(GRandInRange(p->aflt[0], p->aflt[1]), 1e-6f);
    p->aflt[6] = p->aflt[7] = GRandInRange(p->aflt[2], p->aflt[3]);
    SetIframe(p, int(p->aflt[5]));
}

// EYES 0x1EE100 SetEyesEyess: aflt 0 dtBlink, 1/2 dtOpen range, 3 uDoubleBlink, 4 dtOpen, 5 dframe, 6 frame, 7 uOpen
void SetEyes(SAA* p, int eyess, float t)
{
    if (eyess == p->eyess) return;
    switch (eyess)
    {
    case 0:
        p->aflt[6] = float(p->cframe - 1) * p->aflt[7];
        p->aflt[5] = float(p->cframe * 2) * (1.0f - p->aflt[7]) / std::max(p->aflt[0], 1e-6f);
        p->aflt[4] = GRandInRange(0, 1) < p->aflt[3] ? 0.0f : GRandInRange(p->aflt[1], p->aflt[2]);
        break;
    case 2:
        p->aflt[6] = float(p->cframe - 1);
        break;
    default:
        break;
    }
    p->eyess = eyess;
    p->tEyess = t;
}
}  // namespace

bool FSaaForShader(const SAA* psaa, const SHD* pshd)
{
    if (!psaa || !pshd) return false;
    if (psaa->oid == pshd->id) return true;
    if (psaa->cid == SAAK2_Eyes)
    {
        int32_t id2;
        std::memcpy(&id2, psaa->ab + 0x1C, 4);
        return id2 == pshd->id;
    }
    return false;
}

void InitSaa(SAA* p)
{
    switch (p->cid)
    {
    case SAAK2_Loop: InitLoop(p, 1.0f); break;           // orig: 0x001EDB98 InitLoop
    case SAAK2_PingPong: InitLoop(p, 2.0f); break;       // orig: 0x001EDD60 InitPingpong
    case SAAK2_Shuffle:                                  // orig: 0x001EDFA0 InitShuffle (pause range)
        p->aflt[0] = F(p, 0xC);
        p->aflt[1] = F(p, 0x10);
        p->aflt[2] = GRandInRange(p->aflt[0], p->aflt[1]);
        break;
    case SAAK2_Eyes:                                     // orig: 0x001EE090 InitEyes
        for (int i = 0; i < 4; ++i) p->aflt[i] = F(p, 0xC + 4 * i);
        p->aflt[7] = 0.0f;
        SetEyes(p, 0, 0.0f);
        SetIframe(p, int(p->aflt[6]));
        break;
    case SAAK2_Scroller:                                 // orig: 0x001EE480 InitScroller (speeds, wraps; +0x60 /
        for (int i = 0; i < 4; ++i) p->aflt[i] = F(p, 0xC + 4 * i);  // +0x64 the master speeds 1: aflt[4] / [5])
        p->aflt[4] = 1.0f;
        p->aflt[5] = 1.0f;
        break;
    case SAAK2_Circler:                                  // orig: 0x001EE688 InitCircler (speed, radius, centre)
        for (int i = 0; i < 4; ++i) p->aflt[i] = F(p, 0xC + 4 * i);
        break;
    case SAAK3_Follow:
    {
        // orig3: 0x237F10 (the record's two axes +0xC / +0x18, the third their cross product; the inverse kept at +0x60:
        // aflt[0..8], column-major: the owner's place in the axes' units)
        const glm::vec3 a(F(p, 0xC), F(p, 0x10), F(p, 0x14)), b(F(p, 0x18), F(p, 0x1C), F(p, 0x20));
        const glm::mat3 m(a, b, glm::cross(a, b));
        const glm::mat3 inv = std::fabs(glm::determinant(m)) > 0.0f ? glm::inverse(m) : glm::mat3(0.0f);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) p->aflt[3 * c + r] = inv[c][r];
        break;
    }
    default:
        break;
    }
}

namespace
{
void UpdateSaa(SAA* p, float t, float dt)
{
    switch (p->cid)
    {
    case SAAK2_Loop:  // orig: 0x001EDC40 UpdateLoop
        if (p->cframe < 2) return;
        if (p->aflt[7] > 0) { p->aflt[7] -= dt; return; }
        p->aflt[5] += p->aflt[4] * dt;
        if (p->aflt[5] >= float(p->cframe))
        {
            p->aflt[4] = float(p->cframe) / std::max(GRandInRange(p->aflt[0], p->aflt[1]), 1e-6f);
            p->aflt[7] = p->aflt[6] = GRandInRange(p->aflt[2], p->aflt[3]);
        }
        p->aflt[5] = std::fmod(p->aflt[5], float(p->cframe));
        if (p->aflt[5] < 0) p->aflt[5] += float(p->cframe);
        SetIframe(p, int(p->aflt[5]));
        break;
    case SAAK2_PingPong:  // orig: 0x001EDE10 UpdatePingpong (Sly 1 pingpong.cpp)
    {
        if (p->cframe < 2) return;
        if (p->aflt[7] > 0) { p->aflt[7] -= dt; return; }
        const float step = p->aflt[4] * dt;
        p->aflt[5] += step;
        if (p->aflt[5] >= float(p->cframe)) { p->aflt[5] -= step; p->aflt[4] = -p->aflt[4]; }
        if (p->aflt[5] < 0)
        {
            p->aflt[5] = 0;
            p->aflt[4] = float(p->cframe * 2) / std::max(GRandInRange(p->aflt[0], p->aflt[1]), 1e-6f);
            p->aflt[7] = GRandInRange(p->aflt[2], p->aflt[3]);
        }
        SetIframe(p, int(p->aflt[5]));
        break;
    }
    case SAAK2_Shuffle:  // orig: 0x001EDFE0 UpdateShuffle (Sly 1 shuffle.cpp)
        if (p->cframe < 2) return;
        if (p->aflt[2] > 0) p->aflt[2] -= dt;
        else
        {
            SetIframe(p, (p->iframe + 1 + std::rand() % (p->cframe - 1)) % p->cframe);
            p->aflt[2] = GRandInRange(p->aflt[0], p->aflt[1]);
        }
        break;
    case SAAK2_Eyes:  // orig: 0x001EE228 UpdateEyes
    {
        if (p->cframe < 2) return;
        int next = p->eyess;
        switch (next)
        {
        case 0:
            if (p->aflt[4] <= t - p->tEyess) next = 1;
            break;
        case 1:
        {
            const float d = p->aflt[5] * dt;
            p->aflt[6] += d;
            if (float(p->cframe) <= p->aflt[6])
            {
                p->aflt[6] -= d;
                next = int(p->aflt[6]) == p->cframe - 1 ? 2 : 3;
            }
            break;
        }
        case 2:
            if (p->aflt[7] < 1.0f) next = 3;
            break;
        case 3:
            p->aflt[6] -= p->aflt[5] * dt;
            if (p->aflt[6] <= float(p->cframe) * p->aflt[7]) next = 0;
            break;
        }
        SetEyes(p, next, t);
        SetIframe(p, int(p->aflt[6]));
        break;
    }
    case SAAK2_Scroller:  // orig: 0x001EE4E0 UpdateScroller
    {
        // orig3: 0x237CA0: the offsets kept as floats (+0x2C / +0x30: aflt[6] / [7]), each advanced by speed x the
        // master speed x dt and wrapped by its period (GModPositive 0x11EDB8), then quantized (0x237298). Sly 2 re-read
        // the quantized 1/4096 offset each frame: a slow scroll's step rounded to 0 or 1/4096 every frame (stuck, or
        // much faster than authored).
        float u = p->aflt[6] + p->aflt[0] * p->aflt[4] * dt;
        float v = p->aflt[7] + p->aflt[1] * p->aflt[5] * dt;
        auto wrap = [](float g, float m) {
            if (m == 0.0f) return g;   // (fmodf by 0: NaN)
            float r = std::fmod(g, m);
            if (r < 0.0f) r += m;
            return r;
        };
        u = wrap(u, p->aflt[2]);
        v = wrap(v, p->aflt[3]);
        p->aflt[6] = u;
        p->aflt[7] = v;
        SetDuDv(p, u, v);
        break;
    }
    case SAAK2_Circler:  // orig: 0x001EE6D8 UpdateCircler
    {
        const float a = t * p->aflt[0];
        SetDuDv(p, std::sin(a) * p->aflt[1] + p->aflt[2], std::cos(a) * p->aflt[1] + p->aflt[3]);
        break;
    }
    case SAAK3_Follow:
    {
        // orig3: 0x237FA8 (the owner's place (vt slot 36's matrix) in the axes, x / y wrapped to 0..1 (0x11EDB8) as the
        // offsets (0x237298)): m_ext's sea 31490, moved with the camera in steps of its grid, keeps its texture still in the
        // world (natively it jumped by a step's worth whenever the sea moved)
        ALO* palo = p->paloOwner;
        if (!palo) break;
        const XF* pxf = PxfObj(palo);
        const glm::vec3 pos(pxf ? pxf->matWorld[3] : MatWorldOfAlo(palo)[3]);
        glm::mat3 inv;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) inv[c][r] = p->aflt[3 * c + r];
        const glm::vec3 l = inv * pos;
        auto wrap = [](float g) { float r = std::fmod(g, 1.0f); return r < 0.0f ? r + 1.0f : r; };
        SetDuDv(p, wrap(l.x), wrap(l.y));
        break;
    }
    default:
        break;
    }
}
}  // namespace

// orig: 0x001EE3B0 (the eyes' closedness g (+0x78): 1 or more shut (state 2), else reopened through state 0 (its
// state reset first, so the frame follows g), the frame set)
// orig: 0x001EE678 SetScrollerMasterSpeeds (a scroller's +0x60 / +0x64, the speeds' factors)
void SetScrollerMasterSpeeds(SAA* p, float su, float sv)
{
    if (!p || p->cid != SAAK2_Scroller) return;
    p->aflt[4] = su;
    p->aflt[5] = sv;
}

// orig: 0x001EDAD8 (the offsets set at once, in 1/4096)
void SetSaaOffsets(SAA* p, float du, float dv)
{
    if (!p) return;
    if (p->cid == SAAK2_Scroller) { p->aflt[6] = du; p->aflt[7] = dv; }   // (orig3 0x237298 stores the floats)
    SetDuDv(p, du, dv);
}

void SetSaaEyesClosed(SAA* p, float g, float t)
{
    if (!p || p->cid != SAAK2_Eyes) return;
    p->aflt[7] = g;
    if (1.0f <= g) SetEyes(p, 2, t);  // 0x1EE100
    else
    {
        p->eyess = -1;  // (+0x64)
        SetEyes(p, 0, t);
    }
    SetIframe(p, int(p->aflt[6]));  // 0x1EDA68
}

void UpdateShaderAnimations(float t, float dt)
{
    // (debug: SLY3_SAAPRINT=1: every shader animation once: class, oid, frames, its description's floats)
    if (static bool s_fPrinted = false; !s_fPrinted && std::getenv("SLY3_SAAPRINT"))
    {
        s_fPrinted = true;
        for (auto& psaa : g_shds.asaa)
            std::printf("saa cid 0x%X oid %d frames %d grf 0x%X floats %g %g %g %g\n", psaa->cid, psaa->oid, psaa->cframe, psaa->grf, F(psaa.get(), 0xC),
                        F(psaa.get(), 0x10), F(psaa.get(), 0x14), F(psaa.get(), 0x18));
    }
    for (auto& psaa : g_shds.asaa)
    {
        if (psaa->tUpdated == t) continue;   // FUpdatableSaa 0x1EDA20
        psaa->tUpdated = t;
        UpdateSaa(psaa.get(), t, dt);
    }
}
