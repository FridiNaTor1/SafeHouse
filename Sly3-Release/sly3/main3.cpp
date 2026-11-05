// Sly3-Release's commands that already work on Sly 3's disc: --list-keys, --list-files, --dump-world, and the boot
// (the two logo movies). The rest of main.cpp is Sly 2's and is refused for now (main.cpp: FSly3Unported).

#include "main3.h"
#include "anim3.h"

#include "addr.h"
#include "card3.h"
#include "boot3.h"
#include "brx3.h"
#include "ids3.h"
#include "load3.h"

#include "../cls.h"
#include "../glob.h"
#include "../sw.h"

#include "../disc.h"
#include "../frontend.h"
#include "../fmv.h"
#include "../moviegl.h"
#include "../../platform/platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

namespace
{
// A world by index (0..39), name ("m_ext"), key address (0x2EE898) or SECTOR:CB
bool FResolveWorld3(const std::string& arg, FK& fk, std::string& name, std::string& error)
{
    const std::vector<WORLD3> aw = g_disc.Worlds();
    for (const WORLD3& w : aw)
        if (w.name == arg)
        {
            fk = w.fk;
            name = w.name;
            return true;
        }
    if (const std::size_t colon = arg.find(':'); colon != std::string::npos)
    {
        fk.isector = static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 0));
        fk.cb = static_cast<uint32_t>(std::strtoul(arg.c_str() + colon + 1, nullptr, 0));
        name = arg;
        return true;
    }
    char* pchEnd = nullptr;
    const unsigned long n = std::strtoul(arg.c_str(), &pchEnd, 0);
    if (pchEnd == arg.c_str() || *pchEnd)
    {
        error = "no world " + arg + " (--list-files)";
        return false;
    }
    for (const WORLD3& w : aw)
        if (static_cast<unsigned long>(w.iworld) == n || w.vaFk == n)
        {
            fk = w.fk;
            name = w.name;
            return true;
        }
    error = "no world " + arg + " (--list-files)";
    return false;
}

int DumpOneWorld(const std::string& name, const FK& fk, const DUMPWORLD3OPTS& o, bool fSummaryOnly, WORLDWALK3* pw)
{
    if (fk.cb == 0)
    {
        std::printf("world %s: no file\n", name.c_str());
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> ab = g_disc.ReadWorld(fk);
    if (!o.out.empty())
    {
        std::ofstream(cane::path(o.out), std::ios::binary).write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()));
        std::printf("written to %s\n", o.out.c_str());
    }
    WORLDWALK3 w;
    std::string error;
    const bool fOk = FWalkWorld3(ab, w, error);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (fSummaryOnly)
    {
        int cgroupSkipped = 0, cmatch = 0, cdata = 0;
        for (const GROUP3& g : w.agroup)
        {
            cgroupSkipped += g.fSkipped;
            if (!g.fSkipped) ++cdata, cmatch += g.ibEnd - g.ibStart == g.cb;
        }
        std::printf("%-14s id %3u %9zu bytes: %s at %9zu (tail %6zu), groups %3zu (skipped %3d, size ok %3d/%3d), "
                    "objects %5zu, classes %3zu, splice cells %7d, %.0f ms%s%s\n",
                    name.c_str(), fk.id, w.cbFile, fOk ? "walked" : "FAILED", w.ibEnd, w.cbFile - w.ibEnd, w.agroup.size(),
                    cgroupSkipped, cmatch, cdata, w.aobj.size(), w.mpcidc.size(), w.cpairSplice, ms, fOk ? "" : ": ",
                    error.c_str());
    }
    else
    {
        std::printf("world %s (id %u, sector 0x%X, %u bytes compressed)\n", name.c_str(), fk.id, fk.isector, fk.cb);
        PrintWorldWalk3(w, stdout, o.fGroups, o.fObjects);
        if (!fOk) std::printf("FAILED: %s\n", error.c_str());
        else std::printf("walked to the end with no parse error (%.0f ms)\n", ms);
    }
    if (pw) *pw = std::move(w);
    return fOk ? 0 : 1;
}
}  // namespace

int ListKeys3()
{
    for (const auto& [va, fk] : g_disc.ElfKeys())
        std::printf("0x%06X id %5u sector 0x%06X cb 0x%08X\n", va, fk.id, fk.isector, fk.cb);
    return 0;
}

int ListFiles3()
{
    std::printf("# worlds (orig3: names 0x%06X, episodes 0x%06X, keys 0x%06X)\n", orig3::VA_AWORLD_NAME,
                orig3::VA_AWORLD_EPISODE, orig3::VA_AWORLD_FK);
    uint64_t cbWorlds = 0;
    int cfile = 0;
    for (const WORLD3& w : g_disc.Worlds())
    {
        std::printf("world %2d %-14s %-11s key 0x%06X id %3u sector %8u cb %9u%s\n", w.iworld, w.name.c_str(),
                    SzEpisode3(w.episode), w.vaFk, w.fk.id, w.fk.isector, w.fk.cb, w.fk.cb ? "" : "  (no file)");
        cbWorlds += w.fk.cb;
        cfile += w.fk.cb != 0;
    }
    std::printf("# %d world files, %.1f MB compressed\n", cfile, cbWorlds / 1048576.0);
    std::printf("# episodes (orig3: first world 0x%06X)\n", orig3::VA_AEPISODE_WORLD);
    for (int e = 0; e < CEPISODE3; ++e) std::printf("episode %d %-11s first world %d\n", e, SzEpisode3(e), g_disc.IworldFirstOfEpisode(e));
    std::printf("# movies (orig3: keys 0x%06X; size = cb & ~0x3FFF)\n", orig3::VA_AMOVIE_FK);
    for (const MOVIE3& m : g_disc.Movies())
        std::printf("movie key 0x%06X id %3u sector %8u cb %10u flags 0x%X\n", m.vaFk, m.fk.id, m.fk.isector, m.fk.cb, m.grf);
    return 0;
}

int DumpWorld3Command(const std::string& arg, const DUMPWORLD3OPTS& o)
{
    if (arg == "all")
    {
        int cfail = 0, cworld = 0;
        std::map<int, int> mpcidc;
        for (const WORLD3& w : g_disc.Worlds())
        {
            if (!w.fk.cb) continue;
            ++cworld;
            WORLDWALK3 walk;
            cfail += DumpOneWorld(w.name, w.fk, DUMPWORLD3OPTS{}, true, &walk) != 0;
            for (auto [cid, c] : walk.mpcidc) mpcidc[cid] += c;
        }
        std::printf("%d worlds, %d failed; %zu classes among the top-level objects\n", cworld, cfail, mpcidc.size());
        return cfail ? 1 : 0;
    }
    FK fk;
    std::string name, error;
    if (!FResolveWorld3(arg, fk, name, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    return DumpOneWorld(name, fk, o, false, nullptr);
}

namespace
{
// The launcher's boot (Boot3): the movies through the copied player, the card check on the host save slots
struct BOOTHOSTMAIN3 final : BOOTHOST3
{
    std::vector<int> aidMovie;
    bool fReq = false;
    TRANSREQ3 req;
    void QueueMovie(uint32_t vaKey, bool) override
    {
        for (const MOVIE3& m : g_disc.Movies())
            if (m.vaFk == vaKey) aidMovie.push_back(int(m.fk.id));
    }
    void RequestTransition(const TRANSREQ3& r) override
    {
        req = r;
        fReq = true;
    }
    int CardState() override { return g_card3.state; }
    void StartCardCheck() override
    {
        // 0x1D0D30(card, 1): the card's state machine (card3.h; the game loop runs UpdateCard3 each frame: the
        // create-save prompt 0x20 on a new user dir, as the reference shows it at about vblank 1400)
        SetCardState3(g_card3, 1);
        InstallCardGame3();
        std::printf("boot: the card check: status %d (slots in %s)\n", CardStatus3(),
                    cane::utf8(cane::userDirectory() / "Saves").c_str());
    }
};

int LoadOneWorld3(const WORLD3& wd, const DUMPWORLD3OPTS& o, bool fSummaryOnly)
{
    if (wd.fk.cb == 0)
    {
        std::printf("world %s: no file\n", wd.name.c_str());
        return 0;
    }
    InitClasses();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> ab = g_disc.ReadWorld(wd.fk);
    auto psw = std::make_unique<SW>();
    LOAD3OPTS lo;
    lo.psw = psw.get();
    lo.iworld = wd.iworld;
    lo.keyA = o.keyA;
    lo.keyB = o.keyB;
    WORLDWALK3 w;
    std::string error;
    const bool fOk = FWalkWorld3(ab, w, error, &lo);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const LOAD3STATS& st = g_load3stats;
    bool fClean = fOk && st.ctopFail == 0;
    int cunported = 0;
    for (auto [cid, c] : st.mpcidcUnported) cunported += c;
    std::printf("%-14s %s: %zu objects (%d top level read exactly, %d failed; %d proxies, %d filtered, %d missions/tasks "
                "kept, %d shared (%d skipped), %d clones; %d without a loader yet), block %d id %d, %.0f ms%s%s\n",
                wd.name.c_str(), fClean ? "loaded" : "ERRORS", psw->aploAll.size(), st.ctopOk, st.ctopFail, st.cproxy,
                st.cfiltered, st.cmission, st.cshared, st.cskipShared, st.cclone, cunported, w.iblockChosen, w.idChosen, ms,
                fOk ? "" : ": ", error.c_str());
    if (!fSummaryOnly)
    {
        for (auto [sz, c] : st.mperrc) std::printf("  error x%d: %s\n", c, sz.c_str());
        std::printf("camera (0x36BC10): condition %d (+%zu), block %d (%zu options), fog %g %g %g rgba 0x%08X, colour %g %g %g, "
                    "tint %g %g %g\n", g_condset3.cond, g_condset3.acond.size(), g_cmblock3.iblock, g_cmblock3.aoptCm.size(),
                    g_condset3.fogNear, g_condset3.fogFar, g_condset3.fogMax, g_condset3.rgbaFog, g_condset3.gLightness,
                    g_condset3.gContrast, g_condset3.gSaturation, g_condset3.rgbTint[0], g_condset3.rgbTint[1], g_condset3.rgbTint[2]);
        std::printf("particle effects (class 0x2D): %d, %d components (%d of 0xBF, %d of 0xC0), %d filtered out, %d not read "
                    "exactly\n", g_pe3stats.cpe, g_pe3stats.ccomp, g_pe3stats.ccompBF, g_pe3stats.ccompC0, g_pe3stats.cfiltered,
                    g_pe3stats.cbad);
        if (st.cscptMisread) std::printf("  SCPT command lists misread by the draft reader (skipped): %d\n", st.cscptMisread);
        for (auto [cid, c] : st.mpcidcFail) std::printf("  failed top-level class 0x%02X %s: %d\n", cid, SzCid3(cid), c);
        for (auto [cid, c] : st.mpcidcUnported)
            std::printf("  no loader yet: class 0x%02X %s (orig3 0x%06X): %d objects, bodies skipped\n", cid, SzCid3(cid),
                        Cls3(cid).pfnLoad, c);
        std::map<int, int> mpcidc;
        for (const auto& p : psw->aploAll) ++mpcidc[p->Cid()];
        std::printf("class census (all objects; cid3 count name3):\n");
        for (auto [cid, c] : mpcidc) std::printf("  0x%02X %5d %s\n", cid, c, SzCid3(cid));
        if (o.fGroups)
            for (std::size_t i = 0; i < psw->awe.size(); ++i)
            {
                const WE& we = psw->awe[i];
                std::printf("%zu %s c %d inst %d used %d id %d x5C %d x60 %d\n", i, we.StrName().c_str(), we.cloLoaded,
                            we.cinst, we.cUsed, we.x10, we.x5C, we.x60);
            }
        if (!o.fields.empty())
        {
            // one line per object of each oid (its copies too), in the form census3.py's fields command compares
            const bool fAll = o.fields == "all";
            for (const char* pch = o.fields.c_str(); *pch;)
            {
                const OID oid = fAll ? 0 : OID(std::strtol(pch, const_cast<char**>(&pch), 0));
                if (fAll) pch += std::strlen(pch);
                else if (*pch == ',') ++pch;
                for (const auto& p : psw->aploAll)
                {
                    const LO* plo = p.get();
                    if (!fAll && plo->oid != oid) continue;
                    const OID oid = plo->oid;
                    std::printf("fields cid 0x%02X oid %d parent %d clone %d", plo->Cid(), oid,
                                plo->paloParent ? plo->paloParent->oid : -1, plo->iclone);
                    if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO)))
                    {
                        const ALO* palo = static_cast<const ALO*>(plo);
                        std::printf(" cxf %zu csoe %zu cglob %zu", palo->axf.size(), palo->asoe.size(),
                                    palo->pglobset ? palo->pglobset->aglob.size() : 0);
                        for (const auto& pxf : palo->axf)
                            std::printf(" xf(%d,%d,%.3f,%.3f,%.3f)", pxf->kind, pxf->tag, pxf->mat[3][0], pxf->mat[3][1], pxf->mat[3][2]);
                    }
                    else if (FCidDerivedFrom(plo->Cid(), Icid(CID3::LOX)))
                    {
                        const LOX* plox = static_cast<const LOX*>(plo);
                        std::printf(" mat(%.3f,%.3f,%.3f)", plox->mat[3][0], plox->mat[3][1], plox->mat[3][2]);
                    }
                    std::printf(" copt %zu\n", plo->aopt.size());
                }
            }
        }
        if (!o.asegCheck.empty() && sly3::CheckAsegSegments3(psw.get(), o.asegCheck.c_str(), stdout)) fClean = false;
        if (o.fCensus)
        {
            FILE* pf = o.out.empty() ? stdout : std::fopen(o.out.c_str(), "w");
            if (pf)
            {
                PrintObjectCensus3(psw.get(), pf);
                if (pf != stdout) std::fclose(pf);
            }
        }
    }
    g_psw3 = nullptr;
    return fClean ? 0 : 1;
}
}  // namespace

int LoadWorld3Command(const std::string& arg, const DUMPWORLD3OPTS& o)
{
    if (arg == "all")
    {
        int cfail = 0, cworld = 0;
        for (const WORLD3& w : g_disc.Worlds())
        {
            if (!w.fk.cb) continue;
            ++cworld;
            cfail += LoadOneWorld3(w, o, true) != 0;
        }
        std::printf("%d worlds, %d with errors\n", cworld, cfail);
        return cfail ? 1 : 0;
    }
    FK fk;
    std::string name, error;
    if (!FResolveWorld3(arg, fk, name, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    for (const WORLD3& w : g_disc.Worlds())
        if (w.name == name) return LoadOneWorld3(w, o, false);
    std::fprintf(stderr, "--load-world needs a world of the table (its index is the filter)\n");
    return 2;
}

int Boot3(bool fHeadless)
{
    // orig3: 0x1C6EA8 / 0x1C6F30 (boot3.h): the two logo movies, the card check, then world 35 (m_ext) with 0x1A8:
    // the title plays over it
    BOOTQUEUE3 q;
    BOOTHOSTMAIN3 host;
    StartBootSequence3(q, host);
    for (;;)
    {
        if (!host.fReq) return 0;
        host.fReq = false;
        if (host.req.iworld == 0 && !host.aidMovie.empty())  // a movie's transition (the empty key 0x345820)
        {
            for (int id : host.aidMovie)
            {
                const std::string sz = std::to_string(id);
                const int err = fHeadless ? PlayMovieCommand(sz, "", 1e9) : ViewMovieCommand(sz, "", 0.0, 960, 720);
                if (err) return err;
            }
            host.aidMovie.clear();
            FRunBootQueue3(q, host);  // (the transition landed: the next action)
            continue;
        }
        if (g_pfnBootTransition3) return g_pfnBootTransition3(host.req);
        std::printf("Sly3-Release: the boot continues with world %d (m_ext, the prologue) at point 0x%X, which is not "
                    "ported yet\n", host.req.iworld, host.req.point);
        return 0;
    }
}
