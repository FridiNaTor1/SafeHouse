#pragma once

// Load tracing: which ported loader read which byte range of a stream, in the form of the original's parse maps
// (Sly2-Reference SLY2_LOADTRACE + tools/loadmap.py: "start end 0xFUNC ..." per run of reads by one function).
//
// A loader marks itself with LOAD_SCOPE(0xADDR, pbis) (the original's address). While a CLoadTrace is installed
// (g_ploadtrace, normally null: then a scope costs one test), every stretch of the stream consumed while a
// scope is innermost is attributed to that scope's function, which is exactly the map's attribution (the
// function that called the reader), provided every ported function that reads carries a scope.

#include <cstddef>
#include <cstdint>
#include <vector>

class CBinaryInputStream;

struct LOADRUN
{
    std::size_t ibStart;
    std::size_t ibEnd;
    uint32_t addr;  // the original function
};

class CLoadTrace
{
public:
    explicit CLoadTrace(const CBinaryInputStream* pbis) : m_pbis(pbis) {}

    void Enter(uint32_t addr, std::size_t ib);
    void Leave(std::size_t ib);

    const CBinaryInputStream* m_pbis;
    std::vector<LOADRUN> m_arun;

private:
    void Close(std::size_t ib);

    std::vector<uint32_t> m_aaddr;  // the scopes, innermost last
    std::size_t m_ibSegment = 0;    // where the innermost scope's current stretch began
};

extern CLoadTrace* g_ploadtrace;

class CLoadScope
{
public:
    CLoadScope(uint32_t addr, const CBinaryInputStream* pbis);
    ~CLoadScope();

    CLoadScope(const CLoadScope&) = delete;
    CLoadScope& operator=(const CLoadScope&) = delete;

private:
    const CBinaryInputStream* m_pbis;
    bool m_fTraced;
};

#define LOAD_SCOPE_CAT2(a, b) a##b
#define LOAD_SCOPE_CAT(a, b) LOAD_SCOPE_CAT2(a, b)
#define LOAD_SCOPE(addr, pbis) CLoadScope LOAD_SCOPE_CAT(loadscope_, __LINE__)((addr), (pbis))
