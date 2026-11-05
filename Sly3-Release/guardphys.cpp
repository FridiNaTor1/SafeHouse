// The guards' (class 0x38..) overrides of the physics' vtable slots, which read their AI state:
// 85 0x20B090 (the floor probes / contacts), 61 0x206380 (the overlap kind), 75 0x207468 (a contact
// accepted: the jobs get message 12) with 86 0x207530 / 76 0x207598. Slot 59 0x209C88's moves (+0xF80 0..3: the
// climbs / jumps, 0x1E6F10) are not ported (its base 0x1F4FA8 does nothing; the walking guards' +0xF80 is -1).

#include "guard.h"
#include "aihit.h"

#include "ai.h"
#include "aijobs.h"
#include "so.h"
#include "vtnative.h"
#include "clock.h"
#include "vecmat.h"
#include "cls.h"

#include <cstdio>
#include <cstdlib>

namespace
{
// orig: 0x0020B090 (vt slot 85: 1 in state 0x14; 0 in 0x12 / 0x13 / 0x16; else the move job's slot 12, else STEP's
// 0x2040F8)
uint32_t GrfGuardXp(ALO* palo)
{
    GUARD* pg = PguardFromLo(palo);
    if (!pg) return GrfStepXp(palo);
    if (FGuardState(pg, 0x14)) return 1;
    if (pg->grfState & (GrfStateBit(0x12) | GrfStateBit(0x13) | GrfStateBit(0x16))) return 0;
    if (pg->move.pjob) return static_cast<uint32_t>(pg->move.pjob->State());
    return GrfStepXp(palo);
}

// orig: 0x00206380 (vt slot 61: the ALO's kind, at least 1 in state 7 against an object that is not still, or in state
// 0x17 against the same +0xEC as the 0xB1 job's object in its kinds 0 / 4 / 5)
int OverlapKindGuard(ALO* pa, ALO* pb)
{
    using PFN = int (*)(ALO*, ALO*);
    PFN pfnBase = reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8));
    int r = pfnBase ? pfnBase(pa, pb) : 0;
    GUARD* pg = PguardFromLo(pa);
    if (!pg) return r;
    if (FGuardState(pg, 7))
    {
        using PFNS = bool (*)(ALO*);
        PFNS pfnStill = reinterpret_cast<PFNS>(PvNativeVt(pb, 62));
        const bool fStill = pfnStill ? pfnStill(pb) : FAloStill(pb);
        if (!fStill) r = std::max(r, 1);
    }
    else if (FGuardState(pg, 0x17))
    {
        if (auto* pj = dynamic_cast<JOBB1*>(PjobFind(pg, 0xB1)); pj &&
            (pj->stLaunch == 0 || pj->stLaunch == 4 || pj->stLaunch == 5) && pj->ploOwner &&
            FCidDerivedFrom(pj->ploOwner->Cid(), 0x30) && Poxa(static_cast<ALO*>(pj->ploOwner)) == Poxa(pb))
            r = std::max(r, 1);
    }
    return r;
}

}  // namespace
// orig: 0x00207530 (vt slot 86: a hit that knocks out: at least +0x338 deep, of the kinds that hurt)
bool FGuardHitMatters(GUARD* pg, const CTMSG* pctm)
{
    if (pg->palo->pcls && pg->palo->pcls->PfnOrig(86) == 0x14B460) return false;  // (class 0x3F: never)
    if (-pctm->nState < pg->n338) return false;
    const int32_t k = static_cast<int32_t>(pctm->grfFilter);
    if (k == 0x80000) return false;
    if (0x80000 < k) return k != 0x100000;
    if (k < 0) return true;
    return 2 <= k;
}

namespace
{
// orig: 0x00207598 (vt slot 76: busy)
bool FGuardBusy(GUARD* pg) { return pg->f32C != 0 || pg->f298 != 0 || FGuardState(pg, 42); }

// orig: 0x00207468 (vt slot 75: a contact accepted; the ones that matter are offered to the jobs as message 12)
bool FAcceptContactGuard(ALO* palo, CTMSG* pctm)
{
    GUARD* pg = PguardFromLo(palo);
    if (!pg) return pctm->grfAccepted != 0;
    static const bool s_fDebug = std::getenv("SLY2N_HITDEBUG") != nullptr;
    if (s_fDebug)
        std::printf("hit: t %.3f guard %d from %d filter 0x%X state %d accepted 0x%X matters %d\n", g_clock.t, palo->oid,
                    pctm->ploSender ? pctm->ploSender->oid : -1, pctm->grfFilter, pctm->nState, pctm->grfAccepted,
                    FGuardHitMatters(pg, pctm));
    if (FGuardHitMatters(pg, pctm))
    {
        OfferJobMessage(pg, 12, pctm);
        return true;
    }
    if (FGuardBusy(pg)) return true;
    // TODO(ai): a sender of class 0x4F whose +0x200's +0x14 is the guard's +0x14: accepted without the jobs
    if (pctm->grfAccepted != 0)  // (0x1F7588)
    {
        OfferJobMessage(pg, 12, pctm);
        return true;
    }
    return false;
}
}  // namespace

// orig: 0x00209EE0
void ReleaseGuardHeld(LO* plo)
{
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return;
    if (FGuardState(pg, 0x12) || FGuardState(pg, 0x13))
    {
        if (AIJOB* pj = PjobFind(pg, 0xB0)) pj->SetState(2);  // 0x17A8C8: let go
    }
    else
    {
        if (!FGuardState(pg, 0x17)) return;
        auto* pj = dynamic_cast<JOBB1*>(PjobFind(pg, 0xB1));
        if (pj && pj->stLaunch != 3 && pj->stLaunch != 6) pj->SetState(2);
    }
    SetAloVelocityTo(pg->palo, cmm::LimitVectorLength(VelAloPos(pg->palo), 800.0f));  // ELF2C3DA8
}

// orig: 0x00209E28
void ReactGuardHeld(LO* plo, LO* ploHolder)
{
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return;
    auto* pr = dynamic_cast<JOBREACT*>(PjobFind(pg, 0x9C));
    if (!pr) return;
    AIMSG m;  // (cleared: 0x2333B4)
    m.k = 6;
    m.plo = ploHolder;
    pr->React(m);  // 0x176750
}

// orig: 0x00209E88
void CarryGuardHeld(LO* plo)
{
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return;
    if (FGuardState(pg, 0x16) || FGuardState(pg, 0x12))
        if (AIJOB* pj = PjobFind(pg, 0xB0)) pj->SetState(1);
}

// orig: 0x00209FB8
void ThrowGuardHeld(LO* plo)
{
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return;
    if (FGuardState(pg, 0x16) || FGuardState(pg, 0x13))
        if (AIJOB* pj = PjobFind(pg, 0xB0)) pj->SetState(3);
}

NATIVEFN(0x20B090, GrfGuardXp);
NATIVEFN(0x206380, OverlapKindGuard);
NATIVEFN(0x207468, FAcceptContactGuard);

// orig: 0x00207598 (vt slot 76, for the watchers: guard.cpp)
bool FGuardBusy76(GUARD* pg) { return FGuardBusy(pg); }
