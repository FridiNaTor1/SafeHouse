#pragma once

// Sly 3: Honor Among Thieves (USA, SCUS-97464): every game-specific number of the executable and the disc in one place.
//
// Source: reverse-engineering notes on the Sly 3 disc, engine core and formats, checked against the executable. Comments: "orig3: 0x..." is a Sly 3 address;
// code copied from Sly2-Release keeps "orig: 0x..." (a Sly 2 address) until it is re-derived from the Sly 3 asm.
//
// The per-game ids (classes, option ids, splice built-ins and tags) are strong enums in ids3.h: a Sly 2 number in
// copied code does not convert to them silently.

#include <cstdint>

namespace orig3
{
// ---------------------------------------------------------------- disc (verified, REPORT §1)
constexpr const char* SERIAL = "SCUS-97464";
constexpr const char* BOOT_FILE = "SCUS_974.64";
constexpr uint32_t ISO_BLOCKS = 1735232;        // the image's volume size, 2048-byte sectors
constexpr uint32_t ELF_LBA = 271;               // SCUS_974.64;1
constexpr uint32_t ELF_SIZE = 3200076;
constexpr const char* ELF_SHA1 = "5941b7877e6bf5dfb8acb2d04a3c5059367f385b";
constexpr uint32_t WAD_LBA = 0xCA0;             // SLY3.WAD;1 (3,492,583,424 bytes): every file key's sectors are relative to it
constexpr uint64_t WAD_SIZE = 3492583424ull;
// The executable's two LOAD segments (readelf -l): .text .. .bss, then .irx
constexpr uint32_t SEG0_VA = 0x100000, SEG0_OFFSET = 0x1000, SEG0_FILESZ = 0x26ADD8;
constexpr uint32_t SEG1_VA = 0x4F3000, SEG1_OFFSET = 0x26C000, SEG1_FILESZ = 0x73FC0;
constexpr uint32_t ENTRY = 0x100008, GP = 0x372DF0;

// ---------------------------------------------------------------- the executable's tables (verified)
// The worlds: 40 entries (world 1, sampler_menu, has an empty key: 39 files). Sly 2 read its world records from the
// boot world (LoadWorldRecordsFromBrx 0x20F318, no Sly 3 counterpart); Sly 3 keeps them here.
constexpr int CWORLD = 40;
constexpr uint32_t VA_AWORLD_NAME = 0x2EDE48;     // char*[40]
constexpr uint32_t VA_AWORLD_EPISODE = 0x2EDF10;  // S32[40]: EPISODE3 of each world
constexpr uint32_t VA_AEPISODE_WORLD = 0x2EDFB0;  // S32[9]: the first world of each episode (-1: none)
constexpr uint32_t VA_AWORLD_FK = 0x2EDFD8;       // 40 x 64-byte keys; accessor 0x25E070 = 0x2EDFD8 + 64 * i
constexpr uint32_t CB_FK = 0x40;
// The movies: 23 keys (338 and 339 twice); four dummy records sit before them (0x3178F8..0x3179B8).
constexpr int CMOVIE_FK = 23;
constexpr uint32_t VA_AMOVIE_FK = 0x3179F8;
constexpr uint32_t CB_MOVIE_FLAG = 0x3FFF;        // the low bits of a movie key's cb are flags: size = cb & ~0x3FFF
// Classes, options, splice
constexpr uint32_t VA_ACLS = 0x3409E0;            // 249 x {base, cid, cidLast, size, pfnNew} (0x14 each), as Sly 2's
constexpr int CCLS = 249;
constexpr uint32_t CB_CLS = 0x14;
constexpr uint32_t VA_AEOPID = 0x32CA80;          // 2044 x 0x28: {k +0, grf +4, pfn +8, ..., c18 +0x18, pmf +0x20}
constexpr int CEOPID = 2044;
constexpr uint32_t CB_EOPID = 0x28;
constexpr uint32_t VA_ABIF = 0x342030;            // 178 x 12: the splice built-ins
constexpr int CBIF = 178;
constexpr uint32_t CB_BIF = 12;
constexpr uint32_t VA_JT_SPLICE_TAG = 0x31F610;   // PpairSerializeIn's 32-entry tag dispatch
constexpr uint32_t VA_JT_OPTION_KIND = 0x320480;  // LoadOptionFromBrx's 24-entry kind dispatch (field path)
constexpr uint32_t VA_JT_REFOPTION_KIND = 0x320420;  // RefReadOption's

// ---------------------------------------------------------------- engine globals (from the asm; roles partly guessed)
constexpr uint32_t VA_SW = 0x475480;              // the world (LoadSwFromBrx's a0 at boot; +0x4638 iwe, +0x462C cwe)
constexpr uint32_t VA_CONDITIONS = 0x36BC10;      // the object conditions' set (0x151D10, nodes 0x169150, options 0x1500F0)
constexpr uint32_t VA_FDEBUG_SPLICE = 0x47A488;   // the group's script-debug flag (the S32 after the bulk header)
constexpr uint32_t VA_BULK_CB = 0x47A484, VA_BULK_FLOAD = 0x47A48C, VA_BULK_KEY = 0x47A490;
constexpr uint32_t VA_PREFIX_KEY_A = 0x36C700 + 0x1498, VA_PREFIX_KEY_B = 0x36C700 + 0x14A0;  // 0x14FA38's keys
constexpr uint32_t VA_GAME = 0x36DBE0;            // read by 0x14FA38 (+0xC flags), 0x234500 (+0xC & 3: the texture pass)

// ---------------------------------------------------------------- engine functions (REPORT §2.3)
constexpr uint32_t FN_MAIN_LOOP = 0x1CB1D8;
constexpr uint32_t FN_STARTUP = 0x1CB620, FN_STARTUP_GAME = 0x184918;
constexpr uint32_t FN_BOOT = 0x1C6EA8;            // queues movies 338, 339, then world 35 (m_ext) with arg 0x1A8
constexpr uint32_t FN_PFK_WORLD = 0x25E070;       // the key of world i
constexpr uint32_t FN_TRANSITION = 0x25C2A8;      // guess: PswUpdateTransition's role
constexpr uint32_t FN_LOAD_SW = 0x14F440;         // LoadSwFromBrx
constexpr uint32_t FN_LOAD_PREFIX = 0x14FA38;     // new: the two arrival(?) tables before the group count
constexpr uint32_t FN_LOAD_SW_OBJS = 0x151D80;    // LoadSwObjsFromBrx
constexpr uint32_t FN_PLO_NEW = 0x150690, FN_PLO_CLONE = 0x1507E0;
constexpr uint32_t FN_FILTER_MATCHES = 0x151C98, FN_CONDITIONS_MATCH = 0x151D10;
constexpr uint32_t FN_LOAD_OPTIONS = 0x1519C8, FN_LOAD_OPTION = 0x150AE8, FN_REF_READ_OPTION = 0x150960;
constexpr uint32_t FN_LOAD_PROGRESS = 0x21BFF8, FN_LOAD_MUSIC = 0x246F58, FN_LOAD_TEXT = 0x14FD20;
constexpr uint32_t FN_LOAD_NODES = 0x14FF58, FN_LOAD_OPTION_SETS = 0x1500F0;
constexpr uint32_t FN_GLOBALS_25B1B8 = 0x25B1B8, FN_SAVE_1CF640 = 0x1CF640, FN_GLOBALS_25EB00 = 0x25EB00;
constexpr uint32_t FN_GLOBALS_25B6B8 = 0x25B6B8, FN_CONDITIONS_169398 = 0x169398, FN_GROUP_162A88 = 0x162A88;
constexpr uint32_t FN_STARTUP_SW_SPLICE = 0x249980, FN_LOAD_SW_SPLICE = 0x2499F0, FN_PPAIR_SERIALIZE_IN = 0x12C108;
constexpr uint32_t FN_LOAD_BULK_HEADER = 0x25C930, FN_LOAD_BULK = 0x25C9A0;
constexpr uint32_t FN_LOAD_SHADERS = 0x234EA0, FN_LOAD_CLUTS = 0x234518, FN_LOAD_CLUT = 0x233EA8;
constexpr uint32_t FN_LOAD_BMPS = 0x2347C8, FN_LOAD_BMP = 0x234648, FN_LOAD_SHD = 0x234A58, FN_LOAD_TEX = 0x234900;
constexpr uint32_t FN_PSAA_LOAD = 0x236EB0, FN_TEXTURE_PASS = 0x234500;
constexpr uint32_t FN_LOAD_CMESH = 0x1ED7B0;
constexpr uint32_t FN_LOAD_PROXY = 0x1FA778;
constexpr uint32_t FN_LOAD_SOUND_BANK = 0x247470;
constexpr uint32_t FN_ALLOC = 0x1D27F0;
constexpr uint32_t FN_LZSS = 0x118E28, FN_FK_DECODE = 0x1DBE80;
constexpr uint32_t FN_LOAD_GLOBSET = 0x18D148, FN_MESH_DISPATCH = 0x18FD10;
constexpr uint32_t FN_LOAD_ASEG = 0x135FB0;
constexpr uint32_t FN_SAVE_SLOT_READ = 0x1D0410, FN_SAVE_SLOT_WRITE = 0x1D0588;
constexpr uint32_t CB_SAVE_SLOT = 0x6808, CB_SAVE_HEADER = 0x5C;

// ---------------------------------------------------------------- the boot (0x1C6EA8)
constexpr int MOVIE_LOGO_A = 338;  // 4.4 s
constexpr int MOVIE_LOGO_B = 339;  // 14.3 s
constexpr int WORLD_PROLOGUE = 35; // m_ext, Kaine Island; the title plays over it
constexpr int WORLD_BOOT_ARG = 0x1A8;
}  // namespace orig3

// The episodes (the values of the table at 0x2EDF10; names from the reverse-engineering notes).
enum class EPISODE3 : int
{
    MultiPlayer = 0,
    Intro = 1,
    Trainer = 2,
    Venice = 3,
    Outback = 4,
    Holland = 5,
    China = 6,
    Pirate = 7,
    Isle = 8,
};
constexpr int CEPISODE3 = 9;
inline const char* SzEpisode3(int i)
{
    static const char* const s_asz[CEPISODE3] = {"MultiPlayer", "Intro", "Trainer", "Venice", "Outback",
                                                 "Holland",     "China", "Pirate",  "Isle"};
    return i >= 0 && i < CEPISODE3 ? s_asz[i] : "?";
}
