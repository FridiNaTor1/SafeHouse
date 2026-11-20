#include "disc.h"
#include "modnotice.h"
#include "splicediag.h"
#include "splicenames.h"
#include "worldparts.h"
#include "bis.h"
#include "cls.h"
#include "convert.h"

#include "../platform/brx_decompress.h"
#include "../platform/mod_package.h"
#include "../platform/platform.h"
#include "../platform/safe_paths.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <filesystem>

#include <nlohmann/json.hpp>

CDisc g_disc;
void (*g_pfnNoteKeyRecord)(const void* record64, KEYK keyk, int id, int lang) = nullptr;

namespace
{
constexpr uint32_t SECTOR = 2048;

uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t Le16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

#ifdef __SWITCH__
// (the Switch) The files read in pieces (the streams' files: music, dialog, movies, banks) kept open, a
// few at a time, rather than opened for each read: on the SD card an open costs most of a small read's time. Each file
// has its own lock, so a world's load and the sound thread's stream read apart; reads of one file take turns.
class COpenFiles
{
public:
    // cb bytes at ib (fewer at the file's end: *pcbRead); the file's size in *pcbFile. False: it cannot be opened.
    bool FRead(const std::string& path, uint64_t ib, void* pv, size_t cb, size_t* pcbRead, uint64_t* pcbFile)
    {
        const std::shared_ptr<ENT> e = Ent(path);
        *pcbRead = 0;
        if (!e) return false;
        std::lock_guard<std::mutex> lock(e->mtx);
        *pcbFile = e->cb;
        const size_t cbRead = size_t(std::min<uint64_t>(cb, ib < e->cb ? e->cb - ib : 0));
        if (!cbRead) return true;
        e->in.clear();
        e->in.seekg(static_cast<std::streamoff>(ib));
        e->in.read(static_cast<char*>(pv), static_cast<std::streamsize>(cbRead));
        *pcbRead = size_t(e->in.gcount());
        return true;
    }

private:
    struct ENT
    {
        std::mutex mtx;
        std::ifstream in;
        std::string path;
        uint64_t cb = 0, tick = 0;
    };
    std::shared_ptr<ENT> Ent(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        ++m_tick;
        for (const auto& e : m_aent)
            if (e->path == path)
            {
                e->tick = m_tick;
                return e;
            }
        auto e = std::make_shared<ENT>();
        e->in.open(cane::paths::ioPath(cane::path(path)), std::ios::binary);
        if (!e->in) return nullptr;
        e->in.seekg(0, std::ios::end);
        e->cb = uint64_t(e->in.tellg());
        e->path = path;
        e->tick = m_tick;
        // (the least recently read one closed; a reader holding it finishes first: shared)
        if (m_aent.size() >= kMax)
            m_aent.erase(std::min_element(m_aent.begin(), m_aent.end(), [](const auto& a, const auto& b) { return a->tick < b->tick; }));
        m_aent.push_back(e);
        return e;
    }
    static constexpr size_t kMax = 8;
    std::mutex m_mtx;
    uint64_t m_tick = 0;
    std::vector<std::shared_ptr<ENT>> m_aent;
};
COpenFiles s_openfiles;
#endif

uint32_t Crc32(const uint8_t* p, size_t cb)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < cb; ++i)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

#include "regionmaps.inc"
#include "regionlayouts.inc"

// The Sly 2 discs the port knows (their boot executables). Any other executable is refused: the port's tables are
// checked against these (tools/regionmap.py).
struct KNOWNDISC
{
    const char* szName;
    const char* szBoot;      // the executable's name in the root directory
    uint32_t cb, crc;
    DISCREGION region;
    const REGIONRANGE* amap;
    size_t cmap;
    const REGIONPATCH* apatch;  // the field offsets set back to the USA ones (the Japanese build's layouts)
    size_t cpatch;
    int iregionLanguages;       // the boot's region (0x1613D0: the USA build sets none: 0)
    uint32_t vaLanguageLabels;  // the title's "Triangle: Language" labels (the European build's 0x1DE1F8)
};
const KNOWNDISC s_adisc[] = {
    {"USA (SCUS-97316)", "SCUS_973.16", 2597048, 0xD67D6E1Eu, DISCREGION::Usa, nullptr, 0, nullptr, 0, 0, 0},
    {"Europe v1.00 (SCES-52529)", "SCES_525.29", 2626744, 0xEE0E8D27u, DISCREGION::Europe, s_amapPal100, std::size(s_amapPal100), nullptr, 0, 4, 0x2CAA08},
    {"Europe v2.01 (SCES-52529)", "SCES_525.29", 2626744, 0x61086863u, DISCREGION::Europe, s_amapPal201, std::size(s_amapPal201), nullptr, 0, 4, 0x2CAA08},
    {"Japan (SCPS-15090)", "SCPS_150.90", 2634552, 0xDA71B2AFu, DISCREGION::Japan, s_amapJapan, std::size(s_amapJapan), s_apatchJapan, std::size(s_apatchJapan), 5, 0},
    // (the Korean build has the Japanese build's class layouts: tools/regionlayouts.py gives it the same 2146 patches)
    {"Korea (SCKA-20044)", "SCKA_200.44", 2631992, 0xB984A000u, DISCREGION::Korea, s_amapKorea, std::size(s_amapKorea), s_apatchJapan, std::size(s_apatchJapan), 6, 0},
};

// A file of the image's root directory (ISO 9660): its sector and size.
bool FFindRootFile(const cane::DiscReader& read, const std::string& name, uint32_t& lba, uint32_t& cb)
{
    uint8_t pvd[SECTOR];
    if (!read(16ull * SECTOR, pvd, SECTOR) || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    const uint32_t lbaRoot = Le32(pvd + 156 + 2), cbRoot = Le32(pvd + 156 + 10);
    if (cbRoot == 0 || cbRoot > 64 * SECTOR) return false;
    std::vector<uint8_t> ab(cbRoot);
    if (!read(uint64_t(lbaRoot) * SECTOR, ab.data(), ab.size())) return false;
    for (size_t o = 0; o < ab.size();)
    {
        const uint8_t cbRec = ab[o];
        if (cbRec == 0) { o = (o / SECTOR + 1) * SECTOR; continue; }
        if (o + cbRec > ab.size() || cbRec < 34) break;
        std::string n(reinterpret_cast<const char*>(&ab[o + 33]), ab[o + 32]);
        if (const size_t semi = n.find(';'); semi != std::string::npos) n.resize(semi);
        if (n == name)
        {
            lba = Le32(&ab[o + 2]);
            cb = Le32(&ab[o + 10]);
            return true;
        }
        o += cbRec;
    }
    return false;
}

// SYSTEM.CNF's BOOT2 = cdrom0:\NAME;1 -> NAME
std::string SzBootFile(const cane::DiscReader& read)
{
    uint32_t lba = 0, cb = 0;
    if (!FFindRootFile(read, "SYSTEM.CNF", lba, cb) || cb > 4096) return "";
    std::string s(cb, '\0');
    if (!read(uint64_t(lba) * SECTOR, s.data(), cb)) return "";
    const size_t i = s.find("cdrom0:\\");
    if (i == std::string::npos) return "";
    const size_t j = s.find(';', i);
    return s.substr(i + 8, j == std::string::npos ? std::string::npos : j - i - 8);
}

uint32_t VaLookup(const REGIONRANGE* a, size_t c, uint32_t va)
{
    // the last range starting at or before va
    size_t lo = 0, hi = c;
    while (lo < hi)
    {
        const size_t mid = (lo + hi) / 2;
        if (a[mid].vaUsa <= va) lo = mid + 1;
        else hi = mid;
    }
    return lo == 0 ? va : uint32_t(int64_t(va) + a[lo - 1].dva);
}
}

FK FkFromRecord(const void* record64)
{
    const uint8_t* p = static_cast<const uint8_t*>(record64);
    FK fk;
    fk.isector = (Le32(p + 0x00) ^ Le32(p + 0x34)) + 0xCA0;
    fk.cb = Le32(p + 0x04) ^ Le32(p + 0x3C);
    fk.id = Le32(p + 0x08) ^ Le32(p + 0x38);
    return fk;
}

bool CDisc::FOpen(const std::string& path, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p = cane::path(path);
    if (fs::is_directory(p, ec) && cane::imageParts(p).empty()) return FOpenConverted(path, error);
    if (p.filename() == "extraction_manifest.json") return FOpenConverted(cane::utf8(p.parent_path()), error);
    // a disc image: the converter's (an .iso, a CHD or a split .iso), or the developer option
    std::unique_ptr<cane::DiscImage> pimage = cane::DiscImage::open(p, &error);
    if (!pimage) return false;
    const cane::DiscReader read = pimage->reader();
    const std::string szBoot = SzBootFile(read);
    uint32_t lba = 0, cb = 0;
    if (szBoot.empty() || !FFindRootFile(read, szBoot, lba, cb) || cb < 0x1000 || cb > 0x800000)
    {
        error = "cannot read " + path + " (no PS2 boot executable)";
        return false;
    }
    std::vector<uint8_t> elf(cb);
    if (!read(uint64_t(lba) * SECTOR, elf.data(), elf.size()))
    {
        error = "cannot read " + path;
        return false;
    }
    if (!FRecognize(szBoot, std::move(elf), path, error)) return false;
    m_apimage.clear();
    m_apimage.push_back(std::move(pimage));
    m_aszLanguage.clear();
    m_path = path;
    m_fConverted = false;
    return true;
}

// The converted files: extraction_manifest.json's files by their disc sector, the mods folder's files over them.
bool CDisc::FOpenConverted(const std::string& dir, std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path root = cane::path(dir);
    nlohmann::json m;
    try
    {
        std::ifstream in(cane::paths::ioPath(root / "extraction_manifest.json"));
        if (!in)
        {
            error = dir + " has no extraction_manifest.json: install Sly 2 again from the launcher";
            return false;
        }
        in >> m;
        if (m.value("game", "") != "sly2" || m.value("format", "") != "sly2-files")
        {
            error = dir + ": not a converted Sly 2 installation";
            return false;
        }
        if (m.value("format_version", 0) != SLY2_FILES_FORMAT && m.value("format_version", 0) != SLY2_FILES_FORMAT_WAV &&
            m.value("format_version", 0) != SLY2_FILES_FORMAT_PS2)
        {
            error = dir + ": installed with another version of the converter: install Sly 2 again from the launcher";
            return false;
        }
        m_szExe = m.at("executable").get<std::string>();
        m_aszLanguage.clear();
        if (m.contains("languages"))
            for (const auto& lang : m.at("languages")) m_aszLanguage.push_back(lang.get<std::string>());
        m_afile.clear();
        m_mpsectorWorldName.clear();
        // the mods folder's files (relative paths as the manifest spells them, "/" separated), each with its file: the
        // packages' (platform/mod_package.h: <mods>/Packages/<id>/files/, in load order, a later one's file winning),
        // then the loose folder's, then the command line's packages (the editor's play test)
        std::set<std::string> asetMod;
        std::map<std::string, std::string> mpModPath;
        m_adirModRoots.clear();
        auto addRoot = [&](const fs::path& dirRoot) {
            std::error_code ec;
            if (dirRoot.empty() || !fs::is_directory(dirRoot, ec)) return;
            m_adirModRoots.push_back(cane::utf8(dirRoot));
            for (const auto& e : fs::recursive_directory_iterator(dirRoot, fs::directory_options::skip_permission_denied, ec))
            {
                if (!e.is_regular_file()) continue;
#ifdef __SWITCH__
                // (the Switch: no canonical paths on the SD card's file system)
                const std::string rel = cane::utf8(e.path().lexically_relative(dirRoot).generic_u8string());
#else
                const std::string rel = cane::utf8(fs::relative(e.path(), dirRoot).generic_u8string());
#endif
                // (a package's own folder is not a mod of the installation: Packages/ in the loose folder)
                if (rel.rfind(std::string(cane::mods::kPackagesFolder) + "/", 0) == 0) continue;
                // (by the short names: a mod made against an installation with the names from before 2026-11-17
                // matches a new one, and the other way round: worldparts.h RelCanonical)
                const std::string key = RelCanonical(rel);
                asetMod.insert(key);
                mpModPath[key] = cane::utf8(e.path());
            }
        };
        m_aidPackage.clear();
        m_mpCustomTask.clear();
        m_mpCutsceneRule.clear();
        m_amodworld.clear();
        m_amapstate.clear();
        m_aentry.clear();
        m_aguest.erase(std::remove_if(m_aguest.begin(), m_aguest.end(), [](const cane::mods::Guest& g) { return g.id.rfind("cli-", 0) != 0; }), m_aguest.end());
        std::vector<cane::mods::Manifest> apkgWorlds;   // (their new worlds, map states and entries: after the files)
        auto addTasks = [&](const cane::mods::Manifest& pkg) {
            for (const cane::mods::CustomTasks& ct : cane::mods::customTasks(pkg))
                for (int oid : ct.tasks) m_mpCustomTask[{ct.world, oid}] = pkg.id;   // (a later package's listing wins)
            for (const cane::mods::CutsceneRule& r : cane::mods::cutsceneRules(pkg)) m_mpCutsceneRule[{r.world, r.script}] = {pkg.id, r};
        };
        const std::string region = m.value("region", "");
        const int filesFormat = m.value("format_version", 0);
        if (m_fDiscoverPackages && !m_dirMods.empty())
        {
            const cane::mods::Resolved r = cane::mods::discover(cane::path(m_dirMods) / cane::mods::kPackagesFolder, "sly2", region, filesFormat);
            for (const std::string& msg : r.messages) std::fprintf(stderr, "mods: package %s\n", msg.c_str());
            for (const cane::mods::Manifest& pkg : r.packages)
            {
                addRoot(pkg.filesDir());
                m_aidPackage.push_back(pkg.id);
                addTasks(pkg);
                apkgWorlds.push_back(pkg);
            }
        }
        if (!m_dirMods.empty()) addRoot(cane::path(m_dirMods));
        for (const std::string& dirPkg : m_adirPackagesExtra)
        {
            cane::mods::Manifest pkg;
            std::string err;
            if (!cane::mods::readManifest(cane::path(dirPkg), pkg, err)) { std::fprintf(stderr, "mods: package %s\n", err.c_str()); continue; }
            if (const std::string why = cane::mods::mismatch(pkg, "sly2", region, filesFormat); !why.empty())
            {
                std::fprintf(stderr, "mods: package %s: skipped, %s\n", pkg.id.c_str(), why.c_str());
                continue;
            }
            addRoot(pkg.filesDir());
            m_aidPackage.push_back(pkg.id);
            addTasks(pkg);
            apkgWorlds.push_back(pkg);
        }
        for (const std::string& id : m_aidPackage) std::printf("mods: package %s\n", id.c_str());
        // Music/ and Movies/ files also match by their number: "Music/28.wav" or "Music/28 My Song.wav" replaces
        // "Music/28 Paris.wav" (the names may change between converter versions and differ between discs); a movie's
        // sound tracks too ("Movies/French/2062.wav")
        auto keyOfNumber = [](const std::string& rel) -> std::string {
            const auto slash = rel.rfind('/');
            const std::string dir = slash == std::string::npos ? "" : rel.substr(0, slash);
            if (dir != "Music" && dir != "Movies" && !(dir.rfind("Movies/", 0) == 0 && dir.find('/', 7) == std::string::npos)) return "";
            const std::string name = rel.substr(slash + 1);
            const auto dot = name.rfind('.');
            std::size_t n = 0;
            while (n < name.size() && std::isdigit(static_cast<unsigned char>(name[n]))) ++n;
            if (n == 0 || dot == std::string::npos || (n < name.size() && name[n] != ' ' && name[n] != '.')) return "";
            std::string ext = name.substr(dot);
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            return dir + "/" + std::to_string(std::stoul(name.substr(0, std::min<std::size_t>(n, 9)))) + ext;
        };
        std::map<std::string, std::string> mpnumberMod;
        for (const std::string& rel : asetMod)
            if (const std::string k = keyOfNumber(rel); !k.empty()) mpnumberMod.emplace(k, rel);
        // the mods folder's file for an installed path: the same path, else the same number
        auto relModOf = [&](const std::string& relIn) -> std::string {
            const std::string rel = RelCanonical(relIn.rfind("@languages/", 0) == 0 ? "Languages/" + relIn.substr(11) : relIn);
            if (asetMod.count(rel)) return rel;
            if (const std::string k = keyOfNumber(rel); !k.empty())
                if (const auto it = mpnumberMod.find(k); it != mpnumberMod.end()) return it->second;
            return "";
        };
        auto withExt = [](const std::string& rel, const char* ext) {
            const auto dot = rel.rfind('.');
            return (dot == std::string::npos ? rel : rel.substr(0, dot)) + ext;
        };
        m_cmod = 0;
        m_mpsectorName.clear();
        // (an installation's languages in the shared languages folder, langstore.h: "@languages/French/...". Their mods
        // are Mods/Languages/French/..., as langstore.cpp takes them.)
        auto pathInstalled = [&](const std::string& rel) -> fs::path {
            if (rel.rfind("@languages/", 0) == 0) return cane::path(m_dirLanguages) / cane::path(rel.substr(11));
            return root / cane::path(rel);
        };
        for (const auto& f : m.at("files"))
        {
            const std::string rel = f.at("path").get<std::string>();
            if (rel.find("..") != std::string::npos) continue;
            const std::string kind = f.value("kind", "");
            if (f.contains("name")) m_mpsectorName[f.at("sector").get<uint32_t>()] = f.at("name").get<std::string>();
            FILEENT ent;
            ent.fDecompressed = f.value("decompressed", false);
            ent.rel = rel;
            std::string relMod = relModOf(rel);
            // (format 3) a world as its parts: its index; a whole world file in the mods folder (Mods/.../<World>.brx,
            // as before format 3) replaces it all, else each part the mods folder has replaces that part (FRavelWorld)
            if (kind == "world" && f.value("layout", "") == "parts")
            {
                ent.fParts = true;
                const std::string suffix = ".world.json";
                if (rel.size() > suffix.size() && rel.compare(rel.size() - suffix.size(), suffix.size(), suffix) == 0)
                    if (const std::string relBrx = relModOf(rel.substr(0, rel.size() - suffix.size()) + ".brx"); !relBrx.empty())
                    {
                        relMod = relBrx;
                        ent.fParts = false;
                    }
                if (ent.fParts)
                {
                    const std::string dirParts = RelCanonical(rel.substr(0, rel.rfind('/') + 1) + "World/");
                    for (auto it = asetMod.lower_bound(dirParts); it != asetMod.end() && it->compare(0, dirParts.size(), dirParts) == 0; ++it) ++m_cmod;
                }
            }
            // the other formats the game reads at the same path: a dialog line as WAV or FLAC, and the PS2 formats of
            // earlier installations (a VPK the music or ambience's WAV, a VAG a dialog line's, a PSS a movie's M2V and
            // sound): the game reads either (spiop.cpp, fmv.cpp)
            bool fLegacy = false;
            const std::vector<const char*> aext = kind == "music" || kind == "ambience" ? std::vector<const char*>{".wav", ".vpk"}
                                                : kind == "dialog" ? std::vector<const char*>{".wav", ".flac", ".vag"}
                                                : kind == "movie" ? std::vector<const char*>{".pss"} : std::vector<const char*>{};
            for (const char* ext : aext)
                if (relMod.empty() && withExt(rel, ext) != rel)
                {
                    relMod = relModOf(withExt(rel, ext));
                    fLegacy = !relMod.empty() && !std::strcmp(ext, ".pss");
                }
            if (!relMod.empty())
            {
                ent.path = mpModPath.at(relMod);
                ent.fMod = true;
                ++m_cmod;
            }
            else ent.path = cane::utf8(pathInstalled(rel));
            ent.pathInstalled = cane::utf8(pathInstalled(rel));
            // the file's parts: a movie's sound tracks ("audio": [{"track", "path"}]), a bank's samples ("samples":
            // [{"offset", "path"}]); each can be replaced on its own
            auto addParts = [&](const char* name, const char* key, const char* prefix) {
                if (fLegacy || !f.contains(name)) return;
                for (const auto& p : f.at(name))
                {
                    const std::string relPart = p.at("path").get<std::string>();
                    if (relPart.find("..") != std::string::npos) continue;
                    std::string relPartMod = relModOf(relPart);
                    for (const char* ext : {".wav", ".flac"})   // (a movie's sound track: either)
                        if (relPartMod.empty() && !std::strcmp(prefix, "audio")) relPartMod = relModOf(withExt(relPart, ext));
                    if (!relPartMod.empty()) ++m_cmod;
                    ent.mppart[prefix + std::to_string(p.at(key).get<uint32_t>())] =
                        relPartMod.empty() ? cane::utf8(pathInstalled(relPart)) : mpModPath.at(relPartMod);
                }
            };
            addParts("audio", "track", "audio");
            addParts("samples", "offset", "sample");
            if (kind == "world" && f.contains("name"))
            {
                m_mpsectorWorldName[f.at("sector").get<uint32_t>()] = f.at("name").get<std::string>();
                if (f.contains("copies"))
                    for (const auto& c : f.at("copies")) m_mpsectorWorldName[c.get<uint32_t>()] = f.at("name").get<std::string>();
            }
            if (f.contains("copies"))
                for (const auto& c : f.at("copies")) m_afile.emplace_back(c.get<uint32_t>(), ent);
            m_afile.emplace_back(f.at("sector").get<uint32_t>(), std::move(ent));
        }
        std::sort(m_afile.begin(), m_afile.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const cane::mods::Manifest& pkg : apkgWorlds) AddPackageWorlds(pkg);
        m_setMod = std::move(asetMod);
        m_mpModPath = std::move(mpModPath);
    }
    catch (const std::exception& e)
    {
        error = dir + "/extraction_manifest.json: " + e.what();
        return false;
    }
    std::vector<uint8_t> elf;
    {
        std::ifstream in(cane::paths::ioPath(root / cane::path(m_szExe)), std::ios::binary);
        if (!in)
        {
            error = dir + ": the executable " + m_szExe + " is missing";
            return false;
        }
        elf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (!FRecognize(m_szExe, std::move(elf), dir, error)) return false;
    m_path = dir;
    m_fConverted = true;
    if (m_cmod) std::printf("mods: %d files from %s%s\n", m_cmod, m_dirMods.c_str(), m_aidPackage.empty() ? "" : " and the packages");
    return true;
}

void CDisc::AddPackageWorlds(const cane::mods::Manifest& m)
{
    std::vector<std::string> aerr;
    for (const cane::mods::MapState& st : cane::mods::mapStates(m, &aerr)) m_amapstate.emplace_back(m.id, st);
    for (const cane::mods::Entry& e : cane::mods::entries(m, &aerr)) m_aentry.push_back(e);
    for (const cane::mods::Guest& g : cane::mods::guests(m, &aerr)) m_aguest.push_back(g);
    for (const cane::mods::NewWorld& w : cane::mods::newWorlds(m, &aerr))
    {
        if (IsectorOfWorldName(w.name))
        {
            aerr.push_back(m.id + ": the new world \"" + w.name + "\" has the name of a world already there: left out");
            continue;
        }
        MODWORLD mw;
        mw.package = m.id;
        mw.w = w;
        // (its sector and key from the package's id and the world's: the same whatever other packages load before it,
        // so a key kept anywhere (a save, an exit's redirect) names this world; numbered after the disc's records in the
        // packages' order, a world is addressed by name outside the process: --world NAME, --editor NAME)
        mw.isector = IsectorModWorldOf(m.id, w.id);
        FILEENT ent;
        ent.fParts = true;
        ent.fMod = true;
        ent.rel = w.index;
        ent.path = ent.pathInstalled = cane::utf8(m.filesDir() / cane::path(w.index));
        const auto it = std::lower_bound(m_afile.begin(), m_afile.end(), mw.isector, [](const auto& e, uint32_t s) { return e.first < s; });
        m_afile.insert(it, {mw.isector, std::move(ent)});
        m_mpsectorWorldName[mw.isector] = w.name;
        std::printf("mods: package %s: new world \"%s\" (from %s)\n", m.id.c_str(), w.name.c_str(), w.templateWorld.empty() ? "its own parts" : w.templateWorld.c_str());
        m_amodworld.push_back(std::move(mw));
    }
    for (const std::string& e : aerr) std::fprintf(stderr, "mods: package %s\n", e.c_str());
}

uint32_t CDisc::IsectorModWorldOf(const std::string& package, const std::string& id) const
{
    // FNV-1a of "package/id" into 2^19 slots from ISECTOR_MODWORLD (a slot taken by another world: the next free one)
    uint32_t h = 2166136261u;
    for (const char ch : package + "/" + id) h = (h ^ uint8_t(ch)) * 16777619u;
    uint32_t slot = h & 0x7FFFFu;
    for (;;)
    {
        const uint32_t isector = ISECTOR_MODWORLD + slot;
        if (std::none_of(m_amodworld.begin(), m_amodworld.end(), [&](const MODWORLD& w) { return w.isector == isector; })) return isector;
        slot = (slot + 1) & 0x7FFFFu;
    }
}

uint32_t CDisc::IsectorOfWorldName(const std::string& name) const
{
    for (const auto& [isector, n] : m_mpsectorWorldName)
        if (n == name) return isector;
    return 0;
}

std::string CDisc::SzWorldName(uint32_t isector) const
{
    const auto it = m_mpsectorWorldName.find(isector);
    return it != m_mpsectorWorldName.end() ? it->second : std::string();
}

bool CDisc::FRecognize(const std::string& szBoot, std::vector<uint8_t>&& elf, const std::string& where, std::string& error)
{
    const uint32_t cb = uint32_t(elf.size());
    const uint32_t crc = Crc32(elf.data(), elf.size());
    const KNOWNDISC* pdisc = nullptr;
    for (const KNOWNDISC& d : s_adisc)
        if (szBoot == d.szBoot && cb == d.cb && crc == d.crc) pdisc = &d;
    if (!pdisc || std::memcmp(elf.data(), "\x7F" "ELF", 4) != 0 || Le32(elf.data() + 0x18) != 0x100008)
    {
        error = where + " is not a supported Sly 2 disc (USA SCUS-97316, Europe SCES-52529 v1.00 / v2.01, Japan SCPS-15090, Korea SCKA-20044)";
        return false;
    }
    // the two loadable segments (the program; the IOP modules)
    const uint32_t phoff = Le32(elf.data() + 0x1C);
    const uint16_t phentsize = Le16(elf.data() + 0x2A), phnum = Le16(elf.data() + 0x2C);
    int cseg = 0;
    m_vaHi = 0;
    for (uint16_t i = 0; i < phnum && cseg < 2; ++i)
    {
        const uint8_t* ph = elf.data() + phoff + size_t(i) * phentsize;
        if (Le32(ph) != 1) continue;  // PT_LOAD
        m_aseg[cseg++] = {Le32(ph + 8), Le32(ph + 4), Le32(ph + 0x10)};
        if (cseg == 1) m_vaHi = Le32(ph + 8) + Le32(ph + 0x14);
    }
    if (cseg != 2)
    {
        error = where + ": unexpected executable layout";
        return false;
    }
    m_abElf = std::move(elf);
    m_szExe = szBoot;
    m_region = pdisc->region;
    m_szDisc = pdisc->szName;
    m_amap = pdisc->amap;
    m_iregionLanguages = pdisc->iregionLanguages;
    m_vaLanguageLabels = pdisc->vaLanguageLabels;
    m_cmap = pdisc->cmap;
    m_amapInverse.clear();
    for (size_t i = 0; i < m_cmap; ++i) m_amapInverse.push_back({uint32_t(int64_t(m_amap[i].vaUsa) + m_amap[i].dva), -m_amap[i].dva});
    std::sort(m_amapInverse.begin(), m_amapInverse.end(), [](const REGIONRANGE& a, const REGIONRANGE& b) { return a.vaUsa < b.vaUsa; });
    // the Japanese build's classes have other layouts: the executable's field offsets (the option descriptors, the
    // actors' part tables, ...) are set to the USA ones, which the port's objects use
    for (size_t i = 0; i < pdisc->cpatch; ++i)
    {
        const uint32_t va = VaRegion(pdisc->apatch[i].vaUsa);
        for (const SEG& seg : m_aseg)
            if (va >= seg.va && va + 4 <= seg.va + seg.cb)
                std::memcpy(m_abElf.data() + seg.offset + (va - seg.va), &pdisc->apatch[i].u, 4);
    }
    if (m_region != DISCREGION::Usa) std::printf("disc: %s\n", m_szDisc);
    return true;
}

bool CDisc::FReadRootFile(const std::string& name, std::vector<uint8_t>& ab) const
{
    if (m_fConverted) return false;
    uint32_t lba = 0, cb = 0;
    if (!FFindRootFile([this](uint64_t ib, void* pv, size_t cbRead) { return FReadImage(ib, pv, cbRead); }, name, lba, cb)) return false;
    ab.resize(cb);
    return FReadImage(uint64_t(lba) * SECTOR, ab.data(), ab.size());
}

bool CDisc::FReadImage(uint64_t ib, void* pv, std::size_t cb) const
{
    std::unique_ptr<cane::DiscImage> pimage;
    {
        std::lock_guard<std::mutex> lock(m_mtxImage);
        if (!m_apimage.empty())
        {
            pimage = std::move(m_apimage.back());
            m_apimage.pop_back();
        }
    }
    if (!pimage) pimage = cane::DiscImage::open(cane::path(m_path));
    if (!pimage) return false;
    const bool fRead = pimage->read(ib, pv, cb);
    std::lock_guard<std::mutex> lock(m_mtxImage);
    m_apimage.push_back(std::move(pimage));
    return fRead;
}

bool CDisc::FInstalledFile(uint32_t isector) const
{
    const FILEENT* pent = m_fConverted ? PentFind(isector) : nullptr;
    return pent && !pent->fMod;
}

const CDisc::FILEENT* CDisc::PentFind(uint32_t isector) const
{
    const auto it = std::lower_bound(m_afile.begin(), m_afile.end(), isector, [](const auto& e, uint32_t s) { return e.first < s; });
    return it != m_afile.end() && it->first == isector ? &it->second : nullptr;
}

const CDisc::FILEENT* CDisc::PentFindAny(uint32_t isector) const
{
    if (isector >= ISECTOR_EXTERNAL)
        return isector - ISECTOR_EXTERNAL < m_aentExternal.size() ? &m_aentExternal[isector - ISECTOR_EXTERNAL] : nullptr;
    return m_fConverted ? PentFind(isector) : nullptr;
}

uint32_t CDisc::IsectorExternal(const std::string& path)
{
    if (const auto it = m_mppathExternal.find(path); it != m_mppathExternal.end()) return it->second;
    FILEENT ent;
    ent.path = ent.pathInstalled = path;
    m_aentExternal.push_back(std::move(ent));
    const uint32_t isector = ISECTOR_EXTERNAL + uint32_t(m_aentExternal.size() - 1);
    m_mppathExternal.emplace(path, isector);
    return isector;
}

std::string CDisc::SzFileName(uint32_t isector) const
{
    const auto it = m_mpsectorName.find(isector);
    return it != m_mpsectorName.end() ? it->second : std::string();
}

uint32_t CDisc::VaRegion(uint32_t vaUsa) const { return m_amap ? VaLookup(m_amap, m_cmap, vaUsa) : vaUsa; }

uint32_t CDisc::VaUsa(uint32_t vaRegion) const
{
    return m_amap ? VaLookup(m_amapInverse.data(), m_amapInverse.size(), vaRegion) : vaRegion;
}

std::vector<uint8_t> CDisc::ReadFile(const FK& fk) const
{
    if (m_fConverted || fk.isector >= ISECTOR_EXTERNAL)
    {
        const FILEENT* pent = PentFindAny(fk.isector);
        if (!pent) throw std::runtime_error("no installed file for the key at sector " + std::to_string(fk.isector) + " (id " + std::to_string(fk.id) + ")");
#ifdef __SWITCH__
        // (the whole file in one read: its size first)
        size_t cbRead = 0;
        uint64_t cbFile = 0;
        if (!s_openfiles.FRead(pent->path, 0, nullptr, 0, &cbRead, &cbFile)) throw std::runtime_error("cannot read " + pent->path);
        std::vector<uint8_t> ab(static_cast<std::size_t>(cbFile));
        if (!s_openfiles.FRead(pent->path, 0, ab.data(), ab.size(), &cbRead, &cbFile) || cbRead != ab.size())
            throw std::runtime_error("cannot read " + pent->path);
        return ab;
#else
        std::ifstream in(cane::paths::ioPath(cane::path(pent->path)), std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + pent->path);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
#endif
    }
    std::vector<uint8_t> ab(fk.cb);
    if (!FReadImage(uint64_t(fk.isector) * SECTOR, ab.data(), ab.size()))
        throw std::runtime_error("disc read failed at sector " + std::to_string(fk.isector));
    return ab;
}

bool CDisc::FReadFileBytes(uint32_t isector, uint64_t ib, void* pv, std::size_t cb) const
{
    if (!m_fConverted && isector < ISECTOR_EXTERNAL) return FReadImage(uint64_t(isector) * SECTOR + ib, pv, cb);
    const FILEENT* pent = PentFindAny(isector);
    if (!pent)
    {
        std::fprintf(stderr, "disc: no installed file at sector 0x%X\n", isector);
        return false;
    }
#ifdef __SWITCH__
    std::memset(pv, 0, cb);
    size_t cbRead = 0;
    uint64_t cbFile = 0;
    if (!s_openfiles.FRead(pent->path, ib, pv, cb, &cbRead, &cbFile)) return false;
    return cbRead == std::size_t(std::min<uint64_t>(cb, ib < cbFile ? cbFile - ib : 0));
#else
    std::ifstream in(cane::paths::ioPath(cane::path(pent->path)), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const uint64_t cbFile = uint64_t(in.tellg());
    std::memset(pv, 0, cb);
    if (ib >= cbFile) return true;
    in.seekg(static_cast<std::streamoff>(ib));
    const std::size_t cbRead = std::size_t(std::min<uint64_t>(cb, cbFile - ib));
    in.read(static_cast<char*>(pv), static_cast<std::streamsize>(cbRead));
    return static_cast<size_t>(in.gcount()) == cbRead;
#endif
}

std::string CDisc::PathOf(uint32_t isector) const
{
    const FILEENT* pent = PentFindAny(isector);
    return pent ? pent->path : std::string();
}

std::string CDisc::PathPart(uint32_t isector, const std::string& key) const
{
    const FILEENT* pent = m_fConverted ? PentFind(isector) : nullptr;
    if (!pent) return "";
    const auto it = pent->mppart.find(key);
    return it != pent->mppart.end() ? it->second : std::string();
}

std::string CDisc::PathInstalled(uint32_t isector) const
{
    const FILEENT* pent = PentFindAny(isector);
    return pent ? pent->pathInstalled : std::string();
}

bool CDisc::FModded(uint32_t isector) const
{
    const FILEENT* pent = m_fConverted ? PentFind(isector) : nullptr;
    return pent && pent->fMod;
}

uint64_t CDisc::CbFile(uint32_t isector, uint64_t cbKey) const
{
    if (!m_fConverted && isector < ISECTOR_EXTERNAL) return cbKey;
    const FILEENT* pent = PentFindAny(isector);
    std::error_code ec;
    const uint64_t cb = pent ? std::filesystem::file_size(cane::paths::ioPath(cane::path(pent->path)), ec) : 0;
    return ec ? 0 : cb;
}

std::vector<uint8_t> CDisc::ReadWorld(const FK& fk) const { return ReadWorldParts(fk, false); }
std::vector<uint8_t> CDisc::ReadWorldReduced(const FK& fk) const
{
    static const bool s_fFull = std::getenv("SLY2N_KEPTFULL") && std::atoi(std::getenv("SLY2N_KEPTFULL")) != 0;
    return ReadWorldParts(fk, !s_fFull);
}

// which mod a modded file is ("package <id>": a package's file; else the mods folder's)
std::string CDisc::ModOfPath(const std::string& path) const
{
    const std::string pkg = std::string("/") + cane::mods::kPackagesFolder + "/";
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    if (const std::size_t i = p.find(pkg); i != std::string::npos)
    {
        const std::size_t i0 = i + pkg.size();
        return "package " + p.substr(i0, p.find('/', i0) - i0);
    }
    return "the mods folder";
}

void CDisc::ReportModPartError(const std::string& pathMod, const std::string& rel, const std::string& why) const
{
    // why: "line N:C: ..." for a script
    std::fprintf(stderr, "mods: %s: %s: %s (the installed part is used instead)\n", ModOfPath(pathMod).c_str(), pathMod.c_str(), why.c_str());
    std::string file = rel;
    if (const std::size_t w = file.rfind("World/"); w != std::string::npos) file = file.substr(w + 6);
    PostModNotice("Mod error (" + ModOfPath(pathMod) + "): " + file + " " + why.substr(0, 110));
}

void CDisc::ReportModScriptWarnings(const std::vector<std::string>& arelScripts, const std::set<std::string>& setSkip) const
{
    // the modded scripts of the world: their diagnostics (no world: names, calls, counts) in the log, once each
    static std::set<std::string> s_setDone;
    static std::mutex s_mutex;
    for (const std::string& rel : arelScripts)
    {
        const std::string key = RelCanonical(rel);
        if (setSkip.count(key)) continue;
        const auto it = m_mpModPath.find(key);
        if (it == m_mpModPath.end()) continue;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_setDone.insert(it->second).second) continue;
        }
        std::ifstream in(cane::paths::ioPath(cane::path(it->second)), std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SPLICEDIAGCTX ctx;
        std::vector<SPLICEISSUE> adiag;
        SpliceDiagnose(text, ctx, adiag);
        int ce = 0, cw = 0, ch = 0;
        CountSpliceDiags(adiag, ce, cw, ch);
        std::printf("mods: %s: %s: scripts loaded%s (%d errors, %d warnings)\n", ModOfPath(it->second).c_str(), it->second.c_str(),
                    FSpliceTextReadable(text) ? " with their names" : "", ce, cw);
        for (const SPLICEISSUE& d : adiag)
            if (d.sev != SPLICESEV_Hint) std::printf("mods:   %s\n", StrSpliceDiag(d).c_str());
    }
}

[[noreturn]] void FailDamagedInstall(const std::string& why)
{
    // (the launcher shows a failed game's last line that reads as an error, with the exit code: launcher/backend.h
    // explainedProcess. Nothing of the run is left to save: the world did not load.)
    const std::string msg = "error: Sly 2's installation is damaged or incomplete (" + why +
                            "). Install Sly 2 again from your disc in the launcher (Re-extract)";
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", msg.c_str());
    std::fflush(stderr);
    std::_Exit(3);
}

std::vector<uint8_t> CDisc::ReadWorldParts(const FK& fk, bool fReduced) const
{
    // (format 3) a world's parts put back together (worldparts.h): each part the mods folder's, else the installed one
    if (const FILEENT* pent = m_fConverted ? PentFind(fk.isector) : nullptr; pent && pent->fParts)
    {
        // (an installed world that cannot be put back together: a part missing or cut short, an index that does not
        // parse: an install that did not finish, a copy or a disk error. The game cannot go on with that world:
        // DAMAGEDINSTALL, which main.cpp's terminate handler turns into the "install again" error and exit code 3, not
        // an abort the player sees as a crash)
        auto damaged = [&](const std::string& why) {
            return DAMAGEDINSTALL(pent->fMod ? "a mod's level " + pent->rel + " did not load: " + why
                                             : "the installed level " + pent->rel + " did not load: " + why);
        };
        nlohmann::json index;
        {
            std::ifstream in(cane::paths::ioPath(cane::path(pent->path)));
            if (!in) throw damaged("cannot read " + pent->path);
            index = nlohmann::json::parse(in, nullptr, false);
            if (!index.is_object()) throw damaged("its index is not readable (" + pent->path + ")");
        }
        const std::string relDir = pent->rel.substr(0, pent->rel.rfind('/') == std::string::npos ? 0 : pent->rel.rfind('/'));
        std::vector<uint8_t> ab;
        std::string error;
        // (a mod's new world, mod_worlds.h: its parts marked "template" are the template world's)
        if (index.contains("template"))
            if (!FResolveTemplateParts(index, [&](const std::string& name, nlohmann::json& tpl, std::string& relDirTpl) {
                    const FILEENT* pt = PentFind(IsectorOfWorldName(name));
                    if (!pt || !pt->fParts || pt == pent) return false;
                    std::ifstream in(cane::paths::ioPath(cane::path(pt->path)));
                    if (!in) return false;
                    in >> tpl;
                    relDirTpl = pt->rel.substr(0, pt->rel.rfind('/') == std::string::npos ? 0 : pt->rel.rfind('/'));
                    return true;
                }, error))
                throw std::runtime_error(pent->rel + ": " + error);
        const std::filesystem::path root = cane::path(m_path);
        const std::set<int> setDrop = fReduced ? SetLevelGroups(index) : std::set<int>();
        const PFNKEEPOBJ pfnKeep = setDrop.empty() ? PFNKEEPOBJ() : PFNKEEPOBJ([](const WORLDOBJREC& r) { return FCidDerivedFrom(r.cid, 6); });
        if (m_pfnPartRead)
        {
            if (!FRavelWorldFrom(index, m_pfnPartRead, ab, error, pfnKeep, setDrop.empty() ? nullptr : &setDrop, relDir))
                throw std::runtime_error(pent->rel + ": " + error);
            return ab;
        }
        std::set<std::string> setSkip;   // the mods' parts left out (they did not compile)
        bool fModPart = false;           // (the last ravel took a part from the mods folder)
        auto ravel = [&](bool fMods) {
            fModPart = false;
            return FRavelWorld(index, relDir, [&](const std::string& rel) {
                const std::string key = RelCanonical(rel);
                if (fMods && !setSkip.count(key))
                    if (const auto it = m_mpModPath.find(key); it != m_mpModPath.end())
                    {
                        fModPart = true;
                        return it->second;
                    }
                return cane::utf8(root / cane::path(rel));
            }, ab, error, pfnKeep, setDrop.empty() ? nullptr : &setDrop);
        };
        bool fOk = ravel(true);
        // (a mod's part that does not compile, a script's typo: said (the mod, the file, the line and why) in the log
        // and over the game, and that part loads as installed; the mod's other parts stay)
        for (int ctry = 0; !fOk && ctry < 32; ++ctry)
        {
            const std::string relBad = error.substr(0, error.find(": "));
            const std::string key = RelCanonical(relBad);
            const auto it = m_mpModPath.find(key);
            if (it == m_mpModPath.end() || setSkip.count(key)) break;
            setSkip.insert(key);
            ReportModPartError(it->second, relBad, error.size() > relBad.size() + 2 ? error.substr(relBad.size() + 2) : error);
            fOk = ravel(true);
        }
        if (!fOk)
        {
            // (no mod part in it: the installed files themselves; else without the mods, as installed)
            if (!fModPart) throw damaged(error);
            std::fprintf(stderr, "mods: %s: %s: the world loads without its mods\n", pent->rel.c_str(), error.c_str());
            PostModNotice("A mod of " + pent->rel.substr(pent->rel.rfind('/') + 1) + " did not load: " + error.substr(0, 90));
            if (!ravel(false)) throw damaged(error);
        }
        // (the installed parts alone, the whole world: the index's size, else a part was cut short or replaced)
        if (!fModPart && setDrop.empty() && !index.contains("template") && index.contains("size") &&
            ab.size() != index.value("size", std::size_t(0)))
            throw damaged("its parts make " + std::to_string(ab.size()) + " bytes, not " + std::to_string(index.value("size", std::size_t(0))));
        if (std::getenv("SLY2N_WORLDHASH")) std::fprintf(stderr, "world bytes: %s %zu fnv %s\n", pent->rel.c_str(), ab.size(), StrFnv64(ab.data(), ab.size()).c_str());
        if (fOk)
        {
            std::vector<std::string> arel;
            const std::string prefix = relDir.empty() ? std::string() : relDir + "/";
            for (const auto& p : index.at("parts"))
                if (p.value("format", "") == "splice-text" && p.contains("file")) arel.push_back(prefix + p.at("file").get<std::string>());
            ReportModScriptWarnings(arel, setSkip);
        }
        return ab;
    }
    // (a whole world file, formats 1 and 2: one that cannot be read or decompressed, from the installation itself, is
    // a damaged installation too)
    try
    {
        std::vector<uint8_t> ab = ReadFile(fk);
        if (m_fConverted)
            if (const FILEENT* pent = PentFind(fk.isector); pent && pent->fDecompressed) return ab;
        return cane::decompressBrx(ab);
    }
    catch (const DAMAGEDINSTALL&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        if (!FInstalledFile(fk.isector)) throw;
        throw DAMAGEDINSTALL("the installed level " + PentFind(fk.isector)->rel + " did not load: " + e.what());
    }
}

namespace
{
// SLY2_ELFLOG=file: every executable range the port reads (USA addresses), for tools/regioncheck.py
void LogElfRead(uint32_t va, uint32_t cb)
{
    static FILE* s_pf = [] { const char* sz = std::getenv("SLY2_ELFLOG"); return sz ? std::fopen(sz, "a") : nullptr; }();
    if (!s_pf) return;
    static std::set<std::pair<uint32_t, uint32_t>> s_set;
    if (s_set.insert({va, cb}).second) { std::fprintf(s_pf, "%06X %X\n", va, cb); std::fflush(s_pf); }
}
}

const uint8_t* CDisc::PbElf(uint32_t va, uint32_t cb) const
{
    LogElfRead(va, cb);
    va = VaRegion(va);
    for (const SEG& seg : m_aseg)
        if (va >= seg.va && va + cb <= seg.va + seg.cb) return m_abElf.data() + seg.offset + (va - seg.va);
    throw std::out_of_range("address outside the executable's data");
}

const uint8_t* CDisc::PbElfOwn(uint32_t va, uint32_t cb) const
{
    for (const SEG& seg : m_aseg)
        if (va >= seg.va && va + cb <= seg.va + seg.cb) return m_abElf.data() + seg.offset + (va - seg.va);
    throw std::out_of_range("address outside the executable's data");
}

uint32_t CDisc::U32Elf(uint32_t va) const
{
    const uint32_t w = Le32(PbElf(va, 4));
    // (a float or a count in this range would be a denormal or above 1M: the tables have none; a multiple of
    // 0x10000 is a packed value, e.g. the option descriptors' 0x2B0000 / 0x230000, not an address)
    if (m_amap && w >= m_aseg[0].va && w < m_vaHi && (w & 0xFFFF) != 0) return VaUsa(w);
    return w;
}

std::string CDisc::StrElf(uint32_t va) const
{
    const char* p = reinterpret_cast<const char*>(PbElf(va, 1));
    return std::string(p, strnlen(p, 256));
}

std::vector<std::pair<uint32_t, FK>> CDisc::ElfKeys() const
{
    std::vector<std::pair<uint32_t, FK>> keys;
    for (uint32_t va = 0x2A3268; va < 0x2A39E8; va += 0x40) keys.emplace_back(va, FkFromRecord(PbElf(va, 0x40)));
    return keys;
}
