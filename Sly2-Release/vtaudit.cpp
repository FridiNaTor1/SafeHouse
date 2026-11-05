// The decomp coverage audit's class dump (dev command `--vt-dump FILE`): for every class of the class table
// (0x2BAAE8, cls.h) and every slot of its original vtable, the original function, whether the port has a native
// implementation bound to that address in one of its registries, and the function's status in the matching decomp
// (SLY2N_DECOMP=<dir>, default a sly2-decomp checkout beside the repository: config/functions.csv, matched.csv).
// tools/vtaudit.py joins it with the `// orig:` comments of the sources and the runtime sweep.
//
// Registries: N = NATIVEFN (vtnative.cpp), U = the update table (vt slot 16, update.cpp RegisterLoUpdate),
// P = an SW pass (update.cpp RegisterSwPass), L = a native loader (slot 14, loaders.cpp RegisterLoaders).
// Most of the port dispatches by comparing PfnOrig(slot) with an address instead, so an empty registry column
// does not mean "not ported": the script's `orig` column (source comments and address comparisons) completes it.

#include "cls.h"
#include "disc.h"
#include "update.h"
#include "vtnative.h"

#include <cstdio>
#include <exception>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#ifndef SLY2N_DEV_ROOT  // the development checkout (CMakeLists.txt); only the developer options read below it
#define SLY2N_DEV_ROOT "."
#endif
#include <vector>

namespace
{
struct DFN
{
    std::string name, status, source;
    uint32_t cb = 0;
};

std::vector<std::string> SplitCsv(const std::string& line)
{
    std::vector<std::string> a;
    std::stringstream ss(line);
    std::string f;
    while (std::getline(ss, f, ',')) a.push_back(f);
    return a;
}

void LoadDecomp(const std::string& dir, std::unordered_map<uint32_t, DFN>& mp)
{
    std::ifstream fn(dir + "/config/functions.csv");
    std::string line;
    std::getline(fn, line);  // (header: Name,Start,End,Size)
    while (std::getline(fn, line))
    {
        auto a = SplitCsv(line);
        if (a.size() < 4) continue;
        DFN& d = mp[static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 16))];
        d.name = a[0];
        d.cb = static_cast<uint32_t>(std::strtoul(a[3].c_str(), nullptr, 10));
        d.status = "unmatched";
    }
    std::ifstream fm(dir + "/config/matched.csv");
    std::getline(fm, line);  // (header: name,addr,n_instr,status,source)
    while (std::getline(fm, line))
    {
        auto a = SplitCsv(line);
        if (a.size() < 5) continue;
        DFN& d = mp[static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 16))];
        if (d.name.empty() || d.name.rfind("func_", 0) == 0) d.name = a[0];
        d.status = a[3] == "match" ? "match" : "partial:" + a[3];
        d.source = a[4];
    }
}
}

namespace
{
uint32_t PfnOrigSafe(const CLS& c, int slot)
{
    try { return c.PfnOrig(slot); } catch (const std::exception&) { return 0; }
}
}

int VtDumpCommand(const char* path)
{
    InitClasses();
    const char* dir = std::getenv("SLY2N_DECOMP");
    std::unordered_map<uint32_t, DFN> mpdfn;
    LoadDecomp(dir && *dir ? dir : SLY2N_DEV_ROOT "/../sly2-decomp", mpdfn);
    if (mpdfn.empty()) std::fprintf(stderr, "vt-dump: no decomp config found (SLY2N_DECOMP)\n");

    std::FILE* pf = std::fopen(path, "w");
    if (!pf)
    {
        std::fprintf(stderr, "vt-dump: cannot write %s\n", path);
        return 1;
    }
    std::fprintf(pf, "cid\tclass\tbase\tslot\taddr\tinherited\treg\tnative\tdecomp\tname\tsource\n");
    int cslot = 0, cown = 0, creg = 0;
    std::set<uint32_t> setVt;
    for (int i = 0; i < CID_Max; ++i)
        if (g_acls[i].vtOrig) setVt.insert(g_acls[i].vtOrig);
    for (int i = 0; i < CID_Max; ++i)
    {
        const CLS& c = g_acls[i];
        if (!c.vtOrig) continue;
        // the vtable ends at its first null entry after slot 0 (or at another class's vtable)
        for (int slot = 1; slot < 128; ++slot)
        {
            const uint32_t va = c.vtOrig + 8u * static_cast<uint32_t>(slot);
            if (setVt.count(va)) break;
            const uint32_t addr = PfnOrigSafe(c, slot);
            if (!addr || addr < 0x100000 || addr >= 0x300000) break;
            // inherited: the topmost ancestor whose vtable holds the same function in this slot
            int cidFrom = c.cid;
            for (const CLS* pb = c.pclsBase; pb; pb = pb->pclsBase)
                if (pb->vtOrig && PfnOrigSafe(*pb, slot) == addr) cidFrom = pb->cid;
            std::string reg;
            if (PvNativeFn(addr)) reg += 'N';
            if (PfnUpdateOrig(addr)) reg += 'U';
            if (FHasSwPass(addr)) reg += 'P';
            if (FHasNativeLoader(addr)) reg += 'L';
            const char* nameNative = NameNativeFn(addr);
            auto it = mpdfn.find(addr);
            const DFN* pd = it == mpdfn.end() ? nullptr : &it->second;
            std::fprintf(pf, "0x%02X\t%s\t%s\t%d\t0x%06X\t%s\t%s\t%s\t%s\t%s\t%s\n", c.cid, c.name,
                         c.pclsBase ? c.pclsBase->name : "-", slot, addr,
                         cidFrom == c.cid ? "own" : StrClassName(cidFrom).c_str(), reg.empty() ? "-" : reg.c_str(),
                         nameNative ? nameNative : "-", pd ? pd->status.c_str() : "unknown",
                         pd ? pd->name.c_str() : "-", pd && !pd->source.empty() ? pd->source.c_str() : "-");
            ++cslot;
            if (cidFrom == c.cid) ++cown;
            if (!reg.empty()) ++creg;
        }
    }
    std::fclose(pf);
    std::fprintf(stderr, "vt-dump: %d slots (%d own functions, %d with a registered native function) -> %s\n", cslot,
                 cown, creg, path);
    return 0;
}
