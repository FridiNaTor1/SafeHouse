#pragma once
// A texture pack in one file, <pack>/Textures.pak: the PNGs a texture pack holds (the games' texmod.h names,
// "<W>x<H>_<hash>.png"), one after another, and their index. The PS3 HD imports write it (Sly1-Release / Sly2-Release
// ps3hd.cpp), or a Textures folder of the PNGs when asked (TexturePackWriter). One file opens and copies far faster
// than thousands (the Switch's SD card took a few hundred milliseconds to open a file in a folder of them).
//
// The file (little-endian): "SHPK", the version (1), the count, 0 (u32 each), the index's offset (u64); the PNGs; the
// index: `count` entries of a name (32 bytes, zero-padded), the PNG's offset (u64) and size (u32, then 4 zero bytes).
#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cane
{
namespace pak
{
inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kHeader = 24, kEntry = 48, kName = 32;
inline std::FILE* open(const fs::path& p, bool write)
{
#ifdef _WIN32
    return _wfopen(p.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
}
inline bool seek(std::FILE* f, uint64_t at, int whence = SEEK_SET)
{
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(at), whence) == 0;
#else
    return fseeko(f, static_cast<off_t>(at), whence) == 0;
#endif
}
inline uint64_t tell(std::FILE* f)
{
#ifdef _WIN32
    return uint64_t(_ftelli64(f));
#else
    return uint64_t(ftello(f));
#endif
}
}  // namespace pak

class TexturePakReader
{
public:
    TexturePakReader() = default;
    ~TexturePakReader() { Close(); }
    TexturePakReader(const TexturePakReader&) = delete;
    TexturePakReader& operator=(const TexturePakReader&) = delete;

    // False: no such file, or not a pak.
    bool Open(const fs::path& path)
    {
        Close();
        m_file = pak::open(path, false);
        if (!m_file) return false;
        std::setvbuf(m_file, nullptr, _IONBF, 0);
        uint8_t header[pak::kHeader];
        uint32_t version = 0, count = 0;
        uint64_t index = 0, size = 0;
        bool ok = std::fread(header, 1, sizeof(header), m_file) == sizeof(header) && std::memcmp(header, "SHPK", 4) == 0 &&
                  pak::seek(m_file, 0, SEEK_END);
        if (ok)
        {
            size = pak::tell(m_file);
            std::memcpy(&version, header + 4, 4);
            std::memcpy(&count, header + 8, 4);
            std::memcpy(&index, header + 16, 8);
            ok = version == pak::kVersion && index >= pak::kHeader && index <= size && (size - index) / pak::kEntry >= count;
        }
        std::vector<uint8_t> entries(ok ? size_t(count) * pak::kEntry : 0);
        ok = ok && pak::seek(m_file, index) && std::fread(entries.data(), 1, entries.size(), m_file) == entries.size();
        for (uint32_t i = 0; ok && i < count; ++i)
        {
            const uint8_t* e = entries.data() + size_t(i) * pak::kEntry;
            std::string name(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), pak::kName));
            ENTRY entry{};
            std::memcpy(&entry.offset, e + pak::kName, 8);
            std::memcpy(&entry.size, e + pak::kName + 8, 4);
            ok = !name.empty() && entry.offset >= pak::kHeader && entry.offset + entry.size <= index;
            if (ok && m_entries.emplace(name, entry).second) m_names.push_back(std::move(name));
        }
        if (!ok) Close();
        return ok;
    }
    void Close()
    {
        if (m_file) std::fclose(m_file);
        m_file = nullptr;
        m_names.clear();
        m_entries.clear();
    }
    const std::vector<std::string>& Names() const { return m_names; }
    // A PNG's bytes by its name (thread-safe).
    bool Read(const std::string& name, std::vector<uint8_t>& out) const
    {
        const auto it = m_entries.find(name);
        if (it == m_entries.end() || it->second.size == 0) return false;
        out.resize(it->second.size);
        std::lock_guard<std::mutex> lock(m_mutex);
        return pak::seek(m_file, it->second.offset) && std::fread(out.data(), 1, out.size(), m_file) == out.size();
    }

private:
    struct ENTRY
    {
        uint64_t offset = 0;
        uint32_t size = 0;
    };
    std::FILE* m_file = nullptr;
    mutable std::mutex m_mutex;
    std::vector<std::string> m_names;
    std::unordered_map<std::string, ENTRY> m_entries;
};

// Written as <path>.partial, which Close puts in place: a pack that did not finish (an error, a cancelled import)
// leaves no Textures.pak.
class TexturePakWriter
{
public:
    TexturePakWriter() = default;
    ~TexturePakWriter() { Discard(); }
    TexturePakWriter(const TexturePakWriter&) = delete;
    TexturePakWriter& operator=(const TexturePakWriter&) = delete;

    bool Open(const fs::path& path)
    {
        Discard();
        m_path = path;
        m_partial = path;
        m_partial += ".partial";
        m_file = pak::open(m_partial, true);
        m_offset = pak::kHeader;
        const uint8_t header[pak::kHeader] = {};
        return m_file && std::fwrite(header, 1, sizeof(header), m_file) == sizeof(header);
    }
    // A PNG under its name (shorter than 32 bytes).
    bool Add(const std::string& name, const void* data, size_t size)
    {
        if (!m_file || name.empty() || name.size() >= pak::kName || size > UINT32_MAX) return false;
        if (std::fwrite(data, 1, size, m_file) != size) return false;
        uint8_t entry[pak::kEntry] = {};
        std::memcpy(entry, name.data(), name.size());
        const uint32_t size32 = uint32_t(size);
        std::memcpy(entry + pak::kName, &m_offset, 8);
        std::memcpy(entry + pak::kName + 8, &size32, 4);
        m_index.insert(m_index.end(), entry, entry + sizeof(entry));
        m_offset += size;
        ++m_count;
        return true;
    }
    // The index and the header written, the file put in place.
    bool Close()
    {
        if (!m_file) return false;
        uint8_t header[pak::kHeader] = {'S', 'H', 'P', 'K'};
        std::memcpy(header + 4, &pak::kVersion, 4);
        std::memcpy(header + 8, &m_count, 4);
        std::memcpy(header + 16, &m_offset, 8);
        bool ok = std::fwrite(m_index.data(), 1, m_index.size(), m_file) == m_index.size() && pak::seek(m_file, 0) &&
                  std::fwrite(header, 1, sizeof(header), m_file) == sizeof(header);
        ok = std::fclose(m_file) == 0 && ok;
        m_file = nullptr;
        ok = ok && atomicReplace(m_partial, m_path);
        if (!ok) Discard();
        return ok;
    }

private:
    void Discard()
    {
        if (m_file) std::fclose(m_file);
        m_file = nullptr;
        if (!m_partial.empty())
        {
            std::error_code ec;
            fs::remove(m_partial, ec);
        }
        m_index.clear();
        m_count = 0;
    }
    fs::path m_path, m_partial;
    std::FILE* m_file = nullptr;
    uint64_t m_offset = 0;
    uint32_t m_count = 0;
    std::vector<uint8_t> m_index;
};

// A texture pack written into <pack>: Textures.pak, or (png) a Textures folder of the PNG files, to look through and
// edit by hand. The games read either; the one file loads faster.
class TexturePackWriter
{
public:
    bool Open(const fs::path& pack, bool png)
    {
        m_png = png;
        if (!png) return m_pak.Open(pack / "Textures.pak");
        m_dir = pack / "Textures";
        std::error_code ec;
        fs::create_directories(m_dir, ec);
        return !ec;
    }
    bool Add(const std::string& name, const void* data, size_t size)
    {
        if (!m_png) return m_pak.Add(name, data, size);
        std::ofstream out(m_dir / path(name), std::ios::binary);
        return out.write(static_cast<const char*>(data), std::streamsize(size)).good();
    }
    bool Close() { return m_png || m_pak.Close(); }

private:
    TexturePakWriter m_pak;
    fs::path m_dir;
    bool m_png = false;
};
}  // namespace cane
