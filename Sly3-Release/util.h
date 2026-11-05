#pragma once

// Math utilities (Sly 1's util.c; orig 0x11CF60..0x11E000) and the Hermite curves of 0x117030 / 0x117120.

// The functions both the animation and physics ports need live in xfmath.h (the animation code: RadNormalize,
// GLimitAbs, GSmooth / SMP, the random numbers, FFloatsNear, CSolveQuadratic, CalculateSinCos, GModPositive, the
// Hermite curves); this header adds the rest of what physics uses.

#include "xfmath.h"

#include <glm/glm.hpp>

#include <cstdint>

struct CLQ
{
    float u0, u1, u2, uUnused;
};
struct LM
{
    float gMin, gMax;
};

int NRandInRange(int nLow, int nHigh);                                          // 0x11D3F8
void PrescaleClq(const CLQ* pclqSrc, float ru, float du, CLQ* pclqDst);         // 0x11D5B0
void FitClq(float g0, float g1, float u, float gU, CLQ* pclq);                  // 0x11D8D8
float GLimitLm(const LM* plm, float g);                                         // 0x11D948

// orig 0x117120 on 3-vectors (xfmath.h's EvalHermiteVector on vec4)
inline void EvalHermiteVector(const glm::vec3& p0, const glm::vec3& v0, const glm::vec3& p1, const glm::vec3& v1,
                              glm::vec3* ppos, glm::vec3* pvel, glm::vec3* paccel, float dt, float t, float dtScale)
{
    glm::vec4 pos, vel, accel;
    EvalHermiteVector(glm::vec4(p0, 0.0f), glm::vec4(v0, 0.0f), glm::vec4(p1, 0.0f), glm::vec4(v1, 0.0f), ppos ? &pos : nullptr,
                      pvel ? &vel : nullptr, paccel ? &accel : nullptr, dt, t, dtScale);
    if (ppos) *ppos = glm::vec3(pos);
    if (pvel) *pvel = glm::vec3(vel);
    if (paccel) *paccel = glm::vec3(accel);
}

// FFloatsNear on x, y, z in turn (the original's three calls, 1e-4)
inline bool FVecsNear(const glm::vec3& a, const glm::vec3& b)
{
    return FFloatsNear(a.x, b.x, 1e-4f) && FFloatsNear(a.y, b.y, 1e-4f) && FFloatsNear(a.z, b.z, 1e-4f);
}
