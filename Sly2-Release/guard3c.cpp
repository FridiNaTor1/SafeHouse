// Class 0x3C's own update (Neyla): orig 0x1BA128 / 0x1BA408; its post-load parts are in guardstart.cpp (0x1BA018).
#include "ai.h"
#include "aijobs.h"
#include "guard.h"
#include "lo.h"
#include "clock.h"
#include "animfind.h"
#include "aseg.h"
#include "asega.h"
#include "detect.h"
#include "jt.h"
#include "mat.h"
#include "so.h"
#include "stepphys.h"
#include "sw.h"
#include "update.h"
#include "xf.h"
#include "xfphys.h"
#include <cfloat>
#include <cstdio>
#include <cstdlib>

// The guard class 0x3C (Neyla; SCPT 1855 drives her in job 1852): its update 0x1BA128 is the guards' base 0x207350
// (guard.cpp), then her strike: while she runs her command list near Sly (job 0xAE: state 0x25), the nearest guard
// inside her volume (+0x1010 while walking the list (state 9), +0x1014 while watching Sly (state 2)), else (walking) a
// breakable (class 0x62) in the box around the point 400 ahead of her, becomes the target of her strike (the ASEG
// part 0x5D8, her turn transform aimed at it). Natively only the base ran (N43: she walked past them).
namespace
{
LO* PloSly3C()
{
    if (!g_psw) return nullptr;
    for (LO* p : g_psw->mpcidaplo[0x35])
        if (!p->ploCloneOf) return p;
    return nullptr;
}

// orig: 0x001BA408 (the strike's state: leaving 1 the play retracted and released, the turn controller deleted, the
// target cleared, the watch (0x9E) back on Sly and the speed scale 1; entering 1 the ASEG applied (grf 1) from 0 at
// speed 1, the turn controller (priority 5, mode 8, state 3) on 0x5DA, watching the target at the scale 4)
void SetStrikeState(GUARD* pg, int st)
{
    if (st == pg->stStrike) return;
    ALO* palo = pg->palo;
    if (pg->stStrike == 1)
    {
        RetractAsegaPp(&pg->pasegaStrike);
        DeleteCtrl(reinterpret_cast<CTRL**>(&pg->pctrlStrike));
        pg->ploStrike = nullptr;
        if (FGuardState(pg, 2))
            if (auto* pj = static_cast<JOBMOVE*>(PjobFind(pg, 0x9E))) pj->ploFace = PloSly3C();
        SetStepPhysScale(palo, 1.0f);
    }
    pg->stStrike = st;
    pg->tStrike = g_clock.t;
    if (std::getenv("SLY2N_STRIKEPRINT"))  // (debug: Neyla's strikes)
        std::printf("strike %d: state %d t %.3f target %d (cid 0x%X)\n", palo->oid, st, g_clock.t, pg->ploStrike ? pg->ploStrike->oid : -1,
                    pg->ploStrike ? pg->ploStrike->Cid() : 0);
    if (st != 1) return;
    if (pg->pasegStrike) ApplyAseg(pg->pasegStrike, palo, 0.0f, 1.0f, 1, &pg->pasegaStrike);
    if (pg->pxfStrikeTurn) pg->pctrlStrike = PctrlNewPos(palo, pg->pxfStrikeTurn, 5, 8, 3, glm::vec4(0.0f));
    if (FGuardState(pg, 2))
    {
        if (auto* pj = static_cast<JOBMOVE*>(PjobFind(pg, 0x9E))) pj->ploFace = pg->ploStrike;
        SetStepPhysScale(palo, 4.0f);
    }
}

void UpdateGuard3C(LO* plo)
{
    static int s_c = 0;
    if (std::getenv("SLY2N_SCPTPRINT") && s_c++ < 3)
        if (GUARD* pg = PguardFromLo(plo))
        {
            std::fprintf(stderr, "ai: guard 0x3C %d update: mode %d inworld %d jobs", plo->oid, pg->nMode, plo->fInWorld);
            for (auto& j : pg->stack.ajob) std::fprintf(stderr, " 0x%X", j->cid);
            std::fprintf(stderr, "\n");
        }
    if (PFNUPDATE pfn = PfnUpdateOrig(0x207350)) pfn(plo);
    GUARD* pg = PguardFromLo(plo);
    if (!pg || !pg->palo) return;
    ALO* palo = pg->palo;
    switch (pg->stStrike)
    {
    case 0:
    {
        if (!FGuardState(pg, 0x25)) break;
        LO* pvol = nullptr;
        if (FGuardState(pg, 9)) pvol = PloFindSwNearest(plo, static_cast<OID>(plo->U32Field(0, 0x1010, 0xFFFFFFFFu)));
        else if (FGuardState(pg, 2)) pvol = PloFindSwNearest(plo, static_cast<OID>(plo->U32Field(0, 0x1014, 0xFFFFFFFFu)));
        else break;
        pg->ploStrike = nullptr;
        if (!pvol) break;
        float sNearest = FLT_MAX;  // (0x2A7620)
        const glm::vec3 pos = PosAi(palo);
        for (GUARD* po : ApguardActive())
        {
            if (po == pg || !po->palo || FGuardState(po, 7)) continue;
            const glm::vec3 d = pos - PosAi(po->palo);
            const float s = glm::dot(d, d);
            if (sNearest < s) continue;
            if (FPointInShape(pvol, PosAi(po->palo)))  // 0x227C90
            {
                pg->ploStrike = po->palo;
                sNearest = s;
            }
        }
        if (!pg->ploStrike && !FGuardState(pg, 2))
        {
            // 0x20FAA0 over the box 400 ahead (0x1A6F00 of (400, 0, 70)) +-(500, 500, 300), the filter 0x1BA100 (class
            // 0x62): the first inside the volume
            const glm::vec3 c = PosJtConvert(palo, nullptr, glm::vec3(400.0f, 0.0f, 70.0f));
            const glm::vec3 mn = c + glm::vec3(-500.0f, -500.0f, -300.0f), mx = c + glm::vec3(500.0f, 500.0f, 300.0f);
            int cRow = 0;
            for (OXA* poxa : g_physw.apoxaActive)
            {
                if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
                    mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
                    continue;
                for (SOE* psoe : poxa->apsoe)
                {
                    // (0x20FAA0 keeps at most 0x100 rows, counting only those the filter 0x1BA100 passes)
                    if (!psoe->palo || psoe->palo->Cid() != 0x62) continue;
                    if (cRow >= 0x100) break;
                    ++cRow;
                    if (FPointInShape(pvol, PosAi(psoe->palo)))
                    {
                        pg->ploStrike = psoe->palo;
                        break;
                    }
                }
                if (pg->ploStrike) break;
            }
        }
        if (pg->ploStrike) SetStrikeState(pg, 1);
        break;
    }
    case 1:
    {
        if (!pg->pasegaStrike || FAsegaRetracted(pg->pasegaStrike))  // 0x131968
        {
            SetStrikeState(pg, 0);
            break;
        }
        if (!pg->pctrlStrike || !pg->ploStrike || !pg->pxfStrikeTurn) break;
        XF* pxf = pg->pxfStrikeTurn;
        const glm::vec3 local = PosConvertXfs(nullptr, pxf->pxfRef, PosAi(pg->ploStrike));  // 0x22FC48
        const glm::vec3 dir = local - glm::vec3(pxf->mat[3]);
        const glm::mat3 m = BuildRotateVectorsMatrix(pg->vecStrikeAxis, dir);  // 0x11BA08
        pg->pctrlStrike->pos = glm::vec4(DecomposeEulerMatrix(m), 0.0f);        // 0x11BFF0 (into +0x20)
        break;
    }
    default: break;
    }
}
struct INITGUARD3C
{
    INITGUARD3C() { RegisterLoUpdate(0x1BA128, UpdateGuard3C); }
} s_initguard3c;
}  // namespace
