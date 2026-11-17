// cmfocus3.cpp: Sly 3's focus side of the follow camera: an object's raw camera record, its vt slot 85 (cplcy.h
// CPDEFIRAW3), read by the follow policy's focus record (orig3 0x1F3ED8, cplcy.cpp). Ported from the Sly 3 asm:
// the SO's 0x1F3D48, the PO's 0x250690, the actor's 0x153D40 (the floor's focus height, the jump 0x1BD758, the
// interaction 0x1ABBF0), Sly's 0x1ABA80, Bentley's 0x13EC70, Murray's 0x1DEE28.
//
// The copied Sly 2 actor keeps Sly 2's numbering inside (Sly's states, the sub-objects' fields): the Sly 3 tests are
// written with their Sly 2 counterparts (noted at each).
#include "cplcy.h"

#include "activate.h"
#include "actor.h"
#include "gang.h"
#include "clock.h"
#include "cls.h"
#include "combo3.h"
#include "crv.h"
#include "game/sly3.h"
#include "lo.h"
#include "sly.h"
#include "so.h"

#include <cmath>
#include <map>

namespace
{
// orig3 actor +0x1154 (the last time the actor was moved by its controller: 0x252508 sets it while the move input is
// not zero): natively the stick's deflection of Sly's pad
std::map<const LO*, float> s_mptMove;

float TMoveLast3(LO* plo, ACTOR& a)
{
    float& t = s_mptMove[plo];
    if (SLY* psly = dynamic_cast<SLY*>(&a))
        if (PADCTL* p = sly3::PpadctlOfSly3(psly); p && p->uJoy > 0.0f) t = g_clock.t;
    return t;
}

// orig3: 0x001F3D48 (the SO's)
void GetSoCpdefiRaw3(LO* plo, CPDEFIRAW3* pr)
{
    *pr = CPDEFIRAW3{};
    const ACTORPOSE pose = PoseFromLo(plo);   // (the record +0x50: its world rows +0x40, position +0x70)
    pr->pos = pose.pos;
    pr->v = pose.vPoint;                       // VelXfPoint at the position
    pr->radHome = std::atan2(-pose.mat[0].y, -pose.mat[0].x);
    pr->tMoveLast = g_clock.t;
    pr->kSpring = 5.0f;
    pr->kDamp = -3.5f;
    pr->n70 = 1;
    pr->cfk = 0;
    pr->zObj = pose.pos.z;
    pr->wAng = glm::length(pose.w);
}

// orig3: 0x001BD758 (the jump sub-object +0x320, while its state +0x33C != -1)
void GetJumpCpdefiRaw3(const JUMP& j, CPDEFIRAW3* pr)
{
    // +0x13C: the Mega Jump's size hold (Sly 2's +0x174)
    if (j.f174)
    {
        pr->lmZ = cmm::LM{-125.0f, 200.0f};          // 0x323320
        pr->smpZ = cmm::SMP{4000.0f, 0.0f, 0.3f};    // 0x323328
        pr->grf &= ~4u;
    }
    const int n = j.n28Sly3;   // +0x28
    if (n == 0) pr->grf |= 1;
    else if (n >= 2 && n < 4)
    {
        pr->lmZ = cmm::LM{-100.0f, 50.0f};           // 0x323338
        pr->smpZ = cmm::SMP{4000.0f, 0.0f, 2.0f};    // 0x323340
        pr->grf = (pr->grf & ~4u) | 0x10;
    }
}

// orig3: 0x001ABBF0 (the interaction sub-object +0x500 of kind 0: a ledge / wall / swing). The copied GetInteractCpdefi
// (Sly 2's 0x18A4E8) keeps the focus point (+0x40) and the zone entry's swing; Sly 3 springs the record's focus offset
// to it (+0x20) instead of the camera's.
void GetInteractCpdefiRaw3(ACTOR& a, const ACTORPOSE& pose, CPDEFIRAW3* pr)
{
    if (a.n4C8 != 0) return;
    CPDEFI c;
    c.sAdjust = 100.0f;
    GetInteractCpdefi(a, pose, &c);
    pr->radHome = c.radHome;
    pr->sAhead = 0.0f;
    if (a.inter.n34 == 1) pr->kSpring = 1.0f, pr->kDamp = -10.0f;
    else if (a.inter.n34 == 2) pr->kSpring = 5.0f, pr->kDamp = -3.5f;
    if (c.sAdjust == 0.0f) pr->dposFocus = a.inter.posFocus - pr->pos;   // (the actor in the interaction's zone)
    pr->cfk = c.cfk;
}

// orig3: 0x001BFA18 (the pipe climb +0x1010, Sly 2's 0xF50 (climb.cpp), while its state +0x1014 != -1): the record at
// the climber's point on the pipe (+0x1018 / +0x101C: the curve, the distance), moving along it at the climb's speed
// (+0x1020); no look-ahead, its z followed (Sly 2's 0x19EA00 had it as Sly's state 8)
void GetClimbCpdefiRaw3(const ACTOR& a, CPDEFIRAW3* pr)
{
    if (!a.pcrvF50) return;
    glm::vec3 pos(0.0f), dir(0.0f);
    EvalCrvS(a.pcrvF50, a.sCrvF50, &pos, &dir);   // 0x22E330
    pr->pos = pos;
    pr->v = dir * a.gCrvF50;
    pr->sAhead = 0.0f;
    pr->grf &= ~4u;
}

// orig3: 0x001C3D78 (the reaction +0xE40, Sly 2's 0xDF0 (0x1A2160), while its kind +0xE44 != -1): the camera keeps its
// pan; no look-ahead for the kinds 5 and 9..13
void GetReactCpdefiRaw3(int n, CPDEFIRAW3* pr)
{
    pr->grf |= 1;
    // (Sly 3's kinds 5, 9..13; the copied damage controller keeps Sly 2's numbering: 5, 8..12, as 0x1A2160)
    if (n == 5 || (n >= 8 && n <= 12)) pr->sAhead = 0.0f;
}

// orig3: 0x00153D40 (the actor's: after the PO's 0x250690 (+0x70 5))
void GetActorCpdefiRaw3(LO* plo, ACTOR& a, CPDEFIRAW3* pr)
{
    GetSoCpdefiRaw3(plo, pr);
    pr->n70 = 5;
    pr->sAhead = 100.0f;
    pr->tMoveLast = TMoveLast3(plo, a);
    const ACTORPOSE pose = PoseFromLo(plo);
    pr->pos = pose.pos;
    // +0x218 the floor contact (Sly 2's +0x208), +0x248 its point; +0x508 the interaction's kind 3 / 4; +0x1158 the
    // focus height (Sly 2's +0x1048), +0x268 the base (Sly 2's +0x240)
    if (a.pxpFloor || a.n4C8 == 3 || a.n4C8 == 4) pr->pos.z = a.posFloor.z + a.sHeight;
    else
    {
        pr->pos.z += a.vecBase.z + a.sHeight;
        if (a.jump.state != 0x12 && a.jump.state != 0x15) pr->grf |= 4;
    }
    if (a.f1044) pr->grf |= 2;   // +0x1150 (the camera target held)
    if (a.nSubF54 != -1) GetClimbCpdefiRaw3(a, pr);
    if (a.jump.state != -1) GetJumpCpdefiRaw3(a.jump, pr);
    if (a.nSub4CC != -1) GetInteractCpdefiRaw3(a, pose, pr);
    if (a.nSubDF4 != -1) GetReactCpdefiRaw3(a.nSubDF4, pr);
    if (a.nSubF54 != -1) GetClimbCpdefiRaw3(a, pr);
}

// orig3: 0x001ABA80 (Sly's)
void GetSlyCpdefiRaw3(LO* plo, SLY& s, CPDEFIRAW3* pr)
{
    // +0x1B0C: the last time Sly was active (Sly 3's states 0 / 0xE = Sly 2's 0 / 0x12; +0x50C the interaction, +0x1014
    // the climb's state (Sly 2's +0x4CC / +0xF54); as Sly 2's 0x18A428)
    if (s.state != 0 && s.state != 0x12 && s.nSub4CC != 0 && s.nSubF54 != 1) s.tActive = g_clock.t;
    GetActorCpdefiRaw3(plo, s, pr);
    // Sly 3's state 4 (Sly 2's 3: the hook / bar of +0x19F4, Sly 2's +0x18C4): the record 175 below the object; 0x11
    // (100 below it) and 0x13 (the samples' distance 75 less) have no Sly 2 state
    if (s.state == 3 && s.ploOther)
    {
        pr->pos.z = PosLoWorld(s.ploOther).z - 175.0f;
        pr->grf &= ~4u;
    }
    // Sly 3's 4, 6, 7 = Sly 2's 3, 5, 6: moving under their own control
    if (s.state == 3 || s.state == 5 || s.state == 6) pr->grf |= 1;
}
}  // namespace

void GetLoCpdefiRaw3(LO* plo, CPDEFIRAW3* pr)
{
    ACTOR* pa = PactorFromLo(plo);
    if (!pa)
    {
        GetSoCpdefiRaw3(plo, pr);   // (the PO's 0x250690 only sets +0x70 5, not read by the camera)
        return;
    }
    if (SLY* ps = dynamic_cast<SLY*>(pa)) return GetSlyCpdefiRaw3(plo, *ps, pr);
    GetActorCpdefiRaw3(plo, *pa, pr);
    if (dynamic_cast<BENTLEY*>(pa))
    {
        // orig3: 0x0013EC70 (+0x70 4; his states 5..7 move under their own control: TODO(sly3) his Sly 3 states)
        pr->n70 = 4;
    }
}
