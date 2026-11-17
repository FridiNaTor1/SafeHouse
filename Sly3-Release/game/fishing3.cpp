// game/fishing3.cpp: Bentley's Fishing Pole (game/fishing3.h; s3-gadgets). Ported from the Sly 3 decomp:
// func_0017E760.c, func_0017E850.c, func_0017EAD0.c, func_0017EBA0.c, func_0017EF78.cpp, func_0017F560.c,
// func_0017F788.c, gate_test_17e810.c (the controller at Bentley +0x1830), with Bentley's side in gang.cpp (his state
// 16: 0x13E510 enters with the controller's state 0 and leaves with -1, 0x13D518 runs its transitions, 0x13DE58 its
// update, 0x13EE68 hands it the animation events, 0x13F930 draws it).

#include "game/fishing3.h"

#include "activate.h"
#include "actor.h"
#include "alo.h"
#include "aseg.h"
#include "asega.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "emitter.h"
#include "gadget.h"
#include "game/items3.h"
#include "game/sly3.h"
#include "gang.h"
#include "interact.h"
#include "jt.h"
#include "jump.h"
#include "lo.h"
#include "mat.h"
#include "po.h"
#include "pocket.h"
#include "render.h"
#include "smrun.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "util.h"
#include "vecmat.h"
#include "xf.h"
#include "xfphys.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

extern SW* g_psw;
void BindLoPartsTable3(ALO* palo, int c, uint32_t vaTable);   // loaders.cpp (orig3 0x1C8970, under FN3_TAG)
void UpdateBentleyHoldWalk(BENTLEY* p, LO* ploFace);          // gang.cpp (orig3 0x13DE58's case 8 / 0x17EF78's walk)

namespace sly3
{
namespace
{
constexpr int ITEM2_FISH = ITEM2_SLY3 + 7;   // (the gadget framework's Sly 2-side id of Sly 3's item 7)

bool FPrintFish() { static const bool s_f = std::getenv("SLY3_FISHPRINT") != nullptr; return s_f; }

template <class T>
T* PPart3(ALO* palo, uint32_t ib3)
{
    for (const auto& [ib, plo] : palo->apart)
        if (ib == (FN3_TAG | ib3)) return dynamic_cast<T*>(plo);
    return nullptr;
}

PADCTL* PpadctlOf(BENTLEY* p)
{
    PODATA* ppd = PpodataFromLo(p->palo);
    return ppd ? ppd->ppadctl : nullptr;
}

// orig3 0x17E810: the actor's target (+0x11B4, kind +0x11BC 3) is a giver whose pocket glows for the player (mode
// +0x40C 2) with nothing planted in it (+0x4A4)
bool FCanHook(const BENTLEY* p)
{
    if (p->n10E4 != 3 || !p->plo10DC) return false;
    const POCKET& pocket = PocketOf(p->plo10DC);
    return pocket.nMode == 2 && !pocket.fPlanted;
}

glm::vec3 PosTip(const FISH3& f) { return PosLoWorld(f.ploTip); }   // vt slot 36's +0x30
// the tip's velocity (vt slot 37's record, 0x28BCD0): the tip is a point (class 3) on a record of the rod's
glm::vec3 VelTip(const FISH3& f, const glm::vec3& pos)
{
    XF* pxf = nullptr;
    if (FCidDerivedFrom(f.ploTip->Cid(), S3CID(3))) pxf = static_cast<const LOX*>(f.ploTip)->pxf;
    else if (FIsLoDerivedFrom(f.ploTip, S3CID(0x30))) pxf = PxfObj(static_cast<ALO*>(f.ploTip));
    return pxf ? VelXfPoint(pxf, nullptr, pos) : glm::vec3(0.0f);
}
// the target's pocket object (+0x4AC); the target itself when it has none (natively)
LO* PloPocketOf(const BENTLEY* p)
{
    if (!p->plo10DC) return nullptr;
    LO* ppocket = PloPocket(p->plo10DC);
    return ppocket ? ppocket : p->plo10DC;
}

// orig3 0x17EAD0 (the lure's state: 0 hanging (the line as long as the lure is far now), 1 flying, 2 hooked; while it
// flies or is hooked the actor's +0x11B8 bit 2 is set: the target is kept)
void SetLureState(BENTLEY* p, int n)
{
    FISH3& f = p->fish;
    if (n == f.stLure) return;
    f.stLure = n;
    f.tLure = g_clock.t;
    if (FPrintFish()) std::printf("fishing: lure %d t %.3f\n", n, g_clock.t);
    if (n == 0 && f.ploTip) f.sLine = glm::length(f.posLure - PosTip(f));
    if (n == 1 || n == 2) p->grf10E0 |= 2u;
    else p->grf10E0 &= ~2u;
}

bool FReady(const BENTLEY* p) { return !p->pasega || FAsegaRetracted(p->pasega); }   // (!+0x113C || 0x138AC8)
}  // namespace

// orig3 0x17E760 (from Bentley's init 0x13D480: the table 0x344F28's ten parts {0x300, oid, offset})
void InitFishing3(BENTLEY* p)
{
    FISH3& f = p->fish;
    f = FISH3{};
    ALO* palo = p->palo;
    BindLoPartsTable3(palo, 10, 0x344F28);   // 0x1C8970(owner, 10, D_00344F28, 0)
    f.pasegHeld = PPart3<ASEG>(palo, 0x184C);
    f.pasegHooked = PPart3<ASEG>(palo, 0x1850);
    f.pasegOut = PPart3<ASEG>(palo, 0x1858);
    f.pasegAir = PPart3<ASEG>(palo, 0x185C);
    f.pasegPull = PPart3<ASEG>(palo, 0x1860);
    f.pasegAway = PPart3<ASEG>(palo, 0x1864);
    f.ploRod = PPart3<LO>(palo, 0x1868);
    f.paloLine = PPart3<ALO>(palo, 0x186C);
    f.paloLure = PPart3<ALO>(palo, 0x1870);
    f.ploTip = PPart3<LO>(palo, 0x1874);
    if (FPrintFish())
        std::printf("fishing: Bentley %d: clips %d %d %d %d %d %d rod %d line %d lure %d tip %d\n", palo->oid, f.pasegHeld ? f.pasegHeld->oid : -1,
                    f.pasegHooked ? f.pasegHooked->oid : -1, f.pasegOut ? f.pasegOut->oid : -1, f.pasegAir ? f.pasegAir->oid : -1,
                    f.pasegPull ? f.pasegPull->oid : -1, f.pasegAway ? f.pasegAway->oid : -1, f.ploRod ? f.ploRod->oid : -1,
                    f.paloLine ? f.paloLine->oid : -1, f.paloLure ? f.paloLure->oid : -1, f.ploTip ? f.ploTip->oid : -1);
    if (FPrintFish())
        for (ASEG* paseg : {f.pasegHeld, f.pasegHooked, f.pasegOut, f.pasegAir, f.pasegPull, f.pasegAway})
            if (ASEGD* pd = paseg ? PasegdOf(paseg) : nullptr)
                for (const auto& aev : pd->aaev)
                    for (const ASEGEV& ev : aev)
                        std::printf("fishing: clip %d (%.3f s): event kind %d id 0x%X at %.3f\n", paseg->oid, pd->tMax, ev.kind, ev.an[0], ev.t);
}

bool FFishingRod3(const BENTLEY* p) { return p->fish.ploRod != nullptr; }

// orig3 0x17E850
void SetFishState3(BENTLEY* p, int n)
{
    FISH3& f = p->fish;
    if (n == f.state) return;
    f.state = n;
    f.tState = g_clock.t;
    p->vecVel = glm::vec3(0.0f);
    if (FPrintFish()) std::printf("fishing: state %d t %.3f\n", n, g_clock.t);
    ASEG* pa = nullptr;
    ASEG* pb = nullptr;
    int na = 1, nb = 1;
    switch (n)
    {
    case -1:
        if (f.ploRod && !f.ploRod->fRemoved) RemoveLo(g_psw, f.ploRod);   // vt slot 9
        SetLureState(p, -1);
        break;
    case 0:
        pa = f.pasegOut;
        if (!f.ploRod || !f.ploTip) break;
        // the lure 10 under the rod's tip, pointing down, at rest (D_00317550 = the z axis)
        f.posLure = PosTip(f) + glm::vec3(0.0f, 0.0f, 1.0f) * -10.0f;
        f.dirLure = glm::vec3(0.0f, 0.0f, 1.0f) * -1.0f;
        f.velLure = glm::vec3(0.0f);
        SetLureState(p, 0);
        break;
    case 6:
        pa = f.pasegAway;
        SetLureState(p, 0);
        break;
    case 1:
        if (f.ploRod && f.ploRod->fRemoved) AddLo(g_psw, f.ploRod);   // vt slot 8
        pa = p->rb14F0.pbl;   // (+4 = Bentley's +0x1760, the hold's root blend +0x1660)
        na = 9;
        pb = f.pasegHeld;
        nb = 9;
        break;
    case 4:
        pa = p->rb14F0.pbl;
        na = 9;
        pb = f.pasegHooked;
        nb = 9;
        break;
    case 2: case 5:
        pa = f.pasegAir;
        na = 9;
        break;
    case 3: pa = f.pasegPull; break;
    default: break;
    }
    ClearSmaHandle(&p->psma);                                          // 0x23AC58 (+0x1140)
    ApplyAsegCur(pa, p->palo, 0.0f, 1.0f, na, &p->pasega);             // 0x137588 (+0x113C)
    ApplyAsegCur(pb, p->palo, 0.0f, 1.0f, nb, &f.pasegaUpper);         // 0x137588 (the controller's +0x24)
}

// orig3 0x17EBA0 (from Bentley's transitions 0x13D518, state 16)
void UpdateFishTransitions3(BENTLEY* p)
{
    FISH3& f = p->fish;
    PADCTL* pc = PpadctlOf(p);
    for (int iter = 0; iter < 16; ++iter)
    {
        const int stLure = f.stLure, st = f.state;
        LO* ppocket = PloPocketOf(p);
        switch (stLure)
        {
        case 0:
            // held with a pocket to pick within 250 of the rod's tip: the lure flies
            if (st == 1 && FCanHook(p) && f.ploTip && glm::length(PosLoWorld(ppocket) - PosTip(f)) < 250.0f) SetLureState(p, 1);
            break;
        case 1:
            if (!FCanHook(p)) SetLureState(p, 0);
            else if (0.35f < g_clock.t - f.tLure) SetLureState(p, 2);
            break;
        case 2:
            // hooked and walked further than 400 away: the pull
            if (st == 4 && ppocket && f.ploTip && 400.0f < glm::length(PosLoWorld(ppocket) - PosTip(f))) SetFishState3(p, 3);
            break;
        default: break;
        }
        const bool fHeld = pc && (pc->grfbtn & GrfAbilityButton(p, ITEM2_FISH));   // (+0x134's +0x18 & 0x154798(owner, 7))
        const bool fAir = 0.1f < g_clock.t - p->tFloor && !p->pxpFloor;             // 0x2503C8(owner, 0.1)
        switch (f.state)
        {
        case 1: case 2:
            if (!fHeld) SetFishState3(p, 6);
            else if (fAir) SetFishState3(p, 2);
            else if (f.stLure == 2) SetFishState3(p, 4);
            else SetFishState3(p, 1);
            break;
        case 4: case 5:
            if (!fHeld) SetFishState3(p, 6);
            else if (fAir) SetFishState3(p, 5);
            else SetFishState3(p, 4);
            break;
        case 0:
            if (FReady(p)) SetFishState3(p, 1);
            break;
        case 6:
            if (FReady(p)) SetJumpKind(&p->jump, 0xB, 0, 0);   // 0x1B7268(+0x320, 0xB, 1.0, 0, 0): Bentley leaves the state
            break;
        case 3:
            if (FReady(p)) SetFishState3(p, 1);
            break;
        default: break;
        }
        if (f.state == -1) return;
        if (f.state == st && f.stLure == stLure) break;
    }
}

// orig3 0x17EF78 (from Bentley's update 0x13DE58, state 16)
void UpdateFish3(BENTLEY* p)
{
    FISH3& f = p->fish;
    switch (f.state)
    {
    case 1: case 4:
        // the hold's walk (speed and turn by the stick, the root blend +0x1660); facing the pocket while one can be hooked
        UpdateBentleyHoldWalk(p, FCanHook(p) ? PloPocketOf(p) : nullptr);
        break;
    case 3:
    {
        // the pull: the clip's root velocity, reversed (0x24F278)
        glm::vec4 v(0.0f);
        if (ASEGA* pasega = p->pasega; pasega && !FAsegaRetracted(pasega))
            if (const ASEGCHN* pchn = PchnFindAseg(pasega->paseg, p->palo, 1, true); pchn && pchn->ptrkp)
                pchn->ptrkp->Eval(nullptr, &v, pasega->t, pasega->svt);
        p->vecVel = -glm::vec3(v);
        break;
    }
    default: break;
    }
    const float dt = g_clock.dt;
    switch (f.stLure)
    {
    case 0:
    {
        if (!f.ploTip) break;
        // a pendulum under the rod's tip: gravity -2275, damping 2.5, the line eased to 125 (D_00321800 {2000, 500, 0.1})
        float dg = 0.0f;
        if (f.sLine != 125.0f)
        {
            static const SMP s_smp = {2000.0f, 500.0f, 0.1f};
            f.sLine = GSmooth(f.sLine, 125.0f, dt, &s_smp, &dg);   // 0x11E598
        }
        const glm::vec3 vNew = f.velLure + (glm::vec3(0.0f, 0.0f, -2275.0f) + f.velLure * -2.5f) * dt;
        glm::vec3 vel = vNew;
        glm::vec3 pos = f.posLure + (f.velLure + vel) * (dt * 0.5f);
        const glm::vec3 posTip = PosTip(f);
        const glm::vec3 velTip = VelTip(f, posTip);   // vt slot 37, 0x28BCD0
        const glm::vec3 d = pos - posTip;
        const float l = glm::length(d);
        f.dirLure = l < 1e-4f ? glm::vec3(0.0f) : d / l;
        if (f.sLine < l)
        {
            // the line taut: the lure's speed along it is the tip's (plus the line's own change), the lure on the sphere
            const glm::vec3 n = f.dirLure;
            vel = vNew - n * glm::dot(vNew, n) + n * glm::dot(velTip, n);
            vel += n * dg;
            pos = posTip + n * f.sLine;
        }
        f.posLure = pos;
        f.velLure = vel;
        break;
    }
    case 1:
    {
        // flying: a Hermite arc to the pocket's hook point, arriving 0.35 s after the start with the pocket-frame
        // velocity +0x70 (0x118558)
        LO* ppocket = PloPocketOf(p);
        if (!ppocket) break;
        const glm::vec3 posTo = PosJtConvert(ppocket, nullptr, f.posPocket);   // 0x1C9428
        const glm::vec3 velTo = VecJtConvert(ppocket, nullptr, f.vecPocket);   // 0x1C94E8
        glm::vec3 pos = f.posLure, vel = f.velLure;
        EvalHermiteVector(f.posLure, f.velLure, posTo, velTo, &pos, &vel, nullptr, 0.35f - (g_clock.t - f.tLure) + dt, dt, 1.0f);
        f.posLure = pos;
        f.velLure = vel;
        if (const float l = glm::length(vel); !(l < 1e-4f)) f.dirLure = vel / l;
        break;
    }
    default: break;
    }
    if (FPrintFish() && f.stLure >= 0)
    {
        const glm::vec3 t = f.ploTip ? PosTip(f) : glm::vec3(0.0f);
        std::printf("fishing: t %.3f state %d lure %d at %.1f %.1f %.1f (tip %.1f %.1f %.1f, line %.1f) target %d kind %d\n", g_clock.t, f.state,
                    f.stLure, f.posLure.x, f.posLure.y, f.posLure.z, t.x, t.y, t.z, f.sLine, p->plo10DC ? p->plo10DC->oid : -1, p->n10E4);
    }
}

// (debug: SLY3_FISHTEST=F[,D[,N[,C]]]: at the harness frame F (and the C - 1 frames after it) Bentley is put D (default
// 200) behind the N-th guard in the world (default -1: the first whose pocket glows for him, mode 2), facing it; at F
// the guards' pockets are listed)
void FishTestAid3(BENTLEY* p, int frame)
{
    static const char* s_sz = std::getenv("SLY3_FISHTEST");
    if (!s_sz || !g_psw) return;
    int f = 0, n = -1, c = 1;
    float d = 200.0f;
    std::sscanf(s_sz, "%d,%f,%d,%d", &f, &d, &n, &c);
    if (frame < f || frame >= f + c) return;
    static LO* s_pick = nullptr;
    if (frame == f)
    {
        int i = 0;
        s_pick = nullptr;
        for (const auto& up : g_psw->aploAll)
        {
            LO* pg = up.get();
            if (!FCidDerivedFrom(pg->Cid(), S3CID(0x38)) || !pg->fInWorld) continue;
            const glm::vec3 pos = PosLoWorld(pg);
            std::printf("fishtest: guard %d #%d (cid 0x%X) pocket mode %d object %d at %.0f %.0f %.0f\n", pg->oid, i, pg->Cid(), PocketOf(pg).nMode,
                        PloPocket(pg) ? PloPocket(pg)->oid : -1, pos.x, pos.y, pos.z);
            if (n >= 0 ? i == n : (!s_pick && PocketOf(pg).nMode == 2)) s_pick = pg;
            ++i;
        }
        if (!s_pick) std::printf("fishtest: no guard to fish from\n");
    }
    LO* pick = s_pick;
    if (!pick) return;
    const glm::mat4 m = MatJtLo(pick);
    const glm::vec3 fwd = glm::normalize(glm::vec3(m[0].x, m[0].y, 0.0f));
    const glm::vec3 pos = PosLoWorld(pick) - fwd * d;
    SetStepPos(p, pos - glm::vec3(p->vecBase));
    const float hh = std::atan2(fwd.y, fwd.x);
    glm::mat3 r(1.0f);
    r[0] = glm::vec3(std::cos(hh), std::sin(hh), 0.0f);
    r[1] = glm::vec3(-std::sin(hh), std::cos(hh), 0.0f);
    SetStepRotation(p, r);
    if (frame == f || frame == f + c - 1) std::printf("fishtest: frame %d: Bentley %.0f behind guard %d at %.0f %.0f %.0f\n", frame, d, pick->oid, pos.x, pos.y, pos.z);
}

// orig3 0x17F788 (from Bentley's messages 0x13EE68 while in state 16): the pull's event 0x185 with the lure hooked: the
// lure hangs again and the pocket is taken (0x156DC0: the pocket's take toward Bentley; a prize that is no coin / charm
// is held up with the prize clip +0x145C)
void OnFishEvent3(BENTLEY* p, const ASEGMSG9* pm)
{
    FISH3& f = p->fish;
    if (FPrintFish() && pm)
        std::printf("fishing: event 0x%X (the actor's animation %d, the upper %d) state %d lure %d t %.3f\n", pm->n, int(pm->pasega == p->pasega),
                    int(pm->pasega == f.pasegaUpper), f.state, f.stLure, g_clock.t);
    if (!pm || pm->pasega != p->pasega || pm->n != 0x185 || f.state != 3 || f.stLure != 2) return;
    SetLureState(p, 0);
    if (!p->plo10DC) return;
    LO* r = PloTakePocket(p->plo10DC, PosLoWorld(p->palo), true);   // 0x1F1E18
    if (FPrintFish()) std::printf("fishing: took %d from %d t %.3f\n", r ? r->oid : -1, p->plo10DC->oid, g_clock.t);
    if (!r || FIsLoDerivedFrom(r, S3CID(0x5B)) || FIsLoDerivedFrom(r, S3CID(0x5D))) return;   // (0x13B500: 0x67 / 0x69)
    if (ASEG* paseg = PPart3<ASEG>(p->palo, 0x145C)) StartPrizeInteract(p, paseg, r, -1);       // 0x154EF0 (+0x1458 = the prize)
}

// orig3 0x17F560 (Bentley's draw 0x13F930 in state 16, with the rod in the world): hooked, the lure sits on the
// pocket's point along the pocket-frame direction; the lure's model (+0x40) at the lure's place, its x along the
// lure's direction (0x11CBF0 with the actor's x row); the line's model (+0x3C) from the rod's tip to the lure's (-35, 0,
// 0) (0x1C8328: x across the view, y the width, z the model's 100 units along it); both at the rod's fade (+0x104)
void CollectFishingDraws3(const SCENE& scene, const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra)
{
    if (!g_psw) return;
    BENTLEY* p = PbentleyGlobal();
    if (!p || !p->palo || p->state != BS3_Fish) return;
    FISH3& f = p->fish;
    if (!f.ploRod || f.ploRod->fRemoved) return;
    if (f.stLure == 2)
        if (LO* ppocket = PloPocketOf(p))
        {
            f.posLure = PosJtConvert(ppocket, nullptr, f.posPocket);
            const glm::vec3 v = VecJtConvert(ppocket, nullptr, f.vecPocket);
            if (const float l = glm::length(v); !(l < 1e-4f)) f.dirLure = v / l;
            f.velLure = glm::vec3(0.0f);
        }
    if (FPrintFish())
        std::printf("fishing: draw lure %d (scene %d globs %d) line %d (scene %d globs %d)\n", f.paloLure ? f.paloLure->oid : -1,
                    int(f.paloLure && PsgsOfAlo(scene, f.paloLure)), int(f.paloLure && f.paloLure->pglobset), f.paloLine ? f.paloLine->oid : -1,
                    int(f.paloLine && PsgsOfAlo(scene, f.paloLine)), int(f.paloLine && f.paloLine->pglobset));
    const float g = static_cast<const ALO*>(f.ploRod)->gFade;
    const glm::mat4 matOwner = MatJtLo(p->palo);
    glm::mat4 mat(BuildLookAtMatrix(f.dirLure, glm::vec3(matOwner[0])));
    mat[3] = glm::vec4(f.posLure, 1.0f);
    if (f.paloLure) AppendAloExtraDraws(scene, f.paloLure, mat, g, aextra);   // 0x1C8248
    if (f.paloLine && f.ploTip)
    {
        const glm::vec3 a = PosTip(f), b = glm::vec3(mat * glm::vec4(f.vecLine, 1.0f));
        const glm::vec3 d = b - a;
        const glm::vec3 n = glm::cross(a - posCamera, d);
        const float length = glm::length(n);
        if (0.01f < length)
        {
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(glm::normalize(glm::cross(n, d)), 0.0f);
            m[1] = glm::vec4(n * (g / length), 0.0f);
            m[2] = glm::vec4(d * 0.01f, 0.0f);
            m[3] = glm::vec4(a, 1.0f);
            AppendAloExtraDraws(scene, f.paloLine, m, g, aextra);
        }
    }
}
}  // namespace sly3
