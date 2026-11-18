#pragma once

// hackgame3: Sly 3's two-player hacking game (the Two Player list's row 2025: job 3334, tasks 1815 / 3337 / 3339 in
// h_hack, world 22; notes/HACK3_2026-11-12.md "Round 7") on PC: the original's rules run in its scripts (the job's
// splice block 93165 modules 135 / 138, the ship's block 92992); this file gives them what the original's engine does
// around them:
//  - the players: task 1815's option 1402 names the two ships (93052 player 1, 92990 player 2); the world start gives
//    each its player's pad (orig3 0x25B9C0 -> 0x1F4B00 -> the ship's vt slot 83 0x19BE90): player 1 the current player,
//    player 2 split3's second seat (splitscreen3.h: the seats' pads, one shared view);
//  - the camera: the ships name each other (+0x30C, EOPID 1567), so the ship's focus (0x19BF08) is the camera framing
//    both (0x19F8B0 / its update 0x19F9C8): above their midpoint, its height from their distance, 500 back along -x;
//  - PC multiplayer option (user design, 3 / 4 players, hackmp3.h): more ships (copies of player 2's), each in its
//    player's colours, the "between two players" power-ups chained in seat order (hackmp3 CChainLinks).
// Nothing here runs outside that game.

#include <glm/vec3.hpp>
#include <vector>

struct SW;
struct LO;
struct SCENE;
struct EXTRADRAW;

namespace hackgame3
{
// the world started (transition.cpp, after split3::OnWorldLoaded): the game's ships to their players
void OnWorldStart(SW* psw);
// each ship's update (hack3.cpp UpdateShip3): the camera, the players' colours, the chained power-ups
void OnShipUpdate(LO* plo);
// the game runs in this world
bool FActive(const SW* psw);
// the game's ships (players 1..N), null past the last
LO* PloShip(int iplayer);
int CShips();
// (PC: a ship recoloured for its character: the scene to rebuild, game.cpp)
bool FTakeSceneRebuild();
// class 0x81's draw (orig3 0x13CF10 -> 0x1C8328, render.cpp's extra draws): the ribbons between their two objects (the
// game's lasers)
void CollectRibbonDraws(const SCENE& scene, const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra);
}  // namespace hackgame3
