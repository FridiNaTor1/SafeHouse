#pragma once
// What the Collection Style Launcher shows and plays, found on this computer (nothing of the games ships with SafeHouse):
//   - The Sly Collection (PS3) menu files, when the player has the disc (folder, .iso or .zip of the .iso, as the PS3
//     HD import reads it): its menu GUI archive (IMAGE.ZIP: the game logos, the banner and panel frames, the loading
//     screen), its menu music bank (AUDIO/SLYAUDIO_MUSIC.FSB: "FrontEnd_Loop" and the other menu tracks), its interface
//     sound bank and the disc's XMB background (PS3_GAME/PIC1.PNG). They are copied once into the profile
//     (<profile>/Collection/PS3 menu), never into the installation or the program folder.
//   - Otherwise (and as extras) the installed games: Sly 2's music (Music/*.wav: the episode menu, Paris, the hub...)
//     and its sky panoramas (the installation's texture dump, Textures/WxH_*.png with W = 4 H or wider; the PS3 HD
//     pack's copy of the same texture when there is one).
// The menu still works with none of this: the scene is drawn, the logos are the launcher's own.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <miniz.h>
#include <stb/stb_image.h>

#include "audio.h"
#include "install.h"
#include "platform.h"
#include "ps3disc.h"
#include "../l10n.h"

namespace cane::collection {
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return rgba.empty(); }
};
inline Image decodeImage(const uint8_t* data, size_t size) {
    Image img;
    int n = 0;
    if (unsigned char* p = stbi_load_from_memory(data, int(size), &img.width, &img.height, &n, 4)) {
        img.rgba.assign(p, p + size_t(img.width) * size_t(img.height) * 4);
        stbi_image_free(p);
    }
    return img;
}
inline std::vector<uint8_t> readBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- the PS3 menu files (import and cache)
// The files copied from the disc, relative to USRDIR ("../" = PS3_GAME). Only IMAGE.ZIP is required.
inline const std::vector<std::string>& ps3MenuFiles() {
    static const std::vector<std::string> files{"IMAGE.ZIP", "AUDIO/SLYAUDIO_MUSIC.FSB", "AUDIO/SLYAUDIO_BANK.FSB", "../PIC1.PNG"};
    return files;
}
inline std::string cacheName(const std::string& rel) {
    const auto slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}
inline fs::path ps3CacheDir(const fs::path& profile) { return profile / "Collection" / "PS3 menu"; }
inline bool ps3Imported(const fs::path& profile) {
    std::error_code ec;
    return fs::is_regular_file(ps3CacheDir(profile) / "IMAGE.ZIP", ec);
}
// Copies the menu files from a Sly Collection source. Throws with a sentence for the player.
inline void importPs3Menu(const fs::path& source, const fs::path& profile, const std::atomic<bool>& cancel,
                          const std::function<void(float)>& progress) {
    ps3::Disc disc;
    std::string error;
    if (!disc.open(source, error)) throw std::runtime_error(error);
    if (!disc.find("IMAGE.ZIP") || !disc.find("SLY2/SLY2.WAL"))
        throw std::runtime_error(tr("This is not The Sly Collection (PS3): its menu files (IMAGE.ZIP) are missing."));
    std::vector<std::string> wanted;
    for (const auto& f : ps3MenuFiles())
        if (disc.find(f)) wanted.push_back(f);
    const fs::path dir = ps3CacheDir(profile), staging = dir.parent_path() / "PS3 menu.staging";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging);
    std::vector<std::unique_ptr<std::ofstream>> out;
    for (const auto& f : wanted) out.push_back(std::make_unique<std::ofstream>(staging / cacheName(f), std::ios::binary));
    const bool ok = disc.read(
        wanted,
        [&](size_t i, uint64_t, const uint8_t* p, size_t n) {
            out[i]->write(reinterpret_cast<const char*>(p), std::streamsize(n));
            return !cancel.load();
        },
        [&](uint64_t done, uint64_t total) {
            if (progress && total) progress(float(double(done) / double(total)));
            return !cancel.load();
        },
        error);
    out.clear();
    if (!ok || cancel) {
        fs::remove_all(staging, ec);
        throw std::runtime_error(cancel ? tr("Cancelled.") : error);
    }
    writeJson(staging / "source.json", json{{"source", utf8(source)}, {"files", wanted}});
    fs::remove_all(dir, ec);
    fs::rename(staging, dir);
}
inline void removePs3Menu(const fs::path& profile) {
    std::error_code ec;
    fs::remove_all(ps3CacheDir(profile), ec);
}

// ---------------------------------------------------------------- what was found
// The images of the menu, by role. Each may be empty.
struct MenuImages {
    Image logos[6];         // the Collection, Sly 1, Sly 2, Sly 3 (the PS3's game icons), [4..5] unused
    Image background;       // PIC1.PNG: the options' backdrop
    Image loading;          // the loading screen's mask (Screen_LoadingWide)
    Image banner[3];        // left, centre, right of the "Select Game" banner
    Image frame[9];         // the panel frame: left-top, top, right-top, left, centre, right, left-bottom, bottom, right-bottom
    std::vector<Image> skies;  // sky panoramas (Sly 2's), cycled behind the scene
};
// The music and sounds found.
struct MenuSounds {
    PcmPtr theme;                 // the collection's own menu music (PS3 "FrontEnd_Loop")
    std::vector<PcmPtr> quiet;    // calmer tracks (no games installed, the options)
    std::map<int, std::vector<PcmPtr>> games;  // per game (GameId as int): tracks played while it is chosen
    PcmPtr move, select, locked, back;          // interface sounds
};

// Reads the PS3 cache: the images now, the music lazily (decoding is the slow part: see loadPs3Music).
inline MenuImages loadPs3Images(const fs::path& profile) {
    MenuImages m;
    const fs::path dir = ps3CacheDir(profile);
    const auto zipBytes = readBytes(dir / "IMAGE.ZIP");
    if (zipBytes.empty()) return m;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) return m;
    auto load = [&](const char* name) {
        Image img;
        const int i = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
        if (i < 0) return img;
        size_t n = 0;
        if (void* p = mz_zip_reader_extract_to_heap(&zip, mz_uint(i), &n, 0)) {
            img = decodeImage(static_cast<const uint8_t*>(p), n);
            mz_free(p);
        }
        return img;
    };
    // The XMB icons are the logos on black inside a thin white frame: crop the frame, key the black out.
    auto logo = [&](const char* name) {
        Image img = load(name);
        if (img.empty()) return img;
        const int mx = img.width * 6 / 100, my = img.height * 10 / 100;
        Image out;
        out.width = img.width - 2 * mx, out.height = img.height - 2 * my;
        out.rgba.resize(size_t(out.width) * size_t(out.height) * 4);
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x) {
                const uint8_t* p = &img.rgba[(size_t(y + my) * size_t(img.width) + size_t(x + mx)) * 4];
                uint8_t* q = &out.rgba[(size_t(y) * size_t(out.width) + size_t(x)) * 4];
                const int peak = std::max({int(p[0]), int(p[1]), int(p[2])});
                const int key = std::clamp((peak - 10) * 255 / 46, 0, 255);
                q[0] = p[0], q[1] = p[1], q[2] = p[2], q[3] = uint8_t(int(p[3]) * key / 255);
            }
        return out;
    };
    m.logos[0] = logo("System/SlyCollection/icon0.png");
    m.logos[1] = logo("System/Sly1/icon0.png");
    m.logos[2] = logo("System/Sly2/icon0.png");
    m.logos[3] = logo("System/Sly3/icon0.png");
    m.loading = load("Gui/Screen_LoadingWide.tga");
    m.banner[0] = load("Gui/Banner_Left.tga");
    m.banner[1] = load("Gui/Banner_Center.tga");
    m.banner[2] = load("Gui/Banner_Right.tga");
    static const char* frames[9] = {"Gui/Border_NILeftTop.tga",    "Gui/Border_NICenterTop.tga",    "Gui/Border_NIRightTop.tga",
                                    "Gui/Border_NILeft.tga",       "Gui/Border_NICenter.tga",       "Gui/Border_NIRight.tga",
                                    "Gui/Border_NILeftBottom.tga", "Gui/Border_NICenterBottom.tga", "Gui/Border_NIRightBottom.tga"};
    for (int i = 0; i < 9; ++i) m.frame[i] = load(frames[i]);
    mz_zip_reader_end(&zip);
    const auto pic = readBytes(dir / "PIC1.PNG");
    if (!pic.empty()) m.background = decodeImage(pic.data(), pic.size());
    return m;
}
// The PS3 menu music and interface sounds (a few seconds of decoding: call from a worker).
inline void loadPs3Sounds(const fs::path& profile, MenuSounds& s) {
    const fs::path dir = ps3CacheDir(profile);
    const auto music = readBytes(dir / "SLYAUDIO_MUSIC.FSB");
    for (const auto& sample : readFsb(music)) {
        auto track = [&] { return std::make_shared<const Pcm>(decodeFsbSample(music, sample)); };
        if (sample.name == "FrontEnd_Loop") s.theme = track();
        else if (sample.name == "dvd_menu_smooth_L" || sample.name == "dvd_menu_clarinet") s.quiet.push_back(track());
        else if (sample.name == "clockclm_normal" || sample.name == "mugruins_Action") s.games[0].push_back(track());
        else if (sample.name == "f_nightclub_disco") s.games[1].push_back(track());
        else if (sample.name == "i_trainer_L") s.games[2].push_back(track());
    }
    const auto bank = readBytes(dir / "SLYAUDIO_BANK.FSB");
    for (const auto& sample : readFsb(bank)) {
        auto sound = [&] {
            Pcm p = decodeFsbSample(bank, sample);
            p.loops = false;
            return std::make_shared<const Pcm>(std::move(p));
        };
        if (sample.name == "target_flip_01") s.move = sound();
        else if (sample.name == "positive_03") s.select = sound();
        else if (sample.name == "negative_tone_01") s.locked = sound();
        else if (sample.name == "coin_pull_01") s.back = sound();
    }
}

// Sly 2's installed music: the tracks the menu plays (by their friendly names, Sly2-Release/convertmusic.inc).
inline void loadSly2Music(const fs::path& installation, MenuSounds& s, bool asTheme) {
    std::error_code ec;
    const fs::path music = installation / "Music";
    if (!fs::is_directory(music, ec)) return;
    auto find = [&](const char* name) -> PcmPtr {
        for (const auto& e : fs::directory_iterator(music, ec)) {
            const std::string f = utf8(e.path().filename());
            if (f.size() > 3 && f.substr(3) == std::string(name) + ".wav") {
                Pcm p = decodeWav(readBytes(e.path()));
                if (!p.samples.empty()) return std::make_shared<const Pcm>(std::move(p));
            }
        }
        return nullptr;
    };
    if (auto t = find("Episode Menu A")) {
        if (asTheme && !s.theme) s.theme = t;
        else s.games[1].push_back(t);
    }
    if (auto t = find("Paris")) s.games[1].push_back(t);
    if (auto t = find("Episode Menu B")) s.quiet.push_back(t);
}
// Sky panoramas in a texture folder (Textures/WxH_<hash>.png, W >= 4 H, at least 512 wide), at most `limit`, the PS3
// HD pack's copy (same name) preferred.
inline std::vector<Image> loadSkies(const fs::path& textures, const fs::path& hdTextures, size_t limit = 8) {
    std::vector<Image> out;
    std::error_code ec;
    if (!fs::is_directory(textures, ec)) return out;
    std::vector<fs::path> names;
    for (const auto& e : fs::directory_iterator(textures, ec)) {
        int w = 0, h = 0;
        if (std::sscanf(utf8(e.path().filename()).c_str(), "%dx%d_", &w, &h) == 2 && h > 0 && w >= 4 * h && w >= 512)
            names.push_back(e.path().filename());
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        if (out.size() >= limit) break;
        const fs::path hd = hdTextures / n;
        const auto bytes = readBytes(!hdTextures.empty() && fs::is_regular_file(hd, ec) ? hd : textures / n);
        Image img = decodeImage(bytes.data(), bytes.size());
        // (a panorama is mostly opaque: skip masks and strips)
        size_t opaque = 0;
        for (size_t i = 3; i < img.rgba.size(); i += 4) opaque += img.rgba[i] > 200;
        if (!img.empty() && opaque * 4 * 2 > img.rgba.size()) {
            for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
            out.push_back(std::move(img));
        }
    }
    return out;
}
// Fallback interface sounds.
inline void synthesizeSounds(MenuSounds& s) {
    if (!s.move) s.move = std::make_shared<const Pcm>(chime(880, 1175, 0.09f, 0.16f));
    if (!s.select) s.select = std::make_shared<const Pcm>(chime(784, 1568, 0.35f, 0.22f));
    if (!s.locked) s.locked = std::make_shared<const Pcm>(chime(220, 185, 0.22f, 0.22f));
    if (!s.back) s.back = std::make_shared<const Pcm>(chime(988, 659, 0.14f, 0.16f));
}
}  // namespace cane::collection
