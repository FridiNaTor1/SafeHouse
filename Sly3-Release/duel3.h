#pragma once

// PC multiplayer option (user design, notes/PLANE3_2026-11-12.md "Round 3"): the Biplane Duel (the Two Player list's row
// 2026: job 3323 / task 1814 in h_dogfight, world 21) as a free-for-all for three or four players. The original duel
// (two players, Sly against Carmelita: its splice scores the other player at each death, first to 10) stays the default
// and untouched; the choice is made on the Two Player page (Left / Right while the Biplane Duel is shown, as the Cops
// and Robbers map select) and kept for the session (SLY3_DUELPLAYERS=N for tests).
// A free-for-all round:
//  - the players come from the split screen's binding (splitscreen3.h: players 3 / 4 claim a device the same way), the
//    2 x 2 layout, each view its own plane camera (game/biplane3.h) and the original duel HUD's band (views 1 / 3 the
//    first band, 2 / 4 the second) with that player's kills in it;
//  - players 1 / 2 fly the task's two planes (the proxies 94392 / 94393), player 3 the world's third copy of the plane
//    95691 (the single-player one), player 4 an extra copy made at the load (sw.cpp: the plane's group gets one more
//    instance); players 3 / 4 are configured as players 1 / 2 (their pilot binding 1823 and the 2P flag 2483 copied,
//    the start event 17 again: hit points 200, ammunition 150, the power-ups), each player its own team;
//  - a kill counts for its shooter (the plane that fired the last hit, or rammed; a crash scores nobody); the dead
//    plane comes back after the original's fade time at the spawn point (the splice's list 94376..94387) farthest from
//    the others, restored (option 948, as the original's respawn);
//  - the first to the original's 10 kills wins (a tie at the top raises it by one, as the original's s3409); the
//    banner, then the challenge's end (the chooser again).
// The pilots are named "Player 1".."Player 4" everywhere (the messages, the banner, the menu's description); the
// colours are the split screen's characters' (hackmp3.h): Sly blue, Carmelita yellow, Bentley green, Murray purple.

#include <cstdint>
#include <glm/vec4.hpp>
#include <string>
#include <vector>

struct SW;
struct LO;

namespace duel3
{
// the menu's choice: 2 (the original duel) .. 4
int CPlayersChosen();
void StepPlayersChosen(int d);
// the Two Player list's Biplane Duel row (its game object 2026)
bool FDuelNode(const LO* pnode);
// epmenu3.cpp: the row's description (1485 / 1490) when the free-for-all is chosen; false: the original's
bool FDescription(const LO* pnode, std::string* pstr);
// game.cpp (after the HUD, on the Two Player page): the players row
void DrawMenu(int width, int height);

// A free-for-all round runs (its world loaded)
bool FActive();
int CPlayers();
// sw.cpp's group loader: extra copies of a group (the player plane's: player 4's plane) for the round being loaded
int CExtraCopies(const SW* psw, const std::vector<LO*>& aploGroup);
// splitscreen3.cpp: the world loaded (before the players are chosen), the views wanted (0: not a round), the players
// 2.. (null: split3's own choice), after the choice, each frame, the overlay
void OnWorldLoaded(SW* psw);
int CViewsWanted();
LO* PloPlayerWanted(SW* psw, int i);
void OnPlayersChosen(SW* psw);
void Update(SW* psw);
void DrawOverlay(int width, int height, float xLeft, float xRight, float yTop, float yBottom);
// task3.cpp Broadcast3: true = consumed (a round's plane deaths 0xC: scored and respawned here, not by the duel's splice)
bool FOnBroadcast(int code, LO* plo);
// challenge3.cpp (the 2P HUD's score strings 0x360 / 0x361): the kills of the view drawn; false: the board's
bool FScoreString(std::string* pstr);
// splitscreen3.cpp: player i's name tag colour (a text offset) in a round; false: split3's own
bool FTagOffset(int i, glm::vec4* poff);
// player i's colour (0xAABBGGRR, GS units)
uint32_t RgbaPlayer(int i);
}  // namespace duel3
