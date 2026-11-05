#pragma once

// The job markers (class 0x6B, module 0x229EF0..0x22AEE8): an icon high in the air over each job's start with a line
// down to its base, shown for 10 s after L3 (0x140338, with the coin / bottle counters) and while the binocucom's view
// runs (0x150160), lifted so the camera sees it past the world (0x22A220), scaled by the camera's distance; the
// player near one gets its splice event 5 (6 on leaving). marker.cpp.

struct LO;
struct SW;

// orig: 0x00229EF0 + 0x00229F88 (the post-load of a 0x6B object: its parts found, drawn by the marker only)
void PostLoadMarker(SW* psw, LO* plo);
// orig: 0x00140338 (L3 pressed by the current player, from 0x1403D0)
void ShowWorldStats();
// orig: 0x00150160's SW +0x4750 = t (each frame of the binocucom's view)
void NoteBinocView();
struct FONTBRX;
// orig: 0x001CD780 (the frame's job names over the markers, in the binocucom's view: into g_hud, from screenrt.cpp)
void DrawMarkerLabels(const FONTBRX* pfont, const FONTBRX* picons);
