#pragma once

// CFrame: an environment (the original's FRAME {cref; ?; c @8; SPX apframe[4] @0xC (parents);
// SPW pbinding @0x1C (bindings, newest first); fBusy @0x20 (the search's cycle guard); fModule @0x24}).

#include <deque>

#include "ref.h"

struct VARB
{
    SYMID symid;
    CRef ref;
};

class CFrame : public std::enable_shared_from_this<CFrame>
{
public:
    int m_cpframeParent = 0;
    CFrame* m_apframeParent[4] = {};
    std::shared_ptr<CFrame> m_apframeParentOwner[4];
    std::deque<VARB> m_avarb;  // oldest first; searched from the back (the original prepends)
    int m_fVisited = 0;
    int m_fModule = 0;         // +0x24: a script table entry's frame (0x1FFA70)

    // 0x127C00: a new binding (shadowing any older one of symid); returns void
    CRef RefAddBinding(SYMID symid, CRef* pref);
    // 0x127CF0: the found binding (if any) takes *pref; returns void
    CRef RefSetBinding(SYMID symid, CRef* pref);
    // 0x127D78
    int FFindBinding(SYMID symid, int fRecursive, CRef* pref);
    // 0x127DB8
    CRef* PrefFindBinding(SYMID symid, int fRecursive);
    // 0x127B18
    void SetSingleParent(CFrame* pframeParent);
    // 0x127B40 / 0x127B70
    void AddParent(CFrame* pframeParent);
    void RemoveParent(CFrame* pframeParent);
    // 0x127E90: the parents and a clone of the bindings
    void CloneTo(CFrame* pframeClone);
};

// 0x256DD0
std::shared_ptr<CFrame> PframeNew();
