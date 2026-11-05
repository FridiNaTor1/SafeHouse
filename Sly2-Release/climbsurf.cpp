// climbsurf.cpp: the climbing surfaces (cid 0x09, climbsurf.h) and Sly's state 21 on them, from the matched
// 0x185110..0x1865F8 (0x185310 / 0x1856B8 / 0x1856C0 / 0x185A80 / 0x185BF8 / 0x185CE0 / 0x185D18 / 0x185E30 /
// 0x185E40) and the state 21 parts of 0x18C250.
#include "climbsurf.h"
#include "activate.h"
#include "brx.h"
#include "crv.h"
#include "jump.h"
#include <glm/gtc/matrix_transform.hpp>

#include "actor.h"
#include "actanim.h"
#include "alo.h"
#include "animfind.h"
#include "asega.h"
#include "blend.h"
#include "clock.h"
#include "cm.h"
#include "dzg.h"
#include "gang.h"
#include "interact.h"
#include "jt.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "sly.h"
#include "sm.h"
#include "so.h"
#include "update.h"
#include "vecmat.h"
#include "worldaccess.h"
#include "xf.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using cmm::F32;

glm::mat3 InverseMatrix3(const glm::mat3& mat);                             // mat.h (0x11B340)
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)

namespace
{
float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }

// orig: 0x0011EE08 (the point of the segment a-b nearest p, its fraction and distance)
glm::vec3 PosNearestSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float* pt, float* pdist)
{
    const glm::vec3 d = b - a, w = p - a;
    float t = Dot3(w, d) / Dot3(d, d);
    if (t < 0.0f) t = 0.0f;
    else if (1.0f < t) t = 1.0f;
    const glm::vec3 pos = a + d * t;
    if (pt) *pt = t;
    if (pdist) *pdist = std::sqrt(Dot3(p - pos, p - pos));
    return pos;
}

const LOXPTS* Pmesh(LO* plo) { return dynamic_cast<const LOXPTS*>(plo); }

glm::vec3 Xform(const glm::mat4& m, const glm::vec3& v) { return glm::vec3(m * glm::vec4(v, 1.0f)); }

// orig: 0x001856C0 (the planes keeping the points on the surface: along each open edge (no neighbour) of a
// triangle that has one, the edge's inward plane (the face normal x the edge) for the nearest of the points within
// radius + the smoothing's reach; its w the smoothed approach speed (0x11D018's rate) of the nearest)
void AddClimbSurfEdgePlanes(LO* plo, std::vector<glm::vec4>* pplanes, int c, const glm::vec3* apos, const cmm::SMP* psmp,
                            float radius)
{
    const LOXPTS* pm = Pmesh(plo);
    if (!pm) return;
    const glm::mat4 m = MatJtLo(plo);  // vt slot 31
    const float h = (psmp->svFast + psmp->svSlow) * 0.5f;
    const float step = g_clock.dt * 0.5f;
    const float stop = psmp->dtFast * h;
    for (size_t i = 0; i < pm->ax1.size(); ++i)
    {
        const auto& an = pm->ax2[i];
        if (an[0] != -1 && an[1] != -1 && an[2] != -1) continue;
        const auto& t = pm->ax1[i];
        glm::vec3 v[3];
        for (int k = 0; k < 3; ++k) v[k] = Xform(m, pm->avec[t[k]]);
        const glm::vec3 normal = glm::cross(v[1] - v[0], v[2] - v[0]);
        for (int edge = 0; edge < 3; ++edge)
        {
            if (an[edge] != -1) continue;
            const glm::vec3 start = v[(edge + 1) % 3], end = v[(edge + 2) % 3];
            float best = FLT_MAX;
            glm::vec4 plane(0.0f);
            for (int n = 0; n < c; ++n)
            {
                const glm::vec3 point = apos[n];
                float dist = 0.0f;
                const glm::vec3 closest = PosNearestSegment(point, start, end, nullptr, &dist);
                if (!(dist < radius + stop)) continue;
                const glm::vec3 x = glm::cross(normal, end - start);
                const glm::vec3 unit = x * (1.0f / std::sqrt(Dot3(x, x)));
                float clamped = 0.0f;
                cmm::GSmooth(radius, Dot3(unit, point - closest), step, psmp, &clamped);  // 0x11D018
                if (clamped < best)
                {
                    best = clamped;
                    plane = glm::vec4(unit, best);  // 0x1193C0
                }
            }
            if (best != FLT_MAX) pplanes->push_back(plane);
        }
    }
}

// orig: 0x00185A80 (the velocity v moved by the least multiples of the planes (an LCP, 0x11AD30) that bring each
// plane's speed w + n.v to at least 0)
glm::vec3 VecProjectClimbSurfPlanes(const std::vector<glm::vec4>& aplane, glm::vec3 v)
{
    const int n = static_cast<int>(aplane.size());
    if (n == 0) return v;
    std::vector<float> aa(static_cast<size_t>(n * n)), rhs(static_cast<size_t>(n)), aw(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) aa[static_cast<size_t>(i * n + j)] = Dot3(glm::vec3(aplane[i]), glm::vec3(aplane[j]));
    for (int i = 0; i < n; ++i) rhs[static_cast<size_t>(i)] = aplane[i].w + Dot3(v, glm::vec3(aplane[i]));
    SolveDzgConstraints(n, aa.data(), rhs.data(), aw.data(), FLT_MAX);  // (D_002A568C)
    for (int i = 0; i < n; ++i) v = v + glm::vec3(aplane[i]) * aw[static_cast<size_t>(i)];
    return v;
}

const cmm::SMP c_smpClimbRot{25.0f, 0.0f, 0.25f};   // D_002BE920
const cmm::SMP c_smpClimbPos{1000.0f, 0.0f, 0.25f}; // D_002BE930
const cmm::SMP c_smpClimbEdge{250.0f, 0.0f, 0.5f};  // D_002BE940

struct INITCLIMBSURF
{
    INITCLIMBSURF()
    {
        // method 771 (thunk 0x10B618 -> 0x185E30): Sly's three points of the surface
        RegisterObjectMethod(0x10B618, [](LO* plo, const std::vector<CRef>& a) {
            if (SLY* p = PslyFromLo(plo))
                for (size_t i = 0; i < 3 && i < a.size(); ++i)
                    p->aposSurface[i] = glm::vec4(a[i].m_tagk == TAGK_Vector ? a[i].GetVector() : glm::vec3(0.0f), 0.0f);
            return CRef(TAGK_Void);
        });
        // thunk 0x10B6D0 -> 0x18DF58: Sly's hand below a hook (+0x1FA0), e.g. (60 0 50)
        RegisterObjectMethod(0x10B6D0, [](LO* plo, const std::vector<CRef>& a) {
            if (SLY* p = PslyFromLo(plo); p && !a.empty() && a[0].m_tagk == TAGK_Vector) p->vecHangOffset = a[0].GetVector();
            return CRef(TAGK_Void);
        });
    }
} s_initclimbsurf;
}  // namespace

// orig: 0x00185310
void FindClimbSurfClosestPoint(LO* plo, const glm::vec3& pos, SURFLOC* psl, glm::vec3* ppos, glm::vec3* pnormal)
{
    const LOXPTS* pm = Pmesh(plo);
    static const bool s_fDump = std::getenv("SLY2N_SURFDUMP") != nullptr;
    if (s_fDump && pm)
    {
        static std::vector<LO*> s_aplo;
        if (std::find(s_aplo.begin(), s_aplo.end(), plo) == s_aplo.end())
        {
            s_aplo.push_back(plo);
            const glm::mat4 m = MatJtLo(plo);
            glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
            for (const glm::vec3& v : pm->avec) { mn = glm::min(mn, Xform(m, v)); mx = glm::max(mx, Xform(m, v)); }
            std::printf("climbsurf %d: %zu points %zu triangles, (%.0f %.0f %.0f) .. (%.0f %.0f %.0f)\n", plo->oid, pm->avec.size(),
                        pm->ax1.size(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
        }
    }
    SURFLOC best;
    glm::vec3 posBest(0.0f), normalBest(0.0f);
    if (pm)
    {
        const glm::mat4 m = MatJtLo(plo);  // vt slot 31
        float dBest = FLT_MAX;  // D_002A5684
        for (size_t i = 0; i < pm->ax1.size(); ++i)
        {
            const auto& t = pm->ax1[i];
            const glm::vec3 a = Xform(m, pm->avec[t[0]]), b = Xform(m, pm->avec[t[1]]), c = Xform(m, pm->avec[t[2]]);
            // the barycentric place: pos = c + u (a - c) + v (b - c) in the face's plane
            const glm::vec3 eA = a - c, eB = b - c;
            const glm::vec3 normal = glm::cross(eA, eB);
            const glm::vec3 d = pos - c;
            const glm::vec3 nB = glm::cross(normal, eB);
            const float den = 1.0f / Dot3(eA, nB);
            const float u = den * Dot3(d, nB);
            const float v = den * Dot3(normal, glm::cross(d, eA));
            SURFLOC cand;
            glm::vec3 closest;
            float g = 0.0f;
            if (u < 0.0f)
            {
                closest = PosNearestSegment(pos, b, c, &g, nullptr);
                cand.u = 0.0f;
                cand.v = 1.0f - g;
            }
            else if (v < 0.0f)
            {
                closest = PosNearestSegment(pos, a, c, &g, nullptr);
                cand.u = 1.0f - g;
                cand.v = 0.0f;
            }
            else if (1.0f < u + v)
            {
                closest = PosNearestSegment(pos, a, b, &g, nullptr);
                cand.u = 1.0f - g;
                cand.v = g;
            }
            else
            {
                cand.u = u;
                cand.v = v;
                closest = a * u + b * v + c * ((1.0f - u) - v);
            }
            const glm::vec3 r = closest - pos;
            const float dist = Dot3(r, r);
            if (dist < dBest)
            {
                cand.itri = static_cast<int>(i);
                dBest = dist;
                best = cand;
                normalBest = normal;
                posBest = closest;
            }
        }
    }
    if (psl) *psl = best;
    if (ppos) *ppos = posBest;
    if (pnormal) *pnormal = normalBest * (1.0f / std::sqrt(Dot3(normalBest, normalBest)));
}

// orig: 0x00185D18
void ClimbSurfLanding(LO* plo, const glm::vec3& pos, const glm::vec3& vel, const glm::vec3& acc, float sOffset, float t,
                      SURFLOC* psl, glm::vec3* ppos, glm::vec3* pvel)
{
    SURFLOC sl;
    glm::vec3 closest(0.0f), normal(0.0f);
    FindClimbSurfClosestPoint(plo, pos + vel * t + acc * ((t * 0.5f) * t), &sl, &closest, &normal);
    if (psl) *psl = sl;
    if (ppos) *ppos = closest + normal * sOffset;
    if (pvel) *pvel = vel + acc * t + normal * F32(0xC3960000);  // D_002BE918: -300
}

float SHeightClimbSurf(LO* plo)
{
    const uint32_t u = plo ? plo->U32Field(0, 0x108, 0x7F7FFFFFu) : 0x7F7FFFFFu;  // 0x185BF8: FLT_MAX
    return std::bit_cast<float>(u);
}

namespace
{
LO* PloClimbSurfZone(LO* plo) { return plo && plo->U32Field(0, 0x110, 0) ? PloFindSwNearest(plo, static_cast<OID>(plo->U32Field(0, 0x110, 0))) : nullptr; }
}

// orig: 0x0018C250 (entering state 21)
void EnterSlyClimbSurf(SLY* p)
{
    SURFLOC sl;
    FindClimbSurfClosestPoint(p->ploSurface, PosAloCenter(p->palo), &sl, nullptr, nullptr);  // 0x185310
    p->itriSurface = sl.itri;
    p->uSurface = sl.u;
    p->vSurface = sl.v;
    SetAloCnstrLin(p->palo, 1);  // 0x1F5720
    SetAloVelocityTo(p->palo, glm::vec3(0.0f));
    SetAloAngVelocityTo(p->palo, glm::vec3(0.0f));
    p->vecVel = glm::vec3(0.0f);
    if (p->ploSurface)
    {
        HandleLoSpliceEvent(p->ploSurface, 5, false, 0, nullptr);
        if (LO* pzone = PloClimbSurfZone(p->ploSurface)) SetActorZone(p, pzone);  // 0x140C88
    }
}

// orig: 0x0018C250 (leaving state 21)
void LeaveSlyClimbSurf(SLY* p)
{
    SetAloCnstrLin(p->palo, 0);
    if (p->ploSurface)
    {
        HandleLoSpliceEvent(p->ploSurface, 6, false, 0, nullptr);
        if (LO* pzone = PloClimbSurfZone(p->ploSurface); pzone && p->plo1024 == pzone) SetActorZone(p, nullptr);
    }
    p->ploSurface = nullptr;
}

// orig: 0x00185E40
void UpdateSlyClimbSurf(SLY* p)
{
    ALO* palo = p->palo;
    LO* pmesh = p->ploSurface;
    if (!pmesh) return;
    const glm::vec3 position = PosAloCenter(palo);  // 0x1A62E8
    SURFLOC sl{p->itriSurface, p->uSurface, p->vSurface};
    glm::vec3 closest(0.0f), normal(0.0f);
    FindClimbSurfClosestPoint(pmesh, position, &sl, &closest, &normal);
    p->itriSurface = sl.itri;
    p->uSurface = sl.u;
    p->vSurface = sl.v;
    // the frame facing the surface at the hold distance (+0x1F30's x) from it
    const glm::vec3 difference = closest - position;
    const float len = std::sqrt(Dot3(difference, difference));
    const glm::vec3 direction = len < F32(0x38D1B717) ? normal * -1.0f : difference * (1.0f / len);
    const glm::vec3 target = closest + direction * -p->aposSurface[0].x;
    const glm::vec3 up(0.0f, 0.0f, 1.0f);  // D_002A2DC0
    glm::mat4 frame(BuildLookAtMatrix(direction, up));  // 0x11B830
    frame[3] = glm::vec4(target, 1.0f);
    // the surface's plane under the three points: the basis (+0x1B0) faces it
    glm::vec3 contacts[3];
    for (int i = 0; i < 3; ++i)
        FindClimbSurfClosestPoint(pmesh, glm::vec3(frame * glm::vec4(glm::vec3(p->aposSurface[i]), 1.0f)), nullptr, &contacts[i],
                                  nullptr);
    const glm::vec3 surface = glm::cross(contacts[2] - contacts[0], contacts[1] - contacts[0]);
    const glm::vec3 unit = surface * (1.0f / std::sqrt(Dot3(surface, surface)));
    p->matHeading = BuildLookAtMatrix(unit, up);
    glm::vec3 angular(0.0f), linear(0.0f);
    SmoothMatrix(glm::mat3(MatJtLo(palo)), p->matHeading, &c_smpClimbRot, g_clock.dt * 0.5f, nullptr, &angular);  // 0x11C950
    SetAloAngVelocityTo(palo, angular);  // vt slot 36
    cmm::PosSmooth(position, target, g_clock.dt * 0.5f, &c_smpClimbPos, &linear);  // 0x11F088
    glm::vec3 samples[3];
    for (int i = 0; i < 3; ++i) samples[i] = target + p->matHeading * glm::vec3(p->aposSurface[i]);
    std::vector<glm::vec4> aplane;  // 0x1856B8
    AddClimbSurfEdgePlanes(pmesh, &aplane, 3, samples, &c_smpClimbEdge, 10.0f);
    // the stick in the surface's frame (y left / z up, by the camera), at the blend's two directions
    PODATA* ppd = PpodataFromLo(palo);
    PADCTL* pc = ppd ? ppd->ppadctl : nullptr;
    const float speed = pc ? pc->uJoy : 0.0f;
    if (0.0f < speed)
    {
        glm::vec3 movement(0.0f, -pc->x, pc->y);
        const glm::mat3 rotation = InverseMatrix3(p->matHeading) * g_cm.mat;  // (D_002DDFA0)
        movement = rotation * movement;
        movement.x = 0.0f;
        const float l = std::sqrt(Dot3(movement, movement));
        const glm::vec3 dir = l < F32(0x38D1B717) ? up : movement * (1.0f / l);
        movement = dir * speed;
        p->vecVel = glm::vec3(p->rb1E10.avel[1]) * movement.y + glm::vec3(p->rb1E10.avel[2]) * movement.z;
        p->vecVel = p->vecVel * (p->rb1E10.pbl ? p->rb1E10.pbl->svt : 1.0f);  // (+0x1F10's +0x78)
    }
    else p->vecVel = glm::vec3(0.0f);
    if (!aplane.empty())
    {
        const glm::vec3 projected = VecProjectClimbSurfPlanes(aplane, p->matHeading * (p->vecVel * -1.0f));  // 0x185A80
        p->vecVel = InverseMatrix3(p->matHeading) * (projected * -1.0f);
    }
    bool fActive = false;
    if (1.0f < Dot3(p->vecVel, p->vecVel)) fActive = true;
    else if (speed != 0.0f) fActive = true;
    if (fActive)
    {
        SetSlyState(p, 21, 11);
        SteerActorRootBlend(p, &p->rb1E10);  // 0x1A0450
        SetAloVelocityTo(palo, linear + p->matHeading * (p->vecVel * -1.0f));  // vt slot 35
    }
    else
    {
        SetSlyState(p, 21, 10);
        SetAloVelocityTo(palo, linear);
    }
    static const bool s_fPrint = std::getenv("SLY2N_CLIMBPRINT") != nullptr;
    if (s_fPrint)
        std::printf("climbsurf t %.3f: %d tri %d (%.3f %.3f) pos %.1f %.1f %.1f target %.1f %.1f %.1f normal %.3f %.3f %.3f "
                    "vel %.1f %.1f %.1f planes %zu sub %d\n",
                    g_clock.t, pmesh->oid, sl.itri, sl.u, sl.v, position.x, position.y, position.z, target.x, target.y, target.z,
                    unit.x, unit.y, unit.z, p->vecVel.x, p->vecVel.y, p->vecVel.z, aplane.size(), p->substate);
    if (s_fPrint && pc)
        std::printf("   stick %.3f %.3f u %.3f cam x %.3f %.3f %.3f y %.3f %.3f %.3f z %.3f %.3f %.3f svt %.3f/%.3f\n", pc->x, pc->y, pc->uJoy,
                    g_cm.mat[0].x, g_cm.mat[0].y, g_cm.mat[0].z, g_cm.mat[1].x, g_cm.mat[1].y, g_cm.mat[1].z, g_cm.mat[2].x,
                    g_cm.mat[2].y, g_cm.mat[2].z, p->rb1E10.pbl ? p->rb1E10.pbl->svt : -1.0f, p->pasega ? p->pasega->svt : -1.0f);
}

// ---- Sly's state 22 on a hook of kind 3 (+0x124): the hang-follow (the Ep7 wall-climb hooks) ----

namespace
{
const cmm::SMP c_smpHangRot{25.0f, 0.0f, 0.25f};   // D_002BF3F8
const cmm::SMP c_smpHangPos{1000.0f, 0.0f, 0.25f}; // D_002BF408
}

// orig: 0x0018C250 (entering state 22): the blend, no mode, the linear constraint on (0x1F5720(1)), still, the hook's
// splice event 5, its camera zone
void EnterSlyHangFollow(SLY* p)
{
    SetAloCnstrLin(p->palo, 1);  // 0x1F5720
    SetAloVelocityTo(p->palo, glm::vec3(0.0f));
    SetAloAngVelocityTo(p->palo, glm::vec3(0.0f));
    p->vecVel = glm::vec3(0.0f);
    if (p->ploOther)
    {
        HandleLoSpliceEvent(p->ploOther, 5, false, 0, nullptr);
        SetActorZone(p, PloJtDescZone(p->ploOther, JTL_Point));  // (the hook's +0x100: SetPlayerTn)
    }
}

// orig: 0x0018C250 (leaving state 22)
void LeaveSlyHangFollow(SLY* p)
{
    SetAloCnstrLin(p->palo, 0);
    if (p->ploOther) HandleLoSpliceEvent(p->ploOther, 6, false, 0, nullptr);
    p->ploOther = nullptr;
    SetActorZone(p, nullptr);
}

// orig: 0x0018DF60 (state 22 each frame): turned to the hook's frame and pulled to the hook's point less +0x1FA0 (both
// smoothed over half the frame); the stick in the hook's frame (y across): the blend's weights left / middle / right by
// its side, base by its size; +0x1F94 the side, +0x1F98 the size
void UpdateSlyHangFollow(SLY* p)
{
    LO* phook = p->ploOther;
    if (!phook) return;
    ALO* palo = p->palo;
    const glm::mat4 m = MatJtLo(phook);  // vt slot 31
    p->matHeading = glm::mat3(m);
    glm::vec3 angular(0.0f), linear(0.0f);
    SmoothMatrix(glm::mat3(MatJtLo(palo)), p->matHeading, &c_smpHangRot, g_clock.dt * 0.5f, nullptr, &angular);  // 0x11C950
    SetAloAngVelocityTo(palo, angular);  // vt slot 36
    const glm::vec3 target = glm::vec3(m * glm::vec4(-p->vecHangOffset, 1.0f));
    cmm::PosSmooth(PosAloCenter(palo), target, g_clock.dt * 0.5f, &c_smpHangPos, &linear);  // 0x11F088
    SetAloVelocityTo(palo, linear);  // vt slot 35
    PODATA* ppd = PpodataFromLo(palo);
    PADCTL* pc = ppd ? ppd->ppadctl : nullptr;
    const glm::vec3 local(0.0f, pc ? -pc->x : 0.0f, pc ? pc->y : 0.0f);
    const glm::vec3 world = g_cm.mat * local;                        // (D_002DDFA0's rows)
    const glm::vec3 rel = glm::transpose(glm::mat3(m)) * world;      // 0x1A6FC0 (into the hook's frame)
    const float l = std::sqrt(Dot3(rel, rel));
    const glm::vec3 n = l < F32(0x38D1B717) ? glm::vec3(0.0f, 0.0f, -1.0f) : rel * (1.0f / l);
    const float amount = n.y;
    const float right = std::max(amount, 0.0f);
    const float left = std::max(-amount, 0.0f);
    const float middle = 1.0f - right - left;
    const float w = pc ? pc->uJoy : 0.0f;
    p->gHangAmount = amount;
    p->gHangWeight = w;
    if (p->pblHang)
    {
        SetBlendWeights(p->pblHang, {1.0f - w, left * w, middle * w, right * w});  // 0x1384C0
    }
}

namespace
{
// orig: 0x001D6848 (the launch velocity from start to end under acc: the angle k between the line's elevation (at
// least 0) and straight up, blended by sin g; the speed that reaches end on that angle; zero when none does)
glm::vec3 VecBallisticVelocity(float g, const glm::vec3& start, const glm::vec3& end, const glm::vec3& acc)
{
    float k = std::sin(g);
    glm::vec3 delta = end - start;
    const float z = delta.z;
    delta.z = 0.0f;
    const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    float angle = std::atan2(z, len);
    if (angle < 0.0f) angle = 0.0f;
    k = angle + (1.57079637f - angle) * k;
    float q = acc.z / ((z - len * std::tan(k)) * 2.0f);
    if (q <= 0.0f) return glm::vec3(0.0f);
    q = std::sqrt(q);
    const float speed = len * q / std::cos(k);
    glm::vec3 vel = delta;
    vel.z = len * std::tan(k);
    const float l = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    return vel * (speed / l);
}
}  // namespace

// orig: 0x0018A8C8 (state 22's cross): the jump toward the hook's target (+0x138: a curve (cid 10): its point at
// s = length (0.5 - 0.5 side); else its point (or (-50 0 300)) in the hook's frame turned by side * 0.5 about x),
// the ballistic velocity 0x1D6848 (angle blend 0.5), scaled by the stick's size; the jump kind 0xC with that velocity flattened as its air target
bool FSlyHangFollowJump(SLY* p)
{
    LO* phook = p->ploOther;
    if (!phook) return false;
    LO* ptarget = phook->U32Field(0, 0x138, 0) ? PloFindSwNearest(phook, static_cast<OID>(phook->U32Field(0, 0x138, 0))) : nullptr;
    glm::vec3 vTarget(0.0f);
    if (ptarget && FIsLoDerivedFrom(ptarget, 10))
    {
        if (const CRV* pcrv = PcrvFromLo(ptarget)) EvalCrvS(pcrv, SLengthCrv(pcrv) * (0.5f - p->gHangAmount * 0.5f), &vTarget, nullptr);
    }
    else
    {
        const glm::mat4 m = MatJtLo(phook);
        glm::vec3 vLocal = ptarget ? glm::vec3(glm::inverse(m) * glm::vec4(glm::vec3(MatJtLo(ptarget)[3]), 1.0f))
                                   : glm::vec3(-50.0f, 0.0f, 300.0f);  // (D_002BF370)
        const glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.0f), p->gHangAmount * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f)));  // 0x2BF364
        vTarget = glm::vec3(m * glm::vec4(r * vLocal, 1.0f));
    }
    const glm::vec3 from = PosLoWorld(p->palo);
    const glm::vec3 to = vTarget - glm::vec3(p->vecBase);
    const SOST* ps = Psost(p->palo);
    const glm::vec3 g = ps ? ps->dvGravity : glm::vec3(0.0f, 0.0f, -1750.0f);
    const glm::vec3 v = VecBallisticVelocity(0.5f, from, to, g);  // 0x1D6848 (0.5: the launch angle's blend, not a time)
    if (std::getenv("SLY2N_HANGPRINT"))
        if (ptarget) { const glm::mat4 mh = MatJtLo(phook); const glm::vec3 pt = glm::vec3(MatJtLo(ptarget)[3]);
            std::printf("hangfollow: hook pos %.1f %.1f %.1f x %.3f %.3f %.3f y %.3f %.3f %.3f z %.3f %.3f %.3f target pos %.1f %.1f %.1f base %.1f %.1f %.1f\n",
                        mh[3].x, mh[3].y, mh[3].z, mh[0].x, mh[0].y, mh[0].z, mh[1].x, mh[1].y, mh[1].z, mh[2].x, mh[2].y, mh[2].z, pt.x, pt.y, pt.z,
                        p->vecBase.x, p->vecBase.y, p->vecBase.z); }
        std::printf("hangfollow: target obj %d curve %d len %.2f\n", ptarget ? ptarget->oid : -1, ptarget && FIsLoDerivedFrom(ptarget, 10) ? 1 : 0,
                    ptarget && FIsLoDerivedFrom(ptarget, 10) && PcrvFromLo(ptarget) ? SLengthCrv(PcrvFromLo(ptarget)) : 0.0f),
        std::printf("hangfollow: jump t %.3f hook %d amount %.4f weight %.4f target %.1f %.1f %.1f from %.1f %.1f %.1f v %.1f %.1f %.1f\n", g_clock.t, phook->oid,
                    p->gHangAmount, p->gHangWeight, vTarget.x, vTarget.y, vTarget.z, from.x, from.y, from.z, v.x, v.y, v.z);
    SetAloVelocityTo(p->palo, v * p->gHangWeight);
    SetJumpKind(&p->jump, 0xC, 0, 0);
    p->jump.vecA0 = glm::vec3(v.x, v.y, 0.0f);  // (QwProjectOut(v, z))
    return true;
}
