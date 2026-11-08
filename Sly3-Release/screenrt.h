#pragma once
struct FONTBRX;
// The HUD's fonts of the world drawn last (the text, the button icons), or null.
const FONTBRX* PfontHud();
const FONTBRX* PfontHudIcons();
// (Sly 3) the font object's element ielem (a text box's +0x2C), or null
const FONTBRX* PfontHudElem(int ielem);

// The screen objects at run time (Sly 2 module 0x210E90..0x2155C0, the SCREENOBJ state at +0x58..+0xCC): each
// world sprite set (cid 0x7E, sm.h SPRITESET) gets a state machine over its root group's parts - shown with the
// show / idle / hide parts 0x30F / 0x310 / 0x311 (0x2128E8), hidden (0x2129E8), updated every frame (0x212C50)
// and drawn by splayer.h's CScreenPlayer into g_hud (0x213300). The game's HUD widgets (hudwidget.h), the scripts
// (vt slot 49 / 50 through transition.cpp) and the wipe drive them.

#include "lo.h"

#include <array>
#include <string>

struct SW;

// orig: 0x002128E8 (vt slot 49): shows the object on part k (-1: its 0x30F / 0x310 / 0x311 parts)
void ShowScreenObj(LO* plo, int k);
// orig: 0x00212A30: shown idle (state 2): its idle time (+0x5C, against +0x8C) starts again
void RefreshScreenObj(LO* plo);
// orig: 0x002129E8 (vt slot 50): hides it (through its 0x311 part when it has one)
void HideScreenObj(LO* plo);
// orig: the episode menu's 0x15D648 reset: no show / idle / hide parts (+0x70..+0x78 -1), the timer 1, stopped
void StopScreenObj(LO* plo);
// its string argument i (+0xCC + 4 i) set directly (0x19D948's stores)
void SetScreenObjArg(LO* plo, int i, int n);
// orig: 0x00212880: its string arguments (+0xCC, up to 10), then shown (-1)
void ShowScreenObjArgs(LO* plo, int carg, const int* aarg);
// orig: 0x00213830: starts part k on the root (the current part +0x7C)
void StartScreenObjPart(LO* plo, int k);
// orig: 0x002121E8: the group having part k is busy (0x314: the root's part not held)
bool FScreenObjBusy(LO* plo, int k);
// (0x1ADEE8's test on the save icon: shown idle (+0x58 2, timers +0x64 2) for more than dt real seconds (CLOCK +0x3C - +0x5C))
bool FScreenObjIdleLonger(LO* plo, float dt);
// orig: 0x00212E40 / 0x00213B00 / 0x00213928 (see splayer.h)
void SetScreenObjFraction(LO* plo, int k, float g);
void SetScreenObjTime(LO* plo, int k, float t);  // orig 0x00212F80
void SelectScreenObjPart(LO* plo, int k);
// the part the object plays (orig3 +0x6C, Sly 2 +0x7C; -1 none)
int IdScreenObjPartCur(LO* plo);
// orig3: 0x002617F0: the time left of the part k the object's group plays (0 when none)
float DtScreenObjPartLeft(LO* plo, int k);
void ResetScreenObjPart(LO* plo, int k);
// orig: 0x00213BD8 / 0x00213A08 / 0x00213CD8 / 0x00213DB0 (see splayer.h); the last one selects part k in place of
// *pidHandle's (reset), keeping it in the handle
void SelectScreenObjPartAt(LO* plo, int k, int match);
void ResetScreenObjPartAt(LO* plo, int k, int match);
void SetScreenObjPartMatrix(LO* plo, int k, const std::array<float, 6>& m);
// orig: SCREENOBJ +0xF4, projected by 0x218220 before advancing the root's slots.
void SetScreenObjFollow(LO* plo, LO* target);
void SelectScreenObjPartHandle(LO* plo, int k, int* pidHandle);
// the show / idle / hide parts (+0x74 / +0x70 / +0x78), the state (0x212A70) and the timer (0x212A50) set directly
void SetScreenObjParts(LO* plo, int idShow, int idIdle, int idHide);
void SetScreenObjState(LO* plo, int s);
void SetScreenObjTimers(LO* plo, int timers);
// orig: 0x002132D0: it shows something
bool FScreenObjVisible(LO* plo);
// G_GAME +0x3BEC (0x212A70's count): the world's shown screen objects (state >= 2) of priority >= 16
int CBigScreenObjsShown(const SW* psw);
// orig: 0x002121C8: gone (no part shown, the timers below 2)
bool FScreenObjDone(LO* plo);
// +0x58: 0 not loaded, 1 stopped, 2 idle part, 3 show part, 4 hide part
int NScreenObjState(LO* plo);
// +0xB0: the game pauses while it is shown (0x212A70: the clock's factor 7)
void SetScreenObjPauses(LO* plo, bool f);

// Every frame: the world's screen objects updated (0x212C50 on those with a state), at the real step (+0x40).
void UpdateScreenObjs(SW* psw);
// Draws the shown ones into g_hud (between g_hud.Begin and Flush).
void DrawScreenObjs(SW* psw);
// A world loaded: the run-time states of the objects not in it dropped.
void ResetScreenObjs(SW* psw);
// orig: 0x001D9408: clear the shown-screen count, including the restart hold.
void ResetScreenObjPauseCount();

// The script strings (0x216BC8's table 0x3E1C88) the native game provides; "" when not one of them.
std::string StrScreenScript(int id);

// The HUD timer (SW +0x3BC4 = the screen object 0x36F, the fourth of 0x20F710's D_002C3E70; its end SW +0x476C): the
// timed booty's countdown (string 0x2D1)
// orig: 0x001618F8: shown for good (+0x8C 0, vt slot 49 (-1)), ending dt from now
void StartHudTimer(float dt, SW* psw = nullptr);
// orig: 0x00161958: hidden (vt slot 50), its end 0
void StopHudTimer();
// SW +0x476C: the timer's end (0: none)
float THudTimerEnd();
// orig 0x20E890: the timer's seconds left kept over a world change whose request has none of the flags 0xCD
// (taken before the old world goes; restarted after the new one's start)
float DtHudTimerResume(int grfReq);

// orig0x214300: membership in the separately drawn screen-object list.
void SetScreenObjListed(LO* plo, bool listed);
