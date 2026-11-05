// --splice-world W|all: the splice runtime's self-test over Sly 3's worlds. Every group's splice block (orig3 0x2499F0
// LoadSwSpliceFromBrx) is read by the runtime's own loader (splicemap.cpp, PpairSerializeIn 0x12C108 with Sly 3's tags)
// at the offset the world walk (brx3.cpp) found, must end exactly where the walk's parse ended, and each module's
// top-level form is evaluated in its own root frame as the original does (PframeNew + RefEval). Reported per world:
// the modules, their bindings, every evaluation problem (unbound symbols, applications of non-procedures, missing
// parts), the built-ins the code names that are not ported (stubs) or only Sly 2 drafts (game hooks), and the stub
// calls module init made.

#include "brx3.h"
#include "splice3.h"

#include "../bif.h"
#include "../bis.h"
#include "../disc.h"
#include "../eval.h"
#include "../gc.h"
#include "../serialize.h"
#include "../splicemap.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
struct SPLICEWORLD3
{
    long cblock = 0, cmodule = 0, cmismatch = 0, cbinding = 0;
    std::map<int, long> mptagkcBinding;  // the modules' top-level bindings by kind
    std::map<int, long> mpbifkcStatic;   // the built-ins the code names
    std::string firstMismatch;
};
SPLICEWORLD3* s_psw3 = nullptr;
SW* const PSW_TEST = nullptr;  // the splice state of the test's world (PswspliceFromSw keys by SW*)

void CountBif(const CPair* ppair, std::map<int, long>* pmpbifkc)
{
    for (; ppair; ppair = ppair->m_ppairNext.get())
    {
        if (ppair->m_ref.m_tagk == TAGK_Bifk)
            (*pmpbifkc)[ppair->m_ref.m_bifk]++;
        else if (ppair->m_ref.m_tagk == TAGK_Pair)
            CountBif(ppair->m_ref.m_ppair.get(), pmpbifkc);
    }
}

// The walk's hook: the runtime's loader over the same bytes (orig3 0x2499F0, with the group record's +0x10 as Sly 2's)
void SpliceBlock(const uint8_t* pb, std::size_t ib, std::size_t ibEnd, int, const GROUP3& g)
{
    CBinaryInputStream bis;
    bis.OpenMemory(int(ibEnd), pb);
    bis.m_ib = ib;
    g_fLoadDebugInfo = g.fScriptDebug;
    LoadSwSpliceFromBrx(PSW_TEST, g.x10, &bis);
    ++s_psw3->cblock;
    if (bis.m_ib != ibEnd && s_psw3->cmismatch++ == 0)
        s_psw3->firstMismatch = std::string(g.achName) + ": the runtime read to " + std::to_string(bis.m_ib) +
                                ", the walk to " + std::to_string(ibEnd);
    EndSwSpliceGroup(PSW_TEST);
}

// A built-in's state: 0 ported (the language's, native), 1 a game hook (copied from Sly 2: a draft), 2 a stub
int StateOfBif(int bifk)
{
    if (bifk < 0 || bifk >= BIFK_Max) return 2;
    if (!FBifImplemented(static_cast<BIFK>(bifk))) return 2;
    return bifk >= 103 && !FBifNative(static_cast<BIFK>(bifk)) ? 1 : 0;
}

struct ALLSUM
{
    int cworld = 0, cfail = 0;
    long cmodule = 0, cbinding = 0;
    std::map<int, std::set<std::string>> mpbifkworldsUnported;  // stubs and drafts: the worlds whose code names them
    std::map<int, long> mpbifkcUnported;
};

int SpliceOneWorld(const WORLD3& wd, bool fVerbose, ALLSUM* psum)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> ab = g_disc.ReadWorld(wd.fk);
    SPLICEWORLD3 sw3;
    s_psw3 = &sw3;
    g_splicediag = SPLICEDIAG{};
    g_serializestats = SERIALIZESTATS{};
    g_gc.Startup();
    DeleteSwSplice(PSW_TEST);
    StartupSwSplice(PSW_TEST);
    g_pfnSpliceBlock3 = SpliceBlock;
    WORLDWALK3 w;
    std::string error;
    const bool fWalk = FWalkWorld3(ab, w, error);
    g_pfnSpliceBlock3 = nullptr;
    s_psw3 = nullptr;

    SWSPLICE* pswsplice = PswspliceFromSw(PSW_TEST);
    for (const auto& psplct : pswsplice->apsplct)
        for (const SPLC& splc : psplct->asplc)
        {
            ++sw3.cmodule;
            CountBif(splc.ppairCompile.get(), &sw3.mpbifkcStatic);
            if (!splc.pframe) continue;
            for (const VARB& varb : splc.pframe->m_avarb)
            {
                ++sw3.cbinding;
                ++sw3.mptagkcBinding[varb.ref.m_tagk];
            }
        }
    long ccell = 0;
    for (long c : g_serializestats.acpairTagk) ccell += c;
    const bool fCellsAgree = ccell == w.cpairSplice;
    const long cproblem = g_splicediag.csymUnbound + g_splicediag.capplyBad + g_splicediag.cargMissing;
    const bool fOk = fWalk && sw3.cmismatch == 0 && fCellsAgree && cproblem == 0;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::printf("%-14s %3ld blocks, %5ld modules, %7ld cells, %6ld bindings: %s (unbound %ld, bad apply %ld, missing %ld; "
                "stub calls %ld) %.0f ms\n",
                wd.name.c_str(), sw3.cblock, sw3.cmodule, ccell, sw3.cbinding, fOk ? "ok" : "FAILED",
                g_splicediag.csymUnbound, g_splicediag.capplyBad, g_splicediag.cargMissing, g_splicediag.cbifStub, ms);
    if (!fWalk) std::printf("  walk failed: %s\n", error.c_str());
    if (sw3.cmismatch) std::printf("  %ld blocks end elsewhere than the walk's: %s\n", sw3.cmismatch, sw3.firstMismatch.c_str());
    if (!fCellsAgree) std::printf("  cells: the runtime read %ld, the walk %d\n", ccell, w.cpairSplice);
    for (std::size_t i = 0; i < g_splicediag.amsg.size() && (fVerbose || i < 5); ++i)
        std::printf("  ! %s\n", g_splicediag.amsg[i].c_str());
    // the built-ins of the code that are not ported
    std::string szStub, szDraft;
    for (auto [bifk, c] : sw3.mpbifkcStatic)
    {
        const int state = StateOfBif(bifk);
        if (state == 0) continue;
        char sz[96];
        std::snprintf(sz, sizeof(sz), " %d:%s x%ld", bifk, bifk >= 0 && bifk < BIFK_Max ? g_mpbifkbif[bifk].achz : "?", c);
        (state == 2 ? szStub : szDraft) += sz;
        if (psum)
        {
            psum->mpbifkworldsUnported[bifk].insert(wd.name);
            psum->mpbifkcUnported[bifk] += c;
        }
    }
    if (fVerbose || !psum)
    {
        std::printf("  unported built-ins named by the code (stubs):%s\n", szStub.empty() ? " none" : szStub.c_str());
        std::printf("  game built-ins with Sly 2's code (drafts):%s\n", szDraft.empty() ? " none" : szDraft.c_str());
        std::printf("  stub calls during module init:");
        for (int i = 0; i < BIFK_Max; ++i)
            if (long c = g_splicediag.acbifStubCalls[std::size_t(i)]) std::printf(" %d:%s x%ld", i, g_mpbifkbif[i].achz, c);
        std::printf("\n  bindings:");
        for (auto [tagk, c] : sw3.mptagkcBinding) std::printf(" %s %ld", PchzFromTagk(tagk), c);
        std::printf("\n");
    }
    if (psum)
    {
        ++psum->cworld;
        psum->cfail += !fOk;
        psum->cmodule += sw3.cmodule;
        psum->cbinding += sw3.cbinding;
    }
    g_gc.ShutDown();
    DeleteSwSplice(PSW_TEST);
    return fOk ? 0 : 1;
}
}  // namespace

int SpliceWorld3Command(const std::string& arg, bool fVerbose)
{
    const std::vector<WORLD3> aw = g_disc.Worlds();
    if (arg == "all")
    {
        ALLSUM sum;
        for (const WORLD3& wd : aw)
            if (wd.fk.cb) SpliceOneWorld(wd, fVerbose, &sum);
        std::printf("%d worlds, %d failed; %ld modules evaluated, %ld top-level bindings\n", sum.cworld, sum.cfail,
                    sum.cmodule, sum.cbinding);
        std::printf("built-ins named by the code that are stubs (s) or Sly 2 drafts (d), with the worlds that name them:\n");
        for (const auto& [bifk, set] : sum.mpbifkworldsUnported)
        {
            std::printf("  %3d %-34s %c x%-6ld %2zu worlds:", bifk, bifk >= 0 && bifk < BIFK_Max ? g_mpbifkbif[bifk].achz : "?",
                        StateOfBif(bifk) == 2 ? 's' : 'd', sum.mpbifkcUnported[bifk], set.size());
            int i = 0;
            for (const std::string& sz : set)
                if (i++ < 8) std::printf(" %s", sz.c_str());
            std::printf("%s\n", set.size() > 8 ? " ..." : "");
        }
        return sum.cfail ? 1 : 0;
    }
    for (const WORLD3& wd : aw)
    {
        char* pchEnd = nullptr;
        const unsigned long n = std::strtoul(arg.c_str(), &pchEnd, 0);
        if (wd.name == arg || (pchEnd != arg.c_str() && !*pchEnd && static_cast<unsigned long>(wd.iworld) == n))
        {
            if (!wd.fk.cb)
            {
                std::printf("world %s: no file\n", wd.name.c_str());
                return 0;
            }
            return SpliceOneWorld(wd, fVerbose, nullptr);
        }
    }
    std::fprintf(stderr, "no world %s (--list-files)\n", arg.c_str());
    return 2;
}
