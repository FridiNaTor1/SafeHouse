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
// (the harness's helpers, ship3.cpp) the player plo on its ship's deck in a battle: the use point of a ready cannon of the
// side facing the nearest enemy (or the crippled enemy's boarding use), *pgrf the steering that keeps it abeam (L2 0x1 /
// R2 0x2); false: plo is no battling ship's character
bool FShipBroadside3(LO* plo, glm::vec3* ppos, float* pheading, glm::vec3* pvel, unsigned* pgrf, bool* pfUse);
// (galleon3.cpp, the Galleon Duel's free-for-all) the battle manager's methods natively: 2019 (target + the others, at most
// five ships), 2020, the two-player flag 2021 (every ship counts), its state; the ship whose ball hit plo last (*pt when);
// a ship sunk (mode 7, as its hit points' end)
#include <vector>
void StartShipBattle3(LO* target, const std::vector<LO*>& aother, int fNoSave);
void EndShipBattle3();
void SetShipBattleTwoPlayer3(int f);
int NShipBattleState3();
LO* PloShipHitBy3(LO* plo, float* pt);
void SinkShip3(LO* plo);
// (interact.cpp UseObjectBase) a use was made (a deck cannon's: its user, for the cannon script's broadside)
void NoteShipUse3(LO* puse, LO* puser);
// (galleon3.cpp) the face part a ship's gauge shows (selected when it is shown; -1: its option 2023's), the gauge object
void SetShipGaugeFace3(LO* ship, int idPart);
LO* PloShipGauge3(LO* ship);
// (galleon3.cpp: the free-for-all) a sunk ship's gauge kept up (its bars empty) while it is out of the round
void SetShipGaugeKept3(LO* ship, bool f);
void SetShipGauge3(LO* ship, LO* pgauge);
bool FUseDeckCannon3(LO* pchar);
