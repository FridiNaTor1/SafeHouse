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
// Not ported: the oars' splashes 0x20F508 (their points +0x1F8 / +0x1FC are bound; the PE +0x200), the hit flash's colour (+0xC0), the
// horn's camera shake (0x168670).
// Debug: SLY3_ROWPRINT=1 (states, strokes, every second the boat), SLY3_ROWTEST=t (h_ext: the boat given the pad after t seconds).

#include "rowboat3.h"

#include "../activate.h"
#include "../brx.h"
#include "../alo.h"
#include "../so.h"
#include "../clock.h"
#include "../cm.h"
#include "../cplcy.h"
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
#include "../gang.h"
#include "../lox.h"
#include "../pe.h"
#include "../stepphys.h"
#include "../asega.h"
#include "../actanim.h"
#include "../animfind.h"
#include "sly3.h"
#include "task3.h"
#include "trigger3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

void VehicleAccelRotation(ALO* palo, const glm::vec3& v);   // vehicle.cpp (orig3 0x244890)
void BindLoPartsTable3(ALO* palo, int c, uint32_t vaTable);   // loaders.cpp (orig3 0x1C8970)
glm::vec3 VelBallisticLaunch(float gPitch, const glm::vec3& posStart, const glm::vec3& pos, const glm::vec3& accel);   // launcher.cpp (orig3 0x212480)

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
    LO* ploWater = nullptr;             // +0x1F4 (messages 6 / 7): the water it is in
    LO* wake = nullptr;                 // +0x200 (1792): the oars' splash PE
    float heading = 0.0f;               // +0x204
    int fForward = 0, fBack = 0;        // +0x208 / +0x20C: a stroke to apply
    LO* rower = nullptr;                // (orig3 D_0038AE90: the actor whose +0x1C90 is this boat: Murray, EOPID 1152)
    int rowMode = 0;                    // (the rower's +0x1C64: the clip 0..8)
    float tRow = -1.0f;                 // (when his stroke clip started)
    CTRLPOS* pctrlPos = nullptr;        // +0x1D0: state 1's hold of the position record (0x130078 mode 1, 0x13, state 2)
    CTRLPOS* pctrlRot = nullptr;        // +0x1D4: of the rotation record (its Euler angles, 0x11D3C0)
};
std::unordered_map<const LO*, ROWBOAT3> s_mpboat;

int s_cFrameTrace = 0;   // (debug: SLY3_ROWPRINT=2, frames traced after a state change)
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
    // the post-load 0x20EDE0's parts (0x1C8970(p, 3, D_00347FF0, 0)): the records of tags 0x77B / 0x77C / 0x77D at
    // +0x1EC (the rower's seat) / +0x1F8 / +0x1FC (the oars' splash points of 0x20F508: the splashes not ported); its
    // registration 0x1F4670 is lookat.cpp's (PloNearestCharacter3)
    BindLoPartsTable3(palo, 3, 0x347FF0);
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
    if (b.state == 1)
    {
        // (0x1309D8 DeleteCtrl on +0x1D0 / +0x1D4)
        if (b.pctrlPos) DeleteCtrl(reinterpret_cast<CTRL**>(&b.pctrlPos));
        if (b.pctrlRot) DeleteCtrl(reinterpret_cast<CTRL**>(&b.pctrlRot));
    }
    if (FPrint()) std::printf("row3: %d state %d -> %d (t %.2f)\n", palo->oid, b.state, st, g_clock.t);
    b.state = st;
    s_cFrameTrace = 30;
    switch (st)
    {
    case 1:
        // the idle hold: a position controller on each record at its value now (0x130078(p, +0x48, 1, 0x13, 2, its
        // position) / (p, +0x4C, 1, 0x13, 2, 0x11D3C0 its Euler angles))
        // (the jolly boat 0x54 inherits it: the reference holds it (`--warp 31:4060`: SCPT 4063 puts it in state 1 at
        // z 0.00, where it stays). There Murray does not reach the ship either: 4063 clears his boat, his state 23's
        // exit (0x1DE690: 0x1C7A00(p, 0, 0), grf 0) leaves him at his seat's place (0, 0, 72) taken as a world place,
        // he falls from the world's origin, dies and is put back in the boat (vblank 4230) to row again. Natively (no
        // state 23) he is pushed off the held boat into the water: m5_jollyboat's 4062 is forced in the survey both
        // ways. SLY3_ROWHOLD54=0: not for 0x54 (the old behaviour), SLY3_ROWNOHOLD=1: off for both)
        if (std::getenv("SLY3_ROWNOHOLD")) break;
        if (palo->Cid() != S3CID(0x53))
            if (const char* sz = std::getenv("SLY3_ROWHOLD54"); sz && *sz == '0') break;
        // (Sly 3's record mode 0x13 is the native 0x12 XFM_Spring: the records spring back to the held value; passed raw
        // it was the native SpringRest, which froze them (AUDIT5 MODES5-04))
        if (XF* pxf = PxfPos(palo)) b.pctrlPos = PctrlNewPos(palo, pxf, 1, Xfm2FromXfm3(0x13), 2, VecXfValue(pxf, pxf->mat));
        if (XF* pxf = PxfRot(palo)) b.pctrlRot = PctrlNewPos(palo, pxf, 1, Xfm2FromXfm3(0x13), 2, VecXfValue(pxf, pxf->mat));
        break;
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

// the rower's clip (orig3 0x1E4900 on D_0038AE90, Murray's +0x1C64): his clip +0x1C6C[mode] (mode 0 the idle,
// looping (grf 9), a stroke's once (1)), the boat he rows (+0x1C90) ready (+0x1F0) only in mode 0; the stroke's end
// (0x1E49B8, his state 23's update) or its clip's event 0x247 makes it ready again
void SetRowMode(ROWBOAT3& b, int mode)
{
    MURRAY* pm = b.rower ? PmurrayFromLo(b.rower) : nullptr;
    if (pm) { SetMurrayRowClip3(pm, mode); return; }
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

LO* PloPartAt(ALO* palo, uint32_t ib)
{
    for (const auto& [i, plo] : palo->apart)
        if (i == ib) return plo;
    return nullptr;
}
XF* PxfPartAt(ALO* palo, uint32_t ib)
{
    for (const auto& [i, pxf] : palo->apartxf)
        if (i == ib) return pxf;
    return nullptr;
}

// orig3 0x20F508 (Murray's clip event 0x248, a stroke's oar in the water): the PE +0x200 (EOPID 1792), else the
// splash PE +0x150 of the water the boat is in (+0x1F4, messages 6 / 7); D_00348018[the rower's clip] says which
// oar: 1 the left (the record +0x1F8, tag 0x77C), 2 the right (+0x1FC, 0x77D); for each the PE moved to the record's
// world place (slot 34) and started once (0x1FE728(pe, 1)), its frame (+0xE0) out of it meanwhile
void SplashOars(ROWBOAT3& b, int mode)
{
    LO* ppe = b.wake;
    if (!ppe && b.ploWater)
        if (const int oid = static_cast<int>(b.ploWater->U32Field(0, 0x150, 0)); oid > 0) ppe = PloFindSwNearest(b.ploWater, oid);
    if (!ppe || ppe->Cid() != 0x2D) return;
    static const int s_agrf[9] = {0, 1, 1, 2, 2, 3, 3, 3, 3};   // D_00348018
    const int grf = static_cast<unsigned>(mode) < 9 ? s_agrf[mode] : 0;
    for (int i = 0; i < 2; ++i)
    {
        if (!(grf & (1 << i))) continue;
        XF* pxf = PxfPartAt(b.palo, FN3_TAG | (i ? 0x1FCu : 0x1F8u));
        if (!pxf) continue;
        SetLoxPos(static_cast<LOX*>(ppe), glm::vec3(pxf->matWorld[3]));
        TriggerPe(ppe);
        if (FPrint()) std::printf("row3: %d splash %d (clip %d) pe %d at %.0f %.0f %.0f t %.2f\n", b.palo->oid, i, mode, ppe->oid,
                                  pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z, g_clock.t);
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
    // the rower ready again after his stroke's clip (no Murray: by the clip's length seen in the reference's RAM)
    if (!b.active && b.rowMode && !(b.rower && PmurrayFromLo(b.rower)) && g_clock.t - b.tRow >= kStrokeClip) b.active = 1;
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
    ROWBOAT3* pb = Pboat(palo);
    ApplyAloGravity(palo);   // 0x243738
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
    struct TRACK { LO* water; LO* target; };
    const TRACK* pt = static_cast<const TRACK*>(pv);
    if (FPrint() && (msgk == 6 || msgk == 7)) std::printf("row3: %d message %d water %d target %d\n", plo->oid, msgk, pt->water ? pt->water->oid : -1, pt->target ? pt->target->oid : -1);
    if (msgk == 6 && pt->target == plo) pb->ploWater = pt->water;
    if (msgk == 7 && pt->target == plo && pt->water == pb->ploWater) pb->ploWater = nullptr;
}

// ---- the jolly boat (class 0x54 on the rowboat, orig3 0x20FEF0..0x210850; p_ext's Ep5 jobs 4044..4058) ----
// The rowboat's half is the code above (the 0x53 record, its slots inherited). Its own: the init 0x20FEF0 (defaults),
// the options 1793..1806, the update 0x2100B8 (the rowboat's 0x20EE58, then the cannon 0x2100E8), the cannon's states
// 0x210410, the reset 0x210008 (slot 31: the rowboat's 0x20EE28, then state 0), the release 0x210038 (slot 32: state
// -1) and the follow camera 0x210068 (slot 82: a focus of kind 15, vtable 0x362A50: 0x2106A8 / 0x210790).
// The cannon: R1 held (the input interface +0x134's +0x18 bit 3) shows the aim mark (+0x218) ahead of the boat and walks
// it out along +0x240 (600 + 2500 t, to +0x250's 2600) plus +0x278 (the boat's speed through +0x260 into +0x270's
// range, at the press); let go or at the end: a shot from the proxy +0x224 at the muzzle +0x21C, thrown onto the mark
// (0x212480 with +0x220's pitch share), then 0.5 s (+0x22C) with R1 up before the next (state 3 -> 0). (MODES-09 / 10)
struct JOLLY3
{
    SW* psw = nullptr;
    int state = -2;                 // +0x210 (0x20FEF0's -1; -2: not set yet)
    LO* mark = nullptr;             // +0x218 (EOPID 1793): the aim mark
    LO* muzzle = nullptr;           // +0x21C (1794)
    int noise = 0;                  // +0x27C (1795): the shot's alert (0x25D940(G, +0x27C, -1, p, p, 1))
    float pitch = 0.4f;             // +0x220 (1796)
    LO* proxy = nullptr;            // +0x224 (1797): the shots' proxy
    int sound = 0;                  // +0x228 (1798): the aim's loop
    float cooldown = 0.5f;          // +0x22C (1799)
    float tAim = 0.0f, tShot = 0.0f;   // +0x230 / +0x234
    glm::vec3 reach{600.0f, 2500.0f, 0.0f};    // +0x240
    cmm::LM lmReach{600.0f, 2600.0f};          // +0x250
    glm::vec3 lead{0.0f, 1.0f, 0.0f};          // +0x260 (1804)
    cmm::LM lmLead{0.0f, 1000.0f};             // +0x270 (1805)
    float lead0 = 0.0f;             // +0x278
    int fEnabled = 1;               // +0x284 (1806)
    glm::vec3 camOrigin{0.0f, 0.0f, 20.0f};       // +0x290 (1800)
    glm::vec3 camOffset{-270.0f, 0.0f, 250.0f};   // +0x2A0 (1801)
    float camTurn = 1.25f;                        // +0x2B0 (1802)
    cmm::SMP camSmp{2.0f, 0.0f, 0.6f};            // +0x2B4 (1803)
    CSndRef sndAim;                 // +0x280
    bool fCam = false;
};
std::unordered_map<const LO*, JOLLY3> s_mpjolly;
constexpr int CID_JOLLY = 0x54;

void ApplyJollyOption(LO* plo, JOLLY3& j, int eopid, const CRef& r)
{
    auto g = [&] { return r.GAsFloat(); };
    switch (eopid)
    {
    case 1793: j.mark = PloRef(plo, r); break;
    case 1794: j.muzzle = PloRef(plo, r); break;
    case 1795: j.noise = r.RefCoerceS32().m_n; break;
    case 1796: j.pitch = g(); break;
    case 1797: j.proxy = PloRef(plo, r); break;
    case 1798: j.sound = r.RefCoerceS32().m_n; break;
    case 1799: j.cooldown = g(); break;
    case 1800: j.camOrigin = r.GetVector(); break;
    case 1801: j.camOffset = r.GetVector(); break;
    case 1802: j.camTurn = g(); break;
    case 1803: { const glm::vec3 v = r.GetVector(); j.camSmp = cmm::SMP{v.x, v.y, v.z}; break; }
    case 1804: j.lead = r.GetVector(); break;
    case 1805: { const glm::vec3 v = r.GetVector(); j.lmLead = cmm::LM{v.x, v.y}; break; }
    case 1806: j.fEnabled = r.RefCoerceS32().m_n; break;
    default: break;
    }
}

JOLLY3* Pjolly(LO* plo)
{
    if (!plo || !g_psw || !plo->pcls || !FIsLoDerivedFrom(plo, CID_JOLLY)) return nullptr;
    auto it = s_mpjolly.find(plo);
    if (it != s_mpjolly.end() && it->second.psw == g_psw) return &it->second;
    JOLLY3& j = s_mpjolly[plo];
    j = JOLLY3{};
    j.psw = g_psw;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty() || o.eopid < 1793 || o.eopid > 1806) continue;
        const OPTVAL& v = o.aval[0];
        CRef r;
        switch (o.eopid)
        {
        case 1793: case 1794: case 1797: j.mark = o.eopid == 1793 ? PloOpt(plo, v) : j.mark;
            if (o.eopid == 1794) j.muzzle = PloOpt(plo, v);
            if (o.eopid == 1797) j.proxy = PloOpt(plo, v);
            continue;
        case 1800: case 1801: case 1803: case 1804: case 1805:
            r.m_tagk = TAGK_Vector;
            r.m_vector = glm::vec4(v.ag[0], v.ag[1], v.ag[2], 0.0f);
            break;
        case 1795: case 1798: case 1806: r.SetS32(v.n); break;
        default: r.SetF32(v.ag[0]); break;
        }
        ApplyJollyOption(plo, j, o.eopid, r);
    }
    if (FPrint())
        std::printf("row3: jolly %d init: mark %d muzzle %d proxy %d pitch %.2f cooldown %.2f sound %d enabled %d\n", plo->oid, j.mark ? j.mark->oid : -1,
                    j.muzzle ? j.muzzle->oid : -1, j.proxy ? j.proxy->oid : -1, j.pitch, j.cooldown, j.sound, j.fEnabled);
    return &j;
}

glm::vec3 PosLocal(LO* plo, const glm::vec3& v) { return glm::vec3(MatWorld(static_cast<ALO*>(plo)) * glm::vec4(v, 1.0f)); }   // 0x1C9428
void PlaceAt(LO* plo, const glm::vec3& pos, const glm::mat3* pmat)
{
    float a[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    glm::mat3 m = pmat ? *pmat : glm::mat3(sly3::MatLoWorld3(plo));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) a[r * 3 + c] = m[r][c];
    sly3::PlaceLo3(plo, a, pos);
}
// the mark's place now: +0x240 at the time since the press, in +0x250, plus the lead, ahead of the boat (0x2105B0)
glm::vec3 PosJollyMark(LO* plo, const JOLLY3& j)
{
    const float t = g_clock.t - j.tAim;
    const float g = std::clamp(j.reach.x + t * (j.reach.y + t * j.reach.z), j.lmReach.gMin, j.lmReach.gMax);
    return PosLocal(plo, glm::vec3(g + j.lead0, 0.0f, 0.0f));
}

// orig3 0x210410
void SetJollyState(LO* plo, JOLLY3& j, int st)
{
    if (j.state == st) return;
    if (j.state == 1)
    {
        if (j.mark && j.mark->fInWorld) RemoveLo(g_psw, j.mark);   // vt slot 9
        if (j.sndAim) { j.sndAim->Stop(0.0f); j.sndAim = CSndRef(); }
    }
    if (FPrint()) std::printf("row3: jolly %d cannon %d -> %d (t %.2f)\n", plo->oid, j.state, st, g_clock.t);
    j.state = st;
    if (st != 1) return;
    if (j.mark)
    {
        // the lead from the boat's speed along x (0x23D888), then the mark at its start, in the world (vt slots 34 / 8)
        XF* pxf = PxfPos(static_cast<ALO*>(plo));
        const float x = pxf ? glm::dot(glm::vec3(pxf->vel), glm::vec3(MatWorld(static_cast<ALO*>(plo))[0])) : 0.0f;
        j.lead0 = std::clamp(j.lead.x + x * (j.lead.y + x * j.lead.z), j.lmLead.gMin, j.lmLead.gMax);
        PlaceAt(j.mark, PosLocal(plo, glm::vec3(j.lmReach.gMin + j.lead0, 0.0f, 0.0f)), nullptr);
        if (!j.mark->fInWorld) AddLo(g_psw, j.mark);
    }
    j.tAim = g_clock.t;
    if (CSnd* psnd = PsndNewPlain(j.sound))   // 0x2459F8 / 0x244F08(h, 0)
    {
        psnd->SetSource(nullptr);
        j.sndAim = CSndRef(psnd);
    }
}

// orig3 0x2100E8 (the second half of the update 0x2100B8)
void UpdateJollyCannon(LO* plo, JOLLY3& j)
{
    if (!j.fEnabled) return;
    PADCTL* ppad = PpadctlOfSo3(plo);
    bool fHeld = ppad && ppad->nControl == 1 && (ppad->grfbtn & 0x8);   // R1
    // (debug: SLY3_JOLLYTEST=1: R1 held 1 s of every 3 while the boat has the pad)
    if (static const bool s_fTest = std::getenv("SLY3_JOLLYTEST") != nullptr; s_fTest && ppad && ppad->nControl == 1)
        fHeld = std::fmod(g_clock.t, 3.0f) < 1.0f;
    switch (j.state)
    {
    case 0:
        if (fHeld) SetJollyState(plo, j, 1);
        break;
    case 1:
    {
        if (j.mark) PlaceAt(j.mark, PosJollyMark(plo, j), nullptr);
        if (fHeld)
        {
            const float t = g_clock.t - j.tAim;
            const float g = std::clamp(j.reach.x + t * (j.reach.y + t * j.reach.z), j.lmReach.gMin, j.lmReach.gMax);
            if (g < j.lmReach.gMax) break;
        }
        SetJollyState(plo, j, 2);
        break;
    }
    case 2:
        if (j.proxy && j.muzzle)
            if (LO* po = PloAcquireProxyObject(g_psw, j.proxy, false))   // 0x1FBE30(+0x224, 0)
            {
                const glm::mat4 mMuzzle = sly3::MatLoWorld3(j.muzzle);
                const glm::mat3 mat(mMuzzle);
                PlaceAt(po, glm::vec3(mMuzzle[3]), &mat);   // vt slots 34 / 35
                if (FIsLoDerivedFrom(po, sly3::CID_ALO))
                {
                    ALO* pa = static_cast<ALO*>(po);
                    const SOST* ps = Psost(pa);
                    SetAloVelocityTo(pa, VelBallisticLaunch(j.pitch, glm::vec3(mMuzzle[3]), PosJollyMark(plo, j), ps ? ps->dvGravity : glm::vec3(0.0f)));
                    if (FIsLoDerivedFrom(po, 0x59)) SetProjectileLauncher(pa, plo);   // 0x14D1F0
                }
                if (FPrint()) std::printf("row3: jolly %d shot %d (cid 0x%X)\n", plo->oid, po->oid, unsigned(po->Cid()));
            }
        j.tShot = g_clock.t;
        SetJollyState(plo, j, 3);
        break;
    case 3:
        if (!fHeld && j.cooldown < g_clock.t - j.tShot) SetJollyState(plo, j, 0);
        break;
    default: break;
    }
}

// the follow camera (orig3 0x210068 -> 0x210658: a focus of kind 15; its update 0x210790): the heading smoothed toward
// the boat's back (0x11E840 with +0x2B4), the goal the boat's +0x290 plus +0x2A0's length back along it and its height;
// the eye (kept: set to the goal by the start 0x2106A8) moved toward it around the world's solids (0x167660,
// PosCmAvoid: AUDIT5 MODES5-09), looking at that point (0x166E48)
struct CPJOLLY3 : CPLCY
{
    LO* pboat = nullptr;
    float heading = 0.0f;
    glm::vec3 posEye{0.0f};
    uint32_t VtOrig() const override { return 0x362A50; }
    static glm::vec3 PosGoal(LO* pboat, const JOLLY3& j, float heading, glm::vec3* porigin)
    {
        const glm::vec3 origin = PosLocal(pboat, j.camOrigin);
        const float len = std::sqrt(j.camOffset.x * j.camOffset.x + j.camOffset.y * j.camOffset.y);
        if (porigin) *porigin = origin;
        return origin + glm::vec3(std::cos(heading) * len, std::sin(heading) * len, j.camOffset.z);   // 0x11FD70
    }
    void Update(CPDEFI*, const CMJOY*) override
    {
        JOLLY3* pj = Pjolly(pboat);
        if (!pj || !pboat->fInWorld) return;
        ALO* palo = static_cast<ALO*>(pboat);
        const float target = YawOf(palo) + 3.14159265f;
        const float d = cmm::GSmooth(0.0f, cmm::RadNormalize(target - heading), g_clock.dt, &pj->camSmp, nullptr);
        heading = cmm::RadNormalize(heading + d);
        glm::vec3 origin;
        const glm::vec3 goal = PosGoal(pboat, *pj, heading, &origin);
        posEye = std::getenv("SLY3_JOLLYCAMNOAVOID") ? goal : PosCmAvoid(&g_cm, posEye, goal);
        SetCmLookAt(&g_cm, posEye, origin, 0.0f, 0.0f);
    }
};
CPJOLLY3 s_cpjolly;

void SyncJollyCamera(LO* plo, JOLLY3& j)
{
    const bool fCur = g_psw && PloPoCurrent(g_psw) == plo && plo->fInWorld && !std::getenv("SLY3_JOLLYNOCAM");
    if (fCur && !j.fCam)
    {
        s_cpjolly.pboat = plo;
        s_cpjolly.heading = YawOf(static_cast<ALO*>(plo)) + 3.14159265f;   // (0x2106A8)
        s_cpjolly.posEye = CPJOLLY3::PosGoal(plo, j, s_cpjolly.heading, nullptr);
        SetCmPolicy(&g_cm, 7, &s_cpjolly, plo, nullptr);
        j.fCam = true;
    }
    else if (!fCur && j.fCam)
    {
        RevokeCmPolicy(&g_cm, 0xF, 7, &s_cpjolly, plo, nullptr);
        j.fCam = false;
    }
}

void ResetJolly(LO* plo, LO* pproxy);
// orig3 0x2100B8 (vt slot 18)
void UpdateJolly(LO* plo)
{
    // (the rowboat's update only while rowed: run idle (the SO's base update 0x23C660, the rower's seat) it let Sly fall
    // through the deck at p_ext's boarding (m5_treasure 3997's SCPT 0x36 to the seat 50656 never ended); before this
    // port the jolly boat had no update at all)
    if (ROWBOAT3* pb = Pboat(plo); pb && pb->state == 2) UpdateBoat(plo);
    if (static const bool s_f2 = std::getenv("SLY3_ROWPRINT") && std::getenv("SLY3_ROWPRINT")[0] == '2'; s_f2 && s_cFrameTrace > 0)
    {
        --s_cFrameTrace;
        const glm::vec3 p = PosOf(plo);
        LO* pm = PloFindSwObjectByOid(g_psw, 26);
        const glm::vec3 q = pm ? PosOf(pm) : glm::vec3(0.0f);
        std::printf("row3: trace t %.3f boat %.1f %.1f %.1f Murray %.1f %.1f %.1f parent %d\n", g_clock.t, p.x, p.y, p.z, q.x, q.y, q.z,
                    pm && pm->paloParent ? pm->paloParent->oid : -1);
    }
    JOLLY3* pj = Pjolly(plo);
    if (!pj) return;
    // (the world's start runs every object's slot 31 (orig3 0x1FC868): this reset's state 0; natively only the hack
    // objects get that call (guardstart.cpp SendStartEvents3), so the first update stands for it)
    if (pj->state == -2) ResetJolly(plo, nullptr);
    SyncJollyCamera(plo, *pj);
    UpdateJollyCannon(plo, *pj);
}

// orig3 0x210008 (vt slot 31): the rowboat's reset 0x20EE28, then the cannon's state 0. The fnmap paired it with Sly 2's
// ResetVehiclePlacement (0x1D1910, score 96): the jolly boat ran Sly 2's vehicle reset (MODES-10)
void ResetJolly(LO* plo, LO* pproxy)
{
    ResetBoat(plo, pproxy);
    if (JOLLY3* pj = Pjolly(plo)) SetJollyState(plo, *pj, 0);
}
// orig3 0x210038 (vt slot 32: out of the world): the cannon's state -1
void ReleaseJolly(LO* plo)
{
    if (JOLLY3* pj = Pjolly(plo))
    {
        SetJollyState(plo, *pj, -1);
        if (pj->fCam) { RevokeCmPolicy(&g_cm, 0xF, 7, &s_cpjolly, plo, nullptr); pj->fCam = false; }
    }
}

struct INITROWBOAT3
{
    INITROWBOAT3()
    {
        RegisterLoUpdate(FN3_TAG | 0x2100B8, UpdateJolly);
        for (int e = 1793; e <= 1806; ++e)
        {
            static void (*const s_apfn[14])(LO*, const CRef&) = {
#define JOLLYSET(E) [](LO* plo, const CRef& r) { if (JOLLY3* j = Pjolly(plo)) ApplyJollyOption(plo, *j, E, r); }
                JOLLYSET(1793), JOLLYSET(1794), JOLLYSET(1795), JOLLYSET(1796), JOLLYSET(1797), JOLLYSET(1798), JOLLYSET(1799),
                JOLLYSET(1800), JOLLYSET(1801), JOLLYSET(1802), JOLLYSET(1803), JOLLYSET(1804), JOLLYSET(1805), JOLLYSET(1806)
#undef JOLLYSET
            };
            RegisterObjectSetter3(e, s_apfn[e - 1793]);
        }
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
            LO* pboat = PloRef(plo, r);
            if (FPrint()) std::printf("row3: %d's boat %d (t %.2f)\n", plo->oid, pboat ? pboat->oid : -1, g_clock.t);
            if (MURRAY* pm = PmurrayFromLo(plo)) { SetMurrayBoat3(pm, pboat); return; }
            // (not Murray: the old hold, no state of his own)
            ALO* pa = static_cast<ALO*>(plo);
            for (auto& [p, b] : s_mpboat)
                if (b.rower == plo && p != pboat) b.rower = nullptr;
            ROWBOAT3* pb = pboat ? Pboat(pboat) : nullptr;
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
// orig3 0x20F798 / 0x20F7D8 (vt slots 34 / 35, also the rowboat-derived 0x54's): the ALO's position / rotation, then
// the idle hold (+0x1D0 / +0x1D4) moved to the record's new value (0x12F4F8 SetCtrlPos)
void SetBoatPos(ALO* palo, const glm::vec3& pos)
{
    SetAloPos(palo, pos);
    auto it = s_mpboat.find(palo);
    if (it == s_mpboat.end() || !it->second.pctrlPos) return;
    if (XF* pxf = PxfPos(palo)) SetCtrlPos(it->second.pctrlPos, VecXfValue(pxf, pxf->mat));
}
void SetBoatRotation(ALO* palo, const glm::mat3& mat)
{
    SetAloRotation(palo, mat);
    auto it = s_mpboat.find(palo);
    if (it == s_mpboat.end() || !it->second.pctrlRot) return;
    if (XF* pxf = PxfRot(palo)) SetCtrlPos(it->second.pctrlRot, VecXfValue(pxf, pxf->mat));
}
NATIVEFN3(0x20F798, SetBoatPos);
NATIVEFN3(0x20F7D8, SetBoatRotation);
NATIVEFN3(0x20F3C0, BoatForces);
NATIVEFN3(0x20F5F0, FAcceptBoatContact);
NATIVEFN3(0x20EE28, ResetBoat);
NATIVEFN3(0x20FE60, OnBoatMessage);
NATIVEFN3(0x210008, ResetJolly);
NATIVEFN3(0x210038, ReleaseJolly);
}  // namespace

// ---- Murray's boat state 23 (orig3 0x1DE690 case 23, 0x1E47F0 / 0x1E4900 / 0x1E49B8, his clip events 0x247 / 0x248) ----

// orig3 0x1E4900: his rowing clip +0x1C6C[mode] (0..8; else none: -1 the state's end), looping for mode 0 (grf 9),
// once for a stroke (1), his state machine let go (0x23AC58); the boat he rows (+0x1C90) ready (+0x1F0) in mode 0 only
void SetMurrayRowClip3(MURRAY* p, int mode)
{
    if (!p || mode == p->nRow1C64) return;
    p->nRow1C64 = mode;
    p->tRow1C68 = g_clock.t;
    ASEG* paseg = static_cast<unsigned>(mode) < 9 ? dynamic_cast<ASEG*>(PloPartAt(p->palo, FN3_TAG | (0x1C6Cu + 4u * unsigned(mode)))) : nullptr;
    SetActorAnimPp(&p->pasega, &p->psma, p->palo, paseg, 0.0f, 1.0f, mode == 0 ? 9 : 1, nullptr, -1, 4, false);
    if (ROWBOAT3* pb = p->pboat1C90 ? Pboat(p->pboat1C90) : nullptr)
    {
        pb->rowMode = mode;
        pb->tRow = g_clock.t;
        pb->active = mode == 0;
    }
    if (FPrint()) std::printf("row3: Murray's clip %d (aseg %d) t %.2f\n", mode, paseg ? paseg->oid : -1, g_clock.t);
}

// orig3 0x1E47F0 (EOPID 1152's setter): another boat: the old one's oars shown again (mesh groups 0x779 / 0x77A,
// 0x23FE38), his own (+0x1C94 / +0x1C98, oids 1913 / 1914) out of the world (slot 9), state 23 left (0x1DE690(0));
// the new one's hidden (0x23FEC0), his added (slot 8), state 23
void SetMurrayBoat3(MURRAY* p, LO* pboat)
{
    if (!p || pboat == p->pboat1C90) return;
    LO* aploOar[2] = {PloPartAt(p->palo, FN3_TAG | 0x1C94u), PloPartAt(p->palo, FN3_TAG | 0x1C98u)};
    if (LO* pold = p->pboat1C90)
    {
        if (FIsLoDerivedFrom(pold, S3CID(0x30)))
        {
            ShowAloGlobs(static_cast<ALO*>(pold), 0x779, true);
            ShowAloGlobs(static_cast<ALO*>(pold), 0x77A, true);
        }
        for (LO* po : aploOar)
            if (po && g_psw) RemoveLo(g_psw, po);
        SetMurrayState(p, 0);
    }
    for (auto& [plo, b] : s_mpboat)
        if (b.rower == p->palo && plo != pboat) b.rower = nullptr;
    p->pboat1C90 = pboat;
    if (!pboat) return;
    if (FIsLoDerivedFrom(pboat, S3CID(0x30)))
    {
        ShowAloGlobs(static_cast<ALO*>(pboat), 0x779, false);
        ShowAloGlobs(static_cast<ALO*>(pboat), 0x77A, false);
    }
    for (LO* po : aploOar)
        if (po && g_psw) AddLo(g_psw, po);
    SetMurrayState(p, 23);
}

// orig3 0x1DE690 case 23's entry: held (0x23EBF8 1: the linear constraint), the step's motion kind +0x1B8 2 (none),
// at the boat's seat (+0x1EC, slot 43) under it keeping that place (slot 10, grf 3: natively the boat's object record,
// his place (0, 0, 72) in it as the reference's RAM), at rest (0x23D938 / 0x23D9D0), the idle clip (0x1E4900(0))
void EnterMurrayRow3(MURRAY* p)
{
    ALO* pa = p->palo;
    SetAloCnstrLin(pa, 1);
    StepPhysOf(pa).nMotion = 2;
    if (ROWBOAT3* pb = p->pboat1C90 ? Pboat(p->pboat1C90) : nullptr)
    {
        pb->rower = pa;
        SeatRower(*pb);
        SetAloParent(pa, pb->palo, nullptr, 3);
        SeatRower(*pb);
    }
    SetAloVelocityTo(pa, glm::vec3(0.0f));
    SetAloAngVelocityTo(pa, glm::vec3(0.0f));
    SetMurrayRowClip3(p, 0);
}

// orig3 0x1DE690 case 23's exit: no clip (0x1E4900(-1)), not held, the motion kind 0, out of the boat keeping his local
// place as a world place (0x1C7A00(p, 0, 0): grf 0; in the jolly boat (SCPT 4063 clears his boat) he is at the
// world's (0, 0, 72), falls and dies, as in the reference)
void LeaveMurrayRow3(MURRAY* p)
{
    ALO* pa = p->palo;
    SetMurrayRowClip3(p, -1);
    SetAloCnstrLin(pa, 0);
    StepPhysOf(pa).nMotion = 0;
    for (auto& [plo, b] : s_mpboat)
        if (b.rower == pa) b.rower = nullptr;
    if (static const bool s_fOld = std::getenv("SLY3_ROWLEAVE3") != nullptr; s_fOld) SetAloParent(pa, nullptr, nullptr, 3);
    else
    {
        SetAloParent(pa, nullptr, nullptr, 0);
        // (natively the move does not touch the records' stamps (their matrices are the same), so his entries' world
        // geometry and the contacts with the boat stayed where they were and the solve threw him ~5000 away: the place
        // set again as it is (slot 29: the contacts detached, the entries' matrices copied), as the original's next
        // pass sees him)
        if (XF* pxf = PxfObj(pa)) SetAloPos(pa, glm::vec3(pxf->matWorld[3]));
        // (the original's velocities are the records' own (local), at rest in the seat (0x23D938 / 0x23D9D0 at the
        // entry, held since): natively the move out of the boat would read as a speed)
        SetAloVelocityTo(pa, glm::vec3(0.0f));
        SetAloAngVelocityTo(pa, glm::vec3(0.0f));
    }
    if (FPrint())
    {
        const glm::vec3 q = PosOf(pa);
        std::printf("row3: Murray out of his boat at %.1f %.1f %.1f t %.2f\n", q.x, q.y, q.z, g_clock.t);
    }
}

// orig3 0x1E49B8 (his state 23's update, 0x1DD590): a stroke's clip over: the idle again (0x1E4900(0))
void UpdateMurrayRow3(MURRAY* p)
{
    for (int i = 0; i < 4; ++i)
    {
        const int mode = p->nRow1C64;
        if (mode && (!p->pasega || FAsegaRetracted(p->pasega))) SetMurrayRowClip3(p, 0);
        if (p->nRow1C64 == mode) break;
    }
}

// orig3 0x1DEFE8's events 0x247 (the boat ready: +0x1F0 = 1) / 0x248 (the oars' splash 0x20F508)
void OnMurrayRowEvent3(MURRAY* p, int ev)
{
    ROWBOAT3* pb = p->pboat1C90 ? Pboat(p->pboat1C90) : nullptr;
    if (!pb) return;
    if (ev == 0x247) pb->active = 1;
    else if (ev == 0x248) SplashOars(*pb, p->nRow1C64);
}

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

// (menu.cpp Control Help, orig3 0x1F5838's case 8) the jolly boat's cannon on (+0x284, EOPID 1806)
bool FJollyCannon3(LO* plo)
{
    JOLLY3* pj = Pjolly(plo);
    return pj && pj->fEnabled;
}
