#pragma once

// The safe house's shop (orig 0x3DA150 on screen object 0x378, module 0x1DAC58..0x1DE2A0): buying the powerups
// (page 0: the items 7..0x2A of the table 0x2BCD00 open in the episodes played and not owned) and selling the
// loot back (page 1: the loot of the progress "loot mplootkc", the values 0x2C3288), with the coins of "loot
// ccoin". Opened from the safe house's computer (class 0x46: 0x1D72A8, po.cpp).

#include <string>

struct SW;
struct LO;

void InitShop(SW* psw);              // orig 0x001DB7F0 (reset) + its object (0x1DA770)
void UpdateShop(SW* psw);            // orig 0x001DB870 (every frame, with the HUD)
void SetShopPage(int n);             // orig 0x001DC260: -1 closed, 0 buy, 1 sell
bool FShopShown();
void NotifyPlayersPowerups(SW* psw);   // orig 0x0019F0A8 (after a purchase: the players' event 0x1A)
std::string StrShopScript(int id);   // 0x43C..0x453 (its rows), 0x435 (the help), 0x436 (the title)

// The gadget assignment (orig 0x3DA0F0 on 0x375 / 0x376 / 0x377 by the character): the owned powerups on a grid,
// L1 / L2 / R2 assign the selected one to that button (the progress "<sly|bentley|murray> apukCur <i>"). Opened by
// Select in play (orig the player's 0x1403D0 -> 0x1DB490).
void OpenGadgetMenu(SW* psw, LO* ploPlayer);
void SetGadgetMenu(int s);           // orig 0x001DB520: -1 closed, 0 Sly, 1 Bentley, 2 Murray
void UpdateGadgetMenu(SW* psw, LO* ploPlayer);   // orig 0x001DAA90
bool FGadgetMenuShown();
std::string StrGadgetScript(int id); // 0x37A / 0x37B
struct FONTBRX;
void DrawGadgetIcons(const FONTBRX* pfont, const FONTBRX* picons);   // orig 0x001DAC58 (with the screen objects)

// The job help (orig 0x3DA3C0 on 0x4E0): page 0 the episode's open jobs, page 1 a job's goals (the pause menu's Job
// Help: 0x1B0808 -> 0x1DD300(the current job ? 1 : 0)).
void InitJobHelp(SW* psw);
void SetJobHelpPage(int s);          // orig 0x001DD300: -1 closed
void UpdateJobHelp(SW* psw);         // orig 0x001DCD30
bool FJobHelpShown();
std::string StrJobHelpScript(int id);   // 0x51B..0x52B
