#include "eval.h"

#include <cstdio>
#include <cstdlib>

#include "proc.h"

SPLICEDIAG g_splicediag;

void SPLICEDIAG::Note(const std::string& msg)
{
    if (amsg.size() < 64)
        amsg.push_back(msg);
}

namespace
{
// The form's list: the cell's value (tag 0x1F); its first cell holds the form's kind / the operator.
CPair* PpairForm(CPair* ppair)
{
    return ppair->m_ref.m_ppair.get();
}

// The form's first argument cell.
CPair* PpairArgs(CPair* ppair)
{
    CPair* ppairList = PpairForm(ppair);
    return ppairList != nullptr ? ppairList->m_ppairNext.get() : nullptr;
}

CRef RefVoid()
{
    return CRef(TAGK_Void);
}

CRef RefBool(int f)
{
    CRef ref;
    ref.SetBool(f);
    return ref;
}

void NoteMissing(const char* pchzForm)
{
    g_splicediag.cargMissing++;
    char ach[96];
    std::snprintf(ach, sizeof(ach), "%s: a missing part of the form", pchzForm);
    g_splicediag.Note(ach);
}
}

// orig: 0x00126438 (a symbol: its binding's value, Nil when unbound)
CRef RefEvalSymbol(CPair* ppair, CFrame* pframe)
{
    CRef ref;
    if (!pframe->FFindBinding(ppair->m_ref.m_symid, 1, &ref))
    {
        g_splicediag.csymUnbound++;
        char ach[64];
        std::snprintf(ach, sizeof(ach), "unbound symbol %u", ppair->m_ref.m_symid);
        g_splicediag.Note(ach);
    }
    return ref;
}

// orig: 0x00126490 (set!: the value into the existing binding; void)
CRef RefEvalSet(CPair* ppair, CFrame* pframe)
{
    CPair* ppairSym = PpairArgs(ppair);
    if (ppairSym == nullptr || !ppairSym->m_ppairNext)
    {
        NoteMissing("set!");
        return RefVoid();
    }
    CRef ref = RefEval(ppairSym->m_ppairNext.get(), pframe);
    if (const char* e = std::getenv("SLY2N_SETPRINT"); e && std::atoi(e) == int(ppairSym->m_ref.m_symid))  // (debug)
    {
        CFrame* pfOwner = pframe;  // the frame that holds the binding
        while (pfOwner && !pfOwner->PrefFindBinding(ppairSym->m_ref.m_symid, 0)) pfOwner = pfOwner->m_cpframeParent ? pfOwner->m_apframeParent[0] : nullptr;
        std::fprintf(stderr, "set! s%d in frame %p (held by %p): tag %d basic %p\n", int(ppairSym->m_ref.m_symid), (void*)pframe, (void*)pfOwner, int(ref.m_tagk),
                     ref.m_tagk == TAGK_Basic ? (void*)ref.m_pbasic : nullptr);
    }
    return pframe->RefSetBinding(ppairSym->m_ref.m_symid, &ref);
}

// orig: 0x00126540 (define: a new binding in this frame; void)
CRef RefEvalDefine(CPair* ppair, CFrame* pframe)
{
    CPair* ppairSym = PpairArgs(ppair);
    if (ppairSym == nullptr || !ppairSym->m_ppairNext)
    {
        NoteMissing("define");
        return RefVoid();
    }
    CRef ref = RefEval(ppairSym->m_ppairNext.get(), pframe);
    return pframe->RefAddBinding(ppairSym->m_ref.m_symid, &ref);
}

// orig: 0x001265F0 (assert: when the test's payload is 0, prints D_002A43B0 and the second argument's first
// cell; void)
CRef RefEvalAssert(CPair* ppair, CFrame* pframe)
{
    CPair* ppairTest = PpairArgs(ppair);
    if (ppairTest == nullptr)
    {
        NoteMissing("assert");
        return RefVoid();
    }
    CRef ref = RefEval(ppairTest, pframe);
    if (ref.m_n == 0)
    {
        CPair* ppairMsg = ppairTest->m_ppairNext.get();
        std::fprintf(stderr, "splice: assertion failed");
        if (ppairMsg != nullptr && ppairMsg->m_ref.m_tagk == TAGK_Pair && ppairMsg->m_ref.m_ppair &&
            ppairMsg->m_ref.m_ppair->m_ppairNext)
        {
            CRef refMsg = ppairMsg->m_ref.m_ppair->m_ppairNext->m_ref;
            std::fprintf(stderr, ": ");
            g_mpbifkbif[BIFK_Print].pfnbif(1, &refMsg, pframe);
        }
        std::fprintf(stderr, "\n");
    }
    return RefVoid();
}

// orig: 0x001266C8 (form 6: its argument's value, void without one)
CRef RefEvalDebug(CPair* ppair, CFrame* pframe)
{
    CPair* ppairArg = PpairArgs(ppair);
    if (ppairArg != nullptr)
        return RefEval(ppairArg, pframe);
    return RefVoid();
}

// orig: 0x00126758 (if: the test's payload 1 (true) takes the then branch; void without an else)
CRef RefEvalIf(CPair* ppair, CFrame* pframe)
{
    CPair* ppairCond = PpairArgs(ppair);
    CPair* ppairThen = ppairCond != nullptr ? ppairCond->m_ppairNext.get() : nullptr;
    if (ppairThen == nullptr)
    {
        NoteMissing("if");
        return RefVoid();
    }
    CPair* ppairElse = ppairThen->m_ppairNext.get();
    CRef ref = RefEval(ppairCond, pframe);
    if (ref.m_n == 1)
        return RefEval(ppairThen, pframe);
    if (ppairElse != nullptr)
        return RefEval(ppairElse, pframe);
    return RefVoid();
}

// orig: 0x00126840 (or: the first value that is not #f, else #f)
CRef RefEvalOr(CPair* ppair, CFrame* pframe)
{
    for (CPair* ppairArg = PpairArgs(ppair); ppairArg != nullptr; ppairArg = ppairArg->m_ppairNext.get())
    {
        CRef ref = RefEval(ppairArg, pframe);
        if (!ref.FFalse())
            return ref;
    }
    return RefBool(0);
}

// orig: 0x00126928 (and: #f at the first #f, else the last value (#t without arguments))
CRef RefEvalAnd(CPair* ppair, CFrame* pframe)
{
    CRef ref = RefBool(1);
    for (CPair* ppairArg = PpairArgs(ppair); ppairArg != nullptr; ppairArg = ppairArg->m_ppairNext.get())
    {
        ref = RefEval(ppairArg, pframe);
        if (ref.FFalse())
            return ref;
    }
    return ref;
}

// orig: 0x00126A18 (cond: the first clause whose test is #t (or else): its forms' last value, the test's
// value for a clause without forms; void when none)
CRef RefEvalCond(CPair* ppair, CFrame* pframe)
{
    CRef ref = RefVoid();
    for (CPair* ppairClause = PpairArgs(ppair); ppairClause != nullptr; ppairClause = ppairClause->m_ppairNext.get())
    {
        CPair* ppairTest = ppairClause->m_ref.m_ppair.get();
        if (ppairTest == nullptr)
            continue;
        bool f;
        if (ppairTest->m_ref.m_tagk == TAGK_Else)
            f = true;
        else
        {
            CRef refTest = RefEval(ppairTest, pframe);
            f = refTest.FTrue();
            if (f && !ppairTest->m_ppairNext)
                ref = refTest;
        }
        if (f)
        {
            for (CPair* ppairForm = ppairTest->m_ppairNext.get(); ppairForm != nullptr; ppairForm = ppairForm->m_ppairNext.get())
                ref = RefEval(ppairForm, pframe);
            break;
        }
    }
    return ref;
}

// orig: 0x00126BA0 (case: the key against each clause's data with eqv?, else clauses; the clause's forms'
// last value, void when none)
CRef RefEvalCase(CPair* ppair, CFrame* pframe)
{
    CRef ref = RefVoid();
    CPair* ppairKey = PpairArgs(ppair);
    if (ppairKey == nullptr)
    {
        NoteMissing("case");
        return ref;
    }
    CRef aref[2];
    aref[0] = RefEval(ppairKey, pframe);
    for (CPair* ppairClause = ppairKey->m_ppairNext.get(); ppairClause != nullptr; ppairClause = ppairClause->m_ppairNext.get())
    {
        CPair* ppairData = ppairClause->m_ref.m_ppair.get();
        if (ppairData == nullptr)
            continue;
        bool f = false;
        if (ppairData->m_ref.m_tagk == TAGK_Else)
            f = true;
        else
        {
            for (CPair* ppairDatum = ppairData->m_ref.m_ppair.get(); ppairDatum != nullptr; ppairDatum = ppairDatum->m_ppairNext.get())
            {
                aref[1] = ppairDatum->m_ref;
                if (g_mpbifkbif[BIFK_Eqv].pfnbif(2, aref, pframe).FTrue())
                {
                    f = true;
                    break;
                }
            }
        }
        if (f)
        {
            for (CPair* ppairForm = ppairData->m_ppairNext.get(); ppairForm != nullptr; ppairForm = ppairForm->m_ppairNext.get())
                ref = RefEval(ppairForm, pframe);
            break;
        }
    }
    return ref;
}

// orig: 0x00126DB8 (let: with a binding list, a new frame (its values evaluated in this one) over this one;
// the body's last value, Nil without a body)
CRef RefEvalLet(CPair* ppair, CFrame* pframe)
{
    CPair* ppairBindings = PpairArgs(ppair);
    if (ppairBindings == nullptr)
    {
        NoteMissing("let");
        return CRef();
    }
    std::shared_ptr<CFrame> pframeLet;
    CFrame* pframeBody = pframe;
    if (ppairBindings->m_ref.m_tagk == TAGK_Pair)
    {
        pframeLet = PframeNew();
        for (CPair* ppairBinding = ppairBindings->m_ref.m_ppair.get(); ppairBinding != nullptr; ppairBinding = ppairBinding->m_ppairNext.get())
        {
            CPair* ppairSym = ppairBinding->m_ref.m_ppair.get();
            if (ppairSym == nullptr || !ppairSym->m_ppairNext)
            {
                NoteMissing("let binding");
                continue;
            }
            CRef refVal = RefEval(ppairSym->m_ppairNext.get(), pframe);
            pframeLet->RefAddBinding(ppairSym->m_ref.m_symid, &refVal);
        }
        pframeLet->SetSingleParent(pframe);
        pframeBody = pframeLet.get();
    }
    CRef ref;
    for (CPair* ppairForm = ppairBindings->m_ppairNext.get(); ppairForm != nullptr; ppairForm = ppairForm->m_ppairNext.get())
        ref = RefEval(ppairForm, pframeBody);
    return ref;
}

void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)
// orig: 0x00126F78 (while: the body form while the test is not #f; void)
CRef RefEvalWhile(CPair* ppair, CFrame* pframe)
{
    CPair* ppairCond = PpairArgs(ppair);
    CPair* ppairBody = ppairCond != nullptr ? ppairCond->m_ppairNext.get() : nullptr;
    if (ppairBody == nullptr)
    {
        NoteMissing("while");
        return RefVoid();
    }
    // (natively a loop that never ends is cut after 1,000,000 rounds with a note; SLY2N_WHILEGUARD prints the loop.
    // 11:1985..1990's ballroom director (job 1981's frame symbols 3663 / 3664) spun here while option 30 (0x1008D8)
    // was not native: a missing method answers void, which the loop's tests took as true)
    for (long c = 0;; ++c)
    {
        if (RefEval(ppairCond, pframe).FFalse())
            break;
        RefEval(ppairBody, pframe);
        if (c == 1000000)
        {
            CRef r;
            r.SetPair(std::shared_ptr<CPair>(ppair, [](CPair*) {}));
            std::string str;
            PrintRef(r, &str);
            std::fprintf(stderr, "splice: a while loop cut after 1000000 rounds%s%s\n", std::getenv("SLY2N_WHILEGUARD") ? ": " : "",
                         std::getenv("SLY2N_WHILEGUARD") ? str.substr(0, 4000).c_str() : "");
            break;
        }
    }
    return RefVoid();
}

// orig: 0x00127068 (lambda: a closure over this frame, its formals (the count before the Dot, whether a rest
// parameter follows) and body)
CRef RefEvalLambda(CPair* ppair, CFrame* pframe)
{
    CPair* ppairFormals = PpairArgs(ppair);
    if (ppairFormals == nullptr)
    {
        NoteMissing("lambda");
        return RefVoid();
    }
    auto pproc = PprocNew();
    pproc->m_pframe = pframe->shared_from_this();
    if (ppairFormals->m_ref.m_tagk != TAGK_None)
        pproc->m_ppairFormals = ppairFormals->m_ref.m_ppair;
    pproc->m_ppairCodeExpr = ppairFormals->m_ppairNext;
    int c = 0, f = 0;
    for (CPair* ppairFormal = pproc->m_ppairFormals.get(); ppairFormal != nullptr; ppairFormal = ppairFormal->m_ppairNext.get())
    {
        if (ppairFormal->m_ref.m_tagk == TAGK_Dot)
        {
            f = 1;
            break;
        }
        c++;
    }
    pproc->m_crefReq = c;
    pproc->m_fVarArg = f;
    CRef ref;
    ref.SetProc(std::move(pproc));
    return ref;
}

// orig: 0x00127190 (a lambda's body in its frame: the last value)
void PrintRef(const CRef& ref, std::string* ps);
CRef RefEvalLambdaBody(CProc* pproc, CFrame* pframe)
{
    static int s_cDepth = 0;
    struct DEPTH { DEPTH() { ++s_cDepth; } ~DEPTH() { --s_cDepth; } } depth;
    static const bool s_fDeep = std::getenv("SLY2N_DEEPLAMBDA") != nullptr;
    if (s_fDeep && s_cDepth == 400)
    {
        std::string str;
        CRef body;
        body.SetPair(pproc->m_ppairCodeExpr);
        PrintRef(body, &str);
        std::fprintf(stderr, "DEEP lambda: %s\n", str.substr(0, 3000).c_str());
    }
    CRef ref;
    for (CPair* ppairForm = pproc->m_ppairCodeExpr.get(); ppairForm != nullptr; ppairForm = ppairForm->m_ppairNext.get())
        ref = RefEval(ppairForm, pframe);
    return ref;
}

// orig: 0x00127228 (begin: the last value, void when empty)
CRef RefEvalBegin(CPair* ppair, CFrame* pframe)
{
    CRef ref = RefVoid();
    for (CPair* ppairForm = PpairArgs(ppair); ppairForm != nullptr; ppairForm = ppairForm->m_ppairNext.get())
        ref = RefEval(ppairForm, pframe);
    return ref;
}

namespace
{
// The parameter counts of a procedure value; false when it is not one.
bool FProcCounts(const CRef& refProc, int* pcarg, int* pfRest)
{
    switch (refProc.m_tagk)
    {
    case TAGK_Bifk:
        if (refProc.m_bifk < 0 || refProc.m_bifk >= BIFK_Max)
            return false;
        *pcarg = g_mpbifkbif[refProc.m_bifk].crefReq;
        *pfRest = g_mpbifkbif[refProc.m_bifk].fVarArg;
        return true;
    case TAGK_Method:
        if (!refProc.m_pmethod)
            return false;
        *pcarg = refProc.m_pmethod->m_crefReq;
        *pfRest = 0;
        return true;
    case TAGK_Proc:
        if (!refProc.m_pproc)
            return false;
        *pcarg = refProc.m_pproc->m_crefReq;
        *pfRest = refProc.m_pproc->m_fVarArg;
        return true;
    default:
        return false;
    }
}

// The call of the second half of 0x1272E0: aref holds the carg fixed arguments, then (fRest) the rest list;
// cargAll counts the rest list's elements one by one (the built-ins get it as argc).
CRef RefCall(const CRef& refProc, int cargAll, CRef* aref, int cBind, CFrame* pframe)
{
    switch (refProc.m_tagk)
    {
    case TAGK_Bifk:
        return g_mpbifkbif[refProc.m_bifk].pfnbif(cargAll, aref, pframe);
    case TAGK_Method:
        return refProc.m_pmethod->m_pfnthunk(refProc.m_pmethod->m_pbasic, cargAll, aref);
    case TAGK_Proc:
    {
        CProc* pproc = refProc.m_pproc.get();
        auto pframeCall = PframeNew();
        pframeCall->SetSingleParent(pproc->m_pframe.get());
        CPair* ppairFormal = pproc->m_ppairFormals.get();
        CRef* pref = aref;
        for (int k = cBind; k > 0 && ppairFormal != nullptr; k--)
        {
            if (ppairFormal->m_ref.m_tagk == TAGK_Dot)
                ppairFormal = ppairFormal->m_ppairNext.get();
            if (ppairFormal == nullptr)
                break;
            pframeCall->RefAddBinding(ppairFormal->m_ref.m_symid, pref);
            pref++;
            ppairFormal = ppairFormal->m_ppairNext.get();
        }
        return RefEvalLambdaBody(pproc, pframeCall.get());
    }
    default:
        return CRef();
    }
}
}

// orig: 0x001272E0 (an application: the operator, then its arguments (the rest as a list when it takes
// one), applied as a built-in (0x19), a method (0x22) or a lambda (0x20))
CRef RefEvalApply(CPair* ppair, CFrame* pframe)
{
    CPair* ppairOp = PpairForm(ppair);
    CRef refProc = RefEval(ppairOp, pframe);
    int carg = 0, fRest = 0;
    if (!FProcCounts(refProc, &carg, &fRest))
    {
        g_splicediag.capplyBad++;
        char ach[96];
        std::snprintf(ach, sizeof(ach), "application of a %s", PchzFromTagk(refProc.m_tagk));
        g_splicediag.Note(ach);
        return CRef();
    }
    CRef aref[32];
    CPair* ppairArg = ppairOp->m_ppairNext.get();
    int cargAll = carg;
    int i = 0;
    for (; i < carg && i < 31; i++)
    {
        if (ppairArg == nullptr)
        {
            g_splicediag.cargMissing++;
            char ach[96];
            std::snprintf(ach, sizeof(ach), "%s: argument %d of %d missing",
                          refProc.m_tagk == TAGK_Bifk ? g_mpbifkbif[refProc.m_bifk].achz : PchzFromTagk(refProc.m_tagk), i + 1, carg);
            g_splicediag.Note(ach);
            continue;
        }
        aref[i] = RefEval(ppairArg, pframe);
        ppairArg = ppairArg->m_ppairNext.get();
    }
    if (fRest)
    {
        std::shared_ptr<CPair> ppairHead;
        CPair* ppairTail = nullptr;
        for (; ppairArg != nullptr; ppairArg = ppairArg->m_ppairNext.get())
        {
            auto ppairNew = PpairNew();
            ppairNew->m_ref = RefEval(ppairArg, pframe);
            if (ppairTail != nullptr)
                ppairTail->m_ppairNext = ppairNew;
            else
                ppairHead = ppairNew;
            ppairTail = ppairNew.get();
            cargAll++;
        }
        if (ppairHead)
            aref[i].SetPair(std::move(ppairHead));
        else
            aref[i].SetTag(TAGK_None);
    }
    return RefCall(refProc, cargAll, aref, carg + (fRest ? 1 : 0), pframe);
}

CRef RefApply(const CRef& refProc, int cref, CRef* aref, CFrame* pframe)
{
    int carg = 0, fRest = 0;
    if (!FProcCounts(refProc, &carg, &fRest))
    {
        g_splicediag.capplyBad++;
        g_splicediag.Note(std::string("apply of a ") + PchzFromTagk(refProc.m_tagk));
        return CRef();
    }
    CRef arefCall[32];
    int i = 0;
    for (; i < carg && i < 31; i++)
        arefCall[i] = i < cref ? aref[i] : CRef();
    int cargAll = carg;
    if (fRest)
    {
        std::shared_ptr<CPair> ppairHead;
        CPair* ppairTail = nullptr;
        for (int j = carg; j < cref; j++)
        {
            auto ppairNew = PpairNew();
            ppairNew->m_ref = aref[j];
            if (ppairTail != nullptr)
                ppairTail->m_ppairNext = ppairNew;
            else
                ppairHead = ppairNew;
            ppairTail = ppairNew.get();
            cargAll++;
        }
        if (ppairHead)
            arefCall[i].SetPair(std::move(ppairHead));
        else
            arefCall[i].SetTag(TAGK_None);
    }
    return RefCall(refProc, cargAll, arefCall, carg + (fRest ? 1 : 0), pframe);
}

// orig: 0x00127838 RefEval (a symbol (0x18), a form (0x1F: by its first cell's kind), or the value itself)
CRef RefEval(CPair* ppair, CFrame* pframe)
{
    if (ppair == nullptr)
        return CRef();
    switch (ppair->m_ref.m_tagk)
    {
    case TAGK_Symid:
        return RefEvalSymbol(ppair, pframe);
    case TAGK_Pair:
    {
        CPair* ppairList = PpairForm(ppair);
        if (ppairList == nullptr)
        {
            NoteMissing("form");
            return CRef();
        }
        switch (ppairList->m_ref.m_tagk)
        {
        case TAGK_Set: return RefEvalSet(ppair, pframe);
        case TAGK_Define: return RefEvalDefine(ppair, pframe);
        case TAGK_Assert: return RefEvalAssert(ppair, pframe);
        case TAGK_Debug: return RefEvalDebug(ppair, pframe);
        case TAGK_If: return RefEvalIf(ppair, pframe);
        case TAGK_Or: return RefEvalOr(ppair, pframe);
        case TAGK_And: return RefEvalAnd(ppair, pframe);
        case TAGK_Cond: return RefEvalCond(ppair, pframe);
        case TAGK_Case: return RefEvalCase(ppair, pframe);
        case TAGK_Let: return RefEvalLet(ppair, pframe);
        case TAGK_While: return RefEvalWhile(ppair, pframe);
        case TAGK_Lambda: return RefEvalLambda(ppair, pframe);
        case TAGK_Begin: return RefEvalBegin(ppair, pframe);
        case TAGK_Import: return CRef();
        case TAGK_Quote: return ppairList->m_ppairNext ? ppairList->m_ppairNext->m_ref : CRef();
        default: return RefEvalApply(ppair, pframe);
        }
    }
    default:
        return ppair->m_ref;
    }
}

// Debugging (from gdb: call SpliceDumpPair(ppair, 6)): a list printed as an s-expression (symbols as s<id>, built-ins
// as b<n>, objects as their address).
static void DumpRef(const CRef& r, int depth);
static void DumpList(const CPair* p, int depth)
{
    std::printf("(");
    for (; p; p = p->m_ppairNext.get())
    {
        DumpRef(p->m_ref, depth);
        if (p->m_ppairNext) std::printf(" ");
    }
    std::printf(")");
}
static void DumpRef(const CRef& r, int depth)
{
    static const char* s_aszTag[] = {"", "set!", "define", "defmacro", "defmacro-lambda", "assert", "debug", "if", "or", "and",
                                      "cond", "else", "case", "let", "while", "lambda", "begin", "import", "quote", "pipe", "."};
    switch (r.m_tagk)
    {
    case TAGK_S32: std::printf("%d", r.m_n); break;
    case TAGK_F32: std::printf("%g", r.m_g); break;
    case TAGK_Bool: std::printf(r.m_bool ? "#t" : "#f"); break;
    case TAGK_Symid: std::printf("s%d", int(r.m_symid)); break;
    case TAGK_Bifk: std::printf("b%d", int(r.m_bifk)); break;
    case TAGK_Basic: std::printf("<obj %p>", (void*)r.m_pbasic); break;
    case TAGK_Pair:
        if (depth <= 0) std::printf("(...)");
        else DumpList(r.m_ppair.get(), depth - 1);
        break;
    case TAGK_Proc: std::printf("<proc>"); break;
    case TAGK_Void: std::printf("<void>"); break;
    case TAGK_None: std::printf("()"); break;
    default:
        if (r.m_tagk >= 1 && r.m_tagk <= 20) std::printf("%s", s_aszTag[r.m_tagk]);
        else std::printf("<t%d>", int(r.m_tagk));
    }
}
extern "C" void SpliceDumpPair(const CPair* p, int depth)
{
    DumpList(p, depth);
    std::printf("\n");
    std::fflush(stdout);
}
