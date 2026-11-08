#pragma once

// The in-game HUD's widgets (Sly 2 module 0x1D93D8..0x1DFAD8, their list at 0x3D9E70): the character gauge
// (0x3D9E80 on screen object 0x36B: health and gadget power, 0x1D9968 / 0x1D9DF8 / 0x1D9B60) and the task prompt
// (0x3DA0C0 on screen object 0x371: a string shown while a task is current, 0x1DA4B8 / 0x1DA578 / 0x1DA5C8 /
// 0x1DA350), started after a world load (0x1D9520) and updated every frame from the game state (0x1D9648). The
// screen objects themselves run in screenrt.h. Not yet: the shop (0x3DA150), the menus (0x3D9ED0, 0x3DA3C0,
// 0x3DA530), the memory card dialogs (0x2E6550 / 0x2E65A0) and the timed message (0x3DA4F0).

struct SW;

// The player's values the gauge follows (orig PO +0xE00 -> hits, +0xDFC the most; +0x12FC -> / +0x1300 the gadget
// power and its most, 0x1441A0). Sampled from the current player each gauge update; cleared on world release
// or when there is no player. It never holds a pointer into a save or the previous world.
struct HUDPLAYER
{
    int cHit = 0, cHitMax = 0;
    int nPower = 0, nPowerMax = 0;
    bool fGadgets = false;   // a gadget with a power cost is owned (0x1D9B60's table walk: the power bar shown)
};
extern HUDPLAYER g_hudplayer;

// orig: 0x001D9408 (the implemented gauge/prompt reset before their world is released)
void ResetHudWidgets();
// orig: 0x001D9520 (after a world load: the widgets find their screen objects)
void InitHudWidgets(SW* psw);
// orig: 0x001D9648 (every frame), then the screen objects' updates (screenrt.h)
void UpdateHud(SW* psw);
// The frame's 2D: the shown screen objects into g_hud and flushed into the scene target.
void DrawHud(SW* psw, int width, int height);

// orig: 0x001DA4B8 on 0x3DA0C0: the prompt shows string id for g seconds (< 0: until ended; -1 id: ends it)
void HudPrompt(int id, float g);
// orig: 0x001DA578 on 0x3DA0C0: the prompt's string id ends
void HudPromptEnd(int id);
int IdHudPrompt();  // 0x1DA5B0

// orig: 0x001ABD98 (the auto-save after a job / a saving task): into the slot the game is played from (frontend.h
// g_islotChosen; none: no save); not in the first episode unless forced; with the progress "flags fAutosaveWarned"
// set the slot is written (0x1ABCE8), else the warning dialog 0x22 first (menu.h).
void HudAutoSave(bool fForce);
extern bool (*g_pfnWriteSave)();

// (Sly 3) orig3 0x001A9568: the current player has the pad (its pause menu, its gauge)
bool FPadHasPlayer3(SW* psw);
