// sly.cpp: Sly (sly.h). Ported from the reference's m_188488.cpp, m_189530_p3.cpp, m_18E540.cpp, m_13FF70.cpp and
// round4low_animation.cpp (0x18E540): oracle-checked or register-level ports of the original's 0x188488..0x191090
// and the actor's 0x13FF70..0x144E20.
#include "sly.h"
#include "sly3/ids3.h"
#include "routetele.h"
#include "climbsurf.h"
#include "slyfeet.h"
#include "gadget.h"
#include <algorithm>
#include "marker.h"
#include "cm.h"
void BroadcastTaskEvent(SW* psw, int n, LO* plo);  // orig 0x001619A0 (task.h)
#include "pocket.h"
#include "jtthrow.h"

#include "activate.h"
#include "actanim.h"
#include "interact.h"
#include "actmesh.h"
#include "actor.h"
#include "actorai.h"
#include "aipursuit.h"
#include "alo.h"
#include "asega.h"
#include "aseg.h"
#include "blend.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "lo.h"
#include "po.h"
#include "smrun.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "update.h"
#include "carry.h"
#include "transition.h"
#include "vecmat.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "xf.h"
#include "xfmath.h"
#include "dzg.h"
#include "jump.h"
#include "damage.h"
#include "target.h"
#include "binoc.h"
#include "gang.h"
#include "guard.h"
#include "aijobs.h"
#include "aihit.h"
#include "ainoise.h"
#include "sound.h"
#include "physworld.h"
#include "scpt.h"
#include "climb.h"
#include "crv.h"
#include "jt.h"
#include "mat.h"
#include "xfphys.h"
#include "animfind.h"

#include <cstring>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using cmm::F32;

void BindLoPartsTable(ALO* palo, int c, uint32_t vaTable);  // loaders.cpp: orig 0x1A64E8
void StartPlayerCounters(ACTOR* pa, bool fReset);  // orig 0x1440D0

bool FSlyHarnessOn();                       // slycheck.cpp
void CompareSlyFrame(SW* psw, int frame);
extern int g_iframeHarness;

uint32_t (*g_pfnGrfSlyXpImported)(ALO* palo) = nullptr;  // (physcheck.cpp: Sly's slot 85 from a reference RAM)

namespace
{
SW* s_psw = nullptr;
SLY* s_psly = nullptr;  // orig: global 0x2E1E40, assigned by Sly's constructor 0x188488

void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "sly: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

constexpr uint32_t BTN_CROSS = 0x40, BTN_SQUARE = 0x80, BTN_CIRCLE = 0x20, BTN_TRIANGLE = 0x10, BTN_R1 = 0x8;
constexpr uint32_t BTN_L3 = 0x200, BTN_R3 = 0x400;

PADCTL* PpadctlOfActor(ACTOR* p)
{
    PODATA* ppd = PpodataFromLo(p->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
PADCTL* PpadctlOf(SLY* p)
{
    PODATA* ppd = PpodataFromLo(p->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
float RadJoy(SLY* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->radJoy : 0.0f; }
float UJoy(SLY* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->uJoy : 0.0f; }
uint32_t GrfbtnHeld(SLY* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->grfbtn : 0; }
// orig 0x001C46C0 / 0x001C4660 on the actor's controller (+0x150)
bool FBtnTake(SLY* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlTakePressed(pc, grf, dt); }
bool FBtnWithin(SLY* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlPressedWithin(pc, grf, dt); }

LO* PloPart(ALO* palo, uint32_t ib)
{
    for (const auto& [ibPart, plo] : palo->apart)
        if (ibPart == ib) return plo;
    return nullptr;
}
template <class T>
T* PPart(ALO* palo, uint32_t ib)
{
    return dynamic_cast<T*>(PloPart(palo, ib));
}

float GDuration(const ASEG* paseg);  // below

// orig 0x002303A0 on the actor's position (+0x58) / rotation (+0x5C) records
void SetSlyXfModes(SLY* p, int mode)
{
    if (XF* pxf = PxfPos(p->palo)) SetXfMode(p->palo, pxf, mode);
    if (XF* pxf = PxfRot(p->palo)) SetXfMode(p->palo, pxf, mode);
}

}  // namespace

SLY* PslyGlobal() { return s_psw == g_psw ? s_psly : nullptr; }

// (the actor base's helpers, sly.h: also Bentley's / Murray's)
// orig: 0x00142868 / 0x00142880 (the look timer)
void ActorSetLookFlag(ACTOR* pa, int f)
{
    pa->fLook = f;
    pa->tLookNext = F32(0x7F7FFFFF);
}
void ActorNextLookTime(ACTOR* pa)
{
    const float a = pa->fLook ? pa->g1138 : pa->g1140;
    const float b = pa->fLook ? pa->g113C : pa->g1144;
    pa->tLookNext = g_clock.t + GRandInRange(a, b);  // 0x11D448
}

// orig: 0x001430C8 (the look animation's mask by the kind +0x10FC and the side of the target +0x10F8 in the
// actor's frame: ahead (|angle| < 1) 0, behind (> 2.5) 3, left 1, right 2; kind 1: 0x10 << side, 2: 0x100 << side,
// 3: 0x1000, 4: 0x100, else 8. Without a target the original's side is a leftover register: 0 here)
uint32_t GrfActorLookMask(ACTOR* pa)
{
    int n = 0;
    if (pa->ploLook)
    {
        const glm::vec3 v = PosJtConvert(nullptr, pa->palo, PosLoWorld(pa->ploLook));  // 0x1A6F00
        const float g = std::atan2(v.y, v.x);  // 0x2458F0
        const float a = std::fabs(g);
        if (a < 1.0f) n = 0;                   // 0x2BD2C0
        else if (2.5f < a) n = 3;              // 0x2BD2C4
        else if (0.0f < g) n = 1;
        else n = 2;
    }
    switch (pa->nLookKind)
    {
    case 1: return 0x10u << (static_cast<unsigned>(n) & 31);
    case 2: return 0x100u << (static_cast<unsigned>(n) & 31);
    case 3: return 0x1000;
    case 4: return 0x100;
    default: return 8;
    }
}

// orig: 0x001427C8 (the look target: kind and object; a new object while the look timer runs turns the look on and
// clears +0x1100 (0x142860))
void SetActorLookTarget(ACTOR* pa, int k, LO* plo)
{
    if (plo == pa->ploLook && k == pa->nLookKind) return;
    pa->nLookKind = k;
    pa->ploLook = plo;
    if (!plo) return;
    if (pa->tLookNext == F32(0x7F7FFFFF)) return;  // 0x2A4D60
    pa->fLook = 1;
    pa->f1100 = 0;
}

// orig: 0x001426E8 (from the post-loads: Sly 0x17, Bentley / Murray 0x16): the record of tag x (kind 6, 0x1F4C58),
// its rotation in the object's frame kept (+0x1150) and a fixed-value controller on it (0x12DC18: priority 3, mode
// 7, state 2, that rotation's Euler angles), taken out of the record's list until a look drives it (0x12D020)
void AttachActorLook(ACTOR* pa, int x)
{
    XF* pxf = nullptr;
    for (const auto& up : pa->palo->axf)
        if (up->tag == x && up->kind == 6) { pxf = up.get(); break; }
    if (!pxf) return;
    XF* pxfObj = PxfObj(pa->palo);
    const glm::mat3 a = glm::transpose(glm::mat3(pxfObj ? pxfObj->matWorld : glm::mat4(1.0f)));  // 0x11B340
    pa->mat1150 = a * glm::mat3(pxf->matWorld);
    pa->pctrlLook = PctrlNewPos(pa->palo, pxf, 3, 7, 2, glm::vec4(cmm::DecomposeEulerMatrix(pa->mat1150), 0.0f));
    EnsureCtrlRemoved(pa->pctrlLook);
}

namespace
{
float GFieldLo(const LO* plo, uint32_t ib, float g)
{
    uint32_t u;
    std::memcpy(&u, &g, 4);
    u = plo->U32Field(0, ib, u);
    std::memcpy(&g, &u, 4);
    return g;
}

// orig: 0x0011D978 (g clamped to the range [lo, hi])
float GClampLm(const float* lm, float g) { return g < lm[0] ? lm[0] : (lm[1] < g ? lm[1] : g); }

// orig: 0x00142AF8 (from the actor's update: the look's timer (on / off), the target chosen (the actor's target
// +0x10DC by its rank +0x10E4 against the interest points (cid 0x15 of the list 0x3E1474: in range +0xE8, height
// range +0xEC, at least that priority +0xF4) and the tracked target's aura (SW +0x4690, priority 2)), then the head
// turned toward it within its ranges by the controller +0x10F4)
void UpdateActorLook(ACTOR* pa)
{
    CTRLPOS* pq = pa->pctrlLook;
    if (!pq) return;
    int f = (pa->lmPan[0] < pa->lmPan[1] || pa->lmTilt[0] < pa->lmTilt[1]) ? 1 : 0;
    if (pa->nF04 != -1) f = 0;
    if (pa->tLookNext < g_clock.t && (pa->fLook != 0 || f))
    {
        pa->fLook = !pa->fLook;
        ActorNextLookTime(pa);  // 0x142880
    }
    if (pa->f1100 == 0)
    {
        // (D_002DD640, never set by the game: the look target cleared)
        LO* psBest = pa->plo10DC;
        int nBest = pa->n10E4;
        float gBest = F32(0x7F7FFFFF), g1 = 0.0f, g2 = 0.0f;  // 0x2A4D68
        if (psBest) LookScore(pa, psBest, &pa->ploLook, &gBest, &g2);
        if (g_psw)
            for (LO* ps : g_psw->mpcidaplo[0x15])
            {
                if (!ps || !FLoActive(ps)) continue;
                const int nPrio = static_cast<int>(ps->U32Field(0, 0xF4, 1));
                if (nPrio < nBest) continue;
                LookScore(pa, ps, &pa->ploLook, &g1, &g2);
                if (g1 > GFieldLo(ps, 0xE8, 2000.0f)) continue;
                const float lo = GFieldLo(ps, 0xEC, -800.0f), hi = GFieldLo(ps, 0xF0, 800.0f);
                if (!(lo <= g2 && g2 <= hi)) continue;  // 0x11D910
                if (nBest < nPrio || (nPrio == nBest && g1 < gBest))
                {
                    psBest = ps;
                    gBest = g1;
                    nBest = nPrio;
                }
            }
        if (g_jttrack.c > 0 && nBest < 3)
            if (LO* ps = g_jttrack.ae[0].ploAura)
            {
                float g3 = 0.0f, g4 = 0.0f;
                LookScore(pa, ps, &pa->ploLook, &g3, &g4);
                if (nBest < 2 || (nBest == 2 && g3 < gBest))
                {
                    psBest = ps;
                    nBest = 2;
                    gBest = g3;
                }
            }
        SetActorLookTarget(pa, nBest, psBest);
        static const bool s_fDbg = std::getenv("SLY2N_ACTORLOOK") != nullptr;
        if (s_fDbg)
            std::printf("actorlook %d: t %.3f look %d kind %d on %d next %.2f mask 0x%X\n", pa->palo->oid, g_clock.t,
                        pa->ploLook ? pa->ploLook->oid : -1, pa->nLookKind, pa->fLook, pa->tLookNext, GrfActorLookMask(pa));
    }
    float pan = 0.0f, tilt = 0.0f, g = 0.0f;
    if (pa->f1104 != 0)
    {
        EnsureCtrlAdded(pq);  // 0x12CFE0
        pan = pa->g1110;
        tilt = pa->g1114;
        g = pa->g1118;
    }
    else
    {
        if (!pa->ploLook || pa->fLook == 0 || !f)
        {
            EnsureCtrlRemoved(pq);  // 0x12D020
            return;
        }
        EnsureCtrlAdded(pq);
        const glm::vec3 v = PosJtConvert(nullptr, pa->palo, PosLoWorld(pa->ploLook));  // 0x1A6F00
        pan = RadNormalize(std::atan2(v.y, v.x));  // 0x11E958
        tilt = RadNormalize(std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)));
        pan = GClampLm(pa->lmPan, pan);
        tilt = GClampLm(pa->lmTilt, tilt);
        const float t = pa->lmPan[1] == pa->lmPan[0] ? 0.5f : (pan - pa->lmPan[0]) / (pa->lmPan[1] - pa->lmPan[0]);
        const float u = t * std::fabs(t);
        g = (1.0f - u) * pa->g1130 + u * pa->g1134;
    }
    XF* pxfObj = PxfObj(pa->palo);
    XF* pxfHead = pq->pxf;
    const glm::mat3 m0 = cmm::BuildPanTiltMatrix(pan, tilt) * cmm::BuildRotateMatrixAxis(g, glm::vec3(1.0f, 0.0f, 0.0f));
    glm::mat3 m1 = MatConvertXfs(nullptr, pxfObj, glm::mat3(pxfHead->matWorld));  // 0x22FD38
    m1 = m1 * glm::transpose(pa->mat1150);
    glm::mat3 m2;
    if (m0[0][0] + m1[0][0] < 0.0f && m0[0][1] * m1[0][1] < 0.0f) m2 = pa->mat1150;
    else m2 = m0 * pa->mat1150;
    m2 = MatConvertXfs(pxfObj, pxfHead->pxfRef, m2);
    SetCtrlPos(pq, glm::vec4(cmm::DecomposeEulerMatrix(m2), 0.0f));  // 0x11BFF0 / 0x12D108
}
}  // namespace

// orig: 0x00143C78 (slot 85's kind for actors)
uint32_t GrfActorStance(ACTOR* pa)
{
    if (pa->fNoContacts) return 0;
    if (pa->n4C8 == 3 || pa->n4C8 == 4) return 4;
    if (pa->jump.state != -1) return 1;
    switch (pa->nSubDF4)
    {
    case -1: case 0: case 6: return 7;
    case 4: case 7: case 8: case 12: return 1;
    default: return 0;
    }
}

namespace
{
float GDuration(const ASEG* paseg) { return paseg ? PasegdOf(const_cast<ASEG*>(paseg))->tMax : 0.0f; }  // +0x74
}

// orig: 0x00140AF8 (the target +0x10E8 of kind 4 (+0x10EC) within reach: horizontally closer than 1000 (1700 for a
// cid 0x3A), height within +-750; fWide: 500 further, +-1250; never in the gait 3 (+0x10E4))
bool FActorTargetInReach(ACTOR* pa, bool fWide)
{
    if (pa->n10EC != 4 || pa->n10E4 == 3 || !pa->plo10E8) return false;
    float gDist = FIsLoDerivedFrom(pa->plo10E8, 0x3A) ? 1700.0f : 1000.0f;
    float gMin = -750.0f, gMax = 750.0f;
    if (fWide)
    {
        gDist += 500.0f;
        gMin -= 500.0f;
        gMax += 500.0f;
    }
    if (!FIsLoDerivedFrom(pa->plo10E8, Icid(CID3::ALO))) return false;
    const glm::vec3 d = PosAloCenter(static_cast<ALO*>(pa->plo10E8)) - PosAloCenter(pa->palo);
    return std::sqrt(d.x * d.x + d.y * d.y) < gDist && gMin < d.z && d.z < gMax;
}

// orig: 0x0013FF70 (the actor's init: the base 0x203948 (stepphys.h's defaults), the sub-objects, the defaults)
void InitActorFields(ACTOR* pa)
{
    // (the fields' defaults are the struct's: 0x103C 20, 0x1048 125, 0x10D4 1, 0x1138.. 1.5 / 2.5 / 3 / 5, 0x12F0 1,
    // 0x1020 100, 0x1300 100, 0x1304 -60, 0x10C8 / 0x10EC / 0x10E4 / 0x1318 -1, 0x10F0 1, 0x10D8 1)
    // the jump (0x195950)
    InitJump(&pa->jump, pa);
    InitDamage(pa);  // 0x1A0B18 (+0xDF0)
    // orig: 0x13E818 field options 726/728 are loaded after the constructor in the original. Native actor
    // initialization runs after BRX loading, so restore those authored capacities after the damage init.
    if (pa->palo)
    {
        SetDmgCapacity(pa, int32_t(pa->palo->U32Field(0, 0xDFC, uint32_t(DmgCapacityOf(pa)))));
        pa->n1300 = int32_t(pa->palo->U32Field(0, 0x1300, uint32_t(pa->n1300)));
        pa->f10F0 = int32_t(pa->palo->U32Field(0, 0x10F0, uint32_t(pa->f10F0)));  // (EOPID 725, a loaded field)
    }
    InitBinoc(pa);   // 0x19CBF8 (+0xF00)
    InitClimb(pa);   // 0x19DE10 (+0xF50)
    InitInteract(pa);  // 0x191090 (+0x4C0)
    InitJtThrow(pa);    // 0x21F2E0 (+0x11B0)
    // TODO(actors): the other sub-objects' inits (0x191090 +0x4C0, 0x19F3E8 +0x9E0, 0x1A0B18 +0xDF0, 0x19CBF8 +0xF00,
    // 0x144638 +0xFD0)
    // the contact bytes (0x1F7918): all 0, then 8 for the filters 0x10FF00 (bits 8..15 and 20: the hits, the kill
    // volumes 0x4000, ...); the sensors whose filter matches them post their contacts to the actor's slot 75
    if (pa->palo)
    {
        SetAloXpBytes(pa->palo, 0x3FFFFFF, 0);
        SetAloXpBytes(pa->palo, 0x10FF00, 8);
    }
}

// orig: 0x001400B8's 0x1A64E8: bind before the posted 0x143BD8 can restore carried booty.
void BindActorPartTable(ACTOR* pa)
{
    if (pa->fPartTableBound) return;
    BindLoPartsTable(pa->palo, Icid(CID3::JT), 0x2BC9F0);
    pa->fPartTableBound = true;
}

// orig: 0x001400B8 (the actor's post-load: its parts table; TODO(actors): the sub-objects' post-loads 0x1959C0 /
// 0x1910F8 / 0x19F3F0 / 0x1A1098 / 0x19CC30 / 0x21F388 / 0x19DE58, the linked objects, 0x141EF8, 0x1F8278)
void PostLoadActorParts(ACTOR* pa)
{
    BindActorPartTable(pa);
    pa->psm102C = PPart<SM>(pa->palo, 0x102C);
    PostLoadBinoc(pa);  // 0x19CC30 (+0xF00's window part; the clips +0xF0C / +0xF10)
    PostLoadClimb(pa);  // 0x19DE58 (+0xF50's clips)
    PostLoadInteract(pa);  // 0x1910F8 (+0x4C0's)
    PostLoadJump(&pa->jump);  // 0x1959C0 (the paraglider's parts and blend)
    PostLoadJtThrow(pa);      // 0x21F388 (+0x11B0's authored animation blends)
    StartPlayerCounters(pa, (g_transition.req.flags & 4) != 0);  // orig 0x1400B8 -> 0x1440D0, after actor init
}
// (its end: the mass, 0x1F35C8)
void PostLoadActorMass(ACTOR* pa) { SetAloMass(pa->palo, 0.25f); }

// orig: 0x001371B8 (the owner is implicit in the containing SLY)
void InitSlyDodge(SLY* p)
{
    p->dodge.state = -1;
}

// orig: 0x001371C8 (slot 88 returns the player to standing when the main clip ends)
void UpdateSlyDodgeTransitions(SLY* p)
{
    if (FAsegaRetracted(p->pasega)) SetSlyState(p, 0, -1);
}

// orig: 0x00137218 (0x2046F0: face the existing target)
void UpdateSlyDodgeInput(SLY* p)
{
    if (p->plo10DC)
        SetStepPhysHeadingToward(p->palo, PosAloCenter(static_cast<ALO*>(p->plo10DC)) - PosAloCenter(p->palo));
}

// orig: 0x001374F0 (the stick side relative to the target; absent input retains the last side)
int SelectSlyDodgeSide(SLY* p)
{
    if (0.0f < UJoy(p) && p->plo10DC)
    {
        const float rad = RadJoy(p) - F32(0x3FC90FDB);
        const glm::vec3 side = cmm::SetVectorCylind(rad, 1.0f, 0.0f);  // 0x11E1A0
        const glm::vec3 target = PosAloCenter(static_cast<ALO*>(p->plo10DC)) - PosAloCenter(p->palo);
        return 0.0f < glm::dot(side, target) ? 0 : 1;
    }
    return p->dodge.lastSide;
}

// orig: 0x001375D0 (authored initial velocity, desired root motion, and main animation)
void SetSlyDodgeState(SLY* p, int state)
{
    if (state == p->dodge.state) return;
    p->dodge.state = state;
    p->dodge.tState = g_clock.t;
    if (state == -1)
    {
        SetAloVelocityTo(p->palo, VelAloPos(p->palo) * 0.5f);  // D_002BC9B8
        return;
    }
    ASEG* paseg = nullptr;
    if (state >= 0 && state < 2)
    {
        paseg = p->dodge.apaseg[state];
        UpdateSlyDodgeInput(p);
        glm::vec4 root(0.0f);
        if (const ASEGCHN* pchn = paseg ? PchnFindAseg(paseg, p->palo, 1, true) : nullptr; pchn && pchn->ptrkp)
            pchn->ptrkp->Eval(nullptr, &root, 0.0f, paseg->svt);
        SetAloVelocityTo(p->palo, p->matHeading * glm::vec3(root));
        p->vecVel = -glm::vec3(root);
        p->dodge.lastSide = state;
    }
    if (p->psma)
    {
        RetractSma(p->psma);  // 0x1F15B8 on +0x1034
        p->psma = nullptr;
    }
    ApplyAsegCur(paseg, p->palo, 0.0f, 1.0f, 1, &p->pasega);
}

// orig: 0x00137248 (relative radial correction and tangent rotation around the target)
void UpdateSlyDodgeMotion(SLY* p)
{
    if (!p->plo10DC) return;
    ALO* target = static_cast<ALO*>(p->plo10DC);
    const float maxAcceleration = 1500.0f * 1500.0f / 250.0f + 1500.0f;
    const glm::vec3 relative = VelAloPos(p->palo) - VelAloPos(target);
    glm::vec3 normal = PosAloCenter(p->palo) - PosAloCenter(target);
    normal.z = 0.0f;
    const float distance = std::sqrt((normal.x * normal.x + normal.y * normal.y) + normal.z * normal.z);
    normal = distance < F32(0x38D1B717) ? glm::vec3(1.0f, 0.0f, 0.0f) : normal * (1.0f / distance);
    const glm::vec3 cross = glm::cross(normal, relative);
    const float radialSpeed = glm::dot(relative, normal);
    const float omega = cross.z / distance;
    static const cmm::SMP smoothing{500.0f, 0.0f, 0.5f};  // D_002BC9A8
    float desiredSpeed;
    cmm::GSmooth(distance, 250.0f, g_clock.dt, &smoothing, &desiredSpeed);
    const float correction = cmm::GLimitAbs(desiredSpeed - radialSpeed, 1500.0f * g_clock.dt);
    const glm::vec3 next = MatRotateAxis(omega * g_clock.dt, glm::vec3(0.0f, 0.0f, 1.0f)) *
                           (relative + normal * correction);
    const glm::vec3 acceleration = (next - relative) * (1.0f / g_clock.dt);
    AccelAloPos(p->palo, cmm::LimitVectorLength(acceleration, maxAcceleration));
}

// orig: 0x0018C120 (the seven source controllers, in mask-bit order)
void SetSlyHandles(SLY* p, uint32_t mask)
{
    p->slide.grfHandles = mask;
    if (!p->slide.aphandle[0]) return;
    for (int i = 0; i < 7; ++i)
        if (CTRLSOURCE* handle = p->slide.aphandle[i])
        {
            if (mask & (1u << i)) EnsureCtrlAdded(handle);
            else EnsureCtrlRemoved(handle);
        }
}

// orig: 0x0018A8C8 (state 24's branch, after the common floor-loss test)
void UpdateSlySlideTransitions(SLY* p)
{
    if (p->f1D30) { SetSlyState(p, 17, -1); return; }
    if (!(0 < NActorPower(p))) { SetSlyState(p, 0, -1); return; }
    if (FBtnTake(p, BTN_CROSS, 0.1f)) { SetJumpKind(&p->jump, 1, 0, 0); return; }
    if (!(GrfbtnHeld(p) & GrfAbilityButton(p, 25))) { SetSlyState(p, 0, -1); return; }
    if (p->substate == 13 && glm::dot(VelAloPos(p->palo), glm::vec3(PxfObj(p->palo)->matWorld[0])) < 150.0f)
        SetSlyState(p, 0, -1);
}

// orig: 0x0018B7E8 (state 24's drain, alignment and three-way lean blend)
void UpdateSlySlideInput(SLY* p)
{
    DrainActorPower(p, 1.0f);
    if (p->substate == 12)
    {
        if (std::fabs(cmm::RadNormalize(RadStepHeading(p) - p->radHeading)) < 0.4f)
            SetSlyState(p, 24, 13);
    }
    else SetStepPhysHeading(p->palo, RadJoy(p));
    const float lean = 0.5f * cmm::GLimitAbs(AngVelAlo(p->palo).z / (p->vBrake.x * 0.1f), 1.0f);
    if (ASEGBL* blend = p->slide.pbl)
    {
        blend->abl[0].weight = std::max(0.0f, lean);
        blend->abl[1].weight = 1.0f - std::fabs(lean);
        blend->abl[2].weight = std::max(0.0f, -lean);
        RecalcBlend(blend);
    }
}

// orig: 0x00188960 (state 24's addition, following the base STEP motion)
void UpdateSlySlideMotion(SLY* p)
{
    if (p->substate != 13) return;
    const glm::vec3 velocity = VelAloPos(p->palo);
    const glm::vec3 omega = AngVelAlo(p->palo);
    const glm::vec3 forward(PxfObj(p->palo)->matWorld[0]);
    const float along = glm::dot(velocity, forward);
    const glm::vec3 perpendicular = velocity - forward * along;
    const glm::vec3 next = MatRotateAxis(omega.z * g_clock.dt, glm::vec3(0.0f, 0.0f, 1.0f)) *
        (forward * (along - 100.0f * g_clock.dt) + perpendicular * (1.0f - 1.0f * g_clock.dt));
    glm::vec3 acceleration = (next - velocity) * (1.0f / g_clock.dt);
    acceleration.z = 0.0f;
    AccelAloPos(p->palo, cmm::LimitVectorLength(acceleration, 4500.0f));
}

// orig: 0x00188488 (Sly's init)
void InitSlyFields(SLY* p)
{
    InitActorFields(p);
    p->substate = -1;
    p->g19AC = 15.0f;
    p->state = -1;
    p->mode = -1;
    InitGait(&p->gaitWalk, p->palo, 0x6F);
    InitGait(&p->gaitRun, p->palo, 0x6F);
    SetStepPhysScale(p->palo, F32(0x3FCCCCCD));  // 1.6
    p->n1000 = 1;
    p->g1020 = 200.0f;
    InitSlyDodge(p);  // 0x1371B8 (+0x1FF0)
    // orig: 0x188488 initializes the four pocket identities before parts bind.
    const int pocketIds[4] = {26, 37, 38, 41};
    for (int i = 0; i < 4; ++i) p->pockets[i] = SLY::POCKET{pocketIds[i]};
    // TODO(actors): +0x1098.. (the reach)
    s_psly = p;
}

// orig: 0x00188608's 0x137A30(p, 0xC, 4, +0x1F60, .., +0x1F90): state 22's blend of four (the clips at +0x1F64 + 12 i)
void BindSlyHangBlend(SLY* p)
{
    std::vector<BLENDBL> abl(4);
    for (int i = 0; i < 4; ++i) abl[i].paseg = PPart<ASEG>(p->palo, 0x1F64 + 12 * i);
    if (!abl[0].paseg) return;
    p->pblHang = NewBlend(p->palo, std::move(abl), {});
}

// orig: 0x0018F908 (0x137A30(p, 0xC, 3, +0x1970, .., +0x1994): state 3's blend of three (the clips at +0x1974 + 12 i)),
// and state 3's SM +0x1950 (the parts table)
void BindSlySwingBlend(SLY* p)
{
    p->psmHang = PPart<SM>(p->palo, 0x1950);
    std::vector<BLENDBL> abl(3);
    for (int i = 0; i < 3; ++i) abl[i].paseg = PPart<ASEG>(p->palo, 0x1974 + 12 * i);
    if (!abl[0].paseg) return;
    p->pblSwing = NewBlend(p->palo, std::move(abl), {});
}

// orig: 0x001400B8 / 0x00188608 (the post-loads: the parts, the gaits, the first state)
void PostLoadSly(SLY* p)
{
    ALO* palo = p->palo;
    PostLoadActorParts(p);                   // 0x1400B8
    BindLoPartsTable(palo, 0x12, 0x2BF1B0);  // 0x188608's
    p->psmIdle = PPart<SM>(palo, 0x1028);
    p->plo19A0 = PloPart(palo, 0x19A0);
    p->plo19A4 = PloPart(palo, 0x19A4);
    p->plo19A8 = PloPart(palo, 0x19A8);
    p->dodge.apaseg[0] = PPart<ASEG>(palo, 0x2000);
    p->dodge.apaseg[1] = PPart<ASEG>(palo, 0x2004);
    for (int i = 0; i < 4; ++i)
        p->pockets[i].proxy = PloPart(palo, 0x1FB4 + 12 * i);
    // the gaits (0x20B868): walk +0x1334 / +0x1374, run +0x1C44 / +0x1C84
    p->gaitWalk.apaseg[0] = PPart<ASEG>(palo, 0x1334);
    p->gaitWalk.apaseg[1] = PPart<ASEG>(palo, 0x1374);
    p->gaitRun.apaseg[0] = PPart<ASEG>(palo, 0x1C44);
    p->gaitRun.apaseg[1] = PPart<ASEG>(palo, 0x1C84);
    BindGait(&p->gaitWalk);
    BindGait(&p->gaitRun);
    // the root blends (0x1A0300): state 5's walk around the target, state 10's sneak
    BindActorRootBlend(&p->rb13D0, palo, 0x13D0);
    BindActorRootBlend(&p->rb1AC0, palo, 0x1AC0);
    BindActorRootBlend(&p->rb1E10, palo, 0x1E10);
    // orig: 0x00188608: priority-five hand/root sources and priority-seven foot sources.
    // A source follows the next animation controller and applies only its local offset.
    const int handleTags[7] = {0x138, 0xD, 0xC, 0x13, 0x12, -1, 0xC};
    const int handleKinds[7] = {1, 1, 1, 1, 1, 1, 6};
    for (int i = 0; i < 7; ++i)
    {
        ALO* owner = palo;
        XF* record = nullptr;
        if (i == 5)
        {
            if (p->plo19A0 && FCidDerivedFrom(p->plo19A0->Cid(), Icid(CID3::ALO)))
            {
                owner = static_cast<ALO*>(p->plo19A0);
                record = PxfPos(owner);
            }
        }
        else
            for (const auto& xf : palo->axf)
                if (xf->tag == handleTags[i] && xf->kind == handleKinds[i]) { record = xf.get(); break; }
        if (record)
        {
            p->slide.ahandleOwned[i].reset(PctrlNewSource(owner, record, i == 3 || i == 4 ? 7 : 5));
            p->slide.aphandle[i] = p->slide.ahandleOwned[i].get();
        }
    }
    std::vector<BLENDBL> slideParts(3);
    for (int i = 0; i < 3; ++i)
    {
        p->slide.apaseg[i] = PPart<ASEG>(palo, 0x2018 + i * 12);
        slideParts[i].paseg = p->slide.apaseg[i];
    }
    p->slide.pbl = NewBlend(palo, std::move(slideParts), {});  // 0x137A30, original stride 12/count 3
    // the jump's segments (bound by the load 0x1885B8's table 0x2BE990 at +0x3E0..+0x41C)
    for (int i = 0; i < 5; ++i)
    {
        p->jump.apasegJump[i] = PPart<ASEG>(palo, 0x3E0 + 4 * i);
        p->jump.apasegFall[i] = PPart<ASEG>(palo, 0x3F4 + 4 * i);
    }
    p->jump.pasegLand148 = PPart<ASEG>(palo, 0x408);
    p->jump.pasegLand14C = PPart<ASEG>(palo, 0x40C);
    p->jump.pasegLand154 = PPart<ASEG>(palo, 0x414);
    p->jump.pasegLand158 = PPart<ASEG>(palo, 0x418);
    p->jump.pasegLand15C = PPart<ASEG>(palo, 0x41C);
    // TODO(actors): 0x1400B8's linked objects (class 0x401), slot 104, 0x141EF8, 0x1440D0, 0x1F8278; 0x188608's
    // part handles (0x12DBB0), root blends (0x1A0300), 0x18F908, 0x137A30's mode blends, 0x1CC5E8 (+0x17D0)
    BindReach(&p->reach, p, &p->pasegaMode, 0x17DC, 0x17F0, p->plo19A4);  // 0x1CC5E8 (+0x17D0)
    AttachActorLook(p, 0x17);  // 0x1426E8
    LoadPickpocket(p, PPart<ASEG>(palo, 0x1BC8));  // 0x18B348 (0x188810)
    BindSlyHangBlend(p);      // 0x137A30(+0x1F60, 4, +0x1F90)
    BindSlySwingBlend(p);     // 0x18F908(+0x1970, +0x1994)
    PostLoadActorMass(p);     // 0x1400B8's end
    SetSlyState(p, 0, -1);
    SetLoSt(s_psw, palo, 1);  // 0x1A6CA8 (always active)
    // the paraglider's SM: 0x1F0380(+0x1D24, +0x1D20, -1, 1)
    if (SM* psm = PPart<SM>(palo, 0x1D24))
        if (LO* plo = PloPart(palo, 0x1D20); plo && FIsLoDerivedFrom(plo, Icid(CID3::ALO)))
            p->psma1D28 = PsmaApplySm(psm, static_cast<ALO*>(plo), -1, 1);
}

int32_t* PnProgressSlot(const std::vector<std::string>& apath);  // savedata.h (0x1D8C10)

void InitWorldActors(SW* psw)
{
    s_psw = psw;
    s_psly = nullptr;
    for (CID cid = Icid(CID3::JT); cid < g_acls[Icid(CID3::JT)].cidLast; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            ALO* palo = static_cast<ALO*>(plo);
            StepPhysOf(palo);  // (made with 0x203948's defaults when the physics' post-load did not)
            SLY* p = PslyFromLo(plo);
            if (!p) continue;
            InitSlyFields(p);
            PostLoadSly(p);
        }
}

// orig: 0x001440D0 (the players' start: the counters are the progress "<player>.hpCur" (+0xE00, the damage's) and
// "mpCur" (+0x12FC, the gadgets' power), the player's name by vt slot 103 (0x18DE08 sly / 0x136CB0 bentley / 0x1B9B60
// murray); at 0 health (a death: the job restarted at its checkpoint) or fReset: the health to the capacity (0x144188:
// min(n, +0xDFC)) and the power to its maximum (0x1441C0: min(n, +0x1300 = 100)), +0x1308 = 0)
void StartPlayerCounters(ACTOR* pa, bool fReset)
{
    const char* psz = FIsLoDerivedFrom(pa->palo, Icid(CID3::JT)) ? "sly" : FIsLoDerivedFrom(pa->palo, Icid(CID3::BENTLEY)) ? "bentley"
                    : FIsLoDerivedFrom(pa->palo, Icid(CID3::MURRAY)) ? "murray" : nullptr;
    if (!psz) return;
    if (int32_t* pn = PnProgressSlot({psz, "hpCur"})) SetDmgCounter(pa, pn);
    int32_t* pnMp = PnProgressSlot({psz, "mpCur"});
    int* pc = DmgCounterOf(pa);
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("players' start: %s health %d / %d power %d\n", psz, pc ? *pc : -1, DmgCapacityOf(pa), pnMp ? *pnMp : -1);
    if (!pc || !(*pc == 0 || fReset)) return;
    *pc = DmgCapacityOf(pa);                // 0x144188(capacity)
    const int nMpMax = pa->n1300;           // orig 0x1440D0 reads the actor's actual maximum
    if (pnMp) *pnMp = nMpMax;               // 0x1441C0(+0x1300)
    if (std::getenv("SLY2N_HITDEBUG")) std::printf("players' start: %s refilled: health %d power %d\n", psz, *pc, pnMp ? *pnMp : -1);
}

// Debug (F7, not in the original): full health and power for the three players.
void CheatRefillPlayers(SW* psw)
{
    for (const auto& plo : psw->aploAll)
    {
        ACTOR* pa = PactorFromLo(plo.get());
        if (!pa || !pa->palo) continue;
        const char* psz = FIsLoDerivedFrom(pa->palo, Icid(CID3::JT)) ? "sly" : FIsLoDerivedFrom(pa->palo, Icid(CID3::BENTLEY)) ? "bentley"
                        : FIsLoDerivedFrom(pa->palo, Icid(CID3::MURRAY)) ? "murray" : nullptr;
        if (!psz) continue;
        if (int* pc = DmgCounterOf(pa)) *pc = DmgCapacityOf(pa);
        if (int32_t* pnMp = PnProgressSlot({psz, "mpCur"})) *pnMp = 100;
    }
}

// orig: 0x0012F990 (the time of the segment's event 11 with id n; 0 when none)
float GEventAseg11(ASEG* paseg, int n)
{
    ASEGD* pd = paseg ? PasegdOf(paseg) : nullptr;
    if (!pd) return 0.0f;
    for (const auto& aev : pd->aaev)
        for (const ASEGEV& ev : aev)
            if (ev.kind == 11 && ev.an[0] == n) return ev.t;
    return 0.0f;
}

// orig: 0x0018E540 (the attack mode n, -1 none): the cane's entry off unless the new mode continues the old one
// (0x2BF448), the mode's clip played on +0x18E0 from t / at svt (a continuation from where the old one is), its end
// time +0x19B0. Ported: -1, the cane swings 12..21 (+0x1CE0..), 22 / 23 (+0x1D14 / +0x1D18); TODO(actors): 0 (the
// +0x17D0 controller), 1 / 2 / 10 / 11 (the gadgets: the parts' swap 2 / 11), 3..7 (the +0x1A04 blends), 8 / 9, the
// effects (0x1FBB90: 15 / 11), the exit effect of mode 1
void SetSlyMode(SLY* p, int n)
{
    const int old = p->mode;
    if (old == n) return;
    static const int s_amodeNext[24] = {-1, -1, -1, -1, 6, 7, 4, 5, -1, -1, -1, -1, 0xE, 0xF, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    const bool fCompatible = n != -1 && old != -1 && old >= 0 && old < 24 && s_amodeNext[old] == n;
    ALO* palo = p->palo;
    if (!fCompatible)
        for (SOE& e : palo->asoe)
            if (e.tag == 332) { SetSoeOff(palo, &e); break; }  // 0x1F2AD0(+0x1A88: the cane's entry)
    if (old == 1 || old == 2) MarkAloDirty(palo);  // 0x1F4BF8
    if (old == 0) SetReachState(&p->reach, -1);    // 0x1CCAC0
    // (leaving 0: the reach controller off, 0x1CCAC0(+0x17D0, -1); the effect 0x1FBB90(old's) at the actor)
    {
        const bool fBack = old == 2 || old == 11, fBackNew = n == 2 || n == 11;
        // the cane's two parts: +0x19A0 in the hand, +0x19A8 the climbs' (mode 11) and 2's: the new one added (vt
        // slot 8), placed where the old one is (slot 38: 0x1F49C8), the old one removed (slot 9)
        if (fBack != fBackNew && g_psw)
        {
            LO* pOn = fBackNew ? p->plo19A8 : p->plo19A0;
            LO* pOff = fBackNew ? p->plo19A0 : p->plo19A8;
            if (pOn) AddLo(g_psw, pOn);
            if (pOn && pOff) TeleportActor(pOn, pOff);
            if (pOff) RemoveLo(g_psw, pOff);
        }
    }
    float tStart = 0.0f, svt = 1.0f, dtSeek = 0.0f;
    ASEG* paseg = nullptr;
    switch (n)
    {
    case -1: p->plo18C0 = nullptr; break;  // orig: 0x18E540
    case 0:
        // the cane reaching for the jump's target (+0x1854): the reach controller +0x17D0 (0x1CCAC0(0); TODO(actors):
        // its aim of the cane by the clip's blend, 0x1CC2E8), the clip +0x1800 held at its middle
        SetReachState(&p->reach, 0);  // 0x1CCAC0
        paseg = p->reach.pbl;          // (+0x1800)
        tStart = GDuration(paseg) * 0.5f;
        svt = 0.0f;
        p->t19B0 = g_clock.t + F32(0x3DCCCCCD);  // 0.1
        MarkAloDirty(palo);                       // 0x1F4BF8
        break;
    case 1: case 2: MarkAloDirty(palo); break;
    case 10: MarkAloDirty(palo); paseg = PPart<ASEG>(palo, 0x14D4); break;
    case 11: MarkAloDirty(palo); paseg = PPart<ASEG>(palo, 0x14D8); break;
    case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21:
    case 22: case 23:
        dtSeek = F32(0x3D888889);                          // 0x2BF43C
        p->t19B0 = g_clock.t + F32(0x3E088889);           // 0x2BF440
        p->f18EC = n < 22 ? 1 : 0;
        paseg = PPart<ASEG>(palo, n < 22 ? 0x1CE0 + 4 * static_cast<uint32_t>(n - 12) : (n == 22 ? 0x1D14 : 0x1D18));
        if ((n < 14 || n >= 22) && p->state == 0 && p->psmIdle && IsmsFindSm(p->psmIdle, 0x17B) >= 0 && p->psma)
            SetSmaState(p->psma, 0x17B);  // 0x1F0400 / 0x1F09D8
        break;
    default:
        WarnOnce("SetSlyMode: mode " + std::to_string(n));
        break;
    }
    if (fCompatible && p->pasegaMode && paseg)
    {
        tStart = std::min(p->pasegaMode->t, GDuration(paseg));
        svt = p->pasegaMode->svt;
    }
    else p->f18E4 = 0;
    p->mode = n;
    p->cq = 0;  // (+0x1880)
    p->tMode = g_clock.t;
    ApplyAsegCur(paseg, palo, tStart, svt, 1, &p->pasegaMode);  // 0x1305B8
    if (std::getenv("SLY2N_MODEPRINT"))
        std::printf("sly mode %d -> %d: clip %d (%.3f s) t %.3f svt %.3f compatible %d\n", old, n, paseg ? paseg->oid : -1, GDuration(paseg),
                    tStart, svt, fCompatible ? 1 : 0);
    if (dtSeek != 0.0f && p->pasegaMode) SetAsegaSeekTime(p->pasegaMode, g_clock.t + dtSeek);  // 0x131A90
}

namespace
{
// orig: 0x0018E1E0 (the upgraded contextual strike's velocity toward the target, in the heading's frame)
void UpdateSlyJuggleVelocity(SLY* p)
{
    const glm::vec3 d = PosAloCenter(static_cast<ALO*>(p->plo10DC)) - PosAloCenter(p->palo);
    const float len = std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
    const glm::vec3 dir = len < F32(0x38D1B717) ? glm::vec3(1.0f, 0.0f, 0.0f) : d * (1.0f / len);
    SetStepPhysHeadingToward(p->palo, dir);  // 0x2046B0
    const cmm::SMP smp{1000.0f, 0.0f, 0.5f};  // 0x2BF418
    float speed;
    cmm::GSmooth(len, 200.0f, g_clock.dt * 0.5f, &smp, &speed);  // 0x11D018 / 0x2BF414
    glm::mat3 m = glm::transpose(p->matHeading);  // 0x11B340
    m[0] *= speed;
    m[1] *= speed;
    m[2] *= speed;
    p->vecVel = m * dir;
}
}

// orig: 0x00143220 / 0x0018DE80 (slot 96: horizontal velocity predicts the launch's owner position)
void GetSlyLaunchTarget(SLY* p, float dt, LAUNCHTARGET* ptarget)
{
    glm::vec3 vel = VelAloPos(p->palo);
    vel.z = 0.0f;
    const glm::vec3 pos = PosAloCenter(p->palo);
    ptarget->vel = vel;
    ptarget->sRadius = 150.0f;
    ptarget->dz = 75.0f;
    ptarget->pos = pos + vel * dt;
    ptarget->fActive = 0;
    switch (p->state)
    {
    case 0: case 1: case 5: case 10: case 12: ptarget->fActive = 1; break;
    default: break;
    }
}

// orig: 0x0018C250 (the state change: leave the old state, enter the new one, the substate, the animation)
void SetSlyState(SLY* p, int n, int m)
{
    ALO* palo = p->palo;
    ASEG* paseg = nullptr;  // s4
    SM* psm = nullptr;      // sp+0x58
    int oidState = -1;      // s7
    uint32_t grfMask = 0xFFFC;
    int grfapl = 1;         // fp
    float tStart = 0.0f, svt = 1.0f;  // f22 / f23
    uint32_t grfHandles = 0;          // s6
    bool fKeepAnim = false;           // sp+0x60
    if (std::getenv("SLY2N_XFDBG"))
        for (XF* x : {PxfPos(palo), PxfRot(palo)})
            if (x) std::printf("set state %d -> %d: xf kind %d mode %d local t %.1f %.1f %.1f world z %.1f\n", p->state, n, x->kind, x->mode, x->mat[3].x, x->mat[3].y, x->mat[3].z, x->matWorld[3].z);
    if (p->state == n)
    {
        if (p->substate == m) return;
    }
    else
    {
        // the object of the state left (the jump's +0xBC: its target score is worse, 0x19B088)
        switch (p->state)
        {
        case 8: p->jump.ploLeft = p->ploF5C; break;
        case 3: case 0x16: p->jump.ploLeft = p->ploOther; break;
        case 0x15: p->jump.ploLeft = p->ploSurface; break;
        default: p->jump.ploLeft = nullptr; break;
        }
        // leave the old state
        switch (p->state)
        {
        case -1: break;
        case 8:
            SetClimbState(p, -1);  // 0x19EAB0
            SetSlyMode(p, n == 2 && PPart<ASEG>(palo, 0x14D8) ? 0xB : -1);
            break;
        case 0:
            SetSlyXfModes(p, 2);
            switch (p->mode)
            {
            case 6: SetSlyMode(p, 4); break;
            case 7: SetSlyMode(p, 5); break;
            case 8: case 9: SetSlyMode(p, -1); break;
            case 12: if (m != 6) SetSlyMode(p, 0xE); break;
            case 13: if (m != 7) SetSlyMode(p, 0xF); break;
            default: break;
            }
            ActorNextLookTime(p);
            break;
        case 1: case 10: case 12:
            // the gait's phase kept (+0x1A0)
            if (p->pasega && p->pasega->paseg && GDuration(p->pasega->paseg) != 0.0f)
                p->g1A0 = p->pasega->t / GDuration(p->pasega->paseg);
            break;
        case 2:
            // orig: 0x0018C250: play the jump-exit sound before clearing the jump animation.
            if (n == 7 ? p->n4C8 != 1 : n != 3 && n != 8 && n != 18)
            {
                CSnd* psnd = PsndNewPlain(0x3D);
                psnd->SetSource(palo);
                psnd->SetVolume(F32(0x3F333333));
                psnd->SetFalloff(500.0f, 1000.0f);
            }
            // TODO(actors): the +0x1AA8 / +0x1AAC landing effects.
            if (p->mode == 0 && n != 8 && n != 3 && n != 22) SetSlyMode(p, -1);
            SetJumpAnimState(&p->jump, -1, nullptr);  // 0x195FB8
            break;
        case 4: SetDamageState(p, -1); break;  // 0x1A0B40 (+0xDF0)
        case 7: SetInteractState(p, -1); break;  // 0x192BD8 (+0x4C0)
        case 23: SetJtThrowState(p, -1); break;  // 0x2206C8 (+0x11B0)
        case 5: break;
        case 6: SetSlyDodgeState(p, -1); break;  // 0x1375D0
        case 9:
            p->plo18C0 = nullptr;
            if (int i = IsoeActorPart(palo, 0x1A90); i >= 0)
                SetSoeOn(palo, &palo->asoe[i]);  // orig: 0x1F2A60
            break;
        case 24:
            p->uBrake = 1.0f;
            if (PADCTL* pc = PpadctlOf(p)) pc->radJoy = p->radHeading;
            break;
        case 18: case 19: case 20: SetSlyXfModes(p, 2); break;  // (the scripted states)
        case 11: p->grf10E0 &= ~2u; break;  // (the pickpocket)
        case 17:
            p->f1D30 = 0;
            if (std::getenv("SLY2N_HYPNOPRINT")) std::printf("hypno: leave state 17 -> %d t %.3f\n", n, g_clock.t);
            BroadcastTaskEvent(g_psw, 0x38, palo);  // 0x1619A0: the hypnosis over
            break;
        case 21: LeaveSlyClimbSurf(p); break;  // (the climbing surface +0x1F18: climbsurf.cpp)
        case 22: LeaveSlyHangFollow(p); break;  // (the hook +0x18C4 of kind 3: climbsurf.cpp)
        case 3: LeaveSlyHang(p); break;  // (the hook +0x18C4 of kind 0 / 1 / 2: slyhang.cpp)
        case 16:
            if (p->pasega && !FAsegaRetracted(p->pasega)) ReleaseGuardHeld(p->plo10DC);  // 0x209EE0
            p->f1D2C = 0;
            p->grf10E0 &= ~4u;
            break;
        default:
            if (p->state == 13 || p->state == 14) break;  // (13: +0x18C0 = 0, the target)
            WarnOnce("SetState: leaving state " + std::to_string(p->state));
            break;
        }
        // enter the new one
        switch (n)
        {
        case 0:
        {
            psm = p->psmIdle;
            SetSlyXfModes(p, 1);
            p->f1514 = 0;
            const int old = p->state;
            if (old == 10) oidState = 0x177;
            else if (old == 12) oidState = 0x176;
            else if (old == 2)
            {
                bool fHi;
                if (p->n18F0 != -1) fHi = (p->n18F0 ^ 1) != 0;
                else
                {
                    const float d = p->jump.z3C - glm::vec3(MatWorldOfAlo(palo)[3]).z;  // (+0x2FC: the jump's start height)
                    fHi = !(F32(0x43C80000) < d);
                }
                oidState = fHi ? 0x178 : 0x179;
            }
            else oidState = 0x175;
            if (!psm || IsmsFindSm(psm, oidState) < 0) oidState = -1;  // (0x1F0400)
            grfMask = GrfActorLookMask(p);
            p->vecVel = glm::vec3(0.0f);
            grfHandles = 0x1F;
            ActorSetLookFlag(p, 1);
            break;
        }
        case 1:
            paseg = p->gaitWalk.pbl;
            grfapl = 9;
            tStart = p->g1A0 * GDuration(paseg);
            ResetGaitFeet(&p->gaitWalk);
            break;
        case 10:
            paseg = p->rb1AC0.pbl;
            grfapl = 9;
            p->n1038 = -1;
            tStart = p->g1A0 * GDuration(paseg);
            break;
        case 12:
            paseg = p->gaitRun.pbl;
            grfapl = 9;
            tStart = p->g1A0 * GDuration(paseg);
            ResetGaitFeet(&p->gaitRun);
            break;
        case 2: break;  // (the jump's animation: 0x195FB8 from 0x195A00)
        case 9:
            SetSlyMode(p, -1);
            ChooseSlyRushTarget(p);  // orig: 0x18F688
            paseg = PPart<ASEG>(palo, 0x1A7C);
            break;
        case 13:
            SetSlyMode(p, -1);
            p->vecVel = glm::vec3(0.0f);
            ChooseSlyAirSwingTarget(p);  // orig: 0x18F740
            paseg = PPart<ASEG>(palo, 0x1A80);
            p->t19B0 = g_clock.t + 0.25f;  // 0x2BF3E4
            break;
        case 14:
            SetSlyMode(p, -1);
            p->vecVel = glm::vec3(0.0f);
            paseg = PPart<ASEG>(palo, 0x1A84);
            break;
        case 4:
            // (0x19CD38(+0xF00, -1): the grab off)
            if (p->nF04 != -1) WarnOnce("0x19CD38 (+0xF00's state off, entering state 4)");
            p->f1D30 = 0;  // 0x18DF50(p, 0)
            break;
        case 5: break;  // (its substates' animations below)
        case 6:
            fKeepAnim = true;
            SetSlyDodgeState(p, SelectSlyDodgeSide(p));  // 0x1374F0 / 0x1375D0
            break;
        case 7:
            // (the interaction's animation: 0x192BD8 after this; the handles by its kind)
            if (p->n4C8 < 5) grfHandles = p->n4C8 < 3 ? 0 : 7;
            fKeepAnim = true;
            break;
        case 8:
            SetClimbState(p, 0);  // 0x19EAB0 (its animation)
            fKeepAnim = true;
            break;
        case 23:
            fKeepAnim = true;  // 0x18C250: the throw controller owns its animation
            break;
        case 11:
            // the pickpocket: its clip +0x1BC8 (its end: state 0, 0x188F20), +0x10E0 bit 2, no move, the hand pose
            // not set yet (+0x1BD4)
            paseg = PPart<ASEG>(palo, 0x1BC8);
            p->grf10E0 |= 2;
            p->vecVel = glm::vec3(0.0f);
            p->f1BD4 = 0;
            grfHandles = 0x64;
            break;
        case 16:
        {
            p->grf10E0 |= 4;
            if (FPowerupOwned(0x1C))  // 0x1DAEA8
            {
                paseg = PPart<ASEG>(palo, 0x2050);
                UpdateSlyJuggleVelocity(p);  // 0x18E1E0
                SetAloVelocityTo(palo, p->matHeading * -p->vecVel);
                TurnActorNow(p);  // 0x204CA8
            }
            else
            {
                paseg = PPart<ASEG>(palo, 0x1D1C);
                const glm::vec3 posTarget = PosAloCenter(static_cast<ALO*>(p->plo10DC));
                SetStepPhysHeadingToward(palo, -(posTarget - PosAloCenter(palo)));  // 0x2046B0
                TurnActorNow(p);  // 0x204CA8
                const float tHit = GEventAseg11(paseg, 0x173);  // 0x12F990
                glm::mat4 mHit;
                CalculateAsegMatrices(paseg, palo, PxfObj(static_cast<ALO*>(p->plo19A4)), PxfObj(palo),
                                      tHit, 0.0f, &mHit, nullptr);  // 0x1300B8
                const float dtHit = tHit / paseg->svt;
                const glm::vec3 posFuture = PosAloCenter(static_cast<ALO*>(p->plo10DC)) +
                                            VelAloPos(static_cast<ALO*>(p->plo10DC)) * dtHit;
                const glm::vec3 posStrike = posFuture - p->matHeading * glm::vec3(mHit[3]);
                p->vecVel = (posStrike - PosAloCenter(palo)) * (1.0f / dtHit);
                p->vecVel.z = 0.0f;
                SetAloVelocityTo(palo, p->vecVel);
            }
            break;
        }
        case 18: case 19: case 20:
            // the scripted states (18 the script's state machine +0x102C, e.g. the safe house's entry by the splice's
            // slot 88 (4); 19 / 20 a script's animation): the position / rotation records' modes, no velocity, no mode
            if (n == 18 && !std::getenv("SLY2N_NO18SM")) psm = p->psm102C;
            if (!std::getenv("SLY2N_NO18XF")) SetSlyXfModes(p, n == 20 ? 2 : 1);
            p->vecVel = glm::vec3(0.0f);
            SetSlyMode(p, -1);
            break;
        case 21: EnterSlyClimbSurf(p); break;  // (climbsurf.cpp)
        case 3:
            // the swing on a hook of kind 0 / 1 / 2: its blend still at 0.5 (the substate below plays it from its middle)
            paseg = p->pblSwing;
            svt = 0.0f;
            tStart = 0.5f;
            EnterSlyHang(p);
            break;
        case 17:
            // the hypnosis: its clip +0x1D34 (moving by its root), the tasks told (0x37)
            paseg = PPart<ASEG>(palo, 0x1D34);
            grfapl = 9;
            if (std::getenv("SLY2N_HYPNOPRINT")) std::printf("hypno: enter state 17 from %d t %.3f clip %d at %.0f %.0f %.0f head %.2f\n", p->state, g_clock.t, paseg ? paseg->oid : -1, PosAloCenter(palo).x, PosAloCenter(palo).y, PosAloCenter(palo).z, p->radHeading);
            BroadcastTaskEvent(g_psw, 0x37, palo);  // 0x1619A0
            break;
        case 22:
            // the hang-follow on a kind-3 hook: its blend +0x1F90, no jump animation (climbsurf.cpp)
            if (p->pblHang) { paseg = p->pblHang; grfapl = 9; }
            SetSlyMode(p, -1);
            EnterSlyHangFollow(p);
            break;
        case 24:
            grfapl = 9;
            grfHandles = 0x18;
            if (ASEGBL* blend = p->slide.pbl)
            {
                blend->abl[2].weight = 0.0f;
                blend->abl[0].weight = 0.0f;
                blend->abl[1].weight = 1.0f;
                RecalcBlend(blend);
                paseg = blend;
            }
            SetStepPhysHeading(palo, RadJoy(p));
            SetAloVelocityTo(palo, cmm::SetVectorCylind(p->radHeading, 1000.0f, 0.0f));
            p->vecVel = glm::vec3(0.0f);
            break;
        default:
            WarnOnce("SetState: entering state " + std::to_string(n));
            break;
        }
    }
    // the substate
    if (p->substate != m)
    {
        switch (m)
        {
        case -1: break;
        case 0: case 1: case 2:
            // the swing's blend (state 3), kept when it plays
            if (p->pasega && p->pblSwing && p->pasega->paseg == p->pblSwing) fKeepAnim = true;
            else if (p->pblSwing)
            {
                paseg = p->pblSwing;
                svt = 0.0f;
                tStart = GDuration(paseg) * 0.5f;
            }
            break;
        case 3: psm = p->psmHang; break;  // (state 3 hanging still)
        case 4:
            psm = PPart<SM>(palo, 0x1BC4);
            p->vecVel = glm::vec3(0.0f);
            break;
        case 5:
            paseg = p->rb13D0.pbl;  // (+0x14D0)
            p->n1038 = -1;
            p->vecVel = glm::vec3(0.0f);
            grfapl = 9;
            break;
        case 6: case 7:
            // the charged swing's wind-up (square held through the swing's event 0x165): the mode off, the clip
            // +0x1D08 (from the swings 0xC / 0xE / ..) or +0x1D0C (0xD / 0xF / ..), held until square is let go
            // (0x18F0A8: charged by the clip's event 0x166, +0x18E8, the strike 8, else the stance 4)
            SetSlyMode(p, -1);  // 0x18E540
            paseg = PPart<ASEG>(palo, m == 6 ? 0x1D08 : 0x1D0C);
            p->vecVel = glm::vec3(0.0f);
            p->f18E8 = 0;
            p->f18E4 = 0;
            break;
        case 8: case 9:
            // the charged strike (8: +0x1D10; 9: +0x1FE8), its end the stance (0x188F20)
            paseg = PPart<ASEG>(palo, m == 8 ? 0x1D10 : 0x1FE8);
            p->f18E4 = 0;
            p->vecVel = glm::vec3(0.0f);
            break;
        case 10: psm = PPart<SM>(palo, 0x1F14); break;  // (state 21 held)
        case 11:
            paseg = p->rb1E10.pbl;  // (state 21 moving: +0x1F10)
            grfapl = 9;
            break;
        case 12:
            p->uBrake = 3.0f;
            grfHandles = 0x18;
            break;
        case 13:
            fKeepAnim = true;
            p->uBrake = 0.1f;
            grfHandles = 0x18;
            break;
        default: WarnOnce("SetState: substate " + std::to_string(m)); break;
        }
    }
    if (p->state != n)
    {
        p->state = n;
        p->tState = g_clock.t;
    }
    if (p->substate != m)
    {
        p->substate = m;
        p->tSubstate = g_clock.t;
    }
    UpdateActorStepMesh(p);  // slot 90 (0x18C060: the custom entry's shape by the state, actmesh.cpp)
    SetSlyHandles(p, grfHandles);  // 0x18C120
    if (fKeepAnim) return;
    if (p->pasega) RetractAsegaPp(&p->pasega);
    else if (p->psma)
    {
        RetractSma(p->psma);
        p->psma = nullptr;
    }
    if (paseg)
    {
        ApplyAseg(paseg, palo, tStart, svt, grfapl, &p->pasega);
        return;
    }
    if (psm) p->psma = PsmaApplySm(psm, palo, oidState, grfapl, grfMask);
}

// orig: 0x0018A8C8 (the per-frame state transitions, repeated while the state or substate changes; the ground
// states here, the others' parts warn)
void UpdateSlyTransitions(SLY* p)
{
    int s2 = p->state;
    if (std::getenv("SLY2N_PPDEBUG") && (GrfbtnHeld(p) & BTN_CIRCLE))
        std::printf("ppdebug0: t %.3f state %d sub %d\n", g_clock.t, p->state, p->substate);
    for (int iter = 0; iter < 32; ++iter)
    {
        const int s3 = p->substate;
        int n = -1;
        bool fSet = false, fSub = false;
        if (static_cast<unsigned>(s2) < 25)
        {
            // the jump's timed reset (states 0, 1, 5, 10..12, 23, 24 and 7): off the floor for 0.1 s
            bool fTimed = false;
            switch (s2)
            {
            case 7: fTimed = !(p->n4C8 == 12 || (p->n4C8 < 5 && (p->n4C8 >= 3 || p->n4C8 == 1))); break;
            case 0: case 1: case 5: case 10: case 11: case 12: case 23: case 24:
                fTimed = !(static_cast<unsigned>(p->mode - 8) < 2 && !p->f18E4);
                break;
            default: break;
            }
            if (fTimed && 0.1f < g_clock.t - p->tFloor) SetJumpKind(&p->jump, 0, 0, 0);
            switch (p->state)
            {
            case 0: case 1: case 10: case 12:
            {
                if (std::getenv("SLY2N_PPDEBUG") && (GrfbtnHeld(p) & BTN_CIRCLE))
                    std::printf("ppdebug: t %.3f state %d mode %d 10E4 %d 10DC %d reach %d\n", g_clock.t, p->state, p->mode, p->n10E4,
                                p->plo10DC ? int(p->plo10DC->oid) : -1, int(FActorTargetInReach(p, false)));
                if (p->f1D30) { n = 0x11; fSet = true; break; }
                if (static_cast<unsigned>(p->mode - 8) < 2 && !p->f18E4) break;
                if (FBtnTake(p, BTN_CROSS, 0.1f)) { n = 1; fSub = true; break; }
                // a guard in reach (0x140AF8): state 5 (its substate 4)
                if (!(GrfbtnHeld(p) & BTN_R1) && FActorTargetInReach(p, false)) { SetSlyState(p, 5, 4); break; }
                // R3 held: the binocucom's recon view (0x19CC50; slot 99 (3) 0x18D760: TODO its other checks)
                if (FBtnWithin(p, BTN_R3, 0.15f))
                {
                    ForgetPadctlPresses(PpadctlOf(p), BTN_R3);  // 0x1C4738
                    OpenBinoc(p, 0, nullptr, nullptr);
                    break;
                }
                // circle: the pickpocket (state 11) with a guard of rank 3, else the interaction (0x1938C0)
                if (p->n10E4 == 3 && FBtnTake(p, BTN_CIRCLE, F32(0x3E19999A))) { n = 0xB; fSet = true; break; }
                if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD))) { FInteractCircle(p); break; }
                const float v = UJoy(p);
                if (std::getenv("SLY2N_TRANS")) std::printf("  transitions: state %d u %.3f t %.3f\n", p->state, v, g_clock.t);
                if (p->state != 0) { if (!(0.0f < v)) { n = 0; fSet = true; break; } }
                else if (!(0.1f < v)) { n = 0; fSet = true; break; }
                if (GrfbtnHeld(p) & BTN_R1) { n = 1; fSet = true; break; }
                n = p->n10E4 != 3 ? 0xC : 0xA;
                fSet = true;
                break;
            }
            case 2:
                UpdateJumpTransitions(&p->jump);  // 0x197500
                if (p->f1D30) { n = 0x11; fSet = true; break; }
                // (slot 99 (2): 0x18D760) square / triangle in the air: the attack states 13 / 14
                if (FBtnTake(p, BTN_SQUARE, 0.0f)) { n = 0xD; fSet = true; break; }
                if (FBtnTake(p, BTN_TRIANGLE, 0.0f)) { n = 0xE; fSet = true; break; }
                break;
            case 4: UpdateDamageTransitions(p); break;  // 0x1A12F0 (+0xDF0)
            case 6: UpdateSlyDodgeTransitions(p); break;  // 0x1371C8
            case 24: UpdateSlySlideTransitions(p); break;
            case 8: UpdateClimbTransitions(p); break;   // 0x19E1E0 (+0xF50)
            case 7: UpdateInteractTransitions(p); break;  // 0x1911A8 (+0x4C0)
            case 23: UpdateJtThrow(p); break;  // 0x18A8C8 -> 0x21F418
            case 21:
                // on a climbing surface: cross jumps off it (kind 0x12)
                if (FBtnTake(p, BTN_CROSS, 0.25f)) { n = 0x12; fSub = true; }
                break;
            case 22:
                // on a kind-3 hook: cross jumps toward its target (kind 0xC, climbsurf.cpp)
                if (FBtnTake(p, BTN_CROSS, 0.25f)) FSlyHangFollowJump(p);
                break;
            case 3: FSlyHangRelease(p); break;  // the swing: cross lets go (kind 2, slyhang.cpp)
            case 5:
                // a guard in reach: the stance (substate 4) or the walk toward it (5); the swings' substates 6..9 stay
                if (p->f1D30) { n = 0x11; fSet = true; break; }
                if (static_cast<unsigned>(p->substate - 6) < 4) break;
                if (GrfbtnHeld(p) & BTN_R1) { n = 0; fSet = true; break; }
                if (!FActorTargetInReach(p, true)) { n = 0; fSet = true; break; }
                if (FBtnTake(p, BTN_CROSS, 0.1f)) { n = 1; fSub = true; break; }
                if (FBtnWithin(p, BTN_R3, 0.15f))
                {
                    ForgetPadctlPresses(PpadctlOf(p), BTN_R3);  // 0x1C4738
                    OpenBinoc(p, 0, nullptr, nullptr);          // 0x19CC50
                    break;
                }
                if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD))) { FInteractCircle(p); break; }  // 0x1938C0
                SetSlyState(p, 5, 0.0f < UJoy(p) ? 5 : 4);
                break;
            case 13:
                // the air swing: its end goes on falling (the jump's kind 0xB), a floor after 0.4 s stands
                if (p->pasega && FAsegaRetracted(p->pasega)) { n = 0xB; fSub = true; break; }
                if (p->pxpFloor && F32(0x3F333333) < p->pxpFloor->normal.z && F32(0x3ECCCCCD) < g_clock.t - p->tState)
                {
                    n = 0;
                    fSet = true;
                }
                break;
            case 14:
                // the air slam: its end stands; a floor after 0.4 s seeks it to its landing event 0x16D
                if (p->pasega && FAsegaRetracted(p->pasega)) { n = 0; fSet = true; break; }
                if (p->pxpFloor && F32(0x3F333333) < p->pxpFloor->normal.z && F32(0x3ECCCCCD) < g_clock.t - p->tState)
                    if (ASEG* paseg = PPart<ASEG>(p->palo, 0x1A84); paseg && p->pasega)
                    {
                        const float g = GEventAseg11(paseg, 0x16D);  // 0x12F990
                        if (p->pasega->t < g) SetAsegaTime(p->pasega, 0, g, p->pasega->svt);  // 0x130D30
                    }
                break;
            case 17:
                // the hypnosis: over when +0x1D30 is cleared or after 5 s (0x2BF360)
                if (!p->f1D30 || F32(0x40A00000) < g_clock.t - p->tState) { n = 0; fSet = true; }
                break;
            case 9: case 18: case 20: break;  // state 9 ends through animation message 3
            case 11: break;  // (the pickpocket: 0x18A8C8 has only the common floor test for it)
            case 16:
                if (!p->pasega || FAsegaRetracted(p->pasega)) { n = 0; fSet = true; }
                break;
            case 19:
                // a script's animation (+0x1030) or state machine (+0x1034) done: stand
                if (p->pasega ? FAsegaRetracted(p->pasega) : (p->psma && FSmaRetracted(p->psma))) { n = 0; fSet = true; }
                break;
            default:
                WarnOnce("the transitions of state " + std::to_string(p->state));
                break;
            }
        }
        if (fSub) SetJumpKind(&p->jump, n, 0, 0);
        else if (fSet) SetSlyState(p, n, -1);
        if (p->state == s2 && p->substate == s3) break;
        s2 = p->state;
    }
}

namespace
{
// orig: 0x0018ED98 (the average of the followed object +0x19A4 and the record +0x19C0 (0x1F4C58(p, 0xC, 6): its
// world position) in the actor's frame: ahead (y > 0))
bool FSlyAnchorsAhead(SLY* p)
{
    const XF* pxfC0 = nullptr;
    for (const auto& pxf : p->palo->axf)
        if (pxf->tag == 0xC && pxf->kind == 6) { pxfC0 = pxf.get(); break; }
    if (!p->plo19A4 || !FCidDerivedFrom(p->plo19A4->Cid(), Icid(CID3::ALO)) || !pxfC0)
    {
        WarnOnce("0x18ED98 without its objects");
        return false;
    }
    const XF* pxfObj = PxfObj(p->palo);
    const glm::mat4 m = pxfObj ? pxfObj->matWorld : MatWorldOfAlo(p->palo);
    const glm::mat4 mi = glm::inverse(m);  // 0x1A6F00(null, p, pos): into the actor's frame
    const glm::vec3 a = glm::vec3(mi * glm::vec4(PosAloCenter(static_cast<ALO*>(p->plo19A4)), 1.0f));
    const glm::vec3 b = glm::vec3(mi * glm::vec4(glm::vec3(pxfC0->matWorld[3]), 1.0f));
    return 0.0f < (a.y + b.y) * 0.5f;
}

// orig: 0x0018EE38 (the contextual B1 strike, then the directional swing and its left / right side)
int NSlyNextSwing(SLY* p)
{
    if (p->plo10DC)
    {
        const glm::vec3 d = PosAloCenter(static_cast<ALO*>(p->plo10DC)) - PosAloCenter(p->palo);
        const float distance = std::sqrt(d.x * d.x + d.y * d.y);
        if (distance < 400.0f && 250.0f < d.z && d.z < 1000.0f)  // 0x11D910 / 0x2BF4AC / 0x2BF4B0
            if (GUARD* pg = PguardFromLo(p->plo10DC))
                if (JOBB1* pj = dynamic_cast<JOBB1*>(PjobFind(pg, 0xB1));
                    pj && pj->ploOwner == p->palo && 0 <= pj->stLaunch && pj->stLaunch < 2)
                {
                    SetSlyState(p, 16, -1);
                    return -1;
                }
    }
    int n;
    if (p->substate == 5)
    {
        const glm::mat4 mat = MatWorldOfAlo(p->palo);  // vt slot 31
        const float d = cmm::RadNormalize(RadJoy(p) - std::atan2(mat[0].y, mat[0].x));
        const float ad = std::fabs(d);
        if (ad < F32(0x3F490FDB)) n = 0xE;
        else if (F32(0x4016CBE4) < ad) n = 0x10;
        else if (0.0f < d) n = 0x12;
        else n = 0x14;
    }
    else n = p->state == 0 ? 0xC : 0xE;
    const int prev = p->mode;
    if (prev >= 0xC && prev <= 0x15)
    {
        if (!(prev & 1)) ++n;
    }
    else if (FSlyAnchorsAhead(p)) ++n;
    return n;
}

// orig: 0x0018F050 (the triangle's two moves, alternating)
int NSlyNextTriangle(SLY* p)
{
    if (p->mode == 22) return 23;
    if (p->mode == 23) return 22;
    return FSlyAnchorsAhead(p) ? 23 : 22;
}

// orig: 0x0018F0A8 (the buttons' effects by state: triangle / square start a mode while none runs or its combo
// window is open (+0x18E4); square in the states 3 / 21 / 22 lets go (the jump 0); state 5's substates 6 / 7)
void UpdateSlyButtons(SLY* p)
{
    if (p->mode < 0x16 && p->mode >= 0xC && !(GrfbtnHeld(p) & BTN_SQUARE)) p->f18EC = 0;
    bool fGo = false;
    switch (p->state)
    {
    case 0: case 1: case 10: case 12: fGo = true; break;
    case 3: case 21: case 22:
        if (FBtnTake(p, BTN_SQUARE, 0.0f)) SetJumpKind(&p->jump, 0, 0, 0);
        break;
    case 7: fGo = p->n4C8 < 5 && !(p->n4C8 < 3); break;
    case 5:
        if (p->substate < 6) fGo = true;
        else if (p->substate < 8)
        {
            if (!(GrfbtnHeld(p) & BTN_SQUARE)) SetSlyState(p, 5, p->f18E8 ? 8 : 4);
        }
        else if (p->substate < 10) return;
        else fGo = true;
        break;
    default: break;
    }
    if (!fGo) return;
    if (p->mode != -1)
    {
        if (p->mode < -1 || p->mode >= 0x18 || p->mode < 0xC) return;
        if (!p->f18E4) return;
    }
    const float dt = F32(0x3EB33333);  // (T_PRESS 0.35)
    if (FBtnTake(p, BTN_TRIANGLE, dt))
    {
        SetSlyMode(p, -1);
        SetSlyMode(p, NSlyNextTriangle(p));
    }
    else if (FBtnTake(p, BTN_SQUARE, dt))
    {
        SetSlyMode(p, -1);
        SetSlyMode(p, NSlyNextSwing(p));
    }
}

// orig: 0x001A0720 (the walk around the target: facing +0x10DC (its +0x494 in its volume, rank 3), the stick in the
// heading's frame: past 0.3 sideways the side's entries (1 left: 1 / 3 / 5, 0 right: 0 / 2 / 4) by the side, forward
// and back amounts, a change of side first kills the sideways velocity; +0x190 at the ASEGA's speed, no z)
void MoveAroundTarget(SLY* p, ROOTBLEND* prb, bool fOn, float rad, float u)
{
    if (!fOn) return;
    LO* plo = p->plo10DC;
    if (plo && p->n10E4 == 3)
    {
        GUARD* pg = PguardFromLo(plo);
        plo = pg ? pg->plo494 : nullptr;
    }
    if (plo && FIsLoDerivedFrom(plo, Icid(CID3::ALO)))
        SetStepPhysHeadingToward(p->palo, PosAloCenter(static_cast<ALO*>(plo)) - PosAloCenter(p->palo));  // 0x2046F0
    const float d = cmm::RadNormalize(rad - p->radHeading);
    float x = std::cos(d) * u, y = std::sin(d) * u;  // 0x11E1A0
    const float g = F32(0x3E99999A);                 // 0.3 (0x2BF9F0)
    auto killSide = [&]() {
        y = 0.0f;
        const glm::vec3 v = VelAloPos(p->palo);
        const glm::vec3 r1 = p->matHeading[1];  // (0x1A6318's row 1)
        SetAloVelocityTo(p->palo, v - r1 * glm::dot(v, r1));
    };
    int side;
    if (g < y)
    {
        if (p->n1038 == 0) killSide();
        side = 1;
    }
    else if (y < -g)
    {
        if (p->n1038 == 1) killSide();
        side = 0;
    }
    else side = p->n1038 == -1 ? 0 : p->n1038;
    const float fwd = std::max(0.0f, x), back = -std::min(0.0f, x);
    glm::vec3 v;
    if (side == 1) v = glm::vec3(prb->avel[1]) * y + glm::vec3(prb->avel[3]) * fwd + glm::vec3(prb->avel[5]) * back;
    else v = glm::vec3(prb->avel[0]) * -y + glm::vec3(prb->avel[2]) * fwd + glm::vec3(prb->avel[4]) * back;
    v.z = 0.0f;
    p->vecVel = v * (p->pasega ? p->pasega->svt : 1.0f);
    p->n1038 = side;
}

// orig: 0x0018B7E8 (the controller's input: the buttons' effects, the transitions, the state's own input)
void UpdateSlyInput(SLY* p)
{
    UpdateJtCandidates(p, 0xFFFF);  // 0x194A40
    UpdateActorBinoc(p, 0x4F0);  // 0x18B2C0 (+0xF00; 0x410 with the talk prompt's state 7: TODO)
    UpdateSlyTransitions(p);
    UpdateSlyButtons(p);
    switch (p->state)
    {
    case 12:
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitRun, UJoy(p)));  // 0x20BB58 (its result written to +0x190)
        SetStepPhysHeading(p->palo, RadJoy(p));                     // 0x204660
        break;
    case 2: UpdateJumpInput(&p->jump); break;  // 0x197F98
    case 0: case 1: case 8: case 13: case 14: case 18: case 23:
        break;
    case 19: case 20:
    {
        // 0x204E38: the script's animation's root velocity at its time, reversed
        p->vecVel = glm::vec3(0.0f);
        ASEGA* pasega = p->pasega;
        if (pasega && !FAsegaRetracted(pasega))
            if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
            {
                glm::vec4 v(0.0f);
                pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
                p->vecVel = -glm::vec3(v);
            }
        if (p->state == 20 && pasega)  // (as the gang's 11 / 18: the speed and heading by the controller, steered by a drive)
        {
            if (PADCTL* pc = PpadctlOf(p))
            {
                SetAsegaSpeed(pasega, pc->uJoy);              // 0x131A00
                SetStepPhysHeading(p->palo, pc->radJoy);      // 0x204660
            }
        }
        break;
    }
    case 17:
    {
        // the hypnosis (the Contessa's eye, task events 0x37 / 0x38): the clip +0x1D34's root velocity, reversed,
        // turned toward the stick while it is pushed (0x204E38 / 0x204660)
        p->vecVel = glm::vec3(0.0f);
        ASEGA* pasega = p->pasega;
        if (pasega && !FAsegaRetracted(pasega))
            if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
            {
                glm::vec4 v(0.0f);
                pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
                p->vecVel = -glm::vec3(v);
            }
        if (0.0f < UJoy(p)) SetStepPhysHeading(p->palo, RadJoy(p));
        if (std::getenv("SLY2N_HYPNOPRINT"))
            std::printf("hypno: t %.3f clip %d at %.3f svt %.2f vel %.1f %.1f %.1f actual %.1f u %.2f rad %.2f head %.2f pan %.2f cam %.0f %.0f %.0f yrange %.2f\n", g_clock.t,
                        pasega && pasega->paseg ? pasega->paseg->oid : -1, pasega ? pasega->t : -1.f, pasega ? pasega->svt : 0.f, p->vecVel.x,
                        p->vecVel.y, p->vecVel.z, glm::length(VelAloPos(p->palo)), UJoy(p), RadJoy(p), p->radHeading, std::atan2(g_cm.mat[0].y, g_cm.mat[0].x), g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.yScreenRange);
        break;
    }
    case 7: UpdateInteract(p); break;  // 0x191A10 (+0x4C0)
    case 11: UpdatePickpocketPose(p); break;  // 0x18B460
    case 16:
        if (FPowerupOwned(0x1C)) UpdateSlyJuggleVelocity(p);  // 0x18E1E0
        else if (p->f1D2C)
            if (const ASEGCHN* pchn = PchnFindAseg(PPart<ASEG>(p->palo, 0x1D1C), p->palo, 1, true);
                pchn && pchn->ptrkp)
            {
                glm::vec4 v(0.0f);
                pchn->ptrkp->Eval(nullptr, &v, p->pasega->t, p->pasega->svt);
                p->vecVel = -glm::vec3(v);
            }
        break;
    case 6: UpdateSlyDodgeInput(p); break;  // 0x137218
    case 24: UpdateSlySlideInput(p); break;  // state-24 branch of 0x18B7E8
    case 10: MoveAroundTarget(p, &p->rb1AC0, true, RadJoy(p), UJoy(p)); break;  // 0x1A0720
    case 4: case 9:
    {
        // the reaction's clip's root velocity (0x12FF10's channel of kind 1, sampled at its time), reversed
        p->vecVel = glm::vec3(0.0f);
        ASEGA* pasega = p->pasega;
        if (!pasega || FAsegaRetracted(pasega)) break;
        if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
        {
            glm::vec4 v(0.0f);
            pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
            p->vecVel = -glm::vec3(v);
        }
        break;
    }
    case 5:
        if (p->substate == 4)
        {
            // facing the object +0x10DC (0x2046F0)
            if (p->plo10DC && FIsLoDerivedFrom(p->plo10DC, Icid(CID3::ALO)))
                SetStepPhysHeadingToward(p->palo, PosAloCenter(static_cast<ALO*>(p->plo10DC)) - PosAloCenter(p->palo));
        }
        else if (p->substate == 5) MoveAroundTarget(p, &p->rb13D0, true, RadJoy(p), UJoy(p));  // 0x1A0720
        else if (p->substate == 8 || p->substate == 9)
        {
            // the strike steered by the stick: its direction (behind the heading, x 1000: 0x2BF3C0) in Sly's frame
            const float rad = RadJoy(p) + 3.14159265f;
            const glm::vec3 v(std::cos(rad) * UJoy(p) * 1000.0f, std::sin(rad) * UJoy(p) * 1000.0f, 0.0f);  // 0x11E1A0
            p->vecVel = VecJtConvert(nullptr, p->palo, v);  // 0x1A6FC0
        }
        break;
    case 22: break;  // (0x18B7E8 has no part for the hang-follow)
    case 3: UpdateSlyHangInput(p); break;  // 0x190520
    default:
        WarnOnce("the input of state " + std::to_string(p->state));
        break;
    }
    UpdateDamage(p);              // 0x1A1358 (+0xDF0)
    UpdateSafeGround(p, p->state);  // 0x19FC30 (+0x9E0)
    // orig: 0x18B7E8, release the holder after its ability animation retracts.
    if (p->pasegaAbility && FAsegaRetracted(p->pasegaAbility))
    {
        ReleaseAsega(p->pasegaAbility);
        p->pasegaAbility = nullptr;
        p->idAbility = -1;
    }
}

// orig: 0x0018A0C8 (state 0: the idle SM's mask, the support's turn, the feet's probes 0x189C40 / 0x189EC8 (radius
// 5: the feet set on the floor under them by +0x19F0 / +0x19F4, as in the slide), a foot over a drop: the idle SM's
// state 0x17C once)
void UpdateSlyStanding(SLY* p)
{
    if (p->psma) SetSmaMask(p->psma, GrfActorLookMask(p));  // 0x1F1350
    // (slot 92 is 0 for Sly) the support's angular velocity's z, limited to 10, turns the heading
    if (p->pxpFloor)
        if (ALO* paloFloor = p->pxpFloor->axpd[1].paloRoot)
        {
            const glm::vec3 w = AngVelAlo(paloFloor);  // (slot 34 of the floor's object: +0x4C)
            const float z = cmm::GLimitAbs(w.z, 10.0f);
            SetStepPhysHeading(p->palo, p->radHeading + z * g_clock.dt);
        }
    if (p->slide.aphandle[3])
    {
        auto rec = [p](int tag, int kind) -> XF* {
            for (const auto& xf : p->palo->axf)
                if (xf->tag == tag && xf->kind == kind) return xf.get();
            return nullptr;
        };
        RebuildSlyFootProbe(p, rec(0x13, 1), 5.0f, rec(0x18, 6), &p->slide.afoot[0]);  // (+0x19D8, +0x19E0, +0x1520)
        RebuildSlyFootProbe(p, rec(0x12, 1), 5.0f, rec(0x19, 6), &p->slide.afoot[1]);  // (+0x19D4, +0x19DC, +0x1620)
        if ((!MatchSlyFootProbe(p, &p->slide.afoot[0], p->slide.aphandle[3]) ||
             !MatchSlyFootProbe(p, &p->slide.afoot[1], p->slide.aphandle[4])) &&
            !p->f1514 && p->psmIdle)
        {
            if (IsmsFindSm(p->psmIdle, 0x17C) >= 0 && p->psma) SetSmaState(p->psma, 0x17C);  // 0x1F0400 / 0x1F09D8
            p->f1514 = 1;
        }
    }
}

// orig: 0x001403D0 (the actor's update; TODO(actors): the abilities 0x141A50, 0x140338, +0xFD0's 0x144640, the
// targets 0x140558, 0x1A2460, the pickups 0x140750, 0x142AF8, the camera's 0x14FD50 part, the +0x26C list)
}  // namespace
// orig: 0x00140F90 ActorHandleMessage (3: its animation done; 9: an ASEG event of its animation: 0x161 / 0x162 the
// camera target held / let go; TODO(actors): 0x15E (0x204E38 the animation's velocity), 0x27B / 0x27C / 0x1C7 /
// 0x1C8 (+0x11B0, +0x4C0), the attacks' 0x130C / 0x9BC; 4 / 5 the carried object; 1, 2, 6..8, 10, 11; then
// 0x1EED78 (+0x26C) and 0x1C3E20), with Sly's 0x188F20 before it (3: the states 9 / 0xB end, 7 / 0x15, 5's 8..9)
void HandleActorMessage(ACTOR* pa, int msg, void* pv)
{
    SLY* psly = dynamic_cast<SLY*>(pa);
    switch (msg)
    {
    case 3:
    {
        ASEGA* pasega = static_cast<ASEGA*>(pv);
        if (psly && pasega == psly->pasegaMode)  // 0x188F20: the mode's animation done (mode 0xA goes on to 2)
        {
            SetSlyMode(psly, psly->mode == 0xA ? 2 : -1);
            break;
        }
        const bool fOwn = pasega == pa->pasega || (pa->psma && pa->psma->pasega == pasega);
        if (!fOwn) break;
        if (psly && pasega == psly->pasega)  // 0x188F20's part (its +0x1030)
        {
            switch (psly->state)
            {
            case 9: case 0xB: SetSlyState(psly, 0, -1); break;
            case 7: case 0x15: psly->n1038 = -1; break;
            case 5: if (psly->substate < 10 && psly->substate >= 8) SetSlyState(psly, 5, 4); break;
            default: break;
            }
        }
        pa->n1038 = -1;
        if (static_cast<unsigned>(pa->nSubDF4 - 8) < 5) EndDamageReaction(pa);  // 0x1A15D0
        break;
    }
    case 9:
    {
        const ASEGMSG9* pmsg = static_cast<const ASEGMSG9*>(pv);
        // orig: 0x140F90 tests the animation's owner, not the actor's current
        // animation reference. Keep this distinction for the deferred Smoke event.
        if (pmsg && pmsg->n == 0x27C && pmsg->pasega && pmsg->pasega->palo == pa->palo && pa->n4C8 == 11)
        {
            // orig: 0x140F90 event 0x27C: the interaction's object (+0x4D4) a thrown gadget out there (cid 100,
            // 0x224590: state 1 / 2) is triggered (0x2245B8: slot 76 with 5 for kind 11, else 3)
            LO* ploT = pa->inter.ploTarget;
            if (ploT && FIsLoDerivedFrom(ploT, 100))
                if (const CARRY* pc = PcarryFromLo(ploT); pc && (pc->state == 1 || pc->state == 2))
                    SetCarryState(ploT, pc->kind == 11 ? 5 : 3);
        }
        if (pmsg && pmsg->n == 0x27C && pmsg->pasega && pmsg->pasega->palo == pa->palo &&
            (ReleaseCaneAbility(pa) || ReleaseSmokeBomb(pa) || ReleaseTimeLock(pa) || ReleaseRoar(pa))) break;
        // orig: 0x140F90 -> 0x2205A8: release follows the event owner's throw
        // controller even when its current main animation reference has changed.
        if (pmsg && pmsg->n == 0x27B && pmsg->pasega && pmsg->pasega->palo == pa->palo)
        {
            if (pa->jtthrow.state != -1) ReleaseJtThrow(pa);
            break;
        }
        const bool fOwn = pmsg->pasega == pa->pasega || (pa->psma && pa->psma->pasega == pmsg->pasega) ||
                          (psly && pmsg->pasega == psly->pasegaMode);
        if (!fOwn) break;
        switch (pmsg->n)
        {
        case 0x15E:
        {
            // 0x204E38: the animation's root velocity (channel kind 1) at the event's time; +0x190 its negation, and
            // the object's velocity it in the heading's frame (slot 35)
            glm::vec4 v(0.0f);
            if (ASEGA* pasega = pmsg->pasega; pasega && !FAsegaRetracted(pasega))
                if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, pa->palo, 1, true))
                    if (pchn->ptrkp) pchn->ptrkp->Eval(nullptr, &v, pmsg->t, pasega->svt);
            pa->vecVel = -glm::vec3(v);
            SetAloVelocityTo(pa->palo, pa->matHeading * glm::vec3(v));
            break;
        }
        case 0x161: pa->f1044 = 1; break;
        case 0x162:
        {
            pa->f1044 = 0;
            const glm::vec3 d = glm::vec3(pa->posCmTarget) - PosAloCenter(pa->palo);
            pa->vecOffset = glm::vec3(d.x, d.y, 0.0f);
            break;
        }
        case 0x164: if (psly) psly->f18E4 = 1; break;  // 0x188F20
        case 0x166: if (psly) psly->f18E8 = 1; break;
        case 0x165:
            // 0x188F20: square still held through the swing (+0x18EC): the charged swing's wind-up (state 5's 6 / 7)
            if (psly && psly->f18EC)
                switch (psly->mode)
                {
                case 0xC: case 0xE: case 0x10: case 0x12: case 0x14: SetSlyState(psly, 5, 6); break;
                case 0xD: case 0xF: case 0x11: case 0x13: case 0x15: SetSlyState(psly, 5, 7); break;
                default: break;
                }
            break;
        case 0x167: if (psly) PickpocketTake(psly); break;  // 0x188F20: the pickpocket's take
        case 0x173:
            if (psly)
            {
                psly->f1D2C = 1;
                psly->vecVel = glm::vec3(0.0f);
                SetAloVelocityTo(psly->palo, psly->vecVel);
                if (GUARD* pg = PguardFromLo(psly->plo10DC))
                    if (AIJOB* pj = PjobFind(pg, 0xB1)) pj->SetState(4);  // 0x205458 / 0x17B1A8
            }
            break;
        case 0x174:
            if (psly)
                if (GUARD* pg = PguardFromLo(psly->plo10DC))
                    if (AIJOB* pj = PjobFind(pg, 0xB1)) pj->SetState(6);
            break;
        case 0x1C8:  // orig 0x140F90 / 0x1413CC: prize flies to the hand
            if (pa->n4C8 == 12) StartReachGrab(&pa->inter.reach, pa->inter.ploTarget);
            break;
        case 0x1C7:  // orig 0x140F90 / 0x1413F0: arrived; resume the pose and attach
            if (pa->n4C8 == 12)
            {
                SetAsegaSpeed(pa->pasega, 1.0f);
                AttachReachObject(&pa->inter.reach, pa->inter.ploTarget);
            }
            break;
        case 0x16D:
            // orig: 0x00188F20: the slam's landing broadcasts fixed noise 0 through 0x00156B98.
            if (psly) PnoiseNew(PtmplNoiseFixed(0), PosAloCenter(psly->palo), psly->palo);
            break;
        case 0x16E: case 0x16F: case 0x170:  // Sly's (the
        case 0x27C:  // partner, the attacks); the actor's (+0x11B0,
                                                          // +0x4C0's)
            WarnOnce("ActorHandleMessage 9's event " + std::to_string(pmsg->n));
            break;
        default: break;  // (the others are not the actors': footsteps and sounds go by the events' own kinds)
        }
        break;
    }
    case 4: case 5:
    {
        // the water's signals {water, object}: entered (4) / left (5): +0x108C; the splash entries +0x1090 (their slot
        // 8 / 9: TODO)
        struct WMSG { LO* pw; LO* po; };
        const WMSG* pm = static_cast<const WMSG*>(pv);
        if (!pm || pm->po != pa->palo) break;
        if (msg == 4) pa->ploWater = pm->pw;
        else if (pa->ploWater == pm->pw) pa->ploWater = nullptr;
        break;
    }
    case 11:  // orig: 0x140F90, an accepted contact consumes the sender's ability
    {
        struct HITMSG { LO* receiver; LO* sender; };
        const auto* hit = static_cast<const HITMSG*>(pv);
        if (hit && hit->sender == pa->palo)
        {
            RetractAsegaPp(&pa->pasegaAbility);
            pa->idAbility = -1;
        }
        break;
    }
    case 1:  // orig: 0x140F90 case 1: the interaction kind 0xB's object (+0x4D4) out of the world: forgotten (0x133448)
        if (pa->n4C8 == 0xB && pa->inter.ploTarget && pa->inter.ploTarget == pv)
        {
            RemoveLoListener(pa->inter.ploTarget, pa);
            pa->inter.ploTarget = nullptr;
        }
        break;
    default: break;  // (2, 6..8, 10: none of the actor's own; TODO(actors): the defaults 0x1EED78 (+0x26C) / 0x1C3E20)
    }
}

// orig: 0x001425B0's state part (the actor's draw: its two alphas +0x10D4 / +0x10D8 smoothed toward 1, or toward
// 0.15 / 0 on the +0x4C0 sub-object's kind 2 (0x2BD268); global Sly's two parts +0x19A0 / +0x19A4 fade
// with it). The draw itself (0x168638 RenderGlob with the fade +0x12C times these alphas) is scenesw.cpp's
// PlaceGlobset -> SCENEGLOBSET::gAlpha / gAlpha2. Native Sly runs this after his state/input updates so entry,
// release and empty-power transitions reach the draw alphas in the same update; the draw/physics phase remains separate.
void UpdateActorDrawAlpha(ACTOR* pa)
{
    if (pa->nF04 == 1 && pa->nF20 != 2) return;
    float g0 = 1.0f, g1 = 1.0f;
    if (pa->n4C8 == 2)
    {
        g0 = 0.15f;
        g1 = 0.0f;
    }
    static const cmm::SMP s_smp{12.0f, 2.0f, 0.1f};  // 0x2BD270
    pa->g10D4 = cmm::GSmooth(pa->g10D4, g0, g_clock.dt, &s_smp, nullptr);  // 0x11D018
    pa->g10D8 = cmm::GSmooth(pa->g10D8, g1, g_clock.dt, &s_smp, nullptr);
    if (SLY* psly = dynamic_cast<SLY*>(pa); psly && psly == PslyGlobal())
        for (LO* plo : {psly->plo19A0, psly->plo19A4})
            if (plo && FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) static_cast<ALO*>(plo)->gFade = pa->g10D4;
}

namespace
{
// the player's noise template +0x10C4: its child object oid 684 (the post-load's child table 0x2BCAD4 {0x300, 684,
// +0x10C4}; a cid-0x75 template: radius 1000, heights -340..300 (Sly), kind 3, life 0)
NOISETMPL* PtmplActorNoise(ACTOR* pa)
{
    if (!pa->fNoiseResolved)
    {
        pa->fNoiseResolved = true;
        LO* plo = PloFindSwChild(pa->palo, 684);
        pa->ploNoise = plo && plo->Cid() == 0x75 ? plo : nullptr;
    }
    return pa->ploNoise ? PtmplNoise(pa->ploNoise) : nullptr;
}

inline float GClamp01(float g) { return g < 0.0f ? 0.0f : (1.0f < g ? 1.0f : g); }

// orig: 0x001409B8 (0 while the game is paused (0x20F0B0), else -1)
int NActorPaused() { return 0 < g_gamepause.c78 ? 0 : -1; }

// vt slot 98 (orig: JT 0x00189488, Bentley 0x00135E40, Murray 0x001B8100): 1 in a noisy state, 0 paused, else -1
int NActorNoisyState(ACTOR* pa)
{
    const int r = NActorPaused();
    if (r != -1) return r;
    if (SLY* psly = dynamic_cast<SLY*>(pa))
    {
        switch (psly->state) { case 1: case 5: case 6: case 9: return 1; default: return -1; }
    }
    if (BENTLEY* pb = dynamic_cast<BENTLEY*>(pa))
    {
        switch (pb->state) { case 1: case 5: case 14: return 1; default: return -1; }
    }
    if (MURRAY* pm = dynamic_cast<MURRAY*>(pa))
    {
        switch (pm->state)
        {
        case 1: case 12: return 1;
        case 9: return pm->n17F4 ? 1 : -1;
        default: return -1;
        }
    }
    return -1;
}

// orig: 0x001409E8 (2: the binocucom on (0x1443D8), no template, or a guard in state 0x1D / 0xB; 1: a guard (its slot
// 89, 0x208BE0) or a listener (cid 0x2F of the list 0x3E1468, 0x156FE8) would take the actor's noise; else 0). The
// noise tested is made (0x156698) and let go (0x156780) here.
int NActorPerceived(ACTOR* pa, NOISETMPL* ptmpl)
{
    if (static_cast<unsigned>(pa->nF04) < 3) return 2;
    NOISE* pn = ptmpl ? PnoiseNew(ptmpl, PosLoWorld(pa->palo), pa->palo) : nullptr;
    if (!pn) return 2;
    int r = 0;
    // the SOs of the list 0x3E1444 (the guards in the world; natively by class, in creation order, as target.cpp)
    for (CID cid = 0; cid < CID_Max && r == 0; ++cid)
    {
        if (!g_psw || !FCidDerivedFrom(cid, Icid(CID3::GUARD))) continue;
        for (LO* plo : g_psw->mpcidaplo[cid])
        {
            GUARD* pg = plo && plo->fInWorld ? PguardFromLo(plo) : nullptr;
            if (!pg) continue;
            if (FGuardState(pg, 0x1D) || FGuardState(pg, 0xB)) { r = 2; break; }
            if (FGuardAcceptsNoise(pg, pn))
            {
                r = 1;
                if (std::getenv("SLY2N_ACTORNOISE")) std::printf("actornoise: guard %d (cid 0x%X) takes it\n", plo->oid, plo->Cid());
                break;
            }
        }
    }
    if (r == 0 && FNoiseListenersReach(pn)) r = 1;
    RemoveNoise(pn);
    return r;
}

// orig: 0x00140750 (from the actor's update: +0x10CC rises at 2/s while the guards would perceive it, falls at 4/s
// when not, 0 when blocked; +0x10C8 its state; +0x10D0 builds up at 4/s in a noisy state (slot 98) while perceived
// and drops at 4/s otherwise; at 1 the actor makes its noise (0x156548))
void UpdateActorNoise(ACTOR* pa)
{
    NOISETMPL* ptmpl = PtmplActorNoise(pa);
    if (!ptmpl) return;
    const float dt = g_clock.dt;
    switch (NActorPerceived(pa, ptmpl))
    {
    case 1: pa->g10CC = std::min(pa->g10CC + 2.0f * dt, 1.0f); break;   // 0x2BCC6C
    case 0: pa->g10CC = std::max(pa->g10CC + -4.0f * dt, 0.0f); break;  // 0x2BCC70
    case 2: pa->g10CC = 0.0f; break;
    default: break;
    }
    float g;
    if (pa->nSubDF4 != -1 || (g = pa->g10CC) == 0.0f) pa->n10C8 = -1;
    else if (g == 1.0f || (pa->n10C8 == 1 && 0.0f < g)) pa->n10C8 = 1;
    else pa->n10C8 = 0;
    int k = NActorNoisyState(pa);
    if (pa->n10C8 != 1 && k == 1) k = 2;
    switch (k)
    {
    case 0: pa->g10D0 = 0.0f; break;
    case 1: pa->g10D0 = GClamp01(pa->g10D0 + 4.0f * dt); break;  // 0x2BCC74
    case -1: case 2: pa->g10D0 = GClamp01(pa->g10D0 + -4.0f * dt); break;  // 0x2BCC78
    default: break;
    }
    static const bool s_fDbg = std::getenv("SLY2N_ACTORNOISE") != nullptr;
    if (s_fDbg)
        std::printf("actornoise %d: t %.3f cc %.3f c8 %d k %d d0 %.3f (tmpl %d r %.0f z %.0f..%.0f life %.2f kind %d)\n",
                    pa->palo->oid, g_clock.t, pa->g10CC, pa->n10C8, k, pa->g10D0, ptmpl->nType, ptmpl->sRadius, ptmpl->rz.x,
                    ptmpl->rz.y, ptmpl->tLife, ptmpl->nKind);
    if (pa->g10D0 == 1.0f) PnoiseNew(ptmpl, PosLoWorld(pa->palo), pa->palo);
}
}  // namespace

bool g_fActorTargetsImported = false;
void UpdateActor(ACTOR* pa)
{
    UpdateStep(pa);
    UpdateActorAbilities(pa);   // 0x141A50 (the gadgets on L1 / L2 / R2: gadget.cpp)
    if (!dynamic_cast<SLY*>(pa)) UpdateActorDrawAlpha(pa);
    // L3: the counters and the job markers (0x1C46C0(+0x150, 0, 0x200) -> 0x140338)
    if (PADCTL* pc = PpadctlOfActor(pa); pc && GrfPadctlTakePressed(pc, BTN_L3, 0.0f)) ShowWorldStats();
    // 0x1403D0: the top job's slot14 takes precedence over the script runner, after the actor-watch update.
    if (std::getenv("SLY2N_DRIVEWHY") && dynamic_cast<SLY*>(pa))
        std::printf("drivewhy t %.3f df4 %d watch %d jobs %zu run %p\n", g_clock.t, pa->nSubDF4, pa->watch.fActive ? 1 : 0, pa->stack.ajob.size(), pa->prunScript);
    if (pa->nSubDF4 == -1)
    {
        UpdateActorWatch(pa);  // 0x144640
        if (!pa->watch.fActive)
            if (PADCTL* pc = PpadctlOfActor(pa))
            {
                if (!pa->stack.ajob.empty()) pa->stack.ajob.back()->Drive(pc);
                else if (pa->prunScript) DriveScptActor(pa->prunScript, pc);
            }
        static const bool s_fDbg = std::getenv("SLY2N_DRIVEPRINT") != nullptr;
        if (s_fDbg && pa->prunScript)
        {
            PADCTL* pc = PpadctlOfActor(pa);
            std::printf("drive %d: padctl %p control %d\n", pa->palo->oid, static_cast<void*>(pc), pc ? pc->nControl : -9);
        }
        if (s_fDbg && pa->prunScript && PpadctlOfActor(pa))
            std::printf("   -> u %.2f rad %.2f btn 0x%X vecVel %.1f %.1f %.1f uTurn %.2f floor %d pos %.1f %.1f %.1f\n",
                        PpadctlOfActor(pa)->uJoy, PpadctlOfActor(pa)->radJoy, PpadctlOfActor(pa)->grfbtn, pa->vecVel.x, pa->vecVel.y,
                        pa->vecVel.z, pa->uTurn, pa->pxpFloor ? 1 : 0, PosAloCenter(pa->palo).x, PosAloCenter(pa->palo).y,
                        PosAloCenter(pa->palo).z);
    }
    // the targets (0x140558); the harness's import of the reference's (SLY2N_HARNESS_REFTARGET) replaces it
    if (!g_fActorTargetsImported) PickActorTargets(pa);
    UpdateActorWater(pa);  // 0x1A2460
    UpdateActorNoise(pa);  // 0x140750
    UpdateActorLook(pa);   // 0x142AF8
    if (!pa->watch.fActive) UpdateJobs(pa);  // 0x1EED10 (also during damage, as in 0x1403D0)
}

namespace
{
// orig: 0x0011E630 (the parameters of the nearest points of the lines p0 p1 and q0 q1; parallel: 0, 0)
void ClosestLineParams(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1, float* pu, float* pv)
{
    const glm::vec3 dp = p1 - p0, dq = q1 - q0, w = q0 - p0;
    const glm::vec3 n = glm::cross(dp, dq);
    const float n2 = (n.x * n.x + n.y * n.y) + n.z * n.z;
    if (n2 < F32(0x38D1B717))
    {
        *pu = *pv = 0.0f;
        return;
    }
    const glm::vec3 a = glm::cross(w, dq), b = glm::cross(w, dp);
    *pu = ((a.x * n.x + a.y * n.y) + a.z * n.z) / n2;
    *pv = ((b.x * n.x + b.y * n.y) + b.z * n.z) / n2;
}
// orig: 0x0011E7C8 (the distance of the segments' nearest points, each parameter clamped to [0, 1])
float DistSegments(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1)
{
    float u = 0.0f, v = 0.0f;
    ClosestLineParams(p0, p1, q0, q1, &u, &v);
    u = u < 0.0f ? 0.0f : (1.0f < u ? 1.0f : u);
    v = v < 0.0f ? 0.0f : (1.0f < v ? 1.0f : v);
    const glm::vec3 d = ((p0 * (u - 1.0f) + p1 * -u) + q0 * (1.0f - v)) + q1 * v;
    return std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
}

// orig: 0x0018BCE0's end: the cane reaching for +0x1854 in mode 0 (0x18ED70: the reach controller +0x17D0; its curve
// mode follows the target's distance nearest the actor (0x1E7380 from +0x1858)); the swept segments of the cane's
// kept positions (+0x1890..: the actor, the cane before / after the physics) against their nearest points of the target
// within +0x19AC: a pipe -> state 8 at that distance; a point (0x13) -> state 3 (22 by its +0x124)
void UpdateSlyHook(SLY* p)
{
    if (p->mode == 0) UpdateReach(&p->reach);  // 0x18ED70 (0x1CC2E8)
    if (p->mode != 0 || !p->reach.ploTarget) return;
    LO* plo = p->reach.ploTarget;
    const CRV* pcrv = FIsLoDerivedFrom(plo, 0x0B) ? PcrvJtPipe(plo) : nullptr;
    glm::vec3 anp[3];
    if (pcrv)
    {
        for (int i = 0; i < 3; ++i) NearestCrvS(pcrv, glm::vec3(p->aq[i]), p->reach.sTarget, &anp[i], nullptr, nullptr, nullptr);  // 0x1E7380
    }
    else if (FIsLoDerivedFrom(plo, 0x13))
    {
        for (glm::vec3& v : anp) v = PosJtLo(plo);
    }
    else return;
    if (std::getenv("SLY2N_JTPRINT"))
    {
        const glm::vec3 c = p->plo19A4 ? PosAloCenter(static_cast<ALO*>(p->plo19A4)) : glm::vec3(0.0f);
        const glm::vec3 w = p->plo19A4 ? glm::vec3(MatWorldOfAlo(static_cast<ALO*>(p->plo19A4))[3]) : glm::vec3(0.0f);
        std::printf("hook t %.3f cq %d aq %.1f %.1f %.1f | %.1f %.1f %.1f | %.1f %.1f %.1f; cane now %.1f %.1f %.1f (world %.1f %.1f %.1f)\n", g_clock.t, p->cq, p->aq[0].x, p->aq[0].y, p->aq[0].z,
                    p->aq[1].x, p->aq[1].y, p->aq[1].z, p->aq[2].x, p->aq[2].y, p->aq[2].z, c.x, c.y, c.z, w.x, w.y, w.z);
    }
    for (int i = 1; i < p->cq; ++i)
    {
        const float d = DistSegments(glm::vec3(p->aq[i - 1]), glm::vec3(p->aq[i]), anp[i - 1], anp[i]);
        if (std::getenv("SLY2N_JTPRINT")) std::printf("hook: segment %d d %.2f (np %.1f %.1f %.1f -> %.1f %.1f %.1f)\n", i, d, anp[i - 1].x, anp[i - 1].y, anp[i - 1].z, anp[i].x, anp[i].y, anp[i].z);
        if (!(d < p->g19AC)) continue;
        if (pcrv)
        {
            p->ploF5C = plo;
            p->sCrvF50 = p->reach.sTarget;
            if (std::getenv("SLY2N_JTPRINT")) std::printf("sly: the cane caught pipe %d at s %.2f (d %.2f)\n", plo->oid, p->reach.sTarget, d);
            SetSlyState(p, 8, -1);
        }
        else
        {
            // a hook (cid 0x13): +0x18C4; its kind +0x124 3: the hang-follow (0x16), else the swing (3, 1)
            p->ploOther = plo;
            if (plo->U32Field(0, 0x124, 0) == 3) SetSlyState(p, 0x16, -1);
            else SetSlyState(p, 3, 1);
        }
        return;
    }
}
}  // namespace

// Observer only: match original 0x18BCE0 entry and retain the final update separately.
static void TraceFeralFrame(SLY* p, const char* prefix)
{
    static const bool s_fFeral = std::getenv("SLY2N_FERALPRINT") != nullptr;
    if (s_fFeral)
    {
        const PADCTL* pad = PpadctlOf(p);
        const glm::vec3 pos = PosAloCenter(p->palo), vel = VelAloPos(p->palo);
        const ASEGA* clip = p->pasega;
        std::fprintf(stderr, "%s {\"time\":%.9g,\"dt\":%.9g,\"state\":%d,\"substate\":%d,\"jump_kind\":%d,\"jump_state\":%d,\"kind\":%d,\"interaction\":%d,\"power\":%d,\"clip\":%d,\"clip_time\":%.9g,\"clip_speed\":%.9g,\"clip_flags\":%d,\"floor\":%s,\"held\":%u,\"joy\":%.9g,\"heading\":%.9g,\"position\":[%.9g,%.9g,%.9g],\"velocity\":[%.9g,%.9g,%.9g],\"desired\":[%.9g,%.9g,%.9g]}\n",
                     prefix,g_clock.t,g_clock.dt,p->state,p->substate,p->jump.kind,p->jump.state,p->n4C8,p->nSub4CC,NActorPower(p),
                     clip&&clip->paseg?clip->paseg->oid:-1,clip?clip->t:0.0f,clip?clip->svt:0.0f,clip?clip->grfapl:0,
                     p->pxpFloor?"true":"false",pad?pad->grfbtn:0,pad?pad->uJoy:0,p->radHeading,
                     pos.x,pos.y,pos.z,vel.x,vel.y,vel.z,p->vecVel.x,p->vecVel.y,p->vecVel.z);
    }
}

// (0x189618's sliding-rail part: the three-clip lean blend +0x314)
void UpdateSlyRailBlend(SLY* p, const CRV* pcrv, glm::vec3 dir)
{
    INTERACT& i = p->inter;
    dir = glm::normalize(dir);
    const glm::vec3 vel = VelAloPos(p->palo);  // vt slot 33
    const float gSpeed = glm::dot(vel, dir);
    const glm::vec3 vecLean = dir * gSpeed;
    float sAhead = i.s20 + gSpeed * F32(0x3DCCCCCD);  // D_002BF2D0 0.1
    const float sMax = pcrv->as.empty() ? 0.0f : pcrv->as.back();
    if (pcrv->fClosed) sAhead = GModPositive(sAhead, sMax);
    else
    {
        const float sMin = pcrv->as.empty() ? 0.0f : pcrv->as.front();
        if (sAhead < sMin) sAhead = sMin;
        else if (sMax < sAhead) sAhead = sMax;
    }
    EvalCrvS(pcrv, sAhead, nullptr, &dir);  // GetShapePosDirAtS
    const glm::vec3 dv = glm::normalize(dir) * gSpeed - vecLean;
    if (!i.pbl314) return;
    const glm::vec3 across = glm::normalize(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), dir));
    const float gAcross = glm::dot(dv, across);  // SProjectVector
    float g = std::clamp(gAcross / F32(0x447A0000), -1.0f, 1.0f);  // D_002BF2D4 1000
    if (i.f24) g = -g;
    static const cmm::SMPA s_smpa{F32(0x40800000), 0.0f, F32(0x3F000000), F32(0x41200000)};  // D_002BF320 {4, 0, 0.5, 10}
    float dgNext = i.g31C;
    i.g318 = cmm::GSmoothA(i.g318, i.g31C, g * (2.0f - std::fabs(g)), g_clock.dt, &s_smpa, &dgNext);
    i.g31C = dgNext;
    const float w0 = std::max(0.0f, i.g318), w2 = std::max(0.0f, -i.g318);
    SetBlendWeights(i.pbl314, {w0, (1.0f - w0) - w2, w2});  // ReblendAsegbl
}


// orig: 0x00189618 (UpdateJtLean): the body's lean. The actuators +0x19E4 / +0x19E8 / +0x19EC (slide.aphandle[0..2],
// the priority-5 source controllers of the records 0x138 / 0xD / 0xC made by 0x188608) go to zero in the look mode.
// Standing on an object (its floor contact's +0x4C) or holding one (state 7, the interaction's sub-state 0: kind 4
// an object at its position, 3 a SHAPE at u): the velocity of that object at the point (0x22D348) seen from Sly's
// target frame (+0x1B0) gives the lean: k = 2u - u^2 of u = |v.xy| / 1000 (D_002BF310 / D_002BF300, 0..1), the
// lean (v.xy * 20 k / |v.xy|, -7.5 sqrt(1 - k^2)) (D_002BF2D8 / D_002BF2DC), its z kept at or below half of each
// foot's offset (+0x19F0 / +0x19F4's z, D_002BF330 {0, 0.5, 0}); +0x19E4 gets it, +0x19E8 / +0x19EC half of it
// (D_002BF2E0) while no jump state is set (+0x2DC -1), else zero. On a sliding rail (the SHAPE's +0x1BC) the change
// of Sly's velocity along the rail 0.1 s ahead (D_002BF2D0), across the rail's tangent and the vertical, scaled by
// 1 / 1000 (D_002BF2D4) into -1..1 (flipped when going against the rail +0x24), first drives the three-clip lean
// blend +0x314 (weights left / middle / right), smoothed by GSmoothA (+0x318 the value, +0x31C its rate, D_002BF320).
// (Standing, the original reads the point from an unset stack slot; natively Sly's position: only a moving floor's
// velocity depends on it.)
void UpdateSlyLean(SLY* p)
{
    CTRLSOURCE** ah = p->slide.aphandle;
    if (!ah[0] || !FXfHasCtrl(ah[0]->pxf, ah[0])) return;  // 0x12D058
    auto set = [](CTRLSOURCE* pc, const glm::vec3& v) {
        if (pc) SetCtrlSourceOffset(pc, glm::vec4(v, 0.0f));  // 0x12DA30
    };
    if (p->nF04 != -1)  // (FJtModeActive: the look / binocucom mode)
    {
        set(ah[0], glm::vec3(0.0f));
        set(ah[1], glm::vec3(0.0f));
        set(ah[2], glm::vec3(0.0f));
        return;
    }
    LO* plo = nullptr;
    glm::vec3 pos = PosAloCenter(p->palo);
    if (p->state == 0)
    {
        if (XP* pxp = p->pxpFloor) plo = pxp->axpd[1].palo;  // (+0x4C)
    }
    else if (p->state == 7 && p->nSub4CC == 0)  // (+0x4CC 0; +0x4C8 the kind: 4 an object, 3 a SHAPE)
    {
        INTERACT& i = p->inter;
        if (p->n4C8 == 4)
        {
            plo = i.ploTarget;
            if (plo) pos = PosLoWorld(plo);
        }
        else if (p->n4C8 == 3)
        {
            plo = i.ploTarget;
            const CRV* pcrv = plo ? PcrvFromLo(plo) : nullptr;
            if (pcrv)
            {
                glm::vec3 dir(0.0f);
                EvalCrv(pcrv, i.g1C, &pos, &dir);  // GetShapePosDirAtU
                if (plo->U32Field(0, 0x1BC, 0)) UpdateSlyRailBlend(p, pcrv, dir);
            }
        }
    }
    if (!plo) return;
    XF* pxf = nullptr;  // (vt slot 32: an ALO's object record, a LOX's record)
    if (FIsLoDerivedFrom(plo, Icid(CID3::ALO))) pxf = PxfObj(static_cast<ALO*>(plo));
    else if (FIsLoDerivedFrom(plo, 3)) pxf = static_cast<LOX*>(plo)->pxf;
    const glm::vec3 v = pxf ? VelXfPoint(pxf, nullptr, pos) : glm::vec3(0.0f);  // 0x22D348
    const glm::vec3 local = glm::transpose(p->matHeading) * v;  // (the rows +0x1B0)
    const float gLen = std::sqrt(local.x * local.x + local.y * local.y);
    glm::vec3 lean(0.0f);
    if (gLen > 0.0001f)
    {
        const float u = std::min(gLen / 1000.0f, 1.0f);
        const float k = std::clamp(0.0f + u * (2.0f + u * -1.0f), 0.0f, 1.0f);
        lean = glm::vec3(local.x * 20.0f * k / gLen, local.y * 20.0f * k / gLen, -7.5f * std::sqrt(1.0f - k * k));
    }
    float gFootL = ah[3] ? 0.5f * ah[3]->ofs.z : 0.0f;
    float gFootR = ah[4] ? 0.5f * ah[4]->ofs.z : 0.0f;
    if (lean.z < gFootL && lean.z < gFootR) gFootR = lean.z;
    else if (gFootL < gFootR) gFootR = gFootL;
    set(ah[0], glm::vec3(lean.x, lean.y, gFootR));
    const glm::vec3 half = lean * 0.5f;
    set(ah[2], p->jump.state == -1 ? half : glm::vec3(0.0f));
    set(ah[1], p->jump.state == -1 ? half : glm::vec3(0.0f));
    if (static const bool s_f = std::getenv("SLY2N_LEANPRINT") != nullptr; s_f)
        std::printf("lean t %.3f state %d obj %d v (%.1f %.1f %.1f) 19E4 (%.2f %.2f %.2f) feet %.2f %.2f\n", g_clock.t, p->state, plo->oid, v.x, v.y, v.z,
                    lean.x, lean.y, gFootR, ah[3] ? ah[3]->ofs.z : 0.0f, ah[4] ? ah[4]->ofs.z : 0.0f);
}

// orig: 0x0018BCE0
void (*g_pfnSlyUpdated)(SLY* p) = nullptr;
void UpdateSly(LO* plo)
{
    SLY* p = PslyFromLo(plo);
    if (!p) return;
    TraceFeralFrame(p, "FERAL_BEGIN");
    static bool s_fJtPrinted = false;
    // (debug: SLY2N_JTPRINT=1 at the first update, =N at Sly's N-th update: after the proxies' streaming)
    static int s_cJtUpdate = 0;
    ++s_cJtUpdate;
    if (!s_fJtPrinted && std::getenv("SLY2N_JTPRINT") && s_cJtUpdate >= std::atoi(std::getenv("SLY2N_JTPRINT")))
    {
        s_fJtPrinted = true;
        PrintJtLists(s_psw);
    }
    // (debug: SLY2N_TELEPORT="X Y Z@N[,H|,k]" or "X Y Z@tT[,H][/wN]" puts Sly there at his N-th update, heading H (",k": kept), as the reference's
    // --dev-teleport (the actors' slot 38 path: TeleportActor's slots 29 / 30 / 35 / 36))
    static const char* s_szTele = std::getenv("SLY2N_TELEPORT");
    if (s_szTele)
    {
        // (several separated by ';', each "X Y Z@N[,H]")
        static int s_cUpdate = 0;
        const int iUpdate = s_cUpdate++;
        for (const char* q = s_szTele; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
        float x = 0, y = 0, z = 0, h = 0, t = 0;
        int n = 0;
        // ("X Y Z@tT[,H]": at the first update at game time >= T)
        static std::vector<const char*> s_aqDone;
        const bool fTime = std::strstr(q, "@t") && (!std::strchr(q, ';') || std::strstr(q, "@t") < std::strchr(q, ';'));
        const int c = fTime ? std::sscanf(q, "%f %f %f@t%f,%f", &x, &y, &z, &t, &h)
                            : std::sscanf(q, "%f %f %f@%d,%f", &x, &y, &z, &n, &h);
        // ("...@tT/wN": only in the N-th world loaded through a transition, so times after a world change work)
        const char* qw = std::strstr(q, "/w");
        const bool fWorldOk = !qw || (std::strchr(q, ';') && qw > std::strchr(q, ';')) || std::atoi(qw + 2) == g_cTransitionLoads;
        const bool fNow = fWorldOk && (fTime ? (t <= g_clock.t && std::find(s_aqDone.begin(), s_aqDone.end(), q) == s_aqDone.end())
                                             : iUpdate == n);
        if (fTime && fNow) s_aqDone.push_back(q);
        const char* qEndR = std::strchr(q, ';');
        const char* qr = std::strstr(q, ",r");
        const bool fRef = qr && (!qEndR || qr < qEndR);
        if (c >= 4 && fNow && fRef)
        {
            // (",r": as the reference's --dev-teleport: slot 29 (0x143CF8: the point as given, the safe ground's
            // count +0x9E4 = 0), slots 35 / 36 zero velocities, the heading kept)
            SetStepPos(p, glm::vec3(x, y, z));
            p->safe.c = 0;
            SetAloVelocityTo(p->palo, glm::vec3(0.0f));
            SetAloAngVelocityTo(p->palo, glm::vec3(0.0f));
            CutCm(&g_cm);
        }
        else if (c >= 4 && fNow)
        {
            SetStepPos(p, glm::vec3(x, y, z) - glm::vec3(p->vecBase));
            const float hh = c >= 5 ? h : 0.0f;
            glm::mat3 r(1.0f);
            r[0] = glm::vec3(std::cos(hh), std::sin(hh), 0.0f);
            r[1] = glm::vec3(-std::sin(hh), std::cos(hh), 0.0f);
            // (",k" after the frame: the heading kept)
            const char* qEnd = std::strchr(q, ';');
            const char* qk = std::strstr(q, ",k");
            if (!qk || (qEnd && qk > qEnd)) SetStepRotation(p, r);
            SetAloVelocityTo(p->palo, glm::vec3(0.0f));
            CutCm(&g_cm);
        }
        }
    }
    // (debug: a route's "teleport" step, route.cpp; left to gang.cpp's ApplyRouteTeleportActor when Bentley or Murray is
    // the current player, e.g. Ep1 Heist 1904 after 1902 hands control to Bentley)
    LO* pcurTele = g_routetele.fPending && g_psw ? PloPoCurrent(g_psw) : nullptr;
    if (g_routetele.fPending && !(pcurTele && pcurTele != static_cast<LO*>(p->palo) &&
                                  (FIsLoDerivedFrom(pcurTele, Icid(CID3::MURRAY)) || FIsLoDerivedFrom(pcurTele, Icid(CID3::BENTLEY)))))
    {
        g_routetele.fPending = false;
        SetStepPos(p, glm::vec3(g_routetele.x, g_routetele.y, g_routetele.z) - glm::vec3(p->vecBase));
        if (g_routetele.fH)
        {
            glm::mat3 r(1.0f);
            r[0] = glm::vec3(std::cos(g_routetele.h), std::sin(g_routetele.h), 0.0f);
            r[1] = glm::vec3(-std::sin(g_routetele.h), std::cos(g_routetele.h), 0.0f);
            SetStepRotation(p, r);
        }
        SetAloVelocityTo(p->palo, g_routetele.fVel ? g_routetele.vel : glm::vec3(0.0f));
        g_routetele.fVel = false;
        CutCm(&g_cm);
    }
    // (debug: SLY2N_NEARLIST="N:R": at Sly's N-th update, every in-world object within R of him: oid, class, place)
    if (static const char* s_szNear = std::getenv("SLY2N_NEARLIST"); s_szNear && s_psw)
    {
        static int s_c = 0;
        const char* q = std::strchr(s_szNear, ':');
        const float r = q ? std::strtof(q + 1, nullptr) : 1000.0f;
        if (s_c++ == std::atoi(s_szNear))
        {
            const glm::vec3 me = PosLoWorld(p->palo);
            for (const auto& plo : s_psw->aploAll)
            {
                if (!plo || !plo->fInWorld || plo.get() == p->palo) continue;
                const glm::vec3 w = PosLoWorld(plo.get());
                if (glm::length(w - me) < r)
                    std::printf("nearlist: %d cid 0x%X at (%.0f %.0f %.0f) dist %.0f parent %d\n", plo->oid, plo->Cid(), w.x, w.y, w.z,
                                glm::length(w - me), plo->paloParent ? plo->paloParent->oid : -1);
            }
        }
    }
    // (debug: SLY2N_BRKLIST=N: at Sly's N-th update, every breakable (cid 0x62): its parent, world state, contacts' owner)
    if (static const char* s_szBrk = std::getenv("SLY2N_BRKLIST"); s_szBrk && s_psw)
    {
        static int s_c = 0;
        if (s_c++ == std::atoi(s_szBrk))
            for (LO* plo : s_psw->mpcidaplo[0x62])
            {
                ALO* palo = static_cast<ALO*>(plo);
                const glm::vec3 c = PosAloCenter(palo);
                std::printf("brklist: %d parent %d inworld %d removed %d oxa %d soes %zu at (%.0f %.0f %.0f) origin (%.0f %.0f %.0f)\n", plo->oid,
                            plo->paloParent ? plo->paloParent->oid : -1, plo->fInWorld, plo->fRemoved, Poxa(palo) ? 1 : 0,
                            palo->asoe.size(), c.x, c.y, c.z, MatWorldOfAlo(palo)[3].x, MatWorldOfAlo(palo)[3].y, MatWorldOfAlo(palo)[3].z);
            }
    }
    // (debug: SLY2N_TELEGIVER="OID@N[,D]" puts Sly D (default 150) behind the first guard OID in the world at his N-th
    // update, facing it: a pickpocket in the real game; OID -1: the first guard with a prize)
    static const char* s_szGiver = std::getenv("SLY2N_TELEGIVER");
    // (the giver teleport: OID's guard / object; OID -1: the first guard with a prize; D behind it)
    const auto giver = [&](int oid, float d) {
        for (const auto& up : s_psw->aploAll)
        {
            LO* pg = up.get();
            // (OID -1: the first guard with a prize in its pocket, mode 2: the sparkle)
            if ((oid >= 0 ? pg->oid != oid : (!FCidDerivedFrom(pg->Cid(), Icid(CID3::GUARD)) || PocketOf(pg).nMode != 2)) || !pg->fInWorld)
                continue;
            if (!FCidDerivedFrom(pg->Cid(), Icid(CID3::ALO)))
            {
                // (any other object: Sly at its place, D below it, e.g. a switch's use point)
                const glm::vec3 pos = PosLoWorld(pg) - glm::vec3(0.0f, 0.0f, d);
                SetStepPos(p, pos - glm::vec3(p->vecBase));
                SetAloVelocityTo(p->palo, glm::vec3(0.0f));
                CutCm(&g_cm);
                std::printf("telegiver: Sly at %d (cid 0x%X) (%.0f %.0f %.0f)\n", pg->oid, pg->Cid(), pos.x, pos.y, pos.z);
                return;
            }
            ALO* pag = static_cast<ALO*>(pg);
            const glm::mat4 m = MatWorldOfAlo(pag);
            glm::vec3 fwd = glm::normalize(glm::vec3(m[0].x, m[0].y, 0.0f));
            glm::vec3 pos = PosAloCenter(pag) - fwd * d;
            // (a guard with a pickpocket volume (+0x3F4, option 304): Sly at its center (the guard's height), facing
            // the guard: the matrix row is not the walk's heading for every guard, e.g. Ep1's repairman 11539)
            if (GUARD* pgd = PguardFromLo(pg); pgd && pgd->plo3F4)
            {
                LO* pv = pgd->plo3F4;
                const glm::vec3 c = FIsLoDerivedFrom(pv, Icid(CID3::ALO)) ? PosAloCenter(static_cast<ALO*>(pv)) : PosLoWorld(pv), g = PosAloCenter(pag);
                if (glm::length(glm::vec2(g - c)) > 1.0f)
                {
                    pos = glm::vec3(c.x, c.y, g.z);
                    fwd = glm::normalize(glm::vec3(g.x - c.x, g.y - c.y, 0.0f));
                }
            }
            SetStepPos(p, pos - glm::vec3(p->vecBase));
            const float hh = std::atan2(fwd.y, fwd.x);
            glm::mat3 r(1.0f);
            r[0] = glm::vec3(std::cos(hh), std::sin(hh), 0.0f);
            r[1] = glm::vec3(-std::sin(hh), std::cos(hh), 0.0f);
            SetStepRotation(p, r);
            SetAloVelocityTo(p->palo, glm::vec3(0.0f));
            CutCm(&g_cm);
            std::printf("telegiver: Sly behind %d at (%.0f %.0f %.0f) heading %.3f\n", pg->oid, pos.x, pos.y, pos.z, hh);
            return;
        }
    };
    if (s_szGiver && s_psw)
    {
        // (several separated by ';')
        static int s_cUpdate = 0;
        const int iUpdate = s_cUpdate++;
        for (const char* q = s_szGiver; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            int oid = 0, n = 0;
            float d = 150.0f;
            const int c = std::sscanf(q, "%d@%d,%f", &oid, &n, &d);
            if (c >= 2 && iUpdate == n) giver(oid, d);
        }
    }
    // (debug: a route's "giver" step, route.cpp)
    if (g_routetele.fGiver && s_psw)
    {
        g_routetele.fGiver = false;
        giver(g_routetele.oidGiver, g_routetele.dGiver);
    }
    UpdateActor(p);
    if (static const bool s_fFloor = std::getenv("SLY2N_FLOORPRINT") != nullptr; s_fFloor)  // (debug: Sly's floor contact)
    {
        const XP* px = p->pxpFloor;
        const ALO* pr = px ? px->axpd[1].paloRoot : nullptr;
        const ALO* po = px ? px->axpd[1].palo : nullptr;
        const glm::vec3 v = px ? px->axpd[1].vel : glm::vec3(0.0f);
        const glm::vec3 s = PosAloCenter(p->palo);
        std::printf("floor t %.3f st %d sly (%.0f %.0f %.0f) root %d obj %d vel (%.1f %.1f %.1f) rootvel (%.1f %.1f %.1f)\n", g_clock.t, p->state,
                    s.x, s.y, s.z, pr ? pr->oid : -1, po ? po->oid : -1, v.x, v.y, v.z, pr ? VelAloPos(const_cast<ALO*>(pr)).x : 0.f,
                    pr ? VelAloPos(const_cast<ALO*>(pr)).y : 0.f, pr ? VelAloPos(const_cast<ALO*>(pr)).z : 0.f);
    }
    if (!plo->fInWorld) return;
    if (PpadctlOf(p)) UpdateSlyInput(p);
    switch (p->state)
    {
    case 0: UpdateSlyStanding(p); break;
    case 1:
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitWalk, 1.0f));
        UpdateGaitFeet(&p->gaitWalk);
        p->vecVel *= F32(0x3FA66666);  // 1.3 (0x2BF3D4)
        break;
    case 12: UpdateGaitFeet(&p->gaitRun); break;
    case 5:
        if (p->substate == 5)
        {
            SteerActorRootBlend(p, &p->rb13D0);  // 0x1A0450
            p->vecVel *= F32(0x3F59999A);        // 0.85
            p->uTurn = 1.0f;
        }
        break;
    case 10: SteerActorRootBlend(p, &p->rb1AC0); break;  // 0x1A0450
    case 0x15: UpdateSlyClimbSurf(p); break;  // 0x185E40
    case 0x16: UpdateSlyHangFollow(p); break;  // 0x18DF60
    case 24: UpdateSlySlideFeet(p); break;  // 0x189C40 / 0x189EC8
    default: break;
    }
    UpdateSlyHook(p);  // 0x18ED70 (+0x17D0) and the mode-0 test (+0x1854)
    UpdateSlyLean(p);  // 0x189618
    UpdateActorDrawAlpha(p);  // orig: 0x1425B0 observes the final interaction state, after 0x18BCE0
    TraceFeralFrame(p, "FERAL_FRAME");
    static const bool s_fShadow = std::getenv("SLY2N_SHADOWPRINT") != nullptr;
    if (s_fShadow)
    {
        const CSnd* sound = PslyGlobal() ? PslyGlobal()->snd2048.get() : nullptr;
        const auto* plain = dynamic_cast<const CSndPlain*>(sound);
        const PADCTL* pad = PpadctlOf(p);
        std::fprintf(stderr, "SHADOW_FRAME {\"time\":%.9g,\"dt\":%.9g,\"oid\":%d,\"state\":%d,\"substate\":%d,\"kind\":%d,\"interaction\":%d,\"power\":%d,\"alpha\":[%.9g,%.9g],\"clip\":%d,\"flags\":%d,\"sound\":%s,\"sound_id\":%d,\"sound_dirty\":%u,\"held\":%u,\"joy\":%.9g,\"desired\":[%.9g,%.9g,%.9g],\"heading\":%.9g}\n",
                     g_clock.t,g_clock.dt,p->palo->oid,p->state,p->substate,p->n4C8,p->nSub4CC,NActorPower(p),p->g10D4,p->g10D8,
                     p->pasega&&p->pasega->paseg?p->pasega->paseg->oid:-1,p->pasega?p->pasega->grfapl:0,sound?"true":"false",plain?plain->m_sfxid:-1,
                     sound?unsigned(sound->m_grfDirty):0,pad?pad->grfbtn:0,pad?pad->uJoy:0,p->vecVel.x,p->vecVel.y,p->vecVel.z,p->radHeading);
    }
    if (g_pfnSlyUpdated) g_pfnSlyUpdated(p);
    static const bool s_fCane = std::getenv("SLY2N_CANEPRINT") != nullptr;
    if (s_fCane && p->mode != -1)
        for (const SOE& e : p->palo->asoe)
            if (e.id & 0x3E)
                std::printf("cane t %.3f mode %d entry tag %d id 0x%X off %d state %d\n", g_clock.t, p->mode, e.tag, e.id, e.f50 ? 1 : 0, e.state);
    // (debug: SLY2N_HIDETRACE=1: per frame the jump's state, the interaction's kind / state, the position)
    static const bool s_fHideTrace = std::getenv("SLY2N_HIDETRACE") != nullptr;
    if (s_fHideTrace)
    {
        const glm::vec3 pos = PosAloCenter(p->palo);
        LO* pt = p->n4C8 == 1 ? p->inter.ploTarget : p->jump.ploC;
        const glm::vec3 posT = pt && FCidDerivedFrom(pt->Cid(), Icid(CID3::ALO)) ? PosAloCenter(static_cast<ALO*>(pt)) : glm::vec3(0.0f);
        std::printf("hidetrace t %.4f jump %d kind %d sub %d state %d pos %.2f %.2f %.2f obj %.2f %.2f %.2f\n", g_clock.t, p->jump.state, p->n4C8,
                    p->nSub4CC, p->state, pos.x, pos.y, pos.z, posT.x, posT.y, posT.z);
    }
    if (static const char* s_szHit = std::getenv("SLY2N_HIDEHIT"); s_szHit && p->n4C8 == 1 && std::atof(s_szHit) < g_clock.t)
    {
        void DebugHitHide(LO* plo);  // hide.cpp
        DebugHitHide(p->inter.ploTarget);
    }
    static const bool s_fPrint = std::getenv("SLY2N_SLYPRINT") != nullptr;
    static int s_c = 0;
    if (s_fPrint && (s_c++ % (std::getenv("SLY2N_SLYPRINT")[0] == '1' ? 15 : 1)) == 0)
    {
        const glm::vec3 pos = PosAloCenter(p->palo);
        const SOST* pst = Psost(p->palo);
        std::printf("sly t %.2f: h %.3f state %d/%d jump %d pos %.1f %.1f %.1f lin %d g %.0f floor %d in world %d\n", g_clock.t, p->radHeading, p->state,
                    p->substate, p->jump.state, pos.x, pos.y, pos.z, pst ? pst->cnstrLin.kind : -1, pst ? pst->dvGravity.z : 0.0f,
                    p->pxpFloor ? 1 : 0, plo->fInWorld ? 1 : 0);
        std::printf("   anim: clip %d t %.3f inter %d/%d mode %d rank %d cane %d/%d\n", p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -2,
                    p->pasega ? p->pasega->t : 0.0f, p->n4C8, p->nSub4CC, p->mode, p->n10E4, p->plo19A0 ? p->plo19A0->fInWorld : -1,
                    p->plo19A8 ? p->plo19A8->fInWorld : -1);
        if (const ASEGBL* pbl = p->pasega ? dynamic_cast<const ASEGBL*>(p->pasega->paseg) : nullptr)
        {
            std::printf("   blend: side %d vel %.2f %.2f %.2f svt %.2f w", p->n1038, p->vecVel.x, p->vecVel.y, p->vecVel.z, p->pasega->svt);
            for (const BLENDBL& bl : pbl->abl) std::printf(" %.2f", bl.weight);
            std::printf("\n");
        }
        if (PADCTL* pc = PpadctlOf(p))
            std::printf("   padctl control %d u %.2f rad %.2f btn 0x%X script %p cur %d\n", pc->nControl, pc->uJoy, pc->radJoy, pc->grfbtn, p->prunScript,
                        PloPoCurrent(s_psw) ? PloPoCurrent(s_psw)->oid : -1);
        else std::printf("   no padctl\n");
        if (OXA* poxa = Poxa(p->palo))
        {
            std::printf("   contacts:");
            for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
                for (XP* pxp = pox->pxp; pxp; pxp = pxp->pxpNext)
                {
                    const ALO* po = pxp->axpd[0].paloRoot == p->palo ? pxp->axpd[1].paloRoot : pxp->axpd[0].paloRoot;
                    std::printf(" %d(cid 0x%X n %.2f %.2f %.2f)", po ? po->oid : -1, po ? po->Cid() : 0, pxp->normal.x, pxp->normal.y, pxp->normal.z);
                }
            std::printf("\n");
        }
    }
    if (FSlyHarnessOn()) CompareSlyFrame(s_psw, g_iframeHarness);  // (slycheck.cpp: at the reference trace's point)
}

namespace
{
// orig: 0x0018A278 (vt slot 17: the kept positions, the ALO's physics 0x1F3980)
void UpdateSlyPhys(ALO* palo)
{
    SLY* p = PslyFromLo(palo);
    if (!p) return;
    // (0x1A62E8: the object's position: its object record's, not the root matrix's)
    p->aq[1] = glm::vec4(p->plo19A4 ? PosAloCenter(static_cast<ALO*>(p->plo19A4)) : glm::vec3(0.0f), 1.0f);
    p->cq = 0;
    UpdateAloPhys(palo);
    if (p->mode)
    {
        p->cq = 0;
        return;
    }
    p->aq[0] = glm::vec4(PosAloCenter(palo), 1.0f);
    p->aq[2] = glm::vec4(p->plo19A4 ? PosAloCenter(static_cast<ALO*>(p->plo19A4)) : glm::vec3(0.0f), 1.0f);
    p->cq = 3;
}

// orig: 0x00188960 (vt slot 70: the step's motion (0x204108) unless states 8 / 3 (gravity only) or 21 / 22; the
// states' own parts)
void UpdateSlyMotion(ALO* palo)
{
    SLY* p = PslyFromLo(palo);
    if (!p) { UpdateStepPhysMotion(palo); return; }
    if (p->state == 8 || p->state == 3) ApplyAloGravity(palo);
    else if (p->state != 0x15 && p->state != 0x16) UpdateStepPhysMotion(palo);
    switch (p->state)
    {
    case 2: UpdateJumpMotion(&p->jump); break;  // 0x196DF8
    case 4: UpdateDamageMotion(p); break;        // 0x1A1148
    case 8: UpdateClimbMotion(p); break;         // 0x19DF70 (+0xF50)
    case 7: UpdateInteractMotion(p); break;     // 0x194170 (+0x4C0)
    case 14:
    {
        // orig: 0x00188960: follow the air slam's root velocity until its impact marker.
        ASEGA* pasega = p->pasega;
        if (!pasega) break;
        const glm::vec3 v = VelAloPos(palo);
        glm::vec4 root(0.0f);
        if (!FAsegaRetracted(pasega))
            if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, palo, 1, true); pchn && pchn->ptrkp)
                pchn->ptrkp->Eval(nullptr, &root, pasega->t, pasega->svt);
        glm::vec3 wanted(root);
        const float impact = GEventAseg11(PPart<ASEG>(palo, 0x1A84), 0x16D);
        if (pasega->t < impact)
        {
            if (const SOST* psost = Psost(palo)) AccelAloPos(palo, -psost->dvGravity);
        }
        else wanted.z = v.z;
        AccelAloPos(palo, cmm::LimitVectorLength((wanted - v) * (1.0f / g_clock.dt), 4000.0f));
        break;
    }
    case 6: UpdateSlyDodgeMotion(p); break;  // 0x137248, after the base step motion
    case 24: UpdateSlySlideMotion(p); break;
    case 3: UpdateSlyHangMotion(p); break;  // 0x1903B8
    default: break;
    }
}

// orig: 0x0018DEE0 (vt slot 85)
uint32_t GrfSlyXp(ALO* palo)
{
    if (g_pfnGrfSlyXpImported) return g_pfnGrfSlyXpImported(palo);  // (the physics checks: from the reference's RAM)
    SLY* p = PslyFromLo(palo);
    if (!p) return GrfStepXp(palo);
    if (p->fNoContacts) return 0;
    switch (p->state)
    {
    case 3: case 8: case 21: case 22: return 0;
    case 13: case 14: return 1;
    case 24: return 0xF;
    default: return GrfActorStance(p);
    }
}


// orig: 0x00188CC8 (vt slot 55: a new contact: the step's 0x204250, then the state's adjustment)
void OnSlyXpNew(ALO* palo, XP* pxp, int i)
{
    OnStepXpNew(palo, pxp, i);
    SLY* p = PslyFromLo(palo);
    if (!p) return;
    switch (p->state)
    {
    case 5:
        if (static_cast<unsigned>(p->substate - 8) < 2 && g_clock.t - p->tSubstate < 0.25f) pxp->mu = 3.0f;  // +0x94
        break;
    case 4: AdjustDamageXp(p, pxp); break;  // 0x1A2100
    case 8: AdjustClimbXp(pxp); break;       // 0x19E9C8
    case 7:
        // a sliding rail's floor contact: no bounce, and no friction while sliding (state 0), else by the slope
        if (pxp->xpk == 0xE && p->n4C8 == 3 && p->inter.ploTarget && p->inter.ploTarget->U32Field(0, 0x1BC, 0))
        {
            pxp->elas = 0.0f;  // +0x98
            if (p->nSub4CC == 0) pxp->mu = 0.0f;
            else
            {
                const float g = std::sqrt(pxp->normal.x * pxp->normal.x + pxp->normal.y * pxp->normal.y) / pxp->normal.z + 0.5f;
                pxp->mu = std::min(std::max(g, 0.5f), 1.5f);
            }
        }
        break;
    case 6: pxp->mu = 0.0f; break;  // 0x1374E0 (+0x1FF0's: D_002BC9B4 = 0)
    case 24:
        pxp->mu = 0.0f;
        pxp->elas = 0.0f;
        break;
    default: break;
    }
}

// orig: 0x00140D70 (vt slot 58 of the actors: the step's floor 0x204768, then the jump's 0x199298)
void FindActorFloor(ALO* palo)
{
    FindStepFloor(palo);
    ACTOR* pa = PactorFromLo(palo);
    if (pa) PruneJumpLedgeXps(pa);  // 0x199298 (the ledge hang, the jump's state 17)
}

// orig: 0x00140D00 (vt slot 59 of the actors: the ALO's (nothing), the sub-objects' 0x1991A0 / 0x193DE8 / 0x19E950)
void RegisterActorContacts(ALO* palo)
{
    ACTOR* pa = PactorFromLo(palo);
    if (!pa) return;
    AddJumpLedgeContacts(pa);  // 0x1991A0 (the ledge hang)
    if (pa->nSub4CC != -1) AddInteractContacts(pa);  // 0x193DE8
    if (pa->nSubF54 != -1) AddClimbContacts(pa);  // 0x19E950
}

// orig: 0x00140DB0 (vt slot 68 of the actors: a contact with a player pushes back at 1/100 unless this actor is
// controlled and carries nothing)
void FillActorDzResponse(ALO* palo, DZ* pdz, DZENT* pent, int grf, int k)
{
    ACTOR* pa = PactorFromLo(palo);
    const glm::vec3 normalSaved = pent->normal;
    if (pa && pent->pxp)
    {
        ALO* paloOther = pent->pxp->axpd[1 - k].paloRoot;
        const bool fPlayer = paloOther && FIsLoDerivedFrom(paloOther, Icid(CID3::PLAYER));  // (0x2E1E40 / 0x2DD5BC / 0x2F7900)
        if (fPlayer && (NPoEnabled(palo) != 1 /* || 0x144068: TODO(actors) the carrier */))
            pent->normal = normalSaved * F32(0x3C23D70A);
    }
    using PFN68 = void (*)(ALO*, DZ*, DZENT*, int, int);
    if (auto pfn = reinterpret_cast<PFN68>(PvNativeFn(0x1FAD00))) pfn(palo, pdz, pent, grf, k);  // (solve.cpp)
    pent->normal = normalSaved;
}

// orig: 0x0018A2E0 (vt slot 49: the ALO's bounds 0x1F50E0, then each kept position grown in (0x1F5480, radius +0x19AC))
void UpdateSlyBounds(ALO* palo)
{
    UpdateAloBounds(palo);
    SLY* p = PslyFromLo(palo);
    OXA* poxa = Poxa(palo);
    if (!p || !poxa) return;
    for (int i = 0; i < p->cq; ++i)
    {
        const glm::vec3 q(p->aq[i]);
        const float r = p->g19AC;
        poxa->posMin = glm::min(poxa->posMin, q - glm::vec3(r));
        poxa->posMax = glm::max(poxa->posMax, q + glm::vec3(r));
        const glm::vec3 d = q - poxa->posCenter;
        const float l = std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
        if (poxa->sRadius < l) poxa->sRadius = l;
    }
}

// orig: 0x001406E8 (vt slot 79 of the actors: the control, 0x1F82B8, the +0x26C list's reset)
void SetActorControl(LO* plo, int n)
{
    EnablePo(s_psw, plo, n);  // (po.cpp runs 0x1406E8's 0x1F82B8 / 0x1EECD0 part for the objects whose slot 79 it is)
}

struct INITSLY
{
    INITSLY()
    {
        RegisterLoUpdate(0x18BCE0, UpdateSly);
        // the players' state code runs natively (Sly here, Bentley / Murray in gang.cpp): the physics' post-load keeps
        // their load's constraints (physworld.cpp: otherwise fixed)
        // vt slot 5 of the actors (0x140F90 ActorHandleMessage; Sly's 0x188F20 first): the messages from their
        // animations (3 done, 9 an ASEG event), the previous hook kept for the other objects
        RegisterLoMessageHandler([](LO* plo, int msg, void* pv) {
            ACTOR* pa = PactorFromLo(plo);
            if (pa) HandleActorMessage(pa, msg, pv);
            return pa != nullptr;
        });
        g_pfnFActorDrivesPhys = [](ALO* palo) {
            const CID cid = palo->Cid();
            return cid == 0x35 || cid == 0x36 || cid == 0x37;
        };
        RegisterSwPass(0x1C4468, [](SW*) { UpdateAllPadctls(); });  // (0x226550: each controller state)
    }
} s_initsly;
}  // namespace

NATIVEFN(0x18A278, UpdateSlyPhys);
NATIVEFN(0x188960, UpdateSlyMotion);
NATIVEFN(0x18DEE0, GrfSlyXp);
NATIVEFN(0x1406E8, SetActorControl);
NATIVEFN(0x188CC8, OnSlyXpNew);
NATIVEFN(0x140D70, FindActorFloor);
NATIVEFN(0x140D00, RegisterActorContacts);
NATIVEFN(0x140DB0, FillActorDzResponse);
NATIVEFN(0x18A2E0, UpdateSlyBounds);

// (debug: a route's "teleport" / "intovol" / "giver" step for Bentley or Murray as the current player (gang.cpp); the giver
// onto an object's place only, OID's position less D)
void ApplyRouteTeleportActor(ACTOR* p)
{
    if (!g_psw || static_cast<LO*>(p->palo) != PloPoCurrent(g_psw)) return;
    if (g_routetele.fGiver)
    {
        g_routetele.fGiver = false;
        if (LO* pg = PloFindSwObjectByOid(g_psw, g_routetele.oidGiver))
        {
            g_routetele = {true, PosLoWorld(pg).x, PosLoWorld(pg).y, PosLoWorld(pg).z - g_routetele.dGiver, 0.0f, false};
            std::printf("telegiver: player at %d (cid 0x%X)\n", pg->oid, pg->Cid());
        }
    }
    if (!g_routetele.fPending) return;
    g_routetele.fPending = false;
    SetStepPos(p, glm::vec3(g_routetele.x, g_routetele.y, g_routetele.z) - glm::vec3(p->vecBase));
    if (g_routetele.fH)
    {
        glm::mat3 r(1.0f);
        r[0] = glm::vec3(std::cos(g_routetele.h), std::sin(g_routetele.h), 0.0f);
        r[1] = glm::vec3(-std::sin(g_routetele.h), std::cos(g_routetele.h), 0.0f);
        SetStepRotation(p, r);
    }
    SetAloVelocityTo(p->palo, g_routetele.fVel ? g_routetele.vel : glm::vec3(0.0f));
    g_routetele.fVel = false;
    CutCm(&g_cm);
}
