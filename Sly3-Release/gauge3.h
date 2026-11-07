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
// the gauge shown for this player (its class-0xA3 child), none for null (orig3 0x1F4548: a pad took / left the player)
void SetGaugePlayer3(LO* player);
// (interim) true: the gauge follows the current player each frame (none under the title or the card dialog). The player
// code (the player code: the pad's assignment, 0x1F4548) can clear it and call SetGaugePlayer3 itself.
extern bool g_fGaugeFollowsPlayer3;

// The coin counter (class 0xA7, vtable 0x366F48: m_ext's 1048, script string 812 "%d": orig3 0x26A1F8 / 0x25F0E8)
void UpdateCoins3(SW* psw);           // every frame (the HUD's update)
int CCoinsShown3(const LO* plo);      // the count it shows (+0x120); the save's coins for another object
