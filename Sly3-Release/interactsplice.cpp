// interactsplice.cpp: the splice methods of the actors that start / end their interactions (interact.h): the thunks'
// bodies (RefSetArgsFromSplice, then the game's function on the object). (Apart from interact.cpp: ref.h's SMP.)
#include "actor.h"
#include "clock.h"
#include "interact.h"
#include "jump.h"
#include "jt.h"
#include "objmethod.h"
#include "sw.h"
#include "task.h"
#include "update.h"

void UseObjectBy(LO* puse, ACTOR* pa);  // interact.cpp: the use's vt slot 51 (0x184130)
#include <cstdio>
#include <map>
#include <string>

namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "interact: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}
}  // namespace

// The splice methods on the actors (the thunks' bodies: RefSetArgsFromSplice, then the game's function on the object)
namespace
{
struct INITINTERACTMETHODS
{
    INITINTERACTMETHODS()
    {
        // 0x10C2E8 (the world's, one object: 0x195608, the world's aura on that tracked target: jt.cpp)
        RegisterObjectMethod(0x10C2E8, [](LO*, const std::vector<CRef>& a) {
            ShowJtAura(g_psw, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        // 0x10B028 (one int: the actor's vt slot 88, the state of a jump kind: e.g. 4 the safe house's)
        RegisterObjectMethod(0x10B028, [](LO* plo, const std::vector<CRef>& a) {
            if (ACTOR* pa = PactorFromLo(plo)) NotifyActorJumpKind(pa, NArg(a, 0));
            return CRef();
        });
        // 0x1007E8 (no arguments: 0x133340, the object's vt slot 3 (5): e.g. the safe house's checkpoint 393 from the
        // door's script's event 6, whose splice event 5 lets the safe house take the player (Sly's state 18))
        RegisterObjectMethod(0x1007E8, [](LO* plo, const std::vector<CRef>&) {
            HandleLoSpliceEvent(plo, 5, false, 0, nullptr);  // 0x1A61E0 (the base's slot 3)
            BroadcastTaskEvent(g_psw, 0x1A, plo);            // 0x1619A0
            return CRef(TAGK_Void);
        });
        // 0x10D5D0 (an object: the use's vt slot 51 with it as the user, 0x184130 / cid 0x24's 0x184330): e.g. the
        // intro's SCPT 1674 has Sly use 80398 through its event 4
        RegisterObjectMethod(0x10D5D0, [](LO* plo, const std::vector<CRef>& a) {
            if (ACTOR* pa = PactorFromLo(PloArg(a, 0))) UseObjectBy(plo, pa);
            return CRef(TAGK_Void);
        });
        // 0x10D6E0 (three oids: 0x1840B8, FAddRsmg (trigger) on the use's list +0x118 / +0x11C: the root, its state
        // machine, the state; 4 entries less one at most)
        RegisterObjectMethod(0x10D6E0, [](LO* plo, const std::vector<CRef>& a) {
            AddUseRsmg(plo, NArg(a, 0, -1), NArg(a, 1, -1), NArg(a, 2, -1));
            return CRef(TAGK_Void);
        });
        // 0x10D798 (a task's oid and a state: 0x1840F8, 0x161F10 on the use's list +0x15C / +0x160, 4 at most)
        RegisterObjectMethod(0x10D798, [](LO* plo, const std::vector<CRef>& a) {
            AddUseTask(plo, NArg(a, 0, -1), NArg(a, 1, -1));
            if (std::getenv("SLY2N_TASKREFS"))
                std::fprintf(stderr, "taskref use %d (cid 0x%X): task %d state %d\n", plo->oid, plo->Cid(), NArg(a, 0, -1), NArg(a, 1, -1));
            return CRef(TAGK_Void);
        });
        // 0x10A848 (an object: 0x142140, the interaction of kind 5 on it, null ends it)
        RegisterObjectMethod(0x10A848, [](LO* plo, const std::vector<CRef>& a) {
            if (ACTOR* pa = PactorFromLo(plo)) SetActorInteractObject(pa, 5, PloArg(a, 0));
            return CRef();
        });
        // 0x10AC80 (an object: 0x1421F8, kind 5 with the +0x900 reach)
        RegisterObjectMethod(0x10AC80, [](LO* plo, const std::vector<CRef>& a) {
            if (ACTOR* pa = PactorFromLo(plo))
            {
                LO* pt = PloArg(a, 0);
                SetActorInteractObject(pa, 5, pt);
                if (pt) WarnOnce("0x1421F8's reach +0x900 (0x1CD308)");
            }
            return CRef();
        });
    }
} s_initInteractMethods;
}  // namespace

namespace
{
// orig: 0x0010B598 -> 0x0018DF50 (EOPID 770, option 753 of class 0x35: +0x1D30, the hypnosis request; e.g. world 17's
// Contessa eye cone 65523: its s5 does (Sly 753 #t) for the player entering it; the ground / air / slide states then
// enter state 17)
struct INITHYPNOSIS
{
    INITHYPNOSIS()
    {
        // orig: 0x0010B510 -> 0x0018DE18 (EOPID 769, option 752 of class 0x35): Sly's cane reach toward an object
        // (interaction kind 6, held (+0x9C0 = +0x500 of the interaction: no return to the ground)), or nil: the reach
        // ended when it is the current one (vt slot 88 (0)). E.g. the Ep1 heist's harpoon trigger 124158 (block
        // 138372 module 26): Sly reaches for the harpoon's point 42839 while it flies by
        RegisterObjectMethod(0x10B510, [](LO* plo, const std::vector<CRef>& a) {
            if (SLY* p = PslyFromLo(plo))
            {
                LO* pt = PloArg(a, 0);
                if (pt)
                {
                    p->inter.f500 = 1;  // +0x9C0
                    StartInteract(p, 6, pt, JTL_Max);  // 0x1928F8
                }
                else if (p->n4C8 == 6)
                    NotifyActorJumpKind(p, 0);  // vt slot 88 (0)
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10B598, [](LO* plo, const std::vector<CRef>& a) {
            if (SLY* p = PslyFromLo(plo))
            {
                p->f1D30 = NArg(a, 0);
                if (std::getenv("SLY2N_HYPNOPRINT")) std::printf("hypno: 753 = %d t %.3f\n", p->f1D30, g_clock.t);
            }
            return CRef(TAGK_Void);
        });
    }
} s_inithypnosis;
}

void SetZoneLook(LO* pzone, LO* plo);  // cmworld.cpp
void SetZoneReturn1(LO* pzone, int ib, float g);  // cmworld.cpp
namespace
{
// EOPID 210 (option 339 of the camera zones, class 0xF): the look-at object set at run time
struct INITZONELOOK
{
    INITZONELOOK()
    {
        RegisterObjectSetter(210, [](LO* plo, const CRef& r) { SetZoneLook(plo, PloArg({r}, 0)); });
        RegisterObjectSetter(203, [](LO* plo, const CRef& r) { SetZoneReturn1(plo, 0x170, r.GAsFloat()); });
        RegisterObjectSetter(204, [](LO* plo, const CRef& r) { SetZoneReturn1(plo, 0x174, r.GAsFloat()); });
    }
} s_initzonelook;
}  // namespace
