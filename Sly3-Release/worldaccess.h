#pragma once

// Accessors of the loaded world for the renderer (and other systems): objects by class, their world matrices,
// the records a glob refers to. Header-only; the object model owns the data (lo.h, sw.h).
//
// The lights' 3-qw records (LIGHT +0x120) and the light context (0x1A4478 / 0x1A3F78) are computed natively by
// the renderer (lightsw.h from AploLights' options and transforms, light.h SelectLights).
// Not available yet (later milestones): bone poses (skinning),
// visibility from object state (render list SW+0x3F20, zone mask, alpha, distance fades), run-time producers.

#include "alo.h"
#include "cls.h"
#include "sw.h"

#include <vector>

// The objects of class cidBase and its subclasses, in creation order per class.
inline std::vector<LO*> AploOfClass(const SW* psw, CID cidBase)
{
    std::vector<LO*> a;
    if (!psw || cidBase < 0 || cidBase >= CID_Max) return a;
    for (CID cid = cidBase; cid < g_acls[cidBase].cidLast && cid < CID_Max; ++cid)
        a.insert(a.end(), psw->mpcidaplo[cid].begin(), psw->mpcidaplo[cid].end());
    return a;
}

// The lights (class 4 and its subclasses) with their loaded options (LO::aopt: eopid + values).
inline std::vector<LO*> AploLights(const SW* psw) { return AploOfClass(psw, 4); }

// An ALO's placement: its first transform record's world matrix (orig ALO's records +0x40).
inline glm::mat4 MatWorldOfAlo(const ALO* palo)
{
    return palo && !palo->axf.empty() ? palo->axf[0]->matWorld : glm::mat4(1.0f);
}

// The record a glob's S16 owner / bone id refers to (orig vt slot 51, 0x22D778: own records, 0x1000 the
// parent's), its world matrix in XF::matWorld; null for none.
inline XF* PxfOfGlobRef(ALO* palo, int ref) { return palo ? PxfFindAlo(palo, ref) : nullptr; }

// The clones of an object (instances made by the world loader and placed by proxies) share its GLOBSET.
inline std::vector<ALO*> ApaloSharingGlobset(const SW* psw, const GLOBSET* pglobset)
{
    std::vector<ALO*> a;
    for (LO* plo : AploOfClass(psw, 0x30))
        if (static_cast<ALO*>(plo)->pglobset.get() == pglobset) a.push_back(static_cast<ALO*>(plo));
    return a;
}
