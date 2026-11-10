#pragma once
// The games the launcher lists, and identification of PlayStation 2 disc
// images by the boot file named in their SYSTEM.CNF. Shared by the launcher
// and cane-extractor; no GL, GLFW or JSON dependencies.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cane {
enum class GameId : int { Sly1, Sly2, Sly3, Sly4, Hackpack };
inline constexpr int kGameCount = 5;

struct GameInfo {
    GameId id;
    const char* key;        // folder and config name: installations/<key>
    const char* shortName;  // "Sly 2"
    const char* title;      // "Sly 2: Band of Thieves"
    const char* blurb;      // one line under the title
    const char* runtime;    // runtime executable (without .exe), nullptr: none planned yet
    const char* bootFile;   // SYSTEM.CNF boot file of the supported disc, nullptr: no disc install
    const char* serial;     // "SCUS-97316"
    const char* discName;   // "Sly 2: Band of Thieves (USA)"
    uint32_t accent;        // page tint, 0xRRGGBB
};
inline const std::array<GameInfo, kGameCount>& games() {
    static const std::array<GameInfo, kGameCount> list{ {
        { GameId::Sly1, "sly1", "Sly 1", "Sly Cooper and the Thievius Raccoonus",
          "Sly's first heist: recover the Thievius Raccoonus from the Fiendish Five.", "Sly1-Release", "SCUS_971.98",
          "SCUS-97198", "Sly Cooper and the Thievius Raccoonus (USA)", 0x2F7FD8 },
        { GameId::Sly2, "sly2", "Sly 2", "Sly 2: Band of Thieves",
          "The gang reunites to steal the scattered parts of Clockwerk.", "Sly2-Release", "SCUS_973.16", "SCUS-97316",
          "Sly 2: Band of Thieves (USA)", 0x3FA06A },
        // Sly 3: the disc installs like Sly 2's; Play needs Sly3-Release beside the launcher (a build with
        // CANE_BUILD_SLY3=ON), else the launcher says the port is still in development.
        { GameId::Sly3, "sly3", "Sly 3", "Sly 3: Honor Among Thieves",
          "Sly assembles a crew to crack the Cooper family vault.", "Sly3-Release", "SCUS_974.64", "SCUS-97464",
          "Sly 3: Honor Among Thieves (USA)", 0xC8792C },
        { GameId::Sly4, "sly4", "Sly 4", "Sly Cooper: Thieves in Time",
          "Sly travels through history to save the Thievius Raccoonus.", nullptr, nullptr, nullptr, nullptr, 0x8A5CD0 },
        { GameId::Hackpack, "hackpack", "Bentley's Hackpack", "Bentley's Hackpack",
          "Bentley's collection of hacking mini-games.", nullptr, nullptr, nullptr, nullptr, 0x2BA7A0 },
    } };
    return list;
}
inline const GameInfo& game(GameId id) { return games()[static_cast<size_t>(id)]; }
inline std::optional<GameId> gameFromKey(std::string_view key) {
    for (const auto& g : games())
        if (key == g.key) return g.id;
    return std::nullopt;
}
// Games that can be installed from a disc image with this launcher.
inline bool installable(GameId id) {
    return game(id).bootFile != nullptr && (id == GameId::Sly1 || id == GameId::Sly2 || id == GameId::Sly3);
}

// ---------------------------------------------------------------- SHA-1
// Used for the Sly 2 / Sly 3 executable fingerprints (the SHA-1 that disc databases
// such as Redump list). Not used for anything security related.
class Sha1 {
public:
    void update(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        m_length += size;
        while (size) {
            const size_t take = std::min(size, sizeof(m_block) - m_used);
            std::memcpy(m_block + m_used, p, take);
            m_used += take, p += take, size -= take;
            if (m_used == sizeof(m_block)) block(), m_used = 0;
        }
    }
    std::string hex() {
        const uint64_t bits = m_length * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (m_used != 56) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(length, 8);
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (uint32_t word : m_h)
            for (int i = 28; i >= 0; i -= 4) out += digits[(word >> i) & 15];
        return out;
    }

private:
    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void block() {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(m_block[i * 4]) << 24 | uint32_t(m_block[i * 4 + 1]) << 16 | uint32_t(m_block[i * 4 + 2]) << 8 |
                   m_block[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        m_h[0] += a, m_h[1] += b, m_h[2] += c, m_h[3] += d, m_h[4] += e;
    }
    uint32_t m_h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t m_block[64]{};
    size_t m_used = 0;
    uint64_t m_length = 0;
};
inline std::string sha1Hex(const void* data, size_t size) {
    Sha1 h;
    h.update(data, size);
    return h.hex();
}

// ---------------------------------------------------------------- ISO9660
inline constexpr uint32_t kIsoSector = 2048;
struct IsoFile {
    std::string name; // without the ";1" version suffix
    uint32_t sector = 0;
    uint32_t size = 0;
};
// The root directory of an ISO9660 image (PS2 discs keep their system files
// there). Empty when the file is not an ISO9660 image.
struct IsoVolume {
    bool valid = false;
    uint32_t blocks = 0; // volume space size, in 2048-byte sectors
    std::vector<IsoFile> root;
    const IsoFile* find(std::string_view name) const {
        for (const auto& f : root)
            if (f.name.size() == name.size() &&
                std::equal(name.begin(), name.end(), f.name.begin(), [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); }))
                return &f;
        return nullptr;
    }
};
inline bool readAt(std::ifstream& in, uint64_t offset, void* data, size_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in) return false;
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return in.gcount() == static_cast<std::streamsize>(size);
}
// Reads `size` bytes at `offset` of a disc image (an .iso, or a CHD: platform/disc_image.h); false when it cannot.
using DiscReader = std::function<bool(uint64_t offset, void* data, size_t size)>;
inline DiscReader discReader(std::ifstream& in) {
    return [&in](uint64_t offset, void* data, size_t size) { return readAt(in, offset, data, size); };
}
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline IsoVolume readIsoVolume(const DiscReader& read) {
    IsoVolume volume;
    std::array<uint8_t, kIsoSector> pvd{};
    if (!read(16ull * kIsoSector, pvd.data(), pvd.size()) || std::memcmp(pvd.data(), "\x01" "CD001" "\x01", 7) != 0)
        return volume;
    volume.blocks = le32(pvd.data() + 80);
    const uint8_t* root = pvd.data() + 156;
    const uint32_t rootSector = le32(root + 2), rootSize = le32(root + 10);
    if (rootSize == 0 || rootSize > 64 * kIsoSector) return volume;
    std::vector<uint8_t> dir(rootSize);
    if (!read(uint64_t(rootSector) * kIsoSector, dir.data(), dir.size())) return volume;
    for (size_t i = 0; i < dir.size();) {
        const uint8_t length = dir[i];
        if (length == 0) {
            i = (i / kIsoSector + 1) * kIsoSector; // records never cross sectors
            continue;
        }
        if (length < 34 || i + length > dir.size()) break;
        const uint8_t* r = dir.data() + i;
        const uint8_t nameLength = r[32];
        if (33u + nameLength <= length && !(r[25] & 2) && nameLength && !(nameLength == 1 && r[33] <= 1)) {
            std::string name(reinterpret_cast<const char*>(r + 33), nameLength);
            if (auto semicolon = name.find(';'); semicolon != std::string::npos) name.resize(semicolon);
            if (!name.empty() && name.back() == '.') name.pop_back();
            volume.root.push_back({ name, le32(r + 2), le32(r + 10) });
        }
        i += length;
    }
    volume.valid = true;
    return volume;
}
inline IsoVolume readIsoVolume(std::ifstream& in) { return readIsoVolume(discReader(in)); }

// ---------------------------------------------------------------- identity
struct DiscIdentity {
    enum class Kind {
        Unreadable,   // missing or cannot be opened
        NotPs2,       // not an ISO9660 image with a PS2 SYSTEM.CNF
        Supported,    // a disc this launcher installs (game is set)
        OtherRegion,  // a known game, but not the supported release (game is set)
        NoPort,       // a known game without a port yet (game is set)
        Unknown,      // some other PS2 disc
    };
    Kind kind = Kind::Unreadable;
    std::optional<GameId> game;
    std::string bootFile; // "SCUS_973.16"
    std::string label;    // "Sly 2: Band of Thieves (USA)"
};
// Known releases. The supported ones match GameInfo::bootFile.
struct KnownDisc {
    const char* bootFile;
    GameId game;
    const char* region;
    bool otherRegionSupported = false;  // installable although it is not GameInfo::bootFile
};
inline constexpr KnownDisc kKnownDiscs[] = {
    { "SCUS_971.98", GameId::Sly1, "USA" },
    // Sly 1's other releases: the extractor rebuilds them into the same PCBRX layout.
    { "SCES_509.17", GameId::Sly1, "Europe", true },
    { "SCPS_150.36", GameId::Sly1, "Japan", true },
    { "SCKA_200.04", GameId::Sly1, "Korea", true },
    { "SCUS_973.16", GameId::Sly2, "USA" },
    { "SCES_525.29", GameId::Sly2, "Europe", true },
    { "SCPS_150.90", GameId::Sly2, "Japan", true },
    { "SCUS_974.64", GameId::Sly3, "USA" },
};
// "BOOT2 = cdrom0:\SCUS_973.16;1" -> "SCUS_973.16".
inline std::string bootFileFromSystemCnf(std::string_view text) {
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find_first_of("\r\n", at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key;
        for (char c : line.substr(0, eq))
            if (!std::isspace(static_cast<unsigned char>(c))) key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (key != "BOOT2") continue;
        std::string_view value = line.substr(eq + 1);
        if (auto colon = value.find(':'); colon != std::string_view::npos) value.remove_prefix(colon + 1);
        while (!value.empty() && (value.front() == '\\' || value.front() == '/' || std::isspace(static_cast<unsigned char>(value.front()))))
            value.remove_prefix(1);
        if (auto semicolon = value.find(';'); semicolon != std::string_view::npos) value = value.substr(0, semicolon);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
        std::string out;
        for (char c : value) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }
    return {};
}
inline DiscIdentity identifyDisc(const DiscReader& read) {
    DiscIdentity id;
    id.kind = DiscIdentity::Kind::NotPs2;
    const IsoVolume volume = readIsoVolume(read);
    const IsoFile* cnf = volume.valid ? volume.find("SYSTEM.CNF") : nullptr;
    if (!cnf || cnf->size == 0 || cnf->size > 4096) return id;
    std::string text(cnf->size, '\0');
    if (!read(uint64_t(cnf->sector) * kIsoSector, text.data(), text.size())) return id;
    id.bootFile = bootFileFromSystemCnf(text);
    if (id.bootFile.empty()) return id;
    id.kind = DiscIdentity::Kind::Unknown;
    for (const auto& known : kKnownDiscs) {
        if (id.bootFile != known.bootFile) continue;
        const GameInfo& g = game(known.game);
        id.game = known.game;
        id.label = std::string(g.title) + " (" + known.region + ")";
        if (!installable(known.game)) id.kind = DiscIdentity::Kind::NoPort;
        else if ((g.bootFile && id.bootFile == g.bootFile) || known.otherRegionSupported)
            id.kind = DiscIdentity::Kind::Supported;
        else id.kind = DiscIdentity::Kind::OtherRegion;
    }
    return id;
}
inline DiscIdentity identifyDisc(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return identifyDisc(discReader(in));
}
// Why a disc image cannot be installed as `wanted`, or "" when it can.
inline std::string discMismatch(const DiscIdentity& id, GameId wanted) {
    const GameInfo& w = game(wanted);
    switch (id.kind) {
    case DiscIdentity::Kind::Unreadable: return "That disc image cannot be opened.";
    case DiscIdentity::Kind::NotPs2: return "That file is not a PlayStation 2 disc image.";
    case DiscIdentity::Kind::Unknown:
        return "That disc image (" + id.bootFile + ") is not " + w.title + ". Choose your " + w.discName + " disc image.";
    case DiscIdentity::Kind::OtherRegion:
        if (id.game == wanted)
            return "That is " + id.label + ". SafeHouse needs the " + w.discName + " release (" + w.serial + ").";
        break;
    case DiscIdentity::Kind::NoPort:
    case DiscIdentity::Kind::Supported: break;
    }
    if (id.game && id.game != wanted)
        return "That disc image is " + id.label + ", not " + w.title + ".";
    if (id.kind == DiscIdentity::Kind::NoPort) return id.label + " does not have a port yet.";
    return {};
}
} // namespace cane
