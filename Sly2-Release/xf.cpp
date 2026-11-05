// The transform records' runtime: controllers (0x12CE98.., 0x230018..), modes (0x2303A0), the per-frame update
// (0x22DE88) and the world matrices (0x22FAA0). Specification: Sly2-Reference/Sly2-Release/game/m_22C818.cpp, m_22C818_p3.cpp,
// m_22DE88_p3.cpp, m_12B3B0_p3.cpp (oracle-verified) and the decomp's func_00230018/00230100/002303A0/00230598/
// 00230630/0022C840 (matching C). Matrices are glm with columns = the original's rows (lo.h).

#include "xf.h"

#include "anim.h"
#include "ik.h"
#include "xfphys.h"
#include "xfmath.h"
#include "clock.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

int g_cXfStamp = 0;

namespace
{
constexpr float kEpsNear = 1e-4f;      // 0x38D1B717
const float s_gZero = 0.0f;            // orig D_002C44E0: the parameter of an invalidated record

bool FNear3(const glm::vec4& a, const glm::vec4& b)
{
    return FFloatsNear(a.x, b.x, kEpsNear) && FFloatsNear(a.y, b.y, kEpsNear) && FFloatsNear(a.z, b.z, kEpsNear);
}

// orig 0x00230310: the moving modes
bool FXfmMoving(int n)
{
    switch (n)
    {
    case 1: case 2: case 3: case 7: case 8: case 9: case 10: case 11: case 12: case 17: case 18: case 19: case 20:
    case 21:
        return true;
    default:
        return false;
    }
}
// orig 0x00230348: the modes at rest
bool FXfmStatic(int n) { return n == 0 || n == 3 || n == 10 || n == 12 || n == 19; }
// orig 0x00230380
bool FXfmVelocity(int n) { return n == 1 || n == 2 || n == 3; }

bool FKindTranslation(int k) { return k == 1 || k == 4 || k == 5 || k == 9 || k == 10; }
bool FKindRotation(int k) { return k == 6 || k == 7; }

// The rows 0..2 of a matrix as a 3x3 / back (the translation row and the w lanes kept / cleared).
glm::mat3 Rows3(const glm::mat4& m) { return glm::mat3(m); }
void SetRows3(glm::mat4& m, const glm::mat3& r)
{
    m[0] = glm::vec4(r[0], 0.0f);
    m[1] = glm::vec4(r[1], 0.0f);
    m[2] = glm::vec4(r[2], 0.0f);
}
glm::mat3 Mat3FromSeg(const XF* pxf)
{
    return glm::mat3(glm::vec3(pxf->aseg[0]), glm::vec3(pxf->aseg[1]), glm::vec3(pxf->aseg[2]));
}

void Bump(XF* pxf) { pxf->nStamp = ++g_cXfStamp; }

// orig 0x00256D18 (SetPos: xyz) + the original's store of 1.0 into the row's w
void SetTranslation(XF* pxf, const glm::vec4& pos) { pxf->mat[3] = glm::vec4(glm::vec3(pos), 1.0f); }

// orig 0x0022CAD0: b rotated by the angles a.y (tilt) and a.z (pan)
glm::vec4 VecRotateTiltPan(const glm::vec4& a, const glm::vec4& b)
{
    float s0, c0, s1, c1;
    CalculateSinCos(a.y, &s0, &c0);
    CalculateSinCos(a.z, &s1, &c1);
    return glm::vec4(c0 * c1 * b.x - s1 * b.y, c0 * s1 * b.x + c1 * b.y, -s0 * b.x + b.z, 0.0f);
}

// orig 0x0022CCD0: the skew (cross product) matrix of v times the 3x3 rows of pm (translation zero)
glm::mat4 DmatSkew(const glm::mat4& pm, const glm::vec4& v)
{
    glm::mat4 o(0.0f);
    const glm::vec3 w(v);
    for (int i = 0; i < 3; ++i) o[i] = glm::vec4(glm::cross(w, glm::vec3(pm[i])), 0.0f);
    return o;
}

// orig 0x0022CC00: v put by the kind in a zero matrix
glm::mat4 DmatOfValue(const XF* pxf, const glm::vec4& v)
{
    glm::mat4 m(0.0f);
    const int k = pxf->kind;
    if (FKindTranslation(k)) m[3] = glm::vec4(v.x, v.y, v.z, 0.0f);
    else if (k == 11) { m[1][0] = v.x; m[2][0] = v.y; m[2][1] = v.z; }
    else if (k == 12) { m[0][0] = v.x; m[1][1] = v.y; m[2][2] = v.z; }
    return m;
}

// The segment's spline at the record's parameter (0x117120 over [g0, g1] at *pg with the rate *pdgScale).
void EvalXfSpline(const XF* pxf, glm::vec4* ppos, glm::vec4* pvel)
{
    const float g = pxf->G();
    EvalHermiteVector(pxf->aseg[0], pxf->aseg[1], pxf->aseg[2], pxf->aseg[3], ppos, pvel, nullptr, pxf->g1 - pxf->g0,
                      g - pxf->g0, pxf->DgScale());
}

struct UPD
{
    ALO* palo;
    XF* r;
    bool& fp;             // nothing moved in the object (orig $fp)
    bool fSnap = false;   // orig sp+0x194: the object's flag 0x10
    float dt;
    float t;

    void Next(int n) { SetXfMode(palo, r, n); }
    void Moving();
    void RotVel();
    void Spin();
    void Target();
    void SeekRot();
    void SeekPos();
    void SplinePos();
    void SpringPos();
    void SpringRot();
};

// Modes 1, 2 of kind 1
void UPD::Moving()
{
    const glm::vec4 vnew = r->vel + r->aseg[3] * dt;
    const glm::vec4 zero(0.0f);
    if (!(FNear3(zero, vnew) && FNear3(zero, r->vel)))
    {
        fp = false;
        glm::vec4 pos = r->mat[3] + (vnew + r->vel) * (dt * 0.5f);
        // (native, UE6) an object driven by a script's run-to-point move: the position's sum rounded toward zero, as
        // on the EE (lo.h ALO::tScptRtz), so a step below half an ULP still moves it one ULP (the move's asymptotic
        // end reaches its radius as in the original)
        if (palo && palo->tScptRtz >= 0.0f && g_clock.t - palo->tScptRtz <= g_clock.dt * 1.5f)
        {
            const glm::vec4 step = (vnew + r->vel) * (dt * 0.5f);
            for (int i = 0; i < 3; ++i) pos[i] = AddRz(r->mat[3][i], step[i]);
        }
        SetTranslation(r, pos);
        r->vel = vnew;
        Bump(r);
        return;
    }
    r->vel = glm::vec4(0.0f, 0.0f, 0.0f, r->vel.w);
    if (r->mode != XFM_MovingHold) Next(XFM_Rest);
    else fp = false;
}

// Modes 1, 2 of kind 6
void UPD::RotVel()
{
    const glm::vec4 vnew = r->vel + r->aseg[3] * dt;
    const glm::vec4 zero(0.0f);
    if (!(FNear3(zero, vnew) && FNear3(zero, r->vel)))
    {
        fp = false;
        const glm::vec4 rot = (r->vel + vnew) * (dt * 0.5f);
        const glm::mat3 R = MatRotateVector(glm::vec3(rot));
        SetRows3(r->mat, NormalizeRotateMatrix3(R * Rows3(r->mat)));
        r->vel = vnew;
        Bump(r);
        return;
    }
    r->vel = glm::vec4(0.0f, 0.0f, 0.0f, r->vel.w);
    if (r->mode == XFM_MovingHold) fp = false;
    else Next(XFM_Rest);
}

// Mode 20 of kind 6
void UPD::Spin()
{
    if (r->kind != 6) return;
    fp = false;
    const float a = RadNormalize(r->aseg[2].x + dt * r->aseg[1].x);
    r->aseg[2].x = a;
    SetRows3(r->mat, MatRotateAxis(a, glm::vec3(r->aseg[0])));
    Bump(r);
}

// Modes 5, 6
void UPD::Target()
{
    if (r->state == 5)
        SetRows3(r->mat, Mat3FromSeg(r));
    else
        r->mat = MatXfWithValue(r, VecXfTarget(r));
    Bump(r);
    fp = false;
    if (r->mode == XFM_SnapHold && r->state == 2) Next(XFM_Arrived);
}

// Modes 7, 8, 9, 11 of the rotation kinds
void UPD::SeekRot()
{
    const glm::mat3 old = Rows3(r->mat);
    glm::vec4 w7(0.0f);
    if (r->state == 5)
        SetRows3(r->mat, Mat3FromSeg(r));
    else
    {
        const glm::vec4 ang = VecXfTarget(r);
        const glm::vec4 angv = VecXfTargetSlope(r);
        r->mat = MatXfWithValue(r, ang);
        w7 = VecRotateTiltPan(ang, angv);
    }
    // the remaining rotation (old rows transposed times the new ones) as an angle about an axis
    float angle;
    glm::vec3 axis;
    DecomposeRotateMatrixRad(MulMatrix3TransposeA(old, Rows3(r->mat)), &angle, &axis);
    const float f21 = glm::dot(glm::vec3(w7), axis);
    const float gain = static_cast<float>(static_cast<uint16_t>(r->lo));
    SMP smp;
    if (r->state == 3) smp = {gain * 0.015f, gain * 0.0f, 0.5f};
    else smp = {gain * 0.015f + std::fabs(f21), gain * 0.0f + std::fabs(f21), 0.5f};
    float f12;
    float dgNext = 0.0f;
    if (fSnap)
        f12 = angle;
    else
    {
        f12 = GSmooth(0.0f, angle, dt, &smp, &dgNext);
        if (r->mode == XFM_SeekTimed)
        {
            const float tEnd = r->tTarget;
            const float f0 = t < tEnd ? (angle * dt) / ((tEnd - t) + dt) : angle;
            if (f12 < f0)
            {
                f12 = f0;
                dgNext = f12 / dt;
            }
        }
        else if (r->mode == XFM_SeekAccel)
        {
            const float x = dgNext - f21;
            const glm::vec4 wnew = w7 + glm::vec4(axis, 0.0f) * x;
            const float lim = gain * 0.06f;
            const glm::vec4 dv = wnew - r->vel;
            const float d = std::sqrt(glm::dot(glm::vec3(dv), glm::vec3(dv))) / dt;
            if (lim < d)
            {
                const float s = lim / d;
                const glm::vec4 vclamp = r->vel + dv * s;
                const glm::vec4 rot = (r->vel + vclamp) * (dt * 0.5f);
                const glm::mat3 R = MatRotateVector(glm::vec3(rot));
                SetRows3(r->mat, NormalizeRotateMatrix3(R * old));
                r->vel = vclamp;
                fp = false;
                Bump(r);
                return;
            }
        }
    }
    if (angle <= f12)
    {
        // arrived: the angular velocity is the target's; the state moves on
        if (r->mode != XFM_SeekHold)
        {
            if (r->mode == XFM_SeekAccel)
            {
                if (r->state == 2) Next(XFM_Arrived11);
            }
            else if (r->state == 1) Next(XFM_SnapHold);
            else if (r->state == 2) Next(XFM_Arrived);
        }
        r->vel = w7;
    }
    else
    {
        const glm::mat3 R = MatRotateAxis(f12, axis);
        SetRows3(r->mat, NormalizeRotateMatrix3(R * old));
        r->vel = w7 + glm::vec4(axis, 0.0f) * (dgNext - f21);
    }
    fp = false;
    Bump(r);
}

// Modes 7, 8, 9, 11 of the other kinds
void UPD::SeekPos()
{
    const glm::vec4 tgt = VecXfTarget(r);
    const glm::vec4 tv = VecXfTargetSlope(r);
    const glm::vec4 cur = VecXfValue(r, r->mat);
    glm::vec4 dir = tgt - cur;
    const float f21 = std::sqrt(glm::dot(glm::vec3(dir), glm::vec3(dir)));
    if (kEpsNear < f21) dir = dir * (1.0f / f21);
    else dir = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    const float f22 = glm::dot(glm::vec3(tv), glm::vec3(dir));
    const bool fAxis = r->kind == 11 || r->kind == 12;
    const float c0 = fAxis ? 0.005f : 1.5f;
    const float gain = static_cast<float>(static_cast<uint16_t>(r->lo));
    const SMP smp = {gain * c0 + std::fabs(f22), gain * 0.0f + std::fabs(f22), 0.5f};
    float f20 = f21;
    float dgNext = 0.0f;
    if (!fSnap)
    {
        f20 = GSmooth(0.0f, f21, dt, &smp, &dgNext);
        if (r->mode == XFM_SeekTimed)
        {
            const float tEnd = r->tTarget;
            const float f0 = t < tEnd ? (f21 * dt) / ((tEnd - t) + dt) : f21;
            if (f20 < f0)
            {
                f20 = f0;
                dgNext = f20 / dt;
            }
        }
        else if (r->mode == XFM_SeekAccel)
        {
            const float x = dgNext - f22;
            const glm::vec4 vtarget = tv + dir * x;
            const glm::vec4 dv = vtarget - r->vel;
            const float d = std::sqrt(glm::dot(glm::vec3(dv), glm::vec3(dv))) / dt;
            const float lim = gain * (fAxis ? 0.02f : 6.0f);
            if (lim < d)
            {
                const float s = lim / d;
                const glm::vec4 vnew = r->vel + dv * s;
                const glm::vec4 sum = r->vel + vnew;
                r->vel = vnew;
                r->mat = MatXfWithValue(r, cur + sum * (dt * 0.5f));
                fp = false;
                Bump(r);
                return;
            }
        }
    }
    r->mat = MatXfWithValue(r, cur + dir * f20);
    if (f21 <= f20)
    {
        if (r->mode != XFM_SeekHold)
        {
            if (r->state == 1) Next(XFM_SnapHold);
            else if (r->state == 2) Next(XFM_Arrived);
        }
        r->vel = tv;
    }
    else
        r->vel = tv + dir * (dgNext - f22);
    fp = false;
    Bump(r);
}

// Mode 17
void UPD::SplinePos()
{
    const float tEnd = r->tTarget;
    glm::vec4 pos, vel;
    if (tEnd <= t)
    {
        pos = r->aseg[0];
        vel = glm::vec4(0.0f);
    }
    else
        EvalHermiteVector(r->mat[3], r->vel, r->aseg[0], r->aseg[1], &pos, &vel, nullptr, (tEnd - t) + dt, dt, 1.0f);
    fp = false;
    SetTranslation(r, pos);
    r->vel = vel;
    Bump(r);
}

// Mode 18 of kind 1
void UPD::SpringPos()
{
    const glm::vec4 tgt = VecXfTarget(r);
    const glm::vec4 V = VecXfTargetSlope(r);
    const float k1 = static_cast<float>(static_cast<uint16_t>(r->lo)) * 0.015f;
    const float k2 = static_cast<float>(static_cast<uint16_t>(r->hi)) * 0.0025f;
    const glm::vec3 vel(r->vel);
    const glm::vec3 dx = glm::vec3(tgt) - glm::vec3(r->mat[3]);
    const glm::vec3 force = dx * k1 + (glm::vec3(V) - vel) * k2;
    glm::vec3 vnew = vel + force * dt;
    if (r->state == 2 && glm::dot(vnew, vnew) < 25.0f && glm::dot(dx, dx) < 4.0f)
    {
        vnew = glm::vec3(0.0f);
        Next(XFM_SpringRest);
    }
    const float h = dt * 0.5f;
    SetTranslation(r, glm::vec4(glm::vec3(r->mat[3]) + vel * h + vnew * h, 1.0f));
    r->vel = glm::vec4(vnew, 0.0f);
    fp = false;
    Bump(r);
}

// Mode 18 of kind 6
void UPD::SpringRot()
{
    const glm::mat3 old = Rows3(r->mat);
    const glm::vec3 w(r->vel);
    glm::mat3 T;
    glm::vec3 wt(0.0f);
    if (r->state == 5)
        T = Mat3FromSeg(r);
    else
    {
        const glm::vec4 ang = VecXfTarget(r);
        const glm::vec4 angv = VecXfTargetSlope(r);
        T = Rows3(MatXfWithValue(r, ang));
        wt = glm::vec3(VecRotateTiltPan(ang, angv));
    }
    float ang;
    glm::vec3 axis;
    DecomposeRotateMatrixRad(MulMatrix3TransposeA(old, T), &ang, &axis);
    const float k1 = static_cast<float>(static_cast<uint16_t>(r->lo)) * 0.015f;
    const float k2 = static_cast<float>(static_cast<uint16_t>(r->hi)) * 0.0025f;
    const glm::vec3 torque = axis * (k1 * ang) + (wt - w) * k2;
    glm::vec3 wnew = w + torque * dt;
    if (r->state == 2 && glm::dot(wnew, wnew) < 0.0025f && ang < 0.01f)
    {
        wnew = glm::vec3(0.0f);
        Next(XFM_SpringRest);
    }
    const float h = dt * 0.5f;
    const glm::mat3 R = MatRotateVector(w * h + wnew * h);
    SetRows3(r->mat, NormalizeRotateMatrix3(R * old));
    r->vel = glm::vec4(wnew, 0.0f);
    fp = false;
    Bump(r);
}
}  // namespace

// orig: 0x0012CE98
void CTRL::Start()
{
    SetXfMode(palo, pxf, mode);
    InvalidateXfParams(palo, pxf);
}

// orig: 0x0012CEE0 (the head, or forwarded to by a kind-5 head)
bool CTRL::FFirst() const
{
    if (!pxf) return false;
    const CTRL* q = pxf->pctrlFirst;
    if (q == this) return true;
    if (!q || q->kind != 5) return false;
    return static_cast<const CTRLSOURCE*>(q)->psrcCur == this;
}

// orig: 0x0012CF30
void CTRL::Query(glm::vec4* ppos, glm::vec4* pvel, float g)
{
    (void)g;
    if (ppos) *ppos = VecXfValue(pxf, pxf->mat);
    if (pvel) *pvel = glm::vec4(0.0f, 0.0f, 0.0f, pvel->w);
}

// orig: 0x0012D0D8
void CTRLPOS::Query(glm::vec4* ppos, glm::vec4* pvel, float g)
{
    (void)g;
    if (ppos) *ppos = pos;
    if (pvel) *pvel = glm::vec4(0.0f, 0.0f, 0.0f, pvel->w);
}

// orig: 0x0012D088
void CTRLPOS::Fetch()
{
    pxf->aseg[0] = pos;
    pxf->aseg[1] = glm::vec4(0.0f);
    pxf->state = static_cast<int16_t>(state);
    ResetXfParams(pxf);
}

// orig: 0x0012D058
bool FXfHasCtrl(const XF* pxf, const CTRL* pctrl)
{
    for (const CTRL* q = pxf->pctrlFirst; q; q = q->pnext)
        if (q == pctrl) return true;
    return false;
}

// orig: 0x00230018
void AddXfCtrl(ALO* palo, XF* pxf, CTRL* pctrl)
{
    CTRL* phead = pxf->pctrlFirst;
    CTRL** pp = &pxf->pctrlFirst;
    for (CTRL* e = phead; e && e->prio > pctrl->prio; e = e->pnext) pp = &e->pnext;
    pctrl->pnext = *pp;
    *pp = pctrl;
    if (pxf->pctrlFirst != phead)
    {
        InvalidateXfParams(palo, pxf);
        if (phead) phead->End();
        pxf->pctrlFirst->Start();
    }
    else if (phead)
        phead->Refresh();
}

// orig: 0x00230100
void RemoveXfCtrl(ALO* palo, XF* pxf, CTRL* pctrl)
{
    CTRL* phead = pxf->pctrlFirst;
    CTRL** pp = &pxf->pctrlFirst;
    for (CTRL* e = phead; e; e = e->pnext)
    {
        if (e == pctrl)
        {
            *pp = e->pnext;
            if (pxf->pctrlFirst != phead)
            {
                InvalidateXfParams(palo, pxf);
                phead->End();
                if (pxf->pctrlFirst) pxf->pctrlFirst->Start();
            }
            else if (phead)
                phead->Refresh();
            return;
        }
        pp = &e->pnext;
    }
}

// orig: 0x002301D0 (insertion of each controller, last first, by priority)
void SortXfCtrls(ALO* palo, XF* pxf)
{
    std::vector<CTRL*> a;
    for (CTRL* q = pxf->pctrlFirst; q; q = q->pnext) a.push_back(q);
    CTRL* phead = pxf->pctrlFirst;
    pxf->pctrlFirst = nullptr;
    for (int i = static_cast<int>(a.size()) - 1; i >= 0; --i)
    {
        CTRL* pc = a[i];
        CTRL** pp = &pxf->pctrlFirst;
        for (CTRL* e = *pp; e && pc->prio < e->prio; e = e->pnext) pp = &e->pnext;
        pc->pnext = *pp;
        *pp = pc;
    }
    if (pxf->pctrlFirst != phead)
    {
        InvalidateXfParams(palo, pxf);
        if (phead) phead->End();
        if (pxf->pctrlFirst) pxf->pctrlFirst->Start();
    }
    else if (phead)
        phead->Refresh();
}

// orig: 0x0022C818
void ResetXfParams(XF* pxf)
{
    pxf->pg = nullptr;        // &+0x80
    pxf->pdgScale = nullptr;  // &+0x84
    pxf->g1 = 1.0f;
    pxf->g0 = 0.0f;
}

// orig: 0x0022C840
void InvalidateXfParams(ALO* palo, XF* pxf)
{
    switch (pxf->mode)
    {
    case 0: case 1: case 2: case 3: case 13: case 14: case 15: case 16: case 20: case 21:
        break;
    default:
        pxf->pg = &s_gZero;
        pxf->g0 = -FLT_MAX;  // D_002A98D0
        pxf->g1 = -FLT_MAX;
        pxf->pdgScale = &s_gZero;
        break;
    }
    pxf->state = 0;
    if (palo) MarkAloXfs(palo, 4);  // vt slot 47 (0x1F2FE0)
}

// orig: 0x002303A0
void SetXfMode(ALO* palo, XF* pxf, int n)
{
    if (n == pxf->mode) return;
    if (FXfmMoving(n) && !FXfmMoving(pxf->mode))
    {
        const glm::mat4 m = DmatXf(pxf);
        const int k = pxf->kind;
        if (FKindTranslation(k)) pxf->vel = m[3];
        else if (k == 11) pxf->vel = glm::vec4(m[1][0], m[2][0], m[2][1], pxf->vel.w);
        else if (k == 12) pxf->vel = glm::vec4(m[0][0], m[1][1], m[2][2], pxf->vel.w);
        else if (FKindRotation(k))
        {
            const glm::mat4 s = m * InverseAffineMatrix4(pxf->mat);  // orig 0x11C168(m, 0x11C318(p))
            pxf->vel = glm::vec4(s[1][2], s[2][0], s[0][1], pxf->vel.w);
        }
    }
    switch (n)
    {
    case 1: case 2: case 3:
        if (!FXfmVelocity(pxf->mode)) pxf->aseg[3] = glm::vec4(0.0f, 0.0f, 0.0f, pxf->aseg[3].w);
        ResetXfParams(pxf);
        break;
    case 0: case 13: case 14: case 15: case 16: case 17: case 20: case 21:
        ResetXfParams(pxf);
        break;
    default:
        break;
    }
    pxf->mode = static_cast<int16_t>(n);
    if (!FXfmStatic(n) && palo) MarkAloXfs(palo, 4);  // vt slot 47 (0x1F2FE0)
}

// orig: 0x00230598
void HoldXfMode(ALO* palo, XF* pxf)
{
    switch (pxf->mode)
    {
    case 6: case 10: SetXfMode(palo, pxf, 7); break;
    case 12: SetXfMode(palo, pxf, 11); break;
    case 19: SetXfMode(palo, pxf, 18); break;
    case 3: SetXfMode(palo, pxf, 1); break;
    default: break;
    }
}

// orig: 0x00230630
void ReleaseXfMode(ALO* palo, XF* pxf)
{
    switch (pxf->mode)
    {
    case 7: case 9:
        if (pxf->state == 2) SetXfMode(palo, pxf, 10);
        else if (pxf->state == 1) SetXfMode(palo, pxf, 6);
        break;
    case 11:
        if (pxf->state == 2) SetXfMode(palo, pxf, 12);
        break;
    case 18:
        if (pxf->state == 2) SetXfMode(palo, pxf, 19);
        break;
    case 1:
        SetXfMode(palo, pxf, 3);
        break;
    default:
        break;
    }
}

// orig: 0x0022DD00
void RefreshXf(ALO* palo, XF* pxf)
{
    const float g = pxf->G();
    if (!(g < pxf->g0) && !(pxf->g1 < g) && pxf->state < 3) return;
    CTRL* pctrl = pxf->pctrlFirst;
    if (!pctrl)
    {
        pxf->state = 0;
        ResetXfParams(pxf);
        SetXfMode(palo, pxf, 0);
        return;
    }
    pctrl->Fetch();
    const int k = pxf->mode;
    const int st = pxf->state;
    if (k == 10)
    {
        if (st == 3 || st == 4) SetXfMode(palo, pxf, 7);
        else if (st != 2) SetXfMode(palo, pxf, 6);
    }
    else if (k == 6)
    {
        if (st == 3 || st == 4) SetXfMode(palo, pxf, 7);
    }
    else if (k == 12)
    {
        if (st != 2) SetXfMode(palo, pxf, 11);
    }
    else if (k == 19)
    {
        if (st != 2) SetXfMode(palo, pxf, 18);
    }
}

// orig: 0x0022C8C0
glm::mat4 MatXfWithValue(const XF* pxf, const glm::vec4& v)
{
    glm::mat4 m = pxf->mat;
    switch (pxf->kind)
    {
    case 1: case 4: case 5: case 9: case 10:
        m[3] = glm::vec4(v.x, v.y, v.z, m[3].w);
        break;
    case 6: case 7:
        SetRows3(m, MatEuler(glm::vec3(v)));
        m[3] = glm::vec4(glm::vec3(pxf->mat[3]), 1.0f);
        break;
    case 11:
        m[1][0] = v.x; m[2][0] = v.y; m[2][1] = v.z;
        break;
    case 12:
        m[0][0] = v.x; m[1][1] = v.y; m[2][2] = v.z;
        break;
    default:  // 2, 3: the matrix; the others: the original leaves its output as it was (here: the matrix)
        break;
    }
    return m;
}

// orig: 0x0022CA18
glm::vec4 VecXfValue(const XF* pxf, const glm::mat4& m, const glm::vec4& vDefault)
{
    switch (pxf->kind)
    {
    case 1: case 4: case 5: case 9: case 10: return glm::vec4(glm::vec3(m[3]), 0.0f);
    case 6: case 7: return glm::vec4(EulerFromMat(glm::mat3(m)), 0.0f);
    case 11: return glm::vec4(m[1][0], m[2][0], m[2][1], 0.0f);
    case 12: return glm::vec4(m[0][0], m[1][1], m[2][2], 0.0f);
    default: return vDefault;
    }
}

// 0x22DE88's target value by the state
glm::vec4 VecXfTarget(const XF* pxf)
{
    switch (pxf->state)
    {
    case 0: return VecXfValue(pxf, pxf->mat);
    case 1: { glm::vec4 pos; EvalXfSpline(pxf, &pos, nullptr); return pos; }
    case 2: case 3: case 4: return pxf->aseg[0];
    case 5: return glm::vec4(EulerFromMat(Mat3FromSeg(pxf)), 0.0f);
    default: return glm::vec4(0.0f);
    }
}

// 0x22DE88's target velocity by the state
glm::vec4 VecXfTargetSlope(const XF* pxf)
{
    switch (pxf->state)
    {
    case 1: { glm::vec4 vel; EvalXfSpline(pxf, nullptr, &vel); return vel; }
    case 3: case 4: return pxf->aseg[1];
    default: return glm::vec4(0.0f);
    }
}

// orig: 0x0022CDC0
glm::mat4 DmatXf(const XF* pxf)
{
    const int mode = pxf->mode;
    switch (mode)
    {
    case 0: case 3: case 10: case 12: case 13: case 14: case 15: case 16: case 19:
        return glm::mat4(0.0f);
    default:
        break;
    }
    const int kind = pxf->kind;
    auto fMovingMode = [](int m) {
        switch (m)
        {
        case 1: case 2: case 7: case 8: case 9: case 11: case 17: case 18: case 20: case 21: return true;
        default: return false;
        }
    };
    switch (kind)
    {
    case 8: case 14:
    {
        const XF* pxfO = pxf->pxfOrigin;
        glm::mat4 m = pxfO ? DmatXf(pxfO) : glm::mat4(0.0f);
        m[3] = glm::vec4(-glm::vec3(m[3]), m[3].w);
        return m;
    }
    case 6: case 7:
        if (fMovingMode(mode)) return DmatSkew(pxf->mat, pxf->vel);
        if (mode == 5 || mode == 6)
        {
            if (pxf->state == 5) return glm::mat4(0.0f);
            return DmatSkew(pxf->mat, VecRotateTiltPan(VecXfTarget(pxf), VecXfTargetSlope(pxf)));
        }
        return glm::mat4(0.0f);
    case 1: case 4: case 5: case 9: case 10: case 11: case 12:
    {
        glm::vec4 v(0.0f);
        if (fMovingMode(mode)) v = pxf->vel;
        else if (mode == 5 || mode == 6)
        {
            if (pxf->state == 3 || pxf->state == 4) v = pxf->aseg[1];
            else if (pxf->state == 1) EvalXfSpline(pxf, nullptr, &v);
        }
        return DmatOfValue(pxf, v);
    }
    default:
        return glm::mat4(0.0f);
    }
}

// orig: 0x0022DE88
void UpdateAloXfs(ALO* palo, float t, float dt)
{
    // ($fp: 1 unless the parent object's transforms move: flag 4)
    bool fp = !(palo->paloParent && (GrfUpdOfLo(palo->paloParent) & 4));
    // (the object's flag 0x10, orig sp+0x194: the seeks arrive at once; set with 4 by the physics post-load 0x1F2C30,
    // cleared after this update)
    const bool fSnap = (GrfUpdOfLo(palo) & 0x10u) != 0;
    for (auto& up : palo->axf)
    {
        XF* r = up.get();
        UPD u{palo, r, fp};
        u.fSnap = fSnap;
        u.dt = dt;
        u.t = t;
        RefreshXf(palo, r);
        const int mode = r->mode;
        const int kind = r->kind;
        switch (mode)
        {
        case 1: case 2:
            if (kind == 1) u.Moving();
            else if (kind == 6) u.RotVel();
            break;
        case 4:
            if (const XF* po = static_cast<const XF*>(r->pvA8))
            {
                r->mat[3] = glm::vec4(-glm::vec3(po->mat[3]), r->mat[3].w);
                r->nStamp = po->nStamp;
            }
            break;
        case 5: case 6: u.Target(); break;
        case 7: case 8: case 9: case 11:
            if (kind == 6 || kind == 7) u.SeekRot();
            else u.SeekPos();
            break;
        case 13:
            if (r->pvA8) SolveAloPair(palo, static_cast<ALOPAIR*>(r->pvA8));
            break;
        case 15: UpdateXfLink(palo, r->pvA8); break;   // 0x1604E8 (xfphys.cpp)
        case 16: UpdateXfChain(palo, r->pvA8); break;  // 0x21CB18 (chain.cpp)
        case 17: u.SplinePos(); break;
        case 18:
            if (kind == 1) u.SpringPos();
            else if (kind == 6) u.SpringRot();
            else
            {
                u.fp = false;
                Bump(r);
            }
            break;
        case 20: u.Spin(); break;
        case 21: u.fp = false; break;
        default: break;
        }
        // the world matrix: the parent's world times the local; the parent's stamp if newer
        if (XF* par = r->pxfRef)
        {
            r->matWorld = par->matWorld * r->mat;
            r->nStamp = std::max(par->nStamp, r->nStamp);
        }
        else
            r->matWorld = r->mat;
    }
    // the object's flags lose bit 4 when nothing moved (its transforms sleep until a mode change marks them)
    if (fp) SetGrfUpdOfLo(palo, GrfUpdOfLo(palo) & ~4u);
    if (fSnap) SetGrfUpdOfLo(palo, GrfUpdOfLo(palo) & ~0x10u);
    PostUpdateAloXfs(palo);  // 0x22FA20 (xfphys.cpp)
}

// orig: 0x0022FAA0
void UpdateAloXfWorlds(ALO* palo)
{
    for (auto& up : palo->axf)
    {
        XF* r = up.get();
        if (XF* par = r->pxfRef)
        {
            r->matWorld = par->matWorld * r->mat;
            r->nStamp = std::max(par->nStamp, r->nStamp);
        }
        else
            r->matWorld = r->mat;
    }
    PostUpdateAloXfs(palo);  // 0x22FA20 (xfphys.cpp)
}

namespace
{
// kind 9 (0x2CA6A0): spins the record about its vector (orig 0x12E120 Start)
struct CTRLSPIN : CTRLJOB
{
    void Start() override
    {
        CTRL::Start();
        const glm::vec4 v(job.vec, 0.0f);
        pxf->vel = v;
        const float g = std::sqrt(glm::dot(job.vec, job.vec));
        pxf->aseg[0] = g < kEpsNear ? glm::vec4(1.0f, 0.0f, 0.0f, 0.0f) : v * (1.0f / g);
        pxf->aseg[1].x = g;
        pxf->aseg[2].x = 0.0f;
    }
};
}

// The parts of 0x22D7A0 after each record is read (see xf.h).
void InitAloXfs(ALO* palo)
{
    for (auto& up : palo->axf)
    {
        XF* e = up.get();
        e->pctrlFirst = nullptr;
        e->mode = 0;
        e->state = 0;
        const int kind = e->kind;
        if (!(kind == 0 || kind == 13 || kind == 15 || kind == 8 || kind == 14))
        {
            const glm::vec4 v = VecXfValue(e, e->mat);
            e->aseg[0] = v;
            e->aseg[2] = v;
        }
        if (kind == 8 || kind == 14) e->pvA8 = e->pxfOrigin;
        ResetXfParams(e);
        for (const XFJOB& job : e->ajob)
        {
            CTRL* pctrl = nullptr;
            switch (job.kind)
            {
            case 1:
            {
                auto* p = new CTRLPOS();
                p->state = 2;
                p->pos = VecXfValue(e, e->mat);
                pctrl = p;
                pctrl->prio = -1;
                break;
            }
            case 2:
            {
                auto* p = new CTRLLOOKAT();
                p->job = job;
                p->posRest = VecXfValue(e, e->mat);  // +0x20
                pctrl = p;
                pctrl->prio = job.b;  // +0x68
                break;
            }
            case 4:
            {
                auto* p = new CTRLLOOKMOTION();
                p->job = job;
                pctrl = p;
                pctrl->prio = -1;
                break;
            }
            case 9:
            {
                auto* p = new CTRLSPIN();
                p->job = job;
                pctrl = p;
                pctrl->prio = -1;
                break;
            }
            default:
                break;
            }
            if (!pctrl) continue;
            pctrl->kind = job.kind;
            pctrl->mode = job.id;  // +0x14: the S8 read (kind 9: 20)
            pctrl->palo = palo;
            pctrl->pxf = e;
            AddXfCtrl(palo, e, pctrl);  // (the controllers live as long as the world: TODO(anim) free at unload)
        }
    }
    // the joints (0x186670's end: rest poses, kind-7 controllers)
    InitAloPairs(palo);
    // the links' and chains' records (0x1603B8 / 0x21C888's ends: modes 15 / 16; xfphys.cpp, chain.cpp)
    InitAloLinks(palo);
    InitAloChains(palo);
    // 0x22D7A0's end: an object with records loses flag 0x20 of +0x40 (the update table's entry or its own flags)
    if (!palo->axf.empty()) SetGrfUpdOfLo(palo, GrfUpdOfLo(palo) & ~0x20u);
    // (the world matrices are the loader's post-load ones, sw.cpp)
}

// ---- the controllers other code makes (0x12DB50..0x12E460) ----

// orig: 0x0012E010
void CTRLTARGET::Fetch()
{
    pxf->tTarget = tArrive;
    pxf->aseg[0] = pos;
    pxf->aseg[1] = vel;
    pxf->state = 2;
    ResetXfParams(pxf);
}

// orig: 0x0012E0F0 (refresh the live segment only when this controller is at the head)
void CTRLTARGET::SetGoal(const glm::vec4& newPos, const glm::vec4& newVel)
{
    pos = newPos;
    vel = newVel;
    if (pxf->pctrlFirst == this)
    {
        pxf->aseg[0] = pos;
        pxf->aseg[1] = vel;
    }
}

// orig: 0x0012D858
void CTRLSOURCE::Start()
{
    psrcCur = pnext;  // orig +0x10 is the next controller in this record's priority list
    if (psrcCur) psrcCur->Start();
}

// orig: 0x0012D890
void CTRLSOURCE::End()
{
    if (psrcCur)
    {
        psrcCur->End();
        psrcCur = nullptr;
    }
}

// orig: 0x0012D8D8
void CTRLSOURCE::Refresh()
{
    if (psrcCur == pnext) return;
    InvalidateXfParams(palo, pxf);
    if (psrcCur) psrcCur->End();
    psrcCur = pnext;
    if (psrcCur) psrcCur->Start();
}

// orig: 0x0012D958
void CTRLSOURCE::Fetch()
{
    if (!psrcCur) return;  // (orig 0x12CF28: nothing)
    psrcCur->Fetch();
    pxf->aseg[0] += ofs;
    pxf->aseg[2] += ofs;
}

// orig: 0x0012D9D0: predict through the current source, then add only its position offset.
void CTRLSOURCE::Query(glm::vec4* ppos, glm::vec4* pvel, float g)
{
    if (!psrcCur)
    {
        // The original requires an active source. Preserve the prior native
        // current-record fallback while a handle has no downstream controller.
        CTRL::Query(ppos, pvel, g);
        return;
    }
    psrcCur->Query(ppos, pvel, g);
    if (ppos) *ppos += ofs;
}

// orig: 0x0012DC18
CTRLPOS* PctrlNewPos(ALO* palo, XF* pxf, int prio, int mode, int state, const glm::vec4& pos)
{
    auto* p = new CTRLPOS();
    p->kind = 1;
    p->palo = palo;
    p->pxf = pxf;
    p->prio = prio;
    p->mode = mode;
    p->state = state;
    p->pos = pos;
    AddXfCtrl(palo, pxf, p);
    return p;
}

// orig: 0x0012DE58
CTRLTARGET* PctrlNewTarget(ALO* palo, XF* pxf, int prio, float tArrive, const glm::vec4& pos, const glm::vec4& vel)
{
    auto* p = new CTRLTARGET();
    p->kind = 8;
    p->palo = palo;
    p->pxf = pxf;
    p->prio = prio;
    p->mode = 0x11;
    p->tArrive = tArrive;
    p->pos = pos;
    p->vel = vel;
    AddXfCtrl(palo, pxf, p);
    return p;
}

// orig: 0x0012DBB0
CTRLSOURCE* PctrlNewSource(ALO* palo, XF* pxf, int prio)
{
    auto* p = new CTRLSOURCE();
    p->kind = 5;
    p->palo = palo;
    p->pxf = pxf;
    p->prio = prio;
    p->mode = -1;
    AddXfCtrl(palo, pxf, p);
    return p;
}

// orig: 0x0012D108
void SetCtrlPos(CTRLPOS* pctrl, const glm::vec4& pos)
{
    const glm::vec4 d = pos - pctrl->pos;
    pctrl->pos = pos;
    if (pctrl->pxf->pctrlFirst != pctrl) return;
    if (FFloatsNear(0.0f, d.x, kEpsNear) && FFloatsNear(0.0f, d.y, kEpsNear) && FFloatsNear(0.0f, d.z, kEpsNear)) return;
    pctrl->pxf->aseg[0] = pos;
    HoldXfMode(pctrl->palo, pctrl->pxf);
}

// orig: 0x0012DA30
void SetCtrlSourceOffset(CTRLSOURCE* pctrl, const glm::vec4& ofs)
{
    const glm::vec4 d = ofs - pctrl->ofs;
    pctrl->ofs = ofs;
    if (pctrl->pxf->pctrlFirst != pctrl) return;
    if (FFloatsNear(0.0f, d.x, kEpsNear) && FFloatsNear(0.0f, d.y, kEpsNear) && FFloatsNear(0.0f, d.z, kEpsNear)) return;
    pctrl->pxf->aseg[0] += d;
    pctrl->pxf->aseg[2] += d;
    HoldXfMode(pctrl->palo, pctrl->pxf);
}

// orig: 0x0012CFE0
void EnsureCtrlAdded(CTRL* pctrl)
{
    if (!FXfHasCtrl(pctrl->pxf, pctrl)) AddXfCtrl(pctrl->palo, pctrl->pxf, pctrl);
}

// orig: 0x0012D020
void EnsureCtrlRemoved(CTRL* pctrl)
{
    if (FXfHasCtrl(pctrl->pxf, pctrl)) RemoveXfCtrl(pctrl->palo, pctrl->pxf, pctrl);
}

// orig: 0x0012E460
void DeleteCtrl(CTRL** ppctrl)
{
    if (!*ppctrl) return;
    EnsureCtrlRemoved(*ppctrl);
    delete *ppctrl;
    *ppctrl = nullptr;
}

// (QoL draw distance above 1x, drawdist-2: drawdist.cpp) the record's local matrix as its motion would have made it
// dt seconds on, for drawing an inactive tree that the update table leaves out; nothing of the record changes (the
// caller puts the matrix in place for the scene's placement and restores the record). Only the motions that need
// nothing but the record: a constant angular velocity (modes 1 / 2 of kind 6), the spin (mode 20) and a snap to the
// segment's spline value (modes 5 / 6, state 1) while the parameter is inside the segment (the animation's time:
// leaving it would fetch the next segment from the controller). false: none of these.
bool FXfVisualPose(const XF* pxf, float dt, glm::mat4* pmat)
{
    const int mode = pxf->mode, kind = pxf->kind;
    if ((mode == XFM_Moving || mode == XFM_MovingHold) && kind == 6)
    {
        if (FNear3(glm::vec4(0.0f), pxf->vel)) return false;
        *pmat = pxf->mat;
        SetRows3(*pmat, NormalizeRotateMatrix3(MatRotateVector(glm::vec3(pxf->vel) * dt) * Rows3(pxf->mat)));
        return true;
    }
    if (mode == XFM_Spin && kind == 6)
    {
        if (pxf->aseg[1].x == 0.0f) return false;
        *pmat = pxf->mat;
        SetRows3(*pmat, MatRotateAxis(RadNormalize(pxf->aseg[2].x + dt * pxf->aseg[1].x), glm::vec3(pxf->aseg[0])));
        return true;
    }
    if ((mode == XFM_Snap || mode == XFM_SnapHold) && pxf->state == 1)
    {
        const float g = pxf->G();
        if (g < pxf->g0 || pxf->g1 < g) return false;
        *pmat = MatXfWithValue(pxf, VecXfTarget(pxf));
        return true;
    }
    return false;
}
