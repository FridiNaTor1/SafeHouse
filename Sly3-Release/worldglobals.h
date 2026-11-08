#pragma once

// PC addition, not in the original game (debug menu only: pcloader.h). The world loader (FLoadWorldFile and the
// start's record pass) fills and resets global tables; the original loads one world at a time. To load another world's
// file while one runs (a kept world, pcloader.h), each such table is registered here and moved aside for that load:
//
//   WORLD_TABLE(g)   moved out and left default-constructed (T{}) for the load; after it, whatever the load put in it
//                    is destroyed and the running world's contents are moved back (same address: pointers to the
//                    global stay valid; pointers into a container's heap storage too).
//   WORLD_VALUE(g)   copied for the load and copied back after it (counters, the current world pointer, options):
//                    the load starts from the running world's value.
//
// The tables are registered next to their definitions (static initializers). SLY2N_GLOBALDIFF=<nm list> checks the
// set: every writable global's bytes before the stash and after the restore are compared and the changed ones printed
// (tools: `nm -S -C --defined-only Sly2-Release | awk '$3 ~ /^[bBdD]$/'`).

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace worldglobals
{
struct ENTRY
{
    const char* pchz;
    void* pv;
    std::size_t cb;
    std::function<std::shared_ptr<void>()> pfnStash;   // returns the saved state
    std::function<void(std::shared_ptr<void>&)> pfnRestore;
    std::function<void()> pfnClear;   // (registered during a kept load, a function's first call: it was empty before)
    const char* pszFile = nullptr;    // the registering source file: the entries are kept in the files' name order
};
void Register(ENTRY e);

template <class T> void MoveInto(T& dst, T& src)
{
    if constexpr (std::is_array_v<T>)
        for (std::size_t i = 0; i < std::extent_v<T>; ++i) MoveInto(dst[i], src[i]);
    else
        dst = std::move(src);
}
template <class T> void CopyInto(T& dst, const T& src)
{
    if constexpr (std::is_array_v<T>)
        for (std::size_t i = 0; i < std::extent_v<T>; ++i) CopyInto(dst[i], src[i]);
    else
        dst = src;
}
template <class T> void Clear(T& g)
{
    if constexpr (std::is_array_v<T>)
        for (auto& e : g) Clear(e);
    else
        g = T{};
}
template <class T> struct HOLD { T v; };

template <class T> struct TABLEREG
{
    TABLEREG(const char* pchz, T& g, const char* pszFile = __builtin_FILE())
    {
        Register(ENTRY{pchz, &g, sizeof(T),
                       [&g]() -> std::shared_ptr<void> {
                           auto p = std::make_shared<HOLD<T>>();
                           MoveInto(p->v, g);
                           Clear(g);
                           return p;
                       },
                       [&g](std::shared_ptr<void>& p) {
                           Clear(g);   // (the load's contents destroyed first)
                           MoveInto(g, static_cast<HOLD<T>*>(p.get())->v);
                           p.reset();
                       },
                       [&g]() { Clear(g); }, pszFile});
    }
};
template <class T> struct VALUEREG
{
    VALUEREG(const char* pchz, T& g, const char* pszFile = __builtin_FILE())
    {
        Register(ENTRY{pchz, &g, sizeof(T),
                       [&g]() -> std::shared_ptr<void> {
                           auto p = std::make_shared<HOLD<T>>();
                           CopyInto(p->v, g);
                           return p;
                       },
                       [&g](std::shared_ptr<void>& p) {
                           CopyInto(g, static_cast<HOLD<T>*>(p.get())->v);
                           p.reset();
                       },
                       nullptr, pszFile});
    }
};
}  // namespace worldglobals

#define WG_CAT2(a, b) a##b
#define WG_CAT(a, b) WG_CAT2(a, b)
#define WORLD_TABLE(g) static ::worldglobals::TABLEREG<std::remove_reference_t<decltype(g)>> WG_CAT(s_wgtable_, __LINE__)(#g, g)
#define WORLD_TABLE_LOCAL(g) static ::worldglobals::TABLEREG<std::remove_reference_t<decltype(g)>> WG_CAT(s_wgtablel_, __LINE__)(#g, g)
#define WORLD_VALUE(g) static ::worldglobals::VALUEREG<std::remove_reference_t<decltype(g)>> WG_CAT(s_wgvalue_, __LINE__)(#g, g)

// True while a kept world loads (pcloader.h): the loader's steps that act on the running world's renderer skip it.
extern bool g_fKeptWorldLoad;

// The running world's tables moved aside / back (pcloader.cpp). Nested calls are not allowed. A table first registered
// while they are aside (a function-local table's first use) is cleared at the restore.
// (In a function, after its local static table: WORLD_TABLE_LOCAL(s_map).)
struct WGSTASH
{
    std::vector<std::shared_ptr<void>> ap;
};
std::unique_ptr<WGSTASH> PwgStashWorldGlobals();
void RestoreWorldGlobals(std::unique_ptr<WGSTASH> p);

// (debug, SLY2N_GLOBALDIFF) the writable globals' bytes kept / compared; prints the changed symbols' names
void SnapshotGlobalsForDiff();
void ReportGlobalsDiff(const char* pchzWhat);
