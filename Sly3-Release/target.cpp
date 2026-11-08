// The actors' targets (orig 0x140558 each frame of the actor's update: the guards of the 0x3E1444 list ranked by
// 0x208E70; the nearest and the best kept by vt slot 95 0x140678 at +0x10DC / +0x10E4 and +0x10E8 / +0x10EC): target.h.

#include "clock.h"
#include "game/sly3.h"
#include "target.h"
#include "activate.h"

#include "actor.h"
#include "ai.h"
#include "aihit.h"
#include "aijobs.h"
#include "brx.h"
#include "cls.h"
#include "disc.h"
#include "guard.h"
#include "interact.h"
#include "jt.h"
#include "lo.h"
#include "po.h"
#include "so.h"
#include "stepphys.h"
#include "sw.h"
#include "vecmat.h"
#include "xf.h"
#include "xfphys.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

bool FPointInsideShape(LO* plo, const glm::vec3& pos);  // detect.cpp (0x227C90)

void LookScore(ACTOR* pa, LO* po, LO* const* ppoCur, float* pdxy, float* pdz);
namespace
{
void WarnOnce(const std::string& what)
{
    static std::set<std::string> s_set;
    if (s_set.insert(what).second) std::fprintf(stderr, "target: %s not ported\n", what.c_str());
}

// (0x1A62E8, vt slot 31: an ALO's center; another object's world position, e.g. an interest point cid 0x15)
glm::vec3 PosOf(LO* plo) { return FIsLoDerivedFrom(plo, S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(plo)) : PosLoWorld(plo); }


// orig: 0x00208E70 (the guard's rank for the actor: 4 the actor is the object of its grab / carry, 3 inside its
// volume +0x3F4 (+0x3F8 for the current one of kind 3), 2 near (within 700, height -100..350), 1 far (within 3000,
// height -800..800), 0; *pg the distance)
int NRankGuard(GUARD* pg, ACTOR* pa, float* pgDist)
{
    float dxy = 0.0f, dz = 0.0f;
    LookScore(pa, pg->palo, &pa->plo10DC, &dxy, &dz);
    *pgDist = dxy;
    // (the guard's ranges +0x364..+0x388: the reference's Paris guards all have 1, (0, 0), (-100, 350), 1000, 1,
    // (-800, 800), 3000: TODO(ai) the guards' options)
    const int f364 = 1, f37C = 1;
    const float g368[2] = {0.0f, 0.0f}, g370[2] = {-100.0f, 350.0f}, g378 = 1000.0f, g380[2] = {-800.0f, 800.0f},
                g388 = 3000.0f;
    if (!FGuardState(pg, 1))
    {
        if (f364 && pa->f10F0)
        {
            LO* po = nullptr;
            // the object of its pursuit (state 0x1D), of its hit jobs 0xA1 / 0xA2 (the hit's source +0x50), of its
            // launch 0xB1 (the launcher +0x5C: when it is the actor, 500 nearer, D_002C3D90)
            if (FGuardState(pg, 0x1D)) po = pg->plo38C;
            else if (FGuardState(pg, 5))
            {
                if (auto* pj = dynamic_cast<JOBHIT*>(PjobFind(pg, 0xA1))) po = pj->hit.psource;
            }
            else if (FGuardState(pg, 6))
            {
                if (auto* pj = dynamic_cast<JOBHIT*>(PjobFind(pg, 0xA2))) po = pj->hit.psource;
            }
            else if (FGuardState(pg, 0x17))
            {
                if (auto* pj = dynamic_cast<JOBB1*>(PjobFind(pg, 0xB1)))
                {
                    po = pj->ploOwner;
                    if (po == pa->palo) *pgDist += -500.0f;
                }
            }
            if (po && (po == pa->palo || FIsLoDerivedFrom(po, S3CID(0x34)))) return 4;
        }
        LO* pl = (static_cast<LO*>(pg->palo) == pa->plo10DC && pa->n10E4 == 3) ? pg->plo3F8 : pg->plo3F4;
        if (pl && pl->fInWorld && FPointInsideShape(pl, PosOf(pa->palo))) return 3;
    }
    const float* lm = FGuardState(pg, 0x17) ? g368 : g370;
    if (dxy <= g378 * 0.7f && lm[0] <= dz && dz <= lm[1] && f364) return 2;  // (0x2C3D94; 0x11D910)
    if (dxy <= g388 && g380[0] <= dz && dz <= g380[1] && f37C) return 1;
    return 0;
}
// orig: 0x0011EE08 (distance from a point to the target's predicted segment)
float AttackTargetDistance(const glm::vec3& pos, const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a, w = pos - a;
    const float dd = (d.x * d.x + d.y * d.y) + d.z * d.z;
    // A stationary target has one endpoint. Avoid the host's 0/0 NaN; the
    // original's finite divide result is clamped before forming this endpoint.
    float t = dd == 0.0f ? 0.0f : ((w.x * d.x + w.y * d.y) + w.z * d.z) / dd;
    if (t < 0.0f) t = 0.0f;
    else if (1.0f < t) t = 1.0f;
    const glm::vec3 v = pos - (a + d * t);
    return std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
}

}  // namespace

// orig: 0x0018F2B0 (the attack point's mask, predicted distance and facing cone)
LO* ChooseAttackTarget(ACTOR* pa, float heading, uint32_t mask, const glm::vec3& offset,
                       float dt, float cone, glm::vec3* pdir)
{
    const glm::mat3 rotation = cmm::BuildRotateMatrixAxis(heading, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 pos = PosJtLo(pa->palo);
    const glm::vec3 origin = pos + VelAloPos(pa->palo) * dt;
    const glm::vec3 aim = rotation * offset + origin;
    LO* best = nullptr;
    float nearest = 300.0f;
    // Original list membership follows activation. The native list reconstructs
    // class/creation order, so ties across activation reorderings remain a known
    // shared-list limitation; do not reorder by distance or object identity here.
    for (LO* point : AploJtList(g_psw, JTL_Point))
    {
        // orig: 0x21D278 initializes point flags before any authored override.
        if (point->iupd < 0 || !(point->U32Field(0, 0xE8, 0x17E) & mask)) continue;
        const glm::vec3 current = PosJtLo(point);
        XF* frame = nullptr;
        glm::mat4 local(1.0f);
        if (FIsLoDerivedFrom(point, S3CID(0x30))) frame = PxfObj(static_cast<ALO*>(point));
        else if (FIsLoDerivedFrom(point, S3CID(3)))
        {
            const LOX* located = static_cast<LOX*>(point);
            frame = located->pxf;
            local = located->mat;
        }
        glm::mat4 predicted(1.0f);
        PredictXfChain(frame, nullptr, dt, &predicted, nullptr);  // 0x2309E8, slot 32's frame
        const glm::vec3 endpoint(predicted * local[3]);
        float distance = AttackTargetDistance(aim, current, endpoint);
        // 0x15ED78(3, 0x33, point): a parent in the player class family.
        for (LO* parent = point->paloParent; parent; parent = parent->paloParent)
            if (FIsLoDerivedFrom(parent, S3CID(0x33)))
            {
                distance += cmm::F32(g_disc.U32Elf(S3VA(0x2BF4B8)));
                break;
            }
        if (!(distance < nearest)) continue;
        const glm::vec3 delta = current - pos;
        const float angle = std::fabs(cmm::RadNormalize(std::atan2(delta.y, delta.x) - heading));
        if (cone < angle) continue;
        best = point;
        nearest = distance;
        // The original returns the predicted frame's origin, without the point's
        // local offset used for scoring above.
        *pdir = glm::vec3(predicted[3]) - origin;
    }
    return best;
}
namespace
{
}  // namespace

// orig: 0x001428D8 (how far po is from the actor for its look and its targets: the horizontal distance (the current
// target's (never: the original compares the actor with its own +0x10DC) less 200 + 0.2 d) less, with the stick
// pushed toward po, up to 500 + d / 2; the height difference)
void LookScore(ACTOR* pa, LO* po, LO* const* ppoCur, float* pdxy, float* pdz)
{
    const glm::vec3 d = PosOf(pa->palo) - PosOf(po);
    float dxy = std::sqrt(d.x * d.x + d.y * d.y);
    float g22 = 0.0f, g23 = 0.0f;
    if (static_cast<LO*>(pa->palo) == *ppoCur)
    {
        g22 = -200.0f + dxy * (-0.2f + dxy * 0.0f);  // (0x2BD280)
        g23 = 0.0f + d.z * (-0.2f + d.z * 0.0f);     // (0x2BD290)
    }
    PODATA* ppd = PpodataFromLo(pa->palo);
    if (ppd && ppd->ppadctl && 1e-4f < dxy)
    {
        const PADCTL& pc = *ppd->ppadctl;
        const glm::vec3 v(std::cos(pc.radJoy) * pc.uJoy, std::sin(pc.radJoy) * pc.uJoy, 0.0f);  // 0x11E1A0
        const float u = (v.x * d.x + v.y * d.y + v.z * d.z) / dxy;
        const float c = std::clamp(0.5f + u * (-0.5f + u * 0.0f), 0.0f, 1.0f);  // (0x2BD2A0, 0x2BBB60)
        const float h = -500.0f + dxy * (-0.5f + dxy * 0.0f);                // (0x2BD2B0)
        g22 += c * h;
    }
    *pdxy = dxy + g22;
    *pdz = d.z + g23;
}

// orig: 0x0018F688 ChooseJtRushTarget
void ChooseSlyRushTarget(SLY* psly)
{
    const PODATA* data = PpodataFromLo(psly->palo);
    const PADCTL* pad = data ? data->ppadctl : nullptr;
    float heading = pad && 0.5f < pad->uJoy ? pad->radJoy : psly->radHeading;
    const glm::vec3 offset(cmm::F32(g_disc.U32Elf(S3VA(0x2BF4D0))), cmm::F32(g_disc.U32Elf(S3VA(0x2BF4D4))),
                           cmm::F32(g_disc.U32Elf(S3VA(0x2BF4D8))));
    glm::vec3 direction(0.0f);
    psly->plo18C0 = ChooseAttackTarget(psly, heading, 8, offset, 0.25f, 1.0f, &direction);
    if (psly->plo18C0) heading = std::atan2(direction.y, direction.x);
    SetStepPhysHeading(psly->palo, heading);
    TurnActorNow(psly);
}

// orig: 0x0018F740 (the air swing's target, state 13: a point of mask 4 within 300 of the swing's aim, any direction
// (cone 3.15) from the step's heading +0x1A8; found: the actor turns toward it at once)
void ChooseSlyAirSwingTarget(SLY* psly)
{
    const glm::vec3 offset(cmm::F32(g_disc.U32Elf(S3VA(0x2BF4E0))), cmm::F32(g_disc.U32Elf(S3VA(0x2BF4E4))),
                           cmm::F32(g_disc.U32Elf(S3VA(0x2BF4E8))));
    glm::vec3 direction(0.0f);
    psly->plo18C0 = ChooseAttackTarget(psly, psly->radHeading, 4, offset, 0.25f, 3.15f, &direction);
    if (psly->plo18C0)
    {
        SetStepPhysHeadingToward(psly->palo, direction);  // 0x2046B0
        TurnActorNow(psly);                               // 0x204CA8
    }
}

// orig: 0x001431D0: the nearest's (+0x10DC) launch job 0xB1 by this actor stops tracking it (+0x60)
void StopNearestLaunchTracking(ACTOR* pa)
{
    if (!pa->plo10DC) return;
    if (GUARD* pg = PguardFromLo(pa->plo10DC))
        if (auto* pj = dynamic_cast<JOBB1*>(PjobFind(pg, 0xB1)); pj && pj->ploOwner == pa->palo) pj->fTrack = 0;
}

// orig: 0x00140678 (vt slot 95: the nearest (+0x10DC, its rank +0x10E4) and the best (+0x10E8, +0x10EC))
void SetActorTargets(ACTOR* pa, int nNear, LO* ploNear, int nBest, LO* ploBest)
{
    if (ploNear != pa->plo10DC) StopNearestLaunchTracking(pa);  // 0x1431D0
    if (std::getenv("SLY2N_TARGETPRINT") && (nBest != pa->n10EC || ploBest != pa->plo10E8 || nNear != pa->n10E4 || ploNear != pa->plo10DC))
        std::printf("targets %d t %.3f: near %d (%d) best %d (%d)\n", pa->palo ? pa->palo->oid : -1, g_clock.t, ploNear ? ploNear->oid : -1, nNear,
                    ploBest ? ploBest->oid : -1, nBest);  // (debug: the actor's target changes)
    pa->n10EC = nBest;
    pa->plo10DC = ploNear;
    pa->n10E4 = nNear;
    pa->plo10E8 = ploBest;
}

// orig: 0x00140558
void PickActorTargets(ACTOR* pa)
{
    if (pa->grf10E0) return;
    float g0 = FLT_MAX, g1 = FLT_MAX;
    LO* p0 = nullptr, * p1 = nullptr;
    int n0 = -1, n1 = -1;
    for (GUARD* pg : ApguardActive())
    {
        LO* plo = pg->palo;
        float g = 0.0f;
        const int n = NRankGuard(pg, pa, &g);
        static const bool s_fDbg = std::getenv("SLY2N_TARGETDEBUG") != nullptr;
        if (s_fDbg)
            std::printf("target: t %.4f guard %d %p rank %d dist %.1f pos %.1f %.1f %.1f state 0x%llX 38C %d 3F4 %d %p at %.0f %.0f %.0f in %d\n", g_clock.t, plo->oid, (void*)plo, n, g,
                        PosOf(plo).x, PosOf(plo).y, PosOf(plo).z, static_cast<unsigned long long>(pg->grfState),
                        pg->plo38C ? pg->plo38C->oid : -1, pg->plo3F4 ? pg->plo3F4->oid : -1, (void*)pg->plo3F4,
                        pg->plo3F4 ? PosOf(pg->plo3F4).x : 0.0f, pg->plo3F4 ? PosOf(pg->plo3F4).y : 0.0f, pg->plo3F4 ? PosOf(pg->plo3F4).z : 0.0f,
                        pg->plo3F4 ? pg->plo3F4->fInWorld : -1);
        if (g < g0)
        {
            g0 = g;
            n0 = n;
            p0 = plo;
        }
        if (n1 < n || (n == n1 && g < g1))
        {
            g1 = g;
            n1 = n;
            p1 = plo;
        }
    }
    SetActorTargets(pa, n0, p0, n1, p1);
}
