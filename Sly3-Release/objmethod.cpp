#include "objmethod.h"

#include "lo.h"
#include "pair.h"
#include "sw.h"

#include <array>
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

void RegisterObjectMethod(uint32_t pfnThunk, PFNOBJMETHOD pfn) { MpMethod()[pfnThunk] = pfn; }

namespace
{
std::unordered_map<int, PFNOBJGETTER>& MpGetter()
{
    static std::unordered_map<int, PFNOBJGETTER> s_map;
    return s_map;
}
}
void RegisterObjectGetter(int eopid, PFNOBJGETTER pfn) { MpGetter()[eopid] = pfn; }
namespace
{
std::unordered_map<int, PFNOBJSETTER>& MpSetter()
{
    static std::unordered_map<int, PFNOBJSETTER> s_map;
    return s_map;
}
}
void RegisterObjectSetter(int eopid, PFNOBJSETTER pfn) { MpSetter()[eopid] = pfn; }
PFNOBJSETTER PfnObjectSetter(int eopid)
{
    auto it = MpSetter().find(eopid);
    return it == MpSetter().end() ? nullptr : it->second;
}
PFNOBJGETTER PfnObjectGetter(int eopid)
{
    auto it = MpGetter().find(eopid);
    return it == MpGetter().end() ? nullptr : it->second;
}

PFNOBJMETHOD PfnObjectMethod(uint32_t pfnThunk)
{
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
