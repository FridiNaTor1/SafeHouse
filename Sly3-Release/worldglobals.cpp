// PC addition, not in the original game (debug menu only): the world loader's global tables moved aside while a kept
// world loads (worldglobals.h, pcloader.h).
#include "worldglobals.h"

#if defined(__linux__)
#include <link.h>   // (the SLY2N_GLOBALDIFF check: Linux only)
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace worldglobals
{
namespace
{
std::vector<ENTRY>& Aentry()
{
    static std::vector<ENTRY> s_a;
    return s_a;
}
}  // namespace

constinit bool s_fStashed = false;

// In the registering source files' name order (Linux's link order; Windows' CRT runs the static initializers in reverse:
// PACKAGING.md section 7), so the tables are moved aside and back in the same order on both. While the tables are
// aside, a new one (a function-local table's first use) is appended: the stash's entries keep their indices.
void Register(ENTRY e)
{
    auto& a = Aentry();
    auto it = a.end();
    if (!s_fStashed && e.pszFile)
    {
        const char* pszBase = e.pszFile;
        for (const char* pch = e.pszFile; *pch; ++pch)
            if (*pch == '/' || *pch == '\\') pszBase = pch + 1;
        e.pszFile = pszBase;
        while (it != a.begin() && (it - 1)->pszFile && std::strcmp((it - 1)->pszFile, pszBase) > 0) --it;
    }
    a.insert(it, std::move(e));
}
}  // namespace worldglobals

bool g_fKeptWorldLoad = false;

using worldglobals::s_fStashed;

std::unique_ptr<WGSTASH> PwgStashWorldGlobals()
{
    if (s_fStashed) std::abort();   // (nested: a bug)
    s_fStashed = true;
    auto p = std::make_unique<WGSTASH>();
    auto& a = worldglobals::Aentry();
    p->ap.reserve(a.size());
    for (auto& e : a) p->ap.push_back(e.pfnStash());
    return p;
}

void RestoreWorldGlobals(std::unique_ptr<WGSTASH> p)
{
    auto& a = worldglobals::Aentry();
    // (in reverse: a table registered after another may point into it)
    for (size_t i = a.size(); i-- > 0;)
        if (i < p->ap.size()) a[i].pfnRestore(p->ap[i]);
        else if (a[i].pfnClear) a[i].pfnClear();
    s_fStashed = false;
}

// ------------------------------------------------------------ the check (SLY2N_GLOBALDIFF=<nm list>)
namespace
{
struct SYM
{
    uintptr_t addr;
    size_t cb;
    std::string name;
    std::vector<unsigned char> ab;
};
std::vector<SYM> s_asym;
bool s_fSymLoaded = false;

#if defined(__linux__)
int CbPhdr(struct dl_phdr_info* info, size_t, void* pv)
{
    // the first entry is the program itself
    *static_cast<uintptr_t*>(pv) = info->dlpi_addr;
    return 1;
}
#endif

void LoadSyms()
{
    if (s_fSymLoaded) return;
    s_fSymLoaded = true;
    const char* sz = std::getenv("SLY2N_GLOBALDIFF");
    if (!sz) return;
    uintptr_t bias = 0;
#if defined(__linux__)
    dl_iterate_phdr(CbPhdr, &bias);
#else
    std::printf("global diff: SLY2N_GLOBALDIFF is Linux only\n");
    return;
#endif
    std::ifstream in(sz);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        std::string sAddr, sSize, sType;
        if (!(ss >> sAddr >> sSize >> sType)) continue;
        std::string name;
        std::getline(ss, name);
        if (!name.empty() && name[0] == ' ') name.erase(0, 1);
        if (sType != "b" && sType != "B" && sType != "d" && sType != "D") continue;
        const size_t cb = std::strtoull(sSize.c_str(), nullptr, 16);
        if (cb == 0 || cb > (64u << 20)) continue;
        // (the check's own tables and the standard streams change by themselves)
        if (name.find("s_asym") != std::string::npos || name.find("std::") == 0 || name.find("__gnu") == 0) continue;
        s_asym.push_back(SYM{std::strtoull(sAddr.c_str(), nullptr, 16) + bias, cb, name, {}});
    }
    std::printf("global diff: %zu symbols\n", s_asym.size());
}
}  // namespace

void SnapshotGlobalsForDiff()
{
    LoadSyms();
    for (SYM& s : s_asym) s.ab.assign(reinterpret_cast<const unsigned char*>(s.addr), reinterpret_cast<const unsigned char*>(s.addr) + s.cb);
}

void ReportGlobalsDiff(const char* pchzWhat)
{
    if (s_asym.empty()) return;
    int c = 0;
    for (const SYM& s : s_asym)
    {
        if (s.ab.size() != s.cb) continue;
        if (std::memcmp(s.ab.data(), reinterpret_cast<const void*>(s.addr), s.cb) == 0) continue;
        size_t cDiff = 0;
        for (size_t i = 0; i < s.cb; ++i) cDiff += s.ab[i] != reinterpret_cast<const unsigned char*>(s.addr)[i];
        std::printf("global diff (%s): %s (%zu bytes, %zu changed)\n", pchzWhat, s.name.c_str(), s.cb, cDiff);
        ++c;
    }
    std::printf("global diff (%s): %d changed\n", pchzWhat, c);
    std::fflush(stdout);
}
