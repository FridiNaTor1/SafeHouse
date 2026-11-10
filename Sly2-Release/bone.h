#pragma once

// Skinning (Sly 2's glob bone bindings 0x167B68 and the per-group bone matrices 0x1684D0): each group of a mesh
// (GLOB) lists bones {oid, record}; at the object's PostLoad (0x1F36A8) every bone is bound to its record and
// keeps the inverse of the record's world matrix (times the group's owner record's world, when the group has one).
// At render time a bone's matrix is the record's current world matrix times that binding: a vertex (in the group's
// owner space, as the renderer draws unskinned groups) goes to world space by the weighted sum of its bones'.
//
// Part of the animation code; the renderer reads the matrices through FGlobBoneMatrices.

#include "alo.h"
#include "lo.h"

#include <glm/glm.hpp>

#include <vector>

// orig 0x00167B68: the bindings of every group of palo's glob set (call after the records' world matrices are set,
// before any animation)
void BindAloBones(ALO* palo);

// The skinning matrices of group iglob of palo's glob set (GLOB::abone order; VTXGLOB::abone / SUBCEL::aiboneSlot
// index them): world = M[i] * pos (glm column vectors; pos in the group's owner space). False when the group has
// no bones or palo was not bound. These replace the group's model matrix (they include the placement).
bool FGlobBoneMatrices(const ALO* palo, int iglob, std::vector<glm::mat4>* pamat);

// The bindings (for tests): per group, per bone, the record (null: none) and the bind matrix.
struct GLOBBONEBIND
{
    const XF* pxfBone = nullptr;
    glm::mat4 matBind{1.0f};
};
const std::vector<std::vector<GLOBBONEBIND>>* PaabindOfAlo(const ALO* palo);
// FGlobBoneMatrices with palo's bindings looked up once (PaabindOfAlo; null: not bound), for a run of groups
bool FGlobBoneMatricesOf(const std::vector<std::vector<GLOBBONEBIND>>* paabind, int iglob, std::vector<glm::mat4>* pamat);
void ResetBoneBindings();
// (PC addition, pccharacter.h: the debug menu's guest characters / outfits) bindings made elsewhere: an object of a
// character world kept loaded beside the current one, or a model put on the current Sly with its own world's binds
void SetAloBoneBindings(const ALO* palo, const std::vector<std::vector<GLOBBONEBIND>>& aabind);
