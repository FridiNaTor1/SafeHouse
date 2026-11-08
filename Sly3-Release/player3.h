#pragma once

// Sly 3's player in the native game: the development start in gameplay and the per-frame trace (player3.cpp).
//
// SLY3_START=1 (with --world 35): at the first frame Sly (cid 0x34) is placed where the reference has him when he
// gets the pad (Sly3-research/ref3/boot v.5600.ram), heading 1.2043, and is made the current player (EnablePo).
// SLY3_START=x,y,z[,rad] places him elsewhere. SLY3_PLAYERTRACE=file.csv writes his state per frame.

#include <glm/glm.hpp>

struct LO;
struct SW;

namespace sly3
{
LO* PloSly3(SW* psw);
bool FStartPlayer3(SW* psw);              // (game.cpp, frame 0)
void TracePlayer3(SW* psw, int frame);    // (game.cpp, after each frame's update)
int NPlayerCamKind3(SW* psw);             // orig3 0x1A9260: the current player's camera kind (menu.cpp Camera Options)
// SLY3_SETPOS="F:X Y Z;F2:X Y Z;..": Sly moved there at the start of frame F (his velocities zeroed), as the reference's
// SLY2_SETPOS (vt slots 34 / 40 / 41) at vblank F + the route's base (tools/player3: the moves' routes)
void SetPosPlayer3(SW* psw, int frame);   // (game.cpp, before each frame's update)
}  // namespace sly3
