// The AI jobs: the stack (0x1EEC60..0x1EF850) and the job classes (0x1709A8..0x17F090). See ai.h.
// Reference: Sly2-Reference/Sly2-Release/game/m_1EEC60.cpp, m_1E9F50_p3.cpp (the base job's methods), m_174898.cpp,
// m_174B28_p3.cpp, m_17D360_p3.cpp, m_171228_p3.cpp, m_1709A8.cpp and the Sly 2 decomp.

#include "ai.h"

#include "aijobs.h"
#include "guard.h"
#include "actor.h"
#include "actorai.h"
#include "clock.h"
#include "eval.h"
#include "frame.h"
#include "ref.h"

#include <cstdio>
#include <cstdlib>
#include <set>

AISTACK::AISTACK() = default;
AISTACK::~AISTACK() = default;

// ---- the base job (0x1EEF68..0x1EF0D8) ----

void AIJOB::Init() {}                                   // orig 0x1EEF68
void AIJOB::Enter() {}                                  // orig 0x1EEF70
void AIJOB::Exit() { EndChildJob(&pchild); }            // orig 0x1EEF78
void AIJOB::Update() {}                                 // orig 0x1EEF98
void AIJOB::Steer() {}                                  // orig 0x1EEFB8
int AIJOB::State() { return (pguard ? St(pguard).fNoContacts : pactor ? pactor->fNoContacts : 0) ? 0 : 7; }  // 0x1EEFA0
int AIJOB::Message(int, void*) { return 0; }            // orig 0x1EF0B8
void AIJOB::SetState(int) {}
// orig: 0x001331F8 / 0x00132ED8 / 0x00133000
void AIJOB::Script(int k, int n)
{
    if (!pframe) return;
    CRef* pref = pframe->PrefFindBinding(static_cast<SYMID>(k), 1);
    if (!pref) return;
    if (std::getenv("SLY2N_JOBPRINT"))
    {
        const auto* pscript = dynamic_cast<JOBSCRIPT*>(this);
        std::fprintf(stderr, "job script: guard %d cid %X list %d event %d arg %d t %.9g\n",
                     pguard && pguard->palo ? pguard->palo->oid : pactor && pactor->palo ? pactor->palo->oid : -1, cid,
                     pscript && pscript->pcmdlist ? pscript->pcmdlist->oid : -1, k,
                     k == 4 ? n : 0, g_clock.t);
    }
    std::vector<CRef> args(k == 4 ? 2 : 1);
    args[0].SetBasic(this);
    if (k == 4) args[1].SetS32(n);  // event4 argument type6
    RefApply(*pref, static_cast<int>(args.size()), args.data(), pframe.get());
}
// orig: 0x001EF0C0
void AIJOB::Drive(PADCTL* pc)
{
    pc->uJoy = 0.0f;
    pc->radJoy = pactor ? pactor->radHeading : pguard ? St(pguard).radHeading : 0.0f;
}

// orig: 0x001EEFC0
void LockJob(AIJOB* p)
{
    if (p->fLocked) return;
    p->fLocked = 1;
    // TODO(ai): 0x20B438 (a guard holding: its +0x... count)
    for (AIJOB* q = p->pparent; q; q = q->pparent) ++q->cLock;
}

// orig: 0x001EF038
void UnlockJob(AIJOB* p)
{
    if (!p->fLocked) return;
    p->fLocked = 0;
    // TODO(ai): 0x20B478 (the guard told)
    for (AIJOB* q = p->pparent; q; q = q->pparent) --q->cLock;
}

// (the ended jobs are kept until the world goes (ResetEndedJobs): in the original a job's memory is its pool's slot and
// stays readable after the job ends, and a callback a job's script scheduled (schedule-callback with its self, e.g.
// Ep6's 49591 s4: (seto! ((self) 1171) 399 ...)) may still read its options; natively its delete left a dangling
// BASIC and crashed (Ep6 Carmelita job, 27:2896))
static std::vector<std::unique_ptr<AIJOB>> s_ajobEnded;
void ResetEndedJobs() { s_ajobEnded.clear(); }
static void BuryJob(std::vector<std::unique_ptr<AIJOB>>& a)
{
    s_ajobEnded.push_back(std::move(a.back()));
    a.pop_back();
}

// orig: 0x001EF0D8 (the handle's vt slot 9, then 0x1EEED0 on the owner's stack: the count lowered)
void EndChildJob(AIJOB** ppchild)
{
    AIJOB* p = *ppchild;
    if (!p) return;
    GUARD* pg = p->pguard;
    ACTOR* pa = p->pactor;
    p->Exit();
    *ppchild = nullptr;
    // the slot above the count is the one ended (the stack's top): it is released
    if (pg && !pg->stack.ajob.empty() && pg->stack.ajob.back().get() == p)
    {
        if (pg->move.pjob == p) pg->move.pjob = nullptr;
        if (p->pparent) p->pparent->pchild = nullptr;
        BuryJob(pg->stack.ajob);
    }
    else if (pa && !pa->stack.ajob.empty() && pa->stack.ajob.back().get() == p)
    {
        if (p->pparent) p->pparent->pchild = nullptr;
        BuryJob(pa->stack.ajob);
        pa->n270 = static_cast<int>(pa->stack.ajob.size());
    }
    else if (pg)
    {
        // (the original lowers the count whatever the slot: a job ended out of order leaves the top's slot)
        static bool s_fWarned = false;
        if (!s_fWarned) std::fprintf(stderr, "ai: a job ended out of the stack's order (cid 0x%X)\n", p->cid);
        s_fWarned = true;
    }
}

// orig: 0x001EEE00
AIJOB* PjobPush(GUARD* pg, CID cid)
{
    // (debug: SLY2N_JOBPUSH=oid: that guard's jobs pushed, with the stack depth)
    if (static const char* s_sz = std::getenv("SLY2N_JOBPUSH"); s_sz && pg->palo && pg->palo->oid == std::atoi(s_sz))
        std::printf("jobpush: t %.3f guard %d job 0x%X depth %zu\n", g_clock.t, pg->palo->oid, cid, pg->stack.ajob.size());
    std::unique_ptr<AIJOB> pnew = PjobNew(cid);
    AIJOB* p = pnew.get();
    AIJOB* pprev = pg->stack.ajob.empty() ? nullptr : pg->stack.ajob.back().get();
    p->pparent = pprev;
    p->pguard = pg;
    if (pprev) pprev->pchild = p;
    pg->stack.ajob.push_back(std::move(pnew));
    p->Init();
    if (static const char* s_sz = std::getenv("SLY2N_PATHSTEPPRINT"); s_sz && pg->palo && pg->palo->oid == std::atoi(s_sz))
    {
        std::fprintf(stderr, "push %p t %.3f:", (void*)pg, g_clock.t);
        for (const auto& pj : pg->stack.ajob) std::fprintf(stderr, " %X", pj->cid);
        std::fprintf(stderr, "\n");
    }
    return p;
}

// orig: 0x001EEEE0
AIJOB* PjobFind(GUARD* pg, CID cid)
{
    for (const auto& p : pg->stack.ajob)
        if (FCidDerivedFrom(p->cid, cid)) return p.get();
    return nullptr;
}

// orig: 0x001EEE00 / 0x00205438, for a player actor's +0x26C stack
AIJOB* PjobPush(ACTOR* pa, CID cid)
{
    auto pnew = PjobNew(cid);
    AIJOB* p = pnew.get();
    p->pactor = pa;
    p->pparent = pa->stack.ajob.empty() ? nullptr : pa->stack.ajob.back().get();
    if (p->pparent) p->pparent->pchild = p;
    pa->stack.ajob.push_back(std::move(pnew));
    pa->n270 = static_cast<int>(pa->stack.ajob.size());
    p->Init();
    return p;
}

// orig: 0x001EEEE0 / 0x00205458
AIJOB* PjobFind(ACTOR* pa, CID cid)
{
    for (const auto& p : pa->stack.ajob)
        if (FCidDerivedFrom(p->cid, cid)) return p.get();
    return nullptr;
}

// orig: 0x001EED10
void UpdateJobs(ACTOR* pa)
{
    for (int i = static_cast<int>(pa->stack.ajob.size()); --i >= 0;)
        if (i < static_cast<int>(pa->stack.ajob.size())) pa->stack.ajob[i]->Update();
}

// orig: 0x001EECD0
void ClearJobs(ACTOR* pa)
{
    if (!pa->stack.ajob.empty())
    {
        AIJOB* p = pa->stack.ajob.front().get();
        EndChildJob(&p);
    }
    for (auto& pj : pa->stack.ajob) s_ajobEnded.push_back(std::move(pj));
    pa->stack.ajob.clear();
    pa->n270 = 0;
}

// orig: 0x001EED10
void UpdateJobs(GUARD* pg)
{
    for (int i = static_cast<int>(pg->stack.ajob.size()); --i >= 0;)
    {
        if (i >= static_cast<int>(pg->stack.ajob.size())) continue;
        pg->stack.ajob[i]->Update();
    }
}

// orig: 0x001EED78
void OfferJobMessage(GUARD* pg, int msg, void* pm)
{
    for (int i = static_cast<int>(pg->stack.ajob.size()); --i >= 0;)
    {
        if (i >= static_cast<int>(pg->stack.ajob.size())) continue;
        if (pg->stack.ajob[i]->Message(msg, pm)) break;
    }
}

// orig: 0x001EECD0
void ClearJobs(GUARD* pg)
{
    if (!pg->stack.ajob.empty())
    {
        AIJOB* pbottom = pg->stack.ajob.front().get();
        EndChildJob(&pbottom);
    }
    for (auto& pj : pg->stack.ajob) s_ajobEnded.push_back(std::move(pj));
    pg->stack.ajob.clear();
    pg->move.pjob = nullptr;
}

// orig: 0x001EF138 (kinds 0x9C..0xC6)
std::unique_ptr<AIJOB> PjobNew(CID cid)
{
    std::unique_ptr<AIJOB> p = PjobNewActor(cid);
    if (!p) p = PjobNewKind(cid);  // aijobs.cpp
    if (!p)
    {
        static std::set<int> s_acid;
        if (s_acid.insert(cid).second) std::fprintf(stderr, "ai: job class 0x%X not ported (a do-nothing job)\n", cid);
        p = std::make_unique<AIJOB>();
    }
    p->cid = cid;
    return p;
}
