#pragma once
// A language exported as a mod package (the Translate page's "Export as a mod package", loc_page.h): everything the
// game needs to show and play the language, as Sly 2 reads it on every platform (platform/mod_package.h,
// Sly2-Release/langstore.cpp):
//
//   <Mods>/Packages/language-<name>/mod.json
//   <Mods>/Packages/language-<name>/files/Languages/<Name>/   Text.json, language.json, Font.bin,
//                                                             Dialog/**/<line>.flac|.wav|.vag, Dialog/lengths.json,
//                                                             Movies/<name>.flac|.wav, (when asked) Takes/
//
// and the same as <id>.zip (mod.json at its root, as the map editor's; UTF-8 names, so "Føroyskt" stays itself on every
// platform). The dry takes (Takes/: the tool's own, the game never reads them) stay out unless asked for. A WAV dub
// becomes a FLAC (lossless, about half the size: the game reads both, as the European installations' lines) through
// the game program (Sly2-Release --encode-flac LIST); a file that does not convert stays a WAV.
// No ImGui here; `safehouse --loc-selftest` checks it (exportSelfTest).
#include "loc_core.h"
#include "../../platform/mod_package.h"

#include <miniz.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace cane::loc {

struct ExportOptions {
    bool takes = false;   // the dry takes too (Takes/)
    bool flac = true;     // WAV dubs as FLAC
    bool zip = true;      // also <id>.zip
};
// One file of the package: its source and its path under files/Languages/<Name>/ ("Dialog/Paris/1259.wav")
struct PackFile {
    fs::path src;
    std::string rel;
    uint64_t bytes = 0;
};
inline std::string lowerExt(const fs::path& p) {
    std::string e = utf8Of(p.extension());
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}
// The files of a language's folders (later folders win, as in the game): the ones the game reads, and Takes/ when asked.
// A dialog line is one file whatever its format or subfolder (the later folder's), as the game keeps one per id.
inline std::vector<PackFile> packageFiles(const std::vector<fs::path>& dirs, bool takes) {
    std::map<std::string, PackFile> byRel;
    std::map<std::string, std::string> lineRel;   // dialog line id -> its rel (one per id)
    std::error_code ec;
    auto add = [&](const fs::path& file, const std::string& rel) {
        PackFile f{file, rel, fs::file_size(file, ec)};
        if (ec) f.bytes = 0;
        byRel[rel] = f;
    };
    for (const fs::path& d : dirs) {
        for (const char* name : {"Text.json", "language.json", "Font.bin"})
            if (fs::is_regular_file(d / name, ec)) add(d / name, name);
        for (const char* sub : {"Dialog", "Movies", "Takes"}) {
            const bool dialog = !std::strcmp(sub, "Dialog"), takesDir = !std::strcmp(sub, "Takes");
            if ((takesDir && !takes) || !fs::is_directory(d / sub, ec)) continue;
            for (auto it = fs::recursive_directory_iterator(d / sub, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const std::string name = utf8Of(it->path().filename());
                if (name.empty() || name[0] == '.' || lowerExt(it->path()) == ".tmp") continue;
                const std::string rel = utf8Of(it->path().lexically_relative(d).generic_u8string());
                if (takesDir) {
                    if (audioFile(it->path()) || name == "takes.json") add(it->path(), rel);
                    continue;
                }
                if (dialog && name == "lengths.json" && it->path().parent_path() == d / sub) {
                    add(it->path(), rel);
                    continue;
                }
                if (!audioFile(it->path())) continue;
                if (dialog) {
                    const std::string stem = utf8Of(it->path().stem());
                    if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos) continue;
                    if (auto old = lineRel.find(stem); old != lineRel.end()) byRel.erase(old->second);
                    lineRel[stem] = rel;
                } else {
                    // (a movie's sound: one per name too)
                    const std::string stemRel = utf8Of(fs::path(pathOf(rel)).replace_extension().generic_u8string());
                    for (auto jt = byRel.begin(); jt != byRel.end();)
                        jt = jt->first != rel && jt->first.rfind("Movies/", 0) == 0 &&
                                     utf8Of(fs::path(pathOf(jt->first)).replace_extension().generic_u8string()) == stemRel
                                 ? byRel.erase(jt)
                                 : std::next(jt);
                }
                add(it->path(), rel);
            }
        }
    }
    std::vector<PackFile> out;
    for (auto& [rel, f] : byRel) out.push_back(std::move(f));
    return out;
}
// A WAV the export turns into a FLAC: a dub or a movie's sound (the dry takes stay WAV: the tool reads them)
inline bool convertible(const PackFile& f) {
    return lowerExt(f.src) == ".wav" && (f.rel.rfind("Dialog/", 0) == 0 || f.rel.rfind("Movies/", 0) == 0);
}

// The export's progress, read by the page while the worker runs
struct ExportProgress {
    std::atomic<int> phase{0};          // 0 waiting, 1 copying, 2 converting, 3 zipping, 4 done, 5 failed
    std::atomic<int> done{0}, total{0};
    std::atomic<uint64_t> folderBytes{0}, zipBytes{0}, sourceBytes{0};
    std::atomic<int> files{0}, dubs{0}, converted{0};
    std::atomic<bool> cancel{false};
    std::mutex m;
    std::string error, folder, zip;     // (under m)
};
// The encoder: runs the game program on a list file ("IN<tab>OUT" lines), calls line() with each line it prints;
// its exit code. Empty: no conversion.
using Encoder = std::function<int(const fs::path& list, const std::function<void(const std::string&)>& line)>;

inline uint64_t treeBytes(const fs::path& dir) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) n += fs::file_size(it->path(), ec);
    }
    return n;
}
inline bool readFile(const fs::path& file, std::vector<char>& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
// The package folder as a .zip (mod.json at its root; UTF-8 names; the FLAC / VAG files stored, the rest deflated)
inline bool zipFolder(const fs::path& dir, const fs::path& zip, ExportProgress& p, std::string& error) {
    std::error_code ec;
    fs::create_directories(zip.parent_path(), ec);
    const fs::path tmp = zip.parent_path() / pathOf(utf8Of(zip.filename()) + ".tmp");
    mz_zip_archive z;
    std::memset(&z, 0, sizeof(z));
    if (!mz_zip_writer_init_file(&z, utf8Of(tmp).c_str(), 0)) {
        error = "cannot write " + utf8Of(zip);
        return false;
    }
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    p.done = 0;
    p.total = int(files.size());
    bool ok = true;
    std::vector<char> ab;
    for (const fs::path& f : files) {
        if (p.cancel) { ok = false; error = "cancelled"; break; }
        const std::string name = utf8Of(f.lexically_relative(dir).generic_u8string());
        const std::string e = lowerExt(f);
        const mz_uint level = e == ".flac" || e == ".vag" ? MZ_NO_COMPRESSION : MZ_DEFAULT_COMPRESSION;
        if (!readFile(f, ab) || !mz_zip_writer_add_mem(&z, name.c_str(), ab.data(), ab.size(), level)) {
            ok = false;
            error = "cannot add " + name + " to " + utf8Of(zip);
            break;
        }
        ++p.done;
    }
    ok = mz_zip_writer_finalize_archive(&z) && ok;
    mz_zip_writer_end(&z);
    if (!ok) {
        fs::remove(tmp, ec);
        if (error.empty()) error = "cannot write " + utf8Of(zip);
        return false;
    }
    fs::rename(tmp, zip, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

// Exports `files` (packageFiles) as the package `m` (m.dir: its folder in Mods/Packages, replaced as a whole once the
// new one is complete) for the language `key`, and as `zip` when asked; the progress in p.
inline bool exportLanguage(const std::vector<PackFile>& files, cane::mods::Manifest m, const std::string& key, const ExportOptions& o,
                           const fs::path& zip, const Encoder& encode, ExportProgress& p) {
    std::error_code ec;
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> lock(p.m);
        p.error = e;
        p.phase = 5;
        return false;
    };
    const fs::path packages = m.dir.parent_path();
    const fs::path stage = packages / pathOf("." + m.id + ".export");
    fs::remove_all(stage, ec);
    const fs::path finalDir = m.dir;
    m.dir = stage;
    std::string error;
    if (!cane::mods::writeManifest(m, error)) return fail(error);
    const fs::path dest = m.filesDir() / "Languages" / pathOf(key);
    // copying (the WAVs to convert: listed instead)
    p.phase = 1;
    p.done = 0;
    p.total = int(files.size());
    uint64_t source = 0;
    int dubs = 0;
    std::vector<std::pair<const PackFile*, fs::path>> toFlac;
    for (const PackFile& f : files) {
        if (p.cancel) return fail("cancelled");
        source += f.bytes;
        if (f.rel.rfind("Dialog/", 0) == 0 || f.rel.rfind("Movies/", 0) == 0) dubs += audioFile(f.src) ? 1 : 0;
        const fs::path out = dest / pathOf(f.rel);
        fs::create_directories(out.parent_path(), ec);
        if (o.flac && encode && convertible(f)) {
            toFlac.emplace_back(&f, fs::path(out).replace_extension(".flac"));
            ++p.done;
            continue;
        }
        fs::copy_file(f.src, out, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("cannot copy " + utf8Of(f.src) + ": " + ec.message());
        ++p.done;
    }
    p.sourceBytes = source;
    p.files = int(files.size());
    p.dubs = dubs;
    // converting: the game program, one run for every file
    if (!toFlac.empty()) {
        p.phase = 2;
        p.done = 0;
        p.total = int(toFlac.size());
        const fs::path list = stage / "encode.txt";
        {
            std::ofstream out(list, std::ios::binary | std::ios::trunc);
            for (const auto& [f, flac] : toFlac) out << utf8Of(f->src) << '\t' << utf8Of(flac) << '\n';
            if (!out) return fail("cannot write " + utf8Of(list));
        }
        try {
            encode(list, [&](const std::string& line) {
                if (line.rfind("progress ", 0) == 0) p.done = std::atoi(line.c_str() + 9);
            });
        } catch (...) {
        }
        fs::remove(list, ec);
        if (p.cancel) return fail("cancelled");
        // (a file that did not convert: the WAV as it is)
        int converted = 0;
        for (const auto& [f, flac] : toFlac) {
            const double s = audioSeconds(flac);
            if (s >= 0 && std::abs(s - audioSeconds(f->src)) < 0.05) {
                ++converted;
                continue;
            }
            fs::remove(flac, ec);
            fs::copy_file(f->src, dest / pathOf(f->rel), fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("cannot copy " + utf8Of(f->src) + ": " + ec.message());
        }
        p.converted = converted;
    }
    // the new package in place of the old one
    fs::remove_all(finalDir, ec);
    fs::rename(stage, finalDir, ec);
    if (ec) return fail("cannot replace " + utf8Of(finalDir) + ": " + ec.message());
    p.folderBytes = treeBytes(finalDir);
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.folder = utf8Of(finalDir);
    }
    if (o.zip) {
        p.phase = 3;
        if (!zipFolder(finalDir, zip, p, error)) return fail(error);
        p.zipBytes = fs::file_size(zip, ec);
        std::lock_guard<std::mutex> lock(p.m);
        p.zip = utf8Of(zip);
    }
    p.phase = 4;
    return true;
}
// "12.3 MB"
inline std::string sizeText(uint64_t bytes) {
    char b[32];
    if (bytes >= (1ull << 30)) std::snprintf(b, sizeof(b), "%.2f GB", double(bytes) / double(1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(b, sizeof(b), "%.1f MB", double(bytes) / double(1ull << 20));
    else std::snprintf(b, sizeof(b), "%.0f KB", std::max(1.0, double(bytes) / 1024.0));
    return b;
}

// The export on made-up folders (part of safehouse --loc-selftest): the files the game reads, Takes/ only when asked,
// one file per line, a fake encoder's FLACs, the folder and the zip with the language's UTF-8 name. 0 when all pass.
inline int exportSelfTest() {
    int fails = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) ++fails, std::printf("FAIL %s\n", what);
    };
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("safehouse-locexp-" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
    const fs::path lang = root / "langs" / pathOf("Føroyskt"), mods = root / "Mods" / "Languages" / pathOf("Føroyskt");
    auto put = [&](const fs::path& f, const std::string& s) {
        fs::create_directories(f.parent_path(), ec);
        std::ofstream(f, std::ios::binary) << s;
    };
    // (a 16-bit mono WAV of 0.5 s)
    std::string wav = "RIFF....WAVEfmt ";
    auto u32 = [](uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
    auto u16 = [](uint32_t v) { return std::string{char(v), char(v >> 8)}; };
    wav += u32(16) + u16(1) + u16(1) + u32(22050) + u32(44100) + u16(2) + u16(16) + "data" + u32(22050) + std::string(22050, '\0');
    put(lang / "Text.json", "{\"strings\": {}}");
    put(lang / "language.json", "{\"name\": \"Føroyskt\"}");
    put(lang / "Føroyskt.csv", "id,reference,translation\n");
    put(lang / "Dialog" / "Cairo Museum" / "1179.wav", wav);
    put(lang / "Dialog" / "Shared" / "1181.wav", wav);
    put(lang / "Dialog" / "lengths.json", "{\"1179\": 2.5}");
    put(lang / "Dialog" / "notes.txt", "x");
    put(lang / "Takes" / "1179.wav", wav);
    put(lang / "Takes" / "takes.json", "{}");
    put(lang / "Movies" / "Ep1 Intro.wav", wav);
    put(mods / "Dialog" / "Paris" / "1181.flac", "fLaC");   // (a mod folder's line wins over the folder's)
    const auto files = packageFiles({lang, mods}, false);
    std::set<std::string> rels;
    for (const PackFile& f : files) rels.insert(f.rel);
    expect(rels == std::set<std::string>{"Dialog/Cairo Museum/1179.wav", "Dialog/Paris/1181.flac", "Dialog/lengths.json", "Movies/Ep1 Intro.wav", "Text.json", "language.json"},
           "the package's files: the game's, one per line, no takes, no table or notes");
    expect(packageFiles({lang}, true).size() == 8, "the dry takes when asked");
    cane::mods::Manifest m;
    m.id = "language-f-royskt";
    m.name = "Føroyskt";
    m.game = "sly2";
    m.dir = root / "Mods" / "Packages" / m.id;
    put(m.dir / "files" / "old.txt", "an older export");
    ExportProgress p;
    // (the fake encoder: a FLAC header of the WAV's length for every line)
    const Encoder fake = [&](const fs::path& list, const std::function<void(const std::string&)>& line) {
        std::ifstream in(list, std::ios::binary);
        std::vector<std::string> outs;
        for (std::string s; std::getline(in, s);) outs.push_back(s.substr(s.find('\t') + 1));
        for (std::size_t i = 0; i < outs.size(); ++i) {
            std::string h = "fLaC";
            h += std::string{char(0x80), 0, 0, 34};
            h += std::string(10, '\0');
            const uint64_t x = (uint64_t(22050) << 44) | (uint64_t(0) << 41) | (uint64_t(15) << 36) | 11025;
            for (int k = 7; k >= 0; --k) h += char(x >> (k * 8));
            h += std::string(16, '\0');
            put(pathOf(outs[i]), h);
            line("progress " + std::to_string(i + 1) + " " + std::to_string(outs.size()));
        }
        return 0;
    };
    ExportOptions o;
    const fs::path zip = root / "Exports" / "language-f-royskt.zip";
    expect(exportLanguage(files, m, "Føroyskt", o, zip, fake, p) && p.phase == 4, "the export runs");
    const fs::path in = m.dir / "files" / "Languages" / pathOf("Føroyskt");
    expect(fs::is_regular_file(in / "Dialog" / "Cairo Museum" / "1179.flac", ec) && !fs::exists(in / "Dialog" / "Cairo Museum" / "1179.wav", ec) &&
               fs::is_regular_file(in / "Movies" / "Ep1 Intro.flac", ec) && fs::is_regular_file(in / "Dialog" / "Paris" / "1181.flac", ec) &&
               fs::is_regular_file(in / "Dialog" / "lengths.json", ec) && !fs::exists(in / "Takes", ec) && !fs::exists(m.dir / "files" / "old.txt", ec) &&
               p.converted == 2 && p.dubs == 3,
           "the package folder: the WAV dubs as FLAC, the old export replaced");
    cane::mods::Manifest back;
    std::string error;
    expect(cane::mods::readManifest(m.dir, back, error) && back.id == m.id && back.game == "sly2", "its mod.json");
    mz_zip_archive z;
    std::memset(&z, 0, sizeof(z));
    bool named = false, utf8Flag = false;
    if (mz_zip_reader_init_file(&z, utf8Of(zip).c_str(), 0)) {
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            if (std::string(st.m_filename) == "files/Languages/Føroyskt/Dialog/Cairo Museum/1179.flac") named = true, utf8Flag = (st.m_bit_flag & (1 << 11)) != 0;
        }
        mz_zip_reader_end(&z);
    }
    expect(named && utf8Flag && p.zipBytes > 0, "the zip: mod.json at its root, the UTF-8 names flagged as UTF-8");
    // without the encoder (no game program): the WAVs as they are
    ExportProgress q;
    o.zip = false;
    expect(exportLanguage(files, m, "Føroyskt", o, zip, Encoder(), q) && fs::is_regular_file(in / "Dialog" / "Cairo Museum" / "1179.wav", ec), "no encoder: the WAVs kept");
    fs::remove_all(root, ec);
    std::printf("loc export selftest: %d failed\n", fails);
    return fails ? 1 : 0;
}

}  // namespace cane::loc
