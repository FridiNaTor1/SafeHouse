#pragma once

// Sly 3's score tally (class 0xA5, h_hack's 3335) and the ship battles' stat strings (1945): tally3.cpp.

struct SW;

// each frame: the world's tallies (orig3 0x19FC80, their show / hide 0x1A0130 / 0x1A0160)
void UpdateTallies3(SW* psw);
