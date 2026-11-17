// codepanel3.cpp: Sly 3's code-entry panel (class 0x50, vtable 0x356718, module 0x171108..0x1718D4) and its dials
// (class 0x79, vtable 0x356A00, 0x170DF0..0x171108), ported from the Sly 3 decomp (src/engine/panel_mode171538.c,
// state_machine_1711b8.c, panel_navigation171358.c, state_slot.c, digit_split.c, token_mode170f60.c, stable_dispatch.c,
// ready_group.c, src/game_middle/input_repeat_x.c / _y.c). No Sly 2 counterpart.
//
// A script activates the panel with an actor (SCPT 0x12, orig3 0x2271C8 -> 0x1716F8; the splice method 247): the panel
// makes itself player 0's character (0x1F4B00(SW, 0, panel): what SCPT 0x51 waits on), the pad steps left / right
// between up to six dials and turns the current one up / down through its ten digits (the dial's SM), the actor
// reaches for the current dial (interact kind 5 on the dial's part 0x73C); circle confirms (every dial at its target
// digit: mode 0, the actor is the character again; else the sound 0x3E), triangle leaves (pending).
// m_ext's prologue: panel 15342 of task 4306 (vault_power, SCPT 4307).

#include "codepanel3.h"

#include "actor.h"
#include "alo.h"
#include "cls.h"
#include "combo3.h"
#include "disc.h"
#include "game/sly3.h"
#include "interact.h"
#include "joy.h"
#include "objmethod.h"
#include "po.h"
#include "sm.h"
#include "smrun.h"
#include "sound.h"
#include "sw.h"
#include "update.h"
#include "brx.h"
#include "clock.h"
#include "so.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

void NotifyActorJumpKind(ACTOR* pa, int n);  // jump.cpp (the owner's vt slot 88)
extern SW* g_psw;
int NRandom();   // xfmath.cpp (0x11E8B8)

namespace sly3
{
namespace
{
// the dial (class 0x79)
struct DIAL3
{
    bool fInit = false;
    int mode = -1;          // +0x130  0 one down, 1 one up, 2 a random digit (each until the SM arrives), 3 at rest
    float tMode = 0.0f;     // +0x134
    SM* psm = nullptr;      // +0x138  part 0x747 (table 0x344880)
    SMA* psma = nullptr;    // +0x13C  applied at state 0x73D (the digit 0)
    int nTarget = 0;        // +0x140  the code's digit (0x171740)
    int iDigit = 0;         // +0x144  the digit shown
    LO* ploReach = nullptr; // +0x148  part 0x73C: what the actor reaches for
};

// the panel (class 0x50)
struct PANEL3
{
    bool fInit = false;
    int mode = -1;          // +0x190  0 closed (the targets set), 1 the dials scrambling, 2 entered by the player, 3 left
    float tMode = 0.0f;     // +0x194
    int stNav = -1;         // +0x198  -1 none, 0 waiting for the pad, 1 a dial turning, 2 a dial just chosen
    float tNav = 0.0f;      // +0x19C
    LO* ploActivator = nullptr;  // +0x1A0
    std::vector<LO*> aploDial;   // +0x1A4 / +0x1A8 (up to 6)
    int idial = 0;          // +0x1C4
    int fPending = 0;       // +0x1D0  (the getter 1189)
    bool fCharacter = false;     // (native: +0x130, the player index it has while it is a player's character)
    // +0x1D4 the stick-repeat record (0x1A99D0)
    int dirX = 0, dirY = 0;
    float tRepX = 0.0f, tNextX = 0.0f, tRepY = 0.0f, tNextY = 0.0f;
};

uint32_t s_serial = 0;
std::unordered_map<LO*, DIAL3> s_mpdial;
std::unordered_map<LO*, PANEL3> s_mppanel;

void CheckWorld()
{
    if (g_psw && g_psw->serial != s_serial)
    {
        s_serial = g_psw->serial;
        s_mpdial.clear();
        s_mppanel.clear();
    }
}

bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_PANELPRINT") != nullptr;
    return s_f;
}

// table 0x3210E0: {digit, the SM's state oid}
OID OidDigit(int i) { return static_cast<OID>(g_disc.U32Elf(0x3210E0 + 8u * uint32_t(i) + 4)); }

void SetDialMode(LO* plo, int mode);

// the ALO update (orig3 0x23C660 = Sly 2's 0x1F36E0)
void UpdateAlo(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
}

// orig3: 0x170DF0 (init: mode -1) / 0x170E20 (post-load: the parts of table 0x344880, the digit 0, the SM applied at
// state 0x73D: 0x23AAE8) / 0x170EA0 (the reset, vt slot 31: mode -1, then 3)
DIAL3* PdialOf(LO* plo)
{
    if (!plo) return nullptr;
    CheckWorld();
    DIAL3& d = s_mpdial[plo];
    if (!d.fInit)
    {
        d.fInit = true;
        d.ploReach = PloFindOwned3(plo, int(g_disc.U32Elf(0x344880 + 4)));       // 0x73C -> +0x148
        d.psm = dynamic_cast<SM*>(PloFindOwned3(plo, int(g_disc.U32Elf(0x34488C + 4))));  // 0x747 -> +0x138
        d.iDigit = 0;
        if (d.psm && FIsLoDerivedFrom(plo, S3CID(0x30))) ReplaceSmaHandle(d.psm, static_cast<ALO*>(plo), 0x73D, 0, &d.psma);
        d.mode = 3;
        d.tMode = g_clock.t;
    }
    return &d;
}

// orig3: 0x170F60
void SetDialMode(LO* plo, int mode)
{
    DIAL3* d = PdialOf(plo);
    if (!d || d->mode == mode) return;
    if (FPrint()) std::printf("panel3: dial %d mode %d -> %d (digit %d, target %d, sma %d)\n", plo->oidInstance, d->mode, mode, d->iDigit, d->nTarget, d->psma ? int(OidSmaCur(d->psma)) : -1);
    d->mode = mode;
    d->tMode = g_clock.t;
    switch (mode)
    {
    case 0: d->iDigit = (d->iDigit + 9) % 10; break;
    case 1: d->iDigit = (d->iDigit + 1) % 10; break;
    case 2: d->iDigit = NRandom() % 10; break;   // 0x11E8F0(0, 9)
    default: return;
    }
    if (d->psma) SetSmaGoal(d->psma, OidDigit(d->iDigit));   // 0x239CE8
}

// orig3: 0x170EE0 (the dial's update, vt slot 18: at rest once its SM shows the digit)
void UpdateDial3(LO* plo)
{
    UpdateAlo(plo);
    DIAL3* d = PdialOf(plo);
    if (d->mode >= 0 && d->mode < 3)
        if (!d->psma || OidSmaCur(d->psma) == OidDigit(d->iDigit)) SetDialMode(plo, 3);
}

// orig3: 0x171050 (the digit 0, snapped) / 0x171090 (the target digit, snapped)
void ResetDial(LO* plo, bool fSolved)
{
    DIAL3* d = PdialOf(plo);
    if (!d) return;
    d->iDigit = fSolved ? d->nTarget : 0;
    if (d->psma) SnapSmaGoal(d->psma, OidDigit(d->iDigit));   // 0x239FE0
    SetDialMode(plo, 3);
}

// orig3: 0x1710E0
bool FDialRight(LO* plo)
{
    const DIAL3* d = PdialOf(plo);
    return d && d->mode == 3 && d->nTarget == d->iDigit;
}

void SetPanelMode(LO* plo, int mode);

// orig3: 0x171108 (init: mode -1, navigation -1) / 0x171138 (vt slot 31: mode -1, then (+0x1C8 != 0), navigation -1).
// The dials: option 1464 (EOPID 1610, a list setter -> 0x171718: up to 6).
PANEL3* PpanelOf(LO* plo)
{
    if (!plo) return nullptr;
    CheckWorld();
    PANEL3& p = s_mppanel[plo];
    if (!p.fInit)
    {
        p.fInit = true;
        for (const OPT& o : plo->aopt)
            if (o.eopid == 1610)
                for (const OPTVAL& v : o.aval)
                {
                    const auto add = [&](int oid) {
                        // (the dials are instances of one object: the option names their instance oids)
                        LO* pd = nullptr;
                        for (LO* pc : plo->aploOwned)
                            if (pc->oidInstance == oid) pd = pc;
                        if (!pd) pd = PloFindOwned3(plo, oid);
                        if (!pd && plo->paloParent) pd = PloFindOwned3(plo->paloParent, oid);
                        if (!pd && g_psw) pd = PloFindSwObjectByOid(g_psw, oid);
                        if (pd && p.aploDial.size() < 6) p.aploDial.push_back(pd);
                    };
                    if (v.alist.empty()) add(v.n);
                    for (const OPTVAL& w : v.alist) add(w.n);
                }
        if (FPrint())
        {
            for (const OPT& o : plo->aopt)
                std::printf("panel3: %d opt eopid %d grf 0x%X vals %zu n %d list %zu\n", plo->oid, o.eopid, o.grf, o.aval.size(),
                            o.aval.empty() ? 0 : o.aval[0].n, o.aval.empty() ? 0 : o.aval[0].alist.size());
            for (const LO* pc : plo->aploOwned) std::printf("panel3: %d owns %d cid 0x%X\n", plo->oid, pc->oid, pc->Cid());
            if (plo->paloParent) std::printf("panel3: parent %d cid 0x%X\n", static_cast<const LO*>(plo->paloParent)->oid, static_cast<const LO*>(plo->paloParent)->Cid());
        }
        if (FPrint()) std::printf("panel3: %d: %zu dials, code %u, start %u, auto %u\n", plo->oid, p.aploDial.size(), plo->U32Field3(0, 0x1C0, 0),
                                  plo->U32Field3(0, 0x1C8, 0), plo->U32Field3(0, 0x1CC, 0));
        SetPanelMode(plo, plo->U32Field3(0, 0x1C8, 0) != 0 ? 1 : 0);
    }
    return &p;
}

// orig3: 0x154B48: the actor reaches for an object (interact kind 5: 0x1B40F0(its +0x.., 5, the object)), or ends that
void ActorReach(LO* ploActor, LO* ploReach)
{
    ACTOR* pa = ploActor ? PactorFromLo(ploActor) : nullptr;
    if (!pa) return;
    if (ploReach) SetActorInteractObject(pa, 5, ploReach);
    else if (pa->n4C8 == 5) NotifyActorJumpKind(pa, 0);
}

// orig3: 0x171670
void SetPanelNav(LO* plo, PANEL3* p, int st)
{
    if (p->stNav == st) return;
    if (p->stNav == 2 && p->idial >= 0 && p->idial < int(p->aploDial.size()))
        if (DIAL3* d = PdialOf(p->aploDial[p->idial])) ActorReach(p->ploActivator, d->ploReach);
    if (FPrint()) std::printf("panel3: %d nav %d -> %d (dial %d, t %.3f)\n", plo->oid, p->stNav, st, p->idial, g_clock.t);
    p->stNav = st;
    p->tNav = g_clock.t;
    if (st == -1)
    {
        p->idial = -1;
        ActorReach(p->ploActivator, nullptr);
    }
    (void)plo;
}

// orig3: 0x171740: the code number's digits into the dials' targets, the last dial the units
void SetPanelTargets(LO* plo, PANEL3* p)
{
    int i = int(p->aploDial.size()) - 1;
    for (int n = int(plo->U32Field3(0, 0x1C0, 0)); n > 0; n /= 10)
        if (i >= 0)
            if (DIAL3* d = PdialOf(p->aploDial[i--])) d->nTarget = n % 10;
}

// orig3: 0x171538
void SetPanelMode(LO* plo, int mode)
{
    PANEL3* p = PpanelOf(plo);
    if (!p || p->mode == mode) return;
    if (FPrint()) std::printf("panel3: %d mode %d -> %d (t %.3f)\n", plo->oid, p->mode, mode, g_clock.t);
    if (p->mode == 2)
    {
        SetPanelNav(plo, p, -1);
        // (+0x130 != -1: it is a player's character) 0x1F4B00(SW, +0x130, the activator): the activator is that
        // player's character again
        if (p->fCharacter && g_psw)
        {
            p->fCharacter = false;
            if (PloPoCurrent(g_psw) == plo) SetPoCurrent(g_psw, p->ploActivator);
        }
    }
    p->mode = mode;
    p->tMode = g_clock.t;
    switch (mode)
    {
    case 0: SetPanelTargets(plo, p); break;
    case 1:
        for (LO* pd : p->aploDial) SetDialMode(pd, 2);
        break;
    case 2:
        // 0x1F4B00(SW, 0, panel): player 0's character (0x1F4B38: the old one's vt slot 83 (-1), the panel's (0))
        if (g_psw)
        {
            SetPoCurrent(g_psw, plo);
            p->fCharacter = true;
        }
        p->dirX = p->dirY = 0;   // 0x1A99D0
        p->tNextX = p->tNextY = 0.0f;
        p->idial = 0;
        SetPanelNav(plo, p, 2);
        p->fPending = 0;
        break;
    case 3: p->fPending = 1; break;
    default: break;
    }
}

// orig3: 0x1A7A80(pad, mask, dt): the buttons pressed, taken (the game's layout: 0x10 triangle, 0x20 circle, 0x1000 /
// 0x4000 up / down, 0x8000 / 0x2000 left / right). Natively the presses of this frame (the original's 0.15 s window
// on the two buttons is not kept).
uint32_t GrfTake(uint32_t grf)
{
    const uint32_t g = g_joyps2.grfbtnPressed & grf;
    g_joyps2.grfbtnPressed &= ~g;
    return g;
}

// orig3: 0x1A99E0 / 0x1A9C38: a step of -1 / 0 / 1 from the d-pad or the stick, repeated while held (0.333 s, each
// repeat 2/3 of the last, not below 0.1 s). The stick: 0.8 to start, 0.25 to hold, the larger axis.
int NStep(bool fY, int* pdir, float* ptRep, float* ptNext)
{
    const uint32_t grfNeg = fY ? 0x1000 : 0x8000, grfPos = fY ? 0x4000 : 0x2000;
    const float gMain = fY ? g_joyps2.y : g_joyps2.x, gOther = fY ? g_joyps2.x : g_joyps2.y;
    // (x: left -1; y: up (the stick's y above 0) -1)
    const bool fStickNeg = fY ? gMain > 0.0f : gMain < 0.0f;
    const bool fMain = std::fabs(gMain) > std::fabs(gOther);
    if (!*pdir)
    {
        const uint32_t grf = GrfTake(grfNeg | grfPos);
        if (grf & grfNeg) *pdir = -1;
        else if (grf & grfPos) *pdir = 1;
        else if (g_joyps2.uDeflect >= 0.8f && fMain) *pdir = fStickNeg ? -1 : 1;
        if (*pdir)
        {
            *ptRep = 0.3333f;
            *ptNext = g_clock.tReal + *ptRep;
        }
    }
    else
    {
        bool fHeld = false;
        if (g_joyps2.grfbtn & (*pdir < 0 ? grfNeg : grfPos)) fHeld = true;
        else if (g_joyps2.uDeflect >= 0.25f && fMain && fStickNeg == (*pdir < 0) && gMain != 0.0f) fHeld = true;
        if (!fHeld) *pdir = 0;
        else if (*ptNext <= g_clock.tReal)
        {
            *ptRep = std::clamp(2.0f * *ptRep / 3.0f, 0.1f, 0.3333f);
            *ptNext = g_clock.tReal + *ptRep;
        }
        else return 0;
    }
    return *pdir;
}

// orig3: 0x171868
bool FPanelRight(PANEL3* p)
{
    for (LO* pd : p->aploDial)
        if (!FDialRight(pd)) return false;
    return true;
}

// orig3: 0x171358
void UpdatePanelNav(LO* plo, PANEL3* p)
{
    for (int guard = 0; guard < 8; ++guard)
    {
        const int st = p->stNav;
        switch (st)
        {
        case 0:
        {
            const int dx = NStep(false, &p->dirX, &p->tRepX, &p->tNextX);
            const int dy = NStep(true, &p->dirY, &p->tRepY, &p->tNextY);
            if ((dx < 0 && p->idial <= 0) || (dx > 0 && p->idial >= int(p->aploDial.size()) - 1))
            {
                PsndStartSound(0x36);
                break;
            }
            if (dx < 0)
            {
                --p->idial;
                SetPanelNav(plo, p, 2);
            }
            else if (dx > 0)
            {
                ++p->idial;
                SetPanelNav(plo, p, 2);
            }
            else if (dy != 0 && p->idial >= 0 && p->idial < int(p->aploDial.size()))
            {
                SetPanelNav(plo, p, 1);
                SetDialMode(p->aploDial[p->idial], dy > 0 ? 0 : 1);
            }
            break;
        }
        case 2: SetPanelNav(plo, p, 0); break;
        case 1:
            if (p->idial < 0 || p->idial >= int(p->aploDial.size()) || PdialOf(p->aploDial[p->idial])->mode == 3) SetPanelNav(plo, p, 0);
            break;
        default: break;
        }
        if (p->stNav == st) break;
    }
}

// orig3: 0x1711B8 (the panel's update, vt slot 18)
void UpdatePanel3(LO* plo)
{
    UpdateAlo(plo);   // 0x23C660
    PANEL3* p = PpanelOf(plo);
    for (int guard = 0; guard < 8; ++guard)
    {
        const int mode = p->mode;
        switch (mode)
        {
        case 1:
        {
            bool fAll = true;
            for (LO* pd : p->aploDial)
                if (PdialOf(pd)->mode == 2) fAll = false;
            if (fAll) SetPanelMode(plo, 0);
            break;
        }
        case 3: SetPanelMode(plo, plo->U32Field3(0, 0x1C8, 0) != 0 ? 1 : 0); break;
        case 2:
            if (GrfTake(0x10)) SetPanelMode(plo, 3);
            else
            {
                const bool fAuto = plo->U32Field3(0, 0x1CC, 0) != 0;
                const bool fConfirm = GrfTake(0x20) != 0;
                if (fConfirm && !fAuto)
                {
                    if (FPanelRight(p)) SetPanelMode(plo, 0);
                    else PsndStartSound(0x3E);
                }
                else if ((fConfirm || fAuto) && FPanelRight(p)) SetPanelMode(plo, 0);
                else UpdatePanelNav(plo, p);
            }
            break;
        default: break;
        }
        if (p->mode == mode) break;
    }
}

struct INITPANEL3
{
    INITPANEL3()
    {
        RegisterLoUpdate(FN3_TAG | 0x170EE0, UpdateDial3);
        RegisterLoUpdate(FN3_TAG | 0x1711B8, UpdatePanel3);
        // the splice methods: 247 (0x10FA80 -> 0x1716F8: activated by an object), 980 (0x10FB08 -> 0x1717A8: every dial
        // back to 0), 1465 (0x10FB80 -> 0x171808: every dial at its target); the getter 1189 (EOPID 1609, +0x1D0)
        RegisterObjectMethod3(0x10FA80, [](LO* plo, const std::vector<CRef>& a) {
            ActivateCodePanel3(plo, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x10FB08, [](LO* plo, const std::vector<CRef>&) {
            if (PANEL3* p = PpanelOf(plo))
                for (LO* pd : p->aploDial) ResetDial(pd, false);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x10FB80, [](LO* plo, const std::vector<CRef>&) {
            if (PANEL3* p = PpanelOf(plo))
                for (LO* pd : p->aploDial) ResetDial(pd, true);
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter3(1609, [](LO* plo) {
            CRef r;
            const PANEL3* p = PpanelOf(plo);
            r.SetBool(p && p->fPending != 0);
            return r;
        });
    }
} s_initpanel3;
}  // namespace

// orig3: 0x1716F8
void ActivateCodePanel3(LO* ploPanel, LO* ploActivator)
{
    PANEL3* p = PpanelOf(ploPanel);
    if (!p) return;
    p->ploActivator = ploActivator;
    SetPanelMode(ploPanel, 2);
}

// The world start: the panels and dials take their update (the dial's post-load 0x170E20 sets its +0x3C bit 2; the
// panel's comes with its SO base 0x1F3B50) and their start state.
void StartCodePanels3(SW* psw)
{
    if (!psw) return;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (plo->Cid() != 0x50 && plo->Cid() != 0x79) continue;
        if (plo->iupd >= 0) psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
        else plo->grfUpd |= GRFUPD_Update;
        if (plo->Cid() == 0x50) PpanelOf(plo);
        else PdialOf(plo);
    }
}

// orig3: 0x171188 (the panel 0x50's vt slot 32): SO's release, then 0x171538(p, -1): its mode -1 (a panel in use
// gives the pad back to its activator first)
void ReleaseCodePanel3(LO* plo) { SetPanelMode(plo, -1); }

bool FCodePanelCharacter3(const LO* plo)
{
    const auto it = s_mppanel.find(const_cast<LO*>(plo));
    return it != s_mppanel.end() && it->second.fCharacter;
}
}  // namespace sly3
