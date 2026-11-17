// rowboat3.cpp: Sly 3's rowboat (class 0x53, orig3 0x20ED28..0x20FEF0), from the Sly 3 decomp.
//
// m3_heist's inflatable boat (h_ext's proxy 93589): Murray rows it away from the windmill on its rope (tasks 3281 /
// 3285 / 3287). The record keeps the class's fields +0x190..+0x20C. Ported: the init 0x20ED28 (and its options, EOPIDs
// 1778..1792: the scripts' setters / getters, 1786 the state with its setter 0x20F868), the update 0x20EE58 (vt slot
// 18: the horn on R2 (sound 0x2A), the oars' input 0x20FC58, the hit flash's timer 0x20FA10), the forces 0x20F3C0
// (slot 74: gravity, a stroke's impulse along the boat (+0x1C0: Cross forward, Square back), the turn toward the
// heading (+0x204, the stick's camera-relative direction) through the rotation spring {+0x1C4, 0, 0.25}, the rope
// 0x20FB58), the water contact 0x20EEF0 (slot 76: water.cpp ApplyWaterToBoat3), the hits 0x20F5F0 (slot 78), the reset
// 0x20EE28 / 0x20F820 (slot 31), the pad 0x20F738 (slot 83: po.cpp's enable), the state 0x20F868 (1 idle, 2 rowed,
// 3 sunk: broadcast 0xC), the messages 0x20FE60 (slot 5).
// The rower (orig3 D_0038AE90: the actor whose +0x1C90 is the boat, Murray's boat state 23 (0x1DE690) with his nine
// rowing clips +0x1C6C, 0x1E4900): not ported natively; the record keeps his clip mode (+0x1C64) and the boat's
// "ready" flag (+0x1F0, set again by his clip's end: ~0.25 s after a stroke in the reference's RAM).
// Not ported: the oars' splashes 0x20F508 (+0x1F8 / +0x1FC / the PE +0x200), the hit flash's colour (+0xC0), the
// horn's camera shake (0x168670), the idle state's position / rotation controllers (0x130078: the boat stays as it is).
// Debug: SLY3_ROWPRINT=1 (states, strokes, every second the boat), SLY3_ROWTEST=t (h_ext: the boat given the pad after t seconds).

#include "rowboat3.h"

#include "../activate.h"
#include "../brx.h"
#include "../alo.h"
#include "../so.h"
#include "../clock.h"
#include "../cm.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../po.h"
#include "../projectile.h"
#include "../so.h"
#include "../sound.h"
#include "../spliceglue.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../vtnative.h"
#include "../xf.h"
#include "../xfphys.h"
#include "sly3.h"
#include "task3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

void VehicleAccelRotation(ALO* palo, const glm::vec3& v);   // vehicle.cpp (orig3 0x244890)

namespace
{
constexpr int CID_ROWBOAT = 0x53;
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_ROWPRINT") != nullptr;
    return s_f;
}

struct ROWBOAT3
{
    ALO* palo = nullptr;
    SW* psw = nullptr;
    BOATWATER3 water;                   // +0x190..+0x1B8
    float restart = 1.0f;               // +0x1BC: the time after a hit before the next counts (and the flash's)
    float amount = 6000.0f;             // +0x1C0: a stroke's impulse
    float shift = 2.5f;                 // +0x1C4: the turn spring's speed
    float cooldown = 0.0f;              // +0x1C8
    int state = -1;                     // +0x1CC: 1 idle, 2 rowed, 3 sunk
    LO* anchor = nullptr;               // +0x1D8 (EOPID 1787): the rope's two ends
    LO* follower = nullptr;             // +0x1DC (1788)
    float limit = 0.0f, stiffness = 0.0f, damping = 0.0f;   // +0x1E0 / +0x1E4 / +0x1E8 (1789..1791)
    int active = 1;                     // +0x1F0: the rower ready for a stroke
    int tracked = 0;                    // +0x1F4 (messages 6 / 7)
    LO* wake = nullptr;                 // +0x200 (1792): the oars' splash PE
    float heading = 0.0f;               // +0x204
    int fForward = 0, fBack = 0;        // +0x208 / +0x20C: a stroke to apply
    LO* rower = nullptr;                // (orig3 D_0038AE90: the actor whose +0x1C90 is this boat: Murray, EOPID 1152)
    int rowMode = 0;                    // (the rower's +0x1C64: the clip 0..8)
    float tRow = -1.0f;                 // (when his stroke clip started)
};
std::unordered_map<const LO*, ROWBOAT3> s_mpboat;

constexpr float kStrokeClip = 0.25f;   // (the rower's clip until he is ready again: the reference's RAM, 12..18 vblanks)

LO* PloOpt(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    return v.n == -1 || v.n == 0 ? nullptr : PloFindRelative(plo, v.n);
}
LO* PloRef(LO* plo, const CRef& r)
{
    if (r.m_tagk == TAGK_S32) return r.m_n == -1 || r.m_n == 0 ? nullptr : PloFindRelative(plo, r.m_n);
    return dynamic_cast<LO*>(r.m_pbasic);
}
glm::mat4 MatWorld(ALO* palo)
{
    XF* pxf = PxfObj(palo);
    return pxf ? pxf->matWorld : glm::mat4(1.0f);
}
glm::vec3 PosOf(LO* plo)
{
    if (!plo) return glm::vec3(0.0f);
    if (FIsLoDerivedFrom(plo, S3CID(0x30)))
        if (XF* pxf = PxfObj(static_cast<ALO*>(plo))) return glm::vec3(pxf->matWorld[3]);
    return PosLoWorld(plo);
}
float YawOf(ALO* palo)
{
    const glm::mat4 m = MatWorld(palo);
    return std::atan2(m[0][1], m[0][0]);
}

void SetState(ROWBOAT3& b, int st);

void ApplyOption(ROWBOAT3& b, int eopid, const CRef& r)
{
    LO* plo = b.palo;
    auto g = [&] { return r.GAsFloat(); };
    switch (eopid)
    {
    case 1778: b.water.damp = r.GetVector(); break;
    case 1779: b.water.dragSpeed = g(); break;
    case 1780: b.water.bias = r.GetVector(); break;
    case 1781: b.water.dragBase = g(); break;
    case 1782: b.water.turnGain = g(); break;
    case 1783: b.amount = g(); break;
    case 1784: b.shift = g(); break;
    case 1785: b.restart = g(); break;
    case 1786: SetState(b, r.RefCoerceS32().m_n); break;
    case 1787: b.anchor = PloRef(plo, r); break;
    case 1788: b.follower = PloRef(plo, r); break;
    case 1789: b.limit = g(); break;
    case 1790: b.stiffness = g(); break;
    case 1791: b.damping = g(); break;
    case 1792: b.wake = PloRef(plo, r); break;
    default: break;
    }
}

// orig3 0x20ED28: the SO's init, the defaults (D_00325AF0 / D_00325B00), the hit points 100
void InitBoat(ROWBOAT3& b)
{
    ALO* palo = b.palo;
    palo->SetField(0, 0x170, 100u);
    palo->SetField(0, 0x16C, 100u);
    for (const OPT& o : palo->aopt)
    {
        if (o.aval.empty() || o.eopid < 1778 || o.eopid > 1792 || o.eopid == 1786) continue;
        const OPTVAL& v = o.aval[0];
        switch (o.eopid)
        {
        case 1778: b.water.damp = glm::vec3(v.ag[0], v.ag[1], v.ag[2]); break;
        case 1780: b.water.bias = glm::vec3(v.ag[0], v.ag[1], v.ag[2]); break;
        case 1787: b.anchor = PloOpt(palo, v); break;
        case 1788: b.follower = PloOpt(palo, v); break;
        case 1792: b.wake = PloOpt(palo, v); break;
        default:
        {
            CRef r;
            r.SetF32(v.ag[0]);
            ApplyOption(b, o.eopid, r);
            break;
        }
        }
    }
    // the SO's hit points as loaded (EOPIDs 937 / 938)
    for (const OPT& o : palo->aopt)
        if (!o.aval.empty() && o.eopid == 937)
        {
            palo->SetField(0, 0x170, uint32_t(o.aval[0].n));
            palo->SetField(0, 0x16C, uint32_t(o.aval[0].n));
        }
    if (FPrint())
        std::printf("row3: %d init: amount %.0f shift %.2f restart %.2f water bias %.2f damp %.3f %.3f %.3f drag %.2f %.2f turn %.1f rope %d - %d limit %.0f "
                    "stiffness %.2f damping %.2f hp %d\n", palo->oid, b.amount, b.shift, b.restart, b.water.bias.x, b.water.damp.x, b.water.damp.y, b.water.damp.z,
                    b.water.dragBase, b.water.dragSpeed, b.water.turnGain, b.anchor ? b.anchor->oid : -1, b.follower ? b.follower->oid : -1, b.limit, b.stiffness,
                    b.damping, int(palo->U32Field(0, 0x16C, 0)));
}

ROWBOAT3* Pboat(LO* plo)
{
    if (!plo || !g_psw || !FRowboat3(plo)) return nullptr;
    auto it = s_mpboat.find(plo);
    if (it != s_mpboat.end() && it->second.psw == g_psw) return &it->second;
    ROWBOAT3& b = s_mpboat[plo];
    b = ROWBOAT3{};
    b.palo = static_cast<ALO*>(plo);
    b.psw = g_psw;
    InitBoat(b);
    return &b;
}

// orig3 0x20F868
void SetState(ROWBOAT3& b, int st)
{
    if (st == b.state) return;
    ALO* palo = b.palo;
    if (b.state == 2)
    {
        // (orig3 0x20F868: the position (+0x48) and object (+0x50) records back to mode 1 (0x28F068), the turn free (0x23EC98 0))
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 1);
        if (XF* pxf = PxfObj(palo)) SetXfMode(palo, pxf, 1);
        SetAloCnstrAng(palo, 0);
    }
    if (FPrint()) std::printf("row3: %d state %d -> %d (t %.2f)\n", palo->oid, b.state, st, g_clock.t);
    b.state = st;
    switch (st)
    {
    case 2:
        // the records in mode 2 (moving by their velocities), the turn about z only (0x23EC98 4), the heading the boat's
        // (orig3 0x20F868 sets mode 2 whatever the mode was (0x28F068 returns only for the same mode): a record left in
        // mode 1 rests (mode 3) once still and the turn toward the heading then never moved it)
        if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, 2);
        if (XF* pxf = PxfObj(palo)) SetXfMode(palo, pxf, 2);
        SetAloCnstrAng(palo, 4);
        b.heading = YawOf(palo);
        WakeAlo(palo);
        break;
    case 3:
        Broadcast3(0xC, palo);   // (0x1854C8(GAME, 12): sunk: the mission's death)
        break;
    default: break;
    }
}

// the rower's clip (orig3 0x1E4900 on D_0038AE90: mode 0 clears the boat's ready flag's hold, a stroke's clears it until
// the clip's end)
void SetRowMode(ROWBOAT3& b, int mode)
{
    if (mode == b.rowMode) return;
    b.rowMode = mode;
    b.tRow = g_clock.t;
    b.active = mode == 0;
}

// orig3 0x20FC58: the oars: the heading from the stick (camera relative, 0x168108) while the rower strokes; with the
// rower ready, Cross a forward stroke, Square a backward one, else (moving faster than 150 along the boat) a turn's
// oar on the stick's side; the rower's clip D_00348040[row][side]
void UpdateOars(ROWBOAT3& b, PADCTL* ppad)
{
    ALO* palo = b.palo;
    const float desired = YawOf(palo);
    float value = desired;
    if (ppad && ppad->uJoy > 0.5f) value = ppad->radJoy;
    if (b.rowMode) b.heading = value;
    if (!b.active) return;
    const float turn = cmm::RadNormalize(value - desired);
    const int side = turn > 0.15f ? 1 : turn < -0.15f ? 2 : 0;
    int row;
    if (ppad && GrfPadctlTakePressed(ppad, 0x40, 0.15f)) { b.fForward = 1; row = 1; }
    else if (ppad && GrfPadctlTakePressed(ppad, 0x80, 0.15f)) { b.fBack = 1; row = 2; }
    else
    {
        // (0x23D888: the boat's velocity in its own frame)
        XF* pxf = PxfPos(palo);
        const float lane = pxf ? (glm::transpose(glm::mat3(MatWorld(palo))) * glm::vec3(pxf->vel)).x : 0.0f;
        row = !side ? 0 : lane > 150.0f ? 1 : lane < -150.0f ? 2 : 0;
    }
    static const int s_aaction[3][3] = {{0, 7, 8}, {5, 3, 1}, {6, 2, 4}};   // D_00348040
    if (const int action = s_aaction[row][side])
    {
        SetRowMode(b, 0);
        SetRowMode(b, action);
        if (FPrint()) std::printf("row3: %d stroke %d (row %d side %d) heading %.2f yaw %.2f t %.2f\n", palo->oid, action, row, side, b.heading, desired, g_clock.t);
    }
}

// the rower held in his seat (his place in the boat, at rest)
void SeatRower(ROWBOAT3& b)
{
    if (!b.rower || !FIsLoDerivedFrom(b.rower, S3CID(0x30))) return;
    ALO* pa = static_cast<ALO*>(b.rower);
    const glm::mat4 m = MatWorld(b.palo);
    SetAloPos(pa, glm::vec3(m * glm::vec4(0.0f, 0.0f, 72.0f, 1.0f)));
    SetAloRotation(pa, glm::mat3(m));
    SetAloVelocityTo(pa, glm::vec3(0.0f));
    SetAloAngVelocityTo(pa, glm::vec3(0.0f));
}

// orig3 0x20EE58 (vt slot 18)
void UpdateBoat(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    ROWBOAT3* pb = Pboat(plo);
    if (!pb) return;
    ROWBOAT3& b = *pb;
    // (debug: SLY3_ROWTEST=1: the boat given the pad once it is in the world, as SCPT 3282's 0x13)
    if (static const float s_tTest = std::getenv("SLY3_ROWTEST") ? float(std::atof(std::getenv("SLY3_ROWTEST"))) : -1.0f;
        s_tTest >= 0.0f && g_clock.t >= s_tTest && b.state != 2 && plo->fInWorld && g_psw && PloPoCurrent(g_psw) != plo)
    {
        EnablePo(g_psw, plo, 1);
        SetPoCurrent(g_psw, plo);
    }
    if (b.rower && (b.rower->paloParent != plo || !b.rower->fInWorld)) b.rower = nullptr;
    // (his seat each frame: natively his step code still moves him (Sly 3's state 23 has no motion of its own))
    SeatRower(b);
    // the rower ready again after his stroke's clip
    if (!b.active && b.rowMode && g_clock.t - b.tRow >= kStrokeClip) b.active = 1;
    if (b.state >= 0 && b.state < 2) return;
    PADCTL* ppad = PpadctlOfSo3(plo);
    if (ppad && ppad->nControl != 1) ppad = nullptr;
    if (ppad && GrfPadctlTakePressed(ppad, 0x200, 0.15f)) PsndStartSound(0x2A, 1.0f, 0, plo);   // the horn (0x2459F8 / 0x244F08)
    UpdateOars(b, ppad);
    if (b.cooldown > 0.0f) b.cooldown = std::max(0.0f, b.cooldown - g_clock.dt);   // 0x20FA10 (the flash's colour not drawn)
    if (FPrint())
    {
        static float s_t = -1.0f;
        if (g_clock.t - s_t >= 1.0f)
        {
            s_t = g_clock.t;
            const glm::vec3 p = PosOf(plo);
            XF* pxf = PxfPos(b.palo);
            const glm::vec3 v = pxf ? glm::vec3(pxf->vel) : glm::vec3(0.0f);
            if (b.rower)
                std::printf("row3: rower %d in world %d parent %d at %.0f %.0f %.0f\n", b.rower->oid, int(b.rower->fInWorld), b.rower->paloParent ? b.rower->paloParent->oid : -1,
                            PosOf(b.rower).x, PosOf(b.rower).y, PosOf(b.rower).z);
            else if (LO* pm = PloFindSwObjectByOid(g_psw, 26))
                std::printf("row3: no rower; Murray in world %d parent %d at %.0f %.0f %.0f\n", int(pm->fInWorld), pm->paloParent ? pm->paloParent->oid : -1, PosOf(pm).x, PosOf(pm).y, PosOf(pm).z);
            if (b.anchor && b.follower)
                std::printf("row3: rope %d (cid 0x%X) at %.0f %.0f %.0f - %d (cid 0x%X) at %.0f %.0f %.0f\n", b.anchor->oid, unsigned(b.anchor->Cid()), PosOf(b.anchor).x, PosOf(b.anchor).y,
                            PosOf(b.anchor).z, b.follower->oid, unsigned(b.follower->Cid()), PosOf(b.follower).x, PosOf(b.follower).y, PosOf(b.follower).z);
            std::printf("row3: %d t %.2f state %d pos %.0f %.0f %.0f vel %.0f %.0f %.0f heading %.2f yaw %.2f hp %d rope %.0f\n", plo->oid, g_clock.t, b.state, p.x, p.y, p.z, v.x,
                        v.y, v.z, b.heading, YawOf(b.palo), int(plo->U32Field(0, 0x16C, 0)), b.anchor && b.follower ? glm::length(PosOf(b.follower) - PosOf(b.anchor)) : 0.0f);
        }
    }
}

// orig3 0x20F3C0 (vt slot 74)
void BoatForces(ALO* palo)
{
    ApplyAloGravity(palo);   // 0x243738
    ROWBOAT3* pb = Pboat(palo);
    if (!pb || pb->state != 2) return;
    ROWBOAT3& b = *pb;
    const glm::vec3 fwd = glm::vec3(MatWorld(palo)[0]);   // (0x1C94E8 of D_00317530)
    if (b.fForward)
    {
        b.fForward = 0;
        AccelAloPos(palo, fwd * b.amount);   // 0x244828
    }
    else if (b.fBack)
    {
        b.fBack = 0;
        AccelAloPos(palo, fwd * -b.amount);
    }
    // the turn toward the heading (0x23EDE8 with the rotation about z and {+0x1C4, 0, 0.25})
    const glm::mat3 mat = cmm::BuildRotateMatrixAxis(b.heading, glm::vec3(0.0f, 0.0f, 1.0f));
    const CLQ clq{b.shift, 0.0f, 0.25f, 0};
    AccelAloTowardRotation(palo, &mat, &clq, glm::vec3(0.0f), nullptr);
    // the rope (0x20FB58): past its limit the boat pulled along the ends' difference
    if (b.anchor && b.follower)
    {
        const glm::vec3 d = PosOf(b.follower) - PosOf(b.anchor);
        const float len = glm::length(d);
        if (len > b.limit && len > 0.0f) AccelAloPos(palo, d * (b.stiffness * (1.0f - b.limit / len) + b.damping / len));
    }
}

// orig3 0x20F5F0 (vt slot 78): while rowed, after the hit time, a projectile's hit (not its own) takes its amount
// (all of it for a fatal one); at 0 the boat sinks (state 3), else broadcast 10 and the hit time
bool FAcceptBoatContact(ALO* palo, CTMSG* pctm)
{
    ROWBOAT3* pb = Pboat(palo);
    if (!pb) return false;
    ROWBOAT3& b = *pb;
    if (b.cooldown > 0.0f || b.state != 2) return false;
    LO* pfrom = pctm->ploSender;
    if (pfrom && FIsLoDerivedFrom(pfrom, 0x59) && PloProjectileLauncher(pfrom) == palo) return false;
    if (!(pctm->grfAccepted & 2)) return false;
    palo->SetField(0, 0x184, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pfrom) & 0xFFFFFFFFu));   // 0x1F4DD8
    int hp = int(palo->U32Field(0, 0x16C, 0));
    hp = pctm->nKill3 || pctm->fKill3 ? 0 : std::max(0, hp - std::abs(pctm->nState));
    palo->SetField(0, 0x16C, uint32_t(hp));
    if (FPrint()) std::printf("row3: %d hit by %d: %d left (t %.2f)\n", palo->oid, pfrom ? pfrom->oid : -1, hp, g_clock.t);
    if (hp <= 0) SetState(b, 3);
    else
    {
        Broadcast3(10, palo);   // (0x1854C8(GAME, 10))
        b.cooldown = b.restart;
    }
    return true;
}

// orig3 0x20EE28 -> 0x20F820 (vt slot 31): the hit points again, the rower's mode and the flash cleared, idle
void ResetBoat(LO* plo, LO*)
{
    ROWBOAT3* pb = Pboat(plo);
    if (!pb) return;
    plo->SetField(0, 0x16C, plo->U32Field(0, 0x170, 100));
    pb->cooldown = 0.0f;
    SetState(*pb, 1);
}

// orig3 0x20FE60 (vt slot 5): the base's, then events 6 / 7 track the object that holds it
void OnBoatMessage(LO* plo, int msgk, void* pv)
{
    using PFN5 = void (*)(LO*, int, void*);
    if (PFN5 pfn = reinterpret_cast<PFN5>(PvNativeFn(0x1C3E20))) pfn(plo, msgk, pv);
    ROWBOAT3* pb = Pboat(plo);
    if (!pb || !pv) return;
    struct TRACK { int key; LO* target; };
    const TRACK* pt = static_cast<const TRACK*>(pv);
    if (msgk == 6 && pt->target == plo) pb->tracked = pt->key;
    if (msgk == 7 && pt->target == plo && pt->key == pb->tracked) pb->tracked = 0;
}

struct INITROWBOAT3
{
    INITROWBOAT3()
    {
        RegisterLoUpdate(FN3_TAG | 0x20EE58, UpdateBoat);
        // the scripts' setters (every field option of the class)
        static void (*const s_apfn[15])(LO*, const CRef&) = {
#define ROWSET(E) [](LO* plo, const CRef& r) { if (ROWBOAT3* b = Pboat(plo)) ApplyOption(*b, E, r); }
            ROWSET(1778), ROWSET(1779), ROWSET(1780), ROWSET(1781), ROWSET(1782), ROWSET(1783), ROWSET(1784), ROWSET(1785),
            ROWSET(1786), ROWSET(1787), ROWSET(1788), ROWSET(1789), ROWSET(1790), ROWSET(1791), ROWSET(1792)
#undef ROWSET
        };
        for (int e = 1778; e <= 1792; ++e) RegisterObjectSetter3(e, s_apfn[e - 1778]);
        // Murray's boat (EOPID 1152, his +0x1C90; setter 0x1E47F0): his state 23 (0x1DE690): seated in it (the reference's
        // RAM: his parent the boat, his place (0, 0, 72) in it), held there (0x23EBF8 1: SetAloCnstrLin), at rest, the
        // clip mode 0; no boat: standing again (0x23EBF8 0, 0x1C7A00: no parent)
        RegisterObjectSetter3(1152, [](LO* plo, const CRef& r) {
            if (!plo || !FIsLoDerivedFrom(plo, S3CID(0x30))) return;
            ALO* pa = static_cast<ALO*>(plo);
            LO* pboat = PloRef(plo, r);
            for (auto& [p, b] : s_mpboat)
                if (b.rower == plo && p != pboat) b.rower = nullptr;
            ROWBOAT3* pb = pboat ? Pboat(pboat) : nullptr;
            if (FPrint()) std::printf("row3: %d's boat %d (t %.2f)\n", plo->oid, pboat ? pboat->oid : -1, g_clock.t);
            if (!pb)
            {
                SetAloCnstrLin(pa, 0);
                SetAloParent(pa, nullptr, nullptr, 3);
                return;
            }
            pb->rower = plo;
            pb->rowMode = -1;
            SetRowMode(*pb, 0);
            SetAloParent(pa, pb->palo, nullptr, 3);
            SetAloCnstrLin(pa, 1);
            SeatRower(*pb);
        });
        RegisterObjectGetter3(1786, [](LO* plo) { CRef r; ROWBOAT3* b = Pboat(plo); r.SetS32(b ? b->state : -1); return r; });
        RegisterObjectGetter3(1783, [](LO* plo) { CRef r; ROWBOAT3* b = Pboat(plo); r.SetF32(b ? b->amount : 0.0f); return r; });
        RegisterObjectGetter3(1787, [](LO* plo) { CRef r; ROWBOAT3* b = Pboat(plo); if (b && b->anchor) r.SetBasic(b->anchor); return r; });
        RegisterObjectGetter3(1788, [](LO* plo) { CRef r; ROWBOAT3* b = Pboat(plo); if (b && b->follower) r.SetBasic(b->follower); return r; });
        RegisterObjectGetter3(1789, [](LO* plo) { CRef r; ROWBOAT3* b = Pboat(plo); r.SetF32(b ? b->limit : 0.0f); return r; });
    }
} s_initrowboat3;
NATIVEFN3(0x20F3C0, BoatForces);
NATIVEFN3(0x20F5F0, FAcceptBoatContact);
NATIVEFN3(0x20EE28, ResetBoat);
NATIVEFN3(0x20FE60, OnBoatMessage);
}  // namespace

bool FRowboat3(const LO* plo) { return plo && plo->pcls && FIsLoDerivedFrom(plo, CID_ROWBOAT); }

// orig3 0x20F738 (vt slot 83): rowed while a pad has it (state 2), else idle (1); the SO's pad take is po.cpp's
void OnRowboatEnable3(LO* plo, int n)
{
    if (ROWBOAT3* pb = Pboat(plo)) SetState(*pb, n == 1 ? 2 : 1);
}

// (the harness, route.cpp's "row" step) the stick and the strokes that row the current player's boat through task
// TASK's trigger volumes in order (m3_heist: 3281 94593..94595, 3285 94596..94598, 3287 94600..94602: down the river
// away from the windmill's beam), the stick in the pad's units (x right, y up) from the camera's heading
bool FRowStick3(LO* plo, int oidTask, float* px, float* py, bool* pfStroke)
{
    ROWBOAT3* pb = Pboat(plo);
    if (!pb || pb->state != 2 || !g_psw) return false;
    static const struct { int task, oid0; } s_atask[] = {{3281, 94593}, {3285, 94596}, {3287, 94600}};
    int oid0 = -1;
    for (const auto& t : s_atask)
        if (t.task == oidTask) oid0 = t.oid0;
    if (oid0 == -1) return false;
    static int s_task = -1, s_i = 0;
    if (s_task != oidTask) s_task = oidTask, s_i = 0;
    const glm::vec3 pos = PosOf(plo);
    LO* pvol = nullptr;
    while (s_i < 3)
    {
        pvol = PloFindSwObjectByOid(g_psw, oid0 + s_i);
        if (pvol && glm::length(glm::vec2(PosOf(pvol) - pos)) > 300.0f) break;
        ++s_i;
        pvol = nullptr;
    }
    if (!pvol) pvol = PloFindSwObjectByOid(g_psw, oid0 + 2);
    if (!pvol) return false;
    const glm::vec2 d = glm::vec2(PosOf(pvol) - pos);
    const float radWant = std::atan2(d.y, d.x);
    if (static int s_c = 0; FPrint() && (s_c++ % 60) == 0)
        std::printf("row3: route task %d toward %d at %.0f %.0f (boat %.0f %.0f)\n", oidTask, pvol->oid, PosOf(pvol).x, PosOf(pvol).y, pos.x, pos.y);
    const float radCam = std::atan2(g_cm.mat[0][1], g_cm.mat[0][0]);
    const float radStick = radWant - radCam + 1.5707964f;   // (po.cpp: the stick's angle + the camera's - pi/2)
    *px = std::cos(radStick);
    *py = std::sin(radStick);
    // (a stroke when the boat points within 60 degrees of the way, else the oars only turn it)
    *pfStroke = std::fabs(cmm::RadNormalize(radWant - YawOf(pb->palo))) < 1.05f;
    return true;
}

bool FBoatWater3(const LO* plo, BOATWATER3* pbw)
{
    ROWBOAT3* pb = Pboat(const_cast<LO*>(plo));
    if (!pb) return false;
    *pbw = pb->water;
    return true;
}
