// Sly 3's moves (the player code): Sly 3-only pieces of the traversal moves, written from the Sly 3 asm (orig3 addresses).
#include "actor.h"
#include "alo.h"
#include "cls.h"
#include "game/sly3.h"
#include "lo.h"
#include "objmethod.h"
#include "so.h"

// orig3: 0x00109BD0 (the players' option 812 (option name 874, Sly 3 only), CRefs (i, oid, tag): 0x1C20D0 keeps them in
// the actor's sub-object +0xE40 (14 entries of 0x10 at +0x2C); its PostLoad 0x1C2058 / 0x1C2138 (from the actors'
// 0x152488) finds the object by oid (PloFindSwNearest kind 1) and the collision entry by tag (0x1F6030) and turns the
// entry off (0x23B978). Sly's 7676 / 7685 / 7687 / 7689 (his long body boxes) are off this way in the reference from
// the load on: natively they stayed on and pushed him off the ledges / spire tops. (What enables them again, and what
// the objects are for: TODO(player).)
namespace
{
struct INITSLY3SLOTS
{
    INITSLY3SLOTS()
    {
        RegisterObjectMethod3(0x109BD0, [](LO* plo, const std::vector<CRef>& a) {
            const int i = NArg(a, 0, -1), oid = NArg(a, 1, -1), tag = NArg(a, 2, -1);
            if (ACTOR* pa = PactorFromLo(plo); pa && i >= 0 && i < 14) pa->aslot3E40[i] = {oid, tag};
            if (tag != -1 && FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
                for (SOE& e : static_cast<ALO*>(plo)->asoe)
                    if (e.tag == tag)
                    {
                        SetSoeOff(static_cast<ALO*>(plo), &e);
                        break;
                    }
            return CRef(TAGK_Void);
        });
    }
} s_initsly3slots;
}  // namespace
