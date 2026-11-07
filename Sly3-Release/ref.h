#pragma once

#include "game/sly3.h"

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

// Sly 3's tag kinds, each by its Sly 2 number through game/gen/tagmap.inc (S3TAG; renumbered from Sly 2's: DefineMacroLambda (Sly 2 4) and Import (Sly 2 17) are gone, so the forms
// 4..15 are Sly 2's 5..16, quote / pipe / dot are 16..18 and the payload kinds 19..31 are Sly 2's 21..33).
// Evidence (orig3): RefEval 0x1296F0 (symbol 22, list 29; the forms' jump table 0x31F4E0 for tags 1..16: 1 set!
// 0x128428 (RefSetBinding), 2 define 0x1284D8 (RefAddBinding), 3 / 10 / >= 17 apply, 4 assert 0x128588 (evaluates,
// void), 5 debug 0x128610, 6 if 0x1286A0, 7 or 0x128788, 8 and 0x128860, 9 cond, 11 case, 12 let, 13 while, 14 lambda
// 0x128F90, 15 begin 0x129148, 16 quote (the next cell)), PpairSerializeIn 0x12C108 (dispatch 0x31F610: 3, 17, 30
// invalid), RefEvalApply 0x129208 (procedures: builtin 23, closure 30, method 32; the rest marker 18), the sort
// built-in 0x1236B0 (quote 16), InitTag(0x22) for void (34), the predicates 0x122488.. (19..32).
enum TAGK
{
    TAGK_Nil = -1,              // uninitialised / no value (the default CRef)
    TAGK_None = S3TAG(0),              // the empty list ()
    TAGK_Set = S3TAG(1),               // (set! sym expr)
    TAGK_Define = S3TAG(2),            // (define sym expr)
    TAGK_DefineMacro = S3TAG(3),       // invalid in the deserializer; applied by RefEval
    TAGK_Assert = S3TAG(5),            // (assert expr): orig3 0x128588 evaluates it, returns void (no message: Sly 2's printed)
    TAGK_Debug = S3TAG(6),             // (form expr) evaluates its argument if any, else void (0x128610)
    TAGK_If = S3TAG(7),
    TAGK_Or = S3TAG(8),
    TAGK_And = S3TAG(9),
    TAGK_Cond = S3TAG(10),
    TAGK_Else = S3TAG(11),
    TAGK_Case = S3TAG(12),
    TAGK_Let = S3TAG(13),
    TAGK_While = S3TAG(14),
    TAGK_Lambda = S3TAG(15),
    TAGK_Begin = S3TAG(16),
    TAGK_Quote = S3TAG(18),
    TAGK_Pipe = S3TAG(19),             // (invalid in the deserializer; name from Sly 1's order, unconfirmed)
    TAGK_Dot = S3TAG(20),              // the rest-parameter marker in a lambda's formals
    TAGK_S32 = S3TAG(21),
    TAGK_F32 = S3TAG(22),
    TAGK_Bool = S3TAG(23),
    TAGK_Symid = S3TAG(24),
    TAGK_Bifk = S3TAG(25),
    TAGK_Vector = S3TAG(26),
    TAGK_Matrix = S3TAG(27),
    TAGK_Clq = S3TAG(28),
    TAGK_Lm = S3TAG(29),
    TAGK_Smp = S3TAG(30),
    TAGK_Pair = S3TAG(31),             // a list (the original's NODE)
    TAGK_Proc = S3TAG(32),             // a lambda closure (the original's "O")
    TAGK_Basic = S3TAG(33),            // a game object
    TAGK_Method = S3TAG(34),           // a native method bound to an object
    TAGK_Err = S3TAG(35),              // (unconfirmed)
    TAGK_Void = S3TAG(36),
    TAGK_Max = S3TAG(37)
};

// Sly 3's built-in functions: the table 0x342030 {pfn, crefReq, fVarArg} x 178 (Sly 2: 0x2BBB90 x 169). 0..45 are
// Sly 2's 0..45 (checked against the asm: the predicates' tag constants, set-car! / set-cdr! offsets, the void-returning
// print stubs); then Sly 3 inserts sort (46), float-max (55), smooth-float / smooth-vector (90, 91), lerp (100), a
// second clock value (102), 109, 110, 126, 127, add-coins (148), and drops Sly 2's 98 / 99 (world-record built-ins).
// bif.cpp's table says which Sly 2 built-in each one is (Bifk3FromSly2 maps the verified ones).
enum BIFK
{
    BIFK_Nil = -1,
    BIFK_Add = 0, BIFK_Subtract, BIFK_Multiply, BIFK_Divide, BIFK_Print, BIFK_PrintFrame, BIFK_PrintSidebag,
    BIFK_IntEqual, BIFK_Less, BIFK_LE, BIFK_Greater, BIFK_GE, BIFK_Eqv, BIFK_Equal,
    BIFK_IsBoolean = 14, BIFK_IsNumber, BIFK_IsInteger, BIFK_IsFloat, BIFK_IsSymbol, BIFK_IsVector, BIFK_IsMatrix,
    BIFK_IsClq, BIFK_IsLm, BIFK_IsSmp, BIFK_IsList, BIFK_IsNull, BIFK_IsObject, BIFK_IsNullObj, BIFK_IsMethod,
    BIFK_IsProcedure, BIFK_AreNear = 30, BIFK_Not, BIFK_Cons, BIFK_Car, BIFK_Cdr, BIFK_SetCar, BIFK_SetCdr,
    BIFK_Length, BIFK_Nth, BIFK_IsMember, BIFK_List = 40, BIFK_Shuffle = 41, BIFK_Append = 42, BIFK_Map,
    BIFK_Filter, BIFK_ForEach, BIFK_Sort = 46, BIFK_Eval = 47, BIFK_Vector = 48, BIFK_Matrix, BIFK_Clq, BIFK_Lm,
    BIFK_Smp, BIFK_GetElement = 53, BIFK_Random = 54, BIFK_FloatMax = 55, BIFK_Sqrt = 56, BIFK_Sin, BIFK_Cos,
    BIFK_Tan, BIFK_Asin, BIFK_Acos, BIFK_Atan, BIFK_RadNormalize, BIFK_Atan2 = 64, BIFK_VectorDotProduct,
    BIFK_VectorCrossProduct, BIFK_VectorLth, BIFK_VectorDistance, BIFK_VectorDistanceSquared, BIFK_VectorNormalize,
    BIFK_VectorProjectNormal = 71, BIFK_VectorProjectTangent, BIFK_VectorBallisticVelocity, BIFK_VectorRadianNormal,
    BIFK_MatrixTranspose = 75, BIFK_MatrixInvert, BIFK_MatrixCalculateDmat, BIFK_MatrixInterpolateRotate,
    BIFK_MatrixDecomposeToTranslate = 79, BIFK_MatrixDecomposeToRotate, BIFK_MatrixDecomposeToEuler,
    BIFK_MatrixDecomposeToRadianNormal, BIFK_MatrixLookAt = 83, BIFK_MatrixTiltUpright, BIFK_ClqEvaluate,
    BIFK_ClqEvaluateLm, BIFK_ClqFit, BIFK_LmLimit = 88, BIFK_LmCheck, BIFK_SmoothFloat = 90, BIFK_SmoothVector = 91,
    BIFK_Floor = 92, BIFK_Ceiling, BIFK_Round, BIFK_Truncate, BIFK_Abs = 96, BIFK_Maximum, BIFK_Minimum,
    BIFK_Modulo = 99, BIFK_Lerp = 100, BIFK_CurrentTime = 101, BIFK_Time2 = 102,
    // 103..: the game's (bif.cpp names them; most need game objects).
    BIFK_AddCoins = 148,
    BIFK_Max = 178
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
