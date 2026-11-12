#pragma once
// The texture cache of the games' replacement textures (texture mods and texture packs: Sly2-Release texmod.cpp,
// notes/TEXCACHE_2026-11-10.md). A replacement is a PNG that has to be read, inflated, flipped and have its alpha
// halved before every use; the cache keeps the result (the texels as the renderer uploads them: RGBA8, rows bottom-up,
// alpha in the PS2's range) so a later load is one read and an LZ4 decode. The cache is only ever a copy: any file of
// it can be deleted at any time and is made again from the PNGs.
//
// One pack per source (a Mods/Textures folder, a texture pack's Textures folder or its Textures.pak:
// platform/texture_pak.h), two files in the cache folder, named by the source's path:
//   <id>.texpack    the texels, record after record, only ever appended to:
//                   header (16 bytes): "SHTEXPK1", u32 version, u32 0
//                   record: u32 'TXRC', u16 name length, u8 encoding (0 raw, 1 LZ4), u8 0, u32 width, u32 height,
//                           u32 payload bytes, u32 0, u64 source size, i64 source time, u64 payload sum, u64 header
//                           sum (of the 48 bytes before it and the name); the name; the payload
//   <id>.texindex   where each record is, so the pack is not walked at every start: "SHTEXIX1", u32 version, u32 entry
//                   count, u64 pack bytes covered, u64 dead bytes, u16 source path length + path, the entries (the
//                   record's fields and its offset), u64 sum of everything before. Replaced atomically.
// (All integers little-endian.)
//
// An entry is used only for the PNG it was made from: the same name, size and modification time (a PNG inside a
// Textures.pak: its size, and a sum of the pak file's size, time and the PNG's place in it; no file is asked per
// texture). A changed PNG gets a new record (the old one is dead space; a pack more than half dead is started again).
// Robustness: the index is
// checked by its sum and against the pack's size; records after the part the index covers (a run that ended without
// writing its index) are found by walking them, each checked by its header sum; a cut-off or damaged tail is cut away;
// each payload is checked by its sum and its decoded size when it is loaded, and a bad one is dropped and made again
// from the PNG. Nothing read from a cache file is trusted before it is checked.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <lz4.h>
#include <lz4hc.h>

#include "platform.h"

namespace cane::texcache {

inline constexpr uint32_t kVersion = 1;
inline constexpr char kPackMagic[9] = "SHTEXPK1";
inline constexpr char kIndexMagic[9] = "SHTEXIX1";
inline constexpr uint32_t kRecordMagic = 0x43525854;   // "TXRC"
inline constexpr size_t kPackHeader = 16, kRecordHeader = 56;
inline constexpr uint32_t kMaxSide = 16384;

// A fast 64-bit sum (not FNV byte by byte: the payloads are megabytes), 8 bytes a step.
inline uint64_t Sum64(const uint8_t* p, size_t cb, uint64_t h = 0x9E3779B97F4A7C15ull)
{
    size_t i = 0;
    for (; i + 8 <= cb; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    uint64_t w = 0;
    std::memcpy(&w, p + i, cb - i);
    h = (h ^ w ^ (uint64_t(cb) << 56)) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

namespace detail {
inline void Put(std::vector<uint8_t>& ab, uint64_t v, int cb)
{
    for (int i = 0; i < cb; ++i) ab.push_back(uint8_t(v >> (8 * i)));
}
inline uint64_t Get(const uint8_t* p, int cb)
{
    uint64_t v = 0;
    for (int i = 0; i < cb; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
inline uint64_t HashName(const std::string& s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}
}  // namespace detail

// A source file's stamp: its size and modification time (the clock's own ticks: only ever compared for equality).
struct Stamp
{
    uint64_t size = 0;
    int64_t time = 0;
    bool operator==(const Stamp& o) const { return size == o.size && time == o.time; }
};
inline bool StampOf(const fs::path& p, Stamp& stamp)
{
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) return false;
    const auto time = fs::last_write_time(p, ec);
    if (ec) return false;
    stamp.size = uint64_t(size);
    stamp.time = int64_t(time.time_since_epoch().count());
    return true;
}

struct Entry
{
    Stamp stamp;
    uint64_t offset = 0;   // of the payload
    uint32_t cb = 0, w = 0, h = 0;
    uint8_t enc = 0;
    uint64_t sum = 0;
};

class Pack
{
public:
    // dir: the cache folder (made when missing); source: the folder the PNGs are in (names the pack).
    // False: the cache folder cannot be used (nothing is cached; every call below then fails quietly).
    bool Open(const fs::path& dir, const std::string& source)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_source = source;
        char sz[32];
        std::snprintf(sz, sizeof(sz), "%016llx", static_cast<unsigned long long>(detail::HashName(source)));
        std::error_code ec;
        fs::create_directories(dir, ec);
        m_pathPack = dir / (std::string(sz) + ".texpack");
        m_pathIndex = dir / (std::string(sz) + ".texindex");
        m_fOpen = FOpenLocked();
        return m_fOpen;
    }
    [[nodiscard]] bool FOpen() const { return m_fOpen; }
    [[nodiscard]] size_t Centry() const { std::lock_guard<std::mutex> lock(m_mtx); return m_mp.size(); }
    [[nodiscard]] uint64_t CbPack() const { std::lock_guard<std::mutex> lock(m_mtx); return m_cbPack; }
    [[nodiscard]] uint64_t CbDead() const { std::lock_guard<std::mutex> lock(m_mtx); return m_cbDead; }
    [[nodiscard]] const fs::path& PathPack() const { return m_pathPack; }
    [[nodiscard]] const fs::path& PathIndex() const { return m_pathIndex; }

    [[nodiscard]] bool FHas(const std::string& name, const Stamp& stamp) const
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        const auto it = m_mp.find(name);
        return it != m_mp.end() && it->second.stamp == stamp;
    }
    // The texels of the PNG `name` with this stamp; false: not cached (or its record was bad: dropped).
    bool Load(const std::string& name, const Stamp& stamp, std::vector<uint8_t>& texels, int* pw, int* ph)
    {
        Entry e;
        std::vector<uint8_t> ab;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            if (!m_fOpen) return false;
            const auto it = m_mp.find(name);
            if (it == m_mp.end() || !(it->second.stamp == stamp)) return false;
            e = it->second;
            ab.resize(e.cb);
            if (!m_in.is_open()) m_in.open(m_pathPack, std::ios::binary);
            m_in.clear();
            m_in.seekg(std::streamoff(e.offset));
            if (!m_in.read(reinterpret_cast<char*>(ab.data()), std::streamsize(ab.size()))) return FDropLocked(name, e);
        }
        const size_t cbTexels = size_t(e.w) * e.h * 4;
        bool fOk = Sum64(ab.data(), ab.size()) == e.sum;
        if (fOk && e.enc == 0)
        {
            fOk = ab.size() == cbTexels;
            if (fOk) texels = std::move(ab);
        }
        else if (fOk)
        {
            texels.resize(cbTexels);
            fOk = LZ4_decompress_safe(reinterpret_cast<const char*>(ab.data()), reinterpret_cast<char*>(texels.data()), int(ab.size()), int(cbTexels)) ==
                  int(cbTexels);
        }
        if (!fOk)
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            return FDropLocked(name, e);
        }
        *pw = int(e.w);
        *ph = int(e.h);
        return true;
    }
    // Adds (or replaces) the PNG's texels. Compresses outside the lock; one write appends the record.
    bool Store(const std::string& name, const Stamp& stamp, const uint8_t* texels, int w, int h)
    {
        if (!m_fOpen || w <= 0 || h <= 0 || uint32_t(w) > kMaxSide || uint32_t(h) > kMaxSide || name.empty() || name.size() > 1024) return false;
        const size_t cbTexels = size_t(w) * h * 4;
        if (cbTexels > size_t(LZ4_MAX_INPUT_SIZE)) return false;
        std::vector<uint8_t> rec;
        rec.reserve(kRecordHeader + name.size() + cbTexels / 2);
        std::vector<uint8_t> payload;
        payload.resize(size_t(LZ4_compressBound(int(cbTexels))));
        const int cbLz = LZ4_compress_HC(reinterpret_cast<const char*>(texels), reinterpret_cast<char*>(payload.data()), int(cbTexels), int(payload.size()),
                                         LZ4HC_CLEVEL_DEFAULT);
        Entry e;
        e.stamp = stamp;
        e.w = uint32_t(w), e.h = uint32_t(h);
        if (cbLz > 0 && size_t(cbLz) < cbTexels) { payload.resize(size_t(cbLz)); e.enc = 1; }
        else { payload.assign(texels, texels + cbTexels); e.enc = 0; }
        e.cb = uint32_t(payload.size());
        e.sum = Sum64(payload.data(), payload.size());
        PutRecordHeader(rec, name, e);
        rec.insert(rec.end(), payload.begin(), payload.end());

        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_fOpen) return false;
        std::ofstream out(m_pathPack, std::ios::binary | std::ios::app);
        if (!out) return false;
        // (the file's own end: another process may have appended since)
        std::error_code ec;
        const auto cbNow = fs::file_size(m_pathPack, ec);
        if (ec || cbNow < kPackHeader) return false;
        if (!out.write(reinterpret_cast<const char*>(rec.data()), std::streamsize(rec.size())) || !out.flush()) return false;
        out.close();
        const auto cbAfter = fs::file_size(m_pathPack, ec);
        if (ec || cbAfter != cbNow + rec.size())
        {
            // (not where it was expected: the next start walks the pack; this run does not use the record)
            m_fDirty = true;
            return false;
        }
        e.offset = uint64_t(cbNow) + kRecordHeader + name.size();
        AddLocked(name, e);
        m_cbPack = uint64_t(cbAfter);
        m_fDirty = true;
        return true;
    }
    // Drops every entry fKeep refuses (a PNG that is gone). Returns the count dropped.
    size_t Prune(const std::function<bool(const std::string&)>& fKeep)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        size_t c = 0;
        for (auto it = m_mp.begin(); it != m_mp.end();)
            if (!fKeep(it->first))
            {
                m_cbDead += kRecordHeader + it->first.size() + it->second.cb;
                it = m_mp.erase(it);
                m_fDirty = true;
                ++c;
            }
            else ++it;
        return c;
    }
    // Writes the index when anything changed since the last one. A pack more than half dead (and over 16 MiB of it) is
    // started again here: its entries are forgotten and made again from the PNGs as they are used.
    bool Flush()
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_fOpen || !m_fDirty) return true;
        if (m_cbDead > (uint64_t(16) << 20) && m_cbDead * 2 > m_cbPack) ResetLocked();
        return FWriteIndexLocked();
    }
    // Both files removed (the cache cleared, or the pack turned off).
    void Remove()
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_in.is_open()) m_in.close();
        std::error_code ec;
        fs::remove(m_pathPack, ec);
        fs::remove(m_pathIndex, ec);
        m_mp.clear();
        m_cbPack = m_cbDead = 0;
        m_fOpen = false;
        m_fDirty = false;
    }

private:
    static void PutRecordHeader(std::vector<uint8_t>& ab, const std::string& name, const Entry& e)
    {
        const size_t i0 = ab.size();
        detail::Put(ab, kRecordMagic, 4);
        detail::Put(ab, name.size(), 2);
        detail::Put(ab, e.enc, 1);
        detail::Put(ab, 0, 1);
        detail::Put(ab, e.w, 4);
        detail::Put(ab, e.h, 4);
        detail::Put(ab, e.cb, 4);
        detail::Put(ab, 0, 4);
        detail::Put(ab, e.stamp.size, 8);
        detail::Put(ab, uint64_t(e.stamp.time), 8);
        detail::Put(ab, e.sum, 8);
        const uint64_t sum = Sum64(reinterpret_cast<const uint8_t*>(name.data()), name.size(), Sum64(ab.data() + i0, 48));
        detail::Put(ab, sum, 8);
        ab.insert(ab.end(), name.begin(), name.end());
    }
    static bool FSaneEntry(const Entry& e)
    {
        if (e.w == 0 || e.h == 0 || e.w > kMaxSide || e.h > kMaxSide || e.enc > 1 || e.cb == 0) return false;
        const uint64_t cbTexels = uint64_t(e.w) * e.h * 4;
        return e.enc == 0 ? e.cb == cbTexels : e.cb < cbTexels;
    }
    void AddLocked(const std::string& name, const Entry& e)
    {
        auto [it, fNew] = m_mp.try_emplace(name, e);
        if (!fNew)
        {
            m_cbDead += kRecordHeader + name.size() + it->second.cb;
            it->second = e;
        }
    }
    bool FDropLocked(const std::string& name, const Entry& e)
    {
        const auto it = m_mp.find(name);
        if (it != m_mp.end() && it->second.offset == e.offset)
        {
            m_cbDead += kRecordHeader + name.size() + e.cb;
            m_mp.erase(it);
            m_fDirty = true;
        }
        return false;
    }
    bool ResetLocked()
    {
        if (m_in.is_open()) m_in.close();
        m_mp.clear();
        m_cbDead = 0;
        std::vector<uint8_t> ab(kPackMagic, kPackMagic + 8);
        detail::Put(ab, kVersion, 4);
        detail::Put(ab, 0, 4);
        std::ofstream out(m_pathPack, std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size())) || !out.flush()) return false;
        m_cbPack = kPackHeader;
        m_fDirty = true;
        return true;
    }
    bool FWriteIndexLocked()
    {
        std::vector<uint8_t> ab(kIndexMagic, kIndexMagic + 8);
        detail::Put(ab, kVersion, 4);
        detail::Put(ab, m_mp.size(), 4);
        detail::Put(ab, m_cbPack, 8);
        detail::Put(ab, m_cbDead, 8);
        detail::Put(ab, m_source.size(), 2);
        ab.insert(ab.end(), m_source.begin(), m_source.end());
        for (const auto& [name, e] : m_mp)
        {
            detail::Put(ab, name.size(), 2);
            ab.insert(ab.end(), name.begin(), name.end());
            detail::Put(ab, e.stamp.size, 8);
            detail::Put(ab, uint64_t(e.stamp.time), 8);
            detail::Put(ab, e.offset, 8);
            detail::Put(ab, e.cb, 4);
            detail::Put(ab, e.w, 4);
            detail::Put(ab, e.h, 4);
            detail::Put(ab, e.enc, 1);
            detail::Put(ab, e.sum, 8);
        }
        detail::Put(ab, Sum64(ab.data(), ab.size()), 8);
        fs::path tmp = m_pathIndex;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size())) || !out.flush()) return false;
        }
        if (!atomicReplace(tmp, m_pathIndex)) return false;
        m_fDirty = false;
        return true;
    }
    // The index's entries when the file is whole and belongs to this pack; *pcbCovered: the pack bytes it covers.
    bool FReadIndexLocked(uint64_t cbPack, uint64_t* pcbCovered)
    {
        std::ifstream in(m_pathIndex, std::ios::binary | std::ios::ate);
        if (!in) return false;
        const std::streamoff cb = std::streamoff(in.tellg());
        if (cb < 42 || cb > (std::streamoff(1) << 28)) return false;
        std::vector<uint8_t> ab(static_cast<size_t>(cb));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(ab.data()), cb)) return false;
        const size_t cbBody = ab.size() - 8;
        if (std::memcmp(ab.data(), kIndexMagic, 8) != 0 || detail::Get(ab.data() + 8, 4) != kVersion) return false;
        if (detail::Get(ab.data() + cbBody, 8) != Sum64(ab.data(), cbBody)) return false;
        const size_t centry = size_t(detail::Get(ab.data() + 12, 4));
        const uint64_t cbCovered = detail::Get(ab.data() + 16, 8);
        const uint64_t cbDead = detail::Get(ab.data() + 24, 8);
        size_t i = 32;
        const size_t cbSource = size_t(detail::Get(ab.data() + i, 2));
        i += 2;
        if (i + cbSource > cbBody || std::string(reinterpret_cast<const char*>(ab.data() + i), cbSource) != m_source) return false;
        i += cbSource;
        if (cbCovered < kPackHeader || cbCovered > cbPack) return false;   // (the pack was cut: walked from its start)
        std::unordered_map<std::string, Entry> mp;
        mp.reserve(centry);
        for (size_t ientry = 0; ientry < centry; ++ientry)
        {
            if (i + 2 > cbBody) return false;
            const size_t cbName = size_t(detail::Get(ab.data() + i, 2));
            i += 2;
            if (cbName == 0 || i + cbName + 45 > cbBody) return false;
            std::string name(reinterpret_cast<const char*>(ab.data() + i), cbName);
            i += cbName;
            Entry e;
            e.stamp.size = detail::Get(ab.data() + i, 8);
            e.stamp.time = int64_t(detail::Get(ab.data() + i + 8, 8));
            e.offset = detail::Get(ab.data() + i + 16, 8);
            e.cb = uint32_t(detail::Get(ab.data() + i + 24, 4));
            e.w = uint32_t(detail::Get(ab.data() + i + 28, 4));
            e.h = uint32_t(detail::Get(ab.data() + i + 32, 4));
            e.enc = uint8_t(detail::Get(ab.data() + i + 36, 1));
            e.sum = detail::Get(ab.data() + i + 37, 8);
            i += 45;
            if (!FSaneEntry(e) || e.offset < kPackHeader + kRecordHeader || e.offset + e.cb > cbCovered) return false;
            mp.emplace(std::move(name), e);
        }
        if (i != cbBody) return false;
        m_mp = std::move(mp);
        m_cbDead = cbDead;
        *pcbCovered = cbCovered;
        return true;
    }
    // The records from ib to the pack's end added; returns where the last whole record ends.
    uint64_t WalkLocked(std::ifstream& in, uint64_t ib, uint64_t cbPack)
    {
        std::vector<uint8_t> hdr(kRecordHeader);
        std::string name;
        while (ib + kRecordHeader <= cbPack)
        {
            in.clear();
            in.seekg(std::streamoff(ib));
            if (!in.read(reinterpret_cast<char*>(hdr.data()), std::streamsize(kRecordHeader))) break;
            if (detail::Get(hdr.data(), 4) != kRecordMagic) break;
            const size_t cbName = size_t(detail::Get(hdr.data() + 4, 2));
            Entry e;
            e.enc = uint8_t(detail::Get(hdr.data() + 6, 1));
            e.w = uint32_t(detail::Get(hdr.data() + 8, 4));
            e.h = uint32_t(detail::Get(hdr.data() + 12, 4));
            e.cb = uint32_t(detail::Get(hdr.data() + 16, 4));
            e.stamp.size = detail::Get(hdr.data() + 24, 8);
            e.stamp.time = int64_t(detail::Get(hdr.data() + 32, 8));
            e.sum = detail::Get(hdr.data() + 40, 8);
            if (cbName == 0 || cbName > 1024 || !FSaneEntry(e) || ib + kRecordHeader + cbName + e.cb > cbPack) break;
            name.resize(cbName);
            if (!in.read(name.data(), std::streamsize(cbName))) break;
            if (detail::Get(hdr.data() + 48, 8) != Sum64(reinterpret_cast<const uint8_t*>(name.data()), cbName, Sum64(hdr.data(), 48))) break;
            e.offset = ib + kRecordHeader + cbName;
            AddLocked(name, e);
            ib = e.offset + e.cb;
        }
        return ib;
    }
    bool FOpenLocked()
    {
        m_mp.clear();
        m_cbDead = 0;
        m_fDirty = false;
        if (m_in.is_open()) m_in.close();
        std::error_code ec;
        uint64_t cbPack = 0;
        bool fPack = false;
        if (fs::is_regular_file(m_pathPack, ec))
        {
            cbPack = uint64_t(fs::file_size(m_pathPack, ec));
            std::ifstream in(m_pathPack, std::ios::binary);
            uint8_t hdr[kPackHeader] = {};
            fPack = !ec && cbPack >= kPackHeader && in.read(reinterpret_cast<char*>(hdr), std::streamsize(kPackHeader)) &&
                    std::memcmp(hdr, kPackMagic, 8) == 0 && detail::Get(hdr + 8, 4) == kVersion;
        }
        if (!fPack) return ResetLocked() && FWriteIndexLocked();   // (none yet, another version's, or not a pack)
        uint64_t cbCovered = kPackHeader;
        if (!FReadIndexLocked(cbPack, &cbCovered))
        {
            m_mp.clear();
            m_cbDead = 0;
            cbCovered = kPackHeader;
            m_fDirty = true;
        }
        if (cbCovered < cbPack)
        {
            std::ifstream in(m_pathPack, std::ios::binary);
            const uint64_t ibEnd = WalkLocked(in, cbCovered, cbPack);
            in.close();
            if (ibEnd < cbPack)
            {
                // (a cut-off or damaged tail: cut away, so the next record starts where the walk can find it)
                fs::resize_file(m_pathPack, ibEnd, ec);
                if (ec) return ResetLocked() && FWriteIndexLocked();
                cbPack = ibEnd;
            }
            m_fDirty = true;
        }
        m_cbPack = cbPack;
        return true;
    }

    mutable std::mutex m_mtx;
    fs::path m_pathPack, m_pathIndex;
    std::string m_source;
    std::unordered_map<std::string, Entry> m_mp;
    std::ifstream m_in;
    uint64_t m_cbPack = 0, m_cbDead = 0;
    bool m_fOpen = false, m_fDirty = false;
};

// The packs in the cache folder whose source (a folder of PNGs, a Textures.pak) is gone (a texture pack removed, a
// profile moved) deleted.
// Returns the count of packs removed.
inline size_t RemoveOrphans(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    std::vector<fs::path> aindex;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".texindex") aindex.push_back(e.path());
    size_t c = 0;
    for (const fs::path& index : aindex)
    {
        std::string source;
        {
            std::ifstream in(index, std::ios::binary);
            uint8_t hdr[34] = {};
            if (!in.read(reinterpret_cast<char*>(hdr), 34) || std::memcmp(hdr, kIndexMagic, 8) != 0) continue;
            source.resize(size_t(detail::Get(hdr + 32, 2)));
            if (source.empty() || !in.read(source.data(), std::streamsize(source.size()))) continue;
        }
        if (fs::exists(path(source), ec)) continue;
        fs::path pack = index;
        pack.replace_extension(".texpack");
        fs::remove(pack, ec);
        fs::remove(index, ec);
        ++c;
    }
    return c;
}

}  // namespace cane::texcache
