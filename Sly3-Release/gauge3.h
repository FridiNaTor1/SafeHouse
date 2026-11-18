#pragma once

// Sly 3's character gauge: the screen object class 0xA3 (vtable 0x3639B8) under each player, m_ext's
// 7465 under Sly 24 (the blue head in the oval), 6575 under Bentley 25 (the green one), 4370 Murray's. Its own slots:
// 7 init 0x21DEE8, 18 update 0x21E0C8 (0x21E588 the two-player layout, 0x21E028 the fractions, 0x21E158 the state
// step, 0x21E3B8 the two-player chatter), 54 hide 0x21E4F0, 56 show 0x21E558, 57 0x21E520; the state 0x21DF20 (+0x138:
// -1 hidden, 0 showing, 1 idle, 2 a hit (part 0x3C4), 3 the health bar moving, 4 the power bar moving, 5 / 6 the
// player's states 0x3C5 / 0x3C6); the bars are the parts 0x3CA (health: +0x120 shown, +0x124 the player's) and 0x3CB
// (gadget power: +0x128 / +0x12C), their fractions the screen object's (0x261580).
//
// A player's gauge is shown when a pad takes the player (orig3 0x1F4548: the actor's +0x168, its vt slot 56) and hidden
// when the pad leaves it (slot 54): natively, the current player's (hudwidget.h PloPlayerPo).

struct SW;
struct LO;

// the player's values the gauge shows (the actor +0x16C / +0x170 the health and its most, +0x174 / +0x178 the gadget
// power and its most, orig3 0x1F43C8 / 0x1F43F0 / 0x1F4430 / 0x1F4458). Default: the copied actor's (damage.h /
// gadget.h). the player code may supply Sly 3's: return false when the player has none.
struct GAUGEVALUES3
{
    int cHit = 0, cHitMax = 0;
    int nPower = 0, nPowerMax = 0;
};
extern bool (*g_pfnGaugeValues3)(LO* player, GAUGEVALUES3* pv);

void InitGauges3(SW* psw);     // after a world load (the HUD's init)
// every frame (the HUD's update): player the current one (hudwidget.cpp PloPlayerPo: none under the title)
void UpdateGauges3(SW* psw, LO* player);
void ResetGauges3();           // the world released
// (s3-plane) a gauge's player (its parent actor, or the SO whose EOPID 948 names it: the planes), null none; the gauge of a
// player (null none); a gauge given to another player (null: none; the player's other gauge released)
LO* PloGaugePlayer3(LO* plo);
LO* PloGaugeOf3(LO* player);
void SetGaugeOwner3(LO* plo, LO* player);
// (PC, split screen's teams: views 3 / 4 draw band 1 / 2 of the 2P HUD, whose gauges are players 1 / 2's) before view
// iview's HUD pass: the band's gauge shows view iview's player (a copy of Sly / Carmelita: its own health / power,
// smoothed as the gauge does); iview -1 after the pass: the gauges' own values again
void ApplyViewGauges3(SW* psw, int iview);
// the gauge shown for this player (its class-0xA3 child), none for null (orig3 0x1F4548: a pad took / left the player)
void SetGaugePlayer3(LO* player);
// (interim) true: the gauge follows the current player each frame (none under the title or the card dialog). The player
// code (the player code: the pad's assignment, 0x1F4548) can clear it and call SetGaugePlayer3 itself.
extern bool g_fGaugeFollowsPlayer3;

// The coin counter (class 0xA7, vtable 0x366F48: m_ext's 1048, script string 812 "%d": orig3 0x26A1F8 / 0x25F0E8)
void UpdateCoins3(SW* psw);           // every frame (the HUD's update)
int CCoinsShown3(const LO* plo);      // the count it shows (+0x120); the save's coins for another object

// The HUD notice (the script command 0x1C, orig3 widget D_0046FD00: gauge3.cpp): the script hooks and its update
void InstallNotice3();
void UpdateNotice3();
bool FNoticeShown3();
// The 3-D question (s3-3d, 2026-11-12): the notice n 1 is the "play with the red/blue 3-D glasses?" choice (text 151, its
// answers 152 "No 3-D for me right now" (triangle: 0) / 153 "Yes, Bring on the 3-D!" (square: -1, the world's own mode)).
// The answer, orig3 D_0036DBA4 (GAME 0x36C700 +0x14A4; also the pause menu's 3-D resets 0x1D54A0, -1 again at a job's
// close 0x187270), is read only by the next world load (0x14FA38: 0 forces the normal view, -1 keeps the world's entry
// table mode, 1 the 3-D (anaglyph) mode). Asked as the original asks (s3fix-ai3: the 3-D mode is drawn, render.cpp);
// Offered only with Settings.ini [Graphics] 3D Mode on (s3-3dtoggle, the launcher's Sly 3 Graphics "3D mode (red/cyan
// glasses)"; off by default): off, answered No at once, never shown, and the job chooser has no "... in 3-D" start.
// SLY3_ASK3D=1 / 0 overrides the setting (the test harnesses' routes: 0).
extern int g_n3dChoice3;   // orig3: D_0036DBA4
bool FAsk3d3();            // (3D Mode on, or SLY3_ASK3D=1; SLY3_ASK3D=0 never)
// orig3 0x18ADB0: the current job's screen mode (0x186200: its +0x40, option 1367) is 1, the 3-D mode
bool FJob3d3();
// orig3 0x18AE00: a 3-D game of the chooser runs (challenge3.h FChallenge3d3) or the current job is a 3-D one
bool F3dChoosable3();

// (s3-hack) Sly 3 EOPID 948 (the SO's +0x168): the SO's gauge (h_hack's ships name theirs; the gauge's player then)
void SetSoGauge3(LO* pso, LO* pgauge);
LO* PloSoGauge3(const LO* pso);
// (s3-hack, PC 3 / 4 player hacking game) the two-player gauges' players are split3's seats (up to 4); a gauge moved by
// (u, v) canvas units from where it is authored (its root, SetScreenObjRootXY at scale 1) into its player's corner;
// fOn false: where authored (the original's two players)
void SetGaugeSlot3(LO* pgauge, bool fOn, float u, float v);
// orig3 0x21E4F0 / 0x21E558: the player's gauge (its +0x168) hidden / shown by the caller (Sly's state 22)
void ShowGaugeOf3(LO* player, bool fShow);
