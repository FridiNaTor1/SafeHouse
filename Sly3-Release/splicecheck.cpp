// Development checks of the splice runtime and the ASEG loaders against the original's parse maps:
//   --parse-splice W   every scripts block (0x1FF858) and ASEG (0x12F2A8) of world W parsed from where the map
//                      says the original started it; the native loaders' reads (LOAD_SCOPE) must give the same
//                      runs (start, end, function) and end where the original's did.
//   --eval-splice W    the scripts blocks loaded and each module evaluated (module init), with a report of
//                      the bindings and of what the evaluator could not do.
// W: a world record 0..43 (w<W>/map/file-3.map; 0 = the boot world, 1 = the front end), "boot", "frontend".

#include "splicecheck.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "aseg.h"
#include "bif.h"
#include "bis.h"
#include "disc.h"
#include "eval.h"
#include "gc.h"
#include "loadtrace.h"
#include "proc.h"
#include "serialize.h"
#include "splicemap.h"
#include "spliceglue.h"
#include "spliceobj.h"
#include "aijobs.h"

namespace
{
struct MAPRUN
{
    std::size_t ibStart;
    std::size_t ibEnd;
    uint32_t addr;
};

bool FLoadMap(const std::string& path, std::vector<MAPRUN>* parun)
{
    std::ifstream file(path);
    if (!file)
        return false;
    std::string line;
    while (std::getline(file, line))
    {
        unsigned long long a = 0, b = 0;
        unsigned int addr = 0;
        if (std::sscanf(line.c_str(), "%llu %llu 0x%x", &a, &b, &addr) == 3)
            parun->push_back({static_cast<std::size_t>(a), static_cast<std::size_t>(b), addr});
    }
    return true;
}

struct WORLDSRC
{
    std::string name;
    FK fk;
    std::string pathMap;
};

// The boot world's 44 world records (0x20F318, at the offset the boot world's map gives).
bool FWorldRecord(int irec, const std::string& dirMaps, FK* pfk, std::string& error)
{
    std::vector<MAPRUN> arun;
    if (!FLoadMap(dirMaps + "/w0/map/file-0.map", &arun))
    {
        error = "no boot world map " + dirMaps + "/w0/map/file-0.map";
        return false;
    }
    std::size_t ib = 0;
    for (const MAPRUN& run : arun)
        if (run.addr == 0x20F318)
        {
            ib = run.ibStart;
            break;
        }
    if (ib == 0)
    {
        error = "no 0x20F318 run in the boot world map";
        return false;
    }
    std::vector<uint8_t> ab = g_disc.ReadWorld(FkFromRecord(g_disc.PbElf(VA_FK_BOOT_WORLD, 0x40)));
    CBinaryInputStream bis;
    bis.OpenBuffer(std::move(ab));
    bis.m_ib = ib;
    const int c = static_cast<int>(bis.U32Read());
    bis.U32Read();
    for (int i = 0; i < c; i++)
    {
        uint8_t rec[0x40];
        bis.ReadBytes(rec, sizeof(rec));
        bis.U8Read();
        bis.U8Read();
        bis.U8Read();
        const int cw = bis.U8Read();
        for (int j = 0; j < cw; j++)
            bis.U16Read();
        if (i == irec)
        {
            *pfk = FkFromRecord(rec);
            return true;
        }
    }
    error = "no world record " + std::to_string(irec);
    return false;
}

bool FWorldSource(const std::string& arg, const std::string& dirMaps, WORLDSRC* psrc, std::string& error)
{
    if (arg == "boot" || arg == "0")
    {
        psrc->name = "boot";
        psrc->fk = FkFromRecord(g_disc.PbElf(VA_FK_BOOT_WORLD, 0x40));
        psrc->pathMap = dirMaps + "/w0/map/file-0.map";
        return true;
    }
    if (arg == "frontend" || arg == "1")
    {
        psrc->name = "frontend";
        psrc->fk = FkFromRecord(g_disc.PbElf(VA_FK_FRONTEND, 0x40));
        psrc->pathMap = dirMaps + "/w1/map/file-2.map";
        return true;
    }
    const int irec = std::atoi(arg.c_str());
    psrc->name = "w" + std::to_string(irec);
    psrc->pathMap = dirMaps + "/w" + std::to_string(irec) + "/map/file-3.map";
    return FWorldRecord(irec, dirMaps, &psrc->fk, error);
}

const std::set<uint32_t> s_setAddrAseg = {0x12F2A8, 0x12F590, 0x12B4F8, 0x12B5E0, 0x12BA98, 0x12BF30,
                                          0x12C570, 0x12CC68, 0x12CCB8};
const std::set<uint32_t> s_setAddrSplice = {0x1FF858, 0x12A3D8};

// Compares the native runs with the map's from index i; returns the index after the last matched run, or
// -1 (with a message) on a mismatch.
long ICompareRuns(const std::vector<LOADRUN>& arunNative, const std::vector<MAPRUN>& arunMap, std::size_t i,
                  std::size_t ibEndNative, std::string* pmsg)
{
    std::size_t j = i;
    for (std::size_t k = 0; k < arunNative.size(); k++, j++)
    {
        const LOADRUN& a = arunNative[k];
        if (j >= arunMap.size() || arunMap[j].ibStart != a.ibStart || arunMap[j].addr != a.addr ||
            (k + 1 < arunNative.size() && arunMap[j].ibEnd != a.ibEnd))
        {
            char ach[256];
            if (j < arunMap.size())
                std::snprintf(ach, sizeof(ach), "run %zu: native %zu..%zu 0x%X, map %zu..%zu 0x%X", k, a.ibStart, a.ibEnd,
                              a.addr, arunMap[j].ibStart, arunMap[j].ibEnd, arunMap[j].addr);
            else
                std::snprintf(ach, sizeof(ach), "run %zu: native %zu..%zu 0x%X past the map's end", k, a.ibStart, a.ibEnd, a.addr);
            *pmsg = ach;
            return -1;
        }
    }
    // The last native run ends where the next map run starts (the map's end of a run is the next one's start).
    if (j < arunMap.size() && ibEndNative != arunMap[j].ibStart)
    {
        char ach[160];
        std::snprintf(ach, sizeof(ach), "ends at %zu, the original's next run (0x%X) starts at %zu", ibEndNative,
                      arunMap[j].addr, arunMap[j].ibStart);
        *pmsg = ach;
        return -1;
    }
    return static_cast<long>(j);
}

struct WORLDLOAD
{
    WORLDSRC src;
    std::vector<MAPRUN> arun;
    CBinaryInputStream bis;
};

bool FOpenWorld(const std::string& arg, const std::string& dirMaps, WORLDLOAD* pwl)
{
    std::string error;
    if (!FWorldSource(arg, dirMaps, &pwl->src, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (pwl->src.fk.cb == 0)
    {
        std::printf("%s: the world record has no file (nothing to check)\n", pwl->src.name.c_str());
        return false;
    }
    if (!FLoadMap(pwl->src.pathMap, &pwl->arun))
    {
        std::fprintf(stderr, "%s: no map %s\n", pwl->src.name.c_str(), pwl->src.pathMap.c_str());
        return false;
    }
    pwl->bis.OpenBuffer(g_disc.ReadWorld(pwl->src.fk));
    return true;
}
}

int ParseSpliceCommand(const std::string& arg, const std::string& dirMaps, bool fVerbose)
{
    WORLDLOAD wl;
    if (!FOpenWorld(arg, dirMaps, &wl))
        return 1;
    CBinaryInputStream& bis = wl.bis;
    const std::vector<MAPRUN>& arun = wl.arun;
    g_serializestats = SERIALIZESTATS{};
    g_fLoadDebugInfo = 0;  // 0x3E1C34: 0 in every group of the shipped worlds (no 0x118420 reads in any map)
    g_pfnLoadOptionsFromBrx = nullptr;

    long cblock = 0, cmodule = 0, casegs = 0, cfail = 0;
    std::size_t cbSplice = 0, cbAseg = 0;
    std::vector<bool> afCovered(arun.size(), false);
    for (std::size_t i = 0; i < arun.size(); i++)
    {
        const uint32_t addr = arun[i].addr;
        const bool fAsegStart = addr == 0x12F2A8 && (i == 0 || !s_setAddrAseg.count(arun[i - 1].addr));
        if (addr != 0x1FF858 && !fAsegStart)
            continue;
        CLoadTrace trace(&bis);
        g_ploadtrace = &trace;
        bis.m_ib = arun[i].ibStart;
        std::string msg;
        long iEnd = -1;
        try
        {
            if (addr == 0x1FF858)
            {
                std::unique_ptr<SPLCT> psplct = PsplctLoadFromBrx(0, &bis);
                cblock++;
                cmodule += psplct ? static_cast<long>(psplct->asplc.size()) : 0;
            }
            else
            {
                ASEGD asegd;
                LoadAsegFromBrx(&asegd, nullptr, &bis);
                casegs++;
            }
            iEnd = ICompareRuns(trace.m_arun, arun, i, bis.m_ib, &msg);
        }
        catch (const std::exception& e)
        {
            msg = e.what();
        }
        g_ploadtrace = nullptr;
        if (iEnd < 0)
        {
            cfail++;
            if (cfail <= 10)
                std::printf("%s: %s at %zu: %s\n", wl.src.name.c_str(), addr == 0x1FF858 ? "scripts" : "ASEG",
                            arun[i].ibStart, msg.c_str());
            continue;
        }
        for (std::size_t j = i; j < static_cast<std::size_t>(iEnd); j++)
            afCovered[j] = true;
        (addr == 0x1FF858 ? cbSplice : cbAseg) += bis.m_ib - arun[i].ibStart;
        if (addr == 0x12F2A8 && (static_cast<std::size_t>(iEnd) >= arun.size() || arun[static_cast<std::size_t>(iEnd)].addr != 0x13F810))
        {
            cfail++;
            std::printf("%s: ASEG at %zu: not followed by its options (0x13F810)\n", wl.src.name.c_str(), arun[i].ibStart);
        }
    }
    // Every run of the splice / ASEG functions must have been reproduced.
    long cuncovered = 0;
    for (std::size_t i = 0; i < arun.size(); i++)
    {
        if ((s_setAddrSplice.count(arun[i].addr) || s_setAddrAseg.count(arun[i].addr)) && !afCovered[i])
        {
            if (++cuncovered <= 5)
                std::printf("%s: run %zu..%zu 0x%X not reproduced\n", wl.src.name.c_str(), arun[i].ibStart, arun[i].ibEnd, arun[i].addr);
        }
    }
    long ccell = 0;
    for (long c : g_serializestats.acpairTagk)
        ccell += c;
    std::printf("%s: %ld scripts blocks (%ld modules, %ld cells, %zu bytes), %ld ASEGs (%zu bytes): %s",
                wl.src.name.c_str(), cblock, cmodule, ccell, cbSplice, casegs, cbAseg,
                cfail == 0 && cuncovered == 0 ? "all match the map" : "MISMATCH");
    if (cfail || cuncovered)
        std::printf(" (%ld failed, %ld runs not reproduced)", cfail, cuncovered);
    std::printf("\n");
    if (fVerbose)
    {
        std::printf("  cells by tag:");
        for (int t = 0; t < 128; t++)
            if (g_serializestats.acpairTagk[t])
                std::printf(" %s:%ld", PchzFromTagk(t), g_serializestats.acpairTagk[t]);
        std::printf("\n  objects (tag 33) with a nonzero word: %ld\n", g_serializestats.cbasicNonzero);
    }
    return cfail == 0 && cuncovered == 0 ? 0 : 1;
}

namespace
{
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
}

namespace
{
std::string StrValue(const CRef& ref)
{
    char ach[128];
    ach[0] = 0;
    switch (ref.m_tagk)
    {
    case TAGK_S32: std::snprintf(ach, sizeof(ach), "%d", ref.m_n); break;
    case TAGK_F32: std::snprintf(ach, sizeof(ach), "%g", ref.m_g); break;
    case TAGK_Bool: case TAGK_Symid: case TAGK_Bifk: std::snprintf(ach, sizeof(ach), "%u", static_cast<unsigned>(ref.m_n)); break;
    case TAGK_Vector: case TAGK_Clq: case TAGK_Smp:
        std::snprintf(ach, sizeof(ach), "%g %g %g", ref.m_vector.x, ref.m_vector.y, ref.m_vector.z);
        break;
    case TAGK_Lm: std::snprintf(ach, sizeof(ach), "%g %g", ref.m_vector.x, ref.m_vector.y); break;
    case TAGK_Pair:
    {
        int c = 0;
        for (CPair* p = ref.m_ppair.get(); p; p = p->m_ppairNext.get())
            c++;
        std::snprintf(ach, sizeof(ach), "len %d", c);
        break;
    }
    case TAGK_Proc: std::snprintf(ach, sizeof(ach), "c %d rest %d", ref.m_pproc->m_crefReq, ref.m_pproc->m_fVarArg); break;
    case TAGK_Basic: std::snprintf(ach, sizeof(ach), "%s", ref.m_pbasic ? "obj" : "null"); break;
    default: break;
    }
    return ach;
}
}

int EvalSpliceCommand(const std::string& arg, const std::string& dirMaps, bool fVerbose, const std::string& pathDump)
{
    WORLDLOAD wl;
    if (!FOpenWorld(arg, dirMaps, &wl))
        return 1;
    g_fLoadDebugInfo = 0;
    g_splicediag = SPLICEDIAG{};
    g_gc.Startup();
    DeleteSwSplice(nullptr);
    StartupSwSplice(nullptr);
    std::map<int, long> mpbifkcStatic;
    long cblock = 0;
    for (const MAPRUN& run : wl.arun)
    {
        if (run.addr != 0x1FF858)
            continue;
        wl.bis.m_ib = run.ibStart;
        LoadSwSpliceFromBrx(nullptr, static_cast<int>(cblock), &wl.bis);
        cblock++;
    }
    SWSPLICE* pswsplice = PswspliceFromSw(nullptr);
    long cmodule = 0, cbinding = 0;
    std::map<int, long> mptagkc;
    for (const auto& psplct : pswsplice->apsplct)
    {
        for (const SPLC& splc : psplct->asplc)
        {
            cmodule++;
            CountBif(splc.ppairCompile.get(), &mpbifkcStatic);
            if (!splc.pframe)
                continue;
            for (const VARB& varb : splc.pframe->m_avarb)
            {
                cbinding++;
                mptagkc[varb.ref.m_tagk]++;
            }
        }
    }
    std::printf("%s: %ld scripts blocks, %ld modules evaluated: %ld top-level bindings (", wl.src.name.c_str(), cblock,
                cmodule, cbinding);
    bool fFirst = true;
    for (auto [tagk, c] : mptagkc)
    {
        std::printf("%s%s %ld", fFirst ? "" : ", ", PchzFromTagk(tagk), c);
        fFirst = false;
    }
    std::printf(")\n  unbound symbols %ld, bad applications %ld, missing parts %ld, game built-in stub calls %ld\n",
                g_splicediag.csymUnbound, g_splicediag.capplyBad, g_splicediag.cargMissing, g_splicediag.cbifStub);
    for (int i = 0; i < BIFK_Max; i++)
        if (g_splicediag.acbifStubCalls[static_cast<std::size_t>(i)])
            std::printf("  stub %d %s: %ld calls\n", i, g_mpbifkbif[i].achz, g_splicediag.acbifStubCalls[static_cast<std::size_t>(i)]);
    if (fVerbose)
    {
        for (const std::string& msg : g_splicediag.amsg)
            std::printf("  ! %s\n", msg.c_str());
        std::printf("  built-ins in the code:");
        for (auto [bifk, c] : mpbifkcStatic)
            std::printf(" %d:%s%s x%ld", bifk, bifk >= 0 && bifk < BIFK_Max ? g_mpbifkbif[bifk].achz : "?",
                        FBifImplemented(static_cast<BIFK>(bifk)) ? "" : "(stub)", c);
        std::printf("\n");
    }
    if (!pathDump.empty())
    {
        // The format of tools/splice_ram_dump.py (the reference's RAM after the same load).
        std::ofstream file(pathDump);
        int itable = 0;
        for (const auto& psplct : pswsplice->apsplct)
        {
            file << "table " << itable++ << " " << psplct->asplc.size() << "\n";
            int imodule = 0;
            for (const SPLC& splc : psplct->asplc)
            {
                file << "module " << imodule++ << (splc.pframe ? "" : " unevaluated") << "\n";
                if (splc.pframe)
                    for (const VARB& varb : splc.pframe->m_avarb)
                        file << varb.symid << " " << static_cast<int>(varb.ref.m_tagk) << " " << StrValue(varb.ref) << "\n";
            }
        }
    }
    const bool fOk = g_splicediag.csymUnbound == 0 && g_splicediag.capplyBad == 0 && g_splicediag.cargMissing == 0;
    g_gc.ShutDown();
    DeleteSwSplice(nullptr);
    return fOk ? 0 : 1;
}

namespace
{
// A tiny code builder for the self test: cells from a nested initializer.
struct C
{
    CRef ref;
    std::vector<C> a;
    bool fList = false;
    C(int n) { ref.SetS32(n); }
    C(float g) { ref.SetF32(g); }
    C(TAGK tagk) { ref.SetTag(tagk); }
    static C Sym(SYMID s) { C c(0); c.ref.SetSymid(s); return c; }
    static C Bif(BIFK b) { C c(0); c.ref.SetBifk(b); return c; }
    static C L(std::vector<C> a) { C c(0); c.fList = true; c.a = std::move(a); return c; }
};

std::shared_ptr<CPair> PpairBuild(const std::vector<C>& ac)
{
    std::shared_ptr<CPair> ppairHead;
    CPair* ppairTail = nullptr;
    for (const C& c : ac)
    {
        auto ppair = PpairNew();
        if (c.fList)
        {
            if (c.a.empty())
                ppair->m_ref.SetTag(TAGK_None);
            else
                ppair->m_ref.SetPair(PpairBuild(c.a));
        }
        else
            ppair->m_ref = c.ref;
        (ppairTail ? ppairTail->m_ppairNext : ppairHead) = ppair;
        ppairTail = ppair.get();
    }
    return ppairHead;
}
}

namespace
{
// Invariants of the matrix / rotation built-ins (71..82): round trips and the defining properties.
int MathSelfTest()
{
    auto call = [](BIFK bifk, std::vector<CRef> a) {
        a.resize(8);
        return g_mpbifkbif[bifk].pfnbif(g_mpbifkbif[bifk].crefReq, a.data(), nullptr);
    };
    auto vec = [](float x, float y, float z) { CRef r; r.SetVector({x, y, z}); return r; };
    auto flt = [](float g) { CRef r; r.SetF32(g); return r; };
    auto mat = [](const glm::mat4& m) { CRef r; r.SetMatrix(m); return r; };
    auto near = [](const glm::mat4& a, const glm::mat4& b) {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                if (std::fabs(a[i][j] - b[i][j]) > 1e-4f) return false;
        return true;
    };
    auto nearv = [](const glm::vec3& a, const glm::vec3& b, float e = 1e-4f) { return glm::length(a - b) < e; };
    int cfail = 0;
    auto check = [&](const char* pchz, bool f) { std::printf("%-70s %s\n", pchz, f ? "ok" : "FAIL"); cfail += !f; };
    // a rotation about (1 2 3)/|..| by 0.7, positioned
    const glm::vec3 n = glm::normalize(glm::vec3(1, 2, 3));
    CRef refRad = call(BIFK_VectorRadianNormal, {vec(1, 0, 0), vec(0, 1, 0)});
    check("vector-radian-normal x y = (pi/2 z)", std::fabs(refRad.m_ppair->m_ref.m_g - 1.5707963f) < 1e-5f &&
                                                     nearv(refRad.m_ppair->m_ppairNext->m_ref.GetVector(), {0, 0, 1}));
    // the rotation of 0.7 about n: interpolate-rotate from I to it at t = 1 is it; decompose gives 0.7 and n
    glm::mat4 matI(1.0f);
    CRef refHalf = call(BIFK_MatrixInterpolateRotate, {mat(matI), mat(matI), flt(0.5f)});
    check("matrix-interpolate-rotate I I .5 = I", near(refHalf.GetMatrix(), matI));
    glm::mat4 matR(1.0f);
    {
        const float c = std::cos(0.7f), s = std::sin(0.7f), t = 1 - c;
        matR[0] = glm::vec4(n.x * n.x * t + c, n.y * n.x * t + n.z * s, n.z * n.x * t - n.y * s, 0);
        matR[1] = glm::vec4(n.x * n.y * t - n.z * s, n.y * n.y * t + c, n.z * n.y * t + n.x * s, 0);
        matR[2] = glm::vec4(n.x * n.z * t + n.y * s, n.y * n.z * t - n.x * s, n.z * n.z * t + c, 0);
    }
    CRef refRn = call(BIFK_MatrixDecomposeToRadianNormal, {mat(matR)});
    check("matrix-decompose-to-radian-normal R(0.7, n) = (0.7 n)", std::fabs(refRn.m_ppair->m_ref.m_g - 0.7f) < 1e-4f &&
                                                                       nearv(refRn.m_ppair->m_ppairNext->m_ref.GetVector(), n));
    CRef refT1 = call(BIFK_MatrixInterpolateRotate, {mat(matI), mat(matR), flt(1.0f)});
    CRef refT0 = call(BIFK_MatrixInterpolateRotate, {mat(matR), mat(matI), flt(0.0f)});
    check("matrix-interpolate-rotate I R 1 = R, R I 0 = R", near(refT1.GetMatrix(), matR) && near(refT0.GetMatrix(), matR));
    glm::mat4 matA = matR;
    matA[3] = glm::vec4(10, 20, 30, 1);
    glm::mat4 matB = glm::transpose(matR);
    matB[3] = glm::vec4(-5, 7, 2, 1);
    CRef refD = call(BIFK_MatrixCalculateDmat, {mat(matA), mat(matB)});
    check("matrix-calculate-dmat A B times A = B", near(refD.GetMatrix() * matA, matB));
    CRef refE = call(BIFK_MatrixDecomposeToEuler, {mat(glm::mat4(1.0f))});
    check("matrix-decompose-to-euler I = 0", nearv(refE.GetVector(), {0, 0, 0}));
    CRef refL = call(BIFK_MatrixLookAt, {vec(1, 1, 1), vec(4, 5, 1)});
    const glm::mat4& mL = refL.GetMatrix();
    check("matrix-look-at: x along to - from, orthonormal, z up", nearv(glm::vec3(mL[0]), {0.6f, 0.8f, 0}) &&
                                                                  nearv(glm::vec3(mL[2]), {0, 0, 1}) &&
                                                                  nearv(glm::cross(glm::vec3(mL[0]), glm::vec3(mL[1])), glm::vec3(mL[2])));
    CRef refU = call(BIFK_MatrixTiltUpright, {mat(matA)});
    const glm::mat4& mU = refU.GetMatrix();
    check("matrix-tilt-upright: row 2 = z, position kept", nearv(glm::vec3(mU[2]), {0, 0, 1}) &&
                                                          nearv(glm::vec3(mU[3]), {10, 20, 30}));
    CRef refB = call(BIFK_VectorBallisticVelocity, {vec(0, 0, 0), vec(100, 0, 0), flt(0.52359878f)});
    // 45 degrees on flat ground under 1750: speed sqrt(d g) = sqrt(175000)
    check("vector-ballistic-velocity flat 100, sin g = .5 (45 deg)", nearv(refB.GetVector(), glm::vec3(1, 0, 1) * std::sqrt(175000.0f / 2.0f), 0.05f));
    CRef refTr = call(BIFK_MatrixTranspose, {mat(matA)});
    check("matrix-transpose: all of the 4x4", near(refTr.GetMatrix(), glm::transpose(matA)));
    return cfail;
}

// Job handlers receive the job as self, use its owning actor/list properties, and can keep private bindings.
// Exercise the bridge through actual splice forms and JOBSCRIPT lifecycle, including a parent-bound handler.
int JobScriptSelfTest()
{
    InstallSpliceHooks(nullptr);
    auto parent = PframeNew(), frame = PframeNew();
    frame->AddParent(parent.get());
    const auto S = C::Sym;
    const auto L = C::L;
    const auto get = C::Bif(Bifk3FromSly2(111));
    const auto ensure = C::Bif(Bifk3FromSly2(109));
    const auto set = C::Bif(Bifk3FromSly2(110));
    CRef nil;
    parent->RefAddBinding(9010, &nil);
    parent->RefAddBinding(9011, &nil);
    const C start = L({TAGK_Lambda, L({S(9000)}),
        L({TAGK_Set, S(9010), L({get, S(9000), 1171})}), L({ensure, S(9000), 2000, 11})});
    const C stop = L({TAGK_Lambda, L({S(9000)}),
        L({TAGK_Set, S(9011), L({get, S(9000), 1172})}), L({set, S(9000), 2000, 99})});
    const C message = L({TAGK_Lambda, L({S(9000), S(9001)}), L({set, S(9000), 2000, S(9001)})});
    for (const auto& [sym, code] : std::vector<std::pair<SYMID, C>>{{2, start}, {3, stop}, {4, message}})
    {
        auto cells = PpairBuild({code});
        CRef proc = RefEval(cells.get(), parent.get());
        parent->RefAddBinding(sym, &proc);
    }
    ALO actor;
    GUARD guard;
    guard.palo = &actor;
    JOBSCRIPT job;
    job.cid = 0xA9;
    job.pguard = &guard;
    CMDLIST first, second;
    first.pframe = frame;
    second.pframe = PframeNew();
    int failures = 0;
    auto check = [&](const char* name, bool ok) {
        std::printf("job script %-58s %s\n", name, ok ? "ok" : "FAIL");
        failures += !ok;
    };
    auto option = [&](int bif, int id, const CRef* value = nullptr) {
        return g_splicegame.pfnRefOption(bif, &job, id, value);
    };
    job.SetTarget(&first, 0, nullptr);
    CRef seen;
    parent->FFindBinding(9010, 0, &seen);
    check("parent start handler receives job and resolves its actor", seen.m_pbasic == &actor);
    check("job retains source frame and owns private bindings", job.pframe == frame && job.psidebag && !actor.psidebag);
    check("command-list getter reads the active source", option(111, 1172).m_pbasic == &first);
    CMD command;
    command.op = 4;
    command.oid = 152647;
    job.Execute(&command);
    check("command4 passes its inline word through event4", option(111, 2000).m_n == 152647);
    CRef value;
    value.SetS32(7);
    option(108, 2001, &value);
    value.SetS32(8);
    option(108, 2001, &value);
    check("add bindings shadow older private values", option(111, 2001).m_n == 8);
    value.SetS32(9);
    option(109, 2001, &value);
    check("ensure updates the existing private binding", option(111, 2001).m_n == 9);
    option(110, 2002, &value);
    check("setting an absent private binding does not create it", option(111, 2002).m_tagk == TAGK_Nil);
    job.SetTarget(&second, 0, nullptr);
    parent->FFindBinding(9011, 0, &seen);
    check("old stop handler sees its source before replacement", seen.m_pbasic == &first);
    check("replacement releases old frame and private bindings", job.pframe == second.pframe && !job.psidebag);
    job.SetTarget(nullptr, 0, nullptr);
    check("clearing the source releases its frame", !job.pframe && !job.pcmdlist);
    return failures;
}
}

int SpliceSelfTestCommand()
{
    g_splicediag = SPLICEDIAG{};
    auto pframe = PframeNew();
    const auto S = C::Sym;
    const auto B = C::Bif;
    const auto L = C::L;
    struct TEST { const char* pchz; std::vector<C> code; std::string expect; };
    std::vector<TEST> atest = {
        {"(+ 1 2 3)", {L({B(BIFK_Add), 1, 2, 3})}, "6"},
        {"(+ 1 2.5)", {L({B(BIFK_Add), 1, 2.5f})}, "3.5"},
        {"(- 7)", {L({B(BIFK_Subtract), 7})}, "7"},
        {"(* 2 3)", {L({B(BIFK_Multiply), 2, 3})}, "6"},
        {"(/ 7 2)", {L({B(BIFK_Divide), 7, 2})}, "3"},
        {"(begin (define f (lambda (x . r) (+ x (length r)))) (f 10 1 2 3))",
         {L({TAGK_Begin, L({TAGK_Define, S(1), L({TAGK_Lambda, L({S(2), TAGK_Dot, S(3)}), L({B(BIFK_Add), S(2), L({B(BIFK_Length), S(3)})})})}),
             L({S(1), 10, 1, 2, 3})})}, "13"},
        {"(let ((a 2) (b 5)) (* a b))", {L({TAGK_Let, L({L({S(4), 2}), L({S(5), 5})}), L({B(BIFK_Multiply), S(4), S(5)})})}, "10"},
        {"(begin (define i 0) (while (< i 5) (set! i (+ i 1))) i)",
         {L({TAGK_Begin, L({TAGK_Define, S(6), 0}), L({TAGK_While, L({B(BIFK_Less), S(6), 5}), L({TAGK_Set, S(6), L({B(BIFK_Add), S(6), 1})})}), S(6)})}, "5"},
        {"(cond ((< 2 1) 1) (else 2))", {L({TAGK_Cond, L({L({B(BIFK_Less), 2, 1}), 1}), L({TAGK_Else, 2})})}, "2"},
        {"(case 3 ((1 2) 10) ((3 4) 20) (else 30))", {L({TAGK_Case, 3, L({L({1, 2}), 10}), L({L({3, 4}), 20}), L({TAGK_Else, 30})})}, "20"},
        {"(map (lambda (x) (* x x)) (list 1 2 3))", {L({B(BIFK_Map), L({TAGK_Lambda, L({S(7)}), L({B(BIFK_Multiply), S(7), S(7)})}), L({B(BIFK_List), 1, 2, 3})})}, "(1 4 9)"},
        {"(and 1 (not 1))", {L({TAGK_And, 1, L({B(BIFK_Not), 1})})}, "#f"},
        {"(if (= 1 1) 5 6)", {L({TAGK_If, L({B(BIFK_IntEqual), 1, 1}), 5, 6})}, "5"},
        {"(quote (1 2))", {L({TAGK_Quote, L({1, 2})})}, "(1 2)"},
    };
    int cfail = 0;
    for (TEST& test : atest)
    {
        auto ppair = PpairBuild(test.code);
        CRef ref = RefEval(ppair.get(), pframe.get());
        std::string s;
        if (ref.m_tagk == TAGK_S32) s = std::to_string(ref.m_n);
        else if (ref.m_tagk == TAGK_F32) { char ach[32]; std::snprintf(ach, sizeof(ach), "%g", ref.m_g); s = ach; }
        else if (ref.m_tagk == TAGK_Bool) s = ref.m_bool ? "#t" : "#f";
        else if (ref.m_tagk == TAGK_Pair)
        {
            s = "(";
            for (CPair* p = ref.m_ppair.get(); p; p = p->m_ppairNext.get())
                s += (s.size() > 1 ? " " : "") + std::to_string(p->m_ref.m_n);
            s += ")";
        }
        else s = PchzFromTagk(ref.m_tagk);
        const bool fOk = s == test.expect;
        cfail += !fOk;
        std::printf("%-70s => %-8s %s\n", test.pchz, s.c_str(), fOk ? "ok" : ("FAIL, expected " + test.expect).c_str());
    }
    std::printf("%d of %zu passed\n", static_cast<int>(atest.size()) - cfail, atest.size());
    cfail += MathSelfTest();
    cfail += JobScriptSelfTest();
    g_gc.ShutDown();
    return cfail == 0 ? 0 : 1;
}
