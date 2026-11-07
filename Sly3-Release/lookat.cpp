// The looking controllers of transform records (Sly 2 0x12D210-0x12D858): kind 2, a record (a head) turning toward a
// target within a range, and kind 4, a record looking along the motion of the translation up its chain.
// Specification: the original's code (0x12D220, no matched decomp), Sly2-Reference/Sly2-Release/game/m_12B3B0_p3.cpp (0x12D508,
// oracle-verified), mat.cpp (0x11B830), m_11E1A0_p3.cpp (0x11E958), m_22C818_p3.cpp (0x22FCC8 / 0x22FD38).

#include "xf.h"
#include "game/sly3.h"

#include "animfind.h"
#include "clock.h"
#include "po.h"
#include "sw.h"
#include "xfmath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>


namespace
{
constexpr float kEpsilon = 1e-4f;

// orig 0x001A62E8: an object's position (vt slot 31: its world matrix's translation; ALO 0x1F3FA0: the record
// of its third reference (+0x60))
glm::vec3 PosOfLo(const LO* plo)
{
    if (FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
    {
        const ALO* palo = static_cast<const ALO*>(plo);
        if (palo->apxfRef[2]) return glm::vec3(palo->apxfRef[2]->matWorld[3]);
        if (!palo->axf.empty()) return glm::vec3(palo->axf[0]->matWorld[3]);
    }
    if (FCidDerivedFrom(plo->Cid(), S3CID(3)))
    {
        // orig 0x231440 / 0x231368: the reference record's world times the LOX's matrix
        const LOX* plox = static_cast<const LOX*>(plo);
        return glm::vec3((plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat)[3]);
    }
    return glm::vec3(0.0f);
}

// orig: 0x001C4AF8 (the nearest of the world's player objects SW +0x3EBC (every object vt slot 79 enabled or
// disabled) in the world (+0x28 bit 60), by 0x1A62E8's position; none: nullptr)
LO* PloNearestPo(const glm::vec3& pos)
{
    LO* pbest = nullptr;
    float s2Best = 3.4028235e38f;  // D_002A7708
    for (LO* plo : AploRegistered())
    {
        if (!plo->fInWorld) continue;
        const glm::vec3 d = pos - PosOfLo(plo);
        const float s2 = glm::dot(d, d);
        if (s2 < s2Best)
        {
            s2Best = s2;
            pbest = plo;
        }
    }
    return pbest;
}

// orig 0x0022FCC8(0, pxf, v): a world direction in the record's space
glm::vec3 DirToLocal(const XF* pxf, const glm::vec3& v)
{
    return pxf ? glm::inverse(glm::mat3(pxf->matWorld)) * v : v;
}

// orig 0x0011E958: pan = atan2(y, x), tilt = atan2(z, |xy|), normalized
void PanTiltFromVector(const glm::vec3& v, float* ppan, float* ptilt)
{
    *ppan = RadNormalize(std::atan2(v.y, v.x));
    *ptilt = RadNormalize(std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)));
}

// orig 0x0011E390
glm::vec3 PerpendicularVector(const glm::vec3& v)
{
    const float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
    glm::vec3 p;
    if (ax <= ay) p = ax <= az ? glm::vec3(0.0f, v.z, -v.y) : glm::vec3(v.y, -v.x, 0.0f);
    else p = ay <= az ? glm::vec3(v.z, 0.0f, -v.x) : glm::vec3(v.y, -v.x, 0.0f);
    const glm::vec3 c = glm::cross(v, p);
    return c * (1.0f / std::sqrt(glm::dot(c, c)));
}

// orig 0x0011B830 BuildLookAtMatrix: rows x along vecX ((1, 0, 0) when too short), y = vecY x x normalized (any
// perpendicular when degenerate), z = x x y
glm::mat3 MatLookAt(const glm::vec3& vecX, const glm::vec3& vecY)
{
    const float g = std::sqrt(glm::dot(vecX, vecX));
    const glm::vec3 x = g < kEpsilon ? glm::vec3(1.0f, 0.0f, 0.0f) : vecX * (1.0f / g);
    glm::vec3 y = glm::cross(vecY, x);
    const float l = std::sqrt(glm::dot(y, y));
    y = l < kEpsilon ? PerpendicularVector(x) : y * (1.0f / l);
    glm::mat3 m;
    m[0] = x;
    m[1] = y;
    m[2] = glm::cross(x, y);
    return m;
}

// The segment's velocity of a record (0x12D508's switch on the mode, as 0x22CDC0's translation case)
glm::vec3 VelOfXf(const XF* po)
{
    switch (po->mode)
    {
    case 1: case 2: case 7: case 8: case 9: case 11: case 17: case 18: case 20: case 21:
        return glm::vec3(po->vel);
    case 5: case 6:
        return glm::vec3(VecXfTargetSlope(po));
    default:
        return glm::vec3(0.0f);
    }
}
}  // namespace

// orig: 0x0012D210
void CTRLLOOKAT::End()
{
    plo = nullptr;
    fPrev = false;
}

void CTRLLOOKAT::Start() { CTRL::Start(); }

// orig: 0x0012D4D8 (the record's segment start and its slope)
void CTRLLOOKAT::Query(glm::vec4* ppos, glm::vec4* pvel, float g)
{
    (void)g;
    if (ppos) *ppos = pxf->aseg[0];
    if (pvel) *pvel = pxf->aseg[1];
}

// orig: 0x0012D220
void CTRLLOOKAT::Fetch()
{
    const float sRange = job.g;          // +0x60
    const int prioNear = job.a;          // +0x64
    const int prioFar = job.b;           // +0x68
    const OID oidTarget = job.c;         // +0x6C
    if (!plo && oidTarget != -1) plo = PloFindSwNearest(palo, oidTarget);  // (0x15E938(G, 0x104, oid, palo))
    const glm::vec3 posSelf(pxf->matWorld[3]);
    LO* ploTarget = plo ? plo : PloNearestPo(posSelf);  // (0x1C4AF8 when no target)
    glm::vec3 d(0.0f);
    float dist;
    if (ploTarget)
    {
        d = PosOfLo(ploTarget) - posSelf;
        dist = std::sqrt(glm::dot(d, d));
    }
    else
    {
        // (D_002A45A8: out of every range)
        dist = 3.4028235e38f;
    }
    static const char* s_szLook = std::getenv("SLY2N_LOOKPRINT");  // debug: oid (the looking object's)
    if (s_szLook && (palo->oid == std::atoi(s_szLook) || std::atoi(s_szLook) == 0))
        std::printf("look t %.3f %d: target %d (%s) dist %.1f range %.1f prio %d/%d\n", g_clock.t, palo->oid, ploTarget ? ploTarget->oid : -1,
                    plo ? "own" : "nearest po", dist, sRange, prioNear, prioFar);
    const int prioNew = sRange < dist ? prioFar : prioNear;
    if (prioNew != prio)
    {
        prio = prioNew;
        SortXfCtrls(palo, pxf);
        if (pxf->pctrlFirst != this)
        {
            pxf->pctrlFirst->Fetch();  // (the new head's slot 4)
            return;
        }
    }
    glm::vec4 value, vel;
    if (sRange < dist)
    {
        value = posRest;
        vel = glm::vec4(0.0f);
        fPrev = false;
    }
    else
    {
        float pan, tilt;
        PanTiltFromVector(DirToLocal(pxf->pxfRef, d), &pan, &tilt);
        float g = job.agBegin[0] + pan * (job.agBegin[1] + pan * job.agBegin[2]);
        if (g < job.agTimingBegin[0]) g = job.agTimingBegin[0];
        else if (job.agTimingBegin[1] < g) g = job.agTimingBegin[1];
        pan = g;
        float h = job.agEnd[0] + tilt * (job.agEnd[1] + tilt * job.agEnd[2]);
        if (h < job.agTimingEnd[0]) h = job.agTimingEnd[0];
        else if (job.agTimingEnd[1] < h) h = job.agTimingEnd[1];
        tilt = h;
        value = glm::vec4(0.0f, -tilt, pan, 0.0f);
        // the velocity: the change since the last value over the frame (orig: the clock's dt 0x2DDED4)
        vel = fPrev ? (value - pxf->aseg[0]) * (1.0f / g_clock.dt) : glm::vec4(0.0f);
        fPrev = true;
    }
    pxf->aseg[0] = value;
    pxf->aseg[1] = vel;
    pxf->state = 3;
}

// orig: 0x0012D508
void CTRLLOOKMOTION::Fetch()
{
    if (!pxfMover)
    {
        XF* q = pxf->pxfRef;
        while (q && q->kind != 1) q = q->pxfRef;
        if (q) pxfMover = q;
        else
        {
            pxf->aseg[0] = pxf->mat[0];
            pxf->aseg[1] = pxf->mat[1];
            pxf->aseg[2] = pxf->mat[2];
            pxf->state = 5;
            return;
        }
    }
    XF* po = pxfMover;
    if (CTRL* pc = po->pctrlFirst) RefreshXf(pc->palo, po);
    glm::vec3 vel = VelOfXf(po);
    if (const XF* ppar = po->pxfRef) vel = glm::mat3(ppar->matWorld) * vel;
    const float len = std::sqrt(glm::dot(vel, vel));
    glm::vec3 vecX, vecUp;
    if (len < 100.0f)
    {
        vecX = glm::vec3(pxf->matWorld[0]);  // (slow: the record's own x axis)
        vecUp = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    else
    {
        vecX = vel;
        vecUp = glm::vec3(0.0f, 0.0f, 1750.0f);  // D_002BC380
        if (0.0f < job.gHi && 0.0f < job.gLo)
            if (CTRL* pc = po->pctrlFirst)
            {
                // the lead: the head controller's velocity (slot 5), toward which the up vector leans
                glm::vec4 lead(0.0f);
                pc->Query(nullptr, &lead, job.gHi);  // (the lead time +0x20 in $f12)
                glm::vec3 l(lead);
                if (const XF* ppar = po->pxfRef) l = glm::mat3(ppar->matWorld) * l;
                vecUp = vecUp + (l - vel) * (job.gLo / job.gHi);
            }
    }
    // the look-at frame in the record's parent's space (0x22FD38(r, 0, parent, m))
    glm::mat3 m = MatLookAt(vecX, vecUp);
    if (pxf->pxfRef) m = glm::inverse(glm::mat3(pxf->pxfRef->matWorld)) * m;
    pxf->aseg[0] = glm::vec4(m[0], 0.0f);
    pxf->aseg[1] = glm::vec4(m[1], 0.0f);
    pxf->aseg[2] = glm::vec4(m[2], 0.0f);
    pxf->state = 5;
}
