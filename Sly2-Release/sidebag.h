#pragma once

// CSidebag: an object's private bindings (the original's {cref; ?; pbinding @8}, bindings
// {cref; ?; n @8; CRef @0xC; next @0x14}, newest first; module 0x12A980).

#include <deque>

#include "ref.h"

struct SBB
{
    int n;
    CRef ref;
};

class CSidebag
{
public:
    std::deque<SBB> m_asbb;  // oldest first; searched from the back

    // 0x12A980
    CRef RefAddBinding(int n, CRef* pref);
    // 0x12AB28
    int FFindBinding(int n, CRef* pref);
    // 0x12AA70
    CRef RefSetBinding(int n, CRef* pref);
    // 0x12AB78
    void CloneTo(CSidebag* psidebagClone, CFrame* pframeClone = nullptr);
};

std::shared_ptr<CSidebag> PsidebagNew();
