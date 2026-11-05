int IspliceBlockCur(struct SW* psw);
#include <execinfo.h>
#include "wr.h"
#include "clock.h"
#include "breakable.h"
#include "laser.h"
#include "trail.h"
// The classes' LoadXxxFromBrx functions (vtable slot 14 of the original classes), bound by original address
// (cls.cpp), and the native struct of each class.
//
// Most classes' loaders are their base's plus non-reading setup (allocation, links, defaults): those parts are
// marked TODO(M3/M4) with the original function they call, for when the class's behaviour is ported.

#include "activate.h"
#include "task.h"
#include "scpt.h"
#include "alo.h"
#include "sm.h"
#include "bis.h"
#include "brx.h"
#include "cls.h"
#include "readtrace.h"
#include "sw.h"
#include "so.h"
#include "splicemap.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <unordered_map>

// vt slot 37 (SetMatrix: ALO 0x1F4010, LOX 0x231598): the object placed by m (m * its own matrix)
void SetLoMatrix(LO* plo, const glm::mat4& m)
{
    if (FIsLoDerivedFrom(plo, 0x30))
    {
        SetAloMatrix(static_cast<ALO*>(plo), m);
        UpdateAloHierarchyWorlds(static_cast<ALO*>(plo));  // (its world now: the activation measures it at once)
    }
    else if (FIsLoDerivedFrom(plo, 3)) static_cast<LOX*>(plo)->mat = m * static_cast<LOX*>(plo)->mat;  // (0x231598: 0x11C168(t, m, local))
}

// vt slot 10 (part): the object's placement relative to a record of its new parent (the proxy's channel)
void SetLoChannel(LO* plo, XF* pxf)
{
    if (static const char* s_szMove = std::getenv("SLY2N_MOVEPRINT"); s_szMove && std::atoi(s_szMove) == plo->oid)
    {
        void* apv[16];
        std::fprintf(stderr, "move %d by SetLoChannel t %.3f:\n", plo->oid, g_clock.t);
        backtrace_symbols_fd(apv, backtrace(apv, 16), 2);
    }
    if (!pxf) return;
    if (FIsLoDerivedFrom(plo, 0x30))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (!palo->axf.empty() && !palo->axf[0]->pxfRef) palo->axf[0]->pxfRef = pxf;
    }
    else if (FIsLoDerivedFrom(plo, 3) && !static_cast<LOX*>(plo)->pxf) static_cast<LOX*>(plo)->pxf = pxf;
}

namespace
{

// orig 0x001A5C28 LoadLoFromBrx
void LoadLoFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1A5C28);
    LoadOptionsFromBrx(plo->aopt, plo, pbis);
}

// orig 0x00231768 LoadLoxBaseFromBrx: the transform reference (vt slot 49), the matrix, the load flags
void LoadLoxBaseFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x231768);
    LOX* plox = static_cast<LOX*>(plo);
    plox->ixfRef = pbis->S16Read();
    plox->pxf = PxfFindLox(plox, plox->ixfRef);
    plox->mat = pbis->ReadMatrix4();
    // TODO(M3): 0x2313F0
    plox->grfLoad = pbis->U32Read();
    SetLoMask(g_psw, plox, plox->grfLoad);  // orig 0x1A6D28(p, U32)
}

// orig 0x00231170 LoadLoxFromBrx
void LoadLoxFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x231170);
    LoadLoxBaseFromBrx(plo, pbis);
    LoadOptionsFromBrx(plo->aopt, plo, pbis);
}

// orig 0x002317E0: objects loaded by a LOX: children of the LOX's parent, placed by the LOX's matrix
void LoadLoxChildrenFromBrx(LOX* plox, CBinaryInputStream* pbis, std::vector<LO*>* papo)
{
    LOADFN(0x2317E0);
    PushXfBase(plox->pxf);
    LOSINK sink;
    sink.papo = papo;
    LoadSwObjsFromBrx(g_psw, plox->paloParent, pbis, &sink);
    PopXfBase();
    if (papo)
        for (LO* plo : *papo) SetLoMatrix(plo, plox->mat);  // vt slot 37
}

// orig 0x00227380 (cid 5)
void LoadVolumeFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x227380);
    LOXGEOM* p = static_cast<LOXGEOM*>(plo);
    LoadLoxBaseFromBrx(p, pbis);
    p->u = pbis->U32Read();
    pbis->ReadGeom(&p->geom);
    pbis->ReadBsp(&p->geom, &p->bsp);
    LoadOptionsFromBrx(p->aopt, p, pbis);
}

// orig 0x00227BB8 (cid 0x16 family)
void LoadShapeFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x227BB8);
    LOXSHAPE* p = static_cast<LOXSHAPE*>(plo);
    LoadLoxFromBrx(p, pbis);
    pbis->ReadGeom(&p->geom);
    pbis->ReadBsp(&p->geom, &p->bsp);
    // orig: 0x00227BB8, component minima/maxima 0x11F288 / 0x11F2B8.
    if (!p->geom.apos.empty())
    {
        glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
        for (const glm::vec3& pos : p->geom.apos) lo = glm::min(lo, pos);
        for (const glm::vec3& pos : p->geom.apos) hi = glm::max(hi, pos);
        p->posBound = lo * 0.5f + hi * 0.5f;
        const glm::vec3 d = p->posBound - lo;
        p->radiusBoundSquared = (d.x * d.x + d.y * d.y) + d.z * d.z;
    }
    else p->radiusBoundSquared = -FLT_MAX;
}

// orig 0x00149358
void LoadShape149358FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x149358);
    LoadShapeFromBrx(plo, pbis);
    // TODO(M3): 0x148738
}

// orig 0x00228250: a 64-byte key record, kept when its words 1 and 15 differ (the original's "is set")
uint32_t LoadKeyRecordFromBrx(CBinaryInputStream* pbis, uint8_t* pbOut = nullptr)
{
    LOADFN(0x228250);
    uint32_t aw[16];
    pbis->Read(64, aw);
    if (g_pfnNoteKeyRecord) g_pfnNoteKeyRecord(aw, KEYK_Exit, -1, -1);
    if (pbOut) std::memcpy(pbOut, aw, 64);
    return aw[1] != aw[15] ? 1u : 0u;
}

// orig 0x002282B0 (cid 0x1A)
void LoadShapeKeysFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x2282B0);
    LOXSHAPE* p = static_cast<LOXSHAPE*>(plo);
    LoadShapeFromBrx(p, pbis);
    std::array<uint8_t, 64> ab{};
    p->uX = LoadKeyRecordFromBrx(pbis, ab.data());
    p->aabKey.push_back(ab);
    const int c = pbis->U8Read();
    for (int i = 0; i < c; ++i)
    {
        p->au.push_back(LoadKeyRecordFromBrx(pbis, ab.data()));
        p->aabKey.push_back(ab);
    }
}

// orig 0x00228140 / 0x00228720: LOX + a skipped string
void LoadLoxName228140FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x228140);
    LoadLoxFromBrx(plo, pbis);
    pbis->SkipStringSw();
}
void LoadLoxName228720FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x228720);
    LoadLoxFromBrx(plo, pbis);
    pbis->SkipStringSw();
}

// orig 0x0012EB78, 0x001BEA88, 0x002035A8: the LOX loader with class setup around it
void LoadLox12EB78FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x12EB78);
    LoadLoxFromBrx(plo, pbis);
}
void LoadLox1BEA88FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1BEA88);
    LoadLoxFromBrx(plo, pbis);
}
void LoadLox2035A8FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x2035A8);
    LoadLoxFromBrx(plo, pbis);  // (defaults set before: +0xE0 75, +0xE8 0.25, +0x104 5000)
}

// orig 0x001E5DF8 (cid 0x0A family)
void LoadPathFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1E5DF8);
    LOXPATH* p = static_cast<LOXPATH*>(plo);
    LoadLoxBaseFromBrx(p, pbis);
    p->fE0 = pbis->S8Read();
    p->nE4 = pbis->S8Read();
    const int c = pbis->U16Read();
    p->an.resize(c);
    pbis->Read(static_cast<std::size_t>(c) * 4, p->an.data());
    for (int i = 0; i < c; ++i) p->avecF4.push_back(pbis->ReadVector());
    if (p->fE0)
    {
        for (int i = 0; i < c; ++i) p->avec140.push_back(pbis->ReadVector());
        for (int i = 0; i < c; ++i) p->avec144.push_back(pbis->ReadVector());
    }
    LoadOptionsFromBrx(p->aopt, p, pbis);
    // TODO(M3): 0x1E6180
}
void LoadPath1BC088FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1BC088);
    LoadPathFromBrx(plo, pbis);
}

// orig 0x001C03C0 (cid 0x11)
void LoadMeshZoneFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1C03C0);
    LOXMESH* p = static_cast<LOXMESH*>(plo);
    LoadLoxFromBrx(p, pbis);
    const int cvtx = pbis->U16Read();
    const int ctri = pbis->U16Read();
    p->avtx.resize(cvtx);
    for (int i = 0; i < cvtx; ++i)
    {
        p->avtx[i].pos = pbis->ReadVector();
        const int c = pbis->U8Read();
        for (int j = 0; j < c; ++j) p->avtx[i].aivtx.push_back(pbis->U16Read());
    }
    p->atri.resize(ctri);
    for (int k = 0; k < ctri; ++k)
        for (int i = 0; i < 3; ++i) p->atri[k][i] = pbis->U16Read();
    const int cpt = pbis->U16Read();
    p->apt.resize(cpt);
    for (int i = 0; i < cpt; ++i)
    {
        p->apt[i].x = pbis->F32Read();
        p->apt[i].y = pbis->F32Read();
        p->apt[i].z = pbis->F32Read();
        p->apt[i].c = pbis->U16Read();
        p->apt[i].d = pbis->U16Read();
    }
    // TODO(M3): 0x1C0880
    if (p->oid == 0x2CA) g_psw->ploMesh2CA = p;
    ++g_psw->cmeshes;
}

// orig 0x00185150 (cid 8/9)
void LoadPntsFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x185150);
    LOXPTS* p = static_cast<LOXPTS*>(plo);
    LoadLoxBaseFromBrx(p, pbis);
    const int cv = pbis->S16Read();
    for (int i = 0; i < cv; ++i) p->avec.push_back(pbis->ReadVector());
    const int cx = pbis->S16Read();
    p->ax1.resize(cx > 0 ? cx : 0);
    for (int j = 0; j < cx; ++j)
        for (int k = 0; k < 3; ++k) p->ax1[j][k] = pbis->S16Read();
    p->ax2.resize(cx > 0 ? cx : 0);
    for (int j = 0; j < cx; ++j)
        for (int k = 0; k < 3; ++k) p->ax2[j][k] = pbis->S16Read();
    LoadOptionsFromBrx(p->aopt, p, pbis);
}

// orig 0x00220FD0 (cid 0x0F)
void LoadZoneFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x220FD0);
    LOXGEOM* p = static_cast<LOXGEOM*>(plo);
    LoadLoxBaseFromBrx(p, pbis);
    pbis->ReadGeom(&p->geom);
    pbis->ReadBsp(&p->geom, &p->bsp);
    // TODO(M3): 0x221818
    LoadOptionsFromBrx(p->aopt, p, pbis);
    // TODO(M3): 0x221940
    LoadLoxChildrenFromBrx(p, pbis, &p->aploChild);
}

// orig 0x0015C1E8 (cid 0x1F)
void LoadGrouperFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x15C1E8);
    LOXCHILD* p = static_cast<LOXCHILD*>(plo);
    LoadLoxFromBrx(p, pbis);
    LoadLoxChildrenFromBrx(p, pbis, &p->aploChild);
    if (std::getenv("SLY2N_GROUPERDEBUG"))
        std::printf("grouper %d parent %d: %zu children, the first's parent %d\n", p->oid, p->paloParent ? p->paloParent->oid : -1,
                    p->aploChild.size(), !p->aploChild.empty() && p->aploChild[0]->paloParent ? p->aploChild[0]->paloParent->oid : -1);
    // flag bit 57 on each; PostLoad (slot 9) of those not derived from 0x1E (TODO(M3): its body)
    for (LO* pc : p->aploChild)
        if (!FIsLoDerivedFrom(pc, 0x1E)) RemoveLo(g_psw, pc);
}

// orig 0x00159240 LoadEmitmeshFromBrx
void LoadEmitmeshFromBrx(LOXEMIT::EMITMESH* pem, CBinaryInputStream* pbis)
{
    LOADFN(0x159240);
    const int cpos = pbis->U16Read();
    for (int i = 0; i < cpos; ++i) pem->apos.push_back(pbis->ReadVector());
    const int ctri = pbis->U16Read();
    pem->atri.resize(ctri);
    for (int t = 0; t < ctri; ++t)
    {
        for (int i = 0; i < 3; ++i) pem->atri[t].aipos[i] = pbis->U16Read();
        pem->atri[t].sArea = pbis->F32Read();
    }
    pem->v20 = pbis->ReadVector();
    // TODO(M4): the vertex normals
}

// The emitter's kind (its emitter record +0x10): set by the setter option 483 (emitters, 0x258A00) / 536
// (explosions, 0x259498) when still 0, or by a field option through the record's getter (0x258658 / 0x2595xx).
int EmitterKind(const LO* plo, int eopidSetter, uint32_t pfnGetter)
{
    int kind = static_cast<int>(plo->U32Field(pfnGetter, 0x10, 0));
    for (const OPT& opt : plo->aopt)
        if (opt.eopid == eopidSetter && !opt.aval.empty() && kind == 0) kind = opt.aval[0].n;
    return kind;
}

// orig 0x002584A8 / 0x00258F40: the emitter's colours
void LoadEmitColorsFromBrx(uint32_t fn, LOXEMIT* p, CBinaryInputStream* pbis)
{
    LoadScope scope(fn);
    const int n = pbis->U16Read();
    if (!n) return;
    p->nColors = pbis->U8Read();
    for (int i = 0; i < n; ++i)
    {
        const uint32_t u = pbis->U32Read();
        if (i < 32) p->argba.push_back(u);
    }
}

// orig 0x00258598 (cid 0x1D) / 0x00259030 (cid 0x20)
void LoadEmitterFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x258598);
    LOXEMIT* p = static_cast<LOXEMIT*>(plo);
    LoadLoxFromBrx(p, pbis);
    if (EmitterKind(p, 483, 0x258658) == 3)
    {
        p->pemitmesh = std::make_unique<LOXEMIT::EMITMESH>();
        LoadEmitmeshFromBrx(p->pemitmesh.get(), pbis);
    }
    LoadEmitColorsFromBrx(0x2584A8, p, pbis);
}
void LoadExplFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x259030);
    LOXEMIT* p = static_cast<LOXEMIT*>(plo);
    LoadLoxFromBrx(p, pbis);
    if (EmitterKind(p, 536, 1) == 3)
    {
        p->pemitmesh = std::make_unique<LOXEMIT::EMITMESH>();
        LoadEmitmeshFromBrx(p->pemitmesh.get(), pbis);
    }
    LoadEmitColorsFromBrx(0x258F40, p, pbis);
}

// orig 0x001624A8 (cid 0x84)
void LoadMissionFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1624A8);
    LoadLoFromBrx(plo, pbis);
    pbis->SkipStringSw();
}

// orig 0x001635F8 (cid 0x85)
void LoadTaskFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1635F8);
    LOLIST* p = static_cast<LOLIST*>(plo);
    LoadLoFromBrx(p, pbis);
    const int c94 = pbis->U8Read();
    for (int i = 0; i < c94; ++i) p->an90.push_back(pbis->S32Read());
    const int cA0 = pbis->U8Read();
    for (int i = 0; i < cA0; ++i) p->an9C.push_back(pbis->S32Read());
    pbis->SkipStringSw();
}

// orig 0x001731B8, 0x001DFB10, 0x0014A008: LO loaders with class setup around them
void LoadLo1731B8FromBrx(LO* plo, CBinaryInputStream* pbis) { LOADFN(0x1731B8); LoadLoFromBrx(plo, pbis); }
void LoadLo1DFB10FromBrx(LO* plo, CBinaryInputStream* pbis) { LOADFN(0x1DFB10); BeginScptLoad(plo); LoadLoFromBrx(plo, pbis); EndScptLoad(plo); }  // (scpt.cpp)
void LoadLo14A008FromBrx(LO* plo, CBinaryInputStream* pbis) { LOADFN(0x14A008); LoadLoFromBrx(plo, pbis); }

// orig 0x00153B08 (cid 0x7A)
void LoadLo153B08FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x153B08);
    LOLIST* p = static_cast<LOLIST*>(plo);
    LoadLoFromBrx(p, pbis);
    const int c = pbis->U8Read();
    for (int i = 0; i < c; ++i)
    {
        LOLIST::E e;
        e.str = pbis->ReadStringSw();
        e.n4 = pbis->S8Read();
        e.n8 = pbis->S32Read();
        e.nC = pbis->S32Read();
        pbis->ReadRecord64(e.ab);
        p->ae.push_back(e);
    }
}

// orig 0x001C3DC8 LoadSoFromBrx: ALO + a skipped string
void LoadSoFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1C3DC8);
    LoadAloFromBrx(plo, pbis);
    pbis->SkipStringSw();
}

// orig 0x002039C0 (cid 0x33 family)
void LoadPoFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x2039C0);
    LoadSoFromBrx(plo, pbis);
    // TODO(M3): 0x1A6F00, 0x1F8300
}

// orig 0x001A64E8: the class's named parts (a table {grf, id, ib} in the executable): records by tag (0x10),
// parts by id (0x20 / 0x40, TODO(anim)), else an object found by oid (grf 2: kind 4, 1: global, else among the
// object's own); 0x200 posts it (PostLoad, vt slot 9: it leaves the clonable children). Kept by offset.
void BindLoParts(ALO* palo, int c, uint32_t vaTable)
{
    for (int i = 0; i < c; ++i)
    {
        const uint32_t grf = g_disc.U32Elf(vaTable + 12u * i);
        const OID id = static_cast<OID>(g_disc.U32Elf(vaTable + 12u * i + 4));
        const uint32_t ib = g_disc.U32Elf(vaTable + 12u * i + 8);
        // orig: 0x1A6540..0x1A657C: transform tags are searched from the last record backward.
        if (grf & 0x10)
        {
            for (auto it = palo->axf.rbegin(); it != palo->axf.rend(); ++it)
                if ((*it)->tag == id)
                {
                    auto part = std::find_if(palo->apartxf.begin(), palo->apartxf.end(),
                                             [ib](const auto& p) { return p.first == ib; });
                    if (part != palo->apartxf.end()) part->second = it->get();
                    else palo->apartxf.emplace_back(ib, it->get());
                    break;
                }
            continue;
        }
        // 0x20: a geometry entry by its tag (0x1F6030); with 0x200 it is turned off (0x1F2AD0: e.g. the players' body
        // entries, so that only their custom (probe) entry collides); 0x40 (the local entries):
        // TODO(world)
        if ((grf & 0x20) && (grf & 0x200))
        {
            for (SOE& e : palo->asoe)
                if (e.tag == static_cast<int>(id))
                {
                    SetSoeOff(palo, &e);
                    break;
                }
        }
        if (grf & 0x70) continue;
        LO* plo = nullptr;
        if (grf & 1) plo = PloFindSwObjectByOid(g_psw, id);
        else
        {
            std::function<LO*(LO*)> find = [&](LO* p) -> LO* {
                for (LO* pc : p->aploOwned)
                {
                    if (pc->oid == id) return pc;
                    if (LO* q = find(pc)) return q;
                }
                return nullptr;
            };
            plo = find(palo);
        }
        if (!plo) continue;
        if (grf & 0x200) RemoveLo(g_psw, plo);  // vt slot 9 (TODO: the classes' overrides)
        // (the entry stores the object at the actor's offset ib: a later entry for the same offset overwrites it, e.g.
        // Murray's damage reaction 0 +0xE90: 422 then 649, the original keeps 649)
        auto itPart = std::find_if(palo->apart.begin(), palo->apart.end(), [ib](const auto& e) { return e.first == ib; });
        if (itPart != palo->apart.end() && std::getenv("SLY2N_PARTFIRST")) palo->apart.emplace_back(ib, plo); else if (itPart != palo->apart.end()) itPart->second = plo;
        else palo->apart.emplace_back(ib, plo);
        if (static const char* s_sz = std::getenv("SLY2N_PARTPRINT"); s_sz && std::atoi(s_sz) == palo->oid)  // (debug: an object's parts)
            std::printf("part: %d +0x%X -> %d (cid 0x%X) grf 0x%X\n", palo->oid, ib, plo->oid, plo->Cid(), grf);
    }
}

// Loaders that are their base's plus non-reading setup.
#define LOADER_WRAP(addr, base)                                     \
    void Load_##addr(LO* plo, CBinaryInputStream* pbis)             \
    {                                                               \
        LOADFN(0x##addr);                                           \
        base(plo, pbis);                                            \
    }
// Bentley (0x134770), Sly (0x1885B8), Murray (0x1B4888): the PO, then their parts (0x1A64E8)
void Load_134770(LO* plo, CBinaryInputStream* pbis) { LOADFN(0x134770); LoadPoFromBrx(plo, pbis); BindLoParts(static_cast<ALO*>(plo), 0x76, 0x2BC390); }
void Load_1885B8(LO* plo, CBinaryInputStream* pbis) { LOADFN(0x1885B8); LoadPoFromBrx(plo, pbis); BindLoParts(static_cast<ALO*>(plo), 0xAD, 0x2BE990); }
void Load_1B4888(LO* plo, CBinaryInputStream* pbis) { LOADFN(0x1B4888); LoadPoFromBrx(plo, pbis); BindLoParts(static_cast<ALO*>(plo), 0x89, 0x2C0270); }
LOADER_WRAP(1553F0, LoadPoFromBrx)
LOADER_WRAP(13BE58, LoadAloFromBrx)
LOADER_WRAP(148C08, LoadAloFromBrx)
LOADER_WRAP(1B0CA8, LoadAloFromBrx)
void Load_1B1E00(LO* plo, CBinaryInputStream* pbis) { LOADFN(0x1B1E00); LoadAloFromBrx(plo, pbis); BindLoParts(static_cast<ALO*>(plo), 1, 0x2C0258); }
// Breakables (cid 0x62, 0x1BABA0): the init's contact bytes, the ALO, the first event current (breakable.cpp)
void Load_1BABA0(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1BABA0);
    InitBreakableBytes(static_cast<ALO*>(plo));
    LoadAloFromBrx(plo, pbis);
    PostLoadBreakable(plo);
}
LOADER_WRAP(223FE0, LoadAloFromBrx)
#undef LOADER_WRAP

// orig 0x001C4EF8 (cid 0x5A family): the ALO, then its 0x12 child (0x1F7B98, +0xF0 = 1), TODO(M3): 0x2303A0 on
// +0x58 / +0x5C, 0x1A64E8
void Load_1C4EF8(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1C4EF8);
    LoadAloFromBrx(plo, pbis);
    PloEnsureChild391(static_cast<ALO*>(plo));
    BindLoParts(static_cast<ALO*>(plo), 1, 0x2C0D40);
}
void Load_1C76D0(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1C76D0);
    Load_1C4EF8(plo, pbis);
}

template <class T> LO* PloNewT() { return new T(); }
}

// orig 0x001F7B98: the object's child of class 0x12 with oid 391 (0x187), made when it has none
LO* PloEnsureChild391(ALO* palo)
{
    for (LO* pc : palo->aploChild)
        if (pc->oid == 0x187) return pc;
    auto* point = static_cast<LOX*>(PloNew(0x12, g_psw, palo, 0x187, -1));
    // orig: 0x231100 (LOX InitLo) inherits its parent's object transform;
    // 0x21D278 supplies the point defaults before the caller's field options.
    point->pxf = PxfObj(palo);
    point->SetField(0, 0xE8, 0x17E);
    float radius = 25.0f;
    uint32_t bits;
    std::memcpy(&bits, &radius, sizeof(bits));
    point->SetField(0, 0xEC, bits);
    point->SetField(0, 0xF4, 0xFFFFFFFFu);
    return point;
}

// orig 0x001C9B08: an instance of a shared group (classes derived from PROXY, cid 6): either a new object of
// the class (the instance's own) or the group's objects cloned for it, with the instance's options and objects.
void LoadProxyFromBrx(SW* psw, ALO* paloParent, CID cid, OID oid, int iframe, CBinaryInputStream* pbis, LOSINK* psink)
{
    LOADFN(0x1C9B08);
    (void)iframe;
    const int32_t idGroup = pbis->S32Read();
    const int mode = pbis->S8Read();
    XF* pxf = PxfFromRef(pbis->S16Read(), paloParent, nullptr);
    const glm::mat4 mat = pbis->ReadMatrix4();
    const uint32_t grfLoad = pbis->U32Read();
    int iwe = -1;
    for (int i = 0; i < static_cast<int>(psw->awe.size()); ++i)
        if (psw->awe[i].x10 == idGroup) { iwe = i; break; }
    // orig 0x1CB770 searches the groups' +0x10 id
    const bool fNew = mode == 1 || (mode == -1 && iwe >= 0 && psw->awe[iwe].x60 != 0);
    if (fNew)
    {
        LO* plo = PloNew(cid, psw, paloParent, oid, -1);
        LOX* plox = static_cast<LOX*>(plo);
        plox->pxf = pxf;
        plox->mat = mat;
        plox->grfLoad = grfLoad;
        SetLoMask(psw, plox, grfLoad);  // orig 0x1A6D28
        // a streaming proxy (activate.cpp AttachProxy): its group's instances are attached while it is active;
        // its radius is the group's last object's
        PROXYSTREAM& ps = psw->mpproxy[plo];
        ps.iwe = iwe;
        if (iwe >= 0)
        {
            const WE& we = psw->awe[iwe];
            const std::size_t c = static_cast<std::size_t>(we.cloLoaded) * static_cast<std::size_t>(we.cinst);
            if (we.cloLoaded > 0 && c > 0 && c <= we.aplo.size()) plo->sRadius = we.aplo[c - 1]->sRadius;
        }
        const int cbind = pbis->U8Read();
        for (int i = 0; i < cbind; ++i)
        {
            const int32_t kind = pbis->S32Read();
            const int type = pbis->S16Read();
            const int iframeBind = pbis->S16Read();
            const auto pframe = iframeBind != -1 ? PframeFromIsplice(iframeBind, psw) : nullptr;  // orig: 0x001FFA40 (bind +0x08)
            if (std::getenv("SLY2N_BINDPRINT"))
                std::printf("bind %d kind %d type 0x%X frame %d (block %d)\n", plo->oid, kind, type, iframeBind, IspliceBlockCur(psw));
            if (kind == 5)
            {
                plo->pframe = pframe;  // orig: 0x001C9B08 / 0x00256EE0: assign even a null frame before options
                const int cb = pbis->U16Read();
                if (FIsLoDerivedFrom(plo, type)) LoadOptionsFromBrx(plo->aopt, plo, pbis);
                else pbis->Skip(cb);
            }
            else
            {
                // the options for the instance's object of oid kind (6: the instance's first), applied at the attach
                const int cb = pbis->U16Read();
                PROXYBIND bind;
                bind.kind = kind;
                bind.type = type;
                bind.pframe = pframe;
                bind.ab.resize(static_cast<std::size_t>(cb));
                pbis->Read(static_cast<std::size_t>(cb), bind.ab.data());
                ps.abind.push_back(std::move(bind));
            }
        }
        const int32_t cbRest = pbis->S32Read();
        pbis->S16Read();
        pbis->Skip(static_cast<std::size_t>(cbRest - 2));
        if (psink) psink->Append(plo);
        return;
    }
    // orig 0x1CA370: the group's next unused instance (its objects were loaded, or cloned, by the world loader):
    // its objects get the instance's key (slot 25), the parent (slot 10) and the matrix (slot 37)
    std::vector<LO*> aploFirst;
    if (iwe >= 0)
    {
        WE& we = psw->awe[iwe];
        const std::size_t ib = static_cast<std::size_t>(we.cUsed) * we.cloLoaded;
        if (ib + we.cloLoaded > we.aplo.size())
            throw std::runtime_error("proxy: group " + we.StrName() + " has no instance left");
        for (int j = 0; j < we.cloLoaded; ++j)
        {
            LO* plo = we.aplo[ib + j];
            AddLo(psw, plo);  // vt slot 8 (TODO(M3): slot 25, the key)
            if (plo->paloParent != paloParent) SetLoParentActivation(psw, plo, paloParent);  // vt slot 10
            SetLoChannel(plo, pxf);  // slot 10 (a, b): parent and channel
            // orig: 0x001CA370 calls slot 37 for each instance. Placed guards retain that matrix in record 0
            // (Recon's rats123856..123859); skipping every STEP left them at the world origin.
            // The existing player load-order approximation remains: their later body load replaces the records
            // in the original, leaving the root identity before checkpoint placement. Their matrix
            // is kept: with no checkpoint for the point (worlds 20 / 26 by `--world`) the reference leaves them at
            // their instances' places ((-15342, 470) / (-25208, -5526)): the transition applies it then.
            if (!FIsLoDerivedFrom(plo, 0x34)) SetLoMatrix(plo, mat);
            else psw->mpmatPlayerInstance[plo] = mat;
            aploFirst.push_back(plo);
            if (LO* ploOwner = PloLoading()) ploOwner->aploOwned.push_back(plo);  // in the loading object's namespace
        }
        ++we.cUsed;
        if (!aploFirst.empty())
        {
            const OID oidOld = aploFirst[0]->oidInstance;
            aploFirst[0]->oidInstance = oid;
            NoteOidInstanceChanged(psw, aploFirst[0], oidOld);
        }
        if (std::getenv("SLY2N_PROXYLOAD"))
            for (LO* p : aploFirst)
                std::printf("proxy %d: instance object %d (cid 0x%X) mask 0x%X, the record's 0x%X; mat t (%.1f %.1f %.1f) x (%.3f %.3f %.3f) pxf %p parent %d\n",
                            oid, p->oid, p->Cid(), p->bMask, grfLoad, mat[3][0], mat[3][1], mat[3][2], mat[0][0], mat[0][1], mat[0][2], (void*)pxf,
                            paloParent ? paloParent->oid : -1);
        // (the gang's (cid 0x34..0x37) mask comes from its body read after: its own uMeta, as the reference's gang's
        // 0xFF; the guards' (0x38..) is the record's, as the reference's: the intro's 8438 0x10, 12712 0xFF, the
        // seven 11539s 0x60 / 0x30 / 0x30 / 0x60 / 0xE0 / 0xC0 / 0xC0)
        for (LO* p : aploFirst)
            if (!FIsLoDerivedFrom(p, 0x34)) SetLoMask(psw, p, grfLoad);  // orig 0x1A6D28 (TODO: not those with bit 58)  // orig: the instance's namespace node (0x1C99D8)
    }
    const int c = pbis->U8Read();
    for (int i = 0; i < c; ++i)
    {
        const int32_t kind = pbis->S32Read();
        const int type = pbis->S16Read();
        LO* ploFound = nullptr;
        if (kind == 5 || kind == 6)
        {
            if (!aploFirst.empty()) ploFound = aploFirst[0];
        }
        else
        {
            // (orig: 0x15E678 kind 5 in the instance's namespace: the instance's objects and their descendants)
            auto find = [&](auto&& self, LO* p) -> LO* {
                if (p->oid == kind || p->oidInstance == kind) return p;
                for (LO* pc : p->aploOwned)
                    if (LO* q = self(self, pc)) return q;
                return nullptr;
            };
            for (LO* p : aploFirst)
                if ((ploFound = find(find, p))) break;
        }
        if (ploFound && !FIsLoDerivedFrom(ploFound, type)) ploFound = nullptr;
        const int iframeBind = pbis->S16Read();
        if (std::getenv("SLY2N_BINDPRINT"))
            std::printf("proxy %d bind kind %d type 0x%X frame %d -> %d (cid 0x%X)\n", oid, kind, type, iframeBind,
                        ploFound ? ploFound->oid : -1, ploFound ? ploFound->Cid() : 0);
        if (ploFound && iframeBind != -1)
        {
            // orig: 0x001C9B08 / 0x001FFA40, 0x00127B40, 0x00256EE0: the bind frame inherits the old
            // object's frame, then replaces it before LoadOptionsFromBrx.
            auto pframe = PframeFromIsplice(iframeBind, psw);
            if (pframe && ploFound->pframe) pframe->AddParent(ploFound->pframe.get());
            ploFound->pframe = std::move(pframe);
        }
        const int cb = pbis->U16Read();
        if (ploFound) LoadOptionsFromBrx(ploFound->aopt, ploFound, pbis);
        else pbis->Skip(cb);
    }
    std::vector<LO*> aploSecond;
    pbis->S32Read();
    PushXfBase(pxf);
    LOSINK sink;
    sink.papo = &aploSecond;
    LoadSwObjsFromBrx(psw, paloParent, pbis, &sink);
    PopXfBase();
    // the instance's objects and the record's own objects loaded after it (e.g. a bounce pad's landing locator 16758)
    // share the instance's namespace node (orig +0x3C), which 0x15E3E8 checks for kind 4 lookups
    if (!aploFirst.empty())
    {
        for (LO* p : aploFirst) p->ploNs = aploFirst[0];
        for (LO* p : aploSecond) p->ploNs = aploFirst[0];
    }
    for (LO* p : aploSecond)
    {
        SetLoMatrix(p, mat);  // slot 37 (TODO(M3): slot 25 key)
        SetLoMask(psw, p, grfLoad);  // orig 0x1A6D28 (TODO: not those with bit 58)
    }
    if (psink)
    {
        for (LO* p : aploFirst) psink->Append(p);
        for (LO* p : aploSecond) psink->Append(p);
    }
}

void RegisterLoaders(std::unordered_map<uint32_t, PFNLOAD>& m)
{
    m[0x1A5C28] = LoadLoFromBrx;
    m[0x231170] = LoadLoxFromBrx;
    m[0x227380] = LoadVolumeFromBrx;
    m[0x227BB8] = LoadShapeFromBrx;
    m[0x149358] = LoadShape149358FromBrx;
    m[0x2282B0] = LoadShapeKeysFromBrx;
    m[0x228140] = LoadLoxName228140FromBrx;
    m[0x228720] = LoadLoxName228720FromBrx;
    m[0x12EB78] = LoadLox12EB78FromBrx;
    m[0x1BEA88] = LoadLox1BEA88FromBrx;
    m[0x2035A8] = LoadLox2035A8FromBrx;
    m[0x1E5DF8] = LoadPathFromBrx;
    m[0x1BC088] = LoadPath1BC088FromBrx;
    m[0x1C03C0] = LoadMeshZoneFromBrx;
    m[0x185150] = LoadPntsFromBrx;
    m[0x220FD0] = LoadZoneFromBrx;
    m[0x15C1E8] = LoadGrouperFromBrx;
    m[0x258598] = LoadEmitterFromBrx;
    m[0x259030] = LoadExplFromBrx;
    m[0x1624A8] = LoadMissionFromBrx;
    m[0x1635F8] = LoadTaskFromBrx;
    m[0x1731B8] = LoadLo1731B8FromBrx;
    m[0x1DFB10] = LoadLo1DFB10FromBrx;
    m[0x14A008] = LoadLo14A008FromBrx;
    m[0x153B08] = LoadLo153B08FromBrx;
    m[0x1F31E0] = LoadAloFromBrx;
    m[0x1C3DC8] = LoadSoFromBrx;
    m[0x2039C0] = LoadPoFromBrx;
    m[0x134770] = Load_134770;
    m[0x1885B8] = Load_1885B8;
    m[0x1B4888] = Load_1B4888;
    m[0x1553F0] = Load_1553F0;
    m[0x13BE58] = Load_13BE58;
    m[0x148C08] = Load_148C08;
    m[0x1B0CA8] = Load_1B0CA8;
    m[0x1B1E00] = Load_1B1E00;
    m[0x1BABA0] = Load_1BABA0;
    m[0x1DE478] = LoadLaserFromBrx;
    m[0x223FE0] = Load_223FE0;
    m[0x1C4EF8] = Load_1C4EF8;
    m[0x1C76D0] = Load_1C76D0;
    RegisterAsegLoaders(m);
}

// The native struct of each class: that of its nearest ancestor with one.
LO* PloNewCmdlist();  // ai.cpp (the command lists, cid 0x71)
LO* PloNewChasegraph();  // aichase.cpp (Carmelita's chase graphs, cid 0x7B)
PFNNEW PfnNewNativeForCid(CID cid)
{
    static const std::unordered_map<int, PFNNEW> s_map = {
        {0x02, PloNewT<LO>},       {0x03, PloNewT<LOX>},      {0x05, PloNewT<LOXGEOM>}, {0x0F, PloNewT<LOXGEOM>},
        {0x0A, PloNewT<LOXPATH>},  {0x16, PloNewT<LOXSHAPE>}, {0x11, PloNewT<LOXMESH>}, {0x08, PloNewT<LOXPTS>},
        {0x1D, PloNewT<LOXEMIT>},  {0x20, PloNewT<LOXEMIT>},  {0x1F, PloNewT<LOXCHILD>}, {0x30, PloNewT<ALO>},
        {0x85, PloNewT<TASK>},     {0x7A, PloNewT<LOLIST>},   {0x84, PloNewT<JOB>},     {0x2A, PloNewT<CHKPNT>},
        {0x71, PloNewCmdlist},     {0x69, PloNewT<TRAIL>},    {0x7B, PloNewChasegraph}, {0x73, PloNewT<WR>},
    };
    for (CID c = cid; c >= 0; c = g_acls[c].cidBase)
    {
        auto it = s_map.find(c);
        if (it != s_map.end()) return it->second;
        if (auto f = PfnNewAseg(c)) return f;
    }
    return nullptr;
}

// orig 0x001A64E8 for the classes' post-loads (the actors' 0x1400B8 / 0x188608: sly.cpp)
void BindLoPartsTable(ALO* palo, int c, uint32_t vaTable) { BindLoParts(palo, c, vaTable); }
