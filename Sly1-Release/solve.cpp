#include "solve.h"
#include <cstdio>
#include <cstdlib>

namespace
{
    constexpr int cpsoRenumberMax = 48;

    // SolveSwGroup's working storage, kept between calls so that a solve does
    // not allocate.  It is leased by nesting depth: ApplySoImpulse can start a
    // group solve from a callback while an outer solve's DZG is still live.
    struct SOLVESCRATCH
    {
        std::vector<int> acpso;
        std::vector<SO*> apsoRenumber;
        std::vector<SO**> aapso;
        DZG dzg;
    };

    thread_local std::vector<std::unique_ptr<SOLVESCRATCH>> s_apsolvescratch;
    thread_local size_t s_csolvescratchActive = 0;

    struct SOLVESCRATCHLEASE
    {
        SOLVESCRATCH* pscratch;

        SOLVESCRATCHLEASE()
        {
            if (s_csolvescratchActive == s_apsolvescratch.size())
                s_apsolvescratch.push_back(std::make_unique<SOLVESCRATCH>());

            pscratch = s_apsolvescratch[s_csolvescratchActive++].get();
        }

        ~SOLVESCRATCHLEASE()
        {
            s_csolvescratchActive--;
        }

        SOLVESCRATCHLEASE(const SOLVESCRATCHLEASE&) = delete;
        SOLVESCRATCHLEASE& operator=(const SOLVESCRATCHLEASE&) = delete;
    };
}

void SolveSwGroup(SW* psw, int cpsoRoot, SO** apsoRoot, int cpxp, XP** apxp, float dt, GRFSG grfsg)
{
    if ((grfsg & GRFSG_Pivot) == 0) {
        if ((grfsg & GRFSG_Penetration) != 0) {
            bool needsPenetrationSolve = false;

            for (int i = 0; i < cpxp; ++i) {
                XP* pxp = apxp[i];

                if (pxp->xpk == XPK_Equality) {
                    needsPenetrationSolve = fabsf(pxp->gDist) > 0.1f;
                }
                else {
                    needsPenetrationSolve = pxp->gDist < -2.0f;
                }

                if (needsPenetrationSolve) {
                    break;
                }
            }

            if (!needsPenetrationSolve) {
                grfsg &= ~GRFSG_Penetration;
            }
        }

        if ((grfsg & GRFSG_Impact) != 0) {
            bool needsImpactSolve = false;

            for (int i = 0; i < cpxp; ++i) {
                XP* pxp = apxp[i];
                float vRelNormal = glm::dot(pxp->vRelAdj, pxp->normal);

                if (pxp->xpk == XPK_Equality) {
                    needsImpactSolve = fabsf(vRelNormal) > 5.0f;
                }
                else {
                    needsImpactSolve = vRelNormal < -5.0f;
                }

                if (needsImpactSolve) {
                    break;
                }
            }

            if (!needsImpactSolve) {
                grfsg &= ~GRFSG_Impact;
            }
        }
    }

    if (grfsg == 0) {
        return;
    }

    const bool fPivot = (grfsg & GRFSG_Pivot) != 0;

    SOLVESCRATCHLEASE lease;
    std::vector<int>& acpso = lease.pscratch->acpso;
    std::vector<SO**>& aapso = lease.pscratch->aapso;
    DZG& dzg = lease.pscratch->dzg;

    acpso.assign(cpsoRoot, 0);
    aapso.assign(cpsoRoot, nullptr);
    lease.pscratch->apsoRenumber.assign(static_cast<size_t>(cpsoRoot) * cpsoRenumberMax, nullptr);

    for (int i = 0; i < cpsoRoot; ++i) {
        SO* psoRoot = apsoRoot[i];

        psoRoot->ipsoRoot = i;
        aapso[i] = lease.pscratch->apsoRenumber.data() + static_cast<size_t>(i) * cpsoRenumberMax;
        psoRoot->pvtso->pfnRenumberSo(psoRoot, &acpso[i], aapso[i]);

        // Roots share one flat buffer, so an overflow would silently corrupt the
        // next root's slots instead of faulting; stop loudly in every build.
        if (acpso[i] > cpsoRenumberMax) {
            std::fprintf(stderr, "SolveSwGroup: root has %d SOs (max %d)\n", acpso[i], cpsoRenumberMax);
            std::abort();
        }
    }

    for (int i = 0; i < cpxp; ++i) {
        apxp[i]->ipxp = i;
    }

    InitDzg(&dzg, cpxp);

    for (int i = 0; i < cpxp; ++i) {
        XP* pxp = apxp[i];
        DZ* pdz = &dzg.adz[i];

        pdz->pxp = pxp;
        pdz->f = pxp->normal;

        if (pxp->xpk == XPK_Equality) {
            pdz->dzk = DZK_Equality;
        }
        else {
            pdz->dzk = DZK_Normal;

            if (pxp->xpk == XPK_Limited) {
                pdz->fMax = 1;
                pdz->sfMax = pxp->sfMax;
            }
        }

        AppendDzgDz(&dzg, pdz);
    }

    if ((grfsg & GRFSG_Penetration) != 0) 
    {
        for (int i = 0; i < cpxp; ++i) 
        {
            XP* pxp = apxp[i];
            DZ* pdz = &dzg.adz[i];

            pdz->sdv = (pdz->dzk == DZK_Equality) ? pxp->gDist : pxp->gDist + 1.0f;

            for (int j = 0; j < 2; ++j) {
                SO* psoHook = PsoFindSoPhysHook(pxp->axpd[j].psoLeaf, offsetof(VTSO, pfnAdjustSoDz));

                if (psoHook != nullptr && psoHook->pvtso->pfnAdjustSoDz != nullptr) 
                    psoHook->pvtso->pfnAdjustSoDz(psoHook, 12, pdz, j, dt);
            }

        }

        SolveDzg(&dzg, 4, cpxp, apxp, acpso.data(), aapso.data());

        ApplyDzg(&dzg, cpsoRoot, acpso.data(), aapso.data(), 100.0f, 1.0f);

        for (int i = 0; i < cpsoRoot; ++i) {
            apsoRoot[i]->pvtso->pfnDisplaceSo(apsoRoot[i], 0);
        }

        if (grfsg == GRFSG_Penetration) {
            goto cleanup;
        }

        ClearDzgSolution(&dzg);
    }

    if (!fPivot) {
        for (int i = 0; i < cpxp; ++i) {
            XP* pxp = apxp[i];
            DZ* pdz = &dzg.adz[i];

            pxp->svNormal = glm::dot(pxp->vRelAdj, pxp->normal);

            float restitutionScale = GRandInRange(0.9f, 1.1f);
            pdz->sdv = pxp->svNormal * (pxp->elas * restitutionScale + 1.0f);

            for (int j = 0; j < 2; ++j) {
                SO* psoHook = PsoFindSoPhysHook(pxp->axpd[j].psoLeaf, offsetof(VTSO, pfnAdjustSoDz));

                if (psoHook != nullptr && psoHook->pvtso->pfnAdjustSoDz != nullptr) {
                    psoHook->pvtso->pfnAdjustSoDz(psoHook, 10, pdz, j, dt);
                }
            }

        }
    }
    else 
    {
        RecalcSwXpPos(psw, cpxp, apxp);
        RecalcSwXpMovement(psw, cpxp, apxp);

        for (int i = 0; i < cpxp; ++i) {
            XP* pxp = apxp[i];
            DZ* pdz = &dzg.adz[i];

            pdz->sdv = glm::dot(pxp->vRelAdj, pxp->normal);

            for (int j = 0; j < 2; ++j) {
                SO* psoHook = PsoFindSoPhysHook(pxp->axpd[j].psoLeaf, offsetof(VTSO, pfnAdjustSoDz));

                if (psoHook != nullptr && psoHook->pvtso->pfnAdjustSoDz != nullptr) {
                    psoHook->pvtso->pfnAdjustSoDz(psoHook, 9, pdz, j, dt);
                }
            }
        }
    }


    SolveDzgFric(&dzg, grfsg & ~GRFSG_Penetration, cpxp, apxp, acpso.data(), aapso.data(), dt);
    ApplyDzg(&dzg, cpsoRoot, acpso.data(), aapso.data(), 5000.0f, 50.0f);

cleanup:
    for (int i = 0; i < cpsoRoot; ++i) {
        SO* psoRoot = apsoRoot[i];

        psoRoot->ipsoRoot = -1;

        if (fPivot) {
            psoRoot->pvtso->pfnPivotSo(psoRoot, 0);
        }
        else if ((grfsg & GRFSG_Impact) != 0) {
            psoRoot->pvtso->pfnImpactSo(psoRoot, 0);
        }

        for (int j = 0; j < acpso[i]; ++j) {
            aapso[i][j]->ipsoRoot = -1;
        }
    }

    if (!fPivot && (grfsg & GRFSG_Impact) != 0) {
        for (int i = 0; i < cpxp; ++i) {
            SetSwXpImpact(psw, apxp[i], dzg.adz[i].sf);
        }
    }
    else if (fPivot) {
        for (int i = 0; i < cpxp; ++i) {
            apxp[i]->sfPivot = dzg.adz[i].sf;
        }
    }

}

int CpxpBuildXpArray(int cpso, SO** apso, int cpxpMax, XP** apxp)
{
    if (cpso <= 0 || !apso || cpxpMax <= 0 || !apxp)
        return 0;

    int cpxp = 0;

    for (int i = 0; i < cpso; ++i)
    {
        SO* pso = apso[i];
        pso->fCpxpBuildArray = 1;

        for (XP* pxp = pso->pxpInternal; pxp; pxp = pxp->pxpNext)
        {
            if (cpxp >= cpxpMax)
                goto Cleanup;

            apxp[cpxp++] = pxp;
        }

        if (!pso->poxa)
            continue;

        for (OX* pox = pso->poxa->pox; pox; pox = pox->poxNext)
        {
            SO* psoOther = pox->psoOther;

            if (!psoOther || psoOther->fCpxpBuildArray)
                continue;

            for (XP* pxp = pox->pxp; pxp; pxp = pxp->pxpNext)
            {
                if (cpxp >= cpxpMax)
                    goto Cleanup;

                apxp[cpxp++] = pxp;
            }
        }
    }

    Cleanup:

    for (int i = 0; i < cpso; ++i)
        apso[i]->fCpxpBuildArray = 0;

    return cpxp;
}

void BuildContactGroup(SO* psoSeed, int* pcpso, SO** apso, int* pcpsoLocked, SO** apsoLocked)
{
    if (!psoSeed || !pcpso || !apso || !pcpsoLocked || !apsoLocked)
        return;

    int cpso = 1;
    int cpsoLocked = 0;

    apso[0] = psoSeed;
    psoSeed->fCpsoBuildContactGroup = 1;

    for (int ipso = 0; ipso < cpso; ++ipso)
    {
        SO* pso = apso[ipso];

        if (!pso->poxa)
            continue;

        for (OX* pox = pso->poxa->pox; pox; pox = pox->poxNext)
        {
            SO* psoOther = pox->psoOther;

            if (!pox->pxp || !psoOther || psoOther->fCpsoBuildContactGroup)
                continue;

            if (psoOther->fLockedAll)
                apsoLocked[cpsoLocked++] = psoOther;
            else
                apso[cpso++] = psoOther;

            psoOther->fCpsoBuildContactGroup = 1;
        }
    }

    for (int i = 0; i < cpsoLocked; ++i)
        apsoLocked[i]->fCpsoBuildContactGroup = 0;

    *pcpso = cpso;
    *pcpsoLocked = cpsoLocked;
}

void SplitSoFreeze(SO* pso, int fFreeze)
{
    if (fFreeze)
    {
        pso->frz.ctForce = pso->constrForce.ct;
        pso->frz.ctTorque = pso->constrTorque.ct;
        pso->frz.fLockedSelf = pso->fLockedSelf;
        pso->frz.fLockedAll = pso->fLockedAll;

        pso->fLockedSelf = 1;
        pso->fLockedAll = 1;
        pso->constrForce.ct = CT_Locked;
        pso->constrTorque.ct = CT_Locked;
    }
    else
    {
        pso->fLockedSelf = pso->frz.fLockedSelf;
        pso->fLockedAll = pso->frz.fLockedAll;
        pso->constrForce.ct = pso->frz.ctForce;
        pso->constrTorque.ct = pso->frz.ctTorque;
    }
}

void SortSwGroup(int cpso, SO** apso)
{
    if (cpso <= 1 || !apso)
        return;

    std::vector<SO*> sorted(static_cast<std::size_t>(cpso));
    std::vector<unsigned int> keys(static_cast<std::size_t>(cpso), 0);
    std::array<int, 5> bucketOffsets{};

    for (int i = 0; i < cpso; ++i)
    {
        SO* pso = apso[i];
        unsigned int key = 0;

        if (pso->fLockedAll && pso->poxa != nullptr)
        {
            for (OX* pox = pso->poxa->pox; pox; pox = pox->poxNext)
            {
                if (!pox->pxp)
                    continue;

                ++key;

                if (key >= 4)
                    break;
            }
        }

        keys[i] = key;
        ++bucketOffsets[key + 1];
    }

    for (std::size_t i = 1; i < bucketOffsets.size(); ++i)
        bucketOffsets[i] += bucketOffsets[i - 1];

    for (int i = 0; i < cpso; ++i)
    {
        const unsigned int key = keys[i];
        sorted[bucketOffsets[key]++] = apso[i];
    }

    std::copy(sorted.begin(), sorted.end(), apso);
}

void SplitSwGroup(SW* psw, int cpso, SO** apso, float dt, GRFSG grfsg)
{
    std::array<XP*, 64> apxp{};

    const int cpxp = CpxpBuildXpArray(cpso, apso, static_cast<int>(apxp.size()), apxp.data());

    if (cpxp < static_cast<int>(apxp.size()) || cpso < 2)
    {
        SolveSwGroup(psw, cpso, apso, cpxp, apxp.data(), dt, grfsg);
        return;
    }

    SortSwGroup(cpso, apso);

    const int cpsoFirst = cpso / 2;
    const int cpsoSecond = cpso - cpsoFirst;

    for (int i = 0; i < cpsoFirst; ++i)
        apso[i]->fCpxpBuildArray = 1;

    SplitSwGroup(psw, cpsoSecond, apso + cpsoFirst, dt, grfsg);

    for (int i = 0; i < cpsoFirst; ++i)
        apso[i]->fCpxpBuildArray = 0;

    for (int i = cpsoFirst; i < cpso; ++i)
    {
        DLI dli{};

        dli.m_pdl = &apso[i]->dlPhys;
        dli.m_ibDle = apso[i]->dlPhys.ibDle;
        dli.m_pdliNext = s_pdliFirst;

        SO* psoPhys = apso[i]->dlPhys.psoFirst;

        dli.m_ppv = psoPhys
            ? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(psoPhys) + dli.m_ibDle)
            : nullptr;

        s_pdliFirst = &dli;

        while (psoPhys)
        {
            SplitSoFreeze(psoPhys, 1);

            psoPhys = static_cast<SO*>(*dli.m_ppv);

            dli.m_ppv = psoPhys
                ? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(psoPhys) + dli.m_ibDle)
                : nullptr;
        }

        s_pdliFirst = dli.m_pdliNext;
    }

    SplitSwGroup(psw, cpsoFirst, apso, dt, grfsg);

    for (int i = cpsoFirst; i < cpso; ++i)
    {
        DLI dli{};

        dli.m_pdl = &apso[i]->dlPhys;
        dli.m_ibDle = apso[i]->dlPhys.ibDle;
        dli.m_pdliNext = s_pdliFirst;

        SO* psoPhys = apso[i]->dlPhys.psoFirst;

        dli.m_ppv = psoPhys
            ? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(psoPhys) + dli.m_ibDle)
            : nullptr;

        s_pdliFirst = &dli;

        while (psoPhys)
        {
            SplitSoFreeze(psoPhys, 0);

            psoPhys = static_cast<SO*>(*dli.m_ppv);

            dli.m_ppv = psoPhys
                ? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(psoPhys) + dli.m_ibDle)
                : nullptr;
        }

        s_pdliFirst = dli.m_pdliNext;
    }
}

void SolveSw(SW* psw, float dt, GRFSG grfsg)
{
    const int cpsoCapacity = std::max(psw->cpsoRoot, 0);

    // BuildContactGroup writes every entry it later reads, so the grow-only
    // buffers need no clearing between solves.
    thread_local std::vector <SO*> apso;
    thread_local std::vector <SO*> apsoLocked;
    thread_local std::vector <SO*> apsoMarked;

    if (apso.size() < static_cast<std::size_t>(cpsoCapacity))
    {
        apso.resize(static_cast<std::size_t>(cpsoCapacity));
        apsoLocked.resize(static_cast<std::size_t>(cpsoCapacity));
    }

    apsoMarked.clear();

    const unsigned int cSoRootChangeStart = psw->cSoRootChange;
    const bool fRootsUnchangedSinceSolve = cSoRootChangeStart == psw->cSoRootChangeSolved;

    if ((grfsg & 1) != 0)
    {
        DLI dliBusy{};

        dliBusy.m_pdl = &psw->dlBusy;
        dliBusy.m_ibDle = psw->dlBusy.ibDle;
        dliBusy.m_pdliNext = s_pdliFirst;

        ALO* palo = psw->dlBusy.paloFirst;

        dliBusy.m_ppv = palo != nullptr ? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(palo) + dliBusy.m_ibDle) : nullptr;
        s_pdliFirst = &dliBusy;

        while (palo != nullptr)
        {
            const bool fIsSo = palo->pvtalo != nullptr && (palo->pvtalo->grfcid & 2U) != 0;
            const bool fProject = !fIsSo || static_cast<SO*>(palo)->fLockedAll;

            if (fProject && palo->cframeStatic < 2)
                palo->pvtalo->pfnProjectAloTransform(palo, g_clock.dt, 0);

            palo = dliBusy.m_ppv != nullptr ? static_cast<ALO*>(*dliBusy.m_ppv) : nullptr;
            dliBusy.m_ppv = palo != nullptr ? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(palo) + dliBusy.m_ibDle) : nullptr;
        }

        s_pdliFirst = dliBusy.m_pdliNext;

    }

    if (cpsoCapacity > 0)
    {
        SO* pso = psw->dlBusySo.psoFirst;

        while (pso != nullptr)
        {
            SO* psoNext = pso->dleBusySo.psoNext;

            if (!pso->fCpsoBuildContactGroup && !pso->fLockedAll)
            {
                int cpsoGroup = 0;
                int cpsoLocked = 0;

                BuildContactGroup(pso, &cpsoGroup, apso.data(), &cpsoLocked, apsoLocked.data());
                apsoMarked.insert(apsoMarked.end(), apso.begin(), apso.begin() + cpsoGroup);

                bool fSolveGroup = (grfsg & 1) == 0;

                if (!fSolveGroup)
                {
                    for (int i = 0; i < cpsoGroup; ++i)
                    {
                        if (apso[i]->cframeStatic < 2)
                        {
                            fSolveGroup = true;
                            break;
                        }
                    }
                }

                if (!fSolveGroup)
                {
                    for (int i = 0; i < cpsoLocked; ++i)
                    {
                        if (apsoLocked[i]->cframeStatic < 2)
                        {
                            fSolveGroup = true;
                            break;
                        }
                    }
                }

                if (fSolveGroup)
                {
                    
                    if ((grfsg & 1) != 0)
                    {
                        for (int i = 0; i < cpsoGroup; ++i)
                        {
                            apso[i]->pvtalo->pfnProjectAloTransform(apso[i], g_clock.dt, 0);
                        }
                    }

                    SplitSwGroup(psw, cpsoGroup, apso.data(), dt, grfsg);

                    for (int i = 0; i < cpsoGroup; ++i)
                    {
                        if (!FIsAloStatic(apso[i]))
                            ResolveAlo(apso[i]);
                    }
                }
            }

            pso = psoNext;
        }
    }

    // Only SOs marked by this call can have the flag set in dlRoot, unless an
    // SO was added, removed or cloned since the last solve, so resetting just
    // those avoids a walk over every root in the world.
    if (fRootsUnchangedSinceSolve && psw->cSoRootChange == cSoRootChangeStart)
    {
        for (SO* pso : apsoMarked)
        {
            if (FFindDlEntry(&psw->dlRoot, pso))
                pso->fCpsoBuildContactGroup = 0;
        }
    }
    else
    {
        for (SO* pso = psw->dlRoot.psoFirst; pso != nullptr; pso = pso->dleRoot.psoNext)
            pso->fCpsoBuildContactGroup = 0;
    }

    psw->cSoRootChangeSolved = psw->cSoRootChange;

}
