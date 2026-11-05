#pragma once
// Regions (releases) of the installable games, as the launcher shows them, and the check of a chosen disc image.
// Everything here is derived from the shared tables: platform/install.h (sly1Discs, the DiscImageInstall variants:
// the releases the extractor installs) and platform/games.h (kKnownDiscs: every release SafeHouse recognizes). A
// release that games.h knows but install.h does not list (Sly 2 Japan, 2026-11-03) is "recognized, not supported
// yet"; it becomes installable as soon as its DiscVariant is added there, with no launcher change.
//
// One region = one installation folder (backend.h): Europe v1.00 and v2.01 of Sly 2 are one region, "europe".
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "install.h"

namespace cane {
struct RegionInfo {
    GameId game = GameId::Sly1;
    std::string key;      // "usa", "europe", "japan", "korea": the folder suffix and the launcher.json value
    std::string name;     // "USA", "Europe"
    std::string serial;   // "SCES-50917"
    std::vector<std::string> bootFiles;  // the releases' SYSTEM.CNF boot files
    std::vector<std::string> releases;   // "Europe v1.00", "Europe v2.01" (one entry when the region has one release)
    bool supported = false;              // the extractor installs it
    uint64_t needBytes = 0;              // free space an installation needs (the extractor's own check)
};

// "NTSC-U" (Sly 1's manifests) / "USA" / "Europe v2.01" -> "usa" / "europe".
inline std::string regionKey(std::string_view region) {
    if (region == "NTSC-U") return "usa";
    if (region == "PAL") return "europe";
    if (region == "NTSC-J") return "japan";
    if (region == "NTSC-K") return "korea";
    std::string key;
    for (char c : region) {
        if (std::isspace(static_cast<unsigned char>(c))) break;
        key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}
inline std::string regionName(std::string_view key) {
    if (key == "usa") return "USA";
    if (key.empty()) return "Unknown region";
    std::string name(key);
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}
// The free space an install needs: Sly 1 rebuilds its levels (decoded textures; PAL keeps five languages), the
// numbers the extractor checks; Sly 2 converts its disc into files and Sly 3 copies the image: install.h
// installNeedBytes, the extractor's own check.
inline uint64_t sly1NeedBytes(const Sly1Disc& d) {
    const std::string_view r = d.region;
    return (r == "PAL" ? 12ull : r == "NTSC-U" ? 6ull : 8ull) << 30;
}
inline uint64_t discImageNeedBytes(const DiscVariant& v) { return installNeedBytes(v); }

// The game's regions: the installable ones first (in the tables' order, USA first), then the recognized ones.
inline std::vector<RegionInfo> knownRegions(GameId game) {
    std::vector<RegionInfo> out;
    auto entry = [&](const std::string& key) -> RegionInfo& {
        for (auto& r : out)
            if (r.key == key) return r;
        RegionInfo r;
        r.game = game;
        r.key = key;
        r.name = regionName(key);
        out.push_back(r);
        return out.back();
    };
    if (game == GameId::Sly1) {
        for (const auto& d : sly1Discs) {
            auto& r = entry(regionKey(d.region));
            r.serial = d.serial;
            r.bootFiles.push_back(d.bootFile);
            r.releases.push_back(r.name);
            r.supported = true;
            r.needBytes = std::max(r.needBytes, sly1NeedBytes(d));
        }
    } else if (const DiscImageInstall* di = discImageInstall(game)) {
        for (size_t i = 0; i < di->variantCount; ++i) {
            const DiscVariant& v = di->variants[i];
            auto& r = entry(regionKey(v.region));
            r.serial = v.serial;
            if (std::find(r.bootFiles.begin(), r.bootFiles.end(), v.bootFile) == r.bootFiles.end()) r.bootFiles.push_back(v.bootFile);
            r.releases.push_back(v.region);
            r.supported = true;
            r.needBytes = std::max(r.needBytes, discImageNeedBytes(v));
        }
    }
    for (const auto& k : kKnownDiscs) {
        if (k.game != game) continue;
        const bool covered = std::any_of(out.begin(), out.end(), [&](const RegionInfo& r) {
            return std::find(r.bootFiles.begin(), r.bootFiles.end(), k.bootFile) != r.bootFiles.end();
        });
        if (covered) continue;
        auto& r = entry(regionKey(k.region));
        r.bootFiles.push_back(k.bootFile);
        r.releases.push_back(k.region);
    }
    return out;
}
inline std::optional<RegionInfo> regionInfo(GameId game, std::string_view key) {
    for (auto& r : knownRegions(game))
        if (r.key == key) return r;
    return std::nullopt;
}
inline std::optional<RegionInfo> regionByBootFile(GameId game, std::string_view bootFile) {
    for (auto& r : knownRegions(game))
        if (std::find(r.bootFiles.begin(), r.bootFiles.end(), bootFile) != r.bootFiles.end()) return r;
    return std::nullopt;
}
// "about 12 GB" (rounded up to whole GB), "about 2.9 GB" (under 10 GB: rounded up to a tenth).
inline std::string gbRoundedUp(uint64_t bytes) {
    if (bytes >= (10ull << 30)) return std::to_string((bytes + (1ull << 30) - 1) >> 30);
    const uint64_t tenths = (bytes * 10 + (1ull << 30) - 1) >> 30;
    return tenths % 10 ? std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) : std::to_string(tenths / 10);
}
inline std::string aboutGb(uint64_t bytes) { return "about " + gbRoundedUp(bytes) + " GB"; }
// "USA, Europe, Japan or Korea": the regions a game installs from.
inline std::string supportedRegionList(GameId game) {
    std::vector<std::string> names;
    for (const auto& r : knownRegions(game))
        if (r.supported) names.push_back(r.name);
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) out += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + names[i];
    return out;
}
// "6 GB (USA) to 12 GB (Europe)" or "about 3 GB".
inline std::string spaceRange(GameId game) {
    const RegionInfo *lo = nullptr, *hi = nullptr;
    const auto regions = knownRegions(game);
    for (const auto& r : regions) {
        if (!r.supported) continue;
        if (!lo || r.needBytes < lo->needBytes) lo = &r;
        if (!hi || r.needBytes > hi->needBytes) hi = &r;
    }
    if (!lo) return {};
    const auto gb = [](uint64_t b) { return gbRoundedUp(b); };
    if (gb(lo->needBytes) == gb(hi->needBytes)) return aboutGb(lo->needBytes);
    return "about " + gb(lo->needBytes) + " GB (" + lo->name + ") to " + gb(hi->needBytes) + " GB (" + hi->name + ")";
}

// ---------------------------------------------------------------- a disc image's check
// What a chosen disc image is, and whether it installs: the game, the region and release, and the image's
// integrity as far as it can be checked at once (the volume is complete, the boot executable's checksum is a known
// release's). The extractor checks the same again and hashes what it copies.
struct DiscCheck {
    enum class Status {
        Ok,
        Unreadable,       // cannot be opened
        NotPs2,           // not a PS2 disc image
        NotSly,           // a PS2 disc of another game
        WrongGame,        // a Sly disc, but not the wanted game
        NoPort,           // a Sly game without a port
        NotSupportedYet,  // a recognized release the extractor does not install yet (Sly 2 Japan)
        Partial,          // the image is shorter than its volume (an incomplete copy or download)
        BadChecksum,      // the boot executable is not a known release's (damaged or modified)
    };
    Status status = Status::Unreadable;
    DiscIdentity identity;
    std::optional<GameId> game;
    std::optional<RegionInfo> region;
    std::string release;  // "Europe v2.01"
    std::string message;  // "" when Ok
    bool ok() const { return status == Status::Ok; }
    // "Sly 2: Band of Thieves (Europe v2.01)"
    std::string label() const {
        if (!game) return identity.label;
        return std::string(cane::game(*game).title) + " (" + (release.empty() ? region ? region->name : std::string("?") : release) + ")";
    }
};
inline std::string gbText(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(1ull << 30));
    return buf;
}
// The image's own checks, for a disc of `game` whose boot file is in `region`.
inline void checkDiscContents(const fs::path& file, DiscCheck& c) {
    const GameId game = *c.game;
    std::ifstream in(file, std::ios::binary);
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    const IsoVolume volume = readIsoVolume(in);
    const std::string title = cane::game(game).shortName;
    if (!in || ec || !volume.valid) {
        c.status = DiscCheck::Status::Unreadable;
        c.message = "That disc image cannot be read.";
        return;
    }
    const uint64_t volumeBytes = uint64_t(volume.blocks) * kIsoSector;
    if (size < volumeBytes) {
        c.status = DiscCheck::Status::Partial;
        c.message = "That disc image is incomplete: it holds " + gbText(size) + " of the disc's " + gbText(volumeBytes) +
                    ". The copy or download did not finish. Make a new copy of your disc and try again.";
        return;
    }
    const IsoFile* exe = volume.find(c.identity.bootFile);
    std::vector<uint8_t> program;
    if (exe && exe->size && exe->size <= (64u << 20)) {
        program.resize(exe->size);
        if (!readAt(in, uint64_t(exe->sector) * kIsoSector, program.data(), program.size())) program.clear();
    }
    const std::string bad = "The game program on that disc image (" + c.identity.bootFile + ") does not match any known " + title + " " +
                            c.region->name + " disc: the image is damaged or modified (wrong checksum). Make a new copy of your disc and try again.";
    if (game == GameId::Sly1) {
        const Sly1Disc* d = sly1DiscFromBootFile(c.identity.bootFile);
        if (!d || program.size() != d->executableSize || volume.blocks != d->volumeBlocks ||
            picosha2::hash256_hex_string(program) != d->sha256) {
            c.status = DiscCheck::Status::BadChecksum;
            c.message = bad;
            return;
        }
        c.release = c.region->name;
    } else if (const DiscImageInstall* di = discImageInstall(game)) {
        const std::string sha1 = program.empty() ? std::string{} : sha1Hex(program.data(), program.size());
        const DiscVariant* found = nullptr;
        for (size_t i = 0; i < di->variantCount && !found; ++i)
            if (c.identity.bootFile == di->variants[i].bootFile && sha1 == di->variants[i].executableSha1 &&
                volume.blocks == di->variants[i].volumeBlocks)
                found = &di->variants[i];
        if (!found) {
            c.status = DiscCheck::Status::BadChecksum;
            c.message = bad;
            return;
        }
        c.release = found->region;
    }
    c.status = DiscCheck::Status::Ok;
}
// `wanted`: the game the image is chosen for (nullopt: any installable game).
inline DiscCheck checkDisc(const fs::path& file, std::optional<GameId> wanted = std::nullopt) {
    DiscCheck c;
    c.identity = identifyDisc(file);
    c.game = c.identity.game;
    const std::string want = wanted ? std::string(cane::game(*wanted).title) : std::string("a Sly Cooper game");
    switch (c.identity.kind) {
    case DiscIdentity::Kind::Unreadable:
        c.status = DiscCheck::Status::Unreadable;
        c.message = "That disc image cannot be opened.";
        return c;
    case DiscIdentity::Kind::NotPs2:
        c.status = DiscCheck::Status::NotPs2;
        c.message = "That file is not a PlayStation 2 disc image (or it is damaged). Choose the .iso of your " + want + " disc.";
        return c;
    case DiscIdentity::Kind::Unknown:
        c.status = DiscCheck::Status::NotSly;
        c.message = "That is another PlayStation 2 game (" + c.identity.bootFile + "), not " + want + ".";
        return c;
    default: break;
    }
    if (c.game) c.region = regionByBootFile(*c.game, c.identity.bootFile);
    if (wanted && c.game != wanted) {
        c.status = DiscCheck::Status::WrongGame;
        c.message = "That disc image is " + c.identity.label + ", not " + want + ".";
        return c;
    }
    if (!c.game || !installable(*c.game)) {
        c.status = DiscCheck::Status::NoPort;
        c.message = c.identity.label + " does not have a port yet.";
        return c;
    }
    if (!c.region || !c.region->supported) {
        c.status = DiscCheck::Status::NotSupportedYet;
        c.message = "That is " + c.identity.label + ". SafeHouse recognizes this release, but it is not supported yet. " +
                    cane::game(*c.game).shortName + " installs from a " + supportedRegionList(*c.game) + " disc.";
        return c;
    }
    checkDiscContents(file, c);
    return c;
}
}  // namespace cane
