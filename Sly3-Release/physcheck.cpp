// Unit-level differential checks of the physics port against the reference (Sly2-Release --phys-check FUNC PREFIX):
// PREFIX.<k>.in.ram/.regs and PREFIX.<k>.out.ram are the reference's RAM and registers at the entry and the return
// of the k-th captured call of the original function (Sly2-Reference, SLY2_DUMP_CALL=pc:n:count:PREFIX, run with
// --interp-only so the original code runs). The objects the call works on are imported from the entry RAM into
// native objects, the native function runs on them, and its results are compared with the return RAM.

#include "physram.h"

#include "cls.h"
#include "lo.h"
#include "mat.h"
#include "dzg.h"
#include "so.h"
#include "stepphys.h"
#include "vtnative.h"
#include "util.h"
#include "anim.h"
#include "clock.h"
#include "xfphys.h"
#include "cm.h"
#include "cplcy.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>


namespace
{
float g_gXfParamNil = 0.0f;  // the parameter of invalidated records (orig 0x2C44E0)
constexpr uint32_t G_CLOCK = 0x002DDED0;   // {t, dt}
constexpr uint32_t G_XFSTAMP = 0x003E11E8;  // D_003DD490 + 0x3D58

// An actuator found in RAM: records that the update called it.
struct RAMACT : CTRL
{
    uint32_t addr = 0;
    int ccall = 0;
    void Fetch() override { ++ccall; }  // (slot 4: 0x22DD00 called it)
};

// The objects imported from one RAM image.
struct IMPORT
{
    const RAMIMG& ram;
    explicit IMPORT(const RAMIMG& r) : ram(r) {}
    std::unordered_map<uint32_t, XF*> mpxf;
    std::unordered_map<uint32_t, ALO*> mpalo;
    std::vector<std::unique_ptr<XF>> axfForeign;
    std::vector<std::unique_ptr<ALO>> aaloOwned;
    std::vector<std::unique_ptr<RAMACT>> aact;
    std::vector<std::unique_ptr<float>> agParam;
    std::unordered_map<uint32_t, float*> mpgParam;
    std::vector<std::unique_ptr<ALOLINK>> alink;
    std::vector<std::pair<ALO*, uint32_t>> poxaPending;
    std::vector<std::unique_ptr<ALOCHAIN>> achain;
    std::unordered_map<uint32_t, ALOCHAIN*> mpchain;
    std::unordered_map<uint32_t, uint32_t> mpsegChain;  // segment address -> its chain's
    // A chain segment at address a (the whole chain imported: 0x50-byte header, 0x70-byte segments)
    ALOCHAINE* Segment(uint32_t a)
    {
        const uint32_t h = ram.U32(a + 0x60);
        auto it = mpchain.find(h);
        ALOCHAIN* pc;
        if (it != mpchain.end()) pc = it->second;
        else
        {
            achain.push_back(std::make_unique<ALOCHAIN>());
            pc = achain.back().get();
            mpchain[h] = pc;
            const int n = ram.S32(h);
            const uint32_t as = ram.U32(h + 4);
            pc->n8 = ram.S32(h + 8);
            for (int i = 0; i < 4; i++) pc->ag[i] = ram.G(h + 0xC + 4 * i);
            pc->tLast = ram.G(h + 0x1C);
            pc->vec20 = ram.V3(h + 0x20);
            pc->vec30 = ram.V3(h + 0x30);
            pc->posAnchor = ram.V3(h + 0x40);
            pc->ae.resize(std::max(0, n));
            for (int i = 0; i < n; i++)
            {
                const uint32_t e = as + 0x70 * i;
                ALOCHAINE& s = pc->ae[i];
                s.matRest = ram.M3(e);
                s.vecOffset = ram.V3(e + 0x30);
                s.pos = ram.V3(e + 0x40);
                s.v = ram.V3(e + 0x50);
                s.pchain = pc;
            }
            for (int i = 0; i < n; i++) pc->ae[i].pxf = Xf(ram.U32(as + 0x70 * i + 0x64));
        }
        const uint32_t as = ram.U32(h + 4);
        const int i = int((a - as) / 0x70);
        return (i >= 0 && i < int(pc->ae.size())) ? &pc->ae[i] : nullptr;
    }
    void ResolveOxas()
    {
        for (size_t i = 0; i < poxaPending.size(); i++) Sort(poxaPending[i].first).poxa = Oxa(poxaPending[i].second);
        poxaPending.clear();
    }

    CLS* PclsFromVt(uint32_t vt)
    {
        for (CLS& c : g_acls)
            if (c.vtOrig == vt && vt) return &c;
        return nullptr;
    }
    float* PgParamAt(uint32_t a)
    {
        auto it = mpgParam.find(a);
        if (it != mpgParam.end()) return it->second;
        agParam.push_back(std::make_unique<float>(ram.G(a)));
        return mpgParam[a] = agParam.back().get();
    }
    // A record at address a (owned by an imported ALO when it is in one's array; else a standalone copy).
    XF* Xf(uint32_t a)
    {
        if (!a || !ram.FValid(a, 0x100)) return nullptr;
        auto it = mpxf.find(a);
        if (it != mpxf.end()) return it->second;
        axfForeign.push_back(std::make_unique<XF>());
        XF* p = axfForeign.back().get();
        mpxf[a] = p;
        FillXf(p, a);
        return p;
    }
    void FillXf(XF* p, uint32_t a)
    {
        p->mat = ram.M4(a);
        p->matWorld = ram.M4(a + 0x40);
        p->g0 = ram.G(a + 0x80);
        p->g1 = ram.G(a + 0x84);
        const uint32_t pg = ram.U32(a + 0x88), ps = ram.U32(a + 0x8C);
        p->pg = (pg == a + 0x80) ? nullptr : (pg == 0x2C44E0 ? &g_gXfParamNil : PgParamAt(pg));
        p->pdgScale = (ps == a + 0x84) ? nullptr : (ps == 0x2C44E0 ? &g_gXfParamNil : PgParamAt(ps));
        if (const uint32_t pact = ram.U32(a + 0x90))
        {
            aact.push_back(std::make_unique<RAMACT>());
            aact.back()->addr = pact;
            aact.back()->prio = ram.S32(pact + 0xC);
            p->pctrlFirst = aact.back().get();
        }
        p->kind = ram.S32(a + 0x94);
        p->mode = ram.S16(a + 0x98);
        p->state = ram.S16(a + 0x9A);
        p->tag = ram.S32(a + 0x9C);
        p->nStamp = ram.S32(a + 0xA4);
        p->tTarget = ram.G(a + 0xA8);
        p->lo = ram.S16(a + 0xAC);
        p->hi = ram.S16(a + 0xAE);
        p->vel = glm::vec4(ram.V3(a + 0xB0), p->vel.w);
        p->aseg[0] = ram.V4(a + 0xC0);
        p->aseg[1] = ram.V4(a + 0xD0);
        p->aseg[2] = ram.V4(a + 0xE0);
        p->aseg[3] = ram.V4(a + 0xF0);
        const uint32_t ref = ram.U32(a + 0xA0);
        p->pxfRef = Xf(ref);
        const uint32_t a8 = ram.U32(a + 0xA8);
        if (p->kind == 8 || p->kind == 14 || p->mode == 4) p->pxfOrigin = Xf(a8);
        if (p->mode == 15 && a8)
        {
            alink.push_back(std::make_unique<ALOLINK>());
            ALOLINK* pl = alink.back().get();
            pl->pxf = p;
            pl->n8 = ram.S32(a8 + 8);
            pl->nC = ram.S32(a8 + 0xC);
            p->pvA8 = pl;
            pl->pxfTarget = Xf(ram.U32(a8));  // (0x2C44F0, the placeholder, imports as a record with its identity)
        }
        else if (p->mode == 16 && a8) p->pvA8 = Segment(a8);
        else if (p->mode == 13) p->pvA8 = nullptr;  // (the IK pairs (ik.cpp) are not imported: mode-13 records stay as they are)
    }
    std::unordered_map<uint32_t, SOE*> mpsoe;
    std::unordered_map<uint32_t, const void*> mpgeomel;  // world geometry elements (positions, surfaces, edges) by address
    std::unordered_map<const void*, uint32_t> mpelAddr;  // and back (per element: arrays can be shared in RAM)
    std::unordered_map<const void*, uint32_t> mpgeomelAddr() const { return mpelAddr; }
    std::unordered_map<uint32_t, XP*> mpxp;
    std::unordered_map<uint32_t, OX*> mpox;
    XP* Xp(uint32_t a)
    {
        if (!a) return nullptr;
        auto it = mpxp.find(a);
        if (it != mpxp.end()) return it->second;
        XP* pxp = g_physw.PxpNew();
        mpxp[a] = pxp;
        for (int k = 0; k < 2; k++)
        {
            const uint32_t e = a + 0x40 * k;
            XPD& d = pxp->axpd[k];
            const uint32_t psoe = ram.U32(e);
            if (psoe)
            {
                Alo(ram.U32(psoe + 4), true, false);
                auto is = mpsoe.find(psoe);
                d.psoe = is != mpsoe.end() ? is->second : nullptr;
                const uint32_t pxf = ram.U32(e + 4);
                d.pxf = pxf == 0x2C44F0 ? nullptr : Xf(pxf);
                d.paloRoot = Alo(ram.U32(e + 8), true, false);
                d.palo = Alo(ram.U32(e + 0xC), true, false);
            }
            d.pos = ram.V3(e + 0x10);
            d.posLocal = ram.V3(e + 0x20);
            d.vel = ram.V3(e + 0x30);
        }
        pxp->normal = ram.V3(a + 0x80);
        pxp->gDist = ram.G(a + 0x90);
        pxp->mu = ram.G(a + 0x94);
        pxp->elas = ram.G(a + 0x98);
        pxp->xpk = ram.S32(a + 0x9C);
        auto el = [&](uint32_t v) -> const void* {
            auto it2 = mpgeomel.find(v);
            return it2 != mpgeomel.end() ? it2->second : nullptr;
        };
        pxp->pv0 = el(ram.U32(a + 0xA0));
        pxp->pv1 = el(ram.U32(a + 0xA4));
        pxp->gA0 = ram.G(a + 0xA0);
        pxp->gA4 = ram.G(a + 0xA4);
        pxp->ipxp = ram.S32(a + 0xA8);
        pxp->dv = ram.V3(a + 0xB0);
        pxp->pxpNext = Xp(ram.U32(a + 0xAC));
        return pxp;
    }
    // The world's physics lists (SW +0x3CCC..+0x3D40) with every attachment, its entries, overlaps and contacts.
    void PhysWorld()
    {
        constexpr uint32_t SW = 0x3DD490;
        g_physw.Reset();
        auto list = [&](uint32_t dl) {
            std::vector<uint32_t> a;
            const uint32_t ib = ram.U32(dl + 8);
            for (uint32_t p = ram.U32(dl); p && a.size() < 100000; p = ram.U32(p + ib)) a.push_back(p);
            return a;
        };
        const auto aall = list(SW + 0x3D10);
        for (uint32_t a : aall) Oxa(a, true);
        ResolveOxas();
        for (uint32_t a : aall) g_physw.apoxa.push_back(Oxa(a));
        for (uint32_t a : list(SW + 0x3D1C)) g_physw.apoxaActive.push_back(Oxa(a));
        for (uint32_t a : list(SW + 0x3D28)) g_physw.apoxaDirty1.push_back(Oxa(a));
        for (uint32_t a : list(SW + 0x3D34)) g_physw.apoxaDirty2.push_back(Oxa(a));
        g_physw.fAaoxDirty = ram.S32(SW + 0x3CCC) != 0;
        const int caox = ram.S32(SW + 0x3CD0);
        for (int i = 0; i < 3; i++)
        {
            const uint32_t aaox = ram.U32(SW + 0x3CD4 + 4 * i);
            for (int j = 0; j < caox; j++)
            {
                const uint32_t x = aaox + 12 * j;
                OXA* poxa = Oxa(ram.U32(x + 4));
                const int fMax = ram.S32(x);
                g_physw.aaox[i].push_back(AOX{fMax, poxa, poxa ? (fMax ? &poxa->posMax[i] : &poxa->posMin[i]) : nullptr});
            }
        }
        // the overlaps and the contacts
        for (uint32_t a : aall)
        {
            OXA* poxa = Oxa(a);
            OX** pp = &poxa->pox;
            for (uint32_t x = ram.U32(a + 0x64); x; x = ram.U32(x + 4))
            {
                OX* pox = new OX;
                mpox[x] = pox;
                pox->psoOther = Alo(ram.U32(x + 0xC), true, false);
                pox->fOverlap = ram.S32(x + 0x10);
                *pp = pox;
                pp = &pox->poxNext;
            }
        }
        ResolveOxas();
        for (auto& [x, pox] : mpox)
        {
            auto it = mpox.find(ram.U32(x + 8));
            pox->poxOther = it != mpox.end() ? it->second : nullptr;
            pox->pxp = Xp(ram.U32(x));
        }
        for (uint32_t a : aall)
        {
            OXA* poxa = Oxa(a);
            poxa->pxpInternal = Xp(ram.U32(a + 0x68));
            poxa->pxpSensor = Xp(ram.U32(a + 0x6C));
        }
        ResolveOxas();
    }
    std::unordered_map<uint32_t, OXA*> mpoxa;
    std::vector<std::unique_ptr<OXA>> aoxaOwned;
    static int IdxOf(uint32_t p, uint32_t base, uint32_t cb, int c)
    {
        if (!p || p < base) return -1;
        const uint32_t i = (p - base) / cb;
        return (int(i) < c && (p - base) % cb == 0) ? int(i) : -1;
    }
    void FillGeom(GEOM* pg, uint32_t g)
    {
        int cpos = ram.S32(g), cface = ram.S32(g + 8), cedge = ram.S32(g + 0x10);
        if (cpos < 0 || cpos > 200000 || cface < 0 || cface > 200000 || cedge < 0 || cedge > 400000) cpos = cface = cedge = 0;
        const uint32_t apos = ram.U32(g + 4), aface = ram.U32(g + 0xC), aedge = ram.U32(g + 0x14);
        pg->apos.resize(std::max(0, cpos));
        for (int i = 0; i < cpos; i++) pg->apos[i] = ram.V3(apos + 16 * i);
        pg->asurf.assign(std::max(0, cface), SURF{});
        for (int i = 0; i < cface; i++)
        {
            const uint32_t f = aface + 0x20 * i;
            SURF& su = pg->asurf[i];
            su.normal = ram.V3(f);
            su.gDot = ram.G(f + 0xC);
            su.iedgeFirst = IdxOf(ram.U32(f + 0x10), aedge, 0x10, cedge);
            su.iedgeOfOthers = IdxOf(ram.U32(f + 0x14), aedge, 0x10, cedge);
            su.grfMark = ram.U32(f + 0x18);
            su.ipos0 = ram.S32(f + 0x1C);
        }
        pg->aedge.assign(std::max(0, cedge), EDGE{});
        for (int i = 0; i < cedge; i++)
        {
            const uint32_t e = aedge + 0x10 * i;
            EDGE& ed = pg->aedge[i];
            ed.ipos0 = ram.U16(e);
            ed.ipos1 = ram.U16(e + 2);
            ed.isurf = ram.U16(e + 4);
            ed.isurfOther = ram.U16(e + 6);
            ed.iedgeNextInSurf = IdxOf(ram.U32(e + 8), aedge, 0x10, cedge);
            ed.iedgeNextOfOther = IdxOf(ram.U32(e + 0xC), aedge, 0x10, cedge);
        }
    }
    void FillBsp(BSP* pb, uint32_t t, uint32_t gWorld)
    {
        pb->c = ram.U32(t);
        pb->x4 = ram.U32(t + 4);
        pb->x8 = ram.U32(t + 8);
        const uint32_t anode = ram.U32(t + 0xC), aface = ram.U32(gWorld + 0xC);
        const int c = int(pb->c), cface = ram.S32(gWorld + 8);
        pb->abspn.assign(std::max(0, c), BSPN{});
        for (int i = 0; i < c; i++)
        {
            const uint32_t n = anode + 12 * i;
            BSPN& b = pb->abspn[i];
            const uint32_t pv = ram.U32(n + 8);
            b.isurf = pv == 0x2BBAF0 ? -1 : IdxOf(pv, aface, 0x20, cface);
            b.ibspNeg = IdxOf(ram.U32(n), anode, 12, c);
            b.ibspPos = IdxOf(ram.U32(n + 4), anode, 12, c);
        }
    }
    void FillSoe(SOE* pe, uint32_t e, ALO* palo)
    {
        const uint32_t pxf = ram.U32(e);
        pe->pxf = pxf == 0x2C44F0 ? nullptr : Xf(pxf);
        pe->palo = palo;
        pe->sRadius = ram.G(e + 8);
        pe->tag = ram.S32(e + 0xC);
        const uint64_t fl = ram.U64(e + 0x10);
        pe->f48 = (fl >> 48) & 1;
        pe->f49 = (fl >> 49) & 1;
        pe->f50 = (fl >> 50) & 1;
        pe->f51 = (fl >> 51) & 1;
        pe->state = ram.S16(e + 0x14);
        pe->id = ram.U32(e + 0x18);
        pe->pinned = int16_t(ram.S32(e + 0x1C));
        pe->mat = ram.M4(e + 0x20);
        FillGeom(&pe->geomLocal, e + 0x78);
        FillGeom(&pe->geomWorld, e + 0x90);
        {
            const uint32_t apos = ram.U32(e + 0x94), aface = ram.U32(e + 0x9C), aedge = ram.U32(e + 0xA4);
            for (size_t i = 0; i < pe->geomWorld.apos.size(); i++)
                mpgeomel[apos + 16 * uint32_t(i)] = &pe->geomWorld.apos[i], mpelAddr[&pe->geomWorld.apos[i]] = apos + 16 * uint32_t(i);
            for (size_t i = 0; i < pe->geomWorld.asurf.size(); i++)
                mpgeomel[aface + 32 * uint32_t(i)] = &pe->geomWorld.asurf[i], mpelAddr[&pe->geomWorld.asurf[i]] = aface + 32 * uint32_t(i);
            for (size_t i = 0; i < pe->geomWorld.aedge.size(); i++)
                mpgeomel[aedge + 16 * uint32_t(i)] = &pe->geomWorld.aedge[i], mpelAddr[&pe->geomWorld.aedge[i]] = aedge + 16 * uint32_t(i);
        }
        FillBsp(&pe->bsp, e + 0x68, e + 0x90);
        pe->nStampWorld = ram.S32(e + 0xA8);
        pe->nStampObj = ram.S32(e + 0xAC);
        if (const uint32_t pm = static_cast<uint32_t>(fl & 0xFFFFFFFFu))
        {
            auto m = std::make_shared<SOMESH>();
            const int cpos = ram.S32(e + 0x78);
            const uint32_t av = ram.U32(pm), aa = ram.U32(pm + 4), at = ram.U32(pm + 8);
            for (int i = 0; i < cpos; i++)
            {
                m->av.push_back(ram.V3(av + 16 * i));
                m->aa.push_back(ram.V3(aa + 16 * i));
            }
            const int ctri = ram.S32(e + 0x80);
            for (int i = 0; i < ctri; i++)
                m->atri.push_back({ram.S32(at + 12 * i), ram.S32(at + 12 * i + 4), ram.S32(at + 12 * i + 8)});
            pe->pmesh = m;
        }
        mpsoe[e] = pe;
    }
    void FillSost(SOST* ps, uint32_t a)
    {
        ps->momint = ram.M3(a);
        ps->momintInv = ram.M3(a + 0x30);
        ps->v = ram.V3(a + 0x60);
        ps->w = ram.V3(a + 0x70);
        ps->dvGravity = ram.V3(a + 0x80);
        ps->cnstrAng.normal = ram.V3(a + 0x90);
        ps->cnstrAng.kind = ram.S32(a + 0xA0);
        ps->cnstrLin.normal = ram.V3(a + 0xB0);
        ps->cnstrLin.kind = ram.S32(a + 0xC0);
        ps->m = ram.G(a + 0xD0);
    }
    // An attachment (shallow: its box, flags and root; with fLists its entries)
    OXA* Oxa(uint32_t a, bool fLists = false)
    {
        if (!a) return nullptr;
        auto it = mpoxa.find(a);
        OXA* poxa;
        if (it != mpoxa.end()) poxa = it->second;
        else
        {
            aoxaOwned.push_back(std::make_unique<OXA>());
            poxa = aoxaOwned.back().get();
            mpoxa[a] = poxa;
            poxa->posMin = ram.V3(a);
            poxa->posMax = ram.V3(a + 0x10);
            poxa->posCenter = ram.V3(a + 0x20);
            poxa->sRadius = ram.G(a + 0x30);
            poxa->grf = ram.U64(a + 0x70);
            poxa->pso = Alo(ram.U32(a + 0x60), true, false);
        }
        if (fLists && poxa->apsoe.empty())
            for (uint32_t e = ram.U32(a + 0x34); e; e = ram.U32(e + 0x60))
            {
                Alo(ram.U32(e + 4), true, false);
                auto ie = mpsoe.find(e);
                if (ie != mpsoe.end())
                {
                    poxa->apsoe.push_back(ie->second);
                    ie->second->fInOxa = true;
                }
            }
        return poxa;
    }
    // An ALO at address p with its records (the parent as a stub with its flags).
    std::set<ALO*> setFull;
    // The stepping objects' fields (step.h) at their offsets; the probe pointers and the custom entry as indices.
    void FillStep(ALO* palo, uint32_t p, uint32_t ae, int ce)
    {
        STEPPHYS& s = StepPhysOf(palo);
        s.uTurn = ram.G(p + 0x180);
        s.vecVel = ram.V3(p + 0x190);
        s.nMotion = ram.S32(p + 0x1A4);
        s.radHeading = ram.G(p + 0x1A8);
        s.matHeading = ram.M3(p + 0x1B0);
        s.posTarget = ram.V4(p + 0x1E0);
        s.gScale = ram.G(p + 0x1F0);
        s.vBrake = ram.V3(p + 0x1F4);
        s.uBrake = ram.G(p + 0x200);
        s.fNoContacts = ram.S32(p + 0x204);
        s.pxpFloor = Xp(ram.U32(p + 0x208));
        s.tFloor = ram.G(p + 0x20C);
        s.sFloorTarget = ram.G(p + 0x210);
        s.g214 = ram.G(p + 0x214);
        s.g220 = ram.G(p + 0x220);
        s.posFloor = ram.V4(p + 0x230);
        s.vecBase = ram.V4(p + 0x240);
        s.sSize = ram.G(p + 0x260);
        s.oidMesh = ram.S32(p + 0x264);
        auto isoe = [&](uint32_t pe) { return pe ? IdxOf(pe, ae, 0xB0, ce) : -1; };
        s.isoeCustom = isoe(ram.U32(p + 0x258));
        s.isoeOther = isoe(ram.U32(p + 0x25C));
        s.isoeSource = isoe(ram.U32(p + 0x268));
        if (s.isoeCustom >= 0)
        {
            const uint32_t apos = ram.U32(ae + 0xB0u * uint32_t(s.isoeCustom) + 0x94);
            const int cpos = int(palo->asoe[s.isoeCustom].geomWorld.apos.size());
            s.iposProbe0 = IdxOf(ram.U32(p + 0x218), apos, 16, cpos);
            s.iposProbe1 = IdxOf(ram.U32(p + 0x21C), apos, 16, cpos);
        }
    }
    ALO* Alo(uint32_t p, bool fRecords = true, bool fOxa = true)
    {
        if (!p) return nullptr;
        ALO* palo;
        auto it = mpalo.find(p);
        if (it != mpalo.end())
        {
            palo = it->second;
            if (!fRecords || setFull.count(palo)) return palo;
        }
        else
        {
            // (not an ALO: e.g. a stale contact's end while the lists are being rebuilt)
            CLS* pcls = PclsFromVt(ram.U32(p + 0xC));
            if (!pcls || !FCidDerivedFrom(pcls->cid, 0x30) || !ram.FValid(p, 0x150)) return nullptr;
            aaloOwned.push_back(std::make_unique<ALO>());
            palo = aaloOwned.back().get();
            mpalo[p] = palo;
            palo->pcls = PclsFromVt(ram.U32(p + 0xC));
            palo->oid = ram.S32(p + 0x18);
            palo->grf = ram.U64(p + 0x28);
            palo->fInWorld = (palo->grf >> 60) & 1;
            SetGrfUpdOfLo(palo, ram.GrfTagged(p));
            palo->paloParent = Alo(ram.U32(p + 0x10), false);
            if (!fRecords) return palo;
        }
        setFull.insert(palo);
        const int c = ram.S32(p + 0x50);
        const uint32_t a = ram.U32(p + 0x54);
        for (int i = 0; i < c; i++)
        {
            const uint32_t ar = a + 0x100u * uint32_t(i);
            auto ix = mpxf.find(ar);
            if (ix != mpxf.end())
            {
                // a record imported before as a foreign one: owned by the ALO from now on
                for (auto& u : axfForeign)
                    if (u.get() == ix->second)
                    {
                        palo->axf.push_back(std::move(u));
                        break;
                    }
                palo->axf.back()->palo = palo;
                continue;
            }
            palo->axf.push_back(std::make_unique<XF>());
            palo->axf.back()->palo = palo;
            mpxf[ar] = palo->axf.back().get();
        }
        for (int i = 0; i < c; i++) FillXf(palo->axf[i].get(), a + 0x100u * uint32_t(i));
        for (int i = 0; i < 3; i++) palo->apxfRef[i] = Xf(ram.U32(p + 0x58 + 4 * i));
        SORT& so = Sort(palo);
        int ce = ram.S32(p + 0xE0);
        const uint32_t ae = ram.U32(p + 0xE4);
        // (the class's extra entries, vt slot 63, follow the loaded ones: those owned by this object)
        while (ce >= 0 && ram.U32(ae + 0xB0u * uint32_t(ce) + 4) == p && ram.FValid(ae + 0xB0u * uint32_t(ce), 0xB0)) ce++;
        palo->asoe.assign(std::max(0, ce), SOE{});
        for (int i = 0; i < ce; i++) FillSoe(&palo->asoe[i], ae + 0xB0u * uint32_t(i), palo);
        if (const uint32_t ps = ram.U32(p + 0xE8))
        {
            so.psost = std::make_unique<SOST>();
            FillSost(so.psost.get(), ps);
        }
        so.dpos = ram.V3(p + 0xF0);
        so.drot = ram.V3(p + 0x100);
        for (int i = 0; i < 26; i++) so.abXp[i] = ram.Rd<uint8_t>(p + 0x110 + i);
        palo->gFade = ram.G(p + 0x12C);
        palo->dgFade = ram.G(p + 0x130);
        so.n138 = ram.S32(p + 0x138);
        so.g13C = ram.G(p + 0x13C);
        so.nTreeStamp = ram.S32(p + 0x140);
        so.grf148 = ram.U64(p + 0x148);
        so.s148 = ram.Rd<int8_t>(p + 0x148);
        so.s149 = ram.Rd<int8_t>(p + 0x149);
        if (fOxa) so.poxa = Oxa(ram.U32(p + 0xEC));
        else poxaPending.push_back({palo, ram.U32(p + 0xEC)});
        if (FAloStep(palo)) FillStep(palo, p, ae, ce);
        return palo;
    }
};

IMPORT* s_pimpShim = nullptr;  // the import the test shims read (below)
uint32_t AddrOfAloShim(ALO* p)
{
    if (!s_pimpShim) return 0;
    for (auto& [a, q] : s_pimpShim->mpalo)
        if (q == p) return a;
    return 0;
}
}
extern uint32_t (*g_pfnGrfSlyXpImported)(ALO* palo);
namespace
{
// Sly's slot 85 (0x18DEE0 with 0x143C78) from the reference's RAM for an imported Sly (no native SLY data)
uint32_t GrfSlyXpFromRam(ALO* palo)
{
    const uint32_t a = AddrOfAloShim(palo);
    if (!a || !s_pimpShim) return GrfStepXp(palo);
    const RAMIMG& r = s_pimpShim->ram;
    if (std::getenv("PHYS_GRFXP"))
        std::printf("  grfxp oid %d: +204 %d state %d 4C8 %d 2DC %d DF4 %d\n", palo->oid, r.S32(a + 0x204), r.S32(a + 0x18C8),
                    r.S32(a + 0x4C8), r.S32(a + 0x2DC), r.S32(a + 0xDF4));
    if (r.S32(a + 0x204)) return 0;
    switch (r.S32(a + 0x18C8))
    {
    case 3: case 8: case 21: case 22: return 0;
    case 13: case 14: return 1;
    case 24: return 0xF;
    default: break;
    }
    const int n4C8 = r.S32(a + 0x4C8);
    if (n4C8 == 3 || n4C8 == 4) return 4;
    if (r.S32(a + 0x2DC) != -1) return 1;
    switch (r.S32(a + 0xDF4))
    {
    case -1: case 0: case 6: return 7;
    case 4: case 7: case 8: case 12: return 1;
    default: return 0;
    }
}

struct CHECKER
{
    std::map<std::string, DIFFSTAT> mpstat;
    int cbad = 0;
    void F(const std::string& name, float a, float b, double tol = 2e-4)
    {
        DIFFSTAT& s = mpstat[name];
        s.name = name;
        double d = DiffRel(a, b);
        if (std::isnan(a) && std::isnan(b)) d = 0;
        s.Add(d, tol);
    }
    void I(const std::string& name, long a, long b) { mpstat[name].name = name, mpstat[name].Add(a != b ? 1.0 : 0.0, 0.5); }
    void V(const std::string& name, const glm::vec3& a, const glm::vec3& b, double tol = 2e-4)
    {
        for (int i = 0; i < 3; i++) F(name, a[i], b[i], tol);
    }
    void M(const std::string& name, const glm::mat4& a, const glm::mat4& b, double tol = 2e-4)
    {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) F(name, a[i][j], b[i][j], tol);
    }
    void Report() const
    {
        for (const auto& [k, s] : mpstat)
            std::printf("  %-22s %7d values, %5d over tolerance, max rel diff %.3g\n", k.c_str(), s.c, s.cbad, s.gMax);
    }
};

std::string PathOf(const std::string& prefix, int k, const char* io, const char* ext)
{
    return prefix + "." + std::to_string(k) + "." + io + "." + ext;
}

void SetClock(const RAMIMG& ram)
{
    g_clock.t = ram.G(G_CLOCK);
    g_clock.dt = ram.G(G_CLOCK + 4);
    g_cXfStamp = ram.S32(G_XFSTAMP);
}

// 0x22DE88: the records' update. Records of objects with pair/chain modes (13/15/16, not ported yet) or whose
// actuators were called are counted apart.
int CheckUpdateXfs(const std::string& prefix, int ccall)
{
    CHECKER chk, chkSkipped;
    int cobj = 0, cobjSkipped = 0, cChainChecked = 0, c13 = 0, cAct = 0;
    std::map<int, int> mpmodec;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), ""))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        IMPORT imp(in);
        const uint32_t p = in.A0();
        ALO* palo = imp.Alo(p);
        SetClock(in);
        bool fSkip = false, fChain = false, f13 = false;
        for (auto& pxf : palo->axf)
        {
            mpmodec[pxf->mode]++;
            if (pxf->mode == 13) fSkip = f13 = true;
            if (pxf->mode == 16) fChain = true;
        }
        UpdateAloXfs(palo, g_clock.t, g_clock.dt);
        bool fAct = false;
        for (auto& pact : imp.aact)
            if (pact->ccall) fSkip = fAct = true;
        if (fChain && !fSkip) cChainChecked++;
        if (f13) c13++;
        if (fAct) cAct++;
        CHECKER& c = fSkip ? chkSkipped : chk;
        (fSkip ? cobjSkipped : cobj)++;
        const uint32_t a = in.U32(p + 0x54);
        for (size_t i = 0; i < palo->axf.size(); i++)
        {
            const XF* x = palo->axf[i].get();
            const uint32_t r = a + 0x100u * uint32_t(i);
            c.M("mat", x->mat, out.M4(r));
            c.M("matWorld", x->matWorld, out.M4(r + 0x40), 1e-3);
            c.V("v", glm::vec3(x->vel), out.V3(r + 0xB0), 1e-3);
            c.I("mode", x->mode, out.S16(r + 0x98));
            c.I("state", x->state, out.S16(r + 0x9A));
            c.I("nStamp", x->nStamp, out.S32(r + 0xA4));
            c.V("aseg[0]..F0", glm::vec3(x->aseg[0]), out.V3(r + 0xC0));
            c.V("aseg[0]..F0", glm::vec3(x->aseg[1]), out.V3(r + 0xD0));
            c.V("aseg[0]..F0", glm::vec3(x->aseg[2]), out.V3(r + 0xE0));
            c.V("aseg[0]..F0", glm::vec3(x->aseg[3]), out.V3(r + 0xF0));
            c.F("g0/Hi", x->g0, out.G(r + 0x80));
            c.F("g0/Hi", x->g1, out.G(r + 0x84));
            if (std::getenv("PHYS_VERBOSE") && DiffRel(x->mat[3][0], out.G(r + 0x30)) > 1e-4)
                std::printf("call %d obj %d rec %zu kind %d mode %d->%d state %d: pos %g %g %g vs %g %g %g\n", k, palo->oid, i,
                            x->kind, in.S16(r + 0x98), x->mode, x->state, x->mat[3][0], x->mat[3][1], x->mat[3][2],
                            out.G(r + 0x30), out.G(r + 0x34), out.G(r + 0x38));
        }
        c.I("ALO flags 4/0x10", GrfUpdOfLo(palo) & 0x14, out.GrfTagged(p) & 0x14);
        if (std::getenv("PHYS_VERBOSE"))
            std::printf("call %d flags in 0x%X native 0x%X ref 0x%X\n", k, in.GrfTagged(p), GrfUpdOfLo(palo), out.GrfTagged(p));
        c.I("g_cXfStamp", g_cXfStamp, out.S32(G_XFSTAMP));
    }
    std::printf("0x22DE88 UpdateAloXfs: %d calls checked (%d with chains); apart: %d (%d with IK joints (mode 13), %d whose "
                "actuators were called)\n", cobj, cChainChecked, cobjSkipped, c13, cAct);
    chk.Report();
    std::printf(" apart:\n");
    chkSkipped.Report();
    std::printf(" record modes seen:");
    for (auto [m, n] : mpmodec) std::printf(" %d:%d", m, n);
    std::printf("\n");
    return 0;
}
}

namespace
{
// The records of an ALO against the return RAM.
void CompareXfs(CHECKER& c, const ALO* palo, const RAMIMG& in, const RAMIMG& out, uint32_t p)
{
    const uint32_t a = in.U32(p + 0x54);
    for (size_t i = 0; i < palo->axf.size(); i++)
    {
        const XF* x = palo->axf[i].get();
        const uint32_t r = a + 0x100u * uint32_t(i);
        c.M("xf mat", x->mat, out.M4(r));
        c.M("xf matWorld", x->matWorld, out.M4(r + 0x40), 1e-3);
        c.V("xf v", glm::vec3(x->vel), out.V3(r + 0xB0), 1e-3);
        if (std::getenv("PHYS_XFBAD") && (DiffRel(x->vel.x, out.G(r + 0xB0)) > 1e-3 || DiffRel(x->vel.y, out.G(r + 0xB4)) > 1e-3 ||
                                          DiffRel(x->vel.z, out.G(r + 0xB8)) > 1e-3 || DiffRel(x->mat[3].x, out.G(r + 0x30)) > 1e-3 ||
                                          DiffRel(x->mat[3].y, out.G(r + 0x34)) > 1e-3))
            std::printf("  xf of %d (cid 0x%X) rec %zu mode %d (ref %d): v %.3f %.3f %.3f ref %.3f %.3f %.3f (in %.3f %.3f %.3f) pos %.3f %.3f %.3f ref %.3f %.3f %.3f\n",
                        palo->oid, palo->Cid(), i, x->mode, out.S16(r + 0x98), x->vel.x, x->vel.y, x->vel.z, out.G(r + 0xB0), out.G(r + 0xB4),
                        out.G(r + 0xB8), in.G(r + 0xB0), in.G(r + 0xB4), in.G(r + 0xB8), x->mat[3].x, x->mat[3].y, x->mat[3].z,
                        out.G(r + 0x30), out.G(r + 0x34), out.G(r + 0x38));
        if (std::getenv("PHYS_VOID") && palo->oid == std::atoi(std::getenv("PHYS_VOID")) && (palo->apxfRef[0] == x || palo->apxfRef[1] == x))
            std::printf("  oid %d rec %zu (%s): v %.4f %.4f %.4f ref %.4f %.4f %.4f (in %.4f %.4f %.4f) pos %.3f %.3f %.3f ref %.3f %.3f %.3f\n",
                        palo->oid, i, palo->apxfRef[0] == x ? "pos" : "rot", x->vel.x, x->vel.y, x->vel.z, out.G(r + 0xB0),
                        out.G(r + 0xB4), out.G(r + 0xB8), in.G(r + 0xB0), in.G(r + 0xB4), in.G(r + 0xB8), x->mat[3].x, x->mat[3].y,
                        x->mat[3].z, out.G(r + 0x30), out.G(r + 0x34), out.G(r + 0x38));
        c.I("xf mode/state", x->mode, out.S16(r + 0x98));
        c.I("xf mode/state", x->state, out.S16(r + 0x9A));
        c.I("xf nStamp", x->nStamp, out.S32(r + 0xA4));
        c.V("xf aseg[0]..F0", glm::vec3(x->aseg[3]), out.V3(r + 0xF0));
    }
}
// The entries of an ALO against the return RAM.
void CompareSoes(CHECKER& c, const ALO* palo, const RAMIMG& in, const RAMIMG& out, uint32_t p)
{
    const uint32_t ae = in.U32(p + 0xE4);
    for (size_t i = 0; i < palo->asoe.size(); i++)
    {
        const SOE& e = palo->asoe[i];
        const uint32_t ea = ae + 0xB0u * uint32_t(i);
        c.M("soe mat", e.mat, out.M4(ea + 0x20));
        c.I("soe nStampWorld/Obj", e.nStampWorld, out.S32(ea + 0xA8));
        c.I("soe nStampWorld/Obj", e.nStampObj, out.S32(ea + 0xAC));
        const uint32_t apos = out.U32(ea + 0x94), aface = out.U32(ea + 0x9C);
        for (size_t k = 0; k < e.geomWorld.apos.size(); k++) c.V("soe world pos", e.geomWorld.apos[k], out.V3(apos + 16 * k), 1e-3);
        for (size_t k = 0; k < e.geomWorld.asurf.size(); k++)
        {
            c.V("soe world plane n", e.geomWorld.asurf[k].normal, out.V3(aface + 32 * k), 1e-3);
            if (std::getenv("PHYS_VERBOSE") && glm::length(e.geomWorld.asurf[k].normal - out.V3(aface + 32 * k)) > 1e-3)
            {
                const glm::vec3 n = e.geomWorld.asurf[k].normal, r = out.V3(aface + 32 * k), l = e.geomLocal.asurf[k].normal;
                std::printf("obj %d soe %zu surf %zu: native n %g %g %g d %g, ref %g %g %g d %g, local %g %g %g |M| rows %g %g %g\n", palo->oid, i, k,
                            n.x, n.y, n.z, e.geomWorld.asurf[k].gDot, r.x, r.y, r.z, out.G(aface + 32 * k + 12), l.x, l.y, l.z,
                            e.pxf ? glm::length(glm::vec3(e.pxf->matWorld[0])) : 0.f, e.pxf ? glm::length(glm::vec3(e.pxf->matWorld[1])) : 0.f,
                            e.pxf ? glm::length(glm::vec3(e.pxf->matWorld[2])) : 0.f);
            }
            c.F("soe world plane d", e.geomWorld.asurf[k].gDot, out.G(aface + 32 * k + 12), 1e-3);
        }
        c.F("soe radius", e.sRadius, out.G(ea + 8));
    }
}

// 0x1F3980: the ALO's physics update (the records' update 0x22DE88 included).
int CheckUpdateAloPhys(const std::string& prefix, int ccall)
{
    CHECKER chk, chkSkipped;
    int cobj = 0, cobjSkipped = 0;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), ""))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        g_physw.Reset();
        IMPORT imp(in);
        const uint32_t p = in.A0();
        ALO* palo = imp.Alo(p);
        imp.ResolveOxas();
        SetClock(in);
        bool fSkip = false;
        for (auto& pxf : palo->axf)
            if (pxf->mode == 13) fSkip = true;
        UpdateAloPhys(palo);
        for (auto& pact : imp.aact)
            if (pact->ccall) fSkip = true;
        CHECKER& c = fSkip ? chkSkipped : chk;
        (fSkip ? cobjSkipped : cobj)++;
        CompareXfs(c, palo, in, out, p);
        CompareSoes(c, palo, in, out, p);
        if (SOST* ps = Psost(palo))
        {
            const uint32_t pst = in.U32(p + 0xE8);
            c.V("sost v/w", ps->v, out.V3(pst + 0x60));
            c.V("sost v/w", ps->w, out.V3(pst + 0x70));
        }
        if (OXA* poxa = Poxa(palo)) c.I("oxa dirty flags", long(poxa->grf & 3), long(out.U64(in.U32(p + 0xEC) + 0x70) & 3));
        c.I("ALO flags 4/0x10", GrfUpdOfLo(palo) & 0x14, out.GrfTagged(p) & 0x14);
        c.I("g_cXfStamp", g_cXfStamp, out.S32(G_XFSTAMP));
    }
    std::printf("0x1F3980 UpdateAloPhys: %d calls checked (%d with pair/chain modes or actuator callbacks apart)\n", cobj,
                cobjSkipped);
    chk.Report();
    std::printf(" apart:\n");
    chkSkipped.Report();
    return 0;
}
}

namespace
{
template <class K, class V>
std::unordered_map<V, K> Invert(const std::unordered_map<K, V>& m)
{
    std::unordered_map<V, K> r;
    for (auto& [k, v] : m) r[v] = k;
    return r;
}

// The contacts of a list against the return RAM's list at a.
void CompareXpList(CHECKER& c, const XP* pxp, const RAMIMG& out, uint32_t a, const std::unordered_map<SOE*, uint32_t>& mpsoeAddr,
                   const std::unordered_map<const void*, uint32_t>& mpelAddr, const char* pszWhat, int* pcbadList)
{
    std::vector<const XP*> an;
    for (const XP* p = pxp; p; p = p->pxpNext) an.push_back(p);
    std::vector<uint32_t> ar;
    for (uint32_t x = a; x && ar.size() < 10000; x = out.U32(x + 0xAC)) ar.push_back(x);
    c.I(std::string(pszWhat) + " count", long(an.size()), long(ar.size()));
    if (an.size() != ar.size())
    {
        ++*pcbadList;
        if (std::getenv("PHYS_VERBOSE"))
        {
            std::printf("  %s: native %zu, ref %zu:", pszWhat, an.size(), ar.size());
            for (const XP* p : an) std::printf(" n%d(%.1f)", p->xpk, p->gDist);
            for (uint32_t x : ar) std::printf(" r%d(%.1f)", out.S32(x + 0x9C), out.G(x + 0x90));
            std::printf("\n");
        }
        return;
    }
    auto addr = [](const auto& m, const auto* p) -> uint32_t {
        auto it = m.find(const_cast<std::remove_const_t<std::remove_pointer_t<decltype(p)>>*>(p));
        return it != m.end() ? it->second : 0u;
    };
    for (size_t i = 0; i < an.size(); i++)
    {
        const XP* p = an[i];
        const uint32_t x = ar[i];
        c.I(std::string(pszWhat) + " kind", p->xpk, out.S32(x + 0x9C));
        c.I(std::string(pszWhat) + " of kind " + std::to_string(p->xpk), 0, 0);
        c.I(std::string(pszWhat) + " ends", long(addr(mpsoeAddr, p->axpd[0].psoe)), long(out.U32(x)));
        c.I(std::string(pszWhat) + " ends", long(addr(mpsoeAddr, p->axpd[1].psoe)), long(out.U32(x + 0x40)));
        c.V(std::string(pszWhat) + " pos", p->axpd[0].pos, out.V3(x + 0x10), 1e-3);
        c.V(std::string(pszWhat) + " pos", p->axpd[1].pos, out.V3(x + 0x50), 1e-3);
        c.V(std::string(pszWhat) + " normal", p->normal, out.V3(x + 0x80), 1e-3);
        c.F(std::string(pszWhat) + " gDist", p->gDist, out.G(x + 0x90), 1e-3);
        c.V(std::string(pszWhat) + " dv", p->dv, out.V3(x + 0xB0), 1e-3);
        c.F(std::string(pszWhat) + " mu", p->mu, out.G(x + 0x94), 1e-3);
        c.F(std::string(pszWhat) + " elas", p->elas, out.G(x + 0x98), 1e-3);
        if (p->xpk == 0xE)
        {
            c.F(std::string(pszWhat) + " kind 14 limits", p->gA0, out.G(x + 0xA0), 1e-3);
            c.F(std::string(pszWhat) + " kind 14 limits", p->gA4, out.G(x + 0xA4), 1e-3);
        }
        if (std::getenv("PHYS_XPDUMP"))
            std::printf("  xp %zu kind %d oids %d / %d n %.3f %.3f %.3f d %.2f mu %.3f / %.3f elas %.3f / %.3f lim %.2f %.2f / %.2f %.2f dv %.1f %.1f %.1f / %.1f %.1f %.1f\n",
                        i, p->xpk, p->axpd[0].palo ? p->axpd[0].palo->oid : -1, p->axpd[1].palo ? p->axpd[1].palo->oid : -1, p->normal.x,
                        p->normal.y, p->normal.z, p->gDist, p->mu, out.G(x + 0x94), p->elas, out.G(x + 0x98), p->gA0, p->gA4,
                        out.G(x + 0xA0), out.G(x + 0xA4), p->dv.x, p->dv.y, p->dv.z, out.G(x + 0xB0), out.G(x + 0xB4), out.G(x + 0xB8));
        if (p->xpk != 0xE)
        {
            auto ia = mpelAddr.find(p->pv0);
            auto ib = mpelAddr.find(p->pv1);
            c.I(std::string(pszWhat) + " tags", long(p->pv0 ? (ia != mpelAddr.end() ? ia->second : 1) : 0), long(out.U32(x + 0xA0)));
            c.I(std::string(pszWhat) + " tags", long(p->pv1 ? (ib != mpelAddr.end() ? ib->second : 1) : 0), long(out.U32(x + 0xA4)));
        }
    }
}

// The physics world's lists and contacts against the return RAM (after 0x134040 / 0x1F8F80).
void ComparePhysWorld(CHECKER& c, IMPORT& imp, const RAMIMG& in, const RAMIMG& out, int* pcbadList)
{
    const auto mpaloAddr = Invert(imp.mpalo);
    const auto mpoxaAddr = Invert(imp.mpoxa);
    const auto mpsoeAddr = Invert(imp.mpsoe);
    const auto mpelAddr = imp.mpgeomelAddr();
    constexpr uint32_t SW = 0x3DD490;
    c.I("lists: dirty 1/2 empty", long(g_physw.apoxaDirty1.size() + g_physw.apoxaDirty2.size()),
        long((out.U32(SW + 0x3D28) != 0) + (out.U32(SW + 0x3D34) != 0)));
    const int caox = out.S32(SW + 0x3CD0);
    for (int i = 0; i < 3; i++)
    {
        c.I("aox count", long(g_physw.aaox[i].size()), caox);
        const uint32_t aaox = out.U32(SW + 0x3CD4 + 4 * i);
        for (int j = 0; j < caox && j < int(g_physw.aaox[i].size()); j++)
        {
            const AOX& x = g_physw.aaox[i][j];
            auto it = mpoxaAddr.find(x.poxa);
            c.I("aox order", long(it != mpoxaAddr.end() ? it->second : 0) * 2 + x.fMax, long(out.U32(aaox + 12 * j + 4)) * 2 + out.S32(aaox + 12 * j));
        }
    }
    for (OXA* poxa : g_physw.apoxa)
    {
        auto ia = mpoxaAddr.find(poxa);
        if (ia == mpoxaAddr.end()) continue;
        const uint32_t a = ia->second;
        if (std::getenv("PHYS_VERBOSE") && (glm::length(poxa->posMin - out.V3(a)) > 1e-2 || glm::length(poxa->posMax - out.V3(a + 0x10)) > 1e-2))
            std::printf("  oxa %08x (oid %d, %zu entries): native min %g %g %g max %g %g %g r %g, ref min %g %g %g max %g %g %g r %g (in min %g %g %g)\n", a,
                        poxa->pso ? poxa->pso->oid : -1, poxa->apsoe.size(), poxa->posMin.x, poxa->posMin.y, poxa->posMin.z, poxa->posMax.x,
                        poxa->posMax.y, poxa->posMax.z, poxa->sRadius, out.G(a), out.G(a + 4), out.G(a + 8), out.G(a + 0x10),
                        out.G(a + 0x14), out.G(a + 0x18), out.G(a + 0x30), in.G(a), in.G(a + 4), in.G(a + 8));
        c.V("oxa box", poxa->posMin, out.V3(a), 1e-3);
        c.V("oxa box", poxa->posMax, out.V3(a + 0x10), 1e-3);
        c.V("oxa box", poxa->posCenter, out.V3(a + 0x20), 1e-3);
        c.F("oxa box", poxa->sRadius, out.G(a + 0x30), 1e-3);
        c.I("oxa grf", long(poxa->grf & 0x1F), long(out.U64(a + 0x70) & 0x1F));
        // the overlaps: by the other object
        std::unordered_map<uint32_t, uint32_t> mpref;  // other object -> its OX in the return RAM
        int cref = 0;
        for (uint32_t x = out.U32(a + 0x64); x && cref < 10000; x = out.U32(x + 4), cref++) mpref[out.U32(x + 0xC)] = x;
        int cnat = 0;
        for (OX* pox = poxa->pox; pox; pox = pox->poxNext) cnat++;
        c.I("ox count", cnat, cref);
        for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
        {
            auto io = mpaloAddr.find(pox->psoOther);
            const uint32_t aOther = io != mpaloAddr.end() ? io->second : 0;
            auto ir = mpref.find(aOther);
            if (ir == mpref.end())
            {
                c.I("ox missing in ref", 1, 0);
                continue;
            }
            c.I("ox fOverlap", pox->fOverlap, out.S32(ir->second + 0x10));
            CompareXpList(c, pox->pxp, out, out.U32(ir->second), mpsoeAddr, mpelAddr, "xp", pcbadList);
        }
        CompareXpList(c, poxa->pxpInternal, out, out.U32(a + 0x68), mpsoeAddr, mpelAddr, "xp internal", pcbadList);
        CompareXpList(c, poxa->pxpSensor, out, out.U32(a + 0x6C), mpsoeAddr, mpelAddr, "xp sensor", pcbadList);
    }
    (void)in;
}

// 0x134040: the contacts of the moved attachments (the boxes, the sweep lists, the pairs' contacts).
int CheckUpdateSwXps(const std::string& prefix, int ccall)
{
    CHECKER chk;
    int cbadList = 0;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), ""))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        IMPORT imp(in);
        s_pimpShim = &imp;
        imp.PhysWorld();
        SetClock(in);
        g_uRandState = in.U32(in.U32(0x2C49D4) + 0x58);
        std::printf("call %d: %zu attachments (%zu active, dirty %zu / %zu), %zu objects\n", k, g_physw.apoxa.size(),
                    g_physw.apoxaActive.size(), g_physw.apoxaDirty1.size(), g_physw.apoxaDirty2.size(), imp.mpalo.size());
        UpdateSwXps();
        ComparePhysWorld(chk, imp, in, out, &cbadList);
    }
    std::printf("0x134040 UpdateSwXps: %d calls, %d contact lists of another length\n", ccall, cbadList);
    chk.Report();
    return 0;
}
}

namespace
{
// Test shims for classes' own physics slots the port does not cover (registered for the checks only): they read
// the class's fields from the entry RAM of the call being checked.
uint32_t AddrOfAlo(ALO* p)
{
    if (!s_pimpShim) return 0;
    for (auto& [a, q] : s_pimpShim->mpalo)
        if (q == p) return a;
    return 0;
}
// orig: 0x001CE170 (class 0x57 slot 49: the attachment's box from the object's saved one at +0x170..+0x1A0)
void ShimRestoreAttachment(ALO* p)
{
    const uint32_t a = AddrOfAlo(p);
    OXA* poxa = Poxa(p);
    if (!a || !poxa) return;
    const RAMIMG& ram = s_pimpShim->ram;
    poxa->posMin = ram.V3(a + 0x170);
    poxa->posMax = ram.V3(a + 0x180);
    poxa->posCenter = ram.V3(a + 0x190);
    poxa->sRadius = ram.G(a + 0x1A0);
    g_physw.fAaoxDirty = true;
    UnmarkOxaDirty1(p);
}
// orig: 0x001CE6C0 (class 0x57 slot 62: still unless its state +0x150 is 1)
bool ShimStateNotOne(ALO* p)
{
    const uint32_t a = AddrOfAlo(p);
    return a ? s_pimpShim->ram.S32(a + 0x150) != 1 : true;
}
}

namespace
{
// 0x1AA1D8: the contacts of one pair of attachments (the result in $v0 at the return).
int CheckMarkOxaContacts(const std::string& prefix, int ccall)
{
    CHECKER chk;
    int cbadList = 0;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), PathOf(prefix, k, "out", "regs")))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        IMPORT imp(in);
        s_pimpShim = &imp;
        imp.PhysWorld();
        SetClock(in);
        OXA* pa = imp.Oxa(in.A1(), true);
        OXA* pb = imp.Oxa(in.A2(), true);
        imp.ResolveOxas();
        XP* pxp = PxpMarkOxaContacts(pa, pb);
        const auto mpsoeAddr = Invert(imp.mpsoe);
        const auto mpelAddr = imp.mpgeomelAddr();
        if (std::getenv("PHYS_VERBOSE"))
        {
            std::printf("call %d: oids %d / %d, %zu / %zu entries:", k, pa->pso ? pa->pso->oid : -1, pb->pso ? pb->pso->oid : -1,
                        pa->apsoe.size(), pb->apsoe.size());
            for (SOE* pe : pa->apsoe) std::printf(" %d", pe->tag);
            std::printf(" /");
            for (SOE* pe : pb->apsoe) std::printf(" %d", pe->tag);
            std::printf("\n");
        }
        CompareXpList(chk, pxp, out, out.Reg(2), mpsoeAddr, mpelAddr, "xp", &cbadList);
        // (the sensors' contacts go to the attachments' own lists +0x6C)
        int cbadSensor = cbadList;
        CompareXpList(chk, pa->pxpSensor, out, out.U32(in.A1() + 0x6C), mpsoeAddr, mpelAddr, "xp sensor a", &cbadList);
        CompareXpList(chk, pb->pxpSensor, out, out.U32(in.A2() + 0x6C), mpsoeAddr, mpelAddr, "xp sensor b", &cbadList);
        if (std::getenv("PHYS_VERBOSE") && cbadSensor != cbadList)
            std::printf("call %d: oids %d / %d: sensor lists of another length\n", k, pa->pso ? pa->pso->oid : -1, pb->pso ? pb->pso->oid : -1);
    }
    std::printf("0x1AA1D8 PxpMarkOxaContacts: %d calls, %d contact lists of another length\n", ccall, cbadList);
    chk.Report();
    return 0;
}
}

namespace
{
// A pruned node list of the entry's tree (12-byte nodes at a, c of them; children in the list or the sentinel nodes).
std::vector<BSPN> PrunedNodes(const RAMIMG& ram, uint32_t a, int c, uint32_t gWorld)
{
    std::vector<BSPN> v(std::max(0, c));
    const uint32_t aface = ram.U32(gWorld + 0xC);
    const int cface = ram.S32(gWorld + 8);
    auto child = [&](uint32_t p) -> int {
        if (!p) return IBSP_NONE;
        if (p == 0x2BBB10) return IBSP_EMPTY;
        if (p == 0x2BBB20) return IBSP_SOLID;
        return (p >= a && (p - a) % 12 == 0 && int((p - a) / 12) < c) ? int((p - a) / 12) : IBSP_NONE;
    };
    for (int i = 0; i < c; i++)
    {
        const uint32_t n = a + 12 * i;
        const uint32_t pv = ram.U32(n + 8);
        v[i].isurf = pv == 0x2BBAF0 ? ISURF_BEHIND : (pv == 0x2BBAD0 ? ISURF_FRONT
                     : ((pv >= aface && int((pv - aface) / 32) < cface) ? int((pv - aface) / 32) : ISURF_BEHIND));
        v[i].ibspNeg = child(ram.U32(n));
        v[i].ibspPos = child(ram.U32(n + 4));
    }
    return v;
}

// The new contacts a call prepended to the list *ppxp (from the return RAM's head to the entry's head).
uint32_t HeadNew(const RAMIMG& in, const RAMIMG& out, uint32_t ppxp, std::vector<uint32_t>* pa)
{
    const uint32_t headOld = in.U32(ppxp);
    for (uint32_t x = out.U32(ppxp); x && x != headOld && pa->size() < 10000; x = out.U32(x + 0xAC)) pa->push_back(x);
    return out.U32(ppxp);
}

// 0x1A8B50 / 0x1A9D38 / 0x1A9B28: one contact maker on two entries; compares the contacts it prepended.
int CheckMarker(const std::string& fn, const std::string& prefix, int ccall)
{
    CHECKER chk;
    int cbadList = 0;
    std::map<int, int> mpkind;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), PathOf(prefix, k, "out", "regs")))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        g_physw.Reset();
        IMPORT imp(in);
        s_pimpShim = &imp;
        SetClock(in);
        const uint32_t aa = in.A1(), ab = in.A2();
        imp.Alo(in.U32(aa + 4), true, false);
        imp.Alo(in.U32(ab + 4), true, false);
        imp.ResolveOxas();
        SOE* a = imp.mpsoe[aa];
        SOE* b = imp.mpsoe[ab];
        XP* head = nullptr;
        uint32_t ppxp = 0;
        if (fn == "1A8B50")
        {
            ppxp = in.Reg(10);  // $t2 (the 7th argument)
            const int cbsp = in.S32(in.Reg(7) == 0 ? 0 : 0) * 0 + int(in.Reg(7));  // $a3
            const uint32_t absp = in.Reg(8), pbsp = in.Reg(9);
            const std::vector<BSPN> v = PrunedNodes(in, absp, cbsp, ab + 0x90);
            int iroot = IBSP_NONE;
            if (pbsp == 0x2BBB20) iroot = IBSP_SOLID;
            else if (pbsp && pbsp >= absp) iroot = int((pbsp - absp) / 12);
            std::vector<BSPN> vroot = v;
            if (iroot >= int(vroot.size()) && pbsp)
            {
                // the root outside the counted list (the original's cbsp excludes it: the last node)
                vroot = PrunedNodes(in, absp, iroot + 1, ab + 0x90);
            }
            MarkBspBspContacts(a, b, vroot, iroot, &head);
        }
        else if (fn == "1A9D38")
        {
            ppxp = in.Reg(7);
            MarkSweptVerticesCheck(a, b, &head);
        }
        else if (fn == "1A9B28")
        {
            ppxp = in.Reg(9);  // $t1
            const int cbsp = int(in.Reg(7));
            const uint32_t absp = in.Reg(8);
            std::vector<BSPN> v = absp ? PrunedNodes(in, absp, cbsp, ab + 0x90) : std::vector<BSPN>{};
            MarkSphereBspContacts(a, b, absp ? &v : nullptr, &head);
        }
        std::vector<uint32_t> ar;
        HeadNew(in, out, ppxp, &ar);
        // compare (the native list holds only the new contacts)
        const auto mpsoeAddr = Invert(imp.mpsoe);
        const auto mpelAddr = imp.mpgeomelAddr();
        int cn = 0;
        for (XP* p = head; p; p = p->pxpNext) cn++, mpkind[p->xpk]++;
        chk.I("count", cn, long(ar.size()));
        if (cn != int(ar.size()))
        {
            cbadList++;
            if (std::getenv("PHYS_VERBOSE"))
            {
                std::printf("  call %d: native %d, ref %zu:", k, cn, ar.size());
                for (XP* p = head; p; p = p->pxpNext) std::printf(" n%d(%.2f)", p->xpk, p->gDist);
                for (uint32_t x : ar) std::printf(" r%d(%.2f)", out.S32(x + 0x9C), out.G(x + 0x90));
                std::printf("\n");
            }
            continue;
        }
        if (!ar.empty()) CompareXpList(chk, head, out, ar[0], mpsoeAddr, mpelAddr, "xp", &cbadList);
    }
    std::printf("0x%s: %d calls, %d contact lists of another length; native kinds:", fn.c_str(), ccall, cbadList);
    for (auto [kk, n] : mpkind) std::printf(" %d:%d", kk, n);
    std::printf("\n");
    chk.Report();
    return 0;
}
}

namespace
{
// Every imported ALO with records against the return RAM (records, pending displacements), and the world's lists.
void CompareAllAlos(CHECKER& c, IMPORT& imp, const RAMIMG& in, const RAMIMG& out)
{
    for (auto& [a, palo] : imp.mpalo)
    {
        if (!imp.setFull.count(palo)) continue;
        CompareXfs(c, palo, in, out, a);
        if (palo->psort)
        {
            c.V("so dpos/drot", palo->psort->dpos, out.V3(a + 0xF0), 1e-3);
            c.V("so dpos/drot", palo->psort->drot, out.V3(a + 0x100), 1e-3);
        }
    }
    c.I("g_cXfStamp", g_cXfStamp, out.S32(G_XFSTAMP));
}

// 0x1F9410 (one group solved), 0x1F8F80 (a pass), 0x1F8DD8 (the frame's physics).
int CheckSolve(const std::string& fn, const std::string& prefix, int ccall)
{
    CHECKER chk;
    int cbadList = 0;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), PathOf(prefix, k, "out", "regs")))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        IMPORT imp(in);
        s_pimpShim = &imp;
        imp.PhysWorld();
        SetClock(in);
        g_uRandState = in.U32(in.U32(0x2C49D4) + 0x58);
        if (fn == "1F9410")
        {
            const int flags = int(in.A1()), nobj = int(in.A2()), ncont = int(in.Reg(8));
            const uint32_t objs = in.A3(), conts = in.Reg(9);
            std::vector<ALO*> apo;
            std::vector<XP*> apxp;
            for (int i = 0; i < nobj; i++) apo.push_back(imp.Alo(in.U32(objs + 4 * i), true, false));
            for (int i = 0; i < ncont; i++) apxp.push_back(imp.Xp(in.U32(conts + 4 * i)));
            imp.ResolveOxas();
            if (std::getenv("PHYS_VERBOSE"))
            {
                std::printf("call %d: flags %d, %d objects (", k, flags, nobj);
                for (ALO* p : apo) std::printf(" %d", p->oid);
                std::printf(" ), %d contacts (", ncont);
                for (XP* p : apxp) std::printf(" %d", p->xpk);
                std::printf(" )\n");
            }
            SolveGroup(flags, nobj, apo.data(), ncont, apxp.data());
            const auto mpsoeAddr = Invert(imp.mpsoe);
            const auto mpelAddr = imp.mpgeomelAddr();
            for (int i = 0; i < ncont; i++)
            {
                XP* p = apxp[i];
                XP* next = p->pxpNext;
                p->pxpNext = nullptr;  // (each contact alone)
                const uint32_t x = in.U32(conts + 4 * i);
                {
                    chk.V("xp pos", p->axpd[0].pos, out.V3(x + 0x10), 1e-3);
                    chk.V("xp pos", p->axpd[1].pos, out.V3(x + 0x50), 1e-3);
                    chk.F("xp gDist", p->gDist, out.G(x + 0x90), 1e-3);
                    chk.V("xp dv", p->dv, out.V3(x + 0xB0), 1e-3);
                    chk.I("xp ipxp", p->ipxp, out.S32(x + 0xA8));
                }
                p->pxpNext = next;
            }
        }
        else if (fn == "1F8F80")
        {
            imp.ResolveOxas();
            SolvePassSw(int(in.A1()));
            ComparePhysWorld(chk, imp, in, out, &cbadList);
        }
        else if (fn == "1F8DD8")
        {
            // the world's entries for slot 17 (SW +0x3F14 count, +0x3F20 {grf, po})
            constexpr uint32_t SW = 0x3DD490;
            std::vector<LO*> aplo;
            const int c = in.S32(SW + 0x3F14);
            const uint32_t ae = in.U32(SW + 0x3F20);
            int cskip = 0;
            for (int i = 0; i < c; i++)
            {
                const uint32_t po = in.U32(ae + 8 * i + 4);
                if (!po) continue;
                if ((in.U32(ae + 8 * i) & 0x24) != 4) continue;
                ALO* palo = imp.Alo(po, false, false);
                if (!palo || !palo->pcls || !FCidDerivedFrom(palo->pcls->cid, 0x30))
                {
                    cskip++;
                    continue;
                }
                palo = imp.Alo(po, true, false);
                SetGrfUpdOfLo(palo, in.U32(ae + 8 * i));
                aplo.push_back(palo);
            }
            imp.ResolveOxas();
            SetPhysSwObjects(aplo);
            g_pfnUpdateSwEntries17 = nullptr;
            UpdateSwPhys();
            ComparePhysWorld(chk, imp, in, out, &cbadList);
            if (std::getenv("PHYS_VERBOSE")) std::printf("call %d: %zu entries for slot 17 (%d not ALOs)\n", k, aplo.size(), cskip);
        }
        if (std::getenv("PHYS_VERBOSE"))
        {
            CHECKER c1;
            CompareAllAlos(c1, imp, in, out);
            int cbad = 0;
            for (auto& [kk, st] : c1.mpstat) cbad += st.cbad;
            std::printf("  call %d: %d values over tolerance\n", k, cbad);
            if (cbad) c1.Report();
        }
        CompareAllAlos(chk, imp, in, out);
    }
    std::printf("0x%s: %d calls, %d contact lists of another length\n", fn.c_str(), ccall, cbadList);
    chk.Report();
    return 0;
}
}

namespace
{
// 0x14CC58 (the nearest entry a line hits), 0x11AD30 (a small LCP), 0x1F59A0 (angular braking).
int CheckMisc(const std::string& fn, const std::string& prefix, int ccall)
{
    CHECKER chk;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), PathOf(prefix, k, "out", "regs")))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        g_physw.Reset();
        IMPORT imp(in);
        s_pimpShim = &imp;
        SetClock(in);
        if (fn == "14CC58")
        {
            const int grf = int(in.A0());
            const glm::vec3 a = glm::vec3(in.QReg(5)), b = glm::vec3(in.QReg(6));
            const int cso = int(in.A3());
            const uint32_t apso = in.Reg(8), plsg = in.Reg(9);
            std::vector<SOE*> ap;
            for (int i = 0; i < cso; i++)
            {
                const uint32_t e = in.U32(apso + 4 * i);
                imp.Alo(in.U32(e + 4), true, false);
                ap.push_back(imp.mpsoe[e]);
            }
            LSG lsg;
            SOE* pr = PsoeHitTestLineObjects(grf, a, b, ap, &lsg);
            const auto mpsoeAddr = Invert(imp.mpsoe);
            auto it = pr ? mpsoeAddr.find(pr) : mpsoeAddr.end();
            chk.I("hit entry", long(it != mpsoeAddr.end() ? it->second : 0), long(out.Reg(2)));
            if (pr && plsg)
            {
                chk.V("lsg pos", lsg.pos0, out.V3(plsg), 1e-3);
                chk.V("lsg pos", lsg.pos1, out.V3(plsg + 0x10), 1e-3);
                chk.F("lsg t", lsg.t0, out.G(plsg + 0x40), 1e-3);
                chk.F("lsg t", lsg.t1, out.G(plsg + 0x44), 1e-3);
            }
        }
        else if (fn == "11AD30")
        {
            const int count = int(in.A0());
            const uint32_t aa = in.A1(), ab = in.A2(), ar = in.A3();
            const float gMax = in.F(12);
            std::vector<float> va(size_t(count) * count), vb(count), vr(count);
            for (int i = 0; i < count * count; i++) va[i] = in.G(aa + 4 * i);
            for (int i = 0; i < count; i++) vb[i] = in.G(ab + 4 * i);
            SolveDzgConstraints(count, va.data(), vb.data(), vr.data(), gMax);
            for (int i = 0; i < count; i++) chk.F("solution", vr[i], out.G(ar + 4 * i), 1e-3);
        }
        else if (fn == "21CB18")
        {
            const uint32_t pa = in.A0(), seg = in.A1();
            ALO* palo = imp.Alo(pa, true, false);
            imp.ResolveOxas();
            ALOCHAINE* pe = imp.Segment(seg);
            UpdateXfChain(palo, pe);
            const uint32_t h = in.U32(seg + 0x60), as = in.U32(h + 4);
            for (size_t i = 0; pe && i < pe->pchain->ae.size(); i++)
            {
                const ALOCHAINE& s2 = pe->pchain->ae[i];
                chk.V("segment pos", s2.pos, out.V3(as + 0x70 * i + 0x40), 1e-3);
                chk.V("segment v", s2.v, out.V3(as + 0x70 * i + 0x50), 1e-3);
            }
            if (pe)
            {
                chk.F("chain tLast", pe->pchain->tLast, out.G(h + 0x1C));
                chk.V("chain anchor", pe->pchain->posAnchor, out.V3(h + 0x40), 1e-3);
                const uint32_t r = in.U32(seg + 0x64);
                chk.M("end record mat", pe->pxf->mat, out.M4(r), 1e-3);
                chk.I("g_cXfStamp", g_cXfStamp, out.S32(G_XFSTAMP));
            }
        }
        else if (fn == "1A93C8")
        {
            const glm::vec3 pos = glm::vec3(in.QReg(4));
            const uint32_t e = in.A1();
            const int cbsp = int(in.A3());
            const uint32_t absp = in.Reg(8), asbi = in.Reg(10);
            const int csbiMax = int(in.Reg(9));
            const float r = in.F(12);
            imp.Alo(in.U32(e + 4), true, false);
            SOE* pe = imp.mpsoe[e];
            if (!pe) continue;
            std::vector<BSPN> v = absp ? PrunedNodes(in, absp, cbsp, e + 0x90) : std::vector<BSPN>{};
            std::vector<glm::vec3> apos;
            std::vector<glm::vec4> aplane;
            std::vector<int> akind;
            std::vector<const void*> apv;
            const int cn = CsbiSphereEntry(pos, pe, absp ? &v : nullptr, csbiMax, r, &apos, &aplane, &akind, &apv);
            const int cr = int(out.Reg(2));
            chk.I("count", cn, cr);
            const auto mpel = imp.mpgeomelAddr();
            for (int i = 0; i < std::min(cn, cr); i++)
            {
                const uint32_t x = asbi + 0x30 * i;
                chk.V("sbi point", apos[i], out.V3(x), 1e-3);
                chk.V("sbi plane n", glm::vec3(aplane[i]), out.V3(x + 0x10), 1e-3);
                chk.F("sbi plane d", aplane[i].w, out.G(x + 0x1C), 1e-3);
                chk.I("sbi kind", akind[i], out.S32(x + 0x20));
                auto it = mpel.find(apv[i]);
                chk.I("sbi tag", long(apv[i] ? (it != mpel.end() ? it->second : 1) : 0), long(out.U32(x + 0x24)));
                chk.I("sbi of kind " + std::to_string(akind[i]), 0, 0);
            }
        }
        // the camera's queries (actors code: cm.cpp / cplcy.cpp on this runtime)
        else if (fn == "14EA08")
        {
            const int n = int(in.A0());
            const uint32_t apo = in.A1(), pgMin = in.Reg(8), pgMax = in.Reg(9);
            CMSOLIDS a;
            for (int i = 0; i < n; i++)
            {
                const uint32_t e = in.U32(apo + 4 * i);
                imp.Alo(in.U32(e + 4), true, false);
                if (SOE* pe = imp.mpsoe[e]) a.push_back(pe);
            }
            float u0 = -1.0f, u1 = -1.0f;
            const bool f = FFindCmClearInterval(in.F(12), a, glm::vec3(in.QReg(6)), glm::vec3(in.QReg(7)), &u0, &u1);
            chk.I("solids " + std::to_string(std::min<size_t>(a.size(), 9)) + (u0 > 0.0f || u1 < 1.0f ? " (blocked)" : ""), 0, 0);
            chk.I("clear", f ? 1 : 0, long(out.Reg(2)));
            if (f && out.Reg(2))
            {
                if (pgMin) chk.F("u from", u0, out.G(pgMin), 1e-4);
                if (pgMax) chk.F("u to", u1, out.G(pgMax), 1e-4);
            }
        }
        else if (fn == "228C18")
        {
            // 0x228C18 (water, pos, &vel, &rot): the water's velocity at the point (0x230A98 / 0x230710 inside)
            const uint32_t pw = in.A0(), ppos = in.Reg(6);
            XF* pxf = imp.Xf(in.U32(pw + 0x60));
            int cctrl = 0;
            for (XF* q = pxf; q; q = q->pxfRef) cctrl += q->pctrlFirst ? 1 : 0;
            if (cctrl)
            {
                chk.I("skipped: a controller's record", 0, 0);
                continue;
            }
            const glm::vec3 v = VelWaterPoint(pxf, in.V3(pw + 0x160), glm::vec3(in.QReg(5)));
            if (std::getenv("PHYS_VERBOSE"))
                std::printf("call %d: water 0x%X flow %.1f %.1f %.1f -> %.3f %.3f %.3f (reference %.3f %.3f %.3f)\n", k, pw,
                            in.V3(pw + 0x160).x, in.V3(pw + 0x160).y, in.V3(pw + 0x160).z, v.x, v.y, v.z, out.V3(ppos).x,
                            out.V3(ppos).y, out.V3(ppos).z);
            if (ppos) chk.V("228C18 water velocity", v, out.V3(ppos), 1e-3);
        }
        else if (fn == "230710" || fn == "230A98" || fn == "2309E8")
        {
            // 0x230710 (pxf, pmat, pdmat, dt): one record's prediction; 0x230A98 (pxf, pxfStop, pmat, pdmat, dt) the chain
            const bool fChain = fn != "230710";  // (0x2309E8: 0x230A98 from the identity and zero)
            const uint32_t pxfA = in.A0();
            const uint32_t pm = fChain ? in.Reg(6) : in.A1(), pdm = fChain ? in.Reg(7) : in.A2();
            const float dt = in.F(12);
            XF* pxf = imp.Xf(pxfA);
            XF* pxfStop = fChain ? imp.Xf(in.A1()) : nullptr;
            int cctrl = 0, cmode[32] = {};
            for (XF* q = pxf; q && q != pxfStop; q = fChain ? q->pxfRef : nullptr)
            {
                cctrl += q->pctrlFirst ? 1 : 0;
                cmode[std::clamp(int(q->mode), 0, 31)]++;
            }
            if (cctrl)
            {
                chk.I("skipped: a controller's record (its slot 5 is not imported)", 0, 0);
                continue;
            }
            glm::mat4 m = pm ? in.M4(pm) : glm::mat4(1.0f), dm = pdm ? in.M4(pdm) : glm::mat4(0.0f);
            if (fn == "2309E8")
            {
                m = glm::mat4(1.0f);
                dm = glm::mat4(0.0f);
            }
            if (fChain)
                PredictXfChain(pxf, pxfStop, dt, pm ? &m : nullptr, pdm ? &dm : nullptr);
            else
                PredictXf(pxf, dt, &m, &dm);
            std::string what = fn + " kind " + std::to_string(pxf ? pxf->kind : -1) + " mode " + std::to_string(pxf ? pxf->mode : -1);
            if (std::getenv("PHYS_VERBOSE"))
            {
                std::printf("call %d: record 0x%X kind %d mode %d dt %.4f, chain:", k, pxfA, pxf ? pxf->kind : -1, pxf ? pxf->mode : -1, dt);
                for (XF* q = pxf; q && q != pxfStop; q = q->pxfRef) std::printf(" (k%d m%d%s)", q->kind, q->mode, q->pctrlFirst ? " ctrl" : "");
                std::printf("\n");
            }
            if (pm) chk.M(what + " mat", m, out.M4(pm), 1e-3);
            if (pdm) chk.M(what + " dmat", dm, out.M4(pdm), 1e-3);
        }
        else if (fn == "14F110" || fn == "223A70")
        {
            imp.PhysWorld();
            g_clock.dtReal = in.G(G_CLOCK + 0x10);
            g_cmworld.pfnDtReal = [] { return g_clock.dtReal; };
            g_cm.sRadiusNearClip = in.G(0x2DDF40 + 0x48);
            if (fn == "14F110")
            {
                const glm::vec3 r = PosCmAvoid(&g_cm, glm::vec3(in.QReg(5)), glm::vec3(in.QReg(6)));
                const glm::vec3 p0 = glm::vec3(in.QReg(5));
                const CMSOLIDS a = AsolidCm(p0 - glm::vec3(250.0f), p0 + glm::vec3(250.0f));
                chk.I("solids in the box " + std::to_string(std::min<size_t>(a.size(), 9)), 0, 0);
                chk.I(glm::length(r - glm::vec3(in.QReg(6))) > 1e-3f ? "pushed by hits" : "no push", 0, 0);
                chk.V("eye move", r, glm::vec3(out.QReg(2)), 1e-4);
            }
            else
            {
                const uint32_t p = in.A0(), pout = in.A3(), obj = in.U32(p);
                const glm::vec3 posObj = in.V3(in.U32(obj + 0x60) + 0x70);
                bool fBlocked = false;
                const glm::vec3 r = PosCmEyeCut(posObj, glm::vec3(in.QReg(5)), glm::vec3(in.QReg(6)), &fBlocked);
                chk.V("eye cut", r, out.V3(pout), 1e-4);
                chk.I("blocked (mode 4)", fBlocked ? 4 : in.S32(p + 0x34), out.S32(p + 0x34));
            }
        }
        else if (fn == "1F59A0")
        {
            const uint32_t p = in.A0(), q = in.A1(), c = in.A2();
            ALO* palo = imp.Alo(p, true, false);
            imp.ResolveOxas();
            const float ac[3] = {in.G(c), in.G(c + 4), in.G(c + 8)};
            const glm::vec3 f0 = PxfRot(palo) ? glm::vec3(PxfRot(palo)->aseg[3]) : glm::vec3(0.0f);
            BrakeAloRotation(palo, in.M3(q), ac);
            CompareXfs(chk, palo, in, out, p);
            if (std::getenv("PHYS_VERBOSE") && PxfRot(palo))
            {
                const uint32_t r = in.U32(p + 0x5C);
                const glm::vec3 n = glm::vec3(PxfRot(palo)->aseg[3]), o = out.V3(r + 0xF0);
                std::printf("call %d: f0 %g %g %g -> native %g %g %g ref %g %g %g (v %g %g %g, ac %g %g %g)\n", k, f0.x, f0.y, f0.z, n.x, n.y,
                            n.z, o.x, o.y, o.z, PxfRot(palo)->vel.x, PxfRot(palo)->vel.y, PxfRot(palo)->vel.z, ac[0], ac[1], ac[2]);
            }
        }
    }
    std::printf("0x%s: %d calls\n", fn.c_str(), ccall);
    chk.Report();
    return 0;
}
}

namespace
{
// Several frames of the physics (0x1F8DD8 calls 0..K-1 of a capture): the world imported from call 0's entry, the
// native frame run K times (the clock from each call's entry), the dynamic objects (a mass state not fixed) compared
// with each call's return. The rest of the world update between the calls is not run natively.
int CheckFrames(const std::string& prefix, int K)
{
    RAMIMG in;
    if (!in.FLoad(PathOf(prefix, 0, "in", "ram"), PathOf(prefix, 0, "in", "regs"))) return 1;
    IMPORT imp(in);
    s_pimpShim = &imp;
    imp.PhysWorld();
    constexpr uint32_t SW = 0x3DD490;
    std::vector<LO*> aplo;
    const int c = in.S32(SW + 0x3F14);
    const uint32_t ae = in.U32(SW + 0x3F20);
    for (int i = 0; i < c; i++)
    {
        const uint32_t po = in.U32(ae + 8 * i + 4);
        if (!po) continue;  // (every entry: the update flags are tested each frame)
        ALO* palo = imp.Alo(po, false, false);
        if (!palo || !palo->pcls || !FCidDerivedFrom(palo->pcls->cid, 0x30)) continue;
        palo = imp.Alo(po, true, false);
        SetGrfUpdOfLo(palo, in.U32(ae + 8 * i));
        aplo.push_back(palo);
    }
    imp.ResolveOxas();
    SetPhysSwObjects(aplo);
    g_pfnUpdateSwEntries17 = nullptr;
    g_uRandState = in.U32(in.U32(0x2C49D4) + 0x58);
    // the dynamic objects
    std::vector<std::pair<uint32_t, ALO*>> adyn;
    for (auto& [a, palo] : imp.mpalo)
    {
        SOST* ps = Psost(palo);
        if (!ps || !imp.setFull.count(palo)) continue;
        if (ps->cnstrLin.kind == 3 && ps->cnstrAng.kind == 3) continue;
        // (only the objects whose slot 17 is the ALO's: the characters' and actors' own updates are theirs)
        if (!palo->pcls || palo->pcls->PfnOrig(17) != 0x1F3980) continue;
        adyn.push_back({a, palo});
    }
    // PHYS_DROP=oid:dz: that object (its first dynamic copy) is lifted by dz before the first frame and woken: it falls
    // back natively; the reference's position is where it rested.
    ALO* paloDrop = nullptr;
    glm::vec3 posRest(0.0f);
    if (const char* pszDrop = std::getenv("PHYS_DROP"))
    {
        int oid = 0;
        float dz = 0.0f;
        std::sscanf(pszDrop, "%d:%f", &oid, &dz);
        for (auto& [a, palo] : adyn)
            if (palo->oid == oid)
            {
                paloDrop = palo;
                break;
            }
        if (paloDrop)
        {
            XF* p58 = PxfPos(paloDrop);
            posRest = PosOf(PxfObj(paloDrop)->matWorld);
            SetPos(&p58->mat, PosOf(p58->mat) + glm::vec3(0.0f, 0.0f, dz));
            p58->nStamp = ++g_cXfStamp;
            UpdateAloXfWorldsHierarchy(paloDrop);
            MarkAloDirty(paloDrop);
            SetXfMode(paloDrop, p58, 1);  // (free: velocity mode, whatever drove it)
            if (PxfRot(paloDrop)) SetXfMode(paloDrop, PxfRot(paloDrop), 1);
            p58->pctrlFirst = nullptr;
            std::printf("constraints lin %d ang %d, gravity %.1f, mass %.2f\n", Psost(paloDrop)->cnstrLin.kind, Psost(paloDrop)->cnstrAng.kind,
                        Psost(paloDrop)->dvGravity.z, Psost(paloDrop)->m);
            WakeAlo(paloDrop);
            std::printf("dropping oid %d from %.1f above its rest position %.2f %.2f %.2f (in the slot-17 list: %d, update flags 0x%X, "
                        "kind of +0x58 %d)\n", oid, dz, posRest.x, posRest.y, posRest.z,
                        int(std::find(aplo.begin(), aplo.end(), static_cast<LO*>(paloDrop)) != aplo.end()), GrfUpdOfLo(paloDrop), p58->kind);
            if (std::find(aplo.begin(), aplo.end(), static_cast<LO*>(paloDrop)) == aplo.end())
            {
                aplo.push_back(paloDrop);
                SetPhysSwObjects(aplo);
            }
        }
    }
    std::printf("%zu dynamic objects:", adyn.size());
    for (auto& [a, palo] : adyn) std::printf(" %d (cid 0x%X)", palo->oid, palo->pcls ? palo->pcls->cid : -1);
    std::printf("\n");
    for (int k = 0; k < K; k++)
    {
        RAMIMG ink, out;
        if (!ink.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) || !out.FLoad(PathOf(prefix, k, "out", "ram"), ""))
            break;
        SetClock(ink);
        g_cXfStamp = std::max(g_cXfStamp, ink.S32(G_XFSTAMP));
        UpdateSwPhys();
        double dMax = 0.0;
        for (auto& [a, palo] : adyn)
        {
            XF* pxf = PxfObj(palo);
            if (!pxf) continue;
            const uint32_t r = in.U32(a + 0x60);
            const glm::vec3 pn = PosOf(pxf->matWorld), pr = out.V3(r + 0x70);
            const double d = glm::length(pn - pr);
            dMax = std::max(dMax, d);
            if (std::getenv("PHYS_VERBOSE"))
                std::printf("  frame %d oid %d: native %.3f %.3f %.3f ref %.3f %.3f %.3f (|d| %.4f) v %.2f %.2f %.2f / %.2f %.2f %.2f\n", k,
                            palo->oid, pn.x, pn.y, pn.z, pr.x, pr.y, pr.z, d, PxfPos(palo)->vel.x, PxfPos(palo)->vel.y,
                            PxfPos(palo)->vel.z, out.G(in.U32(a + 0x58) + 0xB0), out.G(in.U32(a + 0x58) + 0xB4),
                            out.G(in.U32(a + 0x58) + 0xB8));
        }
        if (paloDrop)
        {
            const glm::vec3 pn = PosOf(PxfObj(paloDrop)->matWorld);
            if (OXA* po = std::getenv("PHYS_VERBOSE") ? Poxa(paloDrop) : nullptr)
            {
                int cox = 0;
                for (OX* ox = po->pox; ox; ox = ox->poxNext) cox++;
                for (OX* ox = po->pox; ox; ox = ox->poxNext)
                {
                    OXA* pb = Poxa(ox->psoOther);
                    std::printf("   overlap with oid %d (cid 0x%X, %zu entries%s), box %.1f %.1f %.1f .. %.1f %.1f %.1f\n", ox->psoOther->oid,
                                ox->psoOther->Cid(), pb ? pb->apsoe.size() : 0, pb && !pb->apsoe.empty() && pb->apsoe[0]->f49 ? ", sphere" : "",
                                pb ? pb->posMin.x : 0, pb ? pb->posMin.y : 0, pb ? pb->posMin.z : 0, pb ? pb->posMax.x : 0,
                                pb ? pb->posMax.y : 0, pb ? pb->posMax.z : 0);
                }
                std::printf("   box %.1f %.1f %.1f .. %.1f %.1f %.1f, %zu entries, %d overlaps, grf 0x%llx, root oid %d\n", po->posMin.x,
                            po->posMin.y, po->posMin.z, po->posMax.x, po->posMax.y, po->posMax.z, po->apsoe.size(), cox,
                            (unsigned long long)po->grf, po->pso ? po->pso->oid : -1);
            }
            std::printf("frame %d: oid %d at %.2f %.2f %.2f (%.3f from its rest), v %.2f %.2f %.2f, mode %d, %d contacts\n", k,
                        paloDrop->oid, pn.x, pn.y, pn.z, glm::length(pn - posRest), PxfPos(paloDrop)->vel.x, PxfPos(paloDrop)->vel.y,
                        PxfPos(paloDrop)->vel.z, PxfPos(paloDrop)->mode, [&] {
                            int c = 0;
                            if (OXA* po = Poxa(paloDrop))
                                for (OX* ox = po->pox; ox; ox = ox->poxNext)
                                    for (XP* x = ox->pxp; x; x = x->pxpNext) c++;
                            return c;
                        }());
            continue;
        }
        std::printf("frame %d (t %.3f): the dynamic objects' positions within %.4g units of the reference's\n", k, g_clock.t, dMax);
    }
    return 0;
}

// The stepping objects' slots (step.cpp): 204768 (58), 204AC8 (60), 2044B0 (57), 204250 (55), 204108 (70).
int CheckStep(const std::string& fn, const std::string& prefix, int ccall)
{
    CHECKER chk;
    int cbadList = 0, cdone = 0;
    std::map<int, int> mpcid;
    for (int k = 0; k < ccall; k++)
    {
        RAMIMG in, out;
        if (!in.FLoad(PathOf(prefix, k, "in", "ram"), PathOf(prefix, k, "in", "regs")) ||
            !out.FLoad(PathOf(prefix, k, "out", "ram"), PathOf(prefix, k, "out", "regs")))
        {
            std::fprintf(stderr, "call %d: missing files\n", k);
            continue;
        }
        IMPORT imp(in);
        s_pimpShim = &imp;
        imp.PhysWorld();
        SetClock(in);
        const uint32_t p = in.A0();
        ALO* palo = imp.Alo(p);
        if (!palo || !FAloStep(palo)) continue;
        mpcid[palo->Cid()]++;
        cdone++;
        STEPPHYS& s = StepPhysOf(palo);
        const auto mpsoeAddr = Invert(imp.mpsoe);
        const auto mpelAddr = imp.mpgeomelAddr();
        const auto mpxpAddr = Invert(imp.mpxp);
        auto addrXp = [&](const XP* x) -> long {
            if (!x) return 0;
            auto it = mpxpAddr.find(const_cast<XP*>(x));
            return it != mpxpAddr.end() ? long(it->second) : -1;
        };
        if (fn == "204768")
        {
            FindStepFloor(palo);
            chk.I("floor contact", addrXp(s.pxpFloor), long(out.U32(p + 0x208)));
            if (s.pxpFloor)
            {
                chk.F("floor time", s.tFloor, out.G(p + 0x20C));
                chk.V("floor point", glm::vec3(s.posFloor), out.V3(p + 0x230));
            }
            // the object's attachment's lists after the frees
            OXA* poxa = Poxa(palo);
            const uint32_t aoxa = in.U32(p + 0xEC);
            if (poxa && aoxa)
            {
                int cn = 0, cr = 0;
                for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
                    for (XP* x = ox->pxp; x; x = x->pxpNext) cn++;
                for (XP* x = poxa->pxpInternal; x; x = x->pxpNext) cn++;
                for (uint32_t x = out.U32(aoxa + 0x64); x; x = out.U32(x + 4))
                    for (uint32_t y = out.U32(x); y && cr < 100000; y = out.U32(y + 0xAC)) cr++;
                for (uint32_t y = out.U32(aoxa + 0x68); y && cr < 100000; y = out.U32(y + 0xAC)) cr++;
                chk.I("contacts left", cn, cr);
            }
        }
        else if (fn == "204AC8")
        {
            auto it = imp.mpsoe.find(in.A1());
            SOE* pk = it != imp.mpsoe.end() ? it->second : nullptr;
            const uint32_t ppxp = in.Reg(9);  // (the sixth argument: $t1)
            XP* head = imp.Xp(in.U32(ppxp));
            AddStepFloorXps(palo, pk, &head);
            CompareXpList(chk, head, out, out.U32(ppxp), mpsoeAddr, mpelAddr, "xp", &cbadList);
        }
        else if (fn == "2044B0" || fn == "204250")
        {
            const uint32_t aq = in.A1();
            XP* q = imp.Xp(aq);
            const int n = int(in.A2());
            if (fn == "2044B0") OnStepXpUpdate(palo, q, n);
            else OnStepXpNew(palo, q, n);
            chk.V("xp end 0 velocity", q->axpd[0].vel, out.V3(aq + 0x30), 1e-3);
            chk.V("xp end 1 velocity", q->axpd[1].vel, out.V3(aq + 0x70), 1e-3);
            chk.F("xp gDist", q->gDist, out.G(aq + 0x90), 1e-3);
            chk.F("xp mu / elas", q->mu, out.G(aq + 0x94));
            chk.F("xp mu / elas", q->elas, out.G(aq + 0x98));
            if (std::getenv("PHYS_VERBOSE"))
                std::printf("call %d oid %d cid 0x%X kind %d n %d: limits %g %g / %g %g, end 0 v %g %g %g / %g %g %g, mass %g\n", k,
                            palo->oid, palo->Cid(), q->xpk, n, q->gA0, q->gA4, out.G(aq + 0xA0), out.G(aq + 0xA4), q->axpd[0].vel.x,
                            q->axpd[0].vel.y, q->axpd[0].vel.z, out.G(aq + 0x30), out.G(aq + 0x34), out.G(aq + 0x38),
                            Psost(palo) ? Psost(palo)->m : -1.0f);
            if (q->xpk == 0xE)
            {
                chk.F("xp row limits", q->gA0, out.G(aq + 0xA0));
                chk.F("xp row limits", q->gA4, out.G(aq + 0xA4));
            }
            if (fn == "204250") CompareXpList(chk, q->pxpNext, out, out.U32(aq + 0xAC), mpsoeAddr, mpelAddr, "xp after", &cbadList);
            mpcid[1000 + q->xpk]++;
        }
        else if (fn == "204108")
        {
            UpdateStepPhysMotion(palo);
            for (int i = 0; i < 2; i++)
            {
                XF* x = palo->apxfRef[i];
                const uint32_t ax = in.U32(p + 0x58 + 4 * i);
                if (!x || !ax) continue;
                chk.V("record velocity", glm::vec3(x->vel), out.V3(ax + 0xB0), 1e-3);
                chk.V("record acceleration", glm::vec3(x->aseg[3]), out.V3(ax + 0xF0), 1e-3);
                if (std::getenv("PHYS_VERBOSE"))
                    std::printf("call %d oid %d cid 0x%X mode %d rec %d: accel %g %g %g ref %g %g %g (in %g %g %g), v %g %g %g\n", k,
                                palo->oid, palo->Cid(), s.nMotion, i, x->aseg[3].x, x->aseg[3].y, x->aseg[3].z, out.G(ax + 0xF0),
                                out.G(ax + 0xF4), out.G(ax + 0xF8), in.G(ax + 0xF0), in.G(ax + 0xF4), in.G(ax + 0xF8), x->vel.x, x->vel.y,
                                x->vel.z);
                chk.I("record mode", x->mode, out.S16(ax + 0x98));
            }
            mpcid[2000 + s.nMotion]++;
        }
    }
    std::printf("0x%s: %d calls of stepping objects, %d contact lists of another length; classes (1000+: xp kinds, 2000+: modes):", fn.c_str(),
                cdone, cbadList);
    for (auto& [c, n] : mpcid) std::printf(" 0x%X:%d", c, n);
    std::printf("\n");
    chk.Report();
    return 0;
}

}

int PhysCheckMain(const std::vector<std::string>& args)
{
    RegisterNativeFn(0x1CE170, reinterpret_cast<void*>(&ShimRestoreAttachment), "shim 0x1CE170");
    RegisterNativeFn(0x1CE6C0, reinterpret_cast<void*>(&ShimStateNotOne), "shim 0x1CE6C0");
    if (args.size() < 2)
    {
        std::fprintf(stderr, "usage: Sly3-Release --phys-check FUNC PREFIX [COUNT]  (FUNC: 22DE88)\n");
        return 2;
    }
    InitClasses();
    g_pfnGrfSlyXpImported = GrfSlyXpFromRam;
    // PHYS_RTZ=1: the native code runs with the PS2's rounding (toward zero), to tell rounding differences apart
    if (std::getenv("PHYS_RTZ")) std::fesetround(FE_TOWARDZERO);
    const std::string& fn = args[0];
    const std::string& prefix = args[1];
    const int c = args.size() > 2 ? std::atoi(args[2].c_str()) : 1;
    if (fn == "22DE88") return CheckUpdateXfs(prefix, c);
    if (fn == "1F3980") return CheckUpdateAloPhys(prefix, c);
    if (fn == "134040") return CheckUpdateSwXps(prefix, c);
    if (fn == "1AA1D8") return CheckMarkOxaContacts(prefix, c);
    if (fn == "1A8B50" || fn == "1A9D38" || fn == "1A9B28") return CheckMarker(fn, prefix, c);
    if (fn == "1F9410" || fn == "1F8F80" || fn == "1F8DD8") return CheckSolve(fn, prefix, c);
    if (fn == "frames") return CheckFrames(prefix, c);
    if (fn == "204768" || fn == "204AC8" || fn == "2044B0" || fn == "204250" || fn == "204108") return CheckStep(fn, prefix, c);
    if (fn == "14CC58" || fn == "11AD30" || fn == "1F59A0" || fn == "21CB18" || fn == "1A93C8" ||
        fn == "14EA08" || fn == "14F110" || fn == "223A70" || fn == "230710" || fn == "230A98" || fn == "228C18" || fn == "2309E8")
        return CheckMisc(fn, prefix, c);
    std::fprintf(stderr, "unknown check %s\n", fn.c_str());
    return 2;
}
