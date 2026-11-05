// See animfind.h. Specification: Sly2-Reference/Sly2-Release/game/find.cpp (0x15E678, 0x15E3E8; oracle-verified).

#include "animfind.h"

#include "sw.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace
{
// The context's chain with its depth marks (orig LO +0x2C: 1 the context, 2 its parent, ...).
struct MARKS
{
    std::unordered_map<const LO*, int> mpips;
    explicit MARKS(const LO* ploCtx)
    {
        int i = 0;
        for (const LO* p = ploCtx; p; p = p->paloParent) mpips[p] = ++i;
    }
    int Ips(const LO* p) const
    {
        auto it = mpips.find(p);
        return it == mpips.end() ? 0 : it->second;
    }
};

// (an object of the oid, or the first object of an instance whose proxy has it: the proxies' table SW +0x1838)
bool FOidMatches(const LO* plo, OID oid) { return plo->oid == oid || plo->oidInstance == oid; }
}  // namespace

// orig 0x15E938 with kind 4 (the first match of 0x15E678 / 0x15E3E8 case 4)
LO* PloFindSwNearest(LO* ploCtx, OID oid, bool fSameChannel)
{
    LO* plo = nullptr;
    CploFindSwNearest(ploCtx, oid, &plo, fSameChannel);
    return plo;
}

// orig 0x15E650 with kind 0x204 (0x15E678: the nearest, the count not capped): 1 when one object shares the closest
// ancestor with the context; with no common ancestor every object of the oid counts (*pplo the first)
int CploFindSwNearest(LO* ploCtx, OID oid, LO** pplo, bool fSameChannel)
{
    *pplo = nullptr;
    if (!g_psw || oid == -1) return 0;
    const MARKS marks(ploCtx);
    int ipsBest = INT_MAX;
    LO* ploBest = nullptr;
    int c = 0;
    for (LO* const up : AploOfOid(g_psw, oid))
    {
        LO* plo = up;
        if (!FOidMatches(plo, oid)) continue;
        if (fSameChannel && ploCtx && ploCtx->ichannel >= 0 && plo->ichannel != ploCtx->ichannel) continue;
        const LO* p = plo;
        int ips = 0;
        for (; p; p = p->paloParent)
            if ((ips = marks.Ips(p)) != 0) break;
        if (p && ips < ipsBest)
        {
            ipsBest = ips;
            c = 0;
            ploBest = plo;
            ++c;
            continue;
        }
        if (ipsBest != INT_MAX || p) continue;  // (farther than the best, or no common ancestor after one with)
        if (c == 0) ploBest = plo;
        ++c;
    }
    *pplo = ploBest;
    return c;
}

// orig 0x15E938 with kind 1 (the nearest marked ancestor of the object is the context itself)
LO* PloFindSwChild(LO* ploCtx, OID oid)
{
    if (!g_psw || oid == -1 || !ploCtx) return nullptr;
    const MARKS marks(ploCtx);
    for (LO* const up : AploOfOid(g_psw, oid))
    {
        LO* plo = up;
        if (!FOidMatches(plo, oid)) continue;
        for (const LO* p = plo; p; p = p->paloParent)
        {
            const int ips = marks.Ips(p);
            if (ips == 0) continue;
            if (ips == 1) return plo;
            break;
        }
    }
    return nullptr;
}

// orig 0x15E938 with kind 5
LO* PloFindSwAny(OID oid)
{
    if (!g_psw || oid == -1) return nullptr;
    // (the world's namespace first, as PloFindSwObjectByOid: sw.cpp FLoInWorldNs)
    LO* ploFirst = nullptr;
    for (LO* const up : AploOfOid(g_psw, oid))
        if (FOidMatches(up, oid))
        {
            if (up->oidInstance == oid || FLoInWorldNs(up)) return up;
            if (!ploFirst) ploFirst = up;
        }
    return ploFirst;
}
