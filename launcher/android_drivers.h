#pragma once
// Settings > Launcher > Vulkan driver (Android): importing and choosing a custom Vulkan driver package (Mesa Turnip
// for Adreno GPUs, as emulators offer it), which every game's Vulkan loader then uses through libadrenotools
// (platform/android/vulkan_driver.h has the files, the game side and the fail-safe rules).
//
// A package is a .zip with meta.json ({"name", "description", "author", "driverVersion", "libraryName", "minApi", ...};
// the adrenotools package format, e.g. the Turnip builds emulators use) and the driver library it names, at its top
// level or in one folder. It is unpacked into drivers/<id>/ (id: from its name and version, made safe as a folder name).
#include <algorithm>
#include <string>
#include <vector>

#include <miniz.h>

#include "backend.h"
#include "android/vulkan_driver.h"
#include "l10n.h"   // (N_: the reasons are translated where they are shown)

namespace cane::android::vkdriver {

struct Package {
    std::string id, name, version, description, library;
};

inline std::string safeId(const std::string& text) {
    std::string id;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') id += char(c);
        else if (!id.empty() && id.back() != '-') id += '-';
        if (id.size() >= 64) break;
    }
    while (!id.empty() && (id.back() == '-' || id.back() == '.')) id.pop_back();
    while (!id.empty() && id.front() == '.') id.erase(0, 1);
    return id.empty() ? std::string("driver") : id;
}

inline std::vector<Package> installed() {
    std::vector<Package> out;
    std::error_code ec;
    for (fs::directory_iterator it(root(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || it->path().filename().string().front() == '.') continue;
        const json meta = readJson(it->path() / "meta.json");
        Package p;
        p.id = it->path().filename().string();
        p.name = meta.value("name", p.id);
        p.version = meta.value("driverVersion", std::string{});
        p.description = meta.value("description", std::string{});
        p.library = meta.value("libraryName", std::string{});
        if (p.library.empty() || p.library.find('/') != std::string::npos || !fs::is_regular_file(it->path() / p.library, ec)) continue;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const Package& a, const Package& b) { return a.name < b.name; });
    return out;
}

inline std::string label(const Package& p) { return p.version.empty() ? p.name : p.name + " " + p.version; }

// Unpacks a picked .zip (NativeBridge.importDriver copied it into drivers/), removes the .zip, and returns the package.
// Throws with a short reason (not a driver package, too new for this Android, damaged).
inline Package importZip(const fs::path& zip, int apiLevel) {
    struct Remove {
        fs::path path;
        ~Remove() { std::error_code ec; fs::remove_all(path, ec); }
    } removeZip{zip};
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!mz_zip_reader_init_file(&archive, utf8(zip).c_str(), 0)) throw std::runtime_error(tr("The file is not a .zip driver package"));
    struct End {
        mz_zip_archive* a;
        ~End() { mz_zip_reader_end(a); }
    } end{&archive};
    // meta.json at the top or in one folder: that folder is the package's root
    std::string prefix;
    int metaIndex = -1;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&archive, i, &st)) continue;
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        const size_t slash = name.rfind('/');
        const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        const std::string dir = slash == std::string::npos ? std::string{} : name.substr(0, slash + 1);
        if (base == "meta.json" && std::count(dir.begin(), dir.end(), '/') <= 1 && (metaIndex < 0 || dir.size() < prefix.size()))
            metaIndex = int(i), prefix = dir;
    }
    if (metaIndex < 0) throw std::runtime_error(tr("This .zip has no meta.json: it is not a Vulkan driver package"));
    size_t metaSize = 0;
    void* metaData = mz_zip_reader_extract_to_heap(&archive, mz_uint(metaIndex), &metaSize, 0);
    if (!metaData) throw std::runtime_error(tr("The driver package is damaged"));
    const json meta = json::parse(std::string(static_cast<const char*>(metaData), metaSize), nullptr, false);
    mz_free(metaData);
    if (!meta.is_object()) throw std::runtime_error(tr("The driver package's meta.json is damaged"));
    Package p;
    p.name = meta.value("name", std::string("Vulkan driver"));
    p.version = meta.value("driverVersion", std::string{});
    p.description = meta.value("description", std::string{});
    p.library = meta.value("libraryName", std::string{});
    if (p.library.empty() || p.library.find('/') != std::string::npos || p.library.size() < 4 || p.library.substr(p.library.size() - 3) != ".so")
        throw std::runtime_error(tr("The driver package's meta.json names no driver library"));
    const int minApi = meta.contains("minApi") && meta.at("minApi").is_number_integer() ? meta.at("minApi").get<int>() : 0;
    if (apiLevel > 0 && minApi > apiLevel)
        throw std::runtime_error(trf("This driver needs Android API level %d; this device has %d", minApi, apiLevel));
    p.id = safeId(label(p));
    const fs::path staging = root() / (".import-" + p.id);
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    bool library = false;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&archive, i, &st) || st.m_is_directory) continue;
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.rfind(prefix, 0) != 0) continue;
        name = name.substr(prefix.size());
        if (name.empty() || name.find('/') != std::string::npos) continue;   // (the package's files are flat)
        const fs::path out = safeAssetPath(staging, name);
        if (!mz_zip_reader_extract_to_file(&archive, i, utf8(out).c_str(), 0)) {
            fs::remove_all(staging, ec);
            throw std::runtime_error(trf("Cannot unpack %s (the package is damaged or the storage is full)", name.c_str()));
        }
        if (name == p.library) library = true;
    }
    if (!library) {
        fs::remove_all(staging, ec);
        throw std::runtime_error(trf("The driver package has no %s", p.library.c_str()));
    }
    const fs::path target = root() / p.id;
    fs::remove_all(target, ec);
    fs::rename(staging, target, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        throw std::runtime_error(tr("Cannot store the driver package"));
    }
    return p;
}

inline void remove(const std::string& id) {
    if (id.empty() || id.find('/') != std::string::npos || id.front() == '.') return;
    if (selection().id == id) select({});
    std::error_code ec;
    fs::remove_all(root() / id, ec);
}

// Which Adreno generations a custom driver (Mesa Turnip) is known to drive: data by GPU family, so a generation is
// added or marked working here and nowhere else. `reason` is the launcher's (English, translated by tr) warning shown
// under the driver row for one that is not; the option stays available there (driver builds are updated often).
struct AdrenoFamily { int generation; bool turnip; const char* reason; };
// 6xx / 7xx: Turnip's supported Adrenos (the Quest 3S's 740 among them). 8xx: tested 2026-11-24 on the S26 Ultra's
// Adreno 840 with the only Turnip build for it (K11MCH1's "Turnip A8XX Draft", Mesa 26.0.0-devel): it loads and the game
// runs, but nothing is drawn (Sly 3's near-clip check: every pixel off; a black screen).
inline constexpr AdrenoFamily kAdrenoFamilies[] = {
    {6, true, ""},
    {7, true, ""},
    {8, false, N_("Mesa Turnip does not draw on Adreno 8xx GPUs yet (the picture stays black).")},
};
// The generation in a GL / Vulkan renderer name ("Adreno (TM) 840" -> 8); 0 when it names no Adreno.
inline int adrenoGeneration(const std::string& gpu) {
    const size_t at = gpu.find("Adreno");
    if (at == std::string::npos) return 0;
    for (size_t i = at + 6; i < gpu.size(); ++i)
        if (gpu[i] >= '1' && gpu[i] <= '9') {
            size_t digits = 0;
            while (i + digits < gpu.size() && gpu[i + digits] >= '0' && gpu[i + digits] <= '9') ++digits;
            return digits == 3 ? gpu[i] - '0' : 0;
        }
    return 0;
}
// Why custom drivers may not work on this GPU ("" when they are known to): an Adreno generation Turnip cannot drive, older
// than 6xx, or not known yet.
inline std::string blockedReason(const std::string& gpu) {
    const int generation = adrenoGeneration(gpu);
    if (generation == 0) return "";   // (no Adreno named: the rows are hidden anyway)
    for (const auto& family : kAdrenoFamilies)
        if (family.generation == generation) return family.turnip ? "" : family.reason;
    return generation < 6 ? N_("Mesa Turnip needs an Adreno 6xx GPU or newer.") : N_("Mesa Turnip is not known to work on this Adreno GPU yet.");
}

}  // namespace cane::android::vkdriver
