// See disc_image.h.
#include "disc_image.h"

#include <libchdr/chd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace cane {
namespace {
namespace fs = std::filesystem;

constexpr uint32_t kCdFrame = 2448;   // a CHD's CD frame: the 2352-byte sector and 96 bytes of subcode

FILE* OpenBinary(const fs::path& file) {
#ifdef _WIN32
    return _wfopen(file.c_str(), L"rb");
#else
    return std::fopen(file.c_str(), "rb");
#endif
}

class IsoImage final : public DiscImage {
public:
    explicit IsoImage(const fs::path& file) : m_in(file, std::ios::binary) {
        std::error_code ec;
        m_size = fs::file_size(file, ec);
    }
    bool opened() const { return m_in.is_open(); }
    uint64_t size() const override { return m_size; }
    bool read(uint64_t offset, void* data, size_t size) override {
        return offset <= m_size && size <= m_size - offset && readAt(m_in, offset, data, size);
    }

private:
    std::ifstream m_in;
    uint64_t m_size = 0;
};

// An .iso in parts (imageParts): one stream over them, in order.
class PartsImage final : public DiscImage {
public:
    explicit PartsImage(const std::vector<fs::path>& paths) {
        for (const fs::path& path : paths) {
            std::error_code ec;
            Part part{std::ifstream(path, std::ios::binary), m_size, fs::file_size(path, ec)};
            if (!part.in.is_open() || ec) {
                m_parts.clear();
                return;
            }
            m_size += part.size;
            m_parts.push_back(std::move(part));
        }
    }
    bool opened() const { return !m_parts.empty(); }
    uint64_t size() const override { return m_size; }
    bool read(uint64_t offset, void* data, size_t size) override {
        if (offset > m_size || size > m_size - offset) return false;
        auto* out = static_cast<char*>(data);
        for (Part& part : m_parts) {
            if (!size) break;
            if (offset >= part.start + part.size) continue;
            const uint64_t within = offset - part.start;
            const size_t n = size_t(std::min<uint64_t>(size, part.size - within));
            if (!readAt(part.in, within, out, n)) return false;
            out += n;
            offset += n;
            size -= n;
        }
        return size == 0;
    }

private:
    struct Part {
        std::ifstream in;
        uint64_t start = 0, size = 0;
    };
    std::vector<Part> m_parts;
    uint64_t m_size = 0;
};

// A CHD's disc: a DVD (2048-byte units), or a CD's data track (its frames' sectors from where their mode puts the
// user data: 0 for MODE1/2048, 16 for raw MODE1, 24 for raw MODE2 form 1).
class ChdImage final : public DiscImage {
public:
    ~ChdImage() override {
        if (m_chd) chd_close(m_chd);
        if (m_file) std::fclose(m_file);
    }
    std::string open(const fs::path& file) {
        m_file = OpenBinary(file);
        if (!m_file) return "That CHD file cannot be opened.";
        const chd_error error = chd_open_file(m_file, CHD_OPEN_READ, nullptr, &m_chd);
        if (error != CHDERR_NONE) {
            m_chd = nullptr;
            if (error == CHDERR_REQUIRES_PARENT) return "That CHD needs its parent CHD: convert the disc to a CHD of its own.";
            return std::string("That CHD file cannot be read (") + chd_error_string(error) + ").";
        }
        const chd_header* header = chd_get_header(m_chd);
        m_hunkBytes = header->hunkbytes;
        m_unitBytes = header->unitbytes;
        if (m_hunkBytes == 0 || m_unitBytes == 0 || m_hunkBytes % m_unitBytes) return "That CHD's layout is not supported.";
        m_hunk.resize(m_hunkBytes);
        if (m_unitBytes == kIsoSector) {
            m_size = header->logicalbytes / kIsoSector * kIsoSector;
            return {};
        }
        if (m_unitBytes != kCdFrame) return "That CHD is not a CD or DVD image.";
        uint8_t pvd[88];
        for (const uint32_t offset : {0u, 16u, 24u}) {
            if (!Unit(16, offset, pvd, sizeof(pvd)) || std::memcmp(pvd, "\x01" "CD001" "\x01", 7) != 0) continue;
            m_sectorOffset = offset;
            // (the track ends with the volume: its frames are padded to a multiple of 4, other tracks may follow)
            const uint64_t blocks = le32(pvd + 80);
            m_size = std::min<uint64_t>(header->unitcount, blocks ? blocks : header->unitcount) * kIsoSector;
            return {};
        }
        return "That CHD holds no PlayStation 2 data track.";
    }
    uint64_t size() const override { return m_size; }
    bool read(uint64_t offset, void* data, size_t size) override {
        if (offset > m_size || size > m_size - offset) return false;
        auto* out = static_cast<uint8_t*>(data);
        while (size) {
            const uint32_t within = uint32_t(offset % kIsoSector);
            const size_t n = std::min<size_t>(size, kIsoSector - within);
            if (!Unit(offset / kIsoSector, m_sectorOffset + within, out, n)) return false;
            out += n;
            offset += n;
            size -= n;
        }
        return true;
    }

private:
    // `size` bytes at `offset` in unit `unit` (a unit never spans two hunks)
    bool Unit(uint64_t unit, uint32_t offset, void* data, size_t size) {
        const uint64_t byte = unit * m_unitBytes + offset;
        const uint64_t hunk = byte / m_hunkBytes;
        const uint32_t within = uint32_t(byte % m_hunkBytes);
        if (hunk >= chd_get_header(m_chd)->totalhunks || within + size > m_hunkBytes) return false;
        if (hunk != m_cached) {
            m_cached = ~uint64_t(0);
            if (chd_read(m_chd, uint32_t(hunk), m_hunk.data()) != CHDERR_NONE) return false;
            m_cached = hunk;
        }
        std::memcpy(data, m_hunk.data() + within, size);
        return true;
    }

    FILE* m_file = nullptr;
    chd_file* m_chd = nullptr;
    uint32_t m_hunkBytes = 0, m_unitBytes = 0, m_sectorOffset = 0;
    uint64_t m_size = 0, m_cached = ~uint64_t(0);
    std::vector<uint8_t> m_hunk;
};
}  // namespace

bool chdFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    char magic[8] = {};
    return in.read(magic, sizeof(magic)) && std::memcmp(magic, "MComprHD", sizeof(magic)) == 0;
}

std::vector<fs::path> imageParts(const fs::path& path) {
    std::error_code ec;
    std::vector<fs::path> parts;
    auto numbered = [&](const fs::path& stem, int first, int width) {
        char number[16];
        for (int i = first;; ++i) {
            std::snprintf(number, sizeof(number), "%0*d", width, i);
            fs::path part = stem;
            part += number;
            if (!fs::is_regular_file(part, ec)) break;
            parts.push_back(part);
        }
    };
    if (fs::is_directory(path, ec)) {
        numbered(path / "", 0, 2);
        return parts;
    }
    // name.iso.NNN: the first part only (from .0, or from .1 when there is no .0), and at least two
    const std::string extension = path.extension().string();
    const std::string digits = extension.size() > 1 ? extension.substr(1) : std::string();
    if (digits.empty() || digits.size() > 3 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {};
    const int first = std::stoi(digits);
    fs::path stem = path;
    stem.replace_extension();
    stem += ".";
    if (first == 1) {
        fs::path zero = stem;
        zero += std::string(digits.size(), '0');
        if (fs::exists(zero, ec)) return {};
    } else if (first != 0) {
        return {};
    }
    numbered(stem, first, int(digits.size()));
    return parts.size() >= 2 ? parts : std::vector<fs::path>{};
}

std::unique_ptr<DiscImage> DiscImage::open(const fs::path& file, std::string* error) {
    if (const std::vector<fs::path> parts = imageParts(file); !parts.empty()) {
        auto split = std::make_unique<PartsImage>(parts);
        if (split->opened()) return split;
        if (error) *error = "The parts of that disc image cannot be opened.";
        return nullptr;
    }
    if (chdFile(file)) {
        auto chd = std::make_unique<ChdImage>();
        std::string why = chd->open(file);
        if (why.empty()) return chd;
        if (error) *error = why;
        return nullptr;
    }
    auto iso = std::make_unique<IsoImage>(file);
    if (iso->opened()) return iso;
    if (error) *error = "That disc image cannot be opened.";
    return nullptr;
}

DiscIdentity identifyDiscImage(const fs::path& file) {
    const std::unique_ptr<DiscImage> image = DiscImage::open(file);
    return image ? identifyDisc(image->reader()) : DiscIdentity{};
}

std::string writeImageAsIso(const fs::path& source, const fs::path& iso, const std::function<bool(uint64_t, uint64_t)>& progress) {
    std::string error;
    const std::unique_ptr<DiscImage> image = DiscImage::open(source, &error);
    if (!image) return error;
    const std::string unreadable = chdFile(source) ? "That CHD file is damaged: part of it does not decompress."
                                                   : "Part of that disc image cannot be read.";
    const uint64_t total = image->size();
    {
        std::ofstream out(iso, std::ios::binary | std::ios::trunc);
        if (!out) return "The decompressed disc image cannot be created.";
        std::vector<char> buffer(1u << 20);
        for (uint64_t done = 0; done < total && error.empty();) {
            const size_t n = size_t(std::min<uint64_t>(buffer.size(), total - done));
            if (!image->read(done, buffer.data(), n)) error = unreadable;
            else if (!out.write(buffer.data(), std::streamsize(n))) error = "The disc image could not be written (is the drive full?).";
            else {
                done += n;
                if (progress && !progress(done, total)) error = "cancelled";
            }
        }
        out.close();
        if (error.empty() && !out) error = "The disc image could not be written (is the drive full?).";
    }
    if (!error.empty()) {
        std::error_code ec;
        fs::remove(iso, ec);
    }
    return error;
}
}  // namespace cane
