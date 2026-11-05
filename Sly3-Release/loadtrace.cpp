#include "loadtrace.h"

#include "bis.h"
#include "readtrace.h"

CLoadTrace* g_ploadtrace = nullptr;

void CLoadTrace::Close(std::size_t ib)
{
    if (m_aaddr.empty() || ib == m_ibSegment)
        return;
    const uint32_t addr = m_aaddr.back();
    if (!m_arun.empty() && m_arun.back().addr == addr && m_arun.back().ibEnd == m_ibSegment)
        m_arun.back().ibEnd = ib;
    else
        m_arun.push_back({m_ibSegment, ib, addr});
}

void CLoadTrace::Enter(uint32_t addr, std::size_t ib)
{
    Close(ib);
    m_aaddr.push_back(addr);
    m_ibSegment = ib;
}

void CLoadTrace::Leave(std::size_t ib)
{
    Close(ib);
    if (!m_aaddr.empty())
        m_aaddr.pop_back();
    m_ibSegment = ib;
}

CLoadScope::CLoadScope(uint32_t addr, const CBinaryInputStream* pbis)
    : m_pbis(pbis), m_fTraced(g_ploadtrace != nullptr && g_ploadtrace->m_pbis == pbis)
{
    // (also the world loader's read trace, readtrace.h: per-read attribution to the innermost scope)
    loadtrace::g_afnStack.push_back(addr);
    if (m_fTraced)
        g_ploadtrace->Enter(addr, pbis->m_ib);
}

CLoadScope::~CLoadScope()
{
    loadtrace::g_afnStack.pop_back();
    if (m_fTraced && g_ploadtrace != nullptr)
        g_ploadtrace->Leave(m_pbis->m_ib);
}
