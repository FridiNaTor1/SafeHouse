#pragma once

// Sly 3's sailing ships (class 0x58, orig3 0x2139D0..0x21A570; game/ship3.cpp; notes/PLANE3_2026-11-12.md "Round 5"):
// Ep5's ships (p_ext 31, p_at_sea 32, p_dive 33, p_island_map 34): the gang's ship (the player's, sailed on the pad or
// by its helmsman while the player runs the deck in a battle), the computer's ships (their patrol / chase / broadside
// AI), the cannons (two sides of up to six), the hits by side, sinking; and the ship-battle manager (class 0x9B, the
// global object 1943: the battle's participants, the tally screen 1945).

#include <glm/vec3.hpp>

struct LO;
struct ALO;

// a ship (class 0x58)
bool FShip3(const LO* plo);
// the wind's heading (orig3 D_0047A2C8: SW +0x4E48, the world's option 997)
float RadWind3();
// po.cpp's enable (orig3 vt slot 83 0x2147B0): a pad taken (n >= 0) sails it (mode 2), released (-1) idle
void OnShipEnable3(LO* plo, int n);
// the ship's mode (+0x1B0: 0 / 1 idle, 2 sailing, 3 the map, 4 in battle, 5 nearly sunk, 6 out, 7 sunk), -1: not a ship
int NShipMode3(const LO* plo);
// the ship's hit points per side (+0x190 / +0x194) and their maximum (+0x198 / +0x19C); false: not a ship
bool FShipHp3(const LO* plo, int* pport, int* pstarboard, int* pmaxPort, int* pmaxStarboard);
// the SO's option 948 (orig3 0x10A520 -> 0x1C93A8: removed, added again, reset (vt slot 31)): a sunk ship back afloat
void RestoreShip3(LO* plo);
// a new world (the records dropped)
void ResetShips3();
// the steering buttons the ship reads (orig3: the input's +0x18 bits 1 / 2: L2 turns to port, R2 to starboard; the
// reference's RAM: the stick does not steer, on the deck it moves the character)
unsigned GrfShipSteer3(LO* plo);
