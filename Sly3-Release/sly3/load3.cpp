// Sly 3's object loading: PloNew, the object stream, options, proxies, clones (see load3.h), ported from the Sly 3 asm
// (orig3 addresses) with Sly2-Release's native code as the draft. The classes' loaders are in loaders3.cpp.

#include "load3.h"

#include "addr.h"
#include "ids3.h"

#include "../alo.h"
#include "../bis.h"
#include "../cls.h"
#include "../disc.h"
#include "../splicemap.h"
#include "../sw.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

CMBLOCK3 g_cmblock3;
SW* g_psw3 = nullptr;
LOAD3STATS g_load3stats;
PE3STATS g_pe3stats;
std::size_t g_ibBodyEnd3 = 0;

// ---------------------------------------------------------------- options

LO* PloEnsureChild422(ALO* palo);

namespace
{
struct EOPID3DESC
{
    uint32_t k = 0, grf = 0, pfnArray = 0, c18 = 0, pfnGet = 0;
};

EOPID3DESC Eopid3Desc(int eopid)
{
    if (eopid < 0 || eopid >= orig3::CEOPID) throw std::runtime_error("option id outside the EOPID table");
    const uint32_t va = orig3::VA_AEOPID + uint32_t(eopid) * orig3::CB_EOPID;
    EOPID3DESC e;
    e.k = g_disc.U32Elf(va);
    e.grf = g_disc.U32Elf(va + 4);
    e.pfnArray = g_disc.U32Elf(va + 8);
    e.c18 = g_disc.U32Elf(va + 0x18);
    e.pfnGet = g_disc.U32Elf(va + 0x24);
    return e;
}

// The value of an option of kind k on 0x150AE8's field / setter paths (the dispatches at 0x320480 / 0x3204E0:
// the same reads as Sly 2's 0x13E818, verified by the option-set sizes of all 39 worlds)
OPTVAL ReadOptionValue3(int k, CBinaryInputStream* pbis)
{
    OPTVAL v;
    v.k = k;
    switch (k < 24 ? k : 24)
    {
    case 0: v.n = pbis->U8Read(); break;
    case 5: case 6: v.n = pbis->S32Read(); break;
    case 1: v.ag[0] = pbis->F32Read(); break;
    case 10:
        v.ag[0] = pbis->F32Read();
        v.ag[1] = pbis->F32Read();
        break;
    case 2: case 4: case 9: case 11:
        for (int i = 0; i < 3; ++i) v.ag[i] = pbis->F32Read();
        break;
    case 3: case 12:
        for (int i = 0; i < 4; ++i) v.ag[i] = pbis->F32Read();
        break;
    case 13: v.n = static_cast<int32_t>(pbis->U32Read()); break;  // a colour (AdjustRgba 0x236908)
    case 22:
    {
        const int c = pbis->U16Read();  // 0x119968
        v.an.resize(std::size_t(c));
        pbis->Read(std::size_t(c) * 2, v.an.data());
        break;
    }
    case 8: case 18: case 24:
    {
        const int cls = (k & ~0x8000) >> 12;
        if (cls == 1) v.n = static_cast<int32_t>(pbis->U32Read());
        else if (cls == 2) v.n = pbis->S32Read();  // an object's oid
        break;
    }
    default: v.n = pbis->S16Read(); break;  // 7, 14..17, 19..21, 23
    }
    return v;
}
}  // namespace

// orig3: 0x150960 RefReadOption (the 24-entry dispatch at 0x320420; Sly 2's 0x13E690 reads; tagk: Sly 3 tags)
OPTVAL RefReadOption3(int k, CBinaryInputStream* pbis)
{
    OPTVAL v;
    v.k = k;
    switch (k)
    {
    case 0: v.n = pbis->U8Read() != 0; v.tagk = int(TAGK3::Bool); break;
    case 1: v.ag[0] = pbis->F32Read(); v.tagk = int(TAGK3::F32); break;
    case 2: case 9:
        for (int i = 0; i < 3; ++i) v.ag[i] = pbis->F32Read();
        v.tagk = k == 2 ? int(TAGK3::Vector) : int(TAGK3::Clq);
        break;
    case 5: case 6: v.n = pbis->S32Read(); v.tagk = int(TAGK3::S32); break;
    case 7: case 14: case 15: case 16: case 17: case 19: case 20: case 21: case 23:
        v.n = pbis->S16Read();
        v.tagk = int(TAGK3::S32);
        break;
    case 10:
        v.ag[0] = pbis->F32Read();
        v.ag[1] = pbis->F32Read();
        v.tagk = int(TAGK3::Lm);
        break;
    default:
        if (((k & ~0x8000) >> 12) == 1)
        {
            v.n = static_cast<int32_t>(pbis->U32Read());
            v.tagk = int(TAGK3::S32);
        }
        break;
    }
    return v;
}

// orig3: 0x150AE8 LoadOptionFromBrx (a1 the EOPID record, a2 its grf & 0x2580): the values are kept in the object's
// option list; field options (grf 0x90: a field at c18 of the object, or of the sub-object the getter at +0x24 gives
// with 0x2000) are kept by (getter, offset) so that loaders that test a field find it (LO::U32Field). The setters
// and the CRef methods (grf 0x400: the EOPID's +0x08) are behaviour: TODO(sly3) with the classes' runtime.
void LoadOptionFromBrx3(OPT& opt, LO* plo, int eopid, int grf, CBinaryInputStream* pbis)
{
    const EOPID3DESC e = Eopid3Desc(eopid);
    opt.eopid = eopid;
    opt.grf = grf;
    if (grf & 0x401)
    {
        // c18 CRefs, each a value or a list (U16 kind | 0x8000, then a U8 count of values of that kind)
        for (uint32_t i = 0; i < e.c18; ++i)
        {
            const int w = pbis->U16Read();
            if (w & 0x8000)
            {
                OPTVAL list;
                list.k = w;
                const int c = pbis->U8Read();
                for (int j = 0; j < c; ++j) list.alist.push_back(RefReadOption3(w & 0x7FFF, pbis));
                opt.aval.push_back(std::move(list));
            }
            else opt.aval.push_back(RefReadOption3(w, pbis));
        }
        // the methods called by the load (grf 0x400: the EOPID's +0x08) that decide what loads:
        // orig3: 0x10AFB0 (EOPID 1032: (group id, count)) -> 0x169500: the instance count of the group whose record
        // +0x10 is the id (the option sets' blocks set it before the groups load: m_ext's camera block 13 zeroes
        // the groups its episode does not use, e.g. carmelita, npc_gator_crab)
        if ((grf & 0x400) && e.pfnArray == 0x10AFB0 && g_psw3 && opt.aval.size() >= 2)
        {
            for (WE& we : g_psw3->awe)
                if (we.x10 == opt.aval[0].n)
                {
                    we.cinst = opt.aval[1].n;
                    break;
                }
        }
        else if ((grf & 0x400) && std::getenv("SLY3_METHODPRINT"))
            std::printf("load method: EOPID %d pfn 0x%06X (object %d)\n", eopid, e.pfnArray, plo ? plo->oid : -1);
        return;
    }
    // the sub-object getter runs before the value is stored (0x241268 makes the 0x11 child it returns)
    if (plo && (grf & 0x2000) && e.pfnGet == 0x241268 && FIsLoDerivedFrom(plo, Icid(CID3::ALO)))
        PloEnsureChild422(static_cast<ALO*>(plo));
    OPTVAL v = ReadOptionValue3(int(e.k), pbis);
    if ((grf & 0x90) && plo)
    {
        const uint32_t pfn = (grf & 0x2000) ? e.pfnGet : 0;
        const uint32_t cg = e.k == 1 ? 1 : e.k == 10 ? 2 : (e.k == 2 || e.k == 9 || e.k == 11) ? 3 : 0;
        if (cg)
            for (uint32_t i = 0; i < cg; ++i)
            {
                uint32_t u;
                std::memcpy(&u, &v.ag[i], 4);
                plo->SetField(pfn, e.c18 + 4 * i, u);
            }
        else plo->SetField(pfn, e.c18, static_cast<uint32_t>(v.n));
    }
    opt.aval.push_back(std::move(v));
}

// (debug: SLY3_OPTPRINT) every option of an object as loaded: EOPID, grf, kind, the field offset (+0x18) or the
// method (+0x08), the getter (+0x24), the values
static void PrintOptList3(const std::vector<OPT>& aopt, FILE* pf);
void PrintObjectOptions3(const LO* plo, FILE* pf)
{
    std::fprintf(pf, "options of cid 0x%02X oid %d (%zu)\n", plo->Cid(), plo->oid, plo->aopt.size());
    PrintOptList3(plo->aopt, pf);
    if (plo->Cid() == 0x2D)
        for (const PECOMP3& c : static_cast<const LOPE3*>(plo)->acomp)
        {
            std::fprintf(pf, " component cid 0x%X oid %d n %d (%zu options)%s\n", c.cid, c.oid, c.n, c.aopt.size(),
                         c.pmesh ? " +mesh" : "");
            PrintOptList3(c.aopt, pf);
        }
}
static void PrintOptList3(const std::vector<OPT>& aopt, FILE* pf)
{
    for (const OPT& o : aopt)
    {
        const EOPID3DESC e = Eopid3Desc(o.eopid);
        std::fprintf(pf, "  eopid %4d grf 0x%04X k %2u +0x%03X pfn 0x%06X get 0x%06X:", o.eopid, o.grf, e.k, e.c18, e.pfnArray,
                     e.pfnGet);
        auto pr = [&](auto&& self, const OPTVAL& v) -> void {
            if (!v.alist.empty())
            {
                std::fprintf(pf, " [");
                for (const OPTVAL& w : v.alist) self(self, w);
                std::fprintf(pf, " ]");
            }
            else if (v.k == 1 || v.k == 2 || v.k == 9 || v.k == 10 || v.k == 11) std::fprintf(pf, " (%g %g %g)", v.ag[0], v.ag[1], v.ag[2]);
            else if (!v.an.empty()) { std::fprintf(pf, " {"); for (int16_t n : v.an) std::fprintf(pf, " %d", n); std::fprintf(pf, " }"); }
            else std::fprintf(pf, " %d", v.n);
        };
        for (const OPTVAL& v : o.aval) pr(pr, v);
        std::fprintf(pf, "\n");
    }
}

// orig3: 0x1519C8 LoadOptionsFromBrx: S16 ids until a negative one, each read with its grf & 0x2580
int LoadOptionsFromBrx3(std::vector<OPT>& aopt, LO* plo, CBinaryInputStream* pbis)
{
    int c = 0;
    for (int n; (n = pbis->S16Read()) >= 0; ++c)
    {
        OPT opt;
        LoadOptionFromBrx3(opt, plo, n, int(Eopid3Desc(n).grf & 0x2580), pbis);
        aopt.push_back(std::move(opt));
    }
    return c;
}

// ---------------------------------------------------------------- objects

namespace
{
std::vector<LO*> s_aploLoading;  // the objects whose loads run, innermost last (their namespaces own what they make)
}

// vt slot 42 (SetMatrix; Sly 2's slot 37): the object placed by m (m * its own matrix)
void SetLoMatrix3(LO* plo, const glm::mat4& m)
{
    if (FIsLoDerivedFrom(plo, Icid(CID3::ALO)))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (!palo->axf.empty()) palo->axf[0]->mat = m * palo->axf[0]->mat;
    }
    else if (FIsLoDerivedFrom(plo, Icid(CID3::LOX))) static_cast<LOX*>(plo)->mat = m * static_cast<LOX*>(plo)->mat;
}

// vt slot 10 (part): an ALO placed relative to a record of its new parent (a proxy's channel)
void SetLoChannel3(LO* plo, XF* pxf)
{
    if (!pxf) return;
    if (FIsLoDerivedFrom(plo, Icid(CID3::ALO)))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (!palo->axf.empty() && !palo->axf[0]->pxfRef) palo->axf[0]->pxfRef = pxf;
    }
    else if (FIsLoDerivedFrom(plo, Icid(CID3::LOX))) static_cast<LOX*>(plo)->pxf = pxf;
}

void LOSINK3::Append(LO* plo)
{
    if (fStack)
    {
        // the loader's own sink for the shared modes (Sly 2's 0x13FA30): the object leaves its parent for the world's
        // root (SetLoParent(null), vt slot 10: so it is not part of the parent's clones), and it (and an ALO's
        // children) get flags 48..51 = n and the key (vt slot 25; TODO(sly3): the slot bodies)
        if (ALO* paloParent = plo->paloParent)
        {
            auto& a = paloParent->aploChild;
            a.erase(std::remove(a.begin(), a.end(), plo), a.end());
            plo->paloParent = nullptr;
        }
        if (!s_aploLoading.empty())
        {
            auto& a = s_aploLoading.back()->aploOwned;
            a.erase(std::remove(a.begin(), a.end(), plo), a.end());
        }
        auto set = [&](auto&& self, LO* p) -> void {
            p->grf = (p->grf & ~(0xFull << 48)) | (uint64_t(n & 0xF) << 48);
            if (FIsLoDerivedFrom(p, Icid(CID3::ALO)))
                for (LO* pc : static_cast<ALO*>(p)->aploChild) self(self, pc);
        };
        set(set, plo);
        return;
    }
    if (papo) papo->push_back(plo);
}

namespace
{

void LoadLoBody3(LO* plo, CBinaryInputStream* pbis)
{
    if (!plo->pcls->pfnLoad)
    {
        char sz[96];
        std::snprintf(sz, sizeof(sz), "no native loader for class 0x%02X (orig3 0x%06X)", plo->Cid(), plo->pcls->pfnLoadOrig);
        throw std::runtime_error(sz);
    }
    s_aploLoading.push_back(plo);
    try
    {
        plo->pcls->pfnLoad(plo, pbis);  // vt slot 14
    }
    catch (...)
    {
        s_aploLoading.pop_back();
        throw;
    }
    s_aploLoading.pop_back();
}

// The objects of the shared modes are searched as Sly2-Release does (TODO(sly3): the namespaces of 0x17E1B8 with
// 0x405 / 0x17E1D8 with 0x805: mode 2 anywhere, mode 3 in the group being loaded)
LO* PloFindShared3(SW* psw, CID cid, OID oid, bool fAnywhere)
{
    (void)cid;
    for (const auto& p : psw->aploAll)
        if (p->oid == oid && (fAnywhere || p->igrp == psw->iwe)) return p.get();
    return nullptr;
}
}  // namespace

// orig3: 0x241268 PloEnsureChild391 (twin 0x1F7B98): the ALO's child of class 0x11 oid 422 (0x1A6), made when
// PloFindSwNearest(2, 422, palo) finds none; also the getter of some options (their EOPID +0x24)
LO* PloEnsureChild422(ALO* palo)
{
    for (LO* pc : palo->aploChild)
        if (pc->oid == 0x1A6) return pc;
    return PloNew3(g_psw3, 0x11, palo, 0x1A6, -1);
}

// The InitLo bodies (vt slot 7) that make objects. orig3: 0x250868 (the guards 0x3A..0x43): the world's one 0x92
// (oid 1864, 0x250820, no parent) and the guard's own 0x0F (oid 757, no parent, activation kind 2: +0x10D0)
void InitLoObjects3(SW* psw, LO* plo)
{
    if (plo->pcls->PfnOrig(7) == 0x250868)
    {
        static SW* s_pswOnce = nullptr;
        if (s_pswOnce != psw)
        {
            s_pswOnce = psw;
            PloNew3(psw, 0x92, nullptr, 0x748, -1);
        }
        PloNew3(psw, 0x0F, nullptr, 0x2F5, -1);
    }
}

// The PostLoad bodies (vt slot 17, after the world's objects are loaded) that make objects:
// - the players (every player class's slot 17 calls the base 0x152488, which calls PostLoadInteract 0x1B2720):
//   a class 0x0E zone, oid 361, no parent (0x1B273C); TODO(sly3): BindReach 0x2086B0's 0x09 oid 226 per reach limb
//   (x8 under Sly, x4 under Bentley: needs the reach structure, the Sly 3 player's);
// - the guards (0x3A..0x43: PostLoadGuard 0x250BD8, also under 0x3C's 0x15F490): a class 0x86 object, oid 802, no
//   parent (0x250C70; stored at the guard's +0x328, then CompletionCommand 0x197420 / 0x1975C8 on it).
void PostLoadSwObjects3(SW* psw)
{
    const std::size_t c = psw->aploAll.size();
    for (std::size_t i = 0; i < c; ++i)
    {
        LO* plo = psw->aploAll[i].get();
        if (FCidDerivedFrom(plo->Cid(), 0x33)) PloNew3(psw, 0x0E, nullptr, 361, -1);
        else if (FCidDerivedFrom(plo->Cid(), 0x3A)) PloNew3(psw, 0x86, nullptr, 802, -1);
    }
}

// orig3: 0x150690 PloNew(cid, paloParent, oid, iframe): the class's pfnNew (the table's +0x10), +0x10 the parent,
// +0x18 the oid; the splice frame of module iframe (0x249B78, 0x2BAA50: the Sly 3 splice port); the oid / cid buckets
// (SW +0x20 + 12 * hash, SW +0x3020 + 12 * cid); InitLo (vt slot 7); OnAdd (slot 11) when +0x2F bit 0; slot 25 when
// 0x1C8610; the load list (SW +0x3F14).
LO* PloNew3(SW* psw, CID cid, ALO* paloParent, OID oid, int iframe)
{
    CLS* pcls = PclsFromCid(cid);
    if (!pcls || !pcls->pfnNew) throw std::runtime_error("PloNew: class " + StrClassName(cid) + " cannot be created");
    LO* plo = pcls->pfnNew();
    plo->pcls = pcls;
    plo->paloParent = paloParent;
    plo->oid = oid;
    plo->iframe = iframe;
    // orig3: 0x249B78 (SW +0x3F18: the loading group's splice table, its +0x8 frames) / 0x2BAA50 (LO +0x0): the
    // object's module frame, its message handlers (the Sly 3 boot to gameplay; null without the splice runtime, as --load-world)
    if (iframe >= 0) plo->pframe = PframeFromIsplice(iframe, psw);
    plo->igrp = psw->iwe;
    plo->ichannel = psw->iweChannel;
    psw->aploAll.emplace_back(plo);
    psw->mpcidaplo[std::size_t(cid)].push_back(plo);
    // InitLo (vt slot 7): the object joins its parent's children (TODO(sly3): the classes' InitLo bodies)
    if (paloParent) paloParent->aploChild.push_back(plo);
    psw->aploLoad.push_back(plo);
    if (!s_aploLoading.empty()) s_aploLoading.back()->aploOwned.push_back(plo);
    ++g_load3stats.cobj;
    InitLoObjects3(psw, plo);
    return plo;
}

namespace
{
// The loaded data of an ALO for its copy (Sly2-Release's CloneAloData without its Sly 2 runtime parts): the records
// (references into the original's records go to the copy's, into the original's parent's to the copy's parent's),
// the glob set (shared), the collision entries, the state.
void CloneAloData3(const ALO* paloSrc, ALO* paloDst)
{
    auto remap = [&](XF* pxf) -> XF* {
        if (!pxf) return nullptr;
        auto find = [&](const ALO* pa, ALO* pb) -> XF* {
            if (!pa || !pb) return nullptr;
            for (std::size_t i = 0; i < pa->axf.size() && i < pb->axf.size(); ++i)
                if (pa->axf[i].get() == pxf) return pb->axf[i].get();
            return nullptr;
        };
        if (XF* p = find(paloSrc, paloDst)) return p;
        if (XF* p = find(paloSrc->paloParent, paloDst->paloParent)) return p;
        return pxf;
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
    paloDst->apair = paloSrc->apair;
    for (ALOPAIR& pair : paloDst->apair)
        for (XF*& pxf : pair.apxf) pxf = remap(pxf);
    paloDst->alink = paloSrc->alink;
    for (ALOLINK& l : paloDst->alink)
    {
        l.pxfTarget = remap(l.pxfTarget);
        l.pxf = remap(l.pxf);
    }
    paloDst->achain = paloSrc->achain;
    for (ALOCHAIN& c : paloDst->achain)
        for (ALOCHAINE& e : c.ae)
        {
            e.pchain = &c;
            e.pxf = remap(e.pxf);
        }
    paloDst->uMeta = paloSrc->uMeta;
    paloDst->pglobset = paloSrc->pglobset;
    paloDst->asoe = paloSrc->asoe;
    for (SOE& e : paloDst->asoe)
    {
        e.pxf = remap(e.pxf);
        e.palo = paloDst;
    }
    paloDst->fHasState = paloSrc->fHasState;
    paloDst->gMass = paloSrc->gMass;
    paloDst->matInertia = paloSrc->matInertia;
}
}  // namespace

// orig3: 0x1507E0 PloCloneLo(plo, paloParent): PloNew(the class of vt slot 6, paloParent, oid, -1), then CloneLo
// (vt slot 44) copies the object's data and clones its children under the copy (the classes' bodies: TODO(sly3);
// here the loaded data of the common structs and the children owned by the object).
LO* PloCloneLo3(SW* psw, LO* plo, ALO* paloParent)
{
    if (!plo) return nullptr;
    LO* ploNew = PloNew3(psw, plo->Cid(), paloParent, plo->oid, -1);
    ++g_load3stats.cclone;
    ploNew->ploCloneOf = plo;
    ploNew->iclone = plo->iclone + 1;
    ploNew->iframe = plo->iframe;
    ploNew->pframe = plo->pframe;
    ploNew->aopt = plo->aopt;
    ploNew->afield = plo->afield;
    ploNew->grfLoad = plo->grfLoad;
    ploNew->igrp = plo->igrp;
    if (FIsLoDerivedFrom(plo, Icid(CID3::ALO))) CloneAloData3(static_cast<ALO*>(plo), static_cast<ALO*>(ploNew));
    else if (FIsLoDerivedFrom(plo, Icid(CID3::LOX)))
    {
        const LOX* p = static_cast<LOX*>(plo);
        LOX* q = static_cast<LOX*>(ploNew);
        q->mat = p->mat;
        q->ixfRef = p->ixfRef;
        q->pxf = p->pxf;
        if (p->pxf && p->paloParent && paloParent)
            for (std::size_t i = 0; i < p->paloParent->axf.size() && i < paloParent->axf.size(); ++i)
                if (p->paloParent->axf[i].get() == p->pxf) q->pxf = paloParent->axf[i].get();
    }
    s_aploLoading.push_back(ploNew);
    const std::vector<LO*> aploOwned = plo->aploOwned;
    for (LO* pc : aploOwned)
    {
        if (pc->fRemoved) continue;  // posted at its load (vt slot 9: an SM's states and animations) - as Sly 2's CloneLo
        PloCloneLo3(psw, pc, pc->paloParent == plo ? static_cast<ALO*>(ploNew) : paloParent);
    }
    s_aploLoading.pop_back();
    return ploNew;
}

namespace
{
// orig3: 0x1FA778 LoadProxyFromBrx(psw, paloParent, cid, oid, iframe, pbis, psink): an instance of a group (classes
// derived from PROXY 8): a new object of the class (mode 1, or -1 with the group's +0x60), or the group's next
// instance placed here (0x1FAFB0), with the binds' options and the record's own objects.
void LoadProxyFromBrx3(SW* psw, ALO* paloParent, CID cid, OID oid, int iframe, CBinaryInputStream* pbis, LOSINK3* psink)
{
    (void)iframe;
    ++g_load3stats.cproxy;
    const int32_t idGroup = pbis->S32Read();
    const int mode = pbis->S8Read();
    XF* pxf = PxfFromRef(pbis->S16Read(), paloParent, nullptr);  // 0x28C250
    const glm::mat4 mat = pbis->ReadMatrix4();
    const uint32_t grfLoad = pbis->U32Read();
    // 0x1FC1A8: the group whose record's +0x10 is the id
    int iwe = -1;
    for (int i = 0; i < int(psw->awe.size()); ++i)
        if (psw->awe[std::size_t(i)].x10 == idGroup) { iwe = i; break; }
    const int fNew = mode != -1 ? mode : (iwe >= 0 && psw->awe[std::size_t(iwe)].x60 != 0);
    if (fNew > 0)
    {
        LO* plo = PloNew3(psw, cid, paloParent, oid, -1);
        LOX* plox = static_cast<LOX*>(plo);
        plox->pxf = pxf;      // +0x40
        plox->mat = mat;      // +0x50
        plox->grfLoad = grfLoad;
        PROXYSTREAM& ps = psw->mpproxy[plo];  // +0xD0 the group, +0xD4 fNew
        ps.iwe = iwe;
        const int cbind = pbis->U8Read();     // +0xDC binds x 0x18 at +0xE0
        for (int i = 0; i < cbind; ++i)
        {
            const int32_t kind = pbis->S32Read();
            const int type = pbis->S16Read();
            pbis->S16Read();  // the bind's splice module (0x249B78: the Sly 3 splice port)
            const int cb = pbis->U16Read();
            if (kind == 7)
            {
                // the proxy's own options (0x13B500: when it derives from type)
                if (FIsLoDerivedFrom(plo, type)) LoadOptionsFromBrx3(plo->aopt, plo, pbis);
                else pbis->Skip(std::size_t(cb));
            }
            else
            {
                PROXYBIND bind;
                bind.kind = kind;
                bind.type = type;
                bind.ab.resize(std::size_t(cb));
                pbis->Read(std::size_t(cb), bind.ab.data());
                ps.abind.push_back(std::move(bind));
            }
        }
        const int32_t cbRest = pbis->S32Read();
        pbis->S16Read();
        pbis->Skip(std::size_t(cbRest - 2));
        if (psink) psink->Append(plo);
        return;
    }
    // 0x1FAFB0: the group's next instance (its objects loaded or cloned by the world loader), placed here: the
    // parent (slot 10), the matrix (slot 42), the mask (0x1C9198 unless bit 0x40'0000'0000'0000)
    std::vector<LO*> aploFirst;
    if (iwe >= 0)
    {
        WE& we = psw->awe[std::size_t(iwe)];
        const std::size_t ib = std::size_t(we.cUsed) * std::size_t(we.cloLoaded);
        if (ib + std::size_t(we.cloLoaded) > we.aplo.size())
            throw std::runtime_error("proxy: group " + we.StrName() + " has no instance left");
        for (int j = 0; j < we.cloLoaded; ++j)
        {
            LO* plo = we.aplo[ib + std::size_t(j)];
            plo->fRemoved = false;  // AddLo
            if (plo->paloParent != paloParent)
            {
                if (plo->paloParent)
                {
                    auto& a = plo->paloParent->aploChild;
                    a.erase(std::remove(a.begin(), a.end(), plo), a.end());
                }
                plo->paloParent = paloParent;
                if (paloParent) paloParent->aploChild.push_back(plo);
            }
            SetLoChannel3(plo, pxf);
            // (the players keep their instance's matrix aside, as Sly2-Release's: in PCSX2 RAM their root record stays
            // the identity and the player code moves their record 1 there at the start; Bentley 25 at st2)
            if (FIsLoDerivedFrom(plo, Icid(CID3::PLAYER))) psw->mpmatPlayerInstance[plo] = mat;
            else SetLoMatrix3(plo, mat);
            plo->oidInstance = oid;
            aploFirst.push_back(plo);
            if (!s_aploLoading.empty()) s_aploLoading.back()->aploOwned.push_back(plo);
        }
        ++we.cUsed;
    }
    const int c = pbis->U8Read();
    for (int i = 0; i < c; ++i)
    {
        const int32_t kind = pbis->S32Read();
        const int type = pbis->S16Read();
        LO* ploFound = nullptr;
        if (!aploFirst.empty())
        {
            if (kind == 7 || kind == 8) ploFound = aploFirst[0];
            else
            {
                // RefFindObjects 0x17DEC0 (0xA05) in the instance's namespace: its objects and their descendants
                auto find = [&](auto&& self, LO* p) -> LO* {
                    if (p->oid == kind) return p;
                    for (LO* pc : p->aploOwned)
                        if (LO* q = self(self, pc)) return q;
                    return nullptr;
                };
                for (LO* p : aploFirst)
                    if ((ploFound = find(find, p))) break;
            }
            if (ploFound && !FIsLoDerivedFrom(ploFound, type)) ploFound = nullptr;
        }
        pbis->S16Read();  // the bind's splice module
        const int cb = pbis->U16Read();
        if (ploFound) LoadOptionsFromBrx3(ploFound->aopt, ploFound, pbis);
        else pbis->Skip(std::size_t(cb));
    }
    std::vector<LO*> aploSecond;
    pbis->S32Read();
    PushXfBase(pxf);
    LOSINK3 sink;
    sink.papo = &aploSecond;
    LoadSwObjsFromBrx3(psw, paloParent, pbis, &sink);
    PopXfBase();
    for (LO* p : aploSecond)
    {
        // slot 27 (the key), slot 42 (the matrix), the mask
        SetLoMatrix3(p, mat);
        p->ploNs = aploFirst.empty() ? nullptr : aploFirst[0];
    }
    if (psink)
    {
        for (LO* p : aploFirst) psink->Append(p);
        for (LO* p : aploSecond) psink->Append(p);
    }
}
}  // namespace

// orig3: 0x151D80 LoadSwObjsFromBrx(paloParent, pbis, psink): S16 c x {S8 mode, (mode 1: U8 n1, U8 n2, S16 filters,
// S32 conditions), S16 cid, S32 oid, S16 n (the splice module), S32 cb, the body}
void LoadSwObjsFromBrx3(SW* psw, ALO* paloParent, CBinaryInputStream* pbis, LOSINK3* psink)
{
    LOSINK3 sinkStack;
    sinkStack.fStack = true;
    const bool fTop = s_aploLoading.empty();
    int c = pbis->S16Read();
    for (; c > 0; --c)
    {
        int16_t afilter[128];
        int32_t acond[64];
        int n1 = 0, n2 = 0;
        const int mode = pbis->S8Read();
        if (mode == 1)
        {
            n1 = pbis->U8Read();
            n2 = pbis->U8Read();
            if (n1 > 128 || n2 > 64) throw std::runtime_error("LoadSwObjsFromBrx: filter list too long");
            pbis->Read(std::size_t(n1) * 2, afilter);
            pbis->Read(std::size_t(n2) * 4, acond);
        }
        const CID cid = pbis->S16Read();
        const OID oid = pbis->S32Read();
        const int iframe = pbis->S16Read();
        const int32_t cb = pbis->S32Read();
        if (cid < 0 || cid >= CID_Max) throw std::runtime_error("LoadSwObjsFromBrx: class outside the table");
        LOSINK3* psinkCur = psink;
        if (mode == 1)
        {
            if (!(FFilterMatches3(n1, afilter) && FConditionsMatch3(n2, acond)))
            {
                if (cid == Icid(CID3::MISSION) || cid == Icid(CID3::TASK))
                {
                    // missions and tasks are made anyway, marked (MISSION +0x50, TASK +0x40 = the mode) and loaded
                    LO* plo = PloNew3(psw, cid, paloParent, oid, -1);
                    plo->fMission = true;
                    ++g_load3stats.cmission;
                    LoadLoBody3(plo, pbis);
                    if (psink) psink->Append(plo);
                }
                else
                {
                    pbis->Skip(std::size_t(cb));
                    ++g_load3stats.cfiltered;
                }
                continue;
            }
        }
        else if (mode == 2 || mode == 3)
        {
            // a shared object: loaded once (2: PloFindSwNearest 0x405, 3: PloFindSwObjectList 0x805), into the
            // loader's own sink (n 1 / 0; key 0x1FA5A8(SW +0x4638, -1))
            ++g_load3stats.cshared;
            if (PloFindShared3(psw, cid, oid, mode == 2))
            {
                pbis->Skip(std::size_t(cb));
                ++g_load3stats.cskipShared;
                continue;
            }
            sinkStack.n = mode == 2 ? 1 : 0;
            sinkStack.key = (psw->iwe << 16) | 0xFFFF;
            psinkCur = &sinkStack;
        }
        const std::size_t ibBody = pbis->m_ib;
        try
        {
            if (FCidDerivedFrom(cid, Icid(CID3::PROXY))) LoadProxyFromBrx3(psw, paloParent, cid, oid, iframe, pbis, psinkCur);
            else
            {
                LO* plo = PloNew3(psw, cid, paloParent, oid, iframe);
                const std::size_t ibEndSave = g_ibBodyEnd3;
                g_ibBodyEnd3 = ibBody + std::size_t(cb);
                if (plo->pcls->pfnLoad) LoadLoBody3(plo, pbis);
                else
                {
                    // (development: a class whose loader is not ported yet is made and its body skipped by its size;
                    // the objects its loader would make are missing: --load-world lists these classes)
                    pbis->Skip(std::size_t(cb));
                    ++g_load3stats.mpcidcUnported[cid];
                }
                g_ibBodyEnd3 = ibEndSave;
                if (psinkCur) psinkCur->Append(plo);
            }
            // every object's body (a proxy's included) is exactly its record's size
            if (pbis->m_ib - ibBody != std::size_t(cb))
            {
                char sz[128];
                std::snprintf(sz, sizeof(sz), "class 0x%02X read %zd bytes of %d", cid, std::ptrdiff_t(pbis->m_ib - ibBody), cb);
                throw std::runtime_error(sz);
            }
            if (fTop) ++g_load3stats.ctopOk;
        }
        catch (const std::exception& e)
        {
            // (development: a top-level object whose loader fails or misreads is kept as far as it loaded, and the
            // stream continues after its body; nested failures end their top-level object)
            if (!fTop) throw;
            s_aploLoading.clear();
            ++g_load3stats.ctopFail;
            ++g_load3stats.mperrc[e.what()];
            ++g_load3stats.mpcidcFail[cid];
            if (std::getenv("SLY3_LOADERR"))
                std::fprintf(stderr, "load3: object oid %d class 0x%02X at %zu: %s\n", oid, cid, ibBody, e.what());
            pbis->m_ib = ibBody + std::size_t(cb);
        }
    }
}

void PrintObjectCensus3(SW* psw, FILE* pf)
{
    std::fprintf(pf, "# obj cid oid parentCid parentOid igrp clone\n");
    for (const auto& p : psw->aploAll)
    {
        const LO* plo = p.get();
        const LO* ploParent = plo->paloParent;
        std::fprintf(pf, "obj 0x%02X %d 0x%02X %d %d %d\n", plo->Cid(), plo->oid, ploParent ? ploParent->Cid() : 0xFF,
                     ploParent ? ploParent->oid : -1, plo->igrp, plo->iclone);
    }
}

uint32_t PfnEopid3(int eopid) { return Eopid3Desc(eopid).pfnArray; }

// The world matrices of every placed ALO's records (alo.cpp UpdateAloHierarchyWorlds from each top-level ALO): the
// native objects' XF::matWorld (the emitters', the lights' and the render-target cameras' placements read them).
void UpdateLoWorlds3(SW* psw)
{
    if (!psw) return;
    for (const auto& plo : psw->aploAll)
        if (FIsLoDerivedFrom(plo.get(), Icid(CID3::ALO)) && !plo->paloParent) UpdateAloHierarchyWorlds(static_cast<ALO*>(plo.get()));
}
