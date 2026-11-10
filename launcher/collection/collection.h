#pragma once
// The Collection Style Launcher (`safehouse --collection`): an extra, playful way to start the games, after the
// PlayStation 3 collection's own menu. A drive at night through the games' worlds scrolls behind a big logo; Left and
// Right choose a game, the logo crossfades, and choosing one fades to black and starts it. When the game quits, the
// menu comes back (the games show "Quit to Launcher" in their pause menus: SAFEHOUSE_LAUNCHER=collection).
//
// Items, in order: the collection itself, Sly 1, Sly 2, Sly 3, Sly 4, Bentley's Hackpack, Extras (Options, Credits, the
// SafeHouse launcher, Quit). A game that isn't installed is locked (installing happens in the SafeHouse launcher); Sly
// 3 is marked in development, Sly 4 and the Hackpack have nothing to install yet, as the SafeHouse launcher shows them.
// The scene's worlds, the music and the logos follow what is installed (assets.h): the PS3 collection's own menu files
// when the player imported them, else Sly 2's music and skies, else a drawn scene and the launcher's own logos.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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
#include "../native_dialog.h"
#include "../ui/face_buttons.h"
#include "../ui/gamepad_nav.h"
#include "../ui/job.h"
#include "../ui/prompt_bar.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../steam.h"
#include "assets.h"
#include "audio.h"
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

// ---------------------------------------------------------------- GL textures of the found images
struct Tex {
    GLuint id = 0;
    int w = 0, h = 0;
    Tex() = default;
    explicit Tex(const Image& img) {
        if (img.empty()) return;
        w = img.width, h = img.height;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    Tex(const Tex&) = delete;
    Tex& operator=(const Tex&) = delete;
    Tex(Tex&& o) noexcept : id(o.id), w(o.w), h(o.h) { o.id = 0; }
    Tex& operator=(Tex&& o) noexcept {
        std::swap(id, o.id), std::swap(w, o.w), std::swap(h, o.h);
        return *this;
    }
    ~Tex() {
        if (id) glDeleteTextures(1, &id);
    }
    explicit operator bool() const { return id != 0; }
    ImTextureID im() const { return reinterpret_cast<ImTextureID>(static_cast<intptr_t>(id)); }
    float aspect() const { return h ? float(w) / float(h) : 1.0f; }
};

// ---------------------------------------------------------------- drawing helpers
namespace draw {
inline ImU32 rgba(uint32_t rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, int(std::clamp(a, 0.0f, 1.0f) * 255));
}
inline uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto c = [&](int s) { return uint32_t(std::lround(float((a >> s) & 255) * (1 - t) + float((b >> s) & 255) * t)) << s; };
    return c(16) | c(8) | c(0);
}
inline float hash(int n) {
    uint32_t x = uint32_t(n) * 0x9E3779B1u;
    x ^= x >> 15, x *= 0x85EBCA77u, x ^= x >> 13, x *= 0xC2B2AE3Du, x ^= x >> 16;
    return float(x & 0xFFFFFF) / float(0xFFFFFF);
}
// Text with a dark outline (the collection's comic captions).
inline ImVec2 outlined(ImDrawList* dl, ImFont* f, float size, ImVec2 p, uint32_t fill, const char* text, float alpha = 1.0f,
                       float outline = -1, uint32_t line = 0x1A1030) {
    if (outline < 0) outline = std::max(1.5f, size * 0.07f);
    p = {std::round(p.x), std::round(p.y)};
    for (int i = 0; i < 12; ++i) {
        const float a = float(i) * 6.2831853f / 12.0f;
        dl->AddText(f, size, {p.x + std::cos(a) * outline, p.y + std::sin(a) * outline}, rgba(line, alpha), text);
    }
    dl->AddText(f, size, {p.x, p.y + outline * 0.6f}, rgba(line, alpha), text);
    dl->AddText(f, size, p, rgba(fill, alpha), text);
    return f->CalcTextSizeA(size, FLT_MAX, 0, text);
}
inline ImVec2 measure(ImFont* f, float size, const char* text) { return f->CalcTextSizeA(size, FLT_MAX, 0, text); }
// The chunky yellow arrow beside the logo (dir -1 left, +1 right).
inline void arrow(ImDrawList* dl, ImVec2 c, float s, int dir, float alpha, bool lit) {
    const float d = float(dir);
    // a short shaft and a wide head, as a fat chevron
    const ImVec2 pts[7] = {{c.x - d * s * 0.55f, c.y - s * 0.18f}, {c.x + d * s * 0.02f, c.y - s * 0.18f}, {c.x + d * s * 0.02f, c.y - s * 0.48f},
                           {c.x + d * s * 0.62f, c.y},             {c.x + d * s * 0.02f, c.y + s * 0.48f}, {c.x + d * s * 0.02f, c.y + s * 0.18f},
                           {c.x - d * s * 0.55f, c.y + s * 0.18f}};
    // (convex pieces: the head, then the shaft)
    auto fill = [&](ImVec2 off, ImU32 col) {
        const ImVec2 head[3] = {{pts[2].x + off.x, pts[2].y + off.y}, {pts[3].x + off.x, pts[3].y + off.y}, {pts[4].x + off.x, pts[4].y + off.y}};
        const ImVec2 shaft[4] = {{pts[0].x + off.x, pts[0].y + off.y}, {pts[1].x + off.x, pts[1].y + off.y}, {pts[5].x + off.x, pts[5].y + off.y},
                                 {pts[6].x + off.x, pts[6].y + off.y}};
        dl->AddConvexPolyFilled(head, 3, col);
        dl->AddConvexPolyFilled(shaft, 4, col);
    };
    fill({s * 0.06f, s * 0.08f}, rgba(0x120A1E, 0.75f * alpha));
    fill({0, 0}, rgba(lit ? 0xFFE45C : 0xF6C21C, alpha));
    dl->AddPolyline(pts, 7, rgba(0x1A1030, alpha), ImDrawFlags_Closed, std::max(2.0f, s * 0.07f));
    // the highlight on the head's upper edge
    dl->AddLine({c.x + d * s * 0.08f, c.y - s * 0.34f}, {c.x + d * s * 0.48f, c.y - s * 0.02f}, rgba(0xFFF6C8, 0.8f * alpha), std::max(1.5f, s * 0.05f));
}
// The padlock badge on a locked logo.
inline void lockBadge(ImDrawList* dl, ImVec2 c, float r, float alpha) {
    dl->AddCircleFilled({c.x + r * 0.08f, c.y + r * 0.1f}, r, rgba(0x0A0616, 0.6f * alpha), 40);
    dl->AddCircleFilled(c, r, rgba(0x1F64C8, alpha), 40);
    dl->AddCircle(c, r, rgba(0xDDEBFF, alpha), 40, std::max(2.0f, r * 0.1f));
    dl->AddCircle(c, r * 0.86f, rgba(0x6FB4FF, alpha * 0.8f), 40, std::max(1.0f, r * 0.05f));
    const float w = r * 0.9f, h = r * 0.7f;
    dl->AddRectFilled({c.x - w * 0.5f, c.y - h * 0.15f}, {c.x + w * 0.5f, c.y + h * 0.75f}, rgba(0xF2F6FF, alpha), r * 0.12f);
    dl->PathArcTo({c.x, c.y - h * 0.15f}, w * 0.32f, 3.14159f, 6.28318f, 16);
    dl->PathStroke(rgba(0xF2F6FF, alpha), 0, std::max(2.0f, r * 0.16f));
    dl->AddCircleFilled({c.x, c.y + h * 0.25f}, r * 0.1f, rgba(0x1F64C8, alpha), 12);
}
}  // namespace draw

// ---------------------------------------------------------------- the menu
class Menu {
public:
    enum class Item { Collection, Sly1, Sly2, Sly3, Sly4, Hackpack, Extras };
    static constexpr int kItems = 7;
    enum class State { Playable, InDevelopment, NotInstalled, Unavailable, Page };
    // Screenshots: the games shown as installed, and the PS3 / Sly 2 sources to read instead of the profile's.
    struct Fixture {
        bool active = false;
        std::array<bool, 3> installed{};  // Sly 1, Sly 2, Sly 3 (Sly 3: installed, runtime present)
        fs::path ps3Profile;              // a profile holding Collection/PS3 menu (empty: none)
        fs::path sly2Installation;        // music and skies (empty: none)
        std::string screen;               // "", "options", "extras", "credits", "launch"
    };

    Menu(LauncherBackend& backend, GLFWwindow* window, Fixture fixture) : m_backend(backend), m_window(window), m_fixture(std::move(fixture)) {
        refreshStatus(true);
        loadAssets();
        m_item = m_fixture.active ? 0 : std::clamp(m_backend.config.value("collection_item", 0), 0, kItems - 1);
        if (m_fixture.screen == "options") openOptions();
        else if (m_fixture.screen == "extras") m_item = int(Item::Extras);
        else if (m_fixture.screen == "credits") m_item = int(Item::Extras), m_credits = true;
        else if (m_fixture.screen == "launch") m_launchFade = 1.0f, m_launching = int(Item::Sly2);
        m_shownItem = m_item;
        const auto& c = collectionConfig();
        m_musicVolume = std::clamp(c.value("music", 0.7f), 0.0f, 1.0f);
        m_soundVolume = std::clamp(c.value("sounds", 0.8f), 0.0f, 1.0f);
        if (!m_fixture.active && !std::getenv("SAFEHOUSE_COLLECTION_SILENT") && m_audio.open()) startSoundLoader();
        m_audio.setVolumes(m_musicVolume, m_soundVolume);
    }
    ~Menu() {
        m_job.cancel = true;
        if (m_loader.joinable()) m_loader.join();
    }
    bool gameRunning() const { return m_job.busy && m_playing >= 0; }
    // A game is starting, running or the menu is fading back in.
    bool busy() const { return m_fixture.screen != "launch" && (m_launching >= 0 || m_job.busy || m_returnFade > 0); }
    bool quitRequested() const { return m_quit; }
    // A frame. size: the window in pixels; pad: the edges from GamepadNav; family: the prompt family.
    void frame(ImVec2 size, const launcher::PadEdges& pad, input::Family family, float dt) {
        m_time += dt;
        m_job.reap();
        pollJob();
        refreshStatus(false);
        pollSounds();
        updateMusic();
        input(pad, size);
        auto* dl = ImGui::GetBackgroundDrawList();
        const float s = size.y / 720.0f;
        m_s = s;
        if (Item(m_item) == Item::Extras) drawDusk(dl, size);
        else drawScene(dl, size, dt);
        // the page fades and logo crossfade
        m_fade = std::min(1.0f, m_fade + dt * 4.0f);
        if (m_shownItem != m_item && m_fade >= 1.0f) m_shownItem = m_item;
        auto* fg = ImGui::GetForegroundDrawList();
        if (Item(m_item) == Item::Extras) drawExtras(fg, size);
        else drawItem(fg, size);
        drawFooter(fg, size, family);
        if (m_options) drawOptions(fg, size);
        if (m_credits) drawCredits(fg, size);
        drawToast(fg, size, dt);
        drawLaunchFade(fg, size, dt);
        drawPicker();
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
    static GameId gameOf(Item i) {
        switch (i) {
        case Item::Sly1: return GameId::Sly1;
        case Item::Sly2: return GameId::Sly2;
        case Item::Sly3: return GameId::Sly3;
        case Item::Sly4: return GameId::Sly4;
        default: return GameId::Hackpack;
        }
    }
    static bool isGame(Item i) { return i != Item::Collection && i != Item::Extras; }
    State state(Item i) const {
        if (!isGame(i)) return State::Page;
        const GameId g = gameOf(i);
        if (!installable(g)) return State::Unavailable;
        const Status& st = m_status[size_t(g)];
        if (!st.assets) return State::NotInstalled;
        if (g == GameId::Sly3 || !st.runtime) return State::InDevelopment;
        return State::Playable;
    }
    int installedCount() const {
        int n = 0;
        for (int g = 0; g < 3; ++g) n += m_status[size_t(g)].assets;
        return n;
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
        if (!m_backend.config.contains("collection") || !m_backend.config.at("collection").is_object())
            m_backend.config["collection"] = json::object();
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

    // ---------------------------------------------------------------- assets
    fs::path ps3Profile() const { return m_fixture.active ? m_fixture.ps3Profile : m_backend.locations.user; }
    fs::path sly2Installation() const {
        if (m_fixture.active) return m_fixture.sly2Installation;
        return m_status[size_t(GameId::Sly2)].assets ? m_backend.dataDir(GameId::Sly2) : fs::path{};
    }
    void loadAssets() {
        m_images = {};
        m_ps3 = !ps3Profile().empty() && ps3Imported(ps3Profile());
        if (m_ps3) m_images = loadPs3Images(ps3Profile());
        const fs::path s2 = sly2Installation();
        if (!s2.empty()) {
            const fs::path hd = m_fixture.active ? fs::path{} : m_backend.ps3HdDir(GameId::Sly2) / "Textures";
            m_images.skies = loadSkies(s2 / "Textures", hd);
        }
        for (int i = 0; i < 4; ++i) m_logo[i] = Tex(m_images.logos[i]);
        m_background = Tex(m_images.background);
        m_loading = Tex(m_images.loading);
        for (int i = 0; i < 3; ++i) m_banner[i] = Tex(m_images.banner[i]);
        for (int i = 0; i < 9; ++i) m_frame[i] = Tex(m_images.frame[i]);
        m_skies.clear();
        for (const auto& sky : m_images.skies) m_skies.emplace_back(sky);
        m_images = {};  // (the pixels live in the textures now)
    }
    void startSoundLoader() {
        if (m_loader.joinable()) m_loader.join();
        m_soundsReady = false;
        const fs::path ps3 = m_ps3 ? ps3Profile() : fs::path{};
        const fs::path s2 = sly2Installation();
        m_loader = std::thread([this, ps3, s2] {
            auto sounds = std::make_shared<MenuSounds>();
            try {
                if (!ps3.empty()) loadPs3Sounds(ps3, *sounds);
                if (!s2.empty()) loadSly2Music(s2, *sounds, !sounds->theme);
            } catch (...) {
            }
            synthesizeSounds(*sounds);
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
    // Which track plays: the collection's theme (or calmer music with nothing installed and in the options), the chosen
    // game's own tracks after a moment on it, varied by the time spent.
    void updateMusic() {
        if (!m_sounds || !m_audio.available()) return;
        PcmPtr want = m_sounds->theme;
        const bool calm = installedCount() == 0 || m_options || m_credits;
        if (calm && !m_sounds->quiet.empty()) want = m_sounds->quiet[size_t(installedCount()) % m_sounds->quiet.size()];
        else if (isGame(Item(m_item)) && m_time - m_itemSince > 2.5 && state(Item(m_item)) != State::NotInstalled &&
                 state(Item(m_item)) != State::Unavailable) {
            auto it = m_sounds->games.find(int(gameOf(Item(m_item))));
            if (it != m_sounds->games.end() && !it->second.empty()) want = it->second[size_t(m_visits[size_t(m_item)]) % it->second.size()];
        }
        if (!want && !m_sounds->quiet.empty()) want = m_sounds->quiet[0];
        if (want != m_audio.music()) m_audio.playMusic(want, 1.5f);
    }
    void sound(const PcmPtr& p) {
        if (m_sounds) m_audio.playSound(p);
    }

    // ---------------------------------------------------------------- input
    static bool pressed(ImGuiKey a, ImGuiKey b = ImGuiKey_None, ImGuiKey c = ImGuiKey_None) {
        return ImGui::IsKeyPressed(a, true) || (b != ImGuiKey_None && ImGui::IsKeyPressed(b, true)) ||
               (c != ImGuiKey_None && ImGui::IsKeyPressed(c, true));
    }
    void select(int item) {
        item = std::clamp(item, 0, kItems - 1);
        if (item == m_item) return;
        m_arrowKick[item > m_item ? 1 : 0] = 1.0f;
        m_shownItem = m_item;
        m_item = item;
        m_fade = 0;
        m_itemSince = m_time;
        ++m_visits[size_t(item)];
        m_extra = 0;
        sound(m_sounds ? m_sounds->move : nullptr);
        m_backend.config["collection_item"] = m_item;
    }
    void input(const launcher::PadEdges& pad, ImVec2 size) {
        if (m_launching >= 0 || gameRunning() || m_returnFade > 0.5f || ImGuiFileDialog::Instance()->IsOpened()) return;
        const bool left = pressed(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_A);
        const bool right = pressed(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_D);
        const bool up = pressed(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_W);
        const bool down = pressed(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_S);
        const bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
        const bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        const bool options = pad.start || pad.y || ImGui::IsKeyPressed(ImGuiKey_Tab, false) || ImGui::IsKeyPressed(ImGuiKey_O, false);
        if (m_credits) {
            if (back || confirm) m_credits = false, sound(m_sounds ? m_sounds->back : nullptr);
            m_creditsScroll += ImGui::GetIO().MouseWheel * -40.0f * m_s + (down ? 30.0f : up ? -30.0f : 0.0f) * m_s;
            return;
        }
        if (m_options) {
            optionsInput(up, down, left, right, confirm, back || options, size);
            return;
        }
        if (options) {
            openOptions();
            return;
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (Item(m_item) == Item::Extras) {
            // the 2 x 2 circles; Left from the left column goes back to the games
            if (left && m_extra % 2 == 0) select(m_item - 1);
            else if (left) m_extra -= 1, sound(m_sounds ? m_sounds->move : nullptr);
            else if (right && m_extra % 2 == 0) m_extra += 1, sound(m_sounds ? m_sounds->move : nullptr);
            else if ((up || down) && (m_extra / 2 == (up ? 1 : 0))) m_extra += up ? -2 : 2, sound(m_sounds ? m_sounds->move : nullptr);
            if (confirm) activateExtra(m_extra);
            if (back) select(m_item - 1);
        } else {
            if (left) select(m_item - 1);
            if (right) select(m_item + 1);
            if (confirm) activate();
            if (back) {
                if (m_item != 0) select(0);
                else m_quit = true;
            }
        }
        if (wheel > 0.5f) select(m_item - 1);
        if (wheel < -0.5f) select(m_item + 1);
        // mouse: the arrows, the logo, the circles
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            if (m_hitLeft.Contains(m)) select(m_item - 1);
            else if (m_hitRight.Contains(m)) select(m_item + 1);
            else if (m_hitLogo.Contains(m) && Item(m_item) != Item::Extras) activate();
            else
                for (int i = 0; i < 4; ++i)
                    if (m_hitExtra[size_t(i)].Contains(m)) m_extra = i, activateExtra(i);
        }
        if (Item(m_item) == Item::Extras && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && input::tracker().lastWasMouse())
            for (int i = 0; i < 4; ++i)
                if (m_hitExtra[size_t(i)].Contains(ImGui::GetIO().MousePos)) m_extra = i;
    }
    void activate() {
        const Item i = Item(m_item);
        switch (state(i)) {
        case State::Playable:
        case State::InDevelopment:
            if (!m_status[size_t(gameOf(i))].runtime) {
                sound(m_sounds ? m_sounds->locked : nullptr);
                toast(trf("%s is still in development: this build cannot play it yet.", game(gameOf(i)).shortName));
                return;
            }
            if (m_status[size_t(gameOf(i))].outdated) {
                sound(m_sounds ? m_sounds->locked : nullptr);
                toast(tr("An update changed the game files' format: open the SafeHouse launcher and re-extract your disc."));
                return;
            }
            sound(m_sounds ? m_sounds->select : nullptr);
            m_launching = m_item;
            m_launchFade = 0;
            break;
        case State::NotInstalled:
            sound(m_sounds ? m_sounds->locked : nullptr);
            toast(trf("Install %s in the SafeHouse launcher to unlock it (Extras > SafeHouse launcher).", game(gameOf(i)).shortName));
            break;
        case State::Unavailable:
            sound(m_sounds ? m_sounds->locked : nullptr);
            toast(trf("A native port of %s is in development. There is nothing to install yet.", game(gameOf(i)).title));
            break;
        case State::Page:
            if (i == Item::Collection) select(1);
            break;
        }
    }
    void activateExtra(int i) {
        sound(m_sounds ? m_sounds->select : nullptr);
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
        const GameId g = gameOf(Item(m_launching));
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
            if (m_importing) {
                m_importing = false;
                if (message == "Completed") {
                    toast(tr("The Sly Collection's menu files are imported."));
                    reloadAssets();
                }
            }
            m_playing = -1;
            m_launching = -1;
            m_launchFade = 0;
        }
        m_wasBusy = busy;
    }
    void reloadAssets() {
        loadAssets();
        if (m_audio.available()) {
            m_audio.playMusic(nullptr, 0.4f);
            startSoundLoader();
        }
    }

    // ---------------------------------------------------------------- the scene (drawn)
    // The drive: a strip of the installed games' worlds (every world with none installed), scrolling right to left.
    enum Biome { Swamp, Peaks, City, Mesa, Jungle, BiomeCount };
    std::vector<Biome> biomes() const {
        std::vector<Biome> b;
        if (m_status[0].assets) b.insert(b.end(), {Swamp, Peaks});
        if (m_status[1].assets) b.insert(b.end(), {City});
        if (m_status[2].assets) b.insert(b.end(), {Mesa, Jungle});
        if (b.empty()) b = {Swamp, City, Mesa, Peaks, Jungle};
        if (b.size() == 1) b.push_back(b[0]);
        return b;
    }
    void drawScene(ImDrawList* dl, ImVec2 size, float dt) {
        const float s = m_s, W = size.x, H = size.y;
        if (m_launching < 0 && !gameRunning()) m_scroll += dt * 90.0f * s;
        // sky
        dl->AddRectFilledMultiColor({0, 0}, {W, H}, draw::rgba(0x07080F), draw::rgba(0x07080F), draw::rgba(0x141A3A), draw::rgba(0x141A3A));
        // a Sly 2 sky panorama, cycled (crossfade every 24 s)
        if (!m_skies.empty()) {
            const double period = 24.0;
            const int a = int(m_time / period) % int(m_skies.size()), b = (a + 1) % int(m_skies.size());
            const float t = std::clamp(float(std::fmod(m_time, period) - (period - 3.0)) / 3.0f, 0.0f, 1.0f);
            auto sky = [&](const Tex& tex, float alpha) {
                if (alpha <= 0) return;
                const float h = H * 0.62f, w = h * tex.aspect();
                const float off = std::fmod(m_scroll * 0.05f, w);
                for (float x = -off; x < W; x += w)
                    dl->AddImage(tex.im(), {x, H * 0.08f}, {x + w + 1, H * 0.08f + h}, {0, 0}, {1, 1}, draw::rgba(0xFFFFFF, alpha * 0.85f));
            };
            sky(m_skies[size_t(a)], 1.0f - t);
            sky(m_skies[size_t(b)], t);
            dl->AddRectFilledMultiColor({0, 0}, {W, H * 0.3f}, draw::rgba(0x07080F, 0.9f), draw::rgba(0x07080F, 0.9f), draw::rgba(0x07080F, 0),
                                        draw::rgba(0x07080F, 0));
        }
        // stars and the moon
        for (int i = 0; i < 90; ++i) {
            const float x = std::fmod(draw::hash(i) * W * 1.3f - m_scroll * 0.01f + W * 2, W), y = draw::hash(i + 500) * H * 0.55f;
            const float tw = 0.5f + 0.5f * std::sin(float(m_time) * (1.0f + draw::hash(i + 77) * 2) + float(i));
            dl->AddCircleFilled({x, y}, std::max(1.0f, s * (0.8f + draw::hash(i + 9) * 1.2f)), draw::rgba(0xFFFFFF, 0.35f + 0.5f * tw), 6);
        }
        const ImVec2 moon{W * 0.83f, H * 0.16f};
        dl->AddCircleFilled(moon, 60 * s, draw::rgba(0xFFF4D6, 0.06f), 48);
        dl->AddCircleFilled(moon, 38 * s, draw::rgba(0xFFF4D6, 0.12f), 48);
        dl->AddCircleFilled(moon, 26 * s, draw::rgba(0xFFF7E0), 48);
        dl->AddCircleFilled({moon.x + 9 * s, moon.y - 5 * s}, 22 * s, draw::rgba(0x0B0D1C, 0.25f), 48);
        // the worlds: far, middle, then the road and the near silhouettes
        const auto strip = biomes();
        const float segment = 1.4f * std::max(W, H * 16.0f / 9.0f);
        drawLayer(dl, size, strip, segment, 0.18f, 0);
        drawLayer(dl, size, strip, segment, 0.45f, 1);
        drawRoad(dl, size);
        drawVan(dl, size);
        drawLayer(dl, size, strip, segment, 1.35f, 2);
        // the return from a game
        if (m_returnFade > 0) {
            dl->AddRectFilled({0, 0}, size, draw::rgba(0x000000, m_returnFade));
            m_returnFade = std::max(0.0f, m_returnFade - dt * 1.4f);
        }
    }
    // One parallax layer: each segment of the strip draws its world's shapes for that depth.
    void drawLayer(ImDrawList* dl, ImVec2 size, const std::vector<Biome>& strip, float segment, float parallax, int depth) {
        const float x0 = m_scroll * parallax;
        const int first = int(std::floor(x0 / segment));
        for (int k = first; k * segment - x0 < size.x; ++k) {
            const Biome b = strip[size_t(((k % int(strip.size())) + int(strip.size())) % int(strip.size()))];
            const float left = k * segment - x0;
            drawBiome(dl, size, b, left, segment, depth, k * 7 + depth * 1000);
        }
    }
    // Hills as a filled band of convex quads under a height function.
    template <class F>
    static void band(ImDrawList* dl, float left, float right, float base, float step, ImU32 color, F height) {
        // (no anti-aliasing: the quads' shared edges would show as seams)
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        struct Restore {
            ImDrawList* dl;
            ImDrawListFlags flags;
            ~Restore() { dl->Flags = flags; }
        } restore{dl, flags};
        for (float x = left; x < right; x += step) {
            const float x2 = std::min(right, x + step);
            const ImVec2 q[4] = {{x, base}, {x, height(x)}, {x2, height(x2)}, {x2, base}};
            dl->AddConvexPolyFilled(q, 4, color);
        }
    }
    void drawBiome(ImDrawList* dl, ImVec2 size, Biome b, float left, float width, int depth, int seed) const {
        const float s = m_s, H = size.y, right = left + width;
        const float ground = H * 0.80f;
        auto r = [&](int i) { return draw::hash(seed * 131 + i); };
        if (depth == 0) {
            // far: mountains / mesas / a distant skyline
            const uint32_t farColor = b == Swamp ? 0x3B2A78 : b == Peaks ? 0x2E3F7A : b == City ? 0x1C2A4E : b == Mesa ? 0x6E2A35 : 0x1F4A3A;
            const uint32_t cap = b == Swamp || b == Peaks ? 0x6FD6F0 : b == Mesa ? 0xB8573A : 0x2E4470;
            if (b == City) {
                for (int i = 0; i < 26; ++i) {
                    const float x = left + width * float(i) / 26.0f, w = width / 26.0f * (0.7f + r(i) * 0.5f), h = H * (0.12f + r(i + 40) * 0.2f);
                    dl->AddRectFilled({x, ground - h}, {x + w, ground}, draw::rgba(farColor));
                    if (r(i + 90) > 0.6f) dl->AddTriangleFilled({x, ground - h}, {x + w * 0.5f, ground - h - H * 0.06f}, {x + w, ground - h}, draw::rgba(farColor));
                }
                return;
            }
            const float amp = b == Mesa ? 0.14f : 0.26f;
            auto height = [&](float x) {
                const float u = (x - left) / width;
                float h = 0;
                for (int o = 1; o <= 3; ++o) h += std::sin(u * 6.2831853f * float(o * 2 + 1) + r(o) * 6.0f) / float(o);
                if (b == Mesa) h = std::round(h * 1.5f) / 1.5f;  // flat tops
                return ground - H * (0.16f + amp * (0.5f + 0.35f * h));
            };
            // a paler range behind, for depth
            band(dl, left, right + 1, ground, std::max(6.0f, 10 * s), draw::rgba(draw::mix(farColor, 0x141A3A, 0.55f)), [&](float x) {
                const float u = (x - left) / width;
                float h = 0;
                for (int o = 1; o <= 2; ++o) h += std::sin(u * 6.2831853f * float(o * 3 + 2) + r(o + 20) * 6.0f) / float(o);
                return ground - H * (0.30f + 0.10f * h);
            });
            band(dl, left, right + 1, ground, std::max(6.0f, 10 * s), draw::rgba(farColor), height);
            // snow caps / lit rims on the highest points
            for (int i = 0; i < 6; ++i) {
                const float x = left + width * (0.08f + 0.16f * float(i)) + r(i + 7) * width * 0.05f;
                const float y = height(x);
                if (y < ground - H * 0.3f || b == Mesa)
                    dl->AddTriangleFilled({x - 26 * s, y + 22 * s}, {x, y}, {x + 26 * s, y + 22 * s}, draw::rgba(cap, b == Mesa ? 0.5f : 0.9f));
            }
            return;
        }
        if (depth == 1) {
            // middle: the worlds' own landmarks
            if (b == City || b == Mesa) {
                const uint32_t wall = b == City ? 0x232B52 : 0x7A2430, roof = b == City ? 0x161C38 : 0x4A1420, lit = b == City ? 0xE8E2B0 : 0xFFC86A;
                for (int i = 0; i < 9; ++i) {
                    const float x = left + width * float(i) / 9.0f + r(i) * 30 * s, w = width / 9.0f * (0.55f + r(i + 3) * 0.35f);
                    const float h = H * (0.22f + r(i + 11) * 0.28f);
                    dl->AddRectFilled({x, ground - h}, {x + w, ground}, draw::rgba(wall));
                    dl->AddRectFilled({x - 4 * s, ground - h - 6 * s}, {x + w + 4 * s, ground - h}, draw::rgba(roof));
                    if (b == City && r(i + 20) > 0.55f) {  // a spire
                        dl->AddTriangleFilled({x + w * 0.3f, ground - h - 6 * s}, {x + w * 0.5f, ground - h - H * 0.12f}, {x + w * 0.7f, ground - h - 6 * s},
                                              draw::rgba(roof));
                    }
                    const int cols = std::max(2, int(w / (18 * s))), rows = std::max(2, int(h / (26 * s)));
                    for (int cx = 0; cx < cols; ++cx)
                        for (int cy = 0; cy < rows - 1; ++cy)
                            if (r(i * 97 + cx * 13 + cy) > 0.45f) {
                                const float wx = x + 6 * s + float(cx) * (w - 12 * s) / float(cols), wy = ground - h + 12 * s + float(cy) * 26 * s;
                                dl->AddRectFilled({wx, wy}, {wx + 6 * s, wy + 12 * s}, draw::rgba(lit, 0.85f));
                            }
                }
                if (b == Mesa) {  // the casino sign
                    const ImVec2 c{left + width * 0.62f, ground - H * 0.5f};
                    dl->AddEllipseFilled(c, 90 * s, 50 * s, draw::rgba(0xB0322E));
                    for (int i = 0; i < 18; ++i) {
                        const float a = float(i) / 18.0f * 6.2831853f;
                        const bool on = (int(m_time * 4) + i) % 3 != 0;
                        dl->AddCircleFilled({c.x + std::cos(a) * 92 * s, c.y + std::sin(a) * 52 * s}, 6 * s, draw::rgba(on ? 0xFFD35A : 0x7A5A20));
                    }
                }
            } else {
                // Sly 1's swamp and peaks, Sly 3's jungle: crooked trees with blobby crowns, a haunted house
                const uint32_t trunk = b == Jungle ? 0x3A2A1A : 0x4A2E2A, crown = b == Swamp ? 0x2FB6C8 : b == Peaks ? 0x9FE6F5 : 0x2F7A3A;
                const uint32_t hill = b == Swamp ? 0x5A2C9A : b == Peaks ? 0x3A4C8A : 0x2A5A30;
                band(dl, left, left + width + 1, ground, std::max(6.0f, 10 * s), draw::rgba(hill), [&](float x) {
                    const float u = (x - left) / width;
                    return ground - H * (0.07f + 0.05f * std::sin(u * 12.0f + r(1) * 6.0f));
                });
                if (b == Swamp) {
                    const float hx = left + width * 0.35f, hy = ground - H * 0.08f;
                    dl->AddRectFilled({hx, hy - 90 * s}, {hx + 130 * s, hy}, draw::rgba(0x4A3A8A));
                    dl->AddTriangleFilled({hx - 14 * s, hy - 88 * s}, {hx + 65 * s, hy - 150 * s}, {hx + 144 * s, hy - 88 * s}, draw::rgba(0x2E2260));
                    for (int i = 0; i < 3; ++i)
                        dl->AddRectFilled({hx + (18 + i * 38) * s, hy - 62 * s}, {hx + (34 + i * 38) * s, hy - 40 * s}, draw::rgba(0xF2A23A));
                }
                for (int i = 0; i < 5; ++i) {
                    const float x = left + width * (0.1f + 0.2f * float(i)) + r(i + 50) * 40 * s;
                    const float h = H * (0.18f + r(i + 60) * 0.12f), lean = (r(i + 70) - 0.5f) * 60 * s;
                    const ImVec2 t[4] = {{x - 8 * s, ground - H * 0.06f}, {x + lean - 4 * s, ground - h}, {x + lean + 6 * s, ground - h}, {x + 10 * s, ground - H * 0.06f}};
                    dl->AddConvexPolyFilled(t, 4, draw::rgba(trunk));
                    for (int j = 0; j < 4; ++j)
                        dl->AddCircleFilled({x + lean + (float(j) - 1.5f) * 26 * s, ground - h - (j % 2) * 12 * s}, (24 + r(i * 5 + j) * 14) * s,
                                            draw::rgba(crown, b == Jungle ? 1.0f : 0.95f), 20);
                }
            }
            return;
        }
        // near: dark silhouettes in front of the road
        const uint32_t dark = 0x15102A;
        for (int i = 0; i < 4; ++i) {
            const float x = left + width * (0.12f + 0.25f * float(i)) + r(i + 300) * 80 * s;
            if (r(i + 310) < 0.45f) continue;
            if (b == City || b == Mesa) {
                dl->AddRectFilled({x, H * 0.80f}, {x + 10 * s, H}, draw::rgba(dark));  // a lamp post
                dl->AddCircleFilled({x + 5 * s, H * 0.80f}, 11 * s, draw::rgba(0xFFE9A0, 0.9f), 16);
            } else {
                for (int j = 0; j < 3; ++j) dl->AddCircleFilled({x + float(j) * 34 * s, H * 0.86f - (j % 2) * 16 * s}, (36 + r(i * 3 + j) * 12) * s, draw::rgba(dark), 20);
            }
        }
    }
    void drawRoad(ImDrawList* dl, ImVec2 size) const {
        const float s = m_s, W = size.x, H = size.y;
        dl->AddRectFilled({0, H * 0.80f}, {W, H * 0.88f}, draw::rgba(0xC89A3A));
        dl->AddRectFilledMultiColor({0, H * 0.80f}, {W, H * 0.83f}, draw::rgba(0xE0B65A), draw::rgba(0xE0B65A), draw::rgba(0xC89A3A), draw::rgba(0xC89A3A));
        const float dash = 90 * s, off = std::fmod(m_scroll, dash * 2);
        for (float x = -off; x < W; x += dash * 2) dl->AddRectFilled({x, H * 0.845f}, {x + dash, H * 0.855f}, draw::rgba(0x8A6420));
        dl->AddRectFilled({0, H * 0.88f}, {W, H}, draw::rgba(0x1A1330));
        // bumps on the near edge
        for (int i = 0; i < 40; ++i) {
            const float x = std::fmod(draw::hash(i + 900) * W * 2 - m_scroll * 1.1f + W * 4, W * 1.2f) - W * 0.1f;
            dl->AddRectFilled({x, H * 0.875f}, {x + (40 + draw::hash(i) * 120) * s, H * 0.9f}, draw::rgba(0x1A1330));
        }
    }
    // The gang's van, bouncing along with its headlights on.
    void drawVan(ImDrawList* dl, ImVec2 size) const {
        const float s = m_s, H = size.y;
        const float bob = std::sin(float(m_time) * 9.0f) * 1.5f * s;
        const ImVec2 o{size.x * 0.47f, H * 0.835f + bob};
        const float w = 96 * s, h = 46 * s;
        // the beam
        const ImVec2 beam[3] = {{o.x + w * 0.5f, o.y - h * 0.45f}, {o.x + w * 0.5f + 260 * s, o.y - h * 0.05f}, {o.x + w * 0.5f + 260 * s, o.y - h * 0.9f}};
        dl->AddConvexPolyFilled(beam, 3, draw::rgba(0xFFF1A8, 0.14f));
        // dust behind
        for (int i = 0; i < 6; ++i) {
            const float t = std::fmod(float(m_time) * 1.4f + float(i) / 6.0f, 1.0f);
            dl->AddCircleFilled({o.x - w * 0.5f - t * 90 * s, o.y - 4 * s - t * 14 * s}, (8 + t * 18) * s, draw::rgba(0xD9B46A, 0.45f * (1 - t)), 16);
        }
        dl->AddRectFilled({o.x - w * 0.5f, o.y - h}, {o.x + w * 0.5f, o.y - 8 * s}, draw::rgba(0x4B3BD6), 12 * s);
        dl->AddRectFilled({o.x + w * 0.12f, o.y - h + 6 * s}, {o.x + w * 0.44f, o.y - h * 0.55f}, draw::rgba(0x9ED8FF), 5 * s);
        dl->AddRectFilled({o.x - w * 0.42f, o.y - h + 6 * s}, {o.x + w * 0.05f, o.y - h * 0.55f}, draw::rgba(0x2C2290), 5 * s);
        // the mask on its side
        const ImVec2 m{o.x - w * 0.18f, o.y - h * 0.32f};
        dl->AddTriangleFilled({m.x - 16 * s, m.y - 6 * s}, {m.x, m.y - 2 * s}, {m.x, m.y + 6 * s}, draw::rgba(0x1A1340));
        dl->AddTriangleFilled({m.x + 16 * s, m.y - 6 * s}, {m.x, m.y + 6 * s}, {m.x, m.y - 2 * s}, draw::rgba(0x1A1340));
        dl->AddRectFilled({o.x + w * 0.44f, o.y - h * 0.5f}, {o.x + w * 0.52f, o.y - h * 0.36f}, draw::rgba(0xFFF6C0), 2 * s);
        for (float wx : {-0.28f, 0.3f}) {
            dl->AddCircleFilled({o.x + w * wx, o.y - 6 * s}, 11 * s, draw::rgba(0x120C20), 20);
            dl->AddCircleFilled({o.x + w * wx, o.y - 6 * s}, 5 * s, draw::rgba(0x8A86A8), 12);
        }
    }
    // Extras: Paris at dusk, as the collection's minigames page.
    void drawDusk(ImDrawList* dl, ImVec2 size) const {
        const float s = m_s, W = size.x, H = size.y;
        dl->AddRectFilledMultiColor({0, 0}, {W, H * 0.7f}, draw::rgba(0x3A2466), draw::rgba(0x3A2466), draw::rgba(0xE07A8A), draw::rgba(0xE07A8A));
        dl->AddRectFilled({0, H * 0.7f}, {W, H}, draw::rgba(0x2A1A4A));
        for (int i = 0; i < 8; ++i) {
            const float y = H * (0.12f + draw::hash(i + 40) * 0.4f), x = std::fmod(draw::hash(i) * W * 1.5f + float(m_time) * 6 * s, W * 1.4f) - W * 0.2f;
            dl->AddEllipseFilled({x, y}, (120 + draw::hash(i + 3) * 160) * s, 18 * s, draw::rgba(0xF2A0B8, 0.25f));
        }
        for (int i = 0; i < 14; ++i) {
            const float x = W * float(i) / 13.0f - 30 * s, w = W / 13.0f + 10 * s, h = H * (0.25f + draw::hash(i + 70) * 0.18f);
            const float top = H * 0.88f - h;
            dl->AddRectFilled({x, top}, {x + w, H}, draw::rgba(i % 2 ? 0x3E3270 : 0x4A3A84));
            dl->AddTriangleFilled({x - 6 * s, top}, {x + w * 0.5f, top - 34 * s}, {x + w + 6 * s, top}, draw::rgba(0x2A2050));
            for (int k = 0; k < 3; ++k)
                if (draw::hash(i * 7 + k) > 0.35f) dl->AddCircleFilled({x + w * (0.25f + 0.25f * float(k)), top + 40 * s}, 9 * s, draw::rgba(0xFFD872, 0.9f), 16);
        }
        dl->AddRectFilled({0, H * 0.9f}, {W, H}, draw::rgba(0x150E26));
        if (m_returnFade > 0) dl->AddRectFilled({0, 0}, size, draw::rgba(0x000000, m_returnFade));
    }

    // ---------------------------------------------------------------- the logo and its caption
    // The logo of an item: the PS3's game icon when imported, else the launcher's own game logos (ui::images()).
    struct Logo {
        ImTextureID tex = nullptr;
        float aspect = 1;
    };
    Logo logoOf(Item i) const {
        const int ps3 = i == Item::Collection ? 0 : i == Item::Sly1 ? 1 : i == Item::Sly2 ? 2 : i == Item::Sly3 ? 3 : -1;
        if (ps3 >= 0 && m_logo[ps3]) return {m_logo[ps3].im(), m_logo[ps3].aspect()};
        if (isGame(i)) {
            const auto& g = ui::images().games[size_t(gameOf(i))];
            return {g.texture, g.aspect};
        }
        if (i == Item::Collection) return {ui::images().logo, ui::images().logoAspect};
        return {};
    }
    void drawLogo(ImDrawList* dl, ImVec2 size, Item i, float alpha, float pop) {
        const float s = m_s, W = size.x, H = size.y;
        const Logo logo = logoOf(i);
        const bool ps3 = logo.tex && (i == Item::Collection || i == Item::Sly1 || i == Item::Sly2 || i == Item::Sly3) &&
                         m_logo[i == Item::Collection ? 0 : int(i)];
        float maxW = std::min(W * 0.62f, 620 * s), maxH = (ps3 ? 300 : 250) * s;
        float lw = maxW, lh = logo.aspect > 0 ? lw / logo.aspect : maxH;
        if (lh > maxH) lh = maxH, lw = lh * logo.aspect;
        lw *= pop, lh *= pop;
        const ImVec2 c{W * 0.5f, H * 0.35f};
        m_logoRect = ImRect({c.x - lw * 0.5f, c.y - lh * 0.5f}, {c.x + lw * 0.5f, c.y + lh * 0.5f});
        const State st = state(i);
        const bool dim = st == State::NotInstalled || st == State::Unavailable;
        if (logo.tex) {
            // a soft shadow, then the logo (dark when locked)
            dl->AddImage(logo.tex, {m_logoRect.Min.x + 6 * s, m_logoRect.Min.y + 8 * s}, {m_logoRect.Max.x + 6 * s, m_logoRect.Max.y + 8 * s}, {0, 0}, {1, 1},
                         draw::rgba(0x000000, 0.45f * alpha));
            dl->AddImage(logo.tex, m_logoRect.Min, m_logoRect.Max, {0, 0}, {1, 1}, draw::rgba(dim ? 0x4A4A66 : 0xFFFFFF, alpha));
        }
        if (i == Item::Collection && !m_logo[0]) {
            // under the SafeHouse logo, the word "Collection" in the PS3 logo's yellow
            const float fs = 46 * s;
            const char* word = "COLLECTION";
            const ImVec2 t = draw::measure(ui::fonts().display, fs, word);
            draw::outlined(dl, ui::fonts().display, fs, {c.x - t.x * 0.5f + 40 * s, m_logoRect.Max.y - fs * 0.55f}, 0xFFC930, word, alpha, 4 * s);
        }
        if (dim) draw::lockBadge(dl, {m_logoRect.Max.x - lh * 0.12f, m_logoRect.Min.y + lh * 0.2f}, 30 * s, alpha);
        if (st == State::InDevelopment || st == State::Unavailable) {
            // a ribbon across the corner
            const char* tag = tr("IN DEVELOPMENT");
            const float fs = 17 * s;
            const ImVec2 t = draw::measure(ui::fonts().tab, fs, tag);
            const ImVec2 p{m_logoRect.Min.x + 14 * s, m_logoRect.Min.y + 6 * s};  // (the top left corner, clear of the arrows)
            dl->AddRectFilled({p.x - 10 * s, p.y - 5 * s}, {p.x + t.x + 10 * s, p.y + t.y + 5 * s}, draw::rgba(0xF4C531, alpha), 6 * s);
            dl->AddRect({p.x - 10 * s, p.y - 5 * s}, {p.x + t.x + 10 * s, p.y + t.y + 5 * s}, draw::rgba(0x1A1030, alpha), 6 * s, 0, 2 * s);
            dl->AddText(ui::fonts().tab, fs, p, draw::rgba(0x1A1405, alpha), tag);
        }
    }
    void drawItem(ImDrawList* dl, ImVec2 size) {
        const float s = m_s, W = size.x, H = size.y;
        const float t = m_fade;
        if (m_shownItem != m_item) drawLogo(dl, size, Item(m_shownItem), 1.0f - t, 1.0f);
        const float pop = 0.92f + 0.08f * (1 - (1 - t) * (1 - t));
        drawLogo(dl, size, Item(m_item), t, pop);
        m_hitLogo = m_logoRect;
        // the arrows beside the logo's lower corners
        const float ay = m_logoRect.Max.y - 12 * s, spread = std::max(m_logoRect.GetWidth() * 0.5f, 170 * s) + 46 * s;
        for (int d = 0; d < 2; ++d) {
            m_arrowKick[size_t(d)] = std::max(0.0f, m_arrowKick[size_t(d)] - ImGui::GetIO().DeltaTime * 5.0f);
            const bool show = d == 0 ? m_item > 0 : m_item < kItems - 1;
            const float bounce = std::sin(float(m_time) * 5.0f) * 4 * s + m_arrowKick[size_t(d)] * 14 * s;
            const ImVec2 c{W * 0.5f + (d ? 1 : -1) * (spread + bounce), ay};
            ImRect& hit = d ? m_hitRight : m_hitLeft;
            hit = ImRect({c.x - 40 * s, c.y - 36 * s}, {c.x + 40 * s, c.y + 36 * s});
            if (!show) {
                hit = {};
                continue;
            }
            const bool hover = hit.Contains(ImGui::GetIO().MousePos) && input::tracker().lastWasMouse();
            draw::arrow(dl, c, 58 * s * (hover ? 1.1f : 1.0f), d ? 1 : -1, 1.0f, hover || m_arrowKick[size_t(d)] > 0.2f);
        }
        // the caption plaque under the logo
        const Item i = Item(m_item);
        std::string title, line;
        uint32_t titleColor = 0xFFD84A;
        switch (state(i)) {
        case State::Playable:
            title = tr("Play");
            line = std::string(game(gameOf(i)).title);
            break;
        case State::InDevelopment:
            title = m_status[size_t(gameOf(i))].runtime ? tr("Play (early development)") : tr("In development");
            line = gameOf(i) == GameId::Sly3 ? brand::sly3Notice : tr("Installed. The port is not ready to play yet.");
            break;
        case State::NotInstalled:
            title = tr("Locked");
            titleColor = 0xC8D4F0;
            line = trf("Install %s in the SafeHouse launcher to unlock it", game(gameOf(i)).shortName);
            break;
        case State::Unavailable:
            title = tr("In development");
            titleColor = 0xC8D4F0;
            line = trf("%s: nothing to install yet", game(gameOf(i)).title);
            break;
        case State::Page: {
            const int n = installedCount();
            title = n == 3 ? tr("The complete collection") : n == 0 ? tr("Welcome to the safehouse") : tr("Choose a game");
            line = n == 0 ? std::string(tr("Install your games in the SafeHouse launcher to unlock them"))
                          : n == 3 ? trf("%d of 3 games installed: every Sly game this build plays", n) : trf("%d of 3 games installed", n);
            break;
        }
        }
        const float tf = 34 * s, lf = 18 * s;
        const ImVec2 ts = draw::measure(ui::fonts().display, tf, title.c_str()), ls = draw::measure(ui::fonts().body, lf, line.c_str());
        const float pw = std::min(W - 40 * s, std::max(ts.x, ls.x) + 80 * s), ph = tf + lf + 40 * s;
        const ImVec2 pc{W * 0.5f, H * 0.655f};
        const ImRect plaque({pc.x - pw * 0.5f, pc.y - ph * 0.5f}, {pc.x + pw * 0.5f, pc.y + ph * 0.5f});
        panel(dl, plaque, t);
        draw::outlined(dl, ui::fonts().display, tf, {pc.x - ts.x * 0.5f, plaque.Min.y + 14 * s}, titleColor, title.c_str(), t, 3 * s);
        dl->AddText(ui::fonts().body, lf, {std::round(pc.x - ls.x * 0.5f), std::round(plaque.Max.y - 18 * s - lf)}, draw::rgba(0xF2F6FF, t), line.c_str());
        // where we are: one dot per item
        const float dy = H * 0.755f, gap = 22 * s;
        for (int k = 0; k < kItems; ++k) {
            const ImVec2 p{W * 0.5f + (float(k) - (kItems - 1) * 0.5f) * gap, dy};
            const bool on = k == m_item;
            dl->AddCircleFilled(p, (on ? 7.0f : 5.0f) * s, draw::rgba(on ? 0xFFD84A : 0x8A93C8, on ? 1.0f : 0.7f), 16);
            dl->AddCircle(p, (on ? 7.0f : 5.0f) * s, draw::rgba(0x1A1030), 16, 1.5f * s);
        }
    }
    // A panel in the collection's blue frame (the PS3's own frame pieces when imported).
    void panel(ImDrawList* dl, const ImRect& r, float alpha) const {
        const float s = m_s;
        if (m_frame[4] && m_frame[0]) {
            const float c = 26 * s;
            const ImVec2 a = r.Min, b = r.Max;
            const float xs[4] = {a.x, a.x + c, b.x - c, b.x}, ys[4] = {a.y, a.y + c, b.y - c, b.y};
            const ImU32 tint = draw::rgba(0xFFFFFF, alpha);
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x)
                    if (m_frame[y * 3 + x]) dl->AddImage(m_frame[y * 3 + x].im(), {xs[x], ys[y]}, {xs[x + 1], ys[y + 1]}, {0, 0}, {1, 1}, tint);
            return;
        }
        dl->AddRectFilled({r.Min.x + 5 * s, r.Min.y + 7 * s}, {r.Max.x + 5 * s, r.Max.y + 7 * s}, draw::rgba(0x0A0616, 0.5f * alpha), 16 * s);
        dl->AddRectFilled(r.Min, r.Max, draw::rgba(0x0C4A86, 0.94f * alpha), 16 * s);
        dl->AddRect(r.Min, r.Max, draw::rgba(0x0A1A3A, alpha), 16 * s, 0, 5 * s);
        dl->AddRect({r.Min.x + 4 * s, r.Min.y + 4 * s}, {r.Max.x - 4 * s, r.Max.y - 4 * s}, draw::rgba(0x3FA0F0, alpha), 12 * s, 0, 3 * s);
        // the curls in the corners
        for (int k = 0; k < 4; ++k) {
            const ImVec2 c{k % 2 ? r.Max.x - 16 * s : r.Min.x + 16 * s, k / 2 ? r.Max.y - 16 * s : r.Min.y + 16 * s};
            dl->PathArcTo(c, 7 * s, 0, 4.7f, 12);
            dl->PathStroke(draw::rgba(0x7FC4FF, 0.8f * alpha), 0, 2 * s);
        }
    }
    // The banner across the top ("Options", "Extras").
    void banner(ImDrawList* dl, ImVec2 size, const char* text, float alpha) const {
        const float s = m_s, W = size.x;
        const float fs = 36 * s;
        text = tr(text);
        const ImVec2 t = draw::measure(ui::fonts().display, fs, text);
        const float w = std::max(t.x + 160 * s, 420 * s), h = 84 * s;
        const ImRect r({W * 0.5f - w * 0.5f, 26 * s}, {W * 0.5f + w * 0.5f, 26 * s + h});
        if (m_banner[0] && m_banner[1] && m_banner[2]) {
            const float cap = h;
            const ImU32 tint = draw::rgba(0xFFFFFF, alpha);
            dl->AddImage(m_banner[0].im(), r.Min, {r.Min.x + cap, r.Max.y}, {0, 0}, {1, 1}, tint);
            dl->AddImage(m_banner[1].im(), {r.Min.x + cap, r.Min.y}, {r.Max.x - cap, r.Max.y}, {0, 0}, {1, 1}, tint);
            dl->AddImage(m_banner[2].im(), {r.Max.x - cap, r.Min.y}, r.Max, {0, 0}, {1, 1}, tint);
        } else {
            panel(dl, ImRect({r.Min.x, r.Min.y + 10 * s}, {r.Max.x, r.Max.y - 10 * s}), alpha);
        }
        draw::outlined(dl, ui::fonts().display, fs, {W * 0.5f - t.x * 0.5f, r.GetCenter().y - t.y * 0.5f}, 0xFFD84A, text, alpha, 3 * s);
    }

    // ---------------------------------------------------------------- Extras
    void drawExtras(ImDrawList* dl, ImVec2 size) {
        const float s = m_s, W = size.x, H = size.y;
        const float t = m_fade;
        banner(dl, size, tr("Extras"), t);
        static const char* labels[4] = {N_("Options"), N_("Credits"), N_("SafeHouse launcher"), N_("Quit")};
        const float r = std::min(92 * s, W * 0.11f), gx = r * 2.5f, gy = r * 2.35f;
        const ImVec2 c0{W * 0.5f - gx * 0.5f + 40 * s, H * 0.36f};
        for (int i = 0; i < 4; ++i) {
            const ImVec2 c{c0.x + float(i % 2) * gx, c0.y + float(i / 2) * gy};
            const bool on = i == m_extra;
            m_hitExtra[size_t(i)] = ImRect({c.x - r, c.y - r}, {c.x + r, c.y + r});
            dl->AddCircleFilled({c.x + 5 * s, c.y + 7 * s}, r, draw::rgba(0x0A0616, 0.5f * t), 48);
            dl->AddCircleFilled(c, r, draw::rgba(on ? 0xF4F4F4 : 0xC8C8D0, t), 48);
            dl->AddCircle(c, r, draw::rgba(on ? 0xFFD84A : 0x3A3A50, t), 48, (on ? 6.0f : 3.0f) * s);
            extraIcon(dl, c, r * 0.55f, i, t);
            const char* label = tr(labels[i]);
            const ImVec2 ts = draw::measure(ui::fonts().display, 24 * s, label);
            draw::outlined(dl, ui::fonts().display, 24 * s, {c.x - ts.x * 0.5f, c.y + r + 6 * s}, on ? 0xFFD84A : 0xF2F6FF, label, t, 2.5f * s);
        }
        // the logo at the left, as the minigames page shows its own
        const Logo logo = logoOf(Item::Collection);
        const float lh0 = 120 * s;
        if (logo.tex && c0.x - r - lh0 * logo.aspect - 50 * s >= 16 * s) {
            const float lh = lh0, lw = lh * logo.aspect;
            const ImVec2 p{c0.x - r - lw - 50 * s, c0.y - lh * 0.4f};
            dl->AddImage(logo.tex, p, {p.x + lw, p.y + lh}, {0, 0}, {1, 1}, draw::rgba(0xFFFFFF, t));
        }
        // the arrow back to the games
        const ImVec2 ac{c0.x - r - 70 * s, H * 0.70f};
        m_hitLeft = ImRect({ac.x - 40 * s, ac.y - 36 * s}, {ac.x + 40 * s, ac.y + 36 * s});
        m_hitRight = {};
        m_hitLogo = {};
        const float bounce = std::sin(float(m_time) * 5.0f) * 4 * s;
        draw::arrow(dl, {ac.x - bounce, ac.y}, 58 * s, -1, t, m_hitLeft.Contains(ImGui::GetIO().MousePos));
    }
    // The circles' silhouettes: a gear, a scroll, the SafeHouse mask, a door.
    void extraIcon(ImDrawList* dl, ImVec2 c, float r, int i, float a) const {
        const ImU32 k = draw::rgba(0x15102A, a);
        if (i == 0) {
            for (int t = 0; t < 8; ++t) {
                const float ang = float(t) / 8.0f * 6.2831853f;
                dl->AddRectFilled({c.x + std::cos(ang) * r * 0.78f - r * 0.14f, c.y + std::sin(ang) * r * 0.78f - r * 0.14f},
                                  {c.x + std::cos(ang) * r * 0.78f + r * 0.14f, c.y + std::sin(ang) * r * 0.78f + r * 0.14f}, k, r * 0.04f);
            }
            dl->AddCircleFilled(c, r * 0.7f, k, 32);
            dl->AddCircleFilled(c, r * 0.28f, draw::rgba(0xE8E8EE, a), 24);
        } else if (i == 1) {
            dl->AddRectFilled({c.x - r * 0.55f, c.y - r * 0.75f}, {c.x + r * 0.55f, c.y + r * 0.75f}, k, r * 0.12f);
            for (int l = 0; l < 4; ++l)
                dl->AddRectFilled({c.x - r * 0.38f, c.y - r * 0.5f + float(l) * r * 0.3f}, {c.x + r * (l == 3 ? 0.1f : 0.38f), c.y - r * 0.4f + float(l) * r * 0.3f},
                                  draw::rgba(0xE8E8EE, a));
        } else if (i == 2) {
            if (ui::images().logo) {
                const float h = r * 1.5f, w = h * ui::images().logoAspect;
                dl->AddImage(ui::images().logo, {c.x - w * 0.5f, c.y - h * 0.5f}, {c.x + w * 0.5f, c.y + h * 0.5f}, {0, 0}, {1, 1}, draw::rgba(0xFFFFFF, a));
            }
        } else {
            dl->AddRectFilled({c.x - r * 0.45f, c.y - r * 0.8f}, {c.x + r * 0.3f, c.y + r * 0.8f}, k, r * 0.06f);
            dl->AddCircleFilled({c.x + r * 0.12f, c.y + r * 0.05f}, r * 0.08f, draw::rgba(0xE8E8EE, a), 10);
            const ImVec2 arrowPts[3] = {{c.x + r * 0.45f, c.y - r * 0.25f}, {c.x + r * 0.85f, c.y}, {c.x + r * 0.45f, c.y + r * 0.25f}};
            dl->AddConvexPolyFilled(arrowPts, 3, k);
        }
    }

    // ---------------------------------------------------------------- footer, toasts, launch
    void drawFooter(ImDrawList* dl, ImVec2 size, input::Family family) const {
        const float s = m_s, W = size.x, H = size.y;
        using input::Button;
        std::vector<launcher::Hint> hints;
        if (m_credits) hints = {{ui::backButton(), GLFW_KEY_ESCAPE, N_("Back")}};
        else if (m_options)
            hints = {{Button::Dpad, 0, N_("Choose")}, {Button::DpadLeft, GLFW_KEY_LEFT, N_("Change")}, {ui::backButton(), GLFW_KEY_ESCAPE, N_("Back")}};
        else {
            hints = {{Button::Dpad, GLFW_KEY_RIGHT, N_("Choose")}, {Button::Start, GLFW_KEY_TAB, N_("Options")}, {ui::backButton(), GLFW_KEY_ESCAPE, m_item ? N_("Back") : N_("Quit")}};
            const State st = state(Item(m_item));
            if (st == State::Playable || (st == State::InDevelopment && m_status[size_t(gameOf(Item(m_item)))].runtime))
                hints.push_back({ui::confirmButton(), GLFW_KEY_ENTER, N_("Play")});
            else if (Item(m_item) == Item::Extras) hints.push_back({ui::confirmButton(), GLFW_KEY_ENTER, N_("Select")});
        }
        dl->AddRectFilledMultiColor({0, H - 64 * s}, {W, H}, draw::rgba(0x000000, 0), draw::rgba(0x000000, 0), draw::rgba(0x000000, 0.55f),
                                    draw::rgba(0x000000, 0.55f));
        ImGui::PushFont(ui::fonts().tab);
        launcher::drawPromptHints(dl, {W - 28 * s, H - 30 * s}, std::round(28 * s), hints, draw::rgba(0xF2F6FF), draw::rgba(0xA8B9D6), family);
        ImGui::PopFont();
        // the brand, small, bottom left
        dl->AddText(ui::fonts().caption, 15 * s, {24 * s, H - 38 * s}, draw::rgba(0xA8B9D6, 0.8f),
                    m_ps3 ? tr("SafeHouse · menu art and music from your Sly Collection") : tr("SafeHouse · Collection Style Launcher"));
    }
    void toast(std::string text) {
        m_toast = tr(text);
        m_toastTime = 5.0f;
    }
    void drawToast(ImDrawList* dl, ImVec2 size, float dt) {
        if (m_toastTime <= 0) return;
        m_toastTime -= dt;
        const float s = m_s, a = std::min(1.0f, m_toastTime * 2);
        const float wrap = std::min(size.x - 80 * s, 760 * s);
        const ImVec2 t = ui::fonts().body->CalcTextSizeA(19 * s, FLT_MAX, wrap, m_toast.c_str());
        const ImRect r({size.x * 0.5f - t.x * 0.5f - 34 * s, 22 * s}, {size.x * 0.5f + t.x * 0.5f + 34 * s, 22 * s + t.y + 36 * s});
        panel(dl, r, a);
        dl->AddText(ui::fonts().body, 19 * s, {r.Min.x + 34 * s, r.Min.y + 18 * s}, draw::rgba(0xF2F6FF, a), m_toast.c_str(), nullptr, wrap);
    }
    void drawLaunchFade(ImDrawList* dl, ImVec2 size, float dt) {
        if (m_launching < 0) return;
        if (!gameRunning()) {
            const float before = m_launchFade;
            m_launchFade = std::min(1.0f, m_launchFade + dt * 1.8f);
            if (before < 1.0f && m_launchFade >= 1.0f && m_fixture.screen != "launch") launchNow();
        }
        const float a = m_launchFade, s = m_s;
        dl->AddRectFilled({0, 0}, size, draw::rgba(0x000000, a));
        // the mask, as the collection's loading screen
        if (m_loading) {
            const float h = size.y, w = h * m_loading.aspect();
            dl->AddImage(m_loading.im(), {size.x * 0.5f - w * 0.5f, 0}, {size.x * 0.5f + w * 0.5f, h}, {0, 0}, {1, 1}, draw::rgba(0xFFFFFF, a));
        } else if (ui::images().logo) {
            const float h = 180 * s, w = h * ui::images().logoAspect;
            dl->AddImage(ui::images().logo, {size.x * 0.5f - w * 0.5f, size.y * 0.5f - h * 0.6f}, {size.x * 0.5f + w * 0.5f, size.y * 0.5f + h * 0.4f}, {0, 0},
                         {1, 1}, draw::rgba(0xFFFFFF, a));
        }
        // the spinner
        const ImVec2 c{size.x * 0.5f, size.y * 0.5f + 100 * s};
        const float start = float(m_time) * 5.0f;
        dl->PathArcTo(c, 14 * s, start, start + 4.6f, 24);
        dl->PathStroke(draw::rgba(0x6FB8F0, a), 0, 4 * s);
    }

    // ---------------------------------------------------------------- options
    struct Row {
        std::string section;  // a header row when label is empty
        std::string label;
        std::function<std::string()> value;          // empty: an action row
        std::function<void(int)> change;             // -1 / +1
        std::function<void()> action;
        std::string help;
    };
    void openOptions() {
        sound(m_sounds ? m_sounds->select : nullptr);
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
                              sound(m_sounds ? m_sounds->move : nullptr);
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
        if (m_ps3) {
            m_rows.push_back({"", N_("Remove the Sly Collection menu files"), {}, {}, [this] {
                                  if (m_fixture.active) return;
                                  removePs3Menu(ps3Profile());
                                  reloadAssets();
                                  buildRows();
                                  toast(N_("The menu files are removed. The drawn scene and the launcher's logos are back."));
                              },
                              N_("The menu uses its own drawn scene and logos again.")});
        } else {
            std::string known;
            for (GameId g : {GameId::Sly2, GameId::Sly1}) {
                if (m_fixture.active) break;
                const auto pack = m_backend.ps3HdPack(g);
                if (!pack.source.empty() && known.empty()) known = pack.source;
            }
            if (!known.empty())
                m_rows.push_back({"", N_("Use the Sly Collection's menu art and music"), {}, {}, [this, known] { startImport(known); },
                                  trf("From %s (your PS3 HD texture import's source).", known.c_str())});
            m_rows.push_back({"", N_("Import the Sly Collection's menu art and music…"), {}, {}, [this] { pickPs3(false); },
                              N_("From your The Sly Collection (PS3) disc image (.iso, or a .zip of it). Copies about 120 MB into your profile.")});
            m_rows.push_back({"", N_("Import from the extracted disc…"), {}, {}, [this] { pickPs3(true); },
                              N_("The folder with PS3_GAME in it.")});
        }
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
    void optionsInput(bool up, bool down, bool left, bool right, bool confirm, bool back, ImVec2) {
        if (back) {
            m_options = false;
            sound(m_sounds ? m_sounds->back : nullptr);
            return;
        }
        auto step = [&](int d) {
            for (int n = 0; n < int(m_rows.size()); ++n) {
                m_row = (m_row + d + int(m_rows.size())) % int(m_rows.size());
                if (!m_rows[size_t(m_row)].label.empty()) break;
            }
            m_ensureVisible = true;
            sound(m_sounds ? m_sounds->move : nullptr);
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
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            for (size_t i = 0; i < m_rowRects.size(); ++i)
                if (m_rowRects[i].Contains(ImGui::GetIO().MousePos) && !m_rows[i].label.empty()) {
                    m_row = int(i);
                    Row& c = m_rows[i];
                    if (c.action) c.action();
                    else if (c.change) c.change(ImGui::GetIO().MousePos.x < m_rowRects[i].GetCenter().x + m_rowRects[i].GetWidth() * 0.15f ? -1 : 1);
                }
        if (input::tracker().lastWasMouse())
            for (size_t i = 0; i < m_rowRects.size(); ++i)
                if (m_rowRects[i].Contains(ImGui::GetIO().MousePos) && !m_rows[i].label.empty()) m_row = int(i);
    }
    void drawOptions(ImDrawList* dl, ImVec2 size) {
        const float s = m_s, W = size.x, H = size.y;
        // the backdrop: the disc's XMB picture when imported, else the scene darkened
        if (m_background) {
            const float a = m_background.aspect();
            float w = W, h = W / a;
            if (h < H) h = H, w = H * a;
            dl->AddImage(m_background.im(), {W * 0.5f - w * 0.5f, H * 0.5f - h * 0.5f}, {W * 0.5f + w * 0.5f, H * 0.5f + h * 0.5f});
            dl->AddRectFilled({0, 0}, size, draw::rgba(0x05030C, 0.55f));
        } else {
            dl->AddRectFilled({0, 0}, size, draw::rgba(0x05030C, 0.72f));
        }
        banner(dl, size, tr("Options"), 1.0f);
        const float pw = std::min(W - 48 * s, 900 * s), top = 126 * s, bottom = H - 120 * s;
        const ImRect pr({W * 0.5f - pw * 0.5f, top}, {W * 0.5f + pw * 0.5f, bottom});
        panel(dl, pr, 1.0f);
        const float rowH = 44 * s, headH = 46 * s;
        // scroll the chosen row into view
        float y = 0, chosenY = 0, chosenH = rowH;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const float h = m_rows[i].label.empty() ? headH : rowH;
            if (int(i) == m_row) chosenY = y, chosenH = h;
            y += h;
        }
        const float viewH = pr.GetHeight() - 40 * s, total = y;
        if (m_ensureVisible) {
            if (chosenY - m_optionsScroll < 0) m_optionsScroll = chosenY - headH;
            if (chosenY + chosenH - m_optionsScroll > viewH) m_optionsScroll = chosenY + chosenH - viewH;
            m_ensureVisible = false;
        }
        m_optionsScroll = std::clamp(m_optionsScroll, 0.0f, std::max(0.0f, total - viewH));
        dl->PushClipRect({pr.Min.x + 10 * s, pr.Min.y + 20 * s}, {pr.Max.x - 10 * s, pr.Max.y - 20 * s}, true);
        m_rowRects.assign(m_rows.size(), {});
        y = pr.Min.y + 20 * s - m_optionsScroll;
        const float lx = pr.Min.x + 44 * s, rx = pr.Max.x - 44 * s;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row& r = m_rows[i];
            if (r.label.empty()) {
                draw::outlined(dl, ui::fonts().display, 26 * s, {lx - 14 * s, y + 8 * s}, 0xFFD84A, tr(r.section.c_str()), 1.0f, 2.5f * s);
                y += headH;
                continue;
            }
            const ImRect rr({pr.Min.x + 24 * s, y}, {pr.Max.x - 24 * s, y + rowH - 4 * s});
            m_rowRects[i] = rr;
            const bool on = int(i) == m_row;
            if (on) {
                dl->AddRectFilled(rr.Min, rr.Max, draw::rgba(0xFFD84A, 0.22f), 10 * s);
                dl->AddRect(rr.Min, rr.Max, draw::rgba(0xFFD84A, 0.9f), 10 * s, 0, 2.5f * s);
            }
            const float ty = rr.GetCenter().y - 11 * s;
            dl->AddText(ui::fonts().tab, 21 * s, {lx, ty}, draw::rgba(on ? 0xFFFFFF : 0xDCE6FA), tr(r.label.c_str()));
            if (r.value) {
                const std::string v = tr(r.value());
                const ImVec2 vs = draw::measure(ui::fonts().tab, 21 * s, v.c_str());
                dl->AddText(ui::fonts().tab, 21 * s, {rx - vs.x - 24 * s, ty}, draw::rgba(on ? 0xFFD84A : 0xF2F6FF), v.c_str());
                if (on) {
                    draw::arrow(dl, {rx - vs.x - 46 * s, rr.GetCenter().y}, 18 * s, -1, 1.0f, false);
                    draw::arrow(dl, {rx - 6 * s, rr.GetCenter().y}, 18 * s, 1, 1.0f, false);
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
            dl->AddText(ui::fonts().body, 19 * s, {W * 0.5f - std::min(hs.x, wrap) * 0.5f, bottom + 10 * s}, draw::rgba(0xF2F6FF), help.c_str(), nullptr, wrap);
        }
        if (m_importing) {
            float p = 0;
            {
                std::lock_guard lock(m_job.mutex);
                p = m_job.progress;
            }
            const ImRect bar({pr.Min.x + 40 * s, bottom - 34 * s}, {pr.Max.x - 40 * s, bottom - 22 * s});
            dl->AddRectFilled(bar.Min, bar.Max, draw::rgba(0x0A1A3A), 6 * s);
            dl->AddRectFilled(bar.Min, {bar.Min.x + bar.GetWidth() * p, bar.Max.y}, draw::rgba(0xFFD84A), 6 * s);
        }
    }
    void pickPs3(bool folder) {
        if (m_fixture.active || m_job.busy) return;
        m_pickFolder = folder;
        const char* title = folder ? tr("Choose the extracted Sly Collection (PS3)") : tr("Choose The Sly Collection (PS3) disc image");
        const fs::path start = filedialog::startFolder({m_backend.config.value("ps3_browse_dir", std::string{})});
        if (!m_dialog.start(folder ? filedialog::Kind::PickFolder : filedialog::Kind::OpenPs3, title, start)) openImGuiPicker(folder);
    }
    void openImGuiPicker(bool folder) {
        m_pickFolder = folder;
        ImGuiFileDialog::Instance()->OpenDialog("ps3menu", folder ? tr("Choose the extracted Sly Collection (PS3)") : tr("Choose The Sly Collection (PS3) disc image"),
                                                folder ? nullptr : ".iso,.zip,.ISO,.ZIP",
                                                utf8(filedialog::startFolder({m_backend.config.value("ps3_browse_dir", std::string{})})), "", 1, nullptr,
                                                ImGuiFileDialogFlags_Modal);
    }
    void drawPicker() {
        if (auto r = m_dialog.poll()) {
            if (r->status == filedialog::Status::Chosen) startImport(r->path);
            else if (r->status == filedialog::Status::Unavailable) openImGuiPicker(m_pickFolder);
        }
        if (ImGuiFileDialog::Instance()->Display("ps3menu", ImGuiWindowFlags_NoCollapse, {650 * m_s, 400 * m_s}, {1100 * m_s, 760 * m_s})) {
            if (ImGuiFileDialog::Instance()->IsOk())
                startImport(m_pickFolder ? ImGuiFileDialog::Instance()->GetCurrentPath() : ImGuiFileDialog::Instance()->GetFilePathName());
            ImGuiFileDialog::Instance()->Close();
        }
    }
    void startImport(const std::string& source) {
        if (m_fixture.active || m_job.busy) return;
        try {
            m_backend.config["ps3_browse_dir"] = utf8(path(source).parent_path());
            saveConfig();
        } catch (...) {
        }
        m_importing = true;
        toast(tr("Importing the Sly Collection's menu files…"));
        const fs::path profile = ps3Profile();
        m_job.start("Collection menu import", [this, source, profile](const auto&) {
            importPs3Menu(path(source), profile, m_job.cancel, [this](float p) {
                std::lock_guard lock(m_job.mutex);
                m_job.progress = p;
            });
        });
    }
    // ---------------------------------------------------------------- credits
    void drawCredits(ImDrawList* dl, ImVec2 size) {
        const float s = m_s, W = size.x, H = size.y;
        dl->AddRectFilled({0, 0}, size, draw::rgba(0x05030C, 0.7f));
        banner(dl, size, tr("Credits"), 1.0f);
        const float pw = std::min(W - 48 * s, 900 * s);
        const ImRect pr({W * 0.5f - pw * 0.5f, 130 * s}, {W * 0.5f + pw * 0.5f, H - 90 * s});
        panel(dl, pr, 1.0f);
        std::vector<std::pair<std::string, std::string>> lines = {
            {"SafeHouse", tr(brand::credit)},
            {tr("Sly Cooper and the Thievius Raccoonus port"), brand::projectCaneCredit},
            {tr("SafeHouse logo and icon"), brand::logoCredit},
            {tr("Controller prompts"), "Kenney Input Prompts (CC0) · kenney.nl"},
            {tr("Typeface"), "Atkinson Hyperlegible, Braille Institute (SIL Open Font License)"},
            {tr("Japanese and Cyrillic letters"), "Noto Sans CJK JP, Noto Sans, Google (SIL Open Font License)"},
            {tr("This menu"), m_ps3 ? tr("After The Sly Collection (PS3). Its menu art and music are read from your own disc and stay in your profile.")
                                    : tr("After The Sly Collection (PS3): a drawn drive through the games' worlds. With your Sly Collection disc it "
                                         "shows that collection's own menu art and music (Options).")},
            {tr("Trademarks"), tr("Sly Cooper, its sequels, Bentley's Hackpack and their logos are trademarks of Sony Interactive Entertainment. "
                                  "SafeHouse is an unofficial fan project, not affiliated with or endorsed by Sony Interactive Entertainment, "
                                  "Sucker Punch Productions or Sanzaru Games.")},
        };
        const float wrap = pw - 90 * s;
        float total = 0;
        for (const auto& [a, b] : lines) total += 34 * s + ui::fonts().body->CalcTextSizeA(19 * s, FLT_MAX, wrap, b.c_str()).y + 22 * s;
        const float viewH = pr.GetHeight() - 50 * s;
        m_creditsScroll = std::clamp(m_creditsScroll, 0.0f, std::max(0.0f, total - viewH));
        dl->PushClipRect({pr.Min.x, pr.Min.y + 22 * s}, {pr.Max.x, pr.Max.y - 22 * s}, true);
        float y = pr.Min.y + 30 * s - m_creditsScroll;
        for (const auto& [a, b] : lines) {
            draw::outlined(dl, ui::fonts().display, 24 * s, {pr.Min.x + 44 * s, y}, 0xFFD84A, a.c_str(), 1.0f, 2.5f * s);
            y += 34 * s;
            dl->AddText(ui::fonts().body, 19 * s, {pr.Min.x + 46 * s, y}, draw::rgba(0xF2F6FF), b.c_str(), nullptr, wrap);
            y += ui::fonts().body->CalcTextSizeA(19 * s, FLT_MAX, wrap, b.c_str()).y + 22 * s;
        }
        dl->PopClipRect();
    }

    LauncherBackend& m_backend;
    GLFWwindow* m_window;
    Fixture m_fixture;
    ui::Job m_job;
    Audio m_audio;
    std::thread m_loader;
    std::mutex m_soundMutex;
    std::atomic<bool> m_soundsReady{false};
    std::shared_ptr<MenuSounds> m_sounds, m_pendingSounds;
    MenuImages m_images;
    Tex m_logo[4], m_background, m_loading, m_banner[3], m_frame[9];
    std::vector<Tex> m_skies;
    bool m_ps3 = false;
    std::array<Status, 3> m_status{};
    double m_time = 0, m_nextStatus = 0, m_itemSince = 0;
    float m_s = 1, m_scroll = 0, m_fade = 1, m_returnFade = 0, m_launchFade = 0;
    int m_item = 0, m_shownItem = 0, m_extra = 0, m_launching = -1, m_playing = -1;
    std::array<int, kItems> m_visits{};
    std::array<float, 2> m_arrowKick{};
    ImRect m_logoRect, m_hitLeft, m_hitRight, m_hitLogo;
    std::array<ImRect, 4> m_hitExtra{};
    bool m_quit = false, m_options = false, m_credits = false, m_wasBusy = false, m_importing = false, m_pickFolder = false, m_ensureVisible = false;
    float m_musicVolume = 0.7f, m_soundVolume = 0.8f, m_optionsScroll = 0, m_creditsScroll = 0, m_toastTime = 0;
    std::string m_toast;
    std::vector<Row> m_rows;
    std::vector<ImRect> m_rowRects;
    int m_row = 1;
    Sly2Settings m_sly2;
    Sly2Load m_sly2Load = Sly2Load::Missing;
    filedialog::Async m_dialog;
};
}  // namespace cane::collection
