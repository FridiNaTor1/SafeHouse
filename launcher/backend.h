#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <ctime>
#include <deque>
#include <mutex>

#include "install.h"
#include "user_folders.h"
#include "process.h"
#include "regions.h"
namespace cane {
// One installed region of a game (regions.h). Storage: the first region installed keeps the game's original folder
// installations/<game> (so the installs made before regions, and older launchers, keep working, read in place), each
// further region gets installations/<game>-<region> beside it. The region is read from the folder's
// installation.json, never from the folder name. Nothing is ever moved or renamed.
struct Installation {
    GameId game = GameId::Sly1;
    std::string region;   // "usa", "europe", ... ("" when the manifest cannot be read)
    std::string release;  // "Europe v2.01"
    fs::path dir;
    bool current = false;   // installed with this launcher's asset format: playable
    bool outdated = false;  // a valid installation in an older asset format: an update changed it, re-extract
    int assetVersion = 0, wantedVersion = 0;
    std::string iso;        // the disc image it was installed from (its manifest)
    std::string name() const { return release.empty() ? regionName(region) : release; }
};
// The asset version this launcher installs for a game.
inline int currentAssetVersion(GameId game) {
    if (game == GameId::Sly1) return assetVersion;
    if (const DiscImageInstall* d = discImageInstall(game)) return d->assetVersion;
    return 0;
}
// The region a manifest was installed from (old manifests have no "region": Sly 1 asset version 1 and the first
// Sly 2 / Sly 3 installs were USA only).
inline std::pair<std::string, std::string> manifestRegion(const json& m, GameId game) {
    auto text = [&](const char* key) { return m.contains(key) && m.at(key).is_string() ? m.at(key).get<std::string>() : std::string{}; };
    if (game == GameId::Sly1) {
        if (const Sly1Disc* d = sly1DiscFromSha256(text("disc_sha256"))) return {regionKey(d->region), regionName(regionKey(d->region))};
        if (const Sly1Disc* d = sly1DiscFromBootFile(text("disc"))) return {regionKey(d->region), regionName(regionKey(d->region))};
    } else if (const DiscImageInstall* di = discImageInstall(game)) {
        if (const DiscVariant* v = di->variantBySha1(text("disc_sha1"))) return {regionKey(v->region), v->region};
    }
    if (!text("region").empty()) {
        const auto key = regionKey(text("region"));
        return {key, text("region") == "NTSC-U" || text("region") == "PAL" || text("region").rfind("NTSC", 0) == 0 ? regionName(key) : text("region")};
    }
    if (auto r = regionByBootFile(game, text("disc"))) return {r->key, r->name};
    return {"usa", "USA"};
}
inline Installation readInstallation(const fs::path& dir, GameId game) {
    Installation in;
    in.game = game;
    in.dir = dir;
    in.wantedVersion = currentAssetVersion(game);
    try {
        const json m = readJson(dir / "installation.json");
        if (manifestGame(m) != game) return in;
        std::tie(in.region, in.release) = manifestRegion(m, game);
        in.current = manifestCurrent(m, game);
        if (m.contains("asset_version") && m.at("asset_version").is_number_integer()) in.assetVersion = m.at("asset_version").get<int>();
        in.outdated = !in.current && in.assetVersion > 0 && in.assetVersion < in.wantedVersion;
        if (m.contains("iso_path") && m.at("iso_path").is_string()) in.iso = m.at("iso_path").get<std::string>();
    } catch (...) {
    }
    return in;
}
inline std::string executableName(const std::string& name) {
#if defined(CANE_ANDROID)
    return "lib" + name + ".so";  // (the app's JNI libraries; platform/process.h starts them in their own process)
#elif defined(CANE_IOS)
    // (the app's frameworks, relative to SafeHouse.app; platform/process.h runs them in the app, ios/ios_app.h)
    const std::string full = cane::ios::programPath(name), bundle = cane::ios::bundleDirectory() + "/";
    return full.compare(0, bundle.size(), bundle) == 0 ? full.substr(bundle.size()) : full;
#elif defined(_WIN32)
    return name + ".exe";
#else
    return name;
#endif
}
// The profile folders after the one-time move from their old name (platform/user_folders.h), which runs before
// anything opens them.
inline UserPaths upgradedUserPaths(folders::Report* report = nullptr) {
    auto r = folders::migrateLegacyFolders();
    if (report) *report = std::move(r);
    return userPaths();
}
struct LauncherBackend {
    folders::Report folderUpgrade;
    UserPaths locations = upgradedUserPaths(&folderUpgrade);
    fs::path tools = executableDirectory();
    json config = json::object();
    LauncherBackend() {
        for (auto& p :
             {locations.data.parent_path(), locations.user, locations.config, locations.logs})
            fs::create_directories(p);
        // What the folder move did (kept for bug reports; the move only runs while an old folder exists).
        if (folderUpgrade.any()) {
            json record = {{"date", [] { auto t = std::time(nullptr); char b[32]; std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", std::localtime(&t)); return std::string(b); }()},
                           {"renamed", folderUpgrade.renamed}, {"copied", folderUpgrade.copied}, {"duplicates", folderUpgrade.duplicates},
                           {"leftovers", folderUpgrade.leftovers}, {"failed", folderUpgrade.failed}, {"rewritten", folderUpgrade.rewritten},
                           {"moves", json::array()}, {"log", folderUpgrade.lines}};
            for (const auto& m : folderUpgrade.moved) record["moves"].push_back({{"from", utf8(m.from)}, {"to", utf8(m.to)}});
            try {
                writeJson(locations.config / "folder-upgrade.json", record);
            } catch (...) {
            }
            for (const auto& line : folderUpgrade.lines) std::fprintf(stderr, "SafeHouse folders: %s\n", line.c_str());
        }
        {
            FileLock migrationLock(locations.config / "profile-migration.lock");
            importDirectoryIfEmpty(runtimeStorage(locations) / "Saves", locations.user / "Saves");
            importDirectoryIfEmpty(legacyLogDirectory(locations), locations.logs);
            importDirectoryIfEmpty(executableDirectory() / "Saves", locations.user / "Saves");
        }
        fs::create_directories(locations.user / "Saves");
        auto file = locations.config / "launcher.json";
        if (fs::exists(file)) {
            try {
                config = readJson(file);
                if (!config.is_object()) throw std::runtime_error("Invalid launcher configuration");
                for (auto key : {"runtime_dir", "iso_path"})
                    if (config.contains(key) && !config.at(key).is_string())
                        throw std::runtime_error("Invalid launcher configuration field");
                // An unexpected value only disables logging; keep the rest.
                if (config.contains("logging") && !config.at("logging").is_boolean())
                    config.erase("logging");
                // Launcher preferences fall back to their defaults one by one.
                for (auto key : {"sly2_iso_path", "sly3_iso_path", "last_game", "sly2_runtime", "browse_dir", "skipped_update"})
                    if (config.contains(key) && !config.at(key).is_string()) config.erase(key);
                // "sly2_wide_backgrounds" (Sly 2's "Widescreen backgrounds", removed 2026-11-05) is ignored and kept as it is.
                for (auto key : {"show_logs", "start_fullscreen", "sly2_native_preview", "sly3_notice_hidden", "check_updates", "keep_imported_iso"})
                    if (config.contains(key) && !config.at(key).is_boolean()) config.erase(key);
                for (auto key : {"ui_scale", "prompt_style"})
                    if (config.contains(key) && !config.at(key).is_number()) config.erase(key);
                // {"sly1": "europe"}: the region last chosen / played; {"sly1": {"europe": ISO}}: the disc images.
                for (auto key : {"regions", "region_isos"})
                    if (config.contains(key) && !config.at(key).is_object()) config.erase(key);
            } catch (...) {
                auto backup = file;
                backup += ".broken";
                std::error_code ec;
                fs::rename(file, backup, ec);
                config = json::object();
            }
        }
        if (config.contains("runtime_dir"))
            tools = locations.portable ? runtimeStorage(locations) / "runtime" / "bin"
                                       : path(config.at("runtime_dir").get<std::string>());
        // Recover activation interrupted between its two renames.
        for (const auto& target : {locations.data, runtimeStorage(locations) / "runtime"}) {
            auto previous = target;
            previous += ".previous";
            if (!fs::exists(target) && fs::exists(previous)) {
                try {
                    InstallationTransaction recovery(target);
                } catch (...) {
                }
            }
        }
    }
    // Log files are opt-in: they are only written for bug reports.
    bool loggingEnabled() const { return config.value("logging", false); }
    void setLoggingEnabled(bool enabled) {
        if (enabled)
            config["logging"] = true;
        else
            config.erase("logging");
        saveConfig();
    }
    bool sly2NativePreviewEnabled() const { return config.value("sly2_native_preview", false); }
    void setSly2NativePreviewEnabled(bool enabled) {
        if (sly2NativePreviewEnabled() == enabled) return;
        const auto previous = config;
        config["sly2_native_preview"] = enabled;
        try {
            saveConfig();
        } catch (...) {
            config = previous;
            throw;
        }
    }
    void saveConfig() { writeJson(locations.config / "launcher.json", config); }
    // "Don't show again" on the Sly 3 early-development notice before Play (launcher.json "sly3_notice_hidden").
    bool sly3NoticeHidden() const { return config.value("sly3_notice_hidden", false); }
    void hideSly3Notice() {
        config["sly3_notice_hidden"] = true;
        saveConfig();
    }
    // Manifests are parsed again only when they change (Sly 1's lists every file: the pages ask every second).
    Installation cachedInstallation(const fs::path& dir, GameId game) const {
        std::error_code ec;
        const auto file = dir / "installation.json";
        const auto stamp = fs::last_write_time(file, ec);
        const auto size = ec ? 0 : fs::file_size(file, ec);
        std::lock_guard lock(m_cacheMutex);
        for (auto& c : m_cache)
            if (c.file == file && c.stamp == stamp && c.size == size && c.value.game == game) return c.value;
        Installation in = readInstallation(dir, game);
        std::erase_if(m_cache, [&](const auto& c) { return c.file == file; });
        m_cache.push_back({file, stamp, size, in});
        return in;
    }
    struct CacheEntry {
        fs::path file;
        fs::file_time_type stamp;
        uintmax_t size;
        Installation value;
    };
    mutable std::mutex m_cacheMutex;
    mutable std::vector<CacheEntry> m_cache;
    // The game's original folder (installations/<game>): its first installed region.
    fs::path baseDir(GameId game) const { return gameDataDirectory(locations, game); }
    // ---- Where games install. Default: the profile's installations folder (baseDir's parent), as always. Settings >
    // Games "Install games to" (launcher.json "install_dir") picks another folder, e.g. another drive: new installs go
    // to <that folder>/SafeHouse/<game>[-<region>]. Every installation outside the default folder is remembered in
    // launcher.json "install_paths" ({"sly2": [dir, ...]}), so it is found wherever it is, also after the setting
    // changes again or when its drive was away for a while.
    fs::path defaultInstallRoot() const { return locations.data.parent_path(); }
    // The folder a chosen folder installs into: <chosen>/SafeHouse, unless the chosen folder is the default one, is
    // itself called SafeHouse or already holds SafeHouse installations.
    fs::path installRootFor(const fs::path& chosen) const {
        const fs::path c = fs::absolute(chosen).lexically_normal();
        const auto same = [](const fs::path& a, const fs::path& b) { return folders::detail::comparable(a) == folders::detail::comparable(b); };
        if (same(c, defaultInstallRoot()) || folders::detail::comparable(c.filename()) == folders::detail::comparable(brand::userFolder)) return c;
        std::error_code ec;
        for (GameId g : {GameId::Sly1, GameId::Sly2, GameId::Sly3})
            if (fs::is_regular_file(c / cane::game(g).key / "installation.json", ec)) return c;
        return c / brand::userFolder;
    }
    bool customInstallRoot() const {
        return config.contains("install_dir") && config.at("install_dir").is_string() && !config.at("install_dir").get<std::string>().empty();
    }
    fs::path installRoot() const { return customInstallRoot() ? path(config.at("install_dir").get<std::string>()) : defaultInstallRoot(); }
    // chosen: the folder picked (installRootFor() makes the install root); empty: back to the default.
    void setInstallRoot(const fs::path& chosen) {
        const auto previous = config;
        if (chosen.empty()) config.erase("install_dir");
        else {
            const fs::path root = installRootFor(chosen);
            if (folders::detail::comparable(root) == folders::detail::comparable(defaultInstallRoot())) config.erase("install_dir");
            else config["install_dir"] = utf8(root);
        }
        try {
            saveConfig();
        } catch (...) {
            config = previous;
            throw;
        }
    }
    std::vector<fs::path> rememberedInstallPaths(GameId game) const {
        std::vector<fs::path> out;
        const char* key = cane::game(game).key;
        if (config.contains("install_paths") && config.at("install_paths").is_object() && config.at("install_paths").contains(key) &&
            config.at("install_paths").at(key).is_array())
            for (const auto& v : config.at("install_paths").at(key))
                if (v.is_string() && !v.get<std::string>().empty()) out.push_back(path(v.get<std::string>()));
        return out;
    }
    void forgetInstallPath(GameId game, const fs::path& dir) {
        const char* key = cane::game(game).key;
        if (!config.contains("install_paths") || !config["install_paths"].is_object() || !config["install_paths"].contains(key) ||
            !config["install_paths"][key].is_array())
            return;
        auto& list = config["install_paths"][key];
        const auto target = folders::detail::comparable(fs::absolute(dir));
        for (size_t i = list.size(); i-- > 0;)
            if (list[i].is_string() && folders::detail::comparable(path(list[i].get<std::string>())) == target) list.erase(i);
        if (list.empty()) config["install_paths"].erase(key);
        if (config["install_paths"].empty()) config.erase("install_paths");
    }
    // Remembers an installation outside the default folder (one inside it is found by name anyway).
    void rememberInstallPath(GameId game, const fs::path& dir) {
        forgetInstallPath(game, dir);
        const fs::path d = fs::absolute(dir).lexically_normal();
        if (folders::detail::comparable(d.parent_path()) == folders::detail::comparable(defaultInstallRoot())) return;
        if (!config.contains("install_paths") || !config["install_paths"].is_object()) config["install_paths"] = json::object();
        auto& list = config["install_paths"][cane::game(game).key];
        if (!list.is_array()) list = json::array();
        list.push_back(utf8(d));
    }
    // Every installed region of a game: the original folder, then installations/<game>-<region> in the order of
    // knownRegions(), then the same names in the chosen install folder, then the remembered installations elsewhere.
    // A folder whose installation.json is missing is not an installation; one that cannot be read still is (it needs
    // repair). Staging / previous folders and locks use a '.' and are never listed. A region found twice is listed
    // once (the first one wins).
    std::vector<Installation> installations(GameId game) const {
        std::vector<Installation> out;
        if (!installable(game)) return out;
        std::error_code ec;
        const fs::path base = baseDir(game);
        if (fs::is_regular_file(base / "installation.json", ec)) out.push_back(cachedInstallation(base, game));
        const std::string prefix = std::string(cane::game(game).key) + "-";
        std::vector<std::string> seen{folders::detail::comparable(base)};
        auto consider = [&](const fs::path& dir, const std::string& regionFromName) {
            const auto id = folders::detail::comparable(dir);
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) return;
            seen.push_back(id);
            if (!fs::is_regular_file(dir / "installation.json", ec)) return;
            Installation in = cachedInstallation(dir, game);
            if (in.region.empty() && !regionFromName.empty()) in.region = regionFromName, in.release = regionName(regionFromName);
            // (a region already in the original folder wins; the duplicate is not listed)
            if (out.empty() || std::none_of(out.begin(), out.end(), [&](const Installation& o) { return o.region == in.region; }))
                out.push_back(in);
        };
        for (const auto& r : knownRegions(game)) consider(base.parent_path() / (prefix + r.key), r.key);
        if (customInstallRoot()) {
            const fs::path root = installRoot();
            consider(root / cane::game(game).key, {});
            for (const auto& r : knownRegions(game)) consider(root / (prefix + r.key), r.key);
        }
        for (const auto& dir : rememberedInstallPaths(game)) {
            std::string fromName;
            for (const auto& r : knownRegions(game))
                if (utf8(dir.filename()) == prefix + r.key) fromName = r.key;
            consider(dir, fromName);
        }
        if (!out.empty() && out[0].region.empty()) out[0].release = tr("Unknown region");
        return out;
    }
    std::optional<Installation> installation(GameId game, const std::string& region) const {
        for (auto& in : installations(game))
            if (in.region == region) return in;
        return std::nullopt;
    }
    // The region the launcher plays: the one chosen last (launcher.json "regions"), else the first playable one.
    std::string selectedRegion(GameId game) const {
        const auto all = installations(game);
        const char* key = cane::game(game).key;
        if (config.contains("regions") && config.at("regions").contains(key) && config.at("regions").at(key).is_string()) {
            const auto chosen = config.at("regions").at(key).get<std::string>();
            for (const auto& in : all)
                if (in.region == chosen) return chosen;
        }
        for (const auto& in : all)
            if (in.current) return in.region;
        return all.empty() ? std::string{} : all[0].region;
    }
    void setSelectedRegion(GameId game, const std::string& region) {
        const char* key = cane::game(game).key;
        if (config.contains("regions") && config["regions"].contains(key) && config["regions"][key] == region) return;
        if (!config.contains("regions")) config["regions"] = json::object();
        config["regions"][cane::game(game).key] = region;
        saveConfig();
    }
    // Where a region installs: its existing folder; else, in the install folder (installRoot()), <game> when that is
    // free (the default folder: when the game has no installation there) or <game>-<region>.
    fs::path regionDir(GameId game, const std::string& region) const {
        if (auto in = installation(game, region)) return in->dir;
        return newRegionDir(game, region, installRoot());
    }
    fs::path newRegionDir(GameId game, const std::string& region, const fs::path& root) const {
        std::error_code ec;
        const fs::path base = root / cane::game(game).key;
        if (folders::detail::comparable(root) == folders::detail::comparable(defaultInstallRoot())) {
            if (!fs::exists(base / "installation.json", ec)) return base;
        } else if (!fs::exists(base, ec) || (fs::is_directory(base, ec) && fs::is_empty(base, ec)))
            return base;
        return root / (std::string(cane::game(game).key) + "-" + region);
    }
    // The selected region's folder (the game's original folder when nothing is installed).
    fs::path dataDir(GameId game) const {
        const auto region = selectedRegion(game);
        if (auto in = installation(game, region)) return in->dir;
        return baseDir(game);
    }
    fs::path userDir(GameId game) const { return gameUserDirectory(locations, game); }
    // The game selected in the sidebar when the launcher last closed.
    GameId lastGame() const {
        if (config.contains("last_game"))
            if (auto id = gameFromKey(config.at("last_game").get<std::string>())) return *id;
        return GameId::Sly1;
    }
    void setLastGame(GameId game) {
        if (lastGame() == game && (config.contains("last_game") || game == GameId::Sly1)) return;
        config["last_game"] = cane::game(game).key;
        saveConfig();
    }
    // The game's runtime program is next to the launcher (or in the custom
    // runtime folder).
    bool runtimeAvailable(GameId game) const {
        const char* runtime = cane::game(game).runtime;
        std::error_code ec;
        return runtime && fs::is_regular_file(tools / executableName(runtime), ec);
    }
    // The installation manifest matches this launcher's asset format.
    bool assetsInstalled(GameId game) const {
        auto in = installation(game, selectedRegion(game));
        return in && in->current;
    }
    // Files from an older or damaged installation are present.
    bool needsRepair(GameId game) const {
        auto in = installation(game, selectedRegion(game));
        return in && !in->current;
    }
    // The selected installation is valid but in an older asset format: an update changed the game's file format.
    bool needsReextract(GameId game) const {
        auto in = installation(game, selectedRegion(game));
        return in && in->outdated;
    }
    fs::path binary(const std::string& name) const {
        auto p = tools / executableName(name);
        if (!fs::is_regular_file(p))
            throw std::runtime_error("Missing bundled program: " + utf8(p));
        return p;
    }
    // Ready to play: current assets and the runtime program. Sly 1 by default.
    bool installed(GameId game = GameId::Sly1) const {
        return assetsInstalled(game) && runtimeAvailable(game);
    }
    static const char* isoKey(GameId game) {
        return game == GameId::Sly1 ? "iso_path" : game == GameId::Sly3 ? "sly3_iso_path" : "sly2_iso_path";
    }
    // The disc image a region was installed from: launcher.json "region_isos", the pre-region keys (for the game's
    // original folder), then the installation's manifest. Region "" = the selected one.
    std::string rememberedIso(GameId game = GameId::Sly1, std::string region = {}) const {
        if (region.empty()) region = selectedRegion(game);
        const char* key = cane::game(game).key;
        if (config.contains("region_isos") && config.at("region_isos").contains(key) && config.at("region_isos").at(key).is_object() &&
            config.at("region_isos").at(key).contains(region) && config.at("region_isos").at(key).at(region).is_string())
            return config.at("region_isos").at(key).at(region).get<std::string>();
        const auto in = installation(game, region);
        if ((!in || in->dir == baseDir(game)) && config.contains(isoKey(game))) return config.at(isoKey(game)).get<std::string>();
        return in ? in->iso : std::string{};
    }
    void install(const std::string& iso, const std::atomic<bool>& cancel, const LogFunction& log) {
        install(iso, GameId::Sly1, cancel, log);
    }
    // Installs a disc image into its region's folder (regionDir): the disc decides the region, so another region's
    // disc installs beside the existing ones and the same region's disc replaces (re-extracts) that one.
    void install(const std::string& iso, GameId game, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (!installable(game))
            throw std::runtime_error(trf("%s cannot be installed yet", cane::game(game).title));
#if defined(CANE_MOBILE)
        // (the phone / tablet apps carry Sly 2 only: Sly 1's level tools and Sly 3's are not in the app, so its
        // extraction could only fail)
        if (!runtimeAvailable(game))
            throw std::runtime_error(trf("%s cannot be installed yet", cane::game(game).title));
#endif
        const DiscCheck check = checkDisc(path(iso), game);
        if (!check.ok()) throw std::runtime_error(check.message);
        const std::string region = check.region->key;
        const auto target = regionDir(game, region);
        log("Disc: " + check.label() + " -> " + utf8(target));
        std::vector<std::string> arguments{utf8(binary("cane-extractor")), "--iso", iso, "--output", utf8(target),
                                           "--non-interactive", "--progress=jsonl"};
        if (game != GameId::Sly1) arguments.insert(arguments.begin() + 1, {"--game", cane::game(game).key});
        // (Sly 2: the disc's languages go into the shared languages folder, langstore.h)
        if (game == GameId::Sly2) arguments.insert(arguments.end(), {"--languages-dir", utf8(languagesDir(game))});
        // The extractor's reason for a failure (its "error" progress event, else its last plain lines) replaces the bare
        // exit code in the message the install page shows.
        std::string reason;
        std::deque<std::string> lastLines;
        const LogFunction watch = [&](const std::string& line) {
            if (!line.empty() && line.front() == '{') {
                const auto event = json::parse(line, nullptr, false);
                if (event.is_object() && event.value("stage", std::string{}) == "error") reason = event.value("item", std::string{});
            } else if (!line.empty()) {
                lastLines.push_back(line);
                if (lastLines.size() > 2) lastLines.pop_front();
            }
            log(line);
        };
        try {
            checkedProcess(arguments, tools, cancel, watch);
        } catch (...) {
            // The extractor may be terminated without running destructors.
            // Acquire its released lock before discarding incomplete staging.
            try {
                InstallationTransaction cleanup(target);
            } catch (...) {
            }
            if (cancel || (reason.empty() && lastLines.empty())) throw;
            if (reason.empty()) {
                // (no error event: the exit code, then what the extractor said last)
                try {
                    throw;
                } catch (const std::exception& e) {
                    reason = e.what();
                } catch (...) {
                }
                for (const auto& line : lastLines) reason += (reason.empty() ? "" : " ") + line;
            }
            throw std::runtime_error(reason);
        }
        if (target == baseDir(game)) config[isoKey(game)] = iso;
        rememberInstallPath(game, target);
        if (!config.contains("region_isos")) config["region_isos"] = json::object();
        if (!config["region_isos"].contains(cane::game(game).key) || !config["region_isos"][cane::game(game).key].is_object())
            config["region_isos"][cane::game(game).key] = json::object();
        config["region_isos"][cane::game(game).key][region] = iso;
        if (!config.contains("regions")) config["regions"] = json::object();
        config["regions"][cane::game(game).key] = region;
        saveConfig();
#if defined(CANE_ANDROID) || defined(CANE_IOS)
        // Android: the document picker copied the disc image into the app (files/imports, native_dialog.h); once the game is
        // installed that copy only takes space (2.7 GB for Sly 2), unless Settings > Games "Keep imported disc images" is on.
        // Only a file inside files/imports is ever removed; a re-extract asks for the disc image again. (iOS: Documents/imports)
        if (!config.value("keep_imported_iso", false)) {
            std::error_code ec;
#if defined(CANE_IOS)
            const fs::path imports = fs::weakly_canonical(path(cane::ios::importsDirectory()), ec);
#else
            const char* home = std::getenv("HOME");
            const fs::path imports = home && *home ? fs::weakly_canonical(path(home) / "imports", ec) : fs::path{};
#endif
            const fs::path file = fs::weakly_canonical(path(iso), ec);
            if (!imports.empty() && !ec && file.parent_path() == imports && fs::is_regular_file(file, ec) && fs::remove(file, ec))
                log("Removed the app's copy of the disc image (" + utf8(file.filename()) + ")");
        }
#endif
    }
    // Removes the selected region's installed files. Saves and settings are kept.
    void uninstall(GameId game) {
        const auto target = fs::absolute(dataDir(game));
        auto lock = target;
        lock += ".lock";
        FileLock guard(lock);
        for (const char* suffix : {"", ".previous", ".staging"}) {
            auto p = target;
            p += suffix;
            fs::remove_all(p);
        }
        forgetInstallPath(game, target);
        saveConfig();
    }
    // Moves an installed region to another install folder (Settings > Games "Move <region> to the install folder"; --move-install passes
    // installRootFor(the folder given)), named like a new install there. One rename on the same drive; else a copy into "<target>.staging" (sizes checked file by
    // file), renamed into place when complete, and only then is the old folder removed. Saves are not touched.
    fs::path moveInstallation(GameId game, const std::string& region, const fs::path& installFolder, const std::atomic<bool>& cancel,
                              const LogFunction& log) {
        const auto in = installation(game, region);
        if (!in) throw std::runtime_error(trf("%s has no %s installation", cane::game(game).shortName, tr(regionName(region)).c_str()));
        const fs::path source = fs::absolute(in->dir).lexically_normal();
        const fs::path root = fs::absolute(installFolder).lexically_normal();
        const fs::path target = fs::absolute(newRegionDir(game, in->region.empty() ? std::string("other") : in->region, root)).lexically_normal();
        const auto inside = [](const fs::path& a, const fs::path& b) {  // a inside (or equal to) b
            const auto x = folders::detail::comparable(a), y = folders::detail::comparable(b);
            return x == y || (x.size() > y.size() && x.compare(0, y.size(), y) == 0 && x[y.size()] == '/');
        };
        if (inside(target, source) || inside(source, target) ||
            folders::detail::comparable(source.parent_path()) == folders::detail::comparable(root))
            throw std::runtime_error(tr("The installation is already in that folder"));
        std::error_code ec;
        if (fs::exists(target, ec) && !fs::is_empty(target, ec))
            throw std::runtime_error(trf("%s already exists. Choose another folder", utf8(target).c_str()));
        auto sourceLock = source;
        sourceLock += ".lock";
        FileLock guard(sourceLock);
        fs::create_directories(root);
        fs::remove(target, ec);  // (an empty folder)
        log("Moving " + utf8(source) + " -> " + utf8(target));
        ec.clear();
        if (folders::detail::renameAllowed()) fs::rename(source, target, ec);
        else ec = std::make_error_code(std::errc::cross_device_link);  // (test hook: as if on another drive)
        if (ec) {
            // another drive: copy, check, switch, then remove the original
            std::vector<std::pair<fs::path, uintmax_t>> files;
            uintmax_t total = 0;
            for (fs::recursive_directory_iterator it(source), end; it != end; ++it)
                if (it->is_regular_file()) files.emplace_back(it->path(), it->file_size()), total += files.back().second;
            if (const auto space = fs::space(root, ec); !ec && space.available < total + (256ull << 20))
                throw std::runtime_error(trf("Not enough free space in %s: the installation needs %s", utf8(root).c_str(), aboutGb(total).c_str()));
            InstallationTransaction transaction(target);
            uintmax_t done = 0;
            size_t lastMiB = 0;
            for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
                if (cancel) throw std::runtime_error("Cancelled");
                const fs::path to = transaction.directory() / it->path().lexically_relative(source);
                if (it->is_directory()) fs::create_directories(to);
                else if (it->is_regular_file()) {
                    fs::create_directories(to.parent_path());
                    fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing);
                    if (fs::file_size(to) != it->file_size()) throw std::runtime_error(trf("Copy failed: %s", utf8(to).c_str()));
                    done += it->file_size();
                    if (const size_t mib = size_t(done >> 20); mib >= lastMiB + 16 || done == total)
                        log(json({{"stage", "move"}, {"completed", lastMiB = mib}, {"total", size_t(total >> 20)},
                                  {"item", utf8(it->path().lexically_relative(source))}})
                                .dump());
                }
            }
            for (const auto& [file, size] : files)
                if (fs::file_size(transaction.directory() / file.lexically_relative(source), ec) != size || ec)
                    throw std::runtime_error(trf("Copy failed: %s", utf8(file).c_str()));
            transaction.commit();
            fs::remove_all(source, ec);
            if (ec) log("The old folder could not be removed completely: " + utf8(source));
        }
        forgetInstallPath(game, source);
        rememberInstallPath(game, target);
        saveConfig();
        log(json({{"stage", "moved"}, {"completed", 1}, {"total", 1}}).dump());
        return target;
    }
    void play(const std::atomic<bool>& cancel, const LogFunction& log) { play(GameId::Sly1, cancel, log); }
    // Settings > Advanced > Debug mode: one Debug Mode.ini in the profile root (Sly 1's user folder). Sly 2 and Sly 3 run
    // with --user-dir <profile>/KEY, where cane::debugMode() looks, so each start copies the switch there (the root
    // file wins; a missing root file is off).
    fs::path debugModeFile() const { return locations.user / "Debug Mode.ini"; }
    void syncDebugMode(GameId game) const {
        const fs::path target = userDir(game) / "Debug Mode.ini";
        if (target == debugModeFile()) return;
        const bool enabled = debugModeIn(debugModeFile());
        std::ofstream(target) << "[Debug]\nDebug Mode=" << (enabled ? 1 : 0) << '\n';
    }
    // Runtime contract (all games): <runtime> --data-dir <installations/KEY>
    // --user-dir <profile>[/KEY], started in the runtime's own folder.
    // The region played is the selected one (`region`, when given, is selected first and so remembered).
    // vr: Sly 2 in VR (Sly2-Release --vr: an OpenXR headset; the game runs flat when it finds none). The launcher's
    // "Play in VR" (ui/game_page.h, vr_probe.h) and `--play-vr sly2`.
    void play(GameId game, const std::atomic<bool>& cancel, const LogFunction& log, const std::string& region = {}, bool vr = false) {
        if (!region.empty()) {
            if (!installation(game, region))
                throw std::runtime_error(trf("%s has no %s installation", cane::game(game).shortName, tr(regionName(region)).c_str()));
            setSelectedRegion(game, region);
        }
        if (needsReextract(game))
            throw std::runtime_error(trf("An update changed the format of the %s game files. Open SafeHouse and re-extract your disc to keep playing",
                                         cane::game(game).shortName));
        if (game == GameId::Sly1 && !installed())
            throw std::runtime_error(tr("Install the game with this launcher version first"));
        if (game != GameId::Sly1) {
            if (!assetsInstalled(game))
                throw std::runtime_error(tr("Install the game with this launcher version first"));
            if (!runtimeAvailable(game))
                throw std::runtime_error(trf("%s is still in development", cane::game(game).title));
        }
        fs::create_directories(userDir(game));
        syncDebugMode(game);
        // Sly 2 runs the native port (Sly2-Release, from its boot sequence). The emulated runtime and its
        // "sly2_runtime" / "sly2_native_preview" options are gone; the keys are ignored if present. So is
        // "sly2_wide_backgrounds" (the "Widescreen backgrounds" toggle, removed 2026-11-05 as redundant: Sly2-Release's
        // widescreen backgrounds are always on; --wide-backgrounds 0 / SLY2N_WIDEBG=0 still turn them off by hand).
        const std::string runtime = cane::game(game).runtime;
        std::vector<std::string> arguments{utf8(binary(runtime)), "--data-dir", utf8(dataDir(game)),
                                           "--user-dir", utf8(userDir(game))};
        for (auto& a : ps3HdArguments(game)) arguments.push_back(a);
        if (game == GameId::Sly2) {
            // (a languages folder from before the Japanese font was imported: taken from the Japan installation now,
            // so that Japanese shows its text on the other discs; the game starts either way)
            try {
                if (japaneseFontSource()) addJapaneseFont(cancel, [](const std::string&) {});
            } catch (...) {
            }
            arguments.insert(arguments.end(), {"--languages-dir", utf8(languagesDir(game))});
            if (vr) arguments.push_back("--vr");
        }
        explainedProcess(arguments, cancel, log);
    }
    // checkedProcess, but a failure's message is the program's own reason (its last line that reads as an error, else
    // its last line) with the exit code, not "Details are in the launcher output" (the toast is all a phone shows).
    void explainedProcess(const std::vector<std::string>& arguments, const std::atomic<bool>& cancel, const LogFunction& log) {
        std::string last, lastError;
        const int code = runProcess(arguments, tools, cancel, [&](const std::string& line) {
            if (!line.empty() && line.front() != '{') {
                last = line;
                std::string lower = line;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                for (const char* word : {"error", "fail", "required", "cannot", "could not", "once per app start"})
                    if (lower.find(word) != std::string::npos) lastError = line;
            }
            log(line);
        });
        if (code == 0) return;
        const std::string& reason = lastError.empty() ? last : lastError;
        if (cancel || reason.empty())
            throw std::runtime_error(arguments[0] + " failed (exit " + std::to_string(code) + "). Details are in the launcher output.");
        throw std::runtime_error(reason + " (" + utf8(path(arguments[0]).filename()) + ", exit " + std::to_string(code) + ")");
    }
    // Sly 2's texture dump (Textures/ in the installation, MODDING.md): the converter writes it at install since
    // 2026-11-04; an installation from before has none, and "Export textures" writes it in place (no re-extract).
    bool texturesMissing(GameId game) const {
        if (game != GameId::Sly2 || !assetsInstalled(game)) return false;
        std::error_code error;
        return !fs::is_directory(dataDir(game) / "Textures", error);
    }
    void exportTextures(GameId game, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (game != GameId::Sly2 || !assetsInstalled(game))
            throw std::runtime_error(tr("Install the game with this launcher version first"));
        fs::create_directories(userDir(game));
        // (Sly2-Release --export-textures: Textures.partial, renamed when complete; "textures I N WORLD" lines)
        checkedProcess({utf8(binary(cane::game(game).runtime)), "--data-dir", utf8(dataDir(game)), "--user-dir",
                        utf8(userDir(game)), "--export-textures"},
                       tools, cancel, [&](const std::string& line) {
                           size_t done = 0, total = 0;
                           char item[256] = {};
                           if (std::sscanf(line.c_str(), "textures %zu %zu %255s", &done, &total, item) >= 2)
                               log(json({{"stage", "textures"}, {"completed", done}, {"total", total}, {"item", item}}).dump());
                           else
                               log(line);
                       });
    }
    // ---- The PS3 HD textures: The Sly Collection's (PS3) redrawn textures as a texture pack for
    // Sly 1 and Sly 2. The game program builds it (--ps3-hd-textures SOURCE OUT: the extracted disc, its .iso or a
    // .zip of that, streamed) into <user dir>/PS3 HD/Textures.pak (or, to edit them, PS3 HD/Textures, the PNG files:
    // --ps3-hd-png), named after the installed textures they replace. The game loads it under the player's own Mods
    // (--texture-pack) while it is turned on (launcher.json "ps3_hd").
    static bool ps3HdGame(GameId game) { return game == GameId::Sly1 || game == GameId::Sly2; }
    fs::path ps3HdDir(GameId game) const { return userDir(game) / "PS3 HD"; }
    struct Ps3HdPack {
        bool installed = false, enabled = false;
        bool png = false;   // (the PNG files in a Textures folder, not Textures.pak)
        int textures = 0;
        double megabytes = 0;
        std::string source, date;
    };
    Ps3HdPack ps3HdPack(GameId game) const {
        Ps3HdPack pack;
        if (!ps3HdGame(game)) return pack;
        std::error_code ec;
        // (a Textures folder: imported as PNG files, or before Textures.pak)
        pack.png = !fs::is_regular_file(ps3HdDir(game) / "Textures.pak", ec);
        if (pack.png && !fs::is_directory(ps3HdDir(game) / "Textures", ec)) return pack;
        pack.installed = true;
        try {
            const json m = readJson(ps3HdDir(game) / "pack.json");
            pack.textures = m.value("textures", 0);
            pack.megabytes = m.value("megabytes", 0.0);
            pack.source = m.value("source", std::string{});
            pack.date = m.value("date", std::string{});
        } catch (...) {
        }
        pack.enabled = !(config.contains("ps3_hd") && config.at("ps3_hd").is_object() && config.at("ps3_hd").contains(cane::game(game).key) &&
                         config.at("ps3_hd").at(cane::game(game).key) == false);
        return pack;
    }
    void setPs3HdEnabled(GameId game, bool enabled) {
        if (!config.contains("ps3_hd") || !config.at("ps3_hd").is_object()) config["ps3_hd"] = json::object();
        if (enabled) config["ps3_hd"].erase(cane::game(game).key);
        else config["ps3_hd"][cane::game(game).key] = false;
        saveConfig();
    }
    // The next import's form: the PNG files (launcher.json "ps3_hd_png"), else Textures.pak
    bool ps3HdPng(GameId game) const {
        const auto it = config.find("ps3_hd_png");
        return it != config.end() && it->is_object() && it->value(cane::game(game).key, false);
    }
    void setPs3HdPng(GameId game, bool png) {
        if (!config.contains("ps3_hd_png") || !config.at("ps3_hd_png").is_object()) config["ps3_hd_png"] = json::object();
        if (png) config["ps3_hd_png"][cane::game(game).key] = true;
        else config["ps3_hd_png"].erase(cane::game(game).key);
        saveConfig();
    }
    void removePs3Hd(GameId game) {
        std::error_code ec;
        for (const char* suffix : {"", ".staging", ".previous"}) {
            auto p = ps3HdDir(game);
            p += suffix;
            fs::remove_all(p, ec);
        }
    }
    // Imports the pack from a disc source (png: as PNG files); the old pack (if any) is replaced only when the new one is
    // complete. The game program's "ps3hd: error: ..." line becomes the error shown.
    void importPs3Hd(GameId game, const std::string& source, bool png, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (!ps3HdGame(game)) throw std::runtime_error(tr("The PS3 HD textures are for Sly 1 and Sly 2"));
        if (!assetsInstalled(game)) throw std::runtime_error(trf("Install %s first: the HD textures replace its installed ones", cane::game(game).shortName));
        if (!runtimeAvailable(game)) throw std::runtime_error(trf("%s is still in development", cane::game(game).title));
        std::error_code ec;
        fs::create_directories(userDir(game), ec);
        if (const auto space = fs::space(userDir(game), ec); !ec && space.available < (2ull << 30))
            throw std::runtime_error(tr("Not enough free disk space: the PS3 HD textures need about 1 GB (Sly 1: 0.6 GB, Sly 2: 0.3 GB)"));
        auto staging = ps3HdDir(game);
        staging += ".staging";
        fs::remove_all(staging, ec);
        std::string error, summary, lastItem;
        auto run = [&] {
            std::vector<std::string> arguments{utf8(binary(cane::game(game).runtime)), "--data-dir", utf8(dataDir(game)), "--user-dir",
                                               utf8(userDir(game)), "--ps3-hd-textures", source, utf8(staging)};
            if (png) arguments.push_back("--ps3-hd-png");
            return runProcess(arguments, tools, cancel, [&](const std::string& line) {
                                  char what[32] = {}, item[256] = {};
                                  unsigned long long done = 0, total = 0;
                                  if (std::sscanf(line.c_str(), "ps3hd-progress %31s %llu %llu %255[^\n]", what, &done, &total, item) >= 3) {
                                      // one bar: the read of the disc (the worlds are matched as they are read; the
                                      // last one is the item), after Sly 1's look at its installed levels
                                      const std::string w = what;
                                      if (w == "textures") lastItem = item;
                                      else
                                          log(json({{"stage", w == "read" ? (lastItem.empty() ? "ps3-read" : "ps3-textures") : "ps3-prepare"},
                                                    {"completed", done}, {"total", total}, {"item", w == "read" ? lastItem : std::string(item)}})
                                                  .dump());
                                      return;
                                  }
                                  if (line.rfind("ps3hd: error: ", 0) == 0) error = line.substr(14);
                                  else if (line.find(" HD textures written") != std::string::npos) summary = line;
                                  log(line);
                              });
        };
        int code = 1;
        try {
            code = run();
        } catch (...) {
            fs::remove_all(staging, ec);
            throw;
        }
        if (code != 0 || !(png ? fs::is_directory(staging / "Textures", ec) : fs::is_regular_file(staging / "Textures.pak", ec))) {
            fs::remove_all(staging, ec);
            if (!error.empty()) {
                if (error.back() == '.') error.pop_back();
                throw std::runtime_error(error);
            }
            throw std::runtime_error(trf("%s could not import the PS3 HD textures (exit %d). Details are in the launcher output", cane::game(game).runtime, code));
        }
        // pack.json: what the Settings page shows
        int textures = 0;
        double megabytes = 0;
        if (const auto at = summary.find(" levels, "); at != std::string::npos) std::sscanf(summary.c_str() + at + 9, "%d HD textures written (%lf MB", &textures, &megabytes);
        else if (const auto at2 = summary.find(" worlds, "); at2 != std::string::npos) std::sscanf(summary.c_str() + at2 + 9, "%d HD textures written (%lf MB", &textures, &megabytes);
        writeJson(staging / "pack.json", json{{"source", source}, {"textures", textures}, {"megabytes", megabytes},
                                              {"date", [] { auto t = std::time(nullptr); char b[32]; std::strftime(b, sizeof(b), "%Y-%m-%d", std::localtime(&t)); return std::string(b); }()},
                                              {"from", "The Sly Collection (PS3)"}});
        auto previous = ps3HdDir(game);
        previous += ".previous";
        fs::remove_all(previous, ec);
        if (fs::exists(ps3HdDir(game), ec)) fs::rename(ps3HdDir(game), previous);
        fs::rename(staging, ps3HdDir(game));
        fs::remove_all(previous, ec);
        setPs3HdEnabled(game, true);
        log(json({{"stage", "ps3-done"}, {"completed", 1}, {"total", 1}}).dump());
    }
    // ---- Sly 2's languages (Sly2-Release/langstore.h, notes/LANG_2026-11-10.md): one languages folder shared by every
    // region's installation, sly2-languages beside them. The converter puts each installed disc's languages there
    // (cane-extractor --languages-dir); other discs' languages are imported into it (a disc image, an older
    // installation's own, The Sly Collection's French / Spanish subtitles). The game offers them all on the title.
    fs::path languagesDir(GameId game) const {
        return baseDir(game).parent_path() / (std::string(cane::game(game).key) + "-languages");
    }
    struct LanguageInfo {
        std::string key;            // the folder name ("French", "English (UK)"): Settings.ini's Language value
        std::string source;         // the disc it came from
        bool text = false, voice = false, movies = false;
        bool wide = false;          // Japanese script: its text needs the Japanese font
        bool font = false;          // (wide) the languages folder has that font (Font.bin): every disc shows its text
        bool disc = false;          // one of an installation's own (its world files)
    };
    // The languages the game offers on the selected region (Sly2-Release/langstore.cpp InitLanguageStore, the same
    // rules): its installation's own, then the languages folder's and the mods folder's Mods/Languages/ that have text,
    // except those taken from a disc of the selected region's own family ("English (UK)" on Europe); the game's order,
    // then by name. Another region's own languages appear once shared into the folder (Settings > Games > Sly 2).
    std::vector<LanguageInfo> sly2Languages() const {
        static const char* const order[] = {"English", "French", "Italian", "German", "Spanish", "Dutch", "Portuguese",
                                            "Danish", "Finnish", "Norwegian", "Swedish", "Japanese", "Korean", "Chinese"};
        std::map<std::string, LanguageInfo> all;
        std::error_code ec;
        const std::string selected = selectedRegion(GameId::Sly2);
        std::string family;
        auto regionFamily = [](const std::string& disc) { return disc.substr(0, disc.find(' ')); };
        for (const auto& in : installations(GameId::Sly2)) {
            if (in.region != selected) continue;
            family = regionFamily(in.name());
            try {
                // (the manifest's "languages" near its start: the whole file is megabytes)
                std::ifstream f(in.dir / "extraction_manifest.json", std::ios::binary);
                std::string head(8192, '\0');
                f.read(head.data(), std::streamsize(head.size()));
                head.resize(size_t(f.gcount()));
                const auto at = head.find("\"languages\"");
                const auto open = at == std::string::npos ? at : head.find('[', at);
                const auto close = open == std::string::npos ? open : head.find(']', open);
                if (close == std::string::npos) continue;
                for (const auto& l : json::parse(head.substr(open, close - open + 1))) {
                    auto& info = all[l.get<std::string>()];
                    info.key = l.get<std::string>();
                    info.disc = info.text = info.voice = info.movies = true;
                    info.wide = info.key == "Japanese";
                    if (info.source.empty()) info.source = in.name();
                }
            } catch (...) {
            }
        }
        std::map<std::string, LanguageInfo> folder;
        for (const fs::path& dir : {languagesDir(GameId::Sly2), userDir(GameId::Sly2) / "Mods" / "Languages"}) {
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (!e.is_directory(ec)) continue;
                const std::string key = utf8(e.path().filename());
                if (key.empty() || key[0] == '.') continue;
                auto& info = folder[key];
                info.key = key;
                info.text = info.text || fs::is_regular_file(e.path() / "Text.json", ec);
                info.voice = info.voice || fs::is_directory(e.path() / "Dialog", ec);
                info.movies = info.movies || fs::is_directory(e.path() / "Movies", ec);
                bool script = false;
                try {
                    const json j = readJson(e.path() / "language.json");
                    if (info.source.empty()) info.source = j.value("source", std::string{});
                    if (j.contains("text") && !info.text) info.text = info.voice;  // (a dub that names its text)
                    if (j.contains("script")) script = true, info.wide = info.wide || j.value("script", std::string{}) == "japanese";
                } catch (...) {
                }
                if (!script && (key == "Japanese" || key == "Korean" || key == "Chinese")) info.wide = true;
                info.font = info.font || fs::is_regular_file(e.path() / "Font.bin", ec);
            }
        }
        // (a Japanese text variant without a font of its own is drawn with the "Japanese" folder's: langstore.cpp)
        if (const auto japanese = folder.find("Japanese"); japanese != folder.end() && japanese->second.font)
            for (auto& [key, info] : folder) info.font = info.font || info.wide;
        for (auto& [key, info] : folder) {
            if (all.count(key)) {
                // (a disc language the folder adds to: still the disc's)
                auto& d = all[key];
                d.voice = d.voice || info.voice, d.movies = d.movies || info.movies;
                continue;
            }
            if (!info.text) continue;
            if (!family.empty() && !info.source.empty() && regionFamily(info.source) == family) continue;
            all[key] = info;
        }
        std::vector<LanguageInfo> out;
        for (auto& [k, v] : all) out.push_back(v);
        auto rank = [&](const std::string& key) {
            const std::string base = key.substr(0, key.find(" ("));
            for (size_t i = 0; i < std::size(order); ++i)
                if (base == order[i]) return int(i) * 2 + (base == key ? 0 : 1);
            return 100;
        };
        std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
            return rank(a.key) != rank(b.key) ? rank(a.key) < rank(b.key) : a.key < b.key;
        });
        return out;
    }
    // ---- Sly 1's language (Sly1-Release/content.cpp): the selected installation's manifest lists its languages
    // ("languages", Japan also "voiceLanguages"); the choice is <profile>/content_settings.json "languages" {serial:
    // name}, which the game reads at start and its PAL menu writes. Nothing else changes: the same file and names.
    struct Sly1Languages {
        std::string serial, region;           // "SCES-509.17", "PAL"
        std::vector<std::string> languages;   // text languages, then voice-only ones (Japan: "English" voices)
        std::vector<std::string> voiceOnly;
        std::string selected;                 // the saved choice, else the game's default
    };
    Sly1Languages sly1Languages() const {
        Sly1Languages out;
        const auto in = installation(GameId::Sly1, selectedRegion(GameId::Sly1));
        if (!in) return out;
        try {
            const json m = readJson(in->dir / "extraction_manifest.json");
            out.serial = m.value("serial", std::string{});
            out.region = m.value("region", std::string{});
            if (m.contains("languages") && m["languages"].is_array())
                for (const auto& l : m["languages"])
                    if (l.is_string()) out.languages.push_back(l.get<std::string>());
            if (m.contains("voiceLanguages") && m["voiceLanguages"].is_array())
                for (const auto& l : m["voiceLanguages"])
                    if (l.is_string() && std::find(out.languages.begin(), out.languages.end(), l.get<std::string>()) == out.languages.end())
                        out.languages.push_back(l.get<std::string>()), out.voiceOnly.push_back(l.get<std::string>());
        } catch (...) {
            return {};
        }
        if (out.languages.empty()) out.languages.push_back("English");
        out.selected = out.region == "NTSC-J" ? "Japanese" : out.region == "NTSC-K" ? "Korean" : "English";
        try {
            const json s = readJson(userDir(GameId::Sly1) / "content_settings.json");
            if (s.contains("languages") && s["languages"].is_object() && s["languages"].contains(out.serial) && s["languages"][out.serial].is_string())
                out.selected = s["languages"][out.serial].get<std::string>();
        } catch (...) {
        }
        return out;
    }
    void setSly1Language(const std::string& serial, const std::string& language) const {
        if (serial.empty()) return;
        const fs::path file = userDir(GameId::Sly1) / "content_settings.json";
        json s = json::object();
        try {
            s = readJson(file);
            if (!s.is_object()) s = json::object();
        } catch (...) {
        }
        if (!s.contains("languages") || !s["languages"].is_object()) s["languages"] = json::object();
        s["languages"][serial] = language;
        writeJson(file, s);
    }
    // An installation that still keeps its languages in its own folder (installed before the languages folder):
    // "share" moves them there, so the other regions offer them too.
    std::vector<Installation> sly2InstallationsWithOwnLanguages() const {
        std::vector<Installation> out;
        for (const auto& in : installations(GameId::Sly2)) {
            if (!in.current) continue;
            std::error_code ec;
            if (fs::is_directory(in.dir / "Dialog", ec)) out.push_back(in);
        }
        return out;
    }
    // The Sly Collection's languages (French, Spanish) the game has neither on an installation nor in the folder.
    std::vector<std::string> ps3MissingLanguages() const {
        std::vector<std::string> out;
        const auto have = sly2Languages();
        for (const char* key : {"French", "Spanish"})
            if (std::none_of(have.begin(), have.end(), [&](const LanguageInfo& l) { return l.key == key && l.text; })) out.push_back(key);
        return out;
    }
    // Runs the game program's language import (its "progress N M item" lines as the job's).
    void runLanguageTool(std::vector<std::string> arguments, const char* stage, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (!runtimeAvailable(GameId::Sly2)) throw std::runtime_error(tr("Sly 2 is still in development"));
        std::error_code ec;
        fs::create_directories(userDir(GameId::Sly2), ec);
        fs::create_directories(languagesDir(GameId::Sly2), ec);
        std::string error;
        arguments.insert(arguments.begin(), utf8(binary(cane::game(GameId::Sly2).runtime)));
        const int code = runProcess(arguments, tools, cancel, [&](const std::string& line) {
            unsigned long long done = 0, total = 0;
            char item[256] = {};
            if (std::sscanf(line.c_str(), "progress %llu %llu %255[^\n]", &done, &total, item) >= 2) {
                log(json({{"stage", stage}, {"completed", done}, {"total", total}, {"item", item}}).dump());
                return;
            }
            if (line.rfind("ps3lang: error: ", 0) == 0) error = line.substr(16);
            else if (line.rfind("import-languages: ", 0) == 0 && line.find(" files written") == std::string::npos) error = line.substr(18);
            log(line);
        });
        if (cancel) throw std::runtime_error("Cancelled");
        if (code != 0) throw std::runtime_error(error.empty() ? trf("The language import failed (exit %d). Details are in the launcher output", code) : error);
        log(json({{"stage", stage}, {"completed", 1}, {"total", 1}}).dump());
    }
    // Japanese is in the languages folder without the Japanese disc's text font (imported by a SafeHouse before the
    // font was taken along): the USA / European discs then play its voices over their own text.
    bool japaneseFontMissing() const {
        std::error_code ec;
        const fs::path dir = languagesDir(GameId::Sly2) / "Japanese";
        return fs::is_regular_file(dir / "Text.json", ec) && !fs::is_regular_file(dir / "Font.bin", ec);
    }
    // The installed Japan disc, when it can supply that font.
    std::optional<Installation> japaneseFontSource() const {
        if (!japaneseFontMissing()) return std::nullopt;
        for (const auto& in : installations(GameId::Sly2))
            if (in.region == "japan" && in.current) return in;
        return std::nullopt;
    }
    // Adds the font from the Japan installation (the game program's language import: it writes only what the folder
    // lacks; a few seconds).
    void addJapaneseFont(const std::atomic<bool>& cancel, const LogFunction& log) {
        const auto in = japaneseFontSource();
        if (!in) return;
        runLanguageTool({"--data-dir", utf8(in->dir), "--user-dir", utf8(userDir(GameId::Sly2)), "--languages-dir",
                         utf8(languagesDir(GameId::Sly2)), "--import-languages", utf8(languagesDir(GameId::Sly2)), "--only-language", "Japanese"},
                        "languages", cancel, log);
    }
    // A disc image's languages (Europe: 11, Japan: Japanese; USA: English) into the folder, without installing it.
    void importLanguagesFromDisc(const std::string& iso, const std::atomic<bool>& cancel, const LogFunction& log) {
        const DiscCheck check = checkDisc(path(iso), GameId::Sly2);
        if (!check.ok()) throw std::runtime_error(check.message);
        runLanguageTool({"--disc", iso, "--user-dir", utf8(userDir(GameId::Sly2)), "--languages-dir", utf8(languagesDir(GameId::Sly2)),
                         "--import-languages", utf8(languagesDir(GameId::Sly2))},
                        "languages", cancel, log);
    }
    // An older installation's own languages moved into the folder (its manifests point there afterwards).
    void shareInstallationLanguages(const std::string& region, const std::atomic<bool>& cancel, const LogFunction& log) {
        const auto in = installation(GameId::Sly2, region);
        if (!in) throw std::runtime_error(trf("%s has no %s installation", "Sly 2", tr(regionName(region)).c_str()));
        auto lock = fs::absolute(in->dir);
        lock += ".lock";
        FileLock guard(lock);
        runLanguageTool({"--data-dir", utf8(in->dir), "--user-dir", utf8(userDir(GameId::Sly2)), "--languages-dir",
                         utf8(languagesDir(GameId::Sly2)), "--import-languages", utf8(languagesDir(GameId::Sly2)), "--move-languages"},
                        "languages", cancel, log);
    }
    // The Sly Collection's (PS3) French and Spanish (text: subtitles over the English voices), when missing.
    void importPs3Languages(const std::string& source, const std::atomic<bool>& cancel, const LogFunction& log) {
        if (!assetsInstalled(GameId::Sly2)) throw std::runtime_error(tr("Install Sly 2 first (the USA or a European disc)"));
        runLanguageTool({"--data-dir", utf8(dataDir(GameId::Sly2)), "--user-dir", utf8(userDir(GameId::Sly2)), "--languages-dir",
                         utf8(languagesDir(GameId::Sly2)), "--import-ps3-languages", source, utf8(languagesDir(GameId::Sly2))},
                        "ps3-languages", cancel, log);
    }
    // The game's --texture-pack argument while the pack is installed and on.
    std::vector<std::string> ps3HdArguments(GameId game) const {
        const Ps3HdPack pack = ps3HdPack(game);
        if (!pack.installed || !pack.enabled) return {};
        return {"--texture-pack", utf8(ps3HdDir(game))};
    }
    void buildSource(const fs::path& checkout, const std::atomic<bool>& cancel,
                     const LogFunction& log) {
        auto source = fs::absolute(checkout);
        if (!fs::is_regular_file(source / "CMakeLists.txt") ||
            !fs::is_directory(source / "Sly1-Release"))
            throw std::runtime_error(
                "Select a SafeHouse checkout containing the portable CMake build");
        fs::create_directories(runtimeStorage(locations));
        FileLock buildLock(runtimeStorage(locations) / "source-build.lock");
        auto build = runtimeStorage(locations) / "source-build";
        std::ifstream cache(build / "CMakeCache.txt");
        std::string line;
        while (std::getline(cache, line)) {
            constexpr const char* prefix = "CMAKE_HOME_DIRECTORY:INTERNAL=";
            if (line.starts_with(prefix) &&
                fs::weakly_canonical(path(line.substr(std::char_traits<char>::length(prefix)))) !=
                    fs::weakly_canonical(source)) {
                cache.close();
                fs::remove_all(build);
                break;
            }
        }
        try {
            checkedProcess({"cmake", "--version"}, source, cancel, log);
        } catch (...) {
            if (cancel) throw;
            throw std::runtime_error(
                "CMake is missing or cannot run. Install CMake 3.22+ and add it to PATH, then "
                "restart the launcher.");
        }
        std::vector<std::string> configure = {"cmake",
                                              "-S",
                                              utf8(source),
                                              "-B",
                                              utf8(build),
                                              "-DCMAKE_BUILD_TYPE=Release",
                                              "-DCANE_BUILD_LAUNCHER=OFF",
                                              "-DBUILD_TESTING=OFF"};
        try {
            checkedProcess(configure, source, cancel, log);
        } catch (...) {
            if (cancel) throw;
#ifdef _WIN32
            throw std::runtime_error(
                "Configuration failed. Install Visual Studio 2022 with Desktop development with "
                "C++, or check the build log for missing dependencies.");
#else
            throw std::runtime_error(
                "Configuration failed. Install a C++20 compiler, make/Ninja, and the development "
                "packages listed in README.md. See the build log for the exact missing tool or "
                "library.");
#endif
        }
        checkedProcess({"cmake", "--build", utf8(build), "--config", "Release", "--parallel", "2"},
                       source, cancel, log);
        auto runtime = runtimeStorage(locations) / "runtime";
        InstallationTransaction transaction(runtime);
        checkedProcess({"cmake", "--install", utf8(build), "--config", "Release", "--prefix",
                        utf8(transaction.directory())},
                       source, cancel, log);
        for (auto name : {"cane-extractor", "Sly1-Release"})
            if (!fs::is_regular_file(transaction.directory() / "bin" / executableName(name)))
                throw std::runtime_error(tr("Build did not install required programs"));
        transaction.commit();
        tools = runtime / "bin";
        config["runtime_dir"] =
            utf8(locations.portable ? fs::relative(tools, locations.config) : tools);
        writeJson(locations.config / "launcher.json", config);
    }
};
}  // namespace cane
