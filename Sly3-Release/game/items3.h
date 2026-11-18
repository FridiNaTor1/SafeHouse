#pragma once

// Sly 3's power-up / gadget table and the item ids (s3-decomp, 2026-11-10; notes/DECOMP3_2026-11-10.md "Gadgets").
//
// The table: orig3 0x343098, 67 entries of 0x3C bytes (Sly 2's 0x2BCD00 had 0x2B of 0x20), indexed by the item id
// (the progress "powerUps mppukfAcquired <id>", the slots "<player> apukCur <i>", the players' vt slots 113..116
// can-do / start / held / end, e.g. Sly's start 0x1AF6C0 case 0x21 = Thief Reflexes). Fields: +0 the kind of use (2
// a press, 1 held, -1 never on a button), +4 the character (24 Sly, 25 Bentley, 26 Murray, 28 a fourth set with no
// names), +8 the price, +0xC the power it costs, +0x10 the power a second while held (0x154608), +0x14 the episode it opens in, +0x18 the ThiefNet
// page (14 / 15), +0x1C its name, +0x20 its description and +0x24 the gadget menu's button line (a format, "%s" the button:
// 0x2207E8) (text ids), +0x28.. a clip / sound.
//
// The copied Sly 2 gadget code (gadget.cpp, shop.cpp, hudwidget.cpp) keeps Sly 2's ids; the two id spaces meet here:
// Item3FromItem2 / Item2FromItem3 pair the items by name (Sly 2's and Sly 3's English text); a Sly 3 item with no
// Sly 2 twin gets the Sly 2-side id ITEM2_SLY3 + its Sly 3 id, so it passes through the copied code.

#include <cstdint>

namespace sly3
{
inline constexpr uint32_t VA_ITEMS = 0x343098;
inline constexpr uint32_t CB_ITEM = 0x3C;
inline constexpr int C_ITEM = 67;
inline constexpr int ITEM2_SLY3 = 100;       // Sly 2-side ids of the Sly 3-only items: 100 + the Sly 3 id
inline constexpr int CHR3_SLY = 24, CHR3_BENTLEY = 25, CHR3_MURRAY = 26;

struct ITEM3
{
    int use = -1, chr = -1, price = 0, cost = 0, drain = 0, episode = -1, page = -1, name = -1, desc = -1, fmt = -1;
};
ITEM3 Item3(int id3);                 // the table's row (all -1 / 0 outside it)
int Item3FromItem2(int id2);          // -1: no Sly 3 twin (Snooze Bomb, Stealth Slide, Time Rush, ...)
int Item2FromItem3(int id3);          // ITEM2_SLY3 + id3 when Sly 2 had none
int Kind2FromChr3(int chr3);          // Sly 2's character kinds 7 Sly / 8 Bentley / 9 Murray (else chr3)
bool FItem2Valid(int id2);            // an item of the table (Sly 2-side id)
}  // namespace sly3
