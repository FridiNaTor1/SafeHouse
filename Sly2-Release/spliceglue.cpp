#if defined(__SANITIZE_ADDRESS__)
#include <set>
#include <sanitizer/asan_interface.h>
#endif
#include "proc.h"
#include "actor.h"
#include "cls.h"
#include "spliceglue.h"
#include "cmworld.h"
#include "shd.h"
#include "glob.h"
#include "xfhandle.h"
#include "cm.h"
#include "so.h"
#include "clock.h"
#include "asega.h"
#include "objmethod.h"
#include "po.h"
#include "vehicle.h"

#include "activate.h"
#include "animfind.h"

#include "brx.h"
#include "disc.h"
#include "lo.h"
#include "sidebag.h"
#include "smrun.h"
#include "spliceobj.h"
#include "sw.h"
#include "transition.h"
#include "sound.h"
#include "hudwidget.h"
#include "ai.h"
#include "aijobs.h"

#include <algorithm>
#include <set>
#include <cstring>
#include "guard.h"
#include <functional>
#include <map>

void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)
int CClueNeededCur();     // vault.cpp, orig 0x1C7D00
int CClueCollectedCur();  // vault.cpp, orig 0x1C7D08
namespace
{
SW* s_psw = nullptr;

LO* PloFromBasic(BASIC* pbasic) { return dynamic_cast<LO*>(pbasic); }

// The objects of plo's namespace (what it created while loading, recursively), first match.
LO* PloFindIn(LO* plo, OID oid)
{
    if (!plo) return nullptr;
    for (LO* pc : plo->aploOwned)
    {
        if (pc->oid == oid || pc->oidInstance == oid) return pc;
        if (LO* p = PloFindIn(pc, oid)) return p;
    }
    return nullptr;
}

// orig: an object's namespace node (+0x3C) as 0x15E3E8 checks it (natively: the instance that placed it, inherited
// down the parents (ploNs), or a pool row's own node (ploNsRow, the object's own: not inherited through a parent, as a
// world object put under a pool object keeps the world's node); null: the world's node)
const LO* PloNsOf(const LO* plo)
{
    if (plo && plo->ploNsRow) return plo->ploNsRow;
    for (const LO* p = plo; p; p = p->paloParent)
        if (p->ploNs) return p->ploNs;
    return nullptr;
}
// orig: 0x15E3E8's check for an object of k -1 (InitLo's default): its node must be on the context's node chain (the
// context's node, then the world's). Applied natively to the pool rows' nodes only (the placed instances' objects
// keep the port's older fallbacks): a row's object is not found from another row (nor, first, from the world), so
// each row's scripts find their own row's objects (the Ep8 TNT barrels' charges 18407, re-parented by name after their
// slot 26 left the barrel)
bool FNsRowVisible(const LO* plo, const LO* ploCtx)
{
    if (!plo->ploNsRow) return true;
    return ploCtx && PloNsOf(ploCtx) == plo->ploNsRow;
}

// (the original's match 0x15E3E8 checks the object's channel against the context's list (G_PCH_DEFAULT without
// one): the objects of a streamed group (a proxy's row, LO::igrp) are on their row's channel, not the default
// list's. Natively the channels are not modelled (TODO(world)): an object of the context's group is taken first,
// then one of the world's base group (-1), then any.)
LO* PloFindGlobalNs(OID oid, const LO* ploCtx, bool fNsRow);
LO* PloFindGlobal(OID oid, const LO* ploCtx = nullptr)
{
    if (!s_psw) return nullptr;
    // the pool rows' nodes first (FNsRowVisible); a context outside the rows keeps the older fallback (any match) when
    // nothing else has the name, a row's context does not (another row's object is never its own)
    if (LO* plo = PloFindGlobalNs(oid, ploCtx, true)) return plo;
    if (ploCtx && ploCtx->ploNsRow) return nullptr;
    return PloFindGlobalNs(oid, ploCtx, false);
}
LO* PloFindGlobalNs(OID oid, const LO* ploCtx, bool fNsRow)
{
    const auto fVisible = [&](const LO* plo) { return !fNsRow || FNsRowVisible(plo, ploCtx); };
    const int igrpCtx = ploCtx ? ploCtx->igrp : -1;
    // (a copy's tree: the context inside a cloned object (a spawner's guard) finds that copy's objects, not the
    // original's: e.g. a guard's class-0x70 child finding its owner 11539 at its set-up)
    if (ploCtx)
    {
        const LO* ploRootCtx = ploCtx;
        while (ploRootCtx->paloParent) ploRootCtx = ploRootCtx->paloParent;
        for (LO* const plo : AploOfOid(s_psw, oid))
            if (plo->oid == oid && fVisible(plo))
            {
                const LO* ploRoot = plo;
                while (ploRoot->paloParent) ploRoot = ploRoot->paloParent;
                if (ploRoot == ploRootCtx && ploRoot->ploCloneOf) return plo;
            }
    }
    LO* ploBase = nullptr;
    LO* ploAny = nullptr;
    // (an instance named by its PROXY record's oid (the alias below) is taken before another instance of the same
    // group whose first object has that oid but another record's name: e.g. the intro's doors 147244 / 147246, two
    // instances of the group of ALO 147246)
    bool fAlias = false;
    for (LO* const plo : AploOfOid(s_psw, oid))
        if (plo->oidInstance == oid) { fAlias = true; break; }
    for (LO* const plo : AploOfOid(s_psw, oid))
        if (plo->oid == oid && fVisible(plo) && !(fAlias && plo->oidInstance != -1 && plo->oidInstance != oid))
        {
            if (plo->igrp == igrpCtx) return plo;
            if (!ploBase && plo->igrp == -1) ploBase = plo;
            if (!ploAny) ploAny = plo;
        }
    if (ploBase || ploAny) return ploBase ? ploBase : ploAny;
    // (0x15E678's second hash 0x15E3C0, searched with 0x15E958's 0x200: the instances' aliases, the PROXY
    // records' oids naming their instances' first objects (0x1C99D8 at the attach): natively LO::oidInstance)
    for (LO* const plo : AploOfOid(s_psw, oid))
        if (plo->oidInstance == oid)  // (an alias's node is its row node's parent: seen from every context)
        {
            if (plo->igrp == igrpCtx) return plo;
            if (!ploBase && plo->igrp == -1) ploBase = plo;
            if (!ploAny) ploAny = plo;
        }
    return ploBase ? ploBase : ploAny;
}

// orig: 0x0015E958 (kind 1: below an object; 4: nearest shared namespace; 5: any). An object's
// name symbol is its oid.
BASIC* PbasicFindObjectImpl(int kind, SYMID symid, BASIC* pbasicRel, BASIC* pbasicScope);
// (debug: SLY2N_FINDPRINT=oid: the finds of that oid and what they return)
BASIC* PbasicFindObject(int kind, SYMID symid, BASIC* pbasicRel, BASIC* pbasicScope)
{
    BASIC* p = PbasicFindObjectImpl(kind, symid, pbasicRel, pbasicScope);
    if (static const char* s_sz = std::getenv("SLY2N_FINDPRINT"); s_sz && static_cast<int>(symid) == std::atoi(s_sz))
    {
        LO* plo = PloFromBasic(p);
        LO* ploRel = PloFromBasic(pbasicRel ? pbasicRel : pbasicScope);
        std::printf("find %d kind 0x%X rel %d: %p oid %d cid 0x%X\n", static_cast<int>(symid), kind, ploRel ? ploRel->oid : -1, (void*)p,
                    plo ? plo->oid : -1, plo ? plo->Cid() : -1);
    }
    return p;
}
BASIC* PbasicFindObjectImpl(int kind, SYMID symid, BASIC* pbasicRel, BASIC* pbasicScope)
{
    const OID oid = static_cast<OID>(symid);
    // orig: 0x0015E678 rejects the absent-name sentinel before either hash.
    if (oid == -1) return nullptr;
    LO* ploRel = PloFromBasic(pbasicRel ? pbasicRel : pbasicScope);
    if ((kind & 0xFF) == 4 && ploRel)
    {
        // orig: 0x0015E3E8's node check (an object's k -1, InitLo's default): a candidate in an instance's namespace
        // node matches only a context in that node (the context's node chain up to the world's). Bounce pads 129699
        // find their own landing locator 16758 this way (U51), not the first one of the world.
        // orig: 0x0015E3E8 uses the candidate's namespace node. An instance
        // locator can also have a scene parent; that parent must not erase the
        // instance's own node (Episode 2 landing locators under object106834).
        // (PloNsOf: also a pool row's own node, e.g. the TNT barrel's script finding its row's charge 18407)
        const auto nsOf = PloNsOf;
        if (s_psw)
        {
            const LO* nsCtx = nsOf(ploRel);
            bool fNs = false;
            for (LO* const up : AploOfOid(s_psw, oid))
                if ((up->oid == oid || up->oidInstance == oid) && nsOf(up)) { fNs = true; break; }
            if (fNs)
            {
                LO* ploFirst = nullptr;
                LO* ploBest = nullptr;
                int ipsBest = INT_MAX;
                for (LO* const up : AploOfOid(s_psw, oid))
                {
                    if (up->oid != oid && up->oidInstance != oid) continue;
                    const LO* ns = nsOf(up);
                    if (ns && ns != nsCtx) continue;
                    // case 4: the nearest marked ancestor first (the context's chain index, 0x15E678's ips: 1 the
                    // context, 2 its parent, ..), else the first unmarked match. E.g. Ep4's lightning puzzles: each
                    // crank's SCPT 88365 grabs its own handle 88369, a sibling under the crank, not the first crank's
                    // (they share the puzzle instance 88296 further up)
                    int ips = INT_MAX;
                    for (const LO* p = up; p && ips == INT_MAX; p = p->paloParent)
                    {
                        int i = 0;
                        for (const LO* r = ploRel; r; r = r->paloParent, ++i)
                            if (p == r) { ips = i; break; }
                    }
                    if (ips < ipsBest) { ipsBest = ips; ploBest = up; }
                    if (!ploFirst) ploFirst = up;
                }
                if (ploBest) return ploBest;
                if (ploFirst) return ploFirst;
            }
        }
        // orig: 0x0015E3E8 case 4: a closer marked ancestor replaces every farther match. A context such as
        // the prologue's spotlight 150325 must find its own ASEG 150321, rather than another spotlight's.
        // When no namespace is shared, retain the channel/group fallback below.
        if (LO* q = PloFindSwNearest(ploRel, oid))
            for (const LO* p = q; p; p = p->paloParent)
                for (const LO* r = ploRel; r; r = r->paloParent)
                    if (p == r) return q;
    }
    if ((kind & 0xFF) == 1 && ploRel)
    {
        // orig: 0x0015E678 marks the context's ancestor chain (ips 1 the context, 2 its parent, ..) and 0x0015E3E8
        // case 1 takes a match only when its first marked ancestor is the context itself: the object or one below
        // it, never one found through the context's parents or anywhere else in the world (Nil then). The RC
        // tank's target scan (17:2716) asks each guard for its 72793; a guard has none, so the guard itself is
        // aimed at; the old fallback up the parents / globally returned a tank's 72793 (the dead one's).
        if (ploRel->oid == oid || ploRel->oidInstance == oid) return ploRel;
        if (LO* q = PloFindIn(ploRel, oid)) return q;
        if (s_psw)
            for (LO* const up : AploOfOid(s_psw, oid))
                if (up->oid == oid || up->oidInstance == oid)
                    for (const LO* p = up->paloParent; p; p = p->paloParent)
                        if (p == ploRel) return up;
        return nullptr;
    }
    return PloFindGlobal(oid, ploRel);
}

// orig: 0x0015E678 (name lookup for built-ins 113/116)
int CFindObjects(int kind, SYMID symid, BASIC* pbasicRel, int cMax, BASIC** apbasic)
{
    const OID oid = static_cast<OID>(symid);
    // orig: 0x0015E678: no name cannot match default instance aliases.
    if (oid == -1) return 0;
    int c = 0;
    auto add = [&](LO* p) { if (c < cMax) apbasic[c] = p; ++c; };
    LO* ploRel = PloFromBasic(pbasicRel);
    if ((kind & 0xFF) == 1 && ploRel)
    {
        std::function<void(LO*)> walk = [&](LO* p) {
            for (LO* pc : p->aploOwned)
            {
                if (pc->oid == oid) add(pc);
                walk(pc);
            }
        };
        walk(ploRel);
    }
    else if (s_psw)
        for (LO* const plo : AploOfOid(s_psw, oid))
            // (a pool row's context sees no other row's objects: FNsRowVisible; others keep every match)
            if (plo->oid == oid && (!ploRel || !ploRel->ploNsRow || FNsRowVisible(plo, ploRel))) add(plo);
    return c < cMax ? c : cMax;
}

// orig: 0x0015E9C8, as used by 0x00124730: class and derived classes,
// in class-list order. These two match kinds have channel checking disabled.
int CFindObjectsByClass(int kind, int cid, BASIC* pbasicRel, int cMax, BASIC** apbasic)
{
    if (!s_psw || cid < 0 || cid >= CID_Max) return 0;
    const int k = kind & 0xFF;
    LO* ploRel = PloFromBasic(pbasicRel);
    if (k != 5 && (k != 1 || !ploRel)) return 0;
    int c = 0;
    for (int icid = cid; icid < CID_Max && FCidDerivedFrom(icid, cid); ++icid)
        for (LO* plo : s_psw->mpcidaplo[icid])
        {
            // orig: 0x0015E9C8 case 5: 0x105 searches active objects only,
            // before applying the result limit (TOM's nearest-guard query).
            if (k == 5 && (kind & 0x100) && !plo->fInWorld) continue;
            if (k == 1)
            {
                const LO* p = plo;
                while (p && p != ploRel) p = p->paloParent;
                if (!p) continue;
            }
            if (c < cMax) apbasic[c] = plo;
            ++c;
        }
    // orig: 0x0015E9C8: bit 0x200 requests the total count, even with no
    // output slots (the guard's 0x605 class-presence queries use cMax=0).
    return (kind & 0x200) ? c : std::min(c, cMax);
}

CRef RefFromOptval(const OPTVAL& v)
{
    CRef ref;
    switch (v.tagk)
    {
    case 0x15: ref.SetS32(v.n); return ref;
    case 0x16: ref.SetF32(v.ag[0]); return ref;
    case 0x17: ref.SetBool(v.n); return ref;
    case 0x1A: case 0x1C: ref.SetVector(glm::vec3(v.ag[0], v.ag[1], v.ag[2])); return ref;
    case 0x21: ref.SetBasic(v.pbasic); return ref;
    default: break;
    }
    // a field / setter option: by its kind
    switch (v.k < 24 ? v.k : 24)
    {
    case 0: ref.SetBool(v.n); break;
    case 1: ref.SetF32(v.ag[0]); break;
    case 2: case 9: case 11: case 4: ref.SetVector(glm::vec3(v.ag[0], v.ag[1], v.ag[2])); break;
    default: ref.SetS32(v.n); break;
    }
    return ref;
}

OPTVAL OptvalFromRef(const CRef& ref)
{
    OPTVAL v;
    switch (ref.m_tagk)
    {
    case TAGK_S32: v.tagk = 0x15; v.n = ref.m_n; break;
    case TAGK_F32: v.tagk = 0x16; v.ag[0] = ref.m_g; break;
    case TAGK_Bool: v.tagk = 0x17; v.n = ref.m_bool; break;
    case TAGK_Vector:
    {
        v.tagk = 0x1A;
        const glm::vec3 vec = ref.GetVector();
        v.ag = {vec.x, vec.y, vec.z, 0.0f};
        break;
    }
    case TAGK_Basic:
        // an object (e.g. the job markers' seto! of their task +0x160 at the start, read back at the player's entry)
        v.tagk = 0x21;
        v.pbasic = ref.m_pbasic;
        if (const LO* plo = dynamic_cast<const LO*>(ref.m_pbasic)) v.n = plo->oid;
        break;
    default: v.tagk = 0x24; break;
    }
    return v;
}

// orig 0x001FFCF8: an option id's property (EOPID) for the object's class (the sorted table D_00279AD0[cid]
// of (optid, EOPID*)); -1 when the class has none
int EopidFromOptid(CID cid, int optid)
{
    if (cid < 0) return -1;
    const uint32_t c = g_disc.U32Elf(0x279AD0 + static_cast<uint32_t>(cid) * 8);
    const uint32_t a = g_disc.U32Elf(0x279AD0 + static_cast<uint32_t>(cid) * 8 + 4);
    int lo = 0, hi = static_cast<int>(c) - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) >> 1;
        const int k = static_cast<int>(g_disc.U32Elf(a + static_cast<uint32_t>(mid) * 8));
        if (k == optid) return static_cast<int>((g_disc.U32Elf(a + static_cast<uint32_t>(mid) * 8 + 4) - 0x2AC880) / 0x28);
        if (optid < k) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

// The methods (properties with flags & 1: geto gives a function of the object, 0x1FFD98) the game has
// natively: 0x100A90 AddLo (vt slot 8), 0x100B88 RemoveLo (vt slot 9), 0x100A00 slot 38 (for the actors); the others do nothing yet.
CRef RefMethodAdd(BASIC* pbasic, int, CRef*)
{
    if (LO* plo = PloFromBasic(pbasic))
    {
        AddLo(s_psw, plo);
        if (std::getenv("SLY2N_METHODPRINT"))
            std::printf("add: %d %p parent %d %p: inworld %d removed %d\n", plo->oid, (void*)plo, plo->paloParent ? plo->paloParent->oid : -1,
                        (void*)plo->paloParent, plo->fInWorld, plo->fRemoved);
    }
    return CRef(TAGK_Void);
}
CRef RefMethodRemove(BASIC* pbasic, int, CRef*)
{
    if (LO* plo = PloFromBasic(pbasic)) RemoveLo(s_psw, plo);
    return CRef(TAGK_Void);
}
// orig 0x100A00: vt slot 38 with an object argument (placed at the object; actor.h)
CRef RefMethodSlot38(BASIC* pbasic, int cref, CRef* aref)
{
    LO* plo = PloFromBasic(pbasic);
    LO* plodest = cref > 0 && aref ? PloFromBasic(aref[0].m_pbasic) : nullptr;
    if (std::getenv("SLY2N_METHODPRINT"))
    {
        const glm::vec3 pos = plodest ? PosLoWorld(plodest) : glm::vec3(0.0f);
        std::printf("slot 38: %d -> %d (%.2f %.2f %.2f)\n", plo ? plo->oid : -1, plodest ? plodest->oid : -1, pos.x, pos.y, pos.z);
    }
    // (0x2052E0 the actors', 0x1F49C8 the ALOs': placed at the object)
    if (plo && plodest) TeleportLo(plo, plodest);  // (other classes: 0x1A6B88, e.g. a LOX placed at its new parent)
    return CRef(TAGK_Void);
}
CRef RefMethodStub(BASIC*, int, CRef*) { return CRef(TAGK_Void); }  // TODO(M4): the other methods

// (debug, audit: SLY2N_UNBOUND=1 prints once per kind / EOPID an option whose original function the port does not
// bind, as authored scripts reach it: "unbound method|getter|setter eopid E option O cid 0xC pfn 0xF")
void NoteUnbound(const char* szKind, int eopid, int optid, const LO* plo, uint32_t pfn)
{
    static const bool s_f = std::getenv("SLY2N_UNBOUND") != nullptr;
    if (!s_f) return;
    static std::set<std::pair<std::string, int>> s_seen;
    if (!s_seen.insert({szKind, eopid}).second) return;
    std::fprintf(stderr, "unbound %s eopid %d option %d cid 0x%X oid %d pfn 0x%X\n", szKind, eopid, optid,
                 plo ? plo->Cid() : -1, plo ? plo->oid : -1, pfn);
}

// orig: 0x001F85E0, class 0x96: a script reference to one tagged SOE of an ALO.
// Keep the selected entry's index when the native vector grows.
struct SOEBASIC : BASIC
{
    ALO* palo = nullptr;
    size_t isoe = 0;
    uint32_t worldSerial = 0;
};

// orig: 0x00101588 -> 0x001F85E0 / 0x001F6030 (option 195: the first entry with this tag).
CRef RefSoeHandle(LO* plo, const std::vector<CRef>& args)
{
    CRef result;
    result.SetBasic(nullptr);
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (!palo) return result;
    const int tag = NArg(args, 0);
    for (size_t i = 0; i < palo->asoe.size(); ++i)
        if (palo->asoe[i].tag == tag)
        {
            auto handle = std::make_shared<SOEBASIC>();
            handle->palo = palo;
            handle->isoe = i;
            handle->worldSerial = s_psw ? s_psw->serial : 0;
            result.SetOwnedBasic(std::move(handle));
            break;
        }
    return result;
}

// orig: 0x00256350 / 0x00256328 (option 122), 0x00256318 / 0x002562F0 (option 123).
CRef RefSoeOption(int bifk, SOEBASIC* handle, int optid, const CRef* pref)
{
    CRef result;
    // Native unload releases the world before clearing its script bindings.
    if (!s_psw || handle->worldSerial != s_psw->serial || handle->isoe >= handle->palo->asoe.size()) return result;
    SOE& entry = handle->palo->asoe[handle->isoe];
    if (bifk == 111)
    {
        if (optid == 122) result.SetS32(static_cast<int32_t>(entry.id));
        else if (optid == 123) result.SetS32(entry.state);
        return result;
    }
    if (bifk == 110 && pref && (optid == 122 || optid == 123))
    {
        const int32_t value = pref->RefCoerceS32().m_n;
        if (optid == 122 && std::getenv("SLY2N_SOEKINDPRINT"))  // (debug: who sets an entry's contact kind)
            std::printf("soe kind: %d tag %d 0x%X -> 0x%X t %.3f\n", handle->palo->oid, entry.tag, entry.id, static_cast<uint32_t>(value), g_clock.t);
        if (optid == 122) entry.id = static_cast<uint32_t>(value);
        else entry.state = static_cast<int16_t>(value);
        MarkAloDirty(handle->palo);
    }
    return CRef(TAGK_Void);
}

// An SMA's properties (class 0x88's table: 251 its object +0x28, 182 its transition +0x30, 481 its goal (set:
// 0x1F0660), 482 / 483 its state / next, 484 its speed (0x1F0850), 485 its SM, 486 its mask (0x1F1350); methods
// 0x1DF seek 0x1F0730, 0x1E0 0x1F11A8 (TODO: the time to the state), 0xEC snap 0x1F0948, 0xED retract 0x1F0598)
CRef RefSmaMethod(SMA* psma, uint32_t pfn, const CRef* pref)
{
    (void)pref;
    (void)psma;
    (void)pfn;
    return CRef(TAGK_Void);
}
SMA* s_psmaMethod = nullptr;
CRef RefSmaThunk(BASIC* pbasic, int argc, CRef* argv, int k)
{
    SMA* psma = dynamic_cast<SMA*>(pbasic);
    if (!psma) return CRef(TAGK_Void);
    const OID oid = argc > 0 ? argv[0].m_n : -1;
    switch (k)
    {
    case 0: SeekSma(psma, oid); break;
    case 1: { CRef r; r.SetF32(0.0f); return r; }
    case 2: SnapSmaGoal(psma, oid); break;
    case 3: RetractSma(psma); break;
    }
    return CRef(TAGK_Void);
}
CRef RefSmaSeek(BASIC* p, int c, CRef* a) { return RefSmaThunk(p, c, a, 0); }
CRef RefSmaTime(BASIC* p, int c, CRef* a) { return RefSmaThunk(p, c, a, 1); }
CRef RefSmaSnap(BASIC* p, int c, CRef* a) { return RefSmaThunk(p, c, a, 2); }
CRef RefSmaRetract(BASIC* p, int c, CRef* a) { return RefSmaThunk(p, c, a, 3); }

CRef RefSmaOption(int bifk, SMA* psma, int optid, const CRef* pref)
{
    CRef r;
    auto method = [&](PFNTHUNK pfn, int creq) {
        auto pm = std::make_shared<CMethod>();
        pm->m_pbasic = psma;
        pm->m_pfnthunk = pfn;
        pm->m_crefReq = creq;
        CRef rm;
        rm.SetMethod(pm);
        return rm;
    };
    if (bifk == 111)
    {
        switch (optid)
        {
        case 0xFB: if (psma->palo) r.SetBasic(psma->palo); return r;
        case 0x1DE: r.SetS32(OidSmaGoal(psma)); return r;
        case 0x1E1: r.SetS32(OidSmaCur(psma)); return r;
        case 0x1E2: r.SetS32(OidSmaNextOrCur(psma)); return r;
        case 0x1E3: r.SetF32(psma->svtLocal); return r;
        case 0x1E4: if (psma->psm) r.SetBasic(psma->psm); return r;
        // 182 (EOPID 378, type 0x2086 at field +0x30): its ASEGA (e.g. Ep8 final's job method 1266 snaps Clock-La's
        // SMA 7697 to her talk pose 16939, then its ASEGA's transforms with ASEGA option 248)
        case 0xB6: if (psma->pasega) r.SetBasic(psma->pasega); return r;
        case 0x1E5: r.SetS32(static_cast<int>(psma->grfMask)); return r;
        case 0x1DF: return method(RefSmaSeek, 1);
        case 0x1E0: return method(RefSmaTime, 1);
        case 0xEC: return method(RefSmaSnap, 1);
        case 0xED: return method(RefSmaRetract, 0);
        default: return r;
        }
    }
    if (bifk == 110 && pref)
    {
        switch (optid)
        {
        case 0x1DE: SetSmaGoal(psma, pref->m_n); break;
        case 0x1E3: SetSmaSpeed(psma, pref->m_tagk == TAGK_F32 ? pref->m_g : static_cast<float>(pref->m_n)); break;
        case 0x1E5: SetSmaMask(psma, static_cast<uint32_t>(pref->m_n)); break;
        default: break;
        }
    }
    return CRef(TAGK_Void);
}

// orig 0x123EB0: 108 addo, 109 ensureo, 110 seto!, 111 geto (0x200BA0): ids up to 0x4D0 are the class's
// properties: a method (flags & 1) gives a function; the others are the values the object keeps (the loaded
// options; the activation's radius (EOPID 13) and kind (14) live; TODO(M3): the classes' getters / setters)
// The world (the original's SW 0x3DD490, cid 1) as the splice sees it (find-player-object, bif 117, 0x124640):
// its properties 844 / 845 the current player (0x1C4840 / 0x1C4A48), 846 the forced one (0x1C4A60); the
// others are kept as set (TODO(world))
struct SWBASIC : BASIC
{
} s_swbasic;

// The camera (g_cm 0x3DDF40 here 0x2DDF40, cid 0x87) as the splice sees it (find-camera-object, bif 119, 0x1246E0):
// Its script properties use class 0x87's original typed descriptors.
struct CMBASIC : BASIC
{
} s_cmbasic;
// orig: 0x001026E0 (EOPID 159): coerce the jolt kind to an integer, call
// 0x14F750, and return void. The camera already applies the authored shake.
CRef RefCmJolt(BASIC*, int c, CRef* a)
{
    if (c > 0) JoltCm(&g_cm, a[0].RefCoerceS32().m_n);
    return CRef(TAGK_Void);
}
CRef RefCmCut(BASIC*, int, CRef*)
{
    CutCm(&g_cm);  // 0x14D8C0
    return CRef(TAGK_Void);
}
CRef RefCameraOption(int bifk, int optid, const CRef* value)
{
    if (bifk == 111 && (optid == 233 || optid == 292))
    {
        auto pm = std::make_shared<CMethod>();
        pm->m_pbasic = &s_cmbasic;
        pm->m_pfnthunk = optid == 233 ? RefCmJolt : RefCmCut;
        pm->m_crefReq = optid == 233 ? 1 : 0;
        CRef rm;
        rm.SetMethod(pm);
        return rm;
    }
    CRef result;
    bool handled = true;
    if (bifk == 111)
    {
        switch (optid)
        {
        case 34: result.SetVector(g_cm.pos); break;  // orig: 0x256298
        case 35: result.SetMatrix(glm::mat4(g_cm.mat)); break;  // orig: 0x2562A0, OTYP4 (rotation only)
        case 36: result.SetVector(cmm::DecomposeEulerMatrix(g_cm.mat) * cmm::F32(0x42652EE0)); break;  // orig: 0x14F478
        case 293: result.SetF32(g_cm.radFOV); break;
        case 294: result.SetF32(g_cm.radFOVTarget); break;
        case 295: result.SetSmp({g_cm.smpaRadFOV.svFast, g_cm.smpaRadFOV.svSlow, g_cm.smpaRadFOV.dtFast}); break;
        case 296: result.SetF32(g_cm.sNearClip); break;
        case 297: result.SetF32(g_cm.sFarClip); break;
        case 298: result.SetF32(g_cm.sNearFog); break;
        case 299: result.SetF32(g_cm.sFarFog); break;
        case 300: result.SetS32(static_cast<int32_t>(g_cm.rgbaFog)); break;
        case 301: result.SetF32(g_cm.uFogMax); break;
        case 302: result.SetF32(g_cm.rMRD); break;
        case 303: result.SetF32(g_cm.sStereo); break;  // EOPID174: read current, write target
        default: handled = false; break;
        }
    }
    else if (bifk == 110 && value)
    {
        switch (optid)
        {
        case 34: SetCmPosMat(&g_cm, value->GetVector(), g_cm.mat); break;  // orig: 0x14F3C8
        case 35: SetCmPosMat(&g_cm, g_cm.pos, glm::mat3(value->GetMatrix())); break;  // orig: 0x14F428
        case 36: SetCmPosMat(&g_cm, g_cm.pos, cmm::BuildEulerMatrix(value->GetVector() * cmm::F32(0x3C8EFA36))); break;  // orig: 0x14F4E0
        case 293: SetCmFov(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F5A0
        case 294: SetCmFovTarget(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F5F8
        case 295:
        {
            const SMP smp = value->GetSmp();  // EOPID163: copy three fields, preserving acceleration limit
            g_cm.smpaRadFOV.svFast = smp.svFast; g_cm.smpaRadFOV.svSlow = smp.svSlow; g_cm.smpaRadFOV.dtFast = smp.dtFast;
            break;
        }
        case 296: SetCmNearClipAll(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F630
        case 297: SetCmFarClip(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F670
        case 298: SetCmNearFog(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F690
        case 299: SetCmFarFog(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F6B0
        case 300: SetCmRgbaFog(&g_cm, static_cast<uint32_t>(value->RefCoerceS32().m_n)); break;  // orig: 0x14F6F0
        case 301: SetCmUFogMax(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F6D0
        case 302: SetCmMrdRatio(&g_cm, value->RefCoerceF32().m_g); break;  // orig: 0x14F730
        case 303: g_cm.sStereoTarget = value->RefCoerceF32().m_g; break;
        default: handled = false; break;
        }
        result.SetTag(TAGK_Void);
    }
    else handled = false;
    if (handled)
    {
        if (std::getenv("SLY2N_CAMERABRIDGE"))
            std::printf("CAMERA_OPTION bif=%d option=%d tag=%d t=%.9g pos=%.9g,%.9g,%.9g mrd=%.9g\n", bifk, optid, int(result.m_tagk), g_clock.t, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.rMRD);
        return result;
    }
    static std::map<int, bool> s_mpWarned;
    if (!s_mpWarned[optid]) std::fprintf(stderr, "splice: the camera's option %d (bif %d) not ported\n", optid, bifk);
    s_mpWarned[optid] = true;
    return CRef(TAGK_Void);
}

// The HUD prompt widget (0x3DA0C0, cid 0x9A) as the splice sees it (bif 137, 0x125690): option 979 shows a string
// (0x10F460 -> 0x1DA4B8 (id, seconds; < 0 until ended)), 980 ends one (0x10F508 -> 0x1DA578); e.g. the safe
// house's "Press X to play as Sly" (SM 27339's state handler: 305 / 306 / 307 / 308 by the character)
struct PROMPTBASIC : BASIC
{
} s_promptbasic;
CRef RefPromptShow(BASIC*, int c, CRef* a)
{
    if (c >= 1) HudPrompt(a[0].m_tagk == TAGK_F32 ? int(a[0].m_g) : a[0].m_n, c >= 2 ? (a[1].m_tagk == TAGK_F32 ? a[1].m_g : float(a[1].m_n)) : -1.0f);
    return CRef(TAGK_Void);
}
CRef RefPromptEnd(BASIC*, int c, CRef* a)
{
    HudPromptEnd(c >= 1 ? (a[0].m_tagk == TAGK_F32 ? int(a[0].m_g) : a[0].m_n) : -1);
    return CRef(TAGK_Void);
}

// The sound manager (g_sound 0x3DD040, cid 0x98) as the splice sees it (find-world-object, bif 118, 0x124690):
// option 273 (EOPID 146) its method push-music (thunk 0x102430: 0x1FE150), 267 (EOPID 140) the dialog's time
// (0x1FE888). (media code, sound.h)
struct SNDBASIC : LO
{
} s_sndbasic;

CRef RefSoundOption(int bifk, int optid, const CRef* pref)
{
    CRef ref;
    const int eopid = EopidFromOptid(0x98, optid);
    if (bifk == 111 && eopid == 140)  // (a getter: 0x1FE888)
    {
        ref.SetF32(TDialog());
        return ref;
    }
    if (bifk == 111 && eopid >= 0)
    {
        const uint32_t vaEopid = 0x2AC880 + static_cast<uint32_t>(eopid) * 0x28;
        if ((g_disc.U32Elf(vaEopid + 4) & 1) &&
            FMakeObjectMethod(g_disc.U32Elf(vaEopid + 0x10), &s_sndbasic, static_cast<int>(g_disc.U32Elf(vaEopid + 0x18)), &ref))
            return ref;
    }
    static std::map<int, bool> s_mpWarned;
    if (!s_mpWarned[optid]) std::fprintf(stderr, "splice: the sound object's option %d (bif %d) not ported\n", optid, bifk);
    s_mpWarned[optid] = true;
    (void)pref;
    return CRef(TAGK_Void);
}

// Every object's option 3 (EOPID 2, the thunk 0x100228): is it of the class n (0x132B10) - a bool (a void answer
// is true to the scripts: job 2020's event 26 restarted the job at any script's start)
struct ISAMETHOD
{
    ISAMETHOD()
    {
        RegisterObjectMethod(0x100228, [](LO* plo, const std::vector<CRef>& a) {
            CRef r;
            r.SetBool(plo && FCidDerivedFrom(plo->Cid(), NArg(a, 0, -1)));
            return r;
        });
        // orig: 0x001008D8 -> 0x001A6290 (option 30: is the object named n: its oid, or an oid of its instance chain
        // +0x3C {+4 next, +8 oid, +0x10 owner} while the entries' owner is the object; natively LO::oidInstance, the
        // chain's only entry for the clones the worlds make). Unregistered it answered void (true), so 11:1990's
        // ballroom director (job 1981's frame symbols 3663 / 3664) never found a free person and its while loop
        // spun (1.5 million calls a frame, cut by the native guard) instead of adding the next one.
        RegisterObjectMethod(0x1008D8, [](LO* plo, const std::vector<CRef>& a) {
            const int oid = NArg(a, 0, -1);
            CRef r;
            r.SetBool(plo && (plo->oid == oid || plo->oidInstance == oid));
            return r;
        });
    }
} s_isamethod;

// The sound object's methods (cid 0x98's EOPIDs 137..150: the splice thunks 0x102038..0x102630)
struct SNDMETHODS
{
    SNDMETHODS()
    {
        using A = const std::vector<CRef>&;
        RegisterObjectMethod(0x102038, [](LO*, A a) { return RefRetainSound(PsndStartDialog(NArg(a, 0, -1), NArg(a, 1))); });  // 0x1FE5F0
        RegisterObjectMethod(0x1020D8, [](LO*, A) { StopDialog(); return CRef(TAGK_Void); });                          // 0x1FE738
        RegisterObjectMethod(0x102150, [](LO*, A a) { CRef r; r.SetBool(FDialogPlaying(NArg(a, 0, -1))); return r; });   // 0x1FE848
        RegisterObjectMethod(0x1021D0, [](LO*, A a) {  // 0x1FCFD0: the dialog line's length
            const SNDSTREAM* p = PstreamFindDialog(NArg(a, 0, -1));
            CRef r;
            r.SetF32(p ? p->tLength : 0.0f);
            return r;
        });
        RegisterObjectMethod(0x102250, [](LO*, A) { ++g_sound.fModeAlert; return CRef(TAGK_Void); });  // 0x1FEE90
        RegisterObjectMethod(0x1022C8, [](LO*, A) { --g_sound.fModeAlert; return CRef(TAGK_Void); });  // 0x1FEEA0
        RegisterObjectMethod(0x102340, [](LO*, A) { ++g_sound.fModeSneak; return CRef(TAGK_Void); });  // 0x1FEEB0
        RegisterObjectMethod(0x1023B8, [](LO*, A) { --g_sound.fModeSneak; return CRef(TAGK_Void); });  // 0x1FEEC0
        RegisterObjectMethod(0x102430, [](LO*, A a) { return RefRetainSound(PsndPushMusic(NArg(a, 0, -1))); });  // 0x1FE150
        RegisterObjectMethod(0x1024B0, [](LO*, A a) { PopMusic(NArg(a, 0, -1)); return CRef(TAGK_Void); });       // 0x1FE310
        RegisterObjectMethod(0x102538, [](LO*, A) { CRef r; r.SetF32(TMusicTop()); return r; });                   // 0x1FE3F8
        RegisterObjectMethod(0x1025A8, [](LO*, A a) { SetSoundMasterVolume(GArg(a, 0, 1.0f)); return CRef(TAGK_Void); });  // 0x1FE9F8
        RegisterObjectMethod(0x102630, [](LO*, A a) {  // 0x1FEA60 (kind 0)
            SetSoundChannelVolume(NArg(a, 0), 0, GArg(a, 1, 1.0f));
            return CRef(TAGK_Void);
        });
    }
} s_sndmethods;

std::map<int, CRef> s_mpWorldOption;  // (the world's other options: kept as set, TODO(world): their fields' uses)

CRef RefWorldOption(int bifk, int optid, const CRef* pref)
{
    CRef ref;
    if (!s_psw) return ref;
    if (bifk == 110 && optid == 846 && pref)  // EOPID 928: the forced player (0x1C4A60, SW +0x3F00)
    {
        SetPoForced(s_psw, dynamic_cast<LO*>(pref->m_pbasic));
        s_mpWorldOption[optid] = *pref;
        return CRef(TAGK_Void);
    }
    // EOPIDs 937 / 938 (options 855 / 856, grf 0x12, otype 6): SW +0x3C88 / +0x3C8C, the current checkpoint's
    // point and key that a death outside a job reloads (0x161A58 case 16); e.g. world 17's safe-house exit script
    // (object 55964) sets 855 to 27433, so the player respawns outside, not at the character select (0x201110)
    if ((optid == 855 || optid == 856) && (bifk == 110 || bifk == 111))
    {
        OID& oid = optid == 855 ? g_transition.oidPointCur : g_transition.oidKeyCur;
        if (bifk == 111)
        {
            ref.SetS32(oid);
            return ref;
        }
        if (pref) oid = NArg(std::vector<CRef>{*pref}, 0, -1);
        return CRef(TAGK_Void);
    }
    if (bifk != 111)
    {
        static std::map<int, bool> s_mpWarned;
        if (!s_mpWarned[optid]) std::fprintf(stderr, "splice: the world's option %d (bif %d) kept, its use not ported\n", optid, bifk);
        s_mpWarned[optid] = true;
        if (pref) s_mpWorldOption[optid] = *pref;
        return CRef(TAGK_Void);
    }
    switch (optid)
    {
    case 844: if (LO* p = PloPoCurrent(s_psw)) ref.SetBasic(p); break;
    case 845: if (LO* p = PloPoCurrent(s_psw)) ref.SetBasic(p); break;  // (0x1C4A48: with the forced one)
    case 859: ref.SetBool(g_gamepause.c78 > 0); break;  // EOPID 941, 0x20F0B0: the game paused (SW +0x3D78)
    case 862:  // EOPID 943: SW +0x4768, initialized by 0x1C7D00
    {
        ref.SetS32(CClueNeededCur());
        break;
    }
    case 863:  // EOPID 944: SW +0x4764, refreshed by 0x1C7C30 / 0x1C7D08
    {
        ref.SetS32(CClueCollectedCur());
        break;
    }
    default:
    {
        auto it = s_mpWorldOption.find(optid);
        if (it != s_mpWorldOption.end()) return it->second;
        // the world's methods (0x200BA0: an EOPID with flag 1 gives a method object of its thunk), e.g. 861
        // (EOPID 942, thunk 0x10C2E8: 0x195608, the abandon icon on a tracked target: the safe house door in a job)
        if (const int eopid = EopidFromOptid(0x01, optid); eopid >= 0)
        {
            const uint32_t vaEopid = 0x2AC880 + static_cast<uint32_t>(eopid) * 0x28;
            if ((g_disc.U32Elf(vaEopid + 4) & 1) &&
                FMakeObjectMethod(g_disc.U32Elf(vaEopid + 0x10), &s_swbasic, static_cast<int>(g_disc.U32Elf(vaEopid + 0x18)), &ref))
                return ref;
        }
        static std::map<int, bool> s_mpWarned;
        if (!s_mpWarned[optid]) std::fprintf(stderr, "splice: the world's option %d not ported\n", optid);
        s_mpWarned[optid] = true;
        break;
    }
    }
    return ref;
}

CRef RefOptionImpl(int bifk, BASIC* pbasic, int optid, const CRef* pref);
// (debug: SLY2N_OPTWATCH=id[,id..]: those options' reads / calls and their results)
CRef RefOption(int bifk, BASIC* pbasic, int optid, const CRef* pref)
{
    static const char* s_sz = std::getenv("SLY2N_OPTWATCH");
    CRef r = RefOptionImpl(bifk, pbasic, optid, pref);
    if (s_sz)
    {
        bool f = false;
        for (const char* q = s_sz; *q;)
        {
            if (std::atoi(q) == optid) f = true;
            const char* c = std::strchr(q, ',');
            if (!c) break;
            q = c + 1;
        }
        if (f)
        {
            std::string str;
            PrintRef(r, &str);
            LO* plo = dynamic_cast<LO*>(pbasic);
            std::printf("optwatch: bif %d option %d of %d: %s\n", bifk, optid, plo ? plo->oid : -1, str.c_str());
            if (r.m_tagk == TAGK_Proc && r.m_pproc)
            {
                std::string body;
                CRef b;
                b.SetPair(r.m_pproc->m_ppairCodeExpr);
                PrintRef(b, &body);
                std::printf("optwatch:   body %s\n", body.substr(0, 3000).c_str());
            }
        }
    }
    return r;
}
CRef RefOptionImpl(int bifk, BASIC* pbasic, int optid, const CRef* pref)
{
#if defined(__SANITIZE_ADDRESS__)
    if (pbasic && __asan_address_is_poisoned(pbasic)) { __asan_describe_address(pbasic); std::abort(); }
#endif
    if (pbasic && !FBasicLive(pbasic))
    {
        // (an object freed natively, still named by a script: its option reads nothing)
        static bool s_fWarned = false;
        if (!s_fWarned) std::fprintf(stderr, "splice: option %d of a freed object (bif %d): nothing\n", optid, bifk);
        s_fWarned = true;
        return CRef(TAGK_Void);
    }
    if (SOEBASIC* handle = dynamic_cast<SOEBASIC*>(pbasic)) return RefSoeOption(bifk, handle, optid, pref);
    if (XFBASIC* handle = dynamic_cast<XFBASIC*>(pbasic)) return RefXfHandleOption(bifk, handle, optid, pref);
    if (CSnd* psnd = dynamic_cast<CSnd*>(pbasic)) return RefSoundObjectOption(psnd, bifk, optid, pref);
    if (auto* attachment = dynamic_cast<VEHICLEATTACH*>(pbasic)) return RefVehicleAttachmentOption(attachment, bifk, optid, pref);
    if (pbasic == &s_swbasic) return RefWorldOption(bifk, optid, pref);
    if (pbasic == &s_cmbasic) return RefCameraOption(bifk, optid, pref);
    if (pbasic == &s_promptbasic)
    {
        if (bifk != 111 || (optid != 979 && optid != 980)) return CRef(TAGK_Void);
        auto pm = std::make_shared<CMethod>();
        pm->m_pbasic = pbasic;
        pm->m_pfnthunk = optid == 979 ? RefPromptShow : RefPromptEnd;
        pm->m_crefReq = optid == 979 ? 2 : 1;
        CRef rm;
        rm.SetMethod(pm);
        return rm;
    }
    if (pbasic == &s_sndbasic) return RefSoundOption(bifk, optid, pref);
    if (AIJOB* pjob = dynamic_cast<AIJOB*>(pbasic))
    {
        // BASIC's private bindings use the same path on jobs as on LO objects (0x200BA0..0x200D10).
        auto sidebag = [&]() -> CSidebag& {
            if (!pjob->psidebag) pjob->psidebag = PsidebagNew();
            return *pjob->psidebag;
        };
        if (bifk == 108 && pref)
        {
            CRef ref = *pref;
            return sidebag().RefAddBinding(optid, &ref);
        }
        if (bifk == 109 && pref)
        {
            CRef ref = *pref;
            if (sidebag().FFindBinding(optid, nullptr)) return sidebag().RefSetBinding(optid, &ref);
            return sidebag().RefAddBinding(optid, &ref);
        }
        if (optid > 0x4D0)
        {
            CRef ref;
            if (bifk == 111 && pjob->psidebag) pjob->psidebag->FFindBinding(optid, &ref);
            else if (bifk == 110 && pref)
            {
                ref = *pref;
                return sidebag().RefSetBinding(optid, &ref);
            }
            return ref;
        }
        CRef ref;
        if (bifk == 111 && optid == 236 && dynamic_cast<JOBSCRIPT*>(pjob))
        {
            // orig: 0x0010FA38 (EOPID1359) -> 0x00171C98: seek without executing the marker.
            auto pm = std::make_shared<CMethod>();
            pm->m_pbasic = pjob;
            pm->m_crefReq = 1;
            pm->m_pfnthunk = [](BASIC* pb, int c, CRef* a) {
                if (auto* p = dynamic_cast<JOBSCRIPT*>(pb); p && c > 0)
                    p->SeekMarker(a[0].RefCoerceS32().m_n);
                return CRef(TAGK_Void);
            };
            ref.SetMethod(pm);
        }
        else if (bifk == 111 && optid == 1171)  // EOPID1357, class9B field +10: the job's actor
            ref.SetBasic(pjob->pguard ? pjob->pguard->palo : pjob->pactor ? pjob->pactor->palo : nullptr);
        else if (bifk == 111 && optid == 1172)  // EOPID1358, JOBSCRIPT field +30: its command list
        {
            if (auto* pscript = dynamic_cast<JOBSCRIPT*>(pjob)) ref.SetBasic(pscript->pcmdlist);
        }
        return ref;
    }
    if (SMA* psma = dynamic_cast<SMA*>(pbasic)) return RefSmaOption(bifk, psma, optid, pref);
    if (PADCTL* ppc = dynamic_cast<PADCTL*>(pbasic))
    {
        // the controller state (class 0x89): 900..907 its sticks (EOPIDs 987..994, fields +0x10..+0x2C), 908 / 909
        // methods (0x10C720 / 0x10C7A0): the buttons of the mask pressed this frame and taken (0x1C4700) / down
        // (0x1C4728: +0x30 & mask)
        CRef r;
        if (std::getenv("SLY2N_OPTPRINT")) std::printf("padctl option %d (bif %d) of %d: control %d\n", optid, bifk, ppc->plo ? ppc->plo->oid : -1, ppc->nControl);
        if (bifk != 111) return r;
        const float* ag[] = {&ppc->x, &ppc->y, &ppc->radJoy, &ppc->uJoy, &ppc->x2, &ppc->y2, &ppc->radJoy2, &ppc->uJoy2};
        if (optid >= 900 && optid <= 907) r.SetF32(*ag[optid - 900]);
        if (optid >= 900 && optid <= 907 && std::getenv("SLY2N_OPTPRINT")) std::printf("  -> %g\n", *ag[optid - 900]);
        else if (optid == 908 || optid == 909)
        {
            auto pm = std::make_shared<CMethod>();
            pm->m_pbasic = pbasic;
            pm->m_crefReq = 1;
            pm->m_pfnthunk = optid == 908 ? [](BASIC* pb, int c, CRef* a) {
                CRef rr;
                rr.SetBool(c > 0 && GrfPadctlTakePressed(static_cast<PADCTL*>(pb), uint32_t(a[0].RefCoerceS32().m_n), 0.0f) != 0);
                return rr;
            } : [](BASIC* pb, int c, CRef* a) {
                CRef rr;
                rr.SetBool(c > 0 && (static_cast<PADCTL*>(pb)->grfbtn & uint32_t(a[0].RefCoerceS32().m_n)) != 0);
                if (std::getenv("SLY2N_OPTIONPRINT"))
                    std::fprintf(stderr, "padctl 909: c %d mask 0x%X down 0x%X\n", c, c > 0 ? a[0].RefCoerceS32().m_n : 0, static_cast<PADCTL*>(pb)->grfbtn);
                return rr;
            };
            r.SetMethod(pm);
        }
        return r;
    }
    if (ASEGA* pasega = dynamic_cast<ASEGA*>(pbasic))
    {
        // the ASEGA's properties (its vtable 0x2CAF48's class): 251 its object (else the ASEG)
        CRef r;
        // 250 (EOPID 116, type 0x206D, field +0x14): its ASEG. Ep4's lightning-puzzle cranks find their crank 88360
        // relative to it in the turn clip's events (aseg 9216 / 88367 s4): nil found another puzzle's crank
        if (bifk == 111 && optid == 250)
        {
            if (pasega->paseg) r.SetBasic(pasega->paseg);
        }
        else if (bifk == 111 && optid == 0xFB)
        {
            if (pasega->palo) r.SetBasic(pasega->palo);
            else if (pasega->paseg) r.SetBasic(pasega->paseg);
        }
        // orig: 0x001FFD98 / 0x00200598, ASEGA option 253 (EOPID119): local speed at +0x44;
        // its float setter is 0x00131A00 SetAsegaSpeed. RefCoerceF32 matches 0x00129DA8.
        else if (optid == 253 && (bifk == 111 || (bifk == 110 && pref)))
        {
            if (bifk == 111) r.SetF32(pasega->svtLocal);
            else SetAsegaSpeed(pasega, pref->RefCoerceF32().m_g);
        }
        // orig: 0x001FFD98, EOPID126 -> 0x00131968 FAsegaRetracted.
        else if (bifk == 111 && optid == 255) r.SetBool(FAsegaRetracted(pasega));
        // EOPID 120 (option 217): the ASEG's speed +0x48, its setter 0x131A20 SetAsegaAsegSpeed (Ep6's paraglide trains)
        else if (optid == 217 && (bifk == 111 || (bifk == 110 && pref)))
        {
            if (bifk == 111) r.SetF32(pasega->svtAseg);
            else SetAsegaAsegSpeed(pasega, pref->RefCoerceF32().m_g);
        }
        // EOPID 118 (option 252): the time +0x1C (read only)
        else if (bifk == 111 && optid == 252) r.SetF32(pasega->t);
        else if (bifk == 111 && optid == 236)
        {
            // 236 (class 0x86's EOPID 123, thunk 0x101C68): a method (k, g, svt) -> 0x130D30 SetAsegaTime (k 0 absolute,
            // 1 relative, 2 from the end; and the speed), e.g. Ep6's red train job seeking the train's loop
            auto pm = std::make_shared<CMethod>();
            pm->m_pbasic = pbasic;
            pm->m_pfnthunk = [](BASIC* pb, int c, CRef* a) {
                if (ASEGA* pa = dynamic_cast<ASEGA*>(pb); pa && c >= 3)
                    SetAsegaTime(pa, a[0].RefCoerceS32().m_n, a[1].RefCoerceF32().m_g, a[2].RefCoerceF32().m_g);
                return CRef(TAGK_Void);
            };
            pm->m_crefReq = 3;
            r.SetMethod(pm);
        }
        else if (bifk == 111 && optid == 248)
        {
            // 248 (class 0x86's EOPID 125, thunk 0x101D98): a method (f) -> 0x230CF0 SnapAsegaXfs: its transforms put
            // at the clip's values now (f: the seeking modes too)
            auto pm = std::make_shared<CMethod>();
            pm->m_pbasic = pbasic;
            pm->m_pfnthunk = [](BASIC* pb, int c, CRef* a) {
                if (ASEGA* pa = dynamic_cast<ASEGA*>(pb); pa && c >= 1)
                    SnapAsegaXfs(pa, a[0].m_tagk == TAGK_Bool ? a[0].m_bool != 0 : a[0].RefCoerceS32().m_n != 0);
                return CRef(TAGK_Void);
            };
            pm->m_crefReq = 1;
            r.SetMethod(pm);
        }
        else if (bifk == 111 && optid == 237)
        {
            // 237 (class 0x86's EOPID 124, 0x101D20): a method retracting it (RetractAsega 0x130B18: its end list's
            // events run, so the intro's skip turns its camera object off)
            auto pm = std::make_shared<CMethod>();
            pm->m_pbasic = pbasic;
            pm->m_pfnthunk = [](BASIC* pb, int, CRef*) {
                if (ASEGA* pa = dynamic_cast<ASEGA*>(pb)) RetractAsega(pa);
                return CRef(TAGK_Void);
            };
            pm->m_crefReq = 0;
            r.SetMethod(pm);
        }
        else if (std::getenv("SLY2N_OPTIONPRINT")) std::fprintf(stderr, "option bif %d id %d of an ASEGA: not ported\n", bifk, optid);
        return r;
    }
    LO* plo = PloFromBasic(pbasic);
    CRef refNil;
    static const bool s_fPrint = std::getenv("SLY2N_OPTIONPRINT") != nullptr;
    if (s_fPrint) std::fprintf(stderr, "option bif %d id %d object %d value tag %d n %d\n", bifk, optid, plo ? plo->oid : -1, pref ? int(pref->m_tagk) : -1, pref ? pref->m_n : 0);
    if (!plo) return refNil;
    // the object's sidebag (orig +0x4, 0x132B78 makes it): addo (0x200CA8) and ensureo (0x200D10) always bind
    // there; seto! (0x200C28) and geto (0x200BA0) above 0x4D0
    auto sidebag = [&]() -> CSidebag& {
        if (!plo->psidebag) plo->psidebag = PsidebagNew();
        return *plo->psidebag;
    };
    if (bifk == 108 && pref)
    {
        CRef ref = *pref;
        return sidebag().RefAddBinding(optid, &ref);
    }
    if (bifk == 109 && pref)
    {
        CRef ref = *pref;
        if (sidebag().FFindBinding(optid, nullptr)) return sidebag().RefSetBinding(optid, &ref);
        return sidebag().RefAddBinding(optid, &ref);
    }
    if (optid > 0x4D0)
    {
        if (bifk == 111)
        {
            CRef ref;
            if (plo->psidebag) plo->psidebag->FFindBinding(optid, &ref);
            if (s_fPrint && ref.m_tagk == TAGK_Proc && ref.m_pproc)
            {
                std::string str;
                CRef body;
                body.SetPair(ref.m_pproc->m_ppairCodeExpr);
                PrintRef(body, &str);
                std::fprintf(stderr, "  option %d of %d: %s\n", optid, plo->oid, str.substr(0, 4000).c_str());
            }
            return ref;
        }
        if (bifk == 110 && pref)
        {
            CRef ref = *pref;
            return sidebag().RefSetBinding(optid, &ref);
        }
        return refNil;
    }
    const int eopid = EopidFromOptid(plo->Cid(), optid);
    if (std::getenv("SLY2N_OPTPRINT")) std::printf("option %d (bif %d) of %d (cid 0x%X): eopid %d\n", optid, bifk, plo->oid, plo->Cid(), eopid);
    if (eopid < 0) return refNil;
    const uint32_t vaEopid = 0x2AC880 + static_cast<uint32_t>(eopid) * 0x28;
    const uint32_t grf = g_disc.U32Elf(vaEopid + 4);
    if (bifk == 111 && (grf & 1))
    {
        const uint32_t pfn = g_disc.U32Elf(vaEopid + 0x10);
        if (std::getenv("SLY2N_METHODPRINT")) std::printf("method %d of %d (cid 0x%X): pfn 0x%X\n", optid, plo->oid, plo->Cid(), pfn);
        CRef refMethod;
        if (FMakeObjectMethod(pfn, pbasic, static_cast<int>(g_disc.U32Elf(vaEopid + 0x18)), &refMethod)) return refMethod;
        auto pmethod = std::make_shared<CMethod>();
        pmethod->m_pbasic = pbasic;
        pmethod->m_pfnthunk = pfn == 0x100A90   ? RefMethodAdd
                              : pfn == 0x100B88 ? RefMethodRemove
                              : pfn == 0x100A00 ? RefMethodSlot38
                                                : RefMethodStub;
        pmethod->m_crefReq = static_cast<int>(g_disc.U32Elf(vaEopid + 0x18));
        if (pmethod->m_pfnthunk == RefMethodStub) NoteUnbound("method", eopid, optid, plo, pfn);
        CRef ref;
        ref.SetMethod(pmethod);
        return ref;
    }
    if (bifk == 111)
        if (PFNOBJGETTER pfnGet = PfnObjectGetter(eopid))
        {
            CRef r = pfnGet(plo);
            if (const char* sz = std::getenv("SLY2N_OPTPRINT"); sz && sz[0] == '2')  // (debug: the getter's value)
            {
                std::string str;
                PrintRef(r, &str);
                std::printf("  = %s\n", str.c_str());
            }
            return r;
        }
    OPT* popt = nullptr;
    for (OPT& opt : plo->aopt)
        if (opt.eopid == eopid) popt = &opt;
    if (bifk == 111)
    {
        CRef ref;
        if (eopid == 15) ref.SetS32(plo->oid);  // (field +0x18: the oid)
        else if (eopid == 13) ref.SetF32(plo->sRadius);
        else if (eopid == 14) ref.SetS32(plo->st);
        else if (eopid == 255 && PguardFromLo(plo)) ref.SetS32(PguardFromLo(plo)->n33C);  // (field +0x33C: a guard's health)
        else if (popt && !popt->aval.empty() && (grf & 0x80) && popt->aval[0].tagk == -1 && popt->aval[0].k >= 24 &&
                 ((popt->aval[0].k & ~0x8000) >> 12) == 2)
        {
            // a field holding an object (kind class 2: the load reads its oid and resolves it, 0x13E818): the
            // getter reads the object pointer, e.g. a recon subject's (cid 0x2E) task +0xEC (option 827, EOPID
            // 1201) that 11:1990's ballroom director asks for option 1080 (natively the oid was returned)
            const int oid = popt->aval[0].n;
            ref.SetBasic(oid == -1 ? nullptr : PloFindSwNearest(plo, oid));
        }
        else if (popt && !popt->aval.empty()) ref = RefFromOptval(popt->aval[0]);
        else if (!popt && (grf & 0x80) && ((g_disc.U32Elf(vaEopid) & ~0x8000u) >> 12) == 2)
        {
            // an object field the code set at run time (no loaded option), e.g. a use's user +0x10C (EOPID 1068,
            // 0x184130 sets it): the stations of Ep4's hack job ask for it (block 28030's lambda) before starting
            // their script
            const uint32_t ib = g_disc.U32Elf(vaEopid + 0x18);
            const OID oid = static_cast<OID>(plo->U32Field(0, ib, 0xFFFFFFFFu));
            if (oid != -1) ref.SetBasic(PloFindSwNearest(plo, oid));
        }
        else if (!popt && (grf & 0x80) && eopid != 15 && eopid != 13 && eopid != 14 && eopid != 255)
            // (debug, audit 2026-11-b: a field getter of a field no loaded option set, which the native code may keep
            // elsewhere (as N21's clip length, EOPID 114): its value is the field's stored word, or nothing)
            NoteUnbound("field", eopid, optid, plo, g_disc.U32Elf(vaEopid + 0x18));
        if (g_disc.U32Elf(vaEopid + 8) == 0xFFFF0000u) NoteUnbound("getter", eopid, optid, plo, g_disc.U32Elf(vaEopid + 0xC));
        if (std::getenv("SLY2N_OPTPRINT"))
        {
            std::string str;
            PrintRef(ref, &str);
            std::printf("  -> %s (no getter: getter 0x%X)\n", str.c_str(), g_disc.U32Elf(vaEopid + 0xC));
        }
        return ref;
    }
    if (!pref) return refNil;
    if (bifk == 109 && popt) return CRef(TAGK_Void);  // ensure: only when absent
    if (!popt || bifk == 108)
    {
        plo->aopt.push_back(OPT{});
        popt = &plo->aopt.back();
        popt->eopid = eopid;
    }
    popt->aval.assign(1, OptvalFromRef(*pref));
    // a field EOPID (grf 0x90): the value lands in the object's field at +0x18's offset, as the loader stores it
    // (brx.cpp; e.g. Bentley's dart kind 774 at +0x16D8, which his 0x1357C0 reads)
    if (bifk == 110 && (grf & 0x90) && !(grf & 0x2000))
    {
        OPTVAL& v = popt->aval[0];
        const int k = static_cast<int>(g_disc.U32Elf(vaEopid));
        const uint32_t ib = g_disc.U32Elf(vaEopid + 0x18);
        // the value converted to the field's type (0x201110: a float field takes RefCoerceF32 0x129DA8 of a
        // non-float, an int field RefCoerceS32 0x129D58 (a truncation) of a non-int), e.g. Elephant Rampage's crawl
        // trigger setting the rats' radii 886 / 887 to 1500 / 1000: integers into float fields, whose float word
        // natively stayed 0, so the rats never saw Sly and never ran into their holes (R60)
        if (k == 1 && v.tagk == 0x15) { v.ag[0] = static_cast<float>(v.n); v.tagk = 0x16; }
        else if (((k >= 5 && k <= 8) || (k >= 0xE && k <= 0x11) || (k >= 0x13 && k <= 0x15)) && v.tagk == 0x16)
        {
            v.n = static_cast<int>(v.ag[0]);
            v.tagk = 0x15;
        }
        const uint32_t cg = k == 1 ? 1 : k == 10 ? 2 : (k == 2 || k == 9 || k == 11) ? 3 : 0;
        if (cg)
            for (uint32_t i = 0; i < cg; ++i)
            {
                uint32_t u;
                std::memcpy(&u, &v.ag[i], 4);
                plo->SetField(0, ib + 4 * i, u);
            }
        else plo->SetField(0, ib, static_cast<uint32_t>(v.n));
        // (a guard's health +0x33C is GUARD::n33C natively)
        if (eopid == 255)
            if (GUARD* pg = PguardFromLo(plo)) pg->n33C = v.n;
    }
    if (bifk == 110 && std::getenv("SLY2N_OPTPRINT"))
    {
        std::string str;
        PrintRef(*pref, &str);
        std::printf("  <- %s%s (setter 0x%X)\n", str.c_str(), PfnObjectSetter(eopid) ? "" : " (no setter)", g_disc.U32Elf(vaEopid + 0x14));
    }
    if (bifk == 110)
    {
        if (PFNOBJSETTER pfnSet = PfnObjectSetter(eopid)) pfnSet(plo, *pref);
        else if (g_disc.U32Elf(vaEopid + 0x18) == 0xFFFF0000u)
            NoteUnbound("setter", eopid, optid, plo, g_disc.U32Elf(vaEopid + 0x1C));
    }
    if (eopid == 13 && pref->m_tagk == TAGK_F32) SetLoRadius(s_psw, plo, pref->m_g);
    else if (eopid == 13 && pref->m_tagk == TAGK_S32) SetLoRadius(s_psw, plo, static_cast<float>(pref->m_n));
    else if (eopid == 14 && pref->m_tagk == TAGK_S32) SetLoSt(s_psw, plo, pref->m_n);
    return CRef(TAGK_Void);
}

// 100..102 (0x3E0F9C / 0x3E0FA0 / 0x3E0FA4), 117 the player, 118 the world, 119 the camera
BASIC* PbasicGlobal(int bifk)
{
    if (!s_psw) return nullptr;
    // 100..102: the current job, task and checkpoint task (SW +0x3B0C / +0x3B10 / +0x3B14)
    if (bifk == 100) return s_psw->tasks.pjobCur;
    if (bifk == 101) return s_psw->tasks.ptaskCur;
    if (bifk == 102) return s_psw->tasks.ptaskChk;
    if (bifk == 117) return &s_swbasic;
    if (bifk == 137) return &s_promptbasic;
    if (bifk == 119) return &s_cmbasic;
    if (bifk == 118) return &s_sndbasic;  // orig 0x124690: g_sound 0x3DD040  // orig 0x124640: the world D_003DD490 (its 844 is the player)
    return nullptr;  // TODO(M3): the globals once the game sets them; the world and the camera are not LOs yet
}
}

void InstallSpliceHooks(SW* psw)
{
    s_psw = psw;
    s_mpWorldOption.clear();
    g_splicegame.pfnFindObject = PbasicFindObject;
    g_splicegame.pfnFindObjects = CFindObjects;
    g_splicegame.pfnFindObjectsByClass = CFindObjectsByClass;
    g_splicegame.pfnRefOption = RefOption;
    g_splicegame.pfnPbasicGlobal = PbasicGlobal;
}

// orig: 0x001002A8 / 0x00100350 / 0x001003F8 / 0x00100610 (the methods EOPIDs 4..10 the loader calls, grf 0x401):
// the object's sidebag option (the first argument's id) ensured to the second as a bool (0x132BC8), an int
// (0x132C20), a float (0x132C78) or an oid (0x132D88); 0x1004A0 / 0x100558 / 0x1006B8 / 0x100760 (0x132CC8 /
// 0x132D28 / 0x132DE0 / 0x132E40) keep the value's own kind
namespace
{
CRef RefEnsureSidebag(LO* plo, const std::vector<CRef>& aref, int tagk)
{
    if (!plo || aref.size() < 2) return CRef(TAGK_Void);
    if (static const char* s_sz = std::getenv("SLY2N_BAGPRINT"); s_sz && plo->oid == std::atoi(s_sz))
        std::printf("sidebag %d: option %d = %d (tag %d)\n", plo->oid, NArg(aref, 0), NArg(aref, 1), tagk);
    CRef v = aref[1];
    if (tagk == TAGK_Bool) v.SetBool(NArg(aref, 1) != 0);
    else if (tagk == TAGK_F32) v.SetF32(GArg(aref, 1));
    else if (tagk == TAGK_S32) v.SetS32(NArg(aref, 1));
    RefOption(109, plo, NArg(aref, 0), &v);
    return CRef(TAGK_Void);
}
struct INITENSURE
{
    INITENSURE()
    {
        RegisterObjectMethod(0x101588, RefSoeHandle);
        // option 90 of an ALO (EOPID 38): its attachment's contacts off (getter 0x1F6838: +0xEC's +0x70 bit 2, 1 without
        // an attachment; setter 0x1F6860), e.g. the Ep1 heist harpoon on the sign (block 123952's s2247)
        RegisterObjectGetter(38, [](LO* plo) {
            CRef r;
            ALO* palo = dynamic_cast<ALO*>(plo);
            const OXA* poxa = palo ? Poxa(palo) : nullptr;
            r.SetBool(!poxa || (poxa->grf & 4) ? 1 : 0);
            return r;
        });
        // 0x101218 / 0x1012C0 (EOPIDs 64 / 65 -> 0x1F7FA0 / 0x1F8038): over the object's shader animations (its
        // globset +0xA4 / +0xA8) of class 0x8F (the scrollers): the master speeds (0x1EE678) / the offsets (0x1EDAD8).
        // Scripts of 86 job starts set them (e.g. class 0x62 breakables); natively stubs (audit 2026-11 R1)
        for (uint32_t pfn : {0x101218u, 0x1012C0u})
            RegisterObjectMethod(pfn, pfn == 0x101218u ? +[](LO* plo, const std::vector<CRef>& a) {
                ALO* palo = dynamic_cast<ALO*>(plo);
                if (palo && palo->pglobset)
                    for (SAA* p : palo->pglobset->apsaa) SetScrollerMasterSpeeds(p, GArg(a, 0), GArg(a, 1));
                return CRef(TAGK_Void);
            } : +[](LO* plo, const std::vector<CRef>& a) {
                ALO* palo = dynamic_cast<ALO*>(plo);
                if (palo && palo->pglobset)
                    for (SAA* p : palo->pglobset->apsaa)
                        if (p && p->cid == 143) SetSaaOffsets(p, GArg(a, 0), GArg(a, 1));
                return CRef(TAGK_Void);
            });
        // 0x110998 (EOPID 1438): vt slot 90 with an integer; class 0x3D's is 0x1CBF90, the pair camera (cmworld.cpp)
        RegisterObjectMethod(0x110998, [](LO* plo, const std::vector<CRef>& a) {
            if (plo && plo->pcls && plo->pcls->PfnOrig(90) == 0x1CBF90) SetPairCamera(plo, NArg(a, 0));
            return CRef(TAGK_Void);
        });
        // EOPID 214 (a zone's +0x13C, setter 0x221268): a script's force of a camera zone (-1 by position, 0 off,
        // 1 on); natively the value only reached a raw field nobody reads (audit 2026-11 C1)
        RegisterObjectSetter(214, [](LO* plo, const CRef& r) {
            SetZoneForced(plo, r.m_tagk == TAGK_Bool ? (r.m_bool ? 1 : 0) : r.RefCoerceS32().m_n);
        });
        RegisterObjectGetter(214, [](LO* plo) {
            CRef r;
            r.SetS32(NZoneForced(plo));
            return r;
        });
        RegisterObjectSetter(38, [](LO* plo, const CRef& r) {
            if (ALO* palo = dynamic_cast<ALO*>(plo)) SetAloContactsOff(palo, r.m_tagk == TAGK_Bool ? r.m_bool != 0 : r.RefCoerceS32().m_n != 0);
        });
        // 0x101468 (option 183: the object's SMA of the SM with this oid, -1 any; 0x1F67E0 walks the SMA list
        // 0x3E126C), e.g. the safe house's ((sly 183) 419) snapped to 5524
        RegisterObjectMethod(0x101468, [](LO* plo, const std::vector<CRef>& a) {
            const int oid = NArg(a, 0, -1);
            CRef r;
            r.SetBasic(nullptr);  // (0x129A10: an object reference, null when none: (null-obj? ..) is true)
            for (SMA* ps : g_apsma)
                if (ps->palo == plo && !FSmaRetracted(ps) && (oid == -1 || (ps->psm && ps->psm->oid == oid)))
                {
                    r.SetBasic(ps);
                    if (std::getenv("SLY2N_SMAFIND"))
                        std::printf("option 183 of %d (sm %d): found, current %d goal %d t %.3f\n", plo->oid, oid, OidSmaCur(ps), OidSmaGoal(ps), g_clock.t);
                    break;
                }
            return r;
        });
        // 0x1013E8 (option 182 of an ALO: its ASEGA of the ASEG with this oid; 0x1F6788 walks the ASEGA list
        // 0x3E1218), e.g. the boot world's skip of the intro ((s4059 123962 123963): ((alo 182) aseg) then 237)
        // orig: 0x00101910 (option 214 of an ASEG: applied to an ALO (object, t, svt, grfapl: 0x130488), its ASEGA
        // returned), e.g. Paris' newspaper dispensers 138376 playing 22922 when a newspaper is gone
        RegisterObjectMethod(0x101910, [](LO* plo, const std::vector<CRef>& a) {
            CRef r;
            // (a nil object: the ASEG's own namespace's objects, as its apply params' 0x12F8C0; e.g. world 30's
            // train-side scenery 48945.. (s0 of block 47475))
            LO* palo = PloArg(a, 0);
            if (!plo || !FIsLoDerivedFrom(plo, 0x6D) || (palo && !FIsLoDerivedFrom(palo, 0x30))) return r;
            if (static const char* s_sz = std::getenv("SLY2N_APPLYOBJ"); s_sz && palo && palo->oid == std::atoi(s_sz))
            {
                std::fprintf(stderr, "aseg method 214: aseg %d obj %d args %zu:", plo->oid, palo->oid, a.size());
                for (const CRef& x : a)
                {
                    std::string str;
                    PrintRef(x, &str);
                    std::fprintf(stderr, " %s", str.c_str());
                }
                std::fprintf(stderr, "\n");
            }
            if (ASEGA* pa = PasegaApplyAseg(static_cast<ASEG*>(plo), static_cast<ALO*>(palo), GArg(a, 1), GArg(a, 2), NArg(a, 3))) r.SetBasic(pa);
            return r;
        });
        // orig: 0x00101A88 (EOPID 108, option 216 of an ASEG: ensured on an ALO (object, the time's kind, t, svt,
        // grfapl: 0x130598 EnsureAseg: the existing ASEGA retimed, else applied), its ASEGA returned). Scripts of 32 job
        // starts call it; natively a stub (audit 2026-11 P6)
        RegisterObjectMethod(0x101A88, [](LO* plo, const std::vector<CRef>& a) {
            CRef r;
            LO* palo = PloArg(a, 0);
            if (!plo || !FIsLoDerivedFrom(plo, 0x6D) || (palo && !FIsLoDerivedFrom(palo, 0x30))) return r;
            if (ASEGA* pa = PasegaEnsureAseg(static_cast<ASEG*>(plo), static_cast<ALO*>(palo), NArg(a, 1), GArg(a, 2), GArg(a, 3), NArg(a, 4)))
                r.SetBasic(pa);
            return r;
        });
        // orig: 0x00104840 (option 214 of an SM: run on an ALO from a state (object, state oid, grfapl: 0x1F0380), its
        // SMA returned), e.g. the boot world's 150730 / 118385 starting 150732 / 118427 at 5519
        RegisterObjectMethod(0x104840, [](LO* plo, const std::vector<CRef>& a) {
            CRef r;
            LO* palo = PloArg(a, 0);
            // (a nil object: an SMA of no object, e.g. world 14's job 2406 event 19 running SM 17348 (the hack's stations))
            if (!plo || plo->Cid() != 0x6F || (palo && !FIsLoDerivedFrom(palo, 0x30))) return r;
            if (SMA* ps = PsmaApplySm(static_cast<SM*>(plo), static_cast<ALO*>(palo), NArg(a, 1, -1), NArg(a, 2))) r.SetBasic(ps);
            return r;
        });
        RegisterObjectMethod(0x1013E8, [](LO* plo, const std::vector<CRef>& a) {
            const int oid = NArg(a, 0, -1);
            CRef r;
            r.SetBasic(nullptr);  // (0x129A10: null when none)
            for (ASEGA* pa : g_apasega)
                if (!FAsegaRetracted(pa) && pa->palo == plo && pa->paseg && (oid == -1 || pa->paseg->oid == oid)) { r.SetBasic(pa); break; }
            return r;
        });
        RegisterObjectMethod(0x1002A8, [](LO* plo, const std::vector<CRef>& a) { return RefEnsureSidebag(plo, a, TAGK_Bool); });
        RegisterObjectMethod(0x100350, [](LO* plo, const std::vector<CRef>& a) { return RefEnsureSidebag(plo, a, TAGK_S32); });
        RegisterObjectMethod(0x1003F8, [](LO* plo, const std::vector<CRef>& a) { return RefEnsureSidebag(plo, a, TAGK_F32); });
        RegisterObjectMethod(0x100610, [](LO* plo, const std::vector<CRef>& a) { return RefEnsureSidebag(plo, a, TAGK_S32); });
        for (uint32_t pfn : {0x1004A0u, 0x100558u, 0x1006B8u, 0x100760u})
            RegisterObjectMethod(pfn, [](LO* plo, const std::vector<CRef>& a) { return RefEnsureSidebag(plo, a, -1); });
        // EOPID 19 (id 32): the field +0x10, the object's parent (a null object at a root)
        RegisterObjectGetter(19, [](LO* plo) {
            CRef r;
            if (std::getenv("SLY2N_PARENTDBG"))
                std::printf("parentdbg: %d (cid 0x%X %p) parent %p %d\n", plo->oid, plo->Cid(), (void*)plo, (void*)plo->paloParent, plo->paloParent ? plo->paloParent->oid : -1);
            r.SetBasic(plo->paloParent);
            return r;
        });
        // EOPID 16 (id 29), orig: 0x001A6260: the last oid of the object's instance chain +0x3C (the entries the
        // object owns), else -1: natively LO::oidInstance. 11:1990's ballroom lists 1982 / 1983 pick each person's
        // dance by it (case 32248 / 12710 / 105826..); unregistered, nobody matched and all skipped their dance.
        RegisterObjectGetter(16, [](LO* plo) {
            CRef r;
            r.SetS32(plo->oidInstance);
            return r;
        });
        // EOPID 27 (id 41), orig: 0x00259AE8: LO +0x28 bit 60,
        // whether the object is in the world (including inactive roots).
        RegisterObjectGetter(27, [](LO* plo) {
            CRef r;
            r.SetBool(plo->fInWorld);
            return r;
        });
    }
} s_initensure;
}

// orig: 0x0015E938, shared by loaded-reference consumers.
LO* PloFindRelative(LO* context, int oid, int kind)
{
    return dynamic_cast<LO*>(PbasicFindObject(kind, oid, context, nullptr));
}
