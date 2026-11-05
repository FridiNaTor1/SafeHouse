// slycheck.cpp: the actors' differential harness against the reference (Sly2-Reference).
//
//   Sly2-Release --world 2 --headless --fixed-dt --sly-start RAM --sly-ref TRACE --press-vblank-base B --press ...
//
// RAM: the reference's memory at the start of a frame (SLY2_FRAMEDUMP_RAM), with Sly standing; TRACE: the reference's
// Sly updates from there on (SLY2_FNTRACE='*18BCE0:TRACE:2DDED0+50,2DDF40+A0,<sly>+2100,<records>+300,<padctl>+E0,
// <sost>+E0'). The native game loads the world, then at its first frame takes over from the RAM what the walk
// depends on: the clock, Sly's position / rotation records and velocities, his heading and STEP motion fields, his
// state (entered natively), the current player and the controller state, the camera's position and matrix. Each
// native frame then is compared with the reference's frame of the same vblank (B + frame): Sly's position, state,
// substate, the jump's state, the heading; the first frames and every change are printed, and the differences
// summarised.
#include "actor.h"
#include "damage.h"
#include "target.h"
#include "pocket.h"
#include "guard.h"
#include "brx.h"
#include "jump.h"
#include "joy.h"
#include "activate.h"
#include "anim.h"
#include "asega.h"
#include "alo.h"
#include "clock.h"
#include "cm.h"
#include "cplcy.h"
#include "lo.h"
#include "po.h"
#include "sly.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "worldaccess.h"
#include "xf.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

void ImportCmStateFromRam(const std::vector<uint8_t>& ram);  // cmcheck.cpp

namespace
{
constexpr uint32_t A_SLY = 0xFB1580;  // (Paris)

struct RAM
{
    std::vector<uint8_t> ab;
    bool FLoad(const std::string& path)
    {
        std::FILE* pf = std::fopen(path.c_str(), "rb");
        if (!pf) return false;
        ab.resize(32u << 20);
        ab.resize(std::fread(ab.data(), 1, ab.size(), pf));
        std::fclose(pf);
        return ab.size() == (32u << 20);
    }
    template <class T>
    T Rd(uint32_t a) const
    {
        T t{};
        if (a + sizeof(T) <= ab.size()) std::memcpy(&t, &ab[a], sizeof(T));
        return t;
    }
    uint32_t U32(uint32_t a) const { return Rd<uint32_t>(a); }
    int32_t S32(uint32_t a) const { return Rd<int32_t>(a); }
    float G(uint32_t a) const { return Rd<float>(a); }
    glm::vec4 V4(uint32_t a) const { return glm::vec4(G(a), G(a + 4), G(a + 8), G(a + 12)); }
    glm::mat4 M4(uint32_t a) const
    {
        glm::mat4 m;
        for (int i = 0; i < 4; i++) m[i] = V4(a + 16 * i);
        return m;
    }
};

// One frame of the reference: the exit RAM ranges of Sly's update.
struct REFFRAME
{
    uint32_t vb = 0;
    float t = 0.0f, dt = 0.0f;
    glm::vec3 pos{0.0f};
    int state = 0, substate = 0, jump = 0, mode = -1;
    float radHeading = 0.0f;
    glm::vec3 posCm{0.0f};
    glm::mat3 matCm{1.0f};
    float uJoy = 0.0f, uJoyDecay = 0.0f, x = 0.0f, y = 0.0f;
    glm::vec3 vecVel{0.0f};
    float uTurn = 0.0f;
    glm::vec3 v{0.0f};
    std::vector<uint8_t> abCm;  // (an optional last range: the whole CM, 2DDF40+630)
    int df4 = -1;               // +0xDF4 the damage controller's reaction
    uint32_t a10DC = 0, a10E8 = 0;  // the faced object, the target (the reference's addresses)
    int n10E4 = -1, n10EC = -1;
    uint8_t abReq[0x60]{};      // +0xE20 the last hit's request (a contact message)
};

struct HARNESS
{
    RAM ram;
    std::vector<REFFRAME> aref;
    size_t iref = 0;
    int vbBase = 0;
    glm::vec3 posLast{0.0f};  // the native position at the last compare
    int c = 0, cOk = 0;
    double dposMax = 0.0;
    int cStateDiff = 0;
    bool fOn = false;
    int iHit = -1;  // the reference frame whose hit lands after this frame's Sly update
} s_h;

// FNT3 records (Sly2-Reference runtime/interp.cpp SLY2_FNTRACE): {magic, kind, icall, vb, pc, cb}, 32 GPRs x 16,
// 32 FPRs, 0x400 stack bytes, then the ranges
bool FLoadRefTrace(const std::string& path)
{
    std::FILE* pf = std::fopen(path.c_str(), "rb");
    if (!pf) return false;
    // the ranges of the harness's trace (see the header): clock 0x50, CM 0xA0, Sly 0x2100, records 0x300,
    // controller 0xE0, mass state 0xE0
    const uint32_t ibClock = 0, ibSly = 0x50 + 0xA0, ibRec = ibSly + 0x2100, ibCm = 0x50;
    for (;;)
    {
        uint32_t h[6];
        if (std::fread(h, sizeof h, 1, pf) != 1 || h[0] != 0x33544E46) break;
        std::vector<uint8_t> ab(h[5]);
        if (h[5] && std::fread(ab.data(), h[5], 1, pf) != 1) break;
        uint32_t a0 = 0;
        std::memcpy(&a0, ab.data() + 16 * 4, 4);
        static std::map<uint32_t, bool> s_mpfSly;  // the entries' a0 by call index
        if (h[1] == 0) { s_mpfSly[h[2]] = a0 == A_SLY; continue; }
        if (h[1] != 1 || !s_mpfSly[h[2]]) continue;
        const uint8_t* pr = ab.data() + 512 + 128 + 0x400;
        auto g = [&](uint32_t ib) { float f; std::memcpy(&f, pr + ib, 4); return f; };
        auto n = [&](uint32_t ib) { int32_t v; std::memcpy(&v, pr + ib, 4); return v; };
        REFFRAME r;
        r.vb = h[3];
        r.t = g(ibClock);
        r.dt = g(ibClock + 4);
        r.pos = glm::vec3(g(ibRec + 0x200 + 0x70), g(ibRec + 0x200 + 0x74), g(ibRec + 0x200 + 0x78));
        r.state = n(ibSly + 0x18C8);
        r.substate = n(ibSly + 0x18D0);
        r.jump = n(ibSly + 0x2DC);
        r.mode = n(ibSly + 0x18D8);
        r.radHeading = g(ibSly + 0x1A8);
        r.vecVel = glm::vec3(g(ibSly + 0x190), g(ibSly + 0x194), g(ibSly + 0x198));
        r.uTurn = g(ibSly + 0x180);
        r.df4 = n(ibSly + 0xDF4);
        r.a10DC = static_cast<uint32_t>(n(ibSly + 0x10DC));
        r.a10E8 = static_cast<uint32_t>(n(ibSly + 0x10E8));
        r.n10E4 = n(ibSly + 0x10E4);
        r.n10EC = n(ibSly + 0x10EC);
        std::memcpy(r.abReq, pr + ibSly + 0xE20, 0x60);
        {
            const uint32_t ibSost = ibRec + 0x300 + 0xE0;
            r.v = glm::vec3(g(ibSost + 0x60), g(ibSost + 0x64), g(ibSost + 0x68));
            const uint32_t ibCmAll = 0x26B0;
            const size_t ib0 = 512 + 128 + 0x400;
            if (ab.size() >= ib0 + ibCmAll + 0x630) r.abCm.assign(pr + ibCmAll, pr + ibCmAll + 0x630);
        }
        r.posCm = glm::vec3(g(ibCm + 0x90), g(ibCm + 0x94), g(ibCm + 0x98));
        {
            const uint32_t ibPad = ibRec + 0x300;
            r.x = g(ibPad + 0x10);
            r.y = g(ibPad + 0x14);
            r.uJoy = g(ibPad + 0x1C);
            r.uJoyDecay = g(ibPad + 0x70);
        }
        for (int i = 0; i < 3; i++) r.matCm[i] = glm::vec3(g(ibCm + 0x60 + 16 * i), g(ibCm + 0x64 + 16 * i), g(ibCm + 0x68 + 16 * i));
        s_h.aref.push_back(r);
    }
    std::fclose(pf);
    return !s_h.aref.empty();
}

void ImportXf(XF* pxf, uint32_t a)
{
    if (!pxf) return;
    const RAM& r = s_h.ram;
    pxf->mat = r.M4(a);
    pxf->matWorld = r.M4(a + 0x40);
    pxf->mode = r.Rd<int16_t>(a + 0x98);
    pxf->state = r.Rd<int16_t>(a + 0x9A);
    pxf->vel = r.V4(a + 0xB0);
    for (int i = 0; i < 4; i++) pxf->aseg[i] = r.V4(a + 0xC0 + 16 * i);
}

// A reference object (its address in the start RAM) as the native one: by its oid (+0x18), the nearest to the
// reference's object record (+0x60 -> +0x70) among several
LO* PloFromRef(SW* psw, uint32_t a)
{
    const RAM& r = s_h.ram;
    if (!a || a >= 0x2000000) return nullptr;
    const int oid = r.S32(a + 0x18);
    const uint32_t axf = r.U32(a + 0x60);
    const glm::vec3 posRef = axf && axf < 0x2000000 ? glm::vec3(r.V4(axf + 0x70)) : glm::vec3(0.0f);
    LO* pBest = nullptr;
    float dBest = 1e30f;
    for (const auto& aplo : psw->mpcidaplo)
        for (LO* plo : aplo)
        {
            if (!plo || plo->oid != oid) continue;
            const float d = FIsLoDerivedFrom(plo, 0x30) ? glm::length(PosAloCenter(static_cast<ALO*>(plo)) - posRef) : 1e29f;
            if (d < dBest) { dBest = d; pBest = plo; }
        }
    return pBest;
}
}  // namespace
extern const glm::mat3* g_pmatDamageSender;  // damage.cpp
namespace
{
// The hit of the reference (its +0xDF4 now 7, before 0) given to the native controller before this frame: its request
// (a contact message: the sender by oid, its world matrix from SLY2N_HARNESS_HITRAM (the RAM at 0x1A19A0) else the
// start RAM)
void InjectRefHit(SW* psw, SLY* p, const REFFRAME& r)
{
    CTMSG ctm;
    auto u = [&](int ib) { uint32_t v; std::memcpy(&v, r.abReq + ib, 4); return v; };
    auto g = [&](int ib) { float f; std::memcpy(&f, r.abReq + ib, 4); return f; };
    const uint32_t aSender = u(0);
    ctm.ploSender = PloFromRef(psw, aSender);
    ctm.ploReceiver = p->palo;
    ctm.grfFilter = u(0xC);
    // (the request keeps the change taken: the reference's nState after the scale; the native scale is 1)
    ctm.nState = static_cast<int32_t>(u(0x10));
    ctm.pos = glm::vec3(g(0x20), g(0x24), g(0x28));
    ctm.vec = glm::vec3(g(0x30), g(0x34), g(0x38));
    static RAM s_ramHit;
    static bool s_fHit = std::getenv("SLY2N_HARNESS_HITRAM") && s_ramHit.FLoad(std::getenv("SLY2N_HARNESS_HITRAM"));
    const RAM& rm = s_fHit ? s_ramHit : s_h.ram;
    static glm::mat3 s_mat;
    const uint32_t axf = rm.U32(aSender + 0x60);
    s_mat = glm::mat3(rm.M4(axf + 0x40));
    g_pmatDamageSender = &s_mat;
    const bool f = FAcceptDamage(p, &ctm);
    g_pmatDamageSender = nullptr;
    std::printf("sly harness: the reference's hit (sender 0x%X oid %d native %p, kind 0x%X, %d): %s; velocity %.1f %.1f %.1f\n",
                aSender, s_h.ram.S32(aSender + 0x18), static_cast<void*>(ctm.ploSender), ctm.grfFilter, ctm.nState,
                f ? "taken" : "refused", VelAloPos(p->palo).x, VelAloPos(p->palo).y, VelAloPos(p->palo).z);
}
}  // namespace

bool FSlyHarnessOn() { return s_h.fOn; }
int g_iframeHarness = 0;  // (game.cpp: the frame being updated)

// The take-over at the native first frame (before its input and clock).
bool FImportSlyStart(SW* psw, const std::string& pathRam, const std::string& pathTrace, int vbBase)
{
    if (!s_h.ram.FLoad(pathRam))
    {
        std::fprintf(stderr, "sly-start: cannot read %s\n", pathRam.c_str());
        return false;
    }
    if (!pathTrace.empty() && !FLoadRefTrace(pathTrace)) std::fprintf(stderr, "sly-ref: no Sly records in %s\n", pathTrace.c_str());
    s_h.vbBase = vbBase;
    s_h.fOn = true;
    const RAM& r = s_h.ram;
    LO* plo = psw->mpcidaplo[0x35].empty() ? nullptr : psw->mpcidaplo[0x35][0];
    SLY* p = PslyFromLo(plo);
    if (!p) return false;
    ALO* palo = p->palo;
    // the clock: the frame's update will add its dt
    g_clock.t = r.G(0x2DDED0) - (1.0f / 60.0f) * 0.0f;  // (the RAM is the frame's before its clock update)
    // the records (the position record +0x58, the rotation / object record +0x5C / +0x60)
    // (the first records in the load's order: the root, the position (kind 1), the rotation (kind 6, the object's))
    const uint32_t axf = r.U32(A_SLY + 0x54);
    for (size_t i = 0; i < 3 && i < palo->axf.size(); ++i) ImportXf(palo->axf[i].get(), axf + 0x100 * static_cast<uint32_t>(i));
    auto index = [&](const XF* pxf) {
        for (size_t i = 0; i < palo->axf.size(); ++i)
            if (palo->axf[i].get() == pxf) return static_cast<int>(i);
        return -1;
    };
    std::printf("sly-start: records pos %d rot %d obj %d (of %zu)\n", index(PxfPos(palo)), index(PxfRot(palo)), index(PxfObj(palo)),
                palo->axf.size());
    UpdateAloXfWorlds(palo);
    // the entries' off bits (bit 50: TODO(actors / physics): who turns Sly's body entries off in the reference)
    {
        const uint32_t ae = r.U32(A_SLY + 0xE4);
        const int ce = r.S32(A_SLY + 0xE0);
        for (int i = 0; i < ce && i < static_cast<int>(palo->asoe.size()); ++i)
        {
            const uint32_t e = ae + 0xB0 * static_cast<uint32_t>(i);
            if (r.S32(e + 0xC) != palo->asoe[i].tag) continue;
            if ((r.Rd<uint64_t>(e + 0x10) >> 50) & 1) SetSoeOff(palo, &palo->asoe[i]);
        }
    }
    // the entries' world geometry and the attachment's box at the new place (as the physics' first-frame refresh)
    CopyAloSoeMatrices(palo);
    for (SOE& e : palo->asoe)
    {
        e.nStampWorld = INT32_MIN;
        UpdateSoeWorld(&e);
    }
    UpdateAloBounds(palo);
    MarkAloDirty(palo);
    // the mass state's velocities
    if (SOST* ps = Psost(palo))
    {
        const uint32_t a = r.U32(A_SLY + 0xE8);
        ps->v = glm::vec3(r.V4(a + 0x60));
        ps->w = glm::vec3(r.V4(a + 0x70));
        // the constraints and the gravity (the load's options 0x1F5720 / 0x1F5880 and the scripts set them; TODO(world):
        // those option setters natively)
        SetAloCnstrAng(palo, r.S32(a + 0xA0));
        SetAloCnstrLin(palo, r.S32(a + 0xC0));
        // (the setters take the original's enum (0x2C3C20 / 0x2C3BA0: the kind and the axis), not the kind: the
        // reference's axes and kinds themselves (the physics port))
        ps->cnstrAng.normal = glm::vec3(r.V4(a + 0x90));
        ps->cnstrAng.kind = r.S32(a + 0xA0);
        ps->cnstrLin.normal = glm::vec3(r.V4(a + 0xB0));
        ps->cnstrLin.kind = r.S32(a + 0xC0);
        SetAloGravity(palo, glm::vec3(r.V4(a + 0x80)));
        if (std::fabs(ps->m - r.G(a + 0xD0)) > 1e-6f) std::printf("sly-start: mass native %.6f ref %.6f\n", ps->m, r.G(a + 0xD0));
    }
    // the STEP fields
    SetStepPhysHeading(palo, r.G(A_SLY + 0x1A8));
    p->posFloor = r.V4(A_SLY + 0x230);  // (the floor point: a script's placement before the take-over moved it)
    p->uTurn = r.G(A_SLY + 0x180);
    p->vecVel = glm::vec3(r.V4(A_SLY + 0x190));
    p->nMotion = r.S32(A_SLY + 0x1A4);
    p->posTarget = r.V4(A_SLY + 0x1E0);
    p->uBrake = r.G(A_SLY + 0x200);
    p->tFloor = r.G(A_SLY + 0x20C);
    p->sSize = r.G(A_SLY + 0x260);  // (TODO(actors): who sets it to 1.3 in the reference)
    // the state, entered natively (its animation)
    // the jump sub-object (a script's placement before the take-over may have left it falling: 0x195A00 kind 0xB)
    if (r.S32(A_SLY + 0x2DC) == -1 && p->jump.state != -1) SetJumpAnimState(&p->jump, -1, nullptr);
    const int state = r.S32(A_SLY + 0x18C8);
    p->state = -1;
    SetSlyState(p, state, r.S32(A_SLY + 0x18D0));
    // the jump (its kind, its animation state and fields as the reference has them in the air)
    if (state == 2 && r.S32(A_SLY + 0x2DC) != -1)
    {
        JUMP& j = p->jump;
        j.kind = r.S32(A_SLY + 0x2C4);
        j.n8 = r.S32(A_SLY + 0x2C8);
        SetJumpAnimState(&j, r.S32(A_SLY + 0x2DC), nullptr);
        j.t18 = r.G(A_SLY + 0x2D8);
        j.t20 = r.G(A_SLY + 0x2E0);
        j.f24 = r.S32(A_SLY + 0x2E4); j.f28 = r.S32(A_SLY + 0x2E8); j.f2C = r.S32(A_SLY + 0x2EC); j.f30 = r.S32(A_SLY + 0x2F0);
        j.g34 = r.G(A_SLY + 0x2F4); j.g38 = r.G(A_SLY + 0x2F8); j.z3C = r.G(A_SLY + 0x2FC);
        j.n40 = r.S32(A_SLY + 0x300);
        j.t44 = r.G(A_SLY + 0x304); j.t48 = r.G(A_SLY + 0x308); j.t4C = r.G(A_SLY + 0x30C);
    }
    p->tState = r.G(A_SLY + 0x18CC);
    // the current player and its controller
    EnablePo(psw, palo, 1);
    if (PODATA* ppd = PpodataFromLo(palo); ppd && ppd->ppadctl)
    {
        const uint32_t a = r.U32(A_SLY + 0x150);
        PADCTL& pc = *ppd->ppadctl;
        pc.radJoy = r.G(a + 0x18);
        pc.uJoyDecay = r.G(a + 0x70);
        pc.radJoy2 = r.G(a + 0x28);
        pc.uJoyDecay2 = r.G(a + 0x74);
        pc.grfbtnPrev = r.U32(a + 0x78);
        for (int i = 0; i < 16; i++) pc.atPressed[i] = r.G(a + 0x88 + 4 * i);
    }
    // the camera's view (its policies stay native)
    g_cm.pos = glm::vec3(r.V4(0x2DDF40 + 0x90));
    g_cm.mat = glm::mat3(glm::vec3(r.V4(0x2DDF40 + 0x60)), glm::vec3(r.V4(0x2DDF40 + 0x70)), glm::vec3(r.V4(0x2DDF40 + 0x80)));
    ImportJoyPs2FromRam(&g_joyps2, r.ab);  // the pad (its sticks' last read)
    ImportCmStateFromRam(r.ab);  // (cmcheck.cpp: the follow camera's state too, for SLY2N_HARNESS_NATIVECM)
    if (std::getenv("SLY2N_HARNESS_V"))
        for (size_t i = 0; i < palo->axf.size() && i < 140; i += 7)
        {
            const glm::vec3 n(palo->axf[i]->matWorld[3]), rr(r.V4(axf + 0x100 * static_cast<uint32_t>(i) + 0x70));
            std::printf("  record %3zu kind %d native %.1f %.1f %.1f ref %.1f %.1f %.1f (kind %d)\n", i, palo->axf[i]->kind, n.x, n.y, n.z,
                        rr.x, rr.y, rr.z, r.S32(axf + 0x100 * static_cast<uint32_t>(i) + 0x94));
        }
    if (std::getenv("SLY2N_HARNESS_V"))
    {
        const uint32_t ae = r.U32(A_SLY + 0xE4);
        const int ce = r.S32(A_SLY + 0xE0);
        for (int i = 0; i < std::max<int>(ce, static_cast<int>(palo->asoe.size())); ++i)
        {
            const uint32_t e = ae + 0xB0 * static_cast<uint32_t>(i);
            const uint64_t fl = r.Rd<uint64_t>(e + 0x10);
            if (i < ce)
                std::printf("  ref entry %2d tag %d f48 %d f49 %d f50 %d f51 %d state %d id 0x%X pinned %d\n", i, r.S32(e + 0xC),
                            int((fl >> 48) & 1), int((fl >> 49) & 1), int((fl >> 50) & 1), int((fl >> 51) & 1), r.Rd<int16_t>(e + 0x14),
                            r.U32(e + 0x18), r.S32(e + 0x1C));
            if (i < static_cast<int>(palo->asoe.size()))
            {
                const SOE& n = palo->asoe[i];
                std::printf("  nat entry %2d tag %d f48 %d f49 %d f50 %d f51 %d state %d id 0x%X pinned %d inoxa %d\n", i, n.tag, n.f48,
                            n.f49, n.f50, n.f51, n.state, n.id, n.pinned, n.fInOxa);
            }
        }
    }
    std::printf("sly-start: native STEP: sFloorTarget %.3f g214 %.3f g220 %.3f vBrake %.3f %.3f %.3f gScale %.3f sSize %.3f vecBase z %.3f "
                "probes %d %d custom %d\n", p->sFloorTarget, p->g214, p->g220, p->vBrake.x, p->vBrake.y, p->vBrake.z, p->gScale,
                p->sSize, p->vecBase.z, p->iposProbe0, p->iposProbe1, p->isoeCustom);
    // (the take-over's setters (constraints 0x1F5720, the jump's reset) wake the records: the reference's modes and
    // velocities of the first three records again, the physics port)
    {
        const uint32_t axfRe = r.U32(A_SLY + 0x54);
        for (size_t i = 0; i < 3 && i < palo->axf.size(); ++i) ImportXf(palo->axf[i].get(), axfRe + 0x100 * static_cast<uint32_t>(i));
    }
    // The faced object and target also belong to the starting checkpoint. With
    // a trace PrepareSlyFrame refreshes these each frame; a one-shot checkpoint
    // must retain them too (e.g. a pickpocket animation already in progress).
    p->plo10DC = PloFromRef(psw, r.U32(A_SLY + 0x10DC));
    p->plo10E8 = PloFromRef(psw, r.U32(A_SLY + 0x10E8));
    p->n10E4 = r.S32(A_SLY + 0x10E4);
    p->n10EC = r.S32(A_SLY + 0x10EC);
    if (s_h.aref.empty() && (p->plo10DC || p->plo10E8)) g_fActorTargetsImported = true;
    // the target (a guard: placed where the reference has it, its AI being the AI code) and the attack mode
    if (const uint32_t at = r.U32(A_SLY + 0x10E8); at && at < 0x2000000)
        if (LO* pt = PloFromRef(psw, at); pt && FIsLoDerivedFrom(pt, 0x30) && !pt->fInWorld)  // (in the world: SLY2N_AI_START placed it)
        {
            const uint32_t axfT = r.U32(at + 0x60);
            ALO* pat = static_cast<ALO*>(pt);
            const glm::vec3 posRef(r.V4(axfT + 0x70));
            SetAloPos(pat, glm::vec3(MatWorldOfAlo(pat)[3]) + (posRef - PosAloCenter(pat)));
            // its rotation (the record's world rows) too, unless SLY2N_HARNESS_NOTARGETROT
            if (!std::getenv("SLY2N_HARNESS_NOTARGETROT"))
                SetAloRotation(pat, glm::mat3(glm::vec3(r.V4(axfT + 0x40)), glm::vec3(r.V4(axfT + 0x50)), glm::vec3(r.V4(axfT + 0x60))));
            UpdateAloXfWorlds(pat);
            // the giver's pocket (pocket.h: +0x404 entries, +0x484 / +0x488 / +0x48C / +0x490, +0x3FC; the pocket object
            // +0x494 for a guard)
            POCKET& pk = PocketOf(pt);
            for (int i = 0; i < 8; ++i)
            {
                pk.ae[i].oid = r.S32(at + 0x404 + 16 * i);
                pk.ae[i].n4 = r.S32(at + 0x408 + 16 * i);
                pk.ae[i].n8 = r.S32(at + 0x40C + 16 * i);
                pk.ae[i].c = r.S32(at + 0x410 + 16 * i);
            }
            pk.c = r.S32(at + 0x484);
            pk.i = r.S32(at + 0x488);
            pk.fReady = r.S32(at + 0x48C);
            pk.fMore = r.S32(at + 0x490);
            pk.nMode = r.S32(at + 0x3FC);
            if (GUARD* pg = PguardFromLo(pt))
                if (const uint32_t ap = r.U32(at + 0x494); ap && ap < 0x2000000) pg->plo494 = PloFromRef(psw, ap);
            // the pocket object's reward (+0x150..+0x158, readied at the guard's start by 0x1F1CF8): readied again
            if (PloPocket(pt))
            {
                ClearReward(PloPocket(pt));
                NextPocketPrize(pt);
            }
            std::printf("sly-start: the target's pocket %d / %d (prize %d x %d), pocket object %d\n", pk.i, pk.c, pk.ae[0].oid, pk.ae[0].c,
                        PloPocket(pt) ? PloPocket(pt)->oid : -1);
            std::printf("sly-start: the target %d placed at %.1f %.1f %.1f (now %.1f %.1f %.1f)\n", pt->oid, posRef.x, posRef.y, posRef.z,
                        PosAloCenter(pat).x, PosAloCenter(pat).y, PosAloCenter(pat).z);
        }
    // (the mode when the reference's first frame has it too: some start RAMs are not the trace's frame)
    if (const int mode = r.S32(A_SLY + 0x18D8); mode != -1 && std::any_of(s_h.aref.begin(), s_h.aref.end(), [&](const REFFRAME& f) {
                                    return f.vb == static_cast<uint32_t>(vbBase) && f.mode == mode; }))
    {
        SetSlyMode(p, mode);
        p->tMode = r.G(A_SLY + 0x18DC);
        p->f18E4 = r.S32(A_SLY + 0x18E4);
        p->f18E8 = r.S32(A_SLY + 0x18E8);
    }
    std::printf("sly-start: t %.3f state %d pos %.2f %.2f %.2f heading %.4f\n", r.G(0x2DDED0), state,
                PosAloCenter(palo).x, PosAloCenter(palo).y, PosAloCenter(palo).z, p->radHeading);
    return true;
}

// Before each native frame's world update: the reference's camera of that frame (its controller states turn the
// sticks by the camera's pan; the native camera's own path is the camera's check, not this one's)
void PrepareSlyFrame(int frame)
{
    if (!s_h.fOn || s_h.aref.empty()) return;
    const uint32_t vb = static_cast<uint32_t>(s_h.vbBase + frame);
    size_t i = s_h.iref;
    while (i < s_h.aref.size() && s_h.aref[i].vb < vb) ++i;
    if (i >= s_h.aref.size() || s_h.aref[i].vb != vb) return;
    // the reference's frame step (its EE tick counter's: not 1/60 exactly) and time, unless SLY2N_HARNESS_NATIVEDT
    static const bool s_fNativeDt = std::getenv("SLY2N_HARNESS_NATIVEDT") != nullptr;
    if (!s_fNativeDt && s_h.aref[i].dt > 0.0f)
    {
        g_clock.dt = s_h.aref[i].dt;
        g_clock.t = s_h.aref[i].t;
    }
    // SLY2N_HARNESS_SNAP=F: at frame F, Sly moved by the previous frame's difference (the cause of a divergence
    // told from the earlier frames' drift)
    static const int s_frameSnap = std::getenv("SLY2N_HARNESS_SNAP") ? std::atoi(std::getenv("SLY2N_HARNESS_SNAP")) : -1;
    if (frame == s_frameSnap && i > 0 && s_h.aref[i - 1].vb + 1 == vb && g_psw && !g_psw->mpcidaplo[0x35].empty())
        if (SLY* p = PslyFromLo(g_psw->mpcidaplo[0x35][0]))
        {
            const glm::vec3 d = s_h.aref[i - 1].pos - s_h.posLast;  // (both at the previous frame's compare point)
            SetStepPos(p, PosAloCenter(p->palo) + d);
            std::printf("sly harness: snapped by %.3f %.3f %.3f at frame %d\n", d.x, d.y, d.z, frame);
        }
    // the targets (0x194A40: not native yet) and the guards' hits (the AI's strikes: SLY2N_HARNESS_NOHIT leaves them)
    if (g_psw && !g_psw->mpcidaplo[0x35].empty())
        if (SLY* p = PslyFromLo(g_psw->mpcidaplo[0x35][0]))
        {
            const REFFRAME& r = s_h.aref[i];
            // (the reference's targets instead of the native pick 0x140558, whose guards (spawned, their AI state) the
            // harness does not have; SLY2N_HARNESS_NATIVETARGET: the native pick)
            static const bool s_fRefTarget = std::getenv("SLY2N_HARNESS_NATIVETARGET") == nullptr;
            g_fActorTargetsImported = s_fRefTarget;
            if (s_fRefTarget)
            {
                p->plo10DC = PloFromRef(g_psw, r.a10DC);
                p->plo10E8 = PloFromRef(g_psw, r.a10E8);
                p->n10E4 = r.n10E4;
                p->n10EC = r.n10EC;
            }
            static int s_n10ECLast = -99;
            if (!s_fRefTarget && (p->n10EC != s_n10ECLast || (r.n10EC != p->n10EC && frame % 10 == 0)))
                std::printf("frame %4d vb %u: targets native near %d (%d) best %d (%d) | ref near 0x%X (%d) best 0x%X (%d)\n", frame, vb,
                            p->plo10DC ? p->plo10DC->oid : -1, p->n10E4, p->plo10E8 ? p->plo10E8->oid : -1, p->n10EC, r.a10DC,
                            r.n10E4, r.a10E8, r.n10EC);
            s_n10ECLast = p->n10EC;
            static bool s_fTold = false;
            if (!s_fTold && r.a10E8)
            {
                s_fTold = true;
                std::printf("sly harness: the target 0x%X (oid %d) is native %p\n", r.a10E8, s_h.ram.S32(r.a10E8 + 0x18),
                            static_cast<void*>(p->plo10E8));
            }
            // (the hit lands after Sly's update of the frame before the reference's record shows it: injected there)
            static const bool s_fNoHit = std::getenv("SLY2N_HARNESS_NOHIT") != nullptr;
            s_h.iHit = (!s_fNoHit && i + 1 < s_h.aref.size() && s_h.aref[i + 1].df4 == 7 && r.df4 != 7 && s_h.aref[i + 1].vb == vb + 1)
                           ? static_cast<int>(i + 1) : -1;
            g_pfnSlyUpdated = [](SLY* p) {
                if (s_h.iHit < 0) return;
                InjectRefHit(g_psw, p, s_h.aref[s_h.iHit]);
                s_h.iHit = -1;
            };
        }
    if (std::getenv("SLY2N_HARNESS_NATIVECM")) return;
    g_cm.pos = s_h.aref[i].posCm;
    g_cm.mat = s_h.aref[i].matCm;
}

// After each native frame: compare with the reference's frame of the same vblank.
void CompareSlyFrame(SW* psw, int frame)
{
    if (!s_h.fOn || s_h.aref.empty()) return;
    const uint32_t vb = static_cast<uint32_t>(s_h.vbBase + frame);
    while (s_h.iref < s_h.aref.size() && s_h.aref[s_h.iref].vb < vb) ++s_h.iref;
    if (s_h.iref >= s_h.aref.size() || s_h.aref[s_h.iref].vb != vb) return;
    const REFFRAME& r = s_h.aref[s_h.iref];
    LO* plo = psw->mpcidaplo[0x35].empty() ? nullptr : psw->mpcidaplo[0x35][0];
    SLY* p = PslyFromLo(plo);
    if (!p) return;
    const glm::vec3 pos = PosAloCenter(p->palo);
    const float d = glm::length(pos - r.pos);
    s_h.posLast = pos;
    const bool fState = p->state == r.state && p->jump.state == r.jump && p->mode == r.mode;
    ++s_h.c;
    if (d < 0.5f && fState) ++s_h.cOk;
    if (!fState) ++s_h.cStateDiff;
    s_h.dposMax = std::max<double>(s_h.dposMax, d);
    static int s_stateLast = -99, s_refLast = -99;
    static const bool s_fAll = std::getenv("SLY2N_HARNESS_ALL") != nullptr;
    static int s_modeLast = -99, s_refModeLast = -99;
    const bool fModeChange = p->mode != s_modeLast || r.mode != s_refModeLast;
    s_modeLast = p->mode;
    s_refModeLast = r.mode;
    static int s_f18E4Last = -1;
    if (fModeChange || p->f18E4 != s_f18E4Last)
        std::printf("frame %4d vb %u: mode native %d ref %d (window %d, mode t %.3f)\n", frame, vb, p->mode, r.mode, p->f18E4,
                    p->pasegaMode ? p->pasegaMode->t : -1.0f);
    s_f18E4Last = p->f18E4;
    if (s_fAll || frame < 5 || frame % 30 == 0 || p->state != s_stateLast || r.state != s_refLast || (d >= 0.5f && s_h.c - s_h.cOk < 20))
        std::printf("frame %4d vb %u t %.3f/%.3f: native state %2d/%2d jump %2d pos %9.2f %9.2f %8.2f h %7.4f | ref state %2d/%2d "
                    "jump %2d pos %9.2f %9.2f %8.2f h %7.4f | d %.3f\n",
                    frame, vb, g_clock.t, r.t, p->state, p->substate, p->jump.state, pos.x, pos.y, pos.z, p->radHeading, r.state,
                    r.substate, r.jump, r.pos.x, r.pos.y, r.pos.z, r.radHeading, d);
    if (std::getenv("SLY2N_HARNESS_NATIVECM") && !r.abCm.empty() && (s_fAll || frame % 10 == 0))
    {
        auto gr = [&](uint32_t ib) { float f; std::memcpy(&f, r.abCm.data() + ib, 4); return f; };
        const CPFOLLOW& f = g_cmpolicies.cpfollow;
        std::printf("    follow native pan %.4f tilt %.4f dist %.1f dy %.3f eye %.1f %.1f %.1f | ref pan %.4f tilt %.4f dist %.1f dy %.3f "
                    "eye %.1f %.1f %.1f\n", f.radPan, f.radTilt, f.sDist, f.dyScreen, f.posEye.x, f.posEye.y, f.posEye.z,
                    gr(0x3B0 + 0x18), gr(0x3B0 + 0x1C), gr(0x3B0 + 0x20), gr(0x3B0 + 0x24), gr(0x3B0 + 0x70), gr(0x3B0 + 0x74),
                    gr(0x3B0 + 0x78));
        const CPDEFI& c = g_cm.cpdefiPrev;
        std::printf("    cpdefi native base %.1f %.1f %.1f v %.1f %.1f %.1f home %.4f adj %.2f | ref base %.1f %.1f %.1f v %.1f %.1f %.1f "
                    "home %.4f adj %.2f\n", c.posBase.x, c.posBase.y, c.posBase.z, c.vBase.x, c.vBase.y, c.vBase.z, c.radHome,
                    c.sAdjust, gr(0x280), gr(0x284), gr(0x288), gr(0x290), gr(0x294), gr(0x298), gr(0x2A0), gr(0x2A4));
        auto nr = [&](uint32_t ib) { int32_t v; std::memcpy(&v, r.abCm.data() + ib, 4); return v; };
        std::printf("    cpdefi native grf %u tMove %.3f cfk %d | ref grf %d tMove %.3f cfk %d; follow native manual %d mode %d first %d "
                    "tManual %.3f polar %.4f tMoveLast %.3f | ref manual %d mode %d first %d tManual %.3f polar %.4f tMoveLast %.3f\n",
                    c.grfcpd, c.tMoveLast, c.cfk, nr(0x270), gr(0x2A8), nr(0x2AC), f.fManual, f.cpmode, f.fFirst, f.tManual,
                    f.radPolar, f.tMoveLast, nr(0x3B0 + 0x28), nr(0x3B0 + 0x34), nr(0x3B0 + 0x38), gr(0x3B0 + 0x48), gr(0x3B0 + 0x4C),
                    gr(0x3B0 + 0x60));
    }
    if (std::getenv("SLY2N_HARNESS_NATIVECM") && (s_fAll || frame % 10 == 0))
        std::printf("    cm native %.1f %.1f %.1f dir %.3f %.3f %.3f | ref %.1f %.1f %.1f dir %.3f %.3f %.3f | d %.1f\n", g_cm.pos.x,
                    g_cm.pos.y, g_cm.pos.z, g_cm.mat[0].x, g_cm.mat[0].y, g_cm.mat[0].z, r.posCm.x, r.posCm.y, r.posCm.z,
                    r.matCm[0].x, r.matCm[0].y, r.matCm[0].z, glm::length(g_cm.pos - r.posCm));
    if (std::getenv("SLY2N_HARNESS_PAD"))
        if (PODATA* ppd = PpodataFromLo(p->palo); ppd && ppd->ppadctl)
            std::printf("    pad native x %.3f y %.9g u %.9g held %.9g | ref x %.3f y %.9g u %.9g held %.9g\n", ppd->ppadctl->x,
                        ppd->ppadctl->y, ppd->ppadctl->uJoy, ppd->ppadctl->uJoyDecay, r.x, r.y, r.uJoy, r.uJoyDecay);
    if (std::getenv("SLY2N_HARNESS_V"))
    {
        const SOST* ps = Psost(p->palo);
        std::printf("    ref vecVel %.3f %.3f %.3f uTurn %.4f v %.3f %.3f %.3f\n", r.vecVel.x, r.vecVel.y, r.vecVel.z, r.uTurn, r.v.x, r.v.y, r.v.z);
        std::printf("    vecVel %.3f %.3f %.3f floor %p v %.1f %.1f %.1f motion %d uTurn %.3f grfupd 0x%X iupd %d removed %d "
                    "inworld %d active %d\n", p->vecVel.x, p->vecVel.y,
                    p->vecVel.z, static_cast<void*>(p->pxpFloor), ps ? ps->v.x : 0.0f, ps ? ps->v.y : 0.0f, ps ? ps->v.z : 0.0f,
                    p->nMotion, p->uTurn, GrfUpdOfLo(p->palo), p->palo->iupd, p->palo->fRemoved, p->palo->fInWorld,
                    PloRoot(p->palo)->fNodeActive);
        if (OXA* po = Poxa(p->palo); po && frame < 4)
        {
            std::printf("    box %.1f %.1f %.1f .. %.1f %.1f %.1f entries %zu\n", po->posMin.x, po->posMin.y, po->posMin.z,
                        po->posMax.x, po->posMax.y, po->posMax.z, po->apsoe.size());
            for (OX* ox = po->pox; ox; ox = ox->poxNext)
                for (XP* x = ox->pxp; x; x = x->pxpNext)
                    std::printf("    xp kind %d other %d n %.3f %.3f %.3f d %.2f pos0 %.1f %.1f %.1f pos1 %.1f %.1f %.1f\n", x->xpk,
                                ox->psoOther ? ox->psoOther->oid : -1, x->normal.x, x->normal.y, x->normal.z, x->gDist,
                                x->axpd[0].pos.x, x->axpd[0].pos.y, x->axpd[0].pos.z, x->axpd[1].pos.x, x->axpd[1].pos.y,
                                x->axpd[1].pos.z);
        }
    }
    s_stateLast = p->state;
    s_refLast = r.state;
}

void ReportSlyHarness()
{
    if (!s_h.fOn) return;
    std::printf("sly harness: %d frames compared, %d within 0.5 with the same state, %d with another state; max |dpos| %.3f\n",
                s_h.c, s_h.cOk, s_h.cStateDiff, s_h.dposMax);
}
