// navlink.cpp: the links between navigation regions (class 0x7C: a way from one region to another, walked (kind 0)
// or jumped (kind 1)) and the search over them (0x1C21D8: A* over the regions, the links as edges), for the guards'
// go-to (0x17D8A8 / 0x17DA28) and the link's job 0xBE. Part of the AI code. Readable C: sly2-decomp
// build/hand_wip/func_001C21D8.cpp, func_001C26E0.c, src/hand/func_0017E458.c.

#include "navlink.h"

#include "aijobs.h"
#include "animfind.h"
#include "clock.h"
#include "crv.h"
#include "guard.h"
#include "lo.h"
#include "nav.h"
#include "objmethod.h"
#include "so.h"
#include "sw.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
// the guards' allowed regions (option 301's oids, +0xE94 count / +0xE98 the objects; 8 at most)
std::unordered_map<const LO*, std::vector<int>>& MpaoidRegion()
{
    static std::unordered_map<const LO*, std::vector<int>> s_mp;
    return s_mp;
}

std::unordered_map<const LO*, NAVLINK>& MpLink()
{
    static std::unordered_map<const LO*, NAVLINK> s_mp;
    return s_mp;
}
NAVLINK& Link(LO* plo)
{
    NAVLINK& l = MpLink()[plo];
    l.plo = plo;
    return l;
}

// orig 0x001C20E8: a record by an object (a path, cid 0x0A: +0 the path; else +4 the object)
void AddNavLinkRecord(NAVLINK* pl, LO* po)
{
    if (pl->agoal.size() >= 6) return;
    GOAL g;
    if (po && FCidDerivedFrom(po->Cid(), 0xA)) g.pcrv = PcrvFromLo(po);
    else g.plo = po;
    pl->agoal.push_back(g);
}

struct INITNAVLINK
{
    INITNAVLINK()
    {
        // option 238 (EOPID 1447, otyp 0x2002 with grf 0x100: an oid setter). 0x13E818 does not call 0x1C20E8 at the
        // load: it queues 0x13E5E0 {oid, setter} on the object's run list (0x1A5D58), run in option order by
        // 0x1A5D98 at the start of the PostLoad (0x1C2170 -> 0x1A5C48), when the paths named exist. So the oid
        // records come after the point records (1232, set at once) and resolve in ResolveNavLink.
        RegisterObjectSetter(1447, [](LO* plo, const CRef& r) {
            NAVLINK& l = Link(plo);
            if (r.m_tagk == TAGK_S32) l.aoidPending.push_back(r.m_n);
            else if (r.m_tagk == TAGK_Basic) AddNavLinkRecord(&l, dynamic_cast<LO*>(r.m_pbasic));
            l.fResolved = false;
        });
        // EOPID 301 (otyp 0x2011, grf 0x120: an oid setter queued like 1447's): 0x20A518 appends the object to the
        // guard's +0xE94 list (8 at most), the regions its links may join (0x1C26E0)
        RegisterObjectSetter(301, [](LO* plo, const CRef& r) {
            std::vector<int>& a = MpaoidRegion()[plo];
            if (a.size() < 8) a.push_back(r.m_tagk == TAGK_S32 ? r.m_n : (r.m_pbasic ? dynamic_cast<LO*>(r.m_pbasic)->oid : -1));
        });
        // option 1232 (EOPID 1448, 0x1C2148): a record by a point
        RegisterObjectSetter(1448, [](LO* plo, const CRef& r) {
            NAVLINK& l = Link(plo);
            if (l.agoal.size() >= 6 || r.m_tagk != TAGK_Vector) return;
            GOAL g;
            g.pos = glm::vec4(r.GetVector(), 0.0f);
            l.agoal.push_back(g);
            l.fResolved = false;
        });
    }
} s_initnavlink;

// orig 0x001C1F28: a record's point (a path's middle, an object's position, else its point)
glm::vec3 PosRecord(const GOAL& g)
{
    if (g.pcrv)
    {
        glm::vec3 pos(0.0f);
        EvalCrvS(g.pcrv, SLengthCrv(g.pcrv) * 0.5f, &pos, nullptr);
        return pos;
    }
    if (g.plo) return PosAi(g.plo);
    return glm::vec3(g.pos);
}
}  // namespace

// The original keeps the link's goals and resolved regions in the LO itself (+0x50..+0x140),
// so they die with that world's objects. The native side table must have the same lifetime.
void DeleteSwNavLinks(const SW* psw)
{
    for (const auto& plo : psw->aploAll)
    {
        MpLink().erase(plo.get());
        MpaoidRegion().erase(plo.get());
    }
}

// orig 0x00206748's +0xE94 = 0: a pooled guard put away forgets its region list (the next copy's option 301 binds
// append to an empty list; kept, the list filled to 8 with earlier copies' regions and dropped the new ones)
void ClearGuardRegions(const LO* plo) { MpaoidRegion().erase(plo); }

// orig 0x001C2170 (the link's update after the load): its ends' points and regions
void ResolveNavLink(NAVLINK* pl)
{
    // 0x1A5C48 -> 0x1A5D98: the run list (0x13E5E0: 0x15E938(G, 4, oid, link), then 0x1C20E8)
    for (int oid : pl->aoidPending) AddNavLinkRecord(pl, PloFindSwNearest(pl->plo, oid));
    pl->aoidPending.clear();
    pl->fResolved = true;
    if (pl->agoal.empty()) return;
    pl->posBegin = PosRecord(pl->agoal.front());
    pl->posEnd = PosRecord(pl->agoal.back());
    pl->pregionBegin = PregionAt(pl->posBegin, false);
    pl->pregionEnd = PregionAt(pl->posEnd, false);
    pl->kind = static_cast<int>(pl->plo->U32Field(0, 0x120, 0));  // (option 1231)
}

std::vector<NAVLINK*> AplinkWorld()
{
    std::vector<NAVLINK*> a;
    for (auto& [plo, l] : MpLink())
    {
        if (!plo->fInWorld) continue;
        NAVLINK* pl = const_cast<NAVLINK*>(&l);
        if (!pl->fResolved) ResolveNavLink(pl);
        if (pl->pregionBegin && pl->pregionEnd) a.push_back(pl);
    }
    std::sort(a.begin(), a.end(), [](NAVLINK* x, NAVLINK* y) { return x->plo->oid < y->plo->oid; });
    static bool s_fListed = false;
    if (!s_fListed && std::getenv("SLY2N_LINKLIST"))
    {
        s_fListed = true;
        for (auto& [plo, l] : MpLink())
        {
            std::printf("link %d inworld %d recs %zu kind %d beg %.0f %.0f %.0f (%d) end %.0f %.0f %.0f (%d)\n", plo->oid, plo->fInWorld ? 1 : 0,
                        l.agoal.size(), l.kind, l.posBegin.x, l.posBegin.y, l.posBegin.z, l.pregionBegin && l.pregionBegin->pmesh ? l.pregionBegin->pmesh->oid : -1,
                        l.posEnd.x, l.posEnd.y, l.posEnd.z, l.pregionEnd && l.pregionEnd->pmesh ? l.pregionEnd->pmesh->oid : -1);
            for (const GOAL& g : l.agoal)
                std::printf("   rec crv %p lo %d pos %.0f %.0f %.0f\n", (void*)g.pcrv, g.plo ? g.plo->oid : -1, g.pos.x, g.pos.y, g.pos.z);
        }
    }
    return a;
}

// orig 0x001C26E0: the link usable by the guard (in the world; its kind's move bits (1: 3, else 2) in +0xFE8; with a
// region list (+0xE94, option 301) both of its ends' regions in it)
bool FNavLinkAllowed(const NAVLINK* pl, GUARD* pg)
{
    if (!pl->plo->fInWorld) return false;
    const int m = pl->kind == 1 ? 3 : 2;
    if ((static_cast<int>(pg->grfFE8) & m) != m) return false;
    const auto it = MpaoidRegion().find(pg->palo);
    if (it == MpaoidRegion().end() || it->second.empty()) return true;
    // (0x13E5E0 resolved the oids at the guard's PostLoad: 0x15E938(G, 4, oid, guard))
    bool f1 = false, f2 = false;
    for (int oid : it->second)
    {
        const LO* plo = oid >= 0 ? PloFindSwNearest(pg->palo, oid) : nullptr;
        if (plo && pl->pregionBegin && plo == pl->pregionBegin->pmesh) f1 = true;
        if (plo && pl->pregionEnd && plo == pl->pregionEnd->pmesh) f2 = true;
        if (f1 && f2) break;
    }
    return f1 && f2;
}

// orig 0x001C21D8: A* from the guard's region over the links: to the target's region (fExact), else to a region
// holding the target; the links in order into apl (at most cMax); its count (1 with apl[0] null when already there,
// fExact 0)
int CNavLinkPath(GUARD* pg, const glm::vec3& posTarget, bool fExact, int cMax, std::vector<NAVLINK*>* papl)
{
    papl->clear();
    const glm::vec3 posStart = PosAi(pg->palo);
    NAVREGION* psrc = PregionAt(posStart, false);
    NAVREGION* pgoal = PregionAt(posTarget, false);
    if (!psrc || !pgoal) return 0;
    const std::vector<NAVLINK*> alink = AplinkWorld();
    struct NODE
    {
        NAVLINK* pin = nullptr;
        int iparent = -1;
        float cost = 0, est = 0;
        int st = 0;  // 1 closed, 2 open
    };
    std::vector<NAVREGION*> areg;
    std::unordered_map<NAVREGION*, int> mpi;
    auto index = [&](NAVREGION* p) {
        auto it = mpi.find(p);
        if (it != mpi.end()) return it->second;
        areg.push_back(p);
        mpi[p] = static_cast<int>(areg.size()) - 1;
        return static_cast<int>(areg.size()) - 1;
    };
    std::vector<NODE> anode;
    auto node = [&](NAVREGION* p) -> NODE& {
        const int i = index(p);
        if (static_cast<int>(anode.size()) <= i) anode.resize(static_cast<size_t>(i) + 1);
        return anode[static_cast<size_t>(i)];
    };
    std::vector<int> aopen;
    {
        NODE& n = node(psrc);
        n.est = glm::length(posStart - posTarget);
        n.st = 2;
        aopen.push_back(index(psrc));
    }
    int icur = -1;
    while (!aopen.empty())
    {
        icur = aopen.front();
        NAVREGION* pcur = areg[static_cast<size_t>(icur)];
        if (fExact ? pcur == pgoal : ItriNav(pcur, posTarget, 1) >= 0) break;  // (0x1C0920: 0x1BF110 on +0xE0)
        aopen.erase(aopen.begin());
        anode[static_cast<size_t>(icur)].st = 1;
        const glm::vec3 posFrom = anode[static_cast<size_t>(icur)].pin ? anode[static_cast<size_t>(icur)].pin->posEnd : posStart;
        for (NAVLINK* pl : alink)
        {
            if (pl->pregionBegin != pcur || !FNavLinkAllowed(pl, pg)) continue;
            const float cost = anode[static_cast<size_t>(icur)].cost + glm::length(posFrom - pl->posBegin) + glm::length(pl->posBegin - pl->posEnd);
            const float est = cost + glm::length(pl->posEnd - posTarget);
            const int inext = index(pl->pregionEnd);
            NODE& n = node(pl->pregionEnd);
            if (n.st && n.est <= est) continue;
            if (n.st == 2) aopen.erase(std::find(aopen.begin(), aopen.end(), inext));
            n.pin = pl;
            n.cost = cost;
            n.est = est;
            n.iparent = icur;
            n.st = 2;
            auto it = aopen.begin();
            while (it != aopen.end() && !(est < anode[static_cast<size_t>(*it)].est)) ++it;
            aopen.insert(it, inext);
        }
        icur = -1;
    }
    if (icur < 0) return 0;
    if (!fExact && anode[static_cast<size_t>(icur)].iparent < 0)
    {
        papl->push_back(nullptr);
        return 1;
    }
    std::vector<NAVLINK*> a;
    for (int i = icur; anode[static_cast<size_t>(i)].iparent >= 0; i = anode[static_cast<size_t>(i)].iparent)
        a.push_back(anode[static_cast<size_t>(i)].pin);
    std::reverse(a.begin(), a.end());
    if (static_cast<int>(a.size()) > cMax) a.resize(static_cast<size_t>(cMax));
    *papl = a;
    if (std::getenv("SLY2N_GOTODEBUG"))
        std::printf("navlink: guard %d path of %zu links (first %d kind %d)\n", pg->palo->oid, a.size(), a.empty() ? -1 : a[0]->plo->oid,
                    a.empty() ? -1 : a[0]->kind);
    return static_cast<int>(a.size());
}
