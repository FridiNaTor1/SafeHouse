// Two-bone joints (IK). Specification: Sly2-Reference/Sly2-Release/game/m_186610.cpp (oracle-verified), vec.cpp (0x11E4B0,
// 0x11E390), m_22C818_p3.cpp (0x22FCC8).

#include "ik.h"

#include "aseg.h"
#include "xfmath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
constexpr float kEpsilon = 1e-4f;  // 0x38D1B717

// orig 0x0011E390: a unit vector perpendicular to v
glm::vec3 PerpendicularVector(const glm::vec3& v)
{
    const float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
    glm::vec3 p;
    if (ax <= ay) p = ax <= az ? glm::vec3(0.0f, v.z, -v.y) : glm::vec3(v.y, -v.x, 0.0f);
    else p = ay <= az ? glm::vec3(v.z, 0.0f, -v.x) : glm::vec3(v.y, -v.x, 0.0f);
    const glm::vec3 c = glm::cross(v, p);
    return c * (1.0f / std::sqrt(glm::dot(c, c)));
}

// orig 0x0011E4B0 RadBetweenVectors (the axis: the unit cross product, x when either is too short)
float RadBetweenVectors(const glm::vec3& a, const glm::vec3& b, glm::vec3* paxis)
{
    const float la = std::sqrt(glm::dot(a, a)), lb = std::sqrt(glm::dot(b, b));
    if (la < kEpsilon || lb < kEpsilon)
    {
        if (paxis) *paxis = glm::vec3(1.0f, 0.0f, 0.0f);
        return 0.0f;
    }
    if (paxis)
    {
        const glm::vec3 c = glm::cross(a, b);
        const float lc = std::sqrt(glm::dot(c, c));
        float k;
        const uint32_t u = 0x322BCC77u;
        std::memcpy(&k, &u, 4);
        *paxis = k < lc ? c * (1.0f / lc) : PerpendicularVector(a);
    }
    // (the original's acosf of a ratio a rounding past 1 would be a NaN: clamped)
    return std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f));
}

// orig 0x0022FCC8(0, pxf, v): a world direction in the record's space (its world 3x3 inverted)
glm::vec3 DirToLocal(const XF* pxf, const glm::vec3& v)
{
    if (!pxf) return v;
    return glm::inverse(glm::mat3(pxf->matWorld)) * v;
}

glm::vec3 PosWorld(const XF* pxf, const glm::vec3& v)
{
    return pxf ? glm::vec3(pxf->matWorld * glm::vec4(v, 1.0f)) : v;
}
glm::vec3 PosOfWorld(const XF* pxf) { return pxf ? glm::vec3(pxf->matWorld[3]) : glm::vec3(0.0f); }

// kind 7 (0x2D2E58): a joint's controller (orig 0x186640 Start: the record's +0xA8 is the joint)
struct CTRLJOINT : CTRL
{
    ALOPAIR* ppair = nullptr;  // +0x1C
    void Start() override
    {
        CTRL::Start();
        pxf->pvA8 = ppair;
    }
};
}  // namespace

// orig: 0x00186930
void InitAloPairRest(ALOPAIR* q)
{
    XF* pupper = q->apxf[1];
    XF* plower = q->apxf[2];
    XF* pend = q->apxf[3];
    if (!pupper || !plower || !pend) return;
    q->matUpperRest = pupper->mat;
    q->matLowerRest = plower->mat;
    const glm::vec3 posLower = PosOfWorld(plower);
    const glm::vec3 d2 = PosWorld(pend, q->vecB0) - posLower;
    const glm::vec3 d1 = posLower - PosOfWorld(pupper);
    q->sLower = std::sqrt(glm::dot(d2, d2));
    q->sUpper = std::sqrt(glm::dot(d1, d1));
    glm::vec3 axis;
    q->radBendRest = RadBetweenVectors(d1, d2, &axis);
    q->axis = DirToLocal(plower->pxfRef, axis);
}

// orig: 0x00186820
void RemeasureAloPairs(ALO* palo)
{
    for (ALOPAIR& q : palo->apair)
    {
        if (!q.apxf[1] || !q.apxf[2] || !q.apxf[3]) continue;
        const glm::vec3 posLower = PosOfWorld(q.apxf[2]);
        const glm::vec3 d2 = PosWorld(q.apxf[3], q.vecB0) - posLower;
        q.sLower = std::sqrt(glm::dot(d2, d2));
        const glm::vec3 d1 = posLower - PosOfWorld(q.apxf[1]);
        q.sUpper = std::sqrt(glm::dot(d1, d1));
    }
}

// orig: 0x00186A50
void SolveAloPair(ALO* palo, ALOPAIR* q)
{
    (void)palo;
    XF* pupper = q->apxf[1];
    XF* plower = q->apxf[2];
    XF* ptarget = q->apxf[0];
    if (!pupper || !plower || !ptarget) return;
    // the upper bone back to its rest local transform
    pupper->mat = q->matUpperRest;
    const XF* pparent = pupper->pxfRef;
    const glm::vec3 posUpper = PosWorld(pparent, glm::vec3(q->matUpperRest[3]));
    const glm::vec3 posTarget = PosWorld(ptarget, q->vecA0);
    const float l1 = q->sUpper, l2 = q->sLower;
    const glm::vec3 dTarget = posTarget - posUpper;
    const float dist = std::sqrt(glm::dot(dTarget, dTarget));
    float cosBend = ((dist * dist - l1 * l1) - l2 * l2) / ((l1 + l1) * l2);
    cosBend = std::clamp(cosBend, -1.0f, 1.0f);
    const float radBend = std::acos(cosBend);
    // the lower bone: its rest local transform turned about the axis by the change of bend
    const glm::mat3 M = MatRotateAxis(radBend - q->radBendRest, q->axis);
    const glm::mat3 lowerRest(q->matLowerRest);
    const glm::mat3 lower = M * lowerRest;  // (each rest row transformed by M)
    plower->mat[0] = glm::vec4(lower[0], 0.0f);
    plower->mat[1] = glm::vec4(lower[1], 0.0f);
    plower->mat[2] = glm::vec4(lower[2], 0.0f);
    plower->mat[3] = glm::vec4(glm::vec3(q->matLowerRest[3]), 1.0f);
    // the end point in the upper bone's parent's space through the chain's local transforms, then to the world
    glm::vec3 vEnd = q->vecB0;
    for (const XF* p = q->apxf[3]; p && p != pparent; p = p->pxfRef) vEnd = glm::vec3(p->mat * glm::vec4(vEnd, 1.0f));
    vEnd = PosWorld(pparent, vEnd);
    const glm::vec3 dEnd = vEnd - posUpper;
    // the turn bringing the end onto the target
    glm::vec3 axisTurnW;
    const float radTurn = RadBetweenVectors(dEnd, dTarget, &axisTurnW);
    const glm::vec3 axisTurn = DirToLocal(pparent, axisTurnW);
    const glm::mat3 A = MatRotateAxis(radTurn, axisTurn * (1.0f / std::sqrt(glm::dot(axisTurn, axisTurn))));
    // the weight eases (an ASEGA's scalar channel)
    if (q->ptrkgWeight && q->ptWeight) q->ptrkgWeight->Eval(&q->gWeight, nullptr, *q->ptWeight, 0.0f);
    // the twist about the target's direction by the weight
    const glm::vec3 dirTarget = DirToLocal(pparent, dTarget);
    const glm::mat3 B = MatRotateAxis(q->gWeight, dirTarget * (1.0f / std::sqrt(glm::dot(dirTarget, dirTarget))));
    // the upper bone: its rest rotation, turned, then twisted (rows: rest x A x B)
    const glm::mat3 upper = B * A * glm::mat3(q->matUpperRest);
    pupper->mat[0] = glm::vec4(upper[0], 0.0f);
    pupper->mat[1] = glm::vec4(upper[1], 0.0f);
    pupper->mat[2] = glm::vec4(upper[2], 0.0f);
    const int stamp = ++g_cXfStamp;
    pupper->nStamp = stamp;
    plower->nStamp = stamp;
}

void InitAloPairs(ALO* palo)
{
    for (ALOPAIR& q : palo->apair)
    {
        InitAloPairRest(&q);
        for (int i = 1; i <= 2; ++i)
        {
            XF* pxf = q.apxf[i];
            if (!pxf) continue;
            auto* p = new CTRLJOINT();
            p->kind = 7;
            p->palo = palo;
            p->pxf = pxf;
            p->prio = -1;
            p->mode = pxf != q.apxf[2] ? XFM_Joint : XFM_Joint14;
            p->ppair = &q;
            AddXfCtrl(palo, pxf, p);
        }
    }
}
