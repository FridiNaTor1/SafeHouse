#include "gc.h"

#include <algorithm>

#include "frame.h"
#include "sidebag.h"

CGc g_gc;

void CGc::Startup()
{
    m_apframeRoot.clear();
    m_apsidebagRoot.clear();
    m_apframeAll.clear();
    m_cpframeLive = 0;
}

// orig: 0x00259A88 (a frame made a root of the collector)
void CGc::AddRootFrame(CFrame* pframe)
{
    if (pframe == nullptr)
        return;
    for (const auto& p : m_apframeRoot)
        if (p.get() == pframe)
            return;
    m_apframeRoot.push_back(pframe->shared_from_this());
}

void CGc::AddRootSidebag(CSidebag* psidebag)
{
    if (psidebag != nullptr && std::find(m_apsidebagRoot.begin(), m_apsidebagRoot.end(), psidebag) == m_apsidebagRoot.end())
        m_apsidebagRoot.push_back(psidebag);
}

void CGc::AddFrame(const std::shared_ptr<CFrame>& pframe)
{
    m_apframeAll.push_back(pframe);
    if (m_apframeAll.size() > 2 * m_cpframeLive + 4096)
    {
        std::erase_if(m_apframeAll, [](const std::weak_ptr<CFrame>& p) { return p.expired(); });
        m_cpframeLive = m_apframeAll.size();
    }
}

// orig: 0x001286E0 (mark from the roots, sweep the pools)
void CGc::Collect()
{
    std::erase_if(m_apframeAll, [](const std::weak_ptr<CFrame>& p) { return p.expired(); });
    m_cpframeLive = m_apframeAll.size();
}

void CGc::ShutDown()
{
    // Break the cycles: every live frame lets go of its bindings and parents.
    for (auto& pw : m_apframeAll)
    {
        if (auto pframe = pw.lock())
        {
            pframe->m_avarb.clear();
            for (auto& p : pframe->m_apframeParentOwner)
                p.reset();
            pframe->m_cpframeParent = 0;
        }
    }
    Startup();
}
