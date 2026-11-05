// Math helpers (see vecmat.h). Each function follows the reference port named in its comment.
#include "vecmat.h"

#include <algorithm>
#include <cmath>

namespace cmm
{

namespace
{
constexpr float EPS_1E4 = F32(0x38D1B717);
}

// orig: 0x0011CF60 RadNormalize
float RadNormalize(float rad)
{
    if (rad < -PI || PI < rad) rad = GModPositive(rad + PI, F32(0x40C90FDB)) - PI;
    return rad;
}

// orig: 0x0011CFE0 GLimitAbs
float GLimitAbs(float g, float gLimit)
{
    if (gLimit < g) return gLimit;
    if (g < -gLimit) g = -gLimit;
    return g;
}

// orig: 0x0011D948 GLimitLm
float GLimitLm(const LM* plm, float g)
{
    if (g < plm->gMin) return plm->gMin;
    if (plm->gMax < g) return plm->gMax;
    return g;
}

// orig: 0x0011D978 RadLimitLm
float RadLimitLm(const LM* plm, float rad)
{
    const float radMid = (plm->gMax + plm->gMin) * 0.5f;
    const float radHalf = (plm->gMax - plm->gMin) * 0.5f;
    return RadNormalize(radMid + GLimitAbs(RadNormalize(rad - radMid), radHalf));
}

// orig: 0x0011D898 GModPositive
float GModPositive(float gDividend, float gDivisor)
{
    float g = std::fmod(gDividend, gDivisor);
    if (g < 0.0f) g += gDivisor;
    return g;
}

// orig: 0x0011D508 CSolveQuadratic
int CSolveQuadratic(float a, float b, float c, float* ax)
{
    const float d = b * b - a * (c * 4.0f);
    if (d < 0.0f) return 0;
    const float a2 = a + a;
    const float alpha = std::sqrt(d) / a2;
    const float nb = -(b / a2);
    if (std::fabs(alpha) < EPS_1E4)
    {
        ax[0] = nb;
        return 1;
    }
    const float x1 = nb - alpha, x0 = nb + alpha;
    ax[0] = x0;
    ax[1] = x1;
    if (x1 < x0)
    {
        ax[0] = x1;
        ax[1] = x0;
    }
    return 2;
}

// orig: 0x0011D608 CalculateSinCos
void CalculateSinCos(float rad, float* pgSin, float* pgCos)
{
    const float g = (rad + F32(0x3FC90FDB)) * F32(0x3EA2F983);
    int n = static_cast<int>(g);
    if (g < 0.0f) n -= 1;
    const float gN = static_cast<float>(n) * PI;
    const bool fOdd = (n & 1) != 0;
    float x = rad - gN;
    if (fOdd) x = -x;
    const float x2 = x * x;
    float s = x;
    float p = s * x2;
    s = s + p * F32(0xBE2AAAA4);
    p = p * x2;
    s = s + p * F32(0x3C08873E);
    p = p * x2;
    s = s + p * F32(0xB94FB21F);
    p = p * x2;
    s = s + p * F32(0x362E9C14);
    s = std::clamp(s, -1.0f, 1.0f);
    float c = std::sqrt(1.0f - s * s);
    if (fOdd) c = -c;
    *pgCos = c;
    *pgSin = s;
}

// orig: 0x0011D018 GSmooth
float GSmooth(float gCur, float gTarget, float dt, const SMP* psmp, float* pdgNext)
{
    const float svFast = psmp->svFast;
    if (svFast == 0.0f)
    {
        if (pdgNext) *pdgNext = 0.0f;
        return gCur;
    }
    float dg = gCur - gTarget;
    const bool fNeg = dg < 0.0f;
    if (fNeg) dg = -dg;
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
            dt -= t;
            dg = dgSlow;
        }
    }
    if (!fDone)
    {
        const float svSlow = psmp->svSlow;
        const float a = (svFast - svSlow) / (dtFast + dtFast);
        float ax[2] = {0.0f, 0.0f};
        const int c = CSolveQuadratic(a, svSlow, -dg, ax);
        const float dtLeft = ax[c > 0 ? c - 1 : 0] - dt;  // (c = 0 reads ax[-1] in the original: never seen)
        if (0.0f < dtLeft)
        {
            const float f0 = (svFast - svSlow) * dtLeft;
            dgNext = -(svSlow + f0 / dtFast);
            g = (f0 * dtLeft) / (dtFast + dtFast) + svSlow * dtLeft;
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

// orig: 0x0011D1C0 GSmoothA
float GSmoothA(float gCur, float dgCur, float gTarget, float dt, const SMPA* psmpa, float* pdgNext)
{
    float dgNext;
    const SMP smp{psmpa->svFast, psmpa->svSlow, psmpa->dtFast};
    float g = GSmooth(gCur, gTarget, dt, &smp, &dgNext);
    if (psmpa->sdvMax > 0.0f)
    {
        const float ddg = psmpa->sdvMax * dt;
        const float dgMax = dgCur + ddg, dgMin = dgCur - ddg;
        if (dgNext > dgMax)
        {
            dgNext = dgMax;
            g = gCur + dgCur * dt + 0.5f * psmpa->sdvMax * dt * dt;
        }
        else if (dgNext < dgMin)
        {
            dgNext = dgMin;
            g = gCur + dgCur * dt - 0.5f * psmpa->sdvMax * dt * dt;
        }
    }
    if (pdgNext) *pdgNext = dgNext;
    return g;
}

// orig: 0x0011D2D0 RadSmooth
float RadSmooth(float radCur, float radTarget, float dt, const SMP* psmp, float* pdradNext)
{
    const float rad = GSmooth(0.0f, RadNormalize(radTarget - radCur), dt, psmp, pdradNext);
    return RadNormalize(radCur + rad);
}

// orig: 0x0011D340 RadSmoothA
float RadSmoothA(float radCur, float dradCur, float radTarget, float dt, const SMPA* psmpa, float* pdradNext)
{
    const float rad = GSmoothA(0.0f, dradCur, RadNormalize(radTarget - radCur), dt, psmpa, pdradNext);
    return RadNormalize(radCur + rad);
}

// orig: 0x0011F088 PosSmooth
glm::vec3 PosSmooth(const glm::vec3& pos, const glm::vec3& posTarget, float dt, const SMP* psmp, glm::vec3* pv)
{
    const glm::vec3 d = posTarget - pos;
    const float l = glm::length(d);
    if (!(EPS_1E4 < l))
    {
        if (pv) *pv = glm::vec3(0.0f);
        return posTarget;
    }
    float dgNext;
    const float g = GSmooth(0.0f, l, dt, psmp, &dgNext);
    if (pv) *pv = d * (dgNext / l);
    return pos + d * (g / l);
}

// orig: 0x0011E1A0 SetVectorCylind
glm::vec3 SetVectorCylind(float rad, float sXY, float sZ)
{
    float s, c;
    CalculateSinCos(rad, &s, &c);
    return glm::vec3(c * sXY, s * sXY, sZ);
}

// orig: 0x0011E208 SetVectorSphere
glm::vec3 SetVectorSphere(float radPan, float radTilt, float s)
{
    float sp, cp, st, ct;
    CalculateSinCos(radPan, &sp, &cp);
    CalculateSinCos(radTilt, &st, &ct);
    return glm::vec3((ct * cp) * s, (ct * sp) * s, st * s);
}

// orig: 0x0011EFB0 ConvertVectorToSphere
void ConvertVectorToSphere(const glm::vec3& v, float* pradPan, float* pradTilt, float* ps)
{
    if (pradPan) *pradPan = RadNormalize(std::atan2(v.y, v.x));
    if (pradTilt) *pradTilt = RadNormalize(std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)));
    if (ps) *ps = glm::length(v);
}

// orig: 0x0011EF08 ConvertVectorToCylind
void ConvertVectorToCylind(const glm::vec3& v, float* prad, float* pr, float* pz)
{
    if (prad) *prad = RadNormalize(std::atan2(v.y, v.x));
    if (pr) *pr = std::sqrt(v.x * v.x + v.y * v.y);
    if (pz) *pz = v.z;
}

// orig: 0x0011E9F8 LimitVectorLength
glm::vec3 LimitVectorLength(const glm::vec3& v, float gMax)
{
    const float l = glm::length(v);
    if (gMax < l) return v * (gMax / l);
    return v;
}

// orig: 0x0011BC00 (row i = sum over k of A[k][i] B[k])
glm::mat3 MulMat3TransposeA(const glm::mat3& a, const glm::mat3& b)
{
    glm::mat3 m;
    for (int i = 0; i < 3; ++i) m[i] = b[0] * a[0][i] + b[1] * a[1][i] + b[2] * a[2][i];
    return m;
}

// orig: 0x0011B500 BuildRotateMatrixAxis (0x11B480 with the sine and cosine of rad)
glm::mat3 BuildRotateMatrixAxis(float rad, const glm::vec3& a)
{
    float s, c;
    CalculateSinCos(rad, &s, &c);
    const float t = 1.0f - c;
    const glm::vec3 v5 = a * t, v4 = a * s;
    glm::mat3 m;
    m[0] = glm::vec3(v5.x * a.x + c, v5.y * a.x + v4.z, v5.z * a.x - v4.y);
    m[1] = glm::vec3(v5.x * a.y - v4.z, v5.y * a.y + c, v5.z * a.y + v4.x);
    m[2] = glm::vec3(v5.x * a.z + v4.y, v5.y * a.z - v4.x, v5.z * a.z + c);
    return m;
}

// orig: 0x0011B570 BuildRotateMatrixVector
glm::mat3 BuildRotateMatrixVector(const glm::vec3& v)
{
    const float g = glm::length(v);
    if (g < EPS_1E4) return glm::mat3(1.0f);
    return BuildRotateMatrixAxis(g, v * (1.0f / g));
}

// orig: 0x0011B628 BuildEulerMatrix
glm::mat3 BuildEulerMatrix(const glm::vec3& e)
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

// orig: 0x0011B720 BuildPanTiltMatrix (R(z, pan) then R(y, -tilt): rows = tilt rows x pan)
glm::mat3 BuildPanTiltMatrix(float radPan, float radTilt)
{
    const glm::mat3 a = BuildRotateMatrixAxis(radPan, glm::vec3(0, 0, 1));
    const glm::mat3 b = BuildRotateMatrixAxis(-radTilt, glm::vec3(0, 1, 0));
    return MulMat3(b, a);
}

// orig: 0x0011BC88 DecomposeRotateMatrixPanTilt
void DecomposeRotateMatrixPanTilt(const glm::mat3& m, float* pradPan, float* pradTilt)
{
    *pradPan = std::atan2(m[0].y, m[0].x);
    *pradTilt = std::atan2(m[0].z, std::sqrt(m[0].x * m[0].x + m[0].y * m[0].y));
}

// orig: 0x0011BCF8 DecomposeRotateMatrixRad
void DecomposeRotateMatrixRad(const glm::mat3& m, float* prad, glm::vec3* pvec)
{
    const float c = (((m[0].x + m[1].y) + m[2].z) - 1.0f) * 0.5f;
    if (F32(0x3F7FFFEF) < c)
    {
        *prad = 0.0f;
        *pvec = glm::vec3(1, 0, 0);
        return;
    }
    float gRest = 1.0f;
    const float d = 1.0f - c;
    glm::vec3 v;
    for (int n = 0; n < 2; ++n)
    {
        float a = std::fabs((m[n][n] - c) / d);
        if (gRest < a) a = gRest;
        else if (a < 0.0f) a = 0.0f;
        v[n] = std::sqrt(a);
        gRest -= a;
    }
    v.z = std::sqrt(gRest);
    if (c < F32(0xBF7FF972))  // a half turn, the signs from the largest component's off-diagonal
    {
        *prad = PI;
        if (v.y < v.x)
        {
            if (v.z < v.x)
            {
                if (m[0].y < 0.0f) v.y = -v.y;
                if (m[0].z < 0.0f) v.z = -v.z;
                *pvec = v;
                return;
            }
        }
        else if (v.z < v.y)
        {
            if (m[1].x < 0.0f) v.x = -v.x;
            if (m[1].z < 0.0f) v.z = -v.z;
            *pvec = v;
            return;
        }
        if (m[2].x < 0.0f) v.x = -v.x;
        if (m[2].y < 0.0f) v.y = -v.y;
        *pvec = v;
        return;
    }
    *prad = std::acos(c);
    if (m[0].y < m[1].x) v.z = -v.z;
    if (m[2].x < m[0].z) v.y = -v.y;
    if (m[1].z < m[2].y) v.x = -v.x;
    *pvec = v;
}

// orig: 0x0011BFF0 DecomposeEulerMatrix
glm::vec3 DecomposeEulerMatrix(const glm::mat3& m)
{
    const float ry = std::asin(GLimitAbs(-m[0].z, 1.0f));
    float rx, rz;
    if (ry < F32(0x3FC90C94))
    {
        if (F32(0xBFC90C94) < ry)
        {
            rx = std::atan2(m[1].z, m[2].z);
            rz = std::atan2(m[0].y, m[0].x);
        }
        else
        {
            rz = std::atan2(-m[1].x, -m[2].x);
            rx = 0.0f;
        }
    }
    else
    {
        rz = std::atan2(-m[1].x, m[2].x);
        rx = 0.0f;
    }
    return glm::vec3(rx, ry, rz);
}

// orig: 0x0011C950 SmoothMatrix
void SmoothMatrix(const glm::mat3& matPrev, const glm::mat3& matTarget, const SMP* psmp, float dt, glm::mat3* pmatOut,
                  glm::vec3* pw)
{
    float rad;
    glm::vec3 axis;
    DecomposeRotateMatrixRad(MulMat3TransposeA(matPrev, matTarget), &rad, &axis);
    float dradNext;
    const float g = GSmooth(0.0f, rad, dt, psmp, &dradNext);
    if (pmatOut)
    {
        if (g == rad) *pmatOut = matTarget;
        else *pmatOut = MulMat3(matPrev, BuildRotateMatrixAxis(g, axis));
    }
    if (pw) *pw = axis * dradNext;
}

// orig: 0x001A37D0 FitRecipFunction
void FitRecipFunction(float x0, float y0, float x1, float y1, float* pdu, float* pru)
{
    const float gEps = F32(0x38D1B717);
    // FFloatsNear (0x11D4D0): |a - b| / max(|a|, 1) < eps
    const float gScale = std::max(std::fabs(x0), 1.0f);
    if (std::fabs(x0 - x1) / gScale < gEps || std::fabs(x0) < gEps || std::fabs(x1) < gEps)
    {
        *pru = 0.0f;
        *pdu = y0;
        return;
    }
    const float r = ((x0 * x1) * (y1 - y0)) / (x0 - x1);
    *pru = r;
    *pdu = y0 - r / x0;
}

// orig: 0x0011C168 MulMatrix4 (each row of B transformed by A: the original's B * A)
MATRIX4 MulMatrix4(const MATRIX4& a, const MATRIX4& b)
{
    MATRIX4 m;
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec4& v = b.r[i];
        m.r[i] = a.r[0] * v.x + a.r[1] * v.y + a.r[2] * v.z + a.r[3] * v.w;
    }
    return m;
}

// orig: 0x0011C318 InverseAffineMatrix4
MATRIX4 InverseAffineMatrix4(const MATRIX4& m)
{
    const glm::vec3 r0(m.r[0]), r1(m.r[1]), r2(m.r[2]), t(m.r[3]);
    const glm::vec3 c0 = glm::cross(r1, r2), c1 = glm::cross(r2, r0), c2 = glm::cross(r0, r1);
    const float q = 1.0f / glm::dot(r0, c0);
    MATRIX4 o;
    o.r[0] = glm::vec4(c0.x * q, c1.x * q, c2.x * q, 0.0f);
    o.r[1] = glm::vec4(c0.y * q, c1.y * q, c2.y * q, 0.0f);
    o.r[2] = glm::vec4(c0.z * q, c1.z * q, c2.z * q, 0.0f);
    const glm::vec3 nt = -t;
    o.r[3] = glm::vec4(glm::vec3(o.r[0]) * nt.x + glm::vec3(o.r[1]) * nt.y + glm::vec3(o.r[2]) * nt.z, 1.0f);
    return o;
}

// orig: 0x0011C2F0 FInvertMatrix4 (0x11C500's Gauss-Jordan; natively glm's inverse, singular below 1e-4)
bool FInvertMatrix4(const MATRIX4& m, MATRIX4* pmatOut)
{
    glm::mat4 g;
    for (int i = 0; i < 4; ++i) g[i] = m.r[i];
    if (std::fabs(glm::determinant(g)) < 1e-12f) return false;
    const glm::mat4 inv = glm::inverse(g);
    for (int i = 0; i < 4; ++i) pmatOut->r[i] = inv[i];
    return true;
}

// orig: 0x0011C448 BuildSimpleProjectionMatrix
MATRIX4 BuildSimpleProjectionMatrix(float rx, float ry, float dx, float dy, float sNear, float sFar)
{
    MATRIX4 m;
    const float zScale = (sNear + sFar) / (sNear - sFar);
    m.r[0].x = rx;
    m.r[1].y = ry;
    m.r[2] = glm::vec4(dx, dy, zScale, 1.0f);
    m.r[3].z = sNear * (1.0f - zScale);
    return m;
}
}  // namespace cmm
