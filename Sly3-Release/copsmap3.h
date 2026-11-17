#pragma once

// PC multiplayer option (user design, notes/SPLIT3_2026-11-12.md "Round 6"): Cops and Robbers on the other episode hubs
// that have Carmelita (Outback, Holland, China besides the original's Venice), chosen on the episode chooser's Two
// Player page (the map select: Left / Right while Cops and Robbers is shown). Venice is the original game (job 2409 in
// v_ext: its splice and script 2414, the board 140111, the booty stands) and stays the default, untouched. Another hub
// has none of those objects, so a native rule set runs there, driven by a data table per hub (copsmap3.cpp s_ahub:
// spawn points, loot stands, safe points and power-up spots on the hub's landmarks: spire tops, roofs, the squares):
//  - Sly takes a loot by walking up to a stand that has one (three stands hold loot at a time) and carries it to a safe
//    point: 1 point. A hit while he carries it makes him drop it (it goes back to a stand).
//  - Carmelita catches Sly (his health out: 1 point); he comes back at the spawn farthest from her. Sly knocking her
//    out is 1 point for him; she comes back at the spawn farthest from him.
//  - Power-up spots (v_ext's power-up model on them; all but one hold one, a taken one shows again at once on a free
//    spot, as Venice's four instances streamed among six spots): the original's power-ups by the class's vt slot 90 -
//    Sly the next of his shuffled Smoke Bomb / Rocket Boots / shield / Mega Jump on L1, Carmelita the next of her super
//    jump / charged shot / jump to the robber / hit; the meter full (round 9).
//  - The first to 5 wins: the banner, then the chooser again (the challenge manager's end, as the original's).
// The round runs as a two-player game of the challenge manager (challenge3.h: the save copied aside, Exit 2P in the
// pause menu) with the original's two-player screen mode (2): the split screen, both gauges, the 2P Smoke Bomb cost.
// The hub's own guards (and their spawners) are taken out of the world for the round; its story (tasks) is the
// challenge's fresh progress.

#include <string>

struct SW;
struct LO;

namespace cops3
{
// the hubs offered (0: Venice, the original game; then the native ones)
int CHubs();
int IepHub(int ihub);          // its episode (sly3::EP_*: the chooser's names)
int IworldHub(int ihub);
// the map select's choice (kept for the session; SLY3_COPSMAP=N chooses it for tests)
int IHubChosen();
void StepHubChosen(int d);

// The Two Player page's start (epmenu3.cpp): Cops and Robbers with a native hub chosen -> the round starts there (true);
// false: the original game (StartChallenge3 as before).
bool FCopsNode(const LO* pnode);
bool FStartChosen(SW* psw, LO* pnode);
// A native round is on (its world loaded).
bool FActive();
// A world loaded (split3::OnWorldLoaded, before the players are chosen): the native round's world -> the two-player
// screen mode, the round set up; any other world ends a round.
void OnWorldLoaded(SW* psw);
// split3's players 2.. for the round (null: none)
LO* PloPlayerWanted(SW* psw, int i);
// After the players were chosen (split3): the hub's guards out, the players at their spawns.
void OnPlayersChosen(SW* psw);
// Once per frame (split3::UpdateCameras: after the world's update).
void Update(SW* psw);
// task3.cpp Broadcast3: true = consumed (the native round handles the deaths 0xC itself: no world reload).
bool FOnBroadcast(int code, LO* plo);
// A power-up for a player (its class's vt slot 90: Sly orig3 0x154AB8, Carmelita 0x161BE0; also Venice's pickups through
// the SO option 947, 0x10A4A0): the kind / item id given, -1 none
int GivePowerup(LO* plo);
// aicarm3.cpp: the speed factor of a Carmelita player (1: the round-6 sprint is gone)
float GSpeedScale(const LO* plo);
// Per view (splitscreen3.cpp DrawOverlay, inside its HUD batch, the full picture's canvas): the markers, the scores,
// the messages and the banner. xLeft..: the canvas's edges of the picture.
void DrawOverlay(int width, int height, float xLeft, float xRight, float yTop, float yBottom);
// The map select on the Two Player page (game.cpp after the HUD: drawn while the page shows Cops and Robbers)
void DrawMenu(int width, int height);
// (SLY3_COPSDUMP=1: a hub's landmarks printed at its load, for the tables)
void DumpLandmarks(SW* psw);
}  // namespace cops3
