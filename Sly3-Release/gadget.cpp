// See gadget.h. Reference: func_00141A50.c, func_00141EF8.c, func_0018DA48.c, func_0018DD78.c, func_00136A90.c,
// func_001B9A08.c (sly2-decomp), the code of 0x141C08 / 0x141CD0 / 0x141DD0 / 0x1441A0 / 0x1441C0 / 0x1441E8 and
// Sly2-Reference's m_13FF70.cpp (ActorUpdateAbilities), m_188488.cpp (0x18D760).

#include "gadget.h"
#include "damage.h"
#include "objmethod.h"

#include <algorithm>

#include "actor.h"
#include "activate.h"
#include "ainoise.h"
#include "asega.h"
#include "alo.h"
#include "brx.h"
#include "clock.h"
#include "carry.h"
#include "cls.h"
#include "disc.h"
#include "lo.h"
#include "po.h"
#include "ref.h"
#include "savedata.h"
#include "sly.h"
#include "jump.h"
#include "emitter.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "guard.h"
#include "update.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

int32_t* PnProgressSlot(const std::vector<std::string>& apath);
bool FBentleyAbilityOk(ACTOR* pa, int k);    // gang.cpp (0x136870)
bool StartBentleyAbility(ACTOR* pa, int k);  // gang.cpp (0x136A90)
bool FMurrayAbilityOk(ACTOR* pa, int k);     // gang.cpp (0x1B98A0)
bool StartMurrayAbility(ACTOR* pa, int k);   // gang.cpp (0x1B9A08)
extern SW* g_psw;

namespace
{
void WarnOnce(const std::string& what)
{
    static std::unordered_map<std::string, int> s_mp;
    if (s_mp[what]++ == 0) std::fprintf(stderr, "gadget: %s\n", what.c_str());
}
PADCTL* PpadctlOfActor(ACTOR* p)
{
    PODATA* ppd = PpodataFromLo(p->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
int32_t NElf(uint32_t va) { int32_t n = 0; if (const uint8_t* pb = g_disc.PbElf(va, 4)) std::memcpy(&n, pb, 4); return n; }
float GElf(uint32_t va) { int32_t n = NElf(va); float g; std::memcpy(&g, &n, 4); return g; }
int ItemUse(int id) { return id >= 0 && id < 0x2B ? NElf(0x2BCD00 + 0x20 * uint32_t(id)) : -1; }
int ItemPower(int id) { return id >= 0 && id < 0x2B ? NElf(0x2BCD00 + 0x20 * uint32_t(id) + 0xC) : 0; }

constexpr uint32_t s_agrfSlot[3] = {4, 1, 2};   // D_002C3258: L1, L2, R2 (the pad's layout)

struct ABSLOT
{
    int id = -1;      // +0
    int state = 0;    // +4 (0 idle, 1 held)
    float t = 0.0f;   // +8
};
struct ABILITIES
{
    SW* psw = nullptr;
    ABSLOT a[3];      // +0x1180
    float g1308 = 0;  // +0x1308 the drained power's fraction
    CSndRef snd2044;  // Sly's +0x2044 (the time gadgets' loop)
};
std::unordered_map<ACTOR*, ABILITIES> s_mpab;
}  // namespace
// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record; the SW check below misses a new SW at the old one's address)
void ResetAbilities() { s_mpab.clear(); }
namespace
{

const char* SzPlayer(ACTOR* pa)
{
    if (!pa || !pa->palo) return nullptr;
    return FIsLoDerivedFrom(pa->palo, 0x35) ? "sly" : FIsLoDerivedFrom(pa->palo, 0x37) ? "bentley"
         : FIsLoDerivedFrom(pa->palo, 0x36) ? "murray" : nullptr;
}

// orig 0x00141EF8 (from the post-load 0x1400B8): the slots' ids from the progress "<player> apukCur <i>" (slot 103's
// root); natively on the first update in a world
void UpdateAbilityDisplay(ACTOR* pa);
ABILITIES& AbOf(ACTOR* pa)
{
    ABILITIES& ab = s_mpab[pa];
    if (ab.psw != g_psw)
    {
        ab = ABILITIES{};
        ab.psw = g_psw;
        if (const char* sz = SzPlayer(pa))
            for (int i = 0; i < 3; ++i)
            {
                const CRef r = RefGetProgress({sz, "apukCur", std::to_string(i)});
                ab.a[i].id = r.m_tagk == TAGK_S32 ? r.m_n : -1;
            }
        UpdateAbilityDisplay(pa);  // 0x141EF8's end: vt slot 105
    }
    return ab;
}

// orig: vt slot 105 (0x141E70 nothing; Bentley 0x00136C50): Bentley shows his Hover Pack (glob group 0x24D on his
// back) while he is the current player (0x1C40C0) and Hover Pack 12 is on one of his buttons (0x141EA8), else hides
// it. Called when a slot changes (0x141FC8), after the slots' load (0x141EF8) and when he becomes or stops being the
// current player (slot 78 0x144480)
void UpdateAbilityDisplay(ACTOR* pa)
{
    if (!pa || !pa->palo || !FIsLoDerivedFrom(pa->palo, 0x37)) return;
    const bool fShow = g_psw && PloPoCurrent(g_psw) == pa->palo && GrfAbilityButton(pa, 12) != 0;
    ShowAloGlobs(pa->palo, 0x24D, fShow);  // 0x1F6978 / 0x1F69F8
}

LO* PloSlyPart(ACTOR* pa, uint32_t ib)
{
    for (const auto& [ibPart, plo] : pa->palo->apart)
        if (ibPart == ib) return plo;
    return nullptr;
}

bool FPrint() { static const bool s_f = std::getenv("SLY2N_GADGETPRINT") != nullptr; return s_f; }

// orig: 0x0018D6F8 (the four loaded pocket rows)
SLY::POCKET* PpocketSly(SLY* p, int id)
{
    for (auto& pocket : p->pockets)
        if (pocket.id == id) return &pocket;
    return nullptr;
}

// vt slot 99 (may the action n start): Sly 0x18D760, Bentley 0x136870, Murray 0x1B98A0
bool FSlyCanDo(SLY* p, int n)
{
    if (static_cast<unsigned>(p->nF04) < 3) return false;  // 0x1443D8 (the binocucom)
    const int st = p->state;
    if (n == 0x1D || n == 0x2A)
    {
        if (st == 4) return false;
        const float g = g_clock.af[1];   // (the clock's factor 1, +0x1C)
        if (g == 1.0f) return true;
        return g == GElf(n == 0x1D ? 0x2BF3EC : 0x2BF3F0);
    }
    if (static_cast<unsigned>(st) >= 0x18) return false;
    switch (st)
    {
    case 0: case 1: case 5: case 10: case 12: break;
    case 2: WarnOnce("0x19C8D0 (the jump's slot 99, a gadget in the air)"); return false;
    case 7:
        // orig: 0x194558: interaction permissions precede the item's own state gates.
        // Binocucom (3) is not an assignable gadget; its camera permission remains with that subsystem.
        if (n == 3) return false;
        switch (p->n4C8)
        {
        case 1: case 8: case 11: case 12: return false;
        case 3: case 4: if (n != 1 && n != 2 && n != 31) return false; break;
        default: break;
        }
        break;
    case 8: WarnOnce("0x19EEF0 (+0xF50's slot 99, a gadget)"); return false;
    case 3: if (n != 3) return false; break;
    case 23:
        // orig: 0x18D760: aiming permits only the currently selected item.
        if ((n == 26 || n == 37 || n == 38 || n == 41) && static_cast<unsigned>(p->jtthrow.state - 4) < 3)
            if (const CARRY* carry = PcarryFromLo(p->jtthrow.selected)) return carry->kind == n;
        return false;
    default: return false;
    }
    switch (n)
    {
    case 1: case 2: case 25: case 30: case 31: case 33: case 34: case 35: case 39: return true;
    case 23:  // orig: 0x18D760 / 0x1CB2C0, retained/free proxy row at threshold 2
        if (LO* proxy = PloSlyPart(p, 0x2010)) return PloProxyFreeRow(g_psw, proxy, 2) != nullptr;
        return false;
    case 24: return st == 5;
    case 27: return st == 2;
    case 26: case 37: case 38: case 41:  // orig: 0x18D760 (0x1A / 0x25 / 0x26 / 0x29: the pockets)
    {
        SLY::POCKET* pocket = PpocketSly(p, n);
        if (pocket->retained)
        {
            // orig: 0x224610; a retained object never falls back to the pool.
            const CARRY* carry = PcarryFromLo(pocket->retained);
            return carry && (carry->kind == 7 || carry->kind == 11);
        }
        return pocket->proxy && PloProxyFreeRow(g_psw, pocket->proxy, 2);
    }
    case 40:
        if (p->f1D30 || p->n10EC == 4 || p->mode != -1) return false;
        return st == 0 || st == 1 || st == 10 || st == 12;
    default: return false;
    }
}

bool FCanDo(ACTOR* pa, int n)
{
    if (SLY* p = dynamic_cast<SLY*>(pa)) return FSlyCanDo(p, n);
    if (FIsLoDerivedFrom(pa->palo, 0x37)) return FBentleyAbilityOk(pa, n);
    return FMurrayAbilityOk(pa, n);  // 0x1B98A0
}

// vt slot 100 (the action n starts): Sly 0x18DA48, Bentley 0x136A90, Murray 0x1B9A08. Each first sets the task
// manager's +0xC to n and broadcasts the task event 0x3C with the player.
void StartAbility(ACTOR* pa, int n)
{
    if (g_psw) BroadcastTaskEvent(g_psw, 0x3C, pa->palo);   // (taskmgr +0xC = n: not kept natively)
    if (FPrint()) std::printf("gadget: %s starts %d (power %d)\n", SzPlayer(pa), n, NActorPower(pa));
    if (SLY* p = dynamic_cast<SLY*>(pa))
    {
        switch (n)
        {
        case 26: case 37: case 38: case 41:  // orig: 0x18DA48, pocket/throw branch (0x1A / 0x25 / 0x26 / 0x29)
        {
            if (static_cast<unsigned>(p->jtthrow.state - 4) < 3)
            {
                SetJtThrowState(p, 7);
                return;
            }
            SLY::POCKET* pocket = PpocketSly(p, n);
            if (pocket->retained)
            {
                StartInteract(p, 11, pocket->retained, JTL_Max);
                return;
            }
            LO* selected = PloAcquireProxyObject(g_psw, pocket->proxy, false);
            if (!selected) return;
            CARRY* carry = PcarryFromLo(selected);
            if (!carry) return;  // authored pocket proxies contain CID64 objects
            carry->kind = pocket->id;
            // 0x224610's persistent carrier/listener path is for kinds7/11 only.
            p->jtthrow.selected = selected;
            (void)GrfAbilityButton(p, n);  // 0x141EA8 is a pure mask lookup
            SetSlyState(p, 23, -1);
            SetJtThrowState(p, 4);
            return;
        }
        case 23:  // orig: 0x18DA48 -> 0x141D38; the animation event spends power
            p->n9BC = n;
            StartInteract(p, 11, nullptr, JTL_Max);
            return;
        case 33:  // orig: 0x18DA48, Knockout Dive
            SpendActorPower(p, n);
            SetSlyState(p, 9, -1);
            return;
        case 34: case 35:  // orig: 0x18DA48; activation waits for event 0x27C
            p->n9BC = n;
            p->pasegAbility = dynamic_cast<ASEG*>(PloSlyPart(p, n == 35 ? 0x1FE0 : 0x1FE4));
            StartInteract(p, 11, nullptr, JTL_Max);
            return;
        case 24:  // Combat Dodge, original 0x18DA48
            SpendActorPower(p, n);
            SetSlyState(p, 6, -1);
            return;
        case 39:  // Lightning Spin, original 0x18DA48 (case 0x27): the charged strike's substate 9 (clip +0x1FE8)
            SpendActorPower(p, n);
            SetSlyState(p, 5, 9);
            return;
        case 25:  // Stealth Slide, original 0x18DA48 (case 0x18DC88)
            SpendActorPower(p, n);
            SetSlyState(p, 24, 12);
            return;
        case 40:  // Shadow Power, original 0x18DA48
            SpendActorPower(p, n);
            StartInteract(p, 2, nullptr, JTL_Max);
            return;
        case 0x1D:   // Thief Reflexes: the clock's factor 1 at 0.25, sound 0x67 kept (+0x2044)
        case 0x2A:   // Time Rush: 1.35, sound 0x66
        {
            SpendActorPower(p, n);
            SetClockFactor(&g_clock, 1, GElf(n == 0x1D ? 0x2BF3EC : 0x2BF3F0));
            // TODO: D_003E1BEC = -1 / 0.7 (read elsewhere: the screen's effect, not found yet)
            AbOf(p).snd2044 = CSndRef(PsndStartSound(n == 0x1D ? 0x67 : 0x66));
            return;
        }
        case 0x1F:   // Mega Jump: the burst +0x204C, the jump kind 9
            if (LO* plo = PloSlyPart(p, 0x204C)) ExplodeLo(plo, nullptr);  // 0x15C150
            SpendActorPower(p, n);
            SetJumpKind(&p->jump, 9, 0, 0);   // 0x195A00
            return;
        case 0x1E:   // Feral Pounce: the jump kind 0xE
            SpendActorPower(p, n);
            SetJumpKind(&p->jump, 0xE, 0, 0);
            return;
        case 0x1B:   // the paraglider: the jump's state 19
            SpendActorPower(p, n);
            SetJumpAnimState(&p->jump, 0x13, nullptr);  // 0x195FB8(+0x2C0, 0x13, 0)
            return;
        default:
            WarnOnce("Sly's gadget " + std::to_string(n) + " (0x18DA48) not ported");
            return;
        }
    }
    if (FIsLoDerivedFrom(pa->palo, 0x37)) { StartBentleyAbility(pa, n); return; }
    if (!StartMurrayAbility(pa, n) && FPrint()) std::printf("gadget: Murray's %d does nothing (0x141D38)\n", n);
}

// vt slot 102 (a held action ends): Sly 0x18DD78 (the time gadgets: the clock's factor 1 back to 1, the loop faded
// out), the others 0x141E28 (nothing)
void EndAbility(ACTOR* pa, int n)
{
    if (FPrint()) std::printf("gadget: %s ends %d (power %d)\n", SzPlayer(pa), n, NActorPower(pa));
    if (!dynamic_cast<SLY*>(pa) || (n != 0x1D && n != 0x2A)) return;
    SetClockFactor(&g_clock, 1, 1.0f);
    ABILITIES& ab = AbOf(pa);
    if (ab.snd2044) ab.snd2044->Stop(0.0f);   // 0x1FB098(+0x2044, 0)
    ab.snd2044 = CSndRef();                    // 0x258040(+0x2044, 0)
}

// orig 0x00141C08: the slot's state (2: started and done at once)
void SetAbilityState(ACTOR* pa, ABSLOT& e, int s)
{
    if (e.state == s) return;
    if (e.state == 1) EndAbility(pa, e.id);   // slot 102
    e.state = s;
    e.t = g_clock.t;
    if (s == 1) StartAbility(pa, e.id);       // slot 100
    else if (s == 2)
    {
        StartAbility(pa, e.id);
        e.state = 0;
    }
}
}  // namespace

bool FPowerupOwned(int id)
{
    const CRef r = RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(id)});
    return r.m_tagk == TAGK_S32 ? r.m_n != 0 : r.m_tagk == TAGK_Bool ? r.m_bool != 0 : false;
}

void StartActorAbility(ACTOR* pa, int id) { StartAbility(pa, id); }

// orig: 0x140F90 event 0x27C, cane ability branch. The caller checks the ASEGA owner.
bool ReleaseCaneAbility(ACTOR* pa)
{
    if (pa->n4C8 != 11) return false;
    if (!pa->pasegAbility)
    {
        // The original still spends the pending ability when no clip was bound.
        if (pa->n9BC != 34 && pa->n9BC != 35 && pa->n9BC != 15) return false;
        SpendActorPower(pa, pa->n9BC);
    }
    else if (pa->idAbility == pa->n9BC)
    {
        RetractAsegaPp(&pa->pasegaAbility);
        pa->idAbility = -1;
    }
    else
    {
        SpendActorPower(pa, pa->n9BC);
        ApplyAsegCur(pa->pasegAbility, pa->palo, 0.0f, 1.0f, 8, &pa->pasegaAbility);
        pa->idAbility = pa->n9BC;
    }
    pa->pasegAbility = nullptr;
    pa->n9BC = -1;
    return true;
}

// orig: 0x0019F170: the guards' freeze (Temporal Lock 14) on / off: G +0x4748 / +0x474C, then every guard (class 0x38,
// 0x15E9C8 kind 0x105) put in mode 14, or taken out of it when it is in it
namespace
{
bool s_fTimeLock = false;   // G +0x4748
float s_tTimeLock = 0.0f;   // G +0x474C
}
void SetGuardsFrozen(bool f)
{
    s_fTimeLock = f;
    s_tTimeLock = g_clock.t;
    if (!g_psw) return;
    for (const auto& up : g_psw->aploAll)
    {
        LO* plo = up.get();
        if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x38)) continue;
        GUARD* pg = PguardFromLo(plo);
        if (!pg) continue;
        if (f) SetGuardMode(pg, 0xE);  // 0x207030
        else if (pg->nMode == 0xE) SetGuardMode(pg, -1);
        if (FPrint()) std::printf("gadget: guard %d mode %d (t %.3f)\n", plo->oid, pg->nMode, g_clock.t);
    }
}
// orig: 0x0019F120 (the world update's pass after the update table): the freeze over after 6 s (D_002BF938)
void UpdateGuardsFrozen()
{
    if (s_fTimeLock && 6.0f < g_clock.t - s_tTimeLock) SetGuardsFrozen(false);
}
namespace
{
struct INITTIMELOCK
{
    INITTIMELOCK()
    {
        RegisterSwPass(0x19F120, [](SW*) { UpdateGuardsFrozen(); });
    }
} s_inittimelock;
}

// orig: 0x140F90 event 0x27C, Temporal Lock branch (case 0xE: 0x141CD0 spend, then 0x19F170(G, 1)). The caller checks
// the ASEGA owner.
bool ReleaseTimeLock(ACTOR* pa)
{
    if (pa->n4C8 != 11 || pa->pasegAbility || pa->n9BC != 14) return false;
    SpendActorPower(pa, pa->n9BC);
    SetGuardsFrozen(true);
    pa->pasegAbility = nullptr;
    pa->n9BC = -1;
    if (FPrint()) std::printf("gadget: %s Temporal Lock released t %.3f\n", SzPlayer(pa), g_clock.t);
    return true;
}

// orig: 0x140F90 event 0x27C, Guttural Roar branch (case 0x15: 0x141CD0 spend, then 0x19F220(G, 0x15): every guard
// (0x15E9C8 kind 0x105, class 0x38) put in mode 0x15 by 0x207030). The caller checks the ASEGA owner.
bool ReleaseRoar(ACTOR* pa)
{
    if (pa->n4C8 != 11 || pa->pasegAbility || pa->n9BC != 0x15) return false;
    SpendActorPower(pa, pa->n9BC);
    if (g_psw)
        for (const auto& up : g_psw->aploAll)
            if (LO* plo = up.get(); plo->fInWorld && FCidDerivedFrom(plo->Cid(), 0x38))
                if (GUARD* pg = PguardFromLo(plo)) SetGuardMode(pg, 0x15);
    pa->pasegAbility = nullptr;
    pa->n9BC = -1;
    if (FPrint()) std::printf("gadget: %s Guttural Roar released t %.3f\n", SzPlayer(pa), g_clock.t);
    return true;
}

// orig: 0x140F90 event 0x27C, Smoke Bomb branch. The caller checks the ASEGA owner.
bool ReleaseSmokeBomb(ACTOR* pa)
{
    if (pa->n4C8 != 11 || pa->pasegAbility || pa->n9BC != 23) return false;
    const int powerBefore = NActorPower(pa);
    SpendActorPower(pa, pa->n9BC);
    LO* effect = nullptr;
    if (SLY* sly = PslyGlobal())
        if (LO* proxy = PloSlyPart(sly, 0x2010))
            effect = PloAcquireProxyObject(g_psw, proxy, false);
    if (effect) SetAloParent(static_cast<ALO*>(effect), nullptr, nullptr, 3);
    const int removed = RemoveActorAlarms(pa->palo);
    SetGuardAlarmResume(g_clock.t + GElf(0x2BCC94));
    pa->pasegAbility = nullptr;
    pa->n9BC = -1;
    if (FPrint())
        std::printf("SMOKE_EVENT {\"time\":%.9g,\"power_before\":%d,\"power_after\":%d,\"effect\":%d,\"removed\":%d,\"alarm_resume\":%.9g}\n",
                    g_clock.t, powerBefore, NActorPower(pa), effect ? effect->oid : -1, removed, TGuardAlarmResume());
    return true;
}

int NActorPower(ACTOR* pa)
{
    const char* sz = SzPlayer(pa);
    if (int32_t* pn = sz ? PnProgressSlot({sz, "mpCur"}) : nullptr) return *pn;   // (+0x12FC)
    return pa->n1300;
}

void SetActorPower(ACTOR* pa, int n)
{
    const char* sz = SzPlayer(pa);
    if (int32_t* pn = sz ? PnProgressSlot({sz, "mpCur"}) : nullptr) *pn = n < pa->n1300 ? n : pa->n1300;
}

void SpendActorPower(ACTOR* pa, int id)
{
    if (id == -1) return;
    SetActorPower(pa, NActorPower(pa) - ItemPower(id));
    pa->g1304 = g_clock.t;
}

void DrainActorPower(ACTOR* pa, float g)
{
    ABILITIES& ab = AbOf(pa);
    ab.g1308 += g * g_clock.dt;
    const int n = static_cast<int>(ab.g1308);   // (cvt.w.s: toward zero)
    if (n == 0) return;
    ab.g1308 -= static_cast<float>(n);
    const int m = NActorPower(pa) - n;
    SetActorPower(pa, m > -1 ? m : 0);
}

uint32_t GrfAbilityButton(ACTOR* pa, int id)
{
    if (id == -1) return 0;
    ABILITIES& ab = AbOf(pa);
    uint32_t mask = 0;
    for (int i = 0; i < 3; ++i)
        if (ab.a[i].id == id) mask |= s_agrfSlot[i];  // orig: 0x141EA8 includes every matching loaded slot
    return mask;
}

void UpdateActorAbilities(ACTOR* pa)
{
    PADCTL* pc = PpadctlOfActor(pa);
    if (!pc) return;   // (+0x150)
    ABILITIES& ab = AbOf(pa);
    for (int i = 0; i < 3; ++i)
    {
        const uint32_t grf = s_agrfSlot[i];
        ABSLOT& e = ab.a[i];
        if (e.state == 0)
        {
            if (!GrfPadctlTakePressed(pc, grf, 0.15f)) continue;   // 0x1C46C0 (D_3E19999A)
            if (!FCanDo(pa, e.id))
            {
                if (FPrint()) std::printf("gadget: %s can't use %d now\n", SzPlayer(pa), e.id);
                PsndStartSound(0x2E);
            }
            else if (NActorPower(pa) < ItemPower(e.id))
            {
                // too little power: the refusal's sound and the meter's flash (+0x1304)
                if (FPrint()) std::printf("gadget: %s too little power for %d (%d < %d)\n", SzPlayer(pa), e.id, NActorPower(pa), ItemPower(e.id));
                PsndStartSound(0x2E);
                pa->g1304 = g_clock.t;
            }
            else SetAbilityState(pa, e, ItemUse(e.id));
        }
        else if (e.state == 1)
        {
            if (!(pc->grfbtn & grf) || !FCanDo(pa, e.id)) SetAbilityState(pa, e, 0);
            else
            {
                // vt slot 101 (0x141DD0): the time gadgets drain 2 / 1 power a second (D_002BD260 / D_002BD264)
                if (e.id == 0x1D) DrainActorPower(pa, GElf(0x2BD260));
                else if (e.id == 0x2A) DrainActorPower(pa, GElf(0x2BD264));
            }
        }
    }
}

// orig 0x00141FC8: slot i takes id (its held action ended first), another slot holding id emptied (ended), then the
// three slots published to the progress "<player> apukCur <j>"
void SetActorAbilitySlot(ACTOR* pa, int i, int id)
{
    if (static_cast<unsigned>(i) >= 3) return;
    ABILITIES& ab = AbOf(pa);
    SetAbilityState(pa, ab.a[i], 0);
    ab.a[i].id = id;
    ab.a[i].state = 0;
    ab.a[i].t = g_clock.t;
    const char* sz = SzPlayer(pa);
    for (int j = 0; j < 3; ++j)
    {
        if (j != i && ab.a[j].id == id)
        {
            SetAbilityState(pa, ab.a[j], 0);
            ab.a[j].id = -1;
        }
        if (sz)
        {
            CRef r;
            r.SetS32(ab.a[j].id);
            FSetProgress({sz, "apukCur", std::to_string(j)}, r);
        }
    }
    UpdateAbilityDisplay(pa);  // vt slot 105
}

// orig 0x0014C258 (from the transition 0x20E890 on a reload of the same world with the request's flag 0x40)
void RemoveMegaJump(SW* psw)
{
    LO* plo = psw ? PloPoCurrent(psw) : nullptr;
    ACTOR* pa = plo ? PactorFromLo(plo) : nullptr;
    if (!pa) return;
    ABILITIES& ab = AbOf(pa);
    for (int i = 0; i < 3; ++i)   // 0x141E78 (the slot of 0x1F)
        if (ab.a[i].id == 0x1F)
        {
            SetActorAbilitySlot(pa, i, -1);
            break;
        }
    CRef r;   // 0x1DAF38(0x1F, 0)
    r.SetBool(false);
    FSetProgress({"powerUps", "mppukfAcquired", "31"}, r);
}

namespace
{
struct INITRESOURCEPROPERTIES
{
    INITRESOURCEPROPERTIES()
    {
        // orig: 0x001FFD98 / 0x00200598, EOPIDs 726..729: the actor's live counters and capacities.
        RegisterObjectGetter(726, [](LO* plo) {
            CRef r;
            if (ACTOR* pa = PactorFromLo(plo))
                r.SetS32(DmgCounterOf(pa) ? DmgCapacityOf(pa) :
                    int32_t(plo->U32Field(0, 0xDFC, uint32_t(DmgCapacityOf(pa)))));  // field +0xDFC
            return r;
        });
        RegisterObjectSetter(726, [](LO* plo, const CRef& ref) {
            if (ACTOR* pa = PactorFromLo(plo))
            {
                const int n = NArg({ref}, 0);
                SetDmgCapacity(pa, n);  // field write only: changing capacity does not refill/clamp the counter
                plo->SetField(0, 0xDFC, uint32_t(n));  // preserve it through the native late actor init
            }
        });
        RegisterObjectGetter(727, [](LO* plo) {
            CRef r;
            if (ACTOR* pa = PactorFromLo(plo))
                if (int* count = DmgCounterOf(pa)) r.SetS32(*count);  // orig: 0x00258280
            return r;
        });
        RegisterObjectSetter(727, [](LO* plo, const CRef& ref) {
            if (ACTOR* pa = PactorFromLo(plo))
                if (int* count = DmgCounterOf(pa))
                    *count = std::min(NArg({ref}, 0), DmgCapacityOf(pa));  // orig: 0x00144188, upper clamp only
        });
        RegisterObjectGetter(728, [](LO* plo) {
            CRef r;
            if (ACTOR* pa = PactorFromLo(plo))
                r.SetS32(DmgCounterOf(pa) ? pa->n1300 :
                    int32_t(plo->U32Field(0, 0x1300, uint32_t(pa->n1300))));  // field +0x1300
            return r;
        });
        RegisterObjectSetter(728, [](LO* plo, const CRef& ref) {
            if (ACTOR* pa = PactorFromLo(plo))
            {
                const int n = NArg({ref}, 0);
                pa->n1300 = n;  // field write only, with no current-power change
                plo->SetField(0, 0x1300, uint32_t(n));
            }
        });
        RegisterObjectGetter(729, [](LO* plo) {
            CRef r;
            if (ACTOR* pa = PactorFromLo(plo)) r.SetS32(NActorPower(pa));  // orig: 0x001441A0
            return r;
        });
        RegisterObjectSetter(729, [](LO* plo, const CRef& ref) {
            if (ACTOR* pa = PactorFromLo(plo)) SetActorPower(pa, NArg({ref}, 0));  // orig: 0x001441C0
        });
    }
} s_initResourceProperties;
}

// (cmworld.cpp's SetCmFocusObject: the players' slot 78 0x144480 ends with vt slot 105)
void UpdateAbilityDisplayLo(LO* plo)
{
    if (ACTOR* pa = plo ? PactorFromLo(plo) : nullptr) UpdateAbilityDisplay(pa);
}
