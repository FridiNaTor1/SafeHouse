// The actors' companion-follow binding (kept apart from the animation math's SMP type).
#include "actorai.h"
#include "actor.h"
#include "objmethod.h"
namespace
{
struct INITACTORAI
{
    INITACTORAI()
    {
        // orig: 0x0010A940 -> 0x142540 (actor method446, EOPID733): object + offset.
        RegisterObjectMethod(0x10A940, [](LO* plo, const std::vector<CRef>& a) {
            if (ACTOR* pa = PactorFromLo(plo); pa && a.size() >= 2) SetActorFollow(pa, PloArg(a, 0), a[1].GetVector());
            return CRef(TAGK_Void);
        });
        // EOPID 725 (field +0x10F0, a bool): the guards rank the actor 4 for their pursuit / hits / launches only
        // while it is set (0x208E70); e.g. 8:2121's Murray has it off, so the gun guard pursuing him leaves him rank 2
        // and his script's waits for standing (SCPT 0x43) are not held by a "pursued" state 9
        RegisterObjectSetter(725, [](LO* plo, const CRef& r) {
            if (ACTOR* pa = PactorFromLo(plo)) pa->f10F0 = r.m_tagk == TAGK_Bool ? r.m_bool : r.m_n;
        });
        RegisterObjectGetter(725, [](LO* plo) {
            CRef r;
            ACTOR* pa = PactorFromLo(plo);
            r.SetBool(pa && pa->f10F0);
            return r;
        });
    }
} s_initactorai;
}  // namespace
