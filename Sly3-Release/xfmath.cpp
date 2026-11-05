// The game's math helpers used by animation and the transform records (Sly 1's mat.c / util.c, Sly 2 addresses;
// specification: Sly2-Reference/Sly2-Release/game/mat.cpp, util.cpp, m_117030.cpp, all oracle-verified).

#include "xfmath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
float U2F(uint32_t u)
{
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}
constexpr float kPi = 3.14159274f;       // 0x40490FDB
constexpr float kHalfPi = 1.57079637f;   // 0x3FC90FDB
constexpr float kTwoPi = 6.28318548f;    // 0x40C90FDB
constexpr float kEpsilon = 1e-4f;        // 0x38D1B717
}

uint32_t g_uRandState = 1;

namespace
{
float RoundTowardZero(double d)
{
    float f = static_cast<float>(d);
    if (std::fabs(static_cast<double>(f)) > std::fabs(d)) f = std::nextafter(f, 0.0f);
    return f;
}
}

float MulRz(float a, float b) { return RoundTowardZero(static_cast<double>(a) * static_cast<double>(b)); }
float AddRz(float a, float b) { return RoundTowardZero(static_cast<double>(a) + static_cast<double>(b)); }

// orig: 0x002342A0 rand
uint64_t g_cRandLibc = 0;  // (debug: the draws so far)
int RandLibc()
{
    ++g_cRandLibc;
    g_uRandState = g_uRandState * 0x41C64E6Du + 0x3039u;
    return static_cast<int>(g_uRandState & 0x7FFFFFFFu);
}

// orig: 0x0011D3C0
int NRandom() { return RandLibc() % 0x35F15E13; }

// orig: 0x0011D448
float GRandInRange(float gLow, float gHigh)
{
    if (gLow == gHigh) return gLow;
    const int n = NRandom();
    const float d = gHigh - gLow;
    const float r = static_cast<float>(n % 0x16A1B);
    return gLow + (d * r) * U2F(0x3734FC67u);  // (/ 92699)
}

// orig: 0x0011D608 CalculateSinCos: the angle folded into [-pi/2, pi/2], a polynomial sine, the cosine from it
void CalculateSinCos(float rad, float* pgSin, float* pgCos)
{
    const float g = (rad + kHalfPi) * U2F(0x3EA2F983u);  // (rad + pi/2) / pi
    int n = static_cast<int>(g);                     // cvt.w.s truncates
    if (g < 0.0f) n -= 1;
    const float gN = static_cast<float>(n) * kPi;
    const int fOdd = n & 1;
    float x = rad - gN;
    if (fOdd) x = -x;
    const float x2 = x * x;
    float s = x;
    float p = s * x2;
    const float t3 = p * U2F(0xBE2AAAA4u);
    p = p * x2;
    s = s + t3;
    const float t5 = p * U2F(0x3C08873Eu);
    p = p * x2;
    s = s + t5;
    const float t7 = p * U2F(0xB94FB21Fu);
    p = p * x2;
    s = s + t7;
    const float t9 = p * U2F(0x362E9C14u);
    s = s + t9;
    if (s < -1.0f) s = -1.0f;
    else if (1.0f < s) s = 1.0f;
    float c = std::sqrt(1.0f - s * s);
    if (fOdd) c = -c;
    if (pgCos) *pgCos = c;
    if (pgSin) *pgSin = s;
}

// orig: 0x0011D898 GModPositive
float GModPositive(float gDividend, float gDivisor)
{
    float g = std::fmod(gDividend, gDivisor);
    if (g < 0.0f) g = g + gDivisor;
    return g;
}

// orig: 0x0011CF60 RadNormalize
float RadNormalize(float rad)
{
    if (rad < -kPi || kPi < rad) rad = GModPositive(rad + kPi, kTwoPi) - kPi;
    return rad;
}

// orig: 0x0011CFE0 GLimitAbs
float GLimitAbs(float g, float gLimit)
{
    if (gLimit < g) return gLimit;
    if (g < -gLimit) g = -gLimit;
    return g;
}

// orig: 0x0011D4D0 FFloatsNear: |g1 - g2| / max(1, |g1|) < gEpsilon
bool FFloatsNear(float g1, float g2, float gEpsilon)
{
    const float d = std::fabs(g1 - g2);
    const float a = std::fabs(g1);
    const float m = a <= 1.0f ? 1.0f : a;
    return d / m < gEpsilon;
}

// orig: 0x0011D508 CSolveQuadratic
int CSolveQuadratic(float a, float b, float c, float* ax)
{
    const float d = b * b - a * (c * 4.0f);
    if (d < 0.0f) return 0;
    const float a2 = a + a;
    const float alpha = std::sqrt(d) / a2;
    const float nb = -(b / a2);
    if (std::fabs(alpha) < kEpsilon)
    {
        ax[0] = nb;
        return 1;
    }
    const float x1 = nb - alpha, x0 = nb + alpha;
    ax[1] = x1;
    ax[0] = x0;
    if (x1 < x0)
    {
        ax[1] = x0;
        ax[0] = x1;
    }
    return 2;
}

// orig: 0x0011D018 GSmooth: gCur moved toward gTarget over dt at most at the fast speed, slowing down to the slow one
float GSmooth(float gCur, float gTarget, float dt, const SMP* psmp, float* pdgNext)
{
    const float svFast = psmp->svFast;
    if (svFast == 0.0f)
    {
        if (pdgNext) *pdgNext = 0.0f;
        return gCur;
    }
    float dg = gCur - gTarget;
    bool fNeg = false;
    if (dg < 0.0f)
    {
        dg = -dg;
        fNeg = true;
    }
    const float dtFast = psmp->dtFast;
    const float dgSlow = ((psmp->svSlow + svFast) * dtFast) * 0.5f;
    float g = 0.0f, dgNext = 0.0f;
    bool fDone = false;
    if (dgSlow <= dg)
    {
        const float t = (dg - dgSlow) / svFast;
        if (dt <= t)
        {
            g = dg - svFast * dt;
            dgNext = -svFast;
            fDone = true;
        }
        else
        {
            dt = dt - t;
            dg = dgSlow;
        }
    }
    if (!fDone)
    {
        const float svSlow = psmp->svSlow;
        const float a = (psmp->svFast - svSlow) / (dtFast + dtFast);
        float ax[2] = {0.0f, 0.0f};
        const int c = CSolveQuadratic(a, svSlow, -dg, ax);
        const float dtLeft = (c > 0 ? ax[c - 1] : 0.0f) - dt;
        if (0.0f < dtLeft)
        {
            const float g1 = psmp->svSlow, g0 = psmp->svFast, g2 = psmp->dtFast;
            const float f4 = g1 * dtLeft;
            float f0 = (g0 - g1) * dtLeft;
            const float f3 = g2 + g2;
            const float f1 = f0 / g2;
            f0 = f0 * dtLeft;
            f0 = f0 / f3;
            dgNext = -(g1 + f1);
            g = f0 + f4;
        }
        else
        {
            g = 0.0f;
            dgNext = -svSlow;
        }
    }
    if (fNeg)
    {
        g = gTarget - g;
        if (pdgNext) *pdgNext = -dgNext;
    }
    else
    {
        g = g + gTarget;
        if (pdgNext) *pdgNext = dgNext;
    }
    return g;
}

// orig: 0x00117030 EvalHermite
void EvalHermite(float* pg, float* pdg, float dt, float t, float dtScale, float g0, float dg0, float g1, float dg1)
{
    const float u = t / dt;
    const float v = 1.0f - u;
    if (pg)
    {
        const float uv2 = u * v * v;
        const float v3 = v * v * v;
        const float u2v = u * u * v;
        const float u3 = u * u * u;
        float g = (v3 + uv2 * 3.0f) * g0;
        g = g + (uv2 * dt) * dg0;
        g = g - (u2v * dt) * dg1;
        *pg = g + (u3 + u2v * 3.0f) * g1;
    }
    if (pdg)
    {
        const float a = (u * (v * 6.0f)) / dt * (g1 - g0);
        const float b = (1.0f - u * (4.0f - u * 3.0f)) * dg0;
        const float c = (u * (u * 3.0f - 2.0f)) * dg1;
        *pdg = ((a + b) + c) * dtScale;
    }
}

// orig: 0x00117120 the vector curve (VU0)
void EvalHermiteVector(const glm::vec4& p0, const glm::vec4& v0, const glm::vec4& p1, const glm::vec4& v1,
                       glm::vec4* ppos, glm::vec4* pvel, glm::vec4* paccel, float dt, float t, float dtScale)
{
    const float u = t / dt;
    const float w = 1.0f - u;
    if (ppos)
    {
        const float uw = u * w * w;
        const float u2w = u * u * w;
        const float a = w * w * w + uw * 3.0f;
        const float b = uw * dt;
        const float c = -u2w * dt;
        const float d = u * u * u + u2w * 3.0f;
        *ppos = p0 * a + v0 * b + v1 * c + p1 * d;
    }
    if (pvel)
    {
        const glm::vec4 dp = p1 - p0;
        const float a = (u * (6.0f - u * 6.0f)) / dt;
        const float b = 1.0f - u * (4.0f - u * 3.0f);
        const float c = u * (u * 3.0f + -2.0f);
        *pvel = (dp * a + v0 * b + v1 * c) * dtScale;
    }
    if (paccel)
    {
        const glm::vec4 dp = p1 - p0;
        const float s2 = (dtScale * dtScale) / dt;
        const float a = (6.0f - u * 12.0f) / dt;
        const float b = u * 6.0f - 4.0f;
        const float c = u * 6.0f - 2.0f;
        *paccel = (dp * a + v0 * b + v1 * c) * s2;
    }
}

// orig: 0x0011B500 (and 0x0011B480 with the game's sine / cosine)
glm::mat3 MatRotateAxis(float rad, const glm::vec3& n)
{
    float s, c;
    CalculateSinCos(rad, &s, &c);
    const float k = 1.0f - c;
    glm::mat3 m;
    m[0] = glm::vec3(k * n.x * n.x + c, k * n.y * n.x + s * n.z, k * n.z * n.x - s * n.y);
    m[1] = glm::vec3(k * n.x * n.y - s * n.z, k * n.y * n.y + c, k * n.z * n.y + s * n.x);
    m[2] = glm::vec3(k * n.x * n.z + s * n.y, k * n.y * n.z - s * n.x, k * n.z * n.z + c);
    return m;
}

// orig: 0x0011B570
glm::mat3 MatRotateVector(const glm::vec3& v)
{
    const float g = std::sqrt(glm::dot(v, v));
    if (g < kEpsilon) return glm::mat3(1.0f);
    return MatRotateAxis(g, v * (1.0f / g));
}

// orig: 0x0011B628 BuildEulerMatrix
glm::mat3 MatEuler(const glm::vec3& e)
{
    float sx, cx, sy, cy, sz, cz;
    CalculateSinCos(e.x, &sx, &cx);
    CalculateSinCos(e.y, &sy, &cy);
    CalculateSinCos(e.z, &sz, &cz);
    const float sxsy = sx * sy, cxsy = cx * sy;
    glm::mat3 m;
    m[0] = glm::vec3(cy * cz, cy * sz, -sy);
    m[1] = glm::vec3(sxsy * cz - cx * sz, sxsy * sz + cx * cz, sx * cy);
    m[2] = glm::vec3(cxsy * cz + sx * sz, cxsy * sz - sx * cz, cx * cy);
    return m;
}

// orig: 0x0011BFF0 DecomposeEulerMatrix (at the poles x is 0)
glm::vec3 EulerFromMat(const glm::mat3& m)
{
    const float ry = std::asin(GLimitAbs(-m[0][2], 1.0f));
    float rx, rz;
    if (ry < U2F(0x3FC90C94u))
    {
        if (U2F(0xBFC90C94u) < ry)
        {
            rx = std::atan2(m[1][2], m[2][2]);
            rz = std::atan2(m[0][1], m[0][0]);
        }
        else
        {
            rz = std::atan2(-m[1][0], -m[2][0]);
            rx = 0.0f;
        }
    }
    else
    {
        rz = std::atan2(-m[1][0], m[2][0]);
        rx = 0.0f;
    }
    return glm::vec3(rx, ry, rz);
}

// orig: 0x0011B3D0
glm::mat3 NormalizeRotateMatrix3(const glm::mat3& m)
{
    const glm::vec3 r0 = glm::normalize(m[0]);
    const glm::vec3 r2 = glm::normalize(glm::cross(r0, m[1]));
    const glm::vec3 r1 = glm::normalize(glm::cross(r2, r0));
    glm::mat3 o;
    o[0] = r0;
    o[1] = r1;
    o[2] = r2;
    return o;
}

// orig: 0x0011BCF8 DecomposeRotateMatrixRad
void DecomposeRotateMatrixRad(const glm::mat3& m, float* prad, glm::vec3* pvec)
{
    const float c = (((m[0][0] + m[1][1]) + m[2][2]) - 1.0f) * 0.5f;
    if (U2F(0x3F7FFFEFu) < c)  // no rotation
    {
        *prad = 0.0f;
        *pvec = glm::vec3(1.0f, 0.0f, 0.0f);
        return;
    }
    float gRest = 1.0f;
    const float d = 1.0f - c;
    glm::vec3 v;
    for (int n = 0; n < 2; ++n)
    {
        const float g = (m[n][n] - c) / d;
        float a = std::fabs(g);
        if (gRest < a) a = gRest;
        else if (a < 0.0f) a = 0.0f;
        v[n] = std::sqrt(a);
        gRest = gRest - a;
    }
    v.z = std::sqrt(gRest);
    if (c < U2F(0xBF7FF972u))  // a half turn
    {
        *prad = kPi;
        if (v.y < v.x)
        {
            if (v.z < v.x)
            {
                if (m[0][1] < 0.0f) v.y = -v.y;
                if (m[0][2] < 0.0f) v.z = -v.z;
                *pvec = v;
                return;
            }
        }
        else if (v.z < v.y)
        {
            if (m[1][0] < 0.0f) v.x = -v.x;
            if (m[1][2] < 0.0f) v.z = -v.z;
            *pvec = v;
            return;
        }
        if (m[2][0] < 0.0f) v.x = -v.x;
        if (m[2][1] < 0.0f) v.y = -v.y;
        *pvec = v;
        return;
    }
    *prad = std::acos(c);
    if (m[0][1] < m[1][0]) v.z = -v.z;
    if (m[2][0] < m[0][2]) v.y = -v.y;
    if (m[1][2] < m[2][1]) v.x = -v.x;
    *pvec = v;
}

// orig: 0x0011BC00
glm::mat3 MulMatrix3TransposeA(const glm::mat3& a, const glm::mat3& b)
{
    glm::mat3 o;
    for (int i = 0; i < 3; ++i) o[i] = (b[0] * a[0][i] + b[1] * a[1][i]) + b[2] * a[2][i];
    return o;
}

// orig: 0x0011C318
glm::mat4 InverseAffineMatrix4(const glm::mat4& m)
{
    const glm::mat3 r(m);
    const glm::mat3 ri = glm::inverse(r);
    glm::mat4 o(ri);
    o[3] = glm::vec4(-(ri * glm::vec3(m[3])), 1.0f);
    return o;
}
