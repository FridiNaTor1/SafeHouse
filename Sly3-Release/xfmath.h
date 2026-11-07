#pragma once

// The game's own math helpers that animation and the transform records need (Sly 1's mat.c / util.c names; Sly 2
// addresses). Plain IEEE floats on the host. The original's MATRIX3/MATRIX4 are row-vector matrices (v' = v M); here
// they are glm matrices whose columns are the original's rows (as everywhere in Sly2-Release), so the original's
// product MulMatrix4(ret, A, B) (rows of B transformed by A) is glm A * B.

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

// The EE FPU's rounding (toward zero) for the few sums where it decides game logic (an ASEGA's time against an
// event exactly at a frame boundary): the exact result rounded toward zero.
float MulRz(float a, float b);
float AddRz(float a, float b);

// The EE FPU's division (div.s): a zero divisor gives +-FLT_MAX (the signs' exclusive or; 0 / 0 too) instead of an
// IEEE infinity or NaN, and a result never passes FLT_MAX. For the divisions whose divisor can be zero in game data
// (a speed scale of 0, a braking rate of 0), so that they come out as on the PS2 instead of poisoning a record with NaN.
inline float DivEe(float a, float b)
{
    const bool fNeg = std::signbit(a) != std::signbit(b);
    if (b == 0.0f || std::isinf(a)) return fNeg ? -3.40282347e38f : 3.40282347e38f;
    const float q = a / b;
    if (std::isnan(q)) return 0.0f;   // (inf / inf: no infinities on the EE; not reached)
    return std::isinf(q) ? (fNeg ? -3.40282347e38f : 3.40282347e38f) : q;
}

// The EE FPU's mul.s / add.s: a result past FLT_MAX saturates to +-FLT_MAX (no infinities)
inline float SatEe(float g) { return std::isinf(g) ? (g < 0.0f ? -3.40282347e38f : 3.40282347e38f) : g; }
inline float MulEe(float a, float b) { return SatEe(a * b); }
inline float AddEe(float a, float b) { return SatEe(a + b); }

// orig: 0x0011D608 CalculateSinCos (the game's polynomial sine, cosine from it)
void CalculateSinCos(float rad, float* pgSin, float* pgCos);
// orig: 0x0011CF60 RadNormalize (into [-pi, pi])
float RadNormalize(float rad);
// orig: 0x0011D898 GModPositive
float GModPositive(float gDividend, float gDivisor);
// orig: 0x0011CFE0 GLimitAbs
float GLimitAbs(float g, float gLimit);
// orig: 0x0011D4D0 FFloatsNear
bool FFloatsNear(float g1, float g2, float gEpsilon);
// orig: 0x0011D508 CSolveQuadratic (ascending roots into ax)
int CSolveQuadratic(float a, float b, float c, float* ax);

// The game's random numbers: the C library's rand (0x2342A0: newlib's LCG, state at *(0x2C49D4) + 0x58), NRandom
// 0x11D3C0 (below 905010707), GRandInRange 0x11D448. TODO(world): one shared state for every system.
extern uint32_t g_uRandState;
extern uint64_t g_cRandLibc;   // (debug: the draws so far)
int RandLibc();
int NRandom();
float GRandInRange(float gLow, float gHigh);

// orig: 0x0011D018 GSmooth (SMP: the fast speed, the slow speed, the slow-down time)
struct SMP
{
    float svFast;
    float svSlow;
    float dtFast;
};
float GSmooth(float gCur, float gTarget, float dt, const SMP* psmp, float* pdgNext);

// orig: 0x00117030 EvalHermite (the value / derivative (times dtScale) at t of [0, dt] of the curve g0 (slope dg0)
// to g1 (slope dg1)); either pointer may be null
void EvalHermite(float* pg, float* pdg, float dt, float t, float dtScale, float g0, float dg0, float g1, float dg1);
// orig: 0x00117120 the vector curve p0 (velocity v0) to p1 (v1): position, velocity (x dtScale), acceleration
void EvalHermiteVector(const glm::vec4& p0, const glm::vec4& v0, const glm::vec4& p1, const glm::vec4& v1,
                       glm::vec4* ppos, glm::vec4* pvel, glm::vec4* paccel, float dt, float t, float dtScale);

// Rotations (3x3, glm columns = the original's rows).
// orig: 0x0011B480 BuildRotateMatrixSinCos / 0x0011B500 BuildRotateMatrixAxis (about a unit axis)
glm::mat3 MatRotateAxis(float rad, const glm::vec3& axis);
// orig: 0x0011B570 BuildRotateMatrixVector (about the vector's direction by its length)
glm::mat3 MatRotateVector(const glm::vec3& v);
// orig: 0x0011B628 BuildEulerMatrix (Euler angles x, y, z)
glm::mat3 MatEuler(const glm::vec3& e);
// orig: 0x0011BFF0 DecomposeEulerMatrix
glm::vec3 EulerFromMat(const glm::mat3& m);
// orig: 0x0011B3D0 NormalizeRotateMatrix3 (re-orthonormalized from its rows 0 and 1)
glm::mat3 NormalizeRotateMatrix3(const glm::mat3& m);
// orig: 0x0011BCF8 DecomposeRotateMatrixRad (the rotation's angle and unit axis)
void DecomposeRotateMatrixRad(const glm::mat3& m, float* prad, glm::vec3* paxis);
// orig: 0x0011BC00 MulMatrix3TransposeA (row i of the result = sum over k of A[k][i] B[k]: the original's A^T B)
glm::mat3 MulMatrix3TransposeA(const glm::mat3& a, const glm::mat3& b);
// orig: 0x0011C318 InverseAffineMatrix4
glm::mat4 InverseAffineMatrix4(const glm::mat4& m);

// The original's row access on the glm matrices (row i of the original = glm column i).
inline glm::vec3 Row3(const glm::mat3& m, int i) { return m[i]; }
inline glm::vec3 Row3(const glm::mat4& m, int i) { return glm::vec3(m[i]); }
