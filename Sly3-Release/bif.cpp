#include "bif.h"
#include "clock.h"
#include "sw.h"
#include "sly3/condset3.h"
#include "sly3/load3.h"
#include "update.h"

#include <cstring>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <cstdio>
#include <string>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "frame.h"
#include "pair.h"
#include "proc.h"
#include "spliceobj.h"
#include "vecmat.h"

// The built-ins get argc (the fixed arguments plus the rest list's elements) and argv (the fixed arguments,
// then the rest list in argv[crefReq] when the built-in takes one: tag 0x1F, or 0 (the empty list)).

float g_tSplice = 0.0f;

namespace
{
uint32_t s_uRandom = 0x12345678u;
float GRandomDefault(float gMin, float gMax)
{
    s_uRandom = s_uRandom * 1103515245u + 12345u;
    const float g = static_cast<float>((s_uRandom >> 8) & 0xFFFFFF) / 16777216.0f;
    return gMin + (gMax - gMin) * g;
}
}
float (*g_pfnGRandomSplice)(float gMin, float gMax) = GRandomDefault;
int (*g_pfnNRandomSplice)() = nullptr;   // the game's NRandom (0x11D3C0) once the game runs (game.cpp)

namespace
{
PFNBIF s_mpbifkpfnGame[BIFK_Max] = {};

CRef RefBool(bool f)
{
    CRef ref;
    ref.SetBool(f);
    return ref;
}
CRef RefS32(s32 n)
{
    CRef ref;
    ref.SetS32(n);
    return ref;
}
CRef RefF32(f32 g)
{
    CRef ref;
    ref.SetF32(g);
    return ref;
}
CRef RefVector(const glm::vec3& v)
{
    CRef ref;
    ref.SetVector(v);
    return ref;
}
CRef RefMatrix(const glm::mat4& m)
{
    CRef ref;
    ref.SetMatrix(m);
    return ref;
}
CRef RefVoid()
{
    return CRef(TAGK_Void);
}
float G(const CRef& ref)
{
    return ref.GAsFloat();
}

// The rest list's cells (argv[i] a list or the empty list).
CPair* PpairRest(const CRef& ref)
{
    return ref.m_tagk == TAGK_Pair ? ref.m_ppair.get() : nullptr;
}

CRef RefList(const std::vector<CRef>& aref)
{
    std::shared_ptr<CPair> ppairHead;
    CPair* ppairTail = nullptr;
    for (const CRef& ref : aref)
    {
        auto ppair = PpairNew();
        ppair->m_ref = ref;
        if (ppairTail)
            ppairTail->m_ppairNext = ppair;
        else
            ppairHead = ppair;
        ppairTail = ppair.get();
    }
    CRef ref;
    if (ppairHead)
        ref.SetPair(std::move(ppairHead));
    else
        ref.SetTag(TAGK_None);
    return ref;
}

std::vector<CRef> AListElements(const CRef& ref)
{
    std::vector<CRef> aref;
    for (CPair* ppair = PpairRest(ref); ppair; ppair = ppair->m_ppairNext.get())
        aref.push_back(ppair->m_ref);
    return aref;
}

// ---- arithmetic (0x11F440, 0x11F6D0, 0x11F960, 0x11FEC8): numbers are summed as floats, an int result
// when every operand was an int.

// orig: 0x0011F440 RefOpAdd
CRef RefOpAdd(int, CRef* aref, CFrame*)
{
    CPair* ppair = PpairRest(aref[1]);
    switch (aref[0].m_tagk)
    {
    case TAGK_S32:
    case TAGK_F32:
    {
        bool fInt = aref[0].m_tagk == TAGK_S32;
        float g = G(aref[0]);
        for (; ppair; ppair = ppair->m_ppairNext.get())
        {
            if (ppair->m_ref.m_tagk == TAGK_S32)
                g += static_cast<float>(ppair->m_ref.m_n);
            else
            {
                fInt = false;
                g += ppair->m_ref.m_g;
            }
        }
        return fInt ? RefS32(static_cast<s32>(g)) : RefF32(g);
    }
    case TAGK_Vector:
    {
        glm::vec3 v = aref[0].GetVector();
        for (; ppair; ppair = ppair->m_ppairNext.get())
            v += ppair->m_ref.GetVector();
        return RefVector(v);
    }
    case TAGK_Matrix:
    {
        glm::mat4 m = aref[0].GetMatrix();
        for (; ppair; ppair = ppair->m_ppairNext.get())
            m += ppair->m_ref.GetMatrix();
        return RefMatrix(m);
    }
    default:
        return CRef();
    }
}

// orig: 0x0011F6D0 RefOpSub (a single operand is returned as is: no negation)
CRef RefOpSub(int, CRef* aref, CFrame*)
{
    CPair* ppair = PpairRest(aref[1]);
    switch (aref[0].m_tagk)
    {
    case TAGK_S32:
    case TAGK_F32:
    {
        bool fInt = aref[0].m_tagk == TAGK_S32;
        float g = G(aref[0]);
        for (; ppair; ppair = ppair->m_ppairNext.get())
        {
            if (ppair->m_ref.m_tagk == TAGK_S32)
                g -= static_cast<float>(ppair->m_ref.m_n);
            else
            {
                fInt = false;
                g -= ppair->m_ref.m_g;
            }
        }
        return fInt ? RefS32(static_cast<s32>(g)) : RefF32(g);
    }
    case TAGK_Vector:
    {
        glm::vec3 v = aref[0].GetVector();
        for (; ppair; ppair = ppair->m_ppairNext.get())
            v -= ppair->m_ref.GetVector();
        return RefVector(v);
    }
    case TAGK_Matrix:
    {
        glm::mat4 m = aref[0].GetMatrix();
        for (; ppair; ppair = ppair->m_ppairNext.get())
            m -= ppair->m_ref.GetMatrix();
        return RefMatrix(m);
    }
    default:
        return CRef();
    }
}

// orig: 0x0011F960 RefOpMult (scalars, vectors, matrices (matrix * vector: a position, w = 1), CLQ, LM)
CRef RefOpMult(int, CRef* aref, CFrame*)
{
    CRef ref;
    bool fInt = false;
    switch (aref[0].m_tagk)
    {
    case TAGK_S32: ref.SetF32(static_cast<float>(aref[0].m_n)); fInt = true; break;
    case TAGK_F32: case TAGK_Vector: case TAGK_Matrix: case TAGK_Clq: case TAGK_Lm: aref[0].CloneTo(&ref, nullptr); break;
    default: break;
    }
    for (CPair* ppair = PpairRest(aref[1]); ppair; ppair = ppair->m_ppairNext.get())
    {
        const CRef& r = ppair->m_ref;
        const bool fNum = r.FNumber();
        switch (ref.m_tagk)
        {
        case TAGK_F32:
            switch (r.m_tagk)
            {
            case TAGK_F32: fInt = false; ref.m_g *= r.m_g; break;
            case TAGK_S32: ref.m_g *= static_cast<float>(r.m_n); break;
            case TAGK_Vector: ref = RefVector(r.GetVector() * ref.m_g); break;
            case TAGK_Matrix: ref = RefMatrix(r.GetMatrix() * ref.m_g); break;
            case TAGK_Clq: { const float g = ref.m_g; ref.SetTag(TAGK_Clq); ref.m_vector = r.m_vector * g; break; }
            case TAGK_Lm: { const float g = ref.m_g; ref.SetTag(TAGK_Lm); ref.m_vector = glm::vec4(r.m_vector.x * g, r.m_vector.y * g, 0, 0); break; }
            default: break;
            }
            break;
        case TAGK_Vector:
            if (fNum)
                ref.m_vector *= G(r);
            break;
        case TAGK_Matrix:
            if (fNum)
                *ref.m_pmatrix = ref.GetMatrix() * G(r);
            else if (r.m_tagk == TAGK_Vector)
                ref = RefVector(glm::vec3(ref.GetMatrix() * glm::vec4(r.GetVector(), 1.0f)));
            else if (r.m_tagk == TAGK_Matrix)
                *ref.m_pmatrix = ref.GetMatrix() * r.GetMatrix();
            break;
        case TAGK_Clq:
            if (fNum)
                ref.m_vector *= G(r);
            break;
        case TAGK_Lm:
            if (fNum)
            {
                ref.m_vector.x *= G(r);
                ref.m_vector.y *= G(r);
            }
            break;
        default:
            break;
        }
    }
    if (fInt && ref.m_tagk == TAGK_F32)
        return RefS32(static_cast<s32>(ref.m_g));
    return ref;
}

// orig: 0x0011FEC8 RefOpDiv
CRef RefOpDiv(int, CRef* aref, CFrame*)
{
    CRef ref;
    bool fInt = false;
    switch (aref[0].m_tagk)
    {
    case TAGK_S32: ref.SetF32(static_cast<float>(aref[0].m_n)); fInt = true; break;
    case TAGK_F32: case TAGK_Vector: case TAGK_Matrix: case TAGK_Clq: case TAGK_Lm: aref[0].CloneTo(&ref, nullptr); break;
    default: break;
    }
    for (CPair* ppair = PpairRest(aref[1]); ppair; ppair = ppair->m_ppairNext.get())
    {
        const CRef& r = ppair->m_ref;
        if (!r.FNumber())
            continue;
        const float d = G(r);
        switch (ref.m_tagk)
        {
        case TAGK_F32:
            if (r.m_tagk == TAGK_F32)
                fInt = false;
            ref.m_g /= d;
            break;
        case TAGK_Vector: ref.m_vector *= 1.0f / d; break;
        case TAGK_Matrix: *ref.m_pmatrix = ref.GetMatrix() * (1.0f / d); break;
        case TAGK_Clq: ref.m_vector *= 1.0f / d; break;
        case TAGK_Lm: { const float s = 1.0f / d; ref.m_vector.x *= s; ref.m_vector.y *= s; break; }
        default: break;
        }
    }
    if (fInt && ref.m_tagk == TAGK_F32)
        return RefS32(static_cast<s32>(ref.m_g));
    return ref;
}

// ---- printing (stdout when g_fSplicePrint)
bool g_fSplicePrint = true;

void PrintRef(const CRef& ref, std::string* ps)
{
    char ach[160];
    switch (ref.m_tagk)
    {
    case TAGK_S32: std::snprintf(ach, sizeof(ach), "%d", ref.m_n); break;
    case TAGK_F32: std::snprintf(ach, sizeof(ach), "%g", ref.m_g); break;
    case TAGK_Bool: std::snprintf(ach, sizeof(ach), "%s", ref.m_bool ? "#t" : "#f"); break;
    case TAGK_Symid: std::snprintf(ach, sizeof(ach), "sym%u", ref.m_symid); break;
    case TAGK_Bifk: std::snprintf(ach, sizeof(ach), "<%s>", ref.m_bifk >= 0 && ref.m_bifk < BIFK_Max ? g_mpbifkbif[ref.m_bifk].achz : "?"); break;
    case TAGK_Vector: std::snprintf(ach, sizeof(ach), "<%g %g %g>", ref.m_vector.x, ref.m_vector.y, ref.m_vector.z); break;
    case TAGK_Clq: std::snprintf(ach, sizeof(ach), "clq<%g %g %g>", ref.m_vector.x, ref.m_vector.y, ref.m_vector.z); break;
    case TAGK_Lm: std::snprintf(ach, sizeof(ach), "lm<%g %g>", ref.m_vector.x, ref.m_vector.y); break;
    case TAGK_Smp: std::snprintf(ach, sizeof(ach), "smp<%g %g %g>", ref.m_vector.x, ref.m_vector.y, ref.m_vector.z); break;
    case TAGK_Pair:
    {
        *ps += "(";
        bool fFirst = true;
        for (CPair* ppair = ref.m_ppair.get(); ppair; ppair = ppair->m_ppairNext.get())
        {
            if (!fFirst)
                *ps += " ";
            fFirst = false;
            PrintRef(ppair->m_ref, ps);
        }
        *ps += ")";
        return;
    }
    case TAGK_None: std::snprintf(ach, sizeof(ach), "()"); break;
    default: std::snprintf(ach, sizeof(ach), "<%s>", PchzFromTagk(ref.m_tagk)); break;
    }
    *ps += ach;
}

// orig: 0x001202D0 RefOpPrint
CRef RefOpPrint(int, CRef* aref, CFrame*)
{
    std::string s;
    PrintRef(aref[0], &s);
    if (g_fSplicePrint)
        std::printf("%s", s.c_str());
    return RefVoid();
}
// orig: 0x00120300 RefOpPrintFrame
CRef RefOpPrintFrame(int, CRef*, CFrame*) { return RefVoid(); }
// orig: 0x00120330 RefOpPrintSidebag
CRef RefOpPrintSidebag(int, CRef*, CFrame*) { return RefVoid(); }

// ---- comparisons
// orig: 0x00120370 (the payload words are the same)
CRef RefOpIntEqual(int, CRef* aref, CFrame*)
{
    if (aref[0].m_tagk == TAGK_Basic || aref[1].m_tagk == TAGK_Basic)
        return RefBool(aref[0].m_pbasic == aref[1].m_pbasic && aref[0].m_tagk == aref[1].m_tagk);
    if (aref[0].m_tagk >= TAGK_Vector && aref[0].m_tagk != TAGK_Void)
        return RefBool(aref[0] == aref[1]);
    return RefBool(aref[0].m_n == aref[1].m_n);
}
// orig: 0x00120488 / 0x001204B8 / 0x001204E8 / 0x00120518 (numbers, compared as floats)
CRef RefOpL(int, CRef* aref, CFrame*) { return RefBool(G(aref[0]) < G(aref[1])); }
CRef RefOpLE(int, CRef* aref, CFrame*) { return RefBool(G(aref[0]) <= G(aref[1])); }
CRef RefOpG(int, CRef* aref, CFrame*) { return RefBool(G(aref[0]) > G(aref[1])); }
CRef RefOpGE(int, CRef* aref, CFrame*) { return RefBool(G(aref[0]) >= G(aref[1])); }
// orig: 0x00120548 RefOpEqv
CRef RefOpEqv(int, CRef* aref, CFrame*) { return RefBool(aref[0] == aref[1]); }

bool FEqual(const CRef& a, const CRef& b)
{
    if (a.m_tagk == TAGK_Pair && b.m_tagk == TAGK_Pair)
    {
        CPair* pa = a.m_ppair.get();
        CPair* pb = b.m_ppair.get();
        for (; pa && pb; pa = pa->m_ppairNext.get(), pb = pb->m_ppairNext.get())
            if (!FEqual(pa->m_ref, pb->m_ref))
                return false;
        return pa == nullptr && pb == nullptr;
    }
    if (a.m_tagk == TAGK_Matrix && b.m_tagk == TAGK_Matrix)
        return a.GetMatrix() == b.GetMatrix();
    return a == b;
}
// orig: 0x001207F0 RefOpEqual
CRef RefOpEqual(int, CRef* aref, CFrame*) { return RefBool(FEqual(aref[0], aref[1])); }

// ---- predicates (0x120820..0x120A20)
CRef RefOpIsBoolean(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Bool); }
CRef RefOpIsNum(int, CRef* a, CFrame*) { return RefBool(a[0].FNumber()); }
CRef RefOpIsInteger(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_S32); }
CRef RefOpIsFloat(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_F32); }
CRef RefOpIsSymbol(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Symid); }
CRef RefOpIsVector(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Vector); }
CRef RefOpIsMatrix(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Matrix); }
CRef RefOpIsClq(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Clq); }
CRef RefOpIsLm(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Lm); }
CRef RefOpIsSmp(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Smp); }
CRef RefOpIsList(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Pair || a[0].m_tagk == TAGK_None); }
CRef RefOpIsNull(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_None); }
CRef RefOpIsObject(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Basic); }
CRef RefOpIsNullObj(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Basic && a[0].m_pbasic == nullptr); }
CRef RefOpIsMethod(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Method); }
CRef RefOpIsProcedure(int, CRef* a, CFrame*) { return RefBool(a[0].m_tagk == TAGK_Proc || a[0].m_tagk == TAGK_Bifk); }
// orig: 0x00120A58 (FFloatsNear(a, b, epsilon))
CRef RefOpAreNear(int, CRef* a, CFrame*)
{
    const float g1 = G(a[0]), g2 = G(a[1]), gEps = G(a[2]);
    const float gScale = std::fabs(g1) < 1.0f ? 1.0f : std::fabs(g1);
    return RefBool(std::fabs(g1 - g2) / gScale < gEps);
}
// orig: 0x00120AE0 RefOpNot (only #f is false)
CRef RefOpNot(int, CRef* a, CFrame*) { return RefBool(a[0].FFalse()); }

// ---- lists
// orig: 0x00120B08 RefOpCons
CRef RefOpCons(int, CRef* a, CFrame*)
{
    auto ppair = PpairNew();
    ppair->m_ref = a[0];
    if (a[1].m_tagk == TAGK_Pair)
        ppair->m_ppairNext = a[1].m_ppair;
    CRef ref;
    ref.SetPair(std::move(ppair));
    return ref;
}
// orig: 0x00120BD0 RefOpCar
CRef RefOpCar(int, CRef* a, CFrame*)
{
    CPair* ppair = PpairRest(a[0]);
    return ppair ? ppair->m_ref : CRef();
}
// orig: 0x00120C00 RefOpCdr (the empty list after the last cell)
CRef RefOpCdr(int, CRef* a, CFrame*)
{
    CPair* ppair = PpairRest(a[0]);
    CRef ref;
    if (ppair && ppair->m_ppairNext)
        ref.SetPair(ppair->m_ppairNext);
    else
        ref.SetTag(TAGK_None);
    return ref;
}
// orig: 0x00120CC8 RefOpSetCar
CRef RefOpSetCar(int, CRef* a, CFrame*)
{
    if (CPair* ppair = PpairRest(a[0]))
        ppair->m_ref = a[1];
    return RefVoid();
}
// orig: 0x00120CF8 RefOpSetCdr
CRef RefOpSetCdr(int, CRef* a, CFrame*)
{
    if (CPair* ppair = PpairRest(a[0]))
        ppair->m_ppairNext = a[1].m_tagk == TAGK_Pair ? a[1].m_ppair : nullptr;
    return RefVoid();
}
// orig: 0x00120D28 RefOpLength
CRef RefOpLength(int, CRef* a, CFrame*)
{
    int c = 0;
    for (CPair* ppair = PpairRest(a[0]); ppair; ppair = ppair->m_ppairNext.get())
        c++;
    return RefS32(c);
}
// orig: 0x00120D78 RefOpNth
CRef RefOpNth(int, CRef* a, CFrame*)
{
    int i = a[1].RefCoerceS32().m_n;
    for (CPair* ppair = PpairRest(a[0]); ppair; ppair = ppair->m_ppairNext.get(), i--)
        if (i == 0)
            return ppair->m_ref;
    return CRef();
}
// orig: 0x00120E10 RefOpIsMember (eqv?)
CRef RefOpIsMember(int, CRef* a, CFrame*)
{
    for (CPair* ppair = PpairRest(a[1]); ppair; ppair = ppair->m_ppairNext.get())
        if (ppair->m_ref == a[0])
            return RefBool(true);
    return RefBool(false);
}
// orig: 0x00120F80 RefOpList (the rest list itself)
CRef RefOpList(int, CRef* a, CFrame*) { return a[0]; }
// orig: 0x00120FB0 (a shuffled copy of the list: each element moved to a random place)
CRef RefOpShuffle(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref = AListElements(a[0]);
    for (int i = static_cast<int>(aref.size()) - 1; i > 0; i--)
    {
        int j = static_cast<int>(g_pfnGRandomSplice(0.0f, static_cast<float>(i + 1)));
        if (j > i)
            j = i;
        std::swap(aref[static_cast<std::size_t>(i)], aref[static_cast<std::size_t>(j)]);
    }
    return RefList(aref);
}
// orig: 0x001212F8 RefOpAppend (lists' elements, copied)
CRef RefOpAppend(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref;
    for (CPair* ppair = PpairRest(a[0]); ppair; ppair = ppair->m_ppairNext.get())
        for (CPair* ppairElt = PpairRest(ppair->m_ref); ppairElt; ppairElt = ppairElt->m_ppairNext.get())
            aref.push_back(ppairElt->m_ref);
    return RefList(aref);
}
// orig: 0x00121710 RefOpMap
CRef RefOpMap(int, CRef* a, CFrame* pframe)
{
    std::vector<CRef> aref;
    for (CPair* ppair = PpairRest(a[1]); ppair; ppair = ppair->m_ppairNext.get())
        aref.push_back(RefApply(a[0], 1, &ppair->m_ref, pframe));
    return RefList(aref);
}
// orig: 0x00121938 RefOpFilter
CRef RefOpFilter(int, CRef* a, CFrame* pframe)
{
    std::vector<CRef> aref;
    for (CPair* ppair = PpairRest(a[1]); ppair; ppair = ppair->m_ppairNext.get())
        if (RefApply(a[0], 1, &ppair->m_ref, pframe).FTrue())
            aref.push_back(ppair->m_ref);
    return RefList(aref);
}
// orig: 0x00121C20 RefOpForEach
CRef RefOpForEach(int, CRef* a, CFrame* pframe)
{
    for (CPair* ppair = PpairRest(a[1]); ppair; ppair = ppair->m_ppairNext.get())
        RefApply(a[0], 1, &ppair->m_ref, pframe);
    return RefVoid();
}
// orig: 0x00121D20 RefOpEval (the value as code, in the caller's frame)
CRef RefOpEval(int, CRef* a, CFrame* pframe)
{
    CPair pair;
    pair.m_ref = a[0];
    return RefEval(&pair, pframe);
}

// ---- constructors
// orig: 0x00121DD8 RefOpVector (0 values: the zero vector; 1: the value on all three; 3: x y z)
CRef RefOpVector(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref = AListElements(a[0]);
    glm::vec3 v(0.0f);
    if (aref.size() == 1)
        v = glm::vec3(G(aref[0]));
    else if (aref.size() >= 3)
        v = glm::vec3(G(aref[0]), G(aref[1]), G(aref[2]));
    return RefVector(v);
}
// orig: 0x00121F00 RefOpMatrix (0: identity; 1 matrix: a copy; 3 or 4 vectors: the columns; 16 numbers)
CRef RefOpMatrix(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref = AListElements(a[0]);
    glm::mat4 m(1.0f);
    if (aref.size() == 1 && aref[0].m_tagk == TAGK_Matrix)
        m = aref[0].GetMatrix();
    else if ((aref.size() == 3 || aref.size() == 4) && aref[0].m_tagk == TAGK_Vector)
    {
        for (std::size_t i = 0; i < aref.size(); i++)
            m[static_cast<int>(i)] = glm::vec4(aref[i].GetVector(), i == 3 ? 1.0f : 0.0f);
    }
    else if (aref.size() == 16)
    {
        for (int i = 0; i < 16; i++)
            m[i / 4][i % 4] = G(aref[static_cast<std::size_t>(i)]);
    }
    return RefMatrix(m);
}
// orig: 0x001220B8 / 0x00122168 / 0x00122218
CRef RefOpClq(int, CRef* a, CFrame*)
{
    CRef ref;
    ref.SetClq({G(a[0]), G(a[1]), G(a[2]), 0.0f});
    return ref;
}
CRef RefOpLm(int, CRef* a, CFrame*)
{
    CRef ref;
    ref.SetLm({G(a[0]), G(a[1])});
    return ref;
}
CRef RefOpSmp(int, CRef* a, CFrame*)
{
    CRef ref;
    ref.SetSmp({G(a[0]), G(a[1]), G(a[2])});
    return ref;
}
// orig: 0x001222C8 RefOpGetElement (a vector's / CLQ's / LM's / SMP's element i, a matrix's (i, j))
CRef RefOpGetElement(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref = AListElements(a[1]);
    const int i = aref.empty() ? 0 : aref[0].RefCoerceS32().m_n;
    switch (a[0].m_tagk)
    {
    case TAGK_Vector: case TAGK_Clq: case TAGK_Lm: case TAGK_Smp:
        return i >= 0 && i < 4 ? RefF32(a[0].m_vector[i]) : CRef();
    case TAGK_Matrix:
    {
        const int j = aref.size() > 1 ? aref[1].RefCoerceS32().m_n : 0;
        if (i < 0 || i > 3 || j < 0 || j > 3)
            return CRef();
        if (aref.size() > 1)
            return RefF32(a[0].GetMatrix()[j][i]);
        return RefVector(glm::vec3(a[0].GetMatrix()[i]));
    }
    default:
        return CRef();
    }
}
// orig: 0x001223C0 RefOpRandom (ints: an int in [a, b])
CRef RefOpRandom(int, CRef* a, CFrame*)
{
    if (a[0].m_tagk == TAGK_S32 && a[1].m_tagk == TAGK_S32)
    {
        // orig: 0x0011D3F8 NRandInRange: nLow + NRandom() % (nHigh - nLow + 1) (e.g. the Ep5 tanks' fire chance
        // (> 0.2 (random 0 1)) in their aim loop's script: 0 or 1)
        if (g_pfnNRandomSplice)
        {
            const int d = a[1].m_n - (a[0].m_n - 1);
            const int n = d == 0 ? a[0].m_n : a[0].m_n + g_pfnNRandomSplice() % d;
            if (static const bool s_f = std::getenv("SLY2N_RANDPRINT") != nullptr; s_f)   // debugging
                std::printf("random t %.3f %d..%d -> %d\n", g_clock.t, a[0].m_n, a[1].m_n, n);
            return RefS32(n);
        }
        int n = static_cast<int>(g_pfnGRandomSplice(static_cast<float>(a[0].m_n), static_cast<float>(a[1].m_n + 1)));
        if (n > a[1].m_n)
            n = a[1].m_n;
        return RefS32(n);
    }
    float g = g_pfnGRandomSplice(G(a[0]), G(a[1]));
    // (debug: SLY2N_RANDFORCE="g,g,...": the float draws of (random 0 1) replaced in order by these values, e.g. the
    // reference's, to check that a script's outcome depends only on them)
    if (static const char* s_szForce = std::getenv("SLY2N_RANDFORCE"); s_szForce && G(a[0]) == 0.0f && G(a[1]) == 1.0f)
    {
        static const char* s_pch = s_szForce;
        if (s_pch && *s_pch)
        {
            g = std::strtof(s_pch, nullptr);
            s_pch = std::strchr(s_pch, ',');
            if (s_pch) ++s_pch;
        }
    }
    if (static const bool s_f = std::getenv("SLY2N_RANDPRINT") != nullptr; s_f)   // debugging
        std::printf("random t %.3f %g..%g -> %.4f (float)\n", g_clock.t, G(a[0]), G(a[1]), g);
    return RefF32(g);
}

// ---- math
float RadNormalizeG(float rad)
{
    const float pi2 = 6.2831855f;
    if (rad < -3.1415927f || rad > 3.1415927f)
    {
        rad = std::fmod(rad + 3.1415927f, pi2);
        if (rad < 0.0f)
            rad += pi2;
        rad -= 3.1415927f;
    }
    return rad;
}
CRef RefOpSqrt(int, CRef* a, CFrame*) { return RefF32(std::sqrt(G(a[0]))); }
CRef RefOpSin(int, CRef* a, CFrame*) { return RefF32(std::sin(G(a[0]))); }
CRef RefOpCos(int, CRef* a, CFrame*) { return RefF32(std::cos(G(a[0]))); }
CRef RefOpTan(int, CRef* a, CFrame*) { return RefF32(std::tan(G(a[0]))); }
CRef RefOpAsin(int, CRef* a, CFrame*) { return RefF32(std::asin(G(a[0]))); }
CRef RefOpAcos(int, CRef* a, CFrame*) { return RefF32(std::acos(G(a[0]))); }
CRef RefOpAtan(int, CRef* a, CFrame*) { return RefF32(std::atan(G(a[0]))); }
CRef RefOpRadNormalize(int, CRef* a, CFrame*) { return RefF32(RadNormalizeG(G(a[0]))); }
CRef RefOpAtan2(int, CRef* a, CFrame*) { return RefF32(std::atan2(G(a[0]), G(a[1]))); }
CRef RefOpVectorDot(int, CRef* a, CFrame*) { return RefF32(glm::dot(a[0].GetVector(), a[1].GetVector())); }
CRef RefOpVectorCross(int, CRef* a, CFrame*) { return RefVector(glm::cross(a[0].GetVector(), a[1].GetVector())); }
CRef RefOpVectorLth(int, CRef* a, CFrame*) { return RefF32(glm::length(a[0].GetVector())); }
// orig: 0x00122810 / 0x001228F0 (one vector: its length; two: their distance)
CRef RefOpVectorDistance(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref = AListElements(a[1]);
    glm::vec3 v = a[0].GetVector();
    if (!aref.empty())
        v -= aref[0].GetVector();
    return RefF32(glm::length(v));
}
CRef RefOpVectorDistanceSquared(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref = AListElements(a[1]);
    glm::vec3 v = a[0].GetVector();
    if (!aref.empty())
        v -= aref[0].GetVector();
    return RefF32(glm::dot(v, v));
}
// orig: 0x001229B8 (normalized, optionally to a length)
CRef RefOpVectorNormalize(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref = AListElements(a[1]);
    glm::vec3 v = a[0].GetVector();
    const float g = glm::length(v);
    if (g > 0.0f)
        v /= g;
    if (!aref.empty())
        v *= G(aref[0]);
    return RefVector(v);
}
CRef RefOpVectorProjectNormal(int, CRef* a, CFrame*)
{
    const glm::vec3 n = a[1].GetVector();
    return RefVector(n * glm::dot(a[0].GetVector(), n));
}
CRef RefOpVectorProjectTangent(int, CRef* a, CFrame*)
{
    const glm::vec3 v = a[0].GetVector(), n = a[1].GetVector();
    return RefVector(v - n * glm::dot(v, n));
}
// ---- matrices and rotations (Sly 2's mat.cpp / vec.cpp math: 0x11B500.., 0x11E390, 0x11E4B0, 0x1D6848).
// A MATRIX4 is 4 quadwords: rows 0..2 the basis, row 3 the position; here each quadword is a glm column,
// so the original's "transform by A" is A * v and its products (0x11C168, MulM3) are A * B.

const glm::vec3 s_vecX(1.0f, 0.0f, 0.0f);
constexpr float kEpsilon = 1e-5f;  // (EPSILON in mat.cpp; 0x322BCC77 = 1e-8 for the cross products)

// orig: 0x0011E390 PerpendicularVector
glm::vec3 VecPerpendicular(const glm::vec3& v)
{
    const float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
    glm::vec3 p;
    if (ax <= ay)
        p = ax <= az ? glm::vec3(0.0f, v.z, -v.y) : glm::vec3(v.y, -v.x, 0.0f);
    else
        p = ay <= az ? glm::vec3(v.z, 0.0f, -v.x) : glm::vec3(v.y, -v.x, 0.0f);
    const glm::vec3 c = glm::cross(v, p);
    return c / glm::length(c);
}

// orig: 0x0011B4.. BuildRotateMatrixSinCos (the rotation about a unit axis)
glm::mat3 MatRotateSinCos(float s, float c, const glm::vec3& n)
{
    const float t = 1.0f - c;
    glm::mat3 m;
    m[0] = glm::vec3(n.x * n.x * t + c, n.y * n.x * t + n.z * s, n.z * n.x * t - n.y * s);
    m[1] = glm::vec3(n.x * n.y * t - n.z * s, n.y * n.y * t + c, n.z * n.y * t + n.x * s);
    m[2] = glm::vec3(n.x * n.z * t + n.y * s, n.y * n.z * t - n.x * s, n.z * n.z * t + c);
    return m;
}

// orig: 0x0011B500 BuildRotateMatrixAxis
glm::mat3 MatRotateAxis(float rad, const glm::vec3& n)
{
    return MatRotateSinCos(std::sin(rad), std::cos(rad), n);
}

// orig: 0x0011BA08 BuildRotateVectorsMatrix (the rotation taking direction va to vb; identity when either is
// too short)
glm::mat3 MatRotateVectors(const glm::vec3& va, const glm::vec3& vb)
{
    const float la = glm::length(va), lb = glm::length(vb);
    if (la < kEpsilon || lb < kEpsilon)
        return glm::mat3(1.0f);
    glm::vec3 n = glm::cross(va, vb);
    const float lc = glm::length(n);
    n = lc > 1e-8f ? n / lc : VecPerpendicular(va);
    float c = glm::dot(va, vb) / (la * lb);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);  // GLimitAbs(.., 1)
    return MatRotateSinCos(std::sqrt(1.0f - c * c), c, n);
}

// orig: 0x0011BCF8 DecomposeRotateMatrixRad (a rotation's angle and unit axis; m[i][j]: the original's
// row i's component j)
void DecomposeRotateMatrixRad(const glm::mat3& m, float* prad, glm::vec3* pvec)
{
    const float c = ((m[0][0] + m[1][1]) + m[2][2] - 1.0f) * 0.5f;
    if (c > 0.99999899f)  // 0x3F7FFFEF
    {
        *prad = 0.0f;
        *pvec = s_vecX;
        return;
    }
    glm::vec3& v = *pvec;
    float gRest = 1.0f;
    const float d = 1.0f - c;
    for (int i = 0; i < 2; i++)
    {
        float a = std::fabs((m[i][i] - c) / d);
        if (gRest < a)
            a = gRest;
        v[i] = std::sqrt(a);
        gRest -= a;
    }
    v.z = std::sqrt(gRest);
    if (c < -0.99990761f)  // 0xBF7FF972: a half turn, the signs from the off-diagonal of the largest component
    {
        *prad = 3.14159274f;
        const float x = v.x, y = v.y, z = v.z;
        if (y < x)
        {
            if (z < x)
            {
                if (m[0][1] < 0.0f) v.y = -y;
                if (m[0][2] < 0.0f) v.z = -v.z;
                return;
            }
        }
        else if (z < y)
        {
            if (m[1][0] < 0.0f) v.x = -x;
            if (m[1][2] < 0.0f) v.z = -v.z;
            return;
        }
        if (m[2][0] < 0.0f) v.x = -x;
        if (m[2][1] < 0.0f) v.y = -v.y;
        return;
    }
    *prad = std::acos(c);
    if (m[0][1] < m[1][0]) v.z = -v.z;
    if (m[2][0] < m[0][2]) v.y = -v.y;
    if (m[1][2] < m[2][1]) v.x = -v.x;
}

CRef RefPair2(const CRef& a, const CRef& b)
{
    return RefList({a, b});
}

// orig: 0x00122BE0 (vector-ballistic-velocity from to g [object]: 0x1D6848 with the acceleration (0, 0, -1750),
// or the object's (its +0xE8's +0x80) through g_splicegame.pfnVecAcceleration)
CRef RefOpVectorBallisticVelocity(int, CRef* a, CFrame*)
{
    glm::vec3 acc(0.0f, 0.0f, -1750.0f);
    if (CPair* ppair = PpairRest(a[3]))
        if (ppair->m_ref.m_pbasic && g_splicegame.pfnVecAcceleration)
            g_splicegame.pfnVecAcceleration(ppair->m_ref.m_pbasic, &acc);
    const glm::vec3 start = a[0].GetVector(), end = a[1].GetVector();
    const float g = G(a[2]);
    // orig: 0x001D6848: the launch angle k between the line's elevation and straight up (by sin g), the speed
    // reaching end under acc
    float k = std::sin(g);
    glm::vec3 delta = end - start;
    const float z = delta.z;
    delta.z = 0.0f;
    const float len = glm::length(delta);
    float angle = std::atan2(z, len);
    if (angle < 0.0f)
        angle = 0.0f;
    const float accZ = acc.z;  // dot(acc, (0, 0, 1))
    k = angle + (1.57079637f - angle) * k;
    float q = accZ / ((z - len * std::tan(k)) * 2.0f);
    if (q <= 0.0f)
        return RefVector(glm::vec3(0.0f));
    q = std::sqrt(q);
    const float speed = len * q / std::cos(k);
    glm::vec3 vel = delta;
    vel.z = len * std::tan(k);
    return RefVector(glm::normalize(vel) * speed);
}

// orig: 0x00122CE0 (vector-radian-normal a b: the list (angle axis), 0x11E4B0 RadBetweenVectors)
CRef RefOpVectorRadianNormal(int, CRef* a, CFrame*)
{
    const glm::vec3 va = a[0].GetVector(), vb = a[1].GetVector();
    const float la = glm::length(va), lb = glm::length(vb);
    if (la < kEpsilon || lb < kEpsilon)
        return RefPair2(RefF32(0.0f), RefVector(s_vecX));
    glm::vec3 n = glm::cross(va, vb);
    const float lc = glm::length(n);
    n = lc > 1e-8f ? n / lc : VecPerpendicular(va);
    float c = glm::dot(va, vb) / (la * lb);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return RefPair2(RefF32(std::acos(c)), RefVector(n));
}

// orig: 0x00122DE0 (matrix-transpose: 0x11C1D8, all of the 4x4)
CRef RefOpMatrixTranspose(int, CRef* a, CFrame*)
{
    return RefMatrix(glm::transpose(a[0].GetMatrix()));
}
// orig: 0x00122E40 (matrix-invert: 0x11C2F0 FInvertMatrix4, the general 4x4 inverse)
CRef RefOpMatrixInvert(int, CRef* a, CFrame*) { return RefMatrix(glm::inverse(a[0].GetMatrix())); }

// orig: 0x00122EA0 (matrix-calculate-dmat a b: 0x11C3D0, b times a's affine inverse 0x11C318)
CRef RefOpMatrixCalculateDmat(int, CRef* a, CFrame*)
{
    return RefMatrix(a[1].GetMatrix() * glm::affineInverse(a[0].GetMatrix()));
}

// orig: 0x00122F00 (matrix-interpolate-rotate a b t: the rotation from a to b (0x11BC00 b a^T), its angle and
// axis (0x11BCF8), the rotation by t of the angle (0x11B500) applied to a; position 0)
CRef RefOpMatrixInterpolateRotate(int, CRef* a, CFrame*)
{
    const glm::mat3 matA(a[0].GetMatrix()), matB(a[1].GetMatrix());
    float rad;
    glm::vec3 n;
    DecomposeRotateMatrixRad(matB * glm::transpose(matA), &rad, &n);
    glm::mat4 m(MatRotateAxis(rad * a[2].m_g, n) * matA);  // (the original reads t's payload as a float)
    return RefMatrix(m);
}

// orig: 0x00123038 (matrix-decompose-to-translate: row 3)
CRef RefOpMatrixDecomposeToTranslate(int, CRef* a, CFrame*) { return RefVector(glm::vec3(a[0].GetMatrix()[3])); }
// orig: 0x00123080 (matrix-decompose-to-rotate: 0x1292B0, a copy of the whole matrix, position included)
CRef RefOpMatrixDecomposeToRotate(int, CRef* a, CFrame*) { return RefMatrix(a[0].GetMatrix()); }

// orig: 0x001230C8 (matrix-decompose-to-euler: 0x11BFF0 DecomposeEulerMatrix; at the poles x is 0)
CRef RefOpMatrixDecomposeToEuler(int, CRef* a, CFrame*)
{
    const glm::mat4& m = a[0].GetMatrix();
    float g = -m[0][2];
    g = g > 1.0f ? 1.0f : (g < -1.0f ? -1.0f : g);
    const float ry = std::asin(g);
    float rx, rz;
    if (ry < 1.57070000f)  // 0x3FC90C94
    {
        if (ry > -1.57070000f)
        {
            rx = std::atan2(m[1][2], m[2][2]);
            rz = std::atan2(m[0][1], m[0][0]);
        }
        else
        {
            rz = std::atan2(-m[1][0], -m[2][0]);
            rx = 0.0f;
        }
    }
    else
    {
        rz = std::atan2(-m[1][0], m[2][0]);
        rx = 0.0f;
    }
    return RefVector(glm::vec3(rx, ry, rz));
}

// orig: 0x00123118 (matrix-decompose-to-radian-normal: the list (angle axis), 0x11BCF8)
CRef RefOpMatrixDecomposeToRadianNormal(int, CRef* a, CFrame*)
{
    float rad;
    glm::vec3 n;
    DecomposeRotateMatrixRad(glm::mat3(a[0].GetMatrix()), &rad, &n);
    return RefPair2(RefF32(rad), RefVector(n));
}

// orig: 0x00123208 (matrix-look-at from to [up]: 0x11B830 with x along to - from, y = up x x normalized (any
// perpendicular when degenerate), z = x x y; up (0, 0, 1) by default; rows' w 0, the position row left as
// the new matrix node had it (here 0))
CRef RefOpMatrixLookAt(int, CRef* a, CFrame*)
{
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if (CPair* ppair = PpairRest(a[2]))
        up = ppair->m_ref.GetVector();
    const glm::vec3 dir = a[1].GetVector() - a[0].GetVector();
    const float l = glm::length(dir);
    const glm::vec3 x = l < kEpsilon ? s_vecX : dir / l;
    glm::vec3 y = glm::cross(up, x);
    const float ly = glm::length(y);
    y = ly < kEpsilon ? VecPerpendicular(x) : y / ly;
    const glm::vec3 z = glm::cross(x, y);
    glm::mat4 m(0.0f);
    m[0] = glm::vec4(x, 0.0f);
    m[1] = glm::vec4(y, 0.0f);
    m[2] = glm::vec4(z, 0.0f);
    return RefMatrix(m);
}

// orig: 0x001232D0 (matrix-tilt-upright: 0x11B938 LevelMatrix3, the rotation taking row 2 to z applied to the
// basis; the position kept, w 1)
CRef RefOpMatrixTiltUpright(int, CRef* a, CFrame*)
{
    const glm::mat4& src = a[0].GetMatrix();
    const glm::mat3 rot = MatRotateVectors(glm::vec3(src[2]), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 m(rot * glm::mat3(src));
    m[3] = glm::vec4(glm::vec3(src[3]), 1.0f);
    return RefMatrix(m);
}

// orig: 0x00123380 (g0 + g1 x + g2 x^2)
CRef RefOpClqEvaluate(int, CRef* a, CFrame*)
{
    const CLQ clq = a[0].GetClq();
    const float x = G(a[1]);
    return RefF32(clq.g0 + x * (clq.g1 + x * clq.g2));
}
// orig: 0x001233F0 (clamped by an LM)
CRef RefOpClqEvaluateLm(int, CRef* a, CFrame*)
{
    const CLQ clq = a[0].GetClq();
    const float x = G(a[1]);
    float g = clq.g0 + x * (clq.g1 + x * clq.g2);
    const LM lm = a[2].GetLm();
    if (g < lm.gMin)
        g = lm.gMin;
    else if (g > lm.gMax)
        g = lm.gMax;
    return RefF32(g);
}
// orig: 0x00123498 RefOpClqFit (0x11D8D8 FitClq: through (0, g0), (1, g1), (u, gU))
CRef RefOpClqFit(int, CRef* a, CFrame*)
{
    const float g0 = G(a[0]), g1 = G(a[1]), u = G(a[2]), gU = G(a[3]);
    const float g2 = ((gU - g0) / u - (g1 - g0)) / (u - 1.0f);
    CRef ref;
    ref.SetClq({g0, (g1 - g0) - g2, g2, 0.0f});
    return ref;
}
// orig: 0x001235A8 / 0x00123608
CRef RefOpLmLimit(int, CRef* a, CFrame*)
{
    const LM lm = a[0].GetLm();
    float g = G(a[1]);
    if (g < lm.gMin)
        g = lm.gMin;
    else if (g > lm.gMax)
        g = lm.gMax;
    return RefF32(g);
}
CRef RefOpLmCheck(int, CRef* a, CFrame*)
{
    const LM lm = a[0].GetLm();
    const float g = G(a[1]);
    return RefBool(g >= lm.gMin && g <= lm.gMax);
}
// orig: 0x00123668 .. 0x001236F8
CRef RefOpFloor(int, CRef* a, CFrame*) { return a[0].m_tagk == TAGK_S32 ? a[0] : RefS32(static_cast<s32>(std::floor(a[0].m_g))); }
CRef RefOpCeiling(int, CRef* a, CFrame*) { return a[0].m_tagk == TAGK_S32 ? a[0] : RefS32(static_cast<s32>(std::ceil(a[0].m_g))); }
CRef RefOpRound(int, CRef* a, CFrame*) { return a[0].m_tagk == TAGK_S32 ? a[0] : RefS32(static_cast<s32>(std::floor(a[0].m_g + 0.5f))); }
CRef RefOpTruncate(int, CRef* a, CFrame*) { return a[0].RefCoerceS32(); }
CRef RefOpAbs(int, CRef* a, CFrame*)
{
    if (a[0].m_tagk == TAGK_S32)
        return RefS32(a[0].m_n < 0 ? -a[0].m_n : a[0].m_n);
    return RefF32(std::fabs(a[0].m_g));
}
// orig: 0x00123788 / 0x001238B0 (the chosen operand itself)
CRef RefOpMaximum(int, CRef* a, CFrame*)
{
    CRef ref = a[0];
    for (CPair* ppair = PpairRest(a[1]); ppair; ppair = ppair->m_ppairNext.get())
        if (G(ppair->m_ref) > G(ref))
            ref = ppair->m_ref;
    return ref;
}
CRef RefOpMinimum(int, CRef* a, CFrame*)
{
    CRef ref = a[0];
    for (CPair* ppair = PpairRest(a[1]); ppair; ppair = ppair->m_ppairNext.get())
        if (G(ppair->m_ref) < G(ref))
            ref = ppair->m_ref;
    return ref;
}
// orig: 0x001239D8 RefOpModulo
CRef RefOpModulo(int, CRef* a, CFrame*)
{
    if (a[0].m_tagk == TAGK_S32 && a[1].m_tagk == TAGK_S32)
        return RefS32(a[1].m_n != 0 ? a[0].m_n % a[1].m_n : 0);
    return RefF32(std::fmod(G(a[0]), G(a[1])));
}
// orig: 0x00123A08 RefOpCurrentTime
// orig: 0x00123A08 (current-time: the game clock's t, D_002DDED0; e.g. the RC chopper's bomb cooldowns compare it,
// natively it was always 0 so CROSS never dropped a bomb)
CRef RefOpCurrentTime(int, CRef*, CFrame*) { return RefF32(g_clock.t); }

// ---- Sly 3's new language built-ins
// orig3: 0x1238F0 (sort: (sort pred list): the list's cells in an array, qsort (0x292540) with a comparator 0x123868
// that evaluates (pred 'a 'b) in the caller's frame (the call form built by 0x1236B0) and returns -1 when it is true,
// 1 otherwise; then a new list of the sorted values. An empty list is returned as is. Natively a stable merge sort with
// the same comparator: the order of elements the predicate does not separate may differ from the PS2's qsort.)
CRef RefOpSort(int, CRef* a, CFrame* pframe)
{
    if (a[1].m_tagk == TAGK_None)
        return a[1];
    std::vector<CRef> aref;
    for (CPair* ppair = PpairRest(a[1]); ppair; ppair = ppair->m_ppairNext.get())
        aref.push_back(ppair->m_ref);
    if (aref.empty())
        return CRef(TAGK_None);
    const CRef refPred = a[0];
    std::stable_sort(aref.begin(), aref.end(), [&](const CRef& x, const CRef& y) {
        CRef arg[2] = {x, y};
        return RefApply(refPred, 2, arg, pframe).m_n != 0;
    });
    return RefList(aref);
}
// orig3: 0x124228 (float-max: the float constant 0x31F4D4 = FLT_MAX)
CRef RefOpFloatMax(int, CRef*, CFrame*) { return RefF32(3.40282347e+38f); }
// orig3: 0x125388 (smooth-float: (g gGoal dt smp) -> (list g' dg'), GSmooth 0x11E598 with the SMP of the fourth
// argument)
CRef RefOpSmoothFloat(int, CRef* a, CFrame*)
{
    const cmm::SMP smp{a[3].m_vector.x, a[3].m_vector.y, a[3].m_vector.z};
    float dg = 0.0f;
    const float g = cmm::GSmooth(G(a[0]), G(a[1]), G(a[2]), &smp, &dg);
    return RefList({RefF32(g), RefF32(dg)});
}
// orig3: 0x1254B8 (smooth-vector: (pos posGoal dt smp) -> (list pos' v'), PosSmooth 0x120C80)
CRef RefOpSmoothVector(int, CRef* a, CFrame*)
{
    const cmm::SMP smp{a[3].m_vector.x, a[3].m_vector.y, a[3].m_vector.z};
    glm::vec3 v(0.0f);
    const glm::vec3 pos = cmm::PosSmooth(glm::vec3(a[0].m_vector), glm::vec3(a[1].m_vector), G(a[2]), &smp, &v);
    return RefList({RefVector(pos), RefVector(v)});
}
// orig3: 0x125928 (lerp: (t a b) -> (1 - t) * a + t * b, a float)
CRef RefOpLerp(int, CRef* a, CFrame*)
{
    const float t = G(a[0]);
    return RefF32((1.0f - t) * G(a[1]) + t * G(a[2]));
}
// orig3: 0x125A10 (the float after the clock's current time, 0x36BB9C; current-time 0x1259F0 reads 0x36BB98)
float g_tSplice2 = 0.0f;
CRef RefOpTime2(int, CRef*, CFrame*) { return RefF32(g_tSplice2); }

// ---- objects (through g_splicegame, see spliceobj.h)
CRef RefStub(int bifk)
{
    g_splicediag.cbifStub++;
    g_splicediag.acbifStubCalls[static_cast<std::size_t>(bifk)]++;
    return CRef();
}
CRef RefBasic(BASIC* pbasic)
{
    CRef ref;
    ref.SetBasic(pbasic);
    return ref;
}

// orig: 0x00123EB0 (RefOptionOp: 0x6C add, 0x6D ensure, 0x6E set (object, option, value), 0x6F get)
// (I3: the Sly 3 built-in; BIFKI: the hooks' role code, Sly 2's built-in number: 108 add, 109 ensure, 110 set, 111 get)
template <int I3, int BIFKI>
CRef RefOpObjectOption(int, CRef* a, CFrame*)
{
    if (!g_splicegame.pfnRefOption)
        return RefStub(I3);
    return g_splicegame.pfnRefOption(BIFKI, a[0].m_pbasic, a[1].m_n, BIFKI == 111 ? nullptr : &a[2]);
}

// The scope of a lookup: the rest list's first object when given, else the relative object.
BASIC* PbasicScope(const CRef* a)
{
    CPair* ppair = PpairRest(a[2]);
    return ppair ? ppair->m_ref.m_pbasic : a[1].m_pbasic;
}

// orig: 0x00124150 (find-object: kind 1 relative to an object, else 5) / 0x001244E8 (kind 4)
template <int I3, int BIFKI>
CRef RefOpFindObject(int, CRef* a, CFrame*)
{
    if (!g_splicegame.pfnFindObject)
        return RefStub(I3);
    const int kind = BIFKI == 115 ? 4 : (a[1].m_pbasic ? 1 : 5);
    BASIC* pb = g_splicegame.pfnFindObject(kind, a[0].m_symid, a[1].m_pbasic, PbasicScope(a));
    static const bool s_fPrint = std::getenv("SLY2N_FINDPRINT") != nullptr;  // (debug)
    if (s_fPrint) std::fprintf(stderr, "find-object %d kind %d rel %p -> %p\n", int(a[0].m_symid), kind, (void*)a[1].m_pbasic, (void*)pb);
    return RefBasic(pb);
}

// orig: 0x001242B8 (the objects of a list of names)
CRef RefOpFindObjectsOfNames(int, CRef* a, CFrame*)
{
    if (a[0].m_tagk == TAGK_None)
        return a[0];
    if (!g_splicegame.pfnFindObject)
        return RefStub(120);
    std::vector<CRef> aref;
    const int kind = a[1].m_pbasic ? 1 : 5;
    static const bool s_fPrint = std::getenv("SLY2N_FINDPRINT") != nullptr;  // (debug)
    for (CPair* ppair = PpairRest(a[0]); ppair; ppair = ppair->m_ppairNext.get())
    {
        aref.push_back(RefBasic(g_splicegame.pfnFindObject(kind, ppair->m_ref.m_symid, a[1].m_pbasic, PbasicScope(a))));
        if (s_fPrint) std::fprintf(stderr, "find-objects-of-names: %d (tag %d) kind %d -> %p\n", int(ppair->m_ref.m_symid), int(ppair->m_ref.m_tagk), kind, (void*)aref.back().m_pbasic);
    }
    return RefList(aref);
}

// orig: 0x00124730 (find-objects: up to 0x180 matches, kind 0x401 relative to an object, else 0x405)
CRef RefOpFindObjects(int, CRef* a, CFrame*)
{
    if (!g_splicegame.pfnFindObjectsByClass)
        return RefStub(128);
    BASIC* apbasic[0x180];
    const int c = g_splicegame.pfnFindObjectsByClass(a[1].m_pbasic ? 0x401 : 0x405, a[0].m_n, a[1].m_pbasic, 0x180, apbasic);
    std::vector<CRef> aref;
    for (int i = 0; i < c; i++)
        aref.push_back(RefBasic(apbasic[i]));
    return RefList(aref);
}

// orig: 0x00123AD0 / 0x00123B00 / 0x00123B30 / 0x00124640 / 0x00124690 / 0x001246E0 (a global object)
// (BIFKI: the role code, Sly 2's number: 100..102 the globals 0x3E0F9C.. (Sly 3 0x4794C4..), 117 player, 118 world,
// 119 camera)
template <int I3, int BIFKI>
CRef RefOpGlobalObject(int, CRef*, CFrame*)
{
    if (!g_splicegame.pfnPbasicGlobal)
        return RefStub(I3);
    return RefBasic(g_splicegame.pfnPbasicGlobal(BIFKI));
}

// ---- the Sly 3 game built-ins ported natively (the Sly 3 script port)
// orig3: 0x125A30 world-index: S32 0x25E150() = GAME 0x475480 +0x441C, the index of the world loaded (m_ext 35)
CRef RefOpWorldIndex3(int, CRef*, CFrame*) { return RefS32(g_nFilterCur3); }
// orig3: 0x125B38: S32 0x25E7A8() (a stub returning 0 in the release build)
CRef RefOpZero3(int, CRef*, CFrame*) { return RefS32(0); }
// orig3: 0x125B70: S32 0x36BC20 = the static camera's (0x36BC10) +0x10, the id of the option block the world chose
// (m_ext 13; condset3.h)
CRef RefOpCondition3(int, CRef*, CFrame*) { return RefS32(g_condset3.cond); }
// orig3: 0x125BF0 schedule-callback (delay proc . args): (proc . args) on a timer (PtimerNewBasic 0x13B9E0(SW, 0,
// list, T 0x36BB98 + delay)) called at the game time T + delay; its id (timer +0xC). update.cpp keeps the timers.
CRef RefOpScheduleCallback3(int, CRef* a, CFrame*)
{
    std::vector<CRef> aref;
    for (const CPair* p = PpairRest(a[2]); p; p = p->m_ppairNext.get()) aref.push_back(p->m_ref);
    return RefS32(IdScheduleCallback(G(a[0]), a[1], std::move(aref)));
}
// orig3: 0x125DC0 cancel-callback (id): 0x13BAC8(SW, id)
CRef RefOpCancelCallback3(int, CRef* a, CFrame*)
{
    CancelScheduledCallback(a[0].m_n);
    return RefVoid();
}
// orig3: 0x126708: 0x165B28(SW): the first camera of the list SW +0x3EBC (count +0x3EB8), else nil (the class 0x05
// cameras StartSw makes, in their order)
CRef RefOpFirstCm3(int, CRef*, CFrame*)
{
    if (g_psw3 && !g_psw3->mpcidaplo[0x05].empty()) return RefBasic(g_psw3->mpcidaplo[0x05].front());
    return CRef();
}
// orig3: 0x127458 symbol-value (symbol): its binding in the calling frames (0x129B58(env, symbol, 1)), else the
// built-in 0x8E (tag 0x17)
CRef RefOpSymbolValue3(int, CRef* a, CFrame* pframe)
{
    CRef ref;
    if (pframe && pframe->FFindBinding(a[0].GetSymid(), 1, &ref)) return ref;
    ref.SetBifk(static_cast<BIFK>(0x8E));
    return ref;
}

// ---- the game's built-ins: the installed hook, else a counted stub (Nil)
template <int BIFKI>
CRef RefOpGame(int argc, CRef* argv, CFrame* pframe)
{
    if (PFNBIF pfn = s_mpbifkpfnGame[BIFKI])
        return pfn(argc, argv, pframe);
    g_splicediag.cbifStub++;
    g_splicediag.acbifStubCalls[BIFKI]++;
    return CRef();
}

template <int... I>
constexpr std::array<PFNBIF, sizeof...(I)> AStubs(std::integer_sequence<int, I...>)
{
    return {&RefOpGame<I>...};
}
constexpr auto s_apfnStub = AStubs(std::make_integer_sequence<int, BIFK_Max>{});
}

#define BIFN(fn, c, f, name) {fn, c, f, name}
#define BIFG(i, c, f, name) {s_apfnStub[i], c, f, name}

// orig3: 0x342030 (Sly 2 0x2BBB90; the counts are Sly 3's; game built-ins are hooks, see SetBifGameHook). The comment
// after an entry is the Sly 2 built-in it is (by the asm: identical tag constants / callees / globals in their order).
BIF g_mpbifkbif[BIFK_Max] = {
    // 0..45: Sly 2's 0..45
    BIFN(RefOpAdd, 1, 1, "+"), BIFN(RefOpSub, 1, 1, "-"), BIFN(RefOpMult, 1, 1, "*"), BIFN(RefOpDiv, 1, 1, "/"),
    BIFN(RefOpPrint, 1, 0, "print"), BIFN(RefOpPrintFrame, 0, 1, "print-frame"),
    BIFN(RefOpPrintSidebag, 1, 0, "print-sidebag"), BIFN(RefOpIntEqual, 2, 0, "="), BIFN(RefOpL, 2, 0, "<"),
    BIFN(RefOpLE, 2, 0, "<="), BIFN(RefOpG, 2, 0, ">"), BIFN(RefOpGE, 2, 0, ">="), BIFN(RefOpEqv, 2, 0, "eqv?"),
    BIFN(RefOpEqual, 2, 0, "equal?"), BIFN(RefOpIsBoolean, 1, 0, "boolean?"), BIFN(RefOpIsNum, 1, 0, "number?"),
    BIFN(RefOpIsInteger, 1, 0, "integer?"), BIFN(RefOpIsFloat, 1, 0, "float?"), BIFN(RefOpIsSymbol, 1, 0, "symbol?"),
    BIFN(RefOpIsVector, 1, 0, "vector?"), BIFN(RefOpIsMatrix, 1, 0, "matrix?"), BIFN(RefOpIsClq, 1, 0, "clq?"),
    BIFN(RefOpIsLm, 1, 0, "lm?"), BIFN(RefOpIsSmp, 1, 0, "smp?"), BIFN(RefOpIsList, 1, 0, "list?"),
    BIFN(RefOpIsNull, 1, 0, "null?"), BIFN(RefOpIsObject, 1, 0, "object?"), BIFN(RefOpIsNullObj, 1, 0, "null-obj?"),
    BIFN(RefOpIsMethod, 1, 0, "method?"), BIFN(RefOpIsProcedure, 1, 0, "procedure?"),
    BIFN(RefOpAreNear, 3, 0, "near?"), BIFN(RefOpNot, 1, 0, "not"), BIFN(RefOpCons, 2, 0, "cons"),
    BIFN(RefOpCar, 1, 0, "car"), BIFN(RefOpCdr, 1, 0, "cdr"), BIFN(RefOpSetCar, 2, 0, "set-car!"),
    BIFN(RefOpSetCdr, 2, 0, "set-cdr!"), BIFN(RefOpLength, 1, 0, "length"), BIFN(RefOpNth, 2, 0, "nth"),
    BIFN(RefOpIsMember, 2, 0, "member?"), BIFN(RefOpList, 0, 1, "list"), BIFN(RefOpShuffle, 1, 0, "shuffle"),
    BIFN(RefOpAppend, 0, 1, "append"), BIFN(RefOpMap, 2, 0, "map"), BIFN(RefOpFilter, 2, 0, "filter"),
    BIFN(RefOpForEach, 2, 0, "for-each"),
    BIFN(RefOpSort, 2, 0, "sort"),                                                   // 46 new
    BIFN(RefOpEval, 1, 0, "eval"), BIFN(RefOpVector, 0, 1, "vector"),               // 47.. = Sly 2 46..
    BIFN(RefOpMatrix, 0, 1, "matrix"), BIFN(RefOpClq, 3, 0, "clq"), BIFN(RefOpLm, 2, 0, "lm"),
    BIFN(RefOpSmp, 3, 0, "smp"), BIFN(RefOpGetElement, 1, 1, "get-element"), BIFN(RefOpRandom, 2, 0, "random"),
    BIFN(RefOpFloatMax, 0, 0, "float-max"),                                          // 55 new
    BIFN(RefOpSqrt, 1, 0, "sqrt"), BIFN(RefOpSin, 1, 0, "sin"), BIFN(RefOpCos, 1, 0, "cos"),  // 56.. = Sly 2 54..
    BIFN(RefOpTan, 1, 0, "tan"), BIFN(RefOpAsin, 1, 0, "asin"), BIFN(RefOpAcos, 1, 0, "acos"),
    BIFN(RefOpAtan, 1, 0, "atan"), BIFN(RefOpRadNormalize, 1, 0, "rad-normalize"), BIFN(RefOpAtan2, 2, 0, "atan2"),
    BIFN(RefOpVectorDot, 2, 0, "vector-dot-product"), BIFN(RefOpVectorCross, 2, 0, "vector-cross-product"),
    BIFN(RefOpVectorLth, 1, 0, "vector-lth"), BIFN(RefOpVectorDistance, 1, 1, "vector-distance"),
    BIFN(RefOpVectorDistanceSquared, 1, 1, "vector-distance-squared"),
    BIFN(RefOpVectorNormalize, 1, 1, "vector-normalize"), BIFN(RefOpVectorProjectNormal, 2, 0, "vector-project-normal"),
    BIFN(RefOpVectorProjectTangent, 2, 0, "vector-project-tangent"),
    BIFN(RefOpVectorBallisticVelocity, 3, 1, "vector-ballistic-velocity"),
    BIFN(RefOpVectorRadianNormal, 2, 0, "vector-radian-normal"),
    BIFN(RefOpMatrixTranspose, 1, 0, "matrix-transpose"), BIFN(RefOpMatrixInvert, 1, 0, "matrix-invert"),
    BIFN(RefOpMatrixCalculateDmat, 2, 0, "matrix-calculate-dmat"),
    BIFN(RefOpMatrixInterpolateRotate, 3, 0, "matrix-interpolate-rotate"),
    BIFN(RefOpMatrixDecomposeToTranslate, 1, 0, "matrix-decompose-to-translate"),
    BIFN(RefOpMatrixDecomposeToRotate, 1, 0, "matrix-decompose-to-rotate"),
    BIFN(RefOpMatrixDecomposeToEuler, 1, 0, "matrix-decompose-to-euler"),
    BIFN(RefOpMatrixDecomposeToRadianNormal, 1, 0, "matrix-decompose-to-radian-normal"),
    BIFN(RefOpMatrixLookAt, 2, 1, "matrix-look-at"), BIFN(RefOpMatrixTiltUpright, 1, 0, "matrix-tilt-upright"),
    BIFN(RefOpClqEvaluate, 2, 0, "clq-evaluate"), BIFN(RefOpClqEvaluateLm, 3, 0, "clq-evaluate-lm"),
    BIFN(RefOpClqFit, 4, 0, "clq-fit"), BIFN(RefOpLmLimit, 2, 0, "lm-limit"), BIFN(RefOpLmCheck, 2, 0, "lm-check?"),
    BIFN(RefOpSmoothFloat, 4, 0, "smooth-float"), BIFN(RefOpSmoothVector, 4, 0, "smooth-vector"),  // 90, 91 new
    BIFN(RefOpFloor, 1, 0, "floor"), BIFN(RefOpCeiling, 1, 0, "ceiling"), BIFN(RefOpRound, 1, 0, "round"),  // 92.. = Sly 2 88..
    BIFN(RefOpTruncate, 1, 0, "truncate"), BIFN(RefOpAbs, 1, 0, "abs"), BIFN(RefOpMaximum, 1, 1, "max"),
    BIFN(RefOpMinimum, 1, 1, "min"), BIFN(RefOpModulo, 2, 0, "modulo"),
    BIFN(RefOpLerp, 3, 0, "lerp"),                                                   // 100 new
    BIFN(RefOpCurrentTime, 0, 0, "current-time"),                                    // 101 = Sly 2 96
    BIFN(RefOpTime2, 0, 0, "clock-time-2"),                                          // 102 new (0x36BB9C)
    // 103..: the game's (names from their code where known)
    BIFN(RefOpWorldIndex3, 0, 0, "world-index"),  // orig3 0x125A30: S32 0x25E150() = GAME 0x475480 +0x441C   // Sly 2 97
    BIFN((RefOpGlobalObject<104, 100>), 0, 0, "object-4794C4"),                      // Sly 2 100..102
    BIFN((RefOpGlobalObject<105, 101>), 0, 0, "object-4794C8"),
    BIFN((RefOpGlobalObject<106, 102>), 0, 0, "object-4794CC"),
    BIFG(107, 0, 0, "bif-107 (0x171C98)"), BIFN(RefOpZero3, 0, 0, "bif-108 (0x25E7A8: 0)"),    // Sly 2 103, 104
    BIFN(RefOpCondition3, 0, 0, "world-condition"), BIFG(110, 2, 0, "bif-110 (0x284828)"),    // new
    // 111 orig3 0x125BF0 (delay, proc, args...): (proc . args) on a timer PtimerNewBasic(SW, 0, list, T 0x36BB98 +
    // delay) 0x13B9E0, returns the timer's id (+0xC, S32); 112 orig3 0x125DC0 (id): 0x13BAC8(SW, id), returns void
    BIFN(RefOpScheduleCallback3, 2, 1, "schedule-callback"), BIFN(RefOpCancelCallback3, 1, 0, "cancel-callback"),        // Sly 2 105, 106
    BIFG(113, 2, 0, "defer-object-update"),                                          // Sly 2 107
    BIFN((RefOpObjectOption<114, 108>), 3, 0, "addo"), BIFN((RefOpObjectOption<115, 109>), 3, 0, "ensureo"),  // Sly 2 108..
    BIFN((RefOpObjectOption<116, 110>), 3, 0, "seto!"), BIFN((RefOpObjectOption<117, 111>), 2, 0, "geto"),
    BIFN((RefOpFindObject<118, 112>), 2, 1, "find-object"), BIFG(119, 2, 1, "find-object-119 (0x1261D0)"),
    BIFN(RefOpFindObjectsOfNames, 2, 1, "find-objects-of-names"),
    BIFN((RefOpFindObject<121, 115>), 2, 1, "find-object-kind-4"), BIFG(122, 2, 1, "find-nearest-objects"),
    BIFN((RefOpGlobalObject<123, 117>), 0, 0, "find-player-object"),
    BIFN((RefOpGlobalObject<124, 118>), 0, 0, "find-world-object"),
    BIFN((RefOpGlobalObject<125, 119>), 0, 0, "find-camera-object"),                 // .. Sly 2 119
    // 127 orig3 0x126708: 0x165B28(SW): SW +0x3EB8 (a count) > 0 ? the first of the list SW +0x3EBC : nil
    BIFG(126, 0, 0, "object-478D24"), BIFN(RefOpFirstCm3, 0, 0, "sw-first-3EBC"),              // new
    BIFN(RefOpFindObjects, 2, 0, "find-objects"),                                    // 128.. = Sly 2 120..
    BIFG(129, 2, 0, "find-objects-in-bounding-box"), BIFG(130, 2, 0, "find-objects-in-bounding-sphere"),
    BIFG(131, 3, 0, "hit-test-objects"), BIFG(132, 3, 0, "hit-test-objects-first"),
    BIFG(133, 3, 0, "convert-object-position"), BIFG(134, 3, 0, "convert-object-vector"),
    BIFG(135, 3, 0, "convert-object-matrix"), BIFG(136, 0, 0, "near-clip-center"),
    BIFG(137, 2, 0, "bif-137 (positioned object)"), BIFG(138, 2, 1, "bif-138"), BIFG(139, 1, 0, "bif-139"),
    // 141 orig3 0x127458 (symbol): the symbol's value in the calling frames (0x129B58(frame env, symbol, 1), up the
    // parents +0x8 while +0x14), else the symbol 0x8E; 145 orig3 0x1275B0: the global object 0x46F770
    BIFG(140, 2, 0, "set-progress-field"), BIFN(RefOpSymbolValue3, 1, 0, "symbol-value"), BIFG(142, 0, 1, "bif-142"),
    BIFG(143, 0, 1, "bif-143"), BIFG(144, 0, 1, "bif-144 (flags)"), BIFG(145, 0, 0, "object-46F770"),
    BIFG(146, 1, 0, "bif-146"), BIFG(147, 2, 0, "bif-147"),                          // .. Sly 2 139
    BIFG(148, 1, 0, "add-coins"),                                                    // new (0x1852E0 / 0x185328)
    // 149..177 = Sly 2 140..168: return Nil in the original (some counts differ from Sly 2's)
    BIFG(149, 0, 0, "bif-149"), BIFG(150, 0, 0, "bif-150"), BIFG(151, 0, 0, "bif-151"), BIFG(152, 0, 0, "bif-152"),
    BIFG(153, 0, 0, "bif-153"), BIFG(154, 0, 0, "bif-154"), BIFG(155, 0, 0, "bif-155"), BIFG(156, 0, 0, "bif-156"),
    BIFG(157, 0, 0, "bif-157"), BIFG(158, 0, 0, "bif-158"), BIFG(159, 0, 0, "bif-159"), BIFG(160, 2, 0, "bif-160"),
    BIFG(161, 2, 0, "bif-161"), BIFG(162, 2, 0, "bif-162"), BIFG(163, 0, 0, "bif-163"), BIFG(164, 0, 0, "bif-164"),
    BIFG(165, 3, 0, "bif-165"), BIFG(166, 1, 0, "bif-166"), BIFG(167, 0, 0, "bif-167"), BIFG(168, 1, 0, "bif-168"),
    BIFG(169, 0, 0, "bif-169"), BIFG(170, 0, 0, "bif-170"), BIFG(171, 4, 0, "bif-171"), BIFG(172, 3, 0, "bif-172"),
    BIFG(173, 1, 0, "bif-173"), BIFG(174, 0, 0, "bif-174"), BIFG(175, 2, 0, "bif-175"), BIFG(176, 0, 0, "bif-176"),
    BIFG(177, 0, 0, "bif-177"),
};

static_assert(sizeof(g_mpbifkbif) / sizeof(g_mpbifkbif[0]) == BIFK_Max);

// The Sly 3 built-in that is Sly 2's built-in bif2 (the alignment of the table above), BIFK_Nil for Sly 2's 98 / 99
// (no Sly 3 counterpart) and out-of-range numbers.
BIFK Bifk3FromSly2(int bif2)
{
    if (bif2 < 0 || bif2 > 168) return BIFK_Nil;
    if (bif2 <= 45) return static_cast<BIFK>(bif2);
    if (bif2 <= 53) return static_cast<BIFK>(bif2 + 1);   // eval .. random
    if (bif2 <= 87) return static_cast<BIFK>(bif2 + 2);   // sqrt .. lm-check?
    if (bif2 <= 95) return static_cast<BIFK>(bif2 + 4);   // floor .. modulo
    if (bif2 == 96) return BIFK_CurrentTime;
    if (bif2 == 97) return static_cast<BIFK>(103);
    if (bif2 == 98 || bif2 == 99) return BIFK_Nil;
    if (bif2 <= 102) return static_cast<BIFK>(bif2 + 4);  // the global objects
    if (bif2 <= 104) return static_cast<BIFK>(bif2 + 4);  // 107, 108
    if (bif2 <= 119) return static_cast<BIFK>(bif2 + 6);  // schedule-callback .. find-camera-object
    if (bif2 <= 139) return static_cast<BIFK>(bif2 + 8);  // find-objects .. 147
    return static_cast<BIFK>(bif2 + 9);                    // the Nil stubs
}

void SetBifGameHook(BIFK bifk, PFNBIF pfnbif)
{
    if (bifk >= 0 && bifk < BIFK_Max)
        s_mpbifkpfnGame[bifk] = pfnbif;
}

bool FBifNative(BIFK bifk)
{
    return bifk >= 0 && bifk < BIFK_Max && g_mpbifkbif[bifk].pfnbif != s_apfnStub[bifk];
}

bool FBifImplemented(BIFK bifk)
{
    if (bifk < 0 || bifk >= BIFK_Max)
        return false;
    return g_mpbifkbif[bifk].pfnbif != s_apfnStub[bifk] || s_mpbifkpfnGame[bifk] != nullptr;
}

SPLICEGAME g_splicegame;

std::vector<CRef> ArefFlatten(BIFK bifk, int argc, CRef* argv)
{
    std::vector<CRef> aref;
    const BIF& bif = g_mpbifkbif[bifk];
    for (int i = 0; i < bif.crefReq && i < argc; i++)
        aref.push_back(argv[i]);
    if (bif.fVarArg)
        for (CPair* ppair = PpairRest(argv[bif.crefReq]); ppair; ppair = ppair->m_ppairNext.get())
            aref.push_back(ppair->m_ref);
    return aref;
}
