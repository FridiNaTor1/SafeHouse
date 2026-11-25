#pragma once
// The Settings page: a game's settings file plus the launcher's own preferences in launcher.json. Each game's
// Settings.ini in its profile folder: Sly 1's (platform/sly1_settings.h; made once from the old
// Saves/system_settings.dat), Sly 2's <profile>/sly2/Settings.ini (platform/sly2_settings.h), which holds every Sly 2
// option, the original game's own Options page included (the PS2 kept those in each save), and Sly 3's
// (platform/sly3_settings.h); the games' in-game Options read and write the same files. A row at the top picks the
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
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
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
#include "sly1_action_bindings.h"
#include "job.h"
#include "prompt_bar.h"
#include "shell.h"
#include "system_settings.h"
#include "sly2_settings.h"
#include "sly1_settings.h"
#include "sly3_settings.h"
#include "sly3_action_bindings.h"
#include "audio_devices_sdl.h"
#include "monitor_select_glfw.h"
#include "../collection/ps3menu.h"
#include "update_dialog.h"
#if defined(__ANDROID__)
#include <android/api-level.h>
#include "../android_drivers.h"
#endif
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
    // (Sly 1's rhythm minigame keys: Settings.ini [Keyboard] Rhythm Triangle / Square / Circle / Cross)
    static constexpr int kRhythmCapture = 900;
    // (Sly 1's cutscene / conversation skip: Settings.ini [Keyboard] / [Controller] Skip Cutscene, sly1_settings.h)
    static constexpr int kSkipCapture = 950;
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
    std::function<std::string()> logError;   // (why launcher.log / last-failure.log could not be written: ui/job.h)
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
    static bool hasSettings(GameId game) { return game == GameId::Sly1 || game == GameId::Sly2 || game == GameId::Sly3; }
    // Every game keeps its options in Settings.ini (platform/sly1_settings.h, sly2_settings.h, sly3_settings.h: each its
    // own schema). iniGame(): Sly 2 and Sly 3, which share most rows (Sly 1 has rows of its own).
    bool iniGame() const { return m_game == GameId::Sly2 || m_game == GameId::Sly3; }
    settings::Sly2Load loadIni(settings::Sly2Settings& s) const {
        if (m_game == GameId::Sly1) return settings::loadSly1(m_backend.userDir(m_game), s);
        return m_game == GameId::Sly3 ? settings::loadSly3(m_backend.userDir(m_game), s) : settings::loadSly2(m_backend.userDir(m_game), s);
    }
    bool saveIni(const settings::Sly2Settings& s) const {
        if (m_game == GameId::Sly1) return settings::saveSly1(m_backend.userDir(m_game), s);
        return m_game == GameId::Sly3 ? settings::saveSly3(m_backend.userDir(m_game), s) : settings::saveSly2(m_backend.userDir(m_game), s);
    }
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
        if (m_folderEntry != FolderEntry::None) {
            m_folderEntry = FolderEntry::None;
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
        if (auto r = m_libraryDialog.poll()) {  // "Disc images folder" (Games)
            if (r->status == filedialog::Status::Chosen) setIsoLibrary(path(r->path));
            else if (r->status == filedialog::Status::Unavailable) openLibraryPicker();
            else if (r->status == filedialog::Status::Failed) m_libraryNote = r->error;
        }
        if (ImGuiFileDialog::Instance()->Display("isolibrary", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) })) {
            if (ImGuiFileDialog::Instance()->IsOk()) setIsoLibrary(path(ImGuiFileDialog::Instance()->GetCurrentPath()));
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
    fs::path file() const { return settings::sly2SettingsPath(m_backend.userDir(m_game)); }
    // The shown game's defaults (Sly 2 has its own keyboard / mouse layout).
    // (Sly 1: its own keyboard layout, Sly 2's action for action: sly1_settings.h sly1Defaults)
    settings::SystemSettings gameDefaults() const {
        return iniGame() ? settings::sly2Defaults() : m_game == GameId::Sly1 ? settings::sly1Defaults().system : settings::defaults();
    }
    void load() {
        {
            // (Settings.ini; made once from the old system_settings.dat, which the game would do at its next start too)
            const auto result = loadIni(m_sly2);
            m_settings = m_sly2.system;
            m_version = 0;
            m_result = result == settings::Sly2Load::Ok         ? settings::LoadResult::Ok
                       : result == settings::Sly2Load::Migrated ? settings::LoadResult::Upgraded
                       : result == settings::Sly2Load::Missing  ? settings::LoadResult::Missing
                                                                : settings::LoadResult::Corrupt;
            if (result == settings::Sly2Load::Migrated && !m_fixture) saveIni(m_sly2);
            m_sly2Saved = m_sly2.game;
        }
        // (Sly 3: the same file in its folder, sly3_action_bindings.h; Sly 1: its own actions, sly1_action_bindings.h)
        if (iniGame()) m_actions = sly2input::load(m_backend.userDir(m_game));
        if (m_game == GameId::Sly1) m_actions1 = sly1input::load(m_backend.userDir(m_game));
        std::error_code ec;
        m_mtime = fs::last_write_time(file(), ec);
        if (ec) m_mtime = {};
        if (actionGame()) {
            m_actionMtime = fs::last_write_time(actionPath(), ec);
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
        if (actionGame()) {
            const auto actionsNow = fs::last_write_time(actionPath(), ec);
            if ((ec ? fs::file_time_type{} : actionsNow) != m_actionMtime) load();
        }
    }
    void commitActions() {
        if (!writable() || actionsReadOnly()) return;
        m_saveOk = m_game == GameId::Sly1 ? sly1input::save(m_backend.userDir(m_game), m_actions1)
                                          : sly2input::save(m_backend.userDir(m_game), m_actions);
        m_savedAt = ImGui::GetTime();
        std::error_code ec;
        m_actionMtime = fs::last_write_time(actionPath(), ec);
        if (ec) m_actionMtime = {};
    }
    // ------------------------------------------------------------ Advanced Input Mapping
    // Sly 2 / Sly 3 share sly2input's table (Sly 3 offers a subset); Sly 1 has its own (sly1input). The file format,
    // <user-dir>/Saves/advanced_controls.json, is the same.
    bool actionGame() const { return iniGame() || m_game == GameId::Sly1; }
    fs::path actionPath() const { return sly2input::profilePath(m_backend.userDir(m_game)); }
    bool actionsEnabled() const { return m_game == GameId::Sly1 ? m_actions1.enabled : iniGame() && m_actions.enabled; }
    void setActionsEnabled(bool on) { (m_game == GameId::Sly1 ? m_actions1.enabled : m_actions.enabled) = on; }
    bool actionsReadOnly() const { return m_game == GameId::Sly1 ? m_actions1.readOnly : m_actions.readOnly; }
    size_t actionCount() const { return m_game == GameId::Sly1 ? sly1input::kActionCount : sly2input::kActionCount; }
    const char* actionLabel(size_t i) const { return m_game == GameId::Sly1 ? sly1input::kActions[i].label : sly2input::kActions[i].label; }
    const char* actionGroup(size_t i) const { return m_game == GameId::Sly1 ? sly1input::kActions[i].group : sly2input::kActions[i].group; }
    bool actionOffered(size_t i) const {
        if (m_game == GameId::Sly1) return true;
        if (!m_debug && sly2input::isNoclipAction(sly2input::Action(i))) return false;
        return m_game != GameId::Sly3 || sly3input::offered(sly2input::Action(i));   // (Sly 2's own minigames)
    }
    int* actionBindings(bool controller) {
        if (m_game == GameId::Sly1) return controller ? m_actions1.gamepad.data() : m_actions1.keyboard.data();
        return controller ? m_actions.gamepad.data() : m_actions.keyboard.data();
    }
    void resetActions() {
        if (m_game == GameId::Sly1) m_actions1 = sly1input::defaults();
        else m_actions = sly2input::defaults();
        setActionsEnabled(true);
    }
    bool writable() const { return m_result != settings::LoadResult::Newer && !m_fixture; }
    void commit() {
        if (!writable()) return;
        // (Sly 1's PS2 448-line internal resolution, which the shared list does not have, is kept)
        const int height = m_settings.internalResolutionHeight;
        settings::sanitizeStatic(m_settings, monitorHeight(), m_maxSamples,
                                 iniGame() ? settings::kSly2DefaultKeyboardBindings
                                 : m_game == GameId::Sly1 ? settings::kSly1DefaultKeyboardBindings : settings::kDefaultKeyboardBindings);
        if (m_game == GameId::Sly1 && height == settings::kSly1Ps2Height) m_settings.internalResolutionHeight = height;
        {
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
            if (loadIni(onDisk) == settings::Sly2Load::Ok) m_sly2.unknown = onDisk.unknown;
            m_saveOk = saveIni(m_sly2);
            if (m_saveOk) m_sly2Saved = m_sly2.game;
        }
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
    // (settings v22: the game's monitor, platform/monitor_select.h, caps the internal resolution list; its modes give the
    // frame rates: m_monitors, read by refreshMonitors)
    int monitorHeight() const {
        // (the game's monitor when one is chosen: the list is that monitor's)
        if (m_settings.monitor[0]) {
            const int i = cane::monitor::resolve(m_monitors.list, cane::monitor::load(m_settings.monitor));
            if (i >= 0 && m_monitors.list[size_t(i)].height > 0) return m_monitors.list[size_t(i)].height;
        }
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
#if defined(CANE_IOS)
        // (iOS: the rates the display paces, Sly2-Release display.cpp: 30, 60 and the screen's own, 120 on ProMotion)
        std::vector<int> rates{ 30, 60 };
#else
        std::vector<int> rates{ 30, 60, 90, 120, 240 };
        // Sly 2 runs 60 Hz steps drawn between them at any rate (Sly2-Release frameinterp.h): 144 and no limit too
        // (kUncappedFrameRate, an ordinary value of the shared field: Sly2-Release reads it as no limit)
        // Sly 1 runs a variable time step: 144 and no limit there too
        rates.insert(rates.end(), { 144, kUncappedFrameRate });
#endif
        // (the game's monitor's highest refresh rate: settings v22's chosen one, else the primary one; m_monitors is read
        // with the Display / Graphics rows)
        const int i = cane::monitor::resolve(m_monitors.list, cane::monitor::load(m_settings.monitor));
        if (i >= 0 && m_monitors.list[size_t(i)].maxRefreshRate >= 30) rates.push_back(m_monitors.list[size_t(i)].maxRefreshRate);
        else if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
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
            actionBindings(m_captureController)[size_t(m_captureAction - kAdvancedCapture)] = key == GLFW_KEY_DELETE ? -1 : key;
            m_captureAction = -1;
            commitActions();
            return;
        }
        if (m_captureAction == kSkipCapture) {
            if (key != GLFW_KEY_DELETE && !settings::validKeyboardBinding(key)) { m_captureNote = tr("That key cannot be bound."); return; }
            // (its own binding: it may share a key with an action, e.g. Enter is also Start; while a scene can be skipped
            //  the key only skips)
            m_sly2.sly1.skipKey = key == GLFW_KEY_DELETE ? settings::kKeyUnknown : key;
            m_captureAction = -1;
            commit();
            return;
        }
        if (m_captureAction >= kRhythmCapture) {
            if (key == settings::kKeyUnknown || !settings::validCameraKey(key)) { m_captureNote = tr("That key cannot be bound."); return; }
            // (the minigame's keys may be the movement's: they only count there; among themselves the two swap)
            auto& keys = m_sly2.sly1.rhythmKeys;
            const size_t slot = size_t(m_captureAction - kRhythmCapture);
            for (int& k : keys)
                if (k == key) k = keys[slot];
            keys[slot] = key;
            m_captureAction = -1;
            commit();
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
        if (m_captureAction >= kAdvancedCapture) return tr(actionLabel(size_t(m_captureAction - kAdvancedCapture)));
        if (m_captureAction == kSkipCapture) return tr(skipLabel());
        if (m_captureAction >= kRhythmCapture) return tr(rhythmNames[static_cast<size_t>(m_captureAction - kRhythmCapture)]);
        return tr(m_captureAction >= settings::kBindingCount ? cameraNames[static_cast<size_t>(m_captureAction - settings::kBindingCount)]
                                                             : standardLabel(m_captureAction));
    }
    // Action capture accepts mouse buttons and the wheel after the click that
    // selected the row has been released. Camera bindings use keyboard keys.
    void pollMouseCapture() {
        if (!capturing() || m_captureController ||
            (m_captureAction >= settings::kBindingCount && m_captureAction < kAdvancedCapture && m_captureAction != kSkipCapture)) return;
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
            actionBindings(false)[size_t(m_captureAction - kAdvancedCapture)] = binding;
            m_captureAction = -1;
            m_captureCooldown = ImGui::GetTime() + 0.3;
            commitActions();
            return;
        }
        if (m_captureAction == kSkipCapture) m_sly2.sly1.skipKey = binding;
        else m_settings.keyboardBindings[m_captureAction] = binding;
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
            // (an advanced action that takes the right stick, sly2input::stickBindable: the hack ship's guns)
            const float rx = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], ry = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
            if (m_captureAction >= kAdvancedCapture && sly2input::stickBindable(sly2input::Action(m_captureAction - kAdvancedCapture)) &&
                rx * rx + ry * ry > 0.5f * 0.5f)
                any = true, pressed = pressed < 0 ? settings::kGamepadRightStick : pressed;
        }
        // Wait for the button that started the capture to be released.
        if (!m_captureArmed) {
            m_captureArmed = !any;
            return;
        }
        if (pressed < 0) return;
        const bool advanced = m_captureAction >= kAdvancedCapture;
        if (advanced) actionBindings(true)[size_t(m_captureAction - kAdvancedCapture)] = pressed;
        else if (m_captureAction == kSkipCapture) m_sly2.sly1.skipPad = pressed;
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
            std::copy(std::begin(d.monitor), std::end(d.monitor), s.monitor);
            m_sly2.upscale = settings::Sly2UpscaleOptions{};
            if (m_game == GameId::Sly2) m_sly2.vr.worldScale = settings::Sly2VrOptions{}.worldScale;
            if (iniGame() || m_game == GameId::Sly1) m_sly2.hudPlacement = settings::Sly2Settings{}.hudPlacement;
            if (m_game == GameId::Sly1) m_sly2.sly1.autoIconify = settings::Sly1Options{}.autoIconify;
            break;
        case Graphics:
            s.internalResolutionHeight = d.internalResolutionHeight, s.msaaEnabled = d.msaaEnabled, s.msaaSamples = d.msaaSamples;
            s.drawDistance = d.drawDistance, s.fogType = d.fogType, s.longShadows = d.longShadows, s.fastShadows = d.fastShadows;
            s.renderer = d.renderer;
            if (iniGame()) m_sly2.outlineThickness = settings::Sly2Settings{}.outlineThickness;
            if (m_game == GameId::Sly1) {
                const settings::Sly1Options o;
                s.anisotropy = d.anisotropy;
                m_sly2.sly1.resolutionScale = o.resolutionScale, m_sly2.sly1.gamma = o.gamma, m_sly2.sly1.textureFiltering = o.textureFiltering;
            }
            break;
        case Audio:
            settings::defaultAudio(s);
            m_sly2.game.speakers = settings::Sly2GameOptions{}.speakers;
            m_sly2.game.outputDevice.clear(), m_sly2.game.inputDevice.clear(), m_sly2.game.headsetOutputDevice.clear();
            m_sly2.game.usbHeadset = 0;
            break;
        case Controls:
            if (actionsEnabled()) {
                resetActions();
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
            if (m_game == GameId::Sly1) m_sly2.sly1.deadZone = settings::Sly1Options{}.deadZone;
            if (m_game == GameId::Sly1) m_sly2.sly1.rhythmKeys = settings::Sly1Options{}.rhythmKeys;
            if (m_game == GameId::Sly1) m_sly2.sly1.skipKey = settings::Sly1Options{}.skipKey, m_sly2.sly1.skipPad = settings::Sly1Options{}.skipPad;
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
    // The game whose settings are shown (Sly 1 / Sly 2 / Sly 3).
    void gameRow(bool locked) {
        static const std::vector<std::string> games{ cane::game(GameId::Sly1).shortName, cane::game(GameId::Sly2).shortName,
                                                     cane::game(GameId::Sly3).shortName };
        int index = m_game == GameId::Sly3 ? 2 : m_game == GameId::Sly2 ? 1 : 0;
        ImGui::BeginDisabled(locked);
        if (stepperRow(tr("Game"), &index, games)) {
            cancelCapture();
            m_game = index == 2 ? GameId::Sly3 : index == 1 ? GameId::Sly2 : GameId::Sly1;
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
    // (settings v22, platform/monitor_select.h) the connected monitors, read when the page is shown and every 2 seconds
    cane::monitor::Connected m_monitors;
    double m_monitorsListed = -1.0;
    void refreshMonitors() {
        if (m_monitorsListed >= 0 && ImGui::GetTime() - m_monitorsListed < 2.0) return;
        m_monitors = cane::monitor::connected();
        m_monitorsListed = ImGui::GetTime();
    }
    void monitorRow() {
        namespace mo = cane::monitor;
        refreshMonitors();
        const std::string stored = mo::load(m_settings.monitor);
        const std::vector<std::string> choices = mo::choices(m_monitors.list, stored);
        std::vector<std::string> labels;
        for (size_t i = 0; i < choices.size(); ++i) {
            if (i == 0) labels.push_back(tr("Primary"));
            else if (i <= m_monitors.list.size()) labels.push_back(mo::label(m_monitors.list, int(i) - 1));
            else labels.push_back(trf("%s (not connected)", mo::storedName(choices[i]).c_str()));
        }
        int index = mo::choiceIndex(m_monitors.list, stored);
        if (stepperRow(tr("Monitor"), &index, labels)) mo::store(m_settings.monitor, choices[size_t(index)]), commit();
        describe(glfwGetPlatform() == GLFW_PLATFORM_WAYLAND
                     ? tr("The monitor Fullscreen and Borderless fill. Primary is the system's main monitor. A monitor that is unplugged "
                          "uses the primary one until it is back. On Wayland a window opens where the desktop puts it.")
                     : tr("The monitor Fullscreen and Borderless fill and a window opens on. Primary is the system's main monitor. A "
                          "monitor that is unplugged uses the primary one until it is back."));
    }
    void displayRows() {
#if !defined(CANE_ANDROID) && !defined(CANE_IOS)
        // (Android: the system places the window, in DeX too; nothing to choose. iOS: the app fills its window)
        const std::vector<std::string> modes{ tr("Windowed"), tr("Borderless"), tr("Fullscreen") };
        int mode = m_settings.windowMode;
        if (stepperRow(tr("Window mode"), &mode, modes)) m_settings.windowMode = mode, commit();
        describe(tr("Borderless fills the screen without changing its resolution."));
        monitorRow();
#endif
#if !defined(CANE_IOS)
        // (iOS: the game is always in step with the display, Sly2-Release display.cpp)
        toggle(tr("VSync"), m_settings.vsyncEnabled, tr("Matches the frame rate to the display to stop tearing."));
#endif
        const std::vector<std::string> aspects{ tr("Fit to screen"), "4:3", "16:9", "16:10", tr("4:3 (PS2)"), tr("16:9 (PS2)") };
        int aspect = m_settings.aspectMode;
        if (stepperRow(tr("Aspect ratio"), &aspect, aspects)) m_settings.aspectMode = aspect, commit();
        describe(tr("Fit to screen uses the whole window. The PS2 options match the original framing."));
        // (Settings.ini [Display] HUD Placement: Sly2-Release / Sly3-Release display.h hudPlacement, Sly 1's
        // safehouse/hudbox.h; the games' Display options have the same row. Replaces the original's Center Screen.)
        if (iniGame() || m_game == GameId::Sly1) {
            const std::vector<std::string> placements{ "4:3", "16:9", "16:10", tr("Match screen") };
            int placement = std::clamp(m_sly2.hudPlacement, 0, settings::kHudPlacementCount - 1);
            if (stepperRow(tr("HUD placement"), &placement, placements)) m_sly2.hudPlacement = placement, commit();
            describe(tr("Where the health, coins, meters and counters sit on wide screens. 4:3 is the original's layout, 16:9 "
                        "matches the PS3 version and Match screen moves them out to the edges of any window."));
        }
        const auto rates = frameRates();
        std::vector<std::string> labels;
        for (int r : rates) labels.push_back(r >= kUncappedFrameRate ? std::string(tr("Uncapped")) : trf("%d FPS", r));
        int index = indexOf(rates, m_settings.frameRate);
        const std::string custom = trf("%d FPS", m_settings.frameRate);
        if (stepperRow(tr("Frame rate limit"), &index, labels, nullptr, index < 0 ? custom.c_str() : nullptr))
            m_settings.frameRate = rates[static_cast<size_t>(index)], commit();
        describe(tr("The highest frame rate the game runs at. Gameplay speed is the same at every setting."));
#if !defined(CANE_ANDROID) && !defined(CANE_IOS)
        // (Sly 1, Settings.ini [Display] Minimize When Unfocused: the game's own video option)
        if (m_game == GameId::Sly1)
            toggle(tr("Minimize when unfocused"), m_sly2.sly1.autoIconify,
                   tr("A fullscreen game minimizes when another window takes the focus. Off: it stays on its screen."));
#endif
        upscalingRows();
        vrRows();
    }
    // (Sly 2, Settings.ini [VR] World Scale: the game's VR Settings has the same row) where a headset can play: a PC
    // (Linux, Windows: SteamVR, the Steam Frame, Monado) or the Meta Quest
    void vrRows() {
#if defined(__APPLE__) || defined(__SWITCH__)
        return;
#else
        if (m_game != GameId::Sly2) return;
#if defined(__ANDROID__)
        if (!vrHeadsetDevice()) return;
#endif
        std::vector<std::string> labels;
        const int lo = int(std::lround(settings::kVrWorldScaleMin * 10.0f)), hi = int(std::lround(settings::kVrWorldScaleMax * 10.0f));
        auto name = [](int tenths) { char b[16]; std::snprintf(b, sizeof(b), "%.1fx", double(tenths) / 10.0); return std::string(b); };
        for (int t = lo; t <= hi; ++t) labels.push_back(name(t));
        const float scale = m_sly2.vr.worldScale;
        const int tenths = int(std::lround(scale * 10.0f));
        int index = std::fabs(scale * 10.0f - float(tenths)) < 0.01f && tenths >= lo && tenths <= hi ? tenths - lo : -1;
        char custom[16];
        std::snprintf(custom, sizeof(custom), "%.2fx", double(scale));
        if (stepperRow(tr("VR world scale"), &index, labels, nullptr, index < 0 ? custom : nullptr))
            m_sly2.vr.worldScale = float(lo + index) / 10.0f, commit();
        describe(tr("Immersive VR: how big the world looks around you. 1.0x is life size; higher values make the world look smaller."));
#endif
    }
    // (Sly 2 and Sly 3, Settings.ini [Display] Upscaling: platform/render/fsr.h; the game's Display options and, on a PC headset,
    // its VR Settings show the same rows. Not on the Meta Quest: its compositor upscales the eyes, VR Settings there.)
    static bool vrHeadsetDevice() {
#if defined(__ANDROID__)
        char manufacturer[PROP_VALUE_MAX] = {};
        __system_property_get("ro.product.manufacturer", manufacturer);
        const std::string m = manufacturer;
        return m == "Oculus" || m == "oculus" || m == "Meta" || m == "meta";
#else
        return false;
#endif
    }
    void upscalingRows() {
        if (!iniGame() || vrHeadsetDevice()) return;
        settings::Sly2UpscaleOptions& u = m_sly2.upscale;
        toggle(tr("Upscaling"), u.enabled,
               tr("Draws the 3D scene smaller and scales it up to the screen with a sharpening upscaler: faster, nearly as "
                  "sharp. The HUD and menus stay at full resolution. While on, it replaces the internal resolution."));
        if (!u.enabled) return;
        const std::vector<std::string> methods{ "FSR 1" };
        int method = 0;
        stepperRow(tr("Upscaling method"), &method, methods);
        describe(tr("AMD FidelityFX Super Resolution 1.0: edge-aware upscaling with sharpening."));
        const std::vector<std::string> presets{ std::string(tr("Ultra quality")) + " (77%)", std::string(tr("Quality")) + " (67%)",
                                                std::string(tr("Balanced")) + " (59%)", std::string(tr("Performance")) + " (50%)" };
        int preset = std::clamp(u.preset, 0, 3);
        if (stepperRow(tr("Upscaling preset"), &preset, presets)) u.preset = preset, commit();
        describe(tr("How large the 3D scene is drawn, as a share of the screen's width and height. Lower is faster."));
        rangeRow(tr("Sharpness"), u.sharpness, 0, 100, 10, tr("How much the upscaled picture is sharpened."), percent);
    }
    void graphicsRows() {
        refreshMonitors();
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
        describe(iniGame() ? tr("How far away scenery, props and effects stay visible. Original matches the PS2; higher settings "
                                          "keep characters and gameplay at the PS2's distances.")
                                        : tr("How far away objects stay visible. Original matches the PS2."));
        // (settings v16 longShadows: Sly2-Release render.cpp FBuildShadow2, a PC improvement, not the original)
        if (iniGame())
            toggle(tr("Long shadows"), m_settings.longShadows,
                   tr("Character shadows from low lights reach the floor in full. Off: they end at a fixed length, as on the PS2."));
        // (settings v20 fastShadows: Sly2-Release display.h fFastShadows, a low-end option)
        if (iniGame())
            toggle(tr("Fast shadows"), m_settings.fastShadows,
                   tr("Character shadows with less GPU work, for low-end devices. The picture stays nearly the same."));
        // (Sly 2 and Sly 3, Settings.ini [Graphics] Outline Thickness: the games' render.cpp; the game's display options
        // (and Sly 2's VR Settings) have the same row)
        if (iniGame())
            rangeRow(tr("Outline thickness"), m_sly2.outlineThickness, settings::kOutlineThicknessMin, settings::kOutlineThicknessMax,
                     settings::kOutlineThicknessStep,
                     tr("The width of the characters' cartoon outlines, in percent of the original. In VR the outlines keep the flat "
                        "picture's look for the headset's wider view."), percent);
        // (Sly 3, Settings.ini [Graphics] 3D Mode: Sly3-Release gauge3.h FAsk3d3; off by default, the 3-D question never asked)
        if (m_game == GameId::Sly3)
            toggle(tr("3D mode (red/cyan glasses)"), m_sly2.game.mode3d,
                   tr("The original's 3D mode for red/cyan glasses. On: the game asks whether to play in 3D, and the job "
                      "menu offers a start in 3D. Off: never asked, normal picture."));
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
        // (settings v21 renderer: Sly2-Release / Sly3-Release renderer.h, Sly 1's gl.cpp through platform/vulkan/vk_game.h,
        // platform/renderer_choice.h: Auto is Vulkan with OpenGL the fallback; Linux and Windows, where the games have the
        // Vulkan renderer)
        if (iniGame() || m_game == GameId::Sly1) {
            const std::vector<std::string> renderers{ tr("Auto (Vulkan)"), "Vulkan", "OpenGL" };
            int renderer = std::clamp(m_settings.renderer, 0, 2);
            if (stepperRow(tr("Renderer"), &renderer, renderers)) m_settings.renderer = renderer, commit();
            describe(tr("Auto uses Vulkan and switches to OpenGL by itself when the graphics driver cannot run Vulkan. After a "
                        "Vulkan start that failed, Auto uses OpenGL for the next few starts and then tries Vulkan again (also "
                        "after an update); choosing Vulkan here tries it at once. Takes effect the next time the game starts."));
        }
#elif defined(__ANDROID__)
        // (Sly2-Release renderer.cpp on Android: Auto is Vulkan, OpenGL ES when it cannot start, for VR, or after a
        // start that stopped in its first frames; Sly 1 the same through platform/vulkan/vk_game.h and
        // platform/render/gles_sly1.h, without VR)
        if (iniGame() || m_game == GameId::Sly1) {
            const std::vector<std::string> renderers{ tr("Automatic"), "Vulkan", "OpenGL ES" };
            int renderer = std::clamp(m_settings.renderer, 0, 2);
            if (stepperRow(tr("Renderer"), &renderer, renderers)) m_settings.renderer = renderer, commit();
            describe(tr("Auto uses Vulkan, and OpenGL ES when this device cannot run it, in VR, or for a few starts after a "
                        "start on Vulkan crashed; then it tries Vulkan again. Vulkan tries it at once. Takes effect the next "
                        "time the game starts."));
        }
#endif
        if (m_game != GameId::Sly1) return;   // (Sly 2 has no fog option)
        // (Sly 1's own Settings.ini keys, platform/sly1_settings.h: the game's PC video options)
        const std::vector<std::string> fogs{ tr("PS2 style"), tr("PS3 style"), tr("Off") };
        int fog = m_settings.fogType == settings::Fog_PS3 ? 1 : m_settings.fogType == settings::Fog_Off ? 2 : 0;
        if (stepperRow(tr("Fog"), &fog, fogs))
            m_settings.fogType = fog == 1 ? settings::Fog_PS3 : fog == 2 ? settings::Fog_Off : settings::Fog_PS2, commit();
        describe(tr("PS2 style is the original look. PS3 style follows the remaster. Off draws no fog."));
        if (m_settings.internalResolutionHeight == 0)
            rangeRow(tr("Resolution scale"), m_sly2.sly1.resolutionScale, 50, 200, 25,
                     tr("With Internal resolution Native: the 3D scene's size against the window. Lower is faster, higher is "
                        "sharper."), percent);
        {
            int brightness = int(std::lround(m_sly2.sly1.gamma * 100.0f));
            if (rangeRow(tr("Brightness"), brightness, 50, 150, 5, tr("The picture's gamma. The original's is 100%."), percent))
                m_sly2.sly1.gamma = float(brightness) / 100.0f, commit();
        }
        const std::vector<std::string> filters{ tr("Trilinear"), tr("Bilinear") };
        int filter = m_sly2.sly1.textureFiltering ? 0 : 1;
        if (stepperRow(tr("Texture filtering"), &filter, filters)) m_sly2.sly1.textureFiltering = filter == 0, commit();
        describe(tr("Trilinear blends between texture sizes for smoother distant surfaces. Bilinear is slightly sharper and "
                    "shows the steps."));
        static const std::vector<int> anisotropies{ 1, 2, 4, 8, 16 };
        const std::vector<std::string> anisotropyLabels{ tr("Off"), "2x", "4x", "8x", "16x" };
        int anisotropy = indexOf(anisotropies, m_settings.anisotropy);
        if (stepperRow(tr("Anisotropic filtering"), &anisotropy, anisotropyLabels, nullptr, anisotropy < 0 ? tr("Custom") : nullptr))
            m_settings.anisotropy = anisotropies[static_cast<size_t>(anisotropy)], commit();
        describe(tr("Keeps textures sharp on surfaces seen at a steep angle, such as floors and roads."));
    }
    // ---- (audio, notes/AUDIO_2026-11-10.md) Sly 2's devices by name (platform/audio_devices.h; Settings.ini [Audio]) and
    // the original's USB Headset option; the list is SDL2's (loaded at run time), read when the page is shown
    cane::audio::DeviceList m_audioDevices;
    double m_audioListed = -1.0;
    // (Sly 1: the output only; it has no USB headset)
    void audioDeviceRows(bool outputOnly = false) {
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
        if (outputOnly) return;
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
            audioDeviceRows(true);
        } else if (iniGame()) {
            // (the original's Speakers option, Settings.ini; values settings::Sly2Speakers)
            const std::vector<std::string> speakers{ tr("Surround"), tr("Stereo"), tr("Mono") };
            static const int values[] = { settings::Speakers_Surround, settings::Speakers_Stereo, settings::Speakers_Mono };
            int speaker = m_sly2.game.speakers == settings::Speakers_Mono ? 2 : m_sly2.game.speakers == settings::Speakers_Stereo ? 1 : 0;
            if (stepperRow(tr("Speakers"), &speaker, speakers)) m_sly2.game.speakers = values[speaker], commit();
            describe(tr("Mono mixes both channels together, for a single speaker or one ear. Surround (the PS2's default) and "
                     "Stereo both play in stereo. The same as the in-game Speakers option."));
            if (m_game == GameId::Sly2) languageRow(), voiceLanguageRow();
            if (m_game == GameId::Sly3) sly3LanguageRow();
            audioDeviceRows();
        }
        auto volume = [&](const char* label, float& value, const char* help) {
            float v = value;
            if (sliderRow(label, &v)) value = v, m_volumeDirty = true;
            describe(help);
        };
        const bool sly2 = iniGame();
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
    // A standard binding's name: the PS2 button's (Sly 2 / Sly 3), or for Sly 1 its action on that button (the same
    // names as its Advanced Input Mapping rows, sly1_action_bindings.h), e.g. L1 "Talk to Bentley (binocucom)".
    const char* standardLabel(int button) const {
        if (m_game == GameId::Sly1)
            for (const auto& a : sly1input::kActions)
                if (a.context == sly1input::Context::Any && a.pad == button) return a.label;
        return actionNames[size_t(button)];
    }
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
        const char* label = tr(advanced ? actionLabel(size_t(index)) : standardLabel(index));
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
            const int* bindings = advanced ? actionBindings(controller)
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
                    m_help = trf("Also used by %s. Both actions trigger together.", tr(standardLabel(duplicate)));
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
    static constexpr std::array<const char*, settings::kSly1RhythmKeyCount> rhythmNames{
        N_("Rhythm: Triangle"), N_("Rhythm: Square"), N_("Rhythm: Circle"), N_("Rhythm: Cross") };
    // Sly 1's rhythm minigame: its own key for one of its buttons (system_settings.h kSly1RhythmPads' order).
    void rhythmKeyRow(int slot) {
        ImGui::PushID(300 + slot);
        const char* label = tr(rhythmNames[static_cast<size_t>(slot)]);
        const Row r = rowBehavior(label, px(52), false, 0.45f);
        if (r.visible) {
            const int action = kRhythmCapture + slot;
            const bool capturingThis = m_captureAction == action && !m_captureController;
            if (r.activated && !r.disabled && !capturingThis && ImGui::GetTime() > m_captureCooldown) startCapture(action, false);
            auto* dl = ImGui::GetWindowDrawList();
            const float cy = r.bb.GetCenter().y;
            static constexpr input::Button buttons[settings::kSly1RhythmKeyCount] = { input::Button::North, input::Button::West,
                                                                                      input::Button::East, input::Button::South };
            drawPadButton(dl, { r.bb.Min.x + px(30), cy }, px(28), input::Family::PlayStation, buttons[slot]);
            dl->AddText(fonts().body, fonts().body->FontSize, { r.bb.Min.x + px(56), std::round(cy - fonts().body->FontSize * 0.5f) },
                        ink(color::text), label);
            recordLayout(label, r.bb);
            const float right = r.control.Max.x;
            const int value = m_sly2.sly1.rhythmKeys[static_cast<size_t>(slot)];
            if (capturingThis) {
                const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
                const auto t = measure(fonts().tab, tr("Press a key…"));
                dl->AddRect({ r.control.Min.x, r.bb.Min.y + px(8) }, { right, r.bb.Max.y - px(8) }, ink(color::cta, pulse), px(6), 0, px(2));
                dl->AddText(fonts().tab, fonts().tab->FontSize, { std::round(r.control.GetCenter().x - t.x * 0.5f), std::round(cy - t.y * 0.5f) },
                            ink(color::cta), tr("Press a key…"));
                m_help = m_captureNote.empty() ? std::string(tr("Select, then press the key to use for this action. Escape cancels.")) : m_captureNote;
            } else if (value == settings::kKeyUnknown) {
                const auto t = measure(fonts().body, tr("Unbound"));
                dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::muted), tr("Unbound"));
            } else {
                const std::string name = keyName(value);
                const float h = px(30), w = keycapWidth(name.c_str(), h);
                drawKeycap(dl, { right - w, std::round(cy - h * 0.5f) - px(1) }, h, name.c_str());
            }
            if (!capturingThis && (ImGui::IsItemFocused() || ImGui::IsItemHovered()))
                m_help = tr("Only in the rhythm minigame (Mz. Ruby's fight): this key plays that button there. Everywhere else it keeps its usual action.");
        }
        ImGui::PopID();
    }
    // Sly 1's cutscene / conversation skip: its own key or controller button (Settings.ini Skip Cutscene), beside the
    // PS2 buttons' bindings and kept in both of the input mappings.
    static const char* skipLabel() { return N_("Skip cutscene / conversation"); }
    void skipRow(bool controller, input::Family family) {
        ImGui::PushID(controller ? 401 : 400);
        const char* label = tr(skipLabel());
        const Row r = rowBehavior(label, px(52), false, 0.45f);
        if (r.visible) {
            const bool capturingThis = m_captureAction == kSkipCapture && m_captureController == controller;
            if (r.activated && !r.disabled && !capturingThis && ImGui::GetTime() > m_captureCooldown) startCapture(kSkipCapture, controller);
            auto* dl = ImGui::GetWindowDrawList();
            const float cy = r.bb.GetCenter().y;
            dl->AddText(fonts().body, fonts().body->FontSize, { r.bb.Min.x + px(56), std::round(cy - fonts().body->FontSize * 0.5f) },
                        ink(color::text), label);
            recordLayout(label, r.bb);
            const float right = r.control.Max.x;
            const int value = controller ? m_sly2.sly1.skipPad : m_sly2.sly1.skipKey;
            if (capturingThis) {
                const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
                const char* prompt = controller ? tr("Press a button…") : tr("Press a key or mouse button…");
                const auto t = measure(fonts().tab, prompt);
                dl->AddRect({ r.control.Min.x, r.bb.Min.y + px(8) }, { right, r.bb.Max.y - px(8) }, ink(color::cta, pulse), px(6), 0, px(2));
                dl->AddText(fonts().tab, fonts().tab->FontSize, { std::round(r.control.GetCenter().x - t.x * 0.5f), std::round(cy - t.y * 0.5f) },
                            ink(color::cta), prompt);
                m_help = m_captureNote.empty() ? std::string(controller ? tr("Select, then press the controller button to use for this action.")
                                                                        : tr("Select, then press the key to use for this action. Escape cancels."))
                                               : m_captureNote;
            } else if (value == settings::kKeyUnknown) {
                const auto t = measure(fonts().body, tr("Unbound"));
                dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::muted), tr("Unbound"));
            } else if (controller) {
                const auto button = input::fromGlfwBinding(value);
                const char* name = input::buttonLabel(button, family);
                const auto t = measure(fonts().body, name);
                dl->AddText(fonts().body, fonts().body->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::text2), name);
                const float glyph = px(30);
                const float gw = padButtonWidth(glyph, family, button);
                if (button != input::Button::Count) drawPadButton(dl, { right - t.x - px(12) - gw * 0.5f, cy }, glyph, family, button);
            } else {
                const std::string name = keyName(value);
                const float h = px(30), w = keycapWidth(name.c_str(), h);
                drawKeycap(dl, { right - w, std::round(cy - h * 0.5f) - px(1) }, h, name.c_str());
            }
            if (!capturingThis && (ImGui::IsItemFocused() || ImGui::IsItemHovered()))
                m_help = tr("Hold it to skip a cutscene or a binocucom conversation (one press with Instant skip cutscenes). "
                            "While a scene can be skipped it does nothing else.");
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
        if (forced) pad = nullptr;   // (the forced family wins over a pad that happens to be connected)
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
        // (a VR family: its controllers, both hands in one picture)
        const auto* device = atlas.deviceTexture()
            ? atlas.device(input::devicePictureId(family == input::Family::PlayStation ? input::Family::PS4 : family)) : nullptr;
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
        const bool advanced = actionsEnabled();
        const auto family = input::promptFamily(promptStyle(), 0);
        section(tr("Controller in use"));
        controllerCard();
        if (actionGame()) {
            ImGui::BeginDisabled(actionsReadOnly());
            bool enabled = actionsEnabled();
            if (toggleRow(tr("Advanced Input Mapping"), &enabled)) {
                setActionsEnabled(enabled);
                cancelCapture();
                commitActions();
            }
            describe(tr("Bind gameplay actions independently. Off restores your original button mapping without changing it."));
            ImGui::EndDisabled();
            if (actionsReadOnly()) caption(tr("The advanced controls file cannot be edited by this version."));
        }
        if (advanced) {
            section(tr("Action bindings"));
            const std::vector<std::string> devices{tr("Keyboard and mouse"), tr("Controller")};
            stepperRow(tr("Input device"), &m_actionDevice, devices);
            std::vector<std::string> groups;
            for (size_t i = 0; i < actionCount(); ++i) {
                if (!actionOffered(i)) continue;
                const char* group = actionGroup(i);
                if (std::find(groups.begin(), groups.end(), group) == groups.end()) groups.emplace_back(group);
            }
            m_actionGroup = std::clamp(m_actionGroup, 0, int(groups.size()) - 1);
            std::vector<std::string> groupNames;
            for (const auto& g : groups) groupNames.push_back(tr(g));
            stepperRow(tr("Action group"), &m_actionGroup, groupNames);
            ImGui::BeginDisabled(actionsReadOnly());
            for (size_t i = 0; i < actionCount(); ++i)
                if (actionOffered(i) && groups[size_t(m_actionGroup)] == actionGroup(i))
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
        // (Sly 1, Settings.ini [Controller] Dead Zone: the sticks')
        if (m_game == GameId::Sly1) {
            int deadZone = int(std::lround(m_sly2.sly1.deadZone * 100.0f));
            if (rangeRow(tr("Stick dead zone"), deadZone, 5, 50, 5,
                         tr("How far a stick moves before it counts. Raise it if the character or camera drifts by itself."), percent))
                m_sly2.sly1.deadZone = float(deadZone) / 100.0f, commit();
        }
        else if (iniGame())
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
               m_game == GameId::Sly1 ? tr("One press of the skip button skips a cutscene or a conversation. Off: hold it for 2 seconds.")
                                      : tr("One press of Circle skips a cutscene. Off: hold Circle for 2 seconds."));
        if (m_game == GameId::Sly1 || iniGame()) {
            const std::vector<std::string> cameraRotationLabels{ tr("Off"), tr("On") };
            int cameraRotation = m_settings.autoCameraRotation != 0 ? 1 : 0;
            if (stepperRow(tr("Automatic camera rotation"), &cameraRotation, cameraRotationLabels))
                m_settings.autoCameraRotation = cameraRotation, commit();
            describe(tr("Rotate the follow camera as you move."));
        }
        section(tr("Controller"));
        // (settings v17: Sly 2's controller camera inversion; off = the original's orbit camera)
        if (iniGame()) {
            toggle(tr("Invert camera horizontal"), m_settings.padInvertCameraX,
                   tr("Off: the right stick orbits the camera as on the PS2 (right swings it around to the right, so the view turns left). On: right turns the view right."));
            toggle(tr("Invert camera vertical"), m_settings.padInvertCameraY,
                   tr("Off: as on the PS2, pushing the right stick up raises the camera to look down. On: up looks up."));
            // (the original's own options, Settings.ini; the PS2 kept them in each save. Sly 3 keeps its per-camera options
            //  in the save, as the original)
            if (m_game == GameId::Sly2) {
                toggle(tr("Invert binocucom"), m_sly2.game.invertBinocucom,
                       tr("The controller in the binocucom. The same as the in-game Invert Binocucom Controls."));
                toggle(tr("Invert turret"), m_sly2.game.invertTurret,
                       tr("The controller on turrets. The same as the in-game Invert Turret Controls."));
            }
            if (m_game == GameId::Sly3) {
                // (Sly 3's Camera Options per camera, Settings.ini; the PS2 kept them in each save: sly3_settings.h)
                static const char* const kinds[7] = { N_("Camera"), N_("Binocucom"), N_("Grapple-Cam"), N_("Turret"), N_("Strafe"),
                                                      N_("Swimming"), N_("Harpoon") };
                const std::vector<std::string> values{ tr("Default"), tr("Reversed") };
                for (int k = 0; k < 7; ++k)
                    for (int a = 0; a < 2; ++a) {
                        int v = m_sly2.game.camReverse[k][a] ? 1 : 0;
                        const std::string label = a ? trf("%s up / down", tr(kinds[k])) : trf("%s left / right", tr(kinds[k]));
                        if (stepperRow(label.c_str(), &v, values)) m_sly2.game.camReverse[k][a] = v, commit();
                    }
                describe(tr("The original's Camera Options for each kind of camera. The same as the in-game Camera Options."));
            }
        }
        if (!advanced) for (int i = 0; i < settings::kBindingCount; ++i) bindingRow(i, true, family);
        if (m_game == GameId::Sly1) skipRow(true, family);
        section(tr("Keyboard"));
        if (!advanced) for (int i = 0; i < settings::kBindingCount; ++i) bindingRow(i, false, family);
        if (m_game == GameId::Sly1) skipRow(false, input::Family::Keyboard);
        for (int i = 0; i < settings::kCameraKeyCount; ++i) cameraKeyRow(i);
        // (Sly 1: the rhythm minigame's own keys; with Advanced Input Mapping on they are its action group's)
        if (m_game == GameId::Sly1 && !advanced) {
            caption(tr("Rhythm minigame"), color::text2);
            for (int i = 0; i < settings::kSly1RhythmKeyCount; ++i) rhythmKeyRow(i);
        }
        ImGui::Indent(px(16));
        caption(tr("Escape also pauses and goes back, and Enter confirms in menus."));
        ImGui::Unindent(px(16));
        section(tr("Mouse"));
        toggle(tr("Mouse camera"), m_settings.mouseCamera,
               tr("While playing, the mouse turns the camera and aims the binocucom. Pause to free the cursor."));
        rangeRow(tr("Camera sensitivity"), m_settings.mouseSensitivity, 10, 500, 10, tr("How far the camera turns per mouse movement."), percent);
        rangeRow(tr("Aim sensitivity"), m_settings.mouseAimSensitivity, 10, 500, 10, tr("Binocucom aiming speed."), percent);
        if (iniGame()) {
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
        if (iniGame())
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
#if defined(__ANDROID__)
        vulkanDriverRows();
#endif
        collectionRows();
    }
#if defined(__ANDROID__)
    // ---- Settings > Launcher > Vulkan driver (launcher/android_drivers.h): the system's driver or an imported package
    // (Mesa Turnip) for every game's Vulkan renderer; Adreno GPUs, arm64, not on VR headsets.
    std::vector<android::vkdriver::Package> m_drivers;
    double m_nextDrivers = -1;
    std::string m_driverNote, m_gpu;
    bool m_driverImporting = false;
    void vulkanDriverRows() {
        namespace drv = android::vkdriver;
        if (m_gpu.empty())
            if (const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) m_gpu = renderer;
        if (android::openXrRuntimeInstalled()) return;   // (VR headsets: the system's driver, vk_loader.cpp)
        if (m_driverImporting) {
            const auto status = android::driverImport();
            if (!status.pending) {
                m_driverImporting = false;
                if (!status.error.empty()) m_driverNote = trf("The driver package was not imported: %s", status.error.c_str());
                else if (!status.path.empty()) {
                    try {
                        const auto package = drv::importZip(path(status.path), android_get_device_api_level());
                        drv::select({ package.id, package.library, drv::label(package) });
                        drv::clearLastError();
                        m_driverNote = trf("%s imported and chosen. It is used the next time a game starts.", drv::label(package).c_str());
                    } catch (const std::exception& e) {
                        m_driverNote = trf("The driver package was not imported: %s", e.what());
                    }
                    m_nextDrivers = -1;
                }
            }
        }
        if (ImGui::GetTime() >= m_nextDrivers) {
            m_nextDrivers = ImGui::GetTime() + 2.0;
            m_drivers = m_fixture ? std::vector<drv::Package>{} : drv::installed();
        }
        const bool adreno = m_gpu.find("Adreno") != std::string::npos;
        if (!adreno && m_drivers.empty()) return;   // (custom drivers are for Adreno GPUs)
        section(tr("Vulkan driver"));
#if !defined(CANE_ADRENOTOOLS)
        caption(tr("Custom Vulkan drivers need the arm64 version of SafeHouse."), color::warning);
        return;
#endif
        // (a GPU generation custom drivers are not known to drive yet, android_drivers.h kAdrenoFamilies: still offered,
        // since a newer driver build may work, with the reason as a warning; the black-picture watch,
        // custom_driver_watch.h, puts the system driver back if a game shows nothing)
        const std::string untested = drv::blockedReason(m_gpu);
        std::vector<std::string> labels{ tr("System driver") };
        const std::string chosen = drv::selection().id;
        int index = 0;
        for (const auto& p : m_drivers) {
            labels.push_back(drv::label(p));
            if (p.id == chosen) index = int(labels.size()) - 1;
        }
        ImGui::BeginDisabled(m_fixture || m_driverImporting);
        if (stepperRow(tr("Vulkan driver"), &index, labels, m_gpu.empty() ? nullptr : m_gpu.c_str())) {
            if (index == 0) drv::select({});
            else {
                const auto& p = m_drivers[size_t(index - 1)];
                drv::select({ p.id, p.library, drv::label(p) });
            }
            drv::clearLastError();
            m_driverNote.clear();
        }
        describe(tr("The Vulkan driver every game uses. System driver is the one your device came with. A custom driver (for "
                    "example Mesa Turnip for Adreno GPUs) can run better or worse: if a game crashes while starting with it, "
                    "SafeHouse goes back to the system driver by itself. VR always uses the system driver."));
        if (!untested.empty()) {
            ImGui::Indent(px(16));
            caption(tr(untested.c_str()), color::warning);
            ImGui::Unindent(px(16));
        }
        if (linkRow(tr("Import a driver package…"), tr(".zip"), nullptr, Icon::Folder) && !m_fixture) {
            m_driverNote.clear();
            try {
                android::requestDriverImport();
                m_driverImporting = true;
            } catch (const std::exception& e) {
                m_driverNote = e.what();
            }
        }
        describe(tr("A .zip with meta.json and the driver library, as emulators use them (for example a Turnip build). It is "
                    "copied into the app; SafeHouse ships no drivers."));
        if (index > 0 && linkRow(tr("Remove this driver"), nullptr, nullptr, Icon::Trash) && !m_fixture) {
            drv::remove(m_drivers[size_t(index - 1)].id);
            m_nextDrivers = -1;
        }
        ImGui::EndDisabled();
        const std::string error = drv::lastError();
        const std::string& note = !m_driverNote.empty() ? m_driverNote : error;
        if (!note.empty()) {
            ImGui::Indent(px(16));
            caption(note.c_str(), color::warning);
            ImGui::Unindent(px(16));
        }
    }
#endif
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
#if defined(CANE_ANDROID) || defined(CANE_IOS)  // (the apps: no command line; iOS: it opens in place of this window, main.cpp takeTurns)
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
#elif defined(CANE_IOS)
        // (iOS: no Steam, no desktop, no home-screen shortcuts an app can add)
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
            m_library = m_backend.isoLibrary();
            std::error_code ec;
            m_libraryAvailable = m_library.empty() || fs::is_directory(m_library, ec);
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
#if defined(CANE_ANDROID)
        storageAccessRows();
#endif
#if !defined(CANE_IOS)
        discLibraryRows(gameRunning);
#endif
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
#elif defined(CANE_IOS)
                describe(installs.size() > 1 ? tr("Choose this region: Play and Verify use it.") : tr("The installed region."));  // (no shortcuts on iOS)
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
            if (g == GameId::Sly3 && !installs.empty()) {
                // (Sly 3's French / Spanish: The Sly Collection's text, Sly3-Release ps3lang.cpp)
                ImGui::BeginDisabled(gameRunning || m_job.busy);
                if (linkRow(trf("Import %s from The Sly Collection (PS3)…", (std::string(tr("French")) + tr(" and ") + tr("Spanish")).c_str()).c_str(),
                            tr(".iso / .zip"), nullptr, Icon::Disc))
                    m_langGame = GameId::Sly3, startLanguagePick(LangPick::Ps3Image);
                if (linkRow(tr("Import them from the extracted disc…"), tr("folder"), nullptr, Icon::Folder))
                    m_langGame = GameId::Sly3, startLanguagePick(LangPick::Ps3Folder);
                ImGui::EndDisabled();
            }
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
        folderEntryRow(FolderEntry::Install);
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
#if defined(CANE_ANDROID)
    // (Android, install_location.h) folders anywhere on the storage: All files access (Android 11+) or the storage
    // permission (6-10), asked only here. Without it: the private folder and the app's own folders on the storage / SD.
    bool m_allFiles = false;
    double m_nextAllFiles = -1;
    void storageAccessRows() {
        if (ImGui::GetTime() >= m_nextAllFiles) {
            m_nextAllFiles = ImGui::GetTime() + 1.0;
            m_allFiles = !m_fixture && android::allFilesAccess();
        }
        if (m_allFiles) {
            ImGui::Indent(px(16));
            caption(tr("SafeHouse may use folders anywhere on this device's storage and SD cards."), color::text2);
            ImGui::Unindent(px(16));
            return;
        }
        if (linkRow(tr("Allow folders anywhere…"), nullptr, nullptr, Icon::Folder) && !m_fixture) android::requestAllFilesAccess();
        describe(tr("Without this, games install in the app's own storage or its folders on the phone storage and SD card, "
                    "which Android removes when SafeHouse is uninstalled. With it (Android's \"All files access\"), you can choose "
                    "any folder for installations and disc images, and they stay when the app is removed."));
    }
#endif
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
    // "Disc images folder" (backend.h isoLibrary): a folder of the player's own .iso / .chd files that the install
    // page searches before the usual folders. Not on iOS (the app reads only the disc images picked with the system's
    // document picker there). Android (2026-11-24): any folder the app can read (its own folders on the storage / SD
    // card, or anywhere with All files access, storageAccessRows).
    void discLibraryRows(bool gameRunning) {
        section(tr("Disc images"));
        ImGui::BeginDisabled(m_job.busy || gameRunning);
        const std::string sub = m_library.empty() ? std::string(tr("Not set: the install page searches the usual folders.")) : utf8(m_library);
        if (linkRow(tr("Disc images folder"), nullptr, sub.c_str(), Icon::Folder) && !m_fixture) startLibraryPick();
        describe(tr("A folder with your .iso or .chd files. The install page searches it and its subfolders (two levels deep) "
                    "before the usual folders. Nothing installs by itself; you choose the disc."));
        folderEntryRow(FolderEntry::Library);
        if (!m_library.empty()) {
            if (linkRow(tr("Stop searching this folder"), nullptr, nullptr, Icon::Close) && !m_fixture) setIsoLibrary({});
            describe(tr("The install page searches only the usual folders again. No files are touched."));
        }
        ImGui::EndDisabled();
        const std::string note = !m_libraryNote.empty() ? m_libraryNote
                                 : m_libraryAvailable ? std::string{}
                                                      : std::string(tr("The disc images folder is not available (a drive that is not connected?)."));
        if (!note.empty()) {
            ImGui::Indent(px(16));
            caption(note.c_str(), color::warning);
            ImGui::Unindent(px(16));
        }
    }
    void setIsoLibrary(const fs::path& chosen) {
        try {
            m_backend.setIsoLibrary(chosen);
            m_libraryNote.clear();
        } catch (const std::exception& e) {
            m_libraryNote = e.what();
        }
        m_nextLocation = 0;
    }
    void startLibraryPick() {
        if (padUser()) return openFolderEntry(FolderEntry::Library);
        if (m_libraryDialog.running()) return;
        const fs::path start = filedialog::startFolder({ utf8(m_library) });
        if (!m_libraryDialog.start(filedialog::Kind::PickFolder, tr("Choose your disc images folder"), start)) openLibraryPicker();
    }
    void openLibraryPicker() {
        ImGuiFileDialog::Instance()->OpenDialog("isolibrary", tr("Choose your disc images folder"), nullptr,
                                                utf8(filedialog::startFolder({ utf8(m_library) })), "", 1, nullptr, ImGuiFileDialogFlags_Modal);
    }
    // Pads cannot drive the folder pickers (the system's dialog is a separate window, ImGuiFileDialog wants a mouse),
    // so "Choose a folder…" and "Disc images folder" open a path entry under the row instead (the on-screen keyboard).
    enum class FolderEntry { None, Install, Library };
    static bool padUser() {
#if defined(CANE_ANDROID) || defined(CANE_IOS)
        return false;
#else
        return input::tracker().lastWasGamepad();
#endif
    }
    void openFolderEntry(FolderEntry which) {
        m_folderEntry = which;
        m_folderEntryFocus = true;
        const std::string current = which == FolderEntry::Install ? (m_customRoot ? utf8(m_installRoot) : std::string{}) : utf8(m_library);
        std::snprintf(m_folderBuf, sizeof(m_folderBuf), "%s", current.c_str());
    }
    void folderEntryRow(FolderEntry which) {
        if (m_folderEntry != which) return;
        ImGui::Indent(px(16));
        const float applyW = std::max(px(110), measure(fonts().body, tr("Apply")).x + px(40));
        const float cancelW = std::max(px(110), measure(fonts().body, tr("Cancel")).x + px(40));
#ifdef _WIN32
        const char* hint = "D:\\Games";
#else
        const char* hint = "~/Games";
#endif
        const bool entered = inputField("##folder-entry", m_folderBuf, sizeof(m_folderBuf), launcher::Charset::Free, hint,
                                        ImGui::GetContentRegionAvail().x - applyW - cancelW - px(40), ImGuiInputTextFlags_EnterReturnsTrue);
        if (m_folderEntryFocus) {
            m_folderEntryFocus = false;
            setFocus(ImGui::GetItemID(), ImGui::GetCurrentWindow(), true);
        }
        ImGui::SameLine(0, px(8));
        ImGui::BeginDisabled(!m_folderBuf[0]);
        const bool apply = button(tr("Apply"), Tone::Primary, { applyW, 0 }) || (entered && m_folderBuf[0]);
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(8));
        if (button(tr("Cancel"), Tone::Quiet, { cancelW, 0 })) m_folderEntry = FolderEntry::None;
        ImGui::Unindent(px(16));
        if (!apply) return;
        // (quotes from a pasted path and a leading ~ are taken care of)
        std::string text = m_folderBuf;
        const auto first = text.find_first_not_of(" \t\"'");
        const auto last = text.find_last_not_of(" \t\"'");
        text = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
        if (text.empty()) return;
        fs::path chosen = path(text);
#ifndef _WIN32
        if (text == "~" || text.rfind("~/", 0) == 0)
            if (const char* home = std::getenv("HOME"); home && *home) chosen = path(home) / path(text.size() > 2 ? text.substr(2) : std::string{});
#endif
        if (which == FolderEntry::Install) {
            setInstallRoot(chosen);
            if (m_locationNote.empty()) m_folderEntry = FolderEntry::None;
        } else {
            setIsoLibrary(chosen);
            if (m_libraryNote.empty()) m_folderEntry = FolderEntry::None;
        }
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
        if (padUser()) return openFolderEntry(FolderEntry::Install);
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
    // How a language plays on the selected region's disc (its text needs that disc's fonts: the Japanese and the Korean
    // discs draw Latin text without accents, a Japanese / Korean language elsewhere needs its Font.bin)
    std::string languageNote(const LauncherBackend::LanguageInfo& l) const {
        const std::string discScript = m_langRegion == "japan" ? "japanese" : m_langRegion == "korea" ? "korean" : "latin";
        if (l.wide && !l.font && l.script != discScript)
            return discScript == "latin" ? tr("its voices, English text on this disc") : tr("voices only: the text stays in the disc's language");
        if (!l.wide && discScript != "latin" && l.key != "English" && l.key.rfind("English", 0) != 0) return tr("text without accents on this disc");
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
    // (Sly 3) English and the languages folder's (the PS3 import, Settings > Games > Sly 3), Settings.ini's Language
    void sly3LanguageRow() {
        const std::vector<std::string> langs = m_backend.sly3Languages();
        std::vector<std::string> names{ tr("English") };
        int index = 0;
        for (const auto& k : langs) {
            names.push_back(tr(k));
            if (k == m_sly2.game.language) index = int(names.size()) - 1;
        }
        if (stepperRow(tr("Language"), &index, names)) {
            m_sly2.game.language = index == 0 ? std::string() : langs[size_t(index) - 1];
            commit();
        }
    }
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
                 "Japanese, Korea has Korean. About 1 minute; Europe needs about 4.5 GB."));
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
        const char* title = pick == LangPick::Disc ? tr("Choose a Sly 2 disc image (Europe, Japan or Korea)")
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
            m_job.start("Languages (The Sly Collection)", [this, source, game = m_langGame](const auto& log) {
                m_backend.importPs3Languages(source, m_job.cancel, log, game);
            });
        m_langGame = GameId::Sly2;
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
        if (const std::string error = logError ? logError() : std::string(); !error.empty())
            caption(trf("Could not write the log: %s", error.c_str()).c_str(), color::warning);
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
        // (the folder's path in the help: findable without the button)
        m_logFolderText = std::string(tr("Opens the folder with the launcher and game logs.")) + " " +
                          tr("Crash reports and the output of a task that failed are always saved there.") + "\n" +
                          shortcut::displayPath(m_backend.locations.logs);
        describe(m_logFolderText.c_str());
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
    sly1input::Profile m_actions1 = sly1input::defaults();
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
    std::string m_help, m_captureNote, m_saveFolderHelp, m_logFolderText;
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
    filedialog::Async m_libraryDialog;
    fs::path m_library;
    bool m_libraryAvailable = true;
    std::string m_libraryNote;
    FolderEntry m_folderEntry = FolderEntry::None;
    bool m_folderEntryFocus = false;
    char m_folderBuf[1024] = {};
    GameId m_ps3Game = GameId::Sly2;
    GameId m_langGame = GameId::Sly2;   // (the PS3 language import's game: Sly 2's rows, or Sly 3's)
    bool m_ps3PickFolder = false;
};
} // namespace cane::ui
