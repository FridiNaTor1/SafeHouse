#pragma once

// The splice evaluator (0x126438-0x127FE0): Eval's dispatch on a cell's form, the special forms, application
// of built-ins (bif.cpp), native methods and lambdas.

#include <string>
#include <vector>

#include "frame.h"
#include "pair.h"

using PFNBIF = CRef (*)(int argc, CRef* argv, CFrame* pframe);

// A built-in (0x2BBB90 + 0xC * bifk {pfn, crefReq, fVarArg}); achz is Sly 1's name where known.
struct BIF
{
    PFNBIF pfnbif;
    int crefReq;
    int fVarArg;
    const char* achz;
};

extern BIF g_mpbifkbif[BIFK_Max];

CRef RefEvalSymbol(CPair* ppair, CFrame* pframe);       // 0x126438
CRef RefEvalSet(CPair* ppair, CFrame* pframe);          // 0x126490
CRef RefEvalDefine(CPair* ppair, CFrame* pframe);       // 0x126540
CRef RefEvalAssert(CPair* ppair, CFrame* pframe);       // 0x1265F0
CRef RefEvalDebug(CPair* ppair, CFrame* pframe);        // 0x1266C8
CRef RefEvalIf(CPair* ppair, CFrame* pframe);           // 0x126758
CRef RefEvalOr(CPair* ppair, CFrame* pframe);           // 0x126840
CRef RefEvalAnd(CPair* ppair, CFrame* pframe);          // 0x126928
CRef RefEvalCond(CPair* ppair, CFrame* pframe);         // 0x126A18
CRef RefEvalCase(CPair* ppair, CFrame* pframe);         // 0x126BA0
CRef RefEvalLet(CPair* ppair, CFrame* pframe);          // 0x126DB8
CRef RefEvalWhile(CPair* ppair, CFrame* pframe);        // 0x126F78
CRef RefEvalLambda(CPair* ppair, CFrame* pframe);       // 0x127068
CRef RefEvalLambdaBody(CProc* pproc, CFrame* pframe);   // 0x127190
CRef RefEvalBegin(CPair* ppair, CFrame* pframe);        // 0x127228
CRef RefEvalApply(CPair* ppair, CFrame* pframe);        // 0x1272E0
CRef RefEval(CPair* ppair, CFrame* pframe);             // 0x127838

// Applies a procedure value (built-in, method or lambda) to evaluated arguments (the second half of
// RefEvalApply; used by map / for-each / filter and by the game's callbacks).
CRef RefApply(const CRef& refProc, int cref, CRef* aref, CFrame* pframe);

// Evaluation problems (natively reported instead of the original's undefined behaviour): unbound symbols,
// applications of non-procedures, missing arguments, built-ins without a native implementation.
struct SPLICEDIAG
{
    long csymUnbound = 0;
    long capplyBad = 0;
    long cargMissing = 0;
    long cbifStub = 0;
    std::vector<long> acbifStubCalls = std::vector<long>(BIFK_Max, 0);
    std::vector<std::string> amsg;  // the first ones
    void Note(const std::string& msg);
};
extern SPLICEDIAG g_splicediag;
