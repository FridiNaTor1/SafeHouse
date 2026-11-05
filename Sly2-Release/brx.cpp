#include "wr.h"
#include "worldglobals.h"
#include "brx.h"
#include "breakable.h"
#include "hide.h"
#include "vehiclehooks.h"
#include "booty.h"
#include "objmethod.h"
#include "sidebag.h"
#include "frame.h"
#include "gc.h"

#include "activate.h"
#include "animfind.h"
#include "trigger.h"
#include "sound.h"
#include "clock.h"

#include "alo.h"

#include "bis.h"
#include "disc.h"
#include "readtrace.h"
#include "sw.h"
#include "shd.h"
#include "splicemap.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

BRXSTATS g_brxstats;
WORLD_VALUE(g_brxstats);   // (PC: moved aside while a kept world loads, worldglobals.h)

// The world filter (orig 0x3E1110, read by 0x210A88): the world record index being loaded; -1 matches anything.
int g_nFilterCur = -1;
WORLD_VALUE(g_nFilterCur);   // (PC: moved aside while a kept world loads, worldglobals.h)

// orig: 0x0013FAD0 FFilterMatches (the current filter value is in the list, or there is no filter)
bool FFilterMatches(int cfilter, const int16_t* afilter)
{
    if (g_nFilterCur == -1) return true;
    for (int i = 0; i < cfilter; ++i)
        if (afilter[i] == g_nFilterCur) return true;
    return false;
}

// CBinaryInputStream::Skip (0x1179B8)
void SkipBytes(CBinaryInputStream* pbis, size_t cb) { pbis->Skip(cb); }

namespace
{
std::vector<LO*> s_aploLoading;  // the objects whose loads are running, innermost last

void LoadLoBody(LO* plo, CBinaryInputStream* pbis)
{
    s_aploLoading.push_back(plo);
    try
    {
        plo->pcls->pfnLoad(plo, pbis);
    }
    catch (...)
    {
        s_aploLoading.pop_back();
        throw;
    }
    s_aploLoading.pop_back();
}
}

namespace
{
constexpr uint32_t VA_AEOPID = 0x2AC880;  // the option descriptors, 0x28 bytes each
constexpr uint32_t CB_EOPID = 0x28;

struct EOPID
{
    int k;            // +0x00: the option's kind (OTYP)
    int grf;          // +0x04
    int c18;          // +0x18: the field offset (grf 0x80) or the CRef count (grf 0x400)
    uint32_t pfnGet;  // +0x24: the sub-object getter (grf 0x2000), a GCC 2.95 PMF's function
    uint32_t pfnArray;  // +0x08: the function a CRef-array option (grf 0x400) is handed to
    uint32_t pfnScript;  // +0x10: the method's thunk the scripts call (a copy's attach, 0x1CAEC8: grf 1 without 0x400)
};

EOPID EopidFromIndex(int i)
{
    const uint32_t va = VA_AEOPID + static_cast<uint32_t>(i) * CB_EOPID;
    EOPID e;
    e.k = static_cast<int>(g_disc.U32Elf(va));
    e.grf = static_cast<int>(g_disc.U32Elf(va + 4));
    e.c18 = static_cast<int>(g_disc.U32Elf(va + 0x18));
    e.pfnGet = g_disc.U32Elf(va + 0x24);
    e.pfnArray = g_disc.U32Elf(va + 0x08);
    e.pfnScript = g_disc.U32Elf(va + 0x10);
    return e;
}

// The value of an option of kind k read by the field (grf 0x80) and setter paths of 0x13E818 (the same reads).
OPTVAL ReadOptionValue(int k, CBinaryInputStream* pbis)
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
    case 2: case 9: case 11:
    case 4:  // euler angles (the original makes a matrix of them, 0x11B570)
        for (int i = 0; i < 3; ++i) v.ag[i] = pbis->F32Read();
        break;
    case 3: case 12:
        for (int i = 0; i < 4; ++i) v.ag[i] = pbis->F32Read();
        break;
    case 13: v.n = static_cast<int32_t>(pbis->U32Read()); break;  // a colour (0x1ED278 converts it)
    case 22:
    {
        const int c = pbis->U16Read();
        v.an.resize(c);
        pbis->Read(static_cast<std::size_t>(c) * 2, v.an.data());
        break;
    }
    case 8: case 18: case 24:
    {
        const int cls = (k & ~0x8000) >> 12;
        if (cls == 1) v.n = static_cast<int32_t>(pbis->U32Read());
        else if (cls == 2) v.n = pbis->S32Read();  // an object's oid (resolved now or after the load)
        break;
    }
    default: v.n = pbis->S16Read(); break;
    }
    return v;
}
}

uint32_t LO::U32Field(uint32_t pfnGetter, uint32_t ib, uint32_t uDefault) const
{
    const uint64_t key = (uint64_t(pfnGetter) << 32) | ib;
    for (const auto& [k, u] : afield)
        if (k == key) return u;
    return uDefault;
}

void LO::SetField(uint32_t pfnGetter, uint32_t ib, uint32_t u)
{
    const uint64_t key = (uint64_t(pfnGetter) << 32) | ib;
    if (static const int s_ib = std::getenv("SLY2N_FIELDPRINT") ? int(std::strtol(std::getenv("SLY2N_FIELDPRINT"), nullptr, 0)) : -1; s_ib == int(ib))
        std::printf("field %d +0x%X = %u (getter 0x%X)\n", oid, ib, u, pfnGetter);  // (debug: a field's stores)
    for (auto& [k, uOld] : afield)
        if (k == key) { uOld = u; return; }
    afield.emplace_back(key, u);
}

// orig 0x0013E690 RefReadOption
OPTVAL RefReadOption(int k, CBinaryInputStream* pbis)
{
    LOADFN(0x13E690);
    OPTVAL v;
    v.k = k;
    switch (k)
    {
    case 0: v.n = pbis->U8Read() != 0; v.tagk = 0x17; break;
    case 1: v.ag[0] = pbis->F32Read(); v.tagk = 0x16; break;
    case 2: case 9:
        for (int i = 0; i < 3; ++i) v.ag[i] = pbis->F32Read();
        v.tagk = k == 2 ? 0x1A : 0x1C;
        break;
    case 5: case 6: v.n = pbis->S32Read(); v.tagk = 0x15; break;
    case 7: case 14: case 15: case 16: case 17: case 19: case 20: case 21: case 23:
        v.n = pbis->S16Read();
        v.tagk = 0x15;
        break;
    case 10:
        v.ag[0] = pbis->F32Read();
        v.ag[1] = pbis->F32Read();
        v.tagk = 0x1D;
        break;
    default:
        if (((k & ~0x8000) >> 12) == 1)
        {
            v.n = static_cast<int32_t>(pbis->U32Read());
            v.tagk = 0x15;
        }
        else v.tagk = 0x24;
        break;
    }
    return v;
}

// orig 0x0013E818 LoadOptionFromBrx
void LoadOptionFromBrx(OPT& opt, LO* plo, int eopid, int grf, CBinaryInputStream* pbis)
{
    LOADFN(0x13E818);
    const EOPID e = EopidFromIndex(eopid);
    opt.eopid = eopid;
    opt.grf = grf;
    // the methods called at the load: 0x100B10 (option 26) queues RemoveLo for the start (0x1A5D30); 0x100A90
    // (option 25) is AddLo (vt slot 8)
    if (plo && (grf & 0x400) && e.pfnArray == 0x100B10) plo->fRemoveAtStart = true;
    if (plo && (grf & 0x400) && e.pfnArray == 0x100A90) AddLo(g_psw, plo);
    if (grf & 0x401)
    {
        // c CRefs (each a value, or a list of U8 count values of one kind) handed to the EOPID's function
        for (int i = 0; i < e.c18; ++i)
        {
            int w = pbis->U16Read();
            if (w & 0x8000)
            {
                w &= ~0x8000;
                OPTVAL list;
                list.k = w | 0x8000;
                const int c = pbis->U8Read();
                for (int j = 0; j < c; ++j) list.alist.push_back(RefReadOption(w, pbis));
                opt.aval.push_back(std::move(list));
            }
            else opt.aval.push_back(RefReadOption(w, pbis));
        }
        // a method the loader calls (grf 0x400: the EOPID's +0x08 thunk) with a native body (objmethod.h)
        if (plo && (grf & 0x400) && e.pfnArray != 0x100A90 && e.pfnArray != 0x100B10)
        {
            if (PFNOBJMETHOD pfn = PfnObjectMethod(e.pfnArray); !pfn && std::getenv("SLY2N_METHODPRINT"))
                std::printf("load method %d of %d (cid 0x%X): pfn 0x%X (no body)\n", eopid, plo->oid, plo->Cid(), e.pfnArray);
            else if (pfn)
            {
                std::vector<CRef> aref;
                for (const OPTVAL& v : opt.aval) aref.push_back(RefFromOptionValue(v));
                pfn(plo, aref);
            }
        }
        // a copy's attach (PspareAcquire 0x1CAEC8 masks the record's flags with 0x2031: no 0x400) calls the record's
        // +0x10 thunk, the one the scripts call, not the loader's +0x08. They differ for options 26 (Remove now, not at
        // the start), 233, 243..248 (a guard's kind entries resolved at once: 0x207BC8 / 0x207CF8 / 0x207E08 / 0x207F50),
        // 364 / 365 and 1040..1044; the records of flag 1 alone have no loader thunk at all. Natively the attach ran the
        // loader's: a pooled Waterpump rat's walk entry (243 from its door proxy) was left unresolved and the rat came
        // out of the door in its bind pose (R09).
        else if (plo && (grf & 1) && !(grf & 0x400))
        {
            if (e.pfnScript == 0x100B88) RemoveLo(g_psw, plo);    // SplLoRemove: vt slot 9 now
            else if (e.pfnScript == 0x100A90) AddLo(g_psw, plo);  // vt slot 8
            else if (PFNOBJMETHOD pfn = PfnObjectMethod(e.pfnScript))
            {
                std::vector<CRef> aref;
                for (const OPTVAL& v : opt.aval) aref.push_back(RefFromOptionValue(v));
                pfn(plo, aref);
            }
            else if (std::getenv("SLY2N_METHODPRINT"))
                std::printf("attach method %d of %d (cid 0x%X): pfn 0x%X (no body)\n", eopid, plo->oid, plo->Cid(), e.pfnScript);
        }
        return;
    }
    // the sub-object getter runs before the value is stored (0x1F7B98 makes the 0x12 child it returns)
    LO* pchildField = nullptr;
    if (plo && (grf & 0x2000) && e.pfnGet == 0x1F7B98 && FIsLoDerivedFrom(plo, 0x30))
        pchildField = PloEnsureChild391(static_cast<ALO*>(plo));
    // orig 0x20A9C0: the guard's default attack is a real child action. The
    // range options must create/register it before storing fields on it.
    if (plo && (grf & 0x2000) && e.pfnGet == 0x20A9C0 && FIsLoDerivedFrom(plo, 0x38))
    {
        for (const OPT& action : plo->aopt)
            if (action.eopid == 233 && !action.aval.empty())
            {
                pchildField = PloFindSwNearest(plo, action.aval[0].n);
                break;
            }
        if (!pchildField)
        {
            pchildField = PloNew(0x78, g_psw, static_cast<ALO*>(plo), 0x2CC, -1);
            OPT action;
            action.eopid = 233;
            OPTVAL oid;
            oid.tagk = TAGK_S32;
            oid.n = 0x2CC;
            action.aval.push_back(oid);
            plo->aopt.push_back(std::move(action));
        }
    }
    OPTVAL v = ReadOptionValue(e.k, pbis);
    if (pchildField && e.pfnGet == 0x20A9C0 && eopid >= 235 && eopid <= 242)
    {
        // The same properties on an explicit action (353..359 / 363).
        static constexpr int aeopid[] = {353, 354, 355, 356, 357, 358, 359, 363};
        OPT actionField;
        actionField.eopid = aeopid[eopid - 235];
        actionField.grf = grf & ~0x2000;
        actionField.aval.push_back(v);
        pchildField->aopt.push_back(std::move(actionField));
    }
    // a space warp's entry fields (the getters 0x22C4D8 / 0x22C4F8: EOPIDs 947..961) and its current entry (964)
    if (plo && plo->Cid() == 0x73 && !(grf & 0x100)) FLoadWrOption(plo, eopid, v.ag.data(), v.n);
    // the booty's current record (the getter 0x13C4D8: EOPIDs 1155 / 1161..1163, booty.cpp)
    if (plo && (grf & 0x2000) && e.pfnGet == 0x13C4D8) SetBootyRecordField(plo, static_cast<uint32_t>(e.c18), v);
    if ((grf & 0x90) && plo)
    {
        // a field of the object (or of the sub-object the getter returns) at offset c18
        // orig: 0x13E818 stores through the getter's returned object. A generated
        // target point is an actual child, so its authored flags belong to it.
        LO* pfield = pchildField ? pchildField : plo;
        const uint32_t pfn = pchildField ? 0 : ((grf & 0x2000) ? e.pfnGet : 0);
        // 0x13E818 stores both floats of kind 10 (e.g. a job marker's in/out radii), just as it stores vectors.
        const uint32_t cg = e.k == 1 ? 1 : e.k == 10 ? 2 : (e.k == 2 || e.k == 9 || e.k == 11) ? 3 : 0;
        if (cg)
            for (uint32_t i = 0; i < cg; ++i)
            {
                uint32_t u;
                std::memcpy(&u, &v.ag[i], 4);
                pfield->SetField(pfn, static_cast<uint32_t>(e.c18) + 4 * i, u);
            }
        else pfield->SetField(pfn, static_cast<uint32_t>(e.c18), static_cast<uint32_t>(v.n));
    }
    // the setters (grf 0x100) the activation keeps: option 13 the radius (vt slot 42), 14 the kind (0x1A6CA8)
    if (plo && (grf & 0x100) && !(grf & 0x2000))  // (the attach's 0x20 is mapped to 0x100: activate.cpp)
    {
        if (eopid == 13) SetLoRadius(g_psw, plo, v.ag[0]);
        else if (eopid == 14) SetLoSt(g_psw, plo, v.n);
        else if (eopid == 32) plo->fMaskOwn = v.n & 1;  // 0x1A6E38
        else if (PFNOBJSETTER pfnSet = PfnObjectSetter(eopid)) pfnSet(plo, RefFromOptionValue(v));
    }
    opt.aval.push_back(std::move(v));
}

// orig 0x0013F810 LoadOptionsFromBrx
void LoadOptionsFromBrx(std::vector<OPT>& aopt, LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x13F810);
    for (int n; (n = pbis->S16Read()) >= 0;)
    {
        const int grf = static_cast<int>(g_disc.U32Elf(VA_AEOPID + static_cast<uint32_t>(n) * CB_EOPID + 4)) & 0x2580;
        OPT opt;
        LoadOptionFromBrx(opt, plo, n, grf, pbis);
        aopt.push_back(std::move(opt));
    }
}

bool FIsLoDerivedFrom(const LO* plo, CID cid) { return plo && FCidDerivedFrom(plo->Cid(), cid); }

void LOSINK::Append(LO* plo)
{
    if (fStack)
    {
        // orig 0x13FA30: 0x1A58D0(po, 0, 0) = SetLoParent(po, null) (vt slot 10 0x1A5900): a shared object leaves its
        // parent for the world's root (and so is not part of the parent's clones); flags 48..51 = n (the object
        // and an ALO's children); slot 25 (m, 1)
        if (plo->paloParent) SetLoParentActivation(g_psw, plo, nullptr);
        for (LO* ploOwner = PloLoading(); ploOwner; ploOwner = nullptr)
        {
            auto& a = ploOwner->aploOwned;
            a.erase(std::remove(a.begin(), a.end(), plo), a.end());
        }
        auto set = [&](auto&& self, LO* p) -> void {
            p->grf = (p->grf & ~(0xFull << 48)) | (uint64_t(n & 0xF) << 48);
            if (FIsLoDerivedFrom(p, 0x30))
                for (LO* pc : static_cast<ALO*>(p)->aploChild) self(self, pc);
        };
        set(set, plo);
        // TODO(M3): vt slot 25 (SetLoKey) with m; 0x1A58D0
        return;
    }
    if (papo) papo->push_back(plo);
}

LO* PloLoading() { return s_aploLoading.empty() ? nullptr : s_aploLoading.back(); }

int IspliceBlockCur(SW* psw);  // splicemap.cpp (debug)
// orig 0x0013E3C8 PloNew
LO* PloNew(CID cid, SW* psw, ALO* paloParent, OID oid, int iframe)
{
    CLS* pcls = PclsFromCid(cid);
    if (!pcls || !pcls->pfnNew)
        throw std::runtime_error("PloNew: class " + StrClassName(cid) + " cannot be created");
    LO* plo = pcls->pfnNew();
    plo->pcls = pcls;
    plo->paloParent = paloParent;
    plo->oid = oid;
    plo->iframe = iframe;
    if (iframe >= 0) plo->pframe = PframeFromIsplice(iframe, psw);  // orig 0x1FFA40 + 0x256EE0
    // (debug: SLY2N_IFRAMELIST=1: every object bound to a splice module, with its parent)
    if (static const bool s_f = std::getenv("SLY2N_IFRAMELIST") != nullptr; s_f && iframe >= 0)
        std::printf("iframe: oid %d cid 0x%X block %d module %d parent %d\n", int(oid), int(cid), IspliceBlockCur(psw),
                    iframe, paloParent ? int(paloParent->oid) : -1);
    if (static const char* s_sz = std::getenv("SLY2N_FRAMEOF"); s_sz && (s_sz[0] == 'm' ? std::atoi(s_sz + 1) == iframe : std::atoi(s_sz) == oid))  // (debug: an object's module; "mN": the objects of module N)
        std::printf("frameof %d (cid 0x%X): module %d frame %p\n", oid, cid, iframe, (void*)plo->pframe.get());
    plo->igrp = psw->iwe;
    plo->ichannel = psw->iweChannel;
    plo->ploNsRow = paloParent ? paloParent->ploNsRow : nullptr;  // InitLo 0x1A5270: +0x3C from the parent
    psw->aploAll.emplace_back(plo);
    psw->mpcidaplo[cid].push_back(plo);  // orig: the oid and cid lists
    // InitLo (vt slot 7): the object joins its parent (before its removed children), its activation fields;
    // OnAdd (slot 11) when in the world, slot 23 when its root is active (TODO(M3): the classes' InitLo bodies)
    if (paloParent)
    {
        auto& a = paloParent->aploChild;
        a.insert(std::find_if(a.begin(), a.end(), [](const LO* p) { return p->fRemoved; }), plo);
    }
    InitLoActivation(psw, plo);
    // orig: 0x00211D08 / 0x00211E40: failure and modal screen InitLo.
    // These defaults precede LoadOptionsFromBrx, so authored overrides still win.
    if (pcls->PfnOrig(7) == 0x211D08 || pcls->PfnOrig(7) == 0x211E40)
        plo->SetField(0, 0xB0, 1);
    // orig: 0x00183900 (cid 0x58's InitLo, hide.cpp): its fields and contact bytes, before the load's options
    if (pcls->PfnOrig(7) == 0x183900) InitHide(plo);
    // orig: 0x00184F50 (the hooks' InitLo, cid 0x13): 0x21D278's +0xE8 0x17E / +0xEC 25 / +0xF4 -1, then +0xE8 1 (the
    // jump target's grfHook bit), +0x12C 8, +0x130 9, before the load's options
    if (pcls->PfnOrig(7) == 0x184F50)
    {
        plo->SetField(0, 0xE8, 1);
        const float g = 25.0f;
        uint32_t u;
        std::memcpy(&u, &g, 4);
        plo->SetField(0, 0xEC, u);
        plo->SetField(0, 0xF4, 0xFFFFFFFFu);
        plo->SetField(0, 0x12C, 8);
        plo->SetField(0, 0x130, 9);
    }
    // orig: 0x001320C8 (the barriers' InitLo, barrier.cpp): a physics root (flag bit 34)
    if (pcls->PfnOrig(7) == 0x1320C8)
    {
        void InitBarrier(LO * plo);
        InitBarrier(plo);
    }
    psw->aploLoad.push_back(plo);  // orig +0x39E4
    if (!s_aploLoading.empty()) s_aploLoading.back()->aploOwned.push_back(plo);
    // the thrown objects' InitLo 0x13A9E8 (projectile.cpp): updated every frame (+0x40 |= 2)
    if (FCidDerivedFrom(cid, 0x4F)) plo->grfUpd |= 0x2;
    // the touch pads' InitLo 0x148BB0 (pad.cpp): updated every frame (+0x40 |= 2)
    if (FCidDerivedFrom(cid, 0x53)) plo->grfUpd |= 0x2;
    // class 0x60's InitLo 0x1B0B78 (bird.cpp): updated every frame (+0x40 |= 2)
    if (cid == 0x60) plo->grfUpd |= 0x2;
    // orig0x1D4E68: ammunition pickups update until the position record rests.
    if (cid == 0x6C) plo->grfUpd |= 0x2;
    // InitLo bodies (vt slot 7) that make objects
    if (FCidDerivedFrom(cid, 0x38))
    {
        // orig 0x205EB8 (the guards' InitLo): the world's one 0x7D (oid 1622, 0x205E70: SW +0x3DD48C) and the
        // guard's own 0x10 object (oid 648, no parent: +0xF64)
        if (!psw->plo7D) psw->plo7D = PloNew(0x7D, psw, nullptr, 0x656, -1);
        LO* pdet = PloNew(0x10, psw, nullptr, 0x288, -1);
        SetLoSt(psw, pdet, 2);  // (0x205EB8: kind 2, never active)
        void OnGuardInitLo(LO* pguard, LO* ploDetector);  // guardstart.cpp (+0xF64)
        OnGuardInitLo(plo, pdet);
    }
    return plo;
}

// orig 0x0013E510 PloCloneLo
LO* PloCloneLo(LO* plo, SW* psw, ALO* paloParent)
{
    if (!plo) return nullptr;
    LO* ploNew = PloNew(plo->Cid(), psw, paloParent, plo->oid, -1);
    // vt slot 40 (CloneLo): TODO(M3) the class's clone bodies; the loader's data is copied here
    ploNew->ploCloneOf = plo;
    ploNew->iframe = plo->iframe;
    ploNew->aopt = plo->aopt;
    ploNew->afield = plo->afield;
    CloneEmbeddedTriggerActions(plo, ploNew);
    CloneAmbientSoundRecords(plo, ploNew);
    {
        // (the native tables the load's methods and setters fill: copied as CloneLo 0x1A6730 copies the object)
        void ClonePocketData(LO* source, LO* target);         // pocket.cpp
        void CloneProjectileData(const LO* source, LO* target);  // projectile.cpp
        void CloneBootyData(const LO* source, LO* target);    // booty.cpp
        ClonePocketData(plo, ploNew);
        CloneProjectileData(plo, ploNew);
        CloneBootyData(plo, ploNew);
    }
    if (FIsLoDerivedFrom(plo, 0x16))
    {
        // orig: 0x1A6730 copies these value fields with the shape; geometry remains shared.
        auto* src = static_cast<LOXSHAPE*>(plo);
        auto* dst = static_cast<LOXSHAPE*>(ploNew);
        dst->posBound = src->posBound;
        dst->radiusBoundSquared = src->radiusBoundSquared;
    }
    // 0x1A6730: the object's own frame cloned (0x127E90) and its sidebag cloned into it (0x12AB78): a copy's scripts
    // keep their own per-instance values
    if (plo->pframe)
    {
        ploNew->pframe = PframeNew();
        plo->pframe->CloneTo(ploNew->pframe.get());
    }
    if (plo->psidebag)
    {
        ploNew->psidebag = PsidebagNew();
        plo->psidebag->CloneTo(ploNew->psidebag.get(), ploNew->pframe.get());
    }
    ploNew->grfLoad = plo->grfLoad;
    ploNew->st = plo->st;  // (the activation kind and radius copied with the object, not the mask: TODO the exact
    ploNew->sRadius = plo->sRadius;  // CloneLo 0x1A6730)
    ploNew->fRemoveAtStart = plo->fRemoveAtStart;  // (the deferred callbacks +0x34 go with the copy)
    ploNew->fMaskOwn = plo->fMaskOwn;
    // a streaming proxy's copy streams the same group (PROXY's CloneLo 0x1CA7B8: its binds copied)
    if (auto it = psw->mpproxy.find(plo); it != psw->mpproxy.end())
    {
        PROXYSTREAM ps = it->second;
        ps.airow.clear();
        // orig: 0x001CA7B8: a cloned proxy owns a clone of each bind's frame (0x127E90), rooted by 0x259A88.
        for (PROXYBIND& bind : ps.abind)
        {
            if (!bind.pframe) continue;
            auto pframe = PframeNew();
            bind.pframe->CloneTo(pframe.get());
            bind.pframe = std::move(pframe);
            g_gc.AddRootFrame(bind.pframe.get());
        }
        psw->mpproxy[ploNew] = std::move(ps);
    }
    ploNew->igrp = plo->igrp;
    if (psw->iweChannel < 0) ploNew->ichannel = plo->ichannel;  // (a copy made while a group loads is in that group's channel)
    {
        const OID oidOld = ploNew->oidInstance;
        ploNew->oidInstance = plo->oidInstance;
        NoteOidInstanceChanged(psw, ploNew, oidOld);
    }
    if (FIsLoDerivedFrom(plo, 0x72))
    {
        void CloneScptData(LO* ploSrc, LO* ploDst);  // scpt.cpp: orig 0x1DFBF8 (SCPT's vt slot 41)
        CloneScptData(plo, ploNew);
    }
    if (plo->Cid() == 0x73) CloneWrData(plo, ploNew);  // 0x22B748
    CloneBreakableData(plo, ploNew);   // (the breakable's fields, copied with the object)
    if (FIsLoDerivedFrom(plo, 0x30)) CloneAloData(static_cast<ALO*>(plo), static_cast<ALO*>(ploNew));
    else if (FIsLoDerivedFrom(plo, 3))
    {
        // a LOX: its matrix, its reference (into the parent's records: the copy's parent's)
        const LOX* p = static_cast<LOX*>(plo);
        LOX* q = static_cast<LOX*>(ploNew);
        q->mat = p->mat;
        q->ixfRef = p->ixfRef;
        q->pxf = p->pxf;
        if (p->pxf && p->paloParent && paloParent)
            for (size_t i = 0; i < p->paloParent->axf.size() && i < paloParent->axf.size(); ++i)
                if (p->paloParent->axf[i].get() == p->pxf) q->pxf = paloParent->axf[i].get();
        // a path (cid 0x0A family: the pipes / ledges / rails of the proxies' instances) and a point set (cid 8 / 9):
        // the loaded points (the original's CloneLo copies the object, its curve's arrays shared)
        if (const LOXPATH* pp = dynamic_cast<const LOXPATH*>(plo))
            if (LOXPATH* qp = dynamic_cast<LOXPATH*>(ploNew))
            {
                qp->fE0 = pp->fE0;
                qp->nE4 = pp->nE4;
                qp->an = pp->an;
                qp->avecF4 = pp->avecF4;
                qp->avec140 = pp->avec140;
                qp->avec144 = pp->avec144;
            }
        if (const LOXPTS* pp = dynamic_cast<const LOXPTS*>(plo))
            if (LOXPTS* qp = dynamic_cast<LOXPTS*>(ploNew))
            {
                qp->avec = pp->avec;
                qp->ax1 = pp->ax1;
                qp->ax2 = pp->ax2;
            }
        // an emitter: its loaded colours and mesh (0x15C320 / the EMITB the clone shares)
        if (const LOXEMIT* pe = dynamic_cast<const LOXEMIT*>(plo))
            if (LOXEMIT* qe = dynamic_cast<LOXEMIT*>(ploNew))
            {
                qe->nColors = pe->nColors;
                qe->argba = pe->argba;
                if (pe->pemitmesh) qe->pemitmesh = std::make_unique<LOXEMIT::EMITMESH>(*pe->pemitmesh);
            }
    }
    CloneVehicle(plo, ploNew);  // orig0x1D1410, independent attachment BASICs
    // the hierarchy: what the object loaded is cloned with it (an ALO's children under the copy)
    s_aploLoading.push_back(ploNew);
    const std::vector<LO*> aploOwned = plo->aploOwned;
    for (size_t i = 0; i < aploOwned.size(); ++i)
    {
        LO* pc = aploOwned[i];
        // the classes' CloneLo bodies: a grouper (0x15C320) clones only its children derived from cid 0x1E
        // (the others were posted at load and carry bit 57, which CloneAlo 0x1F66F0 skips); a zone (0x221140)
        // only its first child (+0x10C)
        if (FIsLoDerivedFrom(plo, 0x1F) && !FIsLoDerivedFrom(pc, 0x1E)) continue;
        if (FIsLoDerivedFrom(plo, 0x0F) && i > 0) continue;
        if (pc->fRemoved) continue;  // posted at load (SM states, a grouper's other children)
        LO* pcNew = PloCloneLo(pc, psw, pc->paloParent == plo ? static_cast<ALO*>(ploNew) : paloParent);
        // orig: 0x0015C320 (the grouper's CloneLo: each entry of its list +0xE0 / +0xE4 cloned into the copy's list):
        // the copy's explosions are the ones a breakable's item 7 bursts
        if (FIsLoDerivedFrom(plo, 0x1F))
            if (const LOXCHILD* pg = dynamic_cast<const LOXCHILD*>(plo))
                if (LOXCHILD* pgNew = dynamic_cast<LOXCHILD*>(ploNew))
                    if (std::find(pg->aploChild.begin(), pg->aploChild.end(), pc) != pg->aploChild.end()) pgNew->aploChild.push_back(pcNew);
    }
    s_aploLoading.pop_back();
    return ploNew;
}

// orig 0x0013FB40 LoadSwObjsFromBrx
void LoadSwObjsFromBrx(SW* psw, ALO* paloParent, CBinaryInputStream* pbis, LOSINK* psink)
{
    LOADFN(0x13FB40);
    LOSINK sinkStack;
    sinkStack.fStack = true;
    int c = pbis->S16Read();
    if (c <= 0) return;
    do
    {
        int16_t as[128];
        const int k = pbis->S8Read();
        if (k > 0 || k < -2)
        {
            if (k * 2 > static_cast<int>(sizeof(as))) throw std::runtime_error("LoadSwObjsFromBrx: filter list too long");
            pbis->Read(static_cast<std::size_t>(k) * 2, as);
        }
        LOSINK* psinkCur = psink;
        const CID cid = pbis->S16Read();
        const OID oid = pbis->S32Read();
        const int n = pbis->S16Read();
        const int cb = pbis->S32Read();
        if (k < -2 || k > 0)
        {
            if (!FFilterMatches(k, as))
            {
                if (static_cast<unsigned>(cid - 0x84) < 2)
                {
                    // missions and tasks are always created, marked at +0x50
                    LO* plo = PloNew(cid, psw, paloParent, oid, -1);
                    plo->fMission = true;
                    ++g_brxstats.cmission;
                    extern std::unordered_map<int, int> g_mpoidiwrecTask;   // (debug harness: autoplay.cpp)
                    if (k > 0) g_mpoidiwrecTask[oid] = as[0];
                    LoadLoBody(plo, pbis);
                    if (psink) psink->Append(plo);
                }
                else
                {
                    pbis->Skip(cb);
                    ++g_brxstats.cskipFilter;
                }
                continue;
            }
        }
        else if (k < 0)
        {
            // an object of a shared group: loaded once (k = -2: anywhere; k = -1: per group instance)
            if (k == -2)
            {
                ++g_brxstats.cshared2;
                if (PloFindSwObjectByOid(psw, oid))
                {
                    pbis->Skip(cb);
                    ++g_brxstats.cskipShared;
                    continue;
                }
                sinkStack.n = 1;
            }
            else
            {
                ++g_brxstats.cshared1;
                // TODO: the original searches the namespace of (iwe, -1) only (0x15E958 with 0x805)
                bool fFound = false;
                for (LO* p : psw->mpcidaplo[cid])
                    if (p->oid == oid && p->igrp == psw->iwe) { fFound = true; break; }
                if (fFound)
                {
                    pbis->Skip(cb);
                    ++g_brxstats.cskipShared;
                    continue;
                }
                sinkStack.n = 0;
            }
            psinkCur = &sinkStack;
            sinkStack.m = (psw->iwe << 16) | 0xFFFF;
        }
        ++g_brxstats.cobj;
        if (FCidDerivedFrom(cid, 6))
        {
            ++g_brxstats.cproxy;
            extern void LoadProxyFromBrx(SW*, ALO*, CID, OID, int, CBinaryInputStream*, LOSINK*);
            LoadProxyFromBrx(psw, paloParent, cid, oid, n, pbis, psinkCur);
        }
        else
        {
            const std::size_t ibStart = pbis->m_ib;
            LO* plo = PloNew(cid, psw, paloParent, oid, n);
            if (!plo->pcls->pfnLoad)
                throw std::runtime_error("no native loader for " + StrClassName(cid) + " (orig 0x" +
                                         [&] { char a[16]; std::snprintf(a, sizeof(a), "%06X", plo->pcls->pfnLoadOrig); return std::string(a); }() + ")");
            LoadLoBody(plo, pbis);
            if (pbis->m_ib - ibStart != static_cast<std::size_t>(cb) && ++g_brxstats.ccbMismatch <= 10)
                std::fprintf(stderr, "note: %s oid %d read %zu bytes, record says %d\n", StrClassName(cid).c_str(), oid,
                             pbis->m_ib - ibStart, cb);
            if (psinkCur) psinkCur->Append(plo);
        }
    } while (--c != 0);
}
int OidOfLo(const LO* plo) { return plo ? plo->oid : -1; }
int CidOfLo(const LO* plo) { return plo ? static_cast<int>(plo->Cid()) : -1; }
