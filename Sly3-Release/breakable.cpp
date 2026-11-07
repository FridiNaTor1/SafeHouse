// breakable.cpp: the touch / hit triggered objects (cid 0x62, module 0x1BAB50..0x1BC050; the reference's
// m_1BAB50_p3.cpp, the decomp's src/hand 0x1BADF0 / 0x1BAED8 / 0x1BAF18 / 0x1BB400 / 0x1BB648 / 0x1BB9B8 / 0x1BBD48 /
// 0x1BBEA8): the potted plants, crates, lamps ... Each keeps a trigger list (+0x150 {class, on}) and an event table
// (+0x15C the current event, +0x164 {id, n, c, items}; the event -1 runs with every event), both built by the
// loader's methods (EOPIDs 1025..1045). A hit (slot 75: a contact message with the bytes 8) or a touch by an object
// of a trigger class (slot 58) runs the current event's items (0x1BB400, deferred): objects added / removed (the
// broken pieces), mesh groups shown / hidden, collision entries on / off, explosions (class 0x1E: ExplodeLo), the
// script events; after n hits the next event.
#include "rumble.h"
#include "game/sly3.h"
#include <algorithm>
#include "worldglobals.h"
#include "breakable.h"
#include "pickup.h"
#include "xf.h"
#include "worldaccess.h"

#include "activate.h"
#include "ainoise.h"
#include "alo.h"
#include "brx.h"
#include "cls.h"
#include "emitter.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "clock.h"
#include "update.h"
#include "vtnative.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

void NotifyAloContacts(ALO* p);  // bbmark.cpp: orig 0x1F4F10

void RequestLoTaskState(LO* plo, int n, LO* ploContext);  // task.cpp (0x164640)

namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "breakable: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

struct BRKITEM
{
    int k = 0;
    int v = 0;
    int v2 = 0;  // (kind 12: the pair's second)
};
struct BRKEV
{
    int id = -1;
    int n = 1;  // the hits it takes (<= 0: every hit, the triggers stay off)
    std::vector<BRKITEM> aitem;
};
struct BRK
{
    std::vector<std::pair<int, int>> atrig;  // +0x150 / +0x154: {class, on}
    int n158 = 0;                            // +0x158 the hits of the current event
    int icur = 0;                            // +0x15C (-1: the event -1 being built)
    BRKEV evAny;                             // the event -1 (+0x164 - 0x10)
    std::vector<BRKEV> aev;                  // +0x160 / +0x164
    int grf168 = 0;                          // +0x168
    LO* ploHit = nullptr;                    // +0x16C who hit it
    int grfHit = 0;                          // +0x170 its contact filter
};
std::unordered_map<const LO*, BRK> s_mpbrk;
WORLD_TABLE(s_mpbrk);   // (PC: moved aside while a kept world loads, worldglobals.h)

bool FBreakable(const LO* plo) { return plo && FCidDerivedFrom(plo->Cid(), S3CID(0x62)); }

// The object's data (a clone's copied from its source when it is made: CloneBreakableData)
BRK& Brk(LO* plo)
{
    auto it = s_mpbrk.find(plo);
    if (it != s_mpbrk.end()) return it->second;
    // (a copy made before its source's events were loaded: the source's data on first use)
    for (const LO* p = plo->ploCloneOf; p; p = p->ploCloneOf)
    {
        auto itSrc = s_mpbrk.find(p);
        if (itSrc != s_mpbrk.end())
        {
            BRK b = itSrc->second;
            b.ploHit = nullptr;
            return s_mpbrk[plo] = b;
        }
    }
    return s_mpbrk[plo];
}

// orig: 0x001BBE18 / 0x001BBE48 / 0x001BBE68
bool FValidEvent(const BRK& b) { return (b.icur >= 0 && b.icur < static_cast<int>(b.aev.size())) || b.icur == -1; }
BRKEV* PevCurrent(BRK& b)
{
    if (!FValidEvent(b)) return nullptr;
    return b.icur == -1 ? &b.evAny : &b.aev[static_cast<size_t>(b.icur)];
}
// orig: 0x001BBDB0 (a new item in the current event, up to 0x40)
BRKITEM* PitemNew(LO* plo, int k)
{
    BRKEV* pev = PevCurrent(Brk(plo));
    if (!pev || pev->aitem.size() >= 0x40) return nullptr;
    pev->aitem.push_back(BRKITEM{k, 0, 0});
    return &pev->aitem.back();
}
void AddItem(LO* plo, int k, int v)
{
    if (BRKITEM* p = PitemNew(plo, k)) p->v = v;
}

// orig 0x0015E938 (an object by oid around ctx: its own subtree, then its parents', then the world)
LO* PloFindAround(LO* ctx, OID oid)
{
    if (!ctx) return PloFindSwObjectByOid(g_psw, oid);
    if (ctx->oid == oid) return ctx;
    std::function<LO*(LO*)> find = [&](LO* p) -> LO* {
        for (LO* pc : p->aploOwned)
        {
            if (pc->oid == oid || pc->oidInstance == oid) return pc;
            if (LO* q = find(pc)) return q;
        }
        return nullptr;
    };
    for (LO* p = ctx; p; p = p->paloParent)
        if (LO* q = find(p)) return q;
    return PloFindSwObjectByOid(g_psw, oid);
}

void Event(LO* plo, int n) { HandleLoSpliceEvent(plo, static_cast<uint32_t>(n), false, 0, nullptr); }

// orig: 0x001BAF18 (to the event id (0x609 the first, 0x60A past the last): the events between applied forwards
// (their items 0..5) or undone backwards)
void GotoEvent(LO* plo, int id)
{
    BRK& b = Brk(plo);
    int iTarget = b.icur;
    if (id == 0x609) iTarget = 0;
    else if (id == 0x60A) iTarget = static_cast<int>(b.aev.size());
    else if (id != -1)
        for (size_t j = 0; j < b.aev.size(); ++j)
            if (b.aev[j].id == id) { iTarget = static_cast<int>(j); break; }
    if (std::getenv("SLY2N_BRKDEBUG"))
        std::fprintf(stderr, "breakable-goto: t %.4f oid %d %p id 0x%X: %d -> %d\n", g_clock.t, plo->oid, static_cast<void*>(plo), id, b.icur, iTarget);
    if (b.icur == iTarget) return;
    auto apply = [&](const BRKITEM& it, bool fForward) {
        LO* ps = nullptr;
        if ((0xC3 >> it.k) & 1)
        {
            ps = PloFindAround(plo, it.v);
            if (!ps) return;
        }
        ALO* palo = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
        switch (it.k)
        {
        case 0: if (fForward) AddLo(g_psw, ps); else RemoveLo(g_psw, ps); break;
        case 1: if (fForward) RemoveLo(g_psw, ps); else AddLo(g_psw, ps); break;
        case 2: case 3: ShowAloGlobs(palo, it.v, (it.k == 2) == fForward); break;
        case 4: case 5: SetAloSoesByTag(palo, it.v, (it.k == 4) == fForward); break;
        default: break;
        }
    };
    if (b.icur < iTarget)
    {
        while (b.icur < iTarget)
        {
            const BRKEV& ev = b.aev[static_cast<size_t>(b.icur)];
            ++b.icur;
            for (const BRKITEM& it : ev.aitem) apply(it, true);
        }
    }
    else
        while (iTarget < b.icur)
        {
            --b.icur;
            const BRKEV& ev = b.aev[static_cast<size_t>(b.icur)];
            for (size_t k = ev.aitem.size(); k-- > 0;) apply(ev.aitem[k], false);
        }
    b.n158 = 0;
}

// orig: 0x001BB648 (an event's items)
void RunItems(LO* plo, BRK& b, const BRKEV& ev)
{
    LO* psPrev = nullptr;
    ALO* palo = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
    for (const BRKITEM& it : ev.aitem)
    {
        if ((b.n158 > 0 || ev.n <= 0) && ((0x3F >> it.k) & 1)) continue;
        LO* ps = nullptr;
        if ((0xC3 >> it.k) & 1)
        {
            ps = PloFindAround(plo, it.v);
            if (!ps) continue;
        }
        switch (it.k)
        {
        case 6: psPrev = ps; break;
        case 0: AddLo(g_psw, ps); break;     // vt slot 8
        case 1: RemoveLo(g_psw, ps); break;  // vt slot 9
        case 2: ShowAloGlobs(palo, it.v, true); break;    // 0x1F6978
        case 3: ShowAloGlobs(palo, it.v, false); break;   // 0x1F69F8
        case 4: SetAloSoesByTag(palo, it.v, true); break;  // 0x1F6AC8
        case 5: SetAloSoesByTag(palo, it.v, false); break; // 0x1F6B48
        case 7:
            // the explosion object (class 0x1E): its slot 51 with {1, the source (the kind 6 object, else itself)}
            if (FCidDerivedFrom(ps->Cid(), S3CID(0x1E)))
            {
                BURSTPARAMS bp;
                bp.grf = 1;
                bp.plo = psPrev ? psPrev : ps;
                ExplodeLo(ps, &bp);
            }
            break;
        case 8: if (b.grf168 & 2) WarnOnce("the camera's shake 0x14F750 (item 8)"); break;
        case 9: if (b.grf168 & 4) TriggerJoyRumbleRumk(it.v, 0.3f, "breakable item 9"); break;   // 0x187E28
        case 10:
            // the drops (pickup.cpp, the actors code's 0x154E10) at the kind 6 object's centre, else the breakable's
            if (b.grf168 & 8)
            {
                LO* pc = psPrev ? psPrev : plo;
                const glm::vec3 pos = FCidDerivedFrom(pc->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(pc)) : PosLoWorld(pc);  // 0x1A62E8
                if (std::getenv("SLY2N_BRKDEBUG"))
                    std::printf("breakable %d: drops from %d (cid 0x%X) at (%.0f %.0f %.0f), the breakable at (%.0f %.0f %.0f) world (%.0f %.0f %.0f)\n", plo->oid,
                                pc->oid, pc->Cid(), pos.x, pos.y, pos.z, palo ? PosAloCenter(palo).x : 0.f, palo ? PosAloCenter(palo).y : 0.f,
                                palo ? PosAloCenter(palo).z : 0.f, palo ? MatWorldOfAlo(palo)[3].x : 0.f, palo ? MatWorldOfAlo(palo)[3].y : 0.f,
                                palo ? MatWorldOfAlo(palo)[3].z : 0.f);
                DropPickups(pos, it.v, -100, plo);
            }
            break;
        case 11:
        {
            CRef ref;
            ref.SetS32(it.v);
            HandleLoSpliceEvent(plo, 4, false, 1, &ref);  // vt slot 3 (4, {v})
            break;
        }
        case 12:
            // orig 0x161F48 (D_002DEB30, 1 entry {oid, state}, the breakable): the task (0x15E938 kind 0x105 around
            // it) asked for the state (0x164640), e.g. world 10's table 91471: "found code" 1589
            if (LO* ptask = PloFindAround(plo, it.v)) RequestLoTaskState(ptask, it.v2, plo);
            break;
        default: break;
        }
    }
}

// orig: 0x001BB400 (the deferred hit: the current event's items, the next event after its n hits)
void RunHit(LO* plo)
{
    BRK& b = Brk(plo);
    if (!FValidEvent(b)) return;
    if (b.ploHit && b.ploHit == PloPoCurrent(g_psw)) b.grf168 = 0xE;
    else
    {
        const int v = b.grfHit;
        b.grf168 = (v == 0x100 || v == 0x10000 || v == 0x20000 || (b.ploHit && FIsLoDerivedFrom(b.ploHit, S3CID(0x3B)))) ? 8 : 0;
    }
    Event(plo, 0x10);  // vt slot 3
    if (b.grf168 & 1) return;
    if (b.icur < 0) return;  // (the event -1 alone: TODO when it is current)
    RunItems(plo, b, b.evAny);
    RunItems(plo, b, b.aev[static_cast<size_t>(b.icur)]);
    // 0x156B98(G, 0, 0x1A62E8(the breakable), the hitter): the fixed noise 706 (SW +0x4020) the guards hear, unless a
    // guard (0x38, not 0x3C) hit it. 17:2716's RC tank nudging breakable 9330 alerts the guards 59433 this way.
    if (!b.ploHit || !FIsLoDerivedFrom(b.ploHit, S3CID(0x38)) || FIsLoDerivedFrom(b.ploHit, S3CID(0x3C)))
        if (NOISETMPL* pt = PtmplNoiseFixed(0)) PnoiseNew(pt, PosLoWorld(plo), b.ploHit);
    const BRKEV& ev = b.aev[static_cast<size_t>(b.icur)];
    if (ev.n > 0 && ++b.n158 >= ev.n)
    {
        b.n158 = 0;
        if (++b.icur >= static_cast<int>(b.aev.size()))
        {
            // vt slot 4 (0x10: the listeners), the script event 5, the tasks' broadcast 0x11
            Event(plo, 5);
            BroadcastTaskEvent(g_psw, 0x11, plo);
        }
    }
    if (FValidEvent(b))
        if (BRKEV* pev = PevCurrent(b); pev && pev->n <= 0)
            for (auto& t : b.atrig) t.second = 0;
    if (std::getenv("SLY2N_BRKDEBUG"))
        std::fprintf(stderr,"breakable-run: t %.9g oid %d %p current %d of %zu (n %d) hits %d hitter %d filter 0x%X flags 0x%X\n",
                     g_clock.t,plo->oid,static_cast<void*>(plo),b.icur,b.aev.size(),FValidEvent(b)&&b.icur>=0&&b.icur<int(b.aev.size())?b.aev[size_t(b.icur)].n:-9,b.n158,b.ploHit?b.ploHit->oid:-1,b.grfHit,b.grf168);
}

// orig: 0x001BAED8 (the hit by a, filter n: posted to the world's deferred calls (0x149AB8))
void PostHit(LO* plo, LO* ploHit, int grf)
{
    BRK& b = Brk(plo);
    b.ploHit = ploHit;
    b.grfHit = grf;
    PostDeferredCall(plo, 0x1BB400, [](void* object, int) { RunHit(static_cast<LO*>(object)); });
}

// orig: 0x001BADF0 (vt slot 75: a contact message; the hits (bytes 8) of a trigger class's object)
bool FAcceptBreakableContact(ALO* p, CTMSG* pctm)
{
    BRK& b = Brk(p);
    if (std::getenv("SLY2N_BRKDEBUG") && std::getenv("SLY2N_BRKDEBUG")[0] == '2')
        std::printf("breakable %d %p contact from %d: valid %d event %d bytes 0x%llX filter 0x%X\n", p->oid, static_cast<void*>(p),
                    pctm->ploSender ? pctm->ploSender->oid : -1, int(FValidEvent(b)), b.icur, (unsigned long long)pctm->grfAccepted, pctm->grfFilter);
    if (!FValidEvent(b)) return false;
    if (!pctm->grfAccepted) return false;
    if ((pctm->grfAccepted & 0xC) != 0x8) return true;
    if (!b.atrig.empty() && pctm->ploSender != p)
    {
        bool f = false;
        for (const auto& [cid, fOn] : b.atrig) f = f || (pctm->ploSender && FIsLoDerivedFrom(pctm->ploSender, static_cast<CID>(cid)));
        if (!f) return false;
    }
    PostHit(p, pctm->ploSender, static_cast<int>(pctm->grfFilter));
    if (std::getenv("SLY2N_BRKDEBUG"))
        std::printf("breakable %d hit by %d (filter 0x%X bytes 0x%X), event %d at (%.0f %.0f %.0f)\n", p->oid, pctm->ploSender ? pctm->ploSender->oid : -1,
                    pctm->grfFilter, pctm->grfAccepted, b.icur, PosAloCenter(p).x, PosAloCenter(p).y, PosAloCenter(p).z);
    return true;
}

// orig: 0x001BB250 (vt slot 58: the base's, then each new contact with an object of an active trigger class)
void UpdateBreakableContacts(ALO* p)
{
    NotifyAloContacts(p);
    BRK& b = Brk(p);
    if (!FValidEvent(b)) return;
    OXA* poxa = Poxa(p);
    if (!poxa) return;
    for (OX* pl = poxa->pox; pl; pl = pl->poxNext)
    {
        if (!pl->poxOther || !pl->poxOther->pxp) continue;
        ALO* pOther = pl->psoOther;
        for (const auto& [cid, fOn] : b.atrig)
        {
            if (!fOn || !pOther || !FIsLoDerivedFrom(pOther, static_cast<CID>(cid))) continue;
            if (!(Sort(p).grf148 & 0x3000000ull)) PostHit(p, pOther, 0);
            if (FBreakable(pOther))
                for (const auto& [cid2, fOn2] : Brk(pOther).atrig)
                    if (fOn2 && FIsLoDerivedFrom(p, static_cast<CID>(cid2)))
                    {
                        PostHit(pOther, p, 0);
                        break;
                    }
            using PFN54 = void (*)(ALO*, ALO*);
            if (PFN54 pfn = reinterpret_cast<PFN54>(PvNativeVt(p, 54))) pfn(p, pOther);
            if (PFN54 pfn = reinterpret_cast<PFN54>(PvNativeVt(pOther, 54))) pfn(pOther, p);
            FreeSwXpList(pl->pxp);
            pl->poxOther->pxp = nullptr;
            pl->pxp = nullptr;
            break;
        }
    }
}

// The loader's methods (EOPIDs 1025..1045: their thunks 0x10C820..) and setters (1033 / 1034 / 1038 / 1039)
struct INITBREAKABLE
{
    INITBREAKABLE()
    {
        RegisterObjectMethod(0x10C820, [](LO* plo, const std::vector<CRef>& a) {  // 0x1BB918: a trigger {class, on}
            BRK& b = Brk(plo);
            if (b.atrig.size() < 8) b.atrig.emplace_back(NArg(a, 0), NArg(a, 1));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10C8C8, [](LO* plo, const std::vector<CRef>& a) {  // 0x1BB950: a new event, current
            BRK& b = Brk(plo);
            if (b.aev.size() < 0x20)
            {
                b.icur = static_cast<int>(b.aev.size());
                BRKEV ev;
                ev.id = NArg(a, 0);
                b.aev.push_back(ev);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10C950, [](LO* plo, const std::vector<CRef>&) {  // 0x1BB9B8: the event -1, current
            BRK& b = Brk(plo);
            b.icur = -1;
            b.evAny.id = -1;
            b.evAny.n = 1;
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10C9C8, [](LO* plo, const std::vector<CRef>& a) {  // 0x1BBA18: the event's hits
            BRK& b = Brk(plo);
            if (b.icur == -1) return CRef(TAGK_Void);
            if (BRKEV* pev = PevCurrent(b); pev && NArg(a, 0)) pev->n = NArg(a, 0);
            return CRef(TAGK_Void);
        });
        // EOPID 1028 (option 929, +0x16C): who hit it last; 1046 (option 935, +0x168): the hit's flags (0xE the
        // player's), a script's bit 1 stops the items (e.g. world 9's tuxedo pieces: 111248's event 16 checks both)
        RegisterObjectGetter(1028, [](LO* plo) { CRef r; r.SetBasic(FBreakable(plo) ? Brk(plo).ploHit : nullptr); return r; });
        // orig 0x1BAED8: option 930 / EOPID 1029 reads the last contact filter at +0x170.
        RegisterObjectGetter(1029, [](LO* plo) { CRef r; r.SetS32(FBreakable(plo) ? Brk(plo).grfHit : 0); return r; });
        RegisterObjectGetter(1046, [](LO* plo) { CRef r; r.SetS32(FBreakable(plo) ? Brk(plo).grf168 : 0); return r; });
        RegisterObjectSetter(1046, [](LO* plo, const CRef& r) {
            if (FBreakable(plo)) Brk(plo).grf168 = r.m_tagk == TAGK_Bool ? r.m_bool : r.m_n;
        });
        RegisterObjectSetter(1033, [](LO* plo, const CRef& r) { AddItem(plo, 6, r.m_n); });   // 0x1BBA60
        RegisterObjectSetter(1034, [](LO* plo, const CRef& r) { AddItem(plo, 7, r.m_n); });   // 0x1BBA90
        RegisterObjectMethod(0x10CA50, [](LO* plo, const std::vector<CRef>& a) { AddItem(plo, 8, NArg(a, 0)); return CRef(TAGK_Void); });
        RegisterObjectMethod(0x10CAD8, [](LO* plo, const std::vector<CRef>& a) { AddItem(plo, 9, NArg(a, 0)); return CRef(TAGK_Void); });
        RegisterObjectMethod(0x10CB60, [](LO* plo, const std::vector<CRef>& a) { AddItem(plo, 10, NArg(a, 0)); return CRef(TAGK_Void); });
        RegisterObjectSetter(1038, [](LO* plo, const CRef& r) { AddItem(plo, 11, r.m_n); });  // 0x1BBB50
        RegisterObjectSetter(1039, [](LO* plo, const CRef& r) { AddItem(plo, 0, r.m_n); });   // 0x1BBB80
        // 0x1BBBB0 / 0x1BBC08 / 0x1BBC98 / 0x1BBCF0: an item 2 / 4 / 3 / 5 in a current event, else at once
        auto itemOr = [](LO* plo, int k, int v) {
            if (!FValidEvent(Brk(plo)))
            {
                ALO* palo = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
                if (k == 2 || k == 3) ShowAloGlobs(palo, v, k == 2);
                else SetAloSoesByTag(palo, v, k == 4);
                return;
            }
            AddItem(plo, k, v);
        };
        static auto s_itemOr = itemOr;
        RegisterObjectMethod(0x10CBE8, [](LO* plo, const std::vector<CRef>& a) { s_itemOr(plo, 2, NArg(a, 0)); return CRef(TAGK_Void); });
        RegisterObjectMethod(0x10CCF8, [](LO* plo, const std::vector<CRef>& a) { s_itemOr(plo, 4, NArg(a, 0)); return CRef(TAGK_Void); });
        RegisterObjectMethod(0x10CE90, [](LO* plo, const std::vector<CRef>& a) { s_itemOr(plo, 3, NArg(a, 0)); return CRef(TAGK_Void); });
        RegisterObjectMethod(0x10CFA0, [](LO* plo, const std::vector<CRef>& a) { s_itemOr(plo, 5, NArg(a, 0)); return CRef(TAGK_Void); });
        // the scripts' versions (EOPID +0x10): at once on the object
        RegisterObjectMethod(0x10CC70, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) ShowAloGlobs(static_cast<ALO*>(plo), NArg(a, 0), true);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10CF18, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) ShowAloGlobs(static_cast<ALO*>(plo), NArg(a, 0), false);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10CD80, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) SetAloSoesByTag(static_cast<ALO*>(plo), NArg(a, 0), true);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10D028, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) SetAloSoesByTag(static_cast<ALO*>(plo), NArg(a, 0), false);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10CE08, [](LO* plo, const std::vector<CRef>& a) {  // 0x1BBC60: item 1 (-1: itself)
            int v = NArg(a, 0);
            if (v == -1) v = plo->oid;
            AddItem(plo, 1, v);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10D0B0, [](LO* plo, const std::vector<CRef>& a) {  // 0x1BBD48: item 12 {a, b}
            if (BRKITEM* p = PitemNew(plo, 12))
            {
                p->v = NArg(a, 0);
                p->v2 = NArg(a, 1);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10D158, [](LO* plo, const std::vector<CRef>& a) {  // 0x1BAED8: a hit from a script
            PostHit(plo, PloArg(a, 0), NArg(a, 1));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10D1D8, [](LO* plo, const std::vector<CRef>& a) {  // 0x1BAF18
            GotoEvent(plo, NArg(a, 0));
            return CRef(TAGK_Void);
        });
    }
} s_initbreakable;
}  // namespace

// orig: 0x001BAB50 (the init's contact bytes) and the end of 0x001BABA0 (the load: the first event current)
void InitBreakableBytes(ALO* palo)
{
    SetAloXpBytes(palo, 0x03FFFFFF, 8);  // 0x1F7918
    SetAloXpBytes(palo, 0x80001, 0);
}
void PrintBreakablesNear(const glm::vec3& pos, float sMax)
{
    std::vector<std::pair<float, const LO*>> a;
    for (const auto& [plo, b] : s_mpbrk)
        if (plo->fInWorld && FCidDerivedFrom(plo->Cid(), S3CID(0x30)) && FValidEvent(b))
        {
            const float s = glm::length(PosAloCenter(const_cast<ALO*>(static_cast<const ALO*>(plo))) - pos);
            if (s < sMax) a.emplace_back(s, plo);
        }
    std::sort(a.begin(), a.end());
    for (const auto& [s, plo] : a)
    {
        const glm::vec3 c = PosAloCenter(const_cast<ALO*>(static_cast<const ALO*>(plo)));
        std::printf("breakable near: %d at (%.0f %.0f %.0f), %.0f away\n", plo->oid, c.x, c.y, c.z, s);
    }
}
void PostLoadBreakable(LO* plo)
{
    Brk(plo).icur = 0;
    if (std::getenv("SLY2N_BRKDEBUG") && !Brk(plo).aev.empty())
    {
        int c10 = 0;
        for (const BRKEV& ev : Brk(plo).aev)
            for (const BRKITEM& it : ev.aitem) c10 += it.k == 10;
        if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) UpdateAloXfWorlds(static_cast<ALO*>(plo));
        const glm::vec3 c = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(plo)) : glm::vec3(0.0f);
        std::printf("breakable %d: %zu triggers, %zu events, first with %zu items, %d drops, at (%.0f %.0f %.0f) inworld %d\n", plo->oid,
                    Brk(plo).atrig.size(), Brk(plo).aev.size(), Brk(plo).aev[0].aitem.size(), c10, c.x, c.y, c.z, plo->fInWorld);
        auto pr = [](const BRKEV& ev) {
            std::printf("  event %d n %d:", ev.id, ev.n);
            for (const BRKITEM& it : ev.aitem) std::printf(" {%d %d}", it.k, it.v);
            std::printf("\n");
        };
        for (const auto& [cid, fOn] : Brk(plo).atrig) std::printf("  trigger cid 0x%X on %d\n", cid, fOn);
        pr(Brk(plo).evAny);
        for (const BRKEV& ev : Brk(plo).aev) pr(ev);
    }
}

// orig: 0x001BBEA8 (vt slot 26 when attached: the base's, the first event (the broken parts undone), the constraints)
void ResetBreakable(LO* plo)
{
    if (!FBreakable(plo)) return;
    GotoEvent(plo, 0x609);
    // 0x1BBEA8 restores both constraints after a pool row is reused.
    // Murray frees the linear constraint while picking up a breakable.
    ALO* palo = static_cast<ALO*>(plo);
    if (Psost(palo))
    {
        SetAloCnstrLin(palo, 1);  // 0x1F5720
        SetAloCnstrAng(palo, 1);  // 0x1F57C0
    }
}

void ResetBreakables()
{
    s_mpbrk.clear();
}

NATIVEFN(0x1BADF0, FAcceptBreakableContact);
NATIVEFN(0x1BB250, UpdateBreakableContacts);

// orig: 0x001BBE18 (for the chase job's aim, aichase.cpp: a breakable not yet broken)
bool FBreakableIntact(LO* plo) { return FBreakable(plo) && FValidEvent(Brk(plo)); }

// orig: the breakable's fields (+0x150..+0x170) are part of the object the clone (0x13E510 / 0x1A6730) copies, so a
// copy starts in its source's state at the moment it is made. The copy here was made on the copy's first use, from
// its source's state then: Ep8's bulb lamps (20699, copies of one another) all took the state of the first lamp
// broken, so every later dart passed through them (the Sly/Bentley Conspire softlock).
void CloneBreakableData(LO* ploSrc, LO* ploDst)
{
    if (!FBreakable(ploSrc) || !FBreakable(ploDst)) return;
    auto it = s_mpbrk.find(ploSrc);
    if (it == s_mpbrk.end())
        for (const LO* p = ploSrc->ploCloneOf; p && it == s_mpbrk.end(); p = p->ploCloneOf) it = s_mpbrk.find(p);
    if (it == s_mpbrk.end() || it->second.aev.empty()) return;   // (not loaded yet: Brk's first use)
    if (std::getenv("SLY2N_BRKDEBUG")) std::fprintf(stderr, "breakable-clone: %d %p from %p (event %d)\n", ploDst->oid, static_cast<void*>(ploDst), static_cast<const void*>(it->first), it->second.icur);
    BRK b = it->second;
    b.ploHit = nullptr;
    s_mpbrk[ploDst] = std::move(b);
}
