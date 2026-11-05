#pragma once

// CPair: a list cell (the original's NODE {cref; ?; CRef ref @8; next @0x10}, pool 0x4520A8).

#include "ref.h"

class CPair
{
public:
    CRef m_ref;
    std::shared_ptr<CPair> m_ppairNext;

    // 0x128CB8: a copy of the list from this cell on, each value cloned (CRef::CloneTo).
    void CloneTo(CPair* ppairClone, CFrame* pframeClone) const;
};

std::shared_ptr<CPair> PpairNew();
