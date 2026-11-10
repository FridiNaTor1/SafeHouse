#pragma once
// The Collection Style Launcher (`safehouse --collection`): The Sly Collection's (PS3) own game-select menu as a way
// to start the games. Its scene, art, letters, sounds and music are the player's own, imported from their disc
// (ps3menu.h); without that import this menu does not exist (main.cpp opens the SafeHouse launcher instead).
//
// As on the PS3: one page per camera of the scene, Left / Right turn the page at once and the camera glides there
// (position += (target - position) * 0.1, sixty times a second), the page takes no input for half a second, the van
// rides along under the logo, and choosing a game fades to black in half a second and starts it. When the game quits
// the menu comes back (the games show "Quit to Launcher" in their pause menus: SAFEHOUSE_LAUNCHER=collection).
//
// Pages, left to right:
//   Extras   (PS3: two bordered buttons, Credits and a teaser)   Options, Credits, SafeHouse launcher, Quit
//   Title    the collection's logo                                Cross: on to Sly 1
//   Sly 1, Sly 2, Sly 3                                           Cross: play. Not installed: the logo in the dark with
//                                                                 the menu's padlock and a note; Sly 3: a note
//   More     (PS3: the minigames, four round icons, locked)       the four icons, locked: Sly 4 and Bentley's Hackpack
//                                                                 (nothing to install yet) and the PS3's own minigames
// SafeHouse's additions use the menu's own pieces: its bordered buttons, its note frame, its banner, its padlock, its
// letters (the launcher's typeface where a language has letters the menu's font lacks).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui/ImGuiFileDialog.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "../backend.h"
#include "../credits.h"
#include "../native_dialog.h"
#include "../ui/face_buttons.h"
#include "../ui/gamepad_nav.h"
#include "../ui/job.h"
#include "../ui/prompt_bar.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../steam.h"
#include "audio.h"
#include "bloom.h"
#include "ps3menu.h"
#include "scene.h"
#include "brand.h"
#include "controller_family.h"
#include "sly2_settings.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cane::collection {
using namespace cane::settings;  // (Sly 2's Settings.ini: platform/sly2_settings.h)
// The environment variable a game started from here sees ("collection"): its pause menu says Quit to Launcher.
inline constexpr const char* kLauncherEnv = "SAFEHOUSE_LAUNCHER";
inline void setEnv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1);
    else unsetenv(key);
#endif
}
// Starts a program on its own (not waited for): the SafeHouse launcher from here, or this menu from Settings.
inline bool spawnDetached(const fs::path& program, const std::vector<std::string>& args) {
#if defined(CANE_ANDROID)
    // Android: the launcher or the direct activity (--collection) of the app (android_app.h startApp); no exec
    (void)program;
    return android::startApp(args);
#elif defined(_WIN32)
    std::wstring line = quoteWindows(program.wstring());
    for (const auto& a : args) line += L" " + quoteWindows(path(a).wstring());
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION process{};
    const std::wstring dir = program.parent_path().wstring();
    if (!CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    std::vector<std::string> all{utf8(program)};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    const std::string dir = utf8(program.parent_path());
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        if (chdir(dir.c_str()) != 0) {
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#endif
}

// ---------------------------------------------------------------- drawing helpers
namespace draw {
inline ImU32 rgba(uint32_t rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, int(std::clamp(a, 0.0f, 1.0f) * 255));
}
// Text in the launcher's own typeface with a dark outline (for the languages the menu's comic font has no letters for).
inline ImVec2 outlined(ImDrawList* dl, ImFont* f, float size, ImVec2 p, uint32_t fill, const char* text, float alpha = 1.0f, float outline = -1,
                       uint32_t line = 0x000000) {
    if (outline < 0) outline = std::max(1.5f, size * 0.09f);
    p = {std::round(p.x), std::round(p.y)};
    for (int i = 0; i < 12; ++i) {
        const float a = float(i) * 6.2831853f / 12.0f;
        dl->AddText(f, size, {p.x + std::cos(a) * outline, p.y + std::sin(a) * outline}, rgba(line, alpha), text);
    }
    dl->AddText(f, size, p, rgba(fill, alpha), text);
    return f->CalcTextSizeA(size, FLT_MAX, 0, text);
}
}  // namespace draw

// ---------------------------------------------------------------- the menu
class Menu {
public:
    // The pages, left to right, one page width apart as the PS3 menu's cameras are.
    enum class Page { Extras, Title, Sly1, Sly2, Sly3, Sly4, Hackpack, Developers };
    enum class State { Playable, InDevelopment, NotInstalled, Unavailable, Page };
    // Screenshots: the games shown as installed and the profile whose imported menu is shown.
    struct Fixture {
        bool active = false;
        std::array<bool, 3> installed{};  // Sly 1, Sly 2, Sly 3 (Sly 3: installed, runtime present)
        fs::path profile;                 // holds Collection/Menu (empty: the backend's)
        std::string screen;               // "", "options", "extras", "credits", "launch", "developers", "confirm", "glide"
        bool scripted = false;            // presses are replayed a few frames apart: no input delay between pages
        // the developers page's pictures (the launcher's own, launcher/assets/developers): each with its silhouette
        struct DeveloperPicture {
            ImTextureID picture = nullptr, silhouette = nullptr;
        };
        std::array<DeveloperPicture, 5> developers{};
    };
    // The profile whose imported menu a start would show.
    static fs::path profileOf(const LauncherBackend& backend, const Fixture& fixture) {
        return fixture.active && !fixture.profile.empty() ? fixture.profile : backend.locations.user;
    }

    Menu(LauncherBackend& backend, GLFWwindow* window, Fixture fixture) : m_backend(backend), m_window(window), m_fixture(std::move(fixture)) {
        m_profile = profileOf(m_backend, m_fixture);
        const json menu = ps3menu::readMenu(m_profile);
        if (menu.is_null()) throw std::runtime_error(tr("The Sly Collection's menu is not imported."));
        const fs::path dir = ps3menu::menuDir(m_profile);
        m_scene.load(dir, menu);
        m_font.load(dir, menu);
        auto pieces = [&](const char* key, Tex* out, size_t count) {
            if (!menu.contains("gui") || !menu.at("gui").contains(key)) return;
            const auto& list = menu.at("gui").at(key);
            for (size_t i = 0; i < count && i < list.size(); ++i) {
                const auto bytes = readBytes(dir / path(list.at(i).get<std::string>()));
                out[i] = Tex(bytes.empty() ? Image{} : decodeImage(bytes.data(), bytes.size()));
            }
        };
        pieces("banner", m_banner, 3), pieces("frame", m_frame, 9), pieces("border", m_border, 9), pieces("loading", &m_loading, 1);
        m_glide = std::clamp(menu.value("tuning", json::object()).value("cameraGlide", 0.1f), 0.01f, 1.0f);
        m_delay = m_fixture.scripted ? 0.0f : std::clamp(menu.value("tuning", json::object()).value("inputDelay", 0.5f), 0.0f, 2.0f);
        m_audioFiles = menu.value("audio", json::object());
        // The pages: the title and the three games at the scene's own cameras (the import checks them), the others one
        // page width further on each side (where the PS3 menu has its credits, minigame and player pages).
        const float title = m_scene.cameraX("Camera_Title"), first = m_scene.cameraX("Camera_Sly1"), last = m_scene.cameraX("Camera_Sly3");
        const float width = m_scene.cameraX("Camera_Sly2") - first;
        m_pages = {{Page::Extras, title - width},   {Page::Title, title},          {Page::Sly1, first},
                   {Page::Sly2, first + width},     {Page::Sly3, last},            {Page::Sly4, last + width},
                   {Page::Hackpack, last + 2 * width}, {Page::Developers, last + 3 * width}};
        // What the scene has no art for: the launcher's own logos (SafeHouse on the title, the two games in
        // development), placed and shadowed as the scene's logos are; and arrows for the pages after Sly 3 (copies of
        // the title's).
        float logoY = 0.15f, logoZ = 0.3f;
        if (auto* n = m_scene.node("STATIC_Sly1Logo_US")) logoY = n->pos[1], logoZ = n->pos[2];
        auto picture = [&](const char* name, ImTextureID image, float aspect, float x, float boxW, float boxH) {
            if (!image) return;
            const float h = std::min(boxH, boxW / std::max(0.1f, aspect));
            m_scene.addImage(name, image, x, logoY, logoZ, h * aspect, h);
        };
        picture("SAFEHOUSE_Title", ui::images().logo, ui::images().logoAspect, title, 1.3f, 0.95f);
        picture("SAFEHOUSE_Sly4", ui::images().games[int(GameId::Sly4)].texture, ui::images().games[int(GameId::Sly4)].aspect, last + width, 1.3f, 0.8f);
        picture("SAFEHOUSE_Hackpack", ui::images().games[int(GameId::Hackpack)].texture, ui::images().games[int(GameId::Hackpack)].aspect, last + 2 * width, 1.3f, 0.8f);
        std::string leftArrow, rightArrow;
        for (auto& n : m_scene.nodes()) {
            if (n.parent >= 0 || !starts(n.name, "STATIC_ScreenArrow") || std::fabs(n.pos[0] - title) > width * 0.4f) continue;
            (n.pos[0] < title ? leftArrow : rightArrow) = n.name;
        }
        if (auto* l = m_scene.node(leftArrow), *r = m_scene.node(rightArrow); l && r) {
            const float lx = l->pos[0] - title, rx = r->pos[0] - title;
            for (int i = 1; i <= 3; ++i) {
                m_scene.clone(leftArrow, "SAFEHOUSE_ArrowLeft" + std::to_string(i), last + float(i) * width + lx);
                if (i < 3) m_scene.clone(rightArrow, "SAFEHOUSE_ArrowRight" + std::to_string(i), last + float(i) * width + rx);
            }
        }
        m_scene.ride("STATIC_SlyVan"), m_scene.ride("STATIC_Sly");
        refreshStatus(true);
        const Page saved = m_fixture.active || m_fixture.scripted ? Page::Title : Page(std::clamp(m_backend.config.value("collection_page", int(Page::Title)), 0, int(Page::Developers)));
        m_page = indexOf(saved);
        if (m_fixture.screen == "options") openOptions();
        else if (m_fixture.screen == "extras") m_page = indexOf(Page::Extras);
        else if (m_fixture.screen == "developers") m_page = indexOf(Page::Developers);
        else if (m_fixture.screen.rfind("developer", 0) == 0 && m_fixture.screen.size() == 10) {  // "developer1" .. "developer5": that one chosen
            m_page = indexOf(Page::Developers);
            m_developer = std::clamp(m_fixture.screen[9] - '1', 0, 4);
        }
        else if (m_fixture.screen == "confirm") m_page = indexOf(Page::Sly3), m_confirm = true;
        else if (m_fixture.screen.rfind("credits", 0) == 0) {  // "credits" or "credits:<section id>" (launcher/credits.h)
            m_page = indexOf(Page::Extras), m_credits = true;
            if (const auto colon = m_fixture.screen.find(':'); colon != std::string::npos) m_creditsSection = m_fixture.screen.substr(colon + 1);
        }
        else if (m_fixture.screen == "launch") m_launchFade = 1.0f, m_launching = int(GameId::Sly2);
        m_cameraX = m_pages[size_t(m_page)].x;
        if (m_fixture.screen == "glide") {  // (a shot halfway between the title and Sly 1)
            m_page = indexOf(Page::Sly1);
            m_cameraX = (m_pages[size_t(indexOf(Page::Title))].x + m_pages[size_t(m_page)].x) * 0.5f;
            m_frozen = true;
        }
        const auto& c = collectionConfig();
        m_musicVolume = std::clamp(c.value("music", 0.7f), 0.0f, 1.0f);
        m_soundVolume = std::clamp(c.value("sounds", 0.8f), 0.0f, 1.0f);
        style();
        m_scene.preload();
        if (!m_fixture.active && !std::getenv("SAFEHOUSE_COLLECTION_SILENT") && m_audio.open(outputDevice())) startSoundLoader();
        m_audio.setVolumes(m_musicVolume, m_soundVolume);
        m_startFade = m_fixture.active || m_fixture.scripted ? 0.0f : 1.0f;
    }
    ~Menu() {
        m_job.cancel = true;
        if (m_loader.joinable()) m_loader.join();
    }
    bool gameRunning() const { return m_job.busy && m_playing >= 0; }
    // A game is starting, running or the menu is fading back in.
    bool busy() const {
        if (m_fixture.screen == "launch") return false;
        const bool gliding = !m_frozen && !m_dragCamera && std::fabs(m_pages[size_t(m_page)].x - m_cameraX) > 0.002f;
        return m_launching >= 0 || m_job.busy || m_returnFade > 0 || gliding;
    }
    bool quitRequested() const { return m_quit; }
    // A frame. size: the window in pixels; pad: the edges from GamepadNav; family: the prompt family.
    void frame(ImVec2 size, const launcher::PadEdges& pad, input::Family family, float dt) {
        m_time += dt;
        m_dt = dt;
        m_job.reap();
        pollJob();
        refreshStatus(false);
        pollSounds();
        if (m_sounds && m_audio.available() && m_audio.music() != m_sounds->music) m_audio.playMusic(m_sounds->music, 0.6f);
        // the overlay's scale: the PS3 menu's 1280x720 layout, following the scene (a window narrower than 4:3 shows
        // the page smaller, see MenuScene::view)
        m_s = std::min(size.y / 720.0f, size.x / 960.0f);
        m_size = size;
        input(pad);
        // the camera glides to the page (the PS3 menu: position += (target - position) * 0.1, sixty times a second)
        const float target = m_pages[size_t(m_page)].x;
        if (!m_frozen && !m_dragCamera) {
            m_cameraX += (target - m_cameraX) * (1.0f - std::pow(1.0f - m_glide, dt * 60.0f));
            if (std::fabs(target - m_cameraX) < 0.0005f) m_cameraX = target;
        }
        m_inputWait = std::max(0.0f, m_inputWait - dt);
        style();
        m_view = m_scene.view(size, m_cameraX);
        if (m_launching < 0 && !gameRunning()) m_scene.update(dt, m_cameraX);
        auto* dl = ImGui::GetBackgroundDrawList();
        dl->AddRectFilled({0, 0}, size, IM_COL32(0, 0, 0, 255));
        m_bloom.begin(dl, m_scene.bloom(), m_fixture.active);
        m_scene.draw(dl, m_view);
        m_bloom.end(dl);
        auto* fg = ImGui::GetForegroundDrawList();
        drawPage(fg);
        if (m_options) drawOptions(fg);
        if (m_credits) drawCredits(fg);
        drawConfirm(fg);
        drawBack(fg);
        drawFooter(fg, family);
        drawToast(fg, dt);
        drawFades(fg, dt);
    }
    // The window comes back after a game: the scene fades in.
    void returned() {
        m_returnFade = 1.0f;
        m_audio.setMuted(false);
    }

private:
    // ---------------------------------------------------------------- state
    struct Status {
        bool assets = false, runtime = false, outdated = false;
    };
    struct PageInfo {
        Page page;
        float x;
    };
    int indexOf(Page p) const {
        for (size_t i = 0; i < m_pages.size(); ++i)
            if (m_pages[i].page == p) return int(i);
        for (size_t i = 0; i < m_pages.size(); ++i)
            if (m_pages[i].page == Page::Title) return int(i);
        return 0;
    }
    Page page() const { return m_pages[size_t(m_page)].page; }
    static bool isGame(Page p) { return p >= Page::Sly1 && p <= Page::Hackpack; }
    static GameId gameOf(Page p) { return GameId(int(p) - int(Page::Sly1)); }
    State state(Page p) const {
        if (!isGame(p)) return State::Page;
        if (!installable(gameOf(p))) return State::Unavailable;  // (Sly 4, the Hackpack: in development, nothing to install)
        const Status& st = m_status[size_t(gameOf(p))];
        if (!st.assets) return State::NotInstalled;
        if (gameOf(p) == GameId::Sly3 || !st.runtime) return State::InDevelopment;
        return State::Playable;
    }
    void refreshStatus(bool now) {
        if (!now && m_time < m_nextStatus) return;
        m_nextStatus = m_time + 2.0;
        for (int g = 0; g < 3; ++g) {
            Status st;
            if (m_fixture.active) st.assets = st.runtime = m_fixture.installed[size_t(g)];
            else try {
                    st.assets = m_backend.assetsInstalled(GameId(g));
                    st.runtime = m_backend.runtimeAvailable(GameId(g));
                    st.outdated = m_backend.needsReextract(GameId(g));
                } catch (...) {
                }
            m_status[size_t(g)] = st;
        }
    }
    json& collectionConfig() {
        if (!m_backend.config.contains("collection") || !m_backend.config.at("collection").is_object()) m_backend.config["collection"] = json::object();
        return m_backend.config["collection"];
    }
    void saveConfig() {
        if (m_fixture.active) return;
        try {
            m_backend.saveConfig();
        } catch (const std::exception& e) {
            toast(e.what());
        }
    }
    // The output device chosen for the games (Sly 2's Settings.ini, Settings > Audio), by name; empty: the system's.
    std::string outputDevice() {
        if (m_fixture.active) return {};
        try {
            Sly2Settings s;
            if (loadSly2(m_backend.userDir(GameId::Sly2), s) != Sly2Load::Unreadable) return s.game.outputDevice;
        } catch (...) {
        }
        return {};
    }

    // ---------------------------------------------------------------- the scene's nodes: what shows, how
    // The logo variant of a game: the region of its installation (the collection's: Japanese with Japanese text, the
    // PAL "Sly Trilogy" when the installed games are PAL ones).
    std::string regionSuffix(int game) {
        // (a menu without that region's logo: the American one)
        static const char* bases[3] = {"STATIC_Sly1Logo", "STATIC_Sly2Logo", "STATIC_Sly3Logo"};
        std::string wanted = "_US";
        if (!m_fixture.active) try {
                const std::string r = m_backend.selectedRegion(GameId(game));
                wanted = r == "europe" ? "_EU" : r == "japan" ? "_JP" : r == "korea" ? "_KR" : "_US";
            } catch (...) {
            }
        return m_scene.node(std::string(bases[game]) + wanted) ? wanted : "_US";
    }
    static bool ends(const std::string& s, std::string_view tail) { return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0; }
    static bool starts(const std::string& s, std::string_view head) { return s.compare(0, head.size(), head) == 0; }
    // The developers page: who, and what they did (launcher/credits.h, the same list as the About page's).
    using Developer = credits::Developer;
    static constexpr const auto& kDevelopers = credits::kDevelopers;
    void style() {
        const bool paris = page() > Page::Sly3;  // (the pages over the scene's second half)
        for (auto& n : m_scene.nodes()) {
            if (n.parent >= 0) continue;
            const std::string& name = n.name;
            n.show = true, n.alpha = 1, n.shade = 1;
            if (starts(name, "STATIC_SlyCollectionLogo") || starts(name, "STATIC_MCGameLogo") || starts(name, "STATIC_Minigame") || starts(name, "STATIC_Player") ||
                starts(name, "STATIC_MoveArrow") || name.find("Locked") != std::string::npos) {
                // the PS3 menu's own title logo (SafeHouse's is shown) and its minigame and player pages
                n.show = false;
            } else if (ends(name, "_US") || ends(name, "_EU") || ends(name, "_JP") || ends(name, "_KR")) {
                const std::string base = name.substr(0, name.size() - 3);
                const int game = base == "STATIC_Sly1Logo" ? 0 : base == "STATIC_Sly2Logo" ? 1 : base == "STATIC_Sly3Logo" ? 2 : -1;
                n.show = game >= 0 && ends(name, regionSuffix(game));
                // a game that is not installed: its logo in the dark
                if (game >= 0 && state(Page(int(Page::Sly1) + game)) == State::NotInstalled) n.shade = 0.28f;
            } else if (name == "SAFEHOUSE_Sly4" || name == "SAFEHOUSE_Hackpack") {
                n.shade = 0.28f;
            } else if (name == "STATIC_SlyVan") {
                n.show = !paris;  // (the PS3 menu: the van on the night drive's pages, Sly on the others)
            } else if (name == "STATIC_Sly") {
                n.show = paris;
                n.pos[1] = -0.60458f;  // (his place under the camera on the PS3 menu)
            }
        }
    }

    // ---------------------------------------------------------------- sounds
    struct Sounds {
        PcmPtr music, move, select, cancel;
    };
    void startSoundLoader() {
        if (m_loader.joinable()) m_loader.join();
        m_soundsReady = false;
        const fs::path dir = ps3menu::menuDir(m_profile);
        const json files = m_audioFiles;
        m_loader = std::thread([this, dir, files] {
            auto sounds = std::make_shared<Sounds>();
            auto load = [&](const std::string& rel, bool loops) -> PcmPtr {
                if (rel.empty()) return nullptr;
                Pcm p = decodeWav(readBytes(dir / path(rel)));
                if (p.samples.empty()) return nullptr;
                p.loops = loops, p.loopStart = 0, p.loopEnd = p.frames();
                return std::make_shared<const Pcm>(std::move(p));
            };
            try {
                if (files.contains("music") && !files.at("music").empty()) sounds->music = load(files.at("music").at(0).get<std::string>(), true);
                sounds->move = load(files.value("move", std::string{}), false);
                sounds->select = load(files.value("select", std::string{}), false);
                sounds->cancel = load(files.value("cancel", std::string{}), false);
            } catch (...) {
            }
            std::lock_guard lock(m_soundMutex);
            m_pendingSounds = sounds;
            m_soundsReady = true;
        });
    }
    void pollSounds() {
        if (!m_soundsReady) return;
        std::lock_guard lock(m_soundMutex);
        if (m_pendingSounds) m_sounds = std::move(m_pendingSounds);
    }
    enum class Sfx { Move, Select, Cancel };
    void sound(Sfx s) {
        if (m_sounds) m_audio.playSound(s == Sfx::Move ? m_sounds->move : s == Sfx::Select ? m_sounds->select : m_sounds->cancel);
    }

    // ---------------------------------------------------------------- input
    static bool pressed(ImGuiKey a, ImGuiKey b = ImGuiKey_None, ImGuiKey c = ImGuiKey_None) {
        return ImGui::IsKeyPressed(a, true) || (b != ImGuiKey_None && ImGui::IsKeyPressed(b, true)) || (c != ImGuiKey_None && ImGui::IsKeyPressed(c, true));
    }
    // Left / Right: the next page at once; the camera glides there. The page takes no input for a moment (the PS3
    // menu: Game.UI.FrontEndInputDelayTime).
    void turn(int by) {
        if (m_inputWait > 0) return;
        show(m_page + by);
    }
    // Makes a page the current one (the camera glides there from wherever it is).
    void show(int next) {
        if (next < 0 || next >= int(m_pages.size()) || next == m_page) return;
        m_page = next;
        m_inputWait = m_delay;
        m_extra = 0;
        // (with keys or a pad the first developer is chosen at once; a pointer chooses by hovering or tapping)
        m_developer = page() == Page::Developers && !m_touch && !input::tracker().lastWasMouse() ? 0 : -1;
        sound(Sfx::Move);
        m_backend.config["collection_page"] = int(page());
    }
    void input(const launcher::PadEdges& pad) {
        if (m_launching >= 0 || gameRunning() || m_returnFade > 0.5f || m_startFade > 0.5f || ImGuiFileDialog::Instance()->IsOpened()) return;
        const bool left = pressed(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_A) || pressed(ImGuiKey_GamepadLStickLeft);
        const bool right = pressed(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_D) || pressed(ImGuiKey_GamepadLStickRight);
        const bool up = pressed(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_W) || pressed(ImGuiKey_GamepadLStickUp);
        const bool down = pressed(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_S) || pressed(ImGuiKey_GamepadLStickDown);
        const bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
        const bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        const bool options = pad.start || pad.y || ImGui::IsKeyPressed(ImGuiKey_Tab, false) || ImGui::IsKeyPressed(ImGuiKey_O, false);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (click) m_touch = ImGui::GetIO().MouseSource == ImGuiMouseSource_TouchScreen;
        if (left || right || up || down || confirm || back || options) m_touch = false;  // (keys or a pad again)
        // the panels' Back button (shown for a finger or the mouse)
        const bool backTapped = (m_options || m_credits) && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !m_dragged && m_hitBack.Contains(mouse) &&
                                m_hitBack.Contains(ImGui::GetIO().MouseClickedPos[0]);
        if (m_credits) {
            if (back || confirm || backTapped) m_credits = false, sound(Sfx::Cancel);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) m_dragged = false;
            m_creditsScroll += ImGui::GetIO().MouseWheel * -40.0f * m_s + (down ? 30.0f : up ? -30.0f : 0.0f) * m_s;
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) m_creditsScroll -= ImGui::GetIO().MouseDelta.y, m_dragged = true;
            return;
        }
        if (m_options) {
            optionsInput(up, down, left, right, confirm, back || options || backTapped);
            return;
        }
        if (m_confirm) {
            confirmInput(up, down, left, right, confirm, back);
            return;
        }
        if (options) {
            openOptions();
            return;
        }
        // A finger (or the mouse held down) drags the scene: the camera follows it, and on release settles on the
        // nearest page, or one page on in the direction of a flick. A press released where it began is a tap.
        if (click) {
            m_dragFrom = mouse, m_dragging = true, m_dragCamera = false;
            m_dragCameraFrom = m_cameraX, m_dragPage = m_page;
            m_trail.clear();
        }
        bool tap = false;
        if (m_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float dx = mouse.x - m_dragFrom.x, dy = mouse.y - m_dragFrom.y;
            if (!m_dragCamera && std::fabs(dx) > 14 * m_s && std::fabs(dx) > std::fabs(dy)) m_dragCamera = true;
            if (m_dragCamera) {
                // (the logos' depth: they stay under the finger)
                const float edge = 1.2f;
                m_cameraX = std::clamp(m_dragCameraFrom - dx / m_view.scale(0.3f), m_pages.front().x - edge, m_pages.back().x + edge);
            }
            m_trail.push_back({m_time, mouse.x});
            while (m_trail.size() > 2 && m_time - m_trail.front().first > 0.12) m_trail.pop_front();
        }
        if (m_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_dragging = false;
            if (m_dragCamera) {
                m_dragCamera = false;
                float speed = 0;  // pixels per second over the last tenth of a second
                if (m_trail.size() >= 2 && m_trail.back().first > m_trail.front().first)
                    speed = float((m_trail.back().second - m_trail.front().second) / (m_trail.back().first - m_trail.front().first));
                int target = 0;
                for (int i = 1; i < int(m_pages.size()); ++i)
                    if (std::fabs(m_pages[size_t(i)].x - m_cameraX) < std::fabs(m_pages[size_t(target)].x - m_cameraX)) target = i;
                if (std::fabs(speed) > 500 * m_s) target = std::clamp(m_dragPage + (speed < 0 ? 1 : -1), 0, int(m_pages.size()) - 1);
                show(target);
            } else {
                tap = true;
            }
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.5f) turn(-1);
        if (wheel < -0.5f) turn(1);
        // the arrows beside the logo
        if (tap) {
            if (m_hitLeft.Contains(mouse)) {
                turn(-1);
                return;
            }
            if (m_hitRight.Contains(mouse)) {
                turn(1);
                return;
            }
        }
        if (page() == Page::Extras) {
            if (up && m_extra > 0) --m_extra, sound(Sfx::Move);
            if (down && m_extra < 3) ++m_extra, sound(Sfx::Move);
            if (right) turn(1);
            for (int i = 0; i < 4; ++i)
                if (m_hitButton[size_t(i)].Contains(mouse)) {
                    if (input::tracker().lastWasMouse() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_extra = i;
                    if (tap) m_extra = i, activateExtra(i);
                }
            if (confirm) activateExtra(m_extra);
            if (back) turn(1);
        } else if (page() == Page::Developers) {
            // the five circles: the first at the left, the others two by two
            static constexpr int toLeft[5] = {-2, 0, 1, 0, 3}, toRight[5] = {1, 2, -1, 4, -1}, toUp[5] = {-1, -1, -1, 1, 2}, toDown[5] = {-1, 3, 4, -1, -1};
            int next = m_developer;
            if (m_developer < 0) {
                if (left) turn(-1);
                else if (right || up || down || confirm) next = 0;
            } else if (left) {
                if (toLeft[m_developer] == -2) turn(-1);
                else next = toLeft[m_developer];
            } else if (right && toRight[m_developer] >= 0) next = toRight[m_developer];
            else if (up && toUp[m_developer] >= 0) next = toUp[m_developer];
            else if (down && toDown[m_developer] >= 0) next = toDown[m_developer];
            if (next != m_developer && page() == Page::Developers) m_developer = next, sound(Sfx::Move);
            if (back) turn(-1);
            // the mouse chooses by being over a circle; a tap chooses a circle, a tap elsewhere none
            int over = -1;
            for (int i = 0; i < 5; ++i) {
                const ImVec2 c = m_hitDeveloper[size_t(i)].GetCenter();
                const float r = m_hitDeveloper[size_t(i)].GetWidth() * 0.5f;
                if (r > 0 && (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) <= r * r) over = i;
            }
            if (tap && !m_hitLeft.Contains(mouse)) {
                if (over != m_developer && over >= 0) sound(Sfx::Move);
                m_developer = over;
            } else if (!m_touch && input::tracker().lastWasMouse() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && over != m_developer) {
                if (over >= 0) sound(Sfx::Move);
                m_developer = over;
            }
        } else {
            if (left) turn(-1);
            if (right) turn(1);
            if (confirm || (tap && m_hitLogo.Contains(mouse) && m_inputWait <= 0)) activate();
            if (back) {
                if (page() != Page::Title) {
                    if (m_inputWait <= 0) m_page = indexOf(Page::Title), m_inputWait = m_delay, sound(Sfx::Cancel);
                } else {
                    m_quit = true;
                }
            }
        }
    }
    void activate() {
        const Page p = page();
        if (p == Page::Title) {
            turn(1);
            return;
        }
        const GameId g = gameOf(p);
        switch (state(p)) {
        case State::Unavailable:
            sound(Sfx::Cancel);
            toast(trf("A native port of %s is in development. There is nothing to install yet.", game(g).title));
            return;
        case State::Playable:
        case State::InDevelopment:
            if (!m_status[size_t(g)].runtime) {
                sound(Sfx::Cancel);
                toast(trf("%s is still in development: this build cannot play it yet.", game(g).shortName));
                return;
            }
            if (m_status[size_t(g)].outdated) {
                sound(Sfx::Cancel);
                toast(tr("An update changed the game files' format: open the SafeHouse launcher and re-extract your disc."));
                return;
            }
            sound(Sfx::Select);
            // Sly 3: the SafeHouse launcher's own question first (until "Don't show again")
            if (g == GameId::Sly3 && !m_sly3Confirmed && !(m_fixture.active ? false : m_backend.sly3NoticeHidden())) {
                m_confirm = true, m_confirmChoice = 2, m_confirmHide = false;
                return;
            }
            m_launching = int(g);
            m_launchFade = 0;
            break;
        default:
            sound(Sfx::Cancel);
            toast(trf("Install %s in the SafeHouse launcher to unlock it (the page left of the title).", game(g).shortName));
            break;
        }
    }
    void activateExtra(int i) {
        sound(Sfx::Select);
        if (i == 0) openOptions();
        else if (i == 1) m_credits = true, m_creditsScroll = 0;
        else if (i == 2) openSafeHouse();
        else m_quit = true;
    }
    void openSafeHouse() {
        if (m_fixture.active) return;
        setEnv(kLauncherEnv, nullptr);  // (the SafeHouse launcher's games quit to the desktop)
        const bool ok = spawnDetached(steam::launcherProgram(), {});
        setEnv(kLauncherEnv, "collection");
        if (ok) m_quit = true;
        else toast(tr("Could not start the SafeHouse launcher."));
    }
    // Starting the chosen game once the screen is black.
    void launchNow() {
        const GameId g = GameId(m_launching);
        m_playing = int(g);
        m_audio.setMuted(true);
        saveConfig();
        if (m_fixture.active) return;
        m_job.start(std::string("Playing ") + game(g).shortName, [this, g](const auto& log) { m_backend.play(g, m_job.cancel, log); });
    }
    void pollJob() {
        const bool busy = m_job.busy.load();
        if (m_wasBusy && !busy) {
            std::string message;
            {
                std::lock_guard lock(m_job.mutex);
                message = m_job.message;
            }
            if (message != "Completed" && message != "Cancelled") toast(message);
            m_playing = -1;
            m_launching = -1;
            m_launchFade = 0;
        }
        m_wasBusy = busy;
    }

    // ---------------------------------------------------------------- text and the GUI pieces
    // A caption in the menu's comic font (white letters with their black outline, tinted), or in the launcher's own
    // typeface when the font has no letters for the language. size: a capital's height. Returns the size drawn.
    ImVec2 caption(ImDrawList* dl, float size, ImVec2 at, uint32_t colour, const char* text, float alpha = 1.0f, bool measureOnly = false) const {
        if (m_font.covers(text)) {
            if (!measureOnly) m_font.draw(dl, size, at, draw::rgba(colour, alpha), text);
            return m_font.measure(size, text);
        }
        ImFont* f = ui::fonts().display;
        const float px = size * 1.12f;
        if (!measureOnly) draw::outlined(dl, f, px, {at.x, at.y + size * 0.1f}, colour, text, alpha);
        const ImVec2 m = f->CalcTextSizeA(px, FLT_MAX, 0, text);
        return {m.x, std::max(m.y, size * 1.3f)};
    }
    ImVec2 captionSize(float size, const char* text) const { return caption(nullptr, size, {}, 0, text, 1, true); }
    void captionCentred(ImDrawList* dl, float size, ImVec2 centre, uint32_t colour, const char* text, float alpha = 1.0f) const {
        const ImVec2 m = captionSize(size, text);
        caption(dl, size, {std::round(centre.x - m.x * 0.5f), std::round(centre.y - m.y * 0.5f)}, colour, text, alpha);
    }
    // Words wrapped to a width, centred line by line around a point.
    void captionWrapped(ImDrawList* dl, float size, ImVec2 centre, float width, uint32_t colour, const std::string& text, float alpha = 1.0f) const {
        std::vector<std::string> lines;
        std::string line, word;
        auto flush = [&] {
            if (word.empty()) return;
            // a word wider than the frame (a language written without spaces): broken between characters
            while (captionSize(size, word.c_str()).x > width) {
                size_t cut = 0;
                for (size_t next = 0; next < word.size();) {
                    size_t step = 1;
                    while (next + step < word.size() && (uint8_t(word[next + step]) & 0xC0) == 0x80) ++step;
                    const std::string head = (line.empty() ? std::string() : line + " ") + word.substr(0, next + step);
                    if (cut && captionSize(size, head.c_str()).x > width) break;
                    next += step, cut = next;
                    if (line.empty() == false && captionSize(size, head.c_str()).x > width) {
                        cut = 0;
                        break;
                    }
                }
                if (cut == 0) {  // (nothing more fits on this line)
                    lines.push_back(line), line.clear();
                    continue;
                }
                if (cut >= word.size()) break;
                lines.push_back((line.empty() ? std::string() : line + " ") + word.substr(0, cut));
                line.clear();
                word.erase(0, cut);
            }
            const std::string tried = line.empty() ? word : line + " " + word;
            if (!line.empty() && captionSize(size, tried.c_str()).x > width) lines.push_back(line), line = word;
            else line = tried;
            word.clear();
        };
        for (char c : text) {
            if (c == ' ' || c == '\n') flush();
            else word.push_back(c);
            if (c == '\n' && !line.empty()) lines.push_back(line), line.clear();
        }
        flush();
        if (!line.empty()) lines.push_back(line);
        const float pitch = size * 1.3f;
        float y = centre.y - pitch * float(lines.size()) * 0.5f;
        for (const auto& l : lines) {
            const ImVec2 m = captionSize(size, l.c_str());
            caption(dl, size, {std::round(centre.x - m.x * 0.5f), std::round(y)}, colour, l.c_str(), alpha);
            y += pitch;
        }
    }
    // A nine-piece frame of the menu (pieces: left-top, top, right-top, left, centre, right, left-bottom, bottom,
    // right-bottom), its corners at the pieces' own size.
    void ninePiece(ImDrawList* dl, const Tex* pieces, const ImRect& r, float alpha, float corner) const {
        if (!pieces[0] || !pieces[4]) {
            dl->AddRectFilled(r.Min, r.Max, draw::rgba(0x0C2A66, 0.9f * alpha), 8 * m_s);
            return;
        }
        const float c = std::min({corner, r.GetWidth() * 0.5f, r.GetHeight() * 0.5f});
        const float xs[4] = {r.Min.x, r.Min.x + c, r.Max.x - c, r.Max.x}, ys[4] = {r.Min.y, r.Min.y + c, r.Max.y - c, r.Max.y};
        const ImU32 tint = draw::rgba(0xFFFFFF, alpha);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                if (pieces[y * 3 + x] && xs[x + 1] > xs[x] && ys[y + 1] > ys[y])
                    dl->AddImage(pieces[y * 3 + x].im(), {xs[x], ys[y]}, {xs[x + 1], ys[y + 1]}, {0, 0}, {1, 1}, tint);
    }
    // The note frame (the PS3 menu's "NI" border) and the button frame (its plain border).
    void panel(ImDrawList* dl, const ImRect& r, float alpha) const { ninePiece(dl, m_frame, r, alpha, float(std::max(8, m_frame[0].w)) * 0.5f * m_s); }
    void button(ImDrawList* dl, const ImRect& r, float alpha) const { ninePiece(dl, m_border, r, alpha, float(std::max(8, m_border[0].w)) * m_s); }
    // The banner (three pieces: the ends at their own shape, the middle stretched), with its title.
    void banner(ImDrawList* dl, ImVec2 centre, float width, const char* text, float alpha) const {
        const float h = 128 * m_s * 0.8f, cap = h;
        const float size = 36 * m_s;
        const ImVec2 t = captionSize(size, text);
        const float w = std::max(width, t.x + cap * 1.5f);
        const ImRect r({centre.x - w * 0.5f, centre.y - h * 0.5f}, {centre.x + w * 0.5f, centre.y + h * 0.5f});
        if (m_banner[0] && m_banner[1] && m_banner[2]) {
            const ImU32 tint = draw::rgba(0xFFFFFF, alpha);
            dl->AddImage(m_banner[0].im(), r.Min, {r.Min.x + cap, r.Max.y}, {0, 0}, {1, 1}, tint);
            dl->AddImage(m_banner[1].im(), {r.Min.x + cap, r.Min.y}, {r.Max.x - cap, r.Max.y}, {0, 0}, {1, 1}, tint);
            dl->AddImage(m_banner[2].im(), {r.Max.x - cap, r.Min.y}, r.Max, {0, 0}, {1, 1}, tint);
        } else {
            panel(dl, ImRect({r.Min.x, r.Min.y + 14 * m_s}, {r.Max.x, r.Max.y - 14 * m_s}), alpha);
        }
        captionCentred(dl, size, {centre.x, centre.y - 2 * m_s}, 0xFFFFFF, text, alpha);
    }

    // ---------------------------------------------------------------- what lies over the scene on each page
    // The window's place of a point of the PS3 menu's 1280x720 overlay, given from the page's centre.
    ImVec2 overlay(Page p, float x, float y) const {
        const float centre = m_view.project(m_pages[size_t(indexOf(p))].x, 0, 0).x;
        return {centre + x * m_s, m_size.y * 0.5f + y * m_s};
    }
    void drawPage(ImDrawList* dl) {
        m_hitLeft = m_hitRight = m_hitLogo = {};
        for (auto& r : m_hitButton) r = {};
        for (auto& r : m_hitDeveloper) r = {};
        const bool settled = !m_options && !m_credits && !m_confirm;
        // the arrows of the page shown: the nearest swaying arrow on each side of the window's centre
        for (auto& n : m_scene.nodes()) {
            if (n.parent >= 0 || !n.show || !(starts(n.name, "STATIC_ScreenArrow") || starts(n.name, "SAFEHOUSE_Arrow"))) continue;
            ImRect r = m_scene.rect(m_view, n.name);
            if (r.Max.x < 0 || r.Min.x > m_size.x) continue;
            r.Expand(std::max(18 * m_s, (110 * m_s - r.GetWidth()) * 0.5f));  // (a finger's width at least)
            if (r.GetCenter().x < m_size.x * 0.5f) m_hitLeft = r;
            else m_hitRight = r;
        }
        if (m_page == 0) m_hitLeft = {};
        if (m_page + 1 == int(m_pages.size())) m_hitRight = {};
        // every page's own pieces, so they slide with the scene while the camera glides
        for (const auto& info : m_pages) {
            const float centre = m_view.project(info.x, 0, 0).x;
            if (centre < -m_size.x || centre > m_size.x * 2) continue;
            const bool current = info.page == page();
            if (isGame(info.page)) drawGameNotes(dl, info.page, current && settled);
            else if (info.page == Page::Extras) drawExtras(dl, current && settled);
            else if (info.page == Page::Developers) drawDevelopers(dl, m_dt);
        }
        const Page p = page();
        if (p == Page::Title) m_hitLogo = m_scene.rect(m_view, "SAFEHOUSE_Title");
        else if (p == Page::Sly4) m_hitLogo = m_scene.rect(m_view, "SAFEHOUSE_Sly4");
        else if (p == Page::Hackpack) m_hitLogo = m_scene.rect(m_view, "SAFEHOUSE_Hackpack");
        else if (isGame(p)) m_hitLogo = m_scene.rect(m_view, std::string("STATIC_Sly") + char('1' + int(gameOf(p))) + "Logo" + regionSuffix(int(gameOf(p))));
    }
    // Beside a game's logo: the padlock badge and a note when it is not installed or cannot be installed yet (Sly 4,
    // the Hackpack), and on Sly 3's page the SafeHouse launcher's own warning that it is not fully playable yet.
    void drawGameNotes(ImDrawList* dl, Page p, bool) {
        const State st = state(p);
        if (st == State::Playable) return;
        const GameId g = gameOf(p);
        std::string note;
        float width = 300;
        if (st == State::NotInstalled || st == State::Unavailable) {
            note = st == State::Unavailable ? trf("%s: in development, nothing to install yet", tr(game(g).title)) : trf("Install %s in the SafeHouse launcher", game(g).shortName);
            if (st == State::Unavailable) width = 380;
            // the PS3 menu's own padlock (the minigame circles' badge), on the logo's lower right
            if (auto* lock = m_scene.node("STATIC_MinigameSlyLock"); lock && !lock->meshes.empty()) {
                const ImVec2 c = overlay(p, 150, 95);
                const float r = 46 * m_s;
                for (auto it = lock->meshes.rbegin(); it != lock->meshes.rend(); ++it) {  // (the shadow first)
                    const bool shadow = it->v.size() >= 9 && it->v[5] < 0.1f;
                    const Tex& tex = m_scene.texture(it->texture);
                    if (!tex) continue;
                    const ImVec2 o = shadow ? ImVec2{c.x - 5 * m_s, c.y + 5 * m_s} : c;
                    dl->AddImage(tex.im(), {o.x - r, o.y - r}, {o.x + r, o.y + r}, {0, 0}, {1, 1}, shadow ? IM_COL32(0, 0, 0, 128) : IM_COL32(204, 204, 204, 255));
                }
            }
        } else {
            note = tr(brand::sly3Notice);  // (Sly 3, installed)
            width = 420;
        }
        // the note frame: right of and under the page's centre, clear of the arrow
        const float half = width * 0.5f * m_s;
        ImVec2 c = overlay(p, 280 + width * 0.5f, 236);
        if (page() == p) c.x = std::min(c.x, m_size.x - half - 20 * m_s);  // (a narrow window: kept inside it)
        const float size = 19 * m_s, lines = std::ceil(captionSize(size, note.c_str()).x / (2 * half - 44 * m_s) + 0.35f);
        const float h = std::max(62 * m_s, (lines * size * 1.3f + 44 * m_s) * 0.5f);
        const ImRect r({c.x - half, c.y - h}, {c.x + half, c.y + h});
        panel(dl, r, 1.0f);
        captionWrapped(dl, size, r.GetCenter(), r.GetWidth() - 44 * m_s, st == State::InDevelopment ? 0xFFD21E : 0xFFFFFF, note);
    }
    // The page left of the title (the PS3 menu: two bordered buttons, Credits and the teaser): SafeHouse's own four.
    void drawExtras(ImDrawList* dl, bool current) {
        static const char* labels[4] = {N_("Options"), N_("Credits"), N_("SafeHouse launcher"), N_("Quit")};
        for (int i = 0; i < 4; ++i) {
            const ImVec2 c = overlay(Page::Extras, 0, -165 + 110 * float(i));
            const bool on = current && m_extra == i;
            const float k = on ? 1.05f : 1.0f;  // (the PS3 menu: Game.UI.FrameScaleOn 1.05)
            const ImRect r({c.x - 190 * m_s * k, c.y - 46 * m_s * k}, {c.x + 190 * m_s * k, c.y + 46 * m_s * k});
            if (current) m_hitButton[size_t(i)] = r;
            button(dl, r, 1.0f);
            captionCentred(dl, (on ? 31 : 28) * m_s, r.GetCenter(), on ? 0xFFD21E : 0xFFFFFF, tr(labels[i]));
        }
    }
    // The last page (where the PS3 menu has its minigame circles and player pages): the developers, one circle each.
    // A circle shows the developer's silhouette on a grey disc until it is chosen (the mouse over it, the pad's focus,
    // a tap); then the disc takes its colour, the silhouette gives way to the picture, and a bar says who and what.
    // The circle itself is the menu's own piece (a player circle's rim and shadow; its inside is covered).
    void drawDevelopers(ImDrawList* dl, float dt) {
        const Page p = Page::Developers;
        const bool current = page() == p;
        banner(dl, overlay(p, 222, -262), 400 * m_s, tr("Developers"), 1.0f);
        // the first developer large at the left, the others where the PS3 page has its four circles
        static constexpr float place[5][3] = {{-322, -52, 146}, {110, -62, 104}, {334, -62, 104}, {110, 160, 104}, {334, 160, 104}};
        const MenuScene::Mesh* rim = nullptr;
        const MenuScene::Mesh* shadow = nullptr;
        for (const char* name : {"STATIC_Player1", "STATIC_MinigameSly"})
            if (auto* n = m_scene.node(name); n && !rim && n->meshes.size() >= 2) rim = &n->meshes[0], shadow = &n->meshes[1];
        auto piece = [&](const MenuScene::Mesh& mesh, ImVec2 c, float half, ImU32 colour) {
            const Tex& tex = m_scene.texture(mesh.texture);
            if (!tex || mesh.v.size() < 36) return false;
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (size_t k = 0; k + 8 < mesh.v.size(); k += 9) u0 = std::min(u0, mesh.v[k + 3]), u1 = std::max(u1, mesh.v[k + 3]), v0 = std::min(v0, mesh.v[k + 4]), v1 = std::max(v1, mesh.v[k + 4]);
            dl->AddImage(tex.im(), {c.x - half, c.y - half}, {c.x + half, c.y + half}, {u0, v0}, {u1, v1}, colour);
            return true;
        };
        for (int i = 0; i < 5; ++i) {
            float& shown = m_reveal[size_t(i)];
            shown = std::clamp(shown + (current && m_developer == i ? dt : -dt) / 0.22f, 0.0f, 1.0f);
            const ImVec2 c = overlay(p, place[i][0], place[i][1]);
            const float half = place[i][2] * m_s * (1.0f + 0.05f * shown), inner = half * 0.868f;
            if (current) m_hitDeveloper[size_t(i)] = ImRect({c.x - half, c.y - half}, {c.x + half, c.y + half});
            const bool drawn = rim && piece(*shadow, {c.x - half * 0.19f, c.y + half * 0.19f}, half, IM_COL32(0, 0, 0, 128)) && piece(*rim, c, half, IM_COL32(204, 204, 204, 255));
            if (!drawn) {  // (a menu without that piece: a plain rim)
                dl->AddCircleFilled({c.x - half * 0.1f, c.y + half * 0.1f}, half * 0.93f, IM_COL32(0, 0, 0, 128), 64);
                dl->AddCircleFilled(c, half * 0.93f, IM_COL32(235, 235, 235, 255), 64);
            }
            // the disc: grey, the developer's colour when chosen
            auto mix = [&](int grey, uint32_t colour, int shift) { return int(float(grey) + (float((colour >> shift) & 255) - float(grey)) * shown); };
            dl->AddCircleFilled(c, inner, IM_COL32(mix(150, kDevelopers[i].colour, 16), mix(150, kDevelopers[i].colour, 8), mix(150, kDevelopers[i].colour, 0), 255), 96);
            const auto& art = m_fixture.developers[size_t(i)];
            const ImVec2 a{c.x - inner, c.y - inner}, b{c.x + inner, c.y + inner};
            if (art.picture && shown > 0) dl->AddImageRounded(art.picture, a, b, {0, 0}, {1, 1}, IM_COL32(255, 255, 255, int(255 * shown)), inner, ImDrawFlags_RoundCornersAll);
            if (art.silhouette && shown < 1) dl->AddImage(art.silhouette, a, b, {0, 0}, {1, 1}, IM_COL32(255, 255, 255, int(255 * (1 - shown))));
        }
        // the bar: who and what, over the first circle
        if (!current || m_developer < 0) return;
        const Developer& who = kDevelopers[m_developer];
        const float alpha = m_reveal[size_t(m_developer)];
        ImVec2 c = overlay(p, -322, -272);
        c.x = std::max(c.x, 222 * m_s);
        const ImRect r({c.x - 212 * m_s, c.y - 60 * m_s}, {c.x + 212 * m_s, c.y + 60 * m_s});
        panel(dl, r, alpha);
        captionCentred(dl, 28 * m_s, {c.x, c.y - 30 * m_s}, 0xFFD21E, who.name, alpha);
        captionWrapped(dl, 16 * m_s, {c.x, c.y + 24 * m_s}, r.GetWidth() - 44 * m_s, 0xFFFFFF, tr(who.role), alpha);
    }
    // Sly 3: the SafeHouse launcher's question before it starts (its words; "Don't show again" is the same setting).
    void confirmInput(bool up, bool down, bool left, bool right, bool confirm, bool back) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int tapped = -1;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            for (int i = 0; i < 3; ++i)
                if (m_hitConfirm[size_t(i)].Contains(mouse) && m_hitConfirm[size_t(i)].Contains(ImGui::GetIO().MouseClickedPos[0])) tapped = i;
        if (up && m_confirmChoice != 0) m_confirmChoice = 0, sound(Sfx::Move);
        if (down && m_confirmChoice == 0) m_confirmChoice = 2, sound(Sfx::Move);
        if (left && m_confirmChoice == 2) m_confirmChoice = 1, sound(Sfx::Move);
        if (right && m_confirmChoice == 1) m_confirmChoice = 2, sound(Sfx::Move);
        if (tapped >= 0) m_confirmChoice = tapped;
        if (back) {
            m_confirm = false, sound(Sfx::Cancel);
            return;
        }
        if (!(confirm || tapped >= 0)) return;
        if (m_confirmChoice == 0) {
            m_confirmHide = !m_confirmHide, sound(Sfx::Move);
        } else if (m_confirmChoice == 1) {
            m_confirm = false, sound(Sfx::Cancel);
        } else {
            m_confirm = false, m_sly3Confirmed = true;
            if (m_confirmHide && !m_fixture.active) try {
                    m_backend.hideSly3Notice();
                } catch (...) {
                }
            sound(Sfx::Select);
            m_launching = int(GameId::Sly3), m_launchFade = 0;
        }
    }
    void drawConfirm(ImDrawList* dl) {
        for (auto& r : m_hitConfirm) r = {};
        if (!m_confirm) return;
        const float s = m_s, W = m_size.x, H = m_size.y;
        dl->AddRectFilled({0, 0}, m_size, draw::rgba(0x000000, 0.6f));
        const float pw = std::min(W - 40 * s, 760 * s);
        const ImRect pr({W * 0.5f - pw * 0.5f, H * 0.5f - 190 * s}, {W * 0.5f + pw * 0.5f, H * 0.5f + 190 * s});
        panel(dl, pr, 1.0f);
        captionWrapped(dl, 26 * s, {W * 0.5f, pr.Min.y + 60 * s}, pw - 60 * s, 0xFFD21E, tr("Sly 3 is in early development"));
        const char* body = tr("Sly 3: Honor Among Thieves is not fully playable yet. Expect missing features, visual and audio problems, "
                              "and crashes. Progress may stop at points the port cannot handle yet.");
        const float wrap = pw - 90 * s;
        const ImVec2 bs = ui::fonts().body->CalcTextSizeA(20 * s, FLT_MAX, wrap, body);
        dl->AddText(ui::fonts().body, 20 * s, {W * 0.5f - std::min(bs.x, wrap) * 0.5f, pr.Min.y + 104 * s}, draw::rgba(0xFFFFFF), body, nullptr, wrap);
        // "Don't show again" (a tick box), then Cancel and Play anyway (the menu's bordered buttons)
        const std::string hide = std::string(m_confirmHide ? "[x] " : "[  ] ") + tr("Don't show again");
        const ImVec2 hs = ui::fonts().tab->CalcTextSizeA(21 * s, FLT_MAX, 0, hide.c_str());
        m_hitConfirm[0] = ImRect({W * 0.5f - hs.x * 0.5f - 22 * s, pr.Max.y - 172 * s}, {W * 0.5f + hs.x * 0.5f + 22 * s, pr.Max.y - 116 * s});
        if (m_confirmChoice == 0) {
            dl->AddRectFilled(m_hitConfirm[0].Min, m_hitConfirm[0].Max, draw::rgba(0xFFD21E, 0.22f), 10 * s);
            dl->AddRect(m_hitConfirm[0].Min, m_hitConfirm[0].Max, draw::rgba(0xFFD21E, 0.9f), 10 * s, 0, 2.5f * s);
        }
        dl->AddText(ui::fonts().tab, 21 * s, {W * 0.5f - hs.x * 0.5f, m_hitConfirm[0].GetCenter().y - hs.y * 0.5f}, draw::rgba(0xFFFFFF), hide.c_str());
        static const char* labels[2] = {N_("Cancel"), N_("Play anyway")};
        for (int i = 0; i < 2; ++i) {
            const bool on = m_confirmChoice == i + 1;
            const float k = on ? 1.05f : 1.0f;
            const ImVec2 c{W * 0.5f + (i ? 1.0f : -1.0f) * std::min(170 * s, pw * 0.24f), pr.Max.y - 62 * s};
            const float hw = std::min(150 * s, pw * 0.22f) * k;
            const ImRect r({c.x - hw, c.y - 40 * s * k}, {c.x + hw, c.y + 40 * s * k});
            m_hitConfirm[size_t(i + 1)] = r;
            button(dl, r, 1.0f);
            captionCentred(dl, (on ? 24 : 22) * s, r.GetCenter(), on ? 0xFFD21E : 0xFFFFFF, tr(labels[i]));
        }
    }

    // The panels' Back button, top left, for a finger or the mouse (keys and pads have their own).
    void drawBack(ImDrawList* dl) {
        m_hitBack = {};
        if (!(m_options || m_credits) || !(m_touch || input::tracker().lastWasMouse())) return;
        const float s = m_s;
        const char* text = tr("Back");
        const float w = std::max(150 * s, captionSize(24 * s, text).x + 60 * s);
        m_hitBack = ImRect({20 * s, 24 * s}, {20 * s + w, 24 * s + 76 * s});
        button(dl, m_hitBack, 1.0f);
        captionCentred(dl, 24 * s, m_hitBack.GetCenter(), 0xFFFFFF, text);
    }

    // ---------------------------------------------------------------- footer, toasts, fades
    void drawFooter(ImDrawList* dl, input::Family family) const {
        const float s = m_s, W = m_size.x, H = m_size.y;
        using input::Button;
        std::vector<launcher::Hint> hints;
        if (m_credits) hints = {{ui::backButton(), GLFW_KEY_ESCAPE, N_("Back")}};
        else if (m_confirm) hints = {{ui::backButton(), GLFW_KEY_ESCAPE, N_("Back")}, {ui::confirmButton(), GLFW_KEY_ENTER, N_("Select")}};
        else if (m_options)
            hints = {{Button::Dpad, 0, N_("Choose")}, {Button::DpadLeft, GLFW_KEY_LEFT, N_("Change")}, {ui::backButton(), GLFW_KEY_ESCAPE, N_("Back")}};
        else {
            hints = {{Button::Dpad, GLFW_KEY_RIGHT, N_("Choose")}, {Button::Start, GLFW_KEY_TAB, N_("Options")},
                     {ui::backButton(), GLFW_KEY_ESCAPE, page() == Page::Title ? N_("Quit") : N_("Back")}};
            const State st = state(page());
            if (st == State::Playable || (st == State::InDevelopment && m_status[size_t(gameOf(page()))].runtime))
                hints.push_back({ui::confirmButton(), GLFW_KEY_ENTER, N_("Play")});
            else if (page() == Page::Extras) hints.push_back({ui::confirmButton(), GLFW_KEY_ENTER, N_("Select")});
        }
        drawPrompts(dl, hints, family);
    }
    // The button prompts, bottom right, in the menu's look: the controller family's own button pictures (the launcher's
    // prompt atlas) at a size that reads from the couch, keyboard keys as keycaps with the key's name written out (the
    // atlas' keycaps print "Tab" / "Esc" / "Enter" too small for this size), the labels in the menu's comic font. Not
    // shown after mouse input (and on a touch screen before any key or controller), as the launcher's own prompt bar.
    void drawPrompts(ImDrawList* dl, const std::vector<launcher::Hint>& hints, input::Family family) const {
        if (input::tracker().lastWasMouse() || m_touch) return;
#if defined(CANE_ANDROID)
        if (input::tracker().idle()) return;
#endif
        const float s = m_s, icon = std::round(40 * s), labelSize = 20 * s, gap = std::round(9 * s), between = std::round(30 * s);
        const bool keyboard = family == input::Family::Keyboard;
        ImFont* capFont = ui::fonts().tab;
        const float capPx = std::round(19 * s);
        // a key drawn here: its name on a keycap (the arrows keep the atlas' picture)
        auto written = [&](int key) {
            return keyboard && key != 0 && key != GLFW_KEY_LEFT && key != GLFW_KEY_RIGHT && key != GLFW_KEY_UP && key != GLFW_KEY_DOWN && key != input::kMouseLeftKey;
        };
        auto capWidth = [&](int key) {
            const std::string text = launcher::keyCapText(key);
            return std::max(icon * 0.9f, capFont->CalcTextSizeA(capPx, FLT_MAX, 0, text.c_str()).x + 22 * s);
        };
        float x = m_size.x - 30 * s;
        const float cy = m_size.y - 40 * s;
        for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
            if (keyboard ? it->key == 0 : it->button == input::Button::Count) continue;
            const char* label = it->label ? tr(it->label) : "";
            const ImVec2 ls = captionSize(labelSize, label);
            const float w = written(it->key) ? capWidth(it->key)
                            : keyboard ? launcher::drawKeyPrompt(nullptr, {}, icon, it->key, 0, false)
                                       : launcher::drawButtonPrompt(nullptr, {}, icon, it->button, family, 0, false);
            x -= ls.x;
            caption(dl, labelSize, {std::round(x), std::round(cy - ls.y * 0.5f)}, 0xFFFFFF, label);
            x -= gap + w;
            const ImVec2 at{std::round(x), std::round(cy - icon * 0.5f)};
            if (written(it->key)) {
                const std::string text = launcher::keyCapText(it->key);
                const float h = std::round(icon * 0.8f), top = std::round(cy - h * 0.5f), r = 7 * s;
                dl->AddRectFilled({at.x + 2 * s, top + 3 * s}, {at.x + w + 2 * s, top + h + 3 * s}, draw::rgba(0x000000, 0.7f), r);
                dl->AddRectFilled({at.x, top}, {at.x + w, top + h}, draw::rgba(0xF4F6FB), r);
                dl->AddRect({at.x, top}, {at.x + w, top + h}, draw::rgba(0x000000), r, 0, std::max(1.5f, 2.5f * s));
                const ImVec2 ts = capFont->CalcTextSizeA(capPx, FLT_MAX, 0, text.c_str());
                dl->AddText(capFont, capPx, {std::round(at.x + (w - ts.x) * 0.5f), std::round(top + (h - ts.y) * 0.5f)}, draw::rgba(0x10131C), text.c_str());
            } else {
                // (a dark copy behind keeps a light button picture readable over the scene)
                ImGui::PushFont(capFont);
                if (keyboard) launcher::drawKeyPrompt(dl, at, icon, it->key, draw::rgba(0xFFFFFF));
                else launcher::drawButtonPrompt(dl, at, icon, it->button, family, draw::rgba(0xFFFFFF));
                ImGui::PopFont();
            }
            x -= between;
        }
    }
    void toast(std::string text) {
        m_toast = tr(text);
        m_toastTime = 5.0f;
    }
    void drawToast(ImDrawList* dl, float dt) {
        if (m_toastTime <= 0) return;
        m_toastTime -= dt;
        const float s = m_s, a = std::min(1.0f, m_toastTime * 2);
        const float wrap = std::min(m_size.x - 80 * s, 760 * s);
        const ImVec2 t = ui::fonts().body->CalcTextSizeA(19 * s, FLT_MAX, wrap, m_toast.c_str());
        const ImRect r({m_size.x * 0.5f - t.x * 0.5f - 34 * s, 22 * s}, {m_size.x * 0.5f + t.x * 0.5f + 34 * s, 22 * s + t.y + 40 * s});
        panel(dl, r, a);
        dl->AddText(ui::fonts().body, 19 * s, {r.Min.x + 34 * s, r.Min.y + 20 * s}, draw::rgba(0xFFFFFF, a), m_toast.c_str(), nullptr, wrap);
    }
    // To black before a game (0.5 s, the PS3 menu's page fade), from black at the start and after a game.
    void drawFades(ImDrawList* dl, float dt) {
        if (m_startFade > 0) {
            dl->AddRectFilled({0, 0}, m_size, draw::rgba(0x000000, m_startFade));
            m_startFade = std::max(0.0f, m_startFade - dt * 2.0f);
        }
        if (m_returnFade > 0) {
            dl->AddRectFilled({0, 0}, m_size, draw::rgba(0x000000, m_returnFade));
            m_returnFade = std::max(0.0f, m_returnFade - dt * 2.0f);
        }
        if (m_launching < 0) return;
        if (!gameRunning()) {
            const float before = m_launchFade;
            m_launchFade = std::min(1.0f, m_launchFade + dt * 2.0f);
            if (before < 1.0f && m_launchFade >= 1.0f && m_fixture.screen != "launch") launchNow();
        }
        const float a = m_launchFade, s = m_s;
        dl->AddRectFilled({0, 0}, m_size, draw::rgba(0x000000, a));
        // the collection's loading screen
        if (m_loading) {
            const float h = m_size.y, w = h * m_loading.aspect();
            dl->AddImage(m_loading.im(), {m_size.x * 0.5f - w * 0.5f, 0}, {m_size.x * 0.5f + w * 0.5f, h}, {0, 0}, {1, 1}, draw::rgba(0xFFFFFF, a));
        }
        const ImVec2 c{m_size.x - 60 * s, m_size.y - 60 * s};
        const float start = float(m_time) * 5.0f;
        dl->PathArcTo(c, 14 * s, start, start + 4.6f, 24);
        dl->PathStroke(draw::rgba(0xFFFFFF, a * 0.9f), 0, 4 * s);
    }

    // ---------------------------------------------------------------- options
    struct Row {
        std::string section;  // a header row when label is empty
        std::string label;
        std::function<std::string()> value;  // empty: an action row
        std::function<void(int)> change;     // -1 / +1
        std::function<void()> action;
        std::string help;
    };
    void openOptions() {
        sound(Sfx::Select);
        m_options = true;
        m_optionsScroll = 0;
        if (!m_fixture.active) loadSly2Settings();
        buildRows();
        m_row = 1;
    }
    void loadSly2Settings() {
        try {
            m_sly2Load = loadSly2(m_backend.userDir(GameId::Sly2), m_sly2);
        } catch (...) {
            m_sly2Load = Sly2Load::Unreadable;
        }
    }
    void saveSly2Settings() {
        if (m_fixture.active || m_sly2Load == Sly2Load::Unreadable) return;
        try {
            fs::create_directories(m_backend.userDir(GameId::Sly2));
            if (!saveSly2(m_backend.userDir(GameId::Sly2), m_sly2)) toast(tr("Could not save Sly 2's settings."));
        } catch (const std::exception& e) {
            toast(e.what());
        }
    }
    // A Sly 2 option by its Settings.ini key (platform/sly2_settings.h), stepping through the values listed.
    Row sly2Row(const char* section, const char* key, std::vector<std::string> values, const char* help) {
        const Sly2Key* k = nullptr;
        for (const auto& e : sly2Schema())
            if (std::string(e.section) == section && std::string(e.key) == key) k = &e;
        Row r;
        r.label = key;
        r.help = help;
        if (!k) return r;
        r.value = [this, k] { return k->write(m_sly2); };
        r.change = [this, k, values](int d) {
            const std::string now = k->write(m_sly2);
            auto it = std::find(values.begin(), values.end(), now);
            int i = it == values.end() ? 0 : int(it - values.begin()) + d;
            i = (i % int(values.size()) + int(values.size())) % int(values.size());
            if (k->read(m_sly2, values[size_t(i)])) saveSly2Settings();
        };
        return r;
    }
    static std::vector<std::string> percents() {
        std::vector<std::string> v;
        for (int i = 0; i <= 100; i += 10) v.push_back(std::to_string(i));
        return v;
    }
    void buildRows() {
        m_rows.clear();
        m_rows.push_back({N_("Collection"), "", {}, {}, {}, ""});
        m_rows.push_back({"", N_("Music volume"), [this] { return std::to_string(int(std::lround(m_musicVolume * 100))) + "%"; },
                          [this](int d) {
                              m_musicVolume = std::clamp(std::round(m_musicVolume * 10 + float(d)) / 10.0f, 0.0f, 1.0f);
                              m_audio.setVolumes(m_musicVolume, m_soundVolume);
                              collectionConfig()["music"] = m_musicVolume;
                              saveConfig();
                          },
                          {}, N_("The menu's music.")});
        m_rows.push_back({"", N_("Sound volume"), [this] { return std::to_string(int(std::lround(m_soundVolume * 100))) + "%"; },
                          [this](int d) {
                              m_soundVolume = std::clamp(std::round(m_soundVolume * 10 + float(d)) / 10.0f, 0.0f, 1.0f);
                              m_audio.setVolumes(m_musicVolume, m_soundVolume);
                              collectionConfig()["sounds"] = m_soundVolume;
                              saveConfig();
                              sound(Sfx::Move);
                          },
                          {}, N_("The menu's sounds.")});
#ifndef CANE_ANDROID  // (Android: the app fills its window)
        m_rows.push_back({"", N_("Start in fullscreen"), [this] { return collectionConfig().value("fullscreen", true) ? std::string(N_("On")) : std::string(N_("Off")); },
                          [this](int) {
                              collectionConfig()["fullscreen"] = !collectionConfig().value("fullscreen", true);
                              saveConfig();
                          },
                          {}, N_("Opens this menu full screen. Applies the next time it opens.")});
#endif
        static const std::vector<std::string> styles{N_("Automatic"), "PlayStation", "Xbox", "Nintendo", "Steam Deck", "Steam Controller", "PlayStation 4", "PlayStation 5", "Steam Controller (2026)"};
        m_rows.push_back({"", N_("Button prompts"),
                          [this] { return styles[size_t(std::clamp(m_backend.config.value("prompt_style", 0), 0, int(styles.size()) - 1))]; },
                          [this](int d) {
                              const int n = int(styles.size());
                              const int v = ((m_backend.config.value("prompt_style", 0) + d) % n + n) % n;
                              m_backend.config["prompt_style"] = v;
                              if (v == 0) m_backend.config.erase("prompt_style");
                              saveConfig();
                          },
                          {}, N_("Which controller's buttons are shown (shared with the SafeHouse launcher).")});
        m_rows.push_back({"", N_("Remove the imported Collection menu"), {}, {},
                          [this] {
                              if (m_fixture.active) return;
                              ps3menu::removeMenu(m_profile);
                              openSafeHouse();
                              m_quit = true;
                          },
                          N_("Deletes the menu imported from your Sly Collection disc and opens the SafeHouse launcher. The Collection style needs it: import it again there.")});
        // Sly 2's own options, the same Settings.ini the game and the SafeHouse launcher use
        m_rows.push_back({"Sly 2: Band of Thieves", "", {}, {}, {}, ""});
        m_rows.push_back(sly2Row(N_("Display"), N_("Window Mode"), {N_("Windowed"), N_("Borderless"), N_("Fullscreen")}, N_("How the game's window opens.")));
        m_rows.push_back(sly2Row(N_("Display"), N_("Aspect Ratio"), {N_("Fit"), "4:3", "16:9", "16:10", N_("PS2 4:3"), N_("PS2 16:9")}, N_("The picture's shape.")));
        m_rows.push_back(sly2Row(N_("Display"), N_("VSync"), {N_("On"), N_("Off")}, N_("Waits for the screen's refresh.")));
        m_rows.push_back(sly2Row(N_("Display"), N_("Frame Rate Limit"), {"30", "60", "120", "144", "240", N_("Uncapped")}, N_("Frames per second.")));
        m_rows.push_back(sly2Row(N_("Graphics"), N_("Internal Resolution"), {N_("Native"), "480", "720", "1080", "1440", "2160"}, N_("The 3D scene's height.")));
        m_rows.push_back(sly2Row(N_("Graphics"), N_("Anti-Aliasing"), {N_("Off"), "2x", "4x", "8x"}, N_("Smooths edges. Higher needs a faster GPU.")));
        m_rows.push_back(sly2Row(N_("Audio"), kSaveOptionMusic, percents(), N_("The game's music.")));
        m_rows.push_back(sly2Row(N_("Audio"), kSaveOptionEffects, percents(), N_("The game's sound effects.")));
        m_rows.push_back(sly2Row(N_("Audio"), N_("Dialogue Volume"), percents(), N_("The game's voices.")));
        m_rows.push_back(sly2Row(N_("Controls"), kSaveOptionVibration, {N_("On"), N_("Off")}, N_("The controller's rumble.")));
        m_rows.push_back({"SafeHouse", "", {}, {}, {}, ""});
        m_rows.push_back({"", N_("Open the SafeHouse launcher"), {}, {}, [this] { openSafeHouse(); },
#ifdef CANE_ANDROID
                          N_("Install games, more options and home screen shortcuts.")});
#else
                          N_("Install games, more options, Steam and desktop shortcuts.")});
#endif
        m_rows.push_back({"", N_("Quit"), {}, {}, [this] { m_quit = true; }, ""});
    }
    void optionsInput(bool up, bool down, bool left, bool right, bool confirm, bool back) {
        if (back) {
            m_options = false;
            sound(Sfx::Cancel);
            return;
        }
        auto step = [&](int d) {
            for (int n = 0; n < int(m_rows.size()); ++n) {
                m_row = (m_row + d + int(m_rows.size())) % int(m_rows.size());
                if (!m_rows[size_t(m_row)].label.empty()) break;
            }
            m_ensureVisible = true;
            sound(Sfx::Move);
        };
        if (up) step(-1);
        if (down) step(+1);
        Row& r = m_rows[size_t(m_row)];
        if ((left || right) && r.change) r.change(left ? -1 : 1);
        if (confirm) {
            if (r.action) r.action();
            else if (r.change) r.change(1);
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) m_optionsScroll -= wheel * 60 * m_s;
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) m_optionsScroll -= ImGui::GetIO().MouseDelta.y, m_dragged = true;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (!m_dragged)
                for (size_t i = 0; i < m_rowRects.size(); ++i)
                    if (m_rowRects[i].Contains(ImGui::GetIO().MousePos) && !m_rows[i].label.empty()) {
                        m_row = int(i);
                        Row& c = m_rows[i];
                        if (c.action) c.action();
                        else if (c.change) {
                            // a finger: the left arrow steps back, anywhere else on the row steps on
                            const bool minus = m_touch ? i < m_rowMinus.size() && m_rowMinus[i].Contains(ImGui::GetIO().MousePos)
                                                       : ImGui::GetIO().MousePos.x < m_rowRects[i].GetCenter().x + m_rowRects[i].GetWidth() * 0.15f;
                            c.change(minus ? -1 : 1);
                        }
                        break;
                    }
            m_dragged = false;
        }
        if (input::tracker().lastWasMouse() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            for (size_t i = 0; i < m_rowRects.size(); ++i)
                if (m_rowRects[i].Contains(ImGui::GetIO().MousePos) && !m_rows[i].label.empty()) m_row = int(i);
    }
    void drawOptions(ImDrawList* dl) {
        const float s = m_s, W = m_size.x, H = m_size.y;
        dl->AddRectFilled({0, 0}, m_size, draw::rgba(0x000000, 0.6f));
        banner(dl, {W * 0.5f, 62 * s}, 400 * s, tr("Options"), 1.0f);
        const float pw = std::min(W - 48 * s, 900 * s), top = 126 * s, bottom = H - 120 * s;
        const ImRect pr({W * 0.5f - pw * 0.5f, top}, {W * 0.5f + pw * 0.5f, bottom});
        panel(dl, pr, 1.0f);
        const float rowH = (m_touch ? 66 : 44) * s, headH = 50 * s;  // (a finger needs taller rows)
        // scroll the chosen row into view
        float y = 0, chosenY = 0, chosenH = rowH;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const float h = m_rows[i].label.empty() ? headH : rowH;
            if (int(i) == m_row) chosenY = y, chosenH = h;
            y += h;
        }
        const float viewH = pr.GetHeight() - 48 * s, total = y;
        if (m_ensureVisible) {
            if (chosenY - m_optionsScroll < 0) m_optionsScroll = chosenY - headH;
            if (chosenY + chosenH - m_optionsScroll > viewH) m_optionsScroll = chosenY + chosenH - viewH;
            m_ensureVisible = false;
        }
        m_optionsScroll = std::clamp(m_optionsScroll, 0.0f, std::max(0.0f, total - viewH));
        dl->PushClipRect({pr.Min.x + 10 * s, pr.Min.y + 24 * s}, {pr.Max.x - 10 * s, pr.Max.y - 24 * s}, true);
        m_rowRects.assign(m_rows.size(), {});
        m_rowMinus.assign(m_rows.size(), {});
        y = pr.Min.y + 24 * s - m_optionsScroll;
        const float lx = pr.Min.x + 48 * s, rx = pr.Max.x - 48 * s;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row& r = m_rows[i];
            if (r.label.empty()) {
                caption(dl, 22 * s, {lx - 14 * s, y + 10 * s}, 0xFFD21E, tr(r.section.c_str()));
                y += headH;
                continue;
            }
            const ImRect rr({pr.Min.x + 28 * s, y}, {pr.Max.x - 28 * s, y + rowH - 4 * s});
            m_rowRects[i] = rr;
            const bool on = int(i) == m_row;
            if (on) {
                dl->AddRectFilled(rr.Min, rr.Max, draw::rgba(0xFFD21E, 0.22f), 10 * s);
                dl->AddRect(rr.Min, rr.Max, draw::rgba(0xFFD21E, 0.9f), 10 * s, 0, 2.5f * s);
            }
            const float ty = rr.GetCenter().y - 11 * s;
            dl->AddText(ui::fonts().tab, 21 * s, {lx, ty}, draw::rgba(0xFFFFFF, on ? 1.0f : 0.88f), tr(r.label.c_str()));
            if (r.value) {
                const std::string v = tr(r.value());
                const ImVec2 vs = ui::fonts().tab->CalcTextSizeA(21 * s, FLT_MAX, 0, v.c_str());
                dl->AddText(ui::fonts().tab, 21 * s, {rx - vs.x - 26 * s, ty}, draw::rgba(on ? 0xFFD21E : 0xFFFFFF), v.c_str());
                m_rowMinus[i] = ImRect({rx - vs.x - 110 * s, rr.Min.y}, {rx - vs.x - 12 * s, rr.Max.y});
                if (on || m_touch) {
                    dl->AddTriangleFilled({rx - vs.x - 36 * s, rr.GetCenter().y - 8 * s}, {rx - vs.x - 36 * s, rr.GetCenter().y + 8 * s}, {rx - vs.x - 48 * s, rr.GetCenter().y},
                                          draw::rgba(0xFFD21E));
                    dl->AddTriangleFilled({rx - 16 * s, rr.GetCenter().y - 8 * s}, {rx - 16 * s, rr.GetCenter().y + 8 * s}, {rx - 4 * s, rr.GetCenter().y}, draw::rgba(0xFFD21E));
                }
            }
            y += rowH;
        }
        dl->PopClipRect();
        // the chosen row's help under the panel
        const Row& r = m_rows[size_t(std::clamp(m_row, 0, int(m_rows.size()) - 1))];
        std::string help = tr(r.help);
        if (r.section.empty() && r.value && m_sly2Load == Sly2Load::Unreadable && r.label != "Music volume") help += std::string(" ") + tr("(Sly 2's Settings.ini cannot be read: not saved.)");
        if (!help.empty()) {
            const float wrap = pw - 40 * s;
            const ImVec2 hs = ui::fonts().body->CalcTextSizeA(19 * s, FLT_MAX, wrap, help.c_str());
            dl->AddText(ui::fonts().body, 19 * s, {W * 0.5f - std::min(hs.x, wrap) * 0.5f, bottom + 10 * s}, draw::rgba(0xFFFFFF), help.c_str(), nullptr, wrap);
        }
    }
    // ---------------------------------------------------------------- credits
    void drawCredits(ImDrawList* dl) {
        const float s = m_s, W = m_size.x, H = m_size.y;
        dl->AddRectFilled({0, 0}, m_size, draw::rgba(0x000000, 0.6f));
        banner(dl, {W * 0.5f, 62 * s}, 400 * s, tr("Credits"), 1.0f);
        const float pw = std::min(W - 48 * s, 900 * s);
        const ImRect pr({W * 0.5f - pw * 0.5f, 130 * s}, {W * 0.5f + pw * 0.5f, H - 90 * s});
        panel(dl, pr, 1.0f);
        // The full credits (launcher/credits.h: the same sections as the SafeHouse launcher's About page), then the
        // package's notices. Headings are translated; names, links and licences stay as they are.
        struct Block {
            std::string id, heading, body;
        };
        std::vector<Block> lines;
        for (const credits::Section& section : credits::sections()) {
            std::string body;
            for (const credits::Line& line : section.lines) {
                if (line.kind == credits::Line::Developers)
                    for (const Developer& d : kDevelopers) body += std::string(body.empty() ? "" : "\n") + d.name + ": " + tr(d.role);
                else body += std::string(body.empty() ? "" : "\n") + (line.kind == credits::Line::Link ? credits::linkText(line) : std::string(tr(line.text)));
            }
            lines.push_back({section.id, tr(section.heading), body});
        }
        // the notices themselves, when the package's file is beside the program
        if (m_notices.empty() && !m_noticesRead) {
            m_noticesRead = true;
            std::error_code ec;
            for (const fs::path& f : {executableDirectory().parent_path() / "THIRD_PARTY_NOTICES.md", executableDirectory() / "THIRD_PARTY_NOTICES.md"})
                if (m_notices.empty() && fs::is_regular_file(f, ec)) {
                    const auto bytes = readBytes(f);
                    m_notices.assign(bytes.begin(), bytes.end());
                }
        }
        if (!m_notices.empty()) lines.push_back({"notices", "THIRD_PARTY_NOTICES.md", m_notices});
        const float wrap = pw - 96 * s;
        float total = 0, target = -1;
        for (const Block& b : lines) {
            if (b.id == m_creditsSection) target = total;  // (screenshots: "--screen credits:<id>" opens the panel there)
            total += 38 * s + ui::fonts().body->CalcTextSizeA(19 * s, FLT_MAX, wrap, b.body.c_str()).y + 22 * s;
        }
        if (target >= 0) m_creditsScroll = target, m_creditsSection.clear();
        const float viewH = pr.GetHeight() - 56 * s;
        m_creditsScroll = std::clamp(m_creditsScroll, 0.0f, std::max(0.0f, total - viewH));
        dl->PushClipRect({pr.Min.x, pr.Min.y + 26 * s}, {pr.Max.x, pr.Max.y - 26 * s}, true);
        float y = pr.Min.y + 34 * s - m_creditsScroll;
        for (const Block& b : lines) {
            const float top = y;
            caption(dl, 20 * s, {pr.Min.x + 46 * s, y}, 0xFFD21E, b.heading.c_str());
            y += 38 * s;
            dl->AddText(ui::fonts().body, 19 * s, {pr.Min.x + 48 * s, y}, draw::rgba(0xFFFFFF), b.body.c_str(), nullptr, wrap);
            y += ui::fonts().body->CalcTextSizeA(19 * s, FLT_MAX, wrap, b.body.c_str()).y + 22 * s;
            // the layout dump (--dump-layout): every section, and each link's address, where it is drawn
            const ImRect rect({pr.Min.x + 46 * s, top}, {pr.Min.x + 48 * s + wrap, y - 22 * s});
            ui::recordLayout(("credits-" + b.id).c_str(), rect);
        }
        if (ui::layoutRecording())
            for (const credits::Section& section : credits::sections())
                for (const credits::Line& line : section.lines)
                    if (line.kind == credits::Line::Link) ui::recordLayout((std::string("credits-link ") + line.url).c_str(), pr);
        dl->PopClipRect();
    }

    LauncherBackend& m_backend;
    GLFWwindow* m_window;
    Fixture m_fixture;
    fs::path m_profile;
    ui::Job m_job;
    Audio m_audio;
    std::thread m_loader;
    std::mutex m_soundMutex;
    std::atomic<bool> m_soundsReady{false};
    std::shared_ptr<Sounds> m_sounds, m_pendingSounds;
    json m_audioFiles;
    MenuScene m_scene;
    MenuScene::View m_view;
    MenuFont m_font;
    Bloom m_bloom;
    Tex m_banner[3], m_frame[9], m_border[9], m_loading;
    std::vector<PageInfo> m_pages;
    std::array<Status, 3> m_status{};
    double m_time = 0, m_nextStatus = 0;
    ImVec2 m_size{1280, 720}, m_dragFrom{};
    float m_s = 1, m_cameraX = 0, m_glide = 0.1f, m_delay = 0.5f, m_inputWait = 0, m_returnFade = 0, m_launchFade = 0, m_startFade = 0;
    int m_page = 0, m_extra = 0, m_launching = -1, m_playing = -1, m_confirmChoice = 2;
    bool m_confirm = false, m_confirmHide = false, m_sly3Confirmed = false;
    std::array<ImRect, 3> m_hitConfirm{};
    std::array<ImRect, 5> m_hitDeveloper{};
    std::array<float, 5> m_reveal{};
    int m_developer = -1;
    float m_dt = 0;
    ImRect m_hitLeft, m_hitRight, m_hitLogo;
    std::array<ImRect, 4> m_hitButton{};
    bool m_quit = false, m_options = false, m_credits = false, m_wasBusy = false, m_ensureVisible = false, m_frozen = false;
    bool m_dragging = false, m_dragged = false, m_dragCamera = false, m_touch = false;
    float m_dragCameraFrom = 0;
    int m_dragPage = 0;
    std::deque<std::pair<double, float>> m_trail;  // the pointer's recent places (time, x): a flick's speed
    ImRect m_hitBack;
    float m_musicVolume = 0.7f, m_soundVolume = 0.8f, m_optionsScroll = 0, m_creditsScroll = 0, m_toastTime = 0;
    std::string m_creditsSection;  // a section the credits panel scrolls to once (screenshots)
    std::string m_toast, m_notices;
    bool m_noticesRead = false;
    std::vector<Row> m_rows;
    std::vector<ImRect> m_rowRects, m_rowMinus;
    int m_row = 1;
    Sly2Settings m_sly2;
    Sly2Load m_sly2Load = Sly2Load::Missing;
};
}  // namespace cane::collection
