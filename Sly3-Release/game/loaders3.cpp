// The Sly 3 loaders of the game logic's classes: the scripts (SCPT 0x87), the tasks (TASK 0xA9,
// MISSION 0xA8), the state machines (SM 0x84), the animation segments (ASEG 0x82 / 0x83) and the managers m_ext has
// (0x8F, 0x96, 0x97, 0xAA). Each is registered by its Sly 3 vtable slot-14 function (cls.cpp InitClasses) and reads
// what the old port's census checked over every object of the 39 worlds (Sly3-research/old-Sly3-Release-2026-11-05:
// sly3/loaders3.cpp, aseg3.cpp, scpt3.cpp), applied to this fork's classes.

#include "loaders3.h"

#include "aseg3.h"
#include "scpt3.h"
#include "sly3.h"

#include "../bis.h"
#include "../brx.h"
#include "../sm.h"
#include "../task.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace
{
// LoadLoFromBrx (orig3 0x1C7D98: the object's options; loaders.cpp's, taken from the registry)
PFNLOAD s_pfnLoadLo = nullptr;
void LoadLoFromBrx(LO* plo, CBinaryInputStream* pbis) { s_pfnLoadLo(plo, pbis); }

// orig3: 0x2250C0 (SCPT 0x87): LoadLoFromBrx, the command list (0x225220: scpt3.cpp)
void LoadScpt3FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x2250C0);
    LoadLoFromBrx(plo, pbis);
    static_cast<LOSCPT3*>(plo)->acmd = AcmdLoadScpt3(pbis);
    // (debug) SLY3_SCPTCENSUS=1: every script's command ids as loaded (tools/s3script/census3.py)
    if (static const bool s_f = std::getenv("SLY3_SCPTCENSUS") != nullptr; s_f)
    {
        std::printf("scptcensus %d:", plo->oid);
        for (const SCPTCMD3& c : static_cast<LOSCPT3*>(plo)->acmd) std::printf(" %02X", c.id);
        std::printf("\n");
        if (std::getenv("SLY3_SCPTCENSUS")[0] == '2')  // (2: each command with its oid and arguments)
            for (const SCPTCMD3& c : static_cast<LOSCPT3*>(plo)->acmd)
            {
                std::printf("scptcmd %d %02X oid %d:", plo->oid, c.id, c.oid);
                for (int32_t n : c.an) std::printf(" %d", n);
                std::printf("\n");
            }
    }
}

// orig3: 0x238FF8 (SM 0x84; Sly 2 0x1EFAC0): the states (objects of the SM's parent), U8 x {S32, S32}, U8 x {S32, S32,
// S32, S32, F32, S32}, the options
void LoadSm3FromBrx(LO* plo, CBinaryInputStream* pbis) { LoadSmFromBrx(plo, pbis); }

// orig3: 0x283150 (0xAA, new): the LO, a debug string (0x119830: U16-counted bytes), then NProgress of it (no reads)
void LoadAA3FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x283150);
    LoadLoFromBrx(plo, pbis);
    pbis->SkipStringSw();
}

// orig3: 0x16F768 (0x8F, new): the LO, U8 c x {S32, S8, S8, S32, S32, a 64-byte key (ReadCb64)} (+0x40 count)
void Load8F3FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x16F768);
    LO8F3* p = static_cast<LO8F3*>(plo);
    LoadLoFromBrx(plo, pbis);
    p->aentry.resize(pbis->U8Read());
    for (LO8F3::ENTRY& e : p->aentry)
    {
        e.a = pbis->S32Read();
        e.b = pbis->S8Read();
        e.c = pbis->S8Read();
        e.d = pbis->S32Read();
        e.e = pbis->S32Read();
        pbis->ReadRecord64(e.abKey.data());
    }
}

// orig3: 0x172608 (0x96): the LO between allocations (PloFindSwNearest / SetLoMatrix: no reads)
void Load963FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x172608);
    LoadLoFromBrx(plo, pbis);
}

// orig3: 0x16EE98 (0x97): LoadLoFromBrx, S32 n x {a file key (ReadCb64), S32, S32}, S32 m x {S8 kind: 1 S32 S32,
// 0 / 2 S32}, then 0x196C18 / InitZpr 0x196AA8 (no reads)
void Load973FromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x16EE98);
    LO97* p = static_cast<LO97*>(plo);
    LoadLoFromBrx(plo, pbis);
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

template <class T> LO* PloNewT() { return new T(); }
}  // namespace

void RegisterScriptLoaders3(std::unordered_map<uint32_t, PFNLOAD>& m)
{
    s_pfnLoadLo = m.at(0x1C7D98);
    // (MISSION 0xA8 0x185EB8 and TASK 0xA9 0x187F08 read as Sly 2's 0x1624A8 / 0x1635F8: loaders.cpp registers them)
    m[0x2250C0] = LoadScpt3FromBrx;     // SCPT 0x87
    m[0x238FF8] = LoadSm3FromBrx;       // SM 0x84
    m[0x135EE8] = LoadAsegObjFromBrx3;  // ASEG 0x82 / ASEGBL 0x83 (aseg3.cpp)
    m[0x283150] = LoadAA3FromBrx;       // 0xAA
    m[0x16F768] = Load8F3FromBrx;       // 0x8F
    m[0x172608] = Load963FromBrx;       // 0x96
    m[0x16EE98] = Load973FromBrx;       // 0x97
}

PFNNEW PfnNewScript3(CID cid)
{
    switch (cid)
    {
    case 0x87: return PloNewT<LOSCPT3>;
    case 0x8F: return PloNewT<LO8F3>;
    case 0x97: return PloNewT<LO97>;
    default: return nullptr;
    }
}
