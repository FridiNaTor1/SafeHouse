// The barriers (cid 0x56, Sly 3 0x61; module 0x1320A8..0x132298): an ALO whose collision applies to some objects only (the
// intro's walls Murray walks through, the guards' fences, the hacks' shot stops): its lists of objects and classes and its
// mode (Sly 2: +0x150 / +0x164 / +0x178, EOPIDs 456..458; Sly 3: +0x130 / +0x144 / +0x158, EOPIDs 510 / 512 / 513).
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

// orig: 0x00132140 / orig3: 0x001392A0 (vt slot 65: the overlap kind against pOther: the lists matched, mode 0: 2
// (collides), else 1 (passes); mode 1: 3 (passes), else the ALO's own). Sly 3's lists: the objects +0x130 / +0x134
// (EOPID 510, optid 601: 0x106C68 -> 0x139510), the classes +0x144 / +0x148 (EOPID 512, optid 603: 0x106DF0 ->
// 0x1395C0, schema 7: a class id), at most four each; the mode +0x158 (EOPID 513). (s3-hack round 10: the maps pair
// Sly 3's 512 with Sly 2's object list 456, so the port read the class list as oids: e.g. Ep3 hack 2's start barrier
// 115611 (512 = 76, the pieces 0x4C) let the turrets' shots through to the idle ship; the reference's stop on it.)
int OverlapKindBarrier(ALO* p, ALO* pOther)
{
    bool f = false;
    bool fMode = false;
    int coid = 0, ccid = 0;
    // (Sly 3's options: 510 the objects (thunk 0x106C68 -> AddBarrierObject 0x139510), 512 the classes (0x106DF0 ->
    // AddBarrierClass 0x1395C0), 513 the mode (+0x158). The game table maps Sly 2's 456 (objects) to 512 and has no row
    // for 457 (classes): every class list was read as oids and every object list skipped, so no Sly 3 barrier ever
    // held anything (m_ext's elevator barrier 15282, classes {0x34}: Sly walked off the elevator's roof during the
    // ride and the scene after it waited for him in trigger 16407 with the player paused)
    constexpr int EOPID_OBJECTS3 = 510, EOPID_CLASSES3 = 512;
    for (const OPT& o : p->aopt)
    {
        if (o.eopid == EOPID_OBJECTS3)
            for (const OPTVAL& v : o.aval)
                if (coid++ < 4 && FOidOrAlias(pOther, v.n)) f = true;
        if (o.eopid == EOPID_CLASSES3)
            for (const OPTVAL& v : o.aval)
                if (ccid++ < 4 && v.n >= 0 && FCidDerivedFrom(pOther->Cid(), static_cast<CID>(v.n))) f = true;  // 0x13B500
        if (o.eopid == 513 && !o.aval.empty()) fMode = o.aval.front().n != 0;
    }
    static const bool s_fPrint = std::getenv("SLY2N_BARRIERPRINT") != nullptr;   // (debug: the barriers against the player)
    if (s_fPrint && (pOther->oid == 7 || std::getenv("SLY2N_BARRIERALL")))
    {
        std::printf("barrier %d vs %d: listed %d mode %d ->", p->oid, pOther->oid, f, fMode);
        for (const OPT& o : p->aopt)
            if (o.eopid >= EOPID_OBJECTS3 && o.eopid <= 513)
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

constexpr int kEopidObjects3 = 510;
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
        // (s3-plane) the scripts' list methods (Sly 3 EOPIDs 510 / 511, optids 601 / 602; thunks 0x106C68 (an oid) /
        // 0x106CF0 (an object, 0x139538: its oid) -> 0x139510 (appended to the oid list +0x150 / +0x154, at most 4),
        // 0x106D78 -> 0x139590 (the list cleared)): e.g. Ep5's ships add their character to the deck barrier 44249 when a
        // battle starts (the ship's s8 handler), so only he stands on that deck
        auto addOid = [](LO* plo, int oid) {
            if (!plo || oid == -1 || std::getenv("SLY3_NOBARRIERLIST")) return;
            // (the object list is Sly 3's EOPID 510 as OverlapKindBarrier reads it; S3EOPID(456) is 512, the class list:
            // the listed character's oid was read as a class, so a ship's deck barrier 44249 never held its character)
            int c = 0;
            for (const OPT& o : plo->aopt)
                if (o.eopid == kEopidObjects3) c += int(o.aval.size());
            if (c >= 4) return;
            OPT o;
            o.eopid = kEopidObjects3;
            OPTVAL v;
            v.n = oid;
            o.aval.push_back(v);
            plo->aopt.push_back(o);
            if (std::getenv("SLY2N_BARRIERPRINT")) std::printf("barrier %d: oid %d listed\n", plo->oid, oid);
        };
        static auto s_addOid = addOid;
        RegisterObjectMethod3(0x106C68, [](LO* plo, const std::vector<CRef>& a) {
            if (!a.empty()) s_addOid(plo, a[0].m_tagk == TAGK_S32 ? a[0].m_n : (dynamic_cast<LO*>(a[0].m_pbasic) ? dynamic_cast<LO*>(a[0].m_pbasic)->oid : -1));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x106CF0, [](LO* plo, const std::vector<CRef>& a) {
            LO* p = !a.empty() ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
            if (p) s_addOid(plo, p->oid);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x106D78, [](LO* plo, const std::vector<CRef>&) {
            if (!plo) return CRef(TAGK_Void);
            for (OPT& o : plo->aopt)
                if (o.eopid == kEopidObjects3) o.aval.clear();
            return CRef(TAGK_Void);
        });
    }
} s_initbarrier;
}  // namespace

// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetBarriers() { MpploToucher().clear(); }

NATIVEFN(0x132140, OverlapKindBarrier);
NATIVEFN(0x132238, OnBarrierXpNew);
