#pragma once

// CRef: the splice (Scheme) runtime's tagged value (Sly1-Release ref.h, Sly 2's numbering).
//
// The original's CRef is 8 bytes {tagk, payload}; vectors, matrices, CLQ/LM/SMP live in refcounted pool nodes,
// lists (NODE = CPair), lambdas (O = CProc), native methods (CMethod) are refcounted objects. Here: scalars
// inline, small float tuples inline (value semantics: the original never mutates them in place), matrices,
// pairs, procs and methods behind shared_ptr (the original's refcounts).

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>

#include "splicetypes.h"

class CPair;
class CProc;
class CFrame;
class CRef;

using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using f32 = float;

// A native method bound to an object (tag 0x22): pfn(pbasic, argc, argv).
using PFNTHUNK = CRef (*)(BASIC* pbasic, int argc, CRef* argv);

// Sly 2's tag kinds. Sly 1 numbering differs from 6 on (Sly 2 adds 6, has the dot at 20, the bool at 23).
// Evidence: Eval 0x127838 (forms 1..18), the list loader 0x12A3D8 (21..33), CRef's ctors 0x129518..0x129A70,
// eqv? 0x120548 (< 37; 0..20 and 36 are "same tag = eqv"), EvalLambda 0x127068 (rest marker 20).
enum TAGK
{
    TAGK_Nil = -1,              // uninitialised / no value (the default CRef)
    TAGK_None = 0,              // the empty list ()
    TAGK_Set = 1,               // (set! sym expr)
    TAGK_Define = 2,            // (define sym expr)
    TAGK_DefineMacro = 3,
    TAGK_DefineMacroLambda = 4,
    TAGK_Assert = 5,            // (assert expr ...): 0x1265F0
    TAGK_Debug = 6,             // new in Sly 2: (form expr) evaluates its argument if any, else void (0x1266C8)
    TAGK_If = 7,
    TAGK_Or = 8,
    TAGK_And = 9,
    TAGK_Cond = 10,
    TAGK_Else = 11,
    TAGK_Case = 12,
    TAGK_Let = 13,
    TAGK_While = 14,
    TAGK_Lambda = 15,
    TAGK_Begin = 16,
    TAGK_Import = 17,           // evaluates to Nil (the compiler resolved imports)
    TAGK_Quote = 18,
    TAGK_Pipe = 19,             // (not serialized; name from Sly 1's order, unconfirmed)
    TAGK_Dot = 20,              // the rest-parameter marker in a lambda's formals
    TAGK_S32 = 21,
    TAGK_F32 = 22,
    TAGK_Bool = 23,
    TAGK_Symid = 24,
    TAGK_Bifk = 25,
    TAGK_Vector = 26,
    TAGK_Matrix = 27,
    TAGK_Clq = 28,
    TAGK_Lm = 29,
    TAGK_Smp = 30,
    TAGK_Pair = 31,             // a list (the original's NODE)
    TAGK_Proc = 32,             // a lambda closure (the original's "O")
    TAGK_Basic = 33,            // a game object
    TAGK_Method = 34,           // a native method bound to an object
    TAGK_Err = 35,              // (unconfirmed)
    TAGK_Void = 36,
    TAGK_Max = 37
};

// Sly 2's built-in functions: the table 0x2BBB90 {pfn, crefReq, fVarArg} x 169. 0..96 are Sly 1's (same
// order, Sly 2 inserts 41 and drops random-seed); the rest are named from their code (see bif.cpp).
enum BIFK
{
    BIFK_Nil = -1,
    BIFK_Add = 0, BIFK_Subtract, BIFK_Multiply, BIFK_Divide, BIFK_Print, BIFK_PrintFrame, BIFK_PrintSidebag,
    BIFK_IntEqual, BIFK_Less, BIFK_LE, BIFK_Greater, BIFK_GE, BIFK_Eqv, BIFK_Equal,
    BIFK_IsBoolean = 14, BIFK_IsNumber, BIFK_IsInteger, BIFK_IsFloat, BIFK_IsSymbol, BIFK_IsVector, BIFK_IsMatrix,
    BIFK_IsClq, BIFK_IsLm, BIFK_IsSmp, BIFK_IsList, BIFK_IsNull, BIFK_IsObject, BIFK_IsNullObj, BIFK_IsMethod,
    BIFK_IsProcedure, BIFK_AreNear = 30, BIFK_Not, BIFK_Cons, BIFK_Car, BIFK_Cdr, BIFK_SetCar, BIFK_SetCdr,
    BIFK_Length, BIFK_Nth, BIFK_IsMember, BIFK_List = 40, BIFK_Shuffle = 41, BIFK_Append = 42, BIFK_Map,
    BIFK_Filter, BIFK_ForEach, BIFK_Eval, BIFK_Vector = 47, BIFK_Matrix, BIFK_Clq, BIFK_Lm, BIFK_Smp,
    BIFK_GetElement = 52, BIFK_Random = 53, BIFK_Sqrt = 54, BIFK_Sin, BIFK_Cos, BIFK_Tan, BIFK_Asin, BIFK_Acos,
    BIFK_Atan, BIFK_RadNormalize, BIFK_Atan2 = 62, BIFK_VectorDotProduct, BIFK_VectorCrossProduct, BIFK_VectorLth,
    BIFK_VectorDistance, BIFK_VectorDistanceSquared, BIFK_VectorNormalize, BIFK_VectorProjectNormal = 69,
    BIFK_VectorProjectTangent, BIFK_VectorBallisticVelocity, BIFK_VectorRadianNormal, BIFK_MatrixTranspose = 73,
    BIFK_MatrixInvert, BIFK_MatrixCalculateDmat, BIFK_MatrixInterpolateRotate, BIFK_MatrixDecomposeToTranslate = 77,
    BIFK_MatrixDecomposeToRotate, BIFK_MatrixDecomposeToEuler, BIFK_MatrixDecomposeToRadianNormal,
    BIFK_MatrixLookAt = 81, BIFK_MatrixTiltUpright, BIFK_ClqEvaluate, BIFK_ClqEvaluateLm, BIFK_ClqFit,
    BIFK_LmLimit = 86, BIFK_LmCheck, BIFK_Floor, BIFK_Ceiling, BIFK_Round, BIFK_Truncate, BIFK_Abs = 92,
    BIFK_Maximum, BIFK_Minimum, BIFK_Modulo, BIFK_CurrentTime = 96,
    // 97..: Sly 2's game built-ins (most need game objects; bif.cpp says which are stubbed).
    BIFK_Max = 169
};

struct CMethod
{
    BASIC* m_pbasic = nullptr;
    PFNTHUNK m_pfnthunk = nullptr;
    int m_crefReq = 0;
};

class CRef
{
public:
    TAGK m_tagk = TAGK_Nil;

    union
    {
        s32 m_n = 0;
        f32 m_g;
        int m_bool;
        SYMID m_symid;
        BIFK m_bifk;
    };
    BASIC* m_pbasic = nullptr;

    // Vector (xyz), Clq (g0 g1 g2), Lm (gMin gMax), Smp (svFast svSlow dtFast): the floats in their order.
    glm::vec4 m_vector{0.0f};

    std::shared_ptr<glm::mat4> m_pmatrix;
    std::shared_ptr<CPair> m_ppair;
    std::shared_ptr<CProc> m_pproc;
    std::shared_ptr<CMethod> m_pmethod;
    // Temporary script objects share the lifetime of their CRefs.
    std::shared_ptr<BASIC> m_pbasicOwner;

    CRef() = default;
    explicit CRef(TAGK tagk) { SetTag(tagk); }

    // InitTag 0x12A0B0 (kinds with a payload start zeroed)
    void SetTag(TAGK tagk);
    void SetS32(s32 n);
    void SetF32(f32 g);
    void SetBool(int fBool);
    void SetSymid(SYMID symid);
    void SetBifk(BIFK bifk);
    void SetPair(std::shared_ptr<CPair> ppair);
    void SetProc(std::shared_ptr<CProc> pproc);
    void SetVector(const glm::vec3& vector);
    void SetMatrix(const glm::mat4& matrix);
    void SetClq(const CLQ& clq);
    void SetLm(const LM& lm);
    void SetSmp(const SMP& smp);
    void SetBasic(BASIC* pbasic);
    void SetOwnedBasic(std::shared_ptr<BASIC> pbasic);
    void SetMethod(std::shared_ptr<CMethod> pmethod);

    [[nodiscard]] SYMID GetSymid() const { return m_symid; }
    [[nodiscard]] glm::vec3 GetVector() const { return glm::vec3(m_vector); }
    [[nodiscard]] const glm::mat4& GetMatrix() const;
    [[nodiscard]] CLQ GetClq() const { return {m_vector.x, m_vector.y, m_vector.z, 0.0f}; }
    [[nodiscard]] LM GetLm() const { return {m_vector.x, m_vector.y}; }
    [[nodiscard]] SMP GetSmp() const { return {m_vector.x, m_vector.y, m_vector.z}; }
    [[nodiscard]] bool FTrue() const { return m_tagk == TAGK_Bool && m_bool != 0; }
    [[nodiscard]] bool FFalse() const { return m_tagk == TAGK_Bool && m_bool == 0; }
    [[nodiscard]] bool FNumber() const { return m_tagk == TAGK_S32 || m_tagk == TAGK_F32; }

    // eqv? 0x120548: same tag, then the payload by tag.
    bool operator==(const CRef& other) const;
    // ToInt 0x129D58 / ToFloat 0x129DA8
    [[nodiscard]] CRef RefCoerceS32() const;
    [[nodiscard]] CRef RefCoerceF32() const;
    [[nodiscard]] float GAsFloat() const { return m_tagk == TAGK_F32 ? m_g : static_cast<float>(m_n); }

    // Clone 0x129BC8: a copy for a new owner (lists and lambdas are copied, the rest shared).
    void CloneTo(CRef* prefClone, CFrame* pframeClone) const;
};

const char* PchzFromTagk(int tagk);
