// The physics side of the transform records (see xfphys.h).

#include "xfphys.h"
#include "sly3/ids3.h"

#include "anim.h"
#include "mat.h"
#include "vtnative.h"

void (*g_pfnXfObjectMoved)(ALO* palo) = nullptr;

namespace
{
inline bool FModeSkipChain(int mode)
{
    return mode == 0 || mode == 3 || mode == 10 || mode == 12 || mode == 15 || mode == 19;
}
inline void Bump(XF* r) { r->nStamp = ++g_cXfStamp; }
}

// orig: 0x00230310
bool FXfModeMoving(int n)
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
// orig: 0x00230348
bool FXfModeStill(int n) { return n == 0 || n == 3 || n == 10 || n == 12 || n == 19; }
// orig: 0x00230380
bool FXfModeVelocity(int n) { return n == 1 || n == 2 || n == 3; }

// orig: 0x001A66C8 (LO vt slot 47: the object's flags)
void OrLoUpdateFlagsLo(LO* plo, uint32_t grf) { SetGrfUpdOfLo(plo, GrfUpdOfLo(plo) | grf); }
NATIVEFN(0x1A66C8, OrLoUpdateFlagsLo);
// orig: 0x001F2FE0 (ALO vt slot 47: anim.h's MarkAloXfs)
void OrLoUpdateFlagsAlo(LO* plo, uint32_t grf) { MarkAloXfs(static_cast<ALO*>(plo), grf); }
NATIVEFN(0x1F2FE0, OrLoUpdateFlagsAlo);

void OrLoUpdateFlags(LO* plo, uint32_t grf)
{
    if (!plo) return;
    using PFN = void (*)(LO*, uint32_t);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(plo, 47))) pfn(plo, grf);
    else if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) MarkAloXfs(static_cast<ALO*>(plo), grf);
    else SetGrfUpdOfLo(plo, GrfUpdOfLo(plo) | grf);
}

// orig: 0x0022D148 (the sum of the derivative matrices of the records from pxf up the parents to pxfStop, each in
// world space (d(world) x inverse(world)); then into pxfStop's space)
glm::mat4 DmatXfChain(XF* po, XF* poStop)
{
    glm::mat4 sum(0.0f);
    for (XF* p = po; p && p != poStop; p = p->pxfRef)
    {
        if (FModeSkipChain(p->mode)) continue;
        const glm::mat4 d = DmatXf(p);
        glm::mat4 e;
        if (p->pxfRef)
            e = MulMatrix4(MulMatrix4(p->pxfRef->matWorld, d), InverseAffineMatrix4(p->matWorld));
        else
            e = MulMatrix4(d, InverseAffineMatrix4(p->matWorld));
        sum = sum + e;
    }
    if (poStop) sum = MulMatrix4(InverseAffineMatrix4(poStop->matWorld), sum);
    return sum;
}

// orig: 0x0022D348 (the velocity of the world point pos from the motion of the records pxf up the parents to stop:
// translations add their velocity rotated to world space; the others their derivative applied to the point (in
// their space); then into stop's space. Native: modes whose velocity the original takes from a stale stack slot
// (4, 13, 14, 16 of translations) count as still.)
glm::vec3 VelXfPoint(XF* po, XF* stop, const glm::vec3& pos)
{
    glm::vec3 sum(0.0f);
    for (XF* p = po; p && p != stop; p = p->pxfRef)
    {
        const int mode = p->mode;
        if (FModeSkipChain(mode)) continue;
        if (FXfKindTranslation(p->kind))
        {
            glm::vec3 v(0.0f);
            switch (mode)
            {
            case 1: case 2: case 7: case 8: case 9: case 11: case 17: case 18: case 20: case 21:
                v = glm::vec3(p->vel);
                break;
            case 5: case 6:
                if (p->pctrlFirst) RefreshXf(p->pctrlFirst->palo, p);
                v = glm::vec3(VecXfTargetSlope(p));  // (state 3 / 4: +0xD0; 1: the spline's slope)
                break;
            default:
                break;
            }
            sum += p->pxfRef ? XformDir(p->pxfRef->matWorld, v) : v;
        }
        else
        {
            if (p->pctrlFirst) RefreshXf(p->pctrlFirst->palo, p);
            const glm::mat4 d = DmatXf(p);
            const glm::vec3 local = XformPos(InverseAffineMatrix4(p->matWorld), pos);
            glm::vec3 dv = XformPos(d, local);
            if (p->pxfRef) dv = XformDir(p->pxfRef->matWorld, dv);
            sum += dv;
        }
    }
    if (stop) sum = XformDir(InverseAffineMatrix4(stop->matWorld), sum);
    return sum;
}

// orig: 0x0022FA20 (an object in the world (flag bit 60) that moved: the world's hook)
void PostUpdateAloXfs(ALO* p)
{
    if (!p->fInWorld) return;
    if (g_pfnXfObjectMoved) g_pfnXfObjectMoved(p);
}

// orig: 0x0022FBE0 (ALO vt slot 18: the world matrices, then slot 18 of each child)
void UpdateAloXfWorldsHierarchy(ALO* p)
{
    UpdateAloXfWorlds(p);
    for (LO* pc : p->aploChild)
    {
        // The child list also holds non-ALO objects; their own slot 18 may be the LO no-op.
        using PFN = void (*)(LO*);
        if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(pc, 18))) pfn(pc);
    }
}
NATIVEFN(0x22FBE0, UpdateAloXfWorldsHierarchy);
// orig: 0x0022FBD8 (LO vt slot 18: nothing)
void UpdateLoXfWorldsNothing(LO*) {}
NATIVEFN(0x22FBD8, UpdateLoXfWorldsNothing);

// orig: 0x0022FC48 (the point from a's space to world, then into b's)
glm::vec3 PosConvertXfs(XF* a, XF* b, const glm::vec3& vin)
{
    glm::vec3 v = vin;
    if (a) v = XformPos(a->matWorld, v);
    if (b) v = XformPos(InverseAffineMatrix4(b->matWorld), v);
    return v;
}

// orig: 0x0022FCC8 (the direction from a's space to world, then into b's)
glm::vec3 VecConvertXfs(XF* a, XF* b, const glm::vec3& vin)
{
    glm::vec3 v = vin;
    if (a) v = Mat3Of(a->matWorld) * v;
    if (b) v = InverseMatrix3(Mat3Of(b->matWorld)) * v;
    return v;
}

// orig: 0x0022FD38 (the 3x3 m from a's space into b's: inverse(b) a m in row terms)
glm::mat3 MatConvertXfs(XF* pa, XF* pb, const glm::mat3& m)
{
    if (pa)
    {
        const glm::mat3 a = Mat3Of(pa->matWorld);
        if (pb) return (InverseMatrix3(Mat3Of(pb->matWorld)) * a) * m;
        return a * m;
    }
    if (pb) return InverseMatrix3(Mat3Of(pb->matWorld)) * m;
    return m;
}

// orig: 0x001603B8's end (the records follow their links: +0xA8 and mode 15, 0x2303A0)
void InitAloLinks(ALO* palo)
{
    for (ALOLINK& l : palo->alink)
        if (l.pxf)
        {
            l.pxf->pvA8 = &l;
            SetXfMode(palo, l.pxf, 0xF);
        }
}

// orig: 0x001604E8 (mode 15: a record follows its link's target in world space: the position (+8) and / or the
// rotation (+0xC) in its parent's frame; stamped)
void UpdateXfLink(ALO* palo, void* pv)
{
    (void)palo;
    ALOLINK* pl = static_cast<ALOLINK*>(pv);
    if (!pl || !pl->pxf) return;
    XF* rec = pl->pxf;
    const glm::mat4 matTarget = pl->pxfTarget ? pl->pxfTarget->matWorld : g_matIdentity4;  // (null: 0x2C44F0)
    const glm::mat4 matParent = rec->pxfRef ? rec->pxfRef->matWorld : g_matIdentity4;
    if (pl->n8) SetPos(&rec->mat, XformPos(InverseAffineMatrix4(matParent), PosOf(matTarget)));
    if (pl->nC)
    {
        glm::mat3 m = InverseMatrix3(Mat3Of(matParent)) * Mat3Of(matTarget);
        NormalizeRotateMatrix3(&m);
        SetMat3(&rec->mat, m);
    }
    Bump(rec);
}

namespace
{
// orig 0x0022CAD0: b rotated by the angles a.y (tilt) and a.z (pan) (the Euler rates' angular velocity)
glm::vec3 VecRotateTiltPanPhys(const glm::vec3& a, const glm::vec3& b)
{
    float s0, c0, s1, c1;
    CalculateSinCos(a.y, &s0, &c0);
    CalculateSinCos(a.z, &s1, &c1);
    return glm::vec3(c0 * c1 * b.x - s1 * b.y, c0 * s1 * b.x + c1 * b.y, -s0 * b.x + b.z);
}
// orig 0x0022CCD0: the skew (cross product) matrix of w times the 3x3 rows of m (translation zero)
glm::mat4 DmatSkewPhys(const glm::mat4& m, const glm::vec3& w)
{
    glm::mat4 o(0.0f);
    for (int i = 0; i < 3; ++i) o[i] = glm::vec4(glm::cross(w, glm::vec3(m[i])), 0.0f);
    return o;
}
// orig 0x0022CC00: v put by the kind in a zero matrix
glm::mat4 DmatOfValuePhys(const XF* pxf, const glm::vec3& v)
{
    glm::mat4 m(0.0f);
    const int k = pxf->kind;
    if (FXfKindTranslation(k)) m[3] = glm::vec4(v, 0.0f);
    else if (k == 11) { m[1][0] = v.x; m[2][0] = v.y; m[2][1] = v.z; }
    else if (k == 12) { m[0][0] = v.x; m[1][1] = v.y; m[2][2] = v.z; }
    return m;
}
}

// orig: 0x00230710 (see xfphys.h)
void PredictXf(XF* p, float dt, glm::mat4* pmat, glm::mat4* pdmat)
{
    if (CTRL* pc = p->pctrlFirst)
    {
        glm::vec4 q0(0.0f), q1(0.0f);
        pc->Query(&q0, &q1, dt);  // orig0x230758 forwards prediction dt to the controller (e.g. ASEGA time + dt*rate)
        *pmat = MatXfWithValue(p, q0);
        if (p->kind == 6 || p->kind == 7)
            *pdmat = DmatSkewPhys(*pmat, VecRotateTiltPanPhys(glm::vec3(q0), glm::vec3(q1)));  // 0x22CB90
        else
            *pdmat = DmatOfValuePhys(p, glm::vec3(q1));
        return;
    }
    if (FXfModeMoving(p->mode))
    {
        const glm::vec3 vel(p->vel);
        if (p->kind != 6 && p->kind != 7)
        {
            const glm::vec4 v = VecXfValue(p, p->mat) + p->vel * dt;
            *pmat = MatXfWithValue(p, v);
            *pdmat = DmatOfValuePhys(p, vel);
        }
        else
        {
            const glm::mat3 r = BuildRotateMatrixVector(vel * dt);
            glm::mat4 m(0.0f);
            for (int i = 0; i < 3; i++) m[i] = glm::vec4(r * glm::vec3(p->mat[i]), 0.0f);
            m[3] = glm::vec4(glm::vec3(p->mat[3]), 1.0f);
            *pmat = m;
            *pdmat = DmatSkewPhys(m, vel);
        }
        return;
    }
    *pmat = p->mat;
    *pdmat = glm::mat4(0.0f);
}

// orig: 0x00230A98 (see xfphys.h)
void PredictXfChain(XF* pxf, XF* pxfStop, float dt, glm::mat4* pmat, glm::mat4* pdmat)
{
    glm::mat4 b = pdmat ? *pdmat : glm::mat4(0.0f);
    glm::mat4 a = pmat ? *pmat : glm::mat4(1.0f);
    for (XF* p = pxf; p != pxfStop && p; p = p->pxfRef)
    {
        glm::mat4 m1, m2;
        PredictXf(p, dt, &m1, &m2);
        b = MulMatrix4(m1, b) + MulMatrix4(m2, a);  // (the original's 0x11C168(m1, b) + 0x11C168(m2, a))
        a = MulMatrix4(m1, a);
    }
    if (pmat) *pmat = a;
    if (pdmat) *pdmat = b;
}
