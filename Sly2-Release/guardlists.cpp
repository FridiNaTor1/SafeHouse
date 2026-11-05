// guardlists.cpp: a guard's two object lists set by scripts (option 426), read by job 0xB2 (aipursuit.cpp)
#include "guard.h"
#include "objmethod.h"
#include "pair.h"
#include "lo.h"

#include <cstdio>
#include <cstdlib>

// orig: 0x00103B38 -> 0x0020A058 (a guard's option 426 (EOPID 302): two lists of at most 4 objects (0x2013D8), +0xF94 /
// +0xF98 and +0xFA8 / +0xFAC; job 0xB2 attacks the first list's objects, the second's when near)
namespace
{
int CListObjects(const CRef& r, LO** aplo)
{
    int c = 0;
    if (r.m_tagk == TAGK_Pair)
        for (const CPair* p = r.m_ppair.get(); p && c < 4; p = p->m_ppairNext.get())
        {
            LO* plo = p->m_ref.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(p->m_ref.m_pbasic) : nullptr;
            if (plo) aplo[c++] = plo;
        }
    else if (r.m_tagk == TAGK_Basic)
        if (LO* plo = dynamic_cast<LO*>(r.m_pbasic)) aplo[c++] = plo;
    for (int i = c; i < 4; ++i) aplo[i] = nullptr;
    return c;
}
struct INITGUARDLISTS
{
    INITGUARDLISTS()
    {
        RegisterObjectMethod(0x103B38, [](LO* plo, const std::vector<CRef>& a) {
            if (GUARD* pg = PguardFromLo(plo))
            {
                pg->fF94 = a.size() > 0 ? CListObjects(a[0], pg->aploF98) : 0;
                pg->fFA8 = a.size() > 1 ? CListObjects(a[1], pg->aploFAC) : 0;
                if (std::getenv("SLY2N_B2PRINT"))
                    std::printf("guard %d option 426: %d / %d objects (%d, %d)\n", plo->oid, pg->fF94, pg->fFA8, pg->aploF98[0] ? pg->aploF98[0]->oid : -1,
                                pg->aploFAC[0] ? pg->aploFAC[0]->oid : -1);
            }
            return CRef(TAGK_Void);
        });
    }
} s_initguardlists;
}  // namespace
