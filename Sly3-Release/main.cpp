// Sly3-Release: Sly 3: Honor Among Thieves as a native PC port, forked from Sly2-Release.
// Working on Sly 3's disc: --list-keys, --list-files, --dump-world, the movie commands and the boot (the two logo
// movies; sly3/main3.cpp). Every other command is Sly 2's copied code and is refused until it is ported
// (FSly3Unported below).

#include "bis.h"
#include "display.h"
#include "brx.h"
#include "cls.h"
#include "disc.h"
#include "readtrace.h"
#include "sw.h"
#include "game.h"
#include "scene.h"
#include "view.h"
#include "moviegl.h"
#include "fmv.h"
#include "sound.h"
#include "splicecheck.h"
#include "animtest.h"
#include "sly3/main3.h"
#include "sly3/game3.h"
#include "sly3/scene3.h"
#include "sly3/screen3.h"
#include "sly3/hud3.h"
#include "sly3/splice3.h"
#include "sly3/text3.h"
#include "sly3/scpt3.h"
#include "sly3/scptrun3.h"
#include "sly3/card3.h"
#include "../platform/platform.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <map>
#include <string>

int CrvCheck(const std::string& pathTrace, const std::string& pathRam, const std::string& world);  // cmcheck.cpp
int ActorCheck(const std::string& pathTrace, const std::string& ranges, const std::string& pathRam);  // cmcheck.cpp
int CmCheck(const std::string& pathTrace, const std::string& pathRam, const std::string& ranges, bool fFree, bool fVerbose);  // cmcheck.cpp

namespace
{
std::string DiscPath(const std::string& explicitDisc)
{
    if (!explicitDisc.empty()) return explicitDisc;
    return cane::utf8(cane::dataDirectory() / "disc.iso");
}

std::string g_ramPath;
DUMPWORLD3OPTS g_dumpWorld3;
std::string g_ram3;   // --ram3 (--view-world3)
bool g_fRam3Pools = false;   // --ram3-pools (--view-world3)
bool g_fRam3Pose = false;    // --ram3-pose (--view-world3)
int g_nRam3Hud = 0;          // --ram3-hud slots|native (--view-world3)
bool s_fSkipLogos3 = false;  // --skip-logos (the game: no logo movies)
int s_iworld3 = -1;          // --world3 W (the game: the world the boot queues; default 35)

// The commands of Sly 2's copied code that read Sly 2's formats or addresses: refused until ported to Sly 3
// (TODO(sly3): each moves out of this list as its part is ported).
bool FSly3Unported(const char* command)
{
    static const char* const s_asz[] = {"vt-dump", "phys-check", "actor-check", "crv-check", "cm-check", "view-world",
                                        "anim-test", "anim-pose", "ai-free", "anim-run", "anim-ref", "parse-splice",
                                        "eval-splice",
                                        "test-load"};
    for (const char* sz : s_asz)
        if (!std::strcmp(sz, command)) return true;
    return false;
}

int TestLoad(const std::string& worldPath, const std::string& mapPath, bool fVerbose)
{
    std::ifstream in(cane::path(worldPath), std::ios::binary);
    std::vector<uint8_t> ab((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SCENE scene;
    std::string error;
    const bool fOk = FLoadSceneFromMap(ab, mapPath, scene, fVerbose, error);
    std::printf("texture blocks %d, bulk blocks %d: CLUTs %d BMPs %d SAAs %d shaders %d (filtered %d, shared %d)\n",
                scene.cblockShd, g_shdstats.cbulk, g_shdstats.cclut, g_shdstats.cbmp, g_shdstats.csaa, g_shdstats.cshd,
                g_shdstats.cfiltered, g_shdstats.cshared);
    std::printf("globsets %d: globs %d (types %d/%d/%d/%d, filtered %d), instances %d, skinned %d, vertices %d, "
                "triangles %d, cel edges %d, effect strips %d, entries without shader %d\n",
                g_globstats.cglobset, g_globstats.cglob, g_globstats.cmeshk[0], g_globstats.cmeshk[1], g_globstats.cmeshk[2],
                g_globstats.cmeshk[3], g_globstats.cfiltered, g_globstats.cinstance, g_globstats.cskinned, g_globstats.cvtx,
                g_globstats.ctri, g_globstats.cedge, g_globstats.cfx, g_globstats.cnoshd);
    {
        int csub = 0, csaa = 0; std::map<int, int> mpcid;
        for (const SCENEGLOBSET& sgs : scene.aglobset) for (const GLOB& g : sgs.pglobset->aglob) for (const SUBGLOB& sub : g.asubglob) { ++csub; if (sub.psaa) { ++csaa; ++mpcid[sub.psaa->cid]; } }
        std::printf("entries %d, animated %d:", csub, csaa);
        for (auto [cid, c] : mpcid) std::printf(" cid %d x%d", cid, c);
        std::printf("\n");
        std::map<int, int> mpall; for (const auto& psaa : g_shds.asaa) ++mpall[psaa->cid];
        std::map<std::string, int> mpk;
        for (const SCENEGLOBSET& sgs : scene.aglobset) for (const GLOB& g : sgs.pglobset->aglob)
        {
            char sz[96];
            if (g.meshk >= 2) for (const SUBFX& fx : g.asubfx) { std::snprintf(sz, sizeof(sz), "meshk %d rp %d shdk %d grfshd 0x%X rgba %08X %08X", g.meshk, g.rp, fx.pshd ? fx.pshd->shdk : -1, fx.pshd ? fx.pshd->grfshd : 0, fx.pshd ? fx.pshd->argba[0] : 0, fx.pshd ? fx.pshd->argba[1] : 0); ++mpk[sz]; }
            else for (const SUBGLOB& sub : g.asubglob) if (sub.pshd && (sub.pshd->shdk == 2 || sub.pshd->shdk == 3 || g.rp == 11 || g.rp == 13 || g.rp == 15 || g.rp == 18 || g.rp == 7 || g.rp == 8)) { std::snprintf(sz, sizeof(sz), "meshk %d rp %d shdk %d grfshd 0x%X rgba %08X %08X %08X tex %zu", g.meshk, g.rp, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0], sub.pshd->argba[1], sub.pshd->argba[2], sub.pshd->atex.size()); ++mpk[sz]; }
        }
        for (auto& [k, c] : mpk) std::printf("  %s x%d\n", k.c_str(), c);
        int crp[32] = {};
        for (const SCENEGLOBSET& sgs : scene.aglobset) for (const GLOB& g : sgs.pglobset->aglob) ++crp[g.rp & 31];
        std::printf("rp histogram:"); for (int i = 0; i < 32; ++i) if (crp[i]) std::printf(" %d:%d", i, crp[i]); std::printf("\n");
        std::printf("SAA classes:"); for (auto [cid, c] : mpall) std::printf(" %d x%d", cid, c); std::printf("\n");
    }
    {
        int celem = 0, cext = 0;
        for (const auto& pso : scene.aso) { celem += int(pso->aelem.size()); cext += pso->fExternal; }
        std::printf("screen objects %zu (external %d), elements %d\n", scene.aso.size(), cext, celem);
        int cq = 0, cqShd = 0, cqBmp = 0;
        std::function<void(const std::vector<SELEM>&)> walk = [&](const std::vector<SELEM>& ae) {
            for (const SELEM& e : ae)
            {
                if (e.kind == SELEMK_Image || e.kind == SELEMK_Quad)
                {
                    ++cq;
                    bool fShd = false;
                    for (const auto& pshd : g_shds.ashd) if (pshd->id == e.idTexture) fShd = true;
                    cqShd += fShd;
                    cqBmp += g_shds.mpidbmp.count(e.idTexture) ? 1 : 0;
                    if (cq <= 5) std::printf("  quad kind %u id 0x%X rect %d %d %d %d shd %d\n", e.kind, e.idTexture, e.x0, e.y0, e.x1, e.y1, int(fShd));
                }
                if (e.kind == SELEMK_Set) walk(e.asub);
            }
        };
        for (const auto& pso : scene.aso) walk(pso->aelem);
        std::printf("  quads %d: with a shader of that id %d, a bitmap %d\n", cq, cqShd, cqBmp);
        for (const FONTBRX& f : FindFonts(scene.aso))
        {
            std::printf("  font: object 0x%X element %d, %zu glyphs:", f.pso->oid, f.ifont, f.pset->aul.size());
            if (f.pset->aul.size() < 40) for (uint32_t c : f.pset->aul) std::printf(" %c", char(c));
            std::printf("\n");
            std::map<uint32_t, int> mprgba; std::map<int, int> mpprim;
            for (const SELEM& g : f.pset->asub) for (const SPOLY& p : g.apoly) { ++mprgba[p.rgba]; ++mpprim[p.primitive * 1000 + p.control]; }
            for (auto [k, c] : mprgba) std::printf("    rgba %08X x%d\n", k, c);
            for (auto [k, c] : mpprim) std::printf("    prim %d control %d x%d\n", k / 1000, k % 1000, c);
            const SELEM* pa = f.PglyphFromCh('A');
            if (pa) for (const SPOLY& p : pa->apoly) { std::printf("    A poly prim %d cv %zu:", p.primitive, p.axy.size()); for (auto& v : p.axy) std::printf(" %d,%d", v[0], v[1]); std::printf("\n"); }
            std::printf("    width A %.1f\n", f.DxFromCh('A'));
        }
    }
    std::printf("strips %d (starting at an odd vertex: %d)\n", g_globstats.cstrip, g_globstats.cstripOdd);
    if (!fOk) { std::printf("FAILED: %s\n", error.c_str()); return 1; }
    std::printf("all blocks end where the map says\n");
    if (!g_ramPath.empty())
    {
        const int c = CApplyRamTransforms(scene, g_ramPath);
        std::printf("reference RAM: %d of %zu globsets located\n", c, scene.aglobset.size());
        if (fVerbose)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                if (sgs.fTransform)
                    std::printf("  glob at %9zu -> 0x%06X pos %.1f %.1f %.1f\n", sgs.ib, sgs.vaGlob, sgs.amatGlob[0][3][0],
                                sgs.amatGlob[0][3][1], sgs.amatGlob[0][3][2]);
    }
    return 0;
}

int DumpWorld(const FK& fk, const std::string& out)
{
    std::vector<uint8_t> ab = g_disc.ReadWorld(fk);
    std::printf("world id %u: %zu bytes decompressed\n", fk.id, ab.size());
    if (!out.empty())
    {
        std::ofstream(cane::path(out), std::ios::binary).write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()));
        std::printf("written to %s\n", out.c_str());
    }
    CBinaryInputStream bis;
    bis.OpenBuffer(std::move(ab));
    const int cgroup = bis.S16Read();
    for (int i = 0; i < cgroup; ++i)
    {
        char name[64];
        bis.ReadBytes(name, sizeof(name));
        const int32_t a = bis.S32Read(), b = bis.S32Read(), c = bis.S32Read(), d = bis.S32Read(), cinst = bis.S32Read();
        std::printf("group %3d %-40.64s %d %d %d %d instances %d\n", i, name, a, b, c, d, cinst);
    }
    return 0;
}

// Per ALO: oid and each transform record's world translation (to compare with the reference's records +0x70)
void DumpXfs(const SW* psw, FILE* pf)
{
    for (int cid = 0x30; cid < 0x6D; ++cid)
        for (const LO* plo : psw->mpcidaplo[cid])
        {
            const ALO* palo = static_cast<const ALO*>(plo);
            std::fprintf(pf, "%d %zu", plo->oid, palo->axf.size());
            if (std::getenv("DUMPXF_OPTS"))
            {
                std::fprintf(pf, " opts");
                for (const OPT& o : plo->aopt)
                {
                    std::fprintf(pf, " %d:%d", o.eopid, o.aval.empty() ? 0 : o.aval[0].n);
                    if (!o.aval.empty() && !o.aval[0].an.empty())
                    {
                        std::fprintf(pf, "[");
                        for (int16_t k : o.aval[0].an) std::fprintf(pf, "%d,", k);
                        std::fprintf(pf, "]");
                    }
                }
                std::fprintf(pf, " parent %d", plo->paloParent ? plo->paloParent->oid : -1);
            }
            for (const auto& pxf : palo->axf)
                std::fprintf(pf, " %.1f,%.1f,%.1f", pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z);
            std::fputc('\n', pf);
        }
}

void DumpObjects(const SW* psw, FILE* pf)
{
    std::fprintf(pf, "# cid class oid parent-oid group name\n");
    for (int cid = 0; cid < CID_Max; ++cid)
        for (const LO* plo : psw->mpcidaplo[cid])
        {
            // (world files carry no object names: objects are known by oid; the class name is printed)
            const std::string name = "-";
            std::fprintf(pf, "0x%02X %s %d %d %d %s", cid, g_acls[cid].name, plo->oid,
                         plo->paloParent ? plo->paloParent->oid : -1, plo->igrp, name.c_str());
            if (std::getenv("SLY2_DUMP_EXTRA"))
            {
                const LO* proot = plo;
                while (proot->paloParent) proot = proot->paloParent;
                const WE* pwe = proot->igrp >= 0 && proot->igrp < int(psw->awe.size()) ? &psw->awe[proot->igrp] : nullptr;
                std::fprintf(pf, " root %d clone %d x5c %d used %d cinst %d frame %d", proot->oid, proot->iclone,
                             pwe ? pwe->x5C : -9, pwe ? pwe->cUsed : -9, pwe ? pwe->cinst : -9, plo->pframe ? 1 : 0);
                std::fprintf(pf, " grfLoad 0x%x st %d removed %d inworld %d mask 0x%x radius %g opts", plo->grfLoad, plo->st, plo->fRemoved,
                             plo->fInWorld, plo->bMask, plo->sRadius);
                for (const OPT& o : plo->aopt)
                {
                    std::fprintf(pf, " %d:%d", o.eopid, o.aval.empty() ? 0 : o.aval[0].n);
                    if (!o.aval.empty() && !o.aval[0].an.empty())
                    {
                        std::fprintf(pf, "[");
                        for (int16_t k : o.aval[0].an) std::fprintf(pf, "%d,", k);
                        std::fprintf(pf, "]");
                    }
                }
            }
            std::fputc('\n', pf);
        }
}
}

int PhysCheckMain(const std::vector<std::string>& args);  // physcheck.cpp

#if defined(__linux__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
namespace
{
// a crash's stack on stderr (the release has no debugger): the addresses for addr2line
void OnCrashSignal(int sig)
{
    void* apv[64];
    const int c = backtrace(apv, 64);
    const char sz[] = "Sly3-Release: crashed, stack:\n";
    (void)!write(2, sz, sizeof(sz) - 1);
    backtrace_symbols_fd(apv, c, 2);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
}
#endif

int main(int argc, char** argv)
{
#if defined(__linux__)
    std::signal(SIGSEGV, OnCrashSignal);
    std::signal(SIGABRT, OnCrashSignal);
#endif
    std::string dumpXf;
    GAMEOPTS go;
    bool fGame = false;
    bool fWorldGiven = false;   // (no --world with the launcher's --data-dir: the game as released, from the boot)
    std::string disc, out, trace, dumpObjects, dirMaps = "build/loadmaps";
    bool fVerbose = false;
    int idSong = -1, istem = 1;
    double tSeconds = -1.0;  // (unset: 30 s for the sound commands, the whole movie)
    const char* command = nullptr;
    const char* arg = nullptr;
    int filter = -2;
    bool fDumpObjects = false;
    const char* arg2 = nullptr;
    const char* arg3 = nullptr;
    int animRefB = 0;
    VIEWOPTS vo;
    std::string cmRanges = "2DDED0+6A0,2DFBC0+2240,FB1580+1100,FB37E0+80,140C9F0+600,1486000+3400";
    bool fCmFree = false;
    struct { bool fAspect = false; DISPASPECT aspect = DISPASPECT_Fit; int internalHeight = -1, frameRate = -1, vsync = -1, window = -1, wideBackgrounds = -1, hqLogo = -1; float drawDistance = -1; } dispo;
    std::vector<std::string> rest, physArgs;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        // the launcher's runtime contract: --data-dir <installations/sly3> --user-dir <profile>/sly3
        else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) cane::dataOverride = std::filesystem::absolute(cane::path(argv[++i]));
        else if (!std::strcmp(argv[i], "--user-dir") && i + 1 < argc) cane::userOverride = std::filesystem::absolute(cane::path(argv[++i]));
        // display options for this run (over system_settings.dat; display.h)
        else if (!std::strcmp(argv[i], "--aspect") && i + 1 < argc)
        {
            if (!FParseAspect(argv[++i], dispo.aspect)) { std::fprintf(stderr, "bad --aspect (fit, 4:3, 16:9, 16:10, ps2-4:3, ps2-16:9)\n"); return 2; }
            dispo.fAspect = true;
        }
        else if (!std::strcmp(argv[i], "--internal-res") && i + 1 < argc) dispo.internalHeight = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frame-rate") && i + 1 < argc) dispo.frameRate = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--draw-distance") && i + 1 < argc) dispo.drawDistance = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--vsync") && i + 1 < argc) dispo.vsync = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--wide-backgrounds") && i + 1 < argc) dispo.wideBackgrounds = std::atoi(argv[++i]);   // (the launcher's option)
        else if (!std::strcmp(argv[i], "--hq-logo") && i + 1 < argc) dispo.hqLogo = std::atoi(argv[++i]);   // (QoL: the launcher's logo in the front end)
        else if (!std::strcmp(argv[i], "--window-mode") && i + 1 < argc) dispo.window = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--list-keys")) command = "list-keys";
        else if (!std::strcmp(argv[i], "--list-files")) command = "list-files";
        else if (!std::strcmp(argv[i], "--groups")) g_dumpWorld3.fGroups = true;    // (--dump-world)
        else if (!std::strcmp(argv[i], "--objects")) g_dumpWorld3.fObjects = true;  // (--dump-world)
        else if (!std::strcmp(argv[i], "--census")) g_dumpWorld3.fCensus = true;
        else if (!std::strcmp(argv[i], "--aseg-check3") && i + 1 < argc) g_dumpWorld3.asegCheck = argv[++i];  // (--load-world: sly3/anim3.h)
        else if (!std::strcmp(argv[i], "--fields") && i + 1 < argc) g_dumpWorld3.fields = argv[++i];   // (--load-world: one line per object)
        else if (!std::strcmp(argv[i], "--arrival") && i + 1 < argc)               // (--load-world: GAME +0x1498:+0x14A0)
            std::sscanf(argv[++i], "%d:%d", &g_dumpWorld3.keyA, &g_dumpWorld3.keyB);
        else if (!std::strcmp(argv[i], "--vt-dump") && i + 1 < argc) { command = "vt-dump"; arg = argv[++i]; }  // (vtaudit.cpp)
        else if (!std::strcmp(argv[i], "--maps") && i + 1 < argc) dirMaps = argv[++i];
        else if (!std::strcmp(argv[i], "-v")) fVerbose = true;
        else if (!std::strcmp(argv[i], "--blends") || !std::strcmp(argv[i], "--ai") || !std::strcmp(argv[i], "--ai-carry")) {}  // (anim-ref)
        else if (!std::strcmp(argv[i], "--splice-selftest")) command = "splice-selftest";
        else if (!std::strcmp(argv[i], "--parse-splice") && i + 1 < argc) { command = "parse-splice"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--eval-splice") && i + 1 < argc) { command = "eval-splice"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--dump-world") && i + 1 < argc) { command = "dump-world"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--skip-logos")) s_fSkipLogos3 = true;                      // (sly3/game3.cpp)
        else if (!std::strcmp(argv[i], "--world3") && i + 1 < argc) s_iworld3 = std::atoi(argv[++i]);  // (the boot's world)
        else if (!std::strcmp(argv[i], "--view-world3") && i + 1 < argc) { command = "view-world3"; arg = argv[++i]; }  // (sly3/view3.cpp)
        else if (!std::strcmp(argv[i], "--ram3") && i + 1 < argc) g_ram3 = argv[++i];  // (--view-world3)
        else if (!std::strcmp(argv[i], "--ram3-pools")) g_fRam3Pools = true;  // (--view-world3)
        else if (!std::strcmp(argv[i], "--ram3-pose")) g_fRam3Pose = true;  // (--view-world3)
        else if (!std::strcmp(argv[i], "--ram3-hud") && i + 1 < argc) { ++i; g_nRam3Hud = !std::strcmp(argv[i], "native") ? 2 : 1; }  // (--view-world3)
        else if (!std::strcmp(argv[i], "--hud-dump3") && i + 1 < argc) { command = "hud-dump3"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--splice-world") && i + 1 < argc) { command = "splice-world"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--scpt-world") && i + 1 < argc) { command = "scpt-world"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--screen-world3") && i + 1 < argc) { command = "screen-world3"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--text-test3") && i + 3 < argc) { command = "text-test3"; arg = argv[++i]; arg2 = argv[++i]; arg3 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--scpt-run") && i + 2 < argc) { command = "scpt-run"; arg = argv[++i]; arg2 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--scpt-dump") && i + 1 < argc) { command = "scpt-dump"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--card-test3")) { command = "card-test3"; if (!arg) arg = ""; }
        else if (!std::strcmp(argv[i], "--card-press") && i + 1 < argc) arg = argv[++i];  // (--card-test3: SQUARE@F,...)
        else if (!std::strcmp(argv[i], "--oid") && i + 1 < argc) arg2 = argv[++i];  // (--scpt-dump)
        else if (!std::strcmp(argv[i], "--list-text") && i + 1 < argc) { command = "list-text"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--grep") && i + 1 < argc) arg2 = argv[++i];  // (--list-text)
        else if (!std::strcmp(argv[i], "--load-world") && i + 1 < argc) { command = "load-world"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--load-trace") && i + 1 < argc) trace = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-xf") && i + 1 < argc) dumpXf = argv[++i];
        else if (!std::strcmp(argv[i], "--world") && i + 1 < argc)
        {
            fWorldGiven = true;
            // W or W:TASK (a job start, like the reference's --warp)
            std::string w = argv[++i];
            const size_t colon = w.find(':');
            if (colon != std::string::npos) { go.task = std::atoi(w.c_str() + colon + 1); w = w.substr(0, colon); }
            go.world = w;
            fGame = true;
        }
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) { go.cframe = std::atoi(argv[++i]); fGame = true; }
        else if (!std::strcmp(argv[i], "--headless")) { go.fHeadless = true; fGame = true; }
        else if (!std::strcmp(argv[i], "--fixed-dt")) go.fFixedDt = true;
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) go.save = argv[++i];
        else if (!std::strcmp(argv[i], "--print-progress") && i + 1 < argc) { go.aprintProgress.push_back(argv[++i]); fGame = true; }
        else if (!std::strcmp(argv[i], "--frame-dump") && i + 1 < argc) { go.frameDump = argv[++i]; fGame = true; }
        else if (!std::strcmp(argv[i], "--sly-start") && i + 1 < argc) { go.slyStart = argv[++i]; fGame = true; }
        else if (!std::strcmp(argv[i], "--sly-ref") && i + 1 < argc) go.slyRef = argv[++i];
        else if (!std::strcmp(argv[i], "--gang-start") && i + 1 < argc) { go.gangStart = argv[++i]; fGame = true; }
        else if (!std::strcmp(argv[i], "--gang-ref") && i + 1 < argc) go.gangRef = argv[++i];
        else if (!std::strcmp(argv[i], "--press-vblank-base") && i + 1 < argc) go.vbBase = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc)
        {
            if (!FParseJoyPresses(argv[++i], go.apress)) { std::fprintf(stderr, "bad --press\n"); return 2; }
        }
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump-objects"))
        {
            fDumpObjects = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') dumpObjects = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--test-load") && i + 2 < argc) { command = "test-load"; arg = argv[++i]; arg2 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--ram") && i + 1 < argc) g_ramPath = argv[++i];
        else if (!std::strcmp(argv[i], "--anim-test") && i + 1 < argc) { command = "anim-test"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--anim-pose") && i + 2 < argc) { command = "anim-pose"; arg = argv[++i]; arg2 = argv[++i]; }
        else if (!std::strcmp(argv[i], "--ai-free") && i + 3 < argc) { command = "ai-free"; arg = argv[++i]; arg2 = argv[++i]; animRefB = ++i; }
        else if (!std::strcmp(argv[i], "--anim-run") && i + 3 < argc) { command = "anim-run"; arg = argv[++i]; arg2 = argv[++i]; animRefB = ++i; }
        else if (!std::strcmp(argv[i], "--anim-ref") && i + 3 < argc) { command = "anim-ref"; arg = argv[++i]; arg2 = argv[++i]; animRefB = ++i; }
        else if ((!std::strcmp(argv[i], "--frames") || !std::strcmp(argv[i], "--dump") || !std::strcmp(argv[i], "--then")) && i + 1 < argc) ++i;  // (anim-test / anim-ref)
        else if (!std::strcmp(argv[i], "--cm-check") && i + 1 < argc) { command = "cm-check"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--cm-ranges") && i + 1 < argc) cmRanges = argv[++i];
        else if (!std::strcmp(argv[i], "--cm-free")) fCmFree = true;
        else if (!std::strcmp(argv[i], "--crv-check") && i + 1 < argc) { command = "crv-check"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--actor-check") && i + 1 < argc) { command = "actor-check"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--view-world") && i + 1 < argc) { command = "view-world"; vo.iworld = std::atoi(argv[++i]); }
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) vo.screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--camera") && i + 1 < argc) vo.camera = argv[++i];
        else if (!std::strcmp(argv[i], "--camera-ram") && i + 1 < argc) vo.cameraRam = argv[++i];
        else if (!std::strcmp(argv[i], "--loadmaps") && i + 1 < argc) vo.loadmaps = argv[++i];
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &vo.width, &vo.height);
        else if (!std::strcmp(argv[i], "--msaa") && i + 1 < argc) vo.msaa = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-cel")) vo.fNoCel = true;
        else if (!std::strcmp(argv[i], "--from-map")) vo.fFromMap = true;
        else if (!std::strcmp(argv[i], "--no-fog")) vo.fNoFog = true;
        else if (!std::strcmp(argv[i], "--no-shadow")) vo.fNoShadow = true;
        else if (!std::strcmp(argv[i], "--time") && i + 1 < argc) vo.tAnim = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--text-test") && i + 2 < argc) { command = "view-world"; vo.iworld = std::atoi(argv[++i]); vo.textTest = argv[++i]; }
        else if (!std::strcmp(argv[i], "--text-size") && i + 1 < argc) vo.textSize = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--hud-ram") && i + 1 < argc) vo.hudRam = argv[++i];
        else if (!std::strcmp(argv[i], "--screens") && i + 1 < argc) vo.screens = argv[++i];
        else if (!std::strcmp(argv[i], "--bench") && i + 1 < argc) vo.cframeBench = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--fade") && i + 1 < argc) vo.fade = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--letterbox") && i + 1 < argc) vo.letterbox = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--wipe") && i + 1 < argc) vo.wipe = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--approx-light")) vo.fApproxLight = true;
        else if (!std::strcmp(argv[i], "--ram-lights")) vo.fRamLights = true;
        else if (!std::strcmp(argv[i], "--particles") && i + 1 < argc) vo.tParticles = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--burst") && i + 1 < argc) vo.oidBurst = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--list-sounds") && i + 1 < argc) { command = "list-sounds"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-music") && i + 1 < argc) { command = "play-music"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-sfx") && i + 1 < argc) { command = "play-sfx"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-dialog") && i + 1 < argc) { command = "play-dialog"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--play-movie") && i + 1 < argc) { command = "play-movie"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--list-movies")) command = "list-movies";
        else if (!std::strcmp(argv[i], "--phys-check"))  // physcheck.cpp: FUNC PREFIX [COUNT] (the rest of the line)
        {
            command = "phys-check";
            while (i + 1 < argc) physArgs.emplace_back(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--view-movie") && i + 1 < argc) { command = "view-movie"; arg = argv[++i]; }
        else if (!std::strcmp(argv[i], "--song") && i + 1 < argc) idSong = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--stem") && i + 1 < argc) istem = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) tSeconds = std::atof(argv[++i]);
        else rest.emplace_back(argv[i]);
    }
    if (filter != -2) g_nFilterCur = filter;  // (--test-load)
    cane::configureRuntime(rest);
    if (command && FSly3Unported(command))
    {
        std::fprintf(stderr, "Sly3-Release: --%s is Sly 2's and is not ported to Sly 3 yet\n", command);
        return 2;
    }
    if (!command && (fWorldGiven || !go.slyStart.empty() || !go.gangStart.empty()))
    {
        std::fprintf(stderr, "Sly3-Release: the game loop (--world, --sly-start, ...) is not ported to Sly 3 yet\n");
        return 2;
    }
    if (command && !std::strcmp(command, "test-load")) return TestLoad(arg, arg2, fVerbose);
    if (command && !std::strcmp(command, "splice-selftest")) return SpliceSelfTestCommand();
    std::string error;
    if (!g_disc.FOpen(DiscPath(disc), error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }

    if (command && !std::strcmp(command, "list-keys")) return ListKeys3();
    if (command && !std::strcmp(command, "list-files")) return ListFiles3();
    int VtDumpCommand(const char*);  // vtaudit.cpp
    if (command && !std::strcmp(command, "vt-dump")) return VtDumpCommand(arg);
    if (command && !std::strcmp(command, "phys-check")) return PhysCheckMain(physArgs);
    if (command && !std::strcmp(command, "actor-check")) { InitClasses(); return ActorCheck(arg, cmRanges, g_ramPath); }
    if (command && !std::strcmp(command, "crv-check"))
    {
        InitClasses();
        return CrvCheck(arg, g_ramPath, go.world);
    }
    if (command && !std::strcmp(command, "cm-check"))
    {
        InitClasses();
        return CmCheck(arg, g_ramPath, cmRanges, fCmFree, fVerbose);
    }
    auto applyDisplayOverrides = [&]() {
        if (dispo.fAspect) g_display.aspect = dispo.aspect;
        if (dispo.internalHeight >= 0) g_display.internalHeight = dispo.internalHeight;
        if (dispo.frameRate >= 30) g_display.frameRate = dispo.frameRate;
        if (dispo.drawDistance > 0) g_display.drawDistance = dispo.drawDistance;
        if (dispo.vsync >= 0) g_display.fVsync = dispo.vsync != 0;
        if (dispo.wideBackgrounds >= 0) g_display.fWideBackgrounds = dispo.wideBackgrounds != 0;
        if (dispo.hqLogo >= 0) g_display.fHqLogo = dispo.hqLogo != 0;
        if (dispo.window >= 0 && dispo.window <= 2) g_display.window = DISPWINDOW(dispo.window);
    };
    applyDisplayOverrides();
    if (command && !std::strcmp(command, "view-world")) return ViewWorld(vo);
    if (command && !std::strcmp(command, "anim-test")) return AnimTestCommand(arg, argc, argv);  // animtest.cpp
    if (command && !std::strcmp(command, "anim-pose")) return AnimPoseCommand(arg, arg2, argc, argv);  // animref.cpp
    int AiFreeCommand(const char*, const char*, const char*, int, char**);  // animref.cpp
    if (command && !std::strcmp(command, "ai-free")) return AiFreeCommand(arg, arg2, argv[std::size_t(animRefB)], argc, argv);
    if (command && !std::strcmp(command, "anim-run")) return AnimRunCommand(arg, arg2, argv[std::size_t(animRefB)], argc, argv);
    if (command && !std::strcmp(command, "anim-ref")) return AnimRefCommand(arg, arg2, argv[std::size_t(animRefB)], argc, argv);
    if (command && !std::strcmp(command, "parse-splice")) return ParseSpliceCommand(arg, dirMaps, fVerbose);
    if (command && !std::strcmp(command, "eval-splice")) return EvalSpliceCommand(arg, dirMaps, fVerbose, out);
    if (command && !std::strcmp(command, "list-sounds")) return ListSoundsCommand(arg, dirMaps);
    if (command && !std::strcmp(command, "play-music")) return PlayMusicCommand(arg, dirMaps, idSong, tSeconds < 0 ? 30.0 : tSeconds, istem);
    if (command && !std::strcmp(command, "play-sfx")) return PlaySfxCommand(arg, dirMaps, idSong, tSeconds < 0 ? 30.0 : tSeconds);
    if (command && !std::strcmp(command, "play-dialog")) return PlayDialogCommand(arg, dirMaps, idSong, tSeconds < 0 ? 30.0 : tSeconds);
    if (command && !std::strcmp(command, "play-movie")) return PlayMovieCommand(arg, out, tSeconds < 0 ? 1e9 : tSeconds);
    if (command && !std::strcmp(command, "list-movies")) return ListMoviesCommand();
    if (command && !std::strcmp(command, "view-movie")) return ViewMovieCommand(arg, vo.screenshot, vo.tAnim, vo.width, vo.height);
    if (command && !std::strcmp(command, "view-world3"))
    {
        VIEW3OPTS v3;
        v3.world = arg;
        v3.ram = g_ram3;
        v3.screenshot = vo.screenshot;
        v3.camera = vo.camera;
        v3.width = vo.width;
        v3.height = vo.height;
        v3.fNoCel = vo.fNoCel;
        v3.fNoFog = vo.fNoFog;
        v3.fRamPools = g_fRam3Pools;
        v3.fRamPose = g_fRam3Pose;
        v3.nRamHud = g_nRam3Hud;
        return ViewWorld3(v3);
    }
    if (command && !std::strcmp(command, "splice-world")) return SpliceWorld3Command(arg, fVerbose);
    if (command && !std::strcmp(command, "scpt-world")) return ScptWorld3Command(arg, fVerbose);
    if (command && !std::strcmp(command, "screen-world3")) return ScreenWorld3Command(arg, fVerbose);
    if (command && !std::strcmp(command, "hud-dump3")) return HudDump3Command(arg);
    if (command && !std::strcmp(command, "text-test3")) return TextTest3Command(arg, arg2, arg3);
    if (command && !std::strcmp(command, "scpt-run")) return ScptRun3Command(arg, arg2, go.cframe > 0 ? go.cframe : 6000, fVerbose);
    if (command && !std::strcmp(command, "card-test3")) return CardTest3Command(arg ? arg : "", go.cframe > 0 ? go.cframe : 600);
    if (command && !std::strcmp(command, "scpt-dump")) return ScptDump3Command(arg, arg2 ? arg2 : "");
    if (command && !std::strcmp(command, "list-text")) return ListText3Command(arg, arg2 ? arg2 : "");
    if (command && !std::strcmp(command, "dump-world"))
    {
        g_dumpWorld3.out = out;
        return DumpWorld3Command(arg, g_dumpWorld3);  // sly3/main3.cpp
    }
    if (command && !std::strcmp(command, "load-world"))
    {
        g_dumpWorld3.out = out;
        return LoadWorld3Command(arg, g_dumpWorld3);  // sly3/main3.cpp
    }
    if (command && !std::strcmp(command, "dump-world-sly2"))  // (Sly 2's, kept for the port: TODO(sly3) remove)
    {
        // VA of a key in the executable (0x2A37E8), or SECTOR:CB
        FK fk;
        if (arg[0] == 'w')  // ("wN": the world record N, as --load-world N)
        {
            InitClasses();
            int filterDefault = 0;
            std::string err;
            if (!FResolveWorld(arg + 1, fk, filterDefault, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        }
        else if (const char* colon = std::strchr(arg, ':'))
        {
            fk.isector = static_cast<uint32_t>(std::strtoul(arg, nullptr, 0));
            fk.cb = static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 0));
        }
        else fk = FkFromRecord(g_disc.PbElf(static_cast<uint32_t>(std::strtoul(arg, nullptr, 0)), 0x40));
        return DumpWorld(fk, out);
    }
    if (command && !std::strcmp(command, "load-world"))
    {
        InitClasses();
        // W: a world record index (0..43; the records come from the boot world), or "boot" (id 93) / "frontend"
        // (id 57), the two worlds keyed in the executable.
        const std::string w = arg;
        FK fk;
        int filterDefault = 0;
        std::string err;
        if (!FResolveWorld(w, fk, filterDefault, err))
        {
            std::fprintf(stderr, "%s\n", err.c_str());
            return err.find("nothing to load") != std::string::npos ? 0 : 2;
        }
        auto psw = std::make_unique<SW>();
        psw->filter = filter != -2 ? filter : filterDefault;
        std::size_t cbRead = 0, cbFile = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const bool fOk = FLoadWorldFile(psw.get(), fk, trace, &cbRead, &cbFile);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr,
                     "world %s (id %u, filter %d): %s, read %zu bytes of %zu, %zu objects (%d loaded, %d filtered out, "
                     "%d missions kept, shared (k -1/-2) %d/%d skipped %d, %d proxies, %d size mismatches), %zu groups, %.0f ms\n",
                     w.c_str(), fk.id, psw->filter, fOk ? "loaded" : "FAILED", cbRead, cbFile,
                     psw->aploAll.size(), g_brxstats.cobj, g_brxstats.cskipFilter, g_brxstats.cmission,
                     g_brxstats.cshared1, g_brxstats.cshared2, g_brxstats.cskipShared, g_brxstats.cproxy,
                     g_brxstats.ccbMismatch, psw->awe.size(), ms);
        {
            std::size_t cframe = 0;
            for (const auto& plo : psw->aploAll) cframe += plo->pframe != nullptr;
            std::fprintf(stderr, "%zu objects with a splice frame\n", cframe);
        }
        if (const char* pszOid = std::getenv("SLY2_OWNER_OF"))
        {
            const OID oid = std::atoi(pszOid);
            for (const auto& plo : psw->aploAll)
                for (LO* pc : plo->aploOwned)
                    if (pc->oid == oid)
                        std::fprintf(stderr, "oid %d (%s) owned by %d (%s), clone %d\n", oid, g_acls[pc->Cid()].name, plo->oid,
                                     g_acls[plo->Cid()].name, plo->iclone);
        }
        if (!dumpXf.empty())
            if (FILE* pf = std::fopen(dumpXf.c_str(), "w")) { DumpXfs(psw.get(), pf); std::fclose(pf); }
        if (fDumpObjects)
        {
            FILE* pf = dumpObjects.empty() ? stdout : std::fopen(dumpObjects.c_str(), "w");
            if (pf)
            {
                DumpObjects(psw.get(), pf);
                if (pf != stdout) std::fclose(pf);
            }
        }
        return fOk ? 0 : 1;
    }
    if (!command)
    {
        // Sly 3: the boot as far as it is ported (orig3 0x1C6EA8: the logo movies; then world 35, m_ext: TODO(sly3))
        // TODO(sly3): the game loop; Sly 2's start below stays for the port.
        GAME3OPTS g3;   // sly3/game3.cpp: the boot, the transition to world 35 (m_ext), the main loop
        g3.fHeadless = go.fHeadless;
        g3.fSkipLogos = s_fSkipLogos3;
        g3.cframe = go.cframe;
        g3.screenshot = vo.screenshot;
        g3.frameDump = go.frameDump;
        g3.width = vo.width;
        g3.height = vo.height;
        g3.iworld = s_iworld3;
        g3.fFixedDt = go.fFixedDt;
        if (g_dumpWorld3.fCensus) g3.census = out;
        g3.apress = go.apress;
        for (JOYPRESS& press : g3.apress) press.frame -= go.vbBase;  // (--press-vblank-base: the reference's vblanks)
        return RunGame3(g3);
        // the launcher's start (--data-dir, no --world and no harness): the disc's boot sequence (logo, title,
        // menus) with the user's own cards only
        if (!fWorldGiven && !cane::dataOverride.empty() && go.slyStart.empty() && go.gangStart.empty() && go.frameDump.empty())
        {
            go.world = "boot";
            g_fDevCard = false;
        }
        // the game (default: the Paris hub): the PC options from <user-dir>/Saves/system_settings.dat
        if (!go.fHeadless)
        {
            std::string message;
            FLoadDisplaySettings(cane::userDirectory(), &message);
            std::printf("settings: %s\n", message.c_str());
            // the debug keys and the debug menu: the launcher's Settings > Advanced > Debug mode (<user-dir>/Debug
            // Mode.ini, which the launcher copies there at each start; not headless, where SLY2N_PCOPT=debugkeys=1)
            void ApplyPcDebugMode();   // pcinput.h
            ApplyPcDebugMode();
        }
        applyDisplayOverrides();
        {
            void ApplyPcInputEnv();   // pcinput.h (tests: SLY2N_PCOPT)
            ApplyPcInputEnv();
        }
        (void)fGame;
        go.screenshot = vo.screenshot;
        go.camera = vo.camera;
        go.cameraRam = vo.cameraRam;
        go.width = vo.width;
        go.height = vo.height;
        go.loadmaps = vo.loadmaps;
        for (JOYPRESS& press : go.apress) press.frame -= go.vbBase;  // (--press-vblank-base: the reference's vblanks)
        return RunGame(go);
    }
    std::fprintf(stderr,
                 "usage: Sly3-Release [--disc disc.iso | --data-dir dir] [--headless] [--skip-logos] [--frames N] [--screenshot f.png] [--frame-dump DIR:N] [--size WxH] [--world3 W]   (the game: the logos, then m_ext)\n"
                 "       | --list-keys | --list-files\n"
                 "       | --dump-world W|all [--groups] [--objects] [--out file]   (W: 0..39, a name (m_ext), a key address, SECTOR:CB)\n"
                 "       | --load-world W|all [--census] [--fields OIDS|all] [--arrival A:B] [--out file]   (the objects created)\n"
                 "       | --splice-world W|all [-v]   (the splice runtime's deserialize-and-evaluate test)\n"
                 "       | --splice-selftest | --list-text W [--grep text] | --scpt-world W|all [-v]\n"
                 "       | --screen-world3 W|all [-v] | --text-test3 W TEXT|#ID OUT.png | --hud-dump3 W\n"
                 "       | --scpt-dump W [--oid N]     (the scripts' command trees)\n"
                 "       | --scpt-run W OID[,OID] [--frames N] [-v]  (the executor with a test host; scptrun3.h)\n"
                 "       | --card-test3 [--card-press SQUARE@F,TRIANGLE@F] [--frames N]  (the boot's card check on --user-dir; card3.h)\n"
                 "       | --list-sounds W | --play-music W [--song ID] | --play-sfx W [--song ID] | --play-dialog W [--song ID]\n"
                 "       | --list-movies | --play-movie ID [--out frames-dir] [--seconds T] | --view-movie ID [--screenshot file --time T]\n");
    return 2;
}
