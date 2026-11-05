// gang.cpp: Bentley (cid 0x37) and Murray (cid 0x36), gang.h. Ported from the decomp's matching C (src/hand:
// 0x134648, 0x1347A8, 0x134850, 0x134D30, 0x135248, 0x135940, 0x135A48, 0x136338, 0x136CC0, 0x136DC0, 0x136F48,
// 0x1B49E0, 0x1B4AA0, 0x1B67F8, 0x1B6728, 0x1B70A0, 0x1B7E40, 0x1B82A0, 0x1B8368; build/hand_wip: 0x134DA8,
// 0x1B4728, 0x1B50B0, 0x1B6078, checked against the disassembly) and the reference's m_134648_p3.cpp /
// m_1B4438_p3.cpp.
#include "gang.h"
#include "routetele.h"
#include "transition.h"
#include "target.h"
#include "guard.h"
#include "aijobs.h"
#include "ai.h"
#include "murraycarry.h"
#include "gadget.h"
void SpawnPickups(int cid, const glm::vec3& pos, LO* psrc, int n);  // pickup.cpp (0x1C6FD8)
#include "jt.h"
#include "binoc.h"
#include "damage.h"

#include "activate.h"
#include "ai.h"
#include "ainoise.h"
#include <algorithm>
struct MURRAY;
void UpdateMurrayBerserkTransitions(MURRAY* p);  // (below, 0x1B8EC0)
#include "actanim.h"
#include "interact.h"
#include "actmesh.h"
#include "actor.h"
#include "jtthrow.h"
#include "alo.h"
#include "asega.h"
#include "blend.h"
#include "brx.h"
#include "carry.h"
#include "emitter.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "cplcy.h"
#include "joy.h"
#include "crv.h"
#include "jump.h"
#include "lo.h"
#include "po.h"
#include "objmethod.h"
#include "sly.h"
#include "smrun.h"
#include "so.h"
#include "sound.h"
#include "spiop.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "xf.h"
#include "projectile.h"
void LaunchFrom(LO* plaunch, LO* po);                                  // launcher.cpp (0x1D6B00)
void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset);  // launcher.cpp
#include "bsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using cmm::F32;

void BindLoPartsTable(ALO* palo, int c, uint32_t vaTable);  // loaders.cpp: orig 0x1A64E8
void CompareGangFrame(ACTOR* p, int frame);                  // gangcheck.cpp
extern int g_iframeHarness;
void AccelAloPos(ALO* p, const glm::vec3& v);  // solve.cpp: orig 0x1FAC30
XP* PxpCloneList(XP* head);                  // mark.cpp: orig 0x1AA020
void PrependXpList(XP* first, XP** phead);    // mark.cpp: orig 0x1A9FE0
ALO* PaloXpFilter(ALO* p, uint32_t f);        // mark.cpp: orig 0x1F7530

LO* PloInteractOidField(LO* plo, uint32_t ib);    // interact.cpp

void FaceActorToward(ACTOR* pa, LO* plo);         // interact.cpp: 0x2046F0
namespace
{
SW* s_psw = nullptr;

void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "gang: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

constexpr uint32_t BTN_CROSS = 0x40, BTN_SQUARE = 0x80, BTN_CIRCLE = 0x20, BTN_TRIANGLE = 0x10, BTN_R1 = 0x8;
constexpr uint32_t BTN_R3 = 0x400;

// the actor's controller (+0x150)
PADCTL* PpadctlOf(ACTOR* p)
{
    PODATA* ppd = PpodataFromLo(p->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
float RadJoy(ACTOR* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->radJoy : 0.0f; }
float UJoy(ACTOR* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->uJoy : 0.0f; }
uint32_t GrfbtnHeld(ACTOR* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->grfbtn : 0; }
// orig 0x001C46C0 / 0x001C4660 / 0x001C4738
bool FBtnTake(ACTOR* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlTakePressed(pc, grf, dt); }
bool FBtnWithin(ACTOR* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlPressedWithin(pc, grf, dt); }
void ForgetBtn(ACTOR* p, uint32_t grf) { if (PADCTL* pc = PpadctlOf(p)) ForgetPadctlPresses(pc, grf); }

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

float GDuration(const ASEG* paseg) { return paseg ? PasegdOf(const_cast<ASEG*>(paseg))->tMax : 0.0f; }  // +0x74

// orig 0x002303A0 on the position (+0x58) / rotation (+0x5C) records
void SetXfModes(ACTOR* p, int mode)
{
    if (XF* pxf = PxfPos(p->palo)) SetXfMode(p->palo, pxf, mode);
    if (XF* pxf = PxfRot(p->palo)) SetXfMode(p->palo, pxf, mode);
}

// The state change's tail (0x130658, 0x1F15B8, 0x1305B8, 0x1F14F8): actanim.h's SetActorAnim on the actor's
// +0x1030 / +0x1034
void SetAnim(ACTOR* p, ASEG* paseg, float t, float svt, int grfapl, SM* psm, OID oidState, uint32_t grfMask, bool fSeekEvent)
{
    SetActorAnimPp(&p->pasega, &p->psma, p->palo, paseg, t, svt, grfapl, psm, oidState, grfMask, fSeekEvent);
}

// The gait's phase when a gait state is left (0x135248 states 1 / 7, 0x1B6078 1 / 10): +0x1030's time / its length
float UGaitPhase(ACTOR* p, float uOld)
{
    if (!p->pasega || !p->pasega->paseg) return uOld;
    return p->pasega->t / GDuration(p->pasega->paseg);
}

// The jump's segments (the class's parts table: +0x3E0.. +0x41C, as Sly's)
void BindJumpParts(ACTOR* p)
{
    ALO* palo = p->palo;
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
}

void BindGaitParts(GAIT* pgait, ALO* palo, uint32_t ib)
{
    pgait->apaseg[0] = PPart<ASEG>(palo, ib + 0x14);
    pgait->apaseg[1] = PPart<ASEG>(palo, ib + 0x54);
    BindGait(pgait);  // 0x20B868
}

void BindRootBlendParts(ROOTBLEND* prb, ALO* palo, uint32_t ib)
{
    for (int i = 0; i < 8; ++i) prb->apaseg[i] = PPart<ASEG>(palo, ib + 4 + 0x20 * i);
    BindRootBlend(prb, palo);  // 0x1A0300
}

// The landing's dust (0x135248 / 0x1B6078 leaving the jump: the object added (vt slot 8), its burst 0x15C150 (slot 51
// with no parameters: ExplodeLo), removed (slot 9))
void LandingBurst(LO* po)
{
    if (!po) return;
    AddLo(g_psw, po);
    ExplodeLo(po, nullptr);
    RemoveLo(g_psw, po);
}

// orig 0x00140AF8 (the climbable / pushable in front: TODO(actors), the base's)
bool FActorObjectAhead(ACTOR* p, int fWide) { return FActorTargetInReach(p, fWide != 0); }

// orig: 0x00135EB0 / 0x001B82A0's first part (Bentley's / Murray's safe ground +0x9E0: the floor stood on)
void UpdateActorSupport(ACTOR* p)
{
    XP* pxp = p->pxpFloor;
    if (!pxp || !(F32(0x3F333333) < pxp->normal.z) || p->ploWater) return;  // (+0x88 > 0.7)
    // (the floor object's +0x148 bit 29: not safe ground; else 0x19FEB0 records the floor entry and point stood on in
    // +0x9E0, as Sly's 0x19FC30: the water / long-fall recovery jumps back to it)
    ALO* paloFloor = pxp->axpd[1].palo;
    if (paloFloor && (Sort(paloFloor).grf148 & 0x20000000ull)) return;
    RecordSafeGround(p, pxp->axpd[1].psoe, pxp->axpd[1].posLocal);  // 0x19FEB0
}
}  // namespace

// The players' globals (D_002DD5BC Bentley / D_002F7900 Murray): the world's first of the class
BENTLEY* PbentleyGlobal()
{
    if (!s_psw) return nullptr;
    for (LO* plo : s_psw->mpcidaplo[0x37])
        if (BENTLEY* p = PbentleyFromLo(plo)) return p;
    return nullptr;
}
MURRAY* PmurrayGlobal()
{
    if (!s_psw) return nullptr;
    for (LO* plo : s_psw->mpcidaplo[0x36])
        if (MURRAY* p = PmurrayFromLo(plo)) return p;
    return nullptr;
}

BENTLEY* PbentleyFromLo(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x37)) return nullptr;
    return dynamic_cast<BENTLEY*>(PactorFromLo(plo));
}
MURRAY* PmurrayFromLo(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x36)) return nullptr;
    return dynamic_cast<MURRAY*>(PactorFromLo(plo));
}

STEP* PstepNewGang(ALO* palo)
{
    if (FIsLoDerivedFrom(palo, 0x37)) return new BENTLEY();
    if (FIsLoDerivedFrom(palo, 0x36)) return new MURRAY();
    return nullptr;
}

// ----------------------------------------------------------------------------------------------------------------
// Bentley
// ----------------------------------------------------------------------------------------------------------------

namespace
{
// orig: 0x00134648 (the init)
void InitBentley(BENTLEY* p)
{
    InitActorFields(p);  // 0x13FF70
    p->state = -1;
    p->n16B0 = -1;
    p->n16BC = -1;
    p->n1800 = -1;
    p->g103C = 45.0f;
    p->sHeight = 125.0f;
    InitGait(&p->gaitWalk, p->palo, 0x73);  // 0x20B840
    InitGait(&p->gaitRun, p->palo, 0x73);
    p->jump.g118 = -70.0f;  // +0x3D8
    // TODO(actors): +0x1040, +0x1098..+0x10B4 (the reach: -100 100 50 -10, 250 0 1 4500), the entries' ids
    // +0x1824..+0x1870 (5, 7, 9, 11, 13, 4)
    p->n1000 = 2;
    p->n1870 = 5;
    // (D_002DD5BC = p: the players' globals are found by class here)
}

// orig: 0x00136D28 (the sub-object's init: its six blends of two, 0x137A30)
void InitBentleySub(BENTLEY* p)
{
    BENTLEYSUB& s = p->sub;
    s.v20 = glm::vec4(175.0f, 0.0f, 0.0f, 0.0f);  // D_002BC920
    s.g30 = -50.0f;
    s.g34 = 50.0f;
    s.state = -1;
    ALO* palo = p->palo;
    for (int i = 0; i < 6; ++i)  // (NewBlend 0x137A30 of the parts +0x174C / +0x1758 + 0x1C i)
    {
        ASEG* a = PPart<ASEG>(palo, 0x174C + 0x1C * i);
        ASEG* b = PPart<ASEG>(palo, 0x1758 + 0x1C * i);
        s.apbl[i] = (a || b) ? PblNewPlain(palo, {a, b}) : nullptr;
    }
    s.ploE4 = PloPart(palo, 0x17F4);
}

// orig: 0x001347A8 (the post-load)
void PostLoadBentley(BENTLEY* p)
{
    ALO* palo = p->palo;
    PostLoadActorParts(p);  // 0x1400B8
    PostLoadActorMass(p);
    p->psmIdle = PPart<SM>(palo, 0x1028);
    // EOPID 775 (a field, grf 0x92, +0x1818): no abilities (slot 99 0x136870 refuses them all), loaded 1 on Bentley in
    // Ep7's boss world 37: there TRIANGLE / SQUARE / CIRCLE are the trap radio's (SCPT 3281) only; natively his TRIANGLE
    // summoned a bomb with every trap (which also counted as hits on Bison) and turned him toward Bison
    p->f1818 = static_cast<int>(palo->U32Field(0, 0x1818, static_cast<uint32_t>(p->f1818)));
    p->paseg1820 = PPart<ASEG>(palo, 0x1820);
    p->paseg1878 = PPart<ASEG>(palo, 0x1878);
    BindJumpParts(p);
    for (const auto& up : palo->axf)  // +0x10B8 = 0x1F4C58(p, 0xB, 1)
        if (up && up->tag == 0xB && up->kind == 1) { p->pxf10B8 = up.get(); break; }
    BindRootBlendParts(&p->rb13E0, palo, 0x13E0);  // 0x1A0300
    BindRootBlendParts(&p->rb14F0, palo, 0x14F0);
    BindGaitParts(&p->gaitWalk, palo, 0x1320);     // 0x20B868
    BindGaitParts(&p->gaitRun, palo, 0x1600);
    InitBentleySub(p);                             // 0x136D28
    {
        static const int s_aid[6] = {5, 7, 9, 11, 13, 4};  // (0x134648: +0x1824 + 0xC i)
        for (int i = 0; i < 6; ++i)
        {
            p->aabil[i].id = s_aid[i];
            p->aabil[i].plo4 = PloPart(palo, 0x1828 + 0xC * i);
            p->aabil[i].plo8 = PloPart(palo, 0x182C + 0xC * i);
        }
    }
    AttachActorLook(p, 0x16);  // 0x1426E8 (the head's look controller +0x10F4)
    SetBentleyState(p, 0);
}

// orig: 0x001362F8 (the second machine's state)
void SetBentleyState2(BENTLEY* p, int n)
{
    if (n == p->n16BC) return;
    p->n16BC = n;
    p->t16C0 = g_clock.t;
    if (n == 0 && p->sub.state != -1) WarnOnce("0x136F48 (Bentley's sub-object mode -1)");
}

bool FBentleyCanDo(BENTLEY* p, int n);

// orig: 0x00136F48 (the sub-object's mode: the square's swing, its two clips blended by the target's height and
// chosen by the side of +0xE4; -1 none)
void SetBentleySubMode(BENTLEY* p, int n)
{
    BENTLEYSUB& s = p->sub;
    if (n == s.state) return;
    if (s.state == -1)
    {
        s.t18 = g_clock.t;
        s.nC = s.n10 = s.n14 = 0;
        // 0x18F2B0: the target points of mask 2 near the point 175 ahead in 0.15 s, within 1.5 (mode 0: 3.15) of the
        // heading; its height in the range +0x30..+0x34 weights the pair
        glm::vec3 v(0.0f);
        LO* ploTarget = ChooseAttackTarget(p, p->radHeading, 2, glm::vec3(s.v20), 0.15f, n == 0 ? 3.15f : 1.5f, &v);
        s.g1C = ploTarget ? std::clamp((v.z - s.g30) / (s.g34 - s.g30), 0.0f, 1.0f) : 0.5f;
        glm::vec3 r(0.0f);
        if (s.ploE4) r = glm::vec3(glm::inverse(MatWorldOfAlo(p->palo)) * glm::vec4(PosLoWorld(s.ploE4), 1.0f));  // 0x1A6F00
        s.n8 = 0.0f < r.y ? 1 : 0;
        if (n == 0 && ploTarget)
        {
            SetStepPhysHeadingToward(p->palo, v);  // 0x2046B0
            TurnActorNow(p);                       // 0x204CA8
        }
        if (std::getenv("SLY2N_GANGDEBUG"))
            std::printf("bentley: swing target %d (parent %d) v (%.1f %.1f %.1f) g1C %.3f heading %.3f\n", ploTarget ? ploTarget->oid : -1,
                        ploTarget && ploTarget->paloParent ? ploTarget->paloParent->oid : -1, v.x, v.y, v.z, s.g1C, p->radHeading);
    }
    s.state = n;
    if (std::getenv("SLY2N_GANGDEBUG")) std::printf("bentley: swing mode %d t %.3f side %d\n", n, g_clock.t, s.n8);
    float dt = 0.0f;
    ASEGBL* pbl = nullptr;
    if (n >= 0 && n <= 2)
    {
        pbl = s.apbl[n * 2 + s.n8];
        if (pbl) SetBlendWeights(pbl, {1.0f - s.g1C, s.g1C});  // (+0 / +0xC, 0x1384C0)
        dt = 0.06666667f;
    }
    ApplyAsegCur(pbl, p->palo, g_clock.t - s.t18, 1.0f, 1, &s.pasega);  // 0x1305B8
    if (!s.pasega) return;
    if (s.n10 && p->pxf10B8) DemoteAsegaOnXf(s.pasega, p->palo, p->pxf10B8);  // 0x131B10
    if (dt != 0.0f) SetAsegaSeekTime(s.pasega, g_clock.t + dt);  // 0x131A90
}

// orig: 0x00136DC0 (the sub-object's mode by the state)
void UpdateBentleySub(BENTLEY* p, int n)
{
    BENTLEYSUB& s = p->sub;
    if (s.pasega && FAsegaRetracted(s.pasega)) SetBentleySubMode(p, -1);  // 0x131968
    if (n != s.state && s.state != -1) SetBentleySubMode(p, n);
    if (n != -1 && (s.state == -1 || s.nC))
        if (FBtnWithin(p, BTN_SQUARE, 0.1f) && FBentleyCanDo(p, 2))  // 0x1C4660, slot 99
        {
            ForgetBtn(p, BTN_SQUARE);  // 0x1C4738
            SetBentleySubMode(p, -1);
            SetBentleySubMode(p, n);
        }
    if (!(GrfbtnHeld(p) & BTN_SQUARE)) s.n14 = 1;
}

// orig: 0x00136870 (slot 99: may the action n start)
ABILITY* PabilFind(BENTLEY* p, int k)  // orig 0x00136A28
{
    for (auto& a : p->aabil)
        if (a.id == k) return &a;
    return nullptr;
}
bool FBentleyCanDo(BENTLEY* p, int n)
{
    if (p->f1818) return false;
    if (static_cast<unsigned>(p->nF04) < 3) return false;  // 0x1443D8
    switch (p->state)
    {
    case 0: case 1: case 5: case 7: break;
    case 2:
        if (n != 12) { WarnOnce("0x19C8D0 (the jump's slot 99)"); return false; }
        // orig: 0x0019C8D0 (Hover Pack): the saved jump permission is
        // consumed by kind 17, and targeted/held air states reject it.
        if (!p->jump.f2C) return false;
        if (p->jump.state == 6 || (p->jump.state >= 9 && p->jump.state <= 22)) return false;
        break;
    case 3:
        if (n == 10 || n == 12) {
            // orig: 0x00194558 for Adrenaline Burst / Hover Pack; other abilities keep their own checks.
            switch (p->n4C8) {
            case 1: case 3: case 4: case 8: case 11: case 12: return false;
            default: break;
            }
            break;
        }
        WarnOnce("0x194558 (+0x4C0's slot 99)"); return false;
    case 13: WarnOnce("0x19EEF0 (+0xF50's slot 99)"); return false;
    case 12:
        // orig: 0x00136870 case 12: aiming a carried gadget (+0x11B4 4..6) lets only the same kind's press through (the
        // throw)
        if (static_cast<unsigned>(p->jtthrow.state - 4) < 3)
            switch (n)
            {
            case 4: case 7: case 9: case 11: case 13:
            {
                const CARRY* pc = PcarryFromLo(p->jtthrow.selected);
                return pc && pc->kind == n;
            }
            default: break;
            }
        return false;
    default: return false;
    }
    switch (n)
    {
    case 4: case 5: case 7: case 9: case 11: case 13:
    {
        ABILITY* pa = PabilFind(p, n);
        if (!pa) return false;
        if (pa->plo8)
        {
            // orig: 0x00224590 (the thrown gadget out there: flying or landed, +0x150 1 / 2)
            const CARRY* pc = PcarryFromLo(pa->plo8);
            return pc && (pc->state == 1 || pc->state == 2);
        }
        if (!pa->plo4) return false;
        return PloProxyFreeRow(g_psw, pa->plo4, 2) != nullptr;  // 0x1CADE0
    }
    case 3:  // the binocucom: the pad the players' (G_GRFJOYT & 2) and no default camera on top (0x14FD50 on CM +0x3A0)
        return p->f12F0 != 0 && (g_grfjoyt & 2) && !FActiveCplcy(&g_cmpolicies.cpdefault);
    case 1: case 2: case 8: case 10: case 12: case 14: return true;
    default: return false;
    }
}

}  // namespace
// orig: 0x001356E8 (the dart's aim: the line from the camera's screen centre (depth 10) to 40000 deep, the nearest
// solid entry it meets (0x20FAA0 with 0x20FA10, not the actor's own; 0x14CC58), else its far end) and 0x001357C0 (the
// dart fired: an instance of the dart proxy +0x16DC (+0x16D8 0; 1: +0x16E0) its contact kind grf (its +0xE4 +0x18),
// the launcher +0x16E4 aimed at the point (+0x60 none, +0x50 the point) throws it (0x1D6B00), no spin, its flight over
// within 4 s (0x13B5C8); the second state machine 1 (the reload), +0x1700 the time). Part of the actors code.
void FireBentleyDart(BENTLEY* p, uint32_t grf)
{
    ALO* palo = p->palo;
    const glm::vec3 a = ConvertCmScreenToWorld(&g_cm, glm::vec3(0.0f, 0.0f, 10.0f));      // D_002BC970
    const glm::vec3 b = ConvertCmScreenToWorld(&g_cm, glm::vec3(0.0f, 0.0f, 40000.0f));   // D_002BC980
    std::vector<SOE*> apsoe;
    for (SOE* pe : AsolidWithTree(a, b))
        if (pe->palo != palo) apsoe.push_back(pe);
    LSG lsg;
    glm::vec3 posAim = PsoeHitTestLineObjects(1, a, b, apsoe, &lsg) ? lsg.pos0 : b;
    // (debug: SLY2N_DARTAT=oid aims at that object's centre instead of the screen's)
    if (static const char* s_sz = std::getenv("SLY2N_DARTAT"); s_sz)
        if (LO* pt = PloFindSwObjectByOid(g_psw, std::atoi(s_sz)); pt && FCidDerivedFrom(pt->Cid(), 0x30))
            posAim = PosAloCenter(static_cast<ALO*>(pt));
    // +0x16D8 the dart kind: EOPID 774 (a field, grf 0x92) that the jobs' scripts set, e.g. 1 for Ep3's red noise darts
    const int nDart = static_cast<int>(palo->U32Field(0, 0x16D8, static_cast<uint32_t>(p->n16D8)));
    LO* pproxy = nDart == 0 ? PPart<LO>(palo, 0x16DC) : nDart == 1 ? PPart<LO>(palo, 0x16E0) : nullptr;
    if (!pproxy) return;
    LO* po = PloAcquireProxyObject(g_psw, pproxy, false);  // 0x1CB250(p, 0): a free row (score >= 2) only
    if (!po || !FCidDerivedFrom(po->Cid(), 0x30)) return;
    SetProjectileFilter(static_cast<ALO*>(po), grf);
    LO* plaunch = PPart<LO>(palo, 0x16E4);
    if (!plaunch) return;
    SetLauncherAim(plaunch, nullptr, posAim);
    LaunchFrom(plaunch, po);  // 0x1D6B00
    SetAloAngVelocityTo(static_cast<ALO*>(po), glm::vec3(0.0f));  // vt slot 36 (D_002A2DD0)
    ExpireProjectileIn(static_cast<ALO*>(po), 4.0f);              // 0x13B5C8
    SetBentleyState2(p, 1);                                       // 0x1362F8
    p->t1700 = g_clock.t;
    if (std::getenv("SLY2N_GANGDEBUG"))
        std::printf("bentley: dart %d (cid 0x%X, kind %d, proxy %d) fired at (%.0f %.0f %.0f) from (%.0f %.0f %.0f) t %.3f\n", po->oid, po->Cid(), nDart, pproxy->oid, posAim.x, posAim.y, posAim.z, a.x, a.y, a.z, g_clock.t);
}
namespace
{

// orig: 0x00136338 (the second state machine, the bombs: run until it stays)
void UpdateBentleyState2(BENTLEY* p)
{
    int s = p->n16BC;
    for (int iter = 0; iter < 32; ++iter)
    {
        if (s == -1)
        {
            if (static_cast<unsigned>(p->nF04) < 3 && p->nF20 != 2)
                SetBentleyState2(p, 2.0f < g_clock.t - p->t1700 ? 0 : 1);
            switch (p->state)
            {
            case 0:
                UpdateBentleySub(p, 0);
                if (p->f181C && p->sub.state != -1 && p->psma && p->psmIdle && IsmsFindSm(p->psmIdle, 0x17B) >= 0)
                    WarnOnce("0x1F09D8 (Bentley's idle 0x17B)");
                break;
            case 1: case 2: case 3: case 7: UpdateBentleySub(p, 1); break;
            case 5: UpdateBentleySub(p, 2); break;
            default: UpdateBentleySub(p, -1); break;
            }
        }
        else if (s >= 0 && s < 2)
        {
            if (!(static_cast<unsigned>(p->nF04) < 3)) SetBentleyState2(p, -1);
            else if (p->n16BC == 1)
            {
                if (2.0f < g_clock.t - p->t1700) SetBentleyState2(p, 0);
            }
            else if (FBtnTake(p, BTN_R1, 0.25f))
                FireBentleyDart(p, 0x80000);  // 0x1357C0
        }
        const int s2 = p->n16BC;
        if (s == s2) break;
        s = s2;
    }
}

}  // namespace
void SetBentleyHold(BENTLEY* p, int n);
void ReleaseBentleyCarried(BENTLEY* p);
namespace
{
// orig: 0x00136118 (state 6's transitions: the summon's end, the carried object's let go (the button released, or
// it touched something), else held / falling)
void UpdateBentleyHold(BENTLEY* p)
{
    for (int iter = 0; iter < 8; ++iter)
    {
        const int s = p->n16B0;
        if (s == 0)
        {
            if (FAsegaRetracted(p->pasega)) SetBentleyHold(p, 1);  // 0x131968
        }
        else if (s == 1 || s == 2)
        {
            CARRY* pc = PcarryFromLo(p->ploCarried);
            if (pc && pc->n158 == 2)
            {
                LandCarry(p->ploCarried, pc->plo1A0);  // 0x224258
                ReleaseBentleyCarried(p);
                SetJumpKind(&p->jump, 0xB, 0, 0);
            }
            else if (!pc || pc->n158 == 4 ||
                     !(GrfbtnHeld(p) & (GrfAbilityButton(p, p->pabil16B4 ? p->pabil16B4->id : -1) | BTN_TRIANGLE)))  // 0x1C4728 (0x141EA8(ability) | 0x10)
            {
                if (p->ploCarried) SetCarryState(p->ploCarried, 1);  // vt slot 76 (1)
                ReleaseBentleyCarried(p);                           // 0x136298
                SetJumpKind(&p->jump, 0xB, 0, 0);                   // 0x195A00 (11)
            }
            else if (0.1f < g_clock.t - p->tFloor) SetBentleyHold(p, 2);
            else SetBentleyHold(p, 1);
        }
        if (p->n16B0 == s || p->state != 6) break;
    }
}

// orig: 0x00134850 (the state machine, run until the state stays)
void UpdateBentleyTransitions(BENTLEY* p)
{
    int s = p->state;
    if (static const char* s_sz = std::getenv("SLY2N_BENTLEYSTATE"); s_sz && g_iframeHarness >= std::atoi(s_sz))
        if (PADCTL* pc = PpadctlOf(p))
            std::printf("bentley state: frame %d t %.3f state %d hold %d u %.3f rad %.3f vel %.2f %.2f held 0x%X forced 0x%X\n", g_iframeHarness, g_clock.t,
                        p->state, p->n16B0, pc->uJoy, pc->radJoy, VelAloPos(p->palo).x, VelAloPos(p->palo).y, pc->grfbtn,
                        PpodataFromLo(p->palo) ? PpodataFromLo(p->palo)->grfbtnForced : 0u);
    for (int iter = 0; iter < 32; ++iter)
    {
        bool fTimed = true;
        if (static_cast<unsigned>(s - 2) < 13)
        {
            if (s == 3) fTimed = p->n4C8 != 1 && p->n4C8 != 12;
            else fTimed = s == 5 || s == 7 || s == 12;
        }
        if (fTimed && 0.1f < g_clock.t - p->tFloor)
        {
            if (std::getenv("SLY2N_GANGDEBUG")) std::printf("bentley: off the floor: t %.4f tFloor %.4f\n", g_clock.t, p->tFloor);
            SetJumpKind(&p->jump, 0, 0, 0);
        }
        auto tryInteract = [&]() {
            if (std::getenv("SLY2N_GANGDEBUG") && FBtnWithin(p, BTN_TRIANGLE, F32(0x3E19999A)))
                std::printf("bentley: triangle t %.3f state %d ability %d can %d\n", g_clock.t, p->state, p->n1870,
                            int(FBentleyCanDo(p, p->n1870)));
            if (FBtnWithin(p, BTN_TRIANGLE, F32(0x3E19999A)) && FBentleyCanDo(p, p->n1870))
            {
                ForgetBtn(p, BTN_TRIANGLE);
                p->pabil16B4 = PabilFind(p, p->n1870);  // 0x136A28
                SetBentleyState(p, 6);
                return true;
            }
            return false;
        };
        switch (p->state)
        {
        case 0: case 1: case 7:
            if (!(GrfbtnHeld(p) & BTN_R1) && FActorObjectAhead(p, 0)) SetBentleyState(p, 5);
            else if (FBtnTake(p, BTN_CROSS, 0.1f)) SetJumpKind(&p->jump, 1, 0, 0);
            else if (FBtnWithin(p, BTN_R3, F32(0x3E19999A)) && FBentleyCanDo(p, 3))
            {
                ForgetBtn(p, BTN_R3);
                OpenBinoc(p, 0, nullptr, nullptr);  // 0x19CC50 (the recon view)
            }
            else if (tryInteract()) {}
            else if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD))) FInteractCircle(p);  // 0x1938C0 (+0x4C0)
            else
            {
                const float u = UJoy(p);
                const bool fMove = p->state == 0 ? 0.1f < u : 0.0f < u;
                if (fMove) SetBentleyState(p, (GrfbtnHeld(p) & BTN_R1) ? 1 : 7);
                else SetBentleyState(p, 0);
            }
            break;
        case 2:
            UpdateJumpTransitions(&p->jump);  // 0x197500
            tryInteract();
            break;
        case 5:
            // orig: 0x00134850 case 5: R1 or nothing ahead (wide) stands; else cross jumps, R3 the binocucom, circle
            // the interaction, triangle the gadget
            if ((GrfbtnHeld(p) & BTN_R1) || !FActorObjectAhead(p, 1)) SetBentleyState(p, 0);
            else if (FBtnTake(p, BTN_CROSS, 0.1f)) SetJumpKind(&p->jump, 1, 0, 0);
            else if (FBtnWithin(p, BTN_R3, F32(0x3E19999A)) && FBentleyCanDo(p, 3))
            {
                ForgetBtn(p, BTN_R3);
                OpenBinoc(p, 0, nullptr, nullptr);
            }
            else if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD))) FInteractCircle(p);  // 0x1938C0 (+0x4C0, 0xF8FE)
            else tryInteract();
            break;
        case 14:
            if (2.0f < g_clock.t - p->tState) SetBentleyState(p, 0);
            break;
        case 4: UpdateDamageTransitions(p); break;  // 0x1A12F0 (+0xDF0)
        case 3: UpdateInteractTransitions(p); break;  // 0x1911A8 (+0x4C0)
        case 6: UpdateBentleyHold(p); break;  // 0x136118
        case 12: UpdateJtThrow(p); break;  // 0x21F418 (+0x11B0)
        case 8:
            // orig: 0x001367D8 (the carried controller: thrown (1) and on a floor again (+0x208): standing, the player)
            if (const char* sz = std::getenv("SLY2N_THROWPRINT"); sz && *sz == '2')
                std::printf("bentley carried state %d floor %p t %.4f\n", p->n1800, static_cast<void*>(p->pxpFloor), g_clock.t);
            if (p->n1800 == 1 && p->pxpFloor)
            {
                SetBentleyState(p, 0);
                EnablePo(g_psw, p->palo, 1);  // vt slot 79 (1)
            }
            break;
        case 9: case 11: break;
        case 10:
            // a script's animation (+0x1030) or state machine (+0x1034) over: standing (0x134850)
            if (p->pasega ? FAsegaRetracted(p->pasega) : (p->psma && FSmaRetracted(p->psma))) SetBentleyState(p, 0);
            break;
        default:
            WarnOnce("Bentley's transitions of state " + std::to_string(p->state));
            break;
        }
        UpdateBentleyState2(p);
        if (p->state == s) break;
        s = p->state;
    }
}
}  // namespace

// orig: 0x00136040 (state 6's part: 0 the summon (+0x14E4), 1 held (+0x15F0, the root blend), 2 falling (+0x14E8, else
// +0x15D4))
void SetBentleyHold(BENTLEY* p, int n)
{
    if (n == p->n16B0) return;
    p->n16B0 = n;
    ASEG* paseg = nullptr;
    int grfapl = 1;
    switch (n)
    {
    case 0: paseg = PPart<ASEG>(p->palo, 0x14E4); p->vecVel = glm::vec3(0.0f); break;
    case 1: paseg = p->rb14F0.pbl; grfapl = 9; break;
    case 2:
        paseg = PPart<ASEG>(p->palo, 0x14E8);
        if (!paseg) paseg = p->rb14F0.apaseg[7];  // (+0x15D4)
        grfapl = 9;
        break;
    default: break;
    }
    ClearSmaHandle(&p->psma);  // 0x1F15B8
    ApplyAsegCur(paseg, p->palo, 0.0f, 1.0f, grfapl, &p->pasega);  // 0x1305B8
    UpdateActorStepMesh(p);  // slot 90 (0x135D80)
}

void OnBentleyMessage(LO* plo, int msgk, void* pv);
// orig: 0x0018D698 (an ability's thrown object +8: the old one forgotten (0x133448), the new one listened to (0x1333C0)
// so its message 1 (out of the world, 0x1A5848) reaches 0x135AD8, which forgets it again)
void SetAbilityCarried(BENTLEY* p, ABILITY* pe, LO* po)
{
    if (pe->plo8)
    {
        RemoveLoListener(pe->plo8, p);  // 0x133448
        pe->plo8 = nullptr;
    }
    if (po)
    {
        pe->plo8 = po;
        LO* plo = p->palo;
        AddLoListener(po, p, [plo](int msgk, void* pv) { OnBentleyMessage(plo, msgk, pv); });  // 0x1333C0
    }
}

// orig: 0x00136298 (the carried object let go: its carrier noted, the row unlocked)
void ReleaseBentleyCarried(BENTLEY* p)
{
    if (CARRY* pc = PcarryFromLo(p->ploCarried); pc && (pc->kind == 7 || pc->kind == 11))  // 0x224610
    {
        pc->plo16C = p->palo;
        if (p->pabil16B4) SetAbilityCarried(p, p->pabil16B4, p->ploCarried);  // 0x18D698
    }
    if (p->pabil16B4 && p->pabil16B4->plo4 && p->ploCarried) UnlockProxyRowOf(g_psw, p->pabil16B4->plo4, p->ploCarried);  // 0x1CB4B8
    p->ploCarried = nullptr;
    p->pabil16B4 = nullptr;
}

// orig: 0x0022FF40 (a complete matrix from one record's frame into another's; null: the world)
glm::mat4 InverseAffineMatrix4(const glm::mat4& m);  // xfmath.h: orig 0x11C318
glm::mat4 MatConvertXfs4(XF* from, XF* to, const glm::mat4& in)
{
    const glm::mat4 m = from ? from->matWorld * in : in;  // (MulMatrix4 0x11C168: glm A * B)
    return to ? InverseAffineMatrix4(to->matWorld) * m : m;
}

// orig: 0x00136530 (Bentley's carried controller +0x1800: -1 none, 0 held on Murray's holding frame (+0x1910's peer
// +0x191C), 1 thrown; its clips +0x1808 / +0x180C)
void SetBentleyCarried(BENTLEY* p, int k)
{
    if (k == p->n1800) return;
    ALO* palo = p->palo;
    switch (p->n1800)
    {
    case 0:
    {
        const int c = p->safe.c;  // (+0x9E4 kept over the parent change)
        SetAloParent(palo, nullptr, nullptr, 0xF);  // 0x1A58D0(p, 0, 0xF)
        p->safe.c = c;
        DeleteCtrl(&p->actrl1810[0]);  // 0x12E460
        DeleteCtrl(&p->actrl1810[1]);
        SetXfModes(p, 2);  // 0x2303A0 (+0x58 / +0x5C, 2)
        break;
    }
    case 1:
        SetStepPhysHeading(palo, RadStepHeading(p));  // 0x204660(0x204740)
        p->nMotion = 0;                               // +0x1A4
        TurnActorNow(p);                              // 0x204CA8
        break;
    default: break;
    }
    p->n1800 = k;
    p->t1804 = g_clock.t;
    if (std::getenv("SLY2N_THROWPRINT")) std::printf("bentley carried %d t %.4f\n", k, g_clock.t);
    ASEG* paseg = k >= 0 ? PPart<ASEG>(palo, 0x1808 + 4 * k) : nullptr;
    switch (k)
    {
    case -1:
    {
        // the toss abandoned: Murray's controller back to its hold unless it is throwing
        MURRAY* pm = PmurrayGlobal();
        if (!pm || pm->state != 15 || pm->jtthrow1910.state == 7) return;
        SetJtThrowState(pm->jtthrow1910, 0);
        return;
    }
    case 0:
    {
        // on Murray's holding frame: its vt slot 32 (its matrix in its record's frame: a LOX's own (0x231550), an
        // ALO's identity (0x1F3FB0)) and that record, converted into the position record's parent frame
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
        m = MatConvertXfs4(pxfRoot ? pxfRoot->pxfRef : nullptr, pxfPos ? pxfPos->pxfRef : nullptr, m);  // 0x22FF40
        const int c = p->safe.c;
        if (pm) SetAloParent(palo, pm->palo, pxfOn, 3);  // vt slot 10 (Murray, the frame's record, 3)
        p->safe.c = c;
        SetAloVelocityTo(palo, glm::vec3(0.0f));     // vt slot 35
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));  // vt slot 36
        if (pxfPos) p->actrl1810[0] = PctrlNewPos(palo, pxfPos, 1, 7, 2, glm::vec4(glm::vec3(m[3]), 1.0f));  // 0x12DC18
        if (pxfRot) p->actrl1810[1] = PctrlNewPos(palo, pxfRot, 1, 7, 2, VecXfValue(pxfRot, m));      // 0x11BFF0
        if (std::getenv("SLY2N_THROWPRINT"))
            std::printf("bentley carried: peer %d cid 0x%X record %p at %.1f %.1f %.1f\n", peer ? peer->oid : -1,
                        peer ? peer->Cid() : 0, static_cast<void*>(pxfOn), m[3].x, m[3].y, m[3].z);
        break;
    }
    case 1: p->nMotion = -1; break;
    }
    ClearSmaHandle(&p->psma);                                 // 0x1F15B8
    ApplyAsegCur(paseg, palo, 0.0f, 1.0f, 9, &p->pasega);      // 0x1305B8
    UpdateActorStepMesh(p);                                   // vt slot 90
}

// orig: 0x00135248
void SetBentleyState(BENTLEY* p, int n)
{
    const int cur = p->state;
    if (n == cur) return;
    // leave the old state
    switch (cur)
    {
    case -1: break;
    case 0:
        SetXfModes(p, 2);
        ActorNextLookTime(p);  // 0x142880
        break;
    case 9: case 10: case 11: SetXfModes(p, 2); break;
    case 1: case 7: p->g1A0 = UGaitPhase(p, p->g1A0); break;
    case 6:
        if (p->ploCarried)
        {
            SetCarryState(p->ploCarried, 1);  // vt slot 76 (1)
            ReleaseBentleyCarried(p);         // 0x136298
        }
        SetBentleyHold(p, -1);
        break;
    case 2:
        if (!p->ploWater)
        {
            LO* po = nullptr;
            if (n == 0 || n == 1) po = PloPart(p->palo, 0x16D0);
            else if (n == 3) po = (p->n4C8 == 1) ? nullptr : (p->n4C8 == 3 || p->n4C8 == 4) ? PloPart(p->palo, 0x16D4) : PloPart(p->palo, 0x16D0);
            LandingBurst(po);
        }
        SetJumpAnimState(&p->jump, -1, nullptr);  // 0x195FB8
        break;
    case 4: SetDamageState(p, -1); break;  // 0x1A0B40 (+0xDF0)
    case 3: SetInteractState(p, -1); break;  // 0x192BD8 (+0x4C0)
    case 12: SetJtThrowState(p, -1); break;  // 0x2206C8 (+0x11B0)
    case 8:
        SetBentleyCarried(p, -1);       // 0x136530
        EnablePo(g_psw, p->palo, 1);    // vt slot 79 (1): the player again
        break;
    default: WarnOnce("Bentley's SetState: leaving state " + std::to_string(cur)); break;
    }
    const int old = p->state;
    p->state = n;
    p->tState = g_clock.t;
    if (std::getenv("SLY2N_GADGETPRINT")) std::printf("bentley: state %d -> %d t %.3f throw %d\n", old, n, g_clock.t, p->jtthrow.state);
    ASEG* paseg = nullptr;
    SM* psm = nullptr;
    int grfapl = 1;
    OID oid = -1;
    uint32_t grfMask = 4;
    float t = 0.0f, svt = 1.0f;
    switch (n)
    {
    case 0:
        SetXfModes(p, 1);
        if (p->f181C)
        {
            paseg = p->paseg1820;
            grfapl = 9;
        }
        else
        {
            psm = p->psmIdle;
            oid = old == 2 ? 0x178 : old != 7 ? 0x175 : 0x176;
            if (!psm || IsmsFindSm(psm, oid) < 0) oid = -1;  // 0x1F0400
        }
        p->vecVel = glm::vec3(0.0f);
        grfMask = GrfActorLookMask(p);  // 0x1430C8
        ActorSetLookFlag(p, 1);         // 0x142868
        break;
    case 1: case 7:
    {
        GAIT* pgait = n == 1 ? &p->gaitWalk : &p->gaitRun;
        paseg = pgait->pbl;
        grfapl = 9;
        t = p->g1A0 * GDuration(paseg);
        ResetGaitFeet(pgait);  // 0x20BD68
        break;
    }
    case 2: break;  // (the jump's animation: 0x195FB8 from 0x195A00)
    case 6: SetBentleyHold(p, 0); return;  // 0x136040
    case 9: case 10: case 11:
        if (n == 9) psm = p->psm102C;
        SetXfModes(p, 1);
        p->vecVel = glm::vec3(0.0f);
        break;
    case 14:
        paseg = p->paseg1878;
        if (!paseg)
        {
            paseg = p->gaitWalk.apaseg[1];  // (+0x1374)
            svt = 1.5f;
        }
        grfapl = 9;
        break;
    case 4:
        SetBinocState(p, -1);  // 0x19CD38 (+0xF00)
        break;
    case 3: UpdateActorStepMesh(p); return;  // (the interaction's animation: 0x192BD8 after this)
    case 12: return;  // (the throw controller's animations: 0x2206C8 after this)
    case 8: SetBentleyCarried(p, 0); return;  // 0x136530 (held by Murray's +0x1910)
    case 5:
        // against an object ahead (0x140AF8, R1 not held): the push blend +0x14E0 (the root blend +0x13E0), looping
        paseg = p->rb13E0.pbl;
        grfapl = 9;
        break;
    default: WarnOnce("Bentley's SetState: entering state " + std::to_string(n)); return;
    }
    SetAnim(p, paseg, t, svt, grfapl, psm, oid, grfMask, false);
    UpdateActorStepMesh(p);  // slot 90 (0x135D80 -> 0x203D00: the step's second mesh)
}

namespace
{
// orig: 0x001A0450 (the root blend's weights by the velocity +0x190's direction: each entry's share of it along its
// own root velocity, the rest to the standing entry; +0x190 then the blend's velocity at the ASEGA's speed; +0x1038 the
// side of the entries 6 / 7)
void SteerRootBlend(ACTOR* p, ROOTBLEND* prb)
{
    if (!prb->pbl || !p->pasega) return;
    const float rate = p->pasega->svt;
    const bool f6 = prb->apaseg[6] != nullptr, f7 = prb->apaseg[7] != nullptr;
    if (!f6 || !f7) p->n1038 = -1;
    else if (p->n1038 == -1) p->n1038 = glm::dot(p->vecVel, glm::vec3(prb->avel[0])) < 0.0f ? 1 : 0;
    float aw[8] = {};
    int ai[5], c = 0;
    switch (p->n1038)
    {
    case -1:
        if (!f7) { ai[0] = 6; ai[1] = 2; ai[2] = 4; }
        else { ai[0] = 7; ai[1] = 3; ai[2] = 5; }
        ai[3] = 0; ai[4] = 1; c = 5;
        break;
    case 0: ai[0] = 6; ai[1] = 2; ai[2] = 4; ai[3] = 0; c = 4; break;
    default: ai[0] = 7; ai[1] = 3; ai[2] = 5; ai[3] = 1; c = 4; break;
    }
    float sum = 0.0f;
    if (0.0f < rate)
        for (int i = 1; i < c; ++i)
        {
            const glm::vec3 d(prb->avel[ai[i]]);
            const float den = rate * glm::dot(d, d);
            const float w = den != 0.0f ? std::max(glm::dot(p->vecVel, d) / den, 0.0f) : 0.0f;
            aw[ai[i]] = w;
            sum += w;
        }
    if (sum < 1.0f) aw[ai[0]] = 1.0f - sum;
    else
    {
        aw[ai[0]] = 0.0f;
        for (int i = 1; i < c; ++i) aw[ai[i]] /= sum;
    }
    glm::vec3 total(0.0f);
    for (int i = 0; i < 8; ++i) total += glm::vec3(prb->avel[i]) * aw[i];
    p->vecVel = total * rate;
    SetBlendWeights(prb->pbl, std::vector<float>(aw, aw + 8));  // 0x1384C0
}

// orig: 0x00134DA8's state 6 part (held, 16B0 1): the turn and the speed by the stick (D_002BC940 / 950 / 960), the
// velocity from the root blend +0x14F0
void UpdateBentleyHoldMotion(BENTLEY* p)
{
    auto quad = [](float c0, float c1, float c2, float g) {
        const float f = c0 + g * (c1 + g * c2);
        return f < 0.0f ? 0.0f : (1.0f < f ? 1.0f : f);
    };
    const float u = UJoy(p);
    const float speed = quad(-1.0f, 2.0f, 0.0f, u);
    const float turn = quad(1.0f, 0.0f, 0.0f, u);
    if (0.0f < u)
    {
        const float d = cmm::RadNormalize(RadJoy(p) - p->radHeading);
        const float lim = turn * 5.0f * g_clock.dt;
        SetStepPhysHeading(p->palo, p->radHeading + cmm::GLimitAbs(d, lim));  // 0x204660
    }
    const glm::vec4& v1 = p->rb14F0.avel[1];  // (+0x1520)
    const float len = std::sqrt(v1.x * v1.x + v1.y * v1.y);
    const float f = speed * len * (p->pasega ? p->pasega->svt : 1.0f);
    const glm::vec3 v = cmm::SetVectorCylind(RadJoy(p) + 3.1415927f, f, 0.0f);  // 0x11E1A0
    p->n1038 = -1;
    p->vecVel = glm::transpose(p->matHeading) * v;  // (the rows +0x1B0..)
    SteerRootBlend(p, &p->rb14F0);  // 0x1A0450
}

// orig: 0x00134D30 (the +0xF00 sub-object while it is busy)
void UpdateBentleyF00(BENTLEY* p)
{
    UpdateActorBinoc(p, 0x4F0);  // (binoc.cpp)
}

// orig: 0x00204E38 as the gang's states 10 / 11 (Murray 17 / 18) use it: the actor's animation's root velocity
// (channel kind 1) at its ASEGA's time, reversed into +0x190
glm::vec3 VelActorAnimRoot(ACTOR* p)
{
    glm::vec4 v(0.0f);
    if (ASEGA* pasega = p->pasega; pasega && !FAsegaRetracted(pasega))
        if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
            pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
    return -glm::vec3(v);
}

// the gang's script-animation states' update (Bentley 0x134DA8's 10 / 11, Murray 0x1B50B0's 17 / 18): the
// animation's root motion; the second (fSteered) also by the stick: its speed the deflection (0x131A00), its heading
// the stick's (0x204660)
void UpdateGangScriptAnim(ACTOR* p, bool fSteered)
{
    p->vecVel = VelActorAnimRoot(p);
    if (!fSteered) return;
    if (p->pasega) SetAsegaSpeed(p->pasega, UJoy(p));
    SetStepPhysHeading(p->palo, RadJoy(p));
}

// orig: 0x00134DA8 (vt slot 16)
void UpdateBentley(LO* plo)
{
    BENTLEY* p = PbentleyFromLo(plo);
    if (!p) return;
    // Debug observer at the original UpdateBentley entry, for same-input pose/dialog comparisons.
    // SLY2N_BENTLEYPRINT=from-to selects game time; SLY2N_BENTLEYPOSE includes the local record matrices.
    const char* szBentleyPrint = std::getenv("SLY2N_BENTLEYPRINT");
    float tPrintFrom = -1.0f, tPrintTo = 1.0e30f;
    if (szBentleyPrint) std::sscanf(szBentleyPrint, "%f-%f", &tPrintFrom, &tPrintTo);
    if (szBentleyPrint && tPrintFrom <= g_clock.t && g_clock.t <= tPrintTo)
    {
        const glm::vec3 pos = PosAloCenter(p->palo), vel = VelAloPos(p->palo);
        const CSndVoice* voice = static_cast<CSndVoice*>(g_sound.sndDialog.get());
        std::fprintf(stderr, "bentley: frame %d t %.9g state %d pos %.9g %.9g %.9g vel %.9g %.9g %.9g anim %d at %.9g dialog %d play %.9g clock %.9g pcm %.9g handle %u busy %u paused %d\n",
                     g_iframeHarness, g_clock.t, p->state, pos.x, pos.y, pos.z, vel.x, vel.y, vel.z,
                     p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -1, p->pasega ? p->pasega->t : 0.0f,
                     voice && voice->m_pstream ? voice->m_pstream->id : -1, voice ? voice->m_tPlay : 0.0, g_sound.tDialog,
                     voice ? SpiopTrackSeconds(voice->m_h) : 0.0, voice ? voice->m_h : 0u,
                     voice ? static_cast<unsigned>(voice->m_cbusy) : 0u, voice ? voice->m_cpause : 0);
        if (std::getenv("SLY2N_BENTLEYFLOOR"))
        {
            const XP* pxp = StepPhysOf(p->palo).pxpFloor;
            std::fprintf(stderr, "bentley-floor: frame %d %d %d\n", g_iframeHarness,
                         pxp && pxp->axpd[0].palo ? pxp->axpd[0].palo->oid : -1, pxp && pxp->axpd[1].palo ? pxp->axpd[1].palo->oid : -1);
        }
        if (std::getenv("SLY2N_BENTLEYPOSE"))
            for (size_t i = 0; i < p->palo->axf.size(); ++i)
            {
                const XF& xf = *p->palo->axf[i];
                std::fprintf(stderr, "bentley-xf: frame %d index %zu kind %d tag %d local", g_iframeHarness, i, xf.kind, xf.tag);
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 3; ++r) std::fprintf(stderr, " %.9g", xf.mat[c][r]);
                std::fprintf(stderr, "\n");
            }
    }
    // Read-only gameplay observation for repeated bomb acquisition/release.
    if (std::getenv("SLY2N_BOMBPRINT") && g_psw)
    {
        std::fprintf(stderr, "BOMB_FRAME {\"frame\":%d,\"time\":%.9g,\"state\":%d,\"hold\":%d,\"health\":%d,\"bpos\":[%.1f,%.1f,%.1f],\"pools\":[",
                     g_iframeHarness, g_clock.t, p->state, p->n16B0, p->dmg.pcount ? *p->dmg.pcount : -1,
                     PosLoWorld(p->palo).x, PosLoWorld(p->palo).y, PosLoWorld(p->palo).z);
        bool first = true;
        for (const ABILITY& ability : p->aabil)
        {
            const auto found = g_psw->mpproxy.find(ability.plo4);
            if (found == g_psw->mpproxy.end() || found->second.iwe < 0 ||
                found->second.iwe >= static_cast<int>(g_psw->awe.size())) continue;
            const WE& group = g_psw->awe[found->second.iwe];
            if (group.cloLoaded != 1) continue;
            if (!first) std::fputc(',', stderr);
            first = false;
            std::fprintf(stderr, "{\"ability\":%d,\"rows\":[", ability.id);
            for (size_t row = 0; row < group.aplo.size(); ++row)
            {
                const LO* object = group.aplo[row];
                const CARRY* carry = PcarryFind(object);
                if (row) std::fputc(',', stderr);
                const glm::vec3 posBomb = PosLoWorld(const_cast<LO*>(object));
                std::fprintf(stderr, "{\"row\":%zu,\"active\":%d,\"held\":%d,\"locked\":%d,\"state\":%d,\"contact\":%d,\"pos\":[%.1f,%.1f,%.1f]}",
                             row, FLoActive(object), object == p->ploCarried,
                             row < group.afRowLocked.size() ? group.afRowLocked[row] : 0,
                             carry ? carry->state : -1, carry ? carry->n158 : -1, posBomb.x, posBomb.y, posBomb.z);
            }
            std::fputs("]}", stderr);
        }
        std::fputs("]}\n", stderr);
    }
    // (debug: SLY2N_CHARMAT=N: at Bentley's N-th update his health 10, power 30 and three charms (0x5D) thrown around
    // him: the health pickup's test)
    static const char* s_szCharm = std::getenv("SLY2N_CHARMAT");
    if (s_szCharm && g_psw)
    {
        static int s_c = 0;
        if (s_c++ == std::atoi(s_szCharm))
        {
            if (int* pc = DmgCounterOf(p)) *pc = 10;
            SetActorPower(p, 30);
            SpawnPickups(0x5D, PosLoWorld(p->palo), p->palo, 3);
        }
    }
    // (debug, the actors code: SLY2N_BTELEGUARD="OID@N[,D]" puts Bentley D (default 600) in front of the first guard
    // OID in the world at his N-th update, facing it: a dart's target)
    static const char* s_szGuard = std::getenv("SLY2N_BTELEGUARD");
    if (s_szGuard && g_psw)
    {
        static int s_c = 0;
        int oid = 0, n = 0;
        float d = 600.0f;
        const int c = std::sscanf(s_szGuard, "%d@%d,%f", &oid, &n, &d);
        if (c >= 2 && s_c++ == n)
            for (const auto& up : g_psw->aploAll)
            {
                LO* pg = up.get();
                if (pg->oid != oid || !pg->fInWorld || !FCidDerivedFrom(pg->Cid(), 0x30)) continue;
                ALO* pag = static_cast<ALO*>(pg);
                const glm::mat4 m = MatWorldOfAlo(pag);
                const glm::vec3 fwd = glm::normalize(glm::vec3(m[0].x, m[0].y, 0.0f));
                const glm::vec3 pos = PosAloCenter(pag) + fwd * d;
                SetStepPos(p, pos - glm::vec3(p->vecBase));
                const float h = std::atan2(-fwd.y, -fwd.x);
                glm::mat3 r(1.0f);
                r[0] = glm::vec3(std::cos(h), std::sin(h), 0.0f);
                r[1] = glm::vec3(-std::sin(h), std::cos(h), 0.0f);
                SetStepRotation(p, r);
                SetAloVelocityTo(p->palo, glm::vec3(0.0f));
                CutCm(&g_cm);
                std::printf("btele: Bentley before %d at (%.0f %.0f %.0f)\n", oid, pos.x, pos.y, pos.z);
                break;
            }
    }
    // (debug: SLY2N_BTELEPORT="X Y Z@N[,H|,k]" puts Bentley there at his N-th update, heading H (",k": kept); several separated by ';',
    // "X Y Z@tT[,H]" at his first update at game time >= T)
    if (static const char* s_szBTele = std::getenv("SLY2N_BTELEPORT"); s_szBTele)
    {
        static int s_cB = 0;
        static std::vector<bool> s_afDone;
        const int cB = s_cB++;
        int iev = 0;
        for (const char* q = s_szBTele; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr, ++iev)
        {
            const std::string ev(q, std::strchr(q, ';') ? std::strchr(q, ';') : q + std::strlen(q));
            if (s_afDone.size() <= size_t(iev)) s_afDone.resize(size_t(iev) + 1, false);
            float x = 0, y = 0, z = 0, h = 0, t = 0;
            int n = 0;
            const bool fTime = ev.find("@t") != std::string::npos;
            const int c = fTime ? std::sscanf(ev.c_str(), "%f %f %f@t%f,%f", &x, &y, &z, &t, &h)
                                : std::sscanf(ev.c_str(), "%f %f %f@%d,%f", &x, &y, &z, &n, &h);
            const size_t iw = ev.find("/w");  // ("/wN": only in the N-th world loaded through a transition)
            if (iw != std::string::npos && std::atoi(ev.c_str() + iw + 2) != g_cTransitionLoads) continue;
            if (c < 4 || s_afDone[size_t(iev)] || !(fTime ? t <= g_clock.t : cB == n)) continue;
            s_afDone[size_t(iev)] = true;
            SetStepPos(p, glm::vec3(x, y, z) - glm::vec3(p->vecBase));
            if (ev.find(",k") == std::string::npos)  // (",k": the heading kept, as the reference's teleport)
            {
                glm::mat3 r(1.0f);
                r[0] = glm::vec3(std::cos(h), std::sin(h), 0.0f);
                r[1] = glm::vec3(-std::sin(h), std::cos(h), 0.0f);
                SetStepRotation(p, r);
            }
            SetAloVelocityTo(p->palo, glm::vec3(0.0f));
            CutCm(&g_cm);
        }
    }
    ApplyRouteTeleportActor(p);
    // (debug: SLY2N_BTELEPORTT="X Y Z@T[,H];...": the same once the game clock passes each T)
    if (static const char* s_szBTeleT = std::getenv("SLY2N_BTELEPORTT"); s_szBTeleT)
    {
        struct BTELE { float x, y, z, t, h; bool fDone; };
        static std::vector<BTELE> s_a;
        static bool s_fParsed = false;
        if (!s_fParsed)
        {
            s_fParsed = true;
            for (const char* pch = s_szBTeleT; pch && *pch;)
            {
                BTELE b{0, 0, 0, 0, 0, false};
                if (std::sscanf(pch, "%f %f %f@%f,%f", &b.x, &b.y, &b.z, &b.t, &b.h) >= 4) s_a.push_back(b);
                pch = std::strchr(pch, ';');
                if (pch) ++pch;
            }
        }
        for (BTELE& b : s_a)
            if (!b.fDone && b.t <= g_clock.t)
            {
                b.fDone = true;
                SetStepPos(p, glm::vec3(b.x, b.y, b.z) - glm::vec3(p->vecBase));
                glm::mat3 r(1.0f);
                r[0] = glm::vec3(std::cos(b.h), std::sin(b.h), 0.0f);
                r[1] = glm::vec3(-std::sin(b.h), std::cos(b.h), 0.0f);
                SetStepRotation(p, r);
                SetAloVelocityTo(p->palo, glm::vec3(0.0f));
                CutCm(&g_cm);
                std::printf("btele: Bentley to (%.0f %.0f %.0f) at t %.3f\n", b.x, b.y, b.z, g_clock.t);
            }
    }
    UpdateActor(p);  // 0x1403D0
    if (!plo->fInWorld) return;
    // (debug: SLY2N_JTPRINTB=N: the interaction lists at Bentley's N-th update)
    if (static const char* s_szJtB = std::getenv("SLY2N_JTPRINTB"); s_szJtB)
    {
        static int s_cJtB = 0;
        if (++s_cJtB == std::atoi(s_szJtB)) PrintJtLists(g_psw);
    }
    UpdateJtCandidates(p, 0xF8FE);  // 0x194A40 (the interaction targets, jt.cpp)
    UpdateBentleyF00(p);
    if (PpadctlOf(p)) UpdateBentleyTransitions(p);
    switch (p->state)
    {
    case 0:
        if (p->f181C) SetStepPhysHeading(p->palo, 0.5f);  // (D_002BC934)
        else if (p->psma) SetSmaMask(p->psma, GrfActorLookMask(p));  // 0x1F1350
        break;
    case 1:
        SetStepPhysHeading(p->palo, RadJoy(p));                     // 0x204660
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitWalk, 1.0f));     // 0x20BB58
        UpdateGaitFeet(&p->gaitWalk);                               // 0x20BC08
        break;
    case 7:
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitRun, UJoy(p)));
        UpdateGaitFeet(&p->gaitRun);
        break;
    case 2: UpdateJumpInput(&p->jump); break;  // 0x197F98
    case 6: if (p->n16B0 == 1) UpdateBentleyHoldMotion(p); break;
    case 5:
    {
        // orig: 0x00134DA8 case 5: facing the object ahead (+0x10DC, 0x2046F0); the push blend's speed (+0x1410's xy
        // length) by the stick and the clip's rate, away from the stick's direction, through the root blend
        if (p->plo10DC) FaceActorToward(p, p->plo10DC);
        const glm::vec4& v1 = p->rb13E0.avel[1];  // (+0x1410)
        const float f = UJoy(p) * std::sqrt(v1.x * v1.x + v1.y * v1.y) * (p->pasega ? p->pasega->svt : 1.0f);
        p->n1038 = -1;
        p->vecVel = glm::transpose(p->matHeading) * cmm::SetVectorCylind(RadJoy(p) + 3.1415927f, f, 0.0f);
        SteerRootBlend(p, &p->rb13E0);  // 0x1A0450
        break;
    }
    case 3: UpdateInteract(p); break;  // 0x191A10 (+0x4C0)
    case 10: UpdateGangScriptAnim(p, false); break;
    case 11: UpdateGangScriptAnim(p, true); break;
    case 14:
        // orig: 0x00134DA8 case 14: retain the authored animation speed.
        p->vecVel = VelActorAnimRoot(p);
        SetStepPhysHeading(p->palo, RadJoy(p));
        break;
    case 4: case 8: case 9: case 12: case 13: break;
    default: WarnOnce("Bentley's update of state " + std::to_string(p->state)); break;
    }
    // 0x135EB0
    switch (p->state)
    {
    case 0: case 1: case 5: case 6: case 7: case 11: case 12: case 14: UpdateActorSupport(p); break;
    case 3: case 13: WarnOnce("0x135EB0's sub-objects (states 3 / 13)"); break;
    default: break;
    }
    UpdateDamage(p);  // 0x1A1358 (+0xDF0)
    CompareGangFrame(p, g_iframeHarness);  // (gangcheck.cpp)
}

// orig: 0x00135940 (vt slot 70)
void UpdateBentleyMotion(ALO* palo)
{
    BENTLEY* p = PbentleyFromLo(palo);
    if (!p) { UpdateStepPhysMotion(palo); return; }
    if (p->state == 13) ApplyAloGravity(palo);  // 0x1F9B20
    else UpdateStepPhysMotion(palo);            // 0x204108
    switch (p->state)
    {
    case 2: UpdateJumpMotion(&p->jump); break;  // 0x196DF8
    case 4: UpdateDamageMotion(p); break;  // 0x1A1148
    case 3: UpdateInteractMotion(p); break;  // 0x194170 (+0x4C0)
    case 13: WarnOnce("Bentley's motion of state " + std::to_string(p->state)); break;
    case 6: if (p->n16B0 == 2) WarnOnce("Bentley's motion of state 6"); break;
    default: break;
    }
}

// orig: 0x00135A48 (vt slot 55)
void OnBentleyXpNew(ALO* palo, XP* pxp, int i)
{
    BENTLEY* p = PbentleyFromLo(palo);
    const SOE* psoe = pxp->axpd[i].psoe;
    if (psoe && psoe->f48) return;
    if (p && p->state == 4) AdjustDamageXp(p, pxp);  // 0x1A2100
    else if (p && p->state == 13) WarnOnce("0x19E9C8 (+0xF50's contact)");
    else OnStepXpNew(palo, pxp, i);  // 0x204250
}

// orig: 0x00136CC0 (vt slot 85)
uint32_t GrfBentleyXp(ALO* palo)
{
    BENTLEY* p = PbentleyFromLo(palo);
    if (!p || p->fNoContacts) return 0;
    if (p->state == 13) return 0;
    if (p->state == 8) return 5;
    if (p->state == 14) return 0xF;
    return GrfActorStance(p);  // 0x143C78
}

// orig: 0x00135D08 (vt slot 89: the jump kind of the state, -1 none; slot 88's inverse)
int BentleyJumpKind(LO* plo)
{
    BENTLEY* p = PbentleyFromLo(plo);
    if (!p) return -1;
    static const int s_ak[14] = {0, -1, 1, 2, 3, -1, -1, -1, -1, 4, 5, 6, 7, 8};
    return static_cast<unsigned>(p->state) < 14 ? s_ak[p->state] : -1;
}

// orig: 0x00135C20 (vt slot 88: the state of the jump's kind n)
void SetBentleyJumpState(LO* plo, int n)
{
    static const int s_ak[9] = {0, 2, 3, 4, 9, 10, 11, 12, 13};
    if (BENTLEY* p = PbentleyFromLo(plo))
        if (static_cast<unsigned>(n) < 9) SetBentleyState(p, s_ak[n]);
}
}  // namespace

void HandleActorMessage(ACTOR* pa, int msg, void* pv);  // sly.cpp: orig 0x140F90
void FireBentleyDart(BENTLEY* p, uint32_t grf);

// orig: 0x00135AD8 (vt slot 5: the messages; 9 the events: 0x216 the summoned object attached to the hand)
void OnBentleyMessage(LO* plo, int msgk, void* pv)
{
    BENTLEY* p = PbentleyFromLo(plo);
    // the base's 0x140F90 first (ActorHandleMessage, sly.cpp: e.g. event 0x15E, the animation's velocity)
    if (p) HandleActorMessage(p, msgk, pv);
    if (p && msgk == 1)
    {
        // orig: 0x00135AD8 case 1: an ability's thrown object (+8) out of the world is forgotten (0x18D698)
        for (ABILITY& e : p->aabil)
            if (e.plo8 && e.plo8 == pv) SetAbilityCarried(p, &e, nullptr);
        return;
    }
    if (!p || msgk != 9 || !pv) return;
    const ASEGMSG9* pm = static_cast<const ASEGMSG9*>(pv);
    if (pm->pasega == p->pasega)
    {
        if (std::getenv("SLY2N_GANGDEBUG") && pm->n == 0x216)
            std::printf("bentley: summon event t %.3f ability %p id %d proxy %d\n", g_clock.t, (void*)p->pabil16B4,
                        p->pabil16B4 ? p->pabil16B4->id : -1, p->pabil16B4 && p->pabil16B4->plo4 ? p->pabil16B4->plo4->oid : -1);
        if (pm->n != 0x216 || !p->pabil16B4 || !p->pabil16B4->plo4) return;
        LO* po = PloAcquireProxyObject(g_psw, p->pabil16B4->plo4, false);  // 0x1CB250
        if (std::getenv("SLY2N_GANGDEBUG")) std::printf("bentley: summoned %d\n", po ? po->oid : -1);
        p->ploCarried = po;
        if (!po) return;
        LockProxyRowOf(g_psw, p->pabil16B4->plo4, po);  // 0x1CB488
        if (CARRY* pc = PcarryFromLo(po)) pc->kind = p->pabil16B4->id;  // +0x15C
        SetCarryState(po, 0);  // vt slot 76 (0)
    }
    else if (pm->pasega == p->pasega16CC)
    {
        // (+0x16CC: no code sets it (an ELF scan finds only this read; 0 in every reference RAM), so this is never
        // reached in the original either)
        if (pm->n == 0x243) FireBentleyDart(p, 0x80000);  // 0x1357C0
    }
    else if (pm->pasega == p->sub.pasega)  // 0x136ED0 checks its own ASEGA
    {
        // orig: 0x00136ED0 (the sub-object's events): 0x16E the swing's controller on the hand record demoted
        // (0x131B10) and remembered (+0x10), 0x242 the swing may chain (+0xC)
        if (pm->n == 0x16E)
        {
            if (p->pxf10B8) DemoteAsegaOnXf(pm->pasega, p->palo, p->pxf10B8);
            p->sub.n10 = 1;
        }
        else if (pm->n == 0x242) p->sub.nC = 1;
    }
}

// orig: 0x001358A8 (vt slot 50)
void GetBentleyCpdefi(BENTLEY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pc)
{
    if (a.state != 0 && a.state != 9 && a.nSub4CC != 0) a.t1080 = ctx.t;
    GetActorCmTarget(a, pose, ctx, pc);  // 0x141640
    switch (a.state)
    {
    case 13:  // 0x19EA00 (as Sly's state 8)
    {
        glm::vec3 dir(0.0f);
        if (a.pcrvF50) EvalCrvS(a.pcrvF50, a.sCrvF50, nullptr, &dir);
        pc->sAdjust = 0.0f;
        pc->vBase = glm::vec4(dir * a.gCrvF50, 0.0f);
        break;
    }
    case 5: pc->grfcpd |= 1; break;
    default: break;
    }
}

// ----------------------------------------------------------------------------------------------------------------
// Murray
// ----------------------------------------------------------------------------------------------------------------

namespace
{
// orig: 0x001B4728 (the init)
void InitMurray(MURRAY* p)
{
    InitActorFields(p);  // 0x13FF70
    p->g220 = 50.0f;
    p->n17F4 = -1;
    p->g1410 = 1500.0f;
    p->g1414 = 3500.0f;
    p->g103C = 60.0f;
    p->sHeight = 125.0f;
    p->n13D8 = p->n1810 = p->n1840 = p->state = p->n144C = p->n1450 = p->n1458 = -1;
    InitGait(&p->gaitWalk, p->palo, 0x15);  // 0x20B840
    InitGait(&p->gait1460, p->palo, 0x15);
    InitGait(&p->gait1630, p->palo, 0x15);
    InitGait(&p->gaitRun, p->palo, 0x15);
    // TODO(actors): 0x20BD78 (+0x1850), the reach +0x1098..+0x10B4
    InitJtThrow(p, p->jtthrow1910, 0x1910);  // 0x21F2E0 (+0x1910: the Bentley toss)
    p->t1434 = -60.0f;
    p->t1AC8 = -60.0f;
    p->n1000 = 4;
    // (G_PTARGET_C = p: found by class here)
}

// orig: 0x001B49E0 (the post-load)
void PostLoadMurray(MURRAY* p)
{
    ALO* palo = p->palo;
    PostLoadActorParts(p);  // 0x1400B8
    PostLoadActorMass(p);
    p->psmIdle = PPart<SM>(palo, 0x1028);
    p->paseg13E0 = PPart<ASEG>(palo, 0x13E0);
    p->paseg1AE8 = PPart<ASEG>(palo, 0x1AE8);
    p->plo13EC = PloPart(palo, 0x13EC);
    p->paseg140C = PPart<ASEG>(palo, 0x140C);
    p->paseg1AE0 = PPart<ASEG>(palo, 0x1AE0);
    p->psm1510 = PPart<SM>(palo, 0x1510);
    p->paseg1440 = PPart<ASEG>(palo, 0x1440);
    p->paseg1444 = PPart<ASEG>(palo, 0x1444);
    p->psm1520 = PPart<SM>(palo, 0x1520);
    for (int i = 0; i < 5; ++i) p->apasegPunch[i] = PPart<ASEG>(palo, 0x17FC + 4 * i);
    BindJumpParts(p);
    BindGaitParts(&p->gaitWalk, palo, 0x1320);  // 0x20B868
    BindGaitParts(&p->gait1460, palo, 0x1460);
    BindGaitParts(&p->gait1630, palo, 0x1630);
    BindGaitParts(&p->gaitRun, palo, 0x1540);
    BindRootBlendParts(&p->rb16F0, palo, 0x16F0);  // 0x1A0300
    p->psm16E0 = PPart<SM>(palo, 0x16E0);
    // TODO(actors): 0x20BD88 (+0x1850), the light +0x1ACC (0x12DC18)
    PostLoadJtThrow(p->jtthrow1910);  // 0x21F388 (+0x1910)
    AttachActorLook(p, 0x16);  // 0x1426E8
    SetMurrayState(p, 0);
}

// orig: 0x001B84E0
int FMurray1B84E0(MURRAY* p) { return (p->pasega190C || p->n1810 != -1) ? p->f1820 : 1; }

// orig: 0x001B98A0 (slot 99)
bool FMurrayCanDo(MURRAY* p, int n)
{
    if (static_cast<unsigned>(p->nF04) < 3) return false;  // 0x1443D8 (the binocucom)
    switch (p->state)
    {
    case 0: case 1: case 9: case 10: break;
    case 2: if (!FJumpCanDo(&p->jump, n)) return false; break;      // 0x19C8D0 (+0x2C0)
    case 13: if (!FInteractCanDo(p, n)) return false; break;        // 0x194558 (+0x4C0)
    case 20: WarnOnce("0x19EEF0 (+0xF50's slot 99, Murray)"); return false;
    case 5:
        if (n == 3) return true;
        // (19: the carried object's owner (+0x13F0 +0x180) derived from 0x38)
        if (n == 19) { LO* po = p->plo13F0 ? PloInteractOidField(p->plo13F0, 0x180) : nullptr; return po && FIsLoDerivedFrom(po, 0x38); }
        return false;
    default: return false;
    }
    switch (n)
    {
    case 3: return p->f12F0 && (g_grfjoyt & 2) && !FActiveCplcy(&g_cmpolicies.cpdefault);
    case 22: return p->jump.kind != 0 && p->jump.kind != 11 && p->jump.kind != -1;
    case 1: case 2: case 15: case 16: case 20: case 21: return true;
    default: return false;
    }
}

void SetMurrayPunch(MURRAY* p, int k);
// orig: 0x001B70A0 (the guard nearest (+0x10DC) launched into the air by Murray (its 0xB1 job's owner, state < 3)
// with the state bit 0x17: CIRCLE close above him (xy 300, height 100..500, it has a carry object +0xFEC) picks it up
// (the job's state 2, 0x1B7050: the carry with clip +0x1444); SQUARE (xy 400, height 0..600) the juggling punch 4)
bool FMurrayGrab(MURRAY* p)
{
    LO* ploGuard = p->plo10DC;
    if (!ploGuard) return false;
    GUARD* pg = PguardFromLo(ploGuard);
    if (!pg) return false;
    if (!(pg->grfState & (uint64_t(1) << 0x17)) || !FBtnWithin(p, BTN_CIRCLE | BTN_SQUARE, F32(0x3E19999A))) return false;
    auto* pj = dynamic_cast<JOBB1*>(PjobFind(pg, 0xB1));
    if (!pj || pj->stLaunch >= 3) return false;
    if (pj->ploOwner != p->palo) return false;
    const glm::vec3 d = PosAloCenter(static_cast<ALO*>(ploGuard)) - PosAloCenter(p->palo);
    const float sxy = std::sqrt(d.x * d.x + d.y * d.y);
    if (sxy < 300.0f && 100.0f <= d.z && d.z <= 500.0f && pg->ploFEC && FBtnTake(p, BTN_CIRCLE, F32(0x3E19999A)))
    {
        if (std::getenv("SLY2N_MURRAYPRINT"))
        {
            LO* po = PloInteractOidField(pg->ploFEC, 0x180);
            std::fprintf(stderr, "murray-grab: guard %d carry %d (cid 0x%X) owner %d\n", ploGuard->oid, pg->ploFEC->oid,
                         pg->ploFEC->Cid(), po ? po->oid : -1);
        }
        pj->SetState(2);  // 0x17B1A8
        // 0x1B7050: the take with the second clip
        if (p->plo13EC)
        {
            p->plo13F0 = pg->ploFEC;
            p->n144C = 1;
            SetMurrayState(p, 4);
            if (LO* po = PloInteractOidField(p->plo13F0, 0x180)) FaceActorToward(p, po);  // 0x2046F0
        }
        return true;
    }
    if (!(sxy < 400.0f && 0.0f <= d.z && d.z <= 600.0f && p->apasegPunch[4] && FBtnTake(p, BTN_SQUARE, F32(0x3E19999A))))
        return false;
    SetMurrayState(p, 0xB);
    SetMurrayPunch(p, 4);  // 0x1B86B0
    return true;
}

// orig: 0x001B86B0 (the punch k: its clip from the start, the stick's steering flags reset, where it ends)
void SetMurrayPunch(MURRAY* p, int k)
{
    if (k == p->n1810) return;
    p->n1810 = k;
    p->t1814 = g_clock.t;
    if (k == -1) return;
    p->f1820 = p->f1818 = p->f181C = 0;
    if (p->pasega190C) RetractAsegaPp(&p->pasega190C);  // 0x130658
    ASEG* paseg = p->apasegPunch[k];
    p->vecVel = glm::vec3(0.0f);
    // orig 0x1B86B0: align a selected target within0.4 radians, unless R1 is held. Preserve the
    // turn helper's angular-velocity update rather than snapping the body's rotation.
    if (p->plo10DC && p->n10E4 >= 2 && !(GrfbtnHeld(p) & 8))
    {
        const glm::vec3 d = PosJtLo(p->plo10DC) - PosAloCenter(p->palo);
        const float rad = std::atan2(d.y, d.x);
        if (std::fabs(cmm::RadNormalize(rad - p->radHeading)) < 0.4f)  // D_002C09F0
        {
            SetStepPhysHeading(p->palo, rad);
            TurnActorNow(p);  // 0x204CA8
        }
    }
    ClearSmaHandle(&p->psma);  // 0x1F15B8
    ApplyAsegCur(paseg, p->palo, 0.0f, 1.0f, 1, &p->pasega);  // 0x1305B8
    if (std::getenv("SLY2N_GANGDEBUG") && paseg)
        for (int l = 0; l < 3; ++l)
            for (const ASEGEV& ev : PasegdOf(paseg)->aaev[l])
                std::printf("punch %d clip %d list %d event t %.3f kind %d an %d %d\n", k, paseg->oid, l, ev.t, ev.kind, ev.an[0], ev.an[1]);
    float g = 0.0f;
    if (paseg)
        for (const ASEGEV& ev : PasegdOf(paseg)->aaev[1])
            if (ev.kind == 11 && ev.an[0] == 0x1AC) { g = ev.t; break; }  // 0x12F938
    if (g != 0.0f && p->pasega) SetAsegaSeekTime(p->pasega, g_clock.t + g / p->pasega->svt);  // 0x131A90
    if (p->pasega)
    {
        const glm::vec3 pos = PosAloCenter(p->palo);  // 0x1A62E8
        p->pos1830 = glm::vec4(pos, 1.0f);
        if (const ASEGCHN* pchn = PchnFindAseg(p->pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)  // 0x12FF10
        {
            glm::vec4 a(0.0f), b(0.0f);
            pchn->ptrkp->Eval(&a, nullptr, 0.0f, 0.0f);
            pchn->ptrkp->Eval(&b, nullptr, PasegdOf(p->pasega->paseg)->tMax, 0.0f);
            p->pos1830 = glm::vec4(pos + p->matHeading * glm::vec3(b - a), 1.0f);
        }
    }
    StopNearestLaunchTracking(p);  // 0x1431D0
}

// orig: 0x001B8508 / 0x001B85D0 (square / triangle: alternate a demoted punch, else choose the target's side)
void StartMurrayPunch(MURRAY* p, bool fTriangle)
{
    if (fTriangle && (!p->apasegPunch[2] || !p->apasegPunch[3])) return;  // (+0x1804 / +0x1808)
    SetMurrayState(p, 0xB);
    int k = fTriangle ? 3 : 1;
    if (p->pasega190C)
    {
        const ASEG* prev = p->pasega190C->paseg;
        const bool fFirst = prev == p->apasegPunch[0] || prev == p->apasegPunch[2];
        k = fTriangle ? (fFirst ? 3 : 2) : (fFirst ? 1 : 0);
    }
    else if (p->plo10DC)
    {
        const glm::vec3 d = VecJtConvert(nullptr, p->palo, PosJtLo(p->plo10DC) - PosAloCenter(p->palo));
        if (d.y > 0.0f) k = fTriangle ? 2 : 0;
    }
    SetMurrayPunch(p, k);
}

// orig: 0x001B8938 (the punch left playing at +0x190C, demoted on the legs, the state 0)
void EndMurrayPunch(MURRAY* p)
{
    if (p->pasega) DemoteAsegaOnAlo(p->pasega, p->palo, {{11, -1}, {17, -1}, {16, -1}, {21, -1}, {20, -1}});  // 0x131B60 (D_002C09F8)
    p->pasega190C = p->pasega;
    p->pasega = nullptr;
    SetMurrayState(p, 0);
}

// orig: 0x001B8988 (state 11's transitions: the punch's end, the next punch, the stick ends it)
void UpdateMurrayPunchTransitions(MURRAY* p)
{
    for (int iter = 0; iter < 8; ++iter)
    {
        const int s = p->n1810;
        if (!p->pasega || FAsegaRetracted(p->pasega)) SetMurrayState(p, 0);
        else if (p->n1810 == 0 || p->n1810 == 1)
        {
            if (FMurray1B84E0(p) && FBtnTake(p, BTN_SQUARE, 0.25f)) SetMurrayPunch(p, p->n1810 == 0 ? 1 : 0);
            else if ((!FMurray1B84E0(p) || !FMurrayGrab(p)) && p->f181C && 0.0f < UJoy(p)) EndMurrayPunch(p);
        }
        else if (p->n1810 == 2 || p->n1810 == 3)
        {
            if (FMurray1B84E0(p) && FBtnTake(p, BTN_TRIANGLE, 0.25f)) SetMurrayPunch(p, p->n1810 == 2 ? 3 : 2);
            else if ((!FMurray1B84E0(p) || !FMurrayGrab(p)) && p->f181C && 0.0f < UJoy(p)) EndMurrayPunch(p);
        }
        if (p->n1810 == s || p->n1810 == -1) break;
    }
}

// orig: 0x001B8B70 (state 11's motion: the clip's root velocity, steered by the stick between the events 0x240 / 0x242)
void UpdateMurrayPunch(MURRAY* p)
{
    if (p->pasega)
    {
        glm::vec4 v(0.0f);
        if (!FAsegaRetracted(p->pasega))  // 0x204E38
            if (const ASEGCHN* pchn = PchnFindAseg(p->pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
                pchn->ptrkp->Eval(nullptr, &v, p->pasega->t, p->pasega->svt);
        p->vecVel = -glm::vec3(v);
        if (p->f1818)
        {
            float t0 = 0.0f, t2 = 0.0f;
            for (const ASEGEV& ev : PasegdOf(p->pasega->paseg)->aaev[1])
                if (ev.kind == 11 && ev.an[0] == 0x240) { t0 = ev.t; break; }
            for (const ASEGEV& ev : PasegdOf(p->pasega->paseg)->aaev[1])
                if (ev.kind == 11 && ev.an[0] == 0x242) { t2 = ev.t; break; }
            const float u = (p->pasega->t - t0) / (t2 - t0);
            float g = 0.5f + u * (0.5f + u * 0.0f);  // D_002C0A20, limited by D_002BBB60
            g = g < 0.0f ? 0.0f : (1.0f < g ? 1.0f : g);
            const glm::vec4& v1700 = p->rb16F0.avel[0];  // (+0x1700)
            const float l = std::sqrt(v1700.x * v1700.x + v1700.y * v1700.y);
            const glm::vec3 wWorld = cmm::SetVectorCylind(RadJoy(p), UJoy(p) * l, 0.0f);  // 0x11E1A0
            const glm::vec3 w = glm::transpose(glm::mat3(MatWorldOfAlo(p->palo))) * wWorld;  // 0x1A6FC0 (0, p)
            p->vecVel = p->vecVel * (1.0f - g) + (-w) * g;
        }
    }
    if (p->plo10DC && p->n10E4 == 4) FaceActorToward(p, p->plo10DC);  // orig 0x1B8B70 -> 0x2046F0
}

// orig: 0x001B5D30 (state9's 0 still, 1 gait, 2 strafe toward the pinned guard)
void SetMurrayBehaviorAnim(MURRAY* p, int n)
{
    if (p->n17F4 == n) return;
    if (p->n17F4 == 0) SetXfModes(p, 2);
    p->n17F4 = n;
    p->t17F8 = g_clock.t;
    ASEG* aseg = nullptr;
    SM* sm = nullptr;
    int grf = 1;
    switch (n)
    {
    case 0: SetXfModes(p, 1); sm = p->psm16E0; p->vecVel = glm::vec3(0.0f); break;
    case 1: aseg = p->gait1630.pbl; ResetGaitFeet(&p->gait1630); grf = 9; break;
    case 2: aseg = p->rb16F0.pbl; grf = 9; break;
    }
    SetAnim(p, aseg, 0.0f, 1.0f, grf, sm, -1, 4, false);
}

// orig: 0x001B5990: state9's input priorities, then its gait/strafe motion.
void UpdateMurrayBehavior(MURRAY* p)
{
    int old;
    do
    {
        old = p->n17F4;
        if (old >= 0 && old < 3)
        {
            if ((GrfbtnHeld(p) & BTN_R1) || !FActorObjectAhead(p, 1)) SetMurrayState(p, 0);
            else if (FBtnTake(p, BTN_CROSS, 0.1f)) SetJumpKind(&p->jump, 1, 0, 0);
            else if (FMurrayGrab(p)) {}
            else if (FBtnTake(p, BTN_CIRCLE, 0.2f))
            {
                if (!FInteractCircle(p) && p->plo13EC) { p->n144C = 0; SetMurrayState(p, 4); }
            }
            else if (FMurray1B84E0(p) && FBtnTake(p, BTN_SQUARE, F32(0x3E19999A))) StartMurrayPunch(p, false);
            else if (FBtnWithin(p, BTN_R3, 0.15f) && FMurrayCanDo(p, 3))
            {
                ForgetBtn(p, BTN_R3);
                OpenBinoc(p, 0, nullptr, nullptr);
            }
            else if (FMurray1B84E0(p) && FBtnTake(p, BTN_TRIANGLE, F32(0x3E19999A))) StartMurrayPunch(p, true);
            else SetMurrayBehaviorAnim(p, (p->state == 0 ? 0.1f < UJoy(p) : 0.0f < UJoy(p)) ? 2 : 0);
        }
    } while (p->state == 9 && old != p->n17F4);
    auto face = [&]() {
        if (p->plo10DC) FaceActorToward(p, p->plo10DC);  // 0x2046F0
    };
    switch (p->n17F4)
    {
    case 0: face(); break;
    case 1:
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(&p->gait1630, UJoy(p)));
        UpdateGaitFeet(&p->gait1630);
        break;
    case 2:
    {
        face();
        const glm::vec4 v = p->rb16F0.avel[1];  // +0x1720
        const float speed = std::sqrt(v.x * v.x + v.y * v.y) * UJoy(p) * (p->pasega ? p->pasega->svt : 0.0f);
        const glm::vec3 world = cmm::SetVectorCylind(RadJoy(p) + F32(0x40490FDB), speed, 0.0f);
        p->vecVel = glm::transpose(p->matHeading) * world;
        p->n1038 = -1;
        SteerRootBlend(p, &p->rb16F0);
        break;
    }
    }
}

// orig: 0x001B5820 (state 6, the hold, as the current player: the face buttons pressed keep the held states
// 0x1CB..0x1CF up; their time out drops to the next state of the tables 0x2C0908 / 0x2C0948)
void UpdateMurrayHold(MURRAY* p)
{
    static const int s_aoidOut[3][5] = {{463, -1, -1, -1, 464}, {461, -1, 463, -1, 464}, {460, 461, 462, 463, 464}};  // 0x2C0908
    static const int s_aoidLeft[3][5] = {{458, -1, -1, -1, 459}, {458, -1, 459, -1, 461}, {458, 459, 459, 460, 461}};  // 0x2C0948
    if (PloPoCurrent(g_psw) != p->palo) return;  // 0x1C4840
    if (0 < g_gamepause.c78) return;             // 0x20F0B0
    if (!p->psma) return;
    int oid = OidSmaCur(p->psma);                // 0x1F07E8
    if (static int s_c = 0; std::getenv("SLY2N_GANGDEBUG") && s_c++ % 30 == 0)
        std::printf("murray hold t %.3f: sm cur %d goal %d t1524 %.2f n152C %d k %d\n", g_clock.t, oid, OidSmaGoal(p->psma), p->t1524, p->n152C, p->n1534);
    if (oid != OidSmaGoal(p->psma)) return;      // 0x1F07C8
    if (oid >= 0x1D0 || oid < 0x1CB) return;
    p->t1524 -= g_clock.dt;
    if (FBtnTake(p, BTN_SQUARE | BTN_CROSS | BTN_CIRCLE | BTN_TRIANGLE, 0.0f))  // 0x1C46C0 (0xF0)
    {
        p->n152C--;
        p->t1528 = p->t1524;
    }
    const int k = std::clamp(p->n1534, 0, 2);
    if (p->t1524 <= 0.0f)
        oid = p->n152C <= 0 ? s_aoidOut[k][oid - 0x1CB] : s_aoidLeft[k][oid - 0x1CB];
    else if (1.0f < p->t1528 - p->t1524)  // 0x2C0900
        oid = 0x1CA;
    if (p->psma1530) SetSmaGoal(p->psma1530, oid);  // 0x1F0660
    SetSmaGoal(p->psma, oid);
}

// orig: 0x001B6DA8 (Thunder Flop phases: 0 wind-up, 1 falling, 2 landing)
void SetMurrayFlopPhase(MURRAY* p, int n)
{
    if (p->n13D8 == n) return;
    if (p->n13D8 == -1) p->vecVel = glm::vec3(0.0f);
    p->n13D8 = n;
    p->t13DC = g_clock.t;
    switch (n)
    {
    case -1:
        FreeSwXpList(p->pxp13E4);
        p->pxp13E4 = nullptr;
        break;
    case 1:
    {
        // slot 33 -> slot 35: retain horizontal velocity, replace its vertical component.
        glm::vec3 v = VelAloPos(p->palo);
        const glm::vec3 up(0.0f, 0.0f, 1.0f);  // D_002A2DC0
        const float z = (v.x * up.x + v.y * up.y) + v.z * up.z;
        v = v - up * z;
        v = v + up * F32(0xC3FA0000);  // D_002C09A8, -500
        SetAloVelocityTo(p->palo, v);
        p->t131C = g_clock.t;
        break;
    }
    case 2:
    {
        p->vecVel = glm::vec3(0.0f);
        for (XP* pxp = p->pxp13E4; pxp; pxp = pxp->pxpNext)
        {
            const int iSelf = pxp->axpd[0].paloRoot == p->palo ? 0 : 1;
            const XPD& other = pxp->axpd[1 - iSelf];
            const XPD& self = pxp->axpd[iSelf];
            if (ALO* receiver = PaloXpFilter(other.palo, 4))
            {
                CTMSG msg;
                msg.ploSender = p->palo;
                msg.pvOther = self.psoe;
                msg.ploReceiver = receiver;
                msg.grfFilter = 4;
                msg.nState = -20;
                msg.pos = other.pos;
                msg.vec = self.vel;
                PaloAcceptContact(&msg);
            }
        }
        FreeSwXpList(p->pxp13E4);
        p->pxp13E4 = nullptr;
        const ASEGEV* landing = nullptr;
        if (p->pasega && p->pasega->paseg)
            for (const ASEGEV& ev : PasegdOf(p->pasega->paseg)->aaev[1])
                if (ev.kind == 11 && ev.an[0] == 0x1A5) { landing = &ev; break; }  // 0x12F938
        if (landing) SetAsegaTime(p->pasega, 0, landing->t, 1.0f);
        else if (p->pasega) RetractAsega(p->pasega);
        break;
    }
    default: break;
    }
    if (std::getenv("SLY2N_MURRAYPRINT"))
        std::fprintf(stderr, "murray-flop: frame %d t %.6f phase %d\n", g_iframeHarness, g_clock.t, n);
}

// orig: 0x001B6BA8 (phase transitions and the horizontal input target)
void UpdateMurrayFlop(MURRAY* p)
{
    for (int iter = 0; iter < 32; ++iter)
    {
        const int n = p->n13D8;
        if (!p->pasega || FAsegaRetracted(p->pasega)) SetMurrayState(p, 0);
        else if (n == 1)
        {
            if (p->pxp13E4) SetMurrayFlopPhase(p, 2);
            else if (FActorStuckOnContacts(p, p->t13DC, &p->t131C)) RecoverActor(p);  // 0x19A098 -> 0x19F590(p, 0)
        }
        if (n == p->n13D8 || p->n13D8 == -1) break;
    }
    if (p->n13D8 == 0 || p->n13D8 == 1)
    {
        const float r = RadJoy(p) + F32(0x40490FDB);
        const float s = (F32(0x3F400000) * F32(0x4400C000)) * UJoy(p);  // D_002C09A0 * D_002BF730
        p->vecVel = cmm::SetVectorCylind(r, s, 0.0f);  // 0x11E1A0 -> 0x11D608 (the game's polynomial sine/cosine)
    }
}

// orig: 0x001B6CC0 (slot 70: suspend gravity during wind-up; steer horizontally during the fall)
void UpdateMurrayFlopMotion(MURRAY* p)
{
    if (p->n13D8 != 0 && p->n13D8 != 1) return;
    if (p->n13D8 == 0)
        if (SOST* ps = Psost(p->palo)) AccelAloPos(p->palo, -ps->dvGravity);
    glm::vec3 d = -p->vecVel - VelAloPos(p->palo);
    const glm::vec3 up(0.0f, 0.0f, 1.0f);  // D_002A2DC0
    const float z = (d.x * up.x + d.y * up.y) + d.z * up.z;
    d = d - up * z;
    AccelAloPos(p->palo, d * F32(0x40200000));  // D_002C09A4, 2.5
}

// orig: 0x001B4AA0 (the state machine, run until the state stays)
void UpdateMurrayTransitions(MURRAY* p)
{
    int s1 = p->state;
    for (int iter = 0; iter < 32; ++iter)
    {
        // the fall after 0.1 s off the floor (states 0, 1, 5, 9..13, 19, >= 21)
        bool fTimed = true;
        if (static_cast<unsigned>(s1 - 2) < 0x13)
            fTimed = s1 == 5 || s1 == 9 || s1 == 10 || s1 == 11 || s1 == 12 || s1 == 13 || s1 == 19;
        if (fTimed && 0.1f < g_clock.t - p->tFloor)
        {
            const int v = p->state;
            if (v == 12) { if (p->n1840 == 3) SetJumpKind(&p->jump, 0xB, 0, 0); }
            else if (v == 5) SetJumpKind(&p->jump, 0x10, 0, 0);
            else if (v == 13) { if (p->n4C8 != 1 && p->n4C8 != 12) SetJumpKind(&p->jump, 0, 0, 0); }
            else SetJumpKind(&p->jump, 0, 0, 0);
        }
        switch (p->state)
        {
        case 0: case 1: case 10:
        {
            if (!(GrfbtnHeld(p) & BTN_R1) && FActorObjectAhead(p, 0)) { SetMurrayState(p, 9); break; }
            if (FBtnTake(p, BTN_CROSS, 0.1f)) { SetJumpKind(&p->jump, 1, 0, 0); break; }
            if (FBtnWithin(p, BTN_R3, F32(0x3E19999A)) && FMurrayCanDo(p, 3))
            {
                ForgetBtn(p, BTN_R3);
                OpenBinoc(p, 0, nullptr, nullptr);  // 0x19CC50 (the recon view)
                break;
            }
            if (FMurray1B84E0(p) && FBtnTake(p, BTN_TRIANGLE, F32(0x3E19999A))) { StartMurrayPunch(p, true); break; }
            if (FMurrayGrab(p)) break;
            if (FBtnTake(p, BTN_CIRCLE, F32(0x3E4CCCCD)))
            {
                if (FInteractCircle(p) || !p->plo13EC) break;  // 0x1938C0 (+0x4C0)
                p->n144C = 0;
                SetMurrayState(p, 4);
                break;
            }
            if (FMurray1B84E0(p) && FBtnTake(p, BTN_SQUARE, F32(0x3E19999A))) { StartMurrayPunch(p, false); break; }
            const float u = UJoy(p);
            const bool fMove = p->state == 0 ? 0.1f < u : 0.0f < u;
            if (!fMove) { SetMurrayState(p, 0); break; }
            SetMurrayState(p, (GrfbtnHeld(p) & BTN_R1) ? 1 : 0xA);
            break;
        }
        case 2:
        {
            UpdateJumpTransitions(&p->jump);  // 0x197500
            const int h = p->jump.kind;
            if (h == 0 || h == 11 || h == 15 || h == 16) break;
            // the Thunder Flop from the air (state 3: wind-up first, +0x13E8 0: the plain clip)
            if (!FBtnWithin(p, BTN_SQUARE | BTN_TRIANGLE, F32(0x3E19999A))) break;  // D_002C08DC
            if (!FMurrayCanDo(p, 2)) break;
            if (!(p->jump.state < 6 && p->jump.state > 0)) break;
            if (FActorFloorSoon(p, F32(0x3DCCCCCD))) break;  // D_002C08E0
            ForgetBtn(p, BTN_SQUARE | BTN_TRIANGLE);
            p->f13E8 = 0;
            SetMurrayState(p, 3);
            break;
        }
        case 6: UpdateMurrayHold(p); break;  // 0x1B5820
        case 4: UpdateMurrayCarryTransitions(p); break; // 0x1B92F8
        case 5: UpdateMurrayCarryHeld(p); break; // 0x1B5380
        case 3: UpdateMurrayFlop(p); break;
        case 9: UpdateMurrayBehavior(p); break;  // 0x1B5990
        case 17:
            // a script's animation (+0x1030) or state machine (+0x1034) over: standing (0x1B4AA0)
            if (p->pasega ? FAsegaRetracted(p->pasega) : (p->psma && FSmaRetracted(p->psma))) SetMurrayState(p, 0);
            break;
        case 7: case 8:
            if (p->pasega && FAsegaRetracted(p->pasega)) SetMurrayState(p, 0);
            break;
        case 11: UpdateMurrayPunchTransitions(p); break;
        case 12: UpdateMurrayBerserkTransitions(p); break;  // 0x1B8EC0
        case 21:
            // Turnbuckle Launch: cross launches (the jump's kind 13), letting go of the gadget's button stands
            if (FBtnTake(p, BTN_CROSS, 0.0f)) SetJumpKind(&p->jump, 13, 0, 0);
            else if (!(GrfbtnHeld(p) & GrfAbilityButton(p, 16))) SetMurrayState(p, 0);  // 0x141EA8(16)
            break;
        case 14: UpdateDamageTransitions(p); break;  // 0x1A12F0 (+0xDF0)
        case 13: UpdateInteractTransitions(p); break;  // 0x1911A8 (+0x4C0)
        case 15: UpdateJtThrow(p->jtthrow1910); break;  // 0x21F418 (+0x1910)
        case 16: case 18: case 19: break;
        default:
            WarnOnce("Murray's transitions of state " + std::to_string(p->state));
            break;
        }
        if (p->state == s1) break;
        s1 = p->state;
    }
}
}  // namespace

// orig: 0x001B6078
void FaceActorToward(ACTOR* pa, LO* plo);         // interact.cpp: 0x2046F0
LO* PloInteractOidField(LO* plo, uint32_t ib);    // interact.cpp

// orig: 0x001B9C00
void StartMurrayHold(MURRAY* p, SMA* psmaObj, int oid, int k, int fSnap)
{
    p->psma1530 = psmaObj;
    p->n1534 = k;
    SetMurrayState(p, 6);
    if (oid == -1) return;
    if (p->psma1530)
    {
        if (fSnap) SnapSmaGoal(p->psma1530, oid);  // 0x1F0948
        else SetSmaGoal(p->psma1530, oid);         // 0x1F0660
    }
    if (fSnap) SnapSmaGoal(p->psma, oid);
    else SetSmaGoal(p->psma, oid);
    if (std::getenv("SLY2N_GANGDEBUG"))
        std::printf("murray: hold kind %d to %d: his sma %p (sm %d cur %d goal %d), the object's %p (sm %d cur %d goal %d)\n", k, oid,
                    static_cast<void*>(p->psma), p->psma && p->psma->psm ? p->psma->psm->oid : -1, p->psma ? OidSmaCur(p->psma) : -1,
                    p->psma ? OidSmaGoal(p->psma) : -1, static_cast<void*>(psmaObj),
                    psmaObj && psmaObj->psm ? psmaObj->psm->oid : -1, psmaObj ? OidSmaCur(psmaObj) : -1, psmaObj ? OidSmaGoal(psmaObj) : -1);
}

void MurrayTakeObject(ACTOR* pa, LO* puse)
{
    MURRAY* p = pa ? PmurrayFromLo(pa->palo) : nullptr;
    if (!p || !p->plo13EC) return;
    p->plo13F0 = puse;
    p->n144C = 0;
    SetMurrayState(p, 4);  // 0x1B6078
    if (LO* po = PloInteractOidField(puse, 0x180)) FaceActorToward(p, po);  // 0x2046F0 (the object's owner)
}

// orig: 0x001B8D30 (Berserker Charge's phase, state 12: 0 the wind-up +0x1848, 1 the charge +0x184C, 2 the bounce off a
// wall (the kind-2 player +0x1890 with +0x1850's tuning, to the point its distance +0x186C behind), 3 the stop
// +0x1904; -1 off)
void SetMurrayBerserk(MURRAY* p, int st)
{
    static K2PLAY s_k2play;  // (+0x1890: one Murray)
    if (st == p->n1840) return;
    if (p->n1840 == 2 && p->pk2play1890)
    {
        StopK2(p->pk2play1890);  // 0x20C080 (and its vt slot 2)
        p->pk2play1890 = nullptr;
    }
    p->n1840 = st;
    p->t1844 = g_clock.t;
    ALO* palo = p->palo;
    ASEG* paseg = nullptr;
    int grf = 0;
    switch (st)
    {
    case 0:
        paseg = PPart<ASEG>(palo, 0x1848);
        p->vecVel = glm::vec3(0.0f);
        grf = 1;
        break;
    case 1:
        paseg = PPart<ASEG>(palo, 0x184C);
        grf = 9;
        break;
    case 2:
    {
        p->f1908 = 0;
        if (!p->pk1850) p->pk1850 = Pk2ResolveAlo(palo, PPart<ASEG>(palo, 0x1854));  // (0x20BD88 at the post-load)
        p->pk2play1890 = &s_k2play;
        StartK2(p->pk1850, palo, 1, p->pk2play1890);  // 0x20BF80
        const glm::mat4 m = MatJtLo(palo);  // 0x1A6318 (the object record's), 0x1A62E8 its position
        p->pk2play1890->posTarget = glm::vec3(m[3]) + glm::vec3(m[0]) * -(p->pk1850 ? p->pk1850->s1C : 0.0f);
        p->vecVel = glm::vec3(0.0f);
        break;
    }
    case 3:
        paseg = PPart<ASEG>(palo, 0x1904);
        grf = 1;
        break;
    case -1:
        p->f1908 = 0;
        return;
    }
    // (0x1F15B8 the state machine let go, 0x1305B8 the clip applied: none in phase 2)
    SetAnim(p, paseg, 0.0f, 1.0f, grf, nullptr, -1, 4, false);
}

// orig: 0x001B90B8 (state 12's update: the charge / stop follow the clip's root)
void UpdateMurrayBerserk(MURRAY* p)
{
    if (p->n1840 == 1 || p->n1840 == 3)
    {
        p->vecVel = VelActorAnimRoot(p);  // 0x204E38, negated
        p->uTurn = 1.0f;                  // +0x180
    }
}

// orig: 0x001B8EC0 (state 12's transitions)
void UpdateMurrayBerserkTransitions(MURRAY* p)
{
    const bool fMore = p->palo->U32Field(0, 0x1AC0, 0) != 0;  // +0x1AC0 (set by the data / an upgrade: the charge goes on)
    for (;;)
    {
        const int s = p->n1840;
        switch (s)
        {
        case 0:
            if (!p->pasega || FAsegaRetracted(p->pasega)) SetMurrayBerserk(p, 1);
            break;
        case 1:
            if (!p->pasega || FAsegaRetracted(p->pasega)) SetMurrayState(p, 0);
            else if (p->f1908) SetMurrayBerserk(p, 2);
            else if (!fMore && F32(0x3F28F5C3) < g_clock.t - p->t1844) SetMurrayBerserk(p, 3);
            break;
        case 2:
            if (!p->pk2play1890 || FK2Done(p->pk2play1890))  // 0x20C0D8
            {
                if (fMore) SetMurrayBerserk(p, 0);
                else SetMurrayState(p, 0);
            }
            else UpdateK2(p->pk2play1890);  // 0x20C0E8
            break;
        case 3:
            if (!p->pasega || FAsegaRetracted(p->pasega)) SetMurrayState(p, 0);
            else if (F32(0x3E19999A) < g_clock.t - p->t1844 && PpadctlOf(p) && PpadctlOf(p)->nControl == 1 &&
                     0.0f < PpadctlOf(p)->uJoy)
                SetMurrayState(p, 0);  // (+0x150's +0x80 1 and +0x1C > 0: the stick pushed)
            break;
        }
        if (p->n1840 == s || p->n1840 == -1) break;
    }
}

void SetMurrayState(MURRAY* p, int n)
{
    const int cur = p->state;
    if (n == cur) return;
    switch (cur)
    {
    case -1: break;
    case 0:
        SetXfModes(p, 2);
        ActorNextLookTime(p);  // 0x142880
        break;
    case 16: case 17: case 18: SetXfModes(p, 2); break;
    case 2:
        if (!p->ploWater && (n == 0 || n == 1 || n == 4 || n == 5 || n == 9 || n == 11 || n == 12 || n == 13))
        {
            LO* po = PloPart(p->palo, 0x1A50);
            if (n == 13) po = (p->n4C8 == 1) ? nullptr : (p->n4C8 == 3 || p->n4C8 == 4) ? PloPart(p->palo, 0x1A54) : PloPart(p->palo, 0x1A50);
            LandingBurst(po);
        }
        SetJumpAnimState(&p->jump, -1, nullptr);  // 0x195FB8
        break;
    case 1: case 10: p->g1A0 = UGaitPhase(p, p->g1A0); break;
    case 7: case 8: case 19: case 21: break;
    case 12: SetMurrayBerserk(p, -1); break;  // 0x1B8D30
    case 11: SetMurrayPunch(p, -1); break;  // 0x1B86B0
    case 3: SetMurrayFlopPhase(p, -1); break;
    case 14: SetDamageState(p, -1); break;  // 0x1A0B40 (+0xDF0)
    case 13: SetInteractState(p, -1); break;  // 0x192BD8 (+0x4C0)
    case 4: SetMurrayCarry(p, -1); break;     // 0x1B93E8
    case 5: SetMurrayCarryMove(p, -1); break; // 0x1B5658
    case 9: SetMurrayBehaviorAnim(p, -1); break;  // 0x1B5D30
    case 6: p->psma1530 = nullptr; break;
    case 15:
        if (p->jtthrow1910.peer) RemoveLo(s_psw, p->jtthrow1910.peer);  // +0x191C's vt slot 9
        SetJtThrowState(p->jtthrow1910, -1);                            // 0x2206C8 (+0x1910)
        SetAloCnstrLin(p->palo, 0);                                     // 0x1F5720
        break;
    default: WarnOnce("Murray's SetState: leaving state " + std::to_string(cur)); break;
    }
    const int old = p->state;
    p->state = n;
    p->tState = g_clock.t;
    ASEG* paseg = nullptr;
    SM* psm = nullptr;
    int grfapl = 1;
    OID oid = -1;
    uint32_t grfMask = 4;
    switch (n)
    {
    case 0:
        SetXfModes(p, 1);
        psm = p->psmIdle;
        p->vecVel = glm::vec3(0.0f);
        oid = old == 2 ? 0x178 : old != 10 ? 0x175 : 0x176;
        if (!psm || IsmsFindSm(psm, oid) < 0) oid = -1;  // 0x1F0400
        grfMask = GrfActorLookMask(p);  // 0x1430C8
        ActorSetLookFlag(p, 1);         // 0x142868
        break;
    case 1:
        paseg = p->gaitWalk.pbl;
        grfapl = 9;
        ResetGaitFeet(&p->gaitWalk);  // 0x20BD68
        break;
    case 10:
        paseg = p->gaitRun.pbl;
        grfapl = 9;
        ResetGaitFeet(&p->gaitRun);
        break;
    case 2: case 11: break;  // (11: the punch's clip by 0x1B86B0 after)
    case 3:
        SetMurrayFlopPhase(p, 0);
        paseg = p->paseg1AE8 && p->f13E8 ? p->paseg1AE8 : p->paseg13E0;
        break;
    case 16: case 17: case 18:
        if (n == 16) psm = p->psm102C;
        SetXfModes(p, 1);
        p->vecVel = glm::vec3(0.0f);
        break;
    case 14: SetBinocState(p, -1); break;  // 0x19CD38 (+0xF00)
    case 6: psm = p->psm1520; break;       // (the hold: its state machine from no state)
    case 5: SetMurrayCarryMove(p, 0); UpdateActorStepMesh(p); return;
    case 7:
        paseg = p->paseg1AE0 && FPowerupOwned(17) ? p->paseg1AE0 : p->paseg140C;
        p->vecVel = glm::vec3(0.0f);
        ChooseMurrayThrowTarget(p);
        SetStepPhysHeadingToward(p->palo, p->pos1420 - PosAloCenter(p->palo));
        break;
    case 9: SetMurrayBehaviorAnim(p, 0); UpdateActorStepMesh(p); return;
    case 4:
        // the pickup: its clip by +0x144C (0 +0x1440, 1 +0x1444), still, the carry's state 0 (0x1B93E8)
        paseg = p->n144C == 0 ? p->paseg1440 : p->n144C == 1 ? p->paseg1444 : nullptr;
        p->vecVel = glm::vec3(0.0f);
        SetMurrayCarry(p, 0);
        break;
    case 13: UpdateActorStepMesh(p); return;  // (the interaction's animation: 0x192BD8 after this)
    case 8:
        // Diablo Fire Slam (19) / the slam of a carried guard: +0x1AE4 when +0x143C, else +0x1438; no velocity
        paseg = PPart<ASEG>(p->palo, 0x1AE4) && p->f143C ? PPart<ASEG>(p->palo, 0x1AE4) : PPart<ASEG>(p->palo, 0x1438);
        p->vecVel = glm::vec3(0.0f);
        break;
    case 21:
        // Turnbuckle Launch (16): its clip +0x1AEC, still, looping (9)
        paseg = PPart<ASEG>(p->palo, 0x1AEC);
        p->vecVel = glm::vec3(0.0f);
        grfapl = 9;
        break;
    case 12: case 19: break;  // (12: Berserker Charge's phases by 0x1B8D30 after)
    case 15:
        // the Bentley toss (option 939, 0x1B9880): the holding frame +0x191C added (vt slot 8), the global Bentley
        // selected (+0x1920), the controller's hold (0x2206C8(+0x1910, 0)), the linear constraint, still (vt slot 35)
        if (p->jtthrow1910.peer) AddLo(s_psw, p->jtthrow1910.peer);
        p->jtthrow1910.selected = PbentleyGlobal() ? PbentleyGlobal()->palo : nullptr;
        SetJtThrowState(p->jtthrow1910, 0);
        SetAloCnstrLin(p->palo, 1);
        SetAloVelocityTo(p->palo, glm::vec3(0.0f));
        return;
    default: WarnOnce("Murray's SetState: entering state " + std::to_string(n)); return;
    }
    // (the seeking records arrive by the clip's event 0x1AC: 0x12F938 / 0x131A90)
    SetAnim(p, paseg, 0.0f, 1.0f, grfapl, psm, oid, grfMask, true);
    UpdateActorStepMesh(p);  // slot 90 (0x1B8008)
}

namespace
{
// orig: 0x001B5038 (the +0xF00 sub-object while it is busy)
void UpdateMurrayF00(MURRAY* p)
{
    UpdateActorBinoc(p, 0x4F0);  // (binoc.cpp)
}

namespace
{
int s_cMurrayUpdate = 0;
std::vector<bool> s_afMurrayTeleDone;
// fPre: the frame start (only the ",k" events); else Murray's update (the others)
void MurrayTeleportDebug(MURRAY* p, bool fPre)
{
    static const char* s_szTele = std::getenv("SLY2N_MTELEPORT");
    if (!s_szTele) return;
    const int cUpdate = fPre ? s_cMurrayUpdate : s_cMurrayUpdate++;
    int iev = 0;
    for (const char* q = s_szTele; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr, ++iev)
    {
        const std::string ev(q, std::strchr(q, ';') ? std::strchr(q, ';') : q + std::strlen(q));
        if (s_afMurrayTeleDone.size() <= static_cast<size_t>(iev)) s_afMurrayTeleDone.resize(iev + 1, false);
        float x = 0, y = 0, z = 0, h = 0, t = 0;
        int n = 0;
        // ("X Y Z@tT[,H]": at the first update at game time >= T)
        const bool fTime = ev.find("@t") != std::string::npos;
        const int c = fTime ? std::sscanf(ev.c_str(), "%f %f %f@t%f,%f", &x, &y, &z, &t, &h)
                            : std::sscanf(ev.c_str(), "%f %f %f@%d,%f", &x, &y, &z, &n, &h);
        const bool fRef = ev.find(",k") != std::string::npos;
        if (fRef != fPre || s_afMurrayTeleDone[iev]) continue;
        if (const size_t iw = ev.find("/w"); iw != std::string::npos && std::atoi(ev.c_str() + iw + 2) != g_cTransitionLoads) continue;
        const bool fNow = fTime ? t <= g_clock.t : cUpdate == n;
        if (c < 4 || !fNow) continue;
        s_afMurrayTeleDone[iev] = true;
        if (fRef)
        {
            // (",k": as the reference's --dev-teleport: slot 29 (0x143CF8: the point as given, the safe ground's
            // count +0x9E4 = 0), slots 35 / 36 zero velocities, the heading kept)
            SetStepPos(p, glm::vec3(x, y, z));
            p->safe.c = 0;
            SetAloAngVelocityTo(p->palo, glm::vec3(0.0f));
        }
        else
        {
            SetStepPos(p, glm::vec3(x, y, z) - glm::vec3(p->vecBase));
            glm::mat3 r(1.0f);
            r[0] = glm::vec3(std::cos(h), std::sin(h), 0.0f);
            r[1] = glm::vec3(-std::sin(h), std::cos(h), 0.0f);
            SetStepRotation(p, r);
        }
        SetAloVelocityTo(p->palo, glm::vec3(0.0f));
        CutCm(&g_cm);
    }
}
}  // namespace

// (debug) the ",k" Murray teleports at the frame start, as the reference's (before_transition, ahead of every update)
void MurrayTeleportPreUpdateImpl(SW* psw)
{
    if (!std::getenv("SLY2N_MTELEPORT") || !psw) return;
    for (const auto& up : psw->aploAll)
        if (MURRAY* p = up->fInWorld ? PmurrayFromLo(up.get()) : nullptr)
        {
            MurrayTeleportDebug(p, true);
            return;
        }
}

// orig: 0x001B50B0 (vt slot 16)
void UpdateMurray(LO* plo)
{
    MURRAY* p = PmurrayFromLo(plo);
    if (!p) return;
    // (debug, the actors code: SLY2N_MTELEPORT="X Y Z@N[,H]" puts Murray there at his N-th update, heading H; ",k": as the reference's
    // --dev-teleport, i.e. at the start of that frame, before any object's update (MurrayTeleportPreUpdate); several events separated by ';')
    MurrayTeleportDebug(p, false);
    ApplyRouteTeleportActor(p);
    UpdateActor(p);  // 0x1403D0
    if (!plo->fInWorld) return;
    if (p->pasega190C && FAsegaRetracted(p->pasega190C)) RetractAsegaPp(&p->pasega190C);  // 0x1306A8
    UpdateJtCandidates(p, 0xF8FA);  // 0x194A40 (the interaction targets, jt.cpp)
    UpdateMurrayF00(p);
    if (PpadctlOf(p)) UpdateMurrayTransitions(p);
    switch (p->state)
    {
    case 0:
        if (p->psma) SetSmaMask(p->psma, GrfActorLookMask(p));  // 0x1F1350
        break;
    case 1:
        SetStepPhysHeading(p->palo, RadJoy(p));                  // 0x204660
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitWalk, 1.0f));  // 0x20BB58
        UpdateGaitFeet(&p->gaitWalk);                            // 0x20BC08
        break;
    case 10:
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitRun, UJoy(p)));
        UpdateGaitFeet(&p->gaitRun);
        break;
    case 2: UpdateJumpInput(&p->jump); break;  // 0x197F98
    case 3: case 6: case 7: case 8: case 9: case 15: break;  // state3 / state9's transitions also steer their motion
    case 11: UpdateMurrayPunch(p); break;  // 0x1B8B70
    case 12: UpdateMurrayBerserk(p); break;  // 0x1B90B8
    case 19: case 21: break;
    case 13: UpdateInteract(p); break;      // 0x191A10 (+0x4C0)
    case 4: case 5: break; // original 0x2A71E0: no additional state4/5 update body
    case 17: UpdateGangScriptAnim(p, false); break;
    case 18: UpdateGangScriptAnim(p, true); break;
    default: WarnOnce("Murray's update of state " + std::to_string(p->state)); break;
    }
    // 0x1B82A0
    switch (p->state)
    {
    case 0: case 1: case 4: case 5: case 7: case 8: case 9: case 10: case 18: case 21: UpdateActorSupport(p); break;
    case 13: case 20: WarnOnce("0x1B82A0's sub-objects (states 13 / 20)"); break;
    default: break;
    }
    UpdateDamage(p);  // 0x1A1358 (+0xDF0)
    UpdateMurrayReleased(p);
    PrintMurrayCarry(p);
    // TODO(actors): the light +0x1ACC
    // Debug observer: optional game-time range ("205-220"); poses also include all local record matrices.
    const char* szMurrayPrint = std::getenv("SLY2N_MURRAYPRINT");
    float tPrintFrom = -1.0f, tPrintTo = 1.0e30f;
    if (szMurrayPrint) std::sscanf(szMurrayPrint, "%f-%f", &tPrintFrom, &tPrintTo);
    if (szMurrayPrint && tPrintFrom <= g_clock.t && g_clock.t <= tPrintTo)
    {
        const glm::vec3 pos = PosAloCenter(p->palo), vel = VelAloPos(p->palo);
        const XF* obj = PxfObj(p->palo);
        const glm::vec3 local = obj ? glm::vec3(obj->mat[3]) : glm::vec3(0.0f);
        std::fprintf(stderr, "murray: frame %d t %.6f state %d phase %d pos %.6f %.6f %.6f vel %.6f %.6f %.6f anim %d at %.6f local %.6f %.6f %.6f\n",
                     g_iframeHarness, g_clock.t, p->state, p->n13D8, pos.x, pos.y, pos.z, vel.x, vel.y, vel.z,
                     p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -1, p->pasega ? p->pasega->t : 0.0f,
                     local.x, local.y, local.z);
        if (std::getenv("SLY2N_MURRAYPOSE") && (p->state == 3 || std::getenv("SLY2N_MURRAYPOSE_ALL")))
            for (size_t i = 0; i < p->palo->axf.size(); ++i)
            {
                const XF& xf = *p->palo->axf[i];
                std::fprintf(stderr, "murray-xf: frame %d index %zu kind %d tag %d local", g_iframeHarness, i, xf.kind, xf.tag);
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 3; ++r) std::fprintf(stderr, " %.9g", xf.mat[c][r]);
                std::fprintf(stderr, "\n");
                if (std::getenv("SLY2N_MURRAYPOSE_ALL"))
                {
                    const CTRL* ctrl = xf.pctrlFirst;
                    const auto* ca = dynamic_cast<const CTRLASEG*>(ctrl);
                    std::fprintf(stderr, "murray-ctrl: frame %d index %zu mode %d state %d g %.9g range %.9g %.9g target %.9g ctrl %d prio %d anim %d time %.9g\n",
                                 g_iframeHarness, i, xf.mode, xf.state, xf.G(), xf.g0, xf.g1, xf.tTarget,
                                 ctrl ? ctrl->kind : -1, ctrl ? ctrl->prio : 0,
                                 ca && ca->pasega && ca->pasega->paseg ? ca->pasega->paseg->oid : -1,
                                 ca && ca->pasega ? ca->pasega->t : 0.0f);
                }
            }
    }
    CompareGangFrame(p, g_iframeHarness);  // (gangcheck.cpp)
}

// orig: 0x001B67F8 (vt slot 70)
void UpdateMurrayMotion(ALO* palo)
{
    MURRAY* p = PmurrayFromLo(palo);
    if (!p) { UpdateStepPhysMotion(palo); return; }
    if (p->state == 20) ApplyAloGravity(palo);  // 0x1F9B20
    else UpdateStepPhysMotion(palo);            // 0x204108
    switch (p->state)
    {
    case 2: UpdateJumpMotion(&p->jump); break;  // 0x196DF8
    case 14: UpdateDamageMotion(p); break;  // 0x1A1148
    case 13: UpdateInteractMotion(p); break;  // 0x194170 (+0x4C0)
    case 3: UpdateMurrayFlopMotion(p); break;
    case 20: WarnOnce("Murray's motion of state " + std::to_string(p->state)); break;
    default: break;
    }
}

// orig: 0x001B9130 (vt slot 55)
void OnMurrayXpNew(ALO* palo, XP* pxp, int i)
{
    OnStepXpNew(palo, pxp, i);  // 0x204250
    MURRAY* p = PmurrayFromLo(palo);
    if (!p) return;
    switch (p->state)
    {
    case 14: AdjustDamageXp(p, pxp); break;  // 0x1A2100
    case 12:
        // the charge's contact with something still and not a floor, against the motion: the bounce (+0x1908)
        if (p->n1840 == 1)
        {
            ALO* pother = pxp->axpd[1 - i].palo;
            using PFNS = bool (*)(ALO*);
            PFNS pfnStill = pother ? reinterpret_cast<PFNS>(PvNativeVt(pother, 62)) : nullptr;
            if (pother && (pfnStill ? pfnStill(pother) : FAloStill(pother)) && pxp->normal.z < 0.7f &&
                glm::dot(VelAloPos(palo), pxp->normal) < F32(0xBE99999A))  // vt slot 33, D_002C0A38 -0.3
                p->f1908 = 1;
        }
        break;
    case 3:
        if (p->n13D8 == 1 && F32(0x3F333333) < (i == 0 ? 1.0f : -1.0f) * pxp->normal.z)
            PrependXpList(PxpCloneList(pxp), &p->pxp13E4);
        break;
    case 20: WarnOnce("0x19E9C8 (+0xF50's contact)"); break;
    default: break;
    }
}

// orig: 0x001B8368 (vt slot 85)
uint32_t GrfMurrayXp(ALO* palo)
{
    MURRAY* p = PmurrayFromLo(palo);
    if (!p || p->fNoContacts) return 0;
    if (p->n13D8 == 1 || p->state == 20) return 0;
    return GrfActorStance(p);  // 0x143C78
}

// orig: 0x001B68B8 (vt slot 5: the messages; 9 the events of its clips)
void OnMurrayMessage(LO* plo, int msgk, void* pv)
{
    MURRAY* p = PmurrayFromLo(plo);
    if (p) HandleActorMessage(p, msgk, pv);  // the base's 0x140F90 first (sly.cpp)
    if (p && msgk == 10 && pv)
    {
        // a state change of his state machine in the hold (state 6): its ends drop it, a held state starts its time
        const SMAMSG10* pm = static_cast<const SMAMSG10*>(pv);
        if (pm->psma != p->psma || p->state != 6 || pm->oidNew == pm->oidOld) return;
        switch (pm->oidNew)
        {
        case 0x1CA: case 0x1D0: SetMurrayState(p, 0); break;
        case 0x1CB: case 0x1CC: case 0x1CD: case 0x1CE: case 0x1CF:
        {
            static const float s_at[3] = {1.5f, 1.5f, 1.5f};  // 0x2C0990
            const float t = s_at[std::clamp(p->n1534, 0, 2)];
            p->t1524 = p->t1528 = t;
            p->n152C = static_cast<int>(4.0f * t);  // 0x2C099C
            break;
        }
        default: break;
        }
        return;
    }
    if (!p || msgk != 9 || !pv) return;
    const ASEGMSG9* pm = static_cast<const ASEGMSG9*>(pv);
    // Berserker Charge's bounce: its kind-2 player's take-off (the ASEGA's listener 0x20C290: the event 0x22)
    if (p->pk2play1890 && pm->pasega && pm->pasega == p->pk2play1890->pasega && pm->n == 0x22) SetK2State(p->pk2play1890, 1);
    if (std::getenv("SLY2N_MURRAYPRINT"))
        std::fprintf(stderr, "murray-event: frame %d t %.4f state %d event 0x%X mine %d\n", g_iframeHarness, g_clock.t, p->state, pm->n,
                     pm->pasega == p->pasega || pm->pasega == p->pasega190C);
    if (pm->pasega != p->pasega && pm->pasega != p->pasega190C) return;
    switch (pm->n)
    {
    case 0x1C6: p->n1448 = 1; break;
    case 0x240: p->f1818 = 1; break;
    case 0x241: p->f181C = 1; break;
    case 0x242: p->f1820 = 1; break;
    case 0x1A4: SetMurrayFlopPhase(p, 1); break;
    case 0x1A5: PnoiseNew(PtmplNoiseFixed(0), PosAloCenter(p->palo), p->palo); break;
    case 0x1EB:
        if (p->state == 7) ReleaseMurrayThrow(p);
        else if (p->state == 8) PutDownMurrayCarry(p);  // 0x1B7AE8
        else if (p->state == 15) ReleaseJtThrow(p->jtthrow1910);  // 0x2205A8 (+0x1910)
        break;
    case 0x1C8:
        if (p->n1450 == 0) { PnoiseNew(PtmplNoiseFixed(0), PosAloCenter(p->palo), p->palo); SetMurrayCarry(p, 1); }
        break;
    case 0x1C7: if (p->n1450 == 1) SetMurrayCarry(p, 2); break;
    default: break;
    }
}

// orig: 0x001B7F90 (vt slot 89: the jump kind of the state, -1 none)
int MurrayJumpKind(LO* plo)
{
    MURRAY* p = PmurrayFromLo(plo);
    if (!p) return -1;
    static const int s_ak[21] = {0, -1, 1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1, 2, 3, -1, 4, 5, 6, 7, 8};
    return static_cast<unsigned>(p->state) < 21 ? s_ak[p->state] : -1;
}

// orig: 0x001B7E40 (vt slot 88)
void SetMurrayJumpState(LO* plo, int n)
{
    MURRAY* p = PmurrayFromLo(plo);
    if (!p) return;
    switch (n)
    {
    case 0: SetMurrayState(p, p->plo13F0 ? 5 : 0); break;
    case 1:
        if (p->jump.kind != 15 && p->jump.kind != 16) CancelMurrayCarry(p);
        SetMurrayState(p, 2);
        break;
    case 2: SetMurrayState(p, 0xD); break;
    case 3: CancelMurrayCarry(p); SetMurrayState(p, 0xE); break;
    case 4: SetMurrayState(p, 0x10); break;
    case 5: CancelMurrayCarry(p); SetMurrayState(p, 0x11); break;
    case 6: CancelMurrayCarry(p); SetMurrayState(p, 0x12); break;
    case 7: CancelMurrayCarry(p); SetMurrayState(p, 0x13); break;
    case 8: CancelMurrayCarry(p); SetMurrayState(p, 0x14); break;
    default: break;
    }
}
}  // namespace

// orig: 0x001B6728 (vt slot 50)
void GetMurrayCpdefi(MURRAY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pc)
{
    if (a.state != 0 && a.state != 16 && a.nSub4CC) a.t1080 = ctx.t;
    GetActorCmTarget(a, pose, ctx, pc);  // 0x141640
    switch (a.state)
    {
    case 11:
        if (a.n10E4 != 4) break;
        [[fallthrough]];
    case 3: case 9: case 12: pc->grfcpd |= 1; break;
    case 15: GetJtThrowCpdefi(a.jtthrow1910, pc); break;  // 0x220388 (+0x1910)
    case 20:
    {
        glm::vec3 dir(0.0f);
        if (a.pcrvF50) EvalCrvS(a.pcrvF50, a.sCrvF50, nullptr, &dir);
        pc->sAdjust = 0.0f;
        pc->vBase = glm::vec4(dir * a.gCrvF50, 0.0f);
        break;
    }
    default: break;
    }
}

void InitWorldGang(SW* psw)
{
    s_psw = psw;
    for (CID cid : {0x37, 0x36})
        for (LO* plo : psw->mpcidaplo[cid])
        {
            ALO* palo = static_cast<ALO*>(plo);
            StepPhysOf(palo);  // (made with 0x203948's defaults when the physics' post-load did not)
            if (std::getenv("SLY2N_GANGDEBUG"))
                for (size_t i = 0; i < palo->axf.size() && i < 5; ++i)
                    std::printf("gang: %d record %zu kind %d local %.2f %.2f %.2f world %.2f %.2f %.2f; vecBase z %.3f\n", plo->oid, i,
                                palo->axf[i]->kind, palo->axf[i]->mat[3].x, palo->axf[i]->mat[3].y, palo->axf[i]->mat[3].z,
                                palo->axf[i]->matWorld[3].x, palo->axf[i]->matWorld[3].y, palo->axf[i]->matWorld[3].z,
                                StepPhysOf(palo).vecBase.z);
            if (BENTLEY* p = PbentleyFromLo(plo))
            {
                InitBentley(p);
                PostLoadBentley(p);
            }
            else if (MURRAY* p = PmurrayFromLo(plo))
            {
                InitMurray(p);
                PostLoadMurray(p);
            }
        }
}

namespace
{
struct INITGANG
{
    INITGANG()
    {
        RegisterLoUpdate(0x134DA8, UpdateBentley);
        RegisterLoUpdate(0x1B50B0, UpdateMurray);
        // EOPID 777 (Bentley's option 760, a field (0x92) at +0x1870): the ability triangle summons (e.g. Ep7's egg
        // section, module 128: 4, the egg's proxy 543; the init's 5 is the bombs)
        RegisterObjectSetter(777, [](LO* plo, const CRef& ref) {
            if (BENTLEY* p = PbentleyFromLo(plo)) p->n1870 = ref.m_tagk == TAGK_F32 ? static_cast<int>(ref.m_g) : ref.m_n;
        });
        RegisterObjectSetter(775, [](LO* plo, const CRef& ref) {
            if (BENTLEY* p = PbentleyFromLo(plo)) p->f1818 = ref.m_tagk == TAGK_F32 ? static_cast<int>(ref.m_g) : ref.m_tagk == TAGK_Bool ? ref.m_bool : ref.m_n;
        });
        RegisterObjectGetter(775, [](LO* plo) {
            CRef r;
            const BENTLEY* p = PbentleyFromLo(plo);
            r.SetS32(p ? p->f1818 : 0);
            return r;
        });
        RegisterObjectGetter(777, [](LO* plo) {
            CRef r;
            const BENTLEY* p = PbentleyFromLo(plo);
            r.SetS32(p ? p->n1870 : 0);
            return r;
        });
        // orig: 0x0010D260 / 0x0010D2E8 (methods937/938).
        RegisterObjectMethod(0x10D260, [](LO* plo, const std::vector<CRef>& a) {
            if (MURRAY* p = PmurrayFromLo(plo); p && !a.empty()) MurrayTakeObject(p, dynamic_cast<LO*>(a[0].m_pbasic));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10D2E8, [](LO* plo, const std::vector<CRef>& a) {
            if (MURRAY* p = PmurrayFromLo(plo)) StartMurrayThrow(p, a.empty() ? nullptr : dynamic_cast<LO*>(a[0].m_pbasic));
            return CRef(TAGK_Void);
        });
        // orig: 0x0010D370 -> 0x001B9880 (option 939, EOPID 1051): SetMurrayState(Murray, 15), the Bentley toss
        RegisterObjectMethod(0x10D370, [](LO* plo, const std::vector<CRef>&) {
            if (MURRAY* p = PmurrayFromLo(plo)) SetMurrayState(p, 15);
            return CRef(TAGK_Void);
        });
        // orig: 0x0010D468 -> 0x001B9D00, method 941 (SCPT 1682's Thunder Flop entrance).
        RegisterObjectMethod(0x10D468, [](LO* plo, const std::vector<CRef>&) {
            if (MURRAY* p = PmurrayFromLo(plo))
            {
                if (std::getenv("SLY2N_MURRAYPOSE_ALL"))
                {
                    std::fprintf(stderr, "murray-inherited: frame %d state %d anim %d time %.9g\n", g_iframeHarness, p->state,
                                 p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -1, p->pasega ? p->pasega->t : 0.0f);
                    for (size_t i = 0; i < p->palo->axf.size(); ++i)
                    {
                        const XF& xf = *p->palo->axf[i];
                        const CTRL* ctrl = xf.pctrlFirst;
                        const auto* ca = dynamic_cast<const CTRLASEG*>(ctrl);
                        std::fprintf(stderr, "murray-before: index %zu kind %d tag %d mode %d state %d anim %d time %.9g local", i, xf.kind, xf.tag, xf.mode, xf.state,
                                     ca && ca->pasega && ca->pasega->paseg ? ca->pasega->paseg->oid : -1, ca && ca->pasega ? ca->pasega->t : 0.0f);
                        for (int c = 0; c < 4; ++c)
                            for (int r = 0; r < 3; ++r) std::fprintf(stderr, " %.9g", xf.mat[c][r]);
                        std::fprintf(stderr, "\n");
                    }
                }
                p->f13E8 = 0;
                SetMurrayState(p, 3);
            }
            return CRef(TAGK_Void);
        });
    }
} s_initgang;
}  // namespace

NATIVEFN(0x135940, UpdateBentleyMotion);
NATIVEFN(0x135A48, OnBentleyXpNew);
NATIVEFN(0x136CC0, GrfBentleyXp);
NATIVEFN(0x135C20, SetBentleyJumpState);
NATIVEFN(0x135D08, BentleyJumpKind);
NATIVEFN(0x135AD8, OnBentleyMessage);
NATIVEFN(0x1B67F8, UpdateMurrayMotion);
NATIVEFN(0x1B9130, OnMurrayXpNew);
NATIVEFN(0x1B8368, GrfMurrayXp);
NATIVEFN(0x1B7E40, SetMurrayJumpState);
NATIVEFN(0x1B7F90, MurrayJumpKind);
NATIVEFN(0x1B68B8, OnMurrayMessage);

// (exported for Sly's state 5 / 10: actors code)
void BindActorRootBlend(ROOTBLEND* prb, ALO* palo, uint32_t ib) { BindRootBlendParts(prb, palo, ib); }
void SteerActorRootBlend(ACTOR* p, ROOTBLEND* prb) { SteerRootBlend(p, prb); }

// ------------------------------------------------------------ the gadgets on L1 / L2 / R2 (gadget.cpp)
// orig 0x001B98A0 (Murray's slot 99) for the gadget framework
bool FMurrayAbilityOk(ACTOR* pa, int k)
{
    MURRAY* p = PmurrayFromLo(pa->palo);
    return p && FMurrayCanDo(p, k);
}
// orig 0x001B9A08 (Murray's slot 100; the task event 0x3C is the caller's): 20 Berserker Charge (state 12, phases 0
// then 1), 15 Fists of Flame (+0x9BC, the clip +0x1AC4 toggled by the interaction's event 0x27C), 19 Diablo Fire Slam
// (+0x143C, state 8), 22 Raging Inferno Flop (+0x13E8, state 3), 21 Guttural Roar (+0x9BC, the interaction with
// +0x10DC; event 0x27C: every guard in mode 0x15), 16 Turnbuckle Launch (state 21); else 0x141D38
bool StartMurrayAbility(ACTOR* pa, int k)
{
    MURRAY* p = PmurrayFromLo(pa->palo);
    if (!p) return false;
    switch (k)
    {
    case 20:
        SpendActorPower(p, k);  // 0x141CD0
        SetMurrayState(p, 12);
        SetMurrayBerserk(p, 0);
        SetMurrayBerserk(p, 1);
        return true;
    case 15:
        p->n9BC = k;
        p->pasegAbility = PPart<ASEG>(p->palo, 0x1AC4);  // +0x130C
        StartInteract(p, 11, nullptr, JTL_Max);
        return true;
    case 19:
        SpendActorPower(p, k);
        p->f143C = 1;
        SetMurrayState(p, 8);
        return true;
    case 22:
        SpendActorPower(p, k);
        p->f13E8 = 1;
        SetMurrayState(p, 3);
        return true;
    case 21:
        p->n9BC = 0x15;
        StartInteract(p, 11, p->plo10DC, JTL_Max);
        return true;
    case 16:
        SetMurrayState(p, 0x15);
        return true;
    default:
        return false;  // (0x141D38: nothing for Murray's other ids, e.g. 27)
    }
}

// orig 0x00136870 (Bentley's slot 99) for the gadget framework
bool FBentleyAbilityOk(ACTOR* pa, int k)
{
    BENTLEY* p = dynamic_cast<BENTLEY*>(pa);
    return p && FBentleyCanDo(p, k);
}
// orig 0x00136A90 (Bentley's slot 100: the task event 0x3C is the caller's): the bombs 4 / 5 / 9 / 13 summoned and
// held as triangle's (state 6, the ability +0x16B4: its proxy), 10 (Adrenaline Burst) state 0xE, and 12 (Hover Pack)
// through generic 0x141D38 to jump kind 17; the thrown ones 7 / 11 (Trigger Bomb, Health Extractor: an instance of the
// proxy (0x1CB250) handed to the throw controller +0x11B0 (state 4, the aim), Bentley in state 0xC; kept in the
// ability's +8 (0x18D698) so the next press triggers it through the interaction kind 0xB) and 8 (the crossbow's Size
// Destabilizer: +0x9BC / +0x130C = +0x1874, the interaction kind 0xB). False when not started.
bool StartBentleyAbility(ACTOR* pa, int k)
{
    BENTLEY* p = dynamic_cast<BENTLEY*>(pa);
    if (!p) return false;
    switch (k)
    {
    case 4: case 5: case 7: case 9: case 11: case 13:
    {
        ABILITY* pe = PabilFind(p, k);   // 0x136A28
        if (!pe) return false;
        if (static_cast<unsigned>(p->jtthrow.state - 4) < 3)
        {
            SetJtThrowState(p, 7);  // 0x2206C8 (+0x11B0, 7): the throw
            return true;
        }
        if (pe->plo8)
        {
            StartInteract(p, 11, pe->plo8, JTL_Max);  // 0x1928F8 (+0x4C0, 0xB, +8): its trigger (event 0x27C)
            return true;
        }
        p->pabil16B4 = pe;
        if (k == 7 || k == 11)
        {
            LO* po = pe->plo4 ? PloAcquireProxyObject(g_psw, pe->plo4, false) : nullptr;  // 0x1CB250
            if (!po) return true;
            CARRY* pc = PcarryFromLo(po);
            if (pc) pc->kind = pe->id;  // +0x15C
            if (pc && (pc->kind == 7 || pc->kind == 11))  // 0x224610
            {
                pc->plo16C = p->palo;
                SetAbilityCarried(p, pe, po);  // 0x18D698
            }
            p->jtthrow.selected = po;  // +0x11C0
            (void)GrfAbilityButton(p, k);  // 0x141EA8 (a mask lookup)
            SetBentleyState(p, 0xC);   // 0x135248
            SetJtThrowState(p, 4);     // 0x2206C8 (+0x11B0, 4): the aim
            return true;
        }
        p->pabil16B4 = pe;
        SpendActorPower(p, k);   // 0x141CD0
        SetBentleyState(p, 6);   // 0x135248
        return true;
    }
    case 8:
        p->n9BC = k;
        p->pasegAbility = PPart<ASEG>(p->palo, 0x1874);  // +0x130C = +0x1874
        if (std::getenv("SLY2N_GADGETPRINT"))
            std::printf("gadget: Size Destabilizer clip %d\n", p->pasegAbility ? p->pasegAbility->oid : -1);
        StartInteract(p, 11, nullptr, JTL_Max);  // 0x1928F8 (+0x4C0, 0xB)
        return true;
    case 10:  // Adrenaline Burst (0x136A90): spend before entering its running animation.
        SpendActorPower(p, k);
        SetBentleyState(p, 14);
        return true;
    case 12:
        // orig: 0x00141D38 (0x136A90's default): entry cost, then Hover Pack's jump kind.
        SpendActorPower(p, k);
        SetJumpKind(&p->jump, 17, 0, 0);
        return true;
    case 14:
        // orig: 0x00141D38 (0x136A90's default): Temporal Lock pending (+0x9BC), the interaction's kind 0xB; the
        // animation's event 0x27C spends and freezes the guards (gadget.cpp ReleaseTimeLock)
        p->n9BC = k;
        StartInteract(p, 11, nullptr, JTL_Max);
        return true;
    default:
        // (0x141D38's default does nothing, e.g. 27)
        return false;
    }
}

// (debug) see MurrayTeleportDebug
void MurrayTeleportPreUpdate(SW* psw) { MurrayTeleportPreUpdateImpl(psw); }
