#pragma once

// Sly 3's two-player split screen on PC (notes/SPLIT3_2026-11-12.md).
//
// The original: a world's entry tables (orig3 0x14FA38) give the screen mode 2 for the two-player games (Cops and
// Robbers: job 2409 / task 1811, Sly 24 and Carmelita 28 in v_ext; the Holland and pirate ones: jobs 3323 / 3334 /
// 4209, tasks 1814..1816), the task's option 1402 names both players' characters (GAME +0x10 / +0x14), each player has
// its own pad record (0x36E740 + i * 0x11C0, the player's +0x130 its index) and the world keeps up to four views
// (SW +0x3EB8 count, +0x3EBC the class-0x05 cameras sorted by +0x14C; each draws into its own rectangle +0x160..+0x16C,
// 0x167AB0, its projection from that rectangle's aspect, 0x166808).
//
// Natively: the screen mode is read as the original reads it (sw.cpp NScreenModeFromPrefix3). Then:
//  - the seats: each player claims a device on the binding screen ("Player 1, press Cross / Space": Space claims the
//    keyboard and mouse, the bottom face button of any pad claims that pad); a player keeps the device for the session.
//    Only in split screen the last-used-device switching is off: P1's prompts and pad are P1's device's.
//  - each player's pad record (seat 0: the game's g_joyps2; seats 1..3: their own) and its prompts (per view).
//  - a camera per player (the world camera's runtime copied, its follow focus the player's character) and a view per
//    camera, Hor+ (each view's aspect from its rectangle: a wider picture shows more, nothing is stretched).
//  - a pad that goes away pauses the game and asks for it again (that player presses Cross / Space again).
// Single player is untouched: nothing here runs unless the split screen is on.
//
// Layouts: Stacked (the original: player 1's view over player 2's, the 2P HUD authored for that: its intro's panes,
// the gauges), Side by Side (the wide-aware variant: each player gets half the width, so each view stays near 16:9 on a
// 32:9 picture), Auto (Stacked up to 16:9, Side by Side from 21:9). The original's 2P HUD is one canvas with a band per
// player (top: player 1, bottom: player 2): natively each band goes into its player's view, fitted, with that player's
// prompts; a menu takes the whole picture.
// Four players (experimental, off by default; PC only, not in VR): four views in a 2 x 2 grid, players 3 and 4 drive
// Bentley and Murray when the world has them (the original has two players only; see the notes' limitations).

#include <cstdint>
#include <string>

#include <glm/vec2.hpp>

struct GLFWwindow;
struct SW;
struct LO;
struct CM;
struct CAMERA;
struct JOYPS2;
struct PADCTL;

namespace split3
{
constexpr int kMaxSeats = 4;

enum LAYOUT : int { LAYOUT_Auto, LAYOUT_Stacked, LAYOUT_SideBySide, LAYOUT_Max };

struct VIEWRECT { int x = 0, y = 0, dx = 1, dy = 1; };   // pixels, y up (GL), in the scene target

// Options (<user-dir>/SplitScreen.ini; the binding screen changes them): the layout, four players (experimental).
LAYOUT Layout();
bool FFourPlayers();
// Tests / harness: SLY3_SPLIT=2|4 forces the split screen in any world (players 2..4 drive the gang members the
// world has), SLY3_SPLITSEATS="k,p0,p1,..." pre-binds the seats (k keyboard, pN pad N, s a scripted seat: no device),
// SLY3_SPLITLAYOUT=auto|side|stacked.
void Startup();

// A world loaded (transition.cpp, after the players were placed): the split screen on when the world's screen mode
// is 2 (or forced); the seats keep their devices between worlds of the session.
void OnWorldLoaded(SW* psw);
// The split screen is on (two or more views).
bool FActive();
int CView();
// (s3-hack) one shared view (the hacking game: one camera for both ships, as the original): CView() is 1, the seats
// (CSeats()) still drive their players' pads
bool FSharedView();
int CSeats();
// The binding screen is up (a seat is to be claimed: the start, or a lost pad): the game is paused under it.
bool FBinding();

// Once per game step before UpdateJoy: the binding, lost pads, the other seats' pad records.
void UpdateSeats(GLFWwindow* pwindow, int frame);
// (tests, s3-hack) a scripted seat's stick / buttons after its presses (a[0] x right, a[1] y up, a[2] / a[3] the right
// stick): hackgame3's SLY3_HACKGAMEPILOT steers players 2..4's ships by play
extern void (*g_pfnScriptedSeatPilot)(int iseat, int frame, float a[4], uint16_t* pgrf);
// (tests) a mode's pad bot: seat i's stick (a[0] x right, a[1] y up, a[2] / a[3] the right stick) and buttons, fed
// through the pad as a player's (seat 0: joy.cpp UpdateJoy; seats 1..: scripted seats); false: that seat untouched
extern bool (*g_pfnPadPilot3)(int iseat, int frame, float a[4], uint16_t* pgrf);
// Seat 0's device restriction for pcinput.cpp's ReadPcInput (the last used device switching off): -2 any (not in
// split screen), -1 the keyboard and mouse only, else that pad's joystick id only (no keyboard).
int Seat0Device();
// The prompts' family for seat i's device (its view's HUD, the binding screen).
int FamilySeat(int iseat);

// The player i's character (0: the current player; 1: the task's second player, GAME +0x14; 2 / 3: the gang members
// of the experimental four-player mode), or null.
LO* PloPlayer(SW* psw, int i);
// the seat (0-based) of the player whose character plo is, -1 none (1.. only: the extra players; the current player 0)
int IExtraPlayerOf(const LO* plo);
// orig3 0x1F4B00 (the world's option 983): player i's character set (null: its pad taken away). Player 1 (i 0) stays
// the current player natively; player 2's character takes player 2's seat (a Carmelita-class guard: SetCarmPlayer3).
void SetPlayerCharacter(SW* psw, int i, LO* plo);
// (for the hacking / vehicle games: s3-hack, s3-plane) player i (0-based seat; 0 = the current player, not changed here)
// drives plo: its pad becomes seat i's (po.cpp EnablePoExtra3: a PADCTL reading seat i's device, the stick in view i's
// camera) and view i's camera follows it. No-op unless the split screen is on with more than i views.
void GivePlayerObject(SW* psw, int i, LO* plo);
// The pad record a player object's controller reads (po.cpp UpdatePadctl): its seat's while the split screen is on;
// null: the game's own (JoyFor(2)). *ppcm: the camera of that player's view (the stick's frame).
const JOYPS2* PjoyForPadctl(const PADCTL* ppc, const CM** ppcm);
// After the world camera's update (cmworld.cpp): the other views' cameras.
void UpdateCameras(SW* psw);
// The view's rectangle in a target of dx x dy pixels.
VIEWRECT RectView(int iview, int dx, int dy);
// The HUD in view i (game.cpp, around DrawHud): sets the canvas transform (g_display.uiScale / uiOffset) for the view's
// aspect (the original two-player HUD: that player's band of the canvas, *pclip its pixels in the view: the other
// band is cut off there); false: no HUD in that view.
bool FHudCanvasView(int iview, float rAspectView, int dx, int dy, VIEWRECT* pclip);
// The two-player HUD's view box of view i in that view's pixels (y up; the box's hole widened as the HUD widens it):
// the scene is drawn there only (the rest of the view is the HUD's own frame). false: the whole view (no box known).
bool FRectScene(int iview, int dx, int dy, VIEWRECT* prect);
// (a two-player mode whose HUD has no view box quads, e.g. the Galleon Duel: its gauges sit beside views the original
// narrows) the view box hole of band 0 / 1 (640 x 448 canvas units, y down); false: none. game.cpp asks each drawn
// frame (ProvideBoxes2p) and records it as the HUD's own box (hud.h abox2p)
extern bool (*g_pfnHole2p3)(int band, float ahole[4]);
void ProvideBoxes2p();
// Side by Side (user feedback): the two-player board's banners (the winner's text, the announcements: the strip the
// original draws between the stacked bands) go under the gauges in the centre column, sized to it, instead of a strip
// across both views. *prect the area (pixels, y up: the scissor), *pscale / *poffset the canvas transform that puts
// the canvas's middle strip there. false: stacked (the original strip over the picture) or no two-player HUD.
// SLY3_SPLITBANNERFULL=1: the old full-width strip (the "before" shots).
bool FBannerArea(int dx, int dy, VIEWRECT* prect, glm::vec2* pscale, glm::vec2* poffset);
// The camera of view i (0: the world camera).
const CM& CmView(int iview);
// orig3: the world's two-player mode SW +0x4FA4 (option 1003, set by 0x25E8C0; the world's own, false in a new world):
// the sounds' 2P attenuation 0x245440 (each player's distance) reads it.
void SetTwoCameras(SW* psw, bool f);
bool FTwoCameras(const SW* psw);
// The other views' viewpoints (views 1..: their cameras' positions, the OR of their flags) for the activation and the
// effects' emission tests (orig3 0x15AF58 / 0x18E728: every view). 0 when the split screen is off.
int CViewExtra();

// (hackmp3.h) the hackers' select: shown; its cards' model areas (pixels, y up, in a dx x dy picture, the full
// picture's canvas transform) with each card's character; game.cpp renders the hacking ship and copies it there after
// DrawOverlay drew the page
bool FHackSelectShown();
int CHackCards();
bool FHackCardModel(int i, int dx, int dy, VIEWRECT* prect, int* pichar);

// The binding screen and each view's player label / lost-pad notice (over everything, the full picture).
// The views' dividing lines before the two-player HUD's centre pass (game.cpp), so the board's banner between the views
// is drawn over them (the line crossed the banner's text); DrawOverlay then leaves them out
void DrawDividers(int width, int height);
void DrawOverlay(int width, int height);
}  // namespace split3
