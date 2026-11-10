#include "fastpaths.h"
#include <execinfo.h>
#include "simdmat.h"
#include "clock.h"
// ALO loading: transforms (0x22D7A0 and the blocks it reads), collision entries, children (0x1F31E0).

#include "activate.h"
#include "alo.h"
#include "laser.h"
#include "trail.h"

#include "bis.h"
#include "brx.h"
#include "readtrace.h"
#include "glob.h"
#include "sw.h"
#include "so.h"
#include "xfphys.h"
#include "objmethod.h"
#include "vtnative.h"

#include <atomic>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)

namespace
{
std::vector<XF*> s_apxfBase;  // orig 0x4437A0/0x4437A8: the base stack of reference kind 0x2000
}

// orig 0x0022D670 / 0x0022D698: push / pop a base for references of kind 0x2000
void PushXfBase(XF* pxf) { s_apxfBase.push_back(pxf); }
void PopXfBase()
{
    if (!s_apxfBase.empty()) s_apxfBase.pop_back();
}

// orig 0x0022D6B0: a transform reference: bits 12-13 select the records: 0 = the object's own (if any),
// 0x1000 = the parent's (the index without bits 12-15), 0x2000 = the top of the base stack, 0x3000 = none.
XF* PxfFromRef(int ref, ALO* paloParent, ALO* paloSelf)
{
    const int sel = ref & 0x3000;
    auto at = [](ALO* palo, int i) -> XF* {
        if (!palo || i < 0 || i >= static_cast<int>(palo->axf.size())) return nullptr;
        return palo->axf[i].get();
    };
    if (sel == 0x1000) return at(paloParent, ref & static_cast<int>(0xFFFF0FFF));
    if (sel == 0) return at(paloSelf, ref);
    if (sel == 0x2000) return s_apxfBase.empty() ? nullptr : s_apxfBase.back();
    return nullptr;
}

// orig 0x0022D778 (ALO vt slot 51): own records or the parent's
XF* PxfFindAlo(ALO* palo, int ref) { return PxfFromRef(ref, palo->paloParent, palo); }
// orig 0x0022D750 (LOX vt slot 49): the parent's records only
XF* PxfFindLox(LO* plo, int ref) { return PxfFromRef(ref, plo->paloParent, nullptr); }

// orig 0x00186670
void LoadAloPairsFromBrx(ALO* palo, CBinaryInputStream* pbis)
{
    LOADFN(0x186670);
    const int c = pbis->U8Read();
    if (!c) return;
    palo->grf |= 1ull << 36;
    for (int i = 0; i < c; ++i)
    {
        ALOPAIR pair;
        pair.apxf[1] = PxfFindAlo(palo, pbis->S16Read());
        pair.apxf[2] = PxfFindAlo(palo, pbis->S16Read());
        pair.apxf[3] = PxfFindAlo(palo, pbis->S16Read());
        pair.vecB0 = pbis->ReadVector();
        pair.apxf[0] = PxfFindAlo(palo, pbis->S16Read());
        pair.vecA0 = pbis->ReadVector();
        // TODO(M3): 0x186930, 0x12DD28 (the pair's jobs)
        palo->apair.push_back(pair);
    }
}

// orig 0x001603B8
void LoadAloLinksFromBrx(ALO* palo, CBinaryInputStream* pbis)
{
    LOADFN(0x1603B8);
    const int c = pbis->S8Read();
    for (int i = 0; i < c; ++i)
    {
        ALOLINK link;
        link.pxfTarget = PxfFindAlo(palo, pbis->S16Read());  // (null: the original's placeholder record)
        link.pxf = PxfFindAlo(palo, pbis->S16Read());
        link.n8 = pbis->S8Read();
        link.nC = pbis->S8Read();
        palo->alink.push_back(link);
    }
}

// orig 0x0021C888
void LoadAloChainsFromBrx(ALO* palo, CBinaryInputStream* pbis)
{
    LOADFN(0x21C888);
    const int c = pbis->S8Read();
    for (int i = 0; i < c; ++i)
    {
        ALOCHAIN chain;
        chain.n8 = pbis->U8Read();
        for (float& g : chain.ag) g = pbis->F32Read();
        chain.vec20 = pbis->ReadVector();
        chain.vec30 = pbis->ReadVector();
        const int ce = pbis->S8Read();
        for (int j = 0; j < ce; ++j)
        {
            ALOCHAINE e;
            e.pxf = PxfFindAlo(palo, pbis->S16Read());
            chain.ae.push_back(e);
        }
        palo->achain.push_back(std::move(chain));
    }
}

// orig 0x0022D7A0 LoadXfsFromBrx (the ALO's transform records and their jobs)
void LoadAloXfsFromBrx(ALO* palo, CBinaryInputStream* pbis)
{
    LOADFN(0x22D7A0);
    const int c = pbis->S16Read();
    palo->axf.clear();
    for (int i = 0; i < c; ++i)
    {
        palo->axf.push_back(std::make_unique<XF>());
        palo->axf.back()->palo = palo;
    }
    for (int i = 0; i < c; ++i)
    {
        XF& xf = *palo->axf[i];
        xf.pxfRef = PxfFindAlo(palo, pbis->S16Read());
        xf.kind = pbis->S16Read();
        xf.tag = pbis->S32Read();
        int grf = pbis->U8Read();
        if (grf & 1)
        {
            xf.lo = static_cast<int>(pbis->F32Read() * 1000.0f);
            xf.hi = 1000;
        }
        else if (grf & 2)
        {
            xf.lo = static_cast<int>(pbis->F32Read() * 1000.0f);
            xf.hi = static_cast<int>(pbis->F32Read() * 1000.0f);
        }
        if (grf & 4)
        {
            XFJOB j;
            j.kind = 1;
            j.id = pbis->S8Read();
            xf.ajob.push_back(j);
        }
        if (grf & 8)
        {
            XFJOB j;
            j.kind = 2;
            j.id = pbis->S8Read();
            j.g = pbis->F32Read();
            pbis->ReadCb12(j.agBegin);
            pbis->ReadCb8(j.agTimingBegin);
            pbis->ReadCb12(j.agEnd);
            pbis->ReadCb8(j.agTimingEnd);
            j.a = pbis->S8Read();
            j.b = pbis->S8Read();
            j.c = pbis->S32Read();
            xf.ajob.push_back(j);
        }
        if (grf & 16)
        {
            XFJOB j;
            j.kind = 4;
            j.id = pbis->S8Read();
            j.gLo = pbis->F32Read();
            j.gHi = pbis->F32Read();
            xf.ajob.push_back(j);
        }
        if (grf & 32)
        {
            XFJOB j;
            j.kind = 9;
            j.id = 20;
            j.vec = pbis->ReadVector();
            xf.ajob.push_back(j);
        }
        switch (xf.kind)
        {
        case 8: case 14:
        {
            xf.mat = glm::mat4(1.0f);
            xf.pxfOrigin = PxfFindAlo(palo, pbis->S16Read());
            if (xf.pxfOrigin)
            {
                // orig: the translation is minus the origin record's position (+0x30)
                xf.mat[3] = glm::vec4(-glm::vec3(xf.pxfOrigin->mat[3]), 1.0f);
            }
            break;
        }
        default:
            xf.mat = pbis->ReadMatrix4();
            break;
        }
        xf.matLoad = xf.mat;
        static uint32_t s_nXfLoad = 0;
        xf.nLoad = ++s_nXfLoad;
    }
    LoadAloPairsFromBrx(palo, pbis);
    LoadAloLinksFromBrx(palo, pbis);
    LoadAloChainsFromBrx(palo, pbis);
    for (XF*& pxf : palo->apxfRef) pxf = PxfFindAlo(palo, pbis->S16Read());
}

// orig 0x001F31E0 LoadAloFromBrx
void LoadAloFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1F31E0);
    ALO* palo = static_cast<ALO*>(plo);
    InitAloPhys(palo);  // (the physics port) the physics part of InitLo 0x1F2108
    LoadAloXfsFromBrx(palo, pbis);
    palo->uMeta = pbis->U32Read();
    palo->pglobset = std::make_shared<GLOBSET>();
    LoadGlobsetFromBrx(palo->pglobset.get(), palo, pbis);  // the renderer's (glob.cpp), orig LoadGlob(alo+0x80)
    // (vt slot 63: extra entries the class adds after the loaded ones; no reads)
    const int c = pbis->S16Read();
    palo->asoe.resize(c);
    for (int i = 0; i < c; ++i)
    {
        SOE& e = palo->asoe[i];
        e.pxf = PxfFindAlo(palo, pbis->S16Read());
        e.tag = pbis->S32Read();
        e.f48 = pbis->S8Read() & 1;
        e.f49 = pbis->S8Read() & 1;
        e.sRadius = pbis->F32Read();
        pbis->ReadGeom(&e.geomLocal);
        // orig: a null record gets the identity (0x2C44F0) and the world GEOM is the local one; else 0x166270
        // transforms it into place (TODO(M3): with the record's matrix)
        e.geomWorld = e.geomLocal;
        pbis->ReadBsp(&e.geomWorld, &e.bsp);
        e.id = pbis->U32Read();
        e.state = pbis->S16Read();
        e.pinned = pbis->S16Read();
        e.f50 = pbis->S8Read() & 1;
    }
    LoadAloPhysEntries(palo);  // (the physics port) 0x1F2B20
    if (c > 0)
    {
        palo->fHasState = true;
        palo->gMass = pbis->F32Read();
        palo->matInertia = pbis->ReadMatrix();
        LoadAloPhysState(palo);  // (the physics port) 0x1F35F8, vt slot 64
    }
    LoadOptionsFromBrx(palo->aopt, palo, pbis);
    // a glob set with globs (+0xB8) makes the object drawable (+0x40 bit 8; vt slot 21 changes it at run time)
    if (palo->pglobset && !palo->pglobset->aglob.empty()) palo->grfUpd |= 8;
    // TODO(M3): 0x1F2500
    LoadSwObjsFromBrx(g_psw, palo, pbis, nullptr);
    palo->grfLoad = palo->uMeta;
    SetLoMask(g_psw, palo, palo->uMeta);  // orig 0x1A6D28(p, uMeta)
}

// The records' world matrices (orig: the transform pass, 0x22CA18 / 0x22C9xx: world = local x the reference's
// world in the original's row-vector convention): records whose reference is none are placed by their local.
// (Each record's state in this pass is its stamp genWorld: 2g started, 2g + 1 done; no per-call allocation or
// index search. A reference inside the object is done first; records of other objects are taken as they are.)
namespace
{
// (atomic: FUpdateAloHierarchyWorldsIsolated takes stamps on several threads at once)
std::atomic<uint64_t> s_genXfWorld{0};
uint64_t NextGenXfWorld() { return s_genXfWorld.fetch_add(1, std::memory_order_relaxed) + 1; }   // (64 bits: never wraps)
void UpdateXfWorld(ALO* palo, XF* pxf, uint64_t gen)
{
    if (pxf->genWorld == 2 * gen + 1) return;
    if (pxf->genWorld == 2 * gen) { pxf->matWorld = pxf->mat; return; }  // (a cycle: not expected)
    pxf->genWorld = 2 * gen;
    XF* pxfRef = pxf->pxfRef;
    if (pxfRef && pxfRef->palo == palo) UpdateXfWorld(palo, pxfRef, gen);
    pxf->matWorld = pxfRef ? MulMat4(pxfRef->matWorld, pxf->mat) : pxf->mat;   // (simdmat.h: glm's product exactly)
    pxf->genWorld = 2 * gen + 1;
}
// UpdateXfWorld for a hierarchy updated alongside others: false, the reference not read, when it lies outside the
// hierarchy of paloRoot (another thread may be writing its world). Across hierarchies only the records' owners and the
// objects' parents are read, which nothing writes meanwhile. A record left started (2g) is redone under a new stamp.
bool FUpdateXfWorldIsolated(ALO* palo, XF* pxf, uint64_t gen, const ALO* paloRoot)
{
    if (pxf->genWorld == 2 * gen + 1) return true;
    if (pxf->genWorld == 2 * gen) { pxf->matWorld = pxf->mat; return true; }  // (a cycle: not expected)
    pxf->genWorld = 2 * gen;
    XF* pxfRef = pxf->pxfRef;
    if (pxfRef && pxfRef->palo == palo)
    {
        if (!FUpdateXfWorldIsolated(palo, pxfRef, gen, paloRoot)) return false;
    }
    else if (pxfRef)
    {
        const ALO* p = pxfRef->palo;
        while (p && p != paloRoot) p = p->paloParent;
        if (!p) return false;
    }
    pxf->matWorld = pxfRef ? MulMat4(pxfRef->matWorld, pxf->mat) : pxf->mat;
    pxf->genWorld = 2 * gen + 1;
    return true;
}
}  // namespace

void UpdateXfWorlds(ALO* palo)
{
    if (palo->axf.empty()) return;
    const uint64_t gen = NextGenXfWorld();
    for (auto& pxf : palo->axf) UpdateXfWorld(palo, pxf.get(), gen);
}

// The ALO and its children's worlds, parents first.
void UpdateAloHierarchyWorlds(ALO* palo)
{
    UpdateXfWorlds(palo);
    for (LO* pc : palo->aploChild)
        if (FCidDerivedFrom(pc->Cid(), 0x30)) UpdateAloHierarchyWorlds(static_cast<ALO*>(pc));
}

bool FUpdateAloHierarchyWorldsIsolated(ALO* palo, const ALO* paloRoot)
{
#if SLY2_FASTPATHS
    // (the Switch: the records two ahead and the children's class pointers prefetched; each record is a heap block of
    // its own)
    auto prefetchXf = [](const XF* p) {
        __builtin_prefetch(&p->genWorld);
        __builtin_prefetch(&p->mat);
        __builtin_prefetch(&p->matWorld, 1);
    };
    for (const LO* pc : palo->aploChild) __builtin_prefetch(&pc->pcls);
    const size_t cxf = palo->axf.size();
    if (cxf > 0) prefetchXf(palo->axf[0].get());
    if (cxf > 1) prefetchXf(palo->axf[1].get());
    if (cxf)
    {
        const uint64_t gen = NextGenXfWorld();
        for (size_t i = 0; i < cxf; ++i)
        {
            if (i + 2 < cxf) prefetchXf(palo->axf[i + 2].get());
            if (!FUpdateXfWorldIsolated(palo, palo->axf[i].get(), gen, paloRoot)) return false;
        }
    }
#else
    if (!palo->axf.empty())
    {
        const uint64_t gen = NextGenXfWorld();
        for (auto& pxf : palo->axf)
            if (!FUpdateXfWorldIsolated(palo, pxf.get(), gen, paloRoot)) return false;
    }
#endif
    for (LO* pc : palo->aploChild)
        if (FCidDerivedFrom(pc->Cid(), 0x30) && !FUpdateAloHierarchyWorldsIsolated(static_cast<ALO*>(pc), paloRoot)) return false;
    return true;
}

// orig 0x001F4010 (ALO vt slot 37 SetMatrix): the first record's local = m * local (the original's 0x11C168(m,
// local) transforms each of the local matrix's rows by m)
void SetAloMatrix(ALO* palo, const glm::mat4& mat)
{
    if (static const char* s_szMove = std::getenv("SLY2N_MOVEPRINT"); s_szMove && std::atoi(s_szMove) == palo->oid)
    {
        void* apv[16];
        std::fprintf(stderr, "move %d by SetAloMatrix t %.3f:\n", palo->oid, g_clock.t);
        backtrace_symbols_fd(apv, backtrace(apv, 16), 2);
    }
    if (palo->axf.empty()) return;
    XF& xf = *palo->axf[0];
    // (0x11C168(m, local): each of the local matrix's columns transformed by m, i.e. m * local: the placement is
    // applied after the object's own matrix; local * m rotated the placement's position by the object's own
    // rotation about the world origin, U18: Prague's 24046 instances 45 degrees around the city)
    xf.mat = mat * xf.mat;
}

// orig: 0x001F21B8 (the physics implementation's base move is the native world's parent lists)
void SetAloParent(ALO* palo, ALO* paloNew, XF* pxfNew, int grf)
{
    ReparentAlo(palo, paloNew, pxfNew, grf, [](ALO* p, ALO* q, XF*, int) {
        SetLoParentActivation(g_psw, p, q, true);
    });
}
NATIVEFN(0x1F21B8, SetAloParent);

// The loaded data of an ALO copied for a clone (orig vt slot 40 CloneLo): the records (references into the
// original's records go to the copy's, into the original's parent's to the copy's parent's), the mesh (shared),
// the collision entries.
void CloneAloData(const ALO* paloSrc, ALO* paloDst)
{
    auto remap = [&](XF* pxf) -> XF* {
        if (!pxf) return nullptr;
        auto find = [&](const ALO* pa, ALO* pb) -> XF* {
            if (!pa || !pb) return nullptr;
            for (size_t i = 0; i < pa->axf.size() && i < pb->axf.size(); ++i)
                if (pa->axf[i].get() == pxf) return pb->axf[i].get();
            return nullptr;
        };
        if (XF* p = find(paloSrc, paloDst)) return p;
        if (XF* p = find(paloSrc->paloParent, paloDst->paloParent)) return p;
        return pxf;  // a base record (0x2000) or the parent's own
    };
    paloDst->axf.clear();
    for (const auto& pxf : paloSrc->axf)
    {
        paloDst->axf.push_back(std::make_unique<XF>(*pxf));
        paloDst->axf.back()->palo = paloDst;
    }
    for (auto& pxf : paloDst->axf)
    {
        pxf->pxfRef = remap(pxf->pxfRef);
        pxf->pxfOrigin = remap(pxf->pxfOrigin);
    }
    for (int i = 0; i < 3; ++i) paloDst->apxfRef[i] = remap(paloSrc->apxfRef[i]);
    paloDst->apartxf = paloSrc->apartxf;
    for (auto& part : paloDst->apartxf) part.second = remap(part.second);
    // (the physics port) the links follow the copy's records (orig 0x1F6068: kind 15's payload)
    paloDst->alink = paloSrc->alink;
    for (auto& pxf : paloDst->axf)
        if (pxf->mode == 15) pxf->pvA8 = nullptr;
    for (ALOLINK& l : paloDst->alink)
    {
        l.pxfTarget = remap(l.pxfTarget);
        l.pxf = remap(l.pxf);
        if (l.pxf) l.pxf->pvA8 = &l;
    }
    // (the physics port) the chains follow the copy's records (orig 0x1F6068: kind 16's group)
    paloDst->achain = paloSrc->achain;
    for (ALOCHAIN& c : paloDst->achain)
        for (ALOCHAINE& e : c.ae)
        {
            e.pchain = &c;
            e.pxf = remap(e.pxf);
            if (e.pxf) e.pxf->pvA8 = &e;
        }
    paloDst->uMeta = paloSrc->uMeta;
    paloDst->pglobset = paloSrc->pglobset;
    // (drawable as the loaded one: +0x40 bit 8 with its glob set, LoadAloFromBrx's; a copy's flags start from InitLo)
    if (paloDst->pglobset && !paloDst->pglobset->aglob.empty()) paloDst->grfUpd |= 8;
    paloDst->asoe = paloSrc->asoe;
    for (SOE& e : paloDst->asoe) e.pxf = remap(e.pxf);
    // the groups' run-time flags (+0xC0's +0xC 0x800, set by the loader's options before the copy: 0x1F6978 /
    // 0x1F69F8) go with the copy (CloneLo copies the object's memory and reallocates the group array with its contents)
    paloDst->aglobShown = paloSrc->aglobShown;
    paloDst->fHasState = paloSrc->fHasState;
    paloDst->gMass = paloSrc->gMass;
    paloDst->matInertia = paloSrc->matInertia;
    CloneAloPhys(paloSrc, paloDst);  // (the physics port)
    CloneLaser(paloSrc, paloDst);    // orig 0x1DE518: the laser's independent endpoint storage
    CloneTrail(paloSrc, paloDst);    // authored vertices for the copy's independent history
}

// orig: 0x001F6978 / 0x001F69F8
void ShowAloGlobs(ALO* palo, int32_t k, bool fShow)
{
    if (!palo) return;
    static const char* s_szOid = std::getenv("SLY2N_SCENEOID");  // (debug, scenesw.cpp)
    if (s_szOid && palo->oid == std::atoi(s_szOid)) std::printf("sceneoid %d %p: groups %d %s\n", palo->oid, (void*)palo, k, fShow ? "shown" : "hidden");
    bool f = false;
    for (auto& [n, fShown] : palo->aglobShown)
        if (n == k) { fShown = fShow; f = true; }
    if (!f) palo->aglobShown.emplace_back(k, fShow);
    // (the group's collision entry: TODO(physics): the +0xB8 entry's +0x24 SOE is not linked natively; by tag)
}
bool FAloGlobShown(const ALO* palo, int32_t n0, bool fDefault)
{
    for (const auto& [n, fShown] : palo->aglobShown)
        if (n == n0) return fShown;
    return fDefault;
}
// orig: 0x001F6AC8 / 0x001F6B48
void SetAloSoesByTag(ALO* palo, int32_t k, bool fOn)
{
    if (!palo) return;
    for (SOE& e : palo->asoe)
        if (e.tag == k)
        {
            if (fOn) SetSoeOn(palo, &e);
            else SetSoeOff(palo, &e);
        }
}

// EOPIDs 29 / 30: the loader's velocity and angular velocity (the setters' PMFs: vt slots 35 / 36, SetAloVelocityTo
// 0x1F4488 / SetAloAngVelocityTo 0x1F46D8), e.g. the rooftop vents' turbines 139218 spinning (option 30)
namespace
{
struct INITALOSETTERS
{
    INITALOSETTERS()
    {
        // orig: 0x00100958 -> 0x001A58D0 (option 33: parent, preservation flags; original slot 10)
        RegisterObjectMethod(0x100958, [](LO* plo, const std::vector<CRef>& a) {
            LO* ploNew = PloArg(a, 0);
            ALO* paloNew = ploNew && FCidDerivedFrom(ploNew->Cid(), 0x30) ? static_cast<ALO*>(ploNew) : nullptr;
            using PFN = void (*)(LO*, ALO*, XF*, int);
            if (std::getenv("SLY2N_PARENTPRINT"))
            {
                std::string str;
                if (a.size() > 1) PrintRef(a[1], &str);
                std::fprintf(stderr, "parent %d -> %d grf %d (%s)\n", plo->oid, ploNew ? ploNew->oid : -1, NArg(a, 1), str.c_str());
            }
            if (PFN pfn = PfnVt<PFN>(plo, 10)) pfn(plo, paloNew, nullptr, NArg(a, 1));
            return CRef(TAGK_Void);
        });
        RegisterObjectSetter(29, [](LO* plo, const CRef& r) {
            if (FCidDerivedFrom(plo->Cid(), 0x30)) SetAloVelocityTo(static_cast<ALO*>(plo), r.GetVector());
        });
        // EOPID 29's getter (option 63): vt slot 33, the position record's world velocity (0x1F4440); e.g. the RC
        // chopper's bombs inherit 0.6 x its velocity (its script's option 1535)
        RegisterObjectGetter(29, [](LO* plo) {
            CRef r;
            using PFN = glm::vec3 (*)(ALO*);
            if (FCidDerivedFrom(plo->Cid(), 0x30))
            {
                ALO* palo = static_cast<ALO*>(plo);
                PFN pfn = reinterpret_cast<PFN>(PvNativeVt(palo, 33));
                r.SetVector(pfn ? pfn(palo) : VelAloPos(palo));
            }
            else r.SetVector(glm::vec3(0.0f));
            return r;
        });
        RegisterObjectSetter(30, [](LO* plo, const CRef& r) {
            if (std::getenv("SLY2N_SETTERPRINT"))
                std::printf("setter 30 %d: %.3f %.3f %.3f rot %p\n", plo->oid, r.GetVector().x, r.GetVector().y, r.GetVector().z,
                            FCidDerivedFrom(plo->Cid(), 0x30) ? (void*)PxfRot(static_cast<ALO*>(plo)) : nullptr);
            if (FCidDerivedFrom(plo->Cid(), 0x30)) SetAloAngVelocityTo(static_cast<ALO*>(plo), r.GetVector());
            if (std::getenv("SLY2N_SETTERPRINT") && FCidDerivedFrom(plo->Cid(), 0x30) && PxfRot(static_cast<ALO*>(plo)))
                std::printf("  after: kind %d mode %d\n", PxfRot(static_cast<ALO*>(plo))->kind, PxfRot(static_cast<ALO*>(plo))->mode);
        });
    }
} s_initalosetters;
}  // namespace

// orig 0x001A6AF8
bool FLoInParentChain(const LO* plo, const LO* ploAncestor)
{
    while (plo && plo != ploAncestor) plo = plo->paloParent;
    return plo != nullptr;
}
