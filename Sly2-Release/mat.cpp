// The matrix module (orig 0x11B300-0x11CB50, Sly 1's mat.c) and vector helpers (0x11E390, 0x11E4B0).
// Ported from the reference's mat.cpp / vec.cpp (oracle-verified ports of the original's VU0 code); the order of
// the arithmetic is kept, the VU0 register leftovers the reference models are not (no other native code reads them).

#include "mat.h"

#include "util.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

const glm::vec3 g_vecZero(0.0f);
const glm::vec3 g_normalX(1.0f, 0.0f, 0.0f);
const glm::vec3 g_normalY(0.0f, 1.0f, 0.0f);
const glm::vec3 g_normalZ(0.0f, 0.0f, 1.0f);
const glm::mat3 g_matIdentity3(1.0f);
const glm::mat4 g_matIdentity4(1.0f);

namespace
{
constexpr float F(uint32_t u) { return std::bit_cast<float>(u); }
constexpr float EPSILON = 1e-4f;  // 0x38D1B717
inline float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
inline float Len3(const glm::vec3& a) { return std::sqrt(std::fabs(Dot3(a, a))); }
inline float Div(float a, float b) { return b == 0.0f ? (std::signbit(a) != std::signbit(b) ? -3.4028235e38f : 3.4028235e38f) : a / b; }
}

// orig: 0x0011C500 FInvertMatrix__FiPfT1 (Gauss-Jordan by column operations with column pivoting on a copy;
// false when a pivot is below 1e-4)
bool FInvertMatrix(int c, const float* aagSrc, float* aagDst, int cStride)
{
    if (cStride < 0) cStride = c;
    std::vector<float> ag(aagSrc, aagSrc + c * cStride);
    auto src = [&](int i) -> float& { return ag[i]; };
    auto dst = [&](int i) -> float& { return aagDst[i]; };
    for (int i = 0; i < c; i++)
        for (int j = 0; j < c; j++) dst(i * cStride + j) = (i == j) ? 1.0f : 0.0f;
    for (int i = 0; i < c; i++)
    {
        const int iRow = i * cStride;
        int iPivot = i;
        float gMax = std::fabs(src(iRow + i));
        for (int k = i + 1; k < c; k++)
        {
            const float g = std::fabs(src(iRow + k));
            if (gMax < g) { gMax = g; iPivot = k; }
        }
        if (gMax < EPSILON) return false;
        if (iPivot != i)
            for (int n = 0; n < c; n++)
            {
                std::swap(src(n * cStride + iPivot), src(n * cStride + i));
                std::swap(dst(n * cStride + iPivot), dst(n * cStride + i));
            }
        for (int k = i + 1; k < c; k++)
        {
            const float g = Div(src(iRow + k), src(iRow + i));
            for (int n = 0; n < c; n++)
            {
                src(n * cStride + k) = src(n * cStride + k) - g * src(n * cStride + i);
                dst(n * cStride + k) = dst(n * cStride + k) - g * dst(n * cStride + i);
            }
        }
    }
    for (int i = c - 1; i >= 0; i--)
    {
        const int iRow = i * cStride;
        for (int k = 0; k < i; k++)
        {
            const float g = Div(src(iRow + k), src(iRow + i));
            for (int n = 0; n < c; n++)
            {
                src(n * cStride + k) = src(n * cStride + k) - g * src(n * cStride + i);
                dst(n * cStride + k) = dst(n * cStride + k) - g * dst(n * cStride + i);
            }
        }
    }
    for (int i = 0; i < c; i++)
    {
        const float gDiag = src(i * cStride + i);
        for (int n = 0; n < c; n++) dst(n * cStride + i) = Div(dst(n * cStride + i), gDiag);
    }
    return true;
}

// orig: 0x0011B318 FInvertMatrix3 (FInvertMatrix on the rows, stride 4)
bool FInvertMatrix3(const glm::mat3& matSrc, glm::mat3* pmatDst)
{
    float a[9], b[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) a[i * 3 + j] = matSrc[i][j];
    if (!FInvertMatrix(3, a, b)) return false;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) (*pmatDst)[i][j] = b[i * 3 + j];
    return true;
}

// orig: 0x0011B340 (InverseMatrix3: the cofactors over the determinant; the original's 1/det is VU0's DIV, which
// gives +-max for 0)
glm::mat3 InverseMatrix3(const glm::mat3& m)
{
    const glm::vec3 r0 = m[0], r1 = m[1], r2 = m[2];
    const glm::vec3 c0 = glm::cross(r1, r2), c1 = glm::cross(r2, r0), c2 = glm::cross(r0, r1);
    const float det = Dot3(r0, c0);
    const float q = Div(1.0f, det);
    glm::mat3 r;
    r[0] = glm::vec3(c0.x, c1.x, c2.x) * q;
    r[1] = glm::vec3(c0.y, c1.y, c2.y) * q;
    r[2] = glm::vec3(c0.z, c1.z, c2.z) * q;
    return r;
}


// orig: 0x0011B480 (the rotation of an angle given by its sine and cosine about a unit axis)
glm::mat3 BuildRotateMatrixSinCos(float s, float c, const glm::vec3& n)
{
    const float t = 1.0f - c;
    const glm::vec3 nt = n * t, ns = n * s;
    glm::mat3 m;
    m[0] = nt * n.x;
    m[1] = nt * n.y;
    m[2] = nt * n.z;
    m[0].x += c;
    m[1].y += c;
    m[2].z += c;
    m[0].y += ns.z;
    m[1].x -= ns.z;
    m[0].z -= ns.y;
    m[2].x += ns.y;
    m[1].z += ns.x;
    m[2].y -= ns.x;
    return m;
}




// orig: 0x0011B720 (pan about z, then tilt about y: rows of the tilt combined by the pan's)
glm::mat3 BuildPanTiltMatrix(float radPan, float radTilt)
{
    const glm::mat3 mPan = BuildRotateMatrixAxis(radPan, g_normalZ);
    const glm::mat3 mTilt = BuildRotateMatrixAxis(-radTilt, g_normalY);
    return mPan * mTilt;
}

// orig: 0x0011B7E0
glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec) { return BuildLookAtMatrix(vec, g_normalZ); }

// orig: 0x0011B830 (x along vecX (the x axis when too short), y = vecY x x normalized (a perpendicular when
// degenerate), z = x x y)
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY)
{
    const float g = Len3(vecX);
    const glm::vec3 x = (g < EPSILON) ? g_normalX : vecX * Div(1.0f, g);
    glm::vec3 y = glm::cross(vecY, x);
    const float l = Len3(y);
    y = y * Div(1.0f, l);
    if (l < EPSILON) y = PerpendicularVector(x);
    glm::mat3 m;
    m[0] = x;
    m[1] = y;
    m[2] = glm::cross(x, y);
    return m;
}

// orig: 0x0011B938 (the rotation taking the frame's z row to z; the frame rotated by it into *pmatDst)
glm::mat3 LevelMatrix3(const glm::mat3& matSrc, glm::mat3* pmatDst)
{
    const glm::mat3 rot = BuildRotateVectorsMatrix(matSrc[2], g_normalZ);
    if (pmatDst)
    {
        glm::mat3 r;
        r[2] = rot * matSrc[2];
        r[0] = rot * matSrc[0];
        r[1] = rot * matSrc[1];
        *pmatDst = r;
    }
    return rot;
}

// orig: 0x0011BA08 (the rotation taking direction a to direction b; the identity when either is too short)
glm::mat3 BuildRotateVectorsMatrix(const glm::vec3& a, const glm::vec3& b)
{
    const float la = Len3(a), lb = Len3(b);
    if (la < EPSILON || lb < EPSILON) return g_matIdentity3;
    glm::vec3 n = glm::cross(a, b);
    const float lc = Len3(n);
    if (F(0x322BCC77) < lc)  // 0x322BCC77
        n = n * Div(1.0f, lc);
    else
        n = PerpendicularVector(a);
    const float d = Dot3(a, b);
    const float c = GLimitAbs(d / (la * lb), 1.0f);
    const float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
    return BuildRotateMatrixSinCos(s, c, n);
}


// orig: 0x0011BC88 DecomposeRotateMatrixPanTilt__FP7MATRIX3PfT1
void DecomposeRotateMatrixPanTilt(const glm::mat3& m, float* pradPan, float* pradTilt)
{
    *pradPan = std::atan2(m[0][1], m[0][0]);
    const float r = std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1]);
    *pradTilt = std::atan2(m[0][2], r);
}



// orig: 0x0011C168 (VU0 micro program 0xC: each row of B combined by A's rows)
glm::mat4 MulMatrix4(const glm::mat4& a, const glm::mat4& b) { return a * b; }


// orig: 0x0011CAB0 (the largest row length)
float GMaxRowLengthMatrix3(const glm::mat3& m)
{
    const float d0 = Dot3(m[0], m[0]), d1 = Dot3(m[1], m[1]), d2 = Dot3(m[2], m[2]);
    float g;
    if (d1 < d0 && d2 < d0) g = d0;
    else if (d2 < d1) g = d1;
    else g = d2;
    return std::sqrt(g);
}

// orig: 0x0011E390 (vec crossed with the axis-aligned vector made from its smallest component, normalized)
glm::vec3 PerpendicularVector(const glm::vec3& v)
{
    const float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
    glm::vec3 p;
    if (ax <= ay)
        p = (ax <= az) ? glm::vec3(0.0f, v.z, -v.y) : glm::vec3(v.y, -v.x, 0.0f);
    else
        p = (ay <= az) ? glm::vec3(v.z, 0.0f, -v.x) : glm::vec3(v.y, -v.x, 0.0f);
    const glm::vec3 c = glm::cross(v, p);
    return c * Div(1.0f, Len3(c));
}

// orig: 0x0011E4B0 RadBetweenVectors (the angle, and the unit axis of the rotation from A to B; x when either is
// too short). Native: the cosine is clamped to [-1, 1] (the PS2's acosf of a rounding overshoot).
float RadBetweenVectors(const glm::vec3& a, const glm::vec3& b, glm::vec3* pnormal)
{
    const float la = Len3(a), lb = Len3(b);
    if (la < EPSILON || lb < EPSILON)
    {
        if (pnormal) *pnormal = g_normalX;
        return 0.0f;
    }
    if (pnormal)
    {
        glm::vec3 c = glm::cross(a, b);
        const float lc = Len3(c);
        *pnormal = (2.0e-8f < lc) ? c * Div(1.0f, lc) : PerpendicularVector(a);
    }
    return std::acos(std::clamp(Div(Dot3(a, b), la * lb), -1.0f, 1.0f));
}

// orig: 0x0011EA60 func_0011EA60 (the closest points of two lines: u on a, v on b, the unit normal of both (towards
// b); false below 1e-6)
bool FClosestPointsLines(const glm::vec3& a0, const glm::vec3& a1, const glm::vec3& b0, const glm::vec3& b1, float* puv,
                         glm::vec3* pnormal)
{
    const glm::vec3 v7 = b0 - a0, v3 = a1 - a0, v4 = b1 - b0;
    const glm::vec3 v9 = glm::cross(v3, v4);
    const glm::vec3 v8 = glm::cross(v7, v3);
    const float d = Dot3(v9, v9);
    const glm::vec3 v2 = glm::cross(v7, v4);
    if (d < F(0x358637BD)) return false;  // 1e-6
    if (puv)
    {
        puv[0] = Div(Dot3(v2, v9), d);
        puv[1] = Div(Dot3(v8, v9), d);
    }
    const float gSign = Dot3(v7, v9) < 0.0f ? -1.0f : 1.0f;
    if (pnormal) *pnormal = v9 * Div(gSign, std::sqrt(d));
    return true;
}
