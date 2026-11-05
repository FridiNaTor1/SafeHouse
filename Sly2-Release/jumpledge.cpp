// jumpledge.cpp: the jump's ledge grab (the jump's animation state 17: hanging from a geometry edge the actor's
// custom entry touches while falling). From the original's code 0x199D48 / 0x1999D0 / 0x199408 / 0x1997D8 / 0x1995D8
// (the asm; Sly 1's PxpFindJtBestEdge / FCheckJtEdgeGrab / FCheckJtSingleEdge / FCheckJtEdgeNormal, Sly1-Release
// jump.cpp, are the same design) and the jump's state 17 parts of 0x195FB8 / 0x197500.
#include "jump.h"

#include "actor.h"
#include "alo.h"
#include "bsp.h"
#include "clock.h"
#include "lo.h"
#include "so.h"
#include "stepphys.h"
#include "xf.h"
#include "xfmath.h"
#include "vecmat.h"
#include "climb.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

void PairActorWith(ACTOR* pa, LO* plo, bool f);  // climb.cpp (0x204F78)

namespace
{
constexpr int INODE_ALL = 0x7FFFFFFF;

const SOE* PsoeOf(ALO* palo, int i) { return i >= 0 && size_t(i) < palo->asoe.size() ? &palo->asoe[i] : nullptr; }

bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_LEDGEPRINT") != nullptr;
    return s_f;
}

glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& vZero)
{
    const float l = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
    return l < 0.0001f ? vZero : v * (1.0f / l);
}

// orig: 0x00199408 FCheckJtSingleEdge (the edge between a floor (n.z > 0.7) and a wall (n.z < 0.55) of the other
// entry: its horizontal normal out of the wall)
bool FCheckSingleEdge(const SOE* psoe, const EDGE& e, glm::vec3* pnormal)
{
    const GEOM& g = psoe->geomWorld;
    if (e.isurf >= g.asurf.size() || e.isurfOther >= g.asurf.size()) return false;
    const SURF& s0 = g.asurf[e.isurf];
    const SURF& s1 = g.asurf[e.isurfOther];
    const SURF* pwall = nullptr;
    if (0.7f < s0.normal.z && s1.normal.z < 0.55f) pwall = &s1;
    else if (s0.normal.z < 0.55f && 0.7f < s1.normal.z) pwall = &s0;
    else return false;
    if (e.ipos0 >= g.apos.size() || e.ipos1 >= g.apos.size()) return false;
    const glm::vec3 d = g.apos[e.ipos1] - g.apos[e.ipos0];
    glm::vec3 n = NormalizeOr(glm::vec3(-d.y, d.x, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    if ((n.x * pwall->normal.x + n.y * pwall->normal.y) + n.z * pwall->normal.z < 0.0f) n = -n;
    *pnormal = n;
    return true;
}

// orig: 0x001995D8 (the 11 probe segments of the edge test in its frame: 0 front up, 1 toward the ledge, 2..10 fanned
// (-60..60 degrees) over the top; from the jump's limits +0x108..+0x11C)
void BuildEdgeProbes(const JUMP* pj, const glm::mat4& m, glm::vec3* apos)
{
    apos[0] = glm::vec3(pj->g114, 0.0f, pj->g11C);
    apos[1] = glm::vec3(pj->g114, 0.0f, pj->g118);
    apos[2] = glm::vec3(pj->g108, 0.0f, pj->g110);
    apos[3] = glm::vec3(pj->g114, 0.0f, pj->g11C);
    const glm::vec3 a(pj->g108, 0.0f, pj->g110), b(pj->g108, 0.0f, pj->g10C);
    int i = 4;
    for (int k = 8; k >= 0; --k)
    {
        const glm::mat3 r = MatRotateAxis(static_cast<float>(k - 4) * cmm::F32(0x3E860A92), glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B500
        apos[i++] = r * a;
        apos[i++] = r * b;
    }
    for (int k = 0; k < 22; ++k) apos[k] = glm::vec3(m * glm::vec4(apos[k], 1.0f));
}

// orig: 0x001997D8 FCheckJtEdgeNormal (the probes around the edge against the objects the actor overlaps: the first
// two pairs must be clear, the others must hit floors (n.z > 0.7) at least twice, none at its start)
bool FCheckEdgeNormal(JUMP* pj, const glm::vec3& posEdge, const glm::vec3& normal)
{
    ACTOR* pa = pj->pactor;
    // the frame: rows the normal, z x normal, normal x that (0x11E2D8), at the edge
    glm::vec3 across = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), normal);
    if (glm::dot(across, across) < 1e-12f) across = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
    across = NormalizeOr(across, glm::vec3(0.0f));
    const glm::vec3 up = NormalizeOr(glm::cross(normal, across), glm::vec3(0.0f));
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(normal, 0.0f);
    m[1] = glm::vec4(across, 0.0f);
    m[2] = glm::vec4(up, 0.0f);
    m[3] = glm::vec4(posEdge, 1.0f);
    glm::vec3 apos[22];
    BuildEdgeProbes(pj, m, apos);
    OXA* poxa = Poxa(pa->palo);
    if (!poxa) return false;
    int cFloor = 0;
    for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
    {
        OXA* poxaOther = pox->psoOther ? Poxa(pox->psoOther) : nullptr;
        if (!poxaOther || (poxaOther->grf & 4)) continue;
        for (int i = 0; i < 22; i += 2)
            for (SOE* psoe : poxaOther->apsoe)
            {
                if (psoe->f48) continue;
                LSG lsg;
                if (ClipSegmentEntry(psoe, apos[i], apos[i + 1], 1, &lsg) <= 0) continue;  // 0x14C6A0
                if (i < 4) return false;
                if (lsg.t0 == 0.0f) return false;
                if (0.7f < lsg.plane0.z) ++cFloor;
            }
    }
    return 2 <= cFloor;
}

// orig: 0x001999D0 FCheckJtEdgeGrab (the contact's feature on the other entry: a point (its edges), a surface (a floor:
// its edges inside the actor's custom entry), an edge; their normals summed; then the probes)
bool FCheckEdgeGrab(JUMP* pj, const SOE* psoe, const glm::vec3& posEdge, int xpk, const void* pv, glm::vec3* pnormal)
{
    ACTOR* pa = pj->pactor;
    const GEOM& g = psoe->geomWorld;
    switch (xpk)
    {
    case 1:
    {
        // a vertex: the edges at it (0x1665F0)
        const glm::vec3* ppos = static_cast<const glm::vec3*>(pv);
        if (!ppos || ppos < g.apos.data() || g.apos.data() + g.apos.size() <= ppos) return false;
        const size_t ipos = static_cast<size_t>(ppos - g.apos.data());
        glm::vec3 sum(0.0f);
        bool f = false;
        for (const EDGE& e : g.aedge)
        {
            if (e.ipos0 != ipos && e.ipos1 != ipos) continue;
            glm::vec3 n;
            if (!FCheckSingleEdge(psoe, e, &n)) continue;
            sum += n;
            f = true;
        }
        if (!f) return false;
        *pnormal = NormalizeOr(sum, glm::vec3(1.0f, 0.0f, 0.0f));
        break;
    }
    case 2:
    {
        // a floor surface: its edges and its neighbours' edges on it that cut the actor's custom entry (0x118668)
        const SURF* psurf = static_cast<const SURF*>(pv);
        if (!psurf || psurf < g.asurf.data() || g.asurf.data() + g.asurf.size() <= psurf) return false;
        if (psurf->normal.z < 0.7f) return false;
        const SOE* psoeCustom = PsoeOf(pa->palo, StepPhysOf(pa->palo).isoeCustom);
        glm::vec3 sum(0.0f);
        bool f = false;
        auto test = [&](const EDGE& e) {
            glm::vec3 n;
            if (!FCheckSingleEdge(psoe, e, &n) || !psoeCustom || psoeCustom->bsp.abspn.empty()) return;
            LSG alsg[2];
            if (ClipSegmentBsp(BSPREF(psoeCustom->bsp, psoeCustom->geomWorld), 0, INODE_ALL, g.apos[e.ipos0], g.apos[e.ipos1], 2, alsg) <= 0) return;
            sum += n;
            f = true;
        };
        for (int i = psurf->iedgeFirst; i >= 0 && static_cast<size_t>(i) < g.aedge.size(); i = g.aedge[i].iedgeNextInSurf) test(g.aedge[i]);
        for (int i = psurf->iedgeOfOthers; i >= 0 && static_cast<size_t>(i) < g.aedge.size(); i = g.aedge[i].iedgeNextOfOther) test(g.aedge[i]);
        if (!f) return false;
        *pnormal = NormalizeOr(sum, glm::vec3(1.0f, 0.0f, 0.0f));
        break;
    }
    case 3:
    {
        const EDGE* pe = static_cast<const EDGE*>(pv);
        if (!pe || pe < g.aedge.data() || g.aedge.data() + g.aedge.size() <= pe) return false;
        if (!FCheckSingleEdge(psoe, *pe, pnormal)) return false;
        break;
    }
    default: return false;
    }
    return FCheckEdgeNormal(pj, posEdge, *pnormal);
}

int Egk(ALO* palo) { return palo && palo->psort ? static_cast<int>(static_cast<int32_t>(static_cast<uint32_t>(palo->psort->grf148) << 12) >> 28) : 0; }
}  // namespace

// orig: 0x00199D48 (the best ledge among the actor's contacts while falling: an edge of the other entry the custom
// entry touches (point-surface with the point the other's, edge-surface with the surface the other's, edge-edge), not
// of an object marked 2 (+0x148's bits 16..19 up its parents), moving down onto it, with room behind it (the point 60
// back and 70 up outside the other entry), the highest; *pside the actor's end of the contact, *pnormal the normal)
XP* PxpFindJumpLedge(JUMP* pj, int* pside, glm::vec3* pnormal)
{
    ACTOR* pa = pj->pactor;
    if (pa->n270 > 0 || pa->prunScript) return nullptr;  // 0x144068
    // (the states 1 / 3..5, not the kinds 15 / 16)
    const int st = pj->state;
    if (st != 1 && !(3 <= st && st < 6)) return nullptr;
    if (15 <= pj->kind && pj->kind < 17) return nullptr;
    OXA* poxa = Poxa(pa->palo);
    if (!poxa) return nullptr;
    XP* pxpBest = nullptr;
    float zBest = 0.0f;
    for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
        for (XP* pxp = pox->pxp; pxp; pxp = pxp->pxpNext)
        {
            const int side = pxp->axpd[0].paloRoot != pa->palo ? 1 : 0;  // the actor's end
            const XPD& xo = pxp->axpd[1 - side];
            const SOE* psoe = xo.psoe;
            if (!psoe) continue;
            {
                bool fSkip = false;
                for (ALO* p = psoe->palo; p; p = p->paloParent)
                {
                    const int k = Egk(p);
                    if (k == 0) continue;
                    fSkip = k == 2;
                    break;
                }
                if (fSkip) continue;
            }
            const void* pv = nullptr;
            switch (pxp->xpk)
            {
            case 2: if (side == 1) continue; pv = pxp->pv1; break;
            case 1: if (side == 0) continue; pv = pxp->pv0; break;
            case 3: pv = side ? pxp->pv0 : pxp->pv1; break;
            default: continue;
            }
            glm::vec3 n;
            if (!FCheckEdgeGrab(pj, psoe, xo.pos, pxp->xpk, pv, &n)) continue;
            if ((poxa->grf & 2) || (Poxa(xo.paloRoot) && (Poxa(xo.paloRoot)->grf & 2))) UpdateXpWorldPositions(1, &pxp);  // 0x1A7FC0
            if (0.0f < (side ? -1.0f : 1.0f) * pxp->dv.z) continue;  // (0x2C3C60)
            const glm::vec3 q = xo.pos + n * -60.0f + glm::vec3(0.0f, 0.0f, 1.0f) * 70.0f;  // (0x2BF8A4 / 0x2BF8A8)
            if (!psoe->bsp.abspn.empty() && IbspFindPoint(BSPREF(psoe->bsp, psoe->geomWorld), 0, INODE_ALL, q) != IBSP_NONE) continue;  // 0x118FB0
            if (pxpBest && xo.pos.z < zBest) continue;
            pxpBest = pxp;
            zBest = xo.pos.z;
            *pside = side;
            *pnormal = n;
        }
    return pxpBest;
}

// the jump's state 17: the grab (0x197500's states 3 / 4 when falling: the ledge's entry +0xC8, the point in its record
// +0xD0, the kind +0x100 and feature +0x104, the normal +0xE0)
bool FJumpGrabLedge(JUMP* pj)
{
    int side = 0;
    glm::vec3 n(0.0f);
    XP* pxp = PxpFindJumpLedge(pj, &side, &n);
    if (!pxp) return false;
    const XPD& xo = pxp->axpd[1 - side];
    pj->psoeEdge = xo.psoe;
    const glm::mat4 m = xo.psoe->pxf ? xo.psoe->pxf->matWorld : glm::mat4(1.0f);
    pj->posEdgeLocal = glm::vec3(glm::inverse(m) * glm::vec4(xo.pos, 1.0f));  // 0x22FC48(null, record, pos)
    pj->xpkEdge = pxp->xpk;
    pj->pvEdge = side ? pxp->pv0 : pxp->pv1;
    pj->normalEdge = n;
    if (FPrint())
        std::printf("ledge t %.3f: %d grabs the edge of %d at %.1f %.1f %.1f (kind %d, normal %.3f %.3f %.3f)\n", g_clock.t, pj->pactor->palo->oid,
                    xo.psoe->palo ? xo.psoe->palo->oid : -1, xo.pos.x, xo.pos.y, xo.pos.z, pxp->xpk, n.x, n.y, n.z);
    SetJumpAnimState(pj, 17, nullptr);
    return true;
}

glm::vec3 PosJumpLedge(JUMP* pj);
glm::vec3 PosJumpLedge(JUMP* pj)
{
    const glm::mat4 m = pj->psoeEdge && pj->psoeEdge->pxf ? pj->psoeEdge->pxf->matWorld : glm::mat4(1.0f);
    return glm::vec3(m * glm::vec4(pj->posEdgeLocal, 1.0f));  // 0x22FC48(record, null, +0xD0)
}

// orig: 0x00197500's state 17 (the hang: CROSS after 0.25 s climbs up (the jump's kind 3); the ledge's object gone or
// the edge no longer a ledge (after 0.03 s) lets go (state 4); facing the wall; the draw offset smoothed away)
bool FCheckJumpLedgeHold(JUMP* pj)
{
    if (!pj->psoeEdge || !pj->psoeEdge->palo || !Poxa(pj->psoeEdge->palo)) return false;
    const glm::vec3 pos = PosJumpLedge(pj);
    if (cmm::F32(0x3CF5C28F) < g_clock.t - pj->t20 &&
        !FCheckEdgeGrab(pj, pj->psoeEdge, pos, pj->xpkEdge, pj->pvEdge, &pj->normalEdge))
        return false;
    return true;
}

// the hang point: the edge less the offset +0xF0 in the wall's frame (rows -normal, z x that, ...: 0x11E2D8)
glm::vec3 PosJumpHang(JUMP* pj)
{
    const glm::vec3 pos = PosJumpLedge(pj);
    const glm::vec3 dir = -pj->normalEdge;
    glm::vec3 across = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), dir);
    if (glm::dot(across, across) < 1e-12f) across = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), dir);
    across = NormalizeOr(across, glm::vec3(0.0f));
    const glm::vec3 up = NormalizeOr(glm::cross(dir, across), glm::vec3(0.0f));
    return pos - (dir * pj->vecF0.x + across * pj->vecF0.y + up * pj->vecF0.z);
}

// orig: 0x001991A0 (vt slot 59's part in state 17: the actor's centre pinned to the hang point by three internal
// contacts, through slot 82 0x140C30: the ledge's object out of the world lets go (the jump's kind 0); the draw offset
// taken off the point (0x205008))
void AddJumpLedgeContacts(ACTOR* pa)
{
    JUMP* pj = &pa->jump;
    if (pj->state != 17 || !pj->psoeEdge) return;
    const glm::vec3 q = PosAloCenter(pa->palo);
    const glm::vec3 r = PosJumpHang(pj);
    ALO* plo = pj->psoeEdge->palo;
    if (!plo || !plo->fInWorld)
    {
        SetJumpKind(pj, 0, 0, 0);
        return;
    }
    AddActorLineXps(pa, 7, r - pa->vecDrawOffset, glm::vec3(0.0f, 1.0f, 0.0f), q);  // (0x2A2DB0)
}

// orig: 0x00199298 (vt slot 58's part in state 17: the actor's contacts with the ledge's object dropped, but the
// internal kind 11)
void PruneJumpLedgeXps(ACTOR* pa)
{
    JUMP* pj = &pa->jump;
    if (pj->state != 17 || !pj->psoeEdge || !pj->psoeEdge->palo) return;
    OXA* poxaLedge = Poxa(pj->psoeEdge->palo);
    OXA* poxa = Poxa(pa->palo);
    if (!poxaLedge || !poxa) return;
    ALO* psoLedge = poxaLedge->pso;  // (TODO(physics): the root's vt predicate: all the static world's then)
    XP* pxpFreed = nullptr;
    for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
    {
        XP** link = &ox->pxp;
        while (XP* x = *link)
        {
            if (x->xpk == 11) { link = &x->pxpNext; continue; }
            const int side = x->axpd[0].paloRoot != pa->palo ? 1 : 0;
            if (x->axpd[1 - side].paloRoot == psoLedge)
            {
                *link = x->pxpNext;
                x->pxpNext = pxpFreed;
                pxpFreed = x;
            }
            else link = &x->pxpNext;
        }
        if (ox->poxOther) ox->poxOther->pxp = ox->pxp;
    }
    FreeSwXpList(pxpFreed);  // 0x1A7E10
}

// the hang's entry (0x195FB8 entering 17: the actor's slot 91, paired with the ledge's object, drawn at the hang point
// (the edge less the offset +0xF0 in the wall's frame) while the physics position follows, no own velocity)
void EnterJumpLedge(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    PairActorWith(pa, pj->psoeEdge ? pj->psoeEdge->palo : nullptr, true);  // 0x204F78
    pa->vecDrawOffset = PosJumpHang(pj) - PosAloCenter(pa->palo);  // +0x1010 (TODO(render): drawn offset)
    pa->vecVel = glm::vec3(0.0f);
    pj->f28 = 0;
}

void LeaveJumpLedge(JUMP* pj)
{
    PairActorWith(pj->pactor, pj->psoeEdge ? pj->psoeEdge->palo : nullptr, false);  // 0x204F78
    pj->psoeEdge = nullptr;
}
