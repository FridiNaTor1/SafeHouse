#include "disc.h"
#include "bis.h"
#include "convert.h"
#include "game/sly3.h"

#include "../platform/brx_decompress.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <cstdio>
#include <cstring>
#include <iterator>
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

bool ReadAt(const std::string& path, uint64_t offset, void* data, size_t cb)
{
    std::ifstream in(cane::path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(cb));
    return static_cast<size_t>(in.gcount()) == cb;
}

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

// The Sly 3 disc the port knows (its boot executable; game/sly3.h). Any other executable is refused: the port's
// tables are the USA executable's.
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
    {"USA (SCUS-97464)", sly3::BOOT_FILE, sly3::ELF_SIZE, sly3::ELF_CRC32, DISCREGION::Usa, nullptr, 0, nullptr, 0, 0, 0},
};

// A file of the image's root directory (ISO 9660): its sector and size.
bool FFindRootFile(const std::string& path, const std::string& name, uint32_t& lba, uint32_t& cb)
{
    uint8_t pvd[SECTOR];
    if (!ReadAt(path, 16ull * SECTOR, pvd, SECTOR) || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    const uint32_t lbaRoot = Le32(pvd + 156 + 2), cbRoot = Le32(pvd + 156 + 10);
    if (cbRoot == 0 || cbRoot > 64 * SECTOR) return false;
    std::vector<uint8_t> ab(cbRoot);
    if (!ReadAt(path, uint64_t(lbaRoot) * SECTOR, ab.data(), ab.size())) return false;
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
std::string SzBootFile(const std::string& path)
{
    uint32_t lba = 0, cb = 0;
    if (!FFindRootFile(path, "SYSTEM.CNF", lba, cb) || cb > 4096) return "";
    std::string s(cb, '\0');
    if (!ReadAt(path, uint64_t(lba) * SECTOR, s.data(), cb)) return "";
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
    if (fs::is_directory(p, ec)) return FOpenConverted(path, error);
    if (p.filename() == "extraction_manifest.json") return FOpenConverted(cane::utf8(p.parent_path()), error);
    // a disc image (the developer option)
    const std::string szBoot = SzBootFile(path);
    uint32_t lba = 0, cb = 0;
    if (szBoot.empty() || !FFindRootFile(path, szBoot, lba, cb) || cb < 0x1000 || cb > 0x800000)
    {
        error = "cannot read " + path + " (no PS2 boot executable)";
        return false;
    }
    std::vector<uint8_t> elf(cb);
    if (!ReadAt(path, uint64_t(lba) * SECTOR, elf.data(), elf.size()))
    {
        error = "cannot read " + path;
        return false;
    }
    if (!FRecognize(szBoot, std::move(elf), path, error)) return false;
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
        std::ifstream in(root / "extraction_manifest.json");
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
        if (m.value("format_version", 0) != SLY2_FILES_FORMAT && m.value("format_version", 0) != SLY2_FILES_FORMAT_PS2)
        {
            error = dir + ": installed with another version of the converter: install Sly 2 again from the launcher";
            return false;
        }
        m_szExe = m.at("executable").get<std::string>();
        m_afile.clear();
        m_mpsectorWorldName.clear();
        // the mods folder's files (relative paths as the manifest spells them, "/" separated)
        std::set<std::string> asetMod;
        if (!m_dirMods.empty() && fs::is_directory(cane::path(m_dirMods)))
            for (const auto& e : fs::recursive_directory_iterator(cane::path(m_dirMods), fs::directory_options::skip_permission_denied))
                if (e.is_regular_file()) asetMod.insert(cane::utf8(fs::relative(e.path(), cane::path(m_dirMods)).generic_u8string()));
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
        auto relModOf = [&](const std::string& rel) -> std::string {
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
        for (const auto& f : m.at("files"))
        {
            const std::string rel = f.at("path").get<std::string>();
            if (rel.find("..") != std::string::npos) continue;
            const std::string kind = f.value("kind", "");
            FILEENT ent;
            ent.fDecompressed = f.value("decompressed", false);
            std::string relMod = relModOf(rel);
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
                ent.path = cane::utf8(cane::path(m_dirMods) / cane::path(relMod));
                ent.fMod = true;
                ++m_cmod;
            }
            else ent.path = cane::utf8(root / cane::path(rel));
            ent.pathInstalled = cane::utf8(root / cane::path(rel));
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
                        relPartMod.empty() ? cane::utf8(root / cane::path(relPart)) : cane::utf8(cane::path(m_dirMods) / cane::path(relPartMod));
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
    }
    catch (const std::exception& e)
    {
        error = dir + "/extraction_manifest.json: " + e.what();
        return false;
    }
    std::vector<uint8_t> elf;
    {
        std::ifstream in(root / cane::path(m_szExe), std::ios::binary);
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
    if (m_cmod) std::printf("mods: %d files from %s\n", m_cmod, m_dirMods.c_str());
    return true;
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
        error = where + " is not a supported Sly 3 disc (USA SCUS-97464)";
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
    if (!FFindRootFile(m_path, name, lba, cb)) return false;
    ab.resize(cb);
    return ReadAt(m_path, uint64_t(lba) * SECTOR, ab.data(), ab.size());
}

const CDisc::FILEENT* CDisc::PentFind(uint32_t isector) const
{
    const auto it = std::lower_bound(m_afile.begin(), m_afile.end(), isector, [](const auto& e, uint32_t s) { return e.first < s; });
    return it != m_afile.end() && it->first == isector ? &it->second : nullptr;
}

uint32_t CDisc::VaRegion(uint32_t vaUsa) const { return m_amap ? VaLookup(m_amap, m_cmap, vaUsa) : vaUsa; }

uint32_t CDisc::VaUsa(uint32_t vaRegion) const
{
    return m_amap ? VaLookup(m_amapInverse.data(), m_amapInverse.size(), vaRegion) : vaRegion;
}

std::vector<uint8_t> CDisc::ReadFile(const FK& fk) const
{
    if (m_fConverted)
    {
        const FILEENT* pent = PentFind(fk.isector);
        if (!pent) throw std::runtime_error("no installed file for the key at sector " + std::to_string(fk.isector) + " (id " + std::to_string(fk.id) + ")");
        std::ifstream in(cane::path(pent->path), std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + pent->path);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<uint8_t> ab(fk.cb);
    if (!ReadAt(m_path, uint64_t(fk.isector) * SECTOR, ab.data(), ab.size()))
        throw std::runtime_error("disc read failed at sector " + std::to_string(fk.isector));
    return ab;
}

bool CDisc::FReadFileBytes(uint32_t isector, uint64_t ib, void* pv, std::size_t cb) const
{
    if (!m_fConverted) return ReadAt(m_path, uint64_t(isector) * SECTOR + ib, pv, cb);
    const FILEENT* pent = PentFind(isector);
    if (!pent)
    {
        std::fprintf(stderr, "disc: no installed file at sector 0x%X\n", isector);
        return false;
    }
    std::ifstream in(cane::path(pent->path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const uint64_t cbFile = uint64_t(in.tellg());
    std::memset(pv, 0, cb);
    if (ib >= cbFile) return true;
    in.seekg(static_cast<std::streamoff>(ib));
    const std::size_t cbRead = std::size_t(std::min<uint64_t>(cb, cbFile - ib));
    in.read(static_cast<char*>(pv), static_cast<std::streamsize>(cbRead));
    return static_cast<size_t>(in.gcount()) == cbRead;
}

std::string CDisc::PathOf(uint32_t isector) const
{
    const FILEENT* pent = m_fConverted ? PentFind(isector) : nullptr;
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
    const FILEENT* pent = m_fConverted ? PentFind(isector) : nullptr;
    return pent ? pent->pathInstalled : std::string();
}

bool CDisc::FModded(uint32_t isector) const
{
    const FILEENT* pent = m_fConverted ? PentFind(isector) : nullptr;
    return pent && pent->fMod;
}

uint64_t CDisc::CbFile(uint32_t isector, uint64_t cbKey) const
{
    if (!m_fConverted) return cbKey;
    const FILEENT* pent = PentFind(isector);
    std::error_code ec;
    const uint64_t cb = pent ? std::filesystem::file_size(cane::path(pent->path), ec) : 0;
    return ec ? 0 : cb;
}

std::vector<uint8_t> CDisc::ReadWorld(const FK& fk) const
{
    std::vector<uint8_t> ab = ReadFile(fk);
    if (m_fConverted)
        if (const FILEENT* pent = PentFind(fk.isector); pent && pent->fDecompressed) return ab;
    return cane::decompressBrx(ab);
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
    // orig3: the world keys (0x2EDFD8, 40; world 1 sampler_menu's is empty) and the movie keys (0x3179F8, 23; the
    // low bits of a movie key's cb are flags: game/sly3.h), each file once
    std::vector<std::pair<uint32_t, FK>> keys;
    for (int i = 0; i < sly3::CWORLD; ++i)
    {
        const FK fk = FkWorld(i);
        if (fk.cb) keys.emplace_back(sly3::VA_WORLD_KEYS + sly3::CB_KEY * uint32_t(i), fk);
    }
    for (int i = 0; i < sly3::CMOVIE_KEY; ++i)
    {
        const uint32_t va = sly3::VA_MOVIE_KEYS + sly3::CB_KEY * uint32_t(i);
        FK fk = FkFromRecord(PbElf(va, sly3::CB_KEY));
        fk.cb &= ~sly3::CB_MOVIE_FLAGS;
        bool fDup = false;
        for (const auto& [vaOther, fkOther] : keys) fDup |= fkOther.isector == fk.isector;
        if (fk.cb && !fDup) keys.emplace_back(va, fk);
    }
    return keys;
}

FK CDisc::FkWorld(int iworld) const
{
    if (iworld < 0 || iworld >= sly3::CWORLD) return FK{};
    const uint8_t* pb = PbElf(sly3::VA_WORLD_KEYS + sly3::CB_KEY * uint32_t(iworld), sly3::CB_KEY);
    bool fEmpty = true;
    for (uint32_t i = 0; i < sly3::CB_KEY; ++i) fEmpty &= pb[i] == 0;
    return fEmpty ? FK{} : FkFromRecord(pb);
}

std::string CDisc::SzWorldElf(int iworld) const
{
    if (iworld < 0 || iworld >= sly3::CWORLD) return "";
    return StrElf(U32Elf(sly3::VA_WORLD_NAMES + 4u * uint32_t(iworld)));
}

int CDisc::IepisodeOfWorld(int iworld) const
{
    if (iworld < 0 || iworld >= sly3::CWORLD) return -1;
    return int(U32Elf(sly3::VA_WORLD_EPISODES + 4u * uint32_t(iworld)));
}

int CDisc::IworldFromName(const std::string& name) const
{
    for (int i = 0; i < sly3::CWORLD; ++i)
        if (SzWorldElf(i) == name) return i;
    return -1;
}
