#pragma once
// The Settings page: a game's settings file plus the launcher's own preferences in launcher.json. Sly 1: its
// Saves/system_settings.dat in the profile (platform/system_settings.h). Sly 2: <profile>/sly2/Settings.ini
// (platform/sly2_settings.h), which holds every Sly 2 option, the original game's own Options page included (the
// PS2 kept those in each save); the game's in-game Options read and write the same file. A row at the top picks the
// game; Sly 2 (Sly2-Release) reads the display, graphics, audio and controls options, so only those categories are
// shown for it. Only options the game itself has are shown. Each game has its own defaults (Sly 2: its keyboard /
// mouse layout, settings::sly2Defaults()). Every change is validated with sanitizeStatic() and saved at once with an
// atomic write. The file is re-read whenever the game rewrites it, and it is never written while the game runs or
// (Sly 1) when a newer build wrote it.
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "../backend.h"
#include "../install_location.h"
#include "../native_dialog.h"
#include "../desktop_shortcut.h"
#include "../steam.h"
#include "brand.h"
#include "controller_family.h"
#include "sly2_action_bindings.h"
#include "job.h"
#include "prompt_bar.h"
#include "shell.h"
#include "system_settings.h"
#include "sly2_settings.h"
#include "audio_devices_sdl.h"
#include "../collection/ps3menu.h"
#include "update_dialog.h"
#include "widgets.h"

namespace cane::ui {
// GLFW key name as the game prints it, with friendlier names for the
// non-printing keys.
inline std::string keyName(int key) {
    switch (key) {
    case GLFW_KEY_UNKNOWN: return tr("Unbound");
    case GLFW_KEY_ESCAPE: return tr("Esc");
    case GLFW_KEY_SPACE: return tr("Space");
    case GLFW_KEY_ENTER: return tr("Enter");
    case GLFW_KEY_TAB: return tr("Tab");
    case GLFW_KEY_BACKSPACE: return tr("Backspace");
    case GLFW_KEY_INSERT: return tr("Insert");
    case GLFW_KEY_DELETE: return tr("Delete");
    case GLFW_KEY_HOME: return tr("Home");
    case GLFW_KEY_END: return tr("End");
    case GLFW_KEY_PAGE_UP: return tr("Page Up");
    case GLFW_KEY_PAGE_DOWN: return tr("Page Down");
    case GLFW_KEY_CAPS_LOCK: return tr("Caps Lock");
    case GLFW_KEY_LEFT_SHIFT: return tr("Left Shift");
    case GLFW_KEY_RIGHT_SHIFT: return tr("Right Shift");
    case GLFW_KEY_LEFT_CONTROL: return tr("Left Ctrl");
    case GLFW_KEY_RIGHT_CONTROL: return tr("Right Ctrl");
    case GLFW_KEY_LEFT_ALT: return tr("Left Alt");
    case GLFW_KEY_RIGHT_ALT: return tr("Right Alt");
    case GLFW_KEY_LEFT_SUPER: return tr("Left Super");
    case GLFW_KEY_RIGHT_SUPER: return tr("Right Super");
    case GLFW_KEY_UP: return tr("Up Arrow");
    case GLFW_KEY_DOWN: return tr("Down Arrow");
    case GLFW_KEY_LEFT: return tr("Left Arrow");
    case GLFW_KEY_RIGHT: return tr("Right Arrow");
    case GLFW_KEY_KP_ENTER: return tr("Keypad Enter");
    default: break;
    }
    if (settings::mouseBinding(key)) {
        static constexpr const char* mouse[] = { N_("Left Mouse"), N_("Right Mouse"), N_("Middle Mouse"), N_("Mouse 4"), N_("Mouse 5"),
                                                 N_("Mouse 6"), N_("Mouse 7"), N_("Mouse 8"), N_("Wheel Up"), N_("Wheel Down") };
        return tr(mouse[key - settings::kMouseBindingBase]);
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return trf("Keypad %d", key - GLFW_KEY_KP_0);
    if (const char* name = glfwGetKeyName(key, 0)) return upper(name);
    if (key >= 32 && key < 127) return std::string(1, static_cast<char>(key));
    return trf("Key %d", key);
}

class SettingsPage {
    static constexpr int kAdvancedCapture = 1000;
public:
    enum Category { Display, Graphics, Audio, Controls, Interface, Launcher, Games, Advanced, CategoryCount };
    static constexpr std::array<const char*, CategoryCount> names{ N_("Display"), N_("Graphics"), N_("Audio"), N_("Controls"),
                                                                    N_("Interface"), N_("Launcher"), N_("Games"), N_("Advanced") };
    static constexpr std::array<const char*, CategoryCount> ids{ "display", "graphics", "audio", "controls",
                                                                  "interface", "launcher", "games", "advanced" };
    static constexpr std::array<const char*, CategoryCount> blurbs{
        N_("How the game window opens and how fast it runs."),
        N_("Rendering quality. Higher settings need a faster GPU."),
        N_("Speaker layout and volume levels."),
        N_("Button prompts, mouse, and key and button bindings."),
        N_("The in-game menus and HUD."),
        N_("This launcher's own preferences."),
        N_("Where games install, and the installed regions of each game. Regions install side by side and share their saves."),
#ifdef CANE_LAUNCHER_SOURCE_BUILD
        N_("Troubleshooting, logs and custom runtimes."),
#else
        N_("Troubleshooting and logs."),
#endif
    };
    // Launcher preferences the page edits on behalf of main.cpp.
    std::function<void(float)> setScale;
    std::function<void(bool)> setLogging; // "Save logs"
    std::function<bool()> logging;
    // "Install another region" of a game (main.cpp opens its install page in that mode).
    std::function<void(GameId)> installRegion;
    bool* showLogs = nullptr;
    // Settings > Launcher's update rows (main.cpp's UpdateUi).
    UpdateUi* updates = nullptr;
    // Settings > Launcher > Collection Style Launcher > Start (main.cpp starts `safehouse --collection` and closes).
    std::function<void()> startCollection;
    // The profile whose imported Collection menu (collection/ps3menu.h) the rows show: empty = none (screenshots).
    fs::path collectionProfile;
    static constexpr const char* kCollectionImportLabel = "Collection menu import";
    int category = Display;

    SettingsPage(LauncherBackend& backend, Job& job, GLFWwindow* window) : m_backend(backend), m_job(job), m_window(window) {
        GLint samples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &samples);
        m_maxSamples = samples;
        active() = this;
        previousKeyCallback() = glfwSetKeyCallback(window, keyCallback);
        load();
    }
    ~SettingsPage() {
        if (active() == this) active() = nullptr;
    }
    // Shared values the rest of the launcher reads.
    const settings::SystemSettings& values() const { return m_settings; }
    // The launcher's button prompt style (launcher.json "prompt_style").
    int promptStyle() const { return std::clamp(m_backend.config.value("prompt_style", 0), 0, input::kPromptStyleLast); }
    bool capturingPad() const { return m_captureAction >= 0 && m_captureController; }
    bool capturing() const { return m_captureAction >= 0; }
    void useFixture() { m_fixture = true; }
    // Screenshots: installed regions to show in Games.
    void fixturePs3Pack(GameId game, LauncherBackend::Ps3HdPack pack) { m_ps3Packs[static_cast<size_t>(game)] = std::move(pack); }
    void fixtureRegions(GameId game, std::vector<Installation> installs, std::string selected) {
        m_regionsFixture = true;
        m_installs[static_cast<size_t>(game)] = std::move(installs);
        m_selected[static_cast<size_t>(game)] = std::move(selected);
    }
    // The tab was opened: show what is on disk now (for the selected game,
    // when it has settings; otherwise the page keeps the game it shows).
    void open() {
        load();
        m_focusRail = true;
    }
    void open(GameId game) {
        if (hasSettings(game)) m_game = game;
        if (!shown(category)) category = Display;
        open();
    }
    GameId game() const { return m_game; }
    // The games whose settings this page edits.
    static bool hasSettings(GameId game) { return game == GameId::Sly1 || game == GameId::Sly2; }
    // Whether a category applies to the shown game.
    bool shown(int c) const {
        if (m_game == GameId::Sly1) return true;
        return c == Display || c == Graphics || c == Audio || c == Controls || c == Launcher || c == Games || c == Advanced;
    }
    void selectCategory(int next) {
        cancelCapture();
        const int step = next < category ? -1 : 1;
        int c = (next % CategoryCount + CategoryCount) % CategoryCount;
        for (int i = 0; i < CategoryCount && !shown(c); ++i) c = ((c + step) % CategoryCount + CategoryCount) % CategoryCount;
        category = c;
        m_scrollTop = true;
    }
    void selectCategory(const std::string& id) {
        for (int i = 0; i < CategoryCount; ++i)
            if (id == ids[static_cast<size_t>(i)]) category = i;
    }
    // Ends a binding capture without changing anything. main.cpp calls this
    // whenever the Settings tab is not shown, so a capture never outlives it.
    void cancelCapture() { m_captureAction = -1; }
    // Footer status while a controller binding is captured: every button can
    // be bound, so none of them can cancel; the capture times out instead.
    std::string captureStatus() const {
        if (!capturingPad()) return {};
        const int left = std::max(1, static_cast<int>(std::ceil(kPadCaptureSeconds - (ImGui::GetTime() - m_captureStarted))));
        return trf("Press the button for %s · cancels in %d s, or press Esc", tr(captureName()), left);
    }
    // B (keyboard capture only) or Esc: cancel a capture, then return from
    // the pane to the rail.
    bool back() {
        if (capturing()) {
            cancelCapture();
            return true;
        }
        if (m_paneFocused) {
            m_focusRail = true;
            return true;
        }
        return false;
    }
    std::vector<launcher::Hint> hints() const {
        using input::Button;
        if (capturingPad()) return {};
        if (capturing()) return { { backButton(), GLFW_KEY_ESCAPE, N_("Cancel") } };
        std::vector<launcher::Hint> h{ { Button::LT, 0, "" }, { Button::RT, 0, N_("Category") } };
        if (m_paneFocused) h.push_back({ Button::DpadLeft, 0, N_("Change") });
        h.push_back({ backButton(), GLFW_KEY_ESCAPE, N_("Back") });
        h.push_back({ confirmButton(), GLFW_KEY_ENTER, N_("Select") });
        return h;
    }

    void draw(bool gameRunning) {
        poll();
        if (gameRunning) cancelCapture();
        pollPadCapture();
        pollMouseCapture();
        if (capturing() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancelCapture();
        m_help.clear();
        // Title row with the save state on the right.
        const ImVec2 titlePos = ImGui::GetCursorPos();
        const std::string subtitle = trf("Game settings apply to %s. Changes are saved at once.", cane::game(m_game).title);
        titleBar(tr("Settings"), false, subtitle.c_str());
        saveIndicator(titlePos);
        const ImVec2 area = ImGui::GetContentRegionAvail();
        const float railW = std::min(px(220), std::floor(area.x * 0.24f));
        rail(railW, area.y);
        ImGui::SameLine(0, px(18));
        pane(area.x - railW - px(24), area.y, gameRunning);
        m_focusRail = false;
    }
    void drawDialogs() {
        if (auto r = m_installDialog.poll()) {  // "Choose a folder…" (Games: where games install)
            if (r->status == filedialog::Status::Chosen) setInstallRoot(path(r->path));
            else if (r->status == filedialog::Status::Unavailable) openInstallPicker();
            else if (r->status == filedialog::Status::Failed) m_locationNote = r->error;
        }
        if (ImGuiFileDialog::Instance()->Display("installdir", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) })) {
            if (ImGuiFileDialog::Instance()->IsOk()) setInstallRoot(path(ImGuiFileDialog::Instance()->GetCurrentPath()));
            ImGuiFileDialog::Instance()->Close();
        }
        // the PS3 HD import's source, or the Collection menu import's (native_dialog.h)
        auto ps3Chosen = [this](const std::string& source) {
            if (m_ps3ForCollection) startCollectionImport(source);
            else startPs3Import(m_ps3Game, source);
        };
        if (auto r = m_ps3Dialog.poll()) {
            if (r->status == filedialog::Status::Chosen) ps3Chosen(r->path);
            else if (r->status == filedialog::Status::Unavailable) openPs3Picker(m_ps3PickFolder);
        }
        if (ImGuiFileDialog::Instance()->Display("ps3src", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) })) {
            if (ImGuiFileDialog::Instance()->IsOk())
                ps3Chosen(m_ps3PickFolder ? ImGuiFileDialog::Instance()->GetCurrentPath() : ImGuiFileDialog::Instance()->GetFilePathName());
            ImGuiFileDialog::Instance()->Close();
        }
        if (auto r = m_langDialog.poll()) {  // a language import's source (Sly 2: a disc image, The Sly Collection)
            if (r->status == filedialog::Status::Chosen) startLanguageImport(r->path);
            else if (r->status == filedialog::Status::Unavailable) openLanguagePicker();
        }
        if (ImGuiFileDialog::Instance()->Display("langsrc", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) })) {
            if (ImGuiFileDialog::Instance()->IsOk())
                startLanguageImport(m_langPick == LangPick::Ps3Folder ? ImGuiFileDialog::Instance()->GetCurrentPath() : ImGuiFileDialog::Instance()->GetFilePathName());
            ImGuiFileDialog::Instance()->Close();
        }
        if (auto r = m_sourceDialog.poll()) {  // the system's folder picker (native_dialog.h)
            if (r->status == filedialog::Status::Chosen) std::snprintf(m_source, sizeof(m_source), "%s", r->path.c_str());
            else if (r->status == filedialog::Status::Unavailable) openSourcePicker();
        }
        if (!ImGuiFileDialog::Instance()->Display("source", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) }))
            return;
        if (ImGuiFileDialog::Instance()->IsOk())
            std::snprintf(m_source, sizeof(m_source), "%s", ImGuiFileDialog::Instance()->GetCurrentPath().c_str());
        ImGuiFileDialog::Instance()->Close();
    }

private:
    void openSourcePicker() {
        ImGuiFileDialog::Instance()->OpenDialog("source", tr("Choose the SafeHouse source folder"), nullptr,
                                                utf8(filedialog::startFolder({ m_source })), "", 1, nullptr, ImGuiFileDialogFlags_Modal);
    }
    filedialog::Async m_sourceDialog;
    // ------------------------------------------------------------ storage
    fs::path file() const {
        return m_game == GameId::Sly2 ? settings::sly2SettingsPath(m_backend.userDir(m_game)) : settings::settingsPath(m_backend.userDir(m_game));
    }
    // The shown game's defaults (Sly 2 has its own keyboard / mouse layout).
    settings::SystemSettings gameDefaults() const { return m_game == GameId::Sly2 ? settings::sly2Defaults() : settings::defaults(); }
    void load() {
        if (m_game == GameId::Sly2) {
            // (Settings.ini; made once from the old system_settings.dat, which the game would do at its next start too)
            const auto result = settings::loadSly2(m_backend.userDir(m_game), m_sly2);
            m_settings = m_sly2.system;
            m_version = 0;
            m_result = result == settings::Sly2Load::Ok         ? settings::LoadResult::Ok
                       : result == settings::Sly2Load::Migrated ? settings::LoadResult::Upgraded
                       : result == settings::Sly2Load::Missing  ? settings::LoadResult::Missing
                                                                : settings::LoadResult::Corrupt;
            if (result == settings::Sly2Load::Migrated && !m_fixture) settings::saveSly2(m_backend.userDir(m_game), m_sly2);
            m_sly2Saved = m_sly2.game;
        } else {
            m_result = settings::load(file(), m_settings, &m_version);
            if (!settings::loaded(m_result)) m_settings = gameDefaults();
        }
        if (m_game == GameId::Sly2) m_actions = sly2input::load(m_backend.userDir(m_game));
        std::error_code ec;
        m_mtime = fs::last_write_time(file(), ec);
        if (ec) m_mtime = {};
        if (m_game == GameId::Sly2) {
            m_actionMtime = fs::last_write_time(sly2input::profilePath(m_backend.userDir(m_game)), ec);
            if (ec) m_actionMtime = {};
        }
        m_nextPoll = ImGui::GetCurrentContext() ? ImGui::GetTime() + 0.5 : 0.0;
        m_debug = readDebugMode();
    }
    void poll() {
        if (ImGui::GetTime() < m_nextPoll || capturing()) return;
        m_nextPoll = ImGui::GetTime() + 0.5;
        std::error_code ec;
        const auto now = fs::last_write_time(file(), ec);
        if ((ec ? fs::file_time_type{} : now) != m_mtime) load();
        if (m_game == GameId::Sly2) {
            const auto actionsNow = fs::last_write_time(sly2input::profilePath(m_backend.userDir(m_game)), ec);
            if ((ec ? fs::file_time_type{} : actionsNow) != m_actionMtime) load();
        }
    }
    void commitActions() {
        if (!writable() || m_actions.readOnly) return;
        m_saveOk = sly2input::save(m_backend.userDir(m_game), m_actions);
        m_savedAt = ImGui::GetTime();
        std::error_code ec;
        m_actionMtime = fs::last_write_time(sly2input::profilePath(m_backend.userDir(m_game)), ec);
        if (ec) m_actionMtime = {};
    }
    bool writable() const { return m_result != settings::LoadResult::Newer && !m_fixture; }
    void commit() {
        if (!writable()) return;
        settings::sanitizeStatic(m_settings, monitorHeight(), m_maxSamples,
                                 m_game == GameId::Sly2 ? settings::kSly2DefaultKeyboardBindings : settings::kDefaultKeyboardBindings);
        if (m_game == GameId::Sly2) {
            // (a save-kept option changed here is the user's choice: no longer taken from the newest save)
            const auto& was = m_sly2.system;
            if (was.controllerVibration != m_settings.controllerVibration) settings::clearPending(m_sly2, settings::kSaveOptionVibration);
            if (was.musicVolume != m_settings.musicVolume) settings::clearPending(m_sly2, settings::kSaveOptionMusic);
            if (was.soundEffectsVolume != m_settings.soundEffectsVolume) settings::clearPending(m_sly2, settings::kSaveOptionEffects);
            if (m_sly2Saved.speakers != m_sly2.game.speakers) settings::clearPending(m_sly2, settings::kSaveOptionSpeakers);
            if (m_sly2Saved.invertBinocucom != m_sly2.game.invertBinocucom) settings::clearPending(m_sly2, settings::kSaveOptionInvertBinocucom);
            if (m_sly2Saved.invertTurret != m_sly2.game.invertTurret) settings::clearPending(m_sly2, settings::kSaveOptionInvertTurret);
            m_sly2.system = m_settings;
            // (the file read again first: keys added by hand or by another build since are kept)
            settings::Sly2Settings onDisk;
            if (settings::loadSly2(m_backend.userDir(m_game), onDisk) == settings::Sly2Load::Ok) m_sly2.unknown = onDisk.unknown;
            m_saveOk = settings::saveSly2(m_backend.userDir(m_game), m_sly2);
            if (m_saveOk) m_sly2Saved = m_sly2.game;
        } else
            m_saveOk = settings::save(file(), m_settings);
        m_savedAt = ImGui::GetTime();
        if (m_saveOk) {
            m_result = settings::LoadResult::Ok;
            std::error_code ec;
            m_mtime = fs::last_write_time(file(), ec);
        }
    }
    void persistLauncher() {
        if (m_fixture) return;
        writeJson(m_backend.locations.config / "launcher.json", m_backend.config);
    }
    int monitorHeight() const {
        // Wayland does not reveal window positions: use the primary monitor,
        // as the game does there.
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            if (GLFWmonitor* primary = glfwGetPrimaryMonitor())
                if (const GLFWvidmode* mode = glfwGetVideoMode(primary)) return mode->height;
            return 0;
        }
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        int wx = 0, wy = 0, ww = 0, wh = 0;
        glfwGetWindowPos(m_window, &wx, &wy);
        glfwGetWindowSize(m_window, &ww, &wh);
        for (int i = 0; i < count; ++i) {
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitors[i], &mx, &my);
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
            if (mode && wx + ww / 2 >= mx && wx + ww / 2 < mx + mode->width && wy + wh / 2 >= my && wy + wh / 2 < my + mode->height)
                return mode->height;
        }
        if (GLFWmonitor* primary = glfwGetPrimaryMonitor())
            if (const GLFWvidmode* mode = glfwGetVideoMode(primary)) return mode->height;
        return 0;
    }
    static constexpr int kUncappedFrameRate = 1000;
    std::vector<int> frameRates() const {
        std::vector<int> rates{ 30, 60, 90, 120, 240 };
        // Sly 2 runs 60 Hz steps drawn between them at any rate (Sly2-Release frameinterp.h): 144 and no limit too
        // (kUncappedFrameRate, an ordinary value of the shared field: Sly2-Release reads it as no limit)
        if (m_game == GameId::Sly2) rates.insert(rates.end(), { 144, kUncappedFrameRate });
        if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
            int count = 0;
            const GLFWvidmode* modes = glfwGetVideoModes(m, &count);
            int maximum = 0;
            for (int i = 0; i < count; ++i) maximum = std::max(maximum, modes[i].refreshRate);
            if (maximum >= 30) rates.push_back(maximum);
        }
        std::sort(rates.begin(), rates.end());
        rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
        return rates;
    }
    fs::path debugFile() const { return m_backend.debugModeFile(); }
    bool readDebugMode() const { return debugModeIn(debugFile()); }
    void writeDebugMode(bool enabled) {
        if (m_fixture) return;
        std::ofstream(debugFile()) << "[Debug]\nDebug Mode=" << (enabled ? 1 : 0) << '\n';
    }

    // ------------------------------------------------------------ capture
    static SettingsPage*& active() {
        static SettingsPage* page = nullptr;
        return page;
    }
    static GLFWkeyfun& previousKeyCallback() {
        static GLFWkeyfun callback = nullptr;
        return callback;
    }
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        SettingsPage* page = active();
        // Presses go to the capture and never reach the UI. Releases still do,
        // so the Enter that started the capture is not left held down.
        if (page && page->m_captureAction >= 0 && (!page->m_captureController || (page->m_captureAction >= kAdvancedCapture && (key == GLFW_KEY_DELETE || key == GLFW_KEY_ESCAPE))) && action != GLFW_RELEASE) {
            if (action == GLFW_PRESS) page->keyCaptured(key);
            return;
        }
        if (previousKeyCallback()) previousKeyCallback()(window, key, scancode, action, mods);
    }
    void keyCaptured(int key) {
        if (key == GLFW_KEY_ESCAPE) {
            cancelCapture();
            return;
        }
        if (m_captureAction >= kAdvancedCapture) {
            if (key != GLFW_KEY_DELETE && !settings::validKeyboardBinding(key)) { m_captureNote = tr("That key cannot be bound."); return; }
            (m_captureController ? m_actions.gamepad : m_actions.keyboard)[size_t(m_captureAction - kAdvancedCapture)] = key == GLFW_KEY_DELETE ? -1 : key;
            m_captureAction = -1;
            commitActions();
            return;
        }
        const bool camera = m_captureAction >= settings::kBindingCount;
        if (camera ? key == settings::kKeyUnknown || !settings::validCameraKey(key) : !settings::validKeyboardBinding(key)) {
            m_captureNote = tr("That key cannot be bound.");
            return;
        }
        if (camera) m_settings.cameraKeyBindings[m_captureAction - settings::kBindingCount] = key;
        else m_settings.keyboardBindings[m_captureAction] = key;
        m_captureAction = -1;
        commit();
    }
    const char* captureName() const {
        if (m_captureAction >= kAdvancedCapture) return tr(sly2input::kActions[size_t(m_captureAction - kAdvancedCapture)].label);
        return tr(m_captureAction >= settings::kBindingCount ? cameraNames[static_cast<size_t>(m_captureAction - settings::kBindingCount)]
                                                             : actionNames[static_cast<size_t>(m_captureAction)]);
    }
    // Action capture accepts mouse buttons and the wheel after the click that
    // selected the row has been released. Camera bindings use keyboard keys.
    void pollMouseCapture() {
        if (!capturing() || m_captureController || (m_captureAction >= settings::kBindingCount && m_captureAction < kAdvancedCapture)) return;
        const auto& io = ImGui::GetIO();
        if (!m_captureArmed) {
            bool held = false;
            for (int b = 0; b < 5; ++b) held |= ImGui::IsMouseDown(b);
            if (!held) m_captureArmed = true;
            return;
        }
        int binding = -1;
        for (int button = 0; button < 5 && binding < 0; ++button)
            if (ImGui::IsMouseClicked(button)) binding = settings::kMouseBindingBase + button;
        if (binding < 0 && io.MouseWheel > 0) binding = settings::kMouseWheelUp;
        if (binding < 0 && io.MouseWheel < 0) binding = settings::kMouseWheelDown;
        if (binding < 0) return;
        if (m_captureAction >= kAdvancedCapture) {
            m_actions.keyboard[size_t(m_captureAction - kAdvancedCapture)] = binding;
            m_captureAction = -1;
            m_captureCooldown = ImGui::GetTime() + 0.3;
            commitActions();
            return;
        }
        m_settings.keyboardBindings[m_captureAction] = binding;
        m_captureAction = -1;
        m_captureCooldown = ImGui::GetTime() + 0.3;
        commit();
    }
    void startCapture(int action, bool controller) {
        m_captureAction = action;
        m_captureController = controller;
        m_captureArmed = false;
        m_captureStarted = ImGui::GetTime();
        m_captureNote.clear();
    }
    static constexpr double kPadCaptureSeconds = 6.0;
    void pollPadCapture() {
        if (!capturingPad()) return;
        if (ImGui::GetTime() - m_captureStarted > kPadCaptureSeconds) {
            cancelCapture();
            return;
        }
        int pressed = -1;
        bool any = false;
        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
            GLFWgamepadstate state{};
            if (!glfwJoystickIsGamepad(jid) || input::ignoredSteamDuplicate(jid) || !glfwGetGamepadState(jid, &state)) continue;
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
                if (state.buttons[b] == GLFW_PRESS && b != GLFW_GAMEPAD_BUTTON_GUIDE) {
                    any = true;
                    if (pressed < 0) pressed = b;
                }
            if ((state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1) * 0.5f > 0.5f) any = true, pressed = pressed < 0 ? settings::kGamepadLeftTrigger : pressed;
            if ((state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1) * 0.5f > 0.5f) any = true, pressed = pressed < 0 ? settings::kGamepadRightTrigger : pressed;
        }
        // Wait for the button that started the capture to be released.
        if (!m_captureArmed) {
            m_captureArmed = !any;
            return;
        }
        if (pressed < 0) return;
        const bool advanced = m_captureAction >= kAdvancedCapture;
        if (advanced) m_actions.gamepad[size_t(m_captureAction - kAdvancedCapture)] = pressed;
        else m_settings.gamepadBindings[m_captureAction] = pressed;
        m_captureAction = -1;
        m_captureCooldown = ImGui::GetTime() + 0.3;
        if (advanced) commitActions(); else commit();
    }

    // ------------------------------------------------------------ chrome
    void saveIndicator(ImVec2 titlePos) {
        const char* label = nullptr;
        uint32_t tone = color::success;
        if (m_result == settings::LoadResult::Newer) label = tr("Read only"), tone = color::warning;
        else if (!m_saveOk) label = tr("Could not save settings"), tone = color::danger;
        else if (ImGui::GetTime() - m_savedAt < 2.5) label = tr("Saved");
        if (!label) return;
        const ImVec2 after = ImGui::GetCursorPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const auto t = measure(fonts().caption, label);
        const ImVec2 p{ ImGui::GetWindowPos().x + titlePos.x + w - t.x, ImGui::GetWindowPos().y + titlePos.y - ImGui::GetScrollY() + px(12) };
        auto* dl = ImGui::GetWindowDrawList();
        drawIcon(dl, tone == color::success ? Icon::Check : Icon::Warning, { p.x - px(14), p.y + t.y * 0.5f }, px(16), ink(tone));
        dl->AddText(fonts().caption, fonts().caption->FontSize, p, ink(tone), label);
        ImGui::SetCursorPos(after);
    }
    void rail(float width, float height) {
        // Padding keeps the focus ring inside the rail's clip rectangle.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - px(6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(6), px(6) });
        ImGui::BeginChild("settings-rail", { width + px(12), height }, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
        ImGui::PopStyleVar();
        for (int i = 0; i < CategoryCount; ++i) {
            if (!shown(i)) continue;
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const ImGuiID id = window->GetID(names[static_cast<size_t>(i)]);
            const ImRect bb(window->DC.CursorPos, { window->DC.CursorPos.x + width, window->DC.CursorPos.y + px(48) });
            ImGui::ItemSize(bb.GetSize());
            if (!ImGui::ItemAdd(bb, id)) continue;
            bool hovered = false, held = false;
            if (ImGui::ButtonBehavior(bb, id, &hovered, &held) && category != i) selectCategory(i);
            // Pad and keyboard focus selects (console style).
            if (ImGui::IsItemFocused() && !input::tracker().lastWasMouse() && category != i && !m_focusRail) selectCategory(i);
            if ((m_focusRail || (focusRequested() && GImGui->FrameCount != focusRequestFrame())) && category == i) {
                setFocus(id, window, !input::tracker().lastWasMouse());
                focusRequested() = false;
            }
            // Right moves into the category's rows.
            if (ImGui::IsItemFocused() && navRightPressed()) m_focusRows = true;
            const bool selected = category == i;
            auto* dl = window->DrawList;
            if (selected) {
                dl->AddRectFilled(bb.Min, bb.Max, ink(color::raised), px(8));
                dl->AddRectFilled({ bb.Min.x, bb.Min.y + px(12) }, { bb.Min.x + px(3), bb.Max.y - px(12) }, ink(color::cta), px(2));
            } else if (hovered)
                dl->AddRectFilled(bb.Min, bb.Max, ink(color::surface), px(8));
            ImFont* font = selected ? fonts().tab : fonts().body;
            dl->AddText(font, font->FontSize, { bb.Min.x + px(18), std::round(bb.GetCenter().y - font->FontSize * 0.5f) },
                        ink(selected ? color::text : hovered ? color::text : color::text2), tr(names[static_cast<size_t>(i)]));
            focusRing(bb, id, px(8));
            recordLayout(names[static_cast<size_t>(i)], bb);
        }
        ImGui::EndChild();
    }
    void describe(const char* help) {
        if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) m_help = help;
    }
    void pane(float width, float height, bool gameRunning) {
        beginCard("settings-pane", { width, height }, 0);
        const float padX = px(24);
        ImGui::SetCursorPos({ padX, px(20) });
        ImGui::BeginGroup();
        heading(tr(names[static_cast<size_t>(category)]));
        tighten(8);
        textWrapped(tr(blurbs[static_cast<size_t>(category)]), fonts().caption, color::muted);
        ImGui::EndGroup();
        const bool systemCategory = category <= Interface;
        const bool locked = systemCategory && gameRunning;
        const bool readOnly = systemCategory && m_result == settings::LoadResult::Newer;
        ImGui::SetCursorPosX(padX);
        ImGui::PushItemWidth(width - padX * 2);
        if (locked || readOnly || (systemCategory && m_result == settings::LoadResult::Corrupt)) {
            ImGui::BeginChild("pane-banner", { width - padX * 2, 0 }, ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
            if (locked) banner("locked", tr("Close the game to change settings – changes made in-game are saved automatically."), BannerTone::Info, false);
            else if (readOnly) banner("newer", tr("These settings were saved by a newer version of the game. Update SafeHouse to change them here."), BannerTone::Warning, false);
            else banner("corrupt", tr("The settings file could not be read, so defaults are shown. Changing anything saves a fresh file."), BannerTone::Warning, false);
            ImGui::EndChild();
        }
        ImGui::PopItemWidth();
        const float footer = px(56);
        ImGui::SetCursorPosX(px(4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(6), px(6) });
        ImGui::BeginChild("pane-rows", { width - px(8), ImGui::GetContentRegionAvail().y - footer }, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
        ImGui::PopStyleVar();
        if (m_scrollTop) ImGui::SetScrollY(0), m_scrollTop = false;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { px(12), px(4) });
        const bool paneWasFocused = m_paneFocused;
        m_paneFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        firstRow() = {};
        if (systemCategory) gameRow(locked);
        ImGui::BeginDisabled(locked || readOnly);
        switch (category) {
        case Display: displayRows(); break;
        case Graphics: graphicsRows(); break;
        case Audio: audioRows(); break;
        case Controls: controlsRows(); break;
        case Interface: interfaceRows(); break;
        case Launcher: launcherRows(); break;
        case Games: gamesRows(gameRunning); break;
        case Advanced: advancedRows(gameRunning); break;
        default: break;
        }
        ImGui::EndDisabled();
        // Entering the rows from the rail (or the header) with the pad or the
        // keyboard starts at the top row, not at the row level with the rail.
        if (((m_paneFocused && !paneWasFocused && !input::tracker().lastWasMouse()) || m_focusRows) && firstRow().id &&
            GImGui->NavId != firstRow().id)
            setFocus(firstRow().id, firstRow().window, true);
        m_focusRows = false;
        gap(8);
        ImGui::PopStyleVar();
        padScrollHere();
        ImGui::EndChild();
        // Footer: description of the focused row, and Restore defaults.
        ImVec2 p = ImGui::GetCursorScreenPos();
        p.x = ImGui::GetWindowPos().x;
        p.y -= ImGui::GetStyle().ItemSpacing.y;
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddLine({ p.x, p.y }, { p.x + width, p.y }, ink(color::divider), px(1));
        // (wider when the translation of "Restore defaults" is: the help text wraps in what is left)
        const float restoreW = std::min(std::max(px(180), measure(fonts().body, tr("Restore defaults")).x + px(18 + 10 + 24)), std::floor(width * 0.45f));
        const float wrap = width - padX * 2 - restoreW - px(16);
        const char* help = m_help.empty() ? "" : m_help.c_str();
        const auto t = fonts().caption->CalcTextSizeA(fonts().caption->FontSize, FLT_MAX, wrap, help);
        dl->AddText(fonts().caption, fonts().caption->FontSize, { p.x + padX, std::round(p.y + (footer - t.y) * 0.5f) },
                    ink(color::text2), help, nullptr, wrap);
        ImGui::SetCursorScreenPos({ p.x + width - padX - restoreW, p.y + (footer - controlHeight()) * 0.5f });
        ImGui::BeginDisabled(locked || readOnly);
        if (button(tr("Restore defaults"), Tone::Quiet, { restoreW, 0 }, Icon::Refresh)) restoreDefaults();
        ImGui::EndDisabled();
        endCard();
    }
    void restoreDefaults() {
        const auto d = gameDefaults();
        auto& s = m_settings;
        switch (category) {
        case Display:
            s.windowMode = d.windowMode, s.vsyncEnabled = d.vsyncEnabled, s.aspectMode = d.aspectMode, s.frameRate = d.frameRate;
            break;
        case Graphics:
            s.internalResolutionHeight = d.internalResolutionHeight, s.msaaEnabled = d.msaaEnabled, s.msaaSamples = d.msaaSamples;
            s.drawDistance = d.drawDistance, s.fogType = d.fogType, s.longShadows = d.longShadows, s.fastShadows = d.fastShadows;
            s.renderer = d.renderer;
            break;
        case Audio:
            settings::defaultAudio(s);
            m_sly2.game.speakers = settings::Sly2GameOptions{}.speakers;
            m_sly2.game.outputDevice.clear(), m_sly2.game.inputDevice.clear(), m_sly2.game.headsetOutputDevice.clear();
            m_sly2.game.usbHeadset = 0;
            break;
        case Controls:
            if (m_game == GameId::Sly2 && m_actions.enabled) {
                m_actions = sly2input::defaults();
                m_actions.enabled = true;
                commitActions();
                return;
            }
            std::copy(std::begin(d.keyboardBindings), std::end(d.keyboardBindings), s.keyboardBindings);
            std::copy(std::begin(d.gamepadBindings), std::end(d.gamepadBindings), s.gamepadBindings);
            s.disableControllerInputWhenUnfocused = d.disableControllerInputWhenUnfocused;
            s.alwaysOriginalPsPrompts = d.alwaysOriginalPsPrompts, s.promptStyle = d.promptStyle;
            s.instantSkipCutscenes = d.instantSkipCutscenes;
            s.mouseCamera = d.mouseCamera, s.mouseSensitivity = d.mouseSensitivity, s.mouseAimSensitivity = d.mouseAimSensitivity;
            s.mouseInvertX = d.mouseInvertX, s.mouseInvertY = d.mouseInvertY, s.mouseRawInput = d.mouseRawInput;
            s.padInvertCameraX = d.padInvertCameraX, s.padInvertCameraY = d.padInvertCameraY;
            s.autoCameraRotation = d.autoCameraRotation;
            s.mouseInvertBinocX = d.mouseInvertBinocX, s.mouseInvertBinocY = d.mouseInvertBinocY;
            s.mouseInvertTurretX = d.mouseInvertTurretX, s.mouseInvertTurretY = d.mouseInvertTurretY;
            s.mouseWheelZoom = d.mouseWheelZoom, s.mouseMenus = d.mouseMenus, s.hackMouseAim = d.hackMouseAim;
            s.controllerVibration = d.controllerVibration;
            std::copy(std::begin(d.cameraKeyBindings), std::end(d.cameraKeyBindings), s.cameraKeyBindings);
            std::copy(std::begin(d.tankKeyBindings), std::end(d.tankKeyBindings), s.tankKeyBindings);
            m_sly2.game.invertBinocucom = settings::Sly2GameOptions{}.invertBinocucom;
            m_sly2.game.invertTurret = settings::Sly2GameOptions{}.invertTurret;
            break;
        case Interface: s.guiStyle = d.guiStyle, s.guiScale = d.guiScale; break;
        case Launcher:
            m_backend.config.erase("ui_scale");
            m_backend.config.erase("start_fullscreen");
            m_backend.config.erase("prompt_style");
            m_backend.config.erase("check_updates");
            m_backend.config.erase("language");
            persistLauncher();
            l10n::setLanguage("system");
            if (setScale) setScale(1.0f);
            return;
        case Advanced:
            m_debug = false;
            writeDebugMode(false);
            if (showLogs) *showLogs = false;
            m_backend.config.erase("show_logs");
            persistLauncher();
            if (setLogging && !m_fixture) setLogging(false);
            return;
        default: return;
        }
        commit();
    }

    // ------------------------------------------------------------ rows
    // The game whose settings are shown (Sly 1 / Sly 2).
    void gameRow(bool locked) {
        static const std::vector<std::string> games{ cane::game(GameId::Sly1).shortName, cane::game(GameId::Sly2).shortName };
        int index = m_game == GameId::Sly2 ? 1 : 0;
        ImGui::BeginDisabled(locked);
        if (stepperRow(tr("Game"), &index, games)) {
            cancelCapture();
            m_game = index == 1 ? GameId::Sly2 : GameId::Sly1;
            if (!shown(category)) category = Display;
            load();
        }
        ImGui::EndDisabled();
        describe(tr("Which game's settings this page shows. Each game keeps its own settings file."));
    }
    template <class T> static int indexOf(const std::vector<T>& values, T value) {
        auto it = std::find(values.begin(), values.end(), value);
        return it == values.end() ? -1 : static_cast<int>(it - values.begin());
    }
    bool toggle(const char* label, int& field, const char* help) {
        bool value = field != 0;
        const bool changed = toggleRow(label, &value);
        describe(help);
        if (changed) field = value ? 1 : 0, commit();
        return changed;
    }
    // A whole-number option as a stepper over min..max.
    bool rangeRow(const char* label, int& field, int minimum, int maximum, int step, const char* help,
                  const std::function<std::string(int)>& name) {
        std::vector<int> values;
        std::vector<std::string> labels;
        for (int v = minimum; v <= maximum; v += step) values.push_back(v), labels.push_back(name(v));
        int index = indexOf(values, field);
        const std::string custom = name(field);
        const bool changed = stepperRow(label, &index, labels, nullptr, index < 0 ? custom.c_str() : nullptr);
        describe(help);
        if (changed) field = values[static_cast<size_t>(index)], commit();
        return changed;
    }
    static std::string percent(int v) { return std::to_string(v) + "%"; }
    void displayRows() {
#if !defined(CANE_ANDROID) && !defined(CANE_IOS)
        // (Android: the system places the window, in DeX too; nothing to choose. iOS: the app fills its window)
        const std::vector<std::string> modes{ tr("Windowed"), tr("Borderless"), tr("Fullscreen") };
        int mode = m_settings.windowMode;
        if (stepperRow(tr("Window mode"), &mode, modes)) m_settings.windowMode = mode, commit();
        describe(tr("Borderless fills the screen without changing its resolution."));
#endif
        toggle(tr("VSync"), m_settings.vsyncEnabled, tr("Matches the frame rate to the display to stop tearing."));
        const std::vector<std::string> aspects{ tr("Fit to screen"), "4:3", "16:9", "16:10", tr("4:3 (PS2)"), tr("16:9 (PS2)") };
        int aspect = m_settings.aspectMode;
        if (stepperRow(tr("Aspect ratio"), &aspect, aspects)) m_settings.aspectMode = aspect, commit();
        describe(tr("Fit to screen uses the whole window. The PS2 options match the original framing."));
        const auto rates = frameRates();
        std::vector<std::string> labels;
        for (int r : rates) labels.push_back(r >= kUncappedFrameRate ? std::string(tr("Uncapped")) : trf("%d FPS", r));
        int index = indexOf(rates, m_settings.frameRate);
        const std::string custom = trf("%d FPS", m_settings.frameRate);
        if (stepperRow(tr("Frame rate limit"), &index, labels, nullptr, index < 0 ? custom.c_str() : nullptr))
            m_settings.frameRate = rates[static_cast<size_t>(index)], commit();
        describe(tr("The highest frame rate the game runs at. Gameplay speed is the same at every setting."));
    }
    void graphicsRows() {
        const int monitor = monitorHeight();
        std::vector<int> heights;
        std::vector<std::string> labels;
        for (int h : settings::kInternalResolutions)
            if (h == 0 || monitor <= 0 || h <= monitor) heights.push_back(h), labels.push_back(h ? std::to_string(h) + "p" : tr("Native"));
        int index = indexOf(heights, m_settings.internalResolutionHeight);
        // A height above this monitor's (set on a bigger display) is kept
        // and shown as it is until another value is picked.
        const std::string stored = std::to_string(m_settings.internalResolutionHeight) + "p";
        if (stepperRow(tr("Internal resolution"), &index, labels, nullptr, index < 0 ? stored.c_str() : nullptr))
            m_settings.internalResolutionHeight = heights[static_cast<size_t>(index)], commit();
        describe(tr("Renders the 3D scene at this height, then scales it to the window. Native matches the window."));
        std::vector<int> samples{ 0 };
        std::vector<std::string> aa{ tr("Off") };
        for (int n : { 2, 4, 8, 16 })
            if (m_maxSamples <= 0 || n <= m_maxSamples) samples.push_back(n), aa.push_back(std::to_string(n) + "x");
        int current = m_settings.msaaEnabled ? m_settings.msaaSamples : 0;
        int aaIndex = indexOf(samples, current);
        if (stepperRow(tr("Anti-aliasing"), &aaIndex, aa, nullptr, aaIndex < 0 ? tr("Custom") : nullptr)) {
            const int n = samples[static_cast<size_t>(aaIndex)];
            m_settings.msaaEnabled = n != 0;
            if (n) m_settings.msaaSamples = n;
            commit();
        }
        describe(tr("Smooths jagged edges (MSAA). Higher values cost more GPU time."));
        static const std::vector<float> distances{ 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };
        const std::vector<std::string> distanceLabels{ tr("Low"), tr("Original"), tr("High"), tr("Very high"), tr("Extreme") };
        int d = -1;
        for (size_t i = 0; i < distances.size(); ++i)
            if (std::fabs(distances[i] - m_settings.drawDistance) < 0.01f) d = static_cast<int>(i);
        char customDistance[32];
        std::snprintf(customDistance, sizeof(customDistance), "%.2fx", m_settings.drawDistance);
        if (stepperRow(tr("Draw distance"), &d, distanceLabels, nullptr, d < 0 ? customDistance : nullptr))
            m_settings.drawDistance = distances[static_cast<size_t>(d)], commit();
        describe(m_game == GameId::Sly2 ? tr("How far away scenery, props and effects stay visible. Original matches the PS2; higher settings "
                                          "keep characters and gameplay at the PS2's distances.")
                                        : tr("How far away objects stay visible. Original matches the PS2."));
        // (settings v16 longShadows: Sly2-Release render.cpp FBuildShadow2, a PC improvement, not the original)
        if (m_game == GameId::Sly2)
            toggle(tr("Long shadows"), m_settings.longShadows,
                   tr("Character shadows from low lights reach the floor in full. Off: they end at a fixed length, as on the PS2."));
        // (settings v20 fastShadows: Sly2-Release display.h fFastShadows, a low-end option)
        if (m_game == GameId::Sly2)
            toggle(tr("Fast shadows"), m_settings.fastShadows,
                   tr("Character shadows with less GPU work, for low-end devices. The picture stays nearly the same."));
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
        // (settings v21 renderer: Sly2-Release renderer.h, platform/renderer_choice.h: Auto is Vulkan with OpenGL the
        // fallback; Linux and Windows, where the game has the Vulkan renderer)
        if (m_game == GameId::Sly2) {
            const std::vector<std::string> renderers{ tr("Auto (Vulkan)"), "Vulkan", "OpenGL" };
            int renderer = std::clamp(m_settings.renderer, 0, 2);
            if (stepperRow(tr("Renderer"), &renderer, renderers)) m_settings.renderer = renderer, commit();
            describe(tr("Auto uses Vulkan and switches to OpenGL by itself when the graphics driver cannot run Vulkan. After a "
                        "Vulkan start that failed, Auto stays on OpenGL until Vulkan is chosen here. Takes effect the next "
                        "time the game starts."));
        }
#elif defined(__ANDROID__)
        // (Sly2-Release renderer.cpp on Android: Auto is Vulkan, OpenGL ES when it cannot start, for VR, or after a
        // start that stopped in its first frames)
        if (m_game == GameId::Sly2) {
            const std::vector<std::string> renderers{ tr("Automatic"), "Vulkan", "OpenGL ES" };
            int renderer = std::clamp(m_settings.renderer, 0, 2);
            if (stepperRow(tr("Renderer"), &renderer, renderers)) m_settings.renderer = renderer, commit();
            describe(tr("Auto uses Vulkan, and OpenGL ES when this device cannot run it, in VR, or when the last start on "
                        "Vulkan did not finish. Vulkan tries it again. Takes effect the next time the game starts."));
        }
#endif
        if (m_game != GameId::Sly1) return;   // (Sly 2 has no fog option)
        const std::vector<std::string> fogs{ tr("PS2 style"), tr("PS3 style") };
        int fog = m_settings.fogType == settings::Fog_PS3 ? 1 : m_settings.fogType == settings::Fog_PS2 ? 0 : -1;
        if (stepperRow(tr("Fog"), &fog, fogs, nullptr, fog < 0 ? tr("Off") : nullptr))
            m_settings.fogType = fog == 1 ? settings::Fog_PS3 : settings::Fog_PS2, commit();
        describe(tr("PS2 style is the original look. PS3 style follows the remaster."));
    }
    // ---- (audio, notes/AUDIO_2026-11-10.md) Sly 2's devices by name (platform/audio_devices.h; Settings.ini [Audio]) and
    // the original's USB Headset option; the list is SDL2's (loaded at run time), read when the page is shown
    cane::audio::DeviceList m_audioDevices;
    double m_audioListed = -1.0;
    void audioDeviceRows() {
#if !defined(__ANDROID__) && !defined(__SWITCH__) && !defined(CANE_IOS)
        if (m_audioListed < 0 || ImGui::GetTime() - m_audioListed > 3.0) {
            m_audioDevices = cane::audio::listDevices();
            m_audioListed = ImGui::GetTime();
        }
        if (!m_audioDevices.available) return;
        auto deviceRow = [&](const char* label, std::string& field, const std::vector<std::string>& present, const char* defaultName,
                             const char* help) {
            const std::vector<std::string> choices = cane::audio::deviceChoices(present, field);
            std::vector<std::string> shown;
            int index = 0;
            // (a long name shortened so the row's arrows stay clear; the whole name is what is stored)
            auto shorten = [](const std::string& n) {
                if (n.size() <= 30) return n;
                size_t cut = 27;
                while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;
                return n.substr(0, cut) + "\u2026";
            };
            for (size_t i = 0; i < choices.size(); ++i) {
                if (choices[i].empty()) shown.push_back(tr(defaultName));
                else if (cane::audio::findDevice(present, choices[i]) < 0) shown.push_back(trf("%s (not connected)", shorten(choices[i]).c_str()));
                else shown.push_back(shorten(choices[i]));
                if (choices[i] == cane::audio::cleanName(field)) index = static_cast<int>(i);
            }
            if (stepperRow(label, &index, shown)) field = choices[static_cast<size_t>(index)], commit();
            describe(help);
        };
        deviceRow(tr("Output device"), m_sly2.game.outputDevice, m_audioDevices.outputs, N_("System default"),
                  tr("Where the game's sound plays. A chosen device that is unplugged plays on the system default until it is back."));
        toggle(tr("USB Headset"), m_sly2.game.usbHeadset,
               tr("The PS2's USB headset option. On: a loud shout into the microphone makes a noise nearby guards come to check, and "
                  "the dialogue plays on the headset output. The same as the in-game USB Headset option."));
        if (m_sly2.game.usbHeadset) {
            deviceRow(tr("Microphone"), m_sly2.game.inputDevice, m_audioDevices.inputs, N_("System default"),
                      tr("Listened to only while USB Headset is on."));
            deviceRow(tr("Headset output"), m_sly2.game.headsetOutputDevice, m_audioDevices.outputs, N_("Same as output"),
                      tr("Where USB Headset plays the dialogue. Same as output keeps it with the rest of the sound."));
        }
#endif
    }
    void audioRows() {
        if (m_game == GameId::Sly1) {
            const std::vector<std::string> speakers{ tr("Stereo"), tr("Mono") };
            int speaker = m_settings.stereoEnabled ? 0 : 1;
            if (stepperRow(tr("Speakers"), &speaker, speakers)) m_settings.stereoEnabled = speaker == 0, commit();
            describe(tr("Mono mixes both channels together, for a single speaker or one ear."));
            sly1LanguageRow();
        } else if (m_game == GameId::Sly2) {
            // (the original's Speakers option, Settings.ini; values settings::Sly2Speakers)
            const std::vector<std::string> speakers{ tr("Surround"), tr("Stereo"), tr("Mono") };
            static const int values[] = { settings::Speakers_Surround, settings::Speakers_Stereo, settings::Speakers_Mono };
            int speaker = m_sly2.game.speakers == settings::Speakers_Mono ? 2 : m_sly2.game.speakers == settings::Speakers_Stereo ? 1 : 0;
            if (stepperRow(tr("Speakers"), &speaker, speakers)) m_sly2.game.speakers = values[speaker], commit();
            describe(tr("Mono mixes both channels together, for a single speaker or one ear. Surround (the PS2's default) and "
                     "Stereo both play in stereo. The same as the in-game Speakers option."));
            languageRow(), voiceLanguageRow();
            audioDeviceRows();
        }
        auto volume = [&](const char* label, float& value, const char* help) {
            float v = value;
            if (sliderRow(label, &v)) value = v, m_volumeDirty = true;
            describe(help);
        };
        const bool sly2 = m_game == GameId::Sly2;
        volume(tr("Music"), m_settings.musicVolume, sly2 ? tr("Background music. The same as the in-game Music Volume.") : tr("Background music."));
        volume(tr("Sound effects"), m_settings.soundEffectsVolume,
               sly2 ? tr("Gadgets, footsteps, guards and the world around you. The same as the in-game Effects Volume.")
                    : tr("Gadgets, footsteps, guards and the world around you."));
        volume(tr("Dialogue"), m_settings.dialogueVolume, tr("Character voices and cutscenes."));
        if (m_volumeDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_volumeDirty = false, commit();
    }
    static constexpr std::array<const char*, settings::kBindingCount> actionNames{
        N_("Move up"), N_("Move down"), N_("Move left"), N_("Move right"), N_("Square"), N_("Circle"), N_("Cross"), N_("Triangle"),
        N_("Start"), N_("Select"), "L1", "R1", "L2", "R2", "L3", "R3" };
    static constexpr std::array<input::Button, settings::kBindingCount> actionButtons{
        input::Button::DpadUp, input::Button::DpadDown, input::Button::DpadLeft, input::Button::DpadRight,
        input::Button::West,   input::Button::East,     input::Button::South,    input::Button::North,
        input::Button::Start,  input::Button::Back,     input::Button::LB,       input::Button::RB,
        input::Button::LT,     input::Button::RT,       input::Button::LSPress,  input::Button::RSPress };
    void section(const char* title) {
        gap(10);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(16));
        overline(title);
        tighten(4);
    }
    // One binding row: the action (with its PlayStation glyph) and its key or button.
    void bindingRow(int action, bool controller, input::Family family) {
        ImGui::PushID(action + (controller ? 100 : 0));
        const bool advanced = action >= kAdvancedCapture;
        const int index = advanced ? action - kAdvancedCapture : action;
        const char* label = tr(advanced ? sly2input::kActions[size_t(index)].label : actionNames[size_t(index)]);
        const Row r = rowBehavior(label, px(52), false, 0.45f);
        if (r.visible) {
            const bool capturingThis = m_captureAction == action && m_captureController == controller;
            if (r.activated && !r.disabled && !capturingThis && ImGui::GetTime() > m_captureCooldown) startCapture(action, controller);
            auto* dl = ImGui::GetWindowDrawList();
            const float cy = r.bb.GetCenter().y;
            if (!advanced) drawPadButton(dl, { r.bb.Min.x + px(30), cy }, px(28), input::Family::PlayStation, actionButtons[size_t(index)]);
            dl->AddText(fonts().body, fonts().body->FontSize, { r.bb.Min.x + px(advanced ? 16 : 56), std::round(cy - fonts().body->FontSize * 0.5f) },
                        ink(color::text), label);
            recordLayout(label, r.bb);
            const int* bindings = advanced ? (controller ? m_actions.gamepad.data() : m_actions.keyboard.data())
                                          : controller ? m_settings.gamepadBindings : m_settings.keyboardBindings;
            const int value = bindings[index];
            int duplicate = -1;
            for (int other = 0; !advanced && other < settings::kBindingCount; ++other)
                if (other != action && value != settings::kKeyUnknown && bindings[other] == value) duplicate = other;
            const float right = r.control.Max.x;
            if (capturingThis) {
                const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
                const char* prompt = controller ? tr("Press a button…") : tr("Press a key or mouse button…");
                const auto t = measure(fonts().tab, prompt);
                dl->AddRect({ r.control.Min.x, r.bb.Min.y + px(8) }, { right, r.bb.Max.y - px(8) }, ink(color::cta, pulse), px(6), 0, px(2));
                dl->AddText(fonts().tab, fonts().tab->FontSize, { std::round(r.control.GetCenter().x - t.x * 0.5f), std::round(cy - t.y * 0.5f) },
                            ink(color::cta), prompt);
                m_help = m_captureNote.empty() ? (controller ? std::string(tr("Press the controller button for this action. Any button can be "
                                                                           "bound, so wait 6 seconds or press Escape to cancel."))
                                                             : family == input::Family::Keyboard
                                                                   ? std::string(tr("Press a key, mouse button, or scroll the wheel. Escape cancels."))
                                                                   : trf("Press a key, mouse button, or scroll the wheel. Escape or %s cancels.", tr(backButtonLabel())))
                                               : m_captureNote;
            } else if (controller) {
                const auto button = input::fromGlfwBinding(value);
                const char* label = input::buttonLabel(button, family);
                const auto t = measure(fonts().body, label);
                dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::text2), label);
                const float glyph = px(30);
                const float gw = padButtonWidth(glyph, family, button);
                if (button != input::Button::Count) drawPadButton(dl, { right - t.x - px(12) - gw * 0.5f, cy }, glyph, family, button);
            } else {
                const std::string name = keyName(value);
                const float h = px(30);
                const float w = keycapWidth(name.c_str(), h);
                if (value == settings::kKeyUnknown) {
                    const auto t = measure(fonts().body, tr("Unbound"));
                    dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::muted), tr("Unbound"));
                } else
                    drawKeycap(dl, { right - w, std::round(cy - h * 0.5f) - px(1) }, h, name.c_str());
            }
            if (duplicate >= 0 && !capturingThis) {
                drawIcon(dl, Icon::Warning, { r.control.Min.x + px(10), cy }, px(18), ink(color::warning));
                if (ImGui::IsItemFocused() || ImGui::IsItemHovered())
                    m_help = trf("Also used by %s. Both actions trigger together.", tr(actionNames[static_cast<size_t>(duplicate)]));
            }
            if (!capturingThis && m_help.empty() && (ImGui::IsItemFocused() || ImGui::IsItemHovered()))
                m_help = controller ? tr("Select, then press the controller button to use for this action.")
                                    : tr("Select, then press the key to use for this action. Escape cancels.");
        }
        ImGui::PopID();
    }
    static constexpr std::array<const char*, settings::kCameraKeyCount> cameraNames{
        N_("Camera left"), N_("Camera right"), N_("Camera up"), N_("Camera down") };
    // A keyboard camera (right stick) key (from ThiefRush-Dev's settings page).
    void cameraKeyRow(int index) {
        ImGui::PushID(200 + index);
        const char* label = tr(cameraNames[static_cast<size_t>(index)]);
        const Row r = rowBehavior(label, px(52), false, 0.45f);
        if (r.visible) {
            const int action = settings::kBindingCount + index;
            const bool capturingThis = m_captureAction == action && !m_captureController;
            if (r.activated && !r.disabled && !capturingThis && ImGui::GetTime() > m_captureCooldown) startCapture(action, false);
            auto* dl = ImGui::GetWindowDrawList();
            const float cy = r.bb.GetCenter().y;
            drawPadButton(dl, { r.bb.Min.x + px(30), cy }, px(28), input::Family::PlayStation, input::Button::RS);
            dl->AddText(fonts().body, fonts().body->FontSize, { r.bb.Min.x + px(56), std::round(cy - fonts().body->FontSize * 0.5f) },
                        ink(color::text), label);
            recordLayout(label, r.bb);
            const float right = r.control.Max.x;
            const int value = m_settings.cameraKeyBindings[index];
            if (capturingThis) {
                const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
                const auto t = measure(fonts().tab, tr("Press a key…"));
                dl->AddRect({ r.control.Min.x, r.bb.Min.y + px(8) }, { right, r.bb.Max.y - px(8) }, ink(color::cta, pulse), px(6), 0, px(2));
                dl->AddText(fonts().tab, fonts().tab->FontSize, { std::round(r.control.GetCenter().x - t.x * 0.5f), std::round(cy - t.y * 0.5f) },
                            ink(color::cta), tr("Press a key…"));
                m_help = m_captureNote.empty() ? std::string(tr("Press the key that turns the camera. Escape cancels.")) : m_captureNote;
            } else if (value == settings::kKeyUnknown) {
                const auto t = measure(fonts().body, tr("Unbound"));
                dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::muted), tr("Unbound"));
            } else {
                const std::string name = keyName(value);
                const float h = px(30), w = keycapWidth(name.c_str(), h);
                drawKeycap(dl, { right - w, std::round(cy - h * 0.5f) - px(1) }, h, name.c_str());
            }
            if (!capturingThis && (ImGui::IsItemFocused() || ImGui::IsItemHovered()))
                m_help = tr("Keyboard camera, like the right stick. In menus these keys also move the highlight.");
        }
        ImGui::PopID();
    }
    static std::vector<std::string> promptStyleNames() {
        std::vector<std::string> names;
        for (int style = 0; style <= input::kPromptStyleLast; ++style) names.emplace_back(tr(input::promptStyleName(style)));
        return names;
    }
    // The controller in use (or the first one connected): its picture, its name and how it is mapped. Pads the
    // controller database does not know are used with an Xbox layout (gamepad_mappings.h); the card says so.
    void controllerCard() {
        int jid = input::tracker().jid();
        if (jid < 0 || !glfwJoystickIsGamepad(jid))
            for (int j = GLFW_JOYSTICK_1; j <= GLFW_JOYSTICK_LAST && jid < 0; ++j)
                if (glfwJoystickPresent(j) && glfwJoystickIsGamepad(j) && !input::ignoredSteamDuplicate(j)) jid = j;
        if (jid >= 0 && !glfwJoystickIsGamepad(jid)) jid = -1;
        const input::PadInfo* pad = jid >= 0 ? &input::identify(jid) : nullptr;
        // (screenshots: PROJECTCANE_TEST_PROMPT_FAMILY stands for a connected pad of that family)
        const bool forced = input::tracker().forced() && input::tracker().family() != input::Family::Keyboard;
        std::string name, detail;
        input::Family family = input::Family::Keyboard;
        if (pad) {
            family = pad->family;
            name = !pad->gamepadName.empty() && pad->mapping == input::MappingSource::Builtin ? pad->gamepadName : pad->joystickName;
            switch (pad->mapping) {
            case input::MappingSource::Evdev:
            case input::MappingSource::Generic:
                detail = tr("Not in the controller database: used with an Xbox button layout."); break;
            case input::MappingSource::Database:
                detail = tr("Mapped from the controller database (another firmware version)."); break;
            default: detail = tr("Mapped from the controller database."); break;
            }
        } else if (forced) {
            family = input::tracker().family();
            name = input::familyDisplayName(family);
            detail = tr("Preview.");
        }
        const float h = px(112);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy({ width, h });
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, { at.x + width, at.y + h }, ink(color::surface), px(10));
        float textX = at.x + px(20);
        auto& atlas = input::promptAtlas();
        // (the original PlayStation family has no picture of its own: the DualShock 4's silhouette is the closest)
        const auto* device = atlas.deviceTexture() ? atlas.device(family == input::Family::PlayStation ? input::Family::PS4 : family) : nullptr;
        if (device) {
            const float k = (h - px(20)) / float(std::max(device->under.w, device->under.h)) ;
            const float w = device->under.w * k, hh = device->under.h * k;
            const ImVec2 min{ at.x + px(20), at.y + (h - hh) * 0.5f }, max{ min.x + w, min.y + hh };
            const auto tex = reinterpret_cast<ImTextureID>(static_cast<intptr_t>(atlas.deviceTexture()));
            const auto under = input::PromptAtlas::deviceUV(device->under), body = input::PromptAtlas::deviceUV(device->body);
            dl->AddImage(tex, min, max, { under.u0, under.v1 }, { under.u1, under.v0 }, ink(0x050913));
            dl->AddImage(tex, min, max, { body.u0, body.v1 }, { body.u1, body.v0 }, ink(color::text2));
            textX = max.x + px(24);
        }
        const float lineY = at.y + px(24);
        if (name.empty()) {
            dl->AddText(fonts().tab, fonts().tab->FontSize, { textX, lineY }, ink(color::text), tr("No controller connected"));
            dl->AddText(fonts().caption, fonts().caption->FontSize, { textX, lineY + px(30) }, ink(color::muted),
                        tr("Connect a controller: any pad works. Ones the controller database does not know use an Xbox layout."),
                        nullptr, at.x + width - textX - px(16));
            return;
        }
        dl->AddText(fonts().tab, fonts().tab->FontSize, { textX, lineY }, ink(color::text), name.c_str());
        const std::string kind = trf("%s buttons", input::familyDisplayName(family));
        dl->AddText(fonts().body, fonts().body->FontSize, { textX, lineY + px(28) }, ink(color::text2), kind.c_str());
        dl->AddText(fonts().caption, fonts().caption->FontSize, { textX, lineY + px(54) }, ink(color::muted), detail.c_str(),
                    nullptr, at.x + width - textX - px(16));
    }
    void controlsRows() {
        const bool advanced = m_game == GameId::Sly2 && m_actions.enabled;
        const auto family = input::promptFamily(promptStyle(), 0);
        section(tr("Controller in use"));
        controllerCard();
        if (m_game == GameId::Sly2) {
            ImGui::BeginDisabled(m_actions.readOnly);
            bool enabled = m_actions.enabled;
            if (toggleRow(tr("Advanced Input Mapping"), &enabled)) {
                m_actions.enabled = enabled;
                cancelCapture();
                commitActions();
            }
            describe(tr("Bind gameplay actions independently. Off restores your original button mapping without changing it."));
            ImGui::EndDisabled();
            if (m_actions.readOnly) caption(tr("The advanced controls file cannot be edited by this version."));
        }
        if (advanced) {
            section(tr("Action bindings"));
            const std::vector<std::string> devices{tr("Keyboard and mouse"), tr("Controller")};
            stepperRow(tr("Input device"), &m_actionDevice, devices);
            std::vector<std::string> groups;
            for (size_t i = 0; i < sly2input::kActionCount; ++i) {
                if (!m_debug && sly2input::isNoclipAction(sly2input::Action(i))) continue;
                const auto& action = sly2input::kActions[i];
                if (std::find(groups.begin(), groups.end(), action.group) == groups.end()) groups.emplace_back(action.group);
            }
            m_actionGroup = std::clamp(m_actionGroup, 0, int(groups.size()) - 1);
            std::vector<std::string> groupNames;
            for (const auto& g : groups) groupNames.push_back(tr(g));
            stepperRow(tr("Action group"), &m_actionGroup, groupNames);
            ImGui::BeginDisabled(m_actions.readOnly);
            for (size_t i = 0; i < sly2input::kActionCount; ++i)
                if (groups[size_t(m_actionGroup)] == sly2input::kActions[i].group)
                    bindingRow(kAdvancedCapture + int(i), m_actionDevice == 1, family);
            ImGui::EndDisabled();
            caption(tr("Bindings may be shared. Actions apply only in their gameplay context. Escape cancels capture. Delete clears a binding."));
        }
        toggle(tr("Disable controller input when unfocused"), m_settings.disableControllerInputWhenUnfocused,
               tr("Ignore the controller while another window has focus."));
        // (settings v15 controllerVibration: platform/pad_rumble.h; Sly 1 and Sly 2 drive the motors)
        if (m_game == GameId::Sly1)
            toggle(tr("Controller vibration"), m_settings.controllerVibration,
                   tr("The game's rumble on hits, explosions and scripted moments. The in-game Vibration option must also be on."));
        else if (m_game == GameId::Sly2)
            toggle(tr("Controller vibration"), m_settings.controllerVibration,
                   tr("The game's rumble on hits, explosions and scripted moments. The same as the in-game Vibrations option."));
        section(tr("Button prompts"));
        const std::vector<std::string> styles = promptStyleNames();
        int style = std::clamp(m_settings.promptStyle, 0, input::kPromptStyleLast);
        if (stepperRow(tr("Button icons"), &style, styles)) m_settings.promptStyle = style, commit();
        describe(tr("Which buttons the game's prompts show. Automatic follows the controller or keyboard you used last."));
        toggle(tr("Original PlayStation icons"), m_settings.alwaysOriginalPsPrompts,
               tr("Every device shows the game's original PlayStation 2 button icons. Off: PS4 / PS5 pads show their own icons, other devices theirs."));
        toggle(tr("Instant skip cutscenes"), m_settings.instantSkipCutscenes,
               tr("One press of Circle skips a cutscene. Off: hold Circle for 2 seconds."));
        if (m_game == GameId::Sly1 || m_game == GameId::Sly2) {
            const std::vector<std::string> cameraRotationLabels{ tr("Off"), tr("On") };
            int cameraRotation = m_settings.autoCameraRotation != 0 ? 1 : 0;
            if (stepperRow(tr("Automatic camera rotation"), &cameraRotation, cameraRotationLabels))
                m_settings.autoCameraRotation = cameraRotation, commit();
            describe(tr("Rotate the follow camera as you move."));
        }
        section(tr("Controller"));
        // (settings v17: Sly 2's controller camera inversion; off = the original's orbit camera)
        if (m_game == GameId::Sly2) {
            toggle(tr("Invert camera horizontal"), m_settings.padInvertCameraX,
                   tr("Off: the right stick orbits the camera as on the PS2 (right swings it around to the right, so the view turns left). On: right turns the view right."));
            toggle(tr("Invert camera vertical"), m_settings.padInvertCameraY,
                   tr("Off: as on the PS2, pushing the right stick up raises the camera to look down. On: up looks up."));
            // (the original's own options, Settings.ini; the PS2 kept them in each save)
            toggle(tr("Invert binocucom"), m_sly2.game.invertBinocucom,
                   tr("The controller in the binocucom. The same as the in-game Invert Binocucom Controls."));
            toggle(tr("Invert turret"), m_sly2.game.invertTurret,
                   tr("The controller on turrets. The same as the in-game Invert Turret Controls."));
        }
        if (!advanced) for (int i = 0; i < settings::kBindingCount; ++i) bindingRow(i, true, family);
        section(tr("Keyboard"));
        if (!advanced) for (int i = 0; i < settings::kBindingCount; ++i) bindingRow(i, false, family);
        for (int i = 0; i < settings::kCameraKeyCount; ++i) cameraKeyRow(i);
        ImGui::Indent(px(16));
        caption(tr("Escape also pauses and goes back, and Enter confirms in menus."));
        ImGui::Unindent(px(16));
        section(tr("Mouse"));
        toggle(tr("Mouse camera"), m_settings.mouseCamera,
               tr("While playing, the mouse turns the camera and aims the binocucom. Pause to free the cursor."));
        rangeRow(tr("Camera sensitivity"), m_settings.mouseSensitivity, 10, 500, 10, tr("How far the camera turns per mouse movement."), percent);
        rangeRow(tr("Aim sensitivity"), m_settings.mouseAimSensitivity, 10, 500, 10, tr("Binocucom aiming speed."), percent);
        if (m_game == GameId::Sly2) {
            // (settings v17: each view's own mouse inversion; off = natural everywhere)
            toggle(tr("Invert camera horizontal"), m_settings.mouseInvertX, tr("Off: moving the mouse right turns the camera right. On: it turns left."));
            toggle(tr("Invert camera vertical"), m_settings.mouseInvertY, tr("Off: moving the mouse up looks up. On: it looks down."));
            toggle(tr("Invert binocucom horizontal"), m_settings.mouseInvertBinocX, tr("Off: moving the mouse right looks right in the binocucom. On: it looks left."));
            toggle(tr("Invert binocucom vertical"), m_settings.mouseInvertBinocY, tr("Off: moving the mouse up looks up in the binocucom. On: it looks down."));
            toggle(tr("Invert turret horizontal"), m_settings.mouseInvertTurretX, tr("Off: moving the mouse right aims a turret right. On: it aims left."));
            toggle(tr("Invert turret vertical"), m_settings.mouseInvertTurretY, tr("Off: moving the mouse up aims a turret up. On: it aims down."));
        } else {
            toggle(tr("Invert horizontal"), m_settings.mouseInvertX, tr("Moving the mouse right turns the camera left."));
            toggle(tr("Invert aim vertical"), m_settings.mouseInvertY, tr("Moving the mouse up aims down in the binocucom."));
        }
        toggle(tr("Raw input"), m_settings.mouseRawInput, tr("Uses unaccelerated mouse motion where the system supports it."));
        if (m_game == GameId::Sly2)
            toggle(tr("Mouse wheel zoom"), m_settings.mouseWheelZoom, tr("Zoom the Binocucom and turrets with unassigned wheel inputs."));
        else
            toggle(tr("Wheel zooms binocucom"), m_settings.mouseWheelZoom, tr("When no action is bound to the wheel."));
        toggle(tr("Mouse in menus"), m_settings.mouseMenus, tr("Point at menu items, left click to choose and right click to go back."));
        if (m_game == GameId::Sly2)
            toggle(tr("Mouse aim in hacking"), m_settings.hackMouseAim,
                   tr("In Bentley's hacking games, the mouse moves a cursor the ship aims at and left click fires."));
        // (no Debug section: the debug keys are Settings > Advanced > Debug mode since 2026-11-04; settings v14
        //  debugKeys is ignored by the games and left as loaded)
        if (m_game != GameId::Sly1) return;
        section(tr("Set in each save"));
        ImGui::Indent(px(16));
        caption(tr("Camera, binocular, turret and jetpack inversion and vibration are saved with each game. "
                "Change them from the in-game Controls menu."));
        ImGui::Unindent(px(16));
    }
    void interfaceRows() {
        const std::vector<std::string> styles{ tr("PS2"), tr("Modern") };
        int style = std::clamp(m_settings.guiStyle, 0, 1);
        if (stepperRow(tr("Menu style"), &style, styles)) {
            m_settings.guiStyle = style;
            m_settings.guiScale = settings::coupledGuiScale(style, m_settings.guiScale);
            commit();
        }
        describe(tr("PS2 keeps the original menus. Modern uses cleaner, higher-resolution text."));
        const bool modern = m_settings.guiStyle == settings::Gui_Modern;
        const std::vector<float> scales = modern ? std::vector<float>{ 1.0f, 1.25f } : std::vector<float>{ 0.75f, 1.0f };
        const std::vector<std::string> labels = modern ? std::vector<std::string>{ "100%", "125%" } : std::vector<std::string>{ "75%", "100%" };
        int index = std::fabs(m_settings.guiScale - scales[0]) < 0.01f ? 0 : std::fabs(m_settings.guiScale - scales[1]) < 0.01f ? 1 : -1;
        if (stepperRow(tr("Menu size"), &index, labels)) m_settings.guiScale = scales[static_cast<size_t>(index)], commit();
        describe(tr("The size of menus and the HUD. The choices depend on the menu style."));
    }
    // The launcher's language (launcher.json "language": "system" or absent, or a launcher/lang code), at once.
    void languageSettingRow() {
        const auto langs = l10n::languages();
        const std::string setting = l10n::setting();
        const l10n::Table* system = l10n::find(l10n::systemLanguage());
        std::vector<std::string> names{ trf("System default (%s)", system ? system->language.name.c_str() : "English") };
        int index = 0;
        for (const auto& l : langs) {
            names.push_back(l.name == l.english || l.code == l10n::current() ? l.name : l.name + " · " + tr(l.english.c_str()));
            if (l.code == setting) index = static_cast<int>(names.size()) - 1;
        }
        if (stepperRow(tr("Language"), &index, names)) {
            const std::string next = index == 0 ? std::string("system") : langs[static_cast<size_t>(index - 1)].code;
            if (index == 0) m_backend.config.erase("language");
            else m_backend.config["language"] = next;
            persistLauncher();
            l10n::setLanguage(next);
        }
        describe(tr("The language of the launcher. System default follows your computer's language (English when the launcher does not "
                    "have it). The games' own language is in Settings > Audio."));
    }
    void launcherRows() {
        languageSettingRow();
        static const std::vector<float> scales{ 1.0f, 1.25f, 1.5f };
        static const std::vector<std::string> labels{ "100%", "125%", "150%" };
        const float current = m_backend.config.value("ui_scale", 1.0f);
        int index = 0;
        for (size_t i = 0; i < scales.size(); ++i)
            if (std::fabs(scales[i] - current) < 0.01f) index = static_cast<int>(i);
        if (stepperRow(tr("Launcher size"), &index, labels)) {
            m_backend.config["ui_scale"] = scales[static_cast<size_t>(index)];
            persistLauncher();
            if (setScale) setScale(scales[static_cast<size_t>(index)]);
        }
        describe(tr("Makes the launcher's text and controls larger, for TVs and handhelds."));
#if !defined(CANE_ANDROID) && !defined(CANE_IOS)  // (Android: the app fills its window; DeX / freeform windows are sized by the system)
        bool fullscreen = m_backend.config.value("start_fullscreen", defaultFullscreen());
        if (toggleRow(tr("Start in fullscreen"), &fullscreen)) {
            m_backend.config["start_fullscreen"] = fullscreen;
            persistLauncher();
        }
        describe(tr("Opens the launcher full screen. On by default on Steam Deck. Applies the next time it opens."));
#endif
        const std::vector<std::string> styles = promptStyleNames();
        int style = promptStyle();
        if (stepperRow(tr("Button prompts"), &style, styles)) {
            m_backend.config["prompt_style"] = style;
            if (style == 0) m_backend.config.erase("prompt_style");
            persistLauncher();
        }
        describe(tr("Which controller's buttons the launcher shows. Automatic follows the controller you used last."));
        if (updates) updates->settingsRows([this](const char* help) { describe(help); }, m_fixture);
        collectionRows();
    }
    // The Collection Style Launcher (launcher/collection/collection.h): The Sly Collection's (PS3) own menu. It exists
    // only with that menu imported from the player's disc (collection/ps3menu.h): until then the rows say so and lead
    // to the import; afterwards: start it, put it in Steam or on the desktop, import again, remove.
    void refreshCollection() {
        if (ImGui::GetTime() < m_nextCollectionCheck) return;
        m_nextCollectionCheck = ImGui::GetTime() + 1.0;
        m_collectionImported = !collectionProfile.empty() && collection::ps3menu::imported(collectionProfile);
        // a source another PS3 import used (the PS3 HD textures'), when it is still where it was
        m_collectionSource.clear();
        if (m_fixture) return;
        std::error_code ec;
        for (GameId g : {GameId::Sly2, GameId::Sly1}) {
            const auto pack = m_backend.ps3HdPack(g);
            if (m_collectionSource.empty() && !pack.source.empty() && fs::exists(path(pack.source), ec)) m_collectionSource = pack.source;
        }
    }
    void startCollectionImport(const std::string& source) {
        try {
            m_backend.config["ps3_browse_dir"] = utf8(path(source).parent_path());
            persistLauncher();
        } catch (...) {
        }
        const fs::path profile = collectionProfile;
        m_job.start(kCollectionImportLabel, [this, source, profile](const auto&) {
            {
                std::lock_guard lock(m_job.mutex);
                m_job.stage = "collection-menu";
            }
            collection::ps3menu::importMenu(path(source), profile, m_job.cancel, [this](float p) {
                std::lock_guard lock(m_job.mutex);
                m_job.progress = p;
            });
        });
    }
    void collectionRows() {
        refreshCollection();
        const bool importing = m_job.busy && m_job.label == kCollectionImportLabel;
        section(tr("Collection Style Launcher"));
        if (m_collectionImported) {
            if (linkRow(tr("Start the Collection Style Launcher"), nullptr, nullptr, Icon::Play) && !m_fixture && startCollection) startCollection();
#ifdef CANE_ANDROID
            describe(tr("The Sly Collection's (PS3) own menu, imported from your disc: turn the pages to a game and start it; quitting a game "
                        "brings you back to it."));
#else
            describe((std::string(tr("The Sly Collection's (PS3) own menu, imported from your disc: turn the pages to a game and start it; quitting a game "
                                     "brings you back to it.")) + " " + tr("Also starts with: safehouse --collection")).c_str());
#endif
        } else {
            ImGui::BeginDisabled(true);
            linkRow(tr("Start the Collection Style Launcher"), tr("Needs the import below"), nullptr, Icon::Play);
            ImGui::EndDisabled();
            describe(tr("This style is The Sly Collection's (PS3) own menu: its scene, art, sounds and music. None of that comes with "
                        "SafeHouse, so it has to be imported from your own Sly Collection disc first."));
        }
        // ---- the import (as the other imports from The Sly Collection)
        ImGui::BeginDisabled(m_job.busy || m_fixture || collectionProfile.empty());
        if (!m_collectionSource.empty() && !m_collectionImported) {
            if (linkRow(tr("Import the Collection menu from the source used before"), nullptr, nullptr, Icon::Disc)) startCollectionImport(m_collectionSource);
            describe(trf("From %s (your PS3 HD texture import's source).", m_collectionSource.c_str()).c_str());
        }
        if (linkRow(m_collectionImported ? tr("Import the Collection menu again…") : tr("Import the Collection menu from The Sly Collection (PS3)…"), tr(".iso / .zip"), nullptr,
                    Icon::Disc))
            startPs3Pick(GameId::Sly2, false, true);
        describe(tr("Choose The Sly Collection (PS3): its disc image (.iso) or a .zip of it, read as it is, nothing is unpacked. "
                    "The menu is converted into your profile (about 30 MB)."));
        if (linkRow(tr("Import from the extracted disc…"), tr("folder"), nullptr, Icon::Folder)) startPs3Pick(GameId::Sly2, true, true);
        describe(tr("Choose the extracted disc: the folder with PS3_GAME in it, PS3_GAME, or PS3_GAME/USRDIR."));
        if (m_collectionImported) {
            if (linkRow(tr("Remove the imported Collection menu"), nullptr, nullptr, Icon::Trash)) {
                collection::ps3menu::removeMenu(collectionProfile);
                m_nextCollectionCheck = 0;
            }
            describe(tr("Deletes the imported menu. The Collection Style Launcher needs it."));
        }
        ImGui::EndDisabled();
        if (importing) {
            float progress = 0;
            {
                std::lock_guard lock(m_job.mutex);
                progress = m_job.progress;
            }
            ImGui::Indent(px(16));
            char text[320];
            std::snprintf(text, sizeof(text), "%s · %d %%", tr("Reading The Sly Collection's menu"), int(progress * 100));
            caption(text, color::text2);
            if (button(tr("Cancel import"), Tone::Secondary, { px(190), 0 }, Icon::Close)) m_job.cancel = true;
            ImGui::Unindent(px(16));
        }
        // ---- shortcuts that open it (they open the SafeHouse launcher while the menu is not imported)
        ImGui::BeginDisabled(!m_collectionImported);
#ifdef CANE_ANDROID
        // Android: started as the app's direct activity (collection.h spawnDetached); a home-screen shortcut instead of
        // Steam / desktop shortcuts.
        if (linkRow(tr("Add a home screen shortcut for it"), nullptr, nullptr, Icon::Plus) && !m_fixture) {
            try {
                m_collectionMessage = android::pinShortcut("collection", shortcut::kCollectionTitle, shortcut::collectionAdaptiveIconImage(), {"--collection"})
                                          ? tr("Confirm the shortcut in the window that opens.")
                                          : tr("This device's home screen does not take shortcuts from apps.");
            } catch (const std::exception& e) {
                m_collectionMessage = trf("Could not add the shortcut: %s.", tr(std::string(e.what())).c_str());
            }
        }
        describe(m_collectionMessage.empty() ? tr("Starts the Collection Style Launcher directly.") : m_collectionMessage.c_str());
#else
        if (linkRow(tr("Add the Collection Style Launcher to Steam"), nullptr, nullptr, Icon::Plus) && !m_fixture) {
            try {
                const auto steam = steam::find();
                if (!steam) throw std::runtime_error(tr("Steam was not found on this computer"));
                const auto r = steam::addCollection(*steam);
                m_collectionMessage = (r.accounts == 1 ? std::string(tr("Added to your Steam library.")) : trf("Added to %d Steam libraries.", r.accounts)) +
                                      " " + (steam::running(*steam) ? tr("Restart Steam to see it.") : tr("It shows the next time Steam starts."));
            } catch (const std::exception& e) {
                m_collectionMessage = trf("Could not add the Steam shortcut: %s.", tr(std::string(e.what())).c_str());
            }
        }
        describe(tr("A Steam shortcut named \"Sly Collection\" that opens this menu (Steam's shortcut list is backed up first)."));
#ifndef __APPLE__
        if (linkRow(tr("Add a desktop shortcut for it"), nullptr, nullptr, Icon::Plus) && !m_fixture) {
            try {
                const auto r = shortcut::createCollection(shortcut::Where::Desktop);
                shortcut::createCollection(shortcut::Where::Menu);
                m_collectionMessage = trf("Shortcut created: %s (and in the %s).", shortcut::displayPath(r.file).c_str(), tr(shortcut::whereName(shortcut::Where::Menu)));
            } catch (const std::exception& e) {
                m_collectionMessage = trf("Could not create the shortcut: %s.", tr(std::string(e.what())).c_str());
            }
        }
        describe(m_collectionMessage.empty() ? tr("On the desktop and in the applications / Start menu.") : m_collectionMessage.c_str());
#endif
#endif
        ImGui::EndDisabled();
    }
    // Each installable game: its installed regions (choosing one makes it the one Play starts) and "Install another
    // region", with the regions it installs from and the space each needs.
    void gamesRows(bool gameRunning) {
#if defined(CANE_ANDROID) || defined(CANE_IOS)
        {
            // (backend.h install: the app's copy of a picked disc image is removed after a successful install unless this is on)
            bool keep = m_backend.config.value("keep_imported_iso", false);
            if (toggleRow(tr("Keep imported disc images"), &keep)) {
                m_backend.config["keep_imported_iso"] = keep;
                persistLauncher();
            }
            describe(tr("Off: the app's copy of a disc image you picked is deleted after the game installs (it needs about 3 GB). "
                        "Re-extracting after an update then asks for the disc image again."));
        }
#endif
        if (ImGui::GetTime() >= m_nextLocation) {  // (fixtures: the throwaway profile's default folder, no drives)
            m_nextLocation = ImGui::GetTime() + 1.0;
            m_installRoot = m_backend.installRoot();
            m_customRoot = m_backend.customInstallRoot();
            m_rootFree = freeBytes(m_installRoot);
            if (!m_fixture) m_places = otherDrives();
            else m_installRoot = "/home/player/.local/share/SafeHouse/installations", m_rootFree = 212ull << 30;
        }
        if (!m_fixture || m_regionsFixture) {
            const double now = ImGui::GetTime();
            if (!m_regionsFixture && now >= m_nextRegions) {
                m_nextRegions = now + 1.0;
                for (GameId g : { GameId::Sly1, GameId::Sly2, GameId::Sly3 }) {
                    m_installs[static_cast<size_t>(g)] = m_backend.installations(g);
                    m_selected[static_cast<size_t>(g)] = m_backend.selectedRegion(g);
                    m_ps3Packs[static_cast<size_t>(g)] = m_backend.ps3HdPack(g);
                }
            }
        }
        installLocationRows(gameRunning);
        for (GameId g : { GameId::Sly1, GameId::Sly2, GameId::Sly3 }) {
            ImGui::PushID(static_cast<int>(g));
            section(cane::game(g).title);
            const auto& installs = m_installs[static_cast<size_t>(g)];
            const auto& selected = m_selected[static_cast<size_t>(g)];
            for (const auto& in : installs) {
                const std::string state = in.outdated ? tr("Update needed") : !in.current ? tr("Needs repair")
                                          : installs.size() > 1 && in.region == selected ? tr("Plays · Ready") : tr("Ready");
                ImGui::PushID(in.region.c_str());
                const bool chosen = linkRow(in.name().c_str(), state.c_str(), utf8(in.dir).c_str(), installs.size() > 1 ? Icon::ChevronRight : Icon::None);
                ImGui::PopID();
#ifdef CANE_ANDROID
                describe(installs.size() > 1 ? tr("Choose this region: Play, Verify and shortcuts use it.") : tr("The installed region."));
#else
                describe(installs.size() > 1 ? tr("Choose this region: Play, Verify and Add to Steam use it.") : tr("The installed region."));
#endif
                if (chosen && installs.size() > 1 && !gameRunning && !m_fixture) {
                    try {
                        m_backend.setSelectedRegion(g, in.region);
                    } catch (...) {
                    }
                    m_nextRegions = 0;
                }
                moveRow(g, in, gameRunning);
            }
            if (installs.empty()) {
                ImGui::Indent(px(16));
                caption(tr("Not installed."));
                ImGui::Unindent(px(16));
            }
            ImGui::BeginDisabled(m_job.busy || gameRunning);
            const std::string label = installs.empty() ? trf("Install %s", cane::game(g).shortName) : std::string(tr("Install another region"));
            if (linkRow(label.c_str(), nullptr, nullptr, Icon::Plus) && installRegion) installRegion(g);
            ImGui::EndDisabled();
            describe(tr("Opens the install page for a disc image of another region. The installed regions stay."));
            std::string regions, later;
            for (const auto& r : knownRegions(g)) {
                if (r.supported) regions += (regions.empty() ? "" : " · ") + std::string(tr(r.name)) + " " + aboutGb(r.needBytes);
                else later += (later.empty() ? "" : ", ") + std::string(tr(r.name));
            }
            ImGui::Indent(px(16));
            caption((later.empty() ? trf("Installs from: %s.", regions.c_str()) : trf("Installs from: %s. Recognized, not supported yet: %s.", regions.c_str(), later.c_str())).c_str());
            ImGui::Unindent(px(16));
            if (LauncherBackend::ps3HdGame(g) && !installs.empty()) ps3HdRows(g, gameRunning);
            if (g == GameId::Sly2 && !installs.empty()) languagesRows(gameRunning);
            ImGui::PopID();
        }
    }
    // "Install games to" (backend.h installRoot): the default folder, the other drives (install_location.h) or any
    // folder from the system's picker. New installations go there; installed games stay where they are (moveRow).
    void installLocationRows(bool gameRunning) {
        section(tr("Install location"));
        const auto same = [](const fs::path& a, const fs::path& b) { return folders::detail::comparable(a) == folders::detail::comparable(b); };
        std::vector<std::string> labels{ tr("Default folder") };
        std::vector<fs::path> choices{ fs::path{} };
        int index = m_customRoot ? -1 : 0;
        for (const auto& place : m_places) {
            labels.push_back(place.label);
            choices.push_back(place.folder);
            if (m_customRoot && same(m_backend.installRootFor(place.folder), m_installRoot)) index = static_cast<int>(labels.size()) - 1;
        }
        if (index < 0) {
            labels.push_back(utf8(m_installRoot.parent_path().filename().empty() ? m_installRoot : m_installRoot.parent_path().filename()));
            choices.push_back(m_installRoot);
            index = static_cast<int>(labels.size()) - 1;
        }
        ImGui::BeginDisabled(m_job.busy || gameRunning);
        const std::string where = utf8(m_installRoot) + " · " + freeText(m_rootFree);
        if (stepperRow(tr("Install games to"), &index, labels, where.c_str(), nullptr, px(72)) && !m_fixture) {
            setInstallRoot(choices[static_cast<size_t>(index)]);
        }
        describe(tr("Where new installations go: the default folder or another drive (a SafeHouse folder is made there). "
                 "Installed games stay where they are; \"Move … to the install folder\" below moves one."));
        if (linkRow(tr("Choose a folder…"), nullptr, nullptr, Icon::Folder) && !m_fixture) startInstallPick();
        describe(tr("Any folder, with the system's folder picker. Games install into a SafeHouse folder inside it."));
        if (m_customRoot && linkRow(tr("Use the default folder"), nullptr, utf8(m_backend.defaultInstallRoot()).c_str(), Icon::Refresh) && !m_fixture)
            setInstallRoot({});
        if (m_customRoot) describe(tr("New installations go to the profile folder again, as before. Installed games stay where they are."));
        ImGui::EndDisabled();
        if (!m_locationNote.empty()) {
            ImGui::Indent(px(16));
            caption(m_locationNote.c_str(), color::warning);
            ImGui::Unindent(px(16));
        }
    }
    void setInstallRoot(const fs::path& chosen) {
        try {
            m_backend.setInstallRoot(chosen);
            m_locationNote.clear();
        } catch (const std::exception& e) {
            m_locationNote = e.what();
        }
        m_nextRegions = 0;
        m_nextLocation = 0;
    }
    // Under an installation outside the install folder: "Move <region> to the install folder" (backend.h moveInstallation, a job with progress).
    void moveRow(GameId g, const Installation& in, bool gameRunning) {
        const auto same = [](const fs::path& a, const fs::path& b) { return folders::detail::comparable(a) == folders::detail::comparable(b); };
        if (same(in.dir.parent_path(), m_installRoot)) return;
        const std::string label = trf("Move (%s %s)", cane::game(g).shortName, tr(in.name()).c_str());
        const bool moving = m_job.busy && m_job.label == label;
        ImGui::PushID(("move-" + in.region).c_str());
        ImGui::BeginDisabled(m_job.busy || gameRunning);
        const std::string title = trf("Move %s to the install folder", tr(in.name()).c_str());
        const std::string sub = utf8(m_installRoot) + " · " + freeText(m_rootFree);
        if (linkRow(title.c_str(), nullptr, sub.c_str(), Icon::ChevronRight) && !m_fixture) {
            const std::string region = in.region;
            const fs::path root = m_installRoot;
            m_job.start(label, [this, g, region, root](const auto& log) { m_backend.moveInstallation(g, region, root, m_job.cancel, log); });
        }
        ImGui::EndDisabled();
        describe(tr("Moves the installed files to the folder chosen above. On another drive they are copied and checked first; "
                 "the old folder is removed only when the copy is complete. Saves stay where they are."));
        if (moving) {
            float progress = 0;
            std::string stage;
            {
                std::lock_guard lock(m_job.mutex);
                stage = stageLabel(m_job.stage);
                progress = m_job.progress;
            }
            ImGui::Indent(px(16));
            char text[200];
            std::snprintf(text, sizeof(text), "%s · %d %%", stage.empty() ? tr("Moving") : tr(stage).c_str(), int(progress * 100));
            caption(text, color::text2);
            if (button(tr("Cancel move"), Tone::Secondary, { px(190), 0 }, Icon::Close)) m_job.cancel = true;
            ImGui::Unindent(px(16));
        }
        ImGui::PopID();
    }
    void startInstallPick() {
        if (m_installDialog.running()) return;
        const fs::path start = filedialog::startFolder({ utf8(m_installRoot.parent_path()) });
        if (!m_installDialog.start(filedialog::Kind::PickFolder, tr("Choose where SafeHouse installs games"), start)) openInstallPicker();
    }
    void openInstallPicker() {
        ImGuiFileDialog::Instance()->OpenDialog("installdir", tr("Choose where SafeHouse installs games"), nullptr,
                                                utf8(filedialog::startFolder({ utf8(m_installRoot.parent_path()) })), "", 1, nullptr,
                                                ImGuiFileDialogFlags_Modal);
    }
    // The PS3 HD textures of a game (backend.h importPs3Hd): on / off, import from The Sly Collection (a disc image,
    // a .zip of one, or the extracted disc), with the job's progress and Cancel, and Remove. The cutscenes stay the
    // PS2's (the PS3 movies are not sharper), which a note says.
    void ps3HdRows(GameId g, bool gameRunning) {
        const size_t gi = static_cast<size_t>(g);
        const auto& pack = m_ps3Packs[gi];
        gap(6);
        ImGui::Indent(px(16));
        field(tr("PS3 HD textures"));
        ImGui::Unindent(px(16));
        const std::string label = trf("PS3 HD import (%s)", cane::game(g).shortName);
        const bool importing = m_job.busy && m_job.label == label;
        ImGui::BeginDisabled(m_job.busy || gameRunning || m_fixture);
        if (pack.installed) {
            bool on = pack.enabled;
            char sub[160];
            const std::string subText = pack.date.empty() ? trf("%d textures from The Sly Collection (PS3), %.0f MB", pack.textures, pack.megabytes)
                                                          : trf("%d textures from The Sly Collection (PS3), %.0f MB, imported %s", pack.textures, pack.megabytes, pack.date.c_str());
            std::snprintf(sub, sizeof(sub), "%s%s%s", subText.c_str(), pack.png ? " · " : "", pack.png ? tr("PNG files") : "");
            if (toggleRow(tr("Use the PS3 HD textures"), &on, sub)) {
                try {
                    m_backend.setPs3HdEnabled(g, on);
                } catch (...) {
                }
                m_nextRegions = 0;
            }
            describe(tr("The redrawn textures of The Sly Collection (PS3), up to 4x sharper, over the original ones. The levels, "
                     "the gameplay and your Mods stay as they are; your own Mods/Textures win. Applies the next time the game starts."));
        }
        bool png = m_backend.ps3HdPng(g);
        if (toggleRow(tr("Extract as PNG files"), &png)) {
            try {
                m_backend.setPs3HdPng(g, png);
            } catch (...) {
            }
        }
        describe(tr("The next import writes the textures as PNG files in a Textures folder, to look through or edit, instead of "
                 "one Textures.pak file. The game reads either; the one file loads faster."));
        if (linkRow(pack.installed ? tr("Import again from a disc image…") : tr("Import PS3 HD textures (The Sly Collection)…"), tr(".iso / .zip"), nullptr, Icon::Disc))
            startPs3Pick(g, false);
        describe(tr("Choose The Sly Collection (PS3): its disc image (.iso) or a .zip of it, read as it is, nothing is unpacked. "
                 "SafeHouse then matches its textures to your installed game: Sly 1 about 2 minutes, Sly 2 about 6."));
        if (linkRow(tr("Import from the extracted disc…"), "folder", nullptr, Icon::Folder)) startPs3Pick(g, true);
        describe(tr("Choose the extracted disc: the folder with PS3_GAME in it, PS3_GAME, or PS3_GAME/USRDIR."));
        if (pack.installed) {
            if (linkRow(tr("Remove the PS3 HD textures"), nullptr, nullptr, Icon::Trash)) {
                try {
                    m_backend.removePs3Hd(g);
                } catch (...) {
                }
                m_nextRegions = 0;
            }
            describe(tr("Deletes the imported pack. The game's own textures are not touched."));
        }
        ImGui::EndDisabled();
        if (importing) {
            std::string stage;
            float progress = 0;
            {
                std::lock_guard lock(m_job.mutex);
                stage = stageLabel(m_job.stage);
                if (!m_job.item.empty()) stage += " · " + m_job.item;
                progress = m_job.progress;
            }
            ImGui::Indent(px(16));
            char text[320];
            std::snprintf(text, sizeof(text), "%s · %d %%", stage.empty() ? tr("Starting") : tr(stage).c_str(), int(progress * 100));
            caption(text, color::text2);
            if (button(tr("Cancel import"), Tone::Secondary, { px(190), 0 }, Icon::Close)) m_job.cancel = true;
            ImGui::Unindent(px(16));
        }
        ImGui::Indent(px(16));
        caption(g == GameId::Sly1
                    ? tr("Cutscenes stay the PS2's: the PS3 movies are the same 640x480 picture at 30 fps, re-encoded in Bink, "
                      "so they would not be sharper.")
                    : tr("Cutscenes stay the PS2's: the PS3 movies are the same 640x480 picture, re-encoded in Bink, so they "
                      "would not be sharper."));
        ImGui::Unindent(px(16));
    }
    // ---- Sly 2's languages (backend.h sly2Languages; Sly2-Release/langstore.h)
    void refreshLanguages() {
        if (m_fixture && m_langsFixture) return;
        const double now = ImGui::GetTime();
        if (now < m_nextLangs && !m_langsDirty) return;
        m_nextLangs = now + 2.0;
        m_langsDirty = false;
        m_langs = m_backend.sly2Languages();
        m_ownLangs = m_backend.sly2InstallationsWithOwnLanguages();
        m_ps3Missing = m_backend.ps3MissingLanguages();
        m_langRegion = m_backend.selectedRegion(GameId::Sly2);
        m_japaneseFontMissing = m_backend.japaneseFontMissing();
        m_japaneseFontInstalled = m_backend.japaneseFontSource().has_value();
    }
    // How a language plays on the selected region's disc (its text needs that disc's fonts)
    std::string languageNote(const LauncherBackend::LanguageInfo& l) const {
        if (l.wide && !l.font && m_langRegion != "japan") return tr("its voices, English text on this disc");
        if (!l.wide && m_langRegion == "japan" && l.key != "English" && l.key.rfind("English", 0) != 0) return tr("text without accents on this disc");
        if (!l.voice) return tr("subtitles, English voices");
        return "";
    }
    // Sly 1: the installed disc's languages (PAL: five; Japan: Japanese text with Japanese or English voices), the
    // game's own choice (content_settings.json, backend.h sly1Languages); hidden without an installation.
    void sly1LanguageRow() {
        if (!m_fixture || !m_sly1LangsFixture) {
            const double now = ImGui::GetTime();
            if (now >= m_nextSly1Langs) m_sly1Langs = m_backend.sly1Languages(), m_nextSly1Langs = now + 2.0;
        }
        const auto& l = m_sly1Langs;
        if (l.serial.empty()) return;
        std::vector<std::string> names;
        int index = 0;
        for (const auto& key : l.languages) {
            if (key == l.selected) index = static_cast<int>(names.size());
            names.push_back(tr(key));
        }
        const bool voiceOnly = std::find(l.voiceOnly.begin(), l.voiceOnly.end(), l.languages[size_t(index)]) != l.voiceOnly.end();
        if (stepperRow(tr("Language"), &index, names, voiceOnly ? tr("voices only: the text stays in the disc's language") : nullptr) &&
            !m_fixture) {
            try {
                m_backend.setSly1Language(l.serial, l.languages[size_t(index)]);
                m_sly1Langs.selected = l.languages[size_t(index)];
                m_saveOk = true;
            } catch (...) {
                m_saveOk = false;
            }
            m_savedAt = ImGui::GetTime();
        }
        describe(l.languages.size() > 1 ? tr("The game's text and voices, from the languages on your installed disc. The same as the "
                                             "game's own language choice; applies the next time the game starts.")
                                        : tr("Only your installed disc's language is available."));
    }
    // Settings.ini [Language]: Automatic or one of the languages
    void languageRow() {
        refreshLanguages();
        std::vector<std::string> names{ tr("Automatic") };
        int index = 0;
        for (const auto& l : m_langs) {
            names.push_back(tr(l.key));
            if (l.key == m_sly2.game.language) index = int(names.size()) - 1;
        }
        if (!m_sly2.game.language.empty() && index == 0) names.push_back(trf("%s (not installed)", tr(m_sly2.game.language).c_str())), index = int(names.size()) - 1;
        std::string sub;
        if (index > 0 && size_t(index) <= m_langs.size()) sub = languageNote(m_langs[size_t(index) - 1]);
        if (stepperRow(tr("Language"), &index, names, sub.empty() ? nullptr : sub.c_str())) {
            m_sly2.game.language = index == 0 || size_t(index) > m_langs.size() ? std::string() : m_langs[size_t(index) - 1].key;
            commit();
        }
        describe(m_langs.size() > 1
                     ? tr("The game's text, voices and cutscenes. Automatic: your computer's language, and the title screen asks (Triangle "
                       "opens the list again). More languages: Settings > Games > Sly 2 > Languages.")
                     : tr("Only your installed disc's language is available. Import other regions' languages in Settings > Games > Sly 2 > Languages."));
    }
    // Settings.ini [Language] Voices: Same as text, or a language with voices (the disc's own, or the folder's with
    // Dialog lines: not a subtitle-only language such as the PS3 import's)
    void voiceLanguageRow() {
        refreshLanguages();
        std::vector<std::string> names{ tr("Same as text") };
        std::vector<std::string> keys{ std::string() };
        int index = 0;
        for (const auto& l : m_langs) {
            if (!l.voice) continue;
            keys.push_back(l.key);
            names.push_back(tr(l.key));
            if (l.key == m_sly2.game.voiceLanguage) index = int(names.size()) - 1;
        }
        if (!m_sly2.game.voiceLanguage.empty() && index == 0)
            names.push_back(trf("%s (not installed)", tr(m_sly2.game.voiceLanguage).c_str())), keys.push_back(m_sly2.game.voiceLanguage),
                index = int(names.size()) - 1;
        if (keys.size() <= 2 && m_sly2.game.voiceLanguage.empty()) return;   // (one language with voices: nothing to choose)
        if (stepperRow(tr("Voice language"), &index, names)) {
            m_sly2.game.voiceLanguage = size_t(index) < keys.size() ? keys[size_t(index)] : std::string();
            commit();
        }
        describe(tr("The dialogue and the cutscenes' sound, apart from the text: Japanese voices with English text, or the other way "
                 "round. Choosing a language on the game's title screen sets both back to it."));
    }
    void languagesRows(bool gameRunning) {
        refreshLanguages();
        gap(6);
        ImGui::Indent(px(16));
        field(tr("Languages"));
        std::string list;
        for (const auto& l : m_langs) {
            const std::string note = languageNote(l);
            list += (list.empty() ? "" : " · ") + l.key + (note.empty() ? "" : " (" + note + ")");
        }
        caption(list.empty() ? tr("None found.") : list.c_str(), color::text2);
        ImGui::Unindent(px(16));
        const bool importing = m_job.busy && m_job.label.rfind("Languages", 0) == 0;
        ImGui::BeginDisabled(m_job.busy || gameRunning || m_fixture);
        for (const auto& in : m_ownLangs) {
            ImGui::PushID(in.region.c_str());
            const std::string label = trf("Share the %s languages", tr(in.name()).c_str());
            if (linkRow(label.c_str(), nullptr, nullptr, Icon::ChevronRight)) {
                const std::string region = in.region;
                m_job.start("Languages (" + in.name() + ")", [this, region](const auto& log) { m_backend.shareInstallationLanguages(region, m_job.cancel, log); });
                m_langsDirty = true;
            }
            ImGui::PopID();
            describe(tr("This installation keeps its languages in its own folder (installed with an older SafeHouse). Moves them into "
                     "the shared languages folder, so every installed region offers them; nothing is copied twice."));
        }
        if (m_japaneseFontMissing) {
            // (Japanese imported before its font was taken along: voices only on the USA / European discs until then)
            if (m_japaneseFontInstalled) {
                if (linkRow(tr("Add the Japanese text"), nullptr, nullptr, Icon::ChevronRight)) {
                    m_job.start("Languages (Japanese text)", [this](const auto& log) { m_backend.addJapaneseFont(m_job.cancel, log); });
                    m_langsDirty = true;
                }
                describe(tr("Japanese plays only its voices on the USA and European discs until the characters of your Japanese "
                         "disc are added. They are read from your Japan installation; a few seconds."));
            } else {
                caption(tr("Japanese plays only its voices on the USA and European discs until the characters of your Japanese "
                           "disc are added: choose your Japan disc image below once more."), color::text2);
            }
        }
        if (linkRow(tr("Import languages from a disc image…"), ".iso", nullptr, Icon::Disc)) startLanguagePick(LangPick::Disc);
        describe(tr("Adds another region's languages without installing it: Europe has English (UK), French, German, Spanish, "
                 "Italian, Dutch, Portuguese, Danish, Finnish, Norwegian and Swedish (text, voices and cutscenes), Japan has "
                 "Japanese. About 1 minute; Europe needs about 4.5 GB."));
        if (!m_ps3Missing.empty()) {
            std::string what;
            for (const auto& k : m_ps3Missing) what += (what.empty() ? std::string() : std::string(tr(" and "))) + tr(k);
            const std::string label = trf("Import %s from The Sly Collection (PS3)…", what.c_str());
            if (linkRow(label.c_str(), tr(".iso / .zip"), nullptr, Icon::Disc)) startLanguagePick(LangPick::Ps3Image);
            if (linkRow(tr("Import them from the extracted disc…"), tr("folder"), nullptr, Icon::Folder)) startLanguagePick(LangPick::Ps3Folder);
            describe(m_ps3Packs[static_cast<size_t>(GameId::Sly2)].installed
                         ? tr("Your Sly Collection (PS3) also has these languages, which your installation lacks: their text, as "
                           "subtitles over the English voices (the PS3's voice files cannot be matched to the lines).")
                         : tr("The Sly Collection (PS3) has French and Spanish: their text, as subtitles over the English voices."));
        }
        if (linkRow(tr("Languages folder"), nullptr, nullptr, Icon::Folder)) {
            std::error_code ec;
            fs::create_directories(m_backend.languagesDir(GameId::Sly2), ec);
            openSafe(m_backend.languagesDir(GameId::Sly2));
        }
        describe(tr("One folder for every region. Add your own language there (or in Mods/Languages): a folder with Text.json, "
                 "and Dialog / Movies for a dub. MODDING.md explains the files."));
        ImGui::EndDisabled();
        if (importing) {
            std::string stage;
            float progress = 0;
            {
                std::lock_guard lock(m_job.mutex);
                stage = stageLabel(m_job.stage);
                if (!m_job.item.empty()) stage += " · " + m_job.item;
                progress = m_job.progress;
            }
            ImGui::Indent(px(16));
            char text[320];
            std::snprintf(text, sizeof(text), "%s · %d %%", stage.empty() ? tr("Starting") : tr(stage).c_str(), int(progress * 100));
            caption(text, color::text2);
            if (button(tr("Cancel import"), Tone::Secondary, { px(190), 0 }, Icon::Close)) m_job.cancel = true;
            ImGui::Unindent(px(16));
        }
    }
    enum class LangPick { Disc, Ps3Image, Ps3Folder };
    void startLanguagePick(LangPick pick) {
        m_langPick = pick;
        if (m_langDialog.running()) return;
        const char* title = pick == LangPick::Disc ? tr("Choose a Sly 2 disc image (Europe or Japan)")
                            : pick == LangPick::Ps3Image ? tr("Choose The Sly Collection (PS3) disc image") : tr("Choose the extracted Sly Collection (PS3)");
        const auto kind = pick == LangPick::Disc ? filedialog::Kind::OpenIso : pick == LangPick::Ps3Image ? filedialog::Kind::OpenPs3 : filedialog::Kind::PickFolder;
        const fs::path start = filedialog::startFolder({ m_backend.config.value(pick == LangPick::Disc ? "browse_dir" : "ps3_browse_dir", std::string{}) });
        if (!m_langDialog.start(kind, title, start)) openLanguagePicker();
    }
    void openLanguagePicker() {
        const bool folder = m_langPick == LangPick::Ps3Folder;
        ImGuiFileDialog::Instance()->OpenDialog("langsrc", folder ? tr("Choose the extracted Sly Collection (PS3)") : tr("Choose a disc image"),
                                                folder ? nullptr : m_langPick == LangPick::Disc ? ".iso,.ISO" : ".iso,.zip,.ISO,.ZIP",
                                                utf8(filedialog::startFolder({ m_backend.config.value("browse_dir", std::string{}) })), "", 1,
                                                nullptr, ImGuiFileDialogFlags_Modal);
    }
    void startLanguageImport(const std::string& source) {
        m_langsDirty = true;
        if (m_langPick == LangPick::Disc)
            m_job.start("Languages (disc image)", [this, source](const auto& log) { m_backend.importLanguagesFromDisc(source, m_job.cancel, log); });
        else
            m_job.start("Languages (The Sly Collection)", [this, source](const auto& log) { m_backend.importPs3Languages(source, m_job.cancel, log); });
    }
    void startPs3Pick(GameId g, bool folder, bool forCollection = false) {
        if (m_ps3Dialog.running()) return;
        m_ps3Game = g;
        m_ps3ForCollection = forCollection;
        m_ps3PickFolder = folder;
        const char* title = folder ? tr("Choose the extracted Sly Collection (PS3)") : tr("Choose The Sly Collection (PS3) disc image");
        const fs::path start = filedialog::startFolder({ m_backend.config.value("ps3_browse_dir", std::string{}) });
        if (m_ps3Dialog.running()) return;
        if (!m_ps3Dialog.start(folder ? filedialog::Kind::PickFolder : filedialog::Kind::OpenPs3, title, start))
            openPs3Picker(folder);
    }
    void openPs3Picker(bool folder) {
        m_ps3PickFolder = folder;
        ImGuiFileDialog::Instance()->OpenDialog("ps3src", folder ? tr("Choose the extracted Sly Collection (PS3)") : tr("Choose The Sly Collection (PS3) disc image"),
                                                folder ? nullptr : ".iso,.zip,.ISO,.ZIP",
                                                utf8(filedialog::startFolder({ m_backend.config.value("ps3_browse_dir", std::string{}) })), "", 1,
                                                nullptr, ImGuiFileDialogFlags_Modal);
    }
    void startPs3Import(GameId g, const std::string& source) {
        try {
            m_backend.config["ps3_browse_dir"] = utf8(path(source).parent_path());
            persistLauncher();
        } catch (...) {
        }
        const std::string label = trf("PS3 HD import (%s)", cane::game(g).shortName);
        const bool png = m_backend.ps3HdPng(g);
        m_job.start(label, [this, g, source, png](const auto& log) { m_backend.importPs3Hd(g, source, png, m_job.cancel, log); });
    }
    void advancedRows(bool gameRunning) {
        bool debug = m_debug;
        if (toggleRow(tr("Debug mode"), &debug)) {
            m_debug = debug;
            writeDebugMode(debug);
        }
        // (one switch for everything debug: Debug Mode.ini, cane::debugMode(); the games' debug keys, Sly 2's debug menu
        //  and Extras, Sly 1's developer debug mode. Backend::play copies it to Sly 2's / Sly 3's own user folder.)
        describe(m_game == GameId::Sly2
                     ? tr("Turns on the debug keys and the debug menu (F10 / R1+Select) with Extras. F1 free camera, "
                       "F2 Paris, F3 next world, F5 save, F6 unlock all, F7 infinite health, F9 finish the job. "
                       "For testing or reporting a problem.")
                     : m_game == GameId::Sly1
                     ? tr("Turns on the debug keys (F3 opens the debug window) plus the game's debug features. "
                       "For testing or reporting a problem.")
                     : tr("Turns on the debug keys plus the game's debug features. For testing or reporting a problem."));
        bool save = logging ? logging() : false;
        if (toggleRow(tr("Save logs"), &save) && setLogging && !m_fixture) setLogging(save);
        describe(tr("Off by default. When on, launcher and game output is saved to launcher.log in the log folder, for bug reports."));
        bool logs = showLogs ? *showLogs : false;
        if (toggleRow(tr("Show activity log"), &logs)) {
            if (showLogs) *showLogs = logs;
            m_backend.config["show_logs"] = logs;
            persistLauncher();
        }
        describe(tr("Shows the install and build log drawer. The View button also opens it."));
        if (linkRow(tr("Save folder"), nullptr, nullptr, Icon::Folder)) openSafe(m_backend.userDir(m_game));
        m_saveFolderHelp = trf("Opens the folder with your %s saves and settings.", cane::game(m_game).shortName);
        describe(m_saveFolderHelp.c_str());
        if (linkRow(tr("Log folder"), nullptr, nullptr, Icon::Folder)) openSafe(m_backend.locations.logs);
        describe(tr("Opens the folder with the launcher and game logs."));
        const bool custom = m_backend.config.contains("runtime_dir");
#ifndef CANE_LAUNCHER_SOURCE_BUILD
        // The packages players download run the bundled programs (built by CI): the section is only the way back from
        // a runtime an older launcher built from source.
        if (!custom) return;
#endif
        section(tr("Custom runtime"));
        ImGui::Indent(px(16));
        caption(custom ? trf("Using a runtime built from source: %s", utf8(m_backend.tools).c_str()).c_str() : tr("Using the runtime bundled with SafeHouse."),
                color::text2);
        ImGui::BeginDisabled(m_job.busy || gameRunning);
#ifdef CANE_LAUNCHER_SOURCE_BUILD
        caption(tr("Building needs CMake and a C++20 compiler, and may download dependencies."));
        gap(14);
        field(tr("Source checkout"));
        const float browseW = px(120);
        inputField("##source", m_source, sizeof(m_source), launcher::Charset::Free, tr("SafeHouse source folder"),
                   ImGui::GetContentRegionAvail().x - browseW - px(28));
        ImGui::SameLine(0, px(12));
        if (button(tr("Browse…##source"), Tone::Secondary, { browseW, 0 }) && !m_sourceDialog.running() &&
            !m_sourceDialog.start(filedialog::Kind::PickFolder, tr("Choose the SafeHouse source folder"), filedialog::startFolder({ m_source })))
            openSourcePicker();
        gap(12);
        ImGui::BeginDisabled(!m_source[0]);
        if (button(tr("Build and use"), Tone::Primary, { px(170), 0 })) {
            const std::string selected = m_source;
            m_job.start("Source build", [this, selected](const auto& log) { m_backend.buildSource(path(selected), m_job.cancel, log); });
            if (showLogs) *showLogs = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(12));
#else
        gap(14);
#endif
        ImGui::BeginDisabled(!custom);
        if (button(tr("Use bundled runtime"), Tone::Secondary, { px(210), 0 })) {
            m_backend.tools = executableDirectory();
            m_backend.config.erase("runtime_dir");
            persistLauncher();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::Unindent(px(16));
    }
    void openSafe(const fs::path& folder) {
        try {
            openFolder(folder);
        } catch (const std::exception&) {
        }
    }

public:
    static bool defaultFullscreen() {
        const char* deck = std::getenv("SteamDeck");
        const char* gamescope = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        return (deck && std::string(deck) == "1") || (gamescope && *gamescope) || (desktop && std::string(desktop) == "gamescope");
    }

private:
    LauncherBackend& m_backend;
    Job& m_job;
    GLFWwindow* m_window;
    settings::SystemSettings m_settings = settings::defaults();
    settings::Sly2Settings m_sly2;          // Sly 2: the whole Settings.ini (m_settings is its .system while shown)
    settings::Sly2GameOptions m_sly2Saved;  // its game options as last read or written (the pending migration)
    sly2input::Profile m_actions = sly2input::defaults();
    fs::file_time_type m_actionMtime{};
    int m_actionDevice = 0, m_actionGroup = 0;
    settings::LoadResult m_result = settings::LoadResult::Missing;
    uint32_t m_version = 0;
    fs::file_time_type m_mtime{};
    double m_nextPoll = 0, m_savedAt = -10, m_captureStarted = 0, m_captureCooldown = 0;
    int m_maxSamples = 0, m_captureAction = -1;
    bool m_captureController = false, m_captureArmed = false, m_saveOk = true, m_volumeDirty = false, m_debug = false;
    bool m_focusRows = false;
    bool m_paneFocused = false, m_focusRail = false, m_scrollTop = false, m_fixture = false;
    std::string m_help, m_captureNote, m_saveFolderHelp;
    GameId m_game = GameId::Sly1;
    char m_source[4096]{};
    std::array<std::vector<Installation>, kGameCount> m_installs;
    std::array<std::string, kGameCount> m_selected;
    double m_nextRegions = 0;
    bool m_regionsFixture = false;
    std::array<LauncherBackend::Ps3HdPack, kGameCount> m_ps3Packs{};
    filedialog::Async m_ps3Dialog;
    // (Sly 2's languages)
    std::vector<LauncherBackend::LanguageInfo> m_langs;
    std::vector<Installation> m_ownLangs;
    std::vector<std::string> m_ps3Missing;
    std::string m_langRegion;
    bool m_japaneseFontMissing = false, m_japaneseFontInstalled = false;
    double m_nextLangs = 0;
    bool m_langsDirty = true, m_langsFixture = false;
    LauncherBackend::Sly1Languages m_sly1Langs;
    double m_nextSly1Langs = 0;
    bool m_sly1LangsFixture = false;
    filedialog::Async m_langDialog;
    LangPick m_langPick = LangPick::Disc;
    std::string m_collectionMessage;  // the Collection Style Launcher rows' last result
    bool m_ps3ForCollection = false;  // the PS3 source being chosen is for the Collection menu import
    bool m_collectionImported = false;
    std::string m_collectionSource;   // an earlier PS3 import's source that is still there
    double m_nextCollectionCheck = 0;
    filedialog::Async m_installDialog;
    fs::path m_installRoot;
    bool m_customRoot = false;
    uint64_t m_rootFree = 0;
    double m_nextLocation = 0;
    std::vector<InstallPlace> m_places;
    std::string m_locationNote;
    GameId m_ps3Game = GameId::Sly2;
    bool m_ps3PickFolder = false;
};
} // namespace cane::ui
