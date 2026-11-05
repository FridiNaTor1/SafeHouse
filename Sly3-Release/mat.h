#pragma once

// The matrix module (Sly 1's mat.c; orig 0x11B300-0x11CB50) and the vector helpers it uses (vec.c, 0x11E1A0..).
// Convention (as the rest of Sly2-Release): a glm column is the original's row, so the original's row-vector
// products become glm's M * v, the original's MulMatrix4(r, a, b) is r = a * b, and the original's matrix element
// (row i, column j) at +4 * (4 i + j) is m[i][j].

// The functions the animation port also has (0x11B3D0, 0x11B500, 0x11B570, 0x11B628, 0x11BC00, 0x11BCF8, 0x11BFF0,
// 0x11C318) are xfmath.h's; the Sly 1 names below forward to them.

#include "xfmath.h"

#include <glm/glm.hpp>

// Constants the original keeps in its data (0x2A2D00 identity, 0x2A2DA0.. the axes, 0x2A2DD0 zero).
extern const glm::vec3 g_vecZero;
extern const glm::vec3 g_normalX;
extern const glm::vec3 g_normalY;
extern const glm::vec3 g_normalZ;
extern const glm::mat3 g_matIdentity3;
extern const glm::mat4 g_matIdentity4;

bool FInvertMatrix(int c, const float* aagSrc, float* aagDst, int cStride = -1);  // 0x11C500 (row-major c x c)
bool FInvertMatrix3(const glm::mat3& matSrc, glm::mat3* pmatDst);                 // 0x11B318
glm::mat3 InverseMatrix3(const glm::mat3& mat);                                     // 0x11B340
inline void NormalizeRotateMatrix3(glm::mat3* pmat) { *pmat = NormalizeRotateMatrix3(*pmat); }  // 0x11B3D0
glm::mat3 BuildRotateMatrixSinCos(float gSin, float gCos, const glm::vec3& normal); // 0x11B480
inline glm::mat3 BuildRotateMatrixAxis(float rad, const glm::vec3& normal) { return MatRotateAxis(rad, normal); }  // 0x11B500
inline glm::mat3 BuildRotateMatrixVector(const glm::vec3& vec) { return MatRotateVector(vec); }  // 0x11B570
inline glm::mat3 BuildEulerMatrix(const glm::vec3& eul) { return MatEuler(eul); }  // 0x11B628
glm::mat3 BuildPanTiltMatrix(float radPan, float radTilt);                          // 0x11B720
glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);                                 // 0x11B7E0
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);          // 0x11B830
glm::mat3 LevelMatrix3(const glm::mat3& matSrc, glm::mat3* pmatDst);                // 0x11B938 (returns the rotation)
glm::mat3 BuildRotateVectorsMatrix(const glm::vec3& vecA, const glm::vec3& vecB);   // 0x11BA08
void DecomposeRotateMatrixPanTilt(const glm::mat3& mat, float* pradPan, float* pradTilt);  // 0x11BC88
inline glm::vec3 DecomposeEulerMatrix(const glm::mat3& mat) { return EulerFromMat(mat); }  // 0x11BFF0
glm::mat4 MulMatrix4(const glm::mat4& matA, const glm::mat4& matB);                 // 0x11C168
float GMaxRowLengthMatrix3(const glm::mat3& mat);                                   // 0x11CAB0

glm::vec3 PerpendicularVector(const glm::vec3& vec);                                // 0x11E390
// 0x11EA60: the closest points of the lines a0 + u (a1 - a0) and b0 + v (b1 - b0): u, v and the unit common
// normal (from a towards b); false when they are parallel
bool FClosestPointsLines(const glm::vec3& a0, const glm::vec3& a1, const glm::vec3& b0, const glm::vec3& b1, float* puv,
                         glm::vec3* pnormal);
float RadBetweenVectors(const glm::vec3& vecStart, const glm::vec3& vecFinish, glm::vec3* pnormal);  // 0x11E4B0

// Helpers for the 4x4 records (rows 0..2 the rotation, row 3 the translation).
inline glm::mat3 Mat3Of(const glm::mat4& m) { return glm::mat3(glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2])); }
inline glm::vec3 PosOf(const glm::mat4& m) { return glm::vec3(m[3]); }
inline void SetMat3(glm::mat4* pm, const glm::mat3& r)
{
    (*pm)[0] = glm::vec4(r[0], 0.0f);
    (*pm)[1] = glm::vec4(r[1], 0.0f);
    (*pm)[2] = glm::vec4(r[2], 0.0f);
}
inline void SetPos(glm::mat4* pm, const glm::vec3& pos) { (*pm)[3] = glm::vec4(pos, 1.0f); }
// The original's point / direction transform by a 4x4 (rows: x * r0 + y * r1 + z * r2 (+ r3)).
inline glm::vec3 XformPos(const glm::mat4& m, const glm::vec3& v) { return glm::vec3(m * glm::vec4(v, 1.0f)); }
inline glm::vec3 XformDir(const glm::mat4& m, const glm::vec3& v) { return Mat3Of(m) * v; }
