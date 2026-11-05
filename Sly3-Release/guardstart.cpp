// The guards' start natively (class 0x38..): the InitLo part that pairs a guard with its detector (0x205EB8: the
// 0x10 object oid 648 it makes, +0xF64), the post-load 0x2061D8 (vt slot 15: the loaded options into GUARD, the kind
// table and the three arrays resolved, the +0xEB8 objects, the home command list), and the reset 0x206518 (vt slot
// 26: on the proxy's attach (activate.cpp AttachProxy) or, for a guard in the world at the start, the deferred
// 0x206170). The physics parts of 0x2061D8 (the size 0x204DA8, the records' modes) are physworld.cpp's
// PostLoadSwPhys.

#include "guard.h"
#include "update.h"
#include "pocket.h"

#include "ai.h"
#include "aijobs.h"
#include "aipursuit.h"
#include "animfind.h"
#include "asega.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "detect.h"
#include "activate.h"
#include "sw.h"
#include "vtnative.h"
#include "objmethod.h"
#include "xf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace
{
std::unordered_map<const LO*, LO*>& MpDetector()
{
    static std::unordered_map<const LO*, LO*> s_map;
    return s_map;
}

uint64_t MaskOfList(const OPTVAL& v)  // orig 0x207B10 / 0x207A40 (up to 16 indices)
{
    uint64_t m = 0;
    int c = 0;
    for (const OPTVAL& w : v.alist)
        if (c++ < 16) m |= GrfStateBit(w.n);
    if (v.alist.empty() && v.n) m |= GrfStateBit(v.n);
    return m;
}

// orig 0x209C18: the table's entry of a mask (added when new), reset (0x2090D0)
KINDE* PkindeOfMask(GUARD* pg, uint64_t mask)
{
    KINDE* pe = nullptr;
    for (KINDE& e : pg->akinde)
        if (e.mask == mask) pe = &e;
    if (!pe)
    {
        if (pg->akinde.size() >= 32) return nullptr;
        pg->akinde.emplace_back();
        pe = &pg->akinde.back();
        pe->mask = mask;
    }
    pe->k = -1;
    pe->oid = -1;
    pe->plo = nullptr;
    pe->pk2 = nullptr;
    pe->pk3 = nullptr;
    return pe;
}

// orig: 0x00207B70 / 0x00207BC8 (the latter resolves the replaced entry immediately).
// 0x2090D0 resets the entry's kind payload; it does not retract an ASEGA/SM playing through that entry.
void SetGuardKinde(GUARD* pg, uint64_t mask, OID oid, int grf, bool fResolve)
{
    KINDE* pe = PkindeOfMask(pg, mask);
    if (!pe) return;
    pe->oid = oid;
    pe->grf = grf;
    if (fResolve) ResolveKinde(pe, pg);
}

// orig 0x207EB8: the array entries of the mask's indices
void SetArrayKindes(std::array<KINDE, 26>& a, uint32_t mask, OID oid, int grf)
{
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i))
        {
            KINDE& e = a[i];
            e = KINDE{};
            e.grf = 0;
            e.oid = oid;
            e.grf = grf;
        }
}

LO* PloNear(LO* plo, OID oid) { return oid == -1 || oid == 0 ? nullptr : PloFindSwNearest(plo, oid); }
float GField(const LO* plo, uint32_t ib, float g)
{
    uint32_t u;
    std::memcpy(&u, &g, 4);
    u = plo->U32Field(0, ib, u);
    float r;
    std::memcpy(&r, &u, 4);
    return r;
}
int NField(const LO* plo, uint32_t ib, int n) { return static_cast<int>(plo->U32Field(0, ib, static_cast<uint32_t>(n))); }
}  // namespace

// brx.cpp (the guards' InitLo 0x205EB8): the detector object made for the guard
void OnGuardInitLo(LO* pguard, LO* ploDetector) { MpDetector()[pguard] = ploDetector; }

// orig: 0x001AE300 FreeSw (called by 0x20E068): detector storage belongs to the world arena.
// Its next allocation (0x110E18 -> 0x1AE4F8 -> 0x1AE2A0) starts cleared even at the same
// address. Release the native side tables at the same world boundary, after the guard jobs.
// Same-world ResetGuard/proxy reuse keeps its detector entries, as in 0x206518.
void ResetGuardDetectors()
{
    MpDetector().clear();
    ResetDetectors();
}

GUARD* g_pguard3F = nullptr;

// orig: 0x00207C48 / 0x00207CF8 (the latter resolves the four directions immediately).
void SetGuardKind3(GUARD* pg, uint64_t mask, const std::array<OID, 4>& aoid, bool fResolve)
{
    for (KINDE& e : pg->akinde)
        if (e.mask == mask) StopKinde(&e);  // original 0x2090D0, before replacing the entry
    KINDE* pe = PkindeOfMask(pg, mask);
    if (!pe) return;
    auto pk = std::make_unique<KIND3>();  // original 0x20CA00 / 0x20C7D8
    for (size_t i = 0; i < aoid.size(); ++i) pk->ae[i].oid = aoid[i];
    pe->k = 3;
    pe->pk3 = pk.get();
    pe->grf = 8;
    pg->akind3.push_back(std::move(pk));
    if (fResolve) ResolveKinde(pe, pg);
}

// orig: 0x0020A540 SetGuardPatrol
void SetGuardPatrol(GUARD* pg, CMDLIST* pcmdlist)
{
    if (pg->pcmdlistPatrol == pcmdlist) return;
    pg->pcmdlistPatrol = pcmdlist;
    pg->saved2DC = SAVEDSCRIPT{};
    auto* pj = static_cast<JOBREACT*>(PjobFind(pg, 0x9C));
    if (pj && pj->msg.k == 1)
    {
        AIMSG msg;
        msg.k = -1;
        pj->React(msg);
        pg->saved2DC = SAVEDSCRIPT{};
        msg.k = 1;
        pj->React(msg);
    }
}

// A guard field option (grf 0x92: stored straight into the guard by 0x13E818) loaded into the object's field store
// copied to the native guard: at the post-load, and for the binds a proxy's instance loads when attached (0x1CAEC8),
// e.g. a spawned guard's run / stand ranges (12:2195's 95176: 5..15 / 3..4)
void SyncGuardOption(GUARD* pg, const LO* plo, int eopid)
{
    switch (eopid)
    {
    case 274:  // the run time's range (kind 10, 0x179E80)
        pg->tWaitA0 = GField(plo, 0x2CC, pg->tWaitA0);
        pg->tWaitA1 = GField(plo, 0x2D0, pg->tWaitA1);
        break;
    case 275:  // the stand time's range (0x179EF8)
        pg->tWaitB0 = GField(plo, 0x2D4, pg->tWaitB0);
        pg->tWaitB1 = GField(plo, 0x2D8, pg->tWaitB1);
        break;
    default: break;
    }
}

// orig: 0x002061D8 (vt slot 15 of the guard classes)
void PostLoadGuard(LO* plo)
{
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) return;
    ALO* palo = static_cast<ALO*>(plo);
    GUARD* pg = PguardFromLo(palo);
    if (!pg) pg = PguardNew(palo);
    // the classes' InitLo after the base 0x205EB8 (vt slot 7): 0x39's 0x16F148 (+0xE78 = 1)
    if (plo->pcls && plo->pcls->PfnOrig(7) == 0x16F148) pg->nE78 = 1;
    // class 0x3F's 0x14B1A0: the world's one (0x2DD648) and +0x4A4 = 1; its post-load 0x14B1D8 (after 0x2061D8):
    // +0x1010 a new object of class 3 (oid 0x62B), the chase job's aim point (aichase.cpp 0x14A4D0)
    const bool f3F = plo->pcls && plo->pcls->PfnOrig(7) == 0x14B1A0;
    if (f3F)
    {
        g_pguard3F = pg;
        pg->f4A4 = 1;
    }
    // class 0x3B's 0x1553F0 (the boss): the base, the records' table, +0x1018 = 0x7FFFFFFF; its post-load 0x155450
    // (after 0x2061D8): each record's four objects found (0x15E938 kind 4) — done at the end of this function
    const bool f3B = FCidDerivedFrom(plo->Cid(), 0x3B);
    if (f3B) pg->n1018 = 0x7FFFFFFF;
    // class 0x40's 0x14FAC8 (the Ep4 Contessa): the base, then +0x4A4 = 1
    if (plo->pcls && plo->pcls->PfnOrig(7) == 0x14FAC8) pg->f4A4 = 1;
    // TODO(ai): 0x3B (0x1553F0 / 0x155450: its +0x1014 records), 0x3C (0x1B9F80 / 0x1BA018: the carried kind),
    // 0x3D, 0x3F (their own InitLo / post-load / update)
    // the loaded fields (EOPID grf 0x92) and options
    pg->n338 = NField(plo, 0x338, pg->n338);        // 254
    pg->n334 = NField(plo, 0x334, pg->n334);        // 332
    pg->n340 = NField(plo, 0x340, pg->n340);
    pg->f298 = NField(plo, 0x298, pg->f298);        // 231 / 1350: busy        // 256 / 257 / 1383: the hits a block takes (the boss: 40)
    pg->g314 = GField(plo, 0x314, pg->g314);        // 292
    pg->g318 = GField(plo, 0x318, pg->g318);        // 293
    pg->gF30 = GField(plo, 0xF30, pg->gF30);        // 320
    pg->gF34 = GField(plo, 0xF34, pg->gF34);        // 321
    pg->gF48 = GField(plo, 0xF48, pg->gF48);        // 298
    SyncGuardOption(pg, plo, 274);
    SyncGuardOption(pg, plo, 275);
    pg->ploE74 = PloNear(plo, NField(plo, 0xE74, -1));  // 327
    pg->ploFEC = PloNear(plo, NField(plo, 0xFEC, -1));  // 331
    pg->plo3F4 = PloNear(plo, NField(plo, 0x3F4, -1));  // 304
    pg->plo3F8 = PloNear(plo, NField(plo, 0x3F8, -1));  // 305
    pg->plo494 = PloNear(plo, NField(plo, 0x494, -1));  // 306
    // EOPID273's kind0x2071 field is resolved by the original loader's deferred 0x13E590 fixup,
    // before reset/start. Native loaded pointer fields keep OIDs until this post-load pass.
    pg->pcmdlistPatrol = dynamic_cast<CMDLIST*>(PloNear(plo, NField(plo, 0x2E8, -1)));
    StopK3(&pg->k3play);
    pg->akinde.clear();
    pg->akinde.reserve(32);  // the original's fixed table keeps active entries' addresses stable
    pg->akind3.clear();
    for (auto& a : pg->aakinde)
        for (KINDE& e : a)
        {
            e = KINDE{};
            e.grf = 0;  // (the guard's memory starts zeroed)
        }
    pg->aoidEB8.clear();
    OID oidVolume = -1, oidVolume2 = -1;
    for (const OPT& o : plo->aopt)
    {
        const auto& v = o.aval;
        // (debug: SLY2N_GUARDOPTS=oid: that guard's options at its post-load, eopid and values)
        if (static const char* s_sz = std::getenv("SLY2N_GUARDOPTS"); s_sz && std::atoi(s_sz) == plo->oid)
        {
            std::printf("guardopt %d eopid %d:", plo->oid, o.eopid);
            for (const OPTVAL& w : v)
            {
                std::printf(" [n %d g %g", w.n, w.ag[0]);
                for (const OPTVAL& x : w.alist) std::printf(" %d", x.n);
                std::printf("]");
            }
            std::printf("\n");
        }
        switch (o.eopid)
        {
        case 243:  // 0x207B70: {indices, oid, grf}
            if (v.size() >= 3)
                SetGuardKinde(pg, MaskOfList(v[0]), v[1].n, v[2].n, false);
            break;
        case 244:  // 0x207C48: a kind-3 entry {indices, four oids}
            if (v.size() >= 5)
                SetGuardKind3(pg, MaskOfList(v[0]), {v[1].n, v[2].n, v[3].n, v[4].n}, false);
            break;
        case 245:  // 0x207D98: a kind-2 entry {indices, oid}
            if (v.size() >= 2)
                if (KINDE* pe = PkindeOfMask(pg, MaskOfList(v[0])))
                {
                    pe->k = 2;
                    pe->grf = 0;
                    pe->oid = v[1].n;  // TODO(ai): the KIND2 object (0x20C010 / 0x20BD78)
                }
            break;
        case 247: if (v.size() >= 3) SetArrayKindes(pg->aakinde[0], static_cast<uint32_t>(v[0].n), v[1].n, v[2].n); break;  // +0x8D0
        case 246: if (v.size() >= 3) SetArrayKindes(pg->aakinde[1], static_cast<uint32_t>(v[0].n), v[1].n, v[2].n); break;  // +0xA70
        case 248: if (v.size() >= 3) SetArrayKindes(pg->aakinde[2], static_cast<uint32_t>(v[0].n), v[1].n, v[2].n); break;  // +0xC10
        case 233:  // (also kept by the loader's call of the method 0x102988, launcher.cpp: once per object)
            if (!v.empty() && pg->aoidEB8.size() < 8 && std::find(pg->aoidEB8.begin(), pg->aoidEB8.end(), v[0].n) == pg->aoidEB8.end())
                pg->aoidEB8.push_back(v[0].n);
            break;
        case 315: if (!v.empty()) oidVolume = v[0].n; break;
        case 316: if (!v.empty()) oidVolume2 = v[0].n; break;
        default: break;
        }
    }
    // the detector (+0xF64)
    auto it = MpDetector().find(plo);
    if (it != MpDetector().end())
    {
        DETECTOR* pdet = PdetectorFromLo(it->second);
        if (!pdet) pdet = PdetectorNew(it->second, palo);
        pdet->ploOwner = nullptr;  // (+0xE0: set by the reset)
        pdet->ploVolume = PloNear(plo, oidVolume);
        pdet->ploVolume2 = PloNear(plo, oidVolume2);
        pdet->grf = it->second->U32Field(0x206378, 0xF8, plo->U32Field(0x206378, 0xF8, 0));  // 312
        pdet->fAlways = int(it->second->U32Field(0x206378, 0xF0, plo->U32Field(0x206378, 0xF0, uint32_t(pdet->fAlways))));  // 313
        pdet->fNever = int(it->second->U32Field(0x206378, 0xF4, plo->U32Field(0x206378, 0xF4, uint32_t(pdet->fNever))));     // 314
        pg->pdet = pdet;
    }
    if (f3B)
    {
        pg->c1010 = int(pg->abossrec.size());
        for (GUARD::BOSSREC& r : pg->abossrec)
            for (int i = 0; i < 4; ++i) r.aplo[i] = PloNear(plo, r.aoid[i]);
        if (std::getenv("SLY2N_BOSSPRINT")) std::printf("boss %d: post-load %d records\n", plo->oid, pg->c1010);
    }
    // orig 0x2080F0: the entries resolved (the table, then +0xC10, +0x8D0, +0xA70)
    for (KINDE& e : pg->akinde) ResolveKinde(&e, pg);
    for (int j : {2, 0, 1})
        for (KINDE& e : pg->aakinde[j])
            if (e.oid != -1) ResolveKinde(&e, pg);
    if (static const char* s_sz = std::getenv("SLY2N_GUARDOPTS"); s_sz && std::atoi(s_sz) == plo->oid)
        for (const KINDE& e : pg->akinde)
            std::printf("guardkinde %d: k %d oid %d plo %d pk2 %p grf %d mask 0x%llX\n", plo->oid, e.k, e.oid, e.plo ? e.plo->oid : -1, (void*)e.pk2, e.grf,
                        static_cast<unsigned long long>(e.mask));
    // class 0x3C (InitLo 0x1B9F80, post-load 0x1BA018): its options 971..974 (the alarm's animation +0x1060 (an ASEG,
    // started by a move: grf 1), the follow speed curve +0x1070 and clamp +0x1080, the wait +0x1088); the detector's
    // +0xF8 3, +0x4A4 1 (TODO(ai): 0x1A64E8 / 0x1300B8: the head's aim offset +0x1040 and its update 0x1BA128)
    if (plo->pcls && plo->pcls->PfnOrig(7) == 0x1B9F80)
    {
        pg->f4A4 = 1;
        pg->kinde1060 = KINDE{};
        pg->kinde1060.k = 0;
        pg->kinde1060.oid = NField(plo, 0x1064, -1);
        pg->kinde1060.grf = 1;
        if (pg->kinde1060.oid != -1) ResolveKinde(&pg->kinde1060, pg);
        else pg->kinde1060.k = -1;
        pg->g1070 = GField(plo, 0x1070, pg->g1070);
        pg->g1074 = GField(plo, 0x1074, pg->g1074);
        pg->g1078 = GField(plo, 0x1078, pg->g1078);
        pg->g1080 = GField(plo, 0x1080, pg->g1080);
        pg->g1084 = GField(plo, 0x1084, pg->g1084);
        pg->g1088 = GField(plo, 0x1088, pg->g1088);
    }
    // orig 0x20AAA8
    pg->aploEDC.clear();
    for (OID oid : pg->aoidEB8)
        if (LO* p = PloFindSwNearest(plo, oid)) pg->aploEDC.push_back(p);
    if (std::getenv("SLY2N_SELDEBUG"))
    {
        std::fprintf(stderr, "actions: guard %d:", plo->oid);
        for (OID oid : pg->aoidEB8) std::fprintf(stderr, " %d", oid);
        std::fprintf(stderr, " -> %zu\n", pg->aploEDC.size());
    }
    // the watchers (0x20CE28 / 0x20D1B0)
    pg->watchA = WATCHA{pg, -1, 0.0f, 1};
    pg->watchB = WATCHB{pg, -1, 0.0f, nullptr, 1};
    // the home's command list: a go-to {oid -1, n10 -1, g14 40} (0x13E3C8(0x71, 0, 0x2CB, -1), 0x170C90), out of the world
    auto* pl = static_cast<CMDLIST*>(PloNew(0x71, g_psw, nullptr, 0x2CB, -1));
    if (pl)
    {
        CMD* pc = PcmdAppend(pl, 7, -1);
        pc->n10 = -1;
        pc->g14 = 40.0f;
        pg->pcmdlistHome = pl;
        pg->pcmdHome = pc;
        RemoveLo(g_psw, pl);
    }
    if (pg->plo3F4) RemoveLo(g_psw, pg->plo3F4);
    if (pg->plo3F8) RemoveLo(g_psw, pg->plo3F8);
    // orig: 0x002061D8 -> 0x1F4C58(guard, guard->oid, 12) / 0x12DC18.
    // XF/controllers are owned by the ALO, as with its other controller handles.
    for (const auto& pxf : pg->palo->axf)
        if (pxf->kind == 12 && pxf->tag == pg->palo->oid)
        {
            pg->pscaleFC4 = PctrlNewPos(pg->palo, pxf.get(), 0, 7, 2, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
            break;
        }
    // TODO(ai): 0x1A64E8(p, 1, 0x2C3CF8); the KIND2 / KIND3 objects
    // orig: 0x0014B1D8 (class 0x3F, after 0x2061D8): +0x1010 = 0x13E3C8(3, 0, 0x62B, -1)
    if (f3F) pg->plo1010 = PloNew(0x03, g_psw, nullptr, 0x62B, -1);
}

// orig: 0x00206518 (vt slot 26 of the guard classes; the base 0x1CB518's mask part is the activation's)
void ResetGuard(LO* plo, LO* pproxy)
{
    (void)pproxy;
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return;
    if (std::getenv("SLY2N_HITDEBUG")) std::printf("reset: t %.3f guard %d %p\n", g_clock.t, plo->oid, (void*)plo);
    pg->n344 = pg->n340;
    pg->nE88 = pg->nE84;
    pg->n33C = pg->n338;
    ClearJobs(pg);
    pg->move = MOVEREF{nullptr, 0, 0, pg};  // 0x209778
    SetGuardStateMask(pg, 0);               // 0x209870
    pg->watchA.st = 0;                      // 0x20CE40 (-1, on, then state 0 at the time)
    pg->watchA.fOn = 1;
    pg->watchA.t = g_clock.t;
    pg->watchB.st = 0;                      // 0x20D1C8
    pg->watchB.fOn = 1;
    pg->watchB.t = g_clock.t;
    pg->watchA.fOn = 1;                     // 0x20D1A0
    pg->posHome = glm::vec4(PosAi(plo), 1.0f);  // 0x1A62E8
    pg->radHome = RadGuardHeading(pg);           // 0x204740
    if (pg->pcmdHome)
    {
        pg->pcmdHome->ppos8 = &pg->posHome;
        pg->pcmdHome->gC = pg->radHome;
    }
    SetGuardHeading(pg, pg->radHome);
    pg->grfFE8 = 0;
    if (PkindeBest(pg, GrfStateBit(9))) pg->grfFE8 |= 2;
    else pg->nE78 = 2;
    if (PkindeBest(pg, GrfStateBit(8))) pg->grfFE8 |= 4;
    if (PkindeBest(pg, GrfStateBit(0xF))) pg->grfFE8 |= 1;
    if (PkindeBest(pg, GrfStateBit(0x11))) pg->grfFE8 |= 8;
    if (static const bool s_f = std::getenv("SLY2N_SHOOTDEBUG") != nullptr; s_f)
        std::fprintf(stderr, "guard start %d cid 0x%X FE8 %x\n", plo->oid, plo->Cid(), pg->grfFE8);
    // the players watched (0x153EF8 with SW's Sly / Bentley / Murray (0x2E1E40 / 0x2F7900 / 0x2DD5BC) and 0x2DD648)
    if (pg->pdet && g_psw)
    {
        for (CID cid : {0x35, 0x36, 0x37})
            for (LO* p : g_psw->mpcidaplo[cid])
                if (!p->ploCloneOf)
                {
                    AddDetectorTarget(pg->pdet, p);
                    break;
                }
        AddDetectorTarget(pg->pdet, nullptr);  // (0x2DD648: none in the worlds checked)
    }
    // +0x494: the pocket filled (0x1F1A40) and its first prize readied (0x1F1CF8): pocket.cpp (the actors code)
    if (PloPocket(plo))
    {
        FillPocket(plo);
        NextPocketPrize(plo);
    }
    if (pg->ploFEC) RemoveLo(g_psw, pg->ploFEC);
    PjobPush(pg, 0x9C)->Enter();
    if (pg->pdet)
    {
        // (0x1A58D0(det, p, 0xF): the detector's parent (vt slot 10, TODO(ai)))
        pg->pdet->ploOwner = plo;  // +0xE0
        // (+0xE4 = the guard's +0xEC: TODO(ai))
    }
}

namespace
{
void ResetGuardSlot26(LO* plo, LO* pproxy) { ResetGuard(plo, pproxy); }

// orig: 0x00206748, the target/effect cleanup in the guard's placement-end
// notification. DetachProxy subsequently removes the object and its physics.
// The remaining original movement/patrol/watch reset fields are still unported.
void DetachGuardSlot27(LO* plo, LO*)
{
    GUARD* pg = PguardFromLo(plo);
    if (!pg) return;
    if (pg->pdet) { pg->pdet->fAlways = 0; pg->pdet->fNever = 0; }
    pg->ploTarget = nullptr;
    SetGuardPursued(pg, nullptr); // 0x208540
    // orig: 0x2088E0, then 0x2089E8 / 0x208B30.
    if (pg->pdet) pg->pdet->ae.clear();
    pg->acid398.clear();
    pg->c3A4 = 0;
    pg->acid3AC.clear();
    pg->acid3C4.clear();
    // The original resets its pocket before leaving the mode.
    SetPocketMode(plo, -1);
    pg->f330 = 0;  // 0x2067E0: the one-time knockout notification (0x179898) re-armed for the instance's next use
    SetGuardMode(pg, -1);
}
struct INITKINDMETHODS
{
    INITKINDMETHODS()
    {
        RegisterObjectSetter(273, [](LO* plo, const CRef& r) {
            if (GUARD* pg = PguardFromLo(plo))
            {
                LO* plist = r.m_pbasic ? dynamic_cast<LO*>(r.m_pbasic) :
                    r.m_tagk == TAGK_S32 ? PloNear(plo, r.m_n) : nullptr;
                SetGuardPatrol(pg, dynamic_cast<CMDLIST*>(plist));
            }
        });
        // orig: 0x102A88 -> 0x207B70 (loader), 0x102B60 -> 0x207BC8 (live splice method).
        static auto setKinde = [](LO* plo, const std::vector<CRef>& a, bool fResolve) {
            if (GUARD* pg = PguardFromLo(plo); pg && a.size() >= 3)
            {
                uint64_t mask = 0;
                const auto an = a[0].m_tagk == TAGK_None ? std::vector<int>{} : AnArgList(a, 0);
                for (size_t i = 0; i < an.size() && i < 16; ++i) mask |= GrfStateBit(an[i]);
                SetGuardKinde(pg, mask, NArg(a, 1), NArg(a, 2), fResolve);
            }
            return CRef(TAGK_Void);
        };
        // orig: 0x0010F6E8 -> 0x00155530 (EOPID 1340, class 0x3B's loader method): a boss escape record of four oids
        RegisterObjectMethod(0x10F6E8, [](LO* p, const std::vector<CRef>& a) {
            if (GUARD* pg = PguardFromLo(p); pg || (p && FCidDerivedFrom(p->Cid(), 0x30)))
            {
                if (!pg) pg = PguardNew(static_cast<ALO*>(p));
                GUARD::BOSSREC r;
                for (int i = 0; i < 4; ++i) r.aoid[i] = NArg(a, size_t(i), -1);
                pg->abossrec.push_back(r);
                pg->c1010 = int(pg->abossrec.size());
                if (std::getenv("SLY2N_BOSSPRINT")) std::printf("boss %d: record %d %d %d %d\n", p->oid, r.aoid[0], r.aoid[1], r.aoid[2], r.aoid[3]);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x102A88, [](LO* p, const std::vector<CRef>& a) { return setKinde(p, a, false); });
        RegisterObjectMethod(0x102B60, [](LO* p, const std::vector<CRef>& a) { return setKinde(p, a, true); });
        // orig: 0x102C38 -> 0x207C48 (loader), 0x102D30 -> 0x207CF8 (live splice method).
        static auto set = [](LO* plo, const std::vector<CRef>& a, bool fResolve) {
            if (GUARD* pg = PguardFromLo(plo); pg && a.size() >= 5)
            {
                uint64_t mask = 0;
                const auto an = AnArgList(a, 0);
                for (size_t i = 0; i < an.size() && i < 16; ++i) mask |= GrfStateBit(an[i]);
                SetGuardKind3(pg, mask, {NArg(a, 1), NArg(a, 2), NArg(a, 3), NArg(a, 4)}, fResolve);
            }
            return CRef(TAGK_Void);
        };
        RegisterObjectMethod(0x102C38, [](LO* p, const std::vector<CRef>& a) { return set(p, a, false); });
        RegisterObjectMethod(0x102D30, [](LO* p, const std::vector<CRef>& a) { return set(p, a, true); });
        // EOPIDs 231 / 1350 (grf 0x92, field +0x298): the guard busy (0x207598: hits not offered to its jobs), e.g.
        // Dimitri's closing cutscene (5:1931) sets it so Sly's scripted swing does not restart the fight
        for (int eopid : {231, 1350})
        {
            RegisterObjectSetter(eopid, [](LO* p, const CRef& r) {
                if (GUARD* pg = PguardFromLo(p)) pg->f298 = r.m_tagk == TAGK_Bool ? r.m_bool : r.RefCoerceS32().m_n;
            });
            RegisterObjectGetter(eopid, [](LO* p) {
                CRef r;
                const GUARD* pg = PguardFromLo(p);
                r.SetBool(pg && pg->f298 ? 1 : 0);
                return r;
            });
        }
        // EOPID 255 (grf 0x92, field +0x33C, e.g. the guards' option 381): the hit points left. Ep7's boss module 165
        // (task 3274) counts Bison's trap hits by it (s2128: a hit by someone other than Bentley that lowered it), so
        // without it the fight never reached 3282..3285
        RegisterObjectSetter(255, [](LO* p, const CRef& r) {
            if (GUARD* pg = PguardFromLo(p)) pg->n33C = r.RefCoerceS32().m_n;
        });
        RegisterObjectGetter(255, [](LO* p) {
            CRef r;
            const GUARD* pg = PguardFromLo(p);
            r.SetS32(pg ? pg->n33C : 0);
            return r;
        });
        // orig: 0x00103AB8 (EOPID 279, e.g. the guards' option 405): is state n in the guard's mask (0x209A68), a bool
        RegisterObjectMethod(0x103AB8, [](LO* p, const std::vector<CRef>& a) {
            const GUARD* pg = PguardFromLo(p);
            CRef ref;
            ref.SetBool(pg && FGuardState(pg, NArg(a, 0)) ? 1 : 0);
            if (std::getenv("SLY2N_BOSSPRINT"))
                std::printf("guard state? %d n %d (%zu args) mask 0x%llX -> %d\n", p ? p->oid : -1, NArg(a, 0), a.size(),
                            pg ? (unsigned long long)pg->grfState : 0ull, ref.m_bool);
            return ref;
        });
    }
} s_initkindmethods;
}
NATIVEFN(0x206518, ResetGuardSlot26);
NATIVEFN(0x206748, DetachGuardSlot27);

// The guards at the start: 0x2061D8 queues 0x206170 (slot 26 (0), then their splice message 0xE) unless the guard's
// group record (+0x3C) has its +0x24 set (a group that proxies instantiate, WE +0x60: those copies get slot 26 when
// placed, activate.cpp); natively run at the end of the load (the original: the first frame's 0x149C80)
void ResetGuardsAtStart(SW* psw)
{
    for (const auto& plo : psw->aploAll)
    {
        if (!plo->pcls || plo->pcls->PfnOrig(26) != 0x206518 || !PguardFromLo(plo.get())) continue;
        const WE* pwe = plo->igrp >= 0 && plo->igrp < static_cast<int>(psw->awe.size()) ? &psw->awe[plo->igrp] : nullptr;
        if (std::getenv("SLY2N_GUARDSTART"))
            std::fprintf(stderr, "guard start %d cid 0x%X: inworld %d removed %d clone %d group %d (proxied %d spare %d)\n", plo->oid,
                         plo->Cid(), plo->fInWorld, plo->fRemoved, plo->ploCloneOf != nullptr, plo->igrp, pwe ? pwe->x60 : -1, plo->fSpareNs);
        // (0x2061D8: not for an object of a spare instance (its namespace node's +0x24, set by 0x1CA500), e.g. Ep6's
        // Carmelita 12712, whose pocket would be filled with coins before her job's keys come)
        if ((pwe && pwe->x60) || plo->fSpareNs) continue;
        ResetGuard(plo.get(), nullptr);
        HandleLoSpliceEvent(plo.get(), 14, false, 0, nullptr);  // (0x206170's splice message 0xE)
    }
}
