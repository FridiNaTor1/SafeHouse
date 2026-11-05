#include "ref.h"

#include "frame.h"
#include "pair.h"
#include "proc.h"

namespace
{
const glm::mat4 s_matIdentity{1.0f};
}

// orig: 0x0012A0B0 InitTag__4CRef (the kinds with a payload node start zeroed)
void CRef::SetTag(TAGK tagk)
{
    m_tagk = tagk;
    m_n = 0;
    m_pbasic = nullptr;
    m_vector = glm::vec4(0.0f);
    m_pmatrix.reset();
    m_ppair.reset();
    m_pproc.reset();
    m_pmethod.reset();
    m_pbasicOwner.reset();
    if (tagk == TAGK_Matrix)
        m_pmatrix = std::make_shared<glm::mat4>(0.0f);
}

// orig: 0x00129518 __as__4CRefi
void CRef::SetS32(s32 n)
{
    SetTag(TAGK_S32);
    m_n = n;
}

// orig: 0x00129558 __as__4CReff
void CRef::SetF32(f32 g)
{
    SetTag(TAGK_F32);
    m_g = g;
}

// orig: 0x00129598 __as__4CRefb
void CRef::SetBool(int fBool)
{
    SetTag(TAGK_Bool);
    m_bool = fBool != 0;
}

// orig: 0x001295D8 (tag 0x18)
void CRef::SetSymid(SYMID symid)
{
    SetTag(TAGK_Symid);
    m_symid = symid;
}

// orig: 0x00129618 (tag 0x19)
void CRef::SetBifk(BIFK bifk)
{
    SetTag(TAGK_Bifk);
    m_bifk = bifk;
}

// orig: 0x00129970 (tag 0x1F)
void CRef::SetPair(std::shared_ptr<CPair> ppair)
{
    SetTag(TAGK_Pair);
    m_ppair = std::move(ppair);
}

// orig: 0x001299C0 (tag 0x20)
void CRef::SetProc(std::shared_ptr<CProc> pproc)
{
    SetTag(TAGK_Proc);
    m_pproc = std::move(pproc);
}

// orig: 0x00129718 __as__4CRefG6VECTOR
void CRef::SetVector(const glm::vec3& vector)
{
    SetTag(TAGK_Vector);
    m_vector = glm::vec4(vector, 0.0f);
}

// orig: 0x00129780 __as__4CRefRC7MATRIX4
void CRef::SetMatrix(const glm::mat4& matrix)
{
    SetTag(TAGK_Matrix);
    *m_pmatrix = matrix;
}

// orig: 0x00129808 (tag 0x1C)
void CRef::SetClq(const CLQ& clq)
{
    SetTag(TAGK_Clq);
    m_vector = glm::vec4(clq.g0, clq.g1, clq.g2, 0.0f);
}

// orig: 0x00129878 (tag 0x1D)
void CRef::SetLm(const LM& lm)
{
    SetTag(TAGK_Lm);
    m_vector = glm::vec4(lm.gMin, lm.gMax, 0.0f, 0.0f);
}

// orig: 0x001298F0 (tag 0x1E)
void CRef::SetSmp(const SMP& smp)
{
    SetTag(TAGK_Smp);
    m_vector = glm::vec4(smp.svFast, smp.svSlow, smp.dtFast, 0.0f);
}

// orig: 0x00129A10 __as__4CRefP5BASIC (tag 0x21; the original AddRefs the object)
void CRef::SetBasic(BASIC* pbasic)
{
    SetTag(TAGK_Basic);
    m_pbasic = pbasic;
}

// orig: 0x00129A10: temporary BASIC objects are retained by their CRefs too.
void CRef::SetOwnedBasic(std::shared_ptr<BASIC> pbasic)
{
    SetTag(TAGK_Basic);
    m_pbasicOwner = std::move(pbasic);
    m_pbasic = m_pbasicOwner.get();
}

// orig: 0x00129A70 (tag 0x22)
void CRef::SetMethod(std::shared_ptr<CMethod> pmethod)
{
    SetTag(TAGK_Method);
    m_pmethod = std::move(pmethod);
}

const glm::mat4& CRef::GetMatrix() const
{
    return m_pmatrix ? *m_pmatrix : s_matIdentity;
}

// orig: 0x00120548 (eqv?: the tags, then 0..20 and void are eqv, scalars and objects by their payloads,
// the float kinds by value)
bool CRef::operator==(const CRef& other) const
{
    if (m_tagk != other.m_tagk)
        return false;
    switch (m_tagk)
    {
    case TAGK_S32:
    case TAGK_Bool:
    case TAGK_Symid:
    case TAGK_Bifk:
        return m_n == other.m_n;
    case TAGK_F32:
        return m_g == other.m_g;
    case TAGK_Vector:
    case TAGK_Clq:
    case TAGK_Lm:
    case TAGK_Smp:
        return m_vector == other.m_vector;
    case TAGK_Matrix:
        return m_pmatrix == other.m_pmatrix;
    case TAGK_Pair:
        return m_ppair == other.m_ppair;
    case TAGK_Proc:
        return m_pproc == other.m_pproc;
    case TAGK_Basic:
        return m_pbasic == other.m_pbasic;
    case TAGK_Method:
        return m_pmethod == other.m_pmethod;
    default:
        return m_tagk <= TAGK_Dot || m_tagk == TAGK_Void;
    }
}

// orig: 0x00129D58 (an int copy: float -> int truncates)
CRef CRef::RefCoerceS32() const
{
    if (m_tagk == TAGK_S32)
        return *this;
    CRef ref;
    ref.SetS32(static_cast<s32>(m_g));
    return ref;
}

// orig: 0x00129DA8 (a float copy: int -> float)
CRef CRef::RefCoerceF32() const
{
    if (m_tagk == TAGK_F32)
        return *this;
    CRef ref;
    ref.SetF32(static_cast<f32>(m_n));
    return ref;
}

// orig: 0x00129BC8 (a copy for a new owner: lists are copied cell by cell, lambdas get the new environment,
// matrices a new node; the rest are new references)
void CRef::CloneTo(CRef* prefClone, CFrame* pframeClone) const
{
    switch (m_tagk)
    {
    case TAGK_Matrix:
        prefClone->SetMatrix(GetMatrix());
        break;
    case TAGK_Pair:
        if (m_ppair)
        {
            auto ppair = PpairNew();
            m_ppair->CloneTo(ppair.get(), pframeClone);
            prefClone->SetPair(std::move(ppair));
        }
        else
            prefClone->SetPair(nullptr);
        break;
    case TAGK_Proc:
        if (m_pproc)
        {
            auto pproc = PprocNew();
            m_pproc->CloneTo(pproc.get(), pframeClone);
            prefClone->SetProc(std::move(pproc));
        }
        else
            *prefClone = *this;
        break;
    default:
        *prefClone = *this;
        break;
    }
}

const char* PchzFromTagk(int tagk)
{
    static const char* const s_apchz[TAGK_Max] = {
        "none", "set!", "define", "define-macro", "assert", "form5", "if", "or", "and", "cond",
        "else", "case", "let", "while", "lambda", "begin", "quote", "pipe", "dot", "int", "float", "bool",
        "symbol", "builtin", "vector", "matrix", "clq", "lm", "smp", "list", "lambda-proc", "object", "method", "err",
        "void"};
    if (tagk == TAGK_Nil)
        return "nil";
    if (tagk < 0 || tagk >= TAGK_Max)
        return "?";
    return s_apchz[tagk];
}
