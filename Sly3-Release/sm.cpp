#include "activate.h"
#include "game/sly3.h"
#include "sm.h"
#include "blend.h"

#include "bis.h"
#include "brx.h"
#include "readtrace.h"
#include "sw.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace
{
// orig 0x0012F2A8 (aseg.cpp's loader, then the object's options through g_pfnLoadOptionsFromBrx)
void LoadAsegLoFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LoadAsegFromBrx(&static_cast<ASEG*>(plo)->asegd, plo, pbis);
    // (debug: SLY2N_ASEGSNDFIND=id: the ASEGs with a sound event of that id, their parent)
    if (static const char* s_sz = std::getenv("SLY2N_ASEGSNDFIND"); s_sz)
        for (const auto& aev : static_cast<ASEG*>(plo)->asegd.aaev)
            for (const ASEGEV& ev : aev)
                if (ev.kind == 0 && ev.an[0] == std::atoi(s_sz))
                    std::printf("asegsnd %d: aseg %d parent %d t %.3f source %d\n", ev.an[0], plo->oid, plo->paloParent ? plo->paloParent->oid : -1, ev.t, ev.an[1]);
}

void LoadOptionsOfLo(void* pv, CBinaryInputStream* pbis)
{
    LO* plo = static_cast<LO*>(pv);
    LoadOptionsFromBrx(plo->aopt, plo, pbis);
}

}  // namespace

// orig 0x001EFAC0 LoadSmFromBrx: the states (children loaded into the SM's parent), the nodes, the edges
// (Sly 3: the same reads, orig3 0x238FF8; game/loaders3.cpp registers it)
void LoadSmFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1EFAC0);
    SM* p = static_cast<SM*>(plo);
    LOSINK sink;
    sink.papo = &p->aploChild;
    LoadSwObjsFromBrx(g_psw, p->paloParent, pbis, &sink);
    for (LO* pc : p->aploChild) RemoveLo(g_psw, pc);  // vt slot 9 of each state
    const int cnode = pbis->U8Read();
    for (int i = 0; i < cnode; ++i)
    {
        const int32_t id = pbis->S32Read();
        const int32_t next = pbis->S32Read();
        p->anode.emplace_back(id, next);
    }
    const int cedge = pbis->U8Read();
    for (int i = 0; i < cedge; ++i)
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
            if (!e.plo) continue;  // an edge of a state that was not loaded is dropped
        }
        p->aedge.push_back(e);
    }
    LoadOptionsFromBrx(p->aopt, p, pbis);
    // TODO(M3): the edge masks' distance tables
}

namespace
{

void LoadSpritesFromBrx(SPRITESET* p, CBinaryInputStream* pbis);

// orig 0x002149D8: a sprite (by its index in the set)
void LoadSpriteFromBrx(SPRITESET* p, CBinaryInputStream* pbis)
{
    LOADFN(0x2149D8);
    pbis->U16Read();  // the sprite's index
    const uint32_t kind = pbis->U32Read();
    ++p->csprite;
    switch (kind)
    {
    case 4: pbis->U16Read(); break;
    case 8:
    {
        LoadSpritesFromBrx(p, pbis);
        const uint32_t cw = pbis->U32Read();
        for (uint32_t i = 0; i < cw; ++i) pbis->U32Read();
        const uint32_t cg = pbis->U32Read();
        for (uint32_t i = 0; i < cg; ++i) pbis->F32Read();
        break;
    }
    case 3:
    {
        pbis->U32Read();
        pbis->U32Read();
        pbis->F32Read();
        pbis->F32Read();
        pbis->F32Read();
        pbis->S8Read();
        const int mode = pbis->S8Read();
        if (mode == 1 || mode == 0) pbis->S32Read();
        break;
    }
    case 2:
    {
        pbis->ReadMatrix();
        const int c = pbis->U16Read();
        pbis->Read(static_cast<std::size_t>(c) * 0x18, nullptr);
        break;
    }
    case 6: case 7:
        for (int i = 0; i < 4; ++i) pbis->S16Read();
        pbis->S32Read();
        break;
    case 1:
    {
        const int c = pbis->U16Read();
        for (int i = 0; i < c; ++i)
        {
            const int cpt = pbis->U8Read();
            for (int k = 0; k < 4; ++k) pbis->U8Read();
            for (int j = 0; j < cpt; ++j)
            {
                pbis->U32Read();
                pbis->U8Read();
                pbis->S16Read();
                pbis->S16Read();
            }
        }
        break;
    }
    default: break;
    }
}

// orig 0x00214398: U16 count sprites
void LoadSpritesFromBrx(SPRITESET* p, CBinaryInputStream* pbis)
{
    LOADFN(0x214398);
    const int c = pbis->U16Read();
    for (int i = 1; i <= c; ++i) LoadSpriteFromBrx(p, pbis);
}

// orig 0x00214460: the sprite groups and their animations
void LoadSpriteGroupsFromBrx(SPRITESET* p, CBinaryInputStream* pbis)
{
    LOADFN(0x214460);
    p->cgroup = pbis->U16Read();
    for (int i = 0; i < p->cgroup; ++i)
    {
        pbis->U16Read();  // reuse count
        pbis->U16Read();  // frames per reuse
        const int canim = pbis->U16Read();
        for (int j = 0; j < canim; ++j)
        {
            const float duration = pbis->F32Read();
            const int32_t id = pbis->S32Read();
            p->aanim.emplace_back(id, duration);
            const int ckey = pbis->U16Read();
            for (int k = 0; k < ckey; ++k)
            {
                pbis->F32Read();
                const int citem = pbis->U8Read();
                for (int m = 0; m < citem; ++m)
                {
                    pbis->U16Read();
                    pbis->U16Read();
                    pbis->U16Read();
                    pbis->S16Read();
                    pbis->U8Read();
                    pbis->U8Read();
                    pbis->S32Read();
                    pbis->ReadMatrix();
                }
                const int ccolor = pbis->U8Read();
                for (int m = 0; m < ccolor; ++m)
                {
                    pbis->U32Read();
                    pbis->U32Read();
                }
                const int cshort = pbis->U8Read();
                for (int m = 0; m < cshort; ++m) pbis->U16Read();
                const int cbyte = pbis->U8Read();
                for (int m = 0; m < cbyte; ++m) pbis->U8Read();
                const int ctag = pbis->U8Read();
                for (int m = 0; m < ctag; ++m)
                {
                    pbis->U8Read();
                    pbis->S32Read();
                }
            }
        }
    }
}

// orig 0x00212298 LoadSpritesetFromBrx (cid 0x7E family). TODO(render): the sprites' packets are not kept
void LoadSpritesetFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x212298);
    SPRITESET* p = static_cast<SPRITESET*>(plo);
    LoadOptionsFromBrx(p->aopt, p, pbis);
    // The data itself: the renderer's native loader (screenobj.cpp, same reads as LoadSpritesFromBrx /
    // LoadSpriteGroupsFromBrx, which are kept for reference).
    p->pso = std::make_shared<SCREENOBJ>();
    p->pso->oid = p->oid;
    p->pso->ib = pbis->GetOffset();
    LoadScreenObjFromBrx(p->pso.get(), pbis);
    p->f = p->pso->fExternal;
    p->g90 = p->pso->g90;
    p->u94 = p->pso->ul94;
    p->u98 = p->pso->ul98;
    p->uBC = p->pso->ulBC;
}

template <class T> LO* PloNewT() { return new T(); }
}

void LoadScreenObj3FromBrx(LO* plo, CBinaryInputStream* pbis) { LoadSpritesetFromBrx(plo, pbis); }

void RegisterAsegLoaders(std::unordered_map<uint32_t, PFNLOAD>& m)
{
    m[0x12F2A8] = LoadAsegLoFromBrx;
    g_pfnLoadOptionsFromBrx = LoadOptionsOfLo;
    m[0x1EFAC0] = LoadSmFromBrx;
    m[0x260810] = LoadSpritesetFromBrx;   // (Sly 3: registered by loaders.cpp RegisterLoaders)
}

PFNNEW PfnNewAseg(CID cid)
{
    if (cid == S3CID(0x6D)) return PloNewT<ASEG>;
    if (cid == S3CID(0x6E)) return PloNewAsegbl;  // blend.cpp (runtime only: NewBlend 0x137A30)
    if (cid == S3CID(0x6F)) return PloNewT<SM>;
    if (cid == S3CID(0x7E)) return PloNewT<SPRITESET>;
    return nullptr;
}
