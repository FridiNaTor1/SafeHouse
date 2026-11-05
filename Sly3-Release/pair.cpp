#include "pair.h"

// orig: 0x00256B48 PpairNew (a node from the pool)
std::shared_ptr<CPair> PpairNew()
{
    return std::make_shared<CPair>();
}

// orig: 0x00128CB8 CloneTo__5CPair (the cell's value cloned, then the rest of the list)
void CPair::CloneTo(CPair* ppairClone, CFrame* pframeClone) const
{
    const CPair* ppair = this;
    CPair* ppairDst = ppairClone;
    for (;;)
    {
        ppair->m_ref.CloneTo(&ppairDst->m_ref, pframeClone);
        if (!ppair->m_ppairNext)
        {
            ppairDst->m_ppairNext.reset();
            break;
        }
        ppairDst->m_ppairNext = PpairNew();
        ppairDst = ppairDst->m_ppairNext.get();
        ppair = ppair->m_ppairNext.get();
    }
}
