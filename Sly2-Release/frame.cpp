#include "frame.h"

#include "gc.h"

// orig: 0x00256DD0 PframeNew (a frame from the pool: no parents, no bindings)
std::shared_ptr<CFrame> PframeNew()
{
    auto pframe = std::make_shared<CFrame>();
    g_gc.AddFrame(pframe);
    return pframe;
}

// orig: 0x00127C00 RefAddBinding__6CFrameUiP4CRef
CRef CFrame::RefAddBinding(SYMID symid, CRef* pref)
{
    m_avarb.push_back({symid, *pref});
    return CRef(TAGK_Void);
}

// orig: 0x00127CF0 RefSetBinding__6CFrameUiP4CRef
CRef CFrame::RefSetBinding(SYMID symid, CRef* pref)
{
    if (CRef* prefFound = PrefFindBinding(symid, 1))
        *prefFound = *pref;
    return CRef(TAGK_Void);
}

// orig: 0x00127D78 FFindBinding__6CFrameUiiP4CRef
int CFrame::FFindBinding(SYMID symid, int fRecursive, CRef* pref)
{
    CRef* prefFound = PrefFindBinding(symid, fRecursive);
    if (prefFound == nullptr)
        return 0;
    if (pref != nullptr)
        *pref = *prefFound;
    return 1;
}

// orig: 0x00127DB8 PrefFindBinding__6CFrameUii (here, newest first, else (fRecursive) in the parents not
// being searched already)
CRef* CFrame::PrefFindBinding(SYMID symid, int fRecursive)
{
    m_fVisited = 1;
    for (auto it = m_avarb.rbegin(); it != m_avarb.rend(); ++it)
    {
        if (it->symid == symid)
        {
            m_fVisited = 0;
            return &it->ref;
        }
    }
    CRef* pref = nullptr;
    if (fRecursive)
    {
        for (int i = 0; i < m_cpframeParent; i++)
        {
            CFrame* pframeParent = m_apframeParent[i];
            if (pframeParent == nullptr || pframeParent->m_fVisited)
                continue;
            pref = pframeParent->PrefFindBinding(symid, fRecursive);
            if (pref != nullptr)
                break;
        }
    }
    m_fVisited = 0;
    return pref;
}

// orig: 0x00127B18 SetSingleParent__6CFrameP6CFrame
void CFrame::SetSingleParent(CFrame* pframeParent)
{
    for (auto& p : m_apframeParentOwner)
        p.reset();
    m_cpframeParent = 1;
    m_apframeParent[0] = pframeParent;
    m_apframeParentOwner[0] = pframeParent != nullptr ? pframeParent->shared_from_this() : nullptr;
}

// orig: 0x00127B40 AddParent__6CFrameP6CFrame
void CFrame::AddParent(CFrame* pframeParent)
{
    if (pframeParent == nullptr || m_cpframeParent >= 4)
        return;
    m_apframeParent[m_cpframeParent] = pframeParent;
    m_apframeParentOwner[m_cpframeParent] = pframeParent->shared_from_this();
    m_cpframeParent++;
}

// orig: 0x00127B70 RemoveParent__6CFrameP6CFrame
void CFrame::RemoveParent(CFrame* pframeParent)
{
    for (int i = 0; i < m_cpframeParent; ++i)
    {
        if (m_apframeParent[i] != pframeParent) continue;
        m_apframeParent[i] = nullptr;
        m_apframeParentOwner[i].reset();
        --m_cpframeParent;
        // The original's following CopyAb reads the released pointer value (now null) as its destination,
        // not this array slot's address (0x127BC0..0x127BCC); it does not compact the frame's parent slots.
        return;
    }
}

// orig: 0x00127E90 CloneTo__6CFrame (the parents shared, each binding's value cloned for the new frame)
void CFrame::CloneTo(CFrame* pframeClone)
{
    pframeClone->m_cpframeParent = m_cpframeParent;
    for (int i = 0; i < 4; i++)
    {
        pframeClone->m_apframeParent[i] = m_apframeParent[i];
        pframeClone->m_apframeParentOwner[i] = m_apframeParentOwner[i];
    }
    pframeClone->m_avarb.clear();
    for (const VARB& varb : m_avarb)
    {
        VARB varbClone{varb.symid, {}};
        varb.ref.CloneTo(&varbClone.ref, pframeClone);
        pframeClone->m_avarb.push_back(std::move(varbClone));
    }
}
