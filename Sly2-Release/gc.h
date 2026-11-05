#pragma once

// CGc: the splice heaps' collector (the original's mark/sweep over pools, m_128138: roots = the frames list
// 0x2DD598 and the sidebags; 0x1286E0 collects). Natively, values are refcounted (shared_ptr); what refcounts
// cannot free are cycles (a frame binding a lambda closed over it). The collector keeps the roots and every
// frame made, and ShutDown (world unload) empties the frames' bindings, which breaks every cycle.

#include <memory>
#include <vector>

class CFrame;
class CSidebag;

class CGc
{
public:
    void Startup();
    // 0x259A88 on a frame: a root (the world's and each script table entry's frame)
    void AddRootFrame(CFrame* pframe);
    void AddRootSidebag(CSidebag* psidebag);
    void AddFrame(const std::shared_ptr<CFrame>& pframe);
    // 0x1286E0: nothing to do natively (refcounts free what is unreachable, except cycles)
    void Collect();
    void ShutDown();

    std::vector<std::shared_ptr<CFrame>> m_apframeRoot;
    std::vector<CSidebag*> m_apsidebagRoot;

private:
    std::vector<std::weak_ptr<CFrame>> m_apframeAll;
    std::size_t m_cpframeLive = 0;
};

extern CGc g_gc;
