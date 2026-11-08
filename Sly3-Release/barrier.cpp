// The barriers (cid 0x56, module 0x1320A8..0x132298): an ALO whose collision applies to some objects only (the
// intro's walls Murray walks through, the guards' fences): its lists of objects (+0x150 count, +0x154 the oids: EOPID
// 456) and classes (+0x164 / +0x168: EOPID 457), and the mode +0x178 (EOPID 458).
#include "alo.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "cls.h"
#include "clock.h"
#include "frame.h"
#include "lo.h"
#include "objmethod.h"
#include "so.h"
#include "update.h"
#include "vtnative.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

int OverlapKindAlo(ALO* pa, ALO* pb);  // bbmark.cpp: 0x1F4FC8

// orig: 0x001320C8 (vt slot 7, InitLo: the ALO's (0x1F2108: InitAloPhys at the load), then flag bit 34: the barrier is
// its own physics root (0x1F2798), so its entries get their own attachment instead of joining the parent's (e.g. the
// Ep3 pool water 97739's electric barrier 103109: natively its touch was the water's sensor and hit the captured Sly);
// called by PloNew (brx.cpp) before the load's options (EOPID 39 may change the bit)
void InitBarrier(LO* plo)
{
    SORT& s = Sort(static_cast<ALO*>(plo));
    s.grf148 |= 1ull << 34;
}

namespace
{
// orig: 0x001A6290 (the object's oid, or one of its names: the instance's alias)
bool FOidOrAlias(const LO* plo, OID oid) { return plo->oid == oid || plo->oidInstance == oid; }

// orig: 0x00132140 (vt slot 61: the overlap kind against pOther: the lists matched, mode 0: 2 (collides), else 1
// (passes); mode 1: 3 (passes), else the ALO's own)
int OverlapKindBarrier(ALO* p, ALO* pOther)
{
    bool f = false;
    bool fMode = false;
    int coid = 0, ccid = 0;
    for (const OPT& o : p->aopt)
    {
        if (o.eopid == S3EOPID(456))
            for (const OPTVAL& v : o.aval)
                if (coid++ < 4 && FOidOrAlias(pOther, v.n)) f = true;
        if (o.eopid == S3EOPID(457))
            for (const OPTVAL& v : o.aval)
                if (ccid++ < 4 && v.n >= 0 && FCidDerivedFrom(pOther->Cid(), static_cast<CID>(v.n))) f = true;  // 0x132B10
        if (o.eopid == S3EOPID(458) && !o.aval.empty()) fMode = o.aval.front().n != 0;
    }
    static const bool s_fPrint = std::getenv("SLY2N_BARRIERPRINT") != nullptr;   // (debug: the barriers against the player)
    if (s_fPrint && (pOther->oid == 7 || std::getenv("SLY2N_BARRIERALL")))
    {
        std::printf("barrier %d vs %d: listed %d mode %d ->", p->oid, pOther->oid, f, fMode);
        for (const OPT& o : p->aopt)
            if (o.eopid >= 456 && o.eopid <= 458)
                for (const OPTVAL& v : o.aval) std::printf(" %d:%d", o.eopid, v.n);
        std::printf(" kind %d\n", fMode ? (f ? 3 : OverlapKindAlo(p, pOther)) : (f ? 2 : 1));
    }
    if (fMode) return f ? 3 : OverlapKindAlo(p, pOther);
    return f ? 2 : 1;
}

// +0x17C: the object the barrier last told its splice event 5 about (EOPID 459, option 534 of the class)
std::unordered_map<const LO*, LO*>& MpploToucher()
{
    static std::unordered_map<const LO*, LO*> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mp;
}

// orig: 0x00132298 (posted by 0x132238): each object in contact with the barrier (its attachment's overlaps'
// contacts: the other end's root, 16 distinct at most) put in +0x17C and told by the splice event 5 (vt slot 3).
// E.g. world 27's barrier 55326: its s5 hands (geto self 534) to 55263's 1579, which removes it under Sly.
void TellBarrierContacts(void* pv, int)
{
    ALO* p = static_cast<ALO*>(pv);
    OXA* poxa = Poxa(p);
    LO* ap[16];
    int c = 0;
    for (OX* pox = poxa ? poxa->pox : nullptr; pox && c < 16; pox = pox->poxNext)
        for (XP* pxp = pox->pxp; pxp && c < 16; pxp = pxp->pxpNext)
        {
            LO* pT = pxp->axpd[pxp->axpd[0].paloRoot == p ? 1 : 0].paloRoot;
            int i = 0;
            while (i < c && ap[i] != pT) ++i;
            if (i >= c) ap[c++] = pT;
        }
    for (int i = 0; i < c; ++i)
    {
        MpploToucher()[p] = ap[i];
        if (std::getenv("SLY2N_BARRIERPRINT"))
            std::printf("barrier %d: event 5, toucher %d t %.3f\n", p->oid, ap[i] ? ap[i]->oid : -1, g_clock.t);
        HandleLoSpliceEvent(p, 5, false, 0, nullptr);
    }
}

// orig: 0x00132238 (vt slot 55: a new contact): the base (0x1F4FB0: nothing), then when the barrier's frame binds
// event 5, 0x132298 posted to the world's deferred calls (0x149AB8 with D_002A4790)
void OnBarrierXpNew(ALO* p, XP*, int)
{
    if (p->pframe && p->pframe->PrefFindBinding(5, 1)) PostDeferredCall(p, 0x132298, TellBarrierContacts);
}

struct INITBARRIER
{
    INITBARRIER()
    {
        // EOPID 459 (option 534 of class 0x56; grf 2, the field +0x17C)
        RegisterObjectGetter(459, [](LO* plo) {
            CRef ref;
            const auto it = MpploToucher().find(plo);
            ref.SetBasic(it != MpploToucher().end() ? it->second : nullptr);
            return ref;
        });
    }
} s_initbarrier;
}  // namespace

// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetBarriers() { MpploToucher().clear(); }

NATIVEFN(0x132140, OverlapKindBarrier);
NATIVEFN(0x132238, OnBarrierXpNew);
