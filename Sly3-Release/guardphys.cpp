// The guards' (class 0x38..) overrides of the physics' vtable slots, which read their AI state:
// 85 0x20B090 (the floor probes / contacts), 61 0x206380 (the overlap kind), 75 0x207468 (a contact
// accepted: the jobs get message12) with86 0x207530 /76 0x207598. Slot59 0x209C88
// adds the rail contacts for attached movement mode 0, the ladder's (mode 1) and the bar's (mode 2); mode 3 remains unported.

#include "airide3.h"
#include "guard.h"
#include "projectile.h"
#include "game/sly3.h"
#include "aihit.h"

#include "ai.h"
#include "aijobs.h"
#include "so.h"
#include "vtnative.h"
#include "clock.h"
#include "vecmat.h"
#include "cls.h"
#include "climb.h"
#include "jt.h"
#include "activate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "game/suck3.h"
#include "sw.h"
#include "po.h"
namespace
{
// orig3 0x172268 (D_0036BD20's row +0x20 / +0x24: 1 / 0.5): a player's object (here: the current player) takes the
// amount scaled by the suck (0x171C98), at least 1 (as hack.cpp's ships)
int NScaleDamagePlayer3(const LO* plo, int n)
{
    if (!g_psw || plo != PloPoCurrent(g_psw) || n <= 0) return n;
    const float g = GSuck3();
    const int v = static_cast<int>(((1.0f - g) * 1.0f + g * 0.5f) * static_cast<float>(n));
    return v < 1 ? 1 : v;
}
}  // namespace

namespace
{
// orig: 0x00209C88, slot59, rail case0. The base0x1F4FA8 is empty.
void AddGuardRailContacts(ALO* palo)
{
    GUARD* guard = PguardFromLo(palo);
    if (!guard) return;
    if (guard->kMove == 2 && guard->ploMove)
    {
        // case 2, on a bar: the hands (+0xF90) held on the bar's line (its place, its x row), all three ways
        const glm::vec3 hands = guard->ploF90 ? PosLoWorld(guard->ploF90) : PosAi(palo);
        AddStepLineXps(&StepPhysOf(palo), 7, PosLoWorld(guard->ploMove), glm::vec3(MatJtLo(guard->ploMove)[0]), hands,
                       guard->ploMove);
        return;
    }
    if (guard->kMove == 1 && guard->ploMove)
    {
        // case 1, on a ladder / pipe: the hands (+0xF90) held on its point at the place and its direction there, all
        // three ways (R50: the Ep4 Contessa's drain pipe)
        if (const CRV* curve = PcrvFromLo(guard->ploMove))
        {
            glm::vec3 position, direction;
            EvalCrvS(curve, guard->sMove, &position, &direction);
            const glm::vec3 hands = guard->ploF90 ? PosLoWorld(guard->ploF90) : PosAi(palo);
            AddStepLineXps(&StepPhysOf(palo), 7, position, direction, hands, guard->ploMove);
        }
        return;
    }
    if (guard->kMove != 0) return;
    if (const CRV* curve = PcrvFromLo(guard->ploMove))
    {
        glm::vec3 position, direction;
        EvalCrvS(curve, guard->sMove, &position, &direction);
        AddStepLineXps(&StepPhysOf(palo), 0x22, position, direction, PosGuardProbe(guard), guard->ploMove);
    }
}

// orig: 0x0020B090 (vt slot 85: 1 in state 0x14; 0 in 0x12 / 0x13 / 0x16; else the move job's slot 12, else STEP's
// 0x2040F8)
uint32_t GrfGuardXp(ALO* palo)
{
    GUARD* pg = PguardFromLo(palo);
    if (!pg) return GrfStepXp(palo);
    if (FGuardState(pg, 0x14)) return 1;
    if (pg->grfState & (GrfStateBit(0x12) | GrfStateBit(0x13) | GrfStateBit(0x16))) return 0;
    if (const int n = GrfRideXp3(pg); n >= 0) return static_cast<uint32_t>(n);   // (Sly 3's ride jump, airide3.h)
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
    // (debug experiment, s3-ai round 3: SLY3_EXPNOBRK=1 lets the guards pass through breakables: v_interpol's seated police
    // then settle at the reference's z 142 instead of being pushed up onto the chairs to z 200; see notes/AI3_2026-11-10.md)
    if (static const bool s_f = std::getenv("SLY3_EXPNOBRK") != nullptr; s_f && FCidDerivedFrom(pb->Cid(), S3CID(0x62))) return std::max(r, 1);
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
            FCidDerivedFrom(pj->ploOwner->Cid(), S3CID(0x30)) && Poxa(static_cast<ALO*>(pj->ploOwner)) == Poxa(pb))
            r = std::max(r, 1);
    }
    return r;
}

}  // namespace
// orig: 0x00207530 (vt slot 86: a hit that knocks out: at least +0x338 deep, of the kinds that hurt). Sly 3 has no such
// rule (its 0x251C08 knocks out at health 0): only the Sly 3 handler's own knockout (CTMSG_KO3) answers true.
bool FGuardHitMatters(GUARD* pg, const CTMSG* pctm)
{
    (void)pg;
    return (pctm->grfAccepted & CTMSG_KO3) != 0;
}

namespace
{
// orig: 0x00207598 (vt slot 76: busy)
bool FGuardBusy(GUARD* pg) { return pg->f32C != 0 || pg->f298 != 0 || FGuardState(pg, 42); }

// orig3: 0x00251C08 (Sly 3's guard vt slot 78, paired with Sly 2's slot 75 0x207468 in vtmap3.cpp): a hit taken.
// Sly 3's hits carry a positive amount (CTMSG::nState, e.g. Sly's cane 10) and the contact's flags (grfAccepted: 2 a
// real hit, 4 a flinch, 8 a hit, 0x10 a knockout, 0x20 a stagger); the health is the SO's +0x16C (natively n33C, its
// maximum n338 from option 939: guardstart.cpp). The outcomes the original hands to the job stack as Sly 3 messages
// (0xD asleep, 0xE stagger, 0xF hit, 0x10 knocked out, 0x11 launched) go to the copied reaction job as Sly 2's: the
// contact (message 12) with Sly 2's flags (0x40 knocked out: FGuardHitMatters, 0x20 a hit, 0xA0 a surprised one) and no
// amount left (the health is already lowered here), the dart's sleep (8) and the launch (7) directly.
// Not ported: vt slots 97 (an instant knockout) / 80 (a kind ignored), a projectile of the guard's own owner, the
// state-dependent kind masks +0x370..+0x378, the shield (+0x4C8, message 0x12), the guard states 0x16 / 0x10 / 0xE,
// the hit effects (D_00479C28 / D_00479C34), the flinch's clip and broadcast 0xF.
bool FAcceptContactGuard(ALO* palo, CTMSG* pctm)
{
    GUARD* pg = PguardFromLo(palo);
    if (!pg) return pctm->grfAccepted != 0;
    static const bool s_fDebug = std::getenv("SLY2N_HITDEBUG") != nullptr;
    uint32_t grf = pctm->grfAccepted;
    const uint32_t kind = pctm->grfFilter;
    int amount = pctm->nState;
    if (s_fDebug)
        std::printf("hit: t %.3f guard %d from %d filter 0x%X amount %d flags 0x%X hp %d/%d\n", g_clock.t, palo->oid,
                    pctm->ploSender ? pctm->ploSender->oid : -1, kind, amount, grf, pg->n33C, pg->n338);
    if (!(grf & 0x3E)) return false;
    // vt slot 97 (0x251B40 -> the SO's 0x250290): a fatal hit (the record's +0x28: 0x240CB0's sixth argument, SCPT
    // 0x6B's A3) of any kind but none / 1 / the dart 0x80000 / 0x100000 knocks out at once (health 0, flags 0x10), past
    // the owner / kind / shield tests. (Not ported: the guard's own two cases, a class-0x71 sender in its state 6 aimed
    // at it, a class-0x72 sender in the guard states 5 / 6 / 0x23)
    const bool fFatal = (pctm->nKill3 || pctm->fKill3) && kind > 1 && kind != 0x80000 && kind != 0x100000;
    if (fFatal)
    {
        pg->n33C = 0;
        grf = 0x10;
        amount = 0;
    }
    // (the original ignores its own owner's projectiles; natively the copied Sly 2 senders also hit the guard itself,
    // e.g. its landing, with Sly 2's negative amounts)
    if (!fFatal && pctm->ploSender == palo) return false;
    // (orig3 0x251C08: a projectile whose owner (+0x1E8, natively its launcher) is this guard: e.g. the playable
    // Carmelita's own shots, which hit everything around where they end)
    if (!fFatal && PloProjectileLauncher(pctm->ploSender) == palo) return false;
    if (amount < 0) amount = -amount;
    // (orig3 0x240CB0 -> 0x240C78 -> 0x172268: a contact's amount to a player's object (class 0x31 under a player's
    // control, 0x1F4350(receiver, -1)) is scaled by the suck: 1 .. 0.5 (D_0036BD20 +0x20 / +0x24), at least 1; e.g. the
    // turf war's Carmelita after a death (suck 0.1): the wave guards' shots take 4 and 9 instead of 5 and 10, as the
    // reference's +0x16C (build/ref/turf21 run g: 40 36 32 after the restart))
    if (!fFatal && amount > 0) amount = NScaleDamagePlayer3(palo, amount);
    if (pg->f29C) amount = 0;  // (+0x29C: invulnerable)
    auto react = [&](int k) {
        if (auto* pr = dynamic_cast<JOBREACT*>(PjobFind(pg, 0x9C)))
        {
            AIMSG m;
            m.k = k;
            m.plo = pctm->ploSender;
            pr->React(m);
        }
    };
    if (kind == 0x80000 && (grf & 2))
    {
        pg->n33C -= amount;  // the dart: asleep at 0
        if (pg->n33C <= 0) react(8);
        return true;
    }
    if (kind == 0x200000 && (grf & 2) && pctm->ploSender && FGuardLaunchAllowed(pg, pctm->ploSender, GrfStateBit(6)))
    {
        pg->n33C = std::max(pg->n33C - amount, 5);
        react(7);
        return true;
    }
    if ((kind == 0x400000 || kind == 0x800000) && (grf & 2))
    {
        grf |= pctm->ploSender && FGuardSurprised(pg, pctm->ploSender, GrfStateBit(0x2A)) ? 0x20 : 8;
        SetGuardMode(pg, kind == 0x400000 ? 8 : 0x22);  // (Sly 3's modes 0xE / 0xC)
    }
    pg->n33C = std::clamp(pg->n33C - amount, 0, std::max(pg->n338, 0));
    if (pg->n33C <= 0) grf |= 0x10;
    // (Sly 3: 0x255D78(p, source, mask 0x2A) makes it a stagger; not in the decomp, natively read as the surprise test,
    // and only for a hit that already is one (8): a flinch-only hit (4: an NPC's, e.g. Ep2 job 2843's Murray on guard
    // 46470) staggered scripted guards and stalled the job (gate13d: movie 314 lost). TODO(ai): 0x255D78 and the state
    // kind masks +0x370..+0x378, then the flinch's own stagger)
    else if ((grf & 8) && pctm->ploSender && FGuardSurprised(pg, pctm->ploSender, GrfStateBit(0x2A))) grf |= 0x20;
    if (s_fDebug)
        std::printf("hit3: guard %d hp %d/%d flags 0x%X -> %s\n", palo->oid, pg->n33C, pg->n338, grf,
                    (grf & 0x10) ? "knocked out" : (grf & 0x20) ? "stagger" : (grf & 8) ? "hit" : (grf & 4) ? "flinch" : "none");
    uint32_t grf2 = 0;
    if (grf & 0x10) grf2 = CTMSG_KO3 | 0x40;
    else if (grf & 0x20)
    {
        // (Sly 3: a kind with no first-row hit entry, unless its second row has one, is a plain hit)
        const int k = [&] { for (int i = 0; i < 32; ++i) if (kind & (1u << i)) return i; return 0; }();  // (0x240DD8)
        const bool fPlain = k >= 0 && k < 26 && pg->aakinde[0][k].k == -1 &&
                            (pg->aakinde[1][k].k != -1 || !PkindeBest(pg, GrfStateBit(6)));
        grf2 = fPlain ? 0x20 : 0xA0;
    }
    else if (grf & 8) grf2 = 0x20;
    else return (grf & 4) != 0;  // TODO(ai): the flinch's clip (+0x... 0x137588) and broadcast 0xF
    CTMSG m = *pctm;
    m.grfAccepted = grf2;
    m.nState = 0;
    OfferJobMessage(pg, 12, &m);
    return true;
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
NATIVEFN(0x209C88, AddGuardRailContacts);

// orig: 0x00207598 (vt slot 76, for the watchers: guard.cpp)
bool FGuardBusy76(GUARD* pg) { return FGuardBusy(pg); }
