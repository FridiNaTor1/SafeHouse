#pragma once

// PC addition, not in the original game (debug menu only): the RC chopper
// of Episode 2's RC Bombing Run (world 8) and Episode 6's Aerial Assault, playable in any world.
//
// The retail chopper (PO 50723, cid 0x32) is flown by its world's splice (block 49605: option 810 its pad, 187 its
// velocity, 36 its heading; Square fires the forward gun (proxy 50747 -> shot 50749, speed option 1378), Cross drops a
// bomb (proxy 50744 -> bomb 50746)). Here a live copy of it (and of its shot and bomb) is put into the running world,
// takes the pad and the camera (slot 79, as the character switch), and is flown by native code with the game's own
// physics (its collisions with the level); its shots and bombs are the game's projectiles (their blast hits guards and
// breakables through the projectiles' own contact messages). Switching back gives the pad back to the player, who
// stood where it was, and takes the copies out of the world. Nothing here writes the save or the progress.

struct SW;
struct LO;

bool FPcRcActive();                       // flying (or waiting for the chopper's file)
bool FPcRcPending();                      // waiting for the chopper's file
const char* PchzPcRcStatus();             // the menu row's value: "Off", "Loading", "Flying", "Not available"
void RequestPcRc(SW* psw, bool fOn);      // the menu's row: on (the chopper made and flown) / off (back to the player)
bool FPcRcObject(const LO* plo);          // plo is the flown chopper or one of its parts (pccharacter.cpp: no puppet on it)

// each step after the world's update (Debug keys off: turned off)
void UpdatePcRc(SW* psw, bool fDebugKeys);
