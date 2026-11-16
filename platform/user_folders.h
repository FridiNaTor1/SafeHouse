#pragma once
// The one-time move of the profile folders from their old name to SafeHouse (brand::legacyUserFolder ->
// brand::userFolder, 2026-11-10), run by the launcher at its start before it opens anything in them
// (launcher/backend.h LauncherBackend()).
//
//   Linux:   $XDG_DATA_HOME/projectcane   -> $XDG_DATA_HOME/SafeHouse    (installations, runtime builds)
//            $XDG_CONFIG_HOME/projectcane -> $XDG_CONFIG_HOME/SafeHouse  (launcher.json, saves, settings, logs, mods)
//            $XDG_STATE_HOME/projectcane  -> $XDG_STATE_HOME/SafeHouse   (the oldest launchers' logs)
//            ($XDG_CACHE_HOME is left alone: the launcher never stored anything there that needs keeping, and a
//             "projectcane" cache folder may belong to another tool.)
//   Windows: %LOCALAPPDATA%\ProjectCane   -> %LOCALAPPDATA%\SafeHouse
//   macOS and portable copies: nothing (macOS always used SafeHouse; portable keeps data/ beside the program).
//
// Safety rules (nothing is ever deleted before it exists in the new place):
//   - The new folder missing: one rename of the whole folder (instant, atomic: the folder is in one place or the
//     other, never half-moved).
//   - The new folder already there (an interrupted move, an update helper that wrote its result first, a test run):
//     the old folder is merged in entry by entry. A folder or file the new one lacks is renamed into it; when a
//     rename is impossible (another drive, a file in use) it is copied to "<name>.safehouse-upgrade", compared byte
//     for byte, renamed into place, and only then is the original removed. A file both have with the same content
//     is removed from the old folder. A file both have with different content is kept: the new folder's version stays
//     and the old one moves to "<new folder>/ProjectCane leftovers/<its path>".
//   - Anything that cannot be moved this time (a file another program holds open) stays in the old folder, which is
//     kept, and the next start tries again. Interrupting the move at any point leaves every file complete in one of
//     the two folders; the next start continues.
//   - Stored absolute paths that point into an old folder are rewritten to the new folder (launcher.json: the custom
//     runtime, disc images and browse folders, install locations; installation.json / pack.json / update-result.json),
//     before the move in the old folder and again after it in the new one, so an interrupted run is fixed by the next.
// Saves and game settings hold no paths (binary .dat, Debug Mode.ini, advanced_controls.json). Steam and desktop
// shortcuts run the launcher program, which never lives in these folders (setup: %LOCALAPPDATA%\Programs\SafeHouse),
// so they need no change.
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include "install.h"

namespace cane::folders {
struct Move {
    fs::path from, to;
};
// The old -> new folder pairs of this computer (from the environment, existing or not).
inline std::vector<Move> legacyMoves(const fs::path& executableDir = executableDirectory()) {
    if (userPaths(executableDir).portable) return {};
#ifdef _WIN32
    const auto base = environmentDirectory("LOCALAPPDATA", userDirectory());
    return {{base / brand::legacyUserFolder, base / brand::userFolder}};
#elif defined(__APPLE__)
    return {};
#else
    const auto home = environmentDirectory("HOME", userDirectory());
    std::vector<Move> moves;
    for (const auto& [key, fallback] : {std::pair<const char*, fs::path>{"XDG_DATA_HOME", home / ".local/share"},
                                        {"XDG_CONFIG_HOME", home / ".config"},
                                        {"XDG_STATE_HOME", home / ".local/state"}}) {
        const auto base = environmentDirectory(key, fallback);
        moves.push_back({base / brand::legacyUserFolder, base / brand::userFolder});
    }
    return moves;
#endif
}
// The folder that receives files the new folder already has in another version.
inline constexpr const char* leftoversFolder = "ProjectCane leftovers";
inline constexpr const char* copySuffix = ".safehouse-upgrade";

struct Report {
    size_t renamed = 0;     // folders / files moved by rename
    size_t copied = 0;      // files copied, compared and then removed from the old folder
    size_t duplicates = 0;  // identical files already in the new folder (the old copy removed)
    size_t leftovers = 0;   // different versions moved to "ProjectCane leftovers"
    size_t failed = 0;      // left in the old folder this time
    size_t rewritten = 0;   // files whose stored paths were rewritten
    std::vector<std::string> lines;
    std::vector<Move> moved;  // the moves that found an old folder
    bool any() const { return !moved.empty(); }
};

namespace detail {
// Test hook (scripts/test-folder-upgrade.py): no renames, as if every file were on another drive. Never documented.
inline bool renameAllowed() {
    const char* v = std::getenv("SAFEHOUSE_TEST_UPGRADE_COPY");
    return !(v && *v == '1');
}
inline bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}
inline bool realDirectory(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec));
}
inline bool sameContent(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const auto sa = fs::symlink_status(a, ec), sb = fs::symlink_status(b, ec);
    if (fs::is_symlink(sa) || fs::is_symlink(sb))
        return fs::is_symlink(sa) && fs::is_symlink(sb) && fs::read_symlink(a, ec) == fs::read_symlink(b, ec);
    if (!fs::is_regular_file(sa) || !fs::is_regular_file(sb)) return false;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (fa && fb) {
        fa.read(ba.data(), std::streamsize(ba.size()));
        fb.read(bb.data(), std::streamsize(bb.size()));
        if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), size_t(fa.gcount())) != 0) return false;
    }
    return fa.eof() && fb.eof();
}
// Moves one file (or symlink) to a destination that does not exist: rename, else copy + compare + remove.
inline bool moveFile(const fs::path& from, const fs::path& to, Report& report) {
    std::error_code ec;
    auto temporary = to;
    temporary += copySuffix;
    fs::remove(temporary, ec);
    fs::create_directories(to.parent_path(), ec);
    if (renameAllowed()) {
        fs::rename(from, to, ec);
        if (!ec) {
            ++report.renamed;
            return true;
        }
    }
    ec.clear();
    if (fs::is_symlink(fs::symlink_status(from, ec))) fs::copy_symlink(from, temporary, ec);
    else fs::copy_file(from, temporary, fs::copy_options::overwrite_existing, ec);
    if (!ec && sameContent(from, temporary)) {
        fs::rename(temporary, to, ec);
        if (!ec) {
            ++report.copied;
            fs::remove(from, ec);
            if (ec) report.lines.push_back("copied, but the original could not be removed (yet): " + utf8(from));
            return true;
        }
    }
    std::error_code ignore;
    fs::remove(temporary, ignore);
    ++report.failed;
    report.lines.push_back("not moved this time: " + utf8(from) + (ec ? " (" + ec.message() + ")" : std::string{}));
    return false;
}
// A free name in the leftovers folder: the path itself, else "name (2)", "name (3)", ...
inline fs::path freeName(const fs::path& p) {
    if (!present(p)) return p;
    for (int i = 2;; ++i) {
        auto candidate = p.parent_path() / (utf8(p.stem()) + " (" + std::to_string(i) + ")" + utf8(p.extension()));
        if (!present(candidate)) return path(utf8(candidate));
    }
}
inline void mergeTree(const fs::path& from, const fs::path& to, const fs::path& leftovers, const fs::path& relative,
                      Report& report) {
    std::error_code ec;
    if (!present(to) && renameAllowed()) {
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (!ec) {
            ++report.renamed;
            return;
        }
        ec.clear();
    }
    if (!present(to)) {
        fs::create_directories(to, ec);
        if (!fs::is_directory(to, ec)) {
            ++report.failed;
            report.lines.push_back("cannot create " + utf8(to));
            return;
        }
    }
    if (!fs::is_directory(to, ec)) {
        // a file where the old folder has a folder: the old folder goes to the leftovers as a whole
        if (relative.empty()) {
            ++report.failed;
            report.lines.push_back("not moved: " + utf8(to) + " is a file");
            return;
        }
        mergeTree(from, freeName(leftovers / relative), leftovers, relative, report);
        return;
    }
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    if (ec) {
        ++report.failed;
        report.lines.push_back("cannot read " + utf8(from) + " (" + ec.message() + ")");
        return;
    }
    for (const auto& source : entries) {
        const auto name = source.filename();
        const auto target = to / name;
        {  // a copy an interrupted run left behind
            auto partial = target;
            partial += copySuffix;
            if (present(partial)) fs::remove(partial, ec);
        }
        if (realDirectory(source)) {
            mergeTree(source, target, leftovers, relative / name, report);
        } else if (!present(target)) {
            moveFile(source, target, report);
        } else if (sameContent(source, target)) {
            fs::remove(source, ec);
            if (!ec) ++report.duplicates;
            else ++report.failed, report.lines.push_back("cannot remove " + utf8(source) + " (" + ec.message() + ")");
        } else {
            const auto keep = freeName(leftovers / relative / name);
            if (moveFile(source, keep, report)) {
                ++report.leftovers;
                report.lines.push_back("kept the newer " + utf8(target) + "; the old version is " + utf8(keep));
            }
        }
    }
    fs::remove(from, ec);  // only when empty (fails otherwise: the rest stays for the next start)
}
// Path text with '/' separators (and lower case on Windows) for prefix comparisons.
inline std::string comparable(const fs::path& p) {
    std::string s = utf8(p.lexically_normal());
    for (auto& c : s) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
#endif
    }
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
// value inside (or equal to) from -> the same place inside to; else nullopt.
inline std::optional<std::string> movedPath(const std::string& value, const Move& move) {
    if (value.empty()) return std::nullopt;
    const fs::path p = path(value);
    if (!p.is_absolute()) return std::nullopt;
    const std::string v = comparable(p), f = comparable(move.from);
    if (v.size() < f.size() || v.compare(0, f.size(), f) != 0 || (v.size() > f.size() && v[f.size()] != '/')) return std::nullopt;
    // (comparable() maps characters one to one, so the prefix has the same length in the path's own text)
    std::string t = utf8(p.lexically_normal()).substr(f.size());
    while (!t.empty() && (t.front() == '/' || t.front() == '\\')) t.erase(t.begin());
    return utf8(t.empty() ? move.to : move.to / path(t));
}
inline bool rewriteValue(json& value, const std::vector<Move>& moves) {
    bool changed = false;
    if (value.is_string()) {
        for (const auto& m : moves)
            if (auto moved = movedPath(value.get<std::string>(), m)) {
                value = *moved;
                return true;
            }
    } else if (value.is_object() || value.is_array()) {
        for (auto& item : value) changed = rewriteValue(item, moves) || changed;
    }
    return changed;
}
inline bool mentionsLegacy(const std::string& text) {
    std::string lower = text;
    for (auto& c : lower)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::string legacy = brand::legacyUserFolder;
    for (auto& c : legacy)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return lower.find(legacy) != std::string::npos;
}
// Rewrites one JSON file's stored paths (only read in full when its text names the old folder).
inline bool rewriteFile(const fs::path& file, const std::vector<Move>& moves, Report& report) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return false;
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (!mentionsLegacy(text)) return false;
    try {
        json value = json::parse(text);
        if (!rewriteValue(value, moves)) return false;
        writeJson(file, value);
        ++report.rewritten;
        report.lines.push_back("updated the paths stored in " + utf8(file));
        return true;
    } catch (const std::exception& e) {
        report.lines.push_back("cannot update " + utf8(file) + " (" + e.what() + ")");
        return false;
    }
}
// The files that store absolute paths, inside one profile folder (old or new, any platform's layout).
inline std::vector<fs::path> pathFiles(const fs::path& root) {
    std::vector<fs::path> files;
    std::error_code ec;
    const auto subfolders = [&](const fs::path& dir) {
        std::vector<fs::path> out{dir};
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (realDirectory(it->path())) out.push_back(it->path());
        ec.clear();
        return out;
    };
    for (const auto& dir : {root, root / "config"})
        for (const char* name : {"launcher.json", "update-result.json"}) files.push_back(dir / name);
    for (const auto& dir : subfolders(root / "installations")) files.push_back(dir / "installation.json");
    for (const auto& base : {root, root / "user"})
        for (const auto& dir : subfolders(base)) files.push_back(dir / "PS3 HD" / "pack.json");
    return files;
}
inline void rewriteStoredPaths(const fs::path& root, const std::vector<Move>& moves, Report& report) {
    if (!realDirectory(root)) return;
    for (const auto& file : pathFiles(root)) rewriteFile(file, moves, report);
}
}  // namespace detail

// Rewrites old-folder paths in one JSON file (launcher.json after the move; tests).
inline bool rewriteStoredPaths(const fs::path& file, const std::vector<Move>& moves) {
    Report ignore;
    return detail::rewriteFile(file, moves, ignore);
}
// Moves every old folder that exists to its new name (see the top of this file). Waits for another launcher that is
// doing the same (up to two minutes, then leaves it to that one).
inline Report migrateLegacyFolders(const std::vector<Move>& all = legacyMoves()) {
    Report report;
    std::vector<Move> moves;
    for (const auto& m : all)
        if (detail::present(m.from) && !m.from.empty() && m.from != m.to) moves.push_back(m);
    if (moves.empty()) return report;
    auto lockFile = moves.front().to;
    lockFile += ".upgrade.lock";
    std::unique_ptr<FileLock> lock;
    for (int i = 0; !lock; ++i) {
        try {
            std::error_code ec;
            fs::create_directories(lockFile.parent_path(), ec);
            lock = std::make_unique<FileLock>(lockFile);
        } catch (...) {
            if (i >= 480) {
                report.lines.push_back("another SafeHouse is moving the folders; skipped");
                return report;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    // (another launcher may have finished while this one waited)
    std::erase_if(moves, [](const Move& m) { return !detail::present(m.from); });
    report.moved = moves;
    for (const auto& m : moves) detail::rewriteStoredPaths(m.from, all, report);
    for (const auto& m : moves) {
        report.lines.push_back("moving " + utf8(m.from) + " to " + utf8(m.to));
        if (fs::is_symlink(fs::symlink_status(m.from))) {
            // a link to a folder elsewhere (another drive): the link itself takes the new name
            std::error_code ec;
            if (!detail::present(m.to) && detail::renameAllowed()) {
                fs::rename(m.from, m.to, ec);
                if (!ec) {
                    ++report.renamed;
                    continue;
                }
            }
        }
        detail::mergeTree(m.from, m.to, m.to / leftoversFolder, {}, report);
    }
    for (const auto& m : moves) detail::rewriteStoredPaths(m.to, all, report);
    lock.reset();
    std::error_code ec;
    if (std::none_of(moves.begin(), moves.end(), [](const Move& m) { return detail::present(m.from); })) fs::remove(lockFile, ec);
    return report;
}
}  // namespace cane::folders
