// actor.cpp: the player characters' actor class (cid 0x34) and Sly (0x35): the camera definition and the checkpoint
// placement (actor.h). Ported from the reference's m_13FF70.cpp (0x141588, 0x141640), m_188488.cpp (0x18A350,
// 0x18A428), so_p3.cpp (0x1F58F0), m_2040F8.cpp (0x2052E0), mat.cpp (0x11B938 / 0x11BA08).
#include "actor.h"
#include "sly3/ids3.h"
#include "activate.h"
#include "vtnative.h"
#include "jt.h"
#include "so.h"
#include "gang.h"

#include "alo.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "crv.h"
#include "lo.h"
#include "vecmat.h"
#include "cmworld.h"
#include "cplcy.h"
#include <algorithm>
#include "worldaccess.h"
#include "step.h"
#include "po.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

using cmm::F32;

namespace
{

struct POSEHIST
{
    glm::vec3 posLast{0.0f};
    float tLast = -1.0f;
    glm::vec3 v{0.0f};
};
std::map<LO*, POSEHIST> s_mppose;

// the limits around the actor's z (0x2BCCA0 / 0x2BCCB0 / 0x2BCCA8 / 0x2BCC98) and the smoothings (0x2BCCB8 /
// 0x2BCCC8 / 0x2BCCD8)
const cmm::LM c_lmFocus434 = {F32(0xC2FA0000), F32(0x43480000)};  // -125, 200
const cmm::LM c_lmFocus16 = {F32(0xC2480000), F32(0x42AA0000)};   // -50, 85
const cmm::LM c_lmFocus = {F32(0xC3898000), F32(0x43160000)};     // -275, 150
const cmm::LM c_lmSmooth = {F32(0xC3960000), F32(0x43480000)};    // -300, 200
const cmm::SMP c_smpFocus = {F32(0x457A0000), 0.0f, F32(0x40800000)};     // 4000, 0, 4
const cmm::SMP c_smpFocus434 = {F32(0x457A0000), 0.0f, F32(0x3E99999A)};  // 4000, 0, 0.3
const cmm::SMP c_smpOffset = {F32(0x44BB8000), 0.0f, F32(0x3F000000)};    // 1500, 0, 0.5
const float c_agAdjust[3] = {F32(0x3FC00000), F32(0xBF000000), 0.0f};     // 1.5, -0.5, 0 (0x2BCCF0)
const cmm::LM c_lmAdjust = {0.0f, 1.0f};                                  // 0x2BBB60

void WarnOnce(const char* what)
{
    static std::map<const char*, bool> s_mp;
    if (!s_mp[what]) std::fprintf(stderr, "actor: %s not ported (stubbed)\n", what), s_mp[what] = true;
}

// orig 0x11BA08 BuildRotateVectorsMatrix applied to v (row vectors: v * R, R taking a's direction to b's)
glm::vec3 RotateVectors(const glm::vec3& a, const glm::vec3& b, const glm::vec3& v)
{
    const float la = std::sqrt(glm::dot(a, a)), lb = std::sqrt(glm::dot(b, b));
    if (la < F32(0x3727C5AC) || lb < F32(0x3727C5AC)) return v;  // (EPSILON 1e-5: the identity)
    glm::vec3 k = glm::cross(a, b);
    const float lk = std::sqrt(glm::dot(k, k));
    if (F32(0x322BCC77) < lk) k *= 1.0f / lk;
    else return v;  // (parallel: the perpendicular axis with sin 0; an opposite pair is not expected for a frame's z)
    const float c = cmm::GLimitAbs(glm::dot(a, b) / (la * lb), 1.0f);
    const float s = std::sqrt(1.0f - c * c);
    return v * c + glm::cross(k, v) * s + k * (glm::dot(k, v) * (1.0f - c));
}
}  // namespace

ACTOR* PactorFromLo(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, Icid(CID3::PLAYER))) return nullptr;
    if (STEP* p = PstepFromLo(plo)) return dynamic_cast<ACTOR*>(p);
    // (made on first use when the world's start has not: the camera's checks)
    ACTOR* pa = static_cast<ACTOR*>(PstepNew(static_cast<ALO*>(plo)));
    InitActorBase(pa);
    return pa;
}

// orig: 0x00143CF8 (the actors' vt slot 29: 0x2041C0, then the safe ground's count +0x9E4 cleared)
namespace
{
void SetActorPosVt(ALO* palo, const glm::vec3& pos)
{
    ACTOR* pa = PactorFromLo(palo);
    if (!pa) return;
    SetStepPos(pa, pos);
    pa->safe.c = 0;
}
}  // namespace
NATIVEFN(0x143CF8, SetActorPosVt);

SLY* PslyFromLo(LO* plo) { return dynamic_cast<SLY*>(PactorFromLo(plo)); }

// orig 0x2039C0 (the post-load): +0x240 = the parent's origin in the object's frame; Sly's load (slot 14, 0x1885B8)
// then sets it to (0, 0, -75)
void InitActorBase(ACTOR* pa)
{
    ALO* palo = pa->palo;
    const glm::mat4 matParent = palo->paloParent ? MatWorldOfAlo(palo->paloParent) : glm::mat4(1.0f);
    const glm::mat4 mat = MatWorldOfAlo(palo);
    pa->vecBase = glm::vec4(glm::vec3(glm::inverse(mat) * (matParent * glm::vec4(0, 0, 0, 1))), 0.0f);
    if (palo->pcls && palo->pcls->PfnOrig(VTSLOT_Load) == 0x1885B8) pa->vecBase = glm::vec4(0.0f, 0.0f, -75.0f, 0.0f);
}

// (the STEP data are reset with the physics world, so.cpp's g_physw.Reset at the load's start)
void ResetActors()
{
    s_mppose.clear();
    ResetJtTrack();  // (SW +0x4678)
}

ACTORPOSE PoseFromLo(LO* plo)
{
    ACTORPOSE pose;
    glm::mat4 m(1.0f);
    if (plo && FIsLoDerivedFrom(plo, Icid(CID3::ALO)))
    {
        // vt slot 31 (0x1A62E8's matrix): the object record's (an actor's moves with its position record, not the root)
        const XF* pxf = PxfObj(static_cast<ALO*>(plo));
        m = pxf ? pxf->matWorld : MatWorldOfAlo(static_cast<ALO*>(plo));
    }
    pose.mat = glm::mat3(m);
    pose.pos = glm::vec3(m[3]);
    // TODO(physics): 0x22D348 (the record's velocity) and slot 34 (0x1F4558); until then the position's change
    POSEHIST& h = s_mppose[plo];
    if (h.tLast >= 0.0f && g_clock.t != h.tLast)
        h.v = (pose.pos - h.posLast) / (g_clock.t - h.tLast);
    if (g_clock.t != h.tLast)
    {
        h.posLast = pose.pos;
        h.tLast = g_clock.t;
    }
    pose.vPoint = h.v;
    if (plo && FIsLoDerivedFrom(plo, Icid(CID3::ALO)) && Psost(static_cast<ALO*>(plo)))
    {
        // 0x22D348 on the object's point: its mass state's velocity (slot 33 0x1F4440), and slot 34's angular one
        ALO* palo = static_cast<ALO*>(plo);
        pose.w = AngVelAlo(palo);
        pose.vPoint = VelAloPos(palo) + glm::cross(pose.w, pose.pos - PosAloCenter(palo));
    }
    return pose;
}

// orig: 0x001F58F0 (the SO's pose into the record)
void GetSoCpdefiBase(const ACTORPOSE& pose, float t, CPDEFI* pc)
{
    pc->posBase = glm::vec4(pose.pos, 1.0f);
    pc->vBase = glm::vec4(pose.vPoint, 0.0f);
    // 0x11B938 LevelMatrix3: the frame rotated so its z row is z; the heading of its x row, reversed
    const glm::vec3 x = RotateVectors(pose.mat[2], glm::vec3(0, 0, 1), pose.mat[0]);
    pc->radHome = std::atan2(-x.y, -x.x);
    pc->sAdjust = 100.0f;
    pc->grfcpd = 0;
    pc->cfk = 0;
    pc->tMoveLast = t;
}

// orig: 0x00141588 ActorGetFocus
glm::vec3 PosActorFocus(const ACTOR& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx)
{
    if (a.nSubF54 != -1)  // 0x19E9D8: the point of sub-object 0xF50's curve
    {
        glm::vec3 pos(0.0f);
        if (a.pcrvF50) EvalCrvS(a.pcrvF50, a.sCrvF50, &pos, nullptr);
        else WarnOnce("sub-object 0xF50's curve (state 8)");
        return pos;
    }
    glm::vec3 pv = pose.pos;
    if (a.pxpFloor) pv.z = a.posFloor.z + a.sHeight;
    else if (a.jump.state == 0x14 || ctx.fCut || !ctx.fFocus) pv.z = pv.z + (a.vecBase.z + a.sHeight);
    else pv.z = a.zFocus;
    return pv;
}

// orig: 0x0018A350 (Sly's slot 97)
glm::vec3 PosSlyFocus(const SLY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx)
{
    switch (a.state)
    {
    case 3: { glm::vec3 v = PosActorFocus(a, pose, ctx); v.z = ctx.zOther + -175.0f; return v; }
    case 0x16: { glm::vec3 v = PosActorFocus(a, pose, ctx); v.z = ctx.zOther + -100.0f; return v; }
    case 0x15: return pose.pos;
    default: return PosActorFocus(a, pose, ctx);
    }
}

// orig: 0x0018A4E8 (the interaction sub-object +0x4C0's part of the camera definition: a ledge / wall / swing
// (kind 0) holds the camera home behind the actor; in its own zone +0x68 the focus point +0x40 (jtfock 2) leads the
// sidestep (+0x2C 1 / 2 by the blend weight +0x50) and, on an open ledge, the zone entry's x offset +0x10 swings out
// near its ends)
void GetInteractCpdefi(ACTOR& a, const ACTORPOSE& pose, CPDEFI* pc)
{
    INTERACT& i = a.inter;
    if (a.n4C8 != 0) return;
    pc->radHome = std::atan2(pose.mat[0].y, pose.mat[0].x);  // 0x204740
    if (a.plo1024 != i.ploZone || !i.ploZone) return;
    pc->sAdjust = 0.0f;
    LO* pt = i.ploTarget;
    const bool fLedge = pt && FIsLoDerivedFrom(pt, 0x0C);
    const CRV* pcrv = fLedge ? PcrvFromLo(pt) : nullptr;
    auto sFirst = [](const CRV* p) { return p && !p->as.empty() ? p->as.front() : 0.0f; };  // 0x1E6620
    auto sLast = [](const CRV* p) { return p && !p->as.empty() ? p->as.back() : 0.0f; };    // 0x1E6648
    const int h = i.hfdk;
    if (h == 1 || h == 2)
    {
        const float d = i.g50 * 125.0f * i.g50;
        if (fLedge)
        {
            float g;
            if (h == 1)
            {
                g = i.s20 + d;
                if (pt->U32Field(0, 0xE4, 0) == 0)
                {
                    float e = sLast(pcrv);
                    if (pt->U32Field(0, 0x1BC, 0) & 2) e += 100.0f;
                    g = std::min(g, e);
                }
            }
            else
            {
                g = i.s20 - d;
                if (pt->U32Field(0, 0xE4, 0) == 0)
                {
                    float e = sFirst(pcrv);
                    if (pt->U32Field(0, 0x1BC, 0) & 1) e -= 100.0f;
                    g = std::max(g, e);
                }
            }
            if (pcrv) EvalCrvS(pcrv, g, &i.posFocus, nullptr);  // 0x1E6F10
            i.posFocus.z = i.posFocus.z + 125.0f;
        }
        else
        {
            // along the actor's y row (0x1A6318), from its position (0x1A62E8)
            const glm::vec3 r = pose.mat[1] * d;
            i.posFocus = h == 1 ? pose.pos + r : pose.pos - r;
            i.posFocus.z = i.posFocus.z + 50.0f;
        }
    }
    else if (h == 0)
    {
        i.posFocus = pose.pos;
        i.posFocus.z = i.posFocus.z + 50.0f;
    }
    if (fLedge && pt->U32Field(0, 0xE4, 0) == 0)
    {
        // near an open ledge's ends (30 in, a ramp of at most 300) the entry's x offset goes to -/+150 (squared)
        const float c = 30.0f;
        float hEnd = sLast(pcrv) - c;
        const float d = std::min(300.0f, (hEnd - c) * 0.5f);
        const float e = d + c;
        const float x = i.s20;
        hEnd = hEnd - d;
        float k = 0.0f;
        if (x < e)
        {
            const float t = std::clamp((e - x) / d, 0.0f, 1.0f);
            k = -t * t * 150.0f;
        }
        else if (hEnd < x)
        {
            const float t = std::clamp((x - hEnd) / d, 0.0f, 1.0f);
            k = t * t * 150.0f;
        }
        if (ZONECM* pz = PzonecmFromLo(i.ploZone); pz && pz->pacmeShared)
        {
            static const cmm::SMP s_smp{F32(0x44160000), F32(0x43960000), F32(0x3DCCCCCD)};  // D_002BF348 {600, 300, 0.1}
            pz->pacmeShared->acoef[0].x = cmm::GSmooth(pz->pacmeShared->acoef[0].x, k, g_clock.dt, &s_smp, nullptr);  // 0x11D018
        }
    }
    pc->cfk = 1;  // (kinds 0 / 13; the others returned above)
}

// orig: 0x00141640 ActorGetCmTarget
void GetActorCmTarget(ACTOR& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pc)
{
    GetSoCpdefiBase(pose, ctx.t, pc);
    if (a.f1044)
    {
        pc->posBase = a.posCmTarget;
        a.zFocus = a.posCmTarget.z;
    }
    else
    {
        const float z = pose.pos.z;  // (0x1A62E8: slot 31's position)
        const SLY* psly = dynamic_cast<const SLY*>(&a);
        const glm::vec3 f = psly ? PosSlyFocus(*psly, pose, ctx) : PosActorFocus(a, pose, ctx);
        const cmm::LM& lm = a.f434 ? c_lmFocus434 : a.jump.state == 0x16 ? c_lmFocus16 : c_lmFocus;
        const float lo = z + lm.gMin, hi = z + lm.gMax;
        float g = f.z;
        if (g < lo) g = lo;
        else if (hi < g) g = hi;
        a.zFocus = g;
        if (ctx.fCut || !ctx.fFocus) pc->posBase.z = g;
        else
        {
            const float r = cmm::GSmooth(a.zFocusSmooth, a.zFocus, ctx.dt, a.f434 == 0 ? &c_smpFocus : &c_smpFocus434, nullptr);
            const float lo2 = z + c_lmSmooth.gMin, hi2 = z + c_lmSmooth.gMax;
            pc->posBase.z = r < lo2 ? lo2 : !(hi2 < r) ? r : hi2;
        }
        pc->posBase.x = f.x;
        pc->posBase.y = f.y;
    }
    a.vecOffset = cmm::PosSmooth(a.vecOffset, glm::vec3(0.0f), ctx.dt, &c_smpOffset, nullptr);
    pc->posBase += glm::vec4(a.vecOffset, 0.0f);
    a.posCmTarget = pc->posBase;
    a.zFocusSmooth = pc->posBase.z;
    // the sub-objects' adjustments
    if (a.jump.state != -1 && a.jump.f28) pc->grfcpd |= 1;  // 0x19C8B0
    if (a.nSub4CC != -1) GetInteractCpdefi(a, pose, pc);  // 0x18A4E8
    if (a.nSubDF4 != -1)  // 0x1A2160
    {
        const int n = a.nSubDF4;
        if (n == 5 || (n >= 8 && n < 13)) pc->sAdjust = 0.0f;
        pc->grfcpd |= 1;  // (every reaction: the knock-back 7 too, the camera keeps its pan)
    }
    const float l = 0.0f + std::sqrt(glm::dot(pose.w, pose.w));
    float g = c_agAdjust[0] + l * (c_agAdjust[1] + l * c_agAdjust[2]);
    if (g < c_lmAdjust.gMin) g = c_lmAdjust.gMin;
    else if (c_lmAdjust.gMax < g) g = c_lmAdjust.gMax;
    pc->sAdjust = pc->sAdjust * g;
}

// orig: 0x0018A428 (Sly's slot 50)
void GetSlyCpdefi(SLY& a, const ACTORPOSE& pose, const ACTORCMCTX& ctx, CPDEFI* pc)
{
    if (a.state != 0 && a.state != 0x12 && a.nSub4CC != 0 && a.nSubF54 != 1) a.tActive = ctx.t;
    GetActorCmTarget(a, pose, ctx, pc);
    switch (a.state)
    {
    case 8:  // 0x19EA00: the velocity along sub-object 0xF50's curve (its direction times the speed +0xF64)
    {
        glm::vec3 dir(0.0f);
        if (a.pcrvF50) EvalCrvS(a.pcrvF50, a.sCrvF50, nullptr, &dir);
        pc->sAdjust = 0.0f;
        pc->vBase = glm::vec4(dir * a.gCrvF50, 0.0f);
        break;
    }
    case 3:
    case 5:
    case 6: pc->grfcpd |= 1; break;
    default: break;
    }
}

void GetLoCpdefi(LO* plo, CPDEFI* pc)
{
    const uint32_t pfn = plo && plo->pcls ? plo->pcls->PfnOrig(50) : 0;
    const ACTORPOSE pose = PoseFromLo(plo);
    ACTORCMCTX ctx;
    ctx.fCut = g_cm.fCut != 0;
    ctx.fFocus = g_cm.psoFocusPrev == plo;  // (CM +0x260)
    ctx.t = g_clock.t;
    ctx.dt = g_clock.dt;
    ACTOR* pa = PactorFromLo(plo);
    SLY* psly = dynamic_cast<SLY*>(pa);
    if (psly && psly->ploOther) ctx.zOther = PosLoWorld(psly->ploOther).z;  // (0x1A62E8: a hook is not an ALO)
    if (psly && pfn == 0x18A428) GetSlyCpdefi(*psly, pose, ctx, pc);
    else if (BENTLEY* pb = dynamic_cast<BENTLEY*>(pa); pb && pfn == 0x1358A8) GetBentleyCpdefi(*pb, pose, ctx, pc);  // (gang.cpp)
    else if (MURRAY* pm = dynamic_cast<MURRAY*>(pa); pm && pfn == 0x1B6728) GetMurrayCpdefi(*pm, pose, ctx, pc);
    else if (pa && pfn == 0x141640) GetActorCmTarget(*pa, pose, ctx, pc);
    else if (pfn == 0x152350)
    {
        // orig: 0x00152350 (the dancers, cid 0x44): the base's definition, its heading turned by -3 pi / 4
        GetSoCpdefiBase(pose, ctx.t, pc);
        pc->radHome -= 2.3561945f;
    }
    else
    {
        if (pfn != 0x1F58F0) WarnOnce("a class's camera definition (slot 50)");
        GetSoCpdefiBase(pose, ctx.t, pc);
    }
}

// orig: 0x002052E0 (slot 38)
void TeleportActor(LO* plo, LO* plodest)
{
    if (!plo || !plodest || !FIsLoDerivedFrom(plo, Icid(CID3::ALO))) return;
    if (!FIsLoDerivedFrom(plodest, 3) && !FIsLoDerivedFrom(plodest, Icid(CID3::ALO)))  // (slot 31: ALO 0x1F3FA0, LOX 0x231440)
    {
        WarnOnce("0x1F49C8 (slot 38 to a non-located object)");
        return;
    }
    glm::mat4 m(1.0f);
    if (FIsLoDerivedFrom(plodest, Icid(CID3::ALO)))
    {
        // the destination's vt slot 31 (ALO 0x1F3FA0): its object record's (+0x60) world matrix
        ALO* paloTo = static_cast<ALO*>(plodest);
        m = PxfObj(paloTo) ? PxfObj(paloTo)->matWorld : MatWorldOfAlo(paloTo);
    }
    else
    {
        const LOX* plox = static_cast<const LOX*>(plodest);
        m = plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    }
    ALO* palo = static_cast<ALO*>(plo);
    ACTOR* pa = PactorFromLo(plo);
    const glm::vec3 vecBase = pa ? glm::vec3(pa->vecBase) : glm::vec3(0.0f);
    if (std::getenv("SLY2N_ACTORPRINT"))
        std::printf("teleport %d (cid 0x%X) to %d (cid 0x%X) at %.2f %.2f %.2f, base %.2f %.2f %.2f, parent %d\n", plo->oid,
                    plo->Cid(), plodest->oid, plodest->Cid(), m[3].x, m[3].y, m[3].z, vecBase.x, vecBase.y, vecBase.z,
                    plo->paloParent ? plo->paloParent->oid : -1);
    // slot 29 (the position: the matrix's, the +0x240 offset back), slot 30 (the rotation)
    glm::mat4 w = m;
    w[3] = glm::vec4(glm::vec3(m * glm::vec4(-vecBase, 1.0f)), 1.0f);
    if (pa && PxfPos(palo) && !std::getenv("SLY2N_OLDTELEPORT"))
    {
        // the actor's slots 29 / 30 (0x143CF8: 0x2041C0 and the safe ground's count +0x9E4 = 0; 0x2041F8), 35 / 36
        // with zero velocities, +0x208 = 0
        SetStepPos(pa, glm::vec3(w[3]));
        pa->safe.c = 0;
        SetStepRotation(pa, glm::mat3(m));
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
    }
    else if (STEP* pstep = !pa ? PstepFromLo(plo) : nullptr; pstep && !std::getenv("SLY2N_OLDTELEPORT"))
    {
        // orig 0x2052E0 for the other PO-derived objects (e.g. the guards): the place minus the step's +0x240 offset
        // in the destination's frame through slot 29 (0x2041C0), the rotation slot 30 (0x2041F8), zero velocities,
        // +0x208 = 0
        const glm::vec3 vecOff(pstep->vecBase);
        SetStepPos(pstep, glm::vec3(m * glm::vec4(-vecOff, 1.0f)));
        SetStepRotation(pstep, glm::mat3(m));
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        pstep->pxpFloor = nullptr;
    }
    else if (!palo->axf.empty() && PxfPos(palo) && PxfRot(palo) && !std::getenv("SLY2N_OLDTELEPORT"))
    {
        // orig 0x1A6B88 -> 0x1A6BE8 for the other ALOs: slot 29 (0x1F40A0, the position record +0x58) at the
        // matrix's place, slot 30 (0x1F42E0, the rotation record +0x5C) to its rows (U18: world 14's 77184, put by its
        // SM's retract handler at 88149, took 88149's rotation in the reference)
        SetAloPos(palo, glm::vec3(w[3]));
        SetAloRotation(palo, glm::mat3(m));
        // then, with a physics state (+0xE8), slots 35 / 36 with the destination's slots 33 / 34 (its velocities; a
        // LOX's are zero): e.g. the Ep1 heist harpoon put on the sign's point 142374 stops there
        if (Psost(palo))
        {
            ALO* paloTo = FIsLoDerivedFrom(plodest, Icid(CID3::ALO)) ? static_cast<ALO*>(plodest) : nullptr;
            SetAloVelocityTo(palo, paloTo ? VelAloPos(paloTo) : glm::vec3(0.0f));
            SetAloAngVelocityTo(palo, paloTo ? AngVelAlo(paloTo) : glm::vec3(0.0f));
        }
    }
    else if (!palo->axf.empty())
    {
        XF& xf = *palo->axf[0];
        xf.mat = xf.pxfRef ? glm::inverse(xf.pxfRef->matWorld) * w : w;
        UpdateAloHierarchyWorlds(palo);
    }
    if (pa)
    {
        // +0x1060: the destination's position raised by +0x1048 (w: 1 + its w, as the original's add)
        pa->posCmTarget = glm::vec4(glm::vec3(m[3]) + glm::vec3(0, 0, 1) * pa->sHeight, 1.0f + m[3].w);
        pa->pxpFloor = nullptr;
    }
    // TODO(physics): slots 35 / 36 (the velocities zeroed)
    s_mppose.erase(plo);
}
