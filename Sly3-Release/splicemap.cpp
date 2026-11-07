#include "brx.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include "splicemap.h"

#include <cstdio>
#include <unordered_map>

#include "bis.h"
#include "eval.h"
#include "gc.h"
#include "loadtrace.h"
#include "serialize.h"

namespace
{
std::unordered_map<SW*, std::unique_ptr<SWSPLICE>> s_mppswswsplice;

// orig: 0x00118420 (a debug string: U16 length, the bytes skipped)
std::string ReadDebugString(CBinaryInputStream* pbis)
{
    return pbis->ReadSkippedStringSw();
}
}

SWSPLICE* PswspliceFromSw(SW* psw)
{
    auto& p = s_mppswswsplice[psw];
    if (!p)
        p = std::make_unique<SWSPLICE>();
    return p.get();
}

void DeleteSwSplice(SW* psw)
{
    s_mppswswsplice.erase(psw);
}

// orig: 0x001FF7E8
void StartupSwSplice(SW* psw)
{
    SWSPLICE* pswsplice = PswspliceFromSw(psw);
    pswsplice->fOn = 1;
    pswsplice->pframe = PframeNew();
    g_gc.AddRootFrame(pswsplice->pframe.get());
}

// orig: 0x001FF828
void ShutdownSwSplice(SW* psw)
{
    PswspliceFromSw(psw)->fOn = 0;
}

void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)

std::unique_ptr<SPLCT> PsplctLoadFromBrx(int a, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1FF858, pbis);
    const uint32_t c = pbis->U32Read();
    if (c == 0)
        return nullptr;
    auto psplct = std::make_unique<SPLCT>();
    psplct->a = a;
    psplct->asplc.resize(c);
    for (SPLC& splc : psplct->asplc)
    {
        splc.ppairCompile = PpairSerializeIn(pbis);
        // (debug: SLY2N_SPLICEGREP=text: a world module whose printed form contains the text (e.g. "sym77218"), printed)
        if (static const char* s_szGrep = std::getenv("SLY2N_SPLICEGREP"); s_szGrep && splc.ppairCompile)
        {
            std::string str = "(";
            for (CPair* ppair = splc.ppairCompile.get(); ppair; ppair = ppair->m_ppairNext.get())
            {
                PrintRef(ppair->m_ref, &str);
                str += " ";
            }
            str += ")";
            if (str.find(s_szGrep) != std::string::npos)
                std::printf("splicegrep: block %d module %d (%zu chars): %s\n", a, int(&splc - psplct->asplc.data()), str.size(), str.c_str());
        }
        if (g_fLoadDebugInfo)
        {
            splc.achzObjectPath = ReadDebugString(pbis);
            splc.achzFile = ReadDebugString(pbis);
            splc.achzSpliceSource = ReadDebugString(pbis);
        }
    }
    return psplct;
}

// orig: 0x001FFA70
void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)
CRef RefEvalModule(SPLCT* psplct, int isplc)
{
    SPLC& splc = psplct->asplc[static_cast<std::size_t>(isplc)];
    if (splc.ppairCompile && !splc.pframe)
    {
        auto pframe = PframeNew();
        g_gc.AddRootFrame(pframe.get());
        pframe->m_fModule = 1;
        if (static const char* s_szDump = std::getenv("SLY2N_SPLICEDUMP"); s_szDump)  // (debug: every module's code)
            if (static std::FILE* s_pf = std::fopen(s_szDump, "w"); s_pf)
            {
                CRef body;
                body.SetPair(splc.ppairCompile);
                std::string str;
                PrintRef(body, &str);
                std::fprintf(s_pf, "module %d (block %d): %s\n", isplc, psplct->a, str.c_str());
                std::fflush(s_pf);
            }
        splc.refResult = RefEval(splc.ppairCompile.get(), pframe.get());
        splc.pframe = pframe;
    }
    return CRef(TAGK_Void);
}

// orig: 0x001FF858 LoadSwSpliceFromBrx
void LoadSwSpliceFromBrx(SW* psw, int a, CBinaryInputStream* pbis)
{
    std::unique_ptr<SPLCT> psplct = PsplctLoadFromBrx(a, pbis);
    if (!psplct)
        return;
    for (int i = 0; i < static_cast<int>(psplct->asplc.size()); i++)
        RefEvalModule(psplct.get(), i);
    SWSPLICE* pswsplice = PswspliceFromSw(psw);
    pswsplice->psplctCur = psplct.get();
    pswsplice->apsplct.push_back(std::move(psplct));
    // (0x128148: the GC's heap heads saved; nothing natively)
}

// orig3 0x2499F0's reads without the evaluation (--dump-world's walk): the modules parsed, their cells counted
void WalkSpliceFromBrx(SW* psw, int a, CBinaryInputStream* pbis)
{
    (void)psw;
    std::unique_ptr<SPLCT> psplct = PsplctLoadFromBrx(a, pbis);
    if (psplct) g_walk.cmodule += int(psplct->asplc.size());
}

// orig: 0x001FFA40
struct LO;
LO* PloLoading();  // brx.cpp
int OidOfLo(const LO*); int CidOfLo(const LO*);
std::shared_ptr<CFrame> PframeFromIsplice(int isplice, SW* psw)
{
    SWSPLICE* pswsplice = PswspliceFromSw(psw);
    if (isplice < 0 || pswsplice->psplctCur == nullptr ||
        isplice >= static_cast<int>(pswsplice->psplctCur->asplc.size()))
        return nullptr;
    // (debug: SLY2N_MODULEOF="block:module": the objects bound to that world module, at their load)
    if (static const char* s_sz = std::getenv("SLY2N_MODULEOF"); s_sz && std::strchr(s_sz, ':') && pswsplice->psplctCur->a == std::atoi(s_sz) &&
                                                                  isplice == std::atoi(std::strchr(s_sz, ':') + 1))
        if (LO* plo = PloLoading()) std::printf("moduleof %s: loading %d (cid 0x%X)\n", s_sz, OidOfLo(plo), CidOfLo(plo));
    if (static const bool s_f = std::getenv("SLY2N_FRAMEIDXPRINT") != nullptr; s_f)  // (debug: which block a frame index resolves in)
        std::printf("frame index %d in block %d\n", isplice, pswsplice->psplctCur->a);
    return pswsplice->psplctCur->asplc[static_cast<std::size_t>(isplice)].pframe;
}

void EndSwSpliceGroup(SW* psw)
{
    PswspliceFromSw(psw)->psplctCur = nullptr;
}

// (debug: the current splice block's id, for SLY2N_IFRAMELIST)
int IspliceBlockCur(SW* psw)
{
    SWSPLICE* pswsplice = PswspliceFromSw(psw);
    return pswsplice->psplctCur ? pswsplice->psplctCur->a : -1;
}
