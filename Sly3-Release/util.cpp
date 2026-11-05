// Math utilities (orig 0x11CF60..0x11E000, Sly 1's util.c) and the Hermite curves (0x117030 / 0x117120).
// Ported from the reference's util.cpp / m_117030.cpp (oracle-verified), arithmetic order kept.

#include "util.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace
{
constexpr float F(uint32_t u) { return std::bit_cast<float>(u); }
constexpr float PI = F(0x40490FDB);  // 0x40490FDB
inline float Div(float a, float b) { return b == 0.0f ? (std::signbit(a) != std::signbit(b) ? -3.4028235e38f : 3.4028235e38f) : a / b; }
}







// orig: 0x0011D3F8 NRandInRange (into [nLow, nHigh])
int NRandInRange(int nLow, int nHigh)
{
    const int n = NRandom();
    const int d = nHigh - (nLow - 1);
    return d ? nLow + n % d : nLow;
}




// orig: 0x0011D5B0 PrescaleClq__FP3CLQffT0 (the quadratic of g(ru t + du))
void PrescaleClq(const CLQ* s, float ru, float du, CLQ* d)
{
    const CLQ c = *s;
    d->u2 = (c.u2 * ru) * ru;
    d->u1 = (((c.u2 + c.u2) * ru) * du) + c.u1 * ru;
    d->u0 = ((c.u2 * du) * du + c.u1 * du) + c.u0;
}



// orig: 0x0011D8D8 FitClq__FffffP3CLQ (the quadratic through (0, g0), (1, g1), (u, gU))
void FitClq(float g0, float g1, float u, float gU, CLQ* pclq)
{
    const float d1 = g1 - g0;
    pclq->u0 = g0;
    const float u2 = Div(Div(gU - g0, u) - d1, u - 1.0f);
    pclq->u2 = u2;
    pclq->u1 = d1 - u2;
}

// orig: 0x0011D948 GLimitLm__FP2LMf
float GLimitLm(const LM* plm, float g)
{
    if (g < plm->gMin) return plm->gMin;
    if (plm->gMax < g) return plm->gMax;
    return g;
}


