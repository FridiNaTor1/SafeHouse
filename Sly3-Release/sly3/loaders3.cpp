// Sly 3's class loaders (vt slot 14 of the classes of the table 0x3409E0), bound by their Sly 3 addresses
// (RegisterLoaders3, cls.cpp), and the native struct of each class (PfnNewNativeForCid3). Each is ported from the Sly 3
// asm (orig3) with the Sly 2 twin's native code (Sly2-Release loaders.cpp / alo.cpp, "twin") as the draft; a loader
// whose twin's calls are the same (research tools/calldiff.py) keeps the twin's reads. Every top-level object of the
// 39 worlds must read exactly its record's size (load3.cpp checks it), which checks every loader below it.
//
// Not ported here (TODO(sly3)): the non-reading setup of the loaders (allocations, defaults, links, BindLoParts'
// part tables 0x1C8970, SetXfModes ...): the classes' behaviour, for the code that ports it.

#include "load3.h"
#include "aseg3.h"
#include "condset3.h"

#include "addr.h"
#include "../glob.h"
#include "ids3.h"

#include "../alo.h"
#include "../bis.h"
#include "../cls.h"
#include "../disc.h"
#include "../sw.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace
{
SW* Psw() { return g_psw3; }

// ---------------------------------------------------------------- LO / LOX

// orig3: 0x1C7D98 LoadLoFromBrx (twin 0x1A5C28)
void LoadLoFromBrx3(LO* plo, CBinaryInputStream* pbis) { LoadOptionsFromBrx3(plo->aopt, plo, pbis); }

// orig3: 0x290438 LoadLoxBaseFromBrx (twin 0x231768): the transform reference (vt slot 53, +0x40), the matrix
// (+0x50), TouchLox 0x290078, the load flags (0x1C9198: the mask)
void LoadLoxBaseFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOX* plox = static_cast<LOX*>(plo);
    plox->ixfRef = pbis->S16Read();
    plox->pxf = PxfFindLox(plox, plox->ixfRef);
    plox->mat = pbis->ReadMatrix4();
    plox->grfLoad = pbis->U32Read();
}

// orig3: 0x28FDE0 LoadLoxFromBrx (twin 0x231170)
void LoadLoxFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadLoxBaseFromBrx3(plo, pbis);
    LoadOptionsFromBrx3(plo->aopt, plo, pbis);
}

// orig3: 0x2904B0 LoadLoxChildrenFromBrx (twin 0x2317E0): objects of the LOX's parent, placed by the LOX's matrix
void LoadLoxChildrenFromBrx3(LOX* plox, CBinaryInputStream* pbis, std::vector<LO*>* papo)
{
    PushXfBase(plox->pxf);
    LOSINK3 sink;
    sink.papo = papo;
    LoadSwObjsFromBrx3(Psw(), plox->paloParent, pbis, &sink);
    PopXfBase();
    if (papo)
        for (LO* plo : *papo) SetLoMatrix3(plo, plox->mat);  // vt slot 42
}

// orig3: 0x27E918 (VOLUME 0x07; twin 0x227380)
void LoadVolumeFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOXGEOM* p = static_cast<LOXGEOM*>(plo);
    LoadLoxBaseFromBrx3(p, pbis);
    p->u = pbis->U32Read();
    pbis->ReadGeom(&p->geom);
    pbis->ReadBsp(&p->geom, &p->bsp);
    LoadOptionsFromBrx3(p->aopt, p, pbis);
}

// orig3: 0x27F158 (SHAPE 0x13; twin 0x227BB8; + 0x119C90, no reads)
void LoadShapeFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOXSHAPE* p = static_cast<LOXSHAPE*>(plo);
    LoadLoxFromBrx3(p, pbis);
    pbis->ReadGeom(&p->geom);
    pbis->ReadBsp(&p->geom, &p->bsp);
}
// orig3: 0x15CCB0 (0x15, 0x17; twin 0x1DCCA0)
void LoadShape15CCB0FromBrx3(LO* plo, CBinaryInputStream* pbis) { LoadShapeFromBrx3(plo, pbis); }

// orig3: 0x27FB30 (0x18; twin 0x2282B0): the shape, a 64-byte key (ReadCb64), U8 c more keys
void LoadShapeKeysFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOXSHAPE* p = static_cast<LOXSHAPE*>(plo);
    LoadShapeFromBrx3(p, pbis);
    std::array<uint8_t, 64> ab{};
    pbis->ReadRecord64(ab.data());
    p->aabKey.push_back(ab);
    const int c = pbis->U8Read();
    for (int i = 0; i < c; ++i)
    {
        pbis->ReadRecord64(ab.data());
        p->aabKey.push_back(ab);
    }
}

// orig3: 0x27FA00 (0x27; twin 0x228140) / 0x27FFE0 (0x28; twin 0x228720): the LOX + a skipped string
void LoadLoxNameFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadLoxFromBrx3(plo, pbis);
    pbis->SkipStringSw();
}

// orig3: 0x22D780 (PATH 0x09..0x0C; twin 0x1E5DF8)
void LoadPathFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOXPATH* p = static_cast<LOXPATH*>(plo);
    LoadLoxBaseFromBrx3(p, pbis);
    p->fE0 = pbis->S8Read();
    p->nE4 = pbis->S8Read();
    const int c = pbis->U16Read();
    p->an.resize(std::size_t(c));
    pbis->Read(std::size_t(c) * 4, p->an.data());
    for (int i = 0; i < c; ++i) p->avecF4.push_back(pbis->ReadVector());
    if (p->fE0)
    {
        for (int i = 0; i < c; ++i) p->avec140.push_back(pbis->ReadVector());
        for (int i = 0; i < c; ++i) p->avec144.push_back(pbis->ReadVector());
    }
    LoadOptionsFromBrx3(p->aopt, p, pbis);
}

// orig3: 0x1ECC58 (MESHZONE 0x10; twin 0x1C03C0)
void LoadMeshZoneFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOXMESH* p = static_cast<LOXMESH*>(plo);
    LoadLoxFromBrx3(p, pbis);
    const int cvtx = pbis->U16Read();
    const int ctri = pbis->U16Read();
    p->avtx.resize(std::size_t(cvtx));
    for (auto& vtx : p->avtx)
    {
        vtx.pos = pbis->ReadVector();
        const int c = pbis->U8Read();
        for (int j = 0; j < c; ++j) vtx.aivtx.push_back(pbis->U16Read());
    }
    p->atri.resize(std::size_t(ctri));
    for (auto& tri : p->atri)
        for (int& i : tri) i = pbis->U16Read();
    const int cpt = pbis->U16Read();
    p->apt.resize(std::size_t(cpt));
    for (auto& pt : p->apt)
    {
        pt.x = pbis->F32Read();
        pt.y = pbis->F32Read();
        pt.z = pbis->F32Read();
        pt.c = pbis->U16Read();
        pt.d = pbis->U16Read();
    }
}

// orig3: 0x273CF8 (ZONE 0x0E; twin 0x220FD0)
void LoadZoneFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOXGEOM* p = static_cast<LOXGEOM*>(plo);
    LoadLoxBaseFromBrx3(p, pbis);
    pbis->ReadGeom(&p->geom);
    pbis->ReadBsp(&p->geom, &p->bsp);
    LoadOptionsFromBrx3(p->aopt, p, pbis);
    LoadLoxChildrenFromBrx3(p, pbis, &p->aploChild);
}

// orig3: 0x179EA8 (GROUPER 0x1D; twin 0x15C1E8): the LOX, its children (flag 57 and PostLoad on those not derived
// from 0x1E: TODO(sly3))
void LoadGrouperFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOXCHILD* p = static_cast<LOXCHILD*>(plo);
    LoadLoxFromBrx3(p, pbis);
    LoadLoxChildrenFromBrx3(p, pbis, &p->aploChild);
}

// orig3: 0x1775B8 LoadEmitmeshFromBrx (twin 0x159240)
void LoadEmitmeshFromBrx3(LOXEMIT::EMITMESH* pem, CBinaryInputStream* pbis)
{
    const int cpos = pbis->U16Read();
    for (int i = 0; i < cpos; ++i) pem->apos.push_back(pbis->ReadVector());
    const int ctri = pbis->U16Read();
    pem->atri.resize(std::size_t(ctri));
    for (auto& tri : pem->atri)
    {
        for (int& i : tri.aipos) i = pbis->U16Read();
        tri.sArea = pbis->F32Read();
    }
    pem->v20 = pbis->ReadVector();
}

// The emitter's kind (its EMITB +0xD4's +0x10): the setter option 617 (emitters, SetPositionKind 0x2BCE58) / 670
// (explosions, 0x2BD8C8) when still 0, or a field option through the EMITB's getter (PimgEmitb 0x2BCAD8)
int EmitterKind3(const LO* plo, int eopidSetter)
{
    int kind = static_cast<int>(plo->U32Field(0x2BCAD8, 0x10, 0));
    for (const OPT& opt : plo->aopt)
        if (opt.eopid == eopidSetter && !opt.aval.empty() && kind == 0) kind = opt.aval[0].n;
    return kind;
}

// orig3: 0x2BC928 / 0x2BD398 LoadEmitColorsFromBrx (twins 0x2584A8 / 0x258F40)
void LoadEmitColorsFromBrx3(LOXEMIT* p, CBinaryInputStream* pbis)
{
    const int n = pbis->U16Read();
    if (!n) return;
    p->nColors = pbis->U8Read();
    for (int i = 0; i < n; ++i)
    {
        const uint32_t u = pbis->U32Read();
        if (i < 32) p->argba.push_back(u);
    }
}

// orig3: 0x2BCA18 (EMITTER 0x1B; twin 0x258598) / 0x2BD488 (0x1F; twin 0x259030)
void LoadEmitterBodyFromBrx3(LO* plo, CBinaryInputStream* pbis, int eopidKind)
{
    LOXEMIT* p = static_cast<LOXEMIT*>(plo);
    LoadLoxFromBrx3(p, pbis);
    if (EmitterKind3(p, eopidKind) == 3)
    {
        p->pemitmesh = std::make_unique<LOXEMIT::EMITMESH>();
        LoadEmitmeshFromBrx3(p->pemitmesh.get(), pbis);
    }
    LoadEmitColorsFromBrx3(p, pbis);
}
void LoadEmitterFromBrx3(LO* plo, CBinaryInputStream* pbis) { LoadEmitterBodyFromBrx3(plo, pbis, 617); }
void LoadExplFromBrx3(LO* plo, CBinaryInputStream* pbis) { LoadEmitterBodyFromBrx3(plo, pbis, 670); }

// orig3: 0x1FE270 (0x2D, new in Sly 3, ~1,500 live in m_ext): the LOX; lists by +0xD8 (0: +0xE8, 2: +0xF4); then
// with the transform base +0x40 pushed, S16 c items, each with an object header (S8 mode, mode 1: U8 n1, U8 n2,
// filters, conditions; S16 cid, S32 oid, S16 n, S32 cb): an item that passes its filter / conditions and whose cid
// is 0xBF (0x1FD758) or 0xC0 (0x1FDE70) is made (a 0x20-byte record, not a world object) and loaded through its
// interface (vt +0x20, then the object's slot 7 with pbis), appended at +0xD4 (count +0xD0); the others are skipped by
// cb. An item's load (0x2BE8F8: its loader object from vt +0x20 = 0x1FD988 / 0x1FDEE8, then that object's slot 7:
// 0x1FD9D8 -> 0x1FD198) is an option list (LoadOptionsFromBrx 0x1519C8) on the item's records: load3.h LOPE3.
void Load2DFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOPE3* ppe = static_cast<LOPE3*>(plo);
    LoadLoxFromBrx3(ppe, pbis);
    PushXfBase(ppe->pxf);
    ++g_pe3stats.cpe;
    const int c = pbis->S16Read();
    for (int i = 0; i < c; ++i)
    {
        const int mode = pbis->S8Read();
        std::vector<int16_t> afilter;
        std::vector<int32_t> acond;
        if (mode == 1)
        {
            afilter.resize(pbis->U8Read());
            acond.resize(pbis->U8Read());
            for (int16_t& f : afilter) f = pbis->S16Read();
            for (int32_t& k : acond) k = pbis->S32Read();
        }
        PECOMP3 comp;
        comp.cid = pbis->S16Read();
        comp.oid = pbis->S32Read();
        comp.n = pbis->S16Read();
        const int32_t cb = pbis->S32Read();
        const bool fPass = mode != 1 || (FFilterMatches3(int(afilter.size()), afilter.data()) &&
                                         FConditionsMatch3(int(acond.size()), acond.data()));
        if (!fPass || (comp.cid != 0xBF && comp.cid != 0xC0))
        {
            ++ppe->cskipped;
            ++g_pe3stats.cfiltered;
            pbis->Skip(std::size_t(cb));
            continue;
        }
        const std::size_t ibStart = pbis->GetOffset();
        LoadOptionsFromBrx3(comp.aopt, nullptr, pbis);
        // orig3: 0x1FCF10: the emission mesh when the position kind (record +0x10's +0x48, setter EOPID 558) is 3
        // (the format of the emitters' 0x1775B8 LoadEmitmeshFromBrx)
        int kPos = 0;
        for (const OPT& o : comp.aopt)
            if (o.eopid == 558 && !o.aval.empty()) kPos = o.aval[0].n;
        if (kPos == 3)
        {
            comp.pmesh = std::make_shared<LOXEMIT::EMITMESH>();
            LoadEmitmeshFromBrx3(comp.pmesh.get(), pbis);
        }
        if (pbis->GetOffset() != ibStart + std::size_t(cb))
        {
            ++g_pe3stats.cbad;   // (the object's size check reports it)
            if (std::getenv("SLY3_PEDBG"))
            {
                std::fprintf(stderr, "pe oid %d comp cid 0x%X oid %d: read %zu of %d:", ppe->oid, comp.cid, comp.oid,
                             pbis->GetOffset() - ibStart, cb);
                for (const OPT& o : comp.aopt) std::fprintf(stderr, " %d(k%d=%d)", o.eopid, o.aval.empty() ? -1 : o.aval[0].k, o.aval.empty() ? 0 : o.aval[0].n);
                std::fprintf(stderr, "\n");
            }
        }
        ++g_pe3stats.ccomp;
        ++(comp.cid == 0xBF ? g_pe3stats.ccompBF : g_pe3stats.ccompC0);
        ppe->acomp.push_back(std::move(comp));
    }
    PopXfBase();
}

// orig3: 0x238FF8 (SM 0x84; twin 0x1EFAC0): the states and animations are objects of the SM's parent (0x151D80 with
// the SM's +0x10 into a list; vt slot 9 on each: posted), then U8 x {S32, S32} (+0x40 / +0x44: the states; 0x239AB8
// resolves the second unless -1 / 0x3B), U8 x {S32 from, S32 to (0x239AB8), S32, S32, F32, S32} (+0x48 / +0x4C: the
// transitions), the options. TODO(sly3): the state machine (sm.cpp is Sly 2's).
void LoadSmFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    SM* p = static_cast<SM*>(plo);
    LOSINK3 sink;
    sink.papo = &p->aploChild;
    LoadSwObjsFromBrx3(Psw(), plo->paloParent, pbis, &sink);
    for (LO* pc : p->aploChild) pc->fRemoved = true;  // vt slot 9
    const int cstate = pbis->U8Read();
    for (int i = 0; i < cstate; ++i)
    {
        const int32_t id = pbis->S32Read();
        const int32_t next = pbis->S32Read();
        p->anode.emplace_back(id, next);
    }
    // the transitions as Sly 2's 0x1EFAC0 keeps them (smrun.cpp InitSm builds the tables): an edge whose ASEG was
    // not loaded is dropped
    const int ctrans = pbis->U8Read();
    for (int i = 0; i < ctrans; ++i)
    {
        SM::EDGE e{};
        e.from = pbis->S32Read();
        e.to = pbis->S32Read();
        e.grf = pbis->S32Read();
        e.value = pbis->S32Read();
        e.weight = pbis->F32Read();
        const int32_t oid = pbis->S32Read();
        if (oid != -1)
        {
            for (LO* pc : p->aploChild)
                if (pc->oid == oid) { e.plo = pc; break; }
            if (!e.plo) continue;
        }
        p->aedge.push_back(e);
    }
    LoadOptionsFromBrx3(plo->aopt, plo, pbis);
}

// orig3: 0x283150 (0xAA, new): the LO, a debug string (0x119830: U16-counted bytes), then NProgress of it (no reads)
void LoadAAFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadLoFromBrx3(plo, pbis);
    pbis->SkipStringSw();
}

// orig3: 0x16F768 (0x8F, new): the LO, U8 c x {S32, S8, S8, S32, S32, a 64-byte key (ReadCb64)} (+0x40 count)
void Load8FFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadLoFromBrx3(plo, pbis);
    const int c = pbis->U8Read();
    for (int i = 0; i < c; ++i)
    {
        pbis->S32Read();
        pbis->S8Read();
        pbis->S8Read();
        pbis->S32Read();
        pbis->S32Read();
        uint8_t ab[64];
        pbis->ReadRecord64(ab);
    }
}

// orig3: 0x260810 (the screen objects 0x9E..0xA7): load3.h LOSCREEN3, the body by screen3.cpp's LoadScreenObj3
void LoadScreenObjFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOSCREEN3* p = static_cast<LOSCREEN3*>(plo);
    LoadLoFromBrx3(plo, pbis);
    const std::size_t ibHead = pbis->m_ib;
    p->so.cid = plo->Cid();
    p->so.oid = plo->oid;
    LoadScreenObj3(p->so, pbis);   // orig3: 0x260810's reads, 0x263118, 0x2631C8
    p->fExternal = p->so.fExternal;
    p->g80 = p->so.g80;
    p->ul84 = p->so.ul84;
    p->ul88 = p->so.ul88;
    p->ulAC = p->so.ulAC;
    if (p->fExternal) std::copy(p->so.abKey.begin(), p->so.abKey.end(), p->abKey.begin());
    else p->ibElems = ibHead + 1 + 4 * 4;
}

// orig3: 0x16EE98 (class 0x97): load3.h LO97
void Load97FromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LO97* p = static_cast<LO97*>(plo);
    LoadLoFromBrx3(plo, pbis);
    p->aentry.resize(std::size_t(std::max(0, pbis->S32Read())));
    for (LO97::ENTRY& e : p->aentry)
    {
        pbis->ReadRecord64(e.abKey.data());
        e.a = pbis->S32Read();
        e.b = pbis->S32Read();
    }
    p->aitem.resize(std::size_t(std::max(0, pbis->S32Read())));
    for (LO97::ITEM& it : p->aitem)
    {
        it.kind = pbis->S8Read();
        if (it.kind == 1)
        {
            it.a = pbis->S32Read();
            it.b = pbis->S32Read();
        }
        else if (it.kind == 0 || it.kind == 2) it.a = pbis->S32Read();
    }
}

// orig3: 0x2250C0 (SCPT 0x87): LoadLoFromBrx, the command list (0x225220: scpt3.cpp)
void LoadScptFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadLoFromBrx3(plo, pbis);
    // (the command reader is a draft, 5,800 of 5,863 scripts exact: one that does not end at the body's end is
    // dropped and the body skipped, so that the world still loads; --load-world counts them)
    const std::size_t ib = pbis->m_ib;
    try
    {
        static_cast<LOSCPT3*>(plo)->acmd = AcmdLoadScpt3(pbis);
    }
    catch (const std::exception&)
    {
        pbis->m_ib = g_ibBodyEnd3 + 1;  // (flagged below)
    }
    if (pbis->m_ib != g_ibBodyEnd3)
    {
        static_cast<LOSCPT3*>(plo)->acmd.clear();
        ++g_load3stats.cscptMisread;
        pbis->m_ib = std::max(ib, g_ibBodyEnd3);
    }
}

// ---------------------------------------------------------------- missions, tasks

// orig3: 0x185EB8 (MISSION 0xA8; twin 0x1624A8): the LO, a skipped string
void LoadMissionFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadLoFromBrx3(plo, pbis);
    pbis->SkipStringSw();
}

// orig3: 0x187F08 (TASK 0xA9; twin 0x1635F8): the LO, U8 x S32, U8 x S32, a skipped string
void LoadTaskFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LOLIST* p = static_cast<LOLIST*>(plo);
    LoadLoFromBrx3(p, pbis);
    const int c94 = pbis->U8Read();
    for (int i = 0; i < c94; ++i) p->an90.push_back(pbis->S32Read());
    const int cA0 = pbis->U8Read();
    for (int i = 0; i < cA0; ++i) p->an9C.push_back(pbis->S32Read());
    pbis->SkipStringSw();
}

// ---------------------------------------------------------------- ALO family

// orig3: 0x1C8970 BindLoParts(palo, c, table, 0) (twin 0x1A64E8): the class's named parts, a table of c x {grf, id,
// ib} in the executable. Ported: the objects (grf without 0x10 / 0x20 / 0x40): PloFindSwNearest(kind, id, palo) with
// kind 4 (grf 2), 5 (grf 1: the world's) or 1 (the object's own), | 0x400 with grf 0x400; 0x200 posts the object
// (vt slot 9: it is not cloned with its owner); the part is kept at the actor's offset ib (ALO::apart).
// TODO(sly3): the transform tags (0x10), the collision entries (0x20, 0x40: RefSoeHandle, SetSoeOff, ShowAloGlobs).
void BindLoParts3(ALO* palo, int c, uint32_t vaTable)
{
    for (int i = 0; i < c; ++i)
    {
        const uint32_t grf = g_disc.U32Elf(vaTable + 12u * uint32_t(i));
        const OID id = static_cast<OID>(g_disc.U32Elf(vaTable + 12u * uint32_t(i) + 4));
        const uint32_t ib = g_disc.U32Elf(vaTable + 12u * uint32_t(i) + 8);
        if (grf & 0x70) continue;
        LO* plo = nullptr;
        auto findOwn = [&](auto&& self, LO* p) -> LO* {
            for (LO* pc : p->aploOwned)
            {
                if (pc->oid == id) return pc;
                if (LO* q = self(self, pc)) return q;
            }
            return nullptr;
        };
        if (!(grf & 1) || (grf & 2)) plo = findOwn(findOwn, palo);
        if (!plo && (grf & 3))
            for (const auto& p : Psw()->aploAll)
                if (p->oid == id) { plo = p.get(); break; }
        if (!plo) continue;
        if (grf & 0x200) plo->fRemoved = true;
        auto it = std::find_if(palo->apart.begin(), palo->apart.end(), [ib](const auto& e) { return e.first == ib; });
        if (it != palo->apart.end()) it->second = plo;
        else palo->apart.emplace_back(ib, plo);
    }
}

// orig3: 0x23C0B8 LoadAloFromBrx (twin 0x1F31E0, 0.56): the transforms (0x28C338, Sly 2's reads), U32 the load
// flags, the glob set (0x18D148, +0x70), vt slot 67's extra entries (no reads), S16 collision entries (0xD0 bytes
// each at +0xD4: S16 the record, S32 tag, S8 bit 16, S8 bit 17, F32 radius, S8 bit 18 (new), the geometry, the BSP,
// U32 id, U16 (+0x18; Sly 2 S16), S8 bit 21 (new), S16 (+0x20), S8 bit 19); with entries the state (+0xD8: F32 mass,
// the inertia matrix); vt slot 68; options; 0x23B3A8; the children (0x151D80); the mask.
void LoadAloFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    ALO* palo = static_cast<ALO*>(plo);
    LoadAloXfsFromBrx(palo, pbis);  // (0x28C338: the same reads as Sly 2's 0x22D7A0, with its pairs / links / chains)
    palo->uMeta = pbis->U32Read();
    palo->pglobset = std::make_shared<GLOBSET>();
    LoadGlobsetFromBrx(palo->pglobset.get(), palo, pbis);  // (glob.cpp: the Sly 3 renderer's Sly 3 reader)
    const int c = pbis->S16Read();
    palo->asoe.resize(std::size_t(std::max(c, 0)));
    for (SOE& e : palo->asoe)
    {
        e.palo = palo;
        e.pxf = PxfFindAlo(palo, pbis->S16Read());
        e.tag = pbis->S32Read();
        e.f48 = pbis->S8Read() & 1;
        e.f49 = pbis->S8Read() & 1;
        e.sRadius = pbis->F32Read();
        pbis->S8Read();  // bit 18 (new in Sly 3)
        pbis->ReadGeom(&e.geomLocal);
        e.geomWorld = e.geomLocal;  // (CopyTransformGeom 0x18AFB0 with the record's matrix: TODO(sly3))
        pbis->ReadBsp(&e.geomWorld, &e.bsp);
        e.id = pbis->U32Read();
        e.state = static_cast<int16_t>(pbis->U16Read());
        pbis->S8Read();  // bit 21 (new in Sly 3)
        e.pinned = pbis->S16Read();
        e.f50 = pbis->S8Read() & 1;
    }
    if (c > 0)
    {
        palo->fHasState = true;
        palo->gMass = pbis->F32Read();
        palo->matInertia = pbis->ReadMatrix();
    }
    LoadOptionsFromBrx3(palo->aopt, palo, pbis);
    LoadSwObjsFromBrx3(Psw(), palo, pbis, nullptr);
    palo->grfLoad = palo->uMeta;
}

// orig3: 0x1F3BC8 LoadSoFromBrx (SO 0x31, 0x50, 0x56; twin 0x1C3DC8): the ALO, a skipped string
void LoadSoFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadAloFromBrx3(plo, pbis);
    pbis->SkipStringSw();
}

// orig3: 0x24D920 (PO 0x32, guards 0x3A / 0x3B / 0x43): the SO, then its position in its parent (0x1C9428,
// PosToParent 0x28E910) and PregentEnsure 0x241B60 (no reads)
void LoadPoFromBrx3(LO* plo, CBinaryInputStream* pbis) { LoadSoFromBrx3(plo, pbis); }
// orig3: 0x152448 (the players' common part): the PO, BindLoParts
void LoadPlayerFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadPoFromBrx3(plo, pbis);
    BindLoParts3(static_cast<ALO*>(plo), 1, 0x342DC0);
}
// orig3: 0x1AA250 (Sly 0x34; twin 0x1885B8): 0x152448, then +0x250 defaults and BindLoParts(0x6A, 0x345140)
void LoadJtFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadPlayerFromBrx3(plo, pbis);
    BindLoParts3(static_cast<ALO*>(plo), 0x6A, 0x345140);
}
// orig3: 0x13D400 (Bentley 0x36): 0x152448, BindLoParts
void LoadBentleyFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadPlayerFromBrx3(plo, pbis);
    BindLoParts3(static_cast<ALO*>(plo), 0x54, 0x342890);
}
// orig3: 0x1D6C00 (0x5A..0x5C; twin 0x1B1E00): the ALO, BindLoParts(1, 0x345FB0)
void Load1D6C00FromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadAloFromBrx3(plo, pbis);
    BindLoParts3(static_cast<ALO*>(plo), 1, 0x345FB0);
}

// orig3: 0x1D5AB8 (0x6D; twin 0x1B0CA8): the ALO, SetXfModes, then BindStepSegs 0x1D69D8: the 6 ids at +0x15C + 12 i
// (set by field options; -1: none) are its own objects (PloFindSwNearest 1), each posted (vt slot 9)
void Load1D5AB8FromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadAloFromBrx3(plo, pbis);
    for (int i = 0; i < 6; ++i)
    {
        const OID id = OID(plo->U32Field(0, 0x15C + 12u * uint32_t(i), 0xFFFFFFFFu));
        if (id == -1) continue;
        auto find = [&](auto&& self, LO* p) -> LO* {
            for (LO* pc : p->aploOwned)
            {
                if (pc->oid == id) return pc;
                if (LO* q = self(self, pc)) return q;
            }
            return nullptr;
        };
        if (LO* pseg = find(find, plo)) pseg->fRemoved = true;
    }
}

// ALO loaders with non-reading setup around the ALO: orig3 0x14D3F0 (0x74; twin 0x13BE58), 0x1D6C00 (0x5A..0x5C; twin 0x1B1E00: BindLoParts(1, 0x345FB0)), 0x1DC6F8
// (0x30), 0x276E80 (0x71..0x73; twin 0x223FE0), 0x223918 (laser 0x60; twin 0x1DE478), 0x1E4CB8 (breakables 0x6F;
// twin 0x1BABA0), 0x281888 (0x65)
void LoadAloPlainFromBrx3(LO* plo, CBinaryInputStream* pbis) { LoadAloFromBrx3(plo, pbis); }

// orig3: 0x1F6668 (0x67, 0x69, 0x6B; twin 0x1C4EF8): the ALO, its child 0x11 / 422 (+0xE0 = 1), SetXfModes, BindLoParts
void Load1F6668FromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    LoadAloFromBrx3(plo, pbis);
    PloEnsureChild422(static_cast<ALO*>(plo));
    BindLoParts3(static_cast<ALO*>(plo), 1, 0x346FC0);
}
// orig3: 0x1F8D58 (0x6A; twin 0x1C76D0): 0x1F6668 between allocations, 0x1F9028
void Load1F8D58FromBrx3(LO* plo, CBinaryInputStream* pbis) { Load1F6668FromBrx3(plo, pbis); }

template <class T> LO* PloNewT() { return new T(); }
}  // namespace

void RegisterLoaders3(std::unordered_map<uint32_t, PFNLOAD>& m)
{
    m[0x1C7D98] = LoadLoFromBrx3;
    m[0x28FDE0] = LoadLoxFromBrx3;
    m[0x1C5300] = LoadLoxFromBrx3;          // (0x04: LoadLoxFromBrx, ComputeLight)
    m[0x131050] = LoadLoxFromBrx3;          // (0x29; twin 0x12EB78)
    m[0x27E918] = LoadVolumeFromBrx3;
    m[0x27F158] = LoadShapeFromBrx3;
    m[0x15CCB0] = LoadShape15CCB0FromBrx3;
    m[0x27FB30] = LoadShapeKeysFromBrx3;
    m[0x27FA00] = LoadLoxNameFromBrx3;
    m[0x27FFE0] = LoadLoxNameFromBrx3;
    m[0x22D780] = LoadPathFromBrx3;
    m[0x1E6238] = LoadPathFromBrx3;         // (0x0D; twin 0x1BC088)
    m[0x1ECC58] = LoadMeshZoneFromBrx3;
    m[0x273CF8] = LoadZoneFromBrx3;
    m[0x179EA8] = LoadGrouperFromBrx3;
    m[0x2BCA18] = LoadEmitterFromBrx3;
    m[0x2BD488] = LoadExplFromBrx3;
    m[0x1FE270] = Load2DFromBrx3;
    m[0x238FF8] = LoadSmFromBrx3;
    m[0x185EB8] = LoadMissionFromBrx3;
    m[0x187F08] = LoadTaskFromBrx3;
    m[0x23C0B8] = LoadAloFromBrx3;
    m[0x1F3BC8] = LoadSoFromBrx3;
    m[0x24D920] = LoadPoFromBrx3;
    m[0x152448] = LoadPlayerFromBrx3;
    m[0x1AA250] = LoadJtFromBrx3;
    m[0x13D400] = LoadBentleyFromBrx3;
    for (uint32_t a : {0x14D3F0u, 0x1DC6F8u, 0x276E80u, 0x223918u, 0x1E4CB8u, 0x281888u})
        m[a] = LoadAloPlainFromBrx3;
    m[0x283150] = LoadAAFromBrx3;
    m[0x2250C0] = LoadScptFromBrx3;
    m[0x135EE8] = LoadAsegObjFromBrx3;
    m[0x16EE98] = Load97FromBrx3;
    m[0x260810] = LoadScreenObjFromBrx3;  // (the head only)    // ASEG 0x82 / 0x83 (aseg3.cpp)
    m[0x1D6C00] = Load1D6C00FromBrx3;
    m[0x1D5AB8] = Load1D5AB8FromBrx3;
    m[0x16F768] = Load8FFromBrx3;
    m[0x172608] = LoadLoFromBrx3;          // (0x96: the LO between allocations)
    m[0x1F6668] = Load1F6668FromBrx3;
    m[0x1F8D58] = Load1F8D58FromBrx3;
}

// The native struct of a class: by its loader where the loader keeps data beyond its base's, else its nearest
// common base's (ALO 0x2F, LOX 0x03, LO).
PFNNEW PfnNewNativeForCid3(CID cid)
{
    switch (Cls3(cid).pfnLoad)
    {
    case 0x27E918: case 0x273CF8: return PloNewT<LOXGEOM>;
    case 0x27F158: case 0x15CCB0: case 0x27FB30: return PloNewT<LOXSHAPE>;
    case 0x22D780: case 0x1E6238: return PloNewT<LOXPATH>;
    case 0x1ECC58: return PloNewT<LOXMESH>;
    case 0x179EA8: return PloNewT<LOXCHILD>;
    case 0x2BCA18: case 0x2BD488: return PloNewT<LOXEMIT>;
    case 0x187F08: return PloNewT<LOLIST>;
    case 0x2250C0: return PloNewT<LOSCPT3>;
    case 0x135EE8: return PloNewT<ASEG3>;
    case 0x238FF8: return PloNewT<SM>;
    case 0x16EE98: return PloNewT<LO97>;
    case 0x1FE270: return PloNewT<LOPE3>;
    case 0x260810: return PloNewT<LOSCREEN3>;
    default: break;
    }
    if (FCid3DerivedFrom(cid, CID3::ALO)) return PloNewT<ALO>;
    if (FCid3DerivedFrom(cid, CID3::LOX)) return PloNewT<LOX>;
    return PloNewT<LO>;
}
