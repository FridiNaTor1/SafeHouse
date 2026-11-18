// --load-world W --dump-splice DIR: every group's scripts of the world as readable text (splicenames.h), with the
// world's objects' names (their group and class), each checked to compile back to the same bytes; a coverage line.
// --splice-names-check DIR: every installed world's scripts (no world loaded: objects unnamed) printed and re-read.
#include "splicedump.h"

#include "asega.h"
#include "aseg.h"
#include "cls.h"
#include "sm.h"
#include "disc.h"
#include "lo.h"
#include "splicenames.h"
#include "splicediag.h"
#include "splicemap.h"
#include "sw.h"
#include "worldparts.h"

#include "../platform/platform.h"
#include "../platform/safe_paths.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <set>

namespace
{
std::string GroupName(const SW* psw, int igrp)
{
    if (!psw || igrp < 0 || igrp >= int(psw->awe.size())) return std::string();
    std::string s = psw->awe[std::size_t(igrp)].achName;
    if (s.rfind("FK$X", 0) == 0) s = s.substr(4);
    return s;
}

bool FReadAll(const std::filesystem::path& p, std::string& s)
{
    std::ifstream in(cane::paths::ioPath(p), std::ios::binary);
    if (!in) return false;
    s.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// the text both ways: the readable text compiles to the bytes the converter's text does
bool FRoundTrip(const std::string& raw, const std::string& readable, std::string& error)
{
    std::string back;
    std::vector<uint8_t> abA, abB;
    if (!FSpliceRaw(readable, back, error)) return false;
    if (!FSpliceFromText(raw, abA, error)) return false;
    if (!FSpliceFromText(back, abB, error)) { error = "the readable text does not compile: " + error; return false; }
    if (abA != abB) { error = "the readable text compiles to other bytes"; return false; }
    return true;
}

void PrintStats(const char* what, const SPLICENAMESTATS& s)
{
    auto pct = [](int a, int b) { return b ? 100.0 * a / b : 100.0; };
    // (round 3: "named" is a traced / used name, "guessed" a ?name; tools/namescoverage.py reads this line)
    std::printf("%s: symbols %d (named %d = %.0f%%, guessed %d, by kind %d, assert words only %d), events %d/%d, engine options %d/%d (%.0f%%, "
                "guessed %d), script options %d/%d (guessed %d), objects %d/%d (%.0f%%, guessed %d), built-ins %d/%d, event words %d/%d (guessed %d), "
                "classes %d/%d, asserts %d\n",
                what, s.csym, s.csymNamed, pct(s.csymNamed, s.csym - s.csymWord), s.csymGuess, s.csymKind, s.csymWord, s.ceventNamed, s.cevent,
                s.coptEngineNamed, s.coptEngine, pct(s.coptEngineNamed, s.coptEngine), s.coptEngineGuess, s.coptScriptNamed, s.coptScript,
                s.coptScriptGuess, s.coidNamed, s.coid, pct(s.coidNamed, s.coid), s.coidGuess, s.cbifNamed, s.cbif, s.cmsgNamed, s.cmsg, s.cmsgGuess,
                s.cclsNamed, s.ccls, s.cassert);
}
}  // namespace

namespace
{
std::map<int, std::vector<WORLDOBJREC>> s_mpgrprec;   // every group's top-level records (the other places' objects too)
std::map<int, SPLICECHILDREC> s_mpchild;              // the records' children
std::string s_dirRoot, s_relEpisode;                    // the installation, the loaded world's episode folder
std::map<int, std::string> s_mpoidOther;               // the episode's other places' objects (oid -> "group class")
bool s_fOtherIndexed = false;

std::map<int, std::string> s_mpoidAll;                 // (round 3) every place's objects (a shared group's scripts
bool s_fAllIndexed = false;                             // look up objects of other episodes: Murray's the train's task)

// a place's records into mp (oid -> "group class")
void IndexPlace(const std::filesystem::path& dirPlace, std::map<int, std::string>& mp)
{
    const std::filesystem::path dirW = dirPlace / "World";
    std::error_code ec2;
    for (auto itG = std::filesystem::directory_iterator(dirW, ec2); !ec2 && itG != std::filesystem::directory_iterator(); itG.increment(ec2))
    {
        std::string ab;
        if (!FReadAll(itG->path() / "Objects.bin", ab)) continue;
        std::string g = cane::utf8(itG->path().filename());
        const int igrp = std::atoi(g.c_str());
        if (g.size() > 4 && g[3] == ' ') g = g.substr(4);
        const auto* pb = reinterpret_cast<const uint8_t*>(ab.data());
        std::vector<WORLDOBJREC> arec;
        std::string err;
        if (!FParseObjects(pb, ab.size(), igrp, arec, err)) continue;
        for (const WORLDOBJREC& r : arec)
            if (!mp.count(r.oid)) mp[r.oid] = FCidDerivedFrom(r.cid, 0x30) ? g : g + " " + g_acls[r.cid].name;
        std::map<int, SPLICECHILDREC> mpc;
        SpliceIndexChildRecords(igrp, pb, ab.size(), mpc);
        for (const auto& [oid, c] : mpc)
            if (!mp.count(oid)) mp[oid] = SpliceChildName(mpc, oid, g);
    }
}
// every episode's places (once: a lookup the episode does not answer)
void IndexAllPlaces()
{
    s_fAllIndexed = true;
    if (s_dirRoot.empty()) return;   // (not the dump's reader: the editor names objects itself)
    std::error_code ec;
    const std::filesystem::path dirWorlds = cane::path(s_dirRoot) / "Worlds";
    std::vector<std::filesystem::path> aplace;
    for (auto itEp = std::filesystem::directory_iterator(dirWorlds, ec); !ec && itEp != std::filesystem::directory_iterator(); itEp.increment(ec))
    {
        std::error_code ec2;
        for (auto itPlace = std::filesystem::directory_iterator(itEp->path(), ec2); !ec2 && itPlace != std::filesystem::directory_iterator(); itPlace.increment(ec2))
            aplace.push_back(itPlace->path());
    }
    std::sort(aplace.begin(), aplace.end());   // (the same name for an oid on every machine)
    for (const std::filesystem::path& d : aplace) IndexPlace(d, s_mpoidAll);
}

// the episode's other places' records (an episode's shared scripts look up objects of its other places)
void IndexOtherPlaces()
{
    s_fOtherIndexed = true;
    if (s_relEpisode.empty()) return;
    std::error_code ec;
    const std::filesystem::path dirEp = cane::path(s_dirRoot) / cane::path(s_relEpisode);
    for (auto itPlace = std::filesystem::directory_iterator(dirEp, ec); !ec && itPlace != std::filesystem::directory_iterator(); itPlace.increment(ec))
    {
        const std::filesystem::path dirW = itPlace->path() / "World";
        std::error_code ec2;
        for (auto itG = std::filesystem::directory_iterator(dirW, ec2); !ec2 && itG != std::filesystem::directory_iterator(); itG.increment(ec2))
        {
            std::string ab;
            if (!FReadAll(itG->path() / "Objects.bin", ab)) continue;
            std::string g = cane::utf8(itG->path().filename());
            const int igrp = std::atoi(g.c_str());
            if (g.size() > 4 && g[3] == ' ') g = g.substr(4);
            const auto* pb = reinterpret_cast<const uint8_t*>(ab.data());
            std::vector<WORLDOBJREC> arec;
            std::string err;
            if (!FParseObjects(pb, ab.size(), igrp, arec, err)) continue;
            for (const WORLDOBJREC& r : arec)
                if (!s_mpoidOther.count(r.oid)) s_mpoidOther[r.oid] = FCidDerivedFrom(r.cid, 0x30) ? g : g + " " + g_acls[r.cid].name;
            std::map<int, SPLICECHILDREC> mpc;
            SpliceIndexChildRecords(igrp, pb, ab.size(), mpc);
            for (const auto& [oid, c] : mpc)
                if (!s_mpoidOther.count(oid)) s_mpoidOther[oid] = SpliceChildName(mpc, oid, g);
        }
    }
}
}

void SpliceIndexChildRecords(int igrp, const uint8_t* pb, std::size_t cb, std::map<int, SPLICECHILDREC>& mp)
{
    std::vector<WORLDOBJREC> arec;
    std::string err;
    if (!FParseObjects(pb, cb, igrp, arec, err)) return;
    auto rd16 = [&](std::size_t i) { int16_t v; std::memcpy(&v, pb + i, 2); return int(v); };
    auto rd32 = [&](std::size_t i) { int32_t v; std::memcpy(&v, pb + i, 4); return v; };
    for (const WORLDOBJREC& r : arec)
    {
        const std::size_t ibEnd = r.ib + r.cb;
        // the top-level record's own header: k, filters, cid, oid, n, size
        std::size_t j0 = r.ib + 1 + (r.k > 0 ? std::size_t(r.k) * 2 : 0) + 2 + 4 + 2 + 4;
        for (std::size_t j = j0; j + 13 <= ibEnd; ++j)
        {
            const int k = int8_t(pb[j]);
            if (k < -2 || k > 4) continue;
            const std::size_t jc = j + 1 + (k > 0 ? std::size_t(k) * 2 : 0);
            if (jc + 12 > ibEnd) continue;
            if (k > 0)
            {
                bool fOk = true;   // (the filters: world indices 0..63)
                for (int i = 0; i < k && fOk; ++i) fOk = rd16(j + 1 + std::size_t(i) * 2) >= -1 && rd16(j + 1 + std::size_t(i) * 2) < 64;
                if (!fOk) continue;
            }
            const int cid = rd16(jc);
            if (cid < 2 || cid >= CID_Max || !g_acls[cid].name || !*g_acls[cid].name) continue;
            const int oid = rd32(jc + 2);
            if (oid <= 0 || oid >= 1000000) continue;
            const uint32_t cbData = uint32_t(rd32(jc + 8));
            if (cbData == 0 || jc + 12 + cbData > ibEnd) continue;
            if (oid == r.oid) continue;
            if (!mp.count(oid)) mp[oid] = SPLICECHILDREC{igrp, cid, r.oid, r.cid};
        }
    }
}

std::string SpliceChildName(const std::map<int, SPLICECHILDREC>& mp, int oid, const std::string& groupName)
{
    const auto it = mp.find(oid);
    if (it == mp.end()) return std::string();
    std::string g = groupName;
    if (g.rfind("FK$X", 0) == 0) g = g.substr(4);
    return (g.empty() ? std::string("group ") + std::to_string(it->second.igrp) : g) + " " + g_acls[it->second.cid].name;
}

std::string SpliceObjectName(const SW* psw, int oid, bool fOtherEpisodes)
{
    if (!psw) return std::string();
    const LO* pbest = nullptr;
    for (const auto& up : psw->aploAll)
        if (up->oid == oid && (!pbest || (pbest->paloParent && !up->paloParent))) pbest = up.get();
    if (!pbest)
    {
        // (a state machine's state: its oid is the state's, not an object's: "group SM state K")
        for (const auto& up : psw->aploAll)
            if (up->Cid() == 0x6F)
            {
                const SM* psm = static_cast<const SM*>(up.get());
                for (std::size_t i = 0; i < psm->anode.size(); ++i)
                    if (psm->anode[i].first == oid) return SpliceObjectName(psw, up->oid) + " state " + std::to_string(i);
            }
        // (not loaded here: another place's object of this world file, by its record)
        for (const auto& [igrp, arec] : s_mpgrprec)
            for (const WORLDOBJREC& r : arec)
                if (r.oid == oid) return GroupName(psw, igrp) + " " + g_acls[r.cid].name;
        // (inside another object's record: made when its instance is placed)
        if (const auto it = s_mpchild.find(oid); it != s_mpchild.end()) return SpliceChildName(s_mpchild, oid, GroupName(psw, it->second.igrp));
        // (another place of the episode)
        if (!s_fOtherIndexed) IndexOtherPlaces();
        if (const auto it = s_mpoidOther.find(oid); it != s_mpoidOther.end()) return it->second;
        // (another episode's place)
        if (!fOtherEpisodes) return std::string();
        if (!s_fAllIndexed) IndexAllPlaces();
        if (const auto it = s_mpoidAll.find(oid); it != s_mpoidAll.end()) return it->second;
        return std::string();
    }
    const std::string g = GroupName(psw, pbest->igrp);
    const char* cls = g_acls[pbest->Cid()].name;
    if (pbest->paloParent == nullptr && !g.empty()) return FCidDerivedFrom(pbest->Cid(), 0x30) ? g : g + " " + cls;
    return g.empty() ? std::string(cls) + " " + std::to_string(oid) : g + " " + cls;
}


void InstallSpliceDumpReader(std::map<int, std::string>* pmptext)
{
    const std::string root = g_disc.Path();
    s_mpgrprec.clear();
    s_mpchild.clear();
    s_mpoidOther.clear();
    s_fOtherIndexed = false;
    s_mpoidAll.clear();
    s_fAllIndexed = false;
    s_dirRoot = root;
    s_relEpisode.clear();
    g_disc.SetPartReader([root, pmptext](const std::string& rel, std::vector<uint8_t>& ab) {
        std::ifstream in(cane::paths::ioPath(cane::path(root) / cane::path(rel)), std::ios::binary);
        if (!in) return false;
        ab.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        // (the episode's folder: "Worlds/<n> <Episode>/")
        if (s_relEpisode.empty() && rel.rfind("Worlds/", 0) == 0)
            if (const std::size_t e = rel.find('/', 7); e != std::string::npos) s_relEpisode = rel.substr(0, e);
        // ".../World/NNN name/Scripts.splice": group NNN's scripts
        const std::size_t w = rel.rfind("World/");
        if (w != std::string::npos && rel.size() > 16 && rel.compare(rel.size() - 15, 15, "/Scripts.splice") == 0)
            (*pmptext)[std::atoi(rel.c_str() + w + 6)] = std::string(ab.begin(), ab.end());
        if (w != std::string::npos && rel.size() > 13 && rel.compare(rel.size() - 12, 12, "/Objects.bin") == 0)
        {
            const int igrp = std::atoi(rel.c_str() + w + 6);
            std::string err;
            FParseObjects(ab.data(), ab.size(), igrp, s_mpgrprec[igrp], err);
            SpliceIndexChildRecords(igrp, ab.data(), ab.size(), s_mpchild);
        }
        return true;
    });
}

std::function<std::string(int word)> SpliceEventWordSource(const SW* psw)
{
    auto pmp = std::make_shared<std::map<int, std::string>>();
    if (psw)
        for (const auto& up : psw->aploAll)
        {
            if (up->Cid() != 0x6D && up->Cid() != 0x6E) continue;
            const ASEGD* pd = PasegdOf(static_cast<ASEG*>(up.get()));
            if (!pd) continue;
            for (const std::vector<ASEGEV>& aev : pd->aaev)
                for (const ASEGEV& ev : aev)
                {
                    if (ev.kind != 12 || pmp->count(ev.an[0])) continue;
                    std::string g = SpliceObjectName(psw, up->oid);
                    for (const char* sz : {" ASEGBL", " ASEG"})
                        if (const std::size_t at = g.rfind(sz); at != std::string::npos && at + std::strlen(sz) == g.size()) g.resize(at);
                    char ach[48];
                    std::snprintf(ach, sizeof(ach), " clip at %.2g s", double(ev.t));
                    (*pmp)[ev.an[0]] = (g.empty() ? std::string("a") : g) + ach;
                }
        }
    return [pmp](int w) {
        const auto it = pmp->find(w);
        return it == pmp->end() ? std::string() : it->second;
    };
}

std::vector<int> SpliceRuntimeClasses(std::vector<int> acid)
{
    // (an animation's / state machine's handlers get its runner: the ASEGA (0x86) / SMA (0x88))
    const std::vector<int> a0 = acid;
    for (int c : a0)
    {
        if (c == 0x6D || c == 0x6E) acid.push_back(0x86);
        if (c == 0x6F) acid.push_back(0x88);
    }
    return acid;
}

// the diagnostics of a world's scripts (the world's objects and the classes running each module): every group's
// problems into out, the counts by severity; codes: how many of each
void DiagnoseWorld(const SW* psw, const std::map<int, std::string>& mptext, std::string& out, int acount[3], std::map<std::string, int>& mpcode)
{
    // (an oid's class when every object of it has the one class; -2: several)
    std::map<int, int> mpoidcid;
    auto note = [&](int oid, int cid) {
        const auto [it, fNew] = mpoidcid.emplace(oid, cid);
        if (!fNew && it->second != cid) it->second = -2;
    };
    for (const auto& up : psw->aploAll) note(up->oid, up->Cid());
    for (const auto& [igrp, arec] : s_mpgrprec)
        for (const WORLDOBJREC& r : arec) note(r.oid, r.cid);
    // the modules' frames -> (group, module); the classes running each
    std::map<std::pair<int, int>, std::set<int>> mpmodcid;
    std::map<const CFrame*, std::pair<int, int>> mpframe;
    if (SWSPLICE* ps = PswspliceFromSw(const_cast<SW*>(psw)))
        for (const auto& pt : ps->apsplct)
        {
            int igroup = -1;
            for (std::size_t g = 0; g < psw->awe.size(); ++g)
                if (psw->awe[g].x10 == pt->a) { igroup = int(g); break; }
            for (std::size_t i = 0; i < pt->asplc.size(); ++i)
                if (pt->asplc[i].pframe) mpframe[pt->asplc[i].pframe.get()] = {igroup, int(i)};
        }
    for (const auto& up : psw->aploAll)
        if (up->pframe)
            if (const auto it = mpframe.find(up->pframe.get()); it != mpframe.end())
            {
                mpmodcid[it->second].insert(up->Cid());
                if (std::getenv("SLY2N_MODOBJS"))   // (debug: the objects running each module)
                    std::printf("module-object: group %d module %d: %d %s (%s)\n", it->second.first, it->second.second, up->oid,
                                g_acls[up->Cid()].name, SpliceObjectName(psw, up->oid).c_str());
            }
    for (const auto& [igrp, raw] : mptext)
    {
        SPLICEDIAGCTX ctx;
        // (the episode's objects: another episode's are not in this world, nor a state machine's states)
        ctx.pfnObjectExists = [&](int oid) {
            if (mpoidcid.count(oid) != 0 || s_mpchild.count(oid) != 0) return true;
            const std::string n = SpliceObjectName(psw, oid, false);
            return !n.empty() && n.find(" state ") == std::string::npos;
        };
        // (a proxy's oid finds its instance's object: no class known)
        ctx.pfnObjectCid = [&](int oid) {
            const auto it = mpoidcid.find(oid);
            return it == mpoidcid.end() || it->second < 0 || it->second == 0x06 || it->second == 0x07 ? -1 : it->second;
        };
        const int ig = igrp;
        ctx.pfnModuleClasses = [&, ig](int imod) {
            std::vector<int> a;
            if (const auto it = mpmodcid.find({ig, imod}); it != mpmodcid.end()) a.assign(it->second.begin(), it->second.end());
            return SpliceRuntimeClasses(a);
        };
        std::vector<SPLICEISSUE> adiag;
        SpliceDiagnose(raw, ctx, adiag);
        for (const SPLICEISSUE& d : adiag)
        {
            ++acount[std::clamp(d.sev, 0, 2)];
            ++mpcode[std::string(d.sev == 0 ? "E " : d.sev == 1 ? "W " : "H ") + d.code];
            out += "group " + std::to_string(igrp) + " " + GroupName(psw, igrp) + ": " + StrSpliceDiag(d) + "\n";
        }
    }
}

int DumpSplice(const SW* psw, const std::map<int, std::string>& mptext, const std::string& dir)
{
    std::error_code ec;
    std::filesystem::create_directories(cane::path(dir), ec);
    SPLICENAMESTATS tot;
    int cfail = 0;
    std::string all;
    // the world's names first (a symbol is one name in every group)
    SPLICEWORLDNAMES world;
    const std::function<std::string(int)> pfnWord = SpliceEventWordSource(psw);
    for (const auto& [igrp, raw] : mptext)
    {
        SPLICENAMECTX ctx;
        ctx.title = "group " + std::to_string(igrp) + " " + GroupName(psw, igrp);
        ctx.pfnObject = [psw](int oid) { return SpliceObjectName(psw, oid); };
        ctx.pfnEventWord = pfnWord;
        SpliceCollectNames(raw, ctx, world);
    }
    // the names table: every name with its confidence (names.tsv)
    std::map<std::pair<std::string, std::string>, SPLICENAMEROW> mprow;
    for (const auto& [igrp, raw] : mptext)
    {
        SPLICENAMECTX ctx;
        ctx.pworld = &world;
        ctx.title = "group " + std::to_string(igrp) + " " + GroupName(psw, igrp);
        ctx.pfnObject = [psw](int oid) { return SpliceObjectName(psw, oid); };
        ctx.pfnEventWord = pfnWord;
        std::string rd, err;
        SPLICENAMESTATS st;
        std::vector<SPLICENAMEROW> arow;
        if (!FSpliceReadable(raw, ctx, rd, err, &st, &arow)) { std::printf("dump-splice: group %d: %s\n", igrp, err.c_str()); ++cfail; continue; }
        for (SPLICENAMEROW& r : arow) mprow.emplace(std::make_pair(r.kind, r.raw), r);
        if (!FRoundTrip(raw, rd, err)) { std::printf("dump-splice: group %d: %s\n", igrp, err.c_str()); ++cfail; }
        tot.Add(st);
        char sz[16];
        std::snprintf(sz, sizeof(sz), "%03d ", igrp);
        const std::string name = std::string(sz) + GroupName(psw, igrp) + ".splice";
        std::ofstream out(cane::paths::ioPath(cane::path(dir) / cane::path(name)), std::ios::binary);
        out << rd;
    }
    {
        std::ofstream out(cane::paths::ioPath(cane::path(dir) / cane::path("names.tsv")), std::ios::binary);
        out << "kind\tnumber\tname\tconfidence\n";
        for (const auto& [k, r] : mprow) out << r.kind << '\t' << r.raw << '\t' << r.name << '\t' << r.conf << '\n';
    }
    std::printf("dump-splice: %zu groups' scripts to %s, %d failed\n", mptext.size(), dir.c_str(), cfail);
    PrintStats("dump-splice coverage", tot);
    // the diagnostics of the game's own scripts (the converter's text, with the world)
    {
        std::string diag;
        int acount[3] = {};
        std::map<std::string, int> mpcode;
        DiagnoseWorld(psw, mptext, diag, acount, mpcode);
        std::ofstream out(cane::paths::ioPath(cane::path(dir) / cane::path("diagnostics.txt")), std::ios::binary);
        out << diag;
        std::string codes;
        for (const auto& [c, n] : mpcode) codes += " " + c + "=" + std::to_string(n);
        std::printf("dump-splice diagnostics: %d errors, %d warnings, %d hints:%s\n", acount[0], acount[1], acount[2], codes.c_str());
    }
    return cfail ? 1 : 0;
}

int SpliceNamesCheck(const std::string& dirInstall)
{
    int cfail = SpliceNamesSelfTest();
    std::printf("splice-names: tables and the small text: %s\n", cfail ? "FAILED" : "passed");
    const int cfailDiag = SpliceDiagSelfTest();
    std::printf("splice-diag: seeded problems: %s\n", cfailDiag ? "FAILED" : "passed");
    cfail += cfailDiag;
    int cdiagErr = 0, cdiagKnown = 0;
    int cfile = 0;
    SPLICENAMESTATS tot;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(cane::path(dirInstall), ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file() || it->path().filename() != "Scripts.splice") continue;
        std::string raw;
        if (!FReadAll(it->path(), raw)) continue;
        ++cfile;
        SPLICENAMECTX ctx;
        std::string rd, err;
        SPLICENAMESTATS st;
        if (!FSpliceReadable(raw, ctx, rd, err, &st) || !FRoundTrip(raw, rd, err))
        {
            std::printf("splice-names: %s: %s\n", cane::utf8(it->path()).c_str(), err.c_str());
            ++cfail;
            continue;
        }
        tot.Add(st);
        // (the game's own scripts give no errors, in the converter's text and in the readable one)
        for (const std::string* pt : {&raw, &rd})
        {
            SPLICEDIAGCTX dctx;
            std::vector<SPLICEISSUE> adiag;
            SpliceDiagnose(*pt, dctx, adiag);
            for (const SPLICEISSUE& d : adiag)
                // (the original's two calls of functions no script defines: the guards' aim managers'
                // on-aim-setup-failed (sym1533) on their failure path, the final job's on-guard-knocked-out (sym1407):
                // warnings, counted apart)
                if (d.code == "never-defined")
                    ++cdiagKnown;
                else if (d.sev == SPLICESEV_Error)
                {
                    if (++cdiagErr <= 20) std::printf("splice-diag: %s%s: %s\n", cane::utf8(it->path()).c_str(), pt == &rd ? " (readable)" : "", StrSpliceDiag(d).c_str());
                }
        }
    }
    std::printf("splice-names: %d scripts files printed readable and read back to the same bytes, %d failed\n", cfile, cfail);
    std::printf("splice-diag: the installed scripts give %d errors (and %d warnings of the original's two never-defined functions)\n", cdiagErr, cdiagKnown);
    if (cdiagErr) ++cfail;
    PrintStats("splice-names coverage (no world: objects unnamed)", tot);
    return cfail ? 1 : 0;
}

int SpliceCompileCommand(const std::string& path)
{
    std::string text;
    if (!FReadAll(cane::path(path), text)) { std::fprintf(stderr, "splice-compile: cannot read %s\n", path.c_str()); return 2; }
    SPLICEDIAGCTX ctx;
    std::vector<SPLICEISSUE> adiag;
    SpliceDiagnose(text, ctx, adiag);
    for (const SPLICEISSUE& d : adiag) std::fprintf(stderr, "%s: %s\n", path.c_str(), StrSpliceDiag(d).c_str());
    std::vector<uint8_t> ab;
    std::string err, out;
    if (!FSpliceTextToBytes(text, ab, err)) { std::fprintf(stderr, "%s: does not compile: %s\n", path.c_str(), err.c_str()); return 1; }
    std::string title = text.substr(0, text.find('\n'));
    title = title.rfind("; ", 0) == 0 ? title.substr(2) : std::string();
    if (!FSpliceToText(ab.data(), ab.size(), title, out, err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fprintf(stderr, "%s: compiles to %zu bytes (fnv %s)\n", path.c_str(), ab.size(), StrFnv64(ab.data(), ab.size()).c_str());
    return 0;
}
