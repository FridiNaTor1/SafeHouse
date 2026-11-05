#pragma once

// The user's Sly 3 disc image (installations/sly3/disc.iso): the executable (read for its data tables only) and
// the game's files in SLY3.WAD (LBA 0xCA0), located through 64-byte file keys exactly as in Sly 2.
// orig3 numbers: sly3/addr.h.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A file on the disc: the decoded form of the game's 64-byte keyed record (the CHK/FK records of the original:
// isector = (w[0x00] ^ w[0x34]) + 0xCA0, cb = w[0x04] ^ w[0x3C], id = w[0x08] ^ w[0x38]).
struct FK
{
    uint32_t isector = 0;
    uint32_t cb = 0;
    uint32_t id = 0;
};

// Decodes a 64-byte record as the original does (orig: (sly2) 0x20E890, 0x1B3BD0, 0x1FC510; orig3: 0x1DBE80, the
// same instructions).
FK FkFromRecord(const void* record64);

// A world of the executable's tables (orig3: names 0x2EDE48, episodes 0x2EDF10, keys 0x2EDFD8).
struct WORLD3
{
    int iworld = -1;
    std::string name;    // "m_ext"
    int episode = -1;    // EPISODE3
    uint32_t vaFk = 0;   // its key's address
    FK fk;               // cb 0: no file (sampler_menu)
};

// A movie key of the executable (orig3: 0x3179F8, 23 records; the size without the flag bits).
struct MOVIE3
{
    uint32_t vaFk = 0;
    FK fk;               // fk.cb: the file's size (cb & ~0x3FFF)
    uint32_t grf = 0;    // the low bits of the record's cb
};

class CDisc
{
public:
    // Opens the image; checks it is the USA Sly 3 (SCUS_974.64 at its known place, its SHA-1).
    bool FOpen(const std::string& path, std::string& error);
    [[nodiscard]] bool FIsOpen() const { return !m_path.empty(); }

    // Raw bytes of a file on the disc.
    std::vector<uint8_t> ReadFile(const FK& fk) const;
    // A file's bytes, LZSS-decompressed (world files).
    std::vector<uint8_t> ReadWorld(const FK& fk) const;
    // cb bytes at byte offset ib of the image (streams: music, dialog, movies); false past the end.
    bool FReadBytes(uint64_t ib, void* pv, std::size_t cb) const;
    [[nodiscard]] const std::string& Path() const { return m_path; }

    // The executable's loaded image (segment 0x100000..), for reading data tables at their original addresses.
    [[nodiscard]] const uint8_t* PbElf(uint32_t va, uint32_t cb) const;
    uint32_t U32Elf(uint32_t va) const;
    std::string StrElf(uint32_t va) const;

    // The file keys of the executable: the 40 world keys, then the 23 movie keys (with their sizes).
    std::vector<std::pair<uint32_t, FK>> ElfKeys() const;
    // The executable's world and movie tables.
    std::vector<WORLD3> Worlds() const;
    std::vector<MOVIE3> Movies() const;
    // The first world of an episode (orig3: 0x2EDFB0), -1: none.
    int IworldFirstOfEpisode(int episode) const;

private:
    std::string m_path;
    std::vector<uint8_t> m_abElf;  // the ELF file
};

extern CDisc g_disc;

// Sly 2's well-known keys (orig: (sly2) 0x2A37E8 / 0x2A38A8). Sly 3 has no boot world: its boot (orig3: 0x1C6EA8)
// queues the logo movies and then world 35 (m_ext) from the world table. Kept for the copied Sly 2 code that still
// names them; every use is a TODO(sly3) and warns at compile time.
[[deprecated("Sly 2 address: Sly 3 keys are in disc.h Worlds() / sly3/addr.h (TODO(sly3))")]] constexpr uint32_t VA_FK_FRONTEND = 0x2A37E8;
[[deprecated("Sly 2 address: Sly 3 keys are in disc.h Worlds() / sly3/addr.h (TODO(sly3))")]] constexpr uint32_t VA_FK_BOOT_WORLD = 0x2A38A8;
