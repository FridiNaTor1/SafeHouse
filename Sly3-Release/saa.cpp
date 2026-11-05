// Shader animations (Sly 2 module 0x1ED7A8-0x1EE978, Sly 1 shdanim.c / loop.c / pingpong.c / shuffle.c /
// eyes.c / scroller.c / circler.c): frame and UV-offset animation of textures. The original pushes the dirty
// states into registered VIF unpack targets (0x1EE7B0); natively the renderer reads SAA::iframe / du / dv.
// Reference ports: Sly2-Reference game/shdanim.cpp (LOOP, EYES, SCROLLER, CIRCLER); PINGPONG and SHUFFLE follow
// the decomp's inits (0x1EDD60) and Sly1-Release's updates.

#include "shd.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
float F(const SAA* p, int ib) { float f; std::memcpy(&f, p->ab + ib, 4); return f; }
// orig: 0x0011D448 GRandInRange
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
    case SAAK2_Scroller:                                 // orig: 0x001EE480 InitScroller (speeds, wraps)
    case SAAK2_Circler:                                  // orig: 0x001EE688 InitCircler (speed, radius, centre)
        for (int i = 0; i < 4; ++i) p->aflt[i] = F(p, 0xC + 4 * i);
        break;
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
        const float u = float(p->du) / 4096.0f + p->aflt[0] * dt;
        const float v = float(p->dv) / 4096.0f + p->aflt[1] * dt;
        SetDuDv(p, p->aflt[2] != 0 ? std::fmod(u, p->aflt[2]) : u, p->aflt[3] != 0 ? std::fmod(v, p->aflt[3]) : v);
        break;
    }
    case SAAK2_Circler:  // orig: 0x001EE6D8 UpdateCircler
    {
        const float a = t * p->aflt[0];
        SetDuDv(p, std::sin(a) * p->aflt[1] + p->aflt[2], std::cos(a) * p->aflt[1] + p->aflt[3]);
        break;
    }
    default:
        break;
    }
}
}  // namespace

// orig: 0x001EE3B0 (the eyes' closedness g (+0x78): 1 or more shut (state 2), else reopened through state 0 (its
// state reset first, so the frame follows g), the frame set)
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
    for (auto& psaa : g_shds.asaa)
    {
        if (psaa->tUpdated == t) continue;   // FUpdatableSaa 0x1EDA20
        psaa->tUpdated = t;
        UpdateSaa(psaa.get(), t, dt);
    }
}
