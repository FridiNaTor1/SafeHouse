// jump.cpp: the actors' jump (the sub-object at +0x2C0, actor.h JUMP; module 0x195950..0x19CBF8). Ported from the
// reference's m_195950_p3.cpp (0x195950, 0x195A00, 0x195FB8, 0x196AA8, 0x196DF8, 0x197500), m_197F98_p3.cpp
// (0x197F98), m_1991A0_p3.cpp (0x19A750) and round4low_extra.cpp (0x197370, 0x143D20).
//
// The plain/double jumps, paraglider (states 19 / 20), Hover Pack (22), air control and wall push are below.
// jumptgt.cpp / jumpledge.cpp implement the targeted-jump and ledge sub-objects. Remaining incomplete paths
// retain focused warnings at their call sites (including the long-fall recovery test 0x19A098).
#include "jump.h"
#include "game/sly3.h"
#include "pccharacter.h"   // (PC addition, debug menu only: Carmelita's moveset)
#include "activate.h"
#include "gang.h"
#include "jtthrow.h"
float GEventAseg11(ASEG*, int);  // sly.cpp: orig 0x12F990
#include "hide.h"
#include "aijobs.h"
#include "cplcy.h"
extern uint32_t g_grfjoyt;  // joy.h (0x2DFBC0)

#include "actanim.h"
#include "aseg.h"
#include "blend.h"
#include "gadget.h"
#include "smrun.h"
#include "interact.h"
#include "cmworld.h"
#include "cplcy.h"
#include "jt.h"
#include "actmesh.h"
#include "actor.h"
#include "alo.h"
#include "brx.h"
#include "asega.h"
#include "clock.h"
#include "crv.h"
#include "damage.h"
#include "dzg.h"
#include "lo.h"
#include "mat.h"
#include "po.h"
#include "sly.h"
#include "smrun.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "stepphys.h"
#include "util.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
#include "xfphys.h"
#include "xfmath.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using cmm::F32;

void AccelAloPos(ALO* p, const glm::vec3& v);  // solve.cpp: orig 0x1FAC30

float TJumpBallistic(const glm::vec3& d, const glm::vec3& g, float vzMax, float vzMin, float sMax, float sMin);  // 0x196928


namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "jump: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

constexpr uint32_t BTN_CROSS = 0x40, BTN_CIRCLE = 0x20, BTN_R1 = 0x8, BTN_R3 = 0x400;
const float c_dtJump1 = F32(0x3E19999A);       // 0.15 (0x2BF818 / 0x2BF81C)
const float c_svJump = F32(0x4400C000);        // 515 (0x2BF730 / 0x2BF734)
const float c_agTurnByDir[5] = {0.0f, F32(0x3FC90FDB), F32(0xBFC90FDB), 0.0f, F32(0x40490FDB)};  // 0x2BF780

PADCTL* PpadctlOf(ACTOR* pa)
{
    PODATA* ppd = PpodataFromLo(pa->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
float RadJoy(ACTOR* pa) { PADCTL* p = PpadctlOf(pa); return p ? p->radJoy : 0.0f; }
float UJoy(ACTOR* pa) { PADCTL* p = PpadctlOf(pa); return p ? p->uJoy : 0.0f; }
uint32_t GrfbtnHeld(ACTOR* pa) { PADCTL* p = PpadctlOf(pa); return p ? p->grfbtn : 0; }
bool FBtnWithin(ACTOR* pa, uint32_t grf, float dt) { PADCTL* p = PpadctlOf(pa); return p && GrfPadctlPressedWithin(p, grf, dt); }
bool FBtnTake(ACTOR* pa, uint32_t grf, float dt) { PADCTL* p = PpadctlOf(pa); return p && GrfPadctlTakePressed(p, grf, dt); }  // 0x1C46C0
void ForgetBtn(ACTOR* pa, uint32_t grf) { if (PADCTL* p = PpadctlOf(pa)) ForgetPadctlPresses(p, grf); }

glm::vec3 VelActor(ACTOR* pa) { return VelAloPos(pa->palo); }  // vt slot 33
glm::vec3 PosActor(ACTOR* pa) { return PosAloCenter(pa->palo); }
float GGravityZ(ACTOR* pa) { SOST* ps = Psost(pa->palo); return ps ? ps->dvGravity.z : 0.0f; }  // (+0xE8 +0x88)

// the custom entry's probe point (+0x218: the base point's world position)
glm::vec3 PosProbe0(ACTOR* pa)
{
    if (pa->isoeCustom >= 0 && pa->isoeCustom < static_cast<int>(pa->palo->asoe.size()))
    {
        const SOE& e = pa->palo->asoe[pa->isoeCustom];
        if (pa->iposProbe0 >= 0 && pa->iposProbe0 < static_cast<int>(e.geomWorld.apos.size())) return e.geomWorld.apos[pa->iposProbe0];
    }
    return PosActor(pa);
}

// the owner's vt slot 88 (Sly's 0x18D568: the state of a jump kind) and slot 90
void SetOwnerJumpState(ACTOR* pa, int n)
{
    if (SLY* p = dynamic_cast<SLY*>(pa))
    {
        static const int s_mpkindState[9] = {0, 2, 7, 4, 0x12, 0x13, 0x14, 0x17, 8};
        if (n >= 0 && n < 9) SetSlyState(p, s_mpkindState[n], -1);
        return;
    }
    // the other players' slot 88 (Bentley 0x135C20, Murray 0x1B7E40: gang.cpp)
    using PFN88 = void (*)(LO*, int);
    if (auto pfn = reinterpret_cast<PFN88>(PvNativeVt(pa->palo, 88))) { pfn(pa->palo, n); return; }
    WarnOnce("slot 88 of the other actors");
}

// the owner's parts (the parts tables' bindings, palo->apart)
LO* PloPartOf(ALO* palo, uint32_t ib)
{
    for (const auto& [ibPart, plo] : palo->apart)
        if (ibPart == ib) return plo;
    return nullptr;
}
ASEG* PPartAseg(ALO* palo, uint32_t ib) { return dynamic_cast<ASEG*>(PloPartOf(palo, ib)); }


// orig: 0x00143D20 (the projected height below the floor in dt: the base's height after dt under gravity; when it is
// below the floor, the floor probe 0x173610 from the position down to that height (0, h, any slope rejected))
bool FActorBelowFloorSoon(ACTOR* pa, float dt)
{
    const glm::vec3 v = VelActor(pa);
    const float h = (pa->vecBase.z + dt * v.z) + (0.5f * dt * dt) * GGravityZ(pa);
    if (0.0f < h) return false;
    glm::vec3 pos = PosActor(pa);  // 0x1A62E8
    return FDropOnGround(&pos, 0.0f, h, false);
}

}  // namespace

bool FActorFloorSoon(ACTOR* pa, float dt) { return FActorBelowFloorSoon(pa, dt); }

// orig: 0x0019C8D0 (the jump's slot 99: what the actor may do in the air)
// orig: 0x0019A098 (falling held by contacts: at least 1 s after tPrev (and the stamp, which it sets), no floor contact
// (+0x208): the actor's contacts' normals (toward the actor: the other end's negated) take up the gravity's direction
// (-z) as far as they can (the Gram system N_i . N_j w = N_i . down solved by 0x11AD30, w in [0, FLT_MAX]); stuck
// when what is left of down, down + sum w_i N_i, has z above -0.1)
bool FActorStuckOnContacts(ACTOR* pa, float tPrev, float* ptStamp)
{
    const float t = g_clock.t;
    if (t - tPrev < 1.0f) return false;
    if (ptStamp)
    {
        if (t - *ptStamp < 1.0f) return false;
        *ptStamp = t;
    }
    if (pa->pxpFloor) return false;
    OXA* poxa = pa->palo && pa->palo->psort ? pa->palo->psort->poxa : nullptr;
    if (!poxa) return false;
    std::vector<glm::vec3> anormal;
    for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
        for (XP* pxp = pox->pxp; pxp; pxp = pxp->pxpNext)
            anormal.push_back(pxp->axpd[0].paloRoot != pa->palo ? -pxp->normal : pxp->normal);
    const int c = static_cast<int>(anormal.size());
    if (!c) return false;
    const glm::vec3 down(0.0f, 0.0f, -1.0f);  // (-D_002A2DC0)
    std::vector<float> aa(static_cast<size_t>(c * c)), ar(static_cast<size_t>(c)), aw(static_cast<size_t>(c));
    for (int i = 0; i < c; ++i)
        for (int j = 0; j < c; ++j) aa[size_t(i) * c + j] = glm::dot(anormal[size_t(i)], anormal[size_t(j)]);
    for (int i = 0; i < c; ++i) ar[size_t(i)] = glm::dot(anormal[size_t(i)], down);
    SolveDzgConstraints(c, aa.data(), ar.data(), aw.data(), FLT_MAX);  // (D_002A656C)
    glm::vec3 sum = down;
    for (int i = 0; i < c; ++i) sum += anormal[size_t(i)] * aw[size_t(i)];
    return -0.1f < sum.z;
}

bool FJumpCanDo(const JUMP* pj, int k)
{
    const auto fFree = [pj] {
        switch (pj->state)
        {
        case 6: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19: case 20: case 21: case 22:
            return false;
        default: return true;
        }
    };
    switch (k)
    {
    case 22: if (pj->kind >= 17 || pj->kind < 15) return fFree(); return false;
    case 12: return pj->f2C ? fFree() : false;
    case 3:
        return pj->state == 0x11 && pj->pactor && pj->pactor->f12F0 && (g_grfjoyt & 2) &&
               !FActiveCplcy(&g_cmpolicies.cpdefault);
    case 2: case 5: case 27: return fFree();
    default: return false;
    }
}


// orig: 0x0019A750 (the velocity with its vertical part raised for a jump of kind k)
glm::vec3 VelJumpLaunch(JUMP* pj, int k)
{
    ACTOR* pa = pj->pactor;
    glm::vec3 v = VelActor(pa);
    const float g = GGravityZ(pa);
    const float vz = v.z;
    float f0 = vz, f12 = vz;
    switch (k)
    {
    case 2: f0 = F32(0x442F0000); break;   // 700
    case 18: f0 = F32(0x44610000); break;  // 900
    case 3: f0 = F32(0x44480000); break;   // 800
    case 1: case 7: case 9: case 13: case 14: case 15:
    {
        float h = 215.0f;                   // 0x2BF738
        if (k == 13) h = 500.0f;
        else if (k == 9) h = 1200.0f;
        else if (k == 14) h = 250.0f;
        const float d = (PosProbe0(pa).z - pa->posFloor.z) - h;
        f0 = std::sqrt(std::fmax(0.0f, (g + g) * d));
        break;
    }
    case 4:
    {
        const float x = F32(0x442F0000) + vz * (0.5f + vz * 0.0f);  // 0x2BF8B0
        f0 = x < -FLT_MAX ? -FLT_MAX : (F32(0x442F0000) < x ? F32(0x442F0000) : x);
        break;
    }
    case 6:
    {
        const float h215 = 215.0f, h315 = 315.0f;
        const float f20a = std::sqrt(g * (h215 * -2.0f));
        const float f27 = -f20a;
        const float f24 = f27 / g;
        const float f21 = std::fmin(f20a, vz);
        const float f23 = (f21 - f20a) / g;
        const float f1a = g * ((h315 - h215) * -2.0f);
        const float f20 = f23 * (f20a + g * (f23 * 0.5f));
        const float f13 = std::sqrt(f1a);
        float ax[2] = {0.0f, 0.0f};
        CSolveQuadratic(g * 0.5f, f13, h215, ax);
        const float f1 = ax[1] - f24;
        if (0.0f < f21) f0 = std::sqrt(g * ((h315 - f20) * -2.0f));
        else
        {
            const float f20b = f21 < f27 ? 0.0f : f20;
            const float t = ((f24 + f24) + f1) - f23;
            f0 = (-f20b) / t - g * (t * 0.5f);
        }
        break;
    }
    default: f12 = f0 = vz; break;
    }
    v.z = std::fmax(f0, f12);
    return v;
}

namespace
{

// orig: 0x00196AA8 (the velocity *pv kept off the contacts' walls (normals made horizontal, steeper than 0.7):
// the least-squares push on the normals' Gram matrix; a large push turns state 0 into 1, 2 into 3)
void PushJumpOffWalls(JUMP* pj, glm::vec3* pv)
{
    ALO* palo = pj->pactor->palo;
    OXA* poxa = Poxa(palo);
    if (!poxa) return;
    std::vector<glm::vec3> an;
    auto add = [&](XP* pxp) {
        for (; pxp; pxp = pxp->pxpNext)
        {
            if (an.size() >= 0x40) return false;
            glm::vec3 v = pxp->normal;
            if (palo != pxp->axpd[0].paloRoot) v = -v;  // (+0x8: end 0's root)
            if (F32(0x3F333333) < v.z) continue;
            const float d = v.z;
            an.push_back(glm::vec3(v.x, v.y, v.z - d));
        }
        return true;
    };
    for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
        if (!add(pox->pxp)) break;
    if (an.size() < 0x40) add(poxa->pxpInternal);  // (+0x68)
    if (an.empty()) return;
    const int c = static_cast<int>(an.size());
    std::vector<float> aa(static_cast<size_t>(c) * c), ab(c), ax(c);
    auto dot3 = [](const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; };
    for (int i = 0; i < c; ++i)
        for (int j = 0; j < c; ++j) aa[static_cast<size_t>(i) * c + j] = dot3(an[i], an[j]);
    const glm::vec3 v0 = *pv;
    for (int i = 0; i < c; ++i) ab[i] = dot3(v0, an[i]);
    SolveDzgConstraints(c, aa.data(), ab.data(), ax.data(), FLT_MAX);  // (0x2A63B0)
    glm::vec3 v = v0;
    for (int i = 0; i < c; ++i) v += an[i] * ax[i];
    const glm::vec3 d(v0.x - v.x, v0.y - v.y, v0.z - v.z);
    if (1.0f < dot3(d, d))
    {
        if (pj->state == 0) SetJumpAnimState(pj, 1, nullptr);
        else if (pj->state == 2) SetJumpAnimState(pj, 3, nullptr);
    }
    *pv = v;
}
}  // namespace

// the owner's vt slot 88 (a jump kind's state: Sly's 0x18D568; 3 the damage's state 4)
void NotifyActorJumpKind(ACTOR* pa, int n) { SetOwnerJumpState(pa, n); }

int ActorJumpKind(ACTOR* pa)
{
    if (SLY* p = dynamic_cast<SLY*>(pa))
    {
        // orig: 0x0018D620 (slot 88's table inverted)
        static const int s_ak[24] = {0, -1, 1, -1, 3, -1, -1, 2, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, 4, 5, 6, -1, -1, 7};
        return static_cast<unsigned>(p->state) < 24 ? s_ak[p->state] : -1;
    }
    using PFN89 = int (*)(LO*);
    if (auto pfn = reinterpret_cast<PFN89>(PvNativeVt(pa->palo, 89))) return pfn(pa->palo);
    WarnOnce("slot 89 of the other actors");
    return -1;
}

// orig: 0x00195950
void InitJump(JUMP* pj, ACTOR* pa)
{
    pj->pactor = pa;
    pj->state = -1;
    pj->kind = -1;
    pj->vecF0 = glm::vec4(F32(0x42140000), 0.0f, F32(0x41C80000), 0.0f);  // 0x2BF740: 37, 0, 25
    pj->g164 = 1.0f;
    pj->g108 = -60.0f;
    pj->g10C = -50.0f;
    pj->g114 = 50.0f;
    pj->g11C = 75.0f;
    pj->g118 = -150.0f;
    pj->g110 = 75.0f;
}

// orig: 0x001959C0
void PostLoadJump(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    if (!pa || !pa->palo) return;
    ALO* palo = pa->palo;
    pj->pasegGlide = PPartAseg(palo, 0x438);
    static const uint32_t s_aib[5] = {0x440, 0x44C, 0x458, 0x464, 0x470};
    std::vector<ASEG*> apaseg;
    for (int i = 0; i < 5; ++i)
    {
        pj->apasegGlideBl[i] = PPartAseg(palo, s_aib[i]);
        if (pj->apasegGlideBl[i]) apaseg.push_back(pj->apasegGlideBl[i]);
    }
    pj->ploGlider = PloPartOf(palo, 0x47C);
    pj->ploZone16C = PloPartOf(palo, 0x42C);
    pj->ploZone170 = PloPartOf(palo, 0x430);
    // (the original makes the blend of whatever BLs are bound; natively only with all five: the weights are by index)
    pj->pbl = apaseg.size() == 5 ? PblNewPlain(palo, apaseg) : nullptr;
}

// orig: 0x00197370. The input direction is relative to the current object matrix,
// not the STEP heading goal. Replace the clip once per initial/double-jump phase.
void SelectDirectionalJumpAnimation(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    pj->f30 = 1;
    const XF* object = PxfObj(pa->palo);
    const float heading = object ? std::atan2(object->matWorld[0].y, object->matWorld[0].x) : pa->radHeading;
    const float relative = cmm::RadNormalize(RadJoy(pa) - heading);
    const float absolute = std::fabs(relative);
    if (absolute < F32(0x3F8CBE4C)) pj->n8 = 3;
    else if (absolute > F32(0x4002B0B4)) pj->n8 = 4;
    else pj->n8 = relative > 0.0f ? 2 : 1;
    ASEGA* current = pa->pasega;
    ASEG** clips = pj->state == 2 ? pj->apasegFall : pj->apasegJump;
    ASEG* clip = clips[pj->n8];
    if (!clip) clip = clips[0];
    if (!clip) clip = pj->apasegJump[0];
    if (!current) return;  // no running animation in a partially loaded native actor
    ClearSmaHandle(&pa->psma);
    const float time = current->t, speed = current->svt;
    const OID previous = current->paseg ? current->paseg->oid : -1;
    ApplyAsegCur(clip, pa->palo, time, speed, 1, &pa->pasega);
    if (std::getenv("SLY2N_JUMPDIRTRACE"))
        std::printf("jump direction: t %.9g actor %d state %d kind %d direction %d joy %.9g heading %.9g relative %.9g oldclip %d clip %d time %.9g speed %.9g resulttime %.9g resultspeed %.9g bound %d sma %d\n",
            g_clock.t, pa->palo->oid, pj->state, pj->kind, pj->n8, RadJoy(pa), heading, relative,
            previous, clip ? clip->oid : -1, time, speed, pa->pasega ? pa->pasega->t : -1,
            pa->pasega ? pa->pasega->svt : -1, pa->pasega && pa->pasega->palo == pa->palo,
            pa->psma != nullptr);
}

// orig: 0x00195FB8 (the animation state n with its object; the old state left, the segment applied)
void SetJumpAnimState(JUMP* pj, int n, LO* plo)
{
    ACTOR* pa = pj->pactor;
    ALO* palo = pa->palo;
    const int old = pj->state;
    if (n == old) return;
    switch (old)
    {
    case 21:
        // the trajectory's controller freed (0x12E460), the record back to mode 2 (0x2303A0), the contacts back on
        DeleteCtrl(&pj->pctrl21);
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 2);
        SetAloContactsOff(palo, false);  // 0x1F6860(0)
        break;
    case 17: LeaveJumpLedge(pj); break;
    case 19: case 20:
        // the paraglider closed (unless 19 -> 20): its SM to 0x5F8, the camera zone off
        if (n < 21 && n >= 19) break;
        if (SLY* p = dynamic_cast<SLY*>(pa); p && p->psma1D28) SetSmaGoal(p->psma1D28, 0x5F8);  // 0x1F0660
        SetActorZone(pa, nullptr);  // 0x140C88(actor, 0)
        break;
    case 7:
    {
        // the jump out of a hiding spot: its overlaps with the actor retested (0x134320)
        LO* pold = pj->ploC;
        pj->ploC = nullptr;
        if (pold && FIsLoDerivedFrom(pold, S3CID(0x30))) RecalcSwOxfFilterForObject(static_cast<ALO*>(pold));
        break;
    }
    case 6:
    {
        // the jump into a hiding spot: unless the actor is now hiding in it (interaction kind 1), its overlaps
        // retested (0x134320) and its reservation given back (0x183B80(0)); the record back to mode 2, no velocity
        LO* pold = pj->ploC;
        pj->ploC = nullptr;
        if (pa->n4C8 != 1 && pold)
        {
            if (FIsLoDerivedFrom(pold, S3CID(0x30))) RecalcSwOxfFilterForObject(static_cast<ALO*>(pold));
            if (NHideState(pold) == 1) SetHideState(pold, 0);
        }
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 2);
        pa->vecVel = glm::vec3(0.0f);
        break;
    }
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 18:
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 2);
        pa->vecVel = glm::vec3(0.0f);
        if (old >= 14 && old < 16)
        {
            // the cane's reach ends (Sly: mode -1; the others' reach controller +0x900: 0x1CCAC0(-1))
            if (SLY* p = dynamic_cast<SLY*>(pa)) SetSlyMode(p, -1);
            else WarnOnce("leaving the jump's state 14 / 15 (the reach controller +0x900)");
        }
        break;
    default: break;
    }
    ASEGA* pasegaCur = pa->pasega;  // s1
    ASEG* paseg = nullptr;          // s0
    pj->state = n;
    pj->ploC = plo;
    int grfapl = 1;
    float tStart = 0.0f, svt = 1.0f;
    pj->t20 = g_clock.t;
    switch (n)
    {
    case 0: case 1: case 4: case 5: case 7:
    {
        const int k = pj->kind;
        if (k == 14) paseg = pj->pasegLand158;
        else if (k == 13) paseg = pj->pasegLand148;
        // orig: 0x00195FB8: Murray's carried jump/fall use his own authored
        // parts before the generic jump fallback (owner +0x1518 / +0x1514).
        else if (k == 15) paseg = PPartAseg(palo, 0x1518);
        else if (k == 16) paseg = PPartAseg(palo, 0x1514);
        if (paseg) break;
        paseg = pj->apasegJump[pj->n8 >= 0 && pj->n8 < 5 ? pj->n8 : 0];
        if (!paseg) paseg = pj->apasegJump[0];
        break;
    }
    case 2: case 3:
        if (pj->kind == 14) paseg = pj->pasegLand15C;
        if (!paseg)
        {
            paseg = pj->apasegFall[pj->n8 >= 0 && pj->n8 < 5 ? pj->n8 : 0];
            if (!paseg) paseg = pj->apasegFall[0];
            if (!paseg) paseg = pj->apasegJump[0];
        }
        pj->f28 = 0;
        break;
    case 11: case 12: case 13: case 14: case 15:
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 0x11);
        pa->vecVel = glm::vec3(0.0f);
        pj->f28 = 0;
        pj->n8 = 0;
        paseg = pj->apasegJump[0];
        if (n >= 14)
        {
            // the cane reaches for the object (Sly: mode 0 with +0x1854; the others: +0x984 and the reach controller
            // +0x900 from the middle of +0x930: TODO(actors))
            if (SLY* p = dynamic_cast<SLY*>(pa); p && p->palo == PloPoCurrent(g_psw))
            {
                p->reach.ploTarget = plo;
                SetSlyMode(p, 0);
            }
            else WarnOnce("the jump's states 14 / 15 of the other actors (+0x984, +0x900)");
        }
        break;
    case 6:
        // the jump into a hiding spot (0x197F98 aimed the record): the record driven (mode 17), the spot's overlaps
        // with the actor retested (0x134320: none now, 0x183A50) and reserved (0x183B80(1)); the clip kept
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 0x11);
        if (plo && FIsLoDerivedFrom(plo, S3CID(0x30))) RecalcSwOxfFilterForObject(static_cast<ALO*>(plo));
        SetHideState(plo, 1);
        pa->vecVel = glm::vec3(0.0f);
        paseg = pasegaCur ? pasegaCur->paseg : nullptr;
        break;
    case 8:
        // the jump to an object of list B: the record driven, the clip kept
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 0x11);
        pa->vecVel = glm::vec3(0.0f);
        paseg = pasegaCur ? pasegaCur->paseg : nullptr;
        break;
    case 9: case 10: case 16: case 18:
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 0x11);
        pa->vecVel = glm::vec3(0.0f);
        pj->f28 = 0;
        SetAloVelocityTo(palo, VelJumpLaunch(pj, 4));  // vt slot 35
        pj->n8 = 0;
        paseg = pj->state == 16 ? pj->pasegLand14C : pj->pasegLand148;
        pasegaCur = nullptr;
        break;
    case 21:
    {
        // the jump back to the safe ground +0x490: the time a jump needs (0x196928, at least 0.7 s), the launch
        // d / t - g t / 2 (natively by the physics: the original drives the record along it, 0x12DE58 kind 5, and
        // turns the collisions off 0x1F6860), its end tEnd21, facing it
        const glm::vec3 posNow = PxfPos(palo) ? glm::vec3(PxfPos(palo)->matWorld[3]) : PosActor(pa);
        const glm::vec3 d = PosSafeGround(pa, pa->entryJump, 0.7f) - posNow;
        const SOST* ps = Psost(palo);
        const glm::vec3 grav = ps ? ps->dvGravity : glm::vec3(0.0f, 0.0f, -1750.0f);
        LO* target = pa->entryJump.ploOwner;
        const bool upward = target && (FIsLoDerivedFrom(target, S3CID(0x13)) || FIsLoDerivedFrom(target, S3CID(0xB)));
        const float tj = std::max(F32(0x3F333333), TJumpBallistic(d, grav, 1000.0f, upward ? 0.0f : -1000.0f,
                                                              1.0f, upward ? 500.0f : -1.0f));
        const glm::vec3 vel = d * (1.0f / tj) - grav * (tj * 0.5f);
        SetAloVelocityTo(palo, vel);  // vt slot 35
        pj->tEnd21 = g_clock.t + tj;
        // the record driven along the flight to the goal by the end, with the flight's final velocity (0x12DE58: a
        // kind-8 controller, priority 5, mode 0x11, in the record's parent space 0x22FC48 / 0x22FCC8). Without it
        // the record stayed in mode 2 and the water still dragged it in the launch frame: each water cycle's hop
        // fell short (w43: 45 lower per cycle than the reference).
        if (XF* pxf = PxfPos(palo))
        {
            const glm::vec3 posL = PosConvertXfs(nullptr, pxf->pxfRef, posNow + d);
            const glm::vec3 velL = VecConvertXfs(nullptr, pxf->pxfRef, vel + grav * tj);
            DeleteCtrl(&pj->pctrl21);
            pj->pctrl21 = PctrlNewTarget(palo, pxf, 5, pj->tEnd21, glm::vec4(posL, 1.0f), glm::vec4(velL, 0.0f));
        }
        pa->vecVel = glm::vec3(0.0f);
        SetStepPhysHeadingToward(palo, d);  // 0x2046B0
        SetAloContactsOff(palo, true);  // 0x1F6860(1): no contacts along the flight
        paseg = pj->apasegJump[0];  // (+0x120)
        break;
    }
    case 19:
    {
        // the paraglider opens: the heading less the jump's direction's turn, the double jump off, its clip +0x178,
        // the camera zone +0x1BC (its first entry's +4 the mean of D_002BF798 / 79C), the SM to 0x5F7
        SetStepPhysHeading(palo, pa->radHeading - c_agTurnByDir[pj->n8 >= 0 && pj->n8 < 5 ? pj->n8 : 0]);  // 0x204660
        pj->f28 = 0;
        paseg = pj->pasegGlide;
        SetActorZone(pa, pj->ploGlider);  // 0x140C88
        // (its zone's first entry +0x114: the path offset +4 = the mean of D_002BF798 {-250, -500})
        if (ZONECME* pcme = PcmeFirstOfZone(pj->ploGlider)) pcme->dtPath = F32(0xC3FA0000) * 0.5f + F32(0xC37A0000) * 0.5f;
        if (SLY* p = dynamic_cast<SLY*>(pa); p && p->psma1D28) SetSmaGoal(p->psma1D28, 0x5F7);  // 0x1F0660
        break;
    }
    case 20:
        // the glide: the blend +0x1B8 of the five BLs, level only (0x1384C0)
        grfapl = 9;
        if (pj->pbl) SetBlendWeights(pj->pbl, {1.0f, 0.0f, 0.0f, 0.0f, 0.0f});
        paseg = pj->pbl;
        break;
    case 22:
        // orig: 0x00195FB8 (Hover Pack): actor +0x488 / jump +0x1C8.
        paseg = PPartAseg(palo, 0x488);
        grfapl = 9;
        break;
    case 17:
        // the hang (the actor's slot 91: Sly's mode off)
        if (SLY* p = dynamic_cast<SLY*>(pa)) SetSlyMode(p, -1);
        EnterJumpLedge(pj);
        paseg = pj->pasegLand154;
        grfapl = 9;
        if (pj->f174)
        {
            SetStepPhysSize(palo, pj->sSizeSaved);
            pj->f174 = 0;
        }
        break;
    case -1:
        if (pj->f174)
        {
            SetStepPhysSize(palo, pj->sSizeSaved);
            SetActorZone(pa, nullptr);  // 0x140C88(actor, 0)
            pj->f174 = 0;
        }
        pj->kind = -1;
        break;
    default:
        WarnOnce("the jump's state " + std::to_string(n));
        break;
    }
    if (pj->state == 2 || pj->state == 0) pj->f30 = 0;
    else if (pj->state == 0x12) RecalcSwOxfFilterForObject(pa->palo);  // 0x134320 (the pair with Murray: 0x220498)
    if (pj->state == -1) return;
    if (!pasegaCur || pasegaCur->paseg != paseg)
    {
        if (pa->psma) { RetractSma(pa->psma); pa->psma = nullptr; }  // 0x1F15B8
        ApplyAsegCur(paseg, palo, tStart, svt, grfapl, &pa->pasega);
    }
    UpdateActorStepMesh(pa);  // the owner's vt slot 90 (0x18C060 / 0x135D80 / 0x1B8008)
}

// orig: 0x00195A00 (the jump's kind n (6 keeps the kind and its times): the owner's state (slot 88), the launch
// velocity, the animation state, the blend weights +0x24.. +0x38)
void SetJumpKind(JUMP* pj, int n, int a2, LO* plo)
{
    ACTOR* pa = pj->pactor;
    ALO* palo = pa->palo;
    if (n != 6) pj->kind = n;
    pj->n8 = a2;
    SetOwnerJumpState(pa, 1);  // vt slot 88 (1)
    if (n != 6)
    {
        pj->n40 = 0;
        const float t = g_clock.t;
        pj->t44 = pj->t4C = pj->t48 = t;
    }
    if (n == 9)
    {
        pj->sSizeSaved = pa->sSize;
        SetStepPhysSize(palo, F32(0x3F400000));  // 0.75
    }
    const glm::vec3 v = VelJumpLaunch(pj, n);
    SetAloVelocityTo(palo, v);  // vt slot 35
    if (static const int s_oid = std::getenv("SLY2N_JUMPPRINT") ? std::atoi(std::getenv("SLY2N_JUMPPRINT")) : 0;
        s_oid && palo && palo->oid == s_oid)
    {
        const glm::vec3 p = PosAloCenter(palo);
        std::printf("jump: oid %d kind %d t %.3f at %.0f %.0f %.0f vel %.0f %.0f %.0f entry %.0f %.0f %.0f\n", s_oid, n,
                    g_clock.t, p.x, p.y, p.z, v.x, v.y, v.z, pa->entryJump.pos.x, pa->entryJump.pos.y, pa->entryJump.pos.z);
    }
    pa->t131C = g_clock.t;
    if (n != 6) pj->t18 = g_clock.t;
    float g34 = 1.0f;
    bool fSetG34 = false;
    auto jumpDir = [&](int kAnim) { SetJumpAnimState(pj, kAnim, plo); };
    switch (n)
    {
    case 0: jumpDir(4); pj->f28 = 1; pj->g38 = 1.0f; pj->f24 = 0; fSetG34 = true; break;
    case 11: jumpDir(4); pj->f24 = 1; pj->g38 = 1.0f; pj->f28 = 0; fSetG34 = true; break;
    case 3:
        if (UJoy(pa) < F32(0x3E4CCCCD))
        {
            pj->n40 = 3;
            const glm::vec3 d = cmm::SetVectorCylind(pa->radHeading, 100.0f, 0.0f);  // 0x11E1A0 (0x2BF754)
            SetAloVelocityTo(palo, v + d);
        }
        jumpDir(0);
        pj->f28 = 1;
        pj->g34 = 1.0f;
        pj->g38 = F32(0x3F19999A);
        pj->f24 = 0;
        break;
    case 1: case 2: case 18: jumpDir(0); pj->f28 = 1; pj->g38 = 1.0f; pj->f24 = 0; fSetG34 = true; break;
    case 7: jumpDir(7); pj->f28 = 1; pj->g34 = 1.0f; pj->f24 = 0; pj->g38 = 1.0f; break;
    case 9:
        // the Mega Jump (31): the animation state 0, its camera zone +0x16C, faster air control (1.5 / 1.2), the
        // size kept 0.75 until it ends (+0x174)
        jumpDir(0);
        pj->f24 = 0;
        pj->f28 = 1;
        SetActorZone(pa, pj->ploZone16C);  // 0x140C88
        pj->g38 = F32(0x3FC00000);         // D_002BF758
        pj->g34 = F32(0x3F99999A);         // D_002BF75C
        pj->f174 = 1;
        break;
    case 14:
    {
        // the Feral Pounce (30): the animation state 1, the air control 1.75 (D_002BF760); the stick let go: a push
        // along the stick's direction of 515 x 1.75 (the input's gate 3)
        jumpDir(1);
        const float g2 = F32(0x3FE00000), g0 = F32(0x3F800000);  // D_002BF760 / D_002BF764
        pj->f28 = 1;
        pj->g34 = g0;
        pj->f24 = 0;
        pj->g38 = g2;
        if (UJoy(pa) < F32(0x3E4CCCCD))
        {
            pj->n40 = 3;
            SetAloVelocityTo(palo, v + cmm::SetVectorCylind(RadJoy(pa), c_svJump * g2, 0.0f));  // 0x11E1A0
        }
        break;
    }
    case 13:
        jumpDir(1);
        pj->f28 = 1;
        pj->g38 = F32(0x3F800000);  // D_002BF768
        pj->g34 = F32(0x3F800000);  // D_002BF76C
        pj->f24 = 0;
        break;
    case 8:
        // launched (0x141968: a pad's bounce): the animation state 5, the launch's heading / speed kept (0x11EF08 to
        // +0xB0 / +0xB8, +0xB4 1)
        jumpDir(5);
        pj->f24 = 0;
        pj->g34 = 1.0f;
        pj->g38 = 1.0f;
        pj->f28 = 1;
        pj->radB0 = std::atan2(v.y, v.x);
        pj->sB8 = std::sqrt(v.x * v.x + v.y * v.y);
        pj->gB4 = 1.0f;
        break;
    case 6:
    {
        const int k = pj->kind;
        jumpDir((k < 15 && k >= 13) ? 3 : 2);
        return;
    }
    case 10:
        // back to the safe ground +0x490 (0x19F590): the animation state 0x15
        pj->n40 = -1;
        jumpDir(0x15);
        pj->f24 = pj->f28 = 0;
        pj->g34 = pj->g38 = 0.0f;
        break;
    case 12: jumpDir(1); pj->n40 = 3; pj->f28 = 1; pj->g38 = 1.0f; pj->f24 = 0; fSetG34 = true; break;
    case 16: jumpDir(4); pj->f24 = 0; pj->g38 = 1.0f; pj->f28 = 0; fSetG34 = true; break;
    case 15: jumpDir(0); pj->f24 = 0; pj->g38 = 1.0f; pj->f28 = 0; fSetG34 = true; break;
    case 17:
        // orig: 0x00195A00 (Hover Pack). Ground takeoff is vertical at 400;
        // the original's incidental planar carrier is zero in the compared
        // ground calls (including nonzero incoming velocities). An air start
        // retains its velocity and double-jump flag; both consume hover permission.
        if (pj->state == -1)
        {
            SetAloVelocityTo(palo, glm::vec3(0.0f, 0.0f, F32(0x43C80000)));  // D_002BF770
            pj->f28 = 1;
        }
        jumpDir(22);
        pj->f24 = 0;
        pj->f2C = 0;
        pj->g38 = 1.0f;
        fSetG34 = true;
        break;
    default:
        WarnOnce("the jump kind " + std::to_string(n));
        break;
    }
    if (fSetG34) pj->g34 = g34;
    if (n != 17) pj->f2C = pj->f28;
    if (SLY* p = dynamic_cast<SLY*>(pa)) p->n18F0 = -1;
    pj->z3C = PosActor(pa).z;
}

// orig: 0x00197500 (the jump's per-frame transitions, repeated until its state settles)
void UpdateJumpTransitions(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    const glm::vec3 v = VelActor(pa);
    bool fFloor = pa->pxpFloor && F32(0x3F333333) < pa->pxpFloor->normal.z;  // (+0x208's +0x88)
    for (int iter = 0; iter < 32; ++iter)
    {
        const int s5 = pj->state;
        if (pj->ploC && !pj->ploC->fInWorld) WarnOnce("0x19C828 (the jump's object left)");
        // (PC addition, debug menu only: Carmelita's moveset has no air targets or paraglider; her double jump below)
        if (!(pj->state < 23 && pj->state >= 9) && !FPcCarm(pa->palo))
        {
            if (pj->f28 && FBtnWithin(pa, BTN_CROSS, 0.0f) && !FActorBelowFloorSoon(pa, F32(0x3D99999A)))
            {
                ForgetBtn(pa, BTN_CROSS);
                SetJumpKind(pj, 6, pj->n8, 0);  // the double jump
            }
            if (FBtnWithin(pa, BTN_CIRCLE, 0.0f))
            {
                // circle: the best target in reach (0x19B110); taken when it changed the state
                const int st = pj->state;
                ChooseJumpTarget(pj, nullptr);
                if (pj->state != st) ForgetBtn(pa, BTN_CIRCLE);
            }
            // R1 in the air: the paraglider (27) when owned, not about to land (0.3 s), for the current player
            if (FBtnWithin(pa, BTN_R1, 0.0f) && pa->palo == PloPoCurrent(g_psw) && FPowerupOwned(0x1B) &&
                !FActorBelowFloorSoon(pa, F32(0x3E99999A)))
            {
                SpendActorPower(pa, 0x1B);  // 0x141CD0 (costs 0)
                ForgetBtn(pa, BTN_R1);      // 0x1C4738
                StartActorAbility(pa, 0x1B);  // the current player's vt slot 100 (Sly: 0x18DA48 -> state 19)
            }
        }
        // (PC addition, debug menu only: Carmelita's own double jump at her height, pccharacter.h; not the guards')
        else if (!(pj->state < 23 && pj->state >= 9) && pj->f28 && FPcCarmDoubleJump(pa->palo) && FBtnWithin(pa, BTN_CROSS, 0.0f) &&
                 !FActorBelowFloorSoon(pa, F32(0x3D99999A)))
        {
            ForgetBtn(pa, BTN_CROSS);
            // (on Bentley / Murray: no kind 6, their jumps have no double jump's animation state; once only)
            if (dynamic_cast<SLY*>(pa)) SetJumpKind(pj, 6, pj->n8, 0);
            else pj->f28 = 0;
            PcCarmJumped(pa->palo, false, true);
        }
        bool fSlot88 = false;
        switch (pj->state)
        {
        case 21:
            // the jump back's end: the horizontal velocity stopped, falling on (kind 0xB)
            if (!(pj->tEnd21 < g_clock.t))
            {
                // a script's leap to an object with an interaction part (SCPT 0x2A on a pipe, a rail ...): within
                // 0.25 s of the end the jump takes it as its target (0x19B110 forced on it: e.g. Sly grabs the pipe)
                const SAFEGROUND::ENTRY& e = pa->entryJump;
                if (e.jtl != JTL_Max && pj->tEnd21 < g_clock.t + 0.25f && FSafeTargetReattach(e.ploOwner))
                {
                    SetJumpKind(pj, 0xB, 0, 0);            // 0x195A00
                    ChooseJumpTarget(pj, e.ploOwner);       // 0x19B110
                    break;
                }
                // 0x197500: SCPT may allow a second launch once the return flight is descending.
                XF* pxf = PxfPos(pa->palo);
                if (pj->f28 && pxf && pxf->vel.z < 0.0f)
                {
                    ApplyAsegCur(pj->apasegFall[0], pa->palo, 0.0f, 1.0f, 1, &pa->pasega);
                    pj->f28 = 0;
                    SetAloVelocityTo(pa->palo, VelJumpLaunch(pj, 6));
                    break;
                }
                if ((e.jtl != JTL_Max || e.psoe) && pxf && pj->pctrl21)
                {
                    const glm::vec3 goal = PosSafeGround(pa, e, std::min(0.7f, pj->tEnd21 - g_clock.t));
                    auto* ctrl = static_cast<CTRLTARGET*>(pj->pctrl21);
                    ctrl->SetGoal(glm::vec4(PosConvertXfs(nullptr, pxf->pxfRef, goal), 1.0f), ctrl->vel);
                }
            }
            else
            {
                SLY* psly = dynamic_cast<SLY*>(pa);
                const int n18F0 = psly ? psly->n18F0 : -1;
                glm::vec3 vv = VelActor(pa);
                vv.x = vv.y = 0.0f;
                SetAloVelocityTo(pa->palo, vv);
                SetJumpKind(pj, 0xB, 0, 0);
                if (psly) psly->n18F0 = n18F0;
            }
            break;
        case 0:
            if (c_dtJump1 < g_clock.t - pj->t20)
            {
                if (!(GrfbtnHeld(pa) & BTN_CROSS) && pj->n40 != 3) pj->f24 = 1;
                SetJumpAnimState(pj, 1, pj->ploC);
                break;
            }
            if (pj->f30) break;
            if (0.5f < UJoy(pa)) SelectDirectionalJumpAnimation(pj);
            break;
        case 2:
            if (c_dtJump1 < g_clock.t - pj->t20)
            {
                SetJumpAnimState(pj, 3, pj->ploC);
                break;
            }
            if (pj->f30) break;
            if (0.5f < UJoy(pa)) SelectDirectionalJumpAnimation(pj);
            break;
        case 1: case 7:
            if (v.z < 0.0f) SetJumpAnimState(pj, 4, pj->ploC);
            break;
        case 5:
            if (0.25f < g_clock.t - pj->t20 && fFloor) fSlot88 = true;
            else if (FActorStuckOnContacts(pa, pj->t20, &pa->t131C)) RecoverActor(pa);  // 0x19A098 -> 0x19F590(pa, 0)
            break;
        case 3: case 4:
            if (fFloor)
            {
                if (pj->kind != 14) fSlot88 = true;
                else StartInteract(pa, 10, nullptr, JTL_Max);  // 0x1928F8 (the kind 14 landing: +0x4C0's kind 10)
                break;
            }
            if (!FPcCarm(pa->palo) && FJumpGrabLedge(pj)) break;  // 0x199D48: a ledge (state 17) (PC: not Carmelita)
            if (FActorStuckOnContacts(pa, pj->t20, &pa->t131C)) RecoverActor(pa);  // 0x19A098 -> 0x19F590(pa, 0)
            break;
        case 17:
        {
            // the hang: CROSS after 0.25 s climbs up; the ledge gone lets go; facing the wall
            if (0.25f < g_clock.t - pj->t20 && FBtnTake(pa, BTN_CROSS, 0.25f)) { SetJumpKind(pj, 3, 0, 0); break; }
            static const cmm::SMP s_smpDraw{1000.0f, 0.0f, 0.25f};  // 0x2BF820
            pa->vecDrawOffset = cmm::PosSmooth(pa->vecDrawOffset, glm::vec3(0.0f), g_clock.dt, &s_smpDraw, nullptr);  // 0x11F088
            if (!FCheckJumpLedgeHold(pj)) { SetJumpAnimState(pj, 4, nullptr); break; }
            SetStepPhysHeadingToward(pa->palo, -pj->normalEdge);  // 0x2046B0
            break;
        }
        case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16:
            // the flight to the target: its time over (the record's +0xA8), its action (0x19C688)
            if (XF* pxf = PxfPos(pa->palo); pxf && pxf->tTarget < g_clock.t) ArriveJumpTarget(pj);
            break;
        case 18:
        {
            // the flight onto Murray's hands (Bentley): Murray's +0x1910 picks him up (its state 3) when the flight's
            // end is within the pickup clip's (+0x1974) event 0x1C7; then as the others
            XF* pxf = PxfPos(pa->palo);
            MURRAY* pm = PmurrayGlobal();
            if (pxf && pm)
            {
                const float tLabel = GEventAseg11(pm->jtthrow1910.pickup, 0x1C7);  // 0x12F990
                if (pxf->tTarget - tLabel <= g_clock.t && pm->jtthrow1910.state != 3)
                {
                    SetJtThrowState(pm->jtthrow1910, 3);  // 0x2206C8 (+0x1910, 3)
                    break;
                }
            }
            if (pxf && pxf->tTarget < g_clock.t) ArriveJumpTarget(pj);
            break;
        }
        case 19: case 20:
        {
            // the glide ends on any contact (0x1974C8: the attachment's overlaps) or when neither R1 nor the
            // paraglider's gadget button is held; the opening (19) becomes the glide (20) when its clip ends
            const OXA* poxa = Poxa(pa->palo);
            bool fContact = false;
            for (const OX* pox = poxa ? poxa->pox : nullptr; pox && !fContact; pox = pox->poxNext) fContact = pox->pxp != nullptr;
            if (fContact || !(GrfbtnHeld(pa) & (GrfAbilityButton(pa, 0x1B) | BTN_R1)))
            {
                SetJumpAnimState(pj, 4, nullptr);
                break;
            }
            if (pj->state != 19) break;
            if (pa->pasega && !FAsegaRetracted(pa->pasega)) break;
            SetJumpAnimState(pj, 20, nullptr);
            break;
        }
        case 22:
        {
            // orig: 0x00197500 (Hover Pack): power, floor, button release, duration.
            const float elapsed = g_clock.t - pj->t20;
            if (NActorPower(pa) <= 0) SetJumpAnimState(pj, 4, nullptr);
            else if (F32(0x3F000000) < elapsed && fFloor) fSlot88 = true;  // D_002BF82C: 0.5
            else if (F32(0x3F000000) < elapsed && !(GrfbtnHeld(pa) & GrfAbilityButton(pa, 12)))
                SetJumpAnimState(pj, 4, nullptr);
            else if (F32(0x40400000) < elapsed) SetJumpAnimState(pj, 4, nullptr);  // D_002BF830: 3
            break;
        }
        case 6:
            // the hiding spot knocked over meanwhile: falling; else at the end of the flight its action (0x19C688)
            if (pj->ploC && NHideState(pj->ploC) == 3) SetJumpAnimState(pj, 4, nullptr);
            else if (XF* pxf = PxfPos(pa->palo); pxf && pxf->tTarget < g_clock.t) ArriveJumpTarget(pj);
            break;
        case 8:
            if (XF* pxf = PxfPos(pa->palo); pxf && pxf->tTarget < g_clock.t) SetJumpAnimState(pj, 4, pj->ploC);
            break;
        default:
            WarnOnce("the jump's state " + std::to_string(pj->state) + " transitions");
            break;
        }
        if (fSlot88) SetOwnerJumpState(pa, 0);  // vt slot 88 (0): back on the ground
        if (pj->state == s5 || pj->state == -1) break;
    }
}

// orig: 0x00197F98 (the deflection gate, target path, facing and move +0xA0)
void UpdateJumpInput(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    const float u = UJoy(pa);
    switch (pj->n40)
    {
    case 1:
        if (u < F32(0x3F666666))
        {
            pj->n40 = 2;
            pj->t4C = g_clock.t;
            if (g_clock.t - pj->t48 < F32(0x3E99999A)) pj->f24 = 1;
        }
        break;
    case 0:
        if (F32(0x3F666666) < u)
        {
            pj->n40 = 1;
            pj->t48 = g_clock.t;
        }
        else if (F32(0x3D8F5C29) < g_clock.t - pj->t44) pj->f24 = 1;
        break;
    case 3:
        if (F32(0x3E4CCCCD) < u) pj->f24 = 1;
        break;
    default: break;
    }
    const glm::vec3 pos = PosActor(pa);
    const glm::vec3 v = VelActor(pa);
    const unsigned st = static_cast<unsigned>(pj->state);
    if ((st >= 23 || st == 1 || st == 3 || st == 4 || st == 5 || st == 17) && !FPcCarm(pa->palo)) FindJumpHideTarget(pj);  // (jumptgt.cpp; PC: not Carmelita)
    const float headingSample = UpdateJumpTargetPath(pj);
    // the facing
    const float f21 = c_agTurnByDir[pj->n8 >= 0 && pj->n8 < 5 ? pj->n8 : 0];
    float f20 = pa->radHeading;
    switch (pj->state)
    {
    case 0:
        if (F32(0x3E4CCCCD) < u) f20 = RadJoy(pa) + f21;
        break;
    case 11: case 12: case 13: f20 = headingSample; break;
    case 17: case 19: break;
    case 14: case 15:
    {
        // toward the target: the pipe's point at +0x10 (state 15), else the object's position; then the heading kept
        glm::vec3 posT(0.0f);
        if (pj->state == 15 && pj->ploC && FIsLoDerivedFrom(pj->ploC, S3CID(0x0A))) EvalCrv(PcrvFromLo(pj->ploC), pj->g10, &posT, nullptr);
        else if (pj->ploC) posT = PosLoWorld(pj->ploC);  // 0x1A62E8 (any object: a hook too, not only an ALO)
        if (!dynamic_cast<SLY*>(pa)) WarnOnce("the jump's facing in state 14 / 15: the reach controller +0x900 (0x1CC2E8)");
        SetStepPhysHeadingToward(pa->palo, posT - pos);  // 0x2046B0
        f20 = pa->radHeading;
        break;
    }
    case 20:
    {
        // the glide's steering: the stick against the heading (sin: the turn, cos: forward / back); the camera zone's
        // distance, the speeds +0x1C0 / +0x1C4 and the turn rate mixed by how far forward; the blend's weights
        const float f24 = RadStepHeading(pa);  // 0x198954 -> 0x204740: actual body heading
        float gSin, gCos;
        cmm::CalculateSinCos(cmm::RadNormalize(RadJoy(pa) - f24), &gSin, &gCos);  // 0x11CF60 / 0x11D608
        const float mag = UJoy(pa);
        const float f22 = gCos * mag, f21b = gSin * mag;
        const float h = (f22 + 1.0f) * 0.5f;
        const float g = 1.0f - h;
        // the camera zone's first entry: its path offset +4 between -250 (back) and -500 (forward), smoothed
        // (0x11D018, D_002BF860 {300, 0, 0.3})
        if (pj->ploGlider)
            if (ZONECME* pcme = PcmeFirstOfZone(pj->ploGlider))
            {
                static const cmm::SMP s_smp{F32(0x43960000), 0.0f, F32(0x3E99999A)};
                pcme->dtPath = cmm::GSmooth(pcme->dtPath, g * F32(0xC37A0000) + h * F32(0xC3FA0000), g_clock.dt, &s_smp, nullptr);
            }
        pj->svGlide1C0 = g * F32(0x43960000) + h * F32(0x44480000);   // D_002BF850: 300 / 800
        const float rate = g * F32(0x40A00000) + h * F32(0x41400000);  // D_002BF870: 5 / 12
        pj->svGlide1C4 = g * F32(0xC3480000) + h * F32(0xC3C80000);   // D_002BF858: -200 / -400
        const float dturn = rate * f21b * g_clock.dt;
        std::vector<float> aw(5, 0.0f);
        aw[0] = 1.0f - mag;
        f20 = f24 + dturn;
        if (mag != 0.0f)
        {
            const float q = mag / (std::fabs(f21b) + std::fabs(f22));
            if (f21b < 0.0f) aw[2] = -f21b * q;
            else aw[1] = f21b * q;
            if (0.0f < f22) aw[3] = f22 * q;
            else aw[4] = -f22 * q;
        }
        if (pj->pbl) SetBlendWeights(pj->pbl, aw);  // +0x17C.. then 0x1384C0
        break;
    }
    case 22:
    {
        // orig: 0x00197F98 (Hover Pack): the target velocity is already in
        // world coordinates; the tilted heading matrix is written directly.
        DrainActorPower(pa, F32(0x3F800000));  // D_002BF878: 1 per second
        const float speed = u * F32(0x43960000);  // D_002BF87C: 300
        pa->vecVel = cmm::SetVectorCylind(RadJoy(pa) + F32(0x40490FDB), speed, F32(0xC2C80000));  // D_002BF880: -100
        const float planarSpeed = 0.0f + std::sqrt(v.x * v.x + v.y * v.y);
        if (F32(0x43480000) < planarSpeed) pa->radHeading = std::atan2(v.y, v.x) + f21;  // D_002BF848: 200
        const glm::vec3 planarVelocity(v.x, v.y, 0.0f);
        const glm::vec3 planarTarget(pa->vecVel.x, pa->vecVel.y, 0.0f);
        glm::vec3 up = glm::vec3(0.0f, 0.0f, F32(0x449C4000)) + planarVelocity * F32(0x3F000000);  // D_002BF890: 1250
        up = up + planarTarget * F32(0xBF000000);
        const glm::mat3 tilt = BuildRotateVectorsMatrix(g_normalZ, up);  // 0x11BA08
        const glm::mat3 heading = BuildRotateMatrixAxis(pa->radHeading, g_normalZ);  // 0x11B500
        pa->matHeading = cmm::MulMat3(heading, tilt);
        break;
    }
    default:
    {
        // (slot 92 is 0 for Sly) the heading follows the horizontal velocity above 200
        const float l = std::sqrt(v.x * v.x + v.y * v.y);
        if (F32(0x43480000) < l) f20 = std::atan2(v.y, v.x) + f21;
        break;
    }
    }
    if (pj->state != 22) SetStepPhysHeading(pa->palo, f20);
    const int k = pj->state;
    if (k >= 0 && (k < 6 || k == 7))
    {
        float sv;
        if (k < 4) sv = c_svJump;
        else if (k == 5)
        {
            // launched (kind 8): the launch's horizontal speed +0xB8 kept as far as the stick still points along its
            // heading +0xB0 (+0xB4: the least cosine so far, from 1), else the jump's
            if (F32(0x3E4CCCCD) < u)
            {
                float c = std::cos(RadJoy(pa) - pj->radB0);
                c = c < 0.0f ? 0.0f : (pj->gB4 < c ? pj->gB4 : c);
                pj->gB4 = c;
            }
            sv = c_svJump < pj->sB8 ? pj->gB4 * pj->sB8 + (1.0f - pj->gB4) * c_svJump : c_svJump;
        }
        else sv = c_svJump;
        sv *= pj->g38;
        glm::vec3 mv = cmm::SetVectorCylind(RadJoy(pa), u * sv, 0.0f);  // 0x11E1A0
        // +0x190: the move in the heading's frame, negated
        {
            const glm::mat3& m = pa->matHeading;  // (rows: glm columns)
            const glm::vec3 t((m[0].x * mv.x + m[0].y * mv.y) + m[0].z * mv.z, (m[1].y * mv.y + m[1].x * mv.x) + m[1].z * mv.z,
                              (m[2].z * mv.z + m[2].x * mv.x) + m[2].y * mv.y);
            pa->vecVel = -t;
        }
        if ((pj->state != 0 || pj->n40 == 3) && pj->f24 == 0)
        {
            glm::vec3 vh = VelActor(pa);
            vh.z = 0.0f;
            const float len = std::sqrt((vh.x * vh.x + vh.y * vh.y) + vh.z * vh.z);
            const glm::vec3 dir = len < F32(0x38D1B717) ? glm::vec3(1, 0, 0) : vh * (1.0f / len);
            const float d = (mv.x * dir.x + mv.y * dir.y) + mv.z * dir.z;  // 0x11E290
            const glm::vec3 perp = mv - dir * d;
            if (d < len && len * -0.25f < d) mv = cmm::LimitVectorLength(perp + dir * len, sv);  // 0x11E9F8
        }
        if (pj->f160) WarnOnce("the jump on a moving support (+0x160)");
        pj->vecA0 = mv;
    }
    if (pj->z3C < pos.z) pj->z3C = pos.z;
    // the Mega Jump: its second camera zone +0x170 from the top (the first one on, rising slower than 200)
    if (pj->f174 && pj->ploZone16C && pj->ploZone170 && pa->plo1024 == pj->ploZone16C && VelActor(pa).z < F32(0x43480000))
        SetActorZone(pa, pj->ploZone170);  // 0x140C88 (the zone's +0x134 on: natively the current zone)
}

// orig: 0x00196DF8 (vt slot 70's jump part: the air control toward +0xA0 in the velocity's frame, kept off the walls,
// a drag below -750, the paraglider (20) and Hover Pack (22))
void UpdateJumpMotion(JUMP* pj)
{
    ACTOR* pa = pj->pactor;
    const int st = pj->state;
    const unsigned k = static_cast<unsigned>(st - 6);
    if (k < 17 && k != 1 && k != 14 && k != 16) return;
    if (k == 14)
    {
        // the glide (state 20): the velocity in the actor's frame of the next step (its angular velocity applied)
        // smoothed toward (+0x1C0, 0, +0x1C4), the acceleration to it at most 8000, never downward
        const glm::vec3 q = VelActor(pa);
        glm::vec3 vl = VecJtConvert(nullptr, pa->palo, q);  // 0x1A6FC0
        const float dt = g_clock.dt;
        glm::vec3 w(0.0f), dw(0.0f);
        if (XF* pxf = PxfRot(pa->palo)) { w = glm::vec3(pxf->vel); dw = glm::vec3(pxf->aseg[3]); }  // (+0x5C: +0xB0 / +0xF0)
        const glm::vec3 v1 = w + (w + dw * dt);
        const glm::mat3 r = cmm::BuildRotateMatrixVector(v1 * (dt * 0.5f));  // 0x11B570
        const glm::mat3 m(MatJtLo(pa->palo));                                  // 0x1A6318
        const glm::mat3 a = cmm::MulMat3(m, r);   // the original's M R (rows)
        static const cmm::SMP s_smpX{800.0f, 0.0f, 0.1f}, s_smpY{800.0f, 0.0f, 0.1f}, s_smpZ{5000.0f, 0.0f, 0.1f};  // 0x2BF7A8..
        vl.x = cmm::GSmooth(vl.x, pj->svGlide1C0, dt, &s_smpX, nullptr);
        vl.y = cmm::GSmooth(vl.y, 0.0f, dt, &s_smpY, nullptr);
        vl.z = cmm::GSmooth(vl.z, pj->svGlide1C4, dt, &s_smpZ, nullptr);
        const glm::vec3 t = a * vl;
        glm::vec3 av = cmm::LimitVectorLength((t - q) * (1.0f / dt), F32(0x45FA0000));  // 8000 (D_002BF7D4)
        if (av.z < 0.0f) av.z = 0.0f;
        AccelAloPos(pa->palo, av);  // 0x1FAC30
        return;
    }
    if (k == 16)
    {
        // orig: 0x00196DF8 (Hover Pack): pursue the negated target and cancel
        // gravity through the same velocity spring (D_002BF7E0).
        static const CLQ s_clqHover{0.0f, 2.5f, 0.0f, 0.0f};
        const SOST* psost = Psost(pa->palo);
        const glm::vec3 gravity = psost ? psost->dvGravity : glm::vec3(0.0f);
        const glm::vec3 target = -pa->vecVel + gravity * (-1.0f / s_clqHover.u1);
        AccelAloTowardPosition(pa->palo, glm::vec3(0.0f), nullptr, target, &s_clqHover);
        return;
    }
    glm::vec3 v = VelActor(pa);
    v.z = 0.0f;
    const float len = 0.0f + std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
    const glm::vec3 dir = len < F32(0x38D1B717) ? glm::vec3(1, 0, 0) : v * (1.0f / len);
    const glm::mat3 r = BuildLookAtMatrixZ(dir);  // 0x11B7E0 (rows: glm columns)
    const glm::vec3& a = pj->vecA0;
    const glm::vec3 u((r[0].x * a.x + r[0].y * a.y) + r[0].z * a.z, (r[1].y * a.y + r[1].x * a.x) + r[1].z * a.z,
                      (r[2].z * a.z + r[2].x * a.x) + r[2].y * a.y);
    const float idt = 1.0f / g_clock.dt;
    float f2, f3;
    if (st == 0 || st == 2) f2 = f3 = idt;
    else
    {
        f3 = u.x < len ? F32(0x40600000) : F32(0x40200000);  // 3.5 / 2.5
        f2 = F32(0x40200000);
    }
    float sc;
    if (pj->kind == 12)
    {
        const float tt = g_clock.t - pj->t18;
        sc = -1.0f + tt * (2.0f + tt * 0.0f);  // 0x2BF800
        sc = sc < 0.0f ? 0.0f : (1.0f < sc ? 1.0f : sc);
    }
    else sc = pj->g34;
    const glm::vec3 w2 = glm::vec3(f3 * (u.x - len), f2 * u.y, 0.0f) * sc;
    glm::vec3 w1 = r * w2;  // (the frame's rows times w2)
    PushJumpOffWalls(pj, &w1);
    if (pj->state != 5)
    {
        const float vz = VelActor(pa).z;
        const float lz = F32(0xC43B8000);  // -750
        if (vz < lz) w1 += glm::vec3(0, 0, 1) * (F32(0xC0000000) * (vz - lz));
    }
    AccelAloPos(pa->palo, w1);
}

// (exported: the custom entry's probe point, +0x218)
glm::vec3 PosActorProbe0(ACTOR* pa) { return PosProbe0(pa); }
