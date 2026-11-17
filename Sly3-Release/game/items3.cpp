// Sly 3's power-up / gadget table and the Sly 2 <-> Sly 3 item ids (items3.h).

#include "game/items3.h"

#include "disc.h"

namespace sly3
{
namespace
{
// Sly 2 id -> Sly 3 id, paired by name (Sly 2's table 0x2BCD00 +0x14 and Sly 3's 0x343098 +0x1C in the two games'
// English text; s3-decomp 2026-11-10). 0..4 are the table's unnamed entries (the same in both); Sly 2's 6 (held) is
// Sly 3's 5. Sly 2 items with no Sly 3 twin: 5, 9 Snooze Bomb, 16 Turnbuckle Launch, 18 Atlas Strength, 25 Stealth
// Slide, 32 Tornado Strike, 35 Voltage Attack, 36 Long Toss, 38 Music Box, 39 Lightning Spin, 41 TOM, 42 Time Rush.
constexpr int c_aitem3[43] = {
    0, 1, 2, 3, 4, -1, 5,
    6,    // 7 Trigger Bomb
    14,   // 8 Size Destabilizer
    -1,   // 9 Snooze Bomb
    9,    // 10 Adrenaline Burst
    10,   // 11 Health Extractor
    11,   // 12 Hover Pack
    16,   // 13 Reduction Bomb
    22,   // 14 Temporal Lock
    21,   // 15 Fists of Flame
    -1,   // 16 Turnbuckle Launch
    19,   // 17 Juggernaut Throw
    -1,   // 18 Atlas Strength
    24,   // 19 Diablo Fire Slam
    18,   // 20 Berserker Charge
    20,   // 21 Guttural Roar
    23,   // 22 Raging Inferno Flop
    25,   // 23 Smoke Bomb
    26,   // 24 Combat Dodge
    -1,   // 25 Stealth Slide
    8,    // 26 Alarm Clock
    27,   // 27 Paraglide
    28,   // 28 Silent Obliteration
    33,   // 29 Thief Reflexes
    29,   // 30 Feral Pounce
    30,   // 31 Mega Jump
    -1,   // 32 Tornado Strike
    31,   // 33 Knockout Dive
    12,   // 34 Insanity Strike
    -1,   // 35 Voltage Attack
    -1,   // 36 Long Toss
    15,   // 37 Rage Bomb
    -1,   // 38 Music Box
    -1,   // 39 Lightning Spin
    32,   // 40 Shadow Power (Sly 3's Shadow Power Level 1)
    -1,   // 41 TOM
    -1,   // 42 Time Rush
};

int32_t S32(uint32_t va) { return int32_t(g_disc.U32Elf(va)); }
}  // namespace

ITEM3 Item3(int id3)
{
    ITEM3 r;
    if (id3 < 0 || id3 >= C_ITEM) return r;
    const uint32_t va = VA_ITEMS + CB_ITEM * uint32_t(id3);
    r.use = S32(va);
    r.chr = S32(va + 0x4);
    r.price = S32(va + 0x8);
    r.cost = S32(va + 0xC);
    r.drain = S32(va + 0x10);
    r.episode = S32(va + 0x14);
    r.page = S32(va + 0x18);
    r.name = S32(va + 0x1C);
    r.desc = S32(va + 0x20);
    r.fmt = S32(va + 0x24);
    return r;
}

int Item3FromItem2(int id2)
{
    if (id2 >= ITEM2_SLY3) return id2 - ITEM2_SLY3 < C_ITEM ? id2 - ITEM2_SLY3 : -1;
    return id2 >= 0 && id2 < int(sizeof(c_aitem3) / sizeof(c_aitem3[0])) ? c_aitem3[id2] : -1;
}

int Item2FromItem3(int id3)
{
    if (id3 < 0 || id3 >= C_ITEM) return -1;
    for (int i = 0; i < int(sizeof(c_aitem3) / sizeof(c_aitem3[0])); ++i)
        if (c_aitem3[i] == id3) return i;
    return ITEM2_SLY3 + id3;
}

int Kind2FromChr3(int chr3)
{
    switch (chr3)
    {
    case CHR3_SLY: return 7;
    case CHR3_BENTLEY: return 8;
    case CHR3_MURRAY: return 9;
    default: return chr3;
    }
}

bool FItem2Valid(int id2) { return Item3FromItem2(id2) >= 0; }
}  // namespace sly3
