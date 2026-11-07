#include "objmethod.h"

#include "game/sly3.h"

#include "lo.h"
#include "pair.h"
#include "sw.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <utility>

namespace
{
std::unordered_map<uint32_t, PFNOBJMETHOD>& MpMethod()
{
    static std::unordered_map<uint32_t, PFNOBJMETHOD> s_map;
    return s_map;
}

// A splice method's thunk has no context but the object: one thunk per registered body (by its index).
constexpr int CTHUNK = 256;
PFNOBJMETHOD s_apfnThunk[CTHUNK];
uint32_t s_apfnOrigThunk[CTHUNK];
int s_cthunk = 0;

template <int I> CRef RefThunk(BASIC* pbasic, int argc, CRef* argv)
{
    LO* plo = dynamic_cast<LO*>(pbasic);
    std::vector<CRef> aref(argv, argv + (argc > 0 ? argc : 0));
    // (the world's methods (option 861 ...) have no LO: their bodies get a null object)
    return pbasic && s_apfnThunk[I] ? s_apfnThunk[I](plo, aref) : CRef(TAGK_Void);
}
template <int... I> constexpr std::array<PFNTHUNK, sizeof...(I)> AThunks(std::integer_sequence<int, I...>)
{
    return {RefThunk<I>...};
}
const auto s_athunk = AThunks(std::make_integer_sequence<int, CTHUNK>{});

int IthunkFor(uint32_t pfnThunk, PFNOBJMETHOD pfn)
{
    for (int i = 0; i < s_cthunk; ++i)
        if (s_apfnOrigThunk[i] == pfnThunk) return i;
    if (s_cthunk >= CTHUNK) return -1;
    s_apfnOrigThunk[s_cthunk] = pfnThunk;
    s_apfnThunk[s_cthunk] = pfn;
    return s_cthunk++;
}
}

// Sly 3: the copied Sly 2 code registers its bodies by Sly 2 numbers (a thunk address, an EOPID);
// the scripts reach them by Sly 3's (the descriptors of the EOPID table 0x32CA80). The keys are translated here: an
// EOPID through game/gen/eopidmap.inc, a thunk through the descriptors it sits in (game/thunkmap3.inc: the Sly 2 and
// Sly 3 descriptors' +0x08 / +0x10 of each mapped EOPID), else game/gen/fnmap.inc. A Sly 2 key with no Sly 3
// counterpart is dropped (SLY3_REGPRINT=1 lists them). The *3 forms register Sly 3 keys as they are and win over
// translated ones.
namespace
{
struct THUNKROW { uint32_t va2, va3; int eopid2, eopid3, off, score; };
constexpr THUNKROW s_athunkrow[] = {
#define THUNKMAP(a, b, c, d, e, f) {a, b, c, d, e, f},
#include "game/thunkmap3.inc"
#undef THUNKMAP
};
bool FRegPrint()
{
    static const bool s_f = std::getenv("SLY3_REGPRINT") != nullptr;
    return s_f;
}
struct REGSTATS { int cmethod = 0, cmethodDropped = 0, ceopid = 0, ceopidDropped = 0; };
REGSTATS& Regstats()
{
    static REGSTATS s;
    return s;
}
std::unordered_map<uint32_t, PFNOBJMETHOD>& MpMethod3()
{
    static std::unordered_map<uint32_t, PFNOBJMETHOD> s_map;
    return s_map;
}
}  // namespace

void RegisterObjectMethod(uint32_t pfnThunk, PFNOBJMETHOD pfn)
{
    bool fAny = false;
    for (const THUNKROW& r : s_athunkrow)
        if (r.va2 == pfnThunk)
        {
            MpMethod().try_emplace(r.va3, pfn);
            fAny = true;
        }
    if (!fAny)
        if (const int64_t va3 = sly3::gen::LookupRt(sly3::gen::afn, pfnThunk); va3 > 0)
        {
            MpMethod().try_emplace(uint32_t(va3), pfn);
            fAny = true;
        }
    ++Regstats().cmethod;
    if (!fAny)
    {
        ++Regstats().cmethodDropped;
        if (FRegPrint()) std::fprintf(stderr, "sly3: method thunk 0x%X (Sly 2) has no Sly 3 thunk: not registered\n", pfnThunk);
    }
}
void RegisterObjectMethod3(uint32_t pfnThunk3, PFNOBJMETHOD pfn) { MpMethod3()[pfnThunk3] = pfn; }

namespace
{
std::unordered_map<int, PFNOBJGETTER>& MpGetter()
{
    static std::unordered_map<int, PFNOBJGETTER> s_map;
    return s_map;
}
std::unordered_map<int, PFNOBJGETTER>& MpGetter3()
{
    static std::unordered_map<int, PFNOBJGETTER> s_map;
    return s_map;
}
std::unordered_map<int, PFNOBJSETTER>& MpSetter()
{
    static std::unordered_map<int, PFNOBJSETTER> s_map;
    return s_map;
}
std::unordered_map<int, PFNOBJSETTER>& MpSetter3()
{
    static std::unordered_map<int, PFNOBJSETTER> s_map;
    return s_map;
}
// (Sly 3: by the mapped EOPID, above)
int Eopid3Registered(int eopid2, const char* szKind)
{
    const int e3 = sly3::Eopid3FromEopid2(eopid2);
    ++Regstats().ceopid;
    if (e3 < 0)
    {
        ++Regstats().ceopidDropped;
        if (FRegPrint()) std::fprintf(stderr, "sly3: %s of EOPID %d (Sly 2) has no Sly 3 EOPID: not registered\n", szKind, eopid2);
    }
    return e3;
}
}  // namespace
void RegisterObjectGetter(int eopid, PFNOBJGETTER pfn)
{
    if (const int e3 = Eopid3Registered(eopid, "the getter"); e3 >= 0) MpGetter().try_emplace(e3, pfn);
}
void RegisterObjectGetter3(int eopid3, PFNOBJGETTER pfn) { MpGetter3()[eopid3] = pfn; }
void RegisterObjectSetter(int eopid, PFNOBJSETTER pfn)
{
    if (const int e3 = Eopid3Registered(eopid, "the setter"); e3 >= 0) MpSetter().try_emplace(e3, pfn);
}
void RegisterObjectSetter3(int eopid3, PFNOBJSETTER pfn) { MpSetter3()[eopid3] = pfn; }
void RegistryStats3(int* pcmethod, int* pcmethodDropped, int* pceopid, int* pceopidDropped)
{
    *pcmethod = Regstats().cmethod;
    *pcmethodDropped = Regstats().cmethodDropped;
    *pceopid = Regstats().ceopid;
    *pceopidDropped = Regstats().ceopidDropped;
}
PFNOBJSETTER PfnObjectSetter(int eopid)
{
    if (auto it3 = MpSetter3().find(eopid); it3 != MpSetter3().end()) return it3->second;
    auto it = MpSetter().find(eopid);
    return it == MpSetter().end() ? nullptr : it->second;
}
PFNOBJGETTER PfnObjectGetter(int eopid)
{
    if (auto it3 = MpGetter3().find(eopid); it3 != MpGetter3().end()) return it3->second;
    auto it = MpGetter().find(eopid);
    return it == MpGetter().end() ? nullptr : it->second;
}

PFNOBJMETHOD PfnObjectMethod(uint32_t pfnThunk)
{
    if (auto it3 = MpMethod3().find(pfnThunk); it3 != MpMethod3().end()) return it3->second;
    auto it = MpMethod().find(pfnThunk);
    return it == MpMethod().end() ? nullptr : it->second;
}

bool FMakeObjectMethod(uint32_t pfnThunk, BASIC* pbasic, int crefReq, CRef* pref)
{
    PFNOBJMETHOD pfn = PfnObjectMethod(pfnThunk);
    if (!pfn) return false;
    const int i = IthunkFor(pfnThunk, pfn);
    if (i < 0) return false;
    auto pmethod = std::make_shared<CMethod>();
    pmethod->m_pbasic = pbasic;
    pmethod->m_pfnthunk = s_athunk[i];
    pmethod->m_crefReq = crefReq;
    pref->SetMethod(pmethod);
    return true;
}

int NArg(const std::vector<CRef>& aref, size_t i, int nDefault)
{
    if (i >= aref.size()) return nDefault;
    const CRef& r = aref[i];
    switch (r.m_tagk)
    {
    case TAGK_S32: return r.m_n;
    case TAGK_F32: return static_cast<int>(r.m_g);
    case TAGK_Bool: return r.m_bool;
    case TAGK_Symid: return static_cast<int>(r.m_symid);
    default: return r.m_pbasic ? (dynamic_cast<LO*>(r.m_pbasic) ? dynamic_cast<LO*>(r.m_pbasic)->oid : nDefault) : nDefault;
    }
}

float GArg(const std::vector<CRef>& aref, size_t i, float gDefault)
{
    if (i >= aref.size()) return gDefault;
    const CRef& r = aref[i];
    if (r.m_tagk == TAGK_F32) return r.m_g;
    if (r.m_tagk == TAGK_S32) return static_cast<float>(r.m_n);
    return gDefault;
}

LO* PloArg(const std::vector<CRef>& aref, size_t i)
{
    if (i >= aref.size()) return nullptr;
    const CRef& r = aref[i];
    if (r.m_pbasic) return dynamic_cast<LO*>(r.m_pbasic);
    if (r.m_tagk == TAGK_S32 && g_psw) return PloFindSwObjectByOid(g_psw, r.m_n);
    return nullptr;
}

std::vector<int> AnArgList(const std::vector<CRef>& aref, size_t i)
{
    std::vector<int> an;
    if (i >= aref.size()) return an;
    const CRef& r = aref[i];
    if (r.m_tagk == TAGK_Pair)
    {
        for (const CPair* p = r.m_ppair.get(); p; p = p->m_ppairNext.get())
            an.push_back(NArg(std::vector<CRef>{p->m_ref}, 0));
    }
    else if (r.m_tagk != TAGK_Nil) an.push_back(NArg(aref, i));
    return an;
}

// orig: 0x0013E690 RefReadOption (the native OPTVAL-to-CRef bridge)
CRef RefFromOptionValue(const OPTVAL& v)
{
    CRef ref;
    if (v.k & 0x8000)
    {
        // a list: a CRef list of its values
        std::shared_ptr<CPair> ppairHead, ppairTail;
        for (const OPTVAL& e : v.alist)
        {
            auto p = PpairNew();
            p->m_ref = RefFromOptionValue(e);
            if (ppairTail) ppairTail->m_ppairNext = p;
            else ppairHead = p;
            ppairTail = p;
        }
        if (ppairHead) ref.SetPair(ppairHead);
        else ref.SetTag(TAGK_Nil);
        return ref;
    }
    switch (v.tagk)
    {
    case 0x15: ref.SetS32(v.n); break;
    case 0x16: ref.SetF32(v.ag[0]); break;
    case 0x17: ref.SetBool(v.n); break;
    case 0x1A: case 0x1C: ref.SetVector(glm::vec3(v.ag[0], v.ag[1], v.ag[2])); break;
    case 0x1D: ref.SetLm({v.ag[0], v.ag[1]}); break;
    default:
        // (a loader option without a CRef tag: by its kind (OTYP): 1 a float, 2 / 9 / 11 a vector (the physics port))
        if (v.tagk == -1 && v.k == 1) ref.SetF32(v.ag[0]);
        else if (v.tagk == -1 && (v.k == 2 || v.k == 9 || v.k == 11)) ref.SetVector(glm::vec3(v.ag[0], v.ag[1], v.ag[2]));
        else if (v.tagk == -1 && v.k == 10) ref.SetLm({v.ag[0], v.ag[1]});
        else ref.SetS32(v.n);
        break;
    }
    return ref;
}
