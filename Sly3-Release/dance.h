#pragma once

// The dance minigame (dance.cpp): the director (cid 0x79) and its dancers (cid 0x44), driven by the script commands
// 0x49 / 0x4A / 0x4B (scpt.cpp) on the top music's clock.

#include <utility>
#include <vector>

struct LO;
struct FONTBRX;

void SetDancePrompts(int state, const std::vector<std::pair<float, int>>& aent);  // orig 0x1531A8 (command 0x49)
void AddDanceBeat(LO* ploDancer, int nButton, int nMove, float gTime);          // orig 0x1532D8 (command 0x4A)
void PlayDancerDefault(LO* ploDancer, LO* ploAseg);                             // orig 0x1524A0 (command 0x4B)
bool FDanceDirector();                                                          // D_002DE570 set
bool FDancer(const LO* plo);                                                    // cid 0x44
void PostLoadDance(LO* plo);                                                    // orig 0x1526D8 / 0x1527C8
void PostLoadDancer(LO* plo);                                                   // orig 0x152100
void DrawDancePrompts(const FONTBRX* pfont, const FONTBRX* picons);             // orig 0x152C58 (vt slot 19)
void ResetDances();
