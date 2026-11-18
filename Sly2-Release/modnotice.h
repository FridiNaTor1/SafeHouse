#pragma once

// A short notice drawn over the game when a mod could not be used as it is (a modded script that does not compile:
// which mod, file, line and why; disc.cpp ReadWorldParts). The log has the whole message; the notice shows for a few
// seconds at the top of the picture (PC addition: the original has no mods).

#include <string>

// queued from any thread (the world's parts are put together while it loads)
void PostModNotice(const std::string& text);
// game.cpp, after the HUD and the debug overlay: the notices still showing (a 2D pass of their own)
void DrawModNotices(int width, int height);
// the notices posted so far (tests: --load-world prints them)
int CModNoticesPosted();
