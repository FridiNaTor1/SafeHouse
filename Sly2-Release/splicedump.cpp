// --load-world W --dump-splice DIR: every group's scripts of the world as readable text (splicenames.h), with the
// world's objects' names (their group and class), each checked to compile back to the same bytes; a coverage line.
// --splice-names-check DIR: every installed world's scripts (no world loaded: objects unnamed) printed and re-read.
#include "splicedump.h"

#include "cls.h"
#include "disc.h"
#include "lo.h"
#include "splicenames.h"
#include "sw.h"
#include "worldparts.h"

#include "../platform/platform.h"
#include "../platform/safe_paths.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

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
    std::printf("%s: symbols %d (named %d = %.0f%%, by kind %d, assert words only %d), events %d/%d, engine options %d/%d (%.0f%%), "
                "script options %d/%d, objects %d/%d (%.0f%%), built-ins %d/%d, event words %d\n",
                what, s.csym, s.csymNamed, pct(s.csymNamed, s.csym - s.csymWord), s.csymKind, s.csymWord, s.ceventNamed, s.cevent,
                s.coptEngineNamed, s.coptEngine, pct(s.coptEngineNamed, s.coptEngine), s.coptScriptNamed, s.coptScript, s.coidNamed,
                s.coid, pct(s.coidNamed, s.coid), s.cbifNamed, s.cbif, s.cmsg);
}
}  // namespace

namespace
{
std::map<int, std::vector<WORLDOBJREC>> s_mpgrprec;   // every group's top-level records (the other places' objects too)
}

std::string SpliceObjectName(const SW* psw, int oid)
{
    if (!psw) return std::string();
    const LO* pbest = nullptr;
    for (const auto& up : psw->aploAll)
        if (up->oid == oid && (!pbest || (pbest->paloParent && !up->paloParent))) pbest = up.get();
    if (!pbest)
    {
        // (not loaded here: another place's object of this world file, by its record)
        for (const auto& [igrp, arec] : s_mpgrprec)
            for (const WORLDOBJREC& r : arec)
                if (r.oid == oid) return GroupName(psw, igrp) + " " + g_acls[r.cid].name;
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
    g_disc.SetPartReader([root, pmptext](const std::string& rel, std::vector<uint8_t>& ab) {
        std::ifstream in(cane::paths::ioPath(cane::path(root) / cane::path(rel)), std::ios::binary);
        if (!in) return false;
        ab.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        // ".../World/NNN name/Scripts.splice": group NNN's scripts
        const std::size_t w = rel.rfind("World/");
        if (w != std::string::npos && rel.size() > 16 && rel.compare(rel.size() - 15, 15, "/Scripts.splice") == 0)
            (*pmptext)[std::atoi(rel.c_str() + w + 6)] = std::string(ab.begin(), ab.end());
        if (w != std::string::npos && rel.size() > 13 && rel.compare(rel.size() - 12, 12, "/Objects.bin") == 0)
        {
            const int igrp = std::atoi(rel.c_str() + w + 6);
            std::string err;
            FParseObjects(ab.data(), ab.size(), igrp, s_mpgrprec[igrp], err);
        }
        return true;
    });
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
    for (const auto& [igrp, raw] : mptext)
    {
        SPLICENAMECTX ctx;
        ctx.pfnObject = [psw](int oid) { return SpliceObjectName(psw, oid); };
        SpliceCollectNames(raw, ctx, world);
    }
    for (const auto& [igrp, raw] : mptext)
    {
        SPLICENAMECTX ctx;
        ctx.pworld = &world;
        ctx.title = "group " + std::to_string(igrp) + " " + GroupName(psw, igrp);
        ctx.pfnObject = [psw](int oid) { return SpliceObjectName(psw, oid); };
        std::string rd, err;
        SPLICENAMESTATS st;
        if (!FSpliceReadable(raw, ctx, rd, err, &st)) { std::printf("dump-splice: group %d: %s\n", igrp, err.c_str()); ++cfail; continue; }
        if (!FRoundTrip(raw, rd, err)) { std::printf("dump-splice: group %d: %s\n", igrp, err.c_str()); ++cfail; }
        tot.Add(st);
        char sz[16];
        std::snprintf(sz, sizeof(sz), "%03d ", igrp);
        const std::string name = std::string(sz) + GroupName(psw, igrp) + ".splice";
        std::ofstream out(cane::paths::ioPath(cane::path(dir) / cane::path(name)), std::ios::binary);
        out << rd;
    }
    std::printf("dump-splice: %zu groups' scripts to %s, %d failed\n", mptext.size(), dir.c_str(), cfail);
    PrintStats("dump-splice coverage", tot);
    return cfail ? 1 : 0;
}

int SpliceNamesCheck(const std::string& dirInstall)
{
    int cfail = SpliceNamesSelfTest();
    std::printf("splice-names: tables and the small text: %s\n", cfail ? "FAILED" : "passed");
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
    }
    std::printf("splice-names: %d scripts files printed readable and read back to the same bytes, %d failed\n", cfile, cfail);
    PrintStats("splice-names coverage (no world: objects unnamed)", tot);
    return cfail ? 1 : 0;
}
