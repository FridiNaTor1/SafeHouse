#pragma once
// The Sly Collection (PS3) as a read-only data source for the PS3 HD imports (Sly2-Release/ps3hd.cpp,
// Sly1-Release/ps3hd.cpp). The source is any of:
//   - the extracted disc: a folder holding PS3_GAME/USRDIR, PS3_GAME, USRDIR itself, or USRDIR's SLY1 / SLY2 folder;
//   - the disc image (.iso, ISO 9660; multi-extent files over 1 GB);
//   - a .zip holding that disc image (deflated or stored, zip64). The image is never unpacked: it is inflated once
//     as a stream and only the bytes of the wanted files are handed on (miniz, Libraries/miniz).
// Paths are relative to PS3_GAME/USRDIR ("SLY2/SLY2.WAL"), matched without case. Nothing from the disc is ever
// run: its files are read as data only.
#include "platform.h"

#include <miniz.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cane::ps3 {
namespace detail {
inline std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
inline FILE* openRead(const fs::path& p) {
#ifdef _WIN32
    return _wfopen(p.c_str(), L"rb");
#else
    return std::fopen(p.c_str(), "rb");
#endif
}
inline bool seek64(FILE* f, uint64_t at) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(at), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
}
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace detail

class Disc {
public:
    enum class Kind { None, Folder, Iso, Zip };
    struct Extent { uint64_t lba = 0, size = 0; };
    struct File {
        std::string path;  // upper case, relative to USRDIR
        uint64_t size = 0;
        std::vector<Extent> extents;  // (an image's; empty for a folder)
        fs::path local;               // (a folder's)
    };
    // Called with each wanted file's bytes in order of their place on the disc (a file's own bytes in order). Return
    // false to stop reading.
    using Sink = std::function<bool(size_t ifile, uint64_t offset, const uint8_t* data, size_t size)>;
    // done / total bytes of the read (for a .zip, of the image streamed through); return false to cancel.
    using Progress = std::function<bool(uint64_t done, uint64_t total)>;

    ~Disc() { close(); }
    Kind kind() const { return m_kind; }
    const char* kindName() const { return m_kind == Kind::Folder ? "folder" : m_kind == Kind::Iso ? "disc image" : m_kind == Kind::Zip ? "zip" : "none"; }

    // Opens a source. On failure, error is a sentence for the player.
    bool open(const fs::path& source, std::string& error) {
        close();
        std::error_code ec;
        if (fs::is_directory(source, ec)) return openFolder(source, error);
        if (!fs::is_regular_file(source, ec)) {
            error = "The selected file or folder does not exist.";
            return false;
        }
        const std::string ext = detail::upper(utf8(source.extension()));
        if (ext == ".ZIP") return openZip(source, error);
        return openIso(source, error);
    }
    const File* find(const std::string& rel) const {
        auto it = m_files.find(detail::upper(rel));
        return it == m_files.end() ? nullptr : &it->second;
    }
    // The checks shared by both games: the disc must be The Sly Collection, decrypted. game: 1 or 2.
    bool checkSlyCollection(int game, std::string& error) const {
        const bool sly1 = find("SLY1/SLY.WAC") && find("SLY1/SLY.WAL");
        const bool sly2 = find("SLY2/SLY2.WAL");
        if (!sly1 && !sly2) {
            error = m_kind == Kind::Folder
                        ? "This folder is not The Sly Collection (PS3): no PS3_GAME/USRDIR with SLY1 and SLY2 in it."
                        : "This is not The Sly Collection (PS3): the disc has no SLY1 / SLY2 game files.";
            return false;
        }
        if ((game == 1 && !sly1) || (game == 2 && !sly2)) {
            error = std::string("The Sly Collection's ") + (game == 1 ? "SLY1/SLY.WAL and SLY.WAC are" : "SLY2/SLY2.WAL is") +
                    " missing from this source.";
            return false;
        }
        return true;
    }

    // Reads the wanted files (paths relative to USRDIR; each must exist).
    bool read(const std::vector<std::string>& rels, const Sink& sink, const Progress& progress, std::string& error) {
        std::vector<const File*> files;
        for (const auto& r : rels) {
            const File* f = find(r);
            if (!f) {
                error = "The disc has no " + r + ".";
                return false;
            }
            files.push_back(f);
        }
        if (m_kind == Kind::Folder) return readFolder(files, sink, progress, error);
        // the extents of every wanted file in disc order, each with its file and its offset in the file
        struct Piece { uint64_t at, size; size_t ifile; uint64_t offset; };
        std::vector<Piece> pieces;
        uint64_t total = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            uint64_t offset = 0;
            for (const Extent& e : files[i]->extents) {
                pieces.push_back({e.lba * 2048, e.size, i, offset});
                offset += e.size;
                total += e.size;
            }
        }
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.at < b.at; });
        if (m_kind == Kind::Iso) {
            std::vector<uint8_t> buffer(4 << 20);
            uint64_t done = 0;
            for (const Piece& p : pieces) {
                if (!detail::seek64(m_file, p.at)) {
                    error = "Cannot read the disc image.";
                    return false;
                }
                for (uint64_t o = 0; o < p.size;) {
                    const size_t n = size_t(std::min<uint64_t>(buffer.size(), p.size - o));
                    if (std::fread(buffer.data(), 1, n, m_file) != n) {
                        error = "The disc image is shorter than its file system says: it may be incomplete.";
                        return false;
                    }
                    if (!sink(p.ifile, p.offset + o, buffer.data(), n)) return true;
                    o += n;
                    done += n;
                    if (progress && !progress(done, total)) {
                        error = "Cancelled.";
                        return false;
                    }
                }
            }
            return true;
        }
        // .zip: one inflate pass over the image up to the end of the last wanted piece
        if (pieces.empty()) return true;
        const uint64_t end = pieces.back().at + pieces.back().size;
        struct State {
            const std::vector<Piece>* pieces;
            const Sink* sink;
            const Progress* progress;
            size_t ip = 0;
            uint64_t end = 0, lastReport = 0;
            bool stopped = false, cancelled = false;
        } st{&pieces, &sink, &progress, 0, end};
        auto callback = [](void* opaque, mz_uint64 at, const void* data, size_t n) -> size_t {
            State& s = *static_cast<State*>(opaque);
            const uint8_t* p = static_cast<const uint8_t*>(data);
            uint64_t a = at;
            const uint64_t b = at + n;
            while (s.ip < s.pieces->size() && a < b) {
                const Piece& pc = (*s.pieces)[s.ip];
                if (pc.at + pc.size <= a) {
                    ++s.ip;
                    continue;
                }
                if (pc.at >= b) break;
                const uint64_t from = std::max(a, pc.at), to = std::min(b, pc.at + pc.size);
                if (!(*s.sink)(pc.ifile, pc.offset + (from - pc.at), p + (from - at), size_t(to - from))) {
                    s.stopped = true;
                    return 0;
                }
                a = to;
                if (to == pc.at + pc.size) ++s.ip;
            }
            if (*s.progress && (b - s.lastReport >= (16u << 20) || b >= s.end)) {
                s.lastReport = b;
                if (!(*s.progress)(std::min(b, s.end), s.end)) {
                    s.cancelled = true;
                    return 0;
                }
            }
            if (s.ip >= s.pieces->size() || b >= s.end) {
                s.stopped = true;  // (everything wanted is read: the rest of the image is skipped)
                return 0;
            }
            return n;
        };
        if (!mz_zip_reader_extract_to_callback(m_zip.get(), m_zipIndex, callback, &st, 0) && !st.stopped) {
            error = st.cancelled ? "Cancelled." : std::string("Cannot unpack the disc image from the zip file (") +
                                                      mz_zip_get_error_string(mz_zip_get_last_error(m_zip.get())) + ").";
            return false;
        }
        return true;
    }
    // A whole small file (tables, PARAM.SFO).
    bool readAll(const std::string& rel, std::vector<uint8_t>& out, std::string& error) {
        const File* f = find(rel);
        if (!f) {
            error = "The disc has no " + rel + ".";
            return false;
        }
        out.assign(size_t(f->size), 0);
        return read({rel}, [&](size_t, uint64_t o, const uint8_t* p, size_t n) {
            std::memcpy(out.data() + o, p, std::min<size_t>(n, out.size() - size_t(o)));
            return true;
        }, nullptr, error);
    }

private:
    void close() {
        if (m_zip) mz_zip_reader_end(m_zip.get());
        m_zip.reset();
        if (m_file) std::fclose(m_file);
        m_file = nullptr;
        m_files.clear();
        m_kind = Kind::None;
    }
    // A folder: USRDIR is found from the disc's root, PS3_GAME, USRDIR, or one of its game folders.
    bool openFolder(const fs::path& source, std::string& error) {
        auto child = [](const fs::path& dir, const char* name) -> fs::path {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (detail::upper(utf8(e.path().filename())) == name && e.is_directory(ec)) return e.path();
            return {};
        };
        fs::path usrdir;
        const std::string name = detail::upper(utf8(absolutePath(source).filename()));
        if (name == "USRDIR") usrdir = source;
        else if (name == "PS3_GAME") usrdir = child(source, "USRDIR");
        else if (fs::path g = child(source, "PS3_GAME"); !g.empty()) usrdir = child(g, "USRDIR");
        else if ((name == "SLY1" || name == "SLY2" || name == "SLY3") && detail::upper(utf8(absolutePath(source).parent_path().filename())) == "USRDIR")
            usrdir = absolutePath(source).parent_path();
        else if (!child(source, "SLY1").empty() || !child(source, "SLY2").empty()) usrdir = source;
        if (usrdir.empty()) {
            error = "This folder is not The Sly Collection (PS3): choose the extracted disc (the folder with PS3_GAME in it), "
                    "its PS3_GAME or USRDIR folder, or the disc image.";
            return false;
        }
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(usrdir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 2) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            File f;
#if defined(__SWITCH__)
            // (lexically: newlib's std::filesystem mishandles "sdmc:/" paths, see absolutePath)
            f.path = detail::upper(utf8(it->path().lexically_relative(usrdir)));
#else
            f.path = detail::upper(utf8(fs::relative(it->path(), usrdir, ec)));
#endif
            std::replace(f.path.begin(), f.path.end(), '\\', '/');
            f.size = it->file_size(ec);
            f.local = it->path();
            m_files[f.path] = std::move(f);
        }
        m_kind = Kind::Folder;
        return true;
    }
    bool readFolder(const std::vector<const File*>& files, const Sink& sink, const Progress& progress, std::string& error) {
        uint64_t total = 0, done = 0;
        for (const File* f : files) total += f->size;
        std::vector<uint8_t> buffer(4 << 20);
        for (size_t i = 0; i < files.size(); ++i) {
            std::unique_ptr<FILE, int (*)(FILE*)> in(detail::openRead(files[i]->local), &std::fclose);
            if (!in) {
                error = "Cannot open " + utf8(files[i]->local) + ".";
                return false;
            }
            for (uint64_t o = 0;;) {
                const size_t n = std::fread(buffer.data(), 1, buffer.size(), in.get());
                if (n == 0) break;
                if (!sink(i, o, buffer.data(), n)) return true;
                o += n;
                done += n;
                if (progress && !progress(done, total)) {
                    error = "Cancelled.";
                    return false;
                }
            }
        }
        return true;
    }
    // ISO 9660 from the image's first sectors (the primary volume descriptor at sector 16, then the directories).
    // fetch(lba, count, out) reads sectors.
    template <class Fetch>
    bool parseIso(const Fetch& fetch, std::string& error) {
        std::vector<uint8_t> pvd;
        if (!fetch(16, 1, pvd) || pvd.size() < 2048 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0 || pvd[0] != 1) {
            error = "This is not a disc image of The Sly Collection (PS3): it has no ISO 9660 file system. If it is an "
                    "encrypted PS3 disc image, decrypt it first.";
            return false;
        }
        const uint8_t* root = pvd.data() + 156;
        std::vector<std::pair<std::string, Extent>> pending{{"", {detail::le32(root + 2), detail::le32(root + 10)}}};
        size_t visited = 0;
        while (!pending.empty()) {
            auto [prefix, dir] = pending.back();
            pending.pop_back();
            if (++visited > 4096 || dir.size > (16u << 20)) break;  // (a damaged image: stop)
            std::vector<uint8_t> data;
            if (!fetch(dir.lba, uint32_t((dir.size + 2047) / 2048), data)) {
                error = "Cannot read the disc image's file system.";
                return false;
            }
            for (size_t o = 0; o < data.size();) {
                const uint8_t len = data[o];
                if (len == 0) {
                    o = (o / 2048 + 1) * 2048;
                    continue;
                }
                if (len < 34 || o + len > data.size()) break;
                const uint8_t* r = data.data() + o;
                o += len;
                const uint8_t nameLen = r[32];
                if (33u + nameLen > len) continue;
                std::string name(reinterpret_cast<const char*>(r + 33), nameLen);
                if (nameLen == 1 && (name[0] == 0 || name[0] == 1)) continue;
                if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
                name = detail::upper(name);
                const Extent e{detail::le32(r + 2), detail::le32(r + 10)};
                if (r[25] & 2) {
                    pending.push_back({prefix + name + "/", e});
                    continue;
                }
                // PS3_GAME/USRDIR/... files only, kept relative to USRDIR (multi-extent: the extents follow in order)
                const std::string full = prefix + name;
                static const std::string usr = "PS3_GAME/USRDIR/";
                if (full.compare(0, usr.size(), usr) != 0) {
                    if (full == "PS3_GAME/PARAM.SFO") {
                        File& f = m_files["../PARAM.SFO"];
                        f.path = "../PARAM.SFO";
                        f.size = e.size;
                        f.extents = {e};
                    }
                    continue;
                }
                File& f = m_files[full.substr(usr.size())];
                f.path = full.substr(usr.size());
                f.size += e.size;
                f.extents.push_back(e);
            }
        }
        if (m_files.empty()) {
            error = "This disc image is not The Sly Collection (PS3): it has no PS3_GAME/USRDIR folder.";
            return false;
        }
        return true;
    }
    bool openIso(const fs::path& source, std::string& error) {
        m_file = detail::openRead(source);
        if (!m_file) {
            error = "Cannot open " + utf8(source) + ".";
            return false;
        }
        auto fetch = [&](uint64_t lba, uint32_t count, std::vector<uint8_t>& out) {
            out.assign(size_t(count) * 2048, 0);
            return detail::seek64(m_file, lba * 2048) && std::fread(out.data(), 1, out.size(), m_file) == out.size();
        };
        if (!parseIso(fetch, error)) return false;
        m_kind = Kind::Iso;
        return true;
    }
    bool openZip(const fs::path& source, std::string& error) {
        m_file = detail::openRead(source);
        std::error_code ec;
        const uint64_t size = fs::file_size(source, ec);
        m_zip = std::make_unique<mz_zip_archive>();
        mz_zip_zero_struct(m_zip.get());
        if (!m_file || !mz_zip_reader_init_cfile(m_zip.get(), m_file, size, 0)) {
            error = "Cannot read the zip file: it is damaged or not a zip file.";
            m_zip.reset();
            return false;
        }
        // the disc image: the largest .iso in the zip
        uint64_t best = 0;
        bool found = false;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(m_zip.get()); ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(m_zip.get(), i, &st) || st.m_is_directory) continue;
            const std::string name = detail::upper(st.m_filename);
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ISO") == 0 && st.m_uncomp_size > best) {
                best = st.m_uncomp_size;
                m_zipIndex = i;
                found = true;
            }
        }
        if (!found) {
            error = "This zip file has no disc image (.iso) in it.";
            return false;
        }
        // The file system is in the image's first megabytes: inflate up to 64 MB of it, then parse from that.
        std::vector<uint8_t> head;
        struct Head { std::vector<uint8_t>* out; uint64_t limit; } h{&head, 64ull << 20};
        auto take = [](void* opaque, mz_uint64 at, const void* data, size_t n) -> size_t {
            Head& s = *static_cast<Head*>(opaque);
            if (at >= s.limit) return 0;
            const size_t keep = size_t(std::min<uint64_t>(n, s.limit - at));
            s.out->insert(s.out->end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + keep);
            return keep == n ? n : 0;
        };
        mz_zip_reader_extract_to_callback(m_zip.get(), m_zipIndex, take, &h, 0);
        if (head.size() < 17 * 2048) {
            error = std::string("Cannot unpack the disc image from the zip file (") +
                    mz_zip_get_error_string(mz_zip_get_last_error(m_zip.get())) + ").";
            return false;
        }
        auto fetch = [&](uint64_t lba, uint32_t count, std::vector<uint8_t>& out) {
            const uint64_t a = lba * 2048, b = a + uint64_t(count) * 2048;
            if (b > head.size()) return false;
            out.assign(head.begin() + ptrdiff_t(a), head.begin() + ptrdiff_t(b));
            return true;
        };
        if (!parseIso(fetch, error)) return false;
        m_kind = Kind::Zip;
        return true;
    }

    Kind m_kind = Kind::None;
    FILE* m_file = nullptr;
    std::unique_ptr<mz_zip_archive> m_zip;
    mz_uint m_zipIndex = 0;
    std::map<std::string, File> m_files;
};
}  // namespace cane::ps3
