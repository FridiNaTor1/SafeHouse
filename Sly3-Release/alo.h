#pragma once

#include "lo.h"

class CBinaryInputStream;

void PushXfBase(XF* pxf);
void PopXfBase();
XF* PxfFromRef(int ref, ALO* paloParent, ALO* paloSelf);
XF* PxfFindAlo(ALO* palo, int ref);
XF* PxfFindLox(LO* plo, int ref);

void LoadAloXfsFromBrx(ALO* palo, CBinaryInputStream* pbis);   // orig 0x22D7A0
void LoadAloFromBrx(LO* plo, CBinaryInputStream* pbis);        // orig 0x1F31E0

void UpdateXfWorlds(ALO* palo);
void UpdateAloHierarchyWorlds(ALO* palo);
// The roots' hierarchies in their order (the step's walk, game.cpp): on the pass helpers where that gives the serial
// walk's results (alo.cpp)
void UpdateAloRootsWorlds(const std::vector<ALO*>& apaloRoot);
void SetAloMatrix(ALO* palo, const glm::mat4& mat);   // orig 0x1F4010 (vt slot 37)
void SetAloParent(ALO* palo, ALO* paloNew, XF* pxfNew, int grf);  // orig 0x1F21B8 (vt slot 10)
void CloneAloData(const ALO* paloSrc, ALO* paloDst);  // part of vt slot 40

// orig 0x001F6978 / 0x001F69F8: the mesh groups with id k shown / hidden (the +0xB8 list's 0x800; its collision entry's
// bit 50: the entries with tag k); FAloGlobShown: the renderer's test (fDefault: the loaded flag)
void ShowAloGlobs(ALO* palo, int32_t k, bool fShow);
bool FAloGlobShown(const ALO* palo, int32_t n0, bool fDefault);
// orig 0x001F6AC8 / 0x001F6B48: the collision entries with tag k on (0x1F2A60) / off (0x1F2AD0)
void SetAloSoesByTag(ALO* palo, int32_t k, bool fOn);
