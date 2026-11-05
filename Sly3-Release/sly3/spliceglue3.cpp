// Sly 3's splice <-> game glue (spliceglue3.h).

#include "spliceglue3.h"

#include "addr.h"
#include "ids3.h"

#include "../basic.h"
#include "../cls.h"
#include "../disc.h"
#include "../frame.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../sidebag.h"
#include "../spliceobj.h"
#include "../sw.h"
#include "../update.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)

namespace sly3
{
namespace
{
SW* s_psw = nullptr;
std::map<int, PFNSPLICEGETTER3>& MpGetter()
{
    static std::map<int, PFNSPLICEGETTER3> s_map;
    return s_map;
}
// (objmethod.cpp's registry is keyed by thunk address and holds Sly 2's bodies: Sly 3's keys carry the top bit)
constexpr uint32_t KEY_SLY3 = 0x80000000u;
constexpr uint32_t VA_EOPID_BY_CID = 0x2ED518;  // orig3: EopidFromOptid's table (cid * 8: count, {optid, EOPID*})
constexpr int OPTID_SIDEBAG = 0x718;             // orig3: 0x24AB90 / 0x24AC18 / 0x24AC98 / 0x24AD00: ids from here up

struct WORLDBASIC3 : BASIC {};  // the world object of find-world-object (SW; its options are not ported yet)
WORLDBASIC3 s_swbasic;

bool FTrace()
{
    static const bool s_f = std::getenv("SLY3_SPLICEPRINT") != nullptr;  // (debug: options, methods, messages)
    return s_f;
}

void WarnOnce(const char* szWhat, int a, int b)
{
    static std::map<std::pair<int, int>, bool> s_mp;
    if (s_mp[{a, b}]) return;
    s_mp[{a, b}] = true;
    std::fprintf(stderr, "splice3: %s %d (%d) not ported\n", szWhat, a, b);
}

// orig3: 0x15E958's kinds (5 global, 1 relative, 4): the objects are named by their oids
BASIC* PbasicFindObject(int kind, SYMID symidName, BASIC* pbasicRel, BASIC* pbasicScope)
{
    (void)kind; (void)pbasicRel; (void)pbasicScope;
    // TODO(sly3): PloFindSwNearest 0x17E1B8's nearest-in-namespace choice for kind 1 (instances of shared groups)
    return s_psw ? PloFindSwObjectByOid(s_psw, static_cast<OID>(symidName)) : nullptr;
}

int CFindObjects(int kind, SYMID symidName, BASIC* pbasicRel, int cMax, BASIC** apbasic)
{
    (void)kind; (void)pbasicRel;
    if (!s_psw || cMax <= 0) return 0;
    int c = 0;
    for (const auto& plo : s_psw->aploAll)
        if (plo->oid == static_cast<OID>(symidName) && c < cMax) apbasic[c++] = plo.get();
    return c;
}

int CFindObjectsByClass(int kind, int cid, BASIC* pbasicRel, int cMax, BASIC** apbasic)
{
    (void)kind; (void)pbasicRel;
    if (!s_psw || cid < 0 || cid >= CID_Max) return 0;
    int c = 0;
    for (int ccur = 0; ccur < CID_Max && c < cMax; ++ccur)
        if (FCidDerivedFrom(ccur, cid))
            for (LO* plo : s_psw->mpcidaplo[std::size_t(ccur)])
                if (c < cMax) apbasic[c++] = plo;
    return c;
}

// the built-ins without arguments (the hooks' role codes: 117 the player, 118 the world, 119 the camera)
BASIC* PbasicGlobal(int bifk)
{
    if (!s_psw) return nullptr;
    if (bifk == 118) return &s_swbasic;
    if (bifk == 117)  // TODO(sly3): the player global (0x370AC0); Sly (cid 0x34) meanwhile
    {
        const auto& a = s_psw->mpcidaplo[std::size_t(Icid(CID3::JT))];
        return a.empty() ? nullptr : a[0];
    }
    WarnOnce("global object of role", bifk, 0);
    return nullptr;
}

CRef RefOption3(int bifk, BASIC* pbasic, int optid, const CRef* pref)
{
    if (pbasic && !FBasicLive(pbasic)) return CRef(TAGK_Void);
    if (pbasic == &s_swbasic)
    {
        WarnOnce("world option", optid, bifk);
        return CRef(TAGK_Void);
    }
    LO* plo = dynamic_cast<LO*>(pbasic);
    if (!plo) return CRef(TAGK_Void);
    auto sidebag = [&]() -> CSidebag& {
        if (!plo->psidebag) plo->psidebag = PsidebagNew();
        return *plo->psidebag;
    };
    // addo / ensureo always bind in the sidebag (0x24AC98 / 0x24AD00 above the class ids too)
    if (bifk == 108 && pref)
    {
        CRef ref = *pref;
        return sidebag().RefAddBinding(optid, &ref);
    }
    if (bifk == 109 && pref)
    {
        CRef ref = *pref;
        if (sidebag().FFindBinding(optid, nullptr)) return CRef(TAGK_Void);
        return sidebag().RefAddBinding(optid, &ref);
    }
    if (optid >= OPTID_SIDEBAG)
    {
        if (bifk == 111)
        {
            CRef ref;
            if (plo->psidebag) plo->psidebag->FFindBinding(optid, &ref);
            return ref;
        }
        if (bifk == 110 && pref)
        {
            CRef ref = *pref;
            return sidebag().RefSetBinding(optid, &ref);
        }
        return CRef(TAGK_Void);
    }
    const int eopid = EopidFromOptid3(plo->Cid(), optid);
    if (eopid < 0)
    {
        WarnOnce("option of a class without it: optid", optid, plo->Cid());
        return CRef(TAGK_Void);
    }
    const uint32_t va = orig3::VA_AEOPID + uint32_t(eopid) * orig3::CB_EOPID;
    const uint32_t grf = g_disc.U32Elf(va + 4);
    if (bifk == 111 && (grf & 1))
    {
        // orig3: 0x249D98's method: a function of the object calling the thunk +0x10 with +0x18 arguments
        const uint32_t pfn = g_disc.U32Elf(va + 0x10);
        CRef ref;
        if (FTrace()) std::printf("splice3: method optid %d of %d (cid 0x%X): EOPID %d thunk 0x%X\n", optid, plo->oid, plo->Cid(), eopid, pfn);
        if (FMakeObjectMethod(pfn | KEY_SLY3, plo, int(g_disc.U32Elf(va + 0x18)), &ref)) return ref;
        WarnOnce("method EOPID", eopid, int(pfn));
        return CRef(TAGK_Void);
    }
    if (bifk == 111)
    {
        CRef ref;
        auto it = MpGetter().find(eopid);
        if (it != MpGetter().end() && it->second(plo, &ref))
        {
            if (FTrace())
            {
                std::string str;
                PrintRef(ref, &str);
                std::printf("splice3: geto optid %d of %d: EOPID %d = %s\n", optid, plo->oid, eopid, str.c_str());
            }
            return ref;
        }
        // the value the loader read for that option (fields set only at the load)
        for (const OPT& o : plo->aopt)
            if (o.eopid == eopid && !o.aval.empty()) return RefFromOptionValue(o.aval[0]);
        WarnOnce("geto EOPID", eopid, plo->Cid());
        return CRef(TAGK_Void);
    }
    WarnOnce("seto! EOPID", eopid, plo->Cid());
    return CRef(TAGK_Void);
}
}  // namespace

void RegisterSpliceMethod3(uint32_t pfnThunk, PFNSPLICEMETHOD3 pfn) { RegisterObjectMethod(pfnThunk | KEY_SLY3, pfn); }
void RegisterSpliceGetter3(int eopid, PFNSPLICEGETTER3 pfn) { MpGetter()[eopid] = pfn; }

// orig3: 0x249CF8 (the class's sorted table; the EOPID's index in 0x32CA80)
int EopidFromOptid3(int cid, int optid)
{
    if (cid < 0 || cid >= CID_Max) return -1;
    const uint32_t c = g_disc.U32Elf(VA_EOPID_BY_CID + uint32_t(cid) * 8);
    const uint32_t a = g_disc.U32Elf(VA_EOPID_BY_CID + uint32_t(cid) * 8 + 4);
    int lo = 0, hi = int(c) - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) >> 1;
        const int k = int(g_disc.U32Elf(a + uint32_t(mid) * 8));
        if (k == optid) return int((g_disc.U32Elf(a + uint32_t(mid) * 8 + 4) - orig3::VA_AEOPID) / orig3::CB_EOPID);
        if (optid < k) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

void InstallSpliceHooks3(SW* psw)
{
    s_psw = psw;
    g_splicegame.pfnFindObject = psw ? PbasicFindObject : nullptr;
    g_splicegame.pfnFindObjects = psw ? CFindObjects : nullptr;
    g_splicegame.pfnFindObjectsByClass = psw ? CFindObjectsByClass : nullptr;
    g_splicegame.pfnRefOption = psw ? RefOption3 : nullptr;
    g_splicegame.pfnPbasicGlobal = psw ? PbasicGlobal : nullptr;
}

bool SendLoMessage3(LO* plo, int msg, int carg, CRef* aref, CRef* prefResult)
{
    if (!plo || !plo->pframe || !plo->pframe->PrefFindBinding(SYMID(msg), 1)) return false;
    if (FTrace()) std::printf("splice3: message %d to %d (cid 0x%X)\n", msg, plo->oid, plo->Cid());
    HandleLoSpliceEvent(plo, uint32_t(msg), false, carg, aref, prefResult);
    return true;
}
}  // namespace sly3
