#include "readtrace.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace loadtrace
{
bool g_fOn = false;
std::vector<uint32_t> g_afnStack;

namespace
{
struct RUN
{
    std::size_t ibStart = 0;
    std::size_t ibEnd = 0;
    uint32_t fn = 0;
    std::vector<std::pair<const char*, int>> akind;  // in first-seen order, like loadmap.py
};
std::vector<RUN> s_arun;
}

void Begin() { s_arun.clear(); }

void Read(const char* kind, std::size_t ib)
{
    if (!g_fOn) return;
    const uint32_t fn = g_afnStack.empty() ? 0 : g_afnStack.back();
    if (!s_arun.empty() && s_arun.back().fn == fn)
    {
        RUN& run = s_arun.back();
        run.ibEnd = ib;
        for (auto& [k, c] : run.akind)
            if (k == kind || std::strcmp(k, kind) == 0) { ++c; return; }
        run.akind.emplace_back(kind, 1);
        return;
    }
    if (!s_arun.empty()) s_arun.back().ibEnd = ib;
    RUN run;
    run.ibStart = run.ibEnd = ib;
    run.fn = fn;
    run.akind.emplace_back(kind, 1);
    s_arun.push_back(std::move(run));
}

void End(std::size_t ib)
{
    if (g_fOn && !s_arun.empty()) s_arun.back().ibEnd = ib;
}

bool Write(const std::string& path)
{
    FILE* pf = std::fopen(path.c_str(), "w");
    if (!pf) return false;
    for (const RUN& run : s_arun)
    {
        std::fprintf(pf, "%9zu %9zu 0x%06X func_%08X", run.ibStart, run.ibEnd, run.fn, run.fn);
        for (const auto& [k, c] : run.akind) std::fprintf(pf, " %sx%d", k, c);
        std::fputc('\n', pf);
    }
    std::fclose(pf);
    return true;
}
}
