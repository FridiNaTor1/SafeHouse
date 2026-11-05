#include "sidebag.h"

// orig: 0x00257D80 PsidebagNew
std::shared_ptr<CSidebag> PsidebagNew()
{
    return std::make_shared<CSidebag>();
}

// orig: 0x0012A980 RefAddBinding__8CSidebagiP4CRef
CRef CSidebag::RefAddBinding(int n, CRef* pref)
{
    m_asbb.push_back({n, *pref});
    return CRef(TAGK_Void);
}

// orig: 0x0012AB28 FFindBinding__8CSidebagiP4CRef
int CSidebag::FFindBinding(int n, CRef* pref)
{
    for (auto it = m_asbb.rbegin(); it != m_asbb.rend(); ++it)
    {
        if (it->n == n)
        {
            if (pref != nullptr)
                *pref = it->ref;
            return 1;
        }
    }
    return 0;
}

// orig: 0x0012AA70 RefSetBinding__8CSidebagiP4CRef
CRef CSidebag::RefSetBinding(int n, CRef* pref)
{
    for (auto it = m_asbb.rbegin(); it != m_asbb.rend(); ++it)
    {
        if (it->n == n)
        {
            it->ref = *pref;
            break;
        }
    }
    return CRef(TAGK_Void);
}

// orig: 0x0012AB78 CloneTo__8CSidebag (0x12B0E8 clones each binding's value)
void CSidebag::CloneTo(CSidebag* psidebagClone, CFrame* pframeClone)
{
    psidebagClone->m_asbb.clear();
    for (const SBB& sbb : m_asbb)
    {
        SBB sbbClone{sbb.n, {}};
        sbb.ref.CloneTo(&sbbClone.ref, pframeClone);
        psidebagClone->m_asbb.push_back(std::move(sbbClone));
    }
}
