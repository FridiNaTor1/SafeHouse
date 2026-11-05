// The attachments' bounding-box overlap tracking (Sly 1's bbmark.c; orig 0x1336F0-0x134398): each attachment's box
// has two entries (min, max) in three sorted sweep lists; an insertion sort of the lists finds the pairs whose
// boxes start or stop overlapping and keeps an OX record per side; the moved attachments' pairs get new contacts.
// Ported from the reference's bbmark.cpp / basic_p3.cpp (oracle-verified).

#include "so.h"

#include "vtnative.h"

#include <algorithm>
#include <unordered_set>

namespace
{
template <class T>
void RemoveFrom(std::vector<T>& a, const T& t)
{
    auto it = std::find(a.begin(), a.end(), t);
    if (it != a.end()) a.erase(it);
}
void VtSetOverlap(ALO* p, ALO* pOther)  // slot 54
{
    using PFN = void (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 54))) pfn(p, pOther);
}
bool VtFStill(ALO* p)  // slot 62
{
    using PFN = bool (*)(ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 62))) return pfn(p);
    return FAloStill(p);
}
int VtOverlapKind(ALO* p, ALO* pOther)  // slot 61 (ALO: 0x1F4FC8)
{
    using PFN = int (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 61))) return pfn(p, pOther);
    using PFN2 = int (*)(ALO*, ALO*);
    if (PFN2 pfn2 = reinterpret_cast<PFN2>(PvNativeFn(0x1F4FC8))) return pfn2(p, pOther);
    return 0;
}
void VtCall0(ALO* p, int slot, uint32_t addrDefault)
{
    using PFN = void (*)(ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, slot));
    if (!pfn && addrDefault) pfn = reinterpret_cast<PFN>(PvNativeFn(addrDefault));
    if (pfn) pfn(p);
}

// orig: 0x00133C68 func_00133C68 (PoxAddSw: a new record at the head of poxa's list for the pair)
OX* PoxAddSw(OXA* poxa, OXA* poxaOther)
{
    OX* pox = new OX;
    pox->psoOther = poxaOther->pso;
    pox->poxNext = poxa->pox;
    poxa->pox = pox;
    return pox;
}

// orig: 0x00133CB8 func_00133CB8 (unlinks from b's list the record of c's object; returns it)
OX* PoxRemoveSw(OXA* b, OXA* c)
{
    ALO* key = c->pso;
    OX** pp = &b->pox;
    for (OX* pe = b->pox; pe; pe = pe->poxNext)
    {
        if (pe->psoOther == key)
        {
            *pp = pe->poxNext;
            return pe;
        }
        pp = &pe->poxNext;
    }
    return nullptr;
}
}

// orig: 0x001F4FC8 func_001F4FC8 (ALO vt slot 61: the overlap kind: 1 when (a has flag 0x4000000 or is still) and b
// is still, else 0)
int OverlapKindAlo(ALO* pa, ALO* pb)
{
    if (!(pa->psort && (pa->psort->grf148 & 0x4000000ull)))
    {
        if (!VtFStill(pa)) return 0;
    }
    return VtFStill(pb) ? 1 : 0;
}
NATIVEFN(0x1F4FC8, OverlapKindAlo);

// orig: 0x001336F0 func_001336F0 (AddSwAaobrObject: a new attachment for the object, two entries appended to each
// sweep list)
OXA* PoxaAddSw(ALO* pso)
{
    g_physw.aoxaOwned.push_back(std::make_unique<OXA>());
    OXA* poxa = g_physw.aoxaOwned.back().get();
    poxa->pso = pso;
    g_physw.apoxa.push_back(poxa);
    if (!g_pfnFAloActive || g_pfnFAloActive(pso)) g_physw.apoxaActive.push_back(poxa);
    for (int i = 0; i < 3; i++)
    {
        g_physw.aaox[i].push_back(AOX{0, poxa, &poxa->posMin[i]});
        g_physw.aaox[i].push_back(AOX{1, poxa, &poxa->posMax[i]});
    }
    return poxa;
}

// orig: 0x001337D0 func_001337D0 (RemoveSwAaobrObject: the overlap records freed, off the lists, its entries out of
// each sweep list)
void RemoveSwOxa(OXA* poxa)
{
    for (OX* pox = poxa->pox; pox;)
    {
        OX* p = pox;
        VtSetOverlap(poxa->pso, p->psoOther);
        VtSetOverlap(p->psoOther, poxa->pso);
        OX* poxOther = p->psoOther && Poxa(p->psoOther) ? PoxRemoveSw(Poxa(p->psoOther), poxa) : nullptr;
        if (p->pxp)
        {
            if (!VtFStill(p->psoOther)) WakeAlo(p->psoOther);
            FreeSwXpList(p->pxp);
        }
        pox = p->poxNext;
        delete p;
        delete poxOther;
    }
    poxa->pox = nullptr;
    FreeSwXpList(poxa->pxpSensor);
    poxa->pxpSensor = nullptr;
    RemoveFrom(g_physw.apoxa, poxa);
    RemoveFrom(g_physw.apoxaActive, poxa);
    if (poxa->grf & 1) RemoveFrom(g_physw.apoxaDirty1, poxa);
    if (poxa->grf & 2) RemoveFrom(g_physw.apoxaDirty2, poxa);
    for (auto& a : g_physw.aaox)
        a.erase(std::remove_if(a.begin(), a.end(), [&](const AOX& x) { return x.poxa == poxa; }), a.end());
    for (auto& p : g_physw.aoxaOwned)
        if (p.get() == poxa)
        {
            p.reset();
            break;
        }
    g_physw.aoxaOwned.erase(std::remove(g_physw.aoxaOwned.begin(), g_physw.aoxaOwned.end(), nullptr), g_physw.aoxaOwned.end());
}

// orig: 0x00133D08 func_00133D08 (the boxes overlap and the objects' overlap kinds allow it)
bool FOverlapOxa(OXA* poxa, OXA* poxaOther)
{
    for (int i = 0; i < 3; i++)
        if (poxaOther->posMax[i] <= poxa->posMin[i]) return false;
    for (int i = 0; i < 3; i++)
        if (poxa->posMax[i] <= poxaOther->posMin[i]) return false;
    const int n1 = VtOverlapKind(poxa->pso, poxaOther->pso);
    const int n2 = VtOverlapKind(poxaOther->pso, poxa->pso);
    const int n = n2 < n1 ? n1 : n2;
    return n != 1 && n != 3;
}

// orig: 0x00133A38 func_00133A38 (UpdateSwPox: the pair's overlap (k 1: overlapping, 2: not, 0: test the boxes);
// the pair's records made or freed)
void UpdateSwPox(OXA* poxa, OXA* poxaOther, int k)
{
    int f = 0;
    OX* pox = poxa->pox;
    while (pox && pox->psoOther != poxaOther->pso) pox = pox->poxNext;
    if (k == 1) f = 1;
    else if (k == 0) f = pox ? pox->fOverlap : 0;
    else if (k == 2) f = 0;
    if (static const char* s_szPox = std::getenv("SLY2N_POXPRINT"); s_szPox && poxa->pso && poxaOther->pso &&
        (poxa->pso->oid == std::atoi(s_szPox) || poxaOther->pso->oid == std::atoi(s_szPox)))  // (debug: one object's pairs)
        std::printf("pox %d / %d: k %d f %d overlap %d kinds %d %d\n", poxa->pso->oid, poxaOther->pso->oid, k, f,
                    int(FOverlapOxa(poxa, poxaOther)), VtOverlapKind(poxa->pso, poxaOther->pso), VtOverlapKind(poxaOther->pso, poxa->pso));
    if (f || FOverlapOxa(poxa, poxaOther))
    {
        if (!pox)
        {
            pox = PoxAddSw(poxa, poxaOther);
            OX* poxOther = PoxAddSw(poxaOther, poxa);
            pox->poxOther = poxOther;
            poxOther->poxOther = pox;
        }
    }
    else
    {
        if (!pox) return;
        VtSetOverlap(poxa->pso, poxaOther->pso);
        VtSetOverlap(poxaOther->pso, poxa->pso);
        OX* pox1 = PoxRemoveSw(poxa, poxaOther);
        OX* pox2 = PoxRemoveSw(poxaOther, poxa);
        if (pox1 && pox1->pxp)
        {
            FreeSwXpList(pox1->pxp);
            if (!VtFStill(pox1->psoOther)) WakeAlo(pox1->psoOther);
            if (pox2 && !VtFStill(pox2->psoOther)) WakeAlo(pox2->psoOther);
        }
        delete pox1;
        delete pox2;
        pox = nullptr;
    }
    if (pox)
    {
        pox->poxOther->fOverlap = f;
        pox->fOverlap = f;
    }
}

// orig: 0x00133E30 func_00133E30 (empties dirty list 1: each object's slot 49 (the bounds) takes it off)
void FlushSwOxaDirty1()
{
    while (!g_physw.apoxaDirty1.empty())
    {
        OXA* poxa = g_physw.apoxaDirty1.front();
        VtCall0(poxa->pso, 49, 0x1F50E0);
        if (!g_physw.apoxaDirty1.empty() && g_physw.apoxaDirty1.front() == poxa)
        {
            poxa->grf &= ~1ull;  // (native guard: a slot 49 that does not take it off)
            g_physw.apoxaDirty1.erase(g_physw.apoxaDirty1.begin());
        }
    }
}

// orig: 0x00133E80 func_00133E80 (UpdateSwAaox: insertion sort of the sweep lists; an entry passing another of the
// other kind (min / max) updates the pair)
void UpdateSwAaox()
{
    if (!g_physw.fAaoxDirty) return;
    g_physw.fAaoxDirty = false;
    for (auto& a : g_physw.aaox)
    {
        if (a.empty()) continue;
        float gMax = *a[0].pg;
        for (size_t j = 1; j < a.size(); j++)
        {
            const float g = *a[j].pg;
            if (gMax <= g)
            {
                gMax = g;
                continue;
            }
            size_t k = j;
            while (k > 0 && !(*a[k - 1].pg <= g))
            {
                if (a[j].fMax != a[k - 1].fMax) UpdateSwPox(a[j].poxa, a[k - 1].poxa, 0);
                k--;
            }
            const AOX x = a[j];
            a.erase(a.begin() + j);
            a.insert(a.begin() + k, x);
        }
    }
}

// The sweep lists rebuilt as the attachments' adds would have made them with their current boxes (native: the world's
// first physics frame, after the load placed the objects; see physworld.cpp): the pairs' records dropped (no
// notifications), the boxes recomputed, then each attachment's ends appended in order and sorted in (0x1336F0 +
// 0x133E80), which makes the pairs that overlap.
void RebuildSwAaox()
{
    std::unordered_set<XP*> setxp;
    for (OXA* poxa : g_physw.apoxa)
    {
        for (OX* pox = poxa->pox; pox;)
        {
            OX* p = pox;
            pox = p->poxNext;
            if (p->pxp && setxp.insert(p->pxp).second) FreeSwXpList(p->pxp);
            delete p;
        }
        poxa->pox = nullptr;
    }
    for (OXA* poxa : g_physw.apoxa)
        if (!(poxa->grf & 1))
        {
            poxa->grf |= 1;
            g_physw.apoxaDirty1.push_back(poxa);
        }
    FlushSwOxaDirty1();
    for (auto& a : g_physw.aaox) a.clear();
    for (OXA* poxa : g_physw.apoxa)
    {
        for (int i = 0; i < 3; i++)
        {
            g_physw.aaox[i].push_back(AOX{0, poxa, &poxa->posMin[i]});
            g_physw.aaox[i].push_back(AOX{1, poxa, &poxa->posMax[i]});
        }
        g_physw.fAaoxDirty = true;
        UpdateSwAaox();
    }
}

// orig: 0x00134040 func_00134040 (UpdateSwXps: the dirty boxes, the sweep lists; the moved attachments' pairs get new
// contacts (0x1AA1D8), their objects told (slot 54, 58) and woken when the contacts changed; dirty list 2 emptied)
void UpdateSwXps()
{
    FlushSwOxaDirty1();
    UpdateSwAaox();
    std::vector<OXA*> apoxaTouched;
    for (OXA* poxa : g_physw.apoxaDirty2)
    {
        uint64_t grf = poxa->grf;
        if (grf & 4) continue;
        grf |= 0x10;
        poxa->grf = grf;
        if (!(grf & 8))
        {
            poxa->grf = grf | 8;
            apoxaTouched.push_back(poxa);
        }
        for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
        {
            OXA* poxa2 = Poxa(pox->psoOther);
            if (!poxa2) continue;
            const uint64_t grf2 = poxa2->grf;
            if (grf2 & 0x10) continue;
            if (grf2 & 4) continue;
            if (!(grf2 & 8))
            {
                poxa2->grf = grf2 | 8;
                apoxaTouched.push_back(poxa2);
            }
            VtSetOverlap(poxa->pso, poxa2->pso);
            VtSetOverlap(poxa2->pso, poxa->pso);
            XP* pxpOld = pox->pxp;
            FreeSwXpList(pxpOld);
            XP* pxp = PxpMarkOxaContacts(poxa, poxa2);
            pox->pxp = pxp;
            pox->poxOther->pxp = pxp;
            if (!pxpOld && !pox->pxp) continue;
            if (!VtFStill(poxa->pso)) WakeAlo(poxa->pso);
            if (!VtFStill(poxa2->pso)) WakeAlo(poxa2->pso);
        }
    }
    while (!g_physw.apoxaDirty2.empty())
    {
        OXA* poxa = g_physw.apoxaDirty2.front();
        poxa->grf &= ~0x10ull;
        VtSetOverlap(poxa->pso, nullptr);
        FreeSwXpList(poxa->pxpInternal);
        poxa->pxpInternal = nullptr;
        VtCall0(poxa->pso, 59, 0);  // (ALO: 0x1F4FA8, nothing)
        poxa->grf &= ~2ull;
        g_physw.apoxaDirty2.erase(g_physw.apoxaDirty2.begin());
    }
    for (OXA* poxa : apoxaTouched)
    {
        poxa->grf &= ~8ull;
        VtCall0(poxa->pso, 58, 0x1F4F10);
    }
}

// orig: 0x001F4F10 func_001F4F10 (ALO vt slot 58: posts the SO to the world's deferred calls (0x149AB8 with the
// member key 0x2A8278 = 0x1F7950, its contact messages) when its attachment has sensor contacts (+0x6C) or, with flags
// 0x3000000, when one of its overlaps has contacts; natively physworld.cpp's queue, run at the next 0x149C80)
void (*g_pfnPostAloContacts)(ALO* palo) = nullptr;
void NotifyAloContacts(ALO* p)
{
    OXA* poxa = Poxa(p);
    if (!poxa || !g_pfnPostAloContacts) return;
    if (poxa->pxpSensor)
    {
        g_pfnPostAloContacts(p);
        return;
    }
    if (p->psort && (p->psort->grf148 & 0x3000000ull))
        for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
            if (ox->pxp)
            {
                g_pfnPostAloContacts(p);
                break;
            }
}
NATIVEFN(0x1F4F10, NotifyAloContacts);

// orig: 0x00134320 func_00134320 (RecalcSwOxfFilterForObject: the object's pairs with every other attachment retested)
void RecalcSwOxfFilterForObject(ALO* pso)
{
    OXA* poxa = Poxa(pso);
    if (!poxa) return;
    MarkAloDirty(pso);
    std::vector<OXA*> a = g_physw.apoxa;
    for (OXA* p : a)
        if (p != Poxa(pso) && Poxa(pso)) UpdateSwPox(Poxa(pso), p, 0);
}
