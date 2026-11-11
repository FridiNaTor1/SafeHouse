#pragma once
// The cores worth giving a frame's parallel work to (Sly2-Release / Sly3-Release passhelpers.h): the CPUs this process
// may run on (sched_getaffinity: taskset, cgroups, Android's cpusets), on a big.LITTLE SoC only its fast ones, and the
// hardware threads of one core (SMT) counted once.
//   Linux / Android: cpu_capacity (the scheduler's relative capacity of each CPU) at least half the highest; without it
//   cpufreq's maximum at least 3/4 of the highest. The cores by their topology's (package, core) pairs.
//   Windows: the cores of the highest efficiency class (Intel's P-cores) among those the process may use.
//   macOS: the performance cores (hw.perflevel0.physicalcpu), else the physical cores.
// 0 when nothing could be read: the caller falls back to std::thread::hardware_concurrency.
#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace cane
{
#if defined(__linux__) || defined(__ANDROID__)
namespace cpucores_detail
{
inline long ReadLong(int cpu, const char* leaf)
{
    char path[160];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", cpu, leaf);
    FILE* pf = std::fopen(path, "r");
    if (!pf) return -1;
    long v = -1;
    if (std::fscanf(pf, "%ld", &v) != 1) v = -1;
    std::fclose(pf);
    return v;
}
}  // namespace cpucores_detail
#endif

inline int CFastCores()
{
#if defined(__linux__) || defined(__ANDROID__)
    using namespace cpucores_detail;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return 0;
    std::vector<int> acpu;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &set)) acpu.push_back(i);
    if (acpu.empty()) return 0;
    // (each CPU's speed class: the capacity where the kernel gives one, else the maximum frequency)
    std::vector<long> acap(acpu.size()), afreq(acpu.size());
    long capMax = 0, freqMax = 0;
    for (size_t i = 0; i < acpu.size(); ++i)
    {
        acap[i] = ReadLong(acpu[i], "cpu_capacity");
        afreq[i] = ReadLong(acpu[i], "cpufreq/cpuinfo_max_freq");
        capMax = std::max(capMax, acap[i]);
        freqMax = std::max(freqMax, afreq[i]);
    }
    std::set<std::pair<long, long>> setCore;
    int cAnon = 0;
    for (size_t i = 0; i < acpu.size(); ++i)
    {
        if (capMax > 0 && acap[i] >= 0 && acap[i] * 2 < capMax) continue;
        if (capMax <= 0 && freqMax > 0 && afreq[i] >= 0 && afreq[i] * 4 < freqMax * 3) continue;
        const long pkg = ReadLong(acpu[i], "topology/physical_package_id"), core = ReadLong(acpu[i], "topology/core_id");
        if (core < 0) ++cAnon;
        else setCore.insert({pkg, core});
    }
    return int(setCore.size()) + cAnon;
#elif defined(_WIN32)
    DWORD cb = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &cb);
    if (cb == 0) return 0;
    std::vector<unsigned char> ab(cb);
    auto* pFirst = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ab.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, pFirst, &cb)) return 0;
    DWORD_PTR maskProcess = 0, maskSystem = 0;
    const bool fMask = GetProcessAffinityMask(GetCurrentProcess(), &maskProcess, &maskSystem) != 0;
    WORD groupProcess = 0;
    {
        USHORT cgroup = 1;
        USHORT agroup[1] = {0};
        if (GetProcessGroupAffinity(GetCurrentProcess(), &cgroup, agroup) && cgroup == 1) groupProcess = agroup[0];
    }
    int classMax = -1, c = 0;
    for (DWORD off = 0; off < cb;)
    {
        auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ab.data() + off);
        if (p->Relationship == RelationProcessorCore)
        {
            bool fAllowed = !fMask;
            for (WORD g = 0; g < p->Processor.GroupCount && !fAllowed; ++g)
                fAllowed = p->Processor.GroupMask[g].Group == groupProcess && (p->Processor.GroupMask[g].Mask & maskProcess) != 0;
            if (fAllowed)
            {
                const int cls = p->Processor.EfficiencyClass;
                if (cls > classMax) classMax = cls, c = 0;
                if (cls == classMax) ++c;
            }
        }
        off += p->Size;
    }
    return c;
#elif defined(__APPLE__)
    int n = 0;
    size_t cb = sizeof n;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &n, &cb, nullptr, 0) == 0 && n > 0) return n;
    cb = sizeof n;
    if (sysctlbyname("hw.physicalcpu", &n, &cb, nullptr, 0) == 0 && n > 0) return n;
    return 0;
#else
    return 0;
#endif
}
}  // namespace cane
