// The contact constraint solver (dzg.h): orig 0x1195E0 / 0x1198F8 (Crout), 0x11A550..0x11AE88 (the LCP pivoting),
// 0x1585D8..0x158FBC (rows from contacts, the SOs' responses, friction rows, applying the solution). Ported from the
// reference's m_1195E0_p3.cpp, m_11A550_p3.cpp, m_1585D8_p3.cpp (oracle-verified), arithmetic order kept.
//
// Native difference: the original allocates each row's couplings with the number of rows at the time and later
// rows write past them into the next scratch allocations; here each row has room for every row (cdzMax).

#include "clock.h"
#include "dzg.h"

#include "so.h"
#include "vtnative.h"

#include <algorithm>
#include <cfloat>
#include <cmath>


namespace
{
constexpr float EPS = 1e-4f;
inline float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
template <class T>
void RemoveFrom(std::vector<T>& a, const T& t)
{
    auto it = std::find(a.begin(), a.end(), t);
    if (it != a.end()) a.erase(it);
}
}

// orig: 0x001195E0 CroutDecomp__FiPfT1 (alu = the LU decomposition of aa; pivots below 1e-8 zero their column)
void CroutDecomp(int n, const float* aa, float* alu)
{
    if (n == 0) return;
    constexpr float CROUT_EPS = 1e-8f;  // 0x322BCC77
    const int nm1 = n - 1;
    for (int k = 0; k < n; k++) alu[k] = aa[k];
    if (std::fabs(alu[0]) < CROUT_EPS)
        for (int i = 0; i < n; i++) alu[i * n] = 0.0f;
    else
        for (int i = 1; i < n; i++) alu[i * n] = aa[i * n] / alu[0];
    for (int i = 1; i < nm1; i++)
    {
        for (int j = i; j < n; j++)
        {
            float sum = 0.0f;
            for (int k = 0; k < i; k++) sum = sum + alu[k * n + j] * alu[i * n + k];
            alu[i * n + j] = aa[i * n + j] - sum;
        }
        if (std::fabs(alu[i * n + i]) < CROUT_EPS)
            for (int j = i; j < n; j++) alu[j * n + i] = 0.0f;
        else
            for (int j = i + 1; j < n; j++)
            {
                float sum = 0.0f;
                for (int k = 0; k < i; k++) sum = sum + alu[k * n + i] * alu[j * n + k];
                alu[j * n + i] = (aa[j * n + i] - sum) / alu[i * n + i];
            }
    }
    float sum = 0.0f;
    for (int k = 0; k < nm1; k++) sum = sum + alu[k * n + nm1] * alu[nm1 * n + k];
    const float g = aa[nm1 * n + n - 1] - sum;
    alu[nm1 * n + n - 1] = std::fabs(g) < CROUT_EPS ? 0.0f : g;
}

// orig: 0x001198F8 CroutSolve__FiPfN21 (forward then back substitution; a zero pivot gives 0 / skips the row)
void CroutSolve(int n, const float* alu, const float* ab, float* ax)
{
    for (int i = 0; i < n; i++)
    {
        if (alu[i * n + i] == 0.0f)
        {
            ax[i] = 0.0f;
            continue;
        }
        float sum = 0.0f;
        for (int j = 0; j < i; j++) sum = sum + alu[j * n + i] * ax[j];
        ax[i] = (ab[i] - sum) / alu[i * n + i];
    }
    for (int i = n - 2; i >= 0; i--)
    {
        if (alu[i * n + i] == 0.0f) continue;
        float sum = 0.0f;
        for (int j = i + 1; j < n; j++) sum = sum + alu[j * n + i] * ax[j];
        ax[i] = ax[i] - sum;
    }
}

// orig: 0x0011A550 func_0011A550 (the rows, the four lists, the solver's arrays)
void InitDzgRows(DZG* p, int cdzMax)
{
    p->cdzMax = cdzMax;
    p->cA = 0;
    p->cdz = 0;
    p->adz.assign(cdzMax, DZ{});
    p->adlA.clear();
    p->adlB.clear();
    p->adlC.clear();
    p->adlD.clear();
    p->aa.assign(size_t(cdzMax) * cdzMax, 0.0f);
    p->alu.assign(size_t(cdzMax) * cdzMax, 0.0f);
    p->ab.assign(cdzMax, 0.0f);
    p->ax.assign(cdzMax, 0.0f);
}

// orig: 0x0011A6B8 ClearDzgSolution__FP3DZG (every row open, g and the residual 0)
void ClearDzgSolution(DZG* p)
{
    p->cA = 0;
    p->adlA.clear();
    p->adlB.clear();
    p->adlC.clear();
    p->adlD.clear();
    for (int i = 0; i < p->cdz; i++)
    {
        DZ* pdz = &p->adz[i];
        pdz->gResidual = 0.0f;
        pdz->g = 0.0f;
        pdz->ilist = 3;
        p->adlD.push_back(pdz);
    }
}

namespace
{
std::vector<DZ*>& List(DZG* p, int i)
{
    switch (i)
    {
    case 0: return p->adlA;
    case 1: return p->adlB;
    case 2: return p->adlC;
    default: return p->adlD;
    }
}
void MoveDz(DZG* p, DZ* pdz, int iTo)
{
    RemoveFrom(List(p, pdz->ilist), pdz);
    List(p, iTo).push_back(pdz);
    pdz->ilist = iTo;
}
inline float W(const DZ* pdz, int i) { return (i >= 0 && i < int(pdz->agW.size())) ? pdz->agW[i] : 0.0f; }
}

// orig: 0x0011A770 EnforceDzgDz__FP3DZGP2DZ (pdz into the system, then at most 10 pivots: the clamped rows' system
// solved for the change cancelling their residuals, the largest step (at most 1) keeping every row in its limits,
// the limiting row moved between the lists)
void EnforceDzgDz(DZG* p, DZ* pdz)
{
    if (pdz)
    {
        MoveDz(p, pdz, 0);
        p->cA++;
    }
    for (int iter = 0; iter < 10; iter++)
    {
        const int n = p->cA;
        size_t im = 0;
        for (DZ* pa : p->adlA)
            for (DZ* pb : p->adlA) p->aa[im++] = W(pa, p->Idz(pb));
        CroutDecomp(n, p->aa.data(), p->alu.data());
        {
            int ib = 0;
            for (DZ* pa : p->adlA) p->ab[ib++] = -pa->gResidual;
        }
        CroutSolve(n, p->alu.data(), p->ab.data(), p->ax.data());
        for (int i = 0; i < p->cdz; i++)
        {
            float sum = 0.0f;
            int ix = 0;
            for (DZ* pa : p->adlA) sum = sum + p->ax[ix++] * W(pa, i);
            p->adz[i].dResidual = sum;
        }
        DZ* pdzLim = nullptr;
        float s = 1.0f;
        int kind = 0;
        {
            int ix = 0;
            for (DZ* pa : p->adlA)
            {
                const float x = p->ax[ix++];
                if (EPS < x)
                {
                    const float lim = pa->gMax;
                    if (lim == FLT_MAX) continue;
                    const float t = (lim - pa->g) / x;
                    if (t < s)
                    {
                        s = t;
                        pdzLim = pa;
                        kind = 4;
                    }
                }
                else if (x < -EPS)
                {
                    const float lim = pa->gMin;
                    if (lim == -FLT_MAX) continue;
                    const float t = (lim - pa->g) / x;
                    if (t < s)
                    {
                        s = t;
                        pdzLim = pa;
                        kind = 2;
                    }
                }
            }
        }
        for (DZ* pb : p->adlB)
        {
            const float d = pb->dResidual;
            if (-EPS < d) continue;
            const float t = -pb->gResidual / d;
            if (t < s)
            {
                s = t;
                pdzLim = pb;
                kind = 1;
            }
        }
        for (DZ* pc : p->adlC)
        {
            const float d = pc->dResidual;
            if (d < EPS) continue;
            const float t = -pc->gResidual / d;
            if (t < s)
            {
                s = t;
                pdzLim = pc;
                kind = 3;
            }
        }
        for (int i = 0; i < p->cdz; i++) p->adz[i].gResidual = p->adz[i].gResidual + s * p->adz[i].dResidual;
        {
            int ix = 0;
            for (DZ* pa : p->adlA) pa->g = pa->g + s * p->ax[ix++];
        }
        switch (kind)
        {
        case 0: return;
        case 1:
            p->cA++;
            MoveDz(p, pdzLim, 0);
            break;
        case 2:
            p->cA--;
            MoveDz(p, pdzLim, 1);
            if (pdzLim == pdz) return;
            break;
        case 3:
            p->cA++;
            MoveDz(p, pdzLim, 0);
            break;
        case 4:
            p->cA--;
            MoveDz(p, pdzLim, 2);
            if (pdzLim == pdz) return;
            break;
        default: break;
        }
    }
}

// orig: 0x001585D8 func_001585D8 (rows for twice the contacts, the contacts and the SOs, the entries)
void InitDzg(DZG* p, int cxp, XP** apxp, int cso, ALO** apso)
{
    InitDzgRows(p, cxp * 2);
    p->cxp = cxp;
    p->apxp = apxp;
    p->cso = cso;
    p->apso = apso;
    p->aent.assign(p->cdzMax, DZENT{});
}

namespace
{
inline DZENT* PentFromDz(DZG* p, DZ* pdz) { return &p->aent[p->Idz(pdz)]; }  // 0x158958
inline glm::vec3 Resp(const DZENT& e, int i) { return (i >= 0 && i < int(e.aresp.size())) ? e.aresp[i] : glm::vec3(0.0f); }
}

// orig: 0x00158668 AppendDzgDz__FP3DZGP2DZ (a new row: each computed row's coupling with it, then into the open
// list)
void AppendDzgDz(DZG* p, DZ* pdz, DZENT* pent)
{
    const int ixp = pent->pxp ? pent->pxp->ipxp : 0;
    for (int i = 0; i < p->cdz; i++)
    {
        DZ& d = p->adz[i];
        if (!d.fW) continue;
        if (int(d.agW.size()) <= p->cdz) d.agW.resize(p->cdzMax + 1, 0.0f);
        d.agW[p->cdz] = Dot3(pent->normal, Resp(p->aent[i], ixp));
    }
    pdz->ilist = 3;
    p->adlD.push_back(pdz);
    p->cdz++;
}

// orig: 0x00158728 func_00158728 (a row's responses: its contact's two SOs fill the entry (slot 68), each contact's
// relative velocity change (slot 69 of its two SOs, the difference), the couplings with every row)
void CalculateDzResponses(DZG* p, int grf, DZ* pdz, DZENT* pent)
{
    XP* ppair = pent->pxp;
    for (int i = 0; i < 2; i++)
    {
        ALO* pso = ppair->axpd[i].paloRoot;
        if (!pso) continue;
        using PFN = void (*)(ALO*, DZ*, DZENT*, int, int);
        if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(pso, 68))) pfn(pso, pdz, pent, grf, i);
        else if (PFN pfnAlo = reinterpret_cast<PFN>(PvNativeFn(0x1FAD00))) pfnAlo(pso, pdz, pent, grf, i);
    }
    pent->aresp.assign(p->cxp, glm::vec3(0.0f));
    for (int i = 0; i < p->cxp; i++)
    {
        XP* pxp = p->apxp[i];
        glm::vec3 av[2];
        for (int k = 1, iend = 0; k >= 0; k--, iend++)
        {
            ALO* x = pxp->axpd[iend].paloRoot;
            if (x && (x == ppair->axpd[0].paloRoot || x == ppair->axpd[1].paloRoot))
            {
                const int ihalf = (x == ppair->axpd[0].paloRoot) ? 0 : 1;
                using PFN = glm::vec3 (*)(ALO*, const glm::vec3&, const glm::vec3&, const glm::vec3&);
                PFN pfn = reinterpret_cast<PFN>(PvNativeVt(x, 69));
                if (!pfn) pfn = reinterpret_cast<PFN>(PvNativeFn(0x1FADD0));
                av[iend] = pfn ? pfn(x, pent->advLin[ihalf], pent->advAng[ihalf], pxp->axpd[iend].pos) : glm::vec3(0.0f);
            }
            else
                av[iend] = glm::vec3(0.0f);
        }
        pent->aresp[i] = av[0] - av[1];
    }
    pdz->agW.assign(p->cdzMax + 1, 0.0f);
    pdz->fW = true;
    const int ixp = pent->pxp ? pent->pxp->ipxp : 0;
    for (int i = 0; i < p->cdz; i++)
    {
        const DZENT& pe = p->aent[i];
        (void)pe;
        pdz->agW[i] = Dot3(p->aent[i].normal, Resp(*pent, p->aent[i].pxp ? p->aent[i].pxp->ipxp : ixp));
    }
}

// orig: 0x00158988 SolveDzg__FP3DZGiiPP2XPPiPPP2SO (the open rows of kind 1 first, then the most violated one at a
// time until none is below -0.01)
void SolveDzg(DZG* p, int grf)
{
    {
        std::vector<DZ*> aopen = p->adlD;
        for (DZ* pdz : aopen)
        {
            if (pdz->kind != 1) continue;
            if (!pdz->fW) CalculateDzResponses(p, grf, pdz, PentFromDz(p, pdz));
            MoveDz(p, pdz, 0);
            p->cA++;
        }
    }
    EnforceDzgDz(p, nullptr);
    for (;;)
    {
        DZ* pdzBest = nullptr;
        float gMin = -0.00999999978f;  // 0xBC23D70A
        for (DZ* pd : p->adlD)
        {
            if (pd->gResidual < gMin)
            {
                gMin = pd->gResidual;
                pdzBest = pd;
            }
        }
        if (!pdzBest) return;
        if (!pdzBest->fW) CalculateDzResponses(p, grf, pdzBest, PentFromDz(p, pdzBest));
        EnforceDzgDz(p, pdzBest);
    }
}

// orig: 0x00158B20 func_00158B20 (solves, then adds a friction row against the contact whose sliding (its relative
// velocity plus the rows' responses, out of its normal) is largest, until none slides more than 0.1)
void SolveDzgContacts(DZG* p, int grf)
{
    for (;;)
    {
        SolveDzg(p, grf);
        int iBest = -1;
        float gBest = 0.00999999978f;  // 0x3C23D70A
        XP* psoBest = nullptr;
        glm::vec3 vBest(0.0f);
        for (int i = 0; i < p->cxp; i++)
        {
            DZ* pdz = &p->adz[i];
            if (pdz->pdzPair) continue;
            if (pdz->g < EPS) continue;
            XP* pxp = p->apxp[i];
            if (pxp->mu < EPS) continue;
            glm::vec3 v = pxp->dv;
            for (int n = 0; n < p->cdz; n++)
            {
                const float g = p->adz[n].g;
                if (g == 0.0f) continue;
                v = v + Resp(p->aent[n], i) * g;
            }
            const float d = Dot3(v, pxp->normal);
            v = v - pxp->normal * d;
            const float l2 = Dot3(v, v);
            if (gBest < l2)
            {
                vBest = v;
                gBest = l2;
                iBest = i;
                psoBest = pxp;
            }
        }
        if (!psoBest) return;
        if (p->cdz >= p->cdzMax) return;  // (native guard: the original has room for two rows per contact)
        DZ* pdzSo = &p->adz[iBest];
        DZ* pdz = &p->adz[p->cdz];
        DZENT* pent = &p->aent[p->cdz];
        *pdz = DZ{};
        *pent = DZENT{};
        pdz->pdzPair = pdzSo;
        pdzSo->pdzPair = pdz;
        const float g = -std::sqrt(gBest);
        pdz->gResidual = g;
        pent->pxp = psoBest;
        pent->normal = vBest * (1.0f / g);
        pdz->kind = 1;
        float lim;
        if (psoBest->xpk != 0xE)
            lim = std::fabs(pdzSo->g) * psoBest->mu;
        else
        {
            SOST* ps = psoBest->axpd[0].paloRoot ? Psost(psoBest->axpd[0].paloRoot) : nullptr;
            const float dvz = ps ? ps->dvGravity.z : 0.0f, m = ps ? ps->m : 0.0f;
            if (grf & 1)
            {
                const float f2 = -dvz * m * g_clock.dt;
                const float f0 = std::fabs(pdzSo->g);
                lim = psoBest->mu * (f0 > f2 ? f0 : f2);
            }
            else
            {
                const float f2 = m * 1500.0f;  // 0x2BDDE8
                const float f0 = psoBest->mu * std::fabs(pdzSo->g);
                lim = f2 < f0 ? f2 : f0;
            }
        }
        pdz->gMax = lim;
        pdz->gMin = -pdz->gMax;
        AppendDzgDz(p, pdz, pent);
    }
}

// orig: 0x00158E20 func_00158E20 (the solved rows into their SOs' pending displacements (+0xF0 / +0x100), limited
// to gMax / gMaxAngular)
void ApplyDzg(DZG* p, float gMax, float gMaxAngular)
{
    for (int i = 0; i < p->cdz; i++)
    {
        DZ& d = p->adz[i];
        if (d.g == 0.0f) continue;
        DZENT& e = p->aent[i];
        for (int k = 0; k < 2; k++)
        {
            ALO* pso = e.pxp ? e.pxp->axpd[k].paloRoot : nullptr;
            if (!pso) continue;
            SORT& s = Sort(pso);
            s.dpos = s.dpos + e.advLin[k] * d.g;
            s.drot = s.drot + e.advAng[k] * d.g;
        }
    }
    auto limit = [](glm::vec3 v, float gMaxL) {
        const float l = std::sqrt(std::fabs(Dot3(v, v)));
        if (gMaxL < l) v = v * (gMaxL / l);
        return v;
    };
    for (int i = 0; i < p->cso; i++)
    {
        SORT& s = Sort(p->apso[i]);
        s.dpos = limit(s.dpos, gMax);
        s.drot = limit(s.drot, gMaxAngular);
    }
}

// orig: 0x0011ABF8 func_0011ABF8 (SolveDzg without responses: the open rows of kind 1 into the system, then the most
// violated one at a time until none is below -0.01)
void EnforceDzgConstraints(DZG* p)
{
    {
        std::vector<DZ*> aopen = p->adlD;
        for (DZ* pdz : aopen)
        {
            if (pdz->kind != 1) continue;
            MoveDz(p, pdz, 0);
            p->cA++;
        }
    }
    EnforceDzgDz(p, nullptr);
    for (;;)
    {
        DZ* pdzBest = nullptr;
        float gMin = -0.00999999978f;
        for (DZ* pd : p->adlD)
            if (pd->gResidual < gMin)
            {
                gMin = pd->gResidual;
                pdzBest = pd;
            }
        if (!pdzBest) return;
        EnforceDzgDz(p, pdzBest);
    }
}

// orig: 0x0011AD30 func_0011AD30 (a small LCP: count rows with the couplings (count x count, row-major) and the
// residuals, bounded above by gMax (below by 0); the solution into ag)
void SolveDzgConstraints(int count, const float* aaCoupling, const float* agResidual, float* ag, float gMax)
{
    DZG dzg;
    InitDzgRows(&dzg, count);
    for (int i = 0; i < count; i++)
    {
        DZ& d = dzg.adz[i];
        d.gMax = gMax;
        d.agW.assign(aaCoupling + size_t(i) * count, aaCoupling + size_t(i + 1) * count);
        d.fW = true;
        d.gResidual = agResidual[i];
        d.ilist = 3;
        dzg.adlD.push_back(&d);
    }
    dzg.cdz = count;
    EnforceDzgConstraints(&dzg);
    for (int i = 0; i < count; i++) ag[i] = dzg.adz[i].g;
}
