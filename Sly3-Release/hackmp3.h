#pragma once

// PC multiplayer option (user design, notes/SPLIT3_2026-11-12.md "Round 6c"): the hacking multiplayer game's
// character select. The game is the Two Player list's row 2025 (job 3334, task 1815 in h_hack, world 22); in the
// original player 1 drives Bentley's green tank and player 2 Sly's blue one until Episode 3 is finished, then
// Penelope's pink one. On the PC each player picks a character (one per player, no two alike) on a page after the
// split screen's binding, up to four players (the binding page's four-player option). The defaults are the original's.
//
// The hack module (hack.cpp / hack3.cpp) reads the choice: the number of players and each player's
// character and colour (its ship's tank and tint), and gives the end section four entrances for four players.

#include <cstdint>

#include <glm/vec3.hpp>

struct SW;
struct SCENE;

namespace hackmp3
{
enum CHAR : int { CHAR_Bentley, CHAR_Sly, CHAR_Penelope, CHAR_Carmelita, CHAR_Murray, CHAR_PandaKing, CHAR_Max };

// the characters' colours (GS units 0..128; their tanks' tint) and names (the port's strings)
uint32_t RgbaChar(int ichar);   // 0xAABBGGRR, alpha 0x80
const char* SzChar(int ichar);  // (English; the page shows the port's string)

// The hacking multiplayer game runs in this world (h_hack started from the Two Player list).
bool FGame(const SW* psw);
// The choice: the players (2..4) and each one's character (the original's defaults until the page sets them).
int CPlayers();
int IcharPlayer(int iplayer);
// The episode chooser notes the save's progress before a game starts (the challenge clears it): the original's player
// 2 is Penelope once Episode 3 (Holland) is complete.
void NoteProgress(bool fEpisode3Done);
// (splitscreen3.cpp's page) the choice made; reset to the defaults for cplayers players
void ResetChoice(int cplayers);
void SetCharPlayer(int iplayer, int ichar);
// the next free character from ichar in direction d for iplayer (none taken by another player)
int IcharStep(int iplayer, int ichar, int d);

// The "affects everybody" power-ups defined between two players (user design 2026-11-12: the laser that connects the
// two players): with N players they chain the players in seat order, P1 -> P2, P2 -> P3, ..., PN -> P1 (4: a square, 3: a
// triangle), skipping players who are dead / out (the next living one in seat order). With 2 living players there is
// exactly one link (the original's single laser), with fewer none. afIn[i]: player i (seat i, 0-based) is in the game;
// writes the links {from, to} (0-based players) into alink (room for 4) and returns their count.
int CChainLinks(const bool* afIn, int cplayers, int (*alink)[2]);

// (the select's cards, game.cpp) the hacking ship the players drive (class 0x4A, h_hack's 92992: its model has the
// original's three colour sets, Bentley's green, Sly's blue and Penelope's pink shaders 0x16BD5..0x16BDD) as a scene
// of its own: the world scene's entries of the ship's first instance, only the globs of ichar's colour set shown (the
// characters without a set of their own: the blue set's vertex colours in the character's colour until EndShipScene).
// false: no ship in the world / the scene.
bool FShipScene(SW* psw, const SCENE& scene, int ichar, SCENE* pout, glm::vec3* pposCenter, float* psRadius);
void EndShipScene();
}  // namespace hackmp3
