// gangcheck.cpp: Bentley's / Murray's differential harness against the reference (Sly2-Reference), after the actors'
// slycheck.cpp (the same take-over, for the current player of the RAM: Bentley 0x37 or Murray 0x36).
//
//   Sly2-Release --world 2:1890 --headless --frames N --gang-start RAM --gang-ref TRACE --press-vblank-base B --press ...
//
// RAM: the reference's memory at a frame's start (SLY2_FRAMEDUMP_RAM) with the player standing; TRACE: its updates
// (Bentley SLY2_FNTRACE='*134DA8:TRACE:2DDED0+50,2DDF40+A0,<actor>+1880,<records>+300,<padctl>+E0,<sost>+E0', Murray
// '*1B50B0:...,<actor>+1B00,...'). At the first native frame the player's records, mass state, STEP fields, state,
// controller and the camera are taken from the RAM; each frame takes the reference's clock step and camera; after
// the class's update the position, state, jump state and heading are compared with the reference's frame of the
// same vblank (B + frame).
#include "actor.h"
#include "actorai.h"
#include "jump.h"
#include "joy.h"
#include "activate.h"
#include "alo.h"
#include "asega.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "gang.h"
#include "lo.h"
#include "po.h"
#include "nav.h"
#include "refram.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "xf.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t A_SW = 0x3DD490;

struct GANGRAM
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
        a &= 0x1FFFFFF;
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

struct REFFRAME
{
    uint32_t vb = 0;
    float t = 0.0f, dt = 0.0f;
    glm::vec3 pos{0.0f};
    int state = 0, jump = 0;
    int sub = -1;                  // Bentley's +0x1714 / Murray's +0x1810
    float radHeading = 0.0f;
    glm::vec3 posCm{0.0f};
    glm::mat3 matCm{1.0f};
    glm::vec3 vecVel{0.0f};
    int anCarry[6]{};
    glm::vec3 v{0.0f};             // the mass state's velocity
    float z2 = 0.0f, mode2 = 0.0f; // the object record's local z and mode (anim)
    glm::vec3 vel2{0.0f};
    glm::vec3 aposW[3]; int amode[3]{};
};

struct HARNESS
{
    GANGRAM ram;
    uint32_t aActor = 0;
    uint32_t cb = 0;
    OID oid = -1;
    std::vector<REFFRAME> aref;
    size_t iref = 0;
    int vbBase = 0;
    int c = 0, cOk = 0, cStateDiff = 0;
    double dposMax = 0.0;
    bool fOn = false;
} s_h;

int StateOf(ACTOR* pa)
{
    if (BENTLEY* p = dynamic_cast<BENTLEY*>(pa)) return p->state;
    if (MURRAY* p = dynamic_cast<MURRAY*>(pa)) return p->state;
    return -99;
}

bool FLoadRefTrace(const std::string& path)
{
    std::FILE* pf = std::fopen(path.c_str(), "rb");
    if (!pf) return false;
    const uint32_t ibClock = 0, ibCm = 0x50, ibA = 0x50 + 0xA0, ibRec = ibA + s_h.cb;
    std::vector<bool> afMine;
    for (;;)
    {
        uint32_t h[6];
        if (std::fread(h, sizeof h, 1, pf) != 1 || h[0] != 0x33544E46) break;
        std::vector<uint8_t> ab(h[5]);
        if (h[5] && std::fread(ab.data(), h[5], 1, pf) != 1) break;
        uint32_t a0 = 0;
        std::memcpy(&a0, ab.data() + 16 * 4, 4);
        if (h[4] != 0x134DA8 && h[4] != 0x1B50B0) continue;  // (other traced functions)
        if (afMine.size() <= h[2]) afMine.resize(h[2] + 1);
        if (h[1] == 0) { afMine[h[2]] = a0 == s_h.aActor; continue; }
        if (h[1] != 1 || !afMine[h[2]]) continue;
        const uint8_t* pr = ab.data() + 512 + 128 + 0x400;
        auto g = [&](uint32_t ib) { float f; std::memcpy(&f, pr + ib, 4); return f; };
        auto n = [&](uint32_t ib) { int32_t v; std::memcpy(&v, pr + ib, 4); return v; };
        REFFRAME r;
        r.vb = h[3];
        r.t = g(ibClock);
        r.dt = g(ibClock + 4);
        r.pos = glm::vec3(g(ibRec + 0x270), g(ibRec + 0x274), g(ibRec + 0x278));
        r.z2 = g(ibRec + 0x238);
        for (int k = 0; k < 3; ++k)
        {
            r.aposW[k] = glm::vec3(g(ibRec + 0x100 * k + 0x70), g(ibRec + 0x100 * k + 0x74), g(ibRec + 0x100 * k + 0x78));
            int16_t m;
            std::memcpy(&m, pr + ibRec + 0x100 * k + 0x98, 2);
            r.amode[k] = m;
        }
        r.vel2 = glm::vec3(g(ibRec + 0x2B0), g(ibRec + 0x2B4), g(ibRec + 0x2B8));

        {
            int16_t m;
            std::memcpy(&m, pr + ibRec + 0x298, 2);
            r.mode2 = m;
        }
        r.state = n(ibA + 0x13D0);
        for (int k = 0; k < 6; ++k) r.anCarry[k] = n(ibA + std::array<int, 6>{0x13EC, 0x13F0, 0x1448, 0x144C, 0x1450, 0x10DC}[k]);
        r.jump = n(ibA + 0x2DC);
        r.sub = n(ibA + (s_h.oid == 8 ? 0x1714 : 0x1810));
        r.radHeading = g(ibA + 0x1A8);
        r.vecVel = glm::vec3(g(ibA + 0x190), g(ibA + 0x194), g(ibA + 0x198));
        r.v = glm::vec3(g(ibRec + 0x3E0 + 0x60), g(ibRec + 0x3E0 + 0x64), g(ibRec + 0x3E0 + 0x68));
        r.posCm = glm::vec3(g(ibCm + 0x90), g(ibCm + 0x94), g(ibCm + 0x98));
        for (int i = 0; i < 3; i++) r.matCm[i] = glm::vec3(g(ibCm + 0x60 + 16 * i), g(ibCm + 0x64 + 16 * i), g(ibCm + 0x68 + 16 * i));
        s_h.aref.push_back(r);
    }
    std::fclose(pf);
    return !s_h.aref.empty();
}

void ImportXf(XF* pxf, uint32_t a)
{
    if (!pxf) return;
    const GANGRAM& r = s_h.ram;
    pxf->mat = r.M4(a);
    pxf->matWorld = r.M4(a + 0x40);
    pxf->mode = r.Rd<int16_t>(a + 0x98);
    pxf->state = r.Rd<int16_t>(a + 0x9A);
    pxf->vel = r.V4(a + 0xB0);
    for (int i = 0; i < 4; i++) pxf->aseg[i] = r.V4(a + 0xC0 + 16 * i);
}

ACTOR* PactorHarness(SW* psw)
{
    if (!psw || s_h.oid < 0) return nullptr;
    for (CID cid : {0x36, 0x37})
        for (LO* plo : psw->mpcidaplo[cid])
            if (plo->oid == s_h.oid) return PactorFromLo(plo);
    return nullptr;
}

// The harness starts after the scene's setup has already run in the reference. Keep a captured ordinary
// C2/A8 companion at that same point in its path, rather than retaining the native scene-start path.
void ImportCompanion(SW* psw, ACTOR* pa, uint32_t a)
{
    const GANGRAM& r = s_h.ram;
    if (r.S32(a + 0x270) == 0) { ClearJobs(pa); return; }
    const uint32_t ac = r.U32(a + 0x274), af = r.U32(a + 0x278);
    if (r.S32(a + 0x270) != 2 || r.U32(ac + 0xC) != g_acls[0xC2].vtOrig ||
        r.U32(af + 0xC) != g_acls[0xA8].vtOrig || r.S32(ac + 0x60) != 0 ||
        r.U32(af + 0x30) != r.U32(a + 0x294) || r.U32(af + 0x54) != 0)
        return;  // Other captured actor-job kinds require their own importer.
    LO* follow = nullptr;
    const OID oid = r.S32(r.U32(ac + 0x30) + 0x18);
    for (CID cid : {0x35, 0x36, 0x37})
        for (LO* q : psw->mpcidaplo[cid]) if (q->oid == oid) follow = q;
    if (!follow) return;
    ClearJobs(pa);
    auto* pc = static_cast<JOBCOMPANION*>(PjobPush(pa, 0xC2));
    pc->ploFollow = follow;
    pc->offset = glm::vec3(r.V4(ac + 0x40));
    pc->posFollow = glm::vec3(r.V4(ac + 0x50));
    pc->st = r.S32(ac + 0x60); pc->tSt = r.G(ac + 0x64); pc->dtPredict = r.G(ac + 0x68);
    pc->sNear = r.G(ac + 0x6C); pc->sFar = r.G(ac + 0x70);
    pc->svNear = r.G(ac + 0x74); pc->svFar = r.G(ac + 0x78);
    pc->posRecover = glm::vec3(r.V4(ac + 0x80));
    const uint32_t ap = r.U32(a + 0x294);
    CRV& c = pa->crv294;
    c = CRV{};
    c.crvk = r.S32(ap + 0xE0); c.fClosed = r.S32(ap + 0xE4);
    for (int k = 0; k < r.S32(ap + 0xE8) && k < 1024; ++k)
    {
        c.ag.push_back(r.G(r.U32(ap + 0xEC) + 4 * k));
        c.apos.push_back(glm::vec3(r.V4(r.U32(ap + 0xF4) + 16 * k)));
        if (c.crvk)
        {
            c.avIn.push_back(glm::vec3(r.V4(r.U32(ap + 0x140) + 16 * k)));
            c.avOut.push_back(glm::vec3(r.V4(r.U32(ap + 0x144) + 16 * k)));
        }
    }
    glm::mat4 mat = r.M4(ap + 0x60);
    if (const uint32_t axf = r.U32(ap + 0x50)) mat = r.M4(axf + 0x40) * mat;
    if (!c.apos.empty()) RecalcCrv(&c, mat);
    c.iseg = r.S32(ap + 0x178);
    auto* pf = static_cast<JOBACTORFOLLOW*>(PjobPush(pa, 0xA8));
    pf->pcrv = &c;
    pf->s = r.G(af + 0x34); pf->fReverse = r.S32(af + 0x38); pf->fOpen = r.S32(af + 0x3C);
    pf->fNoAvoid = r.S32(af + 0x40); pf->sEnd = r.G(af + 0x44); pf->sStart = r.G(af + 0x48);
    pf->st = r.S32(af + 0x4C); pf->tSt = r.G(af + 0x50);
    pf->nWay = r.S32(af + 0x58); pf->sOther = r.G(af + 0x5C); pf->g60 = r.G(af + 0x60);
    pf->sArrive = r.G(af + 0x64); pf->sNear = r.G(af + 0x68); pf->sFar = r.G(af + 0x6C);
    pf->sTarget = r.G(af + 0x70); pf->pos = r.V4(af + 0x80); pf->vel = r.V4(af + 0x90);
    pf->fFirst = r.S32(af + 0xA0); pf->posEnd = r.V4(af + 0xB0);
    if (PODATA* pd = PpodataFromLo(follow)) pf->pcCopy = pd->ppadctl;
    pf->grfCopy = r.U32(af + 0xC4); pf->dtCopy = r.G(af + 0xC8);
    pf->fWalk = r.S32(af + 0xCC); pf->uSpeedActor = r.G(af + 0xD0);
    pa->watch.pos = glm::vec3(r.V4(a + 0xFE0));
    pa->watch.s = r.G(a + 0xFF0); pa->watch.t = r.G(a + 0xFF4); pa->watch.fActive = r.S32(a + 0xFF8);
    std::printf("gang-start: companion %d imports C2/A8 state %d/%d, path %zu points\n",
                pa->palo->oid, pc->st, pf->st, c.apos.size());
}

// Captured area membership is an input to the common follower's avoidance (0x177F78), like the
// already-imported collision environment. Keep live native owners so their evolving poses are queried.
void ImportRegionMembers(SW* psw)
{
    ::RAM r;
    r.ab = s_h.ram.ab;
    MAPPING map;
    MapObjects(psw, r, map);
    int cmember = 0;
    for (const auto& [a, plo] : map.mpaplo)
    {
        if (plo->Cid() != 0x11) continue;
        NAVREGION* preg = PregionOfMesh(static_cast<LOXMESH*>(plo));
        preg->amember.clear();
        for (uint32_t am = r.U32(a + 0x118), n = 0; r.FValid(am) && n < 256; am = r.U32(am + 0x60), ++n)
            if (auto it = map.mpaplo.find(r.U32(am)); it != map.mpaplo.end())
            {
                preg->amember.push_back({it->second, r.F32(am + 4), r.F32(am + 8), r.F32(am + 0xC)});
                ++cmember;
            }
    }
    std::printf("gang-start: imports %d area members for follower avoidance\n", cmember);
}
}  // namespace

void DumpGangWe(SW* psw)
{
    if (const char* sz = std::getenv("SLY2N_GANGPROXY"))
        for (const auto& [pproxy, ps] : psw->mpproxy)
            if (pproxy->oid == std::atoi(sz) && ps.iwe >= 0)
            {
                const WE& we = psw->awe[ps.iwe];
                std::printf("gang-proxy %d streams group %d %s (cinst %d):", pproxy->oid, ps.iwe, we.StrName().c_str(), we.cinst);
                for (int j = 0; j < we.cloLoaded; ++j) std::printf(" %d(cid 0x%X)", we.aplo[j]->oid, we.aplo[j]->Cid());
                std::printf("\n");
            }
    if (const char* sz = std::getenv("SLY2N_GANGWE"))
    {
        const int oid = std::atoi(sz);
        for (size_t iwe = 0; iwe < psw->awe.size(); ++iwe)
        {
            const WE& we = psw->awe[iwe];
            bool f = false;
            for (int j = 0; j < we.cloLoaded; ++j) f = f || we.aplo[j]->oid == oid;
            if (!f) continue;
            std::printf("gang-we %zu %s: loaded %d cinst %d used %d x5C %d x60 %d\n", iwe, we.StrName().c_str(), we.cloLoaded, we.cinst,
                        we.cUsed, we.x5C, we.x60);
            for (const auto& [pproxy, ps] : psw->mpproxy)
                if (ps.iwe == static_cast<int>(iwe))
                {
                    const LOX* px = static_cast<const LOX*>(pproxy);
                    const glm::vec3 c = glm::vec3((px->pxf ? px->pxf->matWorld * px->mat : px->mat)[3]);
                    std::printf("   proxy %d at %.1f %.1f %.1f rows %zu removed %d inworld %d active %d iupd %d radius %.1f\n", pproxy->oid, c.x, c.y, c.z,
                                ps.airow.size(), pproxy->fRemoved, pproxy->fInWorld, PloRoot(pproxy)->fNodeActive, pproxy->iupd, pproxy->sRadius);
                }
        }
    }
}

bool FGangHarnessOn() { return s_h.fOn; }

bool FImportGangStart(SW* psw, const std::string& pathRam, const std::string& pathTrace, int vbBase)
{
    if (!s_h.ram.FLoad(pathRam))
    {
        std::fprintf(stderr, "gang-start: cannot read %s\n", pathRam.c_str());
        return false;
    }
    const GANGRAM& r = s_h.ram;
    s_h.aActor = r.U32(A_SW + 0x3EFC);  // (the current player)
    s_h.oid = r.S32(s_h.aActor + 0x18);
    s_h.cb = s_h.oid == 8 ? 0x1880 : 0x1B00;
    s_h.vbBase = vbBase;
    s_h.fOn = true;
    if (!pathTrace.empty() && !FLoadRefTrace(pathTrace)) std::fprintf(stderr, "gang-ref: no records in %s\n", pathTrace.c_str());
    ACTOR* p = PactorHarness(psw);
    if (!p)
    {
        std::fprintf(stderr, "gang-start: no native player %d\n", s_h.oid);
        return false;
    }
    const uint32_t A = s_h.aActor;
    ALO* palo = p->palo;
    g_clock.t = r.G(0x2DDED0);
    const uint32_t axf = r.U32(A + 0x54);
    for (size_t i = 0; i < 3 && i < palo->axf.size(); ++i) ImportXf(palo->axf[i].get(), axf + 0x100 * static_cast<uint32_t>(i));
    {
        auto index = [&](const XF* pxf) {
            for (size_t i = 0; i < palo->axf.size(); ++i)
                if (palo->axf[i].get() == pxf) return static_cast<int>(i);
            return -1;
        };
        std::printf("gang-start: records pos %d rot %d obj %d (of %zu)\n", index(PxfPos(palo)), index(PxfRot(palo)), index(PxfObj(palo)),
                    palo->axf.size());
        if (std::getenv("SLY2N_HARNESS_V"))
            for (size_t i = 0; i < palo->axf.size() && i < 6; ++i)
            {
                const glm::vec3 n(palo->axf[i]->matWorld[3]), rr(r.V4(axf + 0x100 * static_cast<uint32_t>(i) + 0x70));
                std::printf("  record %zu local %.2f %.2f %.2f\n", i, palo->axf[i]->mat[3].x, palo->axf[i]->mat[3].y, palo->axf[i]->mat[3].z),
                std::printf("  record %zu kind %d native %.2f %.2f %.2f ref %.2f %.2f %.2f (kind %d)\n", i, palo->axf[i]->kind, n.x, n.y,
                            n.z, rr.x, rr.y, rr.z, r.S32(axf + 0x100 * static_cast<uint32_t>(i) + 0x94));
            }
    }
    UpdateAloXfWorlds(palo);
    {
        const uint32_t ae = r.U32(A + 0xE4);
        const int ce = r.S32(A + 0xE0);
        for (int i = 0; i < ce && i < static_cast<int>(palo->asoe.size()); ++i)
        {
            const uint32_t e = ae + 0xB0 * static_cast<uint32_t>(i);
            if (r.S32(e + 0xC) != palo->asoe[i].tag) continue;
            if ((r.Rd<uint64_t>(e + 0x10) >> 50) & 1) SetSoeOff(palo, &palo->asoe[i]);
        }
    }
    CopyAloSoeMatrices(palo);
    for (SOE& e : palo->asoe)
    {
        e.nStampWorld = INT32_MIN;
        UpdateSoeWorld(&e);
    }
    UpdateAloBounds(palo);
    MarkAloDirty(palo);
    if (SOST* ps = Psost(palo))
    {
        const uint32_t a = r.U32(A + 0xE8);
        ps->v = glm::vec3(r.V4(a + 0x60));
        ps->w = glm::vec3(r.V4(a + 0x70));
        SetAloCnstrAng(palo, r.S32(a + 0xA0));
        SetAloCnstrLin(palo, r.S32(a + 0xC0));
        // (the setters take the original's enum (0x2C3C20 / 0x2C3BA0: the kind and the axis), not the kind: the
        // reference's axes and kinds themselves (the physics port))
        ps->cnstrAng.normal = glm::vec3(r.V4(a + 0x90));
        ps->cnstrAng.kind = r.S32(a + 0xA0);
        ps->cnstrLin.normal = glm::vec3(r.V4(a + 0xB0));
        ps->cnstrLin.kind = r.S32(a + 0xC0);
        SetAloGravity(palo, glm::vec3(r.V4(a + 0x80)));
        if (std::fabs(ps->m - r.G(a + 0xD0)) > 1e-6f) std::printf("gang-start: mass native %.6f ref %.6f\n", ps->m, r.G(a + 0xD0));
    }
    SetStepPhysHeading(palo, r.G(A + 0x1A8));
    p->posFloor = r.V4(A + 0x230);  // (the floor point: a script's placement before the take-over moved it)
    p->uTurn = r.G(A + 0x180);
    p->vecVel = glm::vec3(r.V4(A + 0x190));
    p->nMotion = r.S32(A + 0x1A4);
    p->posTarget = r.V4(A + 0x1E0);
    p->uBrake = r.G(A + 0x200);
    p->tFloor = r.G(A + 0x20C);
    p->sSize = r.G(A + 0x260);
    // the jump sub-object (a script's placement before the take-over may have left it falling: 0x195A00 kind 0xB)
    if (r.S32(A + 0x2DC) == -1 && p->jump.state != -1) SetJumpAnimState(&p->jump, -1, nullptr);
    const int state = r.S32(A + 0x13D0);
    if (BENTLEY* pb = dynamic_cast<BENTLEY*>(p))
    {
        pb->state = -1;
        SetBentleyState(pb, state);
        pb->tState = r.G(A + 0x13D4);
    }
    else if (MURRAY* pm = dynamic_cast<MURRAY*>(p))
    {
        pm->state = -1;
        SetMurrayState(pm, state);
        pm->tState = r.G(A + 0x13D4);
    }
    EnablePo(psw, palo, 1);
    if (PODATA* ppd = PpodataFromLo(palo); ppd && ppd->ppadctl)
    {
        const uint32_t a = r.U32(A + 0x150);
        PADCTL& pc = *ppd->ppadctl;
        pc.radJoy = r.G(a + 0x18);
        pc.uJoyDecay = r.G(a + 0x70);
        pc.radJoy2 = r.G(a + 0x28);
        pc.uJoyDecay2 = r.G(a + 0x74);
        pc.grfbtnPrev = r.U32(a + 0x78);
        for (int i = 0; i < 16; i++) pc.atPressed[i] = r.G(a + 0x88 + 4 * i);
    }
    ImportJoyPs2FromRam(&g_joyps2, r.ab);  // the pad (its sticks' last read)
    g_cm.pos = glm::vec3(r.V4(0x2DDF40 + 0x90));
    g_cm.mat = glm::mat3(glm::vec3(r.V4(0x2DDF40 + 0x60)), glm::vec3(r.V4(0x2DDF40 + 0x70)), glm::vec3(r.V4(0x2DDF40 + 0x80)));
    if (const char* sz = std::getenv("SLY2N_GANGOBJ"))
        for (const char* q = sz; *q;)
        {
            const int oid = std::atoi(q);
            for (CID cid = 0; cid < 0xC7; ++cid)
                for (LO* plo : psw->mpcidaplo[cid])
                    if (plo->oid == oid && FCidDerivedFrom(plo->Cid(), 0x30))
                    {
                        ALO* pa = static_cast<ALO*>(plo);
                        OXA* po = Poxa(pa);
                        const glm::vec3 c = PosAloCenter(pa);
                        std::printf("gang-obj %d cid 0x%X inworld %d removed %d iupd %d oxa %p entries %zu pos %.1f %.1f %.1f dist %.1f\n",
                                    oid, plo->Cid(), plo->fInWorld, plo->fRemoved, plo->iupd, static_cast<void*>(po), pa->asoe.size(),
                                    c.x, c.y, c.z, glm::length(c - PosAloCenter(palo)));
                    }
            while (*q && *q != ',') ++q;
            if (*q) ++q;
        }
    DumpGangWe(psw);
    std::printf("gang-start: native STEP: sFloorTarget %.3f g214 %.3f g220 %.3f vBrake %.3f %.3f %.3f gScale %.3f sSize %.3f vecBase %.3f %.3f %.3f "
                "probes %d %d custom %d other %d oidMesh %d source %d\n", p->sFloorTarget, p->g214, p->g220, p->vBrake.x, p->vBrake.y,
                p->vBrake.z, p->gScale, p->sSize, p->vecBase.x, p->vecBase.y, p->vecBase.z, p->iposProbe0, p->iposProbe1, p->isoeCustom,
                p->isoeOther, p->oidMesh, p->isoeSource);
    std::printf("gang-start: ref    STEP: sFloorTarget %.3f g214 %.3f g220 %.3f vBrake %.3f %.3f %.3f gScale %.3f sSize %.3f vecBase %.3f %.3f %.3f "
                "probes %d %d custom 0x%X other 0x%X oidMesh %d source 0x%X\n", r.G(A + 0x210), r.G(A + 0x214), r.G(A + 0x220), r.G(A + 0x1F4),
                r.G(A + 0x1F8), r.G(A + 0x1FC), r.G(A + 0x1F0), r.G(A + 0x260), r.G(A + 0x240), r.G(A + 0x244), r.G(A + 0x248),
                r.S32(A + 0x218), r.S32(A + 0x21C), r.U32(A + 0x258), r.U32(A + 0x25C), r.S32(A + 0x264), r.U32(A + 0x268));
    // the other players as the reference has them (in the world or not; placed at their floor point and heading):
    // a script before the take-over (the job's start, 0x16) put them elsewhere
    for (CID cid : {0x35, 0x36, 0x37})
        for (LO* q : psw->mpcidaplo[cid])
        {
            if (q == palo) continue;
            ACTOR* pq = PactorFromLo(q);
            if (!pq) continue;
            uint32_t aq = 0;
            for (uint32_t x = 0x100000; x + 0x300 < (32u << 20) && !aq; x += 0x10)
                if (r.S32(x + 0x18) == q->oid && (r.U32(x + 0xC) == 0x2D2E98 || r.U32(x + 0xC) == 0x2CB230 || r.U32(x + 0xC) == 0x2D44C8))
                    aq = x;
            if (!aq) continue;
            const bool fIn = (r.U32(aq + 0x2C) >> 28) & 1;  // (+0x28 bit 60)
            if (!fIn)
            {
                if (q->fInWorld) RemoveLo(psw, q);
                continue;
            }
            if (!q->fInWorld) AddLo(psw, q);
            SetStepPos(pq, glm::vec3(r.V4(aq + 0x230)) - glm::vec3(pq->vecBase));
            SetStepPhysHeading(static_cast<ALO*>(q), r.G(aq + 0x1A8));
            SetAloVelocityTo(static_cast<ALO*>(q), glm::vec3(0.0f));
            ImportCompanion(psw, pq, aq);
            std::printf("gang-start: player %d placed at its reference floor point %.2f %.2f %.2f\n", q->oid, r.G(aq + 0x230),
                        r.G(aq + 0x234), r.G(aq + 0x238));
        }
    ImportRegionMembers(psw);
    // (the take-over's setters (constraints 0x1F5720, the jump's reset) wake the records: the reference's modes and
    // velocities of the first three records again, the physics port)
    {
        const uint32_t axfRe = r.U32(A + 0x54);
        for (size_t i = 0; i < 3 && i < palo->axf.size(); ++i) ImportXf(palo->axf[i].get(), axfRe + 0x100 * static_cast<uint32_t>(i));
    }
    std::printf("gang-start: player %d (actor 0x%X), t %.3f state %d pos %.2f %.2f %.2f heading %.4f, %zu reference frames\n",
                s_h.oid, A, r.G(0x2DDED0), state, PosAloCenter(palo).x, PosAloCenter(palo).y, PosAloCenter(palo).z,
                p->radHeading, s_h.aref.size());
    return true;
}

void PrepareGangFrame(int frame)
{
    if (!s_h.fOn || s_h.aref.empty()) return;
    const uint32_t vb = static_cast<uint32_t>(s_h.vbBase + frame);
    size_t i = s_h.iref;
    while (i < s_h.aref.size() && s_h.aref[i].vb < vb) ++i;
    if (i >= s_h.aref.size() || s_h.aref[i].vb != vb) return;
    if (!std::getenv("SLY2N_HARNESS_NATIVEDT") && s_h.aref[i].dt > 0.0f)
    {
        g_clock.dt = s_h.aref[i].dt;
        g_clock.t = s_h.aref[i].t;
    }
    if (std::getenv("SLY2N_HARNESS_NATIVECM")) return;
    g_cm.pos = s_h.aref[i].posCm;
    g_cm.mat = s_h.aref[i].matCm;
    g_actview.posCm = g_cm.pos;  // (the activation's viewpoint: the reference's camera)
}

// After the class's update (gang.cpp)
void CompareGangFrame(ACTOR* p, int frame)
{
    if (!s_h.fOn || s_h.aref.empty() || !p || p->palo->oid != s_h.oid) return;
    const uint32_t vb = static_cast<uint32_t>(s_h.vbBase + frame);
    while (s_h.iref < s_h.aref.size() && s_h.aref[s_h.iref].vb < vb) ++s_h.iref;
    if (s_h.iref >= s_h.aref.size() || s_h.aref[s_h.iref].vb != vb) return;
    const REFFRAME& r = s_h.aref[s_h.iref];
    const glm::vec3 pos = PosAloCenter(p->palo);
    const float d = glm::length(pos - r.pos);
    const int state = StateOf(p);
    int sub = -1;
    if (BENTLEY* pb = dynamic_cast<BENTLEY*>(p)) sub = pb->sub.state;
    if (MURRAY* pm = dynamic_cast<MURRAY*>(p)) sub = pm->n1810;
    const bool fState = state == r.state && p->jump.state == r.jump && sub == r.sub;
    ++s_h.c;
    if (d < 0.5f && fState) ++s_h.cOk;
    if (!fState) ++s_h.cStateDiff;
    s_h.dposMax = std::max<double>(s_h.dposMax, d);
    static int s_stateLast = -99, s_refLast = -99;
    static const bool s_fAll = std::getenv("SLY2N_HARNESS_ALL") != nullptr;
    static int s_subLast = -99, s_subRefLast = -99;
    const bool fSubChange = sub != s_subLast || r.sub != s_subRefLast;
    s_subLast = sub;
    s_subRefLast = r.sub;
    if (s_fAll || frame < 3 || frame % 30 == 0 || state != s_stateLast || r.state != s_refLast || fSubChange || (d >= 0.5f && s_h.c - s_h.cOk < 10))
        std::printf("frame %4d vb %u: sub %2d/%2d native state %2d jump %2d pos %9.2f %9.2f %8.2f h %7.4f | ref state %2d jump %2d pos %9.2f %9.2f "
                    "%8.2f h %7.4f | d %.3f\n",
                    frame, vb, sub, r.sub, state, p->jump.state, pos.x, pos.y, pos.z, p->radHeading, r.state, r.jump, r.pos.x, r.pos.y, r.pos.z,
                    r.radHeading, d);
    if (std::getenv("SLY2N_HARNESS_MCARRY") && s_h.oid != 8)
        if (MURRAY* pm = PmurrayFromLo(p->palo))
            std::printf("    carry ref 13EC %x 13F0 %x 1448 %d 144C %d 1450 %d 10DC %x | native 13EC %d 13F0 %d 1448 %d 144C %d 1450 %d 10DC %d\n",
                        r.anCarry[0], r.anCarry[1], r.anCarry[2], r.anCarry[3], r.anCarry[4], r.anCarry[5],
                        pm->plo13EC ? pm->plo13EC->oid : -1, pm->plo13F0 ? pm->plo13F0->oid : -1, pm->n1448, pm->n144C, pm->n1450,
                        pm->plo10DC ? pm->plo10DC->oid : -1);
    if (std::getenv("SLY2N_HARNESS_V"))
    {
        const SOST* ps = Psost(p->palo);
        const glm::vec3 v = ps ? ps->v : glm::vec3(0.0f);
        std::printf("    vecVel native %.3f %.3f %.3f ref %.3f %.3f %.3f floor %p v native %.2f %.2f %.2f ref %.2f %.2f %.2f\n", p->vecVel.x,
                    p->vecVel.y, p->vecVel.z, r.vecVel.x, r.vecVel.y, r.vecVel.z, static_cast<void*>(p->pxpFloor), v.x, v.y, v.z, r.v.x,
                    r.v.y, r.v.z);
        if (std::getenv("SLY2N_HARNESS_REC2") && p->palo->apxfRef[2])
        {
            const XF* p2 = p->palo->apxfRef[2];
            for (int k = 0; k < 3; ++k)
                if (const XF* px = p->palo->axf.size() > size_t(k) ? p->palo->axf[k].get() : nullptr)
                    std::printf("    record %d world native %.2f %.2f %.2f mode %d | ref %.2f %.2f %.2f mode %d\n", k, px->matWorld[3].x,
                                px->matWorld[3].y, px->matWorld[3].z, px->mode, r.aposW[k].x, r.aposW[k].y, r.aposW[k].z, r.amode[k]);
            std::printf("    object record z native %.3f mode %d v %.2f %.2f %.2f ref %.3f mode %.0f v %.2f %.2f %.2f\n", p2->mat[3].z,
                        p2->mode, p2->vel.x, p2->vel.y, p2->vel.z, r.z2, r.mode2, r.vel2.x, r.vel2.y, r.vel2.z);
        }
        std::printf("    anim asega %p aseg %d t %.3f sma %p\n", static_cast<void*>(p->pasega), p->pasega && p->pasega->paseg ? p->pasega->paseg->oid : -1,
                    p->pasega ? p->pasega->t : 0.0f, static_cast<void*>(p->psma));
        if (XP* x = p->pxpFloor)
            std::printf("    floor xp kind %d n %.3f %.3f %.3f d %.2f mu %.3f end0 vel %.2f %.2f %.2f dv %.2f %.2f %.2f\n", x->xpk, x->normal.x,
                        x->normal.y, x->normal.z, x->gDist, x->mu, x->axpd[0].vel.x, x->axpd[0].vel.y, x->axpd[0].vel.z, x->dv.x, x->dv.y,
                        x->dv.z);
    }
    if (std::getenv("SLY2N_GANGSENS"))
    {
        ALO* pa = p->palo;
        for (const SOE& e : pa->asoe)
            if (e.id)
            {
                glm::vec3 c(0.0f);
                for (const glm::vec3& v : e.geomWorld.apos) c += v;
                if (!e.geomWorld.apos.empty()) c /= float(e.geomWorld.apos.size());
                std::printf("    entry tag %d id 0x%X f48 %d f49 %d f50 %d inoxa %d state %d r %.1f center %.1f %.1f %.1f (%zu points)\n", e.tag, e.id,
                            e.f48, e.f49, e.f50, e.fInOxa, e.state, e.sRadius, c.x, c.y, c.z, e.geomWorld.apos.size());
            }
        if (OXA* po = Poxa(pa))
        {
            int cs = 0;
            for (XP* x = po->pxpSensor; x; x = x->pxpNext) ++cs;
            std::printf("    sensors %d overlaps:", cs);
            for (OX* ox = po->pox; ox; ox = ox->poxNext) std::printf(" %d%s(bytes2 0x%X)", ox->psoOther ? ox->psoOther->oid : -1, ox->pxp ? "*" : "", ox->psoOther ? GrfAloXpBytes(ox->psoOther, 2) : 0);
            std::printf("\n");
        }
    }
    s_stateLast = state;
    s_refLast = r.state;
    static const int s_frameWe = std::getenv("SLY2N_GANGWEF") ? std::atoi(std::getenv("SLY2N_GANGWEF")) : -1;
    if (frame == s_frameWe) DumpGangWe(g_psw);
}

void ReportGangHarness()
{
    if (!s_h.fOn) return;
    std::printf("gang harness (player %d): %d frames compared, %d within 0.5 with the same state, %d with another state; "
                "max |dpos| %.3f\n",
                s_h.oid, s_h.c, s_h.cOk, s_h.cStateDiff, s_h.dposMax);
}
