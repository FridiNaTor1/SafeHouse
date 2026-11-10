#pragma once
#include <array>
#include <cerrno>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <system_error>
#include <vector>

#include "brand.h"
#include "games.h"
#include "picosha2.h"
#include "sha256.h"
#include "platform.h"
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#endif
namespace cane {
using json = nlohmann::json;
// Sly 1 asset layout. 2 = upstream ProjectCane's PCBRX v19 levels (2026-11-03);
// 1 = decompressed retail BRX, which the merged runtime no longer reads, so those installs need re-extracting.
inline constexpr int assetVersion = 2;
// The Sly 1 discs the extractor accepts, identified by the SHA-256 of their boot executable.
struct Sly1Disc {
    const char* bootFile;    // SYSTEM.CNF BOOT2 file in the image root
    const char* serial;      // as the runtime's extraction_manifest.json spells it
    const char* region;      // extraction_manifest.json "region"
    const char* title;
    const char* sha256;      // of the boot executable
    uint32_t executableSize;
    uint32_t volumeBlocks;   // ISO9660 volume size
};
inline constexpr Sly1Disc sly1Discs[] = {
    {"SCUS_971.98", "SCUS-971.98", "NTSC-U", "Sly Cooper and the Thievius Raccoonus",
     "d4ba3ce30047ffb2a825ef13d37a48c4df63aa57977157a194f66bd0b859796c", 1712328, 1368320},
    {"SCES_509.17", "SCES-509.17", "PAL", "Sly Raccoon",
     "cfab20b47a0b9ae1c9eb47d278a48704ca8e272505594c6705de1525a7cccb91", 1751112, 1427648},
    {"SCPS_150.36", "SCPS-150.36", "NTSC-J", "Kaitou Sly Cooper",
     "7cf5e33d304fd9ac4a4f1080d287c96ff90c3f0534923d4a90591cb2f582a910", 1746632, 955152},
    {"SCKA_200.04", "SCKA-200.04", "NTSC-K", "Sly Cooper: Jeonseolui Bibeobseoreul Chajaseo",
     "987bd1b6cda8633b3418ed53192be8132c64013208197e47233df1d6ba25084a", 1747400, 789344},
};
// The USA disc: the only one before asset version 2.
inline constexpr const char* discFingerprint = sly1Discs[0].sha256;
inline const Sly1Disc* sly1DiscFromSha256(const std::string& sha256) {
    for (const auto& disc : sly1Discs)
        if (sha256 == disc.sha256) return &disc;
    return nullptr;
}
inline const Sly1Disc* sly1DiscFromBootFile(const std::string& bootFile) {
    for (const auto& disc : sly1Discs)
        if (bootFile == disc.bootFile) return &disc;
    return nullptr;
}
// Sly 2: Band of Thieves (USA, SCUS-97316). Its game data lives in raw sectors outside the ISO9660 filesystem.
// Asset version 2 (2026-11-03): the disc is read once at install and converted
// into named files with extraction_manifest.json (Sly2-Release --convert); no disc image is kept. 1 = the whole
// disc image (disc.iso): those installs need re-extracting.
// Asset version 3 (2026-11-04, CONVERTER.md "Audio formats"): the audio as WAV (dialog and movie sound FLAC on the
// European discs), the movies as M2V + WAV, the banks' samples as WAV. Version 2 installs (the PS2 formats) still
// play, but the launcher offers the re-extract.
// Asset version 4 (2026-11-10, notes/UNRAVEL_2026-11-10.md): the worlds unravelled into their parts (a
// <World>.world.json index, the text as JSON, the scripts as text, each block a file; Sly2-Release worldparts.h).
// Version 3 installs still play, but the launcher offers the re-extract.
inline constexpr int sly2AssetVersion = 4;
inline constexpr const char* sly2ExecutableSha1 = "de2af92a54de71e33edc8a160c31c0337a95e6f5";
inline constexpr uint32_t sly2VolumeBlocks = 1408272;
inline constexpr const char* sly2ImageName = "disc.iso";
inline constexpr const char* sly2SystemFiles[] = {"SYSTEM.CNF", "SCUS_973.16", "IOPRP300.IMG"};
// Sly 3: Honor Among Thieves (USA, SCUS-97464): installed the same way. Its game data is the ISO9660 file
// SLY3.WAD (LBA 0xCA0), which the game reads by sector through the image.
inline constexpr int sly3AssetVersion = 1;
inline constexpr const char* sly3ExecutableSha1 = "5941b7877e6bf5dfb8acb2d04a3c5059367f385b";
inline constexpr uint32_t sly3VolumeBlocks = 1735232;
inline constexpr const char* sly3SystemFiles[] = {"SYSTEM.CNF", "SCUS_974.64", "IOPRP300.IMG"};
// Sly 2's other releases, installed the same way: the European discs (SCES-52529,
// v1.00 and v2.01, 11 languages) and the Japanese one (SCPS-15090). The game recognizes its disc itself.
inline constexpr const char* sly2PalSystemFiles[] = {"SYSTEM.CNF", "SCES_525.29", "IOPRP300.IMG"};
inline constexpr const char* sly2JapanSystemFiles[] = {"SYSTEM.CNF", "SCPS_150.90", "IOPRP300.IMG"};
// One accepted release of a disc-image game.
struct DiscVariant {
    const char* bootFile;        // "SCUS_973.16"
    const char* serial;          // "SCUS-97316"
    const char* region;          // "USA", "Europe v1.00", ...
    const char* executableSha1;  // SHA-1 of the boot executable
    uint32_t volumeBlocks;       // the image's ISO9660 volume size
    const char* const* systemFiles;
    size_t systemFileCount;
    // What the installation writes when the game converts the disc (Sly 2);
    // 0: the disc image is copied (Sly 3).
    uint64_t installedBytes = 0;
};
// The Sly 2 conversions' sizes (asset version 4, 2026-11-10, CONVERTER.md "Sizes"; the Textures/ folder included):
// USA 4,870,016,654 bytes (41,313 files), Europe v2.01 8,212,190,183 bytes (58,604 files: 11 languages of dialog and
// movie sound, as FLAC), v1.00 8,212,190,604, Japan 4,384,013,926 bytes (41,345 files). (Version 3, the worlds as one
// file each: 4,856,096,772 / 8,193,742,437 / 8,193,742,859 / 4,369,661,290. Version 2, the PS2 formats: 2.76 / 4.25 /
// 2.37 GB.)
inline constexpr DiscVariant sly2Variants[] = {
    {"SCUS_973.16", "SCUS-97316", "USA", sly2ExecutableSha1, sly2VolumeBlocks, sly2SystemFiles, std::size(sly2SystemFiles),
     4870016654ull},
    {"SCES_525.29", "SCES-52529", "Europe v1.00", "329e5df6e6d9524cac8391a900fa96703fc9a8be", 2242304, sly2PalSystemFiles,
     std::size(sly2PalSystemFiles), 8212190604ull},
    {"SCES_525.29", "SCES-52529", "Europe v2.01", "532daf70b535216498b026e57e4a42b9f701698f", 2242304, sly2PalSystemFiles,
     std::size(sly2PalSystemFiles), 8212190183ull},
    {"SCPS_150.90", "SCPS-15090", "Japan", "cabc5d0eedaa892bb4dc70408c9cd1418cbd3c60", 1213424, sly2JapanSystemFiles,
     std::size(sly2JapanSystemFiles), 4384013926ull},
};
inline constexpr DiscVariant sly3Variants[] = {
    {"SCUS_974.64", "SCUS-97464", "USA", sly3ExecutableSha1, sly3VolumeBlocks, sly3SystemFiles, std::size(sly3SystemFiles)},
};
// The free space an install of this release needs (the extractor's check; the launcher shows it): what it writes (the
// converted files, else the image copy) plus 256 MiB. The new files are written into a staging folder beside the
// installation and replace it only when complete (InstallationTransaction), so a re-extract needs this much while the
// old installation is still there.
inline uint64_t installNeedBytes(const DiscVariant& v) {
    constexpr uint64_t sector = 2048;
    return (v.installedBytes ? v.installedBytes : uint64_t(v.volumeBlocks) * sector) + (256ull << 20);
}
// A game installed as its whole disc image (Sly 2, Sly 3): what the extractor checks and the manifest records.
struct DiscImageInstall {
    GameId game;
    int assetVersion;
    const char* executableSha1;  // SHA-1 of the boot executable (GameInfo::bootFile)
    uint32_t volumeBlocks;       // the image's ISO9660 volume size
    const char* const* systemFiles;
    size_t systemFileCount;
    const char* dataFile;        // a file of the filesystem that must be where the game reads it, or nullptr
    uint32_t dataFileSector;
    const char* imageName;
    const DiscVariant* variants;  // every accepted release (the first: the fields above)
    size_t variantCount;
    // The game's own converter (an executable beside cane-extractor, run as `<tool> --disc ISO --convert DIR`) writes
    // the installation instead of a copy of the image (Sly 2); nullptr: the image is kept (Sly 3).
    const char* converterTool = nullptr;
    // the release whose executable has this SHA-1, or nullptr
    const DiscVariant* variantBySha1(const std::string& sha1) const {
        for (size_t i = 0; i < variantCount; ++i)
            if (sha1 == variants[i].executableSha1) return &variants[i];
        return nullptr;
    }
};
inline const DiscImageInstall* discImageInstall(GameId game) {
    static const DiscImageInstall sly2{GameId::Sly2, sly2AssetVersion, sly2ExecutableSha1, sly2VolumeBlocks,
                                       sly2SystemFiles, std::size(sly2SystemFiles), nullptr, 0, sly2ImageName,
                                       sly2Variants, std::size(sly2Variants), "Sly2-Release"};
    static const DiscImageInstall sly3{GameId::Sly3, sly3AssetVersion, sly3ExecutableSha1, sly3VolumeBlocks,
                                       sly3SystemFiles, std::size(sly3SystemFiles), "SLY3.WAD", 0xCA0, sly2ImageName,
                                       sly3Variants, std::size(sly3Variants)};
    return game == GameId::Sly2 ? &sly2 : game == GameId::Sly3 ? &sly3 : nullptr;
}
inline std::string hashFile(const fs::path& file, const std::function<bool()>& cancelled = {},
                            const std::function<void(uint64_t)>& bytesRead = {}) {
    // (sha256.h: the CPU's SHA extensions when it has them, about 12x picosha2; 1 MiB reads)
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read: " + utf8(file));
    Sha256 hash;
    std::vector<char> buffer(size_t(1) << 20);
    while (in) {
        if (cancelled && cancelled()) throw std::runtime_error("Cancelled");
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.update(buffer.data(), static_cast<size_t>(in.gcount()));
        if (bytesRead) bytesRead(static_cast<uint64_t>(in.gcount()));
    }
    if (!in.eof()) throw std::runtime_error("Read failed: " + utf8(file));
    return hash.hex();
}
inline json readJson(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("Cannot open: " + utf8(file));
    json value;
    in >> value;
    return value;
}
inline void writeJson(const fs::path& file, const json& value) {
    fs::create_directories(file.parent_path());
    auto temporary = file;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << value.dump(2) << '\n';
    out.flush();
    if (!out) throw std::runtime_error("Cannot write: " + utf8(file));
    out.close();
    if (!atomicReplace(temporary, file)) throw std::runtime_error("Cannot replace: " + utf8(file));
}
inline uint64_t processId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}
inline bool processAlive(uint64_t pid) {
#ifdef _WIN32
    auto h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    auto alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return pid > 0 && (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
#endif
}
// OS locks are released even when an installer is killed. The empty lock
// file stays in place on POSIX so competing processes always lock one inode.
class FileLock {
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
   public:
    explicit FileLock(const fs::path& file) {
#ifdef _WIN32
        handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error(
                "Another installation is in progress, or its lock cannot be opened");
#else
        handle_ = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (handle_ < 0) throw std::runtime_error("Cannot open installation lock");
        if (flock(handle_, LOCK_EX | LOCK_NB) != 0) {
            close(handle_);
            handle_ = -1;
            throw std::runtime_error("Another installation is in progress");
        }
#endif
    }
    FileLock(const FileLock&) = delete;
    ~FileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) {
            flock(handle_, LOCK_UN);
            close(handle_);
        }
#endif
    }
};
class InstallationTransaction {
    fs::path target_, stage_, previous_, lock_;
    std::unique_ptr<FileLock> guard_;
    bool committed_ = false;

   public:
    explicit InstallationTransaction(fs::path target)
        : target_(fs::absolute(target)), stage_(target_), previous_(target_), lock_(target_) {
        stage_ += ".staging";
        previous_ += ".previous";
        lock_ += ".lock";
        fs::create_directories(target_.parent_path());
        guard_ = std::make_unique<FileLock>(lock_);
        if (!fs::exists(target_) && fs::exists(previous_)) fs::rename(previous_, target_);
        if (fs::exists(target_) && fs::exists(previous_)) fs::remove_all(previous_);
        fs::remove_all(stage_);
        fs::create_directory(stage_);
    }
    InstallationTransaction(const InstallationTransaction&) = delete;
    const fs::path& directory() const { return stage_; }
    void commit() {
        if (fs::exists(target_)) fs::rename(target_, previous_);
        try {
            fs::rename(stage_, target_);
        } catch (...) {
            if (fs::exists(previous_)) fs::rename(previous_, target_);
            throw;
        }
        committed_ = true;
        std::error_code ec;
        fs::remove_all(previous_, ec);
    }
    ~InstallationTransaction() {
        std::error_code ec;
        if (!committed_) fs::remove_all(stage_, ec);
    }
};
inline fs::path safeAssetPath(const fs::path& root, const std::string& name) {
    auto relative = path(name);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("Invalid manifest path");
    for (const auto& part : relative)
        if (part == ".." || part == ".") throw std::runtime_error("Invalid manifest path");
    auto candidate = root;
    for (const auto& part : relative) {
        candidate /= part;
        if (fs::is_symlink(candidate)) throw std::runtime_error("Symlink in installation: " + name);
    }
    return candidate;
}
// The game an installation manifest belongs to. Sly 1 manifests predate the
// "game" field.
inline GameId manifestGame(const json& manifest) {
    if (manifest.is_object() && manifest.contains("game") && manifest.at("game").is_string())
        if (auto id = gameFromKey(manifest.at("game").get<std::string>())) return *id;
    return GameId::Sly1;
}
// True when the manifest was written for `game` by a compatible extractor.
inline bool manifestCurrent(const json& m, GameId game) {
    try {
        if (manifestGame(m) != game || m.at("schema") != 1) return false;
        if (game == GameId::Sly1)
            return m.at("asset_version") == assetVersion &&
                   sly1DiscFromSha256(m.at("disc_sha256").get<std::string>()) != nullptr;
        if (const DiscImageInstall* d = discImageInstall(game))
            return m.at("asset_version") == d->assetVersion && d->variantBySha1(m.at("disc_sha1").get<std::string>());
    } catch (...) {
    }
    return false;
}
// Sly 1 reports progress per file. Sly 2 / Sly 3 are one 2.9 / 3.6 GB image,
// so their progress counts MiB hashed.
inline void verifyInstallation(
    const fs::path& root,
    const std::function<void(size_t, size_t, const std::string&)>& progress = {},
    const std::function<bool()>& cancelled = {}) {
    auto manifest = readJson(root / "installation.json");
    const GameId game = manifestGame(manifest);
    if (game == GameId::Sly1) {
        if (!manifestCurrent(manifest, game))
            throw std::runtime_error("Assets need reinstalling with this version of SafeHouse");
    } else if (!manifestCurrent(manifest, game))
        throw std::runtime_error("Assets need reinstalling with this version of SafeHouse");
    auto& files = manifest.at("files");
    if (!files.is_array() || files.empty())
        throw std::runtime_error("Installation inventory is empty");
    size_t index = 0;
    uint64_t totalBytes = 0, doneBytes = 0;
    for (const auto& entry : files) totalBytes += entry.at("size").get<uint64_t>();
    const size_t totalMiB = static_cast<size_t>((totalBytes + (1u << 20) - 1) >> 20);
    for (const auto& entry : files) {
        if (cancelled && cancelled()) throw std::runtime_error("Cancelled");
        auto name = entry.at("path").get<std::string>();
        auto file = safeAssetPath(root, name);
        std::function<void(uint64_t)> bytes;
        if (game != GameId::Sly1 && progress)
            bytes = [&, last = size_t(0)](uint64_t n) mutable {
                doneBytes += n;
                const size_t mib = static_cast<size_t>(doneBytes >> 20);
                if (mib >= last + 16) progress(last = mib, totalMiB, name);
            };
        if (!fs::is_regular_file(file) || fs::file_size(file) != entry.at("size").get<uint64_t>() ||
            hashFile(file, cancelled, bytes) != entry.at("sha256").get<std::string>())
            throw std::runtime_error("Missing or damaged asset: " + name);
        if (progress) {
            if (game == GameId::Sly1) progress(++index, files.size(), name);
            else progress(static_cast<size_t>(doneBytes >> 20), totalMiB, name);
        }
    }
}
// Sly 1's inventory. Every disc yields 46 PCBRX levels, 7 sound banks and the music groups its levels
// use; the cutscene count differs by release (NTSC-U 18 including the boot splash, PAL / NTSC-J 16).
inline json makeManifest(
    const fs::path& root, const fs::path& iso, const Sly1Disc& disc,
    const std::function<void(size_t, size_t, const std::string&)>& progress = {}) {
    std::vector<fs::path> files;
    for (const char* directory : {"Worlds", "Cutscenes"})
        for (auto& e : fs::recursive_directory_iterator(root / directory))
            if (e.is_regular_file()) files.push_back(e.path());
    files.push_back(root / "extraction_manifest.json");
    std::sort(files.begin(), files.end());
    json manifest = {{"schema", 1},
                     {"asset_version", assetVersion},
                     {"extractor_version", "0.5.0"},
                     {"level_format", "PCBRX"},
                     {"disc", disc.bootFile},
                     {"serial", disc.serial},
                     {"region", disc.region},
                     {"disc_sha256", disc.sha256},
                     {"iso_path", utf8(fs::absolute(iso))},
                     {"files", json::array()}};
    size_t levels = 0, movies = 0, banks = 0, music = 0, index = 0;
    for (auto& file : files) {
        auto name = utf8(fs::relative(file, root));
        manifest["files"].push_back(
            {{"path", name}, {"size", fs::file_size(file)}, {"sha256", hashFile(file)}});
        if (file.extension() == ".brx") ++levels;
        if (file.extension() == ".m2v") {
            ++movies;
            // NTSC-U keeps the sound beside the video; PAL / NTSC-J per language (Cutscenes/<Language>/).
            bool audio = fs::is_regular_file(fs::path(file).replace_extension(".wav"));
            for (auto& sub : fs::directory_iterator(file.parent_path()))
                audio = audio || (sub.is_directory() &&
                                  fs::is_regular_file(sub.path() / fs::path(file.filename()).replace_extension(".wav")));
            if (!audio) throw std::runtime_error("Missing cutscene audio: " + name);
        }
        if (file.extension() == ".bnk") ++banks;
        if (file.extension() == ".mus") ++music;
        if (progress) progress(++index, files.size(), name);
    }
    const bool usa = std::string_view(disc.region) == "NTSC-U";
    if (levels != 46 || banks != 7 || movies < (usa ? 18u : 16u) || music < (usa ? 38u : 1u))
        throw std::runtime_error("Incomplete extraction: found " + std::to_string(levels) + " levels, " +
                                 std::to_string(movies) + " cutscenes, " + std::to_string(banks) +
                                 " sound banks and " + std::to_string(music) + " music groups");
    manifest["inventory"] = {
        {"levels", levels}, {"cutscenes", movies}, {"sound_banks", banks}, {"music_groups", music}};
    return manifest;
}
struct UserPaths {
    fs::path data, user, config, logs;
    bool portable = false;
};
inline fs::path environmentDirectory(const char* key, const fs::path& fallback) {
#ifdef _WIN32
    auto wideKey = path(key).wstring();
    auto value = _wgetenv(wideKey.c_str());
    return value && *value ? fs::path(value) : fallback;
#else
    auto value = std::getenv(key);
    return value && *value ? path(value) : fallback;
#endif
}
// The profile folders (brand::userFolder). Before 2026-11-10 they had the old product name (brand::legacyUserFolder);
// the launcher moves them once at its start (platform/user_folders.h migrateLegacyFolders()), before it uses these.
inline UserPaths userPaths(const fs::path& executableDir = executableDirectory()) {
    for (const auto& root : {executableDir.parent_path(), executableDir}) {
        if (fs::is_regular_file(root / "portable.txt")) {
            auto base = root / "data";
            return {base / "installations" / "sly1", base / "user", base / "config", base / "logs",
                    true};
        }
    }
#ifdef _WIN32
    auto base = environmentDirectory("LOCALAPPDATA", userDirectory()) / brand::userFolder;
    return {base / "installations" / "sly1", base / "user", base / "config", base / "logs"};
#elif defined(__APPLE__)
    auto base = environmentDirectory("HOME", userDirectory()) / "Library/Application Support" / brand::userFolder;
    return {base / "installations" / "sly1", base / "user", base / "config", base / "logs"};
#else
    auto home = environmentDirectory("HOME", userDirectory());
    auto base = environmentDirectory("XDG_DATA_HOME", home / ".local/share") / brand::userFolder;
    auto config = environmentDirectory("XDG_CONFIG_HOME", home / ".config") / brand::userFolder;
    return {base / "installations" / "sly1", config, config, config / "logs"};
#endif
}
// installations/<game>, beside Sly 1's installations/sly1.
inline fs::path gameDataDirectory(const UserPaths& locations, GameId game) {
    return game == GameId::Sly1 ? locations.data : locations.data.parent_path() / cane::game(game).key;
}
// Sly 1 keeps the profile root (Saves, Debug Mode.ini); later games get a
// subfolder of it so their saves and settings never mix.
inline fs::path gameUserDirectory(const UserPaths& locations, GameId game) {
    return game == GameId::Sly1 ? locations.user : locations.user / cane::game(game).key;
}
// Large source builds and runtime artifacts remain alongside the game data.
inline fs::path runtimeStorage(const UserPaths& locations) {
    return locations.data.parent_path().parent_path() / "user";
}
inline fs::path legacyLogDirectory(const UserPaths& locations) {
    if (locations.portable) return locations.logs;
#if defined(_WIN32) || defined(__APPLE__)
    return locations.logs;
#else
    auto home = environmentDirectory("HOME", userDirectory());
    return environmentDirectory("XDG_STATE_HOME", home / ".local/state") / brand::userFolder / "logs";
#endif
}
// Stage imports before activation, leaving the original and any newer profile intact.
inline bool importDirectoryIfEmpty(const fs::path& source, const fs::path& destination) {
    if (source == destination || !fs::is_directory(source) ||
        (fs::exists(destination) && !fs::is_empty(destination)))
        return false;
    InstallationTransaction transaction(destination);
    if (fs::exists(destination) && !fs::is_empty(destination)) return false;
    fs::copy(source, transaction.directory(), fs::copy_options::recursive);
    transaction.commit();
    return true;
}
}  // namespace cane
