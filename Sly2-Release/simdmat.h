#pragma once

// (perf-2) glm's 4x4 products with SSE, lane for lane the same IEEE operations in the same order as glm's own
// (scalar here: no GLM_FORCE_INTRINSICS), so bit for bit the same results; the build has no FMA contraction
// (-ffp-contract=off; SSE2 has no FMA). On the Switch the same with NEON. Elsewhere: glm itself.

#include <glm/glm.hpp>

#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>

// glm operator*(mat4, mat4): r[c] = ((a[0] * b[c][0] + a[1] * b[c][1]) + a[2] * b[c][2]) + a[3] * b[c][3]
inline glm::mat4 MulMat4(const glm::mat4& a, const glm::mat4& b)
{
    const __m128 a0 = _mm_loadu_ps(&a[0][0]), a1 = _mm_loadu_ps(&a[1][0]), a2 = _mm_loadu_ps(&a[2][0]), a3 = _mm_loadu_ps(&a[3][0]);
    glm::mat4 r;
    for (int c = 0; c < 4; ++c)
    {
        __m128 s = _mm_add_ps(_mm_mul_ps(a0, _mm_set1_ps(b[c][0])), _mm_mul_ps(a1, _mm_set1_ps(b[c][1])));
        s = _mm_add_ps(s, _mm_mul_ps(a2, _mm_set1_ps(b[c][2])));
        s = _mm_add_ps(s, _mm_mul_ps(a3, _mm_set1_ps(b[c][3])));
        _mm_storeu_ps(&r[c][0], s);
    }
    return r;
}

// glm operator*(mat4, vec4): (m[0] * v.x + m[1] * v.y) + (m[2] * v.z + m[3] * v.w)
inline glm::vec4 MulMat4Vec4(const glm::mat4& m, const glm::vec4& v)
{
    const __m128 a0 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(&m[0][0]), _mm_set1_ps(v.x)), _mm_mul_ps(_mm_loadu_ps(&m[1][0]), _mm_set1_ps(v.y)));
    const __m128 a1 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(&m[2][0]), _mm_set1_ps(v.z)), _mm_mul_ps(_mm_loadu_ps(&m[3][0]), _mm_set1_ps(v.w)));
    glm::vec4 r;
    _mm_storeu_ps(&r[0], _mm_add_ps(a0, a1));
    return r;
}
#elif defined(__SWITCH__)
#include <arm_neon.h>

// (the Switch) the same products on NEON, the same operations in the same order (-ffp-contract=off: no fused
// multiply-adds; a lane multiply is the plain multiply by that element)
inline glm::mat4 MulMat4(const glm::mat4& a, const glm::mat4& b)
{
    const float32x4_t a0 = vld1q_f32(&a[0][0]), a1 = vld1q_f32(&a[1][0]), a2 = vld1q_f32(&a[2][0]), a3 = vld1q_f32(&a[3][0]);
    glm::mat4 r;
    for (int c = 0; c < 4; ++c)
    {
        const float32x4_t bc = vld1q_f32(&b[c][0]);
        float32x4_t s = vaddq_f32(vmulq_laneq_f32(a0, bc, 0), vmulq_laneq_f32(a1, bc, 1));
        s = vaddq_f32(s, vmulq_laneq_f32(a2, bc, 2));
        s = vaddq_f32(s, vmulq_laneq_f32(a3, bc, 3));
        vst1q_f32(&r[c][0], s);
    }
    return r;
}

inline glm::vec4 MulMat4Vec4(const glm::mat4& m, const glm::vec4& v)
{
    const float32x4_t vv = vld1q_f32(&v[0]);
    const float32x4_t a0 = vaddq_f32(vmulq_laneq_f32(vld1q_f32(&m[0][0]), vv, 0), vmulq_laneq_f32(vld1q_f32(&m[1][0]), vv, 1));
    const float32x4_t a1 = vaddq_f32(vmulq_laneq_f32(vld1q_f32(&m[2][0]), vv, 2), vmulq_laneq_f32(vld1q_f32(&m[3][0]), vv, 3));
    glm::vec4 r;
    vst1q_f32(&r[0], vaddq_f32(a0, a1));
    return r;
}
#else
inline glm::mat4 MulMat4(const glm::mat4& a, const glm::mat4& b) { return a * b; }
inline glm::vec4 MulMat4Vec4(const glm::mat4& m, const glm::vec4& v) { return m * v; }
#endif
