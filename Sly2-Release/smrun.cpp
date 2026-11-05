// State machines at run time (Sly 2 0x1EFAC0's tables, 0x1F00C8-0x1F14F8). Specification: Sly2-Reference/Sly2-Release/game/
// sm_p3.cpp, m_1F0FA0_p3.cpp (oracle-verified) and the decomp's func_001F0850/08D0/0948/09D8/1350/13A8.

#include "smrun.h"
#include "worldglobals.h"
#include "animfind.h"
#include "clock.h"

#include "xfmath.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

std::vector<SMA*> g_apsma;
WORLD_TABLE(g_apsma);   // (PC: moved aside while a kept world loads, worldglobals.h)
constinit void (*g_pfnSmaScript)(SMA* psma, int symid, int nA, int nB) = nullptr;

namespace
{
// orig 0x001F13A8: the state change a -> b announced (slot 4: message 10 to the object and the listeners;
// slot 3: the script handler 8)
void NotifySmaState(SMA* psma, int isA, int isB)
{
    SMAMSG10 msg{psma, OidFromSmIsms(psma->psm, isA), OidFromSmIsms(psma->psm, isB)};
    if (psma->palo && g_pfnLoMessage)
        g_pfnLoMessage(psma->palo, 10, &msg);
    const std::vector<SMALISTENER*> a = psma->aplistener;
    for (SMALISTENER* pl : a) pl->OnSmaState(msg);
    if (g_pfnSmaScript)
        g_pfnSmaScript(psma, 8, msg.oidOld, msg.oidNew);  // vt slot 3 (8)
}

// orig 0x001F0458
const SM::SMG* PsmgFind(const SM* psm, uint32_t grf)
{
    for (const SM::SMG& g : psm->asmg)
        if (g.grf & grf) return &g;
    return nullptr;
}

// orig 0x001F0498
int CsmtDistance(const SM* psm, uint32_t grf, int is0, int is1)
{
    const SM::SMG* pg = PsmgFind(psm, grf);
    if (!pg) return 0;
    // the original's flat index (a state of -1, e.g. no goal, reads a neighbouring entry of the table); outside the
    // table (state 0 toward -1: ASan, Ep6 autoplay, a dialog target's RetractAsega) the PS2 read a byte of the memory
    // before it: natively unreachable
    const long long i = static_cast<long long>(is0) * static_cast<long long>(psm->asms.size()) + is1;
    if (i < 0 || i >= static_cast<long long>(pg->adist.size())) return -1;
    return pg->adist[static_cast<size_t>(i)];
}

// orig 0x001F0AD0: one of the candidates at random, weighted by gProbability
SM::SMT* PsmtChoose(const std::vector<SM::SMT*>& a)
{
    float sum = 0.0f;
    for (SM::SMT* p : a) sum = sum + p->gProbability;
    const float sumAll = sum;
    sum = GRandInRange(0.0f, sum);
    if (std::getenv("SLY2N_SMRANDPRINT"))  // (debug: each weighted choice)
        std::printf("sm choose t %.3f: %zu candidates, %.4f of %.4f\n", g_clock.t, a.size(), sum, sumAll);
    // (debug, test harness: SLY2N_SMFORCE="t=i,t=i..": the first multi-candidate choice within 0.01 s of each t takes
    // candidate i instead, e.g. a reference's random branch, to compare the rest of a run)
    if (static const char* s_szForce = std::getenv("SLY2N_SMFORCE"); s_szForce && a.size() > 1)
    {
        static std::vector<std::pair<float, int>> s_aforce;
        static bool s_fInit = false;
        if (!s_fInit)
        {
            s_fInit = true;
            for (const char* q = s_szForce; *q;)
            {
                float t = 0.0f;
                int i = 0;
                if (std::sscanf(q, "%f=%d", &t, &i) == 2) s_aforce.push_back({t, i});
                const char* e = std::strchr(q, ',');
                if (!e) break;
                q = e + 1;
            }
        }
        for (auto it = s_aforce.begin(); it != s_aforce.end(); ++it)
            if (std::fabs(it->first - g_clock.t) < 0.01f && it->second < static_cast<int>(a.size()))
            {
                SM::SMT* p = a[static_cast<size_t>(it->second)];
                s_aforce.erase(it);
                return p;
            }
    }
    for (SM::SMT* p : a)
    {
        sum = sum - p->gProbability;
        if (sum <= 0.0f) return p;
    }
    return nullptr;
}

// orig 0x001F0B80: a random first transition (under grf) of a shortest path from is0 to is1
SM::SMT* PsmtPath(SM* psm, uint32_t grf, int is0, int is1)
{
    const int n = is0 == is1 ? 1 : CsmtDistance(psm, grf, is0, is1);
    std::vector<SM::SMT*> a;
    for (SM::SMT& e : psm->asmt)
    {
        if (!(e.grf & grf)) continue;
        if (e.isFrom == is0 && CsmtDistance(psm, grf, e.isTo, is1) == n - 1)
        {
            if (a.size() < 64) a.push_back(&e);
            continue;
        }
        if ((e.grf & 1) && e.isTo == is0 && CsmtDistance(psm, grf, e.isFrom, is1) == n - 1)
            if (a.size() < 64) a.push_back(&e);
    }
    if (a.empty()) return nullptr;
    return PsmtChoose(a);
}

// orig 0x001F0CF0: a random transition (under grf) leaving is
SM::SMT* PsmtLeaving(SM* psm, uint32_t grf, int is)
{
    std::vector<SM::SMT*> a;
    for (SM::SMT& e : psm->asmt)
        if ((e.grf & grf) && (e.isFrom == is || ((e.grf & 1) && e.isTo == is)) && a.size() < 64) a.push_back(&e);
    if (a.empty()) return nullptr;
    return PsmtChoose(a);
}

// orig 0x001F0FA0: the transition ends: the pending state becomes the current one
void EndSmaTransition(SMA* psma)
{
    if (psma->pasega)
    {
        auto& a = psma->pasega->aplistener;
        a.erase(std::remove(a.begin(), a.end(), psma), a.end());
    }
    psma->pasega = nullptr;
    const int next = psma->ismsNext;
    const int cur = psma->ismsCur;
    psma->ismsNext = -1;
    psma->ismsCur = next;
    NotifySmaState(psma, cur, next);
}

// orig 0x001F08D0: the transition playing jumps to its end and stops
void FinishSmaTransition(SMA* psma)
{
    ASEGA* pasega = psma->pasega;
    if (!pasega) return;
    psma->pasega = nullptr;
    auto& a = pasega->aplistener;
    a.erase(std::remove(a.begin(), a.end(), psma), a.end());
    if (pasega->svt < 0.0f) SetAsegaTime(pasega, 0, 0.0f, pasega->svt);
    else SetAsegaTime(pasega, 2, 0.0f, pasega->svt);
    RetractAsega(pasega);
    psma->pasega = pasega;  // (0x1F0FA0 clears it again and moves to the pending state)
    EndSmaTransition(psma);
}
}  // namespace

// The tables (0x1EFAC0 after the reads): states' next states as indices, transitions by state index (those
// whose animation was not loaded are already dropped by the loader), one distance matrix per flag group.
void InitSm(SM* psm)
{
    if (psm->fInit) return;
    SM* psrc = psm;
    while (psrc->ploCloneOf && psrc->anode.empty()) psrc = static_cast<SM*>(psrc->ploCloneOf);
    psm->fInit = true;
    psm->asms.clear();
    for (const auto& [oid, next] : psrc->anode) psm->asms.push_back(SM::SMS{oid, -1});
    for (size_t i = 0; i < psrc->anode.size(); ++i)
    {
        const int32_t v = psrc->anode[i].second;
        psm->asms[i].inext = v == -1 ? -1 : (v == 0x20 ? -2 : IsmsFindSm(psm, v));
    }
    uint32_t grfAll = 0;
    psm->asmt.clear();
    for (const SM::EDGE& e : psrc->aedge)
    {
        SM::SMT t;
        t.paseg = e.plo && FCidDerivedFrom(e.plo->Cid(), 0x6D) ? static_cast<ASEG*>(e.plo) : nullptr;
        t.isFrom = IsmsFindSm(psm, e.from);
        t.isTo = IsmsFindSm(psm, e.to);
        t.grf = static_cast<uint32_t>(e.grf);
        t.gProbability = e.weight;
        t.value = static_cast<uint32_t>(e.value);
        psm->asmt.push_back(t);
        grfAll |= t.grf & 0xFFFC;
    }
    // the flag groups: for each flag bit used, the flags always found with it
    std::vector<uint32_t> amask;
    for (uint32_t bit = 4; bit < 0x1001; bit <<= 1)
    {
        if (!(bit & grfAll)) continue;
        bool fSeen = false;
        for (uint32_t m : amask)
            if (bit & m) fSeen = true;
        if (fSeen) continue;
        uint32_t grf = grfAll;
        for (const SM::SMT& t : psm->asmt)
            if (t.grf & bit) grf &= t.grf;
        for (const SM::SMT& t : psm->asmt)
            if ((t.grf & grf) && !(t.grf & bit)) grf &= ~t.grf;
        amask.push_back(grf);
    }
    const int c = static_cast<int>(psm->asms.size());
    psm->asmg.clear();
    for (uint32_t grf : amask)
    {
        SM::SMG g;
        g.grf = grf;
        g.adist.assign(static_cast<size_t>(c) * c, -1);
        for (int r = 0; r < c; ++r) g.adist[static_cast<size_t>(r) * c + r] = 0;
        int d = 1;
        bool fChanged;
        do
        {
            fChanged = false;
            for (const SM::SMT& t : psm->asmt)
            {
                if (!(t.grf & g.grf) || t.isFrom < 0 || t.isTo < 0) continue;
                for (int r = 0; r < c; ++r)
                {
                    int8_t* row = &g.adist[static_cast<size_t>(r) * c];
                    if (row[t.isFrom] == d - 1 && row[t.isTo] < 0)
                    {
                        row[t.isTo] = static_cast<int8_t>(d);
                        fChanged = true;
                        continue;
                    }
                    if (!(t.grf & 1)) continue;
                    if (row[t.isTo] != d - 1) continue;
                    if (row[t.isFrom] >= 0) continue;
                    row[t.isFrom] = static_cast<int8_t>(d);
                    fChanged = true;
                }
            }
            ++d;
        } while (fChanged);
        psm->asmg.push_back(std::move(g));
    }
    // the initial SMA (EOPID 370: 0x1F0128 {oid, state, grfapl})
    for (const OPT& opt : psrc->aopt)
    {
        if (opt.eopid != 370 || opt.aval.size() < 3) continue;
        psm->fInitial = true;
        psm->oidInitial = opt.aval[0].n;
        psm->oidStateInitial = opt.aval[1].n;
        psm->grfaplInitial = opt.aval[2].n;
    }
}

// orig: 0x001F0400
int IsmsFindSm(const SM* psm, OID oid)
{
    for (size_t i = 0; i < psm->asms.size(); ++i)
        if (psm->asms[i].oid == oid) return static_cast<int>(i);
    return -1;
}

// orig: 0x001F04F8
OID OidFromSmIsms(const SM* psm, int isms)
{
    if (isms < 0 || isms >= static_cast<int>(psm->asms.size())) return -1;
    return psm->asms[isms].oid;
}

// orig: 0x001F00C8
void PostSmLoad(SM* psm)
{
    InitSm(psm);
    if (!psm->fInitial) return;
    // orig 0x15E938(G, 4, oid, psm): the object of that oid nearest the SM (e.g. world 14's second SM 72795, with no
    // parent, on 77184 of its own instance; native searched only the SM's parents' owned objects and found none)
    LO* plo = PloFindSwNearest(psm, psm->oidInitial);
    ALO* palo = plo && FCidDerivedFrom(plo->Cid(), 0x30) ? static_cast<ALO*>(plo) : nullptr;
    PsmaApplySm(psm, palo, psm->oidStateInitial, psm->grfaplInitial);
}

// orig: 0x001F01B8
SMA* PsmaApplySm(SM* psm, ALO* palo, OID oid, int grfapl, uint32_t grfMask)
{
    InitSm(psm);
    if (const char* e = std::getenv("SLY2N_SMDUMP"); e && std::atoi(e) == psm->oid)   // debugging: the SM applied
    {
        std::printf("sm %d applied to %d state %d grfapl %d mask 0x%X\n", psm->oid, palo ? palo->oid : -1, oid, grfapl, grfMask);
        for (const SM::SMS& s : psm->asms) std::printf("  state %d next %d\n", s.oid, s.inext);
        for (const SM::SMT& t : psm->asmt)
            std::printf("  trans %d -> %d aseg %d grf 0x%X p %.2f value 0x%X\n", t.isFrom, t.isTo, t.paseg ? t.paseg->oid : -1, t.grf, t.gProbability, t.value);
    }
    SMA* ps = new SMA();
    ps->palo = palo;
    ps->grfapl = grfapl;
    ps->grfMask = grfMask;
    ps->psm = psm;
    ps->ismsNext = -1;
    ps->svtLocal = 1.0f;
    g_apsma.push_back(ps);
    psm->apsma.push_back(ps);
    ++ps->cref;
    // vt slot 3 (2): the script handler's start
    if (g_pfnSmaScript) g_pfnSmaScript(ps, 2, 0, 0);
    if (!FSmaRetracted(ps))
    {
        if (oid != -1)
        {
            const int v = IsmsFindSm(psm, oid);
            ps->ismsCur = v;
            ps->ismsGoal = v;
            NotifySmaState(ps, -1, v);
        }
        ChooseSmaTransition(ps, false);
    }
    return ps;
}

// orig: 0x001F03C8
SMA* PsmaFindSm(SM* psm, ALO* palo)
{
    for (SMA* p : psm->apsma)
        if (p->palo == palo) return p;
    return nullptr;
}

// orig: 0x001F0638
bool FSmaRetracted(const SMA* psma) { return psma->fRetracted; }

// orig: 0x001F0598
void RetractSma(SMA* psma)
{
    if (FSmaRetracted(psma)) return;
    if (ASEGA* pasega = psma->pasega)
    {
        psma->pasega = nullptr;
        auto& a = pasega->aplistener;
        a.erase(std::remove(a.begin(), a.end(), psma), a.end());
        RetractAsega(pasega);
    }
    psma->fRetracted = true;
    auto& a = psma->psm->apsma;
    a.erase(std::remove(a.begin(), a.end(), psma), a.end());
    g_apsma.erase(std::remove(g_apsma.begin(), g_apsma.end(), psma), g_apsma.end());
    if (g_pfnSmaScript) g_pfnSmaScript(psma, 3, 0, 0);  // vt slot 3 (3): its script's retract event (the safe house's
                                                       // SM removes the briefing camera object there)
}

// orig: 0x001F1078
void SMA::OnAsegaMsg(ASEGA* p, int msgk)
{
    if (msgk != 2 || p != pasega) return;
    EndSmaTransition(this);
    ChooseSmaTransition(this, false);
}

// orig: 0x001F0660
void SetSmaGoal(SMA* psma, OID oid)
{
    if (FSmaRetracted(psma)) return;
    const int isms = oid != -1 ? IsmsFindSm(psma->psm, oid) : -1;
    if (std::getenv("SLY2N_SMADEBUG"))
        std::printf("sma %d goal %d (isms %d, was %d, cur %d, playing %d) t %.3f object %d instance %d\n", psma->psm ? psma->psm->oid : -1, oid, isms,
                    psma->ismsGoal, psma->ismsCur, psma->pasega ? 1 : 0, g_clock.t, psma->palo ? psma->palo->oid : -1,
                    psma->palo ? psma->palo->oidInstance : -1);
    if (isms == psma->ismsGoal) return;
    psma->ismsGoal = isms;
    if (psma->pasega)
    {
        if (psma->psmt && (psma->psmt->grf & 2)) RetractAsega(psma->pasega);
    }
    else
        ChooseSmaTransition(psma, false);
}

// orig: 0x001F0730
void SeekSma(SMA* psma, OID oid)
{
    if (FSmaRetracted(psma) || oid == -1) return;
    const int isms = IsmsFindSm(psma->psm, oid);
    if (isms == -1) return;
    const int from = psma->pasega ? psma->ismsNext : psma->ismsCur;
    if (PsmtPath(psma->psm, psma->grfMask, from, isms)) SetSmaGoal(psma, oid);
}

// orig: 0x001F09D8
void SetSmaState(SMA* psma, OID oid)
{
    if (FSmaRetracted(psma)) return;
    const int m = IsmsFindSm(psma->psm, oid);
    if (psma->ismsCur == m && (psma->ismsGoal == m || psma->ismsGoal == -1)) return;
    if (ASEGA* pv = psma->pasega)
    {
        psma->pasega = nullptr;
        auto& a = pv->aplistener;
        a.erase(std::remove(a.begin(), a.end(), psma), a.end());
        RetractAsega(pv);
    }
    const int old = psma->ismsCur;
    psma->ismsNext = -1;
    psma->ismsGoal = m;
    psma->ismsCur = m;
    NotifySmaState(psma, old, m);
    ChooseSmaTransition(psma, false);
}

// orig: 0x001F0948
void SnapSmaGoal(SMA* psma, OID oid)
{
    if (FSmaRetracted(psma)) return;
    psma->ismsGoal = IsmsFindSm(psma->psm, oid);
    static const bool s_fDebug = std::getenv("SLY2N_SNAPDEBUG") != nullptr;
    if (s_fDebug)
        std::fprintf(stderr, "snap sm %d obj %d: cur %d goal %d (oid %d) playing %d\n", psma->psm ? psma->psm->oid : -1,
                     psma->palo ? psma->palo->oid : -1, psma->ismsCur, psma->ismsGoal, oid, psma->pasega != nullptr);
    if (s_fDebug && std::getenv("SLY2N_SNAPDEBUG")[0] == '2')
        for (const SM::SMT& t : psma->psm->asmt)
        {
            std::fprintf(stderr, "  smt %d -> %d grf %x aseg %d\n", t.isFrom, t.isTo, t.grf, t.paseg ? t.paseg->oid : -1);
            if (t.paseg)
                for (int l = 0; l < 3; ++l)
                    for (const ASEGEV& e : PasegdOf(t.paseg)->aaev[l])
                        std::fprintf(stderr, "    list %d t %.3f kind %d an %d %d %d %d\n", l, e.t, e.kind, e.an[0], e.an[1], e.an[2], e.an[3]);
        }
    FinishSmaTransition(psma);
    ChooseSmaTransition(psma, true);
    if (s_fDebug)
        std::fprintf(stderr, "  -> cur %d next %d playing %d psmt %p aseg %d\n", psma->ismsCur, psma->ismsNext,
                     psma->pasega != nullptr, (void*)psma->psmt, psma->psmt && psma->psmt->paseg ? psma->psmt->paseg->oid : -1);
}

// orig: 0x001F0850
void SetSmaSpeed(SMA* psma, float svt)
{
    if (FSmaRetracted(psma)) return;
    psma->svtLocal = svt;
    if (psma->pasega) SetAsegaSpeed(psma->pasega, svt);
}

// orig: 0x001F1350
void SetSmaMask(SMA* psma, uint32_t grf)
{
    psma->grfMask = grf;
    if (psma->pasega && psma->psmt && (psma->psmt->value & grf))  // (the transition's +0x10, not its +0xC flags)
    {
        FinishSmaTransition(psma);
        ChooseSmaTransition(psma, false);
    }
}

// orig: 0x001F07C8 / 0x001F07E8 / 0x001F0808
OID OidSmaGoal(const SMA* psma) { return OidFromSmIsms(psma->psm, psma->ismsGoal); }
OID OidSmaCur(const SMA* psma) { return OidFromSmIsms(psma->psm, psma->ismsCur); }
OID OidSmaNextOrCur(const SMA* psma)
{
    if (psma->ismsNext >= 0) return psma->psm->asms[psma->ismsNext].oid;
    if (psma->ismsCur >= 0) return psma->psm->asms[psma->ismsCur].oid;
    return -1;
}

// orig: 0x001F0D88 ChooseSmaTransition: along transitions toward the goal (or the states' next states) until
// one animates; fReverse starts the first one at its end
void ChooseSmaTransition(SMA* psma, bool fReverse)
{
    if (FSmaRetracted(psma)) return;
    bool s3 = fReverse;
    for (;;)
    {
        const int cur = psma->ismsCur;
        SM* psm = psma->psm;
        SM::SMT* psmt;
        if (cur != psma->ismsGoal)
            psmt = PsmtPath(psm, psma->grfMask, cur, psma->ismsGoal);
        else
        {
            if (cur < 0 || cur >= static_cast<int>(psm->asms.size())) return;
            const int next = psm->asms[cur].inext;
            if (next == -1) return;
            if (next == -2)
            {
                psmt = PsmtLeaving(psm, psma->grfMask, cur);
                if (psmt)
                {
                    int goal = psmt->isFrom;
                    if (goal == psma->ismsCur) goal = psmt->isTo;
                    psma->ismsGoal = goal;
                }
            }
            else
            {
                psma->ismsGoal = next;
                psmt = PsmtPath(psm, psma->grfMask, cur, next);
            }
            s3 = false;
        }
        psma->psmt = psmt;
        if (static const bool s_fDbg = std::getenv("SLY2N_SMADEBUG") != nullptr; s_fDbg)  // (debug: the transition chosen)
            std::printf("sma %d choose cur %d goal %d -> %d..%d aseg %d t %.3f object %d instance %d\n", psm->oid, psma->ismsCur,
                        psma->ismsGoal, psmt ? psmt->isFrom : -1, psmt ? psmt->isTo : -1, psmt && psmt->paseg ? psmt->paseg->oid : -1,
                        g_clock.t, psma->palo ? psma->palo->oid : -1, psma->palo ? psma->palo->oidInstance : -1);
        if (!psmt) return;
        bool s1 = false;
        if (ASEG* paseg = psmt->paseg)
        {
            float t, svt;
            const float tMax = PasegdOf(paseg)->tMax;
            if (psmt->isFrom != psma->ismsCur)
            {
                psma->ismsNext = psmt->isFrom;
                svt = -psma->svtLocal;
                t = tMax;
            }
            else
            {
                t = 0.0f;
                psma->ismsNext = psmt->isTo;
                svt = psma->svtLocal;
            }
            int grf = psma->grfapl;
            if (s3)
            {
                grf = (grf | 2) & ~1;
                s1 = true;
                t = tMax - t;
            }
            else if (tMax == 0.0f)
                s1 = true;
            ApplyAseg(paseg, psma->palo, t, svt, grf, &psma->pasega);
        }
        ASEGA* pasega = psma->pasega;
        if (!pasega)
        {
            const int old = psma->ismsCur;
            int v = psmt->isFrom;
            if (v == old) v = psmt->isTo;
            psma->ismsCur = v;
            NotifySmaState(psma, old, v);
        }
        else
        {
            if (!s1)
            {
                pasega->aplistener.push_back(psma);  // orig 0x1333C0
                return;
            }
            const int next = psma->ismsNext;
            const int old = psma->ismsCur;
            psma->ismsCur = next;
            psma->ismsNext = -1;
            psma->pasega = nullptr;
            RetractAsega(pasega);
            NotifySmaState(psma, old, next);
        }
        if (FSmaRetracted(psma)) return;
        if (psma->pasega) return;
    }
}

// orig: 0x001F1448
void ReplaceSmaHandle(SM* psm, ALO* palo, OID oid, int grfapl, SMA** ppsma)
{
    ClearSmaHandle(ppsma);
    if (psm)
    {
        *ppsma = PsmaApplySm(psm, palo, oid, grfapl);
        ++(*ppsma)->cref;
    }
}

// orig: 0x001F14F8
void ReplaceSmaHandleMask(SM* psm, ALO* palo, OID oid, int grfapl, uint32_t grfMask, SMA** ppsma)
{
    ClearSmaHandle(ppsma);
    if (psm)
    {
        *ppsma = PsmaApplySm(psm, palo, oid, grfapl, grfMask);
        ++(*ppsma)->cref;
    }
}

// orig: 0x001F15B8
void ClearSmaHandle(SMA** ppsma)
{
    if (!*ppsma) return;
    RetractSma(*ppsma);
    --(*ppsma)->cref;
    *ppsma = nullptr;
}
