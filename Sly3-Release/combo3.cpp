// combo3.cpp: Sly 3's combo system (the cane), ported from the Sly 3 asm (combo3.h).
//
// The pieces (Sly 3 addresses):
// - the combo definitions, class 0x94 (0x110 bytes, init 0x139740, post-load 0x139848): LOs owned by the actor, their
//   fields set by options 1657..1683 (descriptors at 0x32CA80): +0x40 the condition mask (setter 0x139A98: normalized
//   by 0x139A40), +0x44 the button pressed (within +0x70, default 0.35 s), +0x48 / +0x4C buttons held / not held, +0x50
//   the cancel mask, +0x54 / +0x58 / +0x5C the cancel buttons, +0x60 / +0x64 (default 1), +0x6C the priority, +0x74
//   the steered speed (default 800), +0x7C the jump animation 0x12 at the start, +0x80, +0x84 the clip (standing),
//   +0xA4 the clip while moving (an overlay: the combo's state 1), +0xC4 the follow-up clip (a contact: state 2), +0xF0
//   a button kept taken, +0xF4 / +0xF8 the abilities one of which must be owned (0x21F410); option 1657 adds it to the
//   actor's list (0x1399F0), 1658 takes it out (0x1399A0). Sly's +0x1F44 / +0x1F48 (parts 249 / 250): the combos his
//   abilities 31 / 25 start (0x1AF6C0 -> 0x13A678).
// - the actor's combo runner at +0x10D0 (init 0x139B70 / 0x139B98): +0 the actor, +4 the state (-1 off, 0 the attack
//   (the actor's generic state 9 = Sly's state 23), 1 the overlay while moving, 2 the follow-up (state 23), 3 the
//   recovery), +8 its time, +0xC the definition, +0x10 the target, +0x14 the blend's value, +0x18 the ASEGA, +0x1C the
//   list (link +0xFC), +0x28 the window to chain (event 0x2A2), +0x2C / +0x30 the steering (0x2A0 / 0x2A1), +0x34
//   (0x2A4 on, 0x2A3 off).
// - the update 0x13A178 (Sly: 0x139FE0, vt +0x314, from UpdateActor 0x152A48 before the damage), the start 0x13A678,
//   the state 0x13ABC8, the end 0x13AB30 (to the clip's event 0x2A8, else off), the messages 0x13A770 (the actor's
//   handler 0x153738 passes them all on), the actor's condition mask 0x139E80 / 0x139BB0 (vt +0x34C).
#include <algorithm>
#include "combo3.h"

#include "game/sly3.h"
#include "game/guru3.h"
#include "game/task3.h"
#include "jump.h"
#include "jt.h"

#include "actanim.h"
#include "actor.h"
#include "aseg.h"
#include "asega.h"
#include "disc.h"
#include "lo.h"
#include "brx.h"
#include "clock.h"
#include "guard.h"
#include "po.h"
#include "savedata.h"
#include "sly.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "vecmat.h"
#include "activate.h"
#include "ainoise.h"
#include "alo.h"
#include "stepphys.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

namespace sly3
{
namespace
{
struct EOPD3 { int k = 0; uint32_t grf = 0, c18 = 0, pfn14 = 0, pfn1C = 0, pfnGet = 0; };
EOPD3 Eopd3(int eopid)
{
    const uint32_t va = 0x32CA80 + uint32_t(eopid) * 0x28;
    EOPD3 d;
    d.k = int(g_disc.U32Elf(va));
    d.grf = g_disc.U32Elf(va + 4);
    d.pfn14 = g_disc.U32Elf(va + 0x14);
    d.c18 = g_disc.U32Elf(va + 0x18);
    d.pfn1C = g_disc.U32Elf(va + 0x1C);
    d.pfnGet = g_disc.U32Elf(va + 0x24);
    return d;
}
}  // namespace

LO* PloFindOwned3(LO* p, int oid)
{
    if (!p) return nullptr;
    for (LO* pc : p->aploOwned)
    {
        if (pc->oid == oid) return pc;
        if (LO* q = PloFindOwned3(pc, oid)) return q;
    }
    return nullptr;
}

void DumpCombos3(SW* psw)
{
    if (!std::getenv("SLY3_COMBODBG") || !psw) return;
    for (LO* plo : psw->mpcidaplo[0x94])
    {
        std::printf("combo3: oid %d parent %d, %zu options\n", plo->oid, plo->paloParent ? plo->paloParent->oid : -1, plo->aopt.size());
        for (const OPT& o : plo->aopt)
        {
            const EOPD3 d = Eopd3(o.eopid);
            std::printf("  eopid %d k %d grf 0x%X off 0x%X set 0x%X/0x%X get 0x%X:", o.eopid, d.k, d.grf, d.c18, d.pfn14, d.pfn1C, d.pfnGet);
            for (const OPTVAL& v : o.aval)
            {
                std::printf(" [k%d n %d g %.3f %.3f %.3f", v.k, v.n, v.ag[0], v.ag[1], v.ag[2]);
                for (const OPTVAL& w : v.alist) std::printf(" {k%d n %d g %.3f}", w.k, w.n, w.ag[0]);
                std::printf("]");
            }
            std::printf("\n");
            if (d.k == 8322 && !o.aval.empty())
                if (LO* pc = PloFindOwned3(plo->paloParent, o.aval[0].n))
                    if (ASEGD* pd = PasegdOf(static_cast<ASEG*>(pc)))
                    {
                        std::printf("    clip %d: %.3f s, events:", pc->oid, pd->tMax);
                        for (const auto& aev : pd->aaev)
                            for (const ASEGEV& ev : aev) std::printf(" %.3f:%X/%X,%X", ev.t, ev.kind, ev.an[0], ev.an[1]);
                        std::printf("\n");
                        for (const OPT& oc : pc->aopt)
                        {
                            const EOPD3 dc = Eopd3(oc.eopid);
                            std::printf("    clip option %d k %d grf 0x%X off 0x%X: %d %.3f\n", oc.eopid, dc.k, dc.grf, dc.c18,
                                        oc.aval.empty() ? 0 : oc.aval[0].n, oc.aval.empty() ? 0.0f : oc.aval[0].ag[0]);
                        }
                    }
        }
    }
}

// ---- the combo system ----
namespace
{
constexpr uint32_t BTN_SQUARE3 = 0x80;

// orig3 0x139A40: a mask's empty groups mean "any"
uint32_t GrfNormalizeCombo(uint32_t m)
{
    if (!(m & 0x3)) m |= 0x3;
    if (!(m & 0xC)) m |= 0xC;
    if (!(m & 0xF0)) m |= 0xF0;
    if (!(m & 0x1F00)) m |= 0x1F00;
    if (!(m & 0x30000)) m |= 0x30000;
    if (!(m & 0x300000)) m |= 0x300000;
    return m;
}

struct CMBDEF3
{
    LO* plo = nullptr;
    uint32_t grfMask = GrfNormalizeCombo(0);  // +0x40 (init -1 -> the setter's normalization; unset: any)
    uint32_t btnPress = 0;      // +0x44
    uint32_t grfHeld = 0;       // +0x48
    uint32_t grfNotHeld = 0;    // +0x4C
    uint32_t grfCancel = 0;     // +0x50
    uint32_t btnCancel = 0;     // +0x54
    uint32_t grfCancelHeld = 0, grfCancelNotHeld = 0;  // +0x58 / +0x5C
    float g60 = 1.0f, g64 = 1.0f;  // +0x60 / +0x64
    int nPrio = 0;              // +0x6C
    float dtPress = 0.35f;      // +0x70 (0x3EB33333)
    float sSteer = 800.0f;      // +0x74
    int fNoisy = 0;             // +0x78 (option 1669: the actor is noisy while it runs, orig3 0x1530F8)
    int fJumpAnim = 0;          // +0x7C
    float g80 = 0.0f;           // +0x80
    ASEG* pclip = nullptr;      // +0x84
    ASEG* pclipMove = nullptr;  // +0xA4
    ASEG* pclipFollow = nullptr;  // +0xC4
    uint32_t btnKeep = 0;       // +0xF0
    std::vector<int> aability; // +0xF4 / +0xF8
};

struct CMBR3
{
    ACTOR* p = nullptr;   // the actor (orig3 runner +0)
    SLY* psly = nullptr;  // (natively: Sly's copied Sly 2 states; null for the Guru, whose states are Sly 3's)
    int state = -1;
    float tState = 0.0f;
    CMBDEF3* pdef = nullptr;
    LO* ptarget = nullptr;
    float gBlend = 0.5f;
    ASEGA* pasega = nullptr;
    int fWindow = 0, fSteer = 0, fSteerFull = 0, f34 = 0;
    std::vector<std::unique_ptr<CMBDEF3>> adef;  // every definition the actor owns
    std::vector<CMBDEF3*> alist;                 // the list (+0x1C; options 1657 prepend)
    CMBDEF3* pdefAbility31 = nullptr;            // +0x1F44
    CMBDEF3* pdefAbility25 = nullptr;            // +0x1F48
    bool fAttack = false;                        // the actor's generic state 9 (Sly's state 23)
    SW* psw = nullptr;
};

std::unordered_map<ACTOR*, std::unique_ptr<CMBR3>> s_mpcmbr;

bool FComboDbg()
{
    static const bool s_f = std::getenv("SLY3_COMBODBG") != nullptr;
    return s_f;
}

// orig3 0x21F410: an ability owned (ids below 0x43). TODO(player): the abilities' owned state (the progress); the
// prologue's Sly owns none of the combo upgrades 41..49 the definitions ask for. SLY3_ABILITIES=a,b,... lists owned ones.
bool FAbilityOwned3(int id)
{
    static std::vector<int> s_a = [] {
        std::vector<int> a;
        if (const char* sz = std::getenv("SLY3_ABILITIES"))
            for (const char* q = sz; *q;)
            {
                a.push_back(std::atoi(q));
                while (*q && *q != ',') ++q;
                if (*q == ',') ++q;
            }
        return a;
    }();
    for (int n : s_a)
        if (n == id) return true;
    // (s3-decomp) the progress: SW +0x4088's 0x43 keys are the power-ups' "powerUps mppukfAcquired <id>" (Sly 3's
    // ids; written by 0x21F510 as a Bool, read by 0x2BC010); game/items3.h. (0x21F410's other branch, the current
    // mission object's own id, is not ported.) SLY3_GADGETS owns them all (gadget.cpp).
    if (id < 0 || id >= 0x43) return false;
    const CRef r = RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(id)});
    return r.m_tagk == TAGK_Bool ? r.m_bool : r.m_tagk == TAGK_S32 ? r.m_n != 0 : false;
}

int State3OfSly(const SLY* p) { return p->jts; }   // (Sly's states are Sly 3's: sly.h jts::)

// orig3 0x1AF200 (vt +0x324): the actor's generic state
int NGeneric3(const CMBR3& c)
{
    if (!c.psly) return ActorJumpKind(c.p);   // (the Guru: his vt +0x324 = slot 100, 0x22CFA8: the jump kind of his state)
    switch (State3OfSly(c.psly))
    {
    case -1: return -1;
    case 0: return 0;
    case 3: return 1;
    case 5: return 3;
    case 6: return 10;
    case 8: return 2;
    case 9: return 8;
    case 14: return 4;
    case 15: return 5;
    case 16: return 6;
    case 18: return 7;
    case 23: return 9;
    default: return 11;
    }
}

// orig3 0x1AF150 (vt +0x31C) for the two generic states the combos set: 9 (Sly's 23: the attack) and 0 (standing)
void SetGeneric3(CMBR3& c, int g)
{
    if (!c.psly)
    {
        // (the Guru: vt +0x31C = slot 99, 0x22CF00: kind 9 -> his state 7, 0 -> 0)
        c.fAttack = g == 9;
        NotifyActorJumpKind(c.p, g);
        return;
    }
    // (0x1AF150's table: kind 9 -> state 23, kind 0 -> state 0; the combos set only these two)
    SLY* p = c.psly;
    if (g == 9) SetSlyState(p, jts::Attack, -1);
    else if (g == 0) SetSlyState(p, jts::Stand, -1);
    c.fAttack = p->jts == jts::Attack;
}

// orig3 0x2503C8(actor, 0.1): off the floor for more than 0.1 s
bool FAirborne3(const ACTOR* p) { return !p->pxpFloor; }  // TODO(player): the 0.1 s (+0x21C the floor's last time)

// orig3 0x139BB0 / 0x139E80 (vt +0x34C): the conditions met now
uint32_t GrfComboConditions(CMBR3& c)
{
    ACTOR* p = c.p;
    PADCTL* pc = PpadctlOfActor3(p);
    uint32_t grf;
    const float u = pc ? pc->uJoy : 0.0f;
    if (u == 0.0f) grf = 0x1000;  // the stick at rest
    else
    {
        const float d = cmm::RadNormalize(p->radHeading - pc->radJoy);
        const float ad = std::fabs(d);
        if (ad < 0.7853982f) grf = 0x100;          // ahead (0x3F490FDB)
        else if (2.3561945f < ad) grf = 0x800;     // behind (0x4016CBE4)
        else grf = 0.0f < d ? 0x400 : 0x200;
    }
    // the side: of the target in Sly's frame, else of the stick from the heading
    LO* ptarget = p->plo10DC;
    if (ptarget)
    {
        const glm::vec3 d = PosAloCenter(static_cast<ALO*>(ptarget)) - PosAloCenter(p->palo);
        const glm::vec3 l = glm::transpose(p->matHeading) * d;
        grf |= 0.0f < l.y ? 0x4 : 0x8;
    }
    else grf |= cmm::RadNormalize((pc && u != 0.0f ? pc->radJoy : p->radHeading) - p->radHeading) < 0.0f ? 0x4 : 0x8;
    // the target's kind: none / not a guard 0x10; a guard knocked down 0x80 (0x2533C0 6 / 0x2A), aware of Sly 0x40, else
    // 0x20. TODO(player): the guard's states 6 / 0x2A and its awareness (0x170500) from the AI code's guard
    if (!ptarget || !PguardFromLo(ptarget)) grf |= 0x10;
    else grf |= 0x20;
    // the generic state: in the air 0x10000, on the ground 0x20000; 0x200000 for state 6, else 0x100000
    const int g = NGeneric3(c);
    if (g == 1) grf |= 0x10000;
    else if (g == 0 || g == 9) grf |= FAirborne3(p) ? 0x10000 : 0x20000;
    else if (g == 2) grf |= 0x20000;
    else grf |= 0x20000;
    grf |= g == 10 ? 0x200000 : 0x100000;
    // the cane's side (0x139E80: the middle of the cane +0x1AA4 and the hand (bone 0x24) in Sly's frame, y >= 0 1,
    // else 2). Natively: the idle pose holds it at 2 (the reference's first swing is 7581 / clip 316: its time runs to
    // 0.82 at the clip's speed 1.2, past 317's 0.667); a swing ends on the other side of the one it started from.
    // TODO(player): the two records' world positions
    int side = 2;
    if (c.state != -1 && c.pdef) side = (c.pdef->grfMask & 0x3) == 0x2 ? 1 : 2;
    grf |= side == 1 ? 0x1 : 0x2;
    return grf;
}

// orig3 0x1AF308(actor, 2) (vt +0x38C): may Sly attack now (states 0, 1, 2, 6, 10, 23; the jump's 0x1BD830)
bool FCanAttack3(const CMBR3& c)
{
    if (!c.psly) return FGuruCanDo3(c.p->palo, 2);   // (the Guru's vt +0x38C = slot 113, 0x22D1F8)
    if (c.fAttack) return true;
    switch (State3OfSly(c.psly))
    {
    case 0: case 1: case 2: case 3: case 6: case 10: return true;
    default: return false;
    }
}

// orig3 0x13ADE0: may a combo start (none playing, or its window open)
bool FCanStart(const CMBR3& c) { return !c.pasega || FAsegaRetracted(c.pasega) || c.fWindow; }

// orig3 0x13ABC8
void SetComboState(CMBR3& c, int n)
{
    if (n == c.state) return;
    float tStart = 0.0f;
    if (c.state == -1) c.fWindow = c.fSteer = c.fSteerFull = c.f34 = 0;
    else if (c.state >= 0 && c.state < 2 && n >= 0 && n < 2 && c.pasega) tStart = c.pasega->t;
    const int old = c.state;
    c.state = n;
    c.tState = g_clock.t;
    ASEG* pclip = nullptr;
    switch (n)
    {
    case -1:
        if (NGeneric3(c) == 9) SetGeneric3(c, 0);
        // (PC safety: a clip cut before its event 0x41 leaves no heading lock behind)
        if (c.p && c.p->palo)
            if (STEPPHYS* ps = PstepPhysOf(c.p->palo)) ps->fHeadingLock3 = 0;
        c.pdef = nullptr;
        c.ptarget = nullptr;
        break;
    case 0: SetGeneric3(c, 9); pclip = c.pdef ? c.pdef->pclip : nullptr; break;
    case 1: pclip = c.pdef ? c.pdef->pclipMove : nullptr; break;
    case 2: SetGeneric3(c, 9); pclip = c.pdef ? c.pdef->pclipFollow : nullptr; break;
    case 3: return;  // (the clip goes on)
    default: break;
    }
    // TODO(player): the blends +0x88 / +0xA8 (none of Sly's prologue definitions has one)
    if (FComboDbg())
        std::printf("combo3: f %.3f state %d -> %d def %d clip %d t %.3f\n", g_clock.t, old, n, c.pdef && c.pdef->plo ? c.pdef->plo->oid : -1,
                    pclip ? pclip->oid : -1, tStart);
    ApplyAsegCur(pclip, c.p->palo, tStart, 1.0f, 3, &c.pasega);  // 0x137588
}

// orig3 0x13A5B8: the attack, or its overlay while Sly moves
void ChooseComboState(CMBR3& c)
{
    if (!c.pdef || c.state < -1 || c.state > 1) return;
    if (!c.pdef->pclip) { SetComboState(c, 1); return; }
    if (!c.pdef->pclipMove) { SetComboState(c, 0); return; }
    const int g = NGeneric3(c);
    SetComboState(c, g != 0 && g != 9 ? 1 : 0);
}

// orig3 0x13A678
void StartCombo(CMBR3& c, CMBDEF3* pdef)
{
    // orig3 0x13A678: 0x1854C8(GAME, 0x59, def): the tasks hear of the combo (e.g. m2_shaman's 2667 closes on the
    // Guru's 34973, the disappearance; module 36)
    if (pdef && pdef->plo) Broadcast3(0x59, pdef->plo);
    SetComboState(c, -1);
    c.pdef = pdef;
    c.gBlend = 1.0f;
    // the target: the actor's (+0x11B4), else the best in front (0x1B0898; TODO(player))
    c.ptarget = c.p->plo10DC;
    // the jump's animation state 0x12 (0x1B7268). TODO(player): Sly 3's jump animation states
    SetComboState(c, -1);  // (0x13A6B0 leaves it off; the def stays)
    c.pdef = pdef;
    ChooseComboState(c);
}

// orig3 0x13AB30: to the clip's end marker 0x2A8 (the recovery, state 3), else off
void EndCombo(CMBR3& c)
{
    if (c.state < 0 || c.state > 1) return;
    if (c.pasega && c.pasega->paseg)
        if (ASEGD* pd = PasegdOf(c.pasega->paseg))
            for (const auto& aev : pd->aaev)
                for (const ASEGEV& ev : aev)
                    if (ev.kind == 0xB && ev.an[0] == 0x2A8)
                    {
                        if (c.pasega->t < ev.t) SetAsegaTime(c.pasega, 0, ev.t, c.pasega->svt);  // 0x137D08
                        SetComboState(c, 3);
                        return;
                    }
    SetComboState(c, -1);
}

CMBR3* PcmbrOf(ACTOR* p, bool fCreate)
{
    auto it = s_mpcmbr.find(p);
    if (it != s_mpcmbr.end() && it->second->psw == g_psw) return it->second.get();
    if (!fCreate || !g_psw) return nullptr;
    auto pc = std::make_unique<CMBR3>();
    pc->p = p;
    pc->psly = dynamic_cast<SLY*>(p);
    pc->psw = g_psw;
    // the definitions Sly owns, in their load order (the options' 1657 prepend them to the list)
    // (PC, split screen's teams: a copy of Sly made while the world runs has no definitions of its own (class 0x94 leaves
    // the world at its start, a copy skips removed children): its template's, their clips the template's)
    LO* ploOwner = p->palo;
    if (p->palo->ploCloneOf && std::none_of(g_psw->mpcidaplo[0x94].begin(), g_psw->mpcidaplo[0x94].end(),
                                            [&](LO* plo) { return plo->paloParent == p->palo; }))
        ploOwner = p->palo->ploCloneOf;
    for (LO* plo : g_psw->mpcidaplo[0x94])
    {
        if (plo->paloParent != ploOwner) continue;
        auto pd = std::make_unique<CMBDEF3>();
        pd->plo = plo;
        bool fList = false;
        for (const OPT& o : plo->aopt)
        {
            const OPTVAL* pv = o.aval.empty() ? nullptr : &o.aval[0];
            auto clip = [&]() -> ASEG* {
                if (!pv) return nullptr;
                LO* plo = PloFindOwned3(p->palo, pv->n);
                if (!plo && ploOwner != p->palo) plo = PloFindOwned3(ploOwner, pv->n);
                return dynamic_cast<ASEG*>(plo);
            };
            switch (o.eopid)
            {
            case 1657: fList = true; break;
            case 1658: fList = false; break;
            case 1659: if (pv) pd->btnPress = uint32_t(pv->n); break;
            case 1662: if (pv) pd->grfMask = GrfNormalizeCombo(uint32_t(pv->n)); break;
            case 1666: if (pv) pd->grfCancel = uint32_t(pv->n); break;
            case 1667: if (pv) pd->nPrio = pv->n; break;
            case 1670: if (pv) pd->fJumpAnim = pv->n; break;
            case 1671: if (pv) pd->g80 = pv->ag[0]; break;
            case 1673: pd->pclip = clip(); break;
            case 1676: pd->pclipMove = clip(); break;
            case 1679: pd->pclipFollow = clip(); break;
            case 1680: if (pv) pd->g60 = pv->ag[0]; break;
            case 1681: if (pv) pd->g64 = pv->ag[0]; break;
            case 1682: if (pv) pd->btnKeep = uint32_t(pv->n); break;
            case 1683:
                if (pv)
                    for (const OPTVAL& w : pv->alist) pd->aability.push_back(w.n);
                break;
            default:
                // (the others by their descriptor's field (0x32CA80 +0x18): e.g. the Guru's 1660 (+0x48, L1 held) and
                // 1665 (+0x5C, cancelled when L1 is let go))
                switch (Eopd3(o.eopid).c18)
                {
                case 0x44: if (pv) pd->btnPress = uint32_t(pv->n); break;
                case 0x48: if (pv) pd->grfHeld = uint32_t(pv->n); break;
                case 0x4C: if (pv) pd->grfNotHeld = uint32_t(pv->n); break;
                case 0x50: if (pv) pd->grfCancel = uint32_t(pv->n); break;
                case 0x54: if (pv) pd->btnCancel = uint32_t(pv->n); break;
                case 0x58: if (pv) pd->grfCancelHeld = uint32_t(pv->n); break;
                case 0x5C: if (pv) pd->grfCancelNotHeld = uint32_t(pv->n); break;
                case 0x6C: if (pv) pd->nPrio = pv->n; break;
                case 0x70: if (pv) pd->dtPress = pv->ag[0]; break;
                case 0x74: if (pv) pd->sSteer = pv->ag[0]; break;
                case 0x78: if (pv) pd->fNoisy = pv->n; break;
                default:
                    if (FComboDbg()) std::printf("combo3: def %d option %d not read\n", plo->oid, o.eopid);
                    break;
                }
                break;
            }
        }
        if (fList) pc->alist.insert(pc->alist.begin(), pd.get());
        pc->adef.push_back(std::move(pd));
    }
    auto bypart = [&](uint32_t ib) -> CMBDEF3* {
        for (const auto& [ibPart, plo] : p->palo->apart)
            if (ibPart == (FN3_TAG | ib))
                for (const auto& pd : pc->adef)
                    if (pd->plo == plo) return pd.get();
        return nullptr;
    };
    pc->pdefAbility31 = bypart(0x1F44);
    pc->pdefAbility25 = bypart(0x1F48);
    if (FComboDbg())
    {
        std::printf("combo3: actor %d: %zu definitions, %zu in the list\n", p->palo->oid, pc->adef.size(), pc->alist.size());
        for (const auto& pd : pc->adef)
            std::printf("combo3: def %d%s press 0x%X mask 0x%X held 0x%X notheld 0x%X cancel 0x%X/0x%X/0x%X/0x%X prio %d dt %.2f steer %.0f clips %d %d %d keep 0x%X abilities %zu\n",
                        pd->plo->oid, std::find(pc->alist.begin(), pc->alist.end(), pd.get()) != pc->alist.end() ? " (listed)" : "",
                        pd->btnPress, pd->grfMask, pd->grfHeld, pd->grfNotHeld, pd->grfCancel, pd->btnCancel, pd->grfCancelHeld,
                        pd->grfCancelNotHeld, pd->nPrio, pd->dtPress, pd->sSteer, pd->pclip ? pd->pclip->oid : -1,
                        pd->pclipMove ? pd->pclipMove->oid : -1, pd->pclipFollow ? pd->pclipFollow->oid : -1, pd->btnKeep, pd->aability.size());
    }
    CMBR3* pr = pc.get();
    s_mpcmbr[p] = std::move(pc);
    return pr;
}

// orig3 0x139928: one of its abilities owned (none listed: yes)
bool FDefAbilities(const CMBDEF3* pd)
{
    if (pd->aability.empty()) return true;
    for (int n : pd->aability)
        if (FAbilityOwned3(n)) return true;
    return false;
}
}  // namespace

// orig3 0x155CE0 (vt slot 102 of the actors, Sly 2's 91): the combo off (0x13ABC8(+0x10D0, -1)); Sly's 0x1AF120 adds
// the reach off (0x208B20(+0x18F0, -1)), the Panda King's 0x1EA178 his fireworks (game/guru3.cpp ActorSlot102Sly3)
void StopCombo3(ACTOR* p)
{
    if (CMBR3* pc = PcmbrOf(p, false)) SetComboState(*pc, -1);
}

PADCTL* PpadctlOfActor3(ACTOR* p)
{
    PODATA* ppd = p ? PpodataFromLo(p->palo) : nullptr;
    return ppd ? ppd->ppadctl : nullptr;
}
PADCTL* PpadctlOfSly3(SLY* p) { return PpadctlOfActor3(p); }

bool FCombo3Attacking(ACTOR* p)
{
    if (SLY* psly = dynamic_cast<SLY*>(p)) return psly->jts == jts::Attack;   // (Sly's state 23)
    CMBR3* c = PcmbrOf(p, false);
    return c && c->fAttack;
}

// orig3 0x1AC098 (Sly's per-frame transitions) case 23: when the combo's definition has a clip while moving
// (0x1398F0) and the stick is out (pad +0x134 +8 > 0) or cross was pressed within 0.1 s (0x40), Sly goes to state 0
// (0x1ADDD0(p, 0, -1)) and the same frame's transitions go on from there (run, jump); the combo then sees a generic
// state other than 0 / 9 and plays its overlay (state 1) from the attack's time (0x13ABC8). Natively the attack held
// Sly until its clip ended (0.7 s for the standing swing): the reference runs within 2 vblanks of the stick
// (build/ref/atk1: square at 6000, stick at 6008, state 1 / combo 1 at 6010, the combo off at 6034)
bool FLeaveCombo3Attack(SLY* p)
{
    CMBR3* pc = PcmbrOf(p, false);
    if (!pc || !pc->fAttack) return false;
    if (!pc->pdef || !pc->pdef->pclipMove) return false;
    PADCTL* ppad = PpadctlOfActor3(p);
    if (!ppad) return false;
    if (!(ppad->uJoy > 0.0f) && !GrfPadctlPressedWithin(ppad, 0x40, 0.1f)) return false;
    if (FComboDbg()) std::printf("combo3: f %.3f the attack left for state 0 (moving)\n", g_clock.t);
    return true;
}

// orig3 0x13A9E0 (Sly's motion 0x1AA630 in state 23, the Guru's in his 7): in the air for over 0.1 s (0x2503C8) the
// velocity steered toward the feet's (+0x1A0 in the world, 0x1C94E8; the floor object's +0x438 / +0x440 part: TODO,
// none) at up to 2000 / s^2 (0x1205F0), its height kept; with the runner's +0x34 (event 0x2A2) gravity cancelled
void UpdateCombo3Motion(ACTOR* p)
{
    if (!p || !p->palo || p->pxpFloor || !(0.1f < g_clock.t - p->tFloor)) return;
    CMBR3* pc = PcmbrOf(p, false);
    const glm::vec3 v = VelAloPos(p->palo);
    glm::vec3 c = -VecJtConvert(p->palo, nullptr, p->vecVel);   // (+0x1A0 is the feet's velocity reversed)
    if (pc && pc->f34)
    {
        if (SOST* ps = Psost(p->palo)) AccelAloPos(p->palo, -glm::vec3(ps->dvGravity));
    }
    else c.z = v.z;
    if (g_clock.dt > 0.0f) AccelAloPos(p->palo, cmm::LimitVectorLength((c - v) * (1.0f / g_clock.dt), 2000.0f));
}

int NCombo3State(ACTOR* p)
{
    CMBR3* c = PcmbrOf(p, false);
    return c ? c->state : -1;
}

// orig3 0x156388's combo branch: the runner in its state 0 or 2 (+0x10D4) on a definition whose cancel mask (+0x50) has
// 0x20000 (the actor's contact kind 1 then)
bool FCombo3StanceAir(ACTOR* p)
{
    CMBR3* c = PcmbrOf(p, false);
    return c && (c->state == 0 || c->state == 2) && c->pdef && (c->pdef->grfCancel & 0x20000u);
}

// orig3 0x1530F8's second part: the runner runs (+0x10D4 != -1) a definition (+0x10DC) whose +0x78 is set
bool FCombo3Noisy(ACTOR* p)
{
    CMBR3* c = PcmbrOf(p, false);
    return c && c->state != -1 && c->pdef && c->pdef->fNoisy;
}

// orig3 0x139FE0 (Sly's vt +0x314) -> 0x13A178
void UpdateCombo3(ACTOR* p)
{
    if (!p || !p->palo) return;
    CMBR3* pc = PcmbrOf(p, true);
    if (!pc) return;
    CMBR3& c = *pc;
    PADCTL* ppad = PpadctlOfActor3(p);
    if (c.psly) c.fAttack = c.psly->jts == jts::Attack;   // (state 23 left by another state change: NGeneric3)
    // TODO(player): 0x139FE0's takedown: a guard target within 400 whose job 0xDA is Sly's: square -> state 13
    const uint32_t grf = GrfNormalizeCombo(GrfComboConditions(c));
    const uint32_t grfHeld = ppad ? ppad->grfbtn : 0;
    if (ppad && FCanStart(c) && FCanAttack3(c))
    {
        CMBDEF3* pbest = nullptr;
        int nBest = int(0x80000001);
        uint32_t btnBest = 0;
        for (CMBDEF3* pd : c.alist)
        {
            if ((grf & pd->grfMask) != grf) continue;
            if (pd->nPrio < nBest) continue;
            if (pd->grfHeld && !(grfHeld & pd->grfHeld)) continue;
            if (pd->grfNotHeld && (~grfHeld & pd->grfNotHeld) == 0) continue;
            uint32_t btn = 0;
            if (pd->btnPress)
            {
                btn = GrfPadctlPressedWithin(ppad, pd->btnPress, pd->dtPress);
                if (btn != pd->btnPress) continue;
            }
            if (!FDefAbilities(pd)) continue;
            nBest = pd->nPrio;
            btnBest = btn;
            pbest = pd;
        }
        if (pbest)
        {
            if (btnBest) GrfPadctlTakePressed(ppad, btnBest, pbest->dtPress);
            StartCombo(c, pbest);
        }
    }
    ChooseComboState(c);
    if (c.pdef)
    {
        if (ppad && c.pdef->btnKeep) GrfPadctlTakePressed(ppad, c.pdef->btnKeep, 0.0f);
        if (c.state == 0 || c.state == 1)
        {
            const CMBDEF3* pd = c.pdef;
            const bool fCancel = (grf & pd->grfCancel) || (grfHeld & pd->grfCancelHeld) || (~grfHeld & pd->grfCancelNotHeld) ||
                                 (ppad && pd->btnCancel && GrfPadctlPressedWithin(ppad, pd->btnCancel, pd->dtPress));
            if (fCancel) EndCombo(c);
            else if (c.pasega && FAsegaRetracted(c.pasega)) EndCombo(c);
        }
        else if (c.pasega && FAsegaRetracted(c.pasega)) EndCombo(c);
    }
    else if (c.state != -1 && c.pasega && FAsegaRetracted(c.pasega)) SetComboState(c, -1);
    if (c.state == 3 && c.pasega && FAsegaRetracted(c.pasega)) SetComboState(c, -1);
    // facing the target in the attack (unless Sly is +0x11C4 4: TODO)
    if (c.state == 0 && c.ptarget)  // 0x24E920 (FaceObject: the step's heading toward it)
        SetStepPhysHeadingToward(p->palo, PosJtLo(c.ptarget) - PosAloCenter(p->palo));
    // the attack's motion: its clip's root velocity (0x24F278), steered by the stick from the events 0x2A0 / 0x2A1
    if ((c.state == 0 || c.state == 2) && c.pasega && !FAsegaRetracted(c.pasega))
    {
        glm::vec4 v(0.0f);
        if (const ASEGCHN* pchn = PchnFindAseg(c.pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
            pchn->ptrkp->Eval(nullptr, &v, c.pasega->t, c.pasega->svt);
        p->vecVel = -glm::vec3(v);
        if ((c.fSteer || c.fSteerFull) && ppad && c.pdef)
        {
            // TODO(player): 0x13A468's ramp between the events 0x2A0 and 0x2A2 by the curve 0x31FA30 (clamped
            // by 0x341FF0); natively linear
            float u = 1.0f;
            if (!c.fSteerFull)
            {
                float t0 = 0.0f, t1 = 0.0f;
                if (ASEGD* pd = PasegdOf(c.pasega->paseg))
                    for (const auto& aev : pd->aaev)
                        for (const ASEGEV& ev : aev)
                            if (ev.kind == 0xB && ev.an[0] == 0x2A0) t0 = ev.t;
                            else if (ev.kind == 0xB && ev.an[0] == 0x2A2) t1 = ev.t;
                u = t1 > t0 ? glm::clamp((c.pasega->t - t0) / (t1 - t0), 0.0f, 1.0f) : 1.0f;
            }
            const float s = ppad->uJoy * c.pdef->sSteer;
            const glm::vec3 w(std::cos(ppad->radJoy) * s, std::sin(ppad->radJoy) * s, 0.0f);
            const glm::vec3 l = glm::transpose(p->matHeading) * w;
            p->vecVel = p->vecVel * (1.0f - u) + l * u;
        }
    }
}

// orig3 0x13A770 (Sly 3's message 10, Sly 2's 9: an event of an animation)
void HandleCombo3Event(ACTOR* p, const ASEGMSG9* pmsg)
{
    CMBR3* pc = PcmbrOf(p, false);
    if (!pc || !pmsg || pmsg->pasega != pc->pasega || !pmsg->pev) return;
    // (Sly 3 takes the id of any event kind; natively the runner's own ids keep the kind-0xB test, the actor ones below
    // (0x40 / 0x41 / 0xFB) come from any kind)
    if (pmsg->pev->kind != 0xB && pmsg->n != 0x40 && pmsg->n != 0x41 && pmsg->n != 0xFB) return;
    CMBR3& c = *pc;
    switch (pmsg->n)
    {
    case 0x2A0: c.fSteer = 1; break;
    case 0x2A1: c.fSteerFull = 1; break;
    case 0x2A2: c.fWindow = 1; break;
    case 0x2A3: c.f34 = 0; break;
    case 0x2A4: c.f34 = 1; break;
    // 0x18C..0x18F (state 1, the overlay while moving): the clip lets go of the root and the legs one group after the
    // other so the run / walk animation under it shows there (0x138C70 on Sly's record +0x1190 = tag 0x159 kind 1
    // (0x1AA2B0); 0x138CC8 with the pairs at 0x31FA40..: tags 0x30 / 0x2B / 0x2D, 0x31 / 0x2A / 0x2C, 0x38 / 0x39).
    // Natively the overlay held the whole body: Sly slid along in the standing swing's pose
    case 0x18C:
        if (c.state == 1 && c.pasega)
            for (const auto& up : p->palo->axf)
                if (up->tag == 0x159 && up->kind == 1)
                {
                    DemoteAsegaOnXf(c.pasega, p->palo, up.get());
                    break;
                }
        break;
    case 0x18D: if (c.state == 1 && c.pasega) DemoteAsegaOnAlo(c.pasega, p->palo, {{0x30, -1}, {0x2B, -1}, {0x2D, -1}}); break;
    case 0x18E: if (c.state == 1 && c.pasega) DemoteAsegaOnAlo(c.pasega, p->palo, {{0x31, -1}, {0x2A, -1}, {0x2C, -1}}); break;
    case 0x18F: if (c.state == 1 && c.pasega) DemoteAsegaOnAlo(c.pasega, p->palo, {{0x38, -1}, {0x39, -1}}); break;
    // (s3fix-ai2 / ai3) 0x40 / 0x41: the actor's heading lock (STEP +0x1B4: SetStepHeading 0x24E878 does nothing while
    // set; stepphys.cpp SetStepPhysHeading: every Sly 3 heading write is a 0x24E878 call, the PC's own placements are not)
    case 0x40: if (STEPPHYS* ps = PstepPhysOf(p->palo)) ps->fHeadingLock3 = 1; break;
    case 0x41: if (STEPPHYS* ps = PstepPhysOf(p->palo)) ps->fHeadingLock3 = 0; break;
    // (s3fix-ai2) 0xFB: 0x1B0650: the smoke of Sly's part +0x1EC0 (oid 629: 0x1FBE30 acquires its proxy's object,
    // 0x1C7A00(object, 0, 3) puts it in the world at the top), the alarms (noise kind 4) on the actor dropped, and the
    // guards' alarms held 2 s (D_0047A3CC): as the Smoke Bomb's release (gadget.cpp) with its own part and time
    case 0xFB:
    {
        if (SLY* psly = PslyGlobal(); psly && psly->palo)
            for (const auto& [ib, proxy] : psly->palo->apart)
                if (ib == (FN3_TAG | 0x1EC0) && proxy)
                {
                    if (LO* peffect = PloAcquireProxyObject(g_psw, proxy, false))
                        SetAloParent(static_cast<ALO*>(peffect), nullptr, nullptr, 3);
                    break;
                }
        const int c = RemoveActorAlarms(p->palo);
        SetGuardAlarmResume(g_clock.t + 2.0f);
        if (FComboDbg()) std::printf("combo3: f %.3f smoke: %d alarms dropped\n", g_clock.t, c);
        break;
    }
    default: break;
    }
    if (FComboDbg()) std::printf("combo3: f %.3f event 0x%X\n", g_clock.t, pmsg->n);
}

// orig3 0x13A770 message 12 (Sly 2's 11: a contact accepted): the follow-up clip, when the contact's accepted bytes
// (the hit record's +0x50) have bit 0. (s3-ai round 20: the test was missing; the boot intro's combo 7561 on the
// elephant (accepted bytes without it) went to its follow clip 8244 at the first contact and never hit again, where
// the reference stays in the clip 242 and hits it once more a frame after SCPT [115]'s commands ended the first launch)
void HandleCombo3Contact(ACTOR* p, const CTMSG* pctm)
{
    CMBR3* pc = PcmbrOf(p, false);
    if (pctm && !(pctm->grfAccepted & 1)) return;
    if (pc && pc->state != -1 && pc->pdef && pc->pdef->pclipFollow) SetComboState(*pc, 2);
}

// The ASEG events 25 / 26 (orig3 0x138270 -> 0x1399F0 / 0x1399A0): the actor's combo definition an[0] put first in
// its list (+0x10EC) / taken out of it (e.g. the Guru's clip 34997: 34975 out at its start, 34973 in at 2.1 s)
void SetCombo3Listed(ACTOR* p, int oidDef, bool fIn)
{
    CMBR3* pc = PcmbrOf(p, true);
    if (!pc) return;
    CMBDEF3* pd = nullptr;
    for (const auto& up : pc->adef)
        if (up->plo && up->plo->oid == oidDef) pd = up.get();
    if (!pd) return;
    auto it = std::find(pc->alist.begin(), pc->alist.end(), pd);
    if (it != pc->alist.end()) pc->alist.erase(it);
    if (fIn) pc->alist.insert(pc->alist.begin(), pd);
    if (FComboDbg()) std::printf("combo3: f %.3f def %d %s the list\n", g_clock.t, oidDef, fIn ? "into" : "out of");
}

// orig3 0x1398F0 on the runner's definition (+0x10DC): its clip while moving (+0xA4 / +0xA8): the actor may move
bool FCombo3DefMoves(ACTOR* p)
{
    CMBR3* pc = PcmbrOf(p, false);
    return pc && pc->pdef && pc->pdef->pclipMove;
}

// orig3 0x1AF308's cases 31 / 25 (Sly's can-do): the combo definition exists (+0x1F44 / +0x1F48) and a combo may start
// (0x13ADE0). (Smoke Bomb's other test, its proxy +0x1EC0 with a free row (0x1FBEA0), is not checked.)
bool FAbilityCombo3Ready(SLY* p, int ability)
{
    CMBR3* pc = PcmbrOf(p, true);
    if (!pc) return false;
    CMBDEF3* pd = ability == 31 ? pc->pdefAbility31 : ability == 25 ? pc->pdefAbility25 : nullptr;
    return pd && FCanStart(*pc);
}

// SCPT 0x28 (orig3 0x226D00 case 40: 0x13A678(actor +0x10D0, the definition, f)): the actor's combo definition started
// by the script; f (the command's object, not null) keeps the target unset (no target search); the done test (0x229438
// case 40) waits for the runner's state -1 (+0x10D4). E.g. the boot intro's SCPT 4262 [100]: Sly's 7561 at the elephant.
bool StartScriptCombo3(ACTOR* p, LO* ploDef, bool fNoTarget)
{
    CMBR3* pc = PcmbrOf(p, true);
    if (!pc || !ploDef) return false;
    CMBDEF3* pd = nullptr;
    for (const auto& up : pc->adef)
        if (up->plo == ploDef) pd = up.get();
    if (FComboDbg()) std::printf("combo3: f %.3f script combo %d (%s) no target %d\n", g_clock.t, ploDef->oid, pd ? "found" : "not a definition", int(fNoTarget));
    if (!pd) return false;
    Broadcast3(0x59, pd->plo);
    SetComboState(*pc, -1);
    pc->pdef = pd;
    pc->gBlend = 1.0f;
    pc->ptarget = fNoTarget ? nullptr : p->plo10DC;
    ChooseComboState(*pc);
    return true;
}

// The abilities 31 / 25 (orig3 0x1AF6C0): their combos
void StartAbilityCombo3(SLY* p, int ability)
{
    CMBR3* pc = PcmbrOf(p, true);
    if (!pc) return;
    CMBDEF3* pd = ability == 31 ? pc->pdefAbility31 : ability == 25 ? pc->pdefAbility25 : nullptr;
    if (pd) StartCombo(*pc, pd);
}
// orig3 0x1AF200 / 0x1AF150 for Sly's slots 100 / 99 (jump.cpp ActorJumpKind / SetOwnerJumpState): the kind of his
// state, and kind 9 (the attack) set
int NSlyJumpKind3(SLY* p)
{
    if (CMBR3* pc = PcmbrOf(p, false)) return NGeneric3(*pc);
    CMBR3 c;
    c.p = p;
    c.psly = p;
    return NGeneric3(c);
}
void SetSlyAttack3(SLY* p)
{
    if (CMBR3* pc = PcmbrOf(p, true)) SetGeneric3(*pc, 9);
}

}  // namespace sly3
