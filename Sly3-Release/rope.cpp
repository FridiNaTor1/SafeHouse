// rope.cpp: a path's rope simulation and the meshes bent along a path (the original's module 0x1E7840..0x1E9F50).
// A path (class 0xA family, e.g. a rail 0xD) gets a simulation (+0x17C, 0x90 bytes) the first time a script sets one of
// its options 1042..1056: two ends (objects' points, else the path's own ends), a force (gravity), the points' positions /
// velocities / rest distances. Its update (vt slot 17, 0x1E6340) by its mode: 1 / 2 the trail (0x1E9090: the points
// pulled after the first end; mode 2 pins the last one to the second end and lengthens the path as the ends separate,
// 0x1E95B0), 0 / 3 the spring (0x1E8378: four weighted passes of drag, stretch, bend and shape forces; mode 3 re-samples
// the path along the old polyline at its rest steps). The points go back into the path (its curve: crv.h), so what reads
// the path (a rail Sly slides on, the camera) follows. A class 0x6A object (0x1E9858 / 0x1E9AC0) draws its model bent
// along a path: each vertex kept as its offset from the path's nearest point at load and that point's parameter.
// E.g. the Ep1 heist harpoon's rope (world 2: path 42820, the mesh 42818, block 123952) and Ep7's boat grapples.
#include "jt.h"
#include "game/sly3.h"
#include "rope.h"
#include "spliceobj.h"

#include "alo.h"
#include "brx.h"
#include "worldaccess.h"
#include "clock.h"
#include "crv.h"
#include "glob.h"
#include "lo.h"
#include "objmethod.h"
#include "render.h"
#include "scene.h"
#include "so.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "xf.h"
#include "xfphys.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

extern SW* g_psw;
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)
LO* PloFindSwNearest(LO* ploCtx, OID oid, bool fSameChannel);                // animfind.h

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_ROPEPRINT") != nullptr;
    return s_f;
}

// The simulation (orig 0x90 bytes; field offsets as the original's)
struct ROPE
{
    glm::vec3 force{0.0f, 0.0f, -1750.0f};  // +0x00 (option 1045)
    LO* apo[2]{};                            // +0x10 / +0x14 the ends' objects (1042 / 1043)
    glm::vec3 apos[2]{};                     // +0x20 / +0x30 the ends' points in their records
    XF* apxf[2]{};                           // +0x40 / +0x44 the ends' records (null: the world, D_002C44F0)
    LO* ppath = nullptr;                     // +0x48
    int mode = -1;                           // +0x4C (1046)
    std::vector<glm::vec3> apos54;           // +0x54 the points (world)
    std::vector<glm::vec3> avel58;           // +0x58 their velocities
    std::vector<float> as5C;                 // +0x5C their rest distances along the rope
    float damping = 3.0f;                    // +0x60 (1051)
    float stretch = 0.0f;                    // +0x64 (1047)
    float stretchDamping = 0.0f;             // +0x68 (1052)
    float bend = 500.0f;                     // +0x6C (1048)
    float bendDamping = 5.0f;                // +0x70 (1053)
    float shape = 0.0f;                      // +0x74 (1049)
    float shapeDamping = 0.0f;               // +0x78 (1054)
    float g7C = 0.0f;                        // +0x7C (1050)
    float g80 = 0.0f;                        // +0x80 (1055)
    int cMax = 0;                            // +0x84 the capacity (the loaded path's points)
    float sStep = 300.0f;                    // +0x88 (1056) the trail's point spacing
    std::vector<float> agLoaded;             // (native) the path's loaded parameters, by point
};

std::unordered_map<const LO*, std::unique_ptr<ROPE>> s_mprope;

CRV* PcrvMutable(LO* ppath) { return const_cast<CRV*>(PcrvFromLo(ppath)); }

glm::vec3 PosXf(const XF* pxf, const glm::vec3& v) { return pxf ? glm::vec3(pxf->matWorld * glm::vec4(v, 1.0f)) : v; }

// the curve's point count changed (the original's +0xE8): the native vectors follow
void SetCrvCount(ROPE& r, CRV* pc, int c)
{
    c = std::max(c, 2);
    pc->apos.resize(c, pc->apos.empty() ? glm::vec3(0.0f) : pc->apos.back());
    pc->as.resize(c, pc->as.empty() ? 0.0f : pc->as.back());
    pc->ag.resize(c);
    for (int i = 0; i < c; ++i) pc->ag[i] = i < static_cast<int>(r.agLoaded.size()) ? r.agLoaded[i] : static_cast<float>(i);
}

// orig: 0x001E6330 (the curve's cached segment dropped)
void RefreshCrv(CRV* pc)
{
    pc->iseg = 0;
    pc->isegCached = -1;
}

// orig: 0x001E9828 (the rest distances from the path's)
void CopyRopeDistances(ROPE& r, const CRV* pc)
{
    for (size_t i = 0; i < pc->as.size() && i < r.as5C.size(); ++i) r.as5C[i] = pc->as[i];
}

// orig: 0x001E81C0
void InitRope(ROPE& r, LO* ppath, CRV* pc)
{
    r.ppath = ppath;
    r.mode = -1;
    const int c = static_cast<int>(pc->apos.size());
    r.apos54.assign(c, glm::vec3(0.0f));
    r.avel58.assign(c, glm::vec3(0.0f));
    r.as5C.assign(c, 0.0f);
    r.force = glm::vec3(0.0f, 0.0f, -1750.0f);
    r.damping = 3.0f;
    r.bend = 500.0f;
    r.bendDamping = 5.0f;
    r.cMax = c;
    r.sStep = 300.0f;
    r.agLoaded = pc->ag;
    const glm::mat4 m = MatWorldLox(ppath);  // vt slot 31
    for (int i = 0; i < c; ++i) r.apos54[i] = glm::vec3(m * glm::vec4(pc->apos[i], 1.0f));
    r.apxf[0] = r.apxf[1] = nullptr;
    if (c > 0)
    {
        r.apos[0] = r.apos54[0];
        r.apos[1] = r.apos54[c - 1];
    }
    CopyRopeDistances(r, pc);  // 0x1E9828
}

void MarkRopeUpdate(LO* ppath)
{
    // (the path's update flags (+0x40): 0x20 off, 4 / 8 on: its slot 17 runs in the physics frame's loop)
    if (ppath->iupd >= 0 && g_psw && ppath->iupd < static_cast<int>(g_psw->aupd.size()))
        g_psw->aupd[static_cast<size_t>(ppath->iupd)].grf = (g_psw->aupd[static_cast<size_t>(ppath->iupd)].grf & ~0x20u) | 0xCu;
    ppath->grfUpd = (ppath->grfUpd & ~0x20u) | 0xCu;
}

// orig: 0x001E7840 (the simulation, made on first use: then the path's distances (0x1E6180) and its update on; f 1: the
// update flag 4 again)
ROPE* PropeEnsure(LO* ppath, int f)
{
    if (!ppath || !FIsLoDerivedFrom(ppath, S3CID(0x0A))) return nullptr;
    CRV* pc = PcrvMutable(ppath);
    if (!pc) return nullptr;
    auto& up = s_mprope[ppath];
    if (!up)
    {
        up = std::make_unique<ROPE>();
        InitRope(*up, ppath, pc);
        RecalcCrv(pc, MatWorldLox(ppath));  // 0x1E6180
        CopyRopeDistances(*up, pc);         // (its end: 0x1E9828)
        MarkRopeUpdate(ppath);              // (0x1A6CA8(p, 0): TODO the +0x28 bits 52..55)
        if (FPrint()) std::printf("rope %d: made, %d points, length %.1f\n", ppath->oid, up->cMax, pc->as.empty() ? 0.0f : pc->as.back());
    }
    else if (f == 1)
        MarkRopeUpdate(ppath);
    return up.get();
}

// orig: 0x001E93F0 (end i: an object's point in its record (vt slot 32), else the path's own end there)
void SetRopeEnd(ROPE& r, int i, LO* po)
{
    r.apo[i] = po;
    if (po)
    {
        if (FIsLoDerivedFrom(po, S3CID(0x30)))
        {
            ALO* palo = static_cast<ALO*>(po);
            r.apxf[i] = PxfObj(palo);
            r.apos[i] = glm::vec3(0.0f);
        }
        else if (FIsLoDerivedFrom(po, S3CID(3)))
        {
            const LOX* plox = static_cast<const LOX*>(po);
            r.apxf[i] = plox->pxf;
            r.apos[i] = glm::vec3(plox->mat[3]);
        }
        else
        {
            r.apxf[i] = nullptr;
            r.apos[i] = glm::vec3(0.0f);
        }
    }
    else
    {
        r.apxf[i] = nullptr;
        const CRV* pc = PcrvFromLo(r.ppath);
        if (pc && !pc->ag.empty()) EvalCrv(pc, i == 0 ? pc->as.front() : pc->ag.back(), &r.apos[i], nullptr);  // 0x1E6610 / 0x1E6630
    }
    if (FPrint()) std::printf("rope %d: end %d = %d at %.1f %.1f %.1f\n", r.ppath->oid, i, po ? po->oid : -1, r.apos[i].x, r.apos[i].y, r.apos[i].z);
}

// orig: 0x001E95B0 (the rope's length set to s: shorter: the points beyond it dropped; longer: points added every sStep
// (interpolated onto the old last segment) until s is within 1.5 steps of the last, up to the capacity (else the
// simulation's mode 0))
void SetRopeLength(ROPE& r, float s)
{
    CRV* pc = PcrvMutable(r.ppath);
    if (!pc) return;
    const int count = static_cast<int>(pc->apos.size());
    if (count < 2) return;
    int n;
    if (s < r.as5C[count - 1])
    {
        int i = count;
        for (;;)
        {
            --i;
            if (i < 0) break;
            if (r.as5C[i] < s) break;
        }
        i++;
        SetCrvCount(r, pc, i + 1);
        r.as5C[i] = s;
        RefreshCrv(pc);
        return;
    }
    n = count;
    if (s < r.as5C[count - 2] + 1.5f * r.sStep)  // D_002C3678
    {
        r.as5C[count - 1] = s;
    }
    else
        for (;;)
        {
            if (n < r.cMax)
            {
                n++;
                SetCrvCount(r, pc, n);
                r.apos54[n - 1] = r.apos54[n - 2];
                r.avel58[n - 1] = r.avel58[n - 2];
                r.as5C[n - 1] = s;
                pc->apos[n - 1] = pc->apos[n - 2];
                pc->as[n - 1] = pc->as[n - 2];
                r.as5C[n - 2] = r.as5C[n - 3] + r.sStep;
                const float f = (r.as5C[n - 2] - r.as5C[n - 3]) / (r.as5C[n - 1] - r.as5C[n - 3]);
                const float cf = 1.0f - f;
                r.apos54[n - 2] = r.apos54[n - 3] * cf + r.apos54[n - 1] * f;
                r.avel58[n - 2] = r.avel58[n - 3] * cf + r.avel58[n - 1] * f;
                pc->apos[n - 2] = pc->apos[n - 3] * cf + pc->apos[n - 1] * f;
                pc->as[n - 2] = cf * pc->as[n - 3] + f * pc->as[n - 1];
                if (s < r.as5C[n - 2] + 1.5f * r.sStep)
                {
                    r.as5C[n - 1] = s;
                    break;
                }
            }
            else
            {
                r.mode = 0;
                break;
            }
        }
    SetCrvCount(r, pc, n);
    RefreshCrv(pc);
}

glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& fallback, float* plen)
{
    const float l = std::sqrt(glm::dot(v, v));
    if (plen) *plen = l;
    return l < 0.0001f ? fallback : v * (1.0f / l);
}

// orig: 0x001E9090 (the trail)
void StepRopeTrail(ROPE& r)
{
    CRV* pc = PcrvMutable(r.ppath);
    if (!pc) return;
    const float dt = g_clock.dt;
    int n = static_cast<int>(pc->apos.size());
    glm::vec3 prev = PosXf(r.apxf[0], r.apos[0]);
    r.apos54[0] = prev;
    glm::vec3 prevVel = r.apxf[0] ? VelXfPoint(r.apxf[0], nullptr, prev) : glm::vec3(0.0f);  // 0x22D348
    r.avel58[0] = prevVel;
    for (int i = 1; i < n; ++i)
    {
        const glm::vec3 accel = r.force + r.avel58[i] * -r.damping;
        const glm::vec3 vel = r.avel58[i] + accel * dt;
        const glm::vec3 pos = r.apos54[i] + r.avel58[i] * (dt * 0.5f) + vel * (dt * 0.5f);
        const glm::vec3 dir = NormalizeOr(pos - prev, glm::vec3(1.0f, 0.0f, 0.0f), nullptr);  // D_002A2DA0
        const float proj = -glm::dot(vel - prevVel, dir);
        const float d = std::fabs(r.as5C[i] - r.as5C[i - 1]);
        prev = prev + dir * d;
        prevVel = vel + dir * proj;
        r.apos54[i] = prev;
        r.avel58[i] = prevVel;
    }
    if (r.mode == 2)
    {
        r.apos54[n - 1] = PosXf(r.apxf[1], r.apos[1]);
        SetRopeLength(r, r.as5C[n - 2] + glm::length(r.apos54[n - 2] - r.apos54[n - 1]));  // 0x1E95B0
        n = static_cast<int>(pc->apos.size());
    }
    const glm::mat4 inv = glm::inverse(MatWorldLox(r.ppath));  // 0x11C318 of slot 31
    for (int i = 0; i < n; ++i) pc->apos[i] = glm::vec3(inv * glm::vec4(r.apos54[i], 1.0f));
    RefreshCrv(pc);
}

// orig: 0x001E8378 (the spring; from the decomp's near-matched C, build/hand_wip/func_001E8378.lilly9.cpp, and the asm)
void StepRopeSpring(ROPE& r)
{
    CRV* pc = PcrvMutable(r.ppath);
    if (!pc) return;
    int count = static_cast<int>(pc->apos.size());
    const glm::vec3 start = PosXf(r.apxf[0], r.apos[0]);
    const glm::vec3 startVel = r.apxf[0] ? VelXfPoint(r.apxf[0], nullptr, start) : glm::vec3(0.0f);
    const glm::vec3 end = PosXf(r.apxf[1], r.apos[1]);
    const glm::vec3 endVel = r.apxf[1] ? VelXfPoint(r.apxf[1], nullptr, end) : glm::vec3(0.0f);
    if (count > 64 || count < 3) return;
    static const float s_aw[4] = {0.0f, 0.5f, 0.5f, 1.0f};                                // D_002C3658
    static const float s_ak[4] = {1.0f / 6.0f, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 6.0f};    // D_002C3668
    const glm::vec3 vecX(1.0f, 0.0f, 0.0f), vecZ(0.0f, 0.0f, 1.0f);                       // D_002A2DA0 / D_002A2DC0
    glm::vec3 newP[64], newV[64], accP[64], accV[64], force[64];
    for (int i = 0; i < count; ++i)
    {
        accP[i] = r.apos54[i];
        accV[i] = r.avel58[i];
        force[i] = glm::vec3(0.0f);
    }
    const float dt = g_clock.dt;
    for (int pass = 0; pass < 4; ++pass)
    {
        const float w = s_aw[pass];
        const float h = w * dt;
        newP[0] = r.apos54[0] * (1.0f - w) + start * w;
        newV[0] = r.avel58[0] * (1.0f - w) + startVel * w;
        newP[count - 1] = r.apos54[count - 1] * (1.0f - w) + end * w;
        newV[count - 1] = r.avel58[count - 1] * (1.0f - w) + endVel * w;
        for (int i = 1; i < count - 1; ++i)
        {
            newV[i] = r.avel58[i] + force[i] * h;
            newP[i] = r.apos54[i] + (r.avel58[i] + newV[i]) * (h * 0.5f);
        }
        const float scale = 0.75f - 0.25f / static_cast<float>(count - 2);
        const float rest = r.as5C[count - 1];
        const glm::vec3 chord = newP[0] - newP[count - 1];
        const float amplitude = scale * std::sqrt(std::max(rest * rest - glm::dot(chord, chord), 0.0f));
        for (int i = 1; i < count - 1; ++i)
        {
            glm::vec3 F = r.force + newV[i] * -r.damping;
            if (r.stretch != 0.0f || r.stretchDamping != 0.0f)
            {
                float len1, len2;
                const glm::vec3 dir1 = NormalizeOr(newP[i - 1] - newP[i], vecX, &len1);
                const glm::vec3 dir2 = NormalizeOr(newP[i + 1] - newP[i], vecX, &len2);
                const float rest1 = r.as5C[i] - r.as5C[i - 1];
                const float rest2 = r.as5C[i + 1] - r.as5C[i];
                const glm::vec3 proj1 = dir1 * glm::dot(newV[i - 1] - newV[i], dir1);
                const glm::vec3 proj2 = dir2 * glm::dot(newV[i + 1] - newV[i], dir2);
                F = F + dir1 * (r.stretch * (len1 - rest1)) + proj1 * r.stretchDamping;
                F = F + dir2 * (r.stretch * (len2 - rest2)) + proj2 * r.stretchDamping;
            }
            if (r.bend != 0.0f || r.bendDamping != 0.0f)
            {
                const float t = (r.as5C[i] - r.as5C[i - 1]) / (r.as5C[i + 1] - r.as5C[i - 1]);
                const glm::vec3 mid = newP[i - 1] * (1.0f - t) + newP[i + 1] * t;
                const glm::vec3 midV = newV[i - 1] * (1.0f - t) + newV[i + 1] * t;
                float len;
                const glm::vec3 dir = NormalizeOr(mid - newP[i], vecZ, &len);
                const glm::vec3 proj = dir * glm::dot(midV - newV[i], dir);
                F = F + dir * (r.bend * len) + proj * r.bendDamping;
            }
            if (r.shape != 0.0f || r.shapeDamping != 0.0f)
            {
                const float t = r.as5C[i] / r.as5C[count - 1];
                const glm::vec3 mid = newP[0] * (1.0f - t) + newP[count - 1] * t;
                const glm::vec3 midV = newV[0] * (1.0f - t) + newV[count - 1] * t;
                float len;
                const glm::vec3 dir = NormalizeOr(mid - newP[i], vecZ, &len);
                const glm::vec3 proj = dir * glm::dot(midV - newV[i], dir);
                const float t4 = t * 4.0f;
                F = F + dir * (r.shape * (len - amplitude * t4 * (1.0f - t))) + proj * r.shapeDamping;
            }
            const float k = s_ak[pass] * dt;
            accV[i] = accV[i] + F * k;
            accP[i] = accP[i] + (r.avel58[i] + newV[i]) * (k * 0.5f);
            force[i] = F;
        }
    }
    if (r.mode == 3)
    {
        int j0 = 1;
        r.apos54[0] = start;
        r.avel58[0] = startVel;
        int i;
        for (i = 1; i < count - 1; ++i)
        {
            const float step = r.as5C[i] - r.as5C[i - 1];
            if (glm::length(end - r.apos54[i - 1]) < step) break;
            for (int j = j0; j < count; ++j)
            {
                float t;
                LSG lsg[1];
                if (!ClipSegmentToSphere(r.apos54[i - 1], accP[j - 1], accP[j], 1, lsg, step))  // 0x14C748
                {
                    r.apos54[i] = accP[j - 1];
                    t = 0.0f;
                }
                else if (lsg[0].t1 < 1.0f)
                {
                    t = lsg[0].t1;
                    r.apos54[i] = lsg[0].pos1;
                }
                else
                    continue;
                pc->as[i] = step;  // (as the original: the rest step, not the running sum)
                j0 = j;
                r.avel58[i] = accV[j - 1] * (1.0f - t) + accV[j] * t;
                break;
            }
        }
        r.apos54[i] = end;
        r.avel58[i] = endVel;
        count = i + 1;
        r.as5C[i] = r.as5C[i - 1] + glm::length(r.apos54[i] - r.apos54[i - 1]);
        SetCrvCount(r, pc, count);
        pc->as[i] = r.as5C[i];
    }
    else
    {
        r.apos54[0] = start;
        r.avel58[0] = startVel;
        r.apos54[count - 1] = end;
        r.avel58[count - 1] = endVel;
        for (int i = 1; i < count - 1; ++i)
        {
            r.apos54[i] = accP[i];
            r.avel58[i] = accV[i];
        }
    }
    const glm::mat4 inv = glm::inverse(MatWorldLox(r.ppath));
    for (int i = 0; i < count; ++i) pc->apos[i] = glm::vec3(inv * glm::vec4(r.apos54[i], 1.0f));
    if (r.mode != 3)
    {
        pc->as[0] = 0.0f;
        for (int i = 1; i < count; ++i) pc->as[i] = pc->as[i - 1] + glm::length(pc->apos[i - 1] - pc->apos[i]);
    }
    RefreshCrv(pc);
}

// orig: 0x001E6340 (vt slot 17 of the paths): the simulation by its mode, else the base's (0x1A5E80)
void UpdatePath17(LO* plo)
{
    auto it = s_mprope.find(plo);
    if (it == s_mprope.end() || !it->second) return;  // (the base 0x1A5E80: nothing for a path natively)
    ROPE& r = *it->second;
    // (the curve's world matrix cached by PcrvFromLo: kept current before the step's world <-> local conversions)
    PcrvFromLo(plo);
    switch (r.mode)
    {
    case 0: case 3: StepRopeSpring(r); break;
    case 1: case 2: StepRopeTrail(r); break;
    default: break;
    }
    if (FPrint())
    {
        const CRV* pc = PcrvFromLo(plo);
        std::printf("rope %d t %.3f: mode %d, %zu points, first %.0f %.0f %.0f last %.0f %.0f %.0f length %.1f\n", plo->oid, g_clock.t, r.mode,
                    pc ? pc->apos.size() : 0, r.apos54.front().x, r.apos54.front().y, r.apos54.front().z,
                    pc && !pc->apos.empty() ? r.apos54[pc->apos.size() - 1].x : 0.0f, pc && !pc->apos.empty() ? r.apos54[pc->apos.size() - 1].y : 0.0f,
                    pc && !pc->apos.empty() ? r.apos54[pc->apos.size() - 1].z : 0.0f, r.as5C[pc ? pc->apos.size() - 1 : 0]);
    }
}
NATIVEFN(0x1E6340, UpdatePath17);

LO* PloOfRef(const CRef& r) { return r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr; }
float GOfRef(const CRef& r) { return r.m_tagk == TAGK_F32 ? r.m_g : static_cast<float>(r.m_n); }
int NOfRef(const CRef& r) { return r.m_tagk == TAGK_F32 ? static_cast<int>(r.m_g) : r.m_n; }

float* PgField(ROPE& r, int eopid)
{
    switch (eopid)
    {
    case 1213: return &r.stretch;         // 1047 +0x64
    case 1214: return &r.bend;            // 1048 +0x6C
    case 1215: return &r.shape;           // 1049 +0x74
    case 1216: return &r.g7C;             // 1050 +0x7C
    case 1217: return &r.damping;         // 1051 +0x60
    case 1218: return &r.stretchDamping;  // 1052 +0x68
    case 1219: return &r.bendDamping;     // 1053 +0x70
    case 1220: return &r.shapeDamping;    // 1054 +0x78
    case 1221: return &r.g80;             // 1055 +0x80
    case 1222: return &r.sStep;           // 1056 +0x88
    default: return nullptr;
    }
}

// The rope meshes (class 0x6A): their path (+0x150, option 1311) and each vertex's sample (offset in the path point's
// basis, that point's parameter), the glob set copied so the bent vertices are this object's own
struct ROPEMESH
{
    bool fReady = false;
    LO* ppath = nullptr;
    std::shared_ptr<GLOBSET> source;
    std::vector<std::vector<glm::vec4>> asample;  // per glob, per vertex
};
std::unordered_map<const ALO*, ROPEMESH> s_mpropemesh;

bool FRopeMesh(const LO* plo) { return plo && plo->pcls && plo->pcls->PfnOrig(15) == 0x1E9858; }
}  // namespace

// orig: 0x001E9858 (vt slot 15 of class 0x6A, after the base 0x1F36A8): the samples of the model's vertices along the
// path at the load
void PostLoadRopeMesh(LO* plo)
{
    if (!FRopeMesh(plo)) return;
    ALO* palo = static_cast<ALO*>(plo);
    ROPEMESH& rm = s_mpropemesh[palo];
    if (rm.fReady) return;
    rm.fReady = true;
    for (const OPT& o : plo->aopt)
        if (o.eopid == S3EOPID(1311) && !o.aval.empty())
        {
            // (0x13E590: the loaded oid resolved in the object's namespace (0x15E958 kind 4), so the copy in a placed
            // instance finds its own instance's path: Ep7 32:3234's three guns each have a rope 42820 and its mesh
            // 42818 with no parent, and a plain nearest search sent all three meshes to the template's path, so the
            // fired line was never drawn (R43))
            LO* p = o.aval[0].pbasic ? dynamic_cast<LO*>(o.aval[0].pbasic) : nullptr;
            if (!p && g_splicegame.pfnFindObject)
                p = dynamic_cast<LO*>(g_splicegame.pfnFindObject(4, o.aval[0].n, plo, plo));
            rm.ppath = p ? p : PloFindSwNearest(plo, o.aval[0].n, false);
        }
    const CRV* pc = rm.ppath ? PcrvFromLo(rm.ppath) : nullptr;
    if (!pc || !palo->pglobset) return;
    rm.source = palo->pglobset;
    palo->pglobset = std::make_shared<GLOBSET>(*rm.source);
    palo->pglobset->palo = palo;
    const glm::mat4 mObj = MatJtLo(palo);  // vt slot 31
    const glm::vec3 axis = glm::vec3(glm::mat3(mObj)[2]);  // 0x1A6318's third row
    rm.asample.resize(palo->pglobset->aglob.size());
    for (size_t i = 0; i < palo->pglobset->aglob.size(); ++i)
    {
        GLOB& g = palo->pglobset->aglob[i];
        if (g.meshk != MESHK_Strips || g.avtx.empty()) continue;
        g.pgl = nullptr;
        for (SUBGLOB& sub : g.asubglob) sub.pgpu = nullptr;
        const glm::mat4 mOwner = g.fMatrix ? mObj * g.dmat : mObj;  // (the group's owner: the object, at its load)
        for (const VTXGLOB& v : g.avtx)
        {
            const glm::vec3 world = glm::vec3(mOwner * glm::vec4(v.pos, 1.0f));
            glm::vec3 hit, dir;
            float gHit = 0.0f;
            NearestCrvAll(pc, world, &hit, &dir, &gHit, nullptr);  // 0x1E6F60
            const glm::mat3 basis = BuildLookAtMatrix(dir, axis);  // 0x11B830
            const glm::vec3 local = glm::transpose(basis) * (world - hit);
            rm.asample[i].push_back(glm::vec4(local, gHit));
        }
        g.oidOwner = -1;
        g.fMatrix = false;
        g.fHistoryWorld = true;  // (positions in the world: the scene places it with the identity)
    }
    if (FPrint()) std::printf("rope mesh %d: path %d, %zu globs\n", plo->oid, rm.ppath ? rm.ppath->oid : -1, rm.asample.size());
}

// orig: 0x001E9AC0 (vt slot 19 of class 0x6A, around the base 0x1F3C70): each vertex at its sample's point of the path
// (the parameter clamped to the path's last), in that point's basis (direction, the object's z axis)
void PlaceRopeMeshScene(ALO* palo, SCENEGLOBSET& sgs)
{
    if (!FRopeMesh(palo)) return;
    PostLoadRopeMesh(palo);
    auto it = s_mpropemesh.find(palo);
    if (it == s_mpropemesh.end() || !it->second.ppath || !palo->pglobset) return;
    ROPEMESH& rm = it->second;
    const CRV* pc = PcrvFromLo(rm.ppath);
    if (!pc || pc->ag.empty()) return;
    const float gEnd = pc->ag.back();  // 0x1E6630
    const glm::vec3 axis = glm::vec3(glm::mat3(MatJtLo(palo))[2]);  // 0x1A6318
    glm::vec3 mnAll(1e30f), mxAll(-1e30f);
    for (size_t i = 0; i < rm.asample.size() && i < palo->pglobset->aglob.size(); ++i)
    {
        if (rm.asample[i].empty()) continue;
        GLOB& g = palo->pglobset->aglob[i];
        for (size_t k = 0; k < rm.asample[i].size() && k < g.avtx.size(); ++k)
        {
            const glm::vec4& s = rm.asample[i][k];
            glm::vec3 pos, dir;
            EvalCrv(pc, std::min(gEnd, s.w), &pos, &dir);  // 0x1E67B8
            const glm::mat3 m = BuildLookAtMatrix(dir, axis);
            g.avtx[k].pos = m * glm::vec3(s) + pos;
        }
        if (static const char* s_sz = std::getenv("SLY2N_ROPEPRINT"); s_sz && s_sz[0] == '2')  // (debug: the bent vertices' box)
        {
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const VTXGLOB& v : g.avtx) { mn = glm::min(mn, v.pos); mx = glm::max(mx, v.pos); }
            std::printf("rope mesh %d glob %zu t %.3f: %zu vertices in %.0f %.0f %.0f .. %.0f %.0f %.0f (path %zu points, end %.2f)\n", palo->oid, i,
                        g_clock.t, g.avtx.size(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, pc->apos.size(), gEnd);
        }
        // (natively the draw culls by bounds: the bent vertices' sphere, else the rope is culled once it leaves the
        // place of its load, e.g. the Ep7 boats' harpoon rope in flight)
        {
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const VTXGLOB& v : g.avtx) { mn = glm::min(mn, v.pos); mx = glm::max(mx, v.pos); }
            mnAll = glm::min(mnAll, mn);
            mxAll = glm::max(mxAll, mx);
            g.posCenter = (mn + mx) * 0.5f;
            g.sRadius = glm::length(mx - mn) * 0.5f;
            SetGlobBoundsGpu(g, g.posCenter, g.sRadius);
        }
        if (i < sgs.amatGlob.size()) sgs.amatGlob[i] = glm::mat4(1.0f);
        if (i < sgs.aamatBone.size()) sgs.aamatBone[i].clear();
        UpdateGlobVerticesGpu(g);
    }
    if (mnAll.x <= mxAll.x)
    {
        sgs.posBounds = (mnAll + mxAll) * 0.5f;
        sgs.sBoundsRadius = glm::length(mxAll - mnAll) * 0.5f;
    }
}

void ResetRopes()
{
    s_mprope.clear();
    s_mpropemesh.clear();
}

namespace
{
struct INITROPE
{
    INITROPE()
    {
        // 1042 / 1043 (EOPIDs 1208 / 1209, setters 0x1E7930 / 0x1E7968): the ends' objects
        RegisterObjectSetter(1208, [](LO* plo, const CRef& r) { if (ROPE* p = PropeEnsure(plo, 1)) SetRopeEnd(*p, 0, PloOfRef(r)); });
        RegisterObjectSetter(1209, [](LO* plo, const CRef& r) { if (ROPE* p = PropeEnsure(plo, 1)) SetRopeEnd(*p, 1, PloOfRef(r)); });
        // 1044 (EOPID 1210): the length (getter 0x1E79A0: the last rest distance; setter 0x1E79F0 -> 0x1E95B0)
        RegisterObjectGetter(1210, [](LO* plo) {
            CRef r;
            const CRV* pc = PcrvFromLo(plo);
            ROPE* p = pc && !pc->apos.empty() ? PropeEnsure(plo, 0) : nullptr;
            r.SetF32(p ? p->as5C[pc->apos.size() - 1] : 0.0f);
            return r;
        });
        RegisterObjectSetter(1210, [](LO* plo, const CRef& r) {
            if (ROPE* p = PropeEnsure(plo, 1)) SetRopeLength(*p, GOfRef(r));
        });
        // 1045 (EOPID 1211, a field at +0 through 0x1E7840): the force
        RegisterObjectSetter(1211, [](LO* plo, const CRef& r) { if (ROPE* p = PropeEnsure(plo, 1)) p->force = r.GetVector(); });
        RegisterObjectGetter(1211, [](LO* plo) {
            CRef r;
            ROPE* p = PropeEnsure(plo, 0);
            r.SetVector(p ? p->force : glm::vec3(0.0f));
            return r;
        });
        // 1046 (EOPID 1212): the mode (getter 0x1E7FF0: -1 without a simulation; setter 0x1E7A28: -1 the update off)
        RegisterObjectGetter(1212, [](LO* plo) {
            CRef r;
            auto it = s_mprope.find(plo);
            r.SetS32(it != s_mprope.end() && it->second ? it->second->mode : -1);
            return r;
        });
        RegisterObjectSetter(1212, [](LO* plo, const CRef& r) {
            const int n = NOfRef(r);
            ROPE* p = n != -1 ? PropeEnsure(plo, 1) : nullptr;
            if (n != -1) MarkRopeUpdate(plo);
            else
            {
                if (plo->iupd >= 0 && g_psw && plo->iupd < static_cast<int>(g_psw->aupd.size()))
                    g_psw->aupd[static_cast<size_t>(plo->iupd)].grf = (g_psw->aupd[static_cast<size_t>(plo->iupd)].grf | 0x20u) & ~0xCu;
                plo->grfUpd = (plo->grfUpd | 0x20u) & ~0xCu;
                auto it = s_mprope.find(plo);
                p = it != s_mprope.end() ? it->second.get() : nullptr;
            }
            if (p) p->mode = n;
            if (FPrint()) std::printf("rope %d t %.3f: mode %d\n", plo->oid, g_clock.t, n);
        });
        // 1047..1056 (EOPIDs 1213..1222): the forces' constants, the trail's step (FIELDREG below)
    }
} s_initrope;

// (the per-EOPID setters / getters of the fields: registered with their EOPID bound)
template <int E>
struct FIELDREG
{
    FIELDREG()
    {
        RegisterObjectSetter(E, [](LO* plo, const CRef& r) {
            if (ROPE* p = PropeEnsure(plo, 1))
                if (float* pg = PgField(*p, E)) *pg = GOfRef(r);
        });
        RegisterObjectGetter(E, [](LO* plo) {
            CRef r;
            ROPE* p = PropeEnsure(plo, 0);
            float* pg = p ? PgField(*p, E) : nullptr;
            r.SetF32(pg ? *pg : 0.0f);
            return r;
        });
    }
};
FIELDREG<1213> s_f1213;
FIELDREG<1214> s_f1214;
FIELDREG<1215> s_f1215;
FIELDREG<1216> s_f1216;
FIELDREG<1217> s_f1217;
FIELDREG<1218> s_f1218;
FIELDREG<1219> s_f1219;
FIELDREG<1220> s_f1220;
FIELDREG<1221> s_f1221;
FIELDREG<1222> s_f1222;
}  // namespace

// The loaded field options (1047..1056 of a path, grf 0x2080 through the resolver 0x1E7840): applied at the post-load,
// which makes the simulation as the original's load does
void PostLoadPathRope(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, S3CID(0x0A))) return;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty() || o.eopid < 1211 || o.eopid > 1222 || o.eopid == S3EOPID(1212)) continue;
        ROPE* p = PropeEnsure(plo, 1);
        if (!p) return;
        if (o.eopid == S3EOPID(1211)) p->force = glm::vec3(o.aval[0].ag[0], o.aval[0].ag[1], o.aval[0].ag[2]);
        else if (float* pg = PgField(*p, o.eopid)) *pg = o.aval[0].ag[0];
        if (FPrint()) std::printf("rope %d: loaded option eopid %d = %g\n", plo->oid, o.eopid, o.aval[0].ag[0]);
    }
}
