// Sly 3's in-place world reset (audit 4 CORE-05): orig3 0x25C2A8's else branch, 0x25BDD0.
//
// The transition 0x25C2A8 reloads the world file only when the request names another world, forces it (flag 2), a
// movie batch is queued (D_00389AC0), or the world has no file. Otherwise it resets the loaded world in place,
// 0x25BDD0, and runs the same start tail as after a load (0x184AA0, 0x1FC458, 0x189118, 0x21D948, 0x25B9C0, the
// cameras cut, 0x184B60, 0x25C128, the wipe). The requests that take this path: a death with no job (0x1855F0, flags 0),
// a job's retry after Job Failed (0x186FE8 -> 0x189F38: the same job and world, flags 4), an exit into its own world
// (0x27FDE8, flags 0), the splice method 0x289020.
//
// 0x25BDD0, in order:
//   0x1D2C90(D_0046F550)                the HUD record stack drained
//   0x25BD68                            every SCPT (class 0x87) stopped (0x226050)
//   0x1F4B00(p, 0 / 1, 0)               both players' characters unset (0x1F4B38: the old one's slot 83 (-1))
//   SW +0x3EB8's objects, slot 57       the players' cameras taken off the world's list (0x1F4B38 puts them back)
//   SW +0x4FA4 = 0; SW +0x4050          a job quit by its marker restarted (0x187118)
//   0x188620 / 0x186298 in mode 2       the active task and the current job left, not saved; GAME +0x1498..+0x14A0 kept
//   every task -1 in mode 2             (0x18A900's list, 0x18A3E8)
//   0x210C90(pad, 3)                    the pads' rumble stopped
//   0x25BBD0                            the particle owners (SW +0x480C, 0x203078) let go; the PSYS (0x1A) and RIPG
//                                       (0x63) objects removed (slot 9)
//   0x25BD20                            every noise removed (0x1739A8)
//   0x15AE70 / 0x15B790                 the activation queues flushed
//   0x1FC6A0                            every object of the world's tree (not a spare proxy row's) slot 30: LO 0x1FC768
//                                       (slot 32 the class's release, then splice event 18), a PROXY 0x1FC7D0 (its rows
//                                       not held detached, the pooled objects back), up to 16 passes
//   0x247588                            the sound manager: every sound stopped but the music
// The start tail's 0x1FC458 then gives every object slot 29 (0x1FC868: slot 31 the class's reset, then event 17):
// ALOs with a reset pose (EOPID 86, optid 209: the record +0x120 filled by the post-load 0x23C580) go back to it,
// breakables to their first event (0x1E6058), guards home (0x250E38), the players' health full (0x24DA88 ...).
//
// What stays (the reference: --warp 3:1826, a death broadcast 0x1854C8(GAME, 0xC, 0) with no job, RAM before and
// after: 0x25BDD0 ran at vblank 3690, no load): the world's objects themselves (no new SW: the scripts' records, the
// objects' options set by scripts, the SMs' states not driven by tasks, the positions of the ALOs without a reset pose
// (69 in v_ext's case: the safe house's moved parts), objects out of the world kept out (Bentley)). The proxied guards
// and their coins went back to their pools (released, then attached again by distance). The checkpoint task keeps its
// suck timer (task3.cpp). Natively: SLY3_INPLACERESET=0 reloads the world for those requests as before.

#include "rowboat3.h"
#include "../activate.h"
#include "../aicarm3.h"
#include "../ainoise.h"
#include "../breakable.h"
#include "../codepanel3.h"
#include "../murraycarry.h"
#include "../gang.h"
#include "../booty.h"
#include "../binoc3.h"
#include "../binoc.h"
#include "../cls.h"
#include "../damage.h"
#include "../actor.h"
#include "../guard.h"
#include "../jobend3.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../po.h"
#include "../rumble.h"
#include "../ref.h"
#include "../so.h"
#include "../stepphys.h"
#include "../sound.h"
#include "../spliceglue.h"
#include "../sw.h"
#include "../transition.h"
#include "../update.h"
#include "../vtnative.h"
#include "../xf.h"
#include "scptgame3.h"
#include "sly3.h"
#include "task3.h"
#include "trigger3.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

void DetachProxyRowsInPlace3(SW* psw, LO* pproxy);   // (activate.cpp: orig3 0x1FC7D0)
void RestoreActorDamage3(ACTOR* pa);                 // (damage.cpp: 0x24DA88's health)
void ResetJobEndInPlace3(JOB3* p);                   // (jobend3.cpp: MISSION slot 32 0x1878A0)
void SetExitState(LO* plo, int n);                   // (frontend.cpp: 0x27FC08)

namespace
{
bool FPrint() { static const bool s_f = std::getenv("SLY3_INPLACEPRINT") != nullptr; return s_f; }

// orig3: the reset pose record +0x120 (EOPID 86 allocates it, 0x241678; ALO slot 17 0x23C580 fills it after the load:
// the world matrix, the velocities, the out-of-world bit 52)
struct POSE3
{
    glm::mat4 matLocal{1.0f};
    glm::vec3 vel{0.0f}, velAng{0.0f};
    bool fOut = false;
    uint32_t serial = 0;
};
std::unordered_map<LO*, POSE3> s_mppose;

bool FResetPose(const LO* plo)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == 86 && !o.aval.empty()) return o.aval[0].n != 0;
    return false;
}
}  // namespace

bool FWorldResetInPlace3()
{
    static const bool s_f = [] { const char* sz = std::getenv("SLY3_INPLACERESET"); return !sz || sz[0] != '0'; }();
    return s_f;
}

void RecordResetPoses3(SW* psw)
{
    s_mppose.clear();
    if (!psw) return;
    for (const auto& pl : psw->aploAll)
    {
        LO* plo = pl.get();
        if (!plo || !FCidDerivedFrom(plo->Cid(), 0x2F) || !FResetPose(plo)) continue;
        ALO* palo = static_cast<ALO*>(plo);
        if (palo->axf.empty()) continue;
        POSE3& p = s_mppose[plo];
        p.matLocal = palo->axf[0]->mat;
        p.vel = VelAloPos(palo);
        p.velAng = AngVelAlo(palo);
        p.fOut = !plo->fInWorld;
        p.serial = psw->serial;
    }
    if (FPrint()) std::printf("inplace: %zu reset poses recorded\n", s_mppose.size());
}

namespace
{
// orig3 0x1FC968's pose part (slot 31 with no owner): the position and rotation records back (0x1C9428 / 0x1C9598 to
// the parent's frame), the velocities, then in or out of the world as after the load (slot 9 / 8)
void RestoreResetPoses3(SW* psw)
{
    for (auto& [plo, p] : s_mppose)
    {
        if (p.serial != psw->serial) continue;
        ALO* palo = static_cast<ALO*>(plo);
        if (palo->axf.empty()) continue;
        palo->axf[0]->mat = p.matLocal;
        UpdateAloHierarchyWorlds(palo);
        if (PxfPos(palo)) SetAloVelocityTo(palo, p.vel);
        if (PxfRot(palo)) SetAloAngVelocityTo(palo, p.velAng);
        if (p.fOut && plo->fInWorld) RemoveLo(psw, plo);
        else if (!p.fOut && !plo->fInWorld) AddLo(psw, plo);
    }
}

// orig3 0x1FC6A0: the release of every object (slot 30): LO 0x1FC768 = slot 32 (natively Sly 2's slot 27, the ALO
// base telling its children) and splice event 18; a proxy's 0x1FC7D0 detaches its rows that are not held first
void ReleaseObjects3(SW* psw)
{
    using PFN27 = void (*)(LO*, LO*);
    std::vector<LO*> aplo;
    for (const auto& pl : psw->aploAll)
        if (pl && (!pl->fSpareNs || pl->fInWorld)) aplo.push_back(pl.get());   // (a spare row's object in use: acquired)
    static const bool s_fProxy = [] { const char* sz = std::getenv("SLY3_INPLACEPROXY"); return !sz || sz[0] != '0'; }();
    if (s_fProxy)
        for (LO* plo : aplo)
            if (FCidDerivedFrom(plo->Cid(), sly3::CID_PROXY)) DetachProxyRowsInPlace3(psw, plo);
    // the classes' slot 32 the native release has no body for: MISSION 0x1878A0 (a job in its end state 8 back to 0,
    // the game resumed), EXIT 0x27FBD8 (state 0: an exit that fired can fire again, its pause undone)
    for (auto& pj : g_tasks3.ajob) ResetJobEndInPlace3(pj.get());
    for (LO* plo : aplo)
        if (FCidDerivedFrom(plo->Cid(), 0x18)) SetExitState(plo, 0);   // (Sly 3 EXIT 0x18)
    CRef refNone;
    for (LO* plo : aplo)
    {
        // (the native ALO slot 27 tells the children itself: called on the tree's roots)
        if (!plo->paloParent)
            if (PFN27 pfn = reinterpret_cast<PFN27>(PvNativeVt(plo, 27))) pfn(plo, nullptr);
        HandleLoSpliceEvent(plo, SPLICEEV3_RELEASE, false, 1, &refNone);
    }
}
}  // namespace

void ResetSwInPlace3(SW* psw)
{
    if (!psw) return;
    if (FPrint()) std::printf("inplace: reset of world %d (objects %zu, job %d, task %d)\n", psw->filter, psw->aploAll.size(),
                              g_gametasks3.oidJob, g_gametasks3.oidTask);
    StopScripts3();                                     // 0x25BD68
    // 0x1F4B00(p, 0 / 1, 0): the players' characters unset (their pads off: slot 83 (-1)); the world start's player
    // selection (transition.cpp, 0x25B9C0) takes them again, the others not taken are removed there
    for (LO* plo : std::vector<LO*>(AploRegistered()))
        if (plo && NPoEnabled(plo) == 1) EnablePo(psw, plo, 0);
    // (a played Carmelita (a guard, not a PO: aicarm3.cpp) gives her pad back the same way: her slot 83 (-1), the
    // reaction back to its AI; e.g. the turf war's Carmelita 28 on a retry)
    for (const auto& pl : psw->aploAll)
        if (pl && IPlayerCarm3(pl.get()) >= 0) SetCarmPlayer3(pl.get(), -1);
    SetPoCurrent(psw, nullptr);
    ResetTasksInPlace3();                               // 0x187118 / 0x188620 / 0x186298 / 0x18A3E8 in mode 2
    StopJoyRumble();                                    // 0x210C90(pad, 3)
    // 0x25BBD0: the particle systems and ripples out of the world (their slot 9)
    for (CID cid : {CID(0x1A), CID(0x63)})   // (Sly 3 cids)
        for (LO* plo : std::vector<LO*>(psw->mpcidaplo[cid]))
            if (plo->fInWorld) RemoveLo(psw, plo);
    // 0x25BD20: every noise removed
    for (NOISE* pn : std::vector<NOISE*>(ApnoiseAll())) RemoveNoise(pn);
    ReleaseObjects3(psw);                               // 0x1FC6A0
    ResetSoundMgrInPlace3();                            // 0x247588
    // the start tail's 0x1FC458 (natively StartSw's order: the guards' slot 26 + 17, then 17 to the others, then the
    // tasks' restore 0x184AA0 / 0x184B60): the class resets first (slot 31)
    RestoreResetPoses3(psw);
    // the classes' slot 31 with a native body (Sly 2's slot 26: birds, vehicles, clue bottles, projectiles, ships,
    // boats ...; the guards' and the hack objects' come below with the start events), on the tree's roots (the native
    // bodies tell their children as Sly 2's did). Off unless SLY3_INPLACESLOT31=1: against the reference's job retry
    // in m_ext (Sly standing at the checkpoint at z -2913.30) it left him at -2912.96 (some ground object's native reset
    // is not Sly 3's slot 31), without it at -2913.30
    static const bool s_fSlot31 = [] { const char* sz = std::getenv("SLY3_INPLACESLOT31"); return sz && sz[0] == '1'; }();
    using PFN26 = void (*)(LO*, LO*);
    if (s_fSlot31)
        for (LO* plo : [&] { std::vector<LO*> a; for (const auto& pl : psw->aploAll) a.push_back(pl.get()); return a; }())
        {
            if (!plo || plo->fSpareNs || plo->paloParent || PguardFromLo(plo) || FCidDerivedFrom(plo->Cid(), 0x49)) continue;
            if (PFN26 pfn = reinterpret_cast<PFN26>(PvNativeVt(plo, 26)))
            {
                if (FPrint()) std::printf("inplace: slot 31 of %d (cid 0x%X)\n", plo->oid, plo->Cid());
                pfn(plo, nullptr);
            }
        }
    for (const auto& pl : psw->aploAll)
    {
        LO* plo = pl.get();
        if (!plo || (plo->fSpareNs && !plo->fInWorld)) continue;
        if (FCidDerivedFrom(plo->Cid(), 0x6F)) ResetBreakable(plo);       // 0x1E6058 (Sly 2 0x1BBEA8): the first event, the broken parts undone
        if (ACTOR* pa = PactorFromLo(plo); pa && FCidDerivedFrom(plo->Cid(), 0x32)) RestoreActorDamage3(pa);   // 0x24DA88: health full
        // 0x24DA88: the PO's heading from its x axis (0x24E878(p, atan2(x.y, x.x)))
        if (FCidDerivedFrom(plo->Cid(), 0x32) && FCidDerivedFrom(plo->Cid(), 0x2F))
        {
            const glm::mat4 m = sly3::MatLoWorld3(plo);
            SetStepPhysHeading(static_cast<ALO*>(plo), std::atan2(m[0].y, m[0].x));
        }
    }
    sly3::ResetTriggersInPlace3();                      // (VOLTRIGGER slot 31 0x15D2A8)
    ResetGuardsAtStart(psw);
    SendStartEvents3(psw);
    // (the load's passes leave out the objects of spare rows (fSpareNs); 0x1FC1F0 tests the row's node, which an
    // acquired row clears: the ones in the world get their reset and event 17 here, e.g. the turf war's playable
    // Carmelita 28 (a spare row's instance), whose knock-out (state 14) the retry must end)
    {
        CRef refNone;
        for (const auto& pl : psw->aploAll)
        {
            LO* plo = pl.get();
            if (!plo || !plo->fSpareNs || !plo->fInWorld) continue;
            if (PguardFromLo(plo)) ResetGuard(plo, nullptr);
            HandleLoSpliceEvent(plo, SPLICEEV3_ATTACH, false, 1, &refNone);
        }
    }
    RestoreTasks3AtStart(psw);
    if (FPrint()) std::printf("inplace: done (job %d, task %d, chk %d)\n", g_gametasks3.oidJob, g_gametasks3.oidTask, g_gametasks3.oidChk);
}

// (debug: SLY3_DEATHAT=F[,F..]: the player's death broadcast 0xC at those frames, the reference's
// SLY2_EECALL="VB:1854C8(36C700,C,0)")
void DebugDeathAt3(int frame)
{
    static const char* s_sz = std::getenv("SLY3_DEATHAT");
    for (const char* q = s_sz; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
        if (std::atoi(q) == frame)
        {
            std::printf("debug: death broadcast at frame %d\n", frame);
            Broadcast3(0xC, nullptr);
        }
    // (SLY3_RETRYAT=F: the current job's retry 0x186FE8 at frame F, the reference's SLY2_EECALL="VB:186FE8(job)")
    static const char* s_szRetry = std::getenv("SLY3_RETRYAT");
    if (s_szRetry && std::atoi(s_szRetry) == frame)
        if (JOB3* pj = g_tasks3.pjobCur)
        {
            std::printf("debug: job %d retry at frame %d\n", pj->oid, frame);
            if (g_jobendhooks3.pfnRestart) g_jobendhooks3.pfnRestart(pj);   // (task3.cpp JobRestart3)
            JobEndQuit3(pj);
        }
}

namespace
{
// orig3: the world's splice methods EOPID 981 / 982 (optid 980 / 981, thunks 0x10A8E8 / 0x10A960): the current world
// again at point 0x1A8 with no wipe, 0x289020 (flags 0: reset in place) / 0x289048 (flags 2: reloaded). Natively they
// were not registered (a script's call did nothing).
struct INITWORLDRESET3
{
    INITWORLDRESET3()
    {
        RegisterObjectMethod3(0x10A8E8, [](LO*, const std::vector<CRef>&) {
            if (g_psw) RequestTransition(g_psw, nullptr, sly3::POINT_DEFAULT, -1, TRANSF_INPLACE3, -1);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x10A960, [](LO*, const std::vector<CRef>&) {
            if (g_psw) RequestTransition(g_psw, nullptr, sly3::POINT_DEFAULT, -1, 0, -1);
            return CRef(TAGK_Void);
        });
    }
} s_initworldreset3;
}  // namespace

// ---- the player classes' releases (vt slot 32, Sly 2's 27) on an in-place reset (0x1FC6A0) ----
// Natively these fell back to SO's 0x1F3C80 (the "[vt] no native function" lines of an in-place reset).
void DetachSoBase32(LO* plo, LO* pproxy);   // (activate.cpp: orig3 0x1F3C80)
void EndActorAbilitySlots(ACTOR* pa);        // (gadget.cpp: orig3 0x154350 per slot)
namespace
{
// orig3: 0x24DB18 (PO): SO's 0x1F3C80, then the owned-record list +0x29C emptied (0x238568: the PO's retained
// records; natively none are kept there)
void DetachPo3Slot32(LO* plo, LO* pproxy) { DetachSoBase32(plo, pproxy); }
NATIVEFN3(0x24DB18, DetachPo3Slot32);

// orig3: 0x152828 (the gang's base, class 0x33): PO's 0x24DB18; vt slot 102: the binocucom put down (the reference's
// m_ext retry: 0x1BDBB0(+0xF60, 2) from 0x1BDB50, then (-1), during 0x25BDD0); 0x1C20A8(+0xE40): the action state -1;
// 0x155ED8(p, 0): the booty it carries dropped (its save record kind 2); the link record +0x940 (0x2093E8: its owner
// told (0x1C7A00(owner, 0, 15)) and the link cleared; natively no actor keeps one across a reset); 0x153EA0(p, 0) (the
// camera-target records follow him again); 0x154350 per item slot (a held item ended); +0x1274 = 0; the state -1
// (slot 99: left; the start's placement sets the next one)
void DetachActor3Base(LO* plo, LO* pproxy)
{
    DetachPo3Slot32(plo, pproxy);
    ACTOR* pa = PactorFromLo(plo);
    if (!pa) return;
    if (pa->nF04 != -1)
    {
        SetBinocState(pa, 2);
        SetBinocState(pa, -1);
    }
    else ShutBinocMode3(plo);   // (the scripts' binocucom: the same +0xF60 in the original)
    DropActorBooty(pa);
    // 0x153EA0(p, 0): the camera-target records follow the actor again (their offset kept, z cleared: sly.cpp's
    // message 0x183)
    if (pa->f1044)
    {
        pa->f1044 = 0;
        const glm::vec3 d = glm::vec3(pa->posCmTarget) - PosAloCenter(pa->palo);
        pa->vecOffset = glm::vec3(d.x, d.y, 0.0f);
    }
    // 0x154350(p, slot i, 0) for the three item slots: a held item ended (gadget.cpp)
    EndActorAbilitySlots(pa);
}
NATIVEFN3(0x152828, DetachActor3Base);

// orig3: 0x1AA5F0 (Sly, class 0x34): the base 0x152828, 0x241928(p, 0) (his 0xB2 children's scalar) and his pin
// record +0x18F0 (0x2093E8)
void DetachSly3Slot32(LO* plo, LO* pproxy) { DetachActor3Base(plo, pproxy); }
NATIVEFN3(0x1AA5F0, DetachSly3Slot32);

// orig3: 0x13D440 (Bentley, class 0x36): the base 0x152828, then his vt slot 116 (13): 0x1403F8, the item 13's
// record +0x19B8 cleared (the placed Grapple-Cam forgotten; the cam object itself is reset with the world)
void DetachBentley3Slot32(LO* plo, LO* pproxy)
{
    DetachActor3Base(plo, pproxy);
    if (BENTLEY* pb = PbentleyFromLo(plo)) pb->ploGrappleCam = nullptr;
}
NATIVEFN3(0x13D440, DetachBentley3Slot32);

// orig3: 0x1DCE40 (Murray, class 0x35): 0x1E47F0(p, 0): what he holds (+0x1C90) let go; 0x1E04E8; the base 0x152828;
// his notify object's vt slot 9
void DetachMurray3Slot32(LO* plo, LO* pproxy)
{
    if (MURRAY* pm = PmurrayFromLo(plo))
    {
        SetMurrayBoat3(pm, nullptr);   // (0x1E47F0(p, 0): out of his boat, game/rowboat3.cpp)
        CancelMurrayCarry(pm);
    }
    DetachActor3Base(plo, pproxy);
}
NATIVEFN3(0x1DCE40, DetachMurray3Slot32);

// orig3: 0x20A6A0 (DIRECTOR 0x46): SO's 0x1F3C80, then 0x20ADE0 (+0x424 / +0x46C / +0x470 cleared: its running
// shot; natively game/director3.cpp keeps none across a reset)
void DetachDirector3Slot32(LO* plo, LO* pproxy) { DetachSoBase32(plo, pproxy); }
NATIVEFN3(0x20A6A0, DetachDirector3Slot32);

// orig3: 0x171188 (the code panel 0x50): SO's 0x1F3C80, then 0x171538(p, -1)
void DetachCodePanel3Slot32(LO* plo, LO* pproxy)
{
    DetachSoBase32(plo, pproxy);
    sly3::ReleaseCodePanel3(plo);
}
NATIVEFN3(0x171188, DetachCodePanel3Slot32);
}  // namespace
