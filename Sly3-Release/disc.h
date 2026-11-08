#pragma once

// The game's data. Sly 3 (installations/sly3) is installed as its disc image (disc.iso; platform/install.h): the game
// data is the ISO9660 file SLY3.WAD at sector 0xCA0, and the game names a file by its 64-byte key, whose start sector
// is absolute (game/sly3.h). (The converted-files layout is Sly 2's, kept from the fork for a future Sly 3 converter.)
// The executable is read for its data tables only.

#include <cstddef>
#include <cstdint>
#include <map>
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

// The disc's region (regions.md). The port's code is the same for every region: it reads the executable's data
// tables at the USA executable's addresses and CDisc translates them to the disc's executable.
enum class DISCREGION
{
    Usa,      // SCUS-97316 (the decomp's executable)
    Europe,   // SCES-52529 v1.00 and v2.01: 11 languages, 50 Hz
    Japan,    // SCPS-15090
};

struct REGIONRANGE
{
    uint32_t vaUsa;   // the first USA address of the range (each range runs to the next one's start)
    int32_t dva;      // the region's address minus the USA address
};

// A word of another region's executable set back to the USA executable's value when the disc is opened (the Japanese
// build's field offsets).
struct REGIONPATCH
{
    uint32_t vaUsa;
    uint32_t u;
};

// Decodes a 64-byte record as the original does (0x20E890, 0x1B3BD0, 0x1FC510).
FK FkFromRecord(const void* record64);

class CDisc
{
public:
    // Opens the converted files (a folder with extraction_manifest.json, or that file) or a disc image; finds the
    // boot executable and recognizes the disc (USA, Europe v1.00 / v2.01, Japan). With converted files, a file of
    // the mods folder (SetModsDirectory) at the same relative path replaces the installed one.
    bool FOpen(const std::string& path, std::string& error);
    // The mods folder (before FOpen; empty: none).
    void SetModsDirectory(const std::string& dir) { m_dirMods = dir; }
    // True: the converted files; false: a disc image (the developer option).
    [[nodiscard]] bool FConverted() const { return m_fConverted; }
    // The boot executable's name (SCUS_973.16, SCES_525.29, SCPS_150.90).
    [[nodiscard]] const std::string& SzExecutable() const { return m_szExe; }
    // A file of the image's ISO 9660 root (the converter: the executable, SYSTEM.CNF); false with converted files.
    bool FReadRootFile(const std::string& name, std::vector<uint8_t>& ab) const;
    [[nodiscard]] DISCREGION Region() const { return m_region; }
    [[nodiscard]] const char* SzDisc() const { return m_szDisc; }
    [[nodiscard]] bool FIsOpen() const { return !m_path.empty(); }

    // A file's bytes.
    std::vector<uint8_t> ReadFile(const FK& fk) const;
    // A world-format file's bytes, decompressed (worlds, screen files: LZSS on the disc, stored decompressed).
    std::vector<uint8_t> ReadWorld(const FK& fk) const;
    // cb bytes at offset ib of the file that starts at sector isector (streams: banks, music, dialog, movies).
    // Converted files: the bytes past the file's end read as zeros; false when no file starts there. A disc image:
    // the image's bytes (past the file, the next sectors'), false past the image's end.
    bool FReadFileBytes(uint32_t isector, uint64_t ib, void* pv, std::size_t cb) const;
    // The size of the file at isector: the installed (or modded) file's; on a disc image the key's cbKey.
    [[nodiscard]] uint64_t CbFile(uint32_t isector, uint64_t cbKey) const;
    // Converted files: the file at isector (the installed one or the mods folder's); "" on a disc image.
    [[nodiscard]] std::string PathOf(uint32_t isector) const;
    // A part of the file at isector (a movie's sound track "audio<track>", a bank's sample "sample<offset>"): its file,
    // the mods folder's when it has one; "" when the file has no such part (or on a disc image).
    [[nodiscard]] std::string PathPart(uint32_t isector, const std::string& key) const;
    // The installed file at isector, never the mods folder's ("" on a disc image).
    [[nodiscard]] std::string PathInstalled(uint32_t isector) const;
    // The file at isector comes from the mods folder.
    [[nodiscard]] bool FModded(uint32_t isector) const;
    [[nodiscard]] const std::string& Path() const { return m_path; }
    [[nodiscard]] const std::string& DirMods() const { return m_dirMods; }
    // The files the mods folder replaces.
    [[nodiscard]] int CModFiles() const { return m_cmod; }
    // The name of the world file at isector (the manifest's: "Paris", "Front End"); "" when unknown (a disc image).
    [[nodiscard]] std::string SzWorldName(uint32_t isector) const;

    // The executable's loaded image (segment 0x100000..), for reading data tables at their original addresses.
    // va is always a USA executable address: on another region's disc it is translated to that executable's.
    [[nodiscard]] const uint8_t* PbElf(uint32_t va, uint32_t cb) const;
    // A word at va. A word that is an address in the executable (a pointer in a table: a function, a string, another
    // table) is returned as the USA address, so the port compares and follows addresses the same on every disc.
    uint32_t U32Elf(uint32_t va) const;
    std::string StrElf(uint32_t va) const;

    // The file keys in the executable: the worlds' (0x2EDFD8) and the movies' (0x3179F8), each file once.
    std::vector<std::pair<uint32_t, FK>> ElfKeys() const;
    // orig3 0x25E070: world iworld's key (0..39; empty for world 1, sampler_menu), its name (0x2EDE48: "m_ext") and
    // its episode (0x2EDF10: sly3::EPISODE); the world of a name, -1 when none.
    [[nodiscard]] FK FkWorld(int iworld) const;
    [[nodiscard]] std::string SzWorldElf(int iworld) const;
    [[nodiscard]] int IepisodeOfWorld(int iworld) const;
    [[nodiscard]] int IworldFromName(const std::string& name) const;

    // The disc executable's own data at its own address (data the USA executable has not: the European build's
    // language labels), and that table's address (0 when the disc has none).
    [[nodiscard]] const uint8_t* PbElfOwn(uint32_t vaRegion, uint32_t cb) const;
    [[nodiscard]] uint32_t VaLanguageLabels() const { return m_vaLanguageLabels; }
    // the region's index in the executable's language table 0x2BFBD0 (orig 0x2E1E58, set at boot by 0x1613D0)
    [[nodiscard]] int IregionLanguages() const { return m_iregionLanguages; }

    // USA address <-> the disc executable's address (identity on the USA disc).
    [[nodiscard]] uint32_t VaRegion(uint32_t vaUsa) const;
    [[nodiscard]] uint32_t VaUsa(uint32_t vaRegion) const;

private:
    bool FOpenConverted(const std::string& dir, std::string& error);
    bool FRecognize(const std::string& szBoot, std::vector<uint8_t>&& elf, const std::string& where, std::string& error);
    struct FILEENT
    {
        std::string path;        // the installed file, or the mods folder's
        std::string pathInstalled;
        bool fDecompressed = false;  // a world-format file stored decompressed
        bool fMod = false;           // the mods folder's
        std::map<std::string, std::string> mppart;  // its parts' files (PathPart)
    };
    const FILEENT* PentFind(uint32_t isector) const;

    std::string m_path;
    bool m_fConverted = false;
    std::string m_szExe;
    std::string m_dirMods;
    int m_cmod = 0;
    std::map<uint32_t, std::string> m_mpsectorWorldName;  // (the world files' names, every copy's sector)
    std::vector<std::pair<uint32_t, FILEENT>> m_afile;  // sorted by sector
    std::vector<uint8_t> m_abElf;  // the ELF file
    DISCREGION m_region = DISCREGION::Usa;
    const char* m_szDisc = "";
    uint32_t m_vaLanguageLabels = 0;
    int m_iregionLanguages = 0;
    struct SEG { uint32_t va, offset, cb; };
    SEG m_aseg[2] = {};
    uint32_t m_vaHi = 0;                         // the end of the executable's image (bss included)
    const REGIONRANGE* m_amap = nullptr;         // USA -> region, sorted by vaUsa
    size_t m_cmap = 0;
    std::vector<REGIONRANGE> m_amapInverse;      // region -> USA ({vaRegion, -dva}), sorted
};

extern CDisc g_disc;

// Well-known keys in the executable. Sly 2 had a boot world (93: the world records) and a front-end world (57);
// Sly 3 boots straight into m_ext (world 35, orig3 0x1C6EA8) and its title and menus run over it, so both name m_ext's
// key until the copied front end is re-derived.
constexpr uint32_t VA_FK_BOOT_WORLD = 0x2EDFD8 + 0x40 * 35;  // orig3: sly3::VA_WORLD_KEYS + 64 * sly3::WORLD_PROLOGUE
constexpr uint32_t VA_FK_FRONTEND = VA_FK_BOOT_WORLD;
