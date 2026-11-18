#pragma once

// The Galleon Duel (the Two Player list's row 2027: job 4209 / task 1816 in p_at_sea, world 32; game/ship3.cpp has the
// ships and the battle manager): the original two-player duel's PC presentation (the views narrowed beside the gauges as
// the original draws them) and the PC option for three or four players (user design, notes/PLANE3_2026-11-12.md
// "Round 7"). galleon3.cpp.

#include <cstdint>
#include <string>
#include <glm/vec4.hpp>

struct SW;
struct LO;

namespace galleon3
{
// the Galleon Duel's job runs (either option)
bool FDuelRunning();
// the menu's choice: 2 (the original duel) .. 4 (SLY3_GALLEONPLAYERS=N for tests); the row (its game object 2027)
int CPlayersChosen();
void StepPlayersChosen(int d);
bool FGalleonNode(const LO* pnode);
// a free-for-all round runs (three or four players)
bool FActive();
int CPlayers();
// splitscreen3.cpp: the world loaded (before the players are chosen), the views wanted (0: not a round), after the choice
void OnWorldLoaded(SW* psw);
// sw.cpp, after the option block's group instance counts: a round's world gets players 3 / 4's ships (more instances of the
// gang's ship 44092) and the Guru / the Panda King (their groups, 29 / 30, which the duel's block leaves out)
void AdjustGroupInstances(SW* psw);
int CViewsWanted();
void OnPlayersChosen(SW* psw);
// splitscreen3.cpp: players 2.. (null: split3's own choice), each frame, the overlay, a player's tag colour
LO* PloPlayerWanted(SW* psw, int i);
void Update(SW* psw);
void DrawOverlay(int width, int height, float xLeft, float xRight, float yTop, float yBottom);
bool FTagOffset(int i, glm::vec4* poff);
// task3.cpp Broadcast3: true = consumed (a round's ship sinks / character deaths, 0xC, and the sinking camera 10)
bool FOnBroadcast(int code, LO* plo);
// epmenu3.cpp: the row's description when the free-for-all is chosen; game.cpp: the players row on the Two Player page
bool FDescription(const LO* pnode, std::string* pstr);
void DrawMenu(int width, int height);
// player i's colour (0xAABBGGRR, GS units): Sly blue, Bentley green, the Guru orange, the Panda King red
uint32_t RgbaPlayer(int i);
}  // namespace galleon3
