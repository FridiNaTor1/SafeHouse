#pragma once

// The links between the navigation regions (class 0x7C) and the search over them: navlink.cpp.

#include "aijobs.h"

#include <glm/glm.hpp>
#include <vector>

struct GUARD;
struct LO;
struct NAVREGION;
struct SW;

struct NAVLINK
{
    LO* plo = nullptr;
    std::vector<GOAL> agoal;          // +0x50 / +0x60.. the records (6 at most; 0x20 each)
    std::vector<int> aoidPending;     // +0x34: option 238's oids, set at the PostLoad (the loader's run list 0x1A5D58)
    int kind = 0;                     // +0x120 (option 1231): 0 walked, 1 jumped
    NAVREGION* pregionBegin = nullptr, * pregionEnd = nullptr;  // +0x124 / +0x128
    glm::vec3 posBegin{0.0f}, posEnd{0.0f};  // +0x130 / +0x140
    bool fResolved = false;
};

void ResolveNavLink(NAVLINK* pl);                         // orig 0x1C2170
std::vector<NAVLINK*> AplinkWorld();
bool FNavLinkAllowed(const NAVLINK* pl, GUARD* pg);       // orig 0x1C26E0
int CNavLinkPath(GUARD* pg, const glm::vec3& posTarget, bool fExact, int cMax, std::vector<NAVLINK*>* papl);  // 0x1C21D8
void DeleteSwNavLinks(const SW* psw);
void ClearGuardRegions(const LO* plo);                  // orig 0x206748's +0xE94 = 0 (a pooled guard put away)                   // discard native metadata with its owning objects
