// sly.cpp: Sly (sly.h). Ported from the reference's m_188488.cpp, m_189530_p3.cpp, m_18E540.cpp, m_13FF70.cpp and
// round4low_animation.cpp (0x18E540): oracle-checked or register-level ports of the original's 0x188488..0x191090
// and the actor's 0x13FF70..0x144E20.
#include "gauge3.h"
#include "sly.h"
#include "splitscreen3.h"
#include "jt.h"
#include "game/sly3.h"
#include "game/cond3.h"
#include "actioninput.h"
#include "routetele.h"
#include "climbsurf.h"
#include "slyfeet.h"
#include "gadget.h"
#include "game/treasuremap3.h"
#include "game/items3.h"
#include "outfit3.h"
#include <algorithm>
#include <unordered_map>
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
#include "binoc3.h"
#include "gang.h"
#include "aipassword3.h"
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
#include "pccharacter.h"   // (PC addition, debug menu only: Carmelita's moveset)
#include "pcguard.h"       // (PC addition, debug menu only: the playable guards)

#include <cstring>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "combo3.h"
#include "game/scptgame3.h"
#include "player3.h"
#include "game/guru3.h"
#include <string>
bool FIsRcCar3(const LO*);   // game/rccar3.cpp

using cmm::F32;

void BindLoPartsTable(ALO* palo, int c, uint32_t vaTable);  // loaders.cpp: orig 0x1A64E8
void SetBindPartsFromTemplate3(bool f);                      // loaders.cpp (a copy's parts: the template's removed children)
void StartPlayerCounters(ACTOR* pa, bool fReset);  // orig 0x1440D0
extern int g_iframeMain;  // (game.cpp: debug prints)

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

// orig: 0x0011D978 (g clamped to the range [lo, hi]) (orig3: 0x11EE98, the same C in the Sly 3 decomp)
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
            for (LO* ps : CIDLIST(g_psw->mpcidaplo, S3CID(0x15)))
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
    case -1: case 0: case 6: break;
    case 4: case 7: case 8: case 12: return 1;
    default: return 0;
    }
    // (orig3 0x156388 (the actors' slot 96): a combo in its state 0 / 2 whose definition has 0x20000 -> 1; Sly 3's
    // damage kind 7 -> 7 has no native kind; AUDIT4 PLAYER-10)
    if (sly3::FCombo3StanceAir(pa)) return 1;
    return 7;
}

// orig3: 0x156388 (vt slot 96, Sly 2's 85) of the Guru / Penelope / the Panda King (the others have their own, which end
// in it): not registered before (AUDIT4 PLAYER-10), so they had no contact kind
uint32_t GrfActorXp3(ALO* palo)
{
    ACTOR* pa = PactorFromLo(palo);
    return pa ? GrfActorStance(pa) : 0;
}
NATIVEFN3(0x156388, GrfActorXp3);

namespace
{
float GDuration(const ASEG* paseg) { return paseg ? PasegdOf(const_cast<ASEG*>(paseg))->tMax : 0.0f; }  // +0x74
}

// orig: 0x00140AF8 (the target +0x10E8 of kind 4 (+0x10EC) within reach: horizontally closer than 1000 (1700 for a
// cid 0x3A), height within +-750; fWide: 500 further, +-1250; never in the gait 3 (+0x10E4))
bool FActorTargetInReach(ACTOR* pa, bool fWide)
{
    if (pa->n10EC != 4 || pa->n10E4 == 3 || !pa->plo10E8) return false;
    float gDist = FIsLoDerivedFrom(pa->plo10E8, S3CID(0x3A)) ? 1700.0f : 1000.0f;
    float gMin = -750.0f, gMax = 750.0f;
    if (fWide)
    {
        gDist += 500.0f;
        gMin -= 500.0f;
        gMax += 500.0f;
    }
    if (!FIsLoDerivedFrom(pa->plo10E8, S3CID(0x30))) return false;
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
    BindLoPartsTable(pa->palo, 0x3B, 0x342DD0);  // (orig3 0x152488: Sly 3's actor table, 59 entries; Sly 2 0x2BC9F0, 53)
    pa->fPartTableBound = true;
}

// orig: 0x001400B8 (the actor's post-load: its parts table; TODO(actors): the sub-objects' post-loads 0x1959C0 /
// 0x1910F8 / 0x19F3F0 / 0x1A1098 / 0x19CC30 / 0x21F388 / 0x19DE58, the linked objects, 0x141EF8, 0x1F8278)
void PostLoadActorParts(ACTOR* pa)
{
    // EOPID 719 (0x13E818) overrides the constructor's outward interaction offset before post-load.
    // Native constructors run after BRX loading; Bentley/Murray must retain the authored value here.
    pa->g103C = GFieldLo(pa->palo, 0x103C, pa->g103C);
    BindActorPartTable(pa);
    pa->psm102C = PPart<SM>(pa->palo, 0x102C);
    // (Sly 3: Sly's script SM is the table 0x342DD0's entry 45 at +0x1138 (oid 452; gen_parts left it unpaired with
    // Sly 2's +0x102C): orig3 0x1ADDD0 state 14 applies it (Sly 2's state 18), e.g. the safe house's seat by option 850 (4))
    if (!pa->psm102C) pa->psm102C = PPart<SM>(pa->palo, FN3_TAG | 0x1138u);
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
    if (p->f1D30) { SetSlyState(p, jts::N2Hypno, -1); return; }
    // (Sly 3 Rocket Boots, s3-decomp: the power and the button are the gadget slot's (gadget.cpp EndAbility: 0x1AF990's
    // jump kind 11); Sly 3's own transitions 0x1AC098 have no decomp source, Sly 2's others kept)
    if (FBtnTake(p, BTN_CROSS, 0.1f)) { SetJumpKind(&p->jump, 1, 0, 0); return; }
    if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD))) { FInteractCircle(p); return; }   // (orig3 0x1AC098 case 19: 0x1B5170)
    if (p->jtbs == jts::SubBootsDash && glm::dot(VelAloPos(p->palo), glm::vec3(PxfObj(p->palo)->matWorld[0])) < 150.0f)
        SetSlyState(p, 0, -1);
}

// orig: 0x0018B7E8 (state 24's drain, alignment and three-way lean blend)
// (Sly 3 Rocket Boots, s3-decomp: orig3 0x1AD1D0 case 19: steered by the stick (else the heading kept), substate 12
// -> 13 (Sly 3's 6 -> 7) after 0.07 s once facing within 0.1, the lean smoothed (0x11E730, D_003225E0 {1.75, 0, 0.5, 35});
// no drain here (the gadget slot's))
void UpdateSlySlideInput(SLY* p)
{
    SetStepPhysHeading(p->palo, 0.0f < UJoy(p) ? RadJoy(p) : RadStepHeading(p));
    if (p->jtbs == jts::SubBootsCharge && 0.07f < g_clock.t - p->tSubstate &&
        std::fabs(cmm::RadNormalize(RadStepHeading(p) - p->radHeading)) < 0.1f)
        SetSlyState(p, jts::Boots, jts::SubBootsDash);
    if (p->jtbs != jts::SubBootsDash) return;
    static const cmm::SMPA s_smpa{1.75f, 0.0f, 0.5f, 35.0f};   // D_003225E0
    const float r = cmm::GLimitAbs(AngVelAlo(p->palo).z / (p->vBrake.x * 0.08f), 1.0f);
    const float lean = p->slide.uLean3 = cmm::GSmoothA(p->slide.uLean3, p->slide.duLean3, r * 0.8f, g_clock.dt, &s_smpa, &p->slide.duLean3);
    if (ASEGBL* blend = p->slide.pbl)
    {
        blend->abl[0].weight = std::max(0.0f, lean);
        blend->abl[1].weight = 1.0f - std::fabs(lean);
        blend->abl[2].weight = std::max(0.0f, -lean);
        RecalcBlend(blend);
    }
}

// orig: 0x00188960 (state 24's addition, following the base STEP motion)
// (Sly 3 Rocket Boots, s3-decomp: orig3 0x1AA630 case 19 substate 7 -> 0x1B0228(p, 0, 1, 6000): no drag, the
// sideways change limited to 6000, then the speed along the heading pushed to 1400 (at most 1500 / s^2 per frame's dt))
void UpdateSlySlideMotion(SLY* p)
{
    if (p->jtbs != 7) return;
    const glm::vec3 velocity = VelAloPos(p->palo);
    const glm::vec3 omega = AngVelAlo(p->palo);
    const glm::vec3 forward(PxfObj(p->palo)->matWorld[0]);
    const float along = glm::dot(velocity, forward);
    const glm::vec3 perpendicular = velocity - forward * along;
    const glm::vec3 next = MatRotateAxis(omega.z * g_clock.dt, glm::vec3(0.0f, 0.0f, 1.0f)) *
        (forward * along + perpendicular * (1.0f - 1.0f * g_clock.dt));
    glm::vec3 acceleration = (next - velocity) * (1.0f / g_clock.dt);
    acceleration.z = 0.0f;
    AccelAloPos(p->palo, cmm::LimitVectorLength(acceleration, 6000.0f));
    const float g = cmm::GLimitAbs((1400.0f - glm::dot(next, forward)) / g_clock.dt, 1500.0f);
    AccelAloPos(p->palo, forward * g);
    if (static const bool s_f = std::getenv("SLY3_ROCKETPRINT") != nullptr; s_f)
        std::printf("rocket: frame %d speed %.0f along %.0f lean %.2f pos %.0f %.0f %.0f\n", g_iframeMain, glm::length(velocity),
                    along, p->slide.uLean3, PosAloCenter(p->palo).x, PosAloCenter(p->palo).y, PosAloCenter(p->palo).z);
}

// orig: 0x00188488 (Sly's init)
void InitSlyFields(SLY* p)
{
    InitActorFields(p);
    p->jtbs = -1;
    p->g19AC = 15.0f;
    p->jts = -1;
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
    BindLoPartsTable(palo, 0xB, 0x345638);   // (orig3 0x1AA2B0's; Sly 2 0x188608's 0x2BF1B0, 18 entries)
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
    // (orig3 0x1AA2B0: Sly 3's record tags 0x159 / 0x25 / 0x24 / 0x2B / 0x2A / - / 0x24; Sly 2's 0x138 / 0xD / 0xC /
    // 0x13 / 0x12 / - / 0xC)
    const int handleTags[7] = {0x159, 0x25, 0x24, 0x2B, 0x2A, -1, 0x24};
    const int handleKinds[7] = {1, 1, 1, 1, 1, 1, 6};
    for (int i = 0; i < 7; ++i)
    {
        ALO* owner = palo;
        XF* record = nullptr;
        if (i == 5)
        {
            if (p->plo19A0 && FCidDerivedFrom(p->plo19A0->Cid(), S3CID(0x30)))
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
    // orig: 0x00188608: +0x19C0 / +0x19C4 / +0x19CC (0x1F4C58 by tag and kind), the hand's actuator +0x1A00
    // (0x12DC18(p, +0x19C0, 5, 5, 2, 0)) made and taken out again (0x230100): state 3 adds it (slyhang.cpp)
    // (orig3 0x1AA2B0: +0x1AC8 / +0x1ACC tag 0x24, +0x1AD4 tag 0x35; Sly 2's tags 0xC / 0x1D: natively none was
    // found, so the pickpocket's reach (LoadPickpocket) had no hand, arm or cane points)
    for (const auto& xf : palo->axf)
    {
        if (xf->tag == 0x24 && xf->kind == 6 && !p->pxfHandRot) p->pxfHandRot = xf.get();
        if (xf->tag == 0x24 && xf->kind == 1 && !p->pxfHandPos) p->pxfHandPos = xf.get();
        if (xf->tag == 0x35 && xf->kind == 6 && !p->pxfArm) p->pxfArm = xf.get();
    }
    if (p->pxfHandRot)
    {
        p->pctrlHand.reset(PctrlNewPos(palo, p->pxfHandRot, 5, 5, 2, glm::vec4(0.0f)));
        RemoveXfCtrl(palo, p->pxfHandRot, p->pctrlHand.get());
    }
    std::vector<BLENDBL> slideParts(3);
    for (int i = 0; i < 3; ++i)
    {
        // (Sly 3 Rocket Boots, s3-decomp: orig3 0x1AA2B0's 0x147E08(p, 0xC, 3, +0x1EC4, .., +0x1EE8): the clips +0x1EC8 /
        // +0x1ED4 / +0x1EE0 (oids 290..292, Sly 2's Stealth Slide 259..261 at +0x2018..))
        p->slide.apaseg[i] = PPart<ASEG>(palo, FN3_TAG | (0x1EC8u + i * 12));
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
    AttachActorLook(p, 0x2F);  // 0x1426E8 (Sly 3: the record of tag 0x2F, orig3 0x1AA2B0 -> 0x155190; Sly 2 0x17)
    LoadPickpocket(p, PPart<ASEG>(palo, 0x1BC8));  // 0x18B348 (0x188810)
    BindSlyHangBlend(p);      // 0x137A30(+0x1F60, 4, +0x1F90)
    BindSlySwingBlend(p);     // 0x18F908(+0x1970, +0x1994)
    PostLoadActorMass(p);     // 0x1400B8's end
    SetSlyState(p, 0, -1);
    SetLoSt(s_psw, palo, 1);  // 0x1A6CA8 (always active)
    // the paraglider's SM: 0x1F0380(+0x1D24, +0x1D20, -1, 1)
    if (SM* psm = PPart<SM>(palo, 0x1D24))
        if (LO* plo = PloPart(palo, 0x1D20); plo && FIsLoDerivedFrom(plo, S3CID(0x30)))
            p->psma1D28 = PsmaApplySm(psm, static_cast<ALO*>(plo), -1, 1);
    // (Sly 3, orig3 0x1AA2B0's end: the paraglider's SM is +0x1D44 on +0x1D40 (above), then +0x1F2C = 0x2399C8(+0x1F28,
    // Sly, -1, 1))
    if (SM* psm = PPart<SM>(palo, FN3_TAG | 0x1F28u))
    {
        if (std::getenv("SLY2N_SLYPRINT")) std::printf("sly: the SM of +0x1F28: %d\n", psm->oid);
        p->psma1F2C = PsmaApplySm(psm, palo, -1, 1);
    }
}

// (Sly 3) orig3 0x152488 (the actors' post-load, every actor class): an actor with mesh groups 0x306 (the red / blue 3-D
// glasses of the gang's models) shows them (0x23FE38) only in the anaglyph frame mode (GAME +0x14EC bit 1, the frame
// table's flag 2 of mode 1), else hides them (0x23FEC0). The PC never takes mode 1 (sw.cpp NScreenModeFromPrefix3), so
// they stay hidden, as after the original's "No" to the 3-D question. Natively at the world's start for every actor
// (also those with no native actor: the Panda King, Penelope, ...); proxies' later copies take their template's state.
void ShowActorGlasses3(SW* psw)
{
    const bool fAnaglyph = (g_grfGame3 & 2) != 0;
    for (const auto& up : psw->aploAll)
        if (LO* plo = up.get(); plo && FIsLoDerivedFrom(plo, S3CID(0x34)))
            ShowAloGlobs(static_cast<ALO*>(plo), 0x306, fAnaglyph);
}

// (PC, split screen's teams: a copy of Sly made while the world runs (pcloader.h PloPcCloneIntoWorld) gets the world
// start's actor part on its own: the init and post-load (InitWorldActors' body), PslyGlobal() stays the world's Sly)
void InitSlyCopy3(LO* plo)
{
    SLY* p = PslyFromLo(plo);
    if (!p) return;
    SLY* const pslyKeep = s_psly;
    ALO* const palo = static_cast<ALO*>(plo);
    const size_t cpartCopied = palo->apart.size();
    // (the load's parts, Sly 3's loader 0x1AA250 (loaders.cpp LoadJt3FromBrx: 0x152448's table, then Sly's), as the copy's
    // load would have bound them: the template's children that had left the world at its start (the ASEGs / SMs) are not
    // copied, those parts are shared with it)
    SetBindPartsFromTemplate3(true);
    BindLoPartsTable(palo, 1, 0x342DC0);
    BindLoPartsTable(palo, 0x6A, 0x345140);
    StepPhysOf(palo);
    InitSlyFields(p);
    s_psly = pslyKeep;
    PostLoadSly(p);
    SetBindPartsFromTemplate3(false);
    // (his own health: the world's Sly's is the save's (damage.cpp), a copy keeps its own, full, and the template's
    // capacity; the gadget power is the actor's in Sly 3 already)
    static std::unordered_map<const LO*, int> s_mpnHealth;   // (stable references: one per copy, never freed)
    SLY* const ptmpl = palo->ploCloneOf ? PslyFromLo(palo->ploCloneOf) : nullptr;
    if (ptmpl) SetDmgCapacity(p, DmgCapacityOf(ptmpl));
    int& nHealth = s_mpnHealth[plo];
    nHealth = DmgCapacityOf(p);
    SetDmgCounter(p, &nHealth);
    p->n1300 = ptmpl ? ptmpl->n1300 : p->n1300;
    if (std::getenv("SLY3_SPLITLOG"))
        std::printf("sly: copy %d: parts %zu copied, %zu bound; run gait %p / %p, idle SM %p\n", plo->oid, cpartCopied, palo->apart.size(),
                    static_cast<void*>(p->gaitRun.apaseg[0]), static_cast<void*>(p->gaitRun.pbl), static_cast<void*>(p->psmIdle));
    if (std::getenv("SLY3_SPLITLOG") && std::getenv("SLY2N_PARTPRINT"))
        for (const auto& [ib, plop] : palo->apart)
            std::printf("sly: copy part +0x%X -> %d (cid 0x%X) removed %d copy %d\n", ib, plop ? plop->oid : -1, plop ? plop->Cid() : 0,
                        plop ? int(plop->fRemoved) : -1, plop && plop->ploCloneOf ? 1 : 0);
}

int32_t* PnProgressSlot(const std::vector<std::string>& apath);  // savedata.h (0x1D8C10)

void InitWorldActors(SW* psw)
{
    s_psw = psw;
    s_psly = nullptr;
    for (CID cid = S3CID(0x35); cid >= 0 && cid < g_acls[S3CID(0x35)].cidLast; ++cid)
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
unsigned UOffsetInGameSave(const void* pv);   // (savedata.cpp: debug)
void StartPlayerCounters(ACTOR* pa, bool fReset)
{
    const char* psz = FIsLoDerivedFrom(pa->palo, S3CID(0x35)) ? "sly" : FIsLoDerivedFrom(pa->palo, S3CID(0x37)) ? "bentley"
                    : FIsLoDerivedFrom(pa->palo, S3CID(0x36)) ? "murray" : nullptr;
    if (!psz) return;
    if (int32_t* pn = PnProgressSlot({psz, "hpCur"})) SetDmgCounter(pa, pn);
    int32_t* pnMp = PnProgressSlot({psz, "mpCur"});
    int* pc = DmgCounterOf(pa);
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("players' start: %s health %d / %d power %d (save +0x%X)\n", psz, pc ? *pc : -1, DmgCapacityOf(pa), pnMp ? *pnMp : -1,
                    UOffsetInGameSave(pc));
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
        const char* psz = FIsLoDerivedFrom(pa->palo, S3CID(0x35)) ? "sly" : FIsLoDerivedFrom(pa->palo, S3CID(0x37)) ? "bentley"
                        : FIsLoDerivedFrom(pa->palo, S3CID(0x36)) ? "murray" : nullptr;
        if (!psz) continue;
        if (int* pc = DmgCounterOf(pa)) *pc = DmgCapacityOf(pa);
        if (int32_t* pnMp = PnProgressSlot({psz, "mpCur"})) *pnMp = 100;
    }
}

// orig: 0x0012F990 (the time of the segment's event 11 with id n; 0 when none) (orig3: 0x1368E0, the same code in Sly 3)
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
// +0x17D0 controller), 1 / 2 / 10 / 11 (the gadgets: the parts' swap 2 / 11), 3..7 (the +0x1A04 blends), 8 / 9. The
// sounds are ported: 0's 15, 4..7's 11, leaving 1 the hook's +0x130
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
    // (leaving 0: the reach controller off, 0x1CCAC0(+0x17D0, -1)); leaving 1 (the hook's hang): the hook's release
    // sound (+0x18C4's +0x130, its constructor's default 9: 0x184F50) at the actor (R31)
    auto emit = [&](int sfx) {  // 0x1FBB90 / 0x1FB2F8(0) / 0x1FB3F0(the actor's center 0x1A62E8); -1 none
        if (sfx == -1) return;
        const glm::vec3 pos = PosAloCenter(palo);
        PsndStartSound(sfx, 1.0f, 0, nullptr, &pos);
    };
    if (old == 1 && p->ploOther) emit(static_cast<int>(p->ploOther->U32Field(0, 0x130, 9)));
    int sfxEnter = -1;  // the new mode's sound: 0 (the cane's reach) 15, 4..7 (the hang blends) 11; none when compatible
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
        sfxEnter = 15;
        paseg = p->reach.pbl;          // (+0x1800)
        tStart = GDuration(paseg) * 0.5f;
        svt = 0.0f;
        p->t19B0 = g_clock.t + F32(0x3DCCCCCD);  // 0.1
        MarkAloDirty(palo);                       // 0x1F4BF8
        break;
    case 1: case 2: MarkAloDirty(palo); break;
    case 4: case 5: case 6: case 7:
        // TODO(actors): the +0x1A04 blends' clips (0x18F560) and stop times; their sound is ported
        sfxEnter = 11;
        WarnOnce("SetSlyMode: mode " + std::to_string(n));
        break;
    case 10: MarkAloDirty(palo); paseg = PPart<ASEG>(palo, 0x14D4); break;
    case 11: MarkAloDirty(palo); paseg = PPart<ASEG>(palo, 0x14D8); break;
    // (Sly 2's swing modes 12..23 (its clips +0x1CE0.., the idle SM state 0x17B) were here: Sly 3 sets only the modes
    // 0 / 1 / 2 / 10 / 11 and has no swing modes, so they went as dead code with Sly 2's numbers: AUDIT5 PLAYER5-13)
    default:
        WarnOnce("SetSlyMode: mode " + std::to_string(n));
        break;
    }
    if (fCompatible) sfxEnter = -1;
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
    emit(sfxEnter);
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
    switch (p->jts)
    {
    case jts::Stand: case jts::Walk: case jts::Target: case jts::Sneak: case jts::Run: ptarget->fActive = 1; break;
    default: break;
    }
}

// (Sly 3, s3-decomp) the run's gait: a worn outfit's (outfit3.h, orig3 0x173408), else Sly's own
static GAIT* PgaitRunSly(SLY* p)
{
    GAIT* pgait = PgaitOutfit3(p->palo, p->gaitRun.sfx);
    return pgait ? pgait : &p->gaitRun;
}

glm::mat4 MatConvertXfs4(XF* from, XF* to, const glm::mat4& in);   // gang.cpp (orig 0x22FF40 / orig3 0x28EC08)

// orig3 0x1ADDD0 case 0x14's entry: Sly on Murray (D_0038AE90): parented to Murray on the record of his toss controller's
// holding frame (+0x18E0's peer +0x18EC, its vt slot 37: the frame's matrix and record), the matrix in the frame of the
// position record's parent (0x28EC08), +0xA24 kept over the parent change (vt slot 10, grf 3), still (slots 40 / 41),
// held by two controllers (0x130078 priority 1, mode 8, state 2: the position, the Euler angles 0x11D3C0); his clip
// +0x1F3C (grfapl 9). As Bentley's (gang.cpp SetBentleyCarried 0). The target search offers Murray only to the player
// his controller selected (+0x18F0): always Bentley in the original's data (0x1E2358), so Sly never gets here there.
static ASEG* EnterSlyCarried(SLY* p)
{
    ALO* palo = p->palo;
    MURRAY* pm = PmurrayGlobal();
    LO* peer = pm ? pm->jtthrow1910.peer : nullptr;
    glm::mat4 m(1.0f);
    XF* pxfOn = nullptr;
    if (ALO* pa = dynamic_cast<ALO*>(peer)) pxfOn = PxfObj(pa);
    else if (LOX* px = dynamic_cast<LOX*>(peer))
    {
        m = px->mat;
        pxfOn = px->pxf;
    }
    XF* pxfPos = PxfPos(palo);
    XF* pxfRot = PxfRot(palo);
    XF* pxfRoot = palo->axf.empty() ? nullptr : palo->axf[0].get();
    m = MatConvertXfs4(pxfRoot ? pxfRoot->pxfRef : nullptr, pxfPos ? pxfPos->pxfRef : nullptr, m);   // 0x28EC08
    const int c = p->safe.c;   // (+0xA24)
    if (pm) SetAloParent(palo, pm->palo, pxfOn, 3);   // vt slot 10 (D_0038AE90, the frame's record, 3)
    p->safe.c = c;
    SetAloVelocityTo(palo, glm::vec3(0.0f));      // vt slot 40 (D_00317560)
    SetAloAngVelocityTo(palo, glm::vec3(0.0f));   // vt slot 41
    if (pxfPos) p->actrlCarried[0] = PctrlNewPos(palo, pxfPos, 1, 8, 2, glm::vec4(glm::vec3(m[3]), 1.0f));
    if (pxfRot) p->actrlCarried[1] = PctrlNewPos(palo, pxfRot, 1, 8, 2, VecXfValue(pxfRot, m));
    return PPart<ASEG>(palo, FN3_TAG | 0x1F3Cu);
}

// orig3 0x1ADDD0 case 0x14's exit: off Murray (0x1C7A00(p, 0, 0xF), +0xA24 kept), the holds deleted (0x1309D8), the
// records back in mode 2 (0x28F068 x 2)
static void LeaveSlyCarried(SLY* p)
{
    const int c = p->safe.c;
    SetAloParent(p->palo, nullptr, nullptr, 0xF);
    p->safe.c = c;
    DeleteCtrl(&p->actrlCarried[0]);
    DeleteCtrl(&p->actrlCarried[1]);
    SetSlyXfModes(p, 2);
}

// orig3: 0x001ADDD0 SetJtJts (Sly 2's 0x18C250): leave the old state, enter the new one, the substate (jtbs), the
// animation. Sly 3's numbers (sly.h jts::); the parts of Sly 2's states that have no Sly 3 state (jts::N2*) are kept
// below Sly 3's cases. Where a native caller orders its sub-object's animation before the state change (the interaction
// 8), the animation is kept as natively.
void SetSlyState(SLY* p, int n, int m)
{
    using namespace jts;
    ALO* palo = p->palo;
    ASEG* paseg = nullptr;  // s4
    SM* psm = nullptr;
    int oidState = -1;
    uint32_t grfMask = 0x7FFC;   // (grfRoute)
    int grfapl = 1;
    float tStart = 0.0f, svt = 1.0f;
    uint32_t grfHandles = 0;
    bool fKeepAnim = false;
    if (static const bool s_fPrint = std::getenv("SLY2N_SLYSTATE") != nullptr; s_fPrint && (n != p->jts || m != p->jtbs))  // (debug: Sly's state changes)
        std::printf("slystate: %d (%d) -> %d (%d) frame %d t %.3f\n", p->jts, p->jtbs, n, m, g_iframeMain, g_clock.t);
    if (std::getenv("SLY2N_XFDBG"))
        for (XF* x : {PxfPos(palo), PxfRot(palo)})
            if (x) std::printf("set state %d -> %d: xf kind %d mode %d local t %.1f %.1f %.1f world z %.1f\n", p->jts, n, x->kind, x->mode, x->mat[3].x, x->mat[3].y, x->mat[3].z, x->matWorld[3].z);
    if (p->jts == n && p->jtbs == m) return;
    if (p->jts != n)
    {
        // the object of the state left, for the jump (+0x388 ploAir: its target score is worse)
        if (n == Jump)
        {
            switch (p->jts)
            {
            case Hang: case HangFollow: p->jump.ploLeft = p->ploOther; break;
            case Climb: p->jump.ploLeft = p->ploF5C; break;
            case N2ClimbSurf: p->jump.ploLeft = p->ploSurface; break;   // (Sly 2's)
            default: p->jump.ploLeft = nullptr; break;
            }
        }
        // leave the old state
        switch (p->jts)
        {
        case -1: break;
        case Jump:
            // the landing sound 0x3D, unless into a hook, the pipe, a script's SM or a hiding spot (interaction 1)
            switch (n)
            {
            case Hang: case Climb: case ScriptSm: break;
            case Interact: if (p->n4C8 == 1) break; [[fallthrough]];
            default:
            {
                CSnd* psnd = PsndNewPlain(0x3D);
                psnd->SetSource(palo);
                psnd->SetVolume(F32(0x3F333333));
                psnd->SetFalloff(500.0f, 1000.0f);
                break;
            }
            }
            // 0x208B20(+0x18F0, -1): the reach off (natively the reach is Sly 2's attack mode 0)
            if (p->mode == 0) SetSlyMode(p, -1);
            // TODO(player): the landing effects (+0x1B10 / +0x1B14 by the new state, 0x1FE728) unless +0x1164
            SetJumpAnimState(&p->jump, -1, nullptr);  // 0x1B77F8
            break;
        case Hang: LeaveSlyHang(p); break;  // (the hook of kind 0 / 1 / 2: slyhang.cpp)
        case Climb:
            SetClimbState(p, -1);  // 0x1BFAD0
            // (Sly 2's: the climbing cane, mode 11, into the jump; Sly 3 has no modes)
            SetSlyMode(p, n == Jump && PPart<ASEG>(palo, 0x14D8) ? 0xB : -1);
            break;
        case Interact: SetInteractState(p, -1); break;  // 0x1B4430
        case Damage: SetDamageState(p, -1); break;      // 0x1C21F0
        case Throw: SetJtThrowState(p, -1); break;      // 0x2734E8
        case Dodge: SetSlyDodgeState(p, -1); break;     // 0x140CD0
        case Stand:
            SetSlyXfModes(p, 2);  // 0x28F068 x 2
            // (Sly 2's swing modes: none is set in Sly 3)
            switch (p->mode)
            {
            case 6: SetSlyMode(p, 4); break;
            case 7: SetSlyMode(p, 5); break;
            case 8: case 9: SetSlyMode(p, -1); break;
            default: break;
            }
            ActorNextLookTime(p);  // 0x155328
            break;
        case Pickpocket: p->grf10E0 &= ~2u; break;
        case Run: case Walk: case Sneak:
            // the gait's phase kept (+0x1B0)
            if (p->pasega && p->pasega->paseg && GDuration(p->pasega->paseg) != 0.0f)
                p->g1A0 = p->pasega->t / GDuration(p->pasega->paseg);
            break;
        case Takedown:
            if (p->pasega && !FAsegaRetracted(p->pasega)) ReleaseGuardHeld(p->plo10DC);  // 0x255960 (a guard 0x3A)
            p->f1D2C = 0;
            p->grf10E0 &= ~4u;
            break;
        case HangFollow: LeaveSlyHangFollow(p); break;  // (the hook of kind 3: climbsurf.cpp)
        case Map: PutAwayTreasureMap3(palo); break;      // 0x279140 (game/treasuremap3.cpp)
        case Boots:
            p->uBrake = 1.0f;
            if (PADCTL* pc = PpadctlOf(p)) pc->radJoy = p->radHeading;   // (+0x110C = +0x1BC)
            SetStepPhysSize(palo, 1.0f);   // 0x24F1E8
            break;
        case ScriptSm: case ScriptAnim: SetSlyXfModes(p, 2); break;
        case ScriptDrive: break;   // (its entry set the modes 2)
        case Target: break;   // (nothing to leave)
        case Ride: case RideOn:
            // the guard's check (Sly +0x1454's job 0xE7, aipassword3.cpp) broken off unless into 0x16 (its state 11);
            // leaving 0x16: +0x168's slot 56 (his gauge 7465, class 0xA3: shown again, 0x21E558), the riding camera
            // revoked (0x169C40)
            if (n != RideOn) BreakOffPassword3(palo);
            if (p->jts == RideOn)
            {
                ShowGaugeOf3(palo, true);
                SetRideCamera3(palo, false);
            }
            break;
        case Carried: LeaveSlyCarried(p); break;   // (0x1ADDD0 case 0x14)
        case Attack: break;
        // Sly 2's states (no Sly 3 state)
        case N2Rush:
            p->plo18C0 = nullptr;
            if (int i = IsoeActorPart(palo, 0x1A90); i >= 0) SetSoeOn(palo, &palo->asoe[i]);  // orig: 0x1F2A60
            break;
        case N2Hypno:
            p->f1D30 = 0;
            BroadcastTaskEvent(g_psw, 0x38, palo);  // 0x1619A0: the hypnosis over
            break;
        case N2ClimbSurf: LeaveSlyClimbSurf(p); break;
        case N2AirSwing: case N2AirSlam: break;
        default: WarnOnce("SetState: leaving state " + std::to_string(p->jts)); break;
        }
        // enter the new one
        switch (n)
        {
        case -1: break;
        case Stand:
        {
            // a worn outfit's idle SM (0x1733F8), else Sly's (+0x1134)
            psm = p->psmIdle;
            if (SM* psmOutfit = PsmOutfitIdle3(palo)) psm = psmOutfit;
            SetSlyXfModes(p, 1);
            p->f1514 = 0;   // (+0x164C)
            // the idle SM's state by the state left (Sly's own SM only): 0x193 from the run, 0x194 the sneak, 0x197
            // the attack, 0x195 / 0x196 a low / high landing (+0x19F0, else fallen over 400), else 0x192
            if (psm == p->psmIdle)
            {
                switch (p->jts)
                {
                case Run: oidState = 0x193; break;
                case Sneak: oidState = 0x194; break;
                case Attack: oidState = 0x197; break;
                case Jump:
                {
                    int fHigh = p->n18F0;
                    if (fHigh == -1) fHigh = F32(0x43C80000) < p->jump.z3C - PosLoWorld(palo).z ? 1 : 0;
                    oidState = fHigh == 1 ? 0x196 : 0x195;
                    break;
                }
                default: oidState = 0x192; break;
                }
                if (!psm || IsmsFindSm(psm, oidState) < 0) oidState = -1;  // 0x239A78
                grfMask = GrfActorLookMask(p);  // vt slot 111
            }
            p->vecVel = glm::vec3(0.0f);
            grfHandles = 0x1F;
            ActorSetLookFlag(p, 1);  // 0x155310
            break;
        }
        case Walk:
            paseg = p->gaitWalk.pbl;  // (+0x1470)
            grfapl = 9;
            tStart = p->g1A0 * GDuration(paseg);
            ResetGaitFeet(&p->gaitWalk);  // 0x258F28
            break;
        case Dodge:
            fKeepAnim = true;
            SetSlyDodgeState(p, SelectSlyDodgeSide(p));  // 0x140CD0 / 0x140BF0
            break;
        case Hang:
            // the swing on a hook of kind 0 / 1 / 2: its blend still at 0.5 (the substate below plays it from its middle)
            paseg = p->pblSwing;
            svt = 0.0f;
            tStart = 0.5f;
            EnterSlyHang(p);
            break;
        case Climb:
            SetClimbState(p, 0);  // 0x1BFAD0
            fKeepAnim = true;
            break;
        case Damage:
            if (p->nF04 != -1) SetBinocState(p, -1);  // 0x1BDBB0(+0xF60, -1): the binocucom off
            p->f1D30 = 0;   // (Sly 2's hypnosis)
            break;
        case Interact:
            if (p->n4C8 < 5) grfHandles = p->n4C8 < 3 ? 0 : 7;
            fKeepAnim = true;   // (natively the interaction's animation is set before this: kept)
            break;
        case Throw: fKeepAnim = true; break;
        case Sneak:
            paseg = p->rb1AC0.pbl;  // (+0x1C20)
            grfapl = 9;
            tStart = p->g1A0 * GDuration(paseg);
            p->n1038 = -1;  // (+0x1144)
            break;
        case Pickpocket:
            // the clip +0x1C28 (its end: state 0), +0x11B8 bit 2, no move, the hand pose not set yet
            sly3::ActorSlot102Sly3(p);   // vt slot 102 (0x1AF120)
            paseg = PPart<ASEG>(palo, 0x1BC8);
            p->vecVel = glm::vec3(0.0f);
            p->grf10E0 |= 2;
            grfHandles = 0x64;
            p->f1BD4 = 0;   // (+0x1C2C)
            break;
        case Run:
        {
            GAIT* pgait = PgaitRunSly(p);   // a worn outfit's gait (0x173408), else +0x1C90
            paseg = pgait->pbl;
            grfapl = 9;
            ResetGaitFeet(pgait);
            tStart = p->g1A0 * GDuration(paseg);
            break;
        }
        case Takedown:
        {
            sly3::ActorSlot102Sly3(p);   // vt slot 102
            p->grf10E0 |= 4;
            if (FPowerupOwned(0x1C))  // vt slot 113 (0x1C): the juggle
            {
                paseg = PPart<ASEG>(palo, 0x2050);
                UpdateSlyJuggleVelocity(p);  // 0x1AFF00
                SetAloVelocityTo(palo, p->matHeading * -p->vecVel);
                TurnActorNow(p);  // 0x24F0B0
            }
            else
            {
                paseg = PPart<ASEG>(palo, 0x1D1C);
                const glm::vec3 posTarget = PosAloCenter(static_cast<ALO*>(p->plo10DC));
                SetStepPhysHeadingToward(palo, -(posTarget - PosAloCenter(palo)));  // 0x24E8E8
                TurnActorNow(p);  // 0x24F0B0
                const float tHit = GEventAseg11(paseg, 0x2A6);  // 0x1368E0 (Sly 3's 0x2A6; Sly 2's 0x173)
                glm::mat4 mHit;
                CalculateAsegMatrices(paseg, palo, PxfObj(static_cast<ALO*>(p->plo19A4)), PxfObj(palo),
                                      tHit, 0.0f, &mHit, nullptr);  // 0x137070
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
        case ScriptSm:
            // a script's state machine (+0x1138), e.g. the safe house's seat by option 850 (4)
            if (!std::getenv("SLY2N_NO18SM")) psm = p->psm102C;
            [[fallthrough]];
        case ScriptAnim:
            if (!std::getenv("SLY2N_NO18XF")) SetSlyXfModes(p, 1);
            p->vecVel = glm::vec3(0.0f);
            sly3::ActorSlot102Sly3(p);   // vt slot 102
            SetSlyMode(p, -1);
            break;
        case ScriptDrive:
            if (!std::getenv("SLY2N_NO18XF")) SetSlyXfModes(p, 2);
            p->vecVel = glm::vec3(0.0f);
            sly3::ActorSlot102Sly3(p);
            SetSlyMode(p, -1);
            break;
        case HangFollow:
            // the hook of kind 3: its blend +0x1E50 (climbsurf.cpp)
            if (p->pblHang) { paseg = p->pblHang; grfapl = 9; }
            sly3::ActorSlot102Sly3(p);
            SetSlyMode(p, -1);
            EnterSlyHangFollow(p);
            break;
        case Map:
            // orig3 0x1ADDD0 case 0x18: the map held up by Sly (0x279120, game/treasuremap3.cpp), still
            FHoldTreasureMap3(palo);
            p->vecVel = glm::vec3(0.0f);
            SetAloVelocityTo(palo, glm::vec3(0.0f));
            SetAloAngVelocityTo(palo, glm::vec3(0.0f));
            break;
        case Boots:
            grfapl = 9;
            grfHandles = 0x18;
            // the blend +0x1EE8: the lean's sides off, the middle on (0x148938)
            if (ASEGBL* blend = p->slide.pbl)
            {
                blend->abl[2].weight = 0.0f;
                blend->abl[0].weight = 0.0f;
                blend->abl[1].weight = 1.0f;
                RecalcBlend(blend);
                paseg = blend;
            }
            p->slide.uLean3 = p->slide.duLean3 = 0.0f;   // +0x1EEC / +0x1EF0
            p->vecVel = glm::vec3(0.0f);
            SetStepPhysSize(palo, 2.0f);   // 0x24F1E8
            break;
        case Attack:
            // a combo's attack (combo3.cpp): its runner plays the clips
            p->vecVel = glm::vec3(0.0f);
            break;
        case Jump: break;   // (the jump's animation: 0x1B77F8)
        case Target: break;  // (its substates' animations below)
        case Carried: paseg = EnterSlyCarried(p); grfapl = 9; break;   // (0x1ADDD0 case 0x14: on Murray)
        case Ride: fKeepAnim = true; break;   // (0x1ADDD0 case 0x15: the animation kept)
        case RideOn:
        {
            // 0x1ADDD0 case 0x16: still, facing the guard (0x24E920 -> 0x24E8E8, then vt slot 35 with +0x1C0 the target
            // matrix: TurnActorNow), the SM +0x1F38 from no state, +0x168's slot 54 (his gauge 7465, class 0xA3: hidden,
            // 0x21E4F0), the riding camera 0x16C608 (aipassword3.cpp CPRIDE3, pushed by 0x169CE0)
            p->vecVel = glm::vec3(0.0f);
            ShowGaugeOf3(palo, false);
            grfHandles = 0x18;
            if (LO* ploGuard = PloPasswordGuard3(palo); ploGuard && FIsLoDerivedFrom(ploGuard, S3CID(0x30)))
            {
                SetStepPhysHeadingToward(palo, PosLoWorld(ploGuard) - PosLoWorld(palo));
                TurnActorNow(p);
            }
            psm = PPart<SM>(palo, FN3_TAG | 0x1F38u);
            oidState = -1;
            SetRideCamera3(palo, true);
            break;
        }
        // Sly 2's states (no Sly 3 state)
        case N2Rush:
            SetSlyMode(p, -1);
            ChooseSlyRushTarget(p);  // orig: 0x18F688
            paseg = PPart<ASEG>(palo, 0x1A7C);
            break;
        case N2AirSwing:
            SetSlyMode(p, -1);
            p->vecVel = glm::vec3(0.0f);
            ChooseSlyAirSwingTarget(p);  // orig: 0x18F740
            paseg = PPart<ASEG>(palo, 0x1A80);
            p->t19B0 = g_clock.t + 0.25f;  // 0x2BF3E4
            break;
        case N2AirSlam:
            SetSlyMode(p, -1);
            p->vecVel = glm::vec3(0.0f);
            paseg = PPart<ASEG>(palo, 0x1A84);
            break;
        case N2Hypno:
            paseg = PPart<ASEG>(palo, 0x1D34);
            grfapl = 9;
            BroadcastTaskEvent(g_psw, 0x37, palo);  // 0x1619A0
            break;
        case N2ClimbSurf: EnterSlyClimbSurf(p); break;
        default:
            WarnOnce("SetState: entering state " + std::to_string(n));
            break;
        }
    }
    // the substate
    if (p->jtbs != m)
    {
        switch (m)
        {
        case -1: break;
        case SubSwingBack: case SubSwingStill: case SubSwingFore:
            // the swing's blend (state 4), kept when it plays
            if (p->pasega && p->pblSwing && p->pasega->paseg == p->pblSwing) fKeepAnim = true;
            else if (p->pblSwing)
            {
                paseg = p->pblSwing;
                svt = 0.0f;
                tStart = GDuration(paseg) * 0.5f;
            }
            break;
        case SubHangSm: psm = p->psmHang; break;  // (+0x1A50: state 4 hanging still)
        case SubTargetStance:
            psm = PPart<SM>(palo, 0x1BC4);  // (+0x1C24)
            p->vecVel = glm::vec3(0.0f);
            break;
        case SubTargetWalk:
            p->n1038 = -1;
            paseg = p->rb13D0.pbl;  // (+0x1620)
            p->vecVel = glm::vec3(0.0f);
            grfapl = 9;
            break;
        case SubBootsCharge:
            p->uBrake = 3.0f;   // (+0x210)
            grfHandles = 0x18;
            break;
        case SubBootsDash:
            p->uBrake = 0.08f;
            grfHandles = 0x18;
            SetAloVelocityTo(palo, cmm::SetVectorCylind(p->radHeading, 1000.0f, 0.0f));   // 0x1B0450
            fKeepAnim = true;
            break;
        case SubMapUp: case SubMapClip:
        {
            // orig3 0x1ADDD0 substates 8 / 9: the map held up (+0x1E70, grfapl 9), the step (+0x1E74 after an odd
            // number of steps, else +0x1E78; the map's +0x27C before this step is counted) (AUDIT5 PLAYER5-08)
            const uint32_t ib = m == SubMapUp ? 0x1E70u : (NMapSteps3() & 1) ? 0x1E74u : 0x1E78u;
            ASEG* pa3 = PPart<ASEG>(palo, FN3_TAG | ib);
            if (!pa3) pa3 = PPart<ASEG>(palo, ib);
            if (pa3) { paseg = pa3; if (m == SubMapUp) grfapl = 9; }
            else fKeepAnim = true;
            grfHandles = 0x18;
            break;
        }
        // Sly 2's substates (no Sly 3 substate)
        case SubN2WindUpL: case SubN2WindUpR:
            SetSlyMode(p, -1);  // 0x18E540
            paseg = PPart<ASEG>(palo, m == SubN2WindUpL ? 0x1D08 : 0x1D0C);
            p->vecVel = glm::vec3(0.0f);
            p->f18E8 = 0;
            p->f18E4 = 0;
            break;
        case SubN2Strike: case SubN2StrikeUp:
            paseg = PPart<ASEG>(palo, m == SubN2Strike ? 0x1D10 : 0x1FE8);
            p->f18E4 = 0;
            p->vecVel = glm::vec3(0.0f);
            break;
        case SubN2SurfHeld: psm = PPart<SM>(palo, 0x1F14); break;
        case SubN2SurfMove:
            paseg = p->rb1E10.pbl;
            grfapl = 9;
            break;
        default: WarnOnce("SetState: substate " + std::to_string(m)); break;
        }
    }
    if (p->jts != n)
    {
        p->jts = n;
        p->tState = g_clock.t;
    }
    if (p->jtbs != m)
    {
        p->jtbs = m;
        p->tSubstate = g_clock.t;
    }
    UpdateActorStepMesh(p);  // vt slot 101 (0x1ADBE0: the custom entry's shape by the state, actmesh.cpp)
    SetSlyHandles(p, grfHandles);  // 0x1ADCA0
    if (fKeepAnim) return;
    if (p->pasega) RetractAsegaPp(&p->pasega);  // 0x137628
    else if (p->psma)
    {
        RetractSma(p->psma);  // 0x23AC58
        p->psma = nullptr;
    }
    if (paseg)
    {
        ApplyAseg(paseg, palo, tStart, svt, grfapl, &p->pasega);  // 0x136908
        return;
    }
    if (psm) p->psma = PsmaApplySm(psm, palo, oidState, grfapl, grfMask);  // 0x2399F0
}

// orig3 0x156998: busy (the binocucom +0xF60's state +0xF64 below 3; a script's binocucom panel open counts too, as
// game/guru3.cpp FGuruBusy)
static bool FSlyBusy3(SLY* p) { return static_cast<unsigned>(p->nF04) < 3 || NBinocPanelState3(p->palo) != -1; }
// orig3 +0x1108 / 0x173428: an outfit worn (the run only, no sneak / walk)
static bool FSlyOutfitWorn3(SLY* p) { return FOutfitWorn3(p->palo); }
// orig3 +0x13E8 (EOPID 775, PLAYER: a field): the walk forced (no run, no guard's stance)
static bool FSlyWalkForced3(SLY* p) { return p->palo->U32Field(0, 0x13E8, 0) != 0; }

// orig3: 0x001AC098 (Sly 2's 0x18A8C8): the per-frame transitions, repeated while the state or substate changes. Sly 3's
// numbers (sly.h jts::); Sly 2's states with no Sly 3 state (jts::N2*) keep their parts after Sly 3's.
void UpdateSlyTransitions(SLY* p)
{
    using namespace jts;
    if (std::getenv("SLY2N_PPDEBUG") && (GrfbtnHeld(p) & BTN_CIRCLE))
        std::printf("ppdebug0: t %.3f state %d sub %d\n", g_clock.t, p->jts, p->jtbs);
    for (int iter = 0; iter < 32; ++iter)
    {
        const int jtsOld = p->jts, jtbsOld = p->jtbs;
        // off the floor for over 0.1 s (0x2503C8): the fall (the jump's kind 0, 0x1B7268)
        bool fFall = false;
        switch (jtsOld)
        {
        case Interact: fFall = !(p->n4C8 == 12 || (p->n4C8 < 5 && (p->n4C8 >= 3 || p->n4C8 == 1))); break;
        case Attack: fFall = sly3::FCombo3DefMoves(p); break;   // (TODO(player): or Sly 3's JT +0x10F8, whose writer is not found)
        case Stand: case Run: case Walk: case Target: case Sneak: case Pickpocket: case Throw: fFall = true; break;
        default: break;
        }
        if (fFall && 0.1f < g_clock.t - p->tFloor) SetJumpKind(&p->jump, 0, 0, 0);
        switch (p->jts)
        {
        case Jump: UpdateJumpTransitions(&p->jump); break;     // 0x1B9020
        case Interact: UpdateInteractTransitions(p); break;     // 0x1B27D8
        case Damage: UpdateDamageTransitions(p); break;         // 0x1C2A18
        case Throw: UpdateJtThrow(p); break;                    // 0x271F78
        case Dodge: UpdateSlyDodgeTransitions(p); break;        // 0x1408B0
        case Climb: UpdateClimbTransitions(p); break;           // 0x1BF1F8
        case Attack:
            // the combo's definition moves (0x1398F0) and the stick is out or cross was pressed within 0.1 s: state 0
            if (sly3::FLeaveCombo3Attack(p)) SetSlyState(p, Stand, -1);
            break;
        case Stand: case Run: case Walk: case Sneak:
        {
            if (std::getenv("SLY2N_PPDEBUG") && (GrfbtnHeld(p) & BTN_CIRCLE))
                std::printf("ppdebug: t %.3f state %d 10E4 %d 10DC %d reach %d\n", g_clock.t, p->jts, p->n10E4,
                            p->plo10DC ? int(p->plo10DC->oid) : -1, int(FActorTargetInReach(p, false)));
            if (p->f1D30) { SetSlyState(p, N2Hypno, -1); break; }   // (Sly 2's hypnosis: never set by Sly 3's data)
            // (PC addition, debug menu only: Carmelita's moveset, pccharacter.h: her shot stands still; Cross her
            // jump; no fighting stance, no pickpocketing; Circle the interaction in reach, else her high jump; she walks
            // by default and runs with R1 held, both in the run state 1: no walk state, nor the sneak near a guard)
            const bool fCarm = FPcCarm(p->palo);
            if (fCarm && FPcCarmShooting() && !FPcCarmAiming())   // (in her aim mode she strafes on while shooting)
            {
                if (p->jts != Stand) SetSlyState(p, Stand, -1);
                break;
            }
            const bool fOutfit = FSlyOutfitWorn3(p);
            const bool fWalk = (GrfbtnHeld(p) & BTN_R1) || FSlyWalkForced3(p);
            if (FSlyBusy3(p)) { SetSlyState(p, Stand, -1); break; }
            if (FBtnTake(p, BTN_CROSS, 0.1f))
            {
                if (fCarm) { if (FPcCarmCanJump()) { SetJumpKind(&p->jump, 1, 0, 0); PcCarmJumped(p->palo, false); } break; }
                // (orig3 0x1AC098: an outfit worn: Sly 3's jump kind 0x11, natively c_jkOutfit3 (jump.h); else 1)
                SetJumpKind(&p->jump, fOutfit ? c_jkOutfit3 : 1, 0, 0);
                break;
            }
            if (fCarm && FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD)))
            {
                // (a guard's Circle: its special, pcguard.h)
                if (!FInteractCircle(p) && FPcCarmCircle(p->palo)) { SetJumpKind(&p->jump, 1, 0, 0); PcCarmJumped(p->palo, true); }
                break;
            }
            // a guard in reach (0x153290): state 6, its stance
            if (!fCarm && !fWalk && FActorTargetInReach(p, false)) { SetSlyState(p, Target, SubTargetStance); break; }
            // R3 within 0.15 s, 0.1 s into the state and the binocucom allowed (vt slot 113 (3): 0x154518's busy test in these
            // states): state 0, the binocucom
            if (FBtnWithin(p, BTN_R3, 0.15f) && 0.1f < g_clock.t - p->tState && !FSlyBusy3(p))
            {
                SetSlyState(p, Stand, -1);
                ForgetPadctlPresses(PpadctlOf(p), BTN_R3);  // 0x1C4738
                OpenBinoc(p, 0, nullptr, nullptr);          // 0x1BDA88(+0xF60, 0, 0)
                break;
            }
            // circle within 0.15 s near a guard of rank 3 whose pocket glows (+0x40C 2): the pickpocket (state 11)
            if (!fCarm && !fOutfit && p->n10E4 == 3 && p->plo10DC && PocketOf(p->plo10DC).nMode == 2 &&
                sly2actions::Take(PpadctlOf(p), sly2actions::Action::SlyPickpocket, BTN_CIRCLE, F32(0x3E19999A)))
            {
                SetSlyState(p, Pickpocket, -1);
                break;
            }
            // circle within 0.2 s: the interaction (0x1B5170)
            if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD))) { FInteractCircle(p); break; }
            const float u = UJoy(p);
            if (std::getenv("SLY2N_TRANS")) std::printf("  transitions: state %d u %.3f t %.3f\n", p->jts, u, g_clock.t);
            const bool fSneak = p->n10E4 == 3 && !fCarm;
            if (p->jts == Stand ? 0.1f < u : 0.0f < u)
            {
                // (an outfit worn: Sly 3's run only (+0x1108, 0x173428: no walk by R1, no sneak). s3fix-ai2: the R1 walk in a
                // disguise (a deviation of s3fix-player2) dropped; the guards' sight of a disguised player is the detector's
                // 0x170580 with 0x2544E0's kind 1 (detect.cpp))
                if (fCarm) SetSlyState(p, Run, -1);
                else if (fWalk && !fOutfit) SetSlyState(p, Walk, -1);   // (orig3 0x1AC098: an outfit runs, +0x13E8 too: AUDIT5 PLAYER5-09)
                else SetSlyState(p, !fOutfit && fSneak ? Sneak : Run, -1);
            }
            else SetSlyState(p, !fOutfit && fSneak ? Sneak : Stand, -1);
            break;
        }
        case Map:
            // orig3 0x1AC098 case 24: the gadget's button (0x154798(p, 0x24)) up after 0.5 s: state 0 (natively also when the
            // map mode let go of the map: a wrong check, the last record)
            if (PloMapHolder3() != p->palo) { SetSlyState(p, Stand, -1); break; }
            if (!(GrfMapButtonsHeld3() & GrfAbilityButton(p, sly3::ITEM2_SLY3 + 36)) && 0.5f < g_clock.t - p->tState)
                SetSlyState(p, Stand, -1);
            break;
        case Boots:
            // the power out (0x1F4430): state 0; cross on the floor (+0x218): the jump; circle: the interaction; the
            // dash stopped by a wall (0x1B04A0: natively its speed along the heading under 150 in the dash)
            if (NActorPower(p) <= 0) { SetSlyState(p, Stand, -1); break; }
            UpdateSlySlideTransitions(p);
            break;
        case Target:
            // a guard in reach: the stance (jtbs 4) or the walk around it (5)
            if (p->f1D30) { SetSlyState(p, N2Hypno, -1); break; }
            if (FSlyBusy3(p)) { SetSlyState(p, Target, SubTargetStance); break; }
            if ((GrfbtnHeld(p) & BTN_R1) || FSlyWalkForced3(p) || !FActorTargetInReach(p, true)) { SetSlyState(p, Stand, -1); break; }
            if (FBtnTake(p, BTN_CROSS, 0.1f)) { SetJumpKind(&p->jump, 1, 0, 0); break; }
            if (FBtnWithin(p, BTN_R3, 0.15f) && 0.1f < g_clock.t - p->tState && !FSlyBusy3(p))
            {
                SetSlyState(p, Target, SubTargetStance);
                ForgetPadctlPresses(PpadctlOf(p), BTN_R3);  // 0x1C4738
                OpenBinoc(p, 0, nullptr, nullptr);          // 0x1BDA88
                break;
            }
            if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD))) { FInteractCircle(p); break; }  // 0x1B5170
            // (Sly 2's charged swing's substates keep the state)
            if (static_cast<unsigned>(p->jtbs - SubN2WindUpL) < 4) break;
            SetSlyState(p, Target, 0.0f < UJoy(p) ? SubTargetWalk : SubTargetStance);
            break;
        case Hang: FSlyHangRelease(p); break;  // after 0.25 s, cross within 0.25 s: the jump's kind 2 (slyhang.cpp)
        case Takedown:
            if (!p->pasega || FAsegaRetracted(p->pasega)) SetSlyState(p, Stand, -1);
            break;
        case ScriptAnim:
            // a script's animation (+0x113C) done, or its state machine (+0x1140) done (orig3 0x1AC098 case 15: also while
            // a clip runs, AUDIT5 PLAYER5-09): stand
            if ((p->pasega && FAsegaRetracted(p->pasega)) || (p->psma && FSmaRetracted(p->psma))) SetSlyState(p, Stand, -1);
            break;
        case HangFollow:
            // on a kind-3 hook: cross jumps toward its target (kind 0xC, climbsurf.cpp)
            if (FBtnTake(p, BTN_CROSS, 0.25f)) FSlyHangFollowJump(p);
            break;
        case Pickpocket: case ScriptSm: case ScriptDrive: case Carried: case Ride: case RideOn: break;
        // Sly 2's states (no Sly 3 state)
        case N2ClimbSurf:
            // on a climbing surface: cross jumps off it (kind 0x12)
            if (FBtnTake(p, BTN_CROSS, 0.25f)) SetJumpKind(&p->jump, 0x12, 0, 0);
            break;
        case N2AirSwing:
            // the air swing: its end goes on falling (the jump's kind 0xB), a floor after 0.4 s stands
            if (p->pasega && FAsegaRetracted(p->pasega)) { SetJumpKind(&p->jump, 0xB, 0, 0); break; }
            if (p->pxpFloor && F32(0x3F333333) < p->pxpFloor->normal.z && F32(0x3ECCCCCD) < g_clock.t - p->tState)
                SetSlyState(p, Stand, -1);
            break;
        case N2AirSlam:
            // the air slam: its end stands; a floor after 0.4 s seeks it to its landing event 0x16D
            if (p->pasega && FAsegaRetracted(p->pasega)) { SetSlyState(p, Stand, -1); break; }
            if (p->pxpFloor && F32(0x3F333333) < p->pxpFloor->normal.z && F32(0x3ECCCCCD) < g_clock.t - p->tState)
                if (ASEG* paseg = PPart<ASEG>(p->palo, 0x1A84); paseg && p->pasega)
                {
                    const float g = GEventAseg11(paseg, 0x16D);  // 0x12F990
                    if (p->pasega->t < g) SetAsegaTime(p->pasega, 0, g, p->pasega->svt);  // 0x130D30
                }
            break;
        case N2Hypno:
            // the hypnosis: over when +0x1D30 is cleared or after 5 s (0x2BF360)
            if (!p->f1D30 || F32(0x40A00000) < g_clock.t - p->tState) SetSlyState(p, Stand, -1);
            break;
        case N2Rush: break;  // (ends through animation message 3)
        default:
            WarnOnce("the transitions of state " + std::to_string(p->jts));
            break;
        }
        if (p->jts == jtsOld && p->jtbs == jtbsOld) break;
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
    if (!p->plo19A4 || !FCidDerivedFrom(p->plo19A4->Cid(), S3CID(0x30)) || !pxfC0)
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
    if (p->jtbs == 5)
    {
        const glm::mat4 mat = MatJtLo(p->palo);  // vt slot 31
        const float d = cmm::RadNormalize(RadJoy(p) - std::atan2(mat[0].y, mat[0].x));
        const float ad = std::fabs(d);
        if (ad < F32(0x3F490FDB)) n = 0xE;
        else if (F32(0x4016CBE4) < ad) n = 0x10;
        else if (0.0f < d) n = 0x12;
        else n = 0x14;
    }
    else n = p->jts == 0 ? 0xC : 0xE;
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

// (Sly 2's 0x18F0A8, the buttons' effects by state (the cane's swing modes, square letting go of a hook): Sly 3's input
// 0x1AD1D0 has no such step (the combos take square / triangle: combo3.cpp); only the PC addition below is kept)
void UpdateSlyButtons(SLY* p)
{
    // (PC addition, debug menu only: Carmelita's moveset: no cane moves; Square on the ground or in the air her shot)
    if (FPcCarm(p->palo))
    {
        using namespace jts;
        const bool fFree = p->jts == Stand || p->jts == Walk || p->jts == Sneak || p->jts == Run || p->jts == Jump;
        if (fFree && FBtnTake(p, BTN_SQUARE, 0.2f)) PcCarmFire(p->palo);
        // (a guard's Circle in the air: its special too; on the ground UpdateSlyTransitions takes it)
        if (p->jts == Jump && FPcGuardMoves() && FBtnTake(p, BTN_CIRCLE, 0.2f)) FPcCarmCircle(p->palo);
        if (p->mode != -1) SetSlyMode(p, -1);
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
    if (plo && FIsLoDerivedFrom(plo, S3CID(0x30)))
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

// the root velocity of the clip playing, reversed (0x136EE0's channel of kind 1 at its time; Sly 3's 0x24F278)
static glm::vec3 VecSlyRootReversed(SLY* p, ASEGA* pasega, ASEG* paseg = nullptr)
{
    if (!pasega || FAsegaRetracted(pasega)) return glm::vec3(0.0f);
    if (const ASEGCHN* pchn = PchnFindAseg(paseg ? paseg : pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
    {
        glm::vec4 v(0.0f);
        pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
        return -glm::vec3(v);
    }
    return glm::vec3(0.0f);
}

// orig3: 0x001AD1D0 (Sly 2's 0x18B7E8): the controller's input: the binocucom (0x1ACCA8), the transitions (0x1AC098),
// the state's own input; Sly 3's numbers (sly.h jts::)
void UpdateSlyInput(SLY* p)
{
    using namespace jts;
    // (PC addition: Carmelita's moveset rates only the objects to use (0x80): no ledges, hooks, spires, ropes, poles;
    // Triangle held: her aim mode, pccharacter.h)
    const bool fCarmAim = FPcCarm(p->palo) && PcCarmAimInput(p->palo, (GrfbtnHeld(p) & BTN_TRIANGLE) != 0, p->jts);
    UpdateJtCandidates(p, FPcCarm(p->palo) ? 0x80 : 0xFFFF);  // (Sly 2's 0x194A40 here)
    UpdateActorBinoc(p, 0x4F0);  // 0x1ACCA8 (+0xF60; 0x410 with the talk prompt's state 7 (+0x1D40): TODO)
    UpdateSlyTransitions(p);     // 0x1AC098
    UpdateSlyButtons(p);         // (the PC addition only)
    switch (p->jts)
    {
    case Stand:
        if (FPcCarm(p->palo) && FPcCarmShooting()) p->vecVel = PcGuardAttackVel();  // (PC addition: a guard's attack's root motion)
        if (fCarmAim) PcCarmFaceAim(p->palo);   // (PC addition: her aim mode)
        break;
    case Damage:
        p->vecVel = VecSlyRootReversed(p, p->pasega);   // (+0x113C's root velocity, reversed)
        break;   // (0x1C2B48(+0xE40): UpdateDamage below, every state as natively)
    case Hang: UpdateSlyHangInput(p); break;      // 0x1B1920
    case Jump: UpdateJumpInput(&p->jump); break;  // 0x1B9C48
    case Interact: UpdateInteract(p); break;      // 0x1B30E8 (+0x500)
    case Dodge: UpdateSlyDodgeInput(p); break;    // 0x140900
    case Sneak: MoveAroundTarget(p, &p->rb1AC0, true, RadJoy(p), UJoy(p)); break;  // 0x1C1888(+0x1B20)
    case Pickpocket: UpdatePickpocketPose(p); break;  // 0x1ACE40
    case Run:
        p->vecVel = glm::vec3(SetGaitMix(PgaitRunSly(p), UJoy(p)));  // 0x258CF0 (a worn outfit's gait, else +0x1C90)
        if (FPcCarm(p->palo)) p->vecVel *= PcCarmMoveScale(glm::length(p->vecVel), UJoy(p), (GrfbtnHeld(p) & BTN_R1) != 0);  // (PC addition: Carmelita's walk / run (R1))
        if (fCarmAim)   // (PC addition: her aim mode, facing the camera's way, strafing by the stick)
        {
            const float radJoyDbg = RadJoy(p);
            p->vecVel = PcCarmStrafeVel(UJoy(p), RadJoy(p), (GrfbtnHeld(p) & BTN_R1) != 0);
            PcCarmFaceAim(p->palo);
            if (std::getenv("SLY2N_STRAFEDBG"))
                std::printf("strafe: rad joy %.1f (before %.1f) aim %.1f vel %.0f %.0f actual %.0f %.0f heading %.1f\n", glm::degrees(RadJoy(p)), glm::degrees(radJoyDbg), glm::degrees(PcCarmAimHeading()),
                            p->vecVel.x, p->vecVel.y, VelAloPos(p->palo).x, VelAloPos(p->palo).y, glm::degrees(p->radHeading));
            break;
        }
        SetStepPhysHeading(p->palo, RadJoy(p));  // 0x24E878(+0x110C)
        break;
    case Target:
        if (p->jtbs == SubTargetStance)
        {
            // facing the object +0x11B4 (0x24E920)
            if (p->plo10DC && FIsLoDerivedFrom(p->plo10DC, S3CID(0x30)))
                SetStepPhysHeadingToward(p->palo, PosAloCenter(static_cast<ALO*>(p->plo10DC)) - PosAloCenter(p->palo));
        }
        else if (p->jtbs == SubTargetWalk) MoveAroundTarget(p, &p->rb13D0, true, RadJoy(p), UJoy(p));  // 0x1C1888(+0x1520)
        else if (p->jtbs == SubN2Strike || p->jtbs == SubN2StrikeUp)
        {
            // (Sly 2's charged strike steered by the stick: its direction (behind the heading, x 1000) in Sly's frame)
            const float rad = RadJoy(p) + 3.14159265f;
            const glm::vec3 v(std::cos(rad) * UJoy(p) * 1000.0f, std::sin(rad) * UJoy(p) * 1000.0f, 0.0f);  // 0x11E1A0
            p->vecVel = VecJtConvert(nullptr, p->palo, v);  // 0x1A6FC0
        }
        break;
    case Takedown:
        if (FPowerupOwned(0x1C) && p->pasega && p->pasega->paseg == PPart<ASEG>(p->palo, 0x2050))
            UpdateSlyJuggleVelocity(p);  // 0x1AFF00 (the juggle's clip +0x1ABC playing)
        else if (p->f1D2C) p->vecVel = VecSlyRootReversed(p, p->pasega, PPart<ASEG>(p->palo, 0x1D1C));   // (+0x1AB8)
        break;
    case ScriptAnim: case ScriptDrive:
    {
        // a script's animation's root velocity at its time, reversed (0x156AB0 / 0x24F278)
        ASEGA* pasega = p->pasega;
        p->vecVel = VecSlyRootReversed(p, pasega);
        if (static const bool s_f = std::getenv("SLY3_ROOTVELPRINT") != nullptr; s_f && pasega)
            std::printf("rootvel: t %.3f aseg %d at %.3f vel %.1f %.1f %.1f actual %.1f %.1f %.1f pos %.0f %.0f %.0f floor %d\n", g_clock.t, pasega->paseg ? pasega->paseg->oid : -1,
                        pasega->t, p->vecVel.x, p->vecVel.y, p->vecVel.z, VelAloPos(p->palo).x, VelAloPos(p->palo).y, VelAloPos(p->palo).z,
                        PosAloCenter(p->palo).x, PosAloCenter(p->palo).y, PosAloCenter(p->palo).z, p->pxpFloor ? 1 : 0);
        if (p->jts == ScriptDrive && pasega)  // (16: the speed by the stick (0x138B68), the heading to it (0x24E878))
        {
            if (PADCTL* pc = PpadctlOf(p))
            {
                SetAsegaSpeed(pasega, pc->uJoy);
                SetStepPhysHeading(p->palo, pc->radJoy);
            }
        }
        break;
    }
    case Boots: UpdateSlySlideInput(p); break;
    case RideOn: break;   // (the face buttons to the password job 0xE7 (0x18C5A8): natively its pad hold, aipassword3.cpp)
    case Map:
        // orig3 0x1AD1D0 case 24: substate 8's heading is set by the map mode (game/treasuremap3.cpp TakePadForMap3, which
        // has the pad); substate 9: the step clip's root velocity, reversed (0x24F278)
        if (p->jtbs == SubMapClip) p->vecVel = VecSlyRootReversed(p, p->pasega);
        break;
    case Walk: case Climb: case ScriptSm: case HangFollow: case Throw: case Attack: case Carried: case Ride: break;
    // Sly 2's states (no Sly 3 state)
    case N2Hypno:
        // the hypnosis: the clip +0x1D34's root velocity, reversed, turned toward the stick while it is pushed
        p->vecVel = VecSlyRootReversed(p, p->pasega);
        if (0.0f < UJoy(p)) SetStepPhysHeading(p->palo, RadJoy(p));
        break;
    case N2Rush: p->vecVel = VecSlyRootReversed(p, p->pasega); break;
    case N2AirSwing: case N2AirSlam: case N2ClimbSurf: break;
    default:
        WarnOnce("the input of state " + std::to_string(p->jts));
        break;
    }
    UpdateDamage(p);                // (Sly 2's 0x1A1358 every state; Sly 3's 0x1C2B48 in state 5)
    UpdateSafeGround(p, p->jts);    // 0x1C0CB8
    // the ability's animation released once retracted (+0x13F8 / +0x13FC)
    if (p->pasegaAbility && FAsegaRetracted(p->pasegaAbility))
    {
        ReleaseAsega(p->pasegaAbility);
        p->pasegaAbility = nullptr;
        p->idAbility = -1;
    }
}

// orig: 0x0018A0C8 (state 0: the idle SM's mask, the support's turn, the feet's probes 0x189C40 / 0x189EC8 (radius
// 5: the feet set on the floor under them by +0x19F0 / +0x19F4, as in the slide), a foot over a drop: the idle SM's
// state 0x17C once) (orig3: 0x1AB760, slot 110: 0x155AF0 then the same with Sly 3's state 0x19A)
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
            !p->f1514 && p->psma)
        {
            // (orig3 0x1AB760, Sly's slot 110: the running SM's state 0x19A (Sly 2's 0x17C, Sly 3's idle SM is shifted by
            // 0x1D / 0x1E: AUDIT4 PLAYER-23), looked up in the SM of the running one (+0x1140 +0x24))
            if (p->psma->psm && IsmsFindSm(p->psma->psm, 0x19A) >= 0) SetSmaState(p->psma, 0x19A);  // 0x239A78 / 0x23A070
            p->f1514 = 1;
        }
    }
}

// Optional observer for the authored mode-animation hand-off. Controller identities
// are reported as clip IDs and record indices, including events with no matching lane.
void TraceSlyModeEvent(const SLY* p, const ASEGMSG9* pm, const char* phase)
{
    static const bool enabled = std::getenv("SLY2N_SLYMODEEVENT") != nullptr;
    if (!enabled) return;
    const auto clip = [](const ASEGA* a) { return a && a->paseg ? a->paseg->oid : -1; };
    const XF* hand = p->slide.aphandle[0] ? p->slide.aphandle[0]->pxf : nullptr;
    for (size_t i = 0; i < p->palo->axf.size(); ++i)
    {
        const XF* xf = p->palo->axf[i].get();
        const bool selected = pm->n == 0x16E ? xf == hand : pm->n == 0x16F ?
            (xf->tag == 24 || xf->tag == 19 || xf->tag == 21) :
            (xf->tag == 25 || xf->tag == 18 || xf->tag == 20);
        if (!selected) continue;
        const CTRL* matching = nullptr;
        for (const CTRL* c = xf->pctrlFirst; c; c = c->pnext)
            if (c->kind == 0 && static_cast<const CTRLASEG*>(c)->pasega == p->pasegaMode)
            {
                matching = c;
                break;
            }
        const CTRL* head = xf->pctrlFirst;
        const int headClip = head && head->kind == 0 ? clip(static_cast<const CTRLASEG*>(head)->pasega) : -1;
        std::fprintf(stderr, "SLYMODEEV,%s,%.9g,%d,%d,%d,%zu,%d,%d,%d,%d,%d,%d\n",
                     phase, g_clock.t, pm->n, clip(pm->pasega), clip(p->pasegaMode), i,
                     xf->tag, xf->kind, matching ? 1 : 0, matching ? matching->prio : -999,
                     head ? head->kind : -1, headClip);
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
            switch (psly->jts)
            {
            // (orig3 0x1AA988 message 5: the pickpocket's clip ends: state 0; the interaction's: +0x1144 -1)
            case jts::Pickpocket: case jts::N2Rush: SetSlyState(psly, jts::Stand, -1); break;
            case jts::Interact: case jts::N2ClimbSurf: psly->n1038 = -1; break;
            case jts::Target: if (psly->jtbs == jts::SubN2Strike || psly->jtbs == jts::SubN2StrikeUp) SetSlyState(psly, jts::Target, jts::SubTargetStance); break;
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
        // (s3-gadgets) the events below by Sly 3's ids (orig3 0x153738: 0x17F the animation's velocity, 0x182 / 0x183,
        // 0x2E8 the throw's release, 0x2E9 a gadget's activation; Sly 2's 0x15E, 0x161 / 0x162, 0x27B, 0x27C were still
        // tested here, which are other events in Sly 3: no thrown gadget ever left the hand)
        if (pmsg && pmsg->pasega && pmsg->pasega->palo == pa->palo) OnOutfitEvent3(pa->palo, pmsg->n);   // (Sly 3: 0x173308; s3-decomp)
        // orig: 0x140F90 tests the animation's owner, not the actor's current
        // animation reference. Keep this distinction for the deferred Smoke event.
        if (pmsg && pmsg->n == 0x2E9 && pmsg->pasega && pmsg->pasega->palo == pa->palo && pa->n4C8 == 11)
        {
            // orig: 0x140F90 event 0x27C: the interaction's object (+0x4D4) a thrown gadget out there (cid 100,
            // 0x224590: state 1 / 2) is triggered (0x2245B8: slot 76 with 5 for kind 11, else 3)
            LO* ploT = pa->inter.ploTarget;
            if (ploT && FIsLoDerivedFrom(ploT, S3CID(100)))
                if (const CARRY* pc = PcarryFromLo(ploT); pc && (pc->state == 1 || pc->state == 2))
                    SetCarryState(ploT, pc->kind == 11 ? 5 : 3);
        }
        if (pmsg && pmsg->n == 0x2E9 && pmsg->pasega && pmsg->pasega->palo == pa->palo &&
            (ReleaseCaneAbility(pa) || ReleaseSmokeBomb(pa) || ReleaseTimeLock(pa) || ReleaseRoar(pa))) break;
        // orig: 0x140F90 -> 0x2205A8: release follows the event owner's throw
        // controller even when its current main animation reference has changed.
        if (pmsg && pmsg->n == 0x2E8 && pmsg->pasega && pmsg->pasega->palo == pa->palo)
        {
            if (pa->jtthrow.state != -1) ReleaseJtThrow(pa);
            break;
        }
        if (psly || sly3::FIsGuru3(pa->palo) || sly3::FIsPandaKing3(pa->palo)) sly3::HandleCombo3Event(pa, pmsg);  // (Sly 3: orig3 0x153738 passes every message to 0x13A770)
        const bool fOwn = pmsg->pasega == pa->pasega || (pa->psma && pa->psma->pasega == pmsg->pasega) ||
                          (psly && pmsg->pasega == psly->pasegaMode);
        if (static const bool s_f = std::getenv("SLY3_SLYEVPRINT") != nullptr; s_f && psly)   // (debug: Sly's events)
            std::printf("slyev t %.3f n 0x%X own %d aseg %d target %d\n", g_clock.t, pmsg->n, int(fOwn),
                        pmsg->pasega && pmsg->pasega->paseg ? pmsg->pasega->paseg->oid : -1, psly->plo10DC ? psly->plo10DC->oid : -1);
        if (!fOwn) break;
        switch (pmsg->n)
        {
        case 0x17F:   // (orig3 0x153738: Sly 3's id; Sly 2's 0x15E)
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
        case 0x182: pa->f1044 = 1; break;   // (orig3 0x153738 -> 0x153EA0(p, 1); Sly 2's 0x161)
        case 0x183:   // (orig3 0x153738 -> 0x153EA0(p, 0); Sly 2's 0x162)
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
                case 0xC: case 0xE: case 0x10: case 0x12: case 0x14: SetSlyState(psly, jts::Target, jts::SubN2WindUpL); break;
                case 0xD: case 0xF: case 0x11: case 0x13: case 0x15: SetSlyState(psly, jts::Target, jts::SubN2WindUpR); break;
                default: break;
                }
            break;
        case 0x185: if (psly) PickpocketTake(psly); break;  // orig3 0x1AA988 (Sly 2's 0x167): the pickpocket's take
        // orig3 0x1AA988 0x2A6 / 0x2A7 (Sly 2's 0x173 / 0x174 are other events in Sly 3: not tested): the takedown's
        // catch: Sly stops (+0x1D4C, his velocity 0), the target's (+0x11B4) launch job 0xDA (Sly 2's 0xB1) held to his
        // part (state 4) / let fall (6), for a target of Sly 3's class 0x3A (a guard, sly3::CID_GUARD; e.g. the boot
        // intro's ASEG 244 on the elephant 15106 after the script's combo 7561)
        case 0x2A6:
            if (psly)
            {
                psly->f1D2C = 1;
                psly->vecVel = glm::vec3(0.0f);
                SetAloVelocityTo(psly->palo, psly->vecVel);
                if (psly->plo10DC && FIsLoDerivedFrom(psly->plo10DC, sly3::CID_GUARD))
                    if (GUARD* pg = PguardFromLo(psly->plo10DC))
                        if (AIJOB* pj = PjobFind(pg, 0xB1)) pj->SetState(4);  // 0x2387A8(+0x29C, 0xDA) / 0x16E118
                if (std::getenv("SLY3_TAKEDOWNPRINT"))
                {
                    GUARD* pg = PguardFromLo(psly->plo10DC);
                    std::printf("takedown: event 0x%X target %d guard %d job b1 %d t %.3f\n", pmsg->n, psly->plo10DC ? psly->plo10DC->oid : -1, pg != nullptr,
                                pg && PjobFind(pg, 0xB1) != nullptr, g_clock.t);
                }
            }
            break;
        case 0x2A7:
            if (psly && psly->plo10DC && FIsLoDerivedFrom(psly->plo10DC, sly3::CID_GUARD))
                if (GUARD* pg = PguardFromLo(psly->plo10DC))
                    if (AIJOB* pj = PjobFind(pg, 0xB1)) pj->SetState(6);
            break;
        case 0x1F6:  // orig3 0x153738 (Sly 2's 0x1C8, 0x140F90 / 0x1413CC): prize flies to the hand (0x208D50)
            if (pa->n4C8 == 12) StartReachGrab(&pa->inter.reach, pa->inter.ploTarget);
            break;
        case 0x1F5:  // orig3 0x153738 (Sly 2's 0x1C7): arrived; resume the pose (0x138B68) and attach (0x2091B0)
            if (pa->n4C8 == 12)
            {
                SetAsegaSpeed(pa->pasega, 1.0f);
                AttachReachObject(&pa->inter.reach, pa->inter.ploTarget);
            }
            break;
        // (orig3 0x153738 / 0x1AA988: Sly 2's 0x16D (the slam's landing noise) and 0x16E..0x170 (the mode lanes'
        // demotion) are not tested in Sly 3: no slam / swing modes, other events there; AUDIT4 PLAYER-13)
        case 0x32B:   // orig3 0x153738: the damage state >= 9 (Sly 3's; native 8, Sly 3 inserted a kind 7) -> vt slot 103
            if (pa->nSubDF4 >= 8) ActorDeathSlot103(pa, true);
            break;
        case 0x27C:
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
        struct HITMSG { LO* receiver; LO* sender; CTMSG* pctm; };   // (so.cpp's message 0xB)
        const auto* hit = static_cast<const HITMSG*>(pv);
        if (hit && hit->sender == pa->palo && (psly || sly3::FIsGuru3(pa->palo) || sly3::FIsPandaKing3(pa->palo))) sly3::HandleCombo3Contact(pa, hit->pctm);  // (Sly 3: 0x13A770 message 12)
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
    if (pa->n4C8 == 2 || FShadowPower2(pa))   // (Sly 3: Shadow Power Level 2 fades too, 0x154FD8; s3-decomp)
    {
        g0 = 0.15f;
        g1 = 0.0f;
    }
    static const cmm::SMP s_smp{12.0f, 2.0f, 0.1f};  // 0x2BD270
    pa->g10D4 = cmm::GSmooth(pa->g10D4, g0, g_clock.dt, &s_smp, nullptr);  // 0x11D018
    pa->g10D8 = cmm::GSmooth(pa->g10D8, g1, g_clock.dt, &s_smp, nullptr);
    if (SLY* psly = dynamic_cast<SLY*>(pa); psly && psly == PslyGlobal())
        for (LO* plo : {psly->plo19A0, psly->plo19A4})
            if (plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30))) static_cast<ALO*>(plo)->gFade = pa->g10D4;
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
        // orig3: the actors' post-load bind table D_00342EB0 (0x1C8970): {flags 0x300, key 770, +0x119C}: Sly 3's noise
        // template child is oid 770 (Sly 2's 684). With 684 none was found, so the player never made a noise and the
        // v_interpol police (they detect by noise: 0x152E88 / 0x153158) never investigated nor caught Sly.
        LO* plo = PloFindSwChild(pa->palo, 770);
        pa->ploNoise = plo && plo->Cid() == S3CID(0x75) ? plo : nullptr;
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
        // orig3: JT's slot 112 0x1AAC30: 0x1530F8 first (0 while the actor's players' pads are paused, 0x1A9478 with
        // +0x130; 1 while a combo runs whose definition is noisy), then Sly 3's states 2 (the walk) / 6 (the guard's
        // stance) / 7 (the dodge) noisy (AUDIT4 PLAYER-07)
        if (FScripts3Active() && FPlayersPausedSel3(int(psly->palo->U32Field(0, 0x130, 0xFFFFFFFFu)))) return 0;
        if (sly3::FCombo3Noisy(psly)) return 1;
        switch (psly->jts) { case jts::Walk: case jts::Target: case jts::Dodge: return 1; default: return -1; }
    }
    // orig3: Bentley's / Murray's slot 112 (0x13F290 / 0x1E0A78) start with 0x1530F8 too (AUDIT5 PLAYER5-03)
    if (dynamic_cast<BENTLEY*>(pa) || dynamic_cast<MURRAY*>(pa))
    {
        if (FScripts3Active() && FPlayersPausedSel3(int(pa->palo->U32Field(0, 0x130, 0xFFFFFFFFu)))) return 0;
        if (sly3::FCombo3Noisy(pa)) return 1;
    }
    if (BENTLEY* pb = dynamic_cast<BENTLEY*>(pa))
    {
        switch (pb->state) { case BS3_Walk: case BS3_Push: case BS3_Push6: case BS3_Push7: case BS3_Adrenaline: return 1; default: return -1; }   // (orig3 0x13F290)
    }
    if (MURRAY* pm = dynamic_cast<MURRAY*>(pa))
    {
        switch (pm->state)
        {
        case 1: case 12: return 1;   // (orig3 0x1E0A78 on the native numbering: Sly 3's 1 / 13, 5, 10 = native 1 / 12, 5, 9)
        case 5: return pm->n1458 == 2 ? 1 : -1;   // the noisy carry move (Sly 3's +0x1594 == 2)
        case 9: return pm->n17F4 ? 1 : -1;
        default: return -1;
        }
    }
    // (the Guru / Penelope / the Panda King: their slot 112, game/guru3.cpp; before: always -1)
    if (const int n = sly3::NGuruNoisy3(pa); n != -2) return n;
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
    // orig: the registered guards in the live-list order.
    for (GUARD* pg : ApguardActive())
    {
        LO* plo = pg->palo;
        if (FGuardState(pg, 0x1D) || FGuardState(pg, 0xB)) { r = 2; break; }
        if (FGuardAcceptsNoise(pg, pn))
        {
            r = 1;
            if (std::getenv("SLY2N_ACTORNOISE")) std::printf("actornoise: guard %d (cid 0x%X) takes it\n", plo->oid, plo->Cid());
            break;
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
        std::printf("actornoise %d: st %d t %.3f cc %.3f c8 %d k %d d0 %.3f (tmpl %d r %.0f z %.0f..%.0f life %.2f kind %d)\n",
                    pa->palo->oid, dynamic_cast<SLY*>(pa) ? dynamic_cast<SLY*>(pa)->jts : -9, g_clock.t, pa->g10CC, pa->n10C8, k, pa->g10D0, ptmpl->nType, ptmpl->sRadius, ptmpl->rz.x,
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
                else if (sly3::FDriveActorFace2F(pa, pc)) {}    // (Sly 3: SCPT 0x2F's turn to an object, player3.cpp)
                else if (sly3::FDriveActorWalk30(pa, pc)) {}   // (Sly 3: SCPT 0x30's walk, player3.cpp)
                else if (sly3::FDriveActorClimb26(pa, pc)) {}   // (Sly 3: SCPT 0x26's climb, player3.cpp)
                sly3::ApplyActorHold5D3(pa, pc);   // (Sly 3: SCPT 0x5D / 0x5E, player3.cpp)
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
    // (Sly 3) orig3 0x152A48: the actor's combos (vt +0x314; Sly's 0x139FE0) before the damage
    if (SLY* psly = dynamic_cast<SLY*>(pa); psly && !FPcCarm(psly->palo)) sly3::UpdateCombo3(psly);
    else if (sly3::FIsGuru3(pa->palo) || sly3::FIsPandaKing3(pa->palo)) sly3::UpdateCombo3(pa);   // (the Panda King's +0x10D0 the same)   // (the Guru's vt +0x314: 0x139FC0 -> 0x13A178)
}

namespace
{
// orig: 0x0011E630 (the parameters of the nearest points of the lines p0 p1 and q0 q1; parallel: 0, 0) (orig3: 0x120200, the same C in the Sly 3 decomp)
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
// orig: 0x0011E7C8 (the distance of the segments' nearest points, each parameter clamped to [0, 1]) (orig3: 0x120398, the same C in the Sly 3 decomp)
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
    if (FPcCarm(p->palo)) return;  // (PC addition: Carmelita has no cane to catch pipes / hooks with)
    if (p->mode == 0) UpdateReach(&p->reach);  // 0x18ED70 (0x1CC2E8)
    if (p->mode != 0 || !p->reach.ploTarget) return;
    LO* plo = p->reach.ploTarget;
    const CRV* pcrv = FIsLoDerivedFrom(plo, S3CID(0x0B)) ? PcrvJtPipe(plo) : nullptr;
    glm::vec3 anp[3];
    if (pcrv)
    {
        for (int i = 0; i < 3; ++i) NearestCrvS(pcrv, glm::vec3(p->aq[i]), p->reach.sTarget, &anp[i], nullptr, nullptr, nullptr);  // 0x1E7380
    }
    else if (FIsLoDerivedFrom(plo, S3CID(0x13)))
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
            SetSlyState(p, jts::Climb, -1);
        }
        else
        {
            // a hook (cid 0x13): +0x18C4; its kind +0x124 3: the hang-follow (17), else the swing (4, 1)
            p->ploOther = plo;
            if (plo->U32Field(0, 0x124, 0) == 3) SetSlyState(p, jts::HangFollow, -1);
            else SetSlyState(p, jts::Hang, jts::SubSwingStill);
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
                     prefix,g_clock.t,g_clock.dt,p->jts,p->jtbs,p->jump.kind,p->jump.state,p->n4C8,p->nSub4CC,NActorPower(p),
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
    if (p->jts == 0)
    {
        if (XP* pxp = p->pxpFloor) plo = pxp->axpd[1].palo;  // (+0x4C)
    }
    else if (p->jts == 8 && p->nSub4CC == 0)  // (+0x4CC 0; +0x4C8 the kind: 4 an object, 3 a SHAPE)
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
    if (FIsLoDerivedFrom(plo, S3CID(0x30))) pxf = PxfObj(static_cast<ALO*>(plo));
    else if (FIsLoDerivedFrom(plo, S3CID(3))) pxf = static_cast<LOX*>(plo)->pxf;
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
        std::printf("lean t %.3f state %d obj %d v (%.1f %.1f %.1f) 19E4 (%.2f %.2f %.2f) feet %.2f %.2f\n", g_clock.t, p->jts, plo->oid, v.x, v.y, v.z,
                    lean.x, lean.y, gFootR, ah[3] ? ah[3]->ofs.z : 0.0f, ah[4] ? ah[4]->ofs.z : 0.0f);
}

// orig: 0x0018BCE0
void (*g_pfnSlyUpdated)(SLY* p) = nullptr;
// (s3-player, the route harness: an actor hanging on a ledge (the jump's states), in an interaction or on a pipe is let
// go before a teleport, as a script's placement does (orig3 0x226D00 case 34: SetJumpKind 11 + the contacts detached).
// Otherwise the hang pulled a teleported Sly back the next frame: the prologue's chain stayed at ledge 15188.)
void ReleaseActorForTeleport3(ACTOR* p)
{
    if (p->nSub4CC == -1 && p->nSubF54 == -1 && p->jump.state == -1) return;
    SetJumpKind(&p->jump, 0xB, 0, nullptr);
    DetachAloContactsAll(p->palo);
}

void UpdateSly(LO* plo)
{
    SLY* p = PslyFromLo(plo);
    if (!p) return;
    // (PC, split screen's teams: another player's Sly tracks his own jump / use targets: his track swapped in for his
    // update (the original's one track SW +0x4678 is the current player's))
    struct JTSWAP
    {
        JTTRACK* ptr = nullptr;
        explicit JTSWAP(LO* plo)
        {
            if (split3::IExtraPlayerOf(plo) < 0) return;
            static std::unordered_map<const LO*, JTTRACK> s_mptrack;
            static SW* s_pswTrack = nullptr;
            if (s_pswTrack != g_psw) s_mptrack.clear(), s_pswTrack = g_psw;
            ptr = &s_mptrack[plo];
            std::swap(*ptr, g_jttrack);
            g_fJtOtherPlayer3 = true;
        }
        ~JTSWAP()
        {
            if (!ptr) return;
            static int s_cLast = -1;
            if (static const bool s_f = std::getenv("SLY3_JTCOPYPRINT") != nullptr; s_f && g_jttrack.c != s_cLast && (s_cLast = g_jttrack.c, true))
                std::printf("jt copy: %d tracked, first %d (list %d)\n", g_jttrack.c, g_jttrack.c ? g_jttrack.ae[0].plo->oid : -1, g_jttrack.c ? int(g_jttrack.ae[0].jtl) : -1);
            std::swap(*ptr, g_jttrack);
            g_fJtOtherPlayer3 = false;
        }
    } jtswap(plo);
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
        // (SLY2N_TELEPORTALIVE=1: none while a damage reaction runs, e.g. tools/cops2p.sh holding Sly in Carmelita's aim)
        else if (c >= 4 && fNow && !(p->nSubDF4 != -1 && std::getenv("SLY2N_TELEPORTALIVE")))
        {
            // (a ledge's hang (jump state 17) would hold him where he was: let go first, as a route's teleport step)
            if (p->jump.state == 17) ReleaseActorForTeleport3(p);
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
    // (debug: a route's "teleport" step, route.cpp; left to gang.cpp's ApplyRouteTeleportActor when Bentley or Murray (or
    // guru3.cpp's for the Guru) is the current player, e.g. Ep1 Heist 1904 after 1902 hands control to Bentley; to rccar3.cpp's update for the RC car)
    LO* pcurTele = g_routetele.fPending && g_psw ? PloPoCurrent(g_psw) : nullptr;
    if (g_routetele.fPending && !(pcurTele && pcurTele != static_cast<LO*>(p->palo) &&
                                  (FIsLoDerivedFrom(pcurTele, S3CID(0x36)) || FIsLoDerivedFrom(pcurTele, S3CID(0x37)) || FIsLoDerivedFrom(pcurTele, sly3::CID_GURU) ||
                                   FIsRcCar3(pcurTele))))
    {
        g_routetele.fPending = false;
        ReleaseActorForTeleport3(p);
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
            for (LO* plo : s_psw->mpcidaplo[S3CID(0x62)])
            {
                ALO* palo = static_cast<ALO*>(plo);
                const glm::vec3 c = PosAloCenter(palo);
                std::printf("brklist: %d parent %d inworld %d removed %d oxa %d soes %zu at (%.0f %.0f %.0f) origin (%.0f %.0f %.0f)\n", plo->oid,
                            plo->paloParent ? plo->paloParent->oid : -1, plo->fInWorld, plo->fRemoved, Poxa(palo) ? 1 : 0,
                            palo->asoe.size(), c.x, c.y, c.z, MatWorldOfAlo(palo)[3].x, MatWorldOfAlo(palo)[3].y, MatWorldOfAlo(palo)[3].z);
            }
    }
    // (debug: SLY2N_TELEGIVER="OID@N[,D]" puts Sly D (default 150) behind the first guard OID in the world at his N-th
    // update, facing it: a pickpocket in the real game; OID -1: the first guard with a prize, -N the N-th (wrapping))
    static const char* s_szGiver = std::getenv("SLY2N_TELEGIVER");
    // (the giver teleport: OID's guard / object; OID -1: the first guard with a prize; D behind it)
    const auto giver = [&](int oid, float d) {
        int cSkip = oid < 0 ? -oid - 1 : 0;  // (OID -N: the N-th such guard, wrapping)
        int cPrize = 0;
        if (oid < 0)
            for (const auto& up : s_psw->aploAll)
                cPrize += FCidDerivedFrom(up->Cid(), S3CID(0x38)) && PocketOf(up.get()).nMode == 2 && up->fInWorld;
        if (cPrize) cSkip %= cPrize;
        for (const auto& up : s_psw->aploAll)
        {
            LO* pg = up.get();
            // (OID -1: the first guard with a prize in its pocket, mode 2: the sparkle)
            if ((oid >= 0 ? pg->oid != oid : (!FCidDerivedFrom(pg->Cid(), S3CID(0x38)) || PocketOf(pg).nMode != 2)) || !pg->fInWorld)
                continue;
            if (cSkip > 0) { --cSkip; continue; }
            if (!FCidDerivedFrom(pg->Cid(), S3CID(0x30)))
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
            glm::vec3 pos = PosAloCenter(pag) - fwd * std::fabs(d);
            // (a guard with a pickpocket volume (+0x3F4, option 304): Sly at its center (the guard's height), facing
            // the guard: the matrix row is not the walk's heading for every guard, e.g. Ep1's repairman 11539)
            if (GUARD* pgd = PguardFromLo(pg); pgd && pgd->plo3F4)
            {
                LO* pv = pgd->plo3F4;
                const glm::vec3 c = FIsLoDerivedFrom(pv, S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(pv)) : PosLoWorld(pv), g = PosAloCenter(pag);
                if (glm::length(glm::vec2(g - c)) > 1.0f)
                {
                    pos = glm::vec3(c.x, c.y, g.z);
                    fwd = glm::normalize(glm::vec3(g.x - c.x, g.y - c.y, 0.0f));
                    // (from the volume's center toward the guard: -D for D < 0, else min(80, half the way), so the
                    // pickpocket's arm reaches a guard walking away at the take: 0x18B460's test, R12)
                    pos += fwd * (d < 0.0f ? -d : std::min(80.0f, 0.5f * glm::length(glm::vec2(g - c))));
                    if (std::getenv("SLY2N_POCKETPRINT")) std::printf("telegiver: volume center %.0f from the guard\n", glm::length(glm::vec2(g - c)));
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
        std::printf("floor t %.3f st %d sly (%.0f %.0f %.0f) root %d obj %d vel (%.1f %.1f %.1f) rootvel (%.1f %.1f %.1f)\n", g_clock.t, p->jts,
                    s.x, s.y, s.z, pr ? pr->oid : -1, po ? po->oid : -1, v.x, v.y, v.z, pr ? VelAloPos(const_cast<ALO*>(pr)).x : 0.f,
                    pr ? VelAloPos(const_cast<ALO*>(pr)).y : 0.f, pr ? VelAloPos(const_cast<ALO*>(pr)).z : 0.f);
    }
    if (!plo->fInWorld) return;
    if (PpadctlOf(p)) UpdateSlyInput(p);
    // (orig3 0x1AD7F0, Sly 3's slot 18: Sly 3's states)
    switch (p->jts)
    {
    case jts::Stand: UpdateSlyStanding(p); break;   // (vt slot 110, 0x1AB760)
    case jts::Walk:
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitWalk, 1.0f));
        UpdateGaitFeet(&p->gaitWalk);
        p->vecVel *= F32(0x3FA66666);  // 1.3 (0x2BF3D4)
        break;
    case jts::Run: UpdateGaitFeet(PgaitRunSly(p)); break;
    case jts::Target:
        if (p->jtbs == jts::SubTargetWalk)
        {
            SteerActorRootBlend(p, &p->rb13D0);  // 0x1A0450
            p->vecVel *= F32(0x3F59999A);        // 0.85
            p->uTurn = 1.0f;
        }
        break;
    case jts::Sneak: SteerActorRootBlend(p, &p->rb1AC0); break;  // 0x1C1550
    case jts::Hang: UpdateSlyHangPlace(p); break;  // 0x1B2430
    case jts::N2ClimbSurf: UpdateSlyClimbSurf(p); break;  // (Sly 2's 0x185E40)
    case jts::HangFollow: UpdateSlyHangFollow(p); break;  // 0x1AFBE8
    case jts::Boots: UpdateSlySlideFeet(p); break;  // 0x1AB308 / 0x1AB560
    case jts::Map: break;   // (0x1B0070: the map mode, po.cpp -> game/treasuremap3.cpp TakePadForMap3)
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
                     g_clock.t,g_clock.dt,p->palo->oid,p->jts,p->jtbs,p->n4C8,p->nSub4CC,NActorPower(p),p->g10D4,p->g10D8,
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
        const glm::vec3 posT = pt && FCidDerivedFrom(pt->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(pt)) : glm::vec3(0.0f);
        std::printf("hidetrace t %.4f jump %d kind %d sub %d state %d pos %.2f %.2f %.2f obj %.2f %.2f %.2f\n", g_clock.t, p->jump.state, p->n4C8,
                    p->nSub4CC, p->jts, pos.x, pos.y, pos.z, posT.x, posT.y, posT.z);
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
        std::printf("sly t %.2f: h %.3f state %d/%d jump %d pos %.1f %.1f %.1f lin %d g %.0f floor %d in world %d\n", g_clock.t, p->radHeading, p->jts,
                    p->jtbs, p->jump.state, pos.x, pos.y, pos.z, pst ? pst->cnstrLin.kind : -1, pst ? pst->dvGravity.z : 0.0f,
                    p->pxpFloor ? 1 : 0, plo->fInWorld ? 1 : 0);
        std::printf("   anim: clip %d t %.3f inter %d/%d mode %d rank %d cane %d/%d\n", p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -2,
                    p->pasega ? p->pasega->t : 0.0f, p->n4C8, p->nSub4CC, p->mode, p->n10E4, p->plo19A0 ? p->plo19A0->fInWorld : -1,
                    p->plo19A8 ? p->plo19A8->fInWorld : -1);
        if (p->psma)
            std::printf("   sma: sm %d cur %d (%d) next %d goal %d retracted %d clip %d t %.3f idle sm %d\n", p->psma->psm ? p->psma->psm->oid : -1,
                        p->psma->ismsCur, p->psma->psm && p->psma->ismsCur >= 0 && p->psma->ismsCur < (int)p->psma->psm->asms.size() ? p->psma->psm->asms[p->psma->ismsCur].oid : -1,
                        p->psma->ismsNext, p->psma->ismsGoal, p->psma->fRetracted ? 1 : 0,
                        p->psma->pasega && p->psma->pasega->paseg ? p->psma->pasega->paseg->oid : -1, p->psma->pasega ? p->psma->pasega->t : 0.0f,
                        p->psmIdle ? p->psmIdle->oid : -1);
        else std::printf("   no sma (idle sm %d)\n", p->psmIdle ? p->psmIdle->oid : -1);
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
    // (orig3 0x1AB958: the positions kept only when the followed object (+0x1AA4) has its +0x2F bit 0 (its in-world bit,
    // as 0x153438 reads it); AUDIT4 PLAYER-22)
    if (p->mode || !p->plo19A4 || !p->plo19A4->fInWorld)
    {
        p->cq = 0;
        return;
    }
    p->aq[0] = glm::vec4(PosAloCenter(palo), 1.0f);
    p->aq[2] = glm::vec4(p->plo19A4 ? PosAloCenter(static_cast<ALO*>(p->plo19A4)) : glm::vec3(0.0f), 1.0f);
    p->cq = 3;
}

// orig3: 0x001AA630 (Sly 3's vt slot 74, Sly 2's 70 0x188960): the step's motion (0x24E2B8) unless the states 4 / 9 /
// 17 (gravity only, 0x243738); then the states' own parts (Sly 3's numbers)
void UpdateSlyMotion(ALO* palo)
{
    using namespace jts;
    SLY* p = PslyFromLo(palo);
    if (!p) { UpdateStepPhysMotion(palo); return; }
    switch (p->jts)
    {
    case Hang: case Climb: case HangFollow: ApplyAloGravity(palo); break;
    case N2ClimbSurf: break;   // (Sly 2's: neither)
    default: UpdateStepPhysMotion(palo); break;
    }
    switch (p->jts)
    {
    case Hang: UpdateSlyHangMotion(p); break;    // 0x1B1798
    case Climb: UpdateClimbMotion(p); break;     // 0x1BEF88 (+0x1010)
    case Jump: UpdateJumpMotion(&p->jump); break;  // 0x1B88E0
    case Damage: UpdateDamageMotion(p); break;   // 0x1C2870
    case Interact: UpdateInteractMotion(p); break;  // 0x1B5910 (+0x500)
    case Dodge: UpdateSlyDodgeMotion(p); break;  // 0x140958
    case Attack: sly3::UpdateCombo3Motion(p); break;  // 0x13A9E0 (+0x10D0)
    case Boots: UpdateSlySlideMotion(p); break;  // (jtbs 7: 0x1B0228(p, 0, 1, 6000))
    case N2AirSlam:
    {
        // (Sly 2's 0x188960: follow the air slam's root velocity until its impact marker)
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
    default: break;
    }
}

// orig: 0x0018DEE0 (vt slot 85) (orig3: 0x1AFB70, slot 96: Sly 3's states)
uint32_t GrfSlyXp(ALO* palo)
{
    if (g_pfnGrfSlyXpImported) return g_pfnGrfSlyXpImported(palo);  // (the physics checks: from the reference's RAM)
    SLY* p = PslyFromLo(palo);
    if (!p) return GrfStepXp(palo);
    if (p->fNoContacts) return 0;
    switch (p->jts)
    {
    case jts::Hang: case jts::Climb: case jts::HangFollow: case jts::N2ClimbSurf: return 0;
    case jts::N2AirSwing: case jts::N2AirSlam: return 1;
    case jts::Boots: return 0xF;
    default: return GrfActorStance(p);
    }
}


// orig: 0x00188CC8 (vt slot 55: a new contact: the step's 0x204250, then the state's adjustment)
void OnSlyXpNew(ALO* palo, XP* pxp, int i)
{
    OnStepXpNew(palo, pxp, i);
    SLY* p = PslyFromLo(palo);
    if (!p) return;
    // (orig3 0x1AA768, Sly 3's slot 59: Sly 3's states)
    switch (p->jts)
    {
    case jts::Target:   // (Sly 2's charged strike)
        if ((p->jtbs == jts::SubN2Strike || p->jtbs == jts::SubN2StrikeUp) && g_clock.t - p->tSubstate < 0.25f) pxp->mu = 3.0f;  // +0x94
        break;
    case jts::Damage: AdjustDamageXp(p, pxp); break;  // 0x1C3D18
    case jts::Climb: AdjustClimbXp(pxp); break;       // 0x1BFA08
    case jts::Interact:
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
    case jts::Dodge: pxp->mu = 0.0f; break;  // 0x140BE8
    case jts::Boots:
        pxp->mu = 0.0f;
        pxp->elas = 0.0f;
        break;
    default: break;
    }
}

// orig3: 0x0024E9E0's first part (Sly 3's FindStepFloor, the actors' vt slot 61; Sly 2's PruneJumpLedgeXps 0x199298
// generalised): the actor's contacts with its pair (PairActorWith's +0x2E8) dropped, but the internal ones (kind 11):
// a still pair (vt slot 66 FAloStill): every contact with a still object; a moving one: the contacts with it
void PruneActorPairXps3(ACTOR* pa)
{
    ALO* pso = pa->psoPair3;
    OXA* poxa = Poxa(pa->palo);
    if (!pso || !poxa) return;
    const bool fStill = FAloStill(pso);
    XP* pxpFreed = nullptr;
    for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
    {
        XP** link = &ox->pxp;
        while (XP* x = *link)
        {
            const int side = x->axpd[0].paloRoot != pa->palo ? 1 : 0;
            ALO* pOther = x->axpd[1 - side].paloRoot;
            const bool fDrop = x->xpk != 11 && pOther && (fStill ? FAloStill(pOther) : pOther == pso);
            if (!fDrop) { link = &x->pxpNext; continue; }
            if (pa->pxpFloor == x) pa->pxpFloor = nullptr;
            *link = x->pxpNext;
            x->pxpNext = pxpFreed;
            pxpFreed = x;
        }
        if (ox->poxOther) ox->poxOther->pxp = ox->pxp;
    }
    FreeSwXpList(pxpFreed);  // 0x1CBAA0
}

// orig: 0x00140D70 (vt slot 58 of the actors: the step's floor 0x204768, then the jump's 0x199298). Sly 3 (orig3
// 0x24E9E0, slot 61): the pair's contacts dropped first (PruneActorPairXps3: the ledge hang's 0x199298 is part of it)
void FindActorFloor(ALO* palo)
{
    ACTOR* pa = PactorFromLo(palo);
    if (pa) PruneActorPairXps3(pa);
    FindStepFloor(palo);
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
        const bool fPlayer = paloOther && FIsLoDerivedFrom(paloOther, S3CID(0x34));  // (0x2E1E40 / 0x2DD5BC / 0x2F7900)
        if (fPlayer && (NPoEnabled(palo) != 1 /* || 0x144068: TODO(actors) the carrier */))
            pent->normal = normalSaved * F32(0x3C23D70A);
    }
    using PFN68 = void (*)(ALO*, DZ*, DZENT*, int, int);
    if (auto pfn = reinterpret_cast<PFN68>(PvNativeFn(0x1FAD00))) pfn(palo, pdz, pent, grf, k);  // (solve.cpp)
    pent->normal = normalSaved;
}

// orig: 0x0018A2E0 (vt slot 49: the ALO's bounds 0x1F50E0, then each kept position grown in (0x1F5480, radius +0x19AC)) (orig3: 0x1ABA10, the same C in the Sly 3 decomp)
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
        // (s3-cine2) the Panda King 0x38 and Penelope 0x39 run natively too (game/guru3.cpp, AUDIT4 PLAYER-01 / s3-player
        // round 21): left out, the post-load gave them its stand-in constraints (index 3: kind 2 along their y axis, not
        // fixed), so ep5_job4071's Panda King could only move along y and the contact solve stopped his fall with
        // impulses of 1e6..1e11 whose friction rows slid him 340,000 units (later a NaN). SLY3_PKCNSTR=1: as before
        static const bool s_fOldPk = std::getenv("SLY3_PKCNSTR") != nullptr;
        g_pfnFActorDrivesPhys = [](ALO* palo) {
            const CID cid = palo->Cid();
            if (!s_fOldPk && (cid == sly3::CID_PANDA_KING || cid == sly3::CID_PENELOPE)) return true;
            return cid == S3CID(0x35) || cid == S3CID(0x36) || cid == S3CID(0x37) || cid == sly3::CID_GURU;   // (s3-player: the Guru, game/guru3.cpp)
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
// orig3: 0x0024E9E0 (Sly 3's FindStepFloor, every step's vt slot 61: the pair's contacts pruned first, PruneActorPairXps3)
NATIVEFN3(0x24E9E0, FindActorFloor);
NATIVEFN(0x140D00, RegisterActorContacts);
// orig3: 0x001B0178 (Sly's vt slot 62, Sly 3 only): the actors' 0x153498 (RegisterActorContacts), then in Sly 3's
// state 17 (+0x19E0 == 0x11) a line contact of his custom entry (+0x2E8 slot) along 0x1AFE60's direction.
// TODO(player): the state-17 line contact (Sly 3's state 17 is not mapped from a Sly 2 state yet).
void RegisterSlyContacts3(ALO* palo) { RegisterActorContacts(palo); }
NATIVEFN3(0x1B0178, RegisterSlyContacts3);
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
    // (s3-player: an actor hanging on a ledge / in an interaction / on a pipe is let go first, as a script's placement
    // does (orig3 0x226D00 case 34: SetJumpKind 11 + the contacts detached): the interaction pulled a teleported Sly
    // back to its ledge the next frame, so the prologue's routes stayed at ledge 15188 from the approach on)
    ReleaseActorForTeleport3(p);
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

