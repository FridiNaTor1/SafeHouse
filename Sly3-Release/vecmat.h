#pragma once

// Math helpers of Sly 2's util.c / vec.c / mat.c (0x11B480..0x11F288) the camera needs, ported from the
// reference (Sly2-Reference/Sly2-Release/game util.cpp, util_p3.cpp, vec.cpp, mat.cpp, mat_p3.cpp: oracle-verified) with Sly 1
// names. Plain IEEE floats; the original's polynomial sine/cosine (0x11D608) is kept so angles agree closely.
//
// Matrices: glm::mat3 whose columns are the original's rows (as in lo.h), so the original's row-vector
// product v * M is glm's M * v and the original's A * B is MulMat3(A, B) = glm B * A.
// They live in namespace cmm (camera math) so that they do not clash with the physics code's util.h / mat.h ports of
// the same functions (the physics port); when those land the duplicates can go.

#include <bit>
#include <cstdint>
#include <glm/glm.hpp>

namespace cmm
{
// A float constant of the original by its bits (exact).
constexpr float F32(uint32_t u) { return std::bit_cast<float>(u); }

struct SMP  // smoothing: the fast speed, the slow speed, the time from fast to slow (Sly 1's SMP)
{
    float svFast, svSlow, dtFast;
};
struct SMPA  // + an acceleration limit
{
    float svFast, svSlow, dtFast, sdvMax;
};
struct LM  // a range
{
    float gMin, gMax;
};

constexpr float PI = F32(0x40490FDB);

// orig 0x11CF60
float RadNormalize(float rad);
// orig 0x11CFE0
float GLimitAbs(float g, float gLimit);
// orig 0x11D948
float GLimitLm(const LM* plm, float g);
// orig 0x11D978 (an angle clamped to the range around its middle)
float RadLimitLm(const LM* plm, float rad);
// orig 0x11D898
float GModPositive(float gDividend, float gDivisor);
// orig 0x11D508 (the real roots, ascending)
int CSolveQuadratic(float a, float b, float c, float* ax);
// orig 0x11D608 (the polynomial sine; the cosine from it)
void CalculateSinCos(float rad, float* pgSin, float* pgCos);
// orig 0x11D018
float GSmooth(float gCur, float gTarget, float dt, const SMP* psmp, float* pdgNext);
// orig 0x11D1C0
float GSmoothA(float gCur, float dgCur, float gTarget, float dt, const SMPA* psmpa, float* pdgNext);
// orig 0x11D2D0
float RadSmooth(float radCur, float radTarget, float dt, const SMP* psmp, float* pdradNext);
// orig 0x11D200 RadSmoothA
float RadSmoothA(float radCur, float dradCur, float radTarget, float dt, const SMPA* psmpa, float* pdradNext);
// orig 0x11F088
glm::vec3 PosSmooth(const glm::vec3& pos, const glm::vec3& posTarget, float dt, const SMP* psmp, glm::vec3* pv);

// orig 0x11E1A0 ((cos, sin) sXY, sZ)
glm::vec3 SetVectorCylind(float rad, float sXY, float sZ);
// orig 0x11E208 (pan, tilt, length)
glm::vec3 SetVectorSphere(float radPan, float radTilt, float s);
// orig 0x11EFB0 (each output may be null)
void ConvertVectorToSphere(const glm::vec3& v, float* pradPan, float* pradTilt, float* ps);
// orig 0x11EF08 (atan2(y, x) normalized, |xy|, z; each may be null)
void ConvertVectorToCylind(const glm::vec3& v, float* prad, float* pr, float* pz);
// orig 0x11E9F8
glm::vec3 LimitVectorLength(const glm::vec3& v, float gMax);

// The original's A * B (rows).
inline glm::mat3 MulMat3(const glm::mat3& a, const glm::mat3& b) { return b * a; }
// orig 0x11BC00: A^T * B
glm::mat3 MulMat3TransposeA(const glm::mat3& a, const glm::mat3& b);
// orig 0x11B480 / 0x11B500 (about a unit axis)
glm::mat3 BuildRotateMatrixAxis(float rad, const glm::vec3& axis);
// orig 0x11B570 (about the vector's direction by its length; Sly 1's LoadRotateMatrix)
glm::mat3 BuildRotateMatrixVector(const glm::vec3& v);
// orig 0x11B628 (Euler x, y, z; Sly 1's LoadRotateMatrixEuler)
glm::mat3 BuildEulerMatrix(const glm::vec3& eul);
// orig 0x11B720 (pan about z, then tilt about y; Sly 1's LoadRotateMatrixPanTilt)
glm::mat3 BuildPanTiltMatrix(float radPan, float radTilt);
// orig 0x11BC88
void DecomposeRotateMatrixPanTilt(const glm::mat3& m, float* pradPan, float* pradTilt);
// orig 0x11BCF8 (the angle and the axis)
void DecomposeRotateMatrixRad(const glm::mat3& m, float* prad, glm::vec3* pvec);
// orig 0x11BFF0
glm::vec3 DecomposeEulerMatrix(const glm::mat3& m);
// orig 0x11C950 (either output may be null)
void SmoothMatrix(const glm::mat3& matPrev, const glm::mat3& matTarget, const SMP* psmp, float dt, glm::mat3* pmatOut,
                  glm::vec3* pw);
// orig 0x1A37D0 (the curve du + ru / x through (x0, y0), (x1, y1))
void FitRecipFunction(float x0, float y0, float x1, float y1, float* pdu, float* pru);

// A 4x4 matrix in the original's layout: rows r[0..3] (MATRIX4); v * M with v = (x, y, z, w).
struct MATRIX4
{
    glm::vec4 r[4]{};
};
MATRIX4 MulMatrix4(const MATRIX4& a, const MATRIX4& b);       // orig 0x11C168: A * B
MATRIX4 InverseAffineMatrix4(const MATRIX4& m);               // orig 0x11C318
bool FInvertMatrix4(const MATRIX4& m, MATRIX4* pmatOut);       // orig 0x11C2F0 (Gauss-Jordan)
// orig 0x11C448 BuildSimpleProjectionMatrix(rx, ry, dx, dy, sNear, sFar, pmat)
MATRIX4 BuildSimpleProjectionMatrix(float rx, float ry, float dx, float dy, float sNear, float sFar);
}  // namespace cmm
