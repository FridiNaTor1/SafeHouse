// guru3.cpp: the Guru (Sly 3 class 0x37), from the Sly 3 decomp (game/guru3.h).
#include "game/guru3.h"
#include "combo3.h"

#include "game/sly3.h"

#include "actanim.h"
#include "airide3.h"
#include "guard.h"
#include "actmesh.h"
#include "actor.h"
#include "alo.h"
#include "asega.h"
#include "binoc.h"
#include "binoc3.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "damage.h"
#include "disc.h"
#include "interact.h"
#include "joy.h"
#include "jump.h"
#include "lo.h"
#include "po.h"
#include "routetele.h"
#include "sly.h"
#include "smrun.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "xf.h"
#include "activate.h"
#include "objmethod.h"
#include "rumble.h"
#include "screenrt.h"
#include "game/scptgame3.h"
#include "cm.h"
#include "splitscreen3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>

void HandleActorMessage(ACTOR* pa, int msg, void* pv);  // sly.cpp: orig 0x140F90 (orig3 0x153738)

int OverlapKindActor(ALO* palo, ALO* pOther);   // jtthrow.cpp (orig 0x144388, orig3 0x156948)

namespace sly3
{
namespace
{
// the pad controller's button bits (the game's grfbtn, as gang.cpp's / jump.cpp's; joy.h's BTN_* are the raw PS2 bits:
// with those the Guru's Cross / Circle / R3 / R1 were never seen, so he could not jump, use, look or walk by R1)
constexpr uint32_t c_grfCross = 0x40, c_grfCircle = 0x20, c_grfR3 = 0x400, c_grfR1 = 0x8, c_grfL1 = 0x4;
constexpr uint32_t c_vaGuruParts = 0x349460;   // the Guru's parts table (43 entries; his loader 0x22C260)
constexpr uint32_t c_vaActorParts = 0x342DD0;  // the actors' (59 entries; 0x152488)
constexpr uint32_t c_vaPenelopeParts = 0x3468A0;   // Penelope's (53 entries; her loader 0x1EFF30)
constexpr uint32_t c_vaPandaKingParts = 0x346650;  // the Panda King's (49 entries; his loader 0x1E9438)

void WarnOnce(const std::string& what)
{
    static std::unordered_set<std::string> s_seen;
    if (s_seen.insert(what).second) std::fprintf(stderr, "guru3: %s not ported\n", what.c_str());
}

bool FDebug()
{
    static const bool s_f = std::getenv("SLY3_GURUPRINT") != nullptr;
    return s_f;
}

void SetPkState(GURU* p, int n);   // (the Panda King's, below)

PADCTL* PpadctlOf(GURU* p)
{
    PODATA* ppd = PpodataFromLo(p->palo);
    return ppd ? ppd->ppadctl : nullptr;
}
float UJoy(GURU* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->uJoy : 0.0f; }
float RadJoy(GURU* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->radJoy : 0.0f; }
uint32_t GrfbtnHeld(GURU* p) { PADCTL* pc = PpadctlOf(p); return pc ? pc->grfbtn : 0; }
// orig3 0x1A7A80 (the input's button pressed within dt, taken)
bool FBtnTake(GURU* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlTakePressed(pc, grf, dt); }
bool FBtnWithin(GURU* p, uint32_t grf, float dt) { PADCTL* pc = PpadctlOf(p); return pc && GrfPadctlPressedWithin(pc, grf, dt); }
void ForgetBtn(GURU* p, uint32_t grf) { if (PADCTL* pc = PpadctlOf(p)) ForgetPadctlPresses(pc, grf); }

LO* PloPart(ALO* palo, uint32_t ib)
{
    for (const auto& [ibPart, plo] : palo->apart)
        if (ibPart == ib) return plo;
    return nullptr;
}
template <class T>
T* PPart(ALO* palo, uint32_t ib) { return dynamic_cast<T*>(PloPart(palo, ib)); }

// orig3 0x28F068 on the position (+0x48) / rotation (+0x4C) records
void SetXfModes(GURU* p, int mode)
{
    if (XF* pxf = PxfPos(p->palo)) SetXfMode(p->palo, pxf, mode);
    if (XF* pxf = PxfRot(p->palo)) SetXfMode(p->palo, pxf, mode);
}

// orig3 0x156998 (busy: the binocucom (+0xF60) state +0xF64 below 3). A script's binocucom (SCPT 0x46, the panel / camera
// of binoc3.cpp / binoccam3.cpp) does not go through the native binoc's state (nF04): its window open counts too, so the
// Guru stays put through a binocucom talk (m2_shaman's SCPT 2678: with his Cross read he jumped during it)
bool FGuruBusy(GURU* p) { return static_cast<unsigned>(p->nF04) < 3 || NBinocPanelState3(p->palo) != -1; }

// orig3 0x22D1F8 (vt slot 113, Sly 2's 99: may the action n start): in the states 0..2 (3 / 4 their sub-objects'
// say: not ported, refused), only the actions 2 and 3 (the binocucom)
bool FGuruCanDo(GURU* p, int n)
{
    if (FGuruBusy(p)) return false;   // (0x154518, the base's: approximated by the busy test)
    if (!(0 <= p->state && p->state <= 2)) return false;
    return n == 2 || n == 3;
}

// The geometry entry a part of kind 0x20 names, by its Sly 3 offset (the actors' table, then the Guru's)
int IsoeGuruPart(ALO* palo, uint32_t ib3)
{
    int tag = -1;
    const bool fPen = FIsLoDerivedFrom(palo, CID_PENELOPE), fPk = FIsLoDerivedFrom(palo, CID_PANDA_KING);
    for (const auto& [va, c] : {std::pair<uint32_t, int>{c_vaActorParts, 59},
                                {fPen ? c_vaPenelopeParts : fPk ? c_vaPandaKingParts : c_vaGuruParts, fPen ? 53 : fPk ? 49 : 43}})
        for (int i = 0; i < c && tag == -1; ++i)
            if (g_disc.U32Elf(va + 12u * i + 8) == ib3 && (g_disc.U32Elf(va + 12u * i) & 0x20))
                tag = static_cast<int>(g_disc.U32Elf(va + 12u * i + 4));
    if (tag == -1) return -1;
    const int isoeCustom = StepPhysOf(palo).isoeCustom;
    for (size_t i = 0; i < palo->asoe.size(); i++)
        if (palo->asoe[i].tag == tag && static_cast<int>(i) != isoeCustom) return static_cast<int>(i);
    return -1;
}

// The jump's segments (the parts +0x3E0.. +0x41C at Sly 2's offsets: loaders.cpp's rows for the Guru's table)
void BindJumpParts(GURU* p)
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
    p->jump.pasegRide = PPart<ASEG>(palo, FN3_TAG | 0x42C);   // (Sly 3 jump +0x10C, part 21: the leap onto a link)
}

// the actor's animation's root velocity (channel kind 1) at its ASEGA's time (orig3 0x24F278), reversed
glm::vec3 VelAnimRoot(GURU* p)
{
    glm::vec4 v(0.0f);
    if (ASEGA* pasega = p->pasega; pasega && !FAsegaRetracted(pasega))
        if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
            pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
    return -glm::vec3(v);
}

// orig3 0x22C188 (the init)
void InitGuru(GURU* p)
{
    InitActorFields(p);  // 0x152218
    p->state = -1;
    // (Penelope, orig3 0x1EFE58: the footstep sound 0x95, height 125, +0x164 0x20, +0x1110 0x2080)
    const bool fPen = FIsLoDerivedFrom(p->palo, CID_PENELOPE);
    InitGait(&p->gaitWalk, p->palo, fPen ? 0x95 : 0x93);  // 0x258930 (+0x1114 = 0x93: the footstep sound)
    InitGait(&p->gaitRun, p->palo, fPen ? 0x95 : 0x93);
    p->sHeight = fPen ? 125.0f : 90.0f;   // +0x1158 (Murray 125)
    p->n1000 = fPen ? 0x20 : 8;           // +0x164
    // (+0x1170.. -100 / 100 / 50 / -10 / 250, +0x1184 0, +0x1188 1, +0x118C 4500 as every player; +0x1110 0x3080: the
    // ability flags; D_0046FF30 = him: PguruGlobal finds him by class)
}

// orig3 0x1E9330 (the Panda King's init): three gaits (footstep 0x94), height 125, +0x164 0x10, +0x1110 0x2080, the
// fireworks off with 4 charges at most and the targets kept 0.5 s; D_0038AE94 = him (natively found by class)
void InitPandaKing(GURU* p)
{
    InitActorFields(p);  // 0x152218
    p->state = -1;
    InitGait(&p->gaitWalk, p->palo, 0x94);
    InitGait(&p->gaitRun, p->palo, 0x94);
    InitGait(&p->gaitThird, p->palo, 0x94);
    p->sHeight = 125.0f;
    p->n1000 = 0x10;
    p->stateFw = -1;
    p->cChargeMax = 4;
    p->dtTargetKeep = 0.5f;
}

// orig3 0x1E9478 (his post-load): the parts, the three gaits (0x258958), the look on record 0x2E; the options' fields
void PostLoadPandaKing(GURU* p)
{
    ALO* palo = p->palo;
    PostLoadActorParts(p);   // 0x152488
    PostLoadActorMass(p);
    p->psmIdle = PPart<SM>(palo, 0x1028);   // (Sly 3 +0x1134, oid 451)
    BindJumpParts(p);
    for (GAIT* pg : {&p->gaitWalk, &p->gaitRun, &p->gaitThird})
    {
        const uint32_t ib = pg == &p->gaitWalk ? 0x1480u : pg == &p->gaitRun ? 0x1530u : 0x15E0u;
        pg->apaseg[0] = PPart<ASEG>(palo, FN3_TAG | (ib + 0x14));
        pg->apaseg[1] = PPart<ASEG>(palo, FN3_TAG | (ib + 0x54));
        BindGait(pg);
    }
    for (int i = 0; i < 4; ++i) p->apasegFw[i] = PPart<ASEG>(palo, FN3_TAG | (0x1690u + 4u * i));
    p->ploMarkerTmpl = PloPart(palo, FN3_TAG | 0x16F4u);
    p->ploFwCount = PloInteractOidField(palo, 0x16F8);   // (EOPIDs 1881 / 1882: objects by oid; 1886 / 1888 the most charges, the keep time)
    p->ploFwGlow = PloInteractOidField(palo, 0x16FC);
    p->cChargeMax = int(palo->U32Field(0, 0x16E8, uint32_t(p->cChargeMax)));
    const uint32_t uKeep = palo->U32Field(0, 0x16F0, 0);
    if (uKeep) std::memcpy(&p->dtTargetKeep, &uKeep, sizeof(float));
    AttachActorLook(p, 0x2E);   // 0x155190
    if (FDebug())
        std::printf("pk: post-load %d idle %d gaits %d %d %d fireworks clips %d %d %d %d marker %d count %d glow %d most %d\n", palo->oid,
                    p->psmIdle ? p->psmIdle->oid : -1, p->gaitWalk.pbl ? 1 : 0, p->gaitRun.pbl ? 1 : 0, p->gaitThird.pbl ? 1 : 0,
                    p->apasegFw[0] ? p->apasegFw[0]->oid : -1, p->apasegFw[1] ? p->apasegFw[1]->oid : -1, p->apasegFw[2] ? p->apasegFw[2]->oid : -1,
                    p->apasegFw[3] ? p->apasegFw[3]->oid : -1, p->ploMarkerTmpl ? p->ploMarkerTmpl->oid : -1, p->ploFwCount ? p->ploFwCount->oid : -1,
                    p->ploFwGlow ? p->ploFwGlow->oid : -1, p->cChargeMax);
    SetPkState(p, 0);
}

// orig3 0x22C2B0 (the post-load)
void PostLoadGuru(GURU* p)
{
    ALO* palo = p->palo;
    PostLoadActorParts(p);   // 0x152488
    PostLoadActorMass(p);
    p->psmIdle = PPart<SM>(palo, 0x1028);   // (Sly 3 +0x1134, oid 451)
    BindJumpParts(p);
    for (GAIT* pg : {&p->gaitWalk, &p->gaitRun})
    {
        const uint32_t ib = pg == &p->gaitWalk ? 0x1480u : 0x1530u;   // 0x258958
        pg->apaseg[0] = PPart<ASEG>(palo, FN3_TAG | (ib + 0x14));
        pg->apaseg[1] = PPart<ASEG>(palo, FN3_TAG | (ib + 0x54));
        BindGait(pg);
    }
    p->psmRide = PPart<SM>(palo, FN3_TAG | 0x15EC);
    if (!p->psmRide) p->psmRide = p->psmIdle;   // (the loader 0x22C260)
    AttachActorLook(p, 0x2E);   // 0x155190
    if (FDebug())
        std::printf("guru3: post-load %d idle %d gaits %d %d / %d %d (blends %p %p) ride %d jump %d\n", palo->oid, p->psmIdle ? p->psmIdle->oid : -1,
                    p->gaitWalk.apaseg[0] ? p->gaitWalk.apaseg[0]->oid : -1, p->gaitWalk.apaseg[1] ? p->gaitWalk.apaseg[1]->oid : -1,
                    p->gaitRun.apaseg[0] ? p->gaitRun.apaseg[0]->oid : -1, p->gaitRun.apaseg[1] ? p->gaitRun.apaseg[1]->oid : -1,
                    (void*)p->gaitWalk.pbl, (void*)p->gaitRun.pbl, p->psmRide ? p->psmRide->oid : -1, p->jump.apasegJump[0] ? p->jump.apasegJump[0]->oid : -1);
    // (+0x1190 = 0x23DDA0(p, 0x22, 1): a record of kind 0x22; not used by the ported states)
    SetGuruState3(p, 0);
}

// orig3 0x22C698 (the state machine, run until the state stays)
void UpdateGuruTransitions(GURU* p)
{
    for (int iter = 0; iter < 32; ++iter)
    {
        const int s = p->state;
        // the fall after 0.1 s off the floor (not in 3 / 5 / 6 / 8; in 4 unless the interaction holds it; in 7 when its
        // effect is over)
        bool fFall = true;
        switch (s)
        {
        case 3: case 5: case 6: case 8: fFall = false; break;
        case 7: fFall = FCombo3DefMoves(p); break;   // (or the combo's window +0x10F8: not kept natively)
        case 4: fFall = !(p->n4C8 == 1 || p->n4C8 == 12); break;   // (+0x508: the interaction's kind)
        default: break;
        }
        if (fFall && 0.1f < g_clock.t - p->tFloor) SetJumpKind(&p->jump, 0, 0, nullptr);   // 0x2503C8, 0x1B7268
        switch (p->state)
        {
        case 0: case 1: case 2:
        {
            if (FGuruBusy(p)) { SetGuruState3(p, 0); break; }
            if (FBtnTake(p, c_grfCross, 0.1f)) { SetJumpKind(&p->jump, 1, 0, nullptr); break; }   // (0x40)
            if (FBtnWithin(p, c_grfR3, 0.15f) && 0.1f < g_clock.t - p->tState && FGuruCanDo(p, 3))   // (0x400)
            {
                SetGuruState3(p, 0);
                ForgetBtn(p, c_grfR3);
                OpenBinoc(p, 0, nullptr, nullptr);   // 0x1BDA88 (+0xF60)
                break;
            }
            if (FBtnTake(p, c_grfCircle, 0.2f)) { FInteractCircle(p); break; }   // (0x20) 0x1B5170 (+0x500)
            const float u = UJoy(p);
            if ((p->state == 0 && 0.1f < u) || (p->state != 0 && 0.0f < u))
                SetGuruState3(p, ((GrfbtnHeld(p) & c_grfR1) || p->fWalk13E8) ? 1 : 2);   // (8)
            else SetGuruState3(p, 0);
            break;
        }
        case 3: UpdateJumpTransitions(&p->jump); break;       // 0x1B9020
        case 4: UpdateInteractTransitions(p); break;          // 0x1B27D8
        case 5: UpdateDamageTransitions(p); break;            // 0x1C2A18
        case 7:
            // the combo's state (orig3 0x22C698 case 7): with a definition that moves (0x1398F0: a clip while moving),
            // the stick runs (2) and Cross jumps; else he stays until the combo ends (combo3.cpp: kind 0)
            if (FCombo3DefMoves(p))
            {
                if (0.0f < UJoy(p)) SetGuruState3(p, 2);
                else if (FBtnTake(p, c_grfCross, 0.1f)) SetJumpKind(&p->jump, 1, 0, nullptr);
            }
            break;
        case 8:
            // a script's animation (+0x113C) or state machine (+0x1140) over: standing
            if (p->pasega ? FAsegaRetracted(p->pasega) : (p->psma && FSmaRetracted(p->psma))) SetGuruState3(p, 0);
            break;
        default: break;
        }
        // (vt slot 98: 0x139FC0, the actors' ability buttons: gadget.cpp's, run by UpdateActor)
        if (p->state == s) break;
    }
}

// orig3 0x22C308 (vt slot 18)
void UpdateGuru(LO* plo)
{
    GURU* p = PguruFromLo(plo);
    if (FDebug() && !p) std::printf("guru3: oid %d is no GURU (step %p)\n", plo->oid, (void*)PstepFromLo(plo));
    if (!p) return;
    UpdateActor(p);   // 0x152A48
    if (!plo->fInWorld) return;
    ApplyRouteTeleportActor(p);   // (debug: a route's teleport / tail step with the Guru as the current player)
    UpdateActorBinoc(p, 0x4F0);   // 0x22C500 (+0xF60 while busy)
    // (s3-player: the ride job's get-off, airide3.cpp's Triangle: the Guru's 0x22D318)
    g_pfnRiderOff3 = [](LO* ploRider) { GuruJumpOff3(ploRider); };
    // (debug: SLY3_GURURIDE=oid: from t 3 the Guru rides that guard, as after a jump onto its link (0x1BD520 kind 17))
    if (static const int s_oidRide = std::getenv("SLY3_GURURIDE") ? std::atoi(std::getenv("SLY3_GURURIDE")) : 0; s_oidRide && !p->ploRide && !p->ploRideLast &&
        g_clock.t > 3.0f && p->state >= 0 && p->state <= 2 && g_psw)
        for (const auto& pl : g_psw->aploAll)
            if (pl->oid == s_oidRide && pl->fInWorld && PguardFromLo(pl.get()))
            {
                p->ploRide = pl.get();
                SetGuruState3(p, 6);
                break;
            }
    if (static const bool s_fPad = std::getenv("SLY3_GURUPRINT") && std::atoi(std::getenv("SLY3_GURUPRINT")) >= 2; s_fPad)
        std::printf("guru3: pad %p busy %d nF04 %d held 0x%X\n", (void*)PpadctlOf(p), FGuruBusy(p) ? 1 : 0, p->nF04, PpadctlOf(p) ? GrfbtnHeld(p) : 0u);
    if (PpadctlOf(p)) UpdateGuruTransitions(p);   // 0x22C698
    switch (p->state)
    {
    case 0:
        if (p->psma) SetSmaMask(p->psma, GrfActorLookMask(p));   // (vt slot 110 0x155AF0: the idle's look mask)
        break;
    case 1:
        SetStepPhysHeading(p->palo, RadJoy(p));                   // 0x24E878 (+0x110C: the stick's heading)
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitWalk, 1.0f));   // 0x258CF0
        UpdateGaitFeet(&p->gaitWalk);                             // 0x258DA0
        break;
    case 2:
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitRun, UJoy(p)));
        UpdateGaitFeet(&p->gaitRun);
        break;
    case 3: UpdateJumpInput(&p->jump); break;   // 0x1B9C48
    case 4: UpdateInteract(p); break;           // 0x1B30E8
    case 5: UpdateDamage(p); break;             // 0x1C2B48
    case 8: p->vecVel = VelAnimRoot(p); break;  // 0x156AB0 / 0x24F278
    case 6:
        // (the records' controllers 0x130078 (mode 5) hold the Guru on the link: natively placed on it every frame)
        if (p->palo && p->palo->paloParent)
            if (const XF* pxf = PxfObj(p->palo->paloParent)) TeleportAloMat(p->palo, pxf->matWorld, nullptr);
        break;
    default: break;
    }
    // 0x22C570: the safe ground (states 0..2 / 7: the floor; 4: the interaction's)
    switch (p->state)
    {
    case 0: case 1: case 2: case 7:
        if (XP* pxp = p->pxpFloor; pxp && 0.7f < pxp->normal.z && !p->ploWater)
        {
            ALO* paloFloor = pxp->axpd[1].palo;
            if (!paloFloor || !(Sort(paloFloor).grf148 & 0x20000000ull)) RecordSafeGround(p, pxp->axpd[1].psoe, pxp->axpd[1].posLocal);
        }
        break;
    case 4: UpdateSafeInteract(p); break;
    default: break;
    }
    if (FDebug())
    {
        const glm::vec3 pos = PosAloCenter(p->palo);
        std::printf("guru3: t %.3f oid %d state %d pos %.1f %.1f %.1f u %.2f rad %.2f vel %.1f %.1f %.1f anim %d\n", g_clock.t, plo->oid, p->state,
                    pos.x, pos.y, pos.z, UJoy(p), RadJoy(p), p->vecVel.x, p->vecVel.y, p->vecVel.z,
                    p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -1);
    }
}

// orig3 0x22D3E8 (vt slot 74)
void UpdateGuruMotion(ALO* palo)
{
    UpdateStepPhysMotion(palo);   // 0x24E2B8
    GURU* p = PguruFromLo(palo);
    if (!p) return;
    switch (p->state)
    {
    case 3: UpdateJumpMotion(&p->jump); break;   // 0x1B88E0
    case 4: UpdateInteractMotion(p); break;      // 0x1B5910
    case 5: UpdateDamageMotion(p); break;        // 0x1C2870
    case 7: break;   // (0x13A9E0: only after 0.1 s in the air, the combo's steering in the air: not ported)
    default: break;
    }
}

// orig3 0x22D488 (vt slot 59, Sly 2's 55)
void OnGuruXpNew(ALO* palo, XP* pxp, int i)
{
    OnStepXpNew(palo, pxp, i);   // 0x24E400
    if (GURU* p = PguruFromLo(palo); p && p->state == 5) AdjustDamageXp(p, pxp);   // 0x1C3D18
}

// orig3 0x22D538 (vt slot 5): the base's, then a jump off the ridden object when it sends event 1 / 3 with its key
void OnGuruMessage(LO* plo, int msgk, void* pv)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return;
    HandleActorMessage(p, msgk, pv);   // 0x153738
    // (event 1 / 3 with the ridden object's key: 0x22D318; the ride (state 6) is not ported, so no object is ridden)
}

// orig3 0x22CF00 (vt slot 99, Sly 2's 88: the state of the jump's kind n)
void SetGuruJumpState(LO* plo, int n)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return;
    switch (n)
    {
    case -1: SetGuruState3(p, -1); break;
    case 0: SetGuruState3(p, 0); break;
    case 1: SetGuruState3(p, 3); break;
    case 2: SetGuruState3(p, 4); break;
    case 3: SetGuruState3(p, 5); break;
    case 9: SetGuruState3(p, 7); break;
    case 5: SetGuruState3(p, 8); break;
    // (Penelope's 0x1F0CE0: kind 6 her state 10, a script's animation with the gait's feet: here as 5's; kind 10 her
    // state 3 (not ported))
    case 6: if (FIsLoDerivedFrom(plo, CID_PENELOPE)) SetGuruState3(p, 8); break;
    default: break;
    }
}

// orig3 0x22CFA8 (vt slot 100, Sly 2's 89: the jump kind of the state)
int GuruJumpKind(LO* plo)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return -1;
    switch (p->state)
    {
    case -1: return -1;
    case 0: return 0;
    case 3: return 1;
    case 4: return 2;
    case 5: return 3;
    case 7: return 9;
    case 8: return 5;
    default: return 0xB;
    }
}

// orig3 0x22D130 (vt slot 65, Sly 2's 61: the overlap kind with another object): the actors' (0x156948), and no
// contact (3) with the guard he rides (+0x15E0's link: its guard, 0x1C8F20 an ancestor of the object) or the one he
// just left (+0x15F0). Natively the function map gave him Murray's carry (0x1B8450): the ridden guard's first step
// pushed him (contact kind 0x200, a hit) and knocked him off at once (m2_shaman's 2669: the ride never got to the gate)

int GuruOverlapKind(ALO* palo, ALO* pOther)
{
    int kind = ::OverlapKindActor(palo, pOther);
    GURU* p = PguruFromLo(palo);
    if (!p) return kind;
    for (LO* plink : {p->ploRide, p->ploRideLast})
    {
        LO* pg = plink ? PloRideGuard3(plink) : nullptr;
        if (!pg) continue;
        for (LO* q = pOther; q; q = q->paloParent)
            if (q == pg) { kind = std::max(kind, 3); break; }
    }
    return kind;
}

// ------------------------------------------------------------------------------------------------------------------
// The Panda King (Sly 3 class 0x38, vtable 0x35E930: the Guru's layout, checked word by word: AUDIT4 PLAYER-01). His
// own functions: 0x1E9330 init, 0x1E9438 loader (loaders.cpp), 0x1E9478 post-load, 0x1E9500 update (0x1E9798 the
// transitions, 0x1E9BC8 the state, 0x1E9678 the binocucom, 0x1E96E8 the safe ground), 0x1EA2F8 motion, 0x1EA3F8
// messages, 0x1EA398 contacts, 0x1EA298 the camera's, 0x1E9F48 / 0x1E9FF0 the jump kinds, 0x1EA0C8 the step mesh,
// 0x1EA178 the reset, 0x1EA1B8 / 0x1EA060 / 0x1EA070 / 0x1EA478 / 0x1EA1E0 the queries, 0x1E94D8, 0x1F54E0 (menu.cpp).
// The fireworks (+0x1690..+0x1700): 0x1EA4B0 their state, 0x1EA750 their update, 0x1EA940, 0x1EB030 / 0x1EA9A8 /
// 0x1EA9B8 the script's (EOPIDs 1884 / 1889 / 1890), 0x1EB118 a target (1883), 0x1EB300 / 0x1EB340 the HUD.
// 0x1EAB40 / 0x1EAA00 the targets in the camera's view and the glow; 0x1EA178 slot 102 (the reset).
// ------------------------------------------------------------------------------------------------------------------
bool FFwDebug() { static const bool s_f = std::getenv("SLY3_PKPRINT") != nullptr; return s_f; }

// orig3 0x1EA478 (vt slot 111): the look mask 0x2000 while the fireworks run, else the actors' (0x155B88)
uint32_t GrfPkLookMask(GURU* p) { return p->stateFw != -1 ? 0x2000u : GrfActorLookMask(p); }

// orig3 0x1EA1E0 (vt slot 113, may the action n start): the base's (0x154518: approximated by the busy test, as the
// Guru's), in the jump (4) / interaction (5) their sub-objects' say (0x1BD830 / 0x1B5D08: not ported, refused), in
// 0..3; only the actions 2 and 3 (the binocucom)
bool FPkCanDo(GURU* p, int n)
{
    if (FGuruBusy(p)) return false;
    if (!(0 <= p->state && p->state <= 3)) return false;
    return n == 2 || n == 3;
}

// orig3 0x1EA940: the fireworks may go off: charging (0 / 1) with a charge and a pad (0x1F4618), always when fired (2)
bool FPkFwCanFire(GURU* p)
{
    switch (p->stateFw)
    {
    case 0: case 1: return p->cCharge > 0 && PpadctlOf(p) != nullptr;
    case 2: return true;
    default: return false;
    }
}

// orig3 0x1EA4B0: the fireworks' state
void SetPkFwState(GURU* p, int n)
{
    if (p->stateFw == n) return;
    switch (p->stateFw)
    {
    case -1:
        ActorSetLookFlag(p, 1);   // 0x155310
        if (p->ploFwCount) { ShowScreenObj(p->ploFwCount, -1); StartScreenObjPart(p->ploFwCount, 0x3CA); }   // slot 56, 0x262450
        if (p->ploFwGlow)
        {
            ShowScreenObj(p->ploFwGlow, -1);
            StartScreenObjPart(p->ploFwGlow, 0x37C);
            p->uFwGlow = 0.0f;
            SetScreenObjFraction(p->ploFwGlow, 0x37C, p->uFwGlow);   // 0x261580
        }
        break;
    case 1: TriggerJoyRumbleRumk(0, 0.1f, "panda king"); break;   // 0x1A9058(the player's pad, 0, 0.1)
    default: break;
    }
    p->stateFw = n;
    p->tStateFw = g_clock.t;
    ASEG* paseg = nullptr;
    int grfapl = 1;
    switch (n)
    {
    case -1:
        ActorSetLookFlag(p, 0);   // 0x155308
        ActorNextLookTime(p);     // 0x155328
        if (p->ploFwCount) HideScreenObj(p->ploFwCount);   // slot 54
        if (p->ploFwGlow) HideScreenObj(p->ploFwGlow);
        break;
    case 0: case 1:
        grfapl = 9;
        paseg = p->apasegFw[n];
        if (n == 1) TriggerJoyRumbleRumk(0, 1.0f, "panda king");
        break;
    case 2: case 3: paseg = p->apasegFw[n]; break;
    default: break;
    }
    ApplyAsegCur(paseg, p->palo, 0.0f, 1.0f, grfapl, &p->pasegaFw);   // 0x137588
    if (FFwDebug()) std::printf("pk: t %.3f fireworks %d (charges %d / %d, targets %d)\n", g_clock.t, n, p->cCharge, p->cChargeMax, p->ctarget);
    if (n == 0 || n == 1) return;
    // (-1 / 2 / 3: the markers out of the world (slot 9), the targets and the charges dropped)
    for (int i = 0; i < p->ctarget; ++i)
        if (LO* pm = p->atarget[i].ploMarker) RemoveLo(g_psw, pm);
    p->cCharge = 0;
    p->ctarget = 0;
}

// orig3 0x1EB118 (EOPID 1883's method 0x110788): the object a target (up to 4; a guard 0x3A takes the place of the
// oldest non-guard), a marker made from the template +0x16F4 (0x1EB058) following it, the head looking at it
LO* PloNewFwMarker(GURU* p)
{
    LO* pt = p->ploMarkerTmpl;
    if (!pt || !g_psw) return nullptr;
    LO* pm = PloProxyFreeRow(g_psw, pt, 0);   // 0x1EB058: the proxy's free copy (0x1FBE30), unparented, into the world
    if (!pm || !FIsLoDerivedFrom(pm, CID_ALO)) return nullptr;
    SetAloParent(static_cast<ALO*>(pm), nullptr, nullptr, 0);   // 0x1C7A00(copy, 0, 0)
    AddLo(g_psw, pm);                                            // (its slot 8)
    return pm;
}
void AddPkFwTarget(GURU* p, LO* ploOwner)
{
    if (!ploOwner) return;
    if (p->ctarget < 4)
    {
        LO* pm = PloNewFwMarker(p);
        if (!pm) return;
        p->atarget[p->ctarget++] = {ploOwner, pm, g_clock.t};
        SetActorLookTarget(p, 4, ploOwner);   // 0x1552D8
        return;
    }
    if (!FIsLoDerivedFrom(ploOwner, CID_GUARD)) return;
    for (int i = 0; i < p->ctarget; ++i)
        if (!FIsLoDerivedFrom(p->atarget[i].ploOwner, CID_GUARD))
        {
            LO* pm = p->atarget[i].ploMarker;
            for (int j = i; j + 1 < p->ctarget; ++j) p->atarget[j] = p->atarget[j + 1];
            p->atarget[p->ctarget - 1] = {ploOwner, pm, g_clock.t};
            SetActorLookTarget(p, 4, ploOwner);
            return;
        }
}

// orig3 0x1F4A80: the camera of the player's object (its own view in split screen, else the world's)
const CM& CmOfPk(GURU* p)
{
    if (split3::FActive() && g_psw)
        for (int i = 0; i < split3::CView(); ++i)
            if (split3::PloPlayer(g_psw, i) == p->palo) return split3::CmView(i);
    return g_cm;
}

// orig3 0x1EAF80: an object may be a fireworks target: in the world (+0x28 bit 56), not the Panda King's own (0x1C8F20),
// its root not a knocked-out guard (0x3A's +0x360: native f330, aihit.cpp JOBHIT3) nor a pickup (0x66), its parent not
// a pickup
bool FPkFwTargetOk(GURU* p, LO* plo)
{
    if (!plo || !plo->fInWorld) return false;
    LO* root = plo;
    for (LO* q = plo; q; q = q->paloParent)
    {
        if (q == p->palo) return false;
        root = q;
    }
    if (FIsLoDerivedFrom(root, CID_GUARD))
    {
        if (const GUARD* pg = PguardFromLo(root); pg && pg->f330) return false;
    }
    else if (FIsLoDerivedFrom(root, 0x66)) return false;
    if (plo->paloParent && FIsLoDerivedFrom(plo->paloParent, 0x66)) return false;
    return true;
}

// orig3 0x1EAA00: the glow (+0x1700) raised by how near a target is to the middle of the camera's view: the screen's x
// (0x1678C8) against a radius by the distance (D_00324D90 {0.4, -0.00012, 0} kept in D_00324DA0 {0.08, 0.4}), eased by
// D_00341FF0 {0, 1}; *pdepth the screen x past that radius
void RaisePkFwGlow(GURU* p, const glm::vec3& pos, float* pdepth)
{
    const CM& cm = CmOfPk(p);
    const glm::vec3 q = ConvertCmWorldToScreen(&cm, pos);
    const float d = glm::length(pos - cm.pos);
    const float r = std::clamp(0.4f + d * (-0.00012f + d * 0.0f), 0.08f, 0.4f);
    const float depth = std::fabs(q.x) - r;
    const float u = 1.0f - std::clamp(depth / (r + r), 0.0f, 1.0f);
    if (pdepth) *pdepth = depth;
    p->uFwGlow = std::max(p->uFwGlow, u);
}

// orig3 0x1EAB40: the targets refreshed: one whose owner may no longer be a target is dropped with its marker (slot 9),
// as is one kept less than +0x16F0 that left the view (0x167F60, margin 50); the others raise the glow. Then the
// world's interest points (class 0x11, the list 0x479AD8) with any of the flags 0x200017E (+0x18D8, EOPID 249) in the
// camera's view (their radius +0xDC, EOPID 250) and near its middle become targets (0x1EB118) unless all four sight
// rays (the point, above, below, to the side, from the camera) are blocked; breakables are skipped while a guard
// is in reach (0x153290)
void RefreshPkFwTargets(GURU* p)
{
    const CM& cm = CmOfPk(p);
    const glm::vec3 posCm = cm.pos;
    p->uFwGlow = 0.0f;
    for (int i = 0; i < p->ctarget;)
    {
        GURU::FWTARGET& e = p->atarget[i];
        bool fStale = !FPkFwTargetOk(p, e.ploOwner);
        if (!fStale && g_clock.t - e.t < p->dtTargetKeep)
            fStale = !FInsideCmPlanes(&cm, PosLoWorld(e.ploOwner) - posCm, 50.0f);
        if (fStale)
        {
            if (LO* pm = e.ploMarker) RemoveLo(g_psw, pm);   // (slot 9)
            for (int j = i; j + 1 < p->ctarget; ++j) p->atarget[j] = p->atarget[j + 1];
            --p->ctarget;
            continue;
        }
        RaisePkFwGlow(p, PosLoWorld(e.ploOwner), nullptr);
        ++i;
    }
    if (!g_psw) return;
    const bool fGuardNear = FActorTargetInReach(p, false);   // 0x153290(p, 0)
    std::vector<SOE*> apsoe;
    bool fEntries = false;
    for (LO* plo : g_psw->mpcidaplo[0x11])
    {
        if (!plo) continue;
        uint32_t grf = plo->U32Field(0, 0xD8, 0);
        float sRadius = 2000.0f;   // (Sly 2's +0xE8 default, natively the look's range: sly.cpp UpdateActorLook)
        if (const uint32_t u = plo->U32Field(0, 0xE8, 0)) std::memcpy(&sRadius, &u, sizeof sRadius);
        for (const OPT& o : plo->aopt)
        {
            if (o.eopid == 249 && !o.aval.empty()) grf = uint32_t(o.aval[0].n);
            if (o.eopid == 250 && !o.aval.empty()) sRadius = o.aval[0].ag[0];
        }
        if (!(grf & 0x200017E) || !FPkFwTargetOk(p, plo)) continue;
        LO* ploFirst = plo->paloParent ? static_cast<LO*>(plo->paloParent) : plo;   // (+0x14's first: the point's object)
        if (fGuardNear && FIsLoDerivedFrom(ploFirst, 0x6F)) continue;
        bool fFound = false;
        for (int j = 0; j < p->ctarget; ++j) fFound |= p->atarget[j].ploOwner == plo;
        if (fFound) continue;
        const glm::vec3 pos = PosLoWorld(plo);
        if (!FInsideCmPlanes(&cm, pos - posCm, sRadius)) continue;
        float depth = 0.0f;
        RaisePkFwGlow(p, pos, &depth);
        if (0.0f < depth) continue;
        if (!fEntries)
        {
            // 0x25CF18: the active entries (those with a camera hull, as the photo's sight rays)
            FlushSwOxaDirty1();
            UpdateSwAaox();
            for (OXA* box : g_physw.apoxaActive)
                for (SOE* entry : box->apsoe)
                    if (!entry->f48 && !entry->bsp.abspn.empty() && 0 < static_cast<int>(entry->bsp.x8) && apsoe.size() < 0x100)
                        apsoe.push_back(entry);
            fEntries = true;
        }
        const glm::vec3 dn = glm::normalize(posCm - pos);
        const glm::vec3 up = cm.mat[1];
        const glm::vec3 side = glm::normalize(glm::cross(up, dn));
        const glm::vec3 center = pos + dn * sRadius;
        const glm::vec3 apos[4] = {center, center + up * sRadius, center - up * sRadius, center + side * sRadius};
        int cHit = 0;
        for (const glm::vec3& q : apos)
        {
            LSG hit;
            if (PsoeHitTestLineObjects(0, posCm, q, apsoe, &hit)) ++cHit;   // 0x165000
        }
        if (cHit == 4) continue;
        if (FFwDebug()) std::printf("pk: t %.3f fireworks target %d (flags 0x%X, radius %.0f, %d rays blocked)\n", g_clock.t, plo->oid, grf, sRadius, cHit);
        AddPkFwTarget(p, plo);   // 0x1EB118
    }
}

// orig3 0x1EA178 (vt slot 102): the combo off (0x155CE0), then the fireworks fired when they may be (0x1EA940), else off
void ResetPkSelection(GURU* p)
{
    StopCombo3(p);
    SetPkFwState(p, FPkFwCanFire(p) ? 2 : -1);
}

// orig3 0x1EA750: the fireworks' state machine, the targets (0x1EAB40 in the states 0 / 1, else the glow 0), the
// markers on their owners (slot 34 at the owner's slot 36 position), the HUD
void UpdatePkFw(GURU* p)
{
    const bool fHeld = p->fFwHeld || (GrfbtnHeld(p) & c_grfL1);
    for (int iter = 0; iter < 8; ++iter)
    {
        const int s = p->stateFw;
        switch (s)
        {
        case 0:
            if (fHeld) { if (p->cCharge >= p->cChargeMax) SetPkFwState(p, 1); }
            else SetPkFwState(p, FPkFwCanFire(p) ? 2 : -1);
            break;
        case 1:
            if (!fHeld) SetPkFwState(p, FPkFwCanFire(p) ? 2 : -1);
            else if (1.0f < g_clock.t - p->tStateFw) SetPkFwState(p, 3);
            break;
        case 2: case 3:
            if (!p->pasegaFw || FAsegaRetracted(p->pasegaFw)) SetPkFwState(p, -1);   // 0x138AC8
            break;
        default: break;
        }
        if (p->stateFw == s) break;
    }
    if (p->stateFw == 0 || p->stateFw == 1) RefreshPkFwTargets(p);   // 0x1EAB40
    else p->uFwGlow = 0.0f;
    for (int i = 0; i < p->ctarget; ++i)
        if (LO* pm = p->atarget[i].ploMarker; pm && p->atarget[i].ploOwner && FIsLoDerivedFrom(pm, CID_ALO))
            SetAloPos(static_cast<ALO*>(pm), PosLoWorld(p->atarget[i].ploOwner));   // slot 34 (the owner's slot 36 row 3)
    if (p->ploFwCount && p->cChargeMax > 0)
        SetScreenObjFraction(p->ploFwCount, 0x3CA, float(p->cCharge) / float(p->cChargeMax));   // 0x1EB300 (0x261528(count, most))
    if (p->ploFwGlow) SetScreenObjFraction(p->ploFwGlow, 0x37C, p->uFwGlow);                    // 0x1EB340
}

// orig3 0x1E9BC8
void SetPkState(GURU* p, int n)
{
    if (n == p->state) return;
    switch (p->state)
    {
    case 0:
        SetXfModes(p, 2);
        if (p->stateFw == -1) ActorNextLookTime(p);   // 0x155328
        break;
    case 4: SetJumpAnimState(&p->jump, -1, nullptr); break;   // 0x1B77F8
    case 5: SetInteractState(p, -1); break;                    // 0x1B4430
    case 6: SetDamageState(p, -1); break;                      // 0x1C21F0
    case 1: case 2: case 3: p->uPhase = p->pasega && p->pasega->paseg ? p->pasega->t / PasegdOf(p->pasega->paseg)->tMax : p->uPhase; break;
    case 8: SetXfModes(p, 2); break;
    default: break;
    }
    const int old = p->state;
    p->state = n;
    p->tState = g_clock.t;
    ASEG* paseg = nullptr;
    SM* psm = nullptr;
    int grfapl = 1;
    OID oid = -1;
    uint32_t grfMask = 4;
    float t = 0.0f;
    switch (n)
    {
    case 0:
        SetXfModes(p, 1);
        psm = p->psmIdle;
        oid = old == 2 ? 0x193 : old == 4 ? 0x195 : old == 7 ? 0x197 : 0x192;
        if (!psm || IsmsFindSm(psm, oid) < 0) oid = -1;   // 0x239A78
        p->vecVel = glm::vec3(0.0f);
        grfMask = GrfPkLookMask(p);   // vt slot 111 (0x1EA478)
        ActorSetLookFlag(p, 1);       // 0x155310
        break;
    case 1: case 2: case 3:
    {
        GAIT* pg = n == 1 ? &p->gaitWalk : n == 2 ? &p->gaitRun : &p->gaitThird;
        paseg = pg->pbl;
        if (paseg) t = p->uPhase * PasegdOf(paseg)->tMax;
        ResetGaitFeet(pg);   // 0x258F28
        grfapl = 9;
        break;
    }
    case 6: SetBinocState(p, -1); break;   // 0x1BDBB0 (+0xF60)
    case 8:
        SetXfModes(p, 1);
        p->vecVel = glm::vec3(0.0f);
        break;
    case 7: p->vecVel = glm::vec3(0.0f); break;
    default: break;   // (4 / 5: the jump's / interaction's own animations after)
    }
    // (any state but the ground's (0..4): the fireworks go off or stop)
    if (!(0 <= p->state && p->state <= 4)) SetPkFwState(p, FPkFwCanFire(p) ? 2 : -1);
    SetActorAnimPp(&p->pasega, &p->psma, p->palo, paseg, t, 1.0f, grfapl, psm, oid, grfMask, true);
    UpdateActorStepMesh(p);   // vt slot 101 (0x1EA0C8)
}

// orig3 0x1E9798 (the state machine, run until the state stays)
void UpdatePkTransitions(GURU* p)
{
    for (int iter = 0; iter < 32; ++iter)
    {
        const int s = p->state;
        bool fFall = true;
        switch (s)
        {
        case 4: case 6: case 8: fFall = false; break;
        case 7: fFall = FCombo3DefMoves(p); break;   // (or the combo's window +0x10F8)
        case 5: fFall = !(p->n4C8 == 1 || p->n4C8 == 12); break;
        default: break;
        }
        if (fFall && 0.1f < g_clock.t - p->tFloor) SetJumpKind(&p->jump, 0, 0, nullptr);   // 0x2503C8, 0x1B7268
        switch (p->state)
        {
        case 0: case 1: case 2: case 3:
        {
            if (FGuruBusy(p)) { SetPkState(p, 0); break; }
            if (FBtnTake(p, c_grfCross, 0.1f)) { SetJumpKind(&p->jump, 1, 0, nullptr); break; }
            if (FBtnWithin(p, c_grfR3, 0.15f) && 0.1f < g_clock.t - p->tState && FPkCanDo(p, 3))
            {
                SetPkState(p, 0);
                ForgetBtn(p, c_grfR3);
                OpenBinoc(p, 0, nullptr, nullptr);   // 0x1BDA88
                break;
            }
            if (FBtnTake(p, c_grfCircle, 0.2f)) { FInteractCircle(p); break; }   // 0x1B5170
            if (p->stateFw == -1 && (p->fFwHeld || (GrfbtnHeld(p) & c_grfL1))) { SetPkFwState(p, 0); break; }   // (L1: 4)
            const float u = UJoy(p);
            if ((p->state == 0 && 0.1f < u) || (p->state != 0 && 0.0f < u))
            {
                if (static_cast<unsigned>(p->stateFw) < 2) SetPkState(p, 3);
                else SetPkState(p, ((GrfbtnHeld(p) & c_grfR1) || p->fWalk13E8) ? 1 : 2);
                break;
            }
            SetPkState(p, 0);
            break;
        }
        case 4: UpdateJumpTransitions(&p->jump); break;   // 0x1B9020
        case 5: UpdateInteractTransitions(p); break;      // 0x1B27D8
        case 6: UpdateDamageTransitions(p); break;        // 0x1C2A18
        case 7:
            if (FCombo3DefMoves(p))
            {
                if (0.0f < UJoy(p)) SetPkState(p, 2);
                else if (FBtnTake(p, c_grfCross, 0.1f)) SetJumpKind(&p->jump, 1, 0, nullptr);
            }
            break;
        case 8:
            if (p->pasega ? FAsegaRetracted(p->pasega) : (p->psma && FSmaRetracted(p->psma))) SetPkState(p, 0);
            break;
        default: break;
        }
        if (p->state == s) break;
    }
}

// orig3 0x1E9500 (vt slot 18)
void UpdatePandaKing(LO* plo)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return;
    UpdateActor(p);   // 0x152A48
    if (!plo->fInWorld) return;
    ApplyRouteTeleportActor(p);
    UpdateActorBinoc(p, 0x4F0);   // 0x1E9678
    if (PpadctlOf(p)) UpdatePkTransitions(p);   // 0x1E9798
    UpdatePkFw(p);                // 0x1EA750
    switch (p->state)
    {
    case 0:
        if (p->psma) SetSmaMask(p->psma, GrfActorLookMask(p));   // (vt slot 110 0x155AF0)
        break;
    case 1:
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(&p->gaitWalk, 1.0f));
        UpdateGaitFeet(&p->gaitWalk);
        break;
    case 2: case 3:
    {
        GAIT* pg = p->state == 2 ? &p->gaitRun : &p->gaitThird;
        SetStepPhysHeading(p->palo, RadJoy(p));
        p->vecVel = glm::vec3(SetGaitMix(pg, UJoy(p)));
        UpdateGaitFeet(pg);
        break;
    }
    case 4: UpdateJumpInput(&p->jump); break;   // 0x1B9C48
    case 5: UpdateInteract(p); break;           // 0x1B30E8
    case 6: UpdateDamage(p); break;             // 0x1C2B48
    case 8: p->vecVel = VelAnimRoot(p); break;  // 0x156AB0 / 0x24F278
    default: break;
    }
    // 0x1E96E8: the safe ground (states 0..3 / 7: the floor; 5: the interaction's)
    switch (p->state)
    {
    case 0: case 1: case 2: case 3: case 7:
        if (XP* pxp = p->pxpFloor; pxp && 0.7f < pxp->normal.z && !p->ploWater)
        {
            ALO* paloFloor = pxp->axpd[1].palo;
            if (!paloFloor || !(Sort(paloFloor).grf148 & 0x20000000ull)) RecordSafeGround(p, pxp->axpd[1].psoe, pxp->axpd[1].posLocal);
        }
        break;
    case 5: UpdateSafeInteract(p); break;
    default: break;
    }
    if (FDebug())
    {
        const glm::vec3 pos = PosAloCenter(p->palo);
        std::printf("pk: t %.3f oid %d state %d fw %d pos %.1f %.1f %.1f u %.2f anim %d\n", g_clock.t, plo->oid, p->state, p->stateFw,
                    pos.x, pos.y, pos.z, UJoy(p), p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -1);
    }
}

// orig3 0x1EA2F8 (vt slot 74)
void UpdatePkMotion(ALO* palo)
{
    UpdateStepPhysMotion(palo);   // 0x24E2B8
    GURU* p = PguruFromLo(palo);
    if (!p) return;
    switch (p->state)
    {
    case 4: UpdateJumpMotion(&p->jump); break;   // 0x1B88E0
    case 5: UpdateInteractMotion(p); break;      // 0x1B5910
    case 6: UpdateDamageMotion(p); break;        // 0x1C2870
    default: break;   // (7: 0x13A9E0, the combo's steering in the air: not ported, as the Guru's)
    }
}

// orig3 0x1EA398 (vt slot 59)
void OnPkXpNew(ALO* palo, XP* pxp, int i)
{
    OnStepXpNew(palo, pxp, i);   // 0x24E400
    if (GURU* p = PguruFromLo(palo); p && p->state == 6) AdjustDamageXp(p, pxp);   // 0x1C3D18
}

// orig3 0x1EA3F8 (vt slot 5): the base's; the fireworks' clip's event 0x704 a charge (up to +0x16E8)
void OnPkMessage(LO* plo, int msgk, void* pv)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return;
    HandleActorMessage(p, msgk, pv);   // 0x153738
    if (msgk == 9 && pv)   // (Sly 3's message 10: the ASEG events)
    {
        const ASEGMSG9* pmsg = static_cast<const ASEGMSG9*>(pv);
        if (pmsg->pasega && pmsg->pasega == p->pasegaFw && pmsg->n == 0x704) p->cCharge = std::min(p->cCharge + 1, p->cChargeMax);
    }
}

// orig3 0x1E9F48 (vt slot 99) / 0x1E9FF0 (slot 100)
void SetPkJumpState(LO* plo, int n)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return;
    switch (n)
    {
    case -1: SetPkState(p, -1); break;
    case 0: SetPkState(p, 0); break;
    case 1: SetPkState(p, 4); break;
    case 2: SetPkState(p, 5); break;
    case 3: SetPkState(p, 6); break;
    case 9: SetPkState(p, 7); break;
    case 5: SetPkState(p, 8); break;
    default: break;
    }
}
int PkJumpKind(LO* plo)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return -1;
    switch (p->state)
    {
    case -1: return -1;
    case 0: return 0;
    case 4: return 1;
    case 5: return 2;
    case 6: return 3;
    case 7: return 9;
    case 8: return 5;
    default: return 11;
    }
}


struct INITGURU
{
    INITGURU()
    {
        RegisterLoUpdate(FN3_TAG | 0x1E9500, UpdatePandaKing);   // (the Panda King's 0x1E9500)
        // (his options: 1883 a target (0x110788 -> 0x1EB118), 1884 the fireworks off for good (0x110810 -> 0x1EB030: +0x16A8
        // 4), 1889 / 1890 a script's hold on / let go (0x110888 -> 0x1EA9A8, 0x110900 -> 0x1EA9B8: fired with a charge),
        // 1886 / 1887 / 1888 the most charges, the charges, the keep time)
        RegisterObjectMethod3(0x110788, [](LO* plo, const std::vector<CRef>& a) {
            if (GURU* p = PguruFromLo(plo); p && FIsPandaKing3(plo)) AddPkFwTarget(p, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x110810, [](LO* plo, const std::vector<CRef>&) {
            if (GURU* p = PguruFromLo(plo); p && FIsPandaKing3(plo)) { p->stateFw = 4; SetPkFwState(p, -1); }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x110888, [](LO* plo, const std::vector<CRef>&) {
            if (GURU* p = PguruFromLo(plo); p && FIsPandaKing3(plo)) p->fFwHeld = 1;
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x110900, [](LO* plo, const std::vector<CRef>&) {
            if (GURU* p = PguruFromLo(plo); p && FIsPandaKing3(plo))
            {
                p->fFwHeld = 0;
                if ((p->stateFw == 0 || p->stateFw == 1) && p->cCharge > 0) SetPkFwState(p, 2);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectSetter3(1886, [](LO* plo, const CRef& r) {
            if (GURU* p = PguruFromLo(plo); p && FIsPandaKing3(plo)) p->cChargeMax = r.RefCoerceS32().m_n;
        });
        RegisterObjectSetter3(1887, [](LO* plo, const CRef& r) {
            if (GURU* p = PguruFromLo(plo); p && FIsPandaKing3(plo)) p->cCharge = r.RefCoerceS32().m_n;
        });
        RegisterObjectGetter3(1887, [](LO* plo) {
            CRef r;
            GURU* p = PguruFromLo(plo);
            r.SetS32(p && FIsPandaKing3(plo) ? p->cCharge : 0);
            return r;
        });
        RegisterObjectSetter3(1888, [](LO* plo, const CRef& r) {
            if (GURU* p = PguruFromLo(plo); p && FIsPandaKing3(plo)) p->dtTargetKeep = r.m_tagk == TAGK_F32 ? r.m_g : float(r.RefCoerceS32().m_n);
        });
        RegisterLoUpdate(FN3_TAG | 0x22C308, UpdateGuru);
        RegisterLoUpdate(FN3_TAG | 0x1EFFD8, UpdateGuru);   // (Penelope's 0x1EFFD8: the same states)
    }
} s_initguru;
}  // namespace

void ActorSlot102Sly3(ACTOR* pa)
{
    if (!pa) return;
    if (FIsPandaKing3(pa->palo))
        if (GURU* p = PguruFromLo(pa->palo)) { ResetPkSelection(p); return; }
    StopCombo3(pa);   // 0x155CE0
}

NATIVEFN3(0x22D3E8, UpdateGuruMotion);
NATIVEFN3(0x1F1080, UpdateGuruMotion);   // (Penelope's: jump / interaction / damage / combo by her numbers 5..8)
NATIVEFN3(0x1F1120, OnGuruXpNew);
NATIVEFN3(0x1F0CE0, SetGuruJumpState);
NATIVEFN3(0x1F0DA8, GuruJumpKind);
NATIVEFN3(0x22D488, OnGuruXpNew);
NATIVEFN3(0x22D538, OnGuruMessage);
NATIVEFN3(0x22CF00, SetGuruJumpState);
NATIVEFN3(0x22D130, GuruOverlapKind);
NATIVEFN3(0x22CFA8, GuruJumpKind);
// (the Panda King's, AUDIT4 PLAYER-01)
NATIVEFN3(0x1EA2F8, UpdatePkMotion);
NATIVEFN3(0x1EA398, OnPkXpNew);
NATIVEFN3(0x1EA3F8, OnPkMessage);
NATIVEFN3(0x1E9F48, SetPkJumpState);
NATIVEFN3(0x1E9FF0, PkJumpKind);

bool FIsGuru3(const LO* plo) { return plo && FIsLoDerivedFrom(plo, CID_GURU); }
bool FIsPandaKing3(const LO* plo) { return plo && FIsLoDerivedFrom(plo, CID_PANDA_KING); }

bool FGuruCanDo3(LO* plo, int n)
{
    GURU* p = PguruFromLo(plo);
    if (!p) return false;
    return FIsPandaKing3(plo) ? FPkCanDo(p, n) : FGuruCanDo(p, n);   // (0x1EA1E0 / 0x22D1F8)
}

// orig3 slot 112 (the noise): 0x1530F8 first (the pads paused: 0; a noisy combo running: 1), then the walk (state 1):
// the Guru's 0x22D028, the Panda King's 0x1EA070, Penelope's 0x1F0E38 (her 1 / 4; she runs on the Guru's numbers here:
// 1 only). -2: not one of them
int NGuruNoisy3(ACTOR* pa)
{
    GURU* p = pa ? PguruFromLo(pa->palo) : nullptr;
    if (!p) return -2;
    if (FPlayersPausedSel3(int(p->palo->U32Field(0, 0x130, 0xFFFFFFFFu)))) return 0;
    if (FCombo3Noisy(p)) return 1;
    return p->state == 1 ? 1 : -1;
}

// orig3 slot 85, the Panda King's 0x1EA298: before the actors' 0x153D40, the last move time (+0x1154) now when he is
// not standing and his interaction's sub-state (+0x50C) is not 0; after it the tilt +0.22 and the distance +100
void GuruCpdefi3(LO* plo, float* pdTilt, float* pdDist, bool fBefore)
{
    GURU* p = FIsPandaKing3(plo) ? PguruFromLo(plo) : nullptr;
    if (!p) return;
    if (fBefore) return;
    if (pdTilt) *pdTilt = 0.22f;
    if (pdDist) *pdDist = 100.0f;
}
bool FPkMovedNow3(LO* plo)
{
    GURU* p = FIsPandaKing3(plo) ? PguruFromLo(plo) : nullptr;
    return p && p->state != 0 && p->nSub4CC != 0;
}

bool FActorTargetsRideLinks3(const ACTOR* pa) { return pa && FIsGuru3(pa->palo); }

const std::vector<LO*>& AploRideLinks3(SW* psw)
{
    static std::vector<LO*> s_aplo;
    s_aplo.clear();
    if (!psw) return s_aplo;
    for (LO* plo : psw->mpcidaplo[0x7A])
        if (plo && plo->fInWorld && FIsLoDerivedFrom(plo, CID_ALO)) s_aplo.push_back(plo);
    return s_aplo;
}

LO* PloLinkRider3(LO* plink)
{
    LO* ploGuard = PloRideGuard3(plink);
    GUARD* pg = ploGuard && ploGuard != plink ? PguardFromLo(ploGuard) : nullptr;
    return pg ? PloGuardRider3(pg) : nullptr;
}

void ArriveRideLink3(LO* plink)
{
    if (!g_psw || !plink) return;
    for (LO* plo : g_psw->mpcidaplo[CID_GURU])
        if (GURU* p = PguruFromLo(plo))
        {
            if (std::getenv("SLY3_GURUPRINT")) std::printf("guru3: t %.3f the leap ends on link %d\n", g_clock.t, plink->oid);
            p->ploRide = plink;
            SetGuruState3(p, 6);
            return;
        }
}

// (Penelope 0x39 runs on the Guru's code: FIsGuru3 stays his alone (the ride links, the invisibility))
bool FIsPenelope3(const LO* plo) { return plo && FIsLoDerivedFrom(plo, CID_PENELOPE); }
GURU* PguruFromLo(LO* plo)
{
    if (!FIsGuru3(plo) && !FIsPenelope3(plo) && !FIsPandaKing3(plo)) return nullptr;
    return dynamic_cast<GURU*>(PactorFromLo(plo));
}

STEP* PstepNewGuru3(ALO* palo) { return FIsGuru3(palo) || FIsPenelope3(palo) || FIsPandaKing3(palo) ? new GURU() : nullptr; }

void InitWorldGuru3(SW* psw)
{
    for (CID cid : {CID(CID_GURU), CID(CID_PENELOPE)})
    for (LO* plo : psw->mpcidaplo[cid])
    {
        StepPhysOf(static_cast<ALO*>(plo));
        if (GURU* p = PguruFromLo(plo))
        {
            InitGuru(p);
            PostLoadGuru(p);
        }
    }
    for (LO* plo : psw->mpcidaplo[CID_PANDA_KING])
    {
        StepPhysOf(static_cast<ALO*>(plo));
        if (GURU* p = PguruFromLo(plo))
        {
            InitPandaKing(p);
            PostLoadPandaKing(p);
        }
    }
}

// orig3 0x22D080 (vt slot 101, Sly 2's 90: the step's second mesh by the state)
void UpdateGuruStepMesh3(GURU* p)
{
    ALO* palo = p->palo;
    bool fSnap = false;
    int isoe = -1;
    // (the Panda King's 0x1EA0C8: the same on his numbers: 4 the jump, 5 the interaction, 6 the damage)
    const int st3 = !FIsPandaKing3(palo) ? p->state : p->state == 4 ? 3 : p->state == 5 ? 4 : p->state == 6 ? 5 : -2;
    switch (st3)
    {
    case 4:   // 0x1B5C78 (the interaction's: kind 0 +0x554 (+0x560 in its state 5), kind 1 +0x558, kind 8 in 9 / 10 +0x564 snapped)
        switch (p->n4C8)
        {
        case 0: isoe = IsoeGuruPart(palo, p->nSub4CC == 5 ? 0x560 : 0x554); break;
        case 1: isoe = IsoeGuruPart(palo, 0x558); break;
        case 8:
            if (p->nSub4CC == 9 || p->nSub4CC == 10) { fSnap = true; isoe = IsoeGuruPart(palo, 0x564); }
            break;
        default: break;
        }
        break;
    case 5: break;   // 0x1C4088 (the damage's: no Sly 3 fallback chain ported)
    case 3:          // 0x1BD718 (the jump's: +0x390 below its state 11, the interaction's +0x560 in 11..13, +0x394 in 17)
    {
        const int st = p->jump.state;
        isoe = IsoeGuruPart(palo, st < 11 ? 0x390u : st < 14 ? 0x560u : st == 17 ? 0x394u : 0x390u);
        break;
    }
    default: break;
    }
    if (isoe == -1) isoe = StepPhysOf(palo).isoeSource;   // +0x268
    if (isoe >= 0) SetStepMesh(palo, isoe, fSnap);        // 0x24DF48
}

// orig3 0x22CA70
// the guard whose ride link (option 494, +0x11C8) is plink (orig3: the link's +0x14 list, its first object); a guard
// given directly (SLY3_GURURIDE's test) is its own
LO* PloRideGuard3(LO* plink)
{
    if (!plink || !g_psw) return nullptr;
    if (PguardFromLo(plink)) return plink;
    for (const auto& plo : g_psw->aploAll)
        if (GUARD* pg = PguardFromLo(plo.get()); pg && PloRideLink3(pg) == plink) return plo.get();
    return nullptr;
}

void SetGuruState3(GURU* p, int n)
{
    const int cur = p->state;
    if (n == cur) return;
    switch (cur)
    {
    case 0:
        SetXfModes(p, 2);
        ActorNextLookTime(p);   // 0x155328
        break;
    case 8: SetXfModes(p, 2); break;
    case 3: SetJumpAnimState(&p->jump, -1, nullptr); break;   // 0x1B77F8
    case 4: SetInteractState(p, -1); break;                    // 0x1B4430
    case 5: SetDamageState(p, -1); break;                      // 0x1C21F0
    case 1: case 2: p->uPhase = p->pasega && p->pasega->paseg ? p->pasega->t / PasegdOf(p->pasega->paseg)->tMax : p->uPhase; break;
    case 6:
    {
        // orig3 0x22CA70 leaving 6: 0x22D2B0 (the guard's ride job 0xE8 lets the rider go: its pad back), 0x13BD88, the
        // link's rider cleared (0x1A28C8(link, 0)), the records' controllers retracted, the parent dropped keeping the
        // world place (0x1C7A00(p, 0, 0xF)), +0x15F0 = the link, +0x15E0 = 0, +0x15F4 = now
        LO* ploGuard = PloRideGuard3(p->ploRide);
        if (ploGuard) ReleaseGuardRide3(ploGuard);
        LO* plink = ploGuard && PguardFromLo(ploGuard) ? PloRideLink3(PguardFromLo(ploGuard)) : nullptr;
        SetRideRider3(plink ? plink : p->ploRide, nullptr);
        if (p->palo && p->palo->paloParent) SetAloParent(p->palo, nullptr, nullptr, 0xF);
        p->ploRideLast = p->ploRide;
        p->ploRide = nullptr;
        p->tRideLast = g_clock.t;
        break;
    }
    default: break;
    }
    const int old = p->state;
    p->state = n;
    p->tState = g_clock.t;
    ASEG* paseg = nullptr;
    SM* psm = nullptr;
    int grfapl = 1;
    OID oid = -1;
    uint32_t grfMask = 4;
    float t = 0.0f;
    switch (n)
    {
    case 0:
        SetXfModes(p, 1);
        psm = p->psmIdle;
        oid = old == 2 ? 0x193 : old == 3 ? 0x195 : old == 7 ? 0x197 : 0x192;
        if (!psm || IsmsFindSm(psm, oid) < 0) oid = -1;   // 0x239A78
        p->vecVel = glm::vec3(0.0f);
        grfMask = GrfActorLookMask(p);   // vt slot 111
        ActorSetLookFlag(p, 1);          // 0x155310
        break;
    case 1: case 2:
    {
        GAIT* pg = n == 1 ? &p->gaitWalk : &p->gaitRun;
        paseg = pg->pbl;
        if (paseg) t = p->uPhase * PasegdOf(paseg)->tMax;
        ResetGaitFeet(pg);   // 0x258F28
        grfapl = 9;
        break;
    }
    case 5: SetBinocState(p, -1); break;   // 0x1BDBB0 (+0xF60)
    case 6:
    {
        // orig3 0x22CA70 entering 6: the link's rider (0x1A28C8(link, p): the guard's reaction then runs the ride job
        // 0xE8 with the Guru's pad, airide3.cpp), 0x13BD00, parented to the link keeping the world place (0x1C7A00(p,
        // link, 0xF); natively to the guard's ALO when the link is no ALO), the records' controllers toward it
        // (0x130078 mode 5: TODO(sly3), natively the place is kept), +0x180 = 1, the ride's SM +0x15EC
        LO* ploGuard = PloRideGuard3(p->ploRide);
        GUARD* pg = ploGuard ? PguardFromLo(ploGuard) : nullptr;
        LO* plink = pg ? PloRideLink3(pg) : nullptr;
        SetRideRider3(plink ? plink : p->ploRide, p->palo);
        LO* ploParent = plink && FIsLoDerivedFrom(plink, CID_ALO) ? plink : ploGuard;
        if (p->palo && ploParent && FIsLoDerivedFrom(ploParent, CID_ALO))
        {
            SetAloParent(p->palo, static_cast<ALO*>(ploParent), nullptr, 0xF);
            // (natively the records' controllers toward the link (0x130078(p, +0x48 / +0x4C, 5, 8, 2)) are a snap: the
            // Guru stood on the link's place with its turn, his base off as a step's placement)
            const ALO* paloLink = static_cast<const ALO*>(ploParent);
            const XF* pxf = PxfObj(const_cast<ALO*>(paloLink));
            TeleportAloMat(p->palo, pxf ? pxf->matWorld : glm::mat4(1.0f), nullptr);
        }
        if (std::getenv("SLY3_GURUPRINT"))
            std::printf("guru3: ride guard %d link %d parent %d\n", ploGuard ? ploGuard->oid : -1, plink ? plink->oid : -1, ploParent ? ploParent->oid : -1);
        psm = p->psmRide;
        break;
    }
    case 7: p->vecVel = glm::vec3(0.0f); break;
    case 8:
        SetXfModes(p, 1);
        p->vecVel = glm::vec3(0.0f);
        break;   // (the script's clip / machine by 0x143EE0 after)
    default: break;   // (3 / 4: the jump's / interaction's own animations after)
    }
    SetActorAnimPp(&p->pasega, &p->psma, p->palo, paseg, t, 1.0f, grfapl, psm, oid, grfMask, true);
    UpdateActorStepMesh(p);   // vt slot 101
}

// orig3 0x22D318 (SCPT 0x6D): off the ridden object: the position (0x250600) and the jump (kind 1)
void GuruJumpOff3(LO* plo)
{
    GURU* p = PguruFromLo(plo);
    if (!p || !p->ploRide) return;
    SetJumpKind(&p->jump, 1, 0, nullptr);
}
}  // namespace sly3
