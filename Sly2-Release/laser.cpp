// CID 0x55 laser beams: native endpoint tracks, broad-phase bounds and retained line contacts.
// Source: original 0x1DE400..0x1DFAD8, matching decomp 0x1DEF98/0x1DF3D0/0x1DF260,
// and the original assembly of 0x1DE5D8/0x1DF0A0. No mission-specific state is used.
#include "laser.h"
#include "worldglobals.h"
#include "alo.h"
#include "activate.h"
#include "animfind.h"
#include "clock.h"
#include "crv.h"
#include "disc.h"
#include "lo.h"
#include "objmethod.h"
#include "readtrace.h"
#include "so.h"
#include "stepphys.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "xfphys.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

namespace
{
struct TRACKSEG { float s = 0, v = 0, a = 0, duration = 0, t = 0; };  // +0x1C, 0x14 each
struct LASEREND
{
    OID oid = -1;
    glm::vec3 params{0.0f};  // loaded +0x18: initial fraction, speed, period
    int mode = 0;          // loaded +0x24
    int kind = -1;         // +4: 0 local point, 1 distance along path
    glm::vec3 pos{0.0f};   // +0x10 (kind 0)
    LO* ppath = nullptr;   // +0x10 (kind 1)
    float s = 0;          // +0x14
    std::array<TRACKSEG, 6> aseg{};
    int cseg = 0, iseg = 0;  // +0x18 / +0x94
};
struct LASERPAIR
{
    LASEREND ends[2];
    float length = 0, hitLength = 0;  // +0x140 / +0x144
    XP* pxp = nullptr;                // +0x148
};
struct LASER
{
    std::vector<LASERPAIR> pairs;  // +0x198 / +0x19C
    std::vector<int> aoid, acid;   // base +0x150 / +0x170 (up to 7)
    bool bound = false;
};
std::unordered_map<const ALO*, LASER> s_lasers;
WORLD_TABLE(s_lasers);   // (PC: moved aside while a kept world loads, worldglobals.h)
LASER& Laser(ALO* p) { return s_lasers[p]; }
float FloatField(const LO* p, uint32_t off, float def)
{
    uint32_t bits;
    std::memcpy(&bits, &def, 4);
    bits = p->U32Field(0, off, bits);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void ApplyLaserContacts(ALO* p);
void Post(ALO* p)
{
    PostDeferredCall(p, 0x1DF260, [](void* object, int) { ApplyLaserContacts(static_cast<ALO*>(object)); });
}

// orig: 0x001DE5A8 (slot 53): the laser's endpoint bounds do not prune the other shape's BSP.
OXA* AddLaserAttachment(ALO* p)
{
    OXA* attachment = PoxaAddSw(p);  // 0x1F56F8's world allocation, native bbmark.cpp
    attachment->grf |= 0x20;
    return attachment;
}

// orig: 0x001DEF98: resolve the two endpoint positions in world space.
std::array<glm::vec3, 2> Endpoints(ALO* p, const LASERPAIR& pair)
{
    std::array<glm::vec3, 2> pos{};
    for (int i = 0; i < 2; ++i)
    {
        const LASEREND& e = pair.ends[i];
        if (e.kind == 0) pos[i] = PosConvertXfs(PxfObj(p), nullptr, e.pos);
        else if (const CRV* path = PcrvFromLo(e.ppath)) EvalCrvS(path, e.s, &pos[i], nullptr);
    }
    return pos;
}

// orig: 0x001DE5D8, 0x1DE6CC..0x1DEAD8: construct each path's polynomial time segments.
void BindEnd(ALO* p, LASEREND& e)
{
    e.kind = -1;
    LO* po = e.oid != -1 ? PloFindSwChild(p, e.oid) : nullptr;  // 0x15E938(G,1,oid,p)
    if (po)
    {
        if (FCidDerivedFrom(po->Cid(), 0x0A))
        {
            const CRV* path = PcrvFromLo(po);
            const float length = SLengthCrv(path);
            const float start = std::clamp(e.params.x, 0.0f, 1.0f);
            const bool back = e.params.y < 0;
            float speed = std::fabs(e.params.y), period = std::fabs(e.params.z);
            const int mode = e.mode == 0 || e.mode == 8 || e.mode == 16 ? e.mode : 8;
            if (period > 0) speed = length / period;
            else if (speed > 0) period = length / speed;
            e.s = start * length;
            if (speed == 0 || period == 0)
            {
                e.kind = 0;
                EvalCrvS(path, e.s, &e.pos, nullptr);
            }
            else
            {
                e.kind = 1;
                e.ppath = po;
                e.aseg = {};
                e.iseg = 0;
                if (mode == 8 || mode == 0)
                {
                    e.cseg = mode == 8 ? 1 : 2;
                    e.aseg[0] = {back ? length : 0, back ? -speed : speed, 0, period,
                                  (back ? 1.0f - start : start) * period};
                    // The original mode-0 second record is a zero-duration stationary endpoint.
                    if (mode == 0) e.aseg[1].s = back ? 0 : length;
                }
                else
                {
                    const float radius = 300.0f <= length ? 100.0f : length * 0.2f;  // D_2C3610
                    const float twice = radius + radius;
                    const float turnTime = (twice * period) / (length + twice);
                    const float straightTime = period - (turnTime + turnTime);
                    const float v = (length - twice) / straightTime;
                    const float a = (v * v) / twice;
                    e.cseg = 6;
                    e.aseg[0] = {0, 0, a, turnTime, 0};
                    e.aseg[1] = {radius, v, 0, straightTime, 0};
                    e.aseg[2] = {length - radius, v, -a, turnTime, 0};
                    e.aseg[3] = {length, 0, -a, turnTime, 0};
                    e.aseg[4] = {length - radius, -v, 0, straightTime, 0};
                    e.aseg[5] = {radius, -v, a, turnTime, 0};
                    for (int k = 0; k < 3; ++k)
                    {
                        e.iseg = k;
                        // +0x14/+0x18 address the next record's start and initial velocity.
                        if (e.s < e.aseg[k + 1].s)
                        {
                            const float t = ((e.s - e.aseg[k].s) + (e.s - e.aseg[k].s)) /
                                            (e.aseg[k].v + e.aseg[k + 1].v);
                            if (back) { e.iseg = 5 - k; e.aseg[e.iseg].t = e.aseg[e.iseg].duration - t; }
                            else e.aseg[k].t = t;
                            break;
                        }
                        e.iseg = k + 1;
                    }
                }
                if (p->iupd >= 0) g_psw->aupd[p->iupd].grf |= GRFUPD_Update;
                else p->grfUpd |= GRFUPD_Update;
            }
        }
        else if (FCidDerivedFrom(po->Cid(), 3) || FCidDerivedFrom(po->Cid(), 0x30))
        {
            e.kind = 0;
            e.pos = PosLoWorld(po);
        }
    }
    if (e.kind == -1) { e.kind = 0; e.pos = PosAloCenter(p); }
    if (e.kind == 0) e.pos = PosConvertXfs(nullptr, PxfObj(p), e.pos);
}

// orig: 0x001DF260: choose the nearest contact and offer the beam's original damage message.
void ApplyLaserContacts(ALO* p)
{
    for (LASERPAIR& pair : Laser(p).pairs)
    {
        const auto ends = Endpoints(p, pair);
        const float length = glm::length(ends[0] - ends[1]);
        if (!pair.pxp) pair.length = length;
        pair.hitLength = length;
        XP* nearest = nullptr;
        for (XP* xp = pair.pxp; xp; xp = xp->pxpNext)
            if (xp->gA0 < pair.hitLength) { pair.hitLength = xp->gA0; nearest = xp; }
        if (!nearest) continue;
        CTMSG msg;
        msg.ploSender = p;
        msg.pvOther = nearest->axpd[0].psoe;
        msg.ploReceiver = nearest->axpd[1].palo;
        msg.grfFilter = 0x1000;
        msg.nState = -10;
        msg.pos = nearest->axpd[0].pos;
        PaloAcceptContact(&msg);
    }
}

// orig: 0x001DEEA8 (slot 54): remove contacts against the detached physics root.
void RemoveLaserContacts(ALO* p, ALO* other)
{
    for (LASERPAIR& pair : Laser(p).pairs)
    {
        XP** link = &pair.pxp;
        while (XP* xp = *link)
        {
            const int side = xp->axpd[0].paloRoot == p ? 1 : 0;
            if (xp->axpd[side].paloRoot == other)
            {
                *link = xp->pxpNext;
                g_physw.FreeXp(xp);
            }
            else link = &xp->pxpNext;
        }
    }
    Post(p);
}

// orig: 0x00205670/0x002057E8: a beam can cross a STEP's two floor-support triangles as well as its body.
bool HitStepSupport(ALO* p, const glm::vec3& a, const glm::vec3& b, glm::vec3* hit)
{
    const STEP* step = PstepPhysOf(p);
    if (!step || !step->pxpFloor || step->isoeCustom < 0 || step->isoeCustom >= int(p->asoe.size())) return false;
    const auto& points = p->asoe[step->isoeCustom].geomWorld.apos;
    if (step->iposProbe0 < 0 || step->iposProbe0 >= int(points.size())) return false;
    const glm::vec3 center = points[step->iposProbe0];
    const glm::mat4 mat = PxfObj(p) ? PxfObj(p)->matWorld : glm::mat4(1.0f);
    const std::array<glm::vec3, 5> verts{PosAloCenter(p), center + glm::vec3(mat[0]) * step->g220,
        center + glm::vec3(mat[1]) * step->g220, center - glm::vec3(mat[0]) * step->g220,
        center - glm::vec3(mat[1]) * step->g220};
    const glm::vec3 segment = b - a;
    const float length = glm::length(segment);
    const glm::vec3 dir = length < 0.0001f ? glm::vec3(0, 0, 1) : segment / length;  // D_2A2DC0
    for (const auto& tri : {std::array<int, 3>{0, 1, 3}, std::array<int, 3>{0, 2, 4}})
    {
        const glm::vec3 edge1 = verts[tri[1]] - verts[tri[0]], edge2 = verts[tri[2]] - verts[tri[0]];
        const glm::vec3 cross = glm::cross(dir, edge2);
        const float det = glm::dot(edge1, cross);
        if (-0.000001f < det && det < 0.000001f) continue;
        const float inv = 1.0f / det;  // orig 0x11EBB0
        const glm::vec3 rel = a - verts[tri[0]];
        const float u = glm::dot(rel, cross) * inv;
        if (u < 0 || u > 1) continue;
        const glm::vec3 q = glm::cross(rel, edge1);
        const float v = glm::dot(dir, q) * inv;
        const float t = glm::dot(edge2, q) * inv;
        if (v < 0 || 1 < u + v || t < 0 || length < t) continue;
        *hit = a + dir * t;
        return true;
    }
    return false;
}

// orig: 0x001DF0A0 (slot 60): retained contacts per beam, outside the ordinary rigid contact list.
void AddLaserContacts(ALO* p, SOE* other, int, const BSPREF&, int, XP**)
{
    LASER& laser = Laser(p);
    if (p->asoe.empty()) return;
    for (LASERPAIR& pair : laser.pairs)
    {
        const auto ends = Endpoints(p, pair);
        LSG lsg;
        glm::vec3 hit;
        bool found = ClipSegmentEntry(other, ends[0], ends[1], 1, &lsg) != 0;
        if (found) hit = lsg.pos0;
        else if (FAloStep(other->palo)) found = HitStepSupport(other->palo, ends[0], ends[1], &hit);
        if (!found) continue;
        const glm::vec3 delta = ends[1] - ends[0];
        const float length = glm::length(delta);
        const glm::vec3 dir = length != 0 ? delta / length : glm::vec3(0.0f);
        XP* xp = PxpAddSw(&p->asoe.front(), other, hit, dir, 10, nullptr, nullptr, &pair.pxp, 0);
        xp->gA0 = glm::length(ends[0] - hit);
        Post(p);
    }
}

// orig: 0x001DE2D8 (slot 61): enable overlap for listed object aliases and derived classes.
int LaserOverlapKind(ALO* p, ALO* other)
{
    const LASER& laser = Laser(p);
    for (int oid : laser.aoid) if (other->oid == oid || other->oidInstance == oid) return 2;
    for (int cid : laser.acid) if (FCidDerivedFrom(other->Cid(), cid)) return 2;
    return 1;
}

// orig: 0x001DF3A0/0x001DF3D0: base update, then advance each track at the live speed multiplier.
void UpdateLaser(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);
    ALO* p = static_cast<ALO*>(plo);
    for (LASERPAIR& pair : Laser(p).pairs)
    {
        bool moved = false;
        for (LASEREND& e : pair.ends)
        {
            if (e.kind != 1 || e.cseg == 0) continue;
            TRACKSEG* seg = &e.aseg[e.iseg];
            seg->t += FloatField(p, 0x1C0, 1.0f) * g_clock.dt;
            if (seg->duration < seg->t)
            {
                const float t = std::fmod(seg->t, seg->duration);
                e.iseg = (e.iseg + 1) % e.cseg;
                seg = &e.aseg[e.iseg];
                seg->t = t;
            }
            e.s = (seg->s + seg->v * seg->t) + ((seg->a * seg->t) * seg->t) * 0.5f;
            // Original 0x1DF4F8 marks dirty2 alone: whole-track bounds stay fixed.
            if (OXA* attachment = Poxa(p); attachment && !(attachment->grf & 2))
            {
                attachment->grf |= 2;
                g_physw.apoxaDirty2.push_back(attachment);
            }
            moved = true;
        }
        if (moved && !pair.pxp)
        {
            const auto ends = Endpoints(p, pair);
            pair.length = pair.hitLength = glm::length(ends[0] - ends[1]);
        }
    }
}

struct INITLASER
{
    INITLASER()
    {
        RegisterObjectMethod(0x1056A0, [](LO* plo, const std::vector<CRef>& a) {
            LASER& laser = Laser(static_cast<ALO*>(plo));
            if (laser.pairs.size() < 0x40)
            {
                LASERPAIR pair;
                pair.ends[0].oid = NArg(a, 0, -1);
                pair.ends[0].params = {GArg(a, 1), GArg(a, 2), GArg(a, 3)};
                pair.ends[0].mode = NArg(a, 4);
                pair.ends[1].oid = NArg(a, 5, -1);
                pair.ends[1].params = {GArg(a, 6), GArg(a, 7), GArg(a, 8)};
                pair.ends[1].mode = NArg(a, 9);
                laser.pairs.push_back(pair);
            }
            return CRef(TAGK_Void);
        });  // orig: 0x001DF908 via EOPID455
        RegisterObjectSetter(448, [](LO* plo, const CRef& ref) {
            auto& v = Laser(static_cast<ALO*>(plo)).aoid;
            if (v.size() < 7) v.push_back(ref.m_n);
        });  // orig: 0x001DE3D8
        RegisterObjectSetter(449, [](LO* plo, const CRef& ref) {
            auto& v = Laser(static_cast<ALO*>(plo)).acid;
            if (v.size() < 7) v.push_back(ref.m_n);
        });  // orig: 0x001DE3B0
        RegisterLoUpdate(0x1DF3A0, UpdateLaser);
    }
} s_initlaser;
}  // namespace

// orig: 0x001DE478: allocate endpoint storage before the base loader evaluates its options.
void LoadLaserFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1DE478);
    ALO* p = static_cast<ALO*>(plo);
    Laser(p);
    Sort(p).grf148 = (Sort(p).grf148 | (uint64_t(1) << 34)) & ~(uint64_t(1) << 35);  // 0x1DE400
    LoadAloFromBrx(plo, pbis);
    // The native base loader initializes physics after construction; retain the derived init's clear bit35.
    Sort(p).grf148 = (Sort(p).grf148 | (uint64_t(1) << 34)) & ~(uint64_t(1) << 35);
}

// orig: 0x001DE568/0x001DE5D8: build the endpoint bounds as the sole sensor entry, then initialize lengths.
void PostLoadLaser(LO* plo)
{
    if (plo->Cid() != 0x55) return;
    ALO* p = static_cast<ALO*>(plo);
    LASER& laser = Laser(p);
    if (laser.bound) return;
    laser.bound = true;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    auto grow = [&](const glm::vec3& point) { lo = glm::min(lo, point); hi = glm::max(hi, point); };
    RemoveAloSoes(p);
    for (LASERPAIR& pair : laser.pairs)
        for (LASEREND& e : pair.ends)
        {
            BindEnd(p, e);
            if (e.kind == 0) grow(PosConvertXfs(PxfObj(p), nullptr, e.pos));
            else if (const CRV* path = PcrvFromLo(e.ppath))
                for (const glm::vec3& point : path->apos) grow(glm::vec3(path->mat * glm::vec4(point, 1)));
        }
    if (!laser.pairs.empty())
    {
        p->asoe.clear();
        p->asoe.emplace_back();
        SOE& entry = p->asoe.front();
        entry.pxf = PxfObj(p);
        entry.palo = p;
        entry.tag = p->oid;
        entry.f48 = true;
        entry.geomLocal.apos = {PosConvertXfs(nullptr, PxfObj(p), lo), PosConvertXfs(nullptr, PxfObj(p), hi)};
        entry.geomWorld.apos = {lo, hi};
        entry.nStampWorld = std::numeric_limits<int32_t>::min();
    }
    if (p->fInWorld) AddAloSoes(p);
    ApplyLaserContacts(p);
    if (std::getenv("SLY2N_LASERPRINT"))
    {
        std::fprintf(stderr, "laser %d pairs %zu bounds %.3f %.3f %.3f .. %.3f %.3f %.3f\n", p->oid, laser.pairs.size(),
                     lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        for (const LASERPAIR& pair : laser.pairs)
        {
            const auto ends = Endpoints(p, pair);
            std::fprintf(stderr, "  beam %.3f %.3f %.3f .. %.3f %.3f %.3f; kind %d %d, path %d %d\n",
                         ends[0].x, ends[0].y, ends[0].z, ends[1].x, ends[1].y, ends[1].z,
                         pair.ends[0].kind, pair.ends[1].kind, pair.ends[0].oid, pair.ends[1].oid);
            for (int j = 0; j < 2; ++j)
            {
                const LASEREND& e = pair.ends[j];
                std::fprintf(stderr, "    end %d s %.9g count %d current %d", j, e.s, e.cseg, e.iseg);
                for (int k = 0; k < e.cseg; ++k)
                {
                    const TRACKSEG& s = e.aseg[k];
                    std::fprintf(stderr, " [%.9g %.9g %.9g %.9g %.9g]", s.s, s.v, s.a, s.duration, s.t);
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
}

// orig: 0x001DE518: cloned laser endpoint storage is independent of the source object.
void CloneLaser(const ALO* src, ALO* dst)
{
    auto it = s_lasers.find(src);
    if (it == s_lasers.end()) return;
    LASER laser = it->second;
    laser.bound = false;
    for (LASERPAIR& pair : laser.pairs) pair.pxp = nullptr;
    s_lasers[dst] = std::move(laser);
}

void ResetLasers() { s_lasers.clear(); }

// orig: 0x001DF5B0, 0x1DF658..0x1DF6B0: truncate the visible endpoint to +0x144.
std::vector<LASERBEAM> LaserBeams(ALO* p)
{
    std::vector<LASERBEAM> result;
    auto it = s_lasers.find(p);
    if (it == s_lasers.end()) return result;
    for (const LASERPAIR& pair : it->second.pairs)
    {
        const auto ends = Endpoints(p, pair);
        const float length = glm::length(ends[0] - ends[1]);
        glm::vec3 b = ends[1];
        if (pair.hitLength < length)
        {
            const float u = pair.hitLength / length;
            b = ends[0] * (1.0f - u) + ends[1] * u;
        }
        result.push_back({ends[0], b});
    }
    return result;
}

NATIVEFN(0x1DEEA8, RemoveLaserContacts);
NATIVEFN(0x1DE5A8, AddLaserAttachment);
NATIVEFN(0x1DF0A0, AddLaserContacts);
NATIVEFN(0x1DE2D8, LaserOverlapKind);
