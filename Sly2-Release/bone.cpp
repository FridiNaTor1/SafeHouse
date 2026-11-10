// Skinning bindings and bone matrices (Sly 2 0x167B68, 0x1684D0). Specification: Sly2-Reference/Sly2-Release/game/m_167310.cpp
// (oracle-verified).

#include "bone.h"
#include "worldglobals.h"

#include "animfind.h"
#include "glob.h"
#include "simdmat.h"
#include "sw.h"
#include "xfmath.h"

#include <unordered_map>

namespace
{
std::unordered_map<const ALO*, std::vector<std::vector<GLOBBONEBIND>>> s_mpaloaabind;
WORLD_TABLE(s_mpaloaabind);   // (PC: moved aside while a kept world loads, worldglobals.h)
}  // namespace

// orig: 0x00167B68
void BindAloBones(ALO* palo)
{
    if (!palo->pglobset) return;
    const GLOBSET& gs = *palo->pglobset;
    auto& aabind = s_mpaloaabind[palo];
    aabind.assign(gs.aglob.size(), {});
    for (size_t i = 0; i < gs.aglob.size(); ++i)
    {
        const GLOB& glob = gs.aglob[i];
        if (glob.abone.empty()) continue;
        // the group's owner record (group +4: the S16 through vt slot 51)
        const XF* pxfGrp = glob.oidOwner >= 0 ? PxfFindAlo(palo, glob.oidOwner) : nullptr;
        auto& abind = aabind[i];
        abind.resize(glob.abone.size());
        for (size_t j = 0; j < glob.abone.size(); ++j)
        {
            GLOBBONEBIND& b = abind[j];
            LO* plo = PloFindSwNearest(palo, glob.abone[j].oid);  // orig 0x15E938(G, 4, oid, owner)
            if (plo && FCidDerivedFrom(plo->Cid(), 0x30)) b.pxfBone = PxfFindAlo(static_cast<ALO*>(plo), glob.abone[j].ibone);
            if (b.pxfBone)
                b.matBind = pxfGrp ? InverseAffineMatrix4(b.pxfBone->matWorld) * pxfGrp->matWorld
                                   : InverseAffineMatrix4(b.pxfBone->matWorld);
            else if (pxfGrp)
                b.matBind = pxfGrp->matWorld;
            else
                b.matBind = glm::mat4(1.0f);
        }
    }
}

// orig: 0x001684D0 (each item's matrix times its bone's world)
bool FGlobBoneMatricesOf(const std::vector<std::vector<GLOBBONEBIND>>* paabind, int iglob, std::vector<glm::mat4>* pamat)
{
    if (!paabind || iglob < 0 || iglob >= static_cast<int>(paabind->size())) return false;
    const auto& abind = (*paabind)[size_t(iglob)];
    if (abind.empty()) return false;
    pamat->resize(abind.size());
    for (size_t j = 0; j < abind.size(); ++j)
    {
        const GLOBBONEBIND& b = abind[j];
        // (a bone without a record: the original reads the null record's matrix; here the binding alone)
        (*pamat)[j] = b.pxfBone ? MulMat4(b.pxfBone->matWorld, b.matBind) : b.matBind;   // (simdmat.h: glm's product exactly)
    }
    return true;
}
bool FGlobBoneMatrices(const ALO* palo, int iglob, std::vector<glm::mat4>* pamat)
{
    return FGlobBoneMatricesOf(PaabindOfAlo(palo), iglob, pamat);
}

const std::vector<std::vector<GLOBBONEBIND>>* PaabindOfAlo(const ALO* palo)
{
    auto it = s_mpaloaabind.find(palo);
    return it == s_mpaloaabind.end() ? nullptr : &it->second;
}

void ResetBoneBindings() { s_mpaloaabind.clear(); }

void SetAloBoneBindings(const ALO* palo, const std::vector<std::vector<GLOBBONEBIND>>& aabind) { s_mpaloaabind[palo] = aabind; }
