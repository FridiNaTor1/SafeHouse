#pragma once
// Sly 3's treasure map (class 0x99) and Sly's map mode (his state 0x18): game/treasuremap3.cpp
#include <cstdint>
struct LO;
LO* PloTreasureMap3();                       // orig3 0x279C28: the world's map, if usable
bool FHoldTreasureMap3(LO* ploPlayer);       // orig3 0x1AF890 -> 0x279120: the map held up (state 2)
LO* PloMapHolder3();                         // the player in the map mode (null: none)
void TakePadForMap3(LO* ploPlayer, uint32_t grfPressed, float xStick);   // orig3 0x1B0070 / 0x1AD1D0 case 24
void ResetTreasureMaps3();
void PutAwayTreasureMap3(LO* ploPlayer);     // orig3 0x279140 (leaving Sly's state 0x18): the map put away (state 1)
// (Sly's state 0x18) the face buttons held while the map mode had the pad (0x1AC098 case 24 tests the gadget's button)
uint32_t GrfMapButtonsHeld3();
void SetMapButtonsHeld3(uint32_t grf);
